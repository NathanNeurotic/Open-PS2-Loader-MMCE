#!/usr/bin/env python3
"""Diff the C catalog parser against the Python reference, fixture by fixture.

docs/HTTP-INTEGRATION-PLAN.md says src/httpcatalog.c and pc/http/catalog_reference.py are one
contract. This is what holds them to it. Every fixture is run through both, in all three
line-ending forms, and any disagreement is printed and fails the run.

    python pc/http/tests/compare_catalog.py --bin /tmp/test_catalog

Build the C side first (pc/http/tests/run.sh does both).
"""

import argparse
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "pc", "http"))
import catalog_reference  # noqa: E402

# enum HTTP_CAT_REJECT in include/httpcatalog.h, in declaration order.
REJECT_NAMES = [
    "ok",
    "empty_startup",
    "startup_too_long",
    "control_char",
    "title_too_long",
    "bad_media",
    "too_many_fields",
    "unterminated_quote",
    "text_after_quoted_field",
    "empty_path",
    "path_too_long",
    "absolute_url",
    "absolute_path",
    "backslash_in_path",
    "empty_segment",
    "parent_segment",
    "unknown_extension",
]

# The Python side names a few things per-field that the C side folds into one control-char reason.
# Mapping them here rather than renaming either side keeps each parser's messages useful on its own.
PY_TO_SHARED = {
    "control_char_in_startup": "control_char",
    "control_char_in_title": "control_char",
    "control_char_in_path": "control_char",
}


def shared(reason):
    return PY_TO_SHARED.get(reason, reason)


def run_c(binary, text):
    with tempfile.NamedTemporaryFile("wb", suffix=".csv", delete=False) as fh:
        fh.write(text.encode("utf-8"))
        path = fh.name
    try:
        out = subprocess.run([binary, path], capture_output=True, text=True, check=True).stdout
    finally:
        os.unlink(path)

    acc, rej = [], []
    for line in out.splitlines():
        if line.startswith("ACC "):
            startup, title, media, p, supported = line[4:].split("|")
            acc.append((startup, title, media, p, supported == "1"))
        elif line.startswith("REJ "):
            lineno, reason = line[4:].split("|")
            rej.append((int(lineno), REJECT_NAMES[int(reason)]))
        elif line.startswith("DUP "):
            rej.append((int(line[4:]), "duplicate_startup"))
    return acc, rej


def run_py(text):
    rows, rejects = catalog_reference.parse_catalog(text)
    acc = [(r.startup, r.title, "CD" if r.media == catalog_reference.MEDIA_CD else "DVD",
            r.path, r.supported) for r in rows]
    rej = [(j.line_no, shared(j.reason)) for j in rejects]
    return acc, rej


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", required=True, help="compiled test_catalog binary")
    ap.add_argument("--fixtures", default=os.path.join(ROOT, "pc", "http", "fixtures"))
    args = ap.parse_args()

    names = sorted(n for n in os.listdir(args.fixtures) if n.endswith(".csv"))
    failures = 0
    compared = 0

    for name in names:
        with open(os.path.join(args.fixtures, name), "r", encoding="utf-8", newline="") as fh:
            text = fh.read()
        for form, variant in catalog_reference.eol_variants(text).items():
            compared += 1
            c_acc, c_rej = run_c(args.bin, variant)
            p_acc, p_rej = run_py(variant)

            if c_acc != p_acc:
                failures += 1
                print("MISMATCH accept {} [{}]".format(name, form))
                for a, b in zip(c_acc + [None] * len(p_acc), p_acc + [None] * len(c_acc)):
                    if a != b:
                        print("    C : {}".format(a))
                        print("    py: {}".format(b))
            c_reasons, p_reasons = c_rej, p_rej
            if c_reasons != p_reasons:
                failures += 1
                print("MISMATCH reject {} [{}]".format(name, form))
                print("    C : {}".format(c_reasons))
                print("    py: {}".format(p_reasons))

    print("{}: {} comparisons over {} fixtures, {} mismatches".format(
        "FAILED" if failures else "PASSED", compared, len(names), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
