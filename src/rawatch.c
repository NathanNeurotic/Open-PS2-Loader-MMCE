/*
  RA: loading the watch list for the game being launched.

  Every game watches its own set of addresses. Baking them into the code would
  mean rebuilding the loader per game, so the list arrives as a file, the same
  way OPL already ships cheats from CHT/.

  The PC client builds the file from the game's achievement set on
  RetroAchievements. The format is modules/network/common/ra_watch.h.

  The list lives in a static array of the loader; ee_core copies it during its
  initialisation, while loader memory is still intact. This is the same trick
  the cheat list uses (cheatman.c + cheat_api.c).

  Upstream design and implementation: hacan359.
*/

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#ifdef __OPLDIAG
#include <sys/stat.h> /* mkdir, for the launch log */
#endif

#include "include/opl.h"
#include "include/ioman.h"
#include "include/rawatch.h"
#include "modules/network/common/ra_watch.h"

static unsigned int gWatchList[RA_WATCH_MAX];
static int gWatchCount = 0;
static int gWatchBytes = 0;
/* Which game's list is in memory. The list can arrive over the network
   (ranet.c) while still in the menu; at launch there is then no reason to read
   the file, which on USB may not even have left the driver's cache yet. */
static char gWatchStartup[16];

unsigned int *GetWatchList(void)
{
    return gWatchCount > 0 ? gWatchList : NULL;
}

int GetWatchCount(void)
{
    return gWatchCount;
}

int GetWatchBytes(void)
{
    return gWatchBytes;
}

void ClearWatchList(void)
{
    gWatchCount = 0;
    gWatchBytes = 0;
    gWatchStartup[0] = '\0';
}

/* Appends one line per event while a game launches.

   Behind __OPLDIAG rather than upstream's own RA_DEBUG switch: CI builds
   `make release`, so a flag that only exists in a debug configuration never
   reaches a tester, which is exactly who needs this. The share comes first --
   a write to a USB stick can sit in the driver's cache and be lost when the
   console powers off, which is when the note matters -- but with no share
   mounted it falls back to the game's device so the note still lands. */
void raLaunchNote(const char *what, int a, int b)
{
#ifdef __OPLDIAG
    static const char *dirs[] = {"smb0:RA", "mass0:RA", "hdd0:RA"};
    FILE *f = NULL;
    int i;

    for (i = 0; i < (int)(sizeof(dirs) / sizeof(dirs[0])) && f == NULL; i++) {
        char file[64];

        mkdir(dirs[i], 0777);
        snprintf(file, sizeof(file), "%s/launch.txt", dirs[i]);
        f = fopen(file, "a");
    }

    if (f == NULL)
        return;

    fprintf(f, "%-28s %6d %6d\n", what ? what : "?", a, b);
    fclose(f);
#else
    (void)what;
    (void)a;
    (void)b;
#endif
}

int SetWatchList(const void *data, int len, const char *startup)
{
    const struct ra_watch_file *hdr = (const struct ra_watch_file *)data;
    unsigned int need;

    ClearWatchList();

    if (data == NULL || startup == NULL || len < (int)sizeof(*hdr))
        return -1;

    if (hdr->magic != RA_WATCH_MAGIC)
        return -3;

    if (hdr->version != RA_WATCH_VERSION)
        return -4;

    if (hdr->count == 0 || hdr->count > RA_WATCH_MAX)
        return -5;

    if (hdr->bytes == 0 || hdr->bytes > RA_SNAP_MAX_BYTES)
        return -6;

    need = hdr->count * sizeof(unsigned int);
    if (len < (int)(sizeof(*hdr) + need))
        return -7;

    memcpy(gWatchList, (const unsigned char *)data + sizeof(*hdr), need);

    gWatchCount = (int)hdr->count;
    gWatchBytes = (int)hdr->bytes;
    snprintf(gWatchStartup, sizeof(gWatchStartup), "%s", startup);

    LOG("RA: list received over the network: %d entries, snapshot %d bytes\n",
        gWatchCount, gWatchBytes);

    return gWatchCount;
}

/* Reads one specific file. Path construction belongs to the caller
   (sbLoadWatchList), which owns the CHT-style extension retries -- keeping it
   there means there is exactly one place that decides where a list may live.

   Returns the number of entries, or negative on failure. A missing file is not
   an error: this game has no set. */
int LoadWatchListFile(const char *file, const char *startup)
{
    struct ra_watch_file hdr;
    int fd, got;

    if (file == NULL || startup == NULL)
        return -1;

    /* This game's list is already in memory: the network brought it in the
       menu, and it is fresher than any file. Leave the file alone; on USB it
       may not have reached the medium yet. */
    if (gWatchCount > 0 &&
        strncmp(gWatchStartup, startup, sizeof(gWatchStartup) - 1) == 0) {
        LOG("RA: list for %s already in memory, %d entries\n", startup, gWatchCount);
        return gWatchCount;
    }

    ClearWatchList();

    LOG("RA: looking for watch list %s\n", file);

    fd = open(file, O_RDONLY);
    if (fd < 0) {
        LOG("RA: no list, telemetry will carry no snapshot\n");
        return -1;
    }

    got = read(fd, &hdr, sizeof(hdr));
    if (got != (int)sizeof(hdr)) {
        LOG("RA: short read on the header\n");
        close(fd);
        return -2;
    }

    if (hdr.magic != RA_WATCH_MAGIC) {
        LOG("RA: wrong file, magic %08X\n", hdr.magic);
        close(fd);
        return -3;
    }

    if (hdr.version != RA_WATCH_VERSION) {
        LOG("RA: format version %u, expected %d\n", hdr.version, RA_WATCH_VERSION);
        close(fd);
        return -4;
    }

    if (hdr.count == 0 || hdr.count > RA_WATCH_MAX) {
        LOG("RA: %u entries, limit %d\n", hdr.count, RA_WATCH_MAX);
        close(fd);
        return -5;
    }

    if (hdr.bytes == 0 || hdr.bytes > RA_SNAP_MAX_BYTES) {
        LOG("RA: snapshot %u bytes, limit %d\n", hdr.bytes, RA_SNAP_MAX_BYTES);
        close(fd);
        return -6;
    }

    got = read(fd, gWatchList, (int)(hdr.count * sizeof(unsigned int)));
    close(fd);

    if (got != (int)(hdr.count * sizeof(unsigned int))) {
        LOG("RA: short read on the list: %d of %u\n", got, (unsigned)(hdr.count * sizeof(unsigned int)));
        return -7;
    }

    gWatchCount = (int)hdr.count;
    gWatchBytes = (int)hdr.bytes;
    snprintf(gWatchStartup, sizeof(gWatchStartup), "%s", startup);

    LOG("RA: list loaded: %d entries, snapshot %d bytes\n", gWatchCount, gWatchBytes);

    return gWatchCount;
}
