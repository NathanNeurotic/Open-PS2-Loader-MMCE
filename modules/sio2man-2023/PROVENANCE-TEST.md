# modules/sio2man-2023 -- DIAGNOSTIC blob, never to be merged

`freesio2-2023.irx` (5428 bytes, sha256
`df200af667a3584a54da5096670ec5c347d3f787a5f47f133a0c29d08ac5eaa9`) is the PS2SDK `sio2man`
extracted VERBATIM from `ghcr.io/ps2dev/ps2dev@sha256:362bcd26b8bd94149c539f41749edb13facbb5b68a8b960e219fc079693cb95f`
-- the exact container digest uOPL's CI pins, image created **2023-09-09**, file dated
2023-09-04 inside the image. This is the PRE-rewrite module: its import table carries
`thbase` + `thevent` (dedicated priority-0x18 worker thread driven by event flags) and no
`thsemap`, unlike the post-PR#709 threadless build.

## Purpose (issue #340)

Confirmation experiment requested by KrahJohlito: if the D-pad drop/queue issue disappears
with ONLY this module swapped (everything else stock), the ps2sdk threadless sio2man rewrite
(PR #709, Jan 2025) is confirmed as the trigger on real hardware. This build exists to
produce that pass/fail datum -- it is NOT a fix.

## Expected breakage -- do not report as bugs of this build

- MMCE menu features (mmceman hooks the modern sio2man internals).
- MX4SIO menu browsing (post-PR#862 mx4sio_bd requires the modern sio2man).
- MX4SIO game launches (the same embed is loaded game-side for CORE_IRX_MX4SIO).

Test on a USB-booted, USB-game setup only.

## The actual fix candidate

`feat/340-sio2man-priority-ceiling` -- the modern module rebuilt from pinned source with a
priority-ceiling bracket (same API, no feature breakage). If THIS diagnostic build fixes the
D-pad and the ceiling build does too, ship the ceiling; if this fixes it and the ceiling does
not, the ceiling theory is wrong and the delta between the two modules needs a deeper look.
