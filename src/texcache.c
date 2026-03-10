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
    int cacheUID;
    char *value;
} load_image_request_t;

typedef struct cache_registry_entry
{
    image_cache_t *cache;
    struct cache_registry_entry *next;
} cache_registry_entry_t;

static cache_registry_entry_t *gCacheRegistry = NULL;

static void cacheClearItem(cache_entry_t *item, int freeTxt);

static void cacheRegister(image_cache_t *cache)
{
    cache_registry_entry_t *entry = malloc(sizeof(*entry));

    if (entry == NULL)
        return;

    entry->cache = cache;
    entry->next = gCacheRegistry;
    gCacheRegistry = entry;
}

static void cacheUnregister(image_cache_t *cache)
{
    cache_registry_entry_t *entry = gCacheRegistry;
    cache_registry_entry_t *previous = NULL;

    while (entry != NULL) {
        if (entry->cache == cache) {
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

static void cacheClearQueuedRequests(void)
{
    cache_registry_entry_t *registry = gCacheRegistry;

    while (registry != NULL) {
        image_cache_t *cache = registry->cache;

        if (cache != NULL) {
            for (int i = 0; i < cache->count; i++) {
                cache_entry_t *entry = &cache->content[i];

                if (entry->qr != NULL) {
                    free(entry->qr);
                    cacheClearItem(entry, 1);
                }
            }
        }

        registry = registry->next;
    }
}

static void cacheReleaseRequest(load_image_request_t *req, int markFailed)
{
    if (req == NULL)
        return;

    if (req->entry != NULL && req->entry->qr == req) {
        req->entry->qr = NULL;

        if (markFailed)
            req->entry->lastUsed = 0;
    }

    free(req);
}

static void cacheLoadImage(void *data)
{
    load_image_request_t *req = data;

    if (req == NULL || req->entry == NULL || req->cache == NULL) {
        cacheReleaseRequest(req, 0);
        return;
    }

    item_list_t *handler = req->list;
    if (handler == NULL) {
        cacheReleaseRequest(req, 1);
        return;
    }

    if (req->cacheUID != req->entry->UID) {
        cacheReleaseRequest(req, 0);
        return;
    }

    GSTEXTURE *texture = &req->entry->texture;
    texFree(texture);

    if (handler->itemGetImage(handler, req->cache->prefix, req->cache->isPrefixRelative, req->value, req->cache->suffix, texture, GS_PSM_CT24) < 0)
        req->entry->lastUsed = 0;
    else
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
    item->texture.ClutStorageMode = GS_CLUT_STORAGE_CSM1;
    item->qr = NULL;
    item->lastUsed = -1;
    item->UID = 0;
}

image_cache_t *cacheInitCache(int userId, const char *prefix, int isPrefixRelative, const char *suffix, int count)
{
    image_cache_t *cache = malloc(sizeof(image_cache_t));

    if (cache == NULL)
        return NULL;

    memset(cache, 0, sizeof(image_cache_t));
    cache->userId = userId;
    cache->count = count;

    if (prefix != NULL) {
        int length = strlen(prefix) + 1;
        cache->prefix = malloc(length * sizeof(char));
        if (cache->prefix == NULL) {
            free(cache);
            return NULL;
        }
        memcpy(cache->prefix, prefix, length);
    }

    cache->isPrefixRelative = isPrefixRelative;

    int length = strlen(suffix) + 1;
    cache->suffix = malloc(length * sizeof(char));
    if (cache->suffix == NULL) {
        free(cache->prefix);
        free(cache);
        return NULL;
    }
    memcpy(cache->suffix, suffix, length);

    cache->nextUID = 1;
    cache->content = malloc(count * sizeof(cache_entry_t));
    if (cache->content == NULL) {
        free(cache->prefix);
        free(cache->suffix);
        free(cache);
        return NULL;
    }

    for (int i = 0; i < count; ++i)
        cacheClearItem(&cache->content[i], 0);

    cacheRegister(cache);

    return cache;
}

void cacheDestroyCache(image_cache_t *cache)
{
    if (cache == NULL)
        return;

    cacheUnregister(cache);

    for (int i = 0; i < cache->count; ++i) {
        if (cache->content[i].qr != NULL) {
            free(cache->content[i].qr);
            cache->content[i].qr = NULL;
        }

        cacheClearItem(&cache->content[i], 1);
    }

    free(cache->prefix);
    free(cache->suffix);
    free(cache->content);
    free(cache);
}

void cacheCancelPendingImageLoads(void)
{
    ioRemoveRequests(IO_CACHE_LOAD_ART);
    cacheClearQueuedRequests();
}

void cacheAdvanceGeneration(void)
{
}

void cachePrimeReadyTexture(void)
{
}

GSTEXTURE *cacheGetTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value)
{
    if (*cacheId == -2) {
        return NULL;
    } else if (*cacheId != -1) {
        cache_entry_t *entry = &cache->content[*cacheId];
        if (entry->UID == *UID) {
            if (entry->qr != NULL)
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

    if (guiInactiveFrames < list->delay)
        return NULL;

    cache_entry_t *currEntry, *oldestEntry = NULL;
    int rtime = guiFrameId;

    for (int i = 0; i < cache->count; i++) {
        currEntry = &cache->content[i];
        if (currEntry->qr == NULL && currEntry->lastUsed < rtime) {
            oldestEntry = currEntry;
            rtime = currEntry->lastUsed;
            *cacheId = i;
        }
    }

    if (oldestEntry != NULL) {
        load_image_request_t *req = malloc(sizeof(load_image_request_t) + strlen(value) + 1);

        if (req == NULL)
            return NULL;

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

        if (ioPutRequest(IO_CACHE_LOAD_ART, req) != IO_OK) {
            cacheClearItem(oldestEntry, 0);
            free(req);
            *cacheId = -1;
        }
    }

    return NULL;
}
