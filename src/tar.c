// Lazy .tar archive loader (ART/CFG/CHT). Ported from wOPL (author: mystyq).
//
// Plain uncompressed ustar (512-byte blocks). For each kind a single archive holds every
// game's files for a device (ART/art.tar of <GAMEID>_<suffix>.png, etc.). The header table is
// indexed ONCE per session (and persisted to a sidecar *_cache.bin keyed on the tar size, so cold
// boots skip the scan); file bodies are read open->seek->read->close per request, keeping NO fds
// open (avoids the EE/IOP fd-desync wOPL hit). POSIX IO only -- matches the EE-side newlib convention.
//
// RiptOPL wires only TAR_KIND_ART, gated by the user toggle gEnableArtTar (default OFF); when the
// toggle is off the engine is never called, and even when on a device with no ART/art.tar is marked
// inactive and costs nothing.

#include <ps2sdkapi.h>
#include <stdio.h>
#include <malloc.h>
#include <fcntl.h>
#include <string.h>

#include "include/tar.h"
#include "include/ioman.h" // LOG -- this engine shipped with ZERO tracing, which made every "art.tar
                           // doesn't work" report unfalsifiable (the CHT path announces its hit at
                           // supportbase.c; ART said nothing at all).

typedef struct
{
    const char *prefix;
    const char *mountRoot;
} TarDevice;

typedef struct
{
    const char *relPath;
    const char *cacheName;
    u32 entrySize;
} TarKindInfo;

// Probe order (first device that has the archive wins). Widened vs wOPL to RiptOPL's mounts:
// all 8 BDM mass slots, both MMCE slots, the internal HDD (APA + pfs), and SMB.
static const TarDevice gDevices[] = {
    {"mass0:", "mass0:/"},
    {"mass1:", "mass1:/"},
    {"mass2:", "mass2:/"},
    {"mass3:", "mass3:/"},
    {"mass4:", "mass4:/"},
    {"mass5:", "mass5:/"},
    {"mass6:", "mass6:/"},
    {"mass7:", "mass7:/"},
    {"mmce0:", "mmce0:/"},
    {"mmce1:", "mmce1:/"},
    {"hdd0:", "hdd0:/"},
    {"pfs0:", "pfs0:/"},
    {"smb0:", "smb0:/"},
    {NULL, NULL}};

static const TarKindInfo gTarInfo[TAR_KIND_MAX] = {
    {"ART/art.tar", "art_cache.bin", sizeof(TarEntryArt)},
    {"CFG/cfg.tar", "cfg_cache.bin", sizeof(TarEntryCfg)},
    {"CHT/cht.tar", "cht_cache.bin", sizeof(TarEntryCht)}};

static void *s_index[TAR_KIND_MAX] = {NULL, NULL, NULL};
static u32 s_count[TAR_KIND_MAX] = {0, 0, 0};
static u32 s_cap[TAR_KIND_MAX] = {0, 0, 0};
static const TarDevice *s_dev[TAR_KIND_MAX] = {NULL, NULL, NULL};
static int s_inactive[TAR_KIND_MAX] = {0, 0, 0};

static const unsigned char s_zeroBlock[TAR_BLOCK_SIZE] __attribute__((aligned(64))) = {0};

static u64 parseOctal(const char *s, int len)
{
    u64 val = 0;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c < '0' || c > '7')
            break;
        val = (val << 3) + (u64)(c - '0');
    }
    return val;
}

static int isZeroBlock(const unsigned char header[TAR_BLOCK_SIZE])
{
    return memcmp(header, s_zeroBlock, TAR_BLOCK_SIZE) == 0;
}

static void tarCloseInternal(TarKind kind)
{
    if (s_index[kind]) {
        free(s_index[kind]);
        s_index[kind] = NULL;
    }
    s_count[kind] = 0;
    s_cap[kind] = 0;
    s_dev[kind] = NULL;
}

static int buildTarPath(const TarDevice *dev, TarKind kind, char *out, int outSize)
{
    if (dev == NULL) // s_dev is NULL if a kind was populated via tarLoadFile (no re-open device); refuse rather than deref
        return -1;

    int len = snprintf(out, outSize, "%s%s", dev->mountRoot, gTarInfo[kind].relPath);

    return (len > 0 && len < outSize) ? 0 : -1;
}

static int tarReadCache(const char *cachePath, const char *tarPath, TarKind kind)
{
    int fd = open(cachePath, O_RDONLY);
    if (fd < 0)
        return -1;

    struct stat stCache;
    if (fstat(fd, &stCache) < 0) {
        close(fd);
        return -1;
    }

    int fileSize = stCache.st_size;
    if (fileSize < (int)sizeof(TarCacheHeader)) {
        close(fd);
        return -1;
    }

    void *buf = memalign(64, fileSize);
    if (!buf) {
        close(fd);
        return -1;
    }

    if (read(fd, buf, fileSize) != fileSize) {
        free(buf);
        close(fd);
        return -1;
    }
    close(fd);

    TarCacheHeader *hdr = (TarCacheHeader *)buf;

    if (memcmp(hdr->magic, ARC_MAGIC, 4) != 0 || hdr->version != ARC_VERSION) {
        free(buf);
        return -1;
    }

    struct stat stTar;
    if (stat(tarPath, &stTar) < 0 || (u64)stTar.st_size != hdr->tarSize) {
        free(buf);
        return -1;
    }

    u32 entrySize = gTarInfo[kind].entrySize;
    // Guard a corrupt/hostile entryCount: validate by DIVISION so entryCount * entrySize can
    // never overflow u32. Without this an overflowed product slips past the size check, malloc
    // and memcpy use the wrapped (small) size, yet s_count keeps the huge original count -- and
    // tarFind then walks that many entrySize-strides off the small allocation (OOB read).
    u32 maxEntries = (u32)(fileSize - (int)sizeof(TarCacheHeader)) / entrySize;
    if (entrySize == 0 || hdr->entryCount > maxEntries) {
        free(buf);
        return -1;
    }

    void *entries = malloc(hdr->entryCount * entrySize);
    if (!entries) {
        free(buf);
        return -1;
    }

    memcpy(entries, (unsigned char *)buf + sizeof(TarCacheHeader), hdr->entryCount * entrySize);

    // The live parser caps every rawSize at MAX_FILE_SIZE (the tar.h invariant); a corrupt cache could
    // carry a larger value. Reject such a cache so the caller re-parses the tar (which re-caps and
    // rewrites a clean cache) instead of trusting an out-of-range size downstream.
    for (u32 i = 0; i < hdr->entryCount; i++) {
        TarEntryBase *e = (TarEntryBase *)((unsigned char *)entries + entrySize * i);
        if (e->rawSize > MAX_FILE_SIZE) {
            free(entries);
            free(buf);
            return -1;
        }
    }

    s_index[kind] = entries;
    s_count[kind] = hdr->entryCount;
    s_cap[kind] = hdr->entryCount;

    free(buf);
    return 0;
}

static int tarWriteCache(const char *cachePath, const char *tarPath, TarKind kind)
{
    struct stat stTar;
    if (stat(tarPath, &stTar) < 0)
        return -1;

    TarCacheHeader hdr;
    memcpy(hdr.magic, ARC_MAGIC, 4);
    hdr.version = ARC_VERSION;
    hdr.tarSize = stTar.st_size;
    hdr.entryCount = s_count[kind];

    int fd = open(cachePath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return -1;

    if (write(fd, &hdr, sizeof(hdr)) != (int)sizeof(hdr)) {
        close(fd);
        return -1;
    }

    u32 entrySize = gTarInfo[kind].entrySize;
    u32 totalSize = entrySize * s_count[kind];
    if (totalSize > 0) {
        if (write(fd, s_index[kind], totalSize) != (int)totalSize) {
            close(fd);
            return -1;
        }
    }

    close(fd);
    return 0;
}

static int tarParseFile(TarKind kind, const char *path)
{
    // No stat() existence probe here. It was pure redundancy -- the caller has already proven the file
    // OPENS, and this function opens it again below -- but its failure latched s_inactive[kind], which is
    // a process-wide, write-once kill switch with no live way to clear it. Any device driver where open()
    // succeeds but stat() fails therefore killed art.tar for the whole session with the archive sitting
    // right there. Let the open() below be the single arbiter of existence.
    char dirPath[256];
    strncpy(dirPath, path, sizeof(dirPath));
    dirPath[sizeof(dirPath) - 1] = '\0';

    for (int i = strlen(dirPath) - 1; i >= 0; i--)
        if (dirPath[i] == '/') {
            dirPath[i + 1] = '\0';
            break;
        }

    char fullCachePath[256];
    snprintf(fullCachePath, sizeof(fullCachePath), "%s%s", dirPath, gTarInfo[kind].cacheName);

    if (tarReadCache(fullCachePath, path, kind) == 0)
        return 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    free(s_index[kind]);
    s_index[kind] = NULL;
    s_count[kind] = 0;
    s_cap[kind] = 0;

    u32 entrySize = gTarInfo[kind].entrySize;
    if (entrySize <= sizeof(TarEntryBase) + 1)
        goto fail;

    u32 nameMax = entrySize - sizeof(TarEntryBase) - 1;

    while (1) {
        unsigned char header[TAR_BLOCK_SIZE] __attribute__((aligned(64)));
        int r = read(fd, header, TAR_BLOCK_SIZE);
        if (r == 0)
            break;

        if (r < 0)
            goto fail;
        if (r != TAR_BLOCK_SIZE)
            goto fail;

        // Do NOT stop at the first end-of-archive marker. A tar EOF marker is a run of zero
        // blocks, and a CONCATENATED art.tar (a covers pack + a BG pack + a SCR pack merged with
        // cat / copy /b, which is how these packs are actually distributed) carries one MID-FILE.
        // Breaking here indexed only the FIRST member and silently dropped every entry after it --
        // in practice every _BG/_SCR/_SCR2, while the covers in the first member kept working.
        // This is GNU tar's --ignore-zeros semantics. The scan now ends only at a real EOF (the
        // read()==0 above); trailing padding on a well-formed archive costs a few extra 512-byte
        // reads. Note the ordering tell: within one game "_BG" sorts BEFORE "_COV", so a single
        // truncated archive would have lost COVERS first -- covers surviving is only possible if
        // they sit in an earlier concatenated member.
        if (isZeroBlock(header))
            continue;

        char typeflag = (char)header[156];
        u64 rawSize = parseOctal((char *)header + 124, 12);
        u64 paddedSize = ((rawSize + 511) / 512) * 512;
        int oversized = (rawSize > MAX_FILE_SIZE); // too big to serve (rawSize/paddedSize are u32) -> don't index it

        if (typeflag != '\0' && typeflag != '0') {
            // Skip directory or non-file entries (e.g. typeflag '5')
            if (lseek64(fd, paddedSize, SEEK_CUR) == (u64)-1)
                goto fail;
            continue;
        }

        u64 dataOffset = lseek64(fd, 0, SEEK_CUR);
        if (dataOffset == (u64)-1)
            goto fail;

        if (oversized) {
            LOG("TAR skip oversized entry (%lu bytes > %d) in %s\n", (unsigned long)rawSize, MAX_FILE_SIZE, path);
            if (lseek64(fd, paddedSize, SEEK_CUR) == (u64)-1)
                goto fail;
            continue;
        }

        if (s_count[kind] >= s_cap[kind]) {
            u32 newCap = s_cap[kind] ? (s_cap[kind] + 64) : 64;
            void *newIndex = realloc(s_index[kind], entrySize * newCap);
            if (!newIndex)
                goto fail;
            s_index[kind] = newIndex;
            s_cap[kind] = newCap;
        }

        char *entry = (char *)s_index[kind] + entrySize * s_count[kind];
        TarEntryBase *base = (TarEntryBase *)entry;
        base->offset = dataOffset;
        base->rawSize = rawSize;
        base->paddedSize = paddedSize;

        // Parse full USTAR path from 100-byte name (offset 0) and 155-byte prefix (offset 345)
        char nameBuf[101];
        memcpy(nameBuf, header, 100);
        nameBuf[100] = '\0';
        for (int i = 99; i >= 0; i--) {
            if (nameBuf[i] == ' ')
                nameBuf[i] = '\0';
            else if (nameBuf[i] != '\0')
                break;
        }

        char prefix[156];
        memcpy(prefix, header + 345, 155);
        prefix[155] = '\0';
        for (int i = 154; i >= 0; i--) {
            if (prefix[i] == ' ')
                prefix[i] = '\0';
            else if (prefix[i] != '\0')
                break;
        }

        char fullPath[256];
        if (prefix[0] != '\0')
            snprintf(fullPath, sizeof(fullPath), "%s/%s", prefix, nameBuf);
        else
            snprintf(fullPath, sizeof(fullPath), "%s", nameBuf);
        fullPath[sizeof(fullPath) - 1] = '\0';

        // Canonical filename normalization: strip leading directory prefixes
        const char *cleanName = fullPath;
        if (!strncasecmp(cleanName, "./", 2))
            cleanName += 2;
        if (!strncasecmp(cleanName, "ART/", 4) || !strncasecmp(cleanName, "CFG/", 4) || !strncasecmp(cleanName, "CHT/", 4))
            cleanName += 4;

        char *fname = entry + sizeof(TarEntryBase);
        strncpy(fname, cleanName, nameMax);
        fname[nameMax - 1] = '\0';

        s_count[kind]++;

        if (lseek64(fd, paddedSize, SEEK_CUR) == (u64)-1)
            goto fail;
    }

    close(fd);
    tarWriteCache(fullCachePath, path, kind);
    return 0;

fail:
    close(fd);
    free(s_index[kind]);
    s_index[kind] = NULL;
    s_count[kind] = 0;
    s_cap[kind] = 0;
    return -1;
}

int tarLoadFile(TarKind kind, const char *path)
{
    s_dev[kind] = NULL;
    return tarParseFile(kind, path);
}

int tarLoadFromAnyDevice(TarKind kind)
{
    if (s_inactive[kind])
        return -1;

    if (s_index[kind] && s_dev[kind]) {
        char tarPath[256];
        if (buildTarPath(s_dev[kind], kind, tarPath, sizeof(tarPath)) == 0) {
            int fd = open(tarPath, O_RDONLY);
            if (fd >= 0) {
                close(fd);
                return 0;
            }
        }
        tarCloseInternal(kind);
    }

    int found = 0;

    for (int i = 0; gDevices[i].prefix; i++) {
        const TarDevice *dev = &gDevices[i];

        char tarPath[256];
        if (buildTarPath(dev, kind, tarPath, sizeof(tarPath)) < 0)
            continue;

        int fd = open(tarPath, O_RDONLY);
        if (fd < 0)
            continue;
        close(fd);

        s_dev[kind] = dev;
        if (tarParseFile(kind, tarPath) == 0) {
            LOG("TAR indexed %s (%u entries)\n", tarPath, (unsigned int)s_count[kind]);
            found = 1;
            break;
        }
        LOG("TAR parse FAILED for %s -- trying the next device\n", tarPath);

        tarCloseInternal(kind);
    }

    // Latch: no archive on any device -> stop probing for the rest of the session (this is what makes
    // the loader cheap by default on setups that ship no .tar -- one failed open per device, then zero).
    // It is write-once and process-wide, so LOG it: a silent latch made every HW report unfalsifiable.
    if (!found) {
        LOG("TAR no archive found for kind %d -- probing disabled for this session\n", (int)kind);
        s_inactive[kind] = 1;
    }

    return found ? 0 : -1;
}

int tarClose(TarKind kind)
{
    tarCloseInternal(kind);
    s_inactive[kind] = 0;
    return 0;
}

TarEntryBase *tarFind(TarKind kind, const char *filename)
{
    if (s_inactive[kind])
        return NULL;

    if (!s_index[kind] || s_count[kind] == 0)
        if (tarLoadFromAnyDevice(kind) < 0)
            return NULL;

    const char *queryClean = filename;
    if (!strncasecmp(queryClean, "./", 2))
        queryClean += 2;
    if (!strncasecmp(queryClean, "ART/", 4) || !strncasecmp(queryClean, "CFG/", 4) || !strncasecmp(queryClean, "CHT/", 4))
        queryClean += 4;

    u32 entrySize = gTarInfo[kind].entrySize;
    char *base = (char *)s_index[kind];

    for (u32 i = 0; i < s_count[kind]; i++) {
        char *entry = base + entrySize * i;
        char *fname = entry + sizeof(TarEntryBase);

        if (strcasecmp(fname, filename) == 0 || strcasecmp(fname, queryClean) == 0)
            return (TarEntryBase *)entry;

        const char *baseFname = strrchr(fname, '/');
        baseFname = baseFname ? baseFname + 1 : fname;
        if (strcasecmp(baseFname, queryClean) == 0)
            return (TarEntryBase *)entry;
    }

    return NULL;
}

u32 tarRead(TarKind kind, const TarEntryBase *entry, void *dst, u32 dstSize)
{
    if (s_inactive[kind])
        return 0;

    if (!entry || dstSize < entry->rawSize)
        return 0;

    char tarPath[256];
    if (buildTarPath(s_dev[kind], kind, tarPath, sizeof(tarPath)) < 0)
        return 0;

    int fd = open(tarPath, O_RDONLY);
    if (fd < 0)
        return 0;

    if (lseek64(fd, entry->offset, SEEK_SET) != entry->offset) {
        close(fd);
        return 0;
    }

    u32 total = 0;
    while (total < entry->rawSize) {
        int r = read(fd, (unsigned char *)dst + total, entry->rawSize - total);
        if (r < 0) {
            close(fd);
            return 0;
        }
        if (r == 0) {
            close(fd);
            return 0;
        }
        total += r;
    }

    close(fd);
    return total;
}

void *tarGet(TarKind kind, const char *filename)
{
    if (s_inactive[kind])
        return NULL;

    TarEntryBase *entry = tarFind(kind, filename);
    if (!entry)
        return NULL;

    void *buf = memalign(64, entry->rawSize);
    if (!buf)
        return NULL;

    if (tarRead(kind, entry, buf, entry->rawSize) != entry->rawSize) {
        free(buf);
        return NULL;
    }

    return buf;
}

int tarEnsureLoaded(TarKind kind)
{
    if (s_inactive[kind])
        return -1;

    if (!s_index[kind] || s_count[kind] == 0)
        return tarLoadFromAnyDevice(kind);
    return 0;
}

void tarInvalidate(TarKind kind)
{
    tarCloseInternal(kind);
    s_inactive[kind] = 0;
}

const char *tarGetDevicePrefix(TarKind kind)
{
    return s_dev[kind] ? s_dev[kind]->prefix : NULL;
}
