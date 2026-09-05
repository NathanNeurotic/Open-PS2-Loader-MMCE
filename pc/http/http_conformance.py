#!/usr/bin/env python3
"""Check an HTTP server against the RiptOPL range profile.

This is the executable half of docs/HTTP-INTEGRATION-PLAN.md section 4 (Reads). It asks the
questions the PS2 client will ask, in the order it will ask them, and fails on exactly the
things that would corrupt a disc read rather than merely look untidy.

Docmine17's unmodified pc/http_server.py is the primary target: it must pass with no changes.
Any other static server that passes is usable too.

    python pc/http/http_conformance.py http://192.168.1.10:1100/
    python pc/http/http_conformance.py http://127.0.0.1:1100/ --iso "DVD/Game.iso"

Exit status is 0 only if every non-skipped check passed.
"""

import argparse
import http.client
import os
import sys
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import catalog_reference  # noqa: E402

SECTOR = 2048
USER_AGENT = "RiptOPL-conformance/1"


class Result:
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.skipped = 0

    def ok(self, name, detail=""):
        self.passed += 1
        print("  PASS  {}{}".format(name, "  -- " + detail if detail else ""))

    def bad(self, name, detail):
        self.failed += 1
        print("  FAIL  {}  -- {}".format(name, detail))

    def skip(self, name, why):
        self.skipped += 1
        print("  SKIP  {}  -- {}".format(name, why))


def encode_path(base_path, rel):
    """Join the configured base path with a catalog-relative path, encoding once."""
    segments = [urllib.parse.quote(s, safe="") for s in rel.split("/")]
    joined = "/".join(segments)
    if not base_path.endswith("/"):
        base_path += "/"
    return base_path + joined


def get(conn, path, byte_range=None):
    """One GET. Returns (status, headers dict lowercased, body bytes)."""
    headers = {"User-Agent": USER_AGENT, "Accept-Encoding": "identity"}
    if byte_range is not None:
        headers["Range"] = "bytes={}-{}".format(byte_range[0], byte_range[1])
    conn.request("GET", path, headers=headers)
    resp = conn.getresponse()
    body = resp.read()
    hdrs = {k.lower(): v for k, v in resp.getheaders()}
    return resp.status, hdrs, body


def parse_content_range(value):
    """'bytes 0-2047/1234567' -> (0, 2047, 1234567). Returns None if unparseable."""
    if not value:
        return None
    v = value.strip()
    if not v.lower().startswith("bytes "):
        return None
    spec = v[6:].strip()
    if "/" not in spec:
        return None
    rng, total = spec.split("/", 1)
    if "-" not in rng:
        return None
    start, end = rng.split("-", 1)
    try:
        total_i = None if total.strip() == "*" else int(total.strip())
        return int(start), int(end), total_i
    except ValueError:
        return None


def check_range(res, conn, path, start, end, total, label):
    """One byte-exact range read, validated the way the PS2 client must validate it."""
    want = end - start + 1
    status, hdrs, body = get(conn, path, (start, end))

    if status != 206:
        res.bad(label, "expected 206, got {}".format(status))
        return None
    if "transfer-encoding" in hdrs:
        res.bad(label, "Transfer-Encoding: {} -- the profile rejects chunked".format(
            hdrs["transfer-encoding"]))
        return None
    enc = hdrs.get("content-encoding", "").strip().lower()
    if enc and enc != "identity":
        res.bad(label, "Content-Encoding: {}".format(enc))
        return None
    ctype = hdrs.get("content-type", "")
    if ctype.lower().startswith("multipart/"):
        res.bad(label, "multipart range response")
        return None

    cr = parse_content_range(hdrs.get("content-range"))
    if cr is None:
        res.bad(label, "missing or unparseable Content-Range: {!r}".format(
            hdrs.get("content-range")))
        return None
    if (cr[0], cr[1]) != (start, end):
        res.bad(label, "Content-Range says {}-{}, asked for {}-{}".format(
            cr[0], cr[1], start, end))
        return None
    if total is not None and cr[2] is not None and cr[2] != total:
        res.bad(label, "total size changed mid-session: {} then {}".format(total, cr[2]))
        return None

    cl = hdrs.get("content-length")
    if cl is None:
        res.bad(label, "no Content-Length")
        return None
    if int(cl) != want:
        res.bad(label, "Content-Length {} != requested {}".format(cl, want))
        return None
    if len(body) != want:
        res.bad(label, "body is {} bytes, expected {}".format(len(body), want))
        return None

    res.ok(label, "{} bytes at {}".format(want, start))
    return body


def check_quiet(conn, path, start, end):
    """A range read with no reporting, for cross-checking one read against another."""
    try:
        status, hdrs, body = get(conn, path, (start, end))
    except (http.client.HTTPException, OSError):
        return None
    if status != 206 or len(body) != end - start + 1:
        return None
    return body


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("base", help="server base URL, e.g. http://192.168.1.10:1100/")
    ap.add_argument("--iso", help="catalog-relative ISO path to exercise "
                                  "(default: the first supported catalog row)")
    ap.add_argument("--catalog", default="games.csv", help="catalog filename (default games.csv)")
    ap.add_argument("--verify-against", metavar="FILE",
                    help="local copy of the same image, to compare real bytes against")
    ap.add_argument("--timeout", type=float, default=15.0)
    args = ap.parse_args()

    url = urllib.parse.urlparse(args.base)
    if url.scheme != "http":
        print("This profile is plain LAN HTTP; {!r} is not supported.".format(url.scheme))
        return 2
    host = url.hostname
    port = url.port or 80
    base_path = url.path or "/"

    res = Result()
    print("Server: http://{}:{}{}".format(host, port, base_path))

    conn = http.client.HTTPConnection(host, port, timeout=args.timeout)

    # 1. The catalog.
    print("\nCatalog")
    cat_path = encode_path(base_path, args.catalog)
    try:
        status, hdrs, body = get(conn, cat_path)
    except OSError as exc:
        print("  FAIL  connect -- {}".format(exc))
        return 1
    rows = []
    if status != 200:
        res.bad("GET " + args.catalog, "expected 200, got {}".format(status))
    else:
        rows, rejects = catalog_reference.parse_catalog(body)
        res.ok("GET " + args.catalog, "{} bytes, {} rows, {} rejected".format(
            len(body), len(rows), len(rejects)))
        for j in rejects:
            print("        catalog line {}: {}".format(j.line_no, j.reason))
        if len(body) > 512:
            res.ok("catalog exceeds the 512-byte menu RPC buffer",
                   "{} bytes -- the client must stream, not truncate".format(len(body)))

    # 2. Pick an image.
    rel = args.iso
    if rel is None:
        supported = [r for r in rows if r.supported]
        if supported:
            rel = supported[0].path
    if rel is None:
        print("\nNo ISO to exercise (empty catalog and no --iso). Range checks skipped.")
        print("An empty catalog is valid, but it proves nothing about range support.")
        print("\n{} passed, {} failed, {} skipped".format(res.passed, res.failed, res.skipped))
        return 1 if res.failed else 0

    iso_path = encode_path(base_path, rel)
    print("\nImage: {}".format(rel))

    # 3. Total size, from a one-byte range -- the client has no HEAD and must not guess.
    status, hdrs, body = get(conn, iso_path, (0, 0))
    if status != 206:
        res.bad("size probe", "expected 206 for bytes=0-0, got {}".format(status))
        print("\n{} passed, {} failed, {} skipped".format(res.passed, res.failed, res.skipped))
        return 1
    cr = parse_content_range(hdrs.get("content-range"))
    if cr is None or cr[2] is None:
        res.bad("size probe", "no usable total in Content-Range: {!r}".format(
            hdrs.get("content-range")))
        print("\n{} passed, {} failed, {} skipped".format(res.passed, res.failed, res.skipped))
        return 1
    total = cr[2]
    res.ok("size probe", "{} bytes ({:.2f} GiB)".format(total, total / (1024 ** 3)))

    # 4. Byte-exact reads, all on this one connection.
    print("\nRange reads (one persistent connection)")
    check_range(res, conn, iso_path, 0, SECTOR - 1, total, "first sector")
    mid = (total // 2) & ~(SECTOR - 1)
    check_range(res, conn, iso_path, mid, mid + SECTOR - 1, total, "middle sector")
    last = ((total - 1) // SECTOR) * SECTOR
    check_range(res, conn, iso_path, last, total - 1, total, "final sector")
    check_range(res, conn, iso_path, 0, (16 * SECTOR) - 1, total, "16-sector burst")

    if total > (4 * 1024 ** 3):
        off = (4 * 1024 ** 3) & ~(SECTOR - 1)
        check_range(res, conn, iso_path, off, off + SECTOR - 1, total, "read past 4 GiB")
    else:
        res.skip("read past 4 GiB", "image is only {:.2f} GiB".format(total / (1024 ** 3)))

    # A server can return correct-looking headers over the WRONG bytes, and no header check
    # will ever catch that -- which is precisely the failure the donor's status-code-only
    # validation cannot see.
    #
    # Read the same region two ways and require agreement. Know the limit of this: a server
    # that shifts EVERY read by the same amount is self-consistent by construction, so no
    # amount of cross-reading can see it. Only --verify-against, or booting the game, can.
    print("\nContent agreement")
    burst_len = min(16 * SECTOR, total)
    burst = check_range(res, conn, iso_path, 0, burst_len - 1, total, "burst for comparison")
    if burst is not None:
        pieces = []
        consistent = True
        for i in range(burst_len // SECTOR):
            piece = check_quiet(conn, iso_path, i * SECTOR, (i + 1) * SECTOR - 1)
            if piece is None:
                consistent = False
                break
            pieces.append(piece)
        if not consistent:
            res.bad("per-sector reads agree with the burst", "a per-sector read failed")
        elif b"".join(pieces) != burst[:len(b"".join(pieces))]:
            res.bad("per-sector reads agree with the burst",
                    "the same bytes came back differently -- the server is serving the wrong "
                    "region despite correct headers")
        else:
            res.ok("per-sector reads agree with the burst",
                   "{} sectors -- catches non-uniform corruption only".format(len(pieces)))

    if args.verify_against:
        try:
            with open(args.verify_against, "rb") as fh:
                fh.seek(0, os.SEEK_END)
                local_total = fh.tell()
                if local_total != total:
                    res.bad("local copy matches", "local file is {} bytes, server says {}".format(
                        local_total, total))
                else:
                    okay = True
                    for start in (0, mid, last):
                        end = min(start + SECTOR - 1, total - 1)
                        fh.seek(start)
                        want_bytes = fh.read(end - start + 1)
                        got_bytes = check_quiet(conn, iso_path, start, end)
                        if got_bytes != want_bytes:
                            res.bad("local copy matches",
                                    "bytes differ at offset {}".format(start))
                            okay = False
                            break
                    if okay:
                        res.ok("local copy matches", "byte-exact at 3 offsets")
        except OSError as exc:
            res.bad("local copy matches", str(exc))
    else:
        res.skip("local copy matches",
                 "pass --verify-against to compare real bytes; without it a uniformly "
                 "shifted server still shows all green")

    # 5. Terminal errors must be terminal, and recognisable.
    print("\nTerminal errors")
    status, hdrs, body = get(conn, iso_path, (total, total + SECTOR - 1))
    if status == 416:
        note = "no Content-Range" if "content-range" not in hdrs else "with Content-Range"
        res.ok("range past EOF -> 416", note + " (both are accepted)")
    else:
        res.bad("range past EOF", "expected 416, got {}".format(status))

    missing = encode_path(base_path, "riptopl-conformance-no-such-file.iso")
    status, hdrs, body = get(conn, missing, (0, SECTOR - 1))
    if status == 404:
        res.ok("missing file -> 404")
    else:
        res.bad("missing file", "expected 404, got {}".format(status))

    # 6. The connection must have survived all of that.
    print("\nPersistence")
    try:
        again = check_range(res, conn, iso_path, 0, SECTOR - 1,
                            total, "reread after the error responses")
        if again is not None:
            res.ok("connection survived the error responses")
    except (http.client.HTTPException, OSError) as exc:
        res.bad("connection survived the error responses",
                "{} -- the client must reopen and retry".format(exc))

    conn.close()

    print("\n{} passed, {} failed, {} skipped".format(res.passed, res.failed, res.skipped))
    return 1 if res.failed else 0


if __name__ == "__main__":
    sys.exit(main())
