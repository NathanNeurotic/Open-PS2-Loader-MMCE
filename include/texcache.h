#ifndef __TEX_CACHE_H
#define __TEX_CACHE_H

#include "include/iosupport.h"

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

    // The value this slot was loaded FOR (a game's startup id, or a VCD's filename). The cache is
    // otherwise addressable only by (slot index, UID), which are handed out per request -- so a row
    // that lost its ids could not find art it already had, even sitting decoded in the very next
    // slot. Every list rebuild resets those ids, so a rebuild threw the whole viewport away and
    // re-read every visible cover off the device.
    //
    // ⚠ This field and the comment above it shipped in rebuild-129 with NO code behind them: nothing
    // in the tree ever wrote it or read it until rebuild-153. That is the fifth instance of this
    // project's recurring defect shape (data half + comment half land, code half does not), and it
    // is why the rebuild's art felt worse than master's -- master has the same reunion, keyed on a
    // malloc'd entry->value.
    //
    // Empty means "no key": set only when the value FITS, so a match is always a whole-string match
    // and two long VCD filenames sharing a prefix can never trade covers.
    //
    // ⚠ SIZED FOR THE LONGEST THING THAT IS ACTUALLY KEYED, WHICH IS NOT A STARTUP ID. At 64 this
    // silently excluded most of the VCD page: a PS2 row keys on a startup id ("SLUS_123.45", 11
    // chars) and always fitted, but a PS1 row keys on the VCD BASENAME, which vcdsupport.h allows up
    // to VCD_NAME_MAX (256) and which in practice looks like "Final Fantasy VII (Disc 1) [SCUS-94163]".
    // Anything at or over the limit gets no key, and with no key it gets NONE of the reunion:
    // no adopting art the cache already holds, no adopting an in-flight load, and -- the expensive
    // one -- no adopting the "this file is absent" memo, so every missing PS1 cover was re-probed
    // from the device on every single list rebuild.
    //
    // That is why the VCD page stayed slow while everything else got faster: rebuild-153's reunion
    // and rebuild-155's absence memo both worked perfectly for ISO rows and were unreachable for PS1
    // ones. 128 covers real PS1 filenames with room to spare; anything longer still falls back to the
    // old behaviour, which is SLOW rather than WRONG -- an exact match or nothing, never a guess.
    char key[128];

    // Which fail generation the "absent" park (lastUsed == 0) belongs to. cacheInvalidateFailMemo()
    // bumps the global; a memo from an older generation is ignored, so "new art may have appeared"
    // (applyConfig, a list rebuild) makes rows re-probe exactly as it always claimed to.
    unsigned int failEpoch;
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

    /// In-flight art requests holding a pointer INTO this cache's content[]. Incremented on the GUI
    /// thread at enqueue, decremented on the art thread when the request is released.
    ///
    /// It exists because a theme switch frees caches out from under the art worker: thmFree ->
    /// endMutableImage -> cacheDestroyCache free()s content[] while a queued request still holds
    /// req->entry (a pointer INTO that array) plus req->cache->prefix and ->suffix. Before the art
    /// thread that was survivable by accident; with a worker running concurrently it is a real
    /// use-after-free, so cacheDestroyCache now waits on this and leaks rather than frees if the
    /// wait expires.
    volatile int activeRequests;
} image_cache_t;

/** Initializes the cache subsystem.
 */
void cacheInit();

/** Stops the art worker and releases its queue. Returns 1 if the worker joined, 0 if it was
 * ABANDONED (still inside a device read after the budget). forceStop picks the longer budget.
 *
 * Abandoning is deliberate: a thread parked inside fileXio must never be TerminateThread'd -- that
 * leaves the shared IOP RPC channel half-used and every later fileXio call from any thread is
 * undefined. Every caller is on its way to an IOP reset or a power-off, so a leaked thread cannot
 * outlive the handoff.
 *
 * Signature changed from `void cacheEnd()` when the art thread landed. Safe: it had ZERO callers.
 */
int cacheEnd(int forceStop);

/** Once per video frame, from the GUI thread (guiReadPads). Samples held direction for the SIO2
 * defer, and re-wakes the worker if a queue is sitting with nothing running. O(1), never blocks.
 */
void cacheTickArt(void);

/** Initializes a single cache
 */
image_cache_t *cacheInitCache(int userId, const char *prefix, int isPrefixRelative, const char *suffix, int count);

/** Destroys a given cache (unallocates all memory stored there, disconnects the pixmaps from the usage points).
 */
void cacheDestroyCache(image_cache_t *cache);

GSTEXTURE *cacheGetTexture(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value);
GSTEXTURE *cacheGetTextureEx(image_cache_t *cache, item_list_t *list, int *cacheId, int *UID, char *value, int isPriority);


/** Lookup only: returns this item's texture if its cache slot is still live, and never claims a
 * slot or queues a load. For drawing art already held while deliberately not requesting more.
 */
GSTEXTURE *cacheLookupTexture(image_cache_t *cache, int *cacheId, int *UID);

/** Diagnostics for the debug HUD: art requests queued, being read/decoded right now, turned away by
 * the depth cap (cumulative), and completed (cumulative). Any argument may be NULL.
 */
void cacheDebugCounters(int *queued, int *active, int *refused, int *done);
int cacheDebugDropped(void);

/** Debug HUD: rows whose value did not fit cache_entry_t.key, and which therefore get none of the
 * reunion or the absence memo. Should be 0; see the field comment for what a non-zero value costs. */
unsigned int cacheDebugKeyTooLong(void);

/** Debug HUD: art loads that failed transiently and were retried rather than parked as absent.
 * Non-zero means a device is misbehaving but the art is self-healing -- which is the point. */
unsigned int cacheDebugTransientFail(void);

/** Debug HUD: cost in ms of the last art load and of the last SUCCESSFUL one, plus that image's
 * decoded pixel dimensions. Separates "one load is slow" from "we ask for too many". Any argument
 * may be NULL.
 */
void cacheDebugLastLoad(int *lastMs, int *lastOkMs, int *width, int *height);

/** May speculative art (viewport warming, far-row thumbnails, the Coverflow lookahead) be issued
 * now? Looser than cacheHasPendingArt(): it allows prefetch while a few loads are already in
 * flight, so the device stays busy instead of prefetch running one image at a time.
 */
int cacheMayPrefetchArt(void);
void cacheInvalidateFailMemo(void);

/** Nonzero while any cover art is queued for, or currently being, read+decoded by the IO worker.
 * menuUpdateHook uses it to keep background device rescans out of the shared queue until the art
 * the user is waiting on has arrived.
 */
int cacheHasPendingArt(void);

// NO LONGER STUBS. Both were `static inline { return 1; }` for as long as art rode the shared IO
// worker, which quietly disarmed FIVE call sites that were written expecting them to work:
//   themes.c   -- the theme-switch guard, whose "keep the current theme, retry later" branch was
//                 therefore UNREACHABLE. That is a live use-after-free: thmFree -> endMutableImage
//                 -> cacheDestroyCache frees cache->content while queued requests still hold
//                 req->entry, a pointer INTO that array, plus req->cache->prefix and ->suffix.
//   appsupport -- appUpdateItemList frees appsList/appArtLookupTable underneath appGetImage.
//   hddsupport -- a non-blocking "drop what is queued" before pfs0: remounts.
//   opl.c      -- the LWNBD preflight.
// Arming them is part of the point of giving art its own thread: with a worker running concurrently,
// those races stop being theoretical.
//
// waitTicks is MILLISECONDS at this tick size (the wait is DelayThread(1000), not util.c's delay(),
// whose "tick" is a ~0.25 s NOP spin). The existing call-site constants are all sane read that way.
//
// Both return 1 when nothing is left outstanding, 0 if the budget expired.
int cacheAbortMmceImageLoadsTimed(int waitTicks);
int cacheCancelPendingImageLoadsTimed(int waitTicks);

// STUB, and it MUST STAY ONE. themes.c calls this on the first theme load (curT == NULL), which is
// every boot -- at that point the cache is not even initialised and nothing can be queued, so the
// fork's "drain now" semantics are a genuine no-op here.
//
// ⚠ rebuild-142 made this real AND sticky for the teardown case and killed cover art on every boot,
// because that themes.c caller was never looked at. The teardown need is a DIFFERENT operation with
// a different lifetime -- it is cacheShutdownArtLoads() below. Do not merge the two again.
static inline void cacheCancelPendingImageLoads(void)
{
}

// Abandon every cover ALREADY QUEUED, because the view they belong to is being thrown away.
//
// Distinct from the per-row cancellation inside the loader, which keys on a row having scrolled away
// (its lastUsed stamp going stale). A view toggle discards every row at once WITHOUT any of them
// scrolling, so the stamps stay fresh and not one of those covers is cancelled -- they sit FIFO
// ahead of the list rebuild on the single shared IO worker, and the page cannot become correct until
// the device has served every cover for the view being discarded.
//
// Re-armable, unlike cacheShutdownArtLoads() below: this is a normal in-session event. Cheap, and
// safe from the GUI thread -- it takes no lock and touches one counter. Each dropped request
// releases its cache slot properly on the way out.
void cacheDropQueuedArt(void);

// Stop servicing queued cover art for the rest of the process. TEARDOWN ONLY.
//
// Not an optimisation -- a launch-latency fix. ioBlockOpsTimed waits for the WHOLE ioman queue to
// drain, and isIOBlocked only stops NEW requests being accepted; the worker keeps servicing
// everything already queued. So a launch pressed while covers are in flight pays for every one of
// them to be read off the game device before the IOP reset -- for a menu guiEnd() is about to
// destroy. On a slow transport (MX4SIO's SD over SIO2) that IS the wait.
//
// One-way on purpose, and that is exactly why it needs its own name: the ONLY callers are deinit()
// and deinitEx(), both of which hand off to another ELF and never return to a live menu. Anything
// that DOES return to a live menu must not call this.
void cacheShutdownArtLoads(void);

#endif
