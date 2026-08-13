/*
  Copyright 2026, RiptOPL
  Licenced under Academic Free License version 3.0
  Review OpenPS2Loader README & LICENSE files for further details.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <kernel.h>

#include "include/opl.h"
#include "include/ioman.h" // LOG
#include "include/artindex.h"

// See artindex.h for why this exists. Everything below is in service of one rule: an uncertain answer
// is always "go and look".

// Directories held at once. Three covers the realistic worst case in one view -- a device's ART/
// folder, a second device's, and the POPS folder the VCD fallback probes -- without turning this into
// a cache that needs an eviction policy of its own.
#define ART_INDEX_SLOTS 3

// Refuse to hold a directory larger than this. Not a memory limit so much as a sanity limit: a sweep
// that big is itself slow enough to be the wrong trade, and a caller is better served by the ordinary
// probe path. 4096 entries is far past any real ART folder (a 400-game library with five art types
// per game is 2000).
#define ART_INDEX_MAX_ENTRIES 4096

#define ART_INDEX_DIR_MAX 192

typedef struct
{
    char dir[ART_INDEX_DIR_MAX];
    unsigned int *hashes; // sorted, `count` of them
    int count;
    int valid; // 0 = this slot answers "don't know" for everything
} art_index_dir_t;

static art_index_dir_t gArtIndex[ART_INDEX_SLOTS];
static int gArtIndexNext = 0; // round-robin replacement; see artIndexSlotFor
static unsigned int gArtIndexAbsentAnswered = 0;

// Sweeps that could not list their directory. THE FIELD THAT WOULD HAVE CAUGHT THIS MODULE BEING
// INERT: on its first hardware run the HUD read IX0/0 -- no directories held, no absences answered --
// which is indistinguishable from "no art has been requested yet". A failure count separates "not
// used" from "tried and could not", and it is the difference between reading the number and guessing.
static unsigned int gArtIndexSweepFailed = 0;

// Probes rejected because they did not come from the owner thread. Folded into the HUD next to the
// failed-sweep count for the same reason: "inert" and "not yet used" must never read the same.
static unsigned int gArtIndexWrongThread = 0;

// SINGLE-OWNER-THREAD CONFINEMENT, and it is not decoration.
//
// texDiscoverLoad is the choke point this hangs off, and it has callers on TWO threads: the art
// worker (every itemGetImage) and the GUI/IO thread (the theme loader). Without this, two threads
// could take the same round-robin slot and both free and reassign its buffer -- a double free -- and
// a reader could be mid-search through a buffer another thread just released.
//
// Rather than reach for a lock (this codebase has already paid once for a semaphore between the art
// path and the render thread), every slot is only ever touched by the thread that registered as the
// owner. Any other caller gets 1 = "go and look", i.e. exactly the behaviour that existed before this
// file. The theme loader therefore does not benefit, which is fine: it runs once per theme switch,
// not once per row.
static int gArtIndexOwnerThread = -1;

// Bumped by artIndexInvalidate, which can be called from ANY thread. A reader samples it before and
// after its search and gives up if it moved -- so an invalidation landing mid-search can only ever
// downgrade the answer to "go and look", never leave a stale "absent" behind. The stale-absent
// direction is the one that would hide art that exists.
static volatile unsigned int gArtIndexGen = 0;

void artIndexSetOwnerThread(int threadId)
{
    gArtIndexOwnerThread = threadId;
}

// FNV-1a over the UPPERCASED name. Case folding is not cosmetic: FAT and exFAT are
// case-insensitive-preserving, so readdir can hand back "Slus_123.45_COV.PNG" for a path we build as
// "SLUS_123.45_COV.png". Hashing them differently would make the index answer "absent" for a file
// that exists -- the one failure this must never have.
static unsigned int artIndexHash(const char *s)
{
    unsigned int h = 2166136261u;

    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c >= 'a' && c <= 'z')
            c = (unsigned char)(c - 'a' + 'A');
        h ^= c;
        h *= 16777619u;
    }

    return h;
}

static int artIndexCmp(const void *a, const void *b)
{
    unsigned int x = *(const unsigned int *)a;
    unsigned int y = *(const unsigned int *)b;

    if (x < y)
        return -1;
    return (x > y) ? 1 : 0;
}

// Split a full path into its directory and its basename. Handles both "mass0:/ART/x.png" and the
// colon-only form "mass0:x.png", where the device token IS the directory.
static int artIndexSplit(const char *fullPath, char *dirOut, int dirMax, const char **baseOut)
{
    const char *slash = strrchr(fullPath, '/');
    const char *cut;

    if (slash != NULL) {
        cut = slash;
    } else {
        const char *colon = strchr(fullPath, ':');
        if (colon == NULL)
            return 0;
        cut = colon + 1; // keep the colon on the directory side
    }

    int dirLen = (int)(cut - fullPath);
    if (dirLen <= 0 || dirLen >= dirMax)
        return 0;

    memcpy(dirOut, fullPath, dirLen);
    dirOut[dirLen] = '\0';

    *baseOut = (slash != NULL) ? slash + 1 : cut;
    if ((*baseOut)[0] == '\0')
        return 0;

    return 1;
}

// Sweep one directory into a fresh slot. Returns the slot, which may be marked invalid -- an invalid
// slot is still worth keeping, because it stops us re-sweeping a directory that cannot be indexed on
// every single probe.
static art_index_dir_t *artIndexBuild(const char *dir)
{
    art_index_dir_t *slot = &gArtIndex[gArtIndexNext];
    gArtIndexNext = (gArtIndexNext + 1) % ART_INDEX_SLOTS;

    // Safe to free here and ONLY here: this runs on the owner thread, which is the only thread that
    // ever reads these buffers. artIndexInvalidate deliberately does not free for exactly this reason.
    if (slot->hashes != NULL) {
        unsigned int *old = slot->hashes;
        slot->hashes = NULL;
        slot->count = 0;
        slot->valid = 0;
        free(old);
    }

    snprintf(slot->dir, sizeof(slot->dir), "%s", dir);
    slot->valid = 0;
    slot->count = 0;

    // TWO PATH FORMS, because OPL builds art paths without a slash after the device token and not
    // every driver accepts that for a DIRECTORY. bdmDeviceRoot is "mass0:" (bdmsupport.c, no trailing
    // slash) and bdmGetImage appends "ART/<name>_COV", so the directory here is "mass0:ART" -- which
    // opendir rejected, silently leaving this whole module inert. Hardware read IX0/0 on a build where
    // a missing cover still cost 4395 ms in open() alone: the index was never built, for any
    // directory, and nothing said so.
    //
    // Try the path as given, then the "mass0:/ART" form. Costs one extra failed opendir once per
    // directory, only when the first form fails.
    DIR *d = opendir(dir);
    if (d == NULL) {
        const char *colon = strchr(dir, ':');
        if (colon != NULL && colon[1] != '\0' && colon[1] != '/') {
            char alt[ART_INDEX_DIR_MAX + 1];
            int head = (int)(colon - dir) + 1;
            if (snprintf(alt, sizeof(alt), "%.*s/%s", head, dir, colon + 1) < (int)sizeof(alt)) {
                d = opendir(alt);
                if (d != NULL)
                    LOG("ARTINDEX: '%s' listed as '%s'\n", dir, alt);
            }
        }
    }

    if (d == NULL) {
        // Could not list it -- a missing ART folder, or a device that went away. Not an error: the
        // slot stays invalid and every probe for this directory takes the ordinary path.
        gArtIndexSweepFailed++;
        LOG("ARTINDEX: cannot list '%s' -- probes will use the device\n", dir);
        return slot;
    }

    unsigned int *hashes = (unsigned int *)malloc(ART_INDEX_MAX_ENTRIES * sizeof(unsigned int));
    if (hashes == NULL) {
        closedir(d);
        return slot;
    }

    int n = 0;
    int overflowed = 0;
    struct dirent *e;

    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '\0' || e->d_name[0] == '.')
            continue;

        if (n >= ART_INDEX_MAX_ENTRIES) {
            overflowed = 1;
            break;
        }

        hashes[n++] = artIndexHash(e->d_name);
    }

    closedir(d);

    if (overflowed) {
        // A partial listing is the one thing that could make this answer "absent" about a file that
        // exists. Throw it away rather than hold a truthful-looking half-index.
        LOG("ARTINDEX: '%s' has more than %d entries -- not indexing\n", dir, ART_INDEX_MAX_ENTRIES);
        free(hashes);
        return slot;
    }

    qsort(hashes, n, sizeof(unsigned int), &artIndexCmp);

    // Publish order matters even on one thread: valid LAST, so the slot is never readable with a
    // count that does not match its buffer.
    slot->hashes = hashes;
    slot->count = n;
    slot->valid = 1;

    LOG("ARTINDEX: '%s' indexed, %d entries\n", dir, n);
    return slot;
}

static art_index_dir_t *artIndexSlotFor(const char *dir)
{
    int i;

    for (i = 0; i < ART_INDEX_SLOTS; i++) {
        if (gArtIndex[i].dir[0] != '\0' && strcmp(gArtIndex[i].dir, dir) == 0)
            return &gArtIndex[i];
    }

    return artIndexBuild(dir);
}

int artIndexMayExist(const char *fullPath)
{
    char dir[ART_INDEX_DIR_MAX];
    const char *base = NULL;

    if (fullPath == NULL || fullPath[0] == '\0')
        return 1;

    // Not the owner thread: never touch the slots at all. See gArtIndexOwnerThread.
    if (gArtIndexOwnerThread < 0 || GetThreadId() != gArtIndexOwnerThread) {
        gArtIndexWrongThread++; // HUD: if this climbs, confinement is rejecting the art worker itself
        return 1;
    }

    if (!artIndexSplit(fullPath, dir, sizeof(dir), &base))
        return 1; // cannot even name the directory: go and look

    unsigned int genBefore = gArtIndexGen;

    art_index_dir_t *slot = artIndexSlotFor(dir);
    if (slot == NULL || !slot->valid || slot->hashes == NULL)
        return 1;

    unsigned int want = artIndexHash(base);
    int lo = 0, hi = slot->count - 1;

    while (lo <= hi) {
        int mid = lo + ((hi - lo) >> 1);
        unsigned int got = slot->hashes[mid];

        if (got == want)
            return 1; // a hash match is only a MAYBE -- collisions exist, so confirm on the device
        if (got < want)
            lo = mid + 1;
        else
            hi = mid - 1;
    }

    // Someone invalidated while we were searching, so this listing may describe a device that has
    // since changed. Downgrade to "go and look" rather than report an absence we are no longer sure
    // of -- a wrong "absent" hides art that exists, which is the one outcome this must never produce.
    if (gArtIndexGen != genBefore)
        return 1;

    gArtIndexAbsentAnswered++;
    return 0; // not in the listing, and the listing was complete: genuinely not there
}

void artIndexInvalidate(void)
{
    int i;

    // Callable from ANY thread, so it deliberately does NOT free: the owner thread may be searching a
    // buffer right now, and releasing it underneath would be a use-after-free. Marking the slots
    // unusable is enough -- the next build on the owner thread reclaims the memory, and the bounded
    // three slots cap what can be held meanwhile.
    gArtIndexGen++;

    for (i = 0; i < ART_INDEX_SLOTS; i++) {
        gArtIndex[i].valid = 0;
        gArtIndex[i].dir[0] = '\0';
    }
}

void artIndexDebug(int *dirsHeld, unsigned int *absentAnswered, unsigned int *sweepsFailed)
{
    if (sweepsFailed)
        *sweepsFailed = gArtIndexSweepFailed + gArtIndexWrongThread;

    if (dirsHeld) {
        int i, n = 0;
        for (i = 0; i < ART_INDEX_SLOTS; i++) {
            if (gArtIndex[i].valid)
                n++;
        }
        *dirsHeld = n;
    }

    if (absentAnswered)
        *absentAnswered = gArtIndexAbsentAnswered;
}
