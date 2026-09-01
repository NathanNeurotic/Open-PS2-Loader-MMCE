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

// Is this directory entry a game folder? Settled by opening it, never by d_type.
//
// The previous version of this comment argued that d_type is "a documented liar on MMCE clones" and
// then trusted it whenever it said DT_DIR. That is the unsafe direction of the same claim: a liar
// answering DT_DIR about a FILE is precisely the failure mode, and it put stray files in the PS1
// list with an X button that could not work. A liar is not half-trustworthy.
static int cueEntryIsDir(const char *devPrefix, const char *gamesDir, const struct dirent *de)
{
    char probe[320];
    DIR *d;

    // ALWAYS PROBE. There used to be a `d_type == DT_DIR` fast path here that returned 1 without
    // checking anything, and it is the reason stray files showed up as games in the PS1 list.
    //
    // d_type is not dependable across this project's drivers -- the MMCE theme scan already learned
    // that the hard way and opendir-probes for exactly this reason (mmceman on some clones reports
    // a d_type that does not describe the entry). A driver that mislabels a FILE as DT_DIR got that
    // file listed as an Ember game, with an X button that could never work.
    //
    // The fallback was always here; it was just unreachable whenever d_type lied. Trusting the
    // probe alone costs one opendir per entry in EMBER/games/, which is the same call the old
    // fallback made and is bounded by the number of game folders.
    //
    // This does NOT change what an Ember game IS. A game is still a FOLDER under EMBER/games/ --
    // that is Ember's own contract and it stays. All this decides is whether the thing we are
    // looking at really is a folder.
    // Separator comes from the DEVICE, not a hardcoded slash. cueBuildGamesDir already builds
    // gamesDir with cueSep(), so a prefix ending in a backslash would otherwise be probed as
    // "smb0:\\EMBER\\games/NAME" -- mixed, and rejected by any handler that cares. Latent today
    // (no backslash device lists Ember yet) and load-bearing the moment one does.
    snprintf(probe, sizeof(probe), "%s%c%s", gamesDir, cueSep(devPrefix), de->d_name);
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

    // NO EMBER CORE, NO EMBER LIBRARY -- and settle that with an open(), not with opendir's errno.
    //
    // The absent-vs-contended split below reads errno to tell "this folder isn't here" (fine, 0 rows)
    // from "this device wouldn't answer" (a failure the caller must not mistake for emptiness). That
    // works only on drivers that actually report ENOENT, and this very file documents one that does
    // not: mmceman collapses every failure, including the card's own not-found reply, into a bare -1.
    // There was never any reason to assume other block drivers are better behaved.
    //
    // The cost of getting it wrong is severe and was reported from hardware (FifthFox, iLink): a
    // device with a POPS library and NO EMBER folder returned -1 from this scan, ps1FillGameList
    // treated that as "device unreadable", and the ENTIRE PS1 list -- including the POPSTARTER
    // titles that had scanned perfectly -- stayed empty. USB and APA were fine because their driver
    // does report ENOENT.
    //
    // A plain open() of ember.elf has none of that ambiguity. If the core is not readable there is
    // no Ember library to list, whatever the reason: we could not launch a row from it either.
    // gamesDir is scratch for the probe here -- cueResolveEmber writes the ELF path into it and we
    // discard that, then rebuild it as the games directory below. One buffer, not two, on a stack
    // this platform keeps small.
    if (!cueResolveEmber(devPrefix, gamesDir, sizeof(gamesDir)))
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
        if (!cueEntryIsDir(devPrefix, gamesDir, de))
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

// The extensions Ember's io_find_disc accepts, in ITS priority order (.cue beats .exe beats .bin).
// We only test presence, so the order is documentation rather than logic -- but it is the reason a
// folder holding just a .bin is still a valid game.
static const char *const cueDiscExts[] = {".cue", ".exe", ".bin"};

void cueApplyDisplaySetting(const char *devPrefix)
{
    char path[320];
    char before[512];
    char after[512];
    const char *want;
    int fd, len = 0, out = 0, i, lineStart;

    if (devPrefix == NULL)
        return;
    want = (gEmberDisplay == EMBER_DISPLAY_480) ? "display:480" : "display:240";

    snprintf(path, sizeof(path), "%s%s%c%s", devPrefix, cueEmberFolder(), cueSep(devPrefix),
             EMBER_SETTINGS_NAME);

    // Read whatever is there. An absent file is the normal first-run case, not an error: on 240/480
    // we create it below, and on Default there is nothing to clear and nothing to create.
    fd = open(path, O_RDONLY);
    if (fd < 0 && gEmberDisplay == EMBER_DISPLAY_LEAVE)
        return;
    if (fd >= 0) {
        len = read(fd, before, sizeof(before) - 1);
        close(fd);
        if (len < 0)
            len = 0;
        // A read that FILLED the buffer means the file may continue past what we hold. Rewriting
        // from that prefix would O_TRUNC away everything beyond it -- silently deleting settings
        // that belong to the user or to a future Ember, which is the exact opposite of this
        // function's promise to preserve them. Leave the file completely alone instead.
        //
        // An Ember settings.txt is a couple of short lines, so this is a pathological case rather
        // than a real one; a file that big is a reason to keep our hands off it, not to grow a
        // buffer. Refusing an exactly-full read costs nothing but the display key this launch.
        if (len == (int)sizeof(before) - 1) {
            LOG("[CUE] %s is larger than we can safely rewrite -- left untouched\n", path);
            return;
        }
    }
    before[len] = 0;

    // Copy every line EXCEPT an existing display: line, then append ours. Keeping the other lines
    // is the point: Ember ignores unknown keys, so anything else in here belongs to the user or to
    // a future Ember, and neither is ours to delete.
    for (i = 0, lineStart = 0; i <= len; i++) {
        if (i != len && before[i] != 0x0A)
            continue;
        int lineLen = i - lineStart;
        if (lineLen > 0 && before[lineStart + lineLen - 1] == 0x0D)
            lineLen--; // tolerate CRLF, which a PC-side editor will leave behind
        if (lineLen > 0 && strncasecmp(&before[lineStart], "display:", 8) != 0) {
            if (out + lineLen + 1 >= (int)sizeof(after))
                break; // pathological file: keep what fits rather than truncate mid-line
            memcpy(&after[out], &before[lineStart], lineLen);
            out += lineLen;
            after[out++] = 0x0A;
        }
        lineStart = i + 1;
    }
    // snprintf returns what it WOULD have written, so adding it blindly can push `out` PAST the end
    // of the buffer and make the write() below over-read into whatever follows on the stack -- and
    // this is a path that writes to the user's memory card or USB stick. If our own line does not
    // fit, leave the file completely alone: a settings.txt we mangled is worse than one we never
    // touched, and Ember runs fine without the key.
    //
    // On Default we append NOTHING, so `out` now holds the file with our key stripped out. That is
    // what makes Default mean default: setting 240p and then changing back would otherwise leave
    // display:240 on the device forever, with the menu claiming Default while Ember still ran 240p.
    if (gEmberDisplay != EMBER_DISPLAY_LEAVE) {
        int n = snprintf(&after[out], sizeof(after) - out, "%s\n", want);
        if (n < 0 || n >= (int)sizeof(after) - out) {
            LOG("[CUE] no room for the display line in %s -- left untouched\n", path);
            return;
        }
        out += n;
    }

    // Nothing left to write. The file existed only to carry the key we just cleared, so remove it
    // rather than leave an empty one behind -- settings.txt is optional, and Default means there
    // should not be one. Only ever reached on Default: every other value appended a line above.
    //
    // This deletes a file, so it is deliberately narrow. It cannot fire while ANY other line
    // survived the copy (a comment, a key we do not know, a key a future Ember adds) -- those all
    // count toward `out` and are rewritten instead. len > 0 keeps us from unlinking a file that was
    // already empty when we found it, which would be deleting something we never wrote to.
    if (out == 0) {
        if (len > 0) {
            if (unlink(path) == 0)
                LOG("[CUE] nothing left to keep in %s -- removed for Default\n", path);
            else
                LOG("[CUE] cannot remove %s -- Ember keeps its previous display mode\n", path);
        }
        return;
    }

    // Only touch the device when the content actually changes. This runs on every Ember launch, and
    // a needless write is a needless card/stick write.
    if (out == len && memcmp(after, before, out) == 0)
        return;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        LOG("[CUE] cannot write %s -- launching without applying the display mode\n", path);
        return;
    }
    if (write(fd, after, out) != out) {
        close(fd);
        // O_TRUNC already emptied the file, so a failed or short write leaves it truncated with the
        // user's other keys and comments gone -- the one outcome this whole function exists to
        // avoid. We still hold the original bytes in `before` (the copy loop only ever read from
        // it), so put them back.
        //
        // This is best-effort, not atomic: the restore can fail too. It is worth doing anyway
        // because the realistic cause is a full card, and the truncate above just freed at least as
        // many bytes as we are writing back. A temp-file-and-rename would be atomic but relies on
        // rename() behaving across mass:/mmce:, which it does not do dependably here.
        if (len > 0) {
            fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd >= 0) {
                int back = write(fd, before, len);
                close(fd);
                (void)back; // read only by LOG, which compiles to nothing in a release build
                LOG("[CUE] write failed on %s -- original %s\n", path,
                    (back == len) ? "restored" : "COULD NOT be restored");
                return;
            }
        }
        LOG("[CUE] write failed on %s\n", path);
        return;
    }
    close(fd);
}

int cueGameHasImage(const char *devPrefix, const char *name)
{
    char gamesDir[288];
    char gameDir[320];
    struct dirent *de;
    DIR *dir;
    int found = 0;
    unsigned int i;

    if (devPrefix == NULL || name == NULL || name[0] == '\0')
        return 0;

    cueBuildGamesDir(devPrefix, gamesDir, sizeof(gamesDir));
    if (gamesDir[0] == '\0')
        return 0;
    snprintf(gameDir, sizeof(gameDir), "%s%c%s", gamesDir, cueSep(devPrefix), name);

    dir = opendir(gameDir);
    if (dir == NULL) {
        // The probe itself failed -- the folder may be there and merely unreadable this instant.
        // Never let a failed PROBE block a launch: report "has an image" and let Ember be the judge.
        LOG("[CUE] cannot probe '%s' -- launching anyway\n", gameDir);
        return 1;
    }

    while (!found && (de = readdir(dir)) != NULL) {
        int len = (int)strlen(de->d_name);
        if (len < 5)
            continue; // shortest possible match is "x.cue"
        for (i = 0; i < sizeof(cueDiscExts) / sizeof(cueDiscExts[0]); i++) {
            if (strcasecmp(de->d_name + len - 4, cueDiscExts[i]) == 0) {
                found = 1;
                break;
            }
        }
    }
    closedir(dir);

    if (!found)
        LOG("[CUE] '%s' holds no .cue/.bin/.exe\n", gameDir);
    return found;
}

int cueRenameGame(const char *devPrefix, const char *oldName, const char *newName)
{
    char gamesDir[288];
    char oldPath[320];
    char newPath[320];
    DIR *probe;

    // The NEW name has to satisfy Ember's argument rules, or the rename would succeed on disk and
    // leave behind a row that can never be launched.
    if (devPrefix == NULL || oldName == NULL || newName == NULL)
        return -1;
    if (!cueNameLaunchable(oldName) || !cueNameLaunchable(newName))
        return -1;
    if (!strcmp(oldName, newName))
        return 0;

    cueBuildGamesDir(devPrefix, gamesDir, sizeof(gamesDir));
    if (gamesDir[0] == '\0')
        return -1;
    if (snprintf(oldPath, sizeof(oldPath), "%s%c%s", gamesDir, cueSep(devPrefix), oldName) >= (int)sizeof(oldPath))
        return -1;
    if (snprintf(newPath, sizeof(newPath), "%s%c%s", gamesDir, cueSep(devPrefix), newName) >= (int)sizeof(newPath))
        return -1;

    // Refuse to clobber an existing folder. rename() over a non-empty directory is not portable
    // across the filesystems here, and silently merging two libraries would be worse than refusing.
    probe = opendir(newPath);
    if (probe != NULL) {
        closedir(probe);
        LOG("[CUE] rename refused, target exists: %s\n", newPath);
        return -1;
    }

    if (rename(oldPath, newPath) != 0) {
        LOG("[CUE] rename failed: %s -> %s (%d)\n", oldPath, newPath, errno);
        return -1;
    }
    return 0;
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

// Build the Ember half without sorting it yet. The merged PS1 path sorts the final union once;
// standalone UDPFS/UDPBD callers go through cueFillGameList below, which sorts this list directly.
static int cueFillGameListUnsorted(const char *devPrefix, base_game_info_t **outGames)
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
        free(*outGames);
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
    // The replacement is complete: only now release the published last-good list. On allocation or
    // scan failure above it remains untouched for the caller to keep displaying.
    free(*outGames);
    *outGames = games;
    return n;
}

int cueFillGameList(const char *devPrefix, base_game_info_t **outGames)
{
    int count = cueFillGameListUnsorted(devPrefix, outGames);

    if (count > 1 && gAutosort)
        qsort(*outGames, count, sizeof(base_game_info_t), &ps1RowCmp);
    return count;
}

int ps1FillGameList(const char *devPrefix, base_game_info_t **outGames)
{
    base_game_info_t *vcdGames = NULL, *cueGames = NULL, *merged = NULL;
    int vcdCount, cueCount, total;

    if (outGames == NULL)
        return 0;

    vcdCount = vcdFillGameList(devPrefix, &vcdGames);
    cueCount = cueFillGameListUnsorted(devPrefix, &cueGames);

    // ONLY BOTH halves failing means the device could not be read. This used to fail the whole scan
    // when EITHER half did, on the reasoning that publishing half a list looks like a user's titles
    // disappearing. That reasoning was wrong in the direction that matters: one half failing then
    // hid the OTHER half too, so a device holding a perfectly readable POPS library showed NOTHING
    // because its (absent) Ember half reported a failure. Hardware-reported on iLink.
    //
    // Half a list beats no list. A half that failed contributes no rows this pass and is retried on
    // the next refresh; the half that succeeded is published, because it is real.
    if (vcdCount < 0 && cueCount < 0) {
        free(vcdGames);
        free(cueGames);
        LOG("[PS1] both scans failed -- keeping last-good list\n");
        return -1;
    }
    if (vcdCount < 0) {
        LOG("[PS1] POPS scan failed -- publishing the Ember half alone this pass\n");
        free(vcdGames);
        vcdGames = NULL;
        vcdCount = 0;
    }
    if (cueCount < 0) {
        LOG("[PS1] EMBER scan failed -- publishing the POPSTARTER half alone this pass\n");
        free(cueGames);
        cueGames = NULL;
        cueCount = 0;
    }

    total = vcdCount + cueCount;

    if (total == 0) {
        // Both scans reached the device and it genuinely holds no PS1 titles: publishing empty is
        // the correct answer, so release the old list here.
        free(*outGames);
        *outGames = NULL;
        free(vcdGames);
        free(cueGames);
        return 0;
    }

    // ALLOCATE BEFORE RELEASING. Freeing the published list first and then failing to allocate its
    // replacement turns a transient out-of-memory into a permanently blank PS1 page -- the caller is
    // told 0 ("readable, nothing here") and has nothing left to fall back on. Failing to allocate is
    // exactly the case where the last-good list is most worth keeping, so report it like any other
    // scan failure and leave *outGames untouched.
    merged = (base_game_info_t *)memalign(64, total * sizeof(base_game_info_t));
    if (merged == NULL) {
        free(vcdGames);
        free(cueGames);
        LOG("[PS1] merge alloc failed (%d rows) -- keeping last-good list\n", total);
        return -1;
    }

    // The replacement exists: only NOW is it safe to drop the old one.
    free(*outGames);
    *outGames = NULL;

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
