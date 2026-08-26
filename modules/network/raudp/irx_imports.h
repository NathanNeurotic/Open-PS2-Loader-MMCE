#ifndef IOP_IRX_IMPORTS_H
#define IOP_IRX_IMPORTS_H

#include "irx.h"

/* Please keep these in alphabetical order!  */
#include "loadcore.h"
#include "smap_tx.h"
#include "smstcpip.h"
#include "thbase.h"

/* MAC lookup in the lwIP ARP table. Added to our SMSTCPIP (etharp.c) as
   export 42, the last slot, so existing slots keep their numbers. Not in
   smstcpip.h because it is not part of the stock stack. */
int etharp_lookup_mac(unsigned int ipaddr, unsigned char *mac_out);
#define I_etharp_lookup_mac DECLARE_IMPORT(42, etharp_lookup_mac)

#endif /* IOP_IRX_IMPORTS_H */
