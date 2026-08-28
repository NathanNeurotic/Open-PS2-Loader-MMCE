/*
  Copyright 2024, Open-PS2-Loader contributors
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.

  Favourites (FAV_MODE): a virtual tab that aggregates favourited items from every loaded
  device list. Each favourite proxies launch / config / art / flags to its source list.
  Persistence uses a versioned, bounds-checked binary store (favourites.bin) written with
  explicit scalar fields -- never a raw-struct fwrite -- so a corrupt file can never index
  or allocate out of bounds.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "include/opl.h"
#include "include/iosupport.h"
#include "include/menusys.h"
#include "include/ioman.h"
#include "include/lang.h"
#include "include/textures.h"
#include "include/config.h"
#include "include/supportbase.h" // sbPopulateConfig + base_game_info_t (VCD favourite config)
#include "include/vcdsupport.h"
#include "include/cuesupport.h" // CUE_ROW_EXTENSION -- a PS1 favourite names its core  // VCD favourites: name-addressed POPSTARTER launch helpers
#include "include/libview.h"    // libViewActive / libListViewActive -- which list this page shows
#include "include/ethsupport.h" // ETH ISO favourite resolution while the live source is in VCD view
#include "include/favsupport.h"

int gFAVStartMode;

// Forward declaration; the initialised definition is at the bottom of this file.
static item_list_t favItemList;

// In-memory, validated favourites. Rebuilt by favUpdateItemList from favourites.bin; each
// entry's owner/id are confirmed present in the source submenu, so proxying never goes OOB.
typedef struct
{
    item_list_t *owner; // resolved source list
    int mode;           // resolved source mode (BDM re-matched across slots)
    int id;             // source item id (validated present in owner's submenu; unused for VCD)
    int icon_id;
    int text_id;
    int kind;   // FAV_KIND_*: which shelf it sits on and which core launches it
    char *text; // heap copy; owned here (for a VCD favourite this is the .VCD basename / launch name)
} fav_rec_t;

static fav_rec_t *favArray = NULL;
static int favCount = 0;

static char *favStrdup(const char *s)
{
    int n = (int)strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p != NULL)
        memcpy(p, s, n);
    return p;
}

// ---- on-disk path -------------------------------------------------------------

// favourites.bin lives next to the master config in the SHARED OPL config dir (not renamed to
// riptopl) so favourites carry across OPL / uOPL / wOPL -- only our settings file is private.
static void favGetFilePath(char *out, int outSize)
{
    config_set_t *cfg = configGetByType(CONFIG_OPL);
    const char *fn = (cfg != NULL && cfg->filename != NULL) ? cfg->filename : "mc0:OPL/" CONFIG_OPL_FILENAME;
    const char *base = CONFIG_OPL_FILENAME;
    int len = (int)strlen(fn);
    int blen = (int)strlen(base);
    if (len >= blen && strcmp(fn + len - blen, base) == 0) {
        int dirLen = len - blen;
        if (dirLen > outSize - 1)
            dirLen = outSize - 1;
        memcpy(out, fn, dirLen);
        out[dirLen] = '\0';
        strncat(out, "favourites.bin", outSize - strlen(out) - 1);
    } else {
        snprintf(out, outSize, "mc0:OPL/favourites.bin");
    }
}

// ---- explicit little-endian scalar IO (never raw-struct) ----------------------

static int rdBytes(int fd, void *buf, int n) { return read(fd, buf, n) == n; }

static int rdU16(int fd, u16 *v)
{
    u8 b[2];
    if (!rdBytes(fd, b, 2))
        return 0;
    *v = (u16)(b[0] | (b[1] << 8));
    return 1;
}
static int rdU32(int fd, u32 *v)
{
    u8 b[4];
    if (!rdBytes(fd, b, 4))
        return 0;
    *v = (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
    return 1;
}

// ---- raw on-disk record (pre-validation) --------------------------------------

typedef struct
{
    int mode;
    int id;
    int icon_id;
    int text_id;
    int kind; // FAV_KIND_*; v1 files and foreign imports default to FAV_KIND_ISO
    char text[FAV_TEXT_MAX];
} fav_raw_t;

// Little-endian signed 32-bit from a byte buffer (for the foreign-format import below).
static int rdS32le(const u8 *b)
{
    return (int)((u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24));
}

// Translate a uOPL/wOPL IO_MODES value to ours. Their enum differs: they have only 5 BDM slots
// (0..4) then ETH=5, HDD=6, APP=7, FAV=8, MMCE=9; we have 8 BDM slots then ETH=8, HDD=9,
// APP=10, MMCE=11, FAV=12. Without this, a foreign ETH/HDD/APP favourite (5/6/7) would be read
// as one of OUR BDM slots and silently fail to resolve. BDM slots pass through unchanged --
// favResolve re-matches a BDM favourite across all of our slots by id+text anyway.
static int favMapWoplMode(int m)
{
    switch (m) {
        case 5:
            return ETH_MODE;
        case 6:
            return HDD_MODE;
        case 7:
            return APP_MODE;
        case 8:
            return FAV_MODE; // favourite of the FAV tab itself; favResolve will reject it
        case 9:
            return MMCE_MODE;
        default:
            return m; // 0..4 are BDM slots in both schemes; anything else passes through
    }
}

// Import a uOPL/wOPL favourites.bin (read-only). Their format is a header-less stream of
// records, each = a raw 32-byte submenu_item_t (we trust only icon_id@0, text_id@8, id@12; the
// on-disk text/cache/owner POINTERS are garbage and ignored), then int text_len, then the text
// bytes, then a short owner-mode. uOPL and wOPL share this exact layout. We never WRITE it --
// our own writes use the hardened OFAV format -- so this is a one-way carry-over that lets
// favourites set in those builds appear in RiptOPL. Returns NULL on empty/corrupt input.
#define WOPL_FAV_STRUCT_SIZE 32 // sizeof(submenu_item_t) on the EE: 8 x 4-byte fields
static fav_raw_t *favReadWoplFile(int fd, int *outCount)
{
    *outCount = 0;
    fav_raw_t *recs = (fav_raw_t *)calloc(FAV_MAX_ITEMS, sizeof(fav_raw_t));
    if (recs == NULL)
        return NULL;

    int got = 0;
    while (got < FAV_MAX_ITEMS) {
        u8 st[WOPL_FAV_STRUCT_SIZE];
        u32 tlen = 0;
        u16 mode = 0;
        if (!rdBytes(fd, st, WOPL_FAV_STRUCT_SIZE))
            break; // clean EOF (or short tail) -> stop, keep what we have
        if (!rdU32(fd, &tlen))
            break;
        if (tlen == 0 || tlen > FAV_TEXT_MAX) {
            LOG("FAV import: bad text_len=%u (not uOPL/wOPL format?), aborting\n", (unsigned)tlen);
            break; // desync / foreign struct size / corruption -> stop
        }
        if (!rdBytes(fd, recs[got].text, tlen))
            break;
        recs[got].text[tlen - 1] = '\0'; // tlen includes the NUL; force-terminate
        if (!rdU16(fd, &mode))
            break;
        recs[got].icon_id = rdS32le(st + 0);
        recs[got].text_id = rdS32le(st + 8);
        recs[got].id = rdS32le(st + 12);
        recs[got].mode = favMapWoplMode((int)(short)mode); // owner mode (signed short on disk) -> our IO_MODES
        got++;
    }

    if (got == 0) {
        free(recs);
        return NULL;
    }
    LOG("FAV imported %d favourite(s) from uOPL/wOPL format\n", got);
    *outCount = got;
    return recs;
}

// Read + bounds-check the whole file into a heap array of raw records. Returns NULL (and
// *outCount = 0) on any error/corruption. Caller frees the array.
static fav_raw_t *favReadFile(int *outCount)
{
    char path[256];
    *outCount = 0;
    favGetFilePath(path, sizeof(path));

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    u32 magic = 0;
    u16 ver = 0, cnt = 0;
    int hdrOk = rdU32(fd, &magic) && rdU16(fd, &ver) && rdU16(fd, &cnt);
    if (!hdrOk || magic != FAV_MAGIC) {
        // No OFAV header -> this may be a uOPL/wOPL favourites.bin in the shared OPL dir.
        // Rewind and import it read-only so favourites carry over from those builds.
        LOG("FAV: no OFAV header (magic=%08x) -- attempting uOPL/wOPL import\n", (unsigned)magic);
        lseek(fd, 0, SEEK_SET);
        fav_raw_t *imported = favReadWoplFile(fd, outCount);
        close(fd);
        return imported;
    }
    if (ver != 1 && ver != 2) {
        LOG("FAV reject: unsupported OFAV version %d\n", ver);
        close(fd);
        return NULL;
    }
    if (cnt == 0) {
        close(fd);
        return NULL;
    }
    if (cnt > FAV_MAX_ITEMS)
        cnt = FAV_MAX_ITEMS; // never trust the stored count beyond the hard cap

    fav_raw_t *recs = (fav_raw_t *)calloc(cnt, sizeof(fav_raw_t));
    if (recs == NULL) {
        close(fd);
        return NULL;
    }

    int got = 0;
    for (int i = 0; i < (int)cnt; i++) {
        u16 mode = 0, tlen = 0;
        u32 id = 0, icon = 0, tid = 0;
        u8 kindByte = 0;
        if (!rdU16(fd, &mode) || !rdU32(fd, &id) || !rdU32(fd, &icon) || !rdU32(fd, &tid))
            break; // short read -> stop, keep what we have
        if (ver >= 2 && !rdBytes(fd, &kindByte, 1))
            break; // v2/v3 records carry a per-record byte between text_id and text_len
        if (!rdU16(fd, &tlen))
            break;
        if (tlen == 0 || tlen > FAV_TEXT_MAX) {
            LOG("FAV bad text_len=%d, aborting parse\n", tlen);
            break; // cannot resync past an unknown-length field -> stop
        }
        if (!rdBytes(fd, recs[got].text, tlen))
            break;                       // short read on text -> stop
        recs[got].text[tlen - 1] = '\0'; // tlen includes the NUL; force-terminate
        recs[got].mode = (int)mode;
        recs[got].id = (int)id;
        recs[got].icon_id = (int)icon;
        recs[got].text_id = (int)tid;
        // v1: no byte at all -> ISO. v2: the byte was a BOOLEAN isVcd, so 1 means VCD and 0
        // means "not VCD" -- which lumped app favourites in with disc games on one shelf. Now that
        // APPS has a shelf of its own, a v2 record whose source is the APPS tab is an ELF
        // favourite; that is the only record whose shelf changes on upgrade. v3 stores the kind
        // directly. Values we do not recognise fall back to ISO rather than being dropped.
        if (ver >= 3)
            recs[got].kind = (kindByte <= FAV_KIND_ELF) ? (int)kindByte : FAV_KIND_ISO;
        else if (kindByte)
            recs[got].kind = FAV_KIND_VCD;
        else
            recs[got].kind = ((int)mode == APP_MODE) ? FAV_KIND_ELF : FAV_KIND_ISO;
        got++;
    }
    close(fd);

    if (got == 0) {
        free(recs);
        return NULL;
    }
    *outCount = got;
    return recs;
}

// Little-endian scalar appenders for the in-memory image favWriteFile builds.
static void bufU16(u8 *b, int *o, u16 v)
{
    b[(*o)++] = (u8)(v & 0xff);
    b[(*o)++] = (u8)((v >> 8) & 0xff);
}

static void bufU32(u8 *b, int *o, u32 v)
{
    b[(*o)++] = (u8)(v & 0xff);
    b[(*o)++] = (u8)((v >> 8) & 0xff);
    b[(*o)++] = (u8)((v >> 16) & 0xff);
    b[(*o)++] = (u8)((v >> 24) & 0xff);
}

/* Write a raw-record array back out (explicit scalar fields; pointers never persisted).

   SERIALISE FIRST, THEN WRITE ONCE. This used to emit the file field by field: three writes for
   the header and SEVEN per record. Each one is an EE->IOP RPC round trip, so a library of 40
   favourites cost 283 of them -- and this runs on the GUI thread, straight off the R3 press in
   itemExecFav, which is why adding a favourite visibly stalled the menu. The byte count was never
   the problem; the syscall count was.

   The image is sized from the actual text lengths rather than FAV_MAX_ITEMS * FAV_TEXT_MAX, so a
   normal library allocates a few KB instead of 140. */
static int favWriteFile(fav_raw_t *recs, int count)
{
    char path[256];
    int i, off = 0, size = 8; // header: magic(4) + version(2) + count(2)
    int written, fd;
    u8 *img;

    if (count > FAV_MAX_ITEMS)
        count = FAV_MAX_ITEMS;

    for (i = 0; i < count; i++) {
        int tlen = (int)strlen(recs[i].text) + 1;
        if (tlen > FAV_TEXT_MAX)
            tlen = FAV_TEXT_MAX;
        size += 17 + tlen; // mode(2) id(4) icon(4) text_id(4) kind(1) text_len(2) + text
    }

    img = (u8 *)malloc(size);
    if (img == NULL) {
        LOG("FAV write: cannot allocate %d byte image\n", size);
        return 0;
    }

    bufU32(img, &off, FAV_MAGIC);
    bufU16(img, &off, FAV_VERSION);
    bufU16(img, &off, (u16)count);
    for (i = 0; i < count; i++) {
        int tlen = (int)strlen(recs[i].text) + 1;
        if (tlen > FAV_TEXT_MAX)
            tlen = FAV_TEXT_MAX;
        bufU16(img, &off, (u16)recs[i].mode);
        bufU32(img, &off, (u32)recs[i].id);
        bufU32(img, &off, (u32)recs[i].icon_id);
        bufU32(img, &off, (u32)recs[i].text_id);
        img[off++] = (u8)recs[i].kind; // same slot v2 used for isVcd, now a FAV_KIND_* (OFAV v3)
        bufU16(img, &off, (u16)tlen);
        memcpy(img + off, recs[i].text, tlen - 1);
        img[off + tlen - 1] = 0; // tlen counts the NUL, and a name capped at FAV_TEXT_MAX has none
        off += tlen;
    }

    // Open as late as possible. O_TRUNC empties the file the instant it succeeds, so anything that
    // can fail belongs above it -- otherwise a failure here trades a good list for an empty one.
    favGetFilePath(path, sizeof(path));
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        LOG("FAV write open failed: %s\n", path);
        free(img);
        return 0;
    }
    written = write(fd, img, off);
    close(fd);
    free(img);

    if (written != off) {
        LOG("FAV write incomplete: %d of %d bytes\n", written, off);
        return 0;
    }
    return 1;
}

// ---- in-memory array lifecycle ------------------------------------------------

static void favFreeArray(void)
{
    if (favArray != NULL) {
        for (int i = 0; i < favCount; i++)
            free(favArray[i].text);
        free(favArray);
        favArray = NULL;
    }
    favCount = 0;
}

// Resolve a stored record to a live source list + mark the source item favourited. BDM-range
// modes are re-matched across all 8 slots (hotplug / different bus). Returns the resolved
// owner (or NULL if the source isn't loaded / the item is absent).
// Best-effort star for a resolved VCD favourite: if the source device is currently in its VCD view,
// its submenu IS the VCD list, so light the star on the matching VCD item (by id+text). Misses
// harmlessly (no star, no change) when the device is in ISO view or the VCD list index has since
// shifted -- purely cosmetic on the source page; the FAV record + launch are unaffected either way.
static void favVcdMarkStar(opl_io_module_t *mod, int id, const char *text)
{
    if (mod == NULL || mod->support == NULL || libViewActive(mod->support->mode) != LIB_VIEW_PS1)
        return;
    submenu_list_t *src = submenuFindItemByIdAndText(mod->subMenu, id, text);
    if (src != NULL)
        src->item.favourited = 1;
}

// An APP favourite's stored id is the row index into the single AGGREGATED appsList (legacy
// conf_apps entries first, then the device scan in mount order) -- add a stick, edit conf_apps, or
// change APPS folders and every later index shifts, so a strict id+text match dies FOREVER for those
// records. Games never have this problem (a game's id is stable within its device list, and BDM even
// gets a lenient cross-slot re-match). For apps the stable identity is the TITLE (the conf_apps key /
// title.cfg value), so apps match by mode+text and ignore the stored id everywhere: resolve (below),
// the add-time duplicate check, and removal. Nathan's "Apps didn't seem to have functioning
// R3/Favorites" (HW, 2026-07-16).
static int favIdsMatchForMode(int mode, int recId, int liveId)
{
    return (mode == APP_MODE) || (recId == liveId);
}

// Title-only submenu walk for the APP fallback: first row whose text matches wins. Titles are the
// apps' identity key already (a duplicate title in conf_apps overwrites in config parsing).
static submenu_list_t *favFindItemByText(submenu_list_t *sub, const char *text)
{
    if (text == NULL)
        return NULL;
    for (submenu_list_t *cur = sub; cur != NULL; cur = cur->next) {
        if (cur->item.text != NULL && strcmp(cur->item.text, text) == 0)
            return cur;
    }
    return NULL;
}

int favKindView(int kind)
{
    switch (kind) {
        case FAV_KIND_VCD:
        case FAV_KIND_CUE:
            return LIB_VIEW_PS1; // both PS1 cores share one shelf, as they do on a device page
        case FAV_KIND_ELF:
            return LIB_VIEW_ELF;
        default:
            return LIB_VIEW_ISO;
    }
}

// A PS1 favourite is name-addressed (art, config and launch all key off the stored text) and so is
// view-independent; an ISO or ELF one is id-addressed against its source's list.
static int favKindIsNameAddressed(int kind)
{
    return kind == FAV_KIND_VCD || kind == FAV_KIND_CUE;
}

static int favResolveStoredId(item_list_t *source, int id, const char *text, int kind, int *outId)
{
    if (source == NULL || text == NULL || outId == NULL || source->itemGetCount == NULL || source->itemGetName == NULL)
        return 0;

    // ETH has one live backing array whose contents follow the source page's L3 state, so a shallow
    // viewOverride cannot turn VCD-backed ethGames into the ISO list. Resolve that one mode through
    // ETH's private read-only ISO backing probe. Every other device keeps the existing proxy path.
    if (source->mode == ETH_MODE && kind == FAV_KIND_ISO)
        return ethResolveIsoFavourite(id, text, outId);

    item_list_t view = *source;
    view.viewOverride = favKindIsNameAddressed(kind) ? ITEM_VIEW_FORCE_PS1 : ITEM_VIEW_FORCE_ISO;
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

// Can this support launch a PS1 favourite of the wanted core by NAME?
static int favSupportLaunches(const item_list_t *support, int needCue)
{
    if (support == NULL)
        return 0;
    return needCue ? (support->itemLaunchCue != NULL) : (support->itemLaunchVcd != NULL);
}

static item_list_t *favResolve(int mode, int id, const char *text, int kind, int *outMode, int *outId)
{
    *outMode = mode;
    *outId = id;

    // PS1 favourites are name-addressed and already view-independent: art/config/launch all key off
    // the stored name. Bind one to a loaded source that can launch ITS core -- a POPSTARTER
    // favourite needs itemLaunchVcd, an Ember one needs itemLaunchCue -- without disturbing that
    // source page's current view. Binding a CUE favourite to a device with no Ember support would
    // produce a row whose X button cannot work.
    if (favKindIsNameAddressed(kind)) {
        const int needCue = (kind == FAV_KIND_CUE);
        if (mode >= BDM_MODE && mode <= BDM_MODE_LAST) {
            opl_io_module_t *mod = oplGetModule(mode);
            if (mod != NULL && mod->support != NULL && favSupportLaunches(mod->support, needCue)) {
                favVcdMarkStar(mod, id, text);
                return mod->support;
            }
            for (int m = BDM_MODE; m <= BDM_MODE_LAST; m++) {
                opl_io_module_t *bm = oplGetModule(m);
                if (bm != NULL && bm->support != NULL && favSupportLaunches(bm->support, needCue)) {
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
        if (mod == NULL || mod->support == NULL || !favSupportLaunches(mod->support, needCue))
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
                if (libViewActive(mod->support->mode) != LIB_VIEW_PS1) {
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

    if (libViewActive(mod->support->mode) != LIB_VIEW_PS1) {
        submenu_list_t *src = submenuFindItemByIdAndText(mod->subMenu, liveId, text);
        if (src != NULL)
            src->item.favourited = 1;
    }
    *outId = liveId;
    return mod->support;
}

// ---- item_list_t callbacks ----------------------------------------------------

static int favGetTextId(item_list_t *itemList) { return _STR_FAV; }
static int favGetIconId(item_list_t *itemList) { return FAV_ICON; }

// The FAV list is rebuilt on demand only. favForceUpdate is raised by loadFavourites (initial
// boot, every source-list change, and each R3 toggle) and consumed exactly once here, so the
// deferred-update driver (menuDeferredUpdate -> updateMenuFromGameList) actually runs
// favUpdateItemList. Starting at 1 makes the FIRST deferred pass populate the tab; a
// self-clearing one-shot -- never a constant 1 -- avoids a per-frame rebuild storm.
static int favForceUpdate = 1;

static int favNeedsUpdate(item_list_t *itemList)
{
    // Consume the FAV L3 ISO<->VCD dirty flag UNCONDITIONALLY (and first) so a concurrent
    // favForceUpdate rebuild also clears it -- otherwise a default-view change that raises both would
    // trigger one redundant byte-identical re-read on the following pass.
    int viewToggled = libViewConsumeDirty(FAV_MODE);
    if (favForceUpdate) {
        favForceUpdate = 0;
        return 1;
    }
    return viewToggled; // L3 toggled the FAV view -> rebuild so the list re-filters
}

static void favInit(item_list_t *itemList)
{
    itemList->enabled = 1;
}

// MUST drive a single append pass: rebuild the validated in-memory array and return its
// count. updateMenuFromGameList does the clear + append from this count -- we never clear or
// append the submenu here (that would double-drive the list).
static int favUpdateItemList(item_list_t *itemList)
{
    favFreeArray();

    int rawCount = 0;
    fav_raw_t *recs = favReadFile(&rawCount);
    if (recs == NULL)
        return 0;

    favArray = (fav_rec_t *)calloc(rawCount, sizeof(fav_rec_t));
    if (favArray == NULL) {
        free(recs);
        return 0;
    }

    // Resolve EVERY record before filtering the Favourites tab's independent ISO/VCD view. Resolution
    // also restores the star on the source submenu. Filtering first meant a saved VCD record was never
    // resolved while FAV itself was in ISO view, so rebuilding the HDD list on an L3 toggle erased the
    // visible star and loadFavourites() could not put it back (#495).
    int favShelf = libViewActive(FAV_MODE);
    for (int i = 0; i < rawCount; i++) {
        int resolvedMode = recs[i].mode;
        int resolvedId = recs[i].id;
        item_list_t *owner = favResolve(recs[i].mode, recs[i].id, recs[i].text, recs[i].kind, &resolvedMode, &resolvedId);

        // The FAV page keeps its own L3 ring (PS2 / PS1 / APPS): resolution above is global
        // star/state reconciliation; only display population is filtered to the current shelf.
        if (favKindView(recs[i].kind) != favShelf)
            continue;

        if (owner == NULL) {
            // APPS never populate on their own: gAPPStartMode defaults to MANUAL and nothing scans a
            // MANUAL tab until the user opens it, so every stored app favourite sat hidden ("device
            // not loaded") until the APPS tab happened to be visited that session -- while game
            // favourites resolved because the user starts their game tab every time. If the user has
            // app favourites, they've opted in: arm the apps scan ONCE per boot ourselves. appInit is
            // plain state init (safe on this IO thread, where config IO already runs), and the
            // deferred update it queues re-calls loadFavourites when the list lands (opl.c
            // menuDeferredUpdate), so the stars light through the normal resync -- no special path.
            if (recs[i].mode == APP_MODE) {
                static int favAppsArmed = 0;
                opl_io_module_t *appMod = oplGetModule(APP_MODE);
                if (!favAppsArmed && appMod != NULL && appMod->support != NULL && !appMod->support->enabled) {
                    favAppsArmed = 1;
                    appMod->support->itemInit(appMod->support);
                    ioPutRequest(IO_MENU_UPDATE_DEFFERED, &appMod->support->mode);
                }
            }
            continue; // device not loaded / item absent -> hidden (kept in the file)
        }

        char *txt = favStrdup(recs[i].text);
        if (txt == NULL)
            continue; // OOM -> skip this record rather than store a NULL name

        favArray[favCount].owner = owner;
        favArray[favCount].mode = resolvedMode;
        favArray[favCount].id = resolvedId;
        favArray[favCount].icon_id = recs[i].icon_id;
        favArray[favCount].text_id = recs[i].text_id;
        favArray[favCount].kind = recs[i].kind;
        favArray[favCount].text = txt;
        favCount++;
    }

    free(recs);
    return favCount;
}

static int favGetItemCount(item_list_t *itemList) { return favCount; }

static int favValidIndex(int id) { return (favArray != NULL && id >= 0 && id < favCount); }

// Build a stack-local view of a favourite's LIVE source support. Only viewOverride changes; priv,
// callbacks, flags and owner all come from the current source object. This lets Favourites keep its
// ISO/VCD split independent from the source page's own L3 state without copying or mutating device
// state. The returned pointer is valid only as long as `view` remains in scope.
static item_list_t *favOwnerView(int id, item_list_t *view)
{
    if (!favValidIndex(id) || view == NULL || favArray[id].owner == NULL)
        return NULL;

    *view = *favArray[id].owner;
    // A PS1 favourite reads its source's retained PS1 array (which holds both cores' rows) even
    // while that source page shows PS2; anything else reads the source's own base list. An ELF
    // favourite's source is the APPS tab, which has one list and ignores the override entirely.
    view->viewOverride = favKindIsNameAddressed(favArray[id].kind) ? ITEM_VIEW_FORCE_PS1 : ITEM_VIEW_FORCE_ISO;
    return view;
}

// Source device mode (APP_MODE / HDD_MODE / BDM range / ...) of the FAV item at FAV-list index
// id, or -1 if id is out of range. Lets the theme engine redirect e.g. an APP favourite to the
// apps element (correct art folder + case overlay) instead of the game cover element.
int favGetItemSourceMode(int id)
{
    return favValidIndex(id) ? favArray[id].mode : -1;
}

// Guard the stored source id against the owner's CURRENT count -- the source list may have
// shrunk / re-scanned since the favourite was validated, so re-check before every proxy call
// to avoid indexing the owner's game array out of bounds.
static int favOwnerHasId(item_list_t *o, int id)
{
    return (o != NULL && o->itemGetCount != NULL && id >= 0 && id < o->itemGetCount(o));
}

static char *favGetItemName(item_list_t *itemList, int id)
{
    return favValidIndex(id) ? favArray[id].text : "";
}

static int favGetItemNameLength(item_list_t *itemList, int id)
{
    return favValidIndex(id) ? ((int)strlen(favArray[id].text) + 1) : 0;
}

static char *favGetItemStartup(item_list_t *itemList, int id)
{
    if (!favValidIndex(id))
        return "";
    if (favKindIsNameAddressed(favArray[id].kind))
        return favArray[id].text; // PS1 favourites key art/launch off the stored name, not a submenu id
    item_list_t ownerView;
    item_list_t *o = favOwnerView(id, &ownerView);
    if (o == NULL || o->itemGetStartup == NULL || !favOwnerHasId(o, favArray[id].id))
        return "";
    return o->itemGetStartup(o, favArray[id].id);
}

static config_set_t *favGetConfig(item_list_t *itemList, int id)
{
    if (!favValidIndex(id))
        return NULL;
    item_list_t ownerView;
    item_list_t *o = favOwnerView(id, &ownerView);
    if (o == NULL)
        return NULL;
    // VCD favourite: the owner's id-based config is the disc list (wrong list / wrong id while the
    // device is in ISO view). Build the PS1 config straight from the .VCD name + the device prefix,
    // exactly as sbPopulateConfig keys VCD per-game data by filename -> gives Title + #DiscType badge.
    if (favKindIsNameAddressed(favArray[id].kind)) {
        char *prefix = (o->itemGetPrefix != NULL) ? o->itemGetPrefix(o) : NULL;
        if (prefix == NULL)
            return NULL;
        int pl = (int)strlen(prefix);
        char sep[2] = {(pl > 0 && prefix[pl - 1] == '\\') ? '\\' : '/', '\0'};
        base_game_info_t game;
        memset(&game, 0, sizeof(game));
        snprintf(game.name, sizeof(game.name), "%s", favArray[id].text);
        // The extension is the ROW-KIND discriminator, exactly as on a device page: stamp the
        // core this favourite actually belongs to, or an Ember favourite would describe itself as a
        // POPSTARTER one to every consumer that inspects the row.
        snprintf(game.extension, sizeof(game.extension), "%s",
                 (favArray[id].kind == FAV_KIND_CUE) ? CUE_ROW_EXTENSION : ".VCD");
        game.parts = 1;
        game.format = GAME_FORMAT_ISO; // matches the device scans; the extension drives the PS1 badge
        return sbPopulateConfig(&game, prefix, sep);
    }
    if (o->itemGetConfig == NULL || !favOwnerHasId(o, favArray[id].id))
        return NULL;
    return o->itemGetConfig(o, favArray[id].id);
}

static void favLaunchItem(item_list_t *itemList, int id, config_set_t *configSet)
{
    if (!favValidIndex(id))
        return;
    item_list_t ownerView;
    item_list_t *o = favOwnerView(id, &ownerView);
    if (o == NULL)
        return;
    // PS1 favourite: hand the stored name to the owner's launcher for THIS row's core. Being
    // name-addressed, it works while the source page shows PS2 -- unlike the id-based itemLaunch,
    // whose PS1 branch is gated on the device being live in its PS1 view. favResolve only binds a
    // PS1 favourite to a device that provides the matching hook, so neither NULL check below should
    // fire for a resolved favourite.
    if (favKindIsNameAddressed(favArray[id].kind)) {
        if (favArray[id].kind == FAV_KIND_CUE) {
            if (o->itemLaunchCue != NULL)
                o->itemLaunchCue(o, favArray[id].text, configSet);
        } else {
            if (o->itemLaunchVcd != NULL)
                o->itemLaunchVcd(o, favArray[id].text, configSet);
        }
        return;
    }
    if (o->itemLaunch == NULL || !favOwnerHasId(o, favArray[id].id))
        return;
    o->itemLaunch(o, favArray[id].id, configSet);
}

// Art proxy: the cache passes the source item's startup as `value`. Find the favourite whose
// source startup matches and forward to its owner's image lookup.
static int favGetImage(item_list_t *itemList, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)
{
    if (favArray == NULL || value == NULL)
        return -1;
    for (int i = 0; i < favCount; i++) {
        item_list_t ownerView;
        item_list_t *o = favOwnerView(i, &ownerView);
        if (o == NULL)
            continue;
        // PS1 favourite: route both the primary name and the cache's strict-ID fallback through the
        // OWNER device's normal ART path, which already resolves the per-core in-library fallback by
        // row kind. The disc-ID fallback simply never matches an Ember folder name, which is correct.
        if (favKindIsNameAddressed(favArray[i].kind)) {
            if (strcmp(favArray[i].text, value) != 0) {
                char fallbackKey[VCD_ID_MAX];
                if (!vcdExtractGameId(favArray[i].text, fallbackKey, sizeof(fallbackKey)) ||
                    strcmp(fallbackKey, value) != 0)
                    continue;
            }
            if (o->itemGetImage == NULL)
                return -1;
            return o->itemGetImage(o, folder, isRelative, value, suffix, resultTex, psm);
        }
        if (o->itemGetStartup == NULL || o->itemGetImage == NULL || !favOwnerHasId(o, favArray[i].id))
            continue;
        char *s = o->itemGetStartup(o, favArray[i].id);
        if (s != NULL && strcmp(s, value) == 0) {
            int r = o->itemGetImage(o, folder, isRelative, value, suffix, resultTex, psm);
            // Relative (ART) requests return as-is -- the matched owner IS the item's owner. For an
            // absolute (theme-folder) request a bare -1 may only mean this owner can't key the path
            // (e.g. appsupport's startup-keyed lookup with an unresolved artMode); fall through to
            // the passthrough loop below, same as an unmatched request (CodeRabbit review of #255).
            if (isRelative || r != -1)
                return r;
        }
    }
    // Attribute-image passthrough (#213): theme attribute caches (Rating/Vmode/Scan/Players...) are
    // created with an ABSOLUTE prefix (the theme path, isPrefixRelative=0, themes.c) and key their
    // request on the attribute VALUE ("4", "ntsc", ...) -- not on an item startup, so the loop
    // above can never match them. Returning -1 here made texcache brand the theme's glyph
    // "genuinely absent" (ERR_BAD_FILE) and memoize the failure for the whole session, which is why
    // the Favourites info page showed only the placeholder while the normal info page -- same value,
    // same theme file, drawn through the source list's itemGetImage -- rendered fine. `folder` is
    // the absolute theme path, so ANY resolved owner's itemGetImage loads the identical file; only
    // the interface needs an owner. ART loads (isRelative=1, startup-keyed) are untouched.
    //
    // Keep scanning past a -1 (CodeRabbit review of #255): some owners' itemGetImage is itself
    // startup-keyed (appsupport's resolves `value` via appLookupByStartup) and returns -1 for an
    // attribute VALUE even though a later, device-backed owner would load the same absolute file
    // fine -- with an APP favourite sorted first, the Favourites info page still failed. A result
    // other than plain -1 is a real signal (success, or a transient error worth surfacing over a
    // bogus "absent"); a bare -1 may just mean "this owner can't key that path".
    if (!isRelative) {
        for (int i = 0; i < favCount; i++) {
            item_list_t ownerView;
            item_list_t *o = favOwnerView(i, &ownerView);
            if (o == NULL || o->itemGetImage == NULL ||
                (!favKindIsNameAddressed(favArray[i].kind) && !favOwnerHasId(o, favArray[i].id)))
                continue;
            int r = o->itemGetImage(o, folder, isRelative, value, suffix, resultTex, psm);
            if (r != -1)
                return r;
        }
    }
    return -1;
}

// Resolve an art-cache `value` (a source item's startup, or a VCD favourite's .VCD name /
// strict PS1 id) to the favourite's SOURCE device mode, using the same matching favGetImage
// uses to route the actual read. Lets texcache apply MMCE idle deferral, abort,
// and load priority to the device the read really lands on instead of the FAV
// wrapper mode. Returns -1 when no favourite matches (e.g. a theme attribute-image
// value, which takes the passthrough path in favGetImage). Pure in-memory scan, no IO.
//
// Thread safety: callers are GUI-thread render paths (prio 31), but favArray is rebuilt
// UNLOCKED by favUpdateItemList on the prio-30 IO worker (menuDeferredUpdate). Safe today
// because the IO thread outranks the GUI thread (a rebuild in progress keeps us off-CPU),
// favFreeArray NULLs favArray/favCount before the rebuild's first blocking point (favReadFile)
// so a scan started around a rebuild hits the NULL guard, entries become visible only after
// full population (favCount++ last), and the FAV submenu rows that drive these scans are torn
// down (clearMenuGameList) before the rebuild starts. Do NOT raise the GUI thread's priority
// to/above the IO worker's or add a yield inside favFreeArray without adding real
// synchronization here.
int favGetArtMode(const char *value)
{
    if (favArray == NULL || value == NULL)
        return -1;
    for (int i = 0; i < favCount; i++) {
        item_list_t ownerView;
        item_list_t *o = favOwnerView(i, &ownerView);
        if (o == NULL)
            continue;
        if (favKindIsNameAddressed(favArray[i].kind)) {
            if (strcmp(favArray[i].text, value) != 0) {
                char fallbackKey[VCD_ID_MAX];
                if (!vcdExtractGameId(favArray[i].text, fallbackKey, sizeof(fallbackKey)) ||
                    strcmp(fallbackKey, value) != 0)
                    continue;
            }
            return favArray[i].mode;
        }
        if (o->itemGetStartup == NULL || !favOwnerHasId(o, favArray[i].id))
            continue;
        char *s = o->itemGetStartup(o, favArray[i].id);
        if (s != NULL && strcmp(s, value) == 0)
            return favArray[i].mode;
    }
    return -1;
}

// Rename/Delete are blocked for favourites (also guarded at the menu level).
static void favDeleteItem(item_list_t *itemList, int id) {}
static void favRenameItem(item_list_t *itemList, int id, char *newName) {}

static void favCleanUp(item_list_t *itemList, int exception)
{
    // Intentionally does NOT free favArray: the FAV submenu's item.text aliases favArray
    // text, and the array is freed/rebuilt only by favUpdateItemList (after the submenu has
    // been cleared) or favShutdown. Freeing here could dangle a live submenu's text.
}

static void favShutdown(item_list_t *itemList)
{
    favFreeArray();
}

unsigned char favGetFlags(item_list_t *itemList)
{
    opl_io_module_t *mod = oplGetModule(FAV_MODE);
    if (mod == NULL || mod->menuItem.current == NULL)
        return 0;
    int id = mod->menuItem.current->item.id;
    if (!favValidIndex(id))
        return 0;
    // Prefer the SOURCE list's live flags so dynamic capabilities are forwarded -- e.g. a BDM
    // device backed by ATA only sets MODE_FLAG_COMPAT_DMA on itemList->flags after its scan, so
    // re-deriving from mode alone would hide the DMA compat option for ATA-backed favourites.
    item_list_t ownerView;
    item_list_t *o = favOwnerView(id, &ownerView);
    if (o != NULL)
        return o->flags;
    // Fallback by resolved mode if the owner pointer is somehow absent.
    int m = favArray[id].mode;
    if (m == APP_MODE)
        return MODE_FLAG_NO_COMPAT | MODE_FLAG_NO_UPDATE;
    if (m == HDD_MODE)
        return MODE_FLAG_COMPAT_DMA;
    return 0;
}

// VMC check proxy: itemCheckVMC is device-context (no id), so forward to the current FAV
// item's SOURCE device. Without this, the game-Options VMC menu would call a NULL callback.
static int favCheckVMC(item_list_t *itemList, char *name, int createSize)
{
    opl_io_module_t *mod = oplGetModule(FAV_MODE);
    if (mod == NULL || mod->menuItem.current == NULL)
        return -1;
    int id = mod->menuItem.current->item.id;
    if (!favValidIndex(id))
        return -1;
    item_list_t ownerView;
    item_list_t *o = favOwnerView(id, &ownerView);
    if (o == NULL || o->itemCheckVMC == NULL)
        return -1;
    return o->itemCheckVMC(o, name, createSize);
}

// Two modes "match" for favourite identity if equal, OR both in the BDM range
// (USB/iLink/MX4SIO/ATA slots are interchangeable -- a BDM favourite can move slots).
static int favModesMatch(int a, int b)
{
    int aBdm = (a >= BDM_MODE && a <= BDM_MODE_LAST);
    int bBdm = (b >= BDM_MODE && b <= BDM_MODE_LAST);
    if (aBdm && bBdm)
        return 1;
    return a == b;
}

// ---- public toggle / refresh API ----------------------------------------------

int addFavouriteItem(int mode, int id, int icon_id, int text_id, const char *text, int kind)
{
    if (text == NULL || text[0] == '\0')
        return 0;

    int count = 0;
    fav_raw_t *recs = favReadFile(&count); // may be NULL (empty / new file)

    // Already present (same mode + id + text + kind) -> treat as success (the star stays set).
    // Use favModesMatch so a BDM favourite that moved slots (e.g. BDM_MODE -> BDM_MODE1) is
    // recognised as already-present, matching the BDM-lenient logic in removeFavouriteByIdAndText.
    // kind is part of the identity so a disc favourite and a PS1 favourite never collide.
    for (int i = 0; i < count; i++) {
        // favIdsMatchForMode: apps identify by title (their stored id shifts with the device set) --
        // without it, a shifted app would collect a DUPLICATE record on every re-favourite.
        if (favModesMatch(recs[i].mode, mode) && favIdsMatchForMode(mode, recs[i].id, id) && recs[i].kind == kind && strcmp(recs[i].text, text) == 0) {
            free(recs);
            return 1;
        }
    }

    int newCount = count + 1;
    if (newCount > FAV_MAX_ITEMS) {
        free(recs);
        return 0;
    }
    fav_raw_t *out = (fav_raw_t *)calloc(newCount, sizeof(fav_raw_t));
    if (out == NULL) {
        free(recs);
        return 0;
    }
    for (int i = 0; i < count; i++)
        out[i] = recs[i];
    out[count].mode = mode;
    out[count].id = id;
    out[count].icon_id = icon_id;
    out[count].text_id = text_id;
    out[count].kind = kind;
    snprintf(out[count].text, FAV_TEXT_MAX, "%s", text);

    int ok = favWriteFile(out, newCount);
    free(out);
    free(recs);
    return ok;
}

int removeFavouriteByIdAndText(int mode, int id, const char *text, int kind)
{
    int count = 0;
    fav_raw_t *recs = favReadFile(&count);
    if (recs == NULL)
        return 0;

    int survivors = 0;
    for (int i = 0; i < count; i++) {
        // Match on id + text + mode (BDM-lenient) + kind so a same-titled favourite in a different
        // device mode -- or a disc vs PS1 favourite of the same title -- is NOT collaterally deleted.
        // Apps match by title alone (favIdsMatchForMode): their stored id shifts with the device set,
        // and without the leniency R3 on an already-starred app could not un-favourite it.
        if (!(favIdsMatchForMode(mode, recs[i].id, id) && text != NULL && strcmp(recs[i].text, text) == 0 && favModesMatch(recs[i].mode, mode) && recs[i].kind == kind))
            recs[survivors++] = recs[i]; // compact in place (single buffer)
    }
    int ok = favWriteFile(recs, survivors); // survivors may be 0 -> writes a header-only (empty) file
    free(recs);
    return ok;
}

void favRemoveByIndex(int favIndex)
{
    if (!favValidIndex(favIndex))
        return;
    int srcMode = favArray[favIndex].mode;
    int srcId = favArray[favIndex].id;
    int srcKind = favArray[favIndex].kind;
    char *txt = favArray[favIndex].text;

    // Clear the star on the source-list copy, then drop the record from the store. (For a VCD fav the
    // live submenu may be the device's disc list right now -> the by-id+text find simply misses, which
    // is fine -- the record is still removed from the store.)
    opl_io_module_t *mod = oplGetModule(srcMode);
    if (mod != NULL) {
        submenu_list_t *src = submenuFindItemByIdAndText(mod->subMenu, srcId, txt);
        if (src != NULL)
            src->item.favourited = 0;
    }
    removeFavouriteByIdAndText(srcMode, srcId, txt, srcKind);
}

void loadFavourites(void)
{
    // Mark the FAV list stale and schedule its single canonical rebuild. The clear + re-append
    // happen together inside the deferred updateMenuFromGameList (favNeedsUpdate consumes the
    // one-shot), so we must NOT clear here: an eager clear on every source refresh would blank
    // the tab (and reset its cursor) even when the favourites set did not change.
    favForceUpdate = 1;
    ioPutRequest(IO_MENU_UPDATE_DEFFERED, &favItemList.mode);
}

item_list_t *favGetObject(int initOnly)
{
    if (initOnly && !favItemList.enabled)
        return NULL;
    return &favItemList;
}

static item_list_t favItemList = {
    FAV_MODE, -1, 0, 0, MENU_MIN_INACTIVE_FRAMES, FAV_MODE_UPDATE_DELAY, NULL, NULL, &favGetTextId, NULL, &favInit, &favNeedsUpdate, &favUpdateItemList,
    &favGetItemCount, NULL, &favGetItemName, &favGetItemNameLength, &favGetItemStartup, &favDeleteItem, &favRenameItem, &favLaunchItem,
    &favGetConfig, &favGetImage, &favCleanUp, &favShutdown, &favCheckVMC, &favGetIconId};
