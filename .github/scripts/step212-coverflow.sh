#!/usr/bin/env bash
set -euo pipefail

prod_branch="rebuild/step-212-apa-boot-and-bgm-resilience"
expected_head="a6c930b6dc3f13d0df584bc644188b7c8b77a9fc"

git fetch origin "$prod_branch"
actual_head="$(git rev-parse "origin/$prod_branch")"
if [[ "$actual_head" != "$expected_head" ]]; then
    echo "Refusing production edit: expected $expected_head, got $actual_head" >&2
    exit 1
fi

git checkout -B "$prod_branch" "origin/$prod_branch"

python3 - <<'PY'
from pathlib import Path


def load(path):
    p = Path(path)
    raw = p.read_bytes()
    crlf = b"\r\n" in raw
    return p, raw.decode("utf-8").replace("\r\n", "\n"), crlf


def save(p, text, crlf):
    if crlf:
        text = text.replace("\n", "\r\n")
    p.write_bytes(text.encode("utf-8"))


def replace_once(path, old, new):
    p, text, crlf = load(path)
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}")
    save(p, text.replace(old, new, 1), crlf)

replace_once(
    "src/vcdsupport.c",
    '''    int count = 0;\n    struct dirent *de;\n    while (count < VCD_MAX_ITEMS && (de = readdir(dir)) != NULL) {\n        int len = (int)strlen(de->d_name);\n''',
    '''    int count = 0;\n    struct dirent *de;\n    while (count < VCD_MAX_ITEMS) {\n        // POSIX readdir() uses the same NULL result for clean end-of-directory and for an error.\n        // Clear errno immediately before each call so a nonzero value on NULL belongs to this read,\n        // not to earlier work in the loop. Publishing the entries collected before a read failure\n        // as a complete list would undo the transactional list ownership in every caller.\n        errno = 0;\n        de = readdir(dir);\n        if (de == NULL) {\n            if (errno != 0) {\n                closedir(dir);\n                free(list);\n                return -1; // incomplete scan: caller preserves its last-good backing list\n            }\n            break; // clean end-of-directory\n        }\n\n        int len = (int)strlen(de->d_name);\n''')

p, text, crlf = load("HANDOFF.md")
note = '''\n## Step 212 follow-up — VCD directory walks distinguish EOF from failure\n\n- `vcdScanOpenDir()` no longer treats every `readdir()` NULL as clean EOF. It clears `errno` before\n  each read and returns scan failure when NULL arrives with an error, discarding the partial candidate\n  list so callers retain their last-good backing list.\n- Reaching `VCD_MAX_ITEMS` remains the intentional list-cap condition; a normal EOF still returns the\n  completed scan.\n- With this change the HDL APA walk, POPS APA-partition walk, and VCD directory walk all reject\n  incomplete enumeration rather than publishing it as authoritative. No ATA/DEV9 behavior changed.\n'''
if "## Step 212 follow-up — VCD directory walks distinguish EOF from failure" not in text:
    text = text.rstrip("\n") + "\n" + note
save(p, text, crlf)
PY

git diff --check

git config user.name "NathanNeurotic Step 212 helper"
git config user.email "109461996+NathanNeurotic@users.noreply.github.com"
git add src/vcdsupport.c HANDOFF.md
git commit -m "rebuild-212: reject partial VCD directory scans"
git push origin HEAD:"$prod_branch"
