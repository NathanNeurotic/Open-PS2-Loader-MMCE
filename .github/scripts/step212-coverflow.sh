#!/usr/bin/env bash
set -euo pipefail

git config user.name "NathanNeurotic Step Builder"
git config user.email "actions@users.noreply.github.com"
git fetch origin rebuild/step-212-apa-boot-and-bgm-resilience
git checkout rebuild/step-212-apa-boot-and-bgm-resilience
test "$(git rev-parse HEAD)" = "798f20d6e8bd4e024440cb058413a5c7a48f337d"

# -----------------------------------------------------------------------------
# Commit 1: APA game enumeration must not depend on the persistent config/art PFS home.
# -----------------------------------------------------------------------------
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
    "src/hddsupport.c",
    """    // Fail closed when no existing PFS data home could be mounted. The update path must not\n    // construct filenames from a NULL prefix or fall into any alternate partition-creation path.\n    if (gHDDPrefix == NULL)\n        return 0;\n""",
    """    // Game discovery and the persistent config/art PFS home are separate lifetimes. HDL games\n    // live in the APA table and HDD VCDs live in existing POPS partitions; neither requires the\n    // long-lived pfs0: data home. Step 211 accidentally returned an empty page whenever that home\n    // was unavailable even though the APA/PFS support modules were healthy. Require only the support\n    // stack here; config/art accessors below remain NULL-safe and fail closed independently.\n    if (!hddSupportModulesLoaded)\n        return 0;\n""",
)

replace_once(
    "src/hddsupport.c",
    """    hddSupportErrToasted = 0;\n    if (gOPLPart[5] != '+') {\n        hddCheckOPLFolder(hddPrefix);\n        gHDDPrefix = \"pfs0:OPL/\";\n    } else {\n        gHDDPrefix = \"pfs0:\";\n    }\n}\n""",
    """    hddSupportErrToasted = 0;\n    if (gOPLPart[5] != '+') {\n        hddCheckOPLFolder(hddPrefix);\n        gHDDPrefix = \"pfs0:OPL/\";\n    } else {\n        gHDDPrefix = \"pfs0:\";\n    }\n\n    // A prior list pass may have parked relative HDD artwork as unavailable while no persistent\n    // PFS home existed. Re-arm those misses exactly when the home becomes usable again.\n    cacheInvalidateFailMemo();\n}\n""",
)

replace_once(
    "src/hddsupport.c",
    """static config_set_t *hddGetConfig(item_list_t *itemList, int id)\n{\n    char path[256];\n\n    // VCD (PS1) view: `id` indexes hddVcdGames, NOT hddGames (which stays at its zero/ISO-view state --\n    // {games=NULL,count=0} on a PS1-only HDD). Mirror the other VCD-aware accessors and key the per-game\n    // CFG off the VCD basename, instead of dereferencing &hddGames.games[id] off a NULL base (crash).\n    if (vcdViewActive(itemList->mode)) {\n        base_game_info_t *g = hddActiveVcd(id); // toggle-window safe: empty entry on a stale id (no OOB)\n        return sbPopulateConfig(g, gHDDPrefix, \"/\");\n    }\n\n    hdl_game_info_t *game = hddActiveHdl(id); // toggle-window safe: empty entry on a stale id (no OOB)\n\n    snprintf(path, sizeof(path), \"%sCFG/%s.cfg\", gHDDPrefix, game->startup);\n    config_set_t *config = configAlloc(0, NULL, path);\n    configRead(config); // Does not matter if the config file exists or not.\n\n    configSetStr(config, CONFIG_ITEM_NAME, game->name);\n    configSetInt(config, CONFIG_ITEM_SIZE, game->total_size_in_kb >> 10);\n    configSetStr(config, CONFIG_ITEM_FORMAT, \"HDL\");\n    // HDD bypasses sbPopulateConfig, so set #System/#Media/#DiscType via the shared helper (FR #49) --\n    // without it a theme's #DiscType / #System AttributeImage badge never rendered on HDD games. HDL\n    // titles are always PS2; reuse game->disctype for the CD-vs-DVD axis.\n    sbSetDiscAttributes(config, 0, game->disctype == SCECdPS2CD);\n    configSetStr(config, CONFIG_ITEM_STARTUP, game->startup);\n\n    return config;\n}\n""",
    """static config_set_t *hddGetConfig(item_list_t *itemList, int id)\n{\n    char path[256];\n    char vcdId[VCD_ID_MAX];\n    config_set_t *config;\n\n    // VCD (PS1) view: `id` indexes hddVcdGames, NOT hddGames. The list itself is discoverable from\n    // APA even when the persistent config/art PFS home is temporarily unavailable. In that state\n    // return a runtime-only config instead of dereferencing a NULL prefix or hiding the whole list.\n    if (vcdViewActive(itemList->mode)) {\n        base_game_info_t *g = hddActiveVcd(id);\n        if (gHDDPrefix != NULL)\n            return sbPopulateConfig(g, gHDDPrefix, \"/\");\n\n        config = configAlloc(0, NULL, NULL);\n        if (config == NULL)\n            return NULL;\n        configSetStr(config, CONFIG_ITEM_NAME, g->name);\n        configSetInt(config, CONFIG_ITEM_SIZE, 0);\n        configSetStr(config, CONFIG_ITEM_FORMAT, \"VCD\");\n        sbSetDiscAttributes(config, 1, 1);\n        configSetStr(config, CONFIG_ITEM_STARTUP,\n                     vcdExtractGameId(g->name, vcdId, sizeof(vcdId)) ? vcdId : g->name);\n        return config;\n    }\n\n    hdl_game_info_t *game = hddActiveHdl(id);\n\n    if (gHDDPrefix != NULL) {\n        snprintf(path, sizeof(path), \"%sCFG/%s.cfg\", gHDDPrefix, game->startup);\n        config = configAlloc(0, NULL, path);\n        if (config != NULL)\n            configRead(config); // Does not matter if the config file exists or not.\n    } else {\n        config = configAlloc(0, NULL, NULL); // metadata only; no unsafe/imaginary save destination\n    }\n    if (config == NULL)\n        return NULL;\n\n    configSetStr(config, CONFIG_ITEM_NAME, game->name);\n    configSetInt(config, CONFIG_ITEM_SIZE, game->total_size_in_kb >> 10);\n    configSetStr(config, CONFIG_ITEM_FORMAT, \"HDL\");\n    sbSetDiscAttributes(config, 0, game->disctype == SCECdPS2CD);\n    configSetStr(config, CONFIG_ITEM_STARTUP, game->startup);\n\n    return config;\n}\n""",
)

replace_once(
    "src/hddsupport.c",
    """static int hddGetImage(item_list_t *itemList, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)\n{\n    char path[256];\n\n    // PS1 (VCD) art uses this same ART path as PS2. The cache supplies the filename first and may retry\n""",
    """static int hddGetImage(item_list_t *itemList, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)\n{\n    char path[256];\n\n    // The APA/HDL list is allowed to exist without a persistent PFS data home, but relative ART is\n    // not. Park the lookup cheaply instead of formatting a path through NULL; a later successful\n    // data-home mount invalidates the miss generation above and re-arms artwork. Absolute theme\n    // image requests remain independent and are still allowed.\n    if (isRelative && gHDDPrefix == NULL)\n        return ERR_BAD_FILE;\n\n    // PS1 (VCD) art uses this same ART path as PS2. The cache supplies the filename first and may retry\n""",
)

replace_once(
    "src/hddsupport.c",
    """    // Not found: restore the default OPL data-partition mount so normal HDD IO keeps working.\n    fileXioUmount(hddPrefix);\n    fileXioMount(hddPrefix, gOPLPart, FIO_MT_RDWR);\n    return 0;\n}\n""",
    """    // Not found: restore the default OPL data-partition mount when one exists. A list-only\n    // APA session may legitimately have no persistent data home, in which case there is nothing\n    // truthful to remount and an empty gOPLPart must never be passed to fileXioMount.\n    fileXioUmount(hddPrefix);\n    if (gOPLPart[0] != '\\0')\n        fileXioMount(hddPrefix, gOPLPart, FIO_MT_RDWR);\n    return 0;\n}\n""",
)

replace_once(
    "src/hddsupport.c",
    """    if ((result = sbLoadCheats(gHDDPrefix, game->startup)) < 0) {\n        // #265: let the user back out instead of sitting through the whole load. The helper does\n        // the sbUnprepare itself -- see include/supportbase.h; skipping it breaks the NEXT launch.\n        // `settings` is not assigned until below, so derive the common block from the IRX base.\n        if (!sbCheatsMissingContinue((u8 *)irx + i, result))\n            return;\n    }\n    sbLoadImage(gHDDPrefix, game->startup);\n""",
    """    if (gHDDPrefix != NULL) {\n        if ((result = sbLoadCheats(gHDDPrefix, game->startup)) < 0) {\n            // #265: let the user back out instead of sitting through the whole load. The helper does\n            // the sbUnprepare itself -- see include/supportbase.h; skipping it breaks the NEXT launch.\n            // `settings` is not assigned until below, so derive the common block from the IRX base.\n            if (!sbCheatsMissingContinue((u8 *)irx + i, result))\n                return;\n        }\n        sbLoadImage(gHDDPrefix, game->startup);\n    } else {\n        LOG("HDDSUPPORT launch: no persistent PFS data home; skipping HDD cheats/image sidecars\\n");\n    }\n""",
)

replace_once(
    "src/hddsupport.c",
    """static int hddCheckVMC(item_list_t *itemList, char *name, int createSize)\n{\n    return sysCheckVMC(gHDDPrefix, \"/\", name, createSize, NULL);\n}\n""",
    """static int hddCheckVMC(item_list_t *itemList, char *name, int createSize)\n{\n    return gHDDPrefix != NULL ? sysCheckVMC(gHDDPrefix, \"/\", name, createSize, NULL) : -1;\n}\n""",
)

replace_once(
    "src/hddsupport.c",
    """    if (!gHDDGameListCache)\n        return 1;\n\n    hddFreeHDLGamelist(cache);\n""",
    """    if (!gHDDGameListCache || gHDDPrefix == NULL)\n        return 1; // cache is optional PFS data; live APA scanning does not depend on it\n\n    hddFreeHDLGamelist(cache);\n""",
)

replace_once(
    "src/hddsupport.c",
    """    if (!gHDDGameListCache)\n        return 1;\n\n    if (cache->count > 0) {\n""",
    """    if (!gHDDGameListCache || gHDDPrefix == NULL)\n        return 1; // no persistent PFS home: keep the live list in RAM and skip games.bin writes\n\n    if (cache->count > 0) {\n""",
)
PY

git diff --check
git add src/hddsupport.c
git commit -m "rebuild-212: decouple APA game discovery from config PFS home" -m "Restore the correct ownership boundary: HDL PS2 enumeration and HDD VCD partition scanning require the APA/PFS support stack, not a successfully mounted persistent config/art pfs0 home. Keep games.bin, per-game CFG, ART, VMC and sidecar launch reads NULL-safe and fail-closed when that optional data home is unavailable. No APA create/format path is added or restored."

# -----------------------------------------------------------------------------
# Commit 2: preserve the concrete memory-card slot that actually booted OPL.
# -----------------------------------------------------------------------------
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
    "src/opl.c",
    """// Absolute mc-side location for that mirror. A memory card is the only store that needs NO\n// configuration to reach -- mcman is loaded by sysReset before anything else, no network, no\n// transport flags, no ordering. Composed from sysCheckMC() so it names the slot that actually holds\n// a card. Returns 0 when there is none.\nstatic int mcConfigPathRedirect(char *out, int outLen)\n{\n    int slot = sysCheckMC();\n\n    if (slot < 0)\n        return 0;\n\n    snprintf(out, outLen, \"mc%d:/OPL/%s\", slot, configPathRedirectFile);\n    return 1;\n}\n""",
    """// Prefer the concrete slot OPL actually booted from. With both cards inserted, a generic\n// first-present probe must not redirect an mc1 boot's bootstrap/config traffic onto mc0. For\n// non-MC boots retain the historical first-present fallback.\nstatic int preferredMcSlot(void)\n{\n    if (!strncmp(gBootDir, \"mc0:\", 4))\n        return 0;\n    if (!strncmp(gBootDir, \"mc1:\", 4))\n        return 1;\n    return sysCheckMC();\n}\n\n// Absolute mc-side location for the bootstrap mirror. A concrete MC boot owns its own slot;\n// network/bare boots use the existing first-present policy. Returns 0 when there is none.\nstatic int mcConfigPathRedirect(char *out, int outLen)\n{\n    int slot = preferredMcSlot();\n\n    if (slot < 0)\n        return 0;\n\n    snprintf(out, outLen, \"mc%d:/OPL/%s\", slot, configPathRedirectFile);\n    return 1;\n}\n""",
)

replace_once(
    "src/opl.c",
    """static int tryMissingConfigPathRecovery(int types)\n{\n    static const char *const mcHomes[] = {\n        \"mc0:/OPL\",\n        \"mc0:/\",\n        \"mc1:/OPL\",\n        \"mc1:/\",\n    };\n    int value;\n\n    // Do not trust sysCheckMC() for recovery: the report that drove this path is specifically a card\n    // that served the ELF but was not selected by the normal card probe. Directly test both slots.\n    for (unsigned int i = 0; i < sizeof(mcHomes) / sizeof(mcHomes[0]); i++) {\n        value = tryReadRecoveryConfigHome(types, mcHomes[i]);\n        if (value & CONFIG_OPL) {\n            restoreRecoverySaveHome(mcHomes[i]);\n            return value;\n        }\n    }\n""",
    """static int tryMissingConfigPathRecovery(int types)\n{\n    int value;\n    int slots[2] = {0, 1};\n    int preferred = preferredMcSlot();\n\n    // A concrete mc1 boot searches mc1 first even when mc0 is also inserted. Otherwise retain the\n    // historical mc0->mc1 recovery order. Both slots are still probed directly; sysCheckMC() is\n    // deliberately not used as a substitute for trying the second card.\n    if (preferred == 1) {\n        slots[0] = 1;\n        slots[1] = 0;\n    }\n    for (int i = 0; i < 2; i++) {\n        char home[16];\n\n        snprintf(home, sizeof(home), \"mc%d:/OPL\", slots[i]);\n        value = tryReadRecoveryConfigHome(types, home);\n        if (value & CONFIG_OPL) {\n            restoreRecoverySaveHome(home);\n            return value;\n        }\n\n        snprintf(home, sizeof(home), \"mc%d:/\", slots[i]);\n        value = tryReadRecoveryConfigHome(types, home);\n        if (value & CONFIG_OPL) {\n            restoreRecoverySaveHome(home);\n            return value;\n        }\n    }\n""",
)

replace_once(
    "src/opl.c",
    """        int homeLeftOnCard = 0;\n        if (sysCheckMC() >= 0) {\n            configSetMove(NULL); // point the config files at the legacy mc?:OPL home\n            value = configReadMulti(types);\n""",
    """        int homeLeftOnCard = 0;\n        if (sysCheckMC() >= 0) {\n            char concreteMcHome[16];\n            if (!strncmp(gBootDir, \"mc0:\", 4) || !strncmp(gBootDir, \"mc1:\", 4)) {\n                snprintf(concreteMcHome, sizeof(concreteMcHome), \"mc%c:/OPL\", gBootDir[2]);\n                configSetMove(concreteMcHome); // MC boot: legacy fallback stays on the booted slot\n            } else {\n                configSetMove(NULL); // non-MC boot: retain the historical mc?:OPL selection\n            }\n            value = configReadMulti(types);\n""",
)
PY

git diff --check
git add src/opl.c
git commit -m "rebuild-212: preserve the booted memory-card slot" -m "Treat mc0 and mc1 as concrete owners when OPL was launched from a memory card. config.path mirrors, missing-config recovery, and legacy mc?:OPL reads now prefer the actual boot slot, so an mc1 launch cannot silently read or bootstrap through mc0 merely because slot 1 is also populated. Non-MC boots retain the existing first-present fallback."

# -----------------------------------------------------------------------------
# Commit 3: make generic List admission use the same bounded center-out workflow as Coverflow.
# -----------------------------------------------------------------------------
python3 - <<'PY'
from pathlib import Path

p = Path("src/themes.c")
text = p.read_text()
start = text.index("static void drawItemsList(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)\n{")
end = text.index("\nstatic void initItemsList(", start)
old = text[start:end]
new = r'''static void drawItemsList(struct menu_list *menu, struct submenu_list *item, config_set_t *config, struct theme_element *elem)
{
    // devices= filter: the nav pointer (gTheme->itemsList) is resolved with the SAME matching rules
    // (thmResolveItemsList), so the rows that draw are always the rows navigation counts.
    if (thmElemSkipsDevice(elem, menu->item->icon_id))
        return;

    if (item) {
        items_list_t *itemsList = (items_list_t *)elem->extended;
        item_list_t *list = menu->item->userdata;

        int posX = elem->posX, posY = elem->posY;
        if (elem->aligned) {
            posX -= elem->width >> 1;
            posY -= elem->height >> 1;
        }

        // List admission now mirrors Coverflow's useful property: selected first, then a small
        // center-out neighbourhood. Merely being visible on a tall List is no longer permission to
        // start a device read. This matters most for loose USB PNGs (a .tar index masks the open/read
        // churn), and fixes the same burst on Games, APPS, Favourites and VCD because they all render
        // through this one function.
        if (itemsList->decoratorImage != NULL && !item->item.isFolder) {
            getGameImageTextureEx(itemsList->decoratorImage->cache, menu->item->userdata, &item->item, 1);
        } else if (itemsList->coverElem != NULL && !item->item.isFolder) {
            mutable_image_t *selImg = (mutable_image_t *)thmGetElemForItem(menu, item, itemsList->coverElem)->extended;
            if (selImg != NULL && selImg->cache != NULL)
                getGameImageTextureEx(selImg->cache, menu->item->userdata, &item->item, 1);
        }

        submenu_list_t *walkSide[2] = {item, item}; // [0] next, [1] previous
        for (int step = 0; step < COVER_WARM_RADIUS; step++) {
            for (int side = 0; side < 2; side++) {
                submenu_list_t *walk = walkSide[side];
                submenu_list_t *next = side ? walk->prev : walk->next;
                if (next == NULL || next == walk)
                    continue;
                walkSide[side] = next;
                if (next->item.isFolder)
                    continue;

                if (itemsList->decoratorImage != NULL) {
                    getGameImageTexture(itemsList->decoratorImage->cache, list, &next->item);
                } else if (itemsList->coverElem != NULL) {
                    mutable_image_t *warmImg = (mutable_image_t *)thmGetElemForItem(menu, next, itemsList->coverElem)->extended;
                    if (warmImg != NULL && warmImg->cache != NULL)
                        getGameImageTexture(warmImg->cache, list, &next->item);
                }
            }
        }

        submenu_list_t *ps = menu->item->pagestart;
        int others = 0;
        u64 color;
        int textEndX = 0;
        while (ps && (others++ < itemsList->displayedItems)) {
            if (ps == item)
                color = gTheme->selTextColor;
            else
                color = elem->color;

            const char *dispText = vcdDisplayName(list ? list->mode : -1, submenuItemGetText(&ps->item));
            char folderBuf[256];
            if (ps->item.isFolder) {
                snprintf(folderBuf, sizeof(folderBuf), "%s/", dispText);
                dispText = folderBuf;
            }

            if (itemsList->decoratorImage) {
                GSTEXTURE *itemIconTex = NULL;

                if (!ps->item.isFolder) {
                    // Admission happened center-out above. Farther visible rows draw only what is
                    // already resident, so a 10-20-row viewport cannot flood USB with loose PNG opens.
                    itemIconTex = getGameImageCached(itemsList->decoratorImage->cache, &ps->item);
                }

                if (itemIconTex && itemIconTex->Mem)
                    rmDrawPixmap(itemIconTex, posX, posY, elem->aligned, DECORATOR_SIZE, DECORATOR_SIZE, elem->scaled, gDefaultCol, 0);
                else if (itemsList->decoratorImage->defaultTexture)
                    rmDrawPixmap(&itemsList->decoratorImage->defaultTexture->source, posX, posY, elem->aligned, DECORATOR_SIZE, DECORATOR_SIZE, elem->scaled, gDefaultCol, 0);

                textEndX = fntRenderString(elem->font, elem->posX + DECORATOR_SIZE, posY, elem->aligned, elem->width, elem->height, dispText, color);
            } else {
                // Decorator-less Lists use the same center-out warming above; their separate COV
                // element draws the selected cover. No additional per-visible-row reads start here.
                textEndX = fntRenderString(elem->font, elem->posX, posY, elem->aligned, elem->width, elem->height, dispText, color);
            }

            if (ps->item.favourited) {
                GSTEXTURE *favTex = thmGetTexture(FAV_MARK);
                if (favTex != NULL && favTex->Mem != NULL)
                    rmDrawPixmap(favTex, textEndX + 4, posY, elem->aligned, MENU_ITEM_HEIGHT, MENU_ITEM_HEIGHT, elem->scaled, gDefaultCol, 0);
            }

            posY += MENU_ITEM_HEIGHT;
            ps = ps->next;
        }
    }
}
'''
p.write_text(text[:start] + new + text[end:])
PY

git diff --check
git add src/themes.c
git commit -m "rebuild-212: bound List artwork around the selection" -m "Apply Coverflow's selected-first, bounded-neighbour admission principle to the generic List renderer. Games, APPS, Favourites and VCD now initiate loose-file cover reads only for the selected row and +/-2 neighbours; farther visible rows draw cached art only. This prevents a tall viewport from flooding USB with PNG opens while preserving already-resident thumbnails."

# -----------------------------------------------------------------------------
# Commit 4: APP browsing must not open each ELF merely to compute #Size.
# -----------------------------------------------------------------------------
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
    "src/supportbase.c",
    """void sbSetConfigStatSize(int enable)\n{\n    sbConfigStatSize = enable;\n}\n\nconfig_set_t *sbPopulateConfig(base_game_info_t *game, const char *prefix, const char *sep)\n""",
    """void sbSetConfigStatSize(int enable)\n{\n    sbConfigStatSize = enable;\n}\n\nint sbConfigStatSizeEnabled(void)\n{\n    return sbConfigStatSize;\n}\n\nconfig_set_t *sbPopulateConfig(base_game_info_t *game, const char *prefix, const char *sep)\n""",
)

replace_once(
    "include/supportbase.h",
    """void sbSetConfigStatSize(int enable);\n""",
    """void sbSetConfigStatSize(int enable);\nint sbConfigStatSizeEnabled(void);\n""",
)

replace_once(
    "src/appsupport.c",
    """        snprintf(tmp, sizeof(tmp), \"%.2f\", appGetELFSize(cur->val));\n        configSetStr(config, CONFIG_ITEM_SIZE, tmp);\n""",
    """        if (sbConfigStatSizeEnabled())\n            snprintf(tmp, sizeof(tmp), \"%.2f\", appGetELFSize(cur->val));\n        else\n            snprintf(tmp, sizeof(tmp), \"0.00\");\n        configSetStr(config, CONFIG_ITEM_SIZE, tmp);\n""",
)

replace_once(
    "src/appsupport.c",
    """        snprintf(tmp, sizeof(tmp), \"%.2f\", appGetELFSize(path));\n        configSetStr(config, CONFIG_ITEM_SIZE, tmp);\n""",
    """        if (sbConfigStatSizeEnabled())\n            snprintf(tmp, sizeof(tmp), \"%.2f\", appGetELFSize(path));\n        else\n            snprintf(tmp, sizeof(tmp), \"0.00\");\n        configSetStr(config, CONFIG_ITEM_SIZE, tmp);\n""",
)
PY

git diff --check
git add src/supportbase.c include/supportbase.h src/appsupport.c
git commit -m "rebuild-212: defer APP ELF size I/O off navigation" -m "Reuse the existing info-screen #Size gate for APPS. Browse-time app config loading no longer opens/seeks every target ELF merely to calculate metadata; the real size is resolved when the info screen explicitly requests it. This also removes inherited metadata pressure from APP favourites."

# -----------------------------------------------------------------------------
# Commit 5: protect the last audio reserve without pushing CPU priority any higher.
# -----------------------------------------------------------------------------
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
    "include/sound.h",
    """int isBgmPlaying(void);\nvoid bgmMute(void);\n""",
    """int isBgmPlaying(void);\n// Discretionary storage users (art, cosmetic metadata) should not START a new read while the\n// decoded BGM reserve is below its low-water mark. Always true when BGM is off/not yet primed.\nint bgmDiscretionaryIoAllowed(void);\nvoid bgmMute(void);\n""",
)

replace_once(
    "src/sound.c",
    """#define BGM_RING_BUFFER_COUNT 192 // 768 KB buffer (~4.35s): extra headroom for long device/list bursts (#364)\n#define BGM_RING_BUFFER_SIZE  4096\n#define BGM_STOP_WAIT_SLICES  16\n""",
    """#define BGM_RING_BUFFER_COUNT       192 // 768 KB buffer (~4.35s): extra headroom for long device/list bursts (#364)\n#define BGM_RING_BUFFER_SIZE        4096\n#define BGM_STOP_WAIT_SLICES        16\n#define BGM_IO_LOW_WATER_CHUNKS     48 // ~1.1 s: stop STARTING discretionary device reads\n#define BGM_IO_RESUME_WATER_CHUNKS  80 // ~1.8 s: hysteresis before artwork/cosmetic IO resumes\n""",
)

replace_once(
    "src/sound.c",
    """static volatile unsigned char bgmThreadRunning, bgmIoThreadRunning;\n\n// Nonzero while bgmIoThread may be inside a device read. Read by the art worst-open latch.\nvolatile int gBgmInRead = 0;\n""",
    """static volatile unsigned char bgmThreadRunning, bgmIoThreadRunning;\nstatic volatile int bgmBufferedChunks = 0;\nstatic volatile unsigned char bgmBufferPrimed = 0;\nstatic volatile unsigned char bgmIoThrottle = 0;\n\n// Nonzero while bgmIoThread may be inside a device read. Read by the art worst-open latch.\nvolatile int gBgmInRead = 0;\n\nstatic void bgmAdjustBufferedChunks(int delta)\n{\n    DIntr();\n    bgmBufferedChunks += delta;\n    if (bgmBufferedChunks < 0)\n        bgmBufferedChunks = 0;\n    else if (bgmBufferedChunks > BGM_RING_BUFFER_COUNT)\n        bgmBufferedChunks = BGM_RING_BUFFER_COUNT;\n    if (bgmBufferedChunks >= BGM_IO_RESUME_WATER_CHUNKS)\n        bgmBufferPrimed = 1;\n    EIntr();\n}\n\nint bgmDiscretionaryIoAllowed(void)\n{\n    if (!bgmIsPlaying || !gEnableBGM || !bgmBufferPrimed)\n        return 1;\n\n    int buffered = bgmBufferedChunks;\n    if (bgmIoThrottle) {\n        if (buffered >= BGM_IO_RESUME_WATER_CHUNKS)\n            bgmIoThrottle = 0;\n    } else if (buffered <= BGM_IO_LOW_WATER_CHUNKS) {\n        bgmIoThrottle = 1;\n    }\n\n    return !bgmIoThrottle;\n}\n""",
)

replace_once(
    "src/sound.c",
    """        audsrv_wait_audio(BGM_RING_BUFFER_SIZE);\n        audsrv_play_audio(bgmBuffer[rdPtr], BGM_RING_BUFFER_SIZE);\n        rdPtr = (rdPtr + 1) % BGM_RING_BUFFER_COUNT;\n\n        SignalSema(inSema);\n""",
    """        bgmAdjustBufferedChunks(-1);\n        audsrv_wait_audio(BGM_RING_BUFFER_SIZE);\n        audsrv_play_audio(bgmBuffer[rdPtr], BGM_RING_BUFFER_SIZE);\n        rdPtr = (rdPtr + 1) % BGM_RING_BUFFER_COUNT;\n\n        SignalSema(inSema);\n""",
)

replace_once(
    "src/sound.c",
    """            if (ret > 0) {\n                bufferPtr += ret;\n                decodeTotal -= ret;\n            } else if (ret < 0) {\n                LOG(\"BGM: I/O error while reading.\\n\");\n                terminateFlag = 1;\n                break;\n            } else if (ret == 0) {\n""",
    """            if (ret > 0) {\n                bufferPtr += ret;\n                decodeTotal -= ret;\n            } else if (ret == OV_HOLE) {\n                // Recoverable Vorbis discontinuity. libvorbisfile explicitly permits callers to\n                // continue after OV_HOLE; treating it as a fatal device error made one stressed\n                // read permanently stop BGM instead of recovering on the next packet.\n                LOG(\"BGM: recoverable Vorbis hole; continuing.\\n\");\n                continue;\n            } else if (ret < 0) {\n                LOG(\"BGM: fatal I/O/decode error while reading (%d).\\n\", ret);\n                terminateFlag = 1;\n                break;\n            } else if (ret == 0) {\n""",
)

replace_once(
    "src/sound.c",
    """        wrPtr = (wrPtr + 1) % BGM_RING_BUFFER_COUNT;\n        SignalSema(outSema);\n""",
    """        wrPtr = (wrPtr + 1) % BGM_RING_BUFFER_COUNT;\n        bgmAdjustBufferedChunks(1);\n        SignalSema(outSema);\n""",
)

replace_once(
    "src/sound.c",
    """    bgmThreadRunning = 0;\n    bgmIoThreadRunning = 0;\n\n    sema.max_count = BGM_RING_BUFFER_COUNT;\n""",
    """    bgmThreadRunning = 0;\n    bgmIoThreadRunning = 0;\n    bgmBufferedChunks = 0;\n    bgmBufferPrimed = 0;\n    bgmIoThrottle = 0;\n\n    sema.max_count = BGM_RING_BUFFER_COUNT;\n""",
)

replace_once(
    "src/texcache.c",
    """#include \"include/pad.h\"        // getKeyPressed -- the SIO2 nav gate, GUI thread only\n#include \"include/bdmsupport.h\" // bdmModeIsSIO2 -- is this cover on the pad's bus?\n""",
    """#include \"include/pad.h\"        // getKeyPressed -- the SIO2 nav gate, GUI thread only\n#include \"include/sound.h\"      // BGM low-water admission: don't start art while audio reserve is critical\n#include \"include/bdmsupport.h\" // bdmModeIsSIO2 -- is this cover on the pad's bus?\n""",
)

replace_once(
    "src/texcache.c",
    """            if (!req)\n                break; // queue empty, or its head is an SIO2 cover deferred while a direction is held\n\n            cacheLoadImage(req);\n\n            // Yield the ready queue so other threads (such as BGM I/O) can process between loads.\n""",
    """            if (!req)\n                break; // queue empty, or its head is an SIO2 cover deferred while a direction is held\n\n            // Device-level backpressure, not another CPU-priority tweak. Finish any read already in\n            // flight, but while BGM's decoded reserve is critical do not START the next PNG open.\n            // The request remains owned by this worker and cacheLoadImage re-checks stale/abort state\n            // after the wait, so scrolling can still make this work evaporate before it touches disk.\n            while (!gArtTerminate && !req->abortRequested && !bgmDiscretionaryIoAllowed())\n                DelayThread(10 * 1000);\n\n            cacheLoadImage(req);\n\n            // Yield the ready queue so other threads (such as BGM I/O) can process between loads.\n""",
)
PY

git diff --check
git add include/sound.h src/sound.c src/texcache.c
git commit -m "rebuild-212: protect BGM low-water reserve from new art reads" -m "Keep the proven 30/31 audio priorities and 768 KiB ring, then add device-level admission: after BGM is primed, the art worker pauses before starting a new image read below ~1.1 s buffered and resumes above ~1.8 s. Existing reads are never cancelled mid-RPC. Also treat libvorbisfile OV_HOLE as recoverable instead of permanently killing playback."

# -----------------------------------------------------------------------------
# Commit 6: VCD deep ID reads are explicit-theme-demand only and opportunistic.
# -----------------------------------------------------------------------------
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
    "include/themes.h",
    """    theme_element_t *first;\n    theme_element_t *last;\n    unsigned char needsItemConfig;\n} theme_elems_t;\n""",
    """    theme_element_t *first;\n    theme_element_t *last;\n    unsigned char needsItemConfig;\n    // Set only when this family contains ItemText, the element that consumes the optional deep\n    // VCD display ID. Keeps cosmetic disc-image inspection completely demand-driven.\n    unsigned char needsVcdDisplayId;\n} theme_elems_t;\n""",
)

replace_once(
    "src/themes.c",
    """            } else if (!strcmp(elementsType[ELEM_TYPE_ITEM_TEXT], type)) {\n                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_ITEM_TEXT, 0, 0, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, theme->textColor, theme->fonts[0]);\n                elem->drawElem = &drawItemText;\n""",
    """            } else if (!strcmp(elementsType[ELEM_TYPE_ITEM_TEXT], type)) {\n                elems->needsVcdDisplayId = 1;\n                elem = initBasic(themePath, themeConfig, theme, name, ELEM_TYPE_ITEM_TEXT, 0, 0, ALIGN_CENTER, DIM_UNDEF, DIM_UNDEF, SCALING_RATIO, theme->textColor, theme->fonts[0]);\n                elem->drawElem = &drawItemText;\n""",
)

replace_once(
    "src/menusys.c",
    """    // The VCD caption id (#380) rides its OWN async request, independent of the needsItemConfig\n    // gate above -- that gate stays exactly as rebuild-155 wrote it. The request resolves ONLY the\n    // id (no CFG read), on the ioman worker; the resolver's per-session memo dedupes, so this costs\n    // one strcmp walk per frame and at most one queued resolve per settled VCD row per session.\n    if (list != NULL && vcdViewActive(list->mode) && selected_item->item->current != NULL) {\n        char *vcdName = list->itemGetStartup(list, selected_item->item->current->item.id);\n        if (vcdName != NULL)\n            vcdRequestDisplayId(vcdName);\n    }\n""",
    """    // Deep VCD ID inspection is cosmetic and must be explicit-theme-demand only. ItemText is\n    // the sole render element that consumes vcdDisplayIdCached(), so a family without ItemText does\n    // zero .VCD opens/seeks here. The resolver itself is opportunistic and yields to art/BGM.\n    if (elems->needsVcdDisplayId && list != NULL && vcdViewActive(list->mode) &&\n        selected_item->item->current != NULL) {\n        char *vcdName = list->itemGetStartup(list, selected_item->item->current->item.id);\n        if (vcdName != NULL)\n            vcdRequestDisplayId(vcdName);\n    }\n""",
)

replace_once(
    "src/supportbase.c",
    """    if (isVcd) {\n        char vcdId[VCD_ID_MAX];\n        // Ask the DISC first (lazy, memoized, config-path only -- see vcdResolveDisplayId), then the\n        // filename, then the filename raw. The image is ground truth and is what the RetroGEM\n        // barcode reads at launch, so the theme and the scanner agree about a game.\n        if (vcdResolveDisplayId(game->name, vcdId, sizeof(vcdId)) && vcdId[0] != '\\0')\n            configSetStr(config, CONFIG_ITEM_STARTUP, vcdId);\n        else\n            configSetStr(config, CONFIG_ITEM_STARTUP,\n                         vcdExtractGameId(game->name, vcdId, sizeof(vcdId)) ? vcdId : game->name);\n    } else {\n""",
    """    if (isVcd) {\n        char vcdId[VCD_ID_MAX];\n        // Config population is navigation metadata and must never inspect the disc image. Use the\n        // free filename parse only; the optional deep resolver is owned solely by an ItemText theme\n        // demand and updates its display memo asynchronously. Identity remains the VCD filename.\n        configSetStr(config, CONFIG_ITEM_STARTUP,\n                     vcdExtractGameId(game->name, vcdId, sizeof(vcdId)) ? vcdId : game->name);\n    } else {\n""",
)

replace_once(
    "src/vcdsupport.c",
    """#include \"include/gui.h\"         // guiWarning (passing toast on a failed launch-path BDMA equip)\n#include \"include/util.h\"        // checkMCSaveIconsDir -- browser icon pair for the POPSTARTER folder\n""",
    """#include \"include/gui.h\"         // guiWarning (passing toast on a failed launch-path BDMA equip)\n#include \"include/texcache.h\"    // cosmetic ID resolver yields while artwork is pending\n#include \"include/sound.h\"       // cosmetic ID resolver yields to BGM low-water protection\n#include \"include/util.h\"        // checkMCSaveIconsDir -- browser icon pair for the POPSTARTER folder\n""",
)

replace_once(
    "src/vcdsupport.c",
    """    // The resolver itself: opens the VCD and reads the id out of the image (memoized, one attempt\n    // per game per session). It runs HERE, on the ioman worker, so a slow device cannot stall the\n    // render thread -- the whole reason the caption does not call it directly.\n    vcdResolveDisplayId(name, id, sizeof(id));\n}\n\nvoid vcdRequestDisplayId(const char *name)\n{\n    if (name == NULL || name[0] == '\\0')\n        return;\n\n    vcd_id_memo_t *m = vcdIdMemoFind(name);\n""",
    """    // Async is not enough by itself: a device read can still starve the same USB/PFS channel\n    // used by artwork and BGM. This metadata has zero authority over those assets, so if either\n    // pipeline is busy simply abandon this attempt; the still-selected row asks again next frame.\n    if (cacheHasPendingArt() || !bgmDiscretionaryIoAllowed())\n        return;\n\n    vcdResolveDisplayId(name, id, sizeof(id));\n}\n\nvoid vcdRequestDisplayId(const char *name)\n{\n    char parsed[VCD_ID_MAX];\n\n    if (name == NULL || name[0] == '\\0')\n        return;\n\n    // Filename already supplies exactly what ItemText needs: display it immediately and never pay\n    // for a deep image read merely to rediscover the same cosmetic ID.\n    if (vcdExtractGameId(name, parsed, sizeof(parsed)))\n        return;\n\n    vcd_id_memo_t *m = vcdIdMemoFind(name);\n""",
)

# Update stale public/header comments that still describe unconditional or config-path resolution.
replace_once(
    "include/vcdsupport.h",
    """// DISPLAY-ONLY PS1 disc id, resolved lazily from the image on the per-game CONFIG path (async, once\n// per settled row, and only when the theme has an element that shows it). Returns 1 and writes the\n""",
    """// DISPLAY-ONLY PS1 disc id, resolved lazily from the image only when the active theme family\n// contains the ItemText element that consumes it. Returns 1 and writes the\n""",
)
replace_once(
    "include/vcdsupport.h",
    """// Ask for a settled VCD row's id to be resolved off-thread (one queued resolve per row per\n// session, memo-deduped, independent of the theme's config needs). Called per frame from the\n""",
    """// Ask for a settled VCD row's id to be resolved off-thread (one queued resolve per row per\n// session, memo-deduped, only under explicit ItemText demand). Called per frame from the\n""",
)

# Handoff: record the whole hardware follow-up without incrementing the step number.
handoff = Path("HANDOFF.md")
text = handoff.read_text()
marker = "### Step 212 hardware follow-up — list/VCD/APA/MC scheduling and ownership"
if marker in text:
    raise SystemExit("HANDOFF.md: hardware follow-up already present")
note = r'''

### Step 212 hardware follow-up — list/VCD/APA/MC scheduling and ownership

The combined Step-212 hardware round exposed four additional shared-lifecycle issues, fixed in this
same PR so testers do not need another branch/artifact cycle:

- **APA enumeration is independent from the config/art PFS home.** HDL PS2 games continue to come
  from the APA/HDL partition table, and HDD VCDs from existing POPS partitions (`pfs1:` scan space).
  Neither page is a CD/DVD-folder scan. A missing persistent `pfs0:` data home now disables only the
  features that actually require it (games.bin, CFG, ART, VMC/sidecars); the lists remain visible.
  All Step-209/210/211 no-create/no-format/raw-APA-write barriers remain intact.
- **Memory-card ownership is concrete.** An `mc1:` launch prefers slot 2 for bootstrap redirects,
  missing-config recovery and the legacy `OPL/` read even when `mc0:` is also populated. `mc0:` is
  symmetric; non-MC boots retain the historical first-present policy.
- **List art admission is bounded around the cursor.** The generic List renderer (Games, APPS,
  Favourites and VCD) requests the selected cover first and only +/-2 neighbours; farther visible
  rows draw resident art without starting new loose-PNG reads. This brings List request pressure in
  line with Coverflow rather than making viewport height equal I/O queue depth.
- **Metadata cannot monopolize the device.** APP `#Size` opens are deferred to the info screen.
  Generic VCD config population uses filename parsing only and never opens the `.VCD`. Deep PS1-ID
  inspection runs only when the active theme family contains the ItemText element that consumes it,
  skips filenames that already carry a strict ID, and yields whenever art is pending or BGM reserve
  is low. BGM itself keeps the 30/31 priorities + 768 KiB ring and now prevents new art reads below
  its low-water reserve; `OV_HOLE` is recoverable instead of terminating playback.

Hardware acceptance should explicitly cover: APA PS2 + VCD pages with/without the persistent PFS
home, mc0 and mc1 boots with both cards inserted, loose-PNG List/Coverflow on USB, APPS/Favourites
missing-art navigation, a VCD theme with no ItemText (zero deep image reads), and one with ItemText
(the ID may pop in opportunistically without starving art/audio). VCD favourite persistence remains
part of the regression matrix.
'''
handoff.write_text(text.rstrip() + note + "\n")
PY

git diff --check
git add include/themes.h src/themes.c src/menusys.c src/supportbase.c src/vcdsupport.c include/vcdsupport.h HANDOFF.md
git commit -m "rebuild-212: make VCD ID resolution demand-driven" -m "Remove deep .VCD inspection from generic config population and queue it only for an active theme family containing ItemText. Strict IDs already present in filenames need no image read; unresolved cosmetic IDs yield to pending artwork and BGM low-water protection. Document the complete Step-212 hardware follow-up without changing the step number."

git diff --check
git push origin HEAD:rebuild/step-212-apa-boot-and-bgm-resilience
