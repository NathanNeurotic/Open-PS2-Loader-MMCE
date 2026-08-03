# modules/sio2man -- locally patched SIO2 manager

## What this is

The PS2SDK `sio2man` module, built from source in-tree with **one local patch**: a
priority-ceiling bracket around the SIO2 transaction lock. The build replaces the SDK's prebuilt
`$(PS2SDK)/iop/irx/freesio2.irx` as the `sio2man` blob embedded into OPL. The menu always loads
it; game-side it reaches the rebooted IOP **only on MX4SIO launches** (`CORE_IRX_MX4SIO` places
it ahead of `mx4sio_bd`) -- other launch paths never load OPL's sio2man into the game IOP.

## Source provenance

- Upstream: `ps2dev/ps2sdk`, path `iop/sio/sio2man/`
- Pinned commit: `f08e889fef8ab361f863c44ebe78212ced2839ca` (master, 2026-08-02)
- Files taken verbatim: `src/exports.tab` (unchanged), `src/sio2man.c`, `src/imports.lst`,
  `src/irx_imports.h` (the latter three carry the patch below). `src/log.c`/`log.h` are omitted:
  they are only referenced under `SIO2LOG`, which this build does not define.
- Upstream style (tabs) is preserved so `diff` against ps2sdk stays a small reviewable patch;
  the directory is excluded from the repo-wide clang-format CI check for that reason
  (`.github/workflows/check-format.yml`), same precedent as `modules/network/smb2man/libsmb2`.

## Why (issue #340)

ps2sdk PR #709 (merged 2025-01-07, commit `c02b4b3099`) removed sio2man's dedicated
priority-24 (0x18) worker thread. Since then a SIO2 transaction -- the whole
`sio2_*_transfer_init` .. `sio2_transfer_reset` bracket -- executes every bus phase in the
**calling client's thread** while holding `m_transfer_semaphore`, a plain IOP semaphore with no
priority inheritance. Consequence on real hardware: a low-priority client (mcserv's RPC thread
runs at 0x68 = 104) holding the lock mid-transaction is preempted by usbd (0x1E/0x24 = 30/36),
and freepad's priority-20 vblank pad poll blocks in `WaitSema` past its vblank window. The EE
sees that as `padRead()` misses; OPL's menu drops and queues D-pad input under USB load
(issue #340; the same failure class ps2sdk PR #898 acknowledges: "inputs not working and hang
on reading"). PCSX2 never reproduces it because emulated SIO2 transfers complete in ~zero
virtual time, so the lock-hold window collapses.

Old (~2023) builds -- e.g. the uOPL-era binaries that stay smooth on the same console -- ran
every bus phase on the module's own priority-0x18 thread, which outranks usbd, so the window
did not exist.

## The patch

`sio2_prio_ceiling_acquire()` after the `WaitSema` in `sio2_pad_transfer_init` (the single
acquire point every client variant routes through, per `exports.tab` aliasing) boosts the
calling thread to priority 0x18 for the life of the transaction, saving its previous priority;
`sio2_prio_ceiling_release()` before the `SignalSema` in `sio2_transfer_reset` restores it.
Threads already at or above 0x18 (freepad's transfer thread at 20) are left untouched. While a
holder blocks awaiting the SIO2 interrupt it yields the CPU exactly as stock; only bus-phase
code runs boosted -- the same scheduling the retired worker thread provided, with the new
module's API surface (mmceman's sio2man hook and the post-PR#862 mx4sio driver keep working).
The release restores only when called by the acquiring thread and runs before the `SignalSema`
(order is load-bearing); both choices confine a legacy SDK-1.3 double-reset -- a stock-identical
semaphore-pumping exposure -- to the lock breakage stock already has, with no priority leak.

## Revert / resync path

- To drop the patch: point the `$(EE_ASM_DIR)sio2man.c` rule in the top-level Makefile back at
  `$(PS2SDK)/iop/irx/freesio2.irx` and delete this directory (plus the check-format exclude).
- To resync with upstream: re-fetch the four files at a new pinned commit, re-apply the three
  hunks (ceiling block after `g_sio2man_data`, acquire call, release call, plus the
  thbase import additions in `imports.lst`/`irx_imports.h`), and update the pin above.
