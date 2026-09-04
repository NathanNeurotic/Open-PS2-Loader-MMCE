# UPSTREAM REPORT — what it took to build OPL under ps2build

Audience: tech (ps2dev/ps2build upstream). Date: 2026-09-04.
Companion: `GAPS.md` (full gap log with severities and error shapes).

**Result: GREEN.** `ps2build build -c ps2.yaml` produces
`build/bin/opl.elf` (MIPS N32, statically linked, stripped, ~4 MB) plus
all 38 IOP `.irx` modules, `ee_core.elf`, `elfldr.elf`. All 148 embedded
assets are real blobs, 0 stubs. **Boots in PCSX2.** Real-hardware test
pending.

Toolchain: ps2build v2026.09.03.3, sdk-core v2026.09.03.1,
sdk-world v2026.09.03.2, audsrv standalone release, ogg + vorbis
rebuilt from source locally (see ask #1).

---

## 1. The changeset ("the commits this needed")

The entire port touches **two pre-existing source files**. Everything
else is new, additive build machinery.

### 1a. `src/ethsupport.c` — the ONLY EE source edit

Classic flat ps2sdk provided `t_ip_info`, `ps2ip_getconfig/setconfig`,
`DHCP_STATE_*`, and the lwip `ip4_addr` types transitively. The packages
layout does not, and `<ps2ipee.h>` is unusable (see ask #2), so:

```diff
+/* ps2ip_getconfig/ps2ip_setconfig + t_ip_info + DHCP_STATE_* live behind
+ * <ps2ip.h> in the new packages-layout SDK (via its <sys/socket.h> chain);
+ * struct ip4_addr/IP4_ADDR/ip4_addrN/ip_addr_* need the real lwip headers
+ * explicitly. <ps2ipee.h> is NOT usable: its own ps2ip_setconfig(const
+ * t_ip_info*) declaration macro-expands to libcglue_ps2ip_setconfig and
+ * conflicts with cglue's non-const one (SDK bug, see GAPS.md). Classic flat
+ * ps2sdk got all of this transitively, so only include in the new layout. */
+#if __has_include(<lwip/ip4_addr.h>)
+#include <ps2ip.h>
+#include <lwip/ip4_addr.h>
+#include <lwip/ip_addr.h>
+#endif
```

The `__has_include` guard keeps the classic Makefile build untouched.
Also required: `ps2ip` added to the link (`ps2ip_getconfig/setconfig`
are `T` in `libps2ip.a`; classic EE_LIBS never needed `-lps2ip`).

### 1b. `modules/iopcore/cdvdman/imports.lst` — one import

The vendored libbdm `bd_cache.c` (compiled into the two BDM cdvdman
variants, see 1d) calls `FreeSysMemory`, which cdvdman never imported:

```diff
 sysmem_IMPORTS_start
 I_Kprintf
 I_AllocSysMemory
+I_FreeSysMemory
 #ifdef __IOPCORE_DEBUG
```

### 1c. New: `ps2.yaml` (2306 lines, hand-authored + generated)

Every Makefile target mapped: `opl` (EE app), `oplxx` (C++ support
lib), `ds34usb_ee`/`ds34bt_ee` (EE half of the DS3/DS4 drivers),
`ee_core`, `elfldr`, and all 38 IOP modules including the multi-variant
families (cdvdman x6, mcemu x4, pademu x2). The IOP section is emitted
by `_ps2build_compat/gen_iop_yaml.py` (idempotent; strips/regenerates
below the `# IOP MODULES` marker).

Notable yaml details:
- `libs:` on `opl` gains `ps2ip` (new) and `pad` **before** the literal
  `padx` flag (see ask #6).
- EE targets must NEVER receive IOP package include dirs — they
  transitively propagate through `libs:` linking and IOP
  `sys/types.h`/`string.h` then shadow EE newlib (the
  clock_t/mode_t/pid_t/implicit-open() cascade). ds34 EE targets are
  trimmed to module dir + `core/kernel/include` only.
- png's headers live at the package ROOT (`include_dirs: [.]`), not
  `include/` — include path points at the package root.

### 1d. New: `_ps2build_compat/` (all workaround machinery)

- `gen_assets.py` — replicates the Makefile's ~150 bin2c/gzip asset
  rules into `gen/*.c` with the exact symbol names the code expects
  (`eecore_elf`, `elfldr_elf`, `<name>_png`, `*_adp`, `bdma_*_gz`,
  `IOPRP_img`, ...). Needs a **two-pass bootstrap**: build once so
  `bin/ee_core.elf`/`elfldr.elf` exist, re-run, rebuild.
- `gen_iop_yaml.py` — emits the IOP section of ps2.yaml. The two BDM
  cdvdman variants additionally compile vendored
  `libbdm/src/{bd_defrag,bd_cache}.c` because the bdm package ships no
  archive (see ask #5).
- `include/` — headers missing from the packages layout, fetched from
  ps2sdk master: `irx.h`, `types.h`, `defs.h`, `atad.h`, `aifdev9.h`,
  `iopcontrol_special.h` (see ask #3).
- `include_iop/` — IOP-only newlib-style headers the new
  `mipsel-none-elf` IOP toolchain lacks: `fcntl.h`, `sys/fcntl.h`,
  `sys/time.h`, `sys/types.h`, `sys/unistd.h`, plus a `ps2ip.h` shim.
  IOP-only so they never shadow EE newlib.
- `libbdm/` — vendored `bd_defrag.c`, `bd_cache.c`, `bd_defrag.h`,
  `bd_cache.h`, and ps2sdk master's `module_debug.h` (from
  `iop/fs/libbdm/src/include/`).

### 1e. Generated in-repo files (not hand edits)

- `src/lang_internal.c` + `include/lang_autogen.h` via the repo's own
  `lang_compiler.py --make_source/--make_header` (Makefile step
  replicated manually — ps2build has no pre-build hooks, see ask #7).
- `_ps2build_compat/gen/*.c` (148 assets) — regenerable, do not diff.

### Reproduce

```sh
cd Open-PS2-Loader
python3 lang_compiler.py --make_source --base lng_tmpl/_base.yml src/lang_internal.c
python3 lang_compiler.py --make_header --base lng_tmpl/_base.yml include/lang_autogen.h
python3 _ps2build_compat/gen_assets.py     # pass 1 (ee_core/elfldr stubbed)
ps2build build -c ps2.yaml                 # builds everything incl. ee_core/elfldr
python3 _ps2build_compat/gen_assets.py     # pass 2: stubs -> real ELFs
ps2build build -c ps2.yaml                 # final link -> build/bin/opl.elf
```

---

## 2. Upstream asks, ranked

### A. BLOCKER — broken ogg/vorbis/jpeg release zips (host-compiled)

Their `.forgejo/workflows` CMake steps never pass
`-DCMAKE_TOOLCHAIN_FILE=$PS2DEV/share/ps2dev.cmake`, so the shipped
`lib/ogg.lib`, `lib/vorbis*.lib`, `lib/jpeg-static.lib` are **x86-COFF
host objects in ar archives** (magic `4c01`) — useless for EE linking,
and misnamed vs. their own package.yaml's `lib/lib*.a`.

Worked around locally by rebuilding ogg + vorbis from source with the
toolchain file (verified EM_MIPS) and installing into `packages/world`.
**jpeg is still broken as installed** (irrelevant to OPL, but the next
project will hit it).

Fix: add the toolchain file to the three CI workflows, re-cut releases.

### B. HIGH — `<ps2ipee.h>` const conflict (sdk-core)

`ps2ipee.h` declares `ps2ip_setconfig(const t_ip_info *)`; it
macro-expands to `libcglue_ps2ip_setconfig` and then conflicts with
cglue `sys/socket.h:928`, which declares the same function with a
NON-const parameter. Any TU including both fails. Fix: make the two
declarations agree on const-ness. (OPL works around it by using
`<ps2ip.h>` + real lwip headers instead.)

### C. HIGH — headers missing from the packages layout

- IOP, needed by essentially every module: `irx.h`, `types.h`,
  `iop/kernel/include/defs.h`, `iop/dev9/atad/include/atad.h`,
  `iop/dev9/dev9/include/aifdev9.h`.
- IOP sys/ newlib-style headers (`sys/fcntl.h`, `sys/unistd.h`,
  `sys/time.h`, `sys/types.h`, `fcntl.h`): the new `mipsel-none-elf`
  IOP toolchain ships no newlib, and `common/io_common.h` does
  `#include <sys/fcntl.h>`, so anything including `ioman.h`/`iomanX.h`
  fails out of the box.
- EE: `iopcontrol_special.h` (ps2sdk `ee/iopreboot/include/`).

All currently vendored in `_ps2build_compat/include{,_iop}/`.

### D. HIGH — tool zips: MSVC DEBUG builds; bin2c missing

`iopfixup`, `eefixup`, `erx-strip`, `irx-strip`, `srxfixup` ship as
MSVC **debug** builds requiring VCRUNTIME140D.dll/ucrtbased.dll (absent
on non-Visual-Studio machines). `bin2c` is not shipped at all. All were
rebuilt from ps2sdk master locally. Ship release builds (or
statically-linked) and add bin2c.

### E. MEDIUM — bdm package ships only bdm.irx

No `libbdm.a`, no headers — but OPL's BDM cdvdman variants compile
`bd_defrag.c`/`bd_cache.c` from source (classic build used
`$(PS2SDK)/iop/fs/libbdm`). Vendored under `_ps2build_compat/libbdm/`.
Either ship the libbdm archive + headers in the bdm package, or
document that consumers must vendor.

### F. LOW — ps2build nits

1. **Literal `-l` flag entries don't pull the providing package's lib
   dir.** `libs: [padx]` emits `-lpadx` but no
   `-Lpackages/core/pad/lib` unless `pad` is also listed →
   `ld: cannot find -lpadx`. Either auto-emit the `-L` when a literal
   flag matches a `lib<X>.a` in some package, or document the pattern.
   (`vorbisfile`/`vorbis` same shape.)
2. Stale default `-I$PS2DEV/ps2sdk/ee/include`, `-I.../iop/include`,
   `-L.../ee/lib` — none exist in the packages layout. Harmless, noisy.
3. MCP server resolves PS2DEV to a stale path instead of the installed
   one (describe_package/resolve_libs wrong); also fails Kimi Code
   schema validation (`tools[3].outputSchema.properties.schema
   invalid`). CLI is fine.

### G. FEATURE REQUESTS — ps2build

1. **Generic file embedding** (bin2c-equivalent): embed arbitrary files
   with exact symbol names (`<name>_png`, `poeveticanew_raw`), optional
   gzip. OPL embeds ~150 assets this way; the `embed_irx:` mechanism
   covers IRX-by-package-name only. This is the single biggest piece of
   out-of-tree machinery the port needed (`gen_assets.py`).
2. **Pre-build codegen hooks** (run a script before generate/build;
   OPL needs `lang_compiler.py` + the two-pass asset bootstrap).
3. **Post-link packing step** — ps2pack integration (per tech: ps2pack
   exists now, just not wired into the pipeline). Classic Makefile
   produces `RIPTOPL.ELF` via ps2-packer; ps2build output is unpacked.
4. Version define from git (`-DOPL_VERSION=...` is hardcoded here).

---

## 3. Known divergences (Ripto, not upstream)

- **ps2pack** exists and is solved on Ripto's side — replaces
  ps2-packer in the release pipeline once ps2build wires a pack step.
- **ps2ip.irx and mmce*.irx fixes** live in Ripto's fork; corrections
  upstream hasn't implemented. The ps2build build here uses the
  shipped package IRXs, so hardware testing may surface those known
  issues — coordinate before upstreaming bug reports about them.

## 4. What worked with zero workarounds

- IOP exports-first link ordering (verified in generated build.ninja).
- `.S` assembly on both ee and iop targets.
- `libs:` pulling package include dirs (incl. `headers:` deps) — the
  mechanism is right; the ds34 incident was our yaml, not ps2build.
- Multi-variant modules from one source dir: objects keyed per-target,
  no collisions (cdvdman x6, mcemu x4, pademu x2 all in one build).
