/*
  raudp: sends RetroAchievements telemetry from inside the running game.

  ee_core takes a snapshot of the watched memory addresses every frame
  and DMAs it into a buffer in this module's memory (ra_snap.h). This
  module wraps each snapshot in one or more UDP packets and hands the
  Ethernet frames straight to the SMAP driver.

  Why bypass the TCP/IP stack. lwip_sendto() posts a message to the
  tcpip thread and waits for it. That mailbox is shared with the game's
  own traffic (SMB reads when the game runs from a share), it is small,
  and 60 sends per second pushed the game's frames out of it: the game
  stalled and a send took 700 us on average with peaks of 18 ms. Building
  the frame here and calling SMAPSendPacket() directly takes about
  100 us, loses nothing, and touches no stack pools. The only shared
  resource is the transmit FIFO, protected by a mutex inside the driver.

  Polite sending. SMAPSendPacket() busy-waits while the controller is
  still transmitting the previous frame, holding the mutex the whole
  time. Waiting there starves the stack thread and the game (audio,
  loading). So before sending we look at the transmitter's busy bit and
  skip our frame when it is set. The game has priority; skipped frames
  are counted and reported in the packet header.

  Finding the PC. Bypassing the stack also means no ARP, so the
  destination MAC must come from somewhere. Once, at start-up, this
  module uses the stack the normal way: it opens a UDP socket,
  broadcasts "RAP1 <own-ip> <port>" and waits for the PC client to
  answer "RAO1". The PC's IP is the reply's source address. Its MAC is
  already in the stack's ARP table by then, because etharp_ip_input()
  records the sender of every incoming IP packet from the local subnet;
  etharp_lookup_mac() (an export added to SMSTCPIP) reads it out. Then
  the socket is closed and every telemetry frame is built by hand.
  Nothing is stored between runs: the IOP is reset on every game launch,
  so a stale address cannot survive. Until the PC answers, telemetry
  stays silent and the query repeats, every second at first and then
  every 30 seconds, so the PC client may start after the game.

  Licenced under Academic Free License version 3.0, like ee_core.
*/

#include <tamtypes.h>
#include <loadcore.h>
#include <thbase.h>
#include <smapregs.h>
#include "smap_tx.h"
#include "smstcpip.h" /* lwip_*: used only during discovery */
#include "ra_snap.h"

/* Added to SMSTCPIP (etharp.c), export 42. Not part of smstcpip.h. */
int etharp_lookup_mac(unsigned int ipaddr, unsigned char *mac_out);

#define MODNAME "raudp"
IRX_ID(MODNAME, 15, 0);

extern struct irx_export_table _exp_raudp;

/* ---- Addresses -------------------------------------------------------
   Own IP arrives as a load argument (the same ipconfig string ee_core
   gives SMAP). The PC's IP and MAC come from discovery. Own MAC is read
   from the controller registers. All IPs are in network byte order. */
static u32 ra_src_ip = 0;
static u32 ra_dst_ip = 0;
static u8 ra_dst_mac[6];

#define RA_DST_PORT 18194 /* PC client listens here */
#define RA_SRC_PORT 18195 /* our port: discovery socket and telemetry source */

/* Hold-off after start so the game can finish loading. */
#define RA_QUIET_US (30 * 1000 * 1000)

/* One snapshot per game frame at 60 Hz. */
#define RA_PERIOD_US 16666

/* ---- Frame layout ----------------------------------------------------
   One static Ethernet + IPv4 + UDP frame. Headers are filled once; each
   send updates only the payload. Payload is the largest that fits
   without IP fragmentation. */
#define RA_PAYLOAD   1472
#define RA_ETH_HLEN  14
#define RA_IP_HLEN   20
#define RA_UDP_HLEN  8
#define RA_HDR_LEN   (RA_ETH_HLEN + RA_IP_HLEN + RA_UDP_HLEN)
#define RA_FRAME_LEN (RA_HDR_LEN + RA_PAYLOAD)

static u8 ra_frame[RA_FRAME_LEN] __attribute__((aligned(16)));
static u8 *ra_payload = &ra_frame[RA_HDR_LEN];

/* Counters reported in every packet. */
static u32 ra_seq = 0;      /* packets sent or skipped */
static u32 ra_fail = 0;     /* SMAPSendPacket() errors */
static int ra_err = 0;      /* last error code */
static u32 ra_us = 0;       /* time spent sending the last snapshot */
static u32 ra_us_max = 0;   /* worst case */
static u32 ra_rxq = 0;      /* controller receive FIFO depth at send time */
static u32 ra_skip = 0;     /* frames skipped because the transmitter was busy */
static u32 ra_snap_bad = 0; /* torn snapshots detected */

/* Snapshot buffer, owned by this module, written by the EE over SIF DMA.
   The address arrives as a load argument. */
static volatile struct ra_snap *ra_snap = NULL;

/* ---- Packet header ---------------------------------------------------
   Text header, then the raw snapshot bytes. Fields are fixed width so
   the PC client can parse them without a tokenizer, and the layout is
   computed here instead of by hand: hand-counted offsets kept going
   wrong during development.

     RA15 seq=000000 sz=0000 us=00000 mx=00000 rxq=000 fail=000000
          err=+000 sk=000000 lk=0000 sq=000000 ds=000000 bad=0000
          n=0000 vb=0000 pt=0 np=0 id=SLUS_210.65~~~~ <values>

   seq  packet counter            sq   snapshot number from the EE
   sz   payload size              ds   frames the EE skipped (DMA busy)
   us   last send time, us        bad  torn snapshots seen here
   mx   worst send time, us       n    entries in the watch list
   rxq  receive FIFO depth        vb   value bytes in this packet
   fail send errors               pt   part index, from 0
   err  last error code           np   number of parts
   sk   frames skipped, tx busy   id   game serial, padded with '~'
   lk   link mode: speed (100/010/999) and duplex (F/H)

   "id" must stay last: the PC client finds the binary tail as the
   offset of " id=" plus 4 plus 15 plus one space. */
struct ra_field
{
    const char *label;
    int width;
    int off; /* filled by ra_head_build() */
};

static struct ra_field ra_fields[] = {
    {"seq", 6, 0},
    {"sz", 4, 0},
    {"us", 5, 0},
    {"mx", 5, 0},
    {"rxq", 3, 0},
    {"fail", 6, 0},
    {"err", 4, 0},
    {"sk", 6, 0},
    {"lk", 4, 0},
    {"sq", 6, 0},
    {"ds", 6, 0},
    {"bad", 4, 0},
    {"n", 4, 0},
    {"vb", 4, 0},
    {"pt", 1, 0},
    {"np", 1, 0},
    {"id", 15, 0},
};

#define RA_FIELD_COUNT (sizeof(ra_fields) / sizeof(ra_fields[0]))

enum ra_field_id {
    RA_F_SEQ = 0,
    RA_F_SZ,
    RA_F_US,
    RA_F_MX,
    RA_F_RXQ,
    RA_F_FAIL,
    RA_F_ERR,
    RA_F_SK,
    RA_F_LK,
    RA_F_SQ,
    RA_F_DS,
    RA_F_BAD,
    RA_F_N,
    RA_F_VB,
    RA_F_PT,
    RA_F_NP,
    RA_F_ID,
};

#define RA_OFF(id) (ra_fields[id].off)

static int ra_head_len = 0;

/* Value bytes per packet. Derived from the real header length so a
   header change can never silently truncate the tail. */
static u32 ra_chunk = RA_SNAP_CHUNK_BYTES;

static void ra_head_build(void)
{
    static const char tag[] = "RA15";
    int pos = 0;
    unsigned int i;
    int j;

    for (j = 0; tag[j] != '\0'; j++)
        ra_payload[pos++] = (u8)tag[j];

    for (i = 0; i < RA_FIELD_COUNT; i++) {
        const char *l = ra_fields[i].label;

        ra_payload[pos++] = ' ';
        for (j = 0; l[j] != '\0'; j++)
            ra_payload[pos++] = (u8)l[j];
        ra_payload[pos++] = '=';

        ra_fields[i].off = pos;
        for (j = 0; j < ra_fields[i].width; j++)
            ra_payload[pos++] = '0';
    }

    ra_payload[pos++] = ' ';
    ra_head_len = pos;

    ra_chunk = (u32)(RA_PAYLOAD - ra_head_len);
    if (ra_chunk > RA_SNAP_CHUNK_BYTES)
        ra_chunk = RA_SNAP_CHUNK_BYTES;
}

/* ---- Formatting helpers (no libc in this module) ---------------------- */

static void ra_fmt(u8 *dst, u32 v, int width)
{
    int i;

    for (i = width - 1; i >= 0; i--) {
        dst[i] = (u8)('0' + (v % 10));
        v /= 10;
    }
}

static void ra_fmt_err(u8 *dst, int v)
{
    u32 a;

    if (v < 0) {
        dst[0] = '-';
        a = (u32)(-v);
    } else {
        dst[0] = '+';
        a = (u32)v;
    }

    ra_fmt(dst + 1, a, 3);
}

/* Parses `len` hex digits at s[off]. Returns 0 on a bad digit. */
static u32 ra_hex_at(const char *s, int off, int len)
{
    u32 v = 0;
    int i;

    for (i = 0; i < len; i++) {
        char c = s[off + i];

        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (u32)(c - '0');
        else if (c >= 'A' && c <= 'F')
            v |= (u32)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f')
            v |= (u32)(c - 'a' + 10);
        else
            return 0;
    }

    return v;
}

/* Writes "a.b.c.d" for an address in network byte order. Returns length. */
static int ra_fmt_ip(char *dst, u32 ip)
{
    int pos = 0, k;

    for (k = 0; k < 4; k++) {
        u32 o = (ip >> (8 * k)) & 0xFF;

        if (k > 0)
            dst[pos++] = '.';
        if (o >= 100)
            dst[pos++] = (char)('0' + o / 100);
        if (o >= 10)
            dst[pos++] = (char)('0' + (o / 10) % 10);
        dst[pos++] = (char)('0' + o % 10);
    }

    dst[pos] = '\0';

    return pos;
}

/* IPv4 header checksum: one's complement sum of 16-bit words. */
static u16 ra_ip_checksum(const u8 *hdr, int len)
{
    u32 sum = 0;
    int i;

    for (i = 0; i < len; i += 2)
        sum += ((u32)hdr[i] << 8) | hdr[i + 1];

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (u16)(~sum);
}

static u32 ra_usec_delta(iop_sys_clock_t *from, iop_sys_clock_t *to)
{
    iop_sys_clock_t d;
    u32 sec = 0, usec = 0;

    d.hi = 0;
    d.lo = to->lo - from->lo;
    SysClock2USec(&d, &sec, &usec);

    return sec * 1000000 + usec;
}

/* ---- Frame headers, filled once ---------------------------------------- */

static void ra_frame_init(void)
{
    USE_SMAP_EMAC3_REGS;
    u32 mac_hi, mac_lo;
    u16 total_len = RA_IP_HLEN + RA_UDP_HLEN + RA_PAYLOAD;
    u16 udp_len = RA_UDP_HLEN + RA_PAYLOAD;
    u16 csum;
    u8 *e = ra_frame;
    u8 *ip = &ra_frame[RA_ETH_HLEN];
    u8 *udp = &ra_frame[RA_ETH_HLEN + RA_IP_HLEN];
    int i;

    ra_head_build();

    /* Ethernet. Own MAC from the controller, as SMAPGetMACAddress() does. */
    for (i = 0; i < 6; i++)
        e[i] = ra_dst_mac[i];

    mac_hi = SMAP_EMAC3_GET32(SMAP_R_EMAC3_ADDR_HI);
    mac_lo = SMAP_EMAC3_GET32(SMAP_R_EMAC3_ADDR_LO);
    e[6] = (u8)(mac_hi >> 8);
    e[7] = (u8)mac_hi;
    e[8] = (u8)(mac_lo >> 24);
    e[9] = (u8)(mac_lo >> 16);
    e[10] = (u8)(mac_lo >> 8);
    e[11] = (u8)mac_lo;

    e[12] = 0x08; /* EtherType IPv4 */
    e[13] = 0x00;

    /* IPv4. No fragmentation, so identification stays zero. */
    ip[0] = 0x45; /* version 4, 5 words */
    ip[1] = 0x00; /* TOS */
    ip[2] = (u8)(total_len >> 8);
    ip[3] = (u8)total_len;
    ip[4] = 0x00; /* identification */
    ip[5] = 0x00;
    ip[6] = 0x00; /* flags, fragment offset */
    ip[7] = 0x00;
    ip[8] = 64;    /* TTL */
    ip[9] = 17;    /* UDP */
    ip[10] = 0x00; /* checksum, below */
    ip[11] = 0x00;
    /* Network byte order already: the low byte in memory is the first octet. */
    ip[12] = (u8)(ra_src_ip);
    ip[13] = (u8)(ra_src_ip >> 8);
    ip[14] = (u8)(ra_src_ip >> 16);
    ip[15] = (u8)(ra_src_ip >> 24);
    ip[16] = (u8)(ra_dst_ip);
    ip[17] = (u8)(ra_dst_ip >> 8);
    ip[18] = (u8)(ra_dst_ip >> 16);
    ip[19] = (u8)(ra_dst_ip >> 24);

    csum = ra_ip_checksum(ip, RA_IP_HLEN);
    ip[10] = (u8)(csum >> 8);
    ip[11] = (u8)csum;

    /* UDP. The checksum is optional in IPv4 and left at zero. */
    udp[0] = (u8)(RA_SRC_PORT >> 8);
    udp[1] = (u8)RA_SRC_PORT;
    udp[2] = (u8)(RA_DST_PORT >> 8);
    udp[3] = (u8)RA_DST_PORT;
    udp[4] = (u8)(udp_len >> 8);
    udp[5] = (u8)udp_len;
    udp[6] = 0x00;
    udp[7] = 0x00;

    /* Payload padding. */
    for (i = ra_head_len; i < RA_PAYLOAD - 1; i++)
        ra_payload[i] = '.';
    ra_payload[RA_PAYLOAD - 1] = '\n';

    ra_fmt(&ra_payload[RA_OFF(RA_F_SZ)], RA_PAYLOAD, 4);

    /* Link mode as negotiated by the driver. 10 Mbit or half duplex
       would explain collisions with incoming traffic. */
    {
        u32 mode1 = SMAP_EMAC3_GET32(SMAP_R_EMAC3_MODE1);
        u32 media = mode1 & SMAP_E3_MEDIA_MSK;
        u8 *lk = &ra_payload[RA_OFF(RA_F_LK)];

        if (media == SMAP_E3_MEDIA_100M) {
            lk[0] = '1';
            lk[1] = '0';
            lk[2] = '0';
        } else if (media == SMAP_E3_MEDIA_10M) {
            lk[0] = '0';
            lk[1] = '1';
            lk[2] = '0';
        } else {
            lk[0] = '9';
            lk[1] = '9';
            lk[2] = '9';
        }

        lk[3] = (mode1 & SMAP_E3_FDX_ENABLE) ? 'F' : 'H';
    }
}

/* Whether the transmitter is busy. GNP_0 in TxMODE0 stays set until the
   controller has finished the previous frame; SMAPSendPacket() would
   busy-wait on it. */
static int ra_tx_busy(void)
{
    USE_SMAP_EMAC3_REGS;

    return (SMAP_EMAC3_GET(SMAP_R_EMAC3_TxMODE0) & SMAP_E3_TX_GNP_0) != 0;
}

/* ---- Sending one snapshot ---------------------------------------------- */

/* The snapshot is copied out whole before it is split into packets.
   Otherwise the EE could land the next frame between two sends and the
   parts would describe different moments; a condition such as
   "A == 1 and B == 2 in the same frame" would fire on a mix. */
static u8 ra_stage[RA_SNAP_MAX_BYTES];

static void ra_send_one(void)
{
    USE_SMAP_REGS;
    iop_sys_clock_t t0, t1;
    u32 nb = 0, parts = 1, part;
    int ret;

    ra_rxq = SMAP_REG8(SMAP_R_RXFIFO_FRAME_CNT);

    /* Checked once per snapshot, not per part: dropping the last part
       of three would waste the two already sent. Busy means the whole
       snapshot is skipped. Pushing into a busy controller produced
       duplicate frames on the wire. */
    if (ra_tx_busy()) {
        ra_skip++;
        ra_seq++; /* the number is consumed so the PC sees the gap */
        return;
    }

    ra_fmt(&ra_payload[RA_OFF(RA_F_US)], ra_us, 5);
    ra_fmt(&ra_payload[RA_OFF(RA_F_MX)], ra_us_max, 5);
    ra_fmt(&ra_payload[RA_OFF(RA_F_RXQ)], ra_rxq, 3);
    ra_fmt(&ra_payload[RA_OFF(RA_F_FAIL)], ra_fail, 6);
    ra_fmt_err(&ra_payload[RA_OFF(RA_F_ERR)], ra_err);
    ra_fmt(&ra_payload[RA_OFF(RA_F_SK)], ra_skip, 6);

    /* The DMA from the EE is not atomic and lands front to back. seq is
       taken from the header before the copy; the trailer word after the
       values is the last to arrive. A trailer or header that no longer
       matches after the copy means a newer snapshot overwrote part of
       what was copied: the snapshot is torn and dropped. */
    if (ra_snap != NULL) {
        u32 sq = ra_snap->seq;

        if (ra_snap->magic == RA_SNAP_MAGIC) {
            const u8 *src = (const u8 *)ra_snap + RA_SNAP_HDR;
            u32 i, tail;

            nb = ra_snap->bytes;
            if (nb > RA_SNAP_MAX_BYTES)
                nb = RA_SNAP_MAX_BYTES;

            for (i = 0; i < nb; i++)
                ra_stage[i] = src[i];

            tail = *(volatile u32 *)((const u8 *)ra_snap + RA_SNAP_TRAILER_OFF(nb));

            if (tail != sq || ra_snap->seq != sq) {
                ra_snap_bad++;
                nb = 0;
            } else {
                ra_fmt(&ra_payload[RA_OFF(RA_F_SQ)], sq, 6);
                ra_fmt(&ra_payload[RA_OFF(RA_F_DS)], ra_snap->dma_skip, 6);
                ra_fmt(&ra_payload[RA_OFF(RA_F_N)], ra_snap->count, 4);

                /* Serial as-is, padded with '~'. Not '_': serials
                   contain underscores (SLUS_210.65). */
                for (i = 0; i < 15; i++) {
                    char c = ra_snap->game_id[i];

                    ra_payload[RA_OFF(RA_F_ID) + i] =
                        (c >= 0x20 && c < 0x7F) ? (u8)c : (u8)'~';
                }
            }
        } else {
            ra_snap_bad++;
        }
    }

    ra_fmt(&ra_payload[RA_OFF(RA_F_BAD)], ra_snap_bad, 4);

    if (nb > 0) {
        parts = (nb + ra_chunk - 1) / ra_chunk;
        if (parts > RA_SNAP_PARTS)
            parts = RA_SNAP_PARTS;
    }

    GetSystemTime(&t0);

    for (part = 0; part < parts; part++) {
        u32 off = part * ra_chunk;
        u32 len = nb > off ? nb - off : 0;
        u32 i;

        if (len > ra_chunk)
            len = ra_chunk;

        ra_fmt(&ra_payload[RA_OFF(RA_F_SEQ)], ra_seq, 6);
        ra_fmt(&ra_payload[RA_OFF(RA_F_VB)], len, 4);
        ra_fmt(&ra_payload[RA_OFF(RA_F_PT)], part, 1);
        ra_fmt(&ra_payload[RA_OFF(RA_F_NP)], parts, 1);

        for (i = 0; i < len; i++)
            ra_payload[ra_head_len + i] = ra_stage[off + i];

        ret = SMAPSendPacket(ra_frame, RA_FRAME_LEN);

        if (ret < 0) {
            ra_fail++;
            ra_err = ret;
        }

        ra_seq++;
    }

    GetSystemTime(&t1);

    ra_us = ra_usec_delta(&t0, &t1);
    if (ra_us > ra_us_max)
        ra_us_max = ra_us;
}

/* ---- Discovery --------------------------------------------------------- */

#define RA_DISC_POLL_US 50000              /* poll interval for the reply */
#define RA_DISC_POLLS   20                 /* 20 * 50 ms = 1 s per attempt */
#define RA_DISC_FAST    10                 /* attempts at 1 s intervals */
#define RA_DISC_SLOW_US (30 * 1000 * 1000) /* then one attempt every 30 s */

/* Time spent in discovery. Subtracted from the start-up hold-off. */
static u32 ra_disc_us = 0;

/* Broadcasts "RAP1 <own-ip> <port>" and waits for "RAO1".

   Own address is in the text because the PC client may run inside a
   container behind NAT, which rewrites the source address; the client
   replies to the address in the packet. The port is the telemetry
   source port, so the reply lands on this socket.

   Returns 1 once both the PC's IP and MAC are known, 0 only if no
   socket could be opened. There is no timeout: while the PC is silent
   the query repeats, first every second, then every 30 seconds. */
static int ra_discover(void)
{
    struct sockaddr_in me, to, from;
    socklen_t fromlen;
    char req[48], rx[64];
    int s, on = 1, len, tries = 0, w, got;

    s = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0)
        return 0;

    lwip_setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    me.sin_family = AF_INET;
    me.sin_port = htons(RA_SRC_PORT);
    me.sin_addr.s_addr = INADDR_ANY;
    if (lwip_bind(s, (struct sockaddr *)&me, sizeof(me)) < 0) {
        lwip_close(s);
        return 0;
    }

    to.sin_family = AF_INET;
    to.sin_port = htons(RA_DST_PORT);
    to.sin_addr.s_addr = INADDR_BROADCAST;

    req[0] = 'R';
    req[1] = 'A';
    req[2] = 'P';
    req[3] = '1';
    req[4] = ' ';
    len = 5 + ra_fmt_ip(&req[5], ra_src_ip);
    req[len++] = ' ';
    ra_fmt((u8 *)&req[len], RA_SRC_PORT, 5);
    len += 5;
    req[len] = '\0';

    for (;;) {
        lwip_sendto(s, req, len, 0, (struct sockaddr *)&to, sizeof(to));

        for (w = 0; w < RA_DISC_POLLS; w++) {
            DelayThread(RA_DISC_POLL_US);
            ra_disc_us += RA_DISC_POLL_US;

            /* Eight arguments: SMSTCPIP's recvfrom splits an SMB header
               from the payload. We want the whole datagram in rx. */
            fromlen = sizeof(from);
            got = lwip_recvfrom(s, NULL, 0, rx, sizeof(rx), MSG_DONTWAIT,
                                (struct sockaddr *)&from, &fromlen);

            if (got >= 4 && rx[0] == 'R' && rx[1] == 'A' && rx[2] == 'O' && rx[3] == '1') {
                u32 ip = from.sin_addr.s_addr;

                /* The reply came through the stack, so the stack has
                   already recorded the sender's IP/MAC pair. An IP
                   without a MAC is useless: a frame with no destination
                   would vanish silently. Keep asking in that case. */
                if (ip != 0 && ip != INADDR_BROADCAST && etharp_lookup_mac(ip, ra_dst_mac)) {
                    ra_dst_ip = ip;
                    lwip_close(s);
                    return 1;
                }
            }
        }

        tries++;
        if (tries >= RA_DISC_FAST) {
            DelayThread(RA_DISC_SLOW_US);
            if (ra_disc_us < 0xF0000000)
                ra_disc_us += RA_DISC_SLOW_US;
        }
    }
}

/* ---- Thread and module entry ------------------------------------------- */

static void ra_thread(void *arg)
{
    (void)arg;

    if (!ra_discover())
        return; /* no socket: the stack is not up, stay silent */

    ra_frame_init();

    if (ra_disc_us < RA_QUIET_US)
        DelayThread(RA_QUIET_US - ra_disc_us);

    for (;;) {
        ra_send_one();
        DelayThread(RA_PERIOD_US);
    }
}

int _shutdown(void)
{
    return 0;
}

/* Arguments, built by ee_core (iopmgr.c):
     argv[1]  snapshot buffer address in IOP RAM, eight hex digits
     argv[2]  own IP in dotted form (followed by netmask and gateway,
              which this module does not need)
   argv[0] is the module name, inserted by the IOP loader. */
int _start(int argc, char *argv[])
{
    iop_thread_t thread;
    int tid;

    if (argc >= 2 && argv[1] != NULL) {
        int len;

        for (len = 0; len < 9 && argv[1][len] != '\0'; len++)
            ;

        if (len >= 8)
            ra_snap = (volatile struct ra_snap *)ra_hex_at(argv[1], 0, 8);
    }

    if (argc >= 3 && argv[2] != NULL)
        ra_src_ip = inet_addr(argv[2]);

    /* Without an own address there is nothing to put in the discovery
       request or the IP header. Staying silent beats sending garbage. */
    if (ra_src_ip == 0 || ra_src_ip == INADDR_BROADCAST)
        return MODULE_RESIDENT_END;

    RegisterLibraryEntries(&_exp_raudp);

    thread.attr = TH_C;
    thread.option = 0;
    thread.thread = ra_thread;
    thread.stacksize = 0x1000;
    thread.priority = 0x68;

    tid = CreateThread(&thread);
    if (tid < 0)
        return MODULE_NO_RESIDENT_END;

    StartThread(tid, NULL);

    return MODULE_RESIDENT_END;
}
