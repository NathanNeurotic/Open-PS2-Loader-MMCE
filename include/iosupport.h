#ifndef __IOSUPPORT_H
#define __IOSUPPORT_H

#include "include/config.h"

#define IO_MODE_SELECTED_NONE -1
#define IO_MODE_SELECTED_ALL  MODE_COUNT

enum IO_MODES {
    BDM_MODE = 0,
    BDM_MODE1,
    BDM_MODE2,
    BDM_MODE3,
    BDM_MODE4,
    BDM_MODE5,
    BDM_MODE6,
    BDM_MODE7,
    ETH_MODE,
    HDD_MODE,
    APP_MODE,
    MMCE_MODE,  // reserved: MMCE support returns with checklist item 1
    FAV_MODE,   // reserved: Favourites returns with checklist item 33
    UDPFS_MODE, // reserved (item 6); appended last so existing mode values don't shift

    MODE_COUNT
};

#define BDM_MODE_LAST  BDM_MODE7
#define BDM_MODE_COUNT (BDM_MODE_LAST - BDM_MODE + 1)

enum START_MODE {
    START_MODE_DISABLED = 0,
    START_MODE_MANUAL,
    START_MODE_AUTO
};

enum ERROR_CODE {
    // Generic error codes
    ERROR_ETH_NOT_STARTED = 100,

    // Ethernet (module startup) error codes
    ERROR_ETH_MODULE_NETIF_FAILURE = 200,
    ERROR_ETH_MODULE_SMBMAN_FAILURE,
    // HDD (module startup) error codes
    ERROR_HDD_MODULE_ATAD_FAILURE = 220,
    ERROR_HDD_MODULE_HDD_FAILURE,
    ERROR_HDD_MODULE_PFS_FAILURE,

    // Ethernet (software) error codes
    ERROR_ETH_SMB_CONN = 300,
    ERROR_ETH_SMB_LOGON,
    ERROR_ETH_SMB_ECHO,
    ERROR_ETH_SMB_OPENSHARE,
    ERROR_ETH_SMB_LISTSHARES,
    ERROR_ETH_SMB_LISTGAMES,
    // Ethernet (hardware) error codes
    ERROR_ETH_LINK_FAIL = 310,
    ERROR_ETH_DHCP_FAIL,

    // HDD error codes
    ERROR_HDD_IF_NOT_DETECTED = 400,
    ERROR_HDD_NOT_DETECTED,
};

#define NO_EXCEPTION      0x00
#define UNMOUNT_EXCEPTION 0x01

#define MODE_FLAG_NO_COMPAT  0x01 // no compat support
#define MODE_FLAG_COMPAT_DMA 0x02 // Supports DMA compat flags
#define MODE_FLAG_NO_UPDATE  0x04 // Network update not supported.

#define COMPAT_MODE_1 0x01 // Accurate Reads
#define COMPAT_MODE_2 0x02 // Alternative data read method (Synchronous)
#define COMPAT_MODE_3 0x04 // Unhook Syscalls
#define COMPAT_MODE_4 0x08 // Skip Videos: Apply 0 (zero) file size to PSS videos and also skip Bink (.BIK) ones
#define COMPAT_MODE_5 0x10 // Emulate DVD-DL
#define COMPAT_MODE_6 0x20 // Disable IGR
#define COMPAT_MODE_7 0x40 // Unused
#define COMPAT_MODE_8 0x80 // Unused

#define COMPAT_MODE_COUNT 6 // only count modes in use

#define OPL_MOD_STORAGE 0x00097000 //(default) Address of the module storage region

// minimal inactive frames for cover display, can be pretty low since it means no button is pressed :)
#define MENU_MIN_INACTIVE_FRAMES 8

#define MENU_UPD_DELAY_NOUPDATE   -1 // Auto refresh is disabled for the item. The refresh button may be used to manually refresh the item.
#define MENU_UPD_DELAY_GENREFRESH 0  // The item will be refreshed every MENU_GENERAL_UPDATE_DELAY frames, regardless of whether automatic refresh is enabled or not.

typedef struct _item_list_t item_list_t;

typedef struct _item_list_t
{
    short int mode;

    /// Device priority when it comes to locating art assets for apps. Higher value = lower priority. (< 0) means no support for art assets.
    char appsPriority;

    char enabled;

    unsigned char flags;

    /// max inactive frame delay
    int delay;

    /// Amount of frame to wait, before refreshing this menu's list. Setting an invalid value (<0) means no automatic refresh.
    /// 0 = General refresh, which means that it will be refreshed every MENU_GENERAL_UPDATE_DELAY frames, regardless of whether automatic refresh is enabled or not.
    int updateDelay;

    // Per-device data
    void *priv;

    // opl_io_module_t instance that owns this item list.
    void *owner;

    /// item description in localised form (used if value is not negative)
    int (*itemTextId)(item_list_t *itemList);

    /// @return path to device prefix (set callback to NULL if not applicable).
    char *(*itemGetPrefix)(item_list_t *itemList);

    void (*itemInit)(item_list_t *itemList);

    /** @return 1 if update is needed, 0 otherwise */
    int (*itemNeedsUpdate)(item_list_t *itemList);

    /** @return game count (0 on error) */
    int (*itemUpdate)(item_list_t *itemList);

    int (*itemGetCount)(item_list_t *itemList);

    void *(*itemGet)(item_list_t *itemList, int id);

    char *(*itemGetName)(item_list_t *itemList, int id);

    int (*itemGetNameLength)(item_list_t *itemList, int id);

    char *(*itemGetStartup)(item_list_t *itemList, int id);

    void (*itemDelete)(item_list_t *itemList, int id);

    void (*itemRename)(item_list_t *itemList, int id, char *newName);

    void (*itemLaunch)(item_list_t *itemList, int id, config_set_t *configSet);

    config_set_t *(*itemGetConfig)(item_list_t *itemList, int id);

    int (*itemGetImage)(item_list_t *itemList, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm);

    void (*itemCleanUp)(item_list_t *itemList, int exception);

    void (*itemShutdown)(item_list_t *itemList);

    int (*itemCheckVMC)(item_list_t *itemList, char *name, int createSize);

    int (*itemIconId)(item_list_t *itemList);

    /// Launch a VCD (PS1/POPSTARTER) item by its stored name, regardless of the device's current
    /// view. NULL for devices without a VCD view (checklist item 12). Used by the Favourites tab to
    /// launch a VCD favourite while its source device page may be in ISO view.
    void (*itemLaunchVcd)(item_list_t *itemList, const char *vcdName, config_set_t *configSet);

    /// Optional view override for a shallow proxy of this support. Zero keeps the support's native
    /// per-mode L3 state; Favourites uses the forced values so an ISO/VCD favourite can proxy its
    /// source without mutating the real source page's independent view. Appended last so all legacy
    /// positional item_list_t initializers default safely to native behavior.
    unsigned char viewOverride;

    /// Resolve this item's ART/art.tar to one exact path. Return >0 after writing that path, 0 to
    /// retain the generic archive lookup, or <0 to skip archive probing entirely. APA uses this to
    /// keep artwork on its already-selected PFS data home.
    int (*itemGetArtArchivePath)(item_list_t *itemList, const char *value, char *path, int pathSize);
} item_list_t;

#define ITEM_VIEW_NATIVE    0
#define ITEM_VIEW_FORCE_ISO 1
#define ITEM_VIEW_FORCE_VCD 2

#endif
