#include "include/opl.h"
#include "include/appsupport.h"
#include "include/favsupport.h"
#include "include/pad.h"
#include "include/texcache.h"
#include "include/textures.h"
#include "include/gui.h"
#include "include/ioman.h"
#include "include/renderman.h"
#include "include/vcdsupport.h"

#include "include/tar.h"

#include <delaythread.h>
#include <kernel.h>
#include <malloc.h>
#include <ps2sdkapi.h>

/*
 * Art cache design
 * ----------------
 * Rendering submits only textures it actually needs. One low-priority worker
 * drains one bounded FIFO, and foreground IO always gets first use of the bus.
 * There is deliberately no speculative prefetch queue, generation invalidation,
 * or worker-side VRAM upload.
 */

static int artTarLoadImage(const char *value, const char *suffix, GSTEXTURE *texture)
{
    char name[64];
    TarEntryBase *entry;
    void *buffer;
    int result;

    if (snprintf(name, sizeof(name), "%s_%s.png", value, suffix) >= (int)sizeof(name))
        return -1;

    entry = tarFind(TAR_KIND_ART, name);
    if (entry == NULL)
        return -1;

    buffer = malloc(entry->rawSize);
    if (buffer == NULL)
        return -1;

    if (tarRead(TAR_KIND_ART, entry, buffer, entry->rawSize) != entry->rawSize) {
        free(buffer);
        return -1;
    }

    result = texLoadFromMemory(texture, buffer, entry->rawSize);
    free(buffer);
    return result;
}

typedef struct load_image_request
{
    struct load_image_request *next;
    image_cache_t *cache;
    cache_entry_t *entry;
    item_list_t *list;
    int cacheUID;
    int effectiveMode;
    int failEpoch;
    volatile int abortRequested;
    unsigned char vcdFallback;
    GSTEXTURE texture;
    char *value;
} load_image_request_t;

enum {
    CACHE_ENTRY_FREE = 0,
    CACHE_ENTRY_QUEUED,
    CACHE_ENTRY_LOADING,
    CACHE_ENTRY_READY,
    CACHE_ENTRY_FAILED
};

#define CACHE_THREAD_PRIORITY           0x40
#define CACHE_MMCE_LOAD_THREAD_PRIORITY 90
#define CACHE_TRANSIENT_RETRY_FRAMES    30
#define CACHE_END_WAIT_TICKS_LAUNCH     500
#define CACHE_END_WAIT_TICKS_TERMINAL   120
#define CACHE_MAX_ENTRIES               256

extern void *_gp;

#define CACHE_THREAD_STACK_SIZE (96 * 1024)

static u8 gArtThreadStack[CACHE_THREAD_STACK_SIZE] ALIGNED(16);
static ee_thread_t gArtThread;
static s32 gArtThreadId = -1;
static s32 gArtSemaId = -1;
static ee_sema_t gArtSema;

static volatile int gArtTerminate = 0;
static volatile int gArtRunning = 0;
static int gArtShutdownAbandoned = 0;
static int gArtQueuedCount = 0;
static int gArtActiveCount = 0;
static int gCacheFailEpoch = 1;
static volatile int gNavInputActive = 0;

static load_image_request_t *gArtCurrentReq = NULL;
static load_image_request_t *gArtReqList = NULL;
static load_image_request_t *gArtReqEnd = NULL;
static image_cache_t *gCacheRegistry = NULL;

static void cacheClearItem(cache_entry_t *item, int freeTexture);
static void cacheWakeWorker(void);

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

static void cacheResetTextureState(GSTEXTURE *texture)
{
    memset(texture, 0, sizeof(*texture));
    texture->ClutStorageMode = GS_CLUT_STORAGE_CSM1;
}

static int cacheLoadImageKey(load_image_request_t *request, char *value)
{
    int result = -1;

    if (gEnableArtTar)
        result = artTarLoadImage(value, request->cache->suffix, &request->texture);

    if (result < 0) {
        result = request->list->itemGetImage(request->list,
                                             request->cache->prefix,
                                             request->cache->isPrefixRelative,
                                             value,
                                             request->cache->suffix,
                                             &request->texture,
                                             GS_PSM_CT24);
    }

    return result;
}

static int cacheGetEffectiveMode(const item_list_t *list, const char *value)
{
    int mode;

    if (list == NULL)
        return -1;

    mode = list->mode;

    if (mode == FAV_MODE && value != NULL) {
        int sourceMode = favGetArtMode(value);
        if (sourceMode >= 0)
            mode = sourceMode;
    }

    if (mode == APP_MODE && value != NULL) {
        int artMode = appGetArtMode(value);
        if (artMode >= 0)
            mode = artMode;
    }

    return mode;
}

static int cacheIsNavigationActive(void)
{
    /*
     * getKeyPressed() is a pure held-state read. getKey() would consume the
     * menu repeat before navigation handles it.
     */
    if (gArtThreadId >= 0 && GetThreadId() == gArtThreadId)
        return gNavInputActive;

    gNavInputActive = getKeyPressed(KEY_LEFT) || getKeyPressed(KEY_RIGHT) ||
                      getKeyPressed(KEY_UP) || getKeyPressed(KEY_DOWN) ||
                      getKeyPressed(KEY_L1) || getKeyPressed(KEY_R1);
    return gNavInputActive;
}

static int cacheGetArtIdleFrames(const image_cache_t *cache, int modeDelay)
{
    return (modeDelay >= 0) ? modeDelay : gArtDelay;
}

static int cacheShouldDeferRequest(const load_image_request_t *request)
{
    if (request == NULL || request->effectiveMode != MMCE_MODE)
        return 0;

    int delay = (request->list != NULL) ? request->list->delay : gArtDelay;
    return cacheIsNavigationActive() ||
           guiInactiveFrames < cacheGetArtIdleFrames(request->cache, delay);
}

int cacheLowerCallerPriority(void)
{
    ee_thread_status_t status;
    int callerPriority = -1;

    memset(&status, 0, sizeof(status));
    if (ReferThreadStatus(GetThreadId(), &status) >= 0) {
        callerPriority = status.current_priority;
        if (callerPriority < CACHE_MMCE_LOAD_THREAD_PRIORITY + 1)
            ChangeThreadPriority(GetThreadId(), CACHE_MMCE_LOAD_THREAD_PRIORITY + 1);
        else
            callerPriority = -1;
    }

    return callerPriority;
}

void cacheRestoreCallerPriority(int savedPriority)
{
    if (savedPriority >= 0)
        ChangeThreadPriority(GetThreadId(), savedPriority);
}

static void cacheWaitForWorkerTick(void)
{
    DelayThread(1000);
}

static void cacheRegister(image_cache_t *cache)
{
    cacheLock();
    cache->registryNext = gCacheRegistry;
    gCacheRegistry = cache;
    cacheUnlock();
}

static void cacheUnregister(image_cache_t *cache)
{
    image_cache_t *entry;
    image_cache_t *previous = NULL;

    cacheLock();

    entry = gCacheRegistry;
    while (entry != NULL) {
        if (entry == cache) {
            if (previous != NULL)
                previous->registryNext = entry->registryNext;
            else
                gCacheRegistry = entry->registryNext;

            entry->registryNext = NULL;
            break;
        }

        previous = entry;
        entry = entry->registryNext;
    }

    cacheUnlock();
}

static void cacheFreeRequestLocked(load_image_request_t *request)
{
    if (request == NULL)
        return;

    /*
     * Worker textures only own EE memory. They are never registered with the
     * renderer's VRAM manager, so freeing them here cannot race its GUI list.
     */
    texFree(&request->texture);
    cacheResetTextureState(&request->texture);

    if (request->cache != NULL && request->cache->activeRequests > 0)
        request->cache->activeRequests--;

    free(request);
}

static void cacheEnqueueRequestLocked(load_image_request_t *request)
{
    request->next = NULL;

    if (gArtReqEnd != NULL)
        gArtReqEnd->next = request;
    else
        gArtReqList = request;

    gArtReqEnd = request;
    gArtQueuedCount++;
}

static int cacheRemoveQueuedRequestLocked(load_image_request_t *target)
{
    load_image_request_t *request = gArtReqList;
    load_image_request_t *previous = NULL;

    while (request != NULL) {
        if (request == target) {
            if (previous != NULL)
                previous->next = request->next;
            else
                gArtReqList = request->next;

            if (gArtReqEnd == request)
                gArtReqEnd = previous;

            request->next = NULL;
            if (gArtQueuedCount > 0)
                gArtQueuedCount--;
            return 1;
        }

        previous = request;
        request = request->next;
    }

    return 0;
}

static void cacheDropQueuedRequestLocked(load_image_request_t *request)
{
    cache_entry_t *entry;

    if (request == NULL || !cacheRemoveQueuedRequestLocked(request))
        return;

    entry = request->entry;
    if (entry != NULL && entry->qr == request) {
        entry->qr = NULL;
        cacheClearItem(entry, 1);
    }

    cacheFreeRequestLocked(request);
}

static int cacheWaitForAllRequestsTimed(int timeoutTicks)
{
    int savedPriority;

    if (!gArtRunning)
        return 1;

    savedPriority = cacheLowerCallerPriority();

    while (timeoutTicks != 0) {
        int pending;

        cacheLock();
        pending = gArtQueuedCount > 0 || gArtActiveCount > 0;
        cacheUnlock();

        if (!pending) {
            cacheRestoreCallerPriority(savedPriority);
            return 1;
        }

        cacheWaitForWorkerTick();
        if (timeoutTicks > 0)
            timeoutTicks--;
    }

    cacheRestoreCallerPriority(savedPriority);
    return 0;
}

static void cacheWaitForCacheRequests(image_cache_t *cache)
{
    int savedPriority = cacheLowerCallerPriority();

    while (1) {
        int pending;

        cacheLock();
        pending = cache->activeRequests;
        cacheUnlock();

        if (!pending)
            break;

        cacheWaitForWorkerTick();
    }

    cacheRestoreCallerPriority(savedPriority);
}

static cache_entry_t *cacheFindEntryByValueLocked(image_cache_t *cache, const char *value, int *entryId)
{
    int i;

    for (i = 0; i < cache->count; i++) {
        cache_entry_t *entry = &cache->content[i];

        if (entry->state != CACHE_ENTRY_FREE &&
            entry->value != NULL &&
            strcmp(entry->value, value) == 0) {
            if (entryId != NULL)
                *entryId = i;
            return entry;
        }
    }

    return NULL;
}

static int cacheHasQueuedModeLocked(int mode)
{
    load_image_request_t *request;

    for (request = gArtReqList; request != NULL; request = request->next) {
        if (request->effectiveMode == mode)
            return 1;
    }

    return 0;
}

static int cacheHasActiveModeLocked(int mode)
{
    return gArtCurrentReq != NULL && gArtCurrentReq->effectiveMode == mode;
}

static load_image_request_t *cacheDequeueRequest(void)
{
    load_image_request_t *request = NULL;

    cacheLock();

    if (gArtReqList != NULL) {
        /*
         * The regular IO worker owns foreground list/config/module work. Art
         * remains queued until that work is clear, then drains in FIFO order.
         */
        if (cacheShouldDeferRequest(gArtReqList)) {
            cacheUnlock();
            return NULL;
        }

        request = gArtReqList;
        gArtReqList = request->next;
        request->next = NULL;

        if (gArtReqEnd == request)
            gArtReqEnd = NULL;

        if (gArtQueuedCount > 0)
            gArtQueuedCount--;
        gArtActiveCount++;
        gArtCurrentReq = request;

        if (request->entry != NULL &&
            request->entry->qr == request &&
            request->entry->state == CACHE_ENTRY_QUEUED)
            request->entry->state = CACHE_ENTRY_LOADING;
    }

    cacheUnlock();
    return request;
}

static void cacheCompleteRequest(load_image_request_t *request, int result)
{
    cacheLock();

    if (gArtCurrentReq == request)
        gArtCurrentReq = NULL;

    if (request->entry != NULL &&
        request->entry->qr == request &&
        request->entry->UID == request->cacheUID &&
        request->cache != NULL &&
        !request->cache->destroying) {
        cache_entry_t *entry = request->entry;

        entry->qr = NULL;

        if (result == ERR_LOAD_ABORTED) {
            cacheClearItem(entry, 0);
        } else if (result < 0 || request->texture.Mem == NULL) {
            entry->lastUsed = guiFrameId;
            entry->state = CACHE_ENTRY_FAILED;
            entry->failAbsent = result == ERR_BAD_FILE &&
                                request->failEpoch == gCacheFailEpoch;
            entry->retryFrame = (unsigned int)guiFrameId +
                                CACHE_TRANSIENT_RETRY_FRAMES;
        } else {
            entry->texture = request->texture;
            cacheResetTextureState(&request->texture);
            entry->lastUsed = guiFrameId;
            entry->state = CACHE_ENTRY_READY;
        }
    }

    if (gArtActiveCount > 0)
        gArtActiveCount--;

    cacheFreeRequestLocked(request);
    cacheUnlock();
}

static int cacheGetLoadThreadPriority(const load_image_request_t *request)
{
    if (request != NULL && request->effectiveMode == MMCE_MODE)
        return CACHE_MMCE_LOAD_THREAD_PRIORITY;

    return CACHE_THREAD_PRIORITY;
}

static void cacheLoadImage(load_image_request_t *request)
{
    ee_thread_status_t status;
    int threadId;
    int loadPriority;
    int originalPriority = -1;
    int result;

    if (request == NULL)
        return;

    if (request->cache == NULL || request->list == NULL || request->entry == NULL) {
        cacheLock();
        if (gArtCurrentReq == request)
            gArtCurrentReq = NULL;
        if (gArtActiveCount > 0)
            gArtActiveCount--;
        cacheFreeRequestLocked(request);
        cacheUnlock();
        return;
    }

    if (request->abortRequested) {
        cacheCompleteRequest(request, ERR_LOAD_ABORTED);
        return;
    }

    threadId = GetThreadId();
    loadPriority = cacheGetLoadThreadPriority(request);
    memset(&status, 0, sizeof(status));

    if (ReferThreadStatus(threadId, &status) >= 0) {
        originalPriority = status.current_priority;
        if (originalPriority != loadPriority)
            ChangeThreadPriority(threadId, loadPriority);
    }

    texSetLoadAbortFlag(&request->abortRequested);
    result = cacheLoadImageKey(request, request->value);

    if (result == ERR_BAD_FILE && request->vcdFallback) {
        char fallbackKey[VCD_ID_MAX];

        if (vcdExtractGameId(request->value, fallbackKey, sizeof(fallbackKey)))
            result = cacheLoadImageKey(request, fallbackKey);
    }

    texSetLoadAbortFlag(NULL);
    cacheCompleteRequest(request, result);

    if (originalPriority >= 0 && originalPriority != loadPriority)
        ChangeThreadPriority(threadId, originalPriority);
}

static void cacheWorkerThread(void *arg)
{
    (void)arg;

    while (!gArtTerminate) {
        SleepThread();

        if (gArtTerminate)
            break;

        while (!gArtTerminate) {
            load_image_request_t *request = cacheDequeueRequest();

            if (request == NULL)
                break;

            cacheLoadImage(request);
        }
    }

    cacheLock();
    cacheUnlock();

    gArtRunning = 0;
    ExitDeleteThread();
}

void cacheInit(void)
{
    if (gArtRunning)
        return;

    /*
     * A previously abandoned worker can still finish after its caller moved
     * on. Reclaim only its semaphore here, once gArtRunning proves it exited.
     */
    if (gArtShutdownAbandoned && gArtSemaId >= 0) {
        DeleteSema(gArtSemaId);
        gArtSemaId = -1;
    }

    gArtTerminate = 0;
    gArtShutdownAbandoned = 0;
    gArtQueuedCount = 0;
    gArtActiveCount = 0;
    gArtCurrentReq = NULL;
    gArtReqList = NULL;
    gArtReqEnd = NULL;
    gNavInputActive = 0;

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
    gArtThread.initial_priority = CACHE_THREAD_PRIORITY;

    gArtThreadId = CreateThread(&gArtThread);
    if (gArtThreadId < 0) {
        DeleteSema(gArtSemaId);
        gArtSemaId = -1;
        return;
    }

    if (StartThread(gArtThreadId, NULL) < 0) {
        DeleteThread(gArtThreadId);
        DeleteSema(gArtSemaId);
        gArtThreadId = -1;
        gArtSemaId = -1;
        return;
    }

    gArtRunning = 1;
}

void cacheEnd(int forceStop)
{
    int waitTicks = forceStop ? CACHE_END_WAIT_TICKS_TERMINAL : CACHE_END_WAIT_TICKS_LAUNCH;
    int savedPriority;

    if (!gArtRunning)
        return;

    (void)cacheCancelPendingImageLoadsTimed(waitTicks);

    gArtTerminate = 1;
    cacheWakeWorker();

    savedPriority = cacheLowerCallerPriority();
    while (gArtRunning && waitTicks-- > 0)
        cacheWaitForWorkerTick();
    cacheRestoreCallerPriority(savedPriority);

    /*
     * Never terminate an art thread inside fileXio. If an abortable staged read
     * still did not return, leave its semaphore/cache storage alive. Terminal
     * teardown will reset the IOP; a later non-terminal init can reclaim the
     * semaphore after the worker exits naturally.
     */
    if (gArtRunning) {
        gArtShutdownAbandoned = 1;
        return;
    }

    cacheLock();
    while (gArtReqList != NULL)
        cacheDropQueuedRequestLocked(gArtReqList);
    gArtQueuedCount = 0;
    gArtActiveCount = 0;
    gArtCurrentReq = NULL;
    cacheUnlock();

    if (gArtSemaId >= 0) {
        DeleteSema(gArtSemaId);
        gArtSemaId = -1;
    }

    gArtThreadId = -1;
}

static void cacheClearItem(cache_entry_t *item, int freeTexture)
{
    if (freeTexture && item->texture.Mem != NULL) {
        /*
         * Use the cache entry's real GSTEXTURE address: gsKit's manager keys
         * its list by pointer identity, not by a copied texture struct.
         */
        rmUnloadTexture(&item->texture);
        texFree(&item->texture);
    }

    free(item->value);

    memset(item, 0, sizeof(*item));
    cacheResetTextureState(&item->texture);
    item->lastUsed = -1;
}

image_cache_t *cacheInitCache(int userId, const char *prefix, int isPrefixRelative, const char *suffix, int count)
{
    image_cache_t *cache;
    int length;
    int i;

    if (suffix == NULL || count <= 0 || count > CACHE_MAX_ENTRIES)
        return NULL;

    cache = malloc(sizeof(*cache));
    if (cache == NULL)
        return NULL;

    memset(cache, 0, sizeof(*cache));
    cache->userId = userId;
    cache->count = count;

    if (prefix != NULL) {
        length = strlen(prefix) + 1;
        cache->prefix = malloc(length);
        if (cache->prefix == NULL) {
            free(cache);
            return NULL;
        }
        memcpy(cache->prefix, prefix, length);
    }

    cache->isPrefixRelative = isPrefixRelative;

    length = strlen(suffix) + 1;
    cache->suffix = malloc(length);
    if (cache->suffix == NULL) {
        free(cache->prefix);
        free(cache);
        return NULL;
    }
    memcpy(cache->suffix, suffix, length);

    cache->content = calloc((size_t)count, sizeof(*cache->content));
    if (cache->content == NULL) {
        free(cache->prefix);
        free(cache->suffix);
        free(cache);
        return NULL;
    }

    cache->nextUID = 1;
    for (i = 0; i < count; i++)
        cacheClearItem(&cache->content[i], 0);

    cacheRegister(cache);
    return cache;
}

static void cacheWakeWorker(void)
{
    if (gArtRunning && gArtThreadId >= 0)
        WakeupThread(gArtThreadId);
}

void cacheDestroyCache(image_cache_t *cache)
{
    int i;

    if (cache == NULL)
        return;

    cacheLock();
    cache->destroying = 1;

    for (i = 0; i < cache->count; i++) {
        cache_entry_t *entry = &cache->content[i];
        load_image_request_t *request = entry->qr;

        if (entry->state == CACHE_ENTRY_QUEUED && request != NULL)
            cacheDropQueuedRequestLocked(request);
        else if (entry->state == CACHE_ENTRY_LOADING && request != NULL)
            request->abortRequested = 1;
        else
            cacheClearItem(entry, 1);
    }

    cacheUnlock();
    cacheWakeWorker();

    if (gArtShutdownAbandoned)
        return;

    cacheWaitForCacheRequests(cache);

    cacheLock();
    for (i = 0; i < cache->count; i++)
        cacheClearItem(&cache->content[i], 1);
    cacheUnlock();

    cacheUnregister(cache);
    free(cache->prefix);
    free(cache->suffix);
    free(cache->content);
    free(cache);
}

void cacheCancelPendingImageLoads(void)
{
    (void)cacheCancelPendingImageLoadsTimed(-1);
}

int cacheCancelPendingImageLoadsTimed(int timeoutTicks)
{
    cacheLock();

    if (gArtCurrentReq != NULL)
        gArtCurrentReq->abortRequested = 1;

    while (gArtReqList != NULL)
        cacheDropQueuedRequestLocked(gArtReqList);

    cacheUnlock();
    cacheWakeWorker();

    return cacheWaitForAllRequestsTimed(timeoutTicks);
}

int cacheAbortMmceImageLoadsTimed(int timeoutTicks)
{
    load_image_request_t *request;
    load_image_request_t *next;
    int savedPriority;

    cacheLock();

    if (gArtCurrentReq != NULL &&
        gArtCurrentReq->effectiveMode == MMCE_MODE)
        gArtCurrentReq->abortRequested = 1;

    for (request = gArtReqList; request != NULL; request = next) {
        next = request->next;
        if (request->effectiveMode == MMCE_MODE)
            cacheDropQueuedRequestLocked(request);
    }

    cacheUnlock();
    cacheWakeWorker();

    savedPriority = cacheLowerCallerPriority();

    while (timeoutTicks != 0) {
        int pending;

        cacheLock();
        pending = cacheHasActiveModeLocked(MMCE_MODE) ||
                  cacheHasQueuedModeLocked(MMCE_MODE);
        cacheUnlock();

        if (!pending) {
            cacheRestoreCallerPriority(savedPriority);
            return 1;
        }

        cacheWaitForWorkerTick();
        if (timeoutTicks > 0)
            timeoutTicks--;
    }

    cacheRestoreCallerPriority(savedPriority);
    return 0;
}

void cacheInvalidateFailMemo(void)
{
    image_cache_t *cache;

    cacheLock();

    if (gCacheFailEpoch == 0x7fffffff)
        gCacheFailEpoch = 1;
    else
        gCacheFailEpoch++;

    /*
     * Clear the entry-side half too. Per-item -2 sentinels self-invalidate on
     * the epoch mismatch; without this pass they would immediately rediscover
     * the old failed entry and memoize it again.
     */
    for (cache = gCacheRegistry; cache != NULL; cache = cache->registryNext) {
        int i;

        if (cache == NULL || cache->destroying)
            continue;

        for (i = 0; i < cache->count; i++) {
            cache_entry_t *entry = &cache->content[i];

            if (entry->state == CACHE_ENTRY_FAILED && entry->failAbsent)
                cacheClearItem(entry, 1);
        }
    }

    cacheUnlock();
}

int cacheHasPendingArt(void)
{
    int pending;

    cacheLock();
    pending = gArtQueuedCount > 0 || gArtActiveCount > 0;
    cacheUnlock();

    return pending;
}

void cachePumpPendingArt(void)
{
    load_image_request_t *request;
    load_image_request_t *next;
    int navigationActive = cacheIsNavigationActive();
    int wakeWorker = 0;

    cacheLock();

    if (navigationActive) {
        // MMCE art is idle-only. Revoke stale demand as soon as input resumes
        // so it cannot sit at the head of the shared FIFO.
        if (gArtCurrentReq != NULL &&
            gArtCurrentReq->effectiveMode == MMCE_MODE)
            gArtCurrentReq->abortRequested = 1;

        for (request = gArtReqList; request != NULL; request = next) {
            next = request->next;
            if (request->effectiveMode == MMCE_MODE)
                cacheDropQueuedRequestLocked(request);
        }
    }

    if (gArtRunning &&
        gArtThreadId >= 0 &&
        gArtActiveCount == 0 &&
        gArtReqList != NULL &&
        !cacheShouldDeferRequest(gArtReqList))
        wakeWorker = 1;
    cacheUnlock();

    if (wakeWorker)
        cacheWakeWorker();
}

GSTEXTURE *cacheGetTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value)
{
    cache_entry_t *entry = NULL;
    cache_entry_t *oldestEntry = NULL;
    load_image_request_t *request;
    GSTEXTURE *result = NULL;
    char *identity;
    int effectiveMode;
    int entryId = -1;
    int oldestEntryId = -1;
    int oldestFrame;
    int requestUID;
    int i;

    if (cache == NULL || cache->destroying || list == NULL ||
        cacheId == NULL || UID == NULL ||
        value == NULL || value[0] == '\0')
        return NULL;

    effectiveMode = cacheGetEffectiveMode(list, value);

    cacheLock();

    if (*cacheId == -2) {
        if (*UID == gCacheFailEpoch) {
            cacheUnlock();
            return NULL;
        }

        *cacheId = -1;
        *UID = -1;
    } else if (*cacheId < -1) {
        *cacheId = -1;
        *UID = -1;
    }

    if (*cacheId >= 0 &&
        *cacheId < cache->count &&
        cache->content[*cacheId].UID == *UID &&
        cache->content[*cacheId].value != NULL &&
        strcmp(cache->content[*cacheId].value, value) == 0) {
        entry = &cache->content[*cacheId];
        entryId = *cacheId;
    } else {
        *cacheId = -1;
        *UID = -1;
        entry = cacheFindEntryByValueLocked(cache, value, &entryId);
    }

    if (entry != NULL) {
        *cacheId = entryId;
        *UID = entry->UID;

        switch (entry->state) {
            case CACHE_ENTRY_QUEUED:
            case CACHE_ENTRY_LOADING:
                cacheUnlock();
                cachePumpPendingArt();
                return NULL;
            case CACHE_ENTRY_READY:
                entry->lastUsed = guiFrameId;
                result = &entry->texture;
                cacheUnlock();
                return result;
            case CACHE_ENTRY_FAILED:
                if (entry->failAbsent) {
                    *cacheId = -2;
                    *UID = gCacheFailEpoch;
                    cacheUnlock();
                    return NULL;
                }

                // Modulo comparison keeps the short retry delay correct across frame-counter wrap.
                if ((unsigned int)guiFrameId - entry->retryFrame >= 0x80000000u) {
                    cacheUnlock();
                    return NULL;
                }

                cacheClearItem(entry, 1);
                *cacheId = -1;
                *UID = -1;
                break;
            default:
                *cacheId = -1;
                *UID = -1;
                break;
        }
    }

    /*
     * Latest-selection-wins, ONE-SLOT "BG" cache only (#296): a background queued
     * or loading for a selection the user has already left must not block the
     * current selection for the whole load (~2s on a slow device). A stale QUEUED
     * background is dropped so the slot scan below recycles it in this same frame;
     * a stale LOADING one is aborted and the normal ERR_LOAD_ABORTED completion
     * clears the slot, so the current selection enqueues on the next frame. READY
     * backgrounds keep plain LRU eviction; every other suffix and cache size is
     * untouched.
     */
    if (cache->count == 1 && cache->suffix != NULL && strcmp(cache->suffix, "BG") == 0) {
        cache_entry_t *only = &cache->content[0];

        if (only->value != NULL && strcmp(only->value, value) != 0) {
            if (only->state == CACHE_ENTRY_QUEUED && only->qr != NULL)
                cacheDropQueuedRequestLocked(only->qr);
            else if (only->state == CACHE_ENTRY_LOADING && only->qr != NULL)
                ((load_image_request_t *)only->qr)->abortRequested = 1;
        }
    }

    // NAV-PARITY TEST BUILD (#340): official gates EVERY art request behind the list's idle
    // settle (`guiInactiveFrames < list->delay`), for every device -- not MMCE-only, and with no
    // cacheIsNavigationActive() pad reads from the render path. So while the user is scrolling,
    // NO art request is enqueued at all; they go out once the selection settles.
    if (guiInactiveFrames < ((list != NULL) ? list->delay : MENU_MIN_INACTIVE_FRAMES)) {
        cacheUnlock();
        return NULL;
    }

    oldestFrame = guiFrameId;
    for (i = 0; i < cache->count; i++) {
        entry = &cache->content[i];

        if ((entry->state == CACHE_ENTRY_FREE ||
             entry->state == CACHE_ENTRY_READY ||
             entry->state == CACHE_ENTRY_FAILED) &&
            entry->lastUsed < oldestFrame) {
            oldestEntry = entry;
            oldestEntryId = i;
            oldestFrame = entry->lastUsed;
        }
    }

    if (oldestEntry == NULL || !gArtRunning) {
        cacheUnlock();
        return NULL;
    }

    request = malloc(sizeof(*request));
    identity = malloc(strlen(value) + 1);
    if (request == NULL || identity == NULL) {
        free(request);
        free(identity);
        cacheUnlock();
        return NULL;
    }

    strcpy(identity, value);
    memset(request, 0, sizeof(*request));
    cacheResetTextureState(&request->texture);

    requestUID = cache->nextUID;
    if (cache->nextUID == 0x7fffffff)
        cache->nextUID = 1;
    else
        cache->nextUID++;

    cacheClearItem(oldestEntry, 1);
    oldestEntry->value = identity;
    oldestEntry->qr = request;
    oldestEntry->state = CACHE_ENTRY_QUEUED;
    oldestEntry->UID = requestUID;

    request->cache = cache;
    request->entry = oldestEntry;
    request->list = list;
    request->cacheUID = requestUID;
    request->effectiveMode = effectiveMode;
    request->failEpoch = gCacheFailEpoch;
    request->vcdFallback = vcdViewActive(list->mode);
    request->value = identity;

    *cacheId = oldestEntryId;
    *UID = requestUID;

    cache->activeRequests++;
    cacheEnqueueRequestLocked(request);
    cacheUnlock();

    cacheWakeWorker();
    return NULL;
}
