/* IOP shim for <sys/time.h>: struct timeval comes from ps2ip; time_t has no IOP definition. */
#ifndef SMB2MAN_SHIM_SYS_TIME
#define SMB2MAN_SHIM_SYS_TIME
#include <stddef.h>
#include <ps2ip.h>
#ifndef SMB2MAN_TIME_T_DEFINED
#define SMB2MAN_TIME_T_DEFINED
typedef long time_t;
#endif
#endif
