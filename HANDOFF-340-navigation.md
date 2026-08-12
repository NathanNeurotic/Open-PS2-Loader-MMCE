# Handoff — #340 navigation/art investigation (2026-08-12)

## Working directory

```
C:\Users\natha\Github\Open-PS2-Loader\.claude\worktrees\opl-issue-340-diagnosis-2cdb77
```

Branch at handoff: `rebuild/step-136-cursorrumble`. Base of the whole stack: `origin/rebuild/main`
at `e0e8f80e` (rebuild-91 merge). Every step below is its own branch `rebuild/step-NNN-*`, each
pushed, each CI-built. **Nothing is merged to `rebuild/main` yet.**

## THE HEADLINE: the navigation bug is SOLVED

**Cause:** `bdmResolveBootDir()` loaded BOTH cheap transports for an untyped `massN:` boot — USB
**and MX4SIO**. `mx4sio_bd.irx` is an SD-card driver on **SIO2, the pad's own bus**, and once loaded
it is resident for the session (`mx4sioModLoaded` latches, no unload path). So every ordinary USB
boot ran with an SD driver on the controller's bus, starving freepad's polls and swallowing presses.

**Why the good builds are clean:** rebuild-66 never reaches that resolver (its `setDefaults()` clears
`gBootDir` before `configInit(NULL)`, so the resolver hits its empty-path guard); uOPL has no
resolver and loads MX4SIO only when the user enables it.

**Fix:** rebuild-135 — MX4SIO deferred to the escalation round (with iLink/ATA), which only runs if
USB fails to find the boot folder. **Nathan confirmed on hardware: navigation fixed.** Zack's
confirmation was still pending at handoff.

Credit where due: found by ChatGPT Codex auditing the snapshots, not by me.

## ~~OPEN ITEM #1~~ — MX4SIO load-order regression: FIXED, hardware-pending (2026-08-12)

Reported immediately after 135: **MX4SIO games list, but show no art, and launching hangs forever on
the loading icon → black screen with a stuck icon.**

Branches, each CI-green on all three flavours, each on top of the last:
`rebuild/step-137-mx4sio-order` (commits 137, 137b, 137c) → `rebuild/step-138-resolver-order` →
`chore/bdmsupport-nul-eol`.

**The art diagnosis written here originally was WRONG and is retracted.** Art never depended on
identity: `bdmGetImage` reads only `bdmPrefix`, which `bdmUpdateDeviceData` builds from the slot
number *before* either ioctl. The real chain is that a cover which fails once is **parked
permanently** (`texcache.c:274` sets `lastUsed = 0`; `cacheGetTexture` turns that into
`cacheId = -2`, "not asked again until a list rebuild") and a BDM list **rebuilds essentially never**.
So every cover requested while the SD card was mounted-but-not-yet-servable is blank for the whole
session. The root cause is still ordering — 135 removed MX4SIO's only synchronous, pre-config load,
so its mount now races the device pages — but the sink is the art cache, not the identity.

The launch hang has **two** independent sinks, both after `deinit()` has torn the UI down:
- the native dispatch is a four-way if-chain on the driver token with **no final `else`**, so an empty
  token never reaches `sysLaunchLoaderElf` and simply returns into a dead OPL;
- a failed `GET_DEVICE_NUMBER` leaves `massDeviceIndex = -1`, which enters `settings->bdDeviceId` — a
  **u32** (`cdvd_config.h:82`) — as `0xFFFFFFFF`. `bdm_matches_launch_device`
  (`device-bdm.c:57-59`) can never match it, so `bdm_io_sema` is never signalled and the game blocks
  forever. ATA is exempt: it binds by driver token alone. Nothing retries either ioctl — the identity
  re-entry gate tests root/driver/type but **never the index**.

**Do NOT fix by reverting 135** — that reintroduces the navigation bug. The fix is ordering, and the
three traps it hides are all documented in the code now: it must run *before* `bdmInitDevicesData()`
(that call is what queues the identity requests, and the wait yields to the prio-32 IO worker); it
must gate on `== START_MODE_AUTO`, not `!= DISABLED` (`initSupport` only reaches `bdmInit` on the AUTO
arm with `force_reinit == 0`, so under MANUAL there is no bdm core to mount against); and it must be
latched to the boot pass, because `applyConfig(-1,-1,0)` is called from **11 settings dialogs on the
GUI thread** and `bdmEnsureSourceModules` takes a lock the IO worker holds across a no-timeout load.

138 then fixes two more 135 consequences in the resolver: the escalation is **tiered** (MX4SIO alone
first, iLink + ATA only after) so an MX4SIO boot no longer drags in dev9/atad and no longer trips
`_STR_HDD_NOT_CONNECTED_ERROR`, whose `setErrorMessageWithCode` **replaces `menuUpdateHook`** — the
hook that schedules BDM rescans and yields to pending art; and the weak fallback is consumed **last**,
after every transport has had its chance, so a second USB stick holding a same-named `/OPL` folder can
no longer steal the boot device and suppress the MX4SIO load entirely.

## OPEN ITEM #2 — art on USB is "good enough" but not perfect

Nathan: "Art is officially good enough." Measured: ~61–125 ms per cover, queue drains. One outlier
seen twice: a **4.4 s FAILED** read (the success timer stayed at ~122 ms) — a device stall, paired
once with a 1050 ms frame. Rare. A designed-and-reviewed "art directory index" (make a MISSING cover
cost zero device operations) exists as a plan; the review concluded **measure first** because a miss
does less work than a hit, so the 4.4 s was a bus stall, not directory-scan cost.

## OPEN ITEM #3 — smaller, real, unfixed

- **VCD/ISO L3 toggle**: stale titles from the previous view persist; the swap is also slow.
- **MC `error 24` (EMFILE)** on the SECOND settings save and on the VCD BDMA marker write.
  rebuild-100 made mc writes single-open like upstream (untested since). A 28-agent audit proved the
  errno is genuine EMFILE, found NO handle leak anywhere, and confirmed `saveConfig` resets
  `gLastSaveErrno` properly. Real remaining hole: `_menuSaveConfig` (per-game save) reports a stale
  errno — it never resets it.
- **PS2-Launcher v4.0 feature gaps** (memory: `ps2launcher-feature-gaps`): Xbox pads, per-game
  language patching, 21:9, a browsable recents list.

## RULED OUT — do not re-investigate (each cost hours)

Art pipeline (throttles/caps/prefetch/cancellation/cache sizing — all stripped to uOPL's shape);
device rescans and list rebuilds; IO worker priority (moved 30→32, below the GUI, no effect on the
skip); sound effects (tester disabled: no change) and BGM (identical to 66); the SDK's `freepad.irx`
(same source, three SDKs, identical behaviour — Zack tested all three flavours); EE-side key repeat,
edge detection and clock arithmetic (**byte-identical** to 66); `dia.c`/`dialogs.c` (hash-identical
to 66); `renderman.c` (only a deleted video mode); the module set in `sysReset()` (zero hunks since
66); the Makefile (unchanged since 66).

Three independent audits (two of mine, ~150 agents total; one Codex) found NOTHING in the EE source.
That was correct — the cause was what we LOADED, not what we wrote.

## Instrumentation shipped (debug HUD, Settings → Advanced → Debug)

HUD line: `Q# A# D# <ms>  F<last>/<worst> OV#  NR# MT#`
- `Q/A/D` art queued / active / completed, `<ms>` last load (`ok` = last successful, when shown)
- `F` last frame ms `/` worst ever, `OV` frames over ~1.5 vsyncs — **proved the EE never stalls**
- `NR` longest run of polls where the pad could not be READ (→ SIO2/IOP side)
- `MT` longest run of polls that read fine but were EMPTY (→ press never reached freepad)
A run ≥ 4 in either column = one swallowed press (a press spans 4–6 frames).

## Build + publish recipe

Container `oplbuild92` (ps2dev). If stopped: `docker start oplbuild92`.

```
docker exec oplbuild92 sh -c "thirdparty/clang-format-lint-action/clang-format/clang-format12 -i <files> && echo FORMAT_OK"
docker exec oplbuild92 sh -c "rm -f obj/<file>.o opl.elf && make -j8 opl.elf > /tmp/b.log 2>&1; echo MAKE_EXIT=\$?"
git checkout -b rebuild/step-NNN-name && git add <files> && git commit && git push -u origin <branch>
gh workflow run flavours.yml --repo NathanNeurotic/Open-PS2-Loader --ref <branch>
gh run watch <id> --repo NathanNeurotic/Open-PS2-Loader
```

**Always hand the tester ALL THREE flavour links** (he asked for this explicitly):
`https://nightly.link/NathanNeurotic/Open-PS2-Loader/actions/runs/<RUN_ID>/OPL-PS2DEVLATESTSDK.zip`
and the same URL with `OPL-PS2DEVPINNEDSDK.zip` and `OPL-OFFICIALSDK.zip`.

Gotchas: `gh` needs `--repo NathanNeurotic/Open-PS2-Loader`. Never edit source with Python
`open(...).write()` — it corrupted `src/bdmsupport.c` with NUL bytes once; use the Edit tool.

**That NUL corruption cost this investigation real time and is now repaired on
`chore/bdmsupport-nul-eol`.** Two raw NUL bytes sat inside the `'\0'` literals on the identity
re-entry gate. GCC accepted them (value 0 — behaviour was always correct), but **grep and ripgrep
classify a file containing a NUL as BINARY and print `Binary file … matches` instead of the matching
lines**. Every plain grep over `src/` therefore silently omitted the contents of the most-searched
file here, across all three audits — and it made one audit agent misread the literal as `' '` and
report a guard bug that does not exist. If that branch is not merged yet, use `grep -a` / `rg --text`
/ the Read tool on `src/bdmsupport.c` and trust nothing else. The same NUL is why the file was stored
CRLF while every sibling is LF (git's binary heuristic only inspects the first 8000 bytes, so it
diffed as text but escaped `* text=auto`), which is why repairing it produces a full-file diff and
has to be its own commit at the tip of the stack.

## Reference trees for comparison

`C:\Users\natha\Github\opl-audit-snapshot\` — `current/`, `rebuild66/`, `uopl/` (no git, safe for
外部 agents), plus `PROMPT.md` and `PROMPT2.md` (the two Codex briefs). Refresh `current/` with
`git archive HEAD | tar -x -C <snap>/current`. uOPL clone also at
`<scratchpad>/uopl`. rebuild-66 = `5390c0fe`; official base = `3e3f34e9`.

## How this investigation actually worked (worth repeating)

Every real fix came from **measurement or mechanical comparison against a known-good build**, never
from reasoning about likely causes. My confident theories were wrong repeatedly — empty-slot probing,
a VRAM story that removed a visual feature, an invented difference in the tester's art sets, an
unbounded retry that made navigation worse. The tester caught most of them. When stuck: get a number
on screen, or diff against uOPL/66, and subtract rather than add.

**Addendum (137/138): audit the fix, not just the bug.** The diff against uOPL found the mechanism
quickly; the expensive mistakes were all in the *fix*. A shipped, CI-green rebuild-137 turned out to
order nothing (the wait sat after the call that queues the identity requests, and it yields), to gate
on the wrong start mode, and to put a blocking lock acquire on the GUI thread from eleven settings
dialogs. None of that was visible from the bug — only from re-reading the patch as an adversary. Of
14 adversarial checks run over the findings, 8 refuted claims from the same side that raised them,
including two "dead end after `deinit()`" reports that turned out to have pre-`deinit` probes. Budget
review effort for your own patch at least equal to what you spent finding the bug.
