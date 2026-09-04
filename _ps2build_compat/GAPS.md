# ps2build / new-SDK-layout gap log — OPL (RiptOPL fork) port

**BUILD STATUS: GREEN (2026-09-04), pre- and post-cleanup.** Full
`ps2build build` of `ps2.yaml` produces `build/bin/opl.elf` (valid MIPS
N32 ELF, statically linked, stripped; boots in PCSX2) plus all 38 IOP
`.irx` modules, `ee_core.elf`, and `elfldr.elf`. All 148 embedded assets
are real blobs (0 stubs).

**UPSTREAM SWEEP 2026-09-04:** tech went through `UPSTREAM-REPORT.md` +
this log and shipped fixes — ps2build v2026.09.04.1, sdk-core
v2026.09.04.2, srxfixup v2026.09.04, ogg/vorbis/jpeg v2026.09.03.2 —
plus a cleanup patch (commit `8a555090` on `phaseDout`) deleting all
vendored compat headers and every hardcoded absolute path. Items
resolved by that sweep are marked below.

Every gap hit while porting this repo's 1034-line Makefile build to
`ps2build` + the `$PS2DEV/packages/{core,world}` SDK layout. Severity:
**BLOCKER** (cannot produce a working opl.elf today), **HIGH** (target or
asset dropped/stubbed), **MEDIUM** (workaround in-tree), **LOW** (cosmetic /
tooling).

Remaining workaround assets in `_ps2build_compat/` (post-sweep):
- `gen_assets.py` — replicates the Makefile's ~150 bin2c/gzip asset rules
  into `gen/*.c`. Still required; ps2build has no replacement (ask G.1).
- `gen_iop_yaml.py` — emits the IOP-target section of `ps2.yaml`.
- `include/`, `include_iop/`, `libbdm/` — **deleted 2026-09-04**; all
  those headers are real shipped package files now reachable by package
  name (see #11–#13), and libbdm is a real `libbdm.a` in the bdm
  package (see #14).

## ps2build toolchain gaps

1. **iopfixup/eefixup/erx-strip/irx-strip/srxfixup shipped as MSVC DEBUG
   builds** needing VCRUNTIME140D.dll/ucrtbased.dll (absent on non-VS
   machines). Workaround: rebuilt from ps2sdk master. Severity: BLOCKER for
   any IOP .irx without the rebuild.
   **srxfixup RESOLVED upstream 2026-09-04** (v2026.09.04): root cause
   was CI setting `--config Release` (only meaningful for multi-config
   CMake generators) while the single-config runner needed
   `-DCMAKE_BUILD_TYPE=Release`; re-cut asset verified against plain
   VCRUNTIME140.dll. The other four tools were not explicitly re-cut —
   current component builds work in our green builds, but only srxfixup
   was confirmed fixed by upstream.
2. **bin2c not shipped at all** — RESOLVED upstream 2026-09-04: bin2c is
   now an official `ps2build update` component (v2026.09.03), same
   repo/release as the newly added `ps2pack` component. Local rebuild no
   longer needed.
3. **No generic asset embedding / codegen steps in ps2.yaml.** OPL embeds
   ~90 PNGs, 8 ADP audio files, a TTF font, icon/sys/cfg files, 7 gzipped
   bdmassault blobs, IOPRP.img, and two embedded EE ELFs (ee_core.elf,
   elfldr.elf) via Makefile bin2c rules with exact symbol names
   (`eecore_elf`, `elfldr_elf`, `poeveticanew_raw`, `<name>_png`, ...).
   Workaround: `_ps2build_compat/gen_assets.py` + explicit generated sources.
   Severity: HIGH (manual pre-step; also needs a two-pass run for the ELFs:
   build ee_core/elfldr, re-run gen_assets.py, rebuild).
   **Still open upstream** (ask G.1, not started).
4. **No pre-build codegen hooks.** `lang_compiler.py` must run manually:
   `python3 lang_compiler.py --make_source --base lng_tmpl/_base.yml
   src/lang_internal.c` and `--make_header ... include/lang_autogen.h`
   (plus the lng_/template steps for runtime .lng packaging, skipped here).
   The Makefile also computes `-DOPL_VERSION` from git; hardcoded to
   `v1.2.0-Beta-ps2build` here. Severity: MEDIUM.
   **Still open upstream** (asks G.2/G.4, not started).
5. **Multi-variant modules**: cdvdman x6, mcemu x4, pademu x2 from one source
   dir each. All variants ARE included in this ps2.yaml (objects are keyed
   per-target for in-tree sources, so no collision). Verified across
   multiple full builds. Severity: — (closed).
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
    only (module dir + `core/kernel/include`). IOP-only newlib shims lived
    in `_ps2build_compat/include_iop/` for the same reason (deleted after
    the upstream sweep; the rule still stands). Severity: — (closed;
    documented as a trap for future ports).
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
    Upstream response 2026-09-04: the stale path is almost certainly our
    own saved `ps2build config`/env value from before the install move,
    not a server bug — the server resolves PS2DEV the same way the CLI
    does. Severity: LOW.
10b. **Generated-and-committed files must not contain absolute paths**
    (hermeticity). The 2026-09-04 cleanup patch itself tripped this:
    `gen_iop_yaml.py` emitted `-include <ROOT>/.../ps2sdk-compat.h` with
    ROOT = the generator-runner's checkout, so tech's regeneration baked
    `D:/_PLATFORMS/.../scratch_opl_pr602/...` into the committed ps2.yaml
    (breaks anywhere else). Fixed on our side: bare `-include
    ps2sdk-compat.h` (found via the existing `-I ports/playstation2`;
    `-include` searches `-I` dirs), plus a CRLF-tolerant MARKER match in
    the generator (a CRLF working copy previously made the strip-marker
    miss and doubled the IOP section). Reported back upstream.

## SDK packages-layout gaps

11. **IOP headers missing from packages** (`irx.h`, `types.h`, `defs.h`,
    `atad.h`, `aifdev9.h`) — **RESOLVED upstream 2026-09-04.** Real root
    cause found by tech: ps2build's `resolvePackageLibs`/`resolvePackageHeaders`
    required a `kind: library` artifact before pulling anything from a
    package; most IOP kernel modules (loadcore, sysclib, ...) are
    `kind: driver`-only, so their (always-shipped!) headers resolved to
    nothing. ps2build v2026.09.04.1 falls back to the driver artifact
    (include_dirs/headers only, no archive/-L). sdk-core v2026.09.04.2
    additionally declares `include_dirs:` on `atad`, `dev9`, `ps2ip-nm`,
    `bdm` — they'd shipped the headers but never declared them.
    Vendored `_ps2build_compat/include/` deleted.
12. **IOP sys/ headers missing** (`sys/fcntl.h`, `sys/unistd.h`,
    `sys/time.h`, `sys/types.h`, `fcntl.h`) — **RESOLVED upstream
    2026-09-04** by the same fix: these are ps2sdk's own
    `iop/kernel/include/` files, shipped in the `loadcore` package all
    along, just unreachable by package name. Vendored
    `_ps2build_compat/include_iop/` deleted.
13. **EE header `iopcontrol_special.h` missing** — **RESOLVED upstream
    2026-09-04** (same driver-artifact fallback / include_dirs sweep).
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
    **Still open upstream** (ask F.1 — documented by tech, deliberately
    not code-fixed yet; package-resolution semantics).
    `-lbdm` sub-item RESOLVED upstream 2026-09-04: the bdm package now
    ships a real `lib/libbdm.a` + headers (second `kind: library`
    artifact, from the same `bd_cache.c`/`bd_defrag.c` we had vendored).
    The two BDM cdvdman variants now use `libs: ["bdm"]`; vendored
    `_ps2build_compat/libbdm/` deleted.
14b. **Broken standalone release zips (ogg/vorbis/jpeg): built with the
    HOST MSVC compiler** — **RESOLVED upstream 2026-09-04.** Their CI
    never passed `-DCMAKE_TOOLCHAIN_FILE=$PS2DEV/share/ps2dev.cmake`, so
    the shipped `.lib` files were x86-COFF host objects. Upstream added
    the toolchain-file default and re-cut all three as v2026.09.03.2
    (real EM_MIPS objects). Our local ogg/vorbis source rebuild is no
    longer needed.
14c. **`<ps2ipee.h>` const conflict** — **RESOLVED upstream 2026-09-04**
    (sdk-core v2026.09.04.2). Root cause was libcglue's own
    `sys/socket.h` externs (`libcglue_ps2ip_setconfig`,
    `libcglue_dns_setserver`) and the `ps2sdkapi.c` trampolines being
    non-const — `ps2ipee.h` was already correct, matching both real
    backends and lwip's `dns_setserver`. Our `src/ethsupport.c`
    workaround (`<ps2ip.h>` + real lwip headers under `__has_include`)
    still works and stays (keeps the classic flat-sdk build untouched);
    `<ps2ipee.h>` is usable again if we ever want to simplify.
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
    gen_assets.py reads them from there.

## OPL-side items intentionally not built here

17. Debug-only modules (`modules/debug/udptty-ingame`, `ps2link`, drvtif/
    tifinet/deci2 blobs) — debug builds only; out of scope for the release
    port. `src/debug.cpp` likewise excluded. Severity: LOW.
18. `lng/*.lng` runtime translation packaging (`--make_lng` per language)
    not run — those are runtime files loaded from disk, not linked into the
    ELF. `src/lang_internal.c` + `include/lang_autogen.h` (compiled in)
    ARE generated. Severity: LOW.
19. ~~ps2-packer/strip packaging~~ — **RESOLVED upstream 2026-09-04**:
    ps2build v2026.09.04.1 adds `pack: true` on `ee` targets — a real
    post-link `ps2pack` step producing `<name>-packed.elf`. `ps2pack` is
    now a `ps2build update` component. **Enabled on the `opl` target** by
    the cleanup patch: the build emits `build/bin/opl-packed.elf`
    (4,068,416 → 1,677,904 bytes, 58.76%) alongside the unpacked
    `opl.elf`.
