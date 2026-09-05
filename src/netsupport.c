// The menu-side TCP/IP stack, shared by every protocol that runs over it. See
// include/netsupport.h for what belongs here and what deliberately does not.
//
// Everything in this file was moved verbatim out of src/ethsupport.c, where it had grown up
// alongside the SMB session code and could only be reached through it. The move renames and
// relocates; it does not change what any of it does. The one structural change is that the stack
// half of the old ethDeinitModules() is now netDeinitModules(), taking the protocol's own teardown
// as a callback so it still runs at exactly the point in the sequence it always did.

#include <delaythread.h> // DelayThread -- bounded lock acquire in netDeinitModules
#include "include/opl.h"
#include "include/netsupport.h"
#include "include/supportbase.h" // sbCreateSemaphore
#include "include/renderman.h"   // rmGetHsync
#include "include/ioman.h"       // LOG
#include "include/system.h"
#include "include/extern_irx.h"
#include "include/bdmsupport.h"   // bdmIsUDPBDLoaded() for the NIC interlock
#include "include/udpfssupport.h" // udpfsGetModulesLoaded() for the NIC interlock
#include "include/nbns.h"         // nbnsDeinit -- released with the other RPC client
#include "httpclient.h"

static unsigned char netModulesLoaded = 0;

static struct ip4_addr lastIP;
static struct ip4_addr lastNM;
static struct ip4_addr lastGW;

static int netInitSemaID = -1;

// forward declaration
static int netWaitValidNetIFLinkState(void);
static int netWaitValidDHCPState(void);
static int netGetNetIFLinkStatus(void);
static int netApplyNetIFConfig(void);
static int netApplyIPConfig(void);
static int netReadNetConfig(void);

// Initializes locking semaphore for network support (not for just SMB support, but for the network subsystem).
int netInitSema(void)
{
    if (netInitSemaID < 0) {
        if ((netInitSemaID = sbCreateSemaphore()) < 0)
            return netInitSemaID;
    }

    return 0;
}

void netLockInit(void)
{
    if (netInitSemaID >= 0)
        WaitSema(netInitSemaID);
}

void netUnlockInit(void)
{
    if (netInitSemaID >= 0)
        SignalSema(netInitSemaID);
}

static void NetStatusCheckCb(s32 alarm_id, u16 time, void *common)
{
    iSignalSema(*(int *)common);
}

static int WaitValidNetState(int (*checkingFunction)(void))
{
    int SemaID, retry_cycles;
    ee_sema_t SemaData;

    // Wait for a valid network status;
    SemaData.option = SemaData.attr = 0;
    SemaData.init_count = 0;
    SemaData.max_count = 1;
    if ((SemaID = CreateSema(&SemaData)) < 0)
        return SemaID;

    for (retry_cycles = 0; checkingFunction() == 0; retry_cycles++) {
        SetAlarm(1000 * rmGetHsync(), &NetStatusCheckCb, &SemaID);
        WaitSema(SemaID);

        if (retry_cycles >= 30) // 30s = 30*1000ms
        {
            DeleteSema(SemaID);
            return -1;
        }
    }

    DeleteSema(SemaID);
    return 0;
}

static int netWaitValidNetIFLinkState(void)
{
    return WaitValidNetState(&netGetNetIFLinkStatus);
}

static int netWaitValidDHCPState(void)
{
    return WaitValidNetState(&netGetDHCPStatus);
}

// Caller must hold the init lock.
static int netInitApplyConfig(void)
{
    LOG("NETSUPPORT ApplyConfig\n");

    do {
        if (netWaitValidNetIFLinkState() != 0) {
            gNetworkStartup = ERROR_ETH_LINK_FAIL;
            return ERROR_ETH_LINK_FAIL;
        }
    } while (netApplyNetIFConfig() != 0);

    // Wait for the link to re-establish after applying the NIF link-mode setting.
    if (netWaitValidNetIFLinkState() != 0) {
        gNetworkStartup = ERROR_ETH_LINK_FAIL;
        return ERROR_ETH_LINK_FAIL;
    }

    netApplyIPConfig();

    // Wait for DHCP to initialize, if DHCP is enabled.
    if (ps2_ip_use_dhcp && (netWaitValidDHCPState() != 0)) {
        gNetworkStartup = ERROR_ETH_DHCP_FAIL;
        return ERROR_ETH_DHCP_FAIL;
    }

    return 0;
}

int netApplyConfig(void)
{
    int ret;

    netLockInit();
    ret = netInitApplyConfig();
    netUnlockInit();

    return ret;
}

int netGetModulesLoaded(void)
{
    return netModulesLoaded;
}

// Caller must hold the init lock.
static int netLoadModules(void)
{
    LOG("NETSUPPORT LoadModules\n");

    // UDPBD owns the single SMAP NIC ("SMAP_driver" modname); refuse to bring up the menu TCP/IP
    // stack on top of it. The Device-hub UI already interlocks the two; this is the runtime backstop.
    if (bdmIsUDPBDLoaded()) {
        LOG("NETSUPPORT: UDPBD active -- not loading the NIC stack\n");
        return -1;
    }
    // Same NIC exclusivity vs the udpfs FILESYSTEM chain (udpfs_smap + ministack). The other two
    // directions already guard symmetrically (udpfssupport checks eth+bdm, bdmsupport checks
    // eth+udpfs); without this one an in-session UDPFS->SMB protocol switch could double-drive the
    // SMAP EMAC with two drivers -- at best SMB fails to start, at worst the IOP wedges.
    if (udpfsGetModulesLoaded()) {
        LOG("NETSUPPORT: UDPFS filesystem NIC active -- not loading the NIC stack\n");
        return -1;
    }

    if (!netModulesLoaded) {
        netModulesLoaded = 1;

        sysInitDev9();

        LOG("[NETMAN]:\n");
        if (sysLoadModuleBuffer(&netman_irx, size_netman_irx, 0, NULL) >= 0) {
            NetManInit();
            LOG("[SMSUTILS]:\n");
            sysLoadModuleBuffer(&smsutils_irx, size_smsutils_irx, 0, NULL);
            LOG("[SMAP]:\n");
            if (sysLoadModuleBuffer(&smap_irx, size_smap_irx, 0, NULL) >= 0) {
                // Before the network stack is loaded, attempt to set the link settings in order to avoid needing double-initialization of the IF.
                // But do not fail here because there is currently no way to re-start initialization.
                netApplyNetIFConfig();
                LOG("[PS2IP]:\n");
                if (sysLoadModuleBuffer(&ps2ip_irx, size_ps2ip_irx, 0, NULL) >= 0) {
                    LOG("[PS2IPS]:\n");
                    sysLoadModuleBuffer(&ps2ips_irx, size_ps2ips_irx, 0, NULL);
                    LOG("[HTTPCLIENT]:\n");
                    if (sysLoadModuleBuffer(&httpclient_irx, size_httpclient_irx, 0, NULL) >= 0) {
                        if (HttpInit() < 0)
                            LOG("NETSUPPORT: httpclient RPC bind failed; compat update unavailable\n");
                    }
                    ps2ip_init();
                    LOG("NETSUPPORT Modules loaded\n");
                    return 0;
                }
            }
        }

        gNetworkStartup = ERROR_ETH_MODULE_NETIF_FAILURE;
        return -1;
    }

    return 0;
}

// Slices netDeinitModules will wait for the init lock before giving up. One slice is 100 ms, so ~3 s
// -- far past any healthy hand-off, and the ONLY unbounded blocker that was left on the teardown
// path (found by the rebuild-154 exit-hang audit and parked then because it is not ATA-specific;
// issue #382 is the not-ATA case).
#define NET_DEINIT_LOCK_SLICES 30

void netDeinitModules(void (*protocolTeardown)(void))
{
    if (netModulesLoaded) {
        // BOUNDED ACQUIRE, was an unbounded WaitSema. deinit runs this while a game launch is in
        // flight, and anything still holding this lock -- an in-progress SMB reconnect, a network
        // read that will not return because the link is already going away -- used to stop the exit
        // dead with the screen already torn down. That is the exact shape of rebuild-154's ATA hang,
        // one transport over.
        //
        // Giving up is safe and is the whole point: every caller is on its way to an IOP reset that
        // reclaims the network stack wholesale. A teardown that gives up beats one that hangs.
        int lockHeld = 0;
        if (netInitSemaID >= 0) {
            int slices = NET_DEINIT_LOCK_SLICES;
            while (slices-- > 0) {
                if (PollSema(netInitSemaID) == netInitSemaID) {
                    lockHeld = 1;
                    break;
                }
                DelayThread(100000); // 100 ms
            }
            if (!lockHeld)
                LOG("NET: deinit could not take the init lock -- tearing down anyway\n");
        }

        // Both RPC clients this stack registered, released together and in the order they always
        // were. nbns is NetBIOS name resolution, so it only matters to SMB, but releasing it is a
        // property of the stack rather than of the session -- and keeping it here is what lets the
        // protocol hook sit at the exact position the SMB flag writes used to occupy.
        HttpDeinit();
        nbnsDeinit();
        NetManDeinit();
        netModulesLoaded = 0;
        // The protocol's own state is cleared HERE, immediately after netModulesLoaded and before
        // the semaphore goes, because that is precisely where SMB cleared its two flags.
        if (protocolTeardown != NULL)
            protocolTeardown();

        if (netInitSemaID >= 0) {
            if (lockHeld)
                SignalSema(netInitSemaID);
            DeleteSema(netInitSemaID);
            netInitSemaID = -1;
        }

        // To allow the configuration to be read later on, read the latest version now.
        netReadNetConfig();
        ps2ip_deinit();
    }
}

int netLoadInitModules(void)
{
    int ret;

    if ((ret = netInitSema()) < 0)
        return ret;

    WaitSema(netInitSemaID);

    if ((ret = netLoadModules()) == 0) {
        ret = netInitApplyConfig();
    }

    SignalSema(netInitSemaID);

    return ret;
}

int netEnsureModules(void)
{
    int ret;

    if ((ret = netInitSema()) < 0)
        return ret;

    WaitSema(netInitSemaID);
    ret = netLoadModules();
    SignalSema(netInitSemaID);

    return ret;
}

static int netReadNetConfig(void)
{
    t_ip_info ip_info;
    int result;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
    if ((result = ps2ip_getconfig("sm0", &ip_info)) >= 0) {
        lastIP = *(struct ip4_addr *)&ip_info.ipaddr;
        lastNM = *(struct ip4_addr *)&ip_info.netmask;
        lastGW = *(struct ip4_addr *)&ip_info.gw;
    } else {
        ip4_addr_set_zero(&lastIP);
        ip4_addr_set_zero(&lastNM);
        ip4_addr_set_zero(&lastGW);
    }
#pragma GCC diagnostic pop

    return result;
}

int netGetConfig(u8 *ip_address, u8 *netmask, u8 *gateway)
{
    int result;

    // Read a cached copy of the settings, if this is read after deinitialization.
    result = netModulesLoaded ? netReadNetConfig() : -1;
    ip_address[0] = ip4_addr1(&lastIP);
    ip_address[1] = ip4_addr2(&lastIP);
    ip_address[2] = ip4_addr3(&lastIP);
    ip_address[3] = ip4_addr4(&lastIP);

    netmask[0] = ip4_addr1(&lastNM);
    netmask[1] = ip4_addr2(&lastNM);
    netmask[2] = ip4_addr3(&lastNM);
    netmask[3] = ip4_addr4(&lastNM);

    gateway[0] = ip4_addr1(&lastGW);
    gateway[1] = ip4_addr2(&lastGW);
    gateway[2] = ip4_addr3(&lastGW);
    gateway[3] = ip4_addr4(&lastGW);

    return result;
}

static int netApplyNetIFConfig(void)
{
    int mode, result;
    static int CurrentMode = NETMAN_NETIF_ETH_LINK_MODE_AUTO;

    switch (gETHOpMode) {
        case ETH_OP_MODE_100M_FDX:
            mode = NETMAN_NETIF_ETH_LINK_MODE_100M_FDX;
            break;
        case ETH_OP_MODE_100M_HDX:
            mode = NETMAN_NETIF_ETH_LINK_MODE_100M_HDX;
            break;
        case ETH_OP_MODE_10M_FDX:
            mode = NETMAN_NETIF_ETH_LINK_MODE_10M_FDX;
            break;
        case ETH_OP_MODE_10M_HDX:
            mode = NETMAN_NETIF_ETH_LINK_MODE_10M_HDX;
            break;
        default:
            mode = NETMAN_NETIF_ETH_LINK_MODE_AUTO;
    }

    if (CurrentMode != mode) {
        if ((result = NetManSetLinkMode(mode)) == 0)
            CurrentMode = mode;
    } else
        result = 0;

    return result;
}

static int netGetNetIFLinkStatus(void)
{
    return (NetManIoctl(NETMAN_NETIF_IOCTL_GET_LINK_STATUS, NULL, 0, NULL, 0) == NETMAN_NETIF_ETH_LINK_STATE_UP);
}

static int netApplyIPConfig(void)
{
    t_ip_info ip_info;
    struct ip4_addr ipaddr, netmask, gw, dns;
    const struct ip4_addr *dns_curr;
    int result;

    if ((result = ps2ip_getconfig("sm0", &ip_info)) >= 0) {
        IP4_ADDR(&ipaddr, ps2_ip[0], ps2_ip[1], ps2_ip[2], ps2_ip[3]);
        IP4_ADDR(&netmask, ps2_netmask[0], ps2_netmask[1], ps2_netmask[2], ps2_netmask[3]);
        IP4_ADDR(&gw, ps2_gateway[0], ps2_gateway[1], ps2_gateway[2], ps2_gateway[3]);
        IP4_ADDR(&dns, ps2_dns[0], ps2_dns[1], ps2_dns[2], ps2_dns[3]);
        dns_curr = dns_getserver(0);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
        // Check if it's the same. Otherwise, apply the new configuration.
        if ((ps2_ip_use_dhcp != ip_info.dhcp_enabled) || (!ps2_ip_use_dhcp &&
                                                          (!ip_addr_cmp(&ipaddr, (struct ip4_addr *)&ip_info.ipaddr) ||
                                                           !ip_addr_cmp(&netmask, (struct ip4_addr *)&ip_info.netmask) ||
                                                           !ip_addr_cmp(&gw, (struct ip4_addr *)&ip_info.gw) ||
                                                           !ip_addr_cmp(&dns, dns_curr)))) {
            if (ps2_ip_use_dhcp) {
                ip4_addr_set_zero((struct ip4_addr *)&ip_info.ipaddr);
                ip4_addr_set_zero((struct ip4_addr *)&ip_info.netmask);
                ip4_addr_set_zero((struct ip4_addr *)&ip_info.gw);
                ip4_addr_set_zero(&dns);

                ip_info.dhcp_enabled = 1;
            } else {
                ip_addr_set((struct ip4_addr *)&ip_info.ipaddr, &ipaddr);
                ip_addr_set((struct ip4_addr *)&ip_info.netmask, &netmask);
                ip_addr_set((struct ip4_addr *)&ip_info.gw, &gw);

                ip_info.dhcp_enabled = 0;
            }

            result = ps2ip_setconfig(&ip_info);
            if (!ps2_ip_use_dhcp)
                dns_setserver(0, &dns);
        } else
            result = 0;
#pragma GCC diagnostic pop
    }

    return result;
}

int netGetDHCPStatus(void)
{
    t_ip_info ip_info;
    int result;

    if ((result = ps2ip_getconfig("sm0", &ip_info)) >= 0) {
        if (ip_info.dhcp_enabled) {
            result = (ip_info.dhcp_status == DHCP_STATE_BOUND || (ip_info.dhcp_status == DHCP_STATE_OFF));
        } else
            result = -1;
    }

    return result;
}
