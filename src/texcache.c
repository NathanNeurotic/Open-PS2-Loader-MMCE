#include "include/opl.h"
#include "include/texcache.h"
#include "include/textures.h"
#include "include/gui.h"
#include "include/util.h"
#include "include/renderman.h"

typedef struct load_image_request
{
    struct load_image_request *next;
    image_cache_t *cache;
    cache_entry_t *entry;
    item_list_t *list;
    int cacheUID;
    GSTEXTURE texture;
    char *value;
} load_image_request_t;

typedef struct cache_registry_entry
{
    image_cache_t *cache;
    struct cache_registry_entry *next;
} cache_registry_entry_t;

enum {
    CACHE_ENTRY_FREE = 0,
    CACHE_ENTRY_QUEUED,
    CACHE_ENTRY_LOADING,
    CACHE_ENTRY_READY,
    CACHE_ENTRY_PRIMED,
    CACHE_ENTRY_DISPLAYABLE,
    CACHE_ENTRY_FAILED
};

extern void *_gp;

#define CACHE_THREAD_STACK_SIZE (96 * 1024)

static u8 gArtThreadStack[CACHE_THREAD_STACK_SIZE] ALIGNED(16);
static ee_thread_t gArtThread;
static s32 gArtThreadId = -1;
static s32 gArtSemaId = -1;
static ee_sema_t gArtSema;

static int gArtTerminate = 0;
static int gArtRunning = 0;
static int gArtQueuedCount = 0;
static int gArtActiveCount = 0;

static load_image_request_t *gArtReqList = NULL;
static load_image_request_t *gArtReqEnd = NULL;
static cache_registry_entry_t *gCacheRegistry = NULL;

static void cacheClearItem(cache_entry_t *item, int freeTxt);
static void cacheResetTextureState(GSTEXTURE *texture);

static void cacheLock(void)
{
    if (gArtSemaId >= 0)
        WaitSema(gArtSemaId);
}

static void cacheUnlock(void)
{
    if (gArtSemaId >= 0)
        SignalSema(gArtSemaId);
}

static void cacheRegister(image_cache_t *cache)
{
    cache_registry_entry_t *entry = malloc(sizeof(*entry));

    if (entry == NULL)
        return;

    entry->cache = cache;

    cacheLock();
    entry->next = gCacheRegistry;
    gCacheRegistry = entry;
    cacheUnlock();
}

static void cacheUnregister(image_cache_t *cache)
{
    cache_registry_entry_t *entry;
    cache_registry_entry_t *previous = NULL;

    cacheLock();

    entry = gCacheRegistry;
    while (entry != NULL) {
        if (entry->cache == cache) {
            if (previous != NULL)
                previous->next = entry->next;
            else
                gCacheRegistry = entry->next;

            free(entry);
            break;
        }

        previous = entry;
        entry = entry->next;
    }

    cacheUnlock();
}

static void cacheResetTextureState(GSTEXTURE *texture)
{
    memset(texture, 0, sizeof(*texture));
    texture->ClutStorageMode = GS_CLUT_STORAGE_CSM1;
}

static void cacheReleaseRequestLocked(load_image_request_t *req)
{
    if (req == NULL)
        return;

    texFree(&req->texture);
    cacheResetTextureState(&req->texture);

    if (req->cache != NULL && req->cache->activeRequests > 0)
        req->cache->activeRequests--;

    free(req);
}

static void cacheEnqueueRequestLocked(load_image_request_t *req)
{
    req->next = NULL;

    if (gArtReqEnd != NULL)
        gArtReqEnd->next = req;
    else
        gArtReqList = req;

    gArtReqEnd = req;
    gArtQueuedCount++;
}

static int cacheRemoveQueuedRequestLocked(load_image_request_t *target)
{
    load_image_request_t *req = gArtReqList;
    load_image_request_t *previous = NULL;

    while (req != NULL) {
        if (req == target) {
            if (previous != NULL)
                previous->next = req->next;
            else
                gArtReqList = req->next;

            if (gArtReqEnd == req)
                gArtReqEnd = previous;

            req->next = NULL;
            if (gArtQueuedCount > 0)
                gArtQueuedCount--;

            return 1;
        }

        previous = req;
        req = req->next;
    }

    return 0;
}

static void cacheInvalidateEntryLocked(cache_entry_t *entry, int freeTxt, int preserveLoaded)
{
    load_image_request_t *req = entry->qr;

    switch (entry->state) {
        case CACHE_ENTRY_QUEUED:
            entry->qr = NULL;
            if (req != NULL && cacheRemoveQueuedRequestLocked(req))
                cacheReleaseRequestLocked(req);
            cacheClearItem(entry, freeTxt);
            break;
        case CACHE_ENTRY_LOADING:
            entry->qr = NULL;
            cacheClearItem(entry, freeTxt);
            break;
        case CACHE_ENTRY_READY:
        case CACHE_ENTRY_PRIMED:
            if (preserveLoaded) {
                entry->qr = NULL;
                entry->state = CACHE_ENTRY_DISPLAYABLE;
                entry->primeFrame = -1;
            } else
                cacheClearItem(entry, freeTxt);
            break;
        default:
            break;
    }
}

static void cacheInvalidatePendingRequestsLocked(int preserveLoaded)
{
    cache_registry_entry_t *registry = gCacheRegistry;

    while (registry != NULL) {
        image_cache_t *cache = registry->cache;

        if (cache != NULL && !cache->destroying) {
            for (int i = 0; i < cache->count; i++)
                cacheInvalidateEntryLocked(&cache->content[i], 1, preserveLoaded);
        }

        registry = registry->next;
    }
}

static int cacheHasReadyEntriesLocked(void)
{
    cache_registry_entry_t *registry = gCacheRegistry;

    while (registry != NULL) {
        image_cache_t *cache = registry->cache;

        if (cache != NULL && !cache->destroying) {
            for (int i = 0; i < cache->count; i++) {
                if (cache->content[i].state == CACHE_ENTRY_READY || cache->content[i].state == CACHE_ENTRY_PRIMED)
                    return 1;
            }
        }

        registry = registry->next;
    }

    return 0;
}

static void cacheWaitForAllRequests(void)
{
    if (!gArtRunning)
        return;

    while (1) {
        int pending;

        cacheLock();
        pending = (gArtQueuedCount > 0) || (gArtActiveCount > 0);
        cacheUnlock();

        if (!pending)
            break;

        delay(1);
    }
}

static void cacheWaitForCacheRequests(image_cache_t *cache)
{
    while (1) {
        int pending;

        cacheLock();
        pending = cache->activeRequests;
        cacheUnlock();

        if (!pending)
            break;

        delay(1);
    }
}

static load_image_request_t *cacheDequeueRequest(void)
{
    load_image_request_t *req = NULL;

    cacheLock();

    if (gArtReqList != NULL) {
        req = gArtReqList;
        gArtReqList = req->next;
        req->next = NULL;

        if (gArtReqEnd == req)
            gArtReqEnd = NULL;

        if (gArtQueuedCount > 0)
            gArtQueuedCount--;
        gArtActiveCount++;

        if (req->entry != NULL && req->entry->qr == req && req->entry->state == CACHE_ENTRY_QUEUED)
            req->entry->state = CACHE_ENTRY_LOADING;
    }

    cacheUnlock();

    return req;
}

static void cacheCompleteRequest(load_image_request_t *req, int result)
{
    cacheLock();

    if (req->entry != NULL && req->entry->qr == req && req->entry->UID == req->cacheUID && req->cache != NULL && !req->cache->destroying) {
        req->entry->qr = NULL;
        req->entry->primeFrame = -1;

        if (result < 0 || req->texture.Mem == NULL) {
            req->entry->lastUsed = 0;
            req->entry->state = CACHE_ENTRY_FAILED;
        } else {
            req->entry->texture = req->texture;
            cacheResetTextureState(&req->texture);
            req->entry->lastUsed = guiFrameId;
            req->entry->state = CACHE_ENTRY_READY;
        }
    }

    if (gArtActiveCount > 0)
        gArtActiveCount--;

    cacheReleaseRequestLocked(req);
    cacheUnlock();
}

static void cacheLoadImage(load_image_request_t *req)
{
    int result = -1;

    if (req == NULL || req->cache == NULL || req->list == NULL || req->entry == NULL) {
        cacheLock();
        if (gArtActiveCount > 0)
            gArtActiveCount--;
        cacheReleaseRequestLocked(req);
        cacheUnlock();
        return;
    }

    result = req->list->itemGetImage(req->list, req->cache->prefix, req->cache->isPrefixRelative, req->value, req->cache->suffix, &req->texture, GS_PSM_CT24);
    cacheCompleteRequest(req, result);
}

static void cacheWorkerThread(void *arg)
{
    (void)arg;

    while (!gArtTerminate) {
        load_image_request_t *req;

        SleepThread();

        if (gArtTerminate)
            break;

        while (!gArtTerminate && (req = cacheDequeueRequest()) != NULL)
            cacheLoadImage(req);
    }

    cacheLock();
    gArtRunning = 0;
    cacheUnlock();

    ExitDeleteThread();
}

void cacheInit()
{
    if (gArtRunning)
        return;

    gArtTerminate = 0;
    gArtQueuedCount = 0;
    gArtActiveCount = 0;
    gArtReqList = NULL;
    gArtReqEnd = NULL;

    gArtSema.init_count = 1;
    gArtSema.max_count = 1;
    gArtSema.option = 0;

    gArtSemaId = CreateSema(&gArtSema);
    if (gArtSemaId < 0)
        return;

    gArtThread.attr = 0;
    gArtThread.stack_size = CACHE_THREAD_STACK_SIZE;
    gArtThread.gp_reg = &_gp;
    gArtThread.func = &cacheWorkerThread;
    gArtThread.stack = gArtThreadStack;
    gArtThread.initial_priority = 32;

    gArtThreadId = CreateThread(&gArtThread);
    if (gArtThreadId < 0) {
        DeleteSema(gArtSemaId);
        gArtSemaId = -1;
        return;
    }

    gArtRunning = 1;
    StartThread(gArtThreadId, NULL);
}

void cacheEnd()
{
    if (!gArtRunning)
        return;

    cacheCancelPendingImageLoads();

    gArtTerminate = 1;
    WakeupThread(gArtThreadId);

    while (gArtRunning)
        delay(1);

    if (gArtSemaId >= 0) {
        DeleteSema(gArtSemaId);
        gArtSemaId = -1;
    }

    gArtThreadId = -1;
}

static void cacheClearItem(cache_entry_t *item, int freeTxt)
{
    if (freeTxt && item->texture.Mem) {
        rmUnloadTexture(&item->texture);
        texFree(&item->texture);
    }

    memset(item, 0, sizeof(cache_entry_t));
    cacheResetTextureState(&item->texture);
    item->qr = NULL;
    item->state = CACHE_ENTRY_FREE;
    item->primeFrame = -1;
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

    {
        int length = strlen(suffix) + 1;
        cache->suffix = malloc(length * sizeof(char));
        if (cache->suffix == NULL) {
            free(cache->prefix);
            free(cache);
            return NULL;
        }
        memcpy(cache->suffix, suffix, length);
    }

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

    cacheLock();
    cache->destroying = 1;

    for (int i = 0; i < cache->count; ++i) {
        cache_entry_t *entry = &cache->content[i];

        cacheInvalidateEntryLocked(entry, 1, 0);
        if (entry->state == CACHE_ENTRY_DISPLAYABLE || entry->state == CACHE_ENTRY_FAILED)
            cacheClearItem(entry, 1);
    }

    cacheUnlock();

    cacheWaitForCacheRequests(cache);
    cacheUnregister(cache);

    free(cache->prefix);
    free(cache->suffix);
    free(cache->content);
    free(cache);
}

void cacheCancelPendingImageLoads(void)
{
    cacheLock();
    cacheInvalidatePendingRequestsLocked(0);
    cacheUnlock();

    cacheWaitForAllRequests();
}

void cacheAdvanceGeneration(void)
{
    cacheLock();
    cacheInvalidatePendingRequestsLocked(1);
    cacheUnlock();
}

void cachePrimeReadyTexture(void)
{
    GSTEXTURE *texture = NULL;
    cache_registry_entry_t *registry;

    if (guiInactiveFrames <= 0)
        return;

    cacheLock();

    registry = gCacheRegistry;
    while (registry != NULL && texture == NULL) {
        image_cache_t *cache = registry->cache;

        if (cache != NULL && !cache->destroying) {
            for (int i = 0; i < cache->count; i++) {
                cache_entry_t *entry = &cache->content[i];

                if (entry->state == CACHE_ENTRY_READY && entry->texture.Mem != NULL) {
                    entry->state = CACHE_ENTRY_PRIMED;
                    entry->primeFrame = guiFrameId;
                    texture = &entry->texture;
                    break;
                }
            }
        }

        registry = registry->next;
    }

    cacheUnlock();

    if (texture != NULL)
        rmPrimeTexture(texture);
}

int cacheHasPendingArt(void)
{
    int pending;

    cacheLock();
    pending = (gArtQueuedCount > 0) || (gArtActiveCount > 0) || cacheHasReadyEntriesLocked();
    cacheUnlock();

    return pending;
}

GSTEXTURE *cacheGetTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value)
{
    cache_entry_t *entry;
    cache_entry_t *oldestEntry = NULL;
    load_image_request_t *req;
    GSTEXTURE *result = NULL;
    int oldestEntryId = -1;
    int rtime = guiFrameId;

    if (*cacheId == -2 || cache == NULL || cache->destroying)
        return NULL;

    cacheLock();

    if (*cacheId != -1) {
        entry = &cache->content[*cacheId];
        if (entry->UID == *UID) {
            switch (entry->state) {
                case CACHE_ENTRY_QUEUED:
                case CACHE_ENTRY_LOADING:
                case CACHE_ENTRY_READY:
                    cacheUnlock();
                    return NULL;
                case CACHE_ENTRY_PRIMED:
                    if (entry->primeFrame == guiFrameId) {
                        cacheUnlock();
                        return NULL;
                    }
                    entry->state = CACHE_ENTRY_DISPLAYABLE;
                    entry->lastUsed = guiFrameId;
                    result = &entry->texture;
                    cacheUnlock();
                    return result;
                case CACHE_ENTRY_DISPLAYABLE:
                    entry->lastUsed = guiFrameId;
                    result = &entry->texture;
                    cacheUnlock();
                    return result;
                case CACHE_ENTRY_FAILED:
                    *cacheId = -2;
                    cacheUnlock();
                    return NULL;
                default:
                    *cacheId = -1;
                    break;
            }
        } else {
            *cacheId = -1;
        }
    }

    if (guiInactiveFrames <= 0) {
        cacheUnlock();
        return NULL;
    }

    for (int i = 0; i < cache->count; i++) {
        entry = &cache->content[i];
        if ((entry->state == CACHE_ENTRY_FREE || entry->state == CACHE_ENTRY_DISPLAYABLE || entry->state == CACHE_ENTRY_FAILED) && entry->lastUsed < rtime) {
            oldestEntry = entry;
            oldestEntryId = i;
            rtime = entry->lastUsed;
        }
    }

    if (oldestEntry == NULL || !gArtRunning) {
        cacheUnlock();
        return NULL;
    }

    req = malloc(sizeof(load_image_request_t) + strlen(value) + 1);
    if (req == NULL) {
        cacheUnlock();
        return NULL;
    }

    memset(req, 0, sizeof(load_image_request_t));
    cacheResetTextureState(&req->texture);

    req->cache = cache;
    req->entry = oldestEntry;
    req->list = list;
    req->value = (char *)req + sizeof(load_image_request_t);
    strcpy(req->value, value);
    req->cacheUID = cache->nextUID;

    cacheClearItem(oldestEntry, 1);
    oldestEntry->qr = req;
    oldestEntry->state = CACHE_ENTRY_QUEUED;
    oldestEntry->UID = cache->nextUID;

    *cacheId = oldestEntryId;
    *UID = cache->nextUID++;

    cache->activeRequests++;
    cacheEnqueueRequestLocked(req);
    cacheUnlock();

    WakeupThread(gArtThreadId);

    return NULL;
}
