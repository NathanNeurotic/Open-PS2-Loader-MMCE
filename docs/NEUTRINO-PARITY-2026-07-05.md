# Why NHDDL "just works" with Neutrino — and what RiptOPL still needs (post-PR#79)

Scope note: PR#79 already closed the biggest historical gaps (no-reset elfldr child with SifLoadElf-primary/fileXio-rescue, argv[0] preservation, gameID-before-fd-close + fs-settle, stock in-game mmcedrv). This document is only about what REMAINS. One premise correction from the wOPL read: wOPL is **not** a "no-Neutrino baseline" — wopl/wOPL-base ships the RiptOPL-derived Neutrino core (`include/config_wopl.h:12-13`, `src/system.c:1260`, README credits Ripto). Its stability envelope is "RiptOPL's design minus the risky extremities," which independently validates the core architecture.

---

## 1. Architectural root causes of NHDDL's reliability

These are structures, not incidents — what NHDDL *never does* and what it *keeps constant*:

1. **One IOP lifetime, no mid-session driver churn.** SifIopReset exactly once at startup (`nhddl/src/module_init.c:127-141`), every IRX from EE-embedded buffers via SifExecModuleBuffer — never loaded from storage, never loaded mid-launch, never unloaded (no deinit code exists anywhere). The drivers that scanned the games are byte-for-byte the drivers that read neutrino.elf at handoff.
2. **Exactly one filesystem access after the last state mutation.** Handoff order is: lastTitle write → HDL sync → mmceMountVMC (gameID + built-in busy-poll settle, `devices_mmce.c:44-75`) → SifLoadElf of neutrino.elf. Nothing else touches storage after the VMC switch (`launcher.c:66-196`).
3. **The launch path does no work Neutrino will redo.** No frag lists, no IRX patching, no cheats, no L1 probes, no VMC validation — Neutrino re-derives everything from `-bsd`/`-dvd` post-reset. NHDDL's pre-handoff cost is ~3 small file operations.
4. **No `-mc` file args at all.** VMC is exclusively the MMCE devctl-0x8 card switch; there is no second VMC mechanism to interact with the first.
5. **One global neutrino.elf resolution, done at init, shown to the user.** `NEUTRINO_ELF_PATH` is resolved once (`nhddl/src/main.c:474-530`), version.txt is read and displayed — the user knows *which* Neutrino will run before any launch.
6. **Single-threaded, near-zero menu I/O.** One cover PNG per cursor move, no IO worker, no queues to drain, no module latches to reason about at teardown.
7. **Conflicts prevented structurally, not debugged:** mmceman never loaded alongside MX4SIO; MX4SIO excluded from MODE_ALL (`module_init.c:148-149`, `main.c:241-242`).
8. **Failure is honest and early.** Neutrino's own contract (buffer-everything-then-reset, `neutrino ee/loader/src/main.c:695-717`) means the launcher only needs live drivers for the *neutrino folder*, not the game device; NHDDL keeps everything alive, so the only real failure window is Neutrino's own post-reset device open.

RiptOPL post-#79 already matches 1, 2 (mostly), and the handoff mechanics. The remaining deltas are in 3, 4, 5, and 6.

---

## 2. Ranked remaining deltas (user-pain-per-effort order)

### Δ1 — Resolution probes only the `.elf`; Neutrino needs `config/` + `modules/` beside it — **S**
- **Failure mode:** a stale/partial `neutrino/` folder (elf present, `modules/` or `config/` missing, or an old version) resolves successfully, survives the elfldr open()-probe, and dies *inside Neutrino, post-teardown* — printf-only, black screen. Neutrino hard-fails on any missing toml/irx (`neutrino config.c:402-405`, `modlist.c:39-43`).
- **Evidence:** `sbFileExists` is a bare open/close of the elf (`src/supportbase.c:544-553`, verified); no probe of `config/system.toml` or `modules/` anywhere in `sbResolveNeutrinoPath` (`supportbase.c:558-675`).
- **Convergence:** in `sbResolveNeutrinoPath`, after an elf hit, additionally require `sbFileExists("<dir>/config/system.toml")` **OR** `sbFileExists("<dir>/system.toml")` (the second covers Neutrino's flat "SAS" layout, `neutrino main.c:489-498`). A candidate failing this is skipped and probing continues — a stale game-device folder no longer shadows a good MC install.
- **Verify:** static — unit-style: create `mass0:/neutrino/` with elf only on PCSX2/HW, confirm resolver skips it and lands on mc0 install; then full folder, confirm it wins again.

### Δ2 — `-mcN` paths emitted unverified; Neutrino aborts the whole boot on an unopenable VMC — **S**
- **Failure mode:** user's `$VMC_0` names a deleted/renamed .bin → Neutrino's fhi open fails post-reset → `return -1` into a dead LE = black screen ("no 'boot without VMC' fallback", `neutrino fhi_config.c:97-100`, `main.c:907-920`).
- **Evidence:** `sbBuildVmcNeutrinoArgs` formats `-mc<slot>=<prefix>VMC/<name>.bin` from config with truncation LOG-only, no existence check (`src/supportbase.c:1086-1108`). NHDDL cannot hit this class because it never emits `-mc`.
- **Convergence:** in `sbBuildVmcNeutrinoArgs`, `sbFileExists()` each path pre-emit (mounts are still up — this is pre-deinit). On miss: 6s `guiWarning` toast ("VMC not found, launching without") and skip the arg. Bonus: this also converts the truncation LOG into the same toast.
- **Verify:** HW/PCSX2 — configure a VMC, delete the .bin, launch via Neutrino: expect toast + game boots with real card, not black screen.

### Δ3 — VMC `-mc` file args and the MMCE card gameID switch can both apply to one launch — **S**
- **Failure mode (double application):** on a cross-device launch with `$VMC_0` set and an MMCE card in slot 1, RiptOPL both (a) points Neutrino's fhi at the .bin (game sees the emulated VMC on slot 0) and (b) devctl-0x8-switches the physical MMCE card's per-game folder. The card switch is useless for that slot, adds the busy-poll latency, and — since the fs-settle wait only guards an mmce-hosted neutrino.elf (`src/mmcesupport.c:615-626`) — a slow Gen2 re-mount can still race any subsequent mmce read. NHDDL never has two VMC mechanisms; wOPL guards only the same neutrino-on-mc collision.
- **Evidence:** "Nothing suppresses the gameID push just because a -mcN VMC arg exists" — the only guard is `protectMcPath` for neutrino.elf itself (`src/mmcesupport.c:84-98`).
- **Convergence:** in each leg's `mmceSendGameID` call site (or centrally in `mmceSendGameID`), skip the push for a slot whose corresponding `-mc<slot>` arg is nonempty (mmce0↔`-mc0`, mmce1↔`-mc1`); if both slots are covered, skip entirely with a LOG. Keep the push when only the *other* slot has a card.
- **Verify:** HW (MMCE required) — launch USB game with `$VMC_0` + MMCE in slot 1: card folder must NOT switch; game must see the .bin. Repeat with card in slot 2, no `$VMC_1`: folder DOES switch.

### Δ4 — mmceSendGameID self-arms an IOP IRX load *at launch time* — **S**
- **Failure mode:** cross-device launch on a console where the MMCE page was never enabled → `mmceLoadModules()` fires inside the launch sequence (`src/mmcesupport.c:62-64`); a wedged mmceman load/start at that moment kills the launch at its most fragile point. NHDDL loads mmceman once at boot; wOPL "never self-arms — cross-device sends silently NO-OP if the page was never enabled" (`wopl src/mmcesupport.c:104-141`).
- **Convergence:** move the arming out of the launch path: when `gMMCEEnableGameID` is on, arm mmceman at GUI init / settings-apply time (where a failure is a harmless LOG), and have `mmceSendGameID` no-op if the module isn't already resident. Preserves the #51 fix's *intent* (gameID works without the MMCE page) while moving the risk off the launch path.
- **Verify:** HW — MMCE page disabled, gameID on: confirm mmceman loads at menu time (LOG) and the launch path does no IRX load; gameID still switches folders on a USB launch.

### Δ5 — Resolution is per-launch and silent; stale game-device folders can shadow a maintained install with zero visibility — **S**
- **Failure mode:** AUTO probes the ACTIVE game device *before* mc0/mc1 (`supportbase.c:627-658`, deliberate, #300). User updates Neutrino on mc0 but forgot the old `neutrino/` on the games USB → old version silently wins → old cdvdman streaming bug (fixed only in Neutrino HEAD, per contract read v1.8.0→HEAD delta (e)) reappears "randomly". NHDDL's structural answer isn't a different order — it's *visibility*: resolve once, read `version.txt`, show it on the splash (`nhddl/src/main.c:534-570`).
- **Convergence:** keep the probe order (it's the zero-config feature). Add: after resolution, read `<dir>/version.txt` if present and (a) always LOG `[NEUTRINO] using <path> (<version>)`, (b) surface path+version in the pre-launch GameID flash screen or as a brief toast on the *first* launch of a session. Δ1's completeness check already removes the *broken*-stale case; this removes the *silent-old* case.
- **Verify:** static + PCSX2 — two installs, confirm LOG names the winner; visual check of the flash line.

### Δ6 — Post-deinit filesystem work that belongs pre-deinit (udpfs toml sync, device-token validation) — **S/M**
- **Failure mode:** (a) `sysSyncNeutrinoUdpfsToml` does a read + O_TRUNC write of `config/bsd-<dev>.toml` *after* deinitEx (`src/system.c:1050-1111, 1221`, verified) — any failure there is invisible and a power cut in the window truncates the toml (short-write restore mitigates, doesn't eliminate); files >2048 bytes are silently left unsynced. (b) `getDeviceName` "unsupported" aborts post-deinit into a dead GUI (`src/system.c:1121-1124`).
- **Evidence:** Neutrino has *no* CLI override for the PS2 IP — toml rewrite is the only mechanism (contract read: `bsd-udpbd.toml` hardcoded `ip=192.168.1.10`), so the sync must exist; it just runs at the wrong time.
- **Convergence:** (a) hoist the toml sync into the udpfs/udp legs *before* `deinitEx` (both inputs — neutrinoPath and the static IP — are known there); on failure, toast + abort while the GUI is alive (matching the existing `_STR_NET_NEEDS_NEUTRINO` pattern). Raise or lift the 2048-byte cap while touching it. (b) compute/validate the device token in each leg pre-deinit; pass the validated token into `sysLaunchNeutrino`.
- **Verify:** HW (UDPFS rig) — normal launch still boots; corrupt/oversize toml → toast + return to menu, drive intact.

### Δ7 — Launch-path deinitEx keeps an UNBOUNDED IO-worker drain — **S (code) / M (care)**
- **Failure mode:** a wedged art/cover request on a dying or slow device = permanently frozen "loading" screen *before* the handoff even starts (`src/opl.c:2328-2338`, deliberately unbounded "so its IOP state stays clean"). NHDDL has no IO worker to drain; wOPL drains but its requests are all local-device.
- **Convergence:** bound the launch-path drain (e.g. 10s), preceded by the existing abort calls (`cacheAbortMmceImageLoadsTimed`/`cacheCancelPendingImageLoadsTimed` already run first, `opl.c:2325-2326`); on timeout, LOG and proceed. Rationale this is safe post-#79: the two excepted mounts stay alive regardless, and Neutrino performs its own IOP reset after reading its files — a straggler request can't corrupt the LE Neutrino builds fresh. Keep the unbounded wait for exit/poweroff.
- **Verify:** HW — yank a USB mid-cover-load, immediately launch from another device: launch proceeds ≤10s later instead of hanging. Regression: normal launches on all devices.

### Δ8 — The coreLoader gate sits AFTER the full native-launch preparation on bdm/hdd — **M–L**
- **Failure mode(s):** a Neutrino launch still pays and can *die on* native-only work: per-part open()+`GET_FRAGLIST` with an abort on `_STR_ERR_FRAGMENTED` (`src/bdmsupport.c:815-844`) — even though Neutrino's bd backend re-derives fragments itself and its mmce/udpfs fileid backend has **no** fragment limit (`neutrino README.md:62`); interactive `guiManageCheats` dialog mid-launch (`src/supportbase.c:1148-1149`) for a core that ignores OPL cheats (`src/menusys.c:1459-1463`); VMC superblock validation with continue/cancel prompts whose result HDD then discards (vmcArgs NULL, `src/hddsupport.c:902`); cdvdman IRX patch + mcemu patch, L1 probes — all dead weight. Only the mmce leg moved its gate early (`src/mmcesupport.c:561-562`).
- **Evidence:** gate positions — `bdmsupport.c:932` after lines 672-928 of prep; `hddsupport.c:791` after 668-775. NHDDL's entire pre-handoff is ~3 file ops.
- **Convergence:** in `bdmLaunchGame` and `hddLaunchGame`, read `CONFIG_ITEM_CORE_LOADER` (+ the UDPBD force) *first* and branch into a lean Neutrino sub-path: config reads (args/gsm/VMC names) → CONFIG_LAST save → resolve (with Δ1 check) → gameID (with Δ3 guard) → deinitEx → sysLaunchNeutrino. Skip frag lists, sbPrepare, mcemu, cheats, L1 probes, PS2Logo read (keep PS2Logo only if `-logo` is emitted — Neutrino does its own logo work per contract step 9). This is the single biggest simplification and removes two *interactive dialogs* and one *hard abort* from the Neutrino path.
- **Verify:** HW matrix — per device (USB/MX4SIO/exFAT-HDD/APA/MMCE/UDPFS): Neutrino launch of a deliberately fragmented ISO (must now boot — fileid backends — or fail inside Neutrino's own 64-frag check for bd), cheats file present (no dialog), VMC configured (still works via `-mc`). Native launches unchanged (gate untouched for coreLoader=0).

### Δ9 — No frag-budget pre-check for the bd backend's combined 64-fragment limit — **M**
- **Failure mode:** ISO + VMC .bin(s) on a bd-backend device (USB/MX4SIO/exFAT/ATA) jointly exceed `BDM_MAX_FRAGS=64` → Neutrino aborts post-reset ("Too many fragments", `neutrino fhi_config.c:133-136`) = black screen. genvmc-created VMCs fragment easily on well-used cards. NHDDL structurally immune (no `-mc`).
- **Convergence:** RiptOPL already owns the `USBMASS_IOCTL_GET_FRAGLIST` machinery (`src/bdmsupport.c:815-844`); in the lean Neutrino sub-path (Δ8), run a cheap *count-only* pass over ISO + each `-mc` file; if total >64, toast ("Too fragmented for Neutrino — defrag or use OPL core") and fall back / abort pre-deinit. Do this only for bd-backend devices — skip mmce/udpfs (fileid, no limit).
- **Verify:** HW — fragment a VMC on purpose (fill/delete cycles), confirm toast instead of black screen; clean device boots normally.

### Δ10 — (Optional, beyond NHDDL) pass `-elf=cdrom0:\<startup>;1` to unlock pre-reset per-game compat — **M**
- **Failure mode it fixes:** with `-elf=auto`, Neutrino applies per-GameID compat.toml entries *after* its IOP reset, and any entry needing extra module/config files fails hard with only a printf hint (`neutrino main.c:830-841`). Neither NHDDL nor RiptOPL passes `-elf` today — RiptOPL can do *better* than NHDDL here because it already knows `game->startup`.
- **Convergence:** in `sysLaunchNeutrino`, emit `-elf=cdrom0:\<startup>;1` when startup matches the `AAAA_NNN.NN` shape (it always does for scanned games — this shape-guard also avoids Neutrino's uninitialized-`sGameID` hazard for non-matching names, `neutrino main.c:418, 600-603`).
- **Caution:** explicit `-elf` bypasses Neutrino's own SYSTEM.CNF autodetect — the startup string must be exact. Ship behind a LOG-visible flag first.
- **Verify:** HW — game with a per-GameID compat entry carrying `depends` files: boots with the entry applied (Neutrino printf log via serial/udptty); several ordinary games regression-boot.

---

## 3. Special-attention items — direct answers

- **(a) Auto order vs stale folders:** covered by Δ1 (completeness check kills broken-stale) + Δ5 (version visibility kills silent-old). Do **not** reorder AUTO — game-device-first is the #300 zero-config feature and matches NHDDL's own "<device>/neutrino/" fallback; NHDDL's real edge is that its pick is *validated by the user's eyes at boot*, which Δ5 replicates.
- **(b) VMC `-mc` vs card gameID:** yes, double-application is real and unguarded today — Δ3.
- **(c) elf-only probing:** confirmed against live source (`supportbase.c:544-553`) — Δ1.
- **(d) menu-session state leaks:** the *tolerated* leaks (two excepted mounts, resident UDPRDMA chain, resident mmceman) are correct-by-design because Neutrino IOP-resets after reading its files; the *actionable* leaks are the launch-time module self-arm (Δ4) and the unbounded IO drain (Δ7). VMC fd ordering was fixed in #79; the mmce gate already precedes the iso open, so no fd crosses the handoff on that leg.
- **(e) outright contract violations:** two found — unverified `-mc` paths vs Neutrino's abort-on-unopenable-VMC (Δ2), and no respect for the bd backend's combined 64-frag/same-device budget (Δ9 — same-device is satisfied since `-mc` uses the game prefix, `supportbase.c:1103`; the frag budget is not checked). Everything else checks out: explicit `-bsd` always emitted (so the `mass:` autodetect blacklist, `neutrino config.c:31-59`, never bites), `-gc=0` never emitted, single `-gsm`/`-cwd` dedup, `-cwd` always supplied (`system.c:1206-1216`, verified), APA via `-bsd=ata -bsdfs=hdl -dvd=hdl:` matches the contract exactly.

---

## 4. What RiptOPL should NOT copy from NHDDL

1. **Fail-to-black launch semantics.** NHDDL frees the target list and closes the UI *before* launching; every failure is terminal (`nhddl gui.c:252-258`, `main.c:133-151`). RiptOPL's pre-deinit toasts + native-core fallback + clean abort-to-menu on network legs are strictly better UX. Keep them; the goal is to move *more* failures pre-deinit (Δ1/Δ2/Δ6), not to adopt NHDDL's "die honestly" model.
2. **`-qb` quickboot.** NHDDL passes `-qb` for all non-HDL modes because its resident IOP is minimal and per-mode-filtered. `-qb` makes Neutrino skip its own IOP reset and inherit the launcher's IOP (`neutrino main.c:719, 766-773`) — after a RiptOPL menu session (art worker history, ETH/SMAP state, self-armed modules), that inheritance is exactly the mixed-state class Neutrino's reset exists to erase. Do not emit `-qb`.
3. **Dropping `-mc` VMC file support.** NHDDL's "no VMC files" is its single biggest reliability simplification, but RiptOPL users on USB/MX4SIO/exFAT rely on per-game .bin VMCs. Keep the feature; harden it (Δ2, Δ3, Δ9) instead.
4. **Boot-time static module set / no device pages.** NHDDL loads a fixed per-mode set once and never touches modules again. Copying that would cost SMB, UDPFS/UDPBD hot-enable, VCD/POPSTARTER, MMCE page semantics — the fork's identity. The wOPL read confirms the dual-exception `deinitEx` (a RiptOPL-only extension, `src/opl.c:2318-2358`) is the *more correct* contract for a keep-IOP handoff than wOPL's single exception; don't regress it.
5. **Resolve-once-at-boot.** RiptOPL devices mount/unmount across a session (device pages, hotplug); per-launch resolution is required. Δ5's visibility gets NHDDL's benefit without the constraint.
6. **NHDDL's mmceMountVMC mc-only guard.** It doesn't guard `mmceN:`-hosted neutrino at all (`nhddl devices_mmce.c:52`); RiptOPL's `protectMcPath` + #79 fs-settle is already ahead — as is the fileXio rescue (wOPL/NHDDL loaders both single-attempt SifLoadElf).

---

## 5. Consolidated plan (user-pain-per-effort order)

| # | Change | Size | Files/functions | Verification |
|---|--------|------|-----------------|--------------|
| 1 | Δ1 completeness probe (config/system.toml OR flat system.toml) | S | `src/supportbase.c` `sbResolveNeutrinoPath` | Static + PCSX2: elf-only folder skipped, full folder wins; HW spot-check |
| 2 | Δ2 `-mc` existence check + toast/skip | S | `src/supportbase.c` `sbBuildVmcNeutrinoArgs` | HW: deleted .bin → toast, boots without VMC |
| 3 | Δ3 suppress gameID for slots covered by `-mc` args | S | `src/mmcesupport.c` `mmceSendGameID` + call sites | HW (MMCE): slot-covered no-switch; other-slot still switches |
| 4 | Δ4 stop launch-time mmceman self-arm; arm at menu/settings time | S | `src/mmcesupport.c:62-64` → GUI init path | HW: page-off + gameID-on, LOG shows menu-time load, launch path clean |
| 5 | Δ5 version.txt read + resolved-path LOG/flash | S | `src/supportbase.c`, launch flash in `src/opl.c`/`menusys.c` | Static + visual on PCSX2 |
| 6 | Δ6 hoist udpfs toml sync + device-token validation pre-deinit | S/M | `src/system.c` `sysSyncNeutrinoUdpfsToml` → udpfs/udp legs; `getDeviceName` validation into legs | HW UDPFS rig: failure → toast + menu, not black screen |
| 7 | Δ7 bound the launch-path ioBlockOps drain (~10s, LOG + proceed) | S | `src/opl.c:2328-2338` `deinitEx` | HW: yank-device-then-launch proceeds; full regression matrix |
| 8 | Δ8 early coreLoader gate + lean Neutrino sub-path (bdm, hdd) | M–L | `src/bdmsupport.c` `bdmLaunchGame`, `src/hddsupport.c` `hddLaunchGame` | HW matrix per device: fragmented ISO, cheats present (no dialog), VMC works; native path unchanged |
| 9 | Δ9 64-frag budget pre-count (bd-backend devices, ISO+VMCs) | M | new count-only helper reusing `bdmsupport.c` fraglist code, called from Δ8's sub-path | HW: fragmented VMC → toast; clean device boots |
| 10 | Δ10 optional `-elf=cdrom0:\<startup>;1` (shape-guarded, flagged) | M | `src/system.c` `sysLaunchNeutrino` | HW: per-GameID compat entry with file deps applies pre-reset (udptty log); broad regression boots |

**Status (2026-07-05):** Δ1–Δ7 shipped via PRs #81/#84/#86 (rolling Beta-2965). Δ8 merged as PR #87 (rolling Beta-2969; `bdmTryNeutrinoLaunch`/`hddTryNeutrinoLaunch` early gates; hdd keeps a single-sector ZSO probe → native fallback; udp abort paths owned by the bdm helper). Δ9 implemented (`bdmNeutrinoFragBudgetOk`: count-only GET_FRAGLIST over ISO + both `-mc` VMCs against neutrino's shared 64-entry table, verified against neutrino fhi_config.c accounting — overflow toasts pre-deinit and udp-aborts / falls native; `guiWarning` gained a pre-GUI guard so autolaunch toasts LOG instead of drawing through a NULL theme). Δ10 not started.

**Bottom line:** NHDDL's reliability is not magic — it is (1) one IOP lifetime, (2) a nearly-empty launch path, (3) one VMC mechanism, (4) a validated, user-visible Neutrino pick. RiptOPL already owns (1) post-#79 and has a *better* failure UX and loader than NHDDL. The remaining convergence is: validate what you resolve (Δ1/Δ2/Δ5), stop doing native-core work and risky mutations after the point where the GUI can't report them (Δ6/Δ8), and de-conflict the two VMC mechanisms NHDDL never had to reconcile (Δ3/Δ9).