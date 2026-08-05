#include "include/opl.h"
#include "include/lang.h"
#include "include/gui.h"
#include "include/supportbase.h"
#include "include/bdmsupport.h"
#include "include/vcdsupport.h"
#include "include/folderbrowse.h"
#include "include/util.h"
#include "include/themes.h"
#include "include/textures.h"
#include "include/ioman.h"
#include "include/system.h"
#include "include/ethsupport.h"   // ethGetModulesLoaded() for the UDPBD<->SMB NIC interlock
#include "include/udpfssupport.h" // udpfsGetModulesLoaded() -- symmetric backstop vs the udpfs_ioman filesystem stack
#include "include/mmcesupport.h"  // mmceSendGameID() cross-device game-id (#261)
#include "include/extern_irx.h"
#include "include/cheatman.h"
#include "include/sound.h"
#include "modules/iopcore/common/cdvd_config.h"

#include <usbhdfsd-common.h>

#include <ps2sdkapi.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h> // fileXioIoctl, fileXioDevctl
#include <delaythread.h>
#include <sys/stat.h> // mkdir() -- create the config folder before a BDM save probe

static int iUSBModLoaded = 0;
static int iLinkModLoaded = 0;
static int iLinkManModLoaded = 0;
static int ieee1394ModLoaded = 0;
static int mx4sioModLoaded = 0;
static int hddModLoaded = 0;
static int udpbdModLoaded = 0;
/*
  WHICH network block transport is actually resident, as NET_BOOT_UDPBD / NET_BOOT_UDPFS, or -1
  when none is loaded.

  udpbdModLoaded alone is protocol-agnostic: both branches of the loader set it to 1, so once
  either chain is up the module gate short-circuits. But the tab label, the tab icon and the
  Neutrino -bsd backend all read the LIVE gNetBootProtocol. Change the picker without rebooting and
  those three start describing a transport that is NOT the one loaded -- e.g. a tab labelled UDPFSBD
  and a "-bsd=udpfsbd" launch arg while the smap_udpbd monolith is what is actually resident.
  (A restart notice is shown on protocol change, but nothing enforces it.)

  Consumers below use this instead, so they always describe what is really loaded.
*/
static int udpbdLoadedProtocol = -1;
static s32 bdmLoadModuleLock;
int bdmDeviceModeStarted;

static item_list_t bdmDeviceList[MAX_BDM_DEVICES];
static int bdmDeviceListInitialized = 0;

void bdmInitDevicesData();
int bdmUpdateDeviceData(item_list_t *itemList);

static unsigned int BdmGeneration = 0;

static int bdmDriverIsUSB(const char *driverName)
{
    return driverName != NULL && strcmp(driverName, "usb") == 0;
}

static int bdmDriverIsIlink(const char *driverName)
{
    return driverName != NULL && (strcmp(driverName, "sd") == 0 || strcmp(driverName, "ilink") == 0);
}

static int bdmDriverIsMx4sio(const char *driverName)
{
    return driverName != NULL && (strcmp(driverName, "sdc") == 0 || strcmp(driverName, "mx4sio") == 0);
}

static int bdmDriverIsATA(const char *driverName)
{
    return driverName != NULL && strcmp(driverName, "ata") == 0;
}

static int bdmDriverIsUDPBD(const char *driverName)
{
    return driverName != NULL && strcmp(driverName, "udp") == 0;
}

static int bdmDetermineDeviceType(const char *driverName)
{
    if (bdmDriverIsUSB(driverName))
        return BDM_TYPE_USB;
    if (bdmDriverIsIlink(driverName))
        return BDM_TYPE_ILINK;
    if (bdmDriverIsMx4sio(driverName))
        return BDM_TYPE_SDC;
    if (bdmDriverIsATA(driverName))
        return BDM_TYPE_ATA;
    if (bdmDriverIsUDPBD(driverName))
        return BDM_TYPE_UDPBD;

    return BDM_TYPE_UNKNOWN;
}

// Parse the leading "<device><unit>:" token of a boot path into its digit-stripped stem ("ata0" ->
// "ata", "mass1" -> "mass") and unit number (-1 when the token carries no digits, e.g. uLE's "mass:").
// Anchored to the FIRST ':' so it can never match a folder name deeper in the path. Returns 1 on a
// well-formed device token, 0 otherwise. Used by bdmResolveBootDir to classify boot directories.
static int bdmParseBootStem(const char *bootPath, char *stem, size_t stemSize, int *unit)
{
    if (bootPath == NULL)
        return 0;

    const char *colon = strchr(bootPath, ':');
    if (colon == NULL || colon == bootPath)
        return 0; // no "<device>:" segment

    size_t n = (size_t)(colon - bootPath);
    if (n >= stemSize)
        return 0;
    memcpy(stem, bootPath, n);
    stem[n] = '\0';

    *unit = -1;
    size_t digits = 0;
    while (n > 0 && stem[n - 1] >= '0' && stem[n - 1] <= '9') { // "ata0" -> "ata", "mx4sio0" -> "mx4sio"
        stem[--n] = '\0';
        digits++;
    }
    // Bound the unit parse: no real device token carries more than 2 digits, and an unbounded
    // accumulate over launcher-controlled argv[0] bytes would run signed arithmetic into UB on a
    // mangled path ("mass4294967296:"). Longer runs keep the stem valid but leave unit = -1
    // ("unspecified"), which the resolver already treats as slot 0 with verification.
    if (digits > 0 && digits <= 2) {
        *unit = 0;
        for (const char *d = bootPath + n; *d != ':'; d++)
            *unit = *unit * 10 + (*d - '0');
    }

    return n > 0; // require a non-empty alpha stem ("0:" is not a device token)
}

static void bdmSetLaunchDeviceBinding(struct cdvdman_settings_bdm *settings, const char *driverName, int deviceIndex)
{
    settings->bdDeviceId = deviceIndex;
    // Match the IOP-side block device by its exported driver token, not the EE typed mount alias.
    if (driverName != NULL && driverName[0] != '\0')
        snprintf(settings->bdDeviceDriver, sizeof(settings->bdDeviceDriver), "%s", driverName);
    else
        settings->bdDeviceDriver[0] = '\0';
}

static void bdmBuildGamePrefix(char *target, int targetLength, const char *deviceRoot)
{
    if (gBDMPrefix[0] != '\0')
        snprintf(target, targetLength, "%s%s/", deviceRoot, gBDMPrefix);
    else
        snprintf(target, targetLength, "%s", deviceRoot);
}

// The VCD (PS1/POPSTARTER) view is anchored to the DEVICE ROOT ("mass<N>:/"), never to the gBDMPrefix
// library folder: POPSTARTER's own BDMA driver always reads <root>/POPS/<name>.VCD, so a prefixed scan
// ("mass0:<prefix>/POPS") would list VCDs that POPSTARTER can never boot -- they'd all drop to OSDSYS.
// gBDMPrefix stays a PS2-game-library concept only.
static void bdmBuildVcdPrefix(char *target, int targetLength, int massSlot)
{
    snprintf(target, targetLength, "mass%d:/", massSlot);
}

static int bdmReadDeviceIdentity(const char *path, char *driverName, int driverNameLength, int *deviceIndex)
{
    int dir, result;

    if (driverNameLength > 0)
        memset(driverName, 0, driverNameLength);
    *deviceIndex = -1;

    dir = fileXioDopen(path);
    if (dir < 0)
        return dir;

    result = fileXioIoctl2(dir, USBMASS_IOCTL_GET_DRIVERNAME, NULL, 0, driverName, driverNameLength - 1);
    if (result >= 0)
        result = fileXioIoctl2(dir, USBMASS_IOCTL_GET_DEVICE_NUMBER, NULL, 0, deviceIndex, sizeof(*deviceIndex));

    fileXioDclose(dir);
    return result;
}

// Find the first mounted BDM device whose driver matches bdmType (BDM_TYPE_*) and write its massN:
// FILESYSTEM root WITH a trailing slash (e.g. "mass0:/") to root. Returns 1 if such a device is mounted,
// 0 otherwise. Used by bdmEnsureSourceModules as a mount-readiness check for a transport type.
// The root is ALWAYS the legacy mass<slot>: mount, NEVER a typed root (ata0:/usb0:/mx4sio0:/ilink0:):
// on the pinned SDK those are launch-binding identities with no filesystem behind them, and on newer
// SDKs (ps2sdk 2026-02-26 "typed devices": bdmfs_fatfs registers them as real iomanX filesystems) a
// typed root here would leak ata0:-rooted paths into consumers that must stay on massN: -- POPSTARTER,
// the BDMA equip, the boot-dir resolver and the Neutrino pickers all read files through massN: only.
// Launch binding keeps using the raw driver token via bdmSetLaunchDeviceBinding, which is unaffected.
// The BDMA equip itself uses bdmGetDeviceSlotsByType to search EVERY same-type slot, not just the first.
int bdmGetDeviceRootByType(int bdmType, char *root, int rootLen)
{
    if (root == NULL || rootLen <= 0)
        return 0;

    for (int i = 0; i < MAX_BDM_DEVICES; i++) {
        char path[16], driver[32];
        int devIndex = -1;

        snprintf(path, sizeof(path), "mass%d:/", i);
        // Identify the device by its DRIVER NAME only. Do NOT gate on the full bdmReadDeviceIdentity
        // return: GET_DEVICE_NUMBER can fail on a perfectly usable device (seen on some USB drives), and
        // the device pages already tolerate that (they keep the device by driver name, bdmsupport.c
        // ~1219). Requiring it here made the equip report "no device matching" for a connected source.
        bdmReadDeviceIdentity(path, driver, sizeof(driver), &devIndex);
        if (driver[0] == '\0')
            continue;
        if (bdmDetermineDeviceType(driver) != bdmType)
            continue;

        snprintf(root, rootLen, "mass%d:/", i);
        return 1;
    }

    return 0;
}

// Fill `slots` with the massN: slot index of EVERY mounted device whose driver matches bdmType, up to
// maxSlots; returns the count. The filesystem root for slot i is "mass<i>:/" (OPL never mounts a typed
// ata0:/usb0: filesystem -- block-device identities only). The BDMA equip uses this to search a source
// family that has more than one device of the same type: the variant files may sit on the second one,
// which bdmGetDeviceRootByType (first-match) would miss.
int bdmGetDeviceSlotsByType(int bdmType, int *slots, int maxSlots)
{
    int n = 0;

    if (slots == NULL || maxSlots <= 0)
        return 0;

    for (int i = 0; i < MAX_BDM_DEVICES && n < maxSlots; i++) {
        char path[16], driver[32];
        int devIndex = -1;

        snprintf(path, sizeof(path), "mass%d:/", i);
        bdmReadDeviceIdentity(path, driver, sizeof(driver), &devIndex);
        if (driver[0] != '\0' && bdmDetermineDeviceType(driver) == bdmType)
            slots[n++] = i;
    }

    return n;
}

static int bdmLoadOptionalModule(const char *name, void *module, int moduleSize)
{
    int result;

    LOG("[%s]:\n", name);
    result = sysLoadModuleBuffer(module, moduleSize, 0, NULL);
    if (result < 0)
        LOG("%s failed to load: %d\n", name, result);

    return result;
}

// Like bdmLoadOptionalModule, but passes IOP module args (arglen = byte length of the packed,
// NUL-terminated args buffer). smap_udpbd requires an "ip=A.B.C.D" arg; the base loader passes none.
static int bdmLoadOptionalModuleArgs(const char *name, void *module, int moduleSize, int arglen, char *args)
{
    int result;

    LOG("[%s]:\n", name);
    result = sysLoadModuleBuffer(module, moduleSize, arglen, args);
    if (result < 0)
        LOG("%s failed to load: %d\n", name, result);

    return result;
}

// Exposed for ethsupport's NIC mutual-exclusion: UDPBD and the SMB stack both own the SMAP adapter.
int bdmIsUDPBDLoaded(void)
{
    return udpbdModLoaded;
}

/*
  The network block transport actually resident (NET_BOOT_UDPBD / NET_BOOT_UDPFS). Falls back to the
  live gNetBootProtocol before anything has loaded, so first-boot labelling is unchanged.
*/
int bdmGetLoadedNetProtocol(void)
{
    return (udpbdLoadedProtocol >= 0) ? udpbdLoadedProtocol : gNetBootProtocol;
}

// True when this support's device is the UDPBD block device (its games are Neutrino-only).
// Returns 0 for non-BDM supports (incl. FAV-wrapped items, which have no source lookup here).
int bdmSupportIsUDPBD(item_list_t *support)
{
    if (support == NULL || support->priv == NULL)
        return 0;
    if (support->mode < BDM_MODE || support->mode > BDM_MODE_LAST)
        return 0;
    return ((bdm_device_data_t *)support->priv)->bdmDeviceType == BDM_TYPE_UDPBD;
}

static void bdmEventHandler(void *packet, void *opt)
{
    BdmGeneration++;
}

// Bump the device generation the per-device refresh latch keys off, exactly as a real hotplug event
// (bdmEventHandler) does. This invalidates every device's bdmDeviceTick so the next bdmNeedsUpdate
// pass re-evaluates presence + page visibility instead of short-circuiting on the latch. Called when
// the user applies Device Settings, so a BDM page that was force-hidden while its enable flag read 0
// (e.g. an exFAT ATA tab, or a UDPBD tab) re-shows as soon as the device is re-enabled and present --
// without needing a physical replug.
void bdmForceDeviceRefresh(void)
{
    BdmGeneration++;
}

// Read-only accessor so the menu hook can detect a real BDM device change (hotplug via bdmEventHandler
// or a Device-Settings apply) and bypass its background-rescan throttle for immediate detection.
// Plain single-word read, matching the existing non-atomic BdmGeneration read at bdmNeedsUpdate.
unsigned int bdmGetGeneration(void)
{
    return BdmGeneration;
}

static int bdmShouldQueueModuleLoad(void)
{
    if (gEnableUSB && !iUSBModLoaded)
        return 1;
    if (gEnableILK && !iLinkModLoaded)
        return 1;
    if (gEnableMX4SIO && !mx4sioModLoaded)
        return 1;
    if (gEnableBdmHDD && !hddModLoaded)
        return 1;
    if (gEnableUDPBD && !udpbdModLoaded && !ethGetModulesLoaded() && !udpfsGetModulesLoaded())
        return 1; // mirror the load gate -- if the SMB or udpfs-filesystem NIC is up, the block chain can't load

    return 0;
}

static void bdmLoadBlockDeviceModules(void)
{
    // Boot-step localizer: this runs on the IO thread and can wedge on a real drive with no timeout,
    // freezing the splash. Publish the step so a hang here names it instead of the main thread's "Ready.".
    // (brenotomaz exFAT-USB boot hang investigation.)
    guiSetBootStatusSticky(_l(_STR_BOOT_LOADING_DRIVERS));

    WaitSema(bdmLoadModuleLock);

    if (gEnableUSB && !iUSBModLoaded) {
        // Load USB Block Device drivers -- the prime suspect for a real exFAT/USB boot wedge.
        guiSetBootStatusSticky(_l(_STR_BOOT_LOADING_USB));
        if (bdmLoadOptionalModule("USBMASS_BD", &usbmass_bd_irx, size_usbmass_bd_irx) >= 0)
            iUSBModLoaded = 1;
    }

    if (gEnableILK && !iLinkModLoaded) {
        // Load iLink Block Device drivers
        if (!iLinkManModLoaded && bdmLoadOptionalModule("ILINKMAN", &iLinkman_irx, size_iLinkman_irx) >= 0)
            iLinkManModLoaded = 1;
        if (iLinkManModLoaded && !ieee1394ModLoaded && bdmLoadOptionalModule("IEEE1394_BD", &IEEE1394_bd_irx, size_IEEE1394_bd_irx) >= 0)
            ieee1394ModLoaded = 1;

        iLinkModLoaded = iLinkManModLoaded && ieee1394ModLoaded;
    }

    if (gEnableMX4SIO && !mx4sioModLoaded) {
        // Load MX4SIO Block Device drivers
        if (bdmLoadOptionalModule("MX4SIO_BD", &mx4sio_bd_irx, size_mx4sio_bd_irx) >= 0)
            mx4sioModLoaded = 1;
    }

    if (gEnableBdmHDD && !hddModLoaded) {
        // Load dev9 and atad device drivers.
        LOG("bdmLoadBlockDeviceModules loading hdd drivers...\n");
        // Only mark loaded on success -- a failure (no HDD/interface) must not
        // suppress future retries via bdmShouldQueueModuleLoad() (matches the
        // conditional pattern used for USB/MX4SIO and opl.c's hddLoadModules gate).
        if (hddLoadModulesReady())
            hddModLoaded = 1;
    }

    // Network block device (UDPBD or UDPFS, picked by gNetBootProtocol). NIC-exclusive with the SMB/ETH
    // stack (smap registers "SMAP_driver"), so only load when SMB isn't up. dev9 is refcounted (shared
    // with ATA-HDD). Both need the PS2's static IP as an "ip=" arg -- the ministack has no DHCP client.
    if (gEnableUDPBD && !udpbdModLoaded && !ethGetModulesLoaded() && !udpfsGetModulesLoaded()) {
        char ipArg[24];
        sysInitDev9();
        snprintf(ipArg, sizeof(ipArg), "ip=%d.%d.%d.%d", ps2_ip[0], ps2_ip[1], ps2_ip[2], ps2_ip[3]);
        if (gNetBootProtocol == NET_BOOT_UDPFS) {
            // UDPFS: a 3-IRX chain loaded in dependency order -- smap (exports to ministack + bd), then
            // ministack (gets the ip= arg, exports to bd), then udpfs_bd (registers the "udp" BDM device).
            if (bdmLoadOptionalModule("UDPFS_SMAP", &udpfs_smap_irx, size_udpfs_smap_irx) >= 0 &&
                bdmLoadOptionalModuleArgs("UDPFS_MINISTACK", &udpfs_ministack_irx, size_udpfs_ministack_irx, (int)strlen(ipArg) + 1, ipArg) >= 0 &&
                bdmLoadOptionalModule("UDPFS_BD", &udpfs_bd_irx, size_udpfs_bd_irx) >= 0) {
                udpbdModLoaded = 1;
                udpbdLoadedProtocol = NET_BOOT_UDPFS;
            }
        } else {
            // UDPBD: the self-contained smap_udpbd monolith (smap + ministack + udpbd in one irx).
            if (bdmLoadOptionalModuleArgs("SMAP_UDPBD", &smap_udpbd_irx, size_smap_udpbd_irx, (int)strlen(ipArg) + 1, ipArg) >= 0) {
                udpbdModLoaded = 1;
                udpbdLoadedProtocol = NET_BOOT_UDPBD;
            }
        }
        // Release the dev9 reference taken above if the load failed -- otherwise a failing/retrying
        // UDPBD/UDPFS (both gates re-enter on every device refresh while !udpbdModLoaded) inflates the
        // refcounted dev9InitCount and a later HDD/ETH teardown can never power dev9 down. On success
        // the reference is intentionally kept (the device stays mounted). Mirrors ETH/HDD pairing.
        if (!udpbdModLoaded)
            sysShutdownDev9();
    }

    SignalSema(bdmLoadModuleLock);
}

void bdmLoadModules(void)
{
    LOG("BDMSUPPORT LoadModules\n");

    // Load Block Device Manager (BDM)
    LOG("[BDM]:\n");
    sysLoadModuleBuffer(&bdm_irx, size_bdm_irx, 0, NULL);

    // Load FATFS (mass:) driver
    LOG("[BDMFS_FATFS]:\n");
    sysLoadModuleBuffer(&bdmfs_fatfs_irx, size_bdmfs_fatfs_irx, 0, NULL);

    // Load Optional Block Device drivers
    ioPutRequest(IO_CUSTOM_SIMPLEACTION, &bdmLoadBlockDeviceModules);

    LOG("[BDMEVENT]:\n");
    sysLoadModuleBuffer(&bdmevent_irx, size_bdmevent_irx, 0, NULL);
    SifAddCmdHandler(0, &bdmEventHandler, NULL);

    LOG("BDMSUPPORT Modules loaded\n");
}

static void bdmInit(item_list_t *itemList)
{
    LOG("BDMSUPPORT Init\n");

    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    pDeviceData->bdmULSizePrev = -2;
    pDeviceData->bdmModifiedCDPrev = 0;
    pDeviceData->bdmModifiedDVDPrev = 0;
    pDeviceData->bdmGameCount = 0;
    pDeviceData->bdmGames = NULL;
    pDeviceData->bdmVcdGameCount = 0; // #120: separate VCD store (see bdm_device_data_t)
    pDeviceData->bdmVcdGames = NULL;
    pDeviceData->FoldersCreated = 0;
    pDeviceData->bdmDeviceType = BDM_TYPE_UNKNOWN;
    pDeviceData->bdmHddIsLBA48 = -1;
    pDeviceData->ataHighestUDMAMode = -1;
    // NAV-PARITY TEST BUILD (#340): official's fixed 8-frame art settle, not the fork Art Delay setting.
    itemList->delay = MENU_MIN_INACTIVE_FRAMES;
    bdmLoadModules();
    itemList->enabled = 1;
}

// Effective BDM device start mode. The UDPBD tab is not a mode-level tab like SMB -- it is a
// hotplug-published BDM mass page, which needs the BDM pages initialized, polled, and the bdm core
// modules loaded before smap_udpbd can even LINK. The unified network-protocol picker couples SMB to
// gETHStartMode and UDPFS to its own derived start mode, but selecting UDPBD/UDPFSBD wrote NOTHING to
// gBDMStartMode -- with the shipped Manual default (or Off), the UDPBD tab could NEVER appear unless
// the user happened to enter the generic BDM placeholder tab first ("UDPBD tab never shows", 2026-07-13).
// Floor the EFFECTIVE mode at AUTO while a BDM network transport is the selected protocol, mirroring
// the UDPFS_MODE derivation; the user's saved gBDMStartMode is never modified or persisted.
int bdmEffectiveStartMode(void)
{
    if (gEnableUDPBD && gBDMStartMode != START_MODE_AUTO)
        return START_MODE_AUTO;
    return gBDMStartMode;
}

// Per-transport enable flag for a classified BDM device type.
// Returns 1 = enabled, 0 = disabled, -1 = unclassifiable (generic/unknown driver: never force-hidden).
static int bdmTransportEnabled(int bdmDeviceType)
{
    switch (bdmDeviceType) {
        case BDM_TYPE_USB:
            return gEnableUSB ? 1 : 0;
        case BDM_TYPE_ILINK:
            return gEnableILK ? 1 : 0;
        case BDM_TYPE_SDC:
            return gEnableMX4SIO ? 1 : 0;
        case BDM_TYPE_ATA:
            return gEnableBdmHDD ? 1 : 0;
        case BDM_TYPE_UDPBD:
            return gEnableUDPBD ? 1 : 0;
        default:
            return -1;
    }
}

static int bdmNeedsUpdate(item_list_t *itemList)
{
    char path[256];
    int result = 0;
    struct stat st;

    // If we made it here then BDM device mode has been started.
    bdmDeviceModeStarted = 1;

    // If bdm mode is disabled bail out as we don't want to update the visibility state of the device pages.
    if (bdmEffectiveStartMode() == START_MODE_DISABLED)
        return 0;

    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    // Check for forced refresh from deleting or renaming a game.
    if (pDeviceData->ForceRefresh != 0) {
        pDeviceData->ForceRefresh = 0;
        return 1;
    }

    // If the device menu is visible double check the device type and if support for this device type is enabled. If the user switches device support
    // to off for a bdm device we want to hide the menu even though the drivers are still loaded and the device is being detected by bdm.
    opl_io_module_t *pOwner = (opl_io_module_t *)itemList->owner;
    if (pOwner != NULL && pOwner->menuItem.visible == 1) {
        // If the device page is visible but the device support is not enabled, hide the device page.
        if (bdmTransportEnabled(pDeviceData->bdmDeviceType) == 0)
            pOwner->menuItem.visible = 0;
    }

    // VCD view: an L3 toggle marks the mode dirty but does NOT bump BdmGeneration or reset
    // bdmULSizePrev, so consume the dirty flag BEFORE the device-tick cache gate below (mirrors how
    // mmceNeedsUpdate checks it first). Otherwise a device already scanned this BdmGeneration -- which
    // every USB stick is, the moment its ISO list loads -- short-circuits at the gate and never
    // rescans, leaving the VCD (PS1/POPSTARTER) list permanently empty on USB/BDM while MMCE worked.
    if (vcdConsumeDirty(itemList->mode))
        return 1;

    // Folder browsing: a descend/ascend marks the mode dirty but bumps nothing the tick gate below
    // watches, so consume it here (same discipline as vcdConsumeDirty) or the deeper/shallower list
    // never rebuilds on a device already scanned this BdmGeneration.
    if (folderConsumeDirty(itemList->mode))
        return 1;

    // NEVER-SCANNED (-2) DEFEATS EVERY SHORT-CIRCUIT IN HERE -- note the condition, not just the body.
    // bdmULSizePrev starts at -2 and only leaves it once a scan has actually run, so while it is -2
    // this device has published no list yet and must be let through, INCLUDING while the page is still
    // invisible -- exactly the state a device sits in between attaching and being shown. (The invisible
    // bail below used to run first, making the old `!= -2` test on the miss-count line unreachable for
    // a never-visible page; that test is now redundant and gone.)
    //
    // Why this reaches far past the invisible case: the BDM list scans essentially ONCE, on the publish
    // pass. Every later refresh hits bdmUpdateDeviceData's "no change to the device state" return 0
    // below, so whatever the list held at that single instant is what the user has for the rest of the
    // boot -- and sbReadList preserves its last-good list on a failed read, which on a FIRST scan
    // means an empty list with no error shown. A network block device (UDPBD) is
    // precisely where that instant can be too early: the volume is mounted but the server round-trip
    // behind the CD/DVD opendir can still fail, and the page then reads "0 games" permanently and
    // silently. UDPFS already rescues itself with the identical idea (udpfssupport.c:134-135,
    // `if (udpfsULSizePrev == -2) result = 1;` -- deliberately above every gate); BDM had no
    // equivalent. Same class as the PR #151 tab bug, one layer down.
    if (pDeviceData->bdmDeviceTick == BdmGeneration && pDeviceData->bdmULSizePrev != -2) {
        if (pOwner != NULL && pOwner->menuItem.visible == 0)
            return 0;
        // While a network page is inside its disconnect grace window (bdmMissCount > 0, set by the
        // presence-poll debounce in bdmUpdateDeviceData), keep re-polling each refresh so the debounce
        // advances to the hide threshold -- otherwise it would stall at the single disconnect event and
        // a truly-gone network page would never hide. Local devices always have bdmMissCount == 0 here.
        if (pDeviceData->bdmMissCount == 0)
            return 0;
    }
    pDeviceData->bdmDeviceTick = BdmGeneration;

    if (bdmShouldQueueModuleLoad())
        ioPutRequest(IO_CUSTOM_SIMPLEACTION, &bdmLoadBlockDeviceModules);

    // Check if the device has been connected or removed.
    result = bdmUpdateDeviceData(itemList);
    // "No change to the device state" normally means there is nothing to do -- but a device that has
    // NEVER scanned (bdmULSizePrev == -2) has no list to leave alone, and bdmUpdateDeviceData reports
    // 0 on every poll once the device is connected and its identity is populated. Returning here would
    // make the -2 bypass at the device-tick gate above a NO-OP, which is exactly what it was before
    // this line changed (Gemini review of #186 -- it caught that my first cut fixed nothing).
    //
    // Falling through is safe and needs no new scan logic: result stays 0 past the connect/disconnect
    // sfx branches, and the sbIsSameSize(bdmPrefix, bdmULSizePrev) check further down cannot match a
    // sentinel of -2, so it sets result = 1 and the first scan publishes through the existing path.
    // ... but ONLY for a slot that has actually CONNECTED (bdmPrefix populated by the connect pass).
    // A NEVER-connected slot also sits at the -2 sentinel with result == 0, and letting it fall
    // through reached sbCreateFolders() with an EMPTY prefix -- mkdir("CFG")/mkdir("THM")/... resolve
    // relative to the CWD, i.e. the BOOT FOLDER, so an enabled-but-absent BDM device recreated the
    // whole OPL library tree inside mmce0:/RIPTOPL/ (or wherever OPL lives) on every startup
    // (AndrewBento, #214; FifthFox reported the same class). The #186 first-scan rescue only ever
    // needed MOUNTED devices, which always have bdmPrefix set before any 0-result poll.
    if (result == 0) {
        // The -2 rescue applies ONLY to a device that is CONNECTED **and PUBLISHED**. Two #214-class
        // holes otherwise: a NEVER-connected slot (bdmPrefix empty) reached sbCreateFolders("") --
        // bare mkdir("CFG")/mkdir("THM")/... resolve against the CWD, i.e. the BOOT FOLDER
        // (AndrewBento, #214); and a PRESENT but transport-WITHHELD device would get folders + scans
        // on a page the user disabled, because bdmUpdateDeviceData populates bdmPrefix BEFORE its
        // explicit-BDM-off/transport-disabled 0-returns (CodeRabbit review of #239 -- vetted against
        // bdmUpdateDeviceData: the withhold paths run after bdmBuildGamePrefix). The #186 first-scan
        // rescue only ever needed MOUNTED devices, whose connect pass sets BOTH the prefix and
        // menuItem.visible before any 0-result poll.
        int neverScanned = (pDeviceData->bdmULSizePrev == -2);
        int connectedAndPublished = pDeviceData->bdmPrefix[0] != '\0' &&
                                    itemList->owner != NULL && ((opl_io_module_t *)itemList->owner)->menuItem.visible;
        if (!(neverScanned && connectedAndPublished))
            return 0;
    }

    // If a device was added or removed play the appropriate UI sound.
    if (result == -1) {
        sfxPlay(SFX_BD_DISCONNECT);
        return result;
    } else if (result == 1)
        sfxPlay(SFX_BD_CONNECT);

    // Belt-and-suspenders: never create the library tree at an empty prefix (= the CWD/boot folder).
    if (!pDeviceData->FoldersCreated && pDeviceData->bdmPrefix[0] != '\0') {
        sbCreateFolders(pDeviceData->bdmPrefix, 1);
        pDeviceData->FoldersCreated = 1;
    }

    // VCD view: while this device shows its VCD list, skip the disc-folder heuristics (it refreshes on
    // L3 toggle / manual refresh only). The toggle's forced rescan is consumed ABOVE the device-tick
    // gate (see vcdConsumeDirty near the top) so the per-generation cache can't swallow it.
    if (vcdViewActive(itemList->mode))
        return 0;

    snprintf(path, sizeof(path), "%sCD", pDeviceData->bdmPrefix);
    if (stat(path, &st) != 0)
        st.st_mtime = 0;
    if (pDeviceData->bdmModifiedCDPrev != st.st_mtime) {
        pDeviceData->bdmModifiedCDPrev = st.st_mtime;
        result = 1;
    }

    snprintf(path, sizeof(path), "%sDVD", pDeviceData->bdmPrefix);
    if (stat(path, &st) != 0)
        st.st_mtime = 0;
    if (pDeviceData->bdmModifiedDVDPrev != st.st_mtime) {
        pDeviceData->bdmModifiedDVDPrev = st.st_mtime;
        result = 1;
    }

    if (!sbIsSameSize(pDeviceData->bdmPrefix, pDeviceData->bdmULSizePrev))
        result = 1;

    // update Themes
    if (!pDeviceData->ThemesLoaded) {
        sprintf(path, "%sTHM", pDeviceData->bdmPrefix);
        if (thmAddElements(path, "/", 1) > 0)
            pDeviceData->ThemesLoaded = 1;
    }

    // update Languages
    if (!pDeviceData->LanguagesLoaded) {
        sprintf(path, "%sLNG", pDeviceData->bdmPrefix);
        if (lngAddLanguages(path, "/", itemList->mode) > 0)
            pDeviceData->LanguagesLoaded = 1;
    }

    return result;
}

static int bdmUpdateGameList(item_list_t *itemList)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    // #120: each view scans into its OWN array (see bdm_device_data_t). Previously both shared
    // bdmGames, so a VCD scan reporting failure (r < 0) left the ISO list published under the VCD
    // view -- the L3 toggle then "never changed the list" (Nathan, HW, ATA/HDD_BD), and a device with
    // no POPS folder hit that on every single toggle.
    if (vcdViewActive(itemList->mode)) {
        char vcdPrefix[BDM_DEVICE_ROOT_MAX + 2];
        bdmBuildVcdPrefix(vcdPrefix, sizeof(vcdPrefix), itemList->mode); // device root, NOT gBDMPrefix
        int r = vcdFillGameList(vcdPrefix, &pDeviceData->bdmVcdGames);
        if (r >= 0) // r < 0: transient scan failure -> keep THIS view's last-good (empty if never scanned)
            pDeviceData->bdmVcdGameCount = r;
        return pDeviceData->bdmVcdGameCount;
    }
    sbReadList(&pDeviceData->bdmGames, pDeviceData->bdmPrefix, folderGetSub(itemList->mode), &pDeviceData->bdmULSizePrev, &pDeviceData->bdmGameCount);
    return pDeviceData->bdmGameCount;
}

static int bdmGetGameCount(item_list_t *itemList)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    return vcdViewActive(itemList->mode) ? pDeviceData->bdmVcdGameCount : pDeviceData->bdmGameCount;
}

// Toggle-window guard (mirrors mmceActiveGame). The L3 toggle flips vcdViewActive() SYNCHRONOUSLY
// (vcdToggleView) but rebuilds the submenu on the DEFERRED IO thread, so for a window the OLD submenu's
// ids are still live while vcdViewActive() already reports the NEW view -- and an id from the old view
// can index the freshly-switched other-view array, which may be NULL (never scanned), shorter, or
// mid-scan. Resolve EVERY id through here: an out-of-range id yields a STATIC EMPTY entry, so a stale-id
// READ is safe empty data instead of an OOB deref, and launch/delete/rename treat &bdmEmptyGame as
// "nothing there". (The shared store this replaced could not OOB: one array + one count stayed
// self-consistent -- splitting them is what makes this guard mandatory.)
static base_game_info_t bdmEmptyGame = {0};
static base_game_info_t *bdmActiveGame(item_list_t *itemList, int id)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    int vcd = vcdViewActive(itemList->mode);
    base_game_info_t *arr = vcd ? pDeviceData->bdmVcdGames : pDeviceData->bdmGames;
    int count = vcd ? pDeviceData->bdmVcdGameCount : pDeviceData->bdmGameCount;

    if (arr == NULL || id < 0 || id >= count)
        return &bdmEmptyGame;
    return &arr[id];
}

static void *bdmGetGame(item_list_t *itemList, int id)
{
    return (void *)bdmActiveGame(itemList, id);
}

static char *bdmGetGameName(item_list_t *itemList, int id)
{
    return bdmActiveGame(itemList, id)->name;
}

static int bdmGetGameNameLength(item_list_t *itemList, int id)
{
    base_game_info_t *g = bdmActiveGame(itemList, id);

    return ((g->format != GAME_FORMAT_USBLD) ? ISO_GAME_NAME_MAX + 1 : UL_GAME_NAME_MAX + 1);
}

static char *bdmGetGameStartup(item_list_t *itemList, int id)
{
    base_game_info_t *g = bdmActiveGame(itemList, id);

    // VCD view: identity is the filename, not a disc ID -> per-game data (CFG/art) keys off the
    // VCD name (matches sbPopulateConfig).
    if (vcdViewActive(itemList->mode))
        return g->name;
    return g->startup;
}

static void bdmDeleteGame(item_list_t *itemList, int id)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    if (vcdViewActive(itemList->mode))
        return; // #120: a VCD is not an ISO game -- no delete in VCD view (mirrors mmceDeleteGame)
    if (bdmActiveGame(itemList, id) == &bdmEmptyGame)
        return;                                   // stale id in the toggle window: sbDelete does NOT bounds-check -> avoid an OOB/wrong unlink
    sbSetBrowseSub(folderGetSub(itemList->mode)); // delete inside the current subfolder, not the root
    sbDelete(&pDeviceData->bdmGames, pDeviceData->bdmPrefix, "/", pDeviceData->bdmGameCount, id);
    pDeviceData->bdmULSizePrev = -2;
    pDeviceData->ForceRefresh = 1;
}

static void bdmRenameGame(item_list_t *itemList, int id, char *newName)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    if (vcdViewActive(itemList->mode))
        return; // #120: no rename in VCD view
    if (bdmActiveGame(itemList, id) == &bdmEmptyGame)
        return;                                   // stale id in the toggle window (see bdmDeleteGame) -> avoid sbRename OOB
    sbSetBrowseSub(folderGetSub(itemList->mode)); // rename inside the current subfolder, not the root
    sbRename(&pDeviceData->bdmGames, pDeviceData->bdmPrefix, "/", pDeviceData->bdmGameCount, id, newName);
    pDeviceData->bdmULSizePrev = -2;
    pDeviceData->ForceRefresh = 1;
}

// Launch a PS1/.VCD entry BY NAME via POPSTARTER (the view-independent entry point used by both the
// in-view menu launch below and the Favourites tab). Every BDM device is a massN: mount at runtime,
// so the ELF + selector both use the live prefix; UNMOUNT_EXCEPTION keeps it mounted across the reset.
static void bdmLaunchVcd(item_list_t *itemList, const char *vcdName, config_set_t *configSet)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    char vcdPrefix[64], vcdElf[256], vcdSelector[320];

    if (pDeviceData == NULL || vcdName == NULL || vcdName[0] == '\0' || !strcasecmp(vcdName, "POPSTARTER")) // reserved-name belt: the scanner no longer lists it (#154); strcasecmp -- FAT is case-insensitive
        return;
    // POPSTARTER does its own IOP reset and reloads its block drivers from the MC -- it can't bring up
    // the network (udp) block device (no BDMA variant for it), so a PS1 VCD on the udp device would just
    // drop to OSDSYS. Abort cleanly with a message instead of a dead launch.
    if (pDeviceData->bdmDeviceType == BDM_TYPE_UDPBD) {
        guiMsgBox(_l(_STR_VCD_NOT_ON_NET), 0, NULL);
        return;
    }
    bdmBuildVcdPrefix(vcdPrefix, sizeof(vcdPrefix), itemList->mode); // device root, NOT gBDMPrefix -- POPSTARTER reads <root>/POPS only
    if (!vcdResolvePopstarter(vcdPrefix, vcdElf, sizeof(vcdElf))) {
        guiMsgBox(_l(_STR_POPSTARTER_NOT_FOUND), 0, NULL);
        return;
    }
    // vcdBuildSelector emits the BARE POPSTARTER label ("mass:/POPS/XX.<name>.ELF"), NOT this live
    // unit-numbered mount -- POPSTARTER remounts under "mass:" after its own IOP reset (see the function).
    vcdBuildSelector(vcdPrefix, VCD_PREFIX_MASS, vcdName, vcdSelector, sizeof(vcdSelector));

    // Populate mc?:/POPSTARTER from this game's own direct POPS/ folder. Best effort: local VCD
    // launches keep their existing handoff behavior when optional files are absent or the card is full.
    (void)vcdInstallPopstarterMc(vcdPrefix);

    // Best-effort card prep: POPSTARTER reloads its block-device driver pair from the MC after its OWN
    // IOP reset, so try to equip the device-matching BDMAssault variant first. NEVER a launch gate --
    // the handoff below always proceeds (POPSTARTER owns everything past the exec); a failed equip just
    // toasts its diagnostic in passing. iLink/UDPBD/unknown have no BDMA variant and are left as-is.
    switch (pDeviceData->bdmDeviceType) {
        case BDM_TYPE_USB: {
            // USB is special: the PS2 cannot detect whether the stick is fat32 or exFAT formatted,
            // so the user picks the POPSTARTER USB driver on EVERY USB VCD launch (maintainer
            // directive 2026-08-01). fat32 = POPSTARTER's built-in USB stack (recommended for
            // non-exFAT users); exFAT = the BDMAssault usbexfat pair. Back cancels the launch.
            int usbMode = guiShowVcdUsbMode();
            if (usbMode == VCDUSB_BTN_FAT32)
                vcdApplyUsbModeForLaunch(VCD_BDMA_FAT32);
            else if (usbMode == VCDUSB_BTN_EXFAT)
                vcdApplyUsbModeForLaunch(VCD_BDMA_USBEXFAT);
            else
                return; // dialog cancelled -- abort the launch, nothing equipped
            break;
        }
        case BDM_TYPE_SDC:
            vcdEnsureBdmaForLaunch(VCD_BDMA_SRC_MX4SIO, VCD_BDMA_MX4SIO);
            break;
        case BDM_TYPE_ATA:
            vcdEnsureBdmaForLaunch(VCD_BDMA_SRC_HDD, VCD_BDMA_ATA);
            break;
        default:
            break;
    }

    char vcdFullPath[256];
    snprintf(vcdFullPath, sizeof(vcdFullPath), "%sPOPS/%s.VCD", vcdPrefix, vcdName);
    vcdPrepareRetroGemBarcode(vcdFullPath);

    deinit(UNMOUNT_EXCEPTION, itemList->mode); // keep the VCD device mounted across the IOP reset
    sysLaunchPopstarter(vcdElf, vcdSelector);
}

// Δ9 (NHDDL parity): Neutrino's bd backend packs the ISO and BOTH -mc VMCs into ONE shared
// 64-entry fragment table (BDM_MAX_FRAGS in neutrino's fhi_bd_config.h; fhi_config.c's
// backend_bd_add_file_fd errors past it). That packing runs AFTER neutrino's own IOP reset,
// where its only error output is a debug printf -- from the couch, a black screen. So pre-count
// the same budget here, while the menu can still explain the abort, using the count-only form
// of the same ioctl (NULL buffer: bdmfs_fatfs's get_frag_list gates only the WRITE on capacity,
// the count is unconditional -- it returns the file's TOTAL fragment count and writes nothing).
// Every device this helper can see is bdmfs_fatfs-backed (mmce/udpfs launches live in their own
// support files), including both udp block transports (udpbd + udpfsbd depend on i_bdm), so the
// ioctl is always valid here. NOT OPL's BDM_MAX_FRAGS: same value today, different owner.
#define NEUTRINO_BDM_MAX_FRAGS 64

static int bdmNeutrinoFragBudgetOk(const char *isoPath, const neutrino_vmc_args_t *vmcArgs, int isUdp)
{
    const char *paths[1 + NEUTRINO_VMC_SLOTS];
    int nPaths = 0, total = 0, i;

    paths[nPaths++] = isoPath;
    for (i = 0; i < NEUTRINO_VMC_SLOTS; i++) {
        const char *eq = vmcArgs->arg[i][0] ? strchr(vmcArgs->arg[i], '=') : NULL;
        if (eq != NULL && eq[1] != '\0')
            paths[nPaths++] = eq + 1; // "-mcN=<path>"; Δ2 already verified the file exists
    }

    for (i = 0; i < nPaths; i++) {
        int fd = open(paths[i], O_RDONLY);
        if (fd < 0) { // advisory check only: never veto the launch on a probe failure
            LOG("[NEUTRINO] frag pre-count: open(%s) failed, file skipped\n", paths[i]);
            continue;
        }
        int frags = fileXioIoctl2(ps2sdk_get_iop_fd(fd), USBMASS_IOCTL_GET_FRAGLIST, NULL, 0, NULL, 0);
        close(fd);
        if (frags < 0) {
            LOG("[NEUTRINO] frag pre-count: ioctl(%s) = %d, file skipped\n", paths[i], frags);
            continue;
        }
        total += frags;
    }

    if (total > NEUTRINO_BDM_MAX_FRAGS) { // == is fine: neutrino packs exactly-full tables
        char msg[256];                    // headroom for the lng_fork overlay's longer translations
        LOG("[NEUTRINO] frag budget exceeded: %d/%d across ISO + VMCs\n", total, NEUTRINO_BDM_MAX_FRAGS);
        // Match the Δ8 abort-message convention: the udp outcome is a consumed launch, the
        // local outcome is a native-core fallback (which usually boots: OPL's own 64-frag
        // table holds the ISO only -- VMCs go through mcemu's contiguity check instead).
        snprintf(msg, sizeof(msg), _l(isUdp ? _STR_NEUTRINO_TOO_FRAGMENTED_NET : _STR_NEUTRINO_TOO_FRAGMENTED), total, NEUTRINO_BDM_MAX_FRAGS);
        guiWarning(msg, 6);
        return 0;
    }
    return 1;
}

// Δ8 (NHDDL parity): the LEAN Neutrino launch path. Everything bdmLaunchGame's native flow
// prepares -- VMC superblock prompts + mcemu patching, sbPrepare's cdvdman patch, per-part
// fragment lists (with a hard abort past 64 frags), layer-1 probing, cheats (with dialogs),
// PS2RD images, ATA DMA setup -- exists for the EMBEDDED cdvdman core; Neutrino re-derives all
// of it from -bsd/-dvd after its own IOP reset. Paying for that work meant a Neutrino launch
// could DIE on native-only failures (the fragmented-ISO abort, an interactive cheats dialog
// mid-launch, a VMC-superblock cancel whose result the launch then ignored). NHDDL's whole
// pre-handoff is ~3 file operations; this is ours.
// Returns 1 = handled (handed off, or aborted with the user informed -- caller returns);
// 0 = proceed with the native launch (core is OPL, or Neutrino unavailable on a non-udp device).
static int bdmTryNeutrinoLaunch(item_list_t *itemList, base_game_info_t *game, bdm_device_data_t *pDeviceData, config_set_t *configSet)
{
    int coreLoader = gDefaultCoreLoader; // no per-game $CoreLoader key -> follow the global default core
    configGetInt(configSet, CONFIG_ITEM_CORE_LOADER, &coreLoader);
    // UDPBD/UDPFS have no embedded cdvdman backend -- the "udp" BDM device can ONLY boot via
    // Neutrino, so force the core and treat every Neutrino failure below as a clean abort
    // (falling through to the native path would deinit into a no-op = black screen).
    int isUdp = bdmDriverIsUDPBD(pDeviceData->bdmDriver);
    if (isUdp)
        coreLoader = 1;
    if (!coreLoader)
        return 0;

    // Abort helper contract: on udp the launch is unbootable without Neutrino -- consume the
    // launch (autolaunch mirrors its normal teardown); on local devices fall back to native.
    int failResult = isUdp ? 1 : 0;

    if (game->format == GAME_FORMAT_USBLD || !strcasecmp(game->extension, ".zso")) {
        // isValidIsoName() admits .zso case-insensitively and game->extension is stored verbatim,
        // so an upper/mixed-case ".ZSO" must reject here too (Neutrino can't run it).
        guiWarning(_l(isUdp ? _STR_NET_NEEDS_NEUTRINO : _STR_NEUTRINO_BAD_FORMAT), 6);
        goto fail;
    }

    const char *neutrinoPath = sbResolveNeutrinoPath(pDeviceData->bdmPrefix); // #300: AUTO probes this device for a co-located install
    if (neutrinoPath == NULL) {
        guiWarning(_l(isUdp ? _STR_NET_NEEDS_NEUTRINO : _STR_NEUTRINO_NOT_FOUND), 6);
        goto fail;
    }

    // Everything Neutrino actually needs from the config/device, copied to THIS stack frame
    // (deinit below frees configSet's owner + pDeviceData).
    // Absent per-game $NeutrinoVideo/$NeutrinoGsmComp keys = follow the global defaults (the
    // per-game picker's "Default" row removes the keys; explicit values -- including Off=0 --
    // persist and override).
    int compatmask = 0, neutrinoVideo = gNeutrinoVideoDefault, neutrinoGsmComp = gNeutrinoGsmCompDefault, neutrinoBsdfs = 0;
    char neutrinoExtraArgs[256] = "";
    neutrino_vmc_args_t neutrinoVmc = {0};
    char partname[256], bdmCurrentDriver[32];
    configGetInt(configSet, CONFIG_ITEM_COMPAT, &compatmask); // same source sbPrepare reads; no IRX patch needed
    configGetStrCopy(configSet, CONFIG_ITEM_NEUTRINO_ARGS, neutrinoExtraArgs, sizeof(neutrinoExtraArgs));
    configGetInt(configSet, CONFIG_ITEM_NEUTRINO_VIDEO, &neutrinoVideo);
    configGetInt(configSet, CONFIG_ITEM_NEUTRINO_GSMCOMP, &neutrinoGsmComp);
    configGetInt(configSet, CONFIG_ITEM_NEUTRINO_BSDFS, &neutrinoBsdfs);     // #11: bdm legs (incl. udpbd/udpfsbd) are the block-backed consumers
    sbBuildVmcNeutrinoArgs(configSet, pDeviceData->bdmPrefix, &neutrinoVmc); // Δ2-validated -mc args
    sbCreatePath(game, partname, pDeviceData->bdmPrefix, "/", 0);            // -dvd target (multi-part was rejected above)
    snprintf(bdmCurrentDriver, sizeof(bdmCurrentDriver), "%s", pDeviceData->bdmDriver);

    // Δ9: neutrino only discovers a blown fragment budget after its own IOP reset (debug
    // printf + exit = black screen). Count it now: udp consumes the launch, local falls native.
    if (!bdmNeutrinoFragBudgetOk(partname, &neutrinoVmc, isUdp))
        goto fail;

    if (gRememberLastPlayed) {
        configSetStr(configGetByType(CONFIG_LAST), "last_played", game->startup);
        saveConfig(CONFIG_LAST, 0);
    }

    // Δ6 preflight (driver token + network toml sync) -- abort stays in a live menu.
    if (sysNeutrinoPreflight(bdmCurrentDriver, neutrinoPath) < 0)
        goto fail;

    // MMCE cross-device game-id (#261) with the Δ3 -mc-covered-slot guard. Before deinit (uses game).
    // Same MX4SIO settle gate as the native leg below (CodeRabbit review of #248, vetted): a switch
    // still in flight through the IOP reboot can starve the SD enumeration on the shared SIO2.
    if (mmceSendGameID(game->startup, neutrinoPath,
                       (neutrinoVmc.arg[0][0] ? 1 : 0) | (neutrinoVmc.arg[1][0] ? 2 : 0)) < 0 &&
        bdmDriverIsMx4sio(bdmCurrentDriver)) {
        if (mmceGameIdSettle(5000) < 0)
            guiWarning(_l(_STR_MMCE_GAMEID_UNSETTLED), 6);
    }

    // game->startup lives inside bdmGames / gAutoLaunchBDMGame, both freed below (deinitEx's
    // itemCleanUp, or the explicit free in the autolaunch branch). Copy it before the teardown so
    // sysLaunchNeutrino's -elf build reads valid stack memory instead of freed heap (UAF).
    char bdmStartup[GAME_STARTUP_MAX + 1];
    snprintf(bdmStartup, sizeof(bdmStartup), "%s", game->startup);

    if (gAutoLaunchBDMGame == NULL) {
        // Keep-IOP handoff: keep BOTH the game device and the neutrino.elf device mounted
        // (Neutrino reads its -cwd config/modules and the ISO through our mounts pre-reset).
        int neutrinoDevMode = oplPath2Mode(neutrinoPath);
        deinitEx(UNMOUNT_EXCEPTION, itemList->mode, neutrinoDevMode); // CAREFUL: itemCleanUp frees bdmGames/game
    } else {
        miniDeinit(configSet);
        free(gAutoLaunchBDMGame);
        gAutoLaunchBDMGame = NULL;
        free(gAutoLaunchDeviceData);
        gAutoLaunchDeviceData = NULL;
    }

    // gPS2Logo passes the user's preference straight through: Neutrino performs its own logo
    // read/validation for -logo, so the native path's CheckPS2Logo disc pass is not needed here.
    sysLaunchNeutrino(bdmCurrentDriver, partname, bdmStartup, compatmask, gPS2Logo, neutrinoPath, neutrinoExtraArgs, neutrinoVideo, neutrinoGsmComp, neutrinoBsdfs, &neutrinoVmc);
    return 1;

fail:
    if (failResult && gAutoLaunchBDMGame != NULL) {
        miniDeinit(configSet); // mirror the normal autolaunch teardown (ioEnd/configEnd + frees configSet)
        free(gAutoLaunchBDMGame);
        gAutoLaunchBDMGame = NULL;
        free(gAutoLaunchDeviceData);
        gAutoLaunchDeviceData = NULL;
    }
    return failResult;
}

void bdmLaunchGame(item_list_t *itemList, int id, config_set_t *configSet)
{
    int i, fd, iop_fd, index, compatmask = 0;
    int EnablePS2Logo = 0;
    int result;
    u64 startingLBA;
    unsigned int startCluster;
    char partname[256], filename[32];
    base_game_info_t *game;
    struct cdvdman_settings_bdm *settings;
    u32 layer1_start, layer1_offset;
    unsigned short int layer1_part;

    bdm_device_data_t *pDeviceData = NULL;

    if (gAutoLaunchBDMGame == NULL) {
        pDeviceData = (bdm_device_data_t *)itemList->priv;
        // Resolve through the ACTIVE view's array (#120 split): in the VCD view this yields the VCD
        // entry whose name the POPSTARTER handoff below uses, and a stale toggle-window id yields the
        // empty entry rather than indexing the other view's (possibly NULL/shorter) array.
        game = bdmActiveGame(itemList, id);
        if (game == &bdmEmptyGame)
            return; // nothing there (stale id / not scanned) -- never launch a blank entry
    } else {
        pDeviceData = gAutoLaunchDeviceData;
        game = gAutoLaunchBDMGame;
    }

    // Folder browsing: a folder row is never launchable (the dispatch descends instead of reaching
    // here); guard defensively. Otherwise pin the path composers to the current subfolder so a game
    // inside it resolves to <prefix>CD|DVD/<sub>/<name>.
    if (game != NULL && game->format == GAME_FORMAT_FOLDER)
        return;
    sbSetBrowseSub(folderGetSub(itemList->mode));

    // VCD view: this device is showing PS1 VCDs -> hand off to POPSTARTER (by name) instead of the
    // disc / Neutrino path below (which is entirely disc-specific). The BDM_TYPE_ATA internal exFAT HDD
    // still launches off the live massN: prefix, NOT ata0:/ (OPL never mounts an ata0: filesystem -- a
    // re-point there made LoadELFFromFile fail into an already-deinit'd OPL = black-screen freeze).
    if (gAutoLaunchBDMGame == NULL && game != NULL && vcdViewActive(itemList->mode)) {
        bdmLaunchVcd(itemList, game->name, configSet);
        return;
    }

    // Δ8: Neutrino core gets its own lean path FIRST -- everything below is native-core prep it
    // neither needs nor should be able to die on (see bdmTryNeutrinoLaunch).
    if (bdmTryNeutrinoLaunch(itemList, game, pDeviceData, configSet))
        return;

    char vmc_name[32], vmc_path[256], have_error = 0;
    int vmc_id, size_mcemu_irx = 0;
    bdm_vmc_infos_t bdm_vmc_infos;
    vmc_superblock_t vmc_superblock;

    for (vmc_id = 0; vmc_id < 2; vmc_id++) {
        memset(&bdm_vmc_infos, 0, sizeof(bdm_vmc_infos_t));
        configGetVMC(configSet, vmc_name, sizeof(vmc_name), vmc_id);
        if (vmc_name[0]) {
            have_error = 1;
            int vmcSizeInMb = sysCheckVMC(pDeviceData->bdmPrefix, "/", vmc_name, 0, &vmc_superblock);
            if (vmcSizeInMb > 0) {
                bdm_vmc_infos.flags = vmc_superblock.mc_flag & 0xFF;
                bdm_vmc_infos.flags |= 0x100;
                bdm_vmc_infos.specs.page_size = vmc_superblock.page_size;
                bdm_vmc_infos.specs.block_size = vmc_superblock.pages_per_block;
                bdm_vmc_infos.specs.card_size = vmc_superblock.pages_per_cluster * vmc_superblock.clusters_per_card;

                sprintf(vmc_path, "%sVMC/%s.bin", pDeviceData->bdmPrefix, vmc_name);

                fd = open(vmc_path, O_RDONLY);
                if (fd >= 0) {
                    iop_fd = ps2sdk_get_iop_fd(fd);
                    if (fileXioIoctl2(iop_fd, USBMASS_IOCTL_GET_LBA, NULL, 0, &startingLBA, sizeof(startingLBA)) == 0 && (startCluster = (unsigned int)fileXioIoctl(iop_fd, USBMASS_IOCTL_GET_CLUSTER, vmc_path)) != 0) {

                        // VMC only supports 32bit LBAs at the moment, so if the starting LBA + size of the VMC crosses the 32bit boundary
                        // just report the VMC as being fragmented to prevent file system corruption.
                        int vmcSectorCount = vmcSizeInMb * ((1024 * 1024) / 512); // size in MB * sectors per MB
                        if (startingLBA + vmcSectorCount > 0x100000000) {
                            LOG("BDMSUPPORT VMC bad LBA range\n");
                            have_error = 2;
                        }
                        // Check VMC cluster chain for fragmentation (write operation can cause damage to the filesystem).
                        else if (fileXioIoctl(iop_fd, USBMASS_IOCTL_CHECK_CHAIN, "") == 1) {
                            LOG("BDMSUPPORT Cluster Chain OK\n");
                            have_error = 0;
                            bdm_vmc_infos.active = 1;
                            bdm_vmc_infos.start_sector = startingLBA;
                            LOG("BDMSUPPORT VMC slot %d start: 0x%08x%08x\n", vmc_id, ((u32 *)&startingLBA)[1], ((u32 *)&startingLBA)[0]);
                        } else {
                            LOG("BDMSUPPORT Cluster Chain NG\n");
                            have_error = 2;
                        }
                    }

                    close(fd);
                }
            }
        }

        if (gAutoLaunchBDMGame == NULL) {
            if (have_error) {
                char error[256];
                if (have_error == 2) // VMC file is fragmented
                    snprintf(error, sizeof(error), _l(_STR_ERR_VMC_FRAGMENTED_CONTINUE), vmc_name, (vmc_id + 1));
                else
                    snprintf(error, sizeof(error), _l(_STR_ERR_VMC_CONTINUE), vmc_name, (vmc_id + 1));
                if (!guiMsgBox(error, 1, NULL)) {
                    return;
                }
            }
        } else
            LOG("VMC error\n");

        for (i = 0; i < size_bdm_mcemu_irx; i++) {
            if (((u32 *)&bdm_mcemu_irx)[i] == (0xC0DEFAC0 + vmc_id)) {
                if (bdm_vmc_infos.active)
                    size_mcemu_irx = size_bdm_mcemu_irx;
                memcpy(&((u32 *)&bdm_mcemu_irx)[i], &bdm_vmc_infos, sizeof(bdm_vmc_infos_t));
                break;
            }
        }
    }

    void *irx = NULL;
    int irx_size = 0;
    if (bdmDriverIsATA(pDeviceData->bdmDriver)) {
        irx = &bdm_ata_cdvdman_irx;
        irx_size = size_bdm_ata_cdvdman_irx;
    } else {
        irx = &bdm_cdvdman_irx;
        irx_size = size_bdm_cdvdman_irx;
    }

    compatmask = sbPrepare(game, configSet, irx_size, irx, &index);
    if (compatmask < 0) // sbPrepare failed (patch zone not found): `index` is unset -- bail before using
        return;         // it. (The old `if (settings == NULL)` guard was dead: irx + index is never NULL.)

#ifdef PADEMU
    // MX4SIO reads the SD card over the SIO2 / memory-card bus -- the SAME bus pad emulation hooks
    // (modules/pademu installs a sio2man hook while pad emulation is enabled). With both active they
    // contend on SIO2 and the game frequently fails to boot (black screen, issue #53). It is an
    // inherent shared-bus conflict, not fixable here, so warn instead of leaving a mystery black
    // screen. sbPrepare() above resolved gEnablePadEmu for this game (global or per-game).
    if (pDeviceData->bdmDeviceType == BDM_TYPE_SDC && gEnablePadEmu)
        guiWarning(_l(_STR_MX4SIO_PADEMU_WARN), 6);
#endif

    settings = (struct cdvdman_settings_bdm *)((u8 *)irx + index);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
    memset(&settings->frags[0], 0, sizeof(bd_fragment_t) * BDM_MAX_FRAGS);
#pragma GCC diagnostic pop
    u8 iTotalFragCount = 0;

    //
    // Add ISO as fragfile[0] to fragment list
    //
    struct cdvdman_fragfile *iso_frag = &settings->fragfile[0];
    iso_frag->frag_start = 0;
    iso_frag->frag_count = 0;
    for (i = 0; i < game->parts; i++) {
        // Open file
        sbCreatePath(game, partname, pDeviceData->bdmPrefix, "/", i);
        fd = open(partname, O_RDONLY);
        if (fd < 0) {
            sbUnprepare(&settings->common);
            guiMsgBox(_l(_STR_ERR_FILE_INVALID), 0, NULL);
            return;
        }
        iop_fd = ps2sdk_get_iop_fd(fd); // only valid after confirming fd >= 0

        // Get fragment list
        int iFragCount = fileXioIoctl2(iop_fd, USBMASS_IOCTL_GET_FRAGLIST, NULL, 0, (void *)&settings->frags[iTotalFragCount], sizeof(bd_fragment_t) * (BDM_MAX_FRAGS - iTotalFragCount));
        if (iFragCount < 0 || iFragCount > BDM_MAX_FRAGS - iTotalFragCount) {
            // Negative (ioctl error) or more fragments than the remaining frags[] slots.
            // A negative value would underflow the u8 iTotalFragCount and corrupt the next
            // iteration's destination offset and size.
            close(fd);
            sbUnprepare(&settings->common);
            guiMsgBox(_l(_STR_ERR_FRAGMENTED), 0, NULL);
            return;
        }
        iso_frag->frag_count += iFragCount;
        iTotalFragCount += iFragCount;

        if ((gPS2Logo) && (i == 0))
            EnablePS2Logo = CheckPS2Logo(fd, 0);

        close(fd);
    }

    // Initialize layer 1 information.
    sbCreatePath(game, partname, pDeviceData->bdmPrefix, "/", 0);
    layer1_start = sbGetISO9660MaxLBA(partname);

    switch (game->format) {
        case GAME_FORMAT_USBLD:
            layer1_part = layer1_start / 0x80000;
            layer1_offset = layer1_start % 0x80000;
            sbCreatePath(game, partname, pDeviceData->bdmPrefix, "/", layer1_part);
            break;
        default: // Raw ISO9660 disc image; one part.
            layer1_part = 0;
            layer1_offset = layer1_start;
    }

    if (sbProbeISO9660(partname, game, layer1_offset) != 0) {
        layer1_start = 0;
        LOG("DVD detected.\n");
    } else {
        layer1_start -= 16;
        LOG("DVD-DL layer 1 @ part %u sector 0x%lx.\n", layer1_part, layer1_offset);
    }
    settings->common.layer1_start = layer1_start;

    // adjust ZSO cache
    settings->common.zso_cache = bdmCacheSize;

    if ((result = sbLoadCheats(pDeviceData->bdmPrefix, game->startup)) < 0) {
        // #265: let the user back out instead of sitting through the whole load. The helper does
        // the sbUnprepare itself -- see include/supportbase.h; skipping it breaks the NEXT launch.
        if (!sbCheatsMissingContinue(&settings->common, result))
            return;
    }
    sbLoadImage(pDeviceData->bdmPrefix, game->startup);

    if (gRememberLastPlayed) {
        configSetStr(configGetByType(CONFIG_LAST), "last_played", game->startup);
        saveConfig(CONFIG_LAST, 0);
    }

    if (configGetStrCopy(configSet, CONFIG_ITEM_ALTSTARTUP, filename, sizeof(filename)) == 0)
        strcpy(filename, game->startup);

    // deinit will free per device data.. copy driver name before free to compare for launch
    char bdmCurrentDriver[32];
    snprintf(bdmCurrentDriver, sizeof(bdmCurrentDriver), "%s", pDeviceData->bdmDriver);
    bdmSetLaunchDeviceBinding(settings, bdmCurrentDriver, pDeviceData->massDeviceIndex);

    if (bdmDriverIsATA(bdmCurrentDriver)) {
        // Get DMA settings for ATA mode.
        int dmaType = 0, dmaMode = 7;

        if (pDeviceData->bdmHddIsLBA48 < 0 || pDeviceData->ataHighestUDMAMode < 0)
            bdmResolveLBA_UDMA(pDeviceData);

        configGetInt(configSet, CONFIG_ITEM_DMA, &dmaMode);

        // Set DMA mode and spindown time.
        if (dmaMode < 3)
            dmaType = 0x20;
        else {
            dmaType = 0x40;
            dmaMode -= 3;
            // Use the user's configured UDMA mode, only CLAMPED to the drive's max -- don't FORCE the
            // highest, which is unstable on some drives/SATA adapters (wOPL 0f7443e, KrahJohlito). NB:
            // RiptOPL already avoids the worse scan-time UDMA push -- bdmResolveLBA_UDMA only queries
            // (never sets) the mode and caps it to UDMA 4 -- so the reported exFAT-HDD corruption from a
            // max-UDMA scan does not apply here; this just respects a user's lower-DMA stability choice.
            if (pDeviceData->ataHighestUDMAMode > 0 && dmaMode > pDeviceData->ataHighestUDMAMode)
                dmaMode = pDeviceData->ataHighestUDMAMode;
        }

        hddSetTransferMode(dmaType, dmaMode);
        // gHDDSpindown [0..20] -> spindown [0..240] -> seconds [0..1200]
        hddSetIdleTimeout(gHDDSpindown * 12);
        settings->hddIsLBA48 = pDeviceData->bdmHddIsLBA48;
    }

    // Δ8: Neutrino never reaches this point (bdmTryNeutrinoLaunch handled it at the top), so
    // everything from here on is the NATIVE (embedded cdvdman) launch only -- including the udp
    // impossibility: the udp device is Neutrino-only and its aborts happen inside the helper.

    // MMCE cross-device game-id (#261): push the disc id to a present SD2PSX/MemCard PRO2 (either slot)
    // so it switches its per-game folder, even though this game is not on the MMCE. Self-probes +
    // no-ops if no card answers / feature off. Must run BEFORE deinit frees `game`.
    //
    // MX4SIO settle gate (HW batch S7): MX4SIO shares SIO2 with the MMCE, and an mmce card switch
    // still in flight during the IOP reboot can starve the SD enumeration cdvdman then waits on
    // FOREVER (the lime-green hang -- ee_core's marker right before LoadElf from cdrom0:). When the
    // send reports "switched but settle NOT confirmed" (-1), re-probe the exact slot it targeted for
    // up to 5 s; on expiry, warn and LAUNCH ANYWAY (the gate mitigates, never blocks -- and other
    // BDM transports don't share SIO2, so only the SDC leg pays it).
    if (mmceSendGameID(game->startup, NULL, 0) < 0 && bdmDriverIsMx4sio(bdmCurrentDriver)) {
        if (mmceGameIdSettle(5000) < 0)
            guiWarning(_l(_STR_MMCE_GAMEID_UNSETTLED), 6);
    }

    if (gAutoLaunchBDMGame == NULL) {
        deinit(NO_EXCEPTION, itemList->mode); // CAREFUL: deinit will call bdmCleanUp, so bdmGames/game will be freed
    } else {
        miniDeinit(configSet);

        free(gAutoLaunchBDMGame);
        gAutoLaunchBDMGame = NULL;

        free(gAutoLaunchDeviceData);
        gAutoLaunchDeviceData = NULL;
    }

    LOG("bdm pre sysLaunchLoaderElf\n");
    if (bdmDriverIsUSB(bdmCurrentDriver)) {
        settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_USBD;
        sysLaunchLoaderElf(filename, "BDM_USB_MODE", irx_size, irx, size_mcemu_irx, bdm_mcemu_irx, EnablePS2Logo, compatmask);
    } else if (bdmDriverIsIlink(bdmCurrentDriver)) {
        settings->common.fakemodule_flags |= 0 /* TODO! fake ilinkman ? */;
        sysLaunchLoaderElf(filename, "BDM_ILK_MODE", irx_size, irx, size_mcemu_irx, bdm_mcemu_irx, EnablePS2Logo, compatmask);
    } else if (bdmDriverIsMx4sio(bdmCurrentDriver)) {
        settings->common.fakemodule_flags |= 0;
        sysLaunchLoaderElf(filename, "BDM_M4S_MODE", irx_size, irx, size_mcemu_irx, bdm_mcemu_irx, EnablePS2Logo, compatmask);
    } else if (bdmDriverIsATA(bdmCurrentDriver)) {
        settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_DEV9;
        settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_ATAD;
        sysLaunchLoaderElf(filename, "BDM_ATA_MODE", irx_size, irx, size_mcemu_irx, bdm_mcemu_irx, EnablePS2Logo, compatmask);
    }
}

static config_set_t *bdmGetConfig(item_list_t *itemList, int id)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    // Active view's array (#120 split) -- a VCD's per-game CFG keys off its own entry, and a stale
    // toggle-window id resolves to the empty entry instead of the other view's array.
    return sbPopulateConfig(bdmActiveGame(itemList, id), pDeviceData->bdmPrefix, "/");
}

static int bdmGetImage(item_list_t *itemList, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)
{
    char path[256];

    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    // OPL's own ART/<name>_COV.png is the PRIMARY lookup (same path PS2 uses; the cache also retries once
    // with a strict PS1 ID).
    if (isRelative)
        snprintf(path, sizeof(path), "%s%s/%s_%s", pDeviceData->bdmPrefix, folder, value, suffix);
    else
        snprintf(path, sizeof(path), "%s%s_%s", folder, value, suffix);
    int r = texDiscoverLoad(resultTex, path, -1);
    // On a VCD (PS1) genuine miss, fall back to the POPSLoader-style suffixless cover next to the .VCD --
    // ROOT-anchored (bdmBuildVcdPrefix), the SAME prefix the VCD scan uses, since the POPS layout lives at
    // the device root, outside gBDMPrefix. Cover/icon only, VCD view only.
    if (r == ERR_BAD_FILE && isRelative && vcdViewActive(itemList->mode)) {
        char vcdPrefix[BDM_DEVICE_ROOT_MAX + 2];
        bdmBuildVcdPrefix(vcdPrefix, sizeof(vcdPrefix), itemList->mode);
        r = vcdLoadPopsCover(vcdPrefix, value, suffix, resultTex);
    }
    return r;
}

static int bdmGetTextId(item_list_t *itemList)
{
    int mode = _STR_BDM_GAMES;

    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    if (bdmDriverIsUSB(pDeviceData->bdmDriver))
        mode = _STR_USB_GAMES;
    else if (bdmDriverIsIlink(pDeviceData->bdmDriver))
        mode = _STR_ILINK_GAMES;
    else if (bdmDriverIsMx4sio(pDeviceData->bdmDriver))
        mode = _STR_MX4SIO_GAMES;
    else if (bdmDriverIsATA(pDeviceData->bdmDriver))
        mode = _STR_HDD_GAMES;
    else if (bdmDriverIsUDPBD(pDeviceData->bdmDriver))
        mode = (bdmGetLoadedNetProtocol() == NET_BOOT_UDPFS) ? _STR_UDPFSBD_GAMES : _STR_UDPBD_GAMES; // BLOCK: UDPFSBD vs UDPBD (the udpfs FILESYSTEM tab is _STR_UDPFS_GAMES, a separate device); mirror bdmGetIconId

    return mode;
}

static int bdmGetIconId(item_list_t *itemList)
{
    int mode = BDM_ICON;

    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;

    if (bdmDriverIsUSB(pDeviceData->bdmDriver))
        mode = USB_ICON;
    else if (bdmDriverIsIlink(pDeviceData->bdmDriver))
        mode = ILINK_ICON;
    else if (bdmDriverIsMx4sio(pDeviceData->bdmDriver))
        mode = MX4SIO_ICON;
    else if (bdmDriverIsATA(pDeviceData->bdmDriver))
        mode = HDD_BD_ICON;
    else if (bdmDriverIsUDPBD(pDeviceData->bdmDriver))
        mode = (bdmGetLoadedNetProtocol() == NET_BOOT_UDPFS) ? UDPFS_ICON : UDP_ICON;

    return mode;
}

// This may be called, even if bdmInit() was not.
static void bdmCleanUp(item_list_t *itemList, int exception)
{
    if (itemList->enabled) {
        LOG("BDMSUPPORT CleanUp\n");

        bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
        free(pDeviceData->bdmGames);
        free(pDeviceData->bdmVcdGames); // #120: free the separate VCD store too
        free(pDeviceData);
        itemList->priv = NULL;

        //      if ((exception & UNMOUNT_EXCEPTION) == 0)
        //          ...
    }
}

// This may be called, even if bdmInit() was not.
static void bdmShutdown(item_list_t *itemList)
{
    char path[BDM_DEVICE_ROOT_MAX];
    LOG("BDMSUPPORT Shutdown\n");

    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    snprintf(path, sizeof(path), "mass%d:", itemList->mode);

    // As required by some (typically 2.5") HDDs, issue the SCSI STOP UNIT command to avoid causing an emergency park.
    // pDeviceData may be NULL here (shutdown without init, or a second deinit pass after priv was freed below),
    // so guard the dereference and fall back to the constructed mass%d: path.
    fileXioDevctl((pDeviceData != NULL && pDeviceData->bdmDeviceRoot[0] != '\0') ? pDeviceData->bdmDeviceRoot : path, USBMASS_DEVCTL_STOP_ALL, NULL, 0, NULL, 0);

    if (itemList->enabled && pDeviceData != NULL) {
        LOG("BDMSUPPORT Shutdown free data\n");

        // Free device data.
        free(pDeviceData->bdmGames);
        free(pDeviceData);
        itemList->priv = NULL;
    }
}

static int bdmCheckVMC(item_list_t *itemList, char *name, int createSize)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    return sysCheckVMC(pDeviceData->bdmPrefix, "/", name, createSize, NULL);
}

static char *bdmGetPrefix(item_list_t *itemList)
{
    bdm_device_data_t *pDeviceData = (bdm_device_data_t *)itemList->priv;
    return pDeviceData->bdmPrefix;
}

static item_list_t bdmGameList = {
    BDM_MODE, 2, 0, 0, MENU_MIN_INACTIVE_FRAMES, BDM_MODE_UPDATE_DELAY, NULL, NULL, &bdmGetTextId, &bdmGetPrefix, &bdmInit, &bdmNeedsUpdate,
    &bdmUpdateGameList, &bdmGetGameCount, &bdmGetGame, &bdmGetGameName, &bdmGetGameNameLength, &bdmGetGameStartup, &bdmDeleteGame, &bdmRenameGame,
    &bdmLaunchGame, &bdmGetConfig, &bdmGetImage, &bdmCleanUp, &bdmShutdown, &bdmCheckVMC, &bdmGetIconId, &bdmLaunchVcd};

void bdmInitSemaphore()
{
    // Create a semaphore so only one thread can load IOP modules at a time.
    ee_sema_t semaphore;
    semaphore.init_count = 1;
    semaphore.max_count = 1;
    semaphore.option = 0;
    bdmLoadModuleLock = CreateSema(&semaphore);
}

void bdmInitDevicesData()
{
    // If the device list hasn't been initialized do it now.
    if (bdmDeviceListInitialized == 0) {
        bdmDeviceListInitialized = 1;

        for (int i = 0; i < MAX_BDM_DEVICES; i++) {
            // Setup the device list item.
            item_list_t *pDeviceSupport = &bdmDeviceList[i];
            memcpy(pDeviceSupport, &bdmGameList, sizeof(item_list_t));
            pDeviceSupport->mode = i;

            // Setup the per-device data.
            bdm_device_data_t *pDeviceData = (bdm_device_data_t *)malloc(sizeof(bdm_device_data_t));
            if (pDeviceData != NULL) // guard the memset on OOM; bdmShutdown tolerates a NULL priv
                memset(pDeviceData, 0, sizeof(bdm_device_data_t));
            pDeviceSupport->priv = pDeviceData;
        }
    }

    // Refresh the visibility of the menu.
    for (int i = 0; i < MAX_BDM_DEVICES; i++) {
        // Register the device structure into the UI.
        initSupport(&bdmDeviceList[i], i, 0);

        // If bdm support is set to auto then make the page invisible and reset the bdm tick counter, when a bdm device is mounted it will dynamically be made visible.
        // If bdm support is set to manual then only make the first page visible.
        if (bdmDeviceList[i].owner != NULL) {
            opl_io_module_t *pOwner = (opl_io_module_t *)bdmDeviceList[i].owner;

            int effectiveMode = bdmEffectiveStartMode();
            if (effectiveMode == START_MODE_DISABLED) {
                pOwner->menuItem.visible = 0;
            } else if (effectiveMode == START_MODE_MANUAL) {
                // If BDM has already been started then make the page invisible and reset the bdm tick counter so visibility status is refreshed
                // according to device state.
                if (bdmDeviceModeStarted == 1) {
                    pOwner->menuItem.visible = 0;
                    ((bdm_device_data_t *)bdmDeviceList[i].priv)->bdmDeviceTick = -1;
                } else
                    pOwner->menuItem.visible = (i == 0 ? 1 : 0);
            } else if (effectiveMode == START_MODE_AUTO) {
                pOwner->menuItem.visible = 0;
                ((bdm_device_data_t *)bdmDeviceList[i].priv)->bdmDeviceTick = -1;
            }

            LOG("bdmInitDevicesData: setting device %d %s\n", i, (pOwner->menuItem.visible != 0 ? "visible" : "invisible"));
        }
    }
}

void bdmEnumerateDevices()
{
    LOG("bdmEnumerateDevices\n");

    // Initialize the device list data if it hasn't been initialized yet.
    bdmInitDevicesData();

    // Because bdmLoadModules is called before the config file is loaded bdmLoadBlockDeviceModules will not have loaded any
    // optional bdm modules. Now that the config file has been loaded try loading any optional modules that weren't previously loaded.
    ioPutRequest(IO_CUSTOM_SIMPLEACTION, &bdmLoadBlockDeviceModules);

    LOG("bdmEnumerateDevices done\n");
}

void bdmResolveLBA_UDMA(bdm_device_data_t *pDeviceData)
{
    // If atad is loaded then xhdd is also loaded, query the hdd to see if it supports LBA48 or not.
    pDeviceData->bdmHddIsLBA48 = fileXioDevctl("xhdd0:", ATA_DEVCTL_IS_48BIT, NULL, 0, NULL, 0);
    if (pDeviceData->bdmHddIsLBA48 < 0) {
        // Failed to query the LBA limit of the device, fail safe to LBA28.
        LOG("Mass device %d is backed by ATA but failed to get LBA limit %d\n", pDeviceData->massDeviceIndex, pDeviceData->bdmHddIsLBA48);
        pDeviceData->bdmHddIsLBA48 = 0;
    }

    // Query the drive for the highest UDMA mode.
    pDeviceData->ataHighestUDMAMode = fileXioDevctl("xhdd0:", ATA_DEVCTL_GET_HIGHEST_UDMA_MODE, NULL, 0, NULL, 0);
    if (pDeviceData->ataHighestUDMAMode < 0) {
        // Failed to query highest UDMA mode supported.
        LOG("Mass device %d is backed by ATA but failed to get highest UDMA mode %d\n", pDeviceData->massDeviceIndex, pDeviceData->ataHighestUDMAMode);
        pDeviceData->ataHighestUDMAMode = 4;
    } else if (pDeviceData->ataHighestUDMAMode > 4) {
        // Limit max UDMA mode to 4 to avoid compatibility issues
        LOG("Mass device %d supports up to UDMA mode %d, limiting to UDMA 4\n", pDeviceData->massDeviceIndex, pDeviceData->ataHighestUDMAMode);
        pDeviceData->ataHighestUDMAMode = 4;
    }
}

int bdmUpdateDeviceData(item_list_t *itemList)
{
    char path[BDM_DEVICE_ROOT_MAX + 2] = {0};
    int driverResult, deviceResult;

    // If bdm mode is disabled bail out as we don't want to update the visibility state of the device pages.
    if (bdmEffectiveStartMode() == START_MODE_DISABLED)
        return 0;

    // LOG("bdmUpdateDeviceData: %d\n", itemList->mode);

    // Get the per-device data and check if the menu item is currently visible.
    bdm_device_data_t *pDeviceData = itemList->priv;
    int visible = itemList->owner != NULL ? ((opl_io_module_t *)itemList->owner)->menuItem.visible : 0;

    // Format the device path and try to open the device.
    snprintf(path, sizeof(path), "mass%d:/", itemList->mode);
    int dir = fileXioDopen(path);
    // LOG("opendir %s -> %d\n", path, dir);

    // Device reachable this poll -> clear the debounce miss counter (see the hide branch below).
    if (dir >= 0)
        pDeviceData->bdmMissCount = 0;

    // If we opened the device and the menu isn't visible (OR is visible but hasn't been initialized ex: manual device start) initialize device info.
    if (dir >= 0 && (visible == 0 || pDeviceData->bdmDeviceRoot[0] == '\0' || pDeviceData->bdmDriver[0] == '\0' || pDeviceData->bdmDeviceType == BDM_TYPE_UNKNOWN)) {
        snprintf(pDeviceData->bdmDeviceRoot, sizeof(pDeviceData->bdmDeviceRoot), "mass%d:", itemList->mode);
        bdmBuildGamePrefix(pDeviceData->bdmPrefix, sizeof(pDeviceData->bdmPrefix), pDeviceData->bdmDeviceRoot);
        pDeviceData->FoldersCreated = 0;

        memset(pDeviceData->bdmDriver, 0, sizeof(pDeviceData->bdmDriver));
        pDeviceData->massDeviceIndex = -1;
        pDeviceData->bdmDeviceType = BDM_TYPE_UNKNOWN;
        pDeviceData->bdmHddIsLBA48 = -1;
        pDeviceData->ataHighestUDMAMode = -1;

        // Get the name of the underlying device driver that backs the fat fs.
        driverResult = fileXioIoctl2(dir, USBMASS_IOCTL_GET_DRIVERNAME, NULL, 0, pDeviceData->bdmDriver, sizeof(pDeviceData->bdmDriver) - 1);
        deviceResult = -1;
        if (driverResult >= 0)
            deviceResult = fileXioIoctl2(dir, USBMASS_IOCTL_GET_DEVICE_NUMBER, NULL, 0, &pDeviceData->massDeviceIndex, sizeof(pDeviceData->massDeviceIndex));

        itemList->flags = 0;

        if (driverResult < 0) {
            LOG("Mass device: %d identity lookup failed for driver name: %d, using %s\n", itemList->mode, driverResult, pDeviceData->bdmDeviceRoot);
        } else {
            pDeviceData->bdmDeviceType = bdmDetermineDeviceType(pDeviceData->bdmDriver);

            if (deviceResult < 0) {
                LOG("Mass device: %d driver %s failed to report device number: %d, using %s\n", itemList->mode, pDeviceData->bdmDriver, deviceResult, pDeviceData->bdmDeviceRoot);
            }
        }

        if (pDeviceData->bdmDeviceType == BDM_TYPE_ATA) {
            itemList->flags = MODE_FLAG_COMPAT_DMA;
        }

        if (pDeviceData->bdmDeviceType == BDM_TYPE_ATA)
            LOG("Mass device: %d (%d) ATA device %s (compat root %s)\n", itemList->mode, pDeviceData->massDeviceIndex, pDeviceData->bdmPrefix, pDeviceData->bdmDeviceRoot);
        else if (pDeviceData->bdmDriver[0] != '\0')
            LOG("Mass device: %d (%d) %s -> %s (compat root %s)\n", itemList->mode, pDeviceData->massDeviceIndex, pDeviceData->bdmPrefix, pDeviceData->bdmDriver, pDeviceData->bdmDeviceRoot);
        else
            LOG("Mass device: %d using generic BDM path %s\n", itemList->mode, pDeviceData->bdmPrefix);

        // Publish-time enable gate (#120 audit F-13): the visible-page filter in bdmNeedsUpdate only
        // hides a page that is ALREADY showing, i.e. one refresh pass too late. Without this gate a
        // mounted slot on a DISABLED transport (e.g. an ATA-backed mass slot left over from the
        // boot-resolver escalation or a BDMA-equip force-load, with BDM HDD OFF) flashes its tab,
        // plays the connect sound and folder-writes the device on every BdmGeneration bump. Identity
        // stays populated above: the BDMA equip and the device pickers read it through
        // bdmGetDeviceSlotsByType/bdmReadDeviceIdentity independent of page visibility.
        if (bdmTransportEnabled(pDeviceData->bdmDeviceType) == 0) {
            LOG("bdmUpdateDeviceData: device %d is %s-backed but that transport is disabled; not publishing\n",
                itemList->mode, pDeviceData->bdmDriver);
            fileXioDclose(dir);
            return 0;
        }

        // An EXPLICIT BDM = Off is only overridden by the UDPBD floor (bdmEffectiveStartMode) so
        // the selected UDPBD protocol can publish ITS tab -- every other transport's page stays
        // hidden exactly as the user asked (their sticks would otherwise resurface with the dialog
        // still showing "Off").
        if (gBDMStartMode == START_MODE_DISABLED && pDeviceData->bdmDeviceType != BDM_TYPE_UDPBD) {
            LOG("bdmUpdateDeviceData: device %d withheld -- BDM is explicitly Off (UDPBD floor active)\n",
                itemList->mode);
            fileXioDclose(dir);
            return 0;
        }

        // Make the menu item visible.
        if (itemList->owner != NULL) {
            LOG("bdmUpdateDeviceData: setting device %d visible\n", itemList->mode);
            ((opl_io_module_t *)itemList->owner)->menuItem.visible = 1;
        }

        // Close the device handle.
        fileXioDclose(dir);
        return 1;
    } else if (dir < 0 && visible == 1) {
        // Device open failed while the page is showing. For a network-backed device (UDPBD/UDPFS) one
        // missed poll is usually a transient stall, not a real removal -- debounce so the tab doesn't
        // flicker away on a blip. Local devices (USB/SDC/ATA) hide on the first miss = a real unplug.
        int hideThreshold = (pDeviceData->bdmDeviceType == BDM_TYPE_UDPBD) ? BDM_NET_HIDE_MISSES : 1;
        if (++pDeviceData->bdmMissCount < hideThreshold) {
            LOG("bdmUpdateDeviceData: device %d miss %d/%d (debounce, keeping page)\n", itemList->mode, pDeviceData->bdmMissCount, hideThreshold);
            return 0; // within the grace window -- keep the page, re-poll next cycle
        }

        // Device has been removed, make the menu item invisible. We can't really cleanup resources (like the game list) just yet
        // as we don't know if the data is being used asynchronously.
        if (itemList->owner != NULL) {
            LOG("bdmUpdateDeviceData: setting device %d invisible\n", itemList->mode);
            ((opl_io_module_t *)itemList->owner)->menuItem.visible = 0;
        }

        pDeviceData->FoldersCreated = 0;
        pDeviceData->bdmMissCount = 0;
        LOG("Mass device: %d (%d) disconnected\n", itemList->mode, pDeviceData->massDeviceIndex);
        return -1;
    }

    // No change to the device state detected.
    if (dir >= 0)
        fileXioDclose(dir);
    return 0;
}

static int bdmWaitForDevice(int deviceId, u32 timeoutMs)
{
    const int RETRY_DELAY = 100;
    char path[16];

    u32 start = GetTimerSystemTime();
    sprintf(path, "mass%d:/", deviceId);

    while (1) {
        int dir = fileXioDopen(path);

        if (dir >= 0) {
            fileXioDclose(dir);
            return 1; // ready
        }

        u32 now = GetTimerSystemTime();
        u32 elapsed_ms = (now - start) / (kBUSCLK / 1000);

        if (elapsed_ms > timeoutMs) {
            return 0; // timeout
        }

        DelayThread(RETRY_DELAY * 1000);
    }
}

static int bdmDeviceIsPresent(int deviceId)
{
    char path[16];
    sprintf(path, "mass%d:/", deviceId);
    int dir = fileXioDopen(path);

    if (dir >= 0) {
        fileXioDclose(dir);
        return 1; // ready
    }

    return 0;
}

// Load the block-device transport driver for one BDM type, IGNORING the gEnable* config gates (the
// callers exist precisely because the wanted device may not be enabled for games -- or, for the boot-dir
// resolver, because the config that holds the gates cannot be read until the transport is up). Caller
// must hold bdmLoadModuleLock. Idempotent. Returns 1 when the transport is (already) loaded, 0 on
// failure or an unsupported type. MMCE is a separate subsystem with its own namespace + loader.
static int bdmEnsureTransportLoaded(int bdmType)
{
    switch (bdmType) {
        case BDM_TYPE_USB:
            if (!iUSBModLoaded && bdmLoadOptionalModule("USBMASS_BD", &usbmass_bd_irx, size_usbmass_bd_irx) >= 0)
                iUSBModLoaded = 1;
            return iUSBModLoaded;
        case BDM_TYPE_SDC:
            if (!mx4sioModLoaded && bdmLoadOptionalModule("MX4SIO_BD", &mx4sio_bd_irx, size_mx4sio_bd_irx) >= 0)
                mx4sioModLoaded = 1;
            return mx4sioModLoaded;
        case BDM_TYPE_ILINK:
            if (!iLinkModLoaded) {
                if (!iLinkManModLoaded && bdmLoadOptionalModule("ILINKMAN", &iLinkman_irx, size_iLinkman_irx) >= 0)
                    iLinkManModLoaded = 1;
                if (iLinkManModLoaded && !ieee1394ModLoaded && bdmLoadOptionalModule("IEEE1394_BD", &IEEE1394_bd_irx, size_IEEE1394_bd_irx) >= 0)
                    ieee1394ModLoaded = 1;
                iLinkModLoaded = iLinkManModLoaded && ieee1394ModLoaded;
            }
            return iLinkModLoaded;
        case BDM_TYPE_ATA:
            if (!hddModLoaded && hddLoadModulesReady())
                hddModLoaded = 1;
            return hddModLoaded;
        default:
            return 0;
    }
}

// Ensure the BDM transport for a BDMA source type is loaded AND a device of that type is mounted, so
// the equip can read modules from a source device even when that device family is NOT enabled for games
// (e.g. BDMA files kept on a USB stick you never browse). Force-loads the driver -- the games tab stays
// hidden because bdmNeedsUpdate gates page visibility on gEnable* independently -- then waits up to
// timeoutMs for the just-loaded device to mount. Idempotent + instant when the transport is already up.
// Returns 1 if a device of the type is present afterwards, 0 otherwise.
int bdmEnsureSourceModules(int bdmType, u32 timeoutMs)
{
    int wasLoaded;
    char root[BDM_DEVICE_ROOT_MAX + 2];
    // u64, NOT u32: GetTimerSystemTime() returns a u64 bus-clock count that passes 2^32 after ~29 s of
    // uptime. Truncating the start into a u32 while subtracting it from the full u64 makes the elapsed
    // math explode on the first check for any call after that -- the wait budget silently collapses.
    u64 start;

    WaitSema(bdmLoadModuleLock);
    switch (bdmType) {
        case BDM_TYPE_USB:
            wasLoaded = iUSBModLoaded;
            break;
        case BDM_TYPE_SDC:
            wasLoaded = mx4sioModLoaded;
            break;
        case BDM_TYPE_ILINK:
            wasLoaded = iLinkModLoaded;
            break;
        case BDM_TYPE_ATA:
            wasLoaded = hddModLoaded;
            break;
        default:
            SignalSema(bdmLoadModuleLock);
            return 0;
    }
    bdmEnsureTransportLoaded(bdmType);
    SignalSema(bdmLoadModuleLock);

    // Already loaded -> any connected device is already mounted, so the first probe returns with no stall
    // (and if nothing is connected we bail immediately). Just loaded -> poll until the IOP detects and
    // mounts the device, or timeoutMs elapses.
    start = GetTimerSystemTime();
    while (!bdmGetDeviceRootByType(bdmType, root, sizeof(root))) {
        if (wasLoaded)
            return 0;
        if ((GetTimerSystemTime() - start) / (kBUSCLK / 1000) > timeoutMs)
            return 0;
        DelayThread(100 * 1000);
    }
    return 1;
}

// True when the booted ELF's own folder exists on mass<slot>: -- the weak boot-device evidence.
static int bdmBootDirPresent(int slot, const char *tail)
{
    char path[288];
    int dir;

    if (tail[0] == '\0')
        snprintf(path, sizeof(path), "mass%d:/", slot);
    else if (tail[0] == '/')
        snprintf(path, sizeof(path), "mass%d:%s", slot, tail);
    else
        snprintf(path, sizeof(path), "mass%d:/%s", slot, tail);

    dir = fileXioDopen(path);
    if (dir < 0)
        return 0;
    fileXioDclose(dir);
    return 1;
}

// True when the booted ELF itself exists at <tail>/<elfName> on mass<slot>: -- the definitive
// boot-device evidence (OPL is running from that very file).
static int bdmBootElfPresent(int slot, const char *tail, const char *elfName)
{
    char path[320];
    int fd;

    if (tail[0] == '\0' || tail[0] == '/')
        snprintf(path, sizeof(path), "mass%d:%s/%s", slot, tail, elfName);
    else
        snprintf(path, sizeof(path), "mass%d:/%s/%s", slot, tail, elfName);

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

// Rewrite ONLY the device prefix of the boot dir, preserving the boot folder ("ata0:/APPS" ->
// "mass1:/APPS"). tail points INTO bootDir, so compose into a local buffer before writing back.
static void bdmRewriteBootDir(char *bootDir, int bootDirSize, int slot, const char *tail)
{
    char resolved[256];

    if (tail[0] == '\0' || tail[0] == '/')
        snprintf(resolved, sizeof(resolved), "mass%d:%s", slot, tail);
    else
        snprintf(resolved, sizeof(resolved), "mass%d:/%s", slot, tail);

    snprintf(bootDir, bootDirSize, "%s", resolved);
}

// Resolve a boot directory that names a BDM device into that device's mounted massN: filesystem root,
// force-loading whatever driver stack the boot device needs FIRST. Called from the deferred config
// load -- after ioInit()/bdmInitSemaphore(), before the first configReadMulti() -- because at boot time
// the IOP has NO BDM modules at all (sysReset loads none; the BDM page only loads them on tab entry),
// so a "mass0:/APPS" or "ata0:/APPS" boot dir is unreadable exactly when settings must load. Two input
// families are handled:
//   - launch-binding identities (usb0:/ilink0:/sd0:/mx4sio0:/sdc0:/ata0:) -- device names with no
//     filesystem OPL reads; remapped to the SAME device's massN: mount.
//   - massN:/mass: -- the right namespace, but the launcher's slot numbering need not match OPL's (and
//     the device isn't mounted yet); verified/renumbered by probing for the booted ELF itself.
// elfName (argv[0]'s basename; may be empty for a getcwd()-derived boot dir) anchors the verification:
// the slot that actually holds <tail>/<elfName> IS the boot device, immune to slot renumbering. The
// gEnable* config flags are deliberately ignored when loading transports -- the config is precisely
// what cannot be read yet (the exFAT-HDD chicken-and-egg: mounting it needs gEnableBdmHDD, which lives
// in the unreadable config); the caller reconciles the flags after the config loads.
// *ioBdmType is in/out: pass the boot device's BDM_TYPE_* when already known (the save-path re-resolve)
// to pin the search to that transport, or BDM_TYPE_UNKNOWN to classify from the prefix; on success it
// holds the resolved device's type.
// Returns 1 with bootDir rewritten in place on success; 0 when bootDir is not a BDM path (left
// untouched -- mc/mmce/host/pfs/hdd/... prefixes are not ours); -1 when the boot device never mounted
// within the budget (bootDir left untouched -- the caller drops it so legacy discovery re-arms).
int bdmResolveBootDir(char *bootDir, int bootDirSize, const char *elfName, int *ioBdmType)
{
    char stem[16];
    int unit;

    // *ioBdmType must never ERASE the caller's knowledge of the boot device's type -- the save-path
    // retry keys on it. It is written on success, and also on the -1 (never-mounted) paths with the
    // PREFIX CLASSIFICATION: that is not an erase (for an untyped massN: boot filterType IS knownType,
    // so it round-trips unchanged; for a typed usb0:/ata0:/... boot the prefix is authoritative). This
    // matters now that a failed resolve KEEPS the boot dir instead of dropping it to legacy discovery:
    // the caller zeroes this to UNKNOWN before every call, so without writing it back a slow-to-mount
    // boot device left gBootDirBdmType == UNKNOWN and _saveConfig's re-resolve-and-retry -- gated on
    // exactly that -- could never fire, so the settings never reached the device once it did mount
    // (CodeRabbit review of #202).
    int knownType = *ioBdmType;

    if (!bdmParseBootStem(bootDir, stem, sizeof(stem), &unit))
        return 0;

    int isMass = (strcmp(stem, "mass") == 0);
    int filterType = isMass ? knownType : bdmDetermineDeviceType(stem);
    if (!isMass && filterType == BDM_TYPE_UNKNOWN)
        return 0; // mc/mmce/host/pfs/hdd/cdrom/... -- not a BDM boot path, leave it alone
    if (filterType == BDM_TYPE_UDPBD) {
        *ioBdmType = filterType; // keep the classification so a later save retry stays pinned to it
        return -1;               // network block device: no local mount to resolve at config-load time
    }

    const char *tail = strchr(bootDir, ':') + 1; // "" | "/APPS" | "APPS" (bdmParseBootStem proved the ':')
    // The boot token's unit digit doubles as the scan-order hint for BOTH families: mass1: names the
    // slot outright, and a typed usb1:/ata1: unit usually tracks same-type enumeration order too.
    int literalSlot = (unit >= 0 && unit < MAX_BDM_DEVICES) ? unit : 0;
    int haveElf = (elfName != NULL && elfName[0] != '\0');

    // Bring-up: the BDM core (bdm.irx + bdmfs_fatfs + hotplug events; idempotent -- every bdmInit calls
    // it too) plus the transport(s) the boot prefix implies. An untyped massN: names the shared BDM
    // filesystem, not a transport, so start with the cheap common ones (USB, MX4SIO) and only escalate
    // to iLink + the ATA stack (iLinkman is known to break PCSX2, where a virtual-USB mass: boot is
    // possible; ATA means dev9 power + drive spin-up) if nothing turns up holding the boot folder.
    bdmLoadModules();
    WaitSema(bdmLoadModuleLock);
    if (filterType != BDM_TYPE_UNKNOWN) {
        bdmEnsureTransportLoaded(filterType);
    } else {
        bdmEnsureTransportLoaded(BDM_TYPE_USB);
        bdmEnsureTransportLoaded(BDM_TYPE_SDC);
    }
    SignalSema(bdmLoadModuleLock);

    u32 budgetMs = (filterType == BDM_TYPE_ATA) ? 10000 : 3000; // ATA = dev9 power-up + platter spin-up
    int escalated = (filterType != BDM_TYPE_UNKNOWN);           // typed searches never escalate
    // u64: GetTimerSystemTime() passes 2^32 bus ticks ~29 s after boot; a u32 start would make every
    // POST-BOOT resolve (the _saveConfig hotplug retry, a settings reload) compute a bogus huge elapsed
    // and give up after a single scan instead of waiting the budget out.
    u64 start = GetTimerSystemTime();

    for (;;) {
        int matchSlot = -1, fallbackSlot = -1;
        int matchType = BDM_TYPE_UNKNOWN, fallbackType = BDM_TYPE_UNKNOWN;

        for (int scan = 0; scan < MAX_BDM_DEVICES; scan++) {
            // Probe the boot dir's LITERAL slot first, then the rest in order: with duplicate OPL
            // installs on two same-type devices the ELF probe matches both, and first-match-wins
            // must prefer the slot the launcher actually named over an arbitrary lower one.
            int i = (scan == 0) ? literalSlot : (scan <= literalSlot ? scan - 1 : scan);
            char path[16], driver[32];
            int devIndex = -1;

            snprintf(path, sizeof(path), "mass%d:/", i);
            bdmReadDeviceIdentity(path, driver, sizeof(driver), &devIndex);
            if (driver[0] == '\0')
                continue; // slot not mounted (yet)

            int slotType = bdmDetermineDeviceType(driver);
            if (filterType != BDM_TYPE_UNKNOWN && slotType != filterType)
                continue; // wrong device family for a typed/pinned search

            if (haveElf && bdmBootElfPresent(i, tail, elfName)) {
                matchSlot = i; // definitive: the ELF we are running from is right here
                matchType = slotType;
                break;
            }
            int dirOk = bdmBootDirPresent(i, tail);
            if (!haveElf && dirOk && (!isMass || i == literalSlot)) {
                matchSlot = i; // getcwd-derived boot dir: folder presence is the best evidence there is
                matchType = slotType;
                break;
            }
            if (fallbackSlot < 0 && dirOk && (!isMass || i == literalSlot)) {
                fallbackSlot = i; // boot folder exists but the ELF probe missed -- hold as weak fallback
                fallbackType = slotType;
            }
        }

        if (matchSlot >= 0) {
            *ioBdmType = matchType;
            bdmRewriteBootDir(bootDir, bootDirSize, matchSlot, tail);
            return 1;
        }

        if ((GetTimerSystemTime() - start) / (kBUSCLK / 1000) > budgetMs) {
            if (fallbackSlot >= 0) {
                *ioBdmType = fallbackType;
                bdmRewriteBootDir(bootDir, bootDirSize, fallbackSlot, tail);
                return 1;
            }
            if (!escalated) {
                // Nothing cheap matched a massN: boot dir -- the boot device may be the internal exFAT
                // HDD handed in massN: form (HDD-OSD launchers do this) or an iLink drive. One
                // escalation: the remaining transports, with extra budget for ATA spin-up.
                escalated = 1;
                WaitSema(bdmLoadModuleLock);
                bdmEnsureTransportLoaded(BDM_TYPE_ILINK);
                bdmEnsureTransportLoaded(BDM_TYPE_ATA);
                SignalSema(bdmLoadModuleLock);
                budgetMs += 8000;
                continue;
            }
            *ioBdmType = filterType; // never mounted in time, but keep the classification: the caller
                                     // now KEEPS the boot dir, and _saveConfig's retry is gated on this
                                     // being != UNKNOWN (see the header comment)
            return -1;
        }
        DelayThread(100 * 1000);
    }
}

static int bdmDeviceIsATA(int deviceId)
{
    char path[16];
    bdm_device_data_t data;

    sprintf(path, "mass%d:/", deviceId);
    int dir = fileXioDopen(path);
    if (dir < 0)
        return 0;

    /* Zero the buffer before the ioctl: if the call fails, strcmp would read
     * uninitialised stack bytes. Pattern mirrors bdmReadDeviceIdentity(). */
    memset(&data.bdmDriver, 0, sizeof(data.bdmDriver));
    if (fileXioIoctl2(dir, USBMASS_IOCTL_GET_DRIVERNAME, NULL, 0, &data.bdmDriver, sizeof(data.bdmDriver) - 1) < 0) {
        fileXioDclose(dir);
        return 0;
    }
    fileXioDclose(dir);

    return (!strcmp(data.bdmDriver, "ata") && strlen(data.bdmDriver) == 3);
}

static int bdmGetATADeviceId()
{
    for (int i = 0; i < MAX_BDM_DEVICES; i++) {
        if (bdmDeviceIsATA(i)) {
            return i;
        }
    }
    return -1;
}

int bdmHDDIsPresent(u32 timeoutMs)
{
    int hdd_id = -1;
    int timedout = 0;

    if (!hddIsPresent())
        return 0;

    // 1. scan via normal methods first...
    hdd_id = bdmGetATADeviceId();
    if (hdd_id >= 0)
        return 1;

    // 2. try to scan as fast as possible if the previous scan fails...
    hdd_id = -1;

    for (int i = 0; i < MAX_BDM_DEVICES; i++) {
        // find the first inaccessible device - this one should be the HDD once it's mounted (we don't have access to device data at this point yet!)
        if (!bdmDeviceIsPresent(i)) {
            hdd_id = i;
            break;
        }
    }

    if (hdd_id < 0) {
        // All slots are already accessible; no inaccessible slot to poll.
        // Skip the targeted wait and rescan immediately.
        timedout = 1;
    } else if (bdmWaitForDevice(hdd_id, timeoutMs)) {
        // double-check to see if this indeed is the HDD, and if it is, we can exit early without stalling any further
        if (bdmDeviceIsATA(hdd_id)) {
            return 1;
        } else
            LOG("bdmHDDIsPresent: device at id %d is not an ATA HDD...\n", hdd_id);
    } else {
        timedout = 1;
        LOG("bdmHDDIsPresent: waiting for hdd at id %d timed out...\n", hdd_id);
    }

    // 3. last resort - time out (if needed) and scan again...
    if (!timedout) {
        // if we haven't timed out already, then we need to wait for the devices to wake up... wait for the timeout...
        LOG("bdmHDDIsPresent: waiting for timeout before scanning again (id %d)...\n", hdd_id);
        DelayThread(timeoutMs * 1000);
    }

    return bdmGetATADeviceId() >= 0;
}


int bdmFindPartition(char *target, const char *name, int write)
{
    int i, fd;
    char path[256];

    /* Probe massN:/<prefix>/<name> on each BDM device for the config file, and
     * return the matching device directory prefix in target so configInit()/
     * configSetMove() can compose "<target>/<filename>".  The format strings
     * MUST carry %d/%s specifiers: the previous version used literal "mass0:/"
     * strings with leftover varargs, which the compiler silently discards, so
     * the loop never iterated devices and the prefix/filename were never
     * appended (BDM per-device config load/save was non-functional and only
     * ever touched mass0's root).  gBDMPrefix is a bounded folder name
     * (char[32]) and target buffers are 64 bytes, so the device-prefix writes
     * cannot overflow. */
    for (i = 0; i < MAX_BDM_DEVICES; i++) {
        if (gBDMPrefix[0] != '\0')
            snprintf(path, sizeof(path), "mass%d:/%s/%s", i, gBDMPrefix, name);
        else
            snprintf(path, sizeof(path), "mass%d:/%s", i, name);
        if (write) {
            /* O_CREAT makes the config FILE but never the gBDMPrefix FOLDER.  On a freshly
             * formatted drive (e.g. an exFAT HDD whose OPL folder doesn't exist yet) the very
             * first save would therefore fail with the folder absent, and only start working
             * once sbCreateFolders() happened to create it on a later device refresh -- exactly
             * the "save errored, then saved fine after a try or two" symptom.  Create the prefix
             * directory up front (mkdir is a harmless no-op when it already exists) so the first
             * save succeeds.  Probe with O_CREAT but WITHOUT O_TRUNC: this open only tests for a
             * writable device, so it must never truncate an existing config -- configWrite()
             * truncates when it actually commits the data. */
            if (gBDMPrefix[0] != '\0') {
                char dir[256];
                snprintf(dir, sizeof(dir), "mass%d:/%s", i, gBDMPrefix);
                mkdir(dir, 0777);
            }
            fd = open(path, O_WRONLY | O_CREAT, 0666);
        } else
            fd = open(path, O_RDONLY);

        if (fd >= 0) {
            if (gBDMPrefix[0] != '\0')
                sprintf(target, "mass%d:/%s", i, gBDMPrefix);
            else
                sprintf(target, "mass%d:", i);
            close(fd);
            return 1;
        }
    }

    if (gBDMPrefix[0] != '\0')
        sprintf(target, "mass0:/%s", gBDMPrefix);
    else
        sprintf(target, "mass0:");
    return 0;
}
