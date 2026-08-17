#!/usr/bin/env bash
set -euo pipefail

git config user.name "NathanNeurotic Step Builder"
git config user.email "actions@users.noreply.github.com"
git fetch origin rebuild/step-212-apa-boot-and-bgm-resilience
git checkout rebuild/step-212-apa-boot-and-bgm-resilience
test "$(git rev-parse HEAD)" = "21d6742de080c9bb90ab1d5e05ef444d444179dd"

# =============================================================================
# 1) APA/FHDB config ownership.  This patch is deliberately gated by
# gBootHomeApa: MC/USB/ETH/MMCE/BDM boot ownership stays byte-for-byte on its
# existing control flow.
# =============================================================================
python3 - <<'PY'
from pathlib import Path

p = Path('src/opl.c')
text = p.read_text()

old = r'''static int configPathRedirectLocation(char *out, int outLen)
{
    // Never compose a writable bootstrap file in raw APA space. hddN: is a partition
    // namespace, not a PFS filesystem; any file-like O_CREAT there is unsafe by definition.
    if (!strncmp(gBootDir, "hdd", 3)) {
        out[0] = '\0';
        return 0;
    }

    // With a known local/network boot identity, anchor config.path absolutely to that home
    // instead of trusting the process CWD. APA boots have already been rewritten to pfs0:.
    if (gBootDir[0] != '\0') {
        size_t len = strlen(gBootDir);
        if (len > 0 && (gBootDir[len - 1] == ':' || gBootDir[len - 1] == '/' || gBootDir[len - 1] == '\\'))
            snprintf(out, outLen, "%s%s", gBootDir, configPathRedirectFile);
        else
            snprintf(out, outLen, "%s/%s", gBootDir, configPathRedirectFile);
    } else {
        // Read-only legacy discovery may still inspect a relative redirect. Writers reject this
        // case below because an unknown CWD could itself be raw hddN: space.
        snprintf(out, outLen, "%s", configPathRedirectFile);
    }

    return 1;
}
'''
new = r'''static int configPathRedirectLocation(char *out, int outLen)
{
    const char *home = gBootDir;

    // APA launch identity and config ownership are separate. A delayed HDD mount can leave
    // gBootDir as raw hddN:, but once the EXISTING persistent PFS home is mounted, config.path
    // belongs there. This is the only APA override; every other boot class keeps gBootDir.
    if (gBootHomeApa && gHDDPrefix != NULL && gHDDPrefix[0] != '\0')
        home = gHDDPrefix;

    // Never compose a writable bootstrap file in raw APA space. hddN: is a partition
    // namespace, not a PFS filesystem; any file-like O_CREAT there is unsafe by definition.
    if (!strncmp(home, "hdd", 3)) {
        out[0] = '\0';
        return 0;
    }

    if (home[0] != '\0') {
        size_t len = strlen(home);
        if (len > 0 && (home[len - 1] == ':' || home[len - 1] == '/' || home[len - 1] == '\\'))
            snprintf(out, outLen, "%s%s", home, configPathRedirectFile);
        else
            snprintf(out, outLen, "%s/%s", home, configPathRedirectFile);
    } else {
        // Read-only legacy discovery may still inspect a relative redirect. Writers reject this
        // case below because an unknown CWD could itself be raw hddN: space.
        snprintf(out, outLen, "%s", configPathRedirectFile);
    }

    return 1;
}
'''
if text.count(old) != 1:
    raise SystemExit('configPathRedirectLocation: expected exactly one old block')
text = text.replace(old, new, 1)

# Direct pfs:/pfs0: syntax is already a mounted-filesystem path.  Do not reinterpret it
# relative to gHDDPrefix; HDD notation remains relative to the active data home.
old = r'''    // __common's data home is already pfs0:OPL/. Consume one redundant OPL component from
    // hdd0:/__common/OPL and pfs:/__common/OPL instead of producing pfs0:OPL/OPL.
    if (commonStyleHome && !strncmp(p, "OPL", 3) &&
        (p[3] == '\0' || p[3] == '/' || p[3] == ':' || p[3] == '\\')) {
'''
new = r'''    // __common's HDD-notation data home is already pfs0:OPL/. Consume one redundant OPL
    // component from hdd0:/__common/OPL. Direct pfs0: syntax remains direct PFS syntax.
    if (!isPfsAlias && commonStyleHome && !strncmp(p, "OPL", 3) &&
        (p[3] == '\0' || p[3] == '/' || p[3] == ':' || p[3] == '\\')) {
'''
if text.count(old) != 1:
    raise SystemExit('prepareCustomApaSettingsPath OPL component gate: expected one match')
text = text.replace(old, new, 1)

old = r'''    if (subfolder[0] != '\0') {
        size_t prefixLen = strlen(gHDDPrefix);
        if (prefixLen > 0 && (gHDDPrefix[prefixLen - 1] == ':' || gHDDPrefix[prefixLen - 1] == '/'))
            n = snprintf(canonical, sizeof(canonical), "%s%s", gHDDPrefix, subfolder);
        else
            n = snprintf(canonical, sizeof(canonical), "%s/%s", gHDDPrefix, subfolder);
    } else {
        n = snprintf(canonical, sizeof(canonical), "%s", gHDDPrefix);
    }
'''
new = r'''    if (isPfsAlias) {
        // pfs:/pfs0: names the already-mounted filesystem ROOT. Preserve that syntax exactly:
        // pfs0:/CFG -> pfs0:CFG, not pfs0:OPL/CFG. A redundant physical partition label was
        // consumed above only when it matched gOPLPart.
        if (subfolder[0] != '\0')
            n = snprintf(canonical, sizeof(canonical), "pfs0:%s", subfolder);
        else
            n = snprintf(canonical, sizeof(canonical), "pfs0:");
    } else if (subfolder[0] != '\0') {
        size_t prefixLen = strlen(gHDDPrefix);
        if (prefixLen > 0 && (gHDDPrefix[prefixLen - 1] == ':' || gHDDPrefix[prefixLen - 1] == '/'))
            n = snprintf(canonical, sizeof(canonical), "%s%s", gHDDPrefix, subfolder);
        else
            n = snprintf(canonical, sizeof(canonical), "%s/%s", gHDDPrefix, subfolder);
    } else {
        n = snprintf(canonical, sizeof(canonical), "%s", gHDDPrefix);
    }
'''
if text.count(old) != 1:
    raise SystemExit('prepareCustomApaSettingsPath canonical composer: expected one match')
text = text.replace(old, new, 1)

# Mount/retry the safe existing-PFS chain before asking for the APA bootstrap redirect.
old = r'''static int tryAlternateDevice(int types)
{
    char redirectPath[64];
    int value;
    DIR *dir;

    // The user's Custom Settings Path, if one was set, takes precedence over every discovery probe
'''
new = r'''static int tryAlternateDevice(int types)
{
    char redirectPath[64];
    int value;
    DIR *dir;

    // APA has one deterministic ownership chain. If the first boot-time mount missed while the
    // drive was settling, retry the EXISTING-PFS resolver before reading config.path. This adds no
    // new device class or fallback; checkLoadConfigHDD already performed the same synchronous HDD
    // work below, only too late to expose a PFS-hosted redirect.
    if (gBootHomeApa && (gHDDPrefix == NULL || gHDDPrefix[0] == '\0')) {
        if (hddLoadModulesReady())
            hddLoadSupportModules();
    }

    // The user's Custom Settings Path, if one was set, takes precedence over every discovery probe
'''
if text.count(old) != 1:
    raise SystemExit('tryAlternateDevice prologue: expected one match')
text = text.replace(old, new, 1)

old = r'''    // APA/PFS boot identity is authoritative. If the first early mount missed because the disk was
    // still settling, retry the existing-partition resolver before broad read-only recovery.
    if (gBootHomeApa) {
        value = checkLoadConfigHDD(types);
        if (value & CONFIG_OPL)
            return value;
    }

    // No redirect (or a stale redirect) must not strand an otherwise valid install. Search existing
    // conventional homes read-only: both MC slots first, then USB. A known boot home is restored as
    // the save destination after the read, so discovery cannot silently scatter future writes.
    value = tryMissingConfigPathRecovery(types);
    if (value & CONFIG_OPL)
        return value;

    // An APA launch that still has no config fails closed here. Never overwrite a valid mounted
    // PFS notification/home with the raw hddN: launch identity merely because no settings file exists.
    // If no PFS home is mounted, the explicit-save gate below retries the safe ownership chain.
    if (gBootHomeApa) {
        if (gHDDPrefix != NULL && gHDDPrefix[0] != '\0')
            configSetMove(gHDDPrefix);
        showCfgPopup = 0;
        return 0;
    }
'''
new = r'''    // APA/PFS boot identity is authoritative. After an explicit redirect misses, ONLY the
    // existing HDD ownership chain is eligible: conf_hdd.cfg's existing target, otherwise
    // __common/OPL. Never import an unrelated MC/USB master config into an FHDB/APA session; that
    // can resurrect stale Custom Settings Path state and makes the next save destination depend on
    // whichever removable device happened to be inserted.
    if (gBootHomeApa) {
        value = checkLoadConfigHDD(types);
        if (value & CONFIG_OPL)
            return value;

        // No master config yet is a valid first-run state. Keep defaults homed to the mounted safe
        // PFS target so the first explicit Save materializes them there. If PFS is still unavailable,
        // leave the raw APA launch identity only as a fail-closed marker; _saveConfig retries this
        // same safe chain and config.c blocks every raw hddN: config write as defense in depth.
        if (gHDDPrefix != NULL && gHDDPrefix[0] != '\0')
            configSetMove(gHDDPrefix);
        showCfgPopup = 0;
        return 0;
    }

    // Non-APA missing/stale redirect recovery retains the existing read-only MC/USB behavior.
    value = tryMissingConfigPathRecovery(types);
    if (value & CONFIG_OPL)
        return value;
'''
if text.count(old) != 1:
    raise SystemExit('APA recovery block: expected one match')
text = text.replace(old, new, 1)

p.write_text(text)
PY

git diff --check -- src/opl.c
git add src/opl.c
git commit -m "rebuild-212: keep APA config ownership on existing PFS"

# =============================================================================
# 2) CodeRabbit: list-only APA is now valid, so every remaining generic consumer
# must tolerate an HDD itemGetPrefix() of NULL.  This only changes the no-PFS
# state; normal MC/USB/ETH/MMCE/HDD-with-PFS behavior is unchanged.
# =============================================================================
python3 - <<'PY'
from pathlib import Path

p = Path('src/opl.c')
text = p.read_text()

old = r'''        if ((listSupport != NULL) && (listSupport->enabled) && (listSupport->itemGetPrefix != NULL)) {
            char *prefix = listSupport->itemGetPrefix(listSupport);
            snprintf(appsPath, sizeof(appsPath), "%sconf_apps.cfg", prefix);

            fd = openFile(appsPath, O_RDONLY);
'''
new = r'''        if ((listSupport != NULL) && (listSupport->enabled) && (listSupport->itemGetPrefix != NULL)) {
            char *prefix = listSupport->itemGetPrefix(listSupport);
            if (prefix == NULL || prefix[0] == '\0')
                continue;
            snprintf(appsPath, sizeof(appsPath), "%sconf_apps.cfg", prefix);

            fd = openFile(appsPath, O_RDONLY);
'''
if text.count(old) != 1:
    raise SystemExit('oplGetLegacyAppsConfig prefix site: expected one match')
text = text.replace(old, new, 1)

old = r'''config_set_t *oplGetLegacyAppsInfo(char *name)
{
    int i, fd;
    item_list_t *listSupport;
    config_set_t *appConfig;
    char appsPath[128];

    for (i = MODE_COUNT - 1; i >= 0; i--) {
'''
new = r'''config_set_t *oplGetLegacyAppsInfo(char *name)
{
    int i, fd;
    item_list_t *listSupport;
    config_set_t *appConfig;
    char appsPath[128] = {0};

    for (i = MODE_COUNT - 1; i >= 0; i--) {
'''
if text.count(old) != 1:
    raise SystemExit('oplGetLegacyAppsInfo init: expected one match')
text = text.replace(old, new, 1)

old = r'''        if ((listSupport != NULL) && (listSupport->enabled) && (listSupport->itemGetPrefix != NULL)) {
            char *prefix = listSupport->itemGetPrefix(listSupport);
            snprintf(appsPath, sizeof(appsPath), "%sCFG%s%s.cfg", prefix, i == ETH_MODE ? "\\" : "/", name);

            fd = openFile(appsPath, O_RDONLY);
'''
new = r'''        if ((listSupport != NULL) && (listSupport->enabled) && (listSupport->itemGetPrefix != NULL)) {
            char *prefix = listSupport->itemGetPrefix(listSupport);
            if (prefix == NULL || prefix[0] == '\0')
                continue;
            snprintf(appsPath, sizeof(appsPath), "%sCFG%s%s.cfg", prefix, i == ETH_MODE ? "\\" : "/", name);

            fd = openFile(appsPath, O_RDONLY);
'''
if text.count(old) != 1:
    raise SystemExit('oplGetLegacyAppsInfo prefix site: expected one match')
text = text.replace(old, new, 1)

old = r'''    /* Apps config not found on any device, go with last tested device.
       Does not matter if the config file could be loaded or not */
    appConfig = configAlloc(0, NULL, appsPath);

    return appConfig;
}

// ----------------------------------------------------------
// ----------------------- Updaters -------------------------
'''
new = r'''    /* Apps config not found on any device, go with the last tested device when one had a
       real prefix. A valid list-only APA session can have no persistent PFS prefix at all; in that
       state return metadata-only config instead of inventing a path on another device. */
    appConfig = configAlloc(0, NULL, appsPath[0] != '\0' ? appsPath : NULL);

    return appConfig;
}

// ----------------------------------------------------------
// ----------------------- Updaters -------------------------
'''
# There are two similar fallback comments; anchoring through the following Updaters marker selects Info.
if text.count(old) != 1:
    raise SystemExit('oplGetLegacyAppsInfo fallback: expected one match')
text = text.replace(old, new, 1)

old = r'''    snprintf(gOPLPart, sizeof(gOPLPart), "hdd0:%s", argv[3]);

    snprintf(path, sizeof(path), "%sCFG/%s.cfg", gHDDPrefix, gAutoLaunchGame->startup);
    configSet = configAlloc(0, NULL, path);
    configRead(configSet);

    hddLaunchGame(NULL, -1, configSet);
'''
new = r'''    snprintf(gOPLPart, sizeof(gOPLPart), "hdd0:%s", argv[3]);

    if (gHDDPrefix != NULL && gHDDPrefix[0] != '\0') {
        snprintf(path, sizeof(path), "%sCFG/%s.cfg", gHDDPrefix, gAutoLaunchGame->startup);
        configSet = configAlloc(0, NULL, path);
        configRead(configSet);
    } else {
        // HDL autolaunch does not require a persistent config/art PFS home. Keep the launch alive
        // with defaults only; never format a NULL prefix into a bogus path.
        configSet = configAlloc(0, NULL, NULL);
    }
    if (configSet == NULL) {
        free(gAutoLaunchGame);
        gAutoLaunchGame = NULL;
        return;
    }

    hddLaunchGame(NULL, -1, configSet);
'''
if text.count(old) != 1:
    raise SystemExit('autoLaunchHDDGame path site: expected one match')
text = text.replace(old, new, 1)

p.write_text(text)
PY

git diff --check -- src/opl.c
git add src/opl.c
git commit -m "rebuild-212: guard list-only HDD prefix consumers"

# =============================================================================
# 3) CodeRabbit: ETH currently owns one visible backing array.  Do NOT refactor
# every device or the live ETH list.  Add one ETH-only, read-only ISO probe cache
# used solely when Favourites must validate an ISO record while ETH is showing VCD.
# =============================================================================
python3 - <<'PY'
from pathlib import Path

# Header: one narrowly-scoped helper.
p = Path('include/ethsupport.h')
text = p.read_text()
old = 'item_list_t *ethGetObject(int initOnly);\n\n#endif\n'
new = '''item_list_t *ethGetObject(int initOnly);\n// Resolve a stored ETH ISO favourite against the ISO backing view even while the live ETH page\n// is showing VCDs. Read-only; never changes the visible ETH list or L3 view.\nint ethResolveIsoFavourite(int id, const char *name, int *outId);\n\n#endif\n'''
if text.count(old) != 1:
    raise SystemExit('ethsupport.h tail: expected one match')
text = text.replace(old, new, 1)
p.write_text(text)

# ETH implementation.
p = Path('src/ethsupport.c')
text = p.read_text()
old = '''static int ethGameCount = 0;\nstatic unsigned char ethModulesLoaded = 0;\nstatic base_game_info_t *ethGames = NULL;\n\nstatic struct ip4_addr lastIP;\n'''
new = '''static int ethGameCount = 0;\nstatic unsigned char ethModulesLoaded = 0;\nstatic base_game_info_t *ethGames = NULL;\n\n// Favourites needs to validate an ISO record even when the live ETH page currently owns the VCD\n// array. Keep that alternate backing list private to ETH instead of teaching generic viewOverride\n// to rescan or mutating the visible list. It is populated lazily, read-only, and invalidated by every\n// ETH list mutation/reconnect. No other device class uses this cache.\nstatic base_game_info_t *ethFavIsoGames = NULL;\nstatic int ethFavIsoGameCount = 0;\nstatic unsigned char ethFavIsoValid = 0;\n\nstatic void ethInvalidateFavIsoBacking(void)\n{\n    free(ethFavIsoGames);\n    ethFavIsoGames = NULL;\n    ethFavIsoGameCount = 0;\n    ethFavIsoValid = 0;\n}\n\nstatic struct ip4_addr lastIP;\n'''
if text.count(old) != 1:
    raise SystemExit('ethsupport.c globals: expected one match')
text = text.replace(old, new, 1)

# Re-init starts a new backing generation.
old = '''void ethInit(item_list_t *itemList)\n{\n    if (ethInitSema() < 0)\n        return;\n\n    if (gNetworkStartup >= ERROR_ETH_SMB_CONN) {\n'''
new = '''void ethInit(item_list_t *itemList)\n{\n    if (ethInitSema() < 0)\n        return;\n\n    ethInvalidateFavIsoBacking();\n\n    if (gNetworkStartup >= ERROR_ETH_SMB_CONN) {\n'''
if text.count(old) != 1:
    raise SystemExit('ethInit: expected one match')
text = text.replace(old, new, 1)

# Any live list update invalidates the alternate ISO snapshot first.
old = '''static int ethUpdateGameList(item_list_t *itemList)\n{\n    if (gPCShareName[0]) {\n'''
new = '''static int ethUpdateGameList(item_list_t *itemList)\n{\n    ethInvalidateFavIsoBacking();\n\n    if (gPCShareName[0]) {\n'''
if text.count(old) != 1:
    raise SystemExit('ethUpdateGameList: expected one match')
text = text.replace(old, new, 1)

# Insert the view-aware ISO resolver after the list updater and before callbacks.
marker = '''    return ethGameCount;\n}\n\nstatic int ethGetGameCount(item_list_t *itemList)\n'''
insert = '''    return ethGameCount;\n}\n\nint ethResolveIsoFavourite(int id, const char *name, int *outId)\n{\n    base_game_info_t *games;\n    int count;\n\n    if (name == NULL || outId == NULL || id < 0 || !gPCShareName[0] || gNetworkStartup != 0)\n        return 0;\n\n    if (!vcdViewActive(ETH_MODE)) {\n        // The live ETH backing store already IS the ISO list. Preserve the old id+name validation.\n        games = ethGames;\n        count = ethGameCount;\n    } else {\n        // The live backing store is VCD. Build a separate read-only ISO snapshot once for this ETH\n        // generation; sbReadList receives its own cache-size/count state and cannot replace ethGames.\n        if (!ethFavIsoValid) {\n            int probeSize = -2;\n            int probeCount = 0;\n            base_game_info_t *probeGames = NULL;\n\n            if (sbReadList(&probeGames, ethPrefix, NULL, &probeSize, &probeCount) < 0) {\n                free(probeGames);\n                return 0;\n            }\n            ethFavIsoGames = probeGames;\n            ethFavIsoGameCount = probeCount;\n            ethFavIsoValid = 1;\n        }\n        games = ethFavIsoGames;\n        count = ethFavIsoGameCount;\n    }\n\n    if (games == NULL || id >= count || strcmp(games[id].name, name) != 0)\n        return 0;\n\n    *outId = id;\n    return 1;\n}\n\nstatic int ethGetGameCount(item_list_t *itemList)\n'''
if text.count(marker) != 1:
    raise SystemExit('ETH resolver insertion marker: expected one match')
text = text.replace(marker, insert, 1)

# Direct mutations invalidate the cached alternate view immediately.
old = '''static void ethDeleteGame(item_list_t *itemList, int id)\n{\n    sbDelete(&ethGames, ethPrefix, "\\\\", ethGameCount, id);\n    ethULSizePrev = -2;\n}\n\nstatic void ethRenameGame(item_list_t *itemList, int id, char *newName)\n{\n    sbRename(&ethGames, ethPrefix, "\\\\", ethGameCount, id, newName);\n    ethULSizePrev = -2;\n}\n'''
new = '''static void ethDeleteGame(item_list_t *itemList, int id)\n{\n    ethInvalidateFavIsoBacking();\n    sbDelete(&ethGames, ethPrefix, "\\\\", ethGameCount, id);\n    ethULSizePrev = -2;\n}\n\nstatic void ethRenameGame(item_list_t *itemList, int id, char *newName)\n{\n    ethInvalidateFavIsoBacking();\n    sbRename(&ethGames, ethPrefix, "\\\\", ethGameCount, id, newName);\n    ethULSizePrev = -2;\n}\n'''
if text.count(old) != 1:
    raise SystemExit('ETH delete/rename: expected one match')
text = text.replace(old, new, 1)

# Cleanup owns both arrays.
old = '''        LOG("ETHSUPPORT CleanUp\\n");\n\n        free(ethGames);\n\n        // disconnect from the active SMB session\n'''
new = '''        LOG("ETHSUPPORT CleanUp\\n");\n\n        free(ethGames);\n        ethGames = NULL;\n        ethInvalidateFavIsoBacking();\n\n        // disconnect from the active SMB session\n'''
if text.count(old) != 1:
    raise SystemExit('ethCleanUp free site: expected one match')
text = text.replace(old, new, 1)

old = '''        LOG("ETHSUPPORT Shutdown\\n");\n\n        free(ethGames);\n\n        // disconnect from the active SMB session\n'''
new = '''        LOG("ETHSUPPORT Shutdown\\n");\n\n        free(ethGames);\n        ethGames = NULL;\n        ethInvalidateFavIsoBacking();\n\n        // disconnect from the active SMB session\n'''
if text.count(old) != 1:
    raise SystemExit('ethShutdown free site: expected one match')
text = text.replace(old, new, 1)
p.write_text(text)

# Favourites: special-case only ETH ISO validation; all other modes keep the existing view proxy.
p = Path('src/favsupport.c')
text = p.read_text()
old = '#include "include/vcdsupport.h"  // vcdViewActive / vcdConsumeDirty (VCD favourites)\n#include "include/favsupport.h"\n'
new = '#include "include/vcdsupport.h"  // vcdViewActive / vcdConsumeDirty (VCD favourites)\n#include "include/ethsupport.h"  // ETH ISO favourite resolution while the live source is in VCD view\n#include "include/favsupport.h"\n'
if text.count(old) != 1:
    raise SystemExit('favsupport include site: expected one match')
text = text.replace(old, new, 1)

old = '''    item_list_t view = *source;\n    view.viewOverride = isVcd ? ITEM_VIEW_FORCE_VCD : ITEM_VIEW_FORCE_ISO;\n    int count = view.itemGetCount(&view);\n\n    // APP ids are aggregate-list positions and can move; their stable identity is the title.\n'''
new = '''    // ETH has one live backing array whose contents follow the source page's L3 state, so a shallow\n    // viewOverride cannot turn VCD-backed ethGames into the ISO list. Resolve that one mode through\n    // ETH's private read-only ISO backing probe. Every other device keeps the existing proxy path.\n    if (source->mode == ETH_MODE && !isVcd)\n        return ethResolveIsoFavourite(id, text, outId);\n\n    item_list_t view = *source;\n    view.viewOverride = isVcd ? ITEM_VIEW_FORCE_VCD : ITEM_VIEW_FORCE_ISO;\n    int count = view.itemGetCount(&view);\n\n    // APP ids are aggregate-list positions and can move; their stable identity is the title.\n'''
if text.count(old) != 1:
    raise SystemExit('favResolveStoredId view site: expected one match')
text = text.replace(old, new, 1)
p.write_text(text)
PY

git diff --check -- include/ethsupport.h src/ethsupport.c src/favsupport.c
git add include/ethsupport.h src/ethsupport.c src/favsupport.c
git commit -m "rebuild-212: resolve ETH favourites against ISO backing"

# =============================================================================
# 4) CodeRabbit List cache-budget correction. This is the only generic runtime
# change in this batch. It cannot exceed each cache's count-1 budget because
# listWarm[].left is the admission counter; BGM low-water gating remains below it.
# =============================================================================
python3 - <<'PY'
from pathlib import Path
p = Path('src/themes.c')
text = p.read_text()
old = '        int warmRadius = (selectedCache != NULL && selectedCache->count > 1) ? (selectedCache->count - 1) / 2 : 0;\n'
new = '        int warmRadius = (selectedCache != NULL && selectedCache->count > 1) ? selectedCache->count - 1 : 0;\n'
if text.count(old) != 1:
    raise SystemExit('themes warmRadius: expected one match')
text = text.replace(old, new, 1)
old = '''                if (warmCache == NULL || warmCache->count < 2)\n                    continue;\n                if (step >= (warmCache->count - 1) / 2)\n                    continue;\n\n                int b;\n'''
new = '''                if (warmCache == NULL || warmCache->count < 2)\n                    continue;\n\n                int b;\n'''
if text.count(old) != 1:
    raise SystemExit('themes per-side half-budget gate: expected one match')
text = text.replace(old, new, 1)
p.write_text(text)
PY

git diff --check -- src/themes.c
git add src/themes.c
git commit -m "rebuild-212: use full List neighbour cache budget"

# =============================================================================
# 5) Handoff: fix the lint finding and record the hardware-driven ownership
# correction so the next tester knows exactly what changed and what must not.
# =============================================================================
python3 - <<'PY'
from pathlib import Path
p = Path('HANDOFF.md')
text = p.read_text()
old = '### Step 212 APA config-source/save-home correction\nHardware on the stacked Step-212 artifact exposed'
new = '### Step 212 APA config-source/save-home correction\n\nHardware on the stacked Step-212 artifact exposed'
if text.count(old) != 1:
    raise SystemExit('HANDOFF heading spacing: expected one match')
text = text.replace(old, new, 1)

note = '''\n\n### Step 212 follow-up — deterministic APA config ownership and review closure\n\nMilker_Myers hardware feedback exposed that an APA/FHDB boot could still fall through the generic missing-`config.path` recovery scan and import an older MC/USB master config. That is now prohibited for APA-origin boots. APA configuration ownership is deterministic: mount existing PFS support, honor `__common/OPL/conf_hdd.cfg` when it names an existing mountable target, otherwise use existing `__common/OPL/`, honor a valid explicit `config.path` from that mounted home, and otherwise keep defaults homed to that safe PFS location for the first explicit save. No APA partition is created, formatted, resized, repaired, or opened with a file-style `O_CREAT`.\n\nThe same follow-up closes the current CodeRabbit findings without broad device rewrites:\n\n- list-only APA sessions skip NULL HDD prefixes in legacy APPS discovery and HDD argv autolaunch uses metadata-only defaults when no persistent PFS home exists;\n- ETH ISO favourites use an ETH-private read-only ISO backing probe only when the live ETH page is showing VCD, leaving every other device's view handling unchanged;\n- List neighbour warming uses every available cache slot (`count - 1`) while the existing per-cache admission counter and BGM low-water gate still bound storage work;\n- direct `pfs:` / `pfs0:` custom paths remain direct mounted-PFS syntax; HDD notation continues to resolve relative to the active configured data home.\n\nHardware regression emphasis: APA/FHDB save/reboot with and without MC inserted; existing `conf_hdd.cfg -> +OPL`; missing master config first-save creation under `__common/OPL`; explicit HDD custom path fallback; MC0/MC1 boot ownership; USB/MMCE/MX4SIO/ETH normal save/load; ETH ISO favourites while the source page is in VCD view; and BGM/art stress after the List cache-budget correction.\n'''
if '### Step 212 follow-up — deterministic APA config ownership and review closure' not in text:
    text += note
p.write_text(text)
PY

git diff --check -- HANDOFF.md
git add HANDOFF.md
git commit -m "docs: record deterministic APA config follow-up"

git diff --check

git push origin HEAD:rebuild/step-212-apa-boot-and-bgm-resilience
