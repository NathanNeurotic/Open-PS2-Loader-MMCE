/*
 Copyright 2010, Volca
 Licenced under Academic Free License version 3.0
 Review OpenUsbLd README & LICENSE files for further details.
 */

#include "include/opl.h"
#include "include/gui.h"
#include "include/diag.h" // gLastDeferredTimedOut -- a bounded wait that expired means the handler never ran
#include "include/renderman.h"
#include "include/menusys.h"
#include "include/fntsys.h"
#include "include/ioman.h"
#include "include/lang.h"
#include "include/themes.h"
#include "include/favsupport.h" // gFAVStartMode -- Favourites Start Mode row
#include "include/pad.h"
#include "include/sound.h" // sfxGetPlayDiag/sfxGetDropDiag -- SFX RPC cost + silent-drop counters for the debug HUD
#include "include/util.h"
#include "include/config.h"
#include "include/system.h"
#include "include/mmcesupport.h"
#include "include/ethsupport.h"
#include "include/udpfssupport.h" // udpfsGetModulesLoaded() -- network-protocol restart-notice check
#include "include/artindex.h"
#include "include/bdmsupport.h" // bdmIsUDPBDLoaded() + bdmForceDeviceRefresh()
#include "include/hddsupport.h" // staged normal APA OPL-home selector
#include "include/vcdsupport.h" // POPStarter pages: BDMA equip, list options, POPS net config
#include "include/libview.h"    // libViewActive / libListViewActive -- which list this page shows
#include "include/compatupd.h"
#include "include/pggsm.h"
#include "include/cheatman.h"
#include "include/sound.h"
#include "include/guigame.h"
#include "include/texcache.h"
#include "include/appsupport.h" // appGetObject() -- push a live Art Delay change onto the APPS list
#include "include/tar.h"        // tarInvalidate -- re-arm the .tar probe when the toggle flips

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

// The eight Settings peer screens reuse the established dialog definitions. Composite screens are
// assembled into scratch buffers at entry so the existing field ids, visibility rules and temporary
// editor buttons remain owned by their original feature code. Values still live in the existing
// globals/config sets; these buffers are only views, never a second configuration store.
#define SETTINGS_DIALOG_CAPACITY 256
static struct UIItem guiSettingsDialog[SETTINGS_DIALOG_CAPACITY];
// MMCE Settings can be opened from the composed Game Sources page. Keep its editor separate so
// returning from that child cannot overwrite the parent's dialog contents.
static struct UIItem guiMmceSettingsDialog[SETTINGS_DIALOG_CAPACITY];
static struct UIItem *guiSettingsActiveDialog;
static int guiSettingsShellActive;
static int guiSettingsCurrentPage;
static int guiSettingsSavePending;
static char guiSettingsPageIndicator[32];

static int guiSettingsIsShellResult(int result);
static int guiSettingsPageResult(int result);
static int guiSettingsPromptSave(void);
static void guiSettingsBeginDialog(struct UIItem *ui);
static void guiSettingsEndDialog(void);
static struct UIItem *guiSettingsCompose(const struct UIItem *const *parts, int partCount,
                                         const int *skipIDs, int skipCount, int skipPart,
                                         int suppressSecondaryHeaders);
static struct UIItem *guiSettingsComposeInto(struct UIItem *dialog, const struct UIItem *const *parts,
                                             int partCount, const int *skipIDs, int skipCount, int skipPart,
                                             int suppressSecondaryHeaders);

// Notification popup: START tick + how long to hold, NOT an absolute deadline. clock() is a
// 32-bit microsecond counter, so it wraps every ~71.6 minutes; `clock() >= start + duration`
// silently becomes false-for-71-minutes whenever the sum crosses the wrap. The (clock() - start)
// ELAPSED form is correct across one wrap by ordinary unsigned arithmetic.
static clock_t popupStart;
static int popupArmed;

// How far ABOVE the vertical centre the boot logo (static splash and animated frames alike) sits.
// The version line follows it, so the whole boot block moves together; the status line stays
// anchored near the bottom edge.
#define BOOT_LOGO_RISE 100

// Boot-splash status line: set via guiSetBootStatus(), drawn under the logo by
// guiRenderGreeting(). Both on the MAIN thread, so gBootStatus needs no locking.
static char gBootStatus[64] = {0};
static int gBootStatusActive = 0;
// Boot-step localizer: a deferred IO-thread boot step can wedge with no timeout on real
// hardware and freeze the splash, while the MAIN thread races ahead setting "Ready." -- the
// frozen screen would then show a useless "Ready." instead of the stuck step. An IO-thread
// step publishes its label via guiSetBootStatusSticky(); guiRenderGreeting PREFERS it over
// gBootStatus, so whichever ordering wins the STUCK STEP is what stays on screen. Cross-thread
// state is a single aligned POINTER (atomic load/store on the EE) to a static _l() string --
// no shared buffer, so no data race. Cleared by guiSetBootStatus(NULL).
static const char *volatile gBootStickyLabel = NULL;
// Dynamic diagnostic labels cannot use guiSetBootStatusSticky(), which deliberately stores the
// caller's pointer. Keep two owned buffers and always format into the one that is not published.
// The EE is single-core: if the renderer is pre-empted after loading one pointer, the IO thread
// only writes the other buffer, then atomically publishes that pointer.
static char gBootStickyCopy[2][64];

// forward decl.
static void guiShow();
static void guiDrawOverlays(void);
static const char **guiCopyNameList(const char **src);
static void guiFreeNameList(const char **list);


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

// Frame-time watchdog for the debug HUD. It answers the one question nobody asked about the periodic
// swallowed navigation step: DOES THE EE STALL AT ALL when it happens?
//
// Every theory this session assumed the menu thread stops for a moment. If it does, this catches the
// overrun. If frames stay on vsync straight THROUGH a swallowed step, the EE polled the pad on time
// and freepad had nothing to give it -- the input died on the IOP over SIO2, and no EE-side work
// (thread priority, art queues, device probing) could ever have fixed it. Which would explain why
// none of it has.
static u32 gFrameLastUs = 0;
static u32 gFrameWorstUs = 0;
static u32 gFrameOverruns = 0; // frames longer than ~1.5 vsyncs

u32 guiGetFrameWatchdog(u32 *worstUs, u32 *overruns)
{
    if (worstUs)
        *worstUs = gFrameWorstUs;
    if (overruns)
        *overruns = gFrameOverruns;
    return gFrameLastUs;
}

void guiEndFrame(void)
{
    rmEndFrame();
    static clock_t frameMark = 0;

    // clock() is microseconds on the EE. Sampled after rmEndFrame (i.e. after the vsync wait) so
    // this measures whole frames back to back.
    clock_t nowUs = clock();
    if (frameMark != 0) {
        u32 elapsed = (u32)(nowUs - frameMark);
        gFrameLastUs = elapsed;
        if (elapsed > gFrameWorstUs)
            gFrameWorstUs = elapsed;
        if (elapsed > 25000)
            gFrameOverruns++;
    }
    frameMark = nowUs;

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
            // Only DISK themes have a path to announce; the built-ins (<OPL>, <Coverflow>) return NULL
            // from thmGetFilePath. The old `!= 0` test was correct only while theme IDs ran 0..nThemes;
            // this tree's themes.c adds a built-in at nThemes+1, so a saved <Coverflow> passed the test
            // and fed NULL to the "%s" in _STR_THM_NOTIFICATION on EVERY boot. Coverflow is our default.
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
    popupArmed = 0;
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
        if (!popupArmed) {
            popupStart = clock();
            popupArmed = 1;
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
            snprintf(notification, sizeof(notification), _l(_STR_CFG_NOTIFICATION), configGetLoadDir());
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

        if ((clock() - popupStart) >= (clock_t)5000 * (CLOCKS_PER_SEC / 1000)) {
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

// Legacy standalone General editor. The Settings peer shell is the normal route, but this stays
// functional for callers outside it and shares the same Language/GSM backends.
void guiShowConfig()
{
    const char **langNamesSnap = NULL;
    int langID;
    int ret;

    // Exit To auto-resolves a built-in default when blank, so show a dim "Default" placeholder
    // rather than "<not set>" -- the empty value (and thus the fallback) is kept.
    diaSetShowDefaultWhenEmpty(diaConfig, CFG_EXITTO, 1);
    diaSetString(diaConfig, CFG_EXITTO, gExitPath);
    // Custom Settings Path: blank = the normal boot-dir/discovery home, so it gets the same dim
    // "Default" placeholder treatment rather than "<not set>".
    diaSetShowDefaultWhenEmpty(diaConfig, CFG_CUSTOMCFGPATH, 1);
    diaSetString(diaConfig, CFG_CUSTOMCFGPATH, gCustomSettingsPath);

    diaSetInt(diaConfig, CFG_LASTPLAYED, gRememberLastPlayed);
    diaSetInt(diaConfig, CFG_FOLDERNAV, gEnableFolderNav);
    diaSetInt(diaConfig, CFG_AUTOSTARTLAST, gAutoStartLastPlayed);
    diaSetVisible(diaConfig, CFG_AUTOSTARTLAST, gRememberLastPlayed);
    diaSetVisible(diaConfig, CFG_LBL_AUTOSTARTLAST, gRememberLastPlayed);

    guiLock();
    langNamesSnap = guiCopyNameList((const char **)lngGetGuiList());
    guiUnlock();
    diaSetEnum(diaConfig, UICFG_LANG, langNamesSnap != NULL ? langNamesSnap : (const char **)lngGetGuiList());
    diaSetInt(diaConfig, UICFG_LANG, lngGetGuiValue());

reshow_config:
    ret = diaExecuteDialog(diaConfig, -1, 1, &guiUpdater);
    if (ret == GENERAL_GSM_DEFAULTS_BUTTON) {
        guiGameShowGSConfig(1);
        goto reshow_config;
    }
    if (ret) {
        diaGetInt(diaConfig, UICFG_LANG, &langID);
        diaGetString(diaConfig, CFG_EXITTO, gExitPath, sizeof(gExitPath));
        diaGetString(diaConfig, CFG_CUSTOMCFGPATH, gCustomSettingsPath, sizeof(gCustomSettingsPath));
        diaGetInt(diaConfig, CFG_LASTPLAYED, &gRememberLastPlayed);
        diaGetInt(diaConfig, CFG_FOLDERNAV, &gEnableFolderNav);
        diaGetInt(diaConfig, CFG_AUTOSTARTLAST, &gAutoStartLastPlayed);

        DisableCron = 1; // Disable Auto Start Last Played counter (we don't want to call it right after enable it on GUI)

        applyConfig(-1, langID, 0);
        menuReinitMainMenu();
    }

    guiFreeNameList(langNamesSnap);
}

// Game Sources page: device start modes, the default device, and the block-device enables
// (inlined; the separate Block Devices sub-dialog is gone).
// What the NIC is ACTUALLY running, as opposed to what the config asks for. The three transports are
// mutually exclusive on the single SMAP NIC -- each of the three loaders refuses to start while either
// of the others is resident (bdmsupport.c's UDPBD gate, ethsupport.c's SMB gate, udpfssupport.c's
// UDPFS gate) -- so at most one of these can answer, and the order below is only for definiteness.
int guiGetResidentNetProtocol(void)
{
    if (ethGetModulesLoaded())
        return NET_PROTO_SMB;
    if (udpfsGetModulesLoaded())
        return NET_PROTO_UDPFS;
    if (bdmIsUDPBDLoaded())
        return (bdmGetLoadedNetProtocol() == NET_BOOT_UDPFS) ? NET_PROTO_UDPFSBD : NET_PROTO_UDPBD;

    return NET_PROTO_OFF;
}

// Does the saved choice disagree with what is live? Nothing is resident -> the next start is free to
// be anything, so there is nothing to restart FOR. This is deliberately a comparison against the
// RESIDENT protocol rather than against the protocol the picker happened to open with: a user who
// switches away and back within one session ends up agreeing with the NIC again, and must not then be
// nagged to restart for a change that is no longer a change.
int guiNetProtocolNeedsRestart(void)
{
    int resident = guiGetResidentNetProtocol();

    if (resident == NET_PROTO_OFF)
        return 0;

    return gNetworkProtocol != resident;
}

// guiShowDeviceConfig is retained for the legacy entry point outside the Settings peer shell.
// Keep its APA selector semantics identical to the shell: merely opening/saving another field
// must not normalize a legacy custom hdd_partition, while an explicit selector interaction may.

static int guiDeviceConfigUpdater(int modified)
{
    (void)modified;
    return 0;
}

void guiShowDeviceConfig(void)
{
    const char *deviceNames[] = {_l(_STR_BDM_GAMES), _l(_STR_NET_GAMES), _l(_STR_HDD_GAMES), _l(_STR_APPS), _l(_STR_MMCE), _l(_STR_FAV), NULL};
    const char *deviceModes[] = {_l(_STR_OFF), _l(_STR_MANUAL), _l(_STR_AUTO), NULL};
    static const char *hddOplHomes[] = {"__common/OPL/", "+OPL/", NULL};

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
    diaSetEnabled(diaDeviceConfig, CFG_ENABLEBDMHDD, 1); // coexists with APA
    diaSetEnabled(diaDeviceConfig, CFG_HDDMODE, 1);

    // Network Start Mode (Off/Manual/Auto) == gNetStartMode (START_MODE_*); the SAME three options
    // (and indices) as every other device's start row, so reuse the localized deviceModes.
    // The Protocol/Access rows live on the Network page.
    diaSetEnum(diaDeviceConfig, CFG_NETSTART, deviceModes);
    diaSetInt(diaDeviceConfig, CFG_NETSTART, gNetStartMode);

    // MMCE Start Mode
    diaSetEnum(diaDeviceConfig, CFG_MMCEMODE, deviceModes);
    diaSetInt(diaDeviceConfig, CFG_MMCEMODE, gMMCEStartMode);
    diaSetEnabled(diaDeviceConfig, CFG_MMCEMODE, 1);

    int ret;
reshow_device:
    ret = diaExecuteDialog(diaDeviceConfig, -1, 1, &guiDeviceConfigUpdater);
    if (ret == MMCE_SETTINGS_BUTTON) {
        guiShowMmceConfig();
        goto reshow_device;
    }
    if (ret) {
        int netProtocolWas = gNetworkProtocol;

        diaGetInt(diaDeviceConfig, CFG_DEFDEVICE, &deviceModeIndex);
        gDefaultDevice = guiDeviceTypeToIoMode(deviceModeIndex);
        diaGetInt(diaDeviceConfig, CFG_BDMMODE, &gBDMStartMode);
        diaGetInt(diaDeviceConfig, CFG_HDDMODE, &gHDDStartMode);
        diaGetInt(diaDeviceConfig, CFG_APPMODE, &gAPPStartMode);
        diaGetInt(diaDeviceConfig, CFG_MMCEMODE, &gMMCEStartMode);
        diaGetInt(diaDeviceConfig, CFG_FAVMODE, &gFAVStartMode);

        diaGetInt(diaDeviceConfig, CFG_ENABLEUSB, &gEnableUSB);
        diaGetInt(diaDeviceConfig, CFG_ENABLEILK, &gEnableILK);
        diaGetInt(diaDeviceConfig, CFG_ENABLEMX4SIO, &gEnableMX4SIO);
        diaGetInt(diaDeviceConfig, CFG_ENABLEBDMHDD, &gEnableBdmHDD);

        // Network Start Mode read-back: Start=Off disables network start;
        // preserve the user's configured gNetworkProtocol (defaulting to SMB only if uninitialized).
        diaGetInt(diaDeviceConfig, CFG_NETSTART, &gNetStartMode);
        if (gNetStartMode == START_MODE_DISABLED)
            gNetworkProtocol = NET_PROTO_OFF;
        else if (gNetworkProtocol == NET_PROTO_OFF)
            gNetworkProtocol = NET_PROTO_SMB;
        gEnableUDPBD = (gNetworkProtocol == NET_PROTO_UDPBD || gNetworkProtocol == NET_PROTO_UDPFSBD);
        gNetBootProtocol = (gNetworkProtocol == NET_PROTO_UDPFSBD) ? NET_BOOT_UDPFS : NET_BOOT_UDPBD;
        // SMB's start mode IS the network start row (Auto = boot connect, Manual = on-entry);
        // every non-SMB protocol forces the SMB/ETH stack off so only one transport claims the NIC.
        gETHStartMode = (gNetworkProtocol == NET_PROTO_SMB) ? gNetStartMode : START_MODE_DISABLED;

        // "Nothing happens" guard: enabling a network protocol gives NO feedback -- the UDPFS tab
        // joins the ring silently (Manual start waits for a Confirm-press inside it), and the block
        // transports show a tab only once the PC server answers.
        if (gNetworkProtocol != netProtocolWas) {
            if (gNetworkProtocol == NET_PROTO_UDPFS)
                guiMsgBox(_l(_STR_NET_UDPFS_TAB_HINT), 0, NULL);
            else if (gNetworkProtocol == NET_PROTO_UDPFSBD || gNetworkProtocol == NET_PROTO_UDPBD)
                guiMsgBox(_l(_STR_NET_UDPBD_TAB_HINT), 0, NULL);
        }

        // Each network transport loads its IOP module chain once per boot (the load latch is not
        // cleared live). If a stack is already up and the Start toggle changed the protocol away from
        // the one actually running, the switch takes effect only after a restart -- say so instead of
        // silently doing nothing. The OFFER to restart deliberately lives on the SAVE path and not
        // here: this dialog only touches RAM, and Save Changes is a separate menu action, so acting
        // on it now would tear OPL down before the choice was ever written to disk.
        if (gNetworkProtocol != netProtocolWas && guiNetProtocolNeedsRestart())
            guiMsgBox(_l(_STR_NETBOOT_RESTART), 0, NULL);

        // A BDM tab can be latched hidden (bdmNeedsUpdate short-circuits until the device generation
        // bumps). Re-evaluate device visibility now so re-enabling a device here brings its tab back
        // without a physical replug.
        bdmForceDeviceRefresh();

        applyConfig(-1, -1, 0);
        menuReinitMainMenu();
    }
}

// MMCE page (settings-layout restructure, was MMCE Settings): SD2PSX / MemCard PRO2 basics plus
// communication tuning and the library path. The legacy child definitions remain available, but
// this entry composes their fields inline so the user does not need another navigation layer.
int guiShowMmceConfig(void)
{
    const struct UIItem *parts[] = {diaMmceConfig, diaMmceCommConfig, diaMmcePathConfig};
    const int skipIDs[] = {MMCE_COMM_BUTTON, MMCE_PATH_BUTTON};
    static const char *deviceSlots[] = {"0", "1", NULL, NULL};
    static const char *deviceIGRSlots[] = {"NONE", "0", "1", "BOTH", NULL};
    static const char *deviceAckWaitCycles[] = {"0", "1", "2", "3", "4", "5", NULL};
    static const char *deviceOnOff[] = {"OFF", "ON", NULL};
    struct UIItem *previousActiveDialog = guiSettingsActiveDialog;
    struct UIItem *ui;

    // diaSetEnum stores the array pointer rather than copying it. Keep the MMCE options alive for
    // the lifetime of the dedicated child dialog, including any parent re-entry after Circle.
    deviceSlots[2] = _l(_STR_AUTO);
    ui = guiSettingsComposeInto(guiMmceSettingsDialog, parts, 3, skipIDs, 2, -1, 1);

    if (ui == NULL)
        return -1;

    diaSetEnum(ui, CFG_MMCESLOT, deviceSlots);
    diaSetInt(ui, CFG_MMCESLOT, gMMCESlot);
    diaSetEnum(ui, CFG_MMCEIGRSLOT, deviceIGRSlots);
    diaSetInt(ui, CFG_MMCEIGRSLOT, gMMCEIGRSlot);
    diaSetInt(ui, CFG_MMCEGAMEID, gMMCEEnableGameID);
    diaSetEnum(ui, CFG_MMCE_WAIT_CYCLES, deviceAckWaitCycles);
    diaSetInt(ui, CFG_MMCE_WAIT_CYCLES, gMMCEAckWaitCycles);
    diaSetEnum(ui, CFG_MMCE_USE_ALARMS, deviceOnOff);
    diaSetInt(ui, CFG_MMCE_USE_ALARMS, gMMCEUseAlarms);
    diaSetString(ui, CFG_MMCEPREFIX, gMMCEPrefix);

    int ret = diaExecuteDialog(ui, -1, 1, NULL);
    if (ret == UIID_BTN_OK) {
        diaGetInt(ui, CFG_MMCESLOT, &gMMCESlot);
        diaGetInt(ui, CFG_MMCEIGRSLOT, &gMMCEIGRSlot);
        diaGetInt(ui, CFG_MMCEGAMEID, &gMMCEEnableGameID);
        diaGetInt(ui, CFG_MMCE_WAIT_CYCLES, &gMMCEAckWaitCycles);
        diaGetInt(ui, CFG_MMCE_USE_ALARMS, &gMMCEUseAlarms);
        diaGetString(ui, CFG_MMCEPREFIX, gMMCEPrefix, sizeof(gMMCEPrefix));
        // The Settings parent commits the complete Game Sources page after this child returns.
        // Legacy callers still own their own apply path.
        if (!guiSettingsShellActive) {
            applyConfig(-1, -1, 0);
            menuReinitMainMenu();
        }
    }

    guiSettingsActiveDialog = previousActiveDialog;
    return ret;
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

void guiShowUIConfig(void)
{
    int themeID = -1, langID = -1;
    showCfgPopup = 0;
    guiResetNotifications();

    const char **themeNamesSnap = NULL;
    const char **langNamesSnap = NULL;

    int previousTheme, previousVMode;
    const char *vmodeNames[] = {_l(_STR_AUTO), "PAL 640x512i @50Hz 24bit", "NTSC 640x448i @60Hz 24bit",
                                "EDTV 640x448p @60Hz 24bit", "EDTV 640x512p @50Hz 24bit", "VGA 640x480p @60Hz 24bit",
                                "PAL 704x576i @50Hz 24bit (HIRES)", "NTSC 704x480i @60Hz 24bit (HIRES)",
                                "EDTV 704x480p @60Hz 24bit (HIRES)", "EDTV 704x576p @50Hz 24bit (HIRES)",
                                "HDTV 1280x720p @60Hz 16bit (HIRES)", "HDTV 1920x1080i @60Hz 16bit (HIRES)",
                                "PAL 640x256p @50Hz 24bit", "NTSC 640x224p @60Hz 24bit", NULL};

reshow_ui:
    previousTheme = thmGetGuiValue();
    previousVMode = gVMode;
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
    const char *gameViewNames[] = {"Both", "PS2", "PS1", NULL};
    diaSetEnum(diaUIConfig, UICFG_GAMEVIEW, gameViewNames);
    diaSetInt(diaUIConfig, UICFG_GAMEVIEW, gDefaultGameView);
    diaSetEnum(diaUIConfig, UICFG_VMODE, vmodeNames);
    diaSetInt(diaUIConfig, UICFG_VMODE, gVMode);

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
    if (ret == UICFG_GAME_LIST_BUTTON) {
        guiShowVcdListConfig();
        goto reshow_ui;
    }

    // Play out the confirm bump the dialog just armed, before applyConfig() below tears down and
    // rebuilds the GS (rmSetMode), reloads the theme and its textures, and holds guiLock over a
    // submenu-cache rebuild -- none of which polls readPads(), so the pulse would run for all of it
    // (#172, "really intense ... after changing the resolution"). Deliberately HERE and not at the top
    // of applyConfig(): applyConfig is ALSO reached off the GUI thread, from _loadConfig() on the IO
    // worker (opl.c: guiHandleDeferedIO with IO_CUSTOM_SIMPLEACTION), and every libpad call in pad.c
    // is GUI-thread-only. This call site is the GUI thread, always.
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
            libViewMarkAllDirty(); // rebuild every VCD-capable page so the new default view takes effect
        }
        diaGetInt(diaUIConfig, UICFG_VMODE, &gVMode);

        if (previousTheme != themeID && isBgmPlaying())
            bgmStop();

        applyConfig(themeID, langID, 1);
        if (gameViewChanged) {
            // applyConfig(..., skipDeviceRefresh=1) deliberately avoids device scans. Queue after it
            // returns so HDD (which has no automatic refresh) cannot retain PS2 rows while rendering
            // uses the new VCD view, without racing the theme/menu bookkeeping above.
            oplQueueLibraryDeviceUpdates();
            loadFavourites(); // queued after source pages so the FAV resolver sees their rebuilt rows
        }
        sfxInit(0);

        if (gEnableBGM && !isBgmPlaying())
            bgmStart();

        if (previousVMode != gVMode && guiConfirmVideoMode() == 0) {
            gVMode = previousVMode;
            applyConfig(-1, -1, 1);
        }
    }

    guiFreeNameList(themeNamesSnap);
    guiFreeNameList(langNamesSnap);
}

static int netConfigUpdater(int modified)
{
    int showAdvancedOptions, isNetBIOS, isDHCPEnabled, netProto, isSMB, i;

    if (modified) {
        diaGetInt(diaNetConfig, NETCFG_SHOW_ADVANCED_OPTS, &showAdvancedOptions);

        diaGetInt(diaNetConfig, NETCFG_PS2_IP_ADDR_TYPE, &isDHCPEnabled);
        diaGetInt(diaNetConfig, NETCFG_SHARE_ADDR_TYPE, &isNetBIOS);
        diaGetInt(diaNetConfig, CFG_NETPROTOCOL, &netProto);
        isSMB = netProto == 0;
        diaSetVisible(diaNetConfig, NETCFG_SHARE_NB_ADDR, isNetBIOS);

        // SMB server fields belong to OPL's SMB consumer. UDPFS/UDPBD are the Neutrino-facing
        // transports, so do not present SMB-only options as if they applied to those protocols.
        diaSetVisible(diaNetConfig, NETCFG_LBL_SMB_SERVER, isSMB);
        diaSetVisible(diaNetConfig, NETCFG_LBL_SHARE_ADDR_TYPE, isSMB);
        diaSetVisible(diaNetConfig, NETCFG_LBL_SHARE_ADDRESS, isSMB);
        diaSetVisible(diaNetConfig, NETCFG_LBL_SHARE_PORT, isSMB);
        diaSetVisible(diaNetConfig, NETCFG_LBL_SHARE_NAME, isSMB);
        diaSetVisible(diaNetConfig, NETCFG_LBL_SHARE_USER, isSMB);
        diaSetVisible(diaNetConfig, NETCFG_LBL_SHARE_PASSWORD, isSMB);
        diaSetVisible(diaNetConfig, NETCFG_LBL_SMBDIALECT, isSMB);
        diaSetVisible(diaNetConfig, NETCFG_SHARE_ADDR_TYPE, isSMB);
        diaSetVisible(diaNetConfig, NETCFG_SHARE_NB_ADDR, isSMB && isNetBIOS);

        for (i = 0; i < 4; i++) {
            diaSetVisible(diaNetConfig, NETCFG_SHARE_IP_ADDR_0 + i, isSMB && !isNetBIOS);

            diaSetEnabled(diaNetConfig, NETCFG_PS2_IP_ADDR_0 + i, !isDHCPEnabled);
            diaSetEnabled(diaNetConfig, NETCFG_PS2_NETMASK_0 + i, !isDHCPEnabled);
            diaSetEnabled(diaNetConfig, NETCFG_PS2_GATEWAY_0 + i, !isDHCPEnabled);
            diaSetEnabled(diaNetConfig, NETCFG_PS2_DNS_0 + i, !isDHCPEnabled);
        }

        for (i = 0; i < 3; i++)
            diaSetVisible(diaNetConfig, NETCFG_SHARE_IP_ADDR_DOT_0 + i, isSMB && !isNetBIOS);

        diaSetEnabled(diaNetConfig, NETCFG_SHARE_PORT, isSMB && showAdvancedOptions);
        diaSetEnabled(diaNetConfig, NETCFG_ETHOPMODE, showAdvancedOptions);
        diaSetVisible(diaNetConfig, NETCFG_SHARE_PORT, isSMB);
        diaSetVisible(diaNetConfig, NETCFG_SHARE_NAME, isSMB);
        diaSetVisible(diaNetConfig, NETCFG_SHARE_USERNAME, isSMB);
        diaSetVisible(diaNetConfig, NETCFG_SHARE_PASSWORD, isSMB);

        // Protocol: lock Access to Files for SMB and to IMG for UDPBD (only UDPFS offers the free
        // toggle) -- snap the value so a stale IMG left over from UDPFS can never mis-derive to
        // UDPFSBD under SMB, AND grey the control so the lock is visible.
        // NOTE(rebuild): the SMB Version row stays greyed at SMBv1 until item 4 lands (the fork
        // enables it while SMB is the selected protocol).
        diaSetEnabled(diaNetConfig, CFG_SMBDIALECT, 0);
        diaSetVisible(diaNetConfig, CFG_SMBDIALECT, isSMB);
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

int guiShowNetConfig(void)
{
    size_t i;
    const char *ethOpModes[] = {_l(_STR_AUTO), _l(_STR_ETH_100MFDX), _l(_STR_ETH_100MHDX), _l(_STR_ETH_10MFDX), _l(_STR_ETH_10MHDX), NULL};
    const char *addrConfModes[] = {_l(_STR_ADDR_TYPE_IP), _l(_STR_ADDR_TYPE_NETBIOS), NULL};
    const char *ipAddrConfModes[] = {_l(_STR_IP_ADDRESS_TYPE_STATIC), _l(_STR_IP_ADDRESS_TYPE_DHCP), NULL};
    const char *netProtocols[] = {"SMB", "UDPFS", "UDPBD", NULL}; // UDPBD = SUDPBDv2 server -- protocol names, not translated
    const char *udpfsModes[] = {"Files", "IMG", NULL};            // Access: Files=udpfs_ioman filesystem, IMG=udpfs_bd block
    // NOTE(rebuild): SMBv1 only until item 4 re-adds the SMB2 dialect; the row shows the active
    // dialect and stays greyed (netConfigUpdater keeps it disabled).
    const char *smbDialects[] = {"SMBv1", NULL};
    diaSetEnum(diaNetConfig, NETCFG_PS2_IP_ADDR_TYPE, ipAddrConfModes);
    diaSetEnum(diaNetConfig, NETCFG_SHARE_ADDR_TYPE, addrConfModes);
    diaSetEnum(diaNetConfig, NETCFG_ETHOPMODE, ethOpModes);
    diaSetEnum(diaNetConfig, CFG_NETPROTOCOL, netProtocols);
    diaSetEnum(diaNetConfig, CFG_UDPFSMODE, udpfsModes);
    diaSetEnum(diaNetConfig, CFG_SMBDIALECT, smbDialects);

    // upload current values
    // Open the Network Config with advanced options ON so SMB Port + ETH op-mode are immediately
    // editable. Forcing 0 here contradicted the row's own def=1 in dialogs.c AND overwrote it:
    // diaSetInt writes both `def` and `current`, so even diaResetValue restored the 0. It matters
    // because this fork defaults the SMB port to 1111 -- anyone pointing at a normal 445 server hit
    // a greyed field first.
    diaSetInt(diaNetConfig, NETCFG_SHOW_ADVANCED_OPTS, 1);
    diaSetEnabled(diaNetConfig, NETCFG_ETHOPMODE, 1);
    diaSetEnabled(diaNetConfig, NETCFG_SHARE_PORT, 1);

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

    // Protocol rows, seeded from the authoritative gNetworkProtocol.
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
    diaSetInt(diaNetConfig, CFG_SMBDIALECT, 0); // SMBv1 -- the only dialect until item 4
    // Seed the initial grey/lock: diaExecuteDialog renders the FIRST frame before it calls
    // netConfigUpdater, so without this the first frame flashes every row enabled.
    diaSetEnabled(diaNetConfig, CFG_UDPFSMODE, netProtoVal == 1);
    diaSetEnabled(diaNetConfig, CFG_SMBDIALECT, 0); // NOTE(rebuild): greyed until item 4
    netConfigUpdater(1);

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

    guiSettingsBeginDialog(diaNetConfig);
    int result;
reshow_network:
    result = diaExecuteDialog(diaNetConfig, -1, 1, &netConfigUpdater);
    if (result == NETCFG_POPSTARTER_BUTTON) {
        // This is the shared POPSTARTER network editor. It owns IPCONFIG.DAT / SMBCONFIG.DAT;
        // the Network page only provides a second entry point and never mirrors that state.
        guiShowPopsNetConfig();
        goto reshow_network;
    }
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
        // NOTE(rebuild): the fork also reads the SMB dialect row back here (item 4).
        int netProtocolWas = gNetworkProtocol;
        int netProtoVal2, netAccessVal2;
        diaGetInt(diaNetConfig, CFG_NETPROTOCOL, &netProtoVal2);
        diaGetInt(diaNetConfig, CFG_UDPFSMODE, &netAccessVal2);
        if (gNetStartMode == START_MODE_DISABLED)
            gNetworkProtocol = NET_PROTO_OFF;
        else
            gNetworkProtocol = (netProtoVal2 == 0)  ? NET_PROTO_SMB :
                               (netProtoVal2 == 2)  ? NET_PROTO_UDPBD :
                               (netAccessVal2 == 1) ? NET_PROTO_UDPFSBD :
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

        // "Nothing happens" guard: enabling a network protocol gives NO feedback -- the UDPFS tab
        // joins the ring silently and then waits for a Confirm-press inside it (Manual start), and
        // the block transports show a tab only once the PC server answers. Tell the user what to
        // expect + which PC server to run, right at the moment they turn it on. Shown BEFORE the
        // restart notice below: when a restart is pending, the restart message must be the LAST
        // word so the guidance reads as "after that".
        if (gNetworkProtocol != netProtocolWas) {
            if (gNetworkProtocol == NET_PROTO_UDPFS)
                guiMsgBox(_l(_STR_NET_UDPFS_TAB_HINT), 0, NULL);
            else if (gNetworkProtocol == NET_PROTO_UDPFSBD || gNetworkProtocol == NET_PROTO_UDPBD)
                guiMsgBox(_l(_STR_NET_UDPBD_TAB_HINT), 0, NULL);
        }

        // Each network transport loads its IOP module chain once per boot (the load latch is not cleared
        // live). If a stack is already up and the user picked a protocol other than the one actually
        // running, the switch takes effect only after a restart -- say so instead of silently doing
        // nothing. The OFFER to restart lives on the Save Settings path (see guiNetProtocolNeedsRestart),
        // since this dialog only touches RAM.
        if (gNetworkProtocol != netProtocolWas && guiNetProtocolNeedsRestart())
            guiMsgBox(_l(_STR_NETBOOT_RESTART), 0, NULL);

        if (result == NETCFG_RECONNECT && gNetworkStartup < ERROR_ETH_SMB_CONN)
            gNetworkStartup = ERROR_ETH_SMB_LOGON;

        applyConfig(-1, -1, 0);
    }

    guiSettingsEndDialog();
    return guiSettingsPageResult(result);
}

// POPStarter page live-updater: reveal the free-text POPSTARTER.ELF Path field only when the
// device picker is "Custom".
static int guiVcdUpdater(int modified)
{
    struct UIItem *ui = guiSettingsActiveDialog != NULL ? guiSettingsActiveDialog : diaVcdConfig;
    int popsDev;

    if (modified) {
        diaGetInt(ui, CFG_POPSTARTER_DEVICE, &popsDev);
        diaSetVisible(ui, CFG_LBL_POPSTARTER_PATH, popsDev == POPS_DEV_CUSTOM);
        diaSetVisible(ui, CFG_POPSTARTER_PATH, popsDev == POPS_DEV_CUSTOM);
    }
    return 0;
}

// BDMA Settings live-updater: hide the manual BDMA Source/Mode pickers while "VCD BDMA Apply on
// Launch" is ON (it auto-equips); re-reveal them live when toggled off.
static int guiBdmaUpdater(int modified)
{
    struct UIItem *ui = guiSettingsActiveDialog != NULL ? guiSettingsActiveDialog : diaBdmaConfig;
    int bdmaApply;

    if (modified) {
        diaGetInt(ui, CFG_BDMA_APPLY, &bdmaApply);
        diaSetVisible(ui, CFG_LBL_BDMASOURCE, !bdmaApply);
        diaSetVisible(ui, CFG_BDMASOURCE, !bdmaApply);
        diaSetVisible(ui, CFG_LBL_BDMAMODE, !bdmaApply);
        diaSetVisible(ui, CFG_BDMAMODE, !bdmaApply);
    }
    return 0;
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
// SUPERSEDED AND UNREFERENCED. The live PS Emulation Settings page is guiSettingsShowPopstarter(),
// which COMPOSES a copy of diaVcdConfig + diaBdmaConfig and drives that copy. Nothing calls this
// function any more; it is kept only because its declaration is still published in gui.h.
//
// Wiring a new row here does NOTHING. That mistake was made once already, with CFG_EMBER_DISPLAY:
// the row rendered on the composed page with no enum list and never saved, because its diaSetEnum
// ran against the template in here instead of against the composed `ui`. Add new rows to
// guiSettingsShowPopstarter().
void guiShowVcdConfig(void)
{
    // POPSTARTER.ELF device TYPE (POPS_DEV_*). MUST stay in sync with vcdResolvePopstarter() (vcdsupport.c).
    const char *popsDevStrs[] = {_l(_STR_DEFAULT), "Memory Card", "USB", "MX4SIO", "MMCE", "HDD (exFAT)", "HDD (APA)", "Custom", _l(_STR_GAMES_DEVICE), NULL}; // "Game's Device" (POPS_DEV_GAME) appended last to match the enum tail
    // diaSetEnum stores the ARRAY POINTER rather than copying, so this must outlive every render of
    // the dialog -- static, like the BDMA option arrays on the peer page.
    static const char *emberDisplayStrs[] = {NULL, "240p", "480p", NULL};
    emberDisplayStrs[0] = _l(_STR_DEFAULT);
    diaSetEnum(diaVcdConfig, CFG_EMBER_DISPLAY, emberDisplayStrs);
    diaSetInt(diaVcdConfig, CFG_EMBER_DISPLAY, gEmberDisplay);

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
        diaGetInt(diaVcdConfig, CFG_EMBER_DISPLAY, &gEmberDisplay);

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

static void guiSetBdmaSettings(struct UIItem *ui)
{
    static const char *bdmaSourceStrs[] = {NULL, NULL, NULL, NULL, NULL};
    static const char *bdmaModeStrs[] = {NULL, NULL, NULL, NULL, NULL, NULL};
    // Order matches enum VCD_USB_BDMA_MODE: Ask / exFAT / fat32. The two driver labels are the ones the
    // per-launch prompt already uses, so the row and the dialog it replaces read identically.
    static const char *vcdUsbBdmaStrs[] = {NULL, NULL, NULL, NULL};

    // diaSetEnum stores the array pointer rather than copying it. These arrays must therefore outlive
    // this initializer, especially when the BDMA rows are composed into the POPSTARTER peer page.
    bdmaSourceStrs[0] = _l(_STR_BDMA_SRC_USB);
    bdmaSourceStrs[1] = _l(_STR_BDMA_SRC_MX4SIO);
    bdmaSourceStrs[2] = _l(_STR_BDMA_SRC_MMCE);
    bdmaSourceStrs[3] = _l(_STR_BDMA_SRC_HDD);
    bdmaModeStrs[0] = _l(_STR_BDMA_MODE_FAT32);
    bdmaModeStrs[1] = _l(_STR_BDMA_MODE_USBEXFAT);
    bdmaModeStrs[2] = _l(_STR_BDMA_MODE_MX4SIO);
    bdmaModeStrs[3] = _l(_STR_BDMA_MODE_MMCE);
    bdmaModeStrs[4] = _l(_STR_BDMA_MODE_ATA);
    vcdUsbBdmaStrs[0] = _l(_STR_VCD_USB_BDMA_ASK);
    vcdUsbBdmaStrs[1] = _l(_STR_VCD_USB_MODE_EXFAT);
    vcdUsbBdmaStrs[2] = _l(_STR_VCD_USB_MODE_FAT32);

    gBdmaMode = vcdReadBdmaMode();
    diaSetEnum(ui, CFG_BDMASOURCE, bdmaSourceStrs);
    diaSetEnum(ui, CFG_BDMAMODE, bdmaModeStrs);
    diaSetEnum(ui, CFG_VCD_USB_BDMA, vcdUsbBdmaStrs);
    diaSetInt(ui, CFG_BDMASOURCE, gBdmaSource);
    diaSetInt(ui, CFG_BDMAMODE, gBdmaMode);
    diaSetInt(ui, CFG_VCD_USB_BDMA, gVcdUsbBdmaMode);
    diaSetInt(ui, CFG_BDMA_APPLY, gBdmaApplyOnLaunch);
    // "VCD BDMA Apply on Launch" ON auto-equips, so hide the manual SOURCE/MODE pickers
    // (guiBdmaUpdater re-reveals them live when toggled off).
    diaSetVisible(ui, CFG_LBL_BDMASOURCE, !gBdmaApplyOnLaunch);
    diaSetVisible(ui, CFG_BDMASOURCE, !gBdmaApplyOnLaunch);
    diaSetVisible(ui, CFG_LBL_BDMAMODE, !gBdmaApplyOnLaunch);
    diaSetVisible(ui, CFG_BDMAMODE, !gBdmaApplyOnLaunch);
}

// Save the BDMA preference and equip changed modules. The helper deliberately does not call
// applyConfig so a composed Settings page can commit all of its fields together.
static void guiSaveBdmaSettings(struct UIItem *ui)
{
    diaGetInt(ui, CFG_BDMA_APPLY, &gBdmaApplyOnLaunch);
    // Read BEFORE the equip block below: that block can re-enter vcdEquipBdma and toast, and this
    // row must be taken from the dialog regardless of how the equip turns out (it governs the
    // per-launch USB prompt, not the card's equipped state).
    diaGetInt(ui, CFG_VCD_USB_BDMA, &gVcdUsbBdmaMode);
    {
        // Equip BDMA modules only when SOURCE or MODE actually changed (the equip copies files to
        // the memory card). vcdEquipBdma is free-space-gated + truncation-safe, so a failure never
        // corrupts the card; report it and resync MODE to what's really equipped.
        int oldSrc = gBdmaSource, oldMode = gBdmaMode; // baselines (MODE = card's actual state)
        int newSrc = oldSrc, newMode = oldMode;
        diaGetInt(ui, CFG_BDMASOURCE, &newSrc);
        diaGetInt(ui, CFG_BDMAMODE, &newMode);
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
}

// POPStarter -> BDMA Settings (BDMAssault exFAT-driver equip). MODE reflects what's ACTUALLY on the
// card (marker read), so the page is honest even if POPSLoader or a prior session set it.
void guiShowBdmaConfig(void)
{
    guiSetBdmaSettings(diaBdmaConfig);

    int ret = diaExecuteDialog(diaBdmaConfig, -1, 1, &guiBdmaUpdater);
    if (ret) {
        guiSaveBdmaSettings(diaBdmaConfig);
        applyConfig(-1, -1, 0);
    }
}

// POPStarter -> Game List Settings (VCD list display options).
void guiShowVcdListConfig(void)
{
    diaSetInt(diaVcdListConfig, CFG_VCD_HIDE_GAMEID, gVcdHideGameId);
    diaSetInt(diaVcdListConfig, CFG_VCD_FIRST_DISC_ONLY, gVcdFirstDiscOnly);
    diaSetInt(diaVcdListConfig, CFG_VCD_SHOW_PP_POPS, gVcdShowPpPops);

    int rebuildVcdLists = 0;
    int ret = diaExecuteDialog(diaVcdListConfig, -1, 1, NULL);
    if (ret) {
        // This editor is reachable from both Settings parents. Its values still belong to the
        // existing globals/config set, but an OK inside the shell must participate in the shell's
        // shared Save Changes prompt regardless of which parent opened it.
        if (guiSettingsShellActive)
            guiSettingsSavePending = 1;
        {
            // #195: hide-gameid is NO LONGER purely cosmetic -- it is now a SORT KEY. The menu sort
            // (submenuSort) orders by the DISPLAYED title, i.e. past the hidden prefix, so a change must
            // re-sort every VCD-capable page. libViewMarkAllDirty() + rebuildVcdLists forces that menu rebuild
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
                libViewMarkAllDirty();
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
                libViewMarkAllDirty();
                hddVcdInvalidateCache(); // scan-time filter changed -> the cached HDD VCD list is stale
                rebuildVcdLists = 1;
            }
        }
        {
            // PP-POPS display changes the HDD VCD list CONTENTS (one-game partitions included or not).
            // Same treatment as first-disc-only: it is a scan-time filter, so the cached HDD VCD list
            // is stale the moment it flips. Enumeration-only -- launch semantics never change.
            int previousShowPpPops = gVcdShowPpPops;
            diaGetInt(diaVcdListConfig, CFG_VCD_SHOW_PP_POPS, &gVcdShowPpPops);
            if (gVcdShowPpPops != previousShowPpPops) {
                libViewMarkAllDirty();
                hddVcdInvalidateCache();
                rebuildVcdLists = 1;
            }
        }
        applyConfig(-1, -1, 0);
        menuReinitMainMenu();
        // Queue after applyConfig/menu reinit so the IO worker cannot rebuild a submenu concurrently
        // with their support/menu bookkeeping on the GUI thread.
        if (rebuildVcdLists)
            oplQueueLibraryDeviceUpdates();
    }
}

// POPSTARTER's local memory-card resolver evaluates both mc0 and mc1, including directory/file
// RPCs. On a slow or absent card that work can take seconds. Keep it on the I/O worker and pump a
// real runtime frame here; guiHandleDeferedIO is intentionally not used because its text screen
// does not poll input or animate the Settings plasma.
#define POPSNET_READ_TIMEOUT_MS  15000
#define GUI_POPSNET_READ_ABORTED (-100)
enum gui_popsnet_read_source {
    GUI_POPSNET_READ_LOCAL = 0,
    GUI_POPSNET_READ_SMB,
};
static int guiPopsNetReadPending;
static int guiPopsNetReadInFlight;
static int guiPopsNetReadAbandoned;
static int guiPopsNetReadSource;
static int guiPopsNetReadResult;
static vcd_popsnet_t guiPopsNetReadData;

static void guiPopsNetReadWorker(void)
{
    vcd_popsnet_t data;
    int result;

    if (guiPopsNetReadSource == GUI_POPSNET_READ_LOCAL)
        result = vcdReadPopstarterNet(&data);
    else
        result = vcdReadPopstarterNetFromSmb(&data);

    if (!guiPopsNetReadAbandoned) {
        guiPopsNetReadData = data;
        guiPopsNetReadResult = result;
    }
    guiPopsNetReadInFlight = 0;
    guiPopsNetReadPending = 0;
}

static int guiReadPopsNet(int source, vcd_popsnet_t *out)
{
    clock_t startTick;
    clock_t timeoutTicks = (clock_t)POPSNET_READ_TIMEOUT_MS * (CLOCKS_PER_SEC / 1000);

    if (guiPopsNetReadInFlight) {
        // A cancelled request still owns the static payload until its I/O RPC returns. Reopening the
        // same editor may resume its wait, but a second source may not overwrite that payload.
        if (guiPopsNetReadSource != source)
            return GUI_POPSNET_READ_ABORTED;
        guiPopsNetReadAbandoned = 0;
    } else {
        memset(&guiPopsNetReadData, 0, sizeof(guiPopsNetReadData));
        guiPopsNetReadSource = source;
        guiPopsNetReadResult = GUI_POPSNET_READ_ABORTED;
        guiPopsNetReadAbandoned = 0;
        guiPopsNetReadPending = 1;
        guiPopsNetReadInFlight = 1;
        if (ioPutRequest(IO_CUSTOM_SIMPLEACTION, &guiPopsNetReadWorker) != IO_OK) {
            guiPopsNetReadInFlight = 0;
            guiPopsNetReadPending = 0;
            return GUI_POPSNET_READ_ABORTED;
        }
    }

    startTick = clock();
    while (guiPopsNetReadInFlight) {
        guiStartFrame();
        if (guiDrawBGSettings() == 0)
            guiDrawBGPlasma();
        rmDrawRect(0, 0, screenWidth, screenHeight, gColDarker);
        fntRenderString(gTheme->fonts[0], screenWidth >> 1, gTheme->usedHeight >> 1,
                        ALIGN_CENTER, 0, 0, _l(_STR_POPS_LOADING_SETTINGS), gTheme->textColor);
        guiDrawOverlays();
        guiEndFrame();

        readPads();
        if (getKeyOn(KEY_CIRCLE)) {
            guiPopsNetReadAbandoned = 1;
            return GUI_POPSNET_READ_ABORTED;
        }
        if ((clock() - startTick) >= timeoutTicks) {
            guiPopsNetReadAbandoned = 1;
            return GUI_POPSNET_READ_ABORTED;
        }
    }

    if (out != NULL)
        *out = guiPopsNetReadData;
    return guiPopsNetReadResult;
}

static void guiSetPopsNetDialogFields(const vcd_popsnet_t *cfg)
{
    size_t i;

    diaSetInt(diaPopsNetConfig, NETCFG_POPS_IPTYPE, cfg->ipDhcp);
    for (i = 0; i < 4; ++i) {
        diaSetInt(diaPopsNetConfig, NETCFG_POPS_IP_0 + i, cfg->ps2Ip[i]);
        diaSetInt(diaPopsNetConfig, NETCFG_POPS_MASK_0 + i, cfg->ps2Mask[i]);
        diaSetInt(diaPopsNetConfig, NETCFG_POPS_GW_0 + i, cfg->ps2Gw[i]);
        diaSetInt(diaPopsNetConfig, NETCFG_POPS_SMB_IP_0 + i, cfg->smbIp[i]);
        diaSetEnabled(diaPopsNetConfig, NETCFG_POPS_IP_0 + i, !cfg->ipDhcp);
        diaSetEnabled(diaPopsNetConfig, NETCFG_POPS_MASK_0 + i, !cfg->ipDhcp);
        diaSetEnabled(diaPopsNetConfig, NETCFG_POPS_GW_0 + i, !cfg->ipDhcp);
    }
    diaSetInt(diaPopsNetConfig, NETCFG_POPS_SMB_PORT, cfg->smbPort);
    diaSetString(diaPopsNetConfig, NETCFG_POPS_SMB_SHARE, cfg->smbShare);
    diaSetString(diaPopsNetConfig, NETCFG_POPS_SMB_USER, cfg->smbUser);
    diaSetString(diaPopsNetConfig, NETCFG_POPS_SMB_PASS, cfg->smbPass);
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

// POPStarter -> Network Settings (VCD over SMB). The local memory-card snapshot remains the sole
// write owner. The explicit import path only copies valid POPS/*.DAT values into the editor, so the
// normal change-detection/Replace flow still decides whether anything is committed to an MC.
void guiShowPopsNetConfig(void)
{
    size_t i;
    static vcd_popsnet_t popsOriginal;
    static char popsNotice[128];
    static const char *ipAddrConfModes[3];

    ipAddrConfModes[0] = _l(_STR_IP_ADDRESS_TYPE_STATIC);
    ipAddrConfModes[1] = _l(_STR_IP_ADDRESS_TYPE_DHCP);
    ipAddrConfModes[2] = NULL;
    diaSetEnum(diaPopsNetConfig, NETCFG_POPS_IPTYPE, ipAddrConfModes);

    if (guiReadPopsNet(GUI_POPSNET_READ_LOCAL, &popsOriginal) == GUI_POPSNET_READ_ABORTED)
        return;
    guiSetPopsNetDialogFields(&popsOriginal);

    if (popsOriginal.smbInvalid || popsOriginal.ipInvalid) {
        diaSetLabel(diaPopsNetConfig, NETCFG_POPS_NOTICE, _l(_STR_POPSTARTER_NET_INVALID));
    } else if (popsOriginal.smbExists || popsOriginal.ipExists) {
        snprintf(popsNotice, sizeof(popsNotice), _l(_STR_POPS_LOADED_FROM), popsOriginal.home);
        diaSetLabel(diaPopsNetConfig, NETCFG_POPS_NOTICE, popsNotice);
    } else {
        diaSetLabel(diaPopsNetConfig, NETCFG_POPS_NOTICE, _l(_STR_POPS_NONE_DETECTED));
    }

    for (;;) {
        int result = diaExecuteDialog(diaPopsNetConfig, -1, 1, &guiPopsNetUpdater);
        if (result == NETCFG_POPS_IMPORT) {
            vcd_popsnet_t imported;
            int importResult;

            // Do not start or reconnect SMB merely to import. Its mounted state, not the saved
            // protocol picker, is the authority for whether these files are readable.
            if (!ethIsSMBShareConnected()) {
                guiMsgBox(_l(_STR_POPS_SMB_NOT_CONNECTED), 0, NULL);
                continue;
            }
            importResult = guiReadPopsNet(GUI_POPSNET_READ_SMB, &imported);
            if (importResult == GUI_POPSNET_READ_ABORTED)
                return;
            if (importResult == VCD_POPSNET_SMB_IMPORT_NOT_CONNECTED) {
                guiMsgBox(_l(_STR_POPS_SMB_NOT_CONNECTED), 0, NULL);
                continue;
            }
            if (importResult != VCD_POPSNET_SMB_IMPORT_OK) {
                guiMsgBox(_l(_STR_POPS_SMB_SETTINGS_NOT_FOUND), 0, NULL);
                continue;
            }
            guiSetPopsNetDialogFields(&imported);
            continue;
        }

        if (result != UIID_BTN_OK)
            return;

        // POPStarter save matrix (POPSLoader parity): compare the dialog's POPStarter fields against
        // the read-time snapshot and write ONLY actual changes. The resolved home from the snapshot
        // remains attached to popsCur, so a valid MC1 setup never causes a new MC0 shadow file.
        vcd_popsnet_t popsCur = popsOriginal;
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
        int popsSmbComplete = vcdPopsNetValuesValid(&popsCur, 1, 0);
        int popsIpComplete = !popsCur.ipDhcp && vcdPopsNetValuesValid(&popsCur, 0, 1);
        int writeSmb = (popsMask & 1) && (popsOriginal.smbExists || popsSmbComplete);
        int writeIp = ((popsMask & 2) || popsOriginal.ipInvalid) &&
                      (popsOriginal.ipExists || popsCur.ipDhcp || popsIpComplete);

        if (writeSmb && !popsOriginal.smbExists && !popsOriginal.ipExists) {
            writeIp = 1;
            if (!popsCur.ipDhcp && !popsIpComplete)
                popsCur.ipDhcp = 1;
        }

        if (!writeSmb && !writeIp)
            return;
        if (!vcdPopsNetValuesValid(&popsCur, writeSmb, writeIp)) {
            guiMsgBox(_l(_STR_POPSTARTER_NET_INVALID), 0, NULL);
            continue;
        }

        if ((writeSmb && popsOriginal.smbExists) || (writeIp && popsOriginal.ipExists)) {
            int ov = diaExecuteDialog(diaPopsOverwrite, -1, 1, NULL);
            if (ov != POPS_OVERWRITE_REPLACE)
                continue;
        }

        if (vcdWritePopstarterNetFiles(&popsCur, writeSmb, writeIp) != 0) {
            guiMsgBox(_l(_STR_POPSTARTER_NET_ERR), 0, NULL);
            continue;
        }
        return;
    }
}

static void guiSetParentalLockValue(struct UIItem *ui)
{
    char password[CONFIG_KEY_VALUE_LEN];
    config_set_t *configOPL = configGetByType(CONFIG_OPL);

    configGetStrCopy(configOPL, CONFIG_OPL_PARENTAL_LOCK_PWD, password, sizeof(password));
    diaSetString(ui, CFG_PARENLOCK_PASSWORD, password);
}

static void guiSaveParentalLockValue(struct UIItem *ui)
{
    char oldPassword[CONFIG_KEY_VALUE_LEN];
    char password[CONFIG_KEY_VALUE_LEN];
    config_set_t *configOPL = configGetByType(CONFIG_OPL);

    configGetStrCopy(configOPL, CONFIG_OPL_PARENTAL_LOCK_PWD, oldPassword, sizeof(oldPassword));
    diaGetString(ui, CFG_PARENLOCK_PASSWORD, password, sizeof(password));
    if (strcmp(oldPassword, password) == 0)
        return;

    if (strlen(password) > 0) {
        if (strncmp(OPL_PARENTAL_LOCK_MASTER_PASS, password, sizeof(password)) != 0)
            configSetStr(configOPL, CONFIG_OPL_PARENTAL_LOCK_PWD, password);
        else {
            diaSetString(ui, CFG_PARENLOCK_PASSWORD, oldPassword);
            guiMsgBox(_l(_STR_PARENLOCK_INVALID_PASSWORD), 0, NULL);
            return;
        }
    } else {
        configRemoveKey(configOPL, CONFIG_OPL_PARENTAL_LOCK_PWD);
        guiMsgBox(_l(_STR_PARENLOCK_DISABLE_WARNING), 0, ui);
    }

    menuSetParentalLockCheckState(1);
}

static void guiSetAdvancedSettings(struct UIItem *ui)
{
    diaSetString(ui, CFG_BDMPREFIX, gBDMPrefix);
    diaSetString(ui, CFG_ETHPREFIX, gETHPrefix);
    diaSetInt(ui, CFG_HDDSPINDOWN, gHDDSpindown);
    diaSetInt(ui, CFG_HDDGAMELISTCACHE, gHDDGameListCache);
    diaSetInt(ui, CFG_BDMCACHE, bdmCacheSize);
    diaSetInt(ui, CFG_HDDCACHE, hddCacheSize);
    diaSetInt(ui, CFG_SMBCACHE, smbCacheSize);
}

static void guiSaveAdvancedSettings(struct UIItem *ui)
{
    diaGetString(ui, CFG_BDMPREFIX, gBDMPrefix, sizeof(gBDMPrefix));
    diaGetString(ui, CFG_ETHPREFIX, gETHPrefix, sizeof(gETHPrefix));
    diaGetInt(ui, CFG_HDDSPINDOWN, &gHDDSpindown);
    diaGetInt(ui, CFG_HDDGAMELISTCACHE, &gHDDGameListCache);
    diaGetInt(ui, CFG_BDMCACHE, &bdmCacheSize);
    diaGetInt(ui, CFG_HDDCACHE, &hddCacheSize);
    diaGetInt(ui, CFG_SMBCACHE, &smbCacheSize);
}

void guiShowParentalLockConfig(void)
{
    guiSetParentalLockValue(diaParentalLockConfig);

    int result = diaExecuteDialog(diaParentalLockConfig, -1, 1, NULL);
    if (result) {
        guiSaveParentalLockValue(diaParentalLockConfig);
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
        diaGetInt(diaLaunchConfig, CFG_DEFAULT_CORE, &gDefaultCoreLoader);
        diaGetInt(diaLaunchConfig, CFG_PS2LOGO, &gPS2Logo);

        applyConfig(-1, -1, 0);
        menuReinitMainMenu();
    }
}

void guiShowSecurityConfig(void)
{
    diaSetInt(diaSecurityConfig, CFG_ENWRITEOP, gEnableWrite);
    guiSetParentalLockValue(diaSecurityConfig);

    int ret = diaExecuteDialog(diaSecurityConfig, -1, 1, NULL);
    if (ret) {
        diaGetInt(diaSecurityConfig, CFG_ENWRITEOP, &gEnableWrite);
        guiSaveParentalLockValue(diaSecurityConfig);

        applyConfig(-1, -1, 0);
        menuReinitMainMenu();
    }
}

void guiShowAdvancedConfig(void)
{
    diaSetInt(diaAdvancedConfig, CFG_DEBUG, gEnableDebug);
    guiSetAdvancedSettings(diaAdvancedConfig);

    int ret = diaExecuteDialog(diaAdvancedConfig, -1, 1, NULL);
    if (ret) {
        diaGetInt(diaAdvancedConfig, CFG_DEBUG, &gEnableDebug);
        guiSaveAdvancedSettings(diaAdvancedConfig);

        applyConfig(-1, -1, 0);
        menuReinitMainMenu();
    }
}

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
            // latch is write-once and process-wide, so a user who turns the loader on after boot would
            // otherwise keep getting nothing until a reboot -- which looks exactly like "the toggle
            // does nothing".
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
static int guiGameIdModeFromGlobals(void)
{
    if (gApplyGameID)
        return gPopstarterRetroGemGameID ? 0 : 2; // All / PS2 only
    return gPopstarterRetroGemGameID ? 1 : 3;     // POPSTARTER only / Off
}

static void guiApplyGameIdMode(int mode)
{
    gApplyGameID = mode == 0 || mode == 2;
    gPopstarterRetroGemGameID = mode == 0 || mode == 1;
}

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
    const char *gameIDModes[] = {_l(_STR_GAMEID_MODE_ALL), _l(_STR_GAMEID_MODE_POPSTARTER),
                                 _l(_STR_GAMEID_MODE_PS2), _l(_STR_GAMEID_MODE_OFF), NULL};
    int previousVMode;
    int ret;

reselect_video_mode:
    previousVMode = gVMode;
    diaSetEnum(diaDisplayConfig, UICFG_VMODE, vmodeNames);
    diaSetEnum(diaDisplayConfig, CFG_APPLYGAMEID, gameIDModes);
    diaSetInt(diaDisplayConfig, UICFG_VMODE, gVMode);
    diaSetInt(diaDisplayConfig, UICFG_WIDESCREEN, gWideScreen);
    diaSetInt(diaDisplayConfig, UICFG_XOFF, gXOff);
    diaSetInt(diaDisplayConfig, UICFG_YOFF, gYOff);
    diaSetInt(diaDisplayConfig, UICFG_OVERSCAN, gOverscan);
    diaSetInt(diaDisplayConfig, CFG_APPLYGAMEID, guiGameIdModeFromGlobals());

reshow_display:
    ret = diaExecuteDialog(diaDisplayConfig, -1, 1, guiDisplayUpdater);

    // The GSM defaults sub-page writes straight into the global config set, so re-enter WITHOUT
    // going back through the diaSetInt block above -- otherwise the rows the user already changed
    // on this page would be reset from the (not yet committed) globals.
    if (ret == DISPLAY_GSM_DEFAULTS_BUTTON) {
        guiGameShowGSConfig(1);
        goto reshow_display;
    }

    if (ret) {
        diaGetInt(diaDisplayConfig, UICFG_VMODE, &gVMode);
        diaGetInt(diaDisplayConfig, UICFG_WIDESCREEN, &gWideScreen);
        diaGetInt(diaDisplayConfig, UICFG_XOFF, &gXOff);
        diaGetInt(diaDisplayConfig, UICFG_YOFF, &gYOff);
        diaGetInt(diaDisplayConfig, UICFG_OVERSCAN, &gOverscan);
        int gameIDMode;
        diaGetInt(diaDisplayConfig, CFG_APPLYGAMEID, &gameIDMode);
        guiApplyGameIdMode(gameIDMode);

        // Same #172 contract as _guiShowUIConfig above: play out the confirm bump before the GS
        // teardown/rebuild below, on the GUI thread -- never inside applyConfig itself.
        padRumbleFlush();
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

int guiDrawBGSettings(void)
{
    GSTEXTURE *bg = thmGetTexture(SETTINGS_BG);
    if (bg) {
        rmDrawPixmap(bg, 0, 0, ALIGN_NONE, screenWidth, screenHeight, SCALING_NONE, gDefaultCol, 0);
        return 1;
    }
    return 0;
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
        guiSettingsSavePending = 1;
        guiSetAudioSettingsState();
    }

    return 0;
}

int guiShowAudioConfig(void)
{
    diaSetInt(diaAudioConfig, CFG_SFX, gEnableSFX);
    diaSetInt(diaAudioConfig, CFG_BOOT_SND, gEnableBootSND);
    diaSetInt(diaAudioConfig, CFG_BGM, gEnableBGM);
    diaSetInt(diaAudioConfig, CFG_SFX_VOLUME, gSFXVolume);
    diaSetInt(diaAudioConfig, CFG_BOOT_SND_VOLUME, gBootSndVolume);
    diaSetInt(diaAudioConfig, CFG_BGM_VOLUME, gBGMVolume);
    diaSetString(diaAudioConfig, CFG_DEFAULT_BGM_PATH, gDefaultBGMPath);
    diaSetShowDefaultWhenEmpty(diaAudioConfig, CFG_DEFAULT_BGM_PATH, 1); // blank -> the theme's own bgm

    guiSettingsBeginDialog(diaAudioConfig);
    int result = diaExecuteDialog(diaAudioConfig, -1, 1, guiAudioUpdater);
    guiSettingsEndDialog();
    return guiSettingsPageResult(result);
}

int guiShowControllerConfig(void)
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

    guiSettingsBeginDialog(diaControllerConfig);
    int result;
reshow_controller:
    result = diaExecuteDialog(diaControllerConfig, -1, 1, NULL);
    if (result) {
        diaGetInt(diaControllerConfig, UICFG_SCROLL, &gScrollSpeed);
        diaGetInt(diaControllerConfig, CFG_XSENSITIVITY, &gXSensitivity);
        diaGetInt(diaControllerConfig, CFG_YSENSITIVITY, &gYSensitivity);
        diaGetInt(diaControllerConfig, CFG_RUMBLE, &gEnableRumble);
        if (!gEnableRumble)
            padRumbleFlush(); // turned off mid-pulse -> do not leave a motor running

        if (diaGetInt(diaControllerConfig, CFG_SELECTBUTTON, &value))
            gSelectButton = value == 0 ? KEY_CIRCLE : KEY_CROSS;
        else
            gSelectButton = KEY_CIRCLE;
#ifdef PADEMU
        if (result == PADEMU_GLOBAL_BUTTON) {
            guiGameShowPadEmuConfig(1);
            goto reshow_controller;
        } else if (result == PADMACRO_GLOBAL_BUTTON) {
            guiGameShowPadMacroConfig(1);
            goto reshow_controller;
        }
#endif
        applyConfig(-1, -1, 1);
    }

    guiSettingsEndDialog();
    return guiSettingsPageResult(result);
}

static int guiSettingsSkipID(int id, const int *skipIDs, int skipCount)
{
    int i;

    for (i = 0; i < skipCount; i++) {
        if (id == skipIDs[i])
            return 1;
    }
    return 0;
}

static struct UIItem *guiSettingsCompose(const struct UIItem *const *parts, int partCount,
                                         const int *skipIDs, int skipCount, int skipPart,
                                         int suppressSecondaryHeaders)
{
    return guiSettingsComposeInto(guiSettingsDialog, parts, partCount, skipIDs, skipCount, skipPart,
                                  suppressSecondaryHeaders);
}

static struct UIItem *guiSettingsComposeInto(struct UIItem *dialog, const struct UIItem *const *parts,
                                             int partCount, const int *skipIDs, int skipCount, int skipPart,
                                             int suppressSecondaryHeaders)
{
    int part, i, skipTrailingBreak, skipSecondarySplitter;
    int count = 0;

    for (part = 0; part < partCount; part++) {
        skipTrailingBreak = 0;
        skipSecondarySplitter = 0;
        for (i = 0; parts[part][i].type != UI_TERMINATOR; i++) {
            const struct UIItem *item = &parts[part][i];

            // Each source definition owns its own OK row. A composite screen has one shared
            // commit point, while feature buttons and section headings remain intact.
            if (item->type == UI_OK) {
                // The source dialog also has a trailing UI_BREAK after its OK. That break is useful
                // when the source is shown alone, but becomes an empty row in a composite page.
                skipTrailingBreak = 1;
                continue;
            }
            if (skipTrailingBreak) {
                skipTrailingBreak = 0;
                if (item->type == UI_BREAK)
                    continue;
            }
            if ((skipPart < 0 || part == skipPart) && guiSettingsSkipID(item->id, skipIDs, skipCount)) {
                // Some legacy rows are represented as LABEL + SPACER + CONTROL. When the control
                // is omitted from a composed peer page, remove that now-orphaned label as well;
                // otherwise the page shows an empty heading before the replacement inline block.
                if (item->type != UI_LABEL && item->type != UI_SPACER && count >= 2 &&
                    dialog[count - 1].type == UI_SPACER &&
                    dialog[count - 2].type == UI_LABEL)
                    count -= 2;
                // A skipped control owns the following break in the source dialog. Once it is
                // omitted from a composite page, that break would become an empty
                // row with no visual or navigation purpose.
                if (item->type != UI_LABEL && item->type != UI_SPACER)
                    skipTrailingBreak = 1;
                continue;
            }
            if (suppressSecondaryHeaders && part > 0 && item->type == UI_HEADER) {
                // The peer page title already supplies the context. Keep the fields and actions,
                // but do not repeat each chained dialog's title as a second hierarchy level.
                skipSecondarySplitter = 1;
                continue;
            }
            if (skipSecondarySplitter && item->type == UI_SPLITTER) {
                skipSecondarySplitter = 0;
                if (count == 0 || dialog[count - 1].type == UI_BREAK)
                    continue;
                dialog[count++] = (struct UIItem) {UI_BREAK};
                continue;
            }
            // Secondary settings sections already have their own colored heading. Keep the heading
            // on its own line, but omit the source dialog's redundant separator line when it is
            // composed into the peer page.
            if (part > 0 && item->type == UI_SPLITTER && count > 0 && dialog[count - 1].type == UI_HEADER) {
                if (count >= SETTINGS_DIALOG_CAPACITY - 3) {
                    LOG("GUI Settings: composed page exceeds SETTINGS_DIALOG_CAPACITY (%d)\n",
                        SETTINGS_DIALOG_CAPACITY);
                    return NULL;
                }
                dialog[count++] = (struct UIItem) {UI_BREAK};
                continue;
            }
            if (count >= SETTINGS_DIALOG_CAPACITY - 3) {
                LOG("GUI Settings: composed page exceeds SETTINGS_DIALOG_CAPACITY (%d)\n",
                    SETTINGS_DIALOG_CAPACITY);
                return NULL;
            }
            dialog[count++] = *item;
        }
    }

    dialog[count++] = (struct UIItem) {UI_OK, 0, 1, 1, -1, 0, 0, {.label = {NULL, _STR_OK}}};
    dialog[count] = (struct UIItem) {UI_TERMINATOR};
    guiSettingsActiveDialog = dialog;
    return dialog;
}

static int guiSettingsIsPeerResult(int result)
{
    return result == DIA_RESULT_PREV || result == DIA_RESULT_NEXT;
}

static int guiSettingsIsShellResult(int result)
{
    return guiSettingsIsPeerResult(result) || result == DIA_RESULT_INDEX;
}

static int guiSettingsPageResult(int result)
{
    // L1/R1 peer paging and an explicit Index request pass straight back to the shell loop.
    if (guiSettingsIsShellResult(result)) {
        guiSettingsSavePending = 1;
        return result;
    }

    /* EVERY other way out of a page finishes at the Settings Index -- never at the main menu.
       Only guiSettingsShowIndex leaves Settings, and only after offering to save.

       The old test recognised the two generic button ids and nothing else, but pages carry their
       OWN button ids: the Network page's confirm button is NETCFG_OK, not UIID_BTN_OK. That fell
       through to 0, which guiShowSettings reads as "leave Settings entirely" -- so pressing OK on
       the Network page dumped the user at the main start menu, skipping the save prompt, with
       every edit still pending. Reconnect (NETCFG_RECONNECT) did the same.

       Fixing the one id would have left the trap armed for the next page that adds a button, so
       this fails safe instead: unless the page asked for a specific shell move, we stay inside.
       Cancel is the only result that does not arm the save prompt. */
    if (result != UIID_BTN_CANCEL)
        guiSettingsSavePending = 1;

    return DIA_RESULT_INDEX;
}

static void guiSettingsBeginDialog(struct UIItem *ui)
{
    if (!guiSettingsShellActive)
        return;

    snprintf(guiSettingsPageIndicator, sizeof(guiSettingsPageIndicator), "%d/8", guiSettingsCurrentPage + 1);
    diaSetSettingsShell(ui, guiSettingsPageIndicator);
    diaSetSettingsContext(1);
}

static void guiSettingsEndDialog(void)
{
    diaSetSettingsShell(NULL, NULL);
    diaSetSettingsContext(0);
}

static int guiSettingsGeneralUpdater(int modified)
{
    int showAutoStartLast;

    if (modified) {
        diaGetInt(guiSettingsActiveDialog, CFG_LASTPLAYED, &showAutoStartLast);
        diaSetVisible(guiSettingsActiveDialog, CFG_LBL_AUTOSTARTLAST, showAutoStartLast);
        diaSetVisible(guiSettingsActiveDialog, CFG_AUTOSTARTLAST, showAutoStartLast);
    }
    return 0;
}

static int guiSettingsShowGeneral(void)
{
    const struct UIItem *parts[] = {diaConfig, diaSecurityConfig, diaAdvancedConfig};
    struct UIItem *ui = guiSettingsCompose(parts, 3, NULL, 0, -1, 1);
    const char **langNamesSnap = NULL;
    int langID;
    int result;

    if (ui == NULL)
        return 0;

    // The General language row is intentionally the same enum/value as Interface, not a second
    // setting. Snapshot ownership matches Interface so deferred language discovery cannot free a
    // list while this dialog renders it.
    guiLock();
    langNamesSnap = guiCopyNameList((const char **)lngGetGuiList());
    guiUnlock();
    diaSetEnum(ui, UICFG_LANG, langNamesSnap != NULL ? langNamesSnap : (const char **)lngGetGuiList());
    diaSetInt(ui, UICFG_LANG, lngGetGuiValue());

    diaSetShowDefaultWhenEmpty(ui, CFG_EXITTO, 1);
    diaSetString(ui, CFG_EXITTO, gExitPath);
    diaSetShowDefaultWhenEmpty(ui, CFG_CUSTOMCFGPATH, 1);
    diaSetString(ui, CFG_CUSTOMCFGPATH, gCustomSettingsPath);
    diaSetInt(ui, CFG_LASTPLAYED, gRememberLastPlayed);
    diaSetInt(ui, CFG_FOLDERNAV, gEnableFolderNav);
    diaSetInt(ui, CFG_AUTOSTARTLAST, gAutoStartLastPlayed);
    diaSetVisible(ui, CFG_AUTOSTARTLAST, gRememberLastPlayed);
    diaSetVisible(ui, CFG_LBL_AUTOSTARTLAST, gRememberLastPlayed);
    diaSetInt(ui, CFG_ENWRITEOP, gEnableWrite);
    diaSetInt(ui, CFG_DEBUG, gEnableDebug);
    guiSetParentalLockValue(ui);
    guiSetAdvancedSettings(ui);
    guiSettingsBeginDialog(ui);

reshow_general:
    result = diaExecuteDialog(ui, -1, 1, &guiSettingsGeneralUpdater);
    if (result == GENERAL_GSM_DEFAULTS_BUTTON) {
        if (guiGameShowGSConfig(1))
            guiSettingsSavePending = 1;
        goto reshow_general;
    }

    if (result != UIID_BTN_CANCEL && result != -1) {
        diaGetInt(ui, UICFG_LANG, &langID);
        diaGetString(ui, CFG_EXITTO, gExitPath, sizeof(gExitPath));
        diaGetString(ui, CFG_CUSTOMCFGPATH, gCustomSettingsPath, sizeof(gCustomSettingsPath));
        diaGetInt(ui, CFG_LASTPLAYED, &gRememberLastPlayed);
        diaGetInt(ui, CFG_FOLDERNAV, &gEnableFolderNav);
        diaGetInt(ui, CFG_AUTOSTARTLAST, &gAutoStartLastPlayed);
        diaGetInt(ui, CFG_ENWRITEOP, &gEnableWrite);
        diaGetInt(ui, CFG_DEBUG, &gEnableDebug);
        guiSaveParentalLockValue(ui);
        guiSaveAdvancedSettings(ui);

        DisableCron = 1;
        applyConfig(-1, langID, 0);
        menuReinitMainMenu();
    }

    guiFreeNameList(langNamesSnap);
    guiSettingsEndDialog();
    guiSettingsActiveDialog = NULL;
    return guiSettingsPageResult(result);
}


static int guiSettingsSourcesUpdater(int modified)
{
    (void)modified;
    return 0;
}

static int guiSettingsShowSources(void)
{
    const struct UIItem *parts[] = {diaDeviceConfig};
    const char *deviceNames[] = {_l(_STR_BDM_GAMES), _l(_STR_NET_GAMES), _l(_STR_HDD_GAMES), _l(_STR_APPS), _l(_STR_MMCE), _l(_STR_FAV), NULL};
    const char *deviceModes[] = {_l(_STR_OFF), _l(_STR_MANUAL), _l(_STR_AUTO), NULL};
    static const char *hddOplHomes[] = {"__common/OPL/", "+OPL/", NULL};
    struct UIItem *ui = guiSettingsCompose(parts, 1, NULL, 0, -1, 0);
    int deviceModeIndex;
    int result;

    if (ui == NULL)
        return 0;

    diaSetEnum(ui, CFG_DEFDEVICE, deviceNames);
    diaSetEnum(ui, CFG_BDMMODE, deviceModes);
    diaSetEnum(ui, CFG_HDDMODE, deviceModes);
    diaSetEnum(ui, CFG_APPMODE, deviceModes);
    diaSetEnum(ui, CFG_FAVMODE, deviceModes);
    deviceModeIndex = guiIoModeToDeviceType(gDefaultDevice);
    diaSetInt(ui, CFG_DEFDEVICE, deviceModeIndex);
    diaSetInt(ui, CFG_BDMMODE, gBDMStartMode);
    diaSetInt(ui, CFG_HDDMODE, gHDDStartMode);
    diaSetInt(ui, CFG_APPMODE, gAPPStartMode);
    diaSetInt(ui, CFG_FAVMODE, gFAVStartMode);
    diaSetInt(ui, CFG_ENABLEUSB, gEnableUSB);
    diaSetInt(ui, CFG_ENABLEILK, gEnableILK);
    diaSetInt(ui, CFG_ENABLEMX4SIO, gEnableMX4SIO);
    diaSetInt(ui, CFG_ENABLEBDMHDD, gEnableBdmHDD);
    diaSetEnabled(ui, CFG_ENABLEBDMHDD, 1);
    diaSetEnabled(ui, CFG_HDDMODE, 1);
    diaSetEnum(ui, CFG_NETSTART, deviceModes);
    diaSetInt(ui, CFG_NETSTART, gNetStartMode);
    diaSetEnum(ui, CFG_MMCEMODE, deviceModes);
    diaSetInt(ui, CFG_MMCEMODE, gMMCEStartMode);
    diaSetEnabled(ui, CFG_MMCEMODE, 1);
    guiSettingsBeginDialog(ui);

reshow_sources:
    result = diaExecuteDialog(ui, -1, 1, &guiSettingsSourcesUpdater);
    if (result == MMCE_SETTINGS_BUTTON) {
        // MMCE uses a separate composition buffer. Confirming the child editor finishes the
        // Settings page and returns to the hub; Circle resumes Game Sources as the parent page.
        int mmceResult = guiShowMmceConfig();
        if (mmceResult != UIID_BTN_OK)
            goto reshow_sources;

        // Treat MMCE confirmation as confirmation of its parent page too. This preserves any
        // Game Sources edits made before opening MMCE, then the normal page-result mapping returns
        // to the Settings Index and marks the session as needing persistence.
        result = UIID_BTN_OK;
    }

    if (result != UIID_BTN_CANCEL && result != -1) {
        int netProtocolWas = gNetworkProtocol;

        diaGetInt(ui, CFG_DEFDEVICE, &deviceModeIndex);
        gDefaultDevice = guiDeviceTypeToIoMode(deviceModeIndex);
        diaGetInt(ui, CFG_BDMMODE, &gBDMStartMode);
        diaGetInt(ui, CFG_HDDMODE, &gHDDStartMode);
        diaGetInt(ui, CFG_APPMODE, &gAPPStartMode);
        diaGetInt(ui, CFG_MMCEMODE, &gMMCEStartMode);
        diaGetInt(ui, CFG_FAVMODE, &gFAVStartMode);
        diaGetInt(ui, CFG_ENABLEUSB, &gEnableUSB);
        diaGetInt(ui, CFG_ENABLEILK, &gEnableILK);
        diaGetInt(ui, CFG_ENABLEMX4SIO, &gEnableMX4SIO);
        diaGetInt(ui, CFG_ENABLEBDMHDD, &gEnableBdmHDD);
        diaGetInt(ui, CFG_NETSTART, &gNetStartMode);
        if (gNetStartMode == START_MODE_DISABLED)
            gNetworkProtocol = NET_PROTO_OFF;
        else if (gNetworkProtocol == NET_PROTO_OFF)
            gNetworkProtocol = NET_PROTO_SMB;
        gEnableUDPBD = (gNetworkProtocol == NET_PROTO_UDPBD || gNetworkProtocol == NET_PROTO_UDPFSBD);
        gNetBootProtocol = (gNetworkProtocol == NET_PROTO_UDPFSBD) ? NET_BOOT_UDPFS : NET_BOOT_UDPBD;
        gETHStartMode = (gNetworkProtocol == NET_PROTO_SMB) ? gNetStartMode : START_MODE_DISABLED;

        if (gNetworkProtocol != netProtocolWas) {
            if (gNetworkProtocol == NET_PROTO_UDPFS)
                guiMsgBox(_l(_STR_NET_UDPFS_TAB_HINT), 0, NULL);
            else if (gNetworkProtocol == NET_PROTO_UDPFSBD || gNetworkProtocol == NET_PROTO_UDPBD)
                guiMsgBox(_l(_STR_NET_UDPBD_TAB_HINT), 0, NULL);
        }
        if (gNetworkProtocol != netProtocolWas && guiNetProtocolNeedsRestart())
            guiMsgBox(_l(_STR_NETBOOT_RESTART), 0, NULL);

        bdmForceDeviceRefresh();
        applyConfig(-1, -1, 0);
        menuReinitMainMenu();
    }

    guiSettingsEndDialog();
    guiSettingsActiveDialog = NULL;
    return guiSettingsPageResult(result);
}

static int guiSettingsDisplayUpdater(int modified)
{
    int temp, x, y;

    if (!modified)
        return 0;

    guiSettingsSavePending = 1;

    diaGetInt(guiSettingsActiveDialog, UICFG_XOFF, &x);
    diaGetInt(guiSettingsActiveDialog, UICFG_YOFF, &y);
    if (x != gXOff || y != gYOff) {
        gXOff = x;
        gYOff = y;
        rmSetDisplayOffset(x, y);
    }
    diaGetInt(guiSettingsActiveDialog, UICFG_OVERSCAN, &temp);
    if (temp != gOverscan) {
        gOverscan = temp;
        rmSetOverscan(gOverscan);
        guiUpdateScreenScale();
    }
    diaGetInt(guiSettingsActiveDialog, UICFG_WIDESCREEN, &temp);
    if (temp != gWideScreen) {
        gWideScreen = temp;
        rmSetAspectRatio((gWideScreen == 0) ? RM_ARATIO_4_3 : RM_ARATIO_16_9);
        guiUpdateScreenScale();
    }
    return 0;
}

static int guiSettingsShowInterface(void)
{
    const struct UIItem *parts[] = {diaUIConfig, diaDisplayConfig};
    const int skipIDs[] = {UICFG_VMODE};
    const char *gameViewNames[] = {"Both", "PS2", "PS1", NULL};
    const char *vmodeNames[] = {_l(_STR_AUTO), "PAL 640x512i @50Hz 24bit", "NTSC 640x448i @60Hz 24bit",
                                "EDTV 640x448p @60Hz 24bit", "EDTV 640x512p @50Hz 24bit", "VGA 640x480p @60Hz 24bit",
                                "PAL 704x576i @50Hz 24bit (HIRES)", "NTSC 704x480i @60Hz 24bit (HIRES)",
                                "EDTV 704x480p @60Hz 24bit (HIRES)", "EDTV 704x576p @50Hz 24bit (HIRES)",
                                "HDTV 1280x720p @60Hz 16bit (HIRES)", "HDTV 1920x1080i @60Hz 16bit (HIRES)",
                                "PAL 640x256p @50Hz 24bit", "NTSC 640x224p @60Hz 24bit", NULL};
    // Video Mode stays in diaDisplayConfig for the legacy standalone Display editor, but the
    // composed Interface page supplies its one copy at the top of diaUIConfig.
    struct UIItem *ui = guiSettingsCompose(parts, 2, skipIDs, 1, 1, 1);
    const char **themeNamesSnap = NULL;
    const char **langNamesSnap = NULL;
    int themeID, langID, previousTheme, previousVMode, result;
    int gameViewChanged = 0;

    if (ui == NULL)
        return 0;

    showCfgPopup = 0;
    guiResetNotifications();
    previousTheme = thmGetGuiValue();
    previousVMode = gVMode;

    guiLock();
    themeNamesSnap = guiCopyNameList((const char **)thmGetGuiList());
    langNamesSnap = guiCopyNameList((const char **)lngGetGuiList());
    guiUnlock();
    diaSetEnum(ui, UICFG_THEME, themeNamesSnap != NULL ? themeNamesSnap : (const char **)thmGetGuiList());
    diaSetEnum(ui, UICFG_LANG, langNamesSnap != NULL ? langNamesSnap : (const char **)lngGetGuiList());
    diaSetEnum(ui, UICFG_GAMEVIEW, gameViewNames);
    diaSetEnum(ui, UICFG_VMODE, vmodeNames);
    diaSetInt(ui, UICFG_THEME, thmGetGuiValue());
    diaSetInt(ui, UICFG_LANG, lngGetGuiValue());
    diaSetInt(ui, UICFG_GAMEVIEW, gDefaultGameView);
    diaSetInt(ui, UICFG_AUTOSORT, gAutosort);
    diaSetInt(ui, UICFG_AUTOREFRESH, gAutoRefresh);
    diaSetInt(ui, UICFG_NOTIFICATIONS, gEnableNotifications);
    // Keep the editor reachable even when the current theme has no active Coverflow view; users
    // need to be able to configure it before enabling or switching to a Coverflow-capable theme.
    diaSetVisible(ui, UICFG_COVERFLOW_BUTTON, 1);
    diaSetInt(ui, UICFG_VMODE, gVMode);
    diaSetInt(ui, UICFG_WIDESCREEN, gWideScreen);
    diaSetInt(ui, UICFG_XOFF, gXOff);
    diaSetInt(ui, UICFG_YOFF, gYOff);
    diaSetInt(ui, UICFG_OVERSCAN, gOverscan);
    const char *gameIDModes[] = {_l(_STR_GAMEID_MODE_ALL), _l(_STR_GAMEID_MODE_POPSTARTER),
                                 _l(_STR_GAMEID_MODE_PS2), _l(_STR_GAMEID_MODE_OFF), NULL};
    diaSetEnum(ui, CFG_APPLYGAMEID, gameIDModes);
    diaSetInt(ui, CFG_APPLYGAMEID, guiGameIdModeFromGlobals());
    guiSettingsBeginDialog(ui);
reshow_interface:
    result = diaExecuteDialog(ui, -1, 1, &guiSettingsDisplayUpdater);
    if (result == UICFG_ARTWORK_BUTTON) {
        guiShowArtworkConfig();
        goto reshow_interface;
    }
    if (result == UICFG_COVERFLOW_BUTTON) {
        guiShowCoverflowConfig();
        goto reshow_interface;
    }
    if (result == UICFG_COLORS_BUTTON) {
        guiShowColorsConfig();
        goto reshow_interface;
    }
    if (result == UICFG_GAME_LIST_BUTTON) {
        guiShowVcdListConfig();
        goto reshow_interface;
    }
    if (result == DISPLAY_GSM_DEFAULTS_BUTTON) {
        if (guiGameShowGSConfig(1))
            guiSettingsSavePending = 1;
        goto reshow_interface;
    }

    padRumbleFlush();
    if (result != UIID_BTN_CANCEL && result != -1) {
        diaGetInt(ui, UICFG_LANG, &langID);
        diaGetInt(ui, UICFG_THEME, &themeID);
        diaGetInt(ui, UICFG_AUTOSORT, &gAutosort);
        diaGetInt(ui, UICFG_AUTOREFRESH, &gAutoRefresh);
        diaGetInt(ui, UICFG_NOTIFICATIONS, &gEnableNotifications);
        {
            int previousGameView = gDefaultGameView;
            diaGetInt(ui, UICFG_GAMEVIEW, &gDefaultGameView);
            gameViewChanged = gDefaultGameView != previousGameView;
            if (gameViewChanged)
                libViewMarkAllDirty();
        }
        diaGetInt(ui, UICFG_VMODE, &gVMode);
        diaGetInt(ui, UICFG_WIDESCREEN, &gWideScreen);
        diaGetInt(ui, UICFG_XOFF, &gXOff);
        diaGetInt(ui, UICFG_YOFF, &gYOff);
        diaGetInt(ui, UICFG_OVERSCAN, &gOverscan);
        {
            int gameIDMode;
            diaGetInt(ui, CFG_APPLYGAMEID, &gameIDMode);
            guiApplyGameIdMode(gameIDMode);
        }

        if (previousTheme != themeID && isBgmPlaying())
            bgmStop();
        applyConfig(themeID, langID, 1);
        if (gameViewChanged) {
            oplQueueLibraryDeviceUpdates();
            loadFavourites();
        }
        sfxInit(0);
        if (gEnableBGM && !isBgmPlaying())
            bgmStart();

        if (previousVMode != gVMode && guiConfirmVideoMode() == 0) {
            gVMode = previousVMode;
            applyConfig(-1, -1, 1);
        }
    }

    guiFreeNameList(themeNamesSnap);
    guiFreeNameList(langNamesSnap);
    guiSettingsEndDialog();
    guiSettingsActiveDialog = NULL;
    return guiSettingsPageResult(result);
}

static int guiSettingsLaunchUpdater(int modified)
{
    int neutrinoVideoDef;

    if (modified) {
        diaGetInt(guiSettingsActiveDialog, CFG_NEUTRINO_VIDEO, &neutrinoVideoDef);
        diaSetEnabled(guiSettingsActiveDialog, CFG_NEUTRINO_GSMCOMP, neutrinoVideoDef != 0);
    }
    return 0;
}

static int guiSettingsShowLaunch(void)
{
    const struct UIItem *parts[] = {diaLaunchConfig, diaNeutrinoDefaults};
    const int skipIDs[] = {LAUNCH_NEUTRINO_DEFAULTS_BUTTON};
    const char *defaultCoreStrs[] = {"<OPL>", "Neutrino", NULL};
    const char *neutrinoDevStrs[] = {_l(_STR_AUTO), "Memory Card", "USB", "MX4SIO", "MMCE", "HDD (exFAT)", "HDD (APA)", _l(_STR_GAMES_DEVICE), NULL};
    static const char *neutrinoVideoDefStrs[] = {"Off", "240p", "480p", "1080i x1", "1080i x2", "1080i x3", NULL};
    static const char *neutrinoGsmCompDefStrs[] = {"Off", "Type 1 (GSM/OPL)", "Type 2", "Type 3", NULL};
    struct UIItem *ui = guiSettingsCompose(parts, 2, skipIDs, 1, -1, 1);
    int result;

    if (ui == NULL)
        return 0;

    diaSetEnum(ui, CFG_DEFAULT_CORE, defaultCoreStrs);
    diaSetInt(ui, CFG_DEFAULT_CORE, gDefaultCoreLoader);
    diaSetInt(ui, CFG_PS2LOGO, gPS2Logo);
    diaSetEnum(ui, CFG_NEUTRINO_DEVICE, neutrinoDevStrs);
    diaSetInt(ui, CFG_NEUTRINO_DEVICE, gNeutrinoDevice);
    diaSetEnum(ui, CFG_NEUTRINO_VIDEO, neutrinoVideoDefStrs);
    diaSetInt(ui, CFG_NEUTRINO_VIDEO, gNeutrinoVideoDefault);
    diaSetEnum(ui, CFG_NEUTRINO_GSMCOMP, neutrinoGsmCompDefStrs);
    diaSetInt(ui, CFG_NEUTRINO_GSMCOMP, gNeutrinoGsmCompDefault);
    diaSetEnabled(ui, CFG_NEUTRINO_GSMCOMP, gNeutrinoVideoDefault != 0);
    guiSettingsBeginDialog(ui);

reshow_launch:
    result = diaExecuteDialog(ui, -1, 1, &guiSettingsLaunchUpdater);
    if (result == LAUNCH_OSD_DEFAULTS_BUTTON) {
        guiGameShowOSDLanguageConfig(1);
        goto reshow_launch;
    }
    if (result == LAUNCH_GSM_DEFAULTS_BUTTON) {
        if (guiGameShowGSConfig(1))
            guiSettingsSavePending = 1;
        goto reshow_launch;
    }
    if (result == CFG_NEUTRINO_ARGS) {
        guiShowNeutrinoArgsConfig(gNeutrinoArgs, sizeof(gNeutrinoArgs));
        goto reshow_launch;
    }

    if (result != UIID_BTN_CANCEL && result != -1) {
        diaGetInt(ui, CFG_DEFAULT_CORE, &gDefaultCoreLoader);
        diaGetInt(ui, CFG_PS2LOGO, &gPS2Logo);
        diaGetInt(ui, CFG_NEUTRINO_DEVICE, &gNeutrinoDevice);
        diaGetInt(ui, CFG_NEUTRINO_VIDEO, &gNeutrinoVideoDefault);
        diaGetInt(ui, CFG_NEUTRINO_GSMCOMP, &gNeutrinoGsmCompDefault);
        applyConfig(-1, -1, 0);
        menuReinitMainMenu();
    }

    guiSettingsEndDialog();
    guiSettingsActiveDialog = NULL;
    return guiSettingsPageResult(result);
}

static int guiSettingsPopstarterUpdater(int modified)
{
    guiVcdUpdater(modified);
    guiBdmaUpdater(modified);
    return 0;
}

static int guiSettingsShowPopstarter(void)
{
    const struct UIItem *parts[] = {diaVcdConfig, diaBdmaConfig};
    const int skipIDs[] = {VCD_BDMA_BUTTON};
    const char *popsDevStrs[] = {_l(_STR_DEFAULT), "Memory Card", "USB", "MX4SIO", "MMCE", "HDD (exFAT)", "HDD (APA)", "Custom", _l(_STR_GAMES_DEVICE), NULL};
    const char *emberDisplayStrs[] = {_l(_STR_DEFAULT), "240p", "480p", NULL};
    struct UIItem *ui = guiSettingsCompose(parts, 2, skipIDs, 1, -1, 1);
    int result;

    if (ui == NULL)
        return 0;

    // Ember's display mode. This MUST be set on `ui` -- the composed COPY -- not on the diaVcdConfig
    // template it was built from: the copy is what renders and what diaGetInt reads back. Setting
    // the template instead leaves the row on screen with no enum list and silently discards every
    // change, which is exactly the bug this line replaced.
    diaSetEnum(ui, CFG_EMBER_DISPLAY, emberDisplayStrs);
    diaSetInt(ui, CFG_EMBER_DISPLAY, gEmberDisplay);

    diaSetEnum(ui, CFG_POPSTARTER_DEVICE, popsDevStrs);
    diaSetInt(ui, CFG_POPSTARTER_DEVICE, gPopstarterDevice);
    diaSetString(ui, CFG_POPSTARTER_PATH, gPopstarterPath);
    diaSetShowDefaultWhenEmpty(ui, CFG_POPSTARTER_PATH, 1);
    diaSetInt(ui, CFG_POPSTARTER_RETROGEM_GAMEID, gPopstarterRetroGemGameID);
    diaSetVisible(ui, CFG_LBL_POPSTARTER_PATH, gPopstarterDevice == POPS_DEV_CUSTOM);
    diaSetVisible(ui, CFG_POPSTARTER_PATH, gPopstarterDevice == POPS_DEV_CUSTOM);
    guiSetBdmaSettings(ui);
    guiSettingsBeginDialog(ui);

reshow_popstarter:
    result = diaExecuteDialog(ui, -1, 1, &guiSettingsPopstarterUpdater);
    if (result == VCD_NET_BUTTON) {
        guiShowPopsNetConfig();
        goto reshow_popstarter;
    }
    if (result == VCD_LIST_BUTTON) {
        // Reuse the exact same static editor as Interface. It owns the VCD-list globals and
        // returns to the caller, so Circle naturally resumes the parent that launched it.
        guiShowVcdListConfig();
        goto reshow_popstarter;
    }

    if (result != UIID_BTN_CANCEL && result != -1) {
        char tmpPop[sizeof(gPopstarterPath)];

        diaGetInt(ui, CFG_POPSTARTER_DEVICE, &gPopstarterDevice);
        diaGetInt(ui, CFG_POPSTARTER_RETROGEM_GAMEID, &gPopstarterRetroGemGameID);
        diaGetInt(ui, CFG_EMBER_DISPLAY, &gEmberDisplay);
        diaGetString(ui, CFG_POPSTARTER_PATH, tmpPop, sizeof(tmpPop));
        if (strncmp(tmpPop, gPopstarterPath, 31) != 0)
            snprintf(gPopstarterPath, sizeof(gPopstarterPath), "%s", tmpPop);
        guiSaveBdmaSettings(ui);
        applyConfig(-1, -1, 0);
        menuReinitMainMenu();
    }

    guiSettingsEndDialog();
    guiSettingsActiveDialog = NULL;
    return guiSettingsPageResult(result);
}

enum gui_settings_page {
    SETTINGS_SOURCES = 0,
    SETTINGS_GENERAL,
    SETTINGS_NETWORK,
    SETTINGS_INTERFACE,
    SETTINGS_LAUNCH,
    SETTINGS_POPSTARTER,
    SETTINGS_CONTROLLERS,
    SETTINGS_AUDIO,
    SETTINGS_PAGE_COUNT
};

enum gui_settings_prompt_result {
    SETTINGS_PROMPT_SAVE = 1,
    SETTINGS_PROMPT_EXIT,
    SETTINGS_PROMPT_CONTINUE
};

static int guiSettingsPromptSave(void)
{
    int promptHints[3] = {_STR_SETTINGS_SAVE, _STR_SETTINGS_EXIT_WITHOUT_SAVING, _STR_SETTINGS_CONTINUE_EDITING};
    int promptIcons[3] = {CROSS_ICON, CIRCLE_ICON, TRIANGLE_ICON};

    sfxPlay(SFX_MESSAGE);
    while (1) {
        int x, y;

        guiStartFrame();
        if (guiDrawBGSettings() == 0)
            guiDrawBGPlasma();

        rmDrawRect(0, 0, screenWidth, screenHeight, gColDarker);
        rmDrawLine(50, 75, screenWidth - 50, 75, gColWhite);
        rmDrawLine(50, 410, screenWidth - 50, 410, gColWhite);
        fntRenderString(gTheme->fonts[0], screenWidth >> 1, 150, ALIGN_CENTER, 0, 0,
                        _l(_STR_SETTINGS_SAVE_PROMPT), gTheme->selTextColor);
        fntRenderString(gTheme->fonts[0], screenWidth >> 1, 205, ALIGN_CENTER, 0, 0,
                        _l(_STR_SETTINGS_SAVE_PROMPT_LINE1), gTheme->textColor);
        fntRenderString(gTheme->fonts[0], screenWidth >> 1, 230, ALIGN_CENTER, 0, 0,
                        _l(_STR_SETTINGS_SAVE_PROMPT_LINE2), gTheme->textColor);

        x = guiAlignSubMenuHints(3, promptHints, promptIcons, gTheme->fonts[0], 12, 1);
        y = gTheme->usedHeight - 32;
        x = guiDrawIconAndText(promptIcons[0], promptHints[0], gTheme->fonts[0], x, y, gTheme->textColor);
        x += 12;
        x = guiDrawIconAndText(promptIcons[1], promptHints[1], gTheme->fonts[0], x, y, gTheme->textColor);
        x += 12;
        guiDrawIconAndText(promptIcons[2], promptHints[2], gTheme->fonts[0], x, y, gTheme->textColor);
        guiEndFrame();

        readPads();
        if (getKeyOn(KEY_CROSS)) {
            sfxPlay(SFX_CONFIRM);
            return SETTINGS_PROMPT_SAVE;
        }
        if (getKeyOn(KEY_CIRCLE)) {
            sfxPlay(SFX_CANCEL);
            return SETTINGS_PROMPT_EXIT;
        }
        if (getKeyOn(KEY_TRIANGLE)) {
            sfxPlay(SFX_CURSOR);
            return SETTINGS_PROMPT_CONTINUE;
        }
    }
}

static void guiDrawSettingsIndexHints(void)
{
    int hints[2] = {_STR_SELECT, _STR_BACK};
    int icons[2] = {CROSS_ICON, CIRCLE_ICON};
    int x = guiAlignSubMenuHints(2, hints, icons, gTheme->fonts[0], 12, 2);
    int y = gTheme->usedHeight - 32;

    x = guiDrawIconAndText(icons[0], hints[0], gTheme->fonts[0], x, y, gTheme->textColor);
    x += 12;
    guiDrawIconAndText(icons[1], hints[1], gTheme->fonts[0], x, y, gTheme->textColor);
}

static int guiSettingsShowIndex(int *page)
{
    const char *labels[SETTINGS_PAGE_COUNT + 1] = {
        _l(_STR_GAME_SOURCES),
        _l(_STR_GENERAL_SYSTEM),
        _l(_STR_MENU_NETWORK),
        _l(_STR_INTERFACE_SETTINGS),
        _l(_STR_GAME_LAUNCHING),
        _l(_STR_POPSTARTER),
        _l(_STR_CONTROLLER_SETTINGS),
        _l(_STR_AUDIO_SETTINGS),
        _l(_STR_SAVE_CHANGES),
    };
    int selected = *page;
    const int itemCount = SETTINGS_PAGE_COUNT + 1;
    int spacing = 25;
    int y;

    if (selected < SETTINGS_SOURCES || selected >= SETTINGS_PAGE_COUNT)
        selected = SETTINGS_SOURCES;

    // This is intentionally rendered with the same geometry and highlight treatment as the main
    // menu. The Index is the Settings hub, not another dialog with a focus box around each row.
    while (1) {
        guiStartFrame();
        if (guiDrawBGSettings() == 0)
            guiDrawBGPlasma();

        fntRenderString(gTheme->fonts[0], screenWidth >> 1, 50, ALIGN_CENTER, 0, 0, "SETTINGS INDEX", gTheme->textColor);

        y = (gTheme->usedHeight >> 1) - (spacing * (itemCount >> 1));
        for (int i = 0; i < itemCount; i++) {
            fntRenderString(gTheme->fonts[0], screenWidth >> 1, y, ALIGN_CENTER, 0, 0, labels[i], (i == selected) ? gTheme->selTextColor : gTheme->textColor);
            y += spacing;
            if (i == SETTINGS_PAGE_COUNT - 1)
                y += spacing / 2;
        }

        guiDrawSettingsIndexHints();
        guiEndFrame();

        readPads();
        if (getKey(KEY_UP)) {
            sfxPlay(SFX_CURSOR);
            selected = (selected + itemCount - 1) % itemCount;
        } else if (getKey(KEY_DOWN)) {
            sfxPlay(SFX_CURSOR);
            selected = (selected + 1) % itemCount;
        } else if (getKeyOn(KEY_CROSS)) {
            sfxPlay(SFX_CONFIRM);
            if (selected == SETTINGS_PAGE_COUNT) {
                if (menuSaveSettings() > 0)
                    guiSettingsSavePending = 0;
            } else {
                *page = selected;
                return 1;
            }
        } else if (getKeyOn(KEY_START) || getKeyOn(KEY_CIRCLE)) {
            sfxPlay(SFX_CANCEL);
            if (!guiSettingsSavePending)
                return 0;

            int promptResult = guiSettingsPromptSave();
            if (promptResult == SETTINGS_PROMPT_SAVE) {
                if (menuSaveSettings() > 0) {
                    guiSettingsSavePending = 0;
                    return 0;
                }
            } else if (promptResult == SETTINGS_PROMPT_EXIT) {
                hddDiscardOplHomeSelection();
                guiSettingsSavePending = 0;
                return 0;
            }
        }
    }
}

void guiShowSettings(void)
{
    int page = SETTINGS_SOURCES;
    int result;

    guiSettingsShellActive = 1;
    guiSettingsSavePending = 0;
    if (!guiSettingsShowIndex(&page)) {
        hddDiscardOplHomeSelection();
        guiSettingsShellActive = 0;
        guiSettingsSavePending = 0;
        return;
    }

    while (1) {
        guiSettingsCurrentPage = page;
        switch (page) {
            case SETTINGS_GENERAL:
                result = guiSettingsShowGeneral();
                break;
            case SETTINGS_SOURCES:
                result = guiSettingsShowSources();
                break;
            case SETTINGS_NETWORK:
                result = guiShowNetConfig();
                break;
            case SETTINGS_INTERFACE:
                result = guiSettingsShowInterface();
                break;
            case SETTINGS_LAUNCH:
                result = guiSettingsShowLaunch();
                break;
            case SETTINGS_POPSTARTER:
                result = guiSettingsShowPopstarter();
                break;
            case SETTINGS_CONTROLLERS:
                result = guiShowControllerConfig();
                break;
            case SETTINGS_AUDIO:
                result = guiShowAudioConfig();
                break;
            default:
                hddDiscardOplHomeSelection();
                guiSettingsShellActive = 0;
                guiSettingsSavePending = 0;
                return;
        }

        if (result == DIA_RESULT_INDEX) {
            if (!guiSettingsShowIndex(&page)) {
                hddDiscardOplHomeSelection();
                guiSettingsShellActive = 0;
                guiSettingsSavePending = 0;
                return;
            }
        } else if (result == DIA_RESULT_NEXT) {
            page = (page + 1) % SETTINGS_PAGE_COUNT;
        } else if (result == DIA_RESULT_PREV) {
            page = (page + SETTINGS_PAGE_COUNT - 1) % SETTINGS_PAGE_COUNT;
        } else {
            hddDiscardOplHomeSelection();
            guiSettingsShellActive = 0;
            guiSettingsSavePending = 0;
            return;
        }
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
        /* OOM: release the semaphore so future callers are not permanently locked out. The op is
           ours to release too -- ownership transfers on enqueue, so a caller that got -1 back has
           no pointer left to free and would otherwise leak the very object we could not queue. */
        SignalSema(gSemaId);
        free(op);
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
            result = submenuAppendItem(item->menu.subMenu, item->submenu.icon_id, item->submenu.text, item->submenu.id, item->submenu.text_id);
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
            int mode = -1;
            item_list_t *list = (item_list_t *)item->menu.menu->userdata;
            if (list != NULL)
                mode = list->mode;
            submenuSort(item->menu.subMenu, mode);
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
            menuHintsLock();
            menuAddHint(item->menu.menu, item->hint.text_id, item->hint.icon_id);
            menuHintsUnlock();
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

        // FREE THE OPERATION, NOT JUST ITS QUEUE NODE. Every deferred op is TWO allocations --
        // guiOpCreate() mallocs the gui_update_t, guiDeferUpdate() mallocs the list node that
        // carries it -- and this loop only ever released the node. The op leaked, always, and no
        // commit in this repository's history has ever freed one.
        //
        // The rate is what makes it fatal rather than untidy: updateMenuFromGameList() calls
        // guiOpCreate(GUI_OP_APPEND_MENU) once PER GAME ROW, so a single list rebuild leaks one
        // object per row and every page change rebuilds. Enough navigation and the heap is gone.
        //
        // guiHandleOp() reads the op but never retains the pointer, and submenu.text points into
        // the support's own storage rather than the op, so releasing it here is safe.
        free(td->item);
        free(td);

        gCompletedOps++;
    }
    // Clear the now-dangling tail pointer INSIDE the lock. The drain loop freed every node and emptied
    // gUpdateList; doing this AFTER SignalSema let an IO-thread guiDeferUpdate slip in (rebuild the list,
    // set gUpdateEnd) only to have it clobbered here -> the next enqueue's gUpdateEnd->next was a NULL deref.
    // Not a rare interleaving: the IO worker is prio 30 and the GUI thread prio 31, and lower wins on EE,
    // so the thread woken by SignalSema preempts us immediately.
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

/* Render ONE frame of the current screen plus the busy animation, for a synchronous main-thread
   wait on hardware that cannot be moved off that thread.

   guiMainLoop dispatches handleInput AFTER guiEndFrame precisely so an input handler may drive
   renderman, so this is safe from there -- but it takes guiLock, so it must never be called from
   inside an existing guiStartFrame/guiEndFrame bracket or from any other thread.

   The vsync wait inside guiEndFrame also paces the caller's poll loop, which is why callers can
   drop their own DelayThread and measure the budget with clock(). */
void guiRenderProbeFrame(void)
{
    guiStartFrame();
    guiShow();
    guiDrawBusy(0x80);
    guiEndFrame();
}

// Boot-splash status line setter. Pass NULL to clear. Main-thread only (writes gBootStatus,
// which only guiRenderGreeting on the same thread reads). guiRenderGreeting prefers any
// IO-thread sticky label over this.
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

// Boot-step localizer setter, called from the deferred IO-thread boot steps. `label` MUST be a
// static string (an _l() lang entry or literal) since only a POINTER to it is stored -- no copy,
// no shared buffer, so no data race with the main-thread render (a single aligned pointer
// store/load is atomic on the EE). If a step wedges, its label stays on the splash.
void guiSetBootStatusSticky(const char *label)
{
    if (label == NULL)
        return;
    gBootStickyLabel = label;
}

void guiSetBootStatusStickyCopy(const char *label)
{
    int target;

    if (label == NULL)
        return;

    target = (gBootStickyLabel == gBootStickyCopy[0]) ? 1 : 0;
    snprintf(gBootStickyCopy[target], sizeof(gBootStickyCopy[target]), "%s", label);
    gBootStickyLabel = gBootStickyCopy[target];
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

    // Sit the logo above centre rather than dead centre: the version + status lines hang below it,
    // so a centred logo pushes that block low and reads bottom-heavy. Clamped so a tall logo (or a
    // short theme height) can never ride off the top of the screen.
    int logoCenterY = (gTheme->usedHeight >> 1) - BOOT_LOGO_RISE;

    GSTEXTURE *logo = thmGetTexture(logoTex);
    if (logo) {
        int drawY = logoCenterY;
        if (drawY < (int)(logo->Height >> 1))
            drawY = logo->Height >> 1;

        mycolor = GS_SETREG_RGBA(0x80, 0x80, 0x80, alpha);
        rmDrawPixmap(logo, screenWidth >> 1, drawY, ALIGN_CENTER, logo->Width, logo->Height, SCALING_RATIO, mycolor, 0);
    }

    // Boot info: the RiptOPL version + a live status line, faded with the splash.
    // Reuses gTheme->fonts[0] (always-loaded built-in) and the same OPL_VERSION About shows.
    u64 infoColor = GS_SETREG_RGBA(0x80, 0x80, 0x80, alpha);
    char verLine[48];
    snprintf(verLine, sizeof(verLine), "RiptOPL %s", OPL_VERSION);
    fntRenderString(gTheme->fonts[0], screenWidth >> 1, logoCenterY + 80, ALIGN_CENTER, 0, 0, verLine, infoColor);
    // Prefer an IO-thread boot-step label (the localizer) over the main-thread scan/Ready line,
    // so a wedged step names itself. gBootStickyLabel is a single atomic pointer to a static string.
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
    // |delta|<=255: endpoints land exactly on blend / curbgColor). Every OTHER half of this feature
    // -- the Colors-page picker, the theme key, the default, the struct field -- was already here;
    // this loop was the one consumer that never read it, so the picker stored a colour the plasma
    // never used.
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
        // A disk theme with use_default=0 that omits circle.png/cross.png leaves this texture NULL
        // (thmGetTexture's documented contract); guiDrawIconAndText in this same file already guards.
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
        // A disk theme with use_default=0 that omits circle.png/cross.png leaves this texture NULL
        // (thmGetTexture's documented contract); guiDrawIconAndText in this same file already guards.
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

static int endIntro = 0;           // Break intro loop and start 'Last Played Auto Start' countdown
static int gIntroSplashActive = 0; // The intro is a splash, never a generic I/O-spinner screen.
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

    if (!oplIsBootInProgress() && !gIntroSplashActive && busyAlpha > 0x00)
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
    if (!pending && DisableCron == 0 && endIntro) {
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

    // Art pipeline HUD (Settings -> Advanced -> Debug). Q = requests queued, A = being read/decoded
    // right now, R = turned away by the depth cap, D = completed. What it is for: "art stops after a
    // few covers" has two very different causes that look identical on screen. If D keeps climbing
    // the pipeline is working and simply slow. If D stops while R climbs, requests are being made
    // and refused -- and if Q stays pinned at the cap with A at 0 and nothing completing, the
    // in-flight counter has drifted above the real number in flight and is refusing everything
    // forever. Cheap enough to leave in: four integer reads and one string, only when debug is on.
    if (gEnableDebug) {
        int q = 0, a = 0, r = 0, d = 0;
        int lastMs = 0, okMs = 0, w = 0, h = 0;
        char artdbg[224]; // 96 truncated the IO counters off the end -- snprintf is safe, but silently
                          // dropping the field you added to diagnose something is its own trap.
        cacheDebugCounters(&q, &a, &r, &d);
        cacheDebugLastLoad(&lastMs, &okMs, &w, &h);
        // ms = the last load, ok = the last SUCCESSFUL one with its decoded size. One cover at a few
        // hundred ms means the pipeline is fine and the wait is how many images we ask for; one at
        // seconds means the cost is inside a single read on this device.
        u32 fWorst = 0, fOver = 0;
        u32 fLast = guiGetFrameWatchdog(&fWorst, &fOver);
        // F<last frame ms>/<worst ms> OV<frames over ~1.5 vsyncs>. Read it AS a step is
        // swallowed: OV climbing at that instant means the EE stalled (cause is EE-side); OV
        // standing still while the step is still lost means the EE polled on time and the
        // input died on the IOP.
        u32 padNR = 0, padEmpty = 0;
        padGetFaultCounters(&padNR, &padEmpty);
        // NR = longest run of polls freepad could not be read at all; MT = longest run of polls that
        // read fine but were EMPTY. A press spans 4-6 frames, so a RUN >= 4 in either column is a
        // swallowed press -- and which column it lands in decides where the fault lives.
        // IO = the shared ioman queue, which is what raises the busy overlay. Two pending counts
        // (Simple / Menu-rescan) then their running totals. ART IS NO LONGER IN IT -- covers have
        // their own thread and their own queue now, so the old third column would have reported a
        // permanent 0 and quietly stopped being a diagnostic.
        //
        // X is the art thread's DROPPED count: requests released without loading (cancelled,
        // superseded, torn down). It is the complement of D -- every request the worker takes ends
        // in exactly one of the two -- so D and X both flat while covers keep being asked for is the
        // signature of a leak, and X racing while D crawls is the signature of over-cancelling.
        // That pair is how the born-stale bug was caught, and it is worth keeping legible.
        // ...and the last SUCCESSFUL load's cost WITH ITS DECODED PIXEL SIZE, which is the field that
        // settles the open question about the VCD page. Covers there measured 4309, 6847, 8764 and
        // 8813 ms against 55-81 ms for a PS2 cover on the same USB stick, and the two explanations --
        // "PS1 box scans are simply enormous PNGs" and "something about that path is slow" -- are
        // indistinguishable from a millisecond alone. WxH tells them apart in one glance: a
        // 1000x1400 scan decoding for eight seconds is arithmetic, a 184x256 one is a bug.
        // OE = staged-art opens that failed with errno != ENOENT, i.e. covers branded permanently
        // absent on evidence that was not the filesystem saying "no such file". Must stay 0; if it
        // climbs, transient bus errors are eating real art and texStagedOpenIsAbsence needs master's
        // ENOENT-only rule.
        // O:<hit>/<miss> = cost of the last staged art open() and of the last one that FAILED. THIS IS
        // THE FIELD TO READ NOW. If a missing cover's open costs seconds while a successful one costs
        // milliseconds, the cost is the filesystem walking the whole directory to prove a file is not
        // there -- and an in-RAM index of the art folder becomes worth building. If both are small,
        // the seconds are in the transfer or in contention and the index would fix nothing.
        // IX<dirs>/<absent> = art directories held in RAM, and probes answered "not there" without
        // touching the device. The second number is the whole point of the index: every one of those
        // is a directory walk that did not happen.
        int ixDirs = 0;
        unsigned int ixAbsent = 0, ixFailed = 0;
        int wOpen = 0, wPend = 0, wMiss = 0, wMenu = 0, wBgm = 0;
        texDebugWorstOpen(&wOpen, &wPend, &wMiss, &wMenu, &wBgm);
        artIndexDebug(&ixDirs, &ixAbsent, &ixFailed);
        // SX<stale>/<full> = SFX silently discarded: stale = cursor ticks aged out (harmless), full =
        // ANY sound dropped by the saturated 8-deep ring -- the one that eats deliberate presses.
        // SP<last>/<max> = per-press audsrv RPC wall ms, measured on the dispatch thread. The diag
        // existed since #340 but NOTHING READ IT -- the classic half-landed instrument. Pair them
        // for #364 ("effects cut without a ~5 s cooldown"): SP huge with SX flat = the IOP is slow;
        // SP small with SX/full climbing = the dispatcher is starved of CPU.
        unsigned int sxStale = 0, sxFull = 0, spLastMs = 0, spMaxMs = 0;
        sfxGetDropDiag(&sxStale, &sxFull);
        sfxGetPlayDiag(&spLastMs, &spMaxMs);
        int padAct0 = 0, padAln0 = 0, padAct1 = 0, padAln1 = 0;
        padGetActuatorDiag(&padAct0, &padAln0, &padAct1, &padAln1);
        char actBuf[16];
        snprintf(actBuf, sizeof(actBuf), "ACT%d%s/%d%s",
                 padAct0, (padAct0 > 0 ? (padAln0 ? "a" : "u") : ""),
                 padAct1, (padAct1 > 0 ? (padAln1 ? "a" : "u") : ""));
        snprintf(artdbg, sizeof(artdbg), "Q%d A%d D%d X%d %dms(ok %dms %dx%d) O:%d/%d W%d@%d/%d/%d%c OE%u IX%d/%u/%u KL%u TF%u SX%u/%u SP%u/%u F%u/%u %s NR%u MT%u IO%d/%d",
                 q, a, d, cacheDebugDropped(), lastMs, okMs, w, h,
                 gTexLastOpenMs, gTexLastMissOpenMs, wOpen, wPend, wMenu, wBgm, wMiss ? 'm' : 'h', gTexStagedOpenNonEnoent, ixDirs, ixAbsent, ixFailed, cacheDebugKeyTooLong(), cacheDebugTransientFail(), sxStale, sxFull, spLastMs, spMaxMs,
                 (unsigned)(fLast / 1000), (unsigned)(fWorst / 1000),
                 actBuf, (unsigned)padNR, (unsigned)padEmpty,
                 ioGetPending(IO_CUSTOM_SIMPLEACTION), ioGetPending(IO_MENU_UPDATE_DEFFERED));
        fntRenderString(gTheme->fonts[0], 8, screenHeight - 24, ALIGN_NONE, 0, 0, artdbg, GS_SETREG_RGBA(255, 255, 0, 128));
    }
}

static void guiReadPads()
{
    // A transition polls without dispatching input. Freeze against the triggering sample so that
    // button cannot replay on the destination, while a different button held through the fade can
    // still register there (see padFreezeEdgeBaseline in pad.c -- deliberately NOT a poll pause).
    padFreezeEdgeBaseline(screenHandlerTarget != NULL);

    if (readPads())
        guiInactiveFrames = 0;
    else
        guiInactiveFrames++;

    // Art thread's per-frame tick, and it must sit HERE, immediately after the poll: it samples the
    // currently HELD direction so the worker can refuse to start an SIO2 cover mid-navigation, and
    // that sample is only meaningful against a pad state read this frame. It also re-wakes a worker
    // that has queued work but nothing running, which closes the lost-wakeup class outright.
    // O(1), never blocks, and every call inside it is GUI-thread-only by design.
    cacheTickArt();
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

void guiIntroLoop(void)
{
    int greetingAlpha = 0x80;
    const int fadeFrameCount = 0x80 / 2;
    const int fadeDuration = (fadeFrameCount * 1000) / 55; // Average between 50 and 60 fps
    // Elapsed, not an absolute deadline: this comparison is the ONLY exit from the loop below,
    // so a clock() wrap between arming and testing it parks OPL on the boot splash for up to
    // ~71 minutes with no timeout and no escape. clock() is not reset when OPL is re-launched
    // from a game, so a console that has been on for a while rolls this dice on every boot.
    clock_t tFadeStart = 0;
    clock_t tFadeDelay = 0;
    int tFadeArmed = 0;

    // oplIsBootInProgress() can become false while background startup work is still queued. That
    // is correct for runtime overlays, but the animated greeting owns the entire intro lifecycle.
    gIntroSplashActive = 1;
    while (!endIntro) {
        guiStartFrame();

        if (greetingAlpha < 0x80)
            guiShow();

        if (greetingAlpha > 0)
            guiRenderGreeting(greetingAlpha);

        // Initialize boot sound
        if (gInitComplete && !tFadeArmed && gEnableBootSND) {
            // Start playing sound
            sfxPlay(SFX_BOOT);
            // Calculate transition delay
            tFadeStart = clock();
            // CLAMP AT ZERO. sfxGetSoundDuration returns 0 when audsrv failed to initialise, and a
            // custom theme's boot sound can legitimately be shorter than the fade -- either way this
            // subtraction goes negative, and clock_t is UNSIGNED LONG, so the cast turns ~-1163 into
            // ~4.29 billion and the wait below becomes ~71 minutes on the boot splash with no exit.
            // The absolute-deadline form this replaced was accidentally immune (a deadline in the
            // past reads as already elapsed); converting it to the elapsed form is what exposed it.
            int fadeDelayMs = sfxGetSoundDuration(SFX_BOOT) - fadeDuration;
            if (fadeDelayMs < 0)
                fadeDelayMs = 0;
            tFadeDelay = (clock_t)fadeDelayMs * (CLOCKS_PER_SEC / 1000);
            tFadeArmed = 1;
        }

        if (gInitComplete && (clock() - tFadeStart) >= tFadeDelay)
            greetingAlpha -= 2;

        if (greetingAlpha <= 0)
            endIntro = 1;

        guiDrawOverlays();

        guiHandleDeferredOps();

        guiEndFrame();

        if (!screenHandlerTarget && screenHandler)
            screenHandler->handleInput();
    }
    gIntroSplashActive = 0;
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

        // The DHCP notice is a misconfiguration warning, not a courtesy popup -- show it even
        // when the user has notifications off (it explains an otherwise-silent empty games page).
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

    // The memset used to run unconditionally, so an allocation failure faulted HERE rather than
    // returning NULL -- which made every "if (!op)" guard at the call sites unreachable code. The
    // callers were written expecting to handle OOM; let them.
    if (op == NULL)
        return NULL;

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

// Greedily word-wrap `text` to innerW, writing '\n'-separated lines into out. Returns the line count.
//
// fntRenderString does NOT wrap: it lays a string on ONE line and CLIPS at the window edge (only an
// explicit '\n' starts a new line), so a long message ran off BOTH screen edges and was unreadable
// -- reported on the 1080p warning, which is simply the longest string we ship.
// NOTE: src/dia.c carries the same algorithm inline for its hint box. Deliberately not merged here:
// that copy is entangled with the hint box's own geometry (it needs the line count to size the box
// before rendering), and this is a message-box fix, not a refactor of a working path.
static int guiWrapText(const char *text, int innerW, char *out, int outSize)
{
    int wlen = 0, lineW = 0, lines = 1;
    int spaceW = rmUnScaleX(fntCalcDimensions(gTheme->fonts[0], " "));
    const char *p = text;

    if (out == NULL || outSize <= 0)
        return 0;

    while (*p) {
        while (*p == ' ') // collapse runs of spaces; we re-insert our own single separators
            p++;
        if (!*p)
            break;

        // An explicit newline in the source string is honoured as a hard break.
        if (*p == '\n') {
            if (wlen < outSize - 1)
                out[wlen++] = '\n';
            lines++;
            lineW = 0;
            p++;
            continue;
        }

        char word[96];
        int k = 0;
        while (*p && *p != ' ' && *p != '\n' && k < (int)sizeof(word) - 1)
            word[k++] = *p++;
        word[k] = '\0';
        int wordW = rmUnScaleX(fntCalcDimensions(gTheme->fonts[0], word));

        if (lineW > 0 && (lineW + spaceW + wordW) > innerW) { // does not fit -> break the line
            if (wlen < outSize - 1)
                out[wlen++] = '\n';
            lines++;
            lineW = 0;
        } else if (lineW > 0) { // same line -> re-insert the separating space
            if (wlen < outSize - 1)
                out[wlen++] = ' ';
            lineW += spaceW;
        }
        for (int i = 0; i < k && wlen < outSize - 1; i++)
            out[wlen++] = word[i];
        lineW += wordW;
    }
    out[wlen] = '\0';
    return lines;
}

int guiMsgBox(const char *text, int addAccept, struct UIItem *ui)
{
    int terminate = 0;
    // Wrapped once up front, not per frame: the result depends only on `text` and the screen width.
    char wrapped[512];
    int lines = guiWrapText(text, screenWidth - 120, wrapped, sizeof(wrapped));

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

        // With a `ui` the CALLER chose what sits behind this box, so keep that context visible.
        // With ui == NULL the backdrop is just whichever menu happened to be on screen -- it carries
        // no information and, at gColDarker's ~75%, left the menu legible straight through the
        // message. Errors get their own background in that case.
        rmDrawRect(0, 0, screenWidth, screenHeight, ui ? gColDarker : gColBlack);

        rmDrawLine(50, 75, screenWidth - 50, 75, gColWhite);
        rmDrawLine(50, 410, screenWidth - 50, 410, gColWhite);

        // Render the wrapped lines ONE AT A TIME: ALIGN_CENTER centres on the measured width of the
        // WHOLE string, so handing it a multi-line buffer would centre it as a single long run and
        // leave every line off-centre. Block is vertically centred on the old single-line position.
        {
            int lineH = MENU_ITEM_HEIGHT;
            int ly = (gTheme->usedHeight >> 1) - ((lines - 1) * lineH) / 2;
            const char *ls = wrapped;
            while (ls != NULL) {
                const char *eol = strchr(ls, '\n');
                char line[192];
                int n = eol ? (int)(eol - ls) : (int)strlen(ls);
                if (n > (int)sizeof(line) - 1)
                    n = (int)sizeof(line) - 1;
                memcpy(line, ls, n);
                line[n] = '\0';
                fntRenderString(gTheme->fonts[0], screenWidth >> 1, ly, ALIGN_CENTER, 0, 0, line, gTheme->textColor);
                ly += lineH;
                ls = eol ? eol + 1 : NULL;
            }
        }
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
    // Neither of these loops calls readPads(), so the rumble decay is frozen for the whole wait.
    // The IOP LATCHES the actuator state, so a pulse armed by the SFX_CONFIRM that started this
    // save keeps the motor spinning for the entire blocking operation -- seconds, at full level.
    // GUI thread only: every libpad call in pad.c is, and applyConfig() is NOT a safe home for
    // this because _loadConfig() reaches it from the IO worker.
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

    // A rejected request never runs, so nothing will ever clear *ptr -- without this the loop below
    // would spin forever on a queue-full/failed put. Clear it here so the caller falls into its
    // normal failure path instead of hanging.
    if (ioPutRequest(type, data) != IO_OK) {
        *ptr = 0;
        return;
    }

    // Bound the wait when the caller asks (timeoutMs > 0). A deferred IO that never completes -- a
    // failing HDD/card that wedges the single IOP fileXio channel mid-write -- would otherwise spin
    // here forever and freeze the GUI on "Saving..." with no escape. After a timeout far beyond any
    // real config save/load we stop waiting and clear the status, so the caller falls into its normal
    // failure path (saveConfig -> "Error saving settings"), exactly like the put-failure branch above.
    // This does NOT recover the stuck IOP -- the IO worker runs requests one at a time and stays
    // blocked on the wedged one until reboot -- but a readable error beats an apparent hard lock.
    // Memory-safe: every caller's *ptr is a file-static int and `data` a file-static handler, so a
    // late write by the still-blocked IO thread is harmless. timeoutMs <= 0 keeps the original
    // unbounded wait (the compat-list update, which is a network fetch with no meaningful bound).
    // clock() is the same idiom used elsewhere in this file; the (clock() - startTick) ELAPSED form
    // is single-wrap-safe, unlike an absolute clock()+limit deadline.
    clock_t startTick = clock();
    clock_t limitTicks = (clock_t)timeoutMs * (CLOCKS_PER_SEC / 1000);
    gLastDeferredTimedOut = 0;
    while (*ptr) {
        if (timeoutMs > 0 && (clock() - startTick) >= limitTicks) {
            LOG("guiHandleDeferedIO: deferred IO unfinished after %d ms; storage stuck, abandoning wait\n", timeoutMs);
            *ptr = 0;
            // The handler never ran. Callers must not report a device error against their target
            // path on the strength of this -- nothing touched it.
            gLastDeferredTimedOut = 1;
            break;
        }
        guiRenderTextScreen(message);
    }
}

void guiGameHandleDeferedIO(int *ptr, struct UIItem *ui, int type, void *data)
{
    // Neither of these loops calls readPads(), so the rumble decay is frozen for the whole wait.
    // The IOP LATCHES the actuator state, so a pulse armed by the SFX_CONFIRM that started this
    // save keeps the motor spinning for the entire blocking operation -- seconds, at full level.
    // GUI thread only: every libpad call in pad.c is, and applyConfig() is NOT a safe home for
    // this because _loadConfig() reaches it from the IO worker.
    padRumbleFlush();

    // A rejected request never runs, so nothing will ever clear *ptr and the loop below spins
    // forever. The sibling guiHandleDeferedIO already checks this; this one did not.
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
    guiStartFrame();

    guiShow();

    // Opaque backdrop, not gColDarker. This is a non-interactive STATUS screen ("Please wait",
    // "NBD Server unloading..."), and gColDarker is only alpha 0x60 of a 0x80 maximum -- i.e. ~75%
    // -- so the menu underneath stayed clearly legible through the message. On a CRT that reads as
    // a transparent error with the settings menu showing through it, which is exactly what the
    // tester reported (and photographed) three times. Whatever is behind a status screen carries no
    // information, so it is hidden. Interactive dialogs deliberately KEEP the translucent overlay
    // (see guiConfirmVideoMode, where seeing the mode behind the prompt is the entire point).
    rmDrawRect(0, 0, screenWidth, screenHeight, gColBlack);

    fntRenderString(gTheme->fonts[0], screenWidth >> 1, gTheme->usedHeight >> 1, ALIGN_CENTER, 0, 0, message, gTheme->textColor);

    guiDrawOverlays();

    guiEndFrame();
}

// ---- Visual GameID barcode (Pixel FX / RetroGEM / PS2Digital HDMI auto-profile) ----
// Renders the CosmicScale "GameID" barcode just before a game is handed to its core, so an HDMI
// scaler can auto-load that game's per-title display profile. Encoding is the canonical CosmicScale
// scheme (start word 0xA5 / end word 0xD5 / length byte / additive 0x100-sum checksum), drawn with
// rmDrawRect exactly as CosmicScale's own OPL fork does. Gated behind gApplyGameID, which ships OFF
// (fork commit cc2cdfed, "GameID defaults OFF") -- the HDMI latch is only verifiable on real GameID
// hardware, so this stays opt-in until a tester confirms it. The fork's own header comment here still
// claimed "default ON since 120045d0"; setDefaults() has said 0 since cc2cdfed, so that claim is stale
// and is not carried over.
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
      strip above, so the #269 premise is untouched.

      NOTE(rebuild): the fork's comment here also credited itemExecSelect with hand-pumping a
      guiRenderBusyFrame() around this call, which is what used to cover the GameID-OFF config.
      That helper was removed again by fork commit 79865cde ("Restore loading icon piping to
      official OPL baseline") and does not exist on fork master or here -- so with the barcode
      off, launch prep still renders nothing. Re-adding the hand-pumped launch-prep loader is
      checklist item 48's business, not this step's.
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
    // Pre-GUI callers exist: autolaunch (miniInit) never runs rmInit/thmInit/guiInit, so rendering
    // here would draw through a NULL gsGlobal/gTheme. Launch-path helpers shared between the menu
    // and autolaunch (the Neutrino/VMC toasts in bdmsupport/hddsupport/supportbase) rely on this
    // guard instead of each call site checking the autolaunch globals.
    if (gTheme == NULL) {
        LOG("guiWarning (pre-GUI): %s\n", text);
        return;
    }

    guiStartFrame();

    guiShow();

    // Same reasoning as guiRenderTextScreen: a warning is a non-interactive surface shown for a
    // fixed number of frames, so it gets an opaque backdrop rather than a ~75% wash.
    rmDrawRect(0, 0, screenWidth, screenHeight, gColBlack);

    rmDrawLine(50, 75, screenWidth - 50, 75, gColWhite);
    rmDrawLine(50, 410, screenWidth - 50, 410, gColWhite);

    fntRenderString(gTheme->fonts[0], screenWidth >> 1, gTheme->usedHeight >> 1, ALIGN_CENTER, screenWidth, screenHeight, text, gTheme->textColor);

    guiEndFrame();

    delay(count);
}

int guiConfirmVideoMode(void)
{
    // Elapsed form. This auto-revert is the ONLY thing that rescues a user whose new video mode
    // does not sync -- there is nothing on screen to read and nothing to aim at. A deadline that
    // straddles the clock() wrap would leave them there.
    clock_t timeStart;
    int terminate = 0;

    sfxPlay(SFX_MESSAGE);

    timeStart = clock();
    while (!terminate) {
        guiStartFrame();

        readPads();

        if (getKeyOn(gSelectButton == KEY_CIRCLE ? KEY_CROSS : KEY_CIRCLE))
            terminate = 1;
        else if (getKeyOn(gSelectButton))
            terminate = 2;

        // If the user fails to respond within the timeout period, deem it as a cancel operation.
        if ((clock() - timeStart) >= (clock_t)OPL_VMODE_CHANGE_CONFIRMATION_TIMEOUT_MS * (CLOCKS_PER_SEC / 1000))
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
