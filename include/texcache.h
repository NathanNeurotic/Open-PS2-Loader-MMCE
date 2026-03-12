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
    int destroying;

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

/** Cancels any queued art loads that have not started yet.
 */
void cacheCancelPendingImageLoads(void);

/** Invalidates queued art loads without blocking on the IO worker.
 */
void cacheAdvanceGeneration(void);

/** Advances the failure-retry generation without canceling queued art loads.
 */
void cacheBumpGeneration(void);

/** Uploads at most one ready texture to VRAM for use on a later frame.
 */
void cachePrimeReadyTexture(void);

/** Returns nonzero while art decode or prime work is still pending.
 */
int cacheHasPendingArt(void);

/** Returns nonzero while any interactive art request is queued or actively decoding.
 */
int cacheHasPendingInteractiveArt(void);

GSTEXTURE *cacheGetTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value);
GSTEXTURE *cachePrefetchTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value);

#endif
