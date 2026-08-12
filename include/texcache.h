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

    // The value this slot was loaded FOR (a game's startup id, or a VCD's filename). The cache used
    // to be addressable only by (slot index, UID), which are handed out per request -- so a row that
    // lost its ids could never find art it already had, even sitting decoded in the very next slot.
    // Every list rebuild resets those ids, which is why a rebuild threw the whole viewport away.
    char key[64];
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

// STUB (rebuild): the fork's THREADED art cache had its own worker to drain, which this rebuild does
// not have. These two keep returning "drained" because there is no such thread here.
//
// ⚠ Their old comment claimed "nothing is ever pending or in-flight" and that was WRONG -- it is
// contradicted three lines up by cacheHasPendingArt(). Art here is not synchronous: cacheLoadImage is
// an IOMAN HANDLER and every cover is a queued request on the shared IO worker. Only the fork's
// dedicated art THREAD is missing. That stale sentence is why the pending queue went unaccounted for
// at teardown; do not restore it.
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

// STUB, and it MUST STAY ONE. themes.c calls this on the first theme load (curT == NULL), which is
// every boot -- at that point the cache is not even initialised and nothing can be queued, so the
// fork's "drain now" semantics are a genuine no-op here.
//
// ⚠ rebuild-142 made this real AND sticky for the teardown case and killed cover art on every boot,
// because that themes.c caller was never looked at. The teardown need is a DIFFERENT operation with
// a different lifetime -- it is cacheShutdownArtLoads() below. Do not merge the two again.
static inline void cacheCancelPendingImageLoads(void)
{
}

// Stop servicing queued cover art for the rest of the process. TEARDOWN ONLY.
//
// Not an optimisation -- a launch-latency fix. ioBlockOpsTimed waits for the WHOLE ioman queue to
// drain, and isIOBlocked only stops NEW requests being accepted; the worker keeps servicing
// everything already queued. So a launch pressed while covers are in flight pays for every one of
// them to be read off the game device before the IOP reset -- for a menu guiEnd() is about to
// destroy. On a slow transport (MX4SIO's SD over SIO2) that IS the wait.
//
// One-way on purpose, and that is exactly why it needs its own name: the ONLY callers are deinit()
// and deinitEx(), both of which hand off to another ELF and never return to a live menu. Anything
// that DOES return to a live menu must not call this.
void cacheShutdownArtLoads(void);

#endif
