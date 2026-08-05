#include "include/opl.h"
#include "include/lang.h"
#include "include/gui.h"
#include "include/supportbase.h"
#include "include/mmcesupport.h"
#include "include/vcdsupport.h"
#include "include/folderbrowse.h"
#include "include/util.h"
#include "include/themes.h"
#include "include/textures.h"
#include "include/texcache.h"
#include "include/ioman.h"
#include "include/system.h"
#include "include/extern_irx.h"
#include "include/cheatman.h"
#include "modules/iopcore/common/cdvd_config.h"
#include "../ee_core/include/coreconfig.h"
#include <usbhdfsd-common.h>

#include <kernel.h>
#include <ps2sdkapi.h>
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h> // fileXioIoctl, fileXioDevctl
#include <delaythread.h> // DelayThread() -- real-sleep gap in the MMCE card-switch wait

static char mmcePrefix[40]; // Contains the full path to the folder where all the games are.
static char mmceArtPrimary[40];
static int mmceULSizePrev = -2;
// VCD-scan self-heal (HW batch S6): a failed vcdFillGameList on a CONTENDED bus (-1) used to latch
// an empty VCD page under NOUPDATE with no rescan for the session. Bounded retry, same shape as the
// ISO first-scan sentinel: ~15 passes at the ~2s cadence, re-armed by a fresh L3 toggle, self-
// quiescing on success/expiry. (The other half of the old "-1 for both" ambiguity is now split at
// the source: vcdScanOpenDir returns 0 for a genuinely-ABSENT POPS folder -- #154 residual -- so
// only contention ever arms this budget.)
static unsigned char mmceVcdScanFailed = 0;
static unsigned char mmceVcdScanRetries = 0;
#define MMCE_VCD_SCAN_RETRY_MAX 15
static time_t mmceModifiedCDPrev;
static time_t mmceModifiedDVDPrev;
static int mmceGameCount = 0;
static base_game_info_t *mmceGames;
// #120: the PS2 (ISO) and PS1 (VCD) views must NOT share one backing store. When they did, a failed
// ISO rescan on a contended MMCE bus fell into sbReadList's preserve-on-failure and re-published the
// STALE list -- which, being shared, was the VCD list -> pressing "show PS2 games" silently kept the PS1
// list on screen (Andrew's #129 "can't switch to PS2 list"). Separate arrays (mirroring hddGames vs
// hddVcdGames) make a failed scan of one view preserve only THAT view's last-good (empty if never
// scanned), so it can never resurrect the other view's contents.
static int mmceVcdGameCount = 0;
static base_game_info_t *mmceVcdGames = NULL;
// Auto-slot (gMMCESlot==2) resolution cache: mmceDetectSlot()'s last result (2=mmce0, 3=mmce1,
// -1=unresolved). Avoids re-probing BOTH slots over SIO2 every menu refresh -- that steady devctl
// drip contends with MX4SIO on the shared bus. Reset by mmceInit (tab re-enable / settings apply).
static int mmceResolvedDevice = -1;
// sbCreateFolders() issues ~10 mkdir devctls; on an mmceN: card each is an SIO2 round-trip that
// contends with MX4SIO. Remember the prefix we last created folders for so a refresh on an unchanged
// card/slot skips the redundant burst (mirrors BDM's FoldersCreated one-shot). Reset by mmceInit and
// on card removal (empty prefix) so a freshly inserted card still gets its folders.
static char mmceFoldersCreatedFor[40] = {0};
#define MMCE_FOLDER_RETRY_MAX 5 // bounded CFG-create retries per card before declaring it obstructed
static unsigned char mmceFolderRetries = 0;
// Auto-slot presence debounce (#154 audit residual): the cache-hit probe in mmceSetPrefix used to
// invalidate the slot on ONE failed presence devctl -- and mmceUpdateGameList then frees BOTH game
// lists -- so a transiently contended SIO2 bus (list scan + art read + MX4SIO traffic all sharing
// it) read as "card pulled" and both lists vanished until the next ~2s refresh re-detected.
// Require MMCE_PRESENCE_PROBE_MAX CONSECUTIVE failures before invalidating, re-probing INLINE with
// a short gap (the same 200 ms DelayThread cadence the GameID settle loops use): worst case ~1.0 s
// on a genuine pull (3x the devctl's own 200 ms timeout + 2 gaps), still inside the ~2s refresh
// cadence, so a pull is still detected on THIS pass -- just two probes later -- while a bus-quiet
// window between probes absorbs the transient. No persistent counter: any successful probe breaks
// the loop and declares the card present.
#define MMCE_PRESENCE_PROBE_MAX 3
#define MMCE_PRESENCE_RETRY_US  (200 * 1000)

// Card-switch wait: poll the MMCE busy bit every 500 ms for up to ~7.5 s, matching mmceman's own
// switch handshake. On a CROSS-DEVICE launch (a USB/HDD/SMB game whose per-game card lives on the
// MMCE) the 0x8 push physically switches the SD card, and the launch MUST wait for that to finish --
// otherwise the game boots while the card is still mounting and freezes at its MC check (issue #50,
// cross-device path). A prior change had collapsed the per-poll gap to a sub-ms nopdelay(), which
// gutted the wait so it returned in well under a second; a real sleep restores it.
#define MMCE_GAMEID_WAIT_TICKS    15           // max polls of the card-switch busy bit
#define MMCE_GAMEID_POLL_US       (200 * 1000) // 200 ms between polls -> ~3 s total budget (was 500 ms x 15 = 7.5 s)
/* The MMCE worker checks its abort flag between 32 KB staged reads. A
 * 500 ms wait gives a slow card time to reach the next safe checkpoint. */
#define MMCE_ART_ABORT_WAIT_TICKS 500

// forward declaration
static item_list_t mmceGameList;
static void mmceGetDeviceRoot(char *root, size_t size);
static int mmceModLoaded = 0;          // latched by mmceLoadModules; read by mmceSendGameID's arm check
static char mmceGameIdTarget[8] = {0}; // last device a GameID 0x8 switch was SENT to (mmceGameIdSettle polls it)

int mmceSendGameID(const char *startup, const char *protectMcPath, int vmcSlotMask)
{
    char mmceDevice[sizeof(mmcePrefix)];

    mmceGameIdTarget[0] = '\0'; // no stale target: only a send made by THIS call may be settled against

    if (!gMMCEEnableGameID || startup == NULL || startup[0] == '\0')
        return 0;

    // Δ4 (NHDDL parity): do NOT self-arm the transport here. Loading an IRX inside the launch
    // sequence puts a module load/start at the launch's most fragile moment; the arming now happens
    // at menu/settings time (mmceArmGameIDTransport, called from initAllSupport whenever the GameID
    // feature is on), where a failure is a harmless LOG. Preserves the #51 intent -- GameID works
    // without the MMCE page ever being enabled -- with the risk moved off the launch path. If the
    // module is not resident by launch time (arm failed / raced), skip gracefully like no-card.
    if (!mmceModLoaded) {
        LOG("MMCE GameID: transport not armed -- skipping (menu-time arm failed or pending)\n");
        return 0;
    }

    // Candidate order: the configured/resolved slot first, then both slots as fallback -- a card in
    // EITHER slot still gets the game-id on a cross-device launch (#261). Same 0x1 presence devctl
    // as mmceDetectSlot. Δ3 (NHDDL parity): a slot whose -mc<slot> Neutrino arg is set gets its MC
    // from the VMC FILE, not the card's per-game folder -- switching the physical card for it is
    // moot and only adds the busy/re-mount window, so covered slots are skipped; a card in the
    // OTHER, uncovered slot still gets the push. vmcSlotMask bit N = "-mcN arg present" (0 = the
    // OPL-core paths, which use mcemu and keep today's behavior).
    mmceGetDeviceRoot(mmceDevice, sizeof(mmceDevice));
    {
        const char *cands[3] = {mmceDevice[0] != '\0' ? mmceDevice : NULL, "mmce0:/", "mmce1:/"};
        int tried[2] = {0, 0};
        int found = 0;
        for (int c = 0; c < 3 && !found; c++) {
            if (cands[c] == NULL || strlen(cands[c]) < 5)
                continue;
            int slot = cands[c][4] - '0';
            if (slot < 0 || slot > 1 || tried[slot])
                continue;
            tried[slot] = 1;
            if ((vmcSlotMask >> slot) & 1) {
                LOG("MMCE GameID: slot %d covered by a -mc%d VMC arg -- not switching that card\n", slot, slot);
                continue;
            }
            if (fileXioDevctl(cands[c], 0x1, NULL, 0, NULL, 0) != -1) {
                if (cands[c] != mmceDevice)
                    snprintf(mmceDevice, sizeof(mmceDevice), "%s", cands[c]);
                found = 1;
            }
        }
        if (!found)
            return 0; // no eligible MMCE card present -> graceful no-op
    }

    // NHDDL-parity guard (#51): never switch the per-game card on a slot whose EMULATED memory card
    // (mcN:) holds the neutrino.elf we are about to load -- the 0x8 switch moves the mcN: surface and
    // would yank the loader out from under sysLaunchNeutrino. A neutrino.elf on the MMCE's SD (mmceN:)
    // is NOT affected by the card switch, so only the mcN: case is guarded.
    if (protectMcPath != NULL && protectMcPath[0] != '\0') {
        const char *mc = NULL;
        if (!strncmp(mmceDevice, "mmce0", 5))
            mc = "mc0:";
        else if (!strncmp(mmceDevice, "mmce1", 5))
            mc = "mc1:";
        if (mc != NULL && !strncmp(protectMcPath, mc, strlen(mc))) {
            // neutrino.elf is on this slot's emulated card -- switching would pull the loader out from
            // under sysLaunchNeutrino. Leave the card as-is so the launch still works, but SOFT-FAIL with
            // a transient notice (was a silent no-op) so the user understands their per-game card folder
            // wasn't applied this launch -- the only situation GameID + MMCE + neutrino-on-mc can collide.
            guiWarning(_l(_STR_MMCE_GAMEID_NEUTRINO_SKIP), 6);
            return 0;
        }
    }

    if (fileXioDevctl(mmceDevice, 0x8, (void *)startup, (strlen(startup) + 1), NULL, 0) < 0)
        return 0;

    // Remember the slot this send actually targeted -- mmceGameIdSettle() (the MX4SIO pre-launch
    // gate, batch S7) must poll the SAME device, not a guess.
    snprintf(mmceGameIdTarget, sizeof(mmceGameIdTarget), "%s", mmceDevice);

    // Wait until the busy bit clears -- i.e. until the physical card has finished switching to the
    // per-game folder. This runs on the single GUI thread BEFORE deinit, so every millisecond here is a
    // frozen loading screen. POLL FIRST, sleep only if still busy: a card that switches instantly (the
    // common case) now costs ~0 ms instead of a guaranteed 500 ms (the old loop slept 500 ms before its
    // first poll, taxing EVERY cross-device launch -- a regression on slow late-slim MC buses). The total
    // budget is generous enough (~3 s) to still cover a slow switch before we launch anyway (#50 race),
    // but no longer the 7.5 s worst case that read as a hard freeze on hardware. Break the instant it clears.
    for (int i = 0; i < MMCE_GAMEID_WAIT_TICKS; i++) {
        int status = fileXioDevctl(mmceDevice, 0x2, NULL, 0, NULL, 0);
        if (status < 0)
            break; // busy-bit query unsupported/failed -> don't block the launch

        if ((status & 1) == 0) {
            LOG("Set MMCE GameID to: %s\n", startup);
            return 1; // card finished switching (settle CONFIRMED)
        }

        DelayThread(MMCE_GAMEID_POLL_US); // still busy -> wait a short interval, then re-poll
    }

    // Tri-state (batch S7): -1 = the 0x8 switch WAS sent but the settle was never confirmed (busy
    // query unsupported, or the budget expired). Truthy on purpose -- the mmce-launch callers test
    // truthiness and must still run their own settle; only the MX4SIO cross-device gate
    // (mmceGameIdSettle) distinguishes -1 from 1, because there the un-settled switch shares SIO2
    // with the SD enumeration the launch is about to depend on.
    LOG("MMCE GameID switch not confirmed within budget; launching anyway\n");
    return -1;
}

// Bounded post-GameID settle for cross-device launches that share SIO2 with the MMCE (MX4SIO, batch
// S7: the lime-green hang is cdvdman waiting forever for the SD card to enumerate; an mmce switch
// still in flight during the IOP reboot can starve that enumeration). Polls the exact device the
// last send targeted: settled when presence answers AND the busy bit is clear OR unavailable --
// requiring a readable busy bit would turn every busy-devctl-less firmware into a guaranteed full
// stall. Returns 0 settled, -1 expired. NEVER blocks the launch -- the caller toasts and proceeds.
int mmceGameIdSettle(int timeoutMs)
{
    if (mmceGameIdTarget[0] == '\0')
        return 0;
    for (int waited = 0; waited <= timeoutMs; waited += 200) {
        if (fileXioDevctl(mmceGameIdTarget, 0x1, NULL, 0, NULL, 0) != -1) {
            int busy = fileXioDevctl(mmceGameIdTarget, 0x2, NULL, 0, NULL, 0);
            if (busy < 0 || (busy & 1) == 0)
                return 0;
        }
        DelayThread(200 * 1000);
    }
    LOG("MMCE GameID settle NOT confirmed after %d ms\n", timeoutMs);
    return -1;
}

static void mmceGetDeviceRoot(char *root, size_t size)
{
    const char *separator = strstr(mmcePrefix, ":/");
    size_t length;

    if (root == NULL || size == 0)
        return;

    if (separator != NULL) {
        length = (size_t)(separator - mmcePrefix) + 2;
        if (length >= size)
            length = size - 1;

        memcpy(root, mmcePrefix, length);
        root[length] = '\0';
        return;
    }

    if (gMMCESlot == 0)
        snprintf(root, size, "mmce0:/");
    else if (gMMCESlot == 1)
        snprintf(root, size, "mmce1:/");
    else
        root[0] = '\0';
}

// Fs-settle after a GameID card switch. The 0x8 devctl physically re-mounts the card, and on Gen2
// the busy bit (mmceSendGameID's own wait) can clear before the FILESYSTEM surface is back. Probe
// the switched slot until a directory open answers: poll-first so a fast card costs ~0 ms; bounded
// (~5 s) so a dead card can't hang; LOG each outcome so a debug ELF can localise a black screen to
// OPL vs the loaded core. The helper may have fallen back to the OTHER slot when the game's slot
// has no card (or is -mc-covered), so stay consistent: if the game's slot isn't present, settle the
// other slot instead -- otherwise we'd probe an empty slot for the full ~5 s (PR #89 review). Same
// 0x1 presence devctl mmceSendGameID itself uses.
static void mmceSettleAfterSwitch(void)
{
    char mmceRoot[sizeof(mmcePrefix)];
    mmceGetDeviceRoot(mmceRoot, sizeof(mmceRoot));
    if (mmceRoot[0] != '\0' && strlen(mmceRoot) >= 5 &&
        fileXioDevctl(mmceRoot, 0x1, NULL, 0, NULL, 0) == -1)
        mmceRoot[4] = (mmceRoot[4] == '0') ? '1' : '0'; // mmce0:/ <-> mmce1:/
    if (mmceRoot[0] == '\0')
        return;
    int settled = 0, settle;
    for (settle = 0; settle < 25; settle++) {
        int dfd = fileXioDopen(mmceRoot);
        if (dfd >= 0) {
            fileXioDclose(dfd);
            settled = 1;
            break;
        }
        DelayThread(200 * 1000);
    }
    if (settled)
        LOG("MMCE settle: %s fs surface up after ~%d ms\n", mmceRoot, settle * 200);
    else
        LOG("MMCE settle: %s fs surface not back within ~5000 ms; launching anyway\n", mmceRoot);
}

static void mmceRefreshArtRoots(void)
{
    int len;

    mmceArtPrimary[0] = '\0';

    if (mmcePrefix[0] == '\0')
        return;

    /* Ensure mmcePrefix always ends with '/' so path concatenation is correct
     * (e.g. "mmce0:/CD" -> "mmce0:/CD/" prevents "mmce0:/CDART" paths). */
    len = strlen(mmcePrefix);
    if (len < (int)sizeof(mmcePrefix) - 1 && mmcePrefix[len - 1] != '/') {
        mmcePrefix[len] = '/';
        mmcePrefix[len + 1] = '\0';
    }

    snprintf(mmceArtPrimary, sizeof(mmceArtPrimary), "%s", mmcePrefix);
}

static int mmceTryLoadImage(const char *prefix, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex)
{
    char path[256];

    if ((prefix == NULL || prefix[0] == '\0') && isRelative)
        return -1;

    if (isRelative)
        snprintf(path, sizeof(path), "%s%s/%s_%s", prefix, folder, value, suffix);
    else
        snprintf(path, sizeof(path), "%s%s_%s", folder, value, suffix);

    return texDiscoverLoad(resultTex, path, -1);
}

int mmceDetectSlot(void)
{
    int ret = -1;
    if (fileXioDevctl("mmce0:/", 0x1, NULL, 0, NULL, 0) != -1) {
        sprintf(mmcePrefix, "mmce0:/%s", gMMCEPrefix);
        ret = 2;
    } else if (fileXioDevctl("mmce1:/", 0x1, NULL, 0, NULL, 0) != -1) {
        sprintf(mmcePrefix, "mmce1:/%s", gMMCEPrefix);
        ret = 3;
    }
    return ret;
}

void mmceSetPrefix(void)
{
    if (gMMCESlot == 0)
        sprintf(mmcePrefix, "mmce0:/%s", gMMCEPrefix);
    else if (gMMCESlot == 1)
        sprintf(mmcePrefix, "mmce1:/%s", gMMCEPrefix);
    else if (gMMCESlot == 2) {
        // Auto: reuse the previously-detected slot instead of probing BOTH slots every refresh.
        // On a cache hit, a presence devctl on the resolved slot confirms the card is still there
        // (mmcePrefix from the prior detect is still correct); only MMCE_PRESENCE_PROBE_MAX
        // CONSECUTIVE misses mean the card was pulled (#154 debounce, below), so invalidate and fall
        // through to a full re-detect. Net: 1 SIO2 probe/cycle instead of 2 on the happy path, and
        // card removal is still noticed (mmceDetectSlot alone leaves a stale prefix on a lost card).
        if (mmceResolvedDevice > 0) {
            const char *root = (mmceResolvedDevice == 2) ? "mmce0:/" : "mmce1:/";
            // Debounced presence check (#154): invalidate only after MMCE_PRESENCE_PROBE_MAX
            // consecutive failed devctls -- see the define block above. One success = present.
            int probe;
            for (probe = 0; probe < MMCE_PRESENCE_PROBE_MAX; probe++) {
                if (fileXioDevctl(root, 0x1, NULL, 0, NULL, 0) != -1)
                    break;
                if (probe + 1 < MMCE_PRESENCE_PROBE_MAX)
                    DelayThread(MMCE_PRESENCE_RETRY_US); // let a contended bus quiet, then re-probe
            }
            if (probe >= MMCE_PRESENCE_PROBE_MAX) {
                mmceResolvedDevice = -1;
                mmcePrefix[0] = '\0';
            } else {
                // Still present. Rebuild mmcePrefix from the CURRENT gMMCEPrefix (a Device-Settings
                // prefix change must apply immediately -- initSupport does not re-init an already-
                // enabled MMCE tab) using the cached slot; we skip only the second SIO2 slot probe.
                sprintf(mmcePrefix, "mmce%d:/%s", (mmceResolvedDevice == 2) ? 0 : 1, gMMCEPrefix);
            }
        }
        if (mmceResolvedDevice <= 0)
            mmceResolvedDevice = mmceDetectSlot();
    }

    mmceRefreshArtRoots();
}

void mmceLoadModules(void)
{
    // MAXIMAL-QUIET TEST BUILD (#340, DO NOT MERGE): never load mmceman into the menu IOP,
    // config ignored. Its hook replaces sio2man's sio2_transfer export and relinks freepad's
    // import table; with no MMCE hardware each probe burns a 200 ms timeout while freepad's
    // vblank pad reads block. Official OPL, sOPL and uOPL ship no mmceman at all.
    LOG("MMCESUPPORT LoadModules SUPPRESSED (#340 maximal-quiet build)\n");
    mmceModLoaded = 1;
}

// Δ4 (NHDDL parity): arm the GameID transport OUTSIDE the launch path. NHDDL loads mmceman once at
// boot; RiptOPL used to self-arm inside mmceSendGameID -- an IRX load/start at the launch's most
// fragile moment (issue #51's fix, right intent, wrong timing). Called from initAllSupport (boot +
// every settings apply) via the IO worker, so a wedged load is a harmless LOG at menu time instead
// of a dead launch. Idempotent (mmceLoadModules latches); no-op when the GameID feature is off.
void mmceArmGameIDTransport(void)
{
    if (gMMCEEnableGameID) {
        guiSetBootStatusSticky(_l(_STR_BOOT_ARMING_MMCE)); // boot-step localizer (IO thread) -- see gui.c
        mmceLoadModules();
        // Post-load marker (#254): the boot arm runs right after GUI_INIT_DONE; a serial log that
        // shows the arm begin without this completion line localizes a wedge to the mmceman load.
        LOG("MMCESUPPORT GameID transport armed\n");
    }
}

void mmceInit(item_list_t *itemList)
{
    LOG("MMCESUPPORT Init\n");
    mmcePrefix[0] = '\0';
    mmceArtPrimary[0] = '\0';
    mmceULSizePrev = -2;
    mmceModifiedCDPrev = 0;
    mmceModifiedDVDPrev = 0;
    mmceGameCount = 0;
    mmceGames = NULL;
    mmceVcdGameCount = 0;
    mmceVcdGames = NULL;
    mmceResolvedDevice = -1; // re-detect the Auto slot on a fresh init (tab re-enable / settings apply)
    mmceFoldersCreatedFor[0] = '\0';
    mmceFolderRetries = 0;

    mmceGameList.delay = gArtDelay;
    mmceGameList.updateDelay = MMCE_MODE_UPDATE_DELAY;

    mmceLoadModules();
    mmceSetPrefix();

    mmceGameList.enabled = 1;
}

item_list_t *mmceGetObject(int initOnly)
{
    if (initOnly && !mmceGameList.enabled)
        return NULL;
    return &mmceGameList;
}

static int mmceNeedsUpdate(item_list_t *itemList)
{
    static unsigned char ThemesLoaded = 0;
    static unsigned char LanguagesLoaded = 0;

    char path[256];
    int result = 0;
    struct stat st;

    // Hacky: check if slot was changed, update prefix if needed
    mmceSetPrefix();

    if (mmcePrefix[0] == '\0') {
        mmceGameList.updateDelay = MMCE_MODE_UPDATE_DELAY;
        mmceFoldersCreatedFor[0] = '\0'; // card gone: recreate folders on the next (possibly different) card
        mmceFolderRetries = 0;
        // Card gone with an EMPTY failed VCD list never reaches mmceUpdateGameList's resets (this
        // early return fires first), so a reinserted card would inherit an exhausted retry budget
        // and a dead VCD page (CodeRabbit review of #248, vetted). Fresh card = fresh budget.
        mmceVcdScanFailed = 0;
        mmceVcdScanRetries = 0;
        // Card gone: re-arm THM/LNG registration so a swapped-in card's assets get discovered
        // (Gemini review of #153). The old card's already-registered entries stay in the pickers --
        // eviction infrastructure doesn't exist -- but picking a stale one fails gracefully
        // (thmLoad abandons and keeps the current theme), and thmAddElements caps at THM_MAX_FILES.
        ThemesLoaded = 0;
        LanguagesLoaded = 0;
        return (mmceGameCount > 0 || mmceVcdGameCount > 0);
    }

    mmceGameList.updateDelay = MENU_UPD_DELAY_NOUPDATE;

    // Register the card's THM/LNG dirs BEFORE the VCD-view early returns (#152, AndrewBento). These
    // used to sit below them, giving one realistic shot at boot: once the first list scan latches
    // NOUPDATE above, the only future passes are L3-toggle / VCD-view ones, which returned before
    // reaching the registration -- so if the boot-time attempt lost a race against the contended
    // MMCE SIO2 bus (config + list + art traffic), themes on the card stayed invisible for the whole
    // session while USB's fast first try succeeded. Here every pass retries until each succeeds; the
    // cost is one dir-open per pass until then (identical to the old ISO-view retry behavior).
    if (!ThemesLoaded) {
        sprintf(path, "%sTHM", mmcePrefix);
        if (thmAddElements(path, "/", 1) > 0)
            ThemesLoaded = 1;
    }
    if (!LanguagesLoaded) {
        sprintf(path, "%sLNG", mmcePrefix);
        if (lngAddLanguages(path, "/", mmceGameList.mode) > 0)
            LanguagesLoaded = 1;
    }

    // VCD view: force a rescan once on toggle, then skip the disc heuristics while showing VCDs.
    if (vcdConsumeDirty(itemList->mode)) {
        mmceVcdScanRetries = 0; // fresh user toggle re-arms the failed-scan retry budget
        return 1;
    }
    // Folder browsing: descend/ascend forces one rescan (consumed before the NOUPDATE latch below).
    if (folderConsumeDirty(itemList->mode))
        return 1;
    if (vcdViewActive(itemList->mode)) {
        // A contended scan left the VCD page empty (S6): keep the ~2s refresh alive until a scan
        // succeeds or the bounded budget runs out (no endless bus churn -- #246 doctrine). Also
        // revives the manual-refresh button in the failed state.
        if (mmceVcdScanFailed && mmceVcdScanRetries < MMCE_VCD_SCAN_RETRY_MAX) {
            mmceVcdScanRetries++;
            mmceGameList.updateDelay = MMCE_MODE_UPDATE_DELAY;
            return 1;
        }
        return 0;
    }

    if (mmceULSizePrev == -2) {
        // First scan not yet successful. If it wedges on a contended SIO2 bus, sbReadList leaves
        // mmceULSizePrev at its -2 sentinel and the tab stays empty; the NOUPDATE latch above would
        // then strand it with no auto-retry (the reported "all MMCE lists vanished"). Keep the ~2s
        // background retry alive until a scan populates the list -- a genuinely-empty readable card
        // sets mmceULSizePrev via *fsize, so this still quiesces once the bus is readable.
        mmceGameList.updateDelay = MMCE_MODE_UPDATE_DELAY;
        result = 1;
    }

    sprintf(path, "%sCD", mmcePrefix);
    if (stat(path, &st) != 0)
        st.st_mtime = 0;

    if (mmceModifiedCDPrev != st.st_mtime) {
        mmceModifiedCDPrev = st.st_mtime;
        result = 1;
    }

    sprintf(path, "%sDVD", mmcePrefix);
    if (stat(path, &st) != 0)
        st.st_mtime = 0;

    if (mmceModifiedDVDPrev != st.st_mtime) {
        mmceModifiedDVDPrev = st.st_mtime;
        result = 1;
    }

    if (!sbIsSameSize(mmcePrefix, mmceULSizePrev))
        result = 1;

    // Themes/Languages registration moved ABOVE the VCD-view early returns (#152) -- see the block
    // after the NOUPDATE latch near the top of this function.

    // Create the library folders once per card/slot, not on every refresh (each is an SIO2 mkdir).
    // Only latch the "done" memo once CFG actually EXISTS on the card: sbCreateFolders' mkdir burst
    // ignores its return, so a single mkdir dropped on a busy card would otherwise mark the tree
    // "created" forever while CFG never got made -- and every per-game save then fails, because the
    // config write targets <prefix>CFG/<id>.cfg (#245, AndrewBento). Leaving the memo unset here lets
    // the ~2s refresh keep retrying until CFG is confirmed present, so a missing folder always heals
    // itself instead of stranding saves.
    if (strcmp(mmceFoldersCreatedFor, mmcePrefix) != 0) {
        sbCreateFolders(mmcePrefix, 1);

        char cfgPath[sizeof(mmcePrefix) + 4];
        snprintf(cfgPath, sizeof(cfgPath), "%sCFG", mmcePrefix);
        DIR *cfgDir = opendir(cfgPath);
        if (cfgDir != NULL) {
            closedir(cfgDir);
            snprintf(mmceFoldersCreatedFor, sizeof(mmceFoldersCreatedFor), "%s", mmcePrefix);
            mmceFolderRetries = 0;
        } else if (++mmceFolderRetries >= MMCE_FOLDER_RETRY_MAX) {
            // CFG is OBSTRUCTED, not merely missing: mkdir + opendir have now both failed repeatedly
            // (a non-directory entry named CFG, or on-card FS damage to that dir entry -- e.g. a
            // PC-side deletion that left a broken record). Retrying forever would churn the shared
            // SIO2 bus with a 10-mkdir burst PLUS a forced full list rescan every ~2s for the whole
            // session (adversarial review of #246), so latch and stop. The user is NOT left stranded:
            // the write-time parent-create in checkFile still attempts CFG on every save, and the
            // save's failure toast names the exact path + errno -- the card needs a PC-side look.
            LOG("MMCE: %s still absent after %d create attempts -- obstructed; giving up for this session\n",
                cfgPath, MMCE_FOLDER_RETRY_MAX);
            snprintf(mmceFoldersCreatedFor, sizeof(mmceFoldersCreatedFor), "%s", mmcePrefix);
            mmceFolderRetries = 0;
        } else {
            // Keep the ~2s background refresh alive so the create genuinely retries -- the NOUPDATE
            // latch at the top of this function (line 390) would otherwise settle the callback to 0
            // and the "retry next refresh" never happens. Mirrors the first-scan retry pattern above.
            LOG("MMCE: %s not present after sbCreateFolders -- re-arming retry (%d/%d)\n",
                cfgPath, mmceFolderRetries, MMCE_FOLDER_RETRY_MAX);
            mmceGameList.updateDelay = MMCE_MODE_UPDATE_DELAY;
            result = 1;
        }
    }

    return result;
}

static int mmceUpdateGameList(item_list_t *itemList)
{
    if (mmcePrefix[0] == '\0') {
        // Card absent / slot unresolved (Auto mode after removal). Actually CLEAR the list rather
        // than returning the stale count: mmceNeedsUpdate keeps reporting "update needed" while
        // mmceGameCount > 0, so returning the stale count here leaves the removed card's games on
        // screen and spins a menu-rebuild + apps-rescan + favourites-reload loop every ~2s. Freeing
        // lets updateMenuFromGameList empty the menu and drives needsUpdate's (count > 0) test to 0.
        if (mmceGames != NULL) {
            free(mmceGames);
            mmceGames = NULL;
        }
        if (mmceVcdGames != NULL) {
            free(mmceVcdGames);
            mmceVcdGames = NULL;
        }
        mmceGameCount = 0;
        mmceVcdGameCount = 0;
        mmceULSizePrev = -2; // force a fresh scan when a card returns
        mmceVcdScanFailed = 0;
        mmceVcdScanRetries = 0;
        return 0;
    }

    // Each view scans into its OWN array (#120): a failed rescan preserves only that view's last-good and
    // can never resurrect the other view's list (see the mmceVcdGames comment at the declarations).
    if (vcdViewActive(itemList->mode)) {
        int r = vcdFillGameList(mmcePrefix, &mmceVcdGames);
        if (r >= 0) { // r < 0: transient scan failure (contended bus) -> keep the last-good VCD list
            mmceVcdGameCount = r;
            mmceVcdScanFailed = 0;
            mmceVcdScanRetries = 0;
        } else {
            mmceVcdScanFailed = 1; // arm mmceNeedsUpdate's bounded retry (S6)
        }
        return mmceVcdGameCount;
    }
    sbReadList(&mmceGames, mmcePrefix, folderGetSub(itemList->mode), &mmceULSizePrev, &mmceGameCount);
    return mmceGameCount;
}

static int mmceGetGameCount(item_list_t *itemList)
{
    return vcdViewActive(itemList->mode) ? mmceVcdGameCount : mmceGameCount;
}

// Toggle-window guard (Codex/Fable audit). The L3 view toggle flips vcdViewActive() SYNCHRONOUSLY
// (vcdToggleView) but rebuilds the submenu on the DEFERRED IO thread. On a contended MMCE bus that rebuild
// lags, so for a window the OLD submenu's ids are still live while vcdViewActive() already reports the NEW
// view -- and an id from the old view can index the freshly-switched other-view array, which may be NULL
// (that view never scanned this session), shorter, or mid-scan. The direct &mmceVcdGames[id]/&mmceGames[id]
// indexing then NULL/OOB-derefs and crashes. (The shared store the #120 split replaced could not OOB: one
// array + one count stayed self-consistent.) Resolve every id through here: an out-of-range id returns a
// STATIC EMPTY entry, so a stale-id READ is safe empty data instead of a crash; LAUNCH treats &mmceEmptyGame
// as "nothing to launch" (mmceLaunchGame, below).
static base_game_info_t mmceEmptyGame = {0};
static base_game_info_t *mmceActiveGame(item_list_t *itemList, int id)
{
    int vcd = vcdViewActive(itemList->mode);
    base_game_info_t *arr = vcd ? mmceVcdGames : mmceGames;
    int count = vcd ? mmceVcdGameCount : mmceGameCount;
    if (arr == NULL || id < 0 || id >= count)
        return &mmceEmptyGame;
    return &arr[id];
}

static void *mmceGetGame(item_list_t *itemList, int id)
{
    return (void *)mmceActiveGame(itemList, id);
}

static char *mmceGetGameName(item_list_t *itemList, int id)
{
    return mmceActiveGame(itemList, id)->name;
}

static int mmceGetGameNameLength(item_list_t *itemList, int id)
{
    base_game_info_t *g = mmceActiveGame(itemList, id);
    return ((g->format != GAME_FORMAT_USBLD) ? ISO_GAME_NAME_MAX + 1 : UL_GAME_NAME_MAX + 1);
}

static char *mmceGetGameStartup(item_list_t *itemList, int id)
{
    // VCD view keys per-game data (CFG/art) off the VCD filename, not a disc ID (see sbPopulateConfig).
    base_game_info_t *g = mmceActiveGame(itemList, id);
    if (vcdViewActive(itemList->mode))
        return g->name;
    return g->startup;
}

static void mmceDeleteGame(item_list_t *itemList, int id)
{
    if (vcdViewActive(itemList->mode))
        return; // #120: a VCD is not an ISO game -- no delete in VCD view
    if (mmceActiveGame(itemList, id) == &mmceEmptyGame)
        return;                                   // stale id in the VCD->ISO toggle window (vcdViewActive already flipped, old VCD submenu id
                                                  // still live): sbDelete does NOT bounds-check, so this avoids an OOB/NULL deref + wrong unlink
    sbSetBrowseSub(folderGetSub(itemList->mode)); // delete inside the current subfolder, not the root
    sbDelete(&mmceGames, mmcePrefix, "/", mmceGameCount, id);
    mmceULSizePrev = -2;
}

static void mmceRenameGame(item_list_t *itemList, int id, char *newName)
{
    if (vcdViewActive(itemList->mode))
        return; // #120: no rename in VCD view
    if (mmceActiveGame(itemList, id) == &mmceEmptyGame)
        return;                                   // stale id in the VCD->ISO toggle window (see mmceDeleteGame) -> avoid sbRename OOB
    sbSetBrowseSub(folderGetSub(itemList->mode)); // rename inside the current subfolder, not the root
    sbRename(&mmceGames, mmcePrefix, "/", mmceGameCount, id, newName);
    mmceULSizePrev = -2;
}

// Launch a PS1/.VCD entry BY NAME via POPSTARTER (view-independent entry point: the in-view menu
// launch below and the Favourites tab both use it). mmcePrefix is static; UNMOUNT_EXCEPTION keeps the
// MMCE device mounted across the IOP reset.
static void mmceLaunchVcd(item_list_t *itemList, const char *vcdName, config_set_t *configSet)
{
    char vcdElf[256], vcdSelector[320];

    if (vcdName == NULL || vcdName[0] == '\0' || !strcasecmp(vcdName, "POPSTARTER")) // reserved-name belt: the scanner no longer lists it (#154); strcasecmp -- FAT is case-insensitive
        return;
    if (!vcdResolvePopstarter(mmcePrefix, vcdElf, sizeof(vcdElf))) {
        guiMsgBox(_l(_STR_POPSTARTER_NOT_FOUND), 0, NULL);
        return;
    }
    vcdBuildSelector(mmcePrefix, VCD_PREFIX_MASS, vcdName, vcdSelector, sizeof(vcdSelector));
    // Source MC-side externals from this MMCE card's direct POPS/ folder; never overwrite card files.
    (void)vcdInstallPopstarterMc(mmcePrefix);
    // Best-effort card prep: try to equip the .mmce BDMAssault variant so the driver pair POPSTARTER
    // reloads from the MC fits this drive. NEVER a launch gate -- the handoff below always proceeds
    // (POPSTARTER owns everything past the exec); a failed equip just toasts its diagnostic in passing.
    vcdEnsureBdmaForLaunch(VCD_BDMA_SRC_MMCE, VCD_BDMA_MMCE);
    char vcdFullPath[256];
    snprintf(vcdFullPath, sizeof(vcdFullPath), "%sPOPS/%s.VCD", mmcePrefix, vcdName);
    vcdPrepareRetroGemBarcode(vcdFullPath);
    deinit(UNMOUNT_EXCEPTION, itemList->mode); // keep the MMCE device mounted across the IOP reset
    sysLaunchPopstarter(vcdElf, vcdSelector);
}

void mmceLaunchGame(item_list_t *itemList, int id, config_set_t *configSet)
{
    int i, index, compatmask = 0;
    int EnablePS2Logo = 0;
    int result;

    char partname[256], filename[32];
    base_game_info_t *game;
    struct cdvdman_settings_mmce *settings;
    u32 layer1_start, layer1_offset;
    unsigned short int layer1_part;

    // No Autolaunch yet
    if (gAutoLaunchBDMGame == NULL) {
        game = mmceActiveGame(itemList, id);
        if (game == &mmceEmptyGame)
            return; // stale id during the L3 toggle window (see mmceActiveGame) -> nothing to launch
    } else
        game = gAutoLaunchBDMGame;

    // Folder browsing: a folder row is never launched (the dispatch descends first); guard defensively
    // and pin the path composers to the current subfolder so a nested game resolves.
    if (game != NULL && game->format == GAME_FORMAT_FOLDER)
        return;
    sbSetBrowseSub(folderGetSub(itemList->mode));

    // Quiesce every in-flight MMCE art read BEFORE either launch path touches the card. The VCD
    // handoff below resolves POPSTARTER and may equip BDMA modules -- real reads/writes on the SAME
    // shared mmceman SIO2 channel -- and its early return used to run BEFORE this guard, so a VCD
    // launch could collide with the art worker mid-read (FifthFox: "bombed one launch of a VCD on
    // the MMCE"; the disc path below has always quiesced first). The by-name handoff (ccd1d7a4)
    // landed AFTER the quiesce existed and slotted in above it -- ordering bug since, probabilistic
    // by nature, which is why it "worked until it didn't". Idea source: PR #236's quiesce-reorder,
    // vetted against this tree and re-landed with the full #120 rationale kept.
    if (!cacheAbortMmceImageLoadsTimed(MMCE_ART_ABORT_WAIT_TICKS)) {
        // The card is still busy. Leave the menu and worker intact so the user
        // can retry after the current read returns.
        guiWarning(_l(_STR_ERR_FILE_INVALID), 8);
        return;
    }

    // VCD view: hand off to POPSTARTER (by name) instead of the disc path below. Menu-launch only.
    if (gAutoLaunchBDMGame == NULL && game != NULL && vcdViewActive(itemList->mode)) {
        mmceLaunchVcd(itemList, game->name, configSet);
        return;
    }

    // (The MMCE art quiesce runs ABOVE the VCD handoff now -- both launch paths are covered by the
    // single guard before any card IO.)
    void *irx = &mmce_cdvdman_irx;
    int irx_size = size_mmce_cdvdman_irx;
    compatmask = sbPrepare(game, configSet, irx_size, irx, &index);
    settings = (struct cdvdman_settings_mmce *)((u8 *)irx + index);
    if (settings == NULL)
        return;

    // Persist last-played BEFORE any card switch below: on an FMCB-on-MMCE setup this write goes to
    // the card's mcN: surface, and after a GameID switch it would land inside the per-game virtual
    // card instead of the boot card (adversarial review of the native-send re-land).
    if (gRememberLastPlayed) {
        configSetStr(configGetByType(CONFIG_LAST), "last_played", game->startup);
        saveConfig(CONFIG_LAST, 0);
    }

    // Native-core GameID (AndrewBento, Gen2: with PS2 Logo off nothing ever names the game to the
    // card, so its per-game save folder never engages). Send the 0x8 push HERE -- before ANY card-side
    // fd exists (the VMC fds and the ISO fd below all traverse the switched card) -- matching the
    // Neutrino leg's proven close-fds -> send -> settle ordering and NHDDL's zero-mmce-traffic-after-
    // switch rule. The historical #50 freeze came from the OPPOSITE shape: a send placed LAST (after
    // every fd was captured against the pre-switch surface) in a build whose card busy-wait had been
    // gutted to zero (see the corrected note below, near the launch tail). Per-slot Δ3 parity: a slot
    // whose per-game VMC will be covered by mcemu keeps its card (the folder is moot for it; saves go
    // to the VMC file), enforced via the mask. The Neutrino leg keeps its own send; a Neutrino config
    // that falls back to native (bad ZSO / neutrino.elf missing) keeps today's no-send behavior.
    {
        int coreLoaderEarly = gDefaultCoreLoader;
        configGetInt(configSet, CONFIG_ITEM_CORE_LOADER, &coreLoaderEarly);
        if (!coreLoaderEarly) {
            char vmcNameEarly[32];
            int vmcMask = 0, vs;
            for (vs = 0; vs < 2; vs++) {
                configGetVMC(configSet, vmcNameEarly, sizeof(vmcNameEarly), vs);
                if (vmcNameEarly[0])
                    vmcMask |= (1 << vs);
            }
            // protectMcPath=NULL: the native path loads no ELF from mcN: mid-launch (ee_core is
            // embedded; sysLaunchLoaderElf reads rom0: only).
            if (mmceSendGameID(game->startup, NULL, vmcMask))
                mmceSettleAfterSwitch();
        }
    }

    char vmc_name[32];
    char vmc_path[256];
    int vmc_size_mb;
    int vmc_id, size_mcemu_irx = 0;
    int vmc_fd;
    int vmc_fds[2] = {-1, -1}; // track VMC fds to close on the Neutrino handoff path (B3)
    mmce_vmc_infos_t mmce_vmc_infos;
    vmc_superblock_t vmc_superblock;

    for (vmc_id = 0; vmc_id < 2; vmc_id++) {
        memset(&mmce_vmc_infos, 0, sizeof(mmce_vmc_infos));
        configGetVMC(configSet, vmc_name, sizeof(vmc_name), vmc_id);
        if (vmc_name[0]) {
            vmc_size_mb = sysCheckVMC(mmcePrefix, "/", vmc_name, 0, &vmc_superblock);
            if (vmc_size_mb > 0) {
                mmce_vmc_infos.flags = vmc_superblock.mc_flag & 0xFF;
                mmce_vmc_infos.flags |= 0x100;
                mmce_vmc_infos.specs.page_size = vmc_superblock.page_size;
                mmce_vmc_infos.specs.block_size = vmc_superblock.pages_per_block;
                mmce_vmc_infos.specs.card_size = vmc_superblock.pages_per_cluster * vmc_superblock.clusters_per_card;

                sprintf(vmc_path, "%sVMC/%s.bin", mmcePrefix, vmc_name);

                vmc_fd = fileXioOpen(vmc_path, 0x3, 0666);
                if (vmc_fd >= 0) {
                    vmc_fds[vmc_id] = vmc_fd;
                    mmce_vmc_infos.fd = fileXioIoctl2(vmc_fd, 0x80, NULL, 0, NULL, 0);
                    mmce_vmc_infos.active = 1;
                }
            }
        }

        for (i = 0; i < size_mmce_mcemu_irx; i++) {
            if (((u32 *)&mmce_mcemu_irx)[i] == (0xC0DEFAC0 + vmc_id)) {
                if (mmce_vmc_infos.active)
                    size_mcemu_irx = size_mmce_mcemu_irx;
                memcpy(&((u32 *)&mmce_mcemu_irx)[i], &mmce_vmc_infos, sizeof(mmce_vmc_infos_t));
                break;
            }
        }
    }

    // Initialize layer 1 information.
    sbCreatePath(game, partname, mmcePrefix, "/", 0);

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
            sbCreatePath(game, partname, mmcePrefix, "/", layer1_part);
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

    if ((result = sbLoadCheats(mmcePrefix, game->startup)) < 0) {
        // #265: let the user back out instead of sitting through the whole load. The helper does
        // the sbUnprepare itself -- see include/supportbase.h; skipping it breaks the NEXT launch.
        if (!sbCheatsMissingContinue(&settings->common, result))
            return;
    }
    sbLoadImage(mmcePrefix, game->startup);

    // (last-played persistence hoisted above the GameID switch -- see the native-core send block)

    if (configGetStrCopy(configSet, CONFIG_ITEM_ALTSTARTUP, filename, sizeof(filename)) == 0)
        strcpy(filename, game->startup);


    // MMCEDRV settings
    if (gMMCESlot == 0)
        settings->port = 2;
    else if (gMMCESlot == 1)
        settings->port = 3;
    else if (gMMCESlot == 2) {
        int detectedPort = mmceDetectSlot();
        if (detectedPort < 0) {
            // Neither slot responded; abort rather than forward port -1 to the IOP.
            LOG("MMCE slot lost, aborting launch\n");
            // Make the bail VISIBLE (HW batch S5: "does gameID and then nothing" with no message).
            // deinit has not run yet, mmceLaunchGame is on the GUI thread, and guiWarning self-guards
            // for autolaunch -- same pattern as the quiesce bail above.
            guiWarning(_l(_STR_ERR_FILE_INVALID), 8);
            // Close the VMC fds opened above so a failed launch does not leak them
            // back to the menu across repeated attempts (Codex audit, Medium 2).
            if (vmc_fds[0] >= 0)
                fileXioClose(vmc_fds[0]);
            if (vmc_fds[1] >= 0)
                fileXioClose(vmc_fds[1]);
            return;
        }
        settings->port = detectedPort;
        // Re-apply trailing-slash normalization: mmceDetectSlot() rewrites
        // mmcePrefix via sprintf with no slash append, de-normalizing the
        // value mmceRefreshArtRoots() previously set. sbBuildVmcNeutrinoArgs
        // (called below) builds "-mcN=<prefix>VMC/<name>.bin" and requires
        // mmcePrefix to end in '/' -- without this call a non-empty gMMCEPrefix
        // (e.g. "GAMES") produces the broken path "mmce0:/GAMESVMC/<name>.bin".
        mmceRefreshArtRoots();
    }

    // Per-game Neutrino core: gate BEFORE opening iso_file so no fd is leaked on
    // the Neutrino path (game is still valid here for the format check).
    int coreLoader = gDefaultCoreLoader; // no per-game $CoreLoader key -> follow the global default core
    configGetInt(configSet, CONFIG_ITEM_CORE_LOADER, &coreLoader);
    const char *neutrinoPath = NULL;
    char neutrinoExtraArgs[256] = "";              // per-game Neutrino flags; copied before deinit teardown
    int neutrinoVideo = gNeutrinoVideoDefault;     // per-game -gsm video mode; absent key = follow the global; copied before deinit
    int neutrinoGsmComp = gNeutrinoGsmCompDefault; // per-game -gsm ":c" field-flip half; absent key = follow the global; copied before deinit
    neutrino_vmc_args_t neutrinoVmc = {0};         // per-game VMC -mc args; resolved before deinit, lives on this stack frame across the launch (#47)
    if (coreLoader) {
        configGetStrCopy(configSet, CONFIG_ITEM_NEUTRINO_ARGS, neutrinoExtraArgs, sizeof(neutrinoExtraArgs));
        configGetInt(configSet, CONFIG_ITEM_NEUTRINO_VIDEO, &neutrinoVideo);
        configGetInt(configSet, CONFIG_ITEM_NEUTRINO_GSMCOMP, &neutrinoGsmComp);
        neutrinoPath = sbResolveNeutrinoPath(mmcePrefix); // #300: AUTO also probes this MMCE card for a co-located neutrino.elf
        if (game->format == GAME_FORMAT_USBLD || !strcasecmp(game->extension, ".zso")) {
            // isValidIsoName() admits .zso case-insensitively and game->extension is stored
            // verbatim, so an upper/mixed-case ".ZSO" must reject here too (Neutrino can't run it).
            guiWarning(_l(_STR_NEUTRINO_BAD_FORMAT), 6);
            coreLoader = 0;
        } else if (neutrinoPath == NULL) {
            guiWarning(_l(_STR_NEUTRINO_NOT_FOUND), 6);
            coreLoader = 0;
        }

        // VMC -> neutrino (#47): resolve any per-game VMC into discrete -mc0/-mc1 argv entries
        // (mmcePrefix ends in '/'); not the whitespace-tokenized extra-args buffer (spaced names).
        if (coreLoader)
            sbBuildVmcNeutrinoArgs(configSet, mmcePrefix, &neutrinoVmc);
    }
    if (coreLoader) {
        char mmcePartname[256];
        snprintf(mmcePartname, sizeof(mmcePartname), "%s", partname); // defensive copy across the deinit teardown (partname is a stack buffer, not freed by deinit)
        // game (== &mmceGames[id]) is freed by deinitEx() below (moduleCleanup -> mmceCleanUp frees
        // mmceGames), but sysLaunchNeutrino still reads game->startup afterwards to build its -elf
        // argument -- a use-after-free read. Copy startup onto this stack frame like mmcePartname.
        char mmceStartup[GAME_STARTUP_MAX + 1];
        snprintf(mmceStartup, sizeof(mmceStartup), "%s", game->startup);
        // Neutrino bypasses OPL's mcemu, so the VMC fds opened above go unused on this path --
        // close them instead of leaking until the IOP reset (B3). Closed BEFORE the GameID push
        // below (Beta-2947 hardware report): the 0x8 switch physically re-mounts the card, and
        // closing a handle opened against the PRE-switch filesystem afterwards wedged mmceman --
        // the GUI froze the instant the GameID appeared on the card. NHDDL's ordering has ZERO
        // mmce filesystem traffic after its mmceMountVMC; match it as closely as we can.
        if (vmc_fds[0] >= 0)
            fileXioClose(vmc_fds[0]);
        if (vmc_fds[1] >= 0)
            fileXioClose(vmc_fds[1]);
        // GameID for the NEUTRINO core (issue #68): the native OPL-core launch deliberately does
        // NOT push a launcher GameID (see the issue-#50 note below -- in OPL core the in-game
        // card is OPL's mcemu, and a mid-launch re-switch froze early-MC-probing games). That
        // reasoning does NOT extend to Neutrino: it has no mcemu -- the game talks to the card's
        // REAL emulated-MC surface -- and nothing else ever tells the card which game is
        // starting, so its per-game folder never engaged (USB-hosted games via Neutrino DID work:
        // the cross-device paths all send it). NHDDL does exactly this before launching neutrino
        // (mmceMountVMC). mmceSendGameID waits out the card's busy bit (bounded ~3 s); the
        // MC-hosted-neutrino protect guard is inside the helper (skips + warns when neutrinoPath
        // sits on this slot's mcN:).
        int gameIdSwitched = mmceSendGameID(game->startup, neutrinoPath,
                                            (neutrinoVmc.arg[0][0] ? 1 : 0) | (neutrinoVmc.arg[1][0] ? 2 : 0)); // Δ3: -mc-covered slots keep their card
        // Fs-settle after a card switch (shared helper; see mmceSettleAfterSwitch). The game on this
        // leg is ALWAYS mmce-hosted, so after any actual switch both our own reads below (neutrino.elf
        // load, ISO open for the keep-IOP handoff) AND Neutrino's own post-reset mmceman read hit the
        // just-switched card -- wait for the switched slot's fs to answer first (issue #56/#68).
        if (gameIdSwitched)
            mmceSettleAfterSwitch();
        // Neutrino keep-IOP handoff (sysLoadELFKeepIOP): Neutrino opens the mmce-hosted game through
        // OUR mmceman mount and its config/modules from the neutrino.elf device (-cwd) before its own
        // IOP reset -- keep BOTH mounted. An MC-hosted neutrino needs no exception (-1 second slot).
        if (sysNeutrinoPreflight("mmce", neutrinoPath) < 0) // D6 pre-teardown validation
            return;
        int neutrinoDevMode = oplPath2Mode(neutrinoPath);
        deinitEx(UNMOUNT_EXCEPTION, itemList->mode, neutrinoDevMode);
        sysLaunchNeutrino("mmce", mmcePartname, mmceStartup, compatmask, EnablePS2Logo, neutrinoPath, neutrinoExtraArgs, neutrinoVideo, neutrinoGsmComp, 0 /* #11: mmce is fileid, no fs layer */, &neutrinoVmc);
        return;
    }

    // Poll-first: try once; on failure retry briefly. Covers a card re-mount that completes just past
    // the settle window (the settle's Dopen probes can burn their whole budget on a recovering channel).
    // A healthy card pays ~0 ms (first try succeeds); a dead one pays a bounded ~2 s before a VISIBLE
    // bail -- the old path returned to the menu with only a LOG, which read on HW as "does gameID and
    // then nothing" (batch S5).
    int iso_file = fileXioOpen(partname, 0x1, 0666);
    for (int isoRetry = 0; iso_file < 0 && isoRetry < 10; isoRetry++) {
        DelayThread(200 * 1000);
        iso_file = fileXioOpen(partname, 0x1, 0666);
    }
    if (iso_file < 0) {
        LOG("Failed to open iso %s (ret %d), aborting\n", partname, iso_file);
        // Same VMC-fd leak guard as the slot-lost path above (Codex audit, Medium 2).
        if (vmc_fds[0] >= 0)
            fileXioClose(vmc_fds[0]);
        if (vmc_fds[1] >= 0)
            fileXioClose(vmc_fds[1]);
        guiWarning(_l(_STR_ERR_FILE_INVALID), 8); // batch S5: never bail silently
        return;
    }

    settings->ack_wait_cycles = gMMCEAckWaitCycles;
    settings->use_alarms = gMMCEUseAlarms;

    // TEMP: The fd given by sd2psx is not the same one we see here on the EE
    // and ps2sdk_get_iop_fd does not seem to return the right value either
    settings->iso_fd = fileXioIoctl2(iso_file, 0x80, NULL, 0, NULL, 0);

    LOG("name: %s\n", game->name);
    LOG("start: %s\n", game->startup);

    // Issue #50, CORRECTED HISTORY (2026-07-13 archaeology): the native GameID send was NOT the bug.
    // Native MMCE launches sent SET_GAMEID from the first MMCE support (incl. the tester's known-good
    // beta 2257, which waited up to 15 s for the card) -- the freeze regression (2257 -> 2813,
    // ramonesfm, SCPH-30001 + Gen2) came from the busy-wait being gutted to nopdelay() (68bf1a73) AND
    // the send sitting LAST, after every card-side fd was captured against the pre-switch surface.
    // The wait has since been restored (aa780f24/847cc620, poll-first ~3 s) and the send re-landed
    // ABOVE (before any fd exists, with the fs-settle) -- the mcemu rationale only ever held when a
    // per-game VMC was configured (no VMC -> mcemu never loads and the game talks to the REAL card,
    // where the per-game folder genuinely matters: AndrewBento's logo-off report). The cross-device
    // paths (bdm/hdd/eth) send it too, with the same wait.

    if (gAutoLaunchBDMGame == NULL) {
        deinit(NO_EXCEPTION, MMCE_MODE); // CAREFUL: deinit will call mmceCleanUp, so mmceGames/game will be freed
    }

    /* No autolaunch yet
    else {
        miniDeinit(configSet);

        free(gAutoLaunchBDMGame);
        gAutoLaunchBDMGame = NULL;
    }*/

    settings->common.zso_cache = 0;

    sysLaunchLoaderElf(filename, "MMCE_MODE", irx_size, irx, size_mcemu_irx, mmce_mcemu_irx, EnablePS2Logo, compatmask);
}

static config_set_t *mmceGetConfig(item_list_t *itemList, int id)
{
    // Config (CFG + #Format/#System/#DiscType badges) comes from the ACTIVE view's array; mmceActiveGame
    // picks it (VCD keys off the basename) and returns a safe empty entry for a stale id during the toggle
    // window -- so this can never index the wrong/NULL array out of range.
    return sbPopulateConfig(mmceActiveGame(itemList, id), mmcePrefix, "/");
}

static int mmceGetImage(item_list_t *itemList, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)
{
    // OPL's own ART/<name>_COV.png is the PRIMARY lookup (same path PS2 uses; the cache also retries once
    // with a strict PS1 ID). On a VCD (PS1) genuine miss, fall back to the POPSLoader-style suffixless
    // cover named exactly like the .VCD, next to it in the POPS/ folder (mmceArtPrimary IS the VCD scan
    // prefix). Cover/icon only, VCD view only -- a PS2 list and any hit never pay the extra probe.
    int r = mmceTryLoadImage(mmceArtPrimary, folder, isRelative, value, suffix, resultTex);
    if (r == ERR_BAD_FILE && isRelative && vcdViewActive(itemList->mode))
        r = vcdLoadPopsCover(mmceArtPrimary, value, suffix, resultTex);
    return r;
}

static int mmceGetTextId(item_list_t *itemList)
{
    int mode = _STR_MMCE_GAMES;

    return mode;
}

static int mmceGetIconId(item_list_t *itemList)
{
    int mode = MMCE_ICON;

    return mode;
}

// This may be called, even if mmceInit() was not.
static void mmceCleanUp(item_list_t *itemList, int exception)
{
    if (mmceGameList.enabled) {
        LOG("MMCESUPPORT CleanUp\n");

        free(mmceGames);
        mmceGames = NULL;
        free(mmceVcdGames); // #120: free the separate VCD array too; NULL both (CleanUp + Shutdown both run)
        mmceVcdGames = NULL;

        //      if ((exception & UNMOUNT_EXCEPTION) == 0)
        //          ...
    }
}

// This may be called, even if mmceInit() was not.
static void mmceShutdown(item_list_t *itemList)
{
    if (mmceGameList.enabled) {
        LOG("MMCESUPPORT Shutdown\n");

        free(mmceGames);
        mmceGames = NULL;
        free(mmceVcdGames);
        mmceVcdGames = NULL;
    }

    // As required by some (typically 2.5") HDDs, issue the SCSI STOP UNIT command to avoid causing an emergency park.
    // fileXioDevctl("mass:", USBMASS_DEVCTL_STOP_ALL, NULL, 0, NULL, 0);
}

static int mmceCheckVMC(item_list_t *itemList, char *name, int createSize)
{
    return sysCheckVMC(mmcePrefix, "/", name, createSize, NULL);
}

static char *mmceGetPrefix(item_list_t *itemList)
{
    return mmcePrefix;
}

static item_list_t mmceGameList = {
    MMCE_MODE, 2, 0, 0, MENU_MIN_INACTIVE_FRAMES, MMCE_MODE_UPDATE_DELAY, NULL, NULL, &mmceGetTextId, &mmceGetPrefix, &mmceInit, &mmceNeedsUpdate,
    &mmceUpdateGameList, &mmceGetGameCount, &mmceGetGame, &mmceGetGameName, &mmceGetGameNameLength, &mmceGetGameStartup, &mmceDeleteGame, &mmceRenameGame,
    &mmceLaunchGame, &mmceGetConfig, &mmceGetImage, &mmceCleanUp, &mmceShutdown, &mmceCheckVMC, &mmceGetIconId, &mmceLaunchVcd};
