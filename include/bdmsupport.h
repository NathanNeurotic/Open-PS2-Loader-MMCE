#ifndef __BDM_SUPPORT_H
#define __BDM_SUPPORT_H

#include "include/iosupport.h"

#define BDM_MODE_UPDATE_DELAY MENU_UPD_DELAY_GENREFRESH

#include "include/mcemu.h"

#define BDM_DEVICE_ROOT_MAX 32
#define BDM_PREFIX_MAX      96

typedef struct
{
    int active;       /* Activation flag */
    u64 start_sector; /* Start sector of vmc file */
    int flags;        /* Card flag */
    vmc_spec_t specs; /* Card specifications */
} bdm_vmc_infos_t;

#define MAX_BDM_DEVICES BDM_MODE_COUNT

#define BDM_TYPE_UNKNOWN -1
#define BDM_TYPE_USB     0
#define BDM_TYPE_ILINK   1
#define BDM_TYPE_SDC     2
#define BDM_TYPE_ATA     3
#define BDM_TYPE_UDPBD   4

// Network-backed BDM devices (UDPBD/UDPFS) open over the wire, so a single failed presence poll is
// usually a transient stall, not a real removal. Debounce: hide a NETWORK page only after this many
// consecutive failed ~1s polls (local USB/SDC/ATA still hide on the first miss = an actual unplug).
#define BDM_NET_HIDE_MISSES 5

typedef struct
{
    int massDeviceIndex;                     // Underlying device index backing the block device. This is not the same as the typed-path unit.
    char bdmDeviceRoot[BDM_DEVICE_ROOT_MAX]; // Device root used for filesystem access, currently the massN: compatibility root.
    char bdmPrefix[BDM_PREFIX_MAX];          // Full path to the folder where all the games are.
    int bdmULSizePrev;
    time_t bdmModifiedCDPrev;
    time_t bdmModifiedDVDPrev;
    int bdmGameCount;
    base_game_info_t *bdmGames;
    // #120: the PS2 (ISO) and PS1 (VCD) views must NOT share one backing store -- BDM was the last
    // device still doing so (MMCE/HDD were split long ago). With one array, a VCD scan that returns
    // "failure" left bdmGameCount untouched while the array still held the ISO list, so the VCD view
    // re-published the ISO games and the L3 toggle looked dead ("the list never changes" -- Nathan, HW,
    // ATA/HDD_BD). A device with NO POPS folder hits that EVERY time: vcdScanOpenDir cannot tell an
    // absent dir from a contended one (opendir just fails) and returns -1 = preserve-last-good. Separate
    // arrays make a failed scan of one view preserve only THAT view's last-good (empty if never scanned),
    // so it can never resurrect the other view's contents. Mirrors mmceGames/mmceVcdGames.
    int bdmPs1GameCount;
    base_game_info_t *bdmPs1Games;
    char bdmDriver[32];
    int bdmDeviceType;      // Type of BDM device, see BDM_TYPE_* above
    int bdmDeviceTick;      // Used alongside BdmGeneration to tell if device data needs to be refreshed
    int bdmMissCount;       // Consecutive failed presence polls; debounces hiding a network page (BDM_NET_HIDE_MISSES)
    int bdmHddIsLBA48;      // 1 if the HDD supports LBA48, 0 if the HDD only supports LBA28
    int ataHighestUDMAMode; // Highest UDMA mode supported by the HDD
    unsigned char ThemesLoaded;
    unsigned char LanguagesLoaded;
    unsigned char FoldersCreated;
    unsigned char ForceRefresh;
    // Set the first time this slot's massN: root answers Dopen, INDEPENDENTLY of whether the
    // identity ioctl has answered yet. bdmPrefix cannot serve this purpose: it is only written once
    // identity succeeds, so a mounted-but-unidentified slot would read as "never connected" and be
    // demoted to the slow empty-slot probe rotation -- exactly the slot that needs the full cadence.
    unsigned char bdmSeenMounted;
} bdm_device_data_t;

void bdmLoadModules(void);
void bdmLaunchGame(item_list_t *itemList, int id, config_set_t *configSet);

void bdmInitSemaphore();
void bdmEnumerateDevices();

void bdmResolveLBA_UDMA(bdm_device_data_t *pDeviceData);
int bdmHDDIsPresent(u32 timeoutMs);
// Find the first mounted BDM device whose driver matches bdmType (BDM_TYPE_*); write its massN:
// filesystem root with a trailing slash (e.g. "mass0:/") to root. Returns 1 if found. Mount-readiness
// check. Always the legacy massN: mount, never a typed ata0:/usb0: root -- newer SDKs register typed
// roots as real filesystems, but every consumer of this path (POPSTARTER, the BDMA equip, the boot-dir
// resolver, the Neutrino pickers) must stay on massN:.
int bdmGetDeviceRootByType(int bdmType, char *root, int rootLen);
// Fill `slots` with the massN: slot index of EVERY mounted device whose driver matches bdmType (root =
// "mass<i>:/"), up to maxSlots; returns the count. The BDMA equip searches all same-type slots so a
// source family with two same-type devices is covered when the files sit on the second.
int bdmGetDeviceSlotsByType(int bdmType, int *slots, int maxSlots);
// Force-load the BDM transport for bdmType (even if its games toggle is off) and wait up to timeoutMs
// for a device of that type to mount, so the BDMA equip can read a source device that isn't enabled for
// games. Returns 1 if a device is present afterwards, 0 otherwise. Idempotent + instant when already up.
int bdmEnsureSourceModules(int bdmType, u32 timeoutMs);
// Resolve a boot directory that names a BDM device ("ata0:/APPS", "usb0:/...", "mass0:/APPS", "mass:")
// to the device's mounted massN: root, force-loading the needed driver stack first (the gEnable* config
// gates are ignored -- the config is what cannot be read until this succeeds). elfName (argv[0]'s
// basename; may be empty) verifies the slot: the device holding <bootdir>/<elfName> IS the boot device.
// *ioBdmType: pass the known boot-device BDM_TYPE_* to pin the search (save-path re-resolve) or
// BDM_TYPE_UNKNOWN to classify from the prefix; on success it returns the resolved device's type.
// Returns 1 with bootDir rewritten in place (or confirmed unchanged for an explicit massN: slot),
// 0 when bootDir is not a BDM path (untouched), -1 when the boot device never mounted in time
// (untouched; callers keep that explicit identity and can fall back to defaults).
int bdmResolveBootDir(char *bootDir, int bootDirSize, const char *elfName, int *ioBdmType);
// Bootstrap variant for the first config read. An explicit massN: is identified only by opening
// that exact slot and reading its driver/device ioctls; it never probes other mass slots. Its
// bounded USB -> MX4SIO -> iLink/ATA transport bring-up prevents a normal first-run boot from
// spending the full recovery budget before defaults are shown. Explicit custom paths and save-time
// re-resolution use bdmResolveBootDir() above.
int bdmResolveBootDirBootstrap(char *bootDir, int bootDirSize, const char *elfName, int *ioBdmType);

int bdmFindPartition(char *target, const char *name, int write);
int bdmIsUDPBDLoaded(void); // 1 if the UDPBD NIC stack is loaded (the SMB stack must not load on top)
// Which network block transport is actually resident (NET_BOOT_UDPBD / NET_BOOT_UDPFS). Use this,
// not gNetBootProtocol, for anything DESCRIBING the live device -- the picker can change without a
// reboot while the loaded IRX cannot.
int bdmGetLoadedNetProtocol(void);
int bdmSupportIsUDPBD(const item_list_t *support); // 1 if this support is the UDPBD block device (its games are Neutrino-only)
int bdmModeIsUDPBD(int mode);                // 1 if this BDM mode slot is the UDPBD block device

// Re-evaluate every BDM device's presence + page visibility on the next refresh (bumps the latch
// generation). Call after a device-enable toggle so a latched-hidden tab re-shows without a replug.
void bdmForceDeviceRefresh(void);
// Effective BDM start mode: floors to AUTO while a BDM network transport (UDPBD/UDPFSBD) is the
// selected protocol so its hotplug tab can exist; never modifies/persists the saved gBDMStartMode.
int bdmEffectiveStartMode(void);
// Current BDM device-change generation (bumped on hotplug / Device-Settings apply). The menu hook
// reads this to bypass its background SIO2 rescan throttle when a real device change occurs.
unsigned int bdmGetGeneration(void);

// Diagnostic-only hooks used by the generic game-settings save worker. They are no-ops for
// non-BDM lists and never change which filesystem path configWrite() consumes.
//
// The reporting functions probe every BDM device root, which is blocking device I/O, so they must
// run with NO menu lock held. That in turn means they cannot borrow the caller's item_list_t /
// config_set_t: once the lock is released those may be freed underneath us. So the caller captures
// every value the report needs into this plain-value snapshot WHILE the lock is held, then releases
// the lock and reports from the copy. Nothing here is a pointer that gets dereferenced -- the two
// address fields are carried for %p identity only.
typedef struct
{
    int valid; // 0 = not a BDM list; the reporting calls do nothing
    int slot;
    int mode;
    int visible;
    unsigned int generation;
    const void *itemListAddr; // logged as %p, NEVER dereferenced
    const void *configAddr;   // logged as %p, NEVER dereferenced
    unsigned int configUid;
    int configModified;
    int configFormat;
    char configFilename[128];
    char deviceRoot[BDM_DEVICE_ROOT_MAX];
    char devicePrefix[BDM_PREFIX_MAX];
    char driver[32];
    int massDeviceIndex;
    int bdmDeviceType;
    unsigned int foldersCreated;
    const void *games; // %p only
    int gameCount;
    const void *ps1Games; // %p only
    int ps1GameCount;
} bdm_config_diag_snapshot_t;

// Call UNDER the menu lock. Pure value copies, no I/O.
void bdmCaptureConfigWriteDiag(item_list_t *itemList, const config_set_t *configSet, bdm_config_diag_snapshot_t *out);
// Call with NO lock held. These perform the device probes.
void bdmLogConfigWriteEntry(const bdm_config_diag_snapshot_t *snap);
void bdmLogConfigWriteResult(const bdm_config_diag_snapshot_t *snap, int result);

/** Nonzero if this BDM slot has had a device on it at some point this session (its prefix is
 * populated). Slots that have never connected are probed on a slow rotation instead of on every
 * background rescan -- attach is event-driven, so the periodic probe is only a missed-event net.
 */
int bdmSlotEverConnected(int mode);

/** Read a BDM device's driver name from an already-open massN: directory fd.
 *
 * ⚠ ALWAYS USE THIS instead of issuing USBMASS_IOCTL_GET_DRIVERNAME with a return buffer yourself.
 * ps2sdk's bdmfs_fatfs handler dereferences NULL when a buffer is supplied for a volume that has no
 * mounted block device, which faults the IOP and takes down every thread waiting on it. The
 * implementation asks without a buffer first, which is the form that fails safely.
 *
 * Returns >= 0 on success. driverName is always left NUL-terminated.
 */
int bdmReadDriverName(int dir, char *driverName, int driverNameLength);

/** True when this BDM slot is the MX4SIO (SD-over-SIO2) device.
 *
 * SIO2 is the CONTROLLER'S OWN BUS. An art read here contends with pad polling, and that contention
 * IS issue #340's mechanism -- inherent to the transport, not something we can code away
 * (hardware-confirmed 2026-08-12: enabling MX4SIO alone reintroduces input skipping, via the chain
 * documented at src/pad.c:108-125). texcache uses this to refuse to START such a read while a
 * direction is held, and to drop the art thread's priority for the duration of one.
 *
 * Answer it on the GUI thread at enqueue time, never on the art worker: it reads the support's
 * private bdm_device_data_t, which a background rescan rewrites.
 */
int bdmModeIsSIO2(int mode);
#endif
