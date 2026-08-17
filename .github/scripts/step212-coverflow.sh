#!/usr/bin/env bash
set -euo pipefail

git config user.name "NathanNeurotic Step Builder"
git config user.email "actions@users.noreply.github.com"
git fetch origin rebuild/step-212-apa-boot-and-bgm-resilience
git checkout rebuild/step-212-apa-boot-and-bgm-resilience
test "$(git rev-parse HEAD)" = "6575f9fcd0ed441bf9cbabdad2456377d2184205"

python3 - <<'PY'
from pathlib import Path
p = Path('src/sound.c')
text = p.read_text()
old = '''#define BGM_RING_BUFFER_COUNT       192 // 768 KB buffer (~4.35s): extra headroom for long device/list bursts (#364)\n#define BGM_RING_BUFFER_SIZE        4096\n#define BGM_STOP_WAIT_SLICES        16\n#define BGM_IO_LOW_WATER_CHUNKS     48 // ~1.1 s: stop STARTING discretionary device reads\n#define BGM_IO_RESUME_WATER_CHUNKS  80 // ~1.8 s: hysteresis before artwork/cosmetic IO resumes\n'''
new = '''#define BGM_RING_BUFFER_COUNT      192 // 768 KB buffer (~4.35s): extra headroom for long device/list bursts (#364)\n#define BGM_RING_BUFFER_SIZE       4096\n#define BGM_STOP_WAIT_SLICES       16\n#define BGM_IO_LOW_WATER_CHUNKS    48 // ~1.1 s: stop STARTING discretionary device reads\n#define BGM_IO_RESUME_WATER_CHUNKS 80 // ~1.8 s: hysteresis before artwork/cosmetic IO resumes\n'''
if text.count(old) != 1:
    raise SystemExit('sound.c: macro block did not match exactly once')
text = text.replace(old, new, 1)
old2 = '''#define BGM_THREAD_BASE_PRIO  0x1E\n#define BGM_THREAD_STACK_SIZE 0x1000\n'''
new2 = '''#define BGM_THREAD_BASE_PRIO       0x1E\n#define BGM_THREAD_STACK_SIZE      0x1000\n'''
if text.count(old2) != 1:
    raise SystemExit('sound.c: thread macro block did not match exactly once')
p.write_text(text.replace(old2, new2, 1))
PY

git diff --check
git add src/sound.c
git commit -m "rebuild-212: clang-format BGM watermarks"
git push origin HEAD:rebuild/step-212-apa-boot-and-bgm-resilience
