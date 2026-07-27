/* IOP shim for <assert.h>: no POSIX layer on the IOP. Present so libsmb2 compiles unmodified;
   the symbols it actually uses come from sysclib, ps2ip or this module. */
#ifndef SMB2MAN_SHIM_assert
#define SMB2MAN_SHIM_assert
#include <stddef.h>
#endif
