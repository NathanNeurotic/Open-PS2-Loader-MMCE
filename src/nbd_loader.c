#include "include/opl.h"
#include "include/nbd_loader.h"
#include "include/ioman.h"
#include "include/gui.h"
#include "include/util.h"
#include "include/ethsupport.h"
#include "include/extern_irx.h"
#include "include/system.h"
#include "include/pad.h"
#include "include/sound.h"
#include "include/log.h"

// reset() was static in opl.c.
// We need to call sysReset and mcInit manually or expose reset().
// sysReset is in system.h? No, system.c.
// Let's implement local reset similar to opl.c or expose it.
// opl.c: static void reset(void) { sysReset(SYS_LOAD_MC_MODULES | SYS_LOAD_USB_MODULES | SYS_LOAD_ISOFS_MODULE); mcInit(MC_TYPE_XMC); }
static void local_reset(void)
{
    sysReset(SYS_LOAD_MC_MODULES | SYS_LOAD_USB_MODULES | SYS_LOAD_ISOFS_MODULE);
    mcInit(MC_TYPE_XMC);
}

static int loadLwnbdSvr(void)
{
    int ret, padStatus;
    struct lwnbd_config
    {
        char defaultexport[32];
        uint8_t readonly;
    };
    struct lwnbd_config config;

    // deint audio lib while nbd server is running
    audioEnd();

    // block all io ops, wait for the ones still running to finish
    ioBlockOps(1);
    guiExecDeferredOps();

    // Deinitialize all support without shutting down the HDD unit.
    deinit(NO_EXCEPTION, IO_MODE_SELECTED_ALL);
    clearErrorMessage(); /* At this point, an error might have been displayed (since background tasks were completed).
                            Clear it, otherwise it will get displayed after the server is closed. */

    unloadPads();
    // sysReset(0); // usefull ? printf doesn't work with it.

    /* compat stuff for user not providing name export (useless when there was only one export) */
    ret = strlen(gExportName);
    if (ret == 0)
        strcpy(config.defaultexport, "hdd0");
    else
        strcpy(config.defaultexport, gExportName);

    config.readonly = !gEnableWrite;

    // see gETHStartMode, gNetworkStartup ? this is slow, so if we don't have to do it (like debug build).
    ret = ethLoadInitModules();
    if (ret == 0) {
        ret = sysLoadModuleBuffer(&ps2atad_irx, size_ps2atad_irx, 0, NULL); /* gHDDStartMode ? */
        if (ret >= 0) {
            ret = sysLoadModuleBuffer(&lwnbdsvr_irx, size_lwnbdsvr_irx, sizeof(config), (char *)&config);
            if (ret >= 0)
                ret = 0;
        }
    }

    padInit(0);

    // init all pads
    padStatus = 0;
    while (!padStatus)
        padStatus = startPads();

    // now ready to display some status

    return ret;
}

static void unloadLwnbdSvr(void)
{
    ethDeinitModules();
    unloadPads();

    local_reset();

    // Initialize logging again if needed, or rely on existing hooks
    // In opl.c reset() calls sysReset, which reboots IOP.
    // Logging might need re-init if it depends on IOP modules (like files).
    // log_init() was added to opl.c main.
    // We should probably re-init logging here if it was lost.
    log_init();

#ifdef __EESIO_DEBUG
    // ee_sio_start(38400, 0, 0, 0, 0, 1); // Not easily accessible without header?
    // It's usually in kernel.h or sio.h.
    // Just skip specific debug macros re-init if not critical, or use what is available.
#endif
#ifdef __DEBUG
    // debugSetActive is in debug.h
    ioPutRequest(IO_CUSTOM_SIMPLEACTION, &debugSetActive);
#endif

    // reinit the input pads
    padInit(0);

    int ret = 0;
    while (!ret)
        ret = startPads();

    // now start io again
    ioBlockOps(0);

    // init all supports again
    initAllSupport(1);

    audioInit();
    sfxInit(0);
    if (gEnableBGM)
        bgmStart();
}

void handleLwnbdSrv(void)
{
    char temp[256];
    // prepare for lwnbd, display screen with info
    guiRenderTextScreen(_l(_STR_STARTINGNBD));
    if (loadLwnbdSvr() == 0) {
        snprintf(temp, sizeof(temp), "%s", _l(_STR_RUNNINGNBD));
        guiMsgBox(temp, 0, NULL);
    } else
        guiMsgBox(_l(_STR_STARTFAILNBD), 0, NULL);

    // restore normal functionality again
    guiRenderTextScreen(_l(_STR_UNLOADNBD));
    unloadLwnbdSvr();
}
