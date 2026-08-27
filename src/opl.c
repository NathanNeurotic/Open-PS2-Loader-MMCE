/*
  Copyright 2009, Volca
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.
*/

#include "include/opl.h"
#include "include/ioman.h"
#include "include/gui.h"
#include "include/guigame.h"
#include "include/renderman.h"
#include "include/lang.h"
#include "include/themes.h"
#include "include/textures.h"
#include "include/artindex.h"
#include "include/pad.h"
#include "include/texcache.h"
#include "include/dia.h"
#include "include/dialogs.h"
#include "include/menusys.h"
#include "include/system.h"
#include "include/debug.h"
#include "include/config.h"
#include "include/util.h"
#include "include/compatupd.h"
#include "include/extern_irx.h"
#include "httpclient.h"

#include "include/supportbase.h"
#include "include/bdmsupport.h"
#include "include/ethsupport.h"
#include "include/udpfssupport.h"
#include "include/hddsupport.h"
#include "include/appsupport.h"
#include "include/favsupport.h"
#include "include/folderbrowse.h" // folderDepth -- favourites suppression inside subfolders
#include "include/mmcesupport.h"  // mmceLoadModules() -- MMCE boot branch of resolveBootDirToMass
#include "include/tar.h"          // tarInvalidate -- re-arm the .tar probe on a settings apply
#include "include/vcdsupport.h"   // vcdViewActive stub -- isVcd stays 0 until item 12 lands

#include "include/cheatman.h"
#include "include/sound.h"
#include "include/xparam.h"

// FIXME: We should not need this function.
//        Use newlib's 'stat' to get GMT time.
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h> // iox_stat_t
int configGetStat(config_set_t *configSet, iox_stat_t *stat);

#include <unistd.h>
#include <sys/stat.h> // mkdir() -- create mc?:/OPL before mirroring the settings-path redirect.
                      // Explicit rather than transitive on purpose: this tree builds WITHOUT -Wall,
                      // so an implicitly declared mkdir would compile silently and only go wrong at
                      // runtime. bdmsupport.c includes it for the same reason.
#ifdef PADEMU
#include <libds34bt.h>
#include <libds34usb.h>
#endif

#ifdef __EESIO_DEBUG
#include "SIOCookie.h"
#define LOG_INIT() ee_sio_start(38400, 0, 0, 0, 0, 1)
#define LOG_ENABLE() \
    do {             \
    } while (0)
#else
#ifdef __DEBUG
#include "include/debug.h"
#define LOG_INIT() \
    do {           \
    } while (0)
#define LOG_ENABLE() ioPutRequest(IO_CUSTOM_SIMPLEACTION, &debugSetActive)
#else
#define LOG_INIT() \
    do {           \
    } while (0)
#define LOG_ENABLE() \
    do {             \
    } while (0)
#endif
#endif

// App support stuff.
static unsigned char shouldAppsUpdate;

// Network support stuff.
#define HTTP_IOBUF_SIZE 512
static unsigned int CompatUpdateComplete, CompatUpdateTotal;
static unsigned char CompatUpdateStopFlag, CompatUpdateFlags;
static short int CompatUpdateStatus;

static void clearIOModuleT(opl_io_module_t *mod)
{
    mod->subMenu = NULL;
    mod->support = NULL;
    mod->menuItem.execCross = NULL;
    mod->menuItem.execCircle = NULL;
    mod->menuItem.execSquare = NULL;
    mod->menuItem.execTriangle = NULL;
    mod->menuItem.fav = NULL;
    mod->menuItem.toggleView = NULL;
    mod->menuItem.hints = NULL;
    mod->menuItem.icon_id = -1;
    mod->menuItem.current = NULL;
    mod->menuItem.submenu = NULL;
    mod->menuItem.pagestart = NULL;
    mod->menuItem.last = NULL; // coverflow wrap tail (device re-init must not leave it dangling)
    mod->menuItem.remindLast = 0;
    mod->menuItem.refresh = NULL;
    mod->menuItem.text = NULL;
    mod->menuItem.text_id = -1;
    mod->menuItem.userdata = NULL;
}

// forward decl
static void clearMenuGameList(opl_io_module_t *mdl);
static void moduleCleanup(opl_io_module_t *mod, int exception, int modeSelected);
static void reset(void);
static void deferredAudioInit(void);

// frame counter
static unsigned int frameCounter;
// Per-mode background-rescan throttle (Fix B): the every-frame (updateDelay==0) device rescans
// enumerate the SIO2/mass bus; space them by a minimum wall-clock interval so they don't run
// unthrottled. A real device change bypasses the throttle via bdmGetGeneration().
// A BDM slot that has never held a device is probed this many times less often than one that has.
// Its probe is only a missed-event net; a present device keeps the full cadence so removal is still
// noticed promptly.
#define BDM_EMPTY_SLOT_PROBE_RATIO 8

static clock_t lastBgRescan[MODE_COUNT];
static unsigned int lastSeenBdmGeneration;

static char errorMessage[256];
static int errorMessageStringId = -1;

static opl_io_module_t list_support[MODE_COUNT];

// Favourites accessor (see opl.h): keep list_support[] file-static, but let favsupport.c
// reach the FAV module through this thin wrapper.
opl_io_module_t *oplGetModule(int mode)
{
    return &list_support[mode];
}

// Global data
char *gBaseMCDir;
int ps2_ip_use_dhcp;
int ps2_ip[4];
int ps2_netmask[4];
int ps2_gateway[4];
int ps2_dns[4];
int gETHOpMode; // See ETH_OP_MODES.
int gPCShareAddressIsNetBIOS;
int pc_ip[4];
int gPCPort;
char gPCShareNBAddress[17];
char gPCShareName[32];
char gPCUserName[32];
char gPCPassword[32];
int gNetworkStartup;
int gHDDSpindown;
int gBDMStartMode;
int gHDDStartMode;
int gETHStartMode;
int gAPPStartMode;
int gMMCEStartMode;
int gMMCEIGRSlot;
int gMMCESlot;
int gMMCEAckWaitCycles;
int gMMCEUseAlarms;
int gMMCEEnableGameID;
int bdmCacheSize;
int hddCacheSize;
int smbCacheSize;
int gApplyGameID;
int gEnableUSB;
char gNeutrinoArgs[256];     // extra command-line flags appended to every Neutrino launch
char gNeutrinoPath[256];     // custom neutrino.elf path; "" -> auto-detect on mc0:/mc1:
int gNeutrinoDevice;         // Neutrino ELF device (NEUTRINO_DEV_*); Auto scans mc0/mc1 + honors a legacy gNeutrinoPath
int gDefaultCoreLoader;      // global default Loader Core (0=<OPL>, 1=Neutrino); per-game $CoreLoader overrides, absent key = follow this
int gNeutrinoVideoDefault;   // global default Neutrino -gsm video mode (0=Off..5=1080i x3); per-game $NeutrinoVideo overrides
int gNeutrinoGsmCompDefault; // global default -gsm ":c" field-flip half (0=off, 1-3=type)
int gNeutrinoElfArg;         // default-on (settings key only, no UI): auto-emit -elf=cdrom0: on Neutrino launches
int gDefaultGameView;
char gPopstarterPath[256];         // custom POPSTARTER.ELF path (used only when gPopstarterDevice == POPS_DEV_CUSTOM)
int gPopstarterDevice;             // POPSTARTER.ELF device (POPS_DEV_*); legacy path -> Custom
int gPopstarterRetroGemGameID = 1; // RetroGEM Game ID optical barcode for VCD launches (1=on, default)
int gBdmaSource;                   // BDMA SOURCE device family (VCD_BDMA_SRC_*)
int gBdmaMode;                     // BDMA MODE mirrored from the mc?:/POPSTARTER/ marker
int gBdmaApplyOnLaunch;            // auto-equip the launched VCD's matching exFAT driver before boot
int gVcdUsbBdmaMode;               // VCD_USB_BDMA_*: POPSTARTER USB driver for USB VCD launches
int gVcdHideGameId;                // display-only: hide a leading PS1 game-ID prefix from VCD lists
int gVcdShowPpPops;                // enumeration-only: list strict PP.<ID>.POPS.<name> one-game HDD partitions
int gVcdFirstDiscOnly;             // hide discs 2+ of multi-disc PS1 sets
char gBootDir[256];                // boot directory (cwd) OPL launched from; "" if undeterminable
int gEnableILK;
int gEnableBGArt;
int gEnableArtTar;                       // .tar art packs (item 45); no UI until gate D
int gArtDelay;                           // inactivity frames before art loads; safe default until gate D tunes it
int gEnableFolderNav;                    // folder browsing in game lists (item 34)
unsigned char gDefaultPlasBlendColor[3]; // plasma gradient low end; black = historical look
volatile int gLastSaveErrno = 0;
// Set by guiHandleDeferedIO when it abandons a bounded wait: the request never ran, so any
// error the caller would otherwise report against the target path is meaningless.
volatile int gLastDeferredTimedOut = 0;
int gEnableMX4SIO;
int gEnableBdmHDD;
int gEnableUDPBD;
int gNetBootProtocol; // NET_BOOT_UDPBD | NET_BOOT_UDPFS (legacy shadow, derived from gNetworkProtocol)
int gNetworkProtocol; // enum NETWORK_PROTOCOL -- authoritative backend selector (Off/SMB/UDPBD/UDPFSBD/UDPFS)
int gNetStartMode;    // START_MODE_* -- the Off/Manual/Auto network start row (see the 3-row Network setting)
int gAutosort;
int gAutoRefresh;
int gEnableNotifications;
int gEnableArt;
int gWideScreen;
int gVMode; // 0 - Auto, 1 - PAL, 2 - NTSC
int gXOff;
int gYOff;
int gOverscan;
int gSelectButton;
int gHDDGameListCache;
int gEnableSFX;
int gEnableRumble;
int gEnableBootSND;
int gEnableBGM;
int gSFXVolume;
int gBootSndVolume;
int gBGMVolume;
char gDefaultBGMPath[128];
int gCheatSource;
int gGSMSource;
int gPadEmuSource;
int gFadeDelay;
int toggleSfx;
int showCfgPopup;
int showNetDhcpPopup; // a UDP transport is selected but IP Type is DHCP -- ministack needs a static IP
#ifdef PADEMU
int gEnablePadEmu;
int gPadEmuSettings;
int gPadMacroSource;
int gPadMacroSettings;
#endif
int gScrollSpeed;
char gExitPath[256];
char gCustomSettingsPath[64];
int gEnableDebug;
int gPS2Logo;
int gDefaultDevice;
int gEnableWrite;
char gBDMPrefix[32];
char gETHPrefix[32];
char gMMCEPrefix[32];
int gRememberLastPlayed;
int KeyPressedOnce;
int gAutoStartLastPlayed;
int RemainSecs, DisableCron;
clock_t CronStart;
unsigned char gDefaultBgColor[3];
unsigned char gDefaultTextColor[3];
unsigned char gDefaultSelTextColor[3];
unsigned char gDefaultUITextColor[3];
hdl_game_info_t *gAutoLaunchGame;
base_game_info_t *gAutoLaunchBDMGame;
bdm_device_data_t *gAutoLaunchDeviceData;
char gOPLPart[128];
char *gHDDPrefix;
char gExportName[32];

int gXSensitivity;
int gYSensitivity;

int gOSDLanguageValue;
int gOSDTVAspectRatio;
int gOSDVideOutput;
int gOSDLanguageEnable;
int gOSDLanguageSource;

void moduleUpdateMenuInternal(opl_io_module_t *mod, int themeChanged, int langChanged);

void moduleUpdateMenu(int mode, int themeChanged, int langChanged)
{
    if (mode == -1)
        return;

    opl_io_module_t *mod = &list_support[mode];
    moduleUpdateMenuInternal(mod, themeChanged, langChanged);
}

void moduleUpdateMenuInternal(opl_io_module_t *mod, int themeChanged, int langChanged)
{
    if (!mod->support)
        return;

    if (langChanged) {
        guiUpdateScreenScale();
        guiCheckNotifications(0, langChanged);
    }

    // refresh Hints
    menuRemoveHints(&mod->menuItem);

    menuAddHint(&mod->menuItem, _STR_MENU, START_ICON);
    if (!mod->support->enabled)
        menuAddHint(&mod->menuItem, _STR_START_DEVICE, gSelectButton == KEY_CIRCLE ? CIRCLE_ICON : CROSS_ICON);
    else {
        menuAddHint(&mod->menuItem, _STR_RUN, gSelectButton == KEY_CIRCLE ? CIRCLE_ICON : CROSS_ICON);

        if (gTheme->infoElems.first)
            menuAddHint(&mod->menuItem, _STR_INFO, SQUARE_ICON);

        unsigned char flags = (mod->support->mode == FAV_MODE) ? favGetFlags(mod->support) : mod->support->flags;
        if (!(flags & MODE_FLAG_NO_COMPAT) || gEnableWrite)
            menuAddHint(&mod->menuItem, _STR_OPTIONS, TRIANGLE_ICON);

        menuAddHint(&mod->menuItem, _STR_REFRESH, SELECT_ICON);

        if (gFAVStartMode)
            menuAddHint(&mod->menuItem, _STR_FAV_HINT, R3_ICON);

        // L3 toggles the device's disc list <-> its VCD (PS1-via-POPSTARTER) list -- only under the
        // "Both" default-view setting; ISO/VCD lock the page, so the toggle and its hint go away.
        if (vcdModeSupported(mod->support->mode) && gDefaultGameView == GAME_VIEW_BOTH)
            menuAddHint(&mod->menuItem, _STR_VCD, L3_ICON);
    }

    // refresh Cache
    if (themeChanged) {
        if (mod->subMenu) {
            // Serialize the per-item cache_id/cache_uid array reallocation with rendering: this runs
            // on the IO thread during a mid-session theme reload while the GUI thread draws the
            // carousel THROUGH those arrays (read + write on enqueue). Unserialized, a draw racing
            // the free()+malloc() wrote a fresh (index, uid) pair through a freed pointer into a
            // NEIGHBOUR's reallocated array -- the permanent wrong-cover mapping.
            guiLock();
            submenuRebuildCache(mod->subMenu);
            guiUnlock();
        }
        guiCheckNotifications(themeChanged, 0);
    }
}

static void itemInitSupport(item_list_t *support)
{
    support->itemInit(support);
    moduleUpdateMenuInternal((opl_io_module_t *)support->owner, 0, 0);
    // Manual refreshing can only be done if either auto refresh is disabled or auto refresh is disabled for the item.
    if (!gAutoRefresh || (support->updateDelay == MENU_UPD_DELAY_NOUPDATE) || support->mode == MMCE_MODE)
        ioPutRequest(IO_MENU_UPDATE_DEFFERED, &support->mode);
}

static void itemExecSelect(struct menu_item *curMenu)
{
    item_list_t *support = curMenu->userdata;
    sfxPlay(SFX_CONFIRM);
    // That confirm just armed a rumble pulse, and everything below blocks the GUI thread without
    // polling readPads() -- config load, GameID hold, the whole itemLaunch chain. The IOP LATCHES
    // the actuator, so without this the motor buzzes through the entire loading screen. GUI-thread
    // call site, which is the only kind pad.c allows.
    padRumbleFlush();

    if (support) {
        if (support->enabled) {
            if (curMenu->current) {
                // Folder browsing: a folder row DESCENDS (rescan one level deeper) instead of
                // launching. folderDescend marks the mode dirty; the deferred update rebuilds the list.
                if (curMenu->current->item.isFolder) {
                    if (folderDescend(support->mode, curMenu->current->item.text))
                        ioPutRequest(IO_MENU_UPDATE_DEFFERED, &support->mode);
                    return;
                }
                config_set_t *configSet = menuLoadConfig();
                if (curMenu->current == NULL)
                    return; // a deferred source refresh replaced the row while config IO was pending
                int launchId = curMenu->current->item.id;
                char gameIdStartup[128] = {0};
                char *startup = support->itemGetStartup(support, launchId);
                if (startup != NULL)
                    snprintf(gameIdStartup, sizeof(gameIdStartup), "%s", startup);
                // Flash the GameID barcode (Pixel FX/RetroGEM HDMI auto-profile) before handoff. Use
                // the stack copy: the hold renders/unlocks for many frames while source lists may refresh.
                guiShowGameID(gameIdStartup);
                support->itemLaunch(support, launchId, configSet);
            }
        } else {
            // If we're trying to enable BDM support we need to enable it for all BDM menu slots.
            if (support->mode == BDM_MODE) {
                // Initialize support for all bdm modules.
                for (int i = BDM_MODE; i <= BDM_MODE_LAST; i++) {
                    opl_io_module_t *mod = &list_support[i];
                    itemInitSupport(mod->support);
                }
            } else {
                // Normal initialization.
                itemInitSupport(support);
            }
        }
    } else
        guiMsgBox("NULL Support object. Please report", 0, NULL);
}

static void itemExecRefresh(struct menu_item *curMenu)
{
    item_list_t *support = curMenu->userdata;

    if (support && support->enabled) {
        // FAV has no device to rescan: loadFavourites() re-reads favourites.bin and schedules its
        // own rebuild. HDD's VCD view is intentionally cached across ordinary L3 flips, so an
        // explicit user Refresh must invalidate that cache before posting the deferred update.
        if (support->mode == FAV_MODE) {
            loadFavourites();
        } else {
            if (support->mode == HDD_MODE && vcdListViewActive(support))
                hddVcdInvalidateCache();
            ioPutRequest(IO_MENU_UPDATE_DEFFERED, &support->mode);
        }
        sfxPlay(SFX_CONFIRM);
    }
}

// L3: toggle the device's list between its disc games and its VCD (PS1-via-POPSTARTER) list. Only
// device classes that have a VCD view (vcdModeSupported) respond. vcdToggleView marks the mode
// dirty; the deferred update + the support's NeedsUpdate (vcdConsumeDirty) then force the rescan.
static void itemExecToggleView(struct menu_item *curMenu)
{
    item_list_t *support = curMenu->userdata;
    if (!support || !vcdModeSupported(support->mode))
        return;
    if (gDefaultGameView != GAME_VIEW_BOTH)
        return; // the global default-view setting locks the page to one type -> L3 is inert

    // Folder browsing: the VCD/POPS list has no folder tree, so drop any ISO-view subfolder position
    // back to root on a view toggle (the deferred rebuild below restores the plain device title).
    folderReset(support->mode);
    vcdToggleView(support->mode);

    // Every cover already queued belongs to the view being discarded, and none of them will be
    // cancelled on their own: the loader's per-row cancellation keys on a row having scrolled away,
    // and here nothing scrolls -- the old rows keep being drawn until the rebuild below lands, so
    // their stamps stay fresh. They are FIFO ahead of that rebuild and art shares the single ioman
    // worker with it, so without this the device has to serve every cover of the OLD list before the
    // NEW one can even be built. That is why the page took so long to become correct.
    cacheDropQueuedArt();

    sfxPlay(SFX_CONFIRM);
    guiWarning(vcdViewActive(support->mode) ? _l(_STR_VCD_ON) : _l(_STR_VCD_OFF), 2);
    ioPutRequest(IO_MENU_UPDATE_DEFFERED, &support->mode);
}

// Folder browsing: the cancel button (whichever of Cross/Circle is not Select) ascends a level.
static void itemFolderAscend(struct menu_item *curMenu)
{
    item_list_t *support = curMenu ? curMenu->userdata : NULL;
    if (support == NULL || folderDepth(support->mode) == 0)
        return;
    if (folderAscend(support->mode)) {
        sfxPlay(SFX_CANCEL);
        ioPutRequest(IO_MENU_UPDATE_DEFFERED, &support->mode);
    }
}

static void itemExecCross(struct menu_item *curMenu)
{
    if (gSelectButton == KEY_CROSS)
        itemExecSelect(curMenu);
    else
        itemFolderAscend(curMenu); // Cross is the cancel button here -> ascend a folder level
}

static void itemExecCircle(struct menu_item *curMenu)
{
    if (gSelectButton == KEY_CIRCLE)
        itemExecSelect(curMenu);
    else
        itemFolderAscend(curMenu); // Circle is the cancel button here -> ascend a folder level
}

static void itemExecSquare(struct menu_item *curMenu)
{
    // Folder browsing: a folder row has no info screen (#Size/#DiscType would stat a directory).
    if (curMenu->current && curMenu->current->item.isFolder)
        return;
    if (curMenu->current && gTheme->infoElems.first) {
        // #Size is skipped while scrolling so the badges paint instantly; resolve it now (async) --
        // but NEVER for a VCD (PS1) list. A VCD carries no meaningful #Size: vcdFillGameList tags the
        // entry .VCD/GAME_FORMAT_ISO (vcdsupport.c), so sbPopulateConfig stats the CD/DVD folder while
        // .VCDs actually live in POPS/ -- the stat always misses, game->sizeMB stays 0 and never
        // caches, so it re-runs on EVERY entry. That makes the resolve a pure redundant CFG re-read +
        // failing stat on the shared MMCE/fileXio channel whose only visible effect is raising the
        // busy spinner (Andrew, #120). Skipping it for VCD strictly REDUCES channel traffic and drops
        // no displayed data (the info page shows no size for a VCD anyway).
        item_list_t *support = curMenu->userdata;
        // Direct HDD/HDL rows already carry their size in APA metadata (total_size_in_kb), so the
        // generic CFG+stat pass is redundant there. VCD rows likewise have no meaningful #Size.
        // Avoid putting either no-op read onto the shared IO worker merely for opening Info.
        if (support == NULL || (!vcdListViewActive(support) && support->mode != HDD_MODE))
            menuRequestInfoSize();
        guiSwitchScreen(GUI_SCREEN_INFO);
    }
}

// R3: toggle the current row's favourite star (checklist item 33). On the Favourites tab
// itself R3 removes; on any source list it adds/removes by (mode, id, text).
static void itemExecFav(struct menu_item *curMenu)
{
    if (!gFAVStartMode) // Favourites disabled -> R3 is a no-op (no hidden writes)
        return;

    if (!curMenu->current)
        return;

    item_list_t *support = curMenu->userdata;
    if (!support)
        return;

    submenu_item_t *it = &curMenu->current->item;

    // Folder browsing: on a source list a folder row is not favouritable, and a game favourited
    // from INSIDE a subfolder would resolve by index against the wrong (root) view later --
    // suppress both. Root favourites are unchanged; the FAV tab's own removals are unaffected.
    if (support->mode != FAV_MODE && (it->isFolder || folderDepth(support->mode) > 0))
        return;

    if (support->mode == FAV_MODE) {
        favRemoveByIndex(it->id);
    } else {
        // A favourite captured while the device page is in its L3 VCD view is a PS1/.VCD favourite
        // (checklist item 12; the stub keeps isVcd at 0 until VCD views exist).
        int isVcd = vcdViewActive(support->mode) ? 1 : 0;
        // Only on a device whose VCD favourites can actually be resolved/launched later: storing one
        // on a device without itemLaunchVcd would leave a permanently-hidden, unlaunchable record.
        if (isVcd && support->itemLaunchVcd == NULL)
            return;
        if (it->favourited) {
            if (removeFavouriteByIdAndText(support->mode, it->id, it->text, isVcd))
                it->favourited = 0; // only clear the star once the store write succeeded
        } else {
            if (addFavouriteItem(support->mode, it->id, it->icon_id, it->text_id, it->text, isVcd))
                it->favourited = 1; // only show the star once the store write succeeded
        }
    }

    sfxPlay(SFX_CONFIRM);
    ioPutRequest(IO_CUSTOM_SIMPLEACTION, &loadFavourites);
}

static void itemExecTriangle(struct menu_item *curMenu)
{
    if (!curMenu->current)
        return;

    // Folder browsing: a folder row has no per-game settings menu.
    if (curMenu->current->item.isFolder)
        return;

    item_list_t *support = curMenu->userdata;

    if (support) {
        // A VCD is a POPSTARTER title, not a PS2-loader title. Route before the generic per-game
        // settings branch: that branch loads/creates CFG state and exposes controls that can never
        // affect a POPSTARTER handoff. vcdListViewActive also covers a forced-VCD Favourites proxy.
        if (vcdListViewActive(support)) {
            if (menuCheckParentalLock() == 0) {
                menuInitVcdMenu();
                guiSwitchScreen(GUI_SCREEN_APP_MENU);
            }
            return;
        }

        unsigned char flags = (support->mode == FAV_MODE) ? favGetFlags(support) : support->flags;

        if (!(flags & MODE_FLAG_NO_COMPAT)) {
            if (menuCheckParentalLock() == 0) {
                menuInitGameMenu();
                guiSwitchScreen(GUI_SCREEN_GAME_MENU);
                guiGameLoadConfig(support, gameMenuLoadConfig(NULL));
            }
        } else {
            if (menuCheckParentalLock() == 0 && gEnableWrite) {
                menuInitAppMenu();
                guiSwitchScreen(GUI_SCREEN_APP_MENU);
            }
        }
    } else
        guiMsgBox("NULL Support object. Please report", 0, NULL);
}

static void initMenuForListSupport(opl_io_module_t *mod)
{
    mod->menuItem.icon_id = mod->support->itemIconId(mod->support);
    mod->menuItem.text = NULL;
    mod->menuItem.text_id = mod->support->itemTextId(mod->support);
    mod->menuItem.visible = 1;

    mod->menuItem.userdata = mod->support;

    mod->subMenu = NULL;

    mod->menuItem.submenu = NULL;
    mod->menuItem.current = NULL;
    mod->menuItem.pagestart = NULL;
    mod->menuItem.last = NULL; // coverflow wrap tail
    mod->menuItem.remindLast = 0;

    mod->menuItem.refresh = &itemExecRefresh;
    mod->menuItem.execCross = &itemExecCross;
    mod->menuItem.execTriangle = &itemExecTriangle;
    mod->menuItem.execSquare = &itemExecSquare;
    mod->menuItem.execCircle = &itemExecCircle;
    mod->menuItem.fav = &itemExecFav;
    mod->menuItem.toggleView = &itemExecToggleView;

    mod->menuItem.hints = NULL;

    moduleUpdateMenuInternal(mod, 0, 0);

    struct gui_update_t *mc = guiOpCreate(GUI_OP_ADD_MENU);
    if (mc) { // guiOpCreate returns NULL on OOM -- skip the deferred op rather than deref NULL
        mc->menu.menu = &mod->menuItem;
        mc->menu.subMenu = &mod->subMenu;
        guiDeferUpdate(mc);
    }
}

static void clearMenuGameList(opl_io_module_t *mdl)
{
    if (mdl->subMenu != NULL) {
        // lock - gui has to be unused here
        guiLock();

        submenuDestroy(&mdl->subMenu);
        mdl->menuItem.submenu = NULL;
        mdl->menuItem.current = NULL;
        mdl->menuItem.pagestart = NULL;
        mdl->menuItem.last = NULL; // coverflow wrap tail (list clear/refresh must reset it)
        mdl->menuItem.remindLast = 0;

        // unlock
        guiUnlock();
    }
}

void initSupport(item_list_t *itemList, int mode, int force_reinit)
{
    opl_io_module_t *mod = &list_support[mode];

    // Set the start mode flag based on device type.
    int startMode = 0;
    if (mode >= BDM_MODE && mode < ETH_MODE)
        // Effective, not raw: a selected UDPBD/UDPFSBD network protocol floors BDM to Auto so its
        // hotplug tab can exist (see bdmEffectiveStartMode) -- the saved setting is untouched.
        startMode = bdmEffectiveStartMode();
    else if (mode == ETH_MODE)
        startMode = gETHStartMode;
    else if (mode == HDD_MODE)
        startMode = gHDDStartMode;
    else if (mode == APP_MODE)
        startMode = gAPPStartMode;
    else if (mode == FAV_MODE)
        startMode = gFAVStartMode;
    else if (mode == UDPFS_MODE)
        // UDPFS filesystem tab honours the network start row: Auto loads the udpfs IRX chain + mount at
        // boot, Manual defers it to tab-entry. Live only while its protocol is the selected one, so
        // exactly one network tab is ever enabled.
        startMode = (gNetworkProtocol == NET_PROTO_UDPFS) ? gNetStartMode : START_MODE_DISABLED;
    else if (mode == MMCE_MODE)
        startMode = gMMCEStartMode;

    if (startMode) {
        if (!mod->support) {
            mod->support = itemList;
            mod->support->owner = mod;
            initMenuForListSupport(mod);

            // First-ever tab while the GUI sits on the start menu (fresh/default config, every
            // mode OFF): take the user TO the new tab, or they stay on the "default interface"
            // with no visible way to it (#254 follow-up: "Apps section does not appear in the
            // default interface even if enabled, until a theme is applied" -- the theme apply was
            // just the thing that returned them to the tab screen). Boot is unaffected:
            // deferredInit's default-device select queues AFTER this one and wins; multi-mode
            // enables land on the last registered tab.
            if (!menuHasRegisteredItems()) {
                struct gui_update_t *sel = guiOpCreate(GUI_OP_SELECT_MENU);
                if (sel) {
                    sel->menu.menu = &mod->menuItem;
                    guiDeferUpdate(sel);
                }
            }
        } else {
            // Re-enable after a prior disable: the support + its menu item already exist (registered on
            // an earlier enable), so the !mod->support branch above -- the ONLY place that sets
            // menuItem.visible = 1 -- is skipped. Disabling a mode (else branch below) set visible = 0
            // but LEFT mod->support non-NULL, so without restoring it here a tab that was ever toggled
            // off stays hidden for the rest of the session even when re-selected. This is why picking a
            // network protocol after having switched away from it showed no tab. BDM device tabs escape
            // this via bdmNeedsUpdate's per-refresh visibility (and bdmInitDevicesData overrides it on
            // its own path); ETH/UDPFS/APP/FAV/HDD have no such hook, so restore it here.
            mod->menuItem.visible = 1;
        }

        if (((force_reinit) && (mod->support->enabled)) || (startMode == START_MODE_AUTO && !mod->support->enabled)) {
            mod->support->itemInit(mod->support);
            moduleUpdateMenuInternal(mod, 0, 0);

            ioPutRequest(IO_MENU_UPDATE_DEFFERED, &list_support[mode].support->mode); // can't use mode as the variable will die at end of execution
        }
    } else {
        // If the module has a valid menu instance try to refresh the visibility state.
        mod->menuItem.visible = 0;
    }
}

// Boot-splash status (#297): true only across init()'s synchronous boot device-load, so the
// in-initAllSupport greeting redraws fire ONLY during boot -- not on a post-boot settings refresh
// (which runs on the IO thread with the menu showing; an ungated redraw would flash the boot logo).
static int gBootInProgress = 0;

int oplIsBootInProgress(void)
{
    return gBootInProgress;
}

static void initAllSupport(int force_reinit)
{
    guiSetBootStatus(_l(_STR_BOOT_SCANNING_BDM));
    if (gBootInProgress)
        guiRenderGreetingScreen();
    bdmEnumerateDevices();
    guiSetBootStatus(_l(_STR_BOOT_SCANNING_NET));
    if (gBootInProgress)
        guiRenderGreetingScreen();
    initSupport(ethGetObject(0), ETH_MODE, force_reinit || (gNetworkStartup >= ERROR_ETH_SMB_CONN));
    // UDPFS filesystem shares the single NIC with SMB/UDPBD; its start-mode gate (initSupport) is live
    // only when gNetworkProtocol == NET_PROTO_UDPFS, so exactly one network tab is ever enabled.
    initSupport(udpfsGetObject(0), UDPFS_MODE, force_reinit);
    guiSetBootStatus(_l(_STR_BOOT_SCANNING_HDD));
    if (gBootInProgress)
        guiRenderGreetingScreen();
    initSupport(hddGetObject(0), HDD_MODE, force_reinit);
    initSupport(appGetObject(0), APP_MODE, force_reinit);
    initSupport(favGetObject(0), FAV_MODE, force_reinit);
    LOG("BOOT scan: bdmEnumerateDevices() done; MMCE initSupport begin\n");
    // Distinct banner for the MMCE init phase so a frozen boot screen LOCALIZES a scan-hang to this
    // step. Helps distinguish between the 4-probe presence check against a genuinely empty card slot
    // on a FAT console and the exact culprit (slow ATA/dev9 probe in bdmEnumerateDevices vs the MMCE
    // presence poll in mmceman).
    initSupport(mmceGetObject(0), MMCE_MODE, force_reinit);
    LOG("BOOT scan: MMCE initSupport done\n");

    // Arm the MMCE GameID transport at boot and on every settings apply -- instead
    // of deferring it to itemLaunchMMCE.
    if (gMMCEEnableGameID && !gBootInProgress)
        mmceArmGameIDTransport();
}

static void deinitAllSupport(int exception, int modeSelected)
{
    for (int i = 0; i < MODE_COUNT; i++) {
        if (list_support[i].support != NULL)
            moduleCleanup(&list_support[i], exception, modeSelected);
    }
}

void oplQueueVcdDeviceUpdates(void)
{
    // vcdMarkAllDirty() is intentionally side-effect-free because it also runs during early config
    // loading, before the IO worker and device modules are ready. Runtime callers must explicitly
    // enqueue the enabled pages. This matters most for HDD, whose updateDelay=-1 means a dirty view
    // otherwise keeps displaying the old submenu indefinitely while rendering uses the new view.
    for (int i = 0; i < MODE_COUNT; i++) {
        item_list_t *support = list_support[i].support;
        if (support != NULL && support->enabled && support->mode != FAV_MODE && vcdModeSupported(support->mode))
            ioPutRequest(IO_MENU_UPDATE_DEFFERED, &support->mode);
    }
}

// For resolving the mode, given an app's path
int oplPath2Mode(const char *path)
{
    char appsPath[64];
    const char *blkdevnameend;
    int i, blkdevnamelen;
    item_list_t *listSupport;

    for (i = 0; i < MODE_COUNT; i++) {
        listSupport = list_support[i].support;
        if ((listSupport != NULL) && (listSupport->itemGetPrefix != NULL)) {
            char *prefix = listSupport->itemGetPrefix(listSupport);
            snprintf(appsPath, sizeof(appsPath), "%sAPPS", prefix);

            blkdevnameend = strchr(appsPath, ':');
            if (blkdevnameend != NULL) {
                blkdevnamelen = (int)(blkdevnameend - appsPath);

                if (strncmp(path, appsPath, blkdevnamelen) == 0)
                    return listSupport->mode;
            }
        }
    }

    return -1;
}

// Art lookup addressed by IO MODE rather than by a device string. appsupport resolves each app's art
// source to a mode once at scan time (appSetArtSource), so the per-cover lookup does not have to
// re-derive it from a path on every request.
int oplGetAppImageByMode(int mode, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)
{
    item_list_t *listSupport;

    if (mode < 0 || mode >= MODE_COUNT)
        return -1;

    listSupport = list_support[mode].support;
    if ((listSupport != NULL) && (listSupport->enabled))
        return listSupport->itemGetImage(listSupport, folder, isRelative, value, suffix, resultTex, psm);

    return -1;
}

int oplGetAppImage(const char *device, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)
{
    int i, remaining, elfbootmode;
    char priority;
    item_list_t *listSupport;

    elfbootmode = -1;
    if (device != NULL) {
        elfbootmode = oplPath2Mode(device);
        if (elfbootmode >= 0) {
            listSupport = list_support[elfbootmode].support;

            if ((listSupport != NULL) && (listSupport->enabled)) {
                if (listSupport->itemGetImage(listSupport, folder, isRelative, value, suffix, resultTex, psm) >= 0)
                    return 0;
            }
        }
    }

    // We search on ever devices from fatest to slowest.
    for (remaining = MODE_COUNT, priority = 0; remaining > 0 && priority < 4; priority++) {
        for (i = 0; i < MODE_COUNT; i++) {
            listSupport = list_support[i].support;

            if (i == elfbootmode)
                continue;

            if ((listSupport != NULL) && (listSupport->enabled) && (listSupport->appsPriority == priority)) {
                if (listSupport->itemGetImage(listSupport, folder, isRelative, value, suffix, resultTex, psm) >= 0)
                    return 0;
                remaining--;
            }
        }
    }

    return -1;
}

static int scanApps(int (*callback)(const char *path, config_set_t *appConfig, void *arg), void *arg, char *appsPath, int exception)
{
    struct dirent *pdirent;
    DIR *pdir;
    int count, ret;
    config_set_t *appConfig;
    char dir[128];
    char path[128];

    count = 0;
    if ((pdir = opendir(appsPath)) != NULL) {
        while ((pdirent = readdir(pdir)) != NULL) {
            if (exception && strchr(pdirent->d_name, '_') == NULL)
                continue;

            if (strcmp(pdirent->d_name, ".") == 0 || strcmp(pdirent->d_name, "..") == 0)
                continue;

            snprintf(dir, sizeof(dir), "%s/%s", appsPath, pdirent->d_name);
            if (pdirent->d_type != DT_DIR)
                continue;

            snprintf(path, sizeof(path), "%s/%s", dir, APP_TITLE_CONFIG_FILE);
            appConfig = configAlloc(0, NULL, path);
            if (appConfig != NULL) {
                configRead(appConfig);

                ret = callback(dir, appConfig, arg);
                configFree(appConfig);

                if (ret == 0)
                    count++;
                else if (ret < 0) { // Stopped because of unrecoverable error.
                    break;
                }
            }
        }

        closedir(pdir);
    } else
        LOG("APPS failed to open dir %s\n", appsPath);

    return count;
}

int oplScanApps(int (*callback)(const char *path, config_set_t *appConfig, void *arg), void *arg)
{
    int i, count;
    item_list_t *listSupport;
    char appsPath[64];

    count = 0;
    for (i = 0; i < MODE_COUNT; i++) {
        listSupport = list_support[i].support;
        if ((listSupport != NULL) && (listSupport->enabled) && (listSupport->itemGetPrefix != NULL)) {
            char *prefix = listSupport->itemGetPrefix(listSupport);
            /*
              #253: never scan a DEVICE-LESS prefix.

              An enabled-but-unmounted slot (a BDM index with nothing plugged in, ETH with no
              share) returns an EMPTY prefix. "%sAPPS" then yields the bare relative path "APPS",
              which the PS2 resolves against the CWD -- OPL's own boot folder. When OPL boots from
              a device root (PixeliGer's case: USB), that is the SAME directory the real mass0:
              prefix already scanned, so every app was discovered twice and appeared twice in the
              list.

              It also explains why dedup did not help: the two hits arrive with different path
              strings ("mass0:APPS/..." vs "APPS/..."), so they are legitimately distinct entries
              as far as the dedup key is concerned.
            */
            if (prefix == NULL || prefix[0] == '\0')
                continue;
            snprintf(appsPath, sizeof(appsPath), "%sAPPS", prefix);
            count += scanApps(callback, arg, appsPath, 0);
        }
    }

    for (i = 0; i < 2; i++) {
        snprintf(appsPath, sizeof(appsPath), "mc%d:", i);
        count += scanApps(callback, arg, appsPath, 1);
    }

    return count;
}

int oplShouldAppsUpdate(void)
{
    int result;

    result = (int)shouldAppsUpdate;
    shouldAppsUpdate = 0;

    return result;
}

config_set_t *oplGetLegacyAppsConfig(void)
{
    int i, fd;
    item_list_t *listSupport;
    config_set_t *appConfig;
    char appsPath[128];

    snprintf(appsPath, sizeof(appsPath), "mc?:OPL/conf_apps.cfg");
    fd = openFile(appsPath, O_RDONLY);
    if (fd >= 0) {
        appConfig = configAlloc(CONFIG_APPS, NULL, appsPath);
        close(fd);
        return appConfig;
    }

    for (i = MODE_COUNT - 1; i >= 0; i--) {
        listSupport = list_support[i].support;
        if ((listSupport != NULL) && (listSupport->enabled) && (listSupport->itemGetPrefix != NULL)) {
            char *prefix = listSupport->itemGetPrefix(listSupport);
            if (prefix == NULL || prefix[0] == '\0')
                continue;
            snprintf(appsPath, sizeof(appsPath), "%sconf_apps.cfg", prefix);

            fd = openFile(appsPath, O_RDONLY);
            if (fd >= 0) {
                appConfig = configAlloc(CONFIG_APPS, NULL, appsPath);
                close(fd);
                return appConfig;
            }
        }
    }

    /* Apps config not found on any device, go with last tested device.
       Does not matter if the config file could be loaded or not */
    appConfig = configAlloc(CONFIG_APPS, NULL, appsPath);

    return appConfig;
}

config_set_t *oplGetLegacyAppsInfo(char *name)
{
    int i, fd;
    item_list_t *listSupport;
    config_set_t *appConfig;
    char appsPath[128] = {0};

    for (i = MODE_COUNT - 1; i >= 0; i--) {
        listSupport = list_support[i].support;
        if ((listSupport != NULL) && (listSupport->enabled) && (listSupport->itemGetPrefix != NULL)) {
            char *prefix = listSupport->itemGetPrefix(listSupport);
            if (prefix == NULL || prefix[0] == '\0')
                continue;
            snprintf(appsPath, sizeof(appsPath), "%sCFG%s%s.cfg", prefix, i == ETH_MODE ? "\\" : "/", name);

            fd = openFile(appsPath, O_RDONLY);
            if (fd >= 0) {
                appConfig = configAlloc(0, NULL, appsPath);
                close(fd);
                return appConfig;
            }
        }
    }

    /* Apps config not found on any device, go with the last tested device when one had a
       real prefix. A valid list-only APA session can have no persistent PFS prefix at all; in that
       state return metadata-only config instead of inventing a path on another device. */
    appConfig = configAlloc(0, NULL, appsPath[0] != '\0' ? appsPath : NULL);

    return appConfig;
}

// ----------------------------------------------------------
// ----------------------- Updaters -------------------------
// ----------------------------------------------------------
static void updateMenuFromGameList(opl_io_module_t *mdl)
{
    guiExecDeferredOps();
    clearMenuGameList(mdl);

    // NO cacheInvalidateFailMemo() HERE, and this is deliberate -- it used to sit on this line.
    //
    // The reasoning was "a rebuilt list is the natural retry point for art that failed earlier". The
    // flaw is what rebuilds a list: not just the user changing view, but every background device
    // rescan that lands while they browse. So the memo was thrown away every few seconds and a game
    // with no cover was re-probed forever, which is the expensive case -- a lookup for a file that
    // does not exist must walk the whole directory before it can answer.
    //
    // The retry now hangs off the event that can actually make absent art exist: a device generation
    // change (hotplug, or a Device Settings apply) in menuUpdateHook, plus the deliberate
    // applyConfig invalidation. master keeps only the latter; ours keeps the hotplug case too, so
    // plugging in a stick with new art still picks it up without a reboot.

    const char *temp = NULL;
    if (gRememberLastPlayed)
        configGetStr(configGetByType(CONFIG_LAST), "last_played", &temp);

    // refresh device icon and text (for bdm)
    mdl->menuItem.icon_id = mdl->support->itemIconId(mdl->support);
    mdl->menuItem.text_id = mdl->support->itemTextId(mdl->support);

    // read the new game list
    struct gui_update_t *gup = NULL;
    int count = mdl->support->itemUpdate(mdl->support);

    // Folder browsing: while inside a subfolder, show the breadcrumb ("Device: RPGs/SNES") as the page
    // title. folderGetSub() points at persistent static state, so the char* stays valid. At the device
    // root the device name set above stands. Only one device is ever inside a folder at a time (the
    // browse state resets to root on device switch), so a single static crumb buffer is safe.
    const int folderMode = folderModeSupported(mdl->support->mode);
    if (folderMode && folderDepth(mdl->support->mode) > 0) {
        static char folderCrumb[256]; // generous headroom for a localized device name + subpath
        snprintf(folderCrumb, sizeof(folderCrumb), "%s: %s", _l(mdl->support->itemTextId(mdl->support)), folderGetSub(mdl->support->mode));
        mdl->menuItem.text = folderCrumb;
        mdl->menuItem.text_id = -1;
    }

    if (count > 0) {
        // Folder browsing: emit in TWO passes -- folders first, then the games -- so folders always sit
        // at the TOP of the list regardless of the Auto Sort setting. Each row keeps id=i (its array
        // index), so favourites still resolve by id+text no matter the display order. Devices that never
        // produce folder rows use a single pass, byte-for-byte the original behaviour. When Auto Sort is
        // on, submenuSort's folder-first comparator keeps this grouping while sorting each group.
        int passes = (gEnableFolderNav && folderMode) ? 2 : 1;
        int pass, i;

        for (pass = 0; pass < passes; ++pass) {
            for (i = 0; i < count; ++i) {
                // Flag folder rows so the dispatch descends and the renderer marks them. Only the
                // loose-file tree devices return base_game_info_t from itemGet, so gate on the mode.
                int isFolderRow = 0;
                if (folderMode && mdl->support->itemGet != NULL) {
                    base_game_info_t *ginfo = (base_game_info_t *)mdl->support->itemGet(mdl->support, i);
                    isFolderRow = (ginfo != NULL && ginfo->format == GAME_FORMAT_FOLDER);
                }
                // Pass 0 emits folders, pass 1 emits games (single-pass mode emits every row).
                if (passes == 2 && ((pass == 0) != (isFolderRow != 0)))
                    continue;

                gup = guiOpCreate(GUI_OP_APPEND_MENU);
                if (!gup) // OOM: skip this entry rather than deref NULL
                    continue;

                gup->menu.menu = &mdl->menuItem;
                gup->menu.subMenu = &mdl->subMenu;

                gup->submenu.icon_id = -1;
                gup->submenu.id = i;
                gup->submenu.text = mdl->support->itemGetName(mdl->support, i);
                gup->submenu.text_id = -1;
                gup->submenu.selected = 0;
                gup->submenu.isFolder = isFolderRow;

                // Last-played auto-select never targets a folder row (its startup is empty).
                if (gRememberLastPlayed && temp && !isFolderRow && strcmp(temp, mdl->support->itemGetStartup(mdl->support, i)) == 0) {
                    gup->submenu.selected = 1; // Select Last Played Game
                }

                guiDeferUpdate(gup);
            }
        }
    }

    if (gAutosort) {
        gup = guiOpCreate(GUI_OP_SORT);
        if (gup) { // OOM: an unsorted list beats dereferencing NULL, same as the append loop above
            gup->menu.menu = &mdl->menuItem;
            gup->menu.subMenu = &mdl->subMenu;
            guiDeferUpdate(gup);
        }
    }
}

void menuDeferredUpdate(void *data)
{
    short int *mode = data;

    opl_io_module_t *mod = &list_support[*mode];
    if (!mod->support)
        return;

    // see if we have to update
    if (mod->support->itemNeedsUpdate(mod->support)) {
        updateMenuFromGameList(mod);

        // If other modes have been updated, then the apps list should be updated too.
        // Exclude FAV: a FAV rebuild marking apps dirty would re-trigger the FAV resync below
        // (an apps refresh calls loadFavourites), looping forever once favNeedsUpdate fires.
        if (mod->support->mode != APP_MODE && mod->support->mode != FAV_MODE)
            shouldAppsUpdate = 1;

        // A source-list refresh may expose newly-loaded items to validate favourites
        // against. Re-sync the FAV tab (cheap/idempotent; skipped when FAV is disabled).
        if (gFAVStartMode && mod->support->mode != FAV_MODE)
            loadFavourites();
    }
}

#define MENU_GENERAL_UPDATE_DELAY          60
// Minimum wall-clock gap between background rescans of the SAME updateDelay==0 device (Fix B). At
// ~2 s this drops the steady SIO2/mass enumeration rate well below the old every-60-frames cadence,
// cutting MX4SIO bus contention, while a real device change still refreshes immediately
// (BdmGeneration bypass below). clock() = microseconds.
#define MENU_BG_RESCAN_MIN_INTERVAL_TICKS  (2 * CLOCKS_PER_SEC)
/*
  Idle time required before STEADY-STATE background probes may run at all, in frames (~1 s NTSC).

  The short gate (MENU_MIN_INACTIVE_FRAMES = 8, ~133 ms) is tuned for "the user paused between
  inputs" -- right for waking cover-art loads, and WRONG for device probes. Frame analysis of a
  tester capture (#271) showed why: tapping through the list at a steady ~4.3 taps/s leaves
  ~130 ms release gaps, which sit exactly AT the short gate, so throttled probes leaked into
  active navigation and a probe firing into a tap gap contends with the pads on SIO2 -- the tap
  landing inside the resulting read-miss burst is eaten.

  60 frames of REAL idleness is far above any tap gap, so probes never fire mid-navigation, and a
  parked console still probes within a second of the user stopping. Hotplug stays immediate-ish:
  the BdmGeneration bypass below keeps the SHORT gate, so a plugged device is picked up in the
  next tap gap rather than after a full second.
*/
#define MENU_BG_RESCAN_MIN_INACTIVE_FRAMES 60

static void menuUpdateHook()
{
    int i;

    // if timer exceeds some threshold, schedule updates of the available input sources
    frameCounter++;

    // Keep background refresh work out of the shared IO queue while the user is actively navigating.
    if (guiInactiveFrames < MENU_MIN_INACTIVE_FRAMES)
        return;

    // Let the current queue drain before adding background refresh work.
    if (ioHasPendingRequests())
        return;

    // Yield to cover art still on its way. The rescans below enqueue one device probe per BDM page
    // onto the SAME single IO worker the art loads use, so without this gate a settle puts a batch
    // of probes AHEAD of the covers the user is looking at -- and since the gate opens again on the
    // next idle window, every scroll-and-stop cycle adds another batch and the art falls further
    // behind (hardware: ~15 s to fill after a few cycles). Device detection loses nothing: this
    // only defers a periodic rescan by the length of the art queue, and a real hotplug still comes
    // through BdmGeneration below once art is idle. Matches the fork.
    if (cacheHasPendingArt())
        return;

    // Steady-state probes additionally require REAL idleness (see the define): a tap gap passes the
    // short gate above, and a probe fired into it contends with the pads on SIO2 and eats the next
    // tap (#271). Event-driven work (the genChanged hotplug bypass below) keeps the short gate.
    int longIdle = (guiInactiveFrames >= MENU_BG_RESCAN_MIN_INACTIVE_FRAMES);

    // schedule updates of all the list handlers
    // Periodic background rescans. Both steady-state rescans enqueue standard requests and are
    // rendered with the unified busy overlay.
    if (gAutoRefresh && longIdle) {
        for (i = 0; i < MODE_COUNT; i++) {
            if ((list_support[i].support && list_support[i].support->enabled) && ((list_support[i].support->updateDelay > 0) && (frameCounter % list_support[i].support->updateDelay == 0)))
                ioPutRequest(IO_MENU_UPDATE_DEFFERED, &list_support[i].support->mode);
        }
    }

    // Schedule updates of the every-frame (updateDelay==0) list handlers -- all BDM/MX4SIO. These
    // enumerate the SIO2/mass bus, so throttle each to a minimum wall-clock interval instead of
    // firing every MENU_GENERAL_UPDATE_DELAY frames. A genuine BDM device change (hotplug/removal
    // via BdmGeneration, or a Device-Settings apply) bypasses the throttle so detection stays
    // immediate.
    if (frameCounter % MENU_GENERAL_UPDATE_DELAY == 0) {
        unsigned int gen = bdmGetGeneration();
        int genChanged = (gen != lastSeenBdmGeneration);
        clock_t now = clock();

        // "Storage changed underneath us" is the ONLY routine event that can make previously-absent
        // art exist, so it is the only routine event that should re-open the question. The memo used
        // to be cleared by updateMenuFromGameList instead -- i.e. on every list rebuild, including the
        // background rescans that fire while the user simply browses -- which meant a missing cover
        // was re-probed over and over forever. That is the expensive direction: a lookup for a file
        // that is not there has to walk the whole directory before it can say so, and this build
        // measured art loads spanning 82 ms to 2922 ms with the decoded size held constant.
        if (genChanged) {
            cacheInvalidateFailMemo();
            artIndexInvalidate();   // the listing is only as current as the device it was read from
            vcdInvalidateGameIds(); // ...and neither is a PS1 disc id read off the old media
        }
        // Consume the generation bump only if every hotplug-driven enqueue is ACCEPTED:
        // ioPutRequest can fail (queue blocked during teardown, allocation failure), and
        // committing lastSeenBdmGeneration up front would silently eat the one immediate-rescan
        // event a hotplug gets. Leaving the generation unconsumed makes the next 1 s tick retry
        // the whole event instead.
        int genConsumed = 1;
        for (i = 0; i < MODE_COUNT; i++) {
            if ((list_support[i].support && list_support[i].support->enabled) && (list_support[i].support->updateDelay == 0)) {
                int mode = list_support[i].support->mode;
                // elapsed form is single-wrap-safe; zero-initialized timestamp allows one immediate rescan.
                // genChanged (hotplug / Device-Settings apply) deliberately requires NEITHER longIdle:
                // it fires precisely BECAUSE a device changed, the scan populates a page the
                // user is waiting on, and a plugged device should be noticed in the
                // next tap gap rather than after a second of stillness (#271).
                // Slots that HAVE a device are probed every tick -- that poll is what notices a
                // removal, and thinning it (rebuild-96) is how a disconnect went unseen and the BDM
                // stack later vanished. Slots that have NEVER held a device are probed far less
                // often, because that probe is only a net for a missed attach event (attach is
                // event-driven via bdmEventHandler, and a generation bump bypasses this gate
                // entirely).
                //
                // Why it matters, measured: the hardware HUD showed "Q12 A0" -- twelve cover loads
                // queued and NONE executing, i.e. the single IO worker was inside something else
                // while the art waited. That something is this batch: eight fileXioDopen round
                // trips per tick on a rig with one stick, seven of them for slots that have been
                // empty since boot. It is also the most plausible source of the spurious
                // "device replugged" the user sees while simply navigating, since those probes poke
                // the USB stack for devices that are not there.
                if (!genChanged && mode >= BDM_MODE && mode <= BDM_MODE_LAST && !bdmSlotEverConnected(mode) &&
                    (frameCounter % (MENU_GENERAL_UPDATE_DELAY * BDM_EMPTY_SLOT_PROBE_RATIO)) != 0)
                    continue;

                if (genChanged || (longIdle && (now - lastBgRescan[mode]) >= MENU_BG_RESCAN_MIN_INTERVAL_TICKS)) {
                    // Stamp the throttle only on an accepted request, so a rejected one retries on
                    // the next tick instead of being silently skipped for a full interval.
                    int accepted = (ioPutRequest(IO_MENU_UPDATE_DEFFERED, &list_support[i].support->mode) == IO_OK);
                    if (accepted)
                        lastBgRescan[mode] = now;
                    else if (genChanged)
                        genConsumed = 0;
                }
            }
        }
        if (genConsumed)
            lastSeenBdmGeneration = gen;
    }
}

static void clearErrorMessage(void)
{
    // reset the original frame hook
    frameCounter = 0;
    errorMessageStringId = -1;
    guiSetFrameHook(&menuUpdateHook);
}

static void errorMessageHook()
{
    guiMsgBox(errorMessage, 0, NULL);
    clearErrorMessage();
}

// Language strings can choose the order of the supported substitutions, but are never handed to
// printf directly. Unknown conversion sequences remain literal text instead of consuming an
// argument that this caller did not provide.
static void formatErrorMessage(const char *format, const char *path, int error, int allowPath, int allowError)
{
    char errorText[16];
    size_t used = 0;

    if (format == NULL) {
        errorMessage[0] = '\0';
        return;
    }

    snprintf(errorText, sizeof(errorText), "%d", error);
    while (*format != '\0' && used < sizeof(errorMessage) - 1) {
        const char *replacement = NULL;

        if (*format != '%') {
            errorMessage[used++] = *format++;
            continue;
        }

        format++;
        if (*format == '\0') {
            errorMessage[used++] = '%';
            break;
        }
        if (*format == '%') {
            errorMessage[used++] = *format++;
            continue;
        }
        if (*format == 's' && allowPath)
            replacement = path ? path : "";
        else if ((*format == 'd' || *format == 'i') && allowError)
            replacement = errorText;

        if (replacement != NULL) {
            format++;
            while (*replacement != '\0' && used < sizeof(errorMessage) - 1)
                errorMessage[used++] = *replacement++;
        } else {
            errorMessage[used++] = '%';
            if (used < sizeof(errorMessage) - 1)
                errorMessage[used++] = *format;
            format++;
        }
    }
    errorMessage[used] = '\0';
}

void setErrorMessageWithCode(int strId, int error)
{
    formatErrorMessage(_l(strId), NULL, error, 0, 1);
    errorMessageStringId = strId;
    guiSetFrameHook(&errorMessageHook);
}

// The detail suffix is an internal enum name ("PS2FS_LOAD_FAILURE"), untranslated by construction,
// so a plain release gets the localized message alone and behaves exactly like setErrorMessageWithCode.
// OPLDIAG=1 appends the reason so a tester's screenshot names WHICH probe failed, not just the code.
void setErrorMessageWithCodeAndDetail(int strId, int error, const char *detail)
{
#ifdef __OPLDIAG
    size_t used;
#else
    (void)detail;
#endif

    formatErrorMessage(_l(strId), NULL, error, 0, 1);
#ifdef __OPLDIAG
    used = strlen(errorMessage);
    if (detail != NULL && detail[0] != '\0' && used < sizeof(errorMessage) - 1)
        snprintf(&errorMessage[used], sizeof(errorMessage) - used, "\nDiagnostic reason: %s", detail);
#endif
    errorMessageStringId = strId;
    guiSetFrameHook(&errorMessageHook);
}

void setErrorMessage(int strId)
{
    formatErrorMessage(_l(strId), NULL, 0, 0, 0);
    errorMessageStringId = strId;
    guiSetFrameHook(&errorMessageHook);
}

void setErrorMessagePathCode(int strId, const char *path, int error)
{
    formatErrorMessage(_l(strId), path, error, 1, 1);
    errorMessageStringId = strId;
    guiSetFrameHook(&errorMessageHook);
}

void clearErrorMessageIf(int strId)
{
    if (errorMessageStringId == strId)
        clearErrorMessage();
}

// ----------------------------------------------------------
// ------------------ Configuration handling ----------------
// ----------------------------------------------------------

static int lscstatus = CONFIG_ALL;
static int lscret = 0;
static char gLastSaveTarget[sizeof(gCustomSettingsPath)];
static int gLastSaveWasStagedOplHome = 0;
static int gHddSettingsFallbackNotice = 0;
static int gBootHddCommonFallback = 0;

static int checkLoadConfigMMCE(int types)
{
    int value;
    DIR *dir = opendir("mmce0:");
    if (dir != NULL) {
        closedir(dir);
        configEnd();
        configInit("mmce0:");
        value = configReadMulti(types);
        if (value & CONFIG_OPL) {
            config_set_t *configOPL = configGetByType(CONFIG_OPL);
            configSetInt(configOPL, CONFIG_OPL_MMCE_MODE, START_MODE_AUTO);
            return value;
        }
    }

    dir = opendir("mmce1:");
    if (dir != NULL) {
        closedir(dir);
        configEnd();
        configInit("mmce1:");
        value = configReadMulti(types);
        if (value & CONFIG_OPL) {
            config_set_t *configOPL = configGetByType(CONFIG_OPL);
            configSetInt(configOPL, CONFIG_OPL_MMCE_MODE, START_MODE_AUTO);
            return value;
        }
    }

    return 0;
}

static int checkLoadConfigBDM(int types)
{
    char path[64];
    int value;
    int bdm_result;
    int is_hdd = 0;

    // Probe the CURRENT settings filename, then the legacy one, so an existing install is still
    // discovered (read-fallback migration; the next save rewrites it under the current name).
    // These MUST be the CONFIG_OPL_FILENAME macros: config.c writes the file through them, so a
    // hardcoded name here silently searches for a file this build never creates.
    // check USB
    bdm_result = bdmFindPartition(path, CONFIG_OPL_FILENAME, 0);
    if (!bdm_result)
        bdm_result = bdmFindPartition(path, CONFIG_OPL_FILENAME_LEGACY, 0);
    // if not on USB, check BDM HDD
    if (bdm_result == 0) {
        // wait for up to 5 seconds for the HDD to spin up and become accessible...
        if (hddLoadModules() >= 0 && bdmHDDIsPresent(5000)) {
            bdm_result = bdmFindPartition(path, CONFIG_OPL_FILENAME, 0);
            if (!bdm_result)
                bdm_result = bdmFindPartition(path, CONFIG_OPL_FILENAME_LEGACY, 0);
            if (bdm_result)
                is_hdd = 1;
        }
    }

    if (bdm_result) {
        configEnd();
        configInit(path);
        value = configReadMulti(types);
        config_set_t *configOPL = configGetByType(CONFIG_OPL);
        configSetInt(configOPL, CONFIG_OPL_BDM_MODE, START_MODE_AUTO);
        if (is_hdd != 0) {
            gEnableBdmHDD = 1;
            configSetInt(configOPL, CONFIG_OPL_ENABLE_BDMHDD, gEnableBdmHDD);
        }
        return value;
    }

    return 0;
}

static int checkLoadConfigHDD(int types)
{
    int value;
    char path[64];

    hddLoadModules();
    hddLoadSupportModules();
    if (gHDDPrefix == NULL) {
        LOG("CONFIG HDD load skipped: no existing PFS data home is mounted\n");
        return 0;
    }

    snprintf(path, sizeof(path), "%s%s", gHDDPrefix, CONFIG_OPL_FILENAME);
    value = open(path, O_RDONLY);
    if (value < 0) {
        // Legacy fallback so an existing conf_riptopl.cfg install is still found (auto-migrates on save).
        snprintf(path, sizeof(path), "%s%s", gHDDPrefix, CONFIG_OPL_FILENAME_LEGACY);
        value = open(path, O_RDONLY);
    }
    if (value >= 0) {
        close(value);
        configEnd();
        configInit(gHDDPrefix);
        value = configReadMulti(types);
        config_set_t *configOPL = configGetByType(CONFIG_OPL);
        configSetInt(configOPL, CONFIG_OPL_HDD_MODE, START_MODE_AUTO);
        return value;
    }

    return 0;
}

// The boot device's filesystem cannot exist until AFTER the config has been read, because bringing
// its transport up needs values THAT LIVE IN THAT CONFIG -- the PS2's static IP for every network
// transport, plus server/share/user/password for SMB. That is a real bootstrap cycle, unlike a BDM
// boot where loading a driver needs no configuration at all.
//
// Set from the boot TOKEN by resolveBootDirToMass(). Deliberately NOT named "is a network boot":
// UDPBD and UDPFS-BD are BLOCK devices that mount as massN: exactly like USB, and their "udp" driver
// token is unreadable until the transport is up -- so those boots are indistinguishable from a local
// one here and correctly leave this at 0. See the classification branch for what that costs.
static int gBootHomeDeferred = 0;
// Set for an APA/PFS launch identity, even when the first persistent-home mount attempt fails.
// Used after config read because a stored HDD=Disabled value must not turn off the transport OPL
// itself was launched from, and by discovery to prohibit an unrelated MC write fallback.
static int gBootHomeApa = 0;

// Basename of the ELF OPL was booted as (argv[0]); pairs with gBootDir. The BDM resolver uses it
// to verify typed launch aliases and a legacy bare mass: token. An explicit massN: is already a
// concrete slot and is identified from that slot's own ioctls instead. Empty when argv[0] had no
// filename part or the boot dir came from getcwd().
static char gBootElfName[64];
// BDM_TYPE_* of the resolved boot device, BDM_TYPE_UNKNOWN for an unclassified boot. The save-path
// retry pins a later re-resolve to this type; config recovery also uses it to keep a known BDM boot
// from probing unrelated storage classes.
static int gBootDirBdmType = BDM_TYPE_UNKNOWN;
// True for a typed BDM boot or a concrete massN: token, even when the latter has not finished
// registering and therefore has no driver classification yet. This keeps a missing first-run file
// on that explicit slot from triggering the legacy all-device recovery scan.
static int gBootHomeBdm = 0;

// The built-in themes have no theme directory. Their conventional BGM belongs to the normal OPL
// data home, never POPS: `pfs0:OPL/THM/bgm.ogg` for __common/OPL and `pfs0:THM/bgm.ogg` for +OPL.
// Every non-APA home is the exact RiptOPL working directory resolved at boot. Do not turn this into
// a directory scan: a missing file is simply silence.
int oplGetDefaultThemeBgmPath(char *path, int pathSize)
{
    int length;

    if (path == NULL || pathSize <= 1)
        return 0;

    path[0] = '\0';
    if (gBootHomeApa) {
        if (gHDDPrefix == NULL || gHDDPrefix[0] == '\0')
            return 0;

        length = snprintf(path, pathSize, "%sTHM/bgm.ogg", gHDDPrefix);
    } else {
        size_t baseLength;

        if (gBootDir[0] == '\0')
            return 0;

        baseLength = strlen(gBootDir);
        if (gBootDir[baseLength - 1] == ':' || gBootDir[baseLength - 1] == '/' ||
            gBootDir[baseLength - 1] == '\\')
            length = snprintf(path, pathSize, "%sbgm.ogg", gBootDir);
        else
            length = snprintf(path, pathSize, "%s/bgm.ogg", gBootDir);
    }

    if (length < 0 || length >= pathSize) {
        path[0] = '\0';
        return 0;
    }

    return 1;
}

// When this function is called, the current device for loading/saving config is the memory card.
// "Custom Settings Path" bootstrap.
//
// THE CHICKEN-AND-EGG: the user's chosen path is stored in CONFIG_OPL_CUSTOM_SETTINGS_PATH, which
// lives INSIDE the config file that path points at. At boot we cannot read the setting without
// already knowing it. So the path is ALSO mirrored to a tiny plain-text "config.path" file in the
// CWD -- readable before any device driver is up, because it is where OPL was launched from. Boot
// reads the redirect; the setting inside the config is what the GUI edits and what keeps the two in
// sync on save.
//
// ⚠ THAT PREMISE HAS ONE EXCEPTION, and it is the whole reason for the mc mirror below: on a NETWORK
// boot the CWD *is* the share, and mounting the share needs the static IP (and for SMB the server,
// share, user and password) out of the very config this redirect exists to locate. So there the CWD
// copy is unreadable at exactly the moment it is needed, and a custom settings path set by a network
// booter was silently lost on the next boot.
static const char *configPathRedirectFile = "config.path";

// Prefer the concrete slot OPL actually booted from. With both cards inserted, a generic
// first-present probe must not redirect an mc1 boot's bootstrap/config traffic onto mc0. For
// non-MC boots retain the historical first-present fallback.
static int preferredMcSlot(void)
{
    if (!strncmp(gBootDir, "mc0:", 4))
        return 0;
    if (!strncmp(gBootDir, "mc1:", 4))
        return 1;
    return sysCheckMC();
}

// Absolute mc-side location for the bootstrap mirror. A concrete MC boot owns its own slot;
// network/bare boots use the existing first-present policy. Returns 0 when there is none.
static int mcConfigPathRedirect(char *out, int outLen)
{
    int slot = preferredMcSlot();

    if (slot < 0)
        return 0;

    snprintf(out, outLen, "mc%d:/OPL/%s", slot, configPathRedirectFile);
    return 1;
}

static int configPathRedirectLocation(char *out, int outLen)
{
    const char *home = gBootDir;

    // APA launch identity and config ownership are separate. A delayed HDD mount can leave
    // gBootDir as raw hddN:, but once the EXISTING persistent PFS home is mounted, config.path
    // belongs there. This is the only APA override; every other boot class keeps gBootDir.
    if (gBootHomeApa && gHDDPrefix != NULL && gHDDPrefix[0] != '\0')
        home = gHDDPrefix;

    // Never compose a writable bootstrap file in raw APA space. hddN: is a partition
    // namespace, not a PFS filesystem; any file-like O_CREAT there is unsafe by definition.
    if (!strncmp(home, "hdd", 3)) {
        out[0] = '\0';
        return 0;
    }

    if (home[0] != '\0') {
        size_t len = strlen(home);
        if (len > 0 && (home[len - 1] == ':' || home[len - 1] == '/' || home[len - 1] == '\\'))
            snprintf(out, outLen, "%s%s", home, configPathRedirectFile);
        else
            snprintf(out, outLen, "%s/%s", home, configPathRedirectFile);
    } else {
        // Read-only legacy discovery may still inspect a relative redirect. Writers reject this
        // case below because an unknown CWD could itself be raw hddN: space.
        snprintf(out, outLen, "%s", configPathRedirectFile);
    }

    return 1;
}

static int readConfigPathRedirect(char *outPath, int outPathLen)
{
    int fd;
    int len;
    char redirectFile[sizeof(gBootDir) + 32];

    // APA launchers can leave the process CWD in raw hdd0: space. Once the data partition
    // is mounted, anchor the bootstrap pointer to the resolved PFS config home instead of
    // sending an ordinary file open through the raw APA namespace.
    if (!configPathRedirectLocation(redirectFile, sizeof(redirectFile)))
        return 0;
    fd = open(redirectFile, O_RDONLY);
    if (fd < 0) {
        char mcPath[64];

        if (!gBootHomeDeferred || !mcConfigPathRedirect(mcPath, sizeof(mcPath)))
            return 0;

        fd = open(mcPath, O_RDONLY);
        if (fd < 0)
            return 0;
    }

    len = read(fd, outPath, outPathLen - 1);
    close(fd);
    if (len <= 0)
        return 0;

    while (len > 0 && (outPath[len - 1] == '\r' || outPath[len - 1] == '\n' || outPath[len - 1] == ' ' || outPath[len - 1] == '\t'))
        len--;
    outPath[len] = '\0';

    return len > 0;
}

static int writeConfigPathRedirect(const char *path)
{
    char redirectFile[sizeof(gBootDir) + 32];
    int fd;
    int primaryOk = 0;

    // No boot identity means no provably safe place for an O_CREAT bootstrap file:
    // the launcher may have left CWD in raw APA space. The config payload may still save via
    // normal legacy discovery, but we do not manufacture a redirect in an unknown namespace.
    if (gBootDir[0] == '\0') {
        LOG("CONFIG refusing relative config.path write with unknown boot CWD\n");
        return 0;
    }
    if (!configPathRedirectLocation(redirectFile, sizeof(redirectFile))) {
        LOG("CONFIG refusing config.path write in raw APA boot namespace %s\n", gBootDir);
        return 0;
    }
    fd = open(redirectFile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        int n1 = write(fd, path, strlen(path));
        int n2 = write(fd, "\n", 1);
        close(fd);
        primaryOk = (n1 == (int)strlen(path) && n2 == 1);
    }

    if (gBootHomeDeferred) {
        char mcPath[64];
        int mirrorOk = 0;

        if (mcConfigPathRedirect(mcPath, sizeof(mcPath))) {
            fd = open(mcPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd < 0) {
                char mcDir[64];
                snprintf(mcDir, sizeof(mcDir), "mc%d:/OPL", sysCheckMC());
                mkdir(mcDir, 0777);
                fd = open(mcPath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            }
            if (fd >= 0) {
                int n1 = write(fd, path, strlen(path));
                int n2 = write(fd, "\n", 1);
                close(fd);
                mirrorOk = (n1 == (int)strlen(path) && n2 == 1);
                if (mirrorOk)
                    LOG("CONFIG mirrored settings path to %s\n", mcPath);
            }
            if (!mirrorOk)
                LOG("CONFIG could not mirror settings path to %s\n", mcPath);
        }

        // The card mirror is the only bootstrap copy a deferred network boot can read.
        return mirrorOk;
    }

    return primaryOk;
}

// Convert an explicit APA settings target to the already-owned pfs0: mount. OPL's HDD
// support owns pfs0: for gOPLPart; Step-208 deliberately refuses to remount another APA
// partition (or borrow transient pfs1:) merely to store settings.
static int prepareCustomApaSettingsPath(char *path, int pathLen)
{
    char targetPart[64] = {0};
    char subfolder[64] = {0};
    char canonical[64] = {0};
    const char *p = path;
    const char *unit = "hdd0";
    int isPfsAlias = 0;
    size_t i = 0;
    DIR *dir;
    int n;

    if (path == NULL || path[0] == '\0') {
        gLastSaveErrno = ENODEV;
        return -1;
    }

    // Accept both pfs: and pfs0: as user-facing aliases for OPL's already-mounted persistent
    // data home. pfs1: remains reserved for transient HDD scanning and is never a settings home.
    if (!strncmp(path, "pfs", 3)) {
        if (path[3] == ':') {
            p = path + 4;
            isPfsAlias = 1;
        } else if (path[3] == '0' && path[4] == ':') {
            p = path + 5;
            isPfsAlias = 1;
        } else {
            gLastSaveErrno = ENODEV;
            return -1;
        }
    } else if (path[0] == '+') {
        p = path;
    } else {
        if (strncmp(path, "hdd", 3) != 0 || (path[3] != '0' && path[3] != '1') || path[4] != ':') {
            LOG("CONFIG malformed APA settings path %s rejected\n", path);
            gLastSaveErrno = ENODEV;
            return -1;
        }
        if (path[3] == '1')
            unit = "hdd1";
        p = path + 5;
        while (*p == '/' || *p == ':' || *p == '\\')
            p++;
    }

    if (!hddLoadModulesReady()) {
        gLastSaveErrno = ENODEV;
        return -1;
    }
    hddLoadSupportModules();
    if (gOPLPart[0] == '\0' || gHDDPrefix == NULL || gHDDPrefix[0] == '\0') {
        gLastSaveErrno = ENODEV;
        return -1;
    }

    const char *activeLabel = strchr(gOPLPart, ':');
    activeLabel = activeLabel ? activeLabel + 1 : gOPLPart;
    // Only the canonical __common fallback maps the OPL data home below a physical OPL/
    // subdirectory. Every valid conf_hdd.cfg-selected partition, including a non-+ label,
    // owns its PFS root directly.
    int commonStyleHome = !strcmp(gOPLPart, "hdd0:__common");

    if (isPfsAlias) {
        // Compatibility spellings seen on hardware include pfs0:, pfs:/__common/OPL and
        // pfs0:/OPL. They all mean the ACTIVE already-mounted data home; never reinterpret a
        // pfs alias as permission to mount another APA partition.
        while (*p == '/' || *p == ':' || *p == '\\')
            p++;

        size_t labelLen = strlen(activeLabel);
        if (labelLen > 0 && !strncmp(p, activeLabel, labelLen) &&
            (p[labelLen] == '\0' || p[labelLen] == '/' || p[labelLen] == ':' || p[labelLen] == '\\')) {
            p += labelLen;
            while (*p == '/' || *p == ':' || *p == '\\')
                p++;
        } else if ((*p == '+' || !strncmp(p, "__", 2)) && *p != '\0') {
            LOG("CONFIG PFS settings alias %s names a non-active APA partition\n", path);
            gLastSaveErrno = ENODEV;
            return -1;
        }
    } else {
        i = 0;
        while (*p != '\0' && *p != '/' && *p != ':' && *p != '\\' && i < sizeof(subfolder) - 1)
            subfolder[i++] = *p++;
        subfolder[i] = '\0';
        if (subfolder[0] == '\0') {
            gLastSaveErrno = ENODEV;
            return -1;
        }
        snprintf(targetPart, sizeof(targetPart), "%s:%s", unit, subfolder);
        if (strcmp(targetPart, gOPLPart) != 0) {
            LOG("CONFIG APA settings partition %s rejected; active OPL data partition is %s\n", targetPart, gOPLPart);
            gLastSaveErrno = ENODEV;
            return -1;
        }

        while (*p != '\0') {
            while (*p == ':' || *p == '/' || *p == '\\')
                p++;
            if (!strncmp(p, "pfs:", 4)) {
                p += 4;
                continue;
            }
            break;
        }
    }

    // __common's HDD-notation data home is already pfs0:OPL/. Consume one redundant OPL
    // component from hdd0:/__common/OPL. Direct pfs0: syntax remains direct PFS syntax.
    if (!isPfsAlias && commonStyleHome && !strncmp(p, "OPL", 3) &&
        (p[3] == '\0' || p[3] == '/' || p[3] == ':' || p[3] == '\\')) {
        p += 3;
        while (*p == ':' || *p == '/' || *p == '\\')
            p++;
    }

    i = 0;
    while (*p != '\0' && i < sizeof(subfolder) - 1) {
        char c = *p++;
        subfolder[i++] = (c == '\\') ? '/' : c;
    }
    subfolder[i] = '\0';
    while (i > 0 && subfolder[i - 1] == '/')
        subfolder[--i] = '\0';

    if (isPfsAlias) {
        // pfs:/pfs0: names the already-mounted filesystem ROOT. Preserve that syntax exactly:
        // pfs0:/CFG -> pfs0:CFG, not pfs0:OPL/CFG. A redundant physical partition label was
        // consumed above only when it matched gOPLPart.
        if (subfolder[0] != '\0')
            n = snprintf(canonical, sizeof(canonical), "pfs0:%s", subfolder);
        else
            n = snprintf(canonical, sizeof(canonical), "pfs0:");
    } else if (subfolder[0] != '\0') {
        size_t prefixLen = strlen(gHDDPrefix);
        if (prefixLen > 0 && (gHDDPrefix[prefixLen - 1] == ':' || gHDDPrefix[prefixLen - 1] == '/'))
            n = snprintf(canonical, sizeof(canonical), "%s%s", gHDDPrefix, subfolder);
        else
            n = snprintf(canonical, sizeof(canonical), "%s/%s", gHDDPrefix, subfolder);
    } else {
        n = snprintf(canonical, sizeof(canonical), "%s", gHDDPrefix);
    }

    if (n < 0 || n >= (int)sizeof(canonical) || n >= pathLen) {
        gLastSaveErrno = ENODEV;
        return -1;
    }

    dir = opendir(canonical);
    if (dir == NULL) {
        LOG("CONFIG APA settings directory %s does not exist\n", canonical);
        gLastSaveErrno = ENOENT;
        return -1;
    }
    closedir(dir);

    snprintf(path, pathLen, "%s", canonical);
    LOG("CONFIG APA settings path -> %s (OPL part %s)\n", path, gOPLPart);
    return 1;
}

static int isApaSettingsPath(const char *path)
{
    return path != NULL && (!strncmp(path, "hdd", 3) || !strncmp(path, "pfs", 3) || path[0] == '+');
}

// Resolve the normal safe HDD settings home after an explicit HDD path cannot be used.
// hddLoadSupportModules owns the policy: read the existing __common/OPL/conf_hdd.cfg redirect
// first, otherwise use __common/OPL/, or an existing +OPL when __common is unavailable. It never
// creates or formats an APA partition. This helper only accepts the resulting mounted PFS namespace.
static int prepareHddSettingsFallback(char *path, int pathLen)
{
    DIR *dir;
    int n;

    if (!hddLoadModulesReady())
        return -1;

    hddLoadSupportModules();
    if (gHDDPrefix == NULL || gHDDPrefix[0] == '\0')
        return -1;

    dir = opendir(gHDDPrefix);
    if (dir == NULL)
        return -1;
    closedir(dir);

    n = snprintf(path, pathLen, "%s", gHDDPrefix);
    return (n >= 0 && n < pathLen) ? 1 : -1;
}

// Make a typed settings path usable: the user may name a device that is not mounted yet. APA/PFS
// paths are handled first and rewritten to OPL's already-mounted pfs0: data partition; BDM paths
// keep the existing transport resolver. HDD targets have a deterministic safe fallback in
// _saveConfig(); non-HDD explicit targets still fail rather than scattering settings elsewhere.
static int prepareCustomSettingsPath(char *path, int pathLen)
{
    int bdmType = BDM_TYPE_UNKNOWN;

    if (path == NULL || path[0] == '\0')
        return 0;

    if (isApaSettingsPath(path))
        return prepareCustomApaSettingsPath(path, pathLen);

    if (strncmp(path, "mass", 4) && strncmp(path, "usb", 3) && strncmp(path, "ata", 3) &&
        strncmp(path, "mx4sio", 6) && strncmp(path, "sd", 2) && strncmp(path, "ilink", 5))
        return 0;

    return bdmResolveBootDir(path, pathLen, "", &bdmType);
}


// Last-resort READ-ONLY discovery for a missing/stale config.path. This exists to recover the
// chicken-and-egg custom-settings value from an already-existing config; it never creates a config,
// never changes APA metadata, and never makes an arbitrary discovered device the permanent save home.
// A known non-MC local boot self-migrates the loaded in-memory sets back to its normal boot home. A
// concrete MC boot preserves the same-card directory it recovered, so its first save cannot relocate
// an already-working MC1 configuration to the card root or the other slot. If the recovered config
// contains Custom Settings Path, _saveConfig will honor it and regenerate config.path on the user's
// next explicit Save Changes.
static int tryReadRecoveryConfigHome(int types, const char *home)
{
    DIR *dir = opendir(home);
    if (dir == NULL)
        return 0;
    closedir(dir);

    configEnd();
    configInit((char *)home);
    int value = configReadMulti(types);
    if (value & CONFIG_OPL)
        LOG("CONFIG recovery found existing settings at %s\n", home);
    return value;
}

static int sameConcreteMcSlot(const char *first, const char *second)
{
    return first != NULL && second != NULL &&
           !strncmp(first, "mc", 2) && !strncmp(second, "mc", 2) &&
           (first[2] == '0' || first[2] == '1') && first[3] == ':' &&
           first[2] == second[2] && second[3] == ':';
}

static void restoreRecoverySaveHome(const char *recoveredHome);

static int bootHomeIsConcreteMc(void)
{
    return !strncmp(gBootDir, "mc", 2) &&
           (gBootDir[2] == '0' || gBootDir[2] == '1') && gBootDir[3] == ':';
}

static int bootHomeIsKnownMmce(void)
{
    return !strncmp(gBootDir, "mmce", 4) &&
           (gBootDir[4] == '0' || gBootDir[4] == '1') && gBootDir[5] == ':';
}

static int bootPathIsConcreteMass(const char *path)
{
    const char *p;

    if (path == NULL || strncmp(path, "mass", 4) != 0)
        return 0;
    p = path + 4;
    if (*p < '0' || *p > '9')
        return 0;
    while (*p >= '0' && *p <= '9')
        p++;
    return *p == ':';
}

static int bootHomeIsKnownBdm(void)
{
    // A resolved BDM driver or a literal massN: token is a concrete local owner. The latter can
    // remain unclassified while its transport starts, but must not be turned into a broad search.
    return !gBootHomeDeferred && gBootHomeBdm;
}

// A known local boot gets at most its own conventional legacy locations. The normal CWD was read
// before this function, so these probes are migration reads only: <device>:/OPL then the device
// root. They cannot wake USB/ATA/MMCE/other MC slots or turn an absent first-run config into a
// whole-machine discovery pass.
static int tryKnownBootConfigRecovery(int types)
{
    const char *colon = strchr(gBootDir, ':');
    char root[16];
    char home[32];
    int value;
    int n;

    if (colon == NULL)
        return 0;

    n = (int)(colon - gBootDir) + 1;
    if (n <= 0 || n >= (int)sizeof(root))
        return 0;
    memcpy(root, gBootDir, n);
    root[n] = '\0';

    snprintf(home, sizeof(home), "%s/OPL", root);
    value = tryReadRecoveryConfigHome(types, home);
    if (value & CONFIG_OPL) {
        restoreRecoverySaveHome(home);
        return value;
    }

    snprintf(home, sizeof(home), "%s/", root);
    value = tryReadRecoveryConfigHome(types, home);
    if (value & CONFIG_OPL) {
        restoreRecoverySaveHome(home);
        return value;
    }

    // Failed recovery probes re-home every config set. Restore the known source before returning
    // defaults so the first explicit save stays on that same device.
    configEnd();
    configInit(gBootDir);
    return 0;
}

static void restoreRecoverySaveHome(const char *recoveredHome)
{
    // Recovery is discovery only and never writes by itself. Known local boots normally return to
    // their real boot home, but a concrete MC boot keeps the same-card directory that supplied it.
    // A bare launch has no independent owner, so the existing config home that was
    // actually recovered becomes the next explicit-save home. A deferred network boot cannot save
    // back to its unreadable share during bootstrap, so select a concrete reachable MC home when possible.
    if (gBootHomeDeferred) {
        const char *mcHome = NULL;
        DIR *dir;

        if (!strncmp(recoveredHome, "mc0:", 4))
            mcHome = "mc0:/OPL";
        else if (!strncmp(recoveredHome, "mc1:", 4))
            mcHome = "mc1:/OPL";
        else {
            dir = opendir("mc0:/");
            if (dir != NULL) {
                closedir(dir);
                mcHome = "mc0:/OPL";
            } else {
                dir = opendir("mc1:/");
                if (dir != NULL) {
                    closedir(dir);
                    mcHome = "mc1:/OPL";
                }
            }
        }

        if (mcHome != NULL)
            configSetMove((char *)mcHome);
        else
            configSetMove(NULL); // no concrete MC is reachable: keep the normal fail-visible wildcard home
    } else if (sameConcreteMcSlot(gBootDir, recoveredHome)) {
        // A concrete MC boot that recovered its existing settings from the same card keeps that
        // exact directory as its save owner. In particular, a launcher that supplies only "mc1:"
        // as the boot CWD must not move a successfully read mc1:/OPL configuration to the card
        // root (or to mc0 when both cards are inserted).
        configSetMove((char *)recoveredHome);
    } else if (gBootDir[0] != '\0') {
        configSetMove(gBootDir);
    } else {
        // A bare launch has no independent boot owner to restore. The existing config home that
        // recovery actually found is therefore the strongest safe ownership signal; keep that
        // concrete MC/USB home so the next explicit save can persist instead of falling into an
        // unreachable mc?:OPL wildcard. Recovery still performs no write by itself.
        configSetMove((char *)recoveredHome);
    }
}

static int tryMissingConfigPathRecovery(int types)
{
    int value;
    int slots[2] = {0, 1};
    int preferred = preferredMcSlot();

    // A concrete mc1 boot searches mc1 first even when mc0 is also inserted. Otherwise retain the
    // historical mc0->mc1 recovery order. Both slots are still probed directly; sysCheckMC() is
    // deliberately not used as a substitute for trying the second card.
    if (preferred == 1) {
        slots[0] = 1;
        slots[1] = 0;
    }
    for (int i = 0; i < 2; i++) {
        char home[16];

        snprintf(home, sizeof(home), "mc%d:/OPL", slots[i]);
        value = tryReadRecoveryConfigHome(types, home);
        if (value & CONFIG_OPL) {
            restoreRecoverySaveHome(home);
            return value;
        }

        snprintf(home, sizeof(home), "mc%d:/", slots[i]);
        value = tryReadRecoveryConfigHome(types, home);
        if (value & CONFIG_OPL) {
            restoreRecoverySaveHome(home);
            return value;
        }
    }

    // USB is third choice. Force only the USB transport, then inspect only slots whose live driver
    // identity is USB; ATA/MX4SIO/iLink are not pulled into this recovery scan. Check both the device
    // root and the conventional /OPL directory. configRead() itself handles the legacy filename.
    if (bdmEnsureSourceModules(BDM_TYPE_USB, 1500)) {
        int slots[MAX_BDM_DEVICES];
        int count = bdmGetDeviceSlotsByType(BDM_TYPE_USB, slots, MAX_BDM_DEVICES);
        for (int i = 0; i < count; i++) {
            char home[32];

            snprintf(home, sizeof(home), "mass%d:/", slots[i]);
            value = tryReadRecoveryConfigHome(types, home);
            if (value & CONFIG_OPL) {
                restoreRecoverySaveHome(home);
                return value;
            }

            snprintf(home, sizeof(home), "mass%d:/OPL", slots[i]);
            value = tryReadRecoveryConfigHome(types, home);
            if (value & CONFIG_OPL) {
                restoreRecoverySaveHome(home);
                return value;
            }
        }
    }

    // MMCE is fourth choice (after MC and USB).
    value = checkLoadConfigMMCE(types);
    if (value & CONFIG_OPL)
        return value;

    // Every failed probe re-homed the config_set filenames. For APA, preserve an already-mounted
    // safe PFS home instead of falling back to the raw hddN: launch namespace. Raw APA remains only
    // a fail-closed launch identity when no writable PFS owner exists at all.
    configEnd();
    if (gBootHomeApa && gHDDPrefix != NULL && gHDDPrefix[0] != '\0')
        configInit(gHDDPrefix);
    else
        configInit(gBootDir[0] != '\0' ? gBootDir : NULL);
    return 0;
}


static int tryAlternateDevice(int types)
{
    char redirectPath[64];
    int value;
    DIR *dir;

    // APA has one deterministic ownership chain. If the first boot-time mount missed while the
    // drive was settling, retry the EXISTING-PFS resolver before reading config.path. This adds no
    // new device class or fallback; checkLoadConfigHDD already performed the same synchronous HDD
    // work below, only too late to expose a PFS-hosted redirect.
    if (gBootHomeApa && (gHDDPrefix == NULL || gHDDPrefix[0] == '\0')) {
        if (hddLoadModulesReady())
            hddLoadSupportModules();
    }

    // The user's Custom Settings Path, if one was set, takes precedence over every discovery probe
    // below -- it is an explicit instruction, not a guess.
    if (readConfigPathRedirect(redirectPath, sizeof(redirectPath))) {
        if (prepareCustomSettingsPath(redirectPath, sizeof(redirectPath)) >= 0) {
            LOG("CONFIG custom settings path -> %s\n", redirectPath);
            configEnd();
            configInit(redirectPath);
            value = configReadMulti(types);
            if (value & CONFIG_OPL)
                return value;

            // A stale pointer must not leave local config_set filenames homed on the missing target.
            // Network boot keeps its existing deferred-home handling below.
            if (!gBootHomeDeferred) {
                configEnd();
                configInit(gBootDir[0] != '\0' ? gBootDir : NULL);
                LOG("CONFIG stale settings redirect failed; restored normal home %s\n", gBootDir);
            }
        } else {
            // Named a BDM device that never mounted.
            LOG("CONFIG custom settings path %s did not mount\n", redirectPath);
        }
    }

    // APA/PFS boot identity is authoritative. After an explicit redirect misses, ONLY the
    // deterministic existing-PFS ownership chain is eligible: __common/OPL/conf_hdd.cfg's valid
    // existing target, otherwise __common/OPL. Never import an unrelated MC/USB master config into
    // an FHDB/APA session; that can resurrect stale Custom Settings Path state and makes the next
    // save destination depend on whichever removable device happened to be inserted.
    if (gBootHomeApa) {
        value = checkLoadConfigHDD(types);
        if (value & CONFIG_OPL)
            return value;

        // No master config yet is a valid first-run state. Keep defaults homed to the mounted safe
        // PFS target so the first explicit Save materializes them there. If PFS is still unavailable,
        // leave the raw APA launch identity only as a fail-closed marker; _saveConfig retries this
        // same safe chain and config.c blocks every raw hddN: config write as defense in depth.
        if (gHDDPrefix != NULL && gHDDPrefix[0] != '\0')
            configSetMove(gHDDPrefix);
        showCfgPopup = 0;
        return 0;
    }

    // A missing file on a known local boot is an ordinary first-run state, not a signal to scan all
    // supported storage. Limit migration reads to the boot device's own conventional legacy homes:
    // same MC slot, the resolved BDM device, or the corresponding MMCE card. This is deliberately
    // before the broad legacy hunt below, whose USB module wait and MMCE probing are unacceptable
    // on a known device that simply has no config yet.
    if (bootHomeIsConcreteMc() || bootHomeIsKnownBdm() || bootHomeIsKnownMmce()) {
        value = tryKnownBootConfigRecovery(types);
        if (value & CONFIG_OPL)
            return value;

        showCfgPopup = 0;
        return 0;
    }

    // Bare, deferred-network, and otherwise unclassified boots retain the bounded read-only
    // recovery chain. These have no verified local settings owner to prefer.
    value = tryMissingConfigPathRecovery(types);
    if (value & CONFIG_OPL)
        return value;

    // If OPL was booted from a valid CWD/boot directory (gBootDir is set, e.g. "mc0:/OPL" or "mc0:"),
    // settings are strictly homed to gBootDir. Never probe alternate devices (mass0:/hdd0:) or hijack
    // the save location away from CWD/Memory Card.
    if (gBootDir[0] != '\0') {
        // LEGACY READ-ONLY FALLBACK. Every build before the CWD doctrine -- ours and official alike
        // -- homed settings at mc?:OPL, so every existing install's config lives THERE, not beside
        // the ELF. Without this, upgrading silently reset every user to defaults (reported from
        // hardware as "it didn't read my settings" and "everything is slow like it used to be" --
        // stock art delay and scroll speed are literally the old feel). The user's setup did not
        // change; we moved where we look, so we keep looking in the old place too.
        //
        // READ ONLY, and only the mc?:OPL home -- never other devices (that hijack is what the
        // strict homing exists to prevent). The home is moved straight back to the boot dir, so
        // the very first save writes the config BESIDE THE ELF and this fallback goes quiet for
        // good: read-old, write-new, self-migrating. The load toast names the boot device rather
        // than the card on the one boot that reads legacy -- the save-location toast telling the
        // truth matters more, and after the first save the two agree anyway.
        int homeLeftOnCard = 0;
        if (sysCheckMC() >= 0) {
            char concreteMcHome[16];
            if (!strncmp(gBootDir, "mc0:", 4) || !strncmp(gBootDir, "mc1:", 4)) {
                snprintf(concreteMcHome, sizeof(concreteMcHome), "mc%c:/OPL", gBootDir[2]);
                configSetMove(concreteMcHome); // MC boot: legacy fallback stays on the booted slot
            } else {
                configSetMove(NULL); // non-MC boot: retain the historical mc?:OPL selection
            }
            value = configReadMulti(types);

            // SELF-MIGRATION IS WRONG WHEN THE BOOT DEVICE CANNOT BE READ AT BOOT. The move-back
            // below is what makes this fallback self-retiring on a BDM boot: read the old card copy
            // once, write the next save beside the ELF, never look at the card again. On a network
            // boot it does the opposite of what it promises -- the read above SUCCEEDS (the card is
            // readable with no network at all), and then the home is moved onto a share that cannot
            // be mounted until the config has already been read. The next save goes there, the next
            // boot cannot see it, and the card copy it falls back to is now stale. That is precisely
            // the "settings save but never load back" report from network booters.
            //
            // So for a deferred-home boot the card KEEPS the home: it is the only place that is both
            // writable and readable at boot time, and configSetMove(NULL) has already pointed the
            // notifications there too. Nothing new is written and no card is touched that was not
            // already serving this config -- sysCheckMC() gated the whole branch.
            if (gBootHomeDeferred)
                homeLeftOnCard = 1;
            else if (sameConcreteMcSlot(gBootDir, concreteMcHome)) {
                configSetMove(concreteMcHome); // MC legacy config remains at the exact discovered home
                // configSetMove already made the exact MC1/MC0 home the notification/save owner.
                // Do not overwrite that with a compact/root boot CWD below: failure reporting must
                // name the same directory that configWriteMulti will actually use.
                homeLeftOnCard = 1;
            } else
                configSetMove(gBootDir); // home (and notifications) back on the boot dir: saves self-migrate

            if (value & CONFIG_OPL)
                return value;
        }
        // Only re-point at the boot dir if the home is actually still there. When the card kept it
        // above, saying "boot dir" would make the save-location toast name a device that is not the
        // save location -- and a truthful toast is the entire point of this call.
        if (!homeLeftOnCard)
            configPrepareNotifications(gBootDir);
        showCfgPopup = 0;
        return 0;
    }

    // Bare ELF launch without boot directory context: try Memory Card first.
    if (sysCheckMC() >= 0) {
        configPrepareNotifications(gBaseMCDir);
        showCfgPopup = 0;
        return 0;
    }

    // No memory card? Check mass0: or HDD as last resort for bare launches...
    dir = opendir("mass0:");
    if (dir != NULL) {
        closedir(dir);
        configEnd();
        configInit("mass0:");
    } else if (hddLoadModulesReady()) {
        hddLoadSupportModules();
        if (gHDDPrefix != NULL) {
            dir = opendir(gHDDPrefix);
            if (dir != NULL) {
                closedir(dir);
                configEnd();
                configInit(gHDDPrefix);
            }
        }
    }
    showCfgPopup = 0;

    return 0;
}

// Shared reader for the Neutrino-launch globals (args / custom path / -elf switch / global default
// core / device TYPE incl. the legacy device-INDEX migration). Factored out so the interactive
// _loadConfig and the autolaunch miniInit can never drift: the argv/autolaunch path previously read
// NONE of these, so a keyless "Default" game autolaunched with a stale AUTO device and empty global
// args, silently diverging from an interactive launch of the same game.
static void configReadNeutrinoGlobals(config_set_t *configOPL)
{
    configGetStrCopy(configOPL, CONFIG_OPL_NEUTRINO_ARGS, gNeutrinoArgs, sizeof(gNeutrinoArgs));
    configGetStrCopy(configOPL, CONFIG_OPL_NEUTRINO_PATH, gNeutrinoPath, sizeof(gNeutrinoPath));
    configGetInt(configOPL, CONFIG_OPL_NEUTRINO_ELF_ARG, &gNeutrinoElfArg);
    // Global default Loader Core (0=<OPL>, 1=Neutrino). Absent in legacy configs -> keep the reset
    // default (0/<OPL>), so existing installs behave exactly as before this key existed.
    configGetInt(configOPL, CONFIG_OPL_DEFAULT_CORE, &gDefaultCoreLoader);
    configGetInt(configOPL, CONFIG_OPL_NEUTRINO_VIDEO, &gNeutrinoVideoDefault);
    if (gNeutrinoVideoDefault < 0 || gNeutrinoVideoDefault > 5)
        gNeutrinoVideoDefault = 0; // sanitize (indexes system.c gsmVideoTokens at the launch legs)
    configGetInt(configOPL, CONFIG_OPL_NEUTRINO_GSMCOMP, &gNeutrinoGsmCompDefault);
    if (gNeutrinoGsmCompDefault < 0 || gNeutrinoGsmCompDefault > 3)
        gNeutrinoGsmCompDefault = 0;
    // Neutrino Device: prefer the new device-TYPE key; if absent (config predates the picker
    // change), migrate the legacy device-INDEX value.
    if (!configGetInt(configOPL, CONFIG_OPL_NEUTRINO_DEVTYPE, &gNeutrinoDevice)) {
        int legacyDev = 0;
        if (configGetInt(configOPL, CONFIG_OPL_NEUTRINO_DEVICE, &legacyDev)) {
            if (legacyDev == 1 || legacyDev == 2)
                gNeutrinoDevice = NEUTRINO_DEV_MC;
            else
                gNeutrinoDevice = NEUTRINO_DEV_AUTO;
        }
    }
}

static void resolveBootDirToMass(void)
{
    gBootHomeApa = 0;
    gBootHddCommonFallback = 0;
    gBootHomeBdm = 0;
    gBootDirBdmType = BDM_TYPE_UNKNOWN;
    gBootHomeDeferred = 0;
    if (gBootDir[0] == '\0')
        return;

    // APA-HDD boot: the ELF launch partition and OPL's persistent data partition are separate
    // concepts. FHDB/uLE/HDD-OSD may launch from __sysconf/__common/__contents, but hddsupport
    // alone owns gOPLPart and mounts that data partition on pfs0:. Config follows gHDDPrefix.
    if (!strncmp(gBootDir, "hdd", 3) || !strncmp(gBootDir, "pfs", 3)) {
        if (hddLoadModulesReady()) {
            char before[sizeof(gBootDir)];
            snprintf(before, sizeof(before), "%s", gBootDir);

            hddLoadSupportModules();
            if (gHDDPrefix != NULL && gHDDPrefix[0] != '\0') {
                DIR *dir = opendir(gHDDPrefix);
                if (dir != NULL) {
                    closedir(dir);
                    snprintf(gBootDir, sizeof(gBootDir), "%s", gHDDPrefix);
                    size_t rlen = strlen(gBootDir);
                    while (rlen > 0 && gBootDir[rlen - 1] == '/')
                        gBootDir[--rlen] = '\0';

                    gBootHomeApa = 1;
                    if (!strcmp(gOPLPart, "hdd0:__common"))
                        gBootHddCommonFallback = 1;
                    if (gHDDStartMode == START_MODE_DISABLED)
                        gHDDStartMode = START_MODE_AUTO;

                    // Preserve rebuild-206's recursion guard: re-home only when resolution changed it.
                    if (strcmp(before, gBootDir) != 0) {
                        LOG("BOOT resolved APA launch %s -> data home %s (OPL part %s)\n", before, gBootDir, gOPLPart);
                        configEnd();
                        configInit(gBootDir);
                    }
                    return;
                }
            }
        }
        LOG("BOOT APA boot dir %s could not resolve an HDD data home; keeping launch identity for safe retry\n", gBootDir);
        // The launch transport is still APA even though the persistent data home did not mount.
        // Keep the real launch identity rather than inventing an unmounted pfs0:OPL home. The raw
        // hddN: config firewall makes the first read fail closed, and tryAlternateDevice retries
        // only the existing-partition HDD ownership chain.
        gBootHomeApa = 1;
        gHDDStartMode = START_MODE_AUTO;
        gBootHddCommonFallback = 0;
        return;
    }

    // CD-ROM / ISO boot (PS3 PS2-Classic ISO or PS2 disc boot): cdrom0: is read-only hardware.
    // Re-home the config to the Memory Card (mc?:OPL) so saves land on Memory Card Slot 1/2 (#353).
    if (!strncmp(gBootDir, "cdrom", 5)) {
        LOG("BOOT read-only cdrom boot dir %s -> redirecting config home to MC\n", gBootDir);
        gBootDir[0] = '\0';
        configEnd();
        configInit(NULL);
        return;
    }

    // MMCE boot: the mmceman driver is likewise not loaded at boot time (sysReset loads none of the
    // device stacks), so mmceN: is unreadable exactly when settings must load. Load it (idempotent)
    // and give the card a moment to register its filesystem. mmceN: IS the readable namespace, so no
    // prefix rewrite is needed.
    // NETWORK BOOT. smb0: (ethsupport's ethBase) and udpfs: (udpfssupport's udpfsBase) are the two
    // network filesystems OPL can be launched from, and neither can be mounted before the config is
    // read: the transport needs the static IP -- and SMB the server, share, user and password --
    // which is exactly what we are trying to read. Nothing to resolve, so leave the boot identity
    // completely alone and just record WHY it is unreadable.
    //
    // Shaped like the mmce branch below (plain return, identity preserved), NOT like the hdd/cdrom
    // branches above that blank gBootDir and re-home to configInit(NULL): blanking here would drop
    // this boot into legacy discovery and hand the config home to a plain memory card the user never
    // named, which is the FifthFox hazard those branches exist to avoid everywhere else.
    //
    // The flag's whole job is to let tryAlternateDevice tell "the device is down" apart from "there
    // is no config", so it can stop self-migrating the home onto a device that can never be read at
    // boot. Without it, that fallback reads the user's settings off the memory card correctly and
    // then moves the home back onto the share -- so the next save lands somewhere the next boot
    // cannot see, which is exactly the "my settings save but never load" report.
    //
    // NOT covered, and it cannot be: a UDPBD / UDPFS-BD boot arrives as "massN:/OPL", byte-identical
    // to a USB one. Its only distinguishing evidence is the BDM driver token "udp", which needs the
    // udpbd stack, which needs the static IP from the unreadable config. Those boots fall through to
    // the ordinary massN: resolve, fail to mount, and keep gBootDir -- today's behaviour, unchanged.
    if (!strncmp(gBootDir, "smb", 3) || !strncmp(gBootDir, "udpfs", 5)) {
        LOG("BOOT network boot dir %s -> config home deferred (mc untouched)\n", gBootDir);
        gBootHomeDeferred = 1;
        return;
    }

    if (!strncmp(gBootDir, "mmce", 4)) {
        // NOTE(rebuild): mmceLoadModules() is still the no-op stub here (MMCE is checklist items 1-3,
        // on WAIT), so the driver never loads and the opendir poll below always times out. That lands
        // on the "keep as config home" fallback -- which is the CORRECT feature-off behaviour, and the
        // one that matters: it is precisely the branch that keeps mc untouched. An MMCE boot therefore
        // reads defaults for now and its first successful save lands on mmce once item 1 makes the
        // driver real. Cost is the ~10 x delay(1) poll on an MMCE boot only.
        mmceLoadModules();
        char devRoot[8];
        const char *colon = strchr(gBootDir, ':');
        size_t rootLen = colon ? (size_t)(colon - gBootDir) + 1 : 0;
        if (rootLen > 0 && rootLen < sizeof(devRoot)) {
            memcpy(devRoot, gBootDir, rootLen);
            devRoot[rootLen] = '\0';
            for (int tries = 0; tries < 10; tries++) { // ~2 s total: the driver detects a present card in ms
                DIR *dir = opendir(devRoot);
                if (dir != NULL) {
                    closedir(dir);
                    return; // mounted -- the boot dir is readable as-is
                }
                delay(1);
            }
        }
        // Card still settling after the wait. It served this very ELF milliseconds ago, so it IS present
        // -- it just has not re-registered its mmceman filesystem yet. Do NOT blank gBootDir and re-home
        // the config to a plain memory card here (the old configInit(NULL) fallback): with an empty boot
        // dir, configGetDir() returns the legacy mc?: default, so _saveConfig's checkMCFolder() + the
        // per-file O_CREAT stamped an unwanted mc?:OPL folder and settings onto a SEPARATE plain mc card
        // (FifthFox, HW 2026-07-16). mc is never an MMCE user's config home. Keep mmce as the home: this
        // boot falls back to defaults if the card is still settling, and the first save lands on mmce
        // once it has mounted (a truly dead card fails the save visibly -- the same contract the BDM boot
        // device honours below). mc stays untouched.
        LOG("BOOT MMCE boot device %s not mounted after wait -> keep as config home (mc untouched)\n", gBootDir);
        return;
    }

    char before[sizeof(gBootDir)];
    snprintf(before, sizeof(before), "%s", gBootDir);
    int ret = bdmResolveBootDirBootstrap(gBootDir, sizeof(gBootDir), gBootElfName, &gBootDirBdmType);
    gBootHomeBdm = ret != 0 && (gBootDirBdmType != BDM_TYPE_UNKNOWN || bootPathIsConcreteMass(before));
    if (ret < 0) {
        // The boot device's BDM stack did not mount within the resolve budget. It served this ELF, so it
        // IS present -- do NOT blank gBootDir and re-home the config to a plain mc here (the old
        // configInit(NULL)). An empty boot dir makes configGetDir() fall to the legacy mc?: default, and
        // _saveConfig's checkMCFolder() + the per-file O_CREAT then stamp an mc?:OPL folder + settings
        // onto a plain memory card (FifthFox, HW 2026-07-16 -- extended from the MMCE case above at
        // NathanNeurotic's request). mc is never the boot device's config home. Keep the boot identity as
        // the home (the config sets were already homed there by init()'s configInit): this boot reads
        // defaults if the stack is still coming up, and a save targets the boot device -- failing visibly
        // if it is genuinely gone -- never a plain mc. bdmResolveBootDir leaves gBootDir UNCHANGED on a
        // failed resolve (it only rewrites on success), so the identity here is intact.
        LOG("BOOT boot device %s not mounted during bounded bootstrap resolve -> keep as config home (mc untouched)\n", before);
        return;
    }
    if (ret > 0 && strcmp(before, gBootDir) != 0) {
        LOG("BOOT resolved boot dir %s -> %s\n", before, gBootDir);
        configEnd();
        configInit(gBootDir); // re-point the config sets from the unreadable prefix to the massN: path
    }
}

static void _loadConfig()
{
    int value, themeID = -1, langID = -1;
    const char *temp;
    resolveBootDirToMass(); // usb0:/ata0:/mx4sio0:/APPS -> mass0:/APPS (boot-device massN:) before the first read
    int result = configReadMulti(lscstatus);

    if (lscstatus & CONFIG_OPL) {
        if (!(result & CONFIG_OPL)) {
            result = tryAlternateDevice(lscstatus);
        }

        if (result & CONFIG_OPL) {
            config_set_t *configOPL = configGetByType(CONFIG_OPL);

            configGetInt(configOPL, CONFIG_OPL_SCROLLING, &gScrollSpeed);
            configGetColor(configOPL, CONFIG_OPL_BGCOLOR, gDefaultBgColor);
            configGetColor(configOPL, CONFIG_OPL_TEXTCOLOR, gDefaultTextColor);
            configGetColor(configOPL, CONFIG_OPL_UI_TEXTCOLOR, gDefaultUITextColor);
            configGetColor(configOPL, CONFIG_OPL_SEL_TEXTCOLOR, gDefaultSelTextColor);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_NOTIFICATIONS, &gEnableNotifications);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_COVERART, &gEnableArt);
            configGetInt(configOPL, CONFIG_OPL_WIDESCREEN, &gWideScreen);

            if (!(getKeyPressed(KEY_TRIANGLE) && getKeyPressed(KEY_CROSS))) {
                configGetInt(configOPL, CONFIG_OPL_VMODE, &gVMode);
            } else {
                // Recovery combo: force 480p PROGRESSIVE (EDTV 640x448p@60, vmode index 3), not
                // Auto -- Auto resolves to region-default interlaced 480i/576i, which is exactly
                // what some modern displays/upscalers fail to sync, leaving the user still blind.
                LOG("--- Triangle + Cross held at boot - forcing Video Mode to 480p (recovery) ---\n");
                gVMode = 3;
                configSetInt(configOPL, CONFIG_OPL_VMODE, gVMode);
            }

            configGetInt(configOPL, CONFIG_OPL_XOFF, &gXOff);
            configGetInt(configOPL, CONFIG_OPL_YOFF, &gYOff);
            configGetInt(configOPL, CONFIG_OPL_OVERSCAN, &gOverscan);

            configGetInt(configOPL, CONFIG_OPL_BDM_CACHE, &bdmCacheSize);
            configGetInt(configOPL, CONFIG_OPL_HDD_CACHE, &hddCacheSize);
            configGetInt(configOPL, CONFIG_OPL_SMB_CACHE, &smbCacheSize);

            if (configGetStr(configOPL, CONFIG_OPL_THEME, &temp))
                themeID = thmFindGuiID(temp);

            if (configGetStr(configOPL, CONFIG_OPL_LANGUAGE, &temp))
                langID = lngFindGuiID(temp);

            if (configGetInt(configOPL, CONFIG_OPL_SWAP_SEL_BUTTON, &value))
                gSelectButton = value == 0 ? KEY_CIRCLE : KEY_CROSS;

            configGetInt(configOPL, CONFIG_OPL_XSENSITIVITY, &gXSensitivity);
            configGetInt(configOPL, CONFIG_OPL_YSENSITIVITY, &gYSensitivity);
            configGetInt(configOPL, CONFIG_OPL_DISABLE_DEBUG, &gEnableDebug);
            configGetInt(configOPL, CONFIG_OPL_PS2LOGO, &gPS2Logo);
            configGetInt(configOPL, CONFIG_OPL_HDD_GAME_LIST_CACHE, &gHDDGameListCache);
            configGetStrCopy(configOPL, CONFIG_OPL_EXIT_PATH, gExitPath, sizeof(gExitPath));
            configGetStrCopy(configOPL, CONFIG_OPL_CUSTOM_SETTINGS_PATH, gCustomSettingsPath, sizeof(gCustomSettingsPath));
            configGetInt(configOPL, CONFIG_OPL_AUTO_SORT, &gAutosort);
            configGetInt(configOPL, CONFIG_OPL_AUTO_REFRESH, &gAutoRefresh);
            configGetInt(configOPL, CONFIG_OPL_DEFAULT_DEVICE, &gDefaultDevice);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_WRITE, &gEnableWrite);
            configGetInt(configOPL, CONFIG_OPL_HDD_SPINDOWN, &gHDDSpindown);
            configGetStrCopy(configOPL, CONFIG_OPL_BDM_PREFIX, gBDMPrefix, sizeof(gBDMPrefix));
            configGetStrCopy(configOPL, CONFIG_OPL_ETH_PREFIX, gETHPrefix, sizeof(gETHPrefix));
            configGetInt(configOPL, CONFIG_OPL_REMEMBER_LAST, &gRememberLastPlayed);
            configGetInt(configOPL, CONFIG_OPL_AUTOSTART_LAST, &gAutoStartLastPlayed);
            configGetInt(configOPL, CONFIG_OPL_BDM_MODE, &gBDMStartMode);
            configGetInt(configOPL, CONFIG_OPL_HDD_MODE, &gHDDStartMode);
            // resolveBootDirToMass runs before this read. A stored Disabled value must not undo
            // the AUTO fallback for an APA/PFS boot that already mounted and served OPL itself.
            if (gBootHomeApa && gHDDStartMode == START_MODE_DISABLED)
                gHDDStartMode = START_MODE_AUTO;
            configGetInt(configOPL, CONFIG_OPL_ETH_MODE, &gETHStartMode);
            configGetInt(configOPL, CONFIG_OPL_APP_MODE, &gAPPStartMode);
            configGetStrCopy(configOPL, CONFIG_OPL_MMCE_PREFIX, gMMCEPrefix, sizeof(gMMCEPrefix));
            configGetInt(configOPL, CONFIG_OPL_MMCE_MODE, &gMMCEStartMode);
            configGetInt(configOPL, CONFIG_OPL_MMCE_SLOT, &gMMCESlot);
            configGetInt(configOPL, CONFIG_OPL_MMCEIGR_SLOT, &gMMCEIGRSlot);
            configGetInt(configOPL, CONFIG_OPL_MMCE_GAMEID, &gMMCEEnableGameID);
            configGetInt(configOPL, CONFIG_OPL_MMCE_WAIT_CYCLES, &gMMCEAckWaitCycles);
            configGetInt(configOPL, CONFIG_OPL_MMCE_USE_ALARMS, &gMMCEUseAlarms);
            // SIO2 pacing migration: silently upgrade the 0/0 defaults to 5/1
            {
                int pacingMigrated = 0;
                configGetInt(configOPL, CONFIG_OPL_MMCE_PACING_MIGR, &pacingMigrated);
                if (!pacingMigrated) {
                    if (gMMCEAckWaitCycles == 0 && gMMCEUseAlarms == 0) {
                        gMMCEAckWaitCycles = 5;
                        gMMCEUseAlarms = 1;
                    }
                    configSetInt(configOPL, CONFIG_OPL_MMCE_PACING_MIGR, 1);
                }
            }
            configGetInt(configOPL, CONFIG_OPL_ENABLE_USB, &gEnableUSB);
            configGetInt(configOPL, CONFIG_OPL_FAV_MODE, &gFAVStartMode);
            configGetInt(configOPL, CONFIG_OPL_APPLY_GAMEID, &gApplyGameID);
            configGetInt(configOPL, CONFIG_OPL_DEFAULT_GAME_VIEW, &gDefaultGameView);
            if (gDefaultGameView < GAME_VIEW_BOTH || gDefaultGameView > GAME_VIEW_VCD)
                gDefaultGameView = GAME_VIEW_BOTH;
            // A boot default-view locked to one type (VCD or ISO) must force the same one-shot
            // rescan the settings dialog does on a view change (gui.c). Without it, vcdViewActive()
            // short-circuits bdm/hdd/eth NeedsUpdate before the initial-scan trigger and the
            // list stays blank on boot -- a manual SELECT does not recover it (NeedsUpdate still
            // returns 0), only re-toggling the view does. This runs before applyConfig()'s first
            // support scans, so each VCD-capable page consumes the dirty flag on its first refresh.
            if (gDefaultGameView != GAME_VIEW_BOTH)
                vcdMarkAllDirty();
            configGetStrCopy(configOPL, CONFIG_OPL_POPSTARTER_PATH, gPopstarterPath, sizeof(gPopstarterPath));
            // POPSTARTER device TYPE (POPS_DEV_*). Absent in legacy configs: a non-empty custom
            // popstarter_path migrates to Custom (honour the old override); otherwise Default (cwd).
            if (!configGetInt(configOPL, CONFIG_OPL_POPSTARTER_DEVICE, &gPopstarterDevice))
                gPopstarterDevice = (gPopstarterPath[0] != '\0') ? POPS_DEV_CUSTOM : POPS_DEV_DEFAULT;
            if (!configGetInt(configOPL, CONFIG_OPL_POPSTARTER_RETROGEM_GAMEID, &gPopstarterRetroGemGameID))
                gPopstarterRetroGemGameID = 1;
            configGetInt(configOPL, CONFIG_OPL_BDMA_SOURCE, &gBdmaSource);
            configGetInt(configOPL, CONFIG_OPL_BDMA_APPLY, &gBdmaApplyOnLaunch);
            configGetInt(configOPL, CONFIG_OPL_VCD_USB_BDMA, &gVcdUsbBdmaMode);
            // A hand-edited or future-written value must not index past the three enum labels.
            if (gVcdUsbBdmaMode < VCD_USB_BDMA_ASK || gVcdUsbBdmaMode > VCD_USB_BDMA_FAT32)
                gVcdUsbBdmaMode = VCD_USB_BDMA_ASK;
            configGetInt(configOPL, CONFIG_OPL_VCD_HIDE_GAMEID, &gVcdHideGameId);
            configGetInt(configOPL, CONFIG_OPL_VCD_FIRST_DISC_ONLY, &gVcdFirstDiscOnly);
            configGetInt(configOPL, CONFIG_OPL_VCD_SHOW_PP_POPS, &gVcdShowPpPops);
            configReadNeutrinoGlobals(configOPL); // shared with miniInit's autolaunch path
            configGetInt(configOPL, CONFIG_OPL_ENABLE_BGART, &gEnableBGArt);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_ART_TAR, &gEnableArtTar);
            configGetInt(configOPL, CONFIG_OPL_ART_DELAY, &gArtDelay);
            // Keep the stored domain identical to the Artwork page's enum {0,2,5,8} (item 45), so a
            // hand-edited or legacy value cannot render as a delay the UI is unable to express.
            // An out-of-domain value falls back to 0, matching setDefaults().
            if (gArtDelay != 0 && gArtDelay != 2 && gArtDelay != 5 && gArtDelay != 8)
                gArtDelay = 0;
            configGetInt(configOPL, CONFIG_OPL_FOLDER_NAV, &gEnableFolderNav);
            configGetColor(configOPL, CONFIG_OPL_PLAS_BLEND_COLOR, gDefaultPlasBlendColor);
            configGetInt(configOPL, CONFIG_OPL_COVERFLOW_COUNT, &gCoverflowCount);
            configGetInt(configOPL, CONFIG_OPL_COVERFLOW_SCALE, &gCoverflowCenterScale);
            configGetInt(configOPL, CONFIG_OPL_COVERFLOW_ANIM, &gCoverflowAnimSpeed);
            configGetInt(configOPL, CONFIG_OPL_COVERFLOW_DIM, &gCoverflowDimCovers);
            // clamp count to {3,5} on load -- defends a hand-edited conf.cfg
            gCoverflowCount = (gCoverflowCount == 5) ? 5 : 3;
            // clamp the remaining coverflow values too -- a hand-edited conf.cfg
            // otherwise feeds unbounded ints into signed render math
            if (gCoverflowCenterScale < 0)
                gCoverflowCenterScale = 0;
            else if (gCoverflowCenterScale > 1000)
                gCoverflowCenterScale = 1000;
            if (gCoverflowAnimSpeed < 0)
                gCoverflowAnimSpeed = 0;
            else if (gCoverflowAnimSpeed > 5000)
                gCoverflowAnimSpeed = 5000;
            gCoverflowDimCovers = gCoverflowDimCovers ? 1 : 0;
            configGetInt(configOPL, CONFIG_OPL_ENABLE_ILINK, &gEnableILK);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_MX4SIO, &gEnableMX4SIO);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_BDMHDD, &gEnableBdmHDD);
            int udpbdKeyPresent = configGetInt(configOPL, CONFIG_OPL_ENABLE_UDPBD, &gEnableUDPBD);
            configGetInt(configOPL, CONFIG_OPL_NET_BOOT_PROTOCOL, &gNetBootProtocol);
            // Unified network-protocol selector (single SMAP NIC -> at most one transport per session).
            // Read the new key if present (authoritative); otherwise DERIVE it from the three legacy keys,
            // preserving the historical "network BDM wins over SMB" precedence (imported/hand-edited configs
            // could set both; at boot UDPBD loaded first, so it won). NET_PROTO_UDPFS (filesystem) has no
            // legacy encoding, so it is only ever reached by an explicit new-format value -- backward-safe.
            // The legacy branch keys off the FILE's enable_udpbd, not the defaulted global -- a legacy
            // config must only ever derive from what IT expressed, never inherit a defaulted enable flag.
            if (!configGetInt(configOPL, CONFIG_OPL_NETWORK_PROTOCOL, &gNetworkProtocol)) {
                if (udpbdKeyPresent)
                    gNetworkProtocol = gEnableUDPBD ? ((gNetBootProtocol == NET_BOOT_UDPFS) ? NET_PROTO_UDPFSBD : NET_PROTO_UDPBD) : ((gETHStartMode != START_MODE_DISABLED) ? NET_PROTO_SMB : NET_PROTO_OFF);
                else if (gETHStartMode != START_MODE_DISABLED)
                    gNetworkProtocol = NET_PROTO_SMB;
                // else: the file never expressed ANY network choice -> the shipped default stands (Off)
            }
            // UDPBD (SUDPBDv2) is a first-class protocol, NOT folded away: it is wire-incompatible with
            // UDPRDMA (SUDPBDv2 on 0xBDBD vs UDPFS on 0xF5F6), so users still on the older udpbd-server
            // must be able to keep it. A saved or legacy-derived NET_PROTO_UDPBD is preserved as-is.
            // Re-derive the legacy shadows from the authoritative selector so downstream consumers
            // (ethsupport start path, system.c getDeviceName, bdmsupport) stay consistent no matter which
            // config format was loaded. SMB keeps its prior Auto/Manual start-mode; a fresh SMB pick that
            // had eth_mode=0 gets Manual. Any non-SMB protocol forces SMB off (preserves UDPBD-wins).
            // NOTE(rebuild): the fork also loads the SMB dialect (gSMBDialect, SMBv1/SMB2) here --
            // that returns with checklist item 4; until then the rebuild speaks SMBv1 unconditionally.
            gEnableUDPBD = (gNetworkProtocol == NET_PROTO_UDPBD || gNetworkProtocol == NET_PROTO_UDPFSBD);
            gNetBootProtocol = (gNetworkProtocol == NET_PROTO_UDPFSBD) ? NET_BOOT_UDPFS : NET_BOOT_UDPBD;
            if (gNetworkProtocol == NET_PROTO_SMB) {
                if (gETHStartMode == START_MODE_DISABLED)
                    gETHStartMode = START_MODE_MANUAL;
            } else {
                gETHStartMode = START_MODE_DISABLED;
            }

            // Network start row (Off/Manual/Auto). A config predating this field has no net_start_mode
            // key -- derive it from the protocol we just resolved so an existing user keeps working:
            //   OFF   -> Off (Row 1); SMB -> its persisted eth_mode (so a prior SMB=Auto survives);
            //   UDPFS -> Manual (matches the old hardcoded UDPFS_MODE start gate); block -> Auto
            //   (matches the old bdm boot-connect for UDPBD/UDPFSBD, where start mode is cosmetic).
            if (!configGetInt(configOPL, CONFIG_OPL_NET_START_MODE, &gNetStartMode)) {
                if (gNetworkProtocol == NET_PROTO_OFF)
                    gNetStartMode = START_MODE_DISABLED;
                else if (gNetworkProtocol == NET_PROTO_SMB)
                    gNetStartMode = gETHStartMode;
                else if (gNetworkProtocol == NET_PROTO_UDPFS)
                    gNetStartMode = START_MODE_MANUAL;
                else
                    gNetStartMode = START_MODE_AUTO; // UDPFSBD / UDPBD block
            }
            // Reconcile the two persisted halves; Network Start Mode is the user's explicit gate.
            // A disabled start row must not be promoted to Manual merely because an older protocol
            // key still says SMB. Conversely, Protocol Off keeps the start row disabled so opening
            // the dialog cannot silently revive a transport.
            if (gNetStartMode == START_MODE_DISABLED || gNetworkProtocol == NET_PROTO_OFF) {
                gNetworkProtocol = NET_PROTO_OFF;
                gNetStartMode = START_MODE_DISABLED;
            } else if (gNetStartMode < START_MODE_MANUAL || gNetStartMode > START_MODE_AUTO) {
                gNetStartMode = START_MODE_MANUAL;
            }
            // Re-derive the legacy shadows after reconciliation, because the gate may have changed
            // a stale live protocol to OFF.
            gEnableUDPBD = (gNetworkProtocol == NET_PROTO_UDPBD || gNetworkProtocol == NET_PROTO_UDPFSBD);
            gNetBootProtocol = (gNetworkProtocol == NET_PROTO_UDPFSBD) ? NET_BOOT_UDPFS : NET_BOOT_UDPBD;
            gETHStartMode = (gNetworkProtocol == NET_PROTO_SMB) ? gNetStartMode : START_MODE_DISABLED;
            configGetInt(configOPL, CONFIG_OPL_SFX, &gEnableSFX);
            configGetInt(configOPL, CONFIG_OPL_RUMBLE, &gEnableRumble);
            configGetInt(configOPL, CONFIG_OPL_BOOT_SND, &gEnableBootSND);
            configGetInt(configOPL, CONFIG_OPL_BGM, &gEnableBGM);
            configGetInt(configOPL, CONFIG_OPL_SFX_VOLUME, &gSFXVolume);
            configGetInt(configOPL, CONFIG_OPL_BOOT_SND_VOLUME, &gBootSndVolume);
            configGetInt(configOPL, CONFIG_OPL_BGM_VOLUME, &gBGMVolume);
            configGetStrCopy(configOPL, CONFIG_OPL_DEFAULT_BGM_PATH, gDefaultBGMPath, sizeof(gDefaultBGMPath));
        }

        // BOOT-DEVICE RECONCILE. The device OPL is RUNNING FROM always has its transport enabled.
        //
        // This existed for ATA alone, and rebuild-137b bolted MX4SIO on beside it. Both were the same
        // rule written twice, and writing it twice is what hid the fact that the two commonest
        // transports had no rule at all. The reasoning was never ATA-specific: the resolve step
        // force-loads whatever transport the boot prefix needs while IGNORING every gEnable* flag -- it
        // has to, because those flags live in the config the resolve exists to make readable -- so the
        // flag has to be repaired AFTER the read or the device OPL booted from has no tab, and the next
        // save persists the contradiction.
        //
        // What that omission actually cost: setDefaults ships gEnableUSB = 0 ("opt-in, like the other
        // BDM transports"), so a USB boot whose config is fresh, missing or unreadable came up with
        // gEnableUSB still 0. bdmUpdateDeviceData's publish gate then reads
        // bdmTransportEnabled(BDM_TYPE_USB) == 0 and WITHHOLDS the page -- no tab for the stick OPL is
        // running from, on the most common setup there is. Same for iLink. The MX4SIO report is what
        // exposed this, but it was never an MX4SIO bug.
        //
        // Runs whether or not a config file was found, AFTER the read so the file cannot overwrite it,
        // and persists so the state stops being contradictory. Setting these inside bdmResolveBootDir
        // instead is ineffective -- it runs before this read.
        //
        // Deliberately NOT touched here:
        //   - gBDMStartMode. This grants the TRANSPORT, not a tab. Config IO reaches massN: directly
        //     and does not need the device page, so "I boot from this stick but browse games on the
        //     HDD only" stays a valid, respected choice. Forcing Auto would overrule it.
        //   - BDM_TYPE_UDPBD. bdmResolveBootDir returns -1 for it without ever resolving (a network
        //     block device has no local mount at config-load time), and gEnableUDPBD is NIC-exclusive
        //     with the SMB/ETH stack -- forcing it on could tear down a working SMB session to serve a
        //     boot device that was never resolved in the first place.
        {
            const char *bootDevKey = NULL;
            int *bootDevFlag = NULL;

            switch (gBootDirBdmType) {
                case BDM_TYPE_USB:
                    bootDevKey = CONFIG_OPL_ENABLE_USB;
                    bootDevFlag = &gEnableUSB;
                    break;
                case BDM_TYPE_ILINK:
                    bootDevKey = CONFIG_OPL_ENABLE_ILINK;
                    bootDevFlag = &gEnableILK;
                    break;
                case BDM_TYPE_SDC:
                    bootDevKey = CONFIG_OPL_ENABLE_MX4SIO;
                    bootDevFlag = &gEnableMX4SIO;
                    break;
                case BDM_TYPE_ATA:
                    bootDevKey = CONFIG_OPL_ENABLE_BDMHDD;
                    bootDevFlag = &gEnableBdmHDD;
                    break;
                default:
                    break;
            }

            if (bootDevFlag != NULL && !*bootDevFlag) {
                LOG("CONFIG boot device is BDM type %d but its transport was off -- enabling %s\n",
                    gBootDirBdmType, bootDevKey);
                *bootDevFlag = 1;
                configSetInt(configGetByType(CONFIG_OPL), bootDevKey, *bootDevFlag);
            }
        }
    }

    if (lscstatus & CONFIG_NETWORK) {
        if (!(result & CONFIG_NETWORK)) {
            result = tryAlternateDevice(lscstatus);
        }

        if (result & CONFIG_NETWORK) {
            config_set_t *configNet = configGetByType(CONFIG_NETWORK);

            configGetInt(configNet, CONFIG_NET_ETH_LINKM, &gETHOpMode);

            configGetInt(configNet, CONFIG_NET_PS2_DHCP, &ps2_ip_use_dhcp);
            configGetInt(configNet, CONFIG_NET_SMB_NBNS, &gPCShareAddressIsNetBIOS);
            configGetStrCopy(configNet, CONFIG_NET_SMB_NB_ADDR, gPCShareNBAddress, sizeof(gPCShareNBAddress));

            if (configGetStr(configNet, CONFIG_NET_SMB_IP_ADDR, &temp))
                sscanf(temp, "%d.%d.%d.%d", &pc_ip[0], &pc_ip[1], &pc_ip[2], &pc_ip[3]);

            configGetInt(configNet, CONFIG_NET_SMB_PORT, &gPCPort);

            configGetStrCopy(configNet, CONFIG_NET_SMB_SHARE, gPCShareName, sizeof(gPCShareName));
            configGetStrCopy(configNet, CONFIG_NET_SMB_USER, gPCUserName, sizeof(gPCUserName));
            configGetStrCopy(configNet, CONFIG_NET_SMB_PASSW, gPCPassword, sizeof(gPCPassword));

            if (configGetStr(configNet, CONFIG_NET_PS2_IP, &temp))
                sscanf(temp, "%d.%d.%d.%d", &ps2_ip[0], &ps2_ip[1], &ps2_ip[2], &ps2_ip[3]);
            if (configGetStr(configNet, CONFIG_NET_PS2_NETM, &temp))
                sscanf(temp, "%d.%d.%d.%d", &ps2_netmask[0], &ps2_netmask[1], &ps2_netmask[2], &ps2_netmask[3]);
            if (configGetStr(configNet, CONFIG_NET_PS2_GATEW, &temp))
                sscanf(temp, "%d.%d.%d.%d", &ps2_gateway[0], &ps2_gateway[1], &ps2_gateway[2], &ps2_gateway[3]);
            if (configGetStr(configNet, CONFIG_NET_PS2_DNS, &temp))
                sscanf(temp, "%d.%d.%d.%d", &ps2_dns[0], &ps2_dns[1], &ps2_dns[2], &ps2_dns[3]);

            configGetStrCopy(configNet, CONFIG_NET_NBD_DEFAULT_EXPORT, gExportName, sizeof(gExportName));
        }
    }

    // A UDP transport binds the ministack to the STATIC PS2 IP fields (it has no DHCP client), so IP
    // Type = DHCP means whatever stale/default address sits there gets used -- discovery then fails
    // with an empty games page and no error. The Device-Settings dialog warns only at the moment of
    // switching protocols; surface it as a boot toast too so an already-configured user sees it.
    showNetDhcpPopup = (ps2_ip_use_dhcp &&
                        (gNetworkProtocol == NET_PROTO_UDPFS || gNetworkProtocol == NET_PROTO_UDPFSBD ||
                         gNetworkProtocol == NET_PROTO_UDPBD));

    applyConfig(themeID, langID, 0);

    lscret = result;
    lscstatus = 0;
    // Only claim "Config loaded from ..." when a config file was actually READ. The toast used to
    // fire unconditionally, so with no config anywhere it still announced a device -- on hardware
    // that read as "it loaded MY settings from mass0:" when nothing of the sort happened.
    showCfgPopup = (result & CONFIG_OPL) ? 1 : 0;
}

static int trySaveConfigMMCE(int types)
{
    DIR *dir = opendir("mmce0:");
    if (dir != NULL) {
        closedir(dir);
        configSetMove("mmce0:");
        return configWriteMulti(types);
    }

    dir = opendir("mmce1:");
    if (dir != NULL) {
        closedir(dir);
        configSetMove("mmce1:");
        return configWriteMulti(types);
    }

    return -ENOENT;
}

static int trySaveConfigBDM(int types)
{
    char path[64];
    int bdm_result;

    // SAVE path: current filename only -- no legacy probe. Writing under the legacy name would
    // un-migrate an install that the load path above just migrated forward.
    // check USB
    bdm_result = bdmFindPartition(path, CONFIG_OPL_FILENAME, 1);
    // if not on USB, check BDM HDD
    if (bdm_result == 0) {
        // wait for up to 5 seconds for the HDD to spin up and become accessible...
        if (hddLoadModules() >= 0 && bdmHDDIsPresent(5000)) {
            bdm_result = bdmFindPartition(path, CONFIG_OPL_FILENAME, 1);
        }
    }

    if (bdm_result) {
        configSetMove(path);
        return configWriteMulti(types);
    }

    return -ENOENT;
}

static int trySaveConfigHDD(int types)
{
    hddLoadModules();
    hddLoadSupportModules();
    // Check that the formatted & usable HDD is connected.
    if (hddCheck() == 0 && gHDDPrefix != NULL) {
        configSetMove(gHDDPrefix);
        return configWriteMulti(types);
    }

    return -ENOENT;
}

static int trySaveConfigMC(int types)
{
    configSetMove(NULL);
    return configWriteMulti(types);
}

static int trySaveAlternateDevice(int types)
{
    char pwd[64] = {0};
    int value;

    if (gBootDir[0] != '\0')
        snprintf(pwd, sizeof(pwd), "%s", gBootDir);
    else if (getcwd(pwd, sizeof(pwd)) == NULL)
        pwd[0] = '\0';

    // First, try the device that OPL booted from.
    if (!strncmp(pwd, "hdd", 3) || !strncmp(pwd, "pfs", 3)) {
        if ((value = trySaveConfigHDD(types)) > 0)
            return value;
    } else if (!strncmp(pwd, "mass", 4) && (pwd[4] == ':' || pwd[5] == ':')) {
        if ((value = trySaveConfigBDM(types)) > 0)
            return value;
    } else if (!strncmp(pwd, "mmce", 4) && (pwd[4] == ':' || pwd[5] == ':')) {
        if ((value = trySaveConfigMMCE(types)) > 0)
            return value;
    }

    // Config was not saved to the boot device. Try all supported devices.
    // Try memory cards
    if (sysCheckMC() >= 0) {
        if ((value = trySaveConfigMC(types)) > 0)
            return value;
    }
    // Try MMCE
    if ((value = trySaveConfigMMCE(types)) > 0)
        return value;
    // Try a USB device
    if ((value = trySaveConfigBDM(types)) > 0)
        return value;
    // Try the HDD
    if ((value = trySaveConfigHDD(types)) > 0)
        return value;

    // We tried everything, but...
    return 0;
}

// configWriteMulti SUMS per-set results, and configWrite returns 1 for an UNMODIFIED set without
// touching the disk -- so a failed write of the master settings can hide behind untouched sibling
// sets and the save reports success ("Settings saved" toast, no retry). Concretely: OPL modified and
// failing returns 0, NETWORK and GAME untouched return 1 each, the sum is 2, and 2 > 0 reads as
// success. configWrite clears a set's modified flag only when its write actually succeeded, and
// _saveConfig configSet*s every set it means to save (marking it modified) -- so any REQUESTED set
// still marked modified after the write IS a failed write. Returns 0 in that case, the raw sum
// otherwise.
static int configWriteChecked(int types)
{
    int result = configWriteMulti(types);
    if (result > 0) {
        for (int bit = 1; bit < (1 << CONFIG_INDEX_COUNT); bit <<= 1) {
            if (types & bit) {
                config_set_t *cfg = configGetByType(bit); // NULL only if a requested set was never allocated
                if (cfg != NULL && cfg->modified)
                    return 0;
            }
        }
    }
    return result;
}

static const char *configFileName(const config_set_t *config)
{
    const char *name;

    if (config == NULL || config->filename == NULL)
        return NULL;

    name = strrchr(config->filename, '/');
    if (name != NULL)
        return name + 1;
    name = strrchr(config->filename, ':');
    return name != NULL ? name + 1 : config->filename;
}

// The source picker does not remount live pfs0:. When the next boot's APA home differs from
// the live one, persist each changed global set through pfs1: first, then let the selector commit.
// This keeps a failed settings write from stranding the next boot on a home that never received it.
static int saveSelectedHddOplHome(int types)
{
    char prefix[64];
    char path[256];
    int selection = hddGetOplHomeSelection();
    int result = 1;

    snprintf(gLastSaveTarget, sizeof(gLastSaveTarget), "%s",
             selection == HDD_OPL_HOME_PLUS ? "hdd0:+OPL" : "hdd0:__common/OPL");
    if (!hddMountSelectedOplHome(prefix, sizeof(prefix))) {
        gLastSaveErrno = EIO;
        return 0;
    }

    for (int bit = 1; bit < (1 << CONFIG_INDEX_COUNT); bit <<= 1) {
        config_set_t *source;
        config_set_t *target;
        const char *name;
        int n;

        if (!(types & bit))
            continue;

        source = configGetByType(bit);
        // configWrite() only materializes a set that actually changed. Preserve that normal
        // contract so a selector-only save neither creates optional empty files nor overwrites an
        // existing target-side config that the user did not edit this session.
        if (source == NULL || !source->modified)
            continue;

        name = configFileName(source);
        n = name == NULL ? -1 : snprintf(path, sizeof(path), "%s%s", prefix, name);
        if (n < 0 || n >= (int)sizeof(path)) {
            gLastSaveErrno = EIO;
            result = 0;
            break;
        }

        // Keep the live sets pointing at their pfs0: home. The target is an isolated copy that
        // carries the live format and values, so pfs1: can be unmounted before selector commit.
        target = configAlloc(source->type, NULL, path);
        if (target == NULL) {
            gLastSaveErrno = EIO;
            result = 0;
            break;
        }
        target->format = source->format;
        configMerge(target, source);
        target->modified = 1;

        if (!configWrite(target) || target->modified) {
            if (gLastSaveErrno == 0)
                gLastSaveErrno = EIO;
            result = 0;
            configFree(target);
            break;
        }
        configFree(target);
        source->modified = 0;
    }

    hddUnmountSelectedOplHome();
    return result;
}

static int commitSelectedHddOplHome(void)
{
    if (hddCommitOplHomeSelection())
        return 1;

    snprintf(gLastSaveTarget, sizeof(gLastSaveTarget), "%s", "hdd0:__common/OPL/conf_hdd.cfg");
    gLastSaveErrno = EIO;
    return 0;
}

static void _saveConfig()
{
    char temp[256];
    char customSettingsTarget[sizeof(gCustomSettingsPath)] = {0};
    int customSettingsExplicit = 0;

    if (lscstatus & CONFIG_OPL) {
        config_set_t *configOPL = configGetByType(CONFIG_OPL);
        configSetInt(configOPL, CONFIG_OPL_SCROLLING, gScrollSpeed);
        configSetStr(configOPL, CONFIG_OPL_THEME, thmGetValue());
        configSetStr(configOPL, CONFIG_OPL_LANGUAGE, lngGetValue());
        configSetColor(configOPL, CONFIG_OPL_BGCOLOR, gDefaultBgColor);
        configSetColor(configOPL, CONFIG_OPL_TEXTCOLOR, gDefaultTextColor);
        configSetColor(configOPL, CONFIG_OPL_UI_TEXTCOLOR, gDefaultUITextColor);
        configSetColor(configOPL, CONFIG_OPL_SEL_TEXTCOLOR, gDefaultSelTextColor);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_NOTIFICATIONS, gEnableNotifications);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_COVERART, gEnableArt);
        configSetInt(configOPL, CONFIG_OPL_WIDESCREEN, gWideScreen);
        configSetInt(configOPL, CONFIG_OPL_VMODE, gVMode);
        configSetInt(configOPL, CONFIG_OPL_XOFF, gXOff);
        configSetInt(configOPL, CONFIG_OPL_YOFF, gYOff);
        configSetInt(configOPL, CONFIG_OPL_OVERSCAN, gOverscan);
        configSetInt(configOPL, CONFIG_OPL_DISABLE_DEBUG, gEnableDebug);
        configSetInt(configOPL, CONFIG_OPL_PS2LOGO, gPS2Logo);
        configSetInt(configOPL, CONFIG_OPL_HDD_GAME_LIST_CACHE, gHDDGameListCache);
        configSetStr(configOPL, CONFIG_OPL_EXIT_PATH, gExitPath);
        configSetStr(configOPL, CONFIG_OPL_CUSTOM_SETTINGS_PATH, gCustomSettingsPath);
        configSetInt(configOPL, CONFIG_OPL_AUTO_SORT, gAutosort);
        configSetInt(configOPL, CONFIG_OPL_AUTO_REFRESH, gAutoRefresh);
        configSetInt(configOPL, CONFIG_OPL_DEFAULT_DEVICE, gDefaultDevice);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_WRITE, gEnableWrite);
        configSetInt(configOPL, CONFIG_OPL_HDD_SPINDOWN, gHDDSpindown);
        configSetStr(configOPL, CONFIG_OPL_BDM_PREFIX, gBDMPrefix);
        configSetStr(configOPL, CONFIG_OPL_ETH_PREFIX, gETHPrefix);
        configSetInt(configOPL, CONFIG_OPL_REMEMBER_LAST, gRememberLastPlayed);
        configSetInt(configOPL, CONFIG_OPL_AUTOSTART_LAST, gAutoStartLastPlayed);
        configSetInt(configOPL, CONFIG_OPL_BDM_MODE, gBDMStartMode);
        configSetInt(configOPL, CONFIG_OPL_HDD_MODE, gHDDStartMode);
        configSetInt(configOPL, CONFIG_OPL_ETH_MODE, gETHStartMode);
        configSetInt(configOPL, CONFIG_OPL_APP_MODE, gAPPStartMode);
        configSetStr(configOPL, CONFIG_OPL_MMCE_PREFIX, gMMCEPrefix);
        configSetInt(configOPL, CONFIG_OPL_MMCE_MODE, gMMCEStartMode);
        configSetInt(configOPL, CONFIG_OPL_MMCE_SLOT, gMMCESlot);
        configSetInt(configOPL, CONFIG_OPL_MMCEIGR_SLOT, gMMCEIGRSlot);
        configSetInt(configOPL, CONFIG_OPL_MMCE_GAMEID, gMMCEEnableGameID);
        configSetInt(configOPL, CONFIG_OPL_MMCE_WAIT_CYCLES, gMMCEAckWaitCycles);
        configSetInt(configOPL, CONFIG_OPL_MMCE_USE_ALARMS, gMMCEUseAlarms);
        configSetInt(configOPL, CONFIG_OPL_MMCE_PACING_MIGR, 1);
        configSetInt(configOPL, CONFIG_OPL_BDM_CACHE, bdmCacheSize);
        configSetInt(configOPL, CONFIG_OPL_HDD_CACHE, hddCacheSize);
        configSetInt(configOPL, CONFIG_OPL_SMB_CACHE, smbCacheSize);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_USB, gEnableUSB);
        configSetInt(configOPL, CONFIG_OPL_FAV_MODE, gFAVStartMode);
        configSetInt(configOPL, CONFIG_OPL_APPLY_GAMEID, gApplyGameID);
        configSetInt(configOPL, CONFIG_OPL_DEFAULT_GAME_VIEW, gDefaultGameView);
        configSetStr(configOPL, CONFIG_OPL_POPSTARTER_PATH, gPopstarterPath);
        configSetInt(configOPL, CONFIG_OPL_POPSTARTER_DEVICE, gPopstarterDevice);
        configSetInt(configOPL, CONFIG_OPL_POPSTARTER_RETROGEM_GAMEID, gPopstarterRetroGemGameID);
        configSetInt(configOPL, CONFIG_OPL_BDMA_SOURCE, gBdmaSource);
        configSetInt(configOPL, CONFIG_OPL_BDMA_APPLY, gBdmaApplyOnLaunch);
        configSetInt(configOPL, CONFIG_OPL_VCD_USB_BDMA, gVcdUsbBdmaMode);
        configSetInt(configOPL, CONFIG_OPL_VCD_HIDE_GAMEID, gVcdHideGameId);
        configSetInt(configOPL, CONFIG_OPL_VCD_FIRST_DISC_ONLY, gVcdFirstDiscOnly);
        configSetInt(configOPL, CONFIG_OPL_VCD_SHOW_PP_POPS, gVcdShowPpPops);
        configSetStr(configOPL, CONFIG_OPL_NEUTRINO_ARGS, gNeutrinoArgs);
        configSetStr(configOPL, CONFIG_OPL_NEUTRINO_PATH, gNeutrinoPath);
        configSetInt(configOPL, CONFIG_OPL_DEFAULT_CORE, gDefaultCoreLoader);
        configSetInt(configOPL, CONFIG_OPL_NEUTRINO_VIDEO, gNeutrinoVideoDefault);
        configSetInt(configOPL, CONFIG_OPL_NEUTRINO_GSMCOMP, gNeutrinoGsmCompDefault);
        configSetInt(configOPL, CONFIG_OPL_NEUTRINO_DEVTYPE, gNeutrinoDevice);
        configSetInt(configOPL, CONFIG_OPL_NEUTRINO_ELF_ARG, gNeutrinoElfArg);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_BGART, gEnableBGArt);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_ART_TAR, gEnableArtTar);
        configSetInt(configOPL, CONFIG_OPL_ART_DELAY, gArtDelay);
        configSetInt(configOPL, CONFIG_OPL_FOLDER_NAV, gEnableFolderNav);
        configSetColor(configOPL, CONFIG_OPL_PLAS_BLEND_COLOR, gDefaultPlasBlendColor);
        configSetInt(configOPL, CONFIG_OPL_COVERFLOW_COUNT, gCoverflowCount);
        configSetInt(configOPL, CONFIG_OPL_COVERFLOW_SCALE, gCoverflowCenterScale);
        configSetInt(configOPL, CONFIG_OPL_COVERFLOW_ANIM, gCoverflowAnimSpeed);
        configSetInt(configOPL, CONFIG_OPL_COVERFLOW_DIM, gCoverflowDimCovers);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_ILINK, gEnableILK);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_MX4SIO, gEnableMX4SIO);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_BDMHDD, gEnableBdmHDD);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_UDPBD, gEnableUDPBD);
        configSetInt(configOPL, CONFIG_OPL_NET_BOOT_PROTOCOL, gNetBootProtocol);
        // Dual-write: the authoritative unified selector PLUS the three legacy keys (derived shadows),
        // so a config saved by this build still boots correctly on an older OPL that only reads the legacy keys.
        // NOTE(rebuild): the fork also persists the SMB dialect here (item 4).
        configSetInt(configOPL, CONFIG_OPL_NETWORK_PROTOCOL, gNetworkProtocol);
        configSetInt(configOPL, CONFIG_OPL_NET_START_MODE, gNetStartMode);
        configSetInt(configOPL, CONFIG_OPL_SFX, gEnableSFX);
        configSetInt(configOPL, CONFIG_OPL_RUMBLE, gEnableRumble);
        configSetInt(configOPL, CONFIG_OPL_BOOT_SND, gEnableBootSND);
        configSetInt(configOPL, CONFIG_OPL_BGM, gEnableBGM);
        configSetInt(configOPL, CONFIG_OPL_SFX_VOLUME, gSFXVolume);
        configSetInt(configOPL, CONFIG_OPL_BOOT_SND_VOLUME, gBootSndVolume);
        configSetInt(configOPL, CONFIG_OPL_BGM_VOLUME, gBGMVolume);
        configSetStr(configOPL, CONFIG_OPL_DEFAULT_BGM_PATH, gDefaultBGMPath);
        configSetInt(configOPL, CONFIG_OPL_XSENSITIVITY, gXSensitivity);
        configSetInt(configOPL, CONFIG_OPL_YSENSITIVITY, gYSensitivity);

        configSetInt(configOPL, CONFIG_OPL_SWAP_SEL_BUTTON, gSelectButton == KEY_CIRCLE ? 0 : 1);
    }

    if (lscstatus & CONFIG_NETWORK) {
        config_set_t *configNet = configGetByType(CONFIG_NETWORK);

        snprintf(temp, sizeof(temp), "%d.%d.%d.%d", ps2_ip[0], ps2_ip[1], ps2_ip[2], ps2_ip[3]);
        configSetStr(configNet, CONFIG_NET_PS2_IP, temp);
        snprintf(temp, sizeof(temp), "%d.%d.%d.%d", ps2_netmask[0], ps2_netmask[1], ps2_netmask[2], ps2_netmask[3]);
        configSetStr(configNet, CONFIG_NET_PS2_NETM, temp);
        snprintf(temp, sizeof(temp), "%d.%d.%d.%d", ps2_gateway[0], ps2_gateway[1], ps2_gateway[2], ps2_gateway[3]);
        configSetStr(configNet, CONFIG_NET_PS2_GATEW, temp);
        snprintf(temp, sizeof(temp), "%d.%d.%d.%d", ps2_dns[0], ps2_dns[1], ps2_dns[2], ps2_dns[3]);
        configSetStr(configNet, CONFIG_NET_PS2_DNS, temp);

        configSetInt(configNet, CONFIG_NET_ETH_LINKM, gETHOpMode);
        configSetInt(configNet, CONFIG_NET_PS2_DHCP, ps2_ip_use_dhcp);
        configSetInt(configNet, CONFIG_NET_SMB_NBNS, gPCShareAddressIsNetBIOS);
        configSetStr(configNet, CONFIG_NET_SMB_NB_ADDR, gPCShareNBAddress);
        snprintf(temp, sizeof(temp), "%d.%d.%d.%d", pc_ip[0], pc_ip[1], pc_ip[2], pc_ip[3]);
        configSetStr(configNet, CONFIG_NET_SMB_IP_ADDR, temp);
        configSetInt(configNet, CONFIG_NET_SMB_PORT, gPCPort);
        configSetStr(configNet, CONFIG_NET_SMB_SHARE, gPCShareName);
        configSetStr(configNet, CONFIG_NET_SMB_USER, gPCUserName);
        configSetStr(configNet, CONFIG_NET_SMB_PASSW, gPCPassword);
    }

    // A current APA session keeps pfs0: mounted on its original data home until restart. When the
    // selected next-boot home differs, write only the changed sets through pfs1: and commit the
    // selector only after that write succeeds. The normal automatic +OPL case still uses pfs0:.
    if (gBootHomeApa && gCustomSettingsPath[0] == '\0' && hddOplHomeSelectionNeedsTargetSave()) {
        if (!saveSelectedHddOplHome(lscstatus) ||
            (hddOplHomeSelectionPending() && !commitSelectedHddOplHome())) {
            lscret = 0;
            lscstatus = 0;
            return;
        }
        gLastSaveWasStagedOplHome = 1;
        lscret = 1;
        lscstatus = 0;
        return;
    }

    // APA launch identity is raw partition space, never a writable config filesystem. Even when
    // discovery loaded defaults or recovered settings elsewhere, an ordinary APA save without an
    // explicit Custom Settings Path must resolve the existing-PFS ownership chain NOW. This keeps
    // raw hddN: out of configWrite entirely: configured existing target first, then __common/OPL,
    // otherwise fail visibly without creating/formatting/repairing any APA partition.
    if (gBootHomeApa && gCustomSettingsPath[0] == '\0') {
        char hddSaveHome[64];
        if (prepareHddSettingsFallback(hddSaveHome, sizeof(hddSaveHome)) <= 0) {
            if (gOPLPart[0] != '\0')
                snprintf(gLastSaveTarget, sizeof(gLastSaveTarget), "%s", gOPLPart);
            else
                snprintf(gLastSaveTarget, sizeof(gLastSaveTarget), "%s", "hdd0:__common/OPL");
            gLastSaveErrno = ENODEV;
            lscret = 0;
            lscstatus = 0;
            return;
        }
        configSetMove(hddSaveHome);
    }

    char *path = configGetDir();
    // Only for the legacy no-boot-identity case. With a known boot dir the config home IS the boot
    // dir (or the custom path below); stamping mc?:OPL/ + icons here on an appdir-on-MC boot
    // sprouted an unwanted folder on every save, and rewriting the notification to the mc?:
    // wildcard made the "Settings saved to %s" toast name the wrong place (fork gates this
    // identically; FifthFox HW 2026-07-16 is the incident this comment block cites elsewhere).
    //
    // ...and for a NETWORK boot, whose home is a card for exactly the same reason: it has no boot
    // identity it can WRITE at boot time either. rebuild-143 moved that home onto the card and did
    // not move the folder creation with it, so the per-file O_CREAT below opened into a directory
    // that does not exist and every save failed with ENOENT -- reported as "could not write settings
    // to mc0: (error 2)". The boot dir is NON-empty on that path, so the original gate never fired.
    //
    // This is not the FifthFox case, and the second half of the condition is what proves it: the
    // folder is only ever created when the config home ALREADY IS a card. On a deferred boot it is
    // one solely because tryAlternateDevice put it there, and only after sysCheckMC() found a card
    // that was already serving this config. The incident being guarded against is a home that fell
    // back to mc?:OPL BY ACCIDENT on a device that had a perfectly good home of its own; a network
    // boot has no such home, which is exactly what _STR_SETTINGS_NO_HOME has always told the user.
    if ((gBootDir[0] == '\0' || gBootHomeDeferred) && !strncmp(path, "mc", 2)) {
        checkMCFolder();
        configPrepareNotifications(gBaseMCDir);
    }

    // Custom Settings Path is an explicit destination. Non-HDD targets still fail visibly
    // when unreachable. HDD/APA targets are different: they have a guaranteed safe ownership
    // chain (conf_hdd.cfg target -> __common/OPL), so an invalid HDD target falls back there
    // explicitly and the success dialog tells the user which HDD home was actually used.
    if (gCustomSettingsPath[0] != '\0') {
        snprintf(customSettingsTarget, sizeof(customSettingsTarget), "%s", gCustomSettingsPath);
        // Keep the user-facing spelling for any failure before configSetMove changes configGetDir().
        snprintf(gLastSaveTarget, sizeof(gLastSaveTarget), "%s", gCustomSettingsPath);
        if (prepareCustomSettingsPath(customSettingsTarget, sizeof(customSettingsTarget)) < 0) {
            if (isApaSettingsPath(gCustomSettingsPath) &&
                prepareHddSettingsFallback(customSettingsTarget, sizeof(customSettingsTarget)) > 0) {
                LOG("CONFIG HDD custom settings path %s unavailable; falling back safely to %s\n",
                    gCustomSettingsPath, customSettingsTarget);
                configSetMove(customSettingsTarget);
                snprintf(gLastSaveTarget, sizeof(gLastSaveTarget), "%s", customSettingsTarget);
                gHddSettingsFallbackNotice = 1;
                gLastSaveErrno = 0;
            } else {
                LOG("CONFIG custom settings path %s unreachable; aborting explicit save\n", gCustomSettingsPath);
                if (gLastSaveErrno == 0)
                    gLastSaveErrno = ENODEV;
                lscret = 0;
                lscstatus = 0;
                return;
            }
        } else {
            configSetMove(customSettingsTarget);
            customSettingsExplicit = 1;
        }
    }

    lscret = configWriteChecked(lscstatus);

    // Save icons for EVERY memory-card save home, not just the legacy mc?:OPL/ one. The block above
    // only stamps icons when there is no boot identity, so an appdir-on-MC boot (settings beside the
    // ELF) saved fine but left the folder showing as "Corrupted Data" in the PS2/PS3 browser. Keyed
    // on the config file's OWN path, after the custom-path re-home and the write, so the icons land
    // in the folder the settings actually went to -- which is what made the old mc?:OPL/ assumption
    // unsafe to run unconditionally. Off mc this is a no-op.
    if (lscret > 0) {
        config_set_t *oplCfg = configGetByType(CONFIG_OPL);
        if (oplCfg != NULL)
            checkMCSaveIcons(oplCfg->filename);
    }

    // Persist the bootstrap pointer only after the explicit config write succeeded. An explicit
    // target owns the whole save attempt: whether it succeeds or fails, do not retry onto the boot
    // device or an alternate device behind the user's back.
    if (customSettingsExplicit) {
        if (lscret > 0 && !writeConfigPathRedirect(customSettingsTarget)) {
            LOG("CONFIG could not persist settings redirect for %s\n", customSettingsTarget);
            gLastSaveErrno = EIO;
            lscret = 0;
        }
        if (lscret > 0 && hddOplHomeSelectionPending() && !commitSelectedHddOplHome())
            lscret = 0;
        lscstatus = 0;
        return;
    }

    // Boot-device save retry: ask the same concrete BDM identity to register once more, then retry
    // the write. A literal massN: remains that exact slot and is never replaced by another device;
    // a typed launch alias may still rewrite to its verified mount. If the boot device is gone the
    // save fails visibly instead of scattering settings elsewhere.
    if (lscret <= 0 && gBootDir[0] != '\0' && gBootHomeBdm) {
        char before[sizeof(gBootDir)];
        snprintf(before, sizeof(before), "%s", gBootDir);
        if (bdmResolveBootDir(gBootDir, sizeof(gBootDir), gBootElfName, &gBootDirBdmType) > 0) {
            if (strcmp(before, gBootDir) != 0) {
                LOG("BOOT re-resolved boot dir for save: %s -> %s\n", before, gBootDir);
                configSetMove(gBootDir); // keep the pending values; re-point the files to the verified mount
            }
            lscret = configWriteChecked(lscstatus);
        }
    }
    // <= 0, not == 0: configWriteChecked can only return 0 or a positive sum today, but the guard
    // costs nothing and a negative would otherwise read as success here.
    // The alternate-device hunt is ONLY for the legacy no-boot-identity case: with a known boot
    // dir, settings live there (or at the custom path) and a failed write must FAIL VISIBLY -- the
    // save-to-CWD doctrine. Scattering them onto whichever other device answers is how settings
    // ended up on mass0: on a memory-card boot.
    if (gBootDir[0] == '\0') {
        if (lscret <= 0)
            lscret = trySaveAlternateDevice(lscstatus);
        // Record where discovery landed so the NEXT boot skips the hunt (the redirect is a
        // legacy-discovery aid; with a known boot dir the custom-path block above owns it).
        if (lscret > 0)
            writeConfigPathRedirect(configGetDir());
    }
    if (lscret > 0 && hddOplHomeSelectionPending() && !commitSelectedHddOplHome())
        lscret = 0;
    lscstatus = 0;
}

void applyConfig(int themeID, int langID, int skipDeviceRefresh)
{
    // NO padRumbleFlush() here, deliberately: _loadConfig() reaches applyConfig from the IO
    // worker, and every libpad call in pad.c is GUI-thread-only (see guiHandleDeferedIO's note).
    // The GUI-thread callers that need a flush do it themselves before calling in.
    // A deliberate settings apply may make new art available, so clear the .tar "no archive
    // anywhere" latch and let it be probed once more. That latch (tar.c s_inactive[]) is write-once
    // and process-wide with no self-clearing path, so without this a user who boots with the loader
    // already ON and no archive present -- then plugs one in -- keeps getting nothing until a
    // reboot. The Artwork page's own toggle-flip re-arm only covers the case where the toggle
    // CHANGES; this covers the rest.
    // Gate archive and directory index invalidation on full device refresh (skipDeviceRefresh == 0).
    // Pure theme/color UI switches (skipDeviceRefresh == 1) keep the indices warm in memory (issue #488)
    // so returning to List Mode does not re-probe USB 1.1 storage.
    if (skipDeviceRefresh == 0) {
        tarInvalidate(TAR_KIND_ART);
        cacheInvalidateFailMemo();
        artIndexInvalidate(); // a full settings apply is the user's own "I changed something, look again"
    }

    // The bound is FAV_MODE, not APP_MODE. The Game Sources picker offers six entries and the last
    // of them is Favourites (guiShowDeviceConfig deviceNames[], byte-identical to the fork), so an
    // APP_MODE bound silently rewrote the two choices ABOVE it -- Favourites and MMCE -- back to
    // Apps, on every applyConfig AND every boot. Picking "Favourites" as the startup page snapped
    // the row back to Apps before the value was ever written to the config: the whole feature was
    // dead with its picker entry, its enum value and its config key all present. Data half without
    // the code half, again.
    if (gDefaultDevice < 0 || gDefaultDevice > FAV_MODE)
        gDefaultDevice = MMCE_MODE;
    // Favourites is a valid startup page only while the FAV tab is enabled; otherwise a stale
    // choice would boot into a hidden tab with no way back to a visible one.
    if (gDefaultDevice == FAV_MODE && !gFAVStartMode)
        gDefaultDevice = MMCE_MODE;
    if (gDefaultDevice == MMCE_MODE && gMMCEStartMode == START_MODE_DISABLED)
        gDefaultDevice = APP_MODE;

    guiUpdateScrollSpeed();

    guiSetFrameHook(&menuUpdateHook);

    int changed = rmSetMode(0);
    if (changed) {
        bgmMute();
        // reinit the graphics...
        thmReloadScreenExtents();
        guiReloadScreenExtents();
    }

    // theme must be set after color, and lng after theme
    changed = thmSetGuiValue(themeID, changed);
    int langChanged = lngSetGuiValue(langID);

    guiUpdateScreenScale();

    // Check if we should refresh device support as well.
    if (skipDeviceRefresh == 0) {
        initAllSupport(0);

        for (int i = 0; i < MODE_COUNT; i++) {
            if (list_support[i].support == NULL)
                continue;

            moduleUpdateMenuInternal(&list_support[i], changed, langChanged);
        }
    } else {
        if (changed) {
            // Same serialization as moduleUpdateMenuInternal: never realloc the per-item art-pair
            // arrays while the GUI thread may be drawing through them.
            guiLock();
            for (int i = 0; i < MODE_COUNT; i++) {
                if (list_support[i].support && list_support[i].subMenu)
                    submenuRebuildCache(list_support[i].subMenu);
            }
            guiUnlock();
        }
    }

    bgmUnMute();

#ifdef __DEBUG
    debugApplyConfig();
#endif
}

int loadConfig(int types)
{
    lscstatus = types;
    lscret = 0;

    guiHandleDeferedIO(&lscstatus, _l(_STR_LOADING_SETTINGS), IO_CUSTOM_SIMPLEACTION, &_loadConfig, OPL_DEFERRED_IO_TIMEOUT_MS);

    return lscret;
}

int saveConfig(int types, int showUI)
{
    char notification[128];
    lscstatus = types;
    lscret = 0;
    // Reset before the attempt. gLastSaveErrno is only ever ASSIGNED at the three real failure sites
    // in configWrite, never cleared -- so a stale value from an earlier save could be reported for a
    // later one, and (worse) a save that never reached configWrite at all reported whatever was left,
    // usually the 0 that produced the nonsensical "(error 0)".
    gLastSaveErrno = 0;
    gLastSaveTarget[0] = '\0';
    gLastSaveWasStagedOplHome = 0;
    gHddSettingsFallbackNotice = 0;

    guiHandleDeferedIO(&lscstatus, _l(_STR_SAVING_SETTINGS), IO_CUSTOM_SIMPLEACTION, &_saveConfig, OPL_DEFERRED_IO_TIMEOUT_MS);

    if (showUI) {
        if (lscret) {
            char *path = configGetDir();
            const char *displayHome = gLastSaveWasStagedOplHome ? gLastSaveTarget : path;
            int savedOnHddHome = gLastSaveWasStagedOplHome ||
                                 (path != NULL && !strncmp(path, "pfs", 3) && gOPLPart[0] != '\0');

            // pfs0: is only the transient mount name. For every successful HDD save, show the
            // physical APA/PFS owner the user can actually find in a partition browser.
            if (savedOnHddHome && !gLastSaveWasStagedOplHome) {
                if (!strcmp(gOPLPart, "hdd0:__common"))
                    displayHome = "hdd0:__common/OPL";
                else
                    displayHome = gOPLPart;
            }

            if (gHddSettingsFallbackNotice || (!gLastSaveWasStagedOplHome && gBootHddCommonFallback && savedOnHddHome))
                snprintf(notification, sizeof(notification), _l(_STR_SETTINGS_HDD_FALLBACK), displayHome);
            else
                snprintf(notification, sizeof(notification), _l(_STR_SETTINGS_SAVED), displayHome);

            guiMsgBox(notification, 0, NULL);
        } else {
            // Distinguish "the write failed" from "the write never ran". guiHandleDeferedIO abandons
            // its wait after OPL_DEFERRED_IO_TIMEOUT_MS and clears the status WITHOUT _saveConfig
            // having executed -- the IO worker is single-threaded and shared with cover-art loads, so
            // a queue full of slow art can starve the save past the bound. In that case nothing ever
            // touched the config, so reporting a write error against the config path is a lie.
            if (gLastDeferredTimedOut)
                snprintf(notification, sizeof(notification), "%s", _l(_STR_ERR_DEVICE_BUSY_TIMEOUT));
            else {
                const char *failedPath = gLastSaveTarget[0] != '\0' ? gLastSaveTarget : configGetDir();
                snprintf(notification, sizeof(notification), _l(_STR_ERROR_SAVING_SETTINGS_TO), failedPath, gLastSaveErrno);
            }
            guiMsgBox(notification, 0, NULL);
        }
    }

    return lscret;
}

#define COMPAT_UPD_MODE_UPD_USR   1 // Update all records, even those that were modified by the user.
#define COMPAT_UPD_MODE_NO_MTIME  2 // Do not check the modified time-stamp.
#define COMPAT_UPD_MODE_MTIME_GMT 4 // Modified time-stamp is in GMT, not JST.

#define EOPLCONNERR 0x4000 // Special error code for connection errors.

static int CompatAttemptConnection(void)
{
    unsigned char retries;
    int HttpSocket;

    for (retries = OPL_COMPAT_HTTP_RETRIES, HttpSocket = -1; !CompatUpdateStopFlag && retries > 0; retries--) {
        if ((HttpSocket = HttpEstabConnection(OPL_COMPAT_HTTP_HOST, OPL_COMPAT_HTTP_PORT)) >= 0) {
            break;
        }
    }

    return HttpSocket;
}

static void compatUpdate(item_list_t *support, unsigned char mode, config_set_t *configSet, int id)
{
    sceCdCLOCK clock;
    config_set_t *itemConfig, *downloadedConfig;
    u16 length;
    s8 ConnMode, hasMtime;
    char *HttpBuffer;
    int i, count, HttpSocket, result, retries, ConfigSource;
    iox_stat_t stat;
    u8 mtime[6];
    char device, uri[64];
    const char *startup;

    switch (support->mode) {
        case BDM_MODE:
            device = 3;
            break;
        case ETH_MODE:
            mode |= COMPAT_UPD_MODE_MTIME_GMT;
            device = 2;
            break;
        case HDD_MODE:
            device = 1;
            break;
        default:
            device = -1;
    }

    if (device < 0) {
        LOG("CompatUpdate: unrecognized mode: %d\n", support->mode);
        CompatUpdateStatus = OPL_COMPAT_UPDATE_STAT_ERROR;
        return; // Shouldn't happen, but what if?
    }

    result = 0;
    LOG("CompatUpdate: updating for: device %d game %d\n", device, configSet == NULL ? -1 : id);

    if ((HttpBuffer = memalign(64, HTTP_IOBUF_SIZE)) != NULL) {
        count = configSet != NULL ? 1 : support->itemGetCount(support);

        if (count > 0) {
            ConnMode = HTTP_CMODE_PERSISTENT;
            if ((HttpSocket = CompatAttemptConnection()) >= 0) {
                // Update compatibility list.
                for (i = 0; !CompatUpdateStopFlag && result >= 0 && i < count; i++, CompatUpdateComplete++) {
                    startup = support->itemGetStartup(support, configSet != NULL ? id : i);

                    if (ConnMode == HTTP_CMODE_CLOSED) {
                        ConnMode = HTTP_CMODE_PERSISTENT;
                        if ((HttpSocket = CompatAttemptConnection()) < 0) {
                            result = HttpSocket | EOPLCONNERR;
                            break;
                        }
                    }

                    itemConfig = configSet != NULL ? configSet : support->itemGetConfig(support, i);
                    if (itemConfig != NULL) {
                        ConfigSource = CONFIG_SOURCE_DEFAULT;
                        if ((mode & COMPAT_UPD_MODE_UPD_USR) || !configGetInt(itemConfig, CONFIG_ITEM_CONFIGSOURCE, &ConfigSource) || ConfigSource != CONFIG_SOURCE_USER) {
                            if (!(mode & COMPAT_UPD_MODE_NO_MTIME) && (ConfigSource == CONFIG_SOURCE_DLOAD) && configGetStat(itemConfig, &stat)) { // Only perform a stat operation for downloaded setting files.
                                if (!(mode & COMPAT_UPD_MODE_MTIME_GMT)) {
                                    clock.second = itob(stat.mtime[1]);
                                    clock.minute = itob(stat.mtime[2]);
                                    clock.hour = itob(stat.mtime[3]);
                                    clock.day = itob(stat.mtime[4]);
                                    clock.month = itob(stat.mtime[5]);
                                    clock.year = itob((stat.mtime[6] | ((unsigned short int)stat.mtime[7] << 8)) - 2000);
                                    configConvertToGmtTime(&clock);

                                    mtime[0] = btoi(clock.year);      // Year
                                    mtime[1] = btoi(clock.month) - 1; // Month
                                    mtime[2] = btoi(clock.day) - 1;   // Day
                                    mtime[3] = btoi(clock.hour);      // Hour
                                    mtime[4] = btoi(clock.minute);    // Minute
                                    mtime[5] = btoi(clock.second);    // Second
                                } else {
                                    mtime[0] = (stat.mtime[6] | ((unsigned short int)stat.mtime[7] << 8)) - 2000; // Year
                                    mtime[1] = stat.mtime[5] - 1;                                                 // Month
                                    mtime[2] = stat.mtime[4] - 1;                                                 // Day
                                    mtime[3] = stat.mtime[3];                                                     // Hour
                                    mtime[4] = stat.mtime[2];                                                     // Minute
                                    mtime[5] = stat.mtime[1];                                                     // Second
                                }
                                hasMtime = 1;

                                LOG("CompatUpdate: LAST MTIME %04u/%02u/%02u %02u:%02u:%02u\n", (unsigned short int)mtime[0] + 2000, mtime[1] + 1, mtime[2] + 1, mtime[3], mtime[4], mtime[5]);
                            } else {
                                hasMtime = 0;
                            }

                            sprintf(uri, OPL_COMPAT_HTTP_URI, startup, device);
                            for (retries = OPL_COMPAT_HTTP_RETRIES; !CompatUpdateStopFlag && retries > 0; retries--) {
                                length = HTTP_IOBUF_SIZE;
                                result = HttpSendGetRequest(HttpSocket, OPL_USER_AGENT, OPL_COMPAT_HTTP_HOST, &ConnMode, hasMtime ? mtime : NULL, uri, HttpBuffer, &length);
                                if (result >= 0) {
                                    if (result == 200) {
                                        if ((downloadedConfig = configAlloc(0, NULL, NULL)) != NULL) {
                                            configReadBuffer(downloadedConfig, HttpBuffer, length);
                                            configMerge(itemConfig, downloadedConfig);
                                            configFree(downloadedConfig);
                                            configSetInt(itemConfig, CONFIG_ITEM_CONFIGSOURCE, CONFIG_SOURCE_DLOAD);
                                            if (!configWrite(itemConfig))
                                                result = -EIO;
                                        } else
                                            result = -ENOMEM;
                                    }

                                    break;
                                } else
                                    result |= EOPLCONNERR;

                                HttpCloseConnection(HttpSocket);

                                LOG("CompatUpdate: Connection lost. Retrying.\n");

                                // Connection lost. Attempt to re-connect.
                                ConnMode = HTTP_CMODE_PERSISTENT;
                                if ((HttpSocket = CompatAttemptConnection()) < 0) {
                                    result = HttpSocket | EOPLCONNERR;
                                    break;
                                }
                            }

                            LOG("CompatUpdate %d. %d, %s: %s %d\n", i + 1, device, startup, ConnMode == HTTP_CMODE_CLOSED ? "CLOSED" : "PERSISTENT", result);
                        } else {
                            LOG("CompatUpdate: skipping %s\n", startup);
                        }

                        if (configSet == NULL) // Do not free what is not ours.
                            configFree(itemConfig);
                    } else {
                        // Can't do anything because the config file cannot be opened/created.
                        LOG("CompatUpdate: skipping %s (no config)\n", startup);
                    }

                    if (ConnMode == HTTP_CMODE_CLOSED)
                        HttpCloseConnection(HttpSocket);
                }

                if (ConnMode == HTTP_CMODE_PERSISTENT)
                    HttpCloseConnection(HttpSocket);
            } else {
                result = HttpSocket | EOPLCONNERR;
            }
        }

        free(HttpBuffer);
    } else {
        result = -ENOMEM;
    }

    if (CompatUpdateStopFlag)
        CompatUpdateStatus = OPL_COMPAT_UPDATE_STAT_ABORTED;
    else {
        if (result >= 0)
            CompatUpdateStatus = OPL_COMPAT_UPDATE_STAT_DONE;
        else {
            CompatUpdateStatus = (result & EOPLCONNERR) ? OPL_COMPAT_UPDATE_STAT_CONN_ERROR : OPL_COMPAT_UPDATE_STAT_ERROR;
        }
    }
    LOG("CompatUpdate: completed with status %d\n", CompatUpdateStatus);
}

static void compatDeferredUpdate(void *data)
{
    opl_io_module_t *mod = &list_support[*(short int *)data];

    compatUpdate(mod->support, CompatUpdateFlags, NULL, -1);
}

int oplGetUpdateGameCompatStatus(unsigned int *done, unsigned int *total)
{
    *done = CompatUpdateComplete;
    *total = CompatUpdateTotal;
    return CompatUpdateStatus;
}

void oplAbortUpdateGameCompat(void)
{
    CompatUpdateStopFlag = 1;
    ioRemoveRequests(IO_COMPAT_UPDATE_DEFFERED);
}

void oplUpdateGameCompat(int UpdateAll)
{
    int i, started, count;

    CompatUpdateTotal = 0;
    CompatUpdateComplete = 0;
    CompatUpdateStopFlag = 0;
    CompatUpdateFlags = UpdateAll ? (COMPAT_UPD_MODE_NO_MTIME | COMPAT_UPD_MODE_UPD_USR) : 0;
    CompatUpdateStatus = OPL_COMPAT_UPDATE_STAT_WIP;

    // Schedule compatibility updates of all the list handlers
    for (i = 0, started = 0; i < MODE_COUNT; i++) {
        if (list_support[i].support && list_support[i].support->enabled && !(list_support[i].support->flags & MODE_FLAG_NO_UPDATE) && (count = list_support[i].support->itemGetCount(list_support[i].support)) > 0) {
            CompatUpdateTotal += count;
            ioPutRequest(IO_COMPAT_UPDATE_DEFFERED, &list_support[i].support->mode);
            started++;

            LOG("CompatUpdate: started for mode %d (%d games)\n", list_support[i].support->mode, count);
        }
    }

    if (started < 1) // Nothing done
        CompatUpdateStatus = OPL_COMPAT_UPDATE_STAT_DONE;
}

static int CompatUpdSingleID, CompatUpdSingleStatus;
static item_list_t *CompatUpdSingleSupport;
static config_set_t *CompatUpdSingleConfigSet;

static void _updateCompatSingle(void)
{
    compatUpdate(CompatUpdSingleSupport, COMPAT_UPD_MODE_UPD_USR, CompatUpdSingleConfigSet, CompatUpdSingleID);
    CompatUpdSingleStatus = 0;
}

int oplUpdateGameCompatSingle(int id, item_list_t *support, config_set_t *configSet)
{
    CompatUpdateStopFlag = 0;
    CompatUpdateStatus = OPL_COMPAT_UPDATE_STAT_WIP;
    CompatUpdateTotal = 1;
    CompatUpdateComplete = 0;
    CompatUpdSingleID = id;
    CompatUpdSingleSupport = support;
    CompatUpdSingleConfigSet = configSet;
    CompatUpdSingleStatus = 1;

    guiHandleDeferedIO(&CompatUpdSingleStatus, _l(_STR_PLEASE_WAIT), IO_CUSTOM_SIMPLEACTION, &_updateCompatSingle, 0); // network fetch: wait unbounded

    return CompatUpdateStatus;
}

// ----------------------------------------------------------
// -------------------- NBD SRV Support ---------------------
// ----------------------------------------------------------


#define LWNBD_ART_DRAIN_TICKS 500
#define LWNBD_IO_DRAIN_TICKS  1000

static void shutdownLwnbdNetwork(void)
{
    // The loaded-check mirrors ethShutdown: DEV9's refcount is shared with BDM/HDD, so only
    // release it when the eth path actually holds it.
    int ethWasLoaded = ethGetModulesLoaded();

    ethDeinitModules();
    if (ethWasLoaded)
        sysShutdownDev9();
}

static int loadLwnbdSvr(int *teardownStarted)
{
    int ret, padStatus, padTries;
    int ethWasLoadedBeforePreflight;
    struct lwnbd_config
    {
        char defaultexport[32];
        uint8_t readonly;
    };
    struct lwnbd_config config;

    *teardownStarted = 0;

    /* compat stuff for user not providing name export (useless when there was only one export) */
    ret = strlen(gExportName);
    if (ret == 0)
        strcpy(config.defaultexport, "hdd0");
    else
        strcpy(config.defaultexport, gExportName);

    config.readonly = !gEnableWrite;

    /*
     * Prove that the NIC can establish link and configuration while the live menu
     * is still intact. In particular, an unplugged cable must not send the failure
     * path through audioEnd(), unloadPads(), or the IOP reset used to restore a
     * successfully-started server -- that is the reported freeze at the
     * "NBD Server unloading..." screen (#307).
     */
    ethWasLoadedBeforePreflight = ethGetModulesLoaded();
    ret = ethLoadInitModules();
    if (ret != 0) {
        if (!ethWasLoadedBeforePreflight)
            shutdownLwnbdNetwork();
        return ret;
    }

    /*
     * If preflight brought up the stack solely for this check, return DEV9 to its
     * prior ownership state. The normal post-teardown load below then starts from
     * the same clean state as every other NBD launch.
     */
    if (!ethWasLoadedBeforePreflight)
        shutdownLwnbdNetwork();

    /*
     * Support cleanup invalidates device-owned lists used by art requests.
     * If the bounded drain cannot make that ownership safe, keep the live menu
     * intact and let the user retry.
     */
    if (!cacheAbortMmceImageLoadsTimed(LWNBD_ART_DRAIN_TICKS) ||
        !cacheCancelPendingImageLoadsTimed(LWNBD_ART_DRAIN_TICKS))
        return -1;

    /*
     * Block new foreground work and bound the drain while the rest of the menu
     * is still live. A stuck request therefore fails the start instead of
     * freezing after audio and pads have already been dismantled.
     */
    ioBlockOpsTimed(1, LWNBD_IO_DRAIN_TICKS);
    if (ioHasPendingRequests()) {
        ioBlockOps(0);
        return -1;
    }

    *teardownStarted = 1;

    // deinit audio lib while nbd server is running
    audioEnd();

    guiExecDeferredOps();

    // Deinitialize all support without shutting down the HDD unit.
    // NOTE(rebuild): the fork passes a 3rd "spare a second mode" argument here; this tree's
    // deinitAllSupport is still the 2-arg form, so the call is adapted rather than copied.
    deinitAllSupport(NO_EXCEPTION, IO_MODE_SELECTED_ALL);
    clearErrorMessage(); /* At this point, an error might have been displayed (since background tasks were completed).
                            Clear it, otherwise it will get displayed after the server is closed. */

    unloadPads();

    ret = ethLoadInitModules();
    if (ret == 0) {
        ret = sysLoadModuleBuffer(&ps2atad_irx, size_ps2atad_irx, 0, NULL); /* gHDDStartMode ? */
        if (ret >= 0) {
            ret = sysLoadModuleBuffer(&lwnbdsvr_irx, size_lwnbdsvr_irx, sizeof(config), (char *)&config);
            if (ret >= 0)
                ret = 0;
        }
    }

    if (ret != 0)
        shutdownLwnbdNetwork();

    padInit(0);

    // init all pads, but never wedge the status screen on a failed reconnect
    padStatus = 0;
    padTries = 0;
    while (!padStatus && padTries++ < 100) {
        padStatus = startPads();
        if (!padStatus)
            delay(50);
    }
    if (!padStatus)
        LOG("loadLwnbdSvr: pads did not start after teardown (%d tries)\n", padTries);

    // now ready to display some status

    return ret;
}


static void unloadLwnbdSvr(void)
{
    // Release the live server stack before resetting the IOP (#307): reset() -> sysReset() parks
    // the EE in unbounded SifIopReset/SifIopSync waits, which wedge against a live, DEV9-powered
    // stack. This function is now only reached when loadLwnbdSvr actually dismantled the menu
    // (teardownStarted), so a failed bring-up never gets here at all.
    shutdownLwnbdNetwork();
    unloadPads();

    reset();

    LOG_INIT();
    LOG_ENABLE();

    // reinit the input pads
    padInit(0);

    /* Bounded retry (#307): an unbounded startPads loop turns a half-completed IOP reset into a
     * hard freeze at exactly the reported "NBD Server unloading..." screen. ~5 s of retries covers
     * a slow IOP; if pads still won't start, degrade (log + continue, the menu's own pad handling
     * recovers on later polls) instead of wedging forever. */
    int ret = 0, tries = 0;
    while (!ret && tries++ < 100) {
        ret = startPads();
        if (!ret)
            delay(50);
    }
    if (!ret)
        LOG("unloadLwnbdSvr: pads did not start after IOP reset (%d tries)\n", tries);

    // now start io again
    ioBlockOps(0);

    // init all supports again
    initAllSupport(1);

    audioInit();
    sfxInit(0);
    if (gEnableBGM)
        bgmStart();
}

void handleLwnbdSrv()
{
    char temp[256];
    int teardownStarted;

    // prepare for lwnbd, display screen with info
    guiRenderTextScreen(_l(_STR_STARTINGNBD));
    if (loadLwnbdSvr(&teardownStarted) == 0) {
        snprintf(temp, sizeof(temp), "%s", _l(_STR_RUNNINGNBD));
        guiMsgBox(temp, 0, NULL);
    } else
        guiMsgBox(_l(_STR_STARTFAILNBD), 0, NULL);

    // Only a path that actually dismantled the menu needs the reset-and-restore cycle. A failed
    // preflight (no cable/link) leaves the menu live, so it must NOT reach the IOP reset -- that
    // reset is the reported freeze at the "NBD Server unloading..." screen (#307).
    if (teardownStarted) {
        guiRenderTextScreen(_l(_STR_UNLOADNBD));
        unloadLwnbdSvr();
    }
}

// ----------------------------------------------------------
// --------------------- Init/Deinit ------------------------
// ----------------------------------------------------------
static void reset(void)
{
    sysReset(SYS_LOAD_MC_MODULES | SYS_LOAD_USB_MODULES | SYS_LOAD_ISOFS_MODULE);

    mcInit(MC_TYPE_XMC);
}

static void moduleCleanup(opl_io_module_t *mod, int exception, int modeSelected)
{
    if (!mod->support)
        return;

    // Shutdown if not required anymore.
    if ((mod->support->mode != modeSelected) && (modeSelected != IO_MODE_SELECTED_ALL)) {
        if (mod->support->itemShutdown)
            mod->support->itemShutdown(mod->support);
    } else {
        if (mod->support->itemCleanUp)
            mod->support->itemCleanUp(mod->support, exception);
    }

    clearMenuGameList(mod);
}

// 1 while deinit() tears down for exit/poweroff, 0 for a game/app launch. Consumed by device shutdowns
// that must behave differently on the launch path (hddShutdown keeps DEV9 powered so the post-deinit
// POPSTARTER.ELF read from the ATA-backed massN: mount still works; ee_core/POPSTARTER reset the IOP
// themselves, so skipping the power-off on launches leaks nothing).
int gDeinitTerminal = 0;

// Set by deinit/deinitEx when cacheEnd() gave up waiting for the art worker, i.e. a thread is still
// inside a device read while the teardown proceeds. hddCleanUp reads it and skips the two operations
// that would pull the ground out from under that thread -- unmounting pfs and PDIOC_CLOSEALL, which
// closes EVERY pfs descriptor including the one the BGM decoder is streaming from. Leaving them is
// free: every caller of deinit hands off to an ELF that resets the IOP anyway.
int gArtAbandoned = 0;

// deinitEx: deinit for the Neutrino keep-IOP handoff -- spares TWO modes' mounts (the game
// device AND the device holding neutrino.elf), since Neutrino reads its -cwd config/modules
// and the ISO through OPL's live mounts before performing its own IOP reset.
// Bound for the teardown drains below. ioBlockOps() waits UNBOUNDED, which is correct only if the
// queue is guaranteed to finish -- and it is not: cover art is queued one request per visible row on
// the same single-threaded FIFO, so on a slow device (a network share especially) "Exit to Browser"
// or a game launch sat waiting for hundreds of art reads with nothing on screen to explain it. The
// NBD preflight already passes a bound for exactly this reason; the exit and launch paths never got
// one. Abandoning a pending art read here is safe: every one of these callers is on its way to an
// IOP reset, and the art request only ever writes into its own cache entry.
// Two budgets, keyed off gDeinitTerminal (fork parity). EXIT can afford to abandon quickly -- the
// console is going away. A LAUNCH gets ~10 s: a healthy slow device drains well under that, but a
// wedged art read must not freeze the loading screen forever, and a straggler request cannot
// survive the handoff target's own IOP reset anyway (ee_core / Neutrino / POPSTARTER all reset it).
//
// ⚠ THE UNITS ARE NOT MILLISECONDS. ioBlockOpsTimed spends one delay(1) per tick, and delay() is a
// NOP spin of 0x01000000 iterations x 4 nops (util.c) -- about a QUARTER SECOND on a 294 MHz EE, not
// a millisecond. The old values were written as if they were ms, so the "~10 s" launch budget above
// was really on the order of forty MINUTES and the "1 s" exit budget several minutes. Neither was a
// bound in any sense that mattered; a launch pressed with covers queued simply waited for all of
// them. Values below are the ORIGINAL INTENT expressed in real ticks. If delay() is ever given a
// real time base, convert these with it -- do not just scale the numbers again.
#define EXIT_IO_DRAIN_TICKS   4  // ~1 s
#define LAUNCH_IO_DRAIN_TICKS 40 // ~10 s, as the paragraph above always meant

void deinitEx(int exception, int modeSelected, int modeSelected2)
{
    gDeinitTerminal = (modeSelected == IO_MODE_SELECTED_ALL || modeSelected == IO_MODE_SELECTED_NONE);

    // Give up on the covers still QUEUED before draining. The drain waits on the ioman LIST, and the
    // worker keeps servicing it regardless of isIOBlocked, so without this the handoff pays for every
    // queued cover to be read off the game device first -- for a menu guiEnd() is about to destroy.
    cacheShutdownArtLoads();

    // JOIN the art worker before anything unmounts a device under it. cacheShutdownArtLoads
    // above only drops the QUEUE and flags the in-flight request; the thread itself may still
    // be parked inside a device read, and deinitAllSupport below is about to unmount that
    // device. Bounded, and a thread that will not come back is ABANDONED rather than
    // terminated -- killing one inside fileXio leaves the shared IOP RPC channel half-used for
    // every later caller, from any thread.
    // ORDER: block+drain the io worker FIRST, then join art. Art left the ioman queue in rebuild-152,
    // so nothing here needs art gone before the drain -- and doing it this way stops the two workers
    // competing for the device during the join, which is the whole point of the join.
    //
    // Stop the music BEFORE anything tears down the device it is streaming from -- bgm.ogg normally
    // lives on the same stick, HDD or share as the games. Issue #382 is that race on the network.
    // Signal only: the thread JOIN stays in audioEnd() where it has always been. rebuild-163 moved
    // the whole blocking bgmStop() here and froze exit from a UDPFS boot; the order was right, the
    // wait was not.
    bgmQuiesce();

    // block all io ops, wait for the ones still running to finish (BOUNDED -- see the note above)
    ioBlockOpsTimed(1, gDeinitTerminal ? EXIT_IO_DRAIN_TICKS : LAUNCH_IO_DRAIN_TICKS);

    // JOIN the art worker before anything unmounts a device under it. Its RESULT IS LOAD-BEARING and
    // used to be thrown away: a 0 means a thread is still inside a device read, and the teardown
    // below is about to unmount that device and close its descriptors.
    gArtAbandoned = !cacheEnd(gDeinitTerminal);
    guiExecDeferredOps();

#ifdef PADEMU
    ds34usb_reset();
    ds34bt_reset();
#endif
    unloadPads();

    for (int i = 0; i < MODE_COUNT; i++) {
        if (list_support[i].support != NULL) {
            int spared = (modeSelected2 >= 0 && list_support[i].support->mode == modeSelected2);
            moduleCleanup(&list_support[i], exception, spared ? modeSelected2 : modeSelected);
        }
    }

    audioEnd();
    ioEnd();
    guiEnd();
    menuEnd();
    lngEnd();
    thmEnd();
    rmEnd();
    configEnd();
}

void deinit(int exception, int modeSelected)
{
    gDeinitTerminal = (modeSelected == IO_MODE_SELECTED_ALL || modeSelected == IO_MODE_SELECTED_NONE);

    // Give up on the covers still QUEUED before draining. The drain waits on the ioman LIST, and the
    // worker keeps servicing it regardless of isIOBlocked, so without this the handoff pays for every
    // queued cover to be read off the game device first -- for a menu guiEnd() is about to destroy.
    cacheShutdownArtLoads();

    // JOIN the art worker before anything unmounts a device under it. cacheShutdownArtLoads
    // above only drops the QUEUE and flags the in-flight request; the thread itself may still
    // be parked inside a device read, and deinitAllSupport below is about to unmount that
    // device. Bounded, and a thread that will not come back is ABANDONED rather than
    // terminated -- killing one inside fileXio leaves the shared IOP RPC channel half-used for
    // every later caller, from any thread.
    // ORDER: block+drain the io worker FIRST, then join art. Art left the ioman queue in rebuild-152,
    // so nothing here needs art gone before the drain -- and doing it this way stops the two workers
    // competing for the device during the join, which is the whole point of the join.
    //
    // Stop the music BEFORE anything tears down the device it is streaming from -- bgm.ogg normally
    // lives on the same stick, HDD or share as the games. Issue #382 is that race on the network.
    // Signal only: the thread JOIN stays in audioEnd() where it has always been. rebuild-163 moved
    // the whole blocking bgmStop() here and froze exit from a UDPFS boot; the order was right, the
    // wait was not.
    bgmQuiesce();

    // block all io ops, wait for the ones still running to finish (BOUNDED -- see the note above)
    ioBlockOpsTimed(1, gDeinitTerminal ? EXIT_IO_DRAIN_TICKS : LAUNCH_IO_DRAIN_TICKS);

    // JOIN the art worker before anything unmounts a device under it. Its RESULT IS LOAD-BEARING and
    // used to be thrown away: a 0 means a thread is still inside a device read, and the teardown
    // below is about to unmount that device and close its descriptors.
    gArtAbandoned = !cacheEnd(gDeinitTerminal);
    guiExecDeferredOps();

#ifdef PADEMU
    ds34usb_reset();
    ds34bt_reset();
#endif
    unloadPads();

    deinitAllSupport(exception, modeSelected);

    audioEnd();
    ioEnd();
    guiEnd();
    menuEnd();
    lngEnd();
    thmEnd();
    rmEnd();
    configEnd();
}

void setDefaultColors(void)
{
    gDefaultBgColor[0] = 0x28;
    gDefaultBgColor[1] = 0xC5;
    gDefaultBgColor[2] = 0xF9;

    gDefaultTextColor[0] = 0xFF;
    gDefaultTextColor[1] = 0xFF;
    gDefaultTextColor[2] = 0xFF;

    gDefaultSelTextColor[0] = 0x00;
    gDefaultSelTextColor[1] = 0xAE;
    gDefaultSelTextColor[2] = 0xFF;

    gDefaultUITextColor[0] = 0x58;
    gDefaultUITextColor[1] = 0x68;
    gDefaultUITextColor[2] = 0xB4;
}

static void setDefaults(void)
{
    for (int i = 0; i < MODE_COUNT; i++)
        clearIOModuleT(&list_support[i]);

    gAutoLaunchGame = NULL;
    gAutoLaunchBDMGame = NULL;
    gAutoLaunchDeviceData = NULL;
    gOPLPart[0] = '\0';
    // NULL is the only truthful pre-mount state. hddLoadSupportModules() uses non-NULL as the
    // proof that the persistent pfs0: data home is already mounted; seeding this with "pfs0:"
    // skipped discovery/mount entirely on a fresh APA boot.
    gHDDPrefix = NULL;
    gBaseMCDir = "mc?:OPL";

    bdmCacheSize = 16;
    hddCacheSize = 8;
    smbCacheSize = 16;

    // Network defaults are ONE coherent set, not four independent knobs -- restoring the fork's
    // opinionated fresh-install profile (this had reverted to the upstream 192.168.0.x / DHCP one).
    // Static addressing on 192.168.1.x, the far more common home subnet, with the PS2 and the PC on
    // the same wire: the UDP transports (UDPBD / UDPFS / NBD) run on a ministack that needs a STATIC
    // ip, so defaulting to DHCP hands a fresh install a config those transports cannot use.
    // gPCShareAddressIsNetBIOS follows the same logic -- raw-IP SMB addressing is what matches the
    // static defaults directly below; NetBIOS name resolution is the opt-in.
    ps2_ip_use_dhcp = 0;
    gETHOpMode = ETH_OP_MODE_AUTO;
    gPCShareAddressIsNetBIOS = 0; // raw-IP SMB addressing by default (matches the static defaults below)
    gPCShareNBAddress[0] = '\0';
    ps2_ip[0] = 192;
    ps2_ip[1] = 168;
    ps2_ip[2] = 1;
    ps2_ip[3] = 10;
    ps2_netmask[0] = 255;
    ps2_netmask[1] = 255;
    ps2_netmask[2] = 255;
    ps2_netmask[3] = 0;
    ps2_gateway[0] = 192;
    ps2_gateway[1] = 168;
    ps2_gateway[2] = 1;
    ps2_gateway[3] = 1;
    pc_ip[0] = 192;
    pc_ip[1] = 168;
    pc_ip[2] = 1;
    pc_ip[3] = 100;
    ps2_dns[0] = 192;
    ps2_dns[1] = 168;
    ps2_dns[2] = 1;
    ps2_dns[3] = 1;
    gPCPort = 1111; // RiptOPL default SMB port (was 445): matches the bundled PC server tooling
    // Fork's opinionated first-boot defaults: the overwhelmingly common SMB setup is a share named
    // "games" with guest access, so a fresh install can connect after typing only the host IP.
    // A loaded config still overwrites both.
    strcpy(gPCShareName, "games");
    strcpy(gPCUserName, "guest");
    gPCPassword[0] = '\0';
    gNetworkStartup = ERROR_ETH_NOT_STARTED;
    gHDDSpindown = 20;
    gScrollSpeed = 1;
    gExitPath[0] = '\0';
    gCustomSettingsPath[0] = '\0'; // opt-in: empty means "use the normal boot-dir/discovery home"
    gDefaultDevice = APP_MODE;
    gAutosort = 1;
    gAutoRefresh = 0;
    gEnableDebug = 0;
    gPS2Logo = 1; // opinionated defaults: the fork ships ready-to-use
    gHDDGameListCache = 0;
    gEnableWrite = 1;
    gRememberLastPlayed = 0;
    gAutoStartLastPlayed = 9;
    gSelectButton = KEY_CIRCLE; // Default to Japan.
    gMMCEPrefix[0] = '\0';
    gBDMPrefix[0] = '\0';
    gETHPrefix[0] = '\0';
    gEnableNotifications = 1;
    gEnableArt = 1;
    gWideScreen = 1;
    gEnableSFX = 1; // safe now: sfxPlay dispatches asynchronously (#340)
    gEnableRumble = 1;
    gEnableBootSND = 1;
    gEnableBGM = 1; // inert without a bgm.ogg on the card
    gSFXVolume = 90;
    gBootSndVolume = 90;
    gBGMVolume = 90;
    gDefaultBGMPath[0] = '\0';
    gXSensitivity = 1;
    gYSensitivity = 1;

    gBDMStartMode = START_MODE_DISABLED;
    gHDDStartMode = START_MODE_DISABLED;
    gETHStartMode = START_MODE_DISABLED;
    gAPPStartMode = START_MODE_DISABLED;
    gMMCEStartMode = START_MODE_DISABLED;
    gFAVStartMode = START_MODE_DISABLED;

    gMMCESlot = 2; // Default to first Auto slot
    gMMCEIGRSlot = 3;
    gMMCEEnableGameID = 0;
    gMMCEAckWaitCycles = 5;
    gMMCEUseAlarms = 1;

    gEnableUSB = 0; // USB block device is opt-in, like the other BDM transports
    // Visual GameID barcode ships OFF, matching fork commit cc2cdfed ("GameID defaults OFF"): the
    // HDMI auto-profile latch is only verifiable on real GameID hardware, so it stays opt-in.
    gApplyGameID = 0;
    gNeutrinoArgs[0] = '\0';
    gNeutrinoPath[0] = '\0';
    gNeutrinoDevice = NEUTRINO_DEV_AUTO;
    gDefaultCoreLoader = 0;    // <OPL> (native) -- preserves behaviour until the user opts into Neutrino globally
    gNeutrinoVideoDefault = 0; // no global -gsm until the user opts in
    gNeutrinoGsmCompDefault = 0;
    gNeutrinoElfArg = 1; // auto-emit the game ELF for Neutrino compatibility lookup by default
    gDefaultGameView = GAME_VIEW_BOTH;
    gPopstarterDevice = POPS_DEV_DEFAULT;
    gPopstarterPath[0] = '\0';
    gPopstarterRetroGemGameID = 1;
    gBdmaSource = VCD_BDMA_SRC_USB;
    gBdmaMode = VCD_BDMA_FAT32;
    gBdmaApplyOnLaunch = 1;             // auto-equip on launch by default
    gVcdUsbBdmaMode = VCD_USB_BDMA_ASK; // preserve the per-launch prompt as the shipped behaviour
    gVcdHideGameId = 1;                 // hide the PS1 game-ID prefix by default (display-only)
    gVcdFirstDiscOnly = 1;              // hide discs 2+ of multi-disc PS1 sets by default (POPSLoader parity)
    gVcdShowPpPops = 1;                 // list strict PP.<ID>.POPS.<name> one-game HDD partitions by default
    // gBootDir is deliberately NOT reset here. main() resolves it (setBootDir, from argv[0] with a
    // getcwd fallback) BEFORE calling init(), and init() calls setDefaults() -- so clearing it here
    // erased the boot identity a few lines before configInit() needs it, on EVERY boot. That made
    // resolveBootDirToMass() early-return at its `gBootDir[0] == '\0'` guard every time, and homed
    // every config set on the mc?:OPL default regardless of what OPL actually booted from.
    // setBootDir() already zeroes the buffer at its own entry, so nothing needs a reset here.
    gEnableBGArt = 1; // fork parity; gEnableArt is 1 above, so this is live
    gEnableArtTar = 0;
    // NO SETTLE BY DEFAULT. This is the number of INACTIVE frames the menu must see before art is
    // even asked for, and it shipped at 8 -- the slowest of the four values the UI offers {0,2,5,8}
    // -- as a placeholder pending a decision that never happened; the comment here previously said
    // as much. The effect is that art will not begin loading until navigation has fully stopped and
    // stayed stopped, which reads as covers refusing to appear while browsing.
    //
    // The reason to settle at all was to keep art reads off the bus while the user is moving, but
    // that concern is device-specific and is already handled where it belongs: texcache gates SIO2
    // devices (MMCE, MX4SIO) on real idleness because their reads contend with pad polling. Making
    // every device wait for the worst device's constraint is not a trade worth a default.
    gArtDelay = 0;
    gEnableFolderNav = 0;
    gDefaultPlasBlendColor[0] = 0x00;
    gDefaultPlasBlendColor[1] = 0x00;
    gDefaultPlasBlendColor[2] = 0x00;
    gEnableILK = 0;
    gEnableMX4SIO = 0;
    gEnableBdmHDD = 0;
    gEnableUDPBD = 0;                  // the UDPBD BLOCK device stays opt-in
    gNetBootProtocol = NET_BOOT_UDPBD; // default transport when network boot is enabled (back-compat)
    // Unified network selector defaults to OFF. The reason is the NIC latch: every network stack
    // loads its IOP chain ONCE per boot and never unloads (re-binding the UDPRDMA socket bricks
    // UDPFS; smap registers a single SMAP_driver), so whichever protocol is active FIRST owns the
    // adapter until a restart -- the settings page even tells you so (NETBOOT_RESTART). Defaulting
    // to Off means nothing claims the NIC at boot, so the first protocol the user picks in Device
    // Settings comes up live -- the apply path re-derives the gEnableUDPBD/gNetBootProtocol shadows
    // and forces a device refresh already. A saved net protocol in the config overrides this.
    gNetworkProtocol = NET_PROTO_OFF;
    gNetStartMode = START_MODE_DISABLED; // Off in the 3-row Network setting; migration reconciles old configs

    frameCounter = 0;

    gVMode = 0;
    gXOff = 0;
    gYOff = 0;
    gOverscan = 0;

    setDefaultColors();

    // Last Played Auto Start
    KeyPressedOnce = 0;
    DisableCron = 1; // Auto Start Last Played counter disabled by default
    CronStart = 0;
    RemainSecs = 0;
}

static void init(void)
{
    // default variable values
    setDefaults();

    padInit(0);
    int padStatus = 0;
    configInit(gBootDir[0] ? gBootDir : NULL); // settings live in the boot dir (cwd), not a fixed MC default

    rmInit();
    lngInit();
    thmInit();
    guiInit();
    ioInit();
    menuInit();

    startPads();

    bdmInitSemaphore();

    // compatibility update handler
    ioRegisterHandler(IO_COMPAT_UPDATE_DEFFERED, &compatDeferredUpdate);

    // handler for deffered menu updates
    ioRegisterHandler(IO_MENU_UPDATE_DEFFERED, &menuDeferredUpdate);
    cacheInit();

    gSelectButton = (InitConsoleRegionData() == CONSOLE_REGION_JAPAN) ? KEY_CIRCLE : KEY_CROSS;

    while (!padStatus)
        padStatus = startPads();
    readPads();
    gBootInProgress = 1; // gate the in-initAllSupport greeting redraws to the boot pass only (#297)
    if (!getKeyPressed(KEY_START)) {
        // Show the boot splash (not guiRenderTextScreen(), which calls guiShow()
        // and would draw the not-yet-ready main menu as a garbled landing page
        // before the intro splash) while the config loads.
        guiSetBootStatus(_l(_STR_BOOT_LOADING_CONFIG));
        guiRenderGreetingScreen();
        _loadConfig(); // only try to restore config if emergency key is not being pressed
    } else {
        LOG("--- SKIPPING OPL CONFIG LOADING\n");
        applyConfig(-1, -1, 0);
    }
    guiSetBootStatus(_l(_STR_BOOT_READY));
    guiRenderGreetingScreen();
    gBootInProgress = 0;


    // queue deffered init of sound effects, which will take place after the preceding initialization steps within the queue are complete.
    ioPutRequest(IO_CUSTOM_SIMPLEACTION, &deferredAudioInit);
}

static void deferredInit(void)
{
    guiSetBootStatusSticky(_l(_STR_BOOT_BUILDING_MENU)); // boot-step localizer (IO thread) -- reaching
                                                         // here means the device init chain cleared; see gui.c

    // inform GUI main init part is over
    struct gui_update_t *id = guiOpCreate(GUI_INIT_DONE);
    if (id)
        guiDeferUpdate(id);

    if (gMMCEEnableGameID)
        ioPutRequest(IO_CUSTOM_SIMPLEACTION, &mmceArmGameIDTransport);

    // Boot onto the configured Default Menu -- but if that mode has no registered support (its
    // device never initialised, or the config named a mode this build defers), fall through to a
    // registered one, mirroring applyConfig's clamp. Only when NOTHING is registered is there no
    // main screen worth selecting and the start menu stays -- previously ANY unregistered pick
    // silently parked every boot on the start menu with no explanation.
    int bootMode = gDefaultDevice;
    if (list_support[bootMode].support == NULL) {
        if (list_support[MMCE_MODE].support != NULL)
            bootMode = MMCE_MODE;
        else if (list_support[APP_MODE].support != NULL)
            bootMode = APP_MODE;
        else {
            bootMode = -1;
            for (int i = 0; i < MODE_COUNT; i++) {
                if (list_support[i].support != NULL) {
                    bootMode = i;
                    break;
                }
            }
        }
    }
    if (bootMode >= 0 && list_support[bootMode].support) {
        id = guiOpCreate(GUI_OP_SELECT_MENU);
        if (id) {
            id->menu.menu = &list_support[bootMode].menuItem;
            guiDeferUpdate(id);
        }
    }
}

static void deferredAudioInit(void)
{
    int ret;

    guiSetBootStatusSticky(_l(_STR_BOOT_LOADING_SOUNDS)); // boot-step localizer (IO thread) -- see gui.c
    audioInit();
    ret = sfxInit(1);
    if (ret < 0)
        LOG("sfxInit: failed to initialize - %d.\n", ret);
    else
        LOG("sfxInit: %d samples loaded.\n", ret);
}

// ----------------------------------------------------------
// --------------------- Auto Loading -----------------------
// ----------------------------------------------------------

static void miniInit(int mode)
{
    int ret;

    setDefaults();
    configInit(gBootDir[0] ? gBootDir : NULL); // settings live in the boot dir (cwd)

    ioInit();
    LOG_ENABLE();

    if (mode == BDM_MODE) {
        bdmInitSemaphore();

        // Force load all BDM modules.. we aren't using the gui so this is fine.
        // gEnableUSB belongs in this list and was the one missing from it. Unlike the others it is a
        // FORK INVENTION -- upstream loads USBMASS_BD unconditionally and has no such flag -- so the
        // opt-in default of 0 that setDefaults() applies a few lines earlier (and which the GUI
        // normally overrides from the saved config) had nothing to override it here: configReadMulti
        // runs AFTER bdmLoadModules(). bdmLoadBlockDeviceModules gates the USB load on the flag, so
        // an argv autolaunch from a USB stick loaded no USB block driver at all.
        gEnableUSB = 1;
        gEnableILK = 1; // iLink will break pcsx2 however.
        gEnableMX4SIO = 1;
        gEnableBdmHDD = 1;
        bdmLoadModules();

        // Autolaunch reads its per-game config from the boot dir too -- resolve a launch-identity or
        // not-yet-mounted massN: boot dir before the configReadMulti below, same as the full boot path.
        resolveBootDirToMass();

    } else if (mode == HDD_MODE) {
        hddLoadModules();
        hddLoadSupportModules();
    } else if (mode == MMCE_MODE) {
        mmceLoadModules();
    }

    InitConsoleRegionData();

    ret = configReadMulti(CONFIG_ALL);
    if (CONFIG_ALL & CONFIG_OPL) {
        if (!(ret & CONFIG_OPL)) {
            if (mode == BDM_MODE)
                ret = checkLoadConfigBDM(CONFIG_ALL);
            else if (mode == HDD_MODE)
                ret = checkLoadConfigHDD(CONFIG_ALL);
            else if (mode == MMCE_MODE)
                ret = checkLoadConfigMMCE(CONFIG_ALL);
        }

        if (ret & CONFIG_OPL) {
            config_set_t *configOPL = configGetByType(CONFIG_OPL);

            configGetInt(configOPL, CONFIG_OPL_PS2LOGO, &gPS2Logo);
            configGetStrCopy(configOPL, CONFIG_OPL_EXIT_PATH, gExitPath, sizeof(gExitPath));
            configGetInt(configOPL, CONFIG_OPL_HDD_SPINDOWN, &gHDDSpindown);
            // Honor ALL the Neutrino-launch globals on the autolaunch/argv path exactly like the
            // interactive _loadConfig -- not just the default core. An autolaunched keyless "Default"
            // game must resolve the SAME neutrino.elf (device pick / custom path) with the SAME global
            // args as an interactive launch, or it silently boots a different/stale core without flags.
            configReadNeutrinoGlobals(configOPL);
            if (mode == BDM_MODE) {
                configGetStrCopy(configOPL, CONFIG_OPL_BDM_PREFIX, gBDMPrefix, sizeof(gBDMPrefix));
                configGetInt(configOPL, CONFIG_OPL_BDM_CACHE, &bdmCacheSize);
            } else if (mode == HDD_MODE)
                configGetInt(configOPL, CONFIG_OPL_HDD_CACHE, &hddCacheSize);
        }
    }
}

void miniDeinit(config_set_t *configSet)
{
    // Autolaunch teardown is always a LAUNCH: the game is about to take over.
    ioBlockOpsTimed(1, LAUNCH_IO_DRAIN_TICKS);
#ifdef PADEMU
    ds34usb_reset();
    ds34bt_reset();
#endif
    configFree(configSet);

    ioEnd();
    configEnd();
}

static void autoLaunchHDDGame(char *argv[])
{
    char path[256];
    char launchPart[sizeof(gOPLPart)];
    config_set_t *configSet;

    miniInit(HDD_MODE);

    gAutoLaunchGame = malloc(sizeof(hdl_game_info_t));
    if (gAutoLaunchGame == NULL) {
        miniDeinit(NULL);
        return;
    }
    memset(gAutoLaunchGame, 0, sizeof(hdl_game_info_t));

    // "%s", argv[1] -- NOT argv[1] as the format string. These come from another program's argv
    // (a launcher/shortcut), so a name containing a % sequence made snprintf walk a nonexistent
    // varargs list. Same below for argv[2] in the BDM twin.
    snprintf(gAutoLaunchGame->startup, sizeof(gAutoLaunchGame->startup), "%s", argv[1]);
    gAutoLaunchGame->start_sector = strtoul(argv[2], NULL, 0);
    snprintf(launchPart, sizeof(launchPart), "hdd0:%s", argv[3]);
    // argv[3] is a compatibility hint from the launching OPL instance. Re-read the authoritative
    // __common/OPL/conf_hdd.cfg policy in miniInit() rather than letting a stale launcher hint
    // overwrite the mounted data home used for CFG/VMC sidecars in this process.
    if (gOPLPart[0] != '\0' && strcmp(gOPLPart, launchPart) != 0)
        LOG("HDD mini launch: ignoring stale partition hint %s; policy selected %s\n", launchPart, gOPLPart);

    if (gHDDPrefix != NULL && gHDDPrefix[0] != '\0') {
        snprintf(path, sizeof(path), "%sCFG/%s.cfg", gHDDPrefix, gAutoLaunchGame->startup);
        configSet = configAlloc(0, NULL, path);
        configRead(configSet);
    } else {
        // HDL autolaunch does not require a persistent config/art PFS home. Keep the launch alive
        // with defaults only; never format a NULL prefix into a bogus path.
        configSet = configAlloc(0, NULL, NULL);
    }
    if (configSet == NULL) {
        free(gAutoLaunchGame);
        gAutoLaunchGame = NULL;
        miniDeinit(NULL);
        return;
    }

    hddLaunchGame(NULL, -1, configSet);
}

static void autoLaunchBDMGame(char *argv[])
{
    char path[256];
    config_set_t *configSet;

    miniInit(BDM_MODE);

    gAutoLaunchBDMGame = malloc(sizeof(base_game_info_t));
    if (gAutoLaunchBDMGame == NULL) {
        miniDeinit(NULL);
        return;
    }
    memset(gAutoLaunchBDMGame, 0, sizeof(base_game_info_t));

    // nameLen MUST be initialised and the return MUST be checked. isValidIsoName (supportbase.c)
    // returns 0 WITHOUT writing *pNameLen for anything that is not *.iso/*.zso -- so an unchecked
    // call left nameLen holding stack garbage, which then became the length argument of the two
    // strncpy calls below and the index of the NUL store after them. Any launcher handing OPL a
    // name it does not recognise wrote an arbitrary distance past a 0x100-byte heap allocation.
    int nameLen = 0;
    int format = isValidIsoName(argv[1], &nameLen);
    // Reject unsupported / over-long filenames. Clamping nameLen instead would desync the
    // extension offset from the real suffix position, so bail rather than repair.
    if (format <= 0 || nameLen < 0 || nameLen > ISO_GAME_NAME_MAX) {
        free(gAutoLaunchBDMGame);
        gAutoLaunchBDMGame = NULL;
        miniDeinit(NULL);
        return;
    }
    if (format == GAME_FORMAT_OLD_ISO) {
        strncpy(gAutoLaunchBDMGame->name, &argv[1][GAME_STARTUP_MAX], nameLen);
        gAutoLaunchBDMGame->name[nameLen] = '\0';
        strncpy(gAutoLaunchBDMGame->extension, &argv[1][GAME_STARTUP_MAX + nameLen], sizeof(gAutoLaunchBDMGame->extension));
        gAutoLaunchBDMGame->extension[sizeof(gAutoLaunchBDMGame->extension) - 1] = '\0';
    } else {
        strncpy(gAutoLaunchBDMGame->name, argv[1], nameLen);
        gAutoLaunchBDMGame->name[nameLen] = '\0';
        strncpy(gAutoLaunchBDMGame->extension, &argv[1][nameLen], sizeof(gAutoLaunchBDMGame->extension));
        gAutoLaunchBDMGame->extension[sizeof(gAutoLaunchBDMGame->extension) - 1] = '\0';
    }

    snprintf(gAutoLaunchBDMGame->startup, sizeof(gAutoLaunchBDMGame->startup), "%s", argv[2]);

    if (strcasecmp("DVD", argv[3]) == 0)
        gAutoLaunchBDMGame->media = SCECdPS2DVD;
    else if (strcasecmp("CD", argv[3]) == 0)
        gAutoLaunchBDMGame->media = SCECdPS2CD;

    gAutoLaunchBDMGame->format = format;
    gAutoLaunchBDMGame->parts = 1; // ul not supported.

    gAutoLaunchDeviceData = malloc(sizeof(bdm_device_data_t));
    if (gAutoLaunchDeviceData == NULL) {
        free(gAutoLaunchBDMGame);
        gAutoLaunchBDMGame = NULL;
        miniDeinit(NULL);
        return;
    }
    memset(gAutoLaunchDeviceData, 0, sizeof(bdm_device_data_t));
    // memset leaves these at 0, and 0 is a VALID BDM_TYPE / a valid UDMA mode -- so an autolaunch
    // that never identifies the device would claim a concrete type instead of "unknown".
    gAutoLaunchDeviceData->bdmDeviceType = BDM_TYPE_UNKNOWN;
    gAutoLaunchDeviceData->bdmHddIsLBA48 = -1;
    gAutoLaunchDeviceData->ataHighestUDMAMode = -1;

    char apaDevicePrefix[BDM_DEVICE_ROOT_MAX] = {0};
    // The driver name and device index MUST be recorded together with the slot they came from.
    // Reading them straight into gAutoLaunchDeviceData meant every probed slot overwrote them, so
    // after the loop they described the LAST slot opened while apaDevicePrefix still named the
    // first -- the config path and the device identity disagreed whenever more than one mass
    // device was present.
    int selectedMassSlot = -1;
    delay(8);
    // Loop through mass0: to mass4:
    for (int i = 0; i <= 4; i++) {
        snprintf(path, sizeof(path), "mass%d:/", i);
        int dir = fileXioDopen(path);

        if (dir >= 0) {
            char detectedDriver[sizeof(gAutoLaunchDeviceData->bdmDriver)] = {0};
            int detectedDeviceIndex = -1;

            // Every slot in this sweep gets asked, including ones whose backing device has gone --
            // and asking for the name WITH a return buffer there faults the IOP outright. Boot is
            // the worst place to do that, so go through the guarded reader and only chase the
            // device number once it has confirmed a mounted block device. See bdmReadDriverName().
            if (bdmReadDriverName(dir, detectedDriver, sizeof(detectedDriver)) >= 0)
                fileXioIoctl2(dir, USBMASS_IOCTL_GET_DEVICE_NUMBER, NULL, 0, &detectedDeviceIndex, sizeof(detectedDeviceIndex));
            fileXioDclose(dir);

            if (selectedMassSlot < 0) {
                selectedMassSlot = i;
                snprintf(gAutoLaunchDeviceData->bdmDriver, sizeof(gAutoLaunchDeviceData->bdmDriver), "%s", detectedDriver);
                gAutoLaunchDeviceData->massDeviceIndex = detectedDeviceIndex;
                snprintf(apaDevicePrefix, sizeof(apaDevicePrefix), "mass%d:", i);
            }

            if (!strcmp(detectedDriver, "ata") && strlen(detectedDriver) == 3) {
                selectedMassSlot = i;
                snprintf(gAutoLaunchDeviceData->bdmDriver, sizeof(gAutoLaunchDeviceData->bdmDriver), "%s", detectedDriver);
                gAutoLaunchDeviceData->massDeviceIndex = detectedDeviceIndex;
                bdmResolveLBA_UDMA(gAutoLaunchDeviceData); // fills bdmHddIsLBA48 / ataHighestUDMAMode
                snprintf(apaDevicePrefix, sizeof(apaDevicePrefix), "mass%d:", i);
                break; // Exit the loop if "ata" device is found
            }
        } else {
            // Retry for mass0: only
            if (i == 0) {
                delay(6);
                i--;
            } else {
                break;
            }
        }
        delay(6);
    }

    // No mass device answered at all: keep the historical mass0: guess rather than an empty prefix,
    // which would build a rootless config path.
    if (selectedMassSlot < 0)
        snprintf(apaDevicePrefix, sizeof(apaDevicePrefix), "mass0:");

    // bdmDeviceRoot was never written on this path, so the launch legs that resolve paths through
    // it saw an empty string.
    snprintf(gAutoLaunchDeviceData->bdmDeviceRoot, sizeof(gAutoLaunchDeviceData->bdmDeviceRoot), "%s", apaDevicePrefix);

    if (gBDMPrefix[0] != '\0') {
        snprintf(path, sizeof(path), "%s%s/CFG/%s.cfg", apaDevicePrefix, gBDMPrefix, gAutoLaunchBDMGame->startup);
        snprintf(gAutoLaunchDeviceData->bdmPrefix, sizeof(gAutoLaunchDeviceData->bdmPrefix), "%s%s/", apaDevicePrefix, gBDMPrefix);
    } else {
        snprintf(path, sizeof(path), "%sCFG/%s.cfg", apaDevicePrefix, gAutoLaunchBDMGame->startup);
        snprintf(gAutoLaunchDeviceData->bdmPrefix, sizeof(gAutoLaunchDeviceData->bdmPrefix), "%s", apaDevicePrefix);
    }


    configSet = configAlloc(0, NULL, path);
    configRead(configSet);

    bdmLaunchGame(NULL, -1, configSet);
}

// --------------------- Main --------------------
// Memory-card roots require an explicit slash for file creation (mc1:/FILE, not mc1:FILE).
// Some launchers provide an equivalent compact path such as mc1:APPS/OPL.ELF or leave getcwd()
// at mc1:. Normalize the generic device representation once, without any launcher-specific branch.
static void normalizeMcBootDir(void)
{
    if (strncmp(gBootDir, "mc", 2) || (gBootDir[2] != '0' && gBootDir[2] != '1') ||
        gBootDir[3] != ':' || gBootDir[4] == '/')
        return;

    size_t len = strlen(gBootDir);
    if (len + 1 >= sizeof(gBootDir))
        return;

    memmove(&gBootDir[5], &gBootDir[4], len - 3);
    gBootDir[4] = '/';
}

static void setBootDir(const char *bootPath)
{
    gBootDir[0] = '\0';
    gBootElfName[0] = '\0';
    if (bootPath == NULL || bootPath[0] == '\0')
        return;

    // Launchers are not consistent about separators (wLaunchELF variants can hand backslash paths);
    // normalize to '/' before splitting so the folder/basename split can't misfire.
    char path[sizeof(gBootDir)];
    snprintf(path, sizeof(path), "%s", bootPath);
    for (char *p = path; *p != '\0'; p++) {
        if (*p == '\\')
            *p = '/';
    }

    const char *slash = strrchr(path, '/');
    if (slash != NULL) {
        size_t len = (size_t)(slash - path); // keep the folder, drop the trailing '/' + filename
        if (len > 0 && len < sizeof(gBootDir)) {
            memcpy(gBootDir, path, len);
            gBootDir[len] = '\0';
            snprintf(gBootElfName, sizeof(gBootElfName), "%s", slash + 1);
        }
    } else {
        const char *colon = strrchr(path, ':'); // "mass0:X.ELF" -> device root "mass0:"
        if (colon != NULL && (size_t)(colon - path) + 1 < sizeof(gBootDir)) {
            size_t len = (size_t)(colon - path) + 1;
            memcpy(gBootDir, path, len);
            gBootDir[len] = '\0';
            snprintf(gBootElfName, sizeof(gBootElfName), "%s", colon + 1);
        }
    }
}

int main(int argc, char *argv[])
{
#ifdef __DECI2_DEBUG
    sysInitDECI2();
#endif

    LOG_INIT();
    PREINIT_LOG("OPL GUI start!\n");

    ChangeThreadPriority(GetThreadId(), 31);

    // reset, load modules
    reset();
    ResetDeckardXParams();

    // Settings live in the boot directory (cwd). Resolve it once, before any config init -- the
    // autolaunch path below calls miniInit() -> configInit(). argv[0] is the launcher's boot path;
    // getcwd() backs it up. Empty only if both are unusable, in which case configInit falls back to
    // the memory-card default.
    setBootDir(argc >= 1 ? argv[0] : NULL);
    if (gBootDir[0] == '\0') {
        char cwd[256];
        if (getcwd(cwd, sizeof(cwd)) != NULL && cwd[0] != '\0') {
            int n = (int)strlen(cwd);
            while (n > 0 && cwd[n - 1] == '/') // configInit appends '/', so drop any trailing one
                cwd[--n] = '\0';
            snprintf(gBootDir, sizeof(gBootDir), "%s", cwd);
        }
    }
    normalizeMcBootDir();

    if (argc >= 5) {
        /* argv[0] boot path
           argv[1] game->startup
           argv[2] str to u32 game->start_sector
           argv[3] legacy OPL data-partition hint (revalidated from hdd0:__common/OPL/conf_hdd.cfg)
           argv[4] "mini" */
        if (!strcmp(argv[4], "mini"))
            autoLaunchHDDGame(argv);
        /* argv[0] boot path
           argv[1] file name (including extention)
           argv[2] game->startup
           argv[3] game->media ("CD" / "DVD")
           argv[4] "bdm" */
        if (!strcmp(argv[4], "bdm"))
            autoLaunchBDMGame(argv);
    }

    init();

    // until this point in the code is reached, only PREINIT_LOG macro should be used
    LOG_ENABLE();

    // queue deffered init which shuts down the intro screen later
    ioPutRequest(IO_CUSTOM_SIMPLEACTION, &deferredInit);

    guiIntroLoop();

    // No writable config home? Say so NOW, not after the user has changed settings and lost them.
    // The '?' only survives configGetDir() when the home fell all the way back to the "mc?:OPL"
    // default AND checkMC() found no card to substitute into it -- i.e. no boot identity, no custom
    // path, no discovery hit, and no local card. Reported from the field as "settings don't save,
    // but they claim to load", which is exactly what silence looks like from the outside.
    // A toast, not a modal: a standing property of the setup, not an error the user just caused,
    // and it must not gate a boot that otherwise works fine.
    if (strchr(configGetDir(), '?') != NULL)
        guiWarning(_l(_STR_SETTINGS_NO_HOME), 6);

    guiMainLoop();

    return 0;
}
