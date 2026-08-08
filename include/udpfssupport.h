#ifndef __UDPFS_SUPPORT_H
#define __UDPFS_SUPPORT_H

#include "include/iosupport.h"

// Reuse ETH's refresh cadence: this is a network filesystem, so the same slow-poll delay applies.
#define UDPFS_MODE_UPDATE_DELAY 300

void udpfsInit(item_list_t *itemList); // Init the udpfs: network-filesystem device (loads the UDPFS ioman IRX chain).
int udpfsGetModulesLoaded(void);       // 1 if the UDPFS ioman NIC stack is loaded (SMB/UDPBD must not load on top).
item_list_t *udpfsGetObject(int initOnly);

#endif
