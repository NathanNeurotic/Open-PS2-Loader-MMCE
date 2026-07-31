#ifndef __TEX_CACHE_H
#define __TEX_CACHE_H

#include "include/iosupport.h"

/// A single cache entry...
typedef struct
{
    GSTEXTURE texture;

    // NULL not queued, otherwise queue request record
    void *qr;

    // Cache entry state managed by texcache.c.
    int state;

    // Frame on which the texture was primed into VRAM.
    int primeFrame;

    // frame counter the icon was used the last time - oldest get rewritten first in case new icon is requested and cache is full. negative numbers mean
    // slot is free and can be used right now
    int lastUsed;

    int UID;

    // Set when this entry's load FAILED with ERR_BAD_FILE -- a GENUINE ABSENCE (open() found no file),
    // as opposed to a transient read error on a contended bus (ERR_FILE_IO). cacheGetTextureInternal
    // memoizes a genuine absence on gCacheFailEpoch (which survives the per-screen-switch generation
    // bump, so the info page stops re-running the same failing opens every visit -- #154/#120) and a
    // transient on gCacheGeneration (re-probed next generation, in case the bus recovered). 0 on a
    // fresh/cleared entry (memset) -- treated as transient, the conservative default.
    unsigned char failAbsent;

    // Identity of the art this entry holds (heap copy of the request value, e.g. the game's
    // startup). The instant-return path validates it against the caller's value so a poisoned or
    // stale per-item (cacheId, UID) pair self-heals in one frame instead of permanently rendering
    // another title's art (the CoverFlow wrong-neighbour-cover bug). Owned by the entry: freed in
    // cacheClearItem / cacheDestroyCache.
    char *value;
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
    int activeRequests;
    int queuedPrefetchRequests;
    int allowPrime;
    int destroying;

    /// the cache entries itself
    cache_entry_t *content;
} image_cache_t;

/** Initializes the cache subsystem.
 */
void cacheInit();

/** Terminates the cache. Does nothing currently. Users of this code have to destroy caches via cacheDestroyCache
 */
void cacheEnd(int forceStop);

/** Initializes a single cache
 */
image_cache_t *cacheInitCache(int userId, const char *prefix, int isPrefixRelative, const char *suffix, int count);

/** Destroys a given cache (unallocates all memory stored there, disconnects the pixmaps from the usage points).
 */
void cacheDestroyCache(image_cache_t *cache);

/** Cancels any queued art loads that have not started yet.
 */
void cacheCancelPendingImageLoads(void);

/** Cancels queued art loads and waits up to timeoutTicks for active loads to drain.
 */
int cacheCancelPendingImageLoadsTimed(int timeoutTicks);

/** Cancels queued MMCE-backed art (both queues) and waits up to timeoutTicks for active MMCE art to drain.
 */
int cacheAbortMmceImageLoadsTimed(int timeoutTicks);

/** Advances the failure-retry generation and demotes loaded art to displayable. Queued and
 *  in-flight loads are LEFT to finish and land in cache (wOPL persist-and-load); teardown paths
 *  must use the cacheCancelPendingImageLoads / cacheAbortMmceImageLoadsTimed family instead.
 *  Never blocks on the IO worker.
 */
void cacheAdvanceGeneration(void);
// Drop the genuine-absence art memos (see texcache.c). Call only on a deliberate settings/theme apply.
void cacheInvalidateFailMemo(void);

/** Invalidates stale interactive art loads while keeping queued prefetch work.
 */
void cacheAdvanceGenerationPreservePrefetch(void);

/** Uploads at most one ready texture to VRAM for use on a later frame.
 */
void cachePrimeReadyTexture(void);

/** Returns nonzero while art IO or decode work is still in flight.
 */
int cacheHasPendingArt(void);

/** Lower the calling thread's priority below the art worker (so it can be
 *  scheduled to release shared IOP/fileXio resources) and restore it afterwards.
 *  cacheLowerCallerPriority() returns the prior priority (or -1 if unchanged) to
 *  pass back to cacheRestoreCallerPriority(). Used around busy-waits such as
 *  guiHandleDeferedIO() to avoid starving the worker (issue #45).
 */
int cacheLowerCallerPriority(void);
void cacheRestoreCallerPriority(int savedPriority);

/** Returns nonzero while any interactive art request is queued or actively decoding.
 */
int cacheHasPendingInteractiveArt(void);

/** Wakes queued interactive art once navigation input becomes idle.
 */
void cacheWakeInteractiveArtOnInputIdle(void);

/** Returns a texture only if the current cache slot is already ready; does not queue new IO.
 */
GSTEXTURE *cacheGetTextureIfReady(image_cache_t *cache, int *cacheId, int *UID);

GSTEXTURE *cacheGetTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value);
GSTEXTURE *cachePrefetchTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value);

#endif
