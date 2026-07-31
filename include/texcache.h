#ifndef __TEX_CACHE_H
#define __TEX_CACHE_H

#include "include/iosupport.h"

/// A single cache entry...
typedef struct cache_entry
{
    GSTEXTURE texture;

    // NULL unless a queued/loading request owns this entry.
    void *qr;

    // Cache entry state and last-use frame are managed by texcache.c.
    int state;
    int lastUsed;
    int UID;

    // ERR_BAD_FILE is memoized until a settings/theme apply. Other failures retry
    // automatically after retryFrame so a transient bus error cannot hide art forever.
    unsigned char failAbsent;
    unsigned int retryFrame;

    // Heap-owned art identity. Every cache hit validates this in addition to
    // (cacheId, UID), preventing a stale item mapping from showing a neighbour's art.
    char *value;
} cache_entry_t;

/// One texture cache instance
typedef struct image_cache
{
    /// User-specified ID, retained for the theme caller.
    int userId;

    /// count of entries (copy of the requested cache size upon cache initialization)
    int count;

    /// directory prefix for this cache (if any)
    char *prefix;
    int isPrefixRelative;
    char *suffix;

    int nextUID;
    int activeRequests;
    int destroying;
    struct image_cache *registryNext;

    /// the cache entries itself
    cache_entry_t *content;
} image_cache_t;

/** Initializes the cache subsystem.
 */
void cacheInit(void);

/** Stops the cache worker after cancelling pending work. */
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

/** Cancels queued MMCE-backed art and waits up to timeoutTicks for active MMCE art to drain.
 */
int cacheAbortMmceImageLoadsTimed(int timeoutTicks);

/** Drop genuine-absence memos after a deliberate settings/theme apply. */
void cacheInvalidateFailMemo(void);

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

/** Refreshes the navigation snapshot and wakes queued art once foreground IO allows it. */
void cachePumpPendingArt(void);

GSTEXTURE *cacheGetTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value);

#endif
