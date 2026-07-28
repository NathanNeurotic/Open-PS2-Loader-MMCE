/* IOP shim for <sys/time.h>.

   ps2ip.h declares lwip_select() but does not expose struct timeval, and the IOP has no time_t.
   Both are defined here, guarded, so a future ps2ip that does export them wins instead of
   colliding. */
#ifndef SMB2MAN_SHIM_SYS_TIME
#define SMB2MAN_SHIM_SYS_TIME

#include <stddef.h>
#include <ps2ip.h>

#ifndef SMB2MAN_TIME_T_DEFINED
#define SMB2MAN_TIME_T_DEFINED
typedef long time_t;
#endif

#if !defined(_TIMEVAL_DEFINED) && !defined(LWIP_TIMEVAL_PRIVATE)
#define _TIMEVAL_DEFINED
struct timeval
{
    long tv_sec;
    long tv_usec;
};
#endif

#endif
