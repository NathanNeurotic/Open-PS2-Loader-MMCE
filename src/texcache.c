#include "include/opl.h"
#include "include/texcache.h"
#include "include/textures.h"
#include "include/ioman.h"
#include "include/gui.h"
#include "include/util.h"
#include "include/renderman.h"
#include "include/tar.h"
#include "include/pad.h"        // getKeyPressed -- the SIO2 nav gate, GUI thread only
#include "include/bdmsupport.h" // bdmModeIsSIO2 -- is this cover on the pad's bus?
#include "include/appsupport.h" // appGetArtMode -- APP rows are proxies for another device
#include "include/favsupport.h" // favGetArtMode -- so are FAV rows
#include <kernel.h>
#include <delaythread.h> // DelayThread -- must be included explicitly, it is not pulled in by kernel.h

extern void *_gp;

// Tagged, not anonymous, because the record now carries its OWN queue link: art no longer rides an
// ioman request node, it rides this one.
typedef struct load_image_request
{
    struct load_image_request *next; // art FIFO link, owned by whoever owns the request
    image_cache_t *cache;
    cache_entry_t *entry;
    item_list_t *list;
    // only for comparison if the deferred action is still valid
    int cacheUID;
    // Art epoch this request was queued in; a mismatch means the whole VIEW it belonged to is gone.
    // See cacheDropQueuedArt().
    unsigned int epoch;
    // Chunk-level abort, read by textures.c via texSetLoadAbortFlag. Lets a cancel interrupt a load
    // that is ALREADY RUNNING, which neither the stale-stamp test nor the epoch test can do -- both
    // only ever caught a request still sitting in the queue.
    //
    // ⚠ It cannot shorten an MX4SIO cover. texShouldUseMemoryReader sends every "mass*" path down
    // the whole-file staging fast path: one lseek and one read() of the entire PNG, a single
    // un-interruptible IOP RPC. The flag is only checked in the CHUNKED staging loop and inside the
    // streaming PNG reader. For the device #340 is actually about, NOT ISSUING the read is the only
    // lever -- which is what the SIO2 defer in artPop is for.
    volatile int abortRequested;
    // Resolved on the GUI THREAD at enqueue: does this cover ride SIO2, the controller's own bus?
    // Not resolvable on the worker -- answering it reads the support's private device data, which a
    // background rescan rewrites underneath. One byte, copied in, is immune.
    unsigned char sio2;
    char *value;
} load_image_request_t;

// Art thread priority. LOWER NUMBER WINS on the EE. GUI/pad thread is 31 (opl.c), the shared io
// worker 32 (ioman.c), sound 45. 64 puts art below every one of them: it can never take the CPU
// from input, from a config save, from a list rebuild, or from audio. Same value the fork uses.
#define ART_THREAD_PRIORITY 64

// ...and lower still while reading anything on SIO2. EE priority does not speed up or slow down a
// thread blocked in an IOP RPC, so this buys nothing on the device read itself -- what it protects
// is the PNG DECODE that follows, which is tens of ms of pure EE work on the pad's own bus.
#define ART_SIO2_LOAD_PRIORITY 90

// Matches ioman's worker stack. The identical libpng decode runs on that stack today, so this is a
// measured size rather than a guess. .bss only; it adds nothing to the ELF.
#define ART_THREAD_STACK_SIZE (96 * 1024)

// Teardown join budgets in MILLISECONDS -- the join sleeps via DelayThread(1000) = 1 ms, NOT via
// util.c's delay(), whose "tick" is a ~0.25 s NOP spin (the units trap that made the old
// LAUNCH_IO_DRAIN_TICKS "10 s" actually forty minutes).
#define ART_JOIN_MS_LAUNCH      500
#define ART_JOIN_MS_TERMINAL    1500
#define ART_FOREGROUND_DRAIN_MS 500

static int gArtThreadId = -1;
static ee_thread_t gArtThread;
static u8 gArtStack[ART_THREAD_STACK_SIZE] __attribute__((aligned(16)));

static volatile int gArtRunning = 0;
static volatile int gArtTerminate = 0;
static volatile int gArtShutdownAbandoned = 0;
static volatile int gArtNavActive = 0;

static load_image_request_t *gArtReqList = NULL;
static load_image_request_t *gArtReqEnd = NULL;
static load_image_request_t *volatile gArtCurrentReq = NULL;

static void cacheWakeArtWorker(void);

// .tar art packs (checklist item 45). Tries "<value>_<suffix>.png" inside ART/art.tar before the
// normal per-file lookup. Opt-in: gEnableArtTar ships 0, so with the toggle off this is a single
// predictable branch and the art path is byte-for-byte the behaviour hardware has already validated.
//
// #340 note -- what this actually costs, stated in full because item 45 is a flagged suspect:
//   WHERE IT RUNS: cacheLoadImage is an ioman handler (ioRegisterHandler/ioPutRequest below), so all
//   of this executes on the IO worker thread at priority 30 -- never on the GUI/pad thread. The
//   caller also gates the enqueue behind the art idle delay (guiInactiveFrames < list->delay in
//   cacheGetTexture), so nothing here is even queued while the user is holding a direction.
//   ONE-SHOT COSTS on the first request after the toggle goes on:
//     - tarFind() lazily probes up to 13 devices (mass0-7, mmce0/1, hdd0:, pfs0:, smb0:) with one
//       open() each; mmce* and smb0: are the slow ones. tar.c's s_inactive[] latch stops a failed
//       probe from repeating.
//     - a successful parse then WRITES a sidecar index (art_cache.bin, ~64 bytes per entry) next to
//       the archive. That is a genuine write on the art path, unlike official's read-only one. It is
//       non-fatal (tarParseFile ignores the result) and happens only when the archive changes, but it
//       is the thing to instrument first if a .tar-enabled build ever feels worse than a plain one.
//   DEFAULT PATH: gEnableArtTar ships 0, so none of the above is reachable unless the user opts in.
static int artTarLoadImage(const char *value, const char *suffix, GSTEXTURE *texture)
{
    char prefix[64];
    TarEntryBase *entry = NULL;
    void *buffer;
    int result;

    if (snprintf(prefix, sizeof(prefix), "%s_%s.", value, suffix) >= (int)sizeof(prefix))
        return -1;

    entry = tarFindPrefix(TAR_KIND_ART, prefix);

    if (entry == NULL) {
        LOG("ART TAR: '%s_%s' not in the archive index\n", value, suffix);
        return -1;
    }

    buffer = malloc(entry->rawSize);
    if (buffer == NULL) {
        LOG("ART TAR: out of memory for '%s' (%u bytes)\n", prefix, entry->rawSize);
        return -1;
    }

    if (tarRead(TAR_KIND_ART, entry, buffer, entry->rawSize) != entry->rawSize) {
        LOG("ART TAR: short read for '%s' (%u bytes expected)\n", prefix, entry->rawSize);
        free(buffer);
        return -1;
    }

    result = texLoadFromMemory(texture, buffer, entry->rawSize);
    if (result < 0)
        LOG("ART TAR: '%s' found (%u bytes) but PNG decode failed (%d)\n", prefix, entry->rawSize, result);
    free(buffer);
    return result;
}



// NO FAIL MEMO. uOPL and rebuild-66 have none: a load that fails parks its ROW (lastUsed = 0 ->
// cacheId -2 in cacheGetTextureEx) and is not asked again until a list rebuild, which is the same
// protection one layer down and without a hash table to get wrong. Ours added a retry budget on top
// to soften a mis-detected absence; absence is detected correctly now (rebuild-108), and the retry
// lane was how the IO worker got pinned (rebuild-103/105). Subtracted.
void cacheInvalidateFailMemo(void)
{
    // Kept as a no-op so the callers that mark "new art may have appeared" (applyConfig, every list
    // rebuild) need no change: rows re-ask naturally, because a rebuild gives every item a fresh
    // cache_id anyway.
}

// Minimum frames of no input before any art may be requested, regardless of the Art Delay
// setting. See the gate in cacheGetTexture for why this floor exists.
#define ART_MIN_SETTLE_FRAMES 3

// Frames a queued art request may go undrawn before the worker abandons it. Generous enough that a
// brief hitch does not throw away a wanted load, short enough that a scroll's worth of superseded
// covers never reaches the device.
#define ART_CANCEL_STALE_FRAMES 20

// Art requests currently sitting in the ioman queue. LOCK-FREE on purpose, and that is the whole
// story of this counter: the first backpressure gate here called ioGetPendingRequestCount(), whose
// WaitSema(gProcSemaId) blocks until the worker finishes draining the ENTIRE queue -- the worker
// holds that sema across the whole BATCH, not per request. Called from cacheGetTexture on the GUI
// thread, that froze the menu for as long as any art was in flight: the hardware 'everything
// turns to chit' / black-screen-after-save reports. A plain volatile int the producer increments
// and the handler decrements needs no lock; a lost update skews the gate by one for a frame, and
// the gate self-heals by resetting when the queue is observed empty.
static volatile int gArtQueuedCount = 0;

// The load the worker is executing RIGHT NOW, which gArtQueuedCount can no longer see: the counter
// above is decremented on entry to cacheLoadImage (it balances the enqueue, once per request), so
// between that decrement and the end of the decode "queued" reads zero while the device is very
// much busy. Single writer (the IO worker), read by the GUI thread in cacheHasPendingArt.
static volatile int gArtActiveCount = 0;

// Diagnostics only (drawn by the debug HUD in gui.c). gArtRefused counts requests the backpressure
// gate turned away, gArtDone counts loads that finished -- together they say whether the pipeline is
// moving or wedged, which is not something a still image of the menu can tell you.
static volatile int gArtRefused = 0;
// Last art load's wall-clock cost in ms, and the last SUCCESSFUL one with its decoded dimensions.
// Written by the IO worker, read by the GUI thread for the debug HUD -- plain ints, a torn read
// mis-prints one frame and nothing else depends on them.
static volatile int gArtLastMs = 0;
static volatile int gArtLastOkMs = 0;
static volatile int gArtLastWidth = 0;
static volatile int gArtLastHeight = 0;
static volatile int gArtDone = 0;
// Requests released WITHOUT loading -- cancelled, superseded, or dropped at teardown. The
// complement of gArtDone: together they must account for every request artPop ever handed out, so a
// climbing total with both of these flat is the signature of a request leak.
static volatile int gArtDropped = 0;

void cacheDebugCounters(int *queued, int *active, int *refused, int *done)
{
    if (queued)
        *queued = gArtQueuedCount;
    if (active)
        *active = gArtActiveCount;
    if (refused)
        *refused = gArtRefused;
    if (done)
        *done = gArtDone;
}

// Requests released WITHOUT loading. The complement of gArtDone: every request artPop hands out ends
// in exactly one of the two, so a climbing enqueue total with BOTH of these flat means a leak.
int cacheDebugDropped(void)
{
    return gArtDropped;
}

void cacheDebugLastLoad(int *lastMs, int *lastOkMs, int *width, int *height)
{
    if (lastMs)
        *lastMs = gArtLastMs;
    if (lastOkMs)
        *lastOkMs = gArtLastOkMs;
    if (width)
        *width = gArtLastWidth;
    if (height)
        *height = gArtLastHeight;
}

// Is any cover art queued or being read/decoded? Used by menuUpdateHook to keep background device
// rescans OUT of the shared IO queue while art is still arriving -- without it, a settle enqueues a
// batch of per-device probes that the worker serializes AHEAD of the covers the user is waiting on,
// so the art keeps falling further behind on every scroll-and-stop cycle. Lock-free by the same
// argument as gArtQueuedCount: a torn read mis-answers one frame and self-corrects on the next.
int cacheHasPendingArt(void)
{
    return (gArtQueuedCount > 0) || (gArtActiveCount > 0);
}


// LOOKUP ONLY: hand back this item's texture if its slot is still live, and never claim a slot,
// never queue a load, never touch the fail memo. For a caller that wants to keep DRAWING art it
// already has while deliberately not asking for more -- without this, "don't request" also means
// "don't draw", and a row that already had its thumbnail would drop back to the placeholder.
GSTEXTURE *cacheLookupTexture(image_cache_t *cache, int *cacheId, int *UID)
{
    if (cache == NULL || cacheId == NULL || UID == NULL)
        return NULL;

    if (*cacheId < 0 || *cacheId >= cache->count)
        return NULL;

    cache_entry_t *entry = &cache->content[*cacheId];
    if (entry->UID != *UID || entry->qr != NULL || entry->lastUsed == 0 || entry->texture.Mem == NULL)
        return NULL;

    entry->lastUsed = guiFrameId; // still on screen: keep it away from the eviction scan
    return &entry->texture;
}

static void cacheClearItem(cache_entry_t *item, int freeTxt); // released below on a transient failure

// Set once, at teardown, by cacheShutdownArtLoads(). See texcache.h for the full reasoning -- and in
// particular why this is NOT wired to cacheCancelPendingImageLoads(), which themes.c calls on the
// first theme load of every boot.
static int gCacheLoadsCancelled = 0;

// Art epoch. Bumped when the ENTIRE view a queue of covers belongs to is discarded, which the
// per-row cancellation below cannot detect on its own.
//
// That cancellation drops a request whose row has scrolled away, keyed on entry->lastUsed going
// stale. On an L3 ISO<->VCD toggle nothing scrolls: the old rows keep being drawn (which is why the
// previous view's titles sit there looking stale) so every stamp keeps refreshing, and not one of
// those now-pointless covers is cancelled. They are FIFO ahead of the IO_MENU_UPDATE_DEFFERED that
// rebuilds the list, and art shares the single ioman worker with that rebuild -- so the page cannot
// become correct until every cover for the view being thrown away has been read off the device
// first. That is the whole "slow to be correct" symptom, and it is why the fork feels faster here:
// its art runs on a dedicated thread and never sits in front of the rebuild.
static unsigned int gArtEpoch = 0;

void cacheDropQueuedArt(void)
{
    gArtEpoch++;
    // Let the worker burn the now-dead requests immediately instead of at the next enqueue. Its job
    // narrowed once art got its own thread -- covers no longer sit FIFO in front of the list rebuild
    // at all, that half is structural now -- but it still stops the DEVICE READS for a view that no
    // longer exists.
    cacheWakeArtWorker();
}

// ---------------------------------------------------------------------------------------------
// ART QUEUE. A private intrusive FIFO, guarded by a DIntr()/EIntr() bracket rather than a
// semaphore.
//
// Why not a semaphore, given the fork uses one: the fork's GUI thread takes gArtSemaId on EVERY
// cacheGetTexture call -- including plain cache hits -- and once more per frame in its pump, while
// the worker can be holding that same semaphore having raised ITSELF to priority 90 for a slow
// SIO2 read. That is an unbounded priority inversion on the render thread: the thread that reads
// your controller blocks on a lock held by the lowest-priority thread in the system, for as long as
// a cover takes. This codebase has already paid for one freeze of exactly that shape (the
// GUI-thread-must-never-take-an-ioman-semaphore rule).
//
// The bracket has none of that. Every critical section below is a few pointer stores with no call
// out to anything, so interrupts are off for a handful of instructions and no thread can ever block
// another. The expensive parts -- the device read, the PNG decode, and every free() -- happen with
// interrupts ENABLED, outside any section.
//
// RULE FOR CALLERS: never call these with interrupts already disabled; EIntr() enables
// unconditionally and would re-enable early.
// ---------------------------------------------------------------------------------------------

static void cacheWakeArtWorker(void)
{
    if (gArtRunning && gArtThreadId >= 0)
        WakeupThread(gArtThreadId);
}

static void artPush(load_image_request_t *req)
{
    req->next = NULL;

    DIntr();
    if (gArtReqEnd)
        gArtReqEnd->next = req;
    else
        gArtReqList = req;
    gArtReqEnd = req;
    gArtQueuedCount++;
    EIntr();
}

// Called ONLY by the worker.
static load_image_request_t *artPop(void)
{
    load_image_request_t *req;

    DIntr();
    req = gArtReqList;
    if (req) {
        // SIO2 DEFER. The head of the queue is a cover on the controller's own bus and the user is
        // holding a direction RIGHT NOW. Issuing that read is issue #340's exact mechanism: the SD
        // transfer and the pad poll share SIO2, and on hardware that is a swallowed step. The
        // enqueue-side idle gate cannot help here -- it only stops requests being MADE during
        // navigation, and this one was made during a settle the user has since ended.
        //
        // Leave it queued. It costs nothing, and the next idle frame runs it. This is the ONLY
        // lever that works for MX4SIO: the abort flag cannot shorten a cover on that device,
        // because a "mass*" path takes the whole-file staging fast path in textures.c -- one
        // lseek and one read() of the entire PNG, a single un-interruptible IOP RPC. Not issuing
        // the read is the whole defence.
        //
        // Head-of-line blocking is accepted deliberately: a page is almost always one device, and
        // a skip-scan would make this pop O(n) inside the interrupts-off bracket.
        if (req->sio2 && gArtNavActive) {
            EIntr();
            return NULL;
        }

        gArtReqList = req->next;
        if (!gArtReqList)
            gArtReqEnd = NULL;
        req->next = NULL;

        if (gArtQueuedCount > 0)
            gArtQueuedCount--;
        // ACTIVE now means "owned by the worker", not "reading the device" -- a deliberate
        // widening. cacheHasPendingArt() gates background rescans and the per-game background
        // request, and it must stay true across a cancellation too, or a rescan slips in exactly
        // while the art lane is churning.
        gArtActiveCount++;
        gArtCurrentReq = req;
    }
    EIntr();

    return req;
}

// Take the WHOLE queue in one O(1) swap. The caller then walks and frees it with interrupts
// ENABLED -- which is why no canceller ever holds a bracket across a free().
static load_image_request_t *artDetachAll(void)
{
    load_image_request_t *list;

    DIntr();
    list = gArtReqList;
    gArtReqList = NULL;
    gArtReqEnd = NULL;
    gArtQueuedCount = 0;
    EIntr();

    return list;
}

// Free a chain the caller has already detached, so nothing else can reach it. Releases each
// request's cache SLOT so the row still on screen can claim it again next frame: a slot left with
// qr set is invisible to BOTH the read path and the eviction scan for the rest of the session,
// which is a failure this file has now recorded twice.
//
// freeTxt is 0 on purpose -- see the GS/VRAM note in cacheLoadImage.
static void artFreeChain(load_image_request_t *chain)
{
    while (chain) {
        load_image_request_t *next = chain->next;

        if (chain->entry && chain->entry->qr == chain)
            cacheClearItem(chain->entry, 0);
        if (chain->cache && chain->cache->activeRequests > 0)
            chain->cache->activeRequests--;

        gArtDropped++;
        free(chain);
        chain = next;
    }
}

// The worker's single exit path. artPop is the ONLY place that increments gArtActiveCount and
// publishes gArtCurrentReq, so this is the ONLY place that undoes either: one increment, one
// decrement, and no path in between can skip it.
static void artReleaseRequest(load_image_request_t *req, int completed)
{
    DIntr();
    if (gArtCurrentReq == req)
        gArtCurrentReq = NULL;
    if (gArtActiveCount > 0)
        gArtActiveCount--;
    EIntr();

    if (req->cache && req->cache->activeRequests > 0)
        req->cache->activeRequests--;

    if (completed)
        gArtDone++;
    else
        gArtDropped++;

    free(req);
}

void cacheShutdownArtLoads(void)
{
    load_image_request_t *drop;

    gCacheLoadsCancelled = 1;

    // The one request the worker already owns cannot be freed here -- it may be inside a device
    // read. Flag it so textures.c can bail at its next chunk boundary where that is possible.
    if (gArtCurrentReq)
        gArtCurrentReq->abortRequested = 1;

    drop = artDetachAll();
    artFreeChain(drop);

    cacheWakeArtWorker(); // so a sleeping worker leaves SleepThread and observes the latch
}

// Drop everything queued and wait, bounded, for the one in-flight request to let go.
//
// WAS A NO-OP STUB, which quietly disarmed every caller: the theme-switch guard (whose "keep the
// current theme and retry" branch was therefore unreachable -- a real use-after-free, since thmFree
// -> cacheDestroyCache frees content[] while queued requests still point into it), the apps list
// rebuild, the pfs0: remount, and the LWNBD preflight. Arming them is part of the point of giving
// art its own thread: with a worker running concurrently those races stop being theoretical.
//
// waitTicks is MILLISECONDS here -- the wait is DelayThread(1000), not util.c's delay(), whose
// "tick" is a ~0.25 s NOP spin. Every existing call-site constant is sane read that way.
int cacheCancelPendingImageLoadsTimed(int waitTicks)
{
    load_image_request_t *drop;

    if (!gArtRunning)
        return 1;

    if (gArtCurrentReq)
        gArtCurrentReq->abortRequested = 1;

    drop = artDetachAll();
    artFreeChain(drop);
    cacheWakeArtWorker();

    while (waitTicks != 0) {
        if (gArtQueuedCount == 0 && gArtActiveCount == 0)
            return 1;
        // DelayThread, not a spin: it puts THIS thread (GUI at 31, or the io worker at 32) to sleep,
        // which is exactly what lets the priority-64 art thread run at all. No priority donation --
        // the fork demotes its caller to 91 for the wait, and a wedged read then leaves the menu
        // pinned there for the rest of the session.
        DelayThread(1000);
        if (waitTicks > 0)
            waitTicks--;
    }

    return (gArtQueuedCount == 0 && gArtActiveCount == 0);
}

// The same operation restricted to SIO2 requests -- the ones sharing the pad's bus. The NAME is kept
// because five call sites use it and because MMCE rejoins the predicate untouched when checklist
// item 1 lands; today the SIO2 set is MX4SIO alone.
//
// Detach the whole queue, free the SIO2 ones, re-push the rest. Re-pushing lands them at the tail,
// so a filtered drop can reorder art against anything enqueued concurrently. Accepted on purpose:
// FIFO order among covers is a soft property -- the stale-stamp cancellation exists precisely
// because order was never a guarantee -- and the alternative is an O(n) walk with interrupts off.
int cacheAbortMmceImageLoadsTimed(int waitTicks)
{
    load_image_request_t *chain, *keepHead = NULL, *keepTail = NULL;

    if (!gArtRunning)
        return 1;

    if (gArtCurrentReq && gArtCurrentReq->sio2)
        gArtCurrentReq->abortRequested = 1;

    chain = artDetachAll();
    while (chain) {
        load_image_request_t *next = chain->next;

        chain->next = NULL;
        if (chain->sio2) {
            artFreeChain(chain);
        } else {
            if (keepTail)
                keepTail->next = chain;
            else
                keepHead = chain;
            keepTail = chain;
        }
        chain = next;
    }
    while (keepHead) {
        load_image_request_t *next = keepHead->next;
        artPush(keepHead);
        keepHead = next;
    }

    cacheWakeArtWorker();

    while (waitTicks != 0) {
        if (!(gArtCurrentReq && gArtCurrentReq->sio2))
            return 1;
        DelayThread(1000);
        if (waitTicks > 0)
            waitTicks--;
    }

    return !(gArtCurrentReq && gArtCurrentReq->sio2);
}

// Runs on the ART THREAD, never on the GUI thread and no longer on the ioman worker. The request
// arrives already unlinked and already counted ACTIVE by artPop, so every exit below goes through
// artReleaseRequest exactly once.
static void cacheLoadImage(load_image_request_t *req)
{
    // Safeguards...
    if (!req)
        return;

    // TEARDOWN. OPL is on its way out, and this cover is for a menu guiEnd() is about to destroy --
    // reading it would spend a device access and a PNG decode on a texture nobody will ever draw.
    // The queue no longer sits in ioman's list, so ioBlockOpsTimed cannot see it at all;
    // cacheShutdownArtLoads() empties it directly and this latch catches the one request the worker
    // had already taken. That is the whole difference between a launch that hands off immediately
    // and one that first pays for every cover the user scrolled past.
    // Placed after the !req guard so the pointer is known good, before every other check so nothing
    // can slip past it, and before the gArtActiveCount++ below like all the other early-outs.
    //
    // Leaves entry->qr pointing at the freed request, exactly as the UID-mismatch early-out below
    // already does. Checked rather than assumed: qr is only ever ASSIGNED or NULL-TESTED (texcache.c
    // 174, 301, 335, 397, 441, 470) and never dereferenced, so a stale one cannot fault -- at worst
    // it makes a slot read as "still loading" to a GUI that is being torn down in the next breath.
    // Clearing it here is not possible anyway: req->entry is not yet known non-NULL at this point.
    if (gCacheLoadsCancelled) {
        artReleaseRequest(req, 0);
        return;
    }

    // Every early-out below must FREE the request. It is a single allocation (the struct with its
    // value string appended), owned solely by this handler once ioPutRequest accepted it, and the
    // entry's qr back-pointer is cleared by cacheClearItem -- so nothing else can reach it and
    // returning without free() simply loses the memory.
    if (!req->entry || !req->cache) {
        artReleaseRequest(req, 0);
        return;
    }

    item_list_t *handler = req->list;
    if (!handler) {
        artReleaseRequest(req, 0);
        return;
    }

    // The cache entry was already reused (or cleared: cacheClearItem zeroes UID and nulls qr, which
    // is what a list rebuild or theme switch does to every slot). This request is now orphaned --
    // and it is NOT a rare path: background rescans rebuild lists while the user browses, so every
    // rebuild used to leak one of these per art load still in flight.
    if (req->cacheUID != req->entry->UID) {
        artReleaseRequest(req, 0);
        return;
    }

    // CANCELLATION. The queue is FIFO and a scroll enqueues a cover per row it passes, so by the
    // time a request reaches the front the user has usually moved on -- and reading it anyway is
    // both wasted device time and a PNG decode that preempts the priority-31 GUI/pad thread. Every
    // frame a row is drawn while its load is pending, cacheGetTexture refreshes entry->lastUsed
    // (above); a row that has scrolled away stops being drawn and its stamp goes stale. So a stale
    // stamp means nobody is waiting for this image: drop it before it costs anything, and release
    // the slot so the row that IS on screen can claim it.
    //
    // This is what "drop the old and immediately load the new" actually requires. Ordering fixes --
    // running the visible cover next, promoting it in the queue -- only choose which stale read goes
    // first; they cannot stop the stale reads existing.
    if ((u32)(guiFrameId - req->entry->lastUsed) > ART_CANCEL_STALE_FRAMES) {
        // freeTxt 0, not 1: the entry holds NO texture during the in-flight window (the enqueue
        // cleared it on the GUI thread) and rmUnloadTexture must never run off the GUI thread --
        // see the GS/VRAM note at the staged decode below.
        cacheClearItem(req->entry, 0);
        artReleaseRequest(req, 0);
        return;
    }

    // WHOLE-VIEW cancellation, which the stamp test above cannot see. A view toggle discards every
    // row at once WITHOUT any of them scrolling away, so their stamps stay fresh and each pointless
    // cover would still be read. Same release as the stale case -- cacheClearItem drops qr and frees
    // the slot, which is load-bearing: a slot left with qr set is invisible to both the read path and
    // the eviction scan for the rest of the session, so freeing the request alone would quietly kill
    // every cache slot the discarded view was using.
    if (req->epoch != gArtEpoch) {
        cacheClearItem(req->entry, 0); // freeTxt 0: see the GS/VRAM note below
        artReleaseRequest(req, 0);
        return;
    }

    // ---- from here the request owns a device read + PNG decode ----
    //
    // STAGED TEXTURE. The decode lands in a GSTEXTURE this THREAD owns, and only a finished image is
    // copied into the cache entry. Two things fall out of that and both are load-bearing:
    //
    //  1. GS/VRAM IS NEVER TOUCHED OFF THE GUI THREAD. cacheClearItem(item, 1) calls rmUnloadTexture
    //     -> gsKit_TexManager_free, which mutates gsKit's VRAM allocator list while the GUI thread
    //     may be inside gsKit_TexManager_bind on that same list. Today that call is REACHABLE from
    //     the io worker and is saved only by an accident: the enqueue cleared the entry with
    //     freeTxt=1 on the GUI thread first, so texture.Mem is NULL and the branch is skipped.
    //     Staging makes it unreachable BY CONSTRUCTION -- the entry stays all-zeros for the whole
    //     in-flight window, so the worker passes freeTxt=0 everywhere and cannot reach gsKit even if
    //     a later edit gets it wrong.
    //  2. A cancelled load frees its own pixels with plain texFree() and leaves the entry alone.
    //
    // The entry is exclusively ours for this window: while qr is set, cacheGetTexture returns NULL,
    // cacheLookupTexture returns NULL, and the eviction scan skips the slot. Nothing on the GUI
    // thread can read entry->texture until qr clears.
    GSTEXTURE staged;
    memset(&staged, 0, sizeof(staged));
    staged.ClutStorageMode = GS_CLUT_STORAGE_CSM1;

    int result = -1;

    // Time the load itself -- open, read off the device, decode -- with nothing else in the window.
    // This is the number that separates the two remaining explanations for slow art, which no amount
    // of reading the source can settle: if ONE cover costs a few hundred ms then the pipeline is
    // fine and the wait is the number of images we ask for, and if it costs seconds then the cost is
    // inside a single read on that device and the queue was never the story.
    clock_t loadStart = clock();

    // SIO2 PRIORITY BRACKET, around the load ONLY and restored before the publish. A thread still at
    // 90 when it touches anything the GUI also touches is the inversion this design exists to avoid.
    // EE priority neither speeds nor slows a thread blocked in an IOP RPC, so this buys nothing on
    // the device read -- what it protects is the PNG DECODE, tens of ms of pure EE work happening
    // while the pad is being polled over the very same bus.
    int sio2Bracket = req->sio2;
    if (sio2Bracket)
        ChangeThreadPriority(gArtThreadId, ART_SIO2_LOAD_PRIORITY);

    texSetLoadAbortFlag(&req->abortRequested);

    if (gEnableArtTar)
        result = artTarLoadImage(req->value, req->cache->suffix, &staged);

    // Fall through to the per-file lookup whenever the archive is off, absent, or lacks this key.
    if (result < 0)
        result = handler->itemGetImage(handler, req->cache->prefix, req->cache->isPrefixRelative, req->value, req->cache->suffix, &staged, GS_PSM_CT24);

    texSetLoadAbortFlag(NULL);

    if (sio2Bracket)
        ChangeThreadPriority(gArtThreadId, ART_THREAD_PRIORITY);

    // clock() is microseconds here; the elapsed form is single-wrap safe.
    gArtLastMs = (int)((clock() - loadStart) / 1000);
    if (result >= 0) {
        gArtLastOkMs = gArtLastMs;
        gArtLastWidth = (int)staged.Width;
        gArtLastHeight = (int)staged.Height;
    }

    // PUBLISH. The order IS the handoff protocol: fill the entry first, clear qr LAST. qr going NULL
    // is what tells the GUI thread the slot is readable, so every store it must see has to precede
    // that one. Single-core EE, so a context switch is a full sync and ordering the stores is all
    // that is required -- there is no barrier to add.
    //
    // uOPL/rebuild-66 shape on failure: park the row (lastUsed = 0 -> cacheGetTexture moves it to
    // the -2 "asked and answered" state) and do not ask again until a list rebuild. No memo, no
    // retry budget -- both existed to soften a mis-detected absence, and absence is detected
    // correctly now (rebuild-108). Retrying was also how the IO worker got pinned (rebuild-103/105).
    if (result >= 0) {
        req->entry->texture = staged;
        req->entry->lastUsed = guiFrameId;
    } else {
        texFree(&staged); // our own pixels, never the entry's -- no gsKit contact on this thread
        req->entry->lastUsed = 0;
    }
    req->entry->qr = NULL;

    artReleaseRequest(req, 1);
}

// The art worker. Wake, drain, sleep -- the same shape as the io worker and the fork's, and it
// leans on the same EE kernel guarantee both do: a WakeupThread landing while this thread is still
// running accumulates, so the next SleepThread returns immediately. cacheTickArt() is a
// belt-and-braces net on top of that.
static void cacheArtWorkerThread(void *arg)
{
    (void)arg;

    while (!gArtTerminate) {
        SleepThread();

        if (gArtTerminate)
            break;

        for (;;) {
            load_image_request_t *req = artPop();

            if (!req)
                break; // queue empty, or its head is an SIO2 cover deferred while a direction is held

            cacheLoadImage(req);

            if (gArtTerminate)
                break;
        }
    }

    gArtRunning = 0;
    ExitDeleteThread();
}

void cacheInit()
{
    // No ioRegisterHandler any more. Art has left the shared queue entirely, and that IS the change:
    // the deferred menu rebuild (IO_MENU_UPDATE_DEFFERED) and the config save
    // (IO_CUSTOM_SIMPLEACTION) no longer queue BEHIND a page's worth of covers on one FIFO whose
    // worker holds gProcSemaId across the whole batch. Reported from hardware as everything feeling
    // "pop-in like": the list could not become correct until the device had served art for a view
    // the user had already left.
    if (gArtRunning)
        return;

    gArtTerminate = 0;
    gArtShutdownAbandoned = 0;
    gArtReqList = NULL;
    gArtReqEnd = NULL;
    gArtCurrentReq = NULL;
    gArtQueuedCount = 0;
    gArtActiveCount = 0;
    gArtNavActive = 0;

    gArtThread.attr = 0;
    gArtThread.option = 0;
    gArtThread.stack_size = ART_THREAD_STACK_SIZE;
    gArtThread.gp_reg = &_gp;
    gArtThread.func = &cacheArtWorkerThread;
    gArtThread.stack = gArtStack;
    gArtThread.initial_priority = ART_THREAD_PRIORITY;

    gArtThreadId = CreateThread(&gArtThread);
    if (gArtThreadId < 0) {
        gArtThreadId = -1;
        return; // gArtRunning stays 0 -> the enqueue refuses to claim slots, see cacheGetTexture
    }

    if (StartThread(gArtThreadId, NULL) < 0) {
        DeleteThread(gArtThreadId);
        gArtThreadId = -1;
        return;
    }

    gArtRunning = 1;
}

// Once per video frame, from the GUI thread. Two jobs, both O(1), neither able to block.
void cacheTickArt(void)
{
    // (1) Sample held direction for the SIO2 defer in artPop. GUI thread only -- getKeyPressed is
    // libpad, and every libpad call in this codebase is GUI-thread-only. getKeyPressed and not
    // getKey: getKey would CONSUME the menu repeat before navigation ever sees it.
    gArtNavActive = getKeyPressed(KEY_LEFT) || getKeyPressed(KEY_RIGHT) ||
                    getKeyPressed(KEY_UP) || getKeyPressed(KEY_DOWN) ||
                    getKeyPressed(KEY_L1) || getKeyPressed(KEY_R1);

    // (2) Lost-wakeup net. Work queued and nothing running means the worker is asleep and a wakeup
    // went missing -- one WakeupThread per frame costs nothing and makes the whole class of "art
    // silently stopped forever" impossible. It also re-arms the queue the instant a held direction
    // is released, without waiting for the next enqueue.
    if (gArtRunning && gArtQueuedCount > 0 && gArtActiveCount == 0)
        cacheWakeArtWorker();
}

int cacheEnd(int forceStop)
{
    int ms = forceStop ? ART_JOIN_MS_TERMINAL : ART_JOIN_MS_LAUNCH;

    if (!gArtRunning)
        return 1;

    cacheShutdownArtLoads(); // idempotent: latch, abort the in-flight one, drop the queue, wake
    gArtTerminate = 1;
    cacheWakeArtWorker();

    while (gArtRunning && ms-- > 0)
        DelayThread(1000);

    if (gArtRunning) {
        // ABANDON, never TerminateThread. Killing a thread parked inside fileXio leaves the shared
        // IOP RPC channel half-used, and every later fileXio call from ANY thread is then undefined.
        // Every caller of this is on its way to an IOP reset or a power-off, so the leaked thread
        // cannot outlive the handoff. We leak strictly less than the fork does -- there is no
        // semaphore to reclaim, because this design has none.
        gArtShutdownAbandoned = 1;
        LOG("cacheEnd: art worker still running after %d ms -- abandoned, not terminated\n",
            forceStop ? ART_JOIN_MS_TERMINAL : ART_JOIN_MS_LAUNCH);
        return 0;
    }

    gArtThreadId = -1;
    return 1;
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
    cache->activeRequests = 0;
    cache->content = (cache_entry_t *)malloc(count * sizeof(cache_entry_t));

    int i;
    for (i = 0; i < count; ++i)
        cacheClearItem(&cache->content[i], 0);

    return cache;
}

void cacheDestroyCache(image_cache_t *cache)
{
    int i, ms = ART_FOREGROUND_DRAIN_MS;

    if (cache == NULL)
        return;

    // Belt and braces on top of the drain the CALLER should have done. The dangerous caller is
    // exactly one: endMutableImage via thmFree, i.e. a theme switch. The others destroy a fresh
    // clone no request can ever have referenced.
    //
    // What a stray request would do if we just freed: dereference req->cache->suffix and ->prefix,
    // and req->entry -- a pointer INTO the content[] array below. Worse, a cancellation early-out
    // would then call cacheClearItem on whatever bytes now occupy the freed slot, i.e. free() a
    // garbage pointer. Harmless while art shared the io worker; a live use-after-free with a worker
    // running concurrently.
    if (cache->activeRequests > 0) {
        if (gArtCurrentReq && gArtCurrentReq->cache == cache)
            gArtCurrentReq->abortRequested = 1;
        cacheWakeArtWorker();

        while (cache->activeRequests > 0 && ms-- > 0)
            DelayThread(1000);
    }

    // LEAK RATHER THAN FREE. If the wait expired something still holds a pointer into this cache. A
    // few KB of EE RAM on a path that only runs on a theme switch is cheap; a use-after-free in the
    // art path is the entire class of bug this change exists to close.
    if (cache->activeRequests > 0) {
        LOG("cacheDestroyCache: %d art request(s) still reference this cache -- leaking it\n",
            cache->activeRequests);
        return;
    }

    for (i = 0; i < cache->count; ++i) {
        cacheClearItem(&cache->content[i], 1); // GUI thread: gsKit contact is correct here
    }

    free(cache->prefix);
    free(cache->suffix);
    free(cache->content);
    free(cache);
}

// Does this cover ride SIO2 -- the CONTROLLER'S OWN BUS? MX4SIO today; MMCE joins it the moment
// checklist item 1 lands, which is why MMCE_MODE is already tested here.
//
// Resolved on the GUI THREAD at enqueue, never on the worker: answering it reaches into the
// support's private device data, and a background rescan rewrites that underneath. One byte copied
// into the request is immune to that.
//
// FAV and APP rows are PROXIES -- their art lives on whatever device the underlying entry came from,
// not on the proxy tab -- so resolve through to the real mode first.
static int cacheRequestIsSIO2(item_list_t *list, const char *value)
{
    int mode;

    if (list == NULL)
        return 0;

    mode = list->mode;

    if (mode == FAV_MODE && value != NULL) {
        int src = favGetArtMode(value);
        if (src >= 0)
            mode = src;
    }
    if (mode == APP_MODE && value != NULL) {
        int src = appGetArtMode(value);
        if (src >= 0)
            mode = src;
    }

    if (mode == MMCE_MODE)
        return 1;

    return bdmModeIsSIO2(mode);
}

GSTEXTURE *cacheGetTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value)
{
    int i, rtime;
    if (cache == NULL || list == NULL || cacheId == NULL || UID == NULL || value == NULL || value[0] == '\0')
        return NULL;

    // NO memo lookup here. This function runs for EVERY art element on screen EVERY frame --
    // including covers that are already loaded and rendering -- and the memo check costs a
    // snprintf + hash + strcmp. Placed here it taxed every cached frame (dozens of format-string
    // calls per frame on a 294 MHz CPU); the fast path below must stay a couple of integer
    // compares, exactly what the hardware-validated builds did. The memo is consulted at the
    // ENQUEUE decision instead -- the only place it saves anything real (an IO attempt).

    if (*cacheId == -2) {
        return NULL;
    } else if (*cacheId != -1) {
        cache_entry_t *entry = &cache->content[*cacheId];
        if (entry->UID == *UID) {
            if (entry->qr) {
                // Still loading -- but mark it as still WANTED. This stamp is the only evidence the
                // worker has that anyone is still looking at this row (see cacheLoadImage).
                entry->lastUsed = guiFrameId;
                return NULL;
            } else if (entry->lastUsed == 0) {
                *cacheId = -2;
                return NULL;
            } else {
                entry->lastUsed = guiFrameId;
                return &entry->texture;
            }
        }

        *cacheId = -1;
    }

    // Under the cache pre-delay (to avoid filling cache while moving around) -- with a FLOOR that
    // the user's Art Delay setting cannot lower past.
    //
    // Why a floor exists at all: the IO worker is priority 30 and the GUI/pad thread is 31, so the
    // worker PREEMPTS rendering and input whenever it is runnable. The device read mostly blocks
    // (the GUI runs happily through it) but the PNG decode is pure CPU -- tens of ms of it -- and
    // during that the menu stops sampling the pad. With Art Delay 0 this predicate is
    // `guiInactiveFrames < 0`, never true, so covers are requested WHILE a direction is held: every
    // decode eats frames, the held-repeat then catches up in one step, and scrolling gains a
    // rhythmic lurch -- one per decode. Reported from hardware as "pressing at the same pace, every
    // so many clicks it jumps", and absent from both uOPL and rebuild-66, neither of which requests
    // art mid-scroll.
    //
    // A couple of frames is below the threshold of noticing for someone who set the delay to 0 to
    // make art appear promptly, and it is enough to keep decodes out of an active scroll.
    int artDelay = list->delay;
    if (artDelay < ART_MIN_SETTLE_FRAMES)
        artDelay = ART_MIN_SETTLE_FRAMES;
    if (guiInactiveFrames < artDelay)
        return NULL;


    cache_entry_t *currEntry, *oldestEntry = NULL;
    rtime = guiFrameId;

    for (i = 0; i < cache->count; i++) {
        currEntry = &cache->content[i];
        if (!currEntry->qr && currEntry->lastUsed < rtime) {
            oldestEntry = currEntry;
            rtime = currEntry->lastUsed;
            *cacheId = i;
        }
    }


    // BACKPRESSURE, lock-free (see gArtQueuedCount above). Art is the one request type that is
    // free to drop: the slot is never claimed, and the next frame simply asks again. Refusing to
    // deepen an already-deep queue keeps the config save, the deferred menu rebuild and device
    // init from starving behind hundreds of queued art reads on a slow device.
    // NO QUEUE-DEPTH CAP, and no in-flight gate. uOPL and rebuild-66 -- the two builds hardware
    // agrees are fast -- both enqueue every visible miss unconditionally, with no throttle of any
    // kind, and this fork's art was the only one that was slow. The cap was added to stop art
    // burying a config save; that is now handled where it belongs (the visible cover is a priority
    // request that runs next, and background device rescans yield to pending art), so the throttle
    // was pure latency. Refusing a request also cost more than it saved: a refusal resets the row's
    // cacheId and the whole claim is redone next frame.
    // gArtRunning is part of the gate: if the worker never started, nothing would ever clear qr and
    // the slot would be dead for the session. Better to draw no art than to poison the cache.
    if (oldestEntry && gArtRunning) {
        load_image_request_t *req = malloc(sizeof(load_image_request_t) + strlen(value) + 1);
        if (!req) {
            *cacheId = -1; // nothing cleared yet at this point, so the slot is untouched
            return NULL;
        }
        req->cache = cache;
        req->entry = oldestEntry;
        req->list = list;
        req->value = (char *)req + sizeof(load_image_request_t);
        strcpy(req->value, value);
        req->cacheUID = cache->nextUID;
        req->epoch = gArtEpoch; // the view this cover belongs to; see cacheDropQueuedArt()
        req->abortRequested = 0;
        req->next = NULL;
        // Resolved HERE, on the GUI thread -- see cacheRequestIsSIO2 for why not on the worker.
        req->sio2 = (unsigned char)cacheRequestIsSIO2(list, value);

        cacheClearItem(oldestEntry, 1);
        oldestEntry->qr = req;
        oldestEntry->UID = cache->nextUID;

        // BORN WANTED. cacheClearItem one line above just wrote the module's "this slot is free"
        // sentinel -- lastUsed = -1 -- and the cancellation in cacheLoadImage reads that SAME field
        // as an AGE: (u32)(guiFrameId - lastUsed). For -1 that evaluates to guiFrameId + 1, so from
        // frame 20 of every session onward a request reached the worker already counting as "20+
        // frames since anyone looked at this row" and was thrown away BEFORE it touched the device.
        // One field carrying two meanings; the cancellation added the reader and there had never
        // been a writer here to match it.
        //
        // It only bites when the queue is EMPTY, which is why it read as an idle-only fault. The IO
        // worker is priority 32 and the GUI 31, so it cannot preempt -- it runs in the vsync gap
        // inside the frame that queued the request, before the requester gets another draw pass to
        // refresh the stamp at the entry->qr branch in cacheGetTexture. Anything the worker was
        // already busy with pushes the request past that frame boundary, the requester re-stamps it,
        // and it survives. That is the whole reason art appeared during boot and device rescans and
        // then stopped the moment the menu settled.
        //
        // HARDWARE, debug HUD, sitting IDLE on a coverflow list with covers visibly on screen: 25
        // video frames apart the art total went 22269 -> 22293 -- one request per frame, forever --
        // while D stayed 59 and A stayed 0. The drop sits above both gArtActiveCount++ and
        // gArtDone++, so nothing counted and no device read was ever issued: 22269 requests, zero
        // loads. Coverflow's covers survived only because the draw path touches each key more than
        // once per frame; the perpetual loser was the per-game Background, which asks ONLY when
        // !cacheHasPendingArt() and therefore asks only in the exact state that guarantees a
        // same-frame drain -- and could not even park itself as "absent", because parking requires a
        // load that actually ran.
        //
        // Stamping here makes the test measure what its own comment says it measures: frames since
        // the row was last DRAWN. A row that scrolls away still stops stamping and its request still
        // dies at 20 frames, so the cancellation keeps doing its job -- it just stops shooting every
        // request the instant it is made. guiFrameId is >= 1 inside any drawn frame (guiStartFrame
        // increments before anything draws), so this can never write the 0 that means "load failed".
        oldestEntry->lastUsed = guiFrameId;

        *UID = cache->nextUID++;

        // The cache now owns an in-flight reference. cacheDestroyCache waits on this rather than
        // freeing content[] out from under a request that still points into it -- which a theme
        // switch would otherwise do.
        cache->activeRequests++;

        // artPush takes its own DIntr bracket and does the gArtQueuedCount++ inside it. It cannot
        // fail: no allocation, no queue cap. The old ioPutRequest-refused rollback is gone with
        // ioPutRequest, and the reason it existed is now structural -- nothing can refuse a push, so
        // no slot can be left with qr set and never cleared. (That failure killed 1-slot caches
        // outright: backgrounds and screenshots vanished for the session. See rebuild-38/43.)
        artPush(req);
        cacheWakeArtWorker();
    }

    return NULL;
}
