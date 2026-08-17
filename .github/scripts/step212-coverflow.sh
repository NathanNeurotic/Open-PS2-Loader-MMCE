#!/usr/bin/env bash
set -euo pipefail

git config user.name "NathanNeurotic Step Builder"
git config user.email "actions@users.noreply.github.com"
git fetch origin rebuild/step-212-apa-boot-and-bgm-resilience
git checkout rebuild/step-212-apa-boot-and-bgm-resilience
test "$(git rev-parse HEAD)" = "27d8d9ece1bdc4fb70128b23613f78c3bc7be6d3"

# -----------------------------------------------------------------------------
# A. APA enumeration must not depend on the combined persistent-PFS-ready latch.
# -----------------------------------------------------------------------------
python3 - <<'PY'
from pathlib import Path
p = Path('src/hddsupport.c')
text = p.read_text()
old = '''    if (!hddSupportModulesLoaded)\n        return 0;\n\n    if (vcdListViewActive(itemList))\n'''
new = '''    // The combined support latch includes PS2FS/PFS readiness, but the PS2 list itself is an\n    // APA-table read and must not disappear merely because the persistent PFS side is still settling.\n    // Retry above, then let each read-only scanner prove what is actually available. HDL enumeration\n    // fails harmlessly if hdd0: is not ready; the VCD builder likewise skips mounts it cannot open.\n    if (!hddSupportModulesLoaded)\n        LOG("HDDSUPPORT UpdateGameList: PFS support incomplete; attempting read-only APA enumeration anyway\\n");\n\n    if (vcdListViewActive(itemList))\n'''
if text.count(old) != 1:
    raise SystemExit('hddUpdateGameList hard-gate target did not match exactly once')
p.write_text(text.replace(old, new, 1))
PY

git diff --check -- src/hddsupport.c
git add src/hddsupport.c
git commit -m "rebuild-212: keep APA enumeration independent of PFS readiness"

# -----------------------------------------------------------------------------
# B. Deep prefetch is useful, but only for the CURRENT selection neighborhood.
#    Give every pending request a focus generation so old neighborhoods evaporate
#    before another device read instead of living for ~20 stale frames.
# -----------------------------------------------------------------------------
python3 - <<'PY'
from pathlib import Path
p = Path('src/texcache.c')
text = p.read_text()

old = '''    unsigned int failEpoch;\n    // Chunk-level abort, read by textures.c via texSetLoadAbortFlag. Lets a cancel interrupt a load\n'''
new = '''    unsigned int failEpoch;\n    // Selection generation this request belongs to. Deep cover lookahead is intentionally aggressive,\n    // but once the cursor moves its old neighborhood is no longer useful. A stale generation is dropped\n    // before another device read; an already-running uninterruptible RPC is allowed to finish safely.\n    unsigned int focusEpoch;\n    // Chunk-level abort, read by textures.c via texSetLoadAbortFlag. Lets a cancel interrupt a load\n'''
if text.count(old) != 1:
    raise SystemExit('request focusEpoch insertion target did not match exactly once')
text = text.replace(old, new, 1)

old = '''static unsigned int gArtEpoch = 0;\n\nvoid cacheDropQueuedArt(void)\n'''
new = '''static unsigned int gArtEpoch = 0;\n\n// Selection-focus epoch. The selected cover is the only priority art request, so it is also the\n// authoritative signal that the useful prefetch neighborhood changed. Keep only a hash+length here:\n// VCD basenames may exceed the cache reunion key, and a collision can at worst preserve a little\n// obsolete prefetch -- it can never select the wrong texture or affect identity. GUI thread writes;\n// the art worker only compares the epoch captured in each request.\nstatic volatile unsigned int gArtFocusEpoch = 1;\nstatic item_list_t *gArtFocusList = NULL;\nstatic unsigned int gArtFocusHash = 0;\nstatic size_t gArtFocusLen = 0;\n\nstatic unsigned int artFocusHashValue(const char *value)\n{\n    unsigned int h = 2166136261u; // FNV-1a\n    const unsigned char *p = (const unsigned char *)value;\n    while (*p != '\\0') {\n        h ^= *p++;\n        h *= 16777619u;\n    }\n    return h;\n}\n\nstatic void artUpdateFocus(item_list_t *list, const char *value)\n{\n    size_t len = strlen(value);\n    unsigned int hash = artFocusHashValue(value);\n\n    if (gArtFocusList == list && gArtFocusLen == len && gArtFocusHash == hash)\n        return;\n\n    gArtFocusList = list;\n    gArtFocusLen = len;\n    gArtFocusHash = hash;\n    gArtFocusEpoch++;\n    if (gArtFocusEpoch == 0)\n        gArtFocusEpoch = 1;\n\n    // Do not forcibly kill gArtCurrentReq here. It may be a warmed neighbor that just became the\n    // selected cover; its priority lookup below will adopt it into this new epoch. If it is truly\n    // obsolete, cacheLoadImage drops it before IO when possible; a whole-file RPC already in flight\n    // remains intentionally unkillable for device/RPC safety.\n}\n\nvoid cacheDropQueuedArt(void)\n'''
if text.count(old) != 1:
    raise SystemExit('focus globals insertion target did not match exactly once')
text = text.replace(old, new, 1)

old = '''    if (cache == NULL || list == NULL || cacheId == NULL || UID == NULL || value == NULL || value[0] == '\\0')\n        return NULL;\n\n    // NO memo lookup here. This function runs for EVERY art element on screen EVERY frame --\n'''
new = '''    if (cache == NULL || list == NULL || cacheId == NULL || UID == NULL || value == NULL || value[0] == '\\0')\n        return NULL;\n\n    // A priority request is the highlighted cover. Move the global focus before ANY cache fast-path\n    // return so even a cover that is already resident immediately invalidates queued lookahead from\n    // the selection we just left.\n    if (isPriority)\n        artUpdateFocus(list, value);\n\n    // NO memo lookup here. This function runs for EVERY art element on screen EVERY frame --\n'''
if text.count(old) != 1:
    raise SystemExit('focus update call target did not match exactly once')
text = text.replace(old, new, 1)

old = '''                    if (isPriority) {\n                        load_image_request_t *req = (load_image_request_t *)entry->qr;\n                        if (req != NULL && !(req->sio2 && gArtNavActive))\n                            artPromote(req);\n                    }\n'''
new = '''                    if (isPriority) {\n                        load_image_request_t *req = (load_image_request_t *)entry->qr;\n                        if (req != NULL) {\n                            // A warmed neighbor can become the selected row while still queued/in-flight.\n                            // Adopt that exact request into the new focus generation instead of throwing it away.\n                            req->focusEpoch = gArtFocusEpoch;\n                            if (!(req->sio2 && gArtNavActive))\n                                artPromote(req);\n                        }\n                    }\n'''
if text.count(old) != 2:
    raise SystemExit(f'priority in-flight adoption target matched {text.count(old)} times, expected 2')
text = text.replace(old, new)

old = '''        req->epoch = gArtEpoch;         // the view this cover belongs to; see cacheDropQueuedArt()\n        req->failEpoch = gArtFailEpoch; // the generation an "absent" answer would belong to\n        req->abortRequested = 0;\n'''
new = '''        req->epoch = gArtEpoch;         // the view this cover belongs to; see cacheDropQueuedArt()\n        req->failEpoch = gArtFailEpoch; // the generation an "absent" answer would belong to\n        req->focusEpoch = gArtFocusEpoch; // selected-game neighborhood this speculative read serves\n        req->abortRequested = 0;\n'''
if text.count(old) != 1:
    raise SystemExit('focusEpoch enqueue target did not match exactly once')
text = text.replace(old, new, 1)

old = '''    // CANCELLATION. The queue is FIFO and a scroll enqueues a cover per row it passes, so by the\n'''
new = '''    // SELECTION-GENERATION CANCELLATION. Cache-sized lookahead intentionally queues deeply so the\n    // next covers can already be waiting, but a new selection makes the PREVIOUS neighborhood\n    // speculative waste. Drop it immediately before device IO instead of waiting up to\n    // ART_CANCEL_STALE_FRAMES for row draw stamps to age out. If this request itself became selected,\n    // the priority reunion path updated focusEpoch before promotion, so it survives.\n    if (req->focusEpoch != gArtFocusEpoch) {\n        cacheClearItem(req->entry, 0);\n        artReleaseRequest(req, 0);\n        return;\n    }\n\n    // CANCELLATION. The queue is FIFO and a scroll enqueues a cover per row it passes, so by the\n'''
if text.count(old) != 1:
    raise SystemExit('focus stale-drop target did not match exactly once')
text = text.replace(old, new, 1)

old = '''    gArtQueuedCount = 0;\n    gArtActiveCount = 0;\n    gArtNavActive = 0;\n\n    gArtThread.attr = 0;\n'''
new = '''    gArtQueuedCount = 0;\n    gArtActiveCount = 0;\n    gArtNavActive = 0;\n    gArtFocusEpoch = 1;\n    gArtFocusList = NULL;\n    gArtFocusHash = 0;\n    gArtFocusLen = 0;\n\n    gArtThread.attr = 0;\n'''
if text.count(old) != 1:
    raise SystemExit('focus reset target did not match exactly once')
text = text.replace(old, new, 1)

p.write_text(text)
PY

git diff --check -- src/texcache.c
git add src/texcache.c
git commit -m "rebuild-212: discard obsolete art neighborhoods on navigation"

# -----------------------------------------------------------------------------
# C. Reuse the browse-prefetched game config on X/Triangle, and make Info #Size
#    wait for BGM reserve instead of competing with audio for USB/device IO.
# -----------------------------------------------------------------------------
python3 - <<'PY'
from pathlib import Path
p = Path('src/menusys.c')
text = p.read_text()

old = '''#include "include/bdmsupport.h"   // bdmSupportIsUDPBD() -- UDPBD games are Neutrino-only\n#include <assert.h>\n'''
new = '''#include "include/bdmsupport.h"   // bdmSupportIsUDPBD() -- UDPBD games are Neutrino-only\n#include <assert.h>\n#include <delaythread.h>\n'''
if text.count(old) != 1:
    raise SystemExit('delaythread include target did not match exactly once')
text = text.replace(old, new, 1)

old = '''static int itemConfigId;\nstatic config_set_t *itemConfig;\n'''
new = '''static int itemConfigId;\nstatic config_set_t *itemConfig;\n// Owner of itemConfigId, including while an async read is in flight. IDs are only indices within a\n// list, so (id == id) across two tabs is not the same game/config. This also lets X/Triangle safely\n// reuse the browse-prefetched config instead of forcing a second device read.\nstatic item_list_t *itemConfigList;\n'''
if text.count(old) != 1:
    raise SystemExit('itemConfigList declaration target did not match exactly once')
text = text.replace(old, new, 1)

old = '''    if (!itemConfig) {\n        item_list_t *list = selected_item->item->userdata;\n        itemConfig = list->itemGetConfig(list, itemConfigId);\n    }\n'''
new = '''    if (!itemConfig) {\n        item_list_t *list = selected_item->item->userdata;\n        itemConfig = list->itemGetConfig(list, itemConfigId);\n        if (itemConfig != NULL)\n            itemConfigList = list;\n    }\n'''
if text.count(old) != 1:
    raise SystemExit('_menuLoadConfig owner target did not match exactly once')
text = text.replace(old, new, 1)

old = '''    if (loadedConfig != NULL && itemConfig == NULL && itemConfigId == configId &&\n        selected_item != NULL && selected_item->item != NULL && selected_item->item->userdata == list) {\n        itemConfig = loadedConfig;\n        loadedConfig = NULL;\n    }\n'''
new = '''    if (loadedConfig != NULL && itemConfig == NULL && itemConfigId == configId && itemConfigList == list &&\n        selected_item != NULL && selected_item->item != NULL && selected_item->item->userdata == list) {\n        itemConfig = loadedConfig;\n        loadedConfig = NULL;\n    }\n'''
if text.count(old) != 1:
    raise SystemExit('async config owner publish target did not match exactly once')
text = text.replace(old, new, 1)

old = '''    WaitSema(menuSemaId);\n    if (selected_item != NULL && selected_item->item != NULL && selected_item->item->current != NULL && itemConfigId >= 0 && itemConfigId == selected_item->item->current->item.id) {\n        list = selected_item->item->userdata;\n        configId = itemConfigId;\n    }\n    SignalSema(menuSemaId);\n\n    if (list == NULL || configId < 0)\n        return;\n\n    // itemGetConfig runs OUTSIDE the semaphore on purpose: it is the slow call (a CFG read plus the\n    // ISO stat this whole mechanism exists to defer), and holding menuSemaId across it would block\n    // the GUI thread for exactly as long as the stat we are trying to keep off the scroll path.\n    sbSetConfigStatSize(1);\n'''
new = '''    WaitSema(menuSemaId);\n    if (selected_item != NULL && selected_item->item != NULL && selected_item->item->current != NULL &&\n        itemConfigId >= 0 && itemConfigId == selected_item->item->current->item.id &&\n        itemConfigList == selected_item->item->userdata) {\n        list = selected_item->item->userdata;\n        configId = itemConfigId;\n    }\n    SignalSema(menuSemaId);\n\n    if (list == NULL || configId < 0)\n        return;\n\n    // #Size is cosmetic/discretionary IO. ART already yields when BGM reserve is critical, but this\n    // path did not: repeatedly entering Info on USB could still make a CFG+stat compete with the\n    // decoder until audio stopped. Wait in short sleeps, and abandon the work entirely if the user\n    // has moved away while audio refills. BGM does its own file IO, so sleeping the general IO worker\n    // here does not prevent the reserve from recovering.\n    while (!bgmDiscretionaryIoAllowed()) {\n        int stillWanted;\n        DelayThread(10 * 1000);\n        WaitSema(menuSemaId);\n        stillWanted = selected_item != NULL && selected_item->item != NULL &&\n                      selected_item->item->current != NULL && itemConfigId == configId &&\n                      itemConfigList == list && itemConfigId == selected_item->item->current->item.id &&\n                      selected_item->item->userdata == list;\n        SignalSema(menuSemaId);\n        if (!stillWanted)\n            return;\n    }\n\n    // itemGetConfig runs OUTSIDE the semaphore on purpose: it is the slow call (a CFG read plus the\n    // ISO stat this whole mechanism exists to defer), and holding menuSemaId across it would block\n    // the GUI thread for exactly as long as the stat we are trying to keep off the scroll path.\n    sbSetConfigStatSize(1);\n'''
if text.count(old) != 1:
    raise SystemExit('Info BGM gate target did not match exactly once')
text = text.replace(old, new, 1)

old = '''    if (selected_item != NULL && selected_item->item != NULL && selected_item->item->current != NULL && itemConfigId == configId && itemConfigId == selected_item->item->current->item.id) {\n        if (itemConfig != NULL)\n            configFree(itemConfig);\n        itemConfig = loadedConfig;\n        loadedConfig = NULL;\n    }\n'''
new = '''    if (selected_item != NULL && selected_item->item != NULL && selected_item->item->current != NULL &&\n        itemConfigId == configId && itemConfigList == list && itemConfigId == selected_item->item->current->item.id &&\n        selected_item->item->userdata == list) {\n        if (itemConfig != NULL)\n            configFree(itemConfig);\n        itemConfig = loadedConfig;\n        itemConfigList = list;\n        loadedConfig = NULL;\n    }\n'''
if text.count(old) != 1:
    raise SystemExit('Info owner publish target did not match exactly once')
text = text.replace(old, new, 1)

old = '''    } else if (itemConfigId != selected_item->item->current->item.id) {\n        if (itemConfig) {\n            configFree(itemConfig);\n            itemConfig = NULL;\n        }\n        item_list_t *list = selected_item->item->userdata;\n        if (itemConfigId == -1 || guiInactiveFrames >= list->delay) {\n            itemConfigId = selected_item->item->current->item.id;\n            shouldQueueLoad = 1;\n        } else\n            actionStatus = 0; // still settling: nothing queued, so release the waiter now\n    } else if (itemConfig == NULL && actionStatus != 0) {\n'''
new = '''    } else if (itemConfigId != selected_item->item->current->item.id ||\n               itemConfigList != selected_item->item->userdata) {\n        if (itemConfig) {\n            configFree(itemConfig);\n            itemConfig = NULL;\n        }\n        item_list_t *list = selected_item->item->userdata;\n        if (itemConfigId == -1 || guiInactiveFrames >= list->delay) {\n            itemConfigId = selected_item->item->current->item.id;\n            itemConfigList = list;\n            shouldQueueLoad = 1;\n        } else\n            actionStatus = 0; // still settling: nothing queued, so release the waiter now\n    } else if (itemConfig == NULL && actionStatus != 0) {\n'''
if text.count(old) != 1:
    raise SystemExit('_menuRequestConfig identity target did not match exactly once')
text = text.replace(old, new, 1)

old = '''        if (itemConfig == NULL)\n            itemConfigId = -1; // let the next pass retry rather than caching a row that never loaded\n        actionStatus = 0;\n'''
new = '''        if (itemConfig == NULL) {\n            itemConfigId = -1; // let the next pass retry rather than caching a row that never loaded\n            itemConfigList = NULL;\n        }\n        actionStatus = 0;\n'''
if text.count(old) != 1:
    raise SystemExit('queue-failure owner reset target did not match exactly once')
text = text.replace(old, new, 1)

old = '''config_set_t *menuLoadConfig()\n{\n    actionStatus = 1;\n    itemConfigId = -1;\n    guiHandleDeferedIO(&actionStatus, _l(_STR_LOADING_SETTINGS), IO_CUSTOM_SIMPLEACTION, &_menuRequestConfig, OPL_DEFERRED_IO_TIMEOUT_MS);\n    return itemConfig;\n}\n\n// we don't want a pop up when transitioning to or refreshing Game Menu gui.\nconfig_set_t *gameMenuLoadConfig(struct UIItem *ui)\n{\n    actionStatus = 1;\n    itemConfigId = -1;\n    guiGameHandleDeferedIO(&actionStatus, ui, IO_CUSTOM_SIMPLEACTION, &_menuRequestConfig);\n    return itemConfig;\n}\n'''
new = '''static config_set_t *menuGetCachedCurrentConfig(void)\n{\n    config_set_t *cached = NULL;\n\n    WaitSema(menuSemaId);\n    if (itemConfig != NULL && selected_item != NULL && selected_item->item != NULL &&\n        selected_item->item->current != NULL && itemConfigId == selected_item->item->current->item.id &&\n        itemConfigList == selected_item->item->userdata)\n        cached = itemConfig;\n    SignalSema(menuSemaId);\n\n    return cached;\n}\n\nconfig_set_t *menuLoadConfig()\n{\n    config_set_t *cached = menuGetCachedCurrentConfig();\n    if (cached != NULL)\n        return cached; // browse-time prefetch already paid the device read\n\n    actionStatus = 1;\n    guiHandleDeferedIO(&actionStatus, _l(_STR_LOADING_SETTINGS), IO_CUSTOM_SIMPLEACTION, &_menuRequestConfig, OPL_DEFERRED_IO_TIMEOUT_MS);\n    return itemConfig;\n}\n\n// we don't want a pop up when transitioning to or refreshing Game Menu gui.\nconfig_set_t *gameMenuLoadConfig(struct UIItem *ui)\n{\n    config_set_t *cached = menuGetCachedCurrentConfig();\n    if (cached != NULL)\n        return cached; // same fast path, without showing a fake loading phase\n\n    actionStatus = 1;\n    guiGameHandleDeferedIO(&actionStatus, ui, IO_CUSTOM_SIMPLEACTION, &_menuRequestConfig);\n    return itemConfig;\n}\n'''
if text.count(old) != 1:
    raise SystemExit('config dialog fast-path target did not match exactly once')
text = text.replace(old, new, 1)

old = '''    itemConfigId = -1;\n    itemConfig = NULL;\n    mainMenu = NULL;\n'''
new = '''    itemConfigId = -1;\n    itemConfig = NULL;\n    itemConfigList = NULL;\n    mainMenu = NULL;\n'''
if text.count(old) != 1:
    raise SystemExit('menuInit owner reset target did not match exactly once')
text = text.replace(old, new, 1)

p.write_text(text)
PY

git diff --check -- src/menusys.c
git add src/menusys.c
git commit -m "rebuild-212: reuse prefetched game config and protect BGM info IO"

# Record the hardware-driven follow-up in the branch handoff without changing production behavior.
cat >> HANDOFF.md <<'EOF'

### Step 212 hardware follow-up — APA lists, rolling art focus, config latency

- APA HDL/VCD enumeration no longer hard-stops solely because the combined persistent-PFS support latch is incomplete; the read-only scanners get a chance to report actual availability after the normal support retry.
- Cache-sized cover prefetch remains aggressive, but queued requests now carry a selected-game focus generation. Moving the cursor discards the old speculative neighborhood before another read; a warmed request that becomes selected is adopted into the new generation.
- Native OPL game selection reuses a browse-prefetched per-game config when the exact list+row is already resident instead of invalidating and rereading it just to enter/launch.
- Info-screen #Size resolution is discretionary storage IO and waits for BGM reserve to recover, abandoning stale work if the user navigates away.
- Hardware retest priorities: APA HDL + HDD VCD visibility; long loose-PNG scrolling without batch stalls; USB BGM while repeatedly entering Info; native-core X/Triangle config latency on settled vs immediate selections.
EOF

git diff --check -- HANDOFF.md
git add HANDOFF.md
git commit -m "docs: extend Step 212 hardware matrix"

git push origin HEAD:rebuild/step-212-apa-boot-and-bgm-resilience
