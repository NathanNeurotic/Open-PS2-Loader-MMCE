/*
  Hand-written stand-in for libsmb2's autoconf config.h, for the freestanding IOP build.

  libsmb2 normally gets this from ./configure, which cannot run against an IRX target. Everything
  below is chosen for what the IOP actually provides: the ps2ip (lwIP) socket layer and the small
  set of headers in the PS2SDK IOP include path -- NOT a hosted POSIX system. Anything absent is
  simply left undefined, which is how libsmb2's #ifdef ladders expect to be told "not available".

  Deliberately OFF:
    HAVE_LIBKRB5      -- no Kerberos on the IOP; NTLMSSP is the only auth path (krb5-wrapper.c is
                         excluded from the build entirely).
    HAVE_DCERPC_FULL  -- the full DCE/RPC surface is only needed for share enumeration, which this
                         driver reports as empty (see SMB_DEVCTL_GETSHARELIST in smb2man.c).
    HAVE_POLL_H / HAVE_SYS_POLL_H
                      -- ps2ip exposes select(), not poll(); libsmb2 falls back to select.
    HAVE_SOCKADDR_LEN / SA_LEN / SIN_LEN
                      -- lwIP's sockaddr has no sa_len field in the PS2SDK build.
*/

#ifndef SMB2MAN_CONFIG_H
#define SMB2MAN_CONFIG_H

#define HAVE_STDINT_H  1
#define HAVE_STDIO_H   1
#define HAVE_STDLIB_H  1
#define HAVE_STRING_H  1
#define HAVE_ERRNO_H   1
#define HAVE_TIME_H    1
#define HAVE_UNISTD_H  1
#define HAVE_FCNTL_H   1
#define HAVE_SYS_TYPES_H 1

/* ps2ip provides the BSD socket API surface libsmb2 needs. */
#define HAVE_SYS_SOCKET_H  1
#define HAVE_NETINET_IN_H  1
#define HAVE_NETINET_TCP_H 1
#define HAVE_ARPA_INET_H   1
#define HAVE_NETDB_H       1
#define HAVE_ADDRINFO      1
#define HAVE_SOCKADDR_STORAGE 1

/* SO_LINGER exists in lwIP. */
#define HAVE_LINGER 1

#endif /* SMB2MAN_CONFIG_H */

/*
  NEXT STEP (build is at this point):

  The IOP has no arpa/inet.h, netdb.h, sys/socket.h etc. as real headers -- ps2ip.h provides the
  whole BSD socket surface in one file. Flipping the HAVE_* macros above off one at a time works
  but fights libsmb2's #ifdef ladders. The cleaner fix, and the one PS2 ports of portable C
  libraries normally use, is a small shim include directory (arpa/inet.h, netdb.h, sys/socket.h,
  netinet/in.h, netinet/tcp.h) where each shim is a one-line `#include <ps2ip.h>`, added to
  IOP_INCS ahead of everything else. Then these HAVE_* stay on and the vendored sources compile
  unmodified.
*/
