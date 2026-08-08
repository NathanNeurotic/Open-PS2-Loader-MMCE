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
    int bdmVcdGameCount;
    base_game_info_t *bdmVcdGames;
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
// Returns 1 with bootDir rewritten in place, 0 when bootDir is not a BDM path (untouched), -1 when the
// boot device never mounted in time (untouched -- caller drops it so legacy discovery re-arms).
int bdmResolveBootDir(char *bootDir, int bootDirSize, const char *elfName, int *ioBdmType);

int bdmFindPartition(char *target, const char *name, int write);
int bdmIsUDPBDLoaded(void); // 1 if the UDPBD NIC stack is loaded (the SMB stack must not load on top)
// Which network block transport is actually resident (NET_BOOT_UDPBD / NET_BOOT_UDPFS). Use this,
// not gNetBootProtocol, for anything DESCRIBING the live device -- the picker can change without a
// reboot while the loaded IRX cannot.
int bdmGetLoadedNetProtocol(void);
int bdmSupportIsUDPBD(item_list_t *support); // 1 if this support is the UDPBD block device (its games are Neutrino-only)

// Re-evaluate every BDM device's presence + page visibility on the next refresh (bumps the latch
// generation). Call after a device-enable toggle so a latched-hidden tab re-shows without a replug.
void bdmForceDeviceRefresh(void);
// Effective BDM start mode: floors to AUTO while a BDM network transport (UDPBD/UDPFSBD) is the
// selected protocol so its hotplug tab can exist; never modifies/persists the saved gBDMStartMode.
int bdmEffectiveStartMode(void);
// Current BDM device-change generation (bumped on hotplug / Device-Settings apply). The menu hook
// reads this to bypass its background SIO2 rescan throttle when a real device change occurs.
unsigned int bdmGetGeneration(void);
#endif
