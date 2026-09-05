# RetroAchievements — what is left, and how to finish it

**Working handover document for the next agent on `claude/retroachievements-port-bb1959` (PR #600).**

Written 2026-09-04 against branch head `2db46cc6`. **Updated 2026-09-05: all three blockers are
cleared and phase 3 is written.** Every claim below was checked against the tree; where a claim is
inherited from `docs/RETROACHIEVEMENTS-INTEGRATION-PLAN.md` rather than re-verified, it says so.

## Where it stands now

| | |
|---|---|
| **B1 rebase** | done — the branch sits on `rebuild/main` |
| **B2 `ps2ips-ra`** | done — dropped during the rebase (`89a4eee0`) |
| **B3 settings screens** | done — the RA rows went on the live Network page |
| **Phase 3** | **written**, in `81755006` (3a), `e0861619` (3b), `340c6e4f` (3c), `e62f6880` (3d) |
| **Phase 6 docs** | status text refreshed; docs-sync template entry + changelog still open |
| **Hardware** | **nothing has run on a PS2.** This is the entire remaining risk. |

So the next agent's job is no longer to write phase 3. It is: get a build in front of a tester,
work the acceptance matrix in §5, and close out the two remaining doc items. Sections 0 and 1 below
are kept for the record — they explain *why* the tree looks the way it does.

> **This file must not survive the merge.** `rebuild/main` commit `b603dadd` (2026-09-03) deleted the
> repo-root handover notes (`HANDOFF.md`, `PICKUP-PROMPT.md`, `MMCE_PHASE_A_PARITY.md`,
> `HANDOFF-340-navigation.md`) on purpose. Delete this file, or fold what is still true into
> `docs/RETROACHIEVEMENTS.md`, in the commit that closes the feature. It lives at the root only
> because it is a scratch coordination note for work in flight.

The design document is `docs/RETROACHIEVEMENTS-INTEGRATION-PLAN.md`. **It is still correct about
design and wrong about state** — it was written before implementation started. This file is the
authority on what is built; that one is the authority on why.

---

## 0. Three blockers — ALL CLEARED. Kept for the reasoning.

### B1 — ~~The branch is 22 commits behind `rebuild/main`~~ — DONE

```
git fetch origin
git rebase origin/rebuild/main
```

The conflict surface is exactly four files. Resolutions:

| File | Conflict | Resolution |
|---|---|---|
| `.clang-format-ignore` | branch adds `./src/md5.c`, `./include/md5.h`, `./modules/network/ps2ips-ra/ps2ips.c`; main adds `./modules/network/ps2ips/ps2ips.c` | keep main's `ps2ips/ps2ips.c`, keep both md5 lines, **drop the `ps2ips-ra` line** (see B2) |
| `Makefile` | branch's `PS2IPS_IRX` switch vs main's unconditional `modules/network/ps2ips` rule | take main's; delete the branch's `PS2IPS_IRX` block and the `-ps2ips-ra` clean echo (see B2) |
| `README.md` | branch adds the RA variant row; main rewrote the intro and added the banner | keep main's prose, re-apply the RA variant row only |
| `include/supportbase.h` | branch adds `sbLoadWatchList`; main removed dead declarations (`1909f58d`) | keep both edits — they do not overlap semantically |

Do not merge instead of rebasing: the PR is a phase-per-commit stack and bisectability is the whole
point of that shape.

### B2 — ~~Phase 4 is obsolete~~ — DONE (`89a4eee0`; the phase-4 commit was dropped in the rebase)

`rebuild/main` commit `561d91d3` ("Replace the ps2sdk ps2ips prebuilt with a fixed copy", 2026-09-03)
landed the same module for the **default** build, unconditionally, at `modules/network/ps2ips/`.

The two files are **byte-identical** — both `modules/network/ps2ips/ps2ips.c` on `rebuild/main` and
`modules/network/ps2ips-ra/ps2ips.c` on this branch are 722 lines with md5
`d1908a8e6d73bedc98aa1f7341137250`. Verify before deleting:

```
git show origin/rebuild/main:modules/network/ps2ips/ps2ips.c | md5sum
git show HEAD:modules/network/ps2ips-ra/ps2ips.c | md5sum
```

If they still match, phase 4's premise is gone: the fix is already in every build, so a second copy
gated on `RETROACHIEVEMENTS=1` would mean the RA flavour builds a *different* ps2ips than the default
flavour it is supposed to be a superset of. Unwind it:

1. `git rm -r modules/network/ps2ips-ra/`
2. Remove the `PS2IPS_IRX` conditional from `Makefile` and the `-ps2ips-ra` line from the clean target
   (the branch's `Makefile:469-470, 830-855`); `rebuild/main`'s rule at `Makefile:763-787` already
   builds the vendored module for everyone.
3. Remove `./modules/network/ps2ips-ra/ps2ips.c` from `.clang-format-ignore`.
4. Drop the RA-only wording from `docs/RETROACHIEVEMENTS.md` — the fix is not RA-specific any more.

Commit `2db46cc6` ("RA phase 4: build our own ps2ips instead of the ps2sdk prebuilt") becomes an
empty-in-effect commit after the rebase. Either drop it during the rebase or add a follow-up commit
that removes the directory, explaining that main promoted it. **Do not leave both directories in the
tree** — two copies of a 722-line IOP module with no build-time link between them is exactly the
defect shape this fork keeps getting bitten by.

Open question #4 in the design doc ("land ps2ips RA-only first, then propose promotion") is
**answered and superseded by events**: promotion already happened. Do not re-ask it.

### B3 — ~~Ten settings screens were deleted~~ — DONE (the RA rows are on the Network page)

`rebuild/main` commit `35180354` removed `guiShowAdvancedConfig`, `guiShowStorageConfig`,
`guiShowDisplayConfig`, `guiShowLaunchConfig`, `guiShowSecurityConfig`, `guiShowVcdConfig`,
`guiShowMmcePathConfig`, `guiShowMmceCommConfig`, `guiShowParentalLockConfig`,
`guiShowPathPrefixConfig` and three orphaned `UIItem` arrays. They had been superseded by the
eight-page peer shell (PR #527) and had no callers — a row added to one of them rendered with no
enum list and silently never saved. Somebody already lost time to that.

The **live** settings pages, from the dispatcher at `src/gui.c:3065` on `rebuild/main`:

| Page | Function |
|---|---|
| `SETTINGS_GENERAL` | `guiSettingsShowGeneral` (`src/gui.c:2423`) |
| `SETTINGS_SOURCES` | `guiSettingsShowSources` (`:2496`) |
| `SETTINGS_NETWORK` | `guiShowNetConfig` (`:1171`) |
| `SETTINGS_INTERFACE` | `guiSettingsShowInterface` (`:2621`) |
| `SETTINGS_LAUNCH` | `guiSettingsShowLaunch` (`:2755`) |
| `SETTINGS_POPSTARTER` | `guiSettingsShowPopstarter` (`:2829`) |
| `SETTINGS_CONTROLLERS` | `guiShowControllerConfig` |
| `SETTINGS_AUDIO` | `guiShowAudioConfig` |

**RA settings belong on `SETTINGS_NETWORK` (`guiShowNetConfig`, backed by `diaNetConfig`).** RA
telemetry is a network feature and the NIC-protocol interlock it is subject to already lives on that
page. Re-verify the line numbers after your rebase before you edit — they will have moved.

---

## 1. Verified inventory — what is actually built

Checked by listing the branch tree and grepping for wiring, not by reading the plan.

| Phase | State | Evidence |
|---|---|---|
| 0 — build flavour | **done** | `RETROACHIEVEMENTS` in `Makefile`, `ee_core/Makefile`, `.github/scripts/build_rolling_extras.sh`; `src/md5.c`, `include/md5.h` vendored |
| 1 — in-game telemetry | **done** | `modules/network/raudp/*` (5 files), `modules/network/common/{ra_snap,ra_watch}.h`, `ee_core/{include,src}/ra*.{h,c}`, `smap-ingame` TX export, `system.c` module-table + `EECoreConfig_t` plumbing |
| 2 — hashing + `RA/` folder | **done** | `src/rahash.c`, `src/rawatch.c`, `include/rahash.h`, `include/rawatch.h`, `"RA"` in `sbCreateFolders` (`src/supportbase.c:1115`), `sbLoadWatchList` at `src/supportbase.c:1710` and called from all four legs |
| 3 — menu integration | **written, untested** | `ranet.c` (PC exchange), `rabadge.c` (badge cache on `MODE_COUNT` slots), `menusys.c` (the two game-menu actions + the four refusals), `opl.c` (badge refresh + badged name), `renderman.c` (`rmDrawInlayPixmap`), `themes.c` (`raIsCover`, `drawRAMark`), `textures.c` + `gfx/ra_mark.png` (`RA_MARK`), `gui.c` (notices + the settings rows), `dialogs.c` (rows + the About credit) |
| 4 — ps2ips | **dropped** | promoted to the default build on `rebuild/main`; see B2 |
| 5 — disc mode | **deferred** | maintainer decision, do not start |
| 6 — docs | **partial** | `docs/RETROACHIEVEMENTS.md`, `README.md` and `ROLLING_RELEASE.md` now describe the feature as written-but-untested rather than half-built; `CREDITS` and the About dialog carry hacan359. **Still open: the xeRAbora link in the docs-sync template, and the changelog / rolling notes.** |

Watch-list call sites, for reference (branch line numbers, pre-rebase):
`src/bdmsupport.c:2260`, `src/ethsupport.c:1000`, `src/hddsupport.c:2141`, `src/mmcesupport.c:1057`.

**Nothing has been hardware-tested.** That is still the whole open risk.

### ~~One gap inside "done" phase 1~~ — closed in `e62f6880`

`src/system.c` gated the in-game network stack on the watch list alone; the design doc (§4) calls for
the `#ifdef`, a runtime master switch, **and** a non-empty list. `gRATelemetry` now supplies the
middle term:

```c
if (gRATelemetry && GetWatchCount() > 0)
    modules |= CORE_IRX_ETH;
```

### Two traps this work turned up, worth keeping

1. **`internalDefault[TEXTURES_COUNT]` in `src/textures.c` is POSITIONAL.** Adding `RA_MARK` to
   `enum INTERNAL_TEXTURE` without adding its row grew the array bound past the initialiser, leaving
   the last slot `{0, NULL, NULL}` — so `texLookupInternalTexId()` ran `strcmp()` against a NULL
   name on every unmatched theme lookup, and the mark could never draw. It compiles silently either
   way. Guard the enum member and the table row with the same `#ifdef`, always.
2. **An `#ifdef` inside a run of `#define`s reflows the whole run.** clang-format aligns consecutive
   `#define`s as one group; a preprocessor conditional splits the group and re-aligns everything
   above it. The RA config keys therefore sit *after* the `CONFIG_OPL_*` block, not inside it.

---

## 2. Phase 3, step by step

This is the only substantial code left. The design doc's Phase 3 table is still the right file list;
what follows is the part that is easy to get wrong, plus everything that has changed under it.

### 2.1 Get the upstream reference

```
git clone --depth 4 --single-branch --branch ra https://github.com/hacan359/Open-PS2-Loader.git /tmp/ra-upstream
```

Reference only — port, never merge (upstream is +5994 lines vs *upstream OPL*, not vs this fork).

### 2.2 The wire protocol is specified. Do not reverse-engineer it.

`pc/RA/xerabora-0.1.0-alpha.4/protocol/PROTOCOL.md` on Nathan's machine is the contract, and
`pc/RA/` is **deliberately untracked — never commit it** (1.6 MB of someone else's binaries). The
client is xeRAbora 0.1.0-alpha.4, MIT, home `hacan359.github.io/xerabora`.

Everything is UDP, default port **18194**. The console's own IP and port travel *inside* every
request because the client replies to that address, not to the datagram source (it may be behind
NAT). The five messages `ranet.c` must implement:

| Direction | Message | Meaning |
|---|---|---|
| → PC | `RAP1 <console-ip> <port>` | discovery |
| ← PC | `RAO1 OK <client>/<version>` | discovery reply |
| → PC | `RAQ1 <hash> <serial> <console-ip> <port>` | identify game |
| ← PC | `RAA1 OK <bytes> <chunks> [<achievements> <title>]` / `RAA1 WAIT` / `RAA1 NO` | known / still asking, retry / unknown |
| → PC | `RAG1 <hash> <index> <console-ip> <port>` | fetch watch-list chunk |
| ← PC | `RAC1 <index> <length> <raw bytes>` | one chunk |
| ← PC | `RAU1 <achievement-id> <points>` | unlock notice, **unprompted** — keep the port open |

Replies are padded to a multiple of 64 and to at least 128 bytes, chunk payload 896, receive ceiling
960. Those constants are not arbitrary: they exist so every reply stays on the DMA path that
actually works. `RAB1`/`RAK2` (badge picture push) are an off-by-default experiment upstream and
**not part of the protocol** — do not port them.

`ra_watch.h` and `ra_snap.h` already in `modules/network/common/` were verified binary-identical to
the client's copies, so phase 1 is on-protocol and the same structs serve phase 3.

### 2.3 The NIC settlement is decided. Do not re-open it.

Maintainer decision, 2026-09-03 (design-doc open question #2, answered):

- RA's menu check is a **fourth claimant** on the NIC, subject to the existing interlock.
- Protocol = SMB and the stack is up → use it.
- Protocol = Off → RA may raise the stack itself through the normal `ethLoadInitModules()` path.
- Protocol = **UDPBD or UDPFS** → **refuse with a clear notice.** Do not double-drive the SMAP EMAC.
- In that refused case, fall back to **showing the hash / writing `RA/hashes.txt`** so xeRAbora can
  build the `.wl` on the PC and the user drops it into `<device>/RA/`.

Why this is safe rather than a compromise: the menu step's entire output is one small **file**, so a
refusal costs convenience, not capability. And the in-game half is unaffected either way — `raudp`
hand-builds frames over whatever SMAP the *launch* loaded.

`ethLoadModules()` (`src/ethsupport.c:296`) is the backstop and it still exists post-cleanup, along
with `bdmIsUDPBDLoaded()` (`src/bdmsupport.c:660`) and `udpfsGetModulesLoaded()`. RA must call
`ethLoadModules()` and honour its `-1` rather than reaching around it; double-driving is then
impossible by construction.

### 2.4 Refuse clearly on the three impossible legs

Only the four legs that reach `sysLaunchLoaderElf` can ever work: **BDM, ETH/SMB, HDD/APA, MMCE**.
Structurally impossible, because they exec an external ELF and `ee_core` never loads:

- **Neutrino core** — per-game `CONFIG_ITEM_CORE_LOADER` (`"$CoreLoader"`), falling back to
  `gDefaultCoreLoader` when the key is absent.
- **UDPFS** — `UDPFS_MODE` in `enum IO_MODES` (`include/iosupport.h`); it is Neutrino-only here.
- **PS1/VCD** — POPSTARTER or Ember; the row itself decides (the VCD views, and `isVcd` on a
  favourite record).

The menu action must **say why**, not silently do nothing and not hash something that will never be
watched. Also refuse on **HDD/APA**'s *hash* action specifically: HDLoader entries
(`hdl_game_info_t`) have no filename and no extension, so there is nothing to hash — watch lists
still *load* from `RA/`, but the check action must refuse rather than produce a wrong hash.

### 2.5 `RA_BADGE_SLOTS 4` is wrong here — use `MODE_COUNT`

Upstream assumes four devices. This fork has **14** (`MODE_COUNT`, `include/iosupport.h:25` — eight
BDM slots, ETH, HDD, APP, MMCE, FAV, UDPFS). With four slots the badge cache silently
mis-attributes lists across devices.

### 2.6 Settings

Two keys, both new and additive (absent key ⇒ default), through `config.c` like every other fork
setting. Do not reuse a key number.

| Setting | Default | Effect |
|---|---|---|
| RA telemetry | **Off** | master switch; Off ⇒ no `CORE_IRX_ETH` forcing, no `raudp`, no snapshot cost |
| RA badges in list | On, when telemetry is on | cosmetic only |

Add the rows to `diaNetConfig` / `guiShowNetConfig` per B3, and add the `&& gRATelemetry` term to
`src/system.c:781` in the same commit. No per-game override in v1 — the presence of a `.wl` already
scopes the feature per game.

### 2.7 Language labels

Every user-visible string goes through the pipeline: new `_STR_RA_*` labels **appended at the very
end** of `lng_tmpl/_base.yml` (`.lng` is consumed by line position — this rule has been broken three
times), translations in `lng_fork/<Lang>.yml`. Two YAML traps: bool coercion, and a `colon-space`
inside a value breaks the overlay — write `--` instead. Upstream ships hardcoded English
(`"RA: check game support"`); none of it may land as-is.

`lang_autogen.h` and `lang_internal.c` are **gitignored build artifacts** — do not hand-edit them
and do not commit them.

### 2.8 Credit, and the About dialog

`diaAbout` never scrolls and has roughly one row of headroom. Upstream adds six rows. **Append
"hacan359" to an existing credits line** instead, and put the full credit in `CREDITS`.

### 2.9 Two upstream things to skip

- **Main-menu `MENU_RA_DISC_LAUNCH` / `MENU_RA_DISC_CHECK`** and the magic `+ 2` separator
  arithmetic in `menuRenderMenu` — those belong to disc mode (phase 5, deferred). Do not import the
  `+ 2`.
- **The `LOG_ENABLE()` kill.** Upstream disables logging because `debugSetActive()` →
  `ethInitApplyConfig()` waits for link for 30 s ahead of `deferredInit`, so `gInitComplete` never
  gets set and the splash never ends. This fork's I/O worker and init ordering were rewritten since.
  **Check whether the hazard actually exists here before copying the workaround** — if it does not,
  importing a silent debug-logging kill is a straight regression.

---

## 3. Build and verify

Both flag states, both containers, `make clean` between. Recipe verified 2026-09-04:

```bash
MSYS_NO_PATHCONV=1 docker run --rm -v "C:/Users/natha/Github/Open-PS2-Loader:/repo" -w /repo \
  ps2dev/ps2dev:latest sh -c "apk add --no-cache make git python3 py3-yaml bash zip dos2unix 2>&1 | tail -2; \
  git config --global --add safe.directory '*'; dos2unix -q *.sh; \
  make download_lng download_lwNBD > /tmp/d.log 2>&1; echo DL=\$?; \
  make -j8 languages > /tmp/l.log 2>&1; echo LANG=\$?; \
  make -j8 opl.elf > /tmp/b.log 2>&1; echo MAKE_EXIT=\$?; \
  grep -nE '\*\*\*|: error' /tmp/b.log | head"
```

Then `git checkout -- '*.sh'` on the host to undo the container's CRLF strip.

Format check, matching CI exactly (CI pins clang-format **12**; the ps2dev image's own clang-format
is **22** and corrupts code that 12 accepts — never use it):

```bash
MSYS_NO_PATHCONV=1 docker run --rm -v "C:/Users/natha/Github/Open-PS2-Loader://work" -w //work \
  xianpengshen/clang-tools:12 sh -c 'clang-format --style=file <file> > /tmp/f.out; diff -u <file> /tmp/f.out'
```

**The acceptance gate is that the default ELF is unaffected**, and verifying it empirically has
already caught a real leak (upstream raised `ARP_TABLE_SIZE` 2→4 for every in-game build, not just
behind `INGAME_UDP`). How to verify: build both flavours from clean, then compare `nm -S`
(name+size per symbol), `ee_core/ee_core.elf` bytes, and each embedded `.irx` size. ⚠ Do **not**
compare raw `opl.elf` file sizes across two worktrees — the unstripped ELF bakes in object-file
paths, so a longer path alone shifts the size by ~200 bytes and looks exactly like a leak. Compare
`size` (text/data/bss) or the stripped image.

`make` does **not** track header dependencies. After any change to `EECoreConfig_t` or
`OPL_MODULE_ID`, `make clean` or delete `obj/` — an incremental build can pass locally while CI's
`make clean release` fails.

---

## 4. Traps

Carried forward from the design doc §5, plus what this pass found. Numbers 1-3 are new.

1. **`modules/network/ps2ips-ra/` is a duplicate.** See B2.
2. **The settings screen you want to edit may have been deleted.** See B3.
3. **`git show <rev>:.clang-format-ignore` fails under Git Bash** — MSYS rewrites the `rev:.dotfile`
   argument as a path list. Prefix with `MSYS_NO_PATHCONV=1`.
4. **`bdmsupport.c` contains NUL bytes** — `grep` calls it binary. Use `grep -a`.
5. **Use the Write tool for any file containing a literal backslash.** A quoted heredoc collapses
   escapes too; one collapsed `'\0'` has already shipped in `bdmsupport.c`.
6. **`MSG_DONTWAIT` is `0x40` in this tree**, not the design doc's `0x08`. Both
   `modules/network/common/smstcpip-common.h:283` and `SMSTCPIP/include/lwip/sockets.h:122` define
   it and `sockets.c` tests that value, so raudp and the stack agree by construction. The
   0x80-vs-lwIP mismatch trap is real but belongs to the **EE-side ps2ips** path.
7. **`lwip_recvfrom` takes 8 arguments**, not 6 — `SMSTCPIP/sockets.c:345` implements 8 and the
   6-arg declaration puts arguments in the wrong IOP registers.
8. **`from`/`fromlen` must always be passed** to `recvfrom` even when unused: the EE wrapper
   `memcpy`s into `from` unconditionally, so `NULL` writes to address zero.
9. **`g_rx` must stay 64-byte aligned.** Snapshot reads go through `UNCACHED_SEG`.
10. **Never wait on DMA inside the VBLANK handler** — skip the frame.
11. **Dead code upstream, do not port:** `ra_smap_result`, `ra_raudp_result` (written, never read)
    and `raGetStartupName` (defined, never called).
12. **Tester diagnostics go behind `__OPLDIAG`, never `__DEBUG`** — CI builds `make release`, so
    `__DEBUG` never reaches a tester. `raLaunchNote` is already re-keyed this way.
13. **Non-ETH launches still get a usable IP** — `set_ipconfig()` runs unconditionally in ee_core
    `main.c`. So RA telemetry from a USB boot depends on OPL's network settings holding a valid IP
    for the console. Worth a line in the user docs.
14. `exports.tab` / `imports.lst` are compiled as C with `IOP_CFLAGS`, so `#ifdef` works inside them
    — that is how the RA export/import additions stay out of the default build.

---

## 5. Definition of done

Phase 3 is finished when, on hardware:

| # | Test | Expected |
|---|---|---|
| 1 | Default build (`RETROACHIEVEMENTS=0`) | no regression on any existing path |
| 2 | RA build, telemetry Off | behaves as the default build |
| 3 | USB launch, valid `.wl`, telemetry On | ~60 snapshots/s for ≥5 min, skip/fail counters flat |
| 4 | Same, watching frame rate | no perceptible impact |
| 5 | SMB launch with telemetry | game's own SMB stream unaffected |
| 6 | Menu check, protocol = SMB | title + counts, `.wl` written |
| 7 | Menu check, protocol = Off | RA raises the stack, same result |
| 8 | Menu check, protocol = UDPBD / UDPFS | clean refusal + hash written to `RA/hashes.txt`, no wedge |
| 9 | Neutrino-core game | clean refusal, no hash attempt |
| 10 | PS1/VCD entry | clean refusal |
| 11 | HDD/APA entry | check action refuses; watch list still loads |
| 12 | Badge + cover mark | appear after refresh, correct game, correct device |
| 13 | Unlock overlay | gold pulse on `RAU1`, game keeps running |
| 14 | IGR combo in an RA launch | still resets |
| 15 | MX4SIO launch with telemetry | no navigation or art regression (this device has history) |

PCSX2 gets you: menu boots, RA entries render and navigate, flag-off build unchanged, no crash with
no `.wl`. That is the ceiling — GS raster timing and real SMAP behaviour are not testable there.

Hand testers a **run-pinned nightly.link build**, never a bare artifact link.

Docs to close out (phase 6): `docs/RETROACHIEVEMENTS.md` refresh, the RA variant row in `README.md`
(**not** the External Tools section — that block is generated by `.github/workflows/docs-sync.yml`;
edit the *template*), the xeRAbora link in the docs-sync template crediting hacan359, `CREDITS`,
changelog / rolling notes, and the variant filenames in `ROLLING_RELEASE.md`.

---

## 6. Do not do these

- Do not build a PC client. The author provides and maintains xeRAbora.
- Do not commit `pc/RA/`.
- Do not start disc mode (phase 5). Deferred by maintainer decision.
- Do not promote ps2ips again — already done on `rebuild/main`.
- Do not re-ask the four "open questions" at the end of the design doc. All four are answered:
  client delivered (xeRAbora, MIT); NIC = interlock claimant + hash fallback; disc mode deferred;
  ps2ips promoted to the default build.
- Do not import upstream's About-dialog rows, its `+ 2` menu arithmetic, or its `LOG_ENABLE()` kill
  without first checking the hazard exists here.
- Do not copy upstream's structure wholesale. `master` was abandoned because its architecture
  cornered this fork: copy behaviour, restructure freely, and change behaviour only with evidence.
