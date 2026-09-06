/*
 * HTTP ISO transport, following Docmine17/Open-PS2-Loader-HTTP @ 6fced11a.
 * Copyright 2024, Open PS2 Loader; integration Copyright 2026, RiptOPL contributors.
 * Licensed under the Academic Free License version 3.0. See LICENSE.
 */
#include "smstcpip.h"
#include "internal.h"
#include "httpclient.h"

// cdvdman loads before ps2ip in the replacement IOPRP, so resolve the SAME ordinals as SMB
// on first filesystem access, after ee_core has loaded SMSTCPIP and SMAP.
static int (*httpClose)(int);
static int (*httpConnect)(int, struct sockaddr *, socklen_t);
static int (*httpRecv)(int, void *, int, unsigned int);
static int (*httpSend)(int, void *, int, unsigned int);
static int (*httpSocket)(int, int, int);
static u32 (*httpInetAddr)(const char *);
static int httpSock = -1, httpSema = -1, httpReady, httpStopped;
static iop_sys_clock_t readStart;

static int RecvBounded(int sock, void *buffer, int length)
{
    iop_sys_clock_t now;
    // A ten-second budget for the entire response, not ten seconds per dribbled TCP byte.
    // IOP timer ticks at 36.864 MHz; subtraction remains correct across its low-word wrap.
    for (;;) {
        int r = httpRecv(sock, buffer, length, MSG_DONTWAIT);
        if (r >= 0)
            return r;
        GetSystemTime(&now);
        if ((u32)(now.lo - readStart.lo) >= 368640000U)
            return -1;
        DelayThread(1000);
    }
}

static int SendData(int sock, char *data, int length)
{
    int sent = 0;
    while (sent < length) {
        int n = httpSend(sock, data + sent, length - sent, 0);
        if (n <= 0)
            return -1;
        sent += n;
    }
    return sent;
}

#include "../../network/common/httpstream.inc"

static void httpDisconnect(void)
{
    HttpStreamEnd();
    if (httpSock >= 0) {
        httpClose(httpSock);
        httpSock = -1;
    }
}

static int httpOpen(void)
{
    struct sockaddr_in addr;
    httpSock = httpSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (httpSock < 0)
        return 0;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cdvdman_settings.port);
    addr.sin_addr.s_addr = httpInetAddr(cdvdman_settings.server);
    // This lwIP predates nonblocking connect. One connect uses its TCP SYN retry timeout;
    // never copy the donor's infinite reconnect loop. Do not claim a ten-second connect bound.
    if (httpConnect(httpSock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        httpDisconnect();
        return 0;
    }
    return 1;
}

void DeviceInit(void)
{
    iop_sema_t sema = {0};
    sema.initial = sema.max = 1;
    httpSema = CreateSema(&sema);
}

void DeviceFSInit(void)
{
    modinfo_t info;
    if (httpSema < 0 || !getModInfo("ps2ip\0\0\0", &info))
        return;
    httpClose = info.exports[6];
    httpConnect = info.exports[7];
    httpRecv = info.exports[9];
    httpSend = info.exports[11];
    httpSocket = info.exports[13];
    httpInetAddr = info.exports[24];
    httpReady = 1;
}

int DeviceReady(void) { return SCECdComplete; }
void DeviceLock(void)
{
    if (httpSema >= 0)
        WaitSema(httpSema);
}
void DeviceUnmount(void)
{
    httpStopped = 1;
    httpDisconnect();
}
void DeviceStop(void) {}
// Called with interrupts disabled. Network close can block; the imminent reset reclaims it.
void DeviceDeinit(void) { httpStopped = 1; }

int DeviceReadSectors(u64 lsn, void *buffer, unsigned int sectors)
{
    u64 first, last, total, expected;
    int attempt, status, length, hasTotal, done, n, rv = SCECdErREAD;
    if (!sectors)
        return SCECdErNO;
    expected = ((u64)cdvdman_settings.size_hi << 32) | cdvdman_settings.size_lo;
    if (!httpReady || httpStopped || lsn > (expected >> 11) || sectors > (expected >> 11) - lsn ||
        sectors > 0x7fffffffU / 2048)
        return SCECdErREAD;
    first = lsn << 11;
    last = first + ((u64)sectors << 11) - 1;
    WaitSema(httpSema);
    // Retry the identical idempotent range once after a dropped persistent connection.
    for (attempt = 0; attempt < 2 && !httpStopped; attempt++) {
        if (httpSock < 0 && !httpOpen())
            break;
        GetSystemTime(&readStart);
        status = HttpStreamBegin(httpSock, cdvdman_settings.server, cdvdman_settings.uri,
                                 1, first, last, 1, &length, &total, &hasTotal);
        if (status == 206 && hasTotal && total == expected) {
            done = 0;
            while (done < length) {
                n = HttpStreamRead((char *)buffer + done, length - done);
                if (n <= 0)
                    break;
                done += n;
            }
            if (done == length) {
                rv = SCECdErNO;
                break;
            }
            status = HTTP_STREAM_ERR_TRUNC;
        }
        httpDisconnect();
        // HTTP errors, invalid framing, wrong ranges and changed images are terminal.
        if (status != HTTP_STREAM_ERR_SEND && status != HTTP_STREAM_ERR_TRUNC)
            break;
    }
    SignalSema(httpSema);
    return rv;
}
