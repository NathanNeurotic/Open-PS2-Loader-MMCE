/* IOP shim for <sys/uio.h>.

   lwIP has no scatter/gather socket I/O, so struct iovec is declared here and readv/writev are
   implemented in posix.c as simple loops over lwip_recv/lwip_send. libsmb2 uses them to send an
   SMB2 header and its payload without copying them into one buffer; looping preserves that
   behaviour, just with one syscall per segment. */
#ifndef SMB2MAN_SHIM_SYS_UIO_H
#define SMB2MAN_SHIM_SYS_UIO_H

#include <stddef.h>

#ifndef SMB2MAN_IOVEC_DEFINED
#define SMB2MAN_IOVEC_DEFINED
struct iovec
{
    void *iov_base;
    size_t iov_len;
};
#endif

int readv(int fd, const struct iovec *iov, int iovcnt);
int writev(int fd, const struct iovec *iov, int iovcnt);

#endif
