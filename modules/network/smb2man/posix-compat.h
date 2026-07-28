/*
  Declarations for the POSIX functions posix.c implements.

  Force-included (-include) into every translation unit rather than injected into the shim headers,
  because some of these normally live in headers the IOP DOES provide (stdio.h) -- shadowing those
  to add one prototype would hide everything else they declare. One forced header keeps the
  vendored libsmb2 sources unmodified and avoids shadowing anything.
*/
#ifndef SMB2MAN_POSIX_COMPAT_H
#define SMB2MAN_POSIX_COMPAT_H

#include <stdarg.h>
#include <stddef.h>
#include <ps2ip.h>

int snprintf(char *str, size_t size, const char *format, ...);
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
char *strdup(const char *s);
/*
  close(): NOT declared here, and this needs care.

  An IOP header already DEFINES close() (redefinition error if we add one), but it is iomanX's
  file-descriptor close, not a socket close. libsmb2 calls close() on SOCKET descriptors, so
  whichever definition wins must be lwip_close or sockets leak / the wrong table is indexed.

  Unresolved. The likely fix is to give the vendored socket.c an explicit lwip_close via a
  -Dclose=lwip_close for that translation unit only, or to include the defining header here so at
  least the implicit-declaration error goes away and then verify which close libsmb2 actually
  binds. Do not paper over this: a file-close on a socket fd fails silently.
*/

#ifndef SMB2MAN_SSIZE_T_DEFINED
#define SMB2MAN_SSIZE_T_DEFINED
typedef int ssize_t; /* IOP is 32-bit; matches the int-returning lwip_recv/lwip_send */
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

int asprintf(char **strp, const char *fmt, ...);
int gethostname(char *name, size_t len);
char *strerror(int errnum);
int smb2man_connect(int s, struct sockaddr *name, socklen_t namelen);
int smb2man_recv(int s, void *mem, int len, unsigned int flags);
int smb2man_send(int s, void *dataptr, int size, unsigned int flags);
int getpid(void);
int getlogin_r(char *buf, size_t size);
void srandom(unsigned int seed);
long random(void);

#ifndef SMB2MAN_TIME_T_DEFINED
#define SMB2MAN_TIME_T_DEFINED
typedef long time_t;
#endif
time_t time(time_t *t);

#endif
