#include "include/opl.h"
#include "include/autolaunch.h"
#include "include/ioman.h"
#include "include/util.h"
#include "include/config.h"
#include "include/hddsupport.h"
#include "include/bdmsupport.h"
#include "include/hdd.h"
#include "include/supportbase.h"
#include "include/log.h"

// Needed for USBMASS_IOCTL_GET_DRIVERNAME
#include <usbhdfsd-common.h>

extern void *_gp;

// Forward declarations or externs if not in headers
// These were static in opl.c. Since we are moving them, we need to handle dependencies.
// checkLoadConfigBDM/HDD will be moved to opl_config.c later.
// For now, I will create local static copies or expose them?
// The plan step "Refactor: Extract OPL Configuration Logic" comes after.
// So checkLoadConfigBDM/HDD are still in opl.c and static.
// I MUST expose them or duplicate them.
// To avoid duplication, I will expose them in opl.h temporarily?
// No, I can't easily change opl.c static without separate step.
// Actually, I can modify opl.c in this step too.
// But checkLoadConfigBDM/HDD are used by tryAlternateDevice in opl.c.
// So I should leave them in opl.c and expose them.

// However, I'm refactoring Auto-Launch.
// `miniInit` uses `checkLoadConfigBDM` and `checkLoadConfigHDD`.
// `miniInit` also uses `setDefaults`.

// I will add prototypes here for now and rely on linker, assuming I remove `static` in opl.c.
void setDefaults(void);
int checkLoadConfigBDM(int types);
int checkLoadConfigHDD(int types);

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
        delay(6); // Wait for the device to be detected.
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

void autoLaunchHDDGame(char *argv[])
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

void autoLaunchBDMGame(char *argv[])
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

    snprintf(path, sizeof(path), "mass0:");
    int dir = fileXioDopen(path);
    if (dir >= 0) {
        fileXioIoctl2(dir, USBMASS_IOCTL_GET_DRIVERNAME, NULL, 0, &gAutoLaunchDeviceData->bdmDriver, sizeof(gAutoLaunchDeviceData->bdmDriver) - 1);
        fileXioIoctl2(dir, USBMASS_IOCTL_GET_DEVICE_NUMBER, NULL, 0, &gAutoLaunchDeviceData->massDeviceIndex, sizeof(gAutoLaunchDeviceData->massDeviceIndex));

        if (!strcmp(gAutoLaunchDeviceData->bdmDriver, "ata") && strlen(gAutoLaunchDeviceData->bdmDriver) == 3)
            bdmResolveLBA_UDMA(gAutoLaunchDeviceData);

        fileXioDclose(dir);
    }

    if (gBDMPrefix[0] != '\0') {
        snprintf(path, sizeof(path), "mass0:%s/CFG/%s.cfg", gBDMPrefix, gAutoLaunchBDMGame->startup);
        snprintf(gAutoLaunchDeviceData->bdmPrefix, sizeof(gAutoLaunchDeviceData->bdmPrefix), "mass0:%s/", gBDMPrefix);
    } else {
        snprintf(path, sizeof(path), "mass0:CFG/%s.cfg", gAutoLaunchBDMGame->startup);
        snprintf(gAutoLaunchDeviceData->bdmPrefix, sizeof(gAutoLaunchDeviceData->bdmPrefix), "mass0:");
    }

    configSet = configAlloc(0, NULL, path);
    configRead(configSet);

    bdmLaunchGame(NULL, -1, configSet);
}
