#include "include/opl.h"
#include "include/lang.h"
#include "include/gui.h"
#include "include/supportbase.h"
#include "include/ethsupport.h"
#include "include/netsupport.h" // the shared TCP/IP stack; this file owns only the SMB session on top of it
#include "include/vcdsupport.h"
#include "include/cuesupport.h" // ps1FillGameList + the Ember launch helpers
#include "include/libview.h"    // libViewActive / libListViewActive -- which list this page shows
#include "include/util.h"
#include "include/renderman.h"
#include "include/themes.h"
#include "include/textures.h"
#include "include/ioman.h"
#include "include/system.h"
#include "include/extern_irx.h"
#include "include/cheatman.h"
#include "include/bdmsupport.h"   // bdmIsUDPBDLoaded() for the SMB<->UDPBD NIC interlock
#include "include/udpfssupport.h" // udpfsGetModulesLoaded() for the SMB<->UDPFS-filesystem NIC interlock
#include "include/mmcesupport.h"  // mmceSendGameID() cross-device game-id (#261)
#include "modules/iopcore/common/cdvd_config.h"

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h> // fileXioDevctl(ethBase, SMB_***)

#include "include/nbns.h"
#include "httpclient.h"

static char ethPrefix[40]; // Contains the full path to the folder where all the games are.
static char *ethBase;
static int ethULSizePrev = -2;
static time_t ethModifiedCDPrev;
static time_t ethModifiedDVDPrev;
static int ethGameCount = 0;
static unsigned char ethSmbModuleLoaded = 0;
static base_game_info_t *ethGames = NULL;
static int ethPs1GameCount = 0;
static base_game_info_t *ethPs1Games = NULL;

// forward declaration
static item_list_t ethGameList;

static unsigned char ethReconnectQueued = 0;

static void ethSMBConnect(void)
{
    unsigned char share_ip_address[4];
    smbLogOn_in_t logon;
    smbEcho_in_t echo;
    smbOpenShare_in_t openshare;
    int result;

    if (gETHPrefix[0] != '\0')
        sprintf(ethPrefix, "%s%s\\", ethBase, gETHPrefix);
    else
        strcpy(ethPrefix, ethBase);

    // open tcp connection with the server / logon to SMB server
    if (gPCShareAddressIsNetBIOS) {
        if (nbnsFindName(gPCShareNBAddress, share_ip_address) != 0) {
            gNetworkStartup = ERROR_ETH_SMB_CONN;
            return;
        }

        sprintf(logon.serverIP, "%u.%u.%u.%u", share_ip_address[0], share_ip_address[1], share_ip_address[2], share_ip_address[3]);
    } else {
        sprintf(logon.serverIP, "%u.%u.%u.%u", pc_ip[0], pc_ip[1], pc_ip[2], pc_ip[3]);
    }

    logon.serverPort = gPCPort;

    if (strlen(gPCPassword) > 0) {
        smbGetPasswordHashes_in_t passwd;
        smbGetPasswordHashes_out_t passwdhashes;

        // we'll try to generate hashed password first
        strncpy(logon.User, gPCUserName, sizeof(logon.User));
        strncpy(passwd.password, gPCPassword, sizeof(passwd.password));

        if (fileXioDevctl(ethBase, SMB_DEVCTL_GETPASSWORDHASHES, (void *)&passwd, sizeof(passwd), (void *)&passwdhashes, sizeof(passwdhashes)) == 0) {
            // hash generated okay, can use
            memcpy((void *)logon.Password, (void *)&passwdhashes, sizeof(passwdhashes));
            logon.PasswordType = HASHED_PASSWORD;
            memcpy((void *)openshare.Password, (void *)&passwdhashes, sizeof(passwdhashes));
            openshare.PasswordType = HASHED_PASSWORD;
        } else {
            // failed hashing, failback to plaintext
            strncpy(logon.Password, gPCPassword, sizeof(logon.Password));
            logon.PasswordType = PLAINTEXT_PASSWORD;
            strncpy(openshare.Password, gPCPassword, sizeof(openshare.Password));
            openshare.PasswordType = PLAINTEXT_PASSWORD;
        }
    } else {
        strncpy(logon.User, gPCUserName, sizeof(logon.User));
        logon.PasswordType = NO_PASSWORD;
        openshare.PasswordType = NO_PASSWORD;
    }

    if ((result = fileXioDevctl(ethBase, SMB_DEVCTL_LOGON, (void *)&logon, sizeof(logon), NULL, 0)) >= 0) {
        // SMB server alive test
        strcpy(echo.echo, "ALIVE ECHO TEST");
        echo.len = strlen("ALIVE ECHO TEST");

        if (gPCShareAddressIsNetBIOS) {
            // Since the SMB server can be connected to, update the IP address.
            pc_ip[0] = share_ip_address[0];
            pc_ip[1] = share_ip_address[1];
            pc_ip[2] = share_ip_address[2];
            pc_ip[3] = share_ip_address[3];
        }

        if (fileXioDevctl(ethBase, SMB_DEVCTL_ECHO, (void *)&echo, sizeof(echo), NULL, 0) >= 0) {
            gNetworkStartup = ERROR_ETH_SMB_OPENSHARE;

            if (gPCShareName[0]) {
                // connect to the share
                strcpy(openshare.ShareName, gPCShareName);

                if (fileXioDevctl(ethBase, SMB_DEVCTL_OPENSHARE, (void *)&openshare, sizeof(openshare), NULL, 0) >= 0) {
                    // everything is ok
                    gNetworkStartup = 0;
                }
            }
        } else {
            gNetworkStartup = ERROR_ETH_SMB_ECHO;
        }
    } else {
        gNetworkStartup = (result == -SMB_DEVCTL_LOGON_ERR_CONN) ? ERROR_ETH_SMB_CONN : ERROR_ETH_SMB_LOGON;
    }
}

static int ethSMBDisconnect(void)
{
    int shareRet, logoffRet;

    // Always attempt both halves. CLOSESHARE legitimately fails after a connection/logon error or
    // when LOGON succeeded but OPENSHARE did not; returning early there used to leave that partial
    // session alive, so the next explicit reconnect was not actually a clean retry.
    shareRet = fileXioDevctl(ethBase, SMB_DEVCTL_CLOSESHARE, NULL, 0, NULL, 0);
    logoffRet = fileXioDevctl(ethBase, SMB_DEVCTL_LOGOFF, NULL, 0, NULL, 0);
    if (shareRet < 0)
        return -1;
    if (logoffRet < 0)
        return -2;

    return 0;
}

int ethApplyConfig(void)
{
    return netApplyConfig();
}

static void ethInitSMB(void)
{
    int ret;

    ret = netApplyConfig();

    if (ret != 0) {
        ethDisplayErrorStatus();
        return;
    }

    // connect
    ethSMBConnect();

    if (gNetworkStartup == 0) {
        // update Themes
        char path[256];
        sprintf(path, "%sTHM", ethPrefix);
        thmAddElements(path, "\\", 1);

        sprintf(path, "%sLNG", ethPrefix);
        lngAddLanguages(path, "\\", ethGameList.mode);

        sbCreateFolders(ethBase, 1);
        if (strcmp(ethPrefix, ethBase) != 0)
            sbCreateFolders(ethPrefix, 1);
    } else if (gPCShareName[0] || !(gNetworkStartup >= ERROR_ETH_SMB_OPENSHARE)) {
        ethDisplayErrorStatus();
    }
}

int ethGetModulesLoaded(void)
{
    return netGetModulesLoaded();
}

int ethIsSMBShareConnected(void)
{
    return netGetModulesLoaded() && gNetworkStartup == 0 && ethPrefix[0] != '\0';
}

// SMB's own teardown. netDeinitModules runs this inside the init lock, at the point in the
// sequence nbnsDeinit() has always occupied: after the HTTP RPC client is released, before
// NetManDeinit.
static void ethProtocolTeardown(void)
{
    nbnsDeinit();
    ethSmbModuleLoaded = 0;
    gNetworkStartup = ERROR_ETH_NOT_STARTED;
}

void ethDeinitModules(void)
{
    netDeinitModules(&ethProtocolTeardown);
}

int ethLoadInitModules(void)
{
    return netLoadInitModules();
}

void ethDisplayErrorStatus(void)
{
    switch (gNetworkStartup) {
        case 0: // No error
            break;
        case ERROR_ETH_MODULE_NETIF_FAILURE:
            setErrorMessageWithCode(_STR_NETWORK_STARTUP_ERROR_NETIF, gNetworkStartup);
            break;
        case ERROR_ETH_SMB_CONN:
            setErrorMessageWithCode(_STR_NETWORK_STARTUP_ERROR_CONN, gNetworkStartup);
            break;
        case ERROR_ETH_SMB_LOGON:
            setErrorMessageWithCode(_STR_NETWORK_STARTUP_ERROR_LOGON, gNetworkStartup);
            break;
        case ERROR_ETH_SMB_OPENSHARE:
            setErrorMessageWithCode(_STR_NETWORK_STARTUP_ERROR_SHARE, gNetworkStartup);
            break;
        case ERROR_ETH_SMB_LISTSHARES:
            setErrorMessageWithCode(_STR_NETWORK_SHARE_LIST_ERROR, gNetworkStartup);
            break;
        case ERROR_ETH_SMB_LISTGAMES:
            setErrorMessageWithCode(_STR_NETWORK_GAMES_LIST_ERROR, gNetworkStartup);
            break;
        case ERROR_ETH_LINK_FAIL:
            LOG("ETH: Unable to get valid link status.\n");
            setErrorMessageWithCode(_STR_NETWORK_ERROR_LINK_FAIL, gNetworkStartup);
            break;
        case ERROR_ETH_DHCP_FAIL:
            LOG("ETH: Unable to get valid IP address via DHCP.\n");
            setErrorMessageWithCode(_STR_NETWORK_ERROR_DHCP_FAIL, gNetworkStartup);
            break;
        default:
            setErrorMessageWithCode(_STR_NETWORK_STARTUP_ERROR, gNetworkStartup);
    }
}

static void smbLoadModules(void)
{
    int ret;

    LOG("SMBSUPPORT LoadModules\n");

    ret = netEnsureModules();

    if (ret == 0) {
        gNetworkStartup = ERROR_ETH_MODULE_SMBMAN_FAILURE;
        /*
          Load ONE SMB filesystem driver, chosen by the SMB Version picker. Both register the same
          iomanX device name ("smb", hence the "smb0:" paths everywhere), so only one may ever be
          resident -- and every path in OPL stays identical whichever one it is.

            smbman  -- PS2SDK's SMB1 driver, consumed prebuilt. Unchanged, still the default.
            smb2man -- our SMB2/SMB3 driver over vendored libsmb2 (modules/network/smb2man).

          Anything other than an explicit SMB2 selection falls back to smbman: losing SMB2 is
          recoverable, booting with no filesystem driver at all is not.
        */
        // NOTE(rebuild): SMB2 (smb2man + gSMBDialect) returns with checklist item 4 -- until then
        // this build speaks SMBv1 unconditionally, exactly like every pre-dialect build.
        LOG("[SMBMAN]:\n");
        if (sysLoadModuleBuffer((void *)&smbman_irx, size_smbman_irx, 0, NULL) >= 0) {
            ethSmbModuleLoaded = 1;
            LOG("[NBNS]:\n");
            sysLoadModuleBuffer(&nbns_irx, size_nbns_irx, 0, NULL);
            nbnsInit();

            LOG("SMBSUPPORT Modules loaded\n");
            ethInitSMB();
            return;
        }
    }

    ethDisplayErrorStatus();
}

static void ethReconnectSMB(void)
{
    // Runs on the serialized IO worker. Keep the old backing allocations alive until the normal
    // deferred menu rebuild has detached their row text, but publish zero counts immediately so a
    // button press during reconnect resolves to an inert row rather than launching stale data.
    if (ethBase == NULL)
        ethBase = "smb0:";

    if (ethSmbModuleLoaded)
        (void)ethSMBDisconnect();

    thmReinit(ethBase);
    ethULSizePrev = -2;
    ethModifiedCDPrev = 0;
    ethModifiedDVDPrev = 0;
    ethGameCount = 0;
    ethPs1GameCount = 0;
    gNetworkStartup = ERROR_ETH_NOT_STARTED;

    if (!netGetModulesLoaded() || !ethSmbModuleLoaded)
        smbLoadModules();
    else
        ethInitSMB(); // applies link/IP settings before logging on and reopening the share

    libViewMarkDirty(ETH_MODE);
    ioPutRequest(IO_MENU_UPDATE_DEFFERED, &ethGameList.mode);
    ethReconnectQueued = 0;
}

void ethRequestReconnect(void)
{
    // A protocol switch while another NIC stack is resident is restart-only. Do not let Refresh on
    // a stale/hidden ETH page fight that interlock or revive SMB after Network Start was disabled.
    if (gNetworkProtocol != NET_PROTO_SMB || gETHStartMode == START_MODE_DISABLED)
        return;
    if (ethReconnectQueued)
        return;
    if (netInitSema() < 0)
        return;

    ethGameList.enabled = 1;
    ethGameList.delay = gArtDelay;
    ethReconnectQueued = 1;
    if (ioPutRequest(IO_CUSTOM_SIMPLEACTION, &ethReconnectSMB) != IO_OK)
        ethReconnectQueued = 0;
}

void ethInit(item_list_t *itemList)
{
    if (netInitSema() < 0)
        return;

    if (gNetworkStartup >= ERROR_ETH_SMB_CONN) {
        LOG("ETHSUPPORT Re-Init\n");
        ethRequestReconnect();
    } else {
        LOG("ETHSUPPORT Init\n");
        ethBase = "smb0:";
        ethULSizePrev = -2;
        ethModifiedCDPrev = 0;
        ethModifiedDVDPrev = 0;
        ethGameCount = 0;
        ethGames = NULL;
        ethPs1GameCount = 0;
        ethPs1Games = NULL;
        ethGameList.delay = gArtDelay;
        gNetworkStartup = ERROR_ETH_NOT_STARTED;
        ioPutRequest(IO_CUSTOM_SIMPLEACTION, &smbLoadModules);
        ethGameList.enabled = 1;
    }
}

item_list_t *ethGetObject(int initOnly)
{
    if (initOnly && !ethGameList.enabled)
        return NULL;
    return &ethGameList;
}

static int ethNeedsUpdate(item_list_t *itemList)
{
    int result;

    result = 0;

    // VCD view: force a rescan once on toggle; once a share is selected, the VCD list refreshes on
    // toggle only (skip disc heuristics). With no share yet, fall through so the share list updates.
    if (libViewConsumeDirty(itemList->mode))
        return 1;
    if (gPCShareName[0] && (libListViewActive(itemList) == LIB_VIEW_PS1))
        return 0;

    if (ethULSizePrev == -2)
        result = 1;

    if (gNetworkStartup == 0) {
        struct stat st;
        char path[256];

        sprintf(path, "%sCD", ethPrefix);
        if (stat(path, &st) != 0)
            st.st_mtime = 0;
        if (ethModifiedCDPrev != st.st_mtime) {
            ethModifiedCDPrev = st.st_mtime;
            result = 1;
        }

        sprintf(path, "%sDVD", ethPrefix);
        if (stat(path, &st) != 0)
            st.st_mtime = 0;
        if (ethModifiedDVDPrev != st.st_mtime) {
            ethModifiedDVDPrev = st.st_mtime;
            result = 1;
        }

        if (!sbIsSameSize(ethPrefix, ethULSizePrev))
            result = 1;
    }

    return result;
}

static int ethUpdateGameList(item_list_t *itemList)
{
    int view = libListViewActive(itemList);

    if (gPCShareName[0]) {
        if (gNetworkStartup != 0)
            return 0;

        if (view == LIB_VIEW_PS1 || view == LIB_VIEW_MIXED) {
            // One list, both cores -- the same union every other device page builds.
            //
            // This used to be POPSTARTER-only, on the reasoning that RiptOPL composes SMB paths
            // with '\' while Ember composes its own with '/', that smbman's tolerance for that was
            // untested, and that listing rows which then fail to open is worse than listing none.
            // The first half of that is now handled -- cuesupport builds every path with
            // cueSep(devPrefix), so a share prefix produces backslashes throughout.
            //
            // The second half was already handled and we had not noticed: cueScanDir opens
            // EMBER/ember.elf for real before it lists anything, and returns 0 rows if that open
            // fails. So the bad outcome the old comment guarded against cannot occur here. If SMB
            // path composition does not work on a given server, the Ember half contributes nothing
            // and the POPSTARTER half is untouched -- which is exactly the "no half lists" rule it
            // wanted. The feature gates itself on a real file open.
            int r = ps1FillGameList(ethPrefix, &ethPs1Games);
            if (r >= 0) // r < 0: transient scan failure -> preserve the last-good list
                ethPs1GameCount = r;
            // NULL sub opts SMB/ETH out of folder-row collection: it uses a "\\" separator and does not
            // participate in folder browsing (see the folderlist gate in sbReadList).
        }
        if ((view == LIB_VIEW_ISO || view == LIB_VIEW_MIXED) &&
            (sbReadList(&ethGames, ethPrefix, /* sub: */ NULL, &ethULSizePrev, &ethGameCount)) < 0) {
            gNetworkStartup = ERROR_ETH_SMB_LISTGAMES;
            ethDisplayErrorStatus();
        }
    } else {
        int i, count;
        ShareEntry_t sharelist[128];
        smbGetShareList_in_t getsharelist;

        if (gNetworkStartup < ERROR_ETH_SMB_OPENSHARE)
            return 0;

        getsharelist.EE_addr = (void *)&sharelist[0];
        getsharelist.maxent = 128;

        count = fileXioDevctl(ethBase, SMB_DEVCTL_GETSHARELIST, (void *)&getsharelist, sizeof(getsharelist), NULL, 0);
        if (count > 0) {
            free(ethGames);
            ethGames = (base_game_info_t *)malloc(sizeof(base_game_info_t) * count);
            // On allocation failure, skip population so the loop below never
            // dereferences NULL; ethGameCount is then set to 0 after the loop.
            if (ethGames == NULL)
                count = 0;
            for (i = 0; i < count; i++) {
                LOG("ETHSUPPORT Share found: %s\n", sharelist[i].ShareName);
                base_game_info_t *g = &ethGames[i];
                memcpy(g->name, sharelist[i].ShareName, sizeof(g->name));
                g->name[31] = '\0';
                sprintf(g->startup, "SHARE");
                g->extension[0] = '\0';
                g->parts = 0x00;
                g->media = 0x00;
                g->format = GAME_FORMAT_USBLD;
                g->sizeMB = 0;
            }
            ethGameCount = count;
        } else if (count == 0) {
            free(ethGames);
            ethGames = NULL;
            ethGameCount = 0;
        } else {
            gNetworkStartup = ERROR_ETH_SMB_LISTSHARES;
            ethDisplayErrorStatus();
        }
    }
    if (!gPCShareName[0])
        return ethGameCount;
    return view == LIB_VIEW_MIXED ? ethGameCount + ethPs1GameCount :
           view == LIB_VIEW_PS1   ? ethPs1GameCount :
                                    ethGameCount;
}

int ethResolveIsoFavourite(int id, const char *name, int *outId)
{
    if (name == NULL || outId == NULL || id < 0 || !gPCShareName[0] || gNetworkStartup != 0)
        return 0;

    // ISO and PS1 now have permanent independent stores, so Favorites never needs to rescan or
    // borrow the visible page's backing array.
    if (ethGames == NULL || id >= ethGameCount || strcmp(ethGames[id].name, name) != 0)
        return 0;

    *outId = id;
    return 1;
}

static int ethGetItemView(item_list_t *itemList, int id)
{
    int view;

    // Before a share is chosen, the page is a share picker regardless of the global game display.
    if (!gPCShareName[0])
        return LIB_VIEW_ISO;
    view = libListViewActive(itemList);
    if (view == LIB_VIEW_MIXED)
        return id >= 0 && id < ethGameCount ? LIB_VIEW_ISO : LIB_VIEW_PS1;
    return view;
}

static int ethGetSourceId(item_list_t *itemList, int id)
{
    return gPCShareName[0] && libListViewActive(itemList) == LIB_VIEW_MIXED && id >= ethGameCount ? id - ethGameCount : id;
}

static base_game_info_t *ethGameForView(item_list_t *itemList, int id)
{
    int ps1 = ethGetItemView(itemList, id) == LIB_VIEW_PS1;
    id = ethGetSourceId(itemList, id);
    base_game_info_t *games = ps1 ? ethPs1Games : ethGames;
    int count = ps1 ? ethPs1GameCount : ethGameCount;
    if (games == NULL || id < 0 || id >= count)
        return NULL;
    return &games[id];
}

static int ethGetGameCount(item_list_t *itemList)
{
    int view;

    if (!gPCShareName[0])
        return ethGameCount;
    view = libListViewActive(itemList);
    return view == LIB_VIEW_MIXED ? ethGameCount + ethPs1GameCount :
           view == LIB_VIEW_PS1   ? ethPs1GameCount :
                                    ethGameCount;
}

static void *ethGetGame(item_list_t *itemList, int id)
{
    return (void *)ethGameForView(itemList, id);
}

static char *ethGetGameName(item_list_t *itemList, int id)
{
    base_game_info_t *game = ethGameForView(itemList, id);
    return game != NULL ? game->name : "";
}

static int ethGetGameNameLength(item_list_t *itemList, int id)
{
    base_game_info_t *game = ethGameForView(itemList, id);
    if (game == NULL)
        return 0;
    return game->format != GAME_FORMAT_USBLD ? ISO_GAME_NAME_MAX + 1 : UL_GAME_NAME_MAX + 1;
}

static char *ethGetGameStartup(item_list_t *itemList, int id)
{
    base_game_info_t *game = ethGameForView(itemList, id);
    if (game == NULL)
        return "";
    // VCD view keys per-game data (CFG/art) off the VCD filename, not a disc ID (see sbPopulateConfig).
    if (ethGetItemView(itemList, id) == LIB_VIEW_PS1)
        return game->name;
    return game->startup;
}

static void ethDeleteGame(item_list_t *itemList, int id)
{
    if (ethGetItemView(itemList, id) == LIB_VIEW_PS1)
        return;
    id = ethGetSourceId(itemList, id);
    sbDelete(&ethGames, ethPrefix, "\\", ethGameCount, id);
    ethULSizePrev = -2;
}

static void ethRenameGame(item_list_t *itemList, int id, char *newName)
{
    if (ethGetItemView(itemList, id) == LIB_VIEW_PS1) {
        base_game_info_t *game = ethGameForView(itemList, id);

        // Rename through the row's OWN core: an Ember title is a FOLDER under EMBER/games/, a
        // POPSTARTER title is a .VCD file in POPS/. Renaming one as the other silently fails.
        if (game != NULL &&
            (cueIsCueEntry(game) ? cueRenameGame(ethPrefix, game->name, newName) : vcdRenameFile(ethPrefix, game->name, newName)) == 0) {
            // ETH otherwise treats its VCD list as toggle-only; consume this on the already queued
            // deferred update so the renamed POPS/ directory is scanned again.
            libViewMarkDirty(itemList->mode);
        }
        return;
    }
    id = ethGetSourceId(itemList, id);
    sbRename(&ethGames, ethPrefix, "\\", ethGameCount, id, newName);
    ethULSizePrev = -2;
}

// Launch a PS1/.VCD entry BY NAME via POPSTARTER over SMB (view-independent entry point: the in-view
// menu launch below and the Favourites tab both use it). ethPrefix is static and smb: paths use '\'
// (auto-detected by vcdSep); UNMOUNT_EXCEPTION keeps the share mounted across the IOP reset.
static void ethLaunchVcd(item_list_t *itemList, const char *vcdName, config_set_t *configSet)
{
    char vcdElf[256], vcdSelector[320];

    if (!gPCShareName[0] || vcdName == NULL || vcdName[0] == '\0' || !strcasecmp(vcdName, "POPSTARTER")) // reserved-name belt: the scanner no longer lists it (#154); strcasecmp -- FAT is case-insensitive
        return;
    if (!vcdResolvePopstarter(ethPrefix, vcdElf, sizeof(vcdElf))) {
        guiMsgBox(_l(_STR_POPSTARTER_NOT_FOUND), 0, NULL);
        return;
    }
    {
        vcd_popsnet_ensure_t ens = vcdPreparePopstarterSmbLaunch(ethPrefix);
        if (ens == VCD_POPSNET_SMB_MISSING) {
            guiMsgBox(_l(_STR_POPSTARTER_SMB_MISSING), 0, NULL);
            return;
        }
        if (ens == VCD_POPSNET_NEED_STATIC) {
            guiMsgBox(_l(_STR_POPSTARTER_SMB_NEEDS_STATIC), 0, NULL);
            return;
        }
        if (ens == VCD_POPSNET_IO_ERROR) {
            guiMsgBox(_l(_STR_POPSTARTER_NET_ERR), 0, NULL);
            return;
        }
        if (ens == VCD_POPSNET_INVALID) {
            guiMsgBox(_l(_STR_POPSTARTER_NET_INVALID), 0, NULL);
            return;
        }
    }
    vcdBuildSelector(ethPrefix, VCD_PREFIX_SMB, vcdName, vcdSelector, sizeof(vcdSelector));
    size_t prefixLen = strlen(ethPrefix);
    char separator = (prefixLen > 0 && ethPrefix[prefixLen - 1] == '\\') ? '\\' : '/';
    char vcdFullPath[256];
    snprintf(vcdFullPath, sizeof(vcdFullPath), "%sPOPS%c%s.VCD", ethPrefix, separator, vcdName);
    vcdPrepareRetroGemBarcode(vcdFullPath);
    deinit(UNMOUNT_EXCEPTION, itemList->mode); // keep the SMB mount alive across the IOP reset
    sysLaunchPopstarter(vcdElf, vcdSelector);
}

// Ember (*.cue) launch over SMB. Mirrors bdmLaunchCue; the only SMB-specific part is that the
// prefix already ends with its own separator, which cuesupport honours via cueSep().
static void ethLaunchCue(item_list_t *itemList, const char *cueName, config_set_t *configSet)
{
    char emberElf[256], biosPath[288];

    (void)configSet; // an Ember title carries no per-game loader settings (see guigame.c)

    if (!gPCShareName[0] || cueName == NULL || cueName[0] == '\0')
        return;

    // Refuse what Ember itself would refuse, while a dialog can still be drawn.
    if (!cueNameLaunchable(cueName)) {
        guiMsgBox(_l(_STR_EMBER_BAD_NAME), 0, NULL);
        return;
    }
    if (!cueResolveEmber(ethPrefix, emberElf, sizeof(emberElf))) {
        guiMsgBox(_l(_STR_EMBER_NOT_FOUND), 0, NULL);
        return;
    }
    if (!cueResolveEmberBios(ethPrefix, biosPath, sizeof(biosPath))) {
        guiMsgBox(_l(_STR_EMBER_BIOS_MISSING), 0, NULL);
        return;
    }
    cueApplyDisplaySetting(ethPrefix); // best-effort marker, never a launch gate
    if (!cueGameHasImage(ethPrefix, cueName)) {
        guiMsgBox(_l(_STR_EMBER_NO_DISC), 0, NULL);
        return;
    }

    // NO vcdPreparePopstarterSmbLaunch here, deliberately. That equips POPSTARTER's own
    // IPCONFIG.DAT / SMBCONFIG.DAT because POPSTARTER resets the IOP and has to redial the share
    // itself. Ember never resets the IOP -- it inherits this live smbman mount -- so it needs no
    // network configuration of its own, and writing POPSTARTER's files for an Ember launch would
    // be a side effect with no purpose.

    // UNMOUNT_EXCEPTION is load-bearing: Ember reads its game through the SMB mount that is live
    // right now, so that mount must survive the teardown.
    deinit(UNMOUNT_EXCEPTION, itemList->mode);
    sysLaunchEmber(emberElf, cueName);
}

static void ethLaunchGame(item_list_t *itemList, int id, config_set_t *configSet)
{
    int i, compatmask;
    int EnablePS2Logo = 0;
    int result;
    char filename[32], partname[256];
    base_game_info_t *game = ethGameForView(itemList, id);
    struct cdvdman_settings_smb *settings;

    if (game == NULL)
        return;
    u32 layer1_start, layer1_offset;
    unsigned short int layer1_part;

    // PS1 view (SMB): the row carries its own core, so dispatch on the ROW, not on the view.
    // A .cue row is Ember's; anything else on this list is a POPSTARTER .VCD.
    if (gPCShareName[0] && game != NULL && (ethGetItemView(itemList, id) == LIB_VIEW_PS1)) {
        if (cueIsCueEntry(game))
            ethLaunchCue(itemList, game->name, configSet);
        else
            ethLaunchVcd(itemList, game->name, configSet);
        return;
    }

    if (!gPCShareName[0]) {
        memcpy(gPCShareName, game->name, sizeof(gPCShareName));
        ethULSizePrev = -2;
        ethGameCount = 0;
        ethPs1GameCount = 0;
        ioPutRequest(IO_MENU_UPDATE_DEFFERED, &ethGameList.mode); // clear the share list
        ioPutRequest(IO_CUSTOM_SIMPLEACTION, &ethInitSMB);
        ioPutRequest(IO_MENU_UPDATE_DEFFERED, &ethGameList.mode); // reload the game list
        return;
    }

    // $CoreLoader honesty: SMB has no Neutrino launch leg (nothing here builds -bsd/-dvd args),
    // so a per-game Neutrino selection -- or a Neutrino global default -- silently resolves to the
    // OPL core. Toast once at launch instead of leaving the setting looking honored. Pre-deinit,
    // so the toast renders; the launch then proceeds normally. Covers Favourites-origin launches
    // too (they delegate to this leg), which the compat-dialog lock in guigame.c cannot reach.
    {
        int coreLoader = gDefaultCoreLoader;
        configGetInt(configSet, CONFIG_ITEM_CORE_LOADER, &coreLoader);
        if (coreLoader == 2)                 // "Default" sentinel: the dialog never persists it (index 2 removes the
            coreLoader = gDefaultCoreLoader; // key), but a hand-edited cfg can carry it (Gemini, #161)
        if (coreLoader)
            guiWarning(_l(_STR_NEUTRINO_SMB_FALLBACK), 6);
    }

    char vmc_name[32];
    int vmc_id, size_mcemu_irx = 0;
    smb_vmc_infos_t smb_vmc_infos;
    vmc_superblock_t vmc_superblock;

    for (vmc_id = 0; vmc_id < 2; vmc_id++) {
        memset(&smb_vmc_infos, 0, sizeof(smb_vmc_infos_t));
        configGetVMC(configSet, vmc_name, sizeof(vmc_name), vmc_id);
        if (vmc_name[0]) {
            if (sysCheckVMC(ethPrefix, "\\", vmc_name, 0, &vmc_superblock) > 0) {
                smb_vmc_infos.flags = vmc_superblock.mc_flag & 0xFF;
                smb_vmc_infos.flags |= 0x100;
                smb_vmc_infos.specs.page_size = vmc_superblock.page_size;
                smb_vmc_infos.specs.block_size = vmc_superblock.pages_per_block;
                smb_vmc_infos.specs.card_size = vmc_superblock.pages_per_cluster * vmc_superblock.clusters_per_card;
                smb_vmc_infos.active = 1;
                smb_vmc_infos.fid = 0xFFFF;
                if (gETHPrefix[0])
                    snprintf(smb_vmc_infos.fname, sizeof(smb_vmc_infos.fname), "%s\\VMC\\%s.bin", gETHPrefix, vmc_name);
                else
                    snprintf(smb_vmc_infos.fname, sizeof(smb_vmc_infos.fname), "VMC\\%s.bin", vmc_name);
            } else {
                char error[256];
                snprintf(error, sizeof(error), _l(_STR_ERR_VMC_CONTINUE), vmc_name, (vmc_id + 1));
                if (!guiMsgBox(error, 1, NULL))
                    return;
            }
        }

        for (i = 0; i < size_smb_mcemu_irx; i++) {
            if (((u32 *)&smb_mcemu_irx)[i] == (0xC0DEFAC0 + vmc_id)) {
                if (smb_vmc_infos.active)
                    size_mcemu_irx = size_smb_mcemu_irx;
                memcpy(&((u32 *)&smb_mcemu_irx)[i], &smb_vmc_infos, sizeof(smb_vmc_infos_t));
                break;
            }
        }
    }

    if (gRememberLastPlayed) {
        configSetStr(configGetByType(CONFIG_LAST), "last_played", game->startup);
        saveConfig(CONFIG_LAST, 0);
    }

    compatmask = sbPrepare(game, configSet, size_smb_cdvdman_irx, smb_cdvdman_irx, &i);

#ifdef RETROACHIEVEMENTS
    // RA: this game's watch list, settled before sysLaunchLoaderElf reads
    // GetWatchCount() to decide whether the network modules travel with the
    // launch. Absent is normal -- the game is simply not tracked.
    sbLoadWatchList(ethPrefix, game->startup);
#endif
    if ((result = sbLoadCheats(ethPrefix, game->startup)) < 0) {
        // #265: let the user back out instead of sitting through the whole load. The helper does
        // the sbUnprepare itself -- see include/supportbase.h; skipping it breaks the NEXT launch.
        // `settings` is not assigned until below, so derive the common block from the IRX base.
        if (!sbCheatsMissingContinue((u8 *)(&smb_cdvdman_irx) + i, result))
            return;
    }
    sbLoadImage(ethPrefix, game->startup);

    settings = (struct cdvdman_settings_smb *)((u8 *)(&smb_cdvdman_irx) + i);

    switch (game->format) {
        case GAME_FORMAT_OLD_ISO:
            snprintf(settings->filename, sizeof(settings->filename), "%s.%s%s", game->startup, game->name, game->extension);
            break;
        case GAME_FORMAT_ISO:
            snprintf(settings->filename, sizeof(settings->filename), "%s%s", game->name, game->extension);
            break;
        default: // USBExtreme format.
            snprintf(settings->filename, sizeof(settings->filename), "ul.%08X.%s", USBA_crc32(game->name), game->startup);
            settings->common.flags |= IOPCORE_SMB_FORMAT_USBLD;
    }

    /*
      Hand the chosen dialect to the in-game reader. smbdisp.c reads these bits once, at negotiate
      time, to pick between smb.c (SMB1) and smb2.c (SMB2).

      They live in common.flags because cdvdman_settings_smb's SMB fields share a union with FIDs[]
      and cannot grow; common.flags already carries IOPCORE_SMB_FORMAT_USBLD for the same reason.
      Note this must come AFTER the format switch above, which also ORs into common.flags.

      Only SMB1/SMB2 are emitted -- gSMBDialect is clamped to those on load, and the picker offers
      no third value. Leaving the bits clear for SMB1 keeps the wire format identical to every
      build that predates the dialect field.
    */
    // NOTE(rebuild): the in-game SMB2 dialect flag (IOPCORE_SMB_DIALECT_V2) returns with item 4;
    // leaving the bits clear keeps the wire format identical to every pre-dialect build.

    sprintf(settings->smb_ip, "%u.%u.%u.%u", pc_ip[0], pc_ip[1], pc_ip[2], pc_ip[3]);
    settings->smb_port = gPCPort;
    strcpy(settings->smb_share, gPCShareName);
    strcpy(settings->smb_prefix, gETHPrefix);
    strcpy(settings->smb_user, gPCUserName);
    strcpy(settings->smb_password, gPCPassword);

    // Initialize layer 1 information.
    sbCreatePath(game, partname, ethPrefix, "\\", 0);

    if (gPS2Logo) {
        int fd = open(partname, O_RDONLY, 0666);
        if (fd >= 0) {
            EnablePS2Logo = CheckPS2Logo(fd, 0);
            close(fd);
        }
    }

    layer1_start = sbGetISO9660MaxLBA(partname);

    switch (game->format) {
        case GAME_FORMAT_USBLD:
            layer1_part = layer1_start / 0x80000;
            layer1_offset = layer1_start % 0x80000;
            sbCreatePath(game, partname, ethPrefix, "\\", layer1_part);
            break;
        default: // Raw ISO9660 disc image; one part.
            layer1_part = 0;
            layer1_offset = layer1_start;
    }

    if (sbProbeISO9660(partname, game, layer1_offset) != 0) {
        layer1_start = 0;
        LOG("DVD detected.\n");
    } else {
        layer1_start -= 16;
        LOG("DVD-DL layer 1 @ part %u sector 0x%lx.\n", layer1_part, layer1_offset);
    }
    settings->common.layer1_start = layer1_start;

    if (configGetStrCopy(configSet, CONFIG_ITEM_ALTSTARTUP, filename, sizeof(filename)) == 0)
        strcpy(filename, game->startup);
    // MMCE cross-device game-id (#261): push the disc id to a present MMCE card before the SMB teardown
    // (self-probes mmce0/mmce1; no-ops if no card / feature off). Read `game` before deinit frees it.
    mmceSendGameID(game->startup, NULL, 0); // SMB has no Neutrino branch -> nothing to protect, no -mc args
    deinit(NO_EXCEPTION, ETH_MODE);         // CAREFUL: deinit will call ethCleanUp, so ethGames/game will be freed

    settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_DEV9;
    settings->common.fakemodule_flags |= FAKE_MODULE_FLAG_SMAP;

    // adjust ZSO cache
    settings->common.zso_cache = smbCacheSize;

    sysLaunchLoaderElf(filename, "ETH_MODE", size_smb_cdvdman_irx, smb_cdvdman_irx, size_mcemu_irx, smb_mcemu_irx, EnablePS2Logo, compatmask);
}

static config_set_t *ethGetConfig(item_list_t *itemList, int id)
{
    base_game_info_t *game = ethGameForView(itemList, id);
    return game != NULL ? sbPopulateConfig(game, ethPrefix, "\\") : NULL;
}

static int ethGetImage(item_list_t *itemList, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)
{
    char path[256];

    if (isRelative)
        snprintf(path, sizeof(path), "%s%s\\%s_%s", ethPrefix, folder, value, suffix);
    else
        snprintf(path, sizeof(path), "%s%s_%s", folder, value, suffix);
    int r = texDiscoverLoad(resultTex, path, -1);
    // ART LIVES IN THE ART FOLDER, and nowhere else. A PS1 cover is
    // ART/<name>_COV.png -- the same rule as a PS2 title -- which the texDiscoverLoad
    // above already tried. A miss is simply "no cover": there is no second directory to
    // search, which also means a missing cover costs one failed open instead of several.
    return r;
}

static int ethGetTextId(item_list_t *itemList)
{
    return _STR_NET_GAMES;
}

static int ethGetIconId(item_list_t *itemList)
{
    return ETH_ICON;
}

// This may be called, even if ethInit() was not.
static void ethCleanUp(item_list_t *itemList, int exception)
{
    if (ethGameList.enabled) {
        LOG("ETHSUPPORT CleanUp\n");

        free(ethGames);
        ethGames = NULL;
        free(ethPs1Games);
        ethPs1Games = NULL;
        ethGameCount = 0;
        ethPs1GameCount = 0;
        // disconnect from the active SMB session
        if ((exception & UNMOUNT_EXCEPTION) == 0)
            ethSMBDisconnect();
    }

    // UI may have initialized modules outside of ETH mode, so deinitialize regardless of the enabled status.
    ethDeinitModules();
}

// This may be called, even if ethInit() was not.
static void ethShutdown(item_list_t *itemList)
{
    if (ethGameList.enabled) {
        LOG("ETHSUPPORT Shutdown\n");

        free(ethGames);
        ethGames = NULL;
        free(ethPs1Games);
        ethPs1Games = NULL;
        ethGameCount = 0;
        ethPs1GameCount = 0;
        // disconnect from the active SMB session
        ethSMBDisconnect();
    }

    // UI may have initialized modules outside of ETH mode, so deinitialize regardless of the enabled status.
    int ethWasLoaded = netGetModulesLoaded(); // capture BEFORE ethDeinitModules() tears the stack down
    ethDeinitModules();

    // Only shut down dev9 from here, if it was initialized from here before.
    if (ethWasLoaded)
        sysShutdownDev9();
}

static int ethCheckVMC(item_list_t *itemList, char *name, int createSize)
{
    return sysCheckVMC(ethPrefix, "\\", name, createSize, NULL);
}

static char *ethGetPrefix(item_list_t *itemList)
{
    return ethPrefix;
}

const char *ethGetSMBPrefix(void)
{
    return ethPrefix;
}

static item_list_t ethGameList = {
    ETH_MODE, 1, 0, 0, MENU_MIN_INACTIVE_FRAMES, ETH_MODE_UPDATE_DELAY, NULL, NULL, &ethGetTextId, &ethGetPrefix, &ethInit, &ethNeedsUpdate,
    &ethUpdateGameList, &ethGetGameCount, &ethGetGame, &ethGetGameName, &ethGetGameNameLength, &ethGetGameStartup, &ethDeleteGame, &ethRenameGame,
    &ethLaunchGame, &ethGetConfig, &ethGetImage, &ethCleanUp, &ethShutdown, &ethCheckVMC, &ethGetIconId, &ethLaunchVcd,
    ITEM_VIEW_NATIVE, NULL, &ethLaunchCue, &ethGetItemView, &ethGetSourceId};

int ethGetNetConfig(u8 *ip_address, u8 *netmask, u8 *gateway)
{
    return netGetConfig(ip_address, netmask, gateway);
}

int ethGetDHCPStatus(void)
{
    return netGetDHCPStatus();
}
