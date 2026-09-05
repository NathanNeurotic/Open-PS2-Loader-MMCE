/*
  RA: the image hash, computed the way RetroAchievements computes it.

  Algorithm (rc_hash_ps2 in rcheevos, hash_disc.c):
    1. open the image and read SYSTEM.CNF
    2. take the boot executable name from BOOT2, e.g. "SLUS_210.65"
    3. MD5( ASCII(name) || contents of that file )

  Half of the work already exists in OPL: ps2cnf.c parses SYSTEM.CNF and
  GetStartupExecName extracts the name and strips "cdrom0:\". The caller
  already holds that name as game->startup, so what remains here is reading the
  file itself and running MD5 over it.

  Hashing on the console means the images themselves decide which games are
  supported; nothing on the PC has to be prepared or kept in sync.

  Upstream design and implementation: hacan359. This port carries the image path
  only -- the disc-in-the-tray source belongs to disc mode, which is deferred.
*/

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "include/opl.h"
#include "include/util.h"
#include "include/iosupport.h"
#include "include/system.h"
#include "include/supportbase.h"
#include "include/ioman.h"
#include "include/md5.h"
#include "include/rahash.h"

#include <ps2sdkapi.h> // lseek64: images can exceed 2 GB

/* Same cap as rcheevos: no boot executable is larger */
#define RA_HASH_MAX_EXEC (64 * 1024 * 1024)
#define RA_HASH_CHUNK    (64 * 1024)

static char g_chunk[RA_HASH_CHUNK] __attribute__((aligned(64)));

/* Crumbs: where to report each step. Set by the caller. */
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

/* A crumb with numbers in it. */
static void step_num(const char *what, unsigned int a, unsigned int b, int c)
{
    char line[64];

    snprintf(line, sizeof(line), "%s lba=%u n=%u rv=%d", what, a, b, c);
    step(line);
}

/* ------------------------------------------------------------------ */
/* Direct image read, no mounting                                      */

/*
  Mounting through "iso:" on USB hangs on the first read when the boot
  executable sits beyond the 2 GB mark of the image, past the reach of a signed
  32-bit offset. The same image works from a share.

  So we bypass the driver: open the image as a plain file, walk the ISO9660
  directory ourselves and seek with 64-bit lseek64.

  This also reads far less than mounting does: one volume descriptor sector, the
  root directory (usually a couple of kilobytes) and the executable itself.

  Do NOT "simplify" this back to a mount plus a normal open/read -- that is the
  bug it exists to avoid.
*/

#define ISO_SECTOR  2048
#define ISO_PVD_LBA 16

static unsigned int le32(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static int read_at(int fd, long long off, void *buf, int len)
{
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

            /* ...which the format promises and a damaged image does not. Without this the
               walk trusts len blindly: rec[32] and the name bytes at rec[33 + i] read up to
               ~287 bytes past p, and since only the first ISO_SECTOR of g_chunk was filled
               that is leftover data from an earlier 64 KB read -- so a truncated image can
               match a name that is not there and hand back a bogus LBA. */
            if (len < 33 || p + len > ISO_SECTOR)
                break;

            namelen = rec[32];

            /* The name must also fit inside the record it claims to belong to. */
            if (33 + namelen > len)
                break;

            /* The name in the image carries a version: "SLUS_210.65;1".
               Compare up to the semicolon so the version does not matter. */
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

/* Root directory of the volume. */
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

/* MD5( name || contents of that file ), the way RetroAchievements does it. */
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

int raHashIsoDirect(const char *isopath, const char *startup, char *out33)
{
    int ret, fd;

    if (isopath == NULL || startup == NULL || out33 == NULL)
        return -1;

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
