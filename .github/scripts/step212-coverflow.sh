#!/usr/bin/env bash
set -euo pipefail

prod_branch="rebuild/step-212-apa-boot-and-bgm-resilience"
expected_head="1908aa0cdc9a43902a355327be0b106b2f6d70c0"

git fetch origin "$prod_branch"
actual_head="$(git rev-parse "origin/$prod_branch")"
if [[ "$actual_head" != "$expected_head" ]]; then
    echo "Refusing production edit: expected $expected_head, got $actual_head" >&2
    exit 1
fi

git checkout -B "$prod_branch" "origin/$prod_branch"

python3 - <<'PY'
from pathlib import Path


def load(path):
    p = Path(path)
    raw = p.read_bytes()
    crlf = b"\r\n" in raw
    return p, raw.decode("utf-8").replace("\r\n", "\n"), crlf


def save(p, text, crlf):
    if crlf:
        text = text.replace("\n", "\r\n")
    p.write_bytes(text.encode("utf-8"))


def replace_between(path, start, end, replacement):
    p, text, crlf = load(path)
    a = text.find(start)
    if a < 0:
        raise SystemExit(f"{path}: start marker not found")
    b = text.find(end, a)
    if b < 0:
        raise SystemExit(f"{path}: end marker not found")
    text = text[:a] + replacement + text[b:]
    save(p, text, crlf)


def replace_once(path, old, new):
    p, text, crlf = load(path)
    if text.count(old) != 1:
        raise SystemExit(f"{path}: expected one match, got {text.count(old)}")
    save(p, text.replace(old, new, 1), crlf)

hdl_func = r'''int hddGetHDLGamelist(hdl_games_list_t *game_list)
{
    struct GameDataEntry *head = NULL, *current = NULL, *next, *pGameEntry;
    unsigned int count = 0, i;
    iox_dirent_t dirent;
    int fd, ret = 0, readResult = 0;

    hddFreeHDLGamelist(game_list);

    fd = fileXioDopen("hdd0:");
    if (fd < 0)
        return fd;

    while ((readResult = fileXioDread(fd, &dirent)) > 0) {
        if (dirent.stat.mode != HDL_FS_MAGIC)
            continue;

        pGameEntry = GetGameListRecord(head, dirent.name);
        if (pGameEntry == NULL) {
            struct GameDataEntry *newEntry = malloc(sizeof(struct GameDataEntry));
            if (newEntry == NULL) {
                ret = -ENOMEM;
                break;
            }

            if (head == NULL)
                head = newEntry;
            else
                current->next = newEntry;
            current = newEntry;

            strncpy(current->id, dirent.name, APA_IDMAX);
            current->id[APA_IDMAX] = '\0';
            current->next = NULL;
            current->size = 0;
            current->lba = 0;
            count++;
            pGameEntry = current;
        }

        if (!(dirent.stat.attr & APA_FLAG_SUB)) {
            // Note: The APA specification states that there is a 4KB area used for storing the
            // partition's information, before the extended attribute area.
            pGameEntry->lba = dirent.stat.private_5 + (HDL_GAME_DATA_OFFSET + 4096) / 512;
        }

        pGameEntry->size += (dirent.stat.size / 4); // HDD sectors * (512 / 2048) = 0.25x
    }

    fileXioDclose(fd);

    // A directory read error is not "end of APA table". Returning success here used to publish a
    // partial list and made games disappear after a refresh. The caller now builds into a candidate
    // list, so propagating the failure preserves the last-good live array.
    if (ret == 0 && readResult < 0)
        ret = readResult;

    if (ret == 0 && head != NULL) {
        game_list->games = malloc(sizeof(hdl_game_info_t) * count);
        if (game_list->games != NULL) {
            memset(game_list->games, 0, sizeof(hdl_game_info_t) * count);

            for (i = 0, current = head; i < count; i++, current = current->next) {
                if ((ret = hddGetHDLGameInfo(current, &game_list->games[i])) != 0)
                    break;
            }

            if (ret != 0) {
                free(game_list->games);
                game_list->games = NULL;
            } else {
                game_list->count = count;
            }
        } else {
            ret = -ENOMEM;
        }
    }

    for (current = head; current != NULL; current = next) {
        next = current->next;
        free(current);
    }

    return ret;
}

//-------------------------------------------------------------------------
'''
replace_between("src/hdd.c", "int hddGetHDLGamelist(hdl_games_list_t *game_list)\n", "void hddFreeHDLGamelist", hdl_func)

pops_func = r'''int hddGetPopsPartitionList(hdd_pops_list_t *list)
{
    iox_dirent_t dirent;
    int fd, i, count = 0, readResult = 0, ret = 0;
    char(*names)[APA_IDMAX + 1] = NULL;

    list->count = 0;
    list->names = NULL;

    fd = fileXioDopen("hdd0:");
    if (fd < 0)
        return fd; // distinguish an enumeration failure from a valid zero-candidate disk

    while ((readResult = fileXioDread(fd, &dirent)) > 0) {
        if (dirent.stat.attr != HDD_APA_ATTR_MAIN_PARTITION || dirent.stat.mode != HDD_APA_FS_TYPE_PFS)
            continue; // skip APA sub-partitions and HDL/raw/system formats
        if (!hddIsPopsContainerName(dirent.name) && !hddIsPopsPartitionGame(dirent.name))
            continue; // not an HDD-resident PS1/VCD source

        int dup = 0;
        for (i = 0; i < count; i++) {
            if (!strncmp(names[i], dirent.name, APA_IDMAX)) {
                dup = 1;
                break;
            }
        }
        if (dup)
            continue;

        char(*grown)[APA_IDMAX + 1] = realloc(names, (count + 1) * sizeof(*names));
        if (grown == NULL) {
            ret = -ENOMEM;
            break;
        }
        names = grown;
        strncpy(names[count], dirent.name, APA_IDMAX);
        names[count][APA_IDMAX] = '\0';
        count++;
    }
    fileXioDclose(fd);

    if (ret == 0 && readResult < 0)
        ret = readResult;
    if (ret < 0) {
        free(names);
        return ret; // never publish a partial APA partition walk as a complete candidate set
    }

    if (count == 0) {
        free(names);
        return 0; // successful enumeration, no POPS candidates
    }

    qsort(names, count, sizeof(*names), hddPopsNameCompare);

    list->names = names;
    list->count = count;
    return count;
}

//-------------------------------------------------------------------------
'''
replace_between("src/hdd.c", "int hddGetPopsPartitionList(hdd_pops_list_t *list)\n", "void hddFreePopsPartitionList", pops_func)

replace_once(
    "src/hddsupport.c",
    '''// (not hddVcdGames != NULL) so a drive whose candidates scanned to ZERO VCDs is also remembered. The\n// no-candidates early return deliberately does NOT latch: hddGetPopsPartitionList returns 0 for a\n// transient hdd0: dopen failure too, and that walk is one mount-free APA pass -- cheap to repeat.\n''',
    '''// (not hddVcdGames != NULL) so a drive whose candidates scanned to ZERO VCDs is also remembered.\n// hddGetPopsPartitionList distinguishes a successful zero-candidate walk from a failed APA walk;\n// only the former may replace/latch the current VCD list.\n''')

vcd_func = r'''static int hddBuildVcdGameList(void)
{
    hdd_pops_list_t parts;
    base_game_info_t *newGames = NULL;
    char(*newParts)[APA_IDMAX + 1] = NULL;
    int total = 0;
    int scanIncomplete = 0;
    unsigned int genAtEntry = hddVcdCacheGen; // re-latch below only if no invalidation raced this build

    // Best-effort cleanup from an interrupted prior scan. This never touches pfs0:, which remains the
    // live OPL data mount used by both PS2 and VCD config/art lookups.
    fileXioUmount("pfs1:");

    int partCount = hddGetPopsPartitionList(&parts);
    if (partCount < 0) {
        // TRANSACTIONAL LIST OWNERSHIP: a failed APA table walk is not an empty VCD library. Keep the
        // last-good arrays exactly as they are and leave the cache unlatched so a later explicit
        // refresh can retry.
        LOG("HDD VCD: APA partition enumeration failed (%d); preserving %d last-good game(s)\n", partCount, hddVcdGameCount);
        hddVcdListBuilt = 0;
        return hddVcdGameCount;
    }
    if (partCount == 0) {
        // This time zero is authoritative: hdd0: opened and the complete APA walk found no candidates.
        hddFreeVcdGameList();
        hddVcdListBuilt = (genAtEntry == hddVcdCacheGen);
        return 0;
    }

    for (int p = 0; p < parts.count; p++) {
        char mountSrc[64];
        snprintf(mountSrc, sizeof(mountSrc), "hdd0:%s", parts.names[p]);

        // PP.<name> / __.<name> one-game install: display the label without its three-character
        // prefix and retain the FULL label for launch. The name predicate excludes the exact
        // __.POPS[0-9]? pooled containers handled below.
        if (hddIsPopsPartitionGame(parts.names[p])) {
            char discId[12]; // validation only -- the entry keys off the full label, never the ID
            if (!vcdExtractGameId(parts.names[p] + 3, discId, sizeof(discId))) {
                // ID-less label (e.g. PP.CASTLEVANIA): require ONE exact IMAGE0.VCD at the partition
                // root. A mount failure means this refresh was incomplete; an open miss AFTER a
                // successful mount is authoritative and simply identifies a non-POPS HDDOSD app.
                if (fileXioMount("pfs1:", mountSrc, FIO_MT_RDONLY) < 0) {
                    scanIncomplete = 1;
                    continue;
                }
                int imgfd = open("pfs1:/IMAGE0.VCD", O_RDONLY);
                if (imgfd >= 0)
                    close(imgfd); // close before unmount; ps2fs may reject an unmount with a live fd
                fileXioUmount("pfs1:");
                if (imgfd < 0)
                    continue;
            }

            base_game_info_t *grownGames = realloc(newGames, (total + 1) * sizeof(base_game_info_t));
            if (grownGames == NULL) {
                scanIncomplete = 1;
                break;
            }
            newGames = grownGames;
            char(*grownParts)[APA_IDMAX + 1] = realloc(newParts, (total + 1) * sizeof(*newParts));
            if (grownParts == NULL) {
                scanIncomplete = 1;
                break;
            }
            newParts = grownParts;

            base_game_info_t *g = &newGames[total];
            memset(g, 0, sizeof(base_game_info_t));
            snprintf(g->name, sizeof(g->name), "%s", parts.names[p] + 3); // strip PP. / __. for display
            snprintf(g->startup, sizeof(g->startup), "%s", g->name);      // keep VCD identity = name
            snprintf(g->extension, sizeof(g->extension), ".VCD");
            g->parts = 1;
            g->format = GAME_FORMAT_ISO;                                  // VCD flag gates launch
            snprintf(newParts[total], APA_IDMAX + 1, "%s", parts.names[p]); // case-preserved label
            total++;
            continue;
        }

        // __.POPS[0-9]? pooled container: mount and scan its root for *.VCD entries.
        if (fileXioMount("pfs1:", mountSrc, FIO_MT_RDONLY) < 0) {
            scanIncomplete = 1;
            continue;
        }

        vcd_entry_t *vcds = NULL;
        int n = vcdScanDirRoot("pfs1:/", &vcds);
        fileXioUmount("pfs1:");
        if (n < 0) {
            free(vcds);
            scanIncomplete = 1;
            continue;
        }
        if (n == 0) {
            free(vcds);
            continue;
        }

        base_game_info_t *grownGames = realloc(newGames, (total + n) * sizeof(base_game_info_t));
        if (grownGames == NULL) {
            free(vcds);
            scanIncomplete = 1;
            break;
        }
        newGames = grownGames;
        char(*grownParts)[APA_IDMAX + 1] = realloc(newParts, (total + n) * sizeof(*newParts));
        if (grownParts == NULL) {
            free(vcds);
            scanIncomplete = 1;
            break;
        }
        newParts = grownParts;

        int kept = 0;
        for (int i = 0; i < n; i++) {
            if (gVcdFirstDiscOnly && vcdIsHiddenDisc(vcds[i].name))
                continue;
            base_game_info_t *g = &newGames[total + kept];
            memset(g, 0, sizeof(base_game_info_t));
            snprintf(g->name, sizeof(g->name), "%s", vcds[i].name);
            snprintf(g->startup, sizeof(g->startup), "%s", vcds[i].name);
            snprintf(g->extension, sizeof(g->extension), ".VCD");
            g->parts = 1;
            g->format = GAME_FORMAT_ISO;
            snprintf(newParts[total + kept], APA_IDMAX + 1, "%s", parts.names[p]);
            kept++;
        }
        free(vcds);
        total += kept;
    }

    hddFreePopsPartitionList(&parts);
    fileXioUmount("pfs1:");

    // If a refresh could not inspect every candidate, a non-empty last-good list has more authority
    // than a newly-built partial one. This is the VCD twin of the transactional HDL refresh above.
    if (scanIncomplete && hddVcdGameCount > 0) {
        free(newGames);
        free(newParts);
        hddVcdListBuilt = 0;
        LOG("HDD VCD: incomplete refresh; preserving %d last-good game(s)\n", hddVcdGameCount);
        return hddVcdGameCount;
    }

    free(hddVcdGames);
    free(hddVcdParts);
    hddVcdGames = newGames;
    hddVcdParts = newParts;
    hddVcdGameCount = total;

    // A first-ever incomplete scan may still expose the entries it proved readable, but it is never
    // latched as complete. A fully successful zero/nonzero scan is latched unless invalidated mid-run.
    hddVcdListBuilt = !scanIncomplete && (genAtEntry == hddVcdCacheGen);
    return total;
}

'''
replace_between("src/hddsupport.c", "static int hddBuildVcdGameList(void)\n", "static int hddNeedsUpdate", vcd_func)

p, text, crlf = load("HANDOFF.md")
note = r'''
## Step 212 follow-up — APA enumerators publish only complete walks

- `hddGetHDLGamelist()` now treats a negative `fileXioDread()` and any record-allocation failure as
  scan failure; it never turns a truncated APA table walk into a successful partial HDL list.
- `hddGetPopsPartitionList()` now distinguishes a successful zero-candidate APA walk from failure and
  rejects partial walks on dopen/dread/OOM errors.
- HDD VCD rebuilds are transactional. A failed APA walk or incomplete candidate scan preserves a
  non-empty last-good VCD list instead of clearing it first; a successful zero-candidate walk may
  authoritatively clear/latch the list. A first-ever incomplete scan may expose only entries actually
  proven readable, but remains unlatched so an explicit refresh can retry.
- These are enumeration/list-lifetime corrections above the transport layer. No ATA/DEV9 readiness
  policy changed.
'''
if "## Step 212 follow-up — APA enumerators publish only complete walks" not in text:
    text = text.rstrip("\n") + "\n" + note
save(p, text, crlf)
PY

git diff --check

git config user.name "NathanNeurotic Step 212 helper"
git config user.email "109461996+NathanNeurotic@users.noreply.github.com"
git add src/hdd.c src/hddsupport.c HANDOFF.md
git commit -m "rebuild-212: make APA list scans transactional"
git push origin HEAD:"$prod_branch"
