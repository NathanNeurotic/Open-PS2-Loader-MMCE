#ifndef __HDD_SUPPORT_H
#define __HDD_SUPPORT_H

#include "include/iosupport.h"
#include "include/hdd.h"

#define HDD_MODE_UPDATE_DELAY MENU_UPD_DELAY_NOUPDATE

#define HDL_GAME_NAME_MAX 64

// APA Partition
#define APA_IOCTL2_GETHEADER 0x6836

typedef struct
{
    char partition_name[APA_IDMAX + 1];
    char name[HDL_GAME_NAME_MAX + 1];
    char startup[8 + 1 + 3 + 1];
    u8 hdl_compat_flags;
    u8 ops2l_compat_flags;
    u8 dma_type;
    u8 dma_mode;
    u8 disctype;
    u32 layer_break;
    u32 start_sector;
    u32 total_size_in_kb;
} hdl_game_info_t;

typedef struct
{
    u32 count;
    hdl_game_info_t *games;
} hdl_games_list_t;

// HDD PS1/VCD sources: exact __.POPS / __.POPS0..9 containers (many named .VCD files) plus
// partition-installed PP.<name> / __.<name> games (one root IMAGE0.VCD). This is the sorted,
// deduped list of matching main PFS partitions enumerated from the APA partition table.
typedef struct
{
    int count;
    char (*names)[APA_IDMAX + 1]; // malloc'd labels, e.g. "__.POPS3", "PP.Game", "__.Hidden"
} hdd_pops_list_t;

typedef struct
{
    u32 start;
    u32 length;
} apa_subs;

#include "include/mcemu.h"
typedef struct
{
    int active;                 /* Activation flag */
    apa_subs parts[5];          /* Vmc file Apa partitions */
    pfs_blockinfo_t blocks[10]; /* Vmc file Pfs inode */
    int flags;                  /* Card flag */
    vmc_spec_t specs;           /* Card specifications */
} hdd_vmc_infos_t;

typedef enum {
    HDD_LOADMODULES_STATUS_ERROR = -2,
    HDD_LOADMODULES_STATUS_UNK = -1,
    HDD_LOADMODULES_STATUS_NOERROR,
    HDD_LOADMODULES_STATUS_ALREADYLOADED,
    HDD_LOADMODULES_STATUS_BUSYLOADING,
    HDD_LOADMODULES_STATUS_COUNT,
} hdd_loadmodules_status;

// See hddLoadModulesReady() below the prototypes -- the single evaluate-once way to ask "is the ATA
// stack actually resident?". Callers used to test `hddLoadModules() >= 0`, which also accepted
// BUSYLOADING(2) -- returned for the whole session after a FAILED first load consumed the one-shot
// count -- so "nothing is loaded" read as success and the APA page sat silently empty under both
// Auto and Manual (Vapor). Pair with the retryable-failure change in hddLoadModules.

int hddCheck(void);
u32 hddGetTotalSectors(void);
int hddIs48bit(void);
int hddSetTransferMode(int type, int mode);
void hddSetIdleTimeout(int timeout);
void hddSetIdleImmediate(void);
int hddGetHDLGamelist(hdl_games_list_t *game_list);
void hddFreeHDLGamelist(hdl_games_list_t *game_list);
// True for a one-game PP.<name> / __.<name> partition label, excluding the exact __.POPS[0-9]?
// container names. Matching is case-sensitive, like POPSTARTER's partition selector.
int hddIsPopsPartitionGame(const char *name);
// Enumerate the present classic-container and one-game APA/PFS partitions. Fills a sorted, deduped
// list; returns the count (0 on none/error). Free via hddFreePopsPartitionList.
int hddGetPopsPartitionList(hdd_pops_list_t *list);
void hddFreePopsPartitionList(hdd_pops_list_t *list);
int hddSetHDLGameInfo(hdl_game_info_t *ginfo);
int hddDeleteHDLGame(hdl_game_info_t *ginfo);

// Drop the once-per-session HDD VCD list cache so the next VCD-view update re-walks the partitions.
// Needed only when a SCAN-TIME filter changes (gVcdFirstDiscOnly); view flips reuse the built list.
void hddVcdInvalidateCache(void);

void hddInit(item_list_t *itemList);
item_list_t *hddGetObject(int initOnly);
int hddLoadModules(void);

// Load (or confirm) the ATA stack and report residency, evaluating hddLoadModules EXACTLY ONCE.
// A function, not a macro taking the call as its argument: the earlier HDD_LOADMODULES_OK(
// hddLoadModules()) macro double-evaluated on any non-NOERROR first result -- and with the
// retryable-failure change, a FAILED load would have immediately re-run the whole load (second
// dev9 init + second error toast) inside one condition (CodeRabbit review of #241; vetted).
static inline int hddLoadModulesReady(void)
{
    int r = hddLoadModules();
    return r == HDD_LOADMODULES_STATUS_NOERROR || r == HDD_LOADMODULES_STATUS_ALREADYLOADED;
}
void hddLoadSupportModules(void);
void hddLaunchGame(item_list_t *itemList, int id, config_set_t *configSet);
int hddIsPresent();

#endif
