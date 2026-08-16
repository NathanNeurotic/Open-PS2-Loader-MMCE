#!/usr/bin/env bash
set -euo pipefail

git config user.name "NathanNeurotic Step Builder"
git config user.email "actions@users.noreply.github.com"
git fetch origin rebuild/step-212-apa-boot-and-bgm-resilience
git checkout rebuild/step-212-apa-boot-and-bgm-resilience
test "$(git rev-parse HEAD)" = "3d44a21d5ca5223d861b1baa0baea6f43be4a926"

python3 - <<'PY'
from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    text = p.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected exactly one match, found {count}")
    p.write_text(text.replace(old, new, 1))


replace_once(
    "src/themes.c",
    """        int isBackground = (drawElem->type == ELEM_TYPE_BACKGROUND);\n        GSTEXTURE *texture;\n""",
    """        int isBackground = (drawElem->type == ELEM_TYPE_BACKGROUND);\n        GSTEXTURE *texture;\n        int coverflowCoverSettled = 1;\n""",
)

replace_once(
    "src/themes.c",
    """                } else if (gTheme->coverflow != NULL) {\n                    struct theme_element *cfElem = thmGetElemForItem(menu, item, gTheme->coverflow);\n                    if (cfElem != NULL && cfElem->extended != NULL) {\n                        mutable_image_t *cfImg = (mutable_image_t *)cfElem->extended;\n                        if (cfImg != NULL && cfImg->cache != NULL)\n                            getGameImageTextureEx(cfImg->cache, menu->item->userdata, &item->item, 1);\n                    }\n                }\n""",
    """                } else if (gTheme->coverflow != NULL) {\n                    struct theme_element *cfElem = thmGetElemForItem(menu, item, gTheme->coverflow);\n                    if (cfElem != NULL && cfElem->extended != NULL) {\n                        mutable_image_t *cfImg = (mutable_image_t *)cfElem->extended;\n                        if (cfImg != NULL && cfImg->cache != NULL) {\n                            GSTEXTURE *coverTexture = getGameImageTextureEx(cfImg->cache, menu->item->userdata, &item->item, 1);\n\n                            // Coverflow draws after the Background element. If its selected cover is still\n                            // pending, starting a full-screen BG read here would occupy the art worker before\n                            // the carousel gets to enqueue its visible neighbours. Hold only this background\n                            // request for that short window. A loaded cover, disabled art, or a confirmed\n                            // absent cover (-2) releases the gate immediately.\n                            if (gEnableArt && coverTexture == NULL && cfImg->cache->userId >= 0 &&\n                                cfImg->cache->userId < gTheme->gameCacheCount && item->item.cache_id != NULL &&\n                                item->item.cache_id[cfImg->cache->userId] != -2)\n                                coverflowCoverSettled = 0;\n                        }\n                    }\n                }\n""",
)

replace_once(
    "src/themes.c",
    """            // Match master: request the background texture immediately on the first frame without\n            // idle-frame deferral, giving instant master-style background loading.\n            texture = getGameImageTexture(gameImage->cache, menu->item->userdata, &item->item);\n""",
    """            // List mode keeps master's immediate background request. Coverflow is different: its\n            // Background element is painted before the carousel, so on a cold selection the large BG read\n            // could start before the side covers even reach the queue. Draw an already-cached background\n            // while the selected cover settles; the same frame's Coverflow draw then gets first claim on\n            // visible-neighbour reads. This is admission ordering, not an artificial frame delay.\n            if (coverflowCoverSettled)\n                texture = getGameImageTexture(gameImage->cache, menu->item->userdata, &item->item);\n            else\n                texture = getGameImageCached(gameImage->cache, &item->item);\n""",
)

handoff = Path("HANDOFF.md")
text = handoff.read_text()
note = """

### Step 212 follow-up — Coverflow background admission

USB hardware feedback on Beta-2587 (`172569c`) confirmed that background-art throughput and BGM
continuity were fixed, while Coverflow side-cover arrival became the remaining visible slowdown.
Coverflow's Background element draws before the carousel; after prioritizing the selected cover it
could still start a much larger `_BG` read before `drawCoverFlow()` had a chance to enqueue the
visible neighbouring covers. Step 212 now suppresses only that new background enqueue while the
selected Coverflow cover is still pending. The cached background may continue drawing; once the
selected cover is loaded, art is disabled, or the cover is confirmed absent, background admission
returns to the immediate path. List mode is unchanged. This preserves BGM priority and the dedicated
art-worker design while ordering USB device work around what the user is visibly waiting for.
"""
if "### Step 212 follow-up — Coverflow background admission" in text:
    raise SystemExit("HANDOFF.md: follow-up note already present")
handoff.write_text(text.rstrip() + note + "\n")
PY

git diff --check
git add src/themes.c HANDOFF.md
git commit -m "rebuild-212: keep Coverflow covers ahead of background reads" -m "Use Coverflow-specific background admission rather than another global delay. When the selected carousel cover is still pending, draw only an already-cached background and let drawCoverFlow enqueue visible neighbours before a large _BG read can occupy the art worker. Loaded, disabled, or confirmed-absent covers release the gate immediately; List mode remains unchanged. This follows USB hardware feedback that Beta-2587 fixed background speed and BGM continuity while Coverflow cover arrival remained slower."
git push origin HEAD:rebuild/step-212-apa-boot-and-bgm-resilience
