# RiptOPL — handoff, 2026-08-13

You are picking up work on **RiptOPL**, Nathan's (NathanNeurotic / "Ripto") fork of **Open PS2
Loader** — PlayStation 2 homebrew, C, EE (MIPS R5900, 294 MHz, 32 MB) + IOP co-processor.
Everything below is state you need. Read it all before touching anything; several traps here have
each cost a full day.

---

## 1. Ground truth: repo, branches, where to work

Repo: `https://github.com/NathanNeurotic/Open-PS2-Loader`
Local checkout: `C:\Users\natha\Github\Open-PS2-Loader` — sits on branch **`rebuild/main`**.
**Do not commit to it, do not commit to `master`, do not commit to any `rebuild/step-*` branch.**

Two lineages, both ours, and the distinction matters constantly:

| name | branch | what it is |
|---|---|---|
| **"the fork"** | `origin/master` | The mature, shipped fork. Battle-tested. **Faster at art than the rebuild.** The reference to compare against. |
| **"the rebuild"** | `rebuild/step-160-index-owner` (tip) | A from-scratch re-port, being hardened. **This is what you are working on.** |

`git show origin/master:src/FILE` reads the fork's copy of any file without checking it out. **Do
this constantly** — most bugs this week were found by diffing the two.

Start work in your own worktree:

```bash
cd C:/Users/natha/Github/Open-PS2-Loader
git fetch origin
git worktree add .claude/worktrees/<yourname> -b <yourname>/<topic> origin/rebuild/step-160-index-owner
cd .claude/worktrees/<yourname>
```

Numbered "rebuild-NNN" steps are sequential branches `rebuild/step-NNN-<slug>`. The next one is
**161**. Each is one focused change with a long explanatory commit message — **keep that convention,
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

Then hand Nathan three nightly.link URLs:

```
https://nightly.link/NathanNeurotic/Open-PS2-Loader/actions/runs/<runId>/OPL-PS2DEVLATESTSDK.zip
https://nightly.link/NathanNeurotic/Open-PS2-Loader/actions/runs/<runId>/OPL-PS2DEVPINNEDSDK.zip
https://nightly.link/NathanNeurotic/Open-PS2-Loader/actions/runs/<runId>/OPL-OFFICIALSDK.zip
```

**He always wants all three.** He tests on real hardware — a physical PS2 with a USB stick, an
MX4SIO SD adapter, an internal ATA drive, and memory cards. There is no emulator path for the
things being worked on.

---

## 3. The debug HUD — you cannot work on this without it

An on-screen line, enabled in OPL's settings, is the primary instrument. Nathan sends **videos**;
frames are extracted with ffmpeg and the line is read off them. Current format (`src/gui.c`, search
`artdbg`):

```
Q<n> A<n> D<n> X<n> <totalMs>ms(ok <okMs> <W>x<H>) O:<openMs>/<missOpenMs> OE<n> IX<dirs>/<absent>/<failed> KL<n>  F<frameMs>/<worstMs> OV<n>  NR<n> MT<n>  IO <pendSimple>/<pendMenu> T<totSimple>/<totMenu>
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
