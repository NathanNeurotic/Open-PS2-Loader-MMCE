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
    char bdmDriver[32];
    int bdmDeviceType;      // Type of BDM device, see BDM_TYPE_* above
    int bdmDeviceTick;      // Used alongside BdmGeneration to tell if device data needs to be refreshed
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
int bdmWaitForDevice(int deviceId, u32 timeoutMs);
int bdmHDDIsPresent(u32 timeoutMs);
int bdmResolveDeviceRoot(char *target, int targetLength, const char *driverName, int massDeviceIndex, int massSlot);

int bdmFindPartition(char *target, const char *name, int write);
#endif
