#include "include/opl.h"
#include "include/lang.h"
#include "include/gui.h"
#include "include/supportbase.h"
#include "include/udpfssupport.h"
#include "include/vcdsupport.h"
#include "include/folderbrowse.h"
#include "include/util.h"
#include "include/renderman.h"
#include "include/themes.h"
#include "include/textures.h"
#include "include/ioman.h"
#include "include/system.h"
#include "include/extern_irx.h"
#include "include/cheatman.h"
#include "include/ethsupport.h"  // ethGetModulesLoaded() for the UDPFS<->SMB NIC interlock
#include "include/bdmsupport.h"  // bdmIsUDPBDLoaded() for the UDPFS<->UDPBD NIC interlock
#include "include/mmcesupport.h" // mmceSendGameID() cross-device game-id (#261)
#include "modules/iopcore/common/cdvd_config.h"

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h>

// udpfs is a network FILESYSTEM device: the iomanX device "udpfs:" is served by the udpfs_ioman IRX
// chain (smap -> ministack(ip=) -> udpfs_ioman). Games are read straight off "udpfs:/" and boot through
// the external Neutrino core (this device has no embedded OPL cdvdman backend), exactly like the "udp"
// BDM transport in bdmsupport.c -- but here the mount is a filesystem, not a block device.
static char udpfsPrefix[40]; // full path to the folder that holds all the games ("udpfs:/")
static char *udpfsBase;      // device root ("udpfs:")
static int udpfsULSizePrev = -2;
static time_t udpfsModifiedCDPrev;
static time_t udpfsModifiedDVDPrev;
static int udpfsGameCount = 0;
static base_game_info_t *udpfsGames = NULL;
static int udpfsIomanModLoaded = 0;

// forward declaration
static item_list_t udpfsGameList;

// Load the UDPFS ioman IRX chain ONCE per boot. Mirrors bdmsupport's udpfs_bd chain load
// (bdmLoadBlockDeviceModules), but loads udpfs_ioman (the FILESYSTEM variant) instead of udpfs_bd (the
// BLOCK-device variant): smap (exports to ministack + ioman) -> ministack (gets the "ip=" arg) ->
// udpfs_ioman (registers the "udpfs:" iomanX device).
//
// SOCKET-ONCE: the ministack has no udp_unbind, so this is load-once -- never DelDrv+re-AddDrv and never
// re-load (a re-bind of the UDPRDMA socket bricks UDPFS after a few cycles). Once udpfsIomanModLoaded is
// set it stays set for the life of the boot.
static void udpfsLoadModules(void)
{
    char ipArg[24];

    LOG("UDPFSSUPPORT LoadModules\n");

    if (udpfsIomanModLoaded)
        return;

    // NIC-exclusivity: the SMAP adapter registers a single "SMAP_driver". The SMB/ETH stack and the
    // UDPBD/udpfs_bd block chain both own it, so refuse to bring up the UDPFS ioman stack on top of
    // either. The Device-hub UI already interlocks these; this is the runtime backstop.
    if (ethGetModulesLoaded()) {
        LOG("UDPFSSUPPORT: SMB NIC active -- not loading the UDPFS ioman stack\n");
        return;
    }
    if (bdmIsUDPBDLoaded()) {
        LOG("UDPFSSUPPORT: UDPBD/udpfs_bd active -- not loading the UDPFS ioman stack\n");
        return;
    }

    // dev9 is refcounted (shared with ATA-HDD). The ministack has no DHCP client, so it needs the PS2's
    // static IP as an "ip=A.B.C.D" arg -- built exactly as bdmsupport does for the udpfs_bd chain.
    sysInitDev9();
    snprintf(ipArg, sizeof(ipArg), "ip=%d.%d.%d.%d", ps2_ip[0], ps2_ip[1], ps2_ip[2], ps2_ip[3]);

    LOG("[UDPFS_SMAP]:\n");
    if (sysLoadModuleBuffer(&udpfs_smap_irx, size_udpfs_smap_irx, 0, NULL) >= 0) {
        LOG("[UDPFS_MINISTACK]:\n");
        if (sysLoadModuleBuffer(&udpfs_ministack_irx, size_udpfs_ministack_irx, (int)strlen(ipArg) + 1, ipArg) >= 0) {
            LOG("[UDPFS_IOMAN]:\n");
            if (sysLoadModuleBuffer(&udpfs_ioman_irx, size_udpfs_ioman_irx, 0, NULL) >= 0) {
                udpfsIomanModLoaded = 1;
                LOG("UDPFSSUPPORT Modules loaded\n");
                return;
            }
        }
    }

    // Release the dev9 reference taken above if any load failed -- otherwise a failing/retrying UDPFS
    // inflates the refcounted dev9InitCount and a later HDD/ETH teardown can never power dev9 down.
    LOG("UDPFSSUPPORT: module chain failed to load\n");
    sysShutdownDev9();
}

int udpfsGetModulesLoaded(void)
{
    return udpfsIomanModLoaded;
}

void udpfsInit(item_list_t *itemList)
{
    LOG("UDPFSSUPPORT Init\n");
    udpfsBase = "udpfs:";
    // The games live directly at the device root; no share/prefix to prepend (unlike SMB).
    snprintf(udpfsPrefix, sizeof(udpfsPrefix), "udpfs:/");
    udpfsULSizePrev = -2;
    udpfsModifiedCDPrev = 0;
    udpfsModifiedDVDPrev = 0;
    udpfsGameCount = 0;
    udpfsGames = NULL;
    // NAV-PARITY TEST BUILD (#340): official's fixed 8-frame art settle, not the fork Art Delay setting.
    udpfsGameList.delay = MENU_MIN_INACTIVE_FRAMES;
    ioPutRequest(IO_CUSTOM_SIMPLEACTION, &udpfsLoadModules);
    udpfsGameList.enabled = 1;
}

item_list_t *udpfsGetObject(int initOnly)
{
    if (initOnly && !udpfsGameList.enabled)
        return NULL;
    return &udpfsGameList;
}

static int udpfsNeedsUpdate(item_list_t *itemList)
{
    int result = 0;

    // VCD view: force a rescan once on toggle, then refresh on toggle only (skip disc heuristics).
    if (vcdConsumeDirty(itemList->mode))
        return 1;
    // Folder browsing: descend/ascend forces one rescan.
    if (folderConsumeDirty(itemList->mode))
        return 1;
    if (vcdViewActive(itemList->mode))
        return 0;

    if (udpfsULSizePrev == -2)
        result = 1;

    // Only stat the mount once the ioman device is actually up -- statting "udpfs:/..." before then
    // just fails and would keep result at 0 anyway, but guard it to avoid pointless RPC churn.
    if (udpfsIomanModLoaded) {
        struct stat st;
        char path[256];

        sprintf(path, "%sCD", udpfsPrefix);
        if (stat(path, &st) != 0)
            st.st_mtime = 0;
        if (udpfsModifiedCDPrev != st.st_mtime) {
            udpfsModifiedCDPrev = st.st_mtime;
            result = 1;
        }

        sprintf(path, "%sDVD", udpfsPrefix);
        if (stat(path, &st) != 0)
            st.st_mtime = 0;
        if (udpfsModifiedDVDPrev != st.st_mtime) {
            udpfsModifiedDVDPrev = st.st_mtime;
            result = 1;
        }

        if (!sbIsSameSize(udpfsPrefix, udpfsULSizePrev))
            result = 1;
    }

    return result;
}

static int udpfsUpdateGameList(item_list_t *itemList)
{
    if (udpfsIomanModLoaded == 0)
        return 0;

    if (vcdViewActive(itemList->mode)) {
        int r = vcdFillGameList(udpfsPrefix, &udpfsGames);
        if (r >= 0) // r < 0: transient scan failure -> preserve the last-good list
            udpfsGameCount = r;
    } else if (sbReadList(&udpfsGames, udpfsPrefix, folderGetSub(itemList->mode), &udpfsULSizePrev, &udpfsGameCount) < 0) {
        udpfsGameCount = 0;
    }
    return udpfsGameCount;
}

static int udpfsGetGameCount(item_list_t *itemList)
{
    return udpfsGameCount;
}

static void *udpfsGetGame(item_list_t *itemList, int id)
{
    return (void *)&udpfsGames[id];
}

static char *udpfsGetGameName(item_list_t *itemList, int id)
{
    return udpfsGames[id].name;
}

static int udpfsGetGameNameLength(item_list_t *itemList, int id)
{
    if (udpfsGames[id].format != GAME_FORMAT_USBLD)
        return ISO_GAME_NAME_MAX + 1;
    else
        return UL_GAME_NAME_MAX + 1;
}

static char *udpfsGetGameStartup(item_list_t *itemList, int id)
{
    // VCD view keys per-game data (CFG/art) off the VCD filename, not a disc ID (see sbPopulateConfig).
    if (vcdViewActive(itemList->mode))
        return udpfsGames[id].name;
    return udpfsGames[id].startup;
}

static void udpfsDeleteGame(item_list_t *itemList, int id)
{
    sbSetBrowseSub(folderGetSub(itemList->mode)); // delete inside the current subfolder, not the root
    sbDelete(&udpfsGames, udpfsPrefix, "/", udpfsGameCount, id);
    udpfsULSizePrev = -2;
}

static void udpfsRenameGame(item_list_t *itemList, int id, char *newName)
{
    sbSetBrowseSub(folderGetSub(itemList->mode)); // rename inside the current subfolder, not the root
    sbRename(&udpfsGames, udpfsPrefix, "/", udpfsGameCount, id, newName);
    udpfsULSizePrev = -2;
}

// udpfs is a network filesystem: POPSTARTER does its own IOP reset and cannot bring the udpfs: mount
// back up (no BDMA variant), so a PS1 VCD here would just drop to OSDSYS. Abort cleanly with a message,
// exactly like bdmLaunchVcd's BDM_TYPE_UDPBD guard.
static void udpfsLaunchVcd(item_list_t *itemList, const char *vcdName, config_set_t *configSet)
{
    guiMsgBox(_l(_STR_VCD_NOT_ON_NET), 0, NULL);
    return;
}

static void udpfsLaunchGame(item_list_t *itemList, int id, config_set_t *configSet)
{
    int compatmask = 0, index;
    int EnablePS2Logo = 0;
    int result;
    char filename[32], partname[256];
    base_game_info_t *game = &udpfsGames[id];

    // Folder browsing: a folder row is never launched (the dispatch descends first); guard defensively
    // and pin the path composers to the current subfolder so a nested game resolves.
    if (game != NULL && game->format == GAME_FORMAT_FOLDER)
        return;
    sbSetBrowseSub(folderGetSub(itemList->mode));

    // VCD view: udpfs cannot host a POPSTARTER launch (network filesystem) -> hand off to the guard.
    if (game != NULL && vcdViewActive(itemList->mode)) {
        udpfsLaunchVcd(itemList, game->name, configSet);
        return;
    }

    // The udpfs device has NO embedded OPL cdvdman backend -- games boot ONLY via the external Neutrino
    // core. Resolve the neutrino.elf below (while `game`/`configSet` are still valid -- deinit frees them),
    // and reject formats Neutrino cannot run (UL-split / .zso), mirroring bdmsupport's udp Neutrino path.
    // NB: VMC emulation is emitted as discrete -mcN Neutrino args (sbBuildVmcNeutrinoArgs below), not via
    // an OPL mcemu IRX, so no smb/bdm-style mcemu patch loop is needed here.

    if (gRememberLastPlayed) {
        configSetStr(configGetByType(CONFIG_LAST), "last_played", game->startup);
        saveConfig(CONFIG_LAST, 0);
    }

    // sbPrepare resolves the per-game compat mask (and per-game pad-emu etc.). The udpfs launch does not
    // patch an OPL cdvdman IRX, but sbPrepare still needs a valid buffer to locate the patch zone; reuse
    // the smb cdvdman image as a scratch target (its patched fields are never launched here -- only the
    // returned compatmask is used). A follow-up stage finalizes the exact backend wiring.
    compatmask = sbPrepare(game, configSet, size_smb_cdvdman_irx, smb_cdvdman_irx, &index);
    if (compatmask < 0)
        return;

    if ((result = sbLoadCheats(udpfsPrefix, game->startup)) < 0) {
        // #265: let the user back out instead of sitting through the whole load. The helper does
        // the sbUnprepare itself -- see include/supportbase.h; skipping it breaks the NEXT launch.
        // udpfs never assigns `settings` (Neutrino reads the game, not an OPL cdvdman), but
        // sbPrepare still patched smb_cdvdman_irx, so the unwind is required all the same.
        if (!sbCheatsMissingContinue((u8 *)(&smb_cdvdman_irx) + index, result))
            return;
    }
    sbLoadImage(udpfsPrefix, game->startup);

    if (gPS2Logo) {
        int fd;
        sbCreatePath(game, partname, udpfsPrefix, "/", 0);
        fd = open(partname, O_RDONLY, 0666);
        if (fd >= 0) {
            EnablePS2Logo = CheckPS2Logo(fd, 0);
            close(fd);
        }
    }

    if (configGetStrCopy(configSet, CONFIG_ITEM_ALTSTARTUP, filename, sizeof(filename)) == 0)
        strcpy(filename, game->startup);

    // Per-game Neutrino ELF + flags, resolved BEFORE deinit frees configSet's owner.
    const char *neutrinoPath = NULL;
    char neutrinoExtraArgs[256] = "";
    int neutrinoVideo = gNeutrinoVideoDefault, neutrinoGsmComp = gNeutrinoGsmCompDefault; // absent per-game keys = follow the globals
    neutrino_vmc_args_t neutrinoVmc = {0};

    configGetStrCopy(configSet, CONFIG_ITEM_NEUTRINO_ARGS, neutrinoExtraArgs, sizeof(neutrinoExtraArgs));
    configGetInt(configSet, CONFIG_ITEM_NEUTRINO_VIDEO, &neutrinoVideo);
    configGetInt(configSet, CONFIG_ITEM_NEUTRINO_GSMCOMP, &neutrinoGsmComp);
    neutrinoPath = sbResolveNeutrinoPath(udpfsPrefix);

    // Format / neutrino availability check: on failure show the network-specific message (the OPL-core
    // fallback the generic _STR_NEUTRINO_* warnings promise does not exist for a network filesystem) and
    // abort cleanly while the GUI is still up (no deinit into a no-op launch = black screen).
    if (game->format == GAME_FORMAT_USBLD || !strcasecmp(game->extension, ".zso")) {
        guiWarning(_l(_STR_NET_NEEDS_NEUTRINO), 6);
        return;
    }
    if (neutrinoPath == NULL) {
        guiWarning(_l(_STR_NET_NEEDS_NEUTRINO), 6);
        return;
    }

    // VMC -> neutrino (#47): resolve any per-game VMC into discrete -mc0/-mc1 argv entries before deinit.
    sbBuildVmcNeutrinoArgs(configSet, udpfsPrefix, &neutrinoVmc);

    // Build the filesystem device path Neutrino reads the game from. This is a plausible, correct-shaped
    // -dvd target (the udpfs: filesystem game file); a follow-up stage finalizes the exact -dvd/-bsd argv.
    sbCreatePath(game, partname, udpfsPrefix, "/", 0);

    // MMCE cross-device game-id (#261): push the disc id to a present MMCE card before teardown frees
    // `game`. Neutrino path forwarded so a Neutrino launch protects the MMCE hand-off timing.
    mmceSendGameID(game->startup, neutrinoPath,
                   (neutrinoVmc.arg[0][0] ? 1 : 0) | (neutrinoVmc.arg[1][0] ? 2 : 0)); // Δ3: -mc-covered slots keep their card

    // Neutrino keep-IOP handoff (sysLoadELFKeepIOP): Neutrino reads the game through OUR udpfs
    // filesystem and its config/modules from the neutrino.elf device (-cwd) before its own IOP
    // reset -- keep BOTH mounted across the teardown. Mirrors bdmsupport's Neutrino deinit contract.
    // D6: pre-teardown validation -- for UDPFS this is where the bsd toml ip= sync now happens
    // (was post-deinit inside sysLaunchNeutrino); a failure toasts and stays in the menu.
    if (sysNeutrinoPreflight("udpfs", neutrinoPath) < 0)
        return;
    int neutrinoDevMode = oplPath2Mode(neutrinoPath);
    deinitEx(UNMOUNT_EXCEPTION, itemList->mode, neutrinoDevMode); // CAREFUL: itemCleanUp still frees udpfsGames/game

    // Hand off to Neutrino with the udpfs driver token. `partname` (the filesystem game path) + the token
    // survive the deinit above; `game` does not and is not used past this point.
    sysLaunchNeutrino("udpfs", partname, game->startup, compatmask, EnablePS2Logo, neutrinoPath, neutrinoExtraArgs, neutrinoVideo, neutrinoGsmComp, 0 /* #11: udpfs is fileid, no fs layer */, &neutrinoVmc);
}

static config_set_t *udpfsGetConfig(item_list_t *itemList, int id)
{
    return sbPopulateConfig(&udpfsGames[id], udpfsPrefix, "/");
}

static int udpfsGetImage(item_list_t *itemList, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)
{
    char path[256];

    // OPL's own ART/<name>_COV.png is the PRIMARY lookup (same path PS2 uses; the cache also retries once
    // with a strict PS1 ID).
    if (isRelative)
        snprintf(path, sizeof(path), "%s%s/%s_%s", udpfsPrefix, folder, value, suffix);
    else
        snprintf(path, sizeof(path), "%s%s_%s", folder, value, suffix);
    int r = texDiscoverLoad(resultTex, path, -1);
    // On a VCD (PS1) genuine miss, fall back to the POPSLoader-style suffixless cover next to the .VCD.
    // Cover/icon only, VCD view only.
    if (r == ERR_BAD_FILE && isRelative && vcdViewActive(itemList->mode))
        r = vcdLoadPopsCover(udpfsPrefix, value, suffix, resultTex);
    return r;
}

static int udpfsGetTextId(item_list_t *itemList)
{
    return _STR_UDPFS_GAMES;
}

static int udpfsGetIconId(item_list_t *itemList)
{
    return UDPFS_ICON;
}

// This may be called, even if udpfsInit() was not.
static void udpfsCleanUp(item_list_t *itemList, int exception)
{
    if (udpfsGameList.enabled) {
        LOG("UDPFSSUPPORT CleanUp\n");
        free(udpfsGames);
        udpfsGames = NULL;
        udpfsGameCount = 0;
    }
    // SOCKET-ONCE: the ministack has no udp_unbind, so the module chain is intentionally NOT torn down
    // here (never DelDrv+re-AddDrv the UDPRDMA socket). udpfsIomanModLoaded stays set for the boot.
    (void)exception;
}

// This may be called, even if udpfsInit() was not.
static void udpfsShutdown(item_list_t *itemList)
{
    if (udpfsGameList.enabled) {
        LOG("UDPFSSUPPORT Shutdown\n");
        free(udpfsGames);
        udpfsGames = NULL;
        udpfsGameCount = 0;
    }
    // SOCKET-ONCE: keep the ioman stack loaded (no unbind path). dev9 was init'd once in udpfsLoadModules
    // and is left to the shared dev9 refcount / final power-off.
}

static int udpfsCheckVMC(item_list_t *itemList, char *name, int createSize)
{
    return sysCheckVMC(udpfsPrefix, "/", name, createSize, NULL);
}

static char *udpfsGetPrefix(item_list_t *itemList)
{
    return udpfsPrefix;
}

static item_list_t udpfsGameList = {
    UDPFS_MODE, 1, 0, 0, MENU_MIN_INACTIVE_FRAMES, UDPFS_MODE_UPDATE_DELAY, NULL, NULL, &udpfsGetTextId, &udpfsGetPrefix, &udpfsInit, &udpfsNeedsUpdate,
    &udpfsUpdateGameList, &udpfsGetGameCount, &udpfsGetGame, &udpfsGetGameName, &udpfsGetGameNameLength, &udpfsGetGameStartup, &udpfsDeleteGame, &udpfsRenameGame,
    &udpfsLaunchGame, &udpfsGetConfig, &udpfsGetImage, &udpfsCleanUp, &udpfsShutdown, &udpfsCheckVMC, &udpfsGetIconId, &udpfsLaunchVcd};
