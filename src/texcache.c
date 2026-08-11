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
        LOG("ART TAR: out of memory for '%s' (%u bytes)\n", prefix, entry->rawSize);
        return -1;
    }

    if (tarRead(TAR_KIND_ART, entry, buffer, entry->rawSize) != entry->rawSize) {
        LOG("ART TAR: short read for '%s' (%u bytes expected)\n", prefix, entry->rawSize);
        free(buffer);
        return -1;
    }

    result = texLoadFromMemory(texture, buffer, entry->rawSize);
    if (result < 0)
        LOG("ART TAR: '%s' found (%u bytes) but PNG decode failed (%d)\n", prefix, entry->rawSize, result);
    free(buffer);
    return result;
}

// Most art requests the queue may hold before new ones are refused. Deliberately small: art is
// re-requested every frame the item stays visible, so a refusal costs one frame of latency on that
// tile, while a deep queue costs the USER seconds on the next thing they do. Non-art requests are
// rare, so this doubles as the art cap without a per-type counter.
#define ART_MAX_QUEUE_DEPTH 4

// Missing-art memo (single-slot hash, keyed startup_suffix): a load that FAILED once is not
// re-probed every delay period -- on a device with sparse art the old retry loop re-open()ed the
// same absent files forever (#120-adjacent churn). One bucket per hash: a collision merely evicts
// an older miss, never blocks a hit. Cleared on applyConfig and on every list rebuild
// (updateMenuFromGameList), which are exactly the moments new art can have appeared.
// Cross-thread by design (worker writes, GUI reads): slots are fixed-size and always
// NUL-terminated, so a torn read can only mis-answer one frame, never fault.
#define FAIL_MEMO_SIZE 256

typedef struct
{
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

// Art requests currently sitting in the ioman queue. LOCK-FREE on purpose, and that is the whole
// story of this counter: the first backpressure gate here called ioGetPendingRequestCount(), whose
// WaitSema(gProcSemaId) blocks until the worker finishes draining the ENTIRE queue -- the worker
// holds that sema across the whole BATCH, not per request. Called from cacheGetTexture on the GUI
// thread, that froze the menu for as long as any art was in flight: the hardware 'everything
// turns to chit' / black-screen-after-save reports. A plain volatile int the producer increments
// and the handler decrements needs no lock; a lost update skews the gate by one for a frame, and
// the gate self-heals by resetting when the queue is observed empty.
static volatile int gArtQueuedCount = 0;

// The load the worker is executing RIGHT NOW, which gArtQueuedCount can no longer see: the counter
// above is decremented on entry to cacheLoadImage (it balances the enqueue, once per request), so
// between that decrement and the end of the decode "queued" reads zero while the device is very
// much busy. Single writer (the IO worker), read by the GUI thread in cacheHasPendingArt.
static volatile int gArtActiveCount = 0;

// Is any cover art queued or being read/decoded? Used by menuUpdateHook to keep background device
// rescans OUT of the shared IO queue while art is still arriving -- without it, a settle enqueues a
// batch of per-device probes that the worker serializes AHEAD of the covers the user is waiting on,
// so the art keeps falling further behind on every scroll-and-stop cycle. Lock-free by the same
// argument as gArtQueuedCount: a torn read mis-answers one frame and self-corrects on the next.
int cacheHasPendingArt(void)
{
    return (gArtQueuedCount > 0) || (gArtActiveCount > 0);
}

GSTEXTURE *cacheGetTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value)
{
    return cacheGetTextureEx(cache, list, cacheId, UID, value, 0);
}

// LOOKUP ONLY: hand back this item's texture if its slot is still live, and never claim a slot,
// never queue a load, never touch the fail memo. For a caller that wants to keep DRAWING art it
// already has while deliberately not asking for more -- without this, "don't request" also means
// "don't draw", and a row that already had its thumbnail would drop back to the placeholder.
GSTEXTURE *cacheLookupTexture(image_cache_t *cache, int *cacheId, int *UID)
{
    if (cache == NULL || cacheId == NULL || UID == NULL)
        return NULL;

    if (*cacheId < 0 || *cacheId >= cache->count)
        return NULL;

    cache_entry_t *entry = &cache->content[*cacheId];
    if (entry->UID != *UID || entry->qr != NULL || entry->lastUsed == 0 || entry->texture.Mem == NULL)
        return NULL;

    entry->lastUsed = guiFrameId; // still on screen: keep it away from the eviction scan
    return &entry->texture;
}

// Io handled action...
static void cacheLoadImage(void *data)
{
    // Balance the enqueue-side increment FIRST: this handler runs exactly once per queued request,
    // including every early-out below.
    if (gArtQueuedCount > 0)
        gArtQueuedCount--;

    load_image_request_t *req = data;

    // Safeguards...
    if (!req)
        return;

    // Every early-out below must FREE the request. It is a single allocation (the struct with its
    // value string appended), owned solely by this handler once ioPutRequest accepted it, and the
    // entry's qr back-pointer is cleared by cacheClearItem -- so nothing else can reach it and
    // returning without free() simply loses the memory.
    if (!req->entry || !req->cache) {
        free(req);
        return;
    }

    item_list_t *handler = req->list;
    if (!handler) {
        free(req);
        return;
    }

    // The cache entry was already reused (or cleared: cacheClearItem zeroes UID and nulls qr, which
    // is what a list rebuild or theme switch does to every slot). This request is now orphaned --
    // and it is NOT a rare path: background rescans rebuild lists while the user browses, so every
    // rebuild used to leak one of these per art load still in flight.
    if (req->cacheUID != req->entry->UID) {
        free(req);
        return;
    }

    // seems okay. we can proceed. From here to the qr release below this request owns a device
    // read + PNG decode: count it ACTIVE so the background-rescan gate keeps seeing "art pending".
    // Every exit path between here and the decrement must go through the end of this function --
    // the early-outs above are all before the increment on purpose.
    gArtActiveCount++;

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

    if (gArtActiveCount > 0)
        gArtActiveCount--;

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

// priority != 0 means "this is on screen right now, not a guess" -- see the backpressure gate below.
GSTEXTURE *cacheGetTextureEx(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value, int priority)
{
    if (cache == NULL || list == NULL || cacheId == NULL || UID == NULL || value == NULL || value[0] == '\0')
        return NULL;

    // NO memo lookup here. This function runs for EVERY art element on screen EVERY frame --
    // including covers that are already loaded and rendering -- and the memo check costs a
    // snprintf + hash + strcmp. Placed here it taxed every cached frame (dozens of format-string
    // calls per frame on a 294 MHz CPU); the fast path below must stay a couple of integer
    // compares, exactly what the hardware-validated builds did. The memo is consulted at the
    // ENQUEUE decision instead -- the only place it saves anything real (an IO attempt).

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

    // Known-missing art: skip the slot claim and the IO attempt entirely. Reached only for rows
    // with no live cache entry, after the idle delay -- i.e. exactly where the old code would have
    // issued a doomed open(); the memo is strictly cheaper than the IO it replaces. The row's
    // cacheId stays -1, so once the memo is cleared (applyConfig / list rebuild) it retries
    // naturally.
    if (cacheIsFailMemo(value, cache->suffix))
        return NULL;

    cache_entry_t *currEntry, *oldestEntry = NULL;
    int i, rtime = guiFrameId, inFlight = 0;

    for (i = 0; i < cache->count; i++) {
        currEntry = &cache->content[i];
        if (currEntry->qr)
            inFlight++;
        else if (currEntry->lastUsed < rtime) {
            oldestEntry = currEntry;
            rtime = currEntry->lastUsed;
            *cacheId = i;
        }
    }

    // ONE IN FLIGHT for the small (floored) caches. Their hardware-validated form had a single
    // slot, which made "at most one load in flight" a structural property: scrolling past ten
    // games never queued ten backgrounds. The rebuild-71 redundancy slot silently removed that --
    // a passed-by game's BG could claim the second slot mid-scroll, and the settled game's
    // background then waited out that STALE load (the largest art item there is) before its own
    // started. Refusing the claim while a sibling load is in flight restores the old pipeline
    // semantics while keeping the redundancy slot for its failure-recovery purpose. Multi-slot
    // cover caches (10/20 slots) are deliberately untouched: they always had several in flight,
    // and that behaviour is hardware-validated as-is.
    if (oldestEntry && cache->count <= 2 && inFlight > 0) {
        *cacheId = -1; // nothing claimed; the next frame simply asks again
        return NULL;
    }

    // BACKPRESSURE, lock-free (see gArtQueuedCount above). Art is the one request type that is
    // free to drop: the slot is never claimed, and the next frame simply asks again. Refusing to
    // deepen an already-deep queue keeps the config save, the deferred menu rebuild and device
    // init from starving behind hundreds of queued art reads on a slow device.
    // PRIORITY bypasses the cap. The art the user is LOOKING AT (the selected row's cover) is not
    // speculative: refusing it means the one image on screen that matters waits for a queue full of
    // guesses about neighbouring rows to drain first -- on a slow device, seconds of staring at a
    // placeholder while the machine reads covers nobody asked for. There is at most one such
    // request per row change, so the cap still bounds the queue in every practical sense.
    if (oldestEntry && !priority && gArtQueuedCount >= ART_MAX_QUEUE_DEPTH) {
        if (!ioHasPendingRequests())
            gArtQueuedCount = 0; // drift self-heal: queue is empty, the counter must be too
        else {
            *cacheId = -1;
            return NULL;
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
        // DIntr bracket: the ++ is a read-modify-write on the GUI thread, and the HIGHER-priority
        // io worker's decrement can preempt it mid-RMW, losing the worker's update -- an
        // upward-only drift (adversarially quantified at ~one event per hours of browsing, and
        // self-healing, but the bracket costs nothing and sound.c already sets the idiom).
        DIntr();
        gArtQueuedCount++;
        EIntr();
        if (ioPutRequest(IO_CACHE_LOAD_ART, req) != IO_OK) {
            DIntr();
            if (gArtQueuedCount > 0)
                gArtQueuedCount--; // the request never entered the queue
            EIntr();
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
