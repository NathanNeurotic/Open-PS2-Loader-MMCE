#!/bin/sh
# Compiles the REAL modules/network/httpclient/httpclient.c on the host, against stub socket and
# sysclib headers, and drives its streaming parser with a scripted peer.
#
# It is the real file, not a copy: the whole point is that the parser which decides whether a
# response's bytes reach a sector buffer is the one under test. The stubs replace only recv/select/
# send, so header framing, Content-Range validation, encoding refusal and truncation detection are
# exercised exactly as they run on the IOP.
#
# Usage, from the repo root:
#     sh pc/http/tests/run.sh
#
# With no host compiler, the same thing inside the build image:
#     docker run --rm -v "$PWD:/repo" -w /repo ps2dev/ps2dev:latest \
#         sh -c "apk add --no-cache build-base >/dev/null 2>&1; sh pc/http/tests/run.sh"

set -e

root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${TMPDIR:-/tmp}/riptopl-http-stream-test

cc=${CC:-gcc}

"$cc" -O1 -Wall -Wextra -Wno-unused-parameter \
    -I"$root/pc/http/tests" \
    -I"$root/pc/http/tests/stub" \
    -I"$root/modules/network/common" \
    -o "$out" \
    "$root/pc/http/tests/test_stream.c" \
    "$root/modules/network/httpclient/httpclient.c"

"$out"
