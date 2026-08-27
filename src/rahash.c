/*
  RA: the image hash, computed the way RetroAchievements computes it.

  Algorithm (rc_hash_ps2 in rcheevos, hash_disc.c):
    1. open the image and read SYSTEM.CNF
    2. take the boot executable name from BOOT2, e.g. "SLUS_210.65"
    3. MD5( ASCII(name) || contents of that file )

  Half of the work already exists in OPL: ps2cnf.c parses SYSTEM.CNF and
  GetStartupExecName extracts the name and strips "cdrom0:\". What
  remains is reading the file itself and running MD5 over it.

  Hashing on the console means the images themselves decide which games
  are supported; nothing on the PC has to be prepared or kept in sync.
*/

/* The include order mirrors supportbase.c, where fileXioMount is
   already in use and its declaration arrives through the chain.
   Including <fileXio_rpc.h> directly in this port hits a #error about
   newlib. */
#include "include/opl.h"
#include "include/util.h"
#include "include/iosupport.h"
#include "include/system.h"
#include "include/supportbase.h"
#include "include/ioman.h"
#include "include/md5.h"
#include "include/rahash.h"

/* NEWLIB_PORT_AWARE lifts the #error about calling fileXio directly.
   supportbase.c and hddsupport.c do the same: the calls are deliberate,
   the POSIX wrapper cannot mount images. */
#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h> // fileXioMount("iso:", ***), fileXioUmount
#include <io_common.h>   // FIO_MT_RDONLY
#include <ps2sdkapi.h>   // lseek64: images can exceed 2 GB

/* Same cap as rcheevos: no boot executable is larger */
#define RA_HASH_MAX_EXEC (64 * 1024 * 1024)
#define RA_HASH_CHUNK    (64 * 1024)

static char g_chunk[RA_HASH_CHUNK] __attribute__((aligned(64)));

/* Crumbs: where to report each step. Set by the caller. A hang on USB
   is silent, and the return code alone cannot tell mounting from
   reading. */
static ra_step_fn g_step = NULL;

void raHashSetStepLog(ra_step_fn fn)
{
    g_step = fn;
}

static void step(const char *what)
{
    if (g_step != NULL)
        g_step(what);
}

/* ------------------------------------------------------------------ */
/* Direct image read, no mounting                                      */

/*
  Mounting through "iso:" on USB hangs on the first read when the boot
  executable sits beyond the 2 GB mark of the image, past the reach of
  a signed 32-bit offset. The same image works from the share.

  So we bypass the driver: open the image as a plain file, walk the
  ISO9660 directory ourselves and seek with 64-bit lseek64.

  This also reads less than mounting does: one volume descriptor
  sector, the root directory (usually a couple of kilobytes) and the
  executable itself.
*/

#define ISO_SECTOR  2048
#define ISO_PVD_LBA 16

/* Not a real descriptor: read_at() routes it to the optical drive. */
#define RA_SRC_DISC_FD (-100)

static unsigned int le32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* The reads below go either to an image file or to the disc in the
   tray. One choke point, two backends: everything above it (the
   ISO9660 walk, the MD5) does not care which. A real disc is the
   cheaper source -- no mounting, no 2 GB offset problem -- and in the
   menu the IOP still runs the console's own CDVDMAN, so cdrom0: is the
   drive. */
/* A crumb with numbers in it. The drive tells us why it refused only
   through sceCdGetError, and a bare step name hid that once already. */
static void step_num(const char *what, unsigned int a, unsigned int b, int c)
{
    char line[64];

    snprintf(line, sizeof(line), "%s lba=%u n=%u rv=%d", what, a, b, c);
    step(line);
}

/*
  One read off the drive, the way ps2sdk's cdfs does it -- the driver
  uLaunchELF uses to browse and copy from real game DVDs
  (src/ps2sdk-ref/iop/cdvd/cdfs/src/cdfs_iop.c, cdfs_readSect):

    - up to 32 attempts, the first 8 at SCECdSpinNom ("start at max
      speed, slow down on errors"), the rest at SCECdSpinStm;
    - trycount 32, not 0: it is the retry budget the IOP driver gets
      per call;
    - sceCdDiskReady(0) before every attempt.

  Why bother: the ROM cdvdman's own read-error recovery
  (src/ps2sdk-ref/iop/cdvd/cdvdman/src/cdvdman.c, ~3936) is skipped
  for DVDs (`!s->m_dvd_flag`), so on a DVD a single sceCdRead that
  hits a bad spot just fails. cdfs retries above the driver for that
  reason. The first hardware run failed exactly like this: sector 16
  and the root directory read, an extent 1.69M sectors in did not
  (SCECdErREAD), and the ROM's cdrom0: file read of the same file came
  back empty too.

  We cap the attempts lower than cdfs (16, half at each speed): a truly
  unreadable spot costs seconds per attempt, and the menu is waiting.
*/
#define DISC_READ_TRIES     16
#define DISC_READ_NOM_TRIES 8

static int disc_read_sectors(unsigned int lba, unsigned int count, void *buf)
{
    sceCdRMode mode;
    int attempt, rv, err = SCECdErNO;

    mode.trycount = 32;
    mode.datapattern = SCECdSecS2048;
    mode.pad = 0;

    for (attempt = 0; attempt < DISC_READ_TRIES; attempt++) {
        mode.spindlctrl = attempt < DISC_READ_NOM_TRIES ? SCECdSpinNom : SCECdSpinStm;

        sceCdDiskReady(0);

        rv = sceCdRead(lba, count, buf, &mode);
        if (!rv) {
            /* Refused before it started: drive not ready or a command
               still pending. Worth another go after DiskReady. */
            err = -1;
            continue;
        }

        sceCdSync(0);

        err = sceCdGetError();
        if (err == SCECdErNO) {
            if (attempt > 0)
                step_num("cd-read-ok-after-retries", lba, count, attempt);
            return 0;
        }
    }

    step_num("cd-read-error", lba, count, err);
    return -1;
}

/*
  Whole sectors, straight into the caller's buffer.

  Every read this file makes off a disc starts on a sector boundary,
  and every destination has room for the rounded-up size: g_chunk is
  64 KB, and the volume descriptor buffer is one sector and asks for
  exactly one. So there is no need for an intermediate buffer, and no
  reason to hand the drive a shape it has not been seen to accept --
  the first version read short tails into a 2 KB staging buffer and
  that was the one call the drive refused on hardware.

  The caller gets back the length it asked for; the sectors past it sit
  in the buffer unused.
*/
static int disc_read_at(long long off, void *buf, int len)
{
    unsigned int lba, sectors;

    if (len <= 0)
        return -1;

    if ((off % ISO_SECTOR) != 0) { /* never happens; a guard, not logic */
        step_num("cd-read-unaligned", (unsigned int)off, (unsigned int)len, 0);
        return -1;
    }

    lba = (unsigned int)(off / ISO_SECTOR);
    sectors = (unsigned int)((len + ISO_SECTOR - 1) / ISO_SECTOR);

    if (disc_read_sectors(lba, sectors, buf) != 0)
        return -1;

    return len;
}

static int read_at(int fd, long long off, void *buf, int len)
{
    if (fd == RA_SRC_DISC_FD)
        return disc_read_at(off, buf, len);

    if (lseek64(fd, off, SEEK_SET) < 0)
        return -1;

    return read(fd, buf, len);
}

/* Finds a directory entry. Returns 0 and fills lba/size. */
static int find_entry(int fd, unsigned int dir_lba, unsigned int dir_size,
                      const char *want, unsigned int *out_lba, unsigned int *out_size)
{
    unsigned int done = 0;

    while (done < dir_size) {
        int got = read_at(fd, (long long)(dir_lba)*ISO_SECTOR + done, g_chunk, ISO_SECTOR);
        int p = 0;

        if (got != ISO_SECTOR)
            return -1;

        while (p < ISO_SECTOR) {
            unsigned char *rec = (unsigned char *)&g_chunk[p];
            int len = rec[0];
            int namelen;

            if (len == 0)
                break; /* entries never cross a sector boundary */

            namelen = rec[32];

            /* The name in the image carries a version: "SLUS_210.65;1".
               Compare up to the semicolon so the version does not
               matter. */
            if (namelen > 0) {
                int i, same = 1;

                for (i = 0; i < namelen && want[i] != '\0'; i++) {
                    if (rec[33 + i] != (unsigned char)want[i]) {
                        same = 0;
                        break;
                    }
                }

                if (same && want[i] == '\0' &&
                    (i == namelen || rec[33 + i] == ';')) {
                    *out_lba = le32(&rec[2]);
                    *out_size = le32(&rec[10]);
                    return 0;
                }
            }

            p += len;
        }

        done += ISO_SECTOR;
    }

    return -2;
}

/* Root directory of the volume. Both sources start here. */
static int read_root(int fd, unsigned int *root_lba, unsigned int *root_size)
{
    static unsigned char pvd[ISO_SECTOR] __attribute__((aligned(64)));

    /* Primary volume descriptor: sector 16, "CD001" at offset 1 */
    if (read_at(fd, (long long)ISO_PVD_LBA * ISO_SECTOR, pvd, ISO_SECTOR) != ISO_SECTOR ||
        pvd[1] != 'C' || pvd[2] != 'D' || pvd[3] != '0' || pvd[4] != '0' || pvd[5] != '1') {
        step("2-no-cd001");
        return -1;
    }
    step("2-cd001-found");

    /* The root directory record lives inside the descriptor at offset 156 */
    *root_lba = le32(&pvd[156 + 2]);
    *root_size = le32(&pvd[156 + 10]);
    step_num("2-root", *root_lba, *root_size, 0);

    return 0;
}

/* MD5( name || contents of that file ), the way RetroAchievements does
   it. Shared by the image and the disc. */
static int hash_boot_exec(int fd, const char *startup, char *out33)
{
    md5_state_t md5;
    md5_byte_t digest[16];
    unsigned int root_lba, root_size, elf_lba, elf_size, left;
    long long off;
    int i;

    if (read_root(fd, &root_lba, &root_size) != 0)
        return -2;

    if (find_entry(fd, root_lba, root_size, startup, &elf_lba, &elf_size) != 0) {
        step("3-elf-not-in-directory");
        return -3;
    }
    step("3-elf-found");

    if (elf_size == 0 || elf_size > RA_HASH_MAX_EXEC) {
        step("3-odd-elf-size");
        return -4;
    }

    md5_init(&md5);
    md5_append(&md5, (const md5_byte_t *)startup, (int)strlen(startup));

    off = (long long)elf_lba * ISO_SECTOR;
    left = elf_size;
    step("4-reading-elf-directly");

    while (left > 0) {
        int want = left > RA_HASH_CHUNK ? RA_HASH_CHUNK : (int)left;
        int got = read_at(fd, off, g_chunk, want);

        if (got <= 0) {
            step("4-read-broke-off");
            return -5;
        }

        md5_append(&md5, (const md5_byte_t *)g_chunk, got);
        off += got;
        left -= (unsigned int)got;
    }

    md5_finish(&md5, digest);

    for (i = 0; i < 16; i++) {
        static const char hex[] = "0123456789abcdef";

        out33[i * 2] = hex[(digest[i] >> 4) & 0xF];
        out33[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    out33[32] = '\0';

    step("5-hashed-directly");
    LOG("RA: direct hash %s = %s (%u bytes)\n", startup, out33, elf_size);

    return 0;
}

/*
  RA: the disc through the console's own file system driver.

  In the menu the IOP still runs the ROM's CDVDMAN and CDVDFSV, so
  cdrom0: is an ordinary path. That is how OPL launches an app listed
  as `cdrom0:\\NAME;1`, and how neutrino and wLaunchELF read
  SYSTEM.CNF off a disc.

  We tried walking ISO9660 ourselves first, the way the image hasher
  does, and on a real DVD it pointed at a sector the drive refused
  (`cd-read-error ... rv=48`, SCECdErREAD, for an extent 1.69M sectors
  in). The driver knows the disc's layout; our walk only knows what we
  taught it. So: files first, the raw walk only as a fallback.
*/
static int disc_open(const char *name)
{
    char path[64];
    int fd;

    snprintf(path, sizeof(path), "cdrom0:\\%s;1", name);
    fd = open(path, O_RDONLY);
    if (fd >= 0)
        return fd;

    /* Some drivers want the name without the version */
    snprintf(path, sizeof(path), "cdrom0:\\%s", name);
    return open(path, O_RDONLY);
}

/* MD5( name || the whole file ), reading to the end: an open file needs
   no directory entry and no size. */
static int hash_stream(int fd, const char *startup, char *out33)
{
    md5_state_t md5;
    md5_byte_t digest[16];
    unsigned int total = 0;
    int i;

    md5_init(&md5);
    md5_append(&md5, (const md5_byte_t *)startup, (int)strlen(startup));

    for (;;) {
        int got = read(fd, g_chunk, RA_HASH_CHUNK);

        if (got < 0)
            return -5;

        if (got == 0)
            break;

        md5_append(&md5, (const md5_byte_t *)g_chunk, got);
        total += (unsigned int)got;

        if (total > RA_HASH_MAX_EXEC)
            return -4;
    }

    if (total == 0)
        return -4;

    md5_finish(&md5, digest);

    for (i = 0; i < 16; i++) {
        static const char hex[] = "0123456789abcdef";

        out33[i * 2] = hex[(digest[i] >> 4) & 0xF];
        out33[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    out33[32] = '\0';

    step_num("5-hashed-from-file", total, 0, 0);
    LOG("RA: hash %s = %s (%u bytes, via cdrom0:)\n", startup, out33, total);

    return 0;
}

/* BOOT2 out of a SYSTEM.CNF already in memory. */
static int parse_boot2(char *cnf, char *out, int max)
{
    char *p, *q;
    int len;

    /* BOOT2 = cdrom0:\SLUS_210.65;1 */
    p = strstr(cnf, "BOOT2");
    if (p == NULL) {
        step("2-no-boot2");
        return -5;
    }

    p = strchr(p, '=');
    if (p == NULL)
        return -6;

    p++;
    while (*p == ' ' || *p == '\t')
        p++;

    /* Strip the device part, keep the name */
    q = strchr(p, '\\');
    if (q != NULL)
        p = q + 1;
    else if ((q = strchr(p, ':')) != NULL)
        p = q + 1;

    /* Cut at the version, the end of line or trailing blanks */
    for (len = 0; p[len] != '\0'; len++) {
        if (p[len] == ';' || p[len] == '\r' || p[len] == '\n' ||
            p[len] == ' ' || p[len] == '\t')
            break;
    }

    if (len <= 0 || len >= max)
        return -7;

    memcpy(out, p, len);
    out[len] = '\0';

    LOG("RA: disc boot file %s\n", out);
    step("3-boot-file-from-disc");

    return 0;
}

/* RA: the disc in the tray. In the menu the IOP still runs the
   console's own CDVDMAN, so this reads the real drive; the same walk,
   the same hash, and none of the image-file trouble. */
int raHashDisc(const char *startup, char *out33)
{
    int fd, ret;

    out33[0] = '\0';
    step("1-reading-disc");

    fd = disc_open(startup);
    if (fd >= 0) {
        ret = hash_stream(fd, startup, out33);
        close(fd);

        if (ret == 0)
            return 0;

        step_num("4-file-read-broke-off", 0, 0, ret);
    } else {
        step("4-cannot-open-exec-on-disc");
    }

    /* Fallback: walk the disc ourselves. Known to point at unreadable
       extents on some discs, but costs nothing to try. */
    return hash_boot_exec(RA_SRC_DISC_FD, startup, out33);
}

/* RA: BOOT2 out of the disc's own SYSTEM.CNF, e.g. "SLUS_210.65".
   OPL's ps2cnf.c wants a path it can open; here there is no file
   system, only sectors, so the two lines of parsing live here. */
int raDiscBootFile(char *out, int max)
{
    unsigned int root_lba, root_size, cnf_lba, cnf_size;
    char cnf[1024 + 1];
    int fd, got;

    out[0] = '\0';

    /* The console's own driver first. */
    fd = disc_open("SYSTEM.CNF");
    if (fd >= 0) {
        got = read(fd, cnf, sizeof(cnf) - 1);
        close(fd);

        if (got > 0) {
            cnf[got] = '\0';
            step_num("2-system-cnf-via-file", (unsigned int)got, 0, 0);

            if (parse_boot2(cnf, out, max) == 0)
                return 0;

            return -5;
        }

        step_num("2-system-cnf-empty", 0, 0, got);
    } else {
        step("2-cannot-open-system-cnf");
    }

    /* Fallback: walk ISO9660 off the raw sectors. */
    if (read_root(RA_SRC_DISC_FD, &root_lba, &root_size) != 0)
        return -1;

    if (find_entry(RA_SRC_DISC_FD, root_lba, root_size, "SYSTEM.CNF", &cnf_lba, &cnf_size) != 0) {
        step("2-no-system-cnf");
        return -2;
    }

    if (cnf_size == 0)
        return -3;

    if (cnf_size > sizeof(cnf) - 1)
        cnf_size = sizeof(cnf) - 1;

    step_num("2-system-cnf-found", cnf_lba, cnf_size, 0);

    got = read_at(RA_SRC_DISC_FD, (long long)cnf_lba * ISO_SECTOR, g_chunk, (int)cnf_size);
    if (got <= 0) {
        step("2-system-cnf-unreadable");
        return -4;
    }

    memcpy(cnf, g_chunk, cnf_size);
    cnf[cnf_size] = '\0';

    return parse_boot2(cnf, out, max);
}

int raHashIsoDirect(const char *isopath, const char *startup, char *out33)
{
    int ret, fd;

    out33[0] = '\0';

    step("1-opening-image");
    fd = open(isopath, O_RDONLY);
    if (fd < 0) {
        step("1-open-failed");
        return -1;
    }

    ret = hash_boot_exec(fd, startup, out33);
    close(fd);

    return ret;
}
