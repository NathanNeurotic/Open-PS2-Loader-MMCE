# MMCE Phase A Parity, Provenance & Validation Record

This document records the audited provenance, git-level diff verification, binary checksums, and hardware validation protocol for Phase A MMCE transplantation into `rebuild/main` (branch `rebuild/step-216-mmce-support`).

---

## 1. Verification Status Summary

| Property | Evidence | Status |
|---|---|---|
| **Source Transplantation** | Audited Git diff vs `master` (0 diff on 1:1 files) | **Verified** |
| **Release Compilation** | Docker build (`make clean && make -j4 release`) | **Verified** (Generated `RIPTOPL.ELF`) |
| **Debug Compilation** | Docker build (`make clean && make -j4 debug`) | **Verified** |
| **Binary Provenance** | SHA-256 hashes against SDK & ps2-mmce commit | **Verified** |
| **Master Behavioral Equivalence** | Awaiting runtime tests on console | **Unverified (Pending Hardware)** |
| **Real MMCE Hardware Detection** | Awaiting physical PS2 test | **Unverified (Pending Hardware)** |
| **VMC Mounting & In-Game Saves** | Awaiting physical PS2 test | **Unverified (Pending Hardware)** |
| **GameID Push & Folder Switch** | Awaiting physical PS2 test | **Unverified (Pending Hardware)** |
| **IGR Bootcard Recovery** | Awaiting physical PS2 test | **Unverified (Pending Hardware)** |
| **MX4SIO Cross-Device Settling** | Awaiting physical PS2 test | **Unverified (Pending Hardware)** |

> [!IMPORTANT]
> **No hardware parity claim has been established yet.** Phase A status is strictly:
> **Phase A Implementation Complete / Build-Verified / Awaiting Hardware Parity Validation**.
> Phase B refactoring (`mmce_state_t`) is **FROZEN** until physical hardware validation of this baseline passes.

---

## 2. 1:1 Component Git Diff Audit (Zero Differences vs `master`)

The following files were diffed directly against `master` via `git diff master -- <file>` and confirmed to have **zero differences** (bit-for-bit source parity):

```bash
git diff master -- \
  src/mmcesupport.c \
  include/mmcesupport.h \
  modules/iopcore/cdvdman/device-mmce.c \
  modules/mcemu/device-mmce.c \
  modules/iopcore/common/mmcedrv_config.h \
  .github/patches/mmceman-fs-close-fd-leak.patch \
  .github/patches/mmceman-fs-open-enoent.patch \
  .github/patches/mmceman-fs-dopen-enoent.patch \
  .github/scripts/install_coherent_mmce.sh
# Result: 0 diff lines (clean exit 0)
```

---

## 3. Provenance & Taxonomy Matrix

| Subsystem | Source (`master`) | Destination (`rebuild/main`) | Taxonomy Classification | Scope of Adaptation / Rationale |
|---|---|---|---|---|
| **MMCE Support Impl** | `src/mmcesupport.c` | `src/mmcesupport.c` | **Copied unchanged from master** | Exact source parity; zero diff vs `master:src/mmcesupport.c`. |
| **MMCE Support Header** | `include/mmcesupport.h` | `include/mmcesupport.h` | **Copied unchanged from master** | Exact source parity; zero diff vs `master:include/mmcesupport.h`. |
| **CDVDMAN Device** | `modules/iopcore/cdvdman/device-mmce.c` | `modules/iopcore/cdvdman/device-mmce.c` | **Copied unchanged from master** | In-game CDVD device backend and offset exports. |
| **MCEMU Device** | `modules/mcemu/device-mmce.c` | `modules/mcemu/device-mmce.c` | **Copied unchanged from master** | In-game VMC backend. |
| **Driver Config Header** | `modules/iopcore/common/mmcedrv_config.h` | `modules/iopcore/common/mmcedrv_config.h` | **Copied unchanged from master** | MMCE driver configuration structs. |
| **CI Installer Script** | `.github/scripts/install_coherent_mmce.sh` | `.github/scripts/install_coherent_mmce.sh` | **Copied unchanged from master** | Builds menu-coherent `mmceman.irx` from pin `db3e93f0` + 3 patches; keeps stock in-game drivers. |
| **Driver Patches** | `.github/patches/*.patch` | `.github/patches/*.patch` | **Copied unchanged from master** | 3 patches: `close-fd-leak`, `open-enoent`, `dopen-enoent`. |
| **CDVDMAN Config Header** | `modules/iopcore/common/cdvd_config.h` | `modules/iopcore/common/cdvd_config.h` | **Copied + branch adaptation required** | Added `struct cdvdman_settings_mmce` matching master. |
| **CDVDMAN Internal Header**| `modules/iopcore/cdvdman/internal.h` | `modules/iopcore/cdvdman/internal.h` | **Copied + branch adaptation required** | Added `#elif MMCE_DRIVER` branch defining `CDVDMAN_SETTINGS_TYPE`. |
| **CDVDMAN Exports** | `modules/iopcore/cdvdman/exports.tab` | `modules/iopcore/cdvdman/exports.tab` | **Copied + branch adaptation required** | Added `mmce_read_offset` and `mmce_write_offset` exports. |
| **CDVDMAN Makefile** | `modules/iopcore/cdvdman/Makefile` | `modules/iopcore/cdvdman/Makefile` | **Copied + branch adaptation required** | Added `ifeq ($(USE_MMCE),1)` rule targeting `mmce_cdvdman.irx`. |
| **MCEMU Imports** | `modules/mcemu/imports.lst` | `modules/mcemu/imports.lst` | **Copied + branch adaptation required** | Added `mmcedrv_IMPORTS_start` block. |
| **MCEMU Header** | `modules/mcemu/mcemu.h` | `modules/mcemu/mcemu.h` | **Copied + branch adaptation required** | Added `int fd` to `McImageSpec`. |
| **MCEMU Utils Header** | `modules/mcemu/mcemu_utils.h` | `modules/mcemu/mcemu_utils.h` | **Copied + branch adaptation required** | Added `DECLARE_MCEMU_DEVICE_MMCE` macros. |
| **MCEMU Makefile** | `modules/mcemu/Makefile` | `modules/mcemu/Makefile` | **Copied + branch adaptation required** | Added `ifeq ($(USE_MMCE),1)` rule targeting `mmce_mcemu.irx`. |
| **EE Core Header** | `ee_core/include/ee_core.h` | `ee_core/include/ee_core.h` | **Copied + branch adaptation required** | Added `MMCE_MODE` to `enum GAME_MODE`. |
| **EE Core Module IDs** | `ee_core/include/modules.h` | `ee_core/include/modules.h` | **Copied + branch adaptation required** | Added `OPL_MODULE_ID_MMCEDRV` and `OPL_MODULE_ID_MMCEIGR`. |
| **EE Core IOP Mgr** | `ee_core/src/iopmgr.c` | `ee_core/src/iopmgr.c` | **Copied + branch adaptation required** | Added `case MMCE_MODE:` loading `OPL_MODULE_ID_MMCEDRV`. |
| **EE Core Main** | `ee_core/src/main.c` | `ee_core/src/main.c` | **Copied + branch adaptation required** | Added `"MMCE_MODE"` string matching in `eecoreInit()`. |
| **EE Core PadHook** | `ee_core/src/padhook.c` | `ee_core/src/padhook.c` | **Copied + branch adaptation required** | Added MMCE bootcard reset trigger load in `IGR_Thread()`. |
| **EE Extern IRX** | `include/extern_irx.h` | `include/extern_irx.h` | **Copied + branch adaptation required** | Added `IMPORT_BIN2C` declarations for MMCE IRX binaries. |
| **EE OPL Header** | `include/opl.h` | `include/opl.h` | **Copied + branch adaptation required** | Added MMCE global variables and extern declarations. |
| **EE GUI Header** | `include/gui.h` | `include/gui.h` | **Copied + branch adaptation required** | Added MMCE config dialog function declarations. |
| **EE OPL Core** | `src/opl.c` | `src/opl.c` | **Copied + branch adaptation required** | Config load/save, `0/0` -> `5/1` pacing migration, `deferredInit` GameID arming. |
| **EE GUI Core** | `src/gui.c` | `src/gui.c` | **Copied + branch adaptation required** | `CFG_MMCEMODE`, MMCE settings dialogs, timeout guards. |
| **EE Menusys Core** | `src/menusys.c` | `src/menusys.c` | **Copied + branch adaptation required** | `MENU_MMCE` ID, main menu registration, `gMMCEStartMode` escape check (12 lines diff). |
| **EE System Core** | `src/system.c` | `src/system.c` | **Copied + branch adaptation required** | Included `mmcesupport.h`, added `CORE_IRX_MMCE`, `sendIrxKernelRAM`, IGR config, `sysLaunchDisc` GameID push. |
| **Language Templates** | `lng_tmpl/_base.yml` | `lng_tmpl/_base.yml` | **Unrelated conflict resolution** | Cleanly appended 17 missing master entries (+34 lines, 0 deletions), zero formatting churn on existing keys. |
| **Top Makefile** | `Makefile` | `Makefile` | **Copied + branch adaptation required** | Added `mmcesupport.o`, MMCE IRXs, bin2c rules, and clean targets. |

---

## 4. Binary Driver Provenance & Reproducibility Record

| Binary | Origin / Commit / Toolchain | Size | SHA-256 Hash |
|---|---|---|---|
| **`mmcedrv.irx`** | PS2SDK stock prebuilt (`$PS2SDK/iop/irx/mmcedrv.irx`) in `codex-opl-mx4-build:20260802` | 11,337 bytes | `d9e0d476e95e1b952ddd7d11d866b77cbec0e78d6c3ba9f739de1b510a8cd517` |
| **`mmceigr.irx`** | PS2SDK stock prebuilt (`$PS2SDK/iop/irx/mmceigr.irx`) in `codex-opl-mx4-build:20260802` | 1,369 bytes | `981323e168a1d91ba09d3021579eec6d9fc2c7b84340a1cf014dd2dd7621533c` |
| **`mmceman.irx`** | Built from `ps2homebrew/ps2-mmce` pin `db3e93f019f2430d8eb875225439c288591ef5ae` + 3 CI patches (`close-fd-leak`, `open-enoent`, `dopen-enoent`) with `mipsel-none-elf-gcc (GCC) 14.2.0` in `codex-opl-mx4-build:20260802` | 17,049 bytes | `c63bcd2979b50e5fcf7b8e4f9ab9d7474f916d8c86f130d8794462a1840c5686` |

---

## 5. Physical PS2 Hardware Validation Protocol

To confirm baseline operation before unfreezing Phase B, test execution must follow this structured protocol:

### Step 1: Detection & Bus Pacing

- [ ] **Slot 1 Cold Boot**: Boot console with MMCE card inserted in Slot 1; verify successful detection.
- [ ] **Slot 2 Cold Boot**: Boot console with MMCE card inserted in Slot 2; verify detection and slot selection.
- [ ] **Hot Removal/Reinsertion**: Remove card at main menu, confirm graceful notification; reinsert and confirm 3×200ms debounce redetects card.

### Step 2: Game Browsing & Stress Test

- [ ] **List Population**: Verify PS2 games (`CD/` and `DVD/`) and PS1 games (`POPS/*.VCD`) populate cleanly.
- [ ] **Subfolder Navigation**: Browse subfolders inside game directories.
- [ ] **Cover Artwork Hits & Misses**: Verify loading covers for games with existing artwork, and verify non-blocking handling for games without artwork.
- [ ] **Browsing Stress Baseline**: Perform 1,000+ cursor movements across game lists with mixed existing/missing artwork; record responsiveness, cover load latency, and verify zero UI stalls or controller starvation.

### Step 3: Filesystem Switching & VMC

- [ ] **GameID Push & Launch**: Launch a GameID-managed game; confirm MMCE switches to the game's per-game directory.
- [ ] **VMC Mounting & In-Game Saves**: Create/mount a VMC on MMCE; confirm the game successfully saves and loads progress.

### Step 4: In-Game Reset (IGR)

- [ ] **Bootcard Restoration**: Trigger IGR from within a game; confirm MMCE receives the reset command and restores access to the bootcard rather than remaining pinned on the game folder.

### Step 5: MX4SIO Coexistence

- [ ] **Cross-Device Launch**: With MMCE inserted in Slot 1 and MX4SIO in Slot 2 (or vice versa), launch a game from MX4SIO; verify the 5,000ms cross-device settle gate operates cleanly during post-reset SD enumeration.

### Step 6: Failure & Recovery

- [ ] **Card Removal Mid-IO**: Remove the MMCE card during active cover browsing; reinsert and verify menu recovers without requiring a console power cycle.

