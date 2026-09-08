/*
  Copyright 2009, Ifcaro
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.
*/

#ifdef __DECI2_DEBUG
#include <iopcontrol_special.h>
#endif

#include <delaythread.h> // DelayThread for the bounded disc re-detect polls in sysLaunchDisc
#include <time.h>        // clock()/CLOCKS_PER_SEC -- the disc waits are budgeted by wall clock

// Disc-probe budgets (issue #465). All are TOTAL wall-clock caps, not per-poll ones.
#define SYS_DISC_DETECT_MS   1000 // first "still identifying" settle
#define SYS_DISC_REDETECT_MS 4000 // spin-up + re-detect after a NODISC report
// Empty-tray shortcut inside the re-detect budget above. A drive that is actually working on a
// disc reports SCECdDETCT ("identifying") while it spins up; a drive with nothing in it only ever
// answers NODISC. So NODISC that has never once been punctuated by DETCT means an empty tray, and
// waiting out the rest of the re-detect budget only makes the user stare at a frozen-looking menu.
// Generous on purpose: this is a floor for a cold drive to reach DETCT, not a tuned timeout. Once
// DETCT is seen the FULL budget applies again, so a slow-but-working drive is unaffected.
#define SYS_DISC_NODISC_MS   2000
#include "include/opl.h"
#include "include/gui.h"
#include "include/lang.h" // _l(_STR_...) -- Neutrino preflight abort toasts
#include "include/ethsupport.h"
#include "include/hddsupport.h"
#include "include/util.h"
#include "include/pad.h"
#include "include/system.h"
#ifdef RETROACHIEVEMENTS
#include "include/discsupport.h"
#endif
#include "include/ioman.h"
#include "include/ioprp.h"
#include "include/bdmsupport.h"
#include "include/mmcesupport.h"
#include "include/OSDHistory.h"
#include "include/renderman.h"
#include "include/extern_irx.h"
#include "../ee_core/include/modules.h"
#include "../ee_core/include/coreconfig.h"
#include <osd_config.h>
#include "include/pggsm.h"
#include "include/cheatman.h"
#include "include/xparam.h"
#ifdef RETROACHIEVEMENTS
#include "include/rawatch.h"
#endif

#ifdef PADEMU
#include <libds34bt.h>
#include <libds34usb.h>
#endif

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>     // fileXioInit, fileXioExit, fileXioDevctl
#include <usbhdfsd-common.h> // USBMASS_IOCTL_GET_LBA / _CHECK_CHAIN -- VMC contiguity probe
#include <ps2sdkapi.h>       // ps2sdk_get_iop_fd -- the ioctls above take the IOP-side fd

typedef struct
{
    char VMC_filename[1024];
    int VMC_size_mb;
    int VMC_blocksize;
    int VMC_thread_priority;
    int VMC_card_slot;
} createVMCparam_t;

extern unsigned char eecore_elf[];
extern unsigned int size_eecore_elf;

extern unsigned char IOPRP_img[];
extern unsigned int size_IOPRP_img;

extern unsigned char eesync_irx[];
extern unsigned int size_eesync_irx;

#define MAX_MODULES 64
static void *g_sysLoadedModBuffer[MAX_MODULES];
static s32 sysLoadModuleLock = -1;

#define ELF_MAGIC   0x464c457f
#define ELF_PT_LOAD 1

typedef struct
{
    u8 ident[16]; // struct definition for ELF object header
    u16 type;
    u16 machine;
    u32 version;
    u32 entry;
    u32 phoff;
    u32 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} elf_header_t;

typedef struct
{
    u32 type; // struct definition for ELF program section header
    u32 offset;
    void *vaddr;
    u32 paddr;
    u32 filesz;
    u32 memsz;
    u32 flags;
    u32 align;
} elf_pheader_t;

void guiWarning(const char *text, int count);
void guiEnd();
void menuEnd();
void lngEnd();
void thmEnd();
void rmEnd();

static void poweroffHandler(void *arg);

int sysLoadModuleBuffer(void *buffer, int size, int argc, char *argv)
{

    int i, id, ret, index = 0;

    WaitSema(sysLoadModuleLock);

    // check we have not reached MAX_MODULES
    for (i = 0; i < MAX_MODULES; i++) {
        if (g_sysLoadedModBuffer[i] == NULL) {
            index = i;
            break;
        }
    }
    if (i == MAX_MODULES) {
        LOG("WARNING: REACHED MODULES LIMIT (%d)\n", MAX_MODULES);
        ret = -1;
        goto exit;
    }

    // check if the module was already loaded
    for (i = 0; i < MAX_MODULES; i++) {
        if (g_sysLoadedModBuffer[i] == buffer) {
            LOG("MODULE ALREADY LOADED (%d)\n", i);
            ret = 0;
            goto exit;
        }
    }

    // load the module
    id = SifExecModuleBuffer(buffer, size, argc, argv, &ret);
    LOG("\t-- ID=%d, ret=%d\n", id, ret);
    if ((id < 0) || (ret)) {
        ret = -2;
        goto exit;
    }

    // add the module to the list
    g_sysLoadedModBuffer[index] = buffer;

    ret = 0;
exit:
    SignalSema(sysLoadModuleLock);
    return ret;
}

#define OPL_SIF_CMD_BUFF_SIZE 1
static SifCmdHandlerData_t OplSifCmdbuffer[OPL_SIF_CMD_BUFF_SIZE];
static unsigned char dev9Initialized = 0, dev9Loaded = 0, dev9InitCount = 0;

void sysInitDev9(void)
{
    if (!dev9Initialized) {
        LOG("[DEV9]:\n");
        // DEV9.IRX must have loaded AND returned RESIDENT END for the adapter to be usable.
        dev9Loaded = (sysLoadModuleBuffer(&ps2dev9_irx, size_ps2dev9_irx, 0, NULL) == 0);

        // LATCH ONLY ON SUCCESS. This used to set dev9Initialized unconditionally, so a single
        // failed load disabled DEV9 -- and with it the NIC and the internal HDD -- for the whole
        // session: every later call saw the flag, skipped the load and just bumped the refcount.
        //
        // Retrying is safe rather than a double-load risk, because the guard lives one layer down:
        // sysLoadModuleBuffer keeps a table of buffers it has loaded, returns 0 for one it already
        // holds, and never records a failure. So a retry after a failure genuinely re-attempts, and
        // a retry after a success is a cheap no-op. That is also why no extra in-progress flag is
        // needed here -- that table is consulted under sysLoadModuleLock.
        dev9Initialized = dev9Loaded;
    }

    // Refcount every caller regardless, so each sysInitDev9 still pairs with its sysShutdownDev9.
    // On a failed load dev9Loaded stays 0, so the power-off at count 0 is correctly skipped.
    dev9InitCount++;
}

void sysShutdownDev9(void)
{
    if (dev9InitCount > 0) {
        --dev9InitCount;

        if (dev9InitCount == 0) { /* Switch off DEV9 once nothing needs it. */
            if (dev9Loaded) {
                // Bounded: an IOP whose dev9x never acknowledges (dying interface, wedged driver)
                // must not spin the power-off forever -- this runs on shutdown paths.
                int retry = 100;
                while (retry-- > 0 && fileXioDevctl("dev9x:", DDIOC_OFF, NULL, 0, NULL, 0) < 0) {
                    ;
                }
            }
        }
    }
}

void sysReset(int modload_mask)
{
    static int sInitialBoot = 1;

    if (!sInitialBoot) {
#ifdef PADEMU
        ds34usb_reset();
        ds34bt_reset();
#endif
        fileXioExit();
        SifExitIopHeap();
        SifLoadFileExit();
        SifExitRpc();
    } else {
        // Initial boot: detach inherited parent-launcher RPC/fileXio state before rebooting the IOP
        fileXioExit();
        SifExitRpc();
    }
    sInitialBoot = 0;

    SifInitRpc(0);

#ifdef _DTL_T10000
    while (!SifIopReset("rom0:UDNL", 0))
        ;
#else
#ifdef __DECI2_DEBUG
    while (!SifIopRebootBuffer(&deci2_img, size_deci2_img))
        ;
#else
    while (!SifIopReset("", 0))
        ;
#endif
#endif

    // The IOP was just rebooted: its DEV9 modules and refcount are gone, so reset the bookkeeping
    // vars together. Leaving dev9InitCount stale inflates the refcount across resets, so
    // sysShutdownDev9() would never power DEV9 off again.
    //
    // dev9Loaded is cleared for the same reason the other two are -- the module it describes no
    // longer exists. It was previously left behind, which was harmless only because the old
    // unconditional assignment in sysInitDev9 always overwrote it before use. Now that the load can
    // fail without latching, a stale 1 could outlive the reset and let sysShutdownDev9 try to power
    // off a DEV9 that was never brought back up.
    dev9Initialized = 0;
    dev9Loaded = 0;
    dev9InitCount = 0;
    while (!SifIopSync())
        ;

    SifInitRpc(0);
    SifSetCmdBuffer(OplSifCmdbuffer, OPL_SIF_CMD_BUFF_SIZE);
    sceCdInit(SCECdINoD);

    // init loadfile & iopheap services
    SifLoadFileInit();
    SifInitIopHeap();

    // apply sbv patches
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();

    if (sysLoadModuleLock < 0) {
        sysLoadModuleLock = sbCreateSemaphore();
    }

    // clears modules list
    memset((void *)&g_sysLoadedModBuffer[0], 0, MAX_MODULES * 4);

    // load modules
    LOG("[IOMANX]:\n");
    sysLoadModuleBuffer(&iomanx_irx, size_iomanx_irx, 0, NULL);
    LOG("[FILEXIO]:\n");
    sysLoadModuleBuffer(&filexio_irx, size_filexio_irx, 0, NULL);

    LOG("[SIO2MAN]:\n");
    sysLoadModuleBuffer(&sio2man_irx, size_sio2man_irx, 0, NULL);

    if (modload_mask & SYS_LOAD_MC_MODULES) {
        LOG("[MCMAN]:\n");
        sysLoadModuleBuffer(&mcman_irx, size_mcman_irx, 0, NULL);
        LOG("[MCSERV]:\n");
        sysLoadModuleBuffer(&mcserv_irx, size_mcserv_irx, 0, NULL);
    }

    LOG("[PADMAN]:\n");
    sysLoadModuleBuffer(&padman_irx, size_padman_irx, 0, NULL);

    LOG("[POWEROFF]:\n");
    sysLoadModuleBuffer(&poweroff_irx, size_poweroff_irx, 0, NULL);

    /*
      usbd is the USB HOST STACK, not a storage driver: it must be resident before ANY usbd client
      loads, and it stays unconditional (independent of gEnableUSB, which gates only usbmass_bd).
      The PADEMU block ~20 lines below loads ds34usb.irx/ds34bt.irx, and both hard-import usbd
      (each module's iop/imports.lst carries a usbd_IMPORTS_start block). Without usbd resident their
      loadcore link stage fails, neither registers its SIF RPC server, and ds34usb_init()'s unbounded
      "do { SifBindRpc } while (!server)" spin never terminates -- sysReset() never returns, init()
      never runs, and the console black-screens before the GS is even initialised.

      This lives HERE, matching fork master (src/system.c, after POWEROFF), NOT in bdmLoadModules().
      Official OPL loads usbd inside bdmsupport.c and steps 01..11 followed official; rebuild-12 then
      landed the FORK's bdmsupport.c byte-identically -- and the fork's copy has no usbd load, because
      the fork does it right here. Both halves of that contract now match the fork. Do not move it
      back into bdmLoadModules() without also removing the fork-parity claim on bdmsupport.c.
    */
    LOG("[USBD]:\n");
    sysLoadModuleBuffer(&usbd_irx, size_usbd_irx, 0, NULL);

    if (modload_mask & SYS_LOAD_ISOFS_MODULE) {
        LOG("[ISOFS]:\n");
        sysLoadModuleBuffer(&isofs_irx, size_isofs_irx, 0, NULL);
    }

    LOG("[GENVMC]:\n");
    sysLoadModuleBuffer(&genvmc_irx, size_genvmc_irx, 0, NULL);

    LOG("[LIBSD]:\n");
    sysLoadModuleBuffer(&libsd_irx, size_libsd_irx, 0, NULL);
    LOG("[AUDSRV]:\n");
    sysLoadModuleBuffer(&audsrv_irx, size_audsrv_irx, 0, NULL);

#ifdef PADEMU
    int ds3pads = 1; // only one pad enabled

    ds34usb_deinit();
    ds34bt_deinit();

    if (modload_mask & SYS_LOAD_USB_MODULES) {
        LOG("[DS34_USB]:\n");
        sysLoadModuleBuffer(&ds34usb_irx, size_ds34usb_irx, 4, (char *)&ds3pads);
        LOG("[DS34_BT]:\n");
        sysLoadModuleBuffer(&ds34bt_irx, size_ds34bt_irx, 4, (char *)&ds3pads);

        ds34usb_init();
        ds34bt_init();
    }
#endif

    fileXioInit();
    poweroffInit();
    poweroffSetCallback(&poweroffHandler, NULL);
}

static void poweroffHandler(void *arg)
{
    sysPowerOff();
}

void sysPowerOff(void)
{
    deinit(NO_EXCEPTION, IO_MODE_SELECTED_NONE);
    poweroffShutdown();
}

static unsigned int crctab[0x400];

unsigned int USBA_crc32(const char *string)
{
    int crc, table, count;
    unsigned char byte; // MUST be unsigned: a signed char sign-extends bytes >= 0x80 and the XOR
                        // below then produces a negative crctab index (OOB read)

    for (table = 0; table < 256; table++) {
        crc = table << 24;

        for (count = 8; count > 0; count--) {
            if (crc < 0)
                crc = crc << 1;
            else
                crc = (crc << 1) ^ 0x04C11DB7;
        }
        crctab[255 - table] = crc;
    }

    do {
        byte = string[count++];
        crc = crctab[byte ^ ((crc >> 24) & 0xFF)] ^ ((crc << 8) & 0xFFFFFF00);
    } while ((string[count - 1] != 0) && (count <= 32));

    return crc;
}

int sysGetDiscID(char *hexDiscID)
{
    u8 key[16];

    if (sceCdStatus() == SCECdErOPENS) // If tray is open, error
        return -1;

    {
        int detecting = 0;
        while (sceCdGetDiskType() == SCECdDETCT && detecting++ < 50)
            DelayThread(20 * 1000); // ~1 s cap, yields (issue #465: bound DETCT, preserve NODISC budget)
    }
    if (sceCdGetDiskType() == SCECdNODISC)
        return -1;

    sceCdDiskReady(0);
    LOG("SYSTEM Disc drive is ready\n");
    int cdmode = sceCdGetDiskType();
    if (cdmode == SCECdNODISC)
        return -1;

    if ((cdmode != SCECdPS2DVD) && (cdmode != SCECdPS2CD) && (cdmode != SCECdPS2CDDA)) {
        sceCdStop();
        sceCdSync(0);
        LOG("SYSTEM Disc stopped, Disc is not ps2 disc!\n");
        return -2;
    }

    LOG("SYSTEM Disc standby\n");
    sceCdStandby();
    sceCdSync(0);

    LOG("SYSTEM Disc read key\n");
    if (sceCdReadKey(0, 0, 0x4b, key) == 0) {
        LOG("SYSTEM Cannot read CD/DVD key.\n");
        sceCdStop();
        sceCdSync(0);
        LOG("SYSTEM Disc stopped\n");
        return -3;
    }

    sceCdStop();

    // convert to hexadecimal string
    snprintf(hexDiscID, 15, "%02X %02X %02X %02X %02X", key[10], key[11], key[12], key[13], key[14]);
    LOG("SYSTEM PS2 Disc ID = %s\n", hexDiscID);

    sceCdSync(0);
    LOG("SYSTEM Disc stopped\n");

    return 1;
}

void sysExecExit(void)
{
    // Deinitialize without shutting down active devices.
    deinit(NO_EXCEPTION, IO_MODE_SELECTED_ALL);
    exit(0);
}

// Parse SYSTEM.CNF text for "BOOT2 = <path>" and copy the path into out. Returns 0 on success.
// Retail discs are tiny uppercase "KEY = value" files, but stay defensive: match the key
// case-insensitively, require the '=' on the SAME line right after the key (a naive strchr
// could walk across a newline on a malformed file and grab the wrong line's value), and keep
// scanning past substring hits so a "BOOT2" inside another value can't shadow the real key.
static int sysParseBoot2(const char *cnf, char *out, int outSize)
{
    const char *p = cnf;

    if (outSize <= 0)
        return -1;

    while (*p != '\0') {
        if (strncasecmp(p, "BOOT2", 5) == 0) {
            const char *eq = p + 5;
            while (*eq == ' ' || *eq == '\t')
                eq++;
            if (*eq == '=') {
                int i = 0;
                eq++;
                while (*eq == ' ' || *eq == '\t')
                    eq++;
                while (*eq != '\0' && *eq != '\r' && *eq != '\n' && *eq != ' ' && *eq != '\t' && i < outSize - 1)
                    out[i++] = *eq++;
                out[i] = '\0';
                return (i > 0) ? 0 : -1;
            }
            p += 5;
        } else {
            p++;
        }
    }
    return -1;
}

// Boot the physical PS2 disc in the drive, always through rom0:PS2LOGO (which performs the disc
// region/auth check). PS2 discs only. Returns a negative code (and stays in OPL) on failure; on
// success it tears OPL down and never returns.
//
// The boot path comes from the disc's OWN SYSTEM.CNF "BOOT2" line -- full PS2BBL/OSDSYS parity.
// A sceCdReadKey(0x004B)-derived name that returns plausible-but-wrong bytes produces a
// nonexistent cdrom0: path, so PS2LOGO shows the logo (the DISC authenticates fine) and falls
// through to OSDSYS. The derivation is kept as the FALLBACK when SYSTEM.CNF is unreadable, and
// as a logged cross-check otherwise.
// One poll interval of a disc wait. With a progress callback the vsync inside it does the waiting
// (and keeps the menu animating); without one we fall back to a plain sleep.
static void sysDiscProbeWait(void (*progress)(void))
{
    if (progress != NULL)
        progress();
    else
        DelayThread(20 * 1000);
}

// Wait out a DETCT ("still identifying") report, bounded by WALL CLOCK rather than by a poll count.
static int sysDiscSettle(void (*progress)(void), int budgetMs)
{
    clock_t deadline = clock() + (clock_t)budgetMs * (CLOCKS_PER_SEC / 1000);
    int type = sceCdGetDiskType();

    while (type == SCECdDETCT && clock() < deadline) {
        sysDiscProbeWait(progress);
        type = sceCdGetDiskType();
    }
    return type;
}

int sysGetDiscBootPath(char *path, int pathSize, void (*progress)(void))
{
    u8 key[16];
    char boot[16], cnf[1024];
    u32 k32;
    int type, fd, len;

    if (path == NULL || pathSize < 64)
        return -3;

    if (sceCdStatus() == SCECdErOPENS) // tray open
        return -1;

    type = sysDiscSettle(progress, SYS_DISC_DETECT_MS);
    // An idle drive spins the disc DOWN and then reports NODISC even with a game disc loaded.
    // The software equivalent of a tray open/close is a TRAY-CLOSE REQUEST on the already-closed
    // tray, which re-runs the detect cycle. A genuinely empty drive still ends at the -2 bail
    // below, just ~4 s later.
    if (type == SCECdNODISC) {
        int spin = 0;
        clock_t deadline;
        u32 traychk = 0;
        int tr = sceCdTrayReq(SCECdTrayClose, &traychk);
        (void)tr; // only read by LOG (a no-op in release builds)
        LOG("[DISC] drive reports NODISC -- tray-close re-detect: req=%d traychk=%lu\n", tr, (unsigned long)traychk);
        sceCdSync(0);

        /* ONE budget for the whole re-detect, not one per poll.

           This used to nest a 1 s DETCT wait inside a 20-iteration loop whose comment claimed a
           "~4 s budget" -- true only if DETCT never appeared. A drive that keeps answering DETCT
           (dirty laser, marginal disc, and routinely an EMPTY tray) turned that into 20 x 1.2 s,
           so selecting Launch Disc with no disc in the tray sat there for up to 24 seconds with a
           frozen UI. That is issue #465's "extended read/poll loop", and the nesting was the whole
           of it: each layer was individually bounded, and the PRODUCT was not. */
        deadline = clock() + (clock_t)SYS_DISC_REDETECT_MS * (CLOCKS_PER_SEC / 1000);
        clock_t nodiscDeadline = clock() + (clock_t)SYS_DISC_NODISC_MS * (CLOCKS_PER_SEC / 1000);
        int sawDetct = 0;
        while (clock() < deadline) {
            spin++;
            type = sceCdGetDiskType();
            if (type != SCECdNODISC && type != SCECdDETCT)
                break;
            if (type == SCECdDETCT)
                sawDetct = 1; // the drive is working on something -- it has earned the full budget
            // Nothing to identify: unbroken NODISC past the shorter cap is an empty tray, and the
            // remaining budget buys only dead time. See SYS_DISC_NODISC_MS.
            if (!sawDetct && clock() >= nodiscDeadline)
                break;
            sysDiscProbeWait(progress);
        }
        LOG("[DISC] after tray-close re-detect: type=%d (%d re-polls, sawDetct=%d)\n", type, spin, sawDetct);
    }

    if (type != SCECdPS2DVD && type != SCECdPS2CD) // no disc / not a PS2 game disc
        return -2;

    sceCdDiskReady(0);

    // Primary: the disc's SYSTEM.CNF BOOT2 line (what OSDSYS itself boots).
    path[0] = '\0';
    fd = open("cdrom0:\\SYSTEM.CNF;1", O_RDONLY);
    if (fd >= 0) {
        len = read(fd, cnf, sizeof(cnf) - 1);
        close(fd);
        if (len > 0) {
            cnf[len] = '\0';
            if (sysParseBoot2(cnf, path, pathSize) != 0)
                path[0] = '\0';
        }
    }

    // Fallback + cross-check: derive the boot name from the disc key (PS2BBL PS2GetBootFile,
    // non-China path). Best-effort: if BOOT2 was parsed, a key failure no longer matters.
    boot[0] = '\0';
    if (sceCdReadKey(0, 0, 0x004B, key) != 0 && sceCdGetError() == 0) {
        boot[11] = '\0';
        k32 = (key[4] >> 3) | (key[14] >> 3 << 5) | ((key[0] & 0x7F) << 10);
        boot[10] = '0' + (k32 % 10);
        boot[9] = '0' + (k32 / 10 % 10);
        boot[8] = '.';
        boot[7] = '0' + (k32 / 10 / 10 % 10);
        boot[6] = '0' + (k32 / 10 / 10 / 10 % 10);
        boot[5] = '0' + (k32 / 10 / 10 / 10 / 10 % 10);
        boot[4] = '_';
        boot[3] = (key[0] >> 7) | ((key[1] & 0x3F) << 1);
        boot[2] = (key[1] >> 6) | ((key[2] & 0x1F) << 2);
        boot[1] = (key[2] >> 5) | ((key[3] & 0xF) << 3);
        boot[0] = ((key[4] & 0x7) << 4) | (key[3] >> 4);
        if (boot[0] < 'A' || boot[0] > 'Z') // sanity: a real boot name starts with a letter
            boot[0] = '\0';
    }

    if (path[0] == '\0') {
        if (boot[0] == '\0') // neither source produced a boot path
            return -3;
        snprintf(path, pathSize, "cdrom0:\\%s;1", boot);
        LOG("[DISC] SYSTEM.CNF unavailable; using key-derived name\n");
    } else if (boot[0] != '\0' && strstr(path, boot) == NULL) {
        // BOOT2 wins (it is what the browser boots); the mismatch is diagnostic.
        LOG("[DISC] key-derived name %s does not match BOOT2 %s -- booting BOOT2\n", boot, path);
    }

    return 0;
}

int sysLaunchDisc(void (*progress)(void))
{
    char path[64];
    char *args[1];
    int result;
#ifdef RETROACHIEVEMENTS
    if (discCheckBusy())
        return -1;
#endif
    result = sysGetDiscBootPath(path, sizeof(path), progress);
    if (result < 0)
        return result;

    LOG("[DISC] booting %s\n", path);

    // Tell the MMCE which game is about to boot (#183). EVERY other launch path already does this --
    // only Launch Disc was missing it. Without this, physical disc launches on a console with a
    // PSX MemCard Gen2 / MMCE never switched to the game's folder and the user simply could not save
    // or load from their card.
    // Disc paths are "cdrom0:\SLUS_123.45;1" (see path[] above) -- strip the prefix and semicolon so
    // we pass the pure id. A naive strchr(';') would also match the ";1" if there is NO slash
    // (a leading slash the MMCE would never match) instead of a clean id. Take whichever separator
    // comes last.
    {
        const char *sep = strrchr(path, '\\');
        const char *fwd = strrchr(path, '/');
        const char *colon = strrchr(path, ':');
        if (fwd > sep)
            sep = fwd;
        if (colon > sep)
            sep = colon;
        const char *idStart = (sep != NULL) ? sep + 1 : path;
        char gameid[32];
        int i = 0;
        while (idStart[i] != '\0' && idStart[i] != ';' && i < (int)sizeof(gameid) - 1) {
            gameid[i] = idStart[i];
            i++;
        }
        gameid[i] = '\0';
        if (gameid[0] != '\0') {
            LOG("[DISC] sending game id '%s' to MMCE\n", gameid);
            mmceSendGameID(gameid, NULL, 0);
        }
    }

    deinit(NO_EXCEPTION, IO_MODE_SELECTED_ALL); // tear OPL down (mirrors sysExecExit)

    args[0] = path;
    LoadExecPS2("rom0:PS2LOGO", 1, args); // logo performs the disc region/auth check
    return 0;                             // unreachable on success
}

// Module bits
#define CORE_IRX_USB    0x01
#define CORE_IRX_ETH    0x02
#define CORE_IRX_SMB    0x04
#define CORE_IRX_HDD    0x08
#define CORE_IRX_VMC    0x10
#define CORE_IRX_DEBUG  0x20
#define CORE_IRX_DECI2  0x40
#define CORE_IRX_ILINK  0x80
#define CORE_IRX_MX4SIO 0x100
#define CORE_IRX_MMCE   0x200

typedef struct
{
    char *game;
    char *mode;
    void *module;
    int *module_size;
} patchlist_t;

// Blank string for mode = all modes.
static const patchlist_t iop_patch_list[] = {
    {"SLUS_205.61", "", &iremsndpatch_irx, &size_iremsndpatch_irx},     // Disaster Report
    {"SLES_513.01", "", &iremsndpatch_irx, &size_iremsndpatch_irx},     // SOS: The Final Escape
    {"SLPS_251.13", "", &iremsndpatch_irx, &size_iremsndpatch_irx},     // Zettai Zetsumei Toshi
    {"SLES_535.08", "", &apemodpatch_irx, &size_apemodpatch_irx},       // Ultimate Pro Pinball
    {"SLUS_204.13", "", &f2techioppatch_irx, &size_f2techioppatch_irx}, // Shadow Man: 2econd Coming (NTSC-U/C)
    {"SLES_504.46", "", &f2techioppatch_irx, &size_f2techioppatch_irx}, // Shadow Man: 2econd Coming (PAL)
    {"SLES_506.08", "", &f2techioppatch_irx, &size_f2techioppatch_irx}, // Shadow Man: 2econd Coming (PAL German)
    {NULL, NULL, NULL, NULL},                                           // Terminator
};

static unsigned int addIopPatch(const char *mode_str, const char *startup, irxptr_t *tab)
{
    const patchlist_t *p;
    int i;

    for (i = 0; iop_patch_list[i].game != NULL; i++) {
        p = &iop_patch_list[i];

        if (!strcmp(p->game, startup) && (p->mode[0] == '\0' || !strcmp(p->mode, mode_str))) { // was strcmp(p->mode, startup): a mode-specific patch row could never match
            tab->info = (*(p->module_size)) | SET_OPL_MOD_ID(OPL_MODULE_ID_IOP_PATCH);
            tab->ptr = (void *)p->module;
            return 1;
        }
    }

    return 0;
}

typedef struct
{
    char *game;
    void *addr;
} modStorageSetting_t;

static const modStorageSetting_t mod_storage_location_list[] = {
    {"SLUS_209.77", (void *)0x01fc7000}, // Virtua Quest
    {"SLPM_656.32", (void *)0x01fc7000}, // Virtua Fighter Cyber Generation: Judgment Six No Yabou
    {NULL, NULL},                        // Terminator
};

static void *GetModStorageLocation(const char *startup, unsigned compatFlags)
{
    const modStorageSetting_t *p;
    int i;

    for (i = 0; mod_storage_location_list[i].game != NULL; i++) {
        p = &mod_storage_location_list[i];

        if (!strcmp(p->game, startup)) {
            return p->addr;
        }
    }

    return ((void *)OPL_MOD_STORAGE);
}

static unsigned int sendIrxKernelRAM(const char *startup, const char *mode_str, unsigned int modules, void *ModuleStorage, int size_cdvdman_irx, void **cdvdman_irx, int size_mcemu_irx, void **mcemu_irx)
{ // Send IOP modules that core must use to Kernel RAM
    irxtab_t *irxtable;
    irxptr_t *irxptr_tab;
    void *irxptr, *ioprp_image;
    int i, modcount;
    unsigned int curIrxSize, size_ioprp_image, total_size;

#ifdef RETROACHIEVEMENTS
    if (!strcmp(mode_str, "DISC_MODE")) {
        // No emulated storage driver for the physical drive.
    } else
#endif
        if (!strcmp(mode_str, "BDM_USB_MODE"))
        modules |= CORE_IRX_USB;
    else if (!strcmp(mode_str, "BDM_ILK_MODE"))
        modules |= CORE_IRX_ILINK;
    else if (!strcmp(mode_str, "BDM_M4S_MODE"))
        modules |= CORE_IRX_MX4SIO;
    else if (!strcmp(mode_str, "BDM_ATA_MODE"))
        modules |= CORE_IRX_HDD;
    else if (!strcmp(mode_str, "ETH_MODE"))
        modules |= CORE_IRX_ETH | CORE_IRX_SMB;
    else if (!strcmp(mode_str, "HTTP_MODE"))
        modules |= CORE_IRX_ETH;
    else if (!strcmp(mode_str, "MMCE_MODE"))
        modules |= CORE_IRX_MMCE;
    else
        modules |= CORE_IRX_HDD;

#ifdef RETROACHIEVEMENTS
    /*
      RetroAchievements telemetry leaves the console over the network whatever the
      game was booted from, so the in-game SMAP + SMSTCPIP pair has to travel in
      the module table even for a USB launch.

      DEVIATION from upstream, which sets this unconditionally. It is not free:
      the two modules cost roughly 57 KB of the module storage area
      (OPL_MOD_STORAGE, include/iosupport.h) and put SMAP on the NIC in every
      game. Gated on the watch list, a RA build launching an untracked game
      reserves and loads exactly what the default build does.

      GetWatchCount() is authoritative here because the launch legs have already
      run sbLoadWatchList() by this point; no list means nothing to send.
    */
    if (gRATelemetry && GetWatchCount() > 0)
        modules |= CORE_IRX_ETH;
#endif

    irxtable = (irxtab_t *)ModuleStorage;
    irxptr_tab = (irxptr_t *)((unsigned char *)irxtable + sizeof(irxtab_t));
    size_ioprp_image = size_IOPRP_img + size_cdvdman_irx + size_cdvdfsv_irx + size_eesync_irx + 256;
#ifdef RETROACHIEVEMENTS
    if (!strcmp(mode_str, "DISC_MODE"))
        size_ioprp_image = patch_IOPRP_image_disc_size();
#endif
    LOG("IOPRP image size calculated: %d\n", size_ioprp_image);
    ioprp_image = malloc(size_ioprp_image);
    if (ioprp_image == NULL) {
        // Avoid patch_IOPRP_image writing through a NULL pointer; an OOM here fails the launch
        // regardless.
        LOG("IOPRP image allocation failed (%d bytes)\n", size_ioprp_image);
        size_ioprp_image = 0;
    } else {
#ifdef RETROACHIEVEMENTS
        if (!strcmp(mode_str, "DISC_MODE"))
            size_ioprp_image = patch_IOPRP_image_disc(ioprp_image);
        else
#endif
            size_ioprp_image = patch_IOPRP_image(ioprp_image, cdvdman_irx, size_cdvdman_irx);
    }
    LOG("IOPRP image size actual:     %d\n", size_ioprp_image);

    modcount = 0;
    // Basic modules
    irxptr_tab[modcount].info = size_udnl_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_UDNL);
    irxptr_tab[modcount++].ptr = (void *)&udnl_irx;
    irxptr_tab[modcount].info = size_ioprp_image | SET_OPL_MOD_ID(OPL_MODULE_ID_IOPRP);
    irxptr_tab[modcount++].ptr = ioprp_image;
    irxptr_tab[modcount].info = size_imgdrv_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_IMGDRV);
    irxptr_tab[modcount++].ptr = (void *)&imgdrv_irx;
    irxptr_tab[modcount].info = size_resetspu_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_RESETSPU);
    irxptr_tab[modcount++].ptr = (void *)&resetspu_irx;

#ifdef PADEMU
#define PADEMU_ARG || gEnablePadEmu
#else
#define PADEMU_ARG
#endif
    if ((modules & CORE_IRX_USB) PADEMU_ARG) {
        irxptr_tab[modcount].info = size_usbd_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_USBD);
        irxptr_tab[modcount++].ptr = (void *)&usbd_irx;
    }
    if (modules & CORE_IRX_USB) {
        irxptr_tab[modcount].info = size_usbmass_bd_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_USBMASSBD);
        irxptr_tab[modcount++].ptr = (void *)&usbmass_bd_irx;
    }
    if (modules & CORE_IRX_ILINK) {
        irxptr_tab[modcount].info = size_iLinkman_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_ILINK);
        irxptr_tab[modcount++].ptr = (void *)&iLinkman_irx;
        irxptr_tab[modcount].info = size_IEEE1394_bd_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_ILINKBD);
        irxptr_tab[modcount++].ptr = (void *)&IEEE1394_bd_irx;
    }
    if (modules & CORE_IRX_MX4SIO) {
        // mx4sio_bd requires PS2SDK's sio2man after the game-side IOP reset. Without it the block
        // device never comes up in-game and the launch hangs. Keep these adjacent and ordered;
        // ee_core loads them in the same order.
        irxptr_tab[modcount].info = size_sio2man_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_SIO2MAN);
        irxptr_tab[modcount++].ptr = (void *)&sio2man_irx;
        irxptr_tab[modcount].info = size_mx4sio_bd_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_MX4SIOBD);
        irxptr_tab[modcount++].ptr = (void *)&mx4sio_bd_irx;
    }
#ifdef RETROACHIEVEMENTS
    if (!strcmp(mode_str, "DISC_MODE") && (modules & CORE_IRX_ETH)) {
        irxptr_tab[modcount].info = size_ps2dev9_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_DEV9);
        irxptr_tab[modcount++].ptr = (void *)&ps2dev9_irx;
        irxptr_tab[modcount].info = size_smsutils_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_SMSUTILS);
        irxptr_tab[modcount++].ptr = (void *)&smsutils_irx;
    }
#endif
    if (modules & CORE_IRX_ETH) {
        irxptr_tab[modcount].info = size_smap_ingame_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_SMAP);
        irxptr_tab[modcount++].ptr = (void *)&smap_ingame_irx;
        irxptr_tab[modcount].info = size_ingame_smstcpip_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_SMSTCPIP);
        irxptr_tab[modcount++].ptr = (void *)&ingame_smstcpip_irx;
    }
    if (modules & CORE_IRX_SMB) {
        irxptr_tab[modcount].info = size_smbinit_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_SMBINIT);
        irxptr_tab[modcount++].ptr = (void *)&smbinit_irx;
    }

#ifdef RETROACHIEVEMENTS
    // RetroAchievements telemetry sender. Registered only alongside the network
    // modules it imports from -- raudp links against SMAP's SMAPSendPacket, so
    // shipping it without CORE_IRX_ETH would just fail to load with -200.
    if (modules & CORE_IRX_ETH) {
        irxptr_tab[modcount].info = size_raudp_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_RAUDP);
        irxptr_tab[modcount++].ptr = (void *)&raudp_irx;
    }
#endif

    //Load MMCEIGR module (~1.4KB) on reset if bootcard switch is enabled for either slot
    if (gMMCEIGRSlot != 0) {
        irxptr_tab[modcount].info = size_mmceigr_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_MMCEIGR);
        irxptr_tab[modcount++].ptr = (void *)&mmceigr_irx;
    }

    if (modules & CORE_IRX_VMC) {
        irxptr_tab[modcount].info = size_mcemu_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_MCEMU);
        irxptr_tab[modcount++].ptr = (void *)mcemu_irx;
    }

    if ((modules & CORE_IRX_MMCE) || gMMCEIGRSlot != 0) {
        irxptr_tab[modcount].info = size_mmcedrv_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_MMCEDRV);
        irxptr_tab[modcount++].ptr = (void *)&mmcedrv_irx;
    }

#ifdef PADEMU
    if (gEnablePadEmu) {
        if (gPadEmuSettings & 0xFF) {
            irxptr_tab[modcount].info = size_bt_pademu_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_PADEMU);
            irxptr_tab[modcount++].ptr = (void *)&bt_pademu_irx;
        } else {
            irxptr_tab[modcount].info = size_usb_pademu_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_PADEMU);
            irxptr_tab[modcount++].ptr = (void *)&usb_pademu_irx;
        }
    }
#endif

#ifdef __INGAME_DEBUG
#ifdef __DECI2_DEBUG
    if (modules & CORE_IRX_DECI2) {
        irxptr_tab[modcount].info = size_drvtif_ingame_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_DRVTIF);
        irxptr_tab[modcount++].ptr = (void *)&drvtif_ingame_irx;
        irxptr_tab[modcount].info = size_tifinet_ingame_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_TIFINET);
        irxptr_tab[modcount++].ptr = (void *)&tifinet_ingame_irx;
    }
#elif defined(TTY_UDP)
    if (modules & CORE_IRX_DEBUG) {
        irxptr_tab[modcount].info = size_udptty_ingame_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_UDPTTY);
        irxptr_tab[modcount++].ptr = (void *)&udptty_ingame_irx;
        irxptr_tab[modcount].info = size_ioptrap_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_IOPTRAP);
        irxptr_tab[modcount++].ptr = (void *)&ioptrap_irx;
    }
#elif defined(TTY_PPC_UART)
    if (modules & CORE_IRX_DEBUG) {
        irxptr_tab[modcount].info = size_ppctty_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_PPCTTY);
        irxptr_tab[modcount++].ptr = (void *)&ppctty_irx;
        irxptr_tab[modcount].info = size_ioptrap_irx | SET_OPL_MOD_ID(OPL_MODULE_ID_IOPTRAP);
        irxptr_tab[modcount++].ptr = (void *)&ioptrap_irx;
    }
#endif
#endif

#ifdef RETROACHIEVEMENTS
    if (strcmp(mode_str, "DISC_MODE") != 0)
#endif
        modcount += addIopPatch(mode_str, startup, &irxptr_tab[modcount]);

    irxtable->modules = irxptr_tab;
    irxtable->count = modcount;

#ifdef __DECI2_DEBUG
    // For DECI2 debugging mode, the UDNL module will have to be stored within kernel RAM because there isn't enough space below user RAM.
    // total_size will hence not include the IOPRP image, but it's okay because the EE core is interested in protecting the module storage within user RAM.
    irxptr = (void *)0x00033000;
    LOG("SYSTEM DECI2 UDNL address start: %p end: %p\n", irxptr, (void *)((u8 *)irxptr + GET_OPL_MOD_SIZE(irxptr_tab[0].info)));
    DI();
    ee_kmode_enter();
    memcpy((void *)(0x80000000 | (unsigned int)irxptr), irxptr_tab[0].ptr, GET_OPL_MOD_SIZE(irxptr_tab[0].info));
    ee_kmode_exit();
    EI();

    irxptr_tab[0].ptr = irxptr; // UDNL is the first module.
#endif

    total_size = (sizeof(irxtab_t) + sizeof(irxptr_t) * modcount + 0xF) & ~0xF;
    irxptr = (void *)((((unsigned int)irxptr_tab + sizeof(irxptr_t) * modcount) + 0xF) & ~0xF);

#ifdef __DECI2_DEBUG
    for (i = 1; i < modcount; i++) {
#else
    for (i = 0; i < modcount; i++) {
#endif
        curIrxSize = GET_OPL_MOD_SIZE(irxptr_tab[i].info);

        if (curIrxSize > 0) {
            LOG("SYSTEM IRX %u address start: %p end: %p\n", GET_OPL_MOD_ID(irxptr_tab[i].info), irxptr, (void *)((u8 *)irxptr + curIrxSize));
            memcpy(irxptr, irxptr_tab[i].ptr, curIrxSize);

            irxptr_tab[i].ptr = irxptr;
            irxptr = (void *)((u8 *)irxptr + ((curIrxSize + 0xF) & ~0xF));
            total_size += ((curIrxSize + 0xF) & ~0xF);
        } else {
            irxptr_tab[i].ptr = NULL;
        }
    }

    free(ioprp_image);

    LOG("SYSTEM IRX STORAGE %p - %p\n", ModuleStorage, (u8 *)ModuleStorage + total_size);

    return total_size;
}

#ifdef __DECI2_DEBUG
/*
    Look for the start of the EE DECI2 manager initialization function.

    The stock EE kernel has no reset function, but the EE kernel is most likely already primed to self-destruct and in need of a good reset.
    What happens is that the OSD initializes the EE DECI2 TTY protocol at startup, but the EE DECI2 manager is never aware that the OSDSYS ever loads other programs.

    As a result, the EE kernel crashes immediately when the EE TTY gets used (when the IOP side of DECI2 comes up), when it invokes whatever that exists at the OSD's old ETTY handler's location. :(

    Must be run in kernel mode.
*/
static int ResetDECI2(void)
{
    int result;
    unsigned int i, *ptr;
    void (*pDeci2ManagerInit)(void);
    static const unsigned int Deci2ManagerInitPattern[] = {
        0x3c02bf80, // lui v0, $bf80
        0x3c04bfc0, // lui a0, $bfc0
        0x34423800, // ori v0, v0, $3800
        0x34840102  // ori a0, a0, $0102
    };

    result = -1;
    ptr = (void *)0x80000000;
    for (i = 0; i < 0x20000 / 4; i++) {
        if (ptr[i + 0] == Deci2ManagerInitPattern[0] &&
            ptr[i + 1] == Deci2ManagerInitPattern[1] &&
            ptr[i + 2] == Deci2ManagerInitPattern[2] &&
            ptr[i + 3] == Deci2ManagerInitPattern[3]) {
            pDeci2ManagerInit = (void *)&ptr[i - 14];
            pDeci2ManagerInit();
            result = 0;
            break;
        }
    }

    return result;
}

int sysInitDECI2(void)
{
    int result;

    DI();
    ee_kmode_enter();

    result = ResetDECI2();

    ee_kmode_exit();
    EI();

    return result;
}
#endif

/*  Returns the patch location of LoadExecPS2(), which resides in kernel memory.
 *  Patches the kernel to use the EELOAD module at the specified location.
 *  Must be run in kernel mode.
 */
static void *initLoadExecPS2(void *new_eeload)
{
    void *result;

    /* The pattern of the code in LoadExecPS2() that prepares the kernel for copying EELOAD from rom0: */
    static const unsigned int initEELOADCopyPattern[] = {
        0x8FA30010, /* lw       v1, 0x0010(sp) */
        0x0240302D, /* daddu    a2, s2, zero */
        0x8FA50014, /* lw       a1, 0x0014(sp) */
        0x8C67000C, /* lw       a3, 0x000C(v1) */
        0x18E00009, /* blez     a3, +9 <- The kernel will skip the EELOAD copying loop if the value in $a3 is less than, or equal to 0. Lets do that... */
    };

    u32 *p;

    result = NULL;
    /* Find the part of LoadExecPS2() that initilizes the EELOAD copying loop's variables */
    for (p = (u32 *)0x80001000; p < (u32 *)0x80030000; p++) {
        if (memcmp(p, &initEELOADCopyPattern, sizeof(initEELOADCopyPattern)) == 0) {
            p[1] = 0x3C120000 | (u16)((u32)new_eeload >> 16);    /* lui s2, HI16(new_eeload) */
            p[2] = 0x36520000 | (u16)((u32)new_eeload & 0xFFFF); /* ori s2, s2, LO16(new_eeload) */
            p[3] = 0x24070000;                                   /* li a3, 0 <- Disable the EELOAD copying loop */
            result = (void *)p;
            break; /* All done. */
        }
    }

    return result;
}

/*  Gets the address of the jump to the function that initializes user memory.
 *  Pathces the kernel to begin erasure of memory from the specified address.
 *  Must be run in kernel mode.
 */
static void *initInitializeUserMemory(void *start)
{
    u32 *p;
    void *result;

    result = NULL;
    for (p = (unsigned int *)0x80001000; p < (unsigned int *)0x80030000; p++) {
        /*
         * Search for function call and where $a0 is set.
         *  lui  $a0, 0x0008
         *  jal  InitializeUserMemory
         *  ori  $a0, $a0, 0x2000
         */
        if (p[0] == 0x3c040008 && (p[1] & 0xfc000000) == 0x0c000000 && p[2] == 0x34842000) {
            p[0] = 0x3c040000 | ((unsigned int)start >> 16);
            p[2] = 0x34840000 | ((unsigned int)start & 0xffff);
            result = (void *)p;
            break;
        }
    }

    return result;
}

static int initKernel(void *eeload, void *modStorageEnd, void **eeloadCopy, void **initUserMemory)
{
    DI();
    ee_kmode_enter();

#ifdef __DECI2_DEBUG
    ResetDECI2();
#endif
    *eeloadCopy = initLoadExecPS2(eeload);
    *initUserMemory = initInitializeUserMemory(modStorageEnd);

    ee_kmode_exit();
    EI();

    return ((*eeloadCopy != NULL && *initUserMemory != NULL) ? 0 : -1);
}

#ifdef __DEBUG
void sysPrintEECoreConfig(struct EECoreConfig_t *config)
{
    LOG("EECoreConfig Values = 0x%08X\n", (u32)config);

    LOG("Game Mode Desc = %s\n", config->GameModeDesc);
    LOG("EnableDebug = %d\n", config->EnableDebug);

    LOG("Exit Path = (%s)\n", config->ExitPath);
    LOG("HDD Spindown = %d\n", config->HDDSpindown);

    LOG("IP=%s NM=%s GW=%s mode: %d\n", config->g_ps2_ip, config->g_ps2_netmask, config->g_ps2_gateway, config->g_ps2_ETHOpMode);

    LOG("PS2RD Cheat Engine = %s\n", config->gCheatList == NULL ? "Disabled" : "Enabled");

    LOG("EnforceLanguage = %s\n", config->enforceLanguage == 0 ? "Disabled" : "Enabled");

    LOG("GSM = %s\n", config->EnableGSMOp == 0 ? "Disabled" : "Enabled");

    LOG("PADEMU = %s\n", config->EnablePadEmuOp == 0 ? "Disabled" : "Enabled");

    LOG("enforceLanguage = %s\n", config->enforceLanguage == 0 ? "Disabled" : "Enabled");

    LOG("eeloadCopy = 0x%08X\n", config->eeloadCopy);
    LOG("initUserMemory = 0x%08X\n", config->initUserMemory);

    LOG("ModStorageStart = 0x%08X\n", config->ModStorageStart);
    LOG("ModStorageEnd = 0x%08X\n", config->ModStorageEnd);

    LOG("GameID = %s\n", config->GameID);

    LOG("Compat Mask = 0x%02x\n", config->_CompatMask);
}
#endif

// --- Neutrino external-core launch (per-game $CoreLoader) --------------------
// Maps an OPL block-device driver token to Neutrino's -bsd backend name. Uses
// exact strcmp per token (NOT a strncmp prefix test): a prefix test matches
// "sdc" against "sd" first and mis-maps MX4SIO to ilink. Mirrors our hardened
// bdmDriverIs* matchers. Returns "unsupported" for anything
// Neutrino cannot back (e.g. eth/smb), which the caller treats as a hard stop.
static const char *getDeviceName(const char *driver)
{
    if (driver == NULL)
        return "unsupported";
    if (!strcmp(driver, "usb"))
        return "usb";
    if (!strcmp(driver, "ilink") || !strcmp(driver, "sd"))
        return "ilink";
    if (!strcmp(driver, "mx4sio") || !strcmp(driver, "sdc"))
        return "mx4sio";
    if (!strcmp(driver, "ata"))
        return "ata";
    if (!strcmp(driver, "apa"))
        return "apa";
    if (!strcmp(driver, "mmce"))
        return "mmce";
    if (!strcmp(driver, "udp"))
        // Both BLOCK transports register the "udp" BDM token; the RESIDENT protocol picks the Neutrino
        // backing store: -bsd=udpbd (smap_udpbd / SUDPBDv2) vs -bsd=udpfsbd (the udpfs_bd UDPRDMA toml).
        return (bdmGetLoadedNetProtocol() == NET_BOOT_UDPFS) ? "udpfsbd" : "udpbd";
    if (!strcmp(driver, "udpfs"))
        // UDPFS *filesystem* device (udpfssupport / udpfs_ioman): the stock Neutrino -bsd token loads
        // config/bsd-udpfs.toml (the FHI filesystem driver), then opens -dvd=udpfs:<path> by name -- no
        // massN: block device and no fraglist. Distinct from the "udp" block tokens above.
        return "udpfs";
    return "unsupported";
}

// Re-encodes the OPL compatmask into Neutrino's -gc concatenated-digit format.
// Forwards OPL bits 1,2,3,5 plus Mode 7 (COMPAT_MODE_7) -- a Neutrino-only flag with
// no OPL ee-core effect (greyed under the OPL core) that emits -gc=7 "fix game buffer
// overrun". result[] is sized off COMPAT_MODE_COUNT
// and every append is bounds-guarded so widening the set later cannot overflow.
// NOTE: Neutrino's -gc numbering is Neutrino's own, not OPL's bitmask semantics;
// the OPL<->Neutrino correspondence must be hardware-verified before widening.
static int convertCompatmaskToModes(int compatmask)
{
    char result[COMPAT_MODE_COUNT + 2];
    int pos = 0;

    if ((compatmask & COMPAT_MODE_1) && pos < (int)sizeof(result) - 1)
        result[pos++] = '1';
    if ((compatmask & COMPAT_MODE_2) && pos < (int)sizeof(result) - 1)
        result[pos++] = '2';
    if ((compatmask & COMPAT_MODE_3) && pos < (int)sizeof(result) - 1)
        result[pos++] = '3';
    if ((compatmask & COMPAT_MODE_5) && pos < (int)sizeof(result) - 1)
        result[pos++] = '5';
    if ((compatmask & COMPAT_MODE_7) && pos < (int)sizeof(result) - 1)
        result[pos++] = '7'; // Neutrino "IOP: fix game buffer overrun"; greyed under the OPL core

    result[pos] = '\0';
    return atoi(result);
}

// Split a whitespace-separated argument string into individual argv entries appended
// after the auto-built ones. `buf` receives a bounded mutable copy of `src`, and the
// tokens point into it, so `buf` must stay live until the argv is consumed. Returns
// the updated argc; never exceeds argvMax.
static int appendArgTokens(char **argv, int argc, int argvMax, char *buf, int bufSize, const char *src)
{
    if (src == NULL || src[0] == '\0')
        return argc;

    snprintf(buf, bufSize, "%s", src);

    char *tok = strtok(buf, " \t");
    while (tok != NULL && argc < argvMax) {
        // A leading '$' marks a user-disabled flag (NHDDL convention): keep it in the
        // args field for easy re-enabling, but don't forward it to Neutrino. Neutrino
        // flags are all '-'-prefixed, so '$' never collides with a real argument.
        if (tok[0] != '$')
            argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    return argc;
}

// Does `s` look like a retail boot-file name (AAAA_NNN.NN, e.g. SLUS_123.45)? Gates the opt-in
// -elf=cdrom0: emission below to startups neutrino can actually resolve a GameID from.
static int sysStartupShapeOk(const char *s)
{
    int i;
    if (s == NULL || strlen(s) != 11 || s[4] != '_' || s[8] != '.')
        return 0;
    for (i = 0; i < 4; i++)
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9')))
            return 0;
    for (i = 5; i <= 10; i++) {
        if (i == 8)
            continue;
        if (s[i] < '0' || s[i] > '9')
            return 0;
    }
    return 1;
}

// True if `args` contains `flag` as an ACTIVE token -- i.e. NOT $-disabled (the leading-$ convention
// that appendArgTokens strips at dispatch). Lets an auto-emitted flag stay suppressed only by a flag
// the user is actually forwarding, not by one they turned off with `$`.
// Does `args` contain an ACTIVE (non-$-disabled) occurrence of `flag` as its OWN token? `flag` is
// either an exact switch ("-dbc") or a key prefix ending in '=' ("-gsm="). A raw strstr is not
// enough: "-dbc" also occurs inside path-valued tokens like -elf=my-dbc-game.elf (PR #96 review),
// which must NOT count -- so a hit needs a token boundary on BOTH sides (start-of-string or
// whitespace before; end-of-string or whitespace after, unless the flag itself is a key prefix
// whose value legitimately continues). This also fixes the same latent false positive for -gsm=.
static int neutrinoArgHasActiveFlag(const char *args, const char *flag)
{
    if (args == NULL)
        return 0;
    size_t flen = strlen(flag);
    int keyPrefix = (flen > 0 && flag[flen - 1] == '=');
    const char *p = args;
    while ((p = strstr(p, flag)) != NULL) {
        int disabled = (p > args && *(p - 1) == '$');
        const char *tokStart = disabled ? p - 1 : p;
        int startOk = (tokStart == args) || (tokStart[-1] == ' ') || (tokStart[-1] == '\t');
        char after = p[flen];
        int endOk = keyPrefix || after == '\0' || after == ' ' || after == '\t';
        if (startOk && endOk && !disabled)
            return 1; // a whole-token, non-$-disabled occurrence -> the user is forwarding this flag
        p += flen;
    }
    return 0;
}

// Hand the game off to an external Neutrino ELF instead of OPL's embedded core.
// Hardened vs the wOPL original: NULL-guarded; an "unsupported" device aborts
// (never launches -bsd=unsupported); DISTINCT argv buffers (wOPL reused ONE
// buffer for both -bsd=ata and -bsdfs=hdl, clobbering -bsd=ata on HDD); argv[32]
// with per-append bounds guards (wOPL's argv[6] was exactly full). User-supplied
// flags (global gNeutrinoArgs + the per-game extraArgs) are tokenized and appended last.
// Rewrite the "ip=A.B.C.D" token inside <neutrino dir>/config/bsd-<device>.toml to the PS2's configured
// static IP. Neutrino's ministack takes its IP from that toml -- hardcoded 192.168.1.10 in the stock and
// bundled files -- NOT from anything OPL used while browsing, so any mismatch means the UDPFS server's
// discovery reply never routes back and the game black-screens after a perfectly healthy list. Δ6: now
// runs PRE-deinit via sysNeutrinoPreflight (every mount is up, the GUI is alive), so a hard failure can
// abort to the menu with a toast instead of dying invisibly post-teardown. Anchored to the stock `"ip=`
// quoting so a comment mentioning ip= can never be clobbered.
// Returns 0 = OK to launch (synced / already-in-sync / benign leave-alone), <0 = abort the launch:
// the toml is MISSING (neutrino cannot load its bsd config at all) or was mangled-then-restored (the
// old ip survives, so the boot would black-screen on any other subnet).
static int sysSyncNeutrinoUdpfsToml(const char *neutrinoPath, const char *deviceName)
{
    static char toml[4096]; // Δ6: raised from 2048 -- an annotated toml no longer gets skipped
    static char updated[4096 + 20];
    char tomlPath[288];
    char newIp[20];
    int fd, len;

    const char *slash = strrchr(neutrinoPath, '/');
    if (slash == NULL)
        return 0; // custom flat path -- no dir to anchor on; old hand-edit contract
    snprintf(tomlPath, sizeof(tomlPath), "%.*sconfig/bsd-%s.toml", (int)(slash - neutrinoPath) + 1, neutrinoPath, deviceName);

    fd = open(tomlPath, O_RDONLY);
    if (fd < 0) {
        LOG("[NEUTRINO] no %s -- neutrino cannot boot this transport without it\n", tomlPath);
        return -1; // missing bsd toml = guaranteed post-teardown failure -> abort while GUI is alive
    }
    len = read(fd, toml, sizeof(toml) - 1);
    close(fd);
    if (len <= 0)
        return -1; // unreadable/empty bsd toml -> same guaranteed failure
    if (len >= (int)sizeof(toml) - 1) {
        LOG("[NEUTRINO] %s larger than %d -- ip= left as-is\n", tomlPath, (int)sizeof(toml));
        return 0; // exotic hand-built toml: proceed on the old hand-edit contract
    }
    toml[len] = '\0';

    char *tok = strstr(toml, "\"ip=");
    if (tok == NULL) {
        LOG("[NEUTRINO] %s has no \"ip= token -- left as-is\n", tomlPath);
        return 0; // hand-restructured toml: the user owns the ip; proceed
    }
    char *valStart = tok + 4;
    char *valEnd = valStart;
    while (*valEnd == '.' || (*valEnd >= '0' && *valEnd <= '9'))
        valEnd++;

    snprintf(newIp, sizeof(newIp), "%d.%d.%d.%d", ps2_ip[0], ps2_ip[1], ps2_ip[2], ps2_ip[3]);
    if ((int)strlen(newIp) == (int)(valEnd - valStart) && !strncmp(valStart, newIp, strlen(newIp)))
        return 0; // already in sync -- don't touch the file

    snprintf(updated, sizeof(updated), "%.*s%s%s", (int)(valStart - toml), toml, newIp, valEnd);

    fd = open(tomlPath, O_WRONLY | O_TRUNC);
    if (fd < 0) {
        LOG("[NEUTRINO] cannot rewrite %s (%d) -- ip= left as-is\n", tomlPath, fd);
        return 0; // write-protected media: proceed on the old hand-edit contract
    }
    int expected = (int)strlen(updated);
    int written = write(fd, updated, expected);
    close(fd);
    if (written != expected) {
        // O_TRUNC already emptied the file; a short write would leave a mangled toml behind. Best
        // recovery without atomic-rename support across every PS2 filesystem: put the ORIGINAL
        // content back so the user is no worse off than before the sync (old hand-edit contract).
        LOG("[NEUTRINO] short write to %s (%d/%d) -- restoring original toml\n", tomlPath, written, expected);
        fd = open(tomlPath, O_WRONLY | O_TRUNC);
        if (fd >= 0) {
            write(fd, toml, len);
            close(fd);
        }
        return -1; // the OLD ip survives -> the boot would black-screen; abort while GUI is alive
    }
    LOG("[NEUTRINO] synced %s ip= -> %s\n", tomlPath, newIp);
    return 0;
}

// Δ6 (NHDDL parity): everything that can FAIL a Neutrino launch and is checkable pre-teardown runs
// HERE, called by every device leg BEFORE deinitEx -- the GUI is alive for a toast and nothing has
// been torn down, so a failure is "stay in the menu", not a post-teardown black screen. Returns
// 0 = proceed; <0 = abort (a toast was shown).
int sysNeutrinoPreflight(const char *driver, const char *neutrinoPath)
{
    if (driver == NULL || neutrinoPath == NULL)
        return -1;

    const char *deviceName = getDeviceName(driver);
    if (!strcmp(deviceName, "unsupported")) {
        LOG("[NEUTRINO] preflight: unsupported device '%s'\n", driver);
        guiWarning(_l(_STR_NEUTRINO_DEV_UNSUPPORTED), 6);
        return -1;
    }

    // Network transports: sync the bsd toml ip= NOW, while the toml's device is mounted and a
    // failure can still be reported (see sysSyncNeutrinoUdpfsToml for the abort conditions).
    if (!strcmp(deviceName, "udpfs") || !strcmp(deviceName, "udpfsbd") || !strcmp(deviceName, "udpbd")) {
        if (sysSyncNeutrinoUdpfsToml(neutrinoPath, deviceName) < 0) {
            guiWarning(_l(_STR_NEUTRINO_TOML_SYNC_FAILED), 6);
            return -1;
        }
    }
    return 0;
}

void sysLaunchNeutrino(const char *driver, const char *path, const char *startup, int compatmask, int EnablePS2Logo, const char *neutrinoPath, const char *extraArgs, int neutrinoVideo, int neutrinoGsmComp, int neutrinoBsdfs, const neutrino_vmc_args_t *vmcArgs)
{
    if (neutrinoPath == NULL || driver == NULL || path == NULL) {
        LOG("[NEUTRINO] null arg, abort\n");
        return;
    }

    const char *deviceName = getDeviceName(driver);
    if (!strcmp(deviceName, "unsupported")) {
        LOG("[NEUTRINO] unsupported device '%s', abort\n", driver);
        return;
    }

    // HW-confirmed (issue #56, AndrewBento, PSXMemCard Gen2): Neutrino's -logo on the mmce backend
    // black-screens GAME-DEPENDENTLY ("depends a bit on luck which game") -- and the identical
    // failure reproduces on NHDDL, so it is a Neutrino/mmce interaction, not this launcher. Every
    // affected game boots with the logo off, so the GLOBAL PS2-Logo toggle is suppressed for
    // mmce-hosted games until that is fixed upstream. Deliberate escape hatch: a user-supplied
    // "-logo" in the global/per-game Neutrino args still passes through the tokenizer untouched.
    if (EnablePS2Logo && !strcmp(driver, "mmce")) {
        LOG("[NEUTRINO] -logo suppressed on mmce (issue #56: game-dependent black screens, repro'd on NHDDL)\n");
        EnablePS2Logo = 0;
    }

    char bsd[16];
    char bsdfs[16];
    char filePath[288];        // "-dvd=hdl:" (9) + up to a 255-char path + NUL — avoid truncation (B2)
    char cwdArg[288];          // "-cwd=" + the neutrino.elf install dir (auto; Neutrino finds its config/modules there)
    char compatModes[32] = ""; // stays empty when no compat modes are forwarded (B1)
    char globalArgsBuf[256];   // mutable copy of gNeutrinoArgs for tokenizing (tokens point in)
    char extraArgsBuf[256];    // mutable copy of the per-game extraArgs for tokenizing
    char *argv[16];            // target argv[0] + auto args + tokenized user flags (kernel-budgeted)
    int argc = 0;
    // 14, NOT sizeof(argv): the kernel args area holds at most 15 strings per ExecPS2 hop (ps2sdk
    // exit.c SetArg, SETARG_MAX_ARGS 15 -- and ExecPS2 forwards the UNCLAMPED count, so overflowing
    // makes the kernel read string bytes as argv pointers). Hop 1 prepends the elfldr child's load
    // path, spending one slot, so the target argv must stay <= 14 entries.
    const int argvMax = 14;

    // Target argv[0] = neutrino.elf's own path (NHDDL convention). sysLoadELFKeepIOP forwards
    // argv verbatim -- argv[0] included -- so this must be supplied explicitly here (POPSTARTER
    // launches use the same loader with a selector as argv[0] instead).
    if (argc < argvMax)
        argv[argc++] = (char *)neutrinoPath;

    if (!strcmp(deviceName, "apa")) {
        snprintf(bsd, sizeof(bsd), "-bsd=ata");
        if (argc < argvMax)
            argv[argc++] = bsd;
        snprintf(bsdfs, sizeof(bsdfs), "-bsdfs=hdl");
        if (argc < argvMax)
            argv[argc++] = bsdfs;
        snprintf(filePath, sizeof(filePath), "-dvd=hdl:%s", path);
        if (argc < argvMax)
            argv[argc++] = filePath;
    } else {
        snprintf(bsd, sizeof(bsd), "-bsd=%s", deviceName);
        if (argc < argvMax)
            argv[argc++] = bsd;
        // Per-game -bsdfs override (parity-audit #11), Auto(0) = today's bytes exactly (no -bsdfs,
        // bare -dvd). Valid driver values are ONLY exfat/hdl/bd; the -dvd prefix changes in lockstep
        // (verified vs rickgaiser/neutrino main.c usage: "-bsdfs=hdl -dvd=hdl:..." and
        // "-bsdfs=bd -dvd=bdfs:..."; exfat is the default fs and takes the bare path unchanged --
        // any ':'-bearing -dvd value is file-mode, opened through the selected fs). NEVER emitted
        // for mmce/udpfs: they are fileid backends with no filesystem layer (Neutrino forces
        // sBSDFS="no" there regardless -- this guard just keeps the argv clean). A user-typed
        // -bsdfs= wins (skip the structured emit); a user-typed -dvd= also wins on its own because
        // user tokens are appended after these and Neutrino's arg parse is last-wins.
        static const char *const bsdfsTokens[] = {"", "exfat", "hdl", "bd"};
        static const char *const bsdfsDvdPrefix[] = {"", "", "hdl:", "bdfs:"};
        int fsOverride = 0;
        if (neutrinoBsdfs >= 1 && neutrinoBsdfs <= 3 &&
            strcmp(deviceName, "mmce") != 0 && strcmp(deviceName, "udpfs") != 0 &&
            !neutrinoArgHasActiveFlag(gNeutrinoArgs, "-bsdfs=") && !neutrinoArgHasActiveFlag(extraArgs, "-bsdfs="))
            fsOverride = neutrinoBsdfs;
        if (fsOverride) {
            snprintf(bsdfs, sizeof(bsdfs), "-bsdfs=%s", bsdfsTokens[fsOverride]);
            if (argc < argvMax)
                argv[argc++] = bsdfs;
        }
        snprintf(filePath, sizeof(filePath), "-dvd=%s%s", bsdfsDvdPrefix[fsOverride], path);
        if (argc < argvMax)
            argv[argc++] = filePath;

        /* QUICKBOOT IS THE SECOND HALF OF THE KEEP-IOP HANDOFF, and without it the first half is
           self-defeating on USB and iLink. elfldr deliberately does NOT reset the IOP, so Neutrino
           inherits OPL's live iomanX/fileXio/bdm/bdmfs_fatfs and its mounts -- that is the entire
           point. But Neutrino's DEFAULT boot then performs its own IOP reset, which destroys exactly
           what we just preserved, leaving it to re-initialise the block device itself before it can
           reach the ISO.

           Symptom when only the first half ships: the handoff completes (no OSDSYS -- the loader
           did its job) and then the screen stays black, because Neutrino reset away the stack the
           game path depends on. -qb keeps the inherited environment and skips that reset.

           USB AND ILINK ONLY, deliberately: both launch through this inherited BDM environment, and
           rev 2692 hardware showed the same reset-boundary symptom on iLink that established the USB
           exception (iLink leg still needs a retest to prove it was the whole cause). Other backends
           stay on the normal boot path until hardware says otherwise. A user-typed -qb in the global
           or per-game args wins -- do not emit a second copy.

           Emitted ABOVE coreArgc so the pool-fit drop loop can never shed it: a dropped -qb would
           silently reinstate the reset and reproduce the black screen with no way to tell why. */
        if ((!strcmp(deviceName, "usb") || !strcmp(deviceName, "ilink")) && argc < argvMax &&
            !neutrinoArgHasActiveFlag(gNeutrinoArgs, "-qb") && !neutrinoArgHasActiveFlag(extraArgs, "-qb"))
            argv[argc++] = "-qb";
    }

    // Everything up to and including -dvd/-qb is the boot-critical core: the pool-fit drop loop below
    // must never shed these. A fixed floor of 3 silently stopped covering -dvd once the optional
    // -bsdfs slots in front of it (argv[3]) -- record the real core count instead.
    const int coreArgc = argc;

    // Only forward -gc when at least one compat mode is set: Neutrino treats
    // -gc=0 as an explicit mode (IOP fast reads), NOT a no-op, so passing it for
    // a game with no OPL modes selected would force unwanted behavior (B1).
    int gcModes = convertCompatmaskToModes(compatmask);
    if (gcModes > 0) {
        snprintf(compatModes, sizeof(compatModes), "-gc=%d", gcModes);
        if (argc < argvMax)
            argv[argc++] = compatModes;
    }

    // Global toggles auto-emit -dbc/-logo; the per-game/global args screens now carry them as
    // structured bools too (forwarded via the tokenizer below), so suppress the auto-emit when the
    // user's args already hold an active (non-$-disabled) copy -- one flag, emitted once.
    if (gEnableDebug && argc < argvMax &&
        !neutrinoArgHasActiveFlag(gNeutrinoArgs, "-dbc") && !neutrinoArgHasActiveFlag(extraArgs, "-dbc"))
        argv[argc++] = "-dbc";

    if (EnablePS2Logo && argc < argvMax &&
        !neutrinoArgHasActiveFlag(gNeutrinoArgs, "-logo") && !neutrinoArgHasActiveFlag(extraArgs, "-logo"))
        argv[argc++] = "-logo";

    // Per-game Neutrino video mode (-gsm): 1=fp1 (240p), 2=fp2 (480p), 3=1080ix1 (1080i); 0/out-of-range
    // = no -gsm. Neutrino is LAST-wins on -gsm and ABORTS the boot on a malformed value, so emit exactly
    // ONE: skip the structured token when the user already typed a -gsm into the global or per-game args,
    // letting that explicit value win (precedence confirmed vs rickgaiser/neutrino ee/loader/src/main.c).
    int userHasGsm = neutrinoArgHasActiveFlag(gNeutrinoArgs, "-gsm=") ||
                     neutrinoArgHasActiveFlag(extraArgs, "-gsm=");
    if (neutrinoVideo >= 1 && neutrinoVideo <= 5 && !userHasGsm && argc < argvMax) {
        // -gsm=v:c grammar (neutrino main.c parse_gsm_flags): v in {fp1, fp2, 1080ix1, 1080ix2,
        // 1080ix3}, optional :c in {1,2,3} = field-flipping compatibility type. NEVER emit a bare
        // ":c" -- an unrecognized -gsm value aborts the whole neutrino boot -- so the comp half is
        // ignored unless a video mode is set (the GUI hint says as much).
        static const char *const gsmVideoTokens[] = {"", "fp1", "fp2", "1080ix1", "1080ix2", "1080ix3"};
        static char gsmArg[24]; // outlives argv[] until the ExecPS2 handoff below
        if (neutrinoGsmComp >= 1 && neutrinoGsmComp <= 3)
            snprintf(gsmArg, sizeof(gsmArg), "-gsm=%s:%d", gsmVideoTokens[neutrinoVideo], neutrinoGsmComp);
        else
            snprintf(gsmArg, sizeof(gsmArg), "-gsm=%s", gsmVideoTokens[neutrinoVideo]);
        argv[argc++] = gsmArg;
    }

    // Parity Delta-10, enabled by default via settings_riptopl.cfg "neutrino_elf_arg"=1
    // (deliberately no UI row): hand neutrino the boot ELF path directly so its per-GameID
    // config/<GameID>.toml compat lookup can resolve pre-reset. Shape-guarded to AAAA_NNN.NN
    // startups and skipped when the user already forwards an -elf= (structured field or free
    // text) -- neutrino must see exactly one.
    static char elfArg[40]; // outlives argv[] until the ExecPS2 handoff
    if (gNeutrinoElfArg && sysStartupShapeOk(startup) && argc < argvMax &&
        !neutrinoArgHasActiveFlag(gNeutrinoArgs, "-elf=") && !neutrinoArgHasActiveFlag(extraArgs, "-elf=")) {
        snprintf(elfArg, sizeof(elfArg), "-elf=cdrom0:\\%s;1", startup);
        argv[argc++] = elfArg;
        LOG("[NEUTRINO] neutrino_elf_arg=1: emitting %s\n", elfArg);
    }

    // VMC slots (#47): emit each configured "-mcN=...bin" as its OWN argv entry. These come from
    // sbBuildVmcNeutrinoArgs (caller storage, alive across the launch) and must NOT pass through the
    // whitespace tokenizer below -- a VMC name with a space would otherwise be split into two args
    // and Neutrino would receive a truncated, unopenable card path (silent no-mount).
    if (vmcArgs != NULL) {
        int slot;
        for (slot = 0; slot < NEUTRINO_VMC_SLOTS; slot++) {
            if (vmcArgs->arg[slot][0] != '\0' && argc < argvMax)
                argv[argc++] = (char *)vmcArgs->arg[slot];
        }
    }

    // Auto -cwd: point Neutrino at the directory holding neutrino.elf so it loads its config / extra
    // modules relative to its install location, UNLESS the user already supplied one (precedence like
    // -gsm above). Without it Neutrino launches with no working dir, so a setup that keeps config next
    // to the ELF would not be found. mc:NEUTRINO/neutrino.elf paths also contain a '/', so this works
    // for memory-card installs too.
    int userHasCwd = neutrinoArgHasActiveFlag(gNeutrinoArgs, "-cwd=") ||
                     neutrinoArgHasActiveFlag(extraArgs, "-cwd=");
    if (!userHasCwd) {
        const char *slash = strrchr(neutrinoPath, '/');
        if (slash != NULL) {
            int dirLen = (int)(slash - neutrinoPath) + 1; // keep the trailing '/'
            snprintf(cwdArg, sizeof(cwdArg), "-cwd=%.*s", dirLen, neutrinoPath);
            if (argc < argvMax)
                argv[argc++] = cwdArg;
        }
    }

    // Δ6: the bsd toml ip= sync (network transports) now runs PRE-deinit inside sysNeutrinoPreflight
    // -- called by every device leg before deinitEx -- so a failure aborts to a live menu with a
    // toast instead of dying invisibly here, post-teardown. Nothing to do at this point.

    // Append user-supplied Neutrino flags: global defaults first, then the per-game
    // string (so a game can extend the global set). Both are tokenized on whitespace.
    argc = appendArgTokens(argv, argc, argvMax, globalArgsBuf, sizeof(globalArgsBuf), gNeutrinoArgs);
    argc = appendArgTokens(argv, argc, argvMax, extraArgsBuf, sizeof(extraArgsBuf), extraArgs);

    // ExecPS2 argv BYTE budget (verified vs ps2sdk exit.c SetArg + the crt0 args struct): each hop
    // carries its strings in ONE 256-byte pool, every NUL included. Hop 1 packs the child's load
    // path PLUS this argv -- neutrinoPath is counted TWICE (loadpath and target argv[0]). SetArg's
    // copy is UNbounded, so exceeding the pool corrupts rather than truncates. Fit by dropping tail
    // args (user extras sit last; each drop LOGged); the boot-critical core (argv[0]/-bsd/
    // [-bsdfs]/-dvd, counted as coreArgc above) must survive or nothing can boot anyway -- past
    // that, refuse and let the LOG name the overage.
    {
        int pool = (int)strlen(neutrinoPath) + 1; // hop-1 child argv[0] = the load path
        int i;
        for (i = 0; i < argc; i++)
            pool += (int)strlen(argv[i]) + 1;
        while (pool > 256 && argc > coreArgc) {
            argc--;
            pool -= (int)strlen(argv[argc]) + 1;
            LOG("[NEUTRINO] argv pool over 256 bytes -- dropping tail arg: %s\n", argv[argc]);
        }
        if (pool > 256) {
            LOG("[NEUTRINO] argv pool %d bytes even at the core-args floor (256 max) -- refusing handoff\n", pool);
            return;
        }
    }

    // Log the FULL argv (not just bsd/dvd/compat) so the VMC -mc args are verifiable on hardware (#47).
    LOG("[NEUTRINO] elf=%s argc=%d\n", neutrinoPath, argc);
    {
        int i;
        for (i = 0; i < argc; i++)
            LOG("[NEUTRINO]   argv[%d]=%s\n", i, argv[i]);
    }

    // Hand off WITHOUT the elf-loader IOP reset (NHDDL parity, vendored elfldr/): Neutrino reads
    // its config/modules (-cwd) and opens the game ISO through OUR still-mounted devices, and only
    // then does its own IOP reset -- the old resetting handoff left it a bare SIO2MAN/MCMAN/MCSERV
    // IOP, so any USB/BDM-hosted neutrino.elf setup black-screened to OSDSYS. The callers keep both
    // the game device and the neutrino.elf device mounted (deinitEx).
    if (sysLoadELFKeepIOP(neutrinoPath, "", argc, argv) < 0)
        LOG("[NEUTRINO] keep-IOP handoff failed for %s\n", neutrinoPath);
}

// Hand off to an external POPSTARTER.ELF to boot a PS1 VCD. The caller resolves the per-device
// POPSTARTER.ELF path + builds the argv[0] selector ("<POPS>/<XX.|SB.><name>.ELF", which
// POPSTARTER itself re-derives the matching .VCD from), copies both to stack buffers, and
// deinit()s the owning device with UNMOUNT_EXCEPTION BEFORE calling this -- the same contract as
// sysLaunchNeutrino (so the VCD-holding device stays mounted across the IOP reset).
void sysLaunchPopstarter(const char *popstarterElf, const char *selector)
{
    if (popstarterElf == NULL || selector == NULL) {
        LOG("[POPS] null arg, abort\n");
        return;
    }

    char *argv[1];
    argv[0] = (char *)selector;
    LOG("[POPS] elf=%s argv0=%s\n", popstarterElf, selector);

    // Every POPSTARTER route depends on the selector remaining target argv[0]. The stock SDK
    // partition loader replaces it with the ELF load path; use the argv-preserving loader
    // uniformly. POPSTARTER performs its own IOP reset and device discovery from the selector.
    if (sysLoadELFKeepIOP(popstarterElf, "", 1, argv) < 0)
        LOG("[POPS] keep-IOP handoff failed for %s\n", popstarterElf);
}

// Hand off to an external ember.elf to boot a PS1 CUE title. Contract in include/system.h.
//
// The keep-IOP loader is MANDATORY here, not a preference. ember.elf carries sceSifInitRpc,
// SifLoadModule and SifInitIopHeap but NO SifIopReset / SifIopRebootBuffer / sceSifRebootIop
// (verified against the shipped Beta binary, which is unstripped) -- it cannot rebuild a device
// stack and simply inherits whatever the launcher leaves running. Ember's own bundled launcher.elf
// resets the IOP and then loads iomanX + fileXio + usbd + usbmass_bd + bdm + bdmfs_fatfs before
// ExecPS2'ing into it; OPL's live IOP is a superset of that, which is why this handoff reaches
// every device OPL can mount rather than USB alone. Using the resetting SDK loader instead leaves
// Ember with no drivers and no way to make any -- a black screen, not a degraded launch.
//
// argv[0] must be ember.elf's real path: ps2sdk's __init_cwd derives the target's working
// directory from it, and Ember resolves bios.bin, settings.txt and games/<name> against that.
// sysLoadELFKeepIOP forwards argv verbatim (argv[0] included), the same property the POPSTARTER
// selector relies on.
void sysLaunchEmber(const char *emberElf, const char *gameFolder)
{
    char *argv[2];
    int argc = 0;

    if (emberElf == NULL) {
        LOG("[EMBER] null arg, abort\n");
        return;
    }

    argv[argc++] = (char *)emberElf;

    // No game argument is a legitimate call, not a caller bug: Ember then resolves games/default
    // and, failing that, runs the PS1 BIOS shell. Both are useful answers when probing a setup.
    if (gameFolder != NULL && gameFolder[0] != '\0')
        argv[argc++] = (char *)gameFolder;

    LOG("[EMBER] elf=%s game=%s\n", emberElf, (argc > 1) ? argv[1] : "(none)");

    // The kernel argv budget (15 strings / 256-byte pool, counting the child loader's own argv[0])
    // is enforced inside sysLoadELFKeepIOP, which refuses rather than truncating. Callers keep the
    // game name under CUE_NAME_LAUNCH_MAX so a refusal here means a pathological device path.
    if (sysLoadELFKeepIOP(emberElf, "", argc, argv) < 0)
        LOG("[EMBER] keep-IOP handoff failed for %s\n", emberElf);
}

void sysLaunchLoaderElf(const char *filename, const char *mode_str, int size_cdvdman_irx, void **cdvdman_irx, int size_mcemu_irx, void **mcemu_irx, int EnablePS2Logo, unsigned int compatflags)
{
    unsigned int modules, ModuleStorageSize;
    void *ModuleStorage, *ModuleStorageEnd;
    u8 local_ip_address[4], local_netmask[4], local_gateway[4];
    u8 *boot_elf = NULL;
    elf_header_t *eh;
    elf_pheader_t *eph;
    void *pdata;
    int argc, i;
    char ElfPath[32];
    char *argv[4];
    void *eeloadCopy, *initUserMemory;
    struct GsmConfig_t gsm_config;

    ethGetNetConfig(local_ip_address, local_netmask, local_gateway);
#if (!defined(__DEBUG) && !defined(_DTL_T10000))
    AddHistoryRecordUsingFullPath(filename);
#endif

    if (gExitPath[0] == '\0')
        strncpy(gExitPath, "Browser", sizeof(gExitPath));

    // Disable sound effects via libsd, to prevent some games with improper initialization from inadvertently using digital effect settings from other software.
    LOG("[CLEAREFFECTS]:\n");
    sysLoadModuleBuffer(&cleareffects_irx, size_cleareffects_irx, 0, NULL);

    // Wipe the low user memory region, since this region might not be wiped after OPL's EE core is installed.
    // Start wiping from 0x00084000 instead (as the HDD Browser does), as the alarm patch is installed at 0x00082000.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
    memset((void *)0x00084000, 0, 0x00100000 - 0x00084000);
#pragma GCC diagnostic pop

    modules = 0;
    ModuleStorage = GetModStorageLocation(filename, compatflags);

#ifdef __DECI2_DEBUG
    modules |= CORE_IRX_DECI2 | CORE_IRX_ETH;
#elif defined(__INGAME_DEBUG)
    modules |= CORE_IRX_DEBUG;
#ifdef TTY_UDP
    modules |= CORE_IRX_ETH;
#endif
#endif

#ifdef RETROACHIEVEMENTS
    const int discMode = !strcmp(mode_str, "DISC_MODE");
#else
    const int discMode = 0;
#endif
    if (!discMode)
        modules |= CORE_IRX_VMC;

    LOG("SYSTEM LaunchLoaderElf loading modules\n");
    ModuleStorageSize = (sendIrxKernelRAM(filename, mode_str, modules, ModuleStorage, size_cdvdman_irx, cdvdman_irx, size_mcemu_irx, mcemu_irx) + 0x3F) & ~0x3F;

    ModuleStorageEnd = (void *)((u8 *)ModuleStorage + ModuleStorageSize);

    // NB: LOADER.ELF is embedded
    boot_elf = (u8 *)&eecore_elf;
    eh = (elf_header_t *)boot_elf;
    eph = (elf_pheader_t *)(boot_elf + eh->phoff);

    ApplyDeckardXParam(filename);

    // Get the kernel to use our EELOAD module and to begin erasure after module storage. EE core will erase any memory before the module storage (if any).
    if (initKernel((void *)eh->entry, ModuleStorageEnd, &eeloadCopy, &initUserMemory) != 0) { // Should not happen, but...
        LOG("Error - kernel is unsupported.\n");
        asm volatile("break\n");
    }
    ConfigParam PARAM;
    GetOsdConfigParam(&PARAM);
    if (!discMode && gOSDLanguageEnable) { // only patch if enabled, and only on config fields wich have not chosen "system default"
        if (gOSDLanguageValue >= LANGUAGE_JAPANESE && gOSDLanguageValue <= LANGUAGE_PORTUGUESE) {
            PARAM.language = gOSDLanguageValue;
            LOG("System Language enforced to %d\n", gOSDLanguageValue);
        }
        if (gOSDTVAspectRatio >= TV_SCREEN_43 && gOSDTVAspectRatio <= TV_SCREEN_169) {
            PARAM.screenType = gOSDTVAspectRatio;
            LOG("System screenType enforced to %d\n", gOSDTVAspectRatio);
        }
        if (gOSDVideOutput == VIDEO_OUTPUT_RGB || gOSDVideOutput == VIDEO_OUTPUT_COMPONENT) {
            PARAM.videoOutput = gOSDVideOutput;
            LOG("System video output enforced to %d\n", gOSDVideOutput);
        }
    }

    struct EECoreConfig_t *config = NULL;
    u32 *core_ptr = (u32 *)&eecore_elf;

    for (i = 0; i < size_eecore_elf / 4; i++) {
        if (core_ptr[0] == EE_CORE_MAGIC_0 && core_ptr[1] == EE_CORE_MAGIC_1) {
            config = (struct EECoreConfig_t *)core_ptr;
            break;
        }
        core_ptr++;
    }

    if (config == NULL) { // Should not happen, but...
        LOG("Error - EE core config MAGIC not found!\n");
        asm volatile("break\n");
    }

    memset(config, 0, sizeof(struct EECoreConfig_t));

    config->magic[0] = EE_CORE_MAGIC_0;
    config->magic[1] = EE_CORE_MAGIC_1;

    strncpy(config->ExitPath, gExitPath, CORE_EXIT_PATH_MAX_LEN);
    strncpy(config->GameModeDesc, mode_str, CORE_GAME_MODE_DESC_MAX_LEN);

    config->EnableDebug = gEnableDebug;
    config->HDDSpindown = gHDDSpindown;
    config->g_ps2_ETHOpMode = gETHOpMode;

    if (!discMode && GetCheatsEnabled()) {
        set_cheats_list();
        config->gCheatList = GetCheatsList();
    } else
        config->gCheatList = NULL;

    // PS2RD .img cheats (item 26). Without this the EE loads the .img into RAM and then never hands
    // the pointer to the core, so LinkImage() had nothing to link and the whole feature was dead.
    config->gImage = !discMode && GetImageEnabled() ? (u32 *)GetImage() : NULL;

#ifdef RETROACHIEVEMENTS
    // RetroAchievements: the watch list for this game, already loaded by the launch
    // leg. It may legitimately be absent -- the game simply is not tracked -- in
    // which case ee_core sees NULL and never starts the telemetry path.
    config->raWatchList = GetWatchList();
    config->raWatchCount = GetWatchCount();
    config->raSnapBytes = GetWatchBytes();

    // The last point where the list is still ours: from here it goes into ee_core
    // with no feedback. A zero shows up in the launch log directly, rather than as
    // empty packets on the PC.
    raLaunchNote("to-ee-core", config->raWatchCount, config->raSnapBytes);
#endif

    sprintf(config->g_ps2_ip, "%u.%u.%u.%u", local_ip_address[0], local_ip_address[1], local_ip_address[2], local_ip_address[3]);
    sprintf(config->g_ps2_netmask, "%u.%u.%u.%u", local_netmask[0], local_netmask[1], local_netmask[2], local_netmask[3]);
    sprintf(config->g_ps2_gateway, "%u.%u.%u.%u", local_gateway[0], local_gateway[1], local_gateway[2], local_gateway[3]);

    // GSM now.
    config->EnableGSMOp = !discMode && GetGSMEnabled();
    if (config->EnableGSMOp) {
        PrepareGSM(NULL, &gsm_config);
        config->GsmConfig.interlace = gsm_config.interlace;
        config->GsmConfig.mode = gsm_config.mode;
        config->GsmConfig.ffmd = gsm_config.ffmd;
        config->GsmConfig.display = gsm_config.display;
        config->GsmConfig.syncv = gsm_config.syncv;
        config->GsmConfig.smode2 = gsm_config.smode2;
        config->GsmConfig.dx_offset = gsm_config.dx_offset;
        config->GsmConfig.dy_offset = gsm_config.dy_offset;
        config->GsmConfig.k576P_fix = gsm_config.k576P_fix;
        config->GsmConfig.kGsDxDyOffsetSupported = gsm_config.kGsDxDyOffsetSupported;
        config->GsmConfig.FIELD_fix = gsm_config.FIELD_fix;
    }

#ifdef PADEMU
    config->EnablePadEmuOp = !discMode && gEnablePadEmu;
    config->PadEmuSettings = (unsigned int)(gPadEmuSettings >> 8);
    config->PadMacroSettings = (unsigned int)(gPadMacroSettings);
#endif

    config->CustomOSDConfigParam.spdifMode = PARAM.spdifMode;
    config->CustomOSDConfigParam.screenType = PARAM.screenType;
    config->CustomOSDConfigParam.videoOutput = PARAM.videoOutput;
    config->CustomOSDConfigParam.japLanguage = PARAM.japLanguage;
    config->CustomOSDConfigParam.ps1drvConfig = PARAM.ps1drvConfig;
    config->CustomOSDConfigParam.version = PARAM.version;
    config->CustomOSDConfigParam.language = PARAM.language;
    config->CustomOSDConfigParam.timezoneOffset = PARAM.timezoneOffset;

    config->enforceLanguage = !discMode && gOSDLanguageEnable;

    config->eeloadCopy = eeloadCopy;
    config->initUserMemory = initUserMemory;

    config->ModStorageStart = ModuleStorage;
    config->ModStorageEnd = ModuleStorageEnd;

    strncpy(config->GameID, filename, CORE_GAME_ID_MAX_LEN);

    config->_CompatMask = compatflags;

    //MMCEIGR Settings
    config->MMCEIGRSettings = gMMCEIGRSlot;

#ifdef __DEBUG

    sysPrintEECoreConfig(config);

#endif
    // Scan through the ELF's program headers and copy them into RAM, then
    // zero out any non-loaded regions.

    FlushCache(0);
    FlushCache(2);

    for (i = 0; i < eh->phnum; i++) {
        if (eph[i].type != ELF_PT_LOAD)
            continue;

        pdata = (void *)((u8 *)boot_elf + eph[i].offset);
        memcpy(eph[i].vaddr, pdata, eph[i].filesz);
        LOG("EECORE PH COPY: %d 0x%08X 0x%08X 0x%08X\n", i, (u32)eph[i].vaddr, (u32)pdata, eph[i].filesz);

        if (eph[i].memsz > eph[i].filesz) {
            LOG("EECORE PH CLEAR: %d 0x%08X 0x%08X\n", i, (u32)((u32)eph[i].vaddr + eph[i].filesz), eph[i].memsz - eph[i].filesz);
            memset(eph[i].vaddr + eph[i].filesz, 0, eph[i].memsz - eph[i].filesz);
        }
    }

    argc = 0;

    // PS2LOGO Caller, based on l_oliveira & SP193 tips
    // Don't call LoadExecPS2 here because it will wipe all memory above the EE core, making it impossible to pass data via pointers.
    if (EnablePS2Logo) { // Not all roms have PS2LOGO
        int fd = 0;
        if ((fd = open("rom0:PS2LOGO", O_RDONLY)) >= 0) {
            close(fd);
            argv[argc] = "rom0:PS2LOGO";
            argc++;
        }
    }

    snprintf(ElfPath, sizeof(ElfPath), "cdrom0:\\%s;1", filename);
    argv[argc] = ElfPath;
    argc++;

#ifdef __DEBUG
    LOG("Starting ee_core with following argv arguments:\n");
    for (i = 0; i < argc; i++) {
        LOG("[%d] %s\n", i, argv[i]);
    }
#endif

    LOG("Leaving OPL GUI, starting eecore = 0x%08X \n", (u32)eh->entry);

    // Let's go.
    fileXioExit();
    SifExitRpc();

    FlushCache(0);
    FlushCache(2);

    ExecPS2((void *)eh->entry, NULL, argc, argv);
}

int sysCheckMC(void)
{
    DIR *mc0_root_dir = opendir("mc0:/");
    if (mc0_root_dir != NULL) {
        closedir(mc0_root_dir);
        return 0;
    }

    DIR *mc1_root_dir = opendir("mc1:/");
    if (mc1_root_dir != NULL) {
        closedir(mc1_root_dir);
        return 1;
    }

    return -11;
}

// Full path of the VMC whose creation was last STARTED, for sysVMCContiguity() to re-open once
// genvmc reports the job finished. Recorded on the create path only: the VMC dialog re-probes with
// createSize == 0 on every keystroke and deletes with -1, and either would otherwise clobber the
// path the completion check needs. Creation is modal and one-at-a-time -- the same assumption
// genvmc's own single-slot status devctl already makes -- so one slot is enough.
static char gVMCCreatePath[256] = {0};

// Did the VMC just created land as ONE contiguous chain? mcemu needs that, and genvmc does not
// defragment, so on a used volume a fresh VMC can land in pieces -- which the user otherwise only
// learns mid-launch, as an error about a card they thought they had just made.
//
// 1 contiguous, 0 FRAGMENTED, -1 unknown. GET_LBA is probed first because bdmfs_fatfs answers
// CHECK_CHAIN with a bare 1/0 and never an error, so a store that cannot do LBA lookups at all
// (mmce, pfs, SMB, udpfs) would otherwise return a 0 meaning only "not my ioctl".
int sysVMCContiguity(void)
{
    u64 startingLBA;
    int fd, iop_fd, chain;

    if (gVMCCreatePath[0] == '\0')
        return -1;

    fd = open(gVMCCreatePath, O_RDONLY);
    if (fd < 0)
        return -1;

    iop_fd = ps2sdk_get_iop_fd(fd);
    if (fileXioIoctl2(iop_fd, USBMASS_IOCTL_GET_LBA, NULL, 0, &startingLBA, sizeof(startingLBA)) != 0) {
        close(fd);
        return -1; // not a fatfs-backed device: CHECK_CHAIN below would be meaningless
    }

    chain = fileXioIoctl(iop_fd, USBMASS_IOCTL_CHECK_CHAIN, "");
    close(fd);

    LOG("SYSTEM VMC contiguity check on %s: chain=%d\n", gVMCCreatePath, chain);
    if (chain == 1)
        return 1;
    if (chain == 0)
        return 0;
    return -1; // anything else is the ioctl itself failing, not a verdict about the file
}

// createSize == -1 : delete, createSize == 0 : probing, createSize > 0 : creation
int sysCheckVMC(const char *prefix, const char *sep, char *name, int createSize, vmc_superblock_t *vmc_superblock)
{
    int size = -1;
    char path[256];
    snprintf(path, sizeof(path), "%sVMC%s%s.bin", prefix, sep, name);

    // Invalidate the completion probe up front on any create request. The creation below is
    // CONDITIONAL -- a file already at the requested size is left alone -- while the GUI arms its
    // check before knowing that. Without this, a skipped creation would leave the path of some
    // EARLIER VMC in place and the probe would answer about the wrong file. Cleared here and set
    // only where genvmc is actually invoked, so "no creation" reads as unknown and stays silent.
    if (createSize > 0)
        gVMCCreatePath[0] = '\0';

    if (createSize == -1)
        unlink(path);
    else {
        int fd = open(path, O_RDONLY, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP | S_IROTH | S_IWOTH | S_IXOTH);
        if (fd >= 0) {
            size = lseek(fd, 0, SEEK_END);

            if (vmc_superblock) {
                memset(vmc_superblock, 0, sizeof(vmc_superblock_t));
                lseek(fd, 0, SEEK_SET);
                read(fd, (void *)vmc_superblock, sizeof(vmc_superblock_t));

                LOG("SYSTEM File size  : 0x%X\n", size);
                LOG("SYSTEM Magic      : %s\n", vmc_superblock->magic);
                LOG("SYSTEM Card type  : %d\n", vmc_superblock->mc_type);
                LOG("SYSTEM Flags      : 0x%X\n", (vmc_superblock->mc_flag & 0xFF) | 0x100);
                LOG("SYSTEM Page_size  : 0x%X\n", vmc_superblock->page_size);
                LOG("SYSTEM Block_size : 0x%X\n", vmc_superblock->pages_per_block);
                LOG("SYSTEM Card_size  : 0x%X\n", vmc_superblock->pages_per_cluster * vmc_superblock->clusters_per_card);

                if (!strncmp(vmc_superblock->magic, "Sony PS2 Memory Card Format", 27) && vmc_superblock->mc_type == 0x2 && size == vmc_superblock->pages_per_cluster * vmc_superblock->clusters_per_card * vmc_superblock->page_size) {
                    LOG("SYSTEM VMC file structure valid: %s\n", path);
                } else
                    size = 0;
            }

            if (size % 1048576) // invalid size, should be a an integer (8, 16, 32, 64, ...)
                size = 0;
            else
                size /= 1048576;

            close(fd);

            if (createSize && (createSize != size))
                unlink(path);
        }


        if (createSize && (createSize != size)) {
            createVMCparam_t createParam;
            // Remember what is being built so the GUI can ask about its layout when genvmc finishes.
            snprintf(gVMCCreatePath, sizeof(gVMCCreatePath), "%s", path);
            strcpy(createParam.VMC_filename, path);
            createParam.VMC_size_mb = createSize;
            createParam.VMC_blocksize = 16;
            createParam.VMC_thread_priority = 0x0f;
            createParam.VMC_card_slot = -1;
            fileXioDevctl("genvmc:", 0xC0DE0001, (void *)&createParam, sizeof(createParam), NULL, 0);
        }
    }
    return size;
}
