#!/usr/bin/env bash
set -euo pipefail

git config user.name "NathanNeurotic Step Builder"
git config user.email "actions@users.noreply.github.com"
git fetch origin rebuild/step-212-apa-boot-and-bgm-resilience
git checkout rebuild/step-212-apa-boot-and-bgm-resilience
test "$(git rev-parse HEAD)" = "21d6742de080c9bb90ab1d5e05ef444d444179dd"

# The prior helper used Path.read_text()/write_text() on bdmsupport.c, whose repository form is CRLF.
# That normalized every line to LF and made a nine-token functional edit look like a whole-file
# rewrite. Rebuild the file byte-for-byte from the pre-follow-up head and apply only the intended
# view-query substitution so the production diff stays surgical.
python3 - <<'PY'
from pathlib import Path
import subprocess

base = subprocess.check_output([
    'git', 'show', '9a961663338083901974822db4feabee434127a1:src/bdmsupport.c'
])
old = b'vcdViewActive(itemList->mode)'
new = b'vcdListViewActive(itemList)'
count = base.count(old)
if count != 9:
    raise SystemExit(f'bdmsupport.c: expected 9 original view checks, got {count}')
fixed = base.replace(old, new)
Path('src/bdmsupport.c').write_bytes(fixed)
PY

git diff --check
# Guard that this correction is now genuinely small rather than another newline rewrite.
changed_lines=$(git diff --numstat -- src/bdmsupport.c | awk '{print $1+$2}')
if [ -z "$changed_lines" ] || [ "$changed_lines" -gt 30 ]; then
    echo "bdmsupport.c correction unexpectedly large: ${changed_lines:-none} changed lines"
    exit 1
fi

git add src/bdmsupport.c
git commit -m "rebuild-212: preserve BDM source line endings"
git push origin HEAD:rebuild/step-212-apa-boot-and-bgm-resilience
