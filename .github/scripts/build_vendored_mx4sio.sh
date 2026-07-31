#!/bin/sh
# Rebuild the vendored mx4sio_bd.irx / mx4sio_bd_mini.irx (modules/mx4sio/, see
# PROVENANCE.md there): ps2sdk master + the fork-carried patch that restores
# ROM-sio2man (rom1:SIO2MAN v2.x, DECKARD) support in the driver's sio2man hook
# (issue #317; upstream regression ps2dev/ps2sdk PR #862, OPL #1731).
#
# Runs the build inside a throwaway ps2dev/ps2dev:latest container (the same
# toolchain family as both CI flavours); only the two .irx outputs are copied
# out. The sha256 of every output is verified against the WANT_* constants
# below before it is allowed to overwrite the vendored copy -- a silent drift
# must fail loudly. (The image tag is mutable by design: toolchain drift shows
# up as an output-hash change and is stopped by that gate.)
#
# Host requirements: docker, git, sha256sum, awk. Usage:
#   sh .github/scripts/build_vendored_mx4sio.sh            # verify-or-rebuild
#   MX4SIO_UPDATE=1 sh .github/scripts/build_vendored_mx4sio.sh   # accept new bytes
# With MX4SIO_UPDATE=1 the new IRXs are copied in even when the hashes differ
# (after a deliberate re-base of the patch). In the same commit you MUST update
# all of: the WANT_*/BASE_COMMIT constants below, the tables in
# modules/mx4sio/PROVENANCE.md, and modules/mx4sio/SHA256SUMS (CI enforces it).
# Without MX4SIO_UPDATE=1, a hash mismatch is an error and nothing is overwritten.
set -eu

# Keep these three values in sync with modules/mx4sio/PROVENANCE.md and SHA256SUMS.
BASE_COMMIT="e228ff7b61a12ad1192a49338754534362a26e58"
WANT_BD="a2dcc719c7ef5f07b4f05cad9f1f108c3846dab3b44894dccda6b4c1949d40f8"
WANT_MINI="9eab5475eb3270d435039e1045b4500baabe102802e5fea175439923cc15677e"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VENDOR_DIR="$SCRIPT_DIR/../../modules/mx4sio"
PATCH="$VENDOR_DIR/0001-mx4sio_bd-restore-rom-sio2man-hook-support.patch"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

for tool in docker git sha256sum awk; do
    command -v "$tool" >/dev/null 2>&1 || { echo "ERROR: $tool not found" >&2; exit 1; }
done
[ -f "$PATCH" ] || { echo "ERROR: patch not found at $PATCH" >&2; exit 1; }

echo "== Cloning ps2sdk @ $BASE_COMMIT =="
git clone --quiet --filter=blob:none https://github.com/ps2dev/ps2sdk "$WORK/ps2sdk"
git -C "$WORK/ps2sdk" checkout --quiet "$BASE_COMMIT"
git -C "$WORK/ps2sdk" apply "$PATCH"
echo "== Patch applied cleanly =="

echo "== Building mx4sio_bd + mx4sio_bd_mini in ps2dev/ps2dev:latest =="
docker run --rm \
    -v "$WORK/ps2sdk:/src" -w /src -e PS2SDKSRC=/src \
    ps2dev/ps2dev:latest \
    sh -c 'apk add --no-cache make gcc musl-dev >/dev/null 2>&1 &&
           make -s -C iop/sio/mx4sio_bd >/dev/null &&
           make -s -C iop/sio/mx4sio_bd_mini >/dev/null'

GOT_BD="$(sha256sum "$WORK/ps2sdk/iop/sio/mx4sio_bd/irx/mx4sio_bd.irx" | awk '{print $1}')"
GOT_MINI="$(sha256sum "$WORK/ps2sdk/iop/sio/mx4sio_bd_mini/irx/mx4sio_bd_mini.irx" | awk '{print $1}')"
echo "mx4sio_bd.irx       sha256 $GOT_BD"
echo "mx4sio_bd_mini.irx  sha256 $GOT_MINI"

if [ "${MX4SIO_UPDATE:-0}" != "1" ] && { [ "$GOT_BD" != "$WANT_BD" ] || [ "$GOT_MINI" != "$WANT_MINI" ]; }; then
    echo "ERROR: built hashes differ from PROVENANCE.md (want $WANT_BD / $WANT_MINI)." >&2
    echo "       If this re-base is deliberate, rerun with MX4SIO_UPDATE=1 and update PROVENANCE.md," >&2
    echo "       the WANT_* constants in this script, and SHA256SUMS." >&2
    exit 1
fi

cp -f "$WORK/ps2sdk/iop/sio/mx4sio_bd/irx/mx4sio_bd.irx" "$VENDOR_DIR/mx4sio_bd.irx"
cp -f "$WORK/ps2sdk/iop/sio/mx4sio_bd_mini/irx/mx4sio_bd_mini.irx" "$VENDOR_DIR/mx4sio_bd_mini.irx"
echo "== Vendored IRXs refreshed in $VENDOR_DIR =="
