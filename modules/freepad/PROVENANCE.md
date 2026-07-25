# Vendored freepad.irx (pad driver, menu-side)

Vendored 2026-07-25 for the #254 pad-death fix. Installed over the container
stock freepad for the PS2MAXSDK and WOPLSDK flavours by
`.github/scripts/install_coherent_freepad.sh` (the PS2DEVLATESTSDK flavour
already ships these exact bytes natively).

## Bytes

| file | bytes | sha256 |
|---|---|---|
| freepad.irx | 36741 | 463fcb30cc4192dce7b4a0ffb8b24b47b3cb0057908c58e4542edebb91e6898e |

Module info: padman export ver 1.02, module version 3.6 (all three flavours'
stock freepad builds report the SAME versions -- only the bytes differ).

## Source

Extracted from the `ps2dev/ps2dev:latest` toolchain image (built 2026-07-17,
ps2sdk-ports build of ps2sdk's `iop/pad/freepad`). This is the build the
PS2DEVLATESTSDK flavour ships -- the only flavour where the GameID mmceman
boot load does NOT kill pad input (#254, PixeliGer: dead cursor on
WOPLSDK/PS2MAXSDK, working cursor on PS2DEVLATESTSDK, three console models,
original + third-party pads).

## Why vendored (hypothesis, hardware-verification pending)

With Game ID on (the default), mmceman loads right after GUI_INIT_DONE and its
sio2man hook installs onto the bus freepad also uses. On the two older
containers that takes pad input down; on latest it does not. sio2man is
byte-identical between latest and the ghcr pin (and version-identical on
ps2max), mmceman is now byte-identical on all three -- freepad is the
remaining SIO2 delta. Swapping it tests the clash directly: if pads survive
the GameID load with these bytes, the freepad build was the vulnerable side.

freepad is menu-side only (games bring their own pad libraries), so the swap
cannot affect in-game input. sio2man compatibility: ghcr's sio2man.irx is
byte-identical to latest's, so this build is compatible by construction there;
ps2max's sio2man differs in bytes but reports the same export table
(2.07/1.02, 68/59 ordinals).

## Re-vendor

Re-extract from a fresh `ps2dev/ps2dev:latest` pull
(`docker run --rm -v $PWD:/out ps2dev/ps2dev:latest cp /usr/local/ps2dev/ps2sdk/iop/irx/freepad.irx /out/`),
recompute the sha256, update the table.
