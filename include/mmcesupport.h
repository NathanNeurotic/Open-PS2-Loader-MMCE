#ifndef __MMCE_SUPPORT_H
#define __MMCE_SUPPORT_H

#include "include/iosupport.h"
#include "include/mcemu.h"

#define MMCE_MODE_UPDATE_DELAY MENU_UPD_DELAY_GENREFRESH

typedef struct
{
    int active;       /* Activation flag */
    int fd;           /* VMC fd */
    int flags;        /* Card flag */
    vmc_spec_t specs; /* Card specifications */
} mmce_vmc_infos_t;

void mmceInit(item_list_t *itemList);
item_list_t *mmceGetObject(int initOnly);
void mmceLoadModules(void);
void mmceLaunchGame(item_list_t *itemList, int fd, config_set_t *configSet);
// Push the selected game's disc id to a present MMCE card (SD2PSX/MemCard PRO2) for per-game folder
// switching. Self-probes mmce0:/mmce1: when no MMCE-tab prefix is set, so it works on ALL launch
// paths (USB/HDD/SMB), not just the MMCE tab. No-ops if the feature is off or no card answers (#261).
// protectMcPath: on a Neutrino launch, the resolved neutrino.elf path. If it lives on the emulated
// memory card (mcN:) of the slot we would switch, the switch is skipped so it can't yank the loader
// out from under sysLaunchNeutrino (NHDDL parity, issue #51). Pass NULL when nothing needs protecting.
// vmcSlotMask bit N = "a -mcN Neutrino VMC arg is set for this launch": that slot's MC comes from
// the VMC FILE, so its physical card is NOT switched (pass 0 on OPL-core paths -- mcemu, unchanged).
int mmceSendGameID(const char *startup, const char *protectMcPath, int vmcSlotMask);
// Bounded post-GameID settle for cross-device launches sharing SIO2 with the MMCE (MX4SIO, batch S7).
// Polls the device the last send targeted; 0 = settled, -1 = expired. Caller warns + proceeds.
int mmceGameIdSettle(int timeoutMs);
// Arm the GameID transport at menu/settings time (idempotent; no-op when the feature is off).
void mmceArmGameIDTransport(void);

#endif
