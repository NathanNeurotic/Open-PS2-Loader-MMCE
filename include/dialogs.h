#ifndef __DIALOGS_H
#define __DIALOGS_H

#include "include/dia.h"

enum UI_ITEMS {
    UIID_BTN_CANCEL = 0,
    UIID_BTN_OK,

    UICFG_THEME = 10,
    UICFG_LANG,
    UICFG_SCROLL,
    UICFG_BGCOL,
    UICFG_UICOL,
    UICFG_TXTCOL,
    UICFG_SELCOL,
    UICFG_PLASCOL,  // plasma blend (gradient low end) color picker -- parity-audit #15
    UICFG_TITLECOL, // game-title text, separate from the general Text colour (#464)
    UICFG_RESETCOL,
    UICFG_AUTOSORT,
    UICFG_COVERART,
    UICFG_ENABLE_BGART,
    UICFG_ENABLE_ART_TAR,
    UICFG_ENABLE_ANALOG_NAV,
    UICFG_ART_DELAY,
    UICFG_WIDESCREEN,
    UICFG_AUTOREFRESH,
    UICFG_VMODE,
    UICFG_XOFF,
    UICFG_YOFF,
    UICFG_OVERSCAN,
    UICFG_NOTIFICATIONS,
    UICFG_GAMEVIEW,
    UICFG_APPSVIEW,

    CFG_DEBUG,
    CFG_PS2LOGO,
    CFG_HDDGAMELISTCACHE,
    CFG_EXITTO,
    CFG_CUSTOMCFGPATH, // opt-in "Custom Settings Path" (typed)
    CFG_DEFDEVICE,
    CFG_BDMMODE,
    CFG_HDDMODE,
    CFG_ETHMODE,
    CFG_APPMODE,
    CFG_MMCEMODE,
    CFG_FAVMODE,
    CFG_MMCEPREFIX,
    CFG_MMCESLOT,
    CFG_MMCEIGRSLOT,
    CFG_MMCE_WAIT_CYCLES,
    CFG_MMCE_USE_ALARMS,
    CFG_MMCEGAMEID,
    CFG_APPLYGAMEID,
    CFG_BDMCACHE,
    CFG_HDDCACHE,
    CFG_SMBCACHE,
    CFG_ENABLEUSB,
    CFG_ENABLEILK,
    CFG_ENABLEMX4SIO,
    CFG_ENABLEBDMHDD,
    CFG_ENABLEUDPBD,     // legacy: kept as an unused placeholder (superseded by CFG_NETPROTOCOL)
    CFG_NETBOOTPROTOCOL, // legacy: kept as an unused placeholder (superseded by CFG_NETPROTOCOL)
    CFG_NETSTART,        // network start mode row: Off / Manual / Auto (== START_MODE_*)
    CFG_NETPROTOCOL,     // protocol row: SMB / UDPFS / UDPBD (Off moved to CFG_NETSTART)
    CFG_UDPFSMODE,       // access row: Files (udpfs_ioman filesystem) vs IMG (udpfs_bd block/massN:); locked per protocol
    CFG_SMBDIALECT,      // SMB version row: SMBv1 / SMB2; only enabled while the protocol row is SMB
    CFG_LASTPLAYED,
    CFG_FOLDERNAV,
    CFG_RUMBLE,
    CFG_LBL_AUTOSTARTLAST,
    CFG_AUTOSTARTLAST,
    CFG_SELECTBUTTON,
    CFG_ENWRITEOP,
    CFG_BDMPREFIX,
    CFG_ETHPREFIX,
    CFG_HDDSPINDOWN,

    ABOUT_TITLE,
    ABOUT_BUILD_DETAILS,

    CFG_PARENLOCK_PASSWORD,

    CFG_SFX,
    CFG_BOOT_SND,
    CFG_BGM,
    CFG_SFX_VOLUME,
    CFG_BOOT_SND_VOLUME,
    CFG_BGM_VOLUME,
    CFG_DEFAULT_BGM_PATH,
    CFG_DEFAULT_CORE, // global default Loader Core (gDefaultCoreLoader); per-game "Default" follows it
    CFG_NEUTRINO_ARGS,
    CFG_NEUTRINO_DEVICE,
    CFG_NEUTRINO_VIDEO,   // global default Neutrino -gsm video mode (gNeutrinoVideoDefault); per-game "Default" follows it
    CFG_NEUTRINO_GSMCOMP, // global default -gsm ":c" comp half (gNeutrinoGsmCompDefault)
    CFG_POPSTARTER_DEVICE,
    CFG_LBL_POPSTARTER_PATH,
    CFG_POPSTARTER_PATH,
    CFG_POPSTARTER_RETROGEM_GAMEID,
    CFG_EMBER_DISPLAY, // Ember's own display mode, written to its settings.txt at launch

    CFG_BDMA_APPLY,
    CFG_LBL_BDMASOURCE,
    CFG_BDMASOURCE,
    CFG_LBL_BDMAMODE,
    CFG_BDMAMODE,
    CFG_LBL_VCD_USB_BDMA,
    CFG_VCD_USB_BDMA,        // USB VCD launches only: Ask every time (default) / pin exFAT / pin fat32
    CFG_VCD_HIDE_GAMEID,     // display-only: hide a leading PS1 game-ID prefix from the VCD list
    CFG_VCD_FIRST_DISC_ONLY, // #118: hide discs 2+ of a multi-disc PS1 set from the device VCD lists
    CFG_VCD_SHOW_PP_POPS,    // enumeration-only: list strict PP.<ID>.POPS.<name> one-game HDD partitions

    CFG_XSENSITIVITY,
    CFG_YSENSITIVITY,

    NETCFG_SHOW_ADVANCED_OPTS,
    NETCFG_PS2_IP_ADDR_TYPE,
    NETCFG_PS2_IP_ADDR_0,
    NETCFG_PS2_IP_ADDR_1,
    NETCFG_PS2_IP_ADDR_2,
    NETCFG_PS2_IP_ADDR_3,
    NETCFG_PS2_NETMASK_0,
    NETCFG_PS2_NETMASK_1,
    NETCFG_PS2_NETMASK_2,
    NETCFG_PS2_NETMASK_3,
    NETCFG_PS2_GATEWAY_0,
    NETCFG_PS2_GATEWAY_1,
    NETCFG_PS2_GATEWAY_2,
    NETCFG_PS2_GATEWAY_3,
    NETCFG_PS2_DNS_0,
    NETCFG_PS2_DNS_1,
    NETCFG_PS2_DNS_2,
    NETCFG_PS2_DNS_3,
    NETCFG_SHARE_ADDR_TYPE,
    NETCFG_SHARE_NB_ADDR,
    NETCFG_SHARE_IP_ADDR_0,
    NETCFG_SHARE_IP_ADDR_1,
    NETCFG_SHARE_IP_ADDR_2,
    NETCFG_SHARE_IP_ADDR_3,
    NETCFG_SHARE_IP_ADDR_DOT_0,
    NETCFG_SHARE_IP_ADDR_DOT_1,
    NETCFG_SHARE_IP_ADDR_DOT_2,
    NETCFG_SHARE_PORT,
    NETCFG_SHARE_NAME,
    NETCFG_SHARE_USERNAME,
    NETCFG_SHARE_PASSWORD,
    NETCFG_POPS_NOTICE,
    NETCFG_POPS_IPTYPE,
    NETCFG_POPS_IP_0,
    NETCFG_POPS_IP_1,
    NETCFG_POPS_IP_2,
    NETCFG_POPS_IP_3,
    NETCFG_POPS_MASK_0,
    NETCFG_POPS_MASK_1,
    NETCFG_POPS_MASK_2,
    NETCFG_POPS_MASK_3,
    NETCFG_POPS_GW_0,
    NETCFG_POPS_GW_1,
    NETCFG_POPS_GW_2,
    NETCFG_POPS_GW_3,
    NETCFG_POPS_SMB_IP_0,
    NETCFG_POPS_SMB_IP_1,
    NETCFG_POPS_SMB_IP_2,
    NETCFG_POPS_SMB_IP_3,
    NETCFG_POPS_SMB_PORT,
    NETCFG_POPS_SMB_SHARE,
    NETCFG_POPS_SMB_USER,
    NETCFG_POPS_SMB_PASS,
    NETCFG_POPS_IMPORT,
    NETCFG_ETHOPMODE,
    NETCFG_RECONNECT,
    NETCFG_POPSTARTER_BUTTON,
    NETCFG_OK,
    // Section labels for the SMB-only block, so it can be hidden whole when the selected network
    // protocol is not SMB. Appended here to keep the NETCFG_*_0..N consecutive runs above intact.
    NETCFG_LBL_SMB_SERVER,
    NETCFG_LBL_SHARE_ADDR_TYPE,
    NETCFG_LBL_SHARE_ADDRESS,
    NETCFG_LBL_SHARE_PORT,
    NETCFG_LBL_SHARE_NAME,
    NETCFG_LBL_SHARE_USER,
    NETCFG_LBL_SHARE_PASSWORD,
    NETCFG_LBL_SMBDIALECT,
    // HTTP endpoint rows. Appended, like the SMB section labels above, so every NETCFG_*_0..N
    // consecutive run stays intact -- gui.c indexes those by NETCFG_x_0 + i.
    NETCFG_LBL_HTTP_SERVER,
    NETCFG_HTTP_IP_0,
    NETCFG_HTTP_IP_1,
    NETCFG_HTTP_IP_2,
    NETCFG_HTTP_IP_3,
    NETCFG_LBL_HTTP_PORT,
    NETCFG_HTTP_PORT,
    NETCFG_LBL_HTTP_BASE,
    NETCFG_HTTP_BASE,
    NETCFG_HTTP_TEST,
    NETCFG_HTTP_DOT_0,
    NETCFG_HTTP_DOT_1,
    NETCFG_HTTP_DOT_2,
    NETCFG_LBL_HTTP_TEST,
#ifdef RETROACHIEVEMENTS
    // RA rows on the Network page. Guarded in lockstep with their rows in diaNetConfig --
    // guard one and not the other and the array carries an id the enum does not define.
    NETCFG_RA_TELEMETRY,
    NETCFG_RA_BADGES,
#endif

    CHTCFG_CHEATSOURCE,
    CHTCFG_CHEATCFG,
    CHTCFG_ENABLECHEAT,
    CHTCFG_CHEATMODE,
    CHTCFG_ENABLEIMAGE,

    GSMCFG_GSMSOURCE,
    GSMCFG_GSCONFIG,
    GSMCFG_ENABLEGSM,
    GSMCFG_GSMVMODE,
    GSMCFG_GSMXOFFSET,
    GSMCFG_GSMYOFFSET,
    GSMCFG_GSMFIELDFIX,

    UICFG_COVERFLOW_BUTTON,
    COVERFLOW_CFG_COUNT,
    COVERFLOW_CFG_SCALE,
    COVERFLOW_CFG_ANIM,
    COVERFLOW_CFG_DIM,

    NARGS_QB,
    NARGS_CWD,
    NARGS_CFG,
    NARGS_ELF,
    NARGS_ATA0,
    NARGS_ATA0ID,
    NARGS_ATA1,
    NARGS_EXTRA,
    NARGS_DBC,
    NARGS_LOGO,

    COMPAT_DMA = 100,
    COMPAT_ALTSTARTUP,
    COMPAT_GAMEID,
    COMPAT_DL_DEFAULTS,
    COMPAT_LOADER,
    COMPAT_NEUTRINO_ARGS,
    COMPAT_NEUTRINO_VIDEO,
    COMPAT_NEUTRINO_GSMCOMP,
    COMPAT_NEUTRINO_BSDFS,

    COMPAT_LOADFROMDISC_ID,

    COMPAT_VMC1_ACTION_ID,
    COMPAT_VMC2_ACTION_ID,
    COMPAT_VMC1_DEFINE_ID,
    COMPAT_VMC2_DEFINE_ID,
    COMPAT_VMC1_DISABLE, // UI_BOOL rows (read via diaGetInt, not dialog results -- no _ID/NOEXIT pair needed)
    COMPAT_VMC2_DISABLE,

    VMC_NAME,
    VMC_SIZE,
    VMC_BUTTON_CREATE,
    VMC_BUTTON_DELETE,
    VMC_STATUS,
    VMC_PROGRESS,
    VMC_REFRESH,

    NETUPD_OPT_UPD_ALL_LBL,
    NETUPD_OPT_UPD_ALL,
    NETUPD_PROGRESS_LBL,
    NETUPD_PROGRESS_PERC_LBL,
    NETUPD_PROGRESS,
    NETUPD_BTN_START,
    NETUPD_BTN_CANCEL,

    OSD_LANGUAGE_SOURCE,
    OSD_LANGUAGE_ENABLE,
    OSD_LANGUAGE_VALUE,
    OSD_TVASPECT_VALUE,
    OSD_VMODE_VALUE,

    VCDUSB_BTN_FAT32,
    VCDUSB_BTN_EXFAT,

    POPS_OVERWRITE_KEEP,
    POPS_OVERWRITE_REPLACE,

    // Settings-layout restructure: sub-page buttons (chained dialogs, goto-reshow pattern)
    UICFG_ARTWORK_BUTTON,
    UICFG_COLORS_BUTTON,
    LAUNCH_NEUTRINO_DEFAULTS_BUTTON,
    LAUNCH_OSD_DEFAULTS_BUTTON,
    DISPLAY_GSM_DEFAULTS_BUTTON,
    VCD_BDMA_BUTTON,
    VCD_LIST_BUTTON,
    VCD_NET_BUTTON,
    MMCE_COMM_BUTTON,
    MMCE_PATH_BUTTON,
    MMCE_SETTINGS_BUTTON,
    UICFG_GAME_LIST_BUTTON,
    SECURITY_PARENTAL_BUTTON,
    ADV_PREFIX_BUTTON,
    ADV_STORAGE_BUTTON,
    CFG_HDDOPLPART,
    GENERAL_GSM_DEFAULTS_BUTTON,
    LAUNCH_GSM_DEFAULTS_BUTTON,

#ifdef PADEMU
    PADEMU_GLOBAL_BUTTON,
    PADCFG_PADEMU_SOURCE,
    PADCFG_PADEMU_CONFIG,
    PADCFG_PADEMU_ENABLE,
    PADCFG_PADEMU_MODE,
    PADCFG_PADEMU_PORT,
    PADCFG_PADEMU_VIB,
    PADCFG_PADPORT,
    PADCFG_USBDG_MAC,
    PADCFG_USBDG_MAC_STR,
    PADCFG_PAD_MAC,
    PADCFG_PAD_MAC_STR,
    PADCFG_PAIR,
    PADCFG_PAIR_STR,
    PADCFG_BTINFO,
    PADCFG_VID,
    PADCFG_PID,
    PADCFG_REV,
    PADCFG_HCIVER,
    PADCFG_LMPVER,
    PADCFG_MANID,
    PADCFG_FEAT_START,
    PADCFG_FEAT_END = PADCFG_FEAT_START + 64,
    PADCFG_BT_SUPPORTED,
    PADCFG_PADEMU_MTAP,
    PADCFG_PADEMU_MTAP_PORT,
    PADCFG_PADEMU_WORKAROUND,
    PADCFG_PADEMU_WORKAROUND_STR,

    PADMACRO_GLOBAL_BUTTON,
    PADMACRO_CFG_SOURCE,
    PADMACRO_SLOWDOWN_L,
    PADMACRO_SLOWDOWN_TOGGLE_L,
    PADMACRO_SLOWDOWN_R,
    PADMACRO_SLOWDOWN_TOGGLE_R,
    PADMACRO_INVERT_LX,
    PADMACRO_INVERT_LY,
    PADMACRO_INVERT_RX,
    PADMACRO_INVERT_RY,
    PADMACRO_TURBO_SPEED,

    COMPAT_MODE_BASE = 250,
#else
    COMPAT_MODE_BASE = 200,
#endif
};

#define COMPAT_NOEXIT       0x70000000
#define COMPAT_LOADFROMDISC (COMPAT_LOADFROMDISC_ID | COMPAT_NOEXIT)

#define COMPAT_VMC1_ACTION (COMPAT_VMC1_ACTION_ID | COMPAT_NOEXIT)
#define COMPAT_VMC2_ACTION (COMPAT_VMC2_ACTION_ID | COMPAT_NOEXIT)
#define COMPAT_VMC1_DEFINE (COMPAT_VMC1_DEFINE_ID | COMPAT_NOEXIT)
#define COMPAT_VMC2_DEFINE (COMPAT_VMC2_DEFINE_ID | COMPAT_NOEXIT)

#ifdef PADEMU
extern struct UIItem diaPadEmuConfig[];
extern struct UIItem diaPadMacroConfig[];
extern struct UIItem diaPadEmuInfo[];
#endif

extern struct UIItem diaNetConfig[];
extern struct UIItem diaUIConfig[];
extern struct UIItem diaAudioConfig[];
extern struct UIItem diaControllerConfig[];
extern struct UIItem diaCompatConfig[];
extern struct UIItem diaVMCConfig[];
extern struct UIItem diaGSConfig[];
extern struct UIItem diaCheatConfig[];

extern struct UIItem diaConfig[];
extern struct UIItem diaAbout[];
extern struct UIItem diaVMC[];
extern struct UIItem diaNetCompatUpdate[];

extern struct UIItem diaOSDConfig[];
extern struct UIItem diaCoverflowConfig[];
extern struct UIItem diaNeutrinoArgs[];
extern struct UIItem diaDeviceConfig[];
extern struct UIItem diaVcdConfig[];
extern struct UIItem diaMmceConfig[];
extern struct UIItem diaVcdUsbMode[];
extern struct UIItem diaArtworkConfig[];
extern struct UIItem diaColorsConfig[];
extern struct UIItem diaDisplayConfig[];
extern struct UIItem diaLaunchConfig[];
extern struct UIItem diaNeutrinoDefaults[];
extern struct UIItem diaBdmaConfig[];
extern struct UIItem diaVcdListConfig[];
extern struct UIItem diaPopsNetConfig[];
extern struct UIItem diaPopsOverwrite[];
extern struct UIItem diaMmceCommConfig[];
extern struct UIItem diaMmcePathConfig[];
extern struct UIItem diaSecurityConfig[];
extern struct UIItem diaAdvancedConfig[];

#endif
