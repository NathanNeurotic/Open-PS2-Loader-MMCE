# RetroAchievements integration plan (RiptOPL)

**Status:** approved, phases 0-2 implemented. This document is the brief for the implementing agent.
**Branch:** `claude/retroachievements-port-bb1959` (PR #600), rebased onto `origin/rebuild/main`.
**Shipping shape:** its own build flavour in the variants archive — `make RETROACHIEVEMENTS=1`. The
default RiptOPL ELF must be **byte-for-byte unaffected**.

> **2026-09-04 status note.** Phase 4 (the `modules/network/ps2ips-ra/` copy) is **obsolete and was
> dropped**: `rebuild/main` commit `561d91d3` promoted the same fixed ps2ips to the default build,
> byte-identical, at `modules/network/ps2ips/`. Every mention of `ps2ips-ra` below describes the
> plan as written, not the tree as built; the RA flavour now uses the same ps2ips as every other
> flavour. Open question #4 ("land RA-only first, then propose promotion") is answered by events.

---

## 0. Read this first

### 0.1 Upstream source

The upstream work is **yoba / `hacan359`**, in
[hacan359/Open-PS2-Loader#1](https://github.com/hacan359/Open-PS2-Loader/pull/1) (branch `ra`).
Use the upstream **[xeRAbora PC client](https://github.com/hacan359/xerabora)** and its published
protocol as the compatibility baseline.

The upstream code is AFL-3.0, same licence as OPL. Two files are vendored under their own terms and
must keep their original formatting via `.clang-format-ignore`:

| File | Licence | Origin |
|---|---|---|
| `src/md5.c`, `include/md5.h` | zlib | L. P. Deutsch |
| `modules/network/ps2ips-ra/ps2ips.c` | AFL (ps2sdk) | ps2sdk, locally patched |

### 0.2 Port, don't merge

The upstream diff is **+5994 / 72 files against upstream ps2homebrew OPL**. Every one of its hot
files (`src/opl.c`, `src/gui.c`, `src/system.c`, `src/menusys.c`, `src/supportbase.c`,
`src/ethsupport.c`, `ee_core/*`, `modules/network/*`) is heavily diverged in RiptOPL. `git
cherry-pick`, `git am` and three-way merge will all produce garbage.

**Take the design and the wire protocol. Re-author the integration by hand.** The good news, from
the survey: `ee_core/`, `src/system.c` and the `modules/network/` tree are still close enough to
upstream that those parts map nearly 1:1. The menu-side (`gui.c`, `menusys.c`, `opl.c`,
`supportbase.c`) does not.

### 0.3 Standing repo rules that bite this feature

These are not suggestions. Each has drawn blood before.

1. **`rebuild/main` is the publishing branch. `master` is dead lineage.** Base everything on
   `origin/rebuild/main`. If you find yourself reading a file that does not match this document,
   check you are not on `master`.
2. **Deviation rule.** Copy upstream *behaviour*, restructure freely. Do not change behaviour
   without evidence. Gratuitous "improvements" to working code are how the 2026-08-27 breakages
   happened.
3. **Language strings are APPEND-ONLY.** New `_STR_RA_*` labels go at the **end** of
   `lng_tmpl/_base.yml`; `.lng` files are consumed by line position. `lang_autogen.h` and
   `lang_internal.c` are gitignored build artifacts — never commit them.
4. **Art lives in the ART folder.** One art location per type; only the key varies. Do not invent a
   second lookup path for RA badges.
5. **Folders vs settings.** Settings go to CWD; **library folders go to the ROOT of each activated
   device** — never `mc`, never CWD. `RA/` follows the `CHT/` precedent exactly.
6. **Use the Write tool for any file containing a literal backslash.** Quoted heredocs collapse
   escapes at the tool layer. A collapsed `'\0'` has already shipped in `bdmsupport.c`.
   Related: `src/bdmsupport.c` contains 2 NUL bytes, so `grep` treats it as binary — use `grep -a`.
7. **CI must always pass.** `check-format` is *not* a required check, so a red format run still
   merges. Build **and** run clang-format 12 locally before pushing.
8. **`SifExitRpc()` must stay** before `ExecPS2()` in elfldr. It has been removed twice on a false
   premise. Do not touch it.
9. **The About dialog cannot scroll** — roughly one row of headroom. Append RA credit to an
   existing line rather than adding a new block (upstream's patch adds 6 rows; that will overflow).
10. **`docs-sync.yml` owns the README's External Tools section** and the site's `#external-tools`
    block. Both are regenerated from templates **inline in
    `.github/workflows/docs-sync.yml`**. Hand edits to `README.md` are deleted on the next push.
    The PC client entry goes in the workflow template.

---

## 1. How the feature works

Two independent halves that share only a file format.

### 1.1 Menu half — "is this game supported?"

Runs on the EE, in the OPL menu, over the **full lwIP stack** (`netman` + `smap` + `ps2ip` +
`ps2ips`).

```
console -> "RAQ1 <hash32> <serial> <own-ip> <own-port>"        (UDP broadcast, port 18194)
PC      -> "RAA1 OK <bytes> <chunks> [<achievements> <title>]" | "RAA1 WAIT" | "RAA1 NO"
console -> "RAG1 <hash32> <chunk-index> <own-ip> <own-port>"
PC      -> "RAC1 <index> <length> " + binary payload
```

The console binds a **fixed** local port (18196) and puts its own IP/port in the request, because a
PC client inside Docker sees a NAT-rewritten source address. Replies are padded to a multiple of 64
bytes and at least 128 bytes so the `ps2ips` small-packet path is never taken (see §5.2). Chunks are
896 bytes.

Result is written to **`<device-root>/RA/<serial>.wl`** — a watch list. The presence of that file is
the entire record that a game is tracked; there is no registry to drift.

### 1.2 In-game half — telemetry

Runs on the IOP, deliberately **bypassing** the menu network stack.

- `ee_core` copies the watch list out of loader memory during init (same trick `SetupCheats()` uses,
  because the game overwrites loader memory).
- Every frame, from the **existing `VBLANK_END` interrupt handler in `padhook.c`**, `RA_OnVblank()`
  packs the watched addresses into a 64-byte-aligned buffer and fires one **interrupt-safe SIF DMA**
  into an IOP-heap buffer allocated at module-load time. If the previous DMA is still in flight the
  frame is **skipped, never waited on** — waiting inside an interrupt handler stalls the SIF channel
  the game shares with audio, disc and pad.
- `raudp.irx` on the IOP hand-builds Ethernet/UDP frames and calls `SMAPSendPacket` directly. It
  gets the destination MAC from the ARP table via a new `etharp_lookup_mac()` in SMSTCPIP (this lwIP
  fork predates `etharp_find_addr`).
- The PC client runs rcheevos and unlocks achievements. It can send `RAU1` back; `ra_overlay.c`
  plays a gold `PMODE`/`BGCOLOR` pulse over the running game (12 frames down, 48 back).

### 1.3 What this means for scope

Telemetry lives **inside OPL's own launch**. That is a hard boundary:

| Launch path | ee_core? | RA works? |
|---|---|---|
| BDM (USB / iLink / MX4SIO / ATA-exFAT) — `sysLaunchLoaderElf` | yes | **yes** |
| ETH / SMB — `sysLaunchLoaderElf` | yes | **yes** |
| HDD (APA) — `sysLaunchLoaderElf` | yes | **yes** |
| MMCE — `sysLaunchLoaderElf` | yes | **yes** |
| **Neutrino core** (`$CoreLoader`) — `sysLaunchNeutrino` | **no** | **no** |
| **UDPFS** — Neutrino-only in this fork | **no** | **no** |
| **PS1 / VCD** — POPSTARTER or Ember | **no** | **no** |

`sysLaunchNeutrino` hands off to an external `neutrino.elf`; ee_core is never loaded. Same for
POPSTARTER and Ember. **Four legs are in scope, three are structurally out.** The UI must say so
rather than silently doing nothing — see Phase 3, item 6.

---

## 2. The build flavour

### 2.1 Makefile flag

Add alongside `EXTRA_FEATURES` / `PADEMU` / `DUALSENSE` (`Makefile:28-63`):

```make
# RetroAchievements: 1 builds the RA flavour (menu check + in-game telemetry).
# A flag is not a dependency: run "make clean" when switching it.
RETROACHIEVEMENTS ?= 0
```

When `1`:

- `EE_CFLAGS += -DRETROACHIEVEMENTS`
- `EECORE_EXTRA_FLAGS += RETROACHIEVEMENTS=1` (and the same `-DRETROACHIEVEMENTS` inside
  `ee_core/Makefile`)
- append `rawatch.o rahash.o md5.o ranet.o rabadge.o` to `FRONTEND_OBJS` (`Makefile:117`)
- append `raudp.o` to `IOP_OBJS` (`Makefile:121`)
- append `ra_mark` to `PNG_ASSETS` (`Makefile:137`)
- swap `$(EE_ASM_DIR)ps2ips.c`'s source from `$(PS2SDK)/iop/irx/ps2ips.irx` to the local
  `modules/network/ps2ips-ra/ps2ips.irx` (`Makefile:759`)
- add `INGAME_UDP=1` to `SMSTCPIP_INGAME_CFLAGS` (`Makefile:289`)
- add `USE_DEV9=1` to the `bdm_cdvdman.irx` recipe

When `0`, **none** of the above. Every RA `.c` file must also be wrapped so that an accidental
compile is a no-op, and every call site in shared files must be inside `#ifdef RETROACHIEVEMENTS`.

> **Acceptance gate for the flag:** `make clean && make release` and
> `make clean && make RETROACHIEVEMENTS=1 release` must produce ELFs whose *non-RA* code paths are
> identical. Practically: diff the two `.map` files and confirm every added symbol is RA-prefixed or
> an RA module blob. If a non-RA symbol moved semantically, you have leaked the feature into the
> default build.

### 2.2 Variants slot

Variants are built by **`.github/scripts/build_rolling_extras.sh`**, called once per SDK flavour
from `rolling-release.yml` (steps "Build variant + debug extras"). Output goes to
`rolling/variants/` and is packed into `RIPTOPL-VARIANTS-*.zip`.

**Do not fold `RETROACHIEVEMENTS` into the `PADEMU x EXTRA_FEATURES x DUALSENSE` loop** — that turns
8 builds into 16 per SDK flavour, i.e. 32 total, and roughly doubles the rolling job. Add a separate
short loop after it, matching the shape of the existing DS5 staging step:

```sh
echo "== Building RIPTOPL RetroAchievements variant (suffix='${SDK_SUFFIX}') =="
for ra_pad in PADEMU=0 PADEMU=1; do
  make clean >/dev/null 2>&1 || true
  if make --trace RETROACHIEVEMENTS=1 $ra_pad EXTRA_FEATURES=1 NOT_PACKED=1 $BRAND_ARG && [ -f opl.elf ]; then
    mv opl.elf "rolling/variants/RIPTOPL-ra-pademu${ra_pad#PADEMU=}${SDK_SUFFIX}.ELF"
  else
    echo "WARN: RA variant '$ra_pad'${SDK_SUFFIX} failed to build; skipping it"
  fi
done
```

Two RA builds per SDK flavour (four total), best-effort like every other extra — **an RA build
failure must never sink the publish**. If the author later wants a DS5 RA build too, it is one more
loop, not a matrix dimension.

Then update, in the same effort (standing rule: keep docs current):

- `rolling-release.yml` — the release-notes generator's line describing what
  `RIPTOPL-VARIANTS-*.zip` contains (~line 919), and the DS5/variants prose (~line 859).
- `.github/workflows/release-normalize.yml` — it asserts `RIPTOPL-VARIANTS-*.zip` exists and repacks
  it; confirm the new filenames survive the repack (they should; it is a blind unzip/rezip).
- `README.md` release-package section and `ROLLING_RELEASE.md`.

---

## 3. Phased work plan

Each phase ends with a build of **both** flavours (pinned + `ps2dev:latest` containers) and a
clang-format 12 run. Commit per phase; keep the phases as separate commits so a hardware regression
can be bisected to one.

### Phase 0 — Scaffolding and the flag (no behaviour)

1. Branch is already created: `feature/retroachievements` off `origin/rebuild/main`.
2. Add `RETROACHIEVEMENTS ?= 0` and all the conditional Makefile wiring from §2.1.
3. Add the variants loop from §2.2.
4. Add `.clang-format-ignore` entries for `./src/md5.c`, `./include/md5.h`,
   `./modules/network/ps2ips-ra/ps2ips.c`.
5. Vendor `src/md5.c` + `include/md5.h` unchanged from the upstream PR.

**Acceptance:** both flavours build; `RETROACHIEVEMENTS=1` build is identical to the default because
nothing is wired yet. Map-file diff shows only the md5 symbols.

### Phase 1 — In-game telemetry (the hard, high-value half)

Order matters: this half is self-contained and testable with the author's PC client and a
hand-written `.wl` file, before any menu UI exists.

| File | Action |
|---|---|
| `modules/network/common/ra_snap.h`, `ra_watch.h`, `smap_tx.h` | port as-is (shared wire/format headers) |
| `modules/network/raudp/` (Makefile, imports.lst, exports.tab, irx_imports.h, raudp.c) | port as-is, ~1088 lines |
| `modules/network/smap-ingame/xfer.c` | add `SMAPTxInit()` + the TX semaphore around `SMAPSendPacket` |
| `modules/network/smap-ingame/main.c` | call `SMAPTxInit()`, `RegisterLibraryEntries(&_exp_smap_driver)` |
| `modules/network/smap-ingame/exports.tab` (new), `Makefile`, `imports.lst`, `irx_imports.h` | export `SMAPSendPacket` |
| `modules/network/SMSTCPIP/etharp.c` | add `etharp_lookup_mac()` |
| `modules/network/SMSTCPIP/exports.tab`, `Makefile` | export it |
| `modules/network/SMSTCPIP/include/lwipopts.h` | the `INGAME_UDP` heap/pool bumps |
| `modules/network/common/smstcpip.h` | correct the `lwip_recvfrom` prototype to 8 args |
| `ee_core/include/ra.h`, `ra_overlay.h`; `ee_core/src/ra.c`, `ra_overlay.c` | port as-is |
| `ee_core/include/coreconfig.h` | add `raWatchList` / `raWatchCount` / `raSnapBytes` **at the end of the struct**, guarded |
| `ee_core/include/modules.h` | add `OPL_MODULE_ID_RAUDP` **before `OPL_MODULE_ID_COUNT`, after the existing IDs** |
| `ee_core/src/iopmgr.c` | allocate the IOP snapshot buffer, load `raudp` last (it imports `SMAPSendPacket`), pass buffer addresses as hex load args |
| `ee_core/src/main.c` | call `RA_SetupWatchList()` next to `EnableCheats()` |
| `ee_core/src/padhook.c` | `RA_OnVblank()` as the **first** statement of `IGR_Intc_Handler()` (`padhook.c:282`) |
| `ee_core/Makefile` | `ifeq ($(RETROACHIEVEMENTS),1)` → `EE_OBJS += ra.o ra_overlay.o`, `EE_CFLAGS += -DRETROACHIEVEMENTS` |
| `src/system.c` | force `CORE_IRX_ETH`, register `raudp` in the module table, plumb the watch list into `EECoreConfig_t` |

**Deviations from upstream you must make:**

- **`OPL_MODULE_ID_*` ordering.** Upstream inserts new IDs in the middle of the enum. Ours is a
  wire-ish contract between `src/system.c` and `ee_core/src/iopmgr.c` compiled together, so
  reordering is safe *in principle* — but append anyway. Cheap, and it keeps any stale `.o` from
  loading the wrong blob. **Delete `obj/` when the enum or `EECoreConfig_t` changes** (a mid-struct
  field change has broken positional initialisers here before).
- **`modules |= CORE_IRX_ETH` must not be unconditional.** Upstream forces the in-game network stack
  into *every* launch. That is ~57 KB of the module-storage region (`OPL_MOD_STORAGE 0x00097000`,
  `include/iosupport.h:91`) and puts SMAP on the NIC in every game. Gate it:
  `#ifdef RETROACHIEVEMENTS` **and** a runtime "RA telemetry" setting **and** a non-empty watch
  list. No watch list means nothing to send, so there is no reason to pay for the stack.
- **Module storage budget.** Verify the worst case (`BDM_USB_MODE` + PADEMU + VMC + MMCEDRV + ETH +
  raudp) still fits. `sendIrxKernelRAM` logs `SYSTEM IRX STORAGE %p - %p` (`src/system.c:924`) —
  read it on a real launch, do not assume.
- `EECoreConfig_t.gCheatList` is `const u32 *` in this fork; match that qualification on
  `raWatchList` or the compiler will complain at the `src/system.c` assignment.

**Acceptance:** with a hand-placed `<device>/RA/<serial>.wl`, a USB launch streams ~60 snapshots/s
to the author's PC client for several minutes with no frame-rate impact and no DMA-fail growth.
Verify `ra_snap_skip` / `ra_snap_fail` counters stay flat. **This needs hardware; PCSX2 will not
prove it.**

### Phase 2 — Hashing and the watch-list file

| File | Action |
|---|---|
| `include/rahash.h`, `src/rahash.c` | port (~576 lines): ISO9660 walk with **64-bit offsets** |
| `include/rawatch.h`, `src/rawatch.c` | port (~204 lines): `.wl` load/store, launch log |
| `include/supportbase.h`, `src/supportbase.c` | add `sbLoadWatchList`, `sbHashGame`, `sbHashGameDeferred`, `raHashLog*` |
| `src/supportbase.c:1104` `sbCreateFolders` | add `"RA"` to `basicFolders[]` |
| `src/bdmsupport.c:2256`, `src/ethsupport.c:996`, `src/hddsupport.c:2137`, `src/mmcesupport.c:1053` | `sbLoadWatchList(<prefix>, game->startup);` immediately **before** the existing `sbLoadCheats` call |

**Why the direct ISO9660 walk:** a *mounted* image on USB hangs when reading past the 2 GB mark.
`raHashIsoDirect` walks the filesystem itself with 64-bit offsets and never mounts. Keep that; do
not "simplify" it to a normal open/read.

**Why hashing is on-demand only:** hashing every image during the scan would hold the console on the
splash screen — the scan runs before the menu appears. Hash one game, when the user asks.

**Deviations:**

- HDD (APA) games are HDLoader format: `hdl_game_info_t` has no filename and no extension, so there
  is nothing to hash. Watch lists still *load* from `RA/`; the *check* action must be disabled or
  clearly refused for HDD entries rather than silently producing a wrong hash.
- `src/supportbase.c` in this fork has a multi-location cheat search and a search log. Mirror that
  shape for `sbLoadWatchList` rather than importing upstream's single-path version, so RA and CHT
  behave the same way for the user.

**Acceptance:** hashing a known-good ISO on USB produces the same 32-hex-digit hash the PC client
computes for the same file. Confirm against the author's client, not against a local reimplementation.

### Phase 3 — Menu integration

| File | Action |
|---|---|
| `include/ranet.h`, `src/ranet.c` | port (~530 lines) |
| `include/rabadge.h`, `src/rabadge.c` | port (~148 lines) **with the slot-count fix below** |
| `modules/network/ps2ips-ra/` | port the whole module (Makefile, imports.lst, irx_imports.h, ps2ips.c) |
| `include/gui.h`, `src/gui.c` | `guiShowRANotice()` / `guiShowRANotices()` |
| `src/menusys.c` | two game-menu entries: RA check, RA test-PC-link |
| `src/opl.c` | `raBadgeRefresh()` in `updateMenuFromGameList()`, badged name selection |
| `include/textures.h`, `src/textures.c`, `gfx/ra_mark.png` | the `RA_MARK` internal texture |
| `include/themes.h`, `src/themes.c`, `include/renderman.h`, `src/renderman.c` | `raIsCover` flag + `rmDrawInlayPixmap()` for the mark over the cover |
| `lng_tmpl/_base.yml` | new `_STR_RA_*` labels **appended at the end** |
| `src/dialogs.c` | RA credit — see the About-dialog constraint below |

**Deviations you must make — this is where upstream is wrong for us:**

1. **`RA_BADGE_SLOTS 4` is wrong.** Upstream says "OPL has exactly this many devices: BDM, ETH, HDD,
   APP". This fork has **14** (`MODE_COUNT` in `include/iosupport.h:9` — eight BDM slots, ETH, HDD,
   APP, MMCE, FAV, UDPFS). Use `MODE_COUNT`. With 4 slots the badge cache silently mis-attributes
   lists across devices.
2. **Hardcoded English strings.** Upstream ships `"RA: check game support"` etc. inline. Route every
   user-visible string through the language pipeline: new labels at the **end** of
   `lng_tmpl/_base.yml`, translations in `lng_fork/<Lang>.yml`. Watch the two known YAML traps —
   bool coercion, and a `colon-space` in a value breaks the overlay (use `--`).
3. **Main-menu items.** Upstream adds `MENU_RA_DISC_LAUNCH` / `MENU_RA_DISC_CHECK` to the main menu
   and then patches the separator arithmetic with a magic `+ 2`
   (`menuRenderMenu`, `if (cp == (MENU_ABOUT - 1 + 2))`). Skip both items in this phase — they
   are now included by Phase 5 (disc mode). The menu counts its entries; do not import the `+ 2`.
4. **About dialog.** Upstream adds six rows (`UI_LABEL`/`UI_BREAK`/`UI_SPACER`...). `diaAbout` never
   scrolls and has about one row of headroom. **Append "hacan359" to an existing credits line
   instead.** Add the full credit to `CREDITS` where there is room.
5. **`LOG_ENABLE()`.** Upstream disables it because `debugSetActive()` → `ethInitApplyConfig()` waits
   for link for 30 s in an unbounded loop, ahead of `deferredInit` in the I/O queue, so
   `gInitComplete` never gets set and the splash never ends. **Check whether that hazard exists in
   this fork before copying the workaround** — our I/O worker and init ordering were rewritten
   (`gemini-recovery-rebuild-82`). If it does not apply, do not import a silent debug-logging kill.
6. **Refuse clearly on the three unsupported legs.** If the selected game's `$CoreLoader` is
   Neutrino, or the device is UDPFS, or the entry is a PS1/VCD title, the RA menu action must say so
   ("RetroAchievements needs OPL's own loader core") instead of hashing something that will never be
   watched. See §1.3.

**The NIC collision — settle it here, explicitly.** `gNetworkProtocol` makes SMB / UDPBD / UDPFS
mutually exclusive, and `ethLoadModules()` (`src/ethsupport.c:296`) **refuses** to bring up
`netman`+`smap`+`ps2ip`+`ps2ips` when `bdmIsUDPBDLoaded()` or `udpfsGetModulesLoaded()` is true.
`ranet.c` needs exactly that stack.

Recommended settlement for this phase — the honest, minimal one:

- RA's menu check is a **fourth claimant** on the NIC, subject to the same interlock.
- If the eth stack is already up (protocol = SMB), use it.
- If the protocol is **Off**, RA may bring the stack up itself through `ethLoadInitModules()`,
  under the same guards.
- If **UDPBD or UDPFS** owns the NIC, the check **fails with a clear notice** — do not try to
  double-drive the SMAP EMAC. At best SMB fails to start; at worst the IOP wedges.
- The **in-game** half is unaffected: `raudp` hand-builds frames over whatever SMAP the launch
  loaded, so telemetry works even when the menu check does not.

Document the limitation in the user-facing docs. Decoupling the menu path (hand-built UDP over
whichever NIC driver is loaded, the way `raudp` does in-game) is a real option but a separate,
larger piece of work — park it as a follow-up, do not attempt it inside this phase.

**Acceptance:** with the PC client running, "RA: test PC connection" reports a round trip; "RA: check
game support" on a known-tracked game reports the title and achievement counts, writes
`<device>/RA/<serial>.wl`, and the badge and cover mark appear after the next list refresh.

### Phase 4 — The `ps2ips` bridge fixes (worth landing regardless)

Upstream replaces the prebuilt `$(PS2SDK)/iop/irx/ps2ips.irx` with a local copy because the stock
`do_recvfrom`:

- **overruns a 128-byte EE buffer** — it DMAs the 144-byte `rests_pkt` into a 128-byte `_intr_data`;
- **leaks sockets**;
- hands `fromlen` to lwIP **uninitialised**, so the sender address comes back as zeros;
- clobbers the DMA destination address, so UDP receive on the EE never worked at all.

Additionally, the socket non-blocking flag **does not take** on the netman stack, so every menu
receive must pass `MSG_DONTWAIT` per call — and it must be **lwIP's `0x08`**, not the EE
`<sys/socket.h>` `MSG_DONTWAIT` (`0x80`), because the flag is forwarded verbatim over SIF RPC.

**These are real bugs and this fork uses the stock `ps2ips.irx`** (`Makefile:759`,
`EE_LIBS ... -lps2ips`). Audit for them independently of RA. The 128-byte overrun in particular is a
memory-corruption bug that would explain unrelated flakiness.

**However:** swapping `ps2ips.irx` for a local build affects `nbns`, `httpclient` (compat updates)
and anything else on the EE socket API. Do that swap **only under `RETROACHIEVEMENTS=1`** in this
phase. If it proves clean on hardware, propose promoting it to the default build as a **separate PR
with its own hardware validation** — never as a side effect of the RA flavour.

### Phase 5 — Disc mode (implemented; PS2 validation pending)

The RA build now provides **RA: check disc support** and **RA: launch disc** in the main menu.
The port uses the upstream EESYNC-only IOPRP layout and standalone DEV9/SMSUTILS modules,
retains ROM CDVDMAN/CDVDFSV, and skips OPL CDVDMAN's shutdown RPC. RiptOPL reuses its bounded
physical-disc probe, active settings folder and normal frontend teardown.

The separate **Launch PS2 Disc** action retains the existing normal disc boot. Physical-disc RA
uses real memory cards and does not apply image-specific patches. See the
[current guide](RETROACHIEVEMENTS.md#physical-discs) for operation, limits and outstanding PS2 tests.
Upstream hardware results do not validate this port.

### Phase 6 — Documentation and credit

1. `docs/RETROACHIEVEMENTS.md` — user guide: what it does, which launch paths are supported
   (§1.3 table), which are not and why, the NIC/protocol limitation, where `RA/` lives, where to get
   the PC client.
2. `README.md` — the RA variant in the release-package listing. **Not** the External Tools
   section — that is generated.
3. `.github/workflows/docs-sync.yml` — add the PC client to the `html_section` template (and its
   README counterpart in the same file) once the author publishes it, crediting **hacan359** with a
   link. Verify the licence he ships it under before writing one down.
4. `CREDITS` — RetroAchievements integration, hacan359. Also the two vendored files and their
   licences.
5. `CHANGELOG` / rolling release notes.
6. `ROLLING_RELEASE.md` — the new variant filenames.

---

## 4. Runtime settings to add

Keep the surface small. Under `RETROACHIEVEMENTS`, in the network/device settings area:

| Setting | Default | Effect |
|---|---|---|
| `RA telemetry` | **Off** | Master switch. Off ⇒ no `CORE_IRX_ETH` forcing, no `raudp`, no snapshot cost. |
| `RA badges in list` | On (when telemetry on) | Purely cosmetic; lets a user keep telemetry without the name prefix. |

Per-game override is **not** needed in v1 — the presence of a `.wl` file already scopes the feature
per game.

Config keys go through `config.c` the way every other fork setting does. New keys are additive;
absent key ⇒ default. Do not reuse a key number.

---

## 5. Traps, in one place

1. **Wrong base branch.** `master` is stale since 2026-08-14 and is cut out of publishing. Use
   `origin/rebuild/main`.
2. **`ps2ips` small-packet path.** A datagram ≤64 bytes, or unaligned edges, takes the broken
   `rests_pkt` route. The protocol pads every reply to a multiple of 64 and ≥128 bytes for exactly
   this reason. If you "optimise" the padding away, receive breaks on real hardware and works fine
   in an emulator.
3. **960-byte reply ceiling.** IOP side has 1024 bytes per call; receive lands at offset 64 for an
   aligned buffer; a datagram over 992 overflows `lwip_buffer`. Chunk size 896 is not arbitrary.
4. **`from`/`fromlen` must always be passed** to `recvfrom` even though unused — the EE wrapper
   `memcpy`s into `from` unconditionally, and `NULL` means a write to address zero.
5. **`g_rx` must stay 64-byte aligned.**
6. **Never wait on DMA inside the VBLANK handler.** Skip the frame.
7. **Snapshot reads go through `UNCACHED_SEG`.** Cached reads would pull up to a thousand cache
   lines per frame through the game's 8 KB data cache. The cost is that a value the game wrote
   moments ago may lag by a write-back — well under a frame, and correct by design.
8. **Two IOP threads now call `SMAPSendPacket`** (the stack thread and raudp). The TX semaphore is
   load-bearing; without it one call lands inside the other's FIFO write.
9. **`MSG_DONTWAIT` is `0x08` here, not `0x80`.**
10. **`lwip_recvfrom` in SMSTCPIP takes 8 arguments,** not 6. The 6-arg declaration compiles and puts
    arguments in the wrong IOP registers.
11. **Deleting `obj/` is mandatory** after any change to `EECoreConfig_t` or `OPL_MODULE_ID`.
12. **`bdmsupport.c` contains NUL bytes** — `grep` calls it binary. Use `grep -a`.
13. **`RA_DEBUG`** exists upstream as a separate HUD-probe build. If you port it, make it a third
    orthogonal flag, not a mode of `RETROACHIEVEMENTS`. Better: fold tester-facing diagnostics into
    the existing `__OPLDIAG` flavour, since CI builds `make release` and `__DEBUG` never reaches a
    tester anyway.

---

## 6. Verification

**Build:** both SDK containers (pinned digest + `ps2dev:latest`), both flag states, `make clean`
between. Then `clang-format` 12 across the diff, minus the `.clang-format-ignore` entries.

**Emulator (PCSX2):** menu boots, RA menu entries render and are navigable, the flag-off build is
unchanged, no crash on a game with no `.wl`. That is the ceiling — GS raster timing and real SMAP
behaviour are not testable there.

**Hardware — required before this ships to anyone:**

| # | Test | Expected |
|---|---|---|
| 1 | Default build (`RETROACHIEVEMENTS=0`) unchanged | no regression on any existing path |
| 2 | RA build, telemetry Off | behaves as the default build |
| 3 | USB launch, valid `.wl`, telemetry On | ~60 snapshots/s sustained ≥5 min, skip/fail counters flat |
| 4 | Same, watching frame rate | no perceptible impact |
| 5 | SMB launch with telemetry | game's own SMB stream unaffected (the `MEM_SIZE 0x400` → `0x1000` bump exists because a 536-byte packet starved it and the console rebooted) |
| 6 | Menu check, protocol = SMB | title + counts, `.wl` written |
| 7 | Menu check, protocol = Off | RA brings the stack up, same result |
| 8 | Menu check, protocol = UDPBD / UDPFS | clean refusal notice, no wedge |
| 9 | Neutrino-core game | clean refusal, no hash attempt |
| 10 | PS1/VCD entry | clean refusal |
| 11 | Badge + cover mark | appear after refresh, correct game, correct device |
| 12 | Unlock overlay | gold pulse on `RAU1`, game keeps running |
| 13 | IGR combo in an RA launch | still resets |
| 14 | MX4SIO launch with telemetry | no navigation/art regression (this device has a history) |

Hand testers a **run-pinned nightly.link build**, never a bare artifact link. Standing rule.

---

## 7. Upstream file map

Reference only — for looking up how upstream did a thing. Do not apply.

```
gh pr diff 1 --repo hacan359/Open-PS2-Loader > ra.patch
git clone --depth 4 --single-branch --branch ra https://github.com/hacan359/Open-PS2-Loader.git
```

| Area | Files |
|---|---|
| New EE units | `src/{ranet,rahash,rawatch,rabadge,md5,discsupport}.c` + headers |
| New ee_core units | `ee_core/src/{ra,ra_overlay}.c` + headers |
| New IOP modules | `modules/network/raudp/`, `modules/network/ps2ips-ra/` |
| Shared headers | `modules/network/common/{ra_snap,ra_watch,smap_tx}.h` |
| Touched, menu | `opl.c`, `gui.c`, `menusys.c`, `dialogs.c`, `supportbase.c`, `themes.c`, `textures.c`, `renderman.c`, `system.c`, `ioprp.c`, `{bdm,eth,hdd}support.c` |
| Touched, ee_core | `main.c`, `iopmgr.c`, `padhook.c`, `coreconfig.h`, `ee_core.h`, `modules.h` |
| Touched, network | `SMSTCPIP/{etharp.c,exports.tab,Makefile,include/lwipopts.h}`, `smap-ingame/*`, `common/smstcpip.h` |
| Build | `Makefile`, `ee_core/Makefile`, `.clang-format-ignore` |
| Asset | `gfx/ra_mark.png` |

---

## 8. Open questions for the maintainer

Answer these before Phase 3 lands; they do not block Phases 0-2.

1. **PC client.** Has the author delivered the platform-independent build and the protocol spec? Under
   what licence, and does it get a `pc/` entry, a docs-sync External Tools entry, or just a link?
2. **NIC settlement.** Is the Phase 3 recommendation (RA as a fourth interlock claimant, clean
   refusal under UDPBD/UDPFS) acceptable for v1, or should the decoupled hand-built menu path be
   scoped now?
3. **Disc mode.** Implemented in the RA flavour; PS2 validation remains outstanding.
4. **`ps2ips` fixes.** Land them RA-only first, as planned — or fast-track the buffer-overrun fix to
   the default build as its own PR?
