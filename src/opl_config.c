#include "include/opl.h"
#include "include/opl_config.h"
#include "include/ioman.h"
#include "include/gui.h"
#include "include/util.h"
#include "include/supportbase.h"
#include "include/bdmsupport.h"
#include "include/ethsupport.h"
#include "include/hddsupport.h"
#include "include/appsupport.h"
#include "include/system.h"
#include "include/debug.h"
#include "include/sound.h"
#include "include/themes.h"
#include "include/lang.h"
#include "include/renderman.h"
#include "include/extern_irx.h"
#include "include/log.h"

// Global data definitions that were in opl.c but are primarily config/settings
// We move definitions here. opl.h has extern declarations.

char *gBaseMCDir;
int ps2_ip_use_dhcp;
int ps2_ip[4];
int ps2_netmask[4];
int ps2_gateway[4];
int ps2_dns[4];
int gETHOpMode;
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
int gEnableILK;
int gEnableMX4SIO;
int gEnableBdmHDD;
int gAutosort;
int gAutoRefresh;
int gEnableNotifications;
int gEnableArt;
int gWideScreen;
int gVMode;
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

// Static helpers
static int lscstatus = CONFIG_ALL;
static int lscret = 0;

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

void setDefaults(void)
{
    // Need to clear IO module T logic?
    // list_support is in opl.c.
    // We can't clear it here without including it or exposing it.
    // The loop in opl.c:setDefaults() called clearIOModuleT.
    // Ideally setDefaults handles settings, not module state.
    // But OPL's setDefaults did both.
    // I will leave the module state clearing in opl.c (maybe rename it to resetState) and call setDefaults from there?
    // No, setDefaults is called from main->init.

    // For variables:
    gAutoLaunchGame = NULL;
    gAutoLaunchBDMGame = NULL;
    gAutoLaunchDeviceData = NULL;
    gOPLPart[0] = '\0';
    gHDDPrefix = "pfs0:";
    gBaseMCDir = "mc?:OPL";

    bdmCacheSize = 16;
    hddCacheSize = 8;
    smbCacheSize = 16;

    ps2_ip_use_dhcp = 1;
    gETHOpMode = ETH_OP_MODE_AUTO;
    gPCShareAddressIsNetBIOS = 1;
    gPCShareNBAddress[0] = '\0';
    ps2_ip[0] = 192;
    ps2_ip[1] = 168;
    ps2_ip[2] = 0;
    ps2_ip[3] = 10;
    ps2_netmask[0] = 255;
    ps2_netmask[1] = 255;
    ps2_netmask[2] = 255;
    ps2_netmask[3] = 0;
    ps2_gateway[0] = 192;
    ps2_gateway[1] = 168;
    ps2_gateway[2] = 0;
    ps2_gateway[3] = 1;
    pc_ip[0] = 192;
    pc_ip[1] = 168;
    pc_ip[2] = 0;
    pc_ip[3] = 2;
    ps2_dns[0] = 192;
    ps2_dns[1] = 168;
    ps2_dns[2] = 0;
    ps2_dns[3] = 1;
    gPCPort = 445;
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
    gPS2Logo = 0;
    gHDDGameListCache = 0;
    gEnableWrite = 0;
    gRememberLastPlayed = 0;
    gAutoStartLastPlayed = 9;
    gSelectButton = KEY_CIRCLE; // Default to Japan.
    gBDMPrefix[0] = '\0';
    gETHPrefix[0] = '\0';
    gEnableNotifications = 0;
    gEnableArt = 0;
    gWideScreen = 0;
    gEnableSFX = 0;
    gEnableBootSND = 0;
    gEnableBGM = 0;
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

    gEnableILK = 0;
    gEnableMX4SIO = 0;
    gEnableBdmHDD = 0;

    // frameCounter = 0; // frameCounter is static in opl.c.
    // It's UI state, not config.

    gVMode = 0;
    gXOff = 0;
    gYOff = 0;
    gOverscan = 0;

    setDefaultColors();

    // Last Played Auto Start
    KeyPressedOnce = 0;
    DisableCron = 1;
    CronStart = 0;
    RemainSecs = 0;
}

int checkLoadConfigBDM(int types)
{
    char path[64];
    int value;

    // check USB
    if (bdmFindPartition(path, "conf_opl.cfg", 0)) {
        configEnd();
        configInit(path);
        value = configReadMulti(types);
        config_set_t *configOPL = configGetByType(CONFIG_OPL);
        configSetInt(configOPL, CONFIG_OPL_BDM_MODE, START_MODE_AUTO);
        return value;
    }

    return 0;
}

int checkLoadConfigHDD(int types)
{
    int value;
    char path[64];

    hddLoadModules();
    hddLoadSupportModules();

    snprintf(path, sizeof(path), "%sconf_opl.cfg", gHDDPrefix);
    value = open(path, O_RDONLY);
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

void moduleUpdateMenuInternal(opl_io_module_t *mod, int themeChanged, int langChanged);

// We need to declare this as external if we use it here, but it's in opl.c?
// moduleUpdateMenuInternal is in opl.c.
// But we use it in applyConfig.
// So we need to include opl.h which should have it?
// Checked opl.c content from memory: "void moduleUpdateMenuInternal(opl_io_module_t *mod, int themeChanged, int langChanged);" was declared static?
// No, it was forward declared.
// Wait, "void moduleUpdateMenuInternal(opl_io_module_t *mod, int themeChanged, int langChanged)" definition was in opl.c.
// And it was not static.
// So we can use it. But we need a prototype.
// I will assume it is not in opl.h (based on list of includes I recall).
// I will duplicate the prototype here or add it to opl.h.
// Ideally add to opl.h. But I am in this step creating opl_config.c.
// I will add prototype here for safety.

// Also need list_support.
extern opl_io_module_t list_support[MODE_COUNT];

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
                LOG("--- Triangle + Cross held at boot - setting Video Mode to Auto ---\n");
                gVMode = 0;
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
            configGetInt(configOPL, CONFIG_OPL_ENABLE_ILINK, &gEnableILK);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_MX4SIO, &gEnableMX4SIO);
            configGetInt(configOPL, CONFIG_OPL_ENABLE_BDMHDD, &gEnableBdmHDD);
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

    applyConfig(themeID, langID, 0);

    lscret = result;
    lscstatus = 0;
    showCfgPopup = 1;
}

static int trySaveConfigBDM(int types)
{
    char path[64];

    // check USB
    if (bdmFindPartition(path, "conf_opl.cfg", 1)) {
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
        configSetInt(configOPL, CONFIG_OPL_ENABLE_ILINK, gEnableILK);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_MX4SIO, gEnableMX4SIO);
        configSetInt(configOPL, CONFIG_OPL_ENABLE_BDMHDD, gEnableBdmHDD);
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

    guiHandleDeferedIO(&lscstatus, _l(_STR_LOADING_SETTINGS), IO_CUSTOM_SIMPLEACTION, &_loadConfig);

    return lscret;
}

int saveConfig(int types, int showUI)
{
    char notification[128];
    lscstatus = types;
    lscret = 0;

    guiHandleDeferedIO(&lscstatus, _l(_STR_SAVING_SETTINGS), IO_CUSTOM_SIMPLEACTION, &_saveConfig);

    if (showUI) {
        if (lscret) {
            char *path = configGetDir();

            snprintf(notification, sizeof(notification), _l(_STR_SETTINGS_SAVED), path);

            guiMsgBox(notification, 0, NULL);
        } else
            guiMsgBox(_l(_STR_ERROR_SAVING_SETTINGS), 0, NULL);
    }

    return lscret;
}
