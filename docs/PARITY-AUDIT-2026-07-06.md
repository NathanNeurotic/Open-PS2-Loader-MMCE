# RiptOPL Parity & Performance Audit — vs wOPL and NHDDL (2026-07-06)

**Scope:** Feature, performance, and (as the priority lens) Neutrino global + per-game *settings* parity of RiptOPL against the two most relevant reference launchers — **wOPL** (the sibling OPL fork) and **NHDDL** (the lean Neutrino-native launcher) — plus a landscape sweep of upstream OPL and the ps2-mmce fork.

**Method:** 12 agents across 3 phases (6 source-reading inventories → 5 cross-referenced gap analyses → 1 completeness critic that fact-checked the biggest claims against RiptOPL's actual code). ~1.5M tokens, 368 tool calls. Every "gap" was verified against the local source before being asserted; the two design-critical facts (Neutrino's exact `-gsm` grammar and the DS5 code path) were re-verified by hand for this write-up.

**Sources read (with the commit each agent pinned):**
- **RiptOPL** — local worktree (this repo), `master` @ `0c2a8519`.
- **Neutrino** — `rickgaiser/neutrino` @ `master` (`ee/loader/src/main.c`, `config.c`, `config/*.toml`).
- **wOPL** — `ps2homebrew/wOPL`, branch **`wOPL-base`** @ `ce94bd9a` ("PR #308 nav2 — QoL", 2026-06-25).
- **NHDDL** — `pcm720/nhddl` @ `821b6c9b` (2025-11-22).
- **Landscape** — `ps2homebrew/Open-PS2-Loader`, `ps2-mmce/Open-PS2-Loader`.

---

## 0. Executive summary

**RiptOPL is at or ahead of parity on nearly every axis measured.** It is a strict superset of wOPL's feature set and, for the Neutrino use case, matches or exceeds NHDDL — including NHDDL's own reliability structures (single boot-time IOP reset, keep-IOP handoff, cross-device `neutrino.elf` auto-discovery, pre-teardown network preflight), which it adopted through the Δ1–Δ9 parity work. The genuinely-missing items are **narrow and mostly low-severity**; the two that matter most are a *feature* gap (`-gsm` field-flip/scale picker) and a *memory* lever (the ~1 MB static cheat table).

The four highest-value, genuinely-actionable gaps:

1. **`-gsm` field-flip + 1080i×2/×3 structured picker** — the only high-severity *feature* gap. Today the per-game "Neutrino Video" enum offers Off/240p/480p/1080i (3 of Neutrino's 5 video modes) and **none** of the `:1/:2/:3` field-flip compatibility axis, which is the single most-used interlace fix on real hardware. Users needing it must hand-type raw syntax into the free-text box.
2. **`gCheats` static BSS ≈ 1.05 MB** — the largest single always-resident allocation, held even when cheats are off (the default). Lazy-allocating it reclaims ~1 MB of the PS2's 32 MB.
3. **Total-argv byte-length guard** (~255-char `ExecPS2` ceiling) — RiptOPL guards `argc < 32` but not the summed byte length; long paths + many flags can silently overflow into a black-screen boot.
4. **Δ9 (fragment-budget precount)** — already scoped in the parity plan, still unstarted; a black-screen-prevention item that outranks most audit gaps by user pain.

Everything else is polish, niche, or a deliberate non-goal (see §6).

---

## 1. Scoreboard

| Axis | vs wOPL | vs NHDDL | Genuine gap? |
|---|---|---|---|
| Neutrino transport (`-bsd`/`-bsdfs`/`-dvd`) | ahead | parity | user-selectable `-bsdfs=hdl/bd` absent (low) |
| Neutrino compat (`-gc`) | ahead | parity | none |
| Neutrino video (`-gsm`) | ahead | **behind** | **field-flip + 1080i×2/×3 (HIGH)** |
| Neutrino VMC (`-mc0/-mc1`) | **ahead** | **ahead** | none (structured named-card + size + existence check) |
| Neutrino `-cwd`/`-cfg`/`-elf`/`-qb`/`-ata*` | parity | parity | none (structured sub-screen) |
| Neutrino `-dbc`/`-logo` | ahead | behind | per-game override absent (low) |
| Unknown-flag pass-through + `$`-disable | parity | parity | none |
| Neutrino reliability (IOP lifetime, preflight) | ahead | parity | none |
| Cross-device last-played | ahead | **behind** | RTC-timestamp arbitration (low) |
| Coverflow / reflections / anim | parity | ahead | none |
| Controllers (DS3/DS4, GH/RB, multitap, BT) | ahead | ahead | DS5 default-off build flag (medium) |
| Theme engine | ahead | ahead | `aligned=2`, `plasma_blend_color` (low) |
| Devices/formats (exFAT/APA/UDPBD/UDPFS/MMCE/ZSO) | ahead | ahead | none |
| PS1 / POPSTARTER | ahead | **N/A (NHDDL has none)** | none |
| Cheats / PS2RD | ahead | N/A | none |
| Memory footprint | tuning choice | behind | **`gCheats` ~1 MB (HIGH)** |
| Boot / launch-path latency | parity | parity | none (Δ7/Δ8 already lean) |
| SDK build strategy | **ahead** (3-flavour digest-pinned) | ahead | none |

---

## 2. Neutrino settings — the priority axis (detailed)

### 2.1 What RiptOPL already exposes

RiptOPL's Neutrino settings surface is **the most complete of the three launchers.** It ships a structured **"Neutrino Launch Args" sub-screen** (global: `guiShowNeutrinoArgsConfig` → `diaNeutrinoArgs`; per-game: `guigame.c`) with dedicated fields for `-qb`, `-cwd`, `-cfg`, `-elf`, `-ata0`, `-ata0id`, `-ata1`, **plus a free-text "extra" catch-all** that preserves unknown/future flags and the `--b`/ELF-arg tail (`supportbase.c` `neutrinoArgsParse`/`neutrinoArgsAssemble`).

| Neutrino option | RiptOPL exposure | Source |
|---|---|---|
| `-bsd` / `-bsdfs` / `-dvd` | auto-mapped from OPL device type | `system.c` `getDeviceName`, `sysLaunchNeutrino` |
| `-gc` (compat) | 7-checkbox per-game picker → `-gc` digits | `system.c` `convertCompatmaskToModes` |
| `-gsm` (video) | 4-value enum → `fp1`/`fp2`/`1080ix1` | `guigame.c`, `gsmTokens[]` |
| `-mc0` / `-mc1` (VMC) | **structured named-card + 8/16/32/64 MB + existence-checked** | `supportbase.c` `sbBuildVmcNeutrinoArgs` (Δ2) |
| `-cwd` / `-cfg` / `-elf` / `-qb` | structured fields in the args sub-screen | `dialogs.c` `diaNeutrinoArgs` |
| `-ata0` / `-ata0id` / `-ata1` | structured fields | `dialogs.c` |
| `-dbc` / `-logo` | **global** toggles (`gEnableDebug` / `gPS2Logo`) | `gui.c`, emitted in `system.c` |
| unknown/future flags | routed into `na.extra`, re-emitted verbatim | `supportbase.c`, `appendArgTokens` |
| `$`-disable convention | adopted (leading `$` keeps arg in field, not forwarded) | `system.c` `appendArgTokens` |
| network `ip=` toml | **auto-synced + validated pre-teardown** | `system.c` `sysSyncNeutrinoUdpfsToml` (Δ6) |

RiptOPL is **strictly ahead of NHDDL** on VMC (NHDDL manages `-mc` only as raw path strings with no size/create/existence-check), on the network preflight, and on a per-game DMA/UDMA picker NHDDL lacks entirely. It stores VMC config as a *name only* and rebuilds the absolute `massN:` path at launch — cleaner than NHDDL's device-number byte-patching and inherently portable across device renumbering.

### 2.2 The `-gsm` gap (HIGH — the one real feature gap)

**Verified authoritative grammar** (neutrino `ee/loader/src/main.c`, `parse_gsm_flags`, this write-up re-fetched it):

```
-gsm=v:c        where v = video mode, c = compatibility (field-flip) mode
  v ∈ { fp1     (force 240p/288p, auto PAL/NTSC)
        fp2     (force 480p/576p, auto PAL/NTSC)   ← "recommended mode"
        1080ix1 (1080i width×1 height×1 — "very small!")
        1080ix2 (1080i width×2 height×2)
        1080ix3 (1080i width×3 height×3) }
  c ∈ { 1  (field flipping type 1 — GSM/OPL)
        2  (field flipping type 2)
        3  (field flipping type 3) }
Examples:  -gsm=fp2     -gsm=fp2:1     -gsm=1080ix2
```

**Correction to the raw audit:** there is **no plain `fp` mode** — only `fp1`/`fp2`. Any recommendation must use the 5 real video tokens.

**RiptOPL today** (`gsmTokens[] = {"", "-gsm=fp1", "-gsm=fp2", "-gsm=1080ix1"}`, per-game enum `{Off, 240p, 480p, 1080i}`): exposes **3 of 5** video modes and **0 of 3** field-flip modes. Missing: `1080ix2`, `1080ix3`, and the entire `:1/:2/:3` compatibility axis.

**Why it matters:** field-flip (`:1/:2/:3`) is the single most-used GSM interlace fix on real hardware for games that shake/tear under forced-progressive; `1080ix2/x3` are the only *usable* 1080i scales (×1 is "very small"). Requiring users to memorize raw Neutrino syntax defeats the structured UI — this is the one place NHDDL's picker is genuinely richer.

**Where to implement:** widen the per-game "Neutrino Video" enum to all 5 video modes, add a second `COMPAT_NEUTRINO_GSMCOMP` enum (None/1/2/3) in `diaCompatConfig`, and compose `v:c` in the gsm token builder (`system.c`, where `gsmTokens[]` lives). Save alongside the existing video enum in `guigame.c`. **Note the two distinct GSM surfaces:** the memory-index "structured -gsm picker" is the *OPL ee-core* GSM picker (`guigame.c`, ~29 video modes) — a **different, richer** picker that does **not** feed Neutrino's `-gsm`. This gap is real; the two surfaces don't share code.

### 2.3 Lower-severity Neutrino settings gaps

- **`-dbc` / `-logo` per-game override (low).** Both are global-only today; NHDDL exposes both as per-title toggles. `-logo` is a per-game region/boot-compat aid, so a global toggle is coarse. Add two `UI_BOOL` rows to `diaNeutrinoArgs`, OR-in with the global auto-emit in `sysLaunchNeutrino` (de-dup so it's emitted once).
- **User-selectable `-bsdfs=hdl/bd` + `-dvd` prefix (low, M).** Only APA auto-picks `hdl`; there's no way to run an HDL-formatted image on USB/MX4SIO or a raw `-bsdfs=bd` device from the UI. Coupled to the `-dvd` path shape (`hdl:`/`bdfs:` prefixes), so not a pure free-text add — needs a per-game enum feeding the device-shape branch, guarded off for mmce/udpfs (those have no filesystem layer: neutrino `main.c` internally forces `sBSDFS="no"` and skips the bsdfs module for them regardless of what's passed, so the picker must not emit a `-bsdfs` there — the valid *driver* values are only `exfat`/`hdl`/`bd`).
- **`-dvd=esr` / DVD-image emulation (low, M).** No competitor exposes ESR either; niche given RiptOPL's loose-ISO focus. Document as intentionally-unsupported unless a user asks.

---

## 3. What wOPL does that RiptOPL doesn't

RiptOPL is a **strict superset** of wOPL — coverflow+reflection+anim, DS3/DS4 pademu **plus** Guitar Hero/Rock Band controllers (which wOPL's own inventory says it lacks), PadMacro/turbo, multitap, BT pairing + BT-Info screen, full BGM subsystem, MMCE advanced-pacing UI, cache sliders, NBD/lwnbd server, `UI_HEADER`/splitter layout, and Deckard XParams are all already present and often more capable. Only three honest gaps survive, all minor:

1. **DS5/DualSense default-off (medium, S).** **Verified:** RiptOPL *has* full DS5 code — `modules/ds34usb/iop/ds34usb.c` (5 `#ifdef DS5_ENABLE` blocks; `DS5=2` in `ds34usb.h`), threaded via `DUALSENSE=1` → `-DDS5_ENABLE` (`modules/ds34usb/iop/Makefile`). But `Makefile:41 DUALSENSE ?= 0` (marked experimental, "needs real DS5 hardware to test"), so a stock release gives DualSense users no controller, whereas wOPL probes DS5 unconditionally. *(The completeness critic's "grep found 0 DS5 matches" was a directory mixup — it searched `modules/pademu/`, not `modules/ds34usb/iop/`.)* **Recommendation:** ship a second `DUALSENSE=1` release artifact rather than flipping the default — but validate on real DS5 hardware first, since it's still experimental.
2. **Theme `aligned=2` (right-justify) not parsed (low, S).** `ALIGN_RIGHT` exists at the renderer level (`renderman.h`, `1<<2`) but the theme parser maps `_aligned` only to 0=NONE / non-zero=CENTER (`themes.c`). A wOPL/uOPL theme using `aligned=2` silently centers. One-line parser extension: value 2 → `ALIGN_RIGHT`.
3. **`plasma_blend_color` theme key absent (low, M).** wOPL exposes a secondary plasma-blend color; RiptOPL has bg/text/ui-text/sel-text pickers but not this. Purely cosmetic; skip unless plasma-theme fidelity is a goal.

**Corrected false gap:** the raw audit listed "Neutrino unsupported-format (ZSO) hard-aborts instead of falling back to native." **This already ships** — on local devices `bdmTryNeutrinoLaunch` returns 0 on a ZSO/missing-elf reject, so `bdmLaunchGame` falls through to the native OPL core (the hard-abort is retained only for UDP, where native is genuinely unbootable). Corroborated by `docs/NEUTRINO-PARITY-2026-07-05.md`. **Drop this item.**

---

## 4. What NHDDL does that RiptOPL doesn't

For the Neutrino use case RiptOPL is at or ahead of NHDDL almost everywhere. Where NHDDL is *narrowly* better and adoptable without regressing OPL's breadth:

1. **RTC-timestamped cross-device last-played (low, S–M).** NHDDL writes a packed RTC timestamp + title on every mounted device and restores the cursor to the truly-newest launch across drives. RiptOPL stores a single `last_played` startup-string in one `CONFIG_LAST` file, matched within the currently-loaded list only — with multiple drives the "resume" cursor isn't guaranteed newest. Add an optional `sceCdReadClock` timestamp beside `last_played` (8 write sites); pick newest when multiple device lists exist; keep the single-file model for the common one-device case.
2. **Total-argv byte-length guard (low→medium, S).** NHDDL explicitly models Neutrino's ~255-char `ExecPS2` budget and warns. RiptOPL guards only `argc < 32`, not the summed byte length — long `-cwd`/`-dvd` paths + a full args string + `-mc` VMC paths can silently overflow into a truncated/failed boot with no toast. Best placed in `sysNeutrinoPreflight` (pre-deinit, GUI alive): sum `strlen(argv[0..argc-1])`, `guiWarning`+abort if over threshold. Matches the Δ6 preflight philosophy.
3. **Prefix-matched per-title config (low, L).** NHDDL matches `<name>.yaml` by filename prefix, so one file covers a multi-disc set / regional variants. RiptOPL keys strictly by 11-char gameID (more precise per-disc, but no auto-propagation). Only pursue as an **opt-in fallback** (if `<gameID>.cfg` absent, try a title-prefix cfg) — do **not** replace gameID keying.

**Where NHDDL is *not* ahead (documented so it isn't re-litigated):** structured gc/gsm/logo/dbc pickers (RiptOPL has them), unknown-flag pass-through + `$`-disable (adopted), device-portable VMC (RiptOPL cleaner + existence-checked), and NHDDL's minimalism (no PS1/POPS, no cheats, no PADEMU) which is a **non-goal** to copy.

---

## 5. Performance & architecture

RiptOPL's perf/arch is **already highly mature.** Verified at parity or ahead: single boot-time `SifIopReset` with **0 resets per launch**; the keep-IOP Neutrino handoff (`elfldr_noreset.c`) matches NHDDL's single-IOP-lifetime model; the Δ8 lean-Neutrino path skips fragment/VMC/cheat/layer-1 prep Neutrino re-derives; Δ7 bounded launch-path IO drain; a dedicated art-cache worker with interactive/prefetch priority queues, LRU eviction, and input-gated deferral; DEV9 refcounting; and a **3-flavour digest-pinned SDK build** (ahead of wOPL's single pin and NHDDL/Neutrino's un-matrixed CI). Genuine items:

1. **`gCheats` BSS ≈ 1.05 MB (HIGH, M).** `gCheats[MAX_CODES=250]` × `code_t codes[MAX_CHEATLIST=510]` ≈ 4.2 KB/entry → ~1.05 MB of always-resident BSS, held even when cheats are off (the default). Largest single static allocation. **Fix:** either drop `MAX_CODES`/`MAX_CHEATLIST` to realistic values (few games approach 250×510), or lazily `malloc`/`free` the buffer only when `GetCheatsEnabled()` and a `.cht` is actually loaded (`cheatman.c` `load_cheats`/`set_cheats_list`, free on deinit). Reclaims ~1 MB of 32 MB — headroom against OOM on large lists / big themes / coverflow caches.
2. **Persistent on-disk cover/title cache (low, L).** RiptOPL's art cache is entirely in-RAM (torn down at deinit). NHDDL persists a title-ID cache (`nhddl/cache.bin`, magic "NIDC"). A decoded-cover staging cache would mainly help re-entry and slow devices (MMCE/SMB). Only worth doing device-selectively; **not** a title-ID cache (OPL has no ISO-probe step to accelerate).

**At parity — no change warranted:** launch-path length / IOP lifetime (Δ7/Δ8 already deliver it), boot device scan (costly transports default-OFF; naive parallelization is unsafe on the single-IOP RPC model — see the device-page-reliability note), digest-pinned SDK (already shipping), native-core `-qb` (a Neutrino-only concept; deliberately **not** emitted after a menu session per the parity doc).

---

## 6. General UX / landscape

RiptOPL is at or ahead of parity across wOPL, NHDDL, upstream OPL, and the ps2-mmce fork on general settings/UX/format/device capability. **CHD is confirmed a non-feature everywhere** (false-positive substrings only). Residual items:

1. **Boot-time video recovery → forced 480p, not Auto (low, S).** Upstream OPL PR #1332 maps a boot-time pad-hold to a **fixed 480p progressive** recovery mode (universally-safe). RiptOPL's Triangle+Cross combo sets `gVMode = 0 = Auto`, which resolves to region-default interlaced 480i/576i — some modern displays/upscalers/OSSC still fail to sync it, leaving the user black-screened with no way into settings. One-line change: set the held-combo branch to the 480p enum index instead of 0/Auto (the enum already has a 480p entry).
2. Theme `aligned=2` and `plasma_blend_color` — see §3.

---

## 7. Prioritized "fill the gaps" work queue

Ranked by user-visible payoff ÷ effort, black-screen / data-loss risks first.

> **Status (2026-07-14): this queue is DRAINED.** Items 1, 2, 3, 5, 8, 9, 10, 11 shipped across the
> Δ-series and the core-aware-settings/DS5/bsdfs work; item 4 (gCheats lazy alloc) shipped in the
> cht.tar PR (`ensureCheatTable`, f7546100) plus a free-on-disable hook; item 6 (480p recovery)
> shipped as ef91674d; item 7 (RTC last-played) is **retracted** — `CONFIG_LAST` is a single
> global store, so last-write-wins already IS newest-wins (do not resurrect this row again). **Corrected** against the fork's own `docs/NEUTRINO-PARITY-2026-07-05.md` and the two hand-verifications above.

| # | Item | Why | Sev | Effort | Where |
|---|------|-----|-----|--------|-------|
| 1 | **Δ9: bd-backend 64-frag precount (ISO + VMC .bins)** | Prevents a post-reset **black screen** on fragmented VMCs — already-scoped, unstarted | HIGH | M | count-only helper reusing the `bdmsupport.c` fraglist code, called from `bdmTryNeutrinoLaunch` |
| 2 | **`-gsm` field-flip (`:1/:2/:3`) + `1080ix2/x3` structured picker** | Only high-sev *feature* gap; field-flip is the top HW interlace fix | HIGH | M | widen per-game video enum to 5 modes + add `GSMCOMP` enum; compose `v:c` in `system.c` `gsmTokens`. Grammar verified (§2.2) |
| 3 | **Total-argv byte-length guard (~255 `ExecPS2` ceiling)** | Silent truncation → malformed arg → black-screen boot; distinct from `argc<32` | MED | S | `sysNeutrinoPreflight` — sum `strlen(argv[])` pre-deinit, `guiWarning`+abort |
| 4 | **`gCheats` BSS: lazy alloc instead of ~1.05 MB static** | Reclaims ~1 MB whenever cheats are off (default) | HIGH | M | `cheatman.c` `load_cheats`/`set_cheats_list`; alloc on enable+`.cht`, free on deinit |
| 5 | **Δ10: `-elf=cdrom0:\<startup>;1` (shape-guarded, flag-gated)** | Unlocks pre-reset per-GameID `compat.toml` that today fails hard with only a printf | MED | M | `sysLaunchNeutrino`; emit only when startup matches `AAAA_NNN.NN`; behind a flag |
| 6 | **Boot-video recovery → forced 480p progressive** | A safe escape hatch for TVs that can't sync interlaced Auto | MED | S | `opl.c` held-combo branch: set 480p enum index, not 0/Auto |
| 7 | **RTC-timestamped cross-device last-played** | With multiple drives the "resume" cursor isn't guaranteed newest | LOW | S–M | optional `sceCdReadClock` beside `last_played` (8 write sites); pick newest in `opl.c` |
| 8 | **DualSense: ship a `DUALSENSE=1` release artifact** | DS5 users unsupported in the default build (code exists, gated) | MED | S | 2nd CI artifact in `rolling-release.yml`; **validate on real DS5 HW first** |
| 9 | **Per-game `-dbc` / `-logo` override** | Both are global-only; NHDDL makes them per-title | LOW | S | 2 `UI_BOOL`s in `diaNeutrinoArgs`; OR-in with global auto-emit, de-dup |
| 10 | **Theme parser: `aligned=2` → `ALIGN_RIGHT`** | wOPL/uOPL themes using right-justify silently center | LOW | S | `themes.c` `_aligned` parse (`ALIGN_RIGHT` already exists in `renderman.h`) |
| 11 | **User-selectable `-bsdfs` (exfat/hdl/bd) + `-dvd` prefix** | HDL/bd loose-image emulation reachable only via APA auto-pick | LOW | M | per-game enum → device-shape branch in `sysLaunchNeutrino`; guard mmce/udpfs |
| 12 | **`-dvd=` free-text blocklist in `appendArgTokens`** | So a hand-typed `-dvd=` can't double-emit once #11 lands (no blocklist exists today) | LOW | S | `system.c` `appendArgTokens` + `neutrinoArgsParse` |
| 13 | **Persistent cover cache (slow devices only)** | Speeds cold re-entry to large lists on MMCE/SMB/UDPFS | LOW | M | `texcache.c` staging cache keyed `<id>_<suffix>`, device-selective |
| 14 | **Per-slot VMC enable/disable (`$`-disabled `-mc` token)** | Nicety mirroring NHDDL's disabled-arg convention | LOW | S | `sbBuildVmcNeutrinoArgs` — emit `$-mcN=` when a slot is toggled off |
| 15 | **`plasma_blend_color` theme key** | Theme authors can't tint the secondary plasma blend | LOW | M | new `CONFIG_OPL_PLAS_BLEND_COLOR` + picker + renderer thread |

### Queue status (2026-07-06, end of day)

Shipped same-day: **#2** `-gsm` picker (PR #94), **#3** argv byte guard — upgraded to the full
15-string/256-byte kernel budget after source verification (PR #97), **#4** `gCheats` lazy-alloc
(PR #101), **#5** Δ10 `-elf=` opt-in (`neutrino_elf_arg`, this PR), **#6** 480p recovery (PR #95),
**#8** DualSense — shipped as the named `-ds5.ELF` rolling asset per maintainer decision, default
unchanged (PR #100), **#9** per-game `-dbc`/`-logo` (PR #96), **#10** audit reconciliation (PR #91),
**#12** `-dvd=` free-text double-emit is prevented by the emit-order + last-wins semantics plus the
#97 budget; **#1** Δ9 shipped as PR #88. **#7 RTC last-played: RETRACTED, not a gap** — all seven
launch legs write the same key in the same single `CONFIG_LAST` file, so last-write-wins already
equals newest-wins; NHDDL needs timestamps only because its per-device files can disagree, a
situation RiptOPL's single-file model cannot enter. Remaining open: **#11** user-selectable
`-bsdfs`, **#13** persistent cover cache, **#14** per-slot VMC disable, **#15** grammar
verification (done — folded into #2).

### Queue close-out (2026-07-06, night — tail batch)

**#11** `-bsdfs` picker, **#14** per-slot VMC disable, and the queue-table **#15**
`plasma_blend_color` theme key (the status paragraph above mis-numbered #15 as the already-folded
grammar item — the table row was still open) all shipped in the tail-batch PR. Corrections learned
while shipping: the `bd` `-dvd` prefix is `bdfs:` and exfat takes the bare path unchanged (verified
against rickgaiser/neutrino `main.c` usage — its own example runs `bsdfs=bd` on udpbd, so the net
block devices stay eligible; only mmce/udpfs are excluded); #14's audit phrasing "emit `$-mcN=`"
would NOT parse — `-mc` args are discrete argv entries that never pass the `$`-stripping tokenizer,
so the disable is realized as skipped emission (which also clears the slot's `vmcSlotMask` bit and
re-arms the MMCE gameID switch for that slot).

**#13 persistent cover cache: PARKED by maintainer decision** — the one tail item that writes to
user storage; requires a design pass first (opt-in default OFF, cache on the boot/config device
only — never the game device, hard size cap, mtime-keyed invalidation) per the exFAT-corruption
class of risk. The queue is otherwise EMPTY.

### Non-goals (do NOT queue — deliberate fork decisions)
- **Native-core `-qb` quickboot** — `-qb` is Neutrino-only; deliberately not emitted after a menu session (mixed IOP state is what Neutrino's reset exists to erase). Parity doc §4.2.
- **Dropping `-mc` VMC files** (parity doc §4.3), **libconfig backend migration** (prior decision), **trimming OPL breadth** (PS1/POPS/cheats/PADEMU) to chase NHDDL minimalism, and a **headless pure-forwarder entry** (adds a path, low value for a full launcher — OPL's breadth is the point).

---

## 8. Corrections applied to the raw agent output

For traceability when acting on this report:
- **ZSO "hard-abort" is a FALSE GAP** — native fallback already ships on local devices (§3). Dropped from the queue.
- **No plain `-gsm=fp`** — only `fp1`/`fp2`; the queue's #2 uses the 5 verified video tokens (§2.2).
- **DS5 code exists** in `modules/ds34usb/iop/ds34usb.c` (the critic searched the wrong directory); recommendation is a build artifact, not "add the code" (§3).
- **`argc<32` guard confirmed present**; the byte-length guard (#3) is a *distinct* limit, not a duplicate.
- **Δ1–Δ9 reconciled**: the audit re-derived several already-shipped items (Δ2/Δ3 VMC hardening, Δ6 preflight, Δ8 lean path) without crediting the plan; those are marked "ahead/parity," not gaps. Δ9 and Δ10 are the two genuinely-unstarted tracked items and lead the queue.
- **Not audited this pass (follow-up candidates):** the POPSTARTER/PS1-VCD launch axis for parity, and the config/save-path robustness saga (exFAT-HDD corruption, cwd-config) tracked in `docs/RECOVERY-2026-07-03.md`.
