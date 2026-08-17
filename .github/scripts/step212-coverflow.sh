#!/usr/bin/env bash
set -euo pipefail

git config user.name "NathanNeurotic Step Builder"
git config user.email "actions@users.noreply.github.com"
git fetch origin rebuild/step-212-apa-boot-and-bgm-resilience
git checkout rebuild/step-212-apa-boot-and-bgm-resilience
test "$(git rev-parse HEAD)" = "2bb3981badf886c7a241bdc89cb531714748e676"

python3 - <<'PY'
from pathlib import Path
p = Path('src/texcache.c')
text = p.read_text()
old = '''        req->epoch = gArtEpoch;         // the view this cover belongs to; see cacheDropQueuedArt()\n        req->failEpoch = gArtFailEpoch; // the generation an "absent" answer would belong to\n        req->focusEpoch = gArtFocusEpoch; // selected-game neighborhood this speculative read serves\n'''
new = '''        req->epoch = gArtEpoch;           // the view this cover belongs to; see cacheDropQueuedArt()\n        req->failEpoch = gArtFailEpoch;   // the generation an "absent" answer would belong to\n        req->focusEpoch = gArtFocusEpoch; // selected-game neighborhood this speculative read serves\n'''
if text.count(old) != 1:
    raise SystemExit('clang-format alignment target did not match exactly once')
p.write_text(text.replace(old, new, 1))
PY

git diff --check -- src/texcache.c
git add src/texcache.c
git commit -m "rebuild-212: fix art focus formatting"
git push origin HEAD:rebuild/step-212-apa-boot-and-bgm-resilience
