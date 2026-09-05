#ifndef __NET_SUPPORT_H
#define __NET_SUPPORT_H

#include "include/opl.h"

// The menu-side TCP/IP stack, as distinct from any protocol that runs over it.
//
// netman + smsutils + smap + ps2ip + ps2ips + the HTTP RPC client are one shared resource: the
// console has a single SMAP NIC, one driver may own it, and bringing it up costs the same
// whichever protocol asked. This file owns that stack. SMB session state, shares, credentials and
// SMB's error strings stay in ethsupport.c; HTTP's stay in httpsupport.c.
//
// The split exists because "the stack is up" and "an SMB share is connected" are different facts
// and were only ever distinguishable by reading ethsupport internals. HTTP has to be able to raise
// the NIC without implying a share, and the UDPBD/UDPFS interlocks have to keep working when it is
// HTTP rather than SMB holding the NIC.
//
// NIC exclusivity comes free from that sharing and must stay that way: every claimant tests
// netGetModulesLoaded(), and netLoadModules() refuses when UDPBD or the UDPFS filesystem chain
// already drives SMAP. Because HTTP raises the very same stack, all three existing interlocks
// cover it without a fourth check being added anywhere. Do not give HTTP a private "modules
// loaded" flag; that is precisely what would make two drivers fight over the EMAC.

// Bring the stack up and apply the current network configuration (link mode, IP/DHCP).
// 0 on success. Sets gNetworkStartup on failure.
int netLoadInitModules(void);

// Bring the stack up WITHOUT applying the network configuration. Only for a protocol that applies
// it later as part of its own connect sequence, which is what SMB does. Everything else wants
// netLoadInitModules().
int netEnsureModules(void);

// 1 once the stack is resident, whichever protocol asked for it. This is the interlock predicate:
// UDPBD and the UDPFS filesystem must not load on top of it. It says nothing about whether any
// share, server or session is reachable.
int netGetModulesLoaded(void);

// Tear the stack down. protocolTeardown, when non-NULL, runs inside the init lock at the point
// the SMB path has always run its own teardown -- after the HTTP RPC client is released and
// before NetManDeinit -- so that ordering is preserved rather than reasoned about.
void netDeinitModules(void (*protocolTeardown)(void));

// Re-apply the current network configuration to a stack that is already up.
int netApplyConfig(void);

// Cached-if-down copy of the live address. Returns <0 when the stack is not loaded, in which case
// the outputs hold the last values read before teardown.
int netGetConfig(u8 *ip_address, u8 *netmask, u8 *gateway);

// >0 bound, 0 pending, <0 DHCP disabled.
int netGetDHCPStatus(void);

// The stack init lock. ethsupport's SMB entry points take it around their own apply-config calls;
// anything else that raises or reconfigures the stack must take it too.
int netInitSema(void);
void netLockInit(void);
void netUnlockInit(void);

#endif
