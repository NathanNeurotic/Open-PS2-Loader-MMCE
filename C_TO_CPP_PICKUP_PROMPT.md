# Copy/paste implementation prompt: Open PS2 Loader `src/` C-to-C++ migration

Paste the prompt below into a future implementation task after the owner has answered the decision questions in `C_TO_CPP_UPGRADE_DOSSIER.md`.

```text
You are implementing a controlled C-to-C++ compatibility migration for the main EE sources of Open PS2 Loader.

Repository: https://github.com/NathanNeurotic/Open-PS2-Loader.git
Working copy: C:\Users\natha\Github\CPLUSPLUS\Open-PS2-Loader
Audited baseline: rebuild/main at 679dc31efc9dc290be405b5efbee5e4d979da79d
Audited tree: fb38df9d7f650b6ce6104d96640f12e7ad5c94a7
Requested source scope: approved main EE translation units under src/ only.
Reference dossier: C_TO_CPP_UPGRADE_DOSSIER.md

Mission
=======
Convert the approved src/ C translation units to C++ compilation while preserving behavior, ABI, binary data layout, resource use, thread/RPC/file I/O behavior, and PS2 hardware behavior. This is a parity migration, not a redesign.

Measured audit facts you must treat as controls
===============================================
* Both repository-pinned SDK images built the untouched C commit with EE/IOP GCC 15.2.0 after the repository's coherent-MMCE preparation. This proves baseline compile/link only; no audit artifact was promoted as a hardware-good control.
* The build has 46 tracked src/*.c files plus generated src/lang_internal.c. Strict GNU++17 target-G++ censuses under both pinned SDK lineages passed the same 19 and failed the same 28 with identical diagnostic counts; OFFICIALPINNED GNU++20 produced the identical set. Use strict GNU++17. Never use -fpermissive in a candidate or release.
* -fpermissive still leaves ten structural failures: six support-list redefinitions (app/eth/fav/hdd/mmce/udpfs), bdm and tar goto-scope failures, guigame's indexed array initializers, and sound's Vorbis static redefinitions.
* Unchanged C++ nbns.c mangles all three public symbols. Its C and C++ objects have equal total size, but nbnsFindName's measured stack frame grows from 48 to 64 bytes and its function bytes differ. Establish C linkage before the leaf spike and compare per-function code/stack, not just total size.
* EE_CFLAGS and EE_CXXFLAGS are separate. OPL currently appends its feature/version/optimization/dependency/section flags only to EE_CFLAGS. A stock .cpp rule can therefore create a green but wrong-feature build.
* EE_DEPS currently evaluates to zero words for 234 EE object words. Incremental-build evidence is invalid until this is corrected and regression-tested.
* The Makefile's dirty/version logic misses staged/untracked conversion files, mislabels linked worktrees, and full hashes vary with DW_AT_comp_dir. Use committed trees, a full clone at a fixed path, full status/diff evidence, and both full plus normalized/stripped hashes.

Before touching source
======================
1. Start read-only. Record git status --porcelain=v2 including untracked files, unstaged and staged diffs, HEAD, tree SHA, worktrees, remotes, file EOL state, PS2SDK/GSKIT paths, compiler versions, image digests, and the exact host/target environment.
2. Use a fresh isolated checkout. Development may use a worktree, but provenance artifacts must come from a full clean clone at a fixed LF-only container path because the current dirty check and DWARF paths make linked/random-path builds misleading. Do not modify a dirty checkout or use destructive reset/checkout commands.
3. Resolve and inspect both repository-pinned images. Verify fresh pullability, mirror the workflow's host-package installation (the minimal ps2dev pin has no Git initially), set the explicit /usr/local/ps2dev EE/IOP PATH, and record SDK/gsKit versions, coherent-MMCE input/output, and IRX manifests. Do not execute CRLF host scripts directly in Linux, and do not use a host compiler as target proof.
4. Obtain explicit approval for the minimum root Makefile exception: C++ source selection, one shared C/C++ front-end flag set, corrected dependency inclusion, and truthful provenance. A physical .cpp rename cannot work faithfully under the current rule/flag split. Confirm any minimum include/ linkage exception separately.
5. Prove C/C++ compile-command and -dM macro parity for all EE feature/debug flavors before accepting one C++ object. Specifically check OPL_VERSION, PADEMU, RTL/IGS including EXTRA_FEATURES, GSM_1080P, DTL_T10000, OPLDIAG, and every debug/TTY macro. Separately prove module/generator inputs such as DUALSENSE and the embedded IRX hashes are unchanged.
6. Correct and regression-test EE_DEPS by touching representative transitive/generated headers and observing the expected rebuilds. Until that passes, start every gate with make clean and reject all incremental evidence.
7. Establish and archive unmodified baselines with both pinned SDK lineages before converting any file. Capture committed source provenance, exact fixed path, commands, generated files, full and normalized ELF hashes, size/sections/map/objects/symbols/relocations, stack usage, SDK/IRX manifests, and hardware-good control results. If a control is unavailable, report the specific environment/build/hardware blocker.
8. Freeze a known-baseline-hazard ledger and apply identical diagnostic-only instrumentation to C and C++ artifacts. If a baseline race prevents a trustworthy control, stabilize it in a separate C-only commit, test it on hardware, and rebaseline; never mix that fix into a conversion wave.

Hard constraints
================
* Convert only the approved main EE files in src/. Do not edit modules/, IOP sources, ee_core, elfldr, generated asm/*.c, or headers/build files unless the owner explicitly approved the minimum integration exception.
* Do not add STL, exceptions, RTTI, new/delete, smart pointers, global constructors, namespaces, or an ownership/scheduling redesign during parity.
* Compile candidate code under strict -std=gnu++17 with -fno-exceptions -fno-rtti -fno-threadsafe-statics and -fno-use-cxa-atexit. Do not use -fpermissive. Do not switch to GNU++20; it removes none of the measured blockers.
* Preserve function and variable names, C linkage, structure packing/bitfields, callback signatures, attributes, inline assembly, setjmp/longjmp cleanup, manual allocation/free, thread lifetimes, IOP reset/keep-IOP sequencing, configuration keys, and save timing.
* Keep generated binary arrays and C objects unmangled. Handle generated assets with one auditable extern "C" boundary, not ad hoc duplicate declarations.
* Do not change behavior to make a compiler warning disappear. Make only the mechanical syntax change required and document every exception.

Known high-risk areas
=====================
* Root Makefile explicitly maps src/*.c to EE C compilation and explicitly lists FRONTEND_OBJS.
* asm/*.c defines embedded ELF/IRX, IOPRP, font, PNG, theme, audio, and icon symbols. extern_irx.h and source-level extern arrays must retain C linkage.
* include/dia.h, include/gui.h, and include/ds34common.h use anonymous structs/unions, packed layouts, bitfields, and function pointers. Prove sizeof/alignof/offsetof and packet bytes before/after.
* dialogs.c's 726 member-designator expressions already pass strict target GNU++17. guigame.c:599-615 uses unsupported indexed array designators and fails identically under GNU++17/20. Re-derive and byte-test its current sequential map (audited snapshot: {16,13,14,15,1,4,2,3,7,8,5,6,12,10,9,11,0}); rewrite only that table mechanically.
* elfldr_noreset.c contains a variable-length argv array, inline assembly, ExecPS2, FlushCache, and keep-IOP-sensitive handoff code.
* textures.c uses libpng setjmp/longjmp and manual cleanup. Never put C++ objects with destructors across the jump boundary.
* gui.c, util.c, system.c, and elfldr_noreset.c contain inline MIPS/VU assembly. Compile and inspect with the real target compiler.
* hddsupport.c has a mutable char* string literal; guigame.c has a mutable char* version-string table. Change to const only after call-site audit.
* Many files assign malloc/calloc/realloc results without casts, rely on C goto rules, or use exact SDK callback signatures.
* The strict census emitted 125 invalid conversions. The largest groups are void* to item_list_t* (28), function pointer to void* (21), void* to void** (9), and void* to char* (9). Treat every cast as an ownership/representation review. Centralize and target-assert the existing function-pointer-through-void* convention instead of scattering casts.
* Six const-discarding sites require call-chain proof rather than const_cast: atlas.c:164, bdmsupport.c:240/1973, ethsupport.c:733, favsupport.c:616, and system.c:1658.
* Fix three same-length string/array initializers with explicit bytes while preserving exact size and absent terminators: dia.c:64, supportbase.c:715's expansion of CDVDMAN_SETTINGS_DEFAULT_COMMON/"DSKID", and util.c:555. Do not edit modules/iopcore merely to change the source-side sample.
* C tentative internal definitions are C++ redefinitions at appsupport.c:36/956, ethsupport.c:57/1093, favsupport.c:34/975, hddsupport.c:200/2272, mmcesupport.c:88/1171, and udpfssupport.c:38/422. Move the single initialized definition and add exact prototypes; do not introduce local-static guards or runtime assignment.
* bdmsupport.c:1754-1829 and tar.c:261-376 need minimal declaration/scope changes for goto. Preserve their fail cleanup, Neutrino buffer lifetimes, TAR index ownership, and return values. sound.c:18-20 must stop redeclaring header-defined OV_CALLBACKS_* objects without creating replacement runtime state.
* lz4.c and zso.c include implementations from modules/isofs; converting those TUs changes how the included code is parsed without changing module files.
* bdmsupport.c contains one actual NUL byte in the source. Resolve it explicitly before format/compiler work, and record the change.

Runtime-only hazards to test
============================
* `ioPutRequest()` (`include/ioman.h:29-30`, `src/ioman.c:132-208,255-295`) queues unowned raw pointers and can reinterpret a payload as a function pointer. Preserve static/persistent ownership, exact callback representation, and timeout/shutdown behavior; never queue a temporary, moved element, `c_str()` result, or integer-round-tripped callback.
* The art worker has a separate ownership graph: requests point into cache content/prefix/suffix and app/menu lists while theme/device rebuilds may free or realloc them (`include/texcache.h:70-86`, `src/texcache.c:605-627`, `src/appsupport.c:605-612`, `src/artindex.c:306-319`). Preserve cancellation/drain order and the deliberate no-free-from-arbitrary-thread rule; stress theme/list rebuilds during queued, decoding, cancelled, and stuck fileXio reads.
* Keep the pre-existing hint-list race mechanism in the baseline ledger: moduleUpdateMenuInternal removes/adds nodes on the IO worker (`opl.c:292-327`; `menusys.c:896-928`) while drawHintText traverses them (`themes.c:1871-1886`). The GUI lock spans frame/vsync (`gui.c:262-317`), but the writer does not take it. This is source-proven concurrency risk, not proof of any observed freeze. Apply identical instrumentation to C/C++ controls; do not "fix" it inside a conversion wave or hold the IO worker on guiLock across vsync. If stabilization is required, publish/swap on the GUI thread in a separate C commit and rebaseline.
* Stress BDM generation handling (`opl.c:1219-1282`) across hotplug/removal/reorder, empty and populated massN pages, queue rejection, and transient dopen failures. Trace generation consumption, accepted enqueues, mode/list/hint rebuilds, art state, input, and frame time; a C++ timing shift can amplify churn without changing logic.
* Treat negative art caching as an error-classification protocol (`artindex.c:165-297`; `texcache.c:36-44,948-983,1322-1408,1540`; `textures.c:424-529,867-921`). Inject genuine absence separately from MMCE/BDM/SMB/PFS transients; prove only true absence parks, transients retry, generations revive stale misses, and last-good data is preserved where intended.
* `guiDeferUpdate()` and dialogs borrow caller-owned menu, text, and enum storage (`include/gui.h:22-50`, `src/gui.c:3280-3425`, `src/dia.c:1410-1437`). Rebuild/reshow after language/theme/device changes and deliberately delay consumers to catch stale pointers. Sticky boot text must use the copy setter (`include/gui.h:209-214`).
* `textures.c` crosses libpng `setjmp`/`longjmp` with manual cleanup. Keep that region C-trivial; test corrupt/truncated/interlaced PNGs and allocation failures for leaks, double frees, and locked caches.
* Preserve C-shaped record layout and bytewise access in `iosupport.h`, favorites/gui update records, packed/bitfield tables, and image/IRX/settings patching. Compare `sizeof`, alignment, offsets, packed bytes, checksums, and patched bytes; do not add virtuals, constructors, bool/container members, or unsafe strict-aliasing changes.
* Theme font slots alias `FNT_DEFAULT` when a custom slot is absent (`themes.c:2554-2585`) but `thmFree()` releases each slot (`themes.c:2469-2472`). Preserve that integer-handle/sentinel behavior; do not introduce C++ ownership or refcount changes. Test theme reloads with sparse and fully populated custom-font slots.
* Treat aligned byte-backed RPC/device buffers as protocol objects (`hdd.c:64-125`, `httpclient.c:31-72`, `nbns.c:33-35`). Preserve alignment, exact request/reply sizes, and target-approved byte-copy/object-lifetime rules; compare complete SIF/fileXio buffers under optimized C++ builds.
* Preserve the MIPS address-patching and ELF-copy rules in `system.c:945-946,975-976,1631-1644,1734-1737`: pointer-to-immediate width, segment, cache flush, and `void *` arithmetic are boot contracts. Compare patched instructions and addresses on every EE-core/boot path.
* Treat `ioprp.c:32-60,99-115` and `xparam.c:79-112,225` as binary patch generators: preserve 16-byte ROMDIR layout/padding, replacement-module offsets, final IOPRP bytes, XPARAM byte 12/NUL placement, fixed SifLoadModule lengths, and per-title ordering. Exercise IOP reset, Deckard/IGR, and regional-title paths.
* Treat packed controller reports in `include/ds34common.h` as wire data: never access them through naturally aligned C++ references or padded class members; compare HID/rumble bytes and test native/PADEMU reconnect paths.
* Preserve the `src/guigame.c:55-72` `PadMacroSettings` anonymous bitfield union and its raw `int` config word (`src/guigame.c:920-947,1672-1706`). Compare `sizeof`, masks, and config round trips; test every slowdown/invert/turbo toggle on native and PADEMU controllers because a changed bitfield allocation order can silently alter persisted behavior.
* Preserve explicit DMA/RPC/audio/controller alignment (`pad.c`, `hdd.c`, `httpclient.c`, `nbns.c`, `util.c`, `tar.c`, `texcache.c`, `sound.c`) and log runtime addresses on hardware.
* Preserve semaphore and interrupt brackets and worker ordering (`ioman.c`, `menusys.c`, `texcache.c`, `vcdsupport.c`, `sound.c`). Stress queue-full, timeout, cancellation, teardown, controller input, audio, and cache/VCD work while tracing ownership and wakeups.
* Treat `strtok` in support/system as process-global state; do not replace it with a shared C++ tokenizer without proving thread ownership. Stress concurrent theme reload, settings editing, and Neutrino launch preparation. qsort and `_io_driver` callbacks must retain exact C signatures/linkage; no capturing lambdas or member-function pointers.
* Audit every old-style empty prototype (`gui.h`, `fntsys.h`, `config.h`, `bdmsupport.h`, `hddsupport.h`, `menusys.h`, `opl.h`) before normalizing it: C `fn()` accepts unspecified arguments, while C++ `fn()` means no arguments. Inventory all cross-TU calls and address-taken callbacks before changing a declaration; an ABI mismatch can survive link and fail only on a rarely used callback path.
* `sound.c:765-770` intentionally passes a small thread ID through the alarm callback's `void *` argument. Preserve the target's 32-bit representation and delayed shutdown/restart behavior; do not pass a temporary address or use an unverified host-sized conversion.
* Preserve plain `volatile` flag representations and polling semantics (`sound.c`, `texcache.c`, `artindex.c`, `gui.c`). Do not replace them with C++ atomics or remove `volatile` during parity; prove any memory-order change on the target.
* Audit all three VLAs (`gui.c:3263`, `menusys.c:126`, `elfldr_noreset.c:116`) for bounds, lifetime, stack pressure, and `ExecPS2` argv validity. Exercise large/empty input, long argv, VMC, and launch return paths with stack high-water measurements.
* `vcdsupport.c:250-301` has a true flexible array member (`char name[]`) with a trailing allocation and linked-list invalidation. Standard C++ cannot express it directly; preserve the `offsetof`/allocation/ownership contract and stress duplicate scans, device changes, invalidation, and OOM.
* Reject unexpected C++ runtime symbols (`__cxa_*`, personality/unwind, guard/constructor/atexit machinery), and compare stack/heap/section/relocation budgets under memory pressure and long sessions.
* Verify generated/file-scope const data remain externally linked and unmangled (`renderman.c`, `extern_irx.h`, `bdma_embed.h`) and exercise rendering, IRX loading, and embedded-asset launch paths.
* Preserve generated language-table ownership (`include/lang.h:9`; `lang.c:24-35,64-79,136-146,214-221`). `internalEnglish` is a writable pointer table mixed with heap-owned translation entries; do not constify/copy it without proving free counts, fallback identity, partial-file behavior, language switches, and `lngEnd()`.
* Keep the `src/zso.c` text-included `modules/isofs/zso.c` as one global ZSO implementation with one audited C ABI boundary (`modules/isofs/zso.h`, callers in `supportbase.c`, `hddsupport.c`, and `util.c`). Do not “repair” C++ linkage by making duplicate/private cache state; test compressed block reads, seeks, cache rollover, and HDD/USB/network opens.
* Treat launch as a timing protocol: preserve static GSM/ELF argv storage (`system.c:1423,1436`), keep-IOP/quickboot and POPSTARTER teardown ordering (`system.c:1363-1379,1513-1542`), and the quiesce-before-free/remount/ExecPS2 rules in MMCE/HDD/Ethernet/UDPFS (`mmcesupport.c:720-792,812-833,969-1041`; `hddsupport.c:1548-1624`; `ethsupport.c:993-995`; `udpfssupport.c:304-351`). Delay art/file-I/O completion during OPL, Neutrino, and POPSTARTER handoffs and capture the last stage reached.
* Audit the network alarm callback stack contract (`ethsupport.c:182-200`): `WaitValidNetState()` passes a local semaphore ID to `SetAlarm` and waits synchronously. Preserve that timing, or prove cancellation/timeout cannot fire after the local is out of scope; test retries, delayed alarms, and teardown.

Execution sequence
==================
1. Approve and prove Phase-0 build plumbing: fixed-path full clone, both image pins, shared C/C++ flags and macro parity, corrected dependency tracking, committed-tree provenance, generated-language policy, normalized hashes, and exact C artifacts.
2. Freeze hardware-good C controls and the known-baseline-hazard ledger. Diagnostic-only instrumentation must be identical in C and C++ artifacts. Stabilize a blocking baseline mechanism only in a separate C-only phase.
3. Define the external C ABI allowlist and intentional extern "C" boundary before the leaf. Then convert nbns.c or httpclient.c, link with untouched C/generated objects, and inspect nm, runtime symbols, function bytes/disassembly, protocol bytes, and -fstack-usage immediately.
4. Convert in waves: (a) leaf/format files; (b) language/config/util/I/O/support/archive/VMC/cheat/application/favorites; (c) BDM/HDD/Ethernet/SMB/UDPFS/MMCE/VCD/handoff/system; (d) renderer/font/texture/cache/theme/sound/pad/menu/dialog/game UI. Convert gui.c and opl.c last.
5. After every file, compile strictly with target GNU++17. After every wave, build from a clean object directory, run git diff --check and format checks, compare commands/macros/symbols/sections/relocations/function bytes/stack/layout/resources, run fault-injected focused tests, and test exact artifacts on the required hardware slice.
6. Keep committed, reviewable changes separated by mechanism and wave. Do not mix linkage, bulk casts, initializer rewrites, goto scopes, baseline fixes, or unrelated generated churn in one opaque commit.
7. Once all waves pass, make the build policy permanent and verify a full clean clone regenerates language/assets and builds all five primary workflow flavors plus the current eight variants and six debug configurations for both ps2dev lineages, or the explicitly approved covering matrix.

Per-file rules
==============
* Preserve public signatures and storage classes.
* Fix implicit void* conversions with explicit, audited typed casts; preserve allocator domains and failure paths. Function-pointer/object-pointer conversions require one documented target representation bridge and size/round-trip probes.
* Fix string-literal constness only where the API proves the object is read-only.
* Hoist declarations or add local scopes only to satisfy C++ goto rules; preserve cleanup order and return values.
* Preserve attributes, alignment, volatile qualifiers, asm constraints/clobbers, and special symbols such as _gp.
* Keep C APIs and C-shaped data. Do not replace NULL wholesale or introduce modern ownership abstractions.
* For each affected binary/on-wire structure, compare sizeof, alignof, offsetof, packed bytes, and bitfield masks.
* For every deferred pointer/callback, document the owner and minimum lifetime, then test delayed consumption, queue-full/OOM, timeout, cancellation, and teardown.
* For every borrowed dialog/menu/text pointer, prove validity through the final render after language/theme/device changes; dynamic boot status must use an owned copy.
* For every setjmp/longjmp path, prove no C++ destructor/RAII object crosses the jump and exercise the error path, not only valid input.
* For every C flexible-array or trailing-allocation record, compare `offsetof`, allocation bytes, alignment, string termination, and invalidation/free behavior; do not model it as a pointer-owning C++ member without an explicit layout decision.
* Preserve whole-record `memcpy` ownership transfers such as the font swap (`fntsys.c:309-321`) as C-trivial operations; do not introduce destructors/vtables or C++ copy assignment into records whose resource pointers are freed later.
* For every launch path, record the owner/lifetime of argv, game-ID, VMC, descriptor, mount, and art buffers through deinit, settle delays, IOP reset, `ExecPS2`, and return; prove no destructor or move changes the handoff order.
* Save -fstack-usage plus targeted function-section hashes/disassembly for worker, RPC, DMA, patching, and handoff code. Equal total object size does not clear an unexplained function or stack delta.

Required validation
===================
Source/build:
* Clean checkout and clean object directory for every release build.
* Converted EE units compile as C++; untouched/generated C units compile as C.
* Strict GNU++17 only; no -fpermissive. Full C/C++ commands and -dM macro dumps prove one feature/version configuration per flavor.
* git diff --check, repository format checks, no accidental source NUL bytes.
* No accidental throw/new/delete/STL/RTTI, __cxa_*, __gxx_personality_*, or other unwanted C++ runtime symbols.
* Dependency regression tests prove representative public/generated header changes rebuild the right C and C++ objects. Until then, only clean-build evidence counts.
* Source provenance records status --porcelain=v2, unstaged/staged diffs, untracked files, commit/tree, fixed path, image/SDK/IRX manifests, commands, and artifact hashes. OPL_VERSION alone is not proof.
* Stress deferred I/O/GUI queues, dialogs, PNG error paths, queue-full/OOM, timeout/cancellation/teardown, and concurrent theme/menu/art/settings/controller/audio operations.
* Capture stack high-water, EE heap, runtime DMA/RPC buffer addresses, semaphore/interrupt traces, and callback targets for the focused tests.
* Under optimization, compare complete RPC/device request/reply bytes for the aligned byte-backed buffers and compare MIPS handoff patch words, IOPRP image bytes, XPARAM fixed lengths, target addresses, and cache synchronization on every EE-core/IOP path.
* Exercise partial/invalid language files, repeated language switches, font reload, OOM, and `lngEnd()`; exercise ZSO/CSO reads and seeks across HDD/USB/network opens and prove the single shared ZSO cache state.

Binary/ABI:
* Compare nm, readelf -h -S -s, relocations, size, map files, full and normalized/stripped ELF hashes, section sizes, alignment, per-function bytes/disassembly, stack usage, stack/heap reservations, and generated asset addresses/sizes.
* Full hashes are comparable only under a fixed or reviewed prefix-mapped build path; separately compare stripped/loadable content so DW_AT_comp_dir cannot invent or hide a semantic difference.
* Explain every symbol, section, relocation, function-code, stack, layout, or resource delta. An unexplained delta is a stop condition.

Hardware:
* Test exact artifact SHA and source commit on the approved console/device matrix.
* Cover memory-card, USB, HDD/APA/PFS, Ethernet/SMB, MMCE, iLink, and MX4SIO paths where supported; discovery, art/index, language, theme, settings save, favorites, VMC, cheats, TAR, and OSD.
* Cover native DualShock/DS3/DS4/DS5, rumble, PADEMU, audio, GSM/1080p, cache pressure, BDM/HDD/network recovery, UDPFS/UDPBD, and long sessions.
* Cover OPL, Neutrino, and POPSTARTER launches, embedded child-loader handoff, keep-IOP, IOP reset, and return paths.
* Stress hint rebuild versus rendering, BDM generation/hotplug and massN pages, and genuine-absence versus transient-art faults with identical C/C++ diagnostics. Record frame/plasma state, input/pad state, IO queue, hint generation, BDM generation/enqueues, fail epochs/error classes, and recovery.
* During those launches, delay art/file-I/O workers and network alarms; verify quiesce-before-free/remount, static argv lifetime, descriptor settle waits, and the `WaitValidNetState()` semaphore callback cannot target a deleted semaphore or dead stack address.
* CI/static success is not hardware proof. Record console, adapter, media, configuration, artifact hash, observed stage, and result.
* Do not call the migration runtime-safe until delayed callbacks, borrowed-pointer dialogs, longjmp cleanup, alignment, synchronization, tokenizer interleaving, VLA/argv bounds, and low-memory/long-session behavior have explicit passing evidence.
* If both exact C and C++ controls reproduce a failure, classify it as a baseline hazard and decide separately whether to stabilize C. If only C++ reproduces it, bisect conversion waves/TUs while holding every build and hardware input fixed.

Stop and ask for direction if:
* baseline or candidate builds are not reproducible;
* C and C++ compile commands/macros differ outside an explicitly reviewed language-only delta, dependency tracking is unproved, or committed-tree/artifact provenance is incomplete;
* a generated, SDK, IRX, callback, or entry-point symbol becomes mangled/unresolved;
* structure layout, packed bytes, embedded assets, sections, relocations, function code, stack/heap, or ELF size changes without an evidence-backed explanation;
* C++ runtime support or global initialization appears;
* thread, RPC, mount, cache, controller, launch, or IOP sequencing changes;
* hardware regresses; or
* the next fix requires broad module/header/build/runtime redesign outside the approved exception.

Final handoff
=============
Report the exact branch/commit/tree, full status and staged/untracked state, scope and exceptions, files converted, diffstat, image/SDK/IRX/MMCE provenance, fixed build path, full commands/macro manifests, dependency proof, all source/build/CI/static results, symbol/relocation/function-code/stack/layout/resource comparisons, full and normalized artifact hashes, baseline-hazard attribution results, hardware matrix, known limitations, rollback point, and remaining decisions. Separate source proof, build/CI proof, and console proof. Do not claim “works” unless the required gates actually passed.
```
