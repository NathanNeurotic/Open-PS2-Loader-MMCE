#!/usr/bin/env bash
set -euo pipefail

prod_branch="rebuild/step-212-apa-boot-and-bgm-resilience"
expected_head="49474c4c2a79d0805558ef0a263141eb41424c25"

git fetch origin "$prod_branch"
actual_head="$(git rev-parse "origin/$prod_branch")"
if [[ "$actual_head" != "$expected_head" ]]; then
    echo "Refusing production edit: expected $expected_head, got $actual_head" >&2
    exit 1
fi

git checkout -B "$prod_branch" "origin/$prod_branch"

python3 - <<'PY'
from pathlib import Path


def edit(path, replacements):
    p = Path(path)
    raw = p.read_bytes()
    crlf = b"\r\n" in raw
    text = raw.decode("utf-8").replace("\r\n", "\n")
    for old, new in replacements:
        count = text.count(old)
        if count != 1:
            raise SystemExit(f"{path}: expected one match, got {count}")
        text = text.replace(old, new, 1)
    if crlf:
        text = text.replace("\n", "\r\n")
    p.write_bytes(text.encode("utf-8"))

edit("src/hddsupport.c", [
    (
        '            g->format = GAME_FORMAT_ISO;                                  // VCD flag gates launch\n',
        '            g->format = GAME_FORMAT_ISO;                                    // VCD flag gates launch\n',
    ),
])

edit("include/hddsupport.h", [
    (
        '// Enumerate the present classic-container and one-game APA/PFS partitions. Fills a sorted, deduped\n// list; returns the count (0 on none/error). Free via hddFreePopsPartitionList.\n',
        '// Enumerate the present classic-container and one-game APA/PFS partitions. Fills a sorted, deduped\n// list; returns the count, 0 on a complete walk with none, or a negative error for an incomplete walk.\n// Free via hddFreePopsPartitionList.\n',
    ),
])
PY

git diff --check

git config user.name "NathanNeurotic Step 212 helper"
git config user.email "109461996+NathanNeurotic@users.noreply.github.com"
git add src/hddsupport.c include/hddsupport.h
git commit -m "rebuild-212: polish APA enumeration contracts"
git push origin HEAD:"$prod_branch"
