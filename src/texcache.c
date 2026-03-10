#include "include/opl.h"
#include "include/texcache.h"
#include "include/textures.h"
#include "include/ioman.h"
#include "include/gui.h"
#include "include/util.h"
#include "include/renderman.h"

typedef struct
{
    image_cache_t *cache;
    cache_entry_t *entry;
    item_list_t *list;
    // only for comparison if the deferred action is still valid
    int cacheUID;
    unsigned int generation;
    GSTEXTURE texture;
    char *value;
} load_image_request_t;

typedef struct cache_registry_entry
{
    image_cache_t *cache;
    struct cache_registry_entry *next;
} cache_registry_entry_t;

static cache_registry_entry_t *gCacheRegistry = NULL;
static image_cache_t *gPrimeCursorCache = NULL;
static unsigned int gCacheGeneration = 1;
static int gCacheInitialized = 0;

static void cacheClearItem(cache_entry_t *item, int freeTxt);

static int cacheIsRequestValid(const load_image_request_t *req)
{
    return req != NULL && req->entry != NULL && req->cache != NULL && req->list != NULL && req->cacheUID == req->entry->UID && req->generation == req->entry->generation;
}

static void cacheRegister(image_cache_t *cache)
{
    cache_registry_entry_t *entry = malloc(sizeof(*entry));

    if (entry == NULL)
        return;

    entry->cache = cache;
    entry->next = gCacheRegistry;
    gCacheRegistry = entry;

    if (gPrimeCursorCache == NULL)
        gPrimeCursorCache = cache;
}

static void cacheUnregister(image_cache_t *cache)
{
    cache_registry_entry_t *entry = gCacheRegistry;
    cache_registry_entry_t *previous = NULL;

    while (entry != NULL) {
        if (entry->cache == cache) {
            if (gPrimeCursorCache == cache)
                gPrimeCursorCache = NULL;

            if (previous != NULL)
                previous->next = entry->next;
            else
                gCacheRegistry = entry->next;

            free(entry);
            return;
        }

        previous = entry;
        entry = entry->next;
    }
}

static void cacheClearHiddenEntries(int freeQueuedRequests)
{
    cache_registry_entry_t *registry = gCacheRegistry;

    while (registry != NULL) {
        image_cache_t *cache = registry->cache;

        if (cache != NULL) {
            for (int i = 0; i < cache->count; i++) {
                cache_entry_t *entry = &cache->content[i];

                if ((entry->state == CACHE_ENTRY_STATE_EMPTY) || (entry->state == CACHE_ENTRY_STATE_DISPLAYABLE))
                    continue;

                if (freeQueuedRequests && entry->qr != NULL)
                    free(entry->qr);

                cacheClearItem(entry, 1);
            }
        }

        registry = registry->next;
    }
}

static void cachePromotePrimedEntries(void)
{
    cache_registry_entry_t *registry = gCacheRegistry;

    while (registry != NULL) {
        image_cache_t *cache = registry->cache;

        if (cache != NULL) {
            for (int i = 0; i < cache->count; i++) {
                cache_entry_t *entry = &cache->content[i];

                if (entry->state == CACHE_ENTRY_STATE_PRIMED && entry->primeFrame < guiFrameId)
                    entry->state = CACHE_ENTRY_STATE_DISPLAYABLE;
            }
        }

        registry = registry->next;
    }
}

static int cachePrimeCache(image_cache_t *cache)
{
    for (int i = 0; i < cache->count; i++) {
        cache_entry_t *entry = &cache->content[i];

        if (entry->state == CACHE_ENTRY_STATE_READY) {
            rmPrimeTexture(&entry->texture);
            entry->state = CACHE_ENTRY_STATE_PRIMED;
            entry->primeFrame = guiFrameId;
            gPrimeCursorCache = cache;
            return 1;
        }
    }

    return 0;
}

// Io handled action...
static void cacheLoadImage(void *data)
{
    load_image_request_t *req = data;

    // Safeguards...
    if (!cacheIsRequestValid(req)) {
        if (req)
            free(req);
        return;
    }

    // seems okay. we can proceed
    memset(&req->texture, 0, sizeof(req->texture));

    if (req->list->itemGetImage(req->list, req->cache->prefix, req->cache->isPrefixRelative, req->value, req->cache->suffix, &req->texture, GS_PSM_CT24) < 0) {
        if (cacheIsRequestValid(req)) {
            req->entry->lastUsed = 0;
            req->entry->state = CACHE_ENTRY_STATE_FAILED;
            req->entry->primeFrame = -1;
            req->entry->qr = NULL;
        } else {
            texFree(&req->texture);
        }

        free(req);
        return;
    }

    if (!cacheIsRequestValid(req)) {
        texFree(&req->texture);
        free(req);
        return;
    }

    req->entry->texture = req->texture;
    req->entry->lastUsed = guiFrameId;
    req->entry->state = CACHE_ENTRY_STATE_READY;
    req->entry->primeFrame = -1;
    req->entry->qr = NULL;

    free(req);
}

void cacheInit()
{
    gCacheInitialized = 1;
    ioRegisterHandler(IO_CACHE_LOAD_ART, &cacheLoadImage);
}

void cacheEnd()
{
    gCacheInitialized = 0;
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
    item->primeFrame = -1;
    item->generation = 0;
    item->state = CACHE_ENTRY_STATE_EMPTY;
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

    cacheRegister(cache);

    return cache;
}

void cacheDestroyCache(image_cache_t *cache)
{
    cacheUnregister(cache);

    int i;
    for (i = 0; i < cache->count; ++i) {
        cacheClearItem(&cache->content[i], 1);
    }

    free(cache->prefix);
    free(cache->suffix);
    free(cache->content);
    free(cache);
}

void cacheCancelPendingImageLoads(void)
{
    if (!gCacheInitialized)
        return;

    gPrimeCursorCache = NULL;
    ioRemoveRequests(IO_CACHE_LOAD_ART);
    cacheClearHiddenEntries(1);
}

void cacheAdvanceGeneration(void)
{
    if (!gCacheInitialized)
        return;

    gCacheGeneration++;
    if (gCacheGeneration == 0)
        gCacheGeneration = 1;

    gPrimeCursorCache = NULL;
    cacheClearHiddenEntries(0);
}

void cachePrimeReadyTexture(void)
{
    if (!gCacheInitialized)
        return;

    cachePromotePrimedEntries();

    if (gCacheRegistry == NULL)
        return;

    cache_registry_entry_t *cursor = gCacheRegistry;
    while (cursor != NULL && cursor->cache != gPrimeCursorCache)
        cursor = cursor->next;

    cache_registry_entry_t *entry = (cursor != NULL && cursor->next != NULL) ? cursor->next : gCacheRegistry;
    cache_registry_entry_t *start = entry;

    do {
        if (entry != NULL && cachePrimeCache(entry->cache))
            return;

        entry = (entry != NULL && entry->next != NULL) ? entry->next : gCacheRegistry;
    } while (entry != NULL && entry != start);
}

GSTEXTURE *cacheGetTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value)
{
    if (*cacheId == -2) {
        return NULL;
    } else if (*cacheId != -1) {
        cache_entry_t *entry = &cache->content[*cacheId];
        if (entry->UID == *UID) {
            if ((entry->state == CACHE_ENTRY_STATE_QUEUED) || (entry->state == CACHE_ENTRY_STATE_READY) || (entry->state == CACHE_ENTRY_STATE_PRIMED))
                return NULL;
            else if (entry->state == CACHE_ENTRY_STATE_FAILED) {
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
        if (req == NULL)
            return NULL;

        memset(req, 0, sizeof(load_image_request_t));
        req->cache = cache;
        req->entry = oldestEntry;
        req->list = list;
        req->value = (char *)req + sizeof(load_image_request_t);
        strcpy(req->value, value);
        req->cacheUID = cache->nextUID;
        req->generation = gCacheGeneration;

        cacheClearItem(oldestEntry, 1);
        oldestEntry->qr = req;
        oldestEntry->UID = cache->nextUID;
        oldestEntry->generation = gCacheGeneration;
        oldestEntry->state = CACHE_ENTRY_STATE_QUEUED;

        *UID = cache->nextUID++;

        if (ioPutRequest(IO_CACHE_LOAD_ART, req) != IO_OK) {
            // Queue is full or IO is blocked. Drop request and free the cache slot.
            cacheClearItem(oldestEntry, 0);
            free(req);
            *cacheId = -1;
        }
    }

    return NULL;
}
