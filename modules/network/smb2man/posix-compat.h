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
