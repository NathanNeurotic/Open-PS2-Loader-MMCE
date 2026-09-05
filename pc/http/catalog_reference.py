#!/usr/bin/env python3
"""Reference implementation of the RiptOPL HTTP catalog (games.csv) contract.

This is the executable half of docs/HTTP-INTEGRATION-PLAN.md section 4. The C parser in
src/httpsupport.c must agree with this file on every fixture in fixtures/, in all three
line-ending forms. Where the two disagree, this file and expected.json are the contract and
the C is wrong -- unless the disagreement is deliberate, in which case change both here and
in the plan, in the same commit.

Run the self-test:

    python pc/http/catalog_reference.py --selftest

Parse a real catalog:

    python pc/http/catalog_reference.py path/to/games.csv
"""

import argparse
import json
import os
import sys

# Mirrors include/supportbase.h. A row that cannot fit the on-console record is rejected here
# rather than silently truncated, because truncating startup is how two discs end up sharing
# one per-game config file.
GAME_STARTUP_MAX = 12
ISO_GAME_NAME_MAX = 160

# Longest relative path httpsupport will carry. Chosen to leave room for percent-encoding
# inside the menu RPC's URI field once that field is widened (see plan section 2.3); the C
# side must define the same number in one place and assert against it.
CATALOG_PATH_MAX = 255

MEDIA_CD = 0x12
MEDIA_DVD = 0x14

# Extensions the catalog may name. .zso parses so that an existing catalog still lists, but it
# is marked unsupported: streaming compressed bytes as raw sectors would corrupt silently.
EXT_SUPPORTED = (".iso",)
EXT_KNOWN = (".iso", ".zso")


class Row:
    """One accepted catalog record."""

    def __init__(self, startup, title, media, path, supported, note):
        self.startup = startup
        self.title = title
        self.media = media
        self.path = path
        self.supported = supported
        self.note = note

    def as_dict(self):
        d = {
            "startup": self.startup,
            "title": self.title,
            "media": "CD" if self.media == MEDIA_CD else "DVD",
            "path": self.path,
            "supported": self.supported,
        }
        if self.note:
            d["note"] = self.note
        return d


class Reject:
    """One rejected line, with the reason a user-facing error would name."""

    def __init__(self, line_no, reason):
        self.line_no = line_no
        self.reason = reason

    def as_dict(self):
        return {"line": self.line_no, "reason": self.reason}


def split_fields(text):
    """RFC4180-style split on commas.

    A double quote only opens a quoted field at the very start of that field; inside a quoted
    field "" is a literal quote. Returns (fields, error) -- error is None on success.
    """
    fields = []
    i = 0
    n = len(text)
    while True:
        if i < n and text[i] == '"':
            i += 1
            buf = []
            while True:
                if i >= n:
                    return None, "unterminated_quote"
                if text[i] == '"':
                    if i + 1 < n and text[i + 1] == '"':
                        buf.append('"')
                        i += 2
                        continue
                    i += 1
                    break
                buf.append(text[i])
                i += 1
            # After a closing quote only a separator or end of line is legal.
            if i < n and text[i] != ",":
                return None, "text_after_quoted_field"
            fields.append("".join(buf))
        else:
            start = i
            while i < n and text[i] != ",":
                i += 1
            fields.append(text[start:i])
        if i >= n:
            return fields, None
        i += 1  # step over the comma
        if i == n:
            fields.append("")  # trailing comma yields a final empty field
            return fields, None


def has_control_chars(s):
    return any(ord(c) < 0x20 or ord(c) == 0x7F for c in s)


def split_extension(name):
    """Donor-compatible: move a trailing .iso/.zso off the title.

    Mirrors the donor's src/ethsupport.c:580-585 so that a legacy row whose title already
    carries the extension resolves to the same filename it resolves to today.
    """
    lowered = name.lower()
    for ext in EXT_KNOWN:
        if lowered.endswith(ext) and len(name) > len(ext):
            return name[: -len(ext)], name[-len(ext):].lower()
    return name, ".iso"


def validate_path(path):
    """Return an error reason, or None if the relative path is usable."""
    if path == "":
        return "empty_path"
    if has_control_chars(path):
        return "control_char_in_path"
    if len(path) > CATALOG_PATH_MAX:
        return "path_too_long"
    if "://" in path:
        return "absolute_url"
    if path.startswith("/"):
        return "absolute_path"
    if "\\" in path:
        return "backslash_in_path"
    segments = path.split("/")
    for seg in segments:
        if seg == "" or seg == ".":
            return "empty_segment"
        if seg == "..":
            return "parent_segment"
    lowered = path.lower()
    if not any(lowered.endswith(ext) for ext in EXT_KNOWN):
        return "unknown_extension"
    return None


def parse_line(raw, line_no):
    """Parse one already-comment-and-blank-filtered line. Returns (Row, Reject)."""
    fields, err = split_fields(raw)
    if err is not None:
        return None, Reject(line_no, err)

    # Five or more fields is rejected rather than folded into media. The donor splits only the
    # first two commas and treats everything after as the media field, which is exactly how a
    # four-field row containing "CD" anywhere gets misclassified.
    if len(fields) > 4:
        return None, Reject(line_no, "too_many_fields")

    startup = fields[0].strip()
    if startup == "":
        return None, Reject(line_no, "empty_startup")
    if has_control_chars(startup):
        return None, Reject(line_no, "control_char_in_startup")
    if len(startup) > GAME_STARTUP_MAX:
        return None, Reject(line_no, "startup_too_long")

    note = None

    # Field 2: title. Absent means the donor's startup-only form, where the startup doubles as
    # both the displayed name and the filename stem.
    if len(fields) >= 2 and fields[1].strip() != "":
        title_raw = fields[1].strip()
    else:
        title_raw = startup
        note = "startup_only_row"
    if has_control_chars(title_raw):
        return None, Reject(line_no, "control_char_in_title")
    if len(title_raw) > ISO_GAME_NAME_MAX:
        return None, Reject(line_no, "title_too_long")

    # Field 3: media. Exact token, unlike the donor's substring match.
    media = MEDIA_DVD
    if len(fields) >= 3 and fields[2].strip() != "":
        token = fields[2].strip().upper()
        if token == "CD":
            media = MEDIA_CD
        elif token == "DVD":
            media = MEDIA_DVD
        else:
            return None, Reject(line_no, "bad_media")

    # Field 4: the relative path, authoritative when present.
    if len(fields) >= 4 and fields[3].strip() != "":
        path = fields[3].strip()
        title = title_raw
    else:
        # Legacy derivation: the filename comes from the title, with the donor's extension rule.
        title, ext = split_extension(title_raw)
        path = title + ext

    err = validate_path(path)
    if err is not None:
        return None, Reject(line_no, err)

    supported = path.lower().endswith(EXT_SUPPORTED)
    if not supported:
        note = "compressed_image_unsupported"

    return Row(startup, title, media, path, supported, note), None


def parse_catalog(data):
    """Parse a whole catalog. `data` is bytes or str. Returns (rows, rejects)."""
    if isinstance(data, bytes):
        # Invalid UTF-8 is replaced rather than fatal: one bad byte in one title must not cost
        # the user their whole library. The C side does the same by copying bytes through.
        data = data.decode("utf-8", errors="replace")

    rows = []
    rejects = []
    seen = {}

    for idx, physical in enumerate(data.split("\n"), start=1):
        line = physical[:-1] if physical.endswith("\r") else physical
        stripped = line.lstrip(" \t")
        if stripped == "" or stripped.startswith("#"):
            continue
        row, reject = parse_line(stripped, idx)
        if reject is not None:
            rejects.append(reject)
            continue
        if row.startup in seen:
            # First record wins, so a refresh is stable rather than order-dependent.
            rejects.append(Reject(idx, "duplicate_startup"))
            continue
        seen[row.startup] = idx
        rows.append(row)

    return rows, rejects


def eol_variants(text):
    """The three forms every fixture must parse identically.

    Generated at read time on purpose: `* text=auto` in .gitattributes normalises committed
    CSVs to LF, so a committed CRLF fixture would silently check out as LF on CI and the test
    would pass for the wrong reason.
    """
    lf = text.replace("\r\n", "\n")
    return {
        "lf": lf,
        "crlf": lf.replace("\n", "\r\n"),
        "no_final_newline": lf[:-1] if lf.endswith("\n") else lf,
    }


def selftest(fixtures_dir):
    expected_path = os.path.join(fixtures_dir, "expected.json")
    with open(expected_path, "r", encoding="utf-8") as fh:
        expected = json.load(fh)

    failures = 0
    checked = 0

    names = sorted(n for n in os.listdir(fixtures_dir) if n.endswith(".csv"))
    unexpected = [n for n in names if n not in expected]
    missing = [n for n in expected if n not in names]
    for n in unexpected:
        print("FAIL {}: fixture has no entry in expected.json".format(n))
        failures += 1
    for n in missing:
        print("FAIL {}: expected.json names a fixture that does not exist".format(n))
        failures += 1

    for name in names:
        if name not in expected:
            continue
        with open(os.path.join(fixtures_dir, name), "r", encoding="utf-8", newline="") as fh:
            text = fh.read()
        want = expected[name]
        for form, variant in eol_variants(text).items():
            checked += 1
            rows, rejects = parse_catalog(variant)
            got = {
                "accept": [r.as_dict() for r in rows],
                "reject": [r.as_dict() for r in rejects],
            }
            # Line numbers shift by definition when the final newline is removed only if the
            # file ended with a blank line; fixtures avoid that, so numbers are comparable.
            if got != want:
                failures += 1
                print("FAIL {} [{}]".format(name, form))
                print("  want: " + json.dumps(want, sort_keys=True))
                print("  got:  " + json.dumps(got, sort_keys=True))

    print("{} checks over {} fixtures, {} failures".format(checked, len(names), failures))
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("catalog", nargs="?", help="games.csv to parse")
    ap.add_argument("--selftest", action="store_true",
                    help="parse every fixture and diff against expected.json")
    ap.add_argument("--fixtures", default=None, help="fixtures directory")
    ap.add_argument("--emit-expected", action="store_true",
                    help="regenerate expected.json from the current parser (review the diff!)")
    args = ap.parse_args()

    fixtures = args.fixtures or os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures")

    if args.emit_expected:
        out = {}
        for name in sorted(n for n in os.listdir(fixtures) if n.endswith(".csv")):
            with open(os.path.join(fixtures, name), "r", encoding="utf-8", newline="") as fh:
                rows, rejects = parse_catalog(fh.read())
            out[name] = {
                "accept": [r.as_dict() for r in rows],
                "reject": [r.as_dict() for r in rejects],
            }
        with open(os.path.join(fixtures, "expected.json"), "w", encoding="utf-8") as fh:
            json.dump(out, fh, indent=2, sort_keys=True)
            fh.write("\n")
        print("wrote " + os.path.join(fixtures, "expected.json"))
        return 0

    if args.selftest:
        return selftest(fixtures)

    if not args.catalog:
        ap.error("give a catalog path, or --selftest")

    with open(args.catalog, "rb") as fh:
        rows, rejects = parse_catalog(fh.read())
    for r in rows:
        flag = "" if r.supported else "  [unsupported: {}]".format(r.note)
        print("{:<12}  {:<3}  {:<40}  {}{}".format(
            r.startup, "CD" if r.media == MEDIA_CD else "DVD", r.title, r.path, flag))
    for j in rejects:
        print("line {}: rejected ({})".format(j.line_no, j.reason), file=sys.stderr)
    print("\n{} accepted, {} rejected".format(len(rows), len(rejects)), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
