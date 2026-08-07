# BDMAssault variant driver pairs — embedded provenance

These are the POPSTARTER BDMA driver variant pairs OPL embeds (gzipped) and pastes to
`mc?:/POPSTARTER/{usbd.irx, usbhdfsd.irx}` when the launch-time equip finds no user-supplied copy on
any device (maintainer directive 2026-07-21: "POPSLoader has modules embedded in the elf, and then
pastes them according to the VCD device if needed"). User-supplied files on the seek path
(Custom POPSTARTER dir → boot device POPS/ → game device POPS/) always win; embedded is the final
fallback — the same external-beats-embedded order POPSLoader ships.

## Bytes (vendored 2026-07-21, sha256)

| file | bytes | sha256 |
|---|---|---|
| usbd.irx.usbexfat | 48500 | 5ea4818ba1cf5207f6d7cadb4c13b5afa88c37c260750c21155b079a8c18f369 |
| usbhdfsd.irx.usbexfat | 34508 | 70a369e47745e7f995fa771edbbfed086727b759fba915a7070c0fbcf061dd34 |
| usbhdfsd.irx.mx4sio | 14993 | 61525fab3b340aaef467eb3a08ce16e35aca98fe742982a7a151c5c7e1ffa00a |
| usbd.irx.mmce | 11841 | 0fed25d9eecdd263d4617ad5c8eeb2c5307c799aa97962b62b40a0e63e3adac8 |
| usbhdfsd.irx.mmce | 19733 | 9f7b77848935f0d8e9bd792b997910777352f8d480dc32d99a04e6e8653779ab |
| usbd.irx.ata | 42749 | 4e1d39365854747117a08f8a8d843937614590ea072cf2b70f6e4c2ebbf3c6ee |
| usbhdfsd.irx.ata | 21837 | 1cc865cd09997bd708e9e102c0f0f1f17f352b7844b282fcfa6a00f38609db9c |

**Dedupe (verified by sha256):** `usbd.irx.mx4sio` is byte-identical to `usbd.irx.usbexfat`, so it
is NOT vendored twice — the mx4sio mode reuses the usbexfat usbd blob (one embedded symbol).

## Sources

- **usbexfat + mx4sio + mmce pairs**: checked-in binaries of saildot4k/POPSLoader at commit
  `ed2d5db59ded6bf014c64231ea6db3fa54689a36` (bin/POPSLDR/*), fetched via
  raw.githubusercontent.com. POPSLoader also vendors the full MX4SIO build source
  (iop/embed/BDMASSAULT_MX4SIO/ + SourceCode tarball, AFL v2.0).
- **usbexfat upstream project**: israpps/BDMAssault (AFL v2.0; its README documents pasting the
  pair into mc?:/POPSTARTER/). It publishes NO GitHub releases — the POPSLoader-checked-in copies
  are the canonical distributed bytes.
- **ata pair**: saildot4k/ATA-Assault release tag `latest` (published 2026-06-22; 42749 + 21837 B).
  WARNING: that tag is MUTABLE, which is exactly why the bytes are vendored here with sha256s.
- **mmce pair caveat**: exact upstream build source unpinned (mmceman-family; distributed via
  POPSLoader). Confirm lineage with saildot4k when convenient.

## License

All three source projects carry the Academic Free License v2.0 (LICENSE in this directory is
israpps/BDMAssault's copy) — the same license family as OPL itself (AFL v3.0). POPSLoader (GPL-3)
shipping these identical blobs embedded is direct redistribution precedent.

## Re-vendor

Fetch from the pinned POPSLoader commit / a NEW ATA-Assault release, recompute sha256, update the
table above, and re-verify the usbd usbexfat==mx4sio dedupe still holds (if it stops holding, add
the mx4sio usbd as an eighth file AND a new embedded symbol + table row in src/vcdsupport.c).
