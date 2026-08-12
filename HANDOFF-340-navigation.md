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

## OPEN ITEM #1 — MX4SIO load-order regression (caused by rebuild-135)

Reported immediately after 135: **MX4SIO games list, but show no art, and launching hangs forever on
the loading icon → black screen with a stuck icon.**

Diagnosis (high confidence, not yet proven): device identity comes from an **ioctl on the mounted
`massN:`** (`USBMASS_IOCTL_GET_DRIVERNAME` + device number, in `bdmUpdateDeviceData`,
`src/bdmsupport.c`). `massN:` is only the filesystem; `usbN:`/`mx4sio0:` style names are block-device
identities used for launch binding and are never readable as files. So identity is knowable only
AFTER the transport is loaded and mounted. rebuild-135 moved the MX4SIO load later (now only when
its page is enabled, via `bdmLoadBlockDeviceModules`, `src/bdmsupport.c:380`), so anything that asks
the slot who it is before that mount completes gets an unidentified generic mass device →
wrong/empty art prefix and a launch bound to the wrong device type.

**Do NOT fix by reverting 135** — that reintroduces the navigation bug. The fix is ordering: keep
MX4SIO out of the *boot resolver*, but ensure that when `gEnableMX4SIO` is set the driver is loaded
AND mounted before any identity query or launch. Complication: the resolver runs INSIDE `_loadConfig`
*before* the first config read, so `gEnableMX4SIO` is not known at that point.

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
