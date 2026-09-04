# Migrating Open-PS2-Loader to the new PS2 SDK

Handoff notes for whoever ports OPL's own build. **Rewritten 2026-09-03
(v2)** after `sdk-world v2026.09.03.2` actually landed and every claim
below was re-verified by direct inspection of the installed
`$PS2DEV/packages/{core,world}` tree on this machine — supersedes the
earlier same-day version of this document. **This document covers the SDK
side only** — OPL's own Makefile→`ps2build` conversion is underway
in-repo (see "Porting OPL's own build" below).

## The short version

This project replaced classic ps2sdk's flat `$PS2SDK/{ee,iop}/{include,lib}`
tree with a per-package layout: `$PS2DEV/packages/{core,world}/<name>/`,
each with its own `include/`, `lib/` and/or `bin/*.irx`, built by
`ps2build` (git.techwritescode.dev/ps2/ps2build) from declarative
`ps2.yaml`/`package.yaml` files instead of ps2sdk's old Makefiles.

OPL's own Makefile expects the classic flat tree. **Nothing converts one
layout into the other, and nothing should** — the standing decision is
that OPL's own build gets converted to this SDK's real layout, not the
other way around (no flat-tree compat generator). Concretely: **OPL's
build becomes a real `ps2.yaml`/`ps2workspace.yaml` project**, consuming
`packages/{core,world}/<name>/{include,lib}` directly through ps2build's
own dependency resolution (`libs:`/`headers:` by package name).

## Current installed state (verified 2026-09-03)

`ps2build update` is sufficient for everything except three standalone
packages (next section). Current versions on this machine:

```
toolchain  v2026.09.03
ps2build   v2026.09.03.3
bin2c      v2026.09.03
sdk-core   v2026.09.03.1
sdk-world  v2026.09.03.2
```

`sdk-world v2026.09.03.2` is the release that folds `png`, `z`,
`freetype`, `jpeg`, `smbman`, and the `mmceman`/`mmcedrv`/`mmceigr`
family into sdk-world itself. If a fresh machine shows an older
sdk-world, just run `ps2build update` again — the earlier `.1` tag was
cut before a CI fix landed and never produced a working release; `.2` is
the real one.

## Standalone packages: installed 2026-09-03, with one real upstream bug

`audsrv`, `ogg`, and `vorbis` are standalone repos (release zips into
`$PS2DEV/packages/world/<name>/`, per
https://ps2.techwritescode.dev/guide/custom-packages). All three are now
installed here — but **the ogg/vorbis release zips are broken upstream**:
their CI workflow never passes
`-DCMAKE_TOOLCHAIN_FILE=$PS2DEV/share/ps2dev.cmake`, so the shipped
`lib/ogg.lib` / `lib/vorbis*.lib` are **x86-COFF objects built with the
host MSVC compiler**, not EE binaries (and misnamed vs. their own
package.yaml's `lib/lib*.a`). `audsrv`'s zip was fine (real
`lib/libaudsrv.a` + `bin/audsrv.irx`).

Worked around locally by rebuilding ogg and vorbis from source
(`cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PS2DEV/share/ps2dev.cmake`,
vorbis configured with `-DCMAKE_PREFIX_PATH=.../packages/world/ogg`) and
installing over the package dirs — the installed `libogg.a`,
`libvorbis.a`, `libvorbisfile.a` are verified EM_MIPS ELF. **Until the
upstream workflows are fixed and releases re-cut, any fresh machine must
do the same** — do not install the ogg/vorbis zips as-is. `jpeg` (in
sdk-world `.2`) has the same bug as installed (`lib/jpeg-static.lib`,
x86-COFF, while its package.yaml declares `lib/libjpeg.a`) — irrelevant
to OPL's EE_LIBS, but broken for any `jpeg_ps2_addons` consumer.

`vorbisfile` and `padx` are not package names — they're second archives
inside the `vorbis` and `pad` packages. In `libs:` write them literally
(`libs: [vorbis, vorbisfile, ...]`, `libs: [pad, padx, ...]`); ps2build
emits the literal `-l` flag and the archive resolves in the parent
package's lib dir.

## Every dependency OPL's Makefile names — verified on disk

Extracted from `Open-PS2-Loader/Makefile` (both
`$(PS2SDK)/iop/irx/*.irx` references and the `EE_LIBS` line), mapped to
the real package and the real artifact filename actually installed.
Watch for **dir-name ≠ artifact-name** splits — they are deliberate and
recurring.

**IOP modules** (`<name>.irx` in OPL's Makefile → package → real file):

```
IEEE1394_bd.irx      core/IEEE1394_bd       ✓
IEEE1394_bd_mini.irx core/IEEE1394_bd_mini  ✓
audsrv.irx           world/audsrv           ✓ (standalone, release zip OK)
bdm.irx              core/bdm               ✓
bdmfs_fatfs.irx      core/bdmfs_fatfs       ✓
eesync-nano.irx      core/eesync-nano       ✓
fileXio.irx          core/filexio           ✓
freepad.irx          core/padman            ✓ (also ships padman.irx, padman-1400.irx)
freesio2.irx         core/sio2man           ✓ (also ships several sio2man variants)
hdproatad.irx        core/hdpro_atad        ✓ (dir name has underscore)
iLinkman.irx         core/iLinkman          ✓
iomanX.irx           core/iomanx            ✓
libsd.irx            core/libsd             ✓
mcman.irx            core/mcman             ✓
mcserv.irx           core/mcserv            ✓
mx4sio_bd.irx        core/mx4sio_bd         ✓
mx4sio_bd_mini.irx   core/mx4sio_bd_mini    ✓
netman.irx           core/netman            ✓
poweroff.irx         core/poweroff          ✓
ps2dev9.irx          core/dev9              ✓ (dir name differs)
ps2fs-osd.irx        core/fs-osd            ✓ (dir name differs)
ps2hdd-osd.irx       core/hdd-osd           ✓ (dir name differs)
ps2ip-nm.irx         core/ps2ip-nm          ✓ (also ships ps2ip.irx)
ps2ips.irx           core/ps2ips-iop        ✓ (the ps2ips package itself is EE-lib-only)
smap.irx             core/smap              ✓
smbman.irx           world/smbman           ✓ (in sdk-world .2)
udnl.irx             core/udnl              ✓
udnl-t300.irx        core/udnl-t300         ✓
usbd_mini.irx        core/usbd_mini         ✓
usbmass_bd.irx       core/usbmass_bd        ✓
usbmass_bd_mini.irx  core/usbmass_bd_mini   ✓
mmceman.irx          world/mmceman          ✓ (in sdk-world .2)
mmcedrv.irx          world/mmcedrv          ✓ (in sdk-world .2)
mmceigr.irx          world/mmceigr          ✓ (in sdk-world .2)
```

**Not built, confirmed NOT needed for the default/release build** (OPL's
Makefile only references these inside `ifeq ($(DEBUG),1)` blocks):
`ioptrap`, `ppctty`, `udptty`. Real upstream source exists at
`ps2sdk/iop/system/{ioptrap,ppctty,udptty}` in the legacy checkout if a
debug build is ever needed.

**EE libraries** (`-l<name>` in `EE_LIBS` → package → real archive):

```
-lgskit               world/gskit               libgskit.a              ✓
-ldmakit              world/dmakit              libdmakit.a             ✓
-lpoweroff            core/poweroff             libpoweroff.a           ✓
-lfilexio             core/filexio              libfilexio.a            ✓
-lpatches             core/patches              libpatches.a            ✓
-lpng                 world/png                 libpng.a                ✓
-lz                   world/z                   libz.a                  ✓
-lmc                  core/mc                   libmc.a                 ✓
-lfreetype            world/freetype            libfreetype.a           ✓
-lvux                 core/vux                  libvux.a                ✓
-lcdvd                core/cdvd                 libcdvd.a               ✓
-lnetman              core/netman               libnetman.a             ✓
-lps2ips              core/ps2ips               libps2ips.a             ✓
-laudsrv              world/audsrv              libaudsrv.a             ✓ (standalone)
-lvorbisfile/-lvorbis world/vorbis              libvorbisfile.a/libvorbis.a ✓ (standalone, rebuilt locally — see above)
-logg                 world/ogg                 libogg.a                ✓ (standalone, rebuilt locally — see above)
-lpadx                core/pad                  libpadx.a               ✓ (lives in the pad package, next to libpad.a)
-lelf-loader-nocolour core/elf-loader-nocolour  libelf-loader-nocolour.a ✓
```

Net: **everything OPL needs is now installed** — sdk-core + sdk-world
via `ps2build update`, plus `audsrv` (release zip) and `ogg`/`vorbis`
(rebuilt from source, see the standalone-packages section).

**Not yet done anywhere**: `cdfs` (CD-filesystem driver) remains
genuinely unmigrated — but OPL's Makefile doesn't reference it directly,
so it may not block OPL (cross-check `modules/iopcore` if it turns out
to matter; possibly redundant with the already-migrated `cdvdfsv`).

## Real path-shape gotchas the porter will hit

Verified against the installed tree, not guessed:

- **freetype**: classic ps2sdk-ports puts headers under
  `ports/include/freetype2`. Here `ft2build.h` sits directly at
  `packages/world/freetype/include/ft2build.h` (with a `freetype/`
  subdir beside it). Use `-I .../freetype/include`, no `freetype2`
  component.
- **gsKit/dmaKit**: classic gsKit repo nests `ee/gs/include` and
  `ee/dma/include`. Here both are flat: `packages/world/gskit/include/`
  has `gsKit.h` directly, same for `dmakit/include/dmaKit.h`.
- **png**: headers are at the **package root**, not `include/` —
  `packages/world/png/png.h` sits next to `package.yaml`
  (`include_dirs: [.]`). Its `package.yaml` already declares
  `headers: [z]`, so a `libs: [png]` reference pulls in zlib's headers
  automatically. Don't go looking for an `include/` dir that doesn't
  exist.
- **`patches` vs `sbv`**: OPL links `-lpatches`; the real archive is
  `packages/core/patches/lib/libpatches.a`. A separate `core/sbv`
  package also exists (`libsbv.a`, same headers) — reference `patches`,
  matching upstream's own `-lpatches` convention.
- **`padx` lives in `pad`**: `libpadx.a` is an artifact of
  `packages/core/pad/` alongside `libpad.a`. Reference the package as
  `pad`.
- **`ps2ips.irx` lives in `ps2ips-iop`**: `core/ps2ips` ships only the
  EE `libps2ips.a`; the IRX is `core/ps2ips-iop/bin/ps2ips.irx`.
- **dir-name ≠ irx-name splits** (reference the package dir name in
  `embed_irx:`/`libs:`, but expect the file names above): `dev9` →
  `ps2dev9.irx`, `fs-osd` → `ps2fs-osd.irx`, `hdd-osd` →
  `ps2hdd-osd.irx`, `hdpro_atad` → `hdproatad.irx`. General rule: when
  in doubt, read the package's own `package.yaml` — it declares the real
  artifact paths.
- There is no merged/flat `ee/include` or `ee/lib` anywhere. ps2build's
  `libs:`/`headers:` resolution is what saves you from hand-writing one
  `-I`/`-L` pair per package.

## Porting OPL's own build: native `ps2.yaml`, already underway

**The decision stands**: OPL's build becomes a real `ps2build` project —
not a CMake wrapper (CMake-wrapping is for projects that already have a
working upstream CMake build; OPL has none), and not a hand-written
Makefile with per-package flags.

**Status as of this rewrite: the port is started, in this repo.**

- `Open-PS2-Loader/ps2.yaml` — the real build definition (EE program +
  IOP module targets, including the multi-variant cdvdman x6 / mcemu x4
  / pademu x2 sets).
- `Open-PS2-Loader/_ps2build_compat/GAPS.md` — **the live gap log**,
  severity-tagged (BLOCKER/HIGH/MEDIUM/LOW). Read this first for
  day-to-day work; it is more current than any handoff doc. Known-good
  items closed there include: `libs:` pulling package include dirs,
  exports-first IOP link ordering, and `.S` assembly on both ee and iop
  targets.
- `_ps2build_compat/gen_assets.py` — replicates the Makefile's ~150
  bin2c/gzip asset rules into `gen/*.c` (needs a two-pass run for the
  embedded `ee_core.elf`/`elfldr.elf`). `gen/STUBS.txt` lists
  empty-stubbed symbols.
- `_ps2build_compat/gen_iop_yaml.py` — emits the IOP-target section of
  `ps2.yaml`.
- `_ps2build_compat/include/` + `include_iop/` — workaround headers for
  real SDK-package gaps (`irx.h`, `types.h`, `defs.h`, `atad.h`,
  `aifdev9.h`, `iopcontrol_special.h`, and the IOP `sys/`+`fcntl.h`
  set). These are still needed; treat their eventual upstreaming as part
  of closing the port.

What this sdk-world `.2` install changes vs. GAPS.md (worth editing
there when someone next touches it): the #14 missing-EE-lib list shrinks
to `audsrv`, `vorbisfile`, `vorbis`, `ogg` (`patches`, `png`, `z`,
`freetype`, `padx`, `elf-loader-nocolour` are all real now); the #15
stubbed-IRX list shrinks to `audsrv.irx` alone (`smbman` and the
`mmceman` family are real now, which also closes #16).

Conventions that still apply when adding or fixing targets:

- Mirror the Makefile's object lists line-for-line in `sources:`, with
  comments explaining *why* each `sources:`/`include_dirs:`/`defines:`
  entry exists — the pattern used throughout `sdk-core/core/*/ps2.yaml`.
- Reference dependencies by real package name via `libs:`/`headers:` —
  never hand-written `-I`/`-L`.
- OPL's `modules/` tree maps to the multi-member-workspace pattern (one
  `ps2workspace.yaml` at repo root, many `ps2.yaml` members) — same
  shape as `sdk-world/mmceman/{mmceman,mmcedrv,mmceigr}`.
- `bin2c` is a real shipped tool (`$PS2DEV/bin/tools/`, or
  `ps2build update bin2c`); the embed-sub-binary pattern is proven in
  `sdk-core/core/kernel/ps2.yaml` and `sdk-core/core/elf-loader-nocolour/`.
- `imports.lst`/`exports.tab` map to the `imports_lst:`/`exports_tab:`
  ps2.yaml fields; OPL's existing files are reused verbatim.

## What was explicitly NOT done (still true)

- **No flat-`$PS2SDK`-tree compat generator.** Ruled out by the user's
  own call ("OPL will get converted over"). Revisit only with explicit
  sign-off.
- **Raw POSIX `socket()`/`bind()`/`recv()`/`send()` etc. are still
  unimplemented** in `libcglue`. Confirmed **not** an OPL blocker:
  `src/ethsupport.c` only calls `ps2ip_getconfig`/`ps2ip_setconfig`,
  never the raw socket family. Documented in `sdk-core`'s `FEEDBACK.md`.
- **`ioptrap`/`ppctty`/`udptty`** (debug-only IRXs) not built — source
  in the legacy ps2sdk checkout if ever needed.
- **`cdfs`** unmigrated; not referenced by OPL's Makefile directly.
- **Debug-only OPL targets** (`modules/debug/udptty-ingame`, ps2link,
  drvtif/tifinet/deci2 blobs, `src/debug.cpp`) intentionally excluded
  from the release port — see GAPS.md #17.
- **ps2-packer/strip packaging** (`RIPTOPL.ELF`) not replicated;
  `build/bin/opl.elf` is the deliverable for now — GAPS.md #19.

## Where to look for more context

- **`Open-PS2-Loader/_ps2build_compat/GAPS.md`** — the live, current gap
  log for the OPL-side port. Start here.
- **`FEEDBACK.md`** — running log of every real finding from the SDK
  effort. **Local-only file**, not committed anywhere (workspace root,
  sibling to `sdk-core`/`sdk-world`); ask for a copy if it's not already
  sitting next to this document.
- Each package's own `package.yaml`/`ps2.yaml` comments — this SDK's
  convention is to document *why*, not just *what*, inline.
- `ps2build explain` — real documentation on the `ps2.yaml` format.
- https://ps2.techwritescode.dev/guide/custom-packages — current
  standalone-package list and release links.
