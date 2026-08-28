#ifndef __OPL_DIAG_H
#define __OPL_DIAG_H

#ifdef __cplusplus
extern "C" {
#endif

// Captured at the settings-write failure site for the later user-facing error.
extern volatile int gLastSaveErrno;
extern volatile int gLastDeferredTimedOut;

#ifdef __cplusplus
}
#endif

#endif

