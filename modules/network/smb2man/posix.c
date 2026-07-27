/*
  POSIX functions libsmb2 calls that the IOP does not provide.

  Companion to shim/ (which supplies the missing HEADERS); this supplies the missing
  IMPLEMENTATIONS. Everything here is deliberately minimal -- just enough for libsmb2's use of it,
  not a general POSIX layer.
*/

#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

#include <ps2ip.h>
#include <sysclib.h>
#include <thbase.h>

void *malloc(size_t size); // from alloc.c

/*
  sysclib has vsprintf but NOT vsnprintf, so there is no bounded formatter on the IOP.

  Formatting into a scratch buffer and copying out is the only option. The scratch buffer must be
  comfortably larger than any string libsmb2 formats (it uses snprintf for paths and error text),
  because an overflow inside vsprintf would corrupt the stack before we ever got to truncate. 1KB
  covers a max-length SMB path with room to spare; the copy out is what actually enforces `size`.
*/
int snprintf(char *str, size_t size, const char *format, ...)
{
    char scratch[1024];
    va_list ap;
    int len;

    va_start(ap, format);
    len = vsprintf(scratch, format, ap);
    va_end(ap);

    if (len < 0)
        return len;

    if (str != NULL && size > 0) {
        size_t copy = ((size_t)len < size - 1) ? (size_t)len : size - 1;
        memcpy(str, scratch, copy);
        str[copy] = '\0';
    }

    // snprintf returns what WOULD have been written, not what fit.
    return len;
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
    char scratch[1024];
    int len;

    len = vsprintf(scratch, format, ap);
    if (len < 0)
        return len;

    if (str != NULL && size > 0) {
        size_t copy = ((size_t)len < size - 1) ? (size_t)len : size - 1;
        memcpy(str, scratch, copy);
        str[copy] = '\0';
    }

    return len;
}

char *strdup(const char *s)
{
    size_t len;
    char *p;

    if (s == NULL)
        return NULL;

    len = strlen(s) + 1;
    p = (char *)malloc(len);
    if (p == NULL)
        return NULL;

    memcpy(p, s, len);

    return p;
}

/*
  close() is NOT defined here. Something in the IOP/ps2ip header set already declares a static
  close(), and adding a second non-static declaration is a hard error. posix-compat.h aliases it to
  lwip_close with a macro instead, which is what libsmb2's socket closes need anyway.
*/

/*
  Used only to salt the NTLMSSP client challenge and to tag temp names. There is no process model
  on the IOP, so a fixed non-zero value is correct rather than merely convenient -- what matters is
  that it is stable within a session.
*/
int getpid(void)
{
    return 1;
}

int getlogin_r(char *buf, size_t size)
{
    // No user database on the IOP. libsmb2 falls back to the credentials OPL passes in, so an
    // empty answer here is the right outcome, not a failure.
    if (buf != NULL && size > 0)
        buf[0] = '\0';

    return 0;
}

/*
  Not cryptographic, and libsmb2 does not use it as such -- it seeds message ids and temp names.
  The NTLM responses themselves come from the server challenge, so this never gates auth strength.
  xorshift32 keeps it deterministic and dependency-free.
*/
static uint32_t rand_state = 0x2545F491;

void srandom(unsigned int seed)
{
    rand_state = (seed != 0) ? (uint32_t)seed : 0x2545F491; // a zero state would lock xorshift at 0
}

long random(void)
{
    rand_state ^= rand_state << 13;
    rand_state ^= rand_state >> 17;
    rand_state ^= rand_state << 5;

    // POSIX random() is [0, 2^31-1]; mask the sign bit off.
    return (long)(rand_state & 0x7FFFFFFF);
}

/*
  Seconds since boot, not since the epoch. libsmb2 uses this for its own relative timeouts, never
  for SMB timestamps (those come off the wire as FILETIME), so a monotonic clock is the right
  answer and the IOP has no RTC to offer anyway.
*/
time_t time(time_t *t)
{
    iop_sys_clock_t sysclock;
    u32 sec, usec;

    GetSystemTime(&sysclock);
    SysClock2USec(&sysclock, &sec, &usec);

    if (t != NULL)
        *t = (time_t)sec;

    return (time_t)sec;
}
