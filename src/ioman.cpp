#include "include/opl.h"
#include "include/ioman.h"
#include "include/util.h" // delay() -- bounded drain in ioBlockOpsTimed
#include <kernel.h>
#include <string.h>
#include <malloc.h>
#include <stdio.h>
#include <unistd.h>
#ifdef __EESIO_DEBUG
#include <sio.h>
#endif

#define MAX_IO_REQUESTS 64
#define MAX_IO_HANDLERS 64

extern void *_gp;

static int gIOTerminate = 0;

#define THREAD_STACK_SIZE (96 * 1024)

static u8 thread_stack[THREAD_STACK_SIZE] ALIGNED(16);

struct io_request_t
{
    int type;
    void *data;
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

// DIAGNOSTIC ONLY. The busy overlay is driven by ioHasPendingRequests(), i.e. purely by this queue
// being non-empty -- so a spinner that never fades means something in here genuinely is not
// completing. Reading the source cannot say WHICH of the four request types that is, and cannot
// tell apart the two very different causes: ONE slow request that never finishes, versus something
// re-queueing itself every frame so the queue is never observed empty. So count both. `pending`
// answers the first; `total` still climbing while the spinner sits still answers the second.
//
// Plain ints on purpose: written under gEndSemaId, which both the enqueue and the worker's unlink
// already hold, and read unsynchronised from the GUI thread for display -- exactly like the gArt*
// counters the HUD already shows. The GUI thread must NEVER take an ioman semaphore.
static int gIoPending[IO_REQ_TYPE_COUNT];
static unsigned int gIoTotal[IO_REQ_TYPE_COUNT];

int ioGetPending(int type)
{
    return (type > 0 && type < IO_REQ_TYPE_COUNT) ? gIoPending[type] : 0;
}

unsigned int ioGetTotal(int type)
{
    return (type > 0 && type < IO_REQ_TYPE_COUNT) ? gIoTotal[type] : 0;
}

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
            ioProcessRequest(req);

            // lock the queue tip as well now
            WaitSema(gEndSemaId);

            if (req->type > 0 && req->type < IO_REQ_TYPE_COUNT && gIoPending[req->type] > 0)
                gIoPending[req->type]--;

            // can't be sure if the request was
            gReqList = req->next;
            free(req);

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
        free(req);
    }

    // delete the semaphores
    DeleteSema(gProcSemaId);
    DeleteSema(gEndSemaId);

    isIORunning = 0;

    ExitDeleteThread();
}

static void ioSimpleActionHandler(void *data)
{
    static_assert(sizeof(io_simpleaction_t) == sizeof(void *), "EE callback and request payload pointers must have the same representation size");

    io_simpleaction_t action;
    memcpy(&action, &data, sizeof(action));

    if (action)
        action();
}

int ioPutSimpleAction(io_simpleaction_t action)
{
    static_assert(sizeof(io_simpleaction_t) == sizeof(void *), "EE callback and request payload pointers must have the same representation size");

    void *data;
    memcpy(&data, &action, sizeof(data));
    return ioPutRequest(IO_CUSTOM_SIMPLEACTION, data);
}

void ioInit(void)
{
    gIOTerminate = 0;
    gHandlerCount = 0;
    gReqList = NULL;
    gReqEnd = NULL;

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
    gIOThread.func = (void *)&ioWorkerThread;
    gIOThread.stack = thread_stack;
    // BELOW the GUI/pad thread (31), not above it.
    //
    // At 30 this worker OUTRANKED the thread that renders and reads the controller, so any work it
    // picked up -- a cover decode, a device probe, a list rebuild, even churning through a queue of
    // superseded requests -- took the CPU away from input for as long as it ran. On hardware that is
    // the navigation fault the reporter has chased all session: hold, a swallowed step, resume, and
    // more recently "jumping instead of stalling" while the HUD showed seventeen art requests queued
    // with none executing. Everything queued here is BACKGROUND work by definition -- it exists
    // precisely so the menu does not have to wait for it -- so it has no business preempting the
    // menu.
    //
    // Safe because the GUI never busy-waits on this thread: guiHandleDeferedIO pumps real frames and
    // blocks on vsync each pass (guiShow), which yields the CPU, so the worker still runs during a
    // wait. Blocking device I/O is unaffected -- a thread blocked in the IOP RPC consumes nothing at
    // either priority. What changes is only who wins when both are runnable, and that should always
    // be the one holding the user's input.
    gIOThread.initial_priority = 32;

    isIORunning = 1;
    gIOThreadId = CreateThread(&gIOThread);
    StartThread(gIOThreadId, NULL);
}

int ioPutRequest(int type, void *data)
{
    if (isIOBlocked)
        return IO_ERR_IO_BLOCKED;

    // check the type before queueing
    if (!ioGetHandler(type))
        return IO_ERR_INVALID_HANDLER;

    WaitSema(gEndSemaId);

    // We don't have to lock the tip of the queue...
    // If it exists, it won't be touched, if it does not exist, it is not being processed
    // Allocate FIRST and bail cleanly on OOM (fork parity): the old shape malloc'd straight into
    // gReqList/gReqEnd->next and then dereferenced the result, so an allocation failure was a NULL
    // write plus a corrupted queue tail -- with the semaphore held.
    struct io_request_t *req = (struct io_request_t *)malloc(sizeof(struct io_request_t));
    if (!req) {
        SignalSema(gEndSemaId);
        return IO_ERR_TOO_MANY_REQUESTS;
    }

    if (!gReqEnd)
        gReqList = req;
    else
        gReqEnd->next = req;
    gReqEnd = req;

    req->next = NULL;
    req->type = type;
    req->data = data;

    if (type > 0 && type < IO_REQ_TYPE_COUNT) {
        gIoPending[type]++;
        gIoTotal[type]++;
    }

    SignalSema(gEndSemaId);

    // Worker thread cannot wake itself up (WakeupThread will return an error), but it will find the new request before sleeping.
    WakeupThread(gIOThreadId);
    return IO_OK;
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
            free(req);

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

int ioGetPendingRequestCount(void)
{
    int count = 0;

    struct io_request_t *req = gReqList;

    WaitSema(gProcSemaId);

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
        // success (that is the IOP thbase variant). An `== 0` test is always false, so haveStatus
        // would always be 0 and the saved priority below would never be restored -- leaving this
        // (usually the main GUI) thread pinned at priority 90 for the rest of the session.
        int haveStatus = (ReferThreadStatus(ThreadID, &status) >= 0);
        ChangeThreadPriority(ThreadID, 90);

        // Wait for the in-flight IO handler(s) to finish. timeoutTicks < 0 waits unbounded
        // (the historical behaviour); a non-negative value caps the wait. The NBD preflight
        // passes a bound so a request stuck on a removed/slow device fails the start instead
        // of freezing after audio and pads have already been dismantled.
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
