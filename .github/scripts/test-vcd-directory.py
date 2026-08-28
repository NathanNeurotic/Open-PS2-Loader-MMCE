#!/usr/bin/env python3
"""Exercise the real VCD scanner with controlled directory replies, without PS2 IO.

MMCE's error-at-EOF fixture follows sd2psXtd/firmware commit
8a20bd991b43ac731882f5f85030b19e94c23a3b:
  src/ps2/mmceman/ps2_mmceman_fs.c: MMCEMAN_FS_DREAD
  src/ps2/mmceman/ps2_mmceman_commands.c: ps2_mmceman_cmd_fs_dread
Exhausted iteration sets rv=-1, sent as status 1. Our pinned mmceman
db3e93f0fdbcf882f88da110cbd9b7db188ec17a returns -1 for that status.
PS2SDK's fileXio/libcglue path consequently presents readdir(NULL) + errno.
This is a source-contract test, not a hardware or timing simulation.
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]


STUBS = r"""
#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define memalign(alignment, size) malloc(size)
#define LOG(...) ((void)0)
#define VCD_MAX_ITEMS 2048
#define VCD_NAME_MAX 256
#define ISO_GAME_NAME_MAX 160
#define GAME_FORMAT_ISO 1
#define POPS_FOLDER "POPS"
typedef struct { char name[VCD_NAME_MAX]; } vcd_entry_t;
typedef struct {
    char name[ISO_GAME_NAME_MAX + 1], startup[ISO_GAME_NAME_MAX + 1];
    char extension[5];
    int parts, format;
} base_game_info_t;
typedef struct { int offset; } DIR;
struct dirent { char d_name[256]; };
static DIR stream;
static const char *const *entries;
static int entryCount, endError, openError, bodyError, openCount, closeCount;
static int gAutosort = 1, gVcdFirstDiscOnly = 0;
static char openedPath[256];
static DIR *opendir(const char *path) {
    snprintf(openedPath, sizeof(openedPath), "%s", path);
    if (openError) { errno = openError; return NULL; }
    stream.offset = 0;
    openCount++;
    return &stream;
}
static struct dirent *readdir(DIR *dir) {
    static struct dirent entry;
    if (dir->offset == entryCount) {
        if (endError) errno = endError;
        return NULL;
    }
    snprintf(entry.d_name, sizeof(entry.d_name), "%s", entries[dir->offset++]);
    return &entry;
}
static int closedir(DIR *dir) { (void)dir; closeCount++; return 0; }
static void vcdNoteScanDir(const char *name, const char *path) {
    (void)name; (void)path;
    if (bodyError) errno = bodyError;
}
static void cacheInvalidateFailMemo(void) {}
static int vcdIsHiddenDisc(const char *name) { (void)name; return 0; }
static int vcdEntryCmp(const void *a, const void *b) {
    return strcasecmp(((const vcd_entry_t *)a)->name, ((const vcd_entry_t *)b)->name);
}
static void fixture(const char *const *names, int count, int error) {
    entries = names; entryCount = count; endError = error;
    openError = bodyError = openCount = closeCount = 0;
}
"""

TESTS = r"""
static const char *const names[] = {".", "..", "Zebra.VCD", "Alpha.vcd", "POPSTARTER.VCD", "cover.png"};
static void check_list(base_game_info_t *games, int count) {
    assert(count == 2);
    assert(games != NULL);
    assert(strcmp(games[0].name, "Alpha") == 0);
    assert(strcmp(games[1].name, "Zebra") == 0);
    assert(strcmp(games[0].extension, ".VCD") == 0);
    assert(openCount == 1 && closeCount == 1);
}
static void mmce_tests(void) {
    const char *const roots[] = {"mmce0:/", "mmce1:/OPL/"};
    for (int i = 0; i < 2; ++i) {
        base_game_info_t *games = NULL;
        fixture(names, 6, EPERM);
        int count = vcdFillGameList(roots[i], &games);
        printf("MMCE error-at-EOF %s: count=%d (expected 2)\n", roots[i], count);
        fflush(stdout);
        check_list(games, count);

        fixture(names, 6, EPERM);
        count = vcdFillGameList(roots[i], &games);
        check_list(games, count); // repeated refresh must retain the visible games
        free(games);

        fixture(NULL, 0, EPERM);
        games = NULL;
        assert(vcdFillGameList(roots[i], &games) == 0); // master's empty-directory contract
        assert(games == NULL && openCount == closeCount);
    }
}
static void portable_tests(void) {
    const char *const roots[] = {"mass0:/", "smb0:\\", "pfs0:/", "notmmce0:/"};
    for (int i = 0; i < 4; ++i) {
        base_game_info_t *games = NULL;
        fixture(names, 6, 0);
        int count = vcdFillGameList(roots[i], &games);
        check_list(games, count);
        base_game_info_t *previous = games;

        fixture(names, 4, EIO); // a failed partial walk must not replace last-good
        assert(vcdFillGameList(roots[i], &games) == -1);
        assert(games == previous && strcmp(games[1].name, "Zebra") == 0);

        fixture(names, 6, 0);
        bodyError = ENOMEM; // best-effort metadata failure is NOT a directory failure
        count = vcdFillGameList(roots[i], &games);
        check_list(games, count);

        fixture(NULL, 0, 0);
        assert(vcdFillGameList(roots[i], &games) == 0);
        assert(games == NULL);
    }

    base_game_info_t *games = NULL;
    fixture(names, 6, 0);
    int count = vcdFillGameList("mmce0:/", &games);
    check_list(games, count);
    base_game_info_t *previous = games;
    fixture(NULL, 0, 0);
    openError = EIO;
    assert(vcdFillGameList("mmce0:/", &games) == -1);
    assert(games == previous); // MMCE opendir failures still preserve the old list
    openError = ENOENT;
    assert(vcdFillGameList("mmce0:/", &games) == 0 && games == NULL);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (strcmp(argv[1], "portable")) mmce_tests();
    if (strcmp(argv[1], "mmce")) portable_tests();
    puts("PASS: VCD directory contract");
    return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref", help="Read scanner from this git ref, not the worktree")
    parser.add_argument("--case", choices=("mmce", "portable", "all"), default="all")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    if args.ref:
        source = subprocess.check_output(
            ["git", "show", f"{args.ref}:src/vcdsupport.c"], cwd=root, text=True)
    else:
        source = (root / "src/vcdsupport.c").read_text(encoding="utf-8")
    code = STUBS + "\n" + "\n".join(function(source, signature) for signature in (
        "static int vcdScanOpenDir(", "int vcdScanDir(", "int vcdFillGameList(")) + TESTS
    with tempfile.TemporaryDirectory(prefix="opl-vcd-scan-") as directory:
        cfile = Path(directory) / "test.c"
        binary = Path(directory) / ("test.exe" if os.name == "nt" else "test")
        cfile.write_text(code, encoding="utf-8")
        subprocess.run([args.cc, "-std=c99", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-function", "-Wno-format-truncation",
                        str(cfile), "-o", str(binary)], check=True)
        subprocess.run([str(binary), args.case], check=True)


if __name__ == "__main__":
    main()
