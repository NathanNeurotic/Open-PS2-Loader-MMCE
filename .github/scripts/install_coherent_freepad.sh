#!/bin/sh
# Install the vendored NEWER-GENERATION freepad.irx (modules/freepad/, see
# PROVENANCE.md there) over the container's stock freepad, for the two older
# build containers (ps2max/dev, ghcr ps2homebrew pin). The PS2DEVLATESTSDK
# container already ships these exact bytes natively -- do NOT run there.
#
# WHY (#254, hardware-confirmed by PixeliGer): with Game ID on (the default),
# mmceman loads right after GUI_INIT_DONE and its sio2man hook installs onto
# the bus freepad drives pads over. On the ps2max/ghcr containers' stock
# freepad builds that takes pad input DOWN (dead cursor, live menu, all three
# reporter consoles, original + third-party pads); on ps2dev:latest's freepad
# build it does not. sio2man is byte-identical between latest and ghcr (and
# export-identical on ps2max), mmceman is byte-identical on all three --
# freepad is the remaining SIO2 delta. freepad is menu-side only, so the swap
# cannot affect in-game input.
#
# This is a hypothesis-driven fix: the clash mechanism (hook vs older freepad
# call pattern) is inferred from the shipped binaries, not yet proven on
# hardware. The script is deliberately loud and records what it installed.
set -eu

: "${PS2SDK:?PS2SDK must be set (run inside a ps2dev toolchain container)}"
DEST="$PS2SDK/iop/irx"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$SCRIPT_DIR/../../modules/freepad/freepad.irx"
WANT_SHA256="463fcb30cc4192dce7b4a0ffb8b24b47b3cb0057908c58e4542edebb91e6898e"

if [ ! -f "$SRC" ]; then
    echo "ERROR: vendored freepad.irx not found at $SRC" >&2
    exit 1
fi

# Verify the vendored bytes before installing (a corrupted or stale blob must
# never ship silently -- same fail-loud rule as the mmceman scripts).
if ! command -v sha256sum >/dev/null 2>&1; then
    echo "ERROR: sha256sum not available to verify vendored freepad.irx" >&2
    exit 1
fi
got="$(sha256sum "$SRC" | awk '{print $1}')"
if [ "$got" != "$WANT_SHA256" ]; then
    echo "ERROR: vendored freepad.irx sha256 mismatch (got $got, want $WANT_SHA256)" >&2
    exit 1
fi

if [ ! -f "$DEST/freepad.irx" ]; then
    echo "ERROR: stock freepad.irx missing from $DEST (unexpected container layout)" >&2
    exit 1
fi
cp -f "$SRC" "$DEST/freepad.irx"
echo "== Installed newer-generation freepad.irx ($(wc -c < "$SRC") bytes, sha256 $(printf '%s' "$WANT_SHA256" | cut -c 1-12)...) -> $DEST/ (#254) =="
