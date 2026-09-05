/*
  RA: fetching the watch list from the PC client while the OPL menu is up.

  rcheevos derives the watch list from the achievement set it downloads
  from the RetroAchievements server, and rcheevos cannot run on the PS2.
  So the console asks the PC client (xeRAbora) for the list that belongs to
  the image it is about to launch.

  This runs in the menu, where the full lwIP stack is still available.
  The bypass and hand-built frames belong to the in-game side; nothing
  of that kind is needed here.

  The request is a broadcast: no PC address is stored anywhere, and
  whoever listens answers.

  Protocol (pc/RA/xerabora-0.1.0-alpha.4/protocol/PROTOCOL.md is the contract):
    console -> "RAP1 <own-ip> <own-port>"                    (discovery)
    PC      -> "RAO1 OK <client>/<version>"
    console -> "RAQ1 <hash32> <serial> <own-ip> <own-port>"
    PC      -> "RAA1 OK <bytes> <chunks> [<achievements> <title>]",
               "RAA1 WAIT" or "RAA1 NO"
    console -> "RAG1 <hash32> <chunk-index> <own-ip> <own-port>"
    PC      -> "RAC1 <index> <length> " + binary data

  The console's own address and port are part of the request because
  the PC client may run inside Docker, where NAT rewrites the source
  address. A reply sent back to the apparent source goes to the Docker
  gateway and dies. The socket is therefore bound to the fixed port
  18196 and the PC replies to that address directly.

  The list travels in chunks: it can approach two kilobytes, and IP
  fragment reassembly on this stack is nothing to rely on.

  Why the receive path looks the way it does. recv/recvfrom are not
  local calls here: libc forwards them over SIF RPC to ps2ips.irx and
  the data comes back by a separate DMA. That layer has two delivery
  paths:

  - a packet LARGER than 64 bytes into a 64-byte-aligned buffer: the
    middle goes by direct SIF DMA into our buffer, the same reliable
    mechanism everything else in OPL uses;
  - a packet of 64 bytes or LESS (or unaligned edges): the bytes go
    into a side structure, rests_pkt, and the recv_intr callback copies
    them out at the end of the RPC. On real hardware this path produced
    "recvfrom returned 14, buffer full of zeros".

  The rules this file follows:
  1) g_rx is 64-byte aligned, so the "middle" starts at byte zero and
     arrives by direct DMA;
  2) the PC pads EVERY reply to a multiple of 64 and to at least 128
     bytes, so there is neither head nor tail for recv_intr and the
     small-packet path is never used;
  3) one reply is at most 960 bytes: the IOP side has 1024 bytes per
     call (BUFF_SIZE in ps2ips.c), receives at offset 64 for an aligned
     buffer, and a datagram longer than 992 would overflow the receive
     buffer. A list chunk is therefore 896 bytes:
     896 + header + padding = 960;
  4) from/fromlen are always passed even though unused: the EE recvfrom
     wrapper memcpys into from unconditionally, and NULL means a write
     to address zero.

  Upstream design and implementation: hacan359. Port notes for this fork:
  the menu's stack is ps2sdk lwIP behind the vendored ps2ips, so
  MSG_DONTWAIT is lwIP's 0x08 here -- the 0x40 in
  modules/network/common/smstcpip-common.h belongs to SMSTCPIP, the
  IN-GAME stack, which this file never talks to.

  This file compiles to nothing unless RETROACHIEVEMENTS=1.
*/

#ifdef RETROACHIEVEMENTS

#include "include/opl.h"
#include "include/util.h"
#include "include/ioman.h"
#include "include/ranet.h"
#include "include/lang.h"
#include "include/supportbase.h" /* raHashStep: crumbs to the log on the share;
                                    LOG output is invisible in the menu on hardware */
#include "include/ethsupport.h"  /* ethGetNetConfig: own IP for the request */
#include "include/bdmsupport.h"  /* bdmIsUDPBDLoaded: the NIC interlock */
#include "include/udpfssupport.h" /* udpfsGetModulesLoaded: the NIC interlock */
#include "include/rawatch.h"     /* SetWatchList: list straight into memory */

#include <ps2ips.h>
#include <errno.h>
#include <kernel.h>
#include <delaythread.h> /* DelayThread */

/* Non-blocking mode goes through fcntl: this port does not declare
   ioctl FIONBIO, and a blocking recvfrom with no reply would hang the
   thread for good. */
#include <fcntl.h>
#include <string.h>
#include <time.h> /* clock(): round trip of the link test */

#define RA_PORT        18194
#define RA_MY_PORT     18196 /* own port: the PC replies here directly, past NAT */
#define RA_CHUNK       896   /* agreed with the PC; rule 3 in the header */
#define RA_RECV_MAX    992   /* cap for one recvfrom; rule 3 */
#define RA_TRY         12    /* attempts per request */
#define RA_POLL_MS     25    /* pause between polls for a reply */
/* How many times to ask again while the PC answers WAIT. Identifying
   the image is a live request to the RetroAchievements server and
   takes seconds. We wait only while the PC keeps answering: if it goes
   silent, ask() gives up within its own three seconds, so "client not
   running" stays a fast failure. */
#define RA_WAIT_ROUNDS 8
#define RA_MAX_BYTES   (16 * 1024)

/* Per-call non-blocking receive. The socket is also set non-blocking via
   fcntl (see open_pc_socket), but on the menu's netman lwIP stack that
   flag does not take effect: recvfrom blocks forever when no reply comes.
   In raAskPC the very first recvfrom is the drain loop that runs BEFORE
   the request is sent, so a blocking socket means the request never goes
   out at all. lwIP honours MSG_DONTWAIT per call regardless of the
   socket flag, so every receive here passes it.

   The value MUST be lwIP's 0x08, not the EE <sys/socket.h> MSG_DONTWAIT
   (0x80): the flag is forwarded verbatim over SIF RPC to lwip_recvfrom
   on the IOP, whose headers define it as 0x08. Sending 0x80 would arrive
   as an unknown flag and block. (The in-game SMSTCPIP stack defines 0x40;
   it serves ee_core's raudp, not this menu path.) */
#define RA_MSG_DONTWAIT 0x08

/* Alignment is mandatory: rule 1 in the header. */
static char g_rx[2048] __attribute__((aligned(64)));
static unsigned char g_wl[RA_MAX_BYTES];

/* The NIC settlement (design doc, decided 2026-09-03): RA's menu check is a
   fourth claimant on the one SMAP NIC. When a UDP transport (UDPBD, or the
   UDPFS filesystem chain) owns it, RA must NOT raise the SMB stack on top --
   ethLoadModules() would refuse anyway, but the caller deserves a distinct
   "network busy" answer rather than a generic socket failure. The in-game
   half is unaffected: raudp speaks over whatever SMAP the launch loaded. */
int raNetNicBusy(void)
{
    return bdmIsUDPBDLoaded() || udpfsGetModulesLoaded();
}

/* Waits for a reply, resending the request. Returns the length or -1.
   The socket is non-blocking; SO_RCVTIMEO is not to be trusted on this
   stack. */
static int ask(int sock, struct sockaddr_in *to, const char *req, char *out, int outmax)
{
    int t;

    int asklen = outmax - 1;

    /* Never let the IOP side read past its own buffer: rule 3. */
    if (asklen > RA_RECV_MAX)
        asklen = RA_RECV_MAX;

    for (t = 0; t < RA_TRY; t++) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int got, w;

        /* Drop replies to earlier retransmits: a late one would pass for
           the answer to this request. */
        while (recvfrom(sock, out, asklen, RA_MSG_DONTWAIT, (struct sockaddr *)&from, &fromlen) > 0)
            fromlen = sizeof(from);

        sendto(sock, req, strlen(req), 0, (struct sockaddr *)to, sizeof(*to));

        /* The reply may take a moment, so poll several times before
           resending. */
        for (w = 0; w < 10; w++) {
            got = recvfrom(sock, out, asklen, RA_MSG_DONTWAIT, (struct sockaddr *)&from, &fromlen);

            if (got > 0) {
                out[got] = '\0';
                return got;
            }

            DelayThread(RA_POLL_MS * 1000);
        }
    }

    return -1;
}

/* Opens the UDP socket for talking to the PC: brings the network up if
   it is down, allows broadcast, makes the socket non-blocking and binds
   it to RA_MY_PORT. Fills myaddr with " <ip> <port>" for the request
   text (empty when the console has no address yet) and ip with the own
   address. Returns the socket or -1.

   The network may be down: OPL loads the network modules only when the
   ETH device is enabled, and one can play from USB alone. Bringing it
   up here is allowed because this runs on the I/O thread, like the
   network settings dialog -- and only when no UDP transport owns the
   NIC (raNetNicBusy; ethLoadModules() is the runtime backstop).
   Non-blocking is mandatory: the first unanswered recvfrom would
   otherwise hang the thread for good. The fixed own port is the return
   path past Docker NAT (see the header). */
static int open_pc_socket(char *myaddr, int sz, u8 ip[4])
{
    struct sockaddr_in me;
    u8 mask[4], gw[4];
    int sock, on = 1, fl;

    myaddr[0] = '\0';

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        /* Record errno and what the module load answered, then retry
           regardless of that answer: a network that is already up may
           report "not zero", and giving up on that would skip a socket
           that would in fact succeed. */
        int e1 = errno, r;
        char crumb[64];

        LOG("RA: no socket (errno %d), bringing up the network\n", e1);
        snprintf(crumb, sizeof(crumb), "6b-bringing-up-network-errno-%d", e1);
        raHashStep(crumb);

        r = ethLoadInitModules();
        sock = socket(AF_INET, SOCK_DGRAM, 0);

        if (sock < 0) {
            snprintf(crumb, sizeof(crumb), "6x-no-socket-errno-%d-ethinit-%d", errno, r);
            LOG("RA: socket creation failed, errno %d, ethLoadInitModules %d\n", errno, r);
            raHashStep(crumb);
            return -1;
        }
    }

    /* Which descriptor we got, for the log. errno on a socket() failure
       is not a diagnosis here: libcglue's __ps2ipcSocketHelper reports
       ENFILE (23) for ANY failure of the IOP-side socket(). The real
       exhaustion to watch for is the UDP PCB pool (DHCP, DNS, NBNS and
       ours) -- kept from leaking by the vendored ps2ips fix. */
    {
        char crumb[32];

        snprintf(crumb, sizeof(crumb), "6c-sock-%d", sock);
        raHashStep(crumb);
    }

    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    fl = fcntl(sock, F_GETFL, 0);
    if (fl >= 0)
        fcntl(sock, F_SETFL, fl | O_NONBLOCK);

    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_port = htons(RA_MY_PORT);
    me.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&me, sizeof(me)) < 0) {
        LOG("RA: bind failed\n");
        raHashStep("6x-bind-failed");
    }

    ethGetNetConfig(ip, mask, gw);
    if (ip[0] | ip[1] | ip[2] | ip[3])
        snprintf(myaddr, sz, " %d.%d.%d.%d %d", ip[0], ip[1], ip[2], ip[3], RA_MY_PORT);

    return sock;
}

static void broadcast_target(struct sockaddr_in *to)
{
    memset(to, 0, sizeof(*to));
    to->sin_family = AF_INET;
    to->sin_port = htons(RA_PORT);
    to->sin_addr.s_addr = htonl(INADDR_BROADCAST);
}

int raAskPC(const char *hash, const char *serial, const char *savepath,
            char *info, int infosz, char *info2, int info2sz)
{
    struct sockaddr_in to;
    char req[80];
    char myaddr[32];
    u8 ip[4];
    int sock, got;
    int total = 0, chunks = 0, received = 0, i;

    if (info != NULL && infosz > 0)
        info[0] = '\0';
    if (info2 != NULL && info2sz > 0)
        info2[0] = '\0';

    if (raNetNicBusy()) {
        LOG("RA: NIC is owned by a UDP transport, not asking the PC\n");
        raHashStep("6-nic-busy-udp-transport");
        return -8;
    }

    sock = open_pc_socket(myaddr, sizeof(myaddr), ip);
    if (sock < 0)
        return -1;

    broadcast_target(&to);

    snprintf(req, sizeof(req), "RAQ1 %s %s%s", hash, serial, myaddr);
    LOG("RA: asking the PC about %s\n", hash);

    got = ask(sock, &to, req, g_rx, sizeof(g_rx));
    if (got <= 0) {
        LOG("RA: no reply from the PC\n");
        disconnect(sock);
        return -2;
    }

    /* WAIT means the PC hears us and is asking the RA server about this
       image. Keep asking while it answers that way. */
    {
        int w;

        for (w = 0; w < RA_WAIT_ROUNDS && strncmp(g_rx, "RAA1 WAIT", 9) == 0; w++) {
            LOG("RA: PC is asking the server, waiting (%d)\n", w + 1);

            got = ask(sock, &to, req, g_rx, sizeof(g_rx));
            if (got <= 0) {
                LOG("RA: PC went silent while identifying the image\n");
                disconnect(sock);
                return -2;
            }
        }
    }

    if (strncmp(g_rx, "RAA1 WAIT", 9) == 0) {
        LOG("RA: PC is still identifying the image\n");
        disconnect(sock);
        return -7;
    }

    if (strncmp(g_rx, "RAA1 OK ", 8) != 0) {
        LOG("RA: PC does not know this image: %s\n", g_rx);
        disconnect(sock);
        return 1; /* not an error: the game is unsupported */
    }

    /* Reply body after "RAA1 OK ": "<bytes> <chunks>" then the
       achievement counts and the title. Newer clients send three counts
       (total, unlocked, unsupported); older ones send only the total.
       The title may itself begin with digits ("2 Fast 2 Furious"), so
       the five-number form is accepted only when all five parse, and the
       older three-number form is re-read otherwise -- that also puts the
       title back together correctly. */
    {
        int bytes = 0, ch = 0;
        int nach = 0, nunlocked = -1, nunsupported = -1, pos = 0;
        char *title;
        int len;

        if (sscanf(g_rx + 8, "%d %d %d %d %d %n",
                   &bytes, &ch, &nach, &nunlocked, &nunsupported, &pos) != 5) {
            nunlocked = nunsupported = -1; /* unknown: older client */
            pos = 0;
            sscanf(g_rx + 8, "%d %d %d %n", &bytes, &ch, &nach, &pos);
        }

        title = g_rx + 8 + pos;
        len = (int)strlen(title);
        while (len > 0 && title[len - 1] == ' ')
            title[--len] = '\0';

        if (info != NULL && infosz > 0)
            snprintf(info, infosz, "%s", len > 0 ? title : _l(_STR_RA_SUPPORTED));

        if (info2 != NULL && info2sz > 0) {
            if (nunlocked >= 0)
                snprintf(info2, info2sz, _l(_STR_RA_ACHIEVEMENTS_FULL),
                         nach, nunlocked, nunsupported);
            else
                snprintf(info2, info2sz, _l(_STR_RA_ACHIEVEMENTS), nach);
        }
    }

    if (sscanf(g_rx + 8, "%d %d", &total, &chunks) != 2 ||
        total <= 0 || total > RA_MAX_BYTES || chunks <= 0) {
        LOG("RA: malformed reply: %s\n", g_rx);
        disconnect(sock);
        return -3;
    }

    LOG("RA: list is %d bytes in %d chunks\n", total, chunks);

    for (i = 0; i < chunks; i++) {
        int idx, len, hdr = 0;

        int attempt;

        snprintf(req, sizeof(req), "RAG1 %s %d%s", hash, i, myaddr);

        /* A reply carrying another chunk's index is a leftover from an
           earlier request: ask again rather than fail. */
        for (attempt = 0; attempt < 3; attempt++) {
            got = ask(sock, &to, req, g_rx, sizeof(g_rx));
            if (got <= 0) {
                LOG("RA: chunk %d did not arrive\n", i);
                disconnect(sock);
                return -4;
            }
            idx = -1;
            hdr = 0;
            if (sscanf(g_rx, "RAC1 %d %d %n", &idx, &len, &hdr) >= 2 && idx == i)
                break;
        }

        if (idx != i) {
            LOG("RA: chunk %d is malformed\n", i);
            disconnect(sock);
            return -5;
        }

        if (len < 0 || len > RA_CHUNK || i * RA_CHUNK + len > RA_MAX_BYTES || hdr == 0 || hdr + len > got) {
            LOG("RA: chunk %d does not fit\n", i);
            disconnect(sock);
            return -6;
        }

        memcpy(&g_wl[i * RA_CHUNK], &g_rx[hdr], len);
        received += len;
    }

    disconnect(sock);

    if (received != total) {
        LOG("RA: chunks add up to %d bytes, expected %d\n", received, total);
        return -6;
    }

    /* Into loader memory first. Launching the game then does not depend
       on whether the file reached the medium: on a USB stick writes sit
       in the USB driver's cache and the file may appear later, or never
       if the power goes off. */
    if (SetWatchList(g_wl, total, serial) > 0)
        raHashStep("8-list-in-memory");
    else
        raHashStep("8x-list-not-parsed");

    /* And as a file, for future launches and so the game gets its badge
       in the list. Two copies: next to the game (the loader reads from
       there) and on the share, if there is one, where writes are
       reliable and the loader looks second (sbLoadWatchList). Failing
       either copy is not an error: the list is already in memory. */
    {
        const char *where[2];
        char dir[128], file[192];
        FILE *f;
        int w, saved = 0;

        where[0] = savepath;
        where[1] = strncmp(savepath, "smb0:", 5) != 0 ? "smb0:" : NULL;

        for (w = 0; w < 2; w++) {
            if (where[w] == NULL)
                continue;

            snprintf(dir, sizeof(dir), "%sRA", where[w]);
            mkdir(dir, 0777);
            snprintf(file, sizeof(file), "%sRA/%s.wl", where[w], serial);

            f = fopen(file, "wb");
            if (f == NULL) {
                LOG("RA: could not write %s\n", file);
                continue;
            }

            fwrite(g_wl, 1, (size_t)total, f);
            fclose(f);
            saved++;

            LOG("RA: list saved: %s (%d bytes)\n", file, total);
        }

        if (saved == 0)
            raHashStep("9x-file-not-written");
    }

    return 0;
}

/* Link test for the menu: broadcasts every 250 ms for up to three seconds.
   The reply's source address and the version in its text tell what
   answered; the round trip is measured with clock(). */
int raNetTestLink(char *line1, int sz1, char *line2, int sz2)
{
    struct sockaddr_in to, from;
    socklen_t fromlen;
    char myaddr[32], req[64];
    /* Rules 1 and 3 from the file header apply here too: a 64-byte
       aligned buffer received in a length that is a multiple of 64, so
       the reply arrives entirely by direct DMA and the ps2ips "rests"
       path (with its EE-side buffer limit) is never used. */
    static char rx[256] __attribute__((aligned(64)));
    u8 ip[4];
    int sock, got = 0, found = 0;
    clock_t t0, deadline;
    unsigned int ms;

    if (raNetNicBusy()) {
        snprintf(line1, sz1, "%s", _l(_STR_RA_NET_BUSY));
        snprintf(line2, sz2, "%s", _l(_STR_RA_NET_BUSY2));
        return 0;
    }

    sock = open_pc_socket(myaddr, sizeof(myaddr), ip);
    if (sock < 0) {
        /* Either the network is really down or the IOP refused a socket
           (pool exhausted). Name both, rather than blaming the cable. */
        snprintf(line1, sz1, "%s", _l(_STR_RA_NO_SOCKET));
        snprintf(line2, sz2, "%s", _l(_STR_RA_NO_SOCKET2));
        return 0;
    }

    if (myaddr[0] == '\0') {
        disconnect(sock);
        snprintf(line1, sz1, "%s", _l(_STR_RA_NO_IP));
        snprintf(line2, sz2, "%s", _l(_STR_RA_NO_IP2));
        return 0;
    }

    broadcast_target(&to);
    snprintf(req, sizeof(req), "RAP1%s", myaddr);

    t0 = clock();
    deadline = t0 + 3 * CLOCKS_PER_SEC;

    while (!found && clock() < deadline) {
        int w;

        sendto(sock, req, strlen(req), 0, (struct sockaddr *)&to, sizeof(to));

        for (w = 0; w < 10 && !found; w++) {
            memset(rx, 0, sizeof(rx));
            fromlen = sizeof(from);
            /* 192: a multiple of 64 (rule 3) that leaves room for the
               terminating zero from the memset above. */
            got = recvfrom(sock, rx, sizeof(rx) - 64, RA_MSG_DONTWAIT, (struct sockaddr *)&from, &fromlen);
            if (got >= 4 && strncmp(rx, "RAO1", 4) == 0)
                found = 1;
            else
                DelayThread(RA_POLL_MS * 1000);
        }
    }

    ms = (unsigned int)((clock() - t0) * 1000 / CLOCKS_PER_SEC);
    disconnect(sock);

    if (!found) {
        snprintf(line1, sz1, "%s", _l(_STR_RA_TEST_NO_ANSWER));
        snprintf(line2, sz2, _l(_STR_RA_TEST_NO_ANSWER2), myaddr);
        return 0;
    }

    {
        const char *ver = (got > 8 && strncmp(rx, "RAO1 OK ", 8) == 0) ? rx + 8 : "";
        unsigned int a = ntohl(from.sin_addr.s_addr);
        char peer[24] = "";

        if (a != 0)
            snprintf(peer, sizeof(peer), "%u.%u.%u.%u", (a >> 24) & 255, (a >> 16) & 255, (a >> 8) & 255, a & 255);

        snprintf(line1, sz1, _l(_STR_RA_TEST_FOUND), peer[0] ? " at " : "", peer, ver);
        snprintf(line2, sz2, _l(_STR_RA_TEST_FOUND2), ms, myaddr);
    }

    return 1;
}

#endif /* RETROACHIEVEMENTS */
