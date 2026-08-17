#!/usr/bin/env bash
set -euo pipefail

prod_branch="rebuild/step-212-apa-boot-and-bgm-resilience"
expected_head="3c592c8d352ee74099291edc4eed42cb31b6c47d"

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

# ETH favourites: an ISO favourite resolved while the live ETH page is in VCD view gets an ID from
# ethFavIsoGames. Every later id-based ETH accessor must therefore use that SAME snapshot when the
# stack-local favourite owner forces ISO view; otherwise the ID is applied to live VCD-backed ethGames.
replace_once(
    "src/ethsupport.c",
    '''static int ethGetGameCount(item_list_t *itemList)\n{\n    return ethGameCount;\n}\n\nstatic void *ethGetGame(item_list_t *itemList, int id)\n{\n    return (void *)&ethGames[id];\n}\n\nstatic char *ethGetGameName(item_list_t *itemList, int id)\n{\n    return ethGames[id].name;\n}\n\nstatic int ethGetGameNameLength(item_list_t *itemList, int id)\n{\n    if (ethGames[id].format != GAME_FORMAT_USBLD)\n        return ISO_GAME_NAME_MAX + 1;\n    else\n        return UL_GAME_NAME_MAX + 1;\n}\n\nstatic char *ethGetGameStartup(item_list_t *itemList, int id)\n{\n    // VCD view keys per-game data (CFG/art) off the VCD filename, not a disc ID (see sbPopulateConfig).\n    if (vcdListViewActive(itemList))\n        return ethGames[id].name;\n    return ethGames[id].startup;\n}\n''',
    '''static base_game_info_t *ethBackingForView(item_list_t *itemList, int *count)\n{\n    // A Favourites ISO owner is a stack-local copy with FORCE_ISO. If the visible ETH page currently\n    // owns the VCD array, the resolved favourite ID belongs to ethFavIsoGames instead. Never fall back\n    // to live ethGames when that snapshot was invalidated: returning an empty view is safer than using\n    // the same numeric ID against the wrong backing list, and the normal favourite rebuild will resolve\n    // it again after the ETH mutation/reconnect that invalidated the snapshot.\n    if (itemList != NULL && itemList->viewOverride == ITEM_VIEW_FORCE_ISO && vcdViewActive(ETH_MODE)) {\n        if (!ethFavIsoValid || ethFavIsoGames == NULL) {\n            if (count != NULL)\n                *count = 0;\n            return NULL;\n        }\n        if (count != NULL)\n            *count = ethFavIsoGameCount;\n        return ethFavIsoGames;\n    }\n\n    if (count != NULL)\n        *count = ethGameCount;\n    return ethGames;\n}\n\nstatic base_game_info_t *ethGameForView(item_list_t *itemList, int id)\n{\n    int count = 0;\n    base_game_info_t *games = ethBackingForView(itemList, &count);\n    if (games == NULL || id < 0 || id >= count)\n        return NULL;\n    return &games[id];\n}\n\nstatic int ethGetGameCount(item_list_t *itemList)\n{\n    int count = 0;\n    ethBackingForView(itemList, &count);\n    return count;\n}\n\nstatic void *ethGetGame(item_list_t *itemList, int id)\n{\n    return (void *)ethGameForView(itemList, id);\n}\n\nstatic char *ethGetGameName(item_list_t *itemList, int id)\n{\n    base_game_info_t *game = ethGameForView(itemList, id);\n    return game != NULL ? game->name : "";\n}\n\nstatic int ethGetGameNameLength(item_list_t *itemList, int id)\n{\n    base_game_info_t *game = ethGameForView(itemList, id);\n    if (game == NULL)\n        return 0;\n    return game->format != GAME_FORMAT_USBLD ? ISO_GAME_NAME_MAX + 1 : UL_GAME_NAME_MAX + 1;\n}\n\nstatic char *ethGetGameStartup(item_list_t *itemList, int id)\n{\n    base_game_info_t *game = ethGameForView(itemList, id);\n    if (game == NULL)\n        return "";\n    // VCD view keys per-game data (CFG/art) off the VCD filename, not a disc ID (see sbPopulateConfig).\n    if (vcdListViewActive(itemList))\n        return game->name;\n    return game->startup;\n}\n''')

replace_once(
    "src/ethsupport.c",
    '''    base_game_info_t *game = &ethGames[id];\n    struct cdvdman_settings_smb *settings;\n''',
    '''    base_game_info_t *game = ethGameForView(itemList, id);\n    struct cdvdman_settings_smb *settings;\n\n    if (game == NULL)\n        return;\n''')

replace_once(
    "src/ethsupport.c",
    '''static config_set_t *ethGetConfig(item_list_t *itemList, int id)\n{\n    return sbPopulateConfig(&ethGames[id], ethPrefix, "\\\\");\n}\n''',
    '''static config_set_t *ethGetConfig(item_list_t *itemList, int id)\n{\n    base_game_info_t *game = ethGameForView(itemList, id);\n    return game != NULL ? sbPopulateConfig(game, ethPrefix, "\\\\") : NULL;\n}\n''')

# HDL list publication: CodeRabbit's earlier consolidated race was only half-closed by protecting the
# VCD arrays. HDL refresh still freed hddGames while GUI callbacks could read its old non-zero count.
# Publish pointer+count and retire the old array under the renderer's guiLock, matching the VCD handoff.
replace_once(
    "src/hddsupport.c",
    '''static void hddFreeVcdGameList(void)\n{\n    hddPublishVcdGameList(NULL, NULL, 0, 0);\n}\n\n// Build the HDD VCD game list from both supported APA/PFS shapes. Exact __.POPS / __.POPS0..9\n''',
    '''static void hddFreeVcdGameList(void)\n{\n    hddPublishVcdGameList(NULL, NULL, 0, 0);\n}\n\nstatic void hddPublishHdlGameList(hdl_games_list_t *replacement)\n{\n    hdl_games_list_t old;\n\n    if (replacement == NULL)\n        return;\n\n    guiLock();\n    old = hddGames;\n    hddGames = *replacement;\n    replacement->games = NULL;\n    replacement->count = 0;\n    hddFreeHDLGamelist(&old);\n    guiUnlock();\n}\n\n// Build the HDD VCD game list from both supported APA/PFS shapes. Exact __.POPS / __.POPS0..9\n''')

replace_once(
    "src/hddsupport.c",
    '''    if (cacheRet == 0 && !hddForceUpdate && cachedGames.count > 0) {\n        hddFreeHDLGamelist(&hddGames);\n        hddGames = cachedGames;\n        cachedGames.games = NULL;\n        cachedGames.count = 0;\n    } else {\n        scanRet = hddGetHDLGamelist(&hddGamesNew);\n        if (scanRet == 0) {\n            hddUpdateGameListCache(&cachedGames, &hddGamesNew);\n            hddFreeHDLGamelist(&hddGames);\n            hddGames = hddGamesNew;\n            hddGamesNew.games = NULL;\n            hddGamesNew.count = 0;\n        } else {\n            // Keep the last-good live list. On a first entry with no live list yet, a valid cache is\n            // still a safer fallback than turning a transient scan failure into an empty page.\n            if (hddGames.count == 0 && cachedGames.count > 0) {\n                hddGames = cachedGames;\n                cachedGames.games = NULL;\n                cachedGames.count = 0;\n            }\n            LOG("HDDSUPPORT HDL refresh failed (%d); preserving %u last-good game(s)\\n", scanRet, hddGames.count);\n        }\n    }\n''',
    '''    if (cacheRet == 0 && !hddForceUpdate && cachedGames.count > 0) {\n        hddPublishHdlGameList(&cachedGames);\n    } else {\n        scanRet = hddGetHDLGamelist(&hddGamesNew);\n        if (scanRet == 0) {\n            hddUpdateGameListCache(&cachedGames, &hddGamesNew);\n            hddPublishHdlGameList(&hddGamesNew);\n        } else {\n            // Keep the last-good live list. On a first entry with no live list yet, a valid cache is\n            // still a safer fallback than turning a transient scan failure into an empty page.\n            if (hddGames.count == 0 && cachedGames.count > 0)\n                hddPublishHdlGameList(&cachedGames);\n            LOG("HDDSUPPORT HDL refresh failed (%d); preserving %u last-good game(s)\\n", scanRet, hddGames.count);\n        }\n    }\n''')

p, text, crlf = load("HANDOFF.md")
note = '''\n## Step 212 follow-up — close remaining backing-list publication gaps\n\n- ETH ISO favourites resolved while the live ETH page is in VCD view now keep using the private\n  `ethFavIsoGames` snapshot for every id-based read/config/launch accessor. `viewOverride=FORCE_ISO`\n  can no longer validate against the snapshot and then apply that numeric id to live VCD-backed\n  `ethGames`; an invalidated snapshot fails closed until normal favourite resolution rebuilds it.\n- HDL live-list replacement now mirrors the VCD publication contract: pointer, count, candidate\n  ownership transfer, and retirement of the old array occur under `guiLock()`. A GUI frame cannot\n  pass an old count check and then dereference a freed `hddGames.games` allocation during refresh.\n- No ATA/DEV9 readiness behavior and no APA write/create/format policy changed.\n'''
if "## Step 212 follow-up — close remaining backing-list publication gaps" not in text:
    text = text.rstrip("\n") + "\n" + note
save(p, text, crlf)
PY

git diff --check

git config user.name "NathanNeurotic Step 212 helper"
git config user.email "109461996+NathanNeurotic@users.noreply.github.com"
git add src/ethsupport.c src/hddsupport.c HANDOFF.md
git commit -m "rebuild-212: close remaining backing-list races"
git push origin HEAD:"$prod_branch"
