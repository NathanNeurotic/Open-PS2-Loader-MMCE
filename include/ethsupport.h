#ifndef __ETH_SUPPORT_H
#define __ETH_SUPPORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "include/iosupport.h"

#define ETH_MODE_UPDATE_DELAY 300

#include "include/mcemu.h"
typedef struct
{
    int active;       /* Activation flag */
    char fname[80];   /* File name (memorycard?.bin): worst-case = 31(prefix)+5(\\VMC\\)+31(name)+4(.bin)+1(NUL) = 72 bytes; rounded up to 80 */
    u16 fid;          /* SMB File ID */
    int flags;        /* Card flag */
    vmc_spec_t specs; /* Card specifications */
} smb_vmc_infos_t;

void ethInit(item_list_t *itemList); // Full initialization (Start ETH + SMB and apply configuration). GUI must be already initialized, used by GUI to start SMB mode.
void ethDeinitModules(void);         // Module-only deinitialization, without the GUI's knowledge (for specific reasons, otherwise unused).
int ethLoadInitModules(void);        // Initializes Ethernet and applies configuration.
int ethGetModulesLoaded(void);       // 1 if the SMB NIC stack is loaded (UDPBD must not load on top).
// True only after OPENSHARE succeeded and the active smb0: prefix is usable. This is deliberately
// stronger than "Network Protocol == SMB": callers must not turn a read-only import into a long
// connection attempt or consume stale saved values.
int ethIsSMBShareConnected(void);
void ethDisplayErrorStatus(void); // Displays the current error status (if any). GUI must be already initialized.
int ethGetNetConfig(u8 *ip_address, u8 *netmask, u8 *gateway);
int ethApplyConfig(void);
int ethGetDHCPStatus(void);
item_list_t *ethGetObject(int initOnly);
// Resolve a stored ETH ISO favourite against the ISO backing view even while the live ETH page
// is showing VCDs. Read-only; never changes the visible ETH list or L3 view.
int ethResolveIsoFavourite(int id, const char *name, int *outId);
const char *ethGetSMBPrefix(void);

#ifdef __cplusplus
}
#endif

#endif

