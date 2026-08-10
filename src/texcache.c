#include "include/opl.h"
#include "include/texcache.h"
#include "include/textures.h"
#include "include/ioman.h"
#include "include/gui.h"
#include "include/util.h"
#include "include/renderman.h"
#include "include/tar.h"

typedef struct
{
    image_cache_t *cache;
    cache_entry_t *entry;
    item_list_t *list;
    // only for comparison if the deferred action is still valid
    int cacheUID;
    char *value;
} load_image_request_t;

// .tar art packs (checklist item 45). Tries "<value>_<suffix>.png" inside ART/art.tar before the
// normal per-file lookup. Opt-in: gEnableArtTar ships 0, so with the toggle off this is a single
// predictable branch and the art path is byte-for-byte the behaviour hardware has already validated.
//
// #340 note -- what this actually costs, stated in full because item 45 is a flagged suspect:
//   WHERE IT RUNS: cacheLoadImage is an ioman handler (ioRegisterHandler/ioPutRequest below), so all
//   of this executes on the IO worker thread at priority 30 -- never on the GUI/pad thread. The
//   caller also gates the enqueue behind the art idle delay (guiInactiveFrames < list->delay in
//   cacheGetTexture), so nothing here is even queued while the user is holding a direction.
//   ONE-SHOT COSTS on the first request after the toggle goes on:
//     - tarFind() lazily probes up to 13 devices (mass0-7, mmce0/1, hdd0:, pfs0:, smb0:) with one
//       open() each; mmce* and smb0: are the slow ones. tar.c's s_inactive[] latch stops a failed
//       probe from repeating.
//     - a successful parse then WRITES a sidecar index (art_cache.bin, ~64 bytes per entry) next to
//       the archive. That is a genuine write on the art path, unlike official's read-only one. It is
//       non-fatal (tarParseFile ignores the result) and happens only when the archive changes, but it
//       is the thing to instrument first if a .tar-enabled build ever feels worse than a plain one.
//   DEFAULT PATH: gEnableArtTar ships 0, so none of the above is reachable unless the user opts in.
static int artTarLoadImage(const char *value, const char *suffix, GSTEXTURE *texture)
{
    char prefix[64];
    TarEntryBase *entry = NULL;
    void *buffer;
    int result;

    if (snprintf(prefix, sizeof(prefix), "%s_%s.", value, suffix) >= (int)sizeof(prefix))
        return -1;

    entry = tarFindPrefix(TAR_KIND_ART, prefix);

    if (entry == NULL) {
        LOG("ART TAR: '%s_%s' not in the archive index\n", value, suffix);
        return -1;
    }

    buffer = malloc(entry->rawSize);
    if (buffer == NULL) {
        LOG("ART TAR: out of memory for '%s' (%u bytes)\n", name, entry->rawSize);
        return -1;
    }

    if (tarRead(TAR_KIND_ART, entry, buffer, entry->rawSize) != entry->rawSize) {
        LOG("ART TAR: short read for '%s' (%u bytes expected)\n", name, entry->rawSize);
        free(buffer);
        return -1;
    }

    result = texLoadFromMemory(texture, buffer, entry->rawSize);
    if (result < 0)
        LOG("ART TAR: '%s' found (%u bytes) but PNG decode failed (%d)\n", name, entry->rawSize, result);
    free(buffer);
    return result;
}

// Most art requests the queue may hold before new ones are refused. Deliberately small: art is
// re-requested every frame the item stays visible, so a refusal costs one frame of latency on that
// tile, while a deep queue costs the USER seconds on the next thing they do. Non-art requests are
// rare, so this doubles as the art cap without a per-type counter.
#define ART_MAX_QUEUE_DEPTH 4

// Io handled action...
static void cacheLoadImage(void *data)
{
    load_image_request_t *req = data;

    // Safeguards...
    if (!req || !req->entry || !req->cache)
        return;

    item_list_t *handler = req->list;
    if (!handler)
        return;

    // the cache entry was already reused!
    if (req->cacheUID != req->entry->UID)
        return;

    // seems okay. we can proceed
    GSTEXTURE *texture = &req->entry->texture;
    texFree(texture);

    int result = -1;

    if (gEnableArtTar)
        result = artTarLoadImage(req->value, req->cache->suffix, texture);

    // Fall through to the per-file lookup whenever the archive is off, absent, or lacks this key.
    if (result < 0)
        result = handler->itemGetImage(handler, req->cache->prefix, req->cache->isPrefixRelative, req->value, req->cache->suffix, texture, GS_PSM_CT24);

    if (result < 0) {
        req->entry->lastUsed = 0;
        cacheMemoFail(req->value, req->cache->suffix);
    } else
        req->entry->lastUsed = guiFrameId;

    req->entry->qr = NULL;

    free(req);
}

void cacheInit()
{
    ioRegisterHandler(IO_CACHE_LOAD_ART, &cacheLoadImage);
}

void cacheEnd()
{
    // nothing to do... others have to destroy the cache via cacheDestroyCache
}

static void cacheClearItem(cache_entry_t *item, int freeTxt)
{
    if (freeTxt && item->texture.Mem) {
        rmUnloadTexture(&item->texture);
        free(item->texture.Mem);
        if (item->texture.Clut)
            free(item->texture.Clut);
    }

    memset(item, 0, sizeof(cache_entry_t));
    item->texture.Mem = NULL;
    item->texture.Vram = 0;
    item->texture.Clut = NULL;
    item->texture.VramClut = 0;
    item->texture.ClutStorageMode = GS_CLUT_STORAGE_CSM1; // Default
    item->qr = NULL;
    item->lastUsed = -1;
    item->UID = 0;
}

image_cache_t *cacheInitCache(int userId, const char *prefix, int isPrefixRelative, const char *suffix, int count)
{
    image_cache_t *cache = (image_cache_t *)malloc(sizeof(image_cache_t));
    cache->userId = userId;
    cache->count = count;
    cache->prefix = NULL;
    int length;
    if (prefix) {
        length = strlen(prefix) + 1;
        cache->prefix = (char *)malloc(length * sizeof(char));
        memcpy(cache->prefix, prefix, length);
    }
    cache->isPrefixRelative = isPrefixRelative;
    length = strlen(suffix) + 1;
    cache->suffix = (char *)malloc(length * sizeof(char));
    memcpy(cache->suffix, suffix, length);
    cache->nextUID = 1;
    cache->content = (cache_entry_t *)malloc(count * sizeof(cache_entry_t));

    int i;
    for (i = 0; i < count; ++i)
        cacheClearItem(&cache->content[i], 0);

    return cache;
}

void cacheDestroyCache(image_cache_t *cache)
{
    int i;
    for (i = 0; i < cache->count; ++i) {
        cacheClearItem(&cache->content[i], 1);
    }

    free(cache->prefix);
    free(cache->suffix);
    free(cache->content);
    free(cache);
}

#define FAIL_MEMO_SIZE 256

typedef struct {
    char key[64];
} fail_memo_entry_t;

static fail_memo_entry_t s_failMemo[FAIL_MEMO_SIZE];

static u32 hashFailKey(const char *str)
{
    u32 hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash % FAIL_MEMO_SIZE;
}

static void cacheMemoFail(const char *value, const char *suffix)
{
    char key[64];
    if (snprintf(key, sizeof(key), "%s_%s", value, suffix ? suffix : "") >= (int)sizeof(key))
        return;

    u32 idx = hashFailKey(key);
    snprintf(s_failMemo[idx].key, sizeof(s_failMemo[idx].key), "%s", key);
}

static int cacheIsFailMemo(const char *value, const char *suffix)
{
    char key[64];
    if (snprintf(key, sizeof(key), "%s_%s", value, suffix ? suffix : "") >= (int)sizeof(key))
        return 0;

    u32 idx = hashFailKey(key);
    return (strcmp(s_failMemo[idx].key, key) == 0);
}

void cacheInvalidateFailMemo(void)
{
    memset(s_failMemo, 0, sizeof(s_failMemo));
}

GSTEXTURE *cacheGetTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value)
{
    if (cache == NULL || list == NULL || cacheId == NULL || UID == NULL || value == NULL || value[0] == '\0')
        return NULL;

    if (cacheIsFailMemo(value, cache->suffix)) {
        *cacheId = -2;
        return NULL;
    }

    if (*cacheId == -2) {
        return NULL;
    } else if (*cacheId != -1) {
        cache_entry_t *entry = &cache->content[*cacheId];
        if (entry->UID == *UID) {
            if (entry->qr)
                return NULL;
            else if (entry->lastUsed == 0) {
                *cacheId = -2;
                return NULL;
            } else {
                entry->lastUsed = guiFrameId;
                return &entry->texture;
            }
        }

        *cacheId = -1;
    }

    // under the cache pre-delay (to avoid filling cache while moving around)
    if (guiInactiveFrames < list->delay)
        return NULL;

    cache_entry_t *currEntry, *oldestEntry = NULL;
    int i, rtime = guiFrameId;

    for (i = 0; i < cache->count; i++) {
        currEntry = &cache->content[i];
        if ((!currEntry->qr) && (currEntry->lastUsed < rtime)) {
            oldestEntry = currEntry;
            rtime = currEntry->lastUsed;
            *cacheId = i;
        }
    }

    if (oldestEntry) {
        load_image_request_t *req = malloc(sizeof(load_image_request_t) + strlen(value) + 1);
        req->cache = cache;
        req->entry = oldestEntry;
        req->list = list;
        req->value = (char *)req + sizeof(load_image_request_t);
        strcpy(req->value, value);
        req->cacheUID = cache->nextUID;

        cacheClearItem(oldestEntry, 1);
        oldestEntry->qr = req;
        oldestEntry->UID = cache->nextUID;

        *UID = cache->nextUID++;

        // A REFUSED request never runs, so nothing ever clears qr -- and qr is what marks this slot
        // "load in flight". Left set, the entry is permanently invisible to BOTH the read path and
        // the eviction scan above (which only considers entries with !qr), so the slot is dead for
        // the rest of the session and its art never appears again. Backgrounds and screenshots are
        // the visible casualties: their caches are ONE slot deep, so a single refusal kills the art
        // type outright, while a 10-slot cover cache just loses one of ten and shrugs it off.
        // Roll the slot back instead, leaving it free for the next frame to retry.
        // (Third instance of this discarded-return shape; see rebuild-38 and rebuild-43.)
        if (ioPutRequest(IO_CACHE_LOAD_ART, req) != IO_OK) {
            // Reuse the module's own definition of an empty slot rather than hand-setting fields
            // (cacheClearItem leaves lastUsed = -1, UID = 0). freeTxt = 0: the texture was already
            // released by the cacheClearItem(.., 1) above, and the entry has held nothing since.
            cacheClearItem(oldestEntry, 0);
            *cacheId = -1; // the cache API's "no entry" sentinel, as used by themes.c
            free(req);
        }
    }

    return NULL;
}
