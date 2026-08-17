#!/usr/bin/env bash
set -euo pipefail

prod_branch="rebuild/step-212-apa-boot-and-bgm-resilience"
expected_head="8fb45b1f0c85a75b7efcaaf9698742c0109f395b"

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


def replace_once(path, old, new):
    p, text, crlf = load(path)
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}")
    save(p, text.replace(old, new, 1), crlf)

# CodeRabbit: VCD favourites use text/name as their stable identity. Their stored numeric id is not
# authoritative and must not reject absolute theme-asset fallback.
replace_once(
    "src/favsupport.c",
    '''            item_list_t ownerView;\n            item_list_t *o = favOwnerView(i, &ownerView);\n            if (o == NULL || o->itemGetImage == NULL || !favOwnerHasId(o, favArray[i].id))\n                continue;\n            int r = o->itemGetImage(o, folder, isRelative, value, suffix, resultTex, psm);\n''',
    '''            item_list_t ownerView;\n            item_list_t *o = favOwnerView(i, &ownerView);\n            if (o == NULL || o->itemGetImage == NULL ||\n                (!favArray[i].isVcd && !favOwnerHasId(o, favArray[i].id)))\n                continue;\n            int r = o->itemGetImage(o, folder, isRelative, value, suffix, resultTex, psm);\n''')

# CodeRabbit: latest-wins Info/#Size admission. One worker remains in flight, but a newer row is
# retained and processed immediately after the stale request instead of being dropped by the latch.
replace_once(
    "src/menusys.c",
    '''// One Info/#Size resolve at a time. Repeated enter/back/enter used to enqueue duplicate CFG+stat\n// reads for the same row on the single IO worker, multiplying storage contention without producing\n// any additional state. Keep the request latched until its worker pass finishes or is abandoned.\nstatic unsigned char infoSizeRequestPending;\n''',
    '''// One Info/#Size worker at a time, with a latest-wins target. Repeated enter/back/enter on the\n// SAME row does not queue duplicate CFG+stat reads; moving to another row while one is active replaces\n// the requested target and the worker loops to that newest row before releasing its latch.\nstatic unsigned char infoSizeWorkerPending;\nstatic item_list_t *infoSizeRequestList;\nstatic int infoSizeRequestId = -1;\nstatic unsigned int infoSizeRequestGeneration;\n''')

start = '''static void menuInfoSizeComplete(void)\n{\n'''
end = '''static void _menuSaveConfig()\n{\n'''
p, text, crlf = load("src/menusys.c")
a = text.find(start)
b = text.find(end, a)
if a < 0 or b < 0:
    raise SystemExit("src/menusys.c: Info-size block markers not found")
new_block = r'''// Opening the info screen needs #Size, which the scroll-time config load deliberately skips.
// The worker is single-instance but the TARGET is replaceable: if the user opens Info for B while A
// is still resolving, A is discarded when stale and the same worker immediately processes B.
static void _menuResolveInfoSize()
{
    for (;;) {
        item_list_t *list = NULL;
        config_set_t *loadedConfig = NULL;
        int configId = -1;
        unsigned int generation = 0;

        WaitSema(menuSemaId);
        list = infoSizeRequestList;
        configId = infoSizeRequestId;
        generation = infoSizeRequestGeneration;
        SignalSema(menuSemaId);

        if (list == NULL || configId < 0)
            goto complete_request;

        // #Size is cosmetic/discretionary IO. Wait for BGM reserve, but stop waiting on a target that
        // has already been superseded or a selection the user left; the next loop takes the newest.
        while (!bgmDiscretionaryIoAllowed()) {
            int stillWanted;
            DelayThread(10 * 1000);
            WaitSema(menuSemaId);
            stillWanted = infoSizeRequestGeneration == generation && infoSizeRequestList == list &&
                          infoSizeRequestId == configId && selected_item != NULL &&
                          selected_item->item != NULL && selected_item->item->current != NULL &&
                          selected_item->item->userdata == list &&
                          selected_item->item->current->item.id == configId;
            SignalSema(menuSemaId);
            if (!stillWanted)
                goto complete_request;
        }

        // itemGetConfig runs outside menuSemaId: it is the slow CFG+stat operation.
        sbSetConfigStatSize(1);
        loadedConfig = list->itemGetConfig(list, configId);
        sbSetConfigStatSize(0);

        // Publish only if this request is STILL the newest target and the same row is selected.
        WaitSema(menuSemaId);
        if (loadedConfig != NULL && infoSizeRequestGeneration == generation &&
            infoSizeRequestList == list && infoSizeRequestId == configId && selected_item != NULL &&
            selected_item->item != NULL && selected_item->item->current != NULL &&
            selected_item->item->userdata == list && selected_item->item->current->item.id == configId) {
            if (itemConfig != NULL)
                configFree(itemConfig);
            itemConfig = loadedConfig;
            itemConfigId = configId;
            itemConfigList = list;
            loadedConfig = NULL;
        }
        SignalSema(menuSemaId);

        if (loadedConfig != NULL)
            configFree(loadedConfig);

complete_request:
        WaitSema(menuSemaId);
        if (infoSizeRequestGeneration == generation) {
            // Nobody replaced this target while it ran: the queue is drained.
            infoSizeRequestList = NULL;
            infoSizeRequestId = -1;
            infoSizeWorkerPending = 0;
            SignalSema(menuSemaId);
            return;
        }
        // A newer target arrived while this one ran. Leave the worker latch set and loop directly;
        // no second IO request is needed and the newest request cannot be lost behind a queue failure.
        SignalSema(menuSemaId);
    }
}

// Queued when the info screen opens: resolve #Size without blocking the UI. Same-row repeats dedupe;
// a different row replaces the target even when the worker is already active.
void menuRequestInfoSize(void)
{
    int queueWorker = 0;

    WaitSema(menuSemaId);
    if (selected_item == NULL || selected_item->item == NULL || selected_item->item->current == NULL) {
        SignalSema(menuSemaId);
        return;
    }

    item_list_t *list = selected_item->item->userdata;
    int configId = selected_item->item->current->item.id;

    if (infoSizeWorkerPending && infoSizeRequestList == list && infoSizeRequestId == configId) {
        SignalSema(menuSemaId);
        return; // exact same work is already active/queued
    }

    infoSizeRequestList = list;
    infoSizeRequestId = configId;
    infoSizeRequestGeneration++;
    if (!infoSizeWorkerPending) {
        infoSizeWorkerPending = 1;
        queueWorker = 1;
    }
    SignalSema(menuSemaId);

    if (queueWorker && ioPutRequest(IO_CUSTOM_SIMPLEACTION, &_menuResolveInfoSize) != IO_OK) {
        WaitSema(menuSemaId);
        infoSizeWorkerPending = 0;
        infoSizeRequestList = NULL;
        infoSizeRequestId = -1;
        SignalSema(menuSemaId);
    }
}

'''
text = text[:a] + new_block + text[b:]
save(p, text, crlf)

replace_once(
    "src/menusys.c",
    '''    itemConfig = NULL;\n    itemConfigList = NULL;\n    infoSizeRequestPending = 0;\n    mainMenu = NULL;\n''',
    '''    itemConfig = NULL;\n    itemConfigList = NULL;\n    infoSizeWorkerPending = 0;\n    infoSizeRequestList = NULL;\n    infoSizeRequestId = -1;\n    infoSizeRequestGeneration = 0;\n    mainMenu = NULL;\n''')

# CodeRabbit: miniInit(HDD_MODE) must be symmetrically unwound on config allocation failure.
replace_once(
    "src/opl.c",
    '''    if (configSet == NULL) {\n        free(gAutoLaunchGame);\n        gAutoLaunchGame = NULL;\n        return;\n    }\n\n    hddLaunchGame(NULL, -1, configSet);\n''',
    '''    if (configSet == NULL) {\n        free(gAutoLaunchGame);\n        gAutoLaunchGame = NULL;\n        miniDeinit(NULL);\n        return;\n    }\n\n    hddLaunchGame(NULL, -1, configSet);\n''')

# Copy the startup before the multi-frame GameID hold. Device backing arrays are mutable on the IO
# worker; holding an itemGetStartup pointer across 45 guiEndFrame() unlocks is not safe.
replace_once(
    "src/opl.c",
    '''                config_set_t *configSet = menuLoadConfig();\n                // Flash the GameID barcode (Pixel FX/RetroGEM HDMI auto-profile) before handoff; this\n                // single menu chokepoint covers both the Neutrino and OPL-native cores. No-op when off.\n                guiShowGameID(support->itemGetStartup(support, curMenu->current->item.id));\n                support->itemLaunch(support, curMenu->current->item.id, configSet);\n''',
    '''                config_set_t *configSet = menuLoadConfig();\n                if (curMenu->current == NULL)\n                    return; // a deferred source refresh replaced the row while config IO was pending\n                int launchId = curMenu->current->item.id;\n                char gameIdStartup[128] = {0};\n                char *startup = support->itemGetStartup(support, launchId);\n                if (startup != NULL)\n                    snprintf(gameIdStartup, sizeof(gameIdStartup), "%s", startup);\n                // Flash the GameID barcode (Pixel FX/RetroGEM HDMI auto-profile) before handoff. Use\n                // the stack copy: the hold renders/unlocks for many frames while source lists may refresh.\n                guiShowGameID(gameIdStartup);\n                support->itemLaunch(support, launchId, configSet);\n''')

# VCD backing publication: the menu renderer holds guiLock for the whole frame. Publish the three
# globals and retire the old arrays under that same lock so a frame cannot observe mixed generations
# or dereference an array while it is being freed. updateMenuFromGameList already clears the old HDD
# submenu under guiLock before itemUpdate; the lock here covers the backing-array handoff itself.
replace_once(
    "src/hddsupport.c",
    '''static void hddFreeVcdGameList(void)\n{\n    free(hddVcdGames);\n    hddVcdGames = NULL;\n    free(hddVcdParts);\n    hddVcdParts = NULL;\n    hddVcdGameCount = 0;\n    hddVcdListBuilt = 0;\n}\n''',
    '''static void hddPublishVcdGameList(base_game_info_t *games, char (*parts)[APA_IDMAX + 1], int count, int built)\n{\n    base_game_info_t *oldGames;\n    char (*oldParts)[APA_IDMAX + 1];\n\n    guiLock();\n    oldGames = hddVcdGames;\n    oldParts = hddVcdParts;\n    hddVcdGames = games;\n    hddVcdParts = parts;\n    hddVcdGameCount = count;\n    hddVcdListBuilt = built;\n    free(oldGames);\n    free(oldParts);\n    guiUnlock();\n}\n\nstatic void hddFreeVcdGameList(void)\n{\n    hddPublishVcdGameList(NULL, NULL, 0, 0);\n}\n''')

replace_once(
    "src/hddsupport.c",
    '''    free(hddVcdGames);\n    free(hddVcdParts);\n    hddVcdGames = newGames;\n    hddVcdParts = newParts;\n    hddVcdGameCount = total;\n\n    // A first-ever incomplete scan may still expose the entries it proved readable, but it is never\n    // latched as complete. A fully successful zero/nonzero scan is latched unless invalidated mid-run.\n    hddVcdListBuilt = !scanIncomplete && (genAtEntry == hddVcdCacheGen);\n    return total;\n''',
    '''    // A first-ever incomplete scan may still expose the entries it proved readable, but it is never\n    // latched as complete. A fully successful zero/nonzero scan is latched unless invalidated mid-run.\n    hddPublishVcdGameList(newGames, newParts, total, !scanIncomplete && (genAtEntry == hddVcdCacheGen));\n    return total;\n''')

# VCD launch is GUI-thread input, outside the frame lock. Copy both parallel-array values under the
# lock before hddDoLaunchVcd starts any blocking/quiesce/mount work, so no backing pointer escapes.
replace_once(
    "src/hddsupport.c",
    '''    if (gAutoLaunchGame == NULL && vcdListViewActive(itemList)) {\n        base_game_info_t *vcd = hddActiveVcd(id);\n        if (vcd == &hddEmptyVcd)\n            return; // stale id in the L3 toggle window -> nothing to launch (hddVcdParts[id] would also OOB)\n        hddDoLaunchVcd(itemList, vcd->name, hddVcdParts[id]);\n        return;\n    }\n''',
    '''    if (gAutoLaunchGame == NULL && vcdListViewActive(itemList)) {\n        char vcdName[VCD_NAME_MAX];\n        char vcdPart[APA_IDMAX + 1];\n\n        guiLock();\n        base_game_info_t *vcd = hddActiveVcd(id);\n        if (vcd == &hddEmptyVcd) {\n            guiUnlock();\n            return; // stale id in the L3 toggle window -> nothing to launch\n        }\n        snprintf(vcdName, sizeof(vcdName), "%s", vcd->name);\n        snprintf(vcdPart, sizeof(vcdPart), "%s", hddVcdParts[id]);\n        guiUnlock();\n\n        hddDoLaunchVcd(itemList, vcdName, vcdPart);\n        return;\n    }\n''')

# The Favourites-by-name launch already blocks the IO worker before reading the backing list; make the
# copy explicit while it is blocked so nothing from hddVcdGames/hddVcdParts survives ioBlockOps(0).
replace_once(
    "src/hddsupport.c",
    '''    if (idx >= 0) {\n        snprintf(resolvedName, sizeof(resolvedName), "%s", hddVcdGames[idx].name);\n        snprintf(resolvedPart, sizeof(resolvedPart), "%s", hddVcdParts[idx]);\n    }\n    ioBlockOps(0);\n''',
    '''    if (idx >= 0) {\n        snprintf(resolvedName, sizeof(resolvedName), "%s", hddVcdGames[idx].name);\n        snprintf(resolvedPart, sizeof(resolvedPart), "%s", hddVcdParts[idx]);\n    }\n    ioBlockOps(0); // resolvedName/resolvedPart are now independent of the mutable backing arrays\n''')

p, text, crlf = load("HANDOFF.md")
note = '''\n## Step 212 follow-up — review hardening after transactional enumeration\n\n- HDD VCD backing-array publication is serialized with `guiLock`: games pointer, partition pointer,\n  count, cache latch, and old-array retirement move as one GUI-reader-safe handoff. Direct VCD launch\n  copies the parallel name/partition fields before any blocking work; GameID likewise receives a stack\n  copy of startup instead of holding a mutable list pointer across its multi-frame display.\n- Info `#Size` admission remains single-worker but is now latest-wins: same-row repeats dedupe, while\n  a different row replaces the target and is processed before the worker exits.\n- VCD favourites no longer require their intentionally-nonstable numeric id for absolute theme-image\n  fallback; their text/name identity remains authoritative.\n- HDD autolaunch allocation failure now calls `miniDeinit(NULL)` to unwind `miniInit(HDD_MODE)`.\n- No ATA/DEV9 readiness behavior changed.\n'''
if "## Step 212 follow-up — review hardening after transactional enumeration" not in text:
    text = text.rstrip("\n") + "\n" + note
save(p, text, crlf)
PY

git diff --check

git config user.name "NathanNeurotic Step 212 helper"
git config user.email "109461996+NathanNeurotic@users.noreply.github.com"
git add src/favsupport.c src/menusys.c src/opl.c src/hddsupport.c HANDOFF.md
git commit -m "rebuild-212: close list publication and request races"
git push origin HEAD:"$prod_branch"
