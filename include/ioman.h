#ifndef __IOMAN_H
#define __IOMAN_H

// Input output manager
// asynchronous io handling thread with worker queue

#define IO_OK                       0
#define IO_ERR_UNKNOWN_REQUEST_TYPE -1
#define IO_ERR_TOO_MANY_HANDLERS    -2
#define IO_ERR_DUPLICIT_HANDLER     -3
#define IO_ERR_INVALID_HANDLER      -4
#define IO_ERR_IO_BLOCKED           -5
#define IO_ERR_TOO_MANY_REQUESTS    -6 // request-node allocation failed (fork parity)

typedef void (*io_request_handler_t)(void *request);

typedef void (*io_simpleaction_t)(void);

/** initializes the io worker thread */
void ioInit(void);

/** deinitializes the io worker thread */
void ioEnd(void);

/** registers a handler for a certain request type */
int ioRegisterHandler(int type, io_request_handler_t handler);

/** schedules a new request into the pending request list
 * @note The data are not freed! */
int ioPutRequest(int type, void *data);



/** removes all requests of a given type from the queue
 * @param type the type of the requests to remove
 * @return the count of the requests removed */
int ioRemoveRequests(int type);

/** returns the count of pending requests */
int ioGetPendingRequestCount(void);

/** returns nonzero if there are any pending io requests */
int ioHasPendingRequests(void);

/** DIAGNOSTIC. Per-IO_*-type queue counters for the debug HUD. ioHasPendingRequests() is what raises
 * the busy overlay, but it only says "the queue is not empty" -- these say WHICH type is sitting in
 * it, and whether the running total is still climbing (something re-queueing itself every frame) or
 * frozen (one request that never finishes). Unsynchronised reads, display only.
 */
int ioGetPending(int type);
unsigned int ioGetTotal(int type);


/** Helper thread safe printf */
int ioPrintf(const char *format, ...);

/** Helper function. Will flush the io operation list
 (wait for all io ops requested to end) and then
 issue a blocking flag that will mean no io
 operation will get in.
 @param block If nonzero, will issue blocking, otherwise it will unblock
*/
int ioBlockOps(int block);

/** As ioBlockOps, but caps the drain wait at timeoutTicks (< 0 = unbounded).
 Used by the NBD preflight so a request stuck on a removed/slow device cannot
 hang the start indefinitely.
 @param block If nonzero, will issue blocking, otherwise it will unblock
 @param timeoutTicks Max delay(1) ticks to wait for pending IO; < 0 = unbounded
*/
int ioBlockOpsTimed(int block, int timeoutTicks);

#ifdef __DEBUG
#define PREINIT_LOG(...) printf(__VA_ARGS__)
#define LOG(...)         ioPrintf(__VA_ARGS__)
#else
#define PREINIT_LOG(...)
#define LOG(...)
#endif

#endif
