#!/usr/bin/env bash
set -euo pipefail

repo="NathanNeurotic/Open-PS2-Loader"
prod_branch="rebuild/step-212-apa-boot-and-bgm-resilience"
expected_head="11725ba7b3b4a461c1f205e2919121799d5c8a39"

git fetch origin "$prod_branch"
actual_head="$(git rev-parse "origin/$prod_branch")"
if [[ "$actual_head" != "$expected_head" ]]; then
    echo "Refusing production edit: expected $expected_head, got $actual_head" >&2
    exit 1
fi

git checkout -B "$prod_branch" "origin/$prod_branch"

python3 - <<'PY'
from pathlib import Path


def read_text(path):
    p = Path(path)
    raw = p.read_bytes()
    crlf = b"\r\n" in raw
    text = raw.decode("utf-8").replace("\r\n", "\n")
    return p, text, crlf


def write_text(p, text, crlf):
    if crlf:
        text = text.replace("\n", "\r\n")
    p.write_bytes(text.encode("utf-8"))


def replace_once(path, old, new):
    p, text, crlf = read_text(path)
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}")
    text = text.replace(old, new, 1)
    write_text(p, text, crlf)


def replace_n(path, old, new, expected):
    p, text, crlf = read_text(path)
    count = text.count(old)
    if count != expected:
        raise SystemExit(f"{path}: expected {expected} matches, got {count}")
    text = text.replace(old, new)
    write_text(p, text, crlf)

# --- HDD view/update lifecycle -------------------------------------------------
replace_once(
    "src/hddsupport.c",
    '''static int hddNeedsUpdate(item_list_t *itemList)\n{ /* Auto refresh is disabled by setting HDD_MODE_UPDATE_DELAY to MENU_UPD_DELAY_NOUPDATE, within hddsupport.h.\n       Hence any update request would be issued by the user, which should be taken as an explicit request to re-scan the HDD. */\n    if (vcdConsumeDirty(itemList->mode))\n        return 1; // L3 toggle / default-view change -> rebuild the submenu (the ARRAY may be cached)\n    if (vcdListViewActive(itemList))\n        return 0; // in VCD view: skip the HDL re-scan churn\n    return 1;\n}\n''',
    '''static int hddNeedsUpdate(item_list_t *itemList)\n{ /* Auto refresh is disabled by setting HDD_MODE_UPDATE_DELAY to MENU_UPD_DELAY_NOUPDATE, within hddsupport.h.\n       Hence any update request would be issued by the user, which should be taken as an explicit request to re-scan the HDD. */\n    if (vcdConsumeDirty(itemList->mode))\n        return 1; // L3 toggle / default-view change -> rebuild the submenu (the ARRAY may be cached)\n    if (vcdListViewActive(itemList))\n        // The locked-to-VCD startup path also receives the normal initial deferred update, but no\n        // toggle dirtied the view first. Build once when the VCD backing list does not exist yet.\n        // A manual HDD/VCD refresh invalidates this latch before posting the same deferred update.\n        return !hddVcdListBuilt;\n    return 1;\n}\n''')

replace_once(
    "src/hddsupport.c",
    '''    hdl_games_list_t hddGamesNew;\n    int ret;\n\n    // Force a live APA scan not only when the cache fails to load (ret != 0) or a prior build asked for it\n    // (hddForceUpdate), but ALSO when the cache loaded EMPTY (count 0). A missing/empty/stale games.bin\n    // otherwise left the first HDD page blank until a second manual refresh (provato's HW report).\n    if (((ret = hddLoadGameListCache(&hddGames)) != 0) || (hddForceUpdate) || (hddGames.count == 0)) {\n        hddGamesNew.count = 0;\n        hddGamesNew.games = NULL;\n        ret = hddGetHDLGamelist(&hddGamesNew);\n        if (ret == 0) {\n            hddUpdateGameListCache(&hddGames, &hddGamesNew);\n            hddFreeHDLGamelist(&hddGames);\n            hddGames = hddGamesNew;\n        }\n    }\n\n    hddForceUpdate = 1; // Subsequent refresh operations will cause the HDD to be scanned.\n\n    return (ret == 0 ? hddGames.count : 0);\n''',
    '''    hdl_games_list_t cachedGames = {0};\n    hdl_games_list_t hddGamesNew = {0};\n    int cacheRet, scanRet = 0;\n\n    // TRANSACTIONAL REFRESH. hddGames is the LIVE backing array used by the menu. The old path\n    // passed it directly to hddLoadGameListCache(), whose first action is hddFreeHDLGamelist(): a\n    // missing/corrupt games.bin therefore destroyed a perfectly good live list BEFORE the fallback\n    // APA scan had proved it could replace it. If that scan failed, VCD -> HDL returned to a blank\n    // PS2 page even though the previous HDL list had been valid. Build cache/live candidates off to\n    // the side and publish only a successful replacement.\n    cacheRet = hddLoadGameListCache(&cachedGames);\n\n    // Force a live APA scan when the cache is absent/bad, a prior build requested a refresh, or the\n    // cache is empty. Otherwise the initial boot may use the valid games.bin exactly as before.\n    if (cacheRet == 0 && !hddForceUpdate && cachedGames.count > 0) {\n        hddFreeHDLGamelist(&hddGames);\n        hddGames = cachedGames;\n        cachedGames.games = NULL;\n        cachedGames.count = 0;\n    } else {\n        scanRet = hddGetHDLGamelist(&hddGamesNew);\n        if (scanRet == 0) {\n            hddUpdateGameListCache(&cachedGames, &hddGamesNew);\n            hddFreeHDLGamelist(&hddGames);\n            hddGames = hddGamesNew;\n            hddGamesNew.games = NULL;\n            hddGamesNew.count = 0;\n        } else {\n            // Keep the last-good live list. On a first entry with no live list yet, a valid cache is\n            // still a safer fallback than turning a transient scan failure into an empty page.\n            if (hddGames.count == 0 && cachedGames.count > 0) {\n                hddGames = cachedGames;\n                cachedGames.games = NULL;\n                cachedGames.count = 0;\n            }\n            LOG("HDDSUPPORT HDL refresh failed (%d); preserving %u last-good game(s)\\n", scanRet, hddGames.count);\n        }\n    }\n\n    hddFreeHDLGamelist(&cachedGames);\n    hddFreeHDLGamelist(&hddGamesNew);\n    hddForceUpdate = 1; // Subsequent refresh operations will cause the HDD to be scanned.\n\n    return hddGames.count;\n''')

# --- Manual refresh and Info-screen admission ---------------------------------
replace_once(
    "src/opl.c",
    '''static void itemExecRefresh(struct menu_item *curMenu)\n{\n    item_list_t *support = curMenu->userdata;\n\n    if (support && support->enabled) {\n        // Fork shape verbatim. FAV has no device to rescan: loadFavourites() re-reads\n        // favourites.bin and schedules its own rebuild. Everything else posts the deferred\n        // update and lets the device's own NeedsUpdate logic decide what a refresh means.\n        if (support->mode == FAV_MODE)\n            loadFavourites();\n        else\n            ioPutRequest(IO_MENU_UPDATE_DEFFERED, &support->mode);\n        sfxPlay(SFX_CONFIRM);\n    }\n}\n''',
    '''static void itemExecRefresh(struct menu_item *curMenu)\n{\n    item_list_t *support = curMenu->userdata;\n\n    if (support && support->enabled) {\n        // FAV has no device to rescan: loadFavourites() re-reads favourites.bin and schedules its\n        // own rebuild. HDD's VCD view is intentionally cached across ordinary L3 flips, so an\n        // explicit user Refresh must invalidate that cache before posting the deferred update.\n        if (support->mode == FAV_MODE) {\n            loadFavourites();\n        } else {\n            if (support->mode == HDD_MODE && vcdListViewActive(support))\n                hddVcdInvalidateCache();\n            ioPutRequest(IO_MENU_UPDATE_DEFFERED, &support->mode);\n        }\n        sfxPlay(SFX_CONFIRM);\n    }\n}\n''')

replace_once(
    "src/opl.c",
    '''        item_list_t *support = curMenu->userdata;\n        if (support == NULL || !vcdViewActive(support->mode))\n            menuRequestInfoSize();\n        guiSwitchScreen(GUI_SCREEN_INFO);\n''',
    '''        item_list_t *support = curMenu->userdata;\n        // Direct HDD/HDL rows already carry their size in APA metadata (total_size_in_kb), so the\n        // generic CFG+stat pass is redundant there. VCD rows likewise have no meaningful #Size.\n        // Avoid putting either no-op read onto the shared IO worker merely for opening Info.\n        if (support == NULL || (!vcdListViewActive(support) && support->mode != HDD_MODE))\n            menuRequestInfoSize();\n        guiSwitchScreen(GUI_SCREEN_INFO);\n''')

# --- Info request dedupe --------------------------------------------------------
replace_once(
    "src/menusys.c",
    '''static item_list_t *itemConfigList;\n\nstatic u8 parentalLockCheckEnabled = 1;\n''',
    '''static item_list_t *itemConfigList;\n// One Info/#Size resolve at a time. Repeated enter/back/enter used to enqueue duplicate CFG+stat\n// reads for the same row on the single IO worker, multiplying storage contention without producing\n// any additional state. Keep the request latched until its worker pass finishes or is abandoned.\nstatic unsigned char infoSizeRequestPending;\n\nstatic u8 parentalLockCheckEnabled = 1;\n''')

old_info = '''// Opening the info screen needs #Size, which the scroll-time config load deliberately skips\n// (sbConfigStatSize off -> no slow per-game stat while browsing). Rebuild the current item's\n// config once with the size resolved and swap it in, but only if the selection is unchanged --\n// the user may have scrolled away before this IO request ran. game->sizeMB is cached afterwards,\n// so subsequent scrolls/info views show the size with no further stat.\nstatic void _menuResolveInfoSize()\n{\n    item_list_t *list = NULL;\n    config_set_t *loadedConfig = NULL;\n    int configId = -1;\n\n    WaitSema(menuSemaId);\n    if (selected_item != NULL && selected_item->item != NULL && selected_item->item->current != NULL &&\n        itemConfigId >= 0 && itemConfigId == selected_item->item->current->item.id &&\n        itemConfigList == selected_item->item->userdata) {\n        list = selected_item->item->userdata;\n        configId = itemConfigId;\n    }\n    SignalSema(menuSemaId);\n\n    if (list == NULL || configId < 0)\n        return;\n\n    // #Size is cosmetic/discretionary IO. ART already yields when BGM reserve is critical, but this\n    // path did not: repeatedly entering Info on USB could still make a CFG+stat compete with the\n    // decoder until audio stopped. Wait in short sleeps, and abandon the work entirely if the user\n    // has moved away while audio refills. BGM does its own file IO, so sleeping the general IO worker\n    // here does not prevent the reserve from recovering.\n    while (!bgmDiscretionaryIoAllowed()) {\n        int stillWanted;\n        DelayThread(10 * 1000);\n        WaitSema(menuSemaId);\n        stillWanted = selected_item != NULL && selected_item->item != NULL &&\n                      selected_item->item->current != NULL && itemConfigId == configId &&\n                      itemConfigList == list && itemConfigId == selected_item->item->current->item.id &&\n                      selected_item->item->userdata == list;\n        SignalSema(menuSemaId);\n        if (!stillWanted)\n            return;\n    }\n\n    // itemGetConfig runs OUTSIDE the semaphore on purpose: it is the slow call (a CFG read plus the\n    // ISO stat this whole mechanism exists to defer), and holding menuSemaId across it would block\n    // the GUI thread for exactly as long as the stat we are trying to keep off the scroll path.\n    sbSetConfigStatSize(1);\n    loadedConfig = list->itemGetConfig(list, configId);\n    sbSetConfigStatSize(0);\n\n    if (loadedConfig == NULL)\n        return;\n\n    // Re-check the selection under the sema: if the user scrolled away while the stat ran, the\n    // freshly loaded config belongs to a row that is no longer current -- drop it rather than\n    // publish someone else's #Size.\n    WaitSema(menuSemaId);\n    if (selected_item != NULL && selected_item->item != NULL && selected_item->item->current != NULL &&\n        itemConfigId == configId && itemConfigList == list && itemConfigId == selected_item->item->current->item.id &&\n        selected_item->item->userdata == list) {\n        if (itemConfig != NULL)\n            configFree(itemConfig);\n        itemConfig = loadedConfig;\n        itemConfigList = list;\n        loadedConfig = NULL;\n    }\n    SignalSema(menuSemaId);\n\n    if (loadedConfig != NULL)\n        configFree(loadedConfig);\n}\n\n// Queued when the info screen opens: resolve #Size for the current item without blocking the UI.\nvoid menuRequestInfoSize(void)\n{\n    ioPutRequest(IO_CUSTOM_SIMPLEACTION, &_menuResolveInfoSize);\n}\n'''

new_info = '''static void menuInfoSizeComplete(void)\n{\n    WaitSema(menuSemaId);\n    infoSizeRequestPending = 0;\n    SignalSema(menuSemaId);\n}\n\n// Opening the info screen needs #Size, which the scroll-time config load deliberately skips\n// (sbConfigStatSize off -> no slow per-game stat while browsing). Rebuild the current item's\n// config once with the size resolved and swap it in, but only if the selection is unchanged --\n// the user may have scrolled away before this IO request ran. game->sizeMB is cached afterwards,\n// so subsequent scrolls/info views show the size with no further stat.\nstatic void _menuResolveInfoSize()\n{\n    item_list_t *list = NULL;\n    config_set_t *loadedConfig = NULL;\n    int configId = -1;\n\n    WaitSema(menuSemaId);\n    if (selected_item != NULL && selected_item->item != NULL && selected_item->item->current != NULL &&\n        itemConfigId >= 0 && itemConfigId == selected_item->item->current->item.id &&\n        itemConfigList == selected_item->item->userdata) {\n        list = selected_item->item->userdata;\n        configId = itemConfigId;\n    }\n    SignalSema(menuSemaId);\n\n    if (list == NULL || configId < 0) {\n        menuInfoSizeComplete();\n        return;\n    }\n\n    // #Size is cosmetic/discretionary IO. ART already yields when BGM reserve is critical, but this\n    // path did not: repeatedly entering Info on USB could still make a CFG+stat compete with the\n    // decoder until audio stopped. Wait in short sleeps, and abandon the work entirely if the user\n    // has moved away while audio refills. BGM does its own file IO, so sleeping the general IO worker\n    // here does not prevent the reserve from recovering.\n    while (!bgmDiscretionaryIoAllowed()) {\n        int stillWanted;\n        DelayThread(10 * 1000);\n        WaitSema(menuSemaId);\n        stillWanted = selected_item != NULL && selected_item->item != NULL &&\n                      selected_item->item->current != NULL && itemConfigId == configId &&\n                      itemConfigList == list && itemConfigId == selected_item->item->current->item.id &&\n                      selected_item->item->userdata == list;\n        SignalSema(menuSemaId);\n        if (!stillWanted) {\n            menuInfoSizeComplete();\n            return;\n        }\n    }\n\n    // itemGetConfig runs OUTSIDE the semaphore on purpose: it is the slow call (a CFG read plus the\n    // ISO stat this whole mechanism exists to defer), and holding menuSemaId across it would block\n    // the GUI thread for exactly as long as the stat we are trying to keep off the scroll path.\n    sbSetConfigStatSize(1);\n    loadedConfig = list->itemGetConfig(list, configId);\n    sbSetConfigStatSize(0);\n\n    if (loadedConfig == NULL) {\n        menuInfoSizeComplete();\n        return;\n    }\n\n    // Re-check the selection under the sema: if the user scrolled away while the stat ran, the\n    // freshly loaded config belongs to a row that is no longer current -- drop it rather than\n    // publish someone else's #Size.\n    WaitSema(menuSemaId);\n    if (selected_item != NULL && selected_item->item != NULL && selected_item->item->current != NULL &&\n        itemConfigId == configId && itemConfigList == list && itemConfigId == selected_item->item->current->item.id &&\n        selected_item->item->userdata == list) {\n        if (itemConfig != NULL)\n            configFree(itemConfig);\n        itemConfig = loadedConfig;\n        itemConfigList = list;\n        loadedConfig = NULL;\n    }\n    SignalSema(menuSemaId);\n\n    if (loadedConfig != NULL)\n        configFree(loadedConfig);\n\n    menuInfoSizeComplete();\n}\n\n// Queued when the info screen opens: resolve #Size for the current item without blocking the UI.\nvoid menuRequestInfoSize(void)\n{\n    WaitSema(menuSemaId);\n    if (infoSizeRequestPending) {\n        SignalSema(menuSemaId);\n        return;\n    }\n    infoSizeRequestPending = 1;\n    SignalSema(menuSemaId);\n\n    if (ioPutRequest(IO_CUSTOM_SIMPLEACTION, &_menuResolveInfoSize) != IO_OK)\n        menuInfoSizeComplete();\n}\n'''
replace_once("src/menusys.c", old_info, new_info)

replace_once(
    "src/menusys.c",
    '''    itemConfig = NULL;\n    itemConfigList = NULL;\n    mainMenu = NULL;\n''',
    '''    itemConfig = NULL;\n    itemConfigList = NULL;\n    infoSizeRequestPending = 0;\n    mainMenu = NULL;\n''')

# --- VCD one-slot request failure contract -------------------------------------
replace_once(
    "src/vcdsupport.c",
    '''    ioPutRequest(IO_CUSTOM_SIMPLEACTION, &vcdResolveQueuedDisplayId);\n}\n''',
    '''    if (ioPutRequest(IO_CUSTOM_SIMPLEACTION, &vcdResolveQueuedDisplayId) != IO_OK) {\n        // No worker owns the one-slot request when enqueue fails. Release the latch immediately or\n        // every future cosmetic ID request is suppressed for the rest of the session.\n        DIntr();\n        gVcdIdReqPending = 0;\n        EIntr();\n    }\n}\n''')

# --- Artwork focus-generation adoption ----------------------------------------
old_art = '''                    if (isPriority) {\n                        load_image_request_t *req = (load_image_request_t *)entry->qr;\n                        if (req != NULL) {\n                            // A warmed neighbor can become the selected row while still queued/in-flight.\n                            // Adopt that exact request into the new focus generation instead of throwing it away.\n                            req->focusEpoch = gArtFocusEpoch;\n                            if (!(req->sio2 && gArtNavActive))\n                                artPromote(req);\n                        }\n                    }\n'''
new_art = '''                    load_image_request_t *req = (load_image_request_t *)entry->qr;\n                    if (req != NULL) {\n                        // Any pending cover touched by THIS frame is still useful to the new\n                        // selection neighborhood. Adopt it into the current generation even when\n                        // it is merely a neighbour; only the highlighted cover gets queue promotion.\n                        // Without this, overlapping lookahead (+2 becomes +1 after one step) kept\n                        // the OLD generation and the worker discarded work the new frame still wanted.\n                        req->focusEpoch = gArtFocusEpoch;\n                        if (isPriority && !(req->sio2 && gArtNavActive))\n                            artPromote(req);\n                    }\n'''
replace_n("src/texcache.c", old_art, new_art, 2)

# --- Handoff note ---------------------------------------------------------------
p, handoff, crlf = read_text("HANDOFF.md")
note = '''\n## Step 212 follow-up — transactional HDD refresh / metadata dedupe / art overlap\n\n- Do not treat the current HDL backing list as scratch space for `games.bin`: cache and live APA\n  candidates are built separately and only a successful replacement is published. A failed refresh\n  preserves the last-good HDL list; a valid cache may rescue a first-entry live-scan failure.\n- Locked-to-VCD startup performs its initial HDD VCD build even without an L3 dirty bit. Explicit\n  Refresh in HDD VCD view invalidates the session VCD cache before rebuilding.\n- Direct HDD/HDL Info does not queue the generic CFG+stat size pass (APA metadata already owns size);\n  generic Info size resolution is one-at-a-time so repeated enter/back cannot stack duplicate reads.\n- A failed VCD cosmetic-ID enqueue releases its one-slot pending latch.\n- Pending cover requests that are touched by the new frame adopt the new focus generation even when\n  they remain neighbours; only the selected request is promoted. This keeps overlapping Coverflow/List\n  lookahead instead of dropping and rereading it after every navigation step.\n- This follow-up deliberately does NOT change ATA/DEV9 readiness: working HDD VCD enumeration proves\n  the relevant APA/PFS stack is resident for the reported HDL-list symptom.\n'''
if "## Step 212 follow-up — transactional HDD refresh / metadata dedupe / art overlap" not in handoff:
    handoff = handoff.rstrip("\n") + "\n" + note
write_text(p, handoff, crlf)
PY

git diff --check

git config user.name "NathanNeurotic Step 212 helper"
git config user.email "109461996+NathanNeurotic@users.noreply.github.com"
git add src/hddsupport.c src/opl.c src/menusys.c src/vcdsupport.c src/texcache.c HANDOFF.md
git commit -m "rebuild-212: harden list refresh and art reuse"
git push origin HEAD:"$prod_branch"
