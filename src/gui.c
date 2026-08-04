/*
 Copyright 2010, Volca
 Licenced under Academic Free License version 3.0
 Review OpenUsbLd README & LICENSE files for further details.
 */

#include "include/opl.h"
#include "include/gui.h"
#include "include/renderman.h"
#include "include/menusys.h"
#include "include/fntsys.h"
#include "include/ioman.h"
#include "include/lang.h"
#include "include/themes.h"
#include "include/pad.h"
#include "include/util.h"
#include "include/config.h"
#include "include/system.h"
#include "include/vcdsupport.h" // BDMA equip (vcdEquipBdma/vcdReadBdmaMode + VCD_BDMA_* enums)
#include "include/appsupport.h"
#include "include/ethsupport.h"
#include "include/mmcesupport.h"
#include "include/udpfssupport.h" // udpfsGetModulesLoaded() -- network-protocol restart-notice check
#include "include/bdmsupport.h"   // bdmForceDeviceRefresh() -- re-show a latched-hidden BDM tab after a device-enable toggle
#include "include/hddsupport.h"   // hddVcdInvalidateCache() -- first-disc-only is a scan-time filter
#include "include/favsupport.h"
#include "include/compatupd.h"
#include "include/pggsm.h"
#include "include/cheatman.h"
#include "include/sound.h"
#include "include/guigame.h"
#include "include/texcache.h"
#include "include/tar.h" // tarInvalidate -- re-arm the .tar probe when the toggle flips

#include <limits.h>
#include <stdlib.h>
#include <libvux.h>

// Last Played Auto Start
#include <time.h>

static int gScheduledOps;
static int gCompletedOps;
static int gTerminate;
static int gInitComplete;

static gui_callback_t gFrameHook;

static s32 gSemaId;
static s32 gGUILockSemaId = -1; // -1 = not created yet (guiLock/guiUnlock no-op; see guiLock)
static ee_sema_t gQueueSema;

static int screenWidth;
static int screenHeight;

static int showPartPopup = 0;
static int showThmPopup;
static int showLngPopup;

static clock_t popupTimer;

// Boot-splash status line (#297, fork-native): set via guiSetBootStatus(), drawn under the logo by
// guiRenderGreeting(). Both on the MAIN thread, so gBootStatus needs no locking.
static char gBootStatus[64] = {0};
static int gBootStatusActive = 0;
// Boot-step localizer: the boot->menu handoff runs on the single IO worker thread (bdmLoadBlockDevice-
// Modules -> mmceArmGameIDTransport -> deferredAudioInit -> deferredInit), any step of which can wedge
// with no timeout on real hardware (e.g. a USB/exFAT module bring-up) and freeze the splash. The MAIN
// thread meanwhile races ahead setting "Scanning MC..."/"Ready.", so the frozen screen would show a
// useless "Ready." (the brenotomaz report) instead of the stuck step. An IO-thread step publishes its
// label via guiSetBootStatusSticky(); guiRenderGreeting PREFERS it over gBootStatus, so whichever
// ordering wins the STUCK STEP is what stays on screen. Cross-thread state is a single aligned POINTER
// (atomic load/store on the EE) to a static _l() string -- no shared buffer, so no data race (the labels
// outlive boot; the greeting is boot-only). Cleared by guiSetBootStatus(NULL).
static const char *volatile gBootStickyLabel = NULL;

// forward decl.
static void guiShow();

#ifdef __DEBUG

// debug version displays an FPS meter
static clock_t prevtime = 0;
static clock_t curtime = 0;
static float fps = 0.0f;

extern GSGLOBAL *gsGlobal;
#endif

// Global data
int guiInactiveFrames;
int guiFrameId;

struct gui_update_list_t
{
    struct gui_update_t *item;
    struct gui_update_list_t *next;
};

struct gui_update_list_t *gUpdateList;
struct gui_update_list_t *gUpdateEnd;

typedef struct
{
    void (*handleInput)(void);
    void (*renderScreen)(void);
    short inMenu;
} gui_screen_handler_t;

static gui_screen_handler_t screenHandlers[] = {{&menuHandleInputMain, &menuRenderMain, 0},
                                                {&menuHandleInputMenu, &menuRenderMenu, 1},
                                                {&menuHandleInputInfo, &menuRenderInfo, 1},
                                                {&menuHandleInputGameMenu, &menuRenderGameMenu, 1},
                                                {&menuHandleInputAppMenu, &menuRenderAppMenu, 1}};

// default screen handler (menu screen)
static gui_screen_handler_t *screenHandler = &screenHandlers[GUI_SCREEN_MENU];

// screen transition handling
static gui_screen_handler_t *screenHandlerTarget = NULL;
static int transIndex;

// Helper perlin noise data
#define PLASMA_H              32
#define PLASMA_W              32
#define PLASMA_ROWS_PER_FRAME 6
#define FADE_SIZE             256

static GSTEXTURE gBackgroundTex;
static int pperm[512];
static float fadetbl[FADE_SIZE + 1];

static VU_VECTOR pgrad3[12] = {{1, 1, 0, 1}, {-1, 1, 0, 1}, {1, -1, 0, 1}, {-1, -1, 0, 1}, {1, 0, 1, 1}, {-1, 0, 1, 1}, {1, 0, -1, 1}, {-1, 0, -1, 1}, {0, 1, 1, 1}, {0, -1, 1, 1}, {0, 1, -1, 1}, {0, -1, -1, 1}};

void guiReloadScreenExtents()
{
    rmGetScreenExtents(&screenWidth, &screenHeight);
}

void guiInit(void)
{
    guiFrameId = 0;
    guiInactiveFrames = 0;

    gFrameHook = NULL;
    gTerminate = 0;
    gInitComplete = 0;
    gScheduledOps = 0;
    gCompletedOps = 0;

    gUpdateList = NULL;
    gUpdateEnd = NULL;

    gQueueSema.init_count = 1;
    gQueueSema.max_count = 1;
    gQueueSema.option = 0;

    gSemaId = CreateSema(&gQueueSema);
    gGUILockSemaId = CreateSema(&gQueueSema);

    guiReloadScreenExtents();

    // background texture - for perlin
    gBackgroundTex.Width = PLASMA_W;
    gBackgroundTex.Height = PLASMA_H;
    gBackgroundTex.Mem = memalign(128, PLASMA_W * PLASMA_H * 4);
    gBackgroundTex.PSM = GS_PSM_CT32;
    gBackgroundTex.Filter = GS_FILTER_LINEAR;
    gBackgroundTex.Vram = 0;
    gBackgroundTex.VramClut = 0;
    gBackgroundTex.Clut = NULL;
    gBackgroundTex.ClutStorageMode = GS_CLUT_STORAGE_CSM1;

    // Precalculate the values for the perlin noise plasma
    int i;
    for (i = 0; i < 256; ++i) {
        pperm[i] = rand() % 256;
        pperm[i + 256] = pperm[i];
    }

    for (i = 0; i <= FADE_SIZE; ++i) {
        float t = (float)(i) / FADE_SIZE;

        fadetbl[i] = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }
}

void guiEnd()
{
    if (gBackgroundTex.Mem)
        free(gBackgroundTex.Mem);

    DeleteSema(gSemaId);
    DeleteSema(gGUILockSemaId);
    gGUILockSemaId = -1; // post-shutdown callers no-op instead of waiting on a deleted id
}

void guiLock(void)
{
    // Not-ready guard: lngInit/thmInit rebuild the GUI name lists BEFORE guiInit creates this
    // semaphore (opl.c init order), and everything is single-threaded until the GUI/IO threads
    // exist -- a no-op lock is correct there, and WaitSema on id 0/garbage is not.
    if (gGUILockSemaId < 0)
        return;
    WaitSema(gGUILockSemaId);
}

void guiUnlock(void)
{
    if (gGUILockSemaId < 0)
        return;
    SignalSema(gGUILockSemaId);
}

void guiStartFrame(void)
{
    guiLock();
    rmStartFrame();
    guiFrameId++;
}

void guiEndFrame(void)
{
    rmEndFrame();
#ifdef __DEBUG
    // Measure time directly after vsync
    prevtime = curtime;
    curtime = clock();
#endif
    guiUnlock();
}

void guiShowAbout()
{
    char OPLVersion[40];
    char OPLBuildDetails[40];

    snprintf(OPLVersion, sizeof(OPLVersion), "Open PS2 Loader %s", OPL_VERSION);
    diaSetLabel(diaAbout, ABOUT_TITLE, OPLVersion);

    snprintf(OPLBuildDetails, sizeof(OPLBuildDetails), "GSM %s"
                                                       " - UDMA+"
#ifdef __RTL
                                                       " - RTL"
#endif
#ifdef IGS
                                                       " - IGS %s"
#endif
#ifdef PADEMU
                                                       " - PADEMU"
#endif
             // Version numbers
             ,
             GSM_VERSION
#ifdef IGS
             ,
             IGS_VERSION
#endif
    );
    diaSetLabel(diaAbout, ABOUT_BUILD_DETAILS, OPLBuildDetails);

    diaExecuteDialog(diaAbout, -1, 1, NULL);
}

void guiCheckNotifications(int checkTheme, int checkLang)
{
    if (gEnableNotifications) {
        if (checkTheme) {
            // Only disk themes have a path to announce; built-ins (<OPL>, <Coverflow>) return
            // NULL from thmGetFilePath -> no popup (and no NULL deref in guiShowNotifications).
            if (thmGetFilePath(thmGetGuiValue()) != NULL)
                showThmPopup = 1;
        }

        if (checkLang) {
            if (lngGetGuiValue() != 0)
                showLngPopup = 1;
        }
    }
}

static void guiResetNotifications(void)
{
    showThmPopup = 0;
    showLngPopup = 0;
    popupTimer = 0;
}

static void guiRenderNotifications(char *string, int y)
{
    int x;

    x = screenWidth - rmUnScaleX(fntCalcDimensions(gTheme->fonts[0], string)) - 10;

    rmDrawRect(x - 10, y, screenWidth - x, MENU_ITEM_HEIGHT + 10, gColDarker);
    fntRenderString(gTheme->fonts[0], x - 5, y + 5, ALIGN_NONE, 0, 0, string, gTheme->textColor);
}

static void guiShowNotifications(void)
{
    char notification[128];
    char *col_pos;
    int y = 10;
    int yadd = 35;

    if (showPartPopup || showThmPopup || showLngPopup || showCfgPopup || showNetDhcpPopup) {
        if (!popupTimer) {
            popupTimer = clock() + 5000 * (CLOCKS_PER_SEC / 1000);
            sfxPlay(SFX_MESSAGE);
        }

        if (showPartPopup) {
            col_pos = strchr(gOPLPart, ':');
            col_pos++;

            snprintf(notification, sizeof(notification), _l(_STR_PARTITION_NOTIFICATION), col_pos);
            guiRenderNotifications(notification, y);
            y += yadd;
        }

        if (showCfgPopup) {
            snprintf(notification, sizeof(notification), _l(_STR_CFG_NOTIFICATION), configGetDir());
            if ((col_pos = strchr(notification, ':')) != NULL)
                *(col_pos + 1) = '\0';

            guiRenderNotifications(notification, y);
            y += yadd;
        }

        if (showThmPopup) {
            snprintf(notification, sizeof(notification), _l(_STR_THM_NOTIFICATION), thmGetFilePath(thmGetGuiValue()));
            if ((col_pos = strchr(notification, ':')) != NULL)
                *(col_pos + 1) = '\0';

            guiRenderNotifications(notification, y);
            y += yadd;
        }

        if (showLngPopup) {
            snprintf(notification, sizeof(notification), _l(_STR_LNG_NOTIFICATION), lngGetFilePath(lngGetGuiValue()));
            if ((col_pos = strchr(notification, ':')) != NULL)
                *(col_pos + 1) = '\0';

            guiRenderNotifications(notification, y);
            y += yadd;
        }

        // One-time network notice set at config load: a UDP transport left on DHCP (the ministack has
        // no DHCP client; it binds the static PS2 IP fields as-is, so an unset static IP fails silently).
        if (showNetDhcpPopup) {
            guiRenderNotifications(_l(_STR_UDPBD_NEEDS_STATIC_IP), y);
            y += yadd;
        }

        if (clock() >= popupTimer) {
            guiResetNotifications();
            showPartPopup = 0;
            showCfgPopup = 0;
            showNetDhcpPopup = 0;
        }
    }
}

static int guiNetCompatUpdRefresh(int modified)
{
    int result;
    unsigned int done, total;

    if ((result = oplGetUpdateGameCompatStatus(&done, &total)) == OPL_COMPAT_UPDATE_STAT_WIP) {
        diaSetInt(diaNetCompatUpdate, NETUPD_PROGRESS, (done == 0 || total == 0) ? 0 : (int)((float)done / total * 100.0f));
    }

    return result;
}

static void guiShowNetCompatUpdateResult(int result)
{
    switch (result) {
        case OPL_COMPAT_UPDATE_STAT_DONE:
            // Completed with no errors.
            guiMsgBox(_l(_STR_NET_UPDATE_DONE), 0, NULL);
            break;
        case OPL_COMPAT_UPDATE_STAT_ERROR:
            // Completed with errors.
            guiMsgBox(_l(_STR_NET_UPDATE_FAILED), 0, NULL);
            break;
        case OPL_COMPAT_UPDATE_STAT_CONN_ERROR:
            // Completed with errors.
            guiMsgBox(_l(_STR_NET_UPDATE_CONN_FAILED), 0, NULL);
            break;
        case OPL_COMPAT_UPDATE_STAT_ABORTED:
            // User-aborted.
            guiMsgBox(_l(_STR_NET_UPDATE_CANCELLED), 0, NULL);
            break;
    }
}

void guiShowNetCompatUpdate(void)
{
    int ret, UpdateAll;
    u8 done, started;
    void *UpdateFunction;

    diaSetVisible(diaNetCompatUpdate, NETUPD_BTN_START, 1);
    diaSetVisible(diaNetCompatUpdate, NETUPD_BTN_CANCEL, 0);
    diaSetVisible(diaNetCompatUpdate, NETUPD_PROGRESS_LBL, 0);
    diaSetVisible(diaNetCompatUpdate, NETUPD_PROGRESS_PERC_LBL, 0);
    diaSetVisible(diaNetCompatUpdate, NETUPD_PROGRESS, 0);
    diaSetInt(diaNetCompatUpdate, NETUPD_OPT_UPD_ALL, 0);
    diaSetEnabled(diaNetCompatUpdate, NETUPD_OPT_UPD_ALL, 1);

    done = 0;
    started = 0;
    UpdateFunction = NULL;
    while (!done) {
        ret = diaExecuteDialog(diaNetCompatUpdate, -1, 1, UpdateFunction);
        switch (ret) {
            case NETUPD_BTN_START:
                if (guiMsgBox(_l(_STR_CONFIRMATION_SETTINGS_UPDATE), 1, NULL)) {
                    guiRenderTextScreen(_l(_STR_PLEASE_WAIT));

                    if ((ret = ethLoadInitModules()) == 0) {
                        diaSetVisible(diaNetCompatUpdate, NETUPD_BTN_START, 0);
                        diaSetVisible(diaNetCompatUpdate, NETUPD_BTN_CANCEL, 1);
                        diaSetVisible(diaNetCompatUpdate, NETUPD_PROGRESS_LBL, 1);
                        diaSetVisible(diaNetCompatUpdate, NETUPD_PROGRESS_PERC_LBL, 1);
                        diaSetVisible(diaNetCompatUpdate, NETUPD_PROGRESS, 1);
                        diaSetEnabled(diaNetCompatUpdate, NETUPD_OPT_UPD_ALL, 0);

                        diaGetInt(diaNetCompatUpdate, NETUPD_OPT_UPD_ALL, &UpdateAll);
                        oplUpdateGameCompat(UpdateAll);
                        UpdateFunction = &guiNetCompatUpdRefresh;
                        started = 1;
                    } else {
                        ethDisplayErrorStatus();
                    }
                }
                break;
            case UIID_BTN_CANCEL: // If the user pressed the cancel button.
            case NETUPD_BTN_CANCEL:
                if (started) {
                    if (guiMsgBox(_l(_STR_CONFIRMATION_CANCEL_UPDATE), 1, NULL)) {
                        guiRenderTextScreen(_l(_STR_PLEASE_WAIT));
                        oplAbortUpdateGameCompat();
                        // The process truly ends when the UI callback gets the update from the worker thread that the process has ended.
                    }
                } else {
                    done = 1;
                    started = 0;
                }
                break;
            default:
                guiShowNetCompatUpdateResult(ret);
                done = 1;
                started = 0;
                UpdateFunction = NULL;
                break;
        }
    }
}

void guiShowNetCompatUpdateSingle(int id, item_list_t *support, config_set_t *configSet)
{
    int ConfigSource, result;

    ConfigSource = CONFIG_SOURCE_DEFAULT;
    configGetInt(configSet, CONFIG_ITEM_CONFIGSOURCE, &ConfigSource);

    if (guiMsgBox(_l(_STR_CONFIRMATION_SETTINGS_UPDATE), 1, NULL)) {
        guiRenderTextScreen(_l(_STR_PLEASE_WAIT));

        if ((ethLoadInitModules()) == 0) {
            if ((result = oplUpdateGameCompatSingle(id, support, configSet)) == OPL_COMPAT_UPDATE_STAT_DONE) {
                configSetInt(configSet, CONFIG_ITEM_CONFIGSOURCE, CONFIG_SOURCE_DLOAD);
            }
            guiShowNetCompatUpdateResult(result);
        } else {
            ethDisplayErrorStatus();
        }
    }
}

static int guiUpdater(int modified)
{
    int showAutoStartLast;

    if (modified) {
        diaGetInt(diaConfig, CFG_LASTPLAYED, &showAutoStartLast);
        diaSetVisible(diaConfig, CFG_LBL_AUTOSTARTLAST, showAutoStartLast);
        diaSetVisible(diaConfig, CFG_AUTOSTARTLAST, showAutoStartLast);
    }
    return 0;
}

// POPStarter page live-updater: reveal the free-text POPSTARTER.ELF Path field only when the
// device picker is "Custom".
static int guiVcdUpdater(int modified)
{
    int popsDev;

    if (modified) {
        diaGetInt(diaVcdConfig, CFG_POPSTARTER_DEVICE, &popsDev);
        diaSetVisible(diaVcdConfig, CFG_LBL_POPSTARTER_PATH, popsDev == POPS_DEV_CUSTOM);
        diaSetVisible(diaVcdConfig, CFG_POPSTARTER_PATH, popsDev == POPS_DEV_CUSTOM);
    }
    return 0;
}

// BDMA Settings live-updater: hide the manual BDMA Source/Mode pickers while "VCD BDMA Apply on
// Launch" is ON (it auto-equips); re-reveal them live when toggled off.
static int guiBdmaUpdater(int modified)
{
    int bdmaApply;

    if (modified) {
        diaGetInt(diaBdmaConfig, CFG_BDMA_APPLY, &bdmaApply);
        diaSetVisible(diaBdmaConfig, CFG_LBL_BDMASOURCE, !bdmaApply);
        diaSetVisible(diaBdmaConfig, CFG_BDMASOURCE, !bdmaApply);
        diaSetVisible(diaBdmaConfig, CFG_LBL_BDMAMODE, !bdmaApply);
        diaSetVisible(diaBdmaConfig, CFG_BDMAMODE, !bdmaApply);
    }
    return 0;
}

int guiDeviceTypeToIoMode(int deviceType)
{
    // Translates an index into deviceNames into an IO mode index used internally.
    if (deviceType == 0)
        return BDM_MODE;
    else if (deviceType == 1)
        return ETH_MODE;
    else if (deviceType == 2)
        return HDD_MODE;
    else if (deviceType == 3)
        return APP_MODE;
    else if (deviceType == 4)
        return MMCE_MODE;
    else if (deviceType == 5)
        return FAV_MODE;
    else
        return APP_MODE; // safe fallback for unexpected indices: Apps is always present, FAV may be disabled
}

int guiIoModeToDeviceType(int ioMode)
{
    if (ioMode >= BDM_MODE && ioMode < ETH_MODE)
        return 0;
    if (ioMode == ETH_MODE)
        return 1;
    if (ioMode == HDD_MODE)
        return 2;
    if (ioMode == APP_MODE)
        return 3;
    if (ioMode == MMCE_MODE)
        return 4;
    if (ioMode == FAV_MODE)
        return 5;

    return 0;
}

// Settings page (settings-layout restructure): the slim "Settings" category -- remember/auto-start
// last played, folder navigation and Exit To. Everything else moved to the Game Launching,
// Security, Controller, Advanced, POPStarter and MMCE pages.
void guiShowConfig()
{
    // Exit To auto-resolves a built-in default when blank, so show a dim "Default" placeholder
    // rather than "<not set>" -- the empty value (and thus the fallback) is kept.
    diaSetShowDefaultWhenEmpty(diaConfig, CFG_EXITTO, 1);
    diaSetString(diaConfig, CFG_EXITTO, gExitPath);

    diaSetInt(diaConfig, CFG_LASTPLAYED, gRememberLastPlayed);
    diaSetInt(diaConfig, CFG_FOLDERNAV, gEnableFolderNav);
    diaSetInt(diaConfig, CFG_AUTOSTARTLAST, gAutoStartLastPlayed);
    diaSetVisible(diaConfig, CFG_AUTOSTARTLAST, gRememberLastPlayed);
    diaSetVisible(diaConfig, CFG_LBL_AUTOSTARTLAST, gRememberLastPlayed);

    int ret = diaExecuteDialog(diaConfig, -1, 1, &guiUpdater);
    if (ret) {
        diaGetString(diaConfig, CFG_EXITTO, gExitPath, sizeof(gExitPath));
        diaGetInt(diaConfig, CFG_LASTPLAYED, &gRememberLastPlayed);
        diaGetInt(diaConfig, CFG_FOLDERNAV, &gEnableFolderNav);
        diaGetInt(diaConfig, CFG_AUTOSTARTLAST, &gAutoStartLastPlayed);

        DisableCron = 1; // Disable Auto Start Last Played counter (we don't want to call it right after enable it on GUI)

        applyConfig(-1, -1, 0);
        menuReinitMainMenu();
    }
}

// Game Launching page: launch-time behavior (PS2 logo, global default Loader Core) + the global
// Neutrino and OSD defaults as chained sub-dialogs.
void guiShowLaunchConfig(void)
{
    // Global default Loader Core: the core a game uses when its per-game selector is "Default".
    // <OPL> = OPL's native ee-core; Neutrino = the external neutrino.elf. Indices 0/1 match the
    // stored value (0=<OPL>, 1=Neutrino) and the first two per-game COMPAT_LOADER options.
    const char *defaultCoreStrs[] = {"<OPL>", "Neutrino", NULL};
    diaSetEnum(diaLaunchConfig, CFG_DEFAULT_CORE, defaultCoreStrs);
    diaSetInt(diaLaunchConfig, CFG_DEFAULT_CORE, gDefaultCoreLoader);
    diaSetInt(diaLaunchConfig, CFG_PS2LOGO, gPS2Logo);

    int ret;
reshow_launch:
    ret = diaExecuteDialog(diaLaunchConfig, -1, 1, NULL);
    if (ret == LAUNCH_NEUTRINO_DEFAULTS_BUTTON) {
        guiShowNeutrinoDefaults();
        goto reshow_launch;
    }
    if (ret == LAUNCH_OSD_DEFAULTS_BUTTON) {
        guiGameShowOSDLanguageConfig(1);
        goto reshow_launch;
    }
    if (ret) {
        diaGetInt(diaLaunchConfig, CFG_PS2LOGO, &gPS2Logo);
        diaGetInt(diaLaunchConfig, CFG_DEFAULT_CORE, &gDefaultCoreLoader);

        applyConfig(-1, -1, 0);
        menuReinitMainMenu();
    }
}

// Neutrino Defaults live-updater: the ":c" comp half is only ever emitted alongside a video mode
// (-gsm=v:c grammar) -- grey it while the global Neutrino Video default is Off (same rule as the
// per-game rows).
static int guiNeutrinoDefaultsUpdater(int modified)
{
    int neutrinoVideoDef;

    if (modified) {
        diaGetInt(diaNeutrinoDefaults, CFG_NEUTRINO_VIDEO, &neutrinoVideoDef);
        diaSetEnabled(diaNeutrinoDefaults, CFG_NEUTRINO_GSMCOMP, neutrinoVideoDef != 0);
    }
    return 0;
}

// Game Launching -> Neutrino Defaults: the global Neutrino device/video/gsm-comp defaults + the
// structured Advanced Arguments editor.
void guiShowNeutrinoDefaults(void)
{
    // Neutrino lives at <root>:/neutrino/neutrino.elf on ANY device -- offer the common roots.
    // MUST stay in sync with the roots[] table in sbResolveNeutrinoPath() (supportbase.c).
    const char *neutrinoDevStrs[] = {_l(_STR_AUTO), "Memory Card", "USB", "MX4SIO", "MMCE", "HDD (exFAT)", "HDD (APA)", _l(_STR_GAMES_DEVICE), NULL}; // device TYPE holding /neutrino/neutrino.elf (NEUTRINO_DEV_*); "Game's Device" (NEUTRINO_DEV_GAME) appended last to match the enum tail
    diaSetEnum(diaNeutrinoDefaults, CFG_NEUTRINO_DEVICE, neutrinoDevStrs);
    diaSetInt(diaNeutrinoDefaults, CFG_NEUTRINO_DEVICE, gNeutrinoDevice);
    // Global default Neutrino Video (-gsm) + comp half: same indices as the per-game picker
    // (system.c gsmVideoTokens). static: literals only, and diaSetEnum stores the raw pointer.
    static const char *neutrinoVideoDefStrs[] = {"Off", "240p", "480p", "1080i x1", "1080i x2", "1080i x3", NULL};
    static const char *neutrinoGsmCompDefStrs[] = {"Off", "Type 1 (GSM/OPL)", "Type 2", "Type 3", NULL};
    diaSetEnum(diaNeutrinoDefaults, CFG_NEUTRINO_VIDEO, neutrinoVideoDefStrs);
    diaSetInt(diaNeutrinoDefaults, CFG_NEUTRINO_VIDEO, gNeutrinoVideoDefault);
    diaSetEnum(diaNeutrinoDefaults, CFG_NEUTRINO_GSMCOMP, neutrinoGsmCompDefStrs);
    diaSetInt(diaNeutrinoDefaults, CFG_NEUTRINO_GSMCOMP, gNeutrinoGsmCompDefault);
    diaSetEnabled(diaNeutrinoDefaults, CFG_NEUTRINO_GSMCOMP, gNeutrinoVideoDefault != 0);

    int ret;
reshow_neutrino:
    ret = diaExecuteDialog(diaNeutrinoDefaults, -1, 1, &guiNeutrinoDefaultsUpdater);
    if (ret == CFG_NEUTRINO_ARGS) {
        // the "Advanced Arguments" button -> open the structured args sub-screen, then re-enter
        guiShowNeutrinoArgsConfig(gNeutrinoArgs, sizeof(gNeutrinoArgs));
        goto reshow_neutrino;
    }
    if (ret) {
        diaGetInt(diaNeutrinoDefaults, CFG_NEUTRINO_DEVICE, &gNeutrinoDevice);
        diaGetInt(diaNeutrinoDefaults, CFG_NEUTRINO_VIDEO, &gNeutrinoVideoDefault);
        diaGetInt(diaNeutrinoDefaults, CFG_NEUTRINO_GSMCOMP, &gNeutrinoGsmCompDefault);

        applyConfig(-1, -1, 0);
    }
}

// Security page: the write-operations gate + the Parental Lock password as a chained sub-dialog.
void guiShowSecurityConfig(void)
{
    diaSetInt(diaSecurityConfig, CFG_ENWRITEOP, gEnableWrite);

    int ret;
reshow_security:
    ret = diaExecuteDialog(diaSecurityConfig, -1, 1, NULL);
    if (ret == SECURITY_PARENTAL_BUTTON) {
        guiShowParentalLockConfig();
        goto reshow_security;
    }
    if (ret) {
        diaGetInt(diaSecurityConfig, CFG_ENWRITEOP, &gEnableWrite);

        applyConfig(-1, -1, 0);
        menuReinitMainMenu();
    }
}

// Advanced page: debug display + path prefixes and storage/cache as chained sub-dialogs.
void guiShowAdvancedConfig(void)
{
    diaSetInt(diaAdvancedConfig, CFG_DEBUG, gEnableDebug);

    int ret;
reshow_advanced:
    ret = diaExecuteDialog(diaAdvancedConfig, -1, 1, NULL);
    if (ret == ADV_PREFIX_BUTTON) {
        guiShowPathPrefixConfig();
        goto reshow_advanced;
    }
    if (ret == ADV_STORAGE_BUTTON) {
        guiShowStorageConfig();
        goto reshow_advanced;
    }
    if (ret) {
        diaGetInt(diaAdvancedConfig, CFG_DEBUG, &gEnableDebug);

        applyConfig(-1, -1, 0);
        menuReinitMainMenu();
    }
}

// Advanced -> Path Prefixes (BDM / Network library prefixes).
void guiShowPathPrefixConfig(void)
{
    diaSetString(diaPathPrefixConfig, CFG_BDMPREFIX, gBDMPrefix);
    diaSetString(diaPathPrefixConfig, CFG_ETHPREFIX, gETHPrefix);

    int ret = diaExecuteDialog(diaPathPrefixConfig, -1, 1, NULL);
    if (ret) {
        diaGetString(diaPathPrefixConfig, CFG_BDMPREFIX, gBDMPrefix, sizeof(gBDMPrefix));
        diaGetString(diaPathPrefixConfig, CFG_ETHPREFIX, gETHPrefix, sizeof(gETHPrefix));

        applyConfig(-1, -1, 0);
    }
}

// Advanced -> Storage and Cache (HDD spindown, HDD game-list cache, BDM/HDD/SMB cache sizes).
void guiShowStorageConfig(void)
{
    diaSetInt(diaStorageConfig, CFG_HDDSPINDOWN, gHDDSpindown);
    diaSetInt(diaStorageConfig, CFG_HDDGAMELISTCACHE, gHDDGameListCache);
    diaSetInt(diaStorageConfig, CFG_BDMCACHE, bdmCacheSize);
    diaSetInt(diaStorageConfig, CFG_HDDCACHE, hddCacheSize);
    diaSetInt(diaStorageConfig, CFG_SMBCACHE, smbCacheSize);

    int ret = diaExecuteDialog(diaStorageConfig, -1, 1, NULL);
    if (ret) {
        diaGetInt(diaStorageConfig, CFG_HDDSPINDOWN, &gHDDSpindown);
        diaGetInt(diaStorageConfig, CFG_HDDGAMELISTCACHE, &gHDDGameListCache);
        diaGetInt(diaStorageConfig, CFG_BDMCACHE, &bdmCacheSize);
        diaGetInt(diaStorageConfig, CFG_HDDCACHE, &hddCacheSize);
        diaGetInt(diaStorageConfig, CFG_SMBCACHE, &smbCacheSize);

        applyConfig(-1, -1, 0);
    }
}

// Tools page: one-shot actions that used to be top-level menu entries. No persistent state of its
// own -- each button runs its tool, then the page re-opens.
void guiShowToolsConfig(void)
{
    int ret;
reshow_tools:
    ret = diaExecuteDialog(diaToolsConfig, -1, 1, NULL);
    if (ret == TOOLS_NET_UPDATE_BUTTON) {
        guiShowNetCompatUpdate();
        goto reshow_tools;
    }
    if (ret == TOOLS_NBD_BUTTON) {
        handleLwnbdSrv();
        goto reshow_tools;
    }
}

// USB .VCD launches only: force the fat32/exFAT POPSTARTER driver pick EVERY launch -- the PS2
// cannot detect the filesystem a USB stick is actually formatted with, so the user chooses per
// launch. Returns the pressed button's id (VCDUSB_BTN_FAT32 / VCDUSB_BTN_EXFAT) or UIID_BTN_CANCEL.
int guiShowVcdUsbMode(void)
{
    return diaExecuteDialog(diaVcdUsbMode, -1, 1, NULL);
}

// POPStarter page (settings-layout restructure, was VCD Settings): PS1-via-POPSTARTER launch
// config (POPSTARTER.ELF device/path). The BDMA equip, VCD list display options and POPStarter
// network settings are chained sub-dialogs (guiShowBdmaConfig / guiShowVcdListConfig /
// guiShowPopsNetConfig). CFG ids are shared with the old rows, so saved config values map through
// unchanged.
void guiShowVcdConfig(void)
{
    // POPSTARTER.ELF device TYPE (POPS_DEV_*). MUST stay in sync with vcdResolvePopstarter() (vcdsupport.c).
    const char *popsDevStrs[] = {_l(_STR_DEFAULT), "Memory Card", "USB", "MX4SIO", "MMCE", "HDD (exFAT)", "HDD (APA)", "Custom", _l(_STR_GAMES_DEVICE), NULL}; // "Game's Device" (POPS_DEV_GAME) appended last to match the enum tail
    diaSetEnum(diaVcdConfig, CFG_POPSTARTER_DEVICE, popsDevStrs);
    diaSetInt(diaVcdConfig, CFG_POPSTARTER_DEVICE, gPopstarterDevice);
    diaSetString(diaVcdConfig, CFG_POPSTARTER_PATH, gPopstarterPath);
    diaSetShowDefaultWhenEmpty(diaVcdConfig, CFG_POPSTARTER_PATH, 1);
    diaSetInt(diaVcdConfig, CFG_POPSTARTER_RETROGEM_GAMEID, gPopstarterRetroGemGameID);

    // POPSTARTER Path is the Custom-only escape hatch (guiVcdUpdater re-reveals it live).
    diaSetVisible(diaVcdConfig, CFG_LBL_POPSTARTER_PATH, gPopstarterDevice == POPS_DEV_CUSTOM);
    diaSetVisible(diaVcdConfig, CFG_POPSTARTER_PATH, gPopstarterDevice == POPS_DEV_CUSTOM);

    int ret;
reshow_vcd:
    ret = diaExecuteDialog(diaVcdConfig, -1, 1, &guiVcdUpdater);
    if (ret == VCD_BDMA_BUTTON) {
        guiShowBdmaConfig();
        goto reshow_vcd;
    }
    if (ret == VCD_LIST_BUTTON) {
        guiShowVcdListConfig();
        goto reshow_vcd;
    }
    if (ret == VCD_NET_BUTTON) {
        guiShowPopsNetConfig();
        goto reshow_vcd;
    }
    if (ret) {
        diaGetInt(diaVcdConfig, CFG_POPSTARTER_DEVICE, &gPopstarterDevice);
        diaGetInt(diaVcdConfig, CFG_POPSTARTER_RETROGEM_GAMEID, &gPopstarterRetroGemGameID);

        {
            // The dialog field is char[32]; only adopt the typed value if it actually changed, so
            // opening+saving this page never truncates a longer path stored via the cfg.
            char tmpPop[sizeof(gPopstarterPath)];
            diaGetString(diaVcdConfig, CFG_POPSTARTER_PATH, tmpPop, sizeof(tmpPop));
            if (strncmp(tmpPop, gPopstarterPath, 31) != 0)
                snprintf(gPopstarterPath, sizeof(gPopstarterPath), "%s", tmpPop);
        }
        applyConfig(-1, -1, 0);
        menuReinitMainMenu();
    }
}

// POPStarter -> BDMA Settings (BDMAssault exFAT-driver equip). MODE reflects what's ACTUALLY on the
// card (marker read), so the page is honest even if POPSLoader or a prior session set it.
void guiShowBdmaConfig(void)
{
    const char *bdmaSourceStrs[] = {_l(_STR_BDMA_SRC_USB), _l(_STR_BDMA_SRC_MX4SIO), _l(_STR_BDMA_SRC_MMCE), _l(_STR_BDMA_SRC_HDD), NULL};
    const char *bdmaModeStrs[] = {_l(_STR_BDMA_MODE_FAT32), _l(_STR_BDMA_MODE_USBEXFAT), _l(_STR_BDMA_MODE_MX4SIO), _l(_STR_BDMA_MODE_MMCE), _l(_STR_BDMA_MODE_ATA), NULL};
    gBdmaMode = vcdReadBdmaMode();
    diaSetEnum(diaBdmaConfig, CFG_BDMASOURCE, bdmaSourceStrs);
    diaSetEnum(diaBdmaConfig, CFG_BDMAMODE, bdmaModeStrs);
    diaSetInt(diaBdmaConfig, CFG_BDMASOURCE, gBdmaSource);
    diaSetInt(diaBdmaConfig, CFG_BDMAMODE, gBdmaMode);
    diaSetInt(diaBdmaConfig, CFG_BDMA_APPLY, gBdmaApplyOnLaunch);
    // "VCD BDMA Apply on Launch" ON auto-equips, so hide the manual SOURCE/MODE pickers
    // (guiBdmaUpdater re-reveals them live when toggled off).
    diaSetVisible(diaBdmaConfig, CFG_LBL_BDMASOURCE, !gBdmaApplyOnLaunch);
    diaSetVisible(diaBdmaConfig, CFG_BDMASOURCE, !gBdmaApplyOnLaunch);
    diaSetVisible(diaBdmaConfig, CFG_LBL_BDMAMODE, !gBdmaApplyOnLaunch);
    diaSetVisible(diaBdmaConfig, CFG_BDMAMODE, !gBdmaApplyOnLaunch);

    int ret = diaExecuteDialog(diaBdmaConfig, -1, 1, &guiBdmaUpdater);
    if (ret) {
        diaGetInt(diaBdmaConfig, CFG_BDMA_APPLY, &gBdmaApplyOnLaunch);
        {
            // Equip BDMA modules only when SOURCE or MODE actually changed (the equip copies files to
            // the memory card). vcdEquipBdma is free-space-gated + truncation-safe, so a failure never
            // corrupts the card; report it and resync MODE to what's really equipped.
            int oldSrc = gBdmaSource, oldMode = gBdmaMode; // baselines (MODE = card's actual state)
            int newSrc = oldSrc, newMode = oldMode;
            diaGetInt(diaBdmaConfig, CFG_BDMASOURCE, &newSrc);
            diaGetInt(diaBdmaConfig, CFG_BDMAMODE, &newMode);
            // Re-equip on any MODE change, and on a SOURCE change only when MODE installs modules. FAT32
            // ignores the source, so a SOURCE-only move while already FAT32 must NOT re-run the pointless work.
            if (newMode != oldMode || (newMode != VCD_BDMA_FAT32 && newSrc != oldSrc)) {
                char bdmaDiag[160];
                int er = vcdEquipBdma(newSrc, newMode, bdmaDiag, sizeof(bdmaDiag));
                if (er == -4) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "%s\n%s", _l(_STR_BDMA_ERR_SRC), bdmaDiag);
                    guiMsgBox(msg, 0, NULL);
                } else if (er == -2)
                    guiMsgBox(_l(_STR_BDMA_ERR_SPACE), 0, NULL);
                else if (er == -3)
                    guiMsgBox(_l(_STR_BDMA_ERR_IO), 0, NULL);
                gBdmaMode = vcdReadBdmaMode(); // MODE = what is now actually equipped
            }
            gBdmaSource = newSrc; // remember the source preference regardless of equip outcome
        }
        applyConfig(-1, -1, 0);
    }
}

// POPStarter -> Game List Settings (VCD list display options).
void guiShowVcdListConfig(void)
{
    diaSetInt(diaVcdListConfig, CFG_VCD_HIDE_GAMEID, gVcdHideGameId);
    diaSetInt(diaVcdListConfig, CFG_VCD_FIRST_DISC_ONLY, gVcdFirstDiscOnly);

    int rebuildVcdLists = 0;
    int ret = diaExecuteDialog(diaVcdListConfig, -1, 1, NULL);
    if (ret) {
        {
            // #195: hide-gameid is NO LONGER purely cosmetic -- it is now a SORT KEY. The menu sort
            // (submenuSort) orders by the DISPLAYED title, i.e. past the hidden prefix, so a change must
            // re-sort every VCD-capable page. vcdMarkAllDirty() + rebuildVcdLists forces that menu rebuild
            // and its submenuSort re-runs against the new vcdDisplayName key -- WITHOUT re-reading any
            // device. Do NOT invalidate the HDD VCD cache here (unlike first-disc-only below): this toggle
            // changes only the DISPLAY ORDER, not the list CONTENTS, and submenuSort reorders the cached
            // rows in place. hddVcdInvalidateCache() would force an unnecessary full re-walk of every
            // __.POPS partition (pfs1: remount) plus a transient empty HDD VCD page on every flip (audit
            // 2026-07-17 of the CodeRabbit #200 fix -- my "sorted with the old key" note was true but
            // irrelevant, the menu sort fixes the order regardless of the backing array).
            int previousHideGameId = gVcdHideGameId;
            diaGetInt(diaVcdListConfig, CFG_VCD_HIDE_GAMEID, &gVcdHideGameId);
            if (gVcdHideGameId != previousHideGameId) {
                vcdMarkAllDirty();
                rebuildVcdLists = 1;
            }
        }
        {
            // #118: first-disc-only changes the VCD list CONTENTS (discs hidden/shown), so unlike the
            // cosmetic hide-gameid it must rebuild every VCD-capable device page when toggled. Device
            // lists only -- Favourites are intentionally left unfiltered (an explicit user pick).
            int previousFirstDiscOnly = gVcdFirstDiscOnly;
            diaGetInt(diaVcdListConfig, CFG_VCD_FIRST_DISC_ONLY, &gVcdFirstDiscOnly);
            if (gVcdFirstDiscOnly != previousFirstDiscOnly) {
                vcdMarkAllDirty();
                hddVcdInvalidateCache(); // scan-time filter changed -> the cached HDD VCD list is stale
                rebuildVcdLists = 1;
            }
        }
        applyConfig(-1, -1, 0);
        menuReinitMainMenu();
        // Queue after applyConfig/menu reinit so the IO worker cannot rebuild a submenu concurrently
        // with their support/menu bookkeeping on the GUI thread.
        if (rebuildVcdLists)
            oplQueueVcdDeviceUpdates();
    }
}

// MMCE page (settings-layout restructure, was MMCE Settings): SD2PSX / MemCard PRO2 basics (memory-
// card slot, IGR slot, GameID push). Communication tuning and the path prefix are chained
// sub-dialogs; CFG ids shared with the old rows.
void guiShowMmceConfig(void)
{
    const char *deviceSlots[] = {"0", "1", _l(_STR_AUTO), NULL};
    const char *deviceIGRSlots[] = {"NONE", "0", "1", "BOTH", NULL};

    diaSetEnum(diaMmceConfig, CFG_MMCESLOT, deviceSlots);
    diaSetInt(diaMmceConfig, CFG_MMCESLOT, gMMCESlot);
    diaSetEnum(diaMmceConfig, CFG_MMCEIGRSLOT, deviceIGRSlots);
    diaSetInt(diaMmceConfig, CFG_MMCEIGRSLOT, gMMCEIGRSlot);
    diaSetInt(diaMmceConfig, CFG_MMCEGAMEID, gMMCEEnableGameID);

    int ret;
reshow_mmce:
    ret = diaExecuteDialog(diaMmceConfig, -1, 1, NULL);
    if (ret == MMCE_COMM_BUTTON) {
        guiShowMmceCommConfig();
        goto reshow_mmce;
    }
    if (ret == MMCE_PATH_BUTTON) {
        guiShowMmcePathConfig();
        goto reshow_mmce;
    }
    if (ret) {
        diaGetInt(diaMmceConfig, CFG_MMCESLOT, &gMMCESlot);
        diaGetInt(diaMmceConfig, CFG_MMCEIGRSLOT, &gMMCEIGRSlot);
        diaGetInt(diaMmceConfig, CFG_MMCEGAMEID, &gMMCEEnableGameID);
        applyConfig(-1, -1, 0);
        menuReinitMainMenu();
    }
}

// MMCE -> Communication Settings (SIO2 ack-wait pacing, alarm usage).
void guiShowMmceCommConfig(void)
{
    const char *deviceAckWaitCycles[] = {"0", "1", "2", "3", "4", "5", NULL};
    const char *deviceOnOff[] = {"OFF", "ON", NULL};

    diaSetEnum(diaMmceCommConfig, CFG_MMCE_WAIT_CYCLES, deviceAckWaitCycles);
    diaSetInt(diaMmceCommConfig, CFG_MMCE_WAIT_CYCLES, gMMCEAckWaitCycles);
    diaSetEnum(diaMmceCommConfig, CFG_MMCE_USE_ALARMS, deviceOnOff);
    diaSetInt(diaMmceCommConfig, CFG_MMCE_USE_ALARMS, gMMCEUseAlarms);

    int ret = diaExecuteDialog(diaMmceCommConfig, -1, 1, NULL);
    if (ret) {
        diaGetInt(diaMmceCommConfig, CFG_MMCE_WAIT_CYCLES, &gMMCEAckWaitCycles);
        diaGetInt(diaMmceCommConfig, CFG_MMCE_USE_ALARMS, &gMMCEUseAlarms);
        applyConfig(-1, -1, 0);
        menuReinitMainMenu();
    }
}

// MMCE -> Path Settings (MMCE library prefix).
void guiShowMmcePathConfig(void)
{
    diaSetString(diaMmcePathConfig, CFG_MMCEPREFIX, gMMCEPrefix);

    int ret = diaExecuteDialog(diaMmcePathConfig, -1, 1, NULL);
    if (ret) {
        diaGetString(diaMmcePathConfig, CFG_MMCEPREFIX, gMMCEPrefix, sizeof(gMMCEPrefix));
        applyConfig(-1, -1, 0);
        menuReinitMainMenu();
    }
}

// Display page live-updater: the geometry/aspect rows apply live so the user sees the change as
// they adjust it (moved out of the old guiUIUpdater).
static int guiDisplayUpdater(int modified)
{
    if (modified) {
        int temp, x, y;
        diaGetInt(diaDisplayConfig, UICFG_XOFF, &x);
        diaGetInt(diaDisplayConfig, UICFG_YOFF, &y);
        if ((x != gXOff) || (y != gYOff)) {
            gXOff = x;
            gYOff = y;
            rmSetDisplayOffset(x, y);
        }
        diaGetInt(diaDisplayConfig, UICFG_OVERSCAN, &temp);
        if (temp != gOverscan) {
            gOverscan = temp;
            rmSetOverscan(gOverscan);
            guiUpdateScreenScale();
        }
        diaGetInt(diaDisplayConfig, UICFG_WIDESCREEN, &temp);
        if (temp != gWideScreen) {
            gWideScreen = temp;
            rmSetAspectRatio((gWideScreen == 0) ? RM_ARATIO_4_3 : RM_ARATIO_16_9);
            guiUpdateScreenScale();
        }
    }

    return 0;
}

// Deep-copy a NULL-terminated name list for handing to diaSetEnum. The CALLER must hold guiLock
// across BOTH the getter fetch (thmGetGuiList/lngGetGuiList) and this copy: the source lists live
// on the heap and are freed+rebuilt under guiLock on the IO worker (thmRebuildGuiNames/
// lngRebuildLangNames; thmReinit also frees the theme NAME strings on device removal), and taking
// the lock only inside this function left a preemption window between fetching the pointer and
// locking (Gemini review of #165). Returns NULL on OOM (caller falls back to the live list).
static const char **guiCopyNameList(const char **src)
{
    int n = 0, i;
    const char **copy;

    if (src == NULL)
        return NULL;

    while (src[n] != NULL)
        n++;
    copy = (const char **)malloc((n + 1) * sizeof(char *));
    if (copy != NULL) {
        for (i = 0; i < n; i++) {
            char *dup = (char *)malloc(strlen(src[i]) + 1);
            if (dup == NULL) {
                while (--i >= 0)
                    free((void *)copy[i]);
                free((void *)copy);
                copy = NULL;
                break;
            }
            strcpy(dup, src[i]);
            copy[i] = dup;
        }
        if (copy != NULL)
            copy[n] = NULL;
    }
    return copy;
}

static void guiFreeNameList(const char **list)
{
    int i;

    if (list == NULL)
        return;
    for (i = 0; list[i] != NULL; i++)
        free((void *)list[i]);
    free((void *)list);
}

// Interface page (settings-layout restructure, was Display Settings): theme, language and game-list
// behavior. Artwork, Coverflow and Colors are chained sub-dialogs; the video/geometry/GameID-barcode
// rows moved to the Display page (guiShowDisplayConfig).
void guiShowUIConfig(void)
{
    int themeID = -1, langID = -1;
    showCfgPopup = 0;
    guiResetNotifications();

    const char **themeNamesSnap = NULL;
    const char **langNamesSnap = NULL;

    int previousTheme;

reshow_ui:
    previousTheme = thmGetGuiValue();
    // Snapshot the theme/language name lists into dialog-owned copies: diaSetEnum stores raw
    // pointers, and BOTH the outer arrays (thmRebuildGuiNames/lngRebuildLangNames free+realloc)
    // AND the theme name strings (thmReinit frees them on device removal) are rebuilt on the IO
    // worker when a deferred device update lands. Dialog frames render OUTSIDE guiStartFrame's
    // lock, so a device event draining mid-dialog dereferenced freed memory. Re-snapshot on every
    // reshow pass (applyConfig below can legitimately change the lists); on OOM fall back to
    // the live list -- the pre-existing narrow race, not a new failure mode.
    guiFreeNameList(themeNamesSnap);
    guiFreeNameList(langNamesSnap);
    guiLock(); // must cover the getter FETCH too, not just the copy (Gemini review of #165)
    themeNamesSnap = guiCopyNameList((const char **)thmGetGuiList());
    langNamesSnap = guiCopyNameList((const char **)lngGetGuiList());
    guiUnlock();
    diaSetEnum(diaUIConfig, UICFG_THEME, themeNamesSnap != NULL ? themeNamesSnap : (const char **)thmGetGuiList());
    diaSetEnum(diaUIConfig, UICFG_LANG, langNamesSnap != NULL ? langNamesSnap : (const char **)lngGetGuiList());
    diaSetInt(diaUIConfig, UICFG_THEME, thmGetGuiValue());
    diaSetInt(diaUIConfig, UICFG_LANG, lngGetGuiValue());
    diaSetInt(diaUIConfig, UICFG_AUTOSORT, gAutosort);
    diaSetInt(diaUIConfig, UICFG_AUTOREFRESH, gAutoRefresh);
    diaSetInt(diaUIConfig, UICFG_NOTIFICATIONS, gEnableNotifications);
    diaSetVisible(diaUIConfig, UICFG_COVERFLOW_BUTTON, gTheme->coverflow != NULL);
    const char *gameViewNames[] = {"Both", "ISO", "VCD", NULL};
    diaSetEnum(diaUIConfig, UICFG_GAMEVIEW, gameViewNames);
    diaSetInt(diaUIConfig, UICFG_GAMEVIEW, gDefaultGameView);

    int ret = diaExecuteDialog(diaUIConfig, -1, 1, NULL);

    if (ret == UICFG_ARTWORK_BUTTON) {
        guiShowArtworkConfig();
        goto reshow_ui;
    }
    if (ret == UICFG_COVERFLOW_BUTTON) {
        guiShowCoverflowConfig();
        goto reshow_ui;
    }
    if (ret == UICFG_COLORS_BUTTON) {
        guiShowColorsConfig();
        goto reshow_ui;
    }

    // Play out the confirm bump the dialog just armed, before applyConfig() below tears down and
    // rebuilds the GS (rmSetMode), reloads the theme and its textures, and holds guiLock over a
    // submenu-cache rebuild -- none of which polls readPads(), so the pulse would run for all of it
    // (#172, "really intense ... after changing the resolution"). Deliberately HERE and not at the top
    // of applyConfig(): applyConfig is ALSO reached off the GUI thread, from _loadConfig() on the IO
    // worker (opl.c: guiHandleDeferedIO with IO_CUSTOM_SIMPLEACTION), and every libpad call in pad.c
    // is documented GUI-thread-only.
    padRumbleFlush();

    if (ret) {
        diaGetInt(diaUIConfig, UICFG_LANG, &langID);
        diaGetInt(diaUIConfig, UICFG_THEME, &themeID);
        diaGetInt(diaUIConfig, UICFG_AUTOSORT, &gAutosort);
        diaGetInt(diaUIConfig, UICFG_AUTOREFRESH, &gAutoRefresh);
        diaGetInt(diaUIConfig, UICFG_NOTIFICATIONS, &gEnableNotifications);
        int previousGameView = gDefaultGameView;
        diaGetInt(diaUIConfig, UICFG_GAMEVIEW, &gDefaultGameView);
        int gameViewChanged = gDefaultGameView != previousGameView;
        if (gameViewChanged) {
            vcdMarkAllDirty(); // rebuild every VCD-capable page so the new default view takes effect
        }

        if (previousTheme != themeID && isBgmPlaying())
            bgmStop();

        applyConfig(themeID, langID, 1);
        if (gameViewChanged) {
            // applyConfig(..., skipDeviceRefresh=1) deliberately avoids device scans. Queue after it
            // returns so HDD (which has no automatic refresh) cannot retain PS2 rows while rendering
            // uses the new VCD view, without racing the theme/menu bookkeeping above.
            oplQueueVcdDeviceUpdates();
            loadFavourites(); // queued after source pages so the FAV resolver sees their rebuilt rows
        }
        sfxInit(0);

        if (gEnableBGM && !isBgmPlaying())
            bgmStart();
    }

    guiFreeNameList(themeNamesSnap);
    guiFreeNameList(langNamesSnap);
}

static const char *artDelayNames[] = {"0 (Instant)", "2 (Fast)", "5 (Medium)", "8 (Standard)", NULL};
static const int artDelayValues[] = {0, 2, 5, 8};

static int artDelayToEnum(int delay)
{
    if (delay <= 0)
        return 0;
    if (delay <= 3)
        return 1;
    if (delay <= 6)
        return 2;
    return 3;
}

// Interface -> Artwork Settings (cover art display + ART.TAR loading).
void guiShowArtworkConfig(void)
{
    diaSetInt(diaArtworkConfig, UICFG_COVERART, gEnableArt);
    diaSetInt(diaArtworkConfig, UICFG_ENABLE_BGART, gEnableBGArt);
    diaSetInt(diaArtworkConfig, UICFG_ENABLE_ART_TAR, gEnableArtTar);
    diaSetEnum(diaArtworkConfig, UICFG_ART_DELAY, artDelayNames);
    diaSetInt(diaArtworkConfig, UICFG_ART_DELAY, artDelayToEnum(gArtDelay));

    int ret = diaExecuteDialog(diaArtworkConfig, -1, 1, NULL);
    if (ret) {
        diaGetInt(diaArtworkConfig, UICFG_COVERART, &gEnableArt);
        diaGetInt(diaArtworkConfig, UICFG_ENABLE_BGART, &gEnableBGArt);
        {
            // Re-arm the .tar probe when the toggle actually flips. tarFind's "no archive anywhere"
            // latch is write-once and process-wide, and NOTHING was clearing it (tarInvalidate had no
            // callers at all) -- so a user who turned the loader on after boot kept getting nothing
            // until a reboot, which looks exactly like "the toggle does nothing".
            int previousArtTar = gEnableArtTar;
            diaGetInt(diaArtworkConfig, UICFG_ENABLE_ART_TAR, &gEnableArtTar);
            if (gEnableArtTar != previousArtTar)
                tarInvalidate(TAR_KIND_ART);
        }
        int artDelayIdx = 0;
        diaGetInt(diaArtworkConfig, UICFG_ART_DELAY, &artDelayIdx);
        if (artDelayIdx >= 0 && artDelayIdx < 4) {
            gArtDelay = artDelayValues[artDelayIdx];
            item_list_t *lists[] = {appGetObject(1), hddGetObject(1), ethGetObject(1), mmceGetObject(1), udpfsGetObject(1), favGetObject(1)};
            for (int i = 0; i < 6; i++) {
                if (lists[i] != NULL)
                    lists[i]->delay = gArtDelay;
            }
        }

        applyConfig(-1, -1, 1);
    }
}

// Interface -> Colors. Only the DEFAULT theme's colours are user-adjustable -- with any other theme
// active the page shows that theme's own colours read-only (the old layout tied this to the theme
// picker in the same dialog; the picker lives on the Interface page now).
void guiShowColorsConfig(void)
{
    int themeID = thmGetGuiValue();
    int editable = (themeID == 0 || themeID == thmFindGuiID("<Coverflow>"));

    if (editable) {
        // Display the default theme's colours.
        diaSetColor(diaColorsConfig, UICFG_BGCOL, gDefaultBgColor);
        diaSetColor(diaColorsConfig, UICFG_UICOL, gDefaultUITextColor);
        diaSetColor(diaColorsConfig, UICFG_TXTCOL, gDefaultTextColor);
        diaSetColor(diaColorsConfig, UICFG_SELCOL, gDefaultSelTextColor);
        diaSetColor(diaColorsConfig, UICFG_PLASCOL, gDefaultPlasBlendColor);
    } else {
        // Display the current theme's colours (read-only).
        diaSetColor(diaColorsConfig, UICFG_BGCOL, gTheme->bgColor);
        diaSetColor(diaColorsConfig, UICFG_PLASCOL, gTheme->plasBlendColor); // raw uchar[3] like bgColor -- NOT the U64 form
        diaSetU64Color(diaColorsConfig, UICFG_UICOL, gTheme->uiTextColor);
        diaSetU64Color(diaColorsConfig, UICFG_TXTCOL, gTheme->textColor);
        diaSetU64Color(diaColorsConfig, UICFG_SELCOL, gTheme->selTextColor);
    }
    diaSetEnabled(diaColorsConfig, UICFG_BGCOL, editable);
    diaSetEnabled(diaColorsConfig, UICFG_UICOL, editable);
    diaSetEnabled(diaColorsConfig, UICFG_TXTCOL, editable);
    diaSetEnabled(diaColorsConfig, UICFG_SELCOL, editable);
    diaSetEnabled(diaColorsConfig, UICFG_PLASCOL, editable);
    diaSetEnabled(diaColorsConfig, UICFG_RESETCOL, editable);

    int ret = diaExecuteDialog(diaColorsConfig, -1, 1, NULL);
    if (ret) {
        if (editable) {
            diaGetColor(diaColorsConfig, UICFG_BGCOL, gDefaultBgColor);
            diaGetColor(diaColorsConfig, UICFG_UICOL, gDefaultUITextColor);
            diaGetColor(diaColorsConfig, UICFG_TXTCOL, gDefaultTextColor);
            diaGetColor(diaColorsConfig, UICFG_SELCOL, gDefaultSelTextColor);
            diaGetColor(diaColorsConfig, UICFG_PLASCOL, gDefaultPlasBlendColor);
        }
        if (ret == UICFG_RESETCOL)
            setDefaultColors();

        applyConfig(themeID, -1, 1);
    }
}

// Display page (settings-layout restructure): video mode, widescreen, offsets, overscan and the
// GameID barcode toggle. Geometry/aspect rows apply live (guiDisplayUpdater); a video-mode change
// keeps the confirm-or-revert flow from the old Display Settings.
void guiShowDisplayConfig(void)
{
    // clang-format off
    const char *vmodeNames[] = {_l(_STR_AUTO)
        , "PAL 640x512i @50Hz 24bit"
        , "NTSC 640x448i @60Hz 24bit"
        , "EDTV 640x448p @60Hz 24bit"
        , "EDTV 640x512p @50Hz 24bit"
        , "VGA 640x480p @60Hz 24bit"
        , "PAL 704x576i @50Hz 24bit (HIRES)"
        , "NTSC 704x480i @60Hz 24bit (HIRES)"
        , "EDTV 704x480p @60Hz 24bit (HIRES)"
        , "EDTV 704x576p @50Hz 24bit (HIRES)"
        , "HDTV 1280x720p @60Hz 16bit (HIRES)"
        , "HDTV 1920x1080i @60Hz 16bit (HIRES)"
        , "PAL 640x256p @50Hz 24bit"
        , "NTSC 640x224p @60Hz 24bit"
        , NULL};
    // clang-format on
    int previousVMode;

reselect_video_mode:
    previousVMode = gVMode;
    diaSetEnum(diaDisplayConfig, UICFG_VMODE, vmodeNames);
    diaSetInt(diaDisplayConfig, UICFG_VMODE, gVMode);
    diaSetInt(diaDisplayConfig, UICFG_WIDESCREEN, gWideScreen);
    diaSetInt(diaDisplayConfig, UICFG_XOFF, gXOff);
    diaSetInt(diaDisplayConfig, UICFG_YOFF, gYOff);
    diaSetInt(diaDisplayConfig, UICFG_OVERSCAN, gOverscan);
    diaSetInt(diaDisplayConfig, CFG_APPLYGAMEID, gApplyGameID); // RetroGEM/Pixel FX GameID barcode

    int ret = diaExecuteDialog(diaDisplayConfig, -1, 1, guiDisplayUpdater);

    // Same padRumbleFlush rationale as the Interface page: flush the confirm bump before
    // applyConfig() tears down and rebuilds the GS without polling readPads() (#172).
    padRumbleFlush();

    if (ret) {
        diaGetInt(diaDisplayConfig, UICFG_VMODE, &gVMode);
        diaGetInt(diaDisplayConfig, UICFG_WIDESCREEN, &gWideScreen);
        diaGetInt(diaDisplayConfig, UICFG_XOFF, &gXOff);
        diaGetInt(diaDisplayConfig, UICFG_YOFF, &gYOff);
        diaGetInt(diaDisplayConfig, UICFG_OVERSCAN, &gOverscan);
        diaGetInt(diaDisplayConfig, CFG_APPLYGAMEID, &gApplyGameID);

        applyConfig(-1, -1, 1);
    }

    if (previousVMode != gVMode) {
        if (guiConfirmVideoMode() == 0) {
            // Restore previous video mode.
            gVMode = previousVMode;
            applyConfig(-1, -1, 1);
            goto reselect_video_mode;
        }
    }
}

static int netConfigUpdater(int modified)
{
    int showAdvancedOptions, isNetBIOS, isDHCPEnabled, netProto, i;

    if (modified) {
        diaGetInt(diaNetConfig, NETCFG_SHOW_ADVANCED_OPTS, &showAdvancedOptions);

        diaGetInt(diaNetConfig, NETCFG_PS2_IP_ADDR_TYPE, &isDHCPEnabled);
        diaGetInt(diaNetConfig, NETCFG_SHARE_ADDR_TYPE, &isNetBIOS);
        diaGetInt(diaNetConfig, CFG_NETPROTOCOL, &netProto);
        // The SMB-server block is always shown (it is its own configuration, not tied to the active
        // protocol -- hiding it made users think fields were removed). The NetBIOS-vs-IP toggle still
        // picks which address widget shows.
        diaSetVisible(diaNetConfig, NETCFG_SHARE_NB_ADDR, isNetBIOS);

        for (i = 0; i < 4; i++) {
            diaSetVisible(diaNetConfig, NETCFG_SHARE_IP_ADDR_0 + i, !isNetBIOS);

            diaSetEnabled(diaNetConfig, NETCFG_PS2_IP_ADDR_0 + i, !isDHCPEnabled);
            diaSetEnabled(diaNetConfig, NETCFG_PS2_NETMASK_0 + i, !isDHCPEnabled);
            diaSetEnabled(diaNetConfig, NETCFG_PS2_GATEWAY_0 + i, !isDHCPEnabled);
            diaSetEnabled(diaNetConfig, NETCFG_PS2_DNS_0 + i, !isDHCPEnabled);
        }

        for (i = 0; i < 3; i++)
            diaSetVisible(diaNetConfig, NETCFG_SHARE_IP_ADDR_DOT_0 + i, !isNetBIOS);

        diaSetEnabled(diaNetConfig, NETCFG_SHARE_PORT, showAdvancedOptions);
        diaSetEnabled(diaNetConfig, NETCFG_ETHOPMODE, showAdvancedOptions);

        // Protocol (moved from Game Sources): lock Access to Files for SMB and to IMG for UDPBD
        // (only UDPFS offers the free toggle) -- snap the value so a stale IMG left over from UDPFS
        // can never mis-derive to UDPFSBD under SMB, AND grey the control so the lock is visible.
        // SMB Version qualifies SMB: live only when SMB is the selected protocol. Value is NOT
        // snapped when greyed -- a stale dialect cannot mis-derive anything (the OK read-back
        // ignores it unless protocol == SMB), and preserving it means flipping away to UDPFS and
        // back does not silently reset the user to SMBv1.
        diaSetEnabled(diaNetConfig, CFG_SMBDIALECT, netProto == 0);
        if (netProto == 0) { // SMB -> Files, locked
            diaSetInt(diaNetConfig, CFG_UDPFSMODE, 0);
            diaSetEnabled(diaNetConfig, CFG_UDPFSMODE, 0);
        } else if (netProto == 2) { // UDPBD -> IMG, locked
            diaSetInt(diaNetConfig, CFG_UDPFSMODE, 1);
            diaSetEnabled(diaNetConfig, CFG_UDPFSMODE, 0);
        } else { // UDPFS -> Files/IMG free
            diaSetEnabled(diaNetConfig, CFG_UDPFSMODE, 1);
        }
    }

    return 0;
}

void guiShowNetConfig(void)
{
    size_t i;
    const char *ethOpModes[] = {_l(_STR_AUTO), _l(_STR_ETH_100MFDX), _l(_STR_ETH_100MHDX), _l(_STR_ETH_10MFDX), _l(_STR_ETH_10MHDX), NULL};
    const char *addrConfModes[] = {_l(_STR_ADDR_TYPE_IP), _l(_STR_ADDR_TYPE_NETBIOS), NULL};
    const char *ipAddrConfModes[] = {_l(_STR_IP_ADDRESS_TYPE_STATIC), _l(_STR_IP_ADDRESS_TYPE_DHCP), NULL};
    const char *netProtocols[] = {"SMB", "UDPFS", "UDPBD", NULL}; // UDPBD = SUDPBDv2 server -- protocol names, not translated
    const char *udpfsModes[] = {"Files", "IMG", NULL};            // Access: Files=udpfs_ioman filesystem, IMG=udpfs_bd block
    // SMB version row. SMB3 is intentionally absent: it mandates packet signing, which does not
    // exist in this tree yet, so offering it would be a picker that silently does something else.
    // Index order matches enum SMB_DIALECT. Protocol names, not translated -- same as the rows above.
    const char *smbDialects[] = {"SMBv1", "SMB2", NULL};
    diaSetEnum(diaNetConfig, NETCFG_PS2_IP_ADDR_TYPE, ipAddrConfModes);
    diaSetEnum(diaNetConfig, NETCFG_SHARE_ADDR_TYPE, addrConfModes);
    diaSetEnum(diaNetConfig, NETCFG_ETHOPMODE, ethOpModes);
    diaSetEnum(diaNetConfig, CFG_NETPROTOCOL, netProtocols);
    diaSetEnum(diaNetConfig, CFG_UDPFSMODE, udpfsModes);
    diaSetEnum(diaNetConfig, CFG_SMBDIALECT, smbDialects);

    // upload current values
    // RiptOPL: open the Network Config with advanced options ON so the SMB Port + ETH op-mode are
    // immediately editable (was OFF -> both fields greyed until the user toggled advanced on).
    diaSetInt(diaNetConfig, NETCFG_SHOW_ADVANCED_OPTS, 1);
    diaSetEnabled(diaNetConfig, NETCFG_ETHOPMODE, 1);
    diaSetEnabled(diaNetConfig, NETCFG_SHARE_PORT, 1);

    // The SMB-server block (address/port/share/user/password) is ALWAYS shown. It is its own
    // configuration, independent of the currently-selected network protocol -- gating it on the
    // protocol made users think the fields had been deleted (and created a chicken-and-egg: you had to
    // select SMB elsewhere before you could even see where to configure it). The PS2 IP block + ETH
    // op-mode above are likewise always shown; the UDP transports need the static PS2 IP + link mode.
    diaSetVisible(diaNetConfig, NETCFG_LBL_SMB_SERVER, 1);
    diaSetVisible(diaNetConfig, NETCFG_LBL_SHARE_ADDR_TYPE, 1);
    diaSetVisible(diaNetConfig, NETCFG_SHARE_ADDR_TYPE, 1);
    diaSetVisible(diaNetConfig, NETCFG_LBL_SHARE_ADDRESS, 1);
    diaSetVisible(diaNetConfig, NETCFG_LBL_SHARE_PORT, 1);
    diaSetVisible(diaNetConfig, NETCFG_SHARE_PORT, 1);
    diaSetVisible(diaNetConfig, NETCFG_LBL_SHARE_NAME, 1);
    diaSetVisible(diaNetConfig, NETCFG_SHARE_NAME, 1);
    diaSetVisible(diaNetConfig, NETCFG_LBL_SHARE_USER, 1);
    diaSetVisible(diaNetConfig, NETCFG_SHARE_USERNAME, 1);
    diaSetVisible(diaNetConfig, NETCFG_LBL_SHARE_PASSWORD, 1);
    diaSetVisible(diaNetConfig, NETCFG_SHARE_PASSWORD, 1);

    diaSetInt(diaNetConfig, NETCFG_PS2_IP_ADDR_TYPE, ps2_ip_use_dhcp);
    diaSetInt(diaNetConfig, NETCFG_SHARE_ADDR_TYPE, gPCShareAddressIsNetBIOS);
    diaSetVisible(diaNetConfig, NETCFG_SHARE_NB_ADDR, gPCShareAddressIsNetBIOS);
    diaSetInt(diaNetConfig, NETCFG_SHARE_NB_ADDR, gPCShareAddressIsNetBIOS);
    diaSetString(diaNetConfig, NETCFG_SHARE_NB_ADDR, gPCShareNBAddress);

    for (i = 0; i < 4; ++i) {
        diaSetEnabled(diaNetConfig, NETCFG_PS2_IP_ADDR_0 + i, !ps2_ip_use_dhcp);
        diaSetEnabled(diaNetConfig, NETCFG_PS2_NETMASK_0 + i, !ps2_ip_use_dhcp);
        diaSetEnabled(diaNetConfig, NETCFG_PS2_GATEWAY_0 + i, !ps2_ip_use_dhcp);
        diaSetEnabled(diaNetConfig, NETCFG_PS2_DNS_0 + i, !ps2_ip_use_dhcp);

        diaSetVisible(diaNetConfig, NETCFG_SHARE_IP_ADDR_0 + i, !gPCShareAddressIsNetBIOS);
        diaSetInt(diaNetConfig, NETCFG_PS2_IP_ADDR_0 + i, ps2_ip[i]);
        diaSetInt(diaNetConfig, NETCFG_PS2_NETMASK_0 + i, ps2_netmask[i]);
        diaSetInt(diaNetConfig, NETCFG_PS2_GATEWAY_0 + i, ps2_gateway[i]);
        diaSetInt(diaNetConfig, NETCFG_PS2_DNS_0 + i, ps2_dns[i]);
        diaSetInt(diaNetConfig, NETCFG_SHARE_IP_ADDR_0 + i, pc_ip[i]);
    }

    for (i = 0; i < 3; ++i)
        diaSetVisible(diaNetConfig, NETCFG_SHARE_IP_ADDR_DOT_0 + i, !gPCShareAddressIsNetBIOS);

    diaSetInt(diaNetConfig, NETCFG_SHARE_PORT, gPCPort);
    diaSetString(diaNetConfig, NETCFG_SHARE_NAME, gPCShareName);
    diaSetString(diaNetConfig, NETCFG_SHARE_USERNAME, gPCUserName);
    diaSetString(diaNetConfig, NETCFG_SHARE_PASSWORD, gPCPassword);
    diaSetInt(diaNetConfig, NETCFG_ETHOPMODE, gETHOpMode);

    // Protocol rows (moved from Game Sources), seeded from the authoritative gNetworkProtocol.
    //   Protocol: SMB(0)/UDPFS(1)/UDPBD(2)  -- OFF and UDPFSBD both collapse to their protocol
    //   Access:   Files(0)/IMG(1); IMG only distinct for UDPFS (-> UDPFSBD backend)
    // A NET_PROTO_OFF backend has no protocol memory, so seed the protocol row to SMB -- the common
    // default a user reaches when they switch the Game Sources Start row from Off to Manual/Auto.
    int netProtoVal = (gNetworkProtocol == NET_PROTO_UDPBD)                                          ? 2 :
                      (gNetworkProtocol == NET_PROTO_UDPFS || gNetworkProtocol == NET_PROTO_UDPFSBD) ? 1 :
                                                                                                       0; // SMB / OFF
    // IMG for the udpfs block backend AND UDPBD (IMG-locked), so the seed already matches the lock.
    int netAccessVal = (gNetworkProtocol == NET_PROTO_UDPFSBD || gNetworkProtocol == NET_PROTO_UDPBD) ? 1 : 0;
    diaSetInt(diaNetConfig, CFG_NETPROTOCOL, netProtoVal);
    diaSetInt(diaNetConfig, CFG_UDPFSMODE, netAccessVal);
    diaSetInt(diaNetConfig, CFG_SMBDIALECT, gSMBDialect);
    // Seed the initial grey/lock: diaExecuteDialog renders the FIRST frame before it calls
    // netConfigUpdater, so without this the first frame flashes every row enabled.
    diaSetEnabled(diaNetConfig, CFG_UDPFSMODE, netProtoVal == 1);
    diaSetEnabled(diaNetConfig, CFG_SMBDIALECT, netProtoVal == 0);

    // Update the spacer item between the OK and reconnect buttons (See dialogs.c).
    if (gNetworkStartup == 0) {
        diaSetLabel(diaNetConfig, NETCFG_OK, _l(_STR_OK));
        diaSetVisible(diaNetConfig, NETCFG_RECONNECT, 1);
    } else if (gNetworkStartup >= ERROR_ETH_SMB_CONN) {
        diaSetLabel(diaNetConfig, NETCFG_OK, _l(_STR_RECONNECT));
        diaSetVisible(diaNetConfig, NETCFG_RECONNECT, 0);
    } else {
        diaSetLabel(diaNetConfig, NETCFG_OK, _l(_STR_OK));
        diaSetVisible(diaNetConfig, NETCFG_RECONNECT, 0);
    }

    int result = diaExecuteDialog(diaNetConfig, -1, 1, &netConfigUpdater);

    if (result) {
        // Store values
        diaGetInt(diaNetConfig, NETCFG_PS2_IP_ADDR_TYPE, &ps2_ip_use_dhcp);
        diaGetInt(diaNetConfig, NETCFG_SHARE_ADDR_TYPE, &gPCShareAddressIsNetBIOS);
        diaGetString(diaNetConfig, NETCFG_SHARE_NB_ADDR, gPCShareNBAddress, sizeof(gPCShareNBAddress));

        for (i = 0; i < 4; ++i) {
            diaGetInt(diaNetConfig, NETCFG_PS2_IP_ADDR_0 + i, &ps2_ip[i]);
            diaGetInt(diaNetConfig, NETCFG_PS2_NETMASK_0 + i, &ps2_netmask[i]);
            diaGetInt(diaNetConfig, NETCFG_PS2_GATEWAY_0 + i, &ps2_gateway[i]);
            diaGetInt(diaNetConfig, NETCFG_PS2_DNS_0 + i, &ps2_dns[i]);
            diaGetInt(diaNetConfig, NETCFG_SHARE_IP_ADDR_0 + i, &pc_ip[i]);
        }
        diaGetInt(diaNetConfig, NETCFG_ETHOPMODE, &gETHOpMode);

        diaGetInt(diaNetConfig, NETCFG_SHARE_PORT, &gPCPort);
        diaGetString(diaNetConfig, NETCFG_SHARE_NAME, gPCShareName, sizeof(gPCShareName));
        diaGetString(diaNetConfig, NETCFG_SHARE_USERNAME, gPCUserName, sizeof(gPCUserName));
        diaGetString(diaNetConfig, NETCFG_SHARE_PASSWORD, gPCPassword, sizeof(gPCPassword));

        // Fold the Protocol/Access rows back into the authoritative gNetworkProtocol. PROTOCOL-FIRST:
        // the Access value is consulted ONLY for UDPFS, so a stale IMG under SMB/UDPBD can never
        // mis-derive. The Game Sources "Network Start Mode" row decides whether the stack runs at
        // all: Start=Off -> OFF regardless of the protocol picked here. Then re-derive the legacy
        // shadows (gEnableUDPBD / gNetBootProtocol / gETHStartMode) downstream consumers read.
        int netProtocolWas = gNetworkProtocol;
        int smbDialectWas = gSMBDialect;
        diaGetInt(diaNetConfig, CFG_NETPROTOCOL, &netProtoVal);
        diaGetInt(diaNetConfig, CFG_UDPFSMODE, &netAccessVal);
        // Read the dialect unconditionally -- it is a property of the SMB backend, not of the
        // current selection, so it survives a detour through another protocol. Everything
        // downstream consults it only when the live protocol is SMB.
        diaGetInt(diaNetConfig, CFG_SMBDIALECT, &gSMBDialect);
        gNetworkProtocol = (gNetStartMode == START_MODE_DISABLED) ? NET_PROTO_OFF :
                           (netProtoVal == 0)                     ? NET_PROTO_SMB :
                           (netProtoVal == 2)                     ? NET_PROTO_UDPBD :
                           (netAccessVal == 1)                    ? NET_PROTO_UDPFSBD :
                                                                    NET_PROTO_UDPFS; // UDPFS + Files
        gEnableUDPBD = (gNetworkProtocol == NET_PROTO_UDPBD || gNetworkProtocol == NET_PROTO_UDPFSBD);
        gNetBootProtocol = (gNetworkProtocol == NET_PROTO_UDPFSBD) ? NET_BOOT_UDPFS : NET_BOOT_UDPBD;
        // SMB's start mode IS the network start row (Auto = boot connect, Manual = on-entry);
        // every non-SMB protocol forces the SMB/ETH stack off so only one transport claims the NIC.
        gETHStartMode = (gNetworkProtocol == NET_PROTO_SMB) ? gNetStartMode : START_MODE_DISABLED;

        // The UDP transports' ministack has no DHCP client; with DHCP on, ps2_ip[] is never refreshed, so
        // they need a static PS2 IP. Warn when switching TO a UDP protocol (UDPFS/UDPFSBD/UDPBD) from a
        // non-UDP one while DHCP is on. SMB is exempt (it runs the full ETH stack that acquires a lease).
        int nowUdp = (gNetworkProtocol == NET_PROTO_UDPFS || gNetworkProtocol == NET_PROTO_UDPFSBD || gNetworkProtocol == NET_PROTO_UDPBD);
        int wasUdp = (netProtocolWas == NET_PROTO_UDPFS || netProtocolWas == NET_PROTO_UDPFSBD || netProtocolWas == NET_PROTO_UDPBD);
        if (nowUdp && !wasUdp && ps2_ip_use_dhcp)
            guiMsgBox(_l(_STR_UDPBD_NEEDS_STATIC_IP), 0, NULL);

        // "Nothing happens" guard (hardware report on Beta-2937): enabling a network protocol gave NO
        // feedback -- the UDPFS tab joins the ring silently and then waits for a Confirm-press inside
        // it (Manual start), and the block transports show a tab only once the PC server answers.
        // Tell the user what to expect + which PC server to run, right at the moment they turn it on.
        // Shown BEFORE the restart notice below (PR #78 review): when a restart is pending, the restart
        // message must be the LAST word so the guidance reads as "after that". Suppressing the hint in
        // the restart case instead would lose it forever -- no protocol-CHANGE event fires on the next
        // boot, which is exactly the silent-enable hole this guards against.
        if (gNetworkProtocol != netProtocolWas) {
            if (gNetworkProtocol == NET_PROTO_UDPFS)
                guiMsgBox(_l(_STR_NET_UDPFS_TAB_HINT), 0, NULL);
            else if (gNetworkProtocol == NET_PROTO_UDPFSBD || gNetworkProtocol == NET_PROTO_UDPBD)
                guiMsgBox(_l(_STR_NET_UDPBD_TAB_HINT), 0, NULL);
        }

        /*
          Turning SMB2 on changes BOTH SMB paths, so say so: browsing switches from the stock
          SMBv1 smbman.irx to smb2man (ethsupport.c picks by gSMBDialect), and the in-game reader
          switches to smb2.c. The whole session is SMB2 -- the server does not need SMBv1 at all.

          Worth telling the user because the dialect only takes effect on the next boot, and
          because a server that ONLY speaks SMBv1 (including RiptOPL's own bundled PC server tool)
          will stop listing games entirely under this setting -- which would otherwise look like a
          regression rather than a configuration mismatch.

          English literal rather than a lang label, matching the untranslated row labels in this
          same dialog ("Protocol", "Access", "SMB Version") and avoiding an insert into the
          position-indexed .lng tables.
        */
        if (gNetworkProtocol == NET_PROTO_SMB && gSMBDialect == SMB_DIALECT_SMB2 && gSMBDialect != smbDialectWas)
            guiMsgBox("SMB2 applies to browsing AND in-game reading.\n"
                      "Your server must speak SMB2 -- an SMBv1-only\n"
                      "server will list no games under this setting.",
                      0, NULL);

        // Each network transport loads its IOP module chain once per boot (the load latch is not cleared
        // live). If any network stack is already up and the user changed protocol, the switch takes effect
        // only after a restart -- say so instead of silently doing nothing.
        // Changing the SMB dialect has the same "takes effect next boot" property as changing the
        // protocol: the in-game reader picks its dialect when cdvdman starts, and the browse-side
        // SMB stack loads its module chain once per boot. Reuse the same notice so a user who
        // switches SMBv1 <-> SMB2 with the stack already up is not left wondering why nothing moved.
        if ((gNetworkProtocol != netProtocolWas ||
             (gNetworkProtocol == NET_PROTO_SMB && gSMBDialect != smbDialectWas)) &&
            (bdmIsUDPBDLoaded() || ethGetModulesLoaded() || udpfsGetModulesLoaded()))
            guiMsgBox(_l(_STR_NETBOOT_RESTART), 0, NULL);

        if (result == NETCFG_RECONNECT && gNetworkStartup < ERROR_ETH_SMB_CONN)
            gNetworkStartup = ERROR_ETH_SMB_LOGON;

        applyConfig(-1, -1, 0);
    }
}

// POPStarter -> Network Settings live-updater: DHCP = no static IP/mask/gateway triple.
static int guiPopsNetUpdater(int modified)
{
    int isPopsDhcp, i;

    if (modified) {
        diaGetInt(diaPopsNetConfig, NETCFG_POPS_IPTYPE, &isPopsDhcp);
        for (i = 0; i < 4; i++) {
            diaSetEnabled(diaPopsNetConfig, NETCFG_POPS_IP_0 + i, !isPopsDhcp);
            diaSetEnabled(diaPopsNetConfig, NETCFG_POPS_MASK_0 + i, !isPopsDhcp);
            diaSetEnabled(diaPopsNetConfig, NETCFG_POPS_GW_0 + i, !isPopsDhcp);
        }
    }

    return 0;
}

// POPStarter -> Network Settings (VCD over SMB). Shows POPSTARTER's OWN IPCONFIG.DAT /
// SMBCONFIG.DAT contents. Absent files leave the fields blank with the notice visible -- absence
// means unknown/unconfigured, NEVER "use OPL's values". Only an explicit edit or the Import button
// fills them, and only an actual change vs the read-time snapshot is written back on OK (the save
// matrix below). Moved out of guiShowNetConfig by the settings-layout restructure; the Import
// source is now the SAVED OPL network globals (the OPL fields no longer share this dialog).
void guiShowPopsNetConfig(void)
{
    size_t i;
    const char *ipAddrConfModes[] = {_l(_STR_IP_ADDRESS_TYPE_STATIC), _l(_STR_IP_ADDRESS_TYPE_DHCP), NULL};
    // POPStarter read-time snapshot for the change-detection save matrix, + static storage for the
    // "Loaded from %s" notice (diaSetLabel stores a RAW POINTER -- it must outlive the dialog).
    static vcd_popsnet_t popsOriginal;
    static char popsNotice[128];
    diaSetEnum(diaPopsNetConfig, NETCFG_POPS_IPTYPE, ipAddrConfModes); // same Static/DHCP index convention

    vcdReadPopstarterNet(&popsOriginal);
    diaSetInt(diaPopsNetConfig, NETCFG_POPS_IPTYPE, popsOriginal.ipDhcp);
    for (i = 0; i < 4; ++i) {
        diaSetInt(diaPopsNetConfig, NETCFG_POPS_IP_0 + i, popsOriginal.ps2Ip[i]);
        diaSetInt(diaPopsNetConfig, NETCFG_POPS_MASK_0 + i, popsOriginal.ps2Mask[i]);
        diaSetInt(diaPopsNetConfig, NETCFG_POPS_GW_0 + i, popsOriginal.ps2Gw[i]);
        diaSetInt(diaPopsNetConfig, NETCFG_POPS_SMB_IP_0 + i, popsOriginal.smbIp[i]);

        diaSetEnabled(diaPopsNetConfig, NETCFG_POPS_IP_0 + i, !popsOriginal.ipDhcp);
        diaSetEnabled(diaPopsNetConfig, NETCFG_POPS_MASK_0 + i, !popsOriginal.ipDhcp);
        diaSetEnabled(diaPopsNetConfig, NETCFG_POPS_GW_0 + i, !popsOriginal.ipDhcp);
    }
    diaSetInt(diaPopsNetConfig, NETCFG_POPS_SMB_PORT, popsOriginal.smbPort);
    diaSetString(diaPopsNetConfig, NETCFG_POPS_SMB_SHARE, popsOriginal.smbShare);
    diaSetString(diaPopsNetConfig, NETCFG_POPS_SMB_USER, popsOriginal.smbUser);
    diaSetString(diaPopsNetConfig, NETCFG_POPS_SMB_PASS, popsOriginal.smbPass);

    if (popsOriginal.smbExists || popsOriginal.ipExists) {
        snprintf(popsNotice, sizeof(popsNotice), _l(_STR_POPS_LOADED_FROM),
                 popsOriginal.smbExists ? popsOriginal.smbDir : popsOriginal.ipDir);
        diaSetLabel(diaPopsNetConfig, NETCFG_POPS_NOTICE, popsNotice);
    } else {
        diaSetLabel(diaPopsNetConfig, NETCFG_POPS_NOTICE, _l(_STR_POPS_NONE_DETECTED));
    }

    int result;
    do {
        result = diaExecuteDialog(diaPopsNetConfig, -1, 1, &guiPopsNetUpdater);
        if (result == NETCFG_POPS_IMPORT) {
            // IMPORT: the ONE sanctioned path that copies OPL's saved Network Settings into the
            // POPStarter fields. Pressing the button exits the dialog with its id; the dialog
            // struct keeps every field's value across the re-execution, so nothing the user typed
            // elsewhere is lost.
            char s[32];

            diaSetInt(diaPopsNetConfig, NETCFG_POPS_IPTYPE, ps2_ip_use_dhcp);
            for (i = 0; i < 4; ++i) {
                diaSetInt(diaPopsNetConfig, NETCFG_POPS_IP_0 + i, ps2_ip[i]);
                diaSetInt(diaPopsNetConfig, NETCFG_POPS_MASK_0 + i, ps2_netmask[i]);
                diaSetInt(diaPopsNetConfig, NETCFG_POPS_GW_0 + i, ps2_gateway[i]);

                diaSetEnabled(diaPopsNetConfig, NETCFG_POPS_IP_0 + i, !ps2_ip_use_dhcp);
                diaSetEnabled(diaPopsNetConfig, NETCFG_POPS_MASK_0 + i, !ps2_ip_use_dhcp);
                diaSetEnabled(diaPopsNetConfig, NETCFG_POPS_GW_0 + i, !ps2_ip_use_dhcp);
            }

            // A NetBIOS share name can't be turned into an IP -- leave the POPStarter server IP
            // untouched in that case instead of importing a wrong value.
            if (!gPCShareAddressIsNetBIOS) {
                for (i = 0; i < 4; ++i)
                    diaSetInt(diaPopsNetConfig, NETCFG_POPS_SMB_IP_0 + i, pc_ip[i]);
            }
            diaSetInt(diaPopsNetConfig, NETCFG_POPS_SMB_PORT, gPCPort);
            snprintf(s, sizeof(s), "%s", gPCShareName);
            diaSetString(diaPopsNetConfig, NETCFG_POPS_SMB_SHARE, s);
            snprintf(s, sizeof(s), "%s", gPCUserName);
            diaSetString(diaPopsNetConfig, NETCFG_POPS_SMB_USER, s);
            snprintf(s, sizeof(s), "%s", gPCPassword);
            diaSetString(diaPopsNetConfig, NETCFG_POPS_SMB_PASS, s);
        }
    } while (result == NETCFG_POPS_IMPORT);

    if (result) {
        // POPStarter save matrix (POPSLoader parity): compare the dialog's POPStarter fields against
        // the read-time snapshot and write ONLY what actually changed -- and only when the file
        // already exists (overwrite at its origin dir) or the user supplied complete values (create
        // at createDir). Blank/incomplete fields with no existing file create NOTHING: absence must
        // never turn into a fabricated configuration.
        vcd_popsnet_t popsCur = popsOriginal; // carries the origin dirs + exists flags over
        diaGetInt(diaPopsNetConfig, NETCFG_POPS_IPTYPE, &popsCur.ipDhcp);
        for (i = 0; i < 4; ++i) {
            diaGetInt(diaPopsNetConfig, NETCFG_POPS_IP_0 + i, &popsCur.ps2Ip[i]);
            diaGetInt(diaPopsNetConfig, NETCFG_POPS_MASK_0 + i, &popsCur.ps2Mask[i]);
            diaGetInt(diaPopsNetConfig, NETCFG_POPS_GW_0 + i, &popsCur.ps2Gw[i]);
            diaGetInt(diaPopsNetConfig, NETCFG_POPS_SMB_IP_0 + i, &popsCur.smbIp[i]);
        }
        diaGetInt(diaPopsNetConfig, NETCFG_POPS_SMB_PORT, &popsCur.smbPort);
        diaGetString(diaPopsNetConfig, NETCFG_POPS_SMB_SHARE, popsCur.smbShare, sizeof(popsCur.smbShare));
        diaGetString(diaPopsNetConfig, NETCFG_POPS_SMB_USER, popsCur.smbUser, sizeof(popsCur.smbUser));
        diaGetString(diaPopsNetConfig, NETCFG_POPS_SMB_PASS, popsCur.smbPass, sizeof(popsCur.smbPass));

        int popsMask = vcdPopsNetChanged(&popsOriginal, &popsCur);
        int popsSmbComplete = popsCur.smbShare[0] != '\0' &&
                              (popsCur.smbIp[0] | popsCur.smbIp[1] | popsCur.smbIp[2] | popsCur.smbIp[3]) != 0;
        int popsIpComplete = (popsCur.ps2Ip[0] | popsCur.ps2Ip[1] | popsCur.ps2Ip[2] | popsCur.ps2Ip[3]) != 0 &&
                             (popsCur.ps2Mask[0] | popsCur.ps2Mask[1] | popsCur.ps2Mask[2] | popsCur.ps2Mask[3]) != 0 &&
                             (popsCur.ps2Gw[0] | popsCur.ps2Gw[1] | popsCur.ps2Gw[2] | popsCur.ps2Gw[3]) != 0;
        int writeSmb = (popsMask & 1) && (popsOriginal.smbExists || popsSmbComplete);
        int writeIp = (popsMask & 2) && (popsOriginal.ipExists || popsCur.ipDhcp || popsIpComplete);
        // Creating a fresh SMBCONFIG.DAT with no IPCONFIG.DAT anywhere: create the pair -- POPStarter
        // expects IPCONFIG.DAT to exist even when blank (DHCP). An incomplete static entry becomes a
        // BLANK file (never garbage); the user can complete it later and it will overwrite.
        if (writeSmb && !popsOriginal.smbExists && !popsOriginal.ipExists) {
            writeIp = 1;
            if (!popsCur.ipDhcp && !popsIpComplete)
                popsCur.ipDhcp = 1;
        }
        if ((writeSmb || writeIp) && vcdWritePopstarterNetFiles(&popsCur, writeSmb, writeIp) != 0)
            guiMsgBox(_l(_STR_POPSTARTER_NET_ERR), 0, NULL);
    }
}

// Game Sources page (settings-layout restructure, was Device Settings): device selection + start
// modes. The Protocol/Access/SMB-Version rows moved to the Network page (guiShowNetConfig); only
// the Network Start Mode row stays here.
void guiShowDeviceConfig(void)
{
    const char *deviceNames[] = {_l(_STR_BDM_GAMES), _l(_STR_NET_GAMES), _l(_STR_HDD_GAMES), _l(_STR_APPS), _l(_STR_MMCE), _l(_STR_FAV), NULL};
    const char *deviceModes[] = {_l(_STR_OFF), _l(_STR_MANUAL), _l(_STR_AUTO), NULL};

    // Devices & modes
    diaSetEnum(diaDeviceConfig, CFG_DEFDEVICE, deviceNames);
    diaSetEnum(diaDeviceConfig, CFG_BDMMODE, deviceModes);
    diaSetEnum(diaDeviceConfig, CFG_HDDMODE, deviceModes);
    diaSetEnum(diaDeviceConfig, CFG_APPMODE, deviceModes);
    diaSetEnum(diaDeviceConfig, CFG_FAVMODE, deviceModes);

    int deviceModeIndex = guiIoModeToDeviceType(gDefaultDevice);
    diaSetInt(diaDeviceConfig, CFG_DEFDEVICE, deviceModeIndex);
    diaSetInt(diaDeviceConfig, CFG_BDMMODE, gBDMStartMode);
    diaSetInt(diaDeviceConfig, CFG_HDDMODE, gHDDStartMode);
    diaSetInt(diaDeviceConfig, CFG_APPMODE, gAPPStartMode);
    diaSetInt(diaDeviceConfig, CFG_FAVMODE, gFAVStartMode);

    // Block devices (inlined; interlocked with the APA HDD mode)
    diaSetInt(diaDeviceConfig, CFG_ENABLEUSB, gEnableUSB);
    diaSetInt(diaDeviceConfig, CFG_ENABLEILK, gEnableILK);
    diaSetInt(diaDeviceConfig, CFG_ENABLEMX4SIO, gEnableMX4SIO);
    diaSetInt(diaDeviceConfig, CFG_ENABLEBDMHDD, gEnableBdmHDD);
    diaSetEnabled(diaDeviceConfig, CFG_ENABLEBDMHDD, 1); // coexists with APA (directive 2026-07-21)
    diaSetEnabled(diaDeviceConfig, CFG_HDDMODE, 1);
    // Network Start Mode (Off/Manual/Auto) == gNetStartMode (START_MODE_*); the SAME three options
    // (and indices) as every other device's start row, so reuse the localized deviceModes (Gemini
    // review of #199). Protocol/Access/SMB-Version live on the Network page now.
    diaSetEnum(diaDeviceConfig, CFG_NETSTART, deviceModes);
    diaSetInt(diaDeviceConfig, CFG_NETSTART, gNetStartMode);

    diaSetEnum(diaDeviceConfig, CFG_MMCEMODE, deviceModes);
    diaSetInt(diaDeviceConfig, CFG_MMCEMODE, gMMCEStartMode);

    int ret = diaExecuteDialog(diaDeviceConfig, -1, 1, NULL);
    if (ret) {
        int netProtocolWas = gNetworkProtocol;
        diaGetInt(diaDeviceConfig, CFG_DEFDEVICE, &deviceModeIndex);
        gDefaultDevice = guiDeviceTypeToIoMode(deviceModeIndex);
        diaGetInt(diaDeviceConfig, CFG_BDMMODE, &gBDMStartMode);
        diaGetInt(diaDeviceConfig, CFG_HDDMODE, &gHDDStartMode);
        diaGetInt(diaDeviceConfig, CFG_APPMODE, &gAPPStartMode);
        diaGetInt(diaDeviceConfig, CFG_FAVMODE, &gFAVStartMode);

        diaGetInt(diaDeviceConfig, CFG_ENABLEUSB, &gEnableUSB);
        diaGetInt(diaDeviceConfig, CFG_ENABLEILK, &gEnableILK);
        diaGetInt(diaDeviceConfig, CFG_ENABLEMX4SIO, &gEnableMX4SIO);
        diaGetInt(diaDeviceConfig, CFG_ENABLEBDMHDD, &gEnableBdmHDD);
        // Network Start Mode read-back: Start=Off forces the protocol OFF; switching Start back on
        // with no protocol memory (was OFF) lands on SMB, the common default. The protocol itself is
        // picked on the Network page. Then re-derive the legacy shadows downstream consumers read.
        diaGetInt(diaDeviceConfig, CFG_NETSTART, &gNetStartMode);
        if (gNetStartMode == START_MODE_DISABLED)
            gNetworkProtocol = NET_PROTO_OFF;
        else if (gNetworkProtocol == NET_PROTO_OFF)
            gNetworkProtocol = NET_PROTO_SMB;
        gEnableUDPBD = (gNetworkProtocol == NET_PROTO_UDPBD || gNetworkProtocol == NET_PROTO_UDPFSBD);
        gNetBootProtocol = (gNetworkProtocol == NET_PROTO_UDPFSBD) ? NET_BOOT_UDPFS : NET_BOOT_UDPBD;
        // SMB's start mode IS the network start row now (Auto = boot connect, Manual = on-entry);
        // every non-SMB protocol forces the SMB/ETH stack off so only one transport claims the NIC.
        gETHStartMode = (gNetworkProtocol == NET_PROTO_SMB) ? gNetStartMode : START_MODE_DISABLED;

        diaGetInt(diaDeviceConfig, CFG_MMCEMODE, &gMMCEStartMode);

        // "Nothing happens" guard (hardware report on Beta-2937): enabling a network protocol gave NO
        // feedback -- the UDPFS tab joins the ring silently and then waits for a Confirm-press inside
        // it (Manual start), and the block transports show a tab only once the PC server answers.
        if (gNetworkProtocol != netProtocolWas) {
            if (gNetworkProtocol == NET_PROTO_UDPFS)
                guiMsgBox(_l(_STR_NET_UDPFS_TAB_HINT), 0, NULL);
            else if (gNetworkProtocol == NET_PROTO_UDPFSBD || gNetworkProtocol == NET_PROTO_UDPBD)
                guiMsgBox(_l(_STR_NET_UDPBD_TAB_HINT), 0, NULL);
        }

        // Each network transport loads its IOP module chain once per boot (the load latch is not
        // cleared live). If any network stack is already up and the Start toggle changed the active
        // protocol, the switch takes effect only after a restart -- say so instead of silently
        // doing nothing.
        if (gNetworkProtocol != netProtocolWas &&
            (bdmIsUDPBDLoaded() || ethGetModulesLoaded() || udpfsGetModulesLoaded()))
            guiMsgBox(_l(_STR_NETBOOT_RESTART), 0, NULL);

        // A BDM tab can be latched hidden (bdmNeedsUpdate force-hides it when its enable flag reads 0,
        // then short-circuits until the device generation bumps). Re-evaluate device visibility now so
        // re-enabling a device here brings its tab back without a physical replug.
        bdmForceDeviceRefresh();

        applyConfig(-1, -1, 0);
        menuReinitMainMenu();
    }
}

void guiShowParentalLockConfig(void)
{
    int result;
    char password[CONFIG_KEY_VALUE_LEN];
    config_set_t *configOPL = configGetByType(CONFIG_OPL);

    // Set current values
    configGetStrCopy(configOPL, CONFIG_OPL_PARENTAL_LOCK_PWD, password, CONFIG_KEY_VALUE_LEN); // This will return the current password, or a blank string if it is not set.
    diaSetString(diaParentalLockConfig, CFG_PARENLOCK_PASSWORD, password);

    result = diaExecuteDialog(diaParentalLockConfig, -1, 1, NULL);
    if (result) {
        diaGetString(diaParentalLockConfig, CFG_PARENLOCK_PASSWORD, password, CONFIG_KEY_VALUE_LEN);

        if (strlen(password) > 0) {
            if (strncmp(OPL_PARENTAL_LOCK_MASTER_PASS, password, CONFIG_KEY_VALUE_LEN) != 0) {
                // Store password
                configSetStr(configOPL, CONFIG_OPL_PARENTAL_LOCK_PWD, password);
            } else {
                // Password not acceptable (i.e. master password entered).
                guiMsgBox(_l(_STR_PARENLOCK_INVALID_PASSWORD), 0, NULL);
            }
        } else {
            configRemoveKey(configOPL, CONFIG_OPL_PARENTAL_LOCK_PWD);

            guiMsgBox(_l(_STR_PARENLOCK_DISABLE_WARNING), 0, diaParentalLockConfig);
        }

        menuSetParentalLockCheckState(1);
    }
}

static void guiSetAudioSettingsState(void)
{
    diaGetInt(diaAudioConfig, CFG_SFX, &gEnableSFX);
    diaGetInt(diaAudioConfig, CFG_BOOT_SND, &gEnableBootSND);
    diaGetInt(diaAudioConfig, CFG_BGM, &gEnableBGM);
    diaGetInt(diaAudioConfig, CFG_SFX_VOLUME, &gSFXVolume);
    diaGetInt(diaAudioConfig, CFG_BOOT_SND_VOLUME, &gBootSndVolume);
    diaGetInt(diaAudioConfig, CFG_BGM_VOLUME, &gBGMVolume);
    diaGetString(diaAudioConfig, CFG_DEFAULT_BGM_PATH, gDefaultBGMPath, sizeof(gDefaultBGMPath));
    audioSetVolume();

    if (gEnableBGM && !isBgmPlaying())
        bgmStart();
}

static int guiAudioUpdater(int modified)
{
    if (modified) {
        guiSetAudioSettingsState();
    }

    return 0;
}

void guiShowAudioConfig(void)
{
    diaSetInt(diaAudioConfig, CFG_SFX, gEnableSFX);
    diaSetInt(diaAudioConfig, CFG_BOOT_SND, gEnableBootSND);
    diaSetInt(diaAudioConfig, CFG_BGM, gEnableBGM);
    diaSetInt(diaAudioConfig, CFG_SFX_VOLUME, gSFXVolume);
    diaSetInt(diaAudioConfig, CFG_BOOT_SND_VOLUME, gBootSndVolume);
    diaSetInt(diaAudioConfig, CFG_BGM_VOLUME, gBGMVolume);
    diaSetString(diaAudioConfig, CFG_DEFAULT_BGM_PATH, gDefaultBGMPath);
    diaSetShowDefaultWhenEmpty(diaAudioConfig, CFG_DEFAULT_BGM_PATH, 1); // blank -> the theme's own bgm

    diaExecuteDialog(diaAudioConfig, -1, 1, guiAudioUpdater);
}

void guiShowCoverflowConfig(void)
{
    int value;
    int i;

    // Map index<->stored value: scale 0/15/30/45 is linear (idx*15); anim 0/100/200/400 is a lookup.
    static const int animValues[] = {0, 100, 200, 400};

    const char *coverCounts[] = {"3", "5", NULL};
    const char *centerScales[] = {_l(_STR_NONE), _l(_STR_SMALL), _l(_STR_MEDIUM), _l(_STR_LARGE), NULL};
    const char *animSpeeds[] = {_l(_STR_OFF), _l(_STR_FAST), _l(_STR_NORMAL), _l(_STR_SLOW), NULL};

    diaSetEnum(diaCoverflowConfig, COVERFLOW_CFG_COUNT, coverCounts);
    diaSetEnum(diaCoverflowConfig, COVERFLOW_CFG_SCALE, centerScales);
    diaSetEnum(diaCoverflowConfig, COVERFLOW_CFG_ANIM, animSpeeds);

    diaSetInt(diaCoverflowConfig, COVERFLOW_CFG_COUNT, (gCoverflowCount == 5) ? 1 : 0);

    int scaleIdx = gCoverflowCenterScale / 15;
    if (scaleIdx < 0)
        scaleIdx = 0;
    else if (scaleIdx > 3)
        scaleIdx = 3;
    diaSetInt(diaCoverflowConfig, COVERFLOW_CFG_SCALE, scaleIdx);

    // default to Normal (200ms) if the stored value isn't one of the table entries
    int animIdx = 2;
    for (i = 0; i < 4; i++)
        if (animValues[i] == gCoverflowAnimSpeed)
            animIdx = i;
    diaSetInt(diaCoverflowConfig, COVERFLOW_CFG_ANIM, animIdx);

    diaSetInt(diaCoverflowConfig, COVERFLOW_CFG_DIM, gCoverflowDimCovers ? 1 : 0);

    int result = diaExecuteDialog(diaCoverflowConfig, -1, 1, NULL);
    if (result) {
        if (diaGetInt(diaCoverflowConfig, COVERFLOW_CFG_COUNT, &value))
            gCoverflowCount = (value == 1) ? 5 : 3;
        if (diaGetInt(diaCoverflowConfig, COVERFLOW_CFG_SCALE, &value))
            gCoverflowCenterScale = ((value >= 0 && value <= 3) ? value : 0) * 15;
        if (diaGetInt(diaCoverflowConfig, COVERFLOW_CFG_ANIM, &value))
            gCoverflowAnimSpeed = animValues[(value >= 0 && value <= 3) ? value : 2];
        if (diaGetInt(diaCoverflowConfig, COVERFLOW_CFG_DIM, &value))
            gCoverflowDimCovers = value ? 1 : 0;
    }
}

// Neutrino Launch Args sub-screen: edit the user-settable Neutrino flags as structured fields and
// reassemble them in a Neutrino-accepted order (--b last). Used for both the global args (here) and
// the per-game args. argsBuf in/out is the stored "Launch Args" string.
void guiShowNeutrinoArgsConfig(char *argsBuf, int bufSize)
{
    neutrino_args_t na;
    neutrinoArgsParse(argsBuf, &na);

    diaSetInt(diaNeutrinoArgs, NARGS_QB, na.qb ? 1 : 0);
    diaSetInt(diaNeutrinoArgs, NARGS_DBC, na.dbc ? 1 : 0);
    diaSetInt(diaNeutrinoArgs, NARGS_LOGO, na.logo ? 1 : 0);
    diaSetString(diaNeutrinoArgs, NARGS_CWD, na.cwd);
    diaSetString(diaNeutrinoArgs, NARGS_CFG, na.cfg);
    diaSetString(diaNeutrinoArgs, NARGS_ELF, na.elf);
    diaSetString(diaNeutrinoArgs, NARGS_ATA0, na.ata0);
    diaSetString(diaNeutrinoArgs, NARGS_ATA0ID, na.ata0id);
    diaSetString(diaNeutrinoArgs, NARGS_ATA1, na.ata1);
    diaSetString(diaNeutrinoArgs, NARGS_EXTRA, na.extra);

    // Baseline = the fields AS POPULATED (the UI caps each string at 31 chars). Comparing the
    // post-dialog fields against this tells us which fields the user actually edited, so untouched
    // fields can keep their FULL parsed value instead of the truncated UI copy.
    neutrino_args_t pop;
    diaGetInt(diaNeutrinoArgs, NARGS_QB, &pop.qb);
    diaGetInt(diaNeutrinoArgs, NARGS_DBC, &pop.dbc);
    diaGetInt(diaNeutrinoArgs, NARGS_LOGO, &pop.logo);
    diaGetString(diaNeutrinoArgs, NARGS_CWD, pop.cwd, sizeof(pop.cwd));
    diaGetString(diaNeutrinoArgs, NARGS_CFG, pop.cfg, sizeof(pop.cfg));
    diaGetString(diaNeutrinoArgs, NARGS_ELF, pop.elf, sizeof(pop.elf));
    diaGetString(diaNeutrinoArgs, NARGS_ATA0, pop.ata0, sizeof(pop.ata0));
    diaGetString(diaNeutrinoArgs, NARGS_ATA0ID, pop.ata0id, sizeof(pop.ata0id));
    diaGetString(diaNeutrinoArgs, NARGS_ATA1, pop.ata1, sizeof(pop.ata1));
    diaGetString(diaNeutrinoArgs, NARGS_EXTRA, pop.extra, sizeof(pop.extra));

    if (diaExecuteDialog(diaNeutrinoArgs, -1, 1, NULL)) {
        neutrino_args_t out;
        char after[256];
        diaGetInt(diaNeutrinoArgs, NARGS_QB, &out.qb);
        diaGetInt(diaNeutrinoArgs, NARGS_DBC, &out.dbc);
        diaGetInt(diaNeutrinoArgs, NARGS_LOGO, &out.logo);
        diaGetString(diaNeutrinoArgs, NARGS_CWD, out.cwd, sizeof(out.cwd));
        diaGetString(diaNeutrinoArgs, NARGS_CFG, out.cfg, sizeof(out.cfg));
        diaGetString(diaNeutrinoArgs, NARGS_ELF, out.elf, sizeof(out.elf));
        diaGetString(diaNeutrinoArgs, NARGS_ATA0, out.ata0, sizeof(out.ata0));
        diaGetString(diaNeutrinoArgs, NARGS_ATA0ID, out.ata0id, sizeof(out.ata0id));
        diaGetString(diaNeutrinoArgs, NARGS_ATA1, out.ata1, sizeof(out.ata1));
        diaGetString(diaNeutrinoArgs, NARGS_EXTRA, out.extra, sizeof(out.extra));
        // Per-field merge: adopt only the fields the user actually changed; untouched fields keep
        // their FULL parsed value (na) so editing one field never truncates the others to 31 chars.
        if (out.qb != pop.qb)
            na.qb = out.qb;
        if (out.dbc != pop.dbc)
            na.dbc = out.dbc;
        if (out.logo != pop.logo)
            na.logo = out.logo;
        if (strcmp(out.cwd, pop.cwd) != 0)
            snprintf(na.cwd, sizeof(na.cwd), "%s", out.cwd);
        if (strcmp(out.cfg, pop.cfg) != 0)
            snprintf(na.cfg, sizeof(na.cfg), "%s", out.cfg);
        if (strcmp(out.elf, pop.elf) != 0)
            snprintf(na.elf, sizeof(na.elf), "%s", out.elf);
        if (strcmp(out.ata0, pop.ata0) != 0)
            snprintf(na.ata0, sizeof(na.ata0), "%s", out.ata0);
        if (strcmp(out.ata0id, pop.ata0id) != 0)
            snprintf(na.ata0id, sizeof(na.ata0id), "%s", out.ata0id);
        if (strcmp(out.ata1, pop.ata1) != 0)
            snprintf(na.ata1, sizeof(na.ata1), "%s", out.ata1);
        if (strcmp(out.extra, pop.extra) != 0)
            snprintf(na.extra, sizeof(na.extra), "%s", out.extra);
        neutrinoArgsAssemble(&na, after, sizeof(after));
        snprintf(argsBuf, bufSize, "%s", after);
    }
}

void guiShowControllerConfig(void)
{
    int value;

    // configure the enumerations
    const char *scrollSpeeds[] = {_l(_STR_SLOW), _l(_STR_MEDIUM), _l(_STR_FAST), NULL};
    const char *selectButtons[] = {_l(_STR_CIRCLE), _l(_STR_CROSS), NULL};
    const char *sensitivity[] = {_l(_STR_OFF), _l(_STR_LOW), _l(_STR_MEDIUM), _l(_STR_HIGH), NULL};

    diaSetEnum(diaControllerConfig, UICFG_SCROLL, scrollSpeeds);
    diaSetEnum(diaControllerConfig, CFG_SELECTBUTTON, selectButtons);
    diaSetEnum(diaControllerConfig, CFG_XSENSITIVITY, sensitivity);
    diaSetEnum(diaControllerConfig, CFG_YSENSITIVITY, sensitivity);

    diaSetInt(diaControllerConfig, UICFG_SCROLL, gScrollSpeed);
    diaSetInt(diaControllerConfig, CFG_SELECTBUTTON, gSelectButton == KEY_CIRCLE ? 0 : 1);
    diaSetInt(diaControllerConfig, CFG_XSENSITIVITY, gXSensitivity);
    diaSetInt(diaControllerConfig, CFG_YSENSITIVITY, gYSensitivity);
    diaSetInt(diaControllerConfig, CFG_RUMBLE, gEnableRumble);

    int result = diaExecuteDialog(diaControllerConfig, -1, 1, NULL);
    if (result) {
        diaGetInt(diaControllerConfig, UICFG_SCROLL, &gScrollSpeed);
        diaGetInt(diaControllerConfig, CFG_XSENSITIVITY, &gXSensitivity);
        diaGetInt(diaControllerConfig, CFG_YSENSITIVITY, &gYSensitivity);
        diaGetInt(diaControllerConfig, CFG_RUMBLE, &gEnableRumble);

        if (diaGetInt(diaControllerConfig, CFG_SELECTBUTTON, &value))
            gSelectButton = value == 0 ? KEY_CIRCLE : KEY_CROSS;
        else
            gSelectButton = KEY_CIRCLE;
#ifdef PADEMU
        if (result == PADEMU_GLOBAL_BUTTON) {
            guiGameShowPadEmuConfig(1);
        } else if (result == PADMACRO_GLOBAL_BUTTON) {
            guiGameShowPadMacroConfig(1);
        }
#endif
        applyConfig(-1, -1, 1);
    }
}

int guiShowKeyboard(char *value, int maxLength)
{
    char tmp[maxLength];
    strncpy(tmp, value, maxLength);

    int result = diaShowKeyb(tmp, maxLength, 0, NULL);
    if (result) {
        strncpy(value, tmp, maxLength);
        value[maxLength - 1] = '\0';
    }

    return result;
}

int guiGetOpCompleted(int opid)
{
    return gCompletedOps > opid;
}

int guiDeferUpdate(struct gui_update_t *op)
{
    WaitSema(gSemaId);

    struct gui_update_list_t *up = (struct gui_update_list_t *)malloc(sizeof(struct gui_update_list_t));
    if (!up) {
        /* OOM: release the semaphore so future callers are not permanently locked out */
        SignalSema(gSemaId);
        return -1;
    }
    up->item = op;
    up->next = NULL;

    if (!gUpdateList) {
        gUpdateList = up;
        gUpdateEnd = gUpdateList;
    } else {
        gUpdateEnd->next = up;
        gUpdateEnd = up;
    }

    SignalSema(gSemaId);

    return gScheduledOps++;
}

static void guiHandleOp(struct gui_update_t *item)
{
    submenu_list_t *result = NULL;

    switch (item->type) {
        case GUI_INIT_DONE:
            gInitComplete = 1;
            break;

        case GUI_OP_ADD_MENU:
            menuAppendItem(item->menu.menu);
            break;

        case GUI_OP_APPEND_MENU:
            result = submenuAppendItem(item->menu.subMenu, item->submenu.icon_id, item->submenu.text, item->submenu.id, item->submenu.text_id, item->submenu.owner);
            if (result != NULL)
                result->item.isFolder = item->submenu.isFolder; // folder-browse row marker
            // coverflow wrap tail: submenuAppendItem always returns the new tail
            item->menu.menu->last = result;
            if (!item->menu.menu->submenu) { // first subitem in list
                item->menu.menu->submenu = result;
                if (!item->submenu.selected) {
                    item->menu.menu->current = result;
                    item->menu.menu->pagestart = result;
                }
            }
            if (item->submenu.selected) { // remember last played game feature
                item->menu.menu->current = result;
                item->menu.menu->pagestart = result;
                item->menu.menu->remindLast = 1;

                // Last Played Auto Start
                if ((gAutoStartLastPlayed) && !(KeyPressedOnce))
                    DisableCron = 0; // Release Auto Start Last Played counter
            }

            break;

        case GUI_OP_SELECT_MENU:
            menuSetSelectedItem(item->menu.menu);
            screenHandler = &screenHandlers[GUI_SCREEN_MAIN];
            break;

        case GUI_OP_CLEAR_SUBMENU:
            submenuDestroy(item->menu.subMenu);
            item->menu.menu->submenu = NULL;
            item->menu.menu->current = NULL;
            item->menu.menu->pagestart = NULL;
            item->menu.menu->last = NULL; // coverflow wrap tail
            break;

        case GUI_OP_SORT: {
            // Sort by the on-screen title: hand the owning device's mode down so a VCD view with "hide
            // game ID" on orders by the rendered name, not the raw filename's game-ID prefix (#195).
            // userdata is the item_list_t (opl.c: menuItem.userdata = mod->support); -1 if unset.
            item_list_t *sortSupport = (item_list_t *)item->menu.menu->userdata;
            submenuSort(item->menu.subMenu, sortSupport ? sortSupport->mode : -1);
        }
            item->menu.menu->submenu = *item->menu.subMenu;

            { // recompute the coverflow wrap tail after the sort reorders the list
                submenu_list_t *tail = item->menu.menu->submenu;
                while (tail && tail->next)
                    tail = tail->next;
                item->menu.menu->last = tail;
            }

            if (!item->menu.menu->remindLast)
                item->menu.menu->current = item->menu.menu->submenu;

            item->menu.menu->pagestart = item->menu.menu->current;
            break;

        case GUI_OP_ADD_HINT:
            // append the hint list in the menu item
            menuAddHint(item->menu.menu, item->hint.text_id, item->hint.icon_id);
            break;

        default:
            LOG("GUI: ??? (%d)\n", item->type);
    }
}

static void guiHandleDeferredOps(void)
{

    WaitSema(gSemaId);
    while (gUpdateList) {

        guiHandleOp(gUpdateList->item);

        struct gui_update_list_t *td = gUpdateList;
        gUpdateList = gUpdateList->next;

        free(td);

        gCompletedOps++;
    }
    // Clear the now-dangling tail pointer INSIDE the lock. The drain loop freed every node and emptied
    // gUpdateList; doing this AFTER SignalSema let an IO-thread guiDeferUpdate slip in (rebuild the list,
    // set gUpdateEnd) only to have it clobbered here -> the next enqueue's gUpdateEnd->next was a NULL deref.
    gUpdateEnd = NULL;
    SignalSema(gSemaId);
}

void guiExecDeferredOps(void)
{
    // Clears deferred operations list by executing them.
    guiHandleDeferredOps();
}

static void guiDrawBusy(int alpha)
{
    if (gTheme->loadingIcon) {
        GSTEXTURE *texture = thmGetTexture(LOAD0_ICON + (guiFrameId >> 1) % gTheme->loadingIconCount);
        if (texture && texture->Mem) {
            u64 mycolor = GS_SETREG_RGBA(0x80, 0x80, 0x80, alpha);
            rmDrawPixmap(texture, gTheme->loadingIcon->posX, gTheme->loadingIcon->posY, gTheme->loadingIcon->aligned, gTheme->loadingIcon->width, gTheme->loadingIcon->height, gTheme->loadingIcon->scaled, mycolor, 0);
        }
    }
}

// Boot-splash status line setter (#297). Pass NULL to clear. Main-thread only (writes gBootStatus, which
// only guiRenderGreeting on the same thread reads). guiRenderGreeting prefers any IO-thread sticky label
// over this, so a main-thread scan/Ready set no longer needs to guard against the IO thread.
void guiSetBootStatus(const char *status)
{
    if (status == NULL) {
        gBootStatus[0] = '\0';
        gBootStatusActive = 0;
        gBootStickyLabel = NULL; // release the localizer latch so a fresh boot can claim the line again
        return;
    }
    snprintf(gBootStatus, sizeof(gBootStatus), "%s", status);
    gBootStatusActive = 1;
}

// Boot-step localizer setter, called from the deferred IO-thread boot steps. `label` MUST be a static
// string (an _l() lang entry or literal) since only a POINTER to it is stored -- no copy, no shared
// buffer, so no data race with the main-thread render (a single aligned pointer store/load is atomic on
// the EE). guiRenderGreeting prefers this over gBootStatus, so if this step wedges its label stays on the
// splash. Later IO-thread steps (same single thread) simply replace the pointer in turn.
void guiSetBootStatusSticky(const char *label)
{
    if (label == NULL)
        return;
    gBootStickyLabel = label;
}

static void guiRenderGreeting(int alpha)
{
    u64 mycolor = GS_SETREG_RGBA(0x1C, 0x1C, 0x1C, alpha);
    rmDrawRect(0, 0, screenWidth, screenHeight, mycolor);

    // If the theme/build ships animated boot-logo frames (logo0..logoN), cycle
    // them at the loading-icon cadence; otherwise use the single LOGO_PICTURE.
    int logoTex = LOGO_PICTURE;
    if (gTheme->logoFrameCount >= 1)
        logoTex = LOGO0_PICTURE + (guiFrameId >> 1) % gTheme->logoFrameCount;

    GSTEXTURE *logo = thmGetTexture(logoTex);
    if (logo) {
        mycolor = GS_SETREG_RGBA(0x80, 0x80, 0x80, alpha);
        rmDrawPixmap(logo, screenWidth >> 1, gTheme->usedHeight >> 1, ALIGN_CENTER, logo->Width, logo->Height, SCALING_RATIO, mycolor, 0);
    }

    // Fork-native boot info (#297): the RiptOPL version + a live status line, faded with the splash.
    // Reuses gTheme->fonts[0] (always-loaded built-in) and the same OPL_VERSION the About dialog shows.
    u64 infoColor = GS_SETREG_RGBA(0x80, 0x80, 0x80, alpha);
    char verLine[48];
    snprintf(verLine, sizeof(verLine), "RiptOPL %s", OPL_VERSION);
    fntRenderString(gTheme->fonts[0], screenWidth >> 1, (gTheme->usedHeight >> 1) + 80, ALIGN_CENTER, 0, 0, verLine, infoColor);
    // Prefer an IO-thread boot-step label (the localizer) over the main-thread scan/Ready line, so a
    // wedged step names itself. gBootStickyLabel is a single atomic pointer to a static string.
    const char *bootLine = gBootStickyLabel;
    if (bootLine == NULL && gBootStatusActive && gBootStatus[0] != '\0')
        bootLine = gBootStatus;
    if (bootLine != NULL)
        fntRenderString(gTheme->fonts[0], screenWidth >> 1, gTheme->usedHeight - 40, ALIGN_CENTER, 0, 0, bootLine, infoColor);
}

// Draw one standalone boot-splash frame: the same greeting guiIntroLoop() shows,
// at full opacity. Used as the boot "loading" screen instead of
// guiRenderTextScreen(), whose guiShow() call would render the not-yet-ready main
// menu (empty lists, no device selected) as a garbled landing page before the
// intro splash. This keeps the OPL logo on screen across the config load so boot
// shows the splash, never a half-drawn menu.
void guiRenderGreetingScreen(void)
{
    guiStartFrame();
    guiRenderGreeting(0x80);
    guiEndFrame();
}

static float mix(float a, float b, float t)
{
    return a + (b - a) * t;
}

static float fade(float t)
{
    return fadetbl[(int)(t * FADE_SIZE)];
}

// The same as mix, but with 8 (2*4) values mixed at once
static void VU0MixVec(VU_VECTOR *a, VU_VECTOR *b, float mix, VU_VECTOR *res)
{
    asm volatile(
#if __GNUC__ > 3
        "lqc2           $vf1, (%[a])\n"        // load the first vector
        "lqc2           $vf2, (%[b])\n"        // load the second vector
        "qmtc2          %[mix], $vf3\n"        // move the mix value from reg to VU
        "vaddw.x        $vf5, $vf0, $vf0\n"    // vf5.x = 1
        "vsub.x         $vf4x, $vf5x, $vf3x\n" // subtract 1 - vf3,x, store the result in vf4.x
        "vmulax.xyzw    $ACC, $vf1, $vf3x\n"   // multiply vf1 by vf3.x, store the result in ACC
        "vmaddx.xyzw    $vf1, $vf2, $vf4x\n"   // multiply vf2 by vf4.x add ACC, store the result in vf1
        "sqc2           $vf1, (%[res])\n"      // transfer the result in acc to the ee
#else
        "lqc2           vf1, (%[a])\n"      // load the first vector
        "lqc2           vf2, (%[b])\n"      // load the second vector
        "qmtc2          %[mix], vf3\n"      // move the mix value from reg to VU
        "vaddw.x        vf5, vf00, vf00\n"  // vf5.x = 1
        "vsub.x         vf4x, vf5x, vf3x\n" // subtract 1 - vf3,x, store the result in vf4.x
        "vmulax.xyzw    ACC, vf1, vf3x\n"   // multiply vf1 by vf3.x, store the result in ACC
        "vmaddx.xyzw    vf1, vf2, vf4x\n"   // multiply vf2 by vf4.x add ACC, store the result in vf1
        "sqc2           vf1, (%[res])\n"    // transfer the result in acc to the ee
#endif
        : [res] "+r"(res), "=m"(*res)
        : [a] "r"(a), [b] "r"(b), [mix] "r"(mix), "m"(*a), "m"(*b));
}

static float guiCalcPerlin(float x, float y, float z)
{
    // Taken from: http://people.opera.com/patrickl/experiments/canvas/plasma/perlin-noise-classical.js
    // By Sean McCullough

    // Find unit grid cell containing point
    int X = floorf(x);
    int Y = floorf(y);
    int Z = floorf(z);

    // Get relative xyz coordinates of point within that cell
    x = x - X;
    y = y - Y;
    z = z - Z;

    // Wrap the integer cells at 255 (smaller integer period can be introduced here)
    X = X & 255;
    Y = Y & 255;
    Z = Z & 255;

    // Calculate a set of eight hashed gradient indices
    int gi000 = pperm[X + pperm[Y + pperm[Z]]] % 12;
    int gi001 = pperm[X + pperm[Y + pperm[Z + 1]]] % 12;
    int gi010 = pperm[X + pperm[Y + 1 + pperm[Z]]] % 12;
    int gi011 = pperm[X + pperm[Y + 1 + pperm[Z + 1]]] % 12;
    int gi100 = pperm[X + 1 + pperm[Y + pperm[Z]]] % 12;
    int gi101 = pperm[X + 1 + pperm[Y + pperm[Z + 1]]] % 12;
    int gi110 = pperm[X + 1 + pperm[Y + 1 + pperm[Z]]] % 12;
    int gi111 = pperm[X + 1 + pperm[Y + 1 + pperm[Z + 1]]] % 12;

    // The gradients of each corner are now:
    // g000 = grad3[gi000];
    // g001 = grad3[gi001];
    // g010 = grad3[gi010];
    // g011 = grad3[gi011];
    // g100 = grad3[gi100];
    // g101 = grad3[gi101];
    // g110 = grad3[gi110];
    // g111 = grad3[gi111];
    // Calculate noise contributions from each of the eight corners
    VU_VECTOR vec;
    vec.x = x;
    vec.y = y;
    vec.z = z;
    vec.w = 1;

    VU_VECTOR a, b;

    // float n000
    a.x = Vu0DotProduct(&pgrad3[gi000], &vec);

    vec.y -= 1;

    // float n010
    a.z = Vu0DotProduct(&pgrad3[gi010], &vec);

    vec.x -= 1;

    // float n110
    b.z = Vu0DotProduct(&pgrad3[gi110], &vec);

    vec.y += 1;

    // float n100
    b.x = Vu0DotProduct(&pgrad3[gi100], &vec);

    vec.z -= 1;

    // float n101
    b.y = Vu0DotProduct(&pgrad3[gi101], &vec);

    vec.y -= 1;

    // float n111
    b.w = Vu0DotProduct(&pgrad3[gi111], &vec);

    vec.x += 1;

    // float n011
    a.w = Vu0DotProduct(&pgrad3[gi011], &vec);

    vec.y += 1;

    // float n001
    a.y = Vu0DotProduct(&pgrad3[gi001], &vec);

    // Compute the fade curve value for each of x, y, z
    float u = fade(x);
    float v = fade(y);
    float w = fade(z);

    // TODO: Low priority... This could be done on VU0 (xyzw for the first 4 mixes)
    // The result in sw
    // Interpolate along x the contributions from each of the corners
    VU_VECTOR rv;
    VU0MixVec(&b, &a, u, &rv);

    // TODO: The VU0MixVec could as well mix the results (as follows) - might improve performance...
    // Interpolate the four results along y
    float nxy0 = mix(rv.x, rv.z, v);
    float nxy1 = mix(rv.y, rv.w, v);
    // Interpolate the two last results along z
    float nxyz = mix(nxy0, nxy1, w);

    return nxyz;
}

static float dir = 0.02;
static float perz = -100;
static int pery = 0;
static unsigned char curbgColor[3] = {0, 0, 0};

static int cdirection(unsigned char a, unsigned char b)
{
    if (a == b)
        return 0;
    else if (a > b)
        return -1;
    else
        return 1;
}

// Optional settings/menu background: if the active theme supplies a SETTINGS_BG texture
// (theme cfg use_settings_bg=1, or a future embedded default), draw it full-screen behind
// dialogs/menus and return 1; otherwise return 0 so callers fall back to guiDrawBGPlasma().
// Dormant by default -- the SETTINGS_BG slot is empty unless a theme provides the image.
int guiDrawBGSettings(void)
{
    GSTEXTURE *bg = thmGetTexture(SETTINGS_BG);
    if (bg) {
        rmDrawPixmap(bg, 0, 0, ALIGN_NONE, screenWidth, screenHeight, SCALING_NONE, gDefaultCol, 0);
        return 1;
    }
    return 0;
}

void guiDrawBGPlasma()
{
    int x, y;

    // transition the colors
    curbgColor[0] += cdirection(curbgColor[0], gTheme->bgColor[0]);
    curbgColor[1] += cdirection(curbgColor[1], gTheme->bgColor[1]);
    curbgColor[2] += cdirection(curbgColor[2], gTheme->bgColor[2]);

    // it's PLASMA_ROWS_PER_FRAME rows a frame to stop being a resource hog
    if (pery >= PLASMA_H) {
        pery = 0;
        perz += dir;

        if (perz > 100.0f || perz < -100.0f)
            dir = -dir;
    }

    u32 *buf = gBackgroundTex.Mem + PLASMA_W * pery;
    int ymax = pery + PLASMA_ROWS_PER_FRAME;

    if (ymax > PLASMA_H)
        ymax = PLASMA_H;

    // plasma_blend_color (parity-audit #15): the gradient's LOW end, historically hardcoded black
    // (the old expression fper*curbgColor>>8 lerps black->bgColor). Lerp blend->bgColor instead;
    // the default black collapses to the exact old bytes (all terms non-negative there, and /256 of
    // a non-negative int is exactly >>8). Signed intermediates: the delta goes negative when the
    // blend is brighter than the tint -- divide instead of shifting (right-shift of a negative int
    // is implementation-defined); the result stays in [0,255] without clamping (fper<=256,
    // |delta|<=255: endpoints land exactly on blend / curbgColor).
    const unsigned char *blend = gTheme->plasBlendColor;

    for (y = pery; y < ymax; y++) {
        for (x = 0; x < PLASMA_W; x++) {
            u32 fper = guiCalcPerlin((float)(2 * x) / PLASMA_W, (float)(2 * y) / PLASMA_H, perz) * 0x80 + 0x80;

            *buf = GS_SETREG_RGBA(
                (u32)(blend[0] + (((int)fper * ((int)curbgColor[0] - (int)blend[0])) / 256)),
                (u32)(blend[1] + (((int)fper * ((int)curbgColor[1] - (int)blend[1])) / 256)),
                (u32)(blend[2] + (((int)fper * ((int)curbgColor[2] - (int)blend[2])) / 256)),
                0x80);

            ++buf;
        }
    }

    pery = ymax;
    rmInvalidateTexture(&gBackgroundTex);
    rmDrawPixmap(&gBackgroundTex, 0, 0, ALIGN_NONE, screenWidth, screenHeight, SCALING_NONE, gDefaultCol, 0);
}

int guiDrawIconAndText(int iconId, int textId, int font, int x, int y, u64 color)
{
    GSTEXTURE *iconTex = thmGetTexture(iconId);
    int w = 0;
    int h = 20;

    if (iconTex) {
        w = (iconTex->Width * 20) / iconTex->Height;
    }

    if (iconTex && iconTex->Mem) {
        y += h >> 1;
        rmDrawPixmap(iconTex, x, y, ALIGN_VCENTER, w, h, SCALING_RATIO, gDefaultCol, 0);
        x += rmWideScale(w) + 2;
    } else {
        // HACK: font is aligned to VCENTER, the default height icon height is 20
        y += 10;
    }

    x = fntRenderString(font, x, y, ALIGN_VCENTER, 0, 0, _l(textId), color);

    return x;
}

int guiAlignMenuHints(menu_hint_item_t *hint, int font, int width)
{
    int x = screenWidth;
    int w;

    for (; hint; hint = hint->next) {
        GSTEXTURE *iconTex = thmGetTexture(hint->icon_id);
        /* thmGetTexture returns NULL when the texture Mem is zero (missing disk-theme icon) */
        w = iconTex ? (iconTex->Width * 20) / iconTex->Height : 20;
        char *text = _l(hint->text_id);

        x -= rmWideScale(w) + 2;
        x -= rmUnScaleX(fntCalcDimensions(font, text));
        if (hint->next != NULL)
            x -= width;
    }

    // align center
    x /= 2;

    return x;
}

int guiAlignSubMenuHints(int hintCount, int *textID, int *iconID, int font, int width, int align)
{
    int x = screenWidth;
    int i, w;

    for (i = 0; i < hintCount; i++) {
        GSTEXTURE *iconTex = thmGetTexture(iconID[i]);
        /* thmGetTexture returns NULL when the texture Mem is zero (missing disk-theme icon) */
        w = iconTex ? (iconTex->Width * 20) / iconTex->Height : 20;
        char *text = _l(textID[i]);

        x -= rmWideScale(w) + 2;
        x -= rmUnScaleX(fntCalcDimensions(font, text));
        if (i != (hintCount - 1))
            x -= width;
    }

    if (align == 1) // align center
        x /= 2;

    if (align == 2) // align right
        x -= 20;

    return x;
}

void guiDrawSubMenuHints(void)
{
    int subMenuHints[2] = {_STR_SELECT, _STR_GAMES_LIST};
    int subMenuIcons[2] = {CIRCLE_ICON, CROSS_ICON};

    int x = guiAlignSubMenuHints(2, subMenuHints, subMenuIcons, gTheme->fonts[0], 12, 2);
    int y = gTheme->usedHeight - 32;

    x = guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? subMenuIcons[0] : subMenuIcons[1], subMenuHints[0], gTheme->fonts[0], x, y, gTheme->textColor);
    x += 12;
    guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? subMenuIcons[1] : subMenuIcons[0], subMenuHints[1], gTheme->fonts[0], x, y, gTheme->textColor);
}

static int endIntro = 0; // Break intro loop and start 'Last Played Auto Start' countdown

void guiDrawDebugLine(void)
{
    if (!gEnableDebug)
        return;

    pad_diag_t pd;
    unsigned int sfxLastMs, sfxMaxMs;
    char dbg[160];

    padGetDiag(&pd);
    sfxGetPlayDiag(&sfxLastMs, &sfxMaxMs);
    snprintf(dbg, sizeof(dbg), "PAD miss:%u brst:%u/%u flap:%u init:%u(%u/%ums) def:%u poll:%ums AC:%d SFX:%u/%ums",
             pd.readMisses, pd.missBurst, pd.missBurstMax, pd.stateFlaps,
             pd.reinitRuns, pd.reinitLastMs, pd.reinitMaxMs, pd.reinitDefers,
             pd.pollMaxMs, pd.analogCapable, sfxLastMs, sfxMaxMs);
    fntRenderString(gTheme->fonts[0], 0, screenHeight - 24, ALIGN_NONE, 0, 0, dbg, GS_SETREG_RGBA(255, 255, 0, 128));
}

static void guiDrawOverlays()
{
    // are there any pending operations?
    int pending = ioHasPendingRequests();
    static int busyAlpha = 0x00; // Fully transparant

    if (!pending) {
        // Fade out
        if (busyAlpha > 0x00)
            busyAlpha -= 0x02;
    } else {
        // Fade in
        if (busyAlpha < 0x80)
            busyAlpha += 0x02;
    }

    if (busyAlpha > 0x00)
        guiDrawBusy(busyAlpha);

#ifdef __DEBUG
    char text[20];
    int x = screenWidth - 120;
    int y = 15;
    int yadd = 15;

    snprintf(text, sizeof(text), "VRAM:");
    fntRenderString(gTheme->fonts[0], x, y, ALIGN_LEFT, 0, 0, text, GS_SETREG_RGBA(0x60, 0x60, 0x60, 0x80));
    y += yadd;

    snprintf(text, sizeof(text), "%dKiB FIXED", gsGlobal->CurrentPointer / 1024);
    fntRenderString(gTheme->fonts[0], x, y, ALIGN_LEFT, 0, 0, text, GS_SETREG_RGBA(0x60, 0x60, 0x60, 0x80));
    y += yadd;

    snprintf(text, sizeof(text), "%dKiB TEXMAN", ((4 * 1024 * 1024) - gsGlobal->CurrentPointer) / 1024);
    fntRenderString(gTheme->fonts[0], x, y, ALIGN_LEFT, 0, 0, text, GS_SETREG_RGBA(0x60, 0x60, 0x60, 0x80));
    y += yadd;
    y += yadd; // Empty line

    if (prevtime != 0) {
        clock_t diff = curtime - prevtime;
        if (diff == 0)
            diff = 1;

        // Raw FPS value with 2 decimal places
        float rawfps = ((100 * CLOCKS_PER_SEC) / diff) / 100.0f;

        if (fps == 0.0f)
            fps = rawfps;
        else
            fps = fps * 0.9f + rawfps / 10.0f; // Smooth FPS value

        snprintf(text, sizeof(text), "%.1f FPS", fps);
        fntRenderString(gTheme->fonts[0], x, y, ALIGN_LEFT, 0, 0, text, GS_SETREG_RGBA(0x60, 0x60, 0x60, 0x80));
        y += yadd;
    }
#endif

    // Last Played Auto Start
    // FULL queue check here, not the spinner's visible-only `pending` (#290): this gate previously
    // shared that variable, and quieting the background rescans would otherwise have let the
    // auto-start countdown run -- and fire the launch -- while a rescan still occupied the IO
    // worker. Freezing the countdown during ANY in-flight work is the pre-#290 behaviour; keep it.
    if (!ioHasPendingRequests() && DisableCron == 0 && endIntro) {
        if (CronStart == 0) {
            CronStart = clock() / CLOCKS_PER_SEC;
        } else {
            char strAutoStartInNSecs[21];
            clock_t CronCurrent;

            CronCurrent = clock() / CLOCKS_PER_SEC;
            RemainSecs = gAutoStartLastPlayed - (CronCurrent - CronStart);
            snprintf(strAutoStartInNSecs, sizeof(strAutoStartInNSecs), _l(_STR_AUTO_START_IN_N_SECS), RemainSecs);
            fntRenderString(gTheme->fonts[0], screenWidth / 2, screenHeight / 2, ALIGN_CENTER, 0, 0, strAutoStartInNSecs, GS_SETREG_RGBA(0x60, 0x60, 0x60, 0x80));
        }
    }

    // BLURT output
    // if (gEnableDebug)
    //     fntRenderString(gTheme->fonts[0], 0, screenHeight - 24, ALIGN_NONE, 0, 0, blurttext, GS_SETREG_RGBA(255, 255, 0, 128));

    // Debug-Colors instrumentation (#271/#272/#296). Steps aside during coverflow slides:
    // binding the font atlas every frame can evict cover textures from the near-saturated VRAM
    // arena mid-animation (the historic #120 HUD note).
    if (!thmCoverflowIsAnimating())
        guiDrawDebugLine();
}

static void guiReadPads()
{
    // A transition polls without dispatching input. Freeze against the triggering sample so that
    // button cannot replay on the destination, while a different button held through the fade can.
    padFreezeEdgeBaseline(screenHandlerTarget != NULL);

    if (readPads())
        guiInactiveFrames = 0;
    else if (guiInactiveFrames < INT_MAX)
        guiInactiveFrames++;

    cachePumpPendingArt();
}

// renders the screen and handles inputs. Also handles screen transitions between numerous
// screen handlers. Fade transition code written by Maximus32
static void guiShow()
{
    // is there a transmission effect going on or are
    // we in a normal rendering state?
    if (screenHandlerTarget) {
        u8 alpha;
        const u8 transition_frames = 26;
        if (transIndex < (transition_frames / 2)) {
            // Fade-out old screen
            // index: 0..7
            // alpha: 1..8 * transition_step
            screenHandler->renderScreen();
            alpha = fade((float)(transIndex + 1) / (transition_frames / 2)) * 0x80;
        } else {
            // Fade-in new screen
            // index: 8..15
            // alpha: 8..1 * transition_step
            screenHandlerTarget->renderScreen();
            alpha = fade((float)(transition_frames - transIndex) / (transition_frames / 2)) * 0x80;
        }

        // Overlay the actual "fade"
        rmDrawRect(0, 0, screenWidth, screenHeight, GS_SETREG_RGBA(0x00, 0x00, 0x00, alpha));

        // Advance the effect
        transIndex++;
        if (transIndex >= transition_frames) {
            screenHandler = screenHandlerTarget;
            screenHandlerTarget = NULL;
        }
    } else
        // render with the set screen handler
        screenHandler->renderScreen();
}

// A message box or modal wait raised mid-transition must land on the DESTINATION screen:
// letting the fade keep running behind it renders the OLD screen for the first half -- reported
// as "error messages always have the previous frame as their background". Snapping makes the same
// assignments guiShow() makes when the fade completes naturally (transIndex == transition_frames).
// One deliberate timing shift: the padFreezeEdgeBaseline input freeze, gated on
// screenHandlerTarget != NULL, releases up to ~13 frames earlier than the fade would have. Benign
// -- the box consumes its own edge-triggered dismiss (getKeyOn) and the transition-triggering
// button is long released by the time a box is dismissed.
static void guiSnapTransition(void)
{
    if (screenHandlerTarget) {
        screenHandler = screenHandlerTarget;
        screenHandlerTarget = NULL;
        transIndex = 0;
    }
}

void guiIntroLoop(void)
{
    int greetingAlpha = 0x80;
    const int fadeFrameCount = 0x80 / 2;
    const int fadeDuration = (fadeFrameCount * 1000) / 55; // Average between 50 and 60 fps
    clock_t tFadeDelayEnd = 0;

    while (!endIntro) {
        guiStartFrame();

        if (greetingAlpha < 0x80)
            guiShow();

        if (greetingAlpha > 0)
            guiRenderGreeting(greetingAlpha);

        // Initialize boot sound
        if (gInitComplete && !tFadeDelayEnd && gEnableBootSND) {
            // Start playing sound
            sfxPlay(SFX_BOOT);
            // Calculate transition delay
            tFadeDelayEnd = clock() + (sfxGetSoundDuration(SFX_BOOT) - fadeDuration) * (CLOCKS_PER_SEC / 1000);
        }

        if (gInitComplete && clock() >= tFadeDelayEnd)
            greetingAlpha -= 2;

        if (greetingAlpha <= 0)
            endIntro = 1;

        guiDrawOverlays();

        guiHandleDeferredOps();

        guiEndFrame();

        if (!screenHandlerTarget && screenHandler)
            screenHandler->handleInput();
    }
}

void guiMainLoop(void)
{
    guiResetNotifications();
    guiCheckNotifications(1, 1);

    if (gOPLPart[0] != '\0')
        showPartPopup = 1;

    if (gEnableBGM)
        bgmStart();

    while (!gTerminate) {
        guiStartFrame();

        // Read the pad states to prepare for input processing in the screen handler
        guiReadPads();

        // handle inputs and render screen
        guiShow();

        // Render overlaying gui thingies :)
        guiDrawOverlays();

        // The DHCP notice is a FUNCTIONAL one-time warning (a UDP transport bound to a stale DHCP-era
        // IP just silently finds nothing) -- it bypasses the cosmetic-notifications toggle.
        if (gEnableNotifications || showNetDhcpPopup)
            guiShowNotifications();

        // handle deferred operations
        guiHandleDeferredOps();

        guiEndFrame();

        // if not transiting, handle input
        // done here so we can use renderman if needed
        if (!screenHandlerTarget && screenHandler)
            screenHandler->handleInput();

        if (gFrameHook)
            gFrameHook();
    }
}

void guiSetFrameHook(gui_callback_t cback)
{
    gFrameHook = cback;
}

int guiGetCurrentScreen(void)
{
    // screenHandler always points INTO screenHandlers[] -- it is initialised to &screenHandlers[
    // GUI_SCREEN_MENU] and every reassignment takes its value from screenHandlerTarget, which is only
    // ever set from &screenHandlers[target] below. So the subtraction is always a valid index.
    return (int)(screenHandler - screenHandlers);
}

void guiSwitchScreen(int target)
{
    // Only initiate the transition once or else we could get stuck in an infinite loop.
    if (screenHandlerTarget != NULL) {
        return;
    }

    sfxPlay(SFX_TRANSITION);
    transIndex = 0;
    screenHandlerTarget = &screenHandlers[target];
}

struct gui_update_t *guiOpCreate(gui_op_type_t type)
{
    struct gui_update_t *op = (struct gui_update_t *)malloc(sizeof(struct gui_update_t));
    if (!op)
        return NULL; /* OOM: callers must check for NULL before writing fields */
    memset(op, 0, sizeof(struct gui_update_t));
    op->type = type;
    return op;
}

void guiUpdateScrollSpeed(void)
{
    // sanitize the settings
    if ((gScrollSpeed < 0) || (gScrollSpeed > 2))
        gScrollSpeed = 1;

    // update the pad delays for KEY_UP and KEY_DOWN
    // default delay is 7
    // fast - 100 ms
    // medium - 300 ms
    // slow - 500 ms
    setButtonDelay(KEY_UP, 500 - gScrollSpeed * 200); // 0,1,2 -> 500, 300, 100
    setButtonDelay(KEY_DOWN, 500 - gScrollSpeed * 200);
}

void guiUpdateScreenScale(void)
{
    fntUpdateAspectRatio();
}

int guiMsgBox(const char *text, int addAccept, struct UIItem *ui)
{
    int terminate = 0;

    // Background contract (the "error box shows the previous frame" report): a box raised while a
    // screen transition is in flight must land on the DESTINATION screen -- letting the fade keep
    // running renders the OLD screen behind the box for the first half. Snap it. (The dialog-flow
    // background case, rendering the settings dialog rather than the main screen behind the box, is
    // intentionally NOT handled here: the deferred error hook fires after the dialog's stack-local
    // enum arrays have died, and rendering it then would deref freed stack -- the #154 landmine.)
    guiSnapTransition();

    sfxPlay(SFX_MESSAGE);

    while (!terminate) {
        guiStartFrame();

        readPads();

        if (getKeyOn(gSelectButton == KEY_CIRCLE ? KEY_CROSS : KEY_CIRCLE))
            terminate = 1;
        else if (getKeyOn(gSelectButton))
            terminate = 2;

        if (ui)
            diaRenderUI(ui, screenHandler->inMenu, NULL, 0);
        else
            guiShow();

        rmDrawRect(0, 0, screenWidth, screenHeight, gColDarker);

        rmDrawLine(50, 75, screenWidth - 50, 75, gColWhite);
        rmDrawLine(50, 410, screenWidth - 50, 410, gColWhite);

        fntRenderString(gTheme->fonts[0], screenWidth >> 1, gTheme->usedHeight >> 1, ALIGN_CENTER, 0, 0, text, gTheme->textColor);
        guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? CROSS_ICON : CIRCLE_ICON, _STR_BACK, gTheme->fonts[0], 500, 417, gTheme->selTextColor);
        if (addAccept)
            guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? CIRCLE_ICON : CROSS_ICON, _STR_ACCEPT, gTheme->fonts[0], 70, 417, gTheme->selTextColor);

        guiEndFrame();
    }

    if (terminate == 1) {
        sfxPlay(SFX_CANCEL);
    }
    if (terminate == 2) {
        sfxPlay(SFX_CONFIRM);
    }

    return terminate - 1;
}

void guiHandleDeferedIO(int *ptr, const char *message, int type, void *data, int timeoutMs)
{
    // Play out any rumble pulse BEFORE we block (#172). This function is almost always entered a few
    // ms after an sfxPlay(SFX_CONFIRM) armed a 110ms bump, and its wait loop below renders every frame
    // but never calls readPads() -- which is the ONLY thing that decays the pulse and the only thing
    // that sends the "off". The IOP LATCHES the actuator value, so the motor does not need re-sending
    // to keep spinning: it simply runs for the entire config write / device scan. That is what the
    // hardware reporter felt as "really intense when you save a setting" -- it was never intensity,
    // it was DURATION. Bounded by RUMBLE_BUMP_MS and inert when nothing is armed or rumble is off.
    padRumbleFlush();

    // Free the shared IOP/fileXio channel before running the deferred IO. The
    // cover-art worker's queued and in-flight reads otherwise tie up that single
    // channel, so a config write (e.g. the last-played save on game launch) queues
    // behind them and the screen freezes on "Saving config..." after browsing the
    // list (issue #45 -- confirmed on hardware: disabling cover art avoids it).
    // Cancel queued cover loads, abort a slow in-flight MMCE read, and drain. The
    // drain returns as soon as the art queue is empty (cacheWaitForAllRequestsTimed
    // early-exits when nothing is queued/active), so this adds no delay in the
    // common case; the timeout only bounds a genuinely stuck read.
    int abortOk = cacheAbortMmceImageLoadsTimed(500);
    int cancelOk = cacheCancelPendingImageLoadsTimed(500);
    if (!abortOk || !cancelOk) {
        // A cover-art read did not drain within the timeout -- most likely a slow
        // (not dead) storage device. We still issue the deferred IO below: bailing
        // here would silently drop a valid config save on a merely-slow card, and
        // could not unwedge a genuinely stuck IOP RPC channel anyway. Logged so a
        // true hardware hang is diagnosable rather than a silent freeze on the
        // unbounded wait below (Codex audit, Medium 1).
        LOG("guiHandleDeferedIO: art drain timed out; deferred IO may stall on stuck storage\n");
    }

    if (ioPutRequest(type, data) != IO_OK) {
        *ptr = 0;
        return;
    }

    // Belt-and-suspenders: lower our (GUI thread) priority while busy-waiting so
    // any art work that slips in afterwards can still reach a yield point and
    // release the channel. Restored before returning; the handshake is unchanged.
    int savedPriority = cacheLowerCallerPriority();

    // Bound the wait when the caller asks (timeoutMs > 0). A deferred IO that never completes -- a
    // failing HDD/card that wedges the single IOP fileXio channel mid-write -- would otherwise spin
    // here forever and freeze the GUI on "Saving..." with no escape (reported: config save never
    // finishing on a flaky HDD). After a timeout far beyond any real config save/load we stop waiting
    // and clear the status, so the caller falls into its normal failure path (saveConfig -> "Error
    // saving settings"), exactly like the ioPutRequest-failure branch above. This does NOT recover
    // the stuck IOP -- the IO worker runs requests one at a time and stays blocked on the wedged one
    // until reboot -- but a readable error beats an apparent hard lock. Memory-safe: every caller's
    // *ptr is a file-static int and `data` a file-static handler, so a late write by the still-blocked
    // IO thread is harmless. timeoutMs <= 0 keeps the original unbounded wait (compat-list update).
    // clock() is microseconds (CLOCKS_PER_SEC = 1e6); the (clock() - startTick) elapsed form is
    // single-wrap-safe, unlike an absolute clock()+limit deadline.
    clock_t startTick = clock();
    clock_t limitTicks = (clock_t)timeoutMs * (CLOCKS_PER_SEC / 1000);
    while (*ptr) {
        if (timeoutMs > 0 && (clock() - startTick) >= limitTicks) {
            LOG("guiHandleDeferedIO: deferred IO unfinished after %d ms; storage stuck, abandoning wait\n", timeoutMs);
            *ptr = 0;
            break;
        }
        guiRenderTextScreen(message);
    }

    cacheRestoreCallerPriority(savedPriority);
}

void guiGameHandleDeferedIO(int *ptr, struct UIItem *ui, int type, void *data)
{
    // Same rumble-vs-blocking-work trap as guiHandleDeferedIO -- see the note there. The wait loop
    // below has the identical shape: it renders every frame and never polls readPads(), so a bump
    // armed by the SFX_CONFIRM that got us here would run for the whole deferred load. Reached from
    // gameMenuLoadConfig() on every per-game settings sub-dialog.
    padRumbleFlush();

    if (ioPutRequest(type, data) != IO_OK) {
        *ptr = 0;
        return;
    }

    while (*ptr) {
        guiStartFrame();
        if (ui)
            diaRenderUI(ui, screenHandler->inMenu, NULL, 0);
        else
            guiShow();
        guiEndFrame();
    }
}

void guiRenderTextScreen(const char *message)
{
    // Same transition contract as guiMsgBox: land any in-flight fade so the wait screen does not
    // sit on the old screen mid-transition.
    guiSnapTransition();

    guiStartFrame();

    guiShow();

    rmDrawRect(0, 0, screenWidth, screenHeight, gColDarker);

    fntRenderString(gTheme->fonts[0], screenWidth >> 1, gTheme->usedHeight >> 1, ALIGN_CENTER, 0, 0, message, gTheme->textColor);

    guiDrawOverlays();

    guiEndFrame();
}

// ---- Visual GameID barcode (Pixel FX / RetroGEM / PS2Digital HDMI auto-profile) ----
// Renders the CosmicScale "GameID" barcode just before a game is handed to its core, so an HDMI
// scaler can auto-load that game's per-title display profile. Encoding is the canonical CosmicScale
// scheme (start word 0xA5 / end word 0xD5 / length byte / additive 0x100-sum checksum), drawn with
// rmDrawRect exactly as CosmicScale's own OPL fork does. Gated behind gApplyGameID, which has been
// default ON since 120045d0 -- the pattern is meaningless to non-GameID displays, and the actual HDMI
// latch is only verifiable on real GameID hardware (experimental until a tester confirms).
//
// NOTE (#269): "imperceptible on other displays" was only true once guiShowGameID stopped LEAVING the
// barcode on screen. It is the last thing OPL draws before the ELF handoff, and on composite the
// 1-pixel-pitch strip averages to a solid white bar that the GS scans out for the whole game load.
// The trailing clean-frame loop at the end of guiShowGameID is what makes that premise hold.

#define GAMEID_HOLD_FRAMES 45 // ~0.75s @ 60fps -- enough stable frames for a scaler to sample

// Normalise a startup id into the serial the GameID device expects: drop a POPS "XX."/"SB." prefix and
// a trailing ".elf"/".ELF", cap at 11 chars (e.g. "SLUS_200.02"). Copied VERBATIM (no case fold) to
// stay byte-identical to CosmicScale's HW-validated guiSetGameId; retail serials are already uppercase.
static void gameIDCleanSerial(const char *startup, char *out, int outSize)
{
    int i = 0, len;
    const char *src = startup;

    out[0] = '\0';
    if (src == NULL)
        return;

    if (!strncmp(src, "XX.", 3) || !strncmp(src, "SB.", 3))
        src += 3;

    while (src[i] != '\0' && i < outSize - 1) {
        out[i] = src[i];
        i++;
    }
    out[i] = '\0';

    len = (int)strlen(out);
    if (len >= 4 && !strcasecmp(&out[len - 4], ".elf"))
        out[len - 4] = '\0';
    if (strlen(out) > 11)
        out[11] = '\0';
}

// Build the GameID packet from a cleaned serial; returns its length in bytes.
static int gameIDBuildPacket(u8 *data, const char *serial)
{
    int n = 0, i, sum = 0, crcpos;
    int gidlen = (int)strlen(serial);
    if (gidlen > 11)
        gidlen = 11;

    data[n++] = 0xA5;       // start / detect word
    data[n++] = 0x00;       // address offset
    crcpos = n++;           // checksum placeholder (data[2])
    data[n++] = (u8)gidlen; // payload length
    for (i = 0; i < gidlen; i++)
        data[n++] = (u8)serial[i];
    data[n++] = 0x00;
    data[n++] = 0xD5; // end word
    data[n++] = 0x00; // padding

    for (i = 3; i < n; i++) // additive 8-bit checksum over {length byte .. end}
        sum += data[i];
    data[crcpos] = (u8)(0x100 - (sum & 0xFF));
    return n;
}

// Draw the barcode once: per data bit (MSB-first) a magenta clock column + a cyan(1)/yellow(0) column.
static void gameIDDrawBars(const char *startup)
{
    u8 data[32];
    char serial[16];
    int data_len, i, ii, xstart, ystart;

    gameIDCleanSerial(startup, serial, sizeof(serial));
    if (serial[0] == '\0')
        return;

    data_len = gameIDBuildPacket(data, serial);
    xstart = (screenWidth / 2) - (data_len * 8); // centered horizontally
    ystart = screenHeight - ((screenHeight / 8) * 2 + 20);

    for (i = 0; i < data_len; i++) {
        for (ii = 7; ii >= 0; ii--) {
            int x = xstart + (i * 16 + (7 - ii) * 2);
            rmDrawRect(x, ystart, 1, 2, GS_SETREG_RGBA(0xFF, 0x00, 0xFF, 0x80)); // magenta clock col
            rmDrawRect(x + 1, ystart, 1, 2,
                       ((data[i] >> ii) & 1) ? GS_SETREG_RGBA(0x00, 0xFF, 0xFF, 0x80) // cyan = 1
                                               :
                                               GS_SETREG_RGBA(0xFF, 0xFF, 0x00, 0x80)); // yellow = 0
        }
    }
}

void guiShowGameID(const char *startup)
{
    int frame;

    if (!gApplyGameID || startup == NULL || startup[0] == '\0')
        return;

    // Hold the barcode on a clean black field for a few frames so an HDMI scaler can latch it.
    // The loader rides these already-pumped frames (#299): this hold is the one place on the
    // synchronous launch path that still renders, so drawing the busy animation here makes it
    // genuinely cycle during launch prep. Bars drawn LAST so the barcode strip stays on top.
    for (frame = 0; frame < GAMEID_HOLD_FRAMES; frame++) {
        guiStartFrame();
        rmDrawRect(0, 0, screenWidth, screenHeight, GS_SETREG_RGBA(0x00, 0x00, 0x00, 0x80));
        guiDrawBusy(0x80);
        gameIDDrawBars(startup);
        guiEndFrame();
    }

    /*
      Leave a CLEAN black frame behind (#269) -- clean of the BARCODE, that is; the loader is drawn
      on these frames on purpose (#299, below).

      This is the last thing the GameID path renders: itemExecSelect() calls us and then goes
      straight into itemLaunch() -> deinit() -> ExecPS2(), and on a default config nothing in
      between draws anything (gRememberLastPlayed is off, so there is no save toast; no cheats, no
      VMC, no MX4SIO warning). Neither deinit() nor sysLaunchLoaderElf() reprograms a GS display
      register, so whatever buffer rmEndFrame last pointed DISPFB2 at keeps being scanned out until
      the GAME programs the GS -- which on USB is minutes away.

      Without this, that buffer is "black field + barcode strip", and the strip is drawn at
      1-virtual-pixel pitch (magenta column immediately followed by cyan/yellow). That is below the
      chroma bandwidth of composite output, so the columns average to near-white and the user sees
      a solid ~288x2 white bar, ~71% down an otherwise black screen, for the entire load. Exactly
      the report: every game, unrelated to Debug Colors, lasting as long as the media takes.

      Two frames because rendering is double-buffered -- one alone leaves the barcode in the OTHER
      buffer, which is the one a scaler may resync to across the video-mode change.

      #299: the busy animation is drawn onto both clean frames, so nothing but "black field +
      loader" is left in EITHER buffer -- the loading indicator issue #299 asks for. The themed
      load*.png pixmaps are ordinary composite-safe art, nothing like the 1-pixel-pitch barcode
      strip above, so the #269 premise is untouched. (itemExecSelect also pumps one more
      guiRenderBusyFrame() after us, which is what covers the GameID-off config where this whole
      function is a no-op.)
    */
    for (frame = 0; frame < 2; frame++) {
        guiStartFrame();
        rmDrawRect(0, 0, screenWidth, screenHeight, GS_SETREG_RGBA(0x00, 0x00, 0x00, 0x80));
        guiDrawBusy(0x80);
        guiEndFrame();
    }
}

void guiWarning(const char *text, int count)
{
    // Pre-GUI callers exist: autolaunch (miniInit) never runs rmInit/thmInit/guiInit, so
    // rendering here would draw through a NULL gsGlobal/gTheme. Launch-path helpers shared
    // between menu and autolaunch (the Δ2/Δ8/Δ9 toasts) rely on this guard instead of each
    // call site checking the autolaunch globals.
    if (gTheme == NULL) {
        LOG("guiWarning (pre-GUI): %s\n", text);
        return;
    }

    guiStartFrame();

    guiShow();

    rmDrawRect(0, 0, screenWidth, screenHeight, gColDarker);

    rmDrawLine(50, 75, screenWidth - 50, 75, gColWhite);
    rmDrawLine(50, 410, screenWidth - 50, 410, gColWhite);

    fntRenderString(gTheme->fonts[0], screenWidth >> 1, gTheme->usedHeight >> 1, ALIGN_CENTER, screenWidth, screenHeight, text, gTheme->textColor);

    guiEndFrame();

    delay(count);
}

int guiConfirmVideoMode(void)
{
    clock_t timeEnd;
    int terminate = 0;

    sfxPlay(SFX_MESSAGE);

    timeEnd = clock() + OPL_VMODE_CHANGE_CONFIRMATION_TIMEOUT_MS * (CLOCKS_PER_SEC / 1000);
    while (!terminate) {
        guiStartFrame();

        readPads();

        if (getKeyOn(gSelectButton == KEY_CIRCLE ? KEY_CROSS : KEY_CIRCLE))
            terminate = 1;
        else if (getKeyOn(gSelectButton))
            terminate = 2;

        // If the user fails to respond within the timeout period, deem it as a cancel operation.
        if (clock() > timeEnd)
            terminate = 1;

        guiShow();

        rmDrawRect(0, 0, screenWidth, screenHeight, gColDarker);

        rmDrawLine(50, 75, screenWidth - 50, 75, gColWhite);
        rmDrawLine(50, 410, screenWidth - 50, 410, gColWhite);

        fntRenderString(gTheme->fonts[0], screenWidth >> 1, gTheme->usedHeight >> 1, ALIGN_CENTER, 0, 0, _l(_STR_CFM_VMODE_CHG), gTheme->textColor);
        guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? CROSS_ICON : CIRCLE_ICON, _STR_BACK, gTheme->fonts[0], 500, 417, gTheme->selTextColor);
        guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? CIRCLE_ICON : CROSS_ICON, _STR_ACCEPT, gTheme->fonts[0], 70, 417, gTheme->selTextColor);

        guiEndFrame();
    }

    if (terminate == 1) {
        sfxPlay(SFX_CANCEL);
    }
    if (terminate == 2) {
        sfxPlay(SFX_CONFIRM);
    }

    return terminate - 1;
}

int guiGameShowRemoveSettings(config_set_t *configSet, config_set_t *configGame)
{
    int terminate = 0;
    char message[256];

    sfxPlay(SFX_MESSAGE);

    while (!terminate) {
        guiStartFrame();

        readPads();

        if (getKeyOn(gSelectButton == KEY_CIRCLE ? KEY_CROSS : KEY_CIRCLE))
            terminate = 1;
        else if (getKeyOn(gSelectButton))
            terminate = 2;
        else if (getKeyOn(KEY_SQUARE))
            terminate = 3;
        else if (getKeyOn(KEY_TRIANGLE))
            terminate = 4;

        guiShow();

        rmDrawRect(0, 0, screenWidth, screenHeight, gColDarker);

        rmDrawLine(50, 75, screenWidth - 50, 75, gColWhite);
        rmDrawLine(50, 410, screenWidth - 50, 410, gColWhite);

        fntRenderString(gTheme->fonts[0], screenWidth >> 1, gTheme->usedHeight >> 1, ALIGN_CENTER, 0, 0, _l(_STR_GAME_SETTINGS_PROMPT), gTheme->textColor);

        guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? CROSS_ICON : CIRCLE_ICON, _STR_BACK, gTheme->fonts[0], 500, 417, gTheme->selTextColor);
        guiDrawIconAndText(SQUARE_ICON, _STR_GLOBAL_SETTINGS, gTheme->fonts[0], 213, 417, gTheme->selTextColor);
        guiDrawIconAndText(TRIANGLE_ICON, _STR_ALL_SETTINGS, gTheme->fonts[0], 356, 417, gTheme->selTextColor);
        guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? CIRCLE_ICON : CROSS_ICON, _STR_PERGAME_SETTINGS, gTheme->fonts[0], 70, 417, gTheme->selTextColor);

        guiEndFrame();
    }

    if (terminate == 1) {
        sfxPlay(SFX_CANCEL);
        return 0;
    } else if (terminate == 2) {
        guiGameRemoveSettings(configSet);
        snprintf(message, sizeof(message), _l(_STR_GAME_SETTINGS_REMOVED), _l(_STR_PERGAME_SETTINGS));
    } else if (terminate == 3) {
        guiGameRemoveGlobalSettings(configGame);
        snprintf(message, sizeof(message), _l(_STR_GAME_SETTINGS_REMOVED), _l(_STR_GLOBAL_SETTINGS));
    } else if (terminate == 4) {
        guiGameRemoveSettings(configSet);
        guiGameRemoveGlobalSettings(configGame);
        snprintf(message, sizeof(message), _l(_STR_GAME_SETTINGS_REMOVED), _l(_STR_ALL_SETTINGS));
    }
    sfxPlay(SFX_CONFIRM);
    guiMsgBox(message, 0, NULL);

    return 1;
}

void guiManageCheats(void)
{
    int offset = 0;
    int terminate = 0;
    int cheatCount = 0;
    int selectedCheat = 0;
    int visibleCheats = 10; // Maximum number of cheats visible on screen

    if (gCheats == NULL) // defensive: the menu is only reachable after load_cheats, but never deref NULL
        return;

    while (cheatCount < MAX_CODES && strlen(gCheats[cheatCount].name) > 0)
        cheatCount++;

    sfxPlay(SFX_MESSAGE);

    while (!terminate) {
        guiStartFrame();
        readPads();

        if (getKeyOn(KEY_UP) && selectedCheat > 0) {
            selectedCheat -= 1;
            if (selectedCheat < offset)
                offset = selectedCheat;
        }

        if (getKeyOn(KEY_DOWN) && selectedCheat < cheatCount - 1) {
            selectedCheat += 1;
            if (selectedCheat >= offset + visibleCheats)
                offset = selectedCheat - visibleCheats + 1;
        }

        if (getKeyOn(gSelectButton)) {
            if (!(strncasecmp(gCheats[selectedCheat].name, "mastercode", 10) == 0 || strncasecmp(gCheats[selectedCheat].name, "master code", 11) == 0))
                gCheats[selectedCheat].enabled = !gCheats[selectedCheat].enabled;
        }

        if (getKeyOn(KEY_SQUARE)) {
            // Disable All: clear every cheat's enabled flag in one press. Skip the mastercode (the
            // per-cheat toggle can't touch it either -- it is the engine enabler, not a cheat).
            for (int i = 0; i < cheatCount; i++) {
                if (!(strncasecmp(gCheats[i].name, "mastercode", 10) == 0 || strncasecmp(gCheats[i].name, "master code", 11) == 0))
                    gCheats[i].enabled = 0;
            }
            sfxPlay(SFX_CURSOR);
        }

        if (getKeyOn(KEY_START))
            terminate = 1;

        guiShow();

        rmDrawRect(0, 0, screenWidth, screenHeight, gColDarker);
        rmDrawLine(50, 75, screenWidth - 50, 75, gColWhite);
        rmDrawLine(50, 410, screenWidth - 50, 410, gColWhite);

        fntRenderString(gTheme->fonts[0], screenWidth >> 1, 60, ALIGN_CENTER, 0, 0, _l(_STR_CHEAT_SELECTION), gTheme->textColor);

        int renderedCheats = 0;
        for (int i = offset; renderedCheats < visibleCheats && i < cheatCount; i++) {
            if (strlen(gCheats[i].name) == 0)
                continue;

            int enabled = gCheats[i].enabled;

            int boxX = 50;
            int boxY = 100 + (renderedCheats * 30);
            int boxWidth = rmWideScale(25);
            int boxHeight = 17;

            if (enabled) {
                rmDrawRect(boxX, boxY + 3, boxWidth, boxHeight, gTheme->textColor);
                rmDrawRect(boxX + 2, boxY + 5, boxWidth - 4, boxHeight - 4, gTheme->selTextColor);
            }

            u32 textColour = (i == selectedCheat) ? gTheme->selTextColor : gTheme->textColor;
            fntRenderString(gTheme->fonts[0], boxX + 35, boxY + 3, ALIGN_LEFT, 0, 0, gCheats[i].name, textColour);

            renderedCheats++;
        }

        guiDrawIconAndText(gSelectButton == KEY_CIRCLE ? CIRCLE_ICON : CROSS_ICON, _STR_SELECT, gTheme->fonts[0], 70, 417, gTheme->selTextColor);
        guiDrawIconAndText(SQUARE_ICON, _STR_DISABLE_ALL, gTheme->fonts[0], 270, 417, gTheme->selTextColor);
        guiDrawIconAndText(START_ICON, _STR_RUN, gTheme->fonts[0], 500, 417, gTheme->selTextColor);

        guiEndFrame();
    }

    sfxPlay(SFX_CONFIRM);
}
