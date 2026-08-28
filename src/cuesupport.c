/*
  Copyright 2026, Open-PS2-Loader contributors
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.

  CUE (PS1-via-Ember) scan, path resolution and argument validation, plus the union that makes one
  PS1 list out of both cores' libraries. See include/cuesupport.h for the measured Ember contract
  this implements, and docs/EMBER-INTEGRATION-PLAN.md for its derivation.

  POSIX IO only -- the newlib port rejects direct fileXio use here, same rule as vcdsupport.c.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>  // errno/ENOENT in the absent-vs-contended split, mirroring vcdScanOpenDir
#include <malloc.h> // memalign -- the published list matches sbReadList's alignment

#include "include/opl.h"        // pulls <dirent.h> (opendir/readdir/DIR) + strcasecmp, like vcdsupport.c
#include "include/ioman.h"      // LOG
#include "include/textures.h"   // texDiscoverLoad + ERR_BAD_FILE (folder cover fallback)
#include "include/vcdsupport.h" // vcdFillGameList + vcdSortKey -- the POPSTARTER half of the union
#include "include/cuesupport.h"

// Path separator for a device prefix: '\\' for SMB (its prefix ends in a backslash), else '/'.
// Auto-detected from the trailing character so one code path serves mass/mmce/pfs and SMB alike.
// Same rule as vcdsupport.c's vcdSep(); kept local rather than shared so neither file's separator
// behaviour can be changed from under the other.
static char cueSep(const char *devPrefix)
{
    int n = (devPrefix != NULL) ? (int)strlen(devPrefix) : 0;
    return (n > 0 && devPrefix[n - 1] == '\\') ? '\\' : '/';
}

const char *cueEmberFolder(void)
{
    // A configurable folder name can arrive later as a setting. Resolving it through this accessor
    // from the start keeps that change to one line and keeps every caller honest about the fact
    // that the folder NAME is not part of Ember's contract -- the ember.elf's DIRECTORY is.
    return EMBER_FOLDER_DEFAULT;
}

// Shared body for the two file probes: compose "<devPrefix><folder><sep><file>" and open() it.
static int cueResolveEmberFile(const char *devPrefix, const char *fileName, char *out, int outSize)
{
    int fd;

    if (out == NULL || outSize <= 0 || devPrefix == NULL || fileName == NULL)
        return 0;

    snprintf(out, outSize, "%s%s%c%s", devPrefix, cueEmberFolder(), cueSep(devPrefix), fileName);

    fd = open(out, O_RDONLY);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

int cueResolveEmber(const char *devPrefix, char *out, int outSize)
{
    return cueResolveEmberFile(devPrefix, EMBER_ELF_NAME, out, outSize);
}

int cueResolveEmberBios(const char *devPrefix, char *out, int outSize)
{
    return cueResolveEmberFile(devPrefix, EMBER_BIOS_NAME, out, outSize);
}

void cueBuildGamesDir(const char *devPrefix, char *out, int outSize)
{
    if (out == NULL || outSize <= 0)
        return;
    if (devPrefix == NULL) {
        out[0] = '\0';
        return;
    }

    snprintf(out, outSize, "%s%s%c%s", devPrefix, cueEmberFolder(), cueSep(devPrefix), EMBER_GAMES_FOLDER);
}

int cueNameLaunchable(const char *name)
{
    if (name == NULL || name[0] == '\0')
        return 0;

    // Ember refuses any argument STARTING with ".." -- it compares buf[0] and buf[1] and never looks
    // further, so "..", "../x" and "..foo" are all refused. Match that, not the tidier "== ..".
    if (name[0] == '.' && name[1] == '.')
        return 0;

    // Scanner artefact: the current-directory entry is never a game.
    if (!strcmp(name, "."))
        return 0;

    // Ember's char scan refuses '/', ':' and '\\' ANYWHERE in the argument. On FAT/exFAT these
    // cannot appear in a directory entry, so in practice this fires only on a hand-typed argument
    // -- but it is the difference between a clear message and Ember dropping to the PS1 BIOS shell.
    if (strpbrk(name, "/:\\") != NULL)
        return 0;

    // Longer than Ember's own buffers can carry (see CUE_NAME_LAUNCH_MAX).
    if ((int)strlen(name) > CUE_NAME_LAUNCH_MAX)
        return 0;

    return 1;
}

// Is this directory entry a game folder? d_type is trusted only when it says DT_DIR: it is a
// documented liar on MMCE clones (the same finding that forced the theme discovery to opendir-probe),
// so anything else is probed rather than believed. On a well-formed FAT/exFAT games/ folder that
// costs nothing -- every entry answers DT_DIR on the first test -- and the probe is paid only for
// stray files, or on a device whose d_type cannot be trusted at all.
static int cueEntryIsDir(const char *gamesDir, const struct dirent *de)
{
    char probe[320];
    DIR *d;

#ifdef DT_DIR
    if (de->d_type == DT_DIR)
        return 1;
#endif

    snprintf(probe, sizeof(probe), "%s/%s", gamesDir, de->d_name);
    d = opendir(probe);
    if (d == NULL)
        return 0;
    closedir(d);
    return 1;
}

int cueScanDir(const char *devPrefix, cue_entry_t **outList)
{
    char gamesDir[288];
    cue_entry_t *list;
    struct dirent *de;
    DIR *dir;
    int count = 0;

    if (outList == NULL)
        return 0;
    *outList = NULL;
    if (devPrefix == NULL)
        return 0;

    cueBuildGamesDir(devPrefix, gamesDir, sizeof(gamesDir));
    if (gamesDir[0] == '\0')
        return 0;

    errno = 0; // clear BEFORE opendir so the NULL branch reads THIS call's errno, not a stale one
    dir = opendir(gamesDir);
    if (dir == NULL) {
        // Absent-vs-contended split, identical in spirit to vcdScanOpenDir. A device with a POPS
        // folder and no EMBER folder is the ordinary case and MUST report 0 ("readable, nothing
        // here"), not a failure -- ps1FillGameList treats a failure from either half as a reason to
        // keep the whole last-good list, so getting this wrong would freeze the PS1 page of every
        // device that only uses one core.
        if (errno == ENOENT)
            return 0;
        // Anything else: the directory could not be READ (bus contended, device mid-detach). Signal
        // failure so the caller preserves its last-good list rather than blanking the page.
        // MMCE caveat, same as the VCD scan: mmceman's dopen collapses every failure into a bare -1
        // (EE sees EPERM, not ENOENT) unless the paired mmceman patch is in the build, so on mmceN:
        // an absent EMBER folder lands here. That costs a preserved list, never a wrong one.
        LOG("[CUE] cannot read '%s' (errno %d)\n", gamesDir, errno);
        return -1;
    }

    list = (cue_entry_t *)calloc(CUE_MAX_ITEMS, sizeof(cue_entry_t));
    if (list == NULL) {
        closedir(dir);
        return -1; // OOM: preserve the caller's current list rather than blank it
    }

    while (count < CUE_MAX_ITEMS && (de = readdir(dir)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        // Refuse here what Ember would refuse at launch, so the list never offers a row whose X
        // button cannot work. A name with a separator cannot occur on FAT/exFAT; an over-long one
        // is a user mistake worth a log line rather than a silent dead row.
        if (!cueNameLaunchable(de->d_name)) {
            LOG("[CUE] skip (unlaunchable name): %s\n", de->d_name);
            continue;
        }
        // The row store caps names at ISO_GAME_NAME_MAX; a longer one would be truncated and then
        // resolve to a folder that does not exist.
        if ((int)strlen(de->d_name) > ISO_GAME_NAME_MAX) {
            LOG("[CUE] skip (name > %d chars): %s\n", ISO_GAME_NAME_MAX, de->d_name);
            continue;
        }
        if (!cueEntryIsDir(gamesDir, de))
            continue; // loose files in games/ (a stray readme, a leftover archive) are not games

        snprintf(list[count].name, sizeof(list[count].name), "%s", de->d_name);
        count++;
    }
    closedir(dir);
    LOG("[CUE] scanned '%s': found %d entries\n", gamesDir, count);

    if (count == 0) {
        free(list);
        return 0;
    }

    *outList = list;
    return count;
}

int cueIsCueEntry(const base_game_info_t *game)
{
    return (game != NULL && strcasecmp(game->extension, CUE_ROW_EXTENSION) == 0);
}

int cueRowIsCueByName(const base_game_info_t *games, int count, const char *name)
{
    int i;

    if (games == NULL || name == NULL)
        return -1;

    for (i = 0; i < count; i++) {
        if (strcmp(games[i].name, name) == 0)
            return cueIsCueEntry(&games[i]) ? 1 : 0;
    }
    return -1;
}

// Merged-list comparator. Sorts by the same visible key the VCD-only scan always used, so turning
// on Ember cannot reorder anyone's existing PS1 list. The extension tie-break is not cosmetic: two
// rows CAN share a display name (the same game held for both cores is the expected case), and
// without a deterministic tie-break qsort would be free to swap them between rescans, making rows
// appear to jump around on every refresh.
static int ps1RowCmp(const void *a, const void *b)
{
    const base_game_info_t *ga = (const base_game_info_t *)a;
    const base_game_info_t *gb = (const base_game_info_t *)b;
    int r = strcasecmp(vcdSortKey(ga->name), vcdSortKey(gb->name));

    if (r != 0)
        return r;
    return strcasecmp(ga->extension, gb->extension);
}

// Fill a list from EMBER/games/ alone. Static: every caller wants the merged PS1 list, and exposing
// a "just the Ember half" entry point would invite a second place where the union is formed.
static int cueFillGameList(const char *devPrefix, base_game_info_t **outGames)
{
    cue_entry_t *entries = NULL;
    base_game_info_t *games;
    int n, i;

    if (outGames == NULL)
        return 0;

    n = cueScanDir(devPrefix, &entries);
    if (n < 0)
        return -1; // could not read the device -> caller preserves its list
    if (n == 0) {
        free(entries);
        *outGames = NULL;
        return 0;
    }

    games = (base_game_info_t *)memalign(64, n * sizeof(base_game_info_t));
    if (games == NULL) {
        free(entries);
        return -1;
    }
    memset(games, 0, n * sizeof(base_game_info_t));

    for (i = 0; i < n; i++) {
        // IDENTITY is the folder name, and it stays the folder name: it is Ember's launch argument,
        // and art / per-game CFG / favourites all key off it. Exactly the discipline the VCD rows
        // use with their filename.
        snprintf(games[i].name, sizeof(games[i].name), "%s", entries[i].name);
        snprintf(games[i].startup, sizeof(games[i].startup), "%s", entries[i].name);
        snprintf(games[i].extension, sizeof(games[i].extension), "%s", CUE_ROW_EXTENSION);
        games[i].parts = 1;
        games[i].format = GAME_FORMAT_ISO; // harmless; the row's extension gates the launch path
    }

    free(entries);
    *outGames = games;
    return n;
}

int ps1FillGameList(const char *devPrefix, base_game_info_t **outGames)
{
    base_game_info_t *vcdGames = NULL, *cueGames = NULL, *merged = NULL;
    int vcdCount, cueCount, total;

    if (outGames == NULL)
        return 0;

    vcdCount = vcdFillGameList(devPrefix, &vcdGames);
    cueCount = cueFillGameList(devPrefix, &cueGames);

    // EITHER half failing means the device could not be READ, not that it holds no games -- an
    // absent POPS or EMBER folder reports 0. Publishing a half list would look exactly like the
    // user's titles disappearing, so keep the caller's last-good list instead.
    if (vcdCount < 0 || cueCount < 0) {
        free(vcdGames);
        free(cueGames);
        LOG("[PS1] scan failed (vcd=%d cue=%d) -- keeping last-good list\n", vcdCount, cueCount);
        return -1;
    }

    total = vcdCount + cueCount;

    // Both scans reached the device: only NOW is it safe to replace the old list.
    free(*outGames);
    *outGames = NULL;

    if (total == 0) {
        free(vcdGames);
        free(cueGames);
        return 0;
    }

    merged = (base_game_info_t *)memalign(64, total * sizeof(base_game_info_t));
    if (merged == NULL) {
        free(vcdGames);
        free(cueGames);
        return 0; // list already released above; an empty page beats a dangling one
    }

    if (vcdCount > 0)
        memcpy(merged, vcdGames, vcdCount * sizeof(base_game_info_t));
    if (cueCount > 0)
        memcpy(merged + vcdCount, cueGames, cueCount * sizeof(base_game_info_t));

    free(vcdGames);
    free(cueGames);

    // Gated on the same Automatic Sorting switch every other list honours. With it off the halves
    // stay concatenated in scan order, which is the "raw directory order" the setting promises.
    if (gAutosort && total > 1)
        qsort(merged, total, sizeof(base_game_info_t), &ps1RowCmp);

    *outGames = merged;
    return total;
}

int cueLoadFolderCover(const char *devPrefix, const char *value, const char *suffix, GSTEXTURE *resultTex)
{
    char path[320];
    char gamesDir[288];
    int r;

    // Cover/icon only, matching vcdLoadPopsCover: the other art suffixes have no in-folder
    // convention and probing for them would be pure IO on the miss path.
    if (devPrefix == NULL || value == NULL || suffix == NULL)
        return ERR_BAD_FILE;
    if (strcmp(suffix, "COV") != 0 && strcmp(suffix, "ICO") != 0)
        return ERR_BAD_FILE;

    cueBuildGamesDir(devPrefix, gamesDir, sizeof(gamesDir));
    if (gamesDir[0] == '\0')
        return ERR_BAD_FILE;

    // "<games>/<name>/cover" first: one obvious filename a user can drop in without knowing the
    // title's exact spelling. texDiscoverLoad appends the extension it supports.
    snprintf(path, sizeof(path), "%s/%s/cover", gamesDir, value);
    r = texDiscoverLoad(resultTex, path, -1);
    if (r >= 0)
        return r;

    // Then "<games>/<name>/<name>", the POPSLoader-style suffixless peer of vcdLoadPopsCover.
    snprintf(path, sizeof(path), "%s/%s/%s", gamesDir, value, value);
    return texDiscoverLoad(resultTex, path, -1);
}
