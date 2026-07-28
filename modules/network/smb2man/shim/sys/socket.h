/* IOP shim for <sys/socket.h>.

   ps2ip.h carries the socket API. lwIP here is IPv4-only, but libsmb2 sizes buffers against
   struct sockaddr_in6, so a minimal definition is provided -- it is never connected to, only
   sizeof'd and zeroed. */
#ifndef SMB2MAN_SHIM_SYS_SOCKET_H
#define SMB2MAN_SHIM_SYS_SOCKET_H

#include <stddef.h>
#include <ps2ip.h>

#ifndef SMB2MAN_SOCKADDR_IN6_DEFINED
#define SMB2MAN_SOCKADDR_IN6_DEFINED
/* struct in6_addr already comes from ps2ip -- only sockaddr_in6 is missing there. */
struct sockaddr_in6
{
    unsigned char sin6_len;
    unsigned char sin6_family;
    unsigned short sin6_port;
    unsigned int sin6_flowinfo;
    struct in6_addr sin6_addr;
    unsigned int sin6_scope_id;
};
#endif

int fcntl(int fd, int cmd, ...);

#endif
