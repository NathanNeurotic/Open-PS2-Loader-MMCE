/* IOP shim for <poll.h>.

   ps2ip exposes select(), not poll(), so struct pollfd and the POLL* bits are declared here and
   poll() itself is implemented over lwip_select() in posix.c. libsmb2 only ever polls a single
   socket for readability/writability, which select covers exactly. */
#ifndef SMB2MAN_SHIM_POLL_H
#define SMB2MAN_SHIM_POLL_H
#include <stddef.h>

#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

struct pollfd
{
    int fd;
    short events;
    short revents;
};

typedef unsigned int nfds_t;

int poll(struct pollfd *fds, nfds_t nfds, int timeout);

#endif
