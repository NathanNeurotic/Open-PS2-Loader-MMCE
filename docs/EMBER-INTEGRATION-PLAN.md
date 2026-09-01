# Ember as a second PS1 core — integration plan

**Status:** ordinary device PS1 pages ship both cores in one list; UDPFS and UDPBD ship an
Ember-only PS1 list because POPSTARTER cannot restore either network transport. Ember handoffs
remain hardware-unproven unless a later test report says otherwise.
**Base commit:** `291716d3` on `rebuild/main` (2026-08-27).
**Permission:** Gage (author of Ember) has approved and encouraged this integration.

---

## Implementation status

Branch `feat/ember-ps1-core`. Update this table as phases land; it is the first thing the next
agent should read.

| Item | State | Notes |
| --- | --- | --- |
| Phase 0 — prove the handoff | **code landed, HARDWARE PENDING** | `sysLaunchEmber()` + `cuesupport.c` + an OPLDIAG-only interception in `appLaunchItem`. See "How to run the Phase 0 test" below. |
| Phase 1 — view engine | **landed** | `libview.c/.h` own the ring; the binary `vcdView` API is deleted, not shadowed. Both audit gates pass. |
| One PS1 list, both cores | **landed** | `ps1FillGameList()`; row-kind dispatch for launch and art. Wired for BDM, MMCE, SMB, and APA/PFS HDD. |
| SMB / ETH | **landed** | R4 resolved: the Ember helpers build paths with `cueSep()`, so the separator follows the device. |
| APA-HDD | **landed, HARDWARE PENDING** | `__.EMBER[0-9]?` partitions, self-contained. Needed a new `KEEPIOP_EXCEPTION` — see below. |
| UDPFS / UDPBD | **landed, HARDWARE PENDING** | Their PS1 pages call `cueFillGameList()` directly and publish Ember rows only. POPSTARTER rows are deliberately excluded. |
| Favorites (OFAV v3 `kind`) | **landed** | ISO, VCD, CUE, and ELF kinds; All in One / PS2 / PS1 / ELF shelves. The compatibility filename remains `favourites.bin`. |
| Ember settings page | **landed** | Ember Display Mode lives on PS Emulation Settings; the library remains presence-driven by each device's `EMBER/` folder. |

### How to test Ember on hardware

There are now two ways in, and both matter.

**1. The real path (works in a normal release build).** Lay out the device exactly as you would a
POPSTARTER library, with `EMBER/` beside `POPS/`:

```
mass0:/POPS/  ...                                   <- your existing PS1 library, untouched
mass0:/EMBER/ember.elf                              <- from Ember.Beta.Release.zip
mass0:/EMBER/bios.bin                               <- your own PS1 BIOS, exactly 512 KB
mass0:/EMBER/games/Spyro 2 (Ripto's Rage)/game.cue  (+ game.bin)
```

With **PS2/PS1 Game Display** set to **Both (L3)**, press **L3** on that device page to reach the
PS1 list. **Mixed** instead combines PS2 and PS1 rows and cycles Mixed → PS2 → PS1 with L3. Ember
titles appear interleaved with your `.VCD` ones inside the PS1 portion, sorted together. On UDPFS
and UDPBD, the PS1 portion contains Ember titles only. Launch one.

**2. The OPLDIAG probe (the control experiment).** An APPS entry with `boot` =
`mass0:/EMBER/ember.elf` and `argv1` = the folder name is handed over with the keep-IOP loader in an
`OPLDIAG=1` build. The same entry on a **release** build is expected to *fail*, because the stock
APPS loader resets the IOP out from under Ember. That contrast is what proves the no-reset premise —
if the release build also works, the premise needs re-examining.

**What to report back**

1. Do the Ember rows appear in the PS1 list, correctly interleaved with the `.VCD` ones?
2. Does a title boot?
3. **Do the controllers work?** This is risk R1 and it decides whether the design stands as written
   or needs the clean-IOP fallback.
4. Does it work from MX4SIO / MMCE / internal exFAT ATA, not just USB?
5. Do `MC1.vmc` / `MC2.vmc` appear in the game folder afterwards? (Confirms it can write there.)
6. Anything that looks like IOP memory exhaustion — a late, unexplained failure.

Covers: an Ember row uses the device's normal `ART/<name>_COV.png`, and nothing else. **ART lives in
the ART folder** — there is no second location for any game type.

## Part 0 — What Ember actually is (measured, not guessed)

There is no Ember source. Everything below was derived from `Ember.Beta.Release.zip`
(tag `Beta`, published 2026-08-28, 1 557 425 bytes) by disassembling the shipped ELFs with the
`ghcr.io/ps2dev/ps2dev:latest` toolchain. Both ELFs are **unstripped with DWARF**, so this is
solid ground, not inference. Re-derive with:

```bash
gh release download Beta --repo Gageformer/Ember
```

The zip contains exactly two files: `ember.elf` (3 759 476 B) and `launcher.elf` (1 666 416 B).

### 0.1 `launcher.elf` — the thing we are replacing

Gage's launcher is a plain ps2sdk bootstrapper. Its symbol table shows the whole recipe:

| Symbol / string | Meaning |
| --- | --- |
| `SifIopReset` | resets the IOP |
| `sbv_patch_disable_prefix_check`, `sbv_patch_enable_lmb` | standard SBV patches |
| `size_iomanX_irx`, `size_fileXio_irx` | loads iomanX + fileXio |
| `size_usbd_irx`, `size_usbmass_bd_irx` | loads the USB host stack + USB mass driver |
| `size_bdm_irx`, `size_bdmfs_fatfs_irx` | loads BDM + the FAT/exFAT filesystem layer |
| `ExecPS2` | jumps into `ember.elf` |

So the environment Ember is *designed* to receive is: **iomanX + fileXio + BDM + bdmfs_fatfs, with
the game device already mounted as `massN:`.** That is a strict subset of what RiptOPL already has
live at launch time. `launcher.elf` is USB-only; RiptOPL's stack is not.

### 0.2 `ember.elf` — it never resets the IOP

`nm ember.elf` contains `sceSifInitRpc`, `sceSifExitRpc`, `SifLoadModule`, `SifLoadFileInit`,
`SifInitIopHeap` — and **no `SifIopReset`, no `SifIopRebootBuffer`, no `sceSifRebootIop`.**
The only `rom0:` strings in the whole binary are `rom0:ROMVER`, `rom0:SIO2MAN`, `rom0:PADMAN`.

This is the single most important fact in this document. Ember **cannot** rebuild its own device
stack; it inherits whatever the launcher leaves running. That is why it must be handed off with
`sysLoadELFKeepIOP()` (`src/elfldr_noreset.c`) — the same vendored no-reset child loader Neutrino
already uses — and **not** with `LoadELFFromFileWithPartition()`.

It is also why Nathan's instinct is right: *because Ember inherits the environment it is handed, it
can play from any device RiptOPL can mount.* MX4SIO, iLink, internal ATA (exFAT), MMCE, APA/PFS,
SMB and even UDPBD are all reachable in principle, because we hand over a live mount rather than
asking Ember to find one. POPSTARTER can never do this (it resets and re-discovers from the memory
card, which is exactly why `bdmLaunchVcd` has to refuse UDPBD).

### 0.3 Argument contract — measured from `main` at `0x158a88`–`0x15bb44`

This is the constraint that dictates the entire folder layout, so it is spelled out in full.

```
argv_join(buf /*sp+1080*/, 192, &argv[1], argc-1);   // 0x15b630 — spaces are re-joined
if (buf[0] == '\0')             goto DEFAULT;        // 0x15b63c
if (buf[0]=='.' && buf[1]=='.') goto REFUSE;         // 0x15bb28
for (c in buf)                                       // 0x15b660 char scan
    if (c=='/' || c==':' || c=='\\') goto REFUSE;    // mask (1<<45)|(1<<11)|(1<<0) over (c-47)
ACCEPT:  s2 = buf;  arg = buf;   goto RESOLVE;       // 0x15bb3c
DEFAULT: s2 = NULL; arg = "default";                 // 0x158af0 (rodata 0x16d710 == "default")
REFUSE:  print "REFUSED game arg '%s' -- name a folder directly under games/";
         s2 = NULL; arg = "default";                 // then falls into RESOLVE, no retry

RESOLVE: snprintf(path, 192, "games/%s", arg);       // 0x158b10, rodata 0x16d388 == "games/%s"
         print "Ember 2: game location: %s", path;
         if (io_find_disc(path, &disc)) goto BOOT;
         print "Ember 2: find=%d errno=%d disc='%s'";
         if (s2) {                                   // 0x158b90
             print "Ember 2: retrying arg as path: %s", s2;
             if (io_find_disc(s2, &disc)) goto BOOT; // relative to cwd, still no separators
         }
         print "Ember 2: no disc (BIOS shell run)";
```

**Therefore:**

1. The game argument is a **bare directory name**. `/`, `:` and `\` are hard-refused anywhere in
   it, and `..` is refused as a prefix. **An absolute path can never be passed.**
2. Multiple argv entries are re-joined with spaces, so a name with spaces can be sent either as one
   argv entry or several. Send it as **one**.
3. Resolution order is `games/<name>` then `<name>`, **both relative to Ember's working directory**.
4. The effective name cap is ~185 bytes (`snprintf` into a 192-byte buffer after `games/`).

### 0.4 Working directory, BIOS, disc discovery, memory cards

* `ember.elf` links ps2sdk's `src/cwd.c` and exports `__init_cwd` / `__cwd` / `__path_absolute`.
  ps2sdk derives the initial cwd from **`argv[0]`**. So `argv[0]` must be the *real full path of
  `ember.elf`*, e.g. `mass0:/EMBER/ember.elf` — that is what makes `games/…` resolve.
  `sysLoadELFKeepIOP()` already forwards a caller-controlled `argv[0]` verbatim (the POPSTARTER
  selector rides the same mechanism), so this costs nothing.
* `bios.bin` is opened relative to cwd and must be **exactly 512 KB**
  (`"Ember 2: bios.bin FAILED ('%s' -> %ld, want %lu)"` / `"bios.bin loaded (512 KB)"`).
  User-supplied; we never ship it.
* `settings.txt` is `key:value` lines, one key implemented: `display:240` or `display:480`.
  Unknown keys are ignored with a log line. We do not need to write it.
* `io_find_disc()` (`0x10eed8`) accepts **either** a file whose extension is `.cue`, `.bin` or
  `.exe` (case-insensitive `strcasecmp`), **or** a directory containing one — it `opendir()`s and
  scans. `.cue` wins over `.bin`. Note `.exe` (PSX-EXE) is supported and undocumented in the README.
* Memory cards are `<gamedir>/MC1.vmc`, `<gamedir>/MC2.vmc`, with optional
  `<gamedir>/SharedMC.txt`. **Ember creates these, so the game folder must be writable.**
* Ember loads only `rom0:SIO2MAN` and `rom0:PADMAN` itself, and logs `"pad init %s (rc=%d)"`.

---

## Part 1 — Folder structure

`<device>:/EMBER/` is the peer of `<device>:/POPS/` — one folder at each activated device's root,
sought the same way on every device class. **That is where the resemblance ends.** POPS holds loose
`*.VCD` FILES; EMBER holds a program plus a `games/` folder of per-game DIRECTORIES. The scan is a
directory scan, not a file scan, and the identity is the directory name with no extension to strip.

```
<device-root>/
  POPS/                             <- unchanged
    POPSTARTER.ELF
    Spyro 2 (Ripto's Rage).VCD
  EMBER/
    ember.elf                       <- required; its presence is what enables Ember rows
    bios.bin                        <- required at launch (512 KB, user-supplied, never shipped)
    settings.txt                    <- optional, Ember's own (key:value; only `display:240|480`)
    games/
      Spyro 2 (Ripto's Rage)/       <- one directory per title; the NAME is the launch argument
        game.cue
        game.bin
        MC1.vmc  MC2.vmc            <- Ember creates these HERE on first launch
```

### Which layouts Ember accepts, and why we list exactly one

Ember's resolver is looser than its README. It tries `games/<arg>` and then `<arg>`, and
`io_find_disc` takes **either** a directory containing a `*.cue` / `*.bin` / `*.exe` **or** such a
file directly. All four of these boot:

| # | Layout | Boots | Saves |
| --- | --- | --- | --- |
| 1 | `games/<Name>/` holding the image | yes | **yes** |
| 2 | `games/<Name>.cue` (loose file) | yes | no |
| 3 | `<Name>/` beside `ember.elf` | yes | wrong place |
| 4 | `<Name>.cue` beside `ember.elf` | yes | wrong place |

**Only layout 1 saves**, and that is why it is the only one we list. `main()` calls `memcard_init`
exactly once — verified, one call site — and always with the *composed* path `games/<arg>`, never
with wherever the disc was actually found. The cards are therefore always
`games/<arg>/MC1.vmc` / `MC2.vmc`. So layout 2 asks for a card *inside a file*
(`games/Spyro.cue/MC1.vmc`), which cannot exist; layouts 3 and 4 put the cards in
`games/<Name>/`, a directory unrelated to the disc mounted from `EMBER/<Name>/`.

Those titles play and silently cannot save. That is a worse experience than not being offered — and
the user blames the launcher, not the layout. Listing only layout 1 means every row shown is a row
that fully works. Widen the scan only if Ember's card resolution changes to follow the resolved disc.

Two further limits, both measured:

- `io_find_disc` does **not** recurse. An image at `games/<Name>/disc1/game.cue` is not found.
- `.cue` is preferred over `.exe` over `.bin` when a folder holds more than one. `.exe` (PSX-EXE) is
  accepted and is undocumented in the README.

### Verifying a folder actually holds a disc

The scan lists directories without reading inside them: that would be one directory read per row on
every refresh, which MMCE and SMB cannot afford. Instead `cueGameHasImage()` runs once on the
**launch** path, before `deinit`, while a dialog can still be drawn. An empty or mis-filled folder
otherwise drops the user into the PS1 BIOS shell with no explanation. A probe that itself fails to
read reports success — never block a launch on a failed probe.

### Where `<device-root>` is, per device class

| Device | Root used |
| --- | --- |
| USB / MX4SIO / iLink / exFAT-ATA (BDM) | `massN:/` — the device **root**, not `gBDMPrefix` |
| MMCE | `mmceN:/` |
| SMB / ETH | the share root |
| APA / PFS HDD | `hdd0:__.EMBER[0-9]?` — a partition of its own, mounted on `pfs0:` and **kept mounted** across the handoff. Self-contained: `EMBER/ember.elf`, `EMBER/bios.bin` and `EMBER/games/` all start at the partition root. |
| UDPFS Files | the served filesystem root (`udpfs:`); PS1 enumeration is Ember-only |
| UDPFS IMG / UDPBD | the live `massN:` network block-device root; PS1 enumeration is Ember-only |

#### ⛔ `__common` is NOT a library location — say what actually lives there

An earlier draft of this document said *"`POPS` lives on `__common`"*, and that sentence is
ambiguous enough to have produced a real bug: the first version of the APA implementation scanned
`__common` for `EMBER/games/` on exactly that reasoning.

Precisely:

| Partition | Holds |
| --- | --- |
| `__common` | `POPS/POPSTARTER.ELF` — the **loader binary**, which `hddResolveHddPopstarter` mounts and loads. Also the OPL **data** home (`__common/OPL/ART/`, config) when `+OPL` is not in use. |
| `__.POPS`, `__.POPS0`…`9`, `PP.*.POPS.*` | The **`.VCD` games**. This is the only place POPS titles come from. |
| `__.EMBER`, `__.EMBER0`…`9` | The **Ember install and its games**, self-contained. |

The rule generalises: one location per kind of thing. A second place being mountable is not a reason
to look in it.

### Art and per-game config

Identity is the game folder name, so the device's normal rules apply unchanged:
`<devroot>ART/<Name>_COV.png` and `<devroot>CFG/<Name>.cfg`. There is no Ember-specific art
fallback. **One art location, ART/, for every game type; only the KEY differs** — a VCD keys on its
filename, an Ember title on its folder name, an app on its filename, and a PS2 title is unchanged.

Badges reuse `sbSetDiscAttributes(config, isPS1=1, isCD=1)` → `#System=PS1`, `#Media=CD`,
`#DiscType=PS1CD`, so an Ember row looks right in every shipped theme on day one.

## Part 2 — The view engine (L3)

### 2.1 What exists today

`src/vcdsupport.c` owned a **binary** per-mode view: `vcdView[mode]` (0 = ISO, 1 = VCD), a dirty
flag consumed in each support's `itemNeedsUpdate`, and a global lock `gDefaultGameView`.
`item_list_t.viewOverride` lets Favorites proxy a source device in a forced view without
disturbing that page's own L3 state.

### 2.2 What it becomes

The state moves to `libview.c`/`libview.h` and the second stop is renamed for what the user sees:

```c
enum LIB_VIEW {
    LIB_VIEW_ISO = 0, // PS2 disc games: ISO / ZSO / UL / HDL
    LIB_VIEW_PS1,     // PS1 titles -- BOTH cores in one list (Part 4)
    LIB_VIEW_ELF,     // homebrew ELFs -- Favorites only
    LIB_VIEW_ALL,     // every resolved favorite kind -- Favorites only
    LIB_VIEW_MIXED,   // combined PS2 + PS1 device rows, or all APPS rows
    LIB_VIEW_PS1_ELF, // APPS rows whose displayed title contains [PS1]
    LIB_VIEW_COUNT
};
```

Device-page rings now follow the four-value global display setting. Favorites always has four stops,
and APPS has its own one- or two-stop setting. The old binary name was one of the two PS1 cores, so
every caller had to ask "is it VCD?" to mean "is it PS1?"; the explicit enum also gives all mixed
lists a row-aware launch/menu path.

| Page | Ring |
| --- | --- |
| Device pages (BDM, MMCE, ETH, HDD, UDPFS) | **Both (L3):** `PS2 → PS1`; **Mixed:** `Mixed → PS2 → PS1`; locked PS2/PS1: one inert stop. UDPFS/UDPBD populate PS1 with Ember only |
| Favorites | `All in One → PS2 → PS1 → ELF →` wrap |
| Apps | **Mixed:** one inert list; **Apps / PS1ELF (L3):** `Apps → PS1ELF` using case-insensitive `[PS1]` title tagging |

Favorites keeps its own independent retained slot. **All in One** resolves each selected row's kind
before opening menus or launching; the filtered stops show only one shelf.

The ring machinery is deliberately kept general — supported-stop set, wrap-around advance — so
adding a stop later needs no second rewrite.

**API**

```c
int  libViewSupported(int mode, int view);      // the single definition of a page's ring
int  libViewRingSize(int mode);                 // what the hint and the toggle guard ask
int  libViewActive(int mode);
int  libListViewActive(const item_list_t *il);  // honours viewOverride
void libViewAdvance(int mode);                  // next stop, wrapping; marks dirty
int  libViewConsumeDirty(int mode);
void libViewMarkDirty(int mode);
void libViewMarkAllDirty(void);
```

`gDefaultGameView` preserves the existing stored values `BOTH` / `ISO` / `VCD` and appends `MIXED`;
the UI shows **Both (L3) / Mixed / PS2 / PS1** on both Interface and PS Emulation. Both and Mixed
enable their respective L3 rings. PS2 or PS1 pins applicable device pages and makes L3 fully inert.
Favorites and APPS are independent of this setting.

### 2.3 The conversion rule

Every old call asked a yes/no question and must become an explicit `LIB_VIEW_*` comparison. Do not
leave a boolean wrapper behind: one that answers "is it VCD?" answers wrongly for a `.cue` row.

Two sites changed *meaning* rather than spelling, and both were already asking the wrong question:
the L3 hint and `itemExecToggleView` tested "does this device have a VCD view" to decide whether to
offer the toggle. What they mean is "does this page have more than one list" — `libViewRingSize`.

**Audit gates (both required before merging this refactor):**

```bash
# Must be ZERO -- the binary API is deleted, not shadowed.
grep -rn "vcdViewActive\|vcdListViewActive\|vcdToggleView\|vcdConsumeDirty" src/ include/

# Per-file call-site counts, COMMENTS EXCLUDED, before vs after must match.
# A raw line count will mislead: comments mentioning the old names inflate the "before".
```

### 2.4 UI strings

The L3 hint became `PS1` and its toast `Showing PS1 games`. `VCD` is a bare token in all 31
`lng_fork` translations, so that swap is safe in every language; `VCD_OFF` ("disc games" and its
translations) is untouched because the PS2 list really is discs.

New labels go at the **END** of `lng_tmpl/_base.yml` — `.lng` files are consumed by line position.
`lang_autogen.h` and `lang_internal.c` are gitignored build artifacts; regenerate, never hand-edit.


## Part 3 — Scanning: `src/cuesupport.c` + `include/cuesupport.h`

Model it directly on `vcdsupport.c`'s scan half. POSIX dir IO only (`opendir`/`readdir`/`closedir`)
— the newlib-port rule; **never** `fileXio*` from the GUI thread.

```c
#define CUE_NAME_MAX        192  // Ember's argv_join buffer; we cap harder below
#define CUE_NAME_LAUNCH_MAX 180  // leaves room for "games/" + NUL inside Ember's 192-byte buffer
#define CUE_MAX_ITEMS       2048

// Build "<devPrefix><gEmberFolder>/ember.elf" into out; 1 if it exists.
int cueResolveEmber(const char *devPrefix, char *out, int outSize);

// Build "<devPrefix><gEmberFolder>/games/" into out.
void cueBuildGamesDir(const char *devPrefix, char *out, int outSize);

// Scan <devPrefix><gEmberFolder>/games/ for SUBDIRECTORIES. Returns count, or -1 on a device
// read failure (caller keeps its last-good list — same contract as vcdFillGameList).
int cueFillGameList(const char *devPrefix, base_game_info_t **outGames);

// 1 when a name is legal as an Ember argument (no '/', ':', '\\'; not "..";
// length <= CUE_NAME_LAUNCH_MAX). Pure string work, no device access.
int cueNameLaunchable(const char *name);

// NOTE: an earlier revision of this plan specified a cueLoadFolderCover() that searched inside the
// game folder when the ART/ lookup missed. That was never asked for and has been removed: covers
// come from ART/<name>_COV.png and nowhere else.
```

`cueFillGameList` fills `base_game_info_t` per entry:

```c
snprintf(g->name,      sizeof(g->name),      "%s", dirName);  // identity: art, CFG, favorites
snprintf(g->startup,   sizeof(g->startup),   "%s", dirName);
snprintf(g->extension, sizeof(g->extension), ".CUE");         // the kind discriminator
g->parts  = 1;
g->format = GAME_FORMAT_ISO;                                  // harmless; the view gates the launch
```

**`.CUE` in `extension` is the row-level kind discriminator** and is what makes the merged PS1 list
work (§4). `ISO_GAME_EXTENSION_MAX` is 4, so `".CUE"` fits exactly. Provide
`int cueIsCueEntry(const base_game_info_t *g)` = `!strcasecmp(g->extension, ".CUE")` and use it
everywhere rather than open-coding the compare.

**Scanner rules**

* Only subdirectories. `d_type` is untrustworthy on MMCE clones (see the MMCE theme-discovery
  finding) — probe with `opendir()` on the candidate and treat success as "is a directory".
* Skip `.` and `..`.
* Skip any name failing `cueNameLaunchable()`, and `LOG()` it. Such a name cannot occur on
  FAT/exFAT anyway; an over-long one is a user mistake, and listing an unlaunchable row would be a
  lying control.
* Do **not** verify that a `.cue`/`.bin`/`.exe` is present inside — that would be an `opendir` per
  row on the scan path and MMCE/SMB cannot afford it. Ember reports the miss itself
  (`"no disc (BIOS shell run)"`).
* Never touch the device from a display path. There is no CUE equivalent of
  `vcdResolveDisplayId()` and none should be added.

---

## Part 4 — One PS1 list, two cores

**Decided 2026-08-28 (Nathan): there are exactly two toggles, PS2 and PS1.** The PS1 list holds
both cores' titles together. This is not a setting and there is no third stop — merged *is* the
list. An earlier draft of this plan proposed a separate CUE stop plus an opt-in merge; that is
superseded, and the simpler shape is also the better one.

A PS1 list can therefore contain:

```
Spyro 2 (Ripto's Rage)      <- POPS/Spyro 2 (Ripto's Rage).VCD      -> POPSTARTER
Spyro 2 (Ripto's Rage)      <- EMBER/games/Spyro 2 (Ripto's Rage)/  -> Ember
```

Two rows, same title, different cores, sorted together. From the front end they behave identically.

**The rule that makes this work: the ROW picks the core, never the page.** Every decision that used
to key off "is this page in the VCD view" must key off the row instead:

| Decision | Ask |
| --- | --- |
| launch dispatch | `cueIsCueEntry(game)` → Ember, else POPSTARTER |
| art fallback | same test; `cueLoadFolderCover` vs `vcdLoadPopsCover` |
| favorite kind | same test, stored in the record |
| `#Size` skip | neither PS1 kind has a meaningful size — skip for the whole PS1 view |

The row-kind discriminator is `base_game_info_t.extension`: `".VCD"` or `".CUE"`.
`ISO_GAME_EXTENSION_MAX` is 4, so it fits exactly, and no struct change is needed.

`ps1FillGameList()` in `cuesupport.c` is the ONE place the union is formed. Its failure contract is
load-bearing: it returns −1 only when a scan could not **read** the device. An absent `POPS` or
`EMBER` folder is `0`, not failure — treating "this device only uses one core" as a failure would
freeze the PS1 page of most setups. When either half genuinely fails, the whole last-good list is
kept, because publishing half a list looks exactly like the user's titles disappearing.

**Sort.** The merged comparator reuses the VCD scan's existing visible-name key, exported as
`vcdSortKey()` so there is one definition rather than two that can drift. Enabling Ember therefore
cannot reorder an existing PS1 list. The extension tie-break is not cosmetic: the same game held for
both cores is the *expected* case, and without a deterministic tie-break `qsort` may swap those two
rows on every rescan, making them appear to jump around.

**Trap.** The PS1 array is a second store to free in every `itemCleanUp` and to bounds-guard in
every `*ActiveGame()` helper. The toggle-window guard exists because the L3 flip is synchronous
while the rebuild is deferred; extend it, do not copy-paste it.


## Part 5 — Launch

### 5.1 The primary handoff (keep-IOP) — `sysLaunchEmber()`

Add to `include/system.h` / `src/system.c`, directly beneath `sysLaunchPopstarter`:

```c
// Launch Ember for one game folder. emberElf = the full path of ember.elf (also becomes the
// target's argv[0], which is what sets Ember's cwd via ps2sdk __init_cwd — everything Ember opens,
// including bios.bin and games/, is relative to that directory). gameFolder = the BARE folder name
// under <emberdir>/games/; Ember hard-refuses '/', ':' and '\\' anywhere in it.
// Caller deinit()s with UNMOUNT_EXCEPTION first: Ember performs NO IOP reset, so the game device
// must still be mounted when it starts.
void sysLaunchEmber(const char *emberElf, const char *gameFolder);
```

```c
void sysLaunchEmber(const char *emberElf, const char *gameFolder)
{
    if (emberElf == NULL || gameFolder == NULL || gameFolder[0] == '\0') {
        LOG("[EMBER] null arg, abort\n");
        return;
    }
    char *argv[2];
    argv[0] = (char *)emberElf;    // ps2sdk __init_cwd derives Ember's cwd from this
    argv[1] = (char *)gameFolder;  // bare folder name; Ember joins argv[1..] with spaces
    LOG("[EMBER] elf=%s game=%s\n", emberElf, gameFolder);

    if (sysLoadELFKeepIOP(emberElf, "", 2, argv) < 0)
        LOG("[EMBER] keep-IOP handoff failed for %s\n", emberElf);
}
```

Budget check that `sysLoadELFKeepIOP` already enforces: `argc + 1 = 3` against the kernel's
15-string `SetArg` limit, and `strlen(emberElf)*2 + strlen(gameFolder) + 3` against the 256-byte
pool. With a 180-byte name cap and a ~48-byte device path this fits, but **pre-validate in the
caller** so the user sees `EMBER_NAME_TOO_LONG` instead of a silent return.

`SifExitRpc()` before `ExecPS2()` stays where it is inside `elfldr_noreset.c`. It has been removed
twice on a false premise; it is EE-side only, resets no IOP, and leaving it out yields a stale SIF0
handler DMAing into dead memory. **Do not touch it.**

### 5.2 Per-device `*LaunchCue()`

One per support, modelled line-for-line on `bdmLaunchVcd` / `mmceLaunchVcd`. Order matters and is
copied deliberately:

```c
static void bdmLaunchCue(item_list_t *itemList, const char *cueName, config_set_t *configSet)
{
    bdm_device_data_t *d = itemList->priv;
    char emberPrefix[64], emberElf[256], biosPath[256];

    if (d == NULL || cueName == NULL || cueName[0] == '\0')
        return;
    if (!cueNameLaunchable(cueName)) {                      // Ember refuses these outright
        guiMsgBox(_l(_STR_EMBER_NAME_BAD_CHARS), 0, NULL);
        return;
    }

    bdmBuildVcdPrefix(emberPrefix, sizeof(emberPrefix), itemList->mode);  // device ROOT
    if (!cueResolveEmber(emberPrefix, emberElf, sizeof(emberElf))) {
        guiMsgBox(_l(_STR_EMBER_NOT_FOUND), 0, NULL);
        return;
    }
    snprintf(biosPath, sizeof(biosPath), "%s%s/bios.bin", emberPrefix, gEmberFolder);
    if (!sbFileExists(biosPath)) {                           // Ember drops to the BIOS shell otherwise
        guiMsgBox(_l(_STR_EMBER_BIOS_MISSING), 0, NULL);
        return;
    }

    if (d->bdmDeviceType == BDM_TYPE_SDC) {                  // MX4SIO and MMCE share SIO2
        if (!cacheAbortMmceImageLoadsTimed(500)) {
            guiWarning(_l(_STR_ERR_FILE_INVALID), 8);
            return;
        }
    }

    deinit(UNMOUNT_EXCEPTION, itemList->mode);               // KEEP this device mounted — Ember never resets
    sysLaunchEmber(emberElf, cueName);
}
```

Notice what is **absent** compared to the VCD path, and why:

* **No memory-card preparation at all.** No `vcdInstallPopstarterMc`, no BDMA equip, no
  `IPCONFIG.DAT`/`SMBCONFIG.DAT`, no fat32/exFAT prompt. All of that exists because POPSTARTER
  resets the IOP and reloads drivers from `mc?:/POPSTARTER/`. Ember inherits our live drivers, so
  none of it applies. **Do not port it.** This is the single biggest simplification and the main
  reason Ember is worth supporting.
* **No UDPBD refusal.** `bdmLaunchVcd` must refuse UDPBD because POPSTARTER cannot bring a network
  block device back up after its reset. Ember has no reset, so this refusal is not needed — see R5.
* **RetroGEM barcode** (`vcdPrepareRetroGemBarcode`) is optional parity. It would need the disc ID
  read from inside the image. **Defer to Phase 5**; it is not on the critical path.

Wire it into each support's `item_list_t` and into `itemLaunch`:

```c
if (gAutoLaunchBDMGame == NULL && game != NULL && libListViewActive(itemList) == LIB_VIEW_CUE) {
    bdmLaunchCue(itemList, game->name, configSet);
    return;
}
```
…plus, when the merged PS1 list is on, the row-level `cueIsCueEntry(game)` dispatch from §4.

### 5.3 `item_list_t` extension — APPEND ONLY

Every support initialises `item_list_t` **positionally**. New members must go at the **very end**,
after `itemGetArtArchivePath`, or every existing initialiser silently mis-assigns:

```c
    /// Launch a CUE (PS1/Ember) item by its stored folder name, regardless of the device's current
    /// view. NULL for devices without a CUE view. Used by Favorites to launch a CUE favorite
    /// while its source device page may be in another view.
    void (*itemLaunchCue)(item_list_t *itemList, const char *cueName, config_set_t *configSet);
```

A unified `itemLaunchNamed(itemList, kind, name, configSet)` replacing `itemLaunchVcd` would be
cleaner, and the rebuild deviation rule permits the restructure — but it rewrites six support tables
in one go. **Recommendation: add `itemLaunchCue` alongside `itemLaunchVcd` now, and unify later if
a third named-launch kind ever appears.** Lower blast radius, identical behaviour.

**After ANY change to `item_list_t` or `base_game_info_t`: `rm obj/*.o` before rebuilding.** A
mid-struct field change with stale objects produces garbage that looks like a logic bug.

### 5.4 Failure surface

Ember's own failure mode is a quiet drop to the PS1 BIOS shell. Everything we can check cheaply, we
check *before* `deinit()` so we can still draw a dialog:

| Condition | Message |
| --- | --- |
| `ember.elf` missing at the resolved path | `EMBER_NOT_FOUND` |
| `bios.bin` missing | `EMBER_BIOS_MISSING` |
| Name contains `/`, `:`, `\` or is `..` | `EMBER_NAME_BAD_CHARS` |
| Name longer than `CUE_NAME_LAUNCH_MAX` | `EMBER_NAME_TOO_LONG` |
| `games/` empty or unreadable | `EMBER_NO_GAMES` (list-level, not a dialog) |

We deliberately do **not** pre-verify a `.cue`/`.bin`/`.exe` inside the folder (§3, scanner rules).

---

## Part 6 — Settings, config keys, favorites format, docs

### 6.1 New globals (`include/opl.h`) and keys (`include/config.h`)

```c
extern int  gEmberView;          // EMBER_VIEW_OFF / _AUTO (default) / _ALWAYS
extern char gEmberFolder[32];    // per-device folder name, default "EMBER"
extern int  gEmberCleanIop;      // fallback handoff, see R1 (default 0)
```

```c
#define CONFIG_OPL_EMBER_VIEW      "ember_view"      // 0=Off 1=Auto 2=Always
#define CONFIG_OPL_EMBER_FOLDER    "ember_folder"    // per-device folder holding ember.elf + games/
#define CONFIG_OPL_EMBER_CLEAN_IOP "ember_clean_iop" // reset+reload the IOP before handing off (fallback)
```

Load in `opl.c` alongside the POPSTARTER keys (~L2674), save (~L3187), default (~L4324).
Missing keys must fall back to the defaults above so existing `settings_riptopl.cfg` files load
unchanged.

### 6.2 Settings UI

Add an **Ember** page as a peer of the existing **POPStarter** page (`guiShowVcdConfig`), reached
from the same parent. Rows:

| Row | Control |
| --- | --- |
| Ember Library | enum: Off / Auto / Always (`gEmberView`) |
| Ember Folder | string, default shown when empty (`gEmberFolder`) — reuse `diaSetShowDefaultWhenEmpty` |
| Clean IOP handoff | on/off (`gEmberCleanIop`), hidden unless advanced/diag is set |

**Trap:** `diaSetEnum` stores the array *pointer*, it does not copy. Any `const char *[]` handed to
it must outlive every render of that dialog — make them `static`, as `guiSetBdmaSettings` does.

Changing `gEmberView` or `gEmberFolder` must call `libViewMarkAllDirty()` and
then `oplQueueVcdDeviceUpdates()` (rename it `oplQueueLibraryDeviceUpdates`) — marking dirty alone
is not enough for HDD, whose `updateDelay == -1` means a dirty view otherwise renders stale forever.

### 6.3 Favorites file format — OFAV v3

`FAV_VERSION` 2 → **3**. The per-record `isVcd` byte becomes a `kind` byte:

```
0 = ISO   1 = VCD   2 = CUE   3 = ELF
```

Read compatibility:

| File version | Mapping |
| --- | --- |
| v1 (no byte) | `kind = ISO` for every record (today's behaviour) |
| v2 (`isVcd` byte) | `isVcd == 1` → VCD; else ISO — unchanged, no records move |
| v3 | `kind` verbatim |

The disk filename remains `favourites.bin` for compatibility. The four Favorites views filter the
same records without rewriting or moving them: All in One accepts every kind, PS2 accepts ISO,
PS1 accepts VCD/CUE, and ELF accepts homebrew apps.

Record size line (`favsupport.c` ~L310) stays `17 + tlen` — one byte either way.

`addFavouriteItem` / `removeFavouriteByIdAndText` take `int kind` instead of `int isVcd`; kind is
part of the identity so an ISO, a VCD and a CUE of the same name never collide.
`favResolve()`'s VCD branch generalises: a CUE favorite binds to a source providing
`itemLaunchCue`; an ELF favorite binds to `APP_MODE`.

### 6.4 Docs

Per the standing rule (docs updated in the same effort the feature lands):

* New `docs/EMBER.md` — user-facing, mirroring `docs/VCD.md`'s structure: what the CUE view is, the
  folder layout, where to put `bios.bin`, the L3 ring, the combined-PS1 setting, and an explicit
  "Ember vs POPSTARTER — which should I use?" section.
* `docs/VCD.md` — update §1 and §2 (the view is no longer binary) and the Favorites paragraph.
* `README.md` — one line in the feature list.
* Rolling release notes.

---

## Part 7 — Risks, in priority order

### R1 — Pad modules (HIGHEST; this is the one that decides the architecture)

RiptOPL loads **`freesio2.irx` as `sio2man` and `freepad.irx` as `padman`** (`Makefile` L850/L853),
not the ROM versions. Ember calls `SifLoadModule("rom0:SIO2MAN")` then `SifLoadModule("rom0:PADMAN")`
against our still-live IOP and logs `"pad init %s (rc=%d)"`.

Two possible outcomes, and only hardware can tell them apart:

* **Benign:** the ROM loads fail as already-registered, Ember's `padInit()` binds to freepad's RPC
  (freepad is a drop-in libpad server), and controllers work. → ship as designed.
* **Broken:** a second SIO2MAN/PADMAN loads and fights freesio2/freepad → no input, or a hang.

`unloadPads()` (already called inside `deinit()`) calls `padEnd()`, which ends the RPC binding but
**does not unload the IRX** — the modules stay resident. There is no cheap way to unload them.

**Fallback if R1 comes back broken — `gEmberCleanIop`, "be the launcher ourselves":**
Replicate `launcher.elf` exactly, using IRX RiptOPL already embeds:

1. Read `ember.elf` into EE RAM **before** anything is torn down (3.7 MB; EE has 32 MB).
2. `deinit(UNMOUNT_EXCEPTION, mode)` as usual.
3. `SifIopReset("", 0)`; wait for sync; `SifInitRpc(0)`.
4. `sbv_patch_disable_prefix_check()`, `sbv_patch_enable_lmb()`.
5. Load `iomanx_irx`, `filexio_irx`, then the **device-matching** driver set (`usbd_irx` +
   `usbmass_bd_irx` for USB; `mx4sio_bd_irx`; `ata_bd_irx`; iLink; `mmceman_irx`; the SMB set),
   then `bdm_irx`, `bdmfs_fatfs_irx`.
6. Wait for the mount to appear, then `ExecPS2()` the in-RAM ELF with `argv[0]` = the ELF's original
   path string (so `__init_cwd` still resolves) and `argv[1]` = the folder name.

This is strictly more code and loses the "any device for free" property (each device needs its
driver set enumerated), which is why it is the fallback and not the default. **Phase 0 exists
specifically to answer R1 before any UI work is written.**

### R2 — Writable game folders

Ember creates `MC1.vmc` / `MC2.vmc` inside the game folder on first launch. A read-only source
(a locked SMB share, a read-only mount) will fail *inside* Ember, after we have already torn down.
Mitigation: document it, and consider a launch-time write probe
(`open(<gamedir>/.oplw, O_WRONLY|O_CREAT)` then `remove`) behind `__OPLDIAG` only — it costs a write
per launch and should not ship enabled.

### R3 — IOP RAM headroom

Ember calls `SifInitIopHeap` and loads modules into a **used** IOP. RiptOPL's IOP at menu time
carries iomanX, fileXio, sio2man, padman, mcman/mcserv, poweroff, usbd, the BDM stack, plus
whichever of mmceman / smbman+ps2ip+ps2smap+ps2dev9 / udpbd are live, plus audio. If Ember runs out
of IOP heap it will fail late and opaquely. Mitigation: `deinit(UNMOUNT_EXCEPTION, mode)` already
tears down every *other* device support before the handoff, which is most of the recovery. Watch for
it in Phase 0 and record it in the test log.

### R4 — SMB path separators

RiptOPL composes SMB paths with `\\` (see `sbCreateFoldersFromList`), while Ember composes with `/`
(`io_path` format string `"%s/%s%s"`). Whether smbman accepts `/` from Ember decides whether the CUE
view can exist on ETH at all. **Gate ETH support on a test**; if it fails, `libViewSupported()`
simply omits `LIB_VIEW_CUE` for `ETH_MODE` and nothing else changes.

### R5 — UDPBD / UDPFS (upside, not downside)

`bdmLaunchVcd` refuses UDPBD because POPSTARTER cannot rebuild a network block device after its
reset. Ember has no reset, so a UDPBD-hosted CUE game *should* just work — PS1 over the network,
which POPSTARTER can never do. Treat this as a bonus to validate in Phase 5, not a requirement.
Do **not** copy the `BDM_TYPE_UDPBD` refusal into `bdmLaunchCue`.

### R6 — `gDefaultGameView` semantics

Adding `GAME_VIEW_CUE` to a persisted enum is safe only if appended after `GAME_VIEW_VCD`.
Clamp on load (`opl.c` ~L2664 already clamps to `GAME_VIEW_BOTH`) and extend the clamp bound.

---

## Part 8 — Phasing and validation gates

Each phase is a separate PR. CI must be read line by line before merging — `check-format` is not a
required check, so a red format run still merges. Build and run **clang-format 12** locally first.

### Phase 0 — Prove the handoff (no UI)

Smallest possible change: a hardcoded `sysLaunchEmber()` reachable from a debug path. Note that the
existing APPS entry mechanism (`boot` + `argv1`) is *not* a shortcut here — `appLaunchItem` uses
`LoadELFFromFileWithPartition`, which resets the IOP, so it will fail. That failure is itself a
useful control experiment: it demonstrates §0.2 on hardware.

Ship an `__OPLDIAG` build (never `__DEBUG` — CI builds `make release`, so `__DEBUG` never reaches a
tester). Hand testers a run-pinned `nightly.link` artifact, per standing policy.

**Answers required before Phase 1 starts:**

1. Does the game boot at all through `sysLoadELFKeepIOP`? (validates §0.2)
2. **Do controllers work?** (R1 — decides architecture)
3. Does it work from MX4SIO / MMCE / internal exFAT ATA, not just USB? (validates "any device")
4. Does `bios.bin` load, and are `MC*.vmc` created? (R2)
5. Any sign of IOP heap exhaustion? (R3)

### Phase 1 — View engine only

`libView*` API in, all existing `vcdView*` call sites converted, `viewOverride` extended.
**No CUE stop is offered yet.** The ring for every page is still `ISO → VCD`.
Success criterion: **zero observable behaviour change.** This is the highest-risk refactor in the
plan and must land on its own so a regression is unambiguous.

**Audit gate before merge** — this is the query that catches the fork's recurring defect shape
(*data half ported, comment half ported, code half not*), and it is not optional:

```bash
# Must be ZERO — the binary API is deleted, not shadowed.
grep -rn "vcdViewActive\|vcdListViewActive\|vcdToggleView\|vcdConsumeDirty" src/ include/

# Call-site COUNT per file before and after must match; an N -> N-1 drop is the bug.
git diff --stat origin/rebuild/main -- src/ include/
```

### Phase 2 — CUE list on BDM only (landed)

This was the intentionally narrow first implementation. The shipped implementation now merges CUE
and VCD rows in `LIB_VIEW_PS1`, records favorites by row kind, and uses the presence of `EMBER/`
rather than a separate enable switch.

Validate: every display mode populates correctly; Both rings PS2 → PS1, Mixed rings
Mixed → PS2 → PS1, and Ember rows remain interleaved among the `.VCD` rows in each PS1-containing
view. Art resolves from `<devroot>ART/`; launch works; a device with no `EMBER/` folder looks exactly
as it did before.

### Phase 3 — MMCE + ETH, settings page, docs (landed)

Mirror into `mmcesupport.c` and (gated on R4) `ethsupport.c`. Add the Ember settings page, the
config keys, the language labels appended to `_base.yml`, `docs/EMBER.md`.

### Phase 4 — Favorites v3 + APA HDD (landed; hardware pending)

OFAV v3 carries the `kind` byte and v2 migration. The current Favorites ring is
All in One → PS2 → PS1 → ELF. There is no per-core merged-list setting — one PS1 view is the
behavior, see Part 4.

**APA HDD: landed.** `hddBuildVcdGameList` grows a second phase that mounts each `__.EMBER[0-9]?`
partition on the `pfs1:` scan slot and hands it to `cueScanDir`; rows join the existing VCD arrays
and are told apart by their `.CUE` extension. `hddDoLaunchEmber` quiesces art, remounts `pfs0:` on
the owning partition **RDWR**, and hands off with that mount live. `__.EMBER[0-9]?` is explicitly
excluded from the generic `__.<name>` POPSTARTER pass, and the Ember pass still runs when there are
zero POPSTARTER candidates, so an Ember-only drive is not mistaken for an empty PS1 library.

That last part needed a new teardown bit. `UNMOUNT_EXCEPTION` stops the `pfs0:` unmount, but
`hddCleanUp` also issues `PDIOC_CLOSEALL`, which drops every pfs descriptor in the IOP — safe under
its stated assumption that *"every path that reaches here hands off to an ELF that resets the IOP"*,
which Ember is the first launch to break. `KEEPIOP_EXCEPTION` (`include/iosupport.h`, `0x02`) now
gates that call.

Validate the migration explicitly: take a v2 `favourites.bin` holding ISO, VCD **and** app
favorites, confirm all three land on the right shelf after the upgrade, **and that a v3 file still
loads on the next boot** — the reader's accepted-version range and the writer's `FAV_VERSION` have
to move together, and getting that wrong presents an empty tab that then overwrites the real list.

### Phase 5 — Network surface (landed; hardware pending)

UDPFS and UDPBD now publish Ember-only PS1 views (R5); POPSTARTER remains unavailable on both.
RetroGEM game-ID parity and `settings.txt` display passthrough remain separate concerns.

---

## Part 9 — Quick reference for the implementing agent

| Thing | Where |
| --- | --- |
| No-reset ELF handoff | `src/elfldr_noreset.c` → `sysLoadELFKeepIOP()` |
| POPSTARTER launch (the template) | `src/system.c:1556` `sysLaunchPopstarter()` |
| Neutrino launch (keep-IOP + args, the other template) | `src/system.c:1308` `sysLaunchNeutrino()` |
| VCD view state | `src/vcdsupport.c:966-1035` |
| L3 handler | `src/opl.c:432` `itemExecToggleView()` |
| L3 hint | `src/opl.c:331` |
| Fav R3 capture | `src/opl.c:~525` `itemExecFav()` |
| BDM two-array pattern | `src/bdmsupport.c:1443-1600` (**contains NUL bytes — use `grep -a`**) |
| Fav record + versioning | `src/favsupport.c:38-120`, `240-340` |
| Fav resolve/launch/art | `src/favsupport.c:448-560`, `713-760` |
| Device badges | `src/supportbase.c:1218` `sbSetDiscAttributes()` |
| POPSTARTER settings page (UI template) | `src/gui.c:1356` `guiShowVcdConfig()` |
| Language template (append at END) | `lng_tmpl/_base.yml`, after `GENERAL_SYSTEM` |

**Standing rules that apply to this work**

* `lng_tmpl/_base.yml` is **append-only**; `.lng` files are read by line position.
  `lang_autogen.h` and `lang_internal.c` are gitignored build artifacts.
* Any `item_list_t` / `base_game_info_t` change → **`rm obj/*.o`** before rebuilding.
* `diaSetEnum` keeps a raw pointer — its string array must outlive every dialog render.
* Never take an ioman semaphore from the GUI thread.
* Tester diagnostics go behind `__OPLDIAG`, never `__DEBUG` (CI builds `make release`).
* Library folders live at the **root of each activated device** — never on `mc`, never in cwd.
  Settings live in cwd.
* `SifExitRpc()` before `ExecPS2()` in `elfldr` must stay.
