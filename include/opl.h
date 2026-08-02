#ifndef __OPL_H
#define __OPL_H

#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <hdd-ioctl.h>
#include "opl-hdd-ioctl.h"
#include <iopcontrol.h>
#include <iopheap.h>
#include <string.h>
#include <loadfile.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sbv_patches.h>
#include <libcdvd.h>
#include <libpad.h>
#include <sifcmd.h> // BEFORE libmc.h: ps2sdk's 2026-07 libmc.h uses ALIGNED_FOR_SIFDMA (sifcmd-common.h) without including it -- not self-contained; harmless on older SDKs
#include <libmc.h>
#include <netman.h>
#include <ps2ips.h>
#include <debug.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <malloc.h>
#include <math.h>
#include <osd_config.h>
#include <libpwroff.h>
#include <usbhdfsd-common.h>
#include <smod.h>
#include <smem.h>
#include <debug.h>
#include <ps2smb.h>
#include "config.h"

#include "include/hddsupport.h"
#include "include/supportbase.h"
#include "include/bdmsupport.h"

// Last Played Auto Start
#include <time.h>

// Master password for disabling the parental lock.
#define OPL_PARENTAL_LOCK_MASTER_PASS "989765"

// IO type IDs
#define IO_CUSTOM_SIMPLEACTION    1 // handler for parameter-less actions
#define IO_MENU_UPDATE_DEFFERED   2
#define IO_COMPAT_UPDATE_DEFFERED 4

// Codes have been planned to fit the design of the GUI functions within gui.c.
#define OPL_COMPAT_UPDATE_STAT_WIP        0
#define OPL_COMPAT_UPDATE_STAT_DONE       1
#define OPL_COMPAT_UPDATE_STAT_ERROR      -1
#define OPL_COMPAT_UPDATE_STAT_CONN_ERROR -2
#define OPL_COMPAT_UPDATE_STAT_ABORTED    -3

#define OPL_VMODE_CHANGE_CONFIRMATION_TIMEOUT_MS 10000

int oplPath2Mode(const char *path);
int oplGetAppImageByMode(int mode, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm);
int oplGetAppImage(const char *device, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm);
int oplScanApps(int (*callback)(const char *path, config_set_t *appConfig, void *arg), void *arg);
int oplShouldAppsUpdate(void);
config_set_t *oplGetLegacyAppsConfig(void);
config_set_t *oplGetLegacyAppsInfo(char *name);

void setErrorMessage(int strId);
void setErrorMessageWithCode(int strId, int error);
void setErrorMessagePathCode(int strId, const char *path, int error);
int loadConfig(int types);
int saveConfig(int types, int showUI);
void applyConfig(int themeID, int langID, int skipDeviceRefresh);
void menuDeferredUpdate(void *data);
// Queue an immediate list rebuild for every enabled VCD-capable device page. Runtime settings that
// call vcdMarkAllDirty() need this because no-auto-refresh modes (notably HDD) will not consume the
// dirty flag on their own. Favourites are rebuilt separately through loadFavourites().
void oplQueueVcdDeviceUpdates(void);
void moduleUpdateMenu(int mode, int themeChanged, int langChanged);
void handleLwnbdSrv();
void deinit(int exception, int modeSelected);
// deinit with a SECOND kept mode (-1 = none): the Neutrino no-reset handoff needs BOTH the game's
// device and the neutrino.elf-hosting device mounted, since Neutrino reads its config/modules and
// the game ISO through our live mounts before doing its own IOP reset.
void deinitEx(int exception, int modeSelected, int modeSelected2);

// Shutdown minimal services initiated for auto loading.
void miniDeinit(config_set_t *configSet);

extern char *gBaseMCDir;

enum ETH_OP_MODES {
    ETH_OP_MODE_AUTO = 0,
    ETH_OP_MODE_100M_FDX,
    ETH_OP_MODE_100M_HDX,
    ETH_OP_MODE_10M_FDX,
    ETH_OP_MODE_10M_HDX,

    ETH_OP_MODE_COUNT
};

extern int ps2_ip_use_dhcp;
extern int ps2_ip[4];
extern int ps2_netmask[4];
extern int ps2_gateway[4];
extern int ps2_dns[4];
extern int gETHOpMode; // See ETH_OP_MODES.
extern int gPCShareAddressIsNetBIOS;
extern int pc_ip[4];
extern int gPCPort;
// Please keep these string lengths in-sync with the limits within CDVDMAN.
extern char gPCShareNBAddress[17];
extern char gPCShareName[32];
extern char gPCUserName[32];
extern char gPCPassword[32];

//// Settings

// describes what is happening in the network startup thread (>0 means loading, <0 means error)...
extern int gNetworkStartup;
extern int gHDDSpindown;
/// Refer to enum START_MODE within iosupport.h
extern int gBDMStartMode;
extern int gHDDStartMode;
extern int gETHStartMode;
extern int gAPPStartMode;
extern int gMMCEStartMode;
extern int bdmCacheSize;
extern int hddCacheSize;
extern int smbCacheSize;

extern int gMMCESlot;
extern int gMMCEIGRSlot;
extern int gMMCEEnableGameID; //Send GameID on game launch
extern int gApplyGameID;      // Display the visual GameID barcode on launch (Pixel FX / RetroGEM HDMI auto-profiles)
extern int gMMCEAckWaitCycles;
extern int gMMCEUseAlarms;
extern int gEnableUSB;
extern int gEnableILK;
extern int gEnableMX4SIO;
extern int gEnableBdmHDD;
extern int gEnableUDPBD; // UDPBD network block device (smap_udpbd); NIC-exclusive with the SMB/ETH stack
// Network-boot transport chosen when gEnableUDPBD is on. Both register the "udp" BDM token and are
// NIC-exclusive with SMB; they differ in wire protocol + which IOP module(s) OPL loads to list games.
enum {
    NET_BOOT_UDPBD = 0, // smap_udpbd monolith (SUDPBDv2); Neutrino launch -bsd=udpbd
    NET_BOOT_UDPFS = 1, // udpfs smap+ministack+udpfs_bd chain (UDPRDMA); Neutrino launch -bsd=udpfsbd
};
extern int gNetBootProtocol; // NET_BOOT_UDPBD | NET_BOOT_UDPFS (legacy shadow, derived from gNetworkProtocol)

// Unified network-protocol selector. The single NIC (SMAP) carries at most ONE network transport per
// session, so these are mutually exclusive by construction -- one enum instead of the old
// {gETHStartMode, gEnableUDPBD, gNetBootProtocol} trio + interlock. Local devices (USB/HDD/MMCE) are
// independent. gETHStartMode / gEnableUDPBD / gNetBootProtocol are kept as DERIVED shadows so
// downstream consumers can migrate incrementally.
// USER-FACING picker is Off / SMB / UDPFS / UDPBD. UDPFS is ONE menu entry with a "UDPFS Access"
// sub-mode (Files -> NET_PROTO_UDPFS filesystem / Image -> NET_PROTO_UDPFSBD block), since one udpfs
// protocol serves both (only one IOP backend can bind port 0xF5F6 per boot). UDPBD (SUDPBDv2) is a
// SEPARATE, wire-incompatible protocol (its own server, port 0xBDBD) and stays first-class so users on
// the older udpbd-server are never stranded -- it is neither hidden nor auto-migrated.
enum NETWORK_PROTOCOL {
    NET_PROTO_OFF = 0,     // no NIC device (SMAP idle) -- fork default
    NET_PROTO_SMB = 1,     // SMB/ETH stack (== legacy gETHStartMode > DISABLED)
    NET_PROTO_UDPFS = 2,   // udpfs FILESYSTEM (udpfs_ioman "udpfs:"); loose ISOs -- picker "UDPFS", access=Files
    NET_PROTO_UDPFSBD = 3, // udpfs BLOCK device (udpfs_bd -> massN:, UDPRDMA) -- picker "UDPFS", access=Image
    NET_PROTO_UDPBD = 4,   // smap_udpbd / SUDPBDv2 monolith -- picker "UDPBD"; server = udpbd-server (0xBDBD)
};
extern int gNetworkProtocol; // enum NETWORK_PROTOCOL -- authoritative backend; the three above are derived shadows
extern int gNetStartMode;    // START_MODE_* -- network start row (Off/Manual/Auto); DISABLED <=> protocol OFF

/*
  SMB dialect, shown as a sub-row of the SMB protocol choice (the same shape as the UDPFS Access
  row). Only meaningful when gNetworkProtocol == NET_PROTO_SMB; ignored otherwise.

  SMBv1 stays the default and is never removed -- it is the only dialect with hardware validation
  behind it, and the fork's own PC server (pc/smbserver/smbserver_opl.py) speaks it.

  SMB3 is deliberately NOT offered yet: it mandates packet signing (AES-CMAC/HMAC-SHA256), which
  does not exist in this tree, so listing it would be a picker that silently behaves as something
  else. The enum value is reserved so the row extends by one line once signing lands.
*/
enum SMB_DIALECT {
    SMB_DIALECT_SMB1 = 0, // NT LM 0.12 -- default, in-game reader = smb.c
    SMB_DIALECT_SMB2 = 1, // 2.0.2 / 2.1 -- in-game reader = smb2.c
    SMB_DIALECT_SMB3 = 2, // reserved, not selectable until signing exists
};
extern int gSMBDialect; // enum SMB_DIALECT; applies to the SMB backend only

extern int gAutosort;
extern int gAutoRefresh;
extern int gEnableNotifications;
extern int gEnableArt;
extern int gEnableArtTar; // opt-in .tar cover-art loader (ART/art.tar), default OFF
extern int gArtDelay;    // artwork inactive frame delay (0=instant, 2=fast default, 5=medium, 8=standard)
extern int gWideScreen;
// Default game-list view for VCD-capable device pages (+ Favourites, later).
enum {
    GAME_VIEW_BOTH = 0, // both lists reachable via the per-device L3 toggle (default)
    GAME_VIEW_ISO,      // lock every VCD-capable page to its ISO/disc list
    GAME_VIEW_VCD       // lock every VCD-capable page to its VCD (PS1) list
};
extern int gDefaultGameView;
extern int gVMode; // 0 - Auto, 1 - PAL, 2 - NTSC
extern int gXOff;
extern int gYOff;
extern int gOverscan;
extern int gSelectButton;
extern int gHDDGameListCache;

extern int gEnableSFX;
extern int gEnableBootSND;
extern int gEnableBGM;
extern int gSFXVolume;
extern int gBootSndVolume;
extern int gBGMVolume;
extern char gDefaultBGMPath[128];

extern int gXSensitivity;
extern int gYSensitivity;

extern int gCheatSource;
extern int gGSMSource;
extern int gPadEmuSource;

extern int gOSDLanguageValue;
extern int gOSDTVAspectRatio;
extern int gOSDVideOutput;
extern int gOSDLanguageEnable;
extern int gOSDLanguageSource;

extern int showCfgPopup;
extern int showNetDhcpPopup; // boot toast: UDP transport selected while IP Type = DHCP (needs static IP)

#ifdef IGS
#define IGS_VERSION "0.1"
#endif

// ------------------------------------------------------------------------------------------------------------------------

#ifdef PADEMU
extern int gEnablePadEmu;
extern int gPadEmuSettings;
extern int gPadMacroSource;
extern int gPadMacroSettings;
#endif

// ------------------------------------------------------------------------------------------------------------------------

// 0,1,2 scrolling speed
extern int gScrollSpeed;
// Exit path
extern char gExitPath[256];
// Extra command-line flags appended to every Neutrino launch
extern char gNeutrinoArgs[256];
extern char gNeutrinoPath[256]; // custom neutrino.elf path (General Settings); "" = mc0:/mc1: auto-detect
// Neutrino Device picker: a driver-accurate device TYPE that holds <root>:/neutrino/neutrino.elf.
// (Replaces the old device-INDEX enum mc0/mc1/mass0-7/mmce0/mmce1; legacy ints are migrated on load.)
enum { NEUTRINO_DEV_AUTO = 0,       // game device, then mc0/mc1 (legacy behaviour)
       NEUTRINO_DEV_MC,             // mc0: / mc1:
       NEUTRINO_DEV_USB,            // BDM "usb"        -> the mounted massN:
       NEUTRINO_DEV_MX4SIO,         // BDM "mx4sio"/sdc -> the mounted massN:
       NEUTRINO_DEV_MMCE,           // mmce0: / mmce1:
       NEUTRINO_DEV_EXFAT_HDD,      // BDM "ata" internal exFAT HDD -> the mounted massN:
       NEUTRINO_DEV_APA_HDD,        // APA HDD: the mounted OPL data partition (pfs0:)
       NEUTRINO_DEV_GAME };         // the active game's OWN device ONLY (co-located neutrino.elf); no MC/boot fallback (a miss toasts "not found"). Appended at the enum end to keep saved neutrino_devtype ints stable.
extern int gNeutrinoDevice;         // Neutrino ELF device (NEUTRINO_DEV_*); Auto scans mc0/mc1 + honors a legacy neutrino_path
extern int gDefaultCoreLoader;      // global default Loader Core: 0 = <OPL> (native), 1 = Neutrino. A game's per-game $CoreLoader overrides it; absent per-game key = follow this global.
extern int gNeutrinoVideoDefault;   // global default Neutrino -gsm video mode (0=Off..5=1080i x3, indices = system.c gsmVideoTokens). Per-game $NeutrinoVideo overrides; absent per-game key = follow this.
extern int gNeutrinoGsmCompDefault; // global default -gsm ":c" field-flip half (0=off, 1-3=type); only emitted when the effective video mode is set.
extern int gNeutrinoElfArg;         // opt-in (settings key only, no UI): auto-emit -elf=cdrom0:\<startup>;1 on Neutrino launches
extern char gPopstarterPath[256];   // custom POPSTARTER.ELF path (used only when gPopstarterDevice == POPS_DEV_CUSTOM)
extern char gBootDir[256];          // boot directory (cwd) OPL launched from, e.g. "mass0:/APPS"; "" if undeterminable
extern int gDeinitTerminal;         // 1 while deinit() runs for exit/poweroff, 0 for a game/app LAUNCH teardown.
                                    // Launch teardown must NOT power shared buses down (dev9: the post-deinit
                                    // POPSTARTER.ELF read comes off the ATA-backed massN: mount).
// POPSTARTER.ELF Device picker: where PS1 VCD launches load POPS/POPSTARTER.ELF from. Default tries the
// boot device (cwd) then the VCD's own device; the 6 TYPEs force a device; Custom uses gPopstarterPath.
enum { POPS_DEV_DEFAULT = 0,   // cwd (gBootDir) /POPS/, then the VCD's own device (back-compat fallback)
       POPS_DEV_MC,            // mc0: / mc1:
       POPS_DEV_USB,           // BDM "usb"        -> the mounted massN:
       POPS_DEV_MX4SIO,        // BDM "mx4sio"/sdc -> the mounted massN:
       POPS_DEV_MMCE,          // mmce0: / mmce1:
       POPS_DEV_EXFAT_HDD,     // BDM "ata" internal exFAT HDD -> the mounted massN:
       POPS_DEV_APA_HDD,       // APA HDD: the mounted OPL data partition (pfs0:)
       POPS_DEV_CUSTOM,        // free-text gPopstarterPath
       POPS_DEV_GAME };        // the VCD's OWN device ONLY (<devPrefix>/POPS/POPSTARTER.ELF); no boot/cwd fallback (a miss toasts "not found"). Appended at the enum end to keep saved popstarter_device ints stable.
extern int gPopstarterDevice;  // POPSTARTER.ELF device (POPS_DEV_*); legacy non-empty popstarter_path -> Custom
extern int gBdmaSource;        // BDMA SOURCE device family (VCD_BDMA_SRC_*); persisted in conf
extern int gBdmaMode;          // BDMA MODE mirrored from the mc?:/POPSTARTER/ marker (VCD_BDMA_*)
extern int gBdmaApplyOnLaunch; // auto-equip the launched VCD's matching exFAT driver before boot (1=on)
extern int gVcdHideGameId;     // display-only: hide a leading PS1 game-ID prefix from the VCD list (1=on, default off)
extern int gVcdFirstDiscOnly;  // #118: hide discs 2+ of a multi-disc PS1 set from the device VCD lists (1=on, default off)
// Enable Debug Colors
extern int gEnableDebug;

extern int gPS2Logo;

// Default device
extern int gDefaultDevice;

extern int gEnableWrite;

// These prefixes are relative to the device's name (meaning that they do not include the device name).
extern char gBDMPrefix[32];
extern char gETHPrefix[32];
extern char gMMCEPrefix[32];

extern int gRememberLastPlayed;
extern int gEnableFolderNav; // opt-in: browse subdirectories inside a device's game list
extern int gEnableRumble;    // opt-in: short pad rumble tap when the menu cursor moves (#172)

// Last Played Auto Start
extern int KeyPressedOnce;
extern int gAutoStartLastPlayed;
extern int RemainSecs, DisableCron;
extern clock_t CronStart;

extern unsigned char gDefaultBgColor[3];
extern unsigned char gDefaultPlasBlendColor[3];
extern unsigned char gDefaultTextColor[3];
extern unsigned char gDefaultSelTextColor[3];
extern unsigned char gDefaultUITextColor[3];

// Launching games with args
extern hdl_game_info_t *gAutoLaunchGame;
extern base_game_info_t *gAutoLaunchBDMGame;
extern bdm_device_data_t *gAutoLaunchDeviceData;
extern char *gHDDPrefix;
extern char gOPLPart[128];

void initSupport(item_list_t *itemList, int mode, int force_reinit);

void setDefaultColors(void);

#define MENU_ITEM_HEIGHT 19

#include "include/menusys.h"

typedef struct
{
    item_list_t *support;

    /// menu item used with this list support
    menu_item_t menuItem;

    /// submenu list
    submenu_list_t *subMenu;
} opl_io_module_t;

// Favourites accessor: expose the (file-static) module table so favsupport.c can reach the
// FAV module without de-static-ing list_support.
opl_io_module_t *oplGetModule(int mode);

/*
BLURT output char blurttext[128];
#define BLURT                                                                           \
    snprintf(blurttext, sizeof(blurttext), "%s\\%s(%d)", __FILE__, __func__, __LINE__); \
    delay(10);
#define BLURT snprintf(blurttext, sizeof(blurttext), "%s(%d)", blurttext, __LINE__);
*/
#endif
