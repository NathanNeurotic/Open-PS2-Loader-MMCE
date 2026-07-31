#include "include/opl.h"
#include "include/diag.h"
#include "include/appsupport.h"
#include "include/favsupport.h"
#include "include/pad.h"
#include "include/texcache.h"
#include "include/textures.h"
#include "include/gui.h"
#include "include/ioman.h"
#include "include/util.h"
#include "include/renderman.h"
#include "include/vcdsupport.h"

#include "include/tar.h"

#include <delaythread.h>
#include <kernel.h>
#include <ps2sdkapi.h>
#include <malloc.h>

// Bridge: load a game's cover art from the ART/art.tar archive (gated by gEnableArtTar). Builds the
// in-archive entry name (<value>_<suffix>.png -- the same name the loose ART/ file would have), pulls
// the PNG bytes via the tar engine, and decodes them through the bounded memory path. Returns >= 0 on
// a hit, -1 on any miss (no tar / entry absent / OOM / bad PNG) so the caller falls back to the loose
// file. Runs on the art worker thread, like the loose-file load it replaces.
static int artTarLoadImage(const char *value, const char *suffix, GSTEXTURE *texture)
{
    char name[64];
    TarEntryBase *entry;
    void *buf;
    int result;

    if (snprintf(name, sizeof(name), "%s_%s.png", value, suffix) >= (int)sizeof(name))
        return -1; // longer than an ART entry name can be -> cannot match; use the loose file

    entry = tarFind(TAR_KIND_ART, name);
    if (entry == NULL)
        return -1;

    buf = malloc(entry->rawSize);
    if (buf == NULL)
        return -1;

    if (tarRead(TAR_KIND_ART, entry, buf, entry->rawSize) != entry->rawSize) {
        free(buf);
        return -1;
    }

    result = texLoadFromMemory(texture, buf, entry->rawSize);
    free(buf);
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
    int generation;
    int failEpoch; // gCacheFailEpoch when this request was ENQUEUED -- see the completion path
    volatile int abortRequested;
    unsigned char priority;
    unsigned char vcdFallback;
    GSTEXTURE texture;
    char *value;
} load_image_request_t;

static int cacheLoadImageKey(load_image_request_t *req, char *value)
{
    int result = -1;

    if (gEnableArtTar)
        result = artTarLoadImage(value, req->cache->suffix, &req->texture);
    if (result < 0)
        result = req->list->itemGetImage(req->list, req->cache->prefix, req->cache->isPrefixRelative, value, req->cache->suffix, &req->texture, GS_PSM_CT24);
    return result;
}

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

enum {
    CACHE_REQ_PRIORITY_INTERACTIVE = 0,
    CACHE_REQ_PRIORITY_PREFETCH
};

#define CACHE_SLOW_MODE_INTERACTIVE_DELAY 4
#define CACHE_MMCE_INTERACTIVE_MAX_DELAY  12
#define CACHE_APP_INTERACTIVE_MAX_DELAY   4
#define CACHE_APP_PREFETCH_DELAY          10
#define CACHE_PRIME_IDLE_DELAY            12
#define CACHE_THREAD_PRIORITY             0x40
#define CACHE_MMCE_LOAD_THREAD_PRIORITY   90
#define CACHE_END_WAIT_TICKS_FORCE        120
#define CACHE_END_WAIT_TICKS_SOFT         15

extern void *_gp;

#define CACHE_THREAD_STACK_SIZE (96 * 1024)

static u8 gArtThreadStack[CACHE_THREAD_STACK_SIZE] ALIGNED(16);
static ee_thread_t gArtThread;
static s32 gArtThreadId = -1;

static s32 gArtSemaId = -1;
static ee_sema_t gArtSema;

static int gArtTerminate = 0;
static int gArtRunning = 0;
static int gArtShutdownAbandoned = 0;
static int gArtQueuedCount = 0;
static int gArtActiveCount = 0;
static int gArtInteractiveActiveCount = 0;
static int gCacheGeneration = 1;
// Separate epoch for GENUINE-ABSENCE art memos. gCacheGeneration bumps on every screen switch and cursor
// move (cacheAdvanceGeneration), which is correct for scroll/nav churn but WRONG for a known-missing file:
// keying an absence on it made every info-page entry re-run the same ~6 failing opens for a PS1 game with
// no art, wedging the slow MMCE SIO2 bus (#154, the "baseline info-entry read burst" #120 never cut). This
// epoch bumps ONLY on a deliberate settings/theme apply (cacheInvalidateFailMemo, from applyConfig) -- NOT
// on generation advance and NEVER on the background rescan poll (that re-hammer is exactly what reintroduces
// #120) -- so a genuine absence is probed ONCE and then costs zero opens until the user changes something.
static int gCacheFailEpoch = 1;
static load_image_request_t *gArtCurrentReq = NULL;
/* Navigation-active snapshot. getKey() mutates GUI-thread-only pad-repeat state,
 * so the art worker thread must not call it; the GUI thread refreshes this flag
 * (via cacheIsNavigationActive) each frame and the worker reads the snapshot. */
static volatile int gNavInputActive = 0;

static load_image_request_t *gArtInteractiveReqList = NULL;
static load_image_request_t *gArtInteractiveReqEnd = NULL;
static load_image_request_t *gArtPrefetchReqList = NULL;
static load_image_request_t *gArtPrefetchReqEnd = NULL;
static load_image_request_t *gArtCleanupReqList = NULL;
static cache_registry_entry_t *gCacheRegistry = NULL;

static void cacheClearItem(cache_entry_t *item, int freeTxt);
static void cacheResetTextureState(GSTEXTURE *texture);
static void cacheResetRequestTrackingLocked(void);
static void cacheWakeWorker(void);
static void cacheDropQueuedRequestLocked(load_image_request_t *req);

static void cacheNextGenerationLocked(void)
{
    gCacheGeneration++;
    if (gCacheGeneration <= 0)
        gCacheGeneration = 1;
}

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

/* Lower the calling thread's priority below CACHE_MMCE_LOAD_THREAD_PRIORITY so
 * that the art worker thread (which runs at that priority during MMCE reads) can
 * be scheduled and complete or respond to an abort request.  Without this, any
 * higher-priority caller busy-waiting in the wait loops would starve the art
 * thread, causing every timed abort to time out and eventually forcing
 * TerminateThread, which kills the art thread mid-fileXio call and leaves the
 * RPC channel in a broken state.
 *
 * Returns the caller's original priority (to be passed to cacheRestoreCallerPriority),
 * or -1 if the priority was already low enough / could not be determined.
 *
 * Non-static: also used by gui.c's guiHandleDeferedIO() so the GUI thread does not
 * starve the art worker while busy-waiting for a deferred IO op (issue #45).
 */
int cacheLowerCallerPriority(void)
{
    ee_thread_status_t status;
    int callerPriority = -1;

    memset(&status, 0, sizeof(status));
    /* EE ReferThreadStatus() returns the thread STATUS (THS_RUN=1 for the running thread that
     * GetThreadId() names here), or a negative error -- it does NOT return 0 on success (that is
     * the IOP thbase variant). The old `== 0` guard was therefore ALWAYS false, so this function
     * never actually lowered the caller and always returned -1: the #45/#111 priority handoff was
     * silently inert. `>= 0` is the correct success test. */
    if (ReferThreadStatus(GetThreadId(), &status) >= 0) {
        callerPriority = status.current_priority;
        if (callerPriority < CACHE_MMCE_LOAD_THREAD_PRIORITY + 1)
            ChangeThreadPriority(GetThreadId(), CACHE_MMCE_LOAD_THREAD_PRIORITY + 1);
        else
            callerPriority = -1; /* already low enough, nothing to restore */
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

static void cacheFreeRequest(load_image_request_t *req)
{
    if (req == NULL)
        return;

    /* req->texture is only ever decoded into EE RAM and struct-copied into the
     * cache entry on success; it is never bound to VRAM via the TexManager.
     * Free EE RAM only -- calling gsKit_TexManager_free (rmUnloadTexture) here
     * would run on the art worker thread and race the GUI thread's per-frame
     * TexManager list mutation. */
    texFree(&req->texture);
    cacheResetTextureState(&req->texture);
    free(req);
}

static void cacheFinalizeRequestLocked(load_image_request_t *req)
{
    if (req != NULL && req->cache != NULL && req->cache->activeRequests > 0)
        req->cache->activeRequests--;
}

static void cacheQueueCleanupRequestLocked(load_image_request_t *req)
{
    if (req == NULL)
        return;

    req->next = gArtCleanupReqList;
    gArtCleanupReqList = req;
}

static void cacheProcessCleanupRequests(void)
{
    while (1) {
        load_image_request_t *req;

        cacheLock();
        req = gArtCleanupReqList;
        if (req != NULL) {
            gArtCleanupReqList = req->next;
            req->next = NULL;
            cacheFinalizeRequestLocked(req);
        }
        cacheUnlock();

        if (req == NULL)
            break;

        cacheFreeRequest(req);
    }
}

static void cacheResetRequestTrackingLocked(void)
{
    cache_registry_entry_t *registry = gCacheRegistry;

    // Drain the queued requests instead of just NULLing the list heads: each queued request owns
    // heap memory AND its cache entry's qr backpointer. Dropping the heads leaked every request
    // still queued at reset time and left entry->qr dangling -- a later invalidate pass would then
    // dereference freed memory through it. Clear the entry linkage first, then finalize + free.
    load_image_request_t *queues[2] = {gArtInteractiveReqList, gArtPrefetchReqList};
    for (int q = 0; q < 2; q++) {
        load_image_request_t *req = queues[q];
        while (req != NULL) {
            load_image_request_t *next = req->next;
            req->next = NULL;
            if (req->entry != NULL && req->entry->qr == req) {
                req->entry->qr = NULL;
                if (req->entry->state == CACHE_ENTRY_QUEUED)
                    cacheClearItem(req->entry, 1);
            }
            cacheFinalizeRequestLocked(req);
            cacheFreeRequest(req);
            req = next;
        }
    }

    gArtInteractiveReqList = NULL;
    gArtInteractiveReqEnd = NULL;
    gArtPrefetchReqList = NULL;
    gArtPrefetchReqEnd = NULL;
    gArtQueuedCount = 0;
    gArtActiveCount = 0;
    gArtInteractiveActiveCount = 0;

    while (gArtCleanupReqList != NULL) {
        load_image_request_t *req = gArtCleanupReqList;

        gArtCleanupReqList = req->next;
        req->next = NULL;
        cacheFinalizeRequestLocked(req);
        cacheFreeRequest(req);
    }

    while (registry != NULL) {
        image_cache_t *cache = registry->cache;

        if (cache != NULL) {
            cache->activeRequests = 0;
            cache->queuedPrefetchRequests = 0;
        }

        registry = registry->next;
    }
}

static load_image_request_t **cacheGetQueueHead(unsigned char priority)
{
    return priority == CACHE_REQ_PRIORITY_PREFETCH ? &gArtPrefetchReqList : &gArtInteractiveReqList;
}

static load_image_request_t **cacheGetQueueTail(unsigned char priority)
{
    return priority == CACHE_REQ_PRIORITY_PREFETCH ? &gArtPrefetchReqEnd : &gArtInteractiveReqEnd;
}

static int cacheGetPrefetchLimit(const image_cache_t *cache)
{
    if (cache == NULL || cache->count <= 1)
        return 0;

    // #296 walk/cap consistency: drawCoverFlow's idle prefetch walks +/-(count-1)/2 covers --
    // 8 requests on the 10-slot COV cache -- so a cap of 4 silently dropped the far half of
    // every walk (the +/-3 and +/-4 covers never queued, then re-missed when the user stepped
    // again). Size the cap to the walk: count-1 admits the whole warmed window (walk + the
    // visible selection still fits the slots, so the LRU never has to evict a visible cover --
    // the #297 anti-thrash invariant), and the 8 ceiling bounds the queue for larger caches.
    return cache->count - 1 < 8 ? cache->count - 1 : 8;
}

static int cacheGetEffectiveMode(const item_list_t *list, const char *value)
{
    int mode;

    if (list == NULL)
        return -1;

    mode = list->mode;
    // A FAV-tab read proxies to the favourite's OWNER device (favGetImage), so resolve to the
    // owner's mode FIRST or every mode-keyed gate below and at the call sites (MMCE prefetch
    // exemption, MMCE/HDD nav-time defer, MMCE abort sweeps, MMCE/HDD load-priority drop) is
    // blind to e.g. an MMCE-sourced favourite's SIO2 read -- the #116/#120 class of contention
    // with the FAV wrapper stripping the mode. An APP-sourced favourite falls through to the
    // existing APP branch (value is the app's startup, exactly what appGetArtMode keys on).
    // Thread contract: this runs on the GUI thread only (all callers are render-path); the art
    // WORKER's defer check reads the request's stamped effectiveMode instead
    // (cacheShouldDeferInteractiveArtOnInputMode). favArray itself is rebuilt UNLOCKED on the
    // prio-30 IO worker (menuDeferredUpdate) -- see favGetArtMode's safety note for why the
    // GUI-thread scan is safe against that rebuild.
    if (mode == FAV_MODE && value != NULL) {
        int srcMode = favGetArtMode(value);

        if (srcMode >= 0)
            mode = srcMode;
    }
    if (mode == APP_MODE && value != NULL) {
        int artMode = appGetArtMode(value);

        if (artMode >= 0)
            mode = artMode;
    }

    return mode;
}

static int cacheGetBaseDelay(const item_list_t *list)
{
    if (list != NULL && list->delay >= MENU_MIN_INACTIVE_FRAMES)
        return list->delay;

    return MENU_MIN_INACTIVE_FRAMES;
}

// Delay helpers take the caller's already-resolved effective mode: cacheGetEffectiveMode's FAV
// resolution scans favArray per call, so resolve ONCE per cacheGetTextureInternal invocation.
static int cacheGetInteractiveDelay(const item_list_t *list, int mode)
{
    int delay = cacheGetBaseDelay(list);

    // #297 follow-up: key the APP cases off the RESOLVED mode too. list->mode covers the apps
    // list itself (its resolved mode is the art DEVICE mode, and a non-MMCE device must still
    // skip the settle via the early return below); mode == APP_MODE adds the app-sourced FAV
    // favourite whose art has no device configured (cacheGetEffectiveMode leaves APP_MODE when
    // appGetArtMode < 0), which must behave exactly like the same app on the apps page.
    if (list != NULL && (list->mode == APP_MODE || mode == APP_MODE)) {
        if (mode != MMCE_MODE)
            return 0;
    }

    // #296/#299 baseline restore: fast devices (USB/ETH, HDD, and any list-less caller) get NO
    // interactive settle -- fire the art request on draw, mid-hold-scroll included. This is
    // official OPL's behaviour: upstream texcache.c gates on guiInactiveFrames < list->delay
    // with the delay DEFAULTING TO 0 (the *_frames_delay config keys are unset by default),
    // and uOPL feels instant for the same reason. cacheGetBaseDelay's 8-frame floor
    // (MENU_MIN_INACTIVE_FRAMES) was built for slow devices, so the slow-mode protections
    // below stay untouched: MMCE keeps its floor/cap and APP its caps. Fail-storm protection
    // is unaffected (epoch fail memos) and duplicate requests still dedup via
    // cacheFindQueuedRequestLocked.
    if (mode != MMCE_MODE && mode != APP_MODE)
        return 0;

    if ((mode == APP_MODE || mode == MMCE_MODE) && delay < CACHE_SLOW_MODE_INTERACTIVE_DELAY)
        delay = CACHE_SLOW_MODE_INTERACTIVE_DELAY;

    if (list != NULL && (list->mode == APP_MODE || mode == APP_MODE) && delay > CACHE_APP_INTERACTIVE_MAX_DELAY)
        delay = CACHE_APP_INTERACTIVE_MAX_DELAY;

    if (mode == MMCE_MODE && delay > CACHE_MMCE_INTERACTIVE_MAX_DELAY)
        delay = CACHE_MMCE_INTERACTIVE_MAX_DELAY;

    return delay;
}

static int cacheGetPrefetchDelay(const item_list_t *list, int mode)
{
    int delay = cacheGetBaseDelay(list);

    if (mode == APP_MODE && delay < CACHE_APP_PREFETCH_DELAY)
        delay = CACHE_APP_PREFETCH_DELAY;

    return delay;
}

static int cacheHasPendingInteractiveArtLocked(void)
{
    return gArtInteractiveReqList != NULL || gArtInteractiveActiveCount > 0;
}

// Mode checks span BOTH queues: MMCE-backed requests can sit on either queue, and every
// quiesce path (launch, theme swap, config IO) must drain those as well.
static int cacheHasQueuedModeLocked(int mode)
{
    load_image_request_t *req;

    for (req = gArtInteractiveReqList; req != NULL; req = req->next) {
        if (req->effectiveMode == mode)
            return 1;
    }
    for (req = gArtPrefetchReqList; req != NULL; req = req->next) {
        if (req->effectiveMode == mode)
            return 1;
    }

    return 0;
}

static int cacheHasActiveModeLocked(int mode)
{
    return gArtCurrentReq != NULL && gArtCurrentReq->effectiveMode == mode;
}

static int cacheIsAbortableMmceRequest(load_image_request_t *req)
{
    // Any MMCE-backed read is abortable, whatever its queue: the abort flag is polled by the same
    // 4KB staging loop regardless of priority, and a PREFETCH read holds the SIO2 bus just
    // like an interactive one.
    return req != NULL && req->effectiveMode == MMCE_MODE;
}


static int cacheIsNavigationActive(void)
{
    /* Use getKeyPressed() (a pure `paddata & mask` read), NOT getKey(): getKey() CONSUMES
     * the button-repeat -- it resets delaycnt when a repeat fires (pad.c). This runs on the
     * GUI thread while DRAWING the MMCE cover, which happens BEFORE the input handler in the
     * SAME frame, so getKey() here stole the held-d-pad repeat from menuNextV/menuPrevV and
     * the MMCE cursor failed to advance while scrolling (the "MMCE nav is laggy / not working"
     * report -- MMCE-only because only MMCE reaches this defer path). getKeyPressed() reads the
     * raw held state with no side effects, so the cursor advances normally AND the snapshot
     * stays true for the WHOLE hold (not just repeat-fire frames), keeping interactive MMCE
     * cover reads correctly deferred off the SIO2 bus during a scroll. The art worker still
     * reads the snapshot; getKeyPressed is side-effect free so the thread guard below is no
     * longer strictly required, but is kept unchanged. */
    if (gArtThreadId >= 0 && GetThreadId() == gArtThreadId)
        return gNavInputActive;

    gNavInputActive = (getKeyPressed(KEY_LEFT) || getKeyPressed(KEY_RIGHT) || getKeyPressed(KEY_UP) ||
                       getKeyPressed(KEY_DOWN) || getKeyPressed(KEY_L1) || getKeyPressed(KEY_R1));
    return gNavInputActive;
}

// Defer interactive cover reads while the user is actively scrolling on the MMCE (SIO2)
// backend: it reads art through the single fileXio channel the menu also uses for badge/config
// IO, so starting a read mid-scroll stalls nav. USB/exFAT BDM and APA-HDD are fast and stay
// unthrottled, matching official OPL. Covers still land once navigation goes idle (the
// per-value settle below).
static int cacheShouldDeferInteractiveArtOnInputMode(int effectiveMode)
{
    if (effectiveMode == MMCE_MODE)
        return cacheIsNavigationActive();

    return 0;
}


static int cacheGetLoadThreadPriority(const load_image_request_t *req)
{
    if (req != NULL && req->list != NULL) {
        // MMCE reads art through the single slow fileXio channel; deprioritize the
        // art worker (higher number = lower EE priority) so an in-flight read yields that channel to
        // the priority-30 IO worker's nav-time badge/config reads instead of starving them.
        if (req->effectiveMode == MMCE_MODE)
            return CACHE_MMCE_LOAD_THREAD_PRIORITY;

        if (req->list->mode == APP_MODE)
            return 0x38;
    }

    return CACHE_THREAD_PRIORITY;
}

static void cacheEnqueueRequestLocked(load_image_request_t *req)
{
    load_image_request_t **head;
    load_image_request_t **tail;

    req->next = NULL;
    head = cacheGetQueueHead(req->priority);
    tail = cacheGetQueueTail(req->priority);

    if (*tail != NULL)
        (*tail)->next = req;
    else
        *head = req;

    *tail = req;
    gArtQueuedCount++;

    if (req->priority == CACHE_REQ_PRIORITY_PREFETCH && req->cache != NULL)
        req->cache->queuedPrefetchRequests++;
}

/* Speculative work always yields to real demand: when INTERACTIVE demand arrives while a
 * prefetch read is in flight, flag that read for abort so the selected item's cover does not
 * queue behind up to a whole cover read it never asked for. This is the #296 patterned ~1 s
 * stall (Beta-3457, zackcage6): #297's settle-triggered prefetch bursts monopolized the single
 * art worker exactly when the user was most likely to tap again, in-flight USB reads were not
 * preemptible (the MMCE abort sweeps don't cover fast modes), and the burst re-armed each time
 * the cursor exited the warmed +/-4 window -- i.e. every ~5 items. Both readers check
 * texLoadAbortRequested() between chunks, so the yield lands within one read chunk (~32 KB).
 * A same-value in-flight prefetch never reaches here (the loading-entry path returns first),
 * and an aborted prefetch is safe by construction: ERR_LOAD_ABORTED clears the entry and the
 * walk re-requests it at the next settle. Mode-agnostic on purpose: prefetch is by definition
 * speculative on every backend. */
static void cacheYieldInFlightPrefetchLocked(void)
{
    if (gArtCurrentReq != NULL && gArtCurrentReq->priority == CACHE_REQ_PRIORITY_PREFETCH)
        gArtCurrentReq->abortRequested = 1;
}

static int cacheRemoveQueuedRequestLocked(load_image_request_t *target)
{
    load_image_request_t *req = *cacheGetQueueHead(target->priority);
    load_image_request_t *previous = NULL;
    load_image_request_t **head = cacheGetQueueHead(target->priority);
    load_image_request_t **tail = cacheGetQueueTail(target->priority);

    while (req != NULL) {
        if (req == target) {
            if (previous != NULL)
                previous->next = req->next;
            else
                *head = req->next;

            if (*tail == req)
                *tail = previous;

            req->next = NULL;
            if (gArtQueuedCount > 0)
                gArtQueuedCount--;

            if (req->priority == CACHE_REQ_PRIORITY_PREFETCH && req->cache != NULL && req->cache->queuedPrefetchRequests > 0)
                req->cache->queuedPrefetchRequests--;

            return 1;
        }

        previous = req;
        req = req->next;
    }

    return 0;
}

static void cacheDropQueuedRequestLocked(load_image_request_t *req)
{
    cache_entry_t *entry;

    if (req == NULL)
        return;

    entry = req->entry;
    if (entry != NULL && entry->qr == req) {
        entry->qr = NULL;
        if (entry->state == CACHE_ENTRY_QUEUED)
            cacheClearItem(entry, 1);
    }

    if (cacheRemoveQueuedRequestLocked(req))
        cacheQueueCleanupRequestLocked(req);
}

static void cachePromoteQueuedRequestLocked(load_image_request_t *req)
{
    if (req == NULL || req->priority != CACHE_REQ_PRIORITY_PREFETCH)
        return;

    if (!cacheRemoveQueuedRequestLocked(req))
        return;

    req->priority = CACHE_REQ_PRIORITY_INTERACTIVE;
    cacheEnqueueRequestLocked(req);
}

static void cacheInvalidateEntryLocked(cache_entry_t *entry, int freeTxt, int preserveLoaded)
{
    load_image_request_t *req = entry->qr;

    switch (entry->state) {
        case CACHE_ENTRY_QUEUED:
            // Teardown (preserveLoaded==0) drops the queued request. But a per-scroll / per-view-switch
            // generation advance (preserveLoaded==1) must LEAVE queued covers enqueued so the single art
            // worker drains them and they land in the LRU cache -- exactly like wOPL, which never cancels
            // queued loads on navigation. Clearing them here made MMCE coverflow re-wait from scratch on
            // every scroll step / device-tab switch (issue #116); the LOADING/READY cases below already
            // honor preserveLoaded, this one did not. On scroll-back a still-QUEUED entry is matched by
            // its UID and returned as "loading" (no double-enqueue), then completes normally.
            if (!preserveLoaded) {
                entry->qr = NULL;
                if (req != NULL && cacheRemoveQueuedRequestLocked(req))
                    cacheQueueCleanupRequestLocked(req);
                cacheClearItem(entry, freeTxt);
            }
            break;
        case CACHE_ENTRY_LOADING:
            if (req != NULL) {
                // Teardown (preserveLoaded==0: cacheEnd / cacheDestroyCache / the timed cancel+abort
                // family) aborts the in-flight read. A generation advance (preserveLoaded==1: per-
                // scroll step, tab/page switch, screen switch, list sort/clear) NEVER does -- wOPL
                // semantics (test note #3): the read finishes, lands in its entry (completion is
                // UID-guarded in cacheCompleteRequest -- qr==req && UID==cacheUID -- and every draw
                // keys on a per-item UID match, so a late texture can never render on a wrong item)
                // and persists for the scroll-back / tab-switch-back. The old nav-gated MMCE abort
                // (#116 "held in reserve", #120 static-screen gate) cancelled the in-flight read on
                // every held-nav carousel step, discarding partial SIO2 progress and forcing a full
                // re-read after settle -- the residual "backs up like it's buffering". It bought
                // nothing: during a hold, cacheShouldDeferInteractiveArtOnInputMode already blocks new
                // MMCE/HDD enqueues (cacheGetTextureInternal) AND worker dequeues (cacheDequeueRequest),
                // so at most this ONE read finishes into an otherwise idle bus.
                if (!preserveLoaded)
                    req->abortRequested = 1;
            } else {
                entry->qr = NULL;
                cacheClearItem(entry, freeTxt);
            }
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

static void cacheInvalidateInteractiveRequestsLocked(void)
{
    cache_registry_entry_t *registry = gCacheRegistry;

    while (registry != NULL) {
        image_cache_t *cache = registry->cache;

        if (cache != NULL && !cache->destroying) {
            for (int i = 0; i < cache->count; i++) {
                cache_entry_t *entry = &cache->content[i];
                load_image_request_t *req = entry->qr;

                if (req == NULL || req->priority != CACHE_REQ_PRIORITY_INTERACTIVE)
                    continue;

                switch (entry->state) {
                    case CACHE_ENTRY_QUEUED:
                        entry->qr = NULL;
                        if (cacheRemoveQueuedRequestLocked(req))
                            cacheQueueCleanupRequestLocked(req);
                        cacheClearItem(entry, 1);
                        break;
                    case CACHE_ENTRY_LOADING:
                        if (req != NULL)
                            req->abortRequested = 1;
                        else {
                            entry->qr = NULL;
                            cacheClearItem(entry, 1);
                        }
                        break;
                    default:
                        break;
                }
            }
        }

        registry = registry->next;
    }
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
        pending = (gArtQueuedCount > 0) || (gArtActiveCount > 0);
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

static load_image_request_t *cacheFindQueuedRequestLocked(image_cache_t *cache, char *value)
{
    load_image_request_t *req;

    for (req = gArtInteractiveReqList; req != NULL; req = req->next) {
        if (req->cache == cache && strcmp(req->value, value) == 0)
            return req;
    }

    for (req = gArtPrefetchReqList; req != NULL; req = req->next) {
        if (req->cache == cache && strcmp(req->value, value) == 0)
            return req;
    }

    return NULL;
}

static cache_entry_t *cacheFindLoadingEntryLocked(image_cache_t *cache, char *value, int *entryId)
{
    for (int i = 0; i < cache->count; i++) {
        cache_entry_t *entry = &cache->content[i];
        load_image_request_t *req;

        if (entry->state != CACHE_ENTRY_LOADING || entry->qr == NULL)
            continue;

        req = (load_image_request_t *)entry->qr;
        if (req->cache == cache && strcmp(req->value, value) == 0) {
            if (entryId != NULL)
                *entryId = i;
            return entry;
        }
    }

    return NULL;
}

static load_image_request_t *cacheDequeueRequest(void)
{
    load_image_request_t *req = NULL;

    cacheLock();

    if (gArtInteractiveReqList != NULL) {
        // Mode variant, NOT the (list, value) resolver: this runs on the art worker thread, and
        // the resolver's FAV lookup scans favArray unlocked. effectiveMode was stamped on the
        // GUI thread at enqueue.
        if (cacheShouldDeferInteractiveArtOnInputMode(gArtInteractiveReqList->effectiveMode)) {
            cacheUnlock();
            return NULL;
        }

        req = gArtInteractiveReqList;
        gArtInteractiveReqList = req->next;
        req->next = NULL;

        if (gArtInteractiveReqEnd == req)
            gArtInteractiveReqEnd = NULL;
    } else if (gArtPrefetchReqList != NULL) {
        // Same nav-time defer the interactive head gets: a prefetch read on a slow shared
        // bus must never START mid-scroll.
        if (cacheShouldDeferInteractiveArtOnInputMode(gArtPrefetchReqList->effectiveMode)) {
            cacheUnlock();
            return NULL;
        }

        /* #296: speculative reads also yield the bus to real IO. Official OPL serializes art
         * BEHIND everything else on the single IO queue; here the art worker has its own
         * channel, so a prefetch staged read can collide with queued io work (boot-time
         * module/list/config loads) and lose with a contended-bus EIO -- which then
         * poisons the entry's failure memo. Don't START a prefetch while the io queue has
         * work pending; interactive dequeues stay aggressive. */
        if (ioHasPendingRequests()) {
            cacheUnlock();
            return NULL;
        }

        req = gArtPrefetchReqList;
        gArtPrefetchReqList = req->next;
        req->next = NULL;

        if (gArtPrefetchReqEnd == req)
            gArtPrefetchReqEnd = NULL;
    }

    if (req != NULL) {
        if (gArtQueuedCount > 0)
            gArtQueuedCount--;
        if (req->priority == CACHE_REQ_PRIORITY_PREFETCH && req->cache != NULL && req->cache->queuedPrefetchRequests > 0)
            req->cache->queuedPrefetchRequests--;
        gArtActiveCount++;
        if (req->priority == CACHE_REQ_PRIORITY_INTERACTIVE)
            gArtInteractiveActiveCount++;
        gArtCurrentReq = req;

        if (req->entry != NULL && req->entry->qr == req && req->entry->state == CACHE_ENTRY_QUEUED)
            req->entry->state = CACHE_ENTRY_LOADING;
    }

    cacheUnlock();

    return req;
}

static void cacheCompleteRequest(load_image_request_t *req, int result)
{
    cacheLock();

    if (gArtCurrentReq == req)
        gArtCurrentReq = NULL;

    if (req->entry != NULL && req->entry->qr == req && req->entry->UID == req->cacheUID && req->cache != NULL && !req->cache->destroying) {
        req->entry->qr = NULL;
        req->entry->primeFrame = -1;

        // Only a genuinely ABORTED read is discarded. A read that COMPLETES is stored even if the
        // cursor moved during it (generation advanced): the enclosing guard above already proved this
        // entry still belongs to THIS request (qr==req && UID==cacheUID, and UIDs are monotonic per request), so
        // the texture can only ever be shown for its own item. Discarding a finished cover just because
        // the user scrolled forced a full re-read on the slow MMCE link when they scrolled back -- the
        // "MMCE covers never stick / pop in and out" churn.
        if (result == ERR_LOAD_ABORTED) {
            cacheClearItem(req->entry, 0);
        } else if (result < 0 || req->texture.Mem == NULL) {
            req->entry->lastUsed = 0;
            req->entry->state = CACHE_ENTRY_FAILED;
            // ONLY ERR_BAD_FILE is a genuine absence (open() failed = no such file) -> long-term memo.
            // ERR_FILE_IO (contended-bus read/stage error) and a decode that yielded no pixels
            // (result >= 0 but Mem == NULL) are transient/present-but-broken -> re-probe next generation,
            // so a real file is never permanently hidden by a one-off bus hiccup (the reason the earlier
            // gCacheFailEpoch attempt was rejected -- PR #134's ERR_FILE_IO split is what makes it safe).
            //
            // AND require the fail-epoch to be UNCHANGED since this request started (req->failEpoch ==
            // gCacheFailEpoch). cacheInvalidateFailMemo() (settings/theme apply) bumps the epoch precisely
            // to force cover-less items to re-probe -- e.g. the user just copied a cover onto the card.
            // A request already IN FLIGHT across that apply must NOT be allowed to write a genuine-absence
            // memo under the NEW epoch: that would re-suppress the very re-probe the apply asked for, so
            // the freshly-added art would not appear until a SECOND apply (CodeRabbit review of #154).
            // Downgrading a stale-epoch absence to transient (failAbsent = 0) re-probes it next generation
            // -- imminent, and exactly the item most likely to have just gained art.
            req->entry->failAbsent = (result == ERR_BAD_FILE && req->failEpoch == gCacheFailEpoch);
        } else {
            req->entry->texture = req->texture;
            cacheResetTextureState(&req->texture);
            req->entry->lastUsed = guiFrameId;
            req->entry->state = CACHE_ENTRY_READY;
        }
    }

    if (gArtActiveCount > 0)
        gArtActiveCount--;
    if (req->priority == CACHE_REQ_PRIORITY_INTERACTIVE && gArtInteractiveActiveCount > 0)
        gArtInteractiveActiveCount--;

    cacheFinalizeRequestLocked(req);
    cacheUnlock();

    cacheFreeRequest(req);
}

static void cacheLoadImage(load_image_request_t *req)
{
    ee_thread_status_t status;
    int threadId;
    int loadPriority;
    int originalPriority = -1;
    int result = -1;

    if (req == NULL || req->cache == NULL || req->list == NULL || req->entry == NULL) {
        cacheLock();
        if (gArtCurrentReq == req)
            gArtCurrentReq = NULL;
        if (gArtActiveCount > 0)
            gArtActiveCount--;
        if (req != NULL && req->priority == CACHE_REQ_PRIORITY_INTERACTIVE && gArtInteractiveActiveCount > 0)
            gArtInteractiveActiveCount--;
        cacheFinalizeRequestLocked(req);
        cacheUnlock();
        cacheFreeRequest(req);
        return;
    }

    if (req->abortRequested) {
        cacheCompleteRequest(req, ERR_LOAD_ABORTED);
        return;
    }

    threadId = GetThreadId();
    loadPriority = cacheGetLoadThreadPriority(req);
    memset(&status, 0, sizeof(status));
    /* EE ReferThreadStatus() returns the thread status (>= 0) or a negative error, not 0-on-success
     * -- see cacheLowerCallerPriority(). The old `== 0` guard was always false, so the art worker
     * was never re-prioritized to its per-mode load priority (0x5A for MMCE / 0x38 for APP) and
     * stayed at the default 0x40 -- i.e. the whole MMCE read deprioritization was inert. */
    if (ReferThreadStatus(threadId, &status) >= 0) {
        originalPriority = status.current_priority;
        if (originalPriority != loadPriority)
            ChangeThreadPriority(threadId, loadPriority);
    }

    texSetLoadAbortFlag(&req->abortRequested);
    // Filename remains the VCD identity. On a total filename miss, retry the same ART/art.tar + loose
    // ART path once with a strict leading PS1 ID; no POPS/ART or suffixless POPS fallback is restored.
    result = cacheLoadImageKey(req, req->value);
    if (result == ERR_BAD_FILE && req->vcdFallback) {
        char fallbackKey[VCD_ID_MAX];
        if (vcdExtractGameId(req->value, fallbackKey, sizeof(fallbackKey)))
            result = cacheLoadImageKey(req, fallbackKey);
    }
    texSetLoadAbortFlag(NULL);
    cacheCompleteRequest(req, result);
    cacheProcessCleanupRequests();

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

        cacheProcessCleanupRequests();

        while (!gArtTerminate) {
            load_image_request_t *req = cacheDequeueRequest();

            if (req == NULL)
                break;

            cacheLoadImage(req);
            cacheProcessCleanupRequests();
        }
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
    gArtShutdownAbandoned = 0;
    gArtQueuedCount = 0;
    gArtActiveCount = 0;
    gArtInteractiveActiveCount = 0;
    gCacheGeneration = 1;
    gCacheFailEpoch = 1;
    gArtInteractiveReqList = NULL;
    gArtInteractiveReqEnd = NULL;
    gArtPrefetchReqList = NULL;
    gArtPrefetchReqEnd = NULL;

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

    gArtRunning = 1;
    StartThread(gArtThreadId, NULL);
}

void cacheEnd(int forceStop)
{
    int waitTicks = forceStop ? CACHE_END_WAIT_TICKS_FORCE : CACHE_END_WAIT_TICKS_SOFT;
    int savedPriority;

    if (!gArtRunning)
        return;

    cacheLock();
    cacheInvalidatePendingRequestsLocked(0);
    cacheUnlock();

    (void)cacheWaitForAllRequestsTimed(waitTicks);

    gArtTerminate = 1;
    WakeupThread(gArtThreadId);

    savedPriority = cacheLowerCallerPriority();
    for (int i = 0; gArtRunning && i < waitTicks; i++)
        cacheWaitForWorkerTick();
    cacheRestoreCallerPriority(savedPriority);

    if (gArtRunning && gArtThreadId >= 0 && forceStop) {
        gDiag.artTerminate++; // #120 diag: THE smoking gun -- this corrupts the shared mmceman RPC channel
        TerminateThread(gArtThreadId);
        DeleteThread(gArtThreadId);
        gArtRunning = 0;
    }

    if (gArtRunning) {
        gArtShutdownAbandoned = 1;
        return;
    }

    /* Run the final cleanup under the real lock, THEN delete the semaphore.
     * Deleting it first makes cacheLock()/cacheUnlock() no-ops, leaving this
     * "Locked" cleanup running unlocked (safe today only because the worker
     * thread has already exited here). */
    cacheLock();
    if (gArtCurrentReq != NULL) {
        load_image_request_t *req = gArtCurrentReq;
        gArtCurrentReq = NULL;
        /* If the art thread was TerminateThread'd while holding this request,
         * its cache entry is still in CACHE_ENTRY_LOADING with entry->qr
         * pointing to req (about to be freed).  Clear it now so the slot is
         * returned to the LRU pool and the dangling pointer can't cause
         * use-after-free in future cacheInvalidateEntryLocked calls. */
        if (req->entry != NULL && req->entry->qr == req)
            cacheClearItem(req->entry, 0);
        cacheFinalizeRequestLocked(req);
        cacheFreeRequest(req);
    }
    cacheResetRequestTrackingLocked();
    cacheUnlock();

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

    // The identity string belongs to the MAPPING, not the texture: free it on every clear
    // (the memset below then leaves value = NULL for the reused slot).
    free(item->value);

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
    cache->allowPrime = 1;
    cache->content = malloc(count * sizeof(cache_entry_t));
    if (cache->content == NULL) {
        free(cache->prefix);
        free(cache->suffix);
        free(cache);
        return NULL;
    }

    // MUST zero before the cacheClearItem loop: cacheClearItem free()s item->value, and a freshly
    // malloc'd content block is recycled heap -- an uninitialized garbage pointer there would be
    // free()d (heap corruption) on every cache creation.
    memset(cache->content, 0, count * sizeof(cache_entry_t));
    for (int i = 0; i < count; ++i)
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
    cacheWakeWorker();

    if (gArtShutdownAbandoned)
        return;

    cacheWaitForCacheRequests(cache);
    cacheUnregister(cache);

    // Free identity strings left on entries that never went through cacheClearItem -- e.g. a
    // LOADING entry whose completion was skipped by the destroying flag above.
    cacheLock();
    for (int i = 0; i < cache->count; ++i) {
        free(cache->content[i].value);
        cache->content[i].value = NULL;
    }
    cacheUnlock();

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
    cacheInvalidatePendingRequestsLocked(0);
    cacheUnlock();

    cacheWakeWorker();
    return cacheWaitForAllRequestsTimed(timeoutTicks);
}

int cacheAbortMmceImageLoadsTimed(int timeoutTicks)
{
    int savedPriority;

    cacheLock();

    if (gArtCurrentReq != NULL && cacheIsAbortableMmceRequest(gArtCurrentReq))
        gArtCurrentReq->abortRequested = 1;

    for (load_image_request_t *req = gArtInteractiveReqList, *next; req != NULL; req = next) {
        next = req->next;
        if (req->effectiveMode == MMCE_MODE)
            cacheDropQueuedRequestLocked(req);
    }
    // MMCE art can sit at PREFETCH priority too -- every quiesce caller (launch, theme
    // swap, deferred config IO, deinit) needs those off the SIO2 bus just the same.
    for (load_image_request_t *req = gArtPrefetchReqList, *next; req != NULL; req = next) {
        next = req->next;
        if (req->effectiveMode == MMCE_MODE)
            cacheDropQueuedRequestLocked(req);
    }

    cacheUnlock();

    cacheWakeWorker();

    savedPriority = cacheLowerCallerPriority();

    while (timeoutTicks != 0) {
        int pending;

        cacheLock();
        pending = cacheHasActiveModeLocked(MMCE_MODE) || cacheHasQueuedModeLocked(MMCE_MODE);
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

void cacheAdvanceGeneration(void)
{
    cacheLock();
    cacheInvalidatePendingRequestsLocked(1);
    cacheNextGenerationLocked();
    cacheUnlock();

    cacheWakeWorker();
}

// Drop the GENUINE-ABSENCE art memos so cover-less items are probed once more. Call ONLY on a deliberate
// settings/theme apply (from applyConfig) -- never on cacheAdvanceGeneration (that fires on every screen
// switch, which is the churn this epoch exists to ignore) and never on the background rescan poll (that
// re-hammer is what reintroduces the #120 wedge). A cover copied onto the card mid-session appears after
// the next settings apply or reboot; a real device change already rebuilds the lists (fresh caches +
// re-zeroed per-item cacheIds), so this is belt-and-braces for the settings/theme case.
void cacheInvalidateFailMemo(void)
{
    cacheLock();
    // Wrap BEFORE overflowing: gCacheFailEpoch++ at INT_MAX is signed-overflow UB, and -O2 may then
    // prove the (<= 0) guard dead and delete it (Gemini review of #154). Practically unreachable (one
    // bump per settings/theme apply), but keep it defined. (gCacheGeneration uses the older <=0 idiom
    // above; left as-is -- pre-existing, out of this change's scope.)
    if (gCacheFailEpoch == 0x7FFFFFFF)
        gCacheFailEpoch = 1;
    else
        gCacheFailEpoch++;
    cacheUnlock();
}

void cacheAdvanceGenerationPreservePrefetch(void)
{
    cacheLock();
    cacheInvalidateInteractiveRequestsLocked();
    cacheNextGenerationLocked();
    cacheUnlock();

    cacheWakeWorker();
}

void cachePrimeReadyTexture(void)
{
    GSTEXTURE *texture = NULL;
    cache_registry_entry_t *registry;

    if (guiInactiveFrames < CACHE_PRIME_IDLE_DELAY)
        return;

    cacheLock();

    if (cacheHasPendingInteractiveArtLocked()) {
        cacheUnlock();
        return;
    }

    registry = gCacheRegistry;
    while (registry != NULL && texture == NULL) {
        image_cache_t *cache = registry->cache;

        if (cache != NULL && !cache->destroying && cache->allowPrime) {
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
    pending = (gArtQueuedCount > 0) || (gArtActiveCount > 0);
    cacheUnlock();

    return pending;
}

int cacheHasPendingInteractiveArt(void)
{
    int pending;

    cacheLock();
    pending = cacheHasPendingInteractiveArtLocked();
    cacheUnlock();

    return pending;
}

void cacheWakeInteractiveArtOnInputIdle(void)
{
    int wakeWorker = 0;

    cacheLock();
    // Prefetch heads can be nav-parked too (prefetch reads defer at dequeue on the slow
    // backends), so wake the worker for parked work on EITHER queue once input goes idle.
    if (gArtRunning && gArtThreadId >= 0 && gArtActiveCount == 0 &&
        (gArtInteractiveReqList != NULL || gArtPrefetchReqList != NULL))
        wakeWorker = 1;
    cacheUnlock();

    if (wakeWorker)
        WakeupThread(gArtThreadId);
}

GSTEXTURE *cacheGetTextureIfReady(image_cache_t *cache, int *cacheId, int *UID)
{
    cache_entry_t *entry;
    GSTEXTURE *result = NULL;

    if (cache == NULL || cache->destroying || cacheId == NULL || UID == NULL || *cacheId < 0 || *cacheId >= cache->count)
        return NULL;

    cacheLock();

    entry = &cache->content[*cacheId];
    if (entry->UID == *UID) {
        switch (entry->state) {
            case CACHE_ENTRY_READY:
                entry->state = CACHE_ENTRY_DISPLAYABLE;
                entry->lastUsed = guiFrameId;
                result = &entry->texture;
                break;
            case CACHE_ENTRY_PRIMED:
                if (entry->primeFrame != guiFrameId) {
                    entry->state = CACHE_ENTRY_DISPLAYABLE;
                    entry->lastUsed = guiFrameId;
                    result = &entry->texture;
                }
                break;
            case CACHE_ENTRY_DISPLAYABLE:
                entry->lastUsed = guiFrameId;
                result = &entry->texture;
                break;
            default:
                break;
        }
    } else {
        *cacheId = -1;
        *UID = -1;
    }

    cacheUnlock();

    return result;
}

static GSTEXTURE *cacheGetTextureInternal(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value, unsigned char priority)
{
    cache_entry_t *entry;
    cache_entry_t *oldestEntry = NULL;
    load_image_request_t *req;
    GSTEXTURE *result = NULL;
    int effectiveMode;
    int oldestEntryId = -1;
    int loadingEntryId = -1;
    int rtime = guiFrameId;
    int wakeWorker = 0;

    if (cache == NULL || cache->destroying || value == NULL || value[0] == '\0')
        return NULL;

    cacheLock();
    effectiveMode = cacheGetEffectiveMode(list, value);

    // Failure memos. -2 = a GENUINE ABSENCE, keyed on gCacheFailEpoch: it survives the per-screen-switch
    // generation bump, so the info page no longer re-runs the same failing opens on every visit (#154).
    // -3 = a TRANSIENT failure, keyed on gCacheGeneration: re-probed on the next generation in case the
    // bus recovered. A mismatch (epoch/generation advanced) drops the memo so the load is re-attempted.
    if (*cacheId == -2) {
        if (*UID == gCacheFailEpoch) {
            cacheUnlock();
            return NULL;
        }

        *cacheId = -1;
        *UID = -1;
    } else if (*cacheId == -3) {
        if (*UID == gCacheGeneration) {
            /* #296: the generation only advances on cursor move/screen switch, so a
             * transient failure memo written in the boot window (art worker racing the
             * io worker for the bus) NEVER heals while the console sits parked -- that
             * game's art stays dead until the user scrolls, the exact "assets load only
             * when highlighted" report. On fast devices, drop the memo once idle and
             * re-probe: a contended-bus EIO heals within ~8 idle frames, while the memo
             * still suppresses hammering DURING navigation (guiInactiveFrames is low)
             * and MMCE keeps the generation-keyed behavior (failing opens are expensive
             * on SIO2; the memo is load-bearing there). */
            if (effectiveMode == MMCE_MODE || guiInactiveFrames < MENU_MIN_INACTIVE_FRAMES) {
                cacheUnlock();
                return NULL;
            }
        }

        *cacheId = -1;
        *UID = -1;
    }

    if (*cacheId != -1) {
        // Neighbor-covers fix: the instant-return used to trust the per-item (cacheId, UID) pair
        // absolutely -- no bounds check (cacheGetTextureIfReady has one; this path did not) and no
        // re-validation that the entry still holds THIS item's art. A pair poisoned by a racing
        // per-item-array rebuild then validly matched ANOTHER title's entry and, because every
        // instant-return refreshes lastUsed (exempting the entry from LRU), the wrong cover stuck
        // PERMANENTLY. Validating the entry's identity string makes any stale/poisoned pair heal in
        // one frame: mismatch -> *cacheId = -1 -> the normal by-value dedupe/enqueue re-associates.
        if (*cacheId >= 0 && *cacheId < cache->count &&
            (entry = &cache->content[*cacheId])->UID == *UID &&
            entry->value != NULL && strcmp(entry->value, value) == 0) {
            switch (entry->state) {
                case CACHE_ENTRY_QUEUED:
                    if (priority == CACHE_REQ_PRIORITY_INTERACTIVE && entry->qr != NULL) {
                        cachePromoteQueuedRequestLocked((load_image_request_t *)entry->qr);
                        /* Same yield as the by-value path below: the promoted cover must not
                         * queue behind an unrelated in-flight prefetch read. */
                        cacheYieldInFlightPrefetchLocked();
                    }
                    cacheUnlock();
                    return NULL;
                case CACHE_ENTRY_LOADING:
                    cacheUnlock();
                    return NULL;
                case CACHE_ENTRY_READY:
                    entry->state = CACHE_ENTRY_DISPLAYABLE;
                    entry->lastUsed = guiFrameId;
                    result = &entry->texture;
                    cacheUnlock();
                    return result;
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
                    // Memo this failure so it is not reopened every frame. A genuine absence is keyed on
                    // gCacheFailEpoch (long-lived: no re-probe until settings/theme apply); a transient on
                    // gCacheGeneration (re-probed next generation, in case the bus recovered).
                    if (entry->failAbsent) {
                        *cacheId = -2;
                        *UID = gCacheFailEpoch;
                    } else {
                        *cacheId = -3;
                        *UID = gCacheGeneration;
                    }
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

    if (priority == CACHE_REQ_PRIORITY_INTERACTIVE) {
        /* The MMCE interactive-art debounce that used to sit here keyed its whole state on a
         * single process-wide (value, until-frame) pair, so ANY frame that drew more than one
         * distinct MMCE value (a coverflow carousel draws ~10 covers/frame; an attribute-art
         * theme draws the cover plus #System/#Media/#DiscType) had every value overwrite the
         * global left by the previous draw, re-arm the window, and return "defer" -- so no MMCE
         * cover was ever enqueued and art never loaded on those themes. It was also redundant:
         * the interactive settle is already enforced below by guiInactiveFrames < delay (4-12
         * MMCE frames) and, during an active scroll, by cacheShouldDeferInteractiveArtOnInputMode.
         * The 2-frame debounce window was strictly shorter than that settle, so its only
         * distinct effect was to block a cover that scrolled into view while the user was
         * already settled -- exactly when it should load. Removed; the two gates below are the
         * complete throttle. */
        if (cacheShouldDeferInteractiveArtOnInputMode(effectiveMode)) {
            cacheUnlock();
            return NULL;
        }

        int delay = cacheGetInteractiveDelay(list, effectiveMode);
        /* In MMCE mode, give Cover art a 1-frame inactivity head start over
         * other art types (Background, Screenshot, etc.).  The default theme
         * draws Background (main0) before Cover (main5) every frame, so
         * without this adjustment BG would always queue first and block COV
         * with a potentially slow open() failure on cards that lack BG art.
         * A single extra inactivity frame guarantees COV queues on frame N
         * while BG/SCR are deferred to frame N+1, by which time COV is
         * already in-flight and the MMCE one-at-a-time throttle keeps the
         * others waiting naturally. */
        if (effectiveMode == MMCE_MODE && cache->suffix != NULL &&
            strcmp(cache->suffix, "COV") != 0)
            delay++;
        if (guiInactiveFrames < delay) {
            cacheUnlock();
            return NULL;
        }
    } else {
        // MMCE is prefetch-exempt (browse-time prefetch hurt nav on the SIO2 bus).
        if (list == NULL || effectiveMode == MMCE_MODE || guiInactiveFrames < cacheGetPrefetchDelay(list, effectiveMode)) {
            cacheUnlock();
            return NULL;
        }

        // #297 follow-up: key the APP case off the RESOLVED mode too -- effectiveMode == APP_MODE
        // is the app-sourced FAV favourite with no art device (cacheGetEffectiveMode leaves
        // APP_MODE when appGetArtMode < 0); it must yield to pending interactive art exactly like
        // the same app on the apps page. list is guaranteed non-NULL by the guard above.
        if ((list->mode == APP_MODE || effectiveMode == APP_MODE) && cacheHasPendingInteractiveArtLocked()) {
            cacheUnlock();
            return NULL;
        }
    }

    req = cacheFindQueuedRequestLocked(cache, value);
    if (req != NULL && req->entry != NULL) {
        if (priority == CACHE_REQ_PRIORITY_INTERACTIVE) {
            cachePromoteQueuedRequestLocked(req);
            cacheYieldInFlightPrefetchLocked();
            if (!cacheShouldDeferInteractiveArtOnInputMode(effectiveMode))
                wakeWorker = 1;
        }

        *cacheId = req->entry - cache->content;
        *UID = req->entry->UID;
        cacheUnlock();
        if (wakeWorker)
            cacheWakeWorker();
        return NULL;
    }

    entry = cacheFindLoadingEntryLocked(cache, value, &loadingEntryId);
    if (entry != NULL) {
        *cacheId = loadingEntryId;
        *UID = entry->UID;
        cacheUnlock();
        return NULL;
    }

    // (Removed a MMCE-only per-draw sibling-drop + one-at-a-time enqueue gate here.) drawCoverFlow
    // requests all 3-5 visible covers each frame; that gate dropped every OTHER queued MMCE cover on
    // each cover's draw, so only one survived per frame and the carousel filled one slow SIO2 read at
    // a time -- issue #116's "covers only appear after a while". It bought no bus-contention benefit:
    // the single art worker already serializes every SIO2 read, and queued-but-not-running requests
    // issue zero concurrent reads. wOPL enqueues the whole visible set and drains it FIFO on one
    // worker; match that. The queue is still bounded by the 10-slot LRU cache (a full cache yields no
    // free victim, so no unbounded growth).

    if (priority == CACHE_REQ_PRIORITY_PREFETCH && cache->queuedPrefetchRequests >= cacheGetPrefetchLimit(cache)) {
        cacheUnlock();
        return NULL;
    }

    // (Removed a MMCE-COV-only first pass that preferred evicting an already-LOADED entry over a FREE
    // slot -- with 10 slots and <=5 visible covers it reverted a just-loaded neighbour to the
    // placeholder = flicker (issue #116). Always use the normal FREE-inclusive LRU below, like wOPL.)
    if (oldestEntry == NULL) {
        rtime = guiFrameId;
        for (int i = 0; i < cache->count; i++) {
            entry = &cache->content[i];
            if ((entry->state == CACHE_ENTRY_FREE || entry->state == CACHE_ENTRY_READY || entry->state == CACHE_ENTRY_PRIMED ||
                 entry->state == CACHE_ENTRY_DISPLAYABLE || entry->state == CACHE_ENTRY_FAILED) &&
                entry->lastUsed < rtime) {
                oldestEntry = entry;
                oldestEntryId = i;
                rtime = entry->lastUsed;
            }
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
    req->effectiveMode = effectiveMode;
    // Key the strict-PS1-id retry on the REQUESTING list's view, not the resolved backend mode:
    // the FAV->owner resolution in cacheGetEffectiveMode would otherwise read the OWNER page's
    // independent L3 view (default ISO under GAME_VIEW_BOTH) and silently disarm the retry for a
    // VCD favourite the FAV tab is displaying right now -- the FAV tab only lists VCD favourites
    // while ITS OWN view is VCD, so the requesting-list stamp is 1 exactly when it should be.
    // Behavior-identical for device lists (list->mode == the pre-resolution mode) and for APP
    // lists (vcdModeSupported(APP_MODE) == 0, and an app value never yields a strict-ID retry).
    req->vcdFallback = (list != NULL) ? vcdViewActive(list->mode) : 0;
    req->priority = priority;
    req->generation = gCacheGeneration;
    req->failEpoch = gCacheFailEpoch; // pin the absence epoch to when the load STARTED, not when it finishes
    req->value = (char *)req + sizeof(load_image_request_t);
    strcpy(req->value, value);
    req->cacheUID = cache->nextUID;

    /* Release the displaced texture using the SAME GSTEXTURE pointer
     * that gsKit_TexManager_bind registered (&oldestEntry->texture).
     * Earlier variants of this code copied the GSTEXTURE struct into
     * either a stack local or a req->displacedTexture heap field and
     * called gsKit_TexManager_free on that copy.  PS2SDK keys its LL
     * lookup by GSTEXTURE pointer identity, so neither address ever
     * matched the registered entry: VRAM was silently leaked while
     * EE RAM was correctly freed.  Across many APPS/MMCE cover
     * navigations the leaks accumulated until gsKit's internal LRU
     * evicted other live textures (e.g. the theme background) to make
     * room, producing the purple/vertical artifacting reported on
     * APPS and MMCE pages.  cacheClearItem() is the established
     * release pattern used by every other site in this file. */
    cacheClearItem(oldestEntry, 1);
    // Record the entry's identity for the instant-return validation (neighbor-covers fix). Own
    // heap copy -- req->value lives inline in the request, which is freed independently. On OOM
    // FAIL the enqueue: an identity-less entry that ever reached READY would mismatch the gate
    // every frame and re-enqueue forever.
    oldestEntry->value = malloc(strlen(value) + 1);
    if (oldestEntry->value == NULL) {
        cacheFreeRequest(req); // the encapsulated release (equivalent to free(req) here: the fresh request owns no texture memory yet)
        cacheUnlock();
        return NULL;
    }
    strcpy(oldestEntry->value, value);
    oldestEntry->qr = req;
    oldestEntry->state = CACHE_ENTRY_QUEUED;
    oldestEntry->UID = cache->nextUID;

    *cacheId = oldestEntryId;
    *UID = cache->nextUID++;

    cache->activeRequests++;
    cacheEnqueueRequestLocked(req);
    if (req->priority == CACHE_REQ_PRIORITY_INTERACTIVE)
        cacheYieldInFlightPrefetchLocked();
    cacheUnlock();

    cacheWakeWorker();

    return NULL;
}

GSTEXTURE *cacheGetTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value)
{
    return cacheGetTextureInternal(cache, list, cacheId, UID, value, CACHE_REQ_PRIORITY_INTERACTIVE);
}

GSTEXTURE *cachePrefetchTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value)
{
    return cacheGetTextureInternal(cache, list, cacheId, UID, value, CACHE_REQ_PRIORITY_PREFETCH);
}
