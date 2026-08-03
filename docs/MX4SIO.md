# MX4SIO game-launch module contract

MX4SIO has two separate runtime paths in OPL: the menu path that lists games and the
post-reset game path that reads the selected disc image. A working game list therefore
does not prove that the modules required after game launch were supplied to the new IOP.

## Why launches regressed

[ps2sdk PR #862](https://github.com/ps2dev/ps2sdk/pull/862) changed `mx4sio_bd` to rely
on PS2SDK's `sio2man` implementation. OPL's menu already loaded that implementation from
`freesio2.irx`, so browsing an MX4SIO card continued to work. At game launch, however,
OPL reset the IOP and originally copied and loaded only `mx4sio_bd`; the required
`freesio2` module was no longer present.

This explains the shared failure pattern in
[RiptOPL issue #317](https://github.com/NathanNeurotic/Open-PS2-Loader/issues/317) and
[upstream OPL issue #1731](https://github.com/ps2homebrew/Open-PS2-Loader/issues/1731):
the card mounts and games are listed, but launching a game stalls before MX4SIO reads
resume. The SCPH-50004 report in
[upstream issue #1750](https://github.com/ps2homebrew/Open-PS2-Loader/issues/1750)
also shows why this is not adequately explained as a DECKARD-only ROM-module problem.

The broader failures collected in
[ps2sdk issue #892](https://github.com/ps2dev/ps2sdk/issues/892) are useful regression
context, but they do not identify the MX4SIO launch cause. The direct evidence is the new
dependency declared by PR #862 and its absence from OPL's post-reset game module set.

## Required implementation

The minimal dependency fix is:

1. Give the embedded `freesio2.irx` image its own `OPL_MODULE_ID_SIO2MAN` identifier.
2. For `CORE_IRX_MX4SIO`, place `sio2man_irx` immediately before `mx4sio_bd_irx` in
   the module table copied to kernel RAM.
3. In the MX4SIO game-core path, load `OPL_MODULE_ID_SIO2MAN` first and load
   `OPL_MODULE_ID_MX4SIOBD` only when the first load returns a positive module ID.
4. Keep the embedded `sio2man` and `mx4sio_bd{,_mini}.irx` on the same PS2SDK interface
   generation so they cannot drift apart. (Since the #340 fix, the embedded `sio2man` is no
   longer the SDK's prebuilt `freesio2.irx`: it is built in-tree from pinned ps2sdk source with
   a priority-ceiling patch -- same export surface and interface generation; see
   `modules/sio2man/PROVENANCE.md`.)

The positive-result guard is intentional. `LoadOPLModule()` returns the loaded IOP module
ID on success; zero does not prove that a module was loaded. Starting `mx4sio_bd` after a
zero or negative dependency result would recreate the unsupported launch state.

This works because the game IOP now receives the exact SIO2 implementation required by
the current SDK driver, in dependency order, rather than asking the driver to adapt to an
arbitrary ROM `sio2man`. A previous downstream workaround vendored a patched driver that
restored legacy ROM-hook behavior, but the reporter still stalled on the resulting build.
Keeping that unconfirmed driver would add a second variable and hide whether the declared
PS2SDK dependency was actually satisfied, so OPL uses the coordinated SDK pair instead.

## Verification boundary

Source inspection can prove that both module images are embedded, copied, and loaded in
the required order. The normal debug and release builds prove that the module IDs and
tables remain internally consistent. Neither replaces a real-console launch test.

For a controlled hardware check:

1. Use the same console, adapter, SD card, filesystem, and game for the before/after builds.
2. Disable PADEMU and VMC for the first pass so they do not add SIO2 traffic or extra modules.
3. Confirm that the MX4SIO activity LED resumes after the green debug stage and that the
   game reaches its first screen.
4. Verify controller input and a real memory card, then repeat with any normally used VMC
   or PADEMU options separately.
5. Test the SCPH-77008 report case and, if available, prefer an SCPH-50004 as the
   non-DECKARD control. Record other SCPH-50000-series models separately.

Until that A/B test is reported, this is a source- and build-validated fix with hardware
confirmation still pending.
