# RiptOPL — handoff, 2026-08-13

You are picking up work on **RiptOPL**, Nathan's (NathanNeurotic / "Ripto") fork of **Open PS2
Loader** — PlayStation 2 homebrew, C, EE (MIPS R5900, 294 MHz, 32 MB) + IOP co-processor.
Everything below is state you need. Read it all before touching anything; several traps here have
each cost a full day.

---

#---

# 0. THE WORKING CONTRACT -- do not deviate from this

**WORK IN THIS DIRECTORY:**
`C:\Users\natha\Github\Open-PS2-Loader\.claude\worktrees\opl-issue-340-diagnosis-2cdb77`

It is already on the tip branch, it holds this file, and **it is the one directory the build
container is mounted to** (`oplbuild92` exposes it as `/src`). Do NOT `git worktree add` a fresh
directory: the container would still build this one, so you would edit one tree and compile
another and see no effect from your changes. Branch with `git checkout -b` INSIDE it. The main
checkout at `C:/Users/natha/Github/Open-PS2-Loader` is on `rebuild/main` and does not even
contain this file.


Nathan's side and his testers' side must look identical across a handover. These are not suggestions.

**BRANCHES.** Never commit to `master`, `rebuild/main`, or any existing `rebuild/step-*` branch.
Every change goes on a NEW branch you create, `rebuild/step-NNN-<slug>`, branched from the current
tip. **Next number: 185. Current tip: `rebuild/step-184-bgm-theme-fallback-restore`.** One focused change
per step, with a long explanatory commit message -- what changed, why, what evidence drove it, and
what it does NOT fix. Those messages are this project's real documentation. Never force-push, never
rewrite history, never merge to `master` without asking. (`rebuild/main` moves only as the
fast-forward publish knob, on Nathan's named tip — see §17; `checkpoint/2026-08-13-rolling-live`
is a frozen reference, never to be committed on, force-pushed, or deleted.)

**THE LOOP, unchanged:** build locally in `oplbuild92` -> run clang-format 12 on every touched
`.c`/`.h` -> push the branch -> `gh workflow run flavours.yml --repo NathanNeurotic/Open-PS2-Loader
--ref <branch>` -> **watch it to completion** -> only then hand over links. Never give Nathan a link
from a run you have not seen go green on all four jobs. Always pass `--repo` or gh targets upstream
and fails with "No commits between".

**THE LINK FORMAT, verbatim, every time:**

```
## Test — run <runId> (`rebuild/step-NNN-<slug>`)

**For testers:**
- https://nightly.link/NathanNeurotic/Open-PS2-Loader/actions/runs/<runId>/OPL-PS2DEVPINNED.zip
- https://nightly.link/NathanNeurotic/Open-PS2-Loader/actions/runs/<runId>/OPL-OFFICIALPINNED.zip

**Early warning:**
- https://nightly.link/NathanNeurotic/Open-PS2-Loader/actions/runs/<runId>/OPL-PS2DEVROLLING.zip
- https://nightly.link/NathanNeurotic/Open-PS2-Loader/actions/runs/<runId>/OPL-OFFICIALROLLING.zip
```

PINNED goes to testers (digest-locked, reproducible, comparable weeks later). ROLLING is early
warning: a rolling build breaking while its pinned twin does not is the toolchain, not us.

**WITH EVERY BUILD:** say what to look for, name the HUD fields worth reading, and name the fallback
build if it misbehaves. **Do not spam builds** -- stage on branches and let Nathan decide when a
tester gets a link (his tester Zack asked for exactly this).

---

# 0b. ENVIRONMENT TRAPS — every one of these cost the previous agent real time

None of this is guessable from the code. It is written down because each item was learned by
breaking something.

## Editing files

- **Source files are CRLF.** `src/*.c`, `include/*.h`, the workflows — all of them. If you script an
  edit in Python, read with `newline=''`, normalise to `\n` to do your matching, then **write the
  CRLF back**. Get this wrong and git reports the entire file as changed, burying your actual diff
  and making review impossible.
- **`src/bdmsupport.c` contains literal NUL bytes.** grep/ripgrep call it binary and skip it. Use
  `grep -a`, or read it directly. It is one of the largest and most important files in the tree — do
  not conclude "no matches" from a silent grep.
- **clang-format REFLOWS what you just wrote.** If you write a function, format, and then try to
  patch it again with a string keyed on your original text, the match fails. Either patch before
  formatting, or re-read the file and key on what is actually there now.
- **The Edit tool refuses a file that changed since you read it** — and formatting counts as a
  change. Re-read, then edit.

## Shell quoting (this wasted the most time by far)

- **Do not put long content in a bash heredoc.** A single apostrophe in prose (`the reporter's
  library`) or a stray backtick will break `<<'PY'` and you get `unexpected EOF while looking for
  matching`. It fails *after* partially running in some cases, leaving the tree half-edited.
- **Write the script to a file and run it.** `Write` the Python to the scratchpad, then
  `python <path>`. This is the reliable pattern; use it by default for anything longer than one line.
- **`\n` inside a Python string that becomes C source turns into a REAL newline.** Writing
  `LOG("...\n")` from Python produces a broken multi-line string literal and a compile error about
  an unterminated macro argument. Build the escape explicitly, e.g. `chr(92) + 'n'`.
- **Git paths get mangled by MSYS on Windows.** `git show "origin/master:.github/workflows/x.yml"`
  becomes a backslash path and fails with "ambiguous argument". Export `MSYS_NO_PATHCONV=1` first, or
  use `git grep <pattern> <ref> -- <path>` instead.

## Git

- **`git checkout -b` carries uncommitted changes with you, and `git commit` lands on whatever
  branch you are currently on.** The previous agent committed a change onto the `step-162` branch and
  *then* created `step-163` pointing at that same commit — so the local `162` label no longer matched
  the artifact Nathan had been given. **Create the new branch BEFORE you commit, and check
  `git rev-parse --abbrev-ref HEAD` before every commit.**
- `git checkout -- '*.sh'` before committing: the build container strips CRLF from shell scripts and
  they show up as spurious modifications.
- Verify with `git status --short` that only the files you meant to touch are staged.

## GitHub / gh

- **Always `--repo NathanNeurotic/Open-PS2-Loader`.** Without it gh targets upstream ps2homebrew and
  fails with "No commits between".
- **Read the WHOLE issue thread, not the tail.** The previous agent read the last two comments of
  #382, told Nathan to ask the tester which flavour he used, and was corrected — the tester had named
  all three in the message just above. `gh issue view <n> --repo ... --json body,comments` and read
  it all.
- **Artifact names changed** with the four-flavour switch. Links older than 2026-08-13 point at
  `OPL-PS2DEVLATESTSDK` / `OPL-PS2DEVPINNEDSDK` / `OPL-OFFICIALSDK`, which no longer exist. Do not
  copy an old link block. The mapping (verified against workflow history): OFFICIALSDK →
  OFFICIALROLLING (same :main image), PS2DEVLATESTSDK → PS2DEVROLLING, PS2DEVPINNEDSDK →
  PS2DEVPINNED but the pin MOVED (c64ae69c → 8fba50ec, not like-for-like). OFFICIALPINNED is a
  2026-08-13 digest pin with no predecessor.
- **Native Windows python cannot see MSYS `/tmp`.** `python` here is a Windows build: `open('/tmp/x')`
  writes to `C:\tmp\x`, and bash then cannot find `/tmp/x`. `cd` into the target directory and use
  RELATIVE paths for python file I/O, or pass bash the Windows path. Bit twice on release-body work.
- **The build container cannot run git in this worktree.** A worktree's `.git` is a FILE whose
  gitdir pointer is host-absolute (`C:\Users\...`), dead inside `oplbuild92` — so in-container
  `make` prints the version as `-dirty` with `expr: syntax error` noise. PRE-EXISTING, not your
  change: judge local builds by the ELF timestamp, and validate Makefile git logic in git bash,
  never in the container.
- **Pushing to `master` FIRES THE RELEASE PIPELINE.** rolling-release.yml triggers on push to
  master (and v* tags, and rebuild/main). The PR #463 template merge accidentally published an
  old-lineage rolling release this way (run 31724547932, deleted). Nothing goes to master until
  Nathan orders the cutover — see §17.

## Build output

- **`expr: syntax error` lines are pre-existing noise** from the version-string makefile rules. They
  appear on every successful build. Judge success by the ELF timestamp/size and by grepping for
  `error:` specifically — not by "the output looked messy".
- There is **no `-Wall`**. The compiler will not warn you about an unused result, a dead field, or a
  contract you half-implemented.

## Other people's files

- `C:\Users\natha\Github\Open-PS2-Loader\agent-file-drop` is where Nathan drops files from
  testers (currently two themes attached to issue #380 that correctly display PS1 game IDs). Check it
  when he references an attachment.

---

# 0c. PROCESS RULES the previous agent learned by getting them wrong

These are not general advice. Each one names a specific build that went out wrong.

1. **Do not ship a fix aimed at a counter you have not read yet.** rebuild-158 enlarged a buffer to
   fix the VCD page; the very next capture showed the counter for that condition reading **0**. The
   fix was correct in principle and irrelevant in fact. If you have just added an instrument, wait
   for the reading.
2. **Turning a stub into a real function is an API change — audit every caller first.** rebuild-142
   made `cacheCancelPendingImageLoads()` real and sticky; `themes.c` calls it on the first theme load
   of every boot, so **all cover art died on every boot**. Nathan found it, not the compiler.
3. **After adding an invalidation generation, grep every existing caller of the invalidator.**
   rebuild-153 added a fail generation so missing art would stop being re-probed; an existing call in
   `updateMenuFromGameList` bumped that generation on *every list rebuild*, silently undoing it.
   Fixed only in 155.
4. **Verify a value actually reaches the screen.** rebuild-165 and 166 resolved PS1 disc ids and were
   **completely inert** — `sbPopulateConfig` re-derives the displayed value from the filename further
   down. Two builds, no effect, discovered only by reading the consumer.
5. **Every counter must distinguish "not used yet" from "broken."** `IX0/0` cost two build cycles
   because a subsystem that never initialised looked identical to an idle one. `OE` could not see the
   SMB bug it was built for, because it only incremented on a code path SMB never takes. When you add
   a counter, ask what its zero means.
6. **Diff the fork before theorising.** Three separate bugs this week were "master does X, we do
   not," each found only after a tester hit it: the cache reunion, the per-row config gate, and the
   transient-failure retry lane. `git show origin/master:src/FILE` is the cheapest debugging tool
   available here.
7. **Adapt, do not transplant.** A wholesale `texcache.c` swap was started and reverted: master's
   file is missing eight symbols this tree calls, plus the MX4SIO/SIO2 protection that fixed #340.
   Port the mechanism, not the file.
8. **Say when something is unproven.** Two 16-agent adversarial investigations this session ended
   with *nothing surviving refutation*. That is a real and useful result — it eliminated four
   hypotheses — and it is more valuable to Nathan than a confident guess would have been.
9. **Never quote a version string you haven't read off a built artifact.** `git rev-list --count`
   in a shallow/local worktree returns a partial local count (e.g. ~313) while CI unshallows to the
   full 2555+ history. Quoting locally computed revisions causes false alarms or downgrades; always
   read the actual `OPL_VERSION` string directly from CI build logs or packaged artifact filenames.
10. **SDK structs have external readers in compiled SDK binaries.** When running dead-contract or
    unused-field sweeps, distinguish internal OPL structs from SDK structs (`gsKit`, `ps2sdk`).
    Fields like `GSTEXTURE.VramClut` may be initialized in OPL and consumed inside external
    pre-compiled SDK functions (`gsKit_TexManager_bind`) with zero readers in OPL's source tree.

# 1. Ground truth: repo, branches, where to work

Repo: `https://github.com/NathanNeurotic/Open-PS2-Loader`
Local checkout: `C:\Users\natha\Github\Open-PS2-Loader` — sits on branch **`rebuild/main`**.
**Do not commit to it, do not commit to `master`, do not commit to any `rebuild/step-*` branch.**

Two lineages, both ours, and the distinction matters constantly:

| name | branch | what it is |
|---|---|---|
| **"the fork"** | `origin/master` | The mature, shipped fork. Battle-tested. **Faster at art than the rebuild.** The reference to compare against. |
| **"the rebuild"** | `rebuild/step-164-eth-bounded-teardown` (tip) | A from-scratch re-port, being hardened. **This is what you are working on.** |

`git show origin/master:src/FILE` reads the fork's copy of any file without checking it out. **Do
this constantly** — most bugs this week were found by diffing the two.

Start work in your own worktree:

```bash
cd C:/Users/natha/Github/Open-PS2-Loader
git fetch origin
git worktree add .claude/worktrees/<yourname> -b <yourname>/<topic> origin/rebuild/step-164-eth-bounded-teardown
cd .claude/worktrees/<yourname>
```

Numbered "rebuild-NNN" steps are sequential branches `rebuild/step-NNN-<slug>`. The next one is
**169**. (164 through 168 all landed on the `step-164` branch -- the numbering is a log, not a rule.) Each is one focused change with a long explanatory commit message — **keep that convention,
the commit messages are the project's real documentation.**

---

## 2. Build, format, ship — the exact loop

**Build** (Docker container `oplbuild92` is running, repo mounted at `/src`):

```bash
docker exec oplbuild92 sh -c "cd /src && rm -f obj/*.o opl.elf && make -j8 opl.elf"
```

⚠ `/src` is bound to **one specific worktree** (`opl-issue-340-diagnosis-2cdb77`), not to yours.
Building compiles whatever is in *that* directory. Either work there or adjust the mount.
⚠ `expr: syntax error` lines in build output are **pre-existing noise** from the version-string
makefile rules. Ignore them; check the ELF timestamp and size instead.

**Format** — CI enforces clang-format **12** and it is not a required check, so a red format check
still lets a merge through. Run it yourself:

```bash
docker run --rm -v "//c/Users/natha/Github/Open-PS2-Loader/.claude/worktrees/<yours>://work" \
  -w //work xianpengshen/clang-tools:12 clang-format --style=file -i <changed .c/.h files>
```

**CI + test builds** — three SDK flavours:

```bash
gh workflow run flavours.yml --repo NathanNeurotic/Open-PS2-Loader --ref <branch>
gh run watch <runId> --repo NathanNeurotic/Open-PS2-Loader --exit-status
```

⚠ **Always pass `--repo NathanNeurotic/Open-PS2-Loader`** — `gh` otherwise targets upstream and
fails with "No commits between".

Then hand Nathan the nightly.link URLs. **FOUR flavours as of 2026-08-13**, on two axes — which
toolchain, and pinned vs rolling:

```
https://nightly.link/NathanNeurotic/Open-PS2-Loader/actions/runs/<runId>/OPL-PS2DEVPINNED.zip
https://nightly.link/NathanNeurotic/Open-PS2-Loader/actions/runs/<runId>/OPL-OFFICIALPINNED.zip
https://nightly.link/NathanNeurotic/Open-PS2-Loader/actions/runs/<runId>/OPL-PS2DEVROLLING.zip
https://nightly.link/NathanNeurotic/Open-PS2-Loader/actions/runs/<runId>/OPL-OFFICIALROLLING.zip
```

**PINNED is what testers should use** — digest-locked, reproducible, comparable across weeks. If a
pinned build breaks, it is us. **ROLLING is early warning**: if a rolling build breaks while its
pinned twin does not, that is upstream's bug, not ours — a distinction that could not be drawn
before. Every artifact carries `BUILD-MANIFEST.txt` (flavour, image, digest, commit, compiler
versions probed after the build) so a report identifies exactly what ran.

⚠ The pins are a dated decision, not a snapshot to refresh casually: moving one invalidates
comparability with every report gathered against it. Bump on purpose and expect to re-baseline.

**He always wants all of them.** He tests on real hardware — a physical PS2 with a USB stick, an
MX4SIO SD adapter, an internal ATA drive, and memory cards. There is no emulator path for the
things being worked on.

---

## 3. The debug HUD — you cannot work on this without it

An on-screen line, enabled in OPL's settings, is the primary instrument. Nathan sends **videos**;
frames are extracted with ffmpeg and the line is read off them. Current format (`src/gui.c`, search
`artdbg`):

```
Q<n> A<n> D<n> X<n> <totalMs>ms(ok <okMs> <W>x<H>) O:<openMs>/<missOpenMs> W<ms>@<simple>/<menu>/<bgm><h|m> OE<n> IX<dirs>/<absent>/<failed> KL<n> TF<n> SX<stale>/<full> SP<last>/<max>  F<frameMs>/<worstMs> OV<n>  NR<n> MT<n>  IO <pendSimple>/<pendMenu> T<totSimple>/<totMenu>
```

| field | meaning |
|---|---|
| `Q` / `A` / `D` / `X` | art requests queued / active / done / dropped-without-loading. **D and X are complements** — every request ends in exactly one. Both flat while covers are wanted = a leak. X racing while D crawls = over-cancelling. |
| `<totalMs>ms(ok <okMs> <W>x<H>)` | cost of the last art load, and of the last **successful** one with its decoded pixel size. |
| `O:<openMs>/<missOpenMs>` | cost of the last staged `open()`, and of the last one that **failed**. **The most important field right now.** |
| `OE` | staged opens that failed with errno != ENOENT. Should stay 0. If it climbs, transient bus errors are branding real art "absent" and `texStagedOpenIsAbsence` needs master's ENOENT-only rule. |
| `IX<dirs>/<absent>/<failed>` | art-index directories held / probes answered "absent" from RAM / sweeps that failed or were rejected. |
| `KL` | rows whose value was too long for the reunion key. Should be 0. |
| `F`/`OV` | last & worst frame ms, frames over ~1.5 vsyncs. |
| `NR`/`MT` | pad fault counters: longest run of unreadable polls / longest run of empty ones. |
| `IO a/b T c/d` | the shared ioman queue: pending SIMPLEACTION / pending MENU_UPDATE, then lifetime totals. |
| `W<ms>@<s>/<m>/<bgm><h\|m>` | worst `open()` of the session, with the ioman queue depths captured AT that open, whether the BGM decoder was mid-read, and hit/miss. THE open-spike instrument (§14). |
| `TF` | transient-failure art retries (167). ~0 on a healthy device; climbing = a flaky share being retried instead of branded absent. |
| `SX<stale>/<full>` | SFX silently discarded (169): cursor ticks aged out (harmless) / ANY sound killed by a saturated ring (the press-eater, #364). |
| `SP<last>/<max>` | per-press audsrv RPC wall ms on the dispatch thread — the since-#340 diag nothing read until 169. SP huge + SX flat = slow IOP; SP small + SX/full climbing = starved dispatcher. |

**Extracting a HUD line from one of Nathan's videos:**

```bash
ffmpeg -y -i "video.mp4" -vf "fps=1,crop=iw:ih/9:0:ih*8/9,scale=1400:-1,tile=1x14" hud_%02d.png
```

That crops the bottom ninth (where the HUD sits), one frame per second, tiled 14-up. Read the PNGs.

**The single most important lesson of this session:** a counter that reads the same when a subsystem
is *idle* and when it is *completely broken* is worthless. `IX0/0` cost two whole builds because
"never used" and "dead on arrival" were indistinguishable. **Every new counter must be able to
distinguish "not used yet" from "tried and failed".**

---

## 4. What has been done (rebuild-137 → 160)

Condensed. Full reasoning is in each commit message — read them, they are detailed on purpose.

**Issue #340 (input skipping / navigation)** — *closed*. Root cause: `mx4sio_bd.irx` was being left
resident on the **pad's own bus** (SIO2) by the boot resolver on every untyped `massN:` boot. Fixed
in rebuild-135. For users who genuinely enable MX4SIO, some contention is **inherent** — the SD
driver shares the controller bus. Not a bug to chase further.

**rebuild-152 — art got its own EE thread.** Previously art rode the shared ioman FIFO worker, so a
list rebuild or config save queued *behind* a page of covers. Now: dedicated thread, own queue,
priority 64. Deliberately **not** a port of master's texcache (that would have deleted four fixes);
only the transport changed. Master's `gArtSemaId` was rejected — it locks the GUI thread on every
`cacheGetTexture` including cache hits, an unbounded priority inversion on the thread that reads the
controller.

**rebuild-153 — cache reunion + fail generation.** `cache_entry_t.key` had shipped with a comment
explaining what it was for and **no code behind it**; wired up. Every list rebuild had been
re-reading art the cache already held. Also narrowed the art idle-gate to SIO2 devices only (master
gates MMCE alone; ours gated everything, which was the "pop-in").

**rebuild-154 — the ATA black-screen exit hang.** Not storage: **`bgmStop()` deadlocks.** The BGM
decoder parks in `WaitSema(inSema)`; `bgmStop` tries to wake it with `WakeupThread`, which cannot
release a semaphore and was aimed at the *playback* thread anyway. ATA-specific because `hddCleanUp`
is the only teardown that closes file descriptors (`PDIOC_CLOSEALL`), killing bgm.ogg's fd, after
which `ov_read` returns permanent EOF and the decode loop spins forever. **My BGM stutter fix (ring
16→48) turned a rare race into the steady state.** Master had four guards we never ported; restored.

**rebuild-155 — the CFG storm.** `menuRenderElements` called `_menuRequestConfig()` **unconditionally**,
so every settled row opened `CFG/<game>.cfg` on the game device even when no theme element could
display it. ~3 storage ops/sec while merely browsing, on the worker that outranks the art thread.
Master gates it on `elems->needsItemConfig` — a flag our parser sets and **nothing read**. Also
stopped `updateMenuFromGameList` wiping the absence memo on every list rebuild (which had been
defeating rebuild-153's own fix).

**rebuild-156** — timed the open; narrowed a spurious device-refresh that fired on boot.

**rebuild-157 — the art directory index** (`src/artindex.c`, new). Sweeps an art directory once with
`readdir`, keeps sorted filename hashes in RAM, answers misses without touching the device. Also
reordered the USB VCD launch so the fat32/exFAT question comes **before** the multi-second memory-card
prep rather than after it.

**rebuild-158** — enlarged the reunion key 64→128 bytes. *Aimed at the wrong thing* — hardware later
showed `KL0`, meaning no row ever exceeded 64. Still correct (VCD names are allowed up to 256) but it
explained nothing.

**rebuild-159 / 160 — the index was inert.** `IX0/0/0`: it never built a single directory. 159 fixed a
path-form problem (`opendir("mass0:ART")` vs `"mass0:/ART"`) and added a failure counter; that counter
then proved the sweep was never even *attempted*, so 160 made the art worker claim index ownership
from **inside itself** using `GetThreadId()`, rather than trusting the id `CreateThread` returned.

**⚠ rebuild-160 is pushed and CI-triggered but has NEVER been tested on hardware.** Run
`31676597254` (or re-run). Nathan's last tested build was 159.

---

## 5. THE OPEN PROBLEM — read this carefully

### The symptom
Art still arrives too slowly. Nathan: *"a LOT better, but not better enough"*, *"VCD art is
absolutely fucked still, popping in super late one by one"*, *"as soon as it hits a missing artwork
it basically hangs like hell"*. **Navigation and input are fine** — do not investigate those.

### The measurement (hardware, USB stick, 159)
```
Q19 A1 D0  X0 0ms(ok 0ms 0x0)           O:0/0     IX0/0/0  IO 0/0 T14/10
Q18 A1 D1  X0 4024ms(ok 4024ms 140x200) O:8/0     IX0/0/0  IO 0/0 T14/10
Q1  A1 D17 X2 48ms(ok 48ms 140x200)     O:6/0     IX0/0/0  IO 0/0 T14/10
Q1  A1 D19 X2 75ms(ok 75ms 140x200)     O:3/0     IX0/0/0  IO 2/0 T16/10
Q2  A1 D20 X2 2794ms(ok 2794ms 140x200) O:2730/0  IX0/0/0  IO 3/0 T18/10
```
And from the 158 capture: `O:2578/4395` — a **hit** open costing 2578 ms, a **miss** open 4395 ms.

### What this establishes
1. **Not decode, not transfer.** A 512×725 cover loads in 203 ms while a 140×200 one takes 2922 ms.
   Thirteen times the pixels, fourteen times faster. Image size is nearly irrelevant.
2. **The cost is inside `open()`.** The 2794 ms load spent 2730 ms opening and 64 ms on everything
   else.
3. **It is intermittent.** Most opens are 3–8 ms; a few are 2700–4400 ms. Same file size, same
   directory, same session. **Any explanation must account for the intermittency** — a mechanism
   that would make *every* open slow is contradicted by the 48 ms and 75 ms totals.
4. **Missing art was not even involved** in the 159 capture: the failed-open field stayed 0 all
   session. So the art index cannot be the whole answer.

### What has been REFUTED — do not re-litigate without new evidence
A 16-agent adversarial investigation ran four independent hypotheses, each through refuters.
**Nothing survived.** Specifically killed, with code citations:

- **fileXio lock contention with the ioman worker.** The lock is real (`__lock_sema_id`, a binary
  semaphore, `FXIO_WAIT`) but it brackets **one RPC, not a batch** — so a 2730 ms wait would require
  a single competing RPC lasting 2.7 s, and the per-game CFG read cannot do that.
- **"The timing is invalid because `clock()` includes descheduled time."** `clock()` on the EE *is*
  wall time (`_times` → `TimerBusClock2USec(GetTimerSystemTime())`) — but this does not rescue the
  measurement, because `gArtLastMs` independently shows 48 ms and 75 ms **totals**, and a 48 ms total
  cannot contain a 2.7 s open. The stall is real.
- **First-access / mount / spin-up costs.** Does not explain a spike 20 loads into a session.
- **A HUD sampling artefact.** Refuted: three distinct fast values (8, 6, 3 ms) at three distinct
  completion counts cannot be fabricated from slow opens.

⚠ Also learned: FatFs `dir_find` **breaks at the first end-of-used-entries marker** — it does *not*
read every entry of the directory. That weakens (does not kill) the "a miss walks the whole
directory" reasoning that motivated the art index.

### The single best next instrument
Both the surviving critique and the refuted-but-salvageable notes converged on the same thing:
**the current HUD correlates two unsynchronised last-value reads.** `gTexLastOpenMs` is up to one
load old while `ioGetPending(...)` is sampled live at HUD-format time — so "slow open ↔ worker busy"
is not actually evidenced.

Fix that first, before proposing any cause:

> In `src/textures.c` just before the timed `open()`, capture `ioGetPending(IO_CUSTOM_SIMPLEACTION)`.
> Keep a **worst-open record**: `{ms, pendingAtOpen, device prefix}`, latched only when this open
> beats the stored worst. Render it as its own HUD field.
>
> A multi-second open with `pending > 0` confirms contention. A multi-second open with
> `pending == 0` kills contention outright and moves the cause to the device/IOP layer.

That is one cheap build and it splits the remaining hypothesis space in half.

---

## 6. Standing rules — violating these has caused real regressions

- **The art thread must stay BELOW the GUI thread in priority.** EE: **lower number = higher
  priority.** GUI/pad 31, ioman worker 32, sound 45, art 64, art-on-SIO2 90. Raising art above the
  GUI trades music/art smoothness for **swallowed controller input** — the exact defect of issue
  #340. Forbidden.
- **Never `TerminateThread` a thread that may be inside a file operation.** It leaves the shared IOP
  RPC channel half-used and every later call from any thread is undefined. Abandon instead.
- **No new semaphore between the art path and the render thread.** That shape has already caused one
  freeze here.
- **`src/bdmsupport.c` contains literal NUL bytes** — grep/ripgrep call it binary. Use `grep -a` or
  read it directly. It is one of the biggest and most important files; do not skip it.
- **`util.c`'s `delay(1)` is a ~0.25 s NOP spin, not 1 ms.** `DelayThread(1000)` *is* 1 ms. This unit
  trap once made a "10 second" budget actually forty minutes. Check units on every wait.
- **There is no `-Wall`.** The compiler will not catch a dead field, an unused result, or an implicit
  declaration for you.
- **Language files:** new labels go at the **END** of `_base.yml`. `.lng` files are consumed by line
  position. This has bitten three times.
- **CI must always pass.** Read *every* check line; `check-format` is not required, so a merge can
  succeed while it is red.

### The recurring defect shape — run this query, it has found five bugs
**Data half and comment half land; code half does not.** A field or flag is written, carefully
documented, and **never read** — so a feature silently does nothing while the source looks complete.
Confirmed instances: `cache_entry_t.key`, `gArtShutdownAbandoned`, `theme_elems_t.needsItemConfig`,
`cacheEnd()`'s discarded return value, `cacheInvalidateFailMemo()` as a documented no-op. Each was
found only after a user reported a symptom.

**The query that finds them:** compare **read-site counts against `origin/master`**, per field and
per function. A count that drops N→N-1 while the declaration and its comment survive is the
signature. Searching for zero callers is *not enough* — `needsItemConfig` had a live writer and only
the reader was missing.

---

## 7. Work in flight

- **KIMI** has a two-part brief (hit its 5-hour limit, will resume): (A) the dead-contract sweep
  above, tree-wide; (B) an adversarial break-attempt on `src/artindex.c`, whose invariant is *"may
  only ever answer 'definitely absent' or 'don't know' — a wrong 'absent' hides art that exists"*.
- **ChatGPT Sol** has a brief on the **IOP storage layer**, which nobody has examined: module
  versions vs upstream ps2sdk and vs the fork, what is serialised against what, FatFs tunables, and
  whether a USB bulk-transfer retry with a ~2.5 s timeout could be the spike. That last one fits the
  data suspiciously well and is unresolved.

Both briefs were written to be self-contained; ask Nathan for them if you want them.

---

## 8. Working with Nathan

- He tests on **real hardware** and sends **videos**. Extract the HUD (§3) — the numbers in those
  frames have repeatedly beaten source reasoning, including mine.
- **Do not spam builds.** His tester Zack asked explicitly: *"just dont build another wait for my
  reports as usual."* Stage work on branches; let Nathan decide when to hand out links.
- **Give all three flavour links** for any build.
- **Verify every claim from a bot review against the actual code before acting on it.** Codex and
  Gemini have both produced genuinely correct, verified findings this week *and* confident wrong
  ones. Two of this week's best fixes came from bot reviews I checked line by line first. One of my
  own builds (158) was aimed at the wrong thing because I did not wait for a counter I had already
  built.
- Tell him plainly when something is unproven. He responds well to "here is what the number says and
  here is what it does not say", and badly to confident guesses — there have been several.

---

## 9. Longer-term state

The rebuild is being hardened toward a **v1.0** cutover: `rebuild/main` eventually replaces `master`.
Still outstanding beyond the art work: **MMCE** items, **SMB2**, and docs. `rebuild/main` is at
rebuild-91; steps 92–160 are unmerged, and many of 92–136 were #340 probes rather than keepers, so
that merge needs triage rather than a straight fast-forward. There is also a
`chore/bdmsupport-nul-eol-v2` branch (full-file renormalisation of those NUL bytes) that must be
merged **last**, after everything else, or it will conflict with everything.


---

## 10. LIVE TESTER REPORT — issue #382, and the three flavours disagree

`https://github.com/NathanNeurotic/Open-PS2-Loader/issues/382` — *"ETH Games wont start with bgm
music"*. Tester **Vass327** tried all three flavours of **rebuild-161** and reported:

| flavour | result |
|---|---|
| `OPL-OFFICIALSDK` | **works** |
| `OPL-PS2DEVLATESTSDK` | game starts, then **black screen** |
| `OPL-PS2DEVPINNEDSDK` | **same failure as before** |

Three things follow, and none of them should be assumed away:

1. **The SDK flavour changes behaviour.** Same source, three outcomes. Until now the three builds
   have been treated as interchangeable and links handed out as a set. They are not interchangeable
   for this bug. Any future report must name which flavour it came from — and Sol's audit adds a
   caveat worth honouring: it verified **byte-identical storage IRXs only for the matching
   *pinned* SDK build**, so the LATEST and OFFICIAL artifacts are *not* known to embed the same IOP
   modules. Check the embedded manifest before assuming source parity means binary parity.
2. **The symptom is BGM + a launch, i.e. the teardown path** — the same area as rebuild-154's
   confirmed `bgmStop()` deadlock, which was fixed for the **ATA** arm (`hddCleanUp` closing bgm's
   fd via `PDIOC_CLOSEALL`). 161 contains that fix and the ETH case still fails on two flavours, so
   the ETH arm has its own cause.
3. **Leading suspect, already flagged and never followed up.** The earlier exit-hang investigation
   surfaced `ethCleanUp` → `ethDeinitModules` taking an **unbounded `WaitSema`** on the teardown
   path (`src/ethsupport.c` — candidate sites at lines 239, 250, 343, 369). It was ranked low at the
   time *because it is not ATA-specific* — which is exactly why it is now interesting, because #382
   is not ATA. `origin/master` carries the same unbounded wait, so if master does not exhibit this,
   the difference is in what OPL is doing *concurrently* at that moment, not in the wait itself.
   The shape to look for is the one rebuild-154 established: a teardown step that blocks forever
   because the thing it waits on was killed by an earlier teardown step.

**Suggested first move:** give those waits a bounded acquire (`PollSema` with a tick budget, then
log-and-skip), mirroring the pattern already used for `sysShutdownDev9`'s bounded `DDIOC_OFF` retry
and for rebuild-154's bounded `bgmStop`. A teardown that gives up is always better than one that
hangs, because every caller is on its way to an IOP reset that reclaims everything anyway. Then ask
Vass327 to retest **naming the flavour**.

⚠ Do not ship this as a guess. This session lost two builds to confident guesses (a dev9 theory on
the exit hang, and a key-length fix aimed at a limit that was never exceeded). Both times the answer
came from a counter or a HUD field, not from reading code. Bound the wait *and* add a counter that
says whether the bound was ever hit.


---

## 11. ⛔ REGRESSION IN rebuild-163 — START HERE

**Nathan, testing 163 (PS2DEVLATESTSDK), booted from UDPFS:** set everything up, activated the USB
page, did scroll testing, then **on exit it froze with horizontal flickering bars on screen. Unable
to exit.**

**Treat rebuild-163 as bad. The known-good build is rebuild-162** (`rebuild/step-162-latch-all-contenders`,
run 31692159834). 162 has every art fix plus the full worst-open latch and does NOT have the
teardown reorder.

### What 163 changed, and why it is the suspect

163 moved `bgmStop()` to the **top of `deinit`/`deinitEx`**, before `ioBlockOpsTimed` and before
`deinitAllSupport`, so the music would stop before the device it streams from is destroyed
(issue #382). It is the only thing in 163. The freeze is on the exit path. That is close to
conclusive, but it has NOT been proven — do not assume the mechanism, find it.

### Specific things to check first

1. **`bgmStop()` waits by `SetAlarm(...)` + `SleepThread()`.** That is fine on the thread it was
   written for. Confirm `deinit` runs on that same thread in the **exit** case — exit and launch may
   not reach `deinit` the same way. A `SleepThread()` that nothing wakes is a hang with the screen
   left exactly as it was, and **horizontal flickering bars are a GS/video-state symptom**, i.e. the
   EE stopped somewhere with the display half-configured — consistent with blocking *earlier* in
   teardown than before.
2. **rebuild-154 gave `bgmStop()` an abandon path** that returns WITHOUT calling `bgmDeinit()` when
   the threads do not stop in ~3 s each. `audioEnd()` later re-checks `bgmIoThreadRunning` and calls
   `bgmStop()` **again** — a second ~6 s of waiting, and on the second pass the semaphores may be in
   a different state. Trace both calls in sequence.
3. **BGM streaming from the boot device.** Nathan booted from **UDPFS** (network). If `bgm.ogg` was
   on that share, stopping the decoder early means waiting on a network read while the network is
   still up — a different wait from the ATA case this was modelled on.
4. `bgmStop()` is now called unconditionally, including when audio exists but BGM was never started.
   Verify the early-return guards actually cover that on **every** path.

**The cheap safe fix if you cannot find it quickly:** revert 163's two `bgmStop()` calls and instead
solve #382 the narrow way — an `ethCleanUp`/`ethDeinitModules` guard mirroring rebuild-154's
`gArtAbandoned` skip in `hddCleanUp`. Ordering was the more general fix, which is why I tried it,
but "general" is not worth a hang on exit.

---

## 12. Art still arrives in BATCHES, one step behind

Same 163 test, VCD page: *"speed did improve, but it would only pop in the art after the next art
loaded. Art drops in batches rather than rolling."* Navigation is solid.

This is a **different symptom from slowness** and it is diagnostic. A cover that appears only once
the *next* cover has loaded means the texture is published but the row that wanted it does not
redraw until something else forces a re-evaluation. Suspect the publish/observe seam rather than the
device:

- `cacheLoadImage` publishes with `entry->qr = NULL` **last** (deliberate, it is the handoff signal).
  Confirm the drawing path re-reads the entry every frame rather than caching the returned pointer.
- Rows that are drawn but not requesting use `cacheLookupTexture()` (lookup-only). It returns NULL
  unless `*cacheId`/`*UID` already match. A row whose load just completed may therefore need a
  `cacheGetTexture()` pass before it can see its own art — and that pass may only happen on the next
  selection change. **That is the exact shape of "it appears when the next one loads."**
- The reunion-by-value scan added in rebuild-153 updates `*cacheId`/`*UID` — check whether the
  lookup-only path benefits from it or bypasses it.

Worth one careful read of `cacheGetTexture` / `cacheLookupTexture` / `getGameImage` /
`getGameImageCached` together, tracing one row from request to first frame drawn. No hardware needed
to form the hypothesis; a HUD counter for "frames between publish and first draw" would confirm it.


---

## 13. Since the first handoff was written (all pushed, all CI-green)

**rebuild-164** — backed 163's teardown reorder out completely (`opl.c` is byte-identical to 162
again) and fixed #382 the narrow way instead: `ethDeinitModules` took an **unbounded** `WaitSema` on
the teardown path, now bounded at ~3 s then it tears down anyway. Strictly safer than 163 — it
removes an unbounded wait and adds none. **Untested on hardware.**

**rebuild-165** — issue #380: PS1 rows now show the disc's real id.
- The id is resolved **inside `vcdScanOpenDir`**, where the directory is the one that was actually
  opened. ⚠ Do not move this "up a layer": `vcdScanDir` appends `POPS/` to a device root while
  `vcdScanDirRoot` takes the directory directly (APA/PFS HDD). Anything above them must rebuild a
  path and can only be right about one — my first attempt did exactly that and Nathan caught it.
- Filename parse first (free), then `retrogemGetVcdGameID` reads the id out of the image. Same
  resolver the RetroGEM barcode uses, so a game reports one id to both.
- The disc read is fenced: only on filename-parse failure, memoized per session (hit and miss), and
  budgeted at 8 discs per scan. Memo cleared on a device generation bump.
- Also removed the `#System`/`#Media` badges from **coverflow's main page**. They were the only
  config-consuming elements in that family, so `needsItemConfig` is now clear for it — browsing
  coverflow issues **no per-row CFG read at all**. rebuild-155 built that gate; this is the first
  build where our own theme passes through it.

**CI is now FOUR flavours** (`flavours.yml`) and so is the **release pipeline**
(`rolling-release.yml` — rehearsed via `workflow_dispatch`, all four build jobs green, publish
correctly gated off non-master). Names: `PS2DEVPINNED`, `OFFICIALPINNED`, `PS2DEVROLLING`,
`OFFICIALROLLING`. Every artifact carries `BUILD-MANIFEST.txt`. Issue-template update is **PR #463**
against `master` (open, needs merging — GitHub reads templates from the default branch).

⚠ **User-visible:** release package folders renamed (`APP_RIPTOPL-PS2DEVLATESTSDK` →
`-PS2DEVROLLING`, etc). Say so in the release notes.

### What to do first, in order

1. **Get 164/165 tested.** Nothing since 162 has touched hardware. Specifically: exit from a UDPFS
   boot (163's regression must be gone), an ETH game launch with BGM playing (#382), and the PS1 list
   showing ids (#380 — ask miladera22-sketch, he is responsive and thorough).
2. **Read `W<ms>@<simple>/<menu>/<bgm><h|m>`** from a VCD-page scroll. That field is the open
   question of the whole session (section 5) and one photo answers it.
3. **Merge PR #463** so bug reports can name a flavour that exists.
4. Section 12's art-batching lead — needs no hardware, just a careful read of four functions.


---

# 14. CURRENT STATE, 2026-08-13 (read this section first)

**Tip branch: `rebuild/step-164-eth-bounded-teardown`. It contains rebuild-164 through 168.**
**Latest CI: run `31710438197`, four flavours, all green.**

⚠ **NOTHING SINCE rebuild-162 HAS BEEN TESTED ON HARDWARE.** Five steps are stacked up unverified.
Getting that tested is job one; everything else can wait behind it.

## What landed since section 13

**164** — backed rebuild-163 out completely (163 fixed #382 but froze exit from a UDPFS boot), and
fixed #382 the narrow way instead: `ethDeinitModules` had an **unbounded** `WaitSema` on the teardown
path, now bounded at ~3 s.

**165/166** — first attempt at #380 (PS1 rows showing full titles instead of the disc id). Both were
effectively **inert**: `sbPopulateConfig` re-derives `#Startup` from the filename, so an id resolved
earlier never reached the display. Superseded by 167. Left in history because the reasoning matters.

**167 — three fixes, and the first is the important one:**
- **SMB art no longer vanishes.** `textures.c` separates a real ENOENT (`ERR_BAD_FILE`, absent) from
  a contended bus or a share mid-reconnect (`ERR_FILE_IO`, transient). **Two places threw that away**:
  the generic loose-file arm returned `ERR_BAD_FILE` unconditionally (and that is the arm SMB/ETH/pfs
  take — the staged path only claims `mmce*`/`mass*`), and `cacheLoadImage` tested only `result < 0`
  and parked every failure as absent. The fork parks only on `ERR_BAD_FILE` and retries the rest; our
  texcache had **zero** references to either code. That is why art is instant on the fork over SMB and
  died on ours. Harmless until rebuild-155 made the absence verdict outlive the list rebuild that used
  to wipe it. **New HUD field `TF`** counts transient retries.
- **#382 fix restored without the freeze.** New `bgmQuiesce()` signals the BGM threads and returns
  immediately; the join stays in `audioEnd()`. 163's *order* was right, its blocking *wait* at the top
  of deinit was not.
- **VCD id resolution moved off the scan.** The scan does no device IO at all now (it records only
  which directory a VCD came from); the disc is read lazily on the per-game config path — async, once
  per settled row, gated on the theme having an element that shows it. Identity (art, CFG, launch)
  stays the **filename**.

**168 — the background could only be requested as a side effect of another load finishing.**
`drawGameImage` gated the Background element on `... && !cacheHasPendingArt()`. That predicate is
`queued > 0 || active > 0`, the worker drains without yielding, and `artPop` increments *active*
before the previous release decrements it — so the lane reads busy **continuously** across a whole
batch. No mid-batch idle frame exists, so the background could only be requested in the frame after
the entire lane drained. The fork has **no gate at all** here: one unconditional request for
backgrounds and covers alike. Gate removed; the 30-frame idle margin already did the ordering job.

## THE OPEN QUESTION, and what is already ruled out

**Art still arrives in batches rather than rolling** — reported on USB (Nathan), SMB (L10N37) and VCD
(miladera22-sketch). **Three transports, so it is not the device.**

⛔ **Do not re-litigate these.** Two 16-agent adversarial passes have run on this, and on the
`open()` latency spike. **Nothing survived refutation in either.** Specifically killed, with code
citations, in `tasks/wh1n6pt40.output` and `tasks/wk9sphckp.output` under the session temp dir:
- fileXio lock contention with the ioman worker (the lock brackets ONE RPC, not a batch)
- `clock()` measuring descheduled time (it is wall time, but 48 ms *total* loads prove the stall is real)
- first-access / mount / spin-up costs
- a HUD sampling artefact
- **any cover-vs-cover publish gap** — both passes examined the cover path in both trees and found
  none. The publish (`entry->texture`, then `lastUsed`, then `qr = NULL` last) is picked up by
  `cacheGetTexture`'s fast path on the very next frame, identically in both builds, and every cover
  site calls the requesting path every frame.

**So the remaining suspect is the `open()` spike, not the cache.** A 140x200 cover opens in 3-8 ms
normally and **2730 ms** occasionally, and `O:2730` on a 2794 ms load means 2730 of it was inside
`open()`. That fits L10N37's SMB report and miladera's VCD one better than anything cache-side.

**The one instrument that splits it** is already built and shipping: `W<ms>@<simple>/<menu>/<bgm><h|m>`
— the worst open of the session with the queue depth captured **at** that open, plus whether the BGM
decoder (which bypasses ioman entirely and shares the same file channel) was mid-read. One photo of
that field after a VCD-page scroll is worth more than another investigation.

## What to do, in order

1. **Get 164-168 tested.** Exit from a UDPFS boot (163's regression must be gone), an ETH launch with
   BGM playing (#382 — Vass327 is responsive), the PS1 list showing ids (#380 — miladera22-sketch),
   SMB art surviving a reconnect (#388 — L10N37), and whether backgrounds now roll in.
2. **Read `W` and `TF`** off a VCD-page scroll. `W` decides the open-spike question; `TF` should be
   ~0 on a healthy device and climbing on a flaky share.
3. **Merge PR #463** (issue template — four flavour names). GitHub reads templates from `master`.
4. L10N37 also reported a **"Network mode" setting in Game Sources that appears dead** (toggles
   Auto/Manual with no visible effect). Unexamined.

## Standing judgement calls from this session

- **The fork is a working example — use it.** Three separate bugs this session were "master does X,
  we do not" (the cache reunion, the CFG-read gate, the transient-failure lane), each found only
  after a hardware report. **When a symptom appears, diff the fork FIRST.**
- **But adapt, do not transplant.** A wholesale `texcache.c` swap was started and reverted: master's
  file lacks eight symbols this tree's callers depend on (`cacheLookupTexture`, `cacheDropQueuedArt`,
  `cacheShutdownArtLoads`, `cacheTickArt`, five debug accessors) plus the MX4SIO/SIO2 protection that
  fixed #340. Master keys its whole navigation-contention apparatus on `MMCE_MODE`; widening that
  predicate to include MX4SIO is the small, correct version of that port if it is ever wanted.
- **Every counter must distinguish "not used" from "broken".** `IX0/0` cost two builds; `OE` could
  not see the SMB bug it was built for because it only incremented on an arm SMB never takes.

---

# 15. OPEN: rumble works on the fork, not the rebuild (reported 2026-08-13, UNFIXED)

Nathan: "I don't think rumble is working... it does work on the fork though, just not our rebuild."
`gEnableRumble` defaults to 0 in BOTH trees, so he had turned it on.

**Everything downstream is intact and was checked:** `sfxPlay()` calls `sfxRumble()` ABOVE all audio
gates (src/sound.c:482), the tuned pulses are byte-for-byte the fork's (sound.c:453-476), the decay +
per-frame re-send is present in the pad read loop (pad.c:657-666, and the re-send matters because
freepad drops `padSetActDirect` outside TASK_UPDATE_PAD while still reporting success), realignment
is handled (`padRumbleRealign`, pad.c:488), and the four `padRumbleFlush()` sites match the fork
4-for-4. None of that is the bug.

**LEADING HYPOTHESIS -- not yet proven, do not ship it blind:**

`padActSet` (pad.c) opens with:

    if (pad->actuators == 0)
        return;

and `initializePad` CLEARS that count up front, pad.c:165-170:

    // Cleared up front because every path below can return before padInfoAct() is reached.
    pad->actuators = 0;

`padInfoAct()` is only reached later, at pad.c:240. **The fork never pre-clears** -- `git show
origin/master:src/pad.c` line 343 assigns `padInfoAct()` straight into the field and nothing zeroes
it beforehand.

Our #340 work BOUNDED the pad-init waits (that was the fix for the device-enable freeze -- an
unbounded `waitPadReady` spinning the GUI thread). So if `initializePad` now returns early on a slow
or contended pad, it never reaches line 240, `actuators` stays 0, and **every rumble call silently
no-ops**. The fork survives the same interruption because a previously-detected count is never
destroyed, and its unbounded waits reach the detection anyway.

**How to confirm before fixing:** the LOG at pad.c:241 prints the count (`PAD # of actuators: %d`),
and pad.c:256 logs `PAD Did not find any actuators.` A debug build that shows 0 there, or a HUD
counter for "padActSet returned early because actuators == 0", settles it in one boot.

**If confirmed, the fix is NOT to delete the pre-clear** -- it exists so a stale count does not
survive a real disconnect. Preserve the last known-good count across an interrupted init and only
zero it when the pad is genuinely gone, or re-run `padInfoAct` when a bounded init bailed early.
Whichever way, keep the bounded waits: they fix a freeze, and that is not worth trading for haptics.

---

# 16. Since §14/§15 were written (KIMI, same day): steps 169–170

**169 — #364, shipped green** (run 31721698642, branch `rebuild/step-169-sfx-presses-not-stale`).
zackcage6 confirmed on 31710438197: menu SFX (open/close, confirm/deny) cut unless each action has
a ~5 s gap. Cause: the SFX dispatch ring's stale test aged out CONFIRM/CANCEL at >500 ms, and the
full ring dropped any id, whenever the dispatcher fell behind under load — deliberate presses
silently discarded. The fork has no queue at all (plays synchronously; the menu pays the 352 ms
#340 stall), so reverting to it would have been wrong. Fix: queue kept, stale test now covers
CURSOR only; deliberate presses always play, even late. New HUD fields `SX<stale>/<full>` (silent
drops) and `SP<last>/<max>` (per-press audsrv RPC wall ms — the #340-era diag NOTHING read until
now; sixth instance of the write-only defect shape). If SP shows the RPC itself wedging for
seconds under BGM, the cause is IOP/SIF-side and 169 only makes it visible — said so in the commit.

**170 — #380: the THIRD inert-fix verdict, then the real fix.** 165/166/167 all resolved the PS1
disc id without the value reaching the screen: 167 wrote it to `#Startup` in sbPopulateConfig,
consumed only by `AttributeText attribute=#Startup` — bound by NO shipped theme
(misc/theme_coverflow.cfg, misc/conf_theme_OPL.cfg) and by neither of miladera's attachments
(their only GameID element is on the apps page). The caption is `drawItemText` (themes.c), which
reads `itemGetStartup` → for a VCD view the FILENAME (bdmGetGameStartup). Fix: the VCD arm of
drawItemText shows the resolved id (memo → filename parse → title fallback), resolved OFF the
render thread by a new one-slot request on the ioman worker (`vcdRequestDisplayId` /
`vcdDisplayIdCached`, vcdsupport.c; hooked in menuRenderElements). Nathan's two conditions
honoured: the menusys needsItemConfig gate is UNTOUCHED (own async request, id-only, memo-deduped
per session), and the product call is stated in the commit: the caption shows the id
unconditionally when resolvable, matching the PS2 page; the title stays in the row text.

**Tester state:** #382 retest asked (Vass327, OFFICIALROLLING on 31710438197 — comment posted).
The #380 ask was HELD until 170 existed (167 could not have worked); miladera gets one ask with a
build that can actually work.

**171 — #380 follow-up, same day.** #382 CLOSED (Vass327 confirmed on 31710438197, second
independent confirmation). PR #463 merged; master's release pipeline still emits the THREE old
flavour names and IRX-MANIFEST (not BUILD-MANIFEST) — both populations are labelled in the merged
template. Nathan corrected 170's priority: the disc is the authoritative id source, the filename
a safety net. Explicit check (asked for, answered): nothing in the 170 path lets a parseable
filename suppress the disc read — the queue hook, the request dedupe and the resolver carry no
filename test; the parse exists only as a display fallback. The real defect was shape: the disc
resolver can return `AAAA-NNNNN` while the filename fallback yields `AAAA_NNN.NN`, so the caption
visibly changed shape when the disc read landed. 171 canonicalises to `AAAA_NNN.NN` (the PS2
page's form) at the memo store; the barcode's own resolver is untouched. Amended description
posted to miladera. **One function owns the id shape: `vcdCanonDisplayId` (vcdsupport.c:227).**
Any future change to the caption id form belongs there and nowhere else — the display chain and
the RetroGEM launch/barcode resolver must NOT pick up their own normalisers.

**172 — the fail-closed audit Nathan ordered on 171's request path, shipped same day.** His
question: a row with no memo entry (memo full at VCD_ID_MEMO_MAX=512, or never scanned) has
nothing to dedupe against — does it re-queue on every settle, the CFG-storm shape #155 removed?
Answer, verified by reading: NO for the main case — `vcdRequestDisplayId` returns on
`m == NULL` BEFORE `ioPutRequest`, so nothing is ever queued for such a row. But the audit found
one real hole: `vcdResolveDisplayId` returns on `m->dir == NULL` BEFORE setting `asked`, and the
request side never tested dir — so an entry whose dir strdup OOM'd in `vcdNoteScanDir` would
re-queue an ioman request on every settle while selected (one in flight at a time via the
pending flag, but forever). OOM-gated, which is exactly when a request loop is least wanted.
Fix: one condition at the queue source — `m->dir == NULL` now fails closed like `asked`.
Recoverable: a later rescan re-points dir and re-arms the row. Commit 95aabdc8.

**173 — rolling-release plumbing, staged NOT published** (branch
`rebuild/step-173-rolling-release-notes`). Nathan is bringing testers in and wants the
release pipeline live. The new-lineage statement (parity goal, MMCE awaiting
reimplementation, old build still fine on Rolling Alpha/MEGA, reports tracked against the
new lineage only, re-report fixed things, pinned-flavour guidance + Settings→About) now
lives in `.github/rolling-release-notes-block.md`, cat'd into notes.md by the "Build
release notes" step on every publish. Get-started order swapped to PINNED-first to match
Nathan's ruling (was PS2DEVROLLING-first, which would have contradicted the block in the
same release body). Publish gate untouched: still master/v*-tag only — the first real
rolling publish needs Nathan's go AND the rebuild tip landing on master. What publish
produces (for the go decision): creates tag `rolling` ("Rolling (master)", prerelease) on
first master push — NO rolling tag exists yet, so nothing is overwritten; `rolling-alpha`
and `pops-bundle` are separate tags, untouched. Subsequent master pushes update it in
place (delete-all-assets + re-upload). MEGA: master-only + secrets (set), one immutable
zip per run to /RiptOPL/Rolling/<ver>/run_<n>/, VARIANTS/DEBUG excluded, nothing ever
overwritten on MEGA. Validation limit, stated plainly: the notes step is inside the gated
publish job so it CANNOT be rehearsed from a branch — checked by YAML parse + local
simulation of the exact printf/cat fragment only.

**174 — rolling-release gate widened: rebuild/main is the publish knob** (branch
`rebuild/step-174-rebuild-main-publishes`; workflow + notes block only, no PS2 code).
Supersedes 173's gate description. Context: merging PR #463 to master FIRED
rolling-release.yml and published an old-lineage build to the `rolling` tag (run
31724547932) — Nathan deleted the release. Audit then found the tag itself had been stale
since 2026-06-16 (`1ee48076`): every rolling release since June hung off it, so "source
at this tag" never matched the assets (the assets were always built from the right
commit). Stale tag DELETED with Nathan's go. He chose option (b): NO master cutover —
instead `on: push: branches`, the publish-rolling `if:`, and BOTH MEGA steps now include
`refs/heads/rebuild/main`; release TITLE is plain "Rolling"; notes header/footer
de-mastered. rebuild/main (stale at the step-91 merge, verified ancestor of step-173, so
it fast-forwards cleanly) is now the ONLY publish trigger: Nathan names the tip, it is
fast-forwarded, the push fires the workflow. Notes block also gained: the "this build can
be ahead of what has been hardware-verified" line (rolling-channel honesty while 164+
await hardware; Nathan can veto) and a ONE-TIME stale-tag paragraph marked for removal
after the first correct publish. **To publish:** ff rebuild/main to the named tip → push
fires the workflow → ONE inspection cycle (tag target, rendered notes, four flavours,
version string) BEFORE any tester gets the link. Nothing has published; master untouched.

**THE ROLLING CHANNEL IS LIVE (2026-08-13, run 31740369719).** rebuild/main was
fast-forwarded to the step-174 tip (`64f40204`) and published
`v1.2.0-Beta-2543-64f4020` to tag `rolling` — prerelease (NOT Latest; pops-bundle keeps
that badge), tag pointing at the pushed SHA (first correct target since June), all four
APP_RIPTOPL-* folders + bare ELFs + DS5 loaders + VARIANTS/DEBUG/LANGS + src.zip,
MEGA archive at /RiptOPL/Rolling/v1.2.0-Beta-2543-64f4020/run_460/. The release body
carries the full new-lineage block. One blemish found in inspection and fixed in
**175** (`rebuild/step-175-notes-cleanup`): an "Other downloads" line still called
PS2DEVROLLING "RECOMMENDED" — patched in the live body (one line, rest byte-preserved)
and in the workflow; the one-time stale-tag paragraph was removed from the block (its
publish has gone out). **Publish procedure from here:** Nathan names a tip → ff
rebuild/main → push fires the workflow → inspect (prerelease flag, tag SHA, notes, four
flavours, version, MEGA) BEFORE any tester gets the URL. Master remains on the OLD
lineage by design; do not push to it.

**176 — post-publish notes restructure (Nathan's read of the live page).** Three defects
in the download guidance: `APP_RIPTOPL-OFFICIALPINNED/` missing from Get started (four
folders ship, three were described); the "prefer PINNED" paragraph stranded ABOVE the Get
started heading (two competing download statements); numbered items could gap under the
HAS_ guards. Fixed in the workflow (all four flavours as bullets, best-effort entries
guarded by their HAS_ flags, guidance leads the Get started section) and the block (tail
paragraph removed, comment marks why it must not return). Live release body patched with
the identical restructure, rest byte-preserved. Branch `rebuild/step-176-notes-four-flavours`.
MSYS trap logged: native Windows python cannot open MSYS /tmp absolute paths — cd there
and use relative names.

**177 — DS5 loaders for all four flavours** (branch `rebuild/step-177-ds5-all-flavours`).
Nathan reviewing the live rolling release: "there should be a ds5 build for all 4
flavours." The official jobs were "main ELF only" by a policy predating the four-flavour
line (rolling-alpha had no OFFICIALSDK-ds5 either). Added the soft-fail DS5 step to both
official jobs (OFFICIALROLLING-ds5, OFFICIALPINNED-ds5); rename + notes loops now cover
all four SDKs. The 1080p ELF from rolling-alpha is NOT missing — GSM 1080p is ON in
every build of this line. REHEARSED via workflow_dispatch on the branch (run
31742808752): four builds green, publish correctly SKIPPED, and the official artifacts
verified to contain RIPTOPL-OFFICIALROLLING-ds5.ELF / RIPTOPL-OFFICIALPINNED-ds5.ELF
(downloaded and listed, not assumed from the log). DUALSENSE=1 builds fine in both
official containers. Awaiting Nathan's go to ff rebuild/main and republish in place.

**178 — the version string anchors to code, not HEAD.** Makefile REVISION/GIT_HASH were
`git rev-list --count HEAD` / `rev-parse HEAD`: a docs- or CI-only publish printed a NEW
version for byte-identical loader code, breaking the pinned reproducibility contract.
Now `CODE_ANCHOR` = last commit touching anything the build can consume (tree minus an
explicit exclusion list: .github/, HANDOFF.md, agent-file-drop/, frame_builds/, notes/,
obj/, .claude/, .agents/, .codex*/ — errs toward inclusion, so a missed code path can
never collide two binaries under one string). One-time consequence: the next publish's
REVISION ticks DOWN (~2543 → ~2531) as the twelve bookkeeping commits stop counting; the
hash now names the last CODE commit (95aabdc8). Tagged v* releases untouched. LOCAL
container builds always printed `-dirty`+expr noise: a worktree's .git is a FILE whose
gitdir pointer is host-absolute, dead inside the container — pre-existing, don't chase.

**179 — REVISION stays HEAD-based; the code anchor moves to GIT_HASH only.** Claude's
review of 178 caught it BEFORE publish (run 31744839948 cancelled mid-build, nothing
went out): REVISION is the ordinal testers compare, so code-anchoring it made versions
go backwards (2543 → ~2531) and broke comparability with every report on file, all
HEAD-derived. Adopted his shape: REVISION = HEAD count (monotonic, strictly increasing);
GIT_HASH = CODE_ANCHOR short hash (the reproducibility half — "check out what the tester
ran"). Exclusion list unchanged. No user-visible scheme change, so NO notes disclaimer
needed: 2543 → ~2547 on the next publish. **Lesson logged:** a version ordinal must
never be derived from anything that can move backwards; identity lives in the hash,
ordinality lives in the count.

**180 — the rolling tag now FOLLOWS the publish.** The 2026-08-13 republish exposed it:
`gh release edit/upload` never re-targets the tag, so the tag stayed on the first
publish's commit while assets moved on — the June stale-tag disease recreated by the
pipeline itself. Live tag moved by hand (→ 0127499b); workflow fixed so BOTH publish
paths force-move the tag to GITHUB_SHA (rolling only; v* tags never move). Sits on
`rebuild/step-180-tag-follows-publish` awaiting Nathan's next named tip — the live
release is already correct, so no republish was needed for it. **Current live state:**
rolling = v1.2.0-Beta-2553-95a138c (run 31745407636): REVISION HEAD-based and monotonic
(2543 → 2553), GIT_HASH = code anchor (95a138c = step-179's Makefile commit — Makefile
IS a build input, correctly not excluded), all four ds5 ELFs present and listed,
four-flavour Get started, stale-tag paragraph gone (removed in 175 as designed), tag =
0127499b, MEGA run_463. Claude's review loop caught 178 pre-publish; log it as the model
**182 — art queue priority lane + dual-port ACT HUD.** Focused selection art pushes to the front
of the worker FIFO (`artPushFront`/`artPromote`) so the cursor cover pops in instantly without waiting
for queued surrounding covers. Added dual-port `ACT<p0>/<p1>` (`padGetActuatorDiag`) to the debug HUD
to diagnose rumble alignment and actuator detection without needing an EE serial cable.

**183 — FatFs FR_NO_FILE absence mapping + artindex SMB guard + VCD fallback restore.**
On FatFs-backed BDM (`mass*`), `bdmfs_fatfs` returns `-FR_NO_FILE` (-4) or `-FR_NO_PATH` (-5), which
EE `libcglue` turns into `open() == -1` with `errno == 4` / `5`. Testing strictly `ENOENT` (2) caused
every USB missing cover to be treated as a transient I/O error (`OE`/`TF` climbing in lockstep),
clearing the cache slot instead of parking it, retrying the 4.3s USB directory walk endlessly on
every scroll, and breaking the `if (r == ERR_BAD_FILE)` condition in `bdmsupport.c` so the VCD POPS
cover fallback was completely bypassed. Step 183 maps errno 4/5/6 on `mass*` to genuine absence, guards
`artindex` against backslash SMB paths, and streamlines the HUD formatting.

**184 — BGM theme-to-default fallback restore & vorbis clean-up.**
In `sound.c:bgmLoad()`, custom themes without bundled `sound/bgm.ogg` were trapping `gDefaultBGMPath`
in an `else` branch of `if (themeID != 0)`, preventing user-configured BGM from ever playing on custom
themes (miladera22-sketch #380 symptom 4). Restored master's clean fallback chain (theme BGM first,
then `gDefaultBGMPath`), properly freeing `vorbisFile` on misses to prevent uninitialized memory access
in `bgmDeinit()`.

# 17. CURRENT STATE, 2026-08-13 (late) — SUPERSEDES §14

§14's refuted list and HUD decoder stay valid; its "current state" (tip 164, nothing
published) does not. This section is the state of the project now.

## The rolling channel is LIVE

- Release: https://github.com/NathanNeurotic/Open-PS2-Loader/releases/tag/rolling —
  **v1.2.0-Beta-2553-95a138c** (run 31745407636, built from `0127499b`). A prerelease, NOT
  Latest: `pops-bundle` keeps the Latest badge and `rolling-alpha` (final old-lineage
  build, archived) sits undisturbed — the "old build is still fine to use" promise made
  structural.
- **How it publishes:** rolling-release.yml fires on push to `rebuild/main` (also master
  and v* tags — master is the old lineage, DO NOT push it; see §0b). `rebuild/main` is the
  PHYSICAL KNOB: Nathan names a step-chain tip, it is fast-forwarded (ff-only; verify
  ancestry first), and the push is the publish. Four flavour builds (PS2DEVPINNED,
  PS2DEVROLLING, OFFICIALPINNED, OFFICIALROLLING), each now WITH a DS5 loader
  (`-ds5.ELF` ×4, step-177), then publish (update in place: delete assets, re-upload,
  rewrite notes, and — since step-180 — RE-TARGET the tag), then MEGA archives one
  immutable zip to `/RiptOPL/Rolling/<version>/run_<n>/` (VARIANTS/DEBUG excluded).
- **The notes are carried, not drafted:** `.github/rolling-release-notes-block.md` is
  cat'd into every body after the header (new-lineage statement: parity goal, MMCE
  awaiting reimplementation, old build still fine, report rules, the OrbitOPL Toolbox
  metadata section). The flavour guidance lives ONLY in the workflow's "Get started"
  section (all four flavours, pinned-first, Settings → About names the running one).
  Do not re-add it to the block — one home, one copy.
- **Publish procedure (every time):** ff rebuild/main → push → watch the run to green →
  INSPECT (prerelease flag, tag SHA == pushed SHA, notes rendered, four APP_RIPTOPL-*
  folders in the package zip, four ds5 assets, the ACTUAL version value from the body,
  MEGA upload line) → only then does Nathan announce. Never announce from an uninspected
  publish. Step-180 (`rebuild/step-180-tag-follows-publish`) sits above the checkpoint
  and enters the publish path on the next ff — it fixed the update path never
  re-targeting the tag (found live: the second publish's tag was one tip behind its
  assets, the June stale-tag disease recreated by the pipeline itself).

## The version scheme — and why it is SPLIT

`Makefile`: **REVISION** = `git rev-list --count HEAD + 2` — HEAD-derived, MONOTONIC. It
is the ordinal testers compare ("am I up to date?"); it must never go backwards, and
every report on file quotes a HEAD-derived count. **GIT_HASH** = `CODE_ANCHOR` short hash
— the last commit touching anything the build can consume (tree minus an exclusion list
of docs/CI/handoff/process paths; errs toward inclusion). The hash is the reproducibility
contract: "check out what the tester ran" = checkout of the version's hash. Step-178
anchored BOTH and review caught it before publish (run 31744839948 cancelled mid-build):
an ordinal that can move backwards is worse than a number that ticks on bookkeeping.
**Identity lives in the hash; ordinality lives in the count. Do not merge the roles again.**
Consequence to know: a docs/CI-only republish prints a NEW number with the SAME hash —
that is the scheme working, not a fault.

## checkpoint/2026-08-13-rolling-live

Frozen reference at `0127499b` (step-179 tip) — the commit the live rolling release was
built from. **Never build on it, force-push it, or delete it.** Its only job is to be the
known-good point to return to if a later agent breaks the chain. `rebuild/main` ALSO
points there right now, but the roles differ: rebuild/main MOVES (publish knob); the
checkpoint never does. Do not confuse them.

## Where every issue stands

- **#382 (ETH launch under BGM) — CLOSED, confirmed twice.** 163's original fix (Vass327)
  and 167's `bgmQuiesce()` reimplementation (Vass327 again, run 31710438197,
  OFFICIALROLLING — the continuation of his working flavour).
- **#364 (menu SFX dropped unless ~5 s apart) — fix out, awaiting report.** 169: the stale
  gate now covers CURSOR only; deliberate presses always play, even late. With zackcage6.
  Read `SX`/`SP` in his report: **SP in the thousands = the audsrv RPC wedges (IOP-side);
  SP small with SX/full climbing = dispatcher starved (CPU-side) — DIFFERENT fixes. Do not
  pre-choose.** (169 also said plainly: if the RPC wedges, it makes it visible, not cured.)
- **#380 (PS1 caption showed the title, not the ID) — fix out, awaiting report.** 170+171:
  the caption shows the ID read OFF THE VCD (the same resolver the RetroGEM barcode uses,
  so list and barcode agree), canonical `AAAA_NNN.NN` shape (owned SOLELY by
  `vcdCanonDisplayId`, vcdsupport.c:227). A filename ID may show first and be REPLACED by
  the disc ID a moment after settling — expected, the disc is authoritative. Filename is
  fallback only. 172 made the request path fail closed (dir==NULL OOM case). With
  miladera22-sketch.
- **#388 (SMB art vanished permanently) — fix out, awaiting report.** 167: only a real
  ENOENT parks a game as absent; every other failure retries (the fork's semantics). With
  L10N37 on the 172 build, watching `TF` (~0 healthy; climbing with art recovering = share
  flaking and the retry lane working). His "Network mode" report is NOT A BUG (Manual =
  page only, Auto = device + page; nothing on screen says so) — Nathan ruled it out;
  explained, dropped. Do not re-investigate. His MC0-config point is confirmed fixed.
- **Vass327 needs nothing.**

## OUTSTANDING TEST LIST — the single most actionable thing in this file

**Nathan's console:**
1. **The `W`/`TF` reading from a VCD scroll** — the CENTRAL open question (art batching,
  §14/§3 decode the fields). Two full investigations died for want of this number.
2. Exit from a UDPFS boot (163's regression must be gone).
3. ATA exit re-confirmed — 167 changed teardown AGAIN (`bgmQuiesce`).
4. Backgrounds rolling in **with BG art ON** (168's own-schedule load).
5. Art batching and VCD page speed generally.
6. The MX4SIO device page appearing after enabling it in Settings (156 — never tested).
7. **Rumble — needs the DEBUG zip** (`RIPTOPL-DEBUG-*.zip` from the rolling release):
   `LOG()` compiles out of release builds, and `PAD # of actuators` is the one-boot
   confirmation (§15). Do NOT fix the obvious way — §15 says why.
8. **Boot from the release ZIP** (the `RIPTOPL-*.zip` package), not a bare ELF — that
   packaging path has never been installed by anyone.

**Testers:** zackcage6 (#364, `SX`/`SP`), miladera22-sketch (#380), L10N37 (#388, `TF`).

**Unassigned:** each of the four flavours actually BOOTING (CI green means it compiled —
nothing more), and the four DS5 loaders, none of which has ever run.

## The refuted list (§14) — TWO categories now

**(a) Specific mechanisms disproven with code citations — DO NOT re-derive.** Each is
cited line-by-line in §14/§12 and cost two 16-agent adversarial passes. They are dead
ends, proven dead.

**(b) The FRAMING of the art-batching question itself is fair game.** Both investigations
assumed "a cover-vs-cover publish gap exists in one of the trees" and failed to find one;
the intermittent-`open()`-stall suspicion (3–8 ms normal, 2730 ms occasional) holds only
by elimination. If the question looks wrong to you, SAY SO — Nathan is bringing in fresh
perspective deliberately. But bring evidence (the `W` reading above is built to provide
it), not a re-derivation of (a).
