#!/usr/bin/env bash
set -euo pipefail

prod_branch="rebuild/step-212-apa-boot-and-bgm-resilience"
expected_head="0424050ccd479e07aafd1f28cf47b9892fb00291"

git fetch origin "$prod_branch"
actual_head="$(git rev-parse "origin/$prod_branch")"
if [[ "$actual_head" != "$expected_head" ]]; then
    echo "Refusing production edit: expected $expected_head, got $actual_head" >&2
    exit 1
fi

git checkout -B "$prod_branch" "origin/$prod_branch"

python3 - <<'PY'
from pathlib import Path


def replace_once(path, old, new):
    p = Path(path)
    raw = p.read_bytes()
    crlf = b"\r\n" in raw
    text = raw.decode("utf-8").replace("\r\n", "\n")
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one match, got {count}")
    text = text.replace(old, new, 1)
    if crlf:
        text = text.replace("\n", "\r\n")
    p.write_bytes(text.encode("utf-8"))

replace_once(
    "src/hddsupport.c",
    "    char (*oldParts)[APA_IDMAX + 1];\n",
    "    char(*oldParts)[APA_IDMAX + 1];\n",
)
replace_once(
    "src/menusys.c",
    "complete_request:\n        WaitSema(menuSemaId);\n",
    "    complete_request:\n        WaitSema(menuSemaId);\n",
)
PY

git diff --check

git config user.name "NathanNeurotic Step 212 helper"
git config user.email "109461996+NathanNeurotic@users.noreply.github.com"
git add src/hddsupport.c src/menusys.c
git commit -m "rebuild-212: apply review follow-up formatting"
git push origin HEAD:"$prod_branch"
