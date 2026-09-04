# ps2build / new-SDK-layout gap log — OPL (RiptOPL fork) port

**BUILD STATUS: GREEN (2026-09-04).** Full `ps2build build` of `ps2.yaml`
produces `build/bin/opl.elf` (valid MIPS N32 ELF, statically linked,
stripped) plus all 38 IOP `.irx` modules, `ee_core.elf`, and
`elfldr.elf`. All 148 embedded assets are real blobs (0 stubs).

Every gap hit while porting this repo's 1034-line Makefile build to
`ps2build` + the `$PS2DEV/packages/{core,world}` SDK layout. Severity:
**BLOCKER** (cannot produce a working opl.elf today), **HIGH** (target or
asset dropped/stubbed), **MEDIUM** (workaround in-tree), **LOW** (cosmetic /
tooling).

Workaround assets live in `_ps2build_compat/`:
- `include/` — headers missing from the packages layout, needed by EE + IOP:
  `irx.h`, `types.h` (copied from `_smoke/compat_include`), plus `defs.h`,
  `atad.h`, `aifdev9.h`, `iopcontrol_special.h` fetched from ps2sdk master.
- `include_iop/` — IOP-only newlib-style headers the packages layout dropped:
  ps2sdk master's `iop/kernel/include/{fcntl.h,sys/fcntl.h,sys/time.h,
  sys/types.h,sys/unistd.h}`. IOP-only so they never shadow EE newlib.
- `gen_assets.py` — replicates the Makefile's ~150 bin2c/gzip asset rules
  into `gen/*.c`. `gen/STUBS.txt` lists every empty-stubbed symbol.
- `gen_iop_yaml.py` — emits the IOP-target section of `ps2.yaml`.

## ps2build toolchain gaps

1. **iopfixup/eefixup/erx-strip/irx-strip/srxfixup shipped as MSVC DEBUG
   builds** needing VCRUNTIME140D.dll/ucrtbased.dll (absent on non-VS
   machines). Workaround: rebuilt from ps2sdk master. Severity: BLOCKER for
   any IOP .irx without the rebuild. (Fixed locally, fix upstream.)
2. **bin2c not shipped at all.** Rebuilt from ps2sdk master, on PATH at
   `/c/Users/natha/bin`. Severity: BLOCKER for embed_irx without the rebuild.
3. **No generic asset embedding / codegen steps in ps2.yaml.** OPL embeds
   ~90 PNGs, 8 ADP audio files, a TTF font, icon/sys/cfg files, 7 gzipped
   bdmassault blobs, IOPRP.img, and two embedded EE ELFs (ee_core.elf,
   elfldr.elf) via Makefile bin2c rules with exact symbol names
   (`eecore_elf`, `elfldr_elf`, `poeveticanew_raw`, `<name>_png`, ...).
   Workaround: `_ps2build_compat/gen_assets.py` + explicit generated sources.
   Severity: HIGH (manual pre-step; also needs a two-pass run for the ELFs:
   build ee_core/elfldr, re-run gen_assets.py, rebuild).
4. **No pre-build codegen hooks.** `lang_compiler.py` must run manually:
   `python3 lang_compiler.py --make_source --base lng_tmpl/_base.yml
   src/lang_internal.c` and `--make_header ... include/lang_autogen.h`
   (plus the lng_/template steps for runtime .lng packaging, skipped here).
   The Makefile also computes `-DOPL_VERSION` from git; hardcoded to
   `v1.2.0-Beta-ps2build` here. Severity: MEDIUM.
5. **Multi-variant modules**: cdvdman x6, mcemu x4, pademu x2 from one source
   dir each. All variants ARE included in this ps2.yaml (objects are keyed
   per-target for in-tree sources, so no collision); remove variants if a
   ps2build regression reintroduces the object-path collision. Status:
   VERIFY at build time. Severity: MEDIUM.
6. **Stale default include/lib paths**: ps2build emits
   `-I$PS2DEV/ps2sdk/ee/include`, `-I$PS2DEV/ps2sdk/iop/include` and
   `-L$PS2DEV/ps2sdk/ee/lib`, none of which exist in the packages layout.
   Harmless but noisy. Severity: LOW.
7. **`libs:` on a target DOES pull the package's include dirs for compile**
   (verified: `libs: [mc, filexio]` adds their include dirs + `headers:`
   deps). The earlier note that it doesn't is outdated. Severity: — (closed).
7b. **Include dirs propagate transitively through `libs:` linking — never
    put IOP package include dirs on an EE target.** ds34usb_ee/ds34bt_ee
    originally carried `loadcore/include` and `sysclib/include` (IOP kernel
    headers); opl links those libs, so it inherited them, and their
    `sys/types.h` / `string.h` shadowed EE newlib. Symptom: a cascade of
    `clock_t`/`mode_t`/`pid_t` unknown-type and implicit-`open()` errors
    across dozens of EE TUs. Fix: trim EE targets to EE-safe include dirs
    only (module dir + `core/kernel/include`). IOP-only newlib shims live
    in `_ps2build_compat/include_iop/` for the same reason. Severity: —
    (closed; documented as a trap for future ports).
8. **Exports-first link ordering works**: ps2build places the generated
   exports.o first in the IOP link (verified in generated build.ninja for
   nbns/udpfs_smap/atad). Severity: — (closed).
9. **.S assembly works** on both ee and iop targets (compiled via the gcc
   driver). Same-stem .c+.S in one target would collide (both map to
   `obj/<target>/.../<stem>.o`) — OPL has no such pair. Severity: — (closed).
10. **MCP server (ps2build) resolves PS2DEV to a stale path**
    (`C:/Users/natha/Github/ps2dev-suite-2026.08.24-windows-x64`) instead of
    the installed `C:/Users/natha/AppData/Local/ps2dev`, so its
    describe_package/resolve_libs answers are wrong; and the server fails
    Kimi Code schema validation in some clients
    (`tools[3].outputSchema.properties.schema invalid`). Use the CLI.
    Severity: LOW.

## SDK packages-layout gaps

11. **IOP headers missing from packages**: `irx.h`, `types.h`,
    `iop/kernel/include/defs.h`, `iop/dev9/atad/include/atad.h`,
    `iop/dev9/dev9/include/aifdev9.h`. Error shape:
    `fatal error: irx.h: No such file or directory`. Severity: HIGH
    (every IOP module needs these; worked around in `include/`).
12. **IOP sys/ headers missing**: `sys/fcntl.h`, `sys/unistd.h`,
    `sys/time.h`, `sys/types.h`, `fcntl.h` (ps2sdk `iop/kernel/include/`).
    The new IOP toolchain (`mipsel-none-elf`) ships no newlib at all, and
    `common/io_common.h` does `#include <sys/fcntl.h>`, so every IOP module
    including `ioman.h`/`iomanX.h` fails:
    `common/include/io_common.h:19:10: fatal error: sys/fcntl.h: No such
    file or directory`. Worked around in `include_iop/`. Severity: HIGH.
13. **EE header `iopcontrol_special.h` missing** (ps2sdk
    `ee/iopreboot/include/`). Breaks `src/system.c`. Worked around in
    `include/`. Severity: HIGH.
14. **Missing EE library packages** — RESOLVED 2026-09-03. sdk-world
    updated to v2026.09.03.2 (brings `png`, `z`, `freetype`, `jpeg`,
    `smbman`, mmceman family); `patches`, `padx` (= `libpadx.a` inside
    the `pad` package), `elf-loader-nocolour` were already in sdk-core
    v2026.09.03.1; `audsrv`/`ogg`/`vorbis` installed from their
    standalone releases. ps2build resolve_libs now satisfies OPL's full
    EE_LIBS list (`vorbisfile`/`padx` resolve as literal `-l` flags
    against the `vorbis`/`pad` package lib dirs). **Gotcha found at final
    link:** a literal-flag entry like `padx` emits only `-lpadx` — the
    matching `-L$PS2DEV/packages/core/pad/lib` is emitted ONLY if the
    `pad` package itself is also in `libs:`. Symptom:
    `ld.exe: cannot find -lpadx`. Fix: list `- pad` immediately before
    `- padx` in the opl target. Same for any future literal-flag entry
    (`vorbisfile` already has `vorbis` listed, so it was fine).
    Remaining compile
    check: the ports-header TUs (`src/fntsys.c`, `src/textures.c`,
    `src/sound.c`, `src/vcdsupport.c`) should now find ft2build.h /
    png.h / zlib.h / audsrv.h / vorbis headers via `libs:` resolution
    (png's headers are at the package ROOT, include_dirs: [.]).
    Note: still no `-lbdm` archive for the cdvdman BDM variants (the new
    bdm package ships only bdm.irx by design; harmless for a `-dc -r`
    partial link, symbols resolve via imports at runtime).
14b. **Broken standalone release zips (ogg/vorbis/jpeg): built with the
    HOST MSVC compiler.** Their CI workflow never passes
    `-DCMAKE_TOOLCHAIN_FILE=$PS2DEV/share/ps2dev.cmake`, so the shipped
    `lib/ogg.lib`, `lib/vorbis*.lib`, `lib/jpeg-static.lib` are x86-COFF
    objects in ar archives, useless for EE linking (and misnamed vs.
    their package.yaml's `lib/lib*.a`). Worked around locally 2026-09-03
    by rebuilding ogg + vorbis from source with the toolchain file and
    installing into packages/world (real `libogg.a`/`libvorbis.a`/
    `libvorbisfile.a`, verified EM_MIPS ELF). **jpeg is still broken as
    installed** — irrelevant to OPL's EE_LIBS. Fix the upstream
    workflows (ogg, vorbis, jpeg) and re-cut releases.
14c. **`<ps2ipee.h>` is unusable as shipped (SDK bug)**: it declares
    `ps2ip_setconfig(const t_ip_info *)`, which macro-expands to
    `libcglue_ps2ip_setconfig` and conflicts with cglue's
    `sys/socket.h:928` declaration of the same function with a NON-const
    parameter. Any TU including both errors out. Workaround in
    `src/ethsupport.c`: include `<ps2ip.h>` + the real lwip headers
    (`<lwip/ip4_addr.h>`, `<lwip/ip_addr.h>`) under
    `#if __has_include(<lwip/ip4_addr.h>)` instead — ps2ip.h's
    sys/socket.h chain provides `ps2ip_getconfig`/`ps2ip_setconfig`/
    `t_ip_info`/`DHCP_STATE_*`, and classic flat ps2sdk got the lwip
    types transitively so the guard keeps the classic build untouched.
    Fix upstream: make the ps2ipee.h and cglue declarations agree on
    const-ness. Severity: MEDIUM (in-tree workaround).
15. **Missing prebuilt IOP IRXs** — packages now ALL present
    (2026-09-03: sdk-core v2026.09.03.1 + sdk-world v2026.09.03.2 +
    audsrv standalone): `ps2fs-osd.irx` (ps2fs_irx), `usbmass_bd_mini.irx`,
    `iLinkman.irx`, `IEEE1394_bd_mini.irx`, `mx4sio_bd_mini.irx`,
    `hdproatad.irx`, `poweroff.irx`, `ps2hdd-osd.irx`, `ps2dev9.irx`,
    `ps2ip-nm.irx`, `ps2ips.irx`, `audsrv.irx`, `eesync-nano.irx`,
    `udnl.irx`, `smbman.irx`, `mmceman.irx`, `mmcedrv.irx`, `mmceigr.irx`.
    gen_assets.py re-run 2026-09-04: all 148 assets real, 0 stubs.
    Severity: — (closed).
16. **mmceman/mmcedrv/mmceigr come from `$(PS2SDK)/iop/irx`** in the classic
    build (MMCE_ASSETS_DIR) — RESOLVED 2026-09-03: all three now ship in
    sdk-world v2026.09.03.2 (`packages/world/mmce{man,drv,igr}/bin/`).
    gen_assets.py should read them from there on the next regen.

## OPL-side items intentionally not built here

17. Debug-only modules (`modules/debug/udptty-ingame`, `ps2link`, drvtif/
    tifinet/deci2 blobs) — debug builds only; out of scope for the release
    port. `src/debug.cpp` likewise excluded. Severity: LOW.
18. `lng/*.lng` runtime translation packaging (`--make_lng` per language)
    not run — those are runtime files loaded from disk, not linked into the
    ELF. `src/lang_internal.c` + `include/lang_autogen.h` (compiled in)
    ARE generated. Severity: LOW.
19. ps2-packer/strip packaging (`RIPTOPL.ELF`) — ps2build has no post-link
    packing step; `build/bin/opl.elf` is the deliverable. Severity: LOW.
