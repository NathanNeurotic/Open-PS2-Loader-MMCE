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
  defer our snapshot when it is set and retry on the next poll. The game
  has priority; deferrals are counted and reported in the packet header.

  Finding the PC. Bypassing the stack also means no ARP, so the
  destination MAC must come from somewhere. Once, at start-up, this
  module uses the stack the normal way: it opens a UDP socket,
  broadcasts "RAP1 <own-ip> <port>" and waits for the PC client to
  answer "RAO1". The PC's IP is the reply's source address. Its MAC is
  already in the stack's ARP table by then, because etharp_ip_input()
  records the sender of every incoming IP packet from the local subnet;
  etharp_lookup_mac() (an export added to SMSTCPIP) reads it out. From
  then on every telemetry frame is built by hand.
  Nothing is stored between runs: the IOP is reset on every game launch,
  so a stale address cannot survive. Until the PC answers, telemetry
  stays silent and the query repeats, every second at first and then
  every 30 seconds, so the PC client may start after the game.

  The socket stays open afterwards for the one thing that travels the
  other way: "RAU1 <id> <points>" from the PC when an achievement
  unlocks. It is polled without waiting once per telemetry period, and
  each unlock is handed to ee_core as a small record DMA'd into a buffer
  there (struct ra_event), where the VBLANK handler picks it up and shows
  the notice over the game.

  Licenced under Academic Free License version 3.0, like ee_core.
*/

#include <tamtypes.h>
#include <loadcore.h>
#include <thbase.h>
#include <sifman.h>
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

/* The EE lands a snapshot every frame, 16.7 ms. Polling four times a
   frame and sending only a new seq catches each one; sleeping a whole
   frame plus the work per pass missed one in twelve (lab/skips, 08.09). */
#define RA_POLL_US         4000
#define RA_IDLE_TICKS      4    /* no snapshot: one header-only packet per frame */
#define RA_KEEPALIVE_TICKS 250  /* a second without a new snapshot: repeat the last */
#define RA_HEARTBEAT_TICKS 2500 /* every ten seconds */

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

/* May this module look for the PC while a game runs? Off when the game
   streams its own disc over the same NIC -- a share (ETH_MODE) or an HTTP
   host. Both roads to the PC take something the game needs: the raw one
   frees every SMAP receive descriptor it walks, and the disc stream arrives
   through that ring; the lwIP one posts to the stack mailbox the SMB or HTTP
   client is waiting on. Either way the game reaches its first menu and loads
   for ever. ee_core decides and says so in a load argument. Sending is
   unaffected and stays on. */
static int ra_rx_in_game = 1;

/* The game's serial, from the same argument, so the very first packets carry
   it instead of fifteen zeroes -- the snapshot has it too, but not before the
   EE has written one. */
static char ra_game_id[16] = {0};

/* Counters reported in every packet. */
static u32 ra_seq = 0;      /* packets sent or skipped */
static u32 ra_fail = 0;     /* SMAPSendPacket() errors */
static int ra_err = 0;      /* last error code */
static u32 ra_us = 0;       /* time spent sending the last snapshot */
static u32 ra_us_max = 0;   /* worst case */
static u32 ra_rxq = 0;      /* controller receive FIFO depth at send time */
static u32 ra_skip = 0;     /* sends deferred because the transmitter was busy */
static u32 ra_snap_bad = 0; /* torn copies detected (retried on the next poll) */
static u32 ra_sent_sq = 0;  /* seq of the last snapshot sent */
static int ra_sent_any = 0;

/* Snapshot buffer, owned by this module, written by the EE over SIF DMA.
   The address arrives as a load argument. */
static volatile struct ra_snap *ra_snap = NULL;

/* Event buffer in ee_core, written from here over SIF DMA. The address
   arrives in the same load argument, after the snapshot buffer's. 0 when
   ee_core did not give one; events are then dropped. */
static u32 ra_ee_event = 0;

/* The record being sent, in IOP RAM: SIF DMA copies from here. Aligned
   for the DMA, and kept whole between sends. */
static struct ra_event ra_event __attribute__((aligned(16)));

/* Badge buffer in ee_core, third address in the load argument. 0 when
   ee_core did not give one; badge chunks are then dropped. */
static u32 ra_ee_badge = 0;

/* The badge assembling in IOP RAM, one per game session: the PC repeats the
   series against loss, later chunks add nothing. */
static struct ra_badge ra_badge __attribute__((aligned(16)));
static u32 ra_badge_mask = 0; /* bit per chunk received */
static int ra_badge_done = 0;

/* Counters for the heartbeat: what this side saw, said to the
   PC every ten seconds, because a silent link cannot be told apart
   from a dead one by staring at the screen. */
static u32 ra_hb_rx = 0;  /* datagrams that reached ra_poll_pc */
static u32 ra_hb_rau = 0; /* unlock notices among them */
static u32 ra_hb_rab = 0; /* badge chunks accepted */

/* Discovery socket, kept open for the PC's messages. */
static int ra_sock = -1;

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
   us   last send time, us        bad  torn copies here, retried
   mx   worst send time, us       n    entries in the watch list
   rxq  receive FIFO depth        vb   value bytes in this packet
   fail send errors               pt   part index, from 0
   err  last error code           np   number of parts
   sk   sends deferred, tx busy   id   game serial, padded with '~'
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

/* Serial as-is, padded with '~'. Not '_': serials contain underscores
   (SLUS_210.65). Only valid once ra_head_build has set the field offsets. */
static void ra_id_put(const char *src)
{
    int i;

    for (i = 0; i < 15; i++) {
        char c = src[i];

        ra_payload[RA_OFF(RA_F_ID) + i] =
            (c >= 0x20 && c < 0x7F) ? (u8)c : (u8)'~';
    }
}

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

    ra_id_put(ra_game_id);
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
/* Eight hex digits. */
static void ra_fmt_hex8(char *dst, u32 v)
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 7; i >= 0; i--) {
        dst[i] = hex[v & 0xF];
        v >>= 4;
    }
}

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

/* ---- Control messages to the PC: down the raw telemetry path, not lwIP,
   which puts nothing on the wire from a running game. Headers copied from
   the telemetry frame, lengths and checksum redone. */
#define RA_CTL_MAX 64

static u8 ra_ctl_frame[RA_HDR_LEN + RA_CTL_MAX] __attribute__((aligned(16)));

static void ra_ctl_send(const char *msg, int len)
{
    u8 *ip = &ra_ctl_frame[RA_ETH_HLEN];
    u8 *udp = &ra_ctl_frame[RA_ETH_HLEN + RA_IP_HLEN];
    u16 total_len, udp_len, csum;
    int i;

    if (len <= 0 || len > RA_CTL_MAX)
        return;

    /* Wait for the transmitter instead of testing it: this runs right after
       the telemetry send, when GNP is still up. Bounded spin, a frame takes
       ~120 us. */
    for (i = 0; i < 20000 && ra_tx_busy(); i++)
        ;
    if (ra_tx_busy())
        return; /* wedged; everything on this path repeats */

    for (i = 0; i < RA_HDR_LEN; i++)
        ra_ctl_frame[i] = ra_frame[i];
    for (i = 0; i < len; i++)
        ra_ctl_frame[RA_HDR_LEN + i] = (u8)msg[i];

    /* The wire minimum is 60 bytes before the FCS; pad with spaces
       rather than trust the controller to. */
    while (RA_HDR_LEN + len < 60)
        ra_ctl_frame[RA_HDR_LEN + len++] = ' ';

    total_len = RA_IP_HLEN + RA_UDP_HLEN + len;
    ip[2] = (u8)(total_len >> 8);
    ip[3] = (u8)total_len;
    ip[10] = 0;
    ip[11] = 0;
    csum = ra_ip_checksum(ip, RA_IP_HLEN);
    ip[10] = (u8)(csum >> 8);
    ip[11] = (u8)csum;

    udp_len = RA_UDP_HLEN + len;
    udp[4] = (u8)(udp_len >> 8);
    udp[5] = (u8)udp_len;
    udp[6] = 0; /* checksum optional in IPv4, and it covers the old payload */
    udp[7] = 0;

    SMAPSendPacket(ra_ctl_frame, RA_HDR_LEN + len);
}

/* ---- Sending one snapshot ---------------------------------------------- */

/* The snapshot is copied out whole before it is split into packets.
   Otherwise the EE could land the next frame between two sends and the
   parts would describe different moments; a condition such as
   "A == 1 and B == 2 in the same frame" would fire on a mix. */
static u8 ra_stage[RA_SNAP_MAX_BYTES] __attribute__((aligned(4)));

/* Byte loops took the IOP most of a millisecond per snapshot; words when
   both sides share alignment, halfwords when they share parity. */
static void ra_copy(u8 *d, const u8 *src, u32 n)
{
    u32 i = 0;

    if ((((u32)d ^ (u32)src) & 3) == 0) {
        while (i < n && ((u32)(d + i) & 3) != 0) {
            d[i] = src[i];
            i++;
        }
        for (; i + 4 <= n; i += 4)
            *(u32 *)(d + i) = *(const u32 *)(src + i);
    } else if ((((u32)d ^ (u32)src) & 1) == 0) {
        if (i < n && ((u32)(d + i) & 1) != 0) {
            d[i] = src[i];
            i++;
        }
        for (; i + 2 <= n; i += 2)
            *(u16 *)(d + i) = *(const u16 *)(src + i);
    }
    for (; i < n; i++)
        d[i] = src[i];
}

static void ra_send_one(void)
{
    USE_SMAP_REGS;
    iop_sys_clock_t t0, t1;
    u32 nb = 0, parts = 1, part, staged_sq = 0;
    int ret, sent = 1;

    ra_rxq = SMAP_REG8(SMAP_R_RXFIFO_FRAME_CNT);

    /* Checked once per snapshot, not per part: dropping the last part
       of three would waste the two already sent. Busy means the whole
       snapshot waits for the next poll. Pushing into a busy controller
       produced duplicate frames on the wire. */
    if (ra_tx_busy()) {
        ra_skip++;
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
       what was copied: retry the snapshot on the next poll. */
    if (ra_snap != NULL) {
        u32 sq = ra_snap->seq;

        if (ra_snap->magic == RA_SNAP_MAGIC) {
            const u8 *src = (const u8 *)ra_snap + RA_SNAP_HDR;
            u32 tail;

            nb = ra_snap->bytes;
            if (nb > RA_SNAP_MAX_BYTES)
                nb = RA_SNAP_MAX_BYTES;

            ra_copy(ra_stage, src, nb);

            tail = *(volatile u32 *)((const u8 *)ra_snap + RA_SNAP_TRAILER_OFF(nb));

            if (tail != sq || ra_snap->seq != sq) {
                ra_snap_bad++;
                return; /* the next poll copies it whole */
            } else {
                staged_sq = sq;
                ra_fmt(&ra_payload[RA_OFF(RA_F_SQ)], sq, 6);
                ra_fmt(&ra_payload[RA_OFF(RA_F_DS)], ra_snap->dma_skip, 6);
                ra_fmt(&ra_payload[RA_OFF(RA_F_N)], ra_snap->count, 4);

                ra_id_put((const char *)ra_snap->game_id);
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

        if (len > ra_chunk)
            len = ra_chunk;

        ra_fmt(&ra_payload[RA_OFF(RA_F_SEQ)], ra_seq, 6);
        ra_fmt(&ra_payload[RA_OFF(RA_F_VB)], len, 4);
        ra_fmt(&ra_payload[RA_OFF(RA_F_PT)], part, 1);
        ra_fmt(&ra_payload[RA_OFF(RA_F_NP)], parts, 1);

        ra_copy(&ra_payload[ra_head_len], &ra_stage[off], len);

        ret = SMAPSendPacket(ra_frame, RA_FRAME_LEN);

        if (ret < 0) {
            ra_fail++;
            ra_err = ret;
            sent = 0;
        }

        ra_seq++;
    }

    GetSystemTime(&t1);

    ra_us = ra_usec_delta(&t0, &t1);
    if (ra_us > ra_us_max)
        ra_us_max = ra_us;

    /* Commit only the staged sequence, after every part was accepted.
       The EE may already have DMAed the next snapshot while we sent this
       one. A failed part leaves this sequence pending for the next poll. */
    if (nb > 0 && sent) {
        ra_sent_sq = staged_sq;
        ra_sent_any = 1;
    }
}

/* -1: no snapshot to speak of; 1: one the PC has not seen; 0: sent already. */
static int ra_snap_pending(void)
{
    if (ra_snap == NULL || ra_snap->magic != RA_SNAP_MAGIC)
        return -1;
    return (!ra_sent_any || ra_snap->seq != ra_sent_sq) ? 1 : 0;
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
                    ra_sock = s; /* kept open: the PC's unlock notices arrive here */
                    return 1;
                }
            }
        }

        tries++;
        if (tries >= RA_DISC_FAST) {
            /* The endless slow re-query exists so the PC client may start
               after the game. On a share it must not: every poll posts to the
               stack mailbox the SMB or HTTP client is waiting on, and there
               that is the road the game's own disc arrives by -- the same
               reason ra_rx_in_game shuts the in-play reads down. Discovery
               runs before that gate is reached, so bound it here instead:
               the fast phase gets ten seconds, inside the start-up hold-off,
               and then the stack is left alone for the rest of the session.
               Nothing that worked is lost -- with no PC there is no address
               to send telemetry to either. */
            if (!ra_rx_in_game) {
                lwip_close(s);
                return 0;
            }

            DelayThread(RA_DISC_SLOW_US);
            if (ra_disc_us < 0xF0000000)
                ra_disc_us += RA_DISC_SLOW_US;
        }
    }
}

/* ---- Messages from the PC ---------------------------------------------- */

/* Decimal digits at s, stopping at the first non-digit. */
static u32 ra_dec_at(const char *s, int max)
{
    u32 v = 0;
    int i;

    for (i = 0; i < max && s[i] >= '0' && s[i] <= '9'; i++)
        v = v * 10 + (u32)(s[i] - '0');

    return v;
}

/* A badge chunk: "RAB1 ii tt " (11 bytes) and 512 raw pixels. The sixteenth
   distinct chunk goes to the EE in one SIF DMA, answered with "RAK2 <mask>
   <dma>". */
static void ra_take_badge(const char *rx, int got)
{
    u32 idx = ra_dec_at(&rx[5], 2);
    u32 total = ra_dec_at(&rx[8], 2);
    char ack[24];
    int dma = 0, n;

    if (got != 11 + RA_BADGE_CHUNK || total != RA_BADGE_CHUNKS || idx >= RA_BADGE_CHUNKS)
        return;
    if (ra_badge_done)
        return;

    {
        u8 *dst = &ra_badge.px[idx * RA_BADGE_CHUNK];
        int i;

        for (i = 0; i < RA_BADGE_CHUNK; i++)
            dst[i] = (u8)rx[11 + i];
    }
    ra_hb_rab++;
    ra_badge_mask |= (u32)1 << idx;

    if (ra_badge_mask != ((u32)1 << RA_BADGE_CHUNKS) - 1)
        return;

    ra_badge.magic = RA_BADGE_MAGIC;
    ra_badge.len = RA_BADGE_BYTES;
    ra_badge.seq++;
    ra_badge_done = 1;

    if (ra_ee_badge != 0) {
        /* Two transfers, pixels first: SIF DMA fills memory upward, and
           the EE takes the header's magic as "the badge is here". Sent
           as one transfer the magic would land before the pixels. */
        SifDmaTransfer_t dmat[2];

        dmat[0].src = ra_badge.px;
        dmat[0].dest = (void *)(ra_ee_badge + 16);
        dmat[0].size = RA_BADGE_BYTES;
        dmat[0].attr = 0;
        dmat[1].src = &ra_badge;
        dmat[1].dest = (void *)ra_ee_badge;
        dmat[1].size = 16;
        dmat[1].attr = 0;
        dma = sceSifSetDma(dmat, 2);
    }

    n = 0;
    ack[n++] = 'R';
    ack[n++] = 'A';
    ack[n++] = 'K';
    ack[n++] = '2';
    ack[n++] = ' ';
    ra_fmt_hex8(&ack[n], ra_ee_badge);
    n += 8;
    ack[n++] = ' ';
    ra_fmt_err((u8 *)&ack[n], dma);
    n += 4;
    ack[n] = '\0';
    ra_ctl_send(ack, n);
}

/* One message from the PC, whichever road it came in on. */
static void ra_handle_pc(char *rx, int got)
{
    char ack[48];
    int dma = 0, n;

    if (got < 5)
        return;

    ra_hb_rx++;

    if (rx[0] == 'R' && rx[1] == 'A' && rx[2] == 'B' && rx[3] == '1' && rx[4] == ' ') {
        ra_take_badge(rx, got);
        return;
    }

    if (rx[0] != 'R' || rx[1] != 'A' || rx[2] != 'U' || rx[3] != '1' || rx[4] != ' ')
        return;

    ra_hb_rau++;

    /* Every notice arrives twice, unicast and broadcast (the console
       answers no ARP in play). The same achievement within a few seconds is
       one pulse. */
    {
        static u32 last_id = 0xFFFFFFFF;
        static u32 last_sec = 0;
        iop_sys_clock_t clk;
        u32 sec, usec, id;

        id = ra_dec_at(&rx[5], 10);
        GetSystemTime(&clk);
        SysClock2USec(&clk, &sec, &usec);

        if (id == last_id && sec - last_sec < 3)
            return;
        last_id = id;
        last_sec = sec;

        ra_event.magic = RA_EVENT_MAGIC;
        ra_event.seq++;
        ra_event.kind = RA_EVENT_UNLOCK;
        ra_event.arg = id;
    }

    if (ra_ee_event != 0) {
        SifDmaTransfer_t dmat;

        dmat.src = &ra_event;
        dmat.dest = (void *)ra_ee_event;
        dmat.size = sizeof(ra_event);
        dmat.attr = 0;
        dma = sceSifSetDma(&dmat, 1);
    }

    /* "RAK1 <seq> <ee-buffer> <dma-id>": the PC logs it, which is how a
       notice that never showed is traced to its link. */
    n = 0;
    ack[n++] = 'R';
    ack[n++] = 'A';
    ack[n++] = 'K';
    ack[n++] = '1';
    ack[n++] = ' ';
    ra_fmt((u8 *)&ack[n], ra_event.seq, 6);
    n += 6;
    ack[n++] = ' ';
    ra_fmt_hex8(&ack[n], ra_ee_event);
    n += 8;
    ack[n++] = ' ';
    ra_fmt_err((u8 *)&ack[n], dma);
    n += 4;
    ack[n] = '\0';
    ra_ctl_send(ack, n);
}

/* The lwIP road: worked at boot, dead in play. Kept for the boot
   window; costs one non-blocking call per period. */
static void ra_poll_pc(void)
{
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    /* Room for the largest message: a badge chunk of 11 + 512 bytes. */
    char rx[576];
    int got;

    if (ra_sock < 0)
        return;

    got = lwip_recvfrom(ra_sock, NULL, 0, rx, sizeof(rx) - 1, MSG_DONTWAIT,
                        (struct sockaddr *)&from, &fromlen);
    if (got < 5)
        return;
    rx[got] = '\0';

    ra_handle_pc(rx, got);
}

/* The raw receive road: in play nothing drains the SMAP RX FIFO (it sits
   full, ARP dies with it), so this thread walks the receive BDs itself, as
   HandleRxIntr does. It must be the ring's only consumer while a game runs. */
static u8 ra_rxfrm[608] __attribute__((aligned(4)));
static int ra_rx_bdi = 0;

static void ra_drain_rx(void)
{
    USE_SMAP_REGS;
    USE_SMAP_RX_BD;
    int budget;

    for (budget = 0; budget < 16; budget++) {
        volatile smap_bd_t *bd;
        u16 cs, len, ptr;
        int words, i, up;

        if (SMAP_REG8(SMAP_R_RXFIFO_FRAME_CNT) == 0)
            return;

        bd = &rx_bd[ra_rx_bdi % SMAP_BD_MAX_ENTRY];
        cs = bd->ctrl_stat;
        if (cs & SMAP_BD_RX_EMPTY) {
            /* Frames somewhere in the ring, but not at this index: the
               boot-time driver consumed an unknown number before going
               quiet. Walk until the indexes line up. */
            ra_rx_bdi++;
            continue;
        }

        len = bd->length;
        ptr = bd->pointer;

        if (!(cs & SMAP_BD_RX_ERROR) && len >= 60 && len <= (u16)sizeof(ra_rxfrm)) {
            u32 *w = (u32 *)ra_rxfrm;

            SMAP_REG16(SMAP_R_RXFIFO_RD_PTR) = ptr;
            words = ((int)len + 3) / 4;
            for (i = 0; i < words; i++)
                w[i] = SMAP_REG32(SMAP_R_RXFIFO_DATA);

            /* IPv4 without options, UDP, to our port; everything else
               in the backlog -- mDNS, SSDP, ARP, weeks of broadcast
               noise -- is dropped unread past this line. */
            if (ra_rxfrm[12] == 0x08 && ra_rxfrm[13] == 0x00 &&
                ra_rxfrm[14] == 0x45 && ra_rxfrm[23] == 17 &&
                ra_rxfrm[36] == (RA_SRC_PORT >> 8) &&
                ra_rxfrm[37] == (RA_SRC_PORT & 0xFF)) {
                up = (((int)ra_rxfrm[38] << 8) | ra_rxfrm[39]) - RA_UDP_HLEN;
                if (up > 0 && RA_HDR_LEN + up < (int)sizeof(ra_rxfrm)) {
                    char *pay = (char *)&ra_rxfrm[RA_HDR_LEN];

                    pay[up] = '\0';
                    ra_handle_pc(pay, up);
                }
            }
        }

        SMAP_REG8(SMAP_R_RXFIFO_FRAME_DEC) = 0;
        bd->ctrl_stat = SMAP_BD_RX_EMPTY;
        ra_rx_bdi++;
    }
}

/* Every ten seconds: "RAH1 <datagrams> <unlocks> <chunks> <mask> <done>".
   No heartbeat means the send path is down; zero datagrams means nothing
   from the PC survives the trip. */
static void ra_heartbeat(void)
{
    char hb[48];
    int n = 0;

    hb[n++] = 'R';
    hb[n++] = 'A';
    hb[n++] = 'H';
    hb[n++] = '1';
    hb[n++] = ' ';
    ra_fmt((u8 *)&hb[n], ra_hb_rx, 6);
    n += 6;
    hb[n++] = ' ';
    ra_fmt((u8 *)&hb[n], ra_hb_rau, 6);
    n += 6;
    hb[n++] = ' ';
    ra_fmt((u8 *)&hb[n], ra_hb_rab, 6);
    n += 6;
    hb[n++] = ' ';
    ra_fmt_hex8(&hb[n], ra_badge_mask);
    n += 8;
    hb[n++] = ' ';
    hb[n++] = ra_badge_done ? '1' : '0';
    hb[n] = '\0';
    ra_ctl_send(hb, n);
}

/* ---- Thread and module entry ------------------------------------------- */

static void ra_thread(void *arg)
{
    u32 iter = 0;
    u32 idle = 0; /* polls since the last new snapshot */

    (void)arg;

    if (!ra_discover())
        return; /* no socket: the stack is not up, stay silent */

    ra_frame_init();

    if (ra_disc_us < RA_QUIET_US)
        DelayThread(RA_QUIET_US - ra_disc_us);

    for (;;) {
        int pending = ra_snap_pending();

        if (pending > 0) {
            ra_send_one();
            idle = 0;
        } else if (pending < 0) {
            if (iter % RA_IDLE_TICKS == 0)
                ra_send_one();
        } else if (++idle >= RA_KEEPALIVE_TICKS) {
            ra_send_one();
            idle = 0;
        }
        if (ra_rx_in_game && iter % RA_IDLE_TICKS == 0) {
            ra_drain_rx();
            ra_poll_pc();
        }
        if (++iter % RA_HEARTBEAT_TICKS == 0)
            ra_heartbeat();
        DelayThread(RA_POLL_US);
    }
}

int _shutdown(void)
{
    return 0;
}

/* Arguments, built by ee_core (iopmgr.c). argv[1] is a comma-separated list,
   each field optional from the left:
     snapshot buffer address in IOP RAM, eight hex digits
     event buffer address in EE RAM, eight hex digits
     badge buffer address in EE RAM, eight hex digits
     '1' or '0': may this module read from the network while a game runs
     the game's serial, up to fifteen characters
   argv[2] is the own IP in dotted form (followed by netmask and gateway,
   which this module does not need). argv[0] is the module name, inserted by
   the IOP loader. */
int _start(int argc, char *argv[])
{
    iop_thread_t thread;
    int tid;

    if (argc >= 2 && argv[1] != NULL) {
        int len;

        for (len = 0; len < 44 && argv[1][len] != '\0'; len++)
            ;

        if (len >= 8)
            ra_snap = (volatile struct ra_snap *)ra_hex_at(argv[1], 0, 8);
        if (len >= 17 && argv[1][8] == ',')
            ra_ee_event = ra_hex_at(argv[1], 9, 8);
        if (len >= 26 && argv[1][17] == ',')
            ra_ee_badge = ra_hex_at(argv[1], 18, 8);
        if (len >= 28 && argv[1][26] == ',')
            ra_rx_in_game = argv[1][27] != '0';
        if (len >= 29 && argv[1][28] == ',') {
            int i;

            for (i = 0; i < 15 && argv[1][29 + i] != '\0'; i++)
                ra_game_id[i] = argv[1][29 + i];
            ra_game_id[i] = '\0';
        }
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
