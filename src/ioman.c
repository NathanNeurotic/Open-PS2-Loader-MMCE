#include "include/opl.h"
#include "include/ioman.h"
#include "include/util.h"
#include <kernel.h>
#include <string.h>
#include <malloc.h>
#include <stdio.h>
#include <unistd.h>
#ifdef __EESIO_DEBUG
#include <sio.h>
#endif

#define MAX_IO_HANDLERS 64

extern void *_gp;

static int gIOTerminate = 0;

#define THREAD_STACK_SIZE (96 * 1024)

static u8 thread_stack[THREAD_STACK_SIZE] ALIGNED(16);

struct io_request_t
{
    int type;
    void *data;
    // Nonzero = background maintenance (periodic device rescans): excluded from the busy-spinner
    // count below, still counted by everything that DRAINS the queue. See ioPutRequestQuiet.
    int quiet;
    struct io_request_t *next;
};

struct io_handler_t
{
    int type;
    io_request_handler_t handler;
};

/// Circular request queue
static struct io_request_t *gReqList;
static struct io_request_t *gReqEnd;

// Total number of queued requests currently stored in the list.
static int gReqCount;

/*
  Number of NON-QUIET requests between enqueue and processing-complete -- what the GUI busy spinner
  keys off (#290).

  The spinner used to key off the raw queue (ioHasPendingRequests), which also counts the periodic
  background device rescans menuUpdateHook enqueues every MENU_GENERAL_UPDATE_DELAY frames. On a
  device where one presence scan takes a few hundred ms (MMCE's SIO2 round-trips, a busy USB stick),
  each rescan held the queue non-empty long enough for the fade-in to become visible -- the loading
  spinner "reappearing every few seconds" while the console sat idle, doing maintenance the user
  never asked for and cannot see the result of. Upstream OPL and wOPL share this (same hook, same
  raw-queue spinner, none of our scheduling gates), which is why the reporter reproduces it on the
  main branch.

  A request is visible from ioPutRequest until the worker has FINISHED processing it (the node is
  unlinked only after its handler returns), so the spinner still spans the whole duration of real
  work, exactly as before. Only who gets counted changed.

  Updated under gEndSemaId in every path that links or unlinks a node; read without the sema by the
  render loop, same single-word-read discipline as ioHasPendingRequests' pointer peek.
*/
static int gVisibleCount;

static struct io_handler_t gRequestHandlers[MAX_IO_HANDLERS];

static int gHandlerCount;

// id of the processing thread
static s32 gIOThreadId;

// lock for tip processing
static s32 gProcSemaId;
// lock for queue end
static s32 gEndSemaId;
// ioPrintf sema id
static s32 gIOPrintfSemaId;

static ee_thread_t gIOThread;
static ee_sema_t gQueueSema;

static int isIOBlocked = 0;
static int isIORunning = 0;

int ioRegisterHandler(int type, io_request_handler_t handler)
{
    WaitSema(gProcSemaId);

    // Every early return below must release gProcSemaId, otherwise a registration
    // failure leaves the semaphore held and deadlocks the entire I/O subsystem.
    if (handler == NULL) {
        SignalSema(gProcSemaId);
        return IO_ERR_INVALID_HANDLER;
    }

    if (gHandlerCount >= MAX_IO_HANDLERS) {
        SignalSema(gProcSemaId);
        return IO_ERR_TOO_MANY_HANDLERS;
    }

    int i;

    for (i = 0; i < gHandlerCount; ++i) {
        if (gRequestHandlers[i].type == type) {
            SignalSema(gProcSemaId);
            return IO_ERR_DUPLICIT_HANDLER;
        }
    }

    gRequestHandlers[gHandlerCount].type = type;
    gRequestHandlers[gHandlerCount].handler = handler;
    gHandlerCount++;

    SignalSema(gProcSemaId);

    return IO_OK;
}

static io_request_handler_t ioGetHandler(int type)
{
    int i;

    for (i = 0; i < gHandlerCount; ++i) {
        struct io_handler_t *h = &gRequestHandlers[i];

        if (h->type == type)
            return h->handler;
    }

    return NULL;
}

static void ioProcessRequest(struct io_request_t *req)
{
    if (!req)
        return;

    io_request_handler_t hlr = ioGetHandler(req->type);

    // invalidate the request
    void *data = req->data;

    if (hlr)
        hlr(data);
}

static void ioWorkerThread(void *arg)
{
    while (!gIOTerminate) {
        SleepThread();

        // if term requested exit immediately from the loop
        if (gIOTerminate)
            break;

        // do we have a request in the queue?
        WaitSema(gProcSemaId);
        while (gReqList) {
            // if term requested exit immediately from the loop
            if (gIOTerminate)
                break;

            struct io_request_t *req = gReqList;
            // In-flight request tracking (#254-class boot wedges): the last "begin" without a
            // matching "end" in the serial/debug log names the request that never completed.
            LOG("IOMAN request begin: type=%d data=%p pending=%d\n", req->type, req->data, gReqCount);
            ioProcessRequest(req);
            LOG("IOMAN request end: type=%d\n", req->type);

            // lock the queue tip as well now
            WaitSema(gEndSemaId);

            // can't be sure if the request was
            gReqList = req->next;
            if (!req->quiet && gVisibleCount > 0)
                gVisibleCount--; // processing done only now, so visibility spans the handler run
            free(req);
            if (gReqCount > 0)
                gReqCount--;

            if (!gReqList)
                gReqEnd = NULL;

            SignalSema(gEndSemaId);
        }
        SignalSema(gProcSemaId);
    }

    // delete the pending requests
    while (gReqList) {
        struct io_request_t *req = gReqList;
        gReqList = gReqList->next;
        if (!req->quiet && gVisibleCount > 0)
            gVisibleCount--;
        free(req);
        if (gReqCount > 0)
            gReqCount--;
    }

    // delete the semaphores
    DeleteSema(gProcSemaId);
    DeleteSema(gEndSemaId);
    DeleteSema(gIOPrintfSemaId);

    isIORunning = 0;

    ExitDeleteThread();
}

static void ioSimpleActionHandler(void *data)
{
    io_simpleaction_t action = (io_simpleaction_t)data;

    if (action)
        action();
}

void ioInit(void)
{
    gIOTerminate = 0;
    gHandlerCount = 0;
    gReqList = NULL;
    gReqEnd = NULL;
    gReqCount = 0;
    gVisibleCount = 0;

    gIOThreadId = 0;

    gQueueSema.init_count = 1;
    gQueueSema.max_count = 1;
    gQueueSema.option = 0;

    gProcSemaId = CreateSema(&gQueueSema);
    gEndSemaId = CreateSema(&gQueueSema);
    gIOPrintfSemaId = CreateSema(&gQueueSema);

    // default custom simple action handler
    ioRegisterHandler(IO_CUSTOM_SIMPLEACTION, &ioSimpleActionHandler);

    gIOThread.attr = 0;
    gIOThread.stack_size = THREAD_STACK_SIZE;
    gIOThread.gp_reg = &_gp;
    gIOThread.func = &ioWorkerThread;
    gIOThread.stack = thread_stack;
    gIOThread.initial_priority = 30;

    isIORunning = 1;
    gIOThreadId = CreateThread(&gIOThread);
    StartThread(gIOThreadId, NULL);
}

static int ioPutRequestInternal(int type, void *data, int quiet)
{
    if (isIOBlocked)
        return IO_ERR_IO_BLOCKED;

    // check the type before queueing
    if (!ioGetHandler(type))
        return IO_ERR_INVALID_HANDLER;

    WaitSema(gEndSemaId);

    // We don't have to lock the tip of the queue...
    // If it exists, it won't be touched, if it does not exist, it is not being processed
    struct io_request_t *req = (struct io_request_t *)malloc(sizeof(struct io_request_t));
    if (!req) {
        // Out of memory: do NOT corrupt the queue. The old code assigned the NULL straight into
        // gReqList/gReqEnd and then dereferenced it (req->next = NULL). Release the lock and fail.
        SignalSema(gEndSemaId);
        return IO_ERR_TOO_MANY_REQUESTS;
    }

    req->next = NULL;
    req->type = type;
    req->data = data;
    req->quiet = quiet;

    if (!gReqEnd)
        gReqList = req;
    else
        gReqEnd->next = req;
    gReqEnd = req;
    gReqCount++;
    if (!quiet)
        gVisibleCount++;

    SignalSema(gEndSemaId);

    // Worker thread cannot wake itself up (WakeupThread will return an error), but it will find the new request before sleeping.
    WakeupThread(gIOThreadId);
    return IO_OK;
}

int ioPutRequest(int type, void *data)
{
    return ioPutRequestInternal(type, data, 0);
}

// Same queue, same handlers, same drain semantics -- but excluded from the busy-spinner count
// (ioHasVisiblePendingRequests). For BACKGROUND MAINTENANCE only: work the user did not ask for and
// whose usual outcome is "nothing changed", like menuUpdateHook's periodic device rescans. Anything
// user-initiated (device enable, manual refresh, config save, module loads) must stay visible.
int ioPutRequestQuiet(int type, void *data)
{
    return ioPutRequestInternal(type, data, 1);
}

int ioRemoveRequests(int type)
{
    // lock the deletion sema and the queue end sema as well
    WaitSema(gProcSemaId);
    WaitSema(gEndSemaId);

    int count = 0;
    struct io_request_t *req = gReqList;
    struct io_request_t *last = NULL;

    while (req) {
        if (req->type == type) {
            struct io_request_t *next = req->next;

            if (last)
                last->next = next;

            if (req == gReqList)
                gReqList = next;

            if (req == gReqEnd)
                gReqEnd = last;

            count++;
            if (!req->quiet && gVisibleCount > 0)
                gVisibleCount--;
            free(req);
            if (gReqCount > 0)
                gReqCount--;

            req = next;
        } else {
            last = req;
            req = req->next;
        }
    }

    SignalSema(gEndSemaId);
    SignalSema(gProcSemaId);

    return count;
}

void ioEnd(void)
{
    // termination requested flag
    gIOTerminate = 1;

    // wake up and wait for end
    WakeupThread(gIOThreadId);
}

int ioIsRunning(void)
{
    return isIORunning;
}

int ioGetPendingRequestCount(void)
{
    int count = 0;

    /* Snapshot gReqList INSIDE the lock: the worker frees nodes under
     * gProcSemaId, so reading the pointer before acquiring it is a
     * use-after-free on the very first req->next dereference. */
    WaitSema(gProcSemaId);
    struct io_request_t *req = gReqList;

    while (req) {
        count++;
        req = req->next;
    }

    SignalSema(gProcSemaId);

    return count;
}

int ioHasPendingRequests(void)
{
    return gReqList != NULL ? 1 : 0;
}

// Pending requests EXCLUDING quiet background maintenance -- what the loading spinner shows (#290).
// Everything that drains or throttles the queue must keep using ioHasPendingRequests: a quiet
// request still occupies the worker, still delays a launch, and must still hold off the next
// background rescan.
int ioHasVisiblePendingRequests(void)
{
    return gVisibleCount > 0 ? 1 : 0;
}

#ifdef __EESIO_DEBUG
static char tbuf[2048];
#endif

int ioPrintf(const char *format, ...)
{
    if (isIORunning == 1)
        WaitSema(gIOPrintfSemaId);

    va_list args;
    va_start(args, format);
#ifdef __EESIO_DEBUG
    int ret = vsnprintf((char *)tbuf, sizeof(tbuf), format, args);
    sio_putsn(tbuf);
#else
    int ret = vprintf(format, args);
#endif
    va_end(args);

    if (isIORunning == 1)
        SignalSema(gIOPrintfSemaId);

    return ret;
}

int ioBlockOpsTimed(int block, int timeoutTicks)
{
    ee_thread_status_t status;
    int ThreadID;

    if (block && !isIOBlocked) {
        isIOBlocked = 1;

        ThreadID = GetThreadId();
        // EE ReferThreadStatus() returns the thread status (>= 0) or a negative error, NOT 0 on
        // success (that is the IOP thbase variant). The old `== 0` test was always false, so
        // haveStatus was always 0 and the saved priority below was never restored -- every
        // ioBlockOps(1) caller left this (usually the main GUI) thread pinned at priority 90 for
        // the rest of the session. `>= 0` is the correct success test.
        int haveStatus = (ReferThreadStatus(ThreadID, &status) >= 0);
        ChangeThreadPriority(ThreadID, 90);

        // Wait for the in-flight IO handler(s) to finish. timeoutTicks < 0 waits
        // unbounded (default); a non-negative value caps the wait. Callers that
        // are tearing the system down (exit/poweroff) pass a bound so a request
        // stuck on a removed/slow device cannot hang teardown forever -- safe
        // there because the IOP is reset/powered off immediately afterward.
        while (ioHasPendingRequests()) {
            if (timeoutTicks == 0)
                break;
            delay(1);
            if (timeoutTicks > 0)
                timeoutTicks--;
        }

        // Only restore the saved priority if ReferThreadStatus actually filled it.
        if (haveStatus)
            ChangeThreadPriority(ThreadID, status.current_priority);

        // now all io should be blocked
    } else if (!block && isIOBlocked) {
        isIOBlocked = 0;
    }

    return IO_OK;
}

int ioBlockOps(int block)
{
    // Unbounded wait (historical behavior) for all non-teardown callers.
    return ioBlockOpsTimed(block, -1);
}
