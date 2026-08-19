#!/bin/sh
# Install a MENU-coherent mmceman into the container's $(PS2SDK)/iop/irx, replacing the
# SDK-provided prebuilt. Used by BOTH build flavours (ps2dev/ps2dev:latest and the ps2dev digest
# pin) since the #254 fix. mmcedrv/mmceigr (the IN-GAME modules) are deliberately
# LEFT AS THE CONTAINER'S STOCK PREBUILDS.
#
# WHY (menu / mmceman): the containers' mmce trio comes via ps2sdk-ports, pinned to ps2-mmce
# v2.1.1 (979dd77e, 2026-03-07) -- which PREDATES the "Changes to mmceman for ps2sdk sio2man
# updates" fix (cccc366). Every current build container ships the SAME sio2man generation
# (verified from the shipped IRX binaries: sio2man export 2.07 / sio2man1 1.02, 68/59 ordinals,
# and the ghcr pin's sio2man.irx is byte-identical to ps2dev:latest's), whose internals the
# v2.1.1-era sio2man hook does not match: short SIO2 exchanges happen to work, but the mismatched
# hook wedges the bus -- sustained transfers freeze the menu, and with the default-on post-boot
# GameID arm (opl.c deferredInit) the wedge hits right after GUI_INIT_DONE and takes the pads
# (freepad, also SIO2) with it: menu drawn, "config loaded" notification up, console frozen
# (#254, WOPLSDK + PS2MAXSDK). Rebuilding mmceman from MMCE_PIN (includes cccc366) fixes the
# hook/sio2man pairing on every flavour. The 2026-07-20 v2.1.1-generation rebuild for the two
# older containers (patch_stock_mmceman.sh, removed) rested on the mistaken premise that those
# containers ship a pre-refactor sio2man.
#
# WHY NOT mmcedrv/mmceigr (in-game): mmcedrv drives SIO2 directly (its import table has NO sio2man
# dependency -- verified by binary import-table parse), and its SOURCE is UNCHANGED between the
# v2.1.1 pin and MMCE_PIN (`git diff v2.1.1..db3e93f0 -- mmcedrv` is empty). Our repo-Makefile
# rebuild on the new toolchain produced a DIFFERENT binary (10009 vs 11337 bytes) that FAILED
# in-game on hardware (issue #56/#68 reports, 2026-07-05: OPL-core MMCE games broken on the
# "PS2DEVLATESTSDK" flavour but fine on the "WOPLSDK" flavour, whose only relevant delta is stock-vs-rebuilt
# mmcedrv). The stock ports-built binary is field-proven in-game (wOPL ships the byte-identical
# file). Same source, proven binary: keep stock.
#
# The OPL Makefile's MMCE_ASSETS_DIR points at $(PS2SDK)/iop/irx, so every subsequent make (ELF,
# cdvdman/mcemu USE_MMCE modules, variants, debug) picks this mix up with no further changes.
# Idempotent: safe to run once per job before building.
#
# Pin is an immutable SHA (the "latest" tag is force-moved upstream). Bump deliberately.
set -eu

MMCE_PIN="${MMCE_PIN:-db3e93f0fdbcf882f88da110cbd9b7db188ec17a}" # ps2-mmce 'latest' @ 2026-06-15 (has cccc366 sio2man fix)
MMCE_REPO="https://github.com/ps2-mmce/mmceman"

: "${PS2SDK:?PS2SDK must be set (run inside the ps2dev toolchain container)}"
DEST="$PS2SDK/iop/irx"

WORK="$(mktemp -d)"
# Clean up on normal exit AND on the signals a CI cancel/timeout sends -- some ash/dash builds don't
# run the EXIT trap on signal termination, so name them explicitly (harmless where EXIT already covers it).
trap 'rm -rf "$WORK"' EXIT TERM INT HUP
echo "== Building menu-coherent mmceman from ps2-mmce @ ${MMCE_PIN} =="
git clone --quiet "$MMCE_REPO" "$WORK/mmceman"
git -C "$WORK/mmceman" checkout --quiet "$MMCE_PIN"

# Local fix on top of the pin: mmce_fs_close/dclose leak their handle slot from the 16-entry FS
# pool on ANY failed close (SIO2 timeout / bad reply / card error) -- only a clean close frees it.
# On a contended card those close failures accumulate and, since mmceman is a session singleton,
# after ~16 the pool is permanently drained and every mmceN: open()/opendir() fails (RiptOPL #120:
# repeated VCD-info art reads -> blank game lists, boot fails, until reboot). The patch frees the
# slot on every teardown path. Applied here (not a pin bump) so it is visible/reviewable and rides
# the existing rebuild; drop it once the fix is upstream in ps2-mmce/mmceman. Fail LOUD if it stops
# applying after a pin bump -- shipping the leak silently is the failure mode to avoid.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FS_LEAK_PATCH="$SCRIPT_DIR/../patches/mmceman-fs-close-fd-leak.patch"
if [ ! -f "$FS_LEAK_PATCH" ]; then
    echo "ERROR: expected mmceman fd-leak patch not found at $FS_LEAK_PATCH" >&2
    exit 1
fi
echo "== Applying mmceman fd-leak fix (mmce_fs_close/dclose slot free-on-failure) =="
git -C "$WORK/mmceman" apply --verbose "$FS_LEAK_PATCH"

# Distinct-errno fix (HW batch S3): mmce_fs_open returned one bare -1 for six different failures --
# only the card's explicit "no such file" reply actually means the file is absent. OPL memoizes
# missing art per-session, so a contended-bus transient used to brand every browsed game's art
# "nonexistent" until reboot. This patch makes ONLY the card's bad-fd reply return -ENOENT; the
# transient paths stay -1, and OPL's classifier (textures.c) memoizes only on ENOENT. Same patch
# content applies at both pins (the open tail is identical); fail LOUD if it stops applying.
FS_ENOENT_PATCH="$SCRIPT_DIR/../patches/mmceman-fs-open-enoent.patch"
if [ ! -f "$FS_ENOENT_PATCH" ]; then
    echo "ERROR: expected mmceman open-ENOENT patch not found at $FS_ENOENT_PATCH" >&2
    exit 1
fi
echo "== Applying mmceman open-ENOENT fix (distinct errno for genuine not-found) =="
git -C "$WORK/mmceman" apply --verbose "$FS_ENOENT_PATCH"

# Same split for DIRECTORY open (RiptOPL #154 audit residual): mmce_fs_dopen also returned one bare
# -1 for every failure, so an absent POPS folder was indistinguishable from a contended bus and
# burned OPL's bounded VCD rescan budget (MMCE_VCD_SCAN_RETRY_MAX) on every refresh. This patch
# makes ONLY the card's explicit invalid-fd reply return -ENOENT; vcdScanOpenDir then treats ENOENT
# as "no POPS folder -> empty, no retry" while contention keeps the -1 preserve-last-good path.
# Applies on top of the two patches above (disjoint hunks); fail LOUD if it stops applying.
FS_DOPEN_ENOENT_PATCH="$SCRIPT_DIR/../patches/mmceman-fs-dopen-enoent.patch"
if [ ! -f "$FS_DOPEN_ENOENT_PATCH" ]; then
    echo "ERROR: expected mmceman dopen-ENOENT patch not found at $FS_DOPEN_ENOENT_PATCH" >&2
    exit 1
fi
echo "== Applying mmceman dopen-ENOENT fix (distinct errno for genuinely-absent dir) =="
git -C "$WORK/mmceman" apply --verbose "$FS_DOPEN_ENOENT_PATCH"

# Older-SDK make quirk: the per-module obj dir isn't auto-created by every Makefile.iopglobal
# vintage -- pre-create it so the first compile step doesn't fail on a missing directory.
mkdir -p "$WORK/mmceman/mmceman/obj"

make -C "$WORK/mmceman/mmceman"

src="$WORK/mmceman/mmceman/irx/mmceman.irx"
if [ ! -f "$src" ]; then
    echo "ERROR: mmceman.irx was not produced by the ps2-mmce build" >&2
    exit 1
fi
cp -f "$src" "$DEST/mmceman.irx"
echo "  installed mmceman.irx ($(wc -c < "$src") bytes) -> $DEST/"
for m in mmcedrv mmceigr; do
    if [ ! -f "$DEST/$m.irx" ]; then
        echo "ERROR: stock $m.irx missing from the SDK prebuilts" >&2
        exit 1
    fi
    echo "  keeping STOCK $m.irx ($(wc -c < "$DEST/$m.irx") bytes) in $DEST/ (in-game, field-proven)"
done
echo "== Menu-coherent mmceman installed; in-game mmcedrv/mmceigr remain the SDK stock prebuilts =="
