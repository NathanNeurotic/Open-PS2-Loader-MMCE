#!/bin/sh
# Build the "extra" RIPTOPL release builds for the rolling release:
#   * the EXTRA_FEATURES x PADEMU x DUALSENSE variant matrix -> rolling/variants/
#   * the debug configs                          -> rolling/debug/
#
# Called by each ps2dev build job in rolling-release.yml. $1 is the SDK suffix appended to each
# filename: "-PS2DEVPINNED" for the digest-pinned ps2dev snapshot
# build, "-PS2DEVROLLING" for the ps2dev:latest build -- so the VARIANTS and DEBUG zips carry
# both ps2dev flavours. Official-flavour DS5 loaders are added separately by the release normalizer.
# $2 is the LOCALVERSION
# toolchain brand ("PS2DEVPINNED"/"PS2DEVROLLING") embedded in each ELF's version string: filenames get renamed
# and moved to cards, and the debug/variant builds are exactly the ones that show up in bug
# reports -- the on-screen version must self-identify the toolchain like the main builds do.
#
# Best-effort per build: a single failing config is logged + skipped, never sinking the publish.
# MUST run AFTER the main release ELF is already staged, since each build does `make clean`.
# No version in the filenames -- the version lives in the zip names (rolling-release.yml).
set -eu

SDK_SUFFIX="${1:-}"
BRAND="${2:-}"
BRAND_ARG=""
[ -n "$BRAND" ] && BRAND_ARG="LOCALVERSION=$BRAND"

echo "== Building RIPTOPL variants (suffix='${SDK_SUFFIX}' brand='${BRAND}') =="
mkdir -p rolling/variants
for pad in PADEMU=0 PADEMU=1; do
  for ex in EXTRA_FEATURES=0 EXTRA_FEATURES=1; do
    for ds5 in DUALSENSE=0 DUALSENSE=1; do
      make clean >/dev/null 2>&1 || true
      if make --trace $pad $ex $ds5 NOT_PACKED=1 $BRAND_ARG && [ -f opl.elf ]; then
        ds5_label=$([ "${ds5#DUALSENSE=}" = "1" ] && echo "-ds5" || echo "")
        mv opl.elf "rolling/variants/RIPTOPL-pademu${pad#PADEMU=}-extra${ex#EXTRA_FEATURES=}${ds5_label}${SDK_SUFFIX}.ELF"
      else
        echo "WARN: variant '$pad $ex $ds5'${SDK_SUFFIX} failed to build; skipping it"
      fi
    done
  done
done
echo "Variants present:"; ls -la rolling/variants || true

echo "== Building RIPTOPL debug builds (suffix='${SDK_SUFFIX}') =="
mkdir -p rolling/debug
for dbg in iopcore_debug ingame_debug eesio_debug iopcore_ppctty_debug ingame_ppctty_debug DTL_T10000=1; do
  make clean >/dev/null 2>&1 || true
  if make --trace $dbg $BRAND_ARG && [ -f opl.elf ]; then
    mv opl.elf "rolling/debug/RIPTOPL-${dbg%=1}${SDK_SUFFIX}.ELF"
  else
    echo "WARN: debug '$dbg'${SDK_SUFFIX} failed to build; skipping it"
  fi
done
echo "Debug present:"; ls -la rolling/debug || true
