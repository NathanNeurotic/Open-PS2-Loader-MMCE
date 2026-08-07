#ifndef __BDM_SUPPORT_H
#define __BDM_SUPPORT_H

#include "include/iosupport.h"

#define BDM_MODE_UPDATE_DELAY MENU_UPD_DELAY_GENREFRESH

#include "include/mcemu.h"

typedef struct
{
    int active;       /* Activation flag */
    u64 start_sector; /* Start sector of vmc file */
    int flags;        /* Card flag */
    vmc_spec_t specs; /* Card specifications */
} bdm_vmc_infos_t;

#define MAX_BDM_DEVICES     5
#define BDM_DEVICE_ROOT_MAX 32

#define BDM_TYPE_UNKNOWN -1
#define BDM_TYPE_USB     0
#define BDM_TYPE_ILINK   1
#define BDM_TYPE_SDC     2
#define BDM_TYPE_ATA     3

typedef struct
{
    int massDeviceIndex; // Underlying device index backing the mass fs partition, ex: usb0 = 0, usb1 = 1, etc.
    char bdmPrefix[40];  // Contains the full path to the folder where all the games are.
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
    unsigned char ForceRefresh;
} bdm_device_data_t;

void bdmInit(item_list_t *itemList);
int bdmFindPartition(char *target, const char *name, int write);
void bdmLoadModules(void);
void bdmLaunchGame(item_list_t *itemList, int id, config_set_t *configSet);

void bdmInitSemaphore();
void bdmEnumerateDevices();

void bdmResolveLBA_UDMA(bdm_device_data_t *pDeviceData);
int bdmHDDIsPresent(u32 timeoutMs);

// First mounted device of a BDM_TYPE_*: writes its massN:/ root; 1 = found.
int bdmGetDeviceRootByType(int bdmType, char *root, int rootLen);

int bdmSupportIsUDPBD(item_list_t *support); // 1 if this support is the UDPBD block device (Neutrino-only games)

int bdmEnsureSourceModules(int bdmType, u32 timeoutMs); // force-load a source transport + wait for a mount
int bdmGetDeviceSlotsByType(int bdmType, int *slots, int maxSlots); // every mounted slot of a type

#endif
