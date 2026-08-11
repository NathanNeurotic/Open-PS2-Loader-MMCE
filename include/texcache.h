#ifndef __TEX_CACHE_H
#define __TEX_CACHE_H

#include "include/iosupport.h"

/// A single cache entry...
typedef struct
{
    GSTEXTURE texture;

    // NULL not queued, otherwise queue request record
    void *qr;

    // frame counter the icon was used the last time - oldest get rewritten first in case new icon is requested and cache is full. negative numbers mean
    // slot is free and can be used right now
    int lastUsed;

    int UID;
} cache_entry_t;


/// One texture cache instance
typedef struct
{
    /// User specified ID, not used in any way by the cache code (not even initialized!)
    int userId;

    /// count of entries (copy of the requested cache size upon cache initialization)
    int count;

    /// directory prefix for this cache (if any)
    char *prefix;
    int isPrefixRelative;
    char *suffix;

    int nextUID;

    /// the cache entries itself
    cache_entry_t *content;
} image_cache_t;

/** Initializes the cache subsystem.
 */
void cacheInit();

/** Terminates the cache. Does nothing currently. Users of this code have to destroy caches via cacheDestroyCache
 */
void cacheEnd();

/** Initializes a single cache
 */
image_cache_t *cacheInitCache(int userId, const char *prefix, int isPrefixRelative, const char *suffix, int count);

/** Destroys a given cache (unallocates all memory stored there, disconnects the pixmaps from the usage points).
 */
void cacheDestroyCache(image_cache_t *cache);

GSTEXTURE *cacheGetTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value);

/** As cacheGetTexture, but priority != 0 marks the request as art that is ON SCREEN NOW rather
 * than speculative, letting it past the queue-depth cap so the image the user is looking at never
 * waits for a queue of prefetched neighbours to drain.
 */
GSTEXTURE *cacheGetTextureEx(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value, int priority);

/** Lookup only: returns this item's texture if its cache slot is still live, and never claims a
 * slot or queues a load. For drawing art already held while deliberately not requesting more.
 */
GSTEXTURE *cacheLookupTexture(image_cache_t *cache, int *cacheId, int *UID);

/** Diagnostics for the debug HUD: art requests queued, being read/decoded right now, turned away by
 * the depth cap (cumulative), and completed (cumulative). Any argument may be NULL.
 */
void cacheDebugCounters(int *queued, int *active, int *refused, int *done);

/** Debug HUD: cost in ms of the last art load and of the last SUCCESSFUL one, plus that image's
 * decoded pixel dimensions. Separates "one load is slow" from "we ask for too many". Any argument
 * may be NULL.
 */
void cacheDebugLastLoad(int *lastMs, int *lastOkMs, int *width, int *height);

/** May speculative art (viewport warming, far-row thumbnails, the Coverflow lookahead) be issued
 * now? Looser than cacheHasPendingArt(): it allows prefetch while a few loads are already in
 * flight, so the device stays busy instead of prefetch running one image at a time.
 */
int cacheMayPrefetchArt(void);
void cacheInvalidateFailMemo(void);

/** Nonzero while any cover art is queued for, or currently being, read+decoded by the IO worker.
 * menuUpdateHook uses it to keep background device rescans out of the shared queue until the art
 * the user is waiting on has arrived.
 */
int cacheHasPendingArt(void);

// STUB (rebuild): drain/abort hooks for the fork's THREADED art cache. This rebuild runs the
// official SYNCHRONOUS cache -- nothing is ever pending or in-flight -- so "drained" is always
// exactly true. If a threaded cache ever returns (checklist item 45 territory), these must
// become the real implementations again.
static inline int cacheAbortMmceImageLoadsTimed(int waitTicks)
{
    (void)waitTicks;
    return 1;
}
static inline int cacheCancelPendingImageLoadsTimed(int waitTicks)
{
    (void)waitTicks;
    return 1;
}
static inline void cacheCancelPendingImageLoads(void)
{
}

#endif
