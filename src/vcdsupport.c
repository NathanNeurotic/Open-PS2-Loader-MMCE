/*
  Copyright 2024, Open-PS2-Loader contributors
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.

  VCD (PS1-via-POPSTARTER) scan + path resolution. See include/vcdsupport.h. POSIX directory IO
  only -- the newlib port rejects direct fileXio use (same rule as favsupport.c), and the stock
  game scan (supportbase.c) already uses opendir/readdir on device prefixes.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>    // errno/ENOENT in the vcdScanOpenDir absent-vs-contended split (#154)
#include <sys/stat.h> // mkdir (POSIX, used like util.c / OSDHistory.c)

#include "include/opl.h"         // pulls <dirent.h> (opendir/readdir/DIR) + strcasecmp, like supportbase.c
#include "include/system.h"      // POPS_FOLDER
#include "include/ioman.h"       // LOG (BDMA equip probe trace)
#include "include/bdmsupport.h"  // BDM_TYPE_* + bdmGetDeviceRootByType (BDMA source differentiation)
#include "include/mmcesupport.h" // mmceLoadModules (ensure mmceman for the MMCE BDMA source)
#include "include/gui.h"         // guiWarning (passing toast on a failed launch-path BDMA equip)
#include "include/util.h"        // checkMCSaveIconsDir -- browser icon pair for the POPSTARTER folder
#include "include/lang.h"        // _l + _STR_BDMA_ERR_* (same texts the Settings-screen equip shows)
#include "include/textures.h"    // texDiscoverLoad + ERR_BAD_FILE (VCD POPS cover fallback)
#include "include/bdma_embed.h"  // embedded BDMAssault variant pairs (gzipped; PROVENANCE.md)
#include <zlib.h>                // inflate for the embedded pairs (already linked via libpng)
#include "include/vcdsupport.h"
#include "include/retrogem.h"


int vcdExtractGameId(const char *name, char *idOut, int idSize)
{
    int i;

    if (idOut == NULL || idSize <= 11)
        return 0;
    idOut[0] = '\0';
    if (name == NULL || strlen(name) < 13)
        return 0; // require AAAA_NNN.NN plus a separator and non-empty title
    for (i = 0; i < 4; i++)
        if (!((name[i] >= 'A' && name[i] <= 'Z') || (name[i] >= 'a' && name[i] <= 'z') ||
              (name[i] >= '0' && name[i] <= '9')))
            return 0;
    if (name[4] != '_')
        return 0;
    for (i = 5; i <= 7; i++)
        if (name[i] < '0' || name[i] > '9')
            return 0;
    if (name[8] != '.' || name[9] < '0' || name[9] > '9' || name[10] < '0' || name[10] > '9')
        return 0;
    if ((name[11] != '.' && name[11] != '_') || name[12] == '\0')
        return 0;

    memcpy(idOut, name, 11);
    idOut[11] = '\0';
    return 1;
}

// Display-only prefix hider (aesthetic setting gVcdHideGameId). Returns the number of leading
// characters to skip when `name` begins with a STRICT PS1 retail game-ID prefix AAAA_NNN.NN
// followed by a '.' or '_' separator (= 12 chars, e.g. "SLUS_005.51." / "SCUS_941.63."), and only
// when there is a non-empty title after it. Returns 0 otherwise, so clean titles are never cut.
// The strict character checks prevent a false positive from eating a real title and short-circuit
// safely on names shorter than 12 characters.
static int vcdGameIdPrefixLen(const char *name)
{
    char gameId[VCD_ID_MAX];
    return vcdExtractGameId(name, gameId, sizeof(gameId)) ? 12 : 0;
}

// Render-time display name for the VCD list. PURELY COSMETIC: returns a pointer PAST a leading
// game-ID prefix when the "hide game ID" option is on AND this is a VCD view AND the name really
// starts with one; otherwise returns `text` unchanged. The stored name is never modified, so
// launch selectors, cover-art keys, favourites match-by-name and config keys all keep the full
// name -- callers MUST use the result for drawing only.
const char *vcdDisplayName(int mode, const char *text)
{
    int n;
    if (!gVcdHideGameId || text == NULL || !vcdViewActive(mode))
        return text;
    n = vcdGameIdPrefixLen(text);
    return n ? text + n : text;
}

// Case-insensitive name order for the scan sort below. MUST stay in the same collation submenuSort
// uses (strcasecmp on the SAME display-adjusted key -- menusys.c was updated alongside this to sort by
// vcdDisplayName), or the two disagree and the menu-level gAutosort pass, which runs LAST, silently
// undoes this backing array's order.
static int vcdEntryCmp(const void *a, const void *b)
{
    const char *na = ((const vcd_entry_t *)a)->name;
    const char *nb = ((const vcd_entry_t *)b)->name;
    // Sort by the TITLE the user actually sees. With "hide game ID" on (gVcdHideGameId), the list
    // renders each name past its "SLES_123.45." prefix (vcdDisplayName), so sorting the raw filenames
    // ordered by the invisible publisher code -- alphabetical to nobody (Blade, HW, #195: "omit that
    // part in the alphabet"). Skip the same prefix here so the visible order matches the visible text.
    // With hide off, the prefix IS shown, so it stays part of the sort key. vcdGameIdPrefixLen returns
    // 0 for a name without a strict game-ID prefix, so untagged titles are unaffected either way.
    if (gVcdHideGameId) {
        na += vcdGameIdPrefixLen(na);
        nb += vcdGameIdPrefixLen(nb);
    }
    return strcasecmp(na, nb);
}

// Core scan: opendir `dirPath` and collect *.VCD basenames into a fresh vcd_entry_t list. POSIX dir
// IO only (newlib-port rule). Shared by vcdScanDir (POPS subfolder) and vcdScanDirRoot (path as-is).
// ---- PS1 disc-id resolution for the game list (issue #380) --------------------------------
//
// A theme shows the game id through an AttributeText bound to #Startup. A PS2 row has a real disc id
// in game->startup; a VCD row had the whole FILENAME, so long PS1 titles overflowed into the cover
// art. That is #380, and it reproduced on every theme the reporter tried -- because no theme can fix
// a value the loader never supplied.
//
// The first attempt parsed the id out of the FILENAME. Free, and correct when a file is named
// "SLUS_123.45.Title" -- and the reporter's library is not, which is why he still saw full titles on
// the build meant to fix it. So when the name yields nothing, ask the DISC: retrogemGetVcdGameID
// reads the id from inside the image (ISO 9660 / SYSTEM.CNF, then PVD, then partition and subpath
// forms). It is the same resolver the RetroGEM barcode uses, so a game reports the SAME id here as
// it does to the scanner -- which is the point, not a coincidence.
//
// RESOLVED HERE, IN THE SCAN, AND DELIBERATELY NOT ONE LAYER UP. There are two VCD layouts:
// vcdScanDir appends "POPS" to a device root, and vcdScanDirRoot takes a directory DIRECTLY (the
// APA/PFS HDD, e.g. "pfs1:/"). A resolver above them has to REBUILD a path, so it can only be right
// about one of them -- it would have quietly produced a wrong path for HDD VCDs. Down here the
// directory is the one that was actually opened and the filename is the one readdir just returned,
// so the path is correct for both layouts by construction rather than by argument.
//
// The disc read is fenced three ways, because this runs on every list build:
//   1. only reached when the free filename parse fails;
//   2. every answer MEMOIZED for the session, hit AND miss, so a game is read at most once no matter
//      how often a background rescan rebuilds the list;
//   3. a per-scan budget caps how many discs one build may open, so a large badly-named library
//      degrades to "some ids resolve, the rest show their filename" instead of stalling the list.
//      The remainder resolve on later builds, a few at a time.
#define VCD_ID_PROBE_BUDGET 8   // discs opened per scan
#define VCD_ID_MEMO_MAX     512 // ids remembered for the session

typedef struct vcd_id_memo
{
    struct vcd_id_memo *next;
    char id[VCD_ID_MAX]; // empty = asked the disc, it had none -- worth remembering so we stop asking
    char name[];         // flexible: VCD basenames may be long, most are far short of the cap
} vcd_id_memo_t;

static vcd_id_memo_t *gVcdIdMemo = NULL;
static int gVcdIdMemoCount = 0;

static const char *vcdIdMemoFind(const char *name)
{
    for (vcd_id_memo_t *m = gVcdIdMemo; m != NULL; m = m->next) {
        if (!strcmp(m->name, name))
            return m->id;
    }
    return NULL;
}

static void vcdIdMemoAdd(const char *name, const char *id)
{
    if (gVcdIdMemoCount >= VCD_ID_MEMO_MAX)
        return; // full: later games re-probe within their budget -- slow, never wrong

    size_t len = strlen(name);
    vcd_id_memo_t *m = (vcd_id_memo_t *)malloc(sizeof(vcd_id_memo_t) + len + 1);
    if (m == NULL)
        return;
    memcpy(m->name, name, len + 1);
    snprintf(m->id, sizeof(m->id), "%s", id ? id : "");
    m->next = gVcdIdMemo;
    gVcdIdMemo = m;
    gVcdIdMemoCount++;
}

void vcdInvalidateGameIds(void)
{
    vcd_id_memo_t *m = gVcdIdMemo;
    gVcdIdMemo = NULL;
    gVcdIdMemoCount = 0;
    while (m != NULL) {
        vcd_id_memo_t *next = m->next;
        free(m);
        m = next;
    }
}

// dirPath is the directory that was just opened; fileName is what readdir returned (with ".VCD");
// baseName is that name with the extension stripped. Writes the id, or an empty string when none.
static void vcdResolveScanId(const char *dirPath, const char *fileName, const char *baseName,
                             char *idOut, int idSize, int *budget)
{
    idOut[0] = '\0';

    if (vcdExtractGameId(baseName, idOut, idSize) && idOut[0] != '\0')
        return; // the name already carries it: free

    const char *memo = vcdIdMemoFind(baseName);
    if (memo != NULL) {
        snprintf(idOut, idSize, "%s", memo);
        return; // including a remembered "no id" -- do not open it again
    }

    if (budget == NULL || *budget <= 0)
        return; // out of budget this scan; a later build resolves it

    char vcdPath[256];
    char discId[RETROGEM_GAMEID_MAX];
    size_t dl = strlen(dirPath);
    const char *sep = (dl > 0 && dirPath[dl - 1] == '/') ? "" : "/";

    (*budget)--;
    if (snprintf(vcdPath, sizeof(vcdPath), "%s%s%s", dirPath, sep, fileName) >= (int)sizeof(vcdPath))
        return;

    if (retrogemGetVcdGameID(vcdPath, discId, sizeof(discId)) && discId[0] != '\0') {
        vcdIdMemoAdd(baseName, discId);
        snprintf(idOut, idSize, "%s", discId);
        return;
    }
    vcdIdMemoAdd(baseName, ""); // genuinely has none
}

static int vcdScanOpenDir(const char *dirPath, vcd_entry_t **outList)
{
    errno = 0; // clear BEFORE opendir so the NULL branch reads THIS call's errno, not a stale one
    DIR *dir = opendir(dirPath);
    if (dir == NULL) {
        // Absent-vs-contended split (#154 audit residual): returning -1 for BOTH made a card with
        // NO POPS folder burn mmcesupport's bounded rescan budget (MMCE_VCD_SCAN_RETRY_MAX) on
        // every refresh. errno IS faithful here: newlib's opendir is a plain open() that returns
        // NULL without touching errno (verified in the toolchain's libc disassembly), so the glue's
        // __transform_errno result survives -- the same propagation textures.c's mmce art classifier
        // and supportbase.c's sbRename already rely on. ENOENT = the folder is GENUINELY absent ->
        // return 0 ("readable, no VCDs here"): the caller treats it as an empty scan and arms NO
        // retry. Every other errno keeps the -1 failure semantics below.
        if (errno == ENOENT)
            return 0;
        // could NOT read the dir (device momentarily unreadable / bus contended). Signal a scan
        // FAILURE -- distinct from a readable-but-empty dir (opens fine, count 0) -- so the caller
        // PRESERVES its last-good list instead of blanking it on a transient wedge. Mirrors
        // scanForISO (the ISO path) -- the #120 fix: the VCD & ISO views share one backing store,
        // and returning 0 here zeroed BOTH on a contended bus.
        // MMCE caveat: mmceman's dopen collapses EVERY failure -- including the card's explicit
        // not-found reply -- into a bare -1 (EE sees EPERM, not ENOENT), so on mmceN: an absent
        // POPS still lands on this -1 path unless the paired mmceman-fs-dopen-enoent.patch is in
        // the build (install_coherent_mmce.sh applies it, same doctrine as the fs-open ENOENT
        // patch behind textures.c's classifier). Without it, MMCE behavior is exactly the old one:
        // bounded retries, then quiesce -- never worse.
        return -1;
    }

    vcd_entry_t *list = (vcd_entry_t *)calloc(VCD_MAX_ITEMS, sizeof(vcd_entry_t));
    if (list == NULL) {
        closedir(dir);
        return -1; // OOM: cannot build a list -> preserve the caller's current one rather than blank it
    }

    int count = 0;
    int idBudget = VCD_ID_PROBE_BUDGET;
    struct dirent *de;
    while (count < VCD_MAX_ITEMS && (de = readdir(dir)) != NULL) {
        int len = (int)strlen(de->d_name);
        if (len < 5 || strcasecmp(de->d_name + len - 4, ".VCD") != 0)
            continue;          // keep only "*.VCD" (case-insensitive)
        int baseLen = len - 4; // strip ".VCD"
        // Skip names no launch leg can start truthfully (#154 forensics) -- listing them made a
        // dead X button:
        // - basenames longer than ISO_GAME_NAME_MAX: the game list stores base_game_info_t names
        //   capped at 160, and every VCD launch resolves BY NAME -- the truncated name targets a
        //   nonexistent file and POPSTARTER drops to OSDSYS. Rename the file to fix.
        // - "POPSTARTER": reserved -- its selector would be "XX.POPSTARTER.ELF", colliding with
        //   POPSTARTER's own naming; the launch legs have always rejected it (silently, which
        //   looked like a dead X button while it was still listed). Case-insensitive: FAT is.
        if (baseLen > ISO_GAME_NAME_MAX) {
            LOG("VCD skip (name > %d chars, unlaunchable): %s\n", ISO_GAME_NAME_MAX, de->d_name);
            continue;
        }
        if (baseLen == 10 && strncasecmp(de->d_name, "POPSTARTER", 10) == 0) {
            LOG("VCD skip (reserved name): %s\n", de->d_name);
            continue;
        }
        if (baseLen > VCD_NAME_MAX - 1)
            baseLen = VCD_NAME_MAX - 1; // unreachable after the cap above; kept as a belt
        memcpy(list[count].name, de->d_name, baseLen);
        list[count].name[baseLen] = '\0';
        vcdResolveScanId(dirPath, de->d_name, list[count].name,
                         list[count].id, sizeof(list[count].id), &idBudget);
        count++;
    }
    closedir(dir);

    if (count == 0) {
        free(list);
        return 0;
    }

    // #195 (Blade, HW): VCD lists arrived in readdir order -- i.e. FAT directory-entry order, which is
    // effectively random after any add/delete churn -- while every other list reads alphabetical. Sort
    // the BACKING array here at the scan, not the menu rows: every consumer then agrees (device pages,
    // the FAV VCD view), ids stay in lockstep with parallel arrays (the HDD path keeps a
    // per-id partition-label array), and it holds no matter which publish path builds the rows.
    // Gated on the same Automatic Sorting switch the game lists honour: Autosort off = raw dir order,
    // as for every other list. Launch/favourites are unaffected either way -- VCDs resolve BY NAME.
    if (gAutosort && count > 1)
        qsort(list, count, sizeof(vcd_entry_t), &vcdEntryCmp);

    *outList = list;
    return count;
}

int vcdScanDir(const char *devPrefix, vcd_entry_t **outList)
{
    if (outList == NULL)
        return 0;
    *outList = NULL;
    if (devPrefix == NULL)
        return 0;

    char dirPath[256];
    snprintf(dirPath, sizeof(dirPath), "%s%s", devPrefix, POPS_FOLDER); // "<prefix>POPS" (prefix ends in '/')

    return vcdScanOpenDir(dirPath, outList);
}

// Scan a directory path DIRECTLY (no POPS/ subfolder) for *.VCD -- used for the APA/PFS HDD, where
// each __.POPS* partition holds its .VCD at the mounted root (caller passes e.g. "pfs1:/").
int vcdScanDirRoot(const char *dirPath, vcd_entry_t **outList)
{
    if (outList == NULL)
        return 0;
    *outList = NULL;
    if (dirPath == NULL)
        return 0;

    return vcdScanOpenDir(dirPath, outList);
}

// POPStarter path separator for a device prefix: '\\' for SMB (ethPrefix ends in '\\'), else '/'.
// Auto-detected from the prefix's trailing char so one code path serves both mass/mmce and SMB.
static char vcdSep(const char *devPrefix)
{
    int n = (devPrefix != NULL) ? (int)strlen(devPrefix) : 0;
    return (n > 0 && devPrefix[n - 1] == '\\') ? '\\' : '/';
}

// VCD (PS1) cover FALLBACK. OPL's own art (<dev>ART/<name>_COV.png) is the PRIMARY -- each device's
// getImage tries it first; this only runs on a genuine miss. It loads the POPSLoader-style cover named
// exactly like the .VCD (suffixless "<name>.png"), sitting NEXT TO the game in the same POPS/ folder the
// VCD scan reads -- so a cover whose name matches the VCD minus the extension still shows (FifthFox, HW
// 2026-07-16; the tier removed by 86da2023's #120 single-lookup simplification). `scanPrefix` MUST be the
// SAME prefix the device passes to vcdFillGameList (the device root for BDM, mmcePrefix for MMCE, etc.),
// so the cover is looked up beside the .VCD. COVER/ICON ONLY -- background/logo/screenshot must never
// fall back to the single suffixless file or each would render the cover instead. Returns
// texDiscoverLoad's result (>= 0 hit, negative miss). No miss-memo: kept deliberately simple -- callers
// gate this on a genuine ERR_BAD_FILE + the VCD view, so a PS2 list and a hit never pay the extra probe.
int vcdLoadPopsCover(const char *scanPrefix, const char *value, const char *suffix, GSTEXTURE *resultTex)
{
    char path[256];

    if (scanPrefix == NULL || value == NULL || suffix == NULL || resultTex == NULL)
        return ERR_BAD_FILE; // resultTex too: texDiscoverLoad dereferences it (CodeRabbit review of #203)
    if (strcmp(suffix, "COV") != 0 && strcmp(suffix, "ICO") != 0)
        return ERR_BAD_FILE; // only the cover/icon fall back to the suffixless POPS name

    // Same directory the scan reads: "<scanPrefix>POPS<sep><value>"; texDiscoverLoad appends the extension.
    snprintf(path, sizeof(path), "%s%s%c%s", scanPrefix, POPS_FOLDER, vcdSep(scanPrefix), value);
    return texDiscoverLoad(resultTex, path, -1);
}

// Probe POPS/POPSTARTER.ELF on a device root given WITHOUT a trailing separator ("mass0", "mc0", "pfs0").
// Tries "<root>:/POPS/..." then "<root>:POPS/..." and returns 1 with `out` filled on the first hit.
static int vcdTryPopsAtRoot(const char *root, char *out, int outSize)
{
    static const char *forms[] = {"%s:/" POPS_FOLDER "/POPSTARTER.ELF", "%s:" POPS_FOLDER "/POPSTARTER.ELF"};
    int i;
    for (i = 0; i < 2; i++) {
        snprintf(out, outSize, forms[i], root);
        int fd = open(out, O_RDONLY);
        if (fd >= 0) {
            close(fd);
            return 1;
        }
    }
    return 0;
}

int vcdResolvePopstarter(const char *devPrefix, char *out, int outSize)
{
    if (out == NULL || outSize <= 0)
        return 0;

    // POPSTARTER.ELF Device (gPopstarterDevice, General Settings): Custom free-text path | a device TYPE
    // (mc/usb/mx4sio/mmce/exfat/apa) -> <root>:/POPS/POPSTARTER.ELF | Default -> the boot device (cwd)
    // then the VCD's own device. Each candidate is open()-probed; a miss falls through to the next tier.
    // NOTE: this serves the bdm/eth/mmce launch paths; the HDD VCD launch keeps its own freeze-guarded
    // hddResolveHddPopstarter (the __common/+OPL pfs search), so HDD VCDs always load POPSTARTER off the HDD.

    // GAME'S DEVICE: resolve ONLY on the VCD's own device (devPrefix); no boot-device (cwd) tier and
    // no Default fallthrough. A miss returns 0 so the launch path shows "Missing POPSTARTER.ELF"
    // (every caller does) instead of silently loading a boot-device copy the user did not pick.
    if (gPopstarterDevice == POPS_DEV_GAME) {
        if (devPrefix == NULL)
            return 0;
        snprintf(out, outSize, "%s%s%cPOPSTARTER.ELF", devPrefix, POPS_FOLDER, vcdSep(devPrefix));
        int fd = open(out, O_RDONLY);
        if (fd < 0)
            return 0;
        close(fd);
        return 1;
    }

    // CUSTOM: the free-text path wins, if it exists.
    if (gPopstarterDevice == POPS_DEV_CUSTOM && gPopstarterPath[0] != '\0') {
        int cfd = open(gPopstarterPath, O_RDONLY);
        if (cfd >= 0) {
            close(cfd);
            snprintf(out, outSize, "%s", gPopstarterPath);
            return 1;
        }
        // Custom set but missing -> fall through to Default below.
    }

    // A specific device TYPE -> resolve its live root, then probe <root>:/POPS/POPSTARTER.ELF.
    {
        int bt = -1;
        switch (gPopstarterDevice) {
            case POPS_DEV_MC:
                if (vcdTryPopsAtRoot("mc0", out, outSize) || vcdTryPopsAtRoot("mc1", out, outSize))
                    return 1;
                break;
            case POPS_DEV_MMCE:
                if (vcdTryPopsAtRoot("mmce0", out, outSize) || vcdTryPopsAtRoot("mmce1", out, outSize))
                    return 1;
                break;
            case POPS_DEV_USB:
                bt = BDM_TYPE_USB;
                break;
            case POPS_DEV_MX4SIO:
                bt = BDM_TYPE_SDC;
                break;
            case POPS_DEV_EXFAT_HDD:
                bt = BDM_TYPE_ATA;
                break;
            case POPS_DEV_APA_HDD:
                // TRAP, deliberately skipped: this resolver serves the bdm/eth/mmce launch paths, and
                // every one of them deinit()s with its OWN mode excepted -- hddShutdown then unmounts
                // pfs0: BEFORE sysLaunchPopstarter re-opens the resolved ELF, so a pfs0: path that
                // open()-probes fine HERE is dead by the time it is read (black screen into deinit'd
                // OPL). HDD-page VCD launches keep APA POPSTARTER via their own freeze-guarded
                // hddResolveHddPopstarter; for the rest, fall through to the Default tiers below.
                LOG("VCD POPSTARTER Device 'HDD (APA)' is HDD-page-only; falling back to Default for this launch\n");
                break;
            default:
                break;
        }
        if (bt >= 0) {
            char root[BDM_DEVICE_ROOT_MAX];
            if (bdmGetDeviceRootByType(bt, root, sizeof(root))) {
                char *colon = strchr(root, ':');
                if (colon)
                    *colon = '\0'; // "massN:/" -> "massN"
                if (vcdTryPopsAtRoot(root, out, outSize))
                    return 1;
            }
        }
        // A TYPE was chosen but its device has no POPSTARTER.ELF -> fall through to Default.
    }

    // DEFAULT (or any miss above): the VCD's OWN device first, then the boot device (cwd).
    //
    // This order is POPSLoader's, and it is deliberate there: its resolver takes the game device's
    // own POPS/POPSTARTER.ELF ahead of the sidecar beside the loader, so that a per-device build
    // can be used without being forced on anyone. We had those two tiers swapped.
    //
    // Why it matters beyond tidiness: POPSTARTER's IGR behaviour lives INSIDE the binary that gets
    // executed -- config byte $424 selects the exit method, and the ELF loader that chains to
    // mc0:/BOOT/BOOT.ELF on reset is POPSTARTER's own. On a console where the boot device and the
    // game device each carry a POPS/POPSTARTER.ELF, this fork would run the boot device's copy
    // while POPSLoader runs the game device's -- two different binaries, two different IGR
    // behaviours, on the same console with the same card. Matching the reference implementation
    // removes that as a variable.
    //
    // The explicit tiers above (POPS_DEV_CUSTOM, POPS_DEV_GAME and the device-TYPE picker) are
    // untouched, so anyone who wants the boot-device copy can still name it.
    if (devPrefix != NULL) {
        snprintf(out, outSize, "%s%s%cPOPSTARTER.ELF", devPrefix, POPS_FOLDER, vcdSep(devPrefix));
        int fd = open(out, O_RDONLY);
        if (fd >= 0) {
            close(fd);
            return 1;
        }
    }

    if (gBootDir[0] != '\0') {
        size_t bl = strlen(gBootDir);
        const char *joiner = (gBootDir[bl - 1] == '/') ? "" : "/"; // gBootDir ends in ':' or a folder name
        snprintf(out, outSize, "%s%s%s/POPSTARTER.ELF", gBootDir, joiner, POPS_FOLDER);
        int fd = open(out, O_RDONLY);
        if (fd >= 0) {
            close(fd);
            return 1;
        }
    }

    return 0;
}

void vcdBuildSelector(const char *devPrefix, const char *prefix, const char *name, char *out, int outSize)
{
    if (out == NULL || outSize <= 0)
        return;
    // POPSTARTER does its OWN SifIopReset + BDMAssault remount BEFORE it reads argv[0], so it resolves
    // the selector against its post-reset namespace -- the BARE device kind-label (mass:/smb:), NOT
    // OPL's live unit-numbered mount (mass0:/mmce0:/smb0:) or the SMB share path. Handing it a
    // unit-numbered path (or backslash-separated SMB path) leaves the sibling <name>.VCD unresolvable
    // and it black-screens after being reached. Match the maintainer's proven POPSLoader format
    // (bin/POPSLDR/system.lua): <bare-label>:/POPS/<XX.|SB.><name>.ELF, forward slashes throughout.
    // RiptOPL mounts every block VCD source (USB/MX4SIO/iLink/exFAT-HDD) through the BDMAssault
    // usbhdfsd variant, which POPSTARTER re-registers as "mass:" -- and MMCE is translated to mass:
    // too -- so the ONLY distinction is SMB (SB. prefix, "smb:") vs everything else ("mass:"). The
    // incoming devPrefix (a live unit-numbered mount) is intentionally NOT used for the string body.
    (void)devPrefix;
    const char *root = (prefix != NULL && !strcmp(prefix, VCD_PREFIX_SMB)) ? "smb:" : "mass:";
    snprintf(out, outSize, "%s/%s/%s%s.ELF", root, POPS_FOLDER, prefix ? prefix : "", name ? name : "");
}

// ---- per-device VCD view state ------------------------------------------------

static unsigned char vcdView[MODE_COUNT];  // 1 = this mode is showing its VCD list
static unsigned char vcdDirty[MODE_COUNT]; // 1 = view just toggled -> force one rescan

int vcdModeSupported(int mode)
{
    // FAV_MODE has its own L3 ISO<->VCD view too: the Favourites tab swaps between disc favourites and
    // PS1/.VCD favourites (favsupport filters its list by vcdViewActive(FAV_MODE)). Its vcdView slot is
    // independent of any device's, so toggling Favourites never disturbs a device page's view.
    // UDPFS_MODE was MISSING from this list -- in the fork too (a latent bug there, masked because
    // VCD was mostly used on BDM/HDD pages). Since this predicate is the FIRST gate in
    // itemExecToggleView AND the L3 hint condition, its absence made the page's entire shipped VCD
    // surface unreachable dead code: udpfssupport.c's vcdFillGameList call, the POPS cover
    // fallback, and the POPSTARTER launch guard (which safely refuses the actual launch with a
    // toast -- network filesystems cannot host a POPSTARTER handoff, but LISTING the library is
    // exactly what the code was written to do). First deliberate divergence from the fork's
    // vcdsupport.c, and it is a fix, not a drift.
    return (mode >= BDM_MODE && mode <= BDM_MODE_LAST) || mode == MMCE_MODE || mode == ETH_MODE || mode == HDD_MODE || mode == FAV_MODE || mode == UDPFS_MODE;
}

int vcdViewActive(int mode)
{
    if (mode < 0 || mode >= MODE_COUNT || !vcdModeSupported(mode))
        return 0;
    // The global default-view setting overrides the per-device L3 toggle when locked to one type.
    if (gDefaultGameView == GAME_VIEW_ISO)
        return 0; // locked to the ISO/disc list
    if (gDefaultGameView == GAME_VIEW_VCD)
        return 1;         // locked to the VCD (PS1) list
    return vcdView[mode]; // GAME_VIEW_BOTH: per-device L3 toggle (defaults to ISO)
}

void vcdToggleView(int mode)
{
    if (mode < 0 || mode >= MODE_COUNT)
        return;
    if (gDefaultGameView != GAME_VIEW_BOTH)
        return; // globally locked to one type -> the L3 toggle is disabled
    vcdView[mode] = vcdView[mode] ? 0 : 1;
    vcdDirty[mode] = 1;
}

int vcdConsumeDirty(int mode)
{
    if (mode < 0 || mode >= MODE_COUNT || !vcdDirty[mode])
        return 0;
    vcdDirty[mode] = 0;
    return 1;
}

// Mark every VCD-capable mode for one rescan -- call after the global default-view setting changes so
// each device page rebuilds its list (ISO <-> VCD) on its next refresh.
void vcdMarkAllDirty(void)
{
    for (int m = 0; m < MODE_COUNT; m++)
        if (vcdModeSupported(m))
            vcdDirty[m] = 1;
}

// #118: a multi-disc PS1 game is a set of separate .VCD files whose titles carry a disc token, e.g.
// "Game (Disc 2).VCD". With gVcdFirstDiscOnly on, the device VCD lists hide discs 2+ (POPSLoader
// parity), leaving Disc 1 as the single entry -- the .VCD files are NOT touched, so every disc stays
// on the card for in-game swapping. Detection is filename-only, case-insensitive: "(disc"/"[disc"/
// "(cd"/"(disk" (optionally spaced) followed by an integer >= 2. Disc 1 / CD 1 always stay. Accepted
// parity failure modes: a Disc 2 whose Disc 1 is absent vanishes; non-parenthesised schemes ("_2",
// "CD2" mid-word) are not caught.
int vcdIsHiddenDisc(const char *name)
{
    static const char *const tokens[] = {"(disc", "[disc", "(cd", "[cd", "(disk", "[disk"};
    if (name == NULL)
        return 0;
    for (unsigned t = 0; t < sizeof(tokens) / sizeof(tokens[0]); t++) {
        int toklen = (int)strlen(tokens[t]);
        for (const char *p = name; *p != '\0'; p++) {
            if (strncasecmp(p, tokens[t], toklen) != 0)
                continue;
            const char *d = p + toklen;
            while (*d == ' ')
                d++;
            if (*d < '0' || *d > '9')
                continue; // token not followed by a disc number
            int num = 0;
            while (*d >= '0' && *d <= '9')
                num = num * 10 + (*d++ - '0');
            if (num >= 2)
                return 1;
        }
    }
    return 0;
}

// Returns the game count (>= 0) and publishes the new list into *outGames, OR returns -1 on a
// transient scan FAILURE (device momentarily unreadable) leaving *outGames UNTOUCHED so the caller
// keeps its last-good list. Callers MUST assign the count only when the return is >= 0. This mirrors
// sbReadList / scanForISO (the ISO path): build into a LOCAL list and do NOT free the old one up
// front, so a contended-bus opendir failure can no longer blank the list (#120: the VCD & ISO views
// of a device share one backing store; the old free-up-front + return-0-on-fail zeroed BOTH).
int vcdFillGameList(const char *devPrefix, base_game_info_t **outGames)
{
    if (outGames == NULL)
        return 0;

    vcd_entry_t *vcds = NULL;
    int n = vcdScanDir(devPrefix, &vcds); // NOTE: does NOT touch *outGames
    if (n < 0) {
        return -1; // could not read the device -> preserve the caller's current list
    }
    // NOTE: the art miss-memo invalidation now lives in vcdScanOpenDir (the shared scan success path) so
    // it also covers the HDD vcdScanDirRoot path -- do NOT re-invalidate here (it already fired).

    base_game_info_t *games = NULL;
    int kept = 0;
    if (n > 0) {
        games = (base_game_info_t *)memalign(64, n * sizeof(base_game_info_t));
        if (games == NULL) {
            free(vcds);
            return -1; // OOM -> preserve rather than blank
        }
        memset(games, 0, n * sizeof(base_game_info_t));
        for (int i = 0; i < n; i++) {
            if (gVcdFirstDiscOnly && vcdIsHiddenDisc(vcds[i].name))
                continue; // #118: hide discs 2+ of a multi-disc PS1 set (device lists only)
            snprintf(games[kept].name, sizeof(games[kept].name), "%s", vcds[i].name);
            // #Startup is what a theme renders as the game id. The resolved id when we have one,
            // the filename when we do not -- which is the pre-#380 behaviour, so nothing regresses
            // for a disc whose id cannot be determined at all.
            snprintf(games[kept].startup, sizeof(games[kept].startup), "%s",
                     vcds[i].id[0] != '\0' ? vcds[i].id : vcds[i].name);
            snprintf(games[kept].extension, sizeof(games[kept].extension), ".VCD");
            games[kept].parts = 1;
            games[kept].format = GAME_FORMAT_ISO; // harmless; the per-mode VCD flag gates the launch path
            kept++;
        }
    }
    free(vcds);

    // Scan reached the device (n >= 0): NOW it is safe to replace the old list.
    free(*outGames);
    if (kept == 0) { // readable but empty (or every disc hidden) -> empty list
        free(games); // free(NULL) when n == 0 is a no-op
        *outGames = NULL;
        return 0;
    }
    *outGames = games; // buffer over-allocated to n when some discs were hidden -- harmless
    return kept;
}

// ---- safe memory-card copy (free-space gated) ---------------------------------------
// Equipping BDMA / SMB modules COPIES files onto mc?:/POPSTARTER/, and writing the small config
// markers (bdma_config.txt, IPCONFIG.DAT, SMBCONFIG.DAT) does the same. Filling a card to zero or
// leaving a half-written module there can wreck a user's POPSTARTER setup, so EVERY such write goes
// through these helpers: we refuse up front unless the destination card reports enough free space
// (plus a margin), and we delete any partially-written file on a short write. POSIX IO only.

#define VCD_MC_CLUSTER  1024        // PS2 memory-card cluster size; mcGetInfo "free" is in clusters
#define VCD_COPY_MARGIN (16 * 1024) // leave >=16 KiB head-room so we never pack the card to 0
#define VCD_COPY_CHUNK  (16 * 1024) // copy buffer (heap, not stack)

// Free bytes on the memory card backing `path` ("mc0:"/"mc1:"), or -1 if it isn't a usable PS2 MC.
static int vcdMcFreeBytes(const char *path)
{
    if (path == NULL || (path[0] != 'm' && path[0] != 'M') || (path[1] != 'c' && path[1] != 'C'))
        return -1;                       // not a memory-card path
    int port = (path[2] == '1') ? 1 : 0; // "mc1:" -> slot 1, anything else -> slot 0
    int type = 0, freeClusters = -1, format = 0, result = -1;
    mcGetInfo(port, 0, &type, &freeClusters, &format);
    mcSync(0, NULL, &result); // mcGetInfo is async; the vars are valid after the sync
    if (type != sceMcTypePS2 || format != MC_FORMATTED || freeClusters < 0)
        return -1; // no PS2 card / unformatted / query failed
    return freeClusters * VCD_MC_CLUSTER;
}

// Room for `needBytes` (+ margin) on `path`'s card? 1 = yes, 0 = no, -1 = not an MC / can't tell.
// Callers writing to an MC must treat 0 as "do NOT write"; -1 means the gate doesn't apply.
int vcdMcHasSpace(const char *path, int needBytes)
{
    int freeB = vcdMcFreeBytes(path);
    if (freeB < 0)
        return -1;
    return (freeB >= needBytes + VCD_COPY_MARGIN) ? 1 : 0;
}

// Copy srcPath -> dstPath, but only after confirming the destination card can hold it.
//   0  success      -1  source missing/unreadable
//  -2  MC too full (NOTHING written)   -3  write/IO error (partial dst removed)
int vcdSafeCopyFile(const char *srcPath, const char *dstPath)
{
    if (srcPath == NULL || dstPath == NULL)
        return -1;

    int sfd = open(srcPath, O_RDONLY);
    if (sfd < 0)
        return -1;

    // Probe source file size for the MC free-space pre-check. MMCE newlib does not support SEEK_END
    // (mirrors textures.c lines 402-403: it returns -1, causing every MMCE equip to abort here).
    // If SEEK_END fails, fall back to srcSize=0 -- the pre-check becomes conservative (always passes)
    // and the write-loop + unlink safety net still catches any actual out-of-space error.
    int srcSize = 0;
    {
        int sz = lseek(sfd, 0, SEEK_END);
        if (sz >= 0) {
            if (lseek(sfd, 0, SEEK_SET) < 0) {
                close(sfd);
                return -1;
            }
            srcSize = sz;
        }
        // SEEK_END failed (e.g. MMCE source): leave srcSize=0, no rewind needed (still at start).
    }

    // Free-space gate: only blocks when the destination IS a memory card and it won't fit.
    if (vcdMcHasSpace(dstPath, srcSize) == 0) {
        close(sfd);
        return -2;
    }

    char *buf = (char *)malloc(VCD_COPY_CHUNK);
    if (buf == NULL) {
        close(sfd);
        return -3;
    }
    int dfd = open(dstPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dfd < 0) {
        free(buf);
        close(sfd);
        return -3;
    }

    int rc = 0, r;
    while ((r = read(sfd, buf, VCD_COPY_CHUNK)) > 0) {
        int off = 0;
        while (off < r) {
            int w = write(dfd, buf + off, r - off);
            if (w <= 0) {
                rc = -3;
                break;
            }
            off += w;
        }
        if (rc != 0)
            break;
    }
    if (r < 0)
        rc = -3;

    close(dfd);
    close(sfd);
    free(buf);
    if (rc != 0)
        unlink(dstPath); // never leave a truncated module/config behind
    return rc;
}

// Write `len` bytes from `buf` to dstPath, gated by the same MC free-space check.
//   0 success   -2 MC too full (nothing written)   -3 write/IO error (partial dst removed)
int vcdSafeWriteFile(const char *dstPath, const void *buf, int len)
{
    if (dstPath == NULL || len < 0 || (buf == NULL && len > 0))
        return -3;
    if (vcdMcHasSpace(dstPath, len) == 0)
        return -2;

    int dfd = open(dstPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dfd < 0)
        return -3;
    const char *p = (const char *)buf;
    int off = 0, rc = 0;
    while (off < len) {
        int w = write(dfd, p + off, len - off);
        if (w <= 0) {
            rc = -3;
            break;
        }
        off += w;
    }
    close(dfd);
    if (rc != 0)
        unlink(dstPath);
    return rc;
}

// ---- BDMA (BDMAssault exFAT driver) equip -------------------------------------------
// POPStarter loads its block-device driver from mc?:/POPSTARTER/{usbd.irx,usbhdfsd.irx}. We let the
// user EQUIP one of three exFAT variants (or FAT32 = none) by copying THEIR OWN files from a source
// device's POPS/ folder -- RiptOPL embeds nothing. "BDMA MODE" picks the variant; "BDMA SOURCE"
// picks which device family to read the loose variant files from (named usbd.irx.<suffix>, the
// POPSLoader convention). The equip fires when either setting changes (opl.c), goes through the
// free-space-gated safe-copy, and records the equipped variant in mc?:/POPSTARTER/bdma_config.txt so
// the settings UI can reflect what's actually installed. (POPSLoader itself is a Lua loader that
// embeds its modules; there's no shared marker file to mirror, so we use the user's release-spec
// name bdma_config.txt with the variant suffix as its single-token contents.)

// MODE -> variant suffix on the loose source files (usbd.irx.<suffix>) AND the marker token.
static const char *vcdBdmaSuffix[VCD_BDMA_MODE_COUNT] = {"fat32", "usbexfat", "mx4sio", "mmce", "ata"};
// The two driver files POPStarter loads, equipped onto the MC WITHOUT the .<suffix>.
static const char *vcdBdmaModule[2] = {"usbd.irx", "usbhdfsd.irx"};

#define VCD_BDMA_MARKER "bdma_config.txt"

// Resolve the memory-card POPSTARTER folder (where the modules live). Prefer an existing folder;
// otherwise create it on the first present card. A slot-2-only first-time setup must not silently
// select absent mc0:, and mkdir/probe failure must reach the caller.
static int vcdResolvePopstarterMc(char *out, int outSize)
{
    static const char *cards[2] = {"mc0:/POPSTARTER", "mc1:/POPSTARTER"};
    static const char *roots[2] = {"mc0:/", "mc1:/"};

    if (out == NULL || outSize <= 0)
        return 0;
    out[0] = '\0';

    for (int i = 0; i < 2; i++) {
        DIR *d = opendir(cards[i]);
        if (d != NULL) {
            closedir(d);
            snprintf(out, outSize, "%s", cards[i]);
            return 1;
        }
    }

    for (int i = 0; i < 2; i++) {
        DIR *root = opendir(roots[i]);
        if (root == NULL)
            continue;
        closedir(root);

        snprintf(out, outSize, "%s", cards[i]);
        mkdir(out, 0777);
        DIR *created = opendir(out);
        if (created != NULL) {
            closedir(created);
            return 1;
        }
    }

    out[0] = '\0';
    return 0;
}

// Write the equipped-state marker mc?:/POPSTARTER/bdma_config.txt = the variant token.
// Returns 0 on success, or the vcdSafeWriteFile error code (-2 card full / -3 IO).
static int vcdWriteBdmaMarker(const char *mcDir, int mode)
{
    if (mode < 0 || mode >= VCD_BDMA_MODE_COUNT)
        return -1;
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", mcDir, VCD_BDMA_MARKER);
    const char *tok = vcdBdmaSuffix[mode];
    return vcdSafeWriteFile(path, tok, (int)strlen(tok));
}

int vcdReadBdmaMode(void)
{
    char mcDir[64];
    if (!vcdResolvePopstarterMc(mcDir, sizeof(mcDir)))
        return VCD_BDMA_FAT32;
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", mcDir, VCD_BDMA_MARKER);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return VCD_BDMA_FAT32; // no marker -> no exFAT modules -> FAT32 is the safe default
    char buf[32];
    int r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r <= 0)
        return VCD_BDMA_FAT32;
    buf[r] = '\0';
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == '\r' || buf[r - 1] == ' ' || buf[r - 1] == '\t'))
        buf[--r] = '\0'; // trim trailing whitespace/newline
    for (int m = 0; m < VCD_BDMA_MODE_COUNT; m++) {
        if (strcmp(buf, vcdBdmaSuffix[m]) == 0)
            return m;
    }
    return VCD_BDMA_FAT32;
}

// Embedded BDMAssault pair table, indexed by VCD_BDMA_* mode. usbexfat and mx4sio share ONE usbd
// blob (byte-identical upstream, deduped -- see modules/bdmassault/PROVENANCE.md). FAT32 row NULL.
typedef struct
{
    const void *usbdGz;
    const int *usbdGzLen;
    const void *hdfsdGz;
    const int *hdfsdGzLen;
} bdma_embedded_pair_t;

static const bdma_embedded_pair_t vcdBdmaEmbedded[VCD_BDMA_MODE_COUNT] = {
    {NULL, NULL, NULL, NULL},                                                                               // FAT32: POPStarter built-in driver
    {bdma_usbd_usb_gz, &size_bdma_usbd_usb_gz, bdma_usbhdfsd_usbexfat_gz, &size_bdma_usbhdfsd_usbexfat_gz}, // usbexfat
    {bdma_usbd_usb_gz, &size_bdma_usbd_usb_gz, bdma_usbhdfsd_mx4sio_gz, &size_bdma_usbhdfsd_mx4sio_gz},     // mx4sio (shared usbd)
    {bdma_usbd_mmce_gz, &size_bdma_usbd_mmce_gz, bdma_usbhdfsd_mmce_gz, &size_bdma_usbhdfsd_mmce_gz},       // mmce
    {bdma_usbd_ata_gz, &size_bdma_usbd_ata_gz, bdma_usbhdfsd_ata_gz, &size_bdma_usbhdfsd_ata_gz},           // ata
};

// Inflate one gzipped embedded blob. Raw size comes from the gzip ISIZE footer (last 4 bytes, LE),
// sanity-capped at 256 KiB (largest real pair member is ~48.5 KiB). Caller frees *out on success.
static int vcdInflateGzip(const unsigned char *gz, unsigned int gzLen, unsigned char **out, unsigned int *outLen)
{
    if (gz == NULL || gzLen < 18 || out == NULL || outLen == NULL)
        return -1;
    unsigned int rawLen = (unsigned int)gz[gzLen - 4] | ((unsigned int)gz[gzLen - 3] << 8) |
                          ((unsigned int)gz[gzLen - 2] << 16) | ((unsigned int)gz[gzLen - 1] << 24);
    if (rawLen == 0 || rawLen > 256 * 1024)
        return -1;
    unsigned char *buf = (unsigned char *)malloc(rawLen);
    if (buf == NULL)
        return -1;
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit2(&z, 15 + 16) != Z_OK) { // 15+16 = expect a gzip wrapper
        free(buf);
        return -1;
    }
    z.next_in = (Bytef *)gz;
    z.avail_in = gzLen;
    z.next_out = buf;
    z.avail_out = rawLen;
    int zr = inflate(&z, Z_FINISH);
    unsigned int got = (unsigned int)z.total_out;
    inflateEnd(&z);
    if (zr != Z_STREAM_END || got != rawLen) {
        free(buf);
        return -1;
    }
    *out = buf;
    *outLen = rawLen;
    return 0;
}

// Unpack the embedded pair for `mode` into the two staging paths, SEQUENTIALLY (one blob inflated at
// a time caps the transient heap at ~48.5 KiB + zlib state). Writes go through the EXISTING
// vcdSafeWriteFile (free-space check + partial-write cleanup -- Gemini review of #251: no redundant
// weaker writer). Returns 0, -1 (no pair for mode / unpack failed), or vcdSafeWriteFile's -2 (card
// full) / -3 (IO) so the caller's toast names the REAL problem.
static int vcdStageEmbeddedPair(int mode, const char *tmp0, const char *tmp1)
{
    if (mode <= VCD_BDMA_FAT32 || mode >= VCD_BDMA_MODE_COUNT || vcdBdmaEmbedded[mode].usbdGz == NULL)
        return -1;
    unsigned char *buf = NULL;
    unsigned int len = 0;
    if (vcdInflateGzip((const unsigned char *)vcdBdmaEmbedded[mode].usbdGz, (unsigned int)*vcdBdmaEmbedded[mode].usbdGzLen, &buf, &len) != 0)
        return -1;
    int r = vcdSafeWriteFile(tmp0, buf, (int)len);
    free(buf);
    if (r != 0)
        return r;
    buf = NULL;
    if (vcdInflateGzip((const unsigned char *)vcdBdmaEmbedded[mode].hdfsdGz, (unsigned int)*vcdBdmaEmbedded[mode].hdfsdGzLen, &buf, &len) != 0)
        return -1;
    r = vcdSafeWriteFile(tmp1, buf, (int)len);
    free(buf);
    return r;
}

int vcdEquipBdma(int source, int mode, char *diag, int diagSize)
{
    if (diag != NULL && diagSize > 0)
        diag[0] = '\0';

    if (mode < 0 || mode >= VCD_BDMA_MODE_COUNT || source < 0 || source >= VCD_BDMA_SRC_COUNT)
        return -1;

    char mcDir[64];
    if (!vcdResolvePopstarterMc(mcDir, sizeof(mcDir))) {
        if (diag != NULL && diagSize > 0)
            snprintf(diag, diagSize, "No writable PS2 memory card is available.");
        return -3;
    }

    char dst0[96], dst1[96];
    snprintf(dst0, sizeof(dst0), "%s/%s", mcDir, vcdBdmaModule[0]);
    snprintf(dst1, sizeof(dst1), "%s/%s", mcDir, vcdBdmaModule[1]);

    if (mode == VCD_BDMA_FAT32) {
        // FAT32 fallback: remove the exFAT modules so POPStarter uses its built-in driver.
        unlink(dst0);
        unlink(dst1);
        int mr = vcdWriteBdmaMarker(mcDir, mode);
        return (mr != 0) ? mr : 0;
    }

    const char *suffix = vcdBdmaSuffix[mode];
    char src0[96], src1[96];
    int found = 0;

    // FifthFox's adaptive seek order (maintainer-approved): look for the variant pair at the CUSTOM
    // POPSTARTER.ELF's own folder first (a user who relocated POPSTARTER keeps its drivers beside it),
    // then the BOOT (cwd) device's POPS/ folder, and only then the game device's family search below.
    // This is also the FAST order: both pre-candidates live on ALREADY-LOADED stacks (we booted from
    // one and are launching from the other), so a hit here costs a few open() probes and skips the
    // family search's transport force-loads and bounded mount waits entirely.
    {
        char preBuf[2][96];
        const char *pre[2];
        int npre = 0;

        if (gPopstarterDevice == POPS_DEV_CUSTOM && gPopstarterPath[0] != '\0') {
            const char *s1 = strrchr(gPopstarterPath, '/');
            const char *s2 = strrchr(gPopstarterPath, '\\'); // SMB custom paths use backslashes
            // Not `(s2 > s1)`: relationally comparing a possibly-NULL pointer is UB in ISO C.
            const char *sl = s1;
            if (s2 != NULL && (sl == NULL || s2 > sl))
                sl = s2;
            int n = (sl != NULL) ? (int)(sl - gPopstarterPath) + 1 : 0; // keep the separator
            if (n > 0 && n < (int)sizeof(preBuf[0])) {
                memcpy(preBuf[npre], gPopstarterPath, n);
                preBuf[npre][n] = '\0';
                pre[npre] = preBuf[npre];
                npre++;
            }
        }
        if (gBootDir[0] != '\0') {
            const char *colon = strchr(gBootDir, ':');
            int n = (colon != NULL) ? (int)(colon - gBootDir) + 1 : 0;
            if (n > 0 && (n + (int)sizeof("/" POPS_FOLDER "/")) < (int)sizeof(preBuf[1])) {
                memcpy(preBuf[npre], gBootDir, n);
                preBuf[npre][n] = '\0';
                strcat(preBuf[npre], "/" POPS_FOLDER "/");
                pre[npre] = preBuf[npre];
                npre++;
            }
        }

        for (int i = 0; i < npre && !found; i++) {
            snprintf(src0, sizeof(src0), "%s%s.%s", pre[i], vcdBdmaModule[0], suffix);
            snprintf(src1, sizeof(src1), "%s%s.%s", pre[i], vcdBdmaModule[1], suffix);
            int f0 = open(src0, O_RDONLY);
            LOG("[BDMA] pre-probe %s -> %d\n", src0, f0);
            if (f0 < 0)
                continue;
            close(f0);
            int f1 = open(src1, O_RDONLY);
            LOG("[BDMA] pre-probe %s -> %d\n", src1, f1);
            if (f1 < 0)
                continue;
            close(f1);
            found = 1;
        }
    }

    // Resolve the SOURCE device(s) to read the variant files from. BDM sources are DIFFERENTIATED by
    // driver: find EVERY mounted device whose driver matches the chosen type (USB / MX4SIO / internal
    // exFAT HDD) and read from its massN: FILESYSTEM root -- the same path the device pages browse. OPL
    // never mounts a typed ata0:/usb0:/mx4sio0: filesystem (those are block-device identities used only
    // for launch binding), so the readable source path is always massN:/. Searching ALL matching slots,
    // not just the first, covers a source family with two same-type devices when the variant files sit
    // on the second one. MMCE has its own mmce0:/mmce1: slots. Skipped when the adaptive pre-probe
    // above already found the pair.
    const char *cands[MAX_BDM_DEVICES];
    char bdmRoots[MAX_BDM_DEVICES][BDM_DEVICE_ROOT_MAX + 2];
    int nc = 0;
    if (found) {
        // Adaptive pre-probe already located the pair -- no transport force-loads needed.
    } else if (source == VCD_BDMA_SRC_MMCE) {
        // Ensure mmceman is loaded even when MMCE games are off / Manual-not-started -- otherwise mmce0:/
        // mmce1:/ are dead and nothing can be read. Then offer only slots that actually have a card, so
        // the not-found diagnostic is honest ("no device" vs "device found, files missing").
        mmceLoadModules();
        DIR *m0 = opendir("mmce0:/");
        if (m0 != NULL) {
            closedir(m0);
            cands[nc++] = "mmce0:/";
        }
        DIR *m1 = opendir("mmce1:/");
        if (m1 != NULL) {
            closedir(m1);
            cands[nc++] = "mmce1:/";
        }
    } else {
        int wantType = (source == VCD_BDMA_SRC_MX4SIO) ? BDM_TYPE_SDC : (source == VCD_BDMA_SRC_HDD) ? BDM_TYPE_ATA :
                                                                                                       BDM_TYPE_USB;
        // The source's transport driver may not be loaded if its device family is OFF for games (you can
        // keep the BDMA module files on a device you never browse). Force-load it + wait for the device
        // to mount -- otherwise the source path is dead and nothing can be read from it.
        bdmEnsureSourceModules(wantType, 2000);
        int slots[MAX_BDM_DEVICES];
        int ns = bdmGetDeviceSlotsByType(wantType, slots, MAX_BDM_DEVICES);
        for (int j = 0; j < ns && nc < (int)(sizeof(cands) / sizeof(cands[0])); j++) {
            snprintf(bdmRoots[nc], sizeof(bdmRoots[nc]), "mass%d:/", slots[j]);
            cands[nc] = bdmRoots[nc];
            nc++;
        }
    }

    for (int i = 0; i < nc && !found; i++) {
        snprintf(src0, sizeof(src0), "%s" POPS_FOLDER "/%s.%s", cands[i], vcdBdmaModule[0], suffix);
        snprintf(src1, sizeof(src1), "%s" POPS_FOLDER "/%s.%s", cands[i], vcdBdmaModule[1], suffix);
        int f0 = open(src0, O_RDONLY);
        LOG("[BDMA] probe %s -> %d\n", src0, f0);
        if (f0 < 0)
            continue;
        close(f0);
        int f1 = open(src1, O_RDONLY);
        LOG("[BDMA] probe %s -> %d\n", src1, f1);
        if (f1 < 0)
            continue;
        close(f1);
        found = 1;
        break;
    }
    // EMBEDDED FINAL FALLBACK (maintainer directive 2026-07-21, POPSLoader parity: "modules embedded
    // in the elf, pasted according to the VCD device"): when NO seek-path device carries the pair --
    // the common case on an MC boot, where nothing has a POPS folder -- unpack the gzipped
    // BDMAssault pair vendored in modules/bdmassault (PROVENANCE.md there). The seek order above is
    // unchanged, so user-supplied newer files still WIN; embedded only fills the void that used to be
    // a hard -4 "source files absent" failure.
    int useEmbedded = 0;
    if (!found) {
        LOG("[BDMA] %s.%s + %s.%s not found on any seek-path device (%s) -- using the embedded pair\n",
            vcdBdmaModule[0], suffix, vcdBdmaModule[1], suffix, nc ? cands[0] : "no matching device");
        useEmbedded = 1;
    }

    // Stage BOTH replacements before touching either live module. vcdSafeCopyFile removes a partial
    // destination on failure, which is safe for these private staging names but not for a live driver.
    // Staging guarantees both variant files were fully READ off the source device (the realistic torn-
    // pair cause is a flaky USB/MMCE source dying between file 1 and file 2) before the live pair moves.
    char tmp0[96], tmp1[96];
    snprintf(tmp0, sizeof(tmp0), "%s/%s.new", mcDir, vcdBdmaModule[0]);
    snprintf(tmp1, sizeof(tmp1), "%s/%s.new", mcDir, vcdBdmaModule[1]);
    unlink(tmp0);
    unlink(tmp1);

    int r;
    if (useEmbedded) {
        r = vcdStageEmbeddedPair(mode, tmp0, tmp1);
        if (r != 0) {
            unlink(tmp0);
            unlink(tmp1);
            if (diag != NULL && diagSize > 0)
                snprintf(diag, diagSize, "No %s BDMA files on any device, and the built-in pair could not be installed.", suffix);
            // Propagate the REAL failure class (Gemini review of #251): -2 card full / -3 IO keep
            // their specific toasts; only "no embedded pair / unpack failed" maps to -4 (source absent).
            return (r == -2 || r == -3) ? r : -4;
        }
    } else {
        r = vcdSafeCopyFile(src0, tmp0);
        if (r == 0)
            r = vcdSafeCopyFile(src1, tmp1);
        if (r != 0) {
            unlink(tmp0);
            unlink(tmp1);
            return r;
        }
    }

    // Commit by COPY, not rename(): this dir is always on mc0:/mc1:, and the stock mcman.irx OPL embeds
    // registers the legacy ioman 'mc' device with NO rename op -- iomanX returns -EUNSUP for every mc
    // rename(), so a rename-based swap can never succeed here. If a commit write fails, the CARD is
    // refusing IO: normalize to the consistent no-pair state (POPStarter falls back to its built-in
    // FAT32 driver, same as the VCD_BDMA_FAT32 path) rather than leave a torn mixed-variant pair.
    unlink(dst0); // free the old module's space first; tmp + old + new pairs may not fit a real MC
    r = vcdSafeCopyFile(tmp0, dst0);
    if (r == 0) {
        unlink(dst1);
        r = vcdSafeCopyFile(tmp1, dst1);
    }
    unlink(tmp0);
    unlink(tmp1);
    if (r != 0) {
        unlink(dst0); // drop the half-installed pair; vcdSafeCopyFile already removed its partial write
        unlink(dst1);
        vcdWriteBdmaMarker(mcDir, VCD_BDMA_FAT32);
        return r;
    }

    int mr = vcdWriteBdmaMarker(mcDir, mode);
    return (mr != 0) ? mr : 0;
}

// 1.0.1-style fast path for the launch equip: a card with NO marker but BOTH driver modules present is
// a pair the user (or an install predating the marker file) manages MANUALLY. vcdReadBdmaMode()
// collapses "marker absent" into VCD_BDMA_FAT32, so before this check every launch on such a card read
// as a MISMATCH and paid the FULL equip -- source-device module loads with bounded waits plus two module
// copies onto the memory card -- on EVERY VCD launch (NathanNeurotic: "unnecessary over work... extended
// wait on game launch... unnecessary MC transfer"). Trust the card and hand off. An EXPLICIT different
// marker (a real variant switch, checked by the caller before this) or a missing/partial pair still does
// the copy work -- ONCE -- after which the marker matches and every later launch takes the cheap path.
static int vcdBdmaManualPairPresent(void)
{
    char mcDir[64];
    char path[96];
    int fd, i;

    if (!vcdResolvePopstarterMc(mcDir, sizeof(mcDir)))
        return 0;
    snprintf(path, sizeof(path), "%s/%s", mcDir, VCD_BDMA_MARKER);
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        close(fd);
        return 0; // marker PRESENT -> its verdict stands (the caller already compared it)
    }
    for (i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/%s", mcDir, vcdBdmaModule[i]);
        fd = open(path, O_RDONLY);
        if (fd < 0)
            return 0; // pair incomplete -> a real equip is useful work
        close(fd);
    }
    return 1;
}

// Best-effort auto-equip of the device-matching BDMA driver before a VCD launch (POPSLoader's
// ApplyBdmaMode parity). POPSTARTER does its OWN SifIopReset, then reloads its block-device drivers
// from the FIXED memory-card files mc?:/POPSTARTER/usbd.irx + usbhdfsd.irx; equipping copies the
// device-matching BDMAssault variant pair there so those files fit the game's device. `source`/`mode`
// are the game device's BDMA family. Idempotent: skips the copy when the matching variant is already
// equipped.
//
// This is CARD PREP, not launch policy. The VCD launch itself is a simple handoff -- resolve
// POPSTARTER.ELF, hand it the argv[0] selector (XX. / SB. / bare), exec -- and POPSTARTER owns
// everything after that (maintainer contract, issues #56 review). So this helper NEVER blocks the
// launch: on any equip failure it keeps whatever pair is on the card (never unlink a working pair as
// collateral), shows a passing toast with the same diagnostic family the Settings-screen equip uses
// (so a failed prep is no longer a silent mystery), and the handoff proceeds regardless.
void vcdEnsureBdmaForLaunch(int source, int mode)
{
    char diag[160];

    if (!gBdmaApplyOnLaunch)
        return; // user opted to manage the BDMA driver manually (General Settings -> BDMA Source/Mode)
    if (mode <= VCD_BDMA_FAT32 || mode >= VCD_BDMA_MODE_COUNT)
        return; // FAT32 / invalid -> POPSTARTER's built-in driver, nothing to equip
    if (vcdReadBdmaMode() == mode)
        return; // the matching variant is already on the card
    if (vcdBdmaManualPairPresent()) {
        // No marker but a full pair on the card = manually managed; dumb 1.0.1-style handoff. LOG it
        // loudly (HW batch follow-up: this skip silently ate an expected ATA equip on a card left
        // over from manual testing) -- a Settings-screen equip writes the marker once and restores
        // auto-correction; deleting the stray pair does the same.
        LOG("[BDMA] unmarked manual pair on the card -- NOT equipping the %s variant (Settings equip once to take over)\n",
            vcdBdmaSuffix[mode]);
        return;
    }

    // Say what is happening. Everything above this point is a cheap decision that ends in "nothing
    // to do"; from here the launch can spend real time -- probing source devices (which may force-
    // load a transport and wait for a mount), copying two driver files, and writing the marker to
    // the card. Silence through that reads as a hung launch, and left the user unable to tell
    // whether the card had been written at all. One frame, no delay added: if this line appears the
    // card is being prepared, and if it never appears nothing needed equipping.
    guiRenderTextScreen(_l(_STR_BDMA_EQUIPPING));

    int er = vcdEquipBdma(source, mode, diag, sizeof(diag));
    if (er == 0)
        return;

    // Equip failed (-4 source files absent, -2 card full, -3 IO error, -1/other mid-copy). Inform in
    // passing -- toast the Settings-screen diagnostic family + LOG the full detail -- and launch anyway.
    LOG("VCD BDMA equip failed (%d: %s) -- launching as-is (card keeps its current driver pair)\n", er, diag);
    if (er == -4)
        guiWarning(_l(_STR_BDMA_ERR_SRC), 6);
    else if (er == -2)
        guiWarning(_l(_STR_BDMA_ERR_SPACE), 6);
    else
        guiWarning(_l(_STR_BDMA_ERR_IO), 6);
}

// Explicit USB mode application for the per-launch fat32/exFAT dialog (bdmLaunchVcd, USB devices).
// The user JUST picked this driver, so the choice applies even when gBdmaApplyOnLaunch is off (that
// setting only gates the AUTOMATIC equip). FAT32 is a real equip here: it removes the exFAT module
// pair so POPSTARTER falls back to its built-in USB stack. The unmarked-manual-pair guard still
// holds for BOTH choices -- a pair the user manages by hand (no marker file) is never deleted or
// replaced as collateral; the exFAT pair reads FAT32 too, so leaving it in place still boots.
// CARD PREP ONLY, same as vcdEnsureBdmaForLaunch: a failure toasts in passing, never blocks the handoff.
void vcdApplyUsbModeForLaunch(int mode)
{
    char diag[160];

    if (mode != VCD_BDMA_FAT32 && mode != VCD_BDMA_USBEXFAT)
        return; // the USB dialog only offers these two
    if (vcdReadBdmaMode() == mode)
        return; // the picked driver is already on the card
    if (vcdBdmaManualPairPresent()) {
        LOG("[BDMA] unmarked manual pair on the card -- NOT applying the %s pick (Settings equip once to take over)\n",
            vcdBdmaSuffix[mode]);
        return;
    }

    guiRenderTextScreen(_l(_STR_BDMA_EQUIPPING)); // same reasoning as vcdEnsureBdmaForLaunch above

    int er = vcdEquipBdma(VCD_BDMA_SRC_USB, mode, diag, sizeof(diag));
    if (er == 0)
        return;

    LOG("VCD USB-mode equip failed (%d: %s) -- launching as-is (card keeps its current driver pair)\n", er, diag);
    if (er == -4)
        guiWarning(_l(_STR_BDMA_ERR_SRC), 6);
    else if (er == -2)
        guiWarning(_l(_STR_BDMA_ERR_SPACE), 6);
    else
        guiWarning(_l(_STR_BDMA_ERR_IO), 6);
}

// ---- POPSTARTER memory-card externals -----------------------------------------------
// POPSTARTER reads its external modules/icons from mc?:/POPSTARTER/ after its own IOP reset. Keep
// the release copies directly beside the games in each device's POPS/ folder and install only files
// missing from the card. Existing card files are user-managed and always win. Network configs and
// RiptOPL's BDMA marker are deliberately excluded; their dedicated settings flows own those files.
static const char *vcdPopstarterMcFile[9] = {
    "smbman.irx", "ps2ip.irx", "ps2smap.irx", "ps2dev9.irx", "SMSUTILS.irx", "poweroff.irx", "del.icn", "list.icn", "icon.sys"};
static const char *vcdSmbModule[4] = {"smbman.irx", "ps2ip.irx", "ps2smap.irx", "ps2dev9.irx"};

int vcdInstallPopstarterMc(const char *devPrefix)
{
    char mcDir[64], src[320], dst[96];
    int firstError = 0;

    if (devPrefix == NULL || devPrefix[0] == '\0')
        return -1;
    if (!vcdResolvePopstarterMc(mcDir, sizeof(mcDir)))
        return -3;

    for (unsigned int i = 0; i < sizeof(vcdPopstarterMcFile) / sizeof(vcdPopstarterMcFile[0]); i++) {
        snprintf(dst, sizeof(dst), "%s/%s", mcDir, vcdPopstarterMcFile[i]);
        int fd = open(dst, O_RDONLY);
        if (fd < 0 && !strcmp(vcdPopstarterMcFile[i], "SMSUTILS.irx")) {
            char altDst[96];
            snprintf(altDst, sizeof(altDst), "%s/smsutils.irx", mcDir);
            fd = open(altDst, O_RDONLY);
        }
        if (fd >= 0) {
            close(fd);
            continue; // install-if-missing: never replace a user-managed card file
        }

        snprintf(src, sizeof(src), "%s%s%c%s", devPrefix, POPS_FOLDER, vcdSep(devPrefix), vcdPopstarterMcFile[i]);
        int result = vcdSafeCopyFile(src, dst);
        if (result == 0)
            LOG("[POPSTARTER] installed %s from %s\n", vcdPopstarterMcFile[i], src);
        else {
            LOG("[POPSTARTER] could not install %s from %s (%d)\n", vcdPopstarterMcFile[i], src, result);
            if (firstError == 0)
                firstError = result;
        }
    }

    return firstError;
}

int vcdSmbModulesPresent(void)
{
    static const char *cards[2] = {"mc0:/POPSTARTER", "mc1:/POPSTARTER"};
    for (int c = 0; c < 2; c++) {
        int all = 1;
        for (int i = 0; i < 4; i++) {
            char path[96];
            snprintf(path, sizeof(path), "%s/%s", cards[c], vcdSmbModule[i]);
            int fd = open(path, O_RDONLY);
            if (fd < 0) {
                all = 0;
                break;
            }
            close(fd);
        }
        if (all)
            return 1; // this card has the complete SMB stack
    }
    return 0;
}

// ---- POPStarter network files (IPCONFIG.DAT / SMBCONFIG.DAT) --------------------------------
// POPSLoader-parity flow (see include/vcdsupport.h for the formats). The central rule: read
// existing values when available, otherwise stay blank -- absence of POPStarter's files means
// "unknown/unconfigured", NEVER "use OPL's defaults". Candidate dirs, in POPStarter's
// precedence order: mc0:/POPSTARTER -> mc1:/POPSTARTER. That's it -- POPSTARTER reads its
// network files from the memory card ONLY (OPL's own settings live in the boot dir/cwd, but
// that is OUR convention, not POPSTARTER's). Nothing here touches VCD files.
static int vcdPopstarterNetDirs(char dirs[][96], int maxDirs)
{
    int n = 0;

    snprintf(dirs[n++], 96, "mc0:/POPSTARTER");
    if (n < maxDirs)
        snprintf(dirs[n++], 96, "mc1:/POPSTARTER");

    return n;
}

// Read a small text config file fully. Returns the length (>= 0), -1 when absent, -2 on a real
// IO error. Absence is data (unknown/unconfigured), not a failure.
static int vcdReadNetFile(const char *path, char *buf, int bufSize)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int size = lseek(fd, 0, SEEK_END);
    if (size < 0 || lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return -2;
    }
    if (size >= bufSize)
        size = bufSize - 1; // a bigger file is garbage/truncated, but never smashes the buffer
    int rd = read(fd, buf, size);
    close(fd);
    if (rd < 0)
        return -2;
    buf[rd] = '\0';
    return rd;
}

static int vcdQuadOk(const int q[4])
{
    for (int i = 0; i < 4; i++)
        if (q[i] < 0 || q[i] > 255)
            return 0;
    return 1;
}

// Parse "<IP> <NETMASK> <GATEWAY>". Returns 1 when a full valid triple was read; 0 means the
// file is blank/garbage -- the caller treats that as DHCP and shows NO invented values.
static int vcdParseIpConfig(const char *buf, vcd_popsnet_t *out)
{
    int n = sscanf(buf, "%d.%d.%d.%d %d.%d.%d.%d %d.%d.%d.%d",
                   &out->ps2Ip[0], &out->ps2Ip[1], &out->ps2Ip[2], &out->ps2Ip[3],
                   &out->ps2Mask[0], &out->ps2Mask[1], &out->ps2Mask[2], &out->ps2Mask[3],
                   &out->ps2Gw[0], &out->ps2Gw[1], &out->ps2Gw[2], &out->ps2Gw[3]);
    return n == 12 && vcdQuadOk(out->ps2Ip) && vcdQuadOk(out->ps2Mask) && vcdQuadOk(out->ps2Gw);
}

// Parse line 1 "<SERVER IP>[:PORT] <SHARE NAME>" + line 2 user + line 3 password. The share name
// may contain spaces, so only the FIRST token is the host; lines 2/3 are verbatim (missing =
// guest). Invalid/absent content simply leaves fields blank -- never invented.
static void vcdParseSmbConfig(const char *buf, vcd_popsnet_t *out)
{
    char work[256];
    snprintf(work, sizeof(work), "%s", buf);

    char *lines[3] = {NULL, NULL, NULL};
    int nlines = 0;
    char *p = work;
    while (nlines < 3 && p != NULL && *p != '\0') {
        lines[nlines++] = p;
        p = strchr(p, '\n');
        if (p != NULL)
            *p++ = '\0';
    }
    for (int i = 0; i < nlines; i++) {
        size_t l = strlen(lines[i]);
        while (l > 0 && (lines[i][l - 1] == '\r' || lines[i][l - 1] == ' ' || lines[i][l - 1] == '\t'))
            lines[i][--l] = '\0';
    }

    if (nlines > 0) {
        char host[32];
        const char *share = lines[0];
        while (*share == ' ' || *share == '\t')
            share++;
        const char *sp = share;
        while (*sp != '\0' && *sp != ' ' && *sp != '\t')
            sp++;
        size_t hostLen = (size_t)(sp - share);
        if (hostLen >= sizeof(host))
            hostLen = sizeof(host) - 1;
        memcpy(host, share, hostLen);
        host[hostLen] = '\0';
        while (*sp == ' ' || *sp == '\t')
            sp++;
        snprintf(out->smbShare, sizeof(out->smbShare), "%s", sp);

        char *colon = strchr(host, ':');
        int port = 0;
        if (colon != NULL) {
            *colon = '\0';
            port = atoi(colon + 1);
            if (port <= 0 || port > 65535)
                port = 0; // garbage port -> default, not a saved value
        }
        int q[4] = {0, 0, 0, 0};
        if (sscanf(host, "%d.%d.%d.%d", &q[0], &q[1], &q[2], &q[3]) == 4 && vcdQuadOk(q)) {
            memcpy(out->smbIp, q, sizeof(q));
            out->smbPort = port;
        }
    }
    if (nlines > 1)
        snprintf(out->smbUser, sizeof(out->smbUser), "%s", lines[1]);
    if (nlines > 2)
        snprintf(out->smbPass, sizeof(out->smbPass), "%s", lines[2]);
}

int vcdReadPopstarterNet(vcd_popsnet_t *out)
{
    if (out == NULL)
        return -3;
    memset(out, 0, sizeof(*out));
    out->ipDhcp = 1; // absent/blank IPCONFIG.DAT displays as DHCP, with no invented values

    char dirs[2][96];
    int ndirs = vcdPopstarterNetDirs(dirs, 2);
    char buf[256];

    for (int i = 0; i < ndirs && (!out->smbExists || !out->ipExists); i++) {
        char path[128];
        if (!out->smbExists) {
            snprintf(path, sizeof(path), "%s/SMBCONFIG.DAT", dirs[i]);
            int rd = vcdReadNetFile(path, buf, sizeof(buf));
            if (rd == -2)
                return -3;
            if (rd >= 0) {
                out->smbExists = 1;
                snprintf(out->smbDir, sizeof(out->smbDir), "%s", dirs[i]);
                vcdParseSmbConfig(buf, out);
            }
        }
        if (!out->ipExists) {
            snprintf(path, sizeof(path), "%s/IPCONFIG.DAT", dirs[i]);
            int rd = vcdReadNetFile(path, buf, sizeof(buf));
            if (rd == -2)
                return -3;
            if (rd >= 0) {
                out->ipExists = 1;
                snprintf(out->ipDir, sizeof(out->ipDir), "%s", dirs[i]);
                if (vcdParseIpConfig(buf, out))
                    out->ipDhcp = 0;
            }
        }
    }

    // Create-target for files that don't exist yet: the first candidate dir that EXISTS; else
    // mc0:/POPSTARTER, created best-effort at write time. Empty when no card is present at all --
    // creation then reports IO.
    for (int i = 0; i < ndirs; i++) {
        DIR *d = opendir(dirs[i]);
        if (d != NULL) {
            closedir(d);
            snprintf(out->createDir, sizeof(out->createDir), "%s", dirs[i]);
            break;
        }
    }
    if (out->createDir[0] == '\0' && ndirs > 0)
        snprintf(out->createDir, sizeof(out->createDir), "mc0:/POPSTARTER");

    return 0;
}

int vcdPopsNetChanged(const vcd_popsnet_t *orig, const vcd_popsnet_t *cur)
{
    int mask = 0;

    if (memcmp(orig->smbIp, cur->smbIp, sizeof(orig->smbIp)) != 0 ||
        orig->smbPort != cur->smbPort ||
        strcmp(orig->smbShare, cur->smbShare) != 0 ||
        strcmp(orig->smbUser, cur->smbUser) != 0 ||
        strcmp(orig->smbPass, cur->smbPass) != 0)
        mask |= 1;

    if (orig->ipDhcp != cur->ipDhcp ||
        (!cur->ipDhcp && (memcmp(orig->ps2Ip, cur->ps2Ip, sizeof(orig->ps2Ip)) != 0 ||
                          memcmp(orig->ps2Mask, cur->ps2Mask, sizeof(orig->ps2Mask)) != 0 ||
                          memcmp(orig->ps2Gw, cur->ps2Gw, sizeof(orig->ps2Gw)) != 0)))
        mask |= 2;

    return mask;
}

// Serialize IPCONFIG.DAT: the static triple, or EMPTY for DHCP -- the file must exist either way.
static int vcdBuildIpConfig(const vcd_popsnet_t *cfg, char *buf, int bufSize)
{
    if (cfg->ipDhcp)
        return 0;
    return snprintf(buf, bufSize, "%d.%d.%d.%d %d.%d.%d.%d %d.%d.%d.%d\n",
                    cfg->ps2Ip[0], cfg->ps2Ip[1], cfg->ps2Ip[2], cfg->ps2Ip[3],
                    cfg->ps2Mask[0], cfg->ps2Mask[1], cfg->ps2Mask[2], cfg->ps2Mask[3],
                    cfg->ps2Gw[0], cfg->ps2Gw[1], cfg->ps2Gw[2], cfg->ps2Gw[3]);
}

// Serialize SMBCONFIG.DAT. The whole file's byte shape lives in this ONE function: if the exact
// POPStarter/POPSLoader convention ever needs a tweak (CRLF, trailing newline), it is a one-line
// change here. Port 0/445 is written bare; lines 2/3 empty = guest.
static int vcdBuildSmbConfig(const vcd_popsnet_t *cfg, char *buf, int bufSize)
{
    char host[24];
    if (cfg->smbPort > 0 && cfg->smbPort != 445)
        snprintf(host, sizeof(host), "%d.%d.%d.%d:%d", cfg->smbIp[0], cfg->smbIp[1], cfg->smbIp[2], cfg->smbIp[3], cfg->smbPort);
    else
        snprintf(host, sizeof(host), "%d.%d.%d.%d", cfg->smbIp[0], cfg->smbIp[1], cfg->smbIp[2], cfg->smbIp[3]);
    return snprintf(buf, bufSize, "%s %s\n%s\n%s\n", host, cfg->smbShare, cfg->smbUser, cfg->smbPass);
}

int vcdWritePopstarterNetFiles(const vcd_popsnet_t *cfg, int writeSmb, int writeIp)
{
    if (cfg == NULL)
        return -3;
    char buf[256];

    if (writeSmb) {
        const char *dir = cfg->smbExists ? cfg->smbDir : cfg->createDir;
        if (dir[0] == '\0')
            return -3;
        if (!cfg->smbExists)
            mkdir(dir, 0777); // best-effort; exists already -> error, ignored
        char path[128];
        snprintf(path, sizeof(path), "%s/SMBCONFIG.DAT", dir);
        int len = vcdBuildSmbConfig(cfg, buf, sizeof(buf));
        int rc = vcdSafeWriteFile(path, buf, len);
        if (rc != 0)
            return rc;
    }

    if (writeIp) {
        const char *dir = cfg->ipExists ? cfg->ipDir : cfg->createDir;
        if (dir[0] == '\0')
            return -3;
        if (!cfg->ipExists)
            mkdir(dir, 0777);
        char path[128];
        snprintf(path, sizeof(path), "%s/IPCONFIG.DAT", dir);
        int len = vcdBuildIpConfig(cfg, buf, sizeof(buf));
        int rc = vcdSafeWriteFile(path, buf, len);
        if (rc != 0)
            return rc;
    }

    return 0;
}

void vcdPrepareRetroGemBarcode(const char *vcdPath)
{
    char gameID[RETROGEM_GAMEID_MAX];

    if (!gPopstarterRetroGemGameID || vcdPath == NULL || vcdPath[0] == '\0')
        return;

    if (retrogemGetVcdGameID(vcdPath, gameID, sizeof(gameID)) && gameID[0] != '\0') {
        displayRetroGemGameID(gameID, 2);
    }
}
