/* IOP shim for <fcntl.h>: open-flag and fcntl constants libsmb2 uses. ps2ip supplies F_GETFL /
   F_SETFL / O_NONBLOCK for sockets; the O_* file flags below are only ever passed through
   libsmb2's own open path, which this driver maps onto SMB2 CREATE dispositions. */
#ifndef SMB2MAN_SHIM_FCNTL_H
#define SMB2MAN_SHIM_FCNTL_H
#include <stddef.h>
#include <ps2ip.h>

#ifndef O_RDONLY
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#endif
#ifndef O_CREAT
#define O_CREAT  0x0200
#define O_TRUNC  0x0400
#define O_EXCL   0x0800
#define O_APPEND 0x0008
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x0004
#endif
#ifndef F_GETFL
#define F_GETFL 3
#define F_SETFL 4
#endif

#endif
