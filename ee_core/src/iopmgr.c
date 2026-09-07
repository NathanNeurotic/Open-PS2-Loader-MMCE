/*
  Copyright 2009-2010, Ifcaro, jimmikaelkael & Polo
  Copyright 2006-2008 Polo
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.

  Some parts of the code are taken from HD Project by Polo
*/

#include <iopcontrol.h>

#include "ee_core.h"
#include "iopmgr.h"
#include "modules.h"
#include "modmgr.h"
#include "util.h"
#include "syshook.h"
#include "coreconfig.h"
#ifdef RETROACHIEVEMENTS
#include "ra_overlay.h"
#include "../../modules/network/common/ra_snap.h"
#endif

extern int _iop_reboot_count;

#ifdef RETROACHIEVEMENTS
/* Snapshot buffer in IOP RAM. The EE allocates it so it knows the address and
   can DMA straight into it without touching the SIF command table, which the
   game shares -- a command number is an index into a table the game owns, and
   sceSifAddCmdHandler returns void, so overrunning it fails silently.
   Zero means the allocation failed. Read by ra.c. */
unsigned int ra_snap_iop = 0;

/* Is there anything to send? The EE leaves raWatchList NULL when the user has
   telemetry switched off or the game has no watch list (see src/system.c), so
   this one test covers both.

   DEVIATION from upstream, which brings the network up in every mode
   unconditionally. That costs roughly 57 KB of the module storage region and
   puts SMAP on the NIC in every game launched from the RA build, whether or not
   a single achievement is being tracked. Gated, a RA build with nothing to
   watch loads exactly what the default build loads. */
static int RA_TelemetryWanted(struct EECoreConfig_t *config)
{
    return config->raWatchList != NULL && config->raWatchCount > 0 && config->raSnapBytes > 0;
}

/* Eight hex digits; there is no sprintf in ee_core. The buffer addresses travel
   to raudp as a load-argument string. */
static void RA_Hex32(unsigned int v, char *out)
{
    int i;

    for (i = 7; i >= 0; i--) {
        out[i] = "0123456789ABCDEF"[v & 0xF];
        v >>= 4;
    }
    out[8] = '\0';
}
#endif

static int imgdrv_offset_ioprpimg = 0;
static int imgdrv_offset_ioprpsiz = 0;

static void ResetIopSpecial(const char *args, unsigned int arglen)
{
    USE_LOCAL_EECORE_CONFIG;
    int i;
    void *pIOP_buffer, *IOPRP_img, *imgdrv_irx;
    unsigned int length_rounded, CommandLen, size_IOPRP_img, size_imgdrv_irx;
    char command[RESET_ARG_MAX + 1];

    if (arglen > 0) {
        strncpy(command, args, arglen);
        command[arglen] = '\0'; /* In a normal IOP reset process, the IOP reset command line will be NULL-terminated properly somewhere.
                        Since we're now taking things into our own hands, NULL terminate it here.
                        Some games like SOCOM3 will use a command line that isn't NULL terminated, resulting in things like "cdrom0:\RUN\IRX\DNAS300.IMGG;1" */
        _strcpy(&command[arglen + 1], "host0:");
        CommandLen = arglen + 7;
    } else {
        _strcpy(command, "host0:");
        CommandLen = 6;
    }

    GetOPLModInfo(OPL_MODULE_ID_IOPRP, &IOPRP_img, &size_IOPRP_img);
    GetOPLModInfo(OPL_MODULE_ID_IMGDRV, &imgdrv_irx, &size_imgdrv_irx);

    length_rounded = (size_IOPRP_img + 0xF) & ~0xF;
    pIOP_buffer = SifAllocIopHeap(length_rounded);

    CopyToIop(IOPRP_img, length_rounded, pIOP_buffer);

    if (imgdrv_offset_ioprpimg == 0 || imgdrv_offset_ioprpsiz == 0) {
        for (i = 0; i < size_imgdrv_irx; i += 4) {
            if (*(u32 *)((&((unsigned char *)imgdrv_irx)[i])) == 0xDEC1DEC1) {
                imgdrv_offset_ioprpimg = i;
            }
            if (*(u32 *)((&((unsigned char *)imgdrv_irx)[i])) == 0xDEC2DEC2) {
                imgdrv_offset_ioprpsiz = i;
            }
        }
    }

    *(void **)(UNCACHED_SEG(&((unsigned char *)imgdrv_irx)[imgdrv_offset_ioprpimg])) = pIOP_buffer;
    *(u32 *)(UNCACHED_SEG(&((unsigned char *)imgdrv_irx)[imgdrv_offset_ioprpsiz])) = size_IOPRP_img;

    LoadMemModule(0, imgdrv_irx, size_imgdrv_irx, 0, NULL);

    DIntr();
    ee_kmode_enter();
    Old_SifSetReg(SIF_REG_SMFLAG, SIF_STAT_BOOTEND);
    ee_kmode_exit();
    EIntr();

    LoadOPLModule(OPL_MODULE_ID_UDNL, SIF_RPC_M_NOWAIT, CommandLen, command);

    DIntr();
    ee_kmode_enter();
    Old_SifSetReg(SIF_REG_SMFLAG, SIF_STAT_SIFINIT);
    Old_SifSetReg(SIF_REG_SMFLAG, SIF_STAT_CMDINIT);
    Old_SifSetReg(SIF_SYSREG_RPCINIT, 0);
    Old_SifSetReg(SIF_SYSREG_SUBADDR, (int)NULL);
    ee_kmode_exit();
    EIntr();

    LoadFileExit(); // OPL's integrated LOADFILE RPC does not automatically unbind itself after IOP resets.

    _iop_reboot_count++; // increment reboot counter to allow RPC clients to detect unbinding!

    while (!SifIopSync()) {
        ;
    }

    SifInitRpc(0);
    SifInitIopHeap();
    LoadFileInit();
    sbv_patch_enable_lmb();

    DPRINTF("Loading extra IOP modules...\n");

#ifdef __LOAD_DEBUG_MODULES
#if !defined(TTY_PPC)
    LoadOPLModule(OPL_MODULE_ID_SMSTCPIP, 0, 0, NULL);
    LoadOPLModule(OPL_MODULE_ID_SMAP, 0, g_ipconfig_len, g_ipconfig);
#endif
#ifdef __DECI2_DEBUG
    LoadOPLModule(OPL_MODULE_ID_DRVTIF, 0, 0, NULL);
    LoadOPLModule(OPL_MODULE_ID_TIFINET, 0, 0, NULL);
#elif defined(TTY_UDP)
    LoadOPLModule(OPL_MODULE_ID_UDPTTY, 0, 0, NULL);
    LoadOPLModule(OPL_MODULE_ID_IOPTRAP, 0, 0, NULL);
#elif defined(TTY_PPC)
    LoadOPLModule(OPL_MODULE_ID_PPCTTY, 0, 0, NULL);
    LoadOPLModule(OPL_MODULE_ID_IOPTRAP, 0, 0, NULL);
#endif
#endif

#ifdef PADEMU
#define PADEMU_ARG || config->EnablePadEmuOp
#else
#define PADEMU_ARG
#endif
    if (config->GameMode == BDM_USB_MODE PADEMU_ARG) {
        LoadOPLModule(OPL_MODULE_ID_USBD, 0, 11, "thpri=2,3");
    }

#if defined(RETROACHIEVEMENTS) && !defined(__LOAD_DEBUG_MODULES)
    /* RetroAchievements telemetry needs the network in every mode, including
       games running from USB: raudp imports SMAPSendPacket from the SMAP
       driver, so without SMAP it fails to link and nothing is sent. ETH mode
       loads both modules in its own branch below; every other mode loads them
       here, and only when there is actually a watch list to stream. */
    if (config->GameMode != ETH_MODE && config->GameMode != HTTP_MODE && RA_TelemetryWanted(config)) {
        LoadOPLModule(OPL_MODULE_ID_SMSTCPIP, 0, 0, NULL);
        LoadOPLModule(OPL_MODULE_ID_SMAP, 0, g_ipconfig_len, g_ipconfig);
    }
#endif

    switch (config->GameMode) {
        case BDM_USB_MODE:
            LoadOPLModule(OPL_MODULE_ID_USBMASSBD, 0, 0, NULL);
            break;
        case ETH_MODE:
#ifndef __LOAD_DEBUG_MODULES
            LoadOPLModule(OPL_MODULE_ID_SMSTCPIP, 0, 0, NULL);
            LoadOPLModule(OPL_MODULE_ID_SMAP, 0, g_ipconfig_len, g_ipconfig);
#endif
            LoadOPLModule(OPL_MODULE_ID_SMBINIT, 0, 0, NULL);
            break;
        case HTTP_MODE:
#ifndef __LOAD_DEBUG_MODULES
            LoadOPLModule(OPL_MODULE_ID_SMSTCPIP, 0, 0, NULL);
            LoadOPLModule(OPL_MODULE_ID_SMAP, 0, g_ipconfig_len, g_ipconfig);
#endif
            break;
        case HDD_MODE:
            break;
        case BDM_ILK_MODE:
            // IEEE1394_bd requires iLinkman, exactly as mx4sio_bd requires sio2man below. Do not
            // start it if that dependency failed: an IEEE1394_bd with no manager under it never
            // brings the block device up, and cdvdman then waits on a device that will never
            // arrive -- a hang with nothing on screen to explain it.
            //
            // Every other transport already checks this. The MX4SIO case below checks it, and the
            // menu-side loader in bdmsupport.c checks it FOR iLink (it gates IEEE1394_BD on
            // iLinkManModLoaded). This game-side path was the only one that did not, so a
            // post-reset iLinkman failure was silent here and loud everywhere else.
            if (LoadOPLModule(OPL_MODULE_ID_ILINK, 0, 0, NULL) > 0)
                LoadOPLModule(OPL_MODULE_ID_ILINKBD, 0, 0, NULL);
            break;
        case BDM_M4S_MODE:
            // mx4sio_bd requires PS2SDK's sio2man. Do not start it if that dependency failed.
            if (LoadOPLModule(OPL_MODULE_ID_SIO2MAN, 0, 0, NULL) > 0)
                LoadOPLModule(OPL_MODULE_ID_MX4SIOBD, 0, 0, NULL);
            break;
        case BDM_HDD_MODE:
            break;
        case MMCE_MODE:
            LoadOPLModule(OPL_MODULE_ID_MMCEDRV, 0, 0, NULL);
            break;
    };

#ifdef RETROACHIEVEMENTS
    /* RetroAchievements telemetry, loaded LAST because it imports
       SMAPSendPacket from the SMAP driver above. The snapshot buffer is
       allocated here, while the IOP heap is up and the game has not started,
       and its address handed to the module as a load argument -- RA_SNAP_TOTAL
       covers the header plus the values of the largest supported watch list. */
    if (RA_TelemetryWanted(config)) {
        void *snap = SifAllocIopHeap(RA_SNAP_TOTAL);

        if (snap != NULL) {
            /* argv[1]: the IOP snapshot, EE event and EE badge buffer
               addresses, eight hex digits each; then whether raudp may read
               from the network in play, then the game's serial.
               argv[2]: SMAP's ipconfig strings -- raudp finds the PC itself. */
            char args[45 + IPCONFIG_MAX_LEN];
            int k, n;

            ra_snap_iop = (unsigned int)snap;
            RA_Hex32(ra_snap_iop, &args[0]);
            args[8] = ',';
            RA_Hex32((unsigned int)RA_OverlayEventBuffer(), &args[9]);
            args[17] = ',';
            RA_Hex32((unsigned int)RA_OverlayBadgeBuffer(), &args[18]);

            /* Both roads raudp takes to the PC cost the game something when it
               streams its own disc over this NIC: the raw one frees the SMAP
               receive descriptors that stream arrives in, the lwIP one queues
               on the mailbox the SMB or HTTP client waits on. A game from a
               share loads for ever with either, so tell raudp to stop looking.
               Sending is unaffected and stays on. HTTP_MODE is ours -- upstream
               has no HTTP protocol -- but it streams down the same path and
               carries the same defect. */
            args[26] = ',';
            args[27] = (config->GameMode == ETH_MODE || config->GameMode == HTTP_MODE) ? '0' : '1';

            /* The serial seeds the packet header, so the PC has it before the
               first snapshot is written. */
            args[28] = ',';
            for (n = 0; n < 15 && config->GameID[n] != '\0'; n++)
                args[29 + n] = config->GameID[n];
            args[29 + n] = '\0';
            n += 30;

            for (k = 0; k < g_ipconfig_len && k < IPCONFIG_MAX_LEN; k++)
                args[n + k] = g_ipconfig[k];

            LoadOPLModule(OPL_MODULE_ID_RAUDP, 0, n + k, args);
        } else {
            /* No buffer, so nothing can be sent. Leaving ra_snap_iop at zero
               makes ra.c's per-frame path a no-op, and there is no reason to
               start the module just to have it idle. */
            ra_snap_iop = 0;
        }
    }
#endif
}

/*----------------------------------------------------------------*/
/* Reset IOP to include our modules.                              */
/*----------------------------------------------------------------*/
int New_Reset_Iop(const char *arg, int arglen)
{
    USE_LOCAL_EECORE_CONFIG;
    DPRINTF("New_Reset_Iop start!\n");
    if (EnableDebug)
        DBGCOL(0xFF00FF, IOPMGR, "New_Reset_Iop()");

    SifInitRpc(0);

    iop_reboot_count++;

    // Reseting IOP.
    while (!Reset_Iop("", 0)) {
        ;
    }
    while (!SifIopSync()) {
        ;
    }

    SifInitRpc(0);
    SifInitIopHeap();
    LoadFileInit();
    sbv_patch_enable_lmb();

    ResetIopSpecial(NULL, 0);
    if (EnableDebug)
        DBGCOL(0x00A5FF, IOPMGR, "ResetIopSpecial (without args) finished!");

    if (arglen > 0) {
        ResetIopSpecial(&arg[10], arglen - 10);
        if (EnableDebug)
            DBGCOL(0x00FFFF, IOPMGR, "ResetIopSpecial (with args) finished!");
    }

    if (iop_reboot_count >= 2) {
#ifdef PADEMU
        config->PadEmuSettings |= (LoadOPLModule(OPL_MODULE_ID_MCEMU, 0, 0, NULL) > 0) << 24;
#else
        LoadOPLModule(OPL_MODULE_ID_MCEMU, 0, 0, NULL);
#endif
    }

#ifdef PADEMU
    if (iop_reboot_count >= 2 && config->EnablePadEmuOp) {
        char args_for_pademu[8];
        memcpy(args_for_pademu, &config->PadEmuSettings, 4);
        memcpy(args_for_pademu + 4, &config->PadMacroSettings, 4);
        LoadOPLModule(OPL_MODULE_ID_PADEMU, 0, sizeof(args_for_pademu), args_for_pademu);
    }
#endif

    DPRINTF("Exiting services...\n");
    SifExitIopHeap();
    LoadFileExit();
    SifExitRpc();

    DPRINTF("New_Reset_Iop complete!\n");
    // we have 4 SifSetReg calls to skip in ELF's SifResetIop, not when we use it ourselves
    if (set_reg_disabled)
        set_reg_hook = 4;

    if (EnableDebug)
        BGCOLND(0x000000);

    return 1;
}

/*----------------------------------------------------------------------------------------*/
/* Reset IOP. This function replaces SifIopReset from the PS2SDK                          */
/*----------------------------------------------------------------------------------------*/
int Reset_Iop(const char *arg, int mode)
{
    static SifCmdResetData_t reset_pkt __attribute__((aligned(64)));
    struct t_SifDmaTransfer dmat;
    int arglen;

    _iop_reboot_count++; // increment reboot counter to allow RPC clients to detect unbinding!

    /*    SifStopDma();        For the sake of IGR (Which uses this function), don't disable SIF0 (IOP -> EE)
                because some games will be still spamming DMA transfers across SIF0 when IGR is invoked.
                SCE documents that DMA transfers should be stopped before IOP resets, but has neglected
                to explain the effects of not doing so.
                So far, it seems like the SIF (at least SIF0) will stop functioning properly.

                2 commits before this one, OPL appears to have worked around this problem by preventing
                the SIF BOOTEND flag from being set,
                which allowed SifInitCmd() to run ASAP (Even before the IOP finishes rebooting.
                That caused SifSetDChain() to be run ASAP, which re-enables SIF0.
                I don't find that a good workaround because it may result in a timing problem.    */

    for (arglen = 0; arg[arglen] != '\0'; arglen++)
        reset_pkt.arg[arglen] = arg[arglen];

    reset_pkt.header.psize = sizeof reset_pkt; // dsize is not initialized (and not processed, even on the IOP).
    reset_pkt.header.cid = SIF_CMD_RESET_CMD;
    reset_pkt.arglen = arglen;
    reset_pkt.mode = mode;

    dmat.src = &reset_pkt;
    dmat.dest = (void *)SifGetReg(SIF_SYSREG_SUBADDR);
    dmat.size = sizeof(reset_pkt);
    dmat.attr = SIF_DMA_ERT | SIF_DMA_INT_O;
    SifWriteBackDCache(&reset_pkt, sizeof(reset_pkt));

    DIntr();
    ee_kmode_enter();
    Old_SifSetReg(SIF_REG_SMFLAG, SIF_STAT_BOOTEND);

    if (!Old_SifSetDma(&dmat, 1)) {
        ee_kmode_exit();
        EIntr();
        return 0;
    }

    Old_SifSetReg(SIF_REG_SMFLAG, SIF_STAT_SIFINIT);
    Old_SifSetReg(SIF_REG_SMFLAG, SIF_STAT_CMDINIT);
    Old_SifSetReg(SIF_SYSREG_RPCINIT, 0);
    Old_SifSetReg(SIF_SYSREG_SUBADDR, (int)NULL);
    ee_kmode_exit();
    EIntr();

    return 1;
}
