/*
  smb2man -- SMB2/SMB3 browse-side filesystem driver.

  Drop-in replacement for PS2SDK's SMB1-only smbman.irx. It registers the SAME iomanX device name
  ("smb", so paths stay "smb0:...") and implements the SAME devctl interface from ps2smb.h, so
  ethsupport.c is unchanged apart from choosing which IRX to load. Exactly one of the two is loaded
  per boot, so there is never a contest for the device name.

  The SMB2 work is done by the SDK's IOP build of libsmb2 (ports_iop/lib/libsmb2.a), which speaks
  2.0.2 through 3.1.1 and carries its own AES-CCM, so signing and SMB3 come for free rather than
  needing a crypto implementation in this tree. libsmb2 links directly against the lwip_* stack OPL
  already brings up for SMB, so there is no second network stack.

  Scope note: this is the BROWSE path only -- listing games, cover art, per-game configs. The
  in-game reader is a separate, deliberately tiny client (modules/iopcore/cdvdman/smb2.c); libsmb2
  is far too large and too malloc-happy to live inside cdvdman while a game is running.
*/

// libsmb2's headers are written against C99 fixed-width types, which the IOP headers do not pull
// in on their own -- include stdint before them.
#include <stdint.h>

#include <errno.h>
#include <intrman.h>
#include <iomanX.h>
#include <loadcore.h>
#include <stdio.h>
#include <sysclib.h>
#include <sysmem.h>
#include <thbase.h>
#include <thsemap.h>

#include <ps2smb.h>
// smb2.h first: it defines SMB2_GUID_SIZE / smb2_lease_key, which libsmb2.h uses without
// including it itself.
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

IRX_ID("smb2man", 1, 1);

#define MAX_OPEN_FILES 8
#define MAX_PATH_LEN   512

static struct smb2_context *smb2 = NULL;
static int smb2_sema = -1;

// Connection parameters captured at LOGON, consumed at OPENSHARE: libsmb2 authenticates as part of
// connect_share, so there is nothing to do at logon time beyond remembering the credentials.
static char conn_server[64];
static char conn_user[256];
static char conn_password[256];
static int conn_valid = 0;
static int share_open = 0;

struct smb2man_file
{
    struct smb2fh *fh;
    int used;
};

static struct smb2man_file file_table[MAX_OPEN_FILES];

#define LOCK()   WaitSema(smb2_sema)
#define UNLOCK() SignalSema(smb2_sema)

// Unix epoch -> the packed date iox_stat_t carries. Defined below, next to getstat which documents
// why it matters; declared here because dread needs it first.
static void smb2man_pack_time(uint64_t unixsec, unsigned char *out);

/*
  Which SMB2 timestamp belongs in iox_stat_t's `ctime`. The two namespaces disagree and it is an easy
  thing to "fix" the wrong way:

    POSIX ctime  = last metadata CHANGE  -> SMB2's smb2_ctime
    PS2  ctime   = CREATION time         -> SMB2's smb2_btime (birth)

  iox_stat_t descends from the FAT/Windows lineage the PS2's filesystems use, where the three stamps
  are (creation, access, write) -- so creation is what belongs here, not change time.

  Fall back to smb2_ctime when the server leaves birth time unset: some servers do not report it, and
  a plausible-but-slightly-wrong stamp beats reporting 1970 for every file. Nothing in OPL keys off
  ctime (mtime is the load-bearing one, see getstat), so the fallback cannot mislead any logic.
*/
static uint64_t smb2man_btime(const struct smb2_stat_64 *st)
{
    return st->smb2_btime ? st->smb2_btime : st->smb2_ctime;
}

/*
  OPL addresses shares with Windows-style backslashes ("\CD\FOO.ISO", built by ethsupport's
  ethPrefix). libsmb2 wants forward slashes and no leading separator, since every path is already
  relative to the connected share. Convert in one place so no caller has to care.
*/
static void smb2man_fixpath(char *out, const char *in, int outmax)
{
    int i = 0;

    while (*in == '\\' || *in == '/')
        in++;

    while (*in != '\0' && i < outmax - 1) {
        out[i++] = (*in == '\\') ? '/' : *in;
        in++;
    }

    out[i] = '\0';
}

//-------------------------------------------------------------------------
static int smb2man_init(iop_device_t *dev)
{
    iop_sema_t smp;

    smp.initial = 1;
    smp.max = 1;
    smp.option = 0;
    smp.attr = 1;
    smb2_sema = CreateSema(&smp);

    memset(file_table, 0, sizeof(file_table));

    return 0;
}

static int smb2man_deinit(iop_device_t *dev)
{
    if (smb2_sema >= 0) {
        DeleteSema(smb2_sema);
        smb2_sema = -1;
    }

    return 0;
}

//-------------------------------------------------------------------------
static int smb2man_alloc_fd(void)
{
    int i;

    for (i = 0; i < MAX_OPEN_FILES; i++) {
        if (!file_table[i].used)
            return i;
    }

    return -1;
}

static int smb2man_open(iop_file_t *f, const char *filename, int flags, int mode)
{
    char path[MAX_PATH_LEN];
    struct smb2fh *fh;
    int slot, oflags;

    if (smb2 == NULL || !share_open)
        return -ENODEV;

    LOCK();

    slot = smb2man_alloc_fd();
    if (slot < 0) {
        UNLOCK();
        return -EMFILE;
    }

    smb2man_fixpath(path, filename, sizeof(path));

    /*
      Translate iomanX open flags into the values libsmb2 expects. These are TWO DIFFERENT
      NAMESPACES and they must never be passed through unconverted:

        incoming (iomanX / FIO, via io_common.h): RDONLY=1, WRONLY=2, RDWR=3
        libsmb2 (shim/fcntl.h, this build):       RDONLY=0, WRONLY=1, RDWR=2, ACCMODE=3

      Passing the incoming value straight through sends FIO_O_RDONLY(1) as libsmb2's O_WRONLY, and
      worse, a plain `flags & O_RDWR` test misreads every read-only open as read-write because
      FIO_O_RDWR(3) is a bit-superset of FIO_O_RDONLY(1) -- yielding 3, which matches no case in
      libsmb2's `switch (flags & O_ACCMODE)` and leaves DesiredAccess at 0. The server then rejects
      every CREATE and the whole share reads as empty. (PS2SDK's own SMB1 smbman guards this with
      the same `(mode & O_RDWR) == O_RDWR` shape, for the same reason.)

      Literal values are used deliberately: BOTH namespaces' O_* macros are reachable from this
      translation unit, so naming them here would be ambiguous to a reader and fragile to an
      include-order change. LIBSMB2_O_* below are libsmb2's, matching shim/fcntl.h.
    */
#define LIBSMB2_O_RDONLY 0x0000
#define LIBSMB2_O_WRONLY 0x0001
#define LIBSMB2_O_RDWR   0x0002
#define LIBSMB2_O_CREAT  0x0200
#define LIBSMB2_O_TRUNC  0x0400
#define LIBSMB2_O_EXCL   0x0800

    if ((flags & FIO_O_RDWR) == FIO_O_RDWR)
        oflags = LIBSMB2_O_RDWR;
    else if (flags & FIO_O_WRONLY)
        oflags = LIBSMB2_O_WRONLY;
    else
        oflags = LIBSMB2_O_RDONLY;

    /*
      Carry the creation flags through. libsmb2 derives its SMB2 CREATE disposition purely from
      these, so dropping them means (a) O_CREAT writes can never make a new file -- every per-game
      CFG save, favourites.bin write and VMC create fails with ENOENT -- and (b) an O_TRUNC rewrite
      leaves the old tail in place when the new content is shorter, producing a half-new/half-old
      config that parses stale keys on the next boot. The FIO and libsmb2 values coincide for these
      three (0x200/0x400/0x800), but they are mapped explicitly rather than relied upon.
    */
    if (flags & FIO_O_CREAT)
        oflags |= LIBSMB2_O_CREAT;
    if (flags & FIO_O_TRUNC)
        oflags |= LIBSMB2_O_TRUNC;
    if (flags & FIO_O_EXCL)
        oflags |= LIBSMB2_O_EXCL;

    fh = smb2_open(smb2, path, oflags);
    if (fh == NULL) {
        UNLOCK();
        return -ENOENT;
    }

    file_table[slot].fh = fh;
    file_table[slot].used = 1;
    f->privdata = (void *)slot;

    UNLOCK();

    return slot;
}

static int smb2man_close(iop_file_t *f)
{
    int slot = (int)f->privdata;

    if (slot < 0 || slot >= MAX_OPEN_FILES || !file_table[slot].used)
        return -EBADF;

    LOCK();

    smb2_close(smb2, file_table[slot].fh);
    file_table[slot].fh = NULL;
    file_table[slot].used = 0;

    UNLOCK();

    return 0;
}

static int smb2man_read(iop_file_t *f, void *buffer, int size)
{
    int slot = (int)f->privdata;
    int rv;

    if (slot < 0 || slot >= MAX_OPEN_FILES || !file_table[slot].used)
        return -EBADF;

    LOCK();
    rv = smb2_read(smb2, file_table[slot].fh, (uint8_t *)buffer, size);
    UNLOCK();

    return (rv < 0) ? -EIO : rv;
}

static int smb2man_write(iop_file_t *f, void *buffer, int size)
{
    int slot = (int)f->privdata;
    int rv;

    if (slot < 0 || slot >= MAX_OPEN_FILES || !file_table[slot].used)
        return -EBADF;

    LOCK();
    rv = smb2_write(smb2, file_table[slot].fh, (uint8_t *)buffer, size);
    UNLOCK();

    return (rv < 0) ? -EIO : rv;
}

static int smb2man_lseek(iop_file_t *f, int offset, int whence)
{
    int slot = (int)f->privdata;
    uint64_t pos = 0;
    int rv;

    if (slot < 0 || slot >= MAX_OPEN_FILES || !file_table[slot].used)
        return -EBADF;

    LOCK();
    rv = smb2_lseek(smb2, file_table[slot].fh, (int64_t)offset, whence, &pos);
    UNLOCK();

    return (rv < 0) ? -EIO : (int)pos;
}

/*
  64-bit seek. OPL's ISOs routinely exceed 2 GB, so the 32-bit lseek above cannot address them --
  iomanX calls this variant for those, and without it large games are unreachable over the browse
  path even though the in-game reader handles them fine.
*/
static s64 smb2man_lseek64(iop_file_t *f, s64 offset, int whence)
{
    int slot = (int)f->privdata;
    uint64_t pos = 0;
    int rv;

    if (slot < 0 || slot >= MAX_OPEN_FILES || !file_table[slot].used)
        return -EBADF;

    LOCK();
    rv = smb2_lseek(smb2, file_table[slot].fh, (int64_t)offset, whence, &pos);
    UNLOCK();

    return (rv < 0) ? -EIO : (s64)pos;
}

//-------------------------------------------------------------------------
// Directory enumeration. iomanX hands out one dirent per dread() call, so the smb2dir handle is
// kept open across calls and walked one entry at a time.
static int smb2man_dopen(iop_file_t *f, const char *dirname)
{
    char path[MAX_PATH_LEN];
    struct smb2dir *dir;

    if (smb2 == NULL || !share_open)
        return -ENODEV;

    LOCK();

    smb2man_fixpath(path, dirname, sizeof(path));

    dir = smb2_opendir(smb2, path);
    if (dir == NULL) {
        UNLOCK();
        return -ENOENT;
    }

    f->privdata = (void *)dir;

    UNLOCK();

    return 0;
}

static int smb2man_dclose(iop_file_t *f)
{
    struct smb2dir *dir = (struct smb2dir *)f->privdata;

    if (dir == NULL)
        return -EBADF;

    LOCK();
    smb2_closedir(smb2, dir);
    UNLOCK();

    f->privdata = NULL;

    return 0;
}

static int smb2man_dread(iop_file_t *f, iox_dirent_t *dirent)
{
    struct smb2dir *dir = (struct smb2dir *)f->privdata;
    struct smb2dirent *ent;

    if (dir == NULL)
        return -EBADF;

    LOCK();

    ent = smb2_readdir(smb2, dir);
    if (ent == NULL) {
        UNLOCK();
        return 0; // end of directory
    }

    memset(dirent, 0, sizeof(iox_dirent_t));
    strncpy(dirent->name, ent->name, sizeof(dirent->name) - 1);
    dirent->name[sizeof(dirent->name) - 1] = '\0';

    dirent->stat.mode = (ent->st.smb2_type == SMB2_TYPE_DIRECTORY) ? FIO_S_IFDIR : FIO_S_IFREG;
    dirent->stat.size = (unsigned int)ent->st.smb2_size;
    dirent->stat.hisize = (unsigned int)(ent->st.smb2_size >> 32);
    // Same reason as getstat: a listing that reports every entry as epoch-zero breaks anything that
    // compares timestamps, and costs nothing to fill correctly.
    smb2man_pack_time(smb2man_btime(&ent->st), dirent->stat.ctime);
    smb2man_pack_time(ent->st.smb2_atime, dirent->stat.atime);
    smb2man_pack_time(ent->st.smb2_mtime, dirent->stat.mtime);

    UNLOCK();

    return 1;
}

/*
  Convert a Unix epoch time to the packed date iox_stat_t carries in ctime/atime/mtime:

      [0] unused   [1] second   [2] minute   [3] hour   [4] day   [5] month   [6..7] year (LE)

  (The layout is confirmed by OPL's own reader at src/opl.c, which unpacks exactly these offsets to
  set the PS2 clock.)

  NOT cosmetic. ethsupport.c's change detector compares stat("<prefix>CD").st_mtime against the
  previous value to decide whether the game list needs rebuilding. While these bytes were left zeroed
  st_mtime was always 0, so the comparison never fired and OPL could not notice a game being ADDED to
  an SMB2 share -- the list only refreshed if the total size happened to change. SMB1's smbman fills
  them, so this is dialect parity.

  32-bit math on purpose: Unix seconds fit in u32 until 2106, and staying out of 64-bit division
  avoids dragging __udivdi3 into more of this module than necessary.

  Timezone: SMB reports UTC and this converts as-is, matching what the SMB1 path does. The consumer
  only ever tests these for INEQUALITY, so a fixed offset is immaterial to it.
*/
static void smb2man_pack_time(uint64_t unixsec, unsigned char *out)
{
    u32 t = (u32)unixsec;
    u32 days = t / 86400u;
    u32 rem = t % 86400u;
    u32 y, m, d;

    out[0] = 0;
    out[1] = (unsigned char)(rem % 60u);
    out[2] = (unsigned char)((rem / 60u) % 60u);
    out[3] = (unsigned char)(rem / 3600u);

    // Civil date from days since 1970-01-01, via the era-based algorithm (Howard Hinnant's
    // civil_from_days). Shift the epoch to 0000-03-01 so leap days land at the end of the cycle.
    {
        u32 z = days + 719468u;
        u32 era = z / 146097u;
        u32 doe = z - era * 146097u;                                         // [0, 146096]
        u32 yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u; // [0, 399]
        u32 doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);                // [0, 365]
        u32 mp = (5u * doy + 2u) / 153u;                                     // [0, 11], March = 0

        d = doy - (153u * mp + 2u) / 5u + 1u;
        m = mp + (mp < 10u ? 3u : (u32)-9);
        y = yoe + era * 400u + (m <= 2u ? 1u : 0u);
    }

    out[4] = (unsigned char)d;
    out[5] = (unsigned char)m;
    out[6] = (unsigned char)(y & 0xff);
    out[7] = (unsigned char)((y >> 8) & 0xff);
}

static int smb2man_getstat(iop_file_t *f, const char *filename, iox_stat_t *stat)
{
    char path[MAX_PATH_LEN];
    struct smb2_stat_64 st;
    int rv;

    if (smb2 == NULL || !share_open)
        return -ENODEV;

    LOCK();

    smb2man_fixpath(path, filename, sizeof(path));
    rv = smb2_stat(smb2, path, &st);

    if (rv < 0) {
        UNLOCK();
        return -ENOENT;
    }

    memset(stat, 0, sizeof(iox_stat_t));
    stat->mode = (st.smb2_type == SMB2_TYPE_DIRECTORY) ? FIO_S_IFDIR : FIO_S_IFREG;
    stat->size = (unsigned int)st.smb2_size;
    stat->hisize = (unsigned int)(st.smb2_size >> 32);
    smb2man_pack_time(smb2man_btime(&st), stat->ctime);
    smb2man_pack_time(st.smb2_atime, stat->atime);
    smb2man_pack_time(st.smb2_mtime, stat->mtime);

    UNLOCK();

    return 0;
}

//-------------------------------------------------------------------------
// Does `name` already exist as a directory on the share? Used to tell "mkdir failed because it is
// already there" (benign, and the common case) from a real failure. Takes the lock itself, so it
// must only be called with the lock RELEASED.
static int smb2man_stat_exists(const char *name)
{
    char path[MAX_PATH_LEN];
    struct smb2_stat_64 st;
    int rv;

    LOCK();

    smb2man_fixpath(path, name, sizeof(path));
    rv = smb2_stat(smb2, path, &st);

    UNLOCK();

    return (rv == 0 && st.smb2_type == SMB2_TYPE_DIRECTORY);
}

// OPL calls sbCreateFolders() on every activated device root (ethsupport.c), which issues a burst
// of mkdir()s for CFG/THM/LNG/ART/CD/DVD/VMC. While these were nulldev stubs returning -EIO, an
// SMB2 share got NONE of them: per-game configs, cover art and themes had nowhere to live unless
// the user happened to have created the folders by hand, and OPL logged a failure for each. The
// SMB1 smbman has always implemented these, so this is dialect parity, not new surface.
static int smb2man_mkdir(iop_file_t *f, const char *dirname, int mode)
{
    char path[MAX_PATH_LEN];
    int rv;

    (void)mode; // no POSIX mode bits over SMB

    if (smb2 == NULL || !share_open)
        return -ENODEV;

    LOCK();

    smb2man_fixpath(path, dirname, sizeof(path));
    rv = smb2_mkdir(smb2, path);

    UNLOCK();

    // EEXIST is the normal steady state -- sbCreateFolders re-runs on every device refresh and
    // must not be made to look like a failure once the folders are already there.
    if (rv < 0)
        return (smb2man_stat_exists(dirname)) ? 0 : -EIO;

    return 0;
}

static int smb2man_rmdir(iop_file_t *f, const char *dirname)
{
    char path[MAX_PATH_LEN];
    int rv;

    if (smb2 == NULL || !share_open)
        return -ENODEV;

    LOCK();

    smb2man_fixpath(path, dirname, sizeof(path));
    rv = smb2_rmdir(smb2, path);

    UNLOCK();

    return (rv < 0) ? -EIO : 0;
}

static int smb2man_remove(iop_file_t *f, const char *filename)
{
    char path[MAX_PATH_LEN];
    int rv;

    if (smb2 == NULL || !share_open)
        return -ENODEV;

    LOCK();

    smb2man_fixpath(path, filename, sizeof(path));
    rv = smb2_unlink(smb2, path);

    UNLOCK();

    return (rv < 0) ? -EIO : 0;
}

static int smb2man_rename(iop_file_t *f, const char *oldname, const char *newname)
{
    char oldpath[MAX_PATH_LEN];
    char newpath[MAX_PATH_LEN];
    int rv;

    if (smb2 == NULL || !share_open)
        return -ENODEV;

    LOCK();

    smb2man_fixpath(oldpath, oldname, sizeof(oldpath));
    smb2man_fixpath(newpath, newname, sizeof(newpath));
    rv = smb2_rename(smb2, oldpath, newpath);

    UNLOCK();

    return (rv < 0) ? -EIO : 0;
}

//-------------------------------------------------------------------------
static void smb2man_teardown(void)
{
    if (smb2 != NULL) {
        if (share_open)
            smb2_disconnect_share(smb2);
        smb2_destroy_context(smb2);
        smb2 = NULL;
    }

    share_open = 0;
}

static int smb2man_devctl(iop_file_t *f, const char *name, int cmd, void *arg, unsigned int arglen,
                          void *buf, unsigned int buflen)
{
    switch (cmd) {
        case SMB_DEVCTL_GETPASSWORDHASHES: {
            /*
              SMB1 needed OPL to pre-hash the password because the LM/NTLM responses were built on
              the EE side. libsmb2 owns the whole NTLMSSP exchange and wants the plaintext password,
              so there is nothing to hash here. Report failure so ethsupport keeps using
              PLAINTEXT_PASSWORD -- "plaintext" meaning only that WE pass it to libsmb2 verbatim;
              it still goes out as an NTLM response, never in the clear.
            */
            return -1;
        }

        case SMB_DEVCTL_LOGON: {
            smbLogOn_in_t *logon = (smbLogOn_in_t *)arg;

            if (arg == NULL || arglen < sizeof(smbLogOn_in_t))
                return -EINVAL;

            LOCK();
            smb2man_teardown();

            /*
              The PORT must be folded into the server string. libsmb2 has no smb2_set_port(); its
              only channel is the "<host>[:<port>]" form documented at libsmb2.h, and a bare host
              defaults to 445 inside socket.c. Ignoring logon->serverPort would therefore always
              dial 445 -- while RiptOPL's own shipped default is 1111 (the PC server tool in
              pc/smbserver/ listens there because Windows still occupies 445). So the common case
              would silently connect to the wrong port, or to a Windows share the user did not mean.
            */
            if (logon->serverPort > 0 && logon->serverPort != 445)
                sprintf(conn_server, "%.48s:%d", logon->serverIP, logon->serverPort);
            else
                snprintf(conn_server, sizeof(conn_server), "%s", logon->serverIP);
            conn_server[sizeof(conn_server) - 1] = '\0';

            strncpy(conn_user, logon->User, sizeof(conn_user) - 1);
            conn_user[sizeof(conn_user) - 1] = '\0';

            /*
              Honour PasswordType. ethsupport.c declares smbLogOn_in_t as an UNINITIALISED stack
              local and, on the no-password path, sets only User and PasswordType -- it never
              touches logon->Password. Copying that field unconditionally would hand libsmb2 256
              bytes of uninitialised stack as the password, which is both a garbage credential (so
              guest/no-password shares fail to authenticate) and a leak of whatever the EE stack
              happened to hold. That is OPL's SHIPPED DEFAULT configuration, so it is the common
              case, not an edge case.

              HASHED_PASSWORD cannot be honoured at all: libsmb2 runs the whole NTLMSSP exchange
              itself and needs the plaintext. GETPASSWORDHASHES already returns failure so that
              ethsupport falls back to PLAINTEXT_PASSWORD; treat anything else as "no password"
              rather than feeding libsmb2 a 32-byte binary hash it would use as literal text.
            */
            if (logon->PasswordType == PLAINTEXT_PASSWORD) {
                strncpy(conn_password, logon->Password, sizeof(conn_password) - 1);
                conn_password[sizeof(conn_password) - 1] = '\0';
            } else {
                conn_password[0] = '\0';
            }
            conn_valid = 1;

            /*
              libsmb2 authenticates during connect_share, so a session cannot be established until
              a share name is known. Build the context now and validate it, but defer the actual
              connect to OPENSHARE. A context allocation failure is the only thing that can be
              reported at this stage.
            */
            smb2 = smb2_init_context();
            if (smb2 == NULL) {
                conn_valid = 0;
                UNLOCK();
                return -SMB_DEVCTL_LOGON_ERR_CONN;
            }

            smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
            // ANY lets libsmb2 negotiate the best dialect the server offers, 2.0.2 through 3.1.1.
            smb2_set_version(smb2, SMB2_VERSION_ANY2);
            /*
              A timeout is mandatory here, not a nicety. libsmb2's synchronous wait loop only calls
              smb2_timeout_pdus() when ->timeout is non-zero, and the context is calloc'd, so the
              default of 0 means "wait forever". Every call in this driver runs with smb2_sema held,
              on OPL's single IO worker -- so one unresponsive or half-dead server does not merely
              stall the share, it wedges the worker with the lock held and takes the whole UI with
              it. That is the exact failure class as the bounded MMCE wait and the bounded connect
              retry: a network device must fail, not hang.
            */
            smb2_set_timeout(smb2, 15);

            UNLOCK();
            return 0;
        }

        case SMB_DEVCTL_LOGOFF: {
            LOCK();
            smb2man_teardown();
            conn_valid = 0;
            UNLOCK();
            return 0;
        }

        case SMB_DEVCTL_OPENSHARE: {
            smbOpenShare_in_t *openshare = (smbOpenShare_in_t *)arg;
            int rv;

            if (arg == NULL || arglen < sizeof(smbOpenShare_in_t))
                return -EINVAL;
            if (!conn_valid || smb2 == NULL)
                return -ENODEV;

            LOCK();

            if (share_open) {
                smb2_disconnect_share(smb2);
                share_open = 0;
            }

            smb2_set_password(smb2, conn_password);

            rv = smb2_connect_share(smb2, conn_server, openshare->ShareName, conn_user);
            if (rv < 0) {
                UNLOCK();
                return -SMB_DEVCTL_LOGON_ERR_LOGON;
            }

            share_open = 1;

            UNLOCK();
            return 0;
        }

        case SMB_DEVCTL_CLOSESHARE: {
            LOCK();
            if (share_open && smb2 != NULL) {
                smb2_disconnect_share(smb2);
                share_open = 0;
            }
            UNLOCK();
            return 0;
        }

        case SMB_DEVCTL_ECHO: {
            /*
              smbman used SMB_COM_ECHO as a liveness probe between logon and open-share. libsmb2
              exposes no echo, and it has no session to probe at this point anyway (see LOGON).
              Report success: the connection is genuinely validated a moment later by OPENSHARE,
              which is the call whose failure ethsupport actually surfaces to the user.
            */
            return 0;
        }

        case SMB_DEVCTL_GETSHARELIST: {
            /*
              NOT SUPPORTED on SMB2, and this reports it rather than pretending.

              Enumerating shares needs the srvsvc DCE/RPC pipe. libsmb2 has the coders for it
              (libsmb2-dcerpc-srvsvc.c), but that file is a thin wrapper that #includes
              "../libdcerpc/dcerpc-srvsvc.c" -- an upstream subtree that is NOT part of lib/ and was
              therefore not vendored here. Wiring it up means vendoring a second tree and putting it
              through the same GPREL/relocation gauntlet this module already needed, which is a
              project rather than a loose end.

              Returning 0 ("zero shares") was worse than useless: an empty picker is indistinguishable
              from a server that genuinely exposes nothing, so the user is left guessing whether the
              connection failed. -EINVAL makes ethsupport report the browse as unsupported, and the
              user types the share name -- which is the ONLY path that ever mattered anyway: listing
              and launching games use the configured share name and never touch srvsvc.
            */
            return -EINVAL;
        }

        case SMB_DEVCTL_QUERYDISKINFO: {
            // Only used for free-space display, which OPL does not show for SMB.
            return -1;
        }
    }

    return -EINVAL;
}

//-------------------------------------------------------------------------
static int smb2man_nulldev(void)
{
    return -EIO;
}

static iop_device_ops_t smb2man_ops = {
    &smb2man_init,
    &smb2man_deinit,
    (void *)&smb2man_nulldev, // format
    &smb2man_open,
    &smb2man_close,
    &smb2man_read,
    &smb2man_write,
    &smb2man_lseek,
    (void *)&smb2man_nulldev, // ioctl
    &smb2man_remove,
    &smb2man_mkdir,
    &smb2man_rmdir,
    &smb2man_dopen,
    &smb2man_dclose,
    &smb2man_dread,
    &smb2man_getstat,
    (void *)&smb2man_nulldev, // chstat
    &smb2man_rename,
    (void *)&smb2man_nulldev, // chdir
    (void *)&smb2man_nulldev, // sync
    (void *)&smb2man_nulldev, // mount
    (void *)&smb2man_nulldev, // umount
    (void *)&smb2man_lseek64,
    &smb2man_devctl,
    (void *)&smb2man_nulldev, // symlink
    (void *)&smb2man_nulldev, // readlink
    (void *)&smb2man_nulldev, // ioctl2
};

// Same device name as smbman, so every existing "smb0:" path keeps working untouched.
static iop_device_t smb2man_dev = {
    "smb",
    IOP_DT_FS | IOP_DT_FSEXT,
    1,
    "SMB2/SMB3 filesystem driver",
    &smb2man_ops,
};

int _start(int argc, char **argv)
{
    DelDrv(smb2man_dev.name);

    if (AddDrv(&smb2man_dev) != 0)
        return MODULE_NO_RESIDENT_END;

    return MODULE_RESIDENT_END;
}
