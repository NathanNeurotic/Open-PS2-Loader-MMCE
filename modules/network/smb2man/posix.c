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

#include <poll.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netdb.h>

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

/*
  poll() over lwip_select().

  ps2ip has no poll(). libsmb2 uses it to wait on a single socket for readability/writability, which
  select covers exactly -- so translate rather than stubbing. Only the bits libsmb2 sets (POLLIN /
  POLLOUT) are honoured; POLLERR/POLLHUP are reported via select's exception set.
*/
int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    fd_set rd, wr, ex;
    struct timeval tv, *ptv;
    int maxfd = -1, rv;
    unsigned int i;

    FD_ZERO(&rd);
    FD_ZERO(&wr);
    FD_ZERO(&ex);

    for (i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0)
            continue;
        if (fds[i].events & POLLIN)
            FD_SET(fds[i].fd, &rd);
        if (fds[i].events & POLLOUT)
            FD_SET(fds[i].fd, &wr);
        FD_SET(fds[i].fd, &ex);
        if (fds[i].fd > maxfd)
            maxfd = fds[i].fd;
    }

    if (maxfd < 0)
        return 0;

    if (timeout < 0) {
        ptv = NULL; // block indefinitely
    } else {
        tv.tv_sec = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;
        ptv = &tv;
    }

    rv = lwip_select(maxfd + 1, &rd, &wr, &ex, ptv);
    if (rv <= 0)
        return rv;

    rv = 0;
    for (i = 0; i < nfds; i++) {
        if (fds[i].fd < 0)
            continue;
        if (FD_ISSET(fds[i].fd, &rd))
            fds[i].revents |= POLLIN;
        if (FD_ISSET(fds[i].fd, &wr))
            fds[i].revents |= POLLOUT;
        if (FD_ISSET(fds[i].fd, &ex))
            fds[i].revents |= POLLERR;
        if (fds[i].revents != 0)
            rv++;
    }

    return rv;
}

int asprintf(char **strp, const char *fmt, ...)
{
    char scratch[1024];
    va_list ap;
    int len;

    if (strp == NULL)
        return -1;

    va_start(ap, fmt);
    len = vsprintf(scratch, fmt, ap);
    va_end(ap);

    if (len < 0)
        return -1;

    *strp = (char *)malloc((size_t)len + 1);
    if (*strp == NULL)
        return -1;

    memcpy(*strp, scratch, (size_t)len + 1);

    return len;
}

int gethostname(char *name, size_t len)
{
    // No hostname on the IOP. libsmb2 uses this only to fill the NTLMSSP workstation field, where
    // a fixed name is fine (and matches what the in-game client sends).
    const char *host = "PLAYSTATION2";
    size_t n = strlen(host);

    if (name == NULL || len == 0)
        return -1;
    if (n >= len)
        n = len - 1;

    memcpy(name, host, n);
    name[n] = '\0';

    return 0;
}

char *strerror(int errnum)
{
    // libsmb2 only ever prints this. Returning a constant avoids dragging in a message table for
    // text nobody reads on a console with no stderr.
    (void)errnum;
    return "error";
}

/*
  readv/writev over plain recv/send.

  lwIP has no scatter/gather socket calls. libsmb2 uses these to push an SMB2 header and its
  payload without first copying them into a single buffer; looping keeps that zero-copy property on
  the caller's side, at the cost of one lwIP call per segment.

  Short writes are propagated rather than retried: libsmb2 tracks how much of its iovec set was
  consumed and re-enters, so silently looping here would double-send.
*/
int writev(int fd, const struct iovec *iov, int iovcnt)
{
    int i, sent, total = 0;

    for (i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0)
            continue;
        sent = lwip_send(fd, iov[i].iov_base, (int)iov[i].iov_len, 0);
        if (sent < 0)
            return (total > 0) ? total : sent;
        total += sent;
        if ((size_t)sent < iov[i].iov_len)
            break; // partial write: let libsmb2 resume from here
    }

    return total;
}

int readv(int fd, const struct iovec *iov, int iovcnt)
{
    int i, got, total = 0;

    for (i = 0; i < iovcnt; i++) {
        if (iov[i].iov_len == 0)
            continue;
        got = lwip_recv(fd, iov[i].iov_base, (int)iov[i].iov_len, 0);
        if (got < 0)
            return (total > 0) ? total : got;
        total += got;
        if ((size_t)got < iov[i].iov_len)
            break; // short read: caller re-enters for the rest
    }

    return total;
}

struct protoent *getprotobyname(const char *name)
{
    // libsmb2 calls this only to look up "tcp" before socket(); ps2ip has no protocol database, and
    // IPPROTO_TCP is the only answer that matters here.
    static char proto_tcp[] = "tcp";
    static struct protoent ent = {proto_tcp, NULL, 6 /* IPPROTO_TCP */};

    (void)name;
    return &ent;
}

/*
  errno.

  IOP modules have no per-thread errno. libsmb2 reads it after socket calls to distinguish
  EAGAIN/EINTR from real failures, so a single global is enough here: this driver serialises every
  SMB operation behind one semaphore (see smb2man.c), so there is never more than one call in
  flight to race over it.
*/
int errno = 0;

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    int arg;

    va_start(ap, cmd);
    arg = va_arg(ap, int);
    va_end(ap);

    return lwip_fcntl(fd, cmd, arg);
}

/*
  Share enumeration is not built (see LIBSMB2_EXCLUDE in the Makefile) -- it needs the DCE/RPC
  layer, and this driver reports an empty share list. libsmb2.c still references the entry point,
  so fail it cleanly rather than linking the whole srvsvc stack for a call OPL never makes.
*/
int smb2_share_enum_async(void *smb2, void *cb, void *cb_data)
{
    (void)smb2;
    (void)cb;
    (void)cb_data;
    return -1;
}
