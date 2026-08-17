#!/usr/bin/env bash
set -euo pipefail

git config user.name "NathanNeurotic Step Builder"
git config user.email "actions@users.noreply.github.com"
git fetch origin rebuild/step-212-apa-boot-and-bgm-resilience
git checkout rebuild/step-212-apa-boot-and-bgm-resilience
test "$(git rev-parse HEAD)" = "9a961663338083901974822db4feabee434127a1"

# -----------------------------------------------------------------------------
# Commit 1: make Favourites source proxies view-specific without mutating the
# real source page's independent ISO/VCD L3 state.
# -----------------------------------------------------------------------------
python3 - <<'PY'
from pathlib import Path

p = Path('include/iosupport.h')
text = p.read_text()
old = '''    /// Launch a VCD (PS1/POPSTARTER) item by its stored name, regardless of the device's current\n    /// view. NULL for devices without a VCD view (checklist item 12). Used by the Favourites tab to\n    /// launch a VCD favourite while its source device page may be in ISO view.\n    void (*itemLaunchVcd)(item_list_t *itemList, const char *vcdName, config_set_t *configSet);\n} item_list_t;\n'''
new = '''    /// Launch a VCD (PS1/POPSTARTER) item by its stored name, regardless of the device's current\n    /// view. NULL for devices without a VCD view (checklist item 12). Used by the Favourites tab to\n    /// launch a VCD favourite while its source device page may be in ISO view.\n    void (*itemLaunchVcd)(item_list_t *itemList, const char *vcdName, config_set_t *configSet);\n\n    /// Optional view override for a shallow proxy of this support. Zero keeps the support's native\n    /// per-mode L3 state; Favourites uses the forced values so an ISO/VCD favourite can proxy its\n    /// source without mutating the real source page's independent view. Appended last so all legacy\n    /// positional item_list_t initializers default safely to native behavior.\n    unsigned char viewOverride;\n} item_list_t;\n\n#define ITEM_VIEW_NATIVE    0\n#define ITEM_VIEW_FORCE_ISO 1\n#define ITEM_VIEW_FORCE_VCD 2\n'''
if text.count(old) != 1:
    raise SystemExit('iosupport.h: item_list tail did not match exactly once')
p.write_text(text.replace(old, new, 1))

p = Path('include/vcdsupport.h')
text = p.read_text()
old = '''// Is the given device mode currently showing its VCD list (vs its disc list)?\nint vcdViewActive(int mode);\n'''
new = '''// Is the given device mode currently showing its VCD list (vs its disc list)?\nint vcdViewActive(int mode);\n// Same query for an item-list instance. A normal source delegates to vcdViewActive(mode); a\n// Favourites shallow proxy may force ISO or VCD without changing the source page's own L3 state.\nint vcdListViewActive(const item_list_t *itemList);\n'''
if text.count(old) != 1:
    raise SystemExit('vcdsupport.h: vcdViewActive declaration did not match exactly once')
p.write_text(text.replace(old, new, 1))

changed = 0
# MMCE is currently the reserved/stub mode in this rebuild and has no src/mmcesupport.c.
for name in ['src/bdmsupport.c', 'src/hddsupport.c', 'src/ethsupport.c', 'src/udpfssupport.c']:
    p = Path(name)
    text = p.read_text()
    count = text.count('vcdViewActive(itemList->mode)')
    if count:
        text = text.replace('vcdViewActive(itemList->mode)', 'vcdListViewActive(itemList)')
        p.write_text(text)
        changed += count
        print(f'{name}: converted {count} view query/queries')
if changed == 0:
    raise SystemExit('no source-support vcdViewActive(itemList->mode) calls were converted')

p = Path('src/vcdsupport.c')
text = p.read_text()
needle = '''int vcdViewActive(int mode)\n{\n    if (mode < 0 || mode >= MODE_COUNT || !vcdModeSupported(mode))\n        return 0;\n    // The global default-view setting overrides the per-device L3 toggle when locked to one type.\n    if (gDefaultGameView == GAME_VIEW_ISO)\n        return 0; // locked to the ISO/disc list\n    if (gDefaultGameView == GAME_VIEW_VCD)\n        return 1;         // locked to the VCD (PS1) list\n    return vcdView[mode]; // GAME_VIEW_BOTH: per-device L3 toggle (defaults to ISO)\n}\n'''
replacement = needle + '''\nint vcdListViewActive(const item_list_t *itemList)\n{\n    if (itemList == NULL)\n        return 0;\n    if (itemList->viewOverride == ITEM_VIEW_FORCE_ISO)\n        return 0;\n    if (itemList->viewOverride == ITEM_VIEW_FORCE_VCD)\n        return 1;\n    return vcdViewActive(itemList->mode);\n}\n'''
if text.count(needle) != 1:
    raise SystemExit('vcdsupport.c: vcdViewActive implementation did not match exactly once')
p.write_text(text.replace(needle, replacement, 1))

p = Path('src/favsupport.c')
text = p.read_text()

old_i = '        item_list_t *o = favArray[i].owner;\n'
new_i = '        item_list_t ownerView;\n        item_list_t *o = favOwnerView(i, &ownerView);\n'
count_i = text.count(old_i)
if count_i < 2:
    raise SystemExit(f'favsupport.c: expected multiple indexed owner proxy sites, got {count_i}')
text = text.replace(old_i, new_i)

old_id = '    item_list_t *o = favArray[id].owner;\n'
new_id = '    item_list_t ownerView;\n    item_list_t *o = favOwnerView(id, &ownerView);\n'
count_id = text.count(old_id)
if count_id < 4:
    raise SystemExit(f'favsupport.c: expected multiple current-record owner proxy sites, got {count_id}')
text = text.replace(old_id, new_id)

needle = '''static int favValidIndex(int id) { return (favArray != NULL && id >= 0 && id < favCount); }\n'''
insert = needle + '''\n// Build a stack-local view of a favourite's LIVE source support. Only viewOverride changes; priv,\n// callbacks, flags and owner all come from the current source object. This lets Favourites keep its\n// ISO/VCD split independent from the source page's own L3 state without copying or mutating device\n// state. The returned pointer is valid only as long as `view` remains in scope.\nstatic item_list_t *favOwnerView(int id, item_list_t *view)\n{\n    if (!favValidIndex(id) || view == NULL || favArray[id].owner == NULL)\n        return NULL;\n\n    *view = *favArray[id].owner;\n    view->viewOverride = favArray[id].isVcd ? ITEM_VIEW_FORCE_VCD : ITEM_VIEW_FORCE_ISO;\n    return view;\n}\n'''
if text.count(needle) != 1:
    raise SystemExit('favsupport.c: favValidIndex marker did not match exactly once')
text = text.replace(needle, insert, 1)

start = text.index('static item_list_t *favResolve(')
end = text.index('// ---- item_list_t callbacks', start)
new_resolve = r'''static int favResolveStoredId(item_list_t *source, int id, const char *text, int isVcd, int *outId)
{
    if (source == NULL || text == NULL || outId == NULL || source->itemGetCount == NULL || source->itemGetName == NULL)
        return 0;

    item_list_t view = *source;
    view.viewOverride = isVcd ? ITEM_VIEW_FORCE_VCD : ITEM_VIEW_FORCE_ISO;
    int count = view.itemGetCount(&view);

    // APP ids are aggregate-list positions and can move; their stable identity is the title.
    if (source->mode == APP_MODE) {
        for (int i = 0; i < count; i++) {
            char *name = view.itemGetName(&view, i);
            if (name != NULL && strcmp(name, text) == 0) {
                *outId = i;
                return 1;
            }
        }
        return 0;
    }

    if (id < 0 || id >= count)
        return 0;
    char *name = view.itemGetName(&view, id);
    if (name == NULL || strcmp(name, text) != 0)
        return 0;
    *outId = id;
    return 1;
}

static item_list_t *favResolve(int mode, int id, const char *text, int isVcd, int *outMode, int *outId)
{
    *outMode = mode;
    *outId = id;

    // VCD favourites are name-addressed and already view-independent: art/config/launch all key off
    // the stored .VCD basename. Bind them to a loaded VCD-capable source without disturbing that
    // source page's current ISO/VCD view.
    if (isVcd) {
        if (mode >= BDM_MODE && mode <= BDM_MODE_LAST) {
            opl_io_module_t *mod = oplGetModule(mode);
            if (mod != NULL && mod->support != NULL && mod->support->itemLaunchVcd != NULL) {
                favVcdMarkStar(mod, id, text);
                return mod->support;
            }
            for (int m = BDM_MODE; m <= BDM_MODE_LAST; m++) {
                opl_io_module_t *bm = oplGetModule(m);
                if (bm != NULL && bm->support != NULL && bm->support->itemLaunchVcd != NULL) {
                    *outMode = m;
                    favVcdMarkStar(bm, id, text);
                    return bm->support;
                }
            }
            return NULL;
        }
        if (mode < 0 || mode >= MODE_COUNT)
            return NULL;
        opl_io_module_t *mod = oplGetModule(mode);
        if (mod == NULL || mod->support == NULL || mod->support->itemLaunchVcd == NULL)
            return NULL;
        favVcdMarkStar(mod, id, text);
        return mod->support;
    }

    // ISO/DVD/CD favourites must be just as independent. Validate against a forced-ISO shallow
    // support view, which reads the source's retained ISO backing array even when its visible page
    // is currently VCD. Only mark a live submenu star when that submenu itself is in ISO view.
    if (mode >= BDM_MODE && mode <= BDM_MODE_LAST) {
        for (int m = BDM_MODE; m <= BDM_MODE_LAST; m++) {
            opl_io_module_t *mod = oplGetModule(m);
            if (mod == NULL || mod->support == NULL)
                continue;
            int liveId = id;
            if (favResolveStoredId(mod->support, id, text, 0, &liveId)) {
                if (!vcdViewActive(mod->support->mode)) {
                    submenu_list_t *src = submenuFindItemByIdAndText(mod->subMenu, liveId, text);
                    if (src != NULL)
                        src->item.favourited = 1;
                }
                *outMode = m;
                *outId = liveId;
                return mod->support;
            }
        }
        return NULL;
    }

    if (mode < 0 || mode >= MODE_COUNT)
        return NULL;
    opl_io_module_t *mod = oplGetModule(mode);
    if (mod == NULL || mod->support == NULL)
        return NULL;

    int liveId = id;
    if (!favResolveStoredId(mod->support, id, text, 0, &liveId))
        return NULL;

    if (!vcdViewActive(mod->support->mode)) {
        submenu_list_t *src = submenuFindItemByIdAndText(mod->subMenu, liveId, text);
        if (src != NULL)
            src->item.favourited = 1;
    }
    *outId = liveId;
    return mod->support;
}

'''
text = text[:start] + new_resolve + text[end:]
p.write_text(text)
PY

git diff --check
git add include/iosupport.h include/vcdsupport.h src/vcdsupport.c src/bdmsupport.c src/hddsupport.c src/ethsupport.c src/udpfssupport.c src/favsupport.c
git commit -m "rebuild-212: isolate Favourites ISO and VCD source views"

# -----------------------------------------------------------------------------
# Commit 2: normalize user-facing APA/PFS settings aliases onto the already-owned
# safe PFS data home. No arbitrary partition mount/create is introduced.
# -----------------------------------------------------------------------------
python3 - <<'PY'
from pathlib import Path
p = Path('src/opl.c')
text = p.read_text()
start = text.index('static int prepareCustomApaSettingsPath(')
end = text.index('\nstatic int isApaSettingsPath(', start)
new_func = r'''static int prepareCustomApaSettingsPath(char *path, int pathLen)
{
    char targetPart[64] = {0};
    char subfolder[64] = {0};
    char canonical[64] = {0};
    const char *p = path;
    const char *unit = "hdd0";
    int isPfsAlias = 0;
    size_t i = 0;
    DIR *dir;
    int n;

    if (path == NULL || path[0] == '\0') {
        gLastSaveErrno = ENODEV;
        return -1;
    }

    // Accept both pfs: and pfs0: as user-facing aliases for OPL's already-mounted persistent
    // data home. pfs1: remains reserved for transient HDD scanning and is never a settings home.
    if (!strncmp(path, "pfs", 3)) {
        if (path[3] == ':') {
            p = path + 4;
            isPfsAlias = 1;
        } else if (path[3] == '0' && path[4] == ':') {
            p = path + 5;
            isPfsAlias = 1;
        } else {
            gLastSaveErrno = ENODEV;
            return -1;
        }
    } else if (path[0] == '+') {
        p = path;
    } else {
        if (strncmp(path, "hdd", 3) != 0 || (path[3] != '0' && path[3] != '1') || path[4] != ':') {
            LOG("CONFIG malformed APA settings path %s rejected\n", path);
            gLastSaveErrno = ENODEV;
            return -1;
        }
        if (path[3] == '1')
            unit = "hdd1";
        p = path + 5;
        while (*p == '/' || *p == ':' || *p == '\\')
            p++;
    }

    if (!hddLoadModulesReady()) {
        gLastSaveErrno = ENODEV;
        return -1;
    }
    hddLoadSupportModules();
    if (gOPLPart[0] == '\0' || gHDDPrefix == NULL || gHDDPrefix[0] == '\0') {
        gLastSaveErrno = ENODEV;
        return -1;
    }

    const char *activeLabel = strchr(gOPLPart, ':');
    activeLabel = activeLabel ? activeLabel + 1 : gOPLPart;
    int commonStyleHome = (activeLabel[0] != '+');

    if (isPfsAlias) {
        // Compatibility spellings seen on hardware include pfs0:, pfs:/__common/OPL and
        // pfs0:/OPL. They all mean the ACTIVE already-mounted data home; never reinterpret a
        // pfs alias as permission to mount another APA partition.
        while (*p == '/' || *p == ':' || *p == '\\')
            p++;

        size_t labelLen = strlen(activeLabel);
        if (labelLen > 0 && !strncmp(p, activeLabel, labelLen) &&
            (p[labelLen] == '\0' || p[labelLen] == '/' || p[labelLen] == ':' || p[labelLen] == '\\')) {
            p += labelLen;
            while (*p == '/' || *p == ':' || *p == '\\')
                p++;
        } else if ((*p == '+' || !strncmp(p, "__", 2)) && *p != '\0') {
            LOG("CONFIG PFS settings alias %s names a non-active APA partition\n", path);
            gLastSaveErrno = ENODEV;
            return -1;
        }
    } else {
        i = 0;
        while (*p != '\0' && *p != '/' && *p != ':' && *p != '\\' && i < sizeof(subfolder) - 1)
            subfolder[i++] = *p++;
        subfolder[i] = '\0';
        if (subfolder[0] == '\0') {
            gLastSaveErrno = ENODEV;
            return -1;
        }
        snprintf(targetPart, sizeof(targetPart), "%s:%s", unit, subfolder);
        if (strcmp(targetPart, gOPLPart) != 0) {
            LOG("CONFIG APA settings partition %s rejected; active OPL data partition is %s\n", targetPart, gOPLPart);
            gLastSaveErrno = ENODEV;
            return -1;
        }

        while (*p != '\0') {
            while (*p == ':' || *p == '/' || *p == '\\')
                p++;
            if (!strncmp(p, "pfs:", 4)) {
                p += 4;
                continue;
            }
            break;
        }
    }

    // __common's data home is already pfs0:OPL/. Consume one redundant OPL component from
    // hdd0:/__common/OPL and pfs:/__common/OPL instead of producing pfs0:OPL/OPL.
    if (commonStyleHome && !strncmp(p, "OPL", 3) &&
        (p[3] == '\0' || p[3] == '/' || p[3] == ':' || p[3] == '\\')) {
        p += 3;
        while (*p == ':' || *p == '/' || *p == '\\')
            p++;
    }

    i = 0;
    while (*p != '\0' && i < sizeof(subfolder) - 1) {
        char c = *p++;
        subfolder[i++] = (c == '\\') ? '/' : c;
    }
    subfolder[i] = '\0';
    while (i > 0 && subfolder[i - 1] == '/')
        subfolder[--i] = '\0';

    if (subfolder[0] != '\0') {
        size_t prefixLen = strlen(gHDDPrefix);
        if (prefixLen > 0 && (gHDDPrefix[prefixLen - 1] == ':' || gHDDPrefix[prefixLen - 1] == '/'))
            n = snprintf(canonical, sizeof(canonical), "%s%s", gHDDPrefix, subfolder);
        else
            n = snprintf(canonical, sizeof(canonical), "%s/%s", gHDDPrefix, subfolder);
    } else {
        n = snprintf(canonical, sizeof(canonical), "%s", gHDDPrefix);
    }

    if (n < 0 || n >= (int)sizeof(canonical) || n >= pathLen) {
        gLastSaveErrno = ENODEV;
        return -1;
    }

    dir = opendir(canonical);
    if (dir == NULL) {
        LOG("CONFIG APA settings directory %s does not exist\n", canonical);
        gLastSaveErrno = ENOENT;
        return -1;
    }
    closedir(dir);

    snprintf(path, pathLen, "%s", canonical);
    LOG("CONFIG APA settings path -> %s (OPL part %s)\n", path, gOPLPart);
    return 1;
}
'''
text = text[:start] + new_func + text[end:]
p.write_text(text)
PY

git diff --check
git add src/opl.c
git commit -m "rebuild-212: normalize safe APA settings aliases"

git push origin HEAD:rebuild/step-212-apa-boot-and-bgm-resilience
