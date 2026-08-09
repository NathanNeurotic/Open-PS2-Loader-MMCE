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
static clock_t lastBgRescan[MODE_COUNT];
static unsigned int lastSeenBdmGeneration;

static char errorMessage[256];

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
int gVcdHideGameId;                // display-only: hide a leading PS1 game-ID prefix from VCD lists
int gVcdFirstDiscOnly;             // hide discs 2+ of multi-disc PS1 sets
char gBootDir[256];                // boot directory (cwd) OPL launched from; "" if undeterminable
int gEnableILK;
int gEnableBGArt;
int gEnableArtTar;                       // .tar art packs (item 45); no UI until gate D
int gArtDelay;                           // inactivity frames before art loads; safe default until gate D tunes it
int gEnableFolderNav;                    // folder browsing in game lists (item 34)
unsigned char gDefaultPlasBlendColor[3]; // plasma gradient low end; black = historical look
volatile int gLastSaveErrno = 0;
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
int gEnableDebug;
int gPS2Logo;
int gDefaultDevice;
int gEnableWrite;
char gBDMPrefix[32];
char gETHPrefix[32];
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
        if (mod->subMenu)
            submenuRebuildCache(mod->subMenu);
        guiCheckNotifications(themeChanged, 0);
    }
}

static void itemInitSupport(item_list_t *support)
{
    support->itemInit(support);
    moduleUpdateMenuInternal((opl_io_module_t *)support->owner, 0, 0);
    // Manual refreshing can only be done if either auto refresh is disabled or auto refresh is disabled for the item.
    if (!gAutoRefresh || (support->updateDelay == MENU_UPD_DELAY_NOUPDATE))
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
                config_set_t *configSet = menuLoadConfig();
                // Flash the GameID barcode (Pixel FX/RetroGEM HDMI auto-profile) before handoff; this
                // single menu chokepoint covers both the Neutrino and OPL-native cores. No-op when off.
                guiShowGameID(support->itemGetStartup(support, curMenu->current->item.id));
                support->itemLaunch(support, curMenu->current->item.id, configSet);
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
        ioPutRequest(IO_MENU_UPDATE_DEFFERED, &support->mode);
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
        if (support == NULL || !vcdViewActive(support->mode))
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
        if (!(support->flags & MODE_FLAG_NO_COMPAT)) {
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
        gup->menu.menu = &mdl->menuItem;
        gup->menu.subMenu = &mdl->subMenu;
        guiDeferUpdate(gup);
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
        if (mod->support->mode != APP_MODE)
            shouldAppsUpdate = 1;
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

    // NOTE(rebuild): the fork also yields to pending cover-art work here (cacheHasPendingArt);
    // that gate returns with the art-cache rework (item 45).

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
    int value;
    DIR *dir;

    getcwd(pwd, sizeof(pwd));

    // First, try the device that OPL booted from.
    if (!strncmp(pwd, "mass", 4) && (pwd[4] == ':' || pwd[5] == ':')) {
        if ((value = checkLoadConfigBDM(types)) != 0)
            return value;
    } else if (!strncmp(pwd, "hdd", 3) && (pwd[3] == ':' || pwd[4] == ':')) {
        if ((value = checkLoadConfigHDD(types)) != 0)
            return value;
    }

    // Config was not found on the boot device. Check all supported devices.
    //  Check USB device
    if ((value = checkLoadConfigBDM(types)) != 0)
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

static void _loadConfig()
{
    int value, themeID = -1, langID = -1;
    const char *temp;
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
            configGetInt(configOPL, CONFIG_OPL_ETH_MODE, &gETHStartMode);
            configGetInt(configOPL, CONFIG_OPL_APP_MODE, &gAPPStartMode);
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
            configGetInt(configOPL, CONFIG_OPL_VCD_HIDE_GAMEID, &gVcdHideGameId);
            configGetInt(configOPL, CONFIG_OPL_VCD_FIRST_DISC_ONLY, &gVcdFirstDiscOnly);
            configReadNeutrinoGlobals(configOPL); // shared with miniInit's autolaunch path
            configGetInt(configOPL, CONFIG_OPL_ENABLE_BGART, &gEnableBGArt);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_ART_TAR, &gEnableArtTar);
            configGetInt(configOPL, CONFIG_OPL_ART_DELAY, &gArtDelay);
            // Keep the stored domain identical to the Artwork page's enum {0,2,5,8} (item 45), so a
            // hand-edited or legacy value cannot render as a delay the UI is unable to express.
            // Fork parity except the fallback: the fork lands on 2, we land on 8 (see setDefaults).
            if (gArtDelay != 0 && gArtDelay != 2 && gArtDelay != 5 && gArtDelay != 8)
                gArtDelay = 8;
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
            // Reconcile the two persisted halves; a hand-edited/stale config can disagree either way:
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
    showCfgPopup = 1;
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
    // Check that the formatted & usable HDD is connected.
    if (hddCheck() == 0) {
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
    char pwd[8];
    int value;

    getcwd(pwd, sizeof(pwd));

    // First, try the device that OPL booted from.
    if (!strncmp(pwd, "mass", 4) && (pwd[4] == ':' || pwd[5] == ':')) {
        if ((value = trySaveConfigBDM(types)) > 0)
            return value;
    } else if (!strncmp(pwd, "hdd", 3) && (pwd[3] == ':' || pwd[4] == ':')) {
        if ((value = trySaveConfigHDD(types)) > 0)
            return value;
    }

    // Config was not saved to the boot device. Try all supported devices.
    // Try memory cards
    if (sysCheckMC() >= 0) {
        if ((value = trySaveConfigMC(types)) > 0)
            return value;
    }
    // Try a USB device
    if ((value = trySaveConfigBDM(types)) > 0)
        return value;
    // Try the HDD
    if ((value = trySaveConfigHDD(types)) > 0)
        return value;

    // We tried everything, but...
    return 0;
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
        configSetInt(configOPL, CONFIG_OPL_VCD_HIDE_GAMEID, gVcdHideGameId);
        configSetInt(configOPL, CONFIG_OPL_VCD_FIRST_DISC_ONLY, gVcdFirstDiscOnly);
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

    char *path = configGetDir();
    if (!strncmp(path, "mc", 2)) {
        checkMCFolder();
        configPrepareNotifications(gBaseMCDir);
    }

    lscret = configWriteMulti(lscstatus);
    if (lscret == 0)
        lscret = trySaveAlternateDevice(lscstatus);
    lscstatus = 0;
}

void applyConfig(int themeID, int langID, int skipDeviceRefresh)
{
    // A deliberate settings apply may make new art available, so clear the .tar "no archive
    // anywhere" latch and let it be probed once more. That latch (tar.c s_inactive[]) is write-once
    // and process-wide with no self-clearing path, so without this a user who boots with the loader
    // already ON and no archive present -- then plugs one in -- keeps getting nothing until a
    // reboot. The Artwork page's own toggle-flip re-arm only covers the case where the toggle
    // CHANGES; this covers the rest.
    // NOTE(rebuild): the fork pairs this with cacheInvalidateFailMemo(), which the rebuild's
    // official-derived texcache does not have. Only the .tar half applies here.
    tarInvalidate(TAR_KIND_ART);

    if (gDefaultDevice < 0 || gDefaultDevice > APP_MODE)
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
            for (int i = 0; i < MODE_COUNT; i++) {
                if (list_support[i].support && list_support[i].subMenu)
                    submenuRebuildCache(list_support[i].subMenu);
            }
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

    guiHandleDeferedIO(&lscstatus, _l(_STR_SAVING_SETTINGS), IO_CUSTOM_SIMPLEACTION, &_saveConfig, OPL_DEFERRED_IO_TIMEOUT_MS);

    if (showUI) {
        if (lscret) {
            char *path = configGetDir();

            snprintf(notification, sizeof(notification), _l(_STR_SETTINGS_SAVED), path);

            guiMsgBox(notification, 0, NULL);
        } else {
            snprintf(notification, sizeof(notification), _l(_STR_ERROR_SAVING_SETTINGS_TO), configGetDir(), gLastSaveErrno);
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

// deinitEx: deinit for the Neutrino keep-IOP handoff -- spares TWO modes' mounts (the game
// device AND the device holding neutrino.elf), since Neutrino reads its -cwd config/modules
// and the ISO through OPL's live mounts before performing its own IOP reset.
void deinitEx(int exception, int modeSelected, int modeSelected2)
{
    gDeinitTerminal = (modeSelected == IO_MODE_SELECTED_ALL || modeSelected == IO_MODE_SELECTED_NONE);

    // block all io ops, wait for the ones still running to finish
    ioBlockOps(1);
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

    // block all io ops, wait for the ones still running to finish
    ioBlockOps(1);
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
    gHDDPrefix = "pfs0:";
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
    gPCShareName[0] = '\0';
    gPCUserName[0] = '\0';
    gPCPassword[0] = '\0';
    gNetworkStartup = ERROR_ETH_NOT_STARTED;
    gHDDSpindown = 20;
    gScrollSpeed = 1;
    gExitPath[0] = '\0';
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
    gBDMPrefix[0] = '\0';
    gETHPrefix[0] = '\0';
    gEnableNotifications = 1;
    gEnableArt = 1;
    gWideScreen = 1;
    gEnableSFX = 1; // safe now: sfxPlay dispatches asynchronously (#340)
    gEnableBootSND = 1;
    gEnableBGM = 1; // inert without a bgm.ogg on the card
    gSFXVolume = 80;
    gBootSndVolume = 80;
    gBGMVolume = 70;
    gDefaultBGMPath[0] = '\0';
    gXSensitivity = 1;
    gYSensitivity = 1;

    gBDMStartMode = START_MODE_DISABLED;
    gHDDStartMode = START_MODE_DISABLED;
    gETHStartMode = START_MODE_DISABLED;
    gAPPStartMode = START_MODE_DISABLED;

    gEnableUSB = 0; // USB block device is opt-in, like the other BDM transports
    gFAVStartMode = START_MODE_DISABLED;
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
    gBdmaApplyOnLaunch = 1; // auto-equip on launch by default
    gVcdHideGameId = 1;     // hide the PS1 game-ID prefix by default (display-only)
    gVcdFirstDiscOnly = 1;  // hide discs 2+ of multi-disc PS1 sets by default (POPSLoader parity)
    gBootDir[0] = '\0';
    gEnableBGArt = 1; // fork parity; gEnableArt is 1 above, so this is live
    gEnableArtTar = 0;
    gArtDelay = 8; // official-like settle (the fork's aggressive 2-frame default is a gate-D decision, item 45)
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
    configInit(NULL);

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
    guiDeferUpdate(id);

    if (list_support[gDefaultDevice].support) {
        id = guiOpCreate(GUI_OP_SELECT_MENU);
        id->menu.menu = &list_support[gDefaultDevice].menuItem;
        guiDeferUpdate(id);
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
    configInit(NULL);

    ioInit();
    LOG_ENABLE();

    if (mode == BDM_MODE) {
        bdmInitSemaphore();

        // Force load iLink & mx4sio modules.. we aren't using the gui so this is fine.
        gEnableILK = 1; // iLink will break pcsx2 however.
        gEnableMX4SIO = 1;
        gEnableBdmHDD = 1;
        bdmLoadModules();

    } else if (mode == HDD_MODE) {
        hddLoadModules();
        hddLoadSupportModules();
    }

    InitConsoleRegionData();

    ret = configReadMulti(CONFIG_ALL);
    if (CONFIG_ALL & CONFIG_OPL) {
        if (!(ret & CONFIG_OPL)) {
            if (mode == BDM_MODE)
                ret = checkLoadConfigBDM(CONFIG_ALL);
            else if (mode == HDD_MODE)
                ret = checkLoadConfigHDD(CONFIG_ALL);
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
    memset(gAutoLaunchGame, 0, sizeof(hdl_game_info_t));

    snprintf(gAutoLaunchGame->startup, sizeof(gAutoLaunchGame->startup), argv[1]);
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
    memset(gAutoLaunchBDMGame, 0, sizeof(base_game_info_t));

    int nameLen;
    int format = isValidIsoName(argv[1], &nameLen);
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

    snprintf(gAutoLaunchBDMGame->startup, sizeof(gAutoLaunchBDMGame->startup), argv[2]);

    if (strcasecmp("DVD", argv[3]) == 0)
        gAutoLaunchBDMGame->media = SCECdPS2DVD;
    else if (strcasecmp("CD", argv[3]) == 0)
        gAutoLaunchBDMGame->media = SCECdPS2CD;

    gAutoLaunchBDMGame->format = format;
    gAutoLaunchBDMGame->parts = 1; // ul not supported.

    gAutoLaunchDeviceData = malloc(sizeof(bdm_device_data_t));
    memset(gAutoLaunchDeviceData, 0, sizeof(bdm_device_data_t));

    char apaDevicePrefix[8] = {0};
    delay(8);
    snprintf(apaDevicePrefix, sizeof(apaDevicePrefix), "mass0:");
    // Loop through mass0: to mass4:
    for (int i = 0; i <= 4; i++) {
        snprintf(path, sizeof(path), "mass%d:", i);
        int dir = fileXioDopen(path);

        if (dir >= 0) {
            fileXioIoctl2(dir, USBMASS_IOCTL_GET_DRIVERNAME, NULL, 0, &gAutoLaunchDeviceData->bdmDriver, sizeof(gAutoLaunchDeviceData->bdmDriver) - 1);
            fileXioIoctl2(dir, USBMASS_IOCTL_GET_DEVICE_NUMBER, NULL, 0, &gAutoLaunchDeviceData->massDeviceIndex, sizeof(gAutoLaunchDeviceData->massDeviceIndex));

            if (!strcmp(gAutoLaunchDeviceData->bdmDriver, "ata") && strlen(gAutoLaunchDeviceData->bdmDriver) == 3) {
                bdmResolveLBA_UDMA(gAutoLaunchDeviceData);
                snprintf(apaDevicePrefix, sizeof(apaDevicePrefix), "mass%d:", i);
                fileXioDclose(dir);
                break; // Exit the loop if "ata" device is found
            }

            fileXioDclose(dir);
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
// Basename of the ELF OPL was booted as (argv[0]); pairs with gBootDir. Static: consumers
// outside this file go through gBootDir only.
static char gBootElfName[64];

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
    guiMainLoop();

    return 0;
}
