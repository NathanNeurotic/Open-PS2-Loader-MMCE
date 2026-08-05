/*
  Copyright 2009, Volca
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.
*/

#include "include/opl.h"
#include "include/diag.h"
#include "include/ioman.h"
#include "include/gui.h"
#include "include/guigame.h"
#include "include/renderman.h"
#include "include/lang.h"
#include "include/themes.h"
#include "include/textures.h"
#include "include/pad.h"
#include "include/texcache.h"
#include "include/tar.h"
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
#include "include/mmcesupport.h"

#include "include/cheatman.h"
#include "include/sound.h"
#include "include/xparam.h"
#include "include/favsupport.h"
#include "include/vcdsupport.h"
#include "include/folderbrowse.h"

// FIXME: We should not need this function.
//        Use newlib's 'stat' to get GMT time.
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h> // iox_stat_t
int configGetStat(config_set_t *configSet, iox_stat_t *stat);

#include <unistd.h>
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
    mod->menuItem.hints = NULL;
    mod->menuItem.icon_id = -1;
    mod->menuItem.current = NULL;
    mod->menuItem.submenu = NULL;
    mod->menuItem.last = NULL; // coverflow wrap tail (device re-init must not leave it dangling)
    mod->menuItem.pagestart = NULL;
    mod->menuItem.remindLast = 0;
    mod->menuItem.refresh = NULL;
    mod->menuItem.text = NULL;
    mod->menuItem.text_id = -1;
    mod->menuItem.userdata = NULL;
}

// forward decl
static void clearMenuGameList(opl_io_module_t *mdl);
static void moduleCleanup(opl_io_module_t *mod, int exception, int modeSelected, int modeSelected2);
static void reset(void);
static void deferredAudioInit(void);

// frame counter
static unsigned int frameCounter;
// Per-mode background-rescan throttle (Fix B): the every-frame (updateDelay==0) device rescans
// enumerate the SIO2/mass bus; space them by a minimum wall-clock interval so they don't run
// unthrottled when cover art is off (art-pending used to incidentally pace them -- the "art off made
// MMCE worse" bug). A real device change bypasses the throttle via bdmGetGeneration().
static clock_t lastBgRescan[MODE_COUNT];
static unsigned int lastSeenBdmGeneration;

static char errorMessage[256];

static opl_io_module_t list_support[MODE_COUNT];

// Global data
char *gBaseMCDir;
int ps2_ip_use_dhcp;
int ps2_ip[4];
int ps2_netmask[4];
int ps2_gateway[4];
int ps2_dns[4];
volatile int gLastSaveErrno = 0;

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
int bdmCacheSize;
int hddCacheSize;
int smbCacheSize;
int gMMCEIGRSlot;
int gMMCESlot;
int gMMCEAckWaitCycles;
int gMMCEUseAlarms;
int gMMCEEnableGameID;
int gApplyGameID;
int gEnableUSB;
int gEnableILK;
int gEnableMX4SIO;
int gEnableBdmHDD;
int gEnableUDPBD;
int gNetBootProtocol; // NET_BOOT_UDPBD | NET_BOOT_UDPFS (legacy shadow, derived from gNetworkProtocol)
int gNetworkProtocol; // enum NETWORK_PROTOCOL -- authoritative backend selector (Off/SMB/UDPBD/UDPFSBD/UDPFS)
int gNetStartMode;    // START_MODE_* -- the Off/Manual/Auto network start row (see the 3-row Network setting)
int gSMBDialect;      // enum SMB_DIALECT -- SMBv1 (default) or SMB2; sub-row of the SMB protocol choice
int gAutosort;
int gAutoRefresh;
int gEnableNotifications;
int gEnableArt;
int gEnableBGArt;
int gEnableArtTar;
int gEnableAnalogNav;
int gArtDelay;
int gWideScreen;
int gDefaultGameView;
int gVMode; // 0 - Auto, 1 - PAL, 2 - NTSC
int gXOff;
int gYOff;
int gOverscan;
int gSelectButton;
int gHDDGameListCache;
int gEnableSFX;
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
// Boot toast (rendered by guiShowNotifications alongside showCfgPopup):
int showNetDhcpPopup; // a UDP transport is selected but IP Type is DHCP -- ministack needs a static IP
#ifdef PADEMU
int gEnablePadEmu;
int gPadEmuSettings;
int gPadMacroSource;
int gPadMacroSettings;
#endif
int gScrollSpeed;
char gExitPath[256];
char gNeutrinoArgs[256];           // extra command-line flags appended to every Neutrino launch
char gNeutrinoPath[256];           // custom neutrino.elf path; "" -> auto-detect on mc0:/mc1:
int gNeutrinoDevice;               // Neutrino ELF device (NEUTRINO_DEV_*); Auto scans mc0/mc1 + honors a legacy gNeutrinoPath
int gDefaultCoreLoader;            // global default Loader Core (0=<OPL>, 1=Neutrino); per-game $CoreLoader overrides, absent key = follow this
int gNeutrinoVideoDefault;         // global default Neutrino -gsm video mode (0=Off..5=1080i x3); per-game $NeutrinoVideo overrides, absent key = follow this (R3Z3N's 1080i x3 "1080p impression" trick, now global)
int gNeutrinoGsmCompDefault;       // global default -gsm ":c" field-flip half (0=off, 1-3=type)
int gNeutrinoElfArg;               // default-on (settings key only, no UI): auto-emit -elf=cdrom0: on Neutrino launches (parity Delta-10)
char gPopstarterPath[256];         // custom POPSTARTER.ELF path (used only when gPopstarterDevice == POPS_DEV_CUSTOM)
int gPopstarterDevice;             // POPSTARTER.ELF device (POPS_DEV_*); Default = cwd then VCD device; legacy path -> Custom
int gPopstarterRetroGemGameID = 1; // RetroGEM Game ID optical barcode for VCD launches (1=on, default)

int gBdmaSource;        // BDMA SOURCE device family (VCD_BDMA_SRC_*) to read exFAT driver variants from
int gBdmaMode;          // BDMA MODE last reflected from the mc?:/POPSTARTER/ marker (VCD_BDMA_*); not persisted
int gBdmaApplyOnLaunch; // auto-equip the launched VCD's matching exFAT driver before boot (1=on, default)
int gVcdHideGameId;     // display-only: hide a leading PS1 game-ID prefix from the VCD list (1=on, default off)
int gVcdFirstDiscOnly;  // #118: hide discs 2+ of a multi-disc PS1 set from the device VCD lists (1=on, default off)
int gEnableDebug;
int gPS2Logo;
int gDefaultDevice;
int gEnableWrite;
char gBDMPrefix[32];
char gMMCEPrefix[32];
char gETHPrefix[32];
int gRememberLastPlayed;
int gEnableFolderNav;
int gEnableRumble;
int KeyPressedOnce;
int gAutoStartLastPlayed;
int RemainSecs, DisableCron;
clock_t CronStart;
unsigned char gDefaultBgColor[3];
unsigned char gDefaultPlasBlendColor[3]; // plasma gradient low end (parity-audit #15); black = historical look
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

        if (!(mod->support->flags & MODE_FLAG_NO_COMPAT) || gEnableWrite)
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
            // NEIGHBOUR's reallocated array -- the permanent wrong-cover mapping (test note #2).
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
                // Menu rumble: play out the confirm bump and stop the motors BEFORE the launch prep.
                // Everything below blocks the GUI thread -- the config read, guiShowGameID's frame hold,
                // and all of itemLaunch (sbPrepare, VMC checks, cheats, fragment counting, and
                // mmceSendGameID's card-switch wait) -- and readPads(), which ticks the decay, does not
                // run during any of it. Without this the bump would buzz for the entire loading screen.
                // Costs ~90ms at most, on a path that already takes seconds. No-op when rumble is off.
                padRumbleFlush();

                config_set_t *configSet = menuLoadConfigDirect();
                // Flash the GameID barcode (Pixel FX/RetroGEM HDMI auto-profile) before handoff; this
                // single menu chokepoint covers both the Neutrino and OPL-native cores. No-op when off.
                guiShowGameID(support->itemGetStartup(support, curMenu->current->item.id));
                support->itemLaunch(support, curMenu->current->item.id, configSet);
            }
        } else {
            (void)cacheCancelPendingImageLoadsTimed(MENU_MIN_INACTIVE_FRAMES);

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
        if (support->mode == FAV_MODE)
            loadFavourites(); // re-read favourites.bin: raises the FAV one-shot + schedules the rebuild
        else
            ioPutRequest(IO_MENU_UPDATE_DEFFERED, &support->mode);
        sfxPlay(SFX_CONFIRM);
    }
}

// Folder browsing: the cancel button (whichever of Cross/Circle is NOT the select button) ascends one
// folder level when inside a subfolder. At the device root it is a no-op -- there is no "back" out of
// a device page -- so this never changes behaviour for users who aren't browsing folders.
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
        if (support == NULL || !vcdViewActive(support->mode))
            menuRequestInfoSize();
        // NOTE: do NOT force a synchronous menuLoadConfigDirect() here for VCD to pre-load the
        // #Format/#System/#DiscType badge config. That read runs on the GUI thread, and on a WEDGED
        // MMCE card it would block the whole UI outright instead of the (harmless, non-blocking) busy
        // spinner the async render path raises -- strictly worse than the spinner it was meant to hide
        // (Andrew, #120: the card wedges card-side during browsing, so any GUI-thread card read is a
        // freeze risk). The badges resolve fine via the render's own async config read.
        guiSwitchScreen(GUI_SCREEN_INFO);
    }
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
        // FAV items report their source's flags dynamically so the Options menu matches.
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

// R3: toggle the highlighted item's favourite state. On the Favourites tab it removes; on a
// source list it adds/removes and updates the star. The reload runs via the single deferred path.
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

    // Folder browsing: on a source list a folder row is not favouritable, and a game favourited from
    // INSIDE a subfolder would resolve by index against the wrong (root) view later -- suppress both in
    // v1. Root favourites are unchanged; the Favourites tab's own removals are unaffected.
    if (support->mode != FAV_MODE && (it->isFolder || folderDepth(support->mode) > 0))
        return;

    if (support->mode == FAV_MODE) {
        favRemoveByIndex(it->id);
    } else {
        // A favourite captured while the device page is in its L3 VCD view is a PS1/.VCD favourite;
        // it resolves + launches as POPSTARTER and lands in the Favourites tab's own VCD view.
        int isVcd = vcdViewActive(support->mode) ? 1 : 0;
        // ...but only on a device whose VCD favourites can actually be resolved/launched later
        // (itemLaunchVcd present). Storing a VCD favourite on a device without it would leave a
        // permanently-hidden, unlaunchable record, so make R3 an honest no-op (no record, no star, no
        // confirm sound) rather than a misleading "saved". Every VCD-capable device (BDM/ETH/MMCE/HDD)
        // implements itemLaunchVcd today -- this is a future-proof backstop. Disc favourites are
        // unaffected (isVcd == 0 there).
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
    sfxPlay(SFX_CONFIRM);
    guiWarning(vcdViewActive(support->mode) ? _l(_STR_VCD_ON) : _l(_STR_VCD_OFF), 2);
    ioPutRequest(IO_MENU_UPDATE_DEFFERED, &support->mode);
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
    mod->menuItem.last = NULL; // coverflow wrap tail
    mod->menuItem.current = NULL;
    mod->menuItem.pagestart = NULL;
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
        mdl->menuItem.last = NULL; // coverflow wrap tail (list clear/refresh must reset it)
        mdl->menuItem.current = NULL;
        mdl->menuItem.pagestart = NULL;
        mdl->menuItem.remindLast = 0;

        // unlock
        guiUnlock();
    }
}

// Favourites accessor (see opl.h): keep list_support[] file-static, but let favsupport.c
// reach the FAV module through this thin wrapper. (The FAV list is cleared via the normal
// deferred updateMenuFromGameList path, so no separate clear wrapper is needed.)
opl_io_module_t *oplGetModule(int mode)
{
    return &list_support[mode];
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
    else if (mode == MMCE_MODE)
        startMode = gMMCEStartMode;
    else if (mode == FAV_MODE)
        startMode = gFAVStartMode;
    else if (mode == UDPFS_MODE)
        // The UDPFS filesystem tab lives only while its protocol is selected. It has no Auto/Manual
        // sub-mode (the unified selector is Off/on); treat "selected" as Manual so the IRX chain + mount
        // happen when the user enters the tab, mirroring SMB's default start behavior.
        // UDPFS filesystem tab honours the network start row: Auto loads the udpfs IRX chain + mount at
        // boot, Manual defers it to tab-entry. (Was a hardcoded MANUAL before the 3-row Network setting.)
        startMode = (gNetworkProtocol == NET_PROTO_UDPFS) ? gNetStartMode : START_MODE_DISABLED;

    if (startMode) {
        if (!mod->support) {
            mod->support = itemList;
            mod->support->owner = mod;
            initMenuForListSupport(mod);

            // First-ever tab while the GUI sits on the start menu (fresh/default config, every
            // mode OFF): take the user TO the new tab, or they stay on the "default interface"
            // with no visible way to it -- exactly how PixeliGer's #254 follow-up presented
            // ("Apps section does not appear in the default interface even if enabled, until a
            // theme is applied" -- the theme apply was just the thing that returned them to the
            // tab screen). Boot is unaffected: deferredInit's default-device select queues AFTER
            // this one and wins; multi-mode enables land on the last registered tab.
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
            // its own path); ETH/UDPFS/MMCE/APP/FAV/HDD have no such hook, so restore it here.
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

static void initAllSupport(int force_reinit)
{
    guiSetBootStatus(_l(_STR_BOOT_SCANNING_BDM));
    if (gBootInProgress)
        guiRenderGreetingScreen();
    LOG("BOOT scan: bdmEnumerateDevices() begin\n");
    bdmEnumerateDevices();
    LOG("BOOT scan: bdmEnumerateDevices() done; MMCE initSupport begin\n");
    // Distinct banner for the MMCE init phase so a frozen boot screen LOCALIZES a scan-hang to this
    // phase (vs the BDM/ATA enumerate above) on ANY build -- FifthFox reported a "Scanning..." freeze
    // on a FAT console and the exact culprit (slow ATA/dev9 probe in bdmEnumerateDevices vs the MMCE
    // slot devctl here) needs pinning on hardware.
    if (gBootInProgress) {
        guiSetBootStatus(_l(_STR_BOOT_SCANNING_MC));
        guiRenderGreetingScreen();
    }
    initSupport(mmceGetObject(0), MMCE_MODE, force_reinit);
    LOG("BOOT scan: MMCE initSupport done\n");
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

    // Δ4 (NHDDL parity): arm the MMCE GameID transport at boot and on every settings apply -- instead
    // of self-arming inside mmceSendGameID during a launch (an IRX load at the launch's most fragile
    // moment; issue #51's fix with the timing corrected). Idempotent; a failure is a harmless LOG.
    //
    // BUT NEVER AHEAD OF THE MENU (GZAst, HW 2026-07-16: boot stuck forever on "Arming MMCE
    // game-ID..."). The arm is a blocking SifExecModuleBuffer of mmceman.irx with no possible EE-side
    // timeout, and posting it here during BOOT put it in the IO FIFO ahead of deferredInit -- so
    // GUI_INIT_DONE queued behind a wedgeable module load, and a rig where mmceman's probe hangs never
    // reached the menu at all. GameID arming is MENU-time work by design (that is the whole Δ4 point):
    // during boot, deferredInit posts it AFTER itself, so a wedge costs GameID on a degraded IO worker
    // instead of the console. Post-boot (settings apply), arm immediately as before. Default-on
    // exposure note: gMMCEEnableGameID ships 1, so EVERY rig pays this load -- including ones with no
    // MMCE hardware anywhere, like the reporter's network-only setup.
    if (gMMCEEnableGameID && !gBootInProgress)
        ioPutRequest(IO_CUSTOM_SIMPLEACTION, &mmceArmGameIDTransport);
}

static void deinitAllSupport(int exception, int modeSelected, int modeSelected2)
{
    for (int i = 0; i < MODE_COUNT; i++) {
        if (list_support[i].support != NULL)
            moduleCleanup(&list_support[i], exception, modeSelected, modeSelected2);
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
    int mode;

    if (device == NULL)
        return -1;

    mode = oplPath2Mode(device);
    if (mode >= 0)
        return oplGetAppImageByMode(mode, folder, isRelative, value, suffix, resultTex, psm);

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

              An enabled-but-unmounted slot (a BDM index with nothing plugged in, MMCE with no card,
              ETH with no share) returns an EMPTY prefix. "%sAPPS" then yields the bare relative
              path "APPS", which the PS2 resolves against the CWD -- OPL's own boot folder. When OPL
              boots from a device root (PixeliGer's case: USB), that is the SAME directory the real
              mass0: prefix already scanned, so every app was discovered twice and appeared twice in
              the list.

              It also explains why dedup did not help: the two hits arrive with different path
              strings ("mass0:APPS/..." vs "APPS/..."), so they are legitimately distinct entries as
              far as the dedup key is concerned.
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
    char appsPath[128];

    for (i = MODE_COUNT - 1; i >= 0; i--) {
        listSupport = list_support[i].support;
        if ((listSupport != NULL) && (listSupport->enabled) && (listSupport->itemGetPrefix != NULL)) {
            char *prefix = listSupport->itemGetPrefix(listSupport);
            snprintf(appsPath, sizeof(appsPath), "%sCFG%s%s.cfg", prefix, i == ETH_MODE ? "\\" : "/", name);

            fd = openFile(appsPath, O_RDONLY);
            if (fd >= 0) {
                appConfig = configAlloc(0, NULL, appsPath);
                close(fd);
                return appConfig;
            }
        }
    }

    /* Apps config not found on any device, go with last tested device.
       Does not matter if the config file could be loaded or not */
    appConfig = configAlloc(0, NULL, appsPath);

    return appConfig;
}

// ----------------------------------------------------------
// ----------------------- Updaters -------------------------
// ----------------------------------------------------------
static void updateMenuFromGameList(opl_io_module_t *mdl)
{
    guiExecDeferredOps();
    clearMenuGameList(mdl);

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
                gup->submenu.owner = (void *)mdl->support; // producing list; Favourites proxies back to it
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
        if (gup) { // OOM-safe: skip the sort op rather than deref NULL
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
// ~2 s this drops the steady SIO2/mass enumeration rate (and, for MMCE, the slot-probe drip) well
// below the old every-60-frames cadence, cutting MX4SIO<->mmceman bus contention, while a real
// device change still refreshes immediately (BdmGeneration bypass below). clock() = microseconds.
#define MENU_BG_RESCAN_MIN_INTERVAL_TICKS  (2 * CLOCKS_PER_SEC)
/*
  Idle time required before STEADY-STATE background probes may run at all, in frames (~1 s NTSC).

  The short gate (MENU_MIN_INACTIVE_FRAMES = 8, ~133 ms) is tuned for "the user paused between
  inputs" -- right for waking cover-art loads, and WRONG for device probes. Frame analysis of
  zackcage6's Beta-3449 capture (#271) showed why: tapping through the list at a steady ~4.3
  taps/s leaves ~130 ms release gaps, which sit exactly AT the short gate, so the throttled
  probes leaked into active navigation -- slides went missing at t=1.07/3.27/4.80/7.77 s,
  spacings of 2.2/1.5/3.0 s, the quasi-periodic signature of the 1 s hook tick x 2 s per-device
  throttle (multiple enabled devices desync, so events can land closer than 2 s). An MMCE/SIO2
  presence probe contends with freepad on the same bus; the tap landing inside the resulting
  read-miss burst is eaten. At 4.3 taps/s a ~2 s clock lands every ~9 games of a 7-game cycle,
  drifting slowly -- his "it does hang on certain games casually, in a pattern".

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

    // Schedule updates of the every-frame (updateDelay==0) list handlers -- all BDM/MX4SIO, and MMCE
    // while no card is present. These enumerate the SIO2/mass bus, so throttle each to a minimum
    // wall-clock interval instead of firing every MENU_GENERAL_UPDATE_DELAY frames. That interval used
    // to be supplied incidentally by cover-art-pending suppressing the whole hook (line above); with
    // art OFF that never fired, so the rescans ran unthrottled and contended on SIO2 (the "cover art
    // off made MMCE worse" bug). A genuine BDM device change (hotplug/removal via BdmGeneration, or a
    // Device-Settings apply) bypasses the throttle so detection stays immediate.
    if (frameCounter % MENU_GENERAL_UPDATE_DELAY == 0) {
        unsigned int gen = bdmGetGeneration();
        int genChanged = (gen != lastSeenBdmGeneration);
        clock_t now = clock();
        // Consume the generation bump only if every hotplug-driven enqueue is ACCEPTED (CodeRabbit,
        // #294): ioPutRequest can fail (queue blocked during teardown, allocation failure), and
        // committing lastSeenBdmGeneration up front would silently eat the one immediate-rescan
        // event a hotplug gets -- the new device's tab then waits for the next steady-state probe,
        // which after this change also needs a second of real idleness. Leaving the generation
        // unconsumed makes the next 1 s tick retry the whole event instead.
        int genConsumed = 1;
        for (i = 0; i < MODE_COUNT; i++) {
            if ((list_support[i].support && list_support[i].support->enabled) && (list_support[i].support->updateDelay == 0)) {
                int mode = list_support[i].support->mode;
                // elapsed form is single-wrap-safe; zero-initialized timestamp allows one immediate rescan.
                // genChanged (hotplug / Device-Settings apply) deliberately requires NEITHER longIdle:
                // it fires precisely BECAUSE a device changed, the scan populates a page the
                // user is waiting on, and a plugged device should be noticed in the
                // next tap gap rather than after a second of stillness (#271).
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
    guiSetFrameHook(&menuUpdateHook);
}

static void errorMessageHook()
{
    guiMsgBox(errorMessage, 0, NULL);
    clearErrorMessage();
}

void setErrorMessageWithCode(int strId, int error)
{
    snprintf(errorMessage, sizeof(errorMessage), _l(strId), error);
    guiSetFrameHook(&errorMessageHook);
}

void setErrorMessage(int strId)
{
    snprintf(errorMessage, sizeof(errorMessage), _l(strId));
    guiSetFrameHook(&errorMessageHook);
}

// Two-argument (path + errno) variant, for a write failure that wants to name WHERE and WHY. The
// per-game save path (#245) uses it to surface the config target and gLastSaveErrno, matching
// the diagnostic the global saveConfig failure already prints -- so a failed mmceN:/CFG write reports
// the errno on screen instead of a bare "Error writing settings!".
void setErrorMessagePathCode(int strId, const char *path, int error)
{
    snprintf(errorMessage, sizeof(errorMessage), _l(strId), path ? path : "", error);
    guiSetFrameHook(&errorMessageHook);
}

// ----------------------------------------------------------
// ------------------ Configuration handling ----------------
// ----------------------------------------------------------

static int lscstatus = CONFIG_ALL;
static int lscret = 0;
static const char *configPathRedirectFile = "config.path";

static int readConfigPathRedirect(char *outPath, int outPathLen)
{
    int fd;
    int len;

    fd = open((char *)configPathRedirectFile, O_RDONLY);
    if (fd < 0)
        return 0;

    len = read(fd, outPath, outPathLen - 1);
    close(fd);
    if (len <= 0)
        return 0;

    while (len > 0 && (outPath[len - 1] == '\r' || outPath[len - 1] == '\n' || outPath[len - 1] == ' ' || outPath[len - 1] == '\t'))
        len--;
    outPath[len] = '\0';

    return len > 0;
}

static void writeConfigPathRedirect(const char *path)
{
    int fd = open((char *)configPathRedirectFile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        write(fd, path, strlen(path));
        write(fd, "\n", 1);
        close(fd);
    }
}

// Boot directory (cwd): the device + folder OPL was launched from, e.g. "mass0:/APPS". Settings live
// HERE, on whatever device booted OPL -- no memory-card default, no multi-device discovery. Derived
// once in main() from argv[0] (the launcher's boot path), with getcwd() as a backup. Stays empty only
// when the boot path is undeterminable, in which case _loadConfig/_saveConfig fall back to the legacy
// discovery as a last-ditch sanity so OPL is never left with no config at all.
// Non-static: vcdResolvePopstarter() reads it for the POPSTARTER "Default = cwd" resolution tier.
char gBootDir[256];
// The booted ELF's own filename (argv[0]'s basename). Not a path -- resolveBootDirToMass() probes for
// it to verify WHICH mounted massN: slot is the boot device (launcher slot numbering need not match
// OPL's). Empty when the boot dir came from getcwd() or argv[0] had no filename part.
static char gBootElfName[64];
// BDM_TYPE_* of the resolved boot device, BDM_TYPE_UNKNOWN for non-BDM boots. Set by
// resolveBootDirToMass(); consumed by the _loadConfig flag reconcile and the _saveConfig re-resolve.
static int gBootDirBdmType = BDM_TYPE_UNKNOWN;

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

static int checkLoadConfigBDM(int types)
{
    char path[64];
    int value;
    int bdm_result;

    // Check BDM devices first (mass:/massX:/mmce:/mx4sio: etc). Probe the current settings file,
    // then the legacy name, so existing installs are still discovered (read-fallback migration).
    bdm_result = bdmFindPartition(path, CONFIG_OPL_FILENAME, 0);
    if (!bdm_result)
        bdm_result = bdmFindPartition(path, CONFIG_OPL_FILENAME_LEGACY, 0);

    if (bdm_result) {
        configEnd();
        configInit(path);
        value = configReadMulti(types);
        config_set_t *configOPL = configGetByType(CONFIG_OPL);
        configSetInt(configOPL, CONFIG_OPL_BDM_MODE, START_MODE_AUTO);
        return value;
    }

    return 0;
}

static int checkLoadConfigMC(int types)
{
    int value;
    DIR *dir = opendir("mc0:/");
    if (dir != NULL) {
        closedir(dir);
        configEnd();
        configInit("mc0:OPL");
        value = configReadMulti(types);
        if (value & CONFIG_OPL)
            return value;
    }

    dir = opendir("mc1:/");
    if (dir != NULL) {
        closedir(dir);
        configEnd();
        configInit("mc1:OPL");
        value = configReadMulti(types);
        if (value & CONFIG_OPL)
            return value;
    }

    return 0;
}

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

static int checkLoadConfigBDMHDD(int types)
{
    char path[64];
    int value;

    // Bounded wait so BDM-on-HDD can be detected without long black-screen stalls.
    if (hddLoadModulesReady() && bdmHDDIsPresent(500)) {
        if (bdmFindPartition(path, CONFIG_OPL_FILENAME, 0) || bdmFindPartition(path, CONFIG_OPL_FILENAME_LEGACY, 0)) {
            configEnd();
            configInit(path);
            value = configReadMulti(types);
            config_set_t *configOPL = configGetByType(CONFIG_OPL);
            configSetInt(configOPL, CONFIG_OPL_BDM_MODE, START_MODE_AUTO);
            gEnableBdmHDD = 1;
            configSetInt(configOPL, CONFIG_OPL_ENABLE_BDMHDD, gEnableBdmHDD);
            return value;
        }
    }

    return 0;
}

static int checkLoadConfigHDD(int types)
{
    int value;
    char path[64];

    hddLoadModules();
    hddLoadSupportModules();

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

// When this function is called, the current device for loading/saving config is the memory card.
static int tryAlternateDevice(int types)
{
    char pwd[8];
    char redirectPath[64];
    int value;
    DIR *dir;

    getcwd(pwd, sizeof(pwd));

    if (readConfigPathRedirect(redirectPath, sizeof(redirectPath))) {
        configEnd();
        configInit(redirectPath);
        value = configReadMulti(types);
        if (value & CONFIG_OPL)
            return value;
    }

    // Try both memory cards explicitly before probing slower removable devices.
    if ((value = checkLoadConfigMC(types)) != 0)
        return value;

    // First, try the device that OPL booted from.
    if (!strncmp(pwd, "mass", 4) && (pwd[4] == ':' || pwd[5] == ':')) {
        if ((value = checkLoadConfigBDM(types)) != 0)
            return value;
    } else if (!strncmp(pwd, "hdd", 3) && (pwd[3] == ':' || pwd[4] == ':')) {
        if ((value = checkLoadConfigHDD(types)) != 0)
            return value;
    }

    // Config was not found on the boot device. Check all supported devices.
    // Check MMCE before BDM.
    if ((value = checkLoadConfigMMCE(types)) != 0)
        return value;
    // Check BDM devices.
    if ((value = checkLoadConfigBDM(types)) != 0)
        return value;
    // Check BDM HDD with a short bounded wait.
    if ((value = checkLoadConfigBDMHDD(types)) != 0)
        return value;
    // Check HDD
    if ((value = checkLoadConfigHDD(types)) != 0)
        return value;

    // At this point, the user has no loadable config files on any supported device, so try to find a device to save on.
    // We don't want to get users into alternate mode for their very first launch of OPL (i.e no config file at all, but still want to save on MC)
    // Check for a memory card inserted.
    if (sysCheckMC() >= 0) {
        configPrepareNotifications(gBaseMCDir);
        showCfgPopup = 0;
        return 0;
    }
    // No memory cards? Try a USB device...
    dir = opendir("mass0:");
    if (dir != NULL) {
        closedir(dir);
        configEnd();
        configInit("mass0:");
    } else {
        // No? Check if the save location on the HDD is available.
        dir = opendir(gHDDPrefix);
        if (dir != NULL) {
            closedir(dir);
            configEnd();
            configInit(gHDDPrefix);
        }
    }
    showCfgPopup = 0;

    return 0;
}

// The launcher can hand OPL a boot path that is a BDM launch-binding IDENTITY (ata0:/usb0:/mx4sio0:/
// ilink0:/sd0:) -- e.g. booting from an internal exFAT HDD, whose cwd may come through as ata0: -- OR a
// massN: path whose driver stack simply isn't loaded yet: at boot time the IOP has NO BDM modules at
// all (sysReset loads none; the BDM page only loads them on tab entry), so BOTH families are unreadable
// exactly when settings must load, and the exFAT HDD adds a chicken-and-egg (mounting it needs
// gEnableBdmHDD, which lives in the unreadable config). Resolve HERE, in the deferred config load --
// after ioInit()/bdmInitSemaphore(), before the first configReadMulti() -- by force-loading the boot
// device's stack and rewriting gBootDir to the mounted massN: root, verified against the booted ELF so
// launcher-vs-OPL slot renumbering can't land settings on the wrong stick. Readable prefixes
// (mc/mmce/host/pfs) pass through untouched. uLE's APA-HDD "hdd0:<part>:pfs:/..." form and a boot
// device that never mounts DROP the boot dir so the legacy discovery/alternate-save re-arm (the
// _loadConfig/_saveConfig gates key on gBootDir[0] == '\0') instead of failing forever.
static void resolveBootDirToMass(void)
{
    if (gBootDir[0] == '\0')
        return;

    // uLE hands APA-HDD boots as "hdd0:<partition>:pfs:/..." -- a launch identity OPL can never open
    // (OPL's own APA mount is pfs0: on the +OPL partition, a different namespace). Unresolvable: drop
    // to legacy discovery, whose checkLoadConfigHDD probes the APA config location properly.
    if (!strncmp(gBootDir, "hdd", 3)) {
        LOG("BOOT unresolvable APA boot dir %s -> legacy discovery\n", gBootDir);
        gBootDir[0] = '\0';
        configEnd();
        configInit(NULL);
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
    if (!strncmp(gBootDir, "mmce", 4)) {
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
    gBootDirBdmType = BDM_TYPE_UNKNOWN; // classify fresh from the prefix
    int ret = bdmResolveBootDir(gBootDir, sizeof(gBootDir), gBootElfName, &gBootDirBdmType);
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
        LOG("BOOT boot device %s not mounted after resolve -> keep as config home (mc untouched)\n", before);
        return;
    }
    if (ret > 0 && strcmp(before, gBootDir) != 0) {
        LOG("BOOT resolved boot dir %s -> %s\n", before, gBootDir);
        configEnd();
        configInit(gBootDir); // re-point the config sets from the unreadable prefix to the massN: path
    }
}

// Shared reader for the Neutrino-launch globals (args/path/-elf switch/global default core/device
// TYPE incl. the legacy device-INDEX migration). Factored out so the interactive _loadConfig and the
// autolaunch miniInit can never drift again -- the argv/autolaunch path previously read none of these
// (then only DEFAULT_CORE), so a keyless "Default" game booted Neutrino with a stale AUTO device and
// empty global args there, silently diverging from an interactive launch of the same game.
static void configReadNeutrinoGlobals(config_set_t *configOPL)
{
    configGetStrCopy(configOPL, CONFIG_OPL_NEUTRINO_ARGS, gNeutrinoArgs, sizeof(gNeutrinoArgs));
    configGetStrCopy(configOPL, CONFIG_OPL_NEUTRINO_PATH, gNeutrinoPath, sizeof(gNeutrinoPath));
    configGetInt(configOPL, CONFIG_OPL_NEUTRINO_ELF_ARG, &gNeutrinoElfArg);
    // Global default Loader Core (0=<OPL>, 1=Neutrino). Absent in legacy configs -> keep the
    // reset default (0/<OPL>), so existing installs behave exactly as before this key existed.
    configGetInt(configOPL, CONFIG_OPL_DEFAULT_CORE, &gDefaultCoreLoader);
    configGetInt(configOPL, CONFIG_OPL_NEUTRINO_VIDEO, &gNeutrinoVideoDefault);
    if (gNeutrinoVideoDefault < 0 || gNeutrinoVideoDefault > 5)
        gNeutrinoVideoDefault = 0; // sanitize (indexes system.c gsmVideoTokens at the launch legs)
    configGetInt(configOPL, CONFIG_OPL_NEUTRINO_GSMCOMP, &gNeutrinoGsmCompDefault);
    if (gNeutrinoGsmCompDefault < 0 || gNeutrinoGsmCompDefault > 3)
        gNeutrinoGsmCompDefault = 0;
    // Neutrino Device: prefer the new device-TYPE key; if absent (config predates the picker
    // change), migrate the legacy device-INDEX value -- 0=Auto, 1/2=mc0/mc1 -> MC, 11/12=
    // mmce0/mmce1 -> MMCE, 3-10=mass* -> Auto (a bare massN index can't name a driver type).
    if (!configGetInt(configOPL, CONFIG_OPL_NEUTRINO_DEVTYPE, &gNeutrinoDevice)) {
        int legacyDev = 0;
        if (configGetInt(configOPL, CONFIG_OPL_NEUTRINO_DEVICE, &legacyDev)) {
            if (legacyDev == 1 || legacyDev == 2)
                gNeutrinoDevice = NEUTRINO_DEV_MC;
            else if (legacyDev == 11 || legacyDev == 12)
                gNeutrinoDevice = NEUTRINO_DEV_MMCE;
            else
                gNeutrinoDevice = NEUTRINO_DEV_AUTO;
        }
    }
}

static void _loadConfig()
{
    int value, themeID = -1, langID = -1;
    const char *temp;
    resolveBootDirToMass(); // ata0:/APPS -> mass0:/APPS (boot-device massN:) before the first read
    int result = configReadMulti(lscstatus);
    // Settings come from the boot dir (cwd). Only when the boot path was undeterminable (gBootDir
    // empty -> configInit fell back to the MC default) do we re-enable the legacy multi-device
    // discovery as a last-ditch sanity. With a known boot dir there is NO fallback: a missing cwd
    // config just yields defaults, which the next save writes back to the boot dir.
    if ((lscstatus & CONFIG_OPL) && !(result & CONFIG_OPL) && gBootDir[0] == '\0')
        result = tryAlternateDevice(lscstatus);

    if (lscstatus & CONFIG_OPL) {
        if (result & CONFIG_OPL) {
            config_set_t *configOPL = configGetByType(CONFIG_OPL);

            configGetInt(configOPL, CONFIG_OPL_SCROLLING, &gScrollSpeed);
            configGetColor(configOPL, CONFIG_OPL_BGCOLOR, gDefaultBgColor);
            configGetColor(configOPL, CONFIG_OPL_PLAS_BLEND_COLOR, gDefaultPlasBlendColor);
            configGetColor(configOPL, CONFIG_OPL_TEXTCOLOR, gDefaultTextColor);
            configGetColor(configOPL, CONFIG_OPL_UI_TEXTCOLOR, gDefaultUITextColor);
            configGetColor(configOPL, CONFIG_OPL_SEL_TEXTCOLOR, gDefaultSelTextColor);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_NOTIFICATIONS, &gEnableNotifications);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_COVERART, &gEnableArt);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_BGART, &gEnableBGArt);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_ART_TAR, &gEnableArtTar);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_ANALOG_NAV, &gEnableAnalogNav);
            configGetInt(configOPL, CONFIG_OPL_ART_DELAY, &gArtDelay);
            if (gArtDelay != 0 && gArtDelay != 2 && gArtDelay != 5 && gArtDelay != 8)
                gArtDelay = 2;
            configGetInt(configOPL, CONFIG_OPL_WIDESCREEN, &gWideScreen);
            configGetInt(configOPL, CONFIG_OPL_DEFAULT_GAME_VIEW, &gDefaultGameView);
            // Clamp: an out-of-enum value (hand-edited/corrupt config) is UNRECOVERABLE on-console --
            // vcdViewActive() treats anything but BOTH as a lock, the L3 toggle goes inert and its hint
            // is hidden, so every VCD-capable page silently serves one (possibly empty) list with no
            // way back. The coverflow ints just below have always been clamped; this one was not.
            if (gDefaultGameView < GAME_VIEW_BOTH || gDefaultGameView > GAME_VIEW_VCD)
                gDefaultGameView = GAME_VIEW_BOTH;
            // A boot default-view locked to one type (VCD or ISO) must force the same one-shot
            // rescan the settings dialog does on a view change (gui.c). Without it, vcdViewActive()
            // short-circuits mmce/bdm/hdd/eth NeedsUpdate before the initial-scan trigger and the
            // list stays blank on boot -- a manual SELECT does not recover it (NeedsUpdate still
            // returns 0), only re-toggling the view does. This runs before applyConfig()'s first
            // support scans, so each VCD-capable page consumes the dirty flag on its first refresh.
            if (gDefaultGameView != GAME_VIEW_BOTH)
                vcdMarkAllDirty();
            configGetInt(configOPL, CONFIG_OPL_COVERFLOW_COUNT, &gCoverflowCount);
            configGetInt(configOPL, CONFIG_OPL_COVERFLOW_SCALE, &gCoverflowCenterScale);
            configGetInt(configOPL, CONFIG_OPL_COVERFLOW_ANIM, &gCoverflowAnimSpeed);
            configGetInt(configOPL, CONFIG_OPL_COVERFLOW_DIM, &gCoverflowDimCovers);
            // clamp count to {3,5} on load -- defends a hand-edited conf.cfg
            gCoverflowCount = (gCoverflowCount == 5) ? 5 : 3;
            // clamp the remaining coverflow values too -- a hand-edited conf.cfg
            // otherwise feeds unbounded ints into signed render math (Codex audit, Low 1)
            if (gCoverflowCenterScale < 0)
                gCoverflowCenterScale = 0;
            else if (gCoverflowCenterScale > 1000)
                gCoverflowCenterScale = 1000;
            if (gCoverflowAnimSpeed < 0)
                gCoverflowAnimSpeed = 0;
            else if (gCoverflowAnimSpeed > 5000)
                gCoverflowAnimSpeed = 5000;
            gCoverflowDimCovers = gCoverflowDimCovers ? 1 : 0;

            if (!(getKeyPressed(KEY_TRIANGLE) && getKeyPressed(KEY_CROSS))) {
                configGetInt(configOPL, CONFIG_OPL_VMODE, &gVMode);
            } else {
                // Recovery combo: force 480p PROGRESSIVE (EDTV 640x448p@60, vmode index 3), not
                // Auto -- Auto resolves to region-default interlaced 480i/576i, which is exactly
                // what some modern displays/upscalers fail to sync, leaving the user still blind
                // (upstream OPL PR #1332 uses the same fixed-480p escape hatch).
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
            else
                themeID = thmFindGuiID("<Coverflow>"); // fork default: boot into the Coverflow theme when no theme is saved

            if (configGetStr(configOPL, CONFIG_OPL_LANGUAGE, &temp))
                langID = lngFindGuiID(temp);

            if (configGetInt(configOPL, CONFIG_OPL_SWAP_SEL_BUTTON, &value))
                gSelectButton = value == 0 ? KEY_CIRCLE : KEY_CROSS;

            configGetInt(configOPL, CONFIG_OPL_XSENSITIVITY, &gXSensitivity);
            if (gXSensitivity < 0 || gXSensitivity > 3)
                gXSensitivity = 1;
            configGetInt(configOPL, CONFIG_OPL_YSENSITIVITY, &gYSensitivity);
            if (gYSensitivity < 0 || gYSensitivity > 3)
                gYSensitivity = 1;
            configGetInt(configOPL, CONFIG_OPL_DISABLE_DEBUG, &gEnableDebug);
            configGetInt(configOPL, CONFIG_OPL_PS2LOGO, &gPS2Logo);
            configGetInt(configOPL, CONFIG_OPL_HDD_GAME_LIST_CACHE, &gHDDGameListCache);
            configGetStrCopy(configOPL, CONFIG_OPL_EXIT_PATH, gExitPath, sizeof(gExitPath));
            configGetInt(configOPL, CONFIG_OPL_AUTO_SORT, &gAutosort);
            configGetInt(configOPL, CONFIG_OPL_AUTO_REFRESH, &gAutoRefresh);
            configGetInt(configOPL, CONFIG_OPL_DEFAULT_DEVICE, &gDefaultDevice);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_WRITE, &gEnableWrite);
            configGetInt(configOPL, CONFIG_OPL_HDD_SPINDOWN, &gHDDSpindown);
            configGetStrCopy(configOPL, CONFIG_OPL_MMCE_PREFIX, gMMCEPrefix, sizeof(gMMCEPrefix));
            configGetStrCopy(configOPL, CONFIG_OPL_BDM_PREFIX, gBDMPrefix, sizeof(gBDMPrefix));
            configGetStrCopy(configOPL, CONFIG_OPL_ETH_PREFIX, gETHPrefix, sizeof(gETHPrefix));
            configGetInt(configOPL, CONFIG_OPL_REMEMBER_LAST, &gRememberLastPlayed);
            configGetInt(configOPL, CONFIG_OPL_FOLDER_NAV, &gEnableFolderNav);
            configGetInt(configOPL, CONFIG_OPL_RUMBLE, &gEnableRumble);
            configGetInt(configOPL, CONFIG_OPL_AUTOSTART_LAST, &gAutoStartLastPlayed);
            configGetInt(configOPL, CONFIG_OPL_BDM_MODE, &gBDMStartMode);
            configGetInt(configOPL, CONFIG_OPL_HDD_MODE, &gHDDStartMode);
            configGetInt(configOPL, CONFIG_OPL_ETH_MODE, &gETHStartMode);
            configGetInt(configOPL, CONFIG_OPL_APP_MODE, &gAPPStartMode);
            configGetInt(configOPL, CONFIG_OPL_MMCE_MODE, &gMMCEStartMode);
            configGetInt(configOPL, CONFIG_OPL_FAV_MODE, &gFAVStartMode);
            configGetInt(configOPL, CONFIG_OPL_MMCE_SLOT, &gMMCESlot);
            configGetInt(configOPL, CONFIG_OPL_MMCEIGR_SLOT, &gMMCEIGRSlot);
            configGetInt(configOPL, CONFIG_OPL_MMCE_GAMEID, &gMMCEEnableGameID);
            configGetInt(configOPL, CONFIG_OPL_APPLY_GAMEID, &gApplyGameID);
            configGetInt(configOPL, CONFIG_OPL_MMCE_WAIT_CYCLES, &gMMCEAckWaitCycles);
            configGetInt(configOPL, CONFIG_OPL_MMCE_USE_ALARMS, &gMMCEUseAlarms);
            // One-time pacing migration: builds 2504..2896 shipped -- and therefore PERSISTED into every
            // saved config -- the aggressive 0-cycles/alarms-OFF pair that freezes slow MMCE cards at the
            // first in-game read (alarms OFF removes the driver's only SIO2 timeout). A config carrying
            // exactly that pair without the marker predates the 5/ON default restore: lift it to the
            // known-good values once. A user who re-picks 0/0 in Device Settings afterwards keeps it
            // (the marker persists with the set from here on).
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
            configGetInt(configOPL, CONFIG_OPL_ENABLE_ILINK, &gEnableILK);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_MX4SIO, &gEnableMX4SIO);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_BDMHDD, &gEnableBdmHDD);
            // APA (gHDDStartMode) and BDM-ATA (gEnableBdmHDD) COEXIST -- maintainer directive
            // 2026-07-21, overruling the old #120-audit F-12 exclusivity that force-disabled one here.
            // There is no driver conflict to arbitrate: OPL's "ps2atad" is the SDK's ata_bd build,
            // which registers the drive with BDM while exporting the atad library ps2hdd links
            // against -- one shared stack serves both, the same approach wLaunchELF-R3Z3N, NHDDL and
            // POPSLoader ship. Per-DRIVE arbitration stays where it belongs:
            // hddDetectNonSonyFileSystem() keeps APA off an MBR/GPT/exFAT drive (corruption guard),
            // and BDM only publishes massN: where a FAT/exFAT partition actually exists.
            int udpbdKeyPresent = configGetInt(configOPL, CONFIG_OPL_ENABLE_UDPBD, &gEnableUDPBD);
            configGetInt(configOPL, CONFIG_OPL_NET_BOOT_PROTOCOL, &gNetBootProtocol);
            // Unified network-protocol selector (single SMAP NIC -> at most one transport per session).
            // Read the new key if present (authoritative); otherwise DERIVE it from the three legacy keys,
            // preserving the historical "network BDM wins over SMB" precedence (imported/hand-edited configs
            // could set both; at boot UDPBD loaded first, so it won). NET_PROTO_UDPFS (filesystem) has no
            // legacy encoding, so it is only ever reached by an explicit new-format value -- backward-safe.
            // Since a NETWORK protocol became the shipped DEFAULT (UDPFS, 2026-07-13), the legacy branch
            // must key off the FILE's enable_udpbd, not the defaulted global -- a legacy config must only
            // ever derive from what IT expressed, never inherit a defaulted enable flag.
            if (!configGetInt(configOPL, CONFIG_OPL_NETWORK_PROTOCOL, &gNetworkProtocol)) {
                if (udpbdKeyPresent)
                    gNetworkProtocol = gEnableUDPBD ? ((gNetBootProtocol == NET_BOOT_UDPFS) ? NET_PROTO_UDPFSBD : NET_PROTO_UDPBD) : ((gETHStartMode != START_MODE_DISABLED) ? NET_PROTO_SMB : NET_PROTO_OFF);
                else if (gETHStartMode != START_MODE_DISABLED)
                    gNetworkProtocol = NET_PROTO_SMB;
                // else: the file never expressed ANY network choice -> the shipped default stands (UDPFS)
            }
            // UDPBD (SUDPBDv2) is a first-class protocol, NOT folded away: it is wire-incompatible with
            // UDPRDMA (SUDPBDv2 on 0xBDBD vs UDPFS on 0xF5F6), so users still on the older udpbd-server
            // must be able to keep it. A saved or legacy-derived NET_PROTO_UDPBD is preserved as-is.
            // Re-derive the legacy shadows from the authoritative selector so downstream consumers
            // (ethsupport start path, system.c getDeviceName, bdmsupport) stay consistent no matter which
            // config format was loaded. SMB keeps its prior Auto/Manual start-mode; a fresh SMB pick that
            // had eth_mode=0 gets Manual. Any non-SMB protocol forces SMB off (preserves UDPBD-wins).
            /*
              SMB dialect. Absent key -> SMBv1, so every config written before the picker existed
              keeps the exact behaviour it had. Clamp anything out of range (a hand-edited file, or
              a config written by a later build that offers SMB3) back to SMBv1 rather than handing
              an unknown value to the IOP side.
            */
            if (!configGetInt(configOPL, CONFIG_OPL_SMB_DIALECT, &gSMBDialect))
                gSMBDialect = SMB_DIALECT_SMB1;
            if (gSMBDialect != SMB_DIALECT_SMB1 && gSMBDialect != SMB_DIALECT_SMB2)
                gSMBDialect = SMB_DIALECT_SMB1;

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
            // Reconcile the two persisted halves; a hand-edited/stale config can disagree either way
            // (CodeRabbit review of #199):
            //  - Protocol Off + a live start row is contradictory the OTHER direction: the dialog would
            //    show that start mode against its SMB fallback protocol, so accepting ANY change would
            //    silently enable SMB. Off wins -- it is the authoritative "network is off".
            //  - A live protocol with an Off row (or a value outside the enum -- an out-of-range int can
            //    reach here from a hand-edited file) must start: floor it to Manual.
            if (gNetworkProtocol == NET_PROTO_OFF)
                gNetStartMode = START_MODE_DISABLED;
            else if (gNetStartMode < START_MODE_MANUAL || gNetStartMode > START_MODE_AUTO)
                gNetStartMode = START_MODE_MANUAL;
            // Keep the SMB start-mode shadow in lockstep with the authoritative row.
            if (gNetworkProtocol == NET_PROTO_SMB)
                gETHStartMode = gNetStartMode;

            configGetInt(configOPL, CONFIG_OPL_SFX, &gEnableSFX);
            configGetInt(configOPL, CONFIG_OPL_BOOT_SND, &gEnableBootSND);
            configGetInt(configOPL, CONFIG_OPL_BGM, &gEnableBGM);
            configGetInt(configOPL, CONFIG_OPL_SFX_VOLUME, &gSFXVolume);
            configGetInt(configOPL, CONFIG_OPL_BOOT_SND_VOLUME, &gBootSndVolume);
            configGetInt(configOPL, CONFIG_OPL_BGM_VOLUME, &gBGMVolume);
            configGetStrCopy(configOPL, CONFIG_OPL_DEFAULT_BGM_PATH, gDefaultBGMPath, sizeof(gDefaultBGMPath));
            configReadNeutrinoGlobals(configOPL); // args/path/-elf/global core/device TYPE (+legacy migration); shared with miniInit
            configGetStrCopy(configOPL, CONFIG_OPL_POPSTARTER_PATH, gPopstarterPath, sizeof(gPopstarterPath));
            // POPSTARTER device TYPE (POPS_DEV_*). Absent in legacy configs: a non-empty custom
            // popstarter_path migrates to Custom (honour the old override); otherwise Default (cwd).
            if (!configGetInt(configOPL, CONFIG_OPL_POPSTARTER_DEVICE, &gPopstarterDevice))
                gPopstarterDevice = (gPopstarterPath[0] != '\0') ? POPS_DEV_CUSTOM : POPS_DEV_DEFAULT;
            if (!configGetInt(configOPL, CONFIG_OPL_POPSTARTER_RETROGEM_GAMEID, &gPopstarterRetroGemGameID))
                gPopstarterRetroGemGameID = 1;

            configGetInt(configOPL, CONFIG_OPL_BDMA_SOURCE, &gBdmaSource);
            configGetInt(configOPL, CONFIG_OPL_BDMA_APPLY, &gBdmaApplyOnLaunch);
            configGetInt(configOPL, CONFIG_OPL_VCD_HIDE_GAMEID, &gVcdHideGameId);
            configGetInt(configOPL, CONFIG_OPL_VCD_FIRST_DISC_ONLY, &gVcdFirstDiscOnly);
        }

        // Booted from the internal exFAT HDD (BDM-ATA): the boot-dir resolver force-loaded the HDD
        // stack ignoring gEnableBdmHDD, because the flag lives in the very config that was unreadable
        // until the mount existed (the chicken-and-egg). Reconcile: keep the boot device enabled so its
        // page shows and the next save persists the flag -- mirrors checkLoadConfigBDMHDD's auto-enable.
        // Runs whether or not a config file was found (a fresh install has no file and the OFF default).
        if (gBootDirBdmType == BDM_TYPE_ATA && !gEnableBdmHDD) {
            gEnableBdmHDD = 1;
            configSetInt(configGetByType(CONFIG_OPL), CONFIG_OPL_ENABLE_BDMHDD, gEnableBdmHDD);
            // A loaded APA start mode is left ALONE (coexistence directive 2026-07-21): a second,
            // APA-formatted drive is a real setup, and on this single exFAT boot drive the
            // hddDetectNonSonyFileSystem guard makes the APA leg a harmless no-op anyway.
        }
    }

    if (lscstatus & CONFIG_NETWORK) {
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

    // MAXIMAL-QUIET TEST BUILD (#340, DO NOT MERGE) -- force every fork-only menu-time subsystem
    // OFF regardless of the saved config, so the running environment matches official OPL's as
    // closely as our binary can. Each of these is work official's menu does NOT do while the user
    // navigates, and each is a candidate for manufacturing the pad-read misses:
    //   art/BG art  -> the whole fork texcache IO pipeline (1139 lines vs official's 175): per-
    //                  selection cover + background reads on the USB/IOP path
    //   SFX/BootSND -> per-cursor-move audsrv SIF RPC (official defaults these OFF; the tester's
    //                  official comparison therefore ran silent)
    //   BGM         -> continuous ogg streaming off the USB device
    //   rumble      -> per-frame padSetActDirect RPCs while a pulse is armed
    //   autoRefresh -> background device rescans
    // mmceman's menu-time load is suppressed separately in mmcesupport.c.
    gEnableArt = 0;
    gEnableBGArt = 0;
    gEnableSFX = 0;
    gEnableBootSND = 0;
    gEnableBGM = 0;
    gEnableRumble = 0;
    gAutoRefresh = 0;

    applyConfig(themeID, langID, 0);

    lscret = result;
    lscstatus = 0;
    // Only claim "Config loaded from ..." when a config file was actually READ (#254 follow-up:
    // the toast used to fire unconditionally, so with no conf_opl.cfg anywhere it still said
    // "Config loaded from mass0:" -- which read exactly like the freeze screen on reporters'
    // photos (brenotomaz, PixeliGer), when the real state was the DEFAULT start menu (every
    // device OFF). Defaults are not a load; stay silent then.
    showCfgPopup = (result & CONFIG_OPL) ? 1 : 0;
}

static int trySaveConfigBDM(int types)
{
    char path[64];
    int bdm_result;

    // Check BDM devices first (mass:/massX:/mmce:/mx4sio: etc).
    bdm_result = bdmFindPartition(path, CONFIG_OPL_FILENAME, 1);

    if (bdm_result) {
        configSetMove(path);
        return configWriteMulti(types);
    }

    return -ENOENT;
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

static int trySaveConfigBDMHDD(int types)
{
    char path[64];

    // Bounded wait so save can target BDM-on-HDD without long stalls.
    if (hddLoadModulesReady() && bdmHDDIsPresent(500)) {
        if (bdmFindPartition(path, CONFIG_OPL_FILENAME, 1)) {
            configSetMove(path);
            return configWriteMulti(types);
        }
    }

    return -ENOENT;
}

static int trySaveConfigHDD(int types)
{
    hddLoadModules();
    // Check that the formatted & usable HDD is connected.
    if (hddCheck() == 0) {
        configSetMove(gHDDPrefix);
        return configWriteMulti(types);
    }

    return -ENOENT;
}

static int trySaveConfigMC(int types)
{
    DIR *dir = opendir("mc0:/");
    if (dir != NULL) {
        closedir(dir);
        configSetMove("mc0:OPL");
        if (configWriteMulti(types) > 0)
            return 1;
    }

    dir = opendir("mc1:/");
    if (dir != NULL) {
        closedir(dir);
        configSetMove("mc1:OPL");
        if (configWriteMulti(types) > 0)
            return 1;
    }

    return 0;
}

static int trySaveAlternateDevice(int types)
{
    int value;

    // BOOT DEVICE FIRST. This whole function only runs with an EMPTY gBootDir (see _saveConfig), and the
    // one common way to get here is an APA/uLE boot: resolveBootDirToMass DELIBERATELY blanks a
    // "hdd0:<part>:pfs:/..." boot dir because that launch identity is unopenable, so legacy discovery can
    // find the real APA config via checkLoadConfigHDD. Without this leg the order below then wrote an APA
    // user's settings to a MEMORY CARD in preference to their own HDD -- the config had just been LOADED
    // from pfs0: and would be saved somewhere else. Upstream has this leg (we dropped it in the fork); it
    // restores "the boot device is the config home, mc is only the fallback".
    //
    // Upstream sizes its buffer `char pwd[8]`, which CANNOT hold an APA cwd ("hdd0:+OPL:pfs:/" is 15) --
    // getcwd then fails and it strncmp's an UNINITIALISED stack buffer, so upstream's own leg is a no-op
    // on the exact case it exists for. Size it properly and initialise it: same intent, actually reached.
    // Deliberately NOT touching the pwd[8] in tryAlternateDevice's LOAD path -- that one is only a probe
    // ORDER hint (checkLoadConfigHDD runs unconditionally there regardless), and staying byte-identical to
    // upstream on the APA discovery mechanism is the whole point.
    {
        char pwd[64];
        pwd[0] = '\0';
        if (getcwd(pwd, sizeof(pwd)) != NULL && pwd[0] != '\0') {
            if (!strncmp(pwd, "hdd", 3)) {
                if ((value = trySaveConfigHDD(types)) > 0)
                    return value;
            } else if (!strncmp(pwd, "mass", 4)) {
                if ((value = trySaveConfigBDM(types)) > 0)
                    return value;
            }
        }
    }

    // Then the deterministic fallback order: MC -> MMCE -> BDM -> BDM-HDD -> HDD.
    if (sysCheckMC() >= 0) {
        if ((value = trySaveConfigMC(types)) > 0)
            return value;
    }
    if ((value = trySaveConfigMMCE(types)) > 0)
        return value;
    if ((value = trySaveConfigBDM(types)) > 0)
        return value;
    if ((value = trySaveConfigBDMHDD(types)) > 0)
        return value;
    if ((value = trySaveConfigHDD(types)) > 0)
        return value;

    // We tried everything, but...
    return 0;
}

// configWriteMulti SUMS per-set results, and configWrite returns 1 for an UNMODIFIED set without
// touching the disk -- so a failed write of the master settings can hide behind untouched sibling
// sets and the save reports success ("Settings saved" toast, no retry). configWrite clears a set's
// modified flag only when its write actually succeeded, and _saveConfig configSet*s every set it
// means to save (marking it modified) -- so any REQUESTED set still marked modified after the write
// IS a failed write. Returns 0 in that case, the raw sum otherwise.
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

static void _saveConfig()
{
    char temp[256];

    if (lscstatus & CONFIG_OPL) {
        config_set_t *configOPL = configGetByType(CONFIG_OPL);
        configSetInt(configOPL, CONFIG_OPL_SCROLLING, gScrollSpeed);
        configSetStr(configOPL, CONFIG_OPL_THEME, thmGetValue());
        configSetStr(configOPL, CONFIG_OPL_LANGUAGE, lngGetValue());
        configSetColor(configOPL, CONFIG_OPL_BGCOLOR, gDefaultBgColor);
        configSetColor(configOPL, CONFIG_OPL_PLAS_BLEND_COLOR, gDefaultPlasBlendColor);
        configSetColor(configOPL, CONFIG_OPL_TEXTCOLOR, gDefaultTextColor);
        configSetColor(configOPL, CONFIG_OPL_UI_TEXTCOLOR, gDefaultUITextColor);
        configSetColor(configOPL, CONFIG_OPL_SEL_TEXTCOLOR, gDefaultSelTextColor);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_NOTIFICATIONS, gEnableNotifications);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_COVERART, gEnableArt);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_BGART, gEnableBGArt);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_ART_TAR, gEnableArtTar);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_ANALOG_NAV, gEnableAnalogNav);
        configSetInt(configOPL, CONFIG_OPL_ART_DELAY, gArtDelay);
        configSetInt(configOPL, CONFIG_OPL_WIDESCREEN, gWideScreen);
        configSetInt(configOPL, CONFIG_OPL_DEFAULT_GAME_VIEW, gDefaultGameView);
        configSetInt(configOPL, CONFIG_OPL_COVERFLOW_COUNT, gCoverflowCount);
        configSetInt(configOPL, CONFIG_OPL_COVERFLOW_SCALE, gCoverflowCenterScale);
        configSetInt(configOPL, CONFIG_OPL_COVERFLOW_ANIM, gCoverflowAnimSpeed);
        configSetInt(configOPL, CONFIG_OPL_COVERFLOW_DIM, gCoverflowDimCovers);
        configSetInt(configOPL, CONFIG_OPL_VMODE, gVMode);
        configSetInt(configOPL, CONFIG_OPL_XOFF, gXOff);
        configSetInt(configOPL, CONFIG_OPL_YOFF, gYOff);
        configSetInt(configOPL, CONFIG_OPL_OVERSCAN, gOverscan);
        configSetInt(configOPL, CONFIG_OPL_DISABLE_DEBUG, gEnableDebug);
        configSetInt(configOPL, CONFIG_OPL_PS2LOGO, gPS2Logo);
        configSetInt(configOPL, CONFIG_OPL_HDD_GAME_LIST_CACHE, gHDDGameListCache);
        configSetStr(configOPL, CONFIG_OPL_EXIT_PATH, gExitPath);
        configSetInt(configOPL, CONFIG_OPL_AUTO_SORT, gAutosort);
        configSetInt(configOPL, CONFIG_OPL_AUTO_REFRESH, gAutoRefresh);
        configSetInt(configOPL, CONFIG_OPL_DEFAULT_DEVICE, gDefaultDevice);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_WRITE, gEnableWrite);
        configSetInt(configOPL, CONFIG_OPL_HDD_SPINDOWN, gHDDSpindown);
        configSetStr(configOPL, CONFIG_OPL_MMCE_PREFIX, gMMCEPrefix);
        configSetStr(configOPL, CONFIG_OPL_BDM_PREFIX, gBDMPrefix);
        configSetStr(configOPL, CONFIG_OPL_ETH_PREFIX, gETHPrefix);
        configSetInt(configOPL, CONFIG_OPL_REMEMBER_LAST, gRememberLastPlayed);
        configSetInt(configOPL, CONFIG_OPL_FOLDER_NAV, gEnableFolderNav);
        configSetInt(configOPL, CONFIG_OPL_RUMBLE, gEnableRumble);
        configSetInt(configOPL, CONFIG_OPL_AUTOSTART_LAST, gAutoStartLastPlayed);
        configSetInt(configOPL, CONFIG_OPL_BDM_MODE, gBDMStartMode);
        configSetInt(configOPL, CONFIG_OPL_HDD_MODE, gHDDStartMode);
        configSetInt(configOPL, CONFIG_OPL_ETH_MODE, gETHStartMode);
        configSetInt(configOPL, CONFIG_OPL_APP_MODE, gAPPStartMode);
        configSetInt(configOPL, CONFIG_OPL_MMCE_MODE, gMMCEStartMode);
        configSetInt(configOPL, CONFIG_OPL_FAV_MODE, gFAVStartMode);
        configSetInt(configOPL, CONFIG_OPL_MMCE_SLOT, gMMCESlot);
        configSetInt(configOPL, CONFIG_OPL_MMCEIGR_SLOT, gMMCEIGRSlot);
        configSetInt(configOPL, CONFIG_OPL_MMCE_GAMEID, gMMCEEnableGameID);
        configSetInt(configOPL, CONFIG_OPL_APPLY_GAMEID, gApplyGameID);
        configSetInt(configOPL, CONFIG_OPL_MMCE_WAIT_CYCLES, gMMCEAckWaitCycles);
        configSetInt(configOPL, CONFIG_OPL_MMCE_USE_ALARMS, gMMCEUseAlarms);
        // Always stamp the pacing-migration marker: any config saved by this build carries CURRENT,
        // deliberate pacing values, so the load-time 0/0 lift must never touch them again.
        configSetInt(configOPL, CONFIG_OPL_MMCE_PACING_MIGR, 1);
        configSetInt(configOPL, CONFIG_OPL_BDM_CACHE, bdmCacheSize);
        configSetInt(configOPL, CONFIG_OPL_HDD_CACHE, hddCacheSize);
        configSetInt(configOPL, CONFIG_OPL_SMB_CACHE, smbCacheSize);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_USB, gEnableUSB);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_ILINK, gEnableILK);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_MX4SIO, gEnableMX4SIO);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_BDMHDD, gEnableBdmHDD);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_UDPBD, gEnableUDPBD);
        configSetInt(configOPL, CONFIG_OPL_NET_BOOT_PROTOCOL, gNetBootProtocol);
        // Dual-write: the authoritative unified selector PLUS the three legacy keys (derived shadows),
        // so a config saved by this build still boots correctly on an older OPL that only reads the legacy keys.
        configSetInt(configOPL, CONFIG_OPL_NETWORK_PROTOCOL, gNetworkProtocol);
        configSetInt(configOPL, CONFIG_OPL_NET_START_MODE, gNetStartMode);
        configSetInt(configOPL, CONFIG_OPL_SMB_DIALECT, gSMBDialect);
        configSetInt(configOPL, CONFIG_OPL_SFX, gEnableSFX);
        configSetInt(configOPL, CONFIG_OPL_BOOT_SND, gEnableBootSND);
        configSetInt(configOPL, CONFIG_OPL_BGM, gEnableBGM);
        configSetInt(configOPL, CONFIG_OPL_SFX_VOLUME, gSFXVolume);
        configSetInt(configOPL, CONFIG_OPL_BOOT_SND_VOLUME, gBootSndVolume);
        configSetInt(configOPL, CONFIG_OPL_BGM_VOLUME, gBGMVolume);
        configSetStr(configOPL, CONFIG_OPL_DEFAULT_BGM_PATH, gDefaultBGMPath);
        configSetStr(configOPL, CONFIG_OPL_NEUTRINO_ARGS, gNeutrinoArgs);
        configSetStr(configOPL, CONFIG_OPL_NEUTRINO_PATH, gNeutrinoPath);
        configSetInt(configOPL, CONFIG_OPL_DEFAULT_CORE, gDefaultCoreLoader); // global default Loader Core (0=<OPL>, 1=Neutrino)
        configSetInt(configOPL, CONFIG_OPL_NEUTRINO_VIDEO, gNeutrinoVideoDefault);
        configSetInt(configOPL, CONFIG_OPL_NEUTRINO_GSMCOMP, gNeutrinoGsmCompDefault);
        configSetInt(configOPL, CONFIG_OPL_NEUTRINO_DEVTYPE, gNeutrinoDevice); // device-TYPE (NEUTRINO_DEV_*); the legacy neutrino_device key is left as-is
        configSetInt(configOPL, CONFIG_OPL_NEUTRINO_ELF_ARG, gNeutrinoElfArg);
        configSetStr(configOPL, CONFIG_OPL_POPSTARTER_PATH, gPopstarterPath);
        configSetInt(configOPL, CONFIG_OPL_POPSTARTER_DEVICE, gPopstarterDevice);
        configSetInt(configOPL, CONFIG_OPL_POPSTARTER_RETROGEM_GAMEID, gPopstarterRetroGemGameID);

        configSetInt(configOPL, CONFIG_OPL_BDMA_SOURCE, gBdmaSource);
        configSetInt(configOPL, CONFIG_OPL_BDMA_APPLY, gBdmaApplyOnLaunch);
        configSetInt(configOPL, CONFIG_OPL_VCD_HIDE_GAMEID, gVcdHideGameId);
        configSetInt(configOPL, CONFIG_OPL_VCD_FIRST_DISC_ONLY, gVcdFirstDiscOnly);
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

    // Create/refresh the legacy mc?:OPL home ONLY in legacy-discovery mode (no known boot dir). The
    // "mc" prefix test alone cannot tell the legacy mc?:OPL config root from an APPDIR THAT LIVES ON
    // A MEMORY CARD (FMCB-style mc0:/APPS install: configGetDir() truncates either to "mc0:"), so an
    // appdir-on-MC boot used to sprout an unwanted mc?:/OPL folder (+ icons) on every settings/theme/
    // last-played save while the .cfg files themselves correctly stayed in the appdir -- and the
    // configPrepareNotifications(gBaseMCDir) call re-pointed the "Settings saved to %s" toast at the
    // wildcard card instead of the real appdir. gBootDir is the authoritative gate, same as the
    // alternate-save logic below: non-empty means the appdir IS the config root, MC stays untouched.
    char *path = configGetDir();
    if (gBootDir[0] == '\0' && !strncmp(path, "mc", 2)) {
        checkMCFolder();
        configPrepareNotifications(gBaseMCDir);
    }

    lscret = configWriteChecked(lscstatus);
    // Boot-device save retry: BDM slot numbering can change between the boot-time resolve and this
    // save (a hotplug add/remove renumbers massN:). Re-resolve against the SAME boot device once --
    // pinned to its known BDM type -- and retry. Never a different device: the cross-device fallback
    // was explicitly rejected (PR #59); if the boot device is truly gone the save fails visibly.
    if (lscret <= 0 && gBootDir[0] != '\0' && gBootDirBdmType != BDM_TYPE_UNKNOWN) {
        char before[sizeof(gBootDir)];
        snprintf(before, sizeof(before), "%s", gBootDir);
        if (bdmResolveBootDir(gBootDir, sizeof(gBootDir), gBootElfName, &gBootDirBdmType) > 0 &&
            strcmp(before, gBootDir) != 0) {
            LOG("BOOT re-resolved boot dir for save: %s -> %s\n", before, gBootDir);
            configSetMove(gBootDir); // keep the pending values; re-point the files to the new slot
            lscret = configWriteChecked(lscstatus);
        }
    }
    // The boot dir (cwd) is the only save target. The alternate-device save + the cwd redirect
    // pointer are legacy-discovery aids, kept only for the boot-path-undeterminable sanity case
    // (gBootDir empty); with a known boot dir, settings save there and nowhere else.
    if (gBootDir[0] == '\0') {
        if (lscret <= 0)
            lscret = trySaveAlternateDevice(lscstatus);
        if (lscret > 0)
            writeConfigPathRedirect(configGetDir());
    }
    // gLastSaveErrno is latched at the actual write-failure site inside configWrite()
    // (config.c) -- NOT here, where later config-set snapshot-opens have already clobbered errno with a
    // spurious ENOENT (adversarial review). See config.c.
    lscstatus = 0;
}

void applyConfig(int themeID, int langID, int skipDeviceRefresh)
{
    // A deliberate settings/theme apply may make new art available, so clear the
    // genuine-absence memo and tar inactive latch to let cover art be probed once more.
    cacheInvalidateFailMemo();
    tarInvalidate(TAR_KIND_ART);

    if (gDefaultDevice < 0 || gDefaultDevice > FAV_MODE)
        gDefaultDevice = MMCE_MODE;
    // Favourites (issue #54) is a valid startup page only while the FAV tab is enabled; otherwise fall
    // back so a stale "start on Favourites" choice can't boot into a hidden/empty tab.
    if (gDefaultDevice == FAV_MODE && !gFAVStartMode)
        gDefaultDevice = MMCE_MODE;

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
            // arrays while the GUI thread may be drawing through them (test note #2).
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
    // Sized for the worst message this function can now build: the failure text carries a full config
    // home path (configGetDir -> cfgDevice, itself up to a 256-byte prefix) plus an errno, and 128
    // truncated exactly the part that made the message worth showing (Gemini review of #187).
    char notification[320];
    lscstatus = types;
    lscret = 0;

    guiHandleDeferedIO(&lscstatus, _l(_STR_SAVING_SETTINGS), IO_CUSTOM_SIMPLEACTION, &_saveConfig, OPL_DEFERRED_IO_TIMEOUT_MS);

    if (showUI) {
        char *path = configGetDir();

        if (lscret) {
            snprintf(notification, sizeof(notification), _l(_STR_SETTINGS_SAVED), path);

            guiMsgBox(notification, 0, NULL);
        } else if (strchr(path, '?') != NULL) {
            // The '?' only survives configGetDir() when we fell back to the "mc?:OPL" home AND
            // checkMC() found no card to substitute -- i.e. there is nowhere to save at all, which is
            // a standing condition, not this one write failing. Explain that instead of an errno.
            guiMsgBox(_l(_STR_SETTINGS_NO_HOME), 0, NULL);
        } else {
            // Say WHERE and WHY. A bare "Error writing settings!" cost a maintainer an afternoon on a
            // network boot: the write was failing against a home he had no way to see, and the errno
            // was already sitting in gLastSaveErrno (latched at the real failure site in
            // config.c) with nothing putting it on screen.
            snprintf(notification, sizeof(notification), _l(_STR_ERROR_SAVING_SETTINGS_TO), path, gLastSaveErrno);

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
     * successfully-started server.
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
    deinitAllSupport(NO_EXCEPTION, IO_MODE_SELECTED_ALL, -1);
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
    // Release the live server stack before resetting the IOP.
    shutdownLwnbdNetwork();
    unloadPads();

    reset();

    LOG_INIT();
    LOG_ENABLE();

    // reinit the input pads
    padInit(0);

    /* Bound pad startup so a half-completed IOP reset cannot leave the EE in an
     * infinite loop on the "NBD Server unloading..." screen. */
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

    // Only a path that actually dismantled the menu needs the reset-and-restore cycle.
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
    sysReset();

    mcInit(MC_TYPE_XMC);
}

static void moduleCleanup(opl_io_module_t *mod, int exception, int modeSelected, int modeSelected2)
{
    if (!mod->support)
        return;

    // Shutdown if not required anymore.
    if ((mod->support->mode != modeSelected) && (mod->support->mode != modeSelected2) && (modeSelected != IO_MODE_SELECTED_ALL)) {
        if (mod->support->itemShutdown)
            mod->support->itemShutdown(mod->support);
    } else {
        if (mod->support->itemCleanUp)
            mod->support->itemCleanUp(mod->support, exception);
    }

    clearMenuGameList(mod);
}

// Max delay(1) ticks (~ms) to drain the IO worker during exit/poweroff teardown
// before proceeding anyway; bounded because the IOP is reset/powered off right after.
#define EXIT_IO_DRAIN_TICKS   1000
// Δ7: launch-path drain bound (~10 s of delay(1) ticks). Generous -- a healthy slow device drains in
// well under this -- but finite, so one wedged art read on a dying device can no longer freeze the
// loading screen forever (the handoff targets IOP-reset themselves; a straggler cannot outlive that).
#define LAUNCH_IO_DRAIN_TICKS 10000

// 1 while deinit() tears down for exit/poweroff, 0 for a game/app launch. Consumed by device shutdowns
// that must behave differently on the launch path (hddShutdown keeps DEV9 powered so the post-deinit
// POPSTARTER.ELF read from the ATA-backed massN: mount still works; ee_core/POPSTARTER reset the IOP
// themselves, so skipping the power-off on launches leaks nothing).
int gDeinitTerminal = 0;

void deinit(int exception, int modeSelected)
{
    deinitEx(exception, modeSelected, -1);
}

void deinitEx(int exception, int modeSelected, int modeSelected2)
{
    gDeinitTerminal = (modeSelected == IO_MODE_SELECTED_ALL || modeSelected == IO_MODE_SELECTED_NONE);

    /* Menu rumble (#172): kill the actuators FIRST, before anything below can block. Every launch and
     * exit path funnels through here, and closing the pad ports later does NOT clear the motors -- so a
     * tap still in flight would buzz on forever, straight into the game. This must stay at the TOP: the
     * IO drain below can take up to LAUNCH_IO_DRAIN_TICKS (10s) on a slow or dying device, and the pad
     * would grind through that entire loading screen if the stop lived down beside unloadPads(). */
    padRumbleStopAll();

    /* Cut launch/exit latency by stopping queued art I/O before globally
     * blocking the I/O worker. This avoids waiting for stale cover requests
     * that are no longer needed once we are deinitializing. */
    cacheAbortMmceImageLoadsTimed(0);
    (void)cacheCancelPendingImageLoadsTimed(0);

    // block all io ops, wait for the ones still running to finish.
    // For the terminal teardown modes (exit = IO_MODE_SELECTED_ALL,
    // poweroff = IO_MODE_SELECTED_NONE) cap the drain: those paths reset the IOP
    // (LoadExecPS2) or power the machine off immediately afterward, so a request
    // stuck on a removed/slow device must not hang teardown forever.
    // Δ7 (NHDDL parity): the LAUNCH path is now bounded too (longer budget). The old unbounded
    // wait "so its IOP state stays clean" predates the keep-IOP handoff work: post-#79 the two
    // excepted mounts stay alive regardless, and Neutrino/POPSTARTER perform their OWN IOP reset
    // after reading their files -- a straggler request cannot corrupt what gets rebuilt fresh.
    // Unbounded, it turned one wedged cover read on a dying device into a permanently frozen
    // loading screen BEFORE the handoff even started. NHDDL has no IO worker to drain at all.
    int terminalTeardown = gDeinitTerminal;
    if (terminalTeardown) {
        ioBlockOpsTimed(1, EXIT_IO_DRAIN_TICKS);
    } else {
        ioBlockOpsTimed(1, LAUNCH_IO_DRAIN_TICKS); // always returns IO_OK -- timeout shows as leftover requests
        if (ioHasPendingRequests())
            LOG("deinit: launch IO drain timed out after %d ticks -- proceeding (straggler cannot survive the target's IOP reset)\n", LAUNCH_IO_DRAIN_TICKS);
    }
    guiExecDeferredOps();
    cacheEnd(terminalTeardown);

#ifdef PADEMU
    ds34usb_reset();
    ds34bt_reset();
#endif
    unloadPads();

    deinitAllSupport(exception, modeSelected, modeSelected2);

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
    // plasma blend low end: BLACK preserves the historical plasma exactly (see guiDrawBGPlasma)
    gDefaultPlasBlendColor[0] = 0x00;
    gDefaultPlasBlendColor[1] = 0x00;
    gDefaultPlasBlendColor[2] = 0x00;
    // The fork's navy <OPL> reskin lives HERE, not in the embedded conf_theme_OPL.cfg: theme-cfg
    // color keys OVERRIDE the user's picked colors on every thmLoad(NULL) reload (boot vmode change,
    // language change, theme-device unplug), while thmSetColors re-applies the user picks on other
    // paths -- so cfg-embedded colors made the look flip-flop and user picks not stick (tester
    // report: "changing settings makes it default look"). As defaults they seed the same navy
    // scheme, survive every reload once the user re-picks, and "Reset Colors" restores the fork look.
    gDefaultBgColor[0] = 0x18;
    gDefaultBgColor[1] = 0x25;
    gDefaultBgColor[2] = 0x80;

    gDefaultTextColor[0] = 0x32;
    gDefaultTextColor[1] = 0x4d;
    gDefaultTextColor[2] = 0x9e;

    gDefaultSelTextColor[0] = 0xFF;
    gDefaultSelTextColor[1] = 0xFF;
    gDefaultSelTextColor[2] = 0xFF;

    gDefaultUITextColor[0] = 0x00;
    gDefaultUITextColor[1] = 0xFF;
    gDefaultUITextColor[2] = 0xFF;
}

static void setDefaults(void)
{
    for (int i = 0; i < MODE_COUNT; i++)
        clearIOModuleT(&list_support[i]);

    gAutoLaunchGame = NULL;
    gAutoLaunchBDMGame = NULL;
    gAutoLaunchDeviceData = NULL;
    gOPLPart[0] = '\0';
    gHDDPrefix = "pfs0:";
    gBaseMCDir = "mc?:OPL";

    bdmCacheSize = 16;
    hddCacheSize = 8;
    smbCacheSize = 16;

    // RiptOPL network defaults (2026-07-13, Nathan's reference config): static addressing on the
    // common 192.168.1.x home subnet with a ready-to-edit SMB target, so a first-time user sees a
    // working-shaped setup instead of blanks. DHCP stays available one toggle away.
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
    gPCPort = 1111; // RiptOPL default SMB port (was 445): non-privileged and freely editable through the always-visible advanced network options; match it to the PC server's displayed port
    strcpy(gPCShareName, "games");
    strcpy(gPCUserName, "guest");
    gPCPassword[0] = '\0';
    gNetworkStartup = ERROR_ETH_NOT_STARTED;
    gHDDSpindown = 20;
    gScrollSpeed = 1;
    gExitPath[0] = '\0';
    gNeutrinoArgs[0] = '\0';
    gNeutrinoPath[0] = '\0';
    gNeutrinoDevice = NEUTRINO_DEV_AUTO;
    gDefaultCoreLoader = 0;      // <OPL> (native) -- preserves pre-existing behaviour until the user opts into Neutrino globally
    gNeutrinoVideoDefault = 0;   // no global -gsm until the user opts in
    gNeutrinoGsmCompDefault = 0; // no field-flip comp half
    gNeutrinoElfArg = 1;         // auto-emit the game ELF for Neutrino compatibility lookup by default
    gPopstarterPath[0] = '\0';
    gPopstarterDevice = POPS_DEV_DEFAULT;
    gPopstarterRetroGemGameID = 1;

    gBdmaSource = VCD_BDMA_SRC_USB;
    gBdmaMode = VCD_BDMA_FAT32;
    gBdmaApplyOnLaunch = 1; // auto-equip on launch by default (the MX4SIO->OSDSYS fix)
    gVcdHideGameId = 1;     // hide the PS1 game-ID prefix by default (display-only; raw names one toggle away)
    gVcdFirstDiscOnly = 1;  // #118: hide discs 2+ of multi-disc PS1 sets by default (POPSLoader parity)
    gDefaultDevice = APP_MODE;
    gAutosort = 1;
    gAutoRefresh = 0;
    gEnableDebug = 0;
    gPS2Logo = 1;
    gHDDGameListCache = 0;
    gEnableWrite = 1;
    gRememberLastPlayed = 0;
    gEnableFolderNav = 0; // opt-in; a flat library is byte-identical to before
    gEnableRumble = 0;    // opt-in: nobody expects a menu to buzz, and these motors are 20+ years old
    gAutoStartLastPlayed = 9;
    gSelectButton = KEY_CROSS; // Default to Cross-select (western layout); swap_select_btn=0 restores Circle
    gMMCEPrefix[0] = '\0';
    gBDMPrefix[0] = '\0';
    gETHPrefix[0] = '\0';
    gEnableNotifications = 1;
    gEnableArt = 1;
    gEnableBGArt = 1;
    // .tar cover-art loader is OPT-IN (default OFF) -- the fork's deliberate opinionated default (#54).
    // The reporter's "art.tar doesn't work" was DISCOVERABILITY, not the default: the toggle lives in
    // Display Settings ("Cover Art .tar Archive"). PR #207 briefly flipped this ON for wOPL parity;
    // reverted at NathanNeurotic's call -- users who want the archive turn it on. The engine fixes from
    // #207 (uncapped-seek index integrity, no stat()-latch, toggle re-arm, the [48] filename bound) stay.
    gEnableArtTar = 0;
    gEnableAnalogNav = 1;
    gArtDelay = 2;
    gWideScreen = 1;
    gDefaultGameView = GAME_VIEW_BOTH;
    gEnableSFX = 1;
    gEnableBootSND = 1;
    gEnableBGM = 1;
    gSFXVolume = 80;
    gBootSndVolume = 80;
    gBGMVolume = 70;
    gDefaultBGMPath[0] = '\0';
    gXSensitivity = 1;
    gYSensitivity = 1;

    // RiptOPL doctrine (NathanNeurotic, 2026-07-20): EVERY device on the Devices page ships OFF -- the
    // user opts in to exactly what their rig has. A fresh install boots to the start menu with no tabs
    // (deferredInit's boot select handles the nothing-registered case explicitly) and the user enables
    // devices in Settings. Existing saved configs override all of these on load.
    gBDMStartMode = START_MODE_DISABLED;
    gHDDStartMode = START_MODE_DISABLED;
    gETHStartMode = START_MODE_DISABLED;
    gAPPStartMode = START_MODE_DISABLED;
    gMMCEStartMode = START_MODE_DISABLED;
    gFAVStartMode = START_MODE_DISABLED;

    gMMCESlot = 2; // Default to first Auto slot
    gMMCEIGRSlot = 3;
    gMMCEEnableGameID = 0;
    gApplyGameID = 0;
    // Restore the fork's long-standing known-good MMCE SIO2 pacing (was flipped to 0/0 in 519f520d,
    // mislabeled "safer" -- 0 cycles + alarms OFF is the aggressive/perf extreme the in-app hints warn
    // about: lower cycles = "instabilities", alarms OFF = "can cause MMCE timeouts to result in freezes").
    // 5 + alarms ON is what build 2421 shipped and ran cleanly (incl. FMV/audio) on real hardware; a slow
    // late-slim SD2PSX loses the very first SIO2 handshake at 0/0 and freezes at the first read. Opinionated
    // safe default per the fork philosophy -- do NOT re-flip to upstream's 0/0.
    gMMCEAckWaitCycles = 5;
    gMMCEUseAlarms = 1;

    gEnableUSB = 0; // all block-device toggles OFF by default too (same opt-in doctrine)
    gEnableILK = 0;
    gEnableMX4SIO = 0;
    gEnableBdmHDD = 0;                 // exFAT BDM HDD OFF by default (the other "HDD type"; APA/PFS is gHDDStartMode above)
    gEnableUDPBD = 0;                  // the UDPBD BLOCK device stays opt-in
    gNetBootProtocol = NET_BOOT_UDPBD; // default transport when network boot is enabled (back-compat)
    // Unified network selector defaults to OFF (was UDPFS, Nathan 2026-07-16). The reason is the NIC
    // latch: every network stack loads its IOP chain ONCE per boot and never unloads (re-binding the
    // UDPRDMA socket bricks UDPFS; smap registers a single SMAP_driver), so whichever protocol is
    // active FIRST owns the adapter until a restart -- the settings page even tells you so
    // (NETBOOT_RESTART). With UDPFS pre-selected, a user who wanted UDPBD or SMB had to change the
    // setting and REBOOT before their choice could load. Defaulting to Off means nothing claims the
    // NIC at boot, so the first protocol the user picks in Device Settings comes up live -- the apply
    // path re-derives the gEnableUDPBD/gNetBootProtocol shadows and forces a device refresh already.
    // Existing installs are unaffected: a saved net protocol in settings_riptopl.cfg overrides this.
    gNetworkProtocol = NET_PROTO_OFF;
    gNetStartMode = START_MODE_DISABLED; // Off in the 3-row Network setting; migration reconciles old configs
    gSMBDialect = SMB_DIALECT_SMB1;      // SMBv1 unless the user opts into SMB2; never auto-migrated

    frameCounter = 0;

    gVMode = 0;
    gXOff = 0;
    gYOff = 0;
    // Overscan compensation defaults OFF (was 100 = a 5%-per-side inset; Nathan 2026-07-16, from
    // FifthFox's HW report). The inset shifts ALL rendering inward, but scrolled dialog text was not
    // clipped to the inset viewport, so rows bled outside the background ("scrolled text moves out of
    // the background image"). Modern displays don't need the inset; anyone on a CRT that does can
    // still raise it in Display Settings. Saved configs override this, as with every default.
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

    // MMCE GameID arming, boot leg: posted HERE -- after GUI_INIT_DONE is on its way to the GUI
    // thread -- and not from initAllSupport, so the blocking, untimeoutable mmceman.irx load can never
    // hold the menu hostage again (GZAst's boot froze forever on "Arming MMCE game-ID..."). This
    // appends the arm after this handler in the IO FIFO: worst case a wedged probe degrades the IO
    // worker POST-boot (visible, diagnosable, menu alive) instead of killing the boot. The
    // settings-apply leg still arms immediately from initAllSupport.
    /*
      DELIBERATELY NOT gated on gMMCEStartMode (#254 / maintainer directive 2026-07-27).

      Arming here on every boot -- regardless of whether the MMCE device is enabled -- is the #51
      contract: GameID works without the MMCE page ever being enabled, so a cross-device launch can
      still push the game id to a card. Do not add a device-enabled condition; it was tried and
      reverted on that basis.

      The #254 dead-cursor report (mmceman's sio2man hook killing pad input on the ps2max/ghcr
      freepad build) must therefore be fixed BUILD-SIDE, by shipping a freepad that survives the
      hook -- see .github/scripts/install_coherent_freepad.sh -- not by narrowing this.
    */
    if (gMMCEEnableGameID)
        ioPutRequest(IO_CUSTOM_SIMPLEACTION, &mmceArmGameIDTransport);

    // Nad #6: never silently SKIP the boot select -- doing so left the GUI on the start-menu screen
    // with the first-appended tab (a BDM instance, typically MX4SIO) as the implicit selection. If
    // the configured Default Menu's mode is not registered, fall back MMCE -> APP -> the first
    // registered mode (mirroring applyConfig's clamp); only when NOTHING is registered is there no
    // main screen worth selecting and the start menu stays.
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
    }

    InitConsoleRegionData();

    ret = configReadMulti(CONFIG_ALL);
    // Fall back to the device's own config home when the cwd read found nothing -- the fork dropped
    // upstream's fallback here and only the BDM leg got a replacement (resolveBootDirToMass above). So an
    // argv "mini" launch on an APA HDD read NO config at all and ran on pure DEFAULTS for gPS2Logo,
    // gExitPath, gHDDSpindown, hddCacheSize and every Neutrino global -- with pfs0: already mounted a few
    // lines up and the config sitting right there, unread. (Reachable only from an external launcher:
    // HDD-OSD / PSBBN / a direct-to-game shortcut. If argv[0] carried no usable path, gBootDir is empty,
    // configInit homed at mc?:OPL, and an APA-only setup misses for the same reason.)
    if (!(ret & CONFIG_OPL)) {
        if (mode == HDD_MODE)
            ret = checkLoadConfigHDD(CONFIG_ALL);
        else if (mode == BDM_MODE)
            ret = checkLoadConfigBDM(CONFIG_ALL);
    }
    if (CONFIG_ALL & CONFIG_OPL) {
        if (ret & CONFIG_OPL) {
            config_set_t *configOPL = configGetByType(CONFIG_OPL);

            configGetInt(configOPL, CONFIG_OPL_PS2LOGO, &gPS2Logo);
            configGetStrCopy(configOPL, CONFIG_OPL_EXIT_PATH, gExitPath, sizeof(gExitPath));
            configGetInt(configOPL, CONFIG_OPL_HDD_SPINDOWN, &gHDDSpindown);
            // Honor ALL the Neutrino-launch globals on the autolaunch/argv path exactly like the
            // interactive _loadConfig -- not just the default core: an autolaunched keyless "Default"
            // game must resolve the SAME neutrino.elf (device pick / custom path) with the SAME global
            // args as an interactive launch, or it silently boots a different/stale core without flags.
            configReadNeutrinoGlobals(configOPL);
            if (mode == BDM_MODE) {
                configGetStrCopy(configOPL, CONFIG_OPL_BDM_PREFIX, gBDMPrefix, sizeof(gBDMPrefix));
                configGetInt(configOPL, CONFIG_OPL_BDM_CACHE, &bdmCacheSize);
            } else if (mode == HDD_MODE) {
                configGetInt(configOPL, CONFIG_OPL_HDD_CACHE, &hddCacheSize);
            } else if (mode == MMCE_MODE) {
                configGetStrCopy(configOPL, CONFIG_OPL_MMCE_PREFIX, gMMCEPrefix, sizeof(gMMCEPrefix));
            }
        }
    }
}

void miniDeinit(config_set_t *configSet)
{
    ioBlockOps(1);
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
    config_set_t *configSet;

    miniInit(HDD_MODE);

    gAutoLaunchGame = malloc(sizeof(hdl_game_info_t));
    if (gAutoLaunchGame == NULL) {
        miniDeinit(NULL);
        return;
    }
    memset(gAutoLaunchGame, 0, sizeof(hdl_game_info_t));

    snprintf(gAutoLaunchGame->startup, sizeof(gAutoLaunchGame->startup), "%s", argv[1]);
    gAutoLaunchGame->start_sector = strtoul(argv[2], NULL, 0);
    snprintf(gOPLPart, sizeof(gOPLPart), "hdd0:%s", argv[3]);

    snprintf(path, sizeof(path), "%sCFG/%s.cfg", gHDDPrefix, gAutoLaunchGame->startup);
    configSet = configAlloc(0, NULL, path);
    configRead(configSet);

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

    int nameLen = 0;
    int format = isValidIsoName(argv[1], &nameLen);
    // Reject unsupported / over-long filenames -- matches supportbase.c:315 scanner pattern.
    // Using a clamped nameLen as the extension offset would desync from the real suffix position.
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
    gAutoLaunchDeviceData->bdmDeviceType = BDM_TYPE_UNKNOWN;
    gAutoLaunchDeviceData->bdmHddIsLBA48 = -1;
    gAutoLaunchDeviceData->ataHighestUDMAMode = -1;

    char apaDevicePrefix[BDM_DEVICE_ROOT_MAX] = {0};
    int selectedMassSlot = -1;
    delay(8);
    // Loop through mass0: to mass4:
    for (int i = 0; i <= 4; i++) {
        snprintf(path, sizeof(path), "mass%d:/", i);
        int dir = fileXioDopen(path);

        if (dir >= 0) {
            char detectedDriver[sizeof(gAutoLaunchDeviceData->bdmDriver)] = {0};
            int detectedDeviceIndex = -1;

            fileXioIoctl2(dir, USBMASS_IOCTL_GET_DRIVERNAME, NULL, 0, detectedDriver, sizeof(detectedDriver) - 1);
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

    if (selectedMassSlot < 0)
        snprintf(apaDevicePrefix, sizeof(apaDevicePrefix), "mass0:");

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
int main(int argc, char *argv[])
{
#ifdef __DECI2_DEBUG
    sysInitDECI2();
#endif

    LOG_INIT();
    PREINIT_LOG("OPL GUI start!\n");

    ChangeThreadPriority(GetThreadId(), 31);

    // Hide the leftover framebuffer left by the BIOS/launcher (garbage window W1).
    // These GS privileged registers are WRITE-ONLY on real hardware, so we only
    // ever WRITE them (never read-back): writing BGCOLOR=0 forces a black border
    // and writing PMODE=0 disables the display read circuits (EN1/EN2=0), so the
    // GS stops fetching the stale framebuffer across reset()/IOP reload. This is
    // the first GS access, so nothing is shown until rmInit() brings the display
    // back up over a cleared framebuffer.
    //
    // Re-enabling the display is delegated entirely to gsKit_init_screen() in
    // rmSetMode(), which performs a GS reset and writes its own complete PMODE
    // (computed from gsGlobal, never a read-back). We therefore never save or
    // restore a PMODE/BGCOLOR value of our own. NOTE: the existing (working) boot
    // already relied on gsKit_init_screen() being the sole thing that turns the
    // display on (no other PMODE write existed and OPL boots), so writing PMODE=0
    // here adds no new dependency; it only blanks the stale W1 framebuffer first.
    // The GS reset inside gsKit_init_screen() also zeroes BGCOLOR again, so the
    // BGCOLOR=0 write only matters for the brief pre-reset() W1 span. The matching
    // blank for the rmInit window (W2) lives in renderman.c rmSetMode().
    *(volatile u64 *)0x120000E0 = 0; // GS BGCOLOR = black
    *(volatile u64 *)0x12000000 = 0; // GS PMODE   = display read circuits off

    // reset, load modules
    reset();
    ResetDeckardXParams();

    // Settings live in the boot directory (cwd). Resolve it once, before any config init -- the
    // autolaunch path below calls miniInit() -> configInit(). argv[0] is the launcher's boot path;
    // getcwd() backs it up. Empty only if both are unusable, in which case configInit falls back to
    // the memory-card default and the _loadConfig/_saveConfig gates re-enable legacy discovery.
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

    if (argc >= 5) {
        /* argv[0] boot path
           argv[1] game->startup
           argv[2] str to u32 game->start_sector
           argv[3] opl partition read from hdd0:__common/OPL/conf_hdd.cfg
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
    // The '?' only survives configGetDir() when the discovery chain fell all the way back to the
    // "mc?:OPL" default AND checkMC() found no card to substitute into it. That is precisely the
    // network-boot case: the config must be read before any network stack exists, so a UDPFS/UDPBD/SMB
    // boot can never resolve its own boot device at load time (bdmResolveBootDir returns -1 for a
    // network block device by design), the chain falls through MC -> MMCE -> BDM -> BDM-HDD, and a rig
    // with no local card has nowhere to put settings. Reported as "settings don't save - but they
    // claim to load", which is exactly what silence looks like from the outside.
    //
    // A toast, not a modal: this is a standing property of the setup, not an error the user just
    // caused, and it must not gate a boot that otherwise works fine.
    if (strchr(configGetDir(), '?') != NULL)
        guiWarning(_l(_STR_SETTINGS_NO_HOME), 6);

    // Menu rumble goes live ONLY now: the boot is done and guiMainLoop below starts polling pads.
    // Before this point guiIntroLoop runs handleInput() against a FROZEN paddata snapshot, so a button
    // held at power-on fires sfxPlay every frame -- which was a harmless no-op until rumble hooked
    // above sfxPlay's audio gate and turned it into blocking libpad RPCs racing the IO worker's IOP
    // module loads (intermittent boot hang at "Loading USB storage driver...", #172).
    padRumbleActivate();

    // Menu rumble: "OPL is ready" tap. Armed HERE rather than off sfxPlay(SFX_BOOT) for two reasons.
    // (1) Correctness: SFX_BOOT plays from inside guiIntroLoop(), whose loop never polls readPads(),
    // so the decay countdown would be frozen for the whole intro -- seconds of buzz instead of a tap.
    // Out here guiMainLoop() is about to start ticking it. (2) Meaning: this is the instant the menu
    // is actually usable, which is what "ready" means to the user -- and it does not depend on the
    // boot SOUND being enabled. No-op when rumble is off or the pad can't do it.
    padRumbleBump();

    guiMainLoop();

    return 0;
}
