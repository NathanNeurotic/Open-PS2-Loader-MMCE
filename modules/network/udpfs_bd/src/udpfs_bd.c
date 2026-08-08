/*
 * UDPFS BD - Block Device over UDPFS/UDPRDMA
 *
 * Provides block device access over network using UDPRDMA for reliable transport.
 * Uses UDPFS block I/O subset (BREAD/BWRITE) with handle 0.
 * Block device capacity is queried via GETSTAT; sectors are fixed at 512 bytes.
 */

#include <errno.h>
#include <bdm.h>
#include <thbase.h>
#include <stdio.h>
#include <string.h>

#include "main.h"
#include "udpfs_core.h"


/* Block device handle - pre-opened by server */
#define BLOCK_DEVICE_HANDLE 0

/* State */
static struct block_device g_udpbd;
static int g_udpfs_registered = 0; // bd currently registered with BDM
static int g_udpfs_tid = 0;        // lazy-connect / reconnect watchdog thread id


/*
 * Block device read (retry + chunking handled by udpfs_bread)
 *
 * On failure just return the error: NO teardown from inside our own bd callback. The BDM/fatfs stack
 * that issued this read is still on the call stack -- bdm_disconnect_bd() here re-enters it mid-
 * operation (freeing filesystem state under the caller's feet). A transport-level failure has already
 * flipped the UDPRDMA session to disconnected, which the watchdog thread observes and handles OUTSIDE
 * callback context (disconnect bd -> re-discover -> re-register); a server-side error on a healthy
 * session is just an IO error, not a reason to drop the device.
 */
static int udpfs_bd_read(struct block_device *bd, uint64_t sector, void *buffer, uint16_t count)
{
    int ret;

    if (!udpfs_core_is_connected())
        return -EIO;

    if (sector >= bd->sectorCount)
        return -EINVAL;

    if ((sector + count) > bd->sectorCount)
        count = bd->sectorCount - sector;

    ret = udpfs_core_bread(BLOCK_DEVICE_HANDLE, sector, buffer, count, bd->sectorSize);
    if (ret < 0) {
        M_DEBUG("udpfs_bd: read failed (%d); teardown deferred to the watchdog\n", ret);
        return ret;
    }

    return count;
}


/*
 * Write sectors via shared block I/O helper (zero-copy). Same no-teardown-in-callback rule as read.
 */
static int udpfs_bd_write(struct block_device *bd, uint64_t sector, const void *buffer, uint16_t count)
{
    M_DEBUG("udpfs_bd: write sector=%u, count=%u\n", (uint32_t)sector, count);

    if (!udpfs_core_is_connected())
        return -EIO;

    if (sector >= bd->sectorCount)
        return -EINVAL;

    if ((sector + count) > bd->sectorCount)
        count = bd->sectorCount - sector;

    if (udpfs_core_bwrite(BLOCK_DEVICE_HANDLE, sector,
                          buffer, count, bd->sectorSize) < 0) {
        M_DEBUG("udpfs_bd: write failed; teardown deferred to the watchdog\n");
        return -EIO;
    }

    return count;
}


static void udpfs_bd_flush(struct block_device *bd)
{
    M_DEBUG("udpfs_bd: flush\n");
}


static int udpfs_bd_stop(struct block_device *bd)
{
    M_DEBUG("udpfs_bd: stop\n");
    return 0;
}


/*
 * Try once to (re)discover the server, query capacity, and register the block device.
 * Returns 1 on success (bd connected), 0 if the server isn't reachable yet. Safe to retry indefinitely:
 * udpfs_core_init() binds the UDPRDMA socket ONCE and reuses it on every retry. We must NOT destroy +
 * recreate the socket per attempt -- the ministack has no udp_unbind (UDP_MAX_PORTS = 4), so that would
 * leak a port slot on every blip and deterministically brick UDPFS after a few retries.
 */
static int udpfs_bd_try_connect(void)
{
    int ret;

    /* Socket is created once and reused; returns <0 while the server isn't reachable (socket kept). */
    if (udpfs_core_init() < 0)
        return 0;

    /* Get disk capacity via GETSTAT (returns sector count). */
    ret = udpfs_core_get_sector_count(BLOCK_DEVICE_HANDLE);
    if (ret < 0)
        return 0; /* keep the bound socket; the watchdog will retry */

    /* Setup block device with fixed 512-byte sectors */
    g_udpbd.name = "udp";
    g_udpbd.devNr = 0;
    g_udpbd.parNr = 0;
    g_udpbd.sectorOffset = 0;
    g_udpbd.sectorSize = 512;  /* Fixed sector size */
    g_udpbd.sectorCount = ret; /* From GETSTAT / 512 */
    g_udpbd.priv = NULL;
    g_udpbd.read = udpfs_bd_read;
    g_udpbd.write = udpfs_bd_write;
    g_udpbd.flush = udpfs_bd_flush;
    g_udpbd.stop = udpfs_bd_stop;

    /* Connect to BDM */
    bdm_connect_bd(&g_udpbd);
    g_udpfs_registered = 1;

    M_DEBUG("udpfs_bd: ready (sectorSize=%u, sectorCount=%u)\n",
            g_udpbd.sectorSize, g_udpbd.sectorCount);
    return 1;
}

/*
 * Lazy-connect / reconnect watchdog. Keys off the UDPRDMA session state: while it is down -- the
 * server hadn't started yet at boot, or a timeout mid-use dropped the session -- first release the
 * registered bd (HERE, outside any bd callback: doing it inside our own read/write re-enters the
 * BDM/fatfs stack that is mid-operation), then retry discovery so the device (and its OPL page)
 * returns whenever the server is back, without a module reload. Cadence: the discovery probe inside
 * udpfs_bd_try_connect() blocks up to ~5s when the server is absent, then this loop adds a 1s pause,
 * so a missing server is re-probed roughly every ~6s. No-op while the session is healthy.
 */
static int udpfs_bd_watchdog(void *arg)
{
    (void)arg;
    while (1) {
        if (!udpfs_core_is_connected()) {
            if (g_udpfs_registered) {
                M_DEBUG("udpfs_bd: session down -- releasing bd for re-discovery\n");
                bdm_disconnect_bd(&g_udpbd);
                g_udpfs_registered = 0;
            }
            udpfs_bd_try_connect();
        }
        DelayThread(1000000); // 1s
    }
    return 0;
}

/*
 * Initialize UDPFS block device driver. Unlike the original, this does NOT fail closed when the server
 * isn't up at load time (a UDPFS server is typically hand-started after the PS2 boots): it tries once,
 * then leaves a watchdog thread retrying so the device appears as soon as the server is reachable.
 */
int udpfs_bd_init(void)
{
    iop_thread_t thinfo;

    M_DEBUG("UDPFS BD over UDPRDMA (lazy connect)\n");

    udpfs_bd_try_connect();

    if (g_udpfs_tid <= 0) {
        thinfo.attr = TH_C;
        thinfo.option = 0;
        thinfo.thread = (void *)udpfs_bd_watchdog;
        thinfo.stacksize = 0x1000;
        thinfo.priority = 0x20;
        g_udpfs_tid = CreateThread(&thinfo);
        if (g_udpfs_tid > 0)
            StartThread(g_udpfs_tid, NULL);
    }

    return 0; /* always resident; the bd connects lazily via the watchdog */
}
