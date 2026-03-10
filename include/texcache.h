#ifndef __TEX_CACHE_H
#define __TEX_CACHE_H

#include "include/iosupport.h"

typedef enum {
    CACHE_ENTRY_STATE_EMPTY = 0,
    CACHE_ENTRY_STATE_QUEUED,
    CACHE_ENTRY_STATE_READY,
    CACHE_ENTRY_STATE_PRIMED,
    CACHE_ENTRY_STATE_DISPLAYABLE,
    CACHE_ENTRY_STATE_FAILED,
} cache_entry_state_t;

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
    int primeFrame;
    unsigned int generation;
    unsigned char state;
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

/** Cancels any queued art loads that have not started yet.
 */
void cacheCancelPendingImageLoads(void);

/** Advances the active art generation and invalidates stale hidden art without flushing loaded textures.
 */
void cacheAdvanceGeneration(void);

/** Promotes at most one ready texture into VRAM per frame and exposes previously primed textures on the following frame.
 */
void cachePrimeReadyTexture(void);

GSTEXTURE *cacheGetTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value);

#endif
