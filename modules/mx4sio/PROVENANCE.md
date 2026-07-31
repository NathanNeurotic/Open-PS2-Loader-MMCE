# MX4SIO drivers with restored ROM-sio2man support — embedded provenance

These are the `mx4sio_bd.irx` / `mx4sio_bd_mini.irx` binaries OPL embeds (Makefile:
`$(EE_ASM_DIR)mx4sio_bd.c` rules) in place of the SDK containers' stock copies. They are
**current** ps2sdk master plus a fork-carried patch — NOT a pin of the old driver.

## Why (issue #317, reporter SCPH-77008 / DECKARD)

ps2sdk commit `b55d76425479696a9632e24b62c0348e90b304ea` (PR ps2dev/ps2sdk#862, merged
2026-06-16, "Changes to mx4sio for ps2sdk sio2man client updates") rewrote mx4sio's
runtime sio2man hook around a "single semaphore" lock model and restricted it to
library versions 1.2 (rom0:XSIO2MAN) and 2.7 (ps2sdk's own sio2man). Every other
sio2man — notably the **rom1:SIO2MAN v2.x that games load on DECKARD consoles**
(SCPH-75000 and later) — is rejected with "ERROR: sio2man version not supported"
(compiled out in the mini driver, so the failure is silent). With no hook installed,
SD transfers run without SIO2 bus arbitration against pad/MC traffic and the card
wedges on the game's first reads: activity LED never blinks, black screen.

Menu-side browsing is unaffected (OPL pairs mx4sio with the SDK's freesio2 = 2.7,
which IS accepted), which is why the failure only appears at game launch. The last
working builds embedded pre-2026-06-16 mx4sio (old hook accepted all versions > 1.1).
Same upstream regression: ps2homebrew/Open-PS2-Loader#1731 (unfixed as of 2026-07-31).

## The fix (0001-mx4sio_bd-restore-rom-sio2man-hook-support.patch)

Base: ps2dev/ps2sdk master `e228ff7b61a12ad1192a49338754534362a26e58` (2026-07-31).
Three files under `iop/sio/mx4sio_bd/src/`:

- `sio2man_hook.c` — any sio2man version > 1.1 that is not 1.2/2.7 now gets the
  **proven pre-2026-06 hook model** (the driver's own semaphore pair wrapping every
  transfer_init / transfer / transfer_reset export, v1.x layout 49/50 vs v2.x layout
  49-52) instead of a hard rejection. The new 1.2/2.7 path is untouched. Two
  deliberate deviations from the old model: (1) no export-23/26 call while hooking —
  the new architecture hooks with interrupts suspended, where a blocking sio2man call
  can deadlock; the semaphore wrappers arbitrate from the moment they are installed;
  (2) `sio2man_hook_sio2_lock/unlock()` fall back to the v1.x library when only one
  is resident (upstream left that as a silent no-op = zero arbitration on
  v1.x-only systems).
- `ioplib.c` / `ioplib.h` — restore `ioplib_hookExportEntry()` (single-entry hook;
  PR #862 kept only `ioplib_hookSameExportEntries`, which the legacy model cannot
  use: hooking several entries that may alias one function needs per-entry
  original-pointer storage).

License: Academic Free License v2.0 (ps2sdk / Maximus32's MX4SIO driver), same as the
embedded sources it derives from. Upstream is welcome to take the patch.

## Bytes (built 2026-07-31, sha256)

| file | bytes | sha256 |
|---|---|---|
| mx4sio_bd.irx      | 14401 | 35fa32e90127466492f6253affc663df1a64a9ac688f6023d568d62c0a3b4555 |
| mx4sio_bd_mini.irx | 13329 | 94c1d6dee9e8e96b66a80068dfad685215d5c05b480a2bde54864e1d971e4e72 |

Stock (BROKEN) bytes in `ps2dev/ps2dev:latest` @ 2026-07-26, for comparison:

| file | bytes | sha256 |
|---|---|---|
| mx4sio_bd.irx      | 11841 | 761972f0154e9fcf4fde2b7feb69a923bee53f8592fcc5553027be32f1c4a991 |
| mx4sio_bd_mini.irx | 10769 | c22c17ce7c6f302903eedaf568db22e215bca0ab68ad9a7758bd500ec587a45e |

## Rebuild / re-vendor

Run `.github/scripts/build_vendored_mx4sio.sh` (host needs docker; uses the
`ps2dev/ps2dev:latest` toolchain image). It clones ps2sdk at the pinned base commit,
applies the patch from this directory, builds both variants, verifies the sha256s
above, and copies the IRXs here. To re-base the patch onto a newer ps2sdk: clone,
`git checkout <new-base>`, `git apply` the patch (resolve conflicts), rebuild, then
update the base commit, the patch, and both tables above.

## Retirement condition

If upstream ps2sdk restores ROM-sio2man support in the hook (watch
ps2dev/ps2sdk `iop/sio/mx4sio_bd/src/sio2man_hook.c` and Open-PS2-Loader#1731), drop
this directory and point the Makefile rules back at `$(PS2SDK)/iop/irx/mx4sio_bd*.irx`.
