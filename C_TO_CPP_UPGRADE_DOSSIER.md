# Open PS2 Loader: `src/` C-to-C++ Upgrade Dossier

**Status:** informational planning document; no source conversion has been performed  
**Prepared:** 2026-08-27  
**Repository:** `https://github.com/NathanNeurotic/Open-PS2-Loader.git`  
**Working copy:** `C:\Users\natha\Github\CPLUSPLUS\Open-PS2-Loader`  
**Scope requested:** convert the main EE sources under `src/` from C to C++, preserve behavior, and prove that the result still works.

## 1. Executive result

The requested conversion is feasible only as a staged compatibility migration. It is not safe to rename every file and call the work complete. This repository is a PS2 EE/IOP system with generated C sources, C-linkage embedded assets, packed controller/network structures, handoff code, inline MIPS/VU assembly, `setjmp`/`longjmp` cleanup, thread and RPC callbacks, and a build system whose normal EE rule explicitly compiles `src/*.c` with the C compiler.

The recommended target is **C++ syntax and compilation for the main EE `src/` translation units while retaining the existing C-shaped APIs, data layouts, allocation model, and runtime architecture**. Do not introduce classes, STL containers, exceptions, RTTI, global constructors, smart pointers, or a broad redesign during the parity phase. Modernization can follow only after the C++ build has binary and hardware parity.

The strict phrase “`src/` folder only” has an important build consequence:

* Renaming `src/*.c` to `src/*.cpp` is a source-tree change, but the root `Makefile` currently has an explicit `$(EE_SRC_DIR)%.o: $(EE_SRC_DIR)%.c` rule and lists object names. A rename cannot build without a build-rule or generated-source adjustment outside `src/`.
* Project headers in `include/` do not currently provide a project-wide `extern "C"` boundary. C++ source can include SDK headers that already have guards, but symbols shared with C-generated objects and untouched C TUs need deliberate linkage handling.
* `src/lz4.c` and `src/zso.c` include implementation files from `modules/isofs/`; converting those TUs changes the language in which those included bodies are parsed without changing the module files.

Therefore, the later implementation must obtain an explicit decision on the smallest necessary build/header exception. If no exception is allowed, the safe alternative is a controlled C++ compile rule that keeps the physical `.c` names, but that is still a `Makefile` change and must be documented as such.

This dossier records the baseline, the hazards, a staged implementation plan, validation gates, rollback rules, and a copy/paste operator prompt for the future execution turn.

## 2. Snapshot and evidence boundary

The repository was cloned into the isolated workspace requested by the user. The clone succeeded using Git's OpenSSL TLS backend after the host Schannel backend failed with `SEC_E_NO_CREDENTIALS`; this was a transport workaround only and did not alter repository content.

The audited snapshot is:

| Item | Value |
|---|---|
| Remote | `https://github.com/NathanNeurotic/Open-PS2-Loader.git` |
| Branch | `rebuild/main` |
| Upstream tracking | `origin/rebuild/main` |
| Commit | `679dc31efc9dc290be405b5efbee5e4d979da79d` |
| Tree | `fb38df9d7f650b6ce6104d96640f12e7ad5c94a7` |
| Commit subject | `fix: rebuild recovery (neutrino, bdm, hdd, pad, art) (#536)` |
| Commit date | 2026-08-26 |
| Initial status | clean; no source edits made |

The local host's ordinary `PATH` is not a complete OPL build environment, but both repository-pinned Docker images are cached locally and contain the EE C and C++ drivers, PS2SDK, ports, and gsKit. Read-only audit builds were therefore run in disposable containers from a clean local clone of the exact commit. Both pinned SDK lineages compiled and linked the unmodified C tree with GCC/G++ 15.2.0 and identical loadable section totals (`text=1,827,720`, `data=2,082,340`, `bss=1,212,180`, `dec=5,122,240`). The official-pinned ELF was 11,866,316 bytes; the ps2dev-pinned ELF was 11,866,328 bytes.

Those audit builds prove that this snapshot and both target C compilers can produce an ELF. They are **not** console proof, and their ephemeral ELFs were not promoted as golden hardware controls. Full ELF hashes are also checkout-path-sensitive because the default `-gdwarf-2 -gz` records `DW_AT_comp_dir`: the same `nbns.c` object built under `/tmp/a/repo` and `/tmp/b/repo` had different hashes, while `strip -g` made the objects byte-identical. Two clean rebuilds at the same path did produce the same official-pinned ELF hash (`bf2c489f205c4d19b29daaf4000222f20b91f51da1164bcfd134b0a762172e80`). A future operator must use a fixed build path or a reviewed prefix-map policy and archive both full/debug and stripped-or-loadable-section hashes before conversion.

Three environment traps were reproduced. First, the Windows checkout stores these text files as CRLF (`git ls-files --eol` reports `i/lf w/crlf`), so executing a directly mounted shell script in Linux can fail on `\r`; clone Git objects inside the container or otherwise guarantee LF. Second, `sh -lc` can reset the image's toolchain `PATH`; use `sh -c` or explicitly prepend `/usr/local/ps2dev/ee/bin`, `/usr/local/ps2dev/iop/bin`, and `/usr/local/ps2dev/bin`. Third, the ps2dev pin is intentionally minimal and lacks Git until the workflow's Alpine host-package step runs; mirror `.github/workflows/flavours.yml` rather than assuming both images have identical host utilities. The official pinned registry reference did not resolve afresh during this audit, although a cached local image whose ID/digest matched the repository pin was usable. Future work must verify pin pullability and inspect the resolved image before trusting it.

### Self-audit record

On 2026-08-27 the dossier was checked against the cloned tree after drafting. The inventory contains all 46 tracked `src/*.c` files; `src/` contains 50,942 physical lines and 44,017 nonblank lines; `dialogs.c` contains 726 designated-initializer expressions; the source contains 47 `goto` tokens and 55 `volatile` tokens; and exactly one source NUL byte remains, in `src/bdmsupport.c:564`. The build also generates a 47th main translation unit, `src/lang_internal.c`. `git diff --name-only -- src include Makefile` is empty. The only working-copy changes are the two Markdown documents named in the final handoff.

## 3. Scope, constraints, and non-goals

### In scope for the future conversion

* Main EE translation units physically under `src/` (the 46 tracked `.c` files listed in Section 5).
* Mechanical C++-compatibility changes required to compile those units and preserve their existing behavior.
* Source-side linkage declarations or a temporary compatibility header only if they are demonstrably necessary to link the converted EE objects. Such changes must be called out as a scope exception if they are outside `src/`.
* Build and validation work needed to demonstrate parity.

### Explicitly out of scope unless the user later authorizes an exception

* `modules/`, `ee_core/`, `elfldr/`, IOP modules, generated `asm/*.c` definitions, and PS2SDK itself.
* A settings redesign, storage redesign, networking redesign, POPSTARTER/Neutrino protocol change, controller behavior change, or launch-sequence change.
* Converting every header to a C++ style, replacing include guards, or replacing all `NULL` with `nullptr`.
* Introducing STL, exceptions, RTTI, dynamic initialization, `new`/`delete`, RAII around PS2 resources, or a class/namespace architecture.
* “Cleaning up” unrelated warnings, formatting, algorithms, or code paths while the migration is in progress.
* Claiming that a green host compile or CI build proves PS2 hardware behavior.

### Required invariants

The conversion must preserve exported names, C ABI entry points, structure sizes and offsets, packed/bitfield wire layouts, stack and heap budgets, section placement, cache and DMA behavior, thread lifetimes, SIF/fileXio sequencing, IOP reset/keep-IOP behavior, embedded asset contents, configuration keys and save timing, and all supported launch paths.

## 4. Current build contract

The root `Makefile` is the controlling build contract for the main EE executable:

* `FRONTEND_OBJS` explicitly enumerates the main objects, including `lang_internal.o`, `zso.o`, and `lz4.o`.
* `EE_SRC_DIR = src/`, `EE_OBJS_DIR = obj/`, and `EE_ASM_DIR = asm/` define source, object, and generated-asset locations.
* The custom EE rule is `$(EE_OBJS_DIR)%.o: $(EE_SRC_DIR)%.c` and invokes `$(EE_CC)`. There is a separate generated-assembly C rule.
* `src/lang_internal.c` is generated by `lang_compiler.py` and ignored by Git. The generated file is not hand-maintained.
* `asm/*.c` supplies generated binary arrays and sizes for ELF/IRX, fonts, PNGs, themes, audio, and icons. Those definitions are compiled as C and use unmangled symbol names.
* The root makefile includes PS2SDK's `Makefile.pref` and `Makefile.eeglobal` only when `PS2SDK` is set.
* The normal flags include MIPS-specific optimization, section garbage collection, dependency generation, and PS2SDK include/library paths. The final link is normally driven by the EE C compiler.
* PS2SDK's pinned `Makefile.eeglobal` has separate `EE_CFLAGS` and `EE_CXXFLAGS` pipelines and its `.cpp` rule consumes only `EE_CXXFLAGS`. OPL adds every feature macro and important front-end flag only to `EE_CFLAGS` (`__RTL`, `_DTL_T10000`, `GSM_1080P`, `IGS`, `PADEMU`, `__OPLDIAG`, debug/TTY macros, optimization, `OPL_VERSION`, dependency generation, and function/data sections). A naïve switch to the stock `.cpp` rule can therefore compile successfully with the wrong feature set and version string.
* Dependency generation is currently ineffective. `Makefile:297` adds `-MMD -MP`, but `Makefile:300` expands `EE_DEPS` through an unintended extra variable-reference level. A target-make evaluation on this snapshot reported `EE_OBJS_WORDS=234` and `EE_DEPS_WORDS=0`; consequently `Makefile:1017` includes no generated `.d` files. Until that expression is corrected and tested, every gate must start with a clean object directory.
* The in-ELF provenance string is not sufficient proof of source identity. `Makefile:98` checks only the unstaged worktree, not staged or untracked conversion files; `Makefile:101-102` treats a linked worktree's `.git` file as dirty because it tests `-d`; and `CODE_ANCHOR` is commit-history based. Candidate artifacts must come from committed trees and carry the commit, tree SHA, full status, staged diff, untracked inventory, build manifest, and ELF hash.
* Default debug information embeds the absolute checkout path. Full object/ELF hashes are comparable only under a fixed build path or a reviewed reproducible-path mapping. Also record stripped or loadable-section hashes so harmless DWARF path drift is not mistaken for runtime drift.

The future build design must choose one of these controlled options and record the choice:

1. **Physical rename:** rename selected `src/*.c` to `.cpp`, extend object/source rules, and update the object list/generator handling.
2. **Language override:** retain `.c` names but add a narrowly scoped C++ rule using `EE_CXX`/`-x c++`; this reduces generated-file churn but means file extensions no longer describe the language.
3. **Hybrid:** convert leaf files first with a temporary rule, then settle the permanent layout after linkage and size evidence.

Option 3 is recommended for the first spike; option 1 is the clearest final repository state if the generated-source policy is resolved. Before either option, define one shared front-end flag set that is deliberately applied to both C and C++ units, prove preprocessor-macro parity for every flavor, and repair or explicitly quarantine the broken dependency list. These are minimum build-integration exceptions, not optional cleanup.

## 5. Source inventory and conversion order

There are 46 tracked C files in `src/`, representing 50,942 physical lines (44,017 nonblank lines) before preprocessing. The following order is deliberately risk-weighted rather than alphabetical. “Last” means high fan-in or hardware-critical; it does not mean unimportant.

### Leaf and format/parsing files — first conversion wave

| File | Approx. LOC | Notes |
|---|---:|---|
| `nbns.c` | 39 | Small protocol helper; parses as GNU++17, but unchanged C++ compilation mangles all three public names and increases one measured stack frame. Use only after establishing the intended C-linkage boundary. |
| `httpclient.c` | 79 | Small network helper; preserve socket/error behavior. |
| `ps2cnf.c` | 120 | Parser/format code; check string and buffer assumptions. |
| `ioprp.c` | 118 | Embedded IOPRP declarations; C linkage is required. |
| `xparam.c` | 246 | Parameter parsing and string ownership. |
| `gsm.c` | 206 | Video/GSM data and SDK calls; preserve packed values. |
| `atlas.c` | 194 | Rendering data helper; low fan-in. |
| `artindex.c` | 333 | Art index/cache state; inspect volatile and retry behavior. |
| `folderbrowse.c` | 101 | Directory enumeration and path buffers. |
| `lz4.c` | 1 | Includes `modules/isofs/lz4.c`; the included body becomes C++-parsed. |
| `zso.c` | 89 | Includes `modules/isofs/zso.c`; raw storage reads and pointer arithmetic. |

### Data, services, and reusable support — second wave

| File | Approx. LOC | Notes |
|---|---:|---|
| `lang.c` | 258 | Generated language table contract; keep `_l` and `internalEnglish` ABI. |
| `config.c` | 1,548 | Configuration ownership, allocation, save timing, and compatibility. |
| `util.c` | 784 | Allocation, file, image, and callback helpers; inline `nop` assembly. |
| `ioman.c` | 439 | `va_list`/`vsnprintf`, file-driver adapters, and `_gp` interactions. |
| `debug.c` | 74 | Logging and low-level diagnostics. |
| `hdd.c` | 476 | Block-device operations and memory/error handling. |
| `supportbase.c` | 1,673 | Shared storage/game-list support; many allocations and frees. |
| `tar.c` | 674 | Archive parsing and cleanup labels; alignment attributes. |
| `retrogem.c` | 350 | Legacy support path; preserve binary data handling. |
| `vmc_groups.c` | 1,474 | VMC grouping/config data; layout and save behavior. |
| `cheatman.c` | 530 | Cheat data parsing and ownership. |
| `appsupport.c` | 959 | Application metadata and path/config interactions. |
| `favsupport.c` | 978 | Favorites persistence and UI-facing data. |

### Storage, network, and handoff — third wave

| File | Approx. LOC | Notes |
|---|---:|---|
| `bdmsupport.c` | 3,313 | BDM drivers, threads, callbacks, and one embedded NUL byte in source. |
| `hddsupport.c` | 2,275 | APA/PFS/HDD/VCD paths; string-literal constness and large state machine. |
| `ethsupport.c` | 1,240 | Ethernet/SMB behavior, threads, sockets, and recovery. |
| `udpfssupport.c` | 425 | UDPFS/UDPBD protocol and asynchronous state. |
| `mmcesupport.c` | 1,174 | MMCE storage/device lifecycle; preserve callback and mount order. |
| `vcdsupport.c` | 2,500 | VCD/POPSTARTER preparation and launch-adjacent behavior. |
| `elfldr_noreset.c` | 144 | Embedded child loader, VLA, inline assembly, `ExecPS2`, and keep-IOP semantics. |
| `system.c` | 1,851 | IOP modules, embedded images, reset/boot sequencing, and `break` assembly. |

### Rendering, input, audio, UI, and orchestration — final wave

| File | Approx. LOC | Notes |
|---|---:|---|
| `renderman.c` | 625 | Renderer state and SDK ABI. |
| `fntsys.c` | 835 | Embedded font arrays and rendering callbacks. |
| `textures.c` | 1,086 | libpng C callbacks, `setjmp`/`longjmp`, embedded PNGs. |
| `texcache.c` | 1,619 | Cache state, DMA/threads, `_gp`, and manual cleanup. |
| `themes.c` | 3,097 | Theme data and embedded configuration blobs. |
| `sound.c` | 853 | Audio assets, threads, aligned stacks, and callbacks. |
| `pad.c` | 1,004 | Controller reports, alignment, rumble, and packed ABI. |
| `menusys.c` | 1,892 | Navigation state and `goto`-based cleanup/reshow paths. |
| `dia.c` | 1,442 | UIItem unions, function pointers, and dialog state. |
| `dialogs.c` | 1,902 | Hundreds of designated initializers in static UI tables. |
| `guigame.c` | 1,917 | Game UI, string-literal arrays, art and launch-facing state. |
| `gui.c` | 4,834 | High fan-in UI, VU inline assembly, dialog compound literals. |
| `opl.c` | 4,878 | Main orchestration/entry point; convert last after all dependencies. |
| `OSDHistory.c` | 293 | Embedded icons and OSD history state. |

## 6. C-to-C++ hazard register

Severity is about the risk of a silent functional regression, not just compiler effort.

| Severity | Hazard | Evidence in this snapshot | Required treatment |
|---|---|---|---|
| P0 | Build extension/rule mismatch | Makefile object rule is explicitly `%.c` + `EE_CC`; object list is explicit. | Settle a controlled `.cpp` or `-x c++` rule before broad conversion. Prove clean/rebuild from scratch. |
| P0 | C/C++ flag split | OPL appends feature/version/optimization/dependency/section flags to `EE_CFLAGS`; PS2SDK's `.cpp` rule reads separate `EE_CXXFLAGS`. | Define a shared front-end flag source; compare full commands and `-dM -E` macro dumps for every supported flavor before accepting any C++ object. |
| P0 | Stale incremental objects | `EE_DEPS` evaluates to zero words although 234 EE object words exist. | Fix and regression-test dependency inclusion as an approved build exception; require `make clean` for every gate until then. Touch a transitive header and prove the expected C++ object rebuilds. |
| P0 | Misleading artifact provenance | Staged/untracked conversion files are not caught by `git diff --quiet`; linked worktrees are always branded dirty; debug paths perturb hashes. | Build committed trees from a full clone/fixed path. Record status, staged/untracked state, commit/tree, normalized and full hashes, image digest, SDK/IRX manifests, and compiler commands. |
| P0 | C/C++ linkage of generated data | `asm/*.c`, `extern_irx.h`, and many source-level `extern` arrays define unmangled names. | Add narrow `extern "C"` declarations/wrappers; inspect `nm` for every generated symbol. |
| P0 | Header layout and ABI | `dia.h`, `gui.h`, and `ds34common.h` use anonymous structs/unions, packed structures, bitfields, and function pointers; project headers lack a common C-linkage wrapper. | Compile with the actual target GCC; record `sizeof`, `alignof`, `offsetof`, and packet bytes before/after. Do not “modernize” layouts. |
| P0 | `setjmp`/`longjmp` cleanup | `textures.c` uses libpng `setjmp(png_jmpbuf(...))`, volatile buffers, and manual frees. | Keep the C-style cleanup region. Never place C++ objects with destructors across the jump boundary. Test corrupted/truncated PNG paths. |
| P1 | C designated initializers | `dialogs.c` contains 726 member-designator expressions and passes strict target GNU++17 unchanged; `guigame.c:599-615` uses indexed array designators and fails identically under GNU++17 and GNU++20. | Use GNU++17 for parity. Rewrite only the indexed button map mechanically, preserve its 17-entry truth table byte-for-byte, and test config/UI mapping. C++20 is not a solution on GCC 15.2.0. |
| P1 | Variable-length array | `elfldr_noreset.c` creates `char *new_argv[argc+1]`. | Either explicitly permit the target compiler's GNU C++ VLA extension or replace it with a bounded allocation while proving stack/argv behavior. |
| P1 | Implicit `void *` allocation conversions | Dozens of `malloc`/`calloc`/`realloc` assignments rely on C implicit conversion. | Add explicit, audited casts or a C-compatible typed helper. Preserve allocation domain and failure handling. |
| P1 | String literal mutability | `hddsupport.c` has `static char *hddPrefix = "pfs0:"`; `guigame.c` has a mutable `char *` version-string table. | Change only after call-site audit to `const char *`; do not const-cast into SDK APIs. |
| P1 | `goto` crossing initialization | About 47 labels/goto sites occur in UI, storage, tar, and system code. | Compile each TU; hoist declarations or add scopes only when required, preserving cleanup order and return codes. |
| P1 | C tentative definitions become C++ redefinitions | Six support files have an early `static item_list_t name;` followed by a later initialized definition; `sound.c` similarly redeclares three header-defined `ov_callbacks`. | Move each single initialized definition to a legal position with exact prototypes, preserving internal linkage and static initialization. Do not replace it with a function-local static, constructor, or runtime assignment. Remove/replace the Vorbis warning suppression without duplicating objects. |
| P1 | Inline MIPS/VU assembly | `elfldr_noreset.c`, `util.c`, `gui.c`, and `system.c` contain inline assembly. | Keep constraints, clobbers, alignment, and volatile semantics. Validate with the PS2 compiler, never a host-only substitute. |
| P1 | Thread/RPC/file I/O callback ABI | Extensive `volatile`, SIF, fileXio, open/close, and callback usage. | Preserve exact function pointer types and lifetimes. Review every callback crossing C/C++ object boundaries. |
| P1 | Embedded included C implementations | `lz4.c` and `zso.c` include implementation bodies from `modules/isofs/`. | Compile and test these as their own language-boundary spike; do not edit module code under strict scope. |
| P1 | Handoff and IOP reset behavior | `elfldr_noreset.c` calls `sceSifExitRpc`, `FlushCache`, and `ExecPS2`; `system.c` mutates embedded IOP images. | Keep C ABI and sequence byte-for-byte unless a hardware test proves otherwise. No RAII around reset/launch resources. |
| P1 | Source encoding anomaly | `bdmsupport.c` contains one actual NUL byte in a character comparison. | Record it, then make an explicit later decision (usually textual `\\0`) before relying on C++ diagnostics or format tools. |
| P1 | Special symbols | `_gp` is declared/used in `ioman.c`, `sound.c`, and `texcache.c`. | Preserve declaration/linkage and inspect final symbol resolution. |
| P2 | C runtime drift and binary size | C++ driver/runtime can introduce `__cxa_*`, `__gxx_personality_*`, constructors, or larger sections. | Build with exceptions/RTTI disabled, reject unwanted runtime symbols, compare ELF/map/size, and enforce a size budget. |
| P2 | Over-broad “modernization” | `NULL` appears about 2,639 times; C-like APIs dominate. | Keep the initial diff mechanical. Defer `nullptr`, STL, ownership redesign, and naming changes until parity. |

## 7. Recommended target model

### Language and compiler policy

Use the target PS2 GCC/G++ used by the repository's pinned PS2SDK suites. The measured parity baseline is `-std=gnu++17` on `mips64r5900el-ps2-elf-g++` 15.2.0. Complete GNU++17 censuses under both OFFICIALPINNED and PS2DEVPINNED produced the same 19-pass/28-fail files and diagnostic counts. OFFICIALPINNED GNU++20 produced the identical set, so C++20 adds risk without removing a blocker. `dialogs.c` already passes GNU++17; only the indexed array designators in `guigame.c` need a mechanical initializer rewrite. The target compiler also accepts the existing VLA, compound-literal, and flexible-array GNU extensions once unrelated conversion errors are removed. Retaining those extensions during parity is lower risk than changing allocation or stack behavior; each retained extension still needs an explicit layout/bounds test and a later modernization decision.

The initial C++ flags should be equivalent to the existing EE C flags plus:

* `-fno-exceptions`
* `-fno-rtti`
* `-fno-threadsafe-statics`
* `-fno-use-cxa-atexit`

Do not add `-Werror` to the first release build until the warning baseline is understood. A separate validation target may promote new warnings to errors. Do not add `-fno-builtin`, aggressive aliasing changes, or optimization changes without a measured reason.

Do not use `-fpermissive` in a candidate or release build. It is useful only as a diagnostic classifier: here it converted invalid pointer/function-pointer/const conversions to warnings yet still left ten structural failures. Every final conversion must compile under the strict selected dialect, with each formerly implicit conversion reviewed at its actual ownership or callback boundary.

### Runtime architecture

Keep the existing C-shaped structs and functions. Keep `malloc`/`free`, SDK allocators, explicit cleanup, `volatile`, `setjmp`, and the existing thread model. C++ is the compilation language, not a license to change ownership or scheduling.

For generated symbols, prefer a small source-side compatibility boundary:

```cpp
#ifdef __cplusplus
extern "C" {
#endif
/* declarations for symbols defined by asm/*.c and other C objects */
#ifdef __cplusplus
}
#endif
```

If this boundary must live in `include/` or the build system, record that as the minimum scope exception. Do not duplicate dozens of declarations with inconsistent types.

### Linker and binary policy

The first C++ link must be compared with the C baseline at the object, symbol, section, ELF, and map-file levels. Using `EE_CXX` as the final linker driver is not automatically harmless; it must be selected only if the resulting link has no unwanted runtime objects and identical SDK/IRX resolution. If the C driver can link C++ objects with the necessary libraries, that may be the lower-risk final arrangement.

## 8. Staged implementation plan

### Phase 0 — Freeze the baseline

1. Create a full clean clone from the agreed branch/commit at a fixed build path. A linked worktree is fine for analysis, but the current Makefile brands it dirty and therefore must not be the sole provenance source for a hardware artifact.
2. Record `git status --porcelain=v2`, unstaged and staged diffs, untracked files, `HEAD`, tree SHA, remotes, and submodule/dependency state. Build only committed candidate waves; never rely on the in-ELF short hash to identify staged or untracked `.cpp` content.
3. Resolve and inspect the exact PS2DEVPINNED and OFFICIALPINNED image digests. Verify fresh pullability, compiler/SDK/gsKit versions, stock IRX hashes, the coherent-MMCE input commit/patches/output hash, and the explicit EE/IOP `PATH`. Do not run CRLF host scripts directly in Linux.
4. Establish a shared C/C++ front-end flag definition. Compare complete C and C++ commands and preprocessor macro dumps for all EE-affecting configurations: release, OPLDIAG, PADEMU, RTL/IGS (including `EXTRA_FEATURES`), GSM1080P, DTL-T10000, and every debug mode. Separately prove module/generator inputs such as DUALSENSE remain identical. A C++ object without the same feature/version macros—or an artifact with different embedded IRX inputs—is a wrong build even if it links.
5. Correct and regression-test `EE_DEPS`, or formally declare all incremental results untrusted and clean-build every gate. Prove the dependency test by changing a transitive header in a disposable tree and observing exactly the expected object rebuild.
6. Run the unmodified C baseline from a clean object directory for all five `.github/workflows/flavours.yml` flavors and the current extras matrix (eight PADEMU/EXTRA_FEATURES/DUALSENSE variants plus six debug configurations for each ps2dev lineage).
7. Capture full and normalized/stripped ELF hashes, file/loadable/debug section sizes, maps, objects, symbols, relocations, generated-file hashes, compiler commands, stack-usage files, SDK/IRX manifests, and the fixed absolute build path.
8. Create a **known-baseline-hazard ledger** before conversion. Instrument, but do not silently fix, pre-existing races/error-classification/timing hazards. If a hazard prevents a trustworthy hardware control, stabilize it in a separate C-only commit, prove that commit on hardware, and rebaseline before conversion.
9. Record hardware-good C control artifacts and the console/device/controller matrix before any source rename.

**Gate:** no conversion starts until both pinned baselines link from committed clean trees, same-path rebuilds are deterministic or all path-dependent bytes are explained, macro/flag parity is proved, dependency policy is explicit, and at least one exact C artifact is hardware-good. If any condition is missing, report the narrower environment/build/hardware blocker; do not label it a C++ failure.

### Phase 1 — Build-plumbing spike

Establish the intentional mixed-language ABI boundary before converting the first leaf. `nbns.c` includes a project header with no `extern "C"` guard; compiling it unchanged as GNU++17 produced `_Z8nbnsInitv`, `_Z10nbnsDeinitv`, and `_Z12nbnsFindNamePKcPh` instead of the C names. Use a source-local wrapper or the explicitly approved minimum header guard, then convert `nbns.c` or `httpclient.c` in an isolated branch and link it with untouched C/generated objects.

For `nbns.c`, capture `nm`, `size`, `readelf`, function-section hashes, disassembly, and `-fstack-usage`. The measured unchanged C and C++ objects both totaled 456 bytes and had no C++ runtime symbols, but `nbnsFindName` changed bytes and its stack estimate grew from 48 to 64 bytes; `nbnsInit` remained 32 and `nbnsDeinit` remained 0. Similar total size is therefore not sufficient parity evidence.

**Gate:** the spike must compile strictly as GNU++17, link, preserve every allowlisted C symbol, introduce no runtime support, retain protocol bytes/alignment, and have every code/stack delta reviewed. If it needs broad header/build changes, stop and obtain the scope decision before converting more files.

### Phase 2 — Leaf wave

Convert the files in the first inventory group. For `lz4.c` and `zso.c`, separately record whether included module implementations compile cleanly as C++. Keep the generated `asm/*.c` files in C unless an explicit generated-source decision is made.

**Gate:** clean rebuild with the leaf wave, static symbol comparison, no unexpected C++ runtime symbols, and focused parser/storage tests.

### Phase 3 — Data and service wave

Convert language/config/util/I/O/support/archive/VMC/cheat/application/favorites files. Fix allocation casts and literal constness mechanically. Preserve config keys, save timing, path casing, error codes, and cache invalidation semantics.

**Gate:** configuration round trips, language/theme lookup, archive/VMC/cheat parsing, and storage enumeration remain byte- and behavior-compatible.

### Phase 4 — Storage, network, and handoff wave

Convert BDM, HDD, Ethernet/SMB, UDPFS, MMCE, VCD, then handoff/system code. Review every thread start/stop, callback, mount/unmount, retry, and error path. Treat `elfldr_noreset.c` and `system.c` as separate approval points even if they compile cleanly.

**Gate:** all launch-adjacent static checks pass; exact artifacts are tested on the relevant devices before proceeding to final UI conversion.

### Phase 5 — Rendering/input/audio/UI wave

Convert low-level renderer/font/texture/cache/theme/sound/pad files, then menu/dialog/game UI. Convert `gui.c` and `opl.c` last because they have the highest fan-in and orchestration risk. Keep the VU assembly and `setjmp` cleanup unchanged except for syntax-required declaration movement.

**Gate:** menu navigation, drawing, art, fonts, themes, audio, rumble, controller modes, and settings behavior match the baseline.

### Phase 6 — Generated sources and permanent build policy

Decide whether generated `src/lang_internal.c` stays a C object, is generated as `.cpp`, or is compiled with an explicit language override. Make the permanent build rule self-describing, reproducible, and compatible with clean clones. Verify that generated files are not accidentally committed or omitted from releases.

### Phase 7 — Release and hardware parity

Build every supported release flavor from a clean clone. Test exact artifacts on controlled hardware. For regressions, use an A/B artifact pair with the same media/configuration and identify the first failing stage before assigning causality.

## 9. Per-translation-unit checklist

For every file:

1. Record the pre-conversion hash and object size.
2. Convert only the extension/build selection or the minimum syntax required by the compiler.
3. Preserve all public names, storage classes, attributes, section placement, and function signatures.
4. Fix C++ errors mechanically under strict GNU++17: explicit allocation casts only after confirming the pointee/allocator domain, `const char *` where the full call chain is read-only, declaration scopes for `goto`, and exact function-pointer representation. Never use `-fpermissive` to make a file green.
5. Do not replace `NULL`, add containers, or restructure ownership during this pass.
6. Check every SDK callback, thread entry point, `extern`, generated symbol, and function pointer crossing a C/C++ boundary.
7. For affected data, compare `sizeof`, `alignof`, `offsetof`, packed bytes, and bitfield masks against the baseline.
8. Compile with the real MIPS target compiler and link the smallest meaningful image.
9. Compare `nm`, `readelf`, map, section sizes, function-section hashes/disassembly, `-fstack-usage`, and both full and normalized/stripped ELF hashes. Normalize or fix build paths before calling a full-hash delta semantic.
10. Run the file's focused behavior test before moving it into the next wave.
11. Commit one coherent cluster only after its gate passes; keep unrelated edits out of the commit.

## 10. Validation matrix

### Source and build validation

* Clean checkout and clean object directory before each release build.
* Compile converted EE units as C++; compile untouched/generated C units as C.
* Use one reviewed shared flag set for both languages. Save the full compile database/log and compare `-dM -E` macro dumps for every supported flavor; specifically prove `OPL_VERSION` and every feature/debug macro.
* Strict GNU++17 only for the parity build; no `-fpermissive`. GNU++20 is not an accepted workaround for `guigame.c`.
* `git diff --check` and the repository's `make format-check` where available.
* No accidental source NUL bytes; explicitly resolve the existing `bdmsupport.c` anomaly.
* No accidental `throw`, `new`, `delete`, STL, RTTI, exception, constructor, or C++ runtime dependency in the parity build.
* No unresolved or newly mangled names for generated assets, SDK APIs, IRX imports, `_gp`, or C callbacks.
* Until `EE_DEPS` is corrected and proven, reject incremental-build evidence. After correction, touch representative public/generated headers and prove all and only dependent C/C++ objects rebuild.
* Record `git status --porcelain=v2`, `git diff`, `git diff --cached`, untracked paths, commit/tree, image digest, fixed build path, SDK/IRX manifest, and artifact hash together. Do not infer provenance from `OPL_VERSION` alone.

### Binary and ABI validation

Compare C baseline and C++ candidate using:

* `nm`/demangled symbol lists, with special attention to entry points and generated arrays.
* `readelf -h -S -s`, `size`, and linker map files.
* ELF SHA-256, file size, section sizes, alignment, and stack/heap reservations.
* Per-function stack-usage output and targeted disassembly/function-section hashes for worker, RPC, DMA, patching, and handoff paths. The measured `nbnsFindName` delta proves equal object size can hide stack/code changes.
* `sizeof`, `alignof`, and `offsetof` probes for `UIItem`, controller reports, packed network/storage records, configuration structures, and any binary-on-disk/on-wire data.
* Embedded array addresses and sizes for ELF/IRX, IOPRP, fonts, PNGs, themes, audio, and icons.

Exact binary equality is not required for C++ compilation, but every difference must be explained. Compare full hashes only under a fixed/normalized build path; also compare stripped or loadable-section hashes so `DW_AT_comp_dir` noise cannot hide or invent a semantic delta. An unexplained size, section, symbol, stack, code, relocation, or layout delta is a stop condition.

### Runtime and hardware validation

Use the exact candidate artifact whose commit and build manifest are recorded. At minimum, cover:

* Boot from memory card, USB, HDD/APA/PFS, Ethernet/SMB, MMCE, iLink, and MX4SIO where supported by the hardware matrix.
* Game discovery, art/index rebuild and retry, language, theme, fonts, settings save/reload, favorites, VMC, cheats, TAR/archive paths, and OSD history.
* Native DualShock/DS3/DS4/DS5 input, rumble, PADEMU, and controller edge cases.
* SMB/network reconnect, UDPFS/UDPBD behavior, BDM recovery, HDD retry/error paths, and exFAT-specific media where applicable.
* OPL, Neutrino, and POPSTARTER launch paths, including embedded child-loader handoff, keep-IOP behavior, IOP reset sequencing, and return paths.
* Audio, GSM/1080p, texture/art loading, cache pressure, and long-session stability.
* Repeated BDM-generation changes, hotplug/removal/reorder, all populated and empty `massN:` pages, queue rejection, and transient directory/open failures while tracing generation consumption, enqueue acceptance, list/hint rebuilds, art fail epochs, and frame/input watchdogs.
* The pre-existing hint-list concurrency mechanism: delay `moduleUpdateMenuInternal()` while the GUI draws hints, but attribute results against the C control. If the plasma/frame loop stops, investigate a GUI-thread stall; if animation continues while input dies, treat pad/IOP/SIO2 as a separate hypothesis. These are discriminators, not proof of cause.
* Art absence versus transient failure: inject genuine ENOENT/FR_NO_FILE/FR_NO_PATH and retryable MMCE/BDM/SMB/PFS faults. Prove only true absence parks a row, transient errors retry, generation invalidation revives stale misses, and device/list changes preserve last-good data where intended.

Static/CI green is not console proof. Hardware results must name the exact ELF SHA, source commit, build run, media/configuration, console/device, and observed result.

## 11. Stop criteria and rollback

Stop the migration and preserve the last known-good commit if any of the following occurs:

* Baseline or converted builds cannot be reproduced from a clean checkout.
* A generated/SDK/IRX symbol becomes mangled, unresolved, duplicated, or points at a different ABI.
* A structure size, offset, packing, bitfield mask, or embedded asset changes without an approved explanation.
* C++ runtime support, global constructors, exception tables, or code/data growth exceeds the approved budget.
* Thread, RPC, mount, cache, controller, launch, or IOP sequencing changes.
* A hardware test regresses, even if static checks and CI remain green.
* The next fix would require changing modules, headers, generated sources, build files, or runtime architecture beyond the explicit scope decision.

Use one commit per conversion wave and preserve the original C branch/tag. Roll back by reverting the last wave, not by resetting a shared or dirty user checkout. Keep failed candidate ELFs and logs for diagnosis; do not overwrite the baseline artifact.

## 12. Acceptance criteria

The later task is complete only when all of these are true:

1. The exact source scope and any approved build/header exception are documented.
2. A clean checkout builds all required flavors with the target PS2 toolchain.
3. Converted `src/` units compile as C++ with no unintended C++ runtime dependency.
4. Symbol/linkage, structure layout, embedded assets, sections, and resource budgets have been compared to the C baseline.
5. Static checks and focused tests pass.
6. The hardware matrix passes with exact artifact provenance.
7. The final diff contains no unrelated refactor or generated-file churn.
8. The handoff report distinguishes source evidence, CI/build evidence, and console evidence; it does not say “works” based solely on a host compile.

## 13. Decisions required before implementation

The future operator should obtain explicit answers to these questions before Phase 1:

1. Is the minimum root `Makefile` exception authorized for a C++ source rule, a shared C/C++ flag definition, correct `.d` dependency inclusion, and truthful provenance? Without at least the rule/flag work, a faithful permanent conversion is not implementable.
2. Are minimal `include/` linkage guards allowed if source-only wrappers are insufficient?
3. Do generated `asm/*.c` definitions and generated `src/lang_internal.c` count as outside the requested `src/` scope?
4. Is the measured GNU++17/GCC 15.2.0 baseline approved, including temporary retention of the target compiler's VLA/compound-literal/flexible-array GNU extensions? GNU++20 is not justified by the current compiler results.
5. What maximum ELF/section/heap/stack growth is acceptable?
6. Must the implementation compile all five workflow flavors plus the eight variant and six debug configurations currently emitted per ps2dev lineage, or may a formally justified covering matrix replace any combinations?
7. Which consoles, adapters, storage devices, network modes, and controllers are available for A/B testing?
8. What branch, commit, PR, and review policy should be used? No merge should occur without authorization.

## 14. Immediate pickup commands

Run these first in the future execution turn, replacing no paths silently:

```powershell
$repo = 'C:\Users\natha\Github\CPLUSPLUS\Open-PS2-Loader'
Set-Location $repo
git -c safe.directory=$repo status --short --branch
git -c safe.directory=$repo status --porcelain=v2 --untracked-files=all
git -c safe.directory=$repo rev-parse HEAD
git -c safe.directory=$repo rev-parse 'HEAD^{tree}'
git -c safe.directory=$repo diff --name-status
git -c safe.directory=$repo diff --cached --name-status
git -c safe.directory=$repo worktree list
git -c safe.directory=$repo ls-files --eol .github/scripts Makefile
rg --files src -g '*.c' | Sort-Object
rg -n 'FRONTEND_OBJS|EE_SRC_DIR|EE_CFLAGS|EE_CXXFLAGS|EE_DEPS|%.o:.*%.c|lang_internal|Makefile.eeglobal|CODE_ANCHOR|GIT_HASH|DIRTY' Makefile include src
```

Then resolve both pinned images, set the full EE/IOP toolchain `PATH`, clone into a fixed LF-only container path, install the repository's coherent MMCE input exactly as CI does, and capture the unmodified baseline. Do not rename or edit source files merely to “see whether it compiles”; the baseline, flag/dependency proof, and known-hazard ledger are required controls.

## 15. Future operator prompt

The following prompt is intentionally self-contained. Paste it into a later implementation task after the decisions in Section 13 are resolved:

> You are implementing a controlled C-to-C++ compatibility migration for the main EE sources of Open PS2 Loader.
>
> Repository: `https://github.com/NathanNeurotic/Open-PS2-Loader.git`  
> Working directory: `C:\Users\natha\Github\CPLUSPLUS\Open-PS2-Loader`  
> Baseline commit: `679dc31efc9dc290be405b5efbee5e4d979da79d` (replace only if the owner explicitly approves a newer snapshot)  
> Baseline branch: `rebuild/main`  
> Requested source scope: main EE translation units under `src/` only.  
> Current task mode: implementation is authorized; do not broaden scope without an explicit decision.
>
> Mission: convert the agreed `src/` C translation units to C++ compilation while preserving behavior, ABI, binary data layout, resource usage, thread/RPC/file I/O behavior, and PS2 hardware behavior. This is a parity migration, not a redesign.
>
> Hard constraints:
>
> * Start read-only. Record `git status --short --branch`, `git rev-parse HEAD`, tree SHA, worktrees, toolchain versions, and the exact build environment.
> * Work in a fresh isolated checkout. Use a full clean clone at a fixed LF-only path for artifact provenance; the current Makefile mislabels linked worktrees and debug information embeds the checkout path. Do not modify a dirty user checkout or use destructive reset/checkout commands.
> * Establish and archive unmodified baselines with both pinned SDK images before converting any file. Record full/staged/untracked status, commit/tree, image/SDK/IRX manifests, full and normalized hashes, maps/sections/symbols, stack usage, and exact commands. If the environment or hardware control is missing, report that narrower blocker.
> * Convert only the approved EE `src/` files. Do not edit modules, IOP sources, `ee_core`, `elfldr`, generated `asm/*.c`, or headers/build files unless the owner has explicitly approved the minimum exception needed to integrate C++.
> * Do not add STL, exceptions, RTTI, `new`/`delete`, smart pointers, global constructors, namespaces, or ownership redesign during parity.
> * Preserve symbol names, C linkage, structure packing/bitfields, callback signatures, inline assembly, `setjmp`/`longjmp` cleanup, manual allocation/free, thread lifetimes, IOP reset/keep-IOP sequencing, and configuration/save semantics.
>
> Required workflow:
>
> 1. Freeze baseline ELF(s), map files, object list, symbol list, section sizes, hashes, and hardware control artifact.
> 2. Resolve the build boundary: either a permanent `.cpp` rule or an explicit C++ language override for `.c` files. Define one shared C/C++ flag set, prove macro parity for every flavor, fix/test the currently empty `EE_DEPS`, and give generated C assets plus `src/lang_internal.c` an intentional policy.
> 3. Establish the approved `extern "C"` boundary before converting one leaf spike (`nbns.c` or `httpclient.c`). Unchanged C++ `nbns.c` mangles all three public symbols. Link the corrected spike with untouched C/generated objects and inspect symbols, runtime dependencies, function bytes, and stack usage before continuing.
> 4. Convert in waves: leaf/format, data/services, storage/network/handoff, then rendering/input/audio/UI, with `opl.c` and `gui.c` last.
> 5. For each file, make only compiler-required mechanical changes. Explicitly audit allocations, string literal constness, function pointers, `goto` scopes, VLA use, attributes, `_gp`, generated symbols, and included C implementations.
> 6. After every wave, clean-build, run `git diff --check`, compare symbols/sections/size/layout, and run focused tests. Keep one commit per wave.
> 7. Build every approved release flavor from a clean checkout. Reject unexpected C++ runtime symbols or unexplained resource/layout changes.
> 8. Run the controlled PS2 hardware matrix: all approved boot/storage/network paths, settings/language/theme/art, VMC/cheats/TAR, controller/rumble/PADEMU, audio/GSM, BDM/HDD/SMB/UDPFS recovery, OPL/Neutrino/POPSTARTER launches, IOP reset/keep-IOP behavior, and return paths.
>
> Use strict GNU++17 on the measured target compiler; never use `-fpermissive`, and do not switch to GNU++20 because it removes none of the measured blockers. Stop and ask for direction if a change requires broad header/module/build redesign, if a symbol/layout/stack/code/resource delta is unexplained, or if hardware regresses. Never convert a failed hardware result into a source fix by guesswork.
>
> Final response must include: exact branch and commit, scope and exceptions, files converted, diffstat, build commands and toolchain, all validation results, ELF/map/symbol/layout/resource comparisons, exact artifact hashes/manifests, hardware console/device matrix, known limitations, rollback point, and a clear separation of source proof, build/CI proof, and console proof. Do not claim “works” unless the required gates actually passed.

## 16. Handoff summary

The next useful action is not a blind rename. It is to archive fixed-path, hardware-good C controls; approve the minimum build/linkage exception; unify C/C++ flags; repair or quarantine dependency tracking; and freeze the known-baseline-hazard ledger. Only then should a leaf spike establish the intended mixed-language ABI. The rest of the migration should proceed in small, reversible waves with compiler, binary, runtime-attribution, and hardware evidence at each gate.

## 17. Second-pass runtime audit: failures a successful build will not expose

This pass looked specifically for contracts that can remain source-compatible and link-clean after a C-to-C++ conversion, but fail only after a delay, a callback, a render cycle, an error path, a concurrent operation, or a real PS2 device interaction. These are source-proven hazards in the baseline; they are not claims that the current C build is already failing.

### P0: deferred raw-pointer and callback lifetimes

* `include/ioman.h:29-30` defines `ioPutRequest(int type, void *data)` and explicitly says request data are not freed. `src/ioman.c:132-208,255-295` queues the raw pointer and later invokes it from the worker. `IO_CUSTOM_SIMPLEACTION` converts the payload to a function pointer and calls it; other operations dereference pointers such as `short *` in `src/opl.c:1128-1148`.
* `src/opl.c:707` documents why a local `mode` cannot be queued: it dies before the worker consumes it. `src/gui.c:4373-4421` likewise relies on file-static callback/payload storage because a timeout may abandon the wait. A C++ temporary, `std::string::c_str()`, moved container element, stack callback trampoline, or integer round-trip through `void *` can therefore compile and fail nondeterministically.
* `guiDeferUpdate()` has the same shape. `include/gui.h:22-50` stores pointers to menu/submenu/text data, while `src/gui.c:3280-3425` owns only the operation node. A copied or moved C++ owner does not extend the lifetime of those borrowed pointers.
* Dialogs retain borrowed pointers: `diaSetLabel()` (`src/dia.c:1410-1420`) and `diaSetEnum()` (`src/dia.c:1425-1437`, `include/dia.h:118-119`) require the pointed-to text/enum table to remain valid through every render. Local arrays in `src/gui.c:715-748,1135-1148` and `src/guigame.c:421-476` are safe only because the dialog is synchronous. Boot status is a separate sticky contract (`include/gui.h:209-214`, `src/gui.c:3464-3480`): dynamic text must use the copy setter, not a temporary/local buffer.

Required discriminator: delay the worker and GUI consumer, force queue-full/OOM, time out and tear down while requests remain pending, rebuild menus, change language/theme/device, and reshow each dialog. Record the payload address, owner lifetime, callback target, completion signal, and final rendered text. Any stale address, wrong mode, missed completion, or post-timeout callback is a stop condition.

The art subsystem adds a second, more complex ownership graph. `include/texcache.h:70-86` documents that an in-flight request owns pointers into `image_cache_t::content[]`, `prefix`, and `suffix`; `src/texcache.c:1519-1570` packs request strings into the same allocation; `src/texcache.c:605-627` releases the request; and `src/appsupport.c:605-612` drains/cancels art before freeing or reallocating app lists. `src/artindex.c:306-319` intentionally invalidates index slots without freeing them from an arbitrary thread. A C++ destructor, copy/move operation, or eager `vector`/string cleanup can reintroduce the exact theme-switch/app-list UAF that these waits and leaks prevent. Preserve the ownership graph, cancellation order, and “do not free cross-thread” rule; test theme/device/list rebuilds while an art read is queued, decoding, cancelled, or stuck in fileXio.

### P0: non-local error handling and resource cleanup

`src/textures.c:825,946-955` keeps a `volatile` decode buffer across libpng's `setjmp` and frees resources in the `longjmp` branch. C++ objects with destructors, RAII guards, or non-trivial temporaries whose lifetime crosses that boundary are undefined to unwind through. Keep the decode/jump region C-trivial and test corrupt, truncated, interlaced, and allocator-failure PNGs; verify no double free, leak, half-initialized texture, or locked cache remains.

### P0/P1: binary layout, aliasing, and DMA/RPC alignment

The runtime consumes C-shaped bytes directly. Packed/bitfield tables and shallow copies include `include/iosupport.h:93-177`, `src/favsupport.c:590-595` (`*view = *favArray[id].owner`), and `src/gui.c:4177-4188` (malloc plus `memset` for `gui_update_t`). Preserve standard-layout/trivially-copyable properties, `sizeof`, alignment, offsets, bit masks, and zero-initialization assumptions; do not add virtual functions, constructors, default members, `bool` substitutions, or containers to these records.

`font_t` is swapped with `memcpy` under a semaphore and then the old resource graph is freed (`src/fntsys.c:309-321`). Similar whole-record copies occur in the app/favorites paths. These are ownership transfers between C-trivial records, not C++ copy-assignment opportunities: a destructor, vtable, or non-trivial member would be skipped by `memcpy`, while a changed pointer/array layout can free the wrong font, atlas, or menu text after a later render. Keep such records trivially copyable and test reload/render overlap and teardown.

Theme font slots have an additional aliasing rule (`src/themes.c:2554-2585,2469-2472`): unspecified theme fonts copy the `FNT_DEFAULT` handle, while `thmFree()` releases every slot. Do not turn those integer handles into owning C++ objects or add deduplication/refcounting as part of the language conversion; that can double-release a shared slot or leave later elements pointing at a released face. Preserve the current sentinel/alias behavior and test theme reloads with zero, sparse, and fully populated custom-font slots.

Controller reports are explicitly packed in `include/ds34common.h:110,171,220,286,344`. A C++ rewrite that reads a packed member through a naturally aligned reference, or copies it into a class with padding, can produce an unaligned MIPS load or a shifted HID/rumble byte while still passing host tests. Compare packet bytes and exercise native controllers, PADEMU, reconnect, and rumble—not just menu navigation.

Those reports also use anonymous unions/structs to expose the same button bytes as both a word and individual fields (`include/ds34common.h:60-110,112-171,173-220,222-286,288-344`). This is intentional wire-format type punning. A C++ “cleanup” that selects one active union member, changes endianness, or replaces the anonymous members with accessors can make button masks or rumble commands wrong only for particular controller models. Preserve the byte-level representation and compare raw input/output reports.

`src/guigame.c:55-72` adds another persisted bitfield contract: `PadMacroSettings` is an anonymous union of individual bitfields and a raw `int`, and the raw word is loaded/saved through the config system (`src/guigame.c:920-947,1672-1706`). C++ bitfield allocation order, width, and anonymous-union rules are implementation-defined enough that a “clean” replacement can silently change which macro buttons, inversions, or turbo values are stored. Keep the raw word/masks stable; compare `sizeof`, raw values, and round-trip config files, then test every pad-macro toggle on native and PADEMU controllers.

Raw image/IRX/settings patching also depends on exact aliasing and byte offsets (`src/system.c:830-852,1631-1644`, `src/supportbase.c:715-730`, `src/hddsupport.c:1871-1875,1947-1949`, `src/mmcesupport.c:874`, `src/ethsupport.c:896-899`). Compare patched bytes, checksums, and launch behavior. `-fno-strict-aliasing` may be a diagnostic control, not a substitute for preserving the access contract.

Several RPC and device paths cast aligned byte storage to a protocol struct and then let the SDK read or write it (`src/hdd.c:64-125`, `src/httpclient.c:31-72`, `src/nbns.c:33-35`). That is ordinary C practice, but C++ object-lifetime and aliasing rules are stricter; a compiler can legally make assumptions that change the bytes seen by SIF/fileXio even though the casts and sizes look unchanged. Keep these buffers aligned and C-shaped, use only a target-approved byte-copy/implicit-lifetime technique, and compare complete request/reply buffers under optimized builds.

The kernel handoff also embeds pointers into MIPS instruction immediates (`src/system.c:945-946,975-976`) and performs `void *`/address arithmetic while copying ELF segments (`src/system.c:1631-1644,1734-1737`). A mechanical C++ cast to `uintptr_t`, a widened intermediate, or a changed pointer segment can patch the wrong halfword or clear the wrong address range. Compare the exact patched instructions and target addresses, flush/invalidate caches as before, and exercise every boot/EE-core handoff rather than only the ordinary game path.

`src/ioprp.c:32-60,99-115` is another binary-image generator: it walks 16-byte ROMDIR records, replaces CDVDMAN/CDVDFSV/EESYNC payloads, and fills every output gap to a 16-byte boundary. A C++ layout/alignment change in `romdir_entry`, a signed/unsigned offset change, or a different pointer-arithmetic workaround can yield an apparently valid buffer whose module offsets are wrong; the failure appears later as an IOP reset or boot hang. Keep `sizeof(romdir_entry) == 16`, field offsets, ROMDIR names, output padding, and the final image bytes/checksum as explicit gates. `src/xparam.c:79-112,225` similarly edits a fixed 30-byte XPARAM buffer at byte 12 and sends hard-coded 19/22/28-byte lengths to `SifLoadModule`; preserve the exact NUL placement, lengths, and per-title ordering, then exercise Deckard/IGR and regional-title paths.

DMA/RPC/audio/controller stacks are explicitly aligned: `src/pad.c:38`, `src/hdd.c:33,116`, `src/httpclient.c:9-10`, `src/nbns.c:9`, `src/util.c:622`, `src/tar.c:68,266`, `src/texcache.c:96`, and `src/sound.c:467-468,646-659`. C++ members, wrappers, or changed local types can silently alter alignment or stack placement. Log runtime addresses and exercise controller, audio, RPC, cache, HDD, TAR, and network paths on hardware.

### P0/P1: semaphore, interrupt, and worker scheduling order

`src/ioman.c:156-180` holds `gProcSemaId` while executing a queued callback. `src/menusys.c:404-440`, `src/texcache.c:443-509`, `src/vcdsupport.c:486-553`, and `src/sound.c:440-448` contain exact semaphore/interrupt disable-enable brackets. C++ scope cleanup or an apparently harmless helper can extend a lock across I/O, reorder `EIntr/DIntr`, signal a different waiter, or run a destructor while a semaphore is held. Stress queue-full, timeout, cancellation, teardown, menu/cache/VCD work, controller input, and audio while tracing semaphore ownership and interrupt state.

### P1: process-global tokenizer state and callback language linkage

`src/supportbase.c:1550-1586` and `src/system.c:1108-1115` use `strtok`, whose state is process-global. `src/themes.c:167-184` deliberately uses `strtok_r` because theme loading can interleave with GUI argument parsing. A C++ tokenizer refactor is unsafe without a thread/ownership proof; stress theme reload, settings editing, and Neutrino launch preparation concurrently.

The qsort callbacks (`src/artindex.c:224`, `src/hdd.c:369`, `src/vcdsupport.c:641`) and the `_io_driver` callback cast (`src/util.c:579`) require the exact C ABI and signature. Do not replace them with capturing lambdas, member-function pointers, or mismatched C++ linkage. Verify symbols/types and execute sorting and driver paths.

Audit old-style empty prototypes before changing headers. Public declarations use `fn()` in `include/gui.h:65,68-83,120,129-131,177`, `include/fntsys.h:12,15,30`, `include/config.h:232`, `include/bdmsupport.h:75-76`, `include/hddsupport.h:148`, `include/menusys.h:102-103`, and `include/opl.h:91`; definitions are a mixture of `fn()` and `fn(void)` (`src/gui.c:182,187,235,316,3696,4056,4118`, `src/fntsys.c:267,329,438`, `src/config.c:366`, `src/hddsupport.c:2249`, `src/menusys.c:646,677`, `src/opl.c:3985`). In C, an empty parameter list means “unspecified arguments”; in C++ it means “no arguments.” A hidden C caller or callback that currently passes extra state can therefore change from ABI-tolerated to a different call contract without any link failure. Inventory every declaration, definition, address-taken use, and call site; normalize only after proving the no-argument contract and then exercise menu/GUI callbacks on all entry paths.

Some alarm callbacks intentionally encode a small thread ID as a `void *` (`src/sound.c:765-770`). Preserve the target's 32-bit integer/pointer representation and decode contract exactly; do not “fix” the warning by passing the address of a temporary or by assuming a host-sized `uintptr_t` is interchangeable. Delay the alarm and exercise shutdown/restart races so the callback cannot target a stale or reused thread ID.

The cross-thread flags are deliberately plain `volatile` objects (`src/sound.c:425-436`, `src/texcache.c:98-105`, `src/artindex.c:73`, `src/gui.c:113`). They are polled by SDK threads or interrupt-adjacent code and their width/layout is part of the runtime contract. Removing `volatile`, changing an `int`/byte flag to a C++ atomic, or widening a pointer flag can alter generated loads, alignment, cache behavior, or teardown timing. Preserve the existing representation first; if a race is suspected, prove it with a target-specific memory-order experiment rather than silently introducing `std::atomic`.

### P1: stack/lifetime changes hidden by source equivalence

Three VLAs encode stack lifetime and failure behavior: `src/gui.c:3263`, `src/menusys.c:126`, and `src/elfldr_noreset.c:116`. Replacing them with heap, static, or vector storage changes bounds handling, aliasing, failure paths, and (for `ExecPS2`) the validity window of argv pointers. Exercise empty/large keyboard input, long rename strings, long argv lists, VMC/launch paths, and collect stack high-water marks.

`src/vcdsupport.c:250-301` uses a true flexible array member (`char name[]`) and allocates `sizeof(vcd_id_memo_t) + len + 1`, then frees the linked memo list during invalidation (`src/vcdsupport.c:317-325`). Standard C++ has no flexible array member. Replacing it with `char *name`, a one-element array, or a `std::string` changes the allocation formula, `offsetof(name)`, ownership, cache identity, and invalidation behavior. Preserve the trailing-byte layout with an explicitly audited allocation/offset strategy and stress repeated VCD scans, duplicate names, device changes, memo invalidation, and OOM.

The target is resource-constrained. C++ exceptions, RTTI, thread-safe static guards, `__cxa_atexit`, larger prologues/spills, and hidden constructors can fail only under memory pressure or long sessions. Keep the approved freestanding flags, reject unexpected `__cxa_*`, personality/unwind, guard, or global-constructor symbols, and compare EE/IOP stack, heap, section, and relocation budgets.

### P1: generated data and linkage that only fail at runtime

`include/renderman.h:61-65` declares file-scope color objects defined in `src/renderman.c:82-88`; C++ namespace-scope `const` can become internal if the external declaration is not visible first. Generated symbols in `include/extern_irx.h:4-6` and `include/bdma_embed.h:9-16` must remain unmangled. A private duplicate or changed address can leave a build green while consumers read the wrong table or embedded image. Check `nm`/map symbols, addresses, sizes, contents, and generated checksums, then exercise rendering, IRX loading, and embedded-asset launch paths.

The generated language table is also an ownership contract: `include/lang.h:9` declares writable `char *internalEnglish[]`, while `src/lang.c:64-79,136-146` mixes heap-owned translations with pointers back into that generated table and `src/lang.c:24-35,214-221` frees only the file-owned prefix before switching languages. Do not constify or copy the generated table just to satisfy C++ diagnostics without auditing `lngFreeFromFile`; a changed pointer identity or entry count can free fallback text, leak a translation, or leave a language dialog pointing at reclaimed names. Test partial language files, language switches, OOM, font reload, and `lngEnd()`.

`src/zso.c:1-9` text-includes `modules/isofs/zso.c`; its `ziso_*` globals and functions are declared by `modules/isofs/zso.h:31-46` and called from `src/supportbase.c:647-695`, `src/hddsupport.c:1947-1950`, and `src/util.c:646-650`. The header has no shared C-linkage wrapper. A partial C++ conversion can therefore produce mangled/mismatched entry points (an obvious link failure) or tempt a workaround that creates duplicate/private copies of the ZSO index/cache state, which is a runtime-corruption risk. Keep exactly one storage owner and one audited C ABI boundary; test ZSO/CSO block reads, cache rollover, and HDD/USB/network transports under repeated opens and seeks.

### P0: launch/handoff ordering and callback-stack lifetime

Launch paths are temporal protocols, not ordinary function calls. `include/system.h:26-47` describes the keep-IOP/quickboot contract; `src/system.c:1363-1379,1513-1519` performs the two-stage handoff, while `src/system.c:1423,1436` deliberately keeps GSM/ELF argument strings in static storage so `argv` remains valid after the caller returns. POPSTARTER has a different teardown and exception path (`src/system.c:1522-1542`). Do not replace these statics with automatic C++ strings, temporaries, or containers whose move/destructor timing differs.

Storage/network launchers explicitly quiesce asynchronous work before destroying or remounting resources: MMCE drains art before launch (`src/mmcesupport.c:720-792`), HDD refuses a PFS remount unless art and I/O quiesce (`src/hddsupport.c:1548-1624`), and Ethernet/UDPFS copy game/argv/VMC data before deinitialization (`src/ethsupport.c:993-995`, `src/udpfssupport.c:304-351`). VCD/MMCE handoff code also closes descriptors, sends the command, and waits for the settle interval (`src/mmcesupport.c:812-833,969-1041`). A C++ destructor, moved buffer, or reordered scope exit can turn a clean build into a pre-launch UAF, stale mount, or child-loader hang. Validate every OPL, Neutrino, and POPSTARTER path with delayed art/file-I/O completion and capture the last stage reached before `ExecPS2`/reset.

`src/ethsupport.c:182-200` contains a narrower callback-stack contract: `WaitValidNetState()` passes the address of its local `SemaID` to an alarm callback, then immediately waits on that semaphore. That is safe only while the current synchronous protocol is preserved. Any C++ timeout helper, cancellation path, scope guard, or altered alarm scheduling that lets the function return before the callback fires creates a delayed write through a dead stack address. Force alarm delay, retry, cancellation, and shutdown during network bring-up; record callback timing relative to semaphore creation/deletion.

### Runtime validation matrix to add to the implementation handoff

1. Queue every `IO_CUSTOM_SIMPLEACTION`, `IO_MENU_UPDATE_DEFFERED`, `IO_CACHE_LOAD_ART`, and `IO_COMPAT_UPDATE_DEFFERED` path while the worker is deliberately delayed; include queue-full/OOM, timeout, cancellation, and shutdown.
2. Open and reshow dialogs after language, theme, media, and device changes; prove borrowed labels/enums survive the last frame and sticky boot status remains valid.
3. Exercise keyboard/rename bounds, corrupt/truncated/interlaced PNGs, long argv, VMC, and cache pressure with stack high-water and EE heap measurements.
4. Run concurrent theme reload/menu rebuild/art cancellation/settings save/audio/controller activity while tracing semaphore and interrupt brackets.
5. Compare `sizeof`/`alignof`/`offsetof` (including pointer, `long`, enum, and bitfield-bearing records), callback types, packet/packed bytes, patched bytes, generated-asset checksums, symbols, section sizes, and runtime buffer addresses.
6. Exercise OPL, Neutrino, and POPSTARTER launch handoffs while art/file-I/O workers are delayed; verify static argv lifetimes, quiesce-before-free/remount ordering, descriptor settle waits, IOP reset/keep-IOP behavior, and return paths.
7. Delay `WaitValidNetState()` alarms across retries and teardown; prove no callback can signal a deleted semaphore or write through a dead stack address.
8. Exercise optimized RPC/device calls using the aligned byte-backed buffers and compare complete request/reply bytes; run MIPS address-patching, EE-core copy, IOPRP reconstruction, and XPARAM paths and compare instruction/image words, target addresses, fixed lengths, and cache-sync behavior.
9. Load partial/invalid language files, switch languages repeatedly under OOM and font reload, and end the language subsystem; run ZSO/CSO reads and seeks across HDD/USB/network opens while checking one shared cache state.
10. For every console result record exact source SHA, build manifest/ELF hash, console, adapter/media/configuration, observed stage, and result. A green compile/link/CI run is not runtime or hardware proof.

## 18. Third-pass adversarial audit: measured compiler baseline, build traps, and splash-damage controls

This pass rechecked the earlier plan against the exact target compilers and current source rather than treating plausible C++ hazards as facts. It also looked for ways a green conversion could ship the wrong features, use stale objects, misidentify its source, or amplify a pre-existing runtime race. The results below supersede earlier speculative statements where they conflict.

### 18.1 Claim audit

| Earlier assumption or claim | Result | Evidence and consequence |
|---|---|---|
| No usable target C++ compiler/build environment was available. | **Refuted.** | Both repository-pinned local images contain GCC/G++ 15.2.0, PS2SDK, ports, and gsKit. Both built the untouched C snapshot after the CI MMCE preparation step. |
| The untouched source might require GNU++20 because of designated initializers. | **Refuted.** | Strict GNU++17 and GNU++20 produced exactly the same 19-pass/28-fail set. `dialogs.c` passes GNU++17; `guigame.c` indexed array designators fail in both. Use GNU++17 plus one mechanical map rewrite. |
| `nbns.c` can be the first mixed-language leaf without prior ABI work. | **Refuted.** | It parses cleanly, but unchanged C++ compilation mangles all three public names. Establish C linkage first. |
| Similar object/ELF size is enough to establish parity. | **Refuted.** | C and C++ `nbns.o` both total 456 bytes, but `nbnsFindName` has different bytes and grows from a 48-byte to a 64-byte measured stack frame. |
| A full ELF hash is comparable across clean container paths. | **Refuted.** | `DW_AT_comp_dir` changes debug bytes. Different-path `nbns.o` hashes became identical after `strip -g`; same-path complete rebuilds were byte-identical. |
| Generated `.d` files protect incremental conversion builds. | **Refuted.** | The current make expression yields zero dependency words for 234 EE object words. Clean builds are mandatory until fixed. |
| The in-app version string uniquely proves candidate source. | **Refuted.** | Staged/untracked files evade the dirty check, while a linked worktree is always marked dirty. Commit/tree/status/build-manifest proof is mandatory. |
| A successful C++ syntax/build audit proves runtime or console safety. | **Not established.** | No candidate source exists and no console test was performed. Runtime mechanisms below remain source-proven risks or hypotheses, not hardware outcomes. |

### 18.2 Exact target-G++ syntax census

The audit generated `src/lang_internal.c` through the repository's normal `download_lng`/`languages` prerequisites, then parsed all 47 main translation units with the pinned `mips64r5900el-ps2-elf-g++` 15.2.0, each image's SDK headers, the same includes/feature macros as the baseline, optimization enabled, and `-fsyntax-only`. GNU++17 was run independently under both pinned SDK lineages; GNU++20 was run under OFFICIALPINNED to test whether it removed the designated-initializer blocker. No host compiler result is used as target proof.

| Pin/dialect | Strict pass | Strict fail | Result |
|---|---:|---:|---|
| OFFICIALPINNED `gnu++17` | 19 | 28 | Selected parity baseline. |
| PS2DEVPINNED `gnu++17` | 19 | 28 | Same files and all diagnostic category counts. |
| OFFICIALPINNED `gnu++20` | 19 | 28 | No blocker removed; not justified. |

Strict passes in both dialects:

`OSDHistory.c`, `artindex.c`, `debug.c`, `dialogs.c`, `elfldr_noreset.c`, `folderbrowse.c`, `gsm.c`, `httpclient.c`, `ioprp.c`, `lang.c`, generated `lang_internal.c`, `lz4.c`, `nbns.c`, `pad.c`, `ps2cnf.c`, `renderman.c`, `retrogem.c`, `vmc_groups.c`, and `xparam.c`.

Strict failures in both dialects:

`appsupport.c`, `atlas.c`, `bdmsupport.c`, `cheatman.c`, `config.c`, `dia.c`, `ethsupport.c`, `favsupport.c`, `fntsys.c`, `gui.c`, `guigame.c`, `hdd.c`, `hddsupport.c`, `ioman.c`, `menusys.c`, `mmcesupport.c`, `opl.c`, `sound.c`, `supportbase.c`, `system.c`, `tar.c`, `texcache.c`, `textures.c`, `themes.c`, `udpfssupport.c`, `util.c`, `vcdsupport.c`, and `zso.c`.

The GNU++17 run emitted 140 `error:` lines:

| Diagnostic class | Count | Important examples/treatment |
|---|---:|---|
| Invalid conversions | 125 | Includes 28 `void * -> item_list_t *`, 21 `void (*)() -> void *`, 9 `void * -> void **`, 9 `void * -> char *`, and many smaller ownership/callback cases. Use typed casts only after proving representation, owner, lifetime, and allocator domain. Centralize the function-pointer/object-pointer bridge and assert the target representation; do not scatter warning-silencing casts. |
| Redefinitions | 9 | Six support-list objects plus three Vorbis callback objects; see structural blockers below. |
| Jump-to-label across initialization | 3 | `bdmsupport.c` and `tar.c`; preserve the existing fail cleanup and return values when moving declarations/scopes. |
| Fixed array initialized by same-length string | 3 | `src/dia.c:64` (`char[2] = "\\0\\0"`), `modules/iopcore/common/cdvd_config.h:108` as expanded by `src/supportbase.c:715` (`u8[5] = "DSKID"`), and `src/util.c:555` (`char[16] = "0123456789ABCDEF"`). Use explicit byte/character lists in `src/`, preserving exact array sizes and the intentionally absent terminator. Do not edit the module header merely to fix the source-side sample. |

GCC also printed 16 `sorry, unimplemented: non-trivial designated initializers not supported` messages for `src/guigame.c:599-615`. The current enum values prove the equivalent sequential 17-byte initializer is:

```text
{ 16, 13, 14, 15, 1, 4, 2, 3, 7, 8, 5, 6, 12, 10, 9, 11, 0 }
```

Do not paste that sequence without rechecking the enum at the implementation commit. Add a target or host truth-table probe for every `BtnBit_Off`/`DS2BtnBit_*` index and verify pad-macro dialog round trips.

At least six invalid conversions discard constness and need call-chain review rather than a cast: `atlas.c:164`, `bdmsupport.c:240,1973`, `ethsupport.c:733`, `favsupport.c:616`, and `system.c:1658`.

`-fpermissive` was used only to classify what remained after warning-grade conversions were demoted. It passed 37 units and still failed these ten:

* `appsupport.c:36,956`, `ethsupport.c:57,1093`, `favsupport.c:34,975`, `hddsupport.c:200,2272`, `mmcesupport.c:88,1171`, and `udpfssupport.c:38,422`: a C tentative internal definition followed by a later initialized definition becomes a C++ redefinition. Move the one initialized definition to a legal point and add exact function prototypes as needed; preserve internal linkage and static initialization.
* `bdmsupport.c:1754-1829`: fail jumps cross locals initialized around `1768-1770`, including Neutrino arguments and VMC/path buffers. Hoist or scope without moving teardown, `deinitEx`, or the `sysLaunchNeutrino` lifetime boundary.
* `tar.c:261-376`: the early fail jump crosses `nameMax` at `263`. Preserve the concatenated-TAR loop, archive index ownership, and single fail cleanup.
* `guigame.c:599-615`: indexed array designators; use the verified sequential byte map above.
* `sound.c:18-20`: the included Vorbis header already defines the three `OV_CALLBACKS_*` static objects. The C redeclarations merely attach an unused attribute; C++ sees redefinitions. Replace the warning suppression without defining another object.

The permissive result is diagnostic evidence, not permission to ship permissive code. It masks precisely the callback, ownership, and constness boundaries most likely to fail later at runtime.

### 18.3 Baseline build witnesses and their limits

Both disposable builds used a clean `git clone --no-local` of commit `679dc31efc9dc290be405b5efbee5e4d979da79d` / tree `fb38df9d7f650b6ce6104d96640f12e7ad5c94a7`, explicit EE/IOP toolchain paths, the repository's `install_coherent_mmce.sh`, `make --trace clean`, and `make --trace LOCALVERSION=CPP-AUDITBASE NOT_PACKED=1 all`.

| Pin | Compiler | Audit result |
|---|---|---|
| OFFICIALPINNED `sha256:a1b1f87f09a88f64efbe11356aae47098d4b54b3b3a84f0609fa42369244e25d` | EE C/C++ and IOP GCC 15.2.0 | Clean link; 11,866,316-byte ELF; `text/data/bss/dec = 1,827,720 / 2,082,340 / 1,212,180 / 5,122,240`. Two clean rebuilds at the same path both hashed `bf2c489f205c4d19b29daaf4000222f20b91f51da1164bcfd134b0a762172e80`. |
| PS2DEVPINNED `sha256:8fba50ecc2229acd7f8da63d34302f12939b7d4fa6848dda1e6a0ce083321a11` | EE C/C++ and IOP GCC 15.2.0 | Clean link; 11,866,328-byte ELF; same loadable section totals; path-sensitive full hash `d3f76db3d6d6d91fe7d0c06e7305929b9068abfe081acd2b1ec27eb6f85929eb`. |

These are compile/link witnesses only. They did not build the full shipping matrix, were not saved as test artifacts, and were not run on a PS2. The 12-byte file-size difference between the two rows must not be attributed to SDK code because the builds used different absolute temp paths and include compressed debug data. The future baseline must normalize the path, hash all SDK/IRX inputs, preserve the artifacts, and obtain hardware-good controls.

### 18.4 Build-system hazards that can produce a green but wrong conversion

1. **Flag loss:** PS2SDK defines separate C and C++ pipelines, while OPL appends all project features to `EE_CFLAGS`. The future Makefile should derive both from one reviewed shared variable, then append only language-specific flags. Compare full commands and preprocessor macro sets for every flavor; a missing `PADEMU`, `GSM_1080P`, `__OPLDIAG`, debug macro, or `OPL_VERSION` is a functional regression.
2. **Stale objects:** `EE_DEPS` currently has zero words. Until repaired, a header or generated-language change can leave an old C object beside a new C++ object and still link. Require clean builds and add a dependency regression test before trusting incremental waves.
3. **False provenance:** the dirty check misses staged/untracked conversions and mislabels linked worktrees. Require committed waves and capture `status --porcelain=v2`, unstaged/staged diffs, untracked files, `HEAD`, tree SHA, compile manifest, and ELF hash together. A version string is a label, not proof.
4. **Path-dependent debug bytes:** use a fixed container checkout path or a deliberately reviewed `-ffile-prefix-map`/`-fdebug-prefix-map`; record both full/debug and stripped or loadable-section comparisons.
5. **Container presentation:** a login shell can hide installed compilers by resetting `PATH`; a CRLF mount can break shell scripts; a cached digest does not prove the registry pin is still pullable. Preflight all three before blaming source.
6. **Matrix splash damage:** `.github/workflows/flavours.yml` currently defines five primary flavors. `build_rolling_extras.sh` adds eight PADEMU/EXTRA_FEATURES/DUALSENSE combinations and six debug configurations for each ps2dev lineage. The shared C++ flags and source list must cover every one, not merely the default local build.

### 18.5 Mixed-language ABI and stack/code evidence

The unchanged `nbns.c` target-object comparison is the minimum standard for later waves:

| Property | C | GNU++17 |
|---|---|---|
| Defined public names | `nbnsInit`, `nbnsDeinit`, `nbnsFindName` | `_Z8nbnsInitv`, `_Z10nbnsDeinitv`, `_Z12nbnsFindNamePKcPh` |
| Total object size | 456 bytes | 456 bytes |
| Unexpected C++ runtime symbols | none | none |
| Stack: init/deinit/find | 32 / 0 / 48 bytes | 32 / 0 / 64 bytes |
| Function bytes | init/deinit matched | find differed |

This does not prove the C++ `nbnsFindName` is wrong; it proves that source equivalence and total size do not establish code-generation or stack parity. For every worker/RPC/DMA/launch cluster, save `-fstack-usage`, function-section hashes, and targeted disassembly. Define an external-ABI allowlist: preserve names consumed by C/generated/SDK/assembly code, while documenting harmless internal mangling rather than demanding impossible whole-ELF identity.

### 18.6 Known baseline hazards and conversion-attribution protocol

The following mechanisms exist in the current C source. They must be logged before conversion so a timing change is not mistaken for a new root cause or quietly “fixed” inside a language wave.

| Mechanism | Source status | Conversion risk | Required discriminator |
|---|---|---|---|
| Hint-list mutation versus render traversal | `moduleUpdateMenuInternal()` removes/adds raw nodes at `src/opl.c:292-327`; `menuRemoveHints()` frees them at `src/menusys.c:918-928`; `drawHintText()` traverses them at `src/themes.c:1871-1886`. The GUI holds `gGUILockSemaId` across a frame/vsync at `src/gui.c:262-317`, but the hint writer does not take it. This is a source-proven race/UAF mechanism, not a proven observed freeze. | C++ stack/code/timing changes can alter the race window without changing source logic. | Use identical instrumentation in C and C++ controls: hint generation/pointer publication, IO worker stage, frame watchdog, and final rendered node. Stress language/theme/device/list rebuilds during drawing. Do not simply hold `guiLock()` around frequent IO-worker rebuilds; that can block the IO worker behind frame/vsync. If stabilization is authorized, publish/swap on the GUI thread in a separate C commit and rebaseline. |
| BDM generation and rescan churn | `src/opl.c:1219-1282` lets a changed `BdmGeneration` bypass steady-state throttling and consumes the generation only when hotplug enqueues are accepted. | Changed worker speed can alter queue occupancy, rebuild cadence, art cancellation, and the hint race. | Trace generation, accepted/rejected enqueues, mode, `massN:` root, queue depth, art state, input, and frame timing. Test hotplug/removal/reorder, empty slots, and transient `dopen` failures across all exposed BDM pages. |
| Art-index/miss memoization | `src/artindex.c:165-297` publishes complete indexes and downgrades uncertainty to a real probe; `src/texcache.c:36-44,948-983,1322-1408,1540` stamps queued reads and negative results with generations; `src/textures.c:424-529,867-921` distinguishes genuine absence from transient device/network failures. | Scheduling changes can decide which error becomes memoized and whether stale misses revive. A cover may disappear only after contention or repeated navigation. | Inject true absence and transient faults separately on MMCE, BDM/FatFs, SMB, PFS, and archive paths. Record errno/FatFs class, fail epoch, park/retry decision, invalidation, preserve-last-good behavior, and eventual recovery. |
| Launch and callback temporal contracts | Section 17 documents queued raw pointers, alarm stack addresses, quiesce-before-free/remount, static argv, IOP reset, and child-loader handoffs. | Small stack/scheduling/destructor differences can appear only at teardown or after a delayed callback. | Log owner address/lifetime and last stage before reset/`ExecPS2`; delay callbacks/art/file I/O; test OPL, Neutrino, and POPSTARTER separately with exact artifacts. |

Use this attribution order for every runtime report:

1. Confirm exact C control and C++ candidate commit/tree, image/SDK/IRX manifest, flavor/macro set, fixed build path, and artifact hashes.
2. Apply the same diagnostic-only instrumentation commit to both artifacts. Do not compare an instrumented candidate to an uninstrumented control.
3. Reproduce on the same console, adapter, media, controller, configuration, navigation sequence, and cold/warm state.
4. Record the deepest stage and relevant counters, not merely “freeze,” “blank,” or “works.” Plasma/frame animation continuing versus stopping is a useful fault-domain inference, not causality proof.
5. If both controls reproduce, classify it as a baseline hazard and decide separately whether to stabilize C first. If only the candidate reproduces, revert/bisect conversion waves and then individual TUs while keeping build inputs fixed.
6. Never repair a suspected race, retry policy, storage path, or handoff order in the same commit that changes compilation language.

### 18.7 Corrected pickup order for the next agent

1. Verify the exact commit/tree and read this dossier plus `C_TO_CPP_PICKUP_PROMPT.md`; do not silently rebase to a newer rolling snapshot.
2. Obtain approval for the minimum Makefile and, if necessary, header linkage exceptions. If those are denied, report that a permanent physical `src/*.cpp` migration cannot be integrated faithfully under the current build contract.
3. Fix/prove shared flag propagation, dependency inclusion, committed-tree provenance, fixed-path hashing, generated-language policy, and both pinned baseline builds before editing a C source.
4. Freeze hardware-good C artifacts and the known-baseline-hazard ledger. Stabilize a blocking baseline mechanism only in a separate C-only phase with new controls.
5. Establish the C ABI allowlist and convert the `nbns.c` leaf spike under strict GNU++17. Review its symbols, protocol bytes, function code, and stack delta before proceeding.
6. Convert in the existing risk-weighted waves. Use the measured failure list as a checklist, not a bulk search/replace; one structural fix type per reviewable commit is safer than mixing casts, initializers, scopes, and linkage.
7. At every wave: clean-build the covering matrix, compare macros/symbols/layout/code/stack/resources, run fault-injected focused tests, then test exact artifacts on hardware before widening scope.
8. Finish only when all 46 tracked sources plus generated-language policy are intentional, every approved flavor builds from a clean clone, no unexpected runtime support exists, runtime attribution gates pass, and the hardware matrix names exact artifacts. Until then, report partial evidence and remaining unknowns rather than “the C++ conversion works.”
