# Ember as a second PS1 core — integration plan

**Status:** phases 0 and 1 landed (see Implementation status below); phases 2-5 pending hardware.
**Base commit:** `291716d3` on `rebuild/main` (2026-08-27).
**Permission:** Gage (author of Ember) has approved and encouraged this integration.

---

## Implementation status

Branch `feat/ember-ps1-core`. Update this table as phases land; it is the first thing the next
agent should read.

| Phase | State | Notes |
| --- | --- | --- |
| 0 — prove the handoff | **code landed, HARDWARE PENDING** | `sysLaunchEmber()` + `cuesupport.c` + an OPLDIAG-only interception in `appLaunchItem`. **Blocks phases 2-5.** See "How to run the Phase 0 test" below. |
| 1 — view engine | **landed** | `libview.c`/`libview.h` own the ring; the binary `vcdView` API is deleted, not shadowed. Both audit gates pass (zero old-API references; per-file call-site counts identical). Rings still ISO -> VCD only, so behaviour is unchanged. |
| 2 — CUE list on BDM | not started | gated on Phase 0 |
| 3 — MMCE + ETH, settings, docs | not started | gated on Phase 0 |
| 4 — Favourites v3, merged list, APA | not started | gated on Phase 0 |
| 5 — bonus surface | not started | gated on Phase 0 |

### How to run the Phase 0 test

Build with `OPLDIAG=1` (a release build deliberately does NOT include the probe). On the test
device, lay out:

```
mass0:/EMBER/ember.elf      <- from Ember.Beta.Release.zip
mass0:/EMBER/bios.bin       <- your own PS1 BIOS, exactly 512 KB
mass0:/EMBER/games/Crash Bandicoot (USA)/game.cue  (+ game.bin)
```

Add an APPS entry pointing at it — `boot` = `mass0:/EMBER/ember.elf`, `argv1` =
`Crash Bandicoot (USA)` — and launch it from the APPS page.

**What to report back**

1. Does the game boot at all?
2. **Do the controllers work?** This is risk R1 and it decides whether the rest of the design
   stands as written or needs the clean-IOP fallback.
3. Does it work from MX4SIO / MMCE / internal exFAT ATA, not just USB? (Use `mass1:` etc.)
4. Does `bios.bin` load, and do `MC1.vmc` / `MC2.vmc` appear in the game folder afterwards?
5. Anything that looks like IOP memory exhaustion (late, unexplained failure).

The control case: the identical APPS entry on a **release** build is expected to fail, because
`LoadELFFromFileWithPartition` resets the IOP out from under Ember. That contrast is the
measurement — if the release build also works, the no-reset premise needs re-examining.

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

Ember cannot be given a path, so the layout is forced: **the games live under the same directory as
the `ember.elf` we launch.** Per-device, mirroring the existing `POPS/` doctrine
(*library folders at the ROOT of each activated device — never mc, never cwd*):

```
<device-root>/
  EMBER/                      <- folder name configurable (gEmberFolder, default "EMBER")
    ember.elf                 <- required; its presence is what enables the CUE view
    bios.bin                  <- required by Ember at launch (512 KB, user-supplied)
    settings.txt              <- optional, Ember's own
    games/
      Crash Bandicoot (USA)/  <- one folder == one CUE library entry; the folder NAME is the identity
        game.cue
        game.bin
        MC1.vmc  MC2.vmc      <- created by Ember on first launch
      Spyro the Dragon (USA)/
        Spyro.cue
        Spyro.bin
```

`<device-root>` per device class, matching where `POPS/` already lives:

| Device | Root used |
| --- | --- |
| USB / MX4SIO / iLink / exFAT-ATA (all BDM) | `massN:/` — the **device root**, not `gBDMPrefix`. Reuse `bdmBuildVcdPrefix()`. |
| MMCE | `mmceN:/` (`mmcePrefix`) |
| SMB / ETH | the mounted share root (`ethGetSMBPrefix()`) — **validation-gated, see Risk R4** |
| APA / PFS HDD | `pfs0:/EMBER/…` on the selected OPL partition (`+OPL`, else `__common`) — **Phase 4** |
| UDPBD / UDPFS | the live `massN:` / udpfs mount — **bonus, see Risk R5** |

**Invariant to enforce in code:** the scanned `games/` directory and the launched `ember.elf` are
always the *same* `EMBER/` directory on the *same* device. There is deliberately **no**
POPSTARTER-style "Ember Device" picker: POPSTARTER can be loaded from anywhere because it takes a
`mass:/POPS/XX.<name>.ELF` selector string, but Ember takes a bare folder name and can only look
inside its own directory. A picker would be a lying control. The only knob is the folder *name*
(`gEmberFolder`), which lets a user keep e.g. `PS1/` instead of `EMBER/`.

### Art and per-game config

Identity = **the game folder name**, exactly parallel to a VCD's basename-without-`.VCD`. So the
existing device art/CFG rules apply unchanged:

* `<devroot>ART/<Folder Name>_COV.png`, `<devroot>CFG/<Folder Name>.cfg`
* Fallback, mirroring `vcdLoadPopsCover()`: `<devroot>EMBER/games/<Folder Name>/cover.png`, then
  `<devroot>EMBER/games/<Folder Name>/<Folder Name>.png`. Cover/icon suffixes only, CUE view only.

Badges: reuse `sbSetDiscAttributes(config, /*isPS1*/1, /*isCD*/1)` → `#System=PS1`, `#Media=CD`,
`#DiscType=PS1CD`. Every shipped theme already draws these, so CUE entries look right on day one.
Optionally also stamp `CONFIG_ITEM_FORMAT = "CUE"` so a theme can add a `CUE_#Format.png` glyph —
purely additive, no existing theme changes.

---

## Part 2 — The view engine (L3)

### 2.1 What exists today

`src/vcdsupport.c` owns a **binary** per-mode view: `vcdView[mode]` (0=ISO, 1=VCD), a dirty flag
consumed in each support's `itemNeedsUpdate`, and a global lock `gDefaultGameView`
(`GAME_VIEW_BOTH` / `_ISO` / `_VCD`). `item_list_t.viewOverride` lets Favourites proxy a source
device in a forced view without disturbing that page's own L3 state
(`ITEM_VIEW_NATIVE` / `_FORCE_ISO` / `_FORCE_VCD`).

### 2.2 What it becomes

Generalise the binary flag into an **N-way ring**. Keep the state in `vcdsupport.c` (it already owns
the dirty-flag protocol every support consumes) or move it to a new `src/libview.c` +
`include/libview.h` — either is fine; what matters is that the old binary API is **deleted, not
shadowed** (see the audit gate in §8, Phase 1).

```c
enum LIB_VIEW {
    LIB_VIEW_ISO = 0,   // disc games (PS2 ISO/ZSO/UL/HDL) — today's vcdView == 0
    LIB_VIEW_VCD,       // PS1 via POPSTARTER               — today's vcdView == 1
    LIB_VIEW_CUE,       // PS1 via Ember                    — new
    LIB_VIEW_ELF,       // homebrew ELFs                    — Favourites only
    LIB_VIEW_COUNT
};
```

**Rings (the order Nathan specified):**

| Page | Ring |
| --- | --- |
| Device pages (BDM, MMCE, ETH, HDD) | `ISO → VCD → CUE →` wrap |
| Favourites (`FAV_MODE`) | `ISO → VCD → CUE → ELF →` wrap |
| Apps (`APP_MODE`) | none — ELF only, L3 inert, no hint |

**Combined PS1 list setting** (`gEmberSharePs1List`, default OFF). When ON:

| Page | Ring |
| --- | --- |
| Device pages | `ISO → PS1 →` wrap, where PS1 = VCD entries **and** CUE entries merged |
| Favourites | `ISO → PS1 → ELF →` wrap |

This is the "removing the need for a 3rd toggle if the user wants PS1 all together" requirement.

**Stop suppression.** A ring stop is only offered when it can actually produce something, so nobody
gets a dead L3 press:

* `LIB_VIEW_VCD` — offered when `vcdModeSupported(mode)` (unchanged).
* `LIB_VIEW_CUE` — governed by `gEmberView`: `Off` / `Auto` (default) / `Always`.
  In `Auto`, the stop appears only when `<devroot><gEmberFolder>/ember.elf` exists. That probe is
  one `open()`+`close()`, folded into the support's existing `itemNeedsUpdate` device tick and
  memoised per `gCacheGeneration` — **never** on the render path.
* `LIB_VIEW_ELF` — Favourites only, and only when `gFAVStartMode` is on.

**API to add** (replacing the binary calls one-for-one):

```c
int  libViewSupported(int mode, int view);      // is this stop in this mode's ring?
int  libViewActive(int mode);                   // current LIB_VIEW_* for the page
int  libListViewActive(const item_list_t *il);  // honours viewOverride, else libViewActive(il->mode)
void libViewAdvance(int mode);                  // L3: next supported stop, wrapping; marks dirty
int  libViewConsumeDirty(int mode);             // unchanged semantics from vcdConsumeDirty
void libViewMarkDirty(int mode);
void libViewMarkAllDirty(void);
```

`gDefaultGameView` gains `GAME_VIEW_CUE` **appended after `GAME_VIEW_VCD`** so persisted ints do not
shift. `GAME_VIEW_BOTH` continues to mean "L3 rings"; the lock values pin the page and make L3
inert, exactly as today.

`item_list_t.viewOverride` gains `ITEM_VIEW_FORCE_CUE = 3` and `ITEM_VIEW_FORCE_ELF = 4`.

### 2.3 Call-site conversion table

Every one of these currently asks a **yes/no** question and must be re-expressed as an explicit
`LIB_VIEW_*` comparison. Do not leave a `vcdViewActive()` wrapper behind — a wrapper that answers
"is it VCD?" will silently answer "no" for a CUE page and route CUE rows down the ISO path, which is
precisely the failure shape the pre-v1.0 audit catalogued (data half ported, code half not).

| File | Sites | Becomes |
| --- | --- | --- |
| `src/opl.c` | `itemExecToggleView`, hint block (~L331), `itemExecSquare` `#Size` skip, `itemExecFav` `isVcd` capture, `itemExecTriangle` VCD routing, `oplQueueVcdDeviceUpdates` | ring advance; hint text per view; skip `#Size` for VCD **and** CUE; capture a `kind`; route both PS1 kinds away from the PS2 per-game settings |
| `src/bdmsupport.c` | `bdmNeedsUpdate`, `bdmUpdateGameList`, `bdmGetGameCount`, `bdmActiveGame`, delete/rename guards, `bdmGetImage` fallback, `bdmLaunchGame` dispatch, cleanup frees | third array `bdmCueGames`/`bdmCueGameCount` alongside `bdmGames`/`bdmVcdGames`, same last-good-on-failure discipline |
| `src/mmcesupport.c` | mirror of the above | same |
| `src/ethsupport.c` | mirror | same |
| `src/hddsupport.c` | mirror (+ APA specifics) | Phase 4 |
| `src/udpfssupport.c` | currently VCD-unsupported | leave unsupported unless R5 passes |
| `src/favsupport.c` | `favUpdateItemList` filter, `favResolve`, `favOwnerView`, `favGetConfig`, `favLaunchItem`, `favGetImage`, `favResolveStoredId` | `kind` instead of `isVcd` throughout |
| `src/guigame.c` | VCD per-game routing | treat CUE like VCD (no Loader Core, no compat flags) |
| `src/texcache.c` | `cacheGetEffectiveMode` via `favGetArtMode` | unchanged shape, `kind`-aware |

**Note the `bdmsupport.c` gotcha:** the file contains NUL bytes, so `grep` treats it as binary.
Use `grep -a` / `tr -d '\000'` when reading it.

### 2.4 UI strings

Existing `_STR_VCD` (L3 hint), `_STR_VCD_ON`, `_STR_VCD_OFF` are binary. Replace the hint with a
per-view label and add toast strings. **All new labels go at the END of `lng_tmpl/_base.yml`**
(currently after `GENERAL_SYSTEM`) — `.lng` files are consumed by line position and this rule has
been broken three times already. Suggested new labels:

```
VIEW_TOGGLE, VIEW_ISO, VIEW_VCD, VIEW_CUE, VIEW_ELF, VIEW_PS1,
EMBER, EMBER_VIEW, HINT_EMBER_VIEW, EMBER_FOLDER, HINT_EMBER_FOLDER,
EMBER_SHARE_PS1, HINT_EMBER_SHARE_PS1, EMBER_CLEAN_IOP, HINT_EMBER_CLEAN_IOP,
EMBER_NOT_FOUND, EMBER_BIOS_MISSING, EMBER_NAME_TOO_LONG, EMBER_NAME_BAD_CHARS,
EMBER_NO_GAMES
```

`lang_autogen.h` and `lang_internal.c` are **gitignored build artifacts** — regenerate, do not
hand-edit.

---

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

// Cover fallback inside the game folder (cover.png, then <name>.png). Only called after the
// device's own ART/ lookup misses, and only in the CUE view.
int cueLoadFolderCover(const char *devPrefix, const char *value, const char *suffix, GSTEXTURE *tex);
```

`cueFillGameList` fills `base_game_info_t` per entry:

```c
snprintf(g->name,      sizeof(g->name),      "%s", dirName);  // identity: art, CFG, favourites
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

## Part 4 — Merged PS1 list (`gEmberSharePs1List`)

When ON, `LIB_VIEW_VCD` and `LIB_VIEW_CUE` collapse into one stop labelled **PS1**.

Implementation: keep the two arrays scanned separately (they have different failure modes and
different last-good semantics), then build a **third, merged view array** at publish time:

```
ps1Games = memalign(64, (vcdCount + cueCount) * sizeof(base_game_info_t));
memcpy VCD rows, then CUE rows, then sort by name with the existing submenu sort.
```

Every downstream decision then keys off the **row**, not the page:

| Decision | Today | With merged list |
| --- | --- | --- |
| launch dispatch | `if (vcdViewActive(mode))` | `if (cueIsCueEntry(g))` → Ember; else → POPSTARTER |
| art fallback | `vcdLoadPopsCover` | `cueIsCueEntry(g) ? cueLoadFolderCover : vcdLoadPopsCover` |
| `#Size` skip | view-based | both PS1 kinds skip |
| favourite `kind` | `isVcd` | `cueIsCueEntry(g) ? CUE : VCD` |

**Trap (already bitten once, see the VCD-sort note):** `submenuSort` runs *last*, on raw display
text, and it must stay aware of `vcdDisplayName()` (the game-ID-hiding cosmetic transform). A merged
list runs both name shapes through the same sort, so verify `gVcdHideGameId` still only affects VCD
rows and never mangles a CUE folder name.

**Trap:** the CUE array is a third store to free in every `itemCleanUp` and to bounds-guard in every
`*ActiveGame()` helper. The `bdmActiveGame` toggle-window guard exists precisely because the L3 flip
is synchronous while the rebuild is deferred; with three (or four) arrays the guard is mandatory,
not optional. Extend it, do not copy-paste it.

---

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
    /// view. NULL for devices without a CUE view. Used by Favourites to launch a CUE favourite
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

## Part 6 — Settings, config keys, favourites format, docs

### 6.1 New globals (`include/opl.h`) and keys (`include/config.h`)

```c
extern int  gEmberView;          // EMBER_VIEW_OFF / _AUTO (default) / _ALWAYS
extern char gEmberFolder[32];    // per-device folder name, default "EMBER"
extern int  gEmberSharePs1List;  // merge the VCD and CUE lists into one PS1 stop (default 0)
extern int  gEmberCleanIop;      // fallback handoff, see R1 (default 0)
```

```c
#define CONFIG_OPL_EMBER_VIEW      "ember_view"      // 0=Off 1=Auto 2=Always
#define CONFIG_OPL_EMBER_FOLDER    "ember_folder"    // per-device folder holding ember.elf + games/
#define CONFIG_OPL_EMBER_SHARE_PS1 "ember_share_ps1" // one merged PS1 list instead of VCD + CUE stops
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
| Combine VCD + CUE into one PS1 list | on/off (`gEmberSharePs1List`) |
| Clean IOP handoff | on/off (`gEmberCleanIop`), hidden unless advanced/diag is set |

**Trap:** `diaSetEnum` stores the array *pointer*, it does not copy. Any `const char *[]` handed to
it must outlive every render of that dialog — make them `static`, as `guiSetBdmaSettings` does.

Changing `gEmberView`, `gEmberFolder` or `gEmberSharePs1List` must call `libViewMarkAllDirty()` and
then `oplQueueVcdDeviceUpdates()` (rename it `oplQueueLibraryDeviceUpdates`) — marking dirty alone
is not enough for HDD, whose `updateDelay == -1` means a dirty view otherwise renders stale forever.

### 6.3 Favourites file format — OFAV v3

`FAV_VERSION` 2 → **3**. The per-record `isVcd` byte becomes a `kind` byte:

```
0 = ISO   1 = VCD   2 = CUE   3 = ELF
```

Read compatibility:

| File version | Mapping |
| --- | --- |
| v1 (no byte) | `kind = ISO` for every record (today's behaviour) |
| v2 (`isVcd` byte) | `isVcd == 1` → VCD; `isVcd == 0 && mode == APP_MODE` → **ELF**; else ISO |
| v3 | `kind` verbatim |

The v2 → ELF remap is the one behaviour change existing users will see: app favourites currently sit
in the FAV tab's ISO list and will move to the new ELF stop. That is exactly the requested
`ISO → VCD → CUE → ELF` ring, but it must be called out in the release notes, because a user with
app favourites will otherwise think they vanished.

Record size line (`favsupport.c` ~L310) stays `17 + tlen` — one byte either way.

`addFavouriteItem` / `removeFavouriteByIdAndText` take `int kind` instead of `int isVcd`; kind is
part of the identity so an ISO, a VCD and a CUE of the same name never collide.
`favResolve()`'s VCD branch generalises: a CUE favourite binds to a source providing
`itemLaunchCue`; an ELF favourite binds to `APP_MODE`.

### 6.4 Docs

Per the standing rule (docs updated in the same effort the feature lands):

* New `docs/EMBER.md` — user-facing, mirroring `docs/VCD.md`'s structure: what the CUE view is, the
  folder layout, where to put `bios.bin`, the L3 ring, the combined-PS1 setting, and an explicit
  "Ember vs POPSTARTER — which should I use?" section.
* `docs/VCD.md` — update §1 and §2 (the view is no longer binary) and the Favourites paragraph.
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

### Phase 2 — CUE list on BDM only

`cuesupport.c`, the third array in `bdmsupport.c`, `bdmLaunchCue`, `LIB_VIEW_CUE` in the BDM ring
under `gEmberView`. No favourites, no merged list, no settings page yet (hardcode `gEmberView =
AUTO`, `gEmberFolder = "EMBER"`).

Validate: list populates; L3 rings ISO → VCD → CUE; art resolves from `<devroot>ART/`; launch works;
a device with no `EMBER/` folder shows exactly today's two-stop ring.

### Phase 3 — MMCE + ETH, settings page, docs

Mirror into `mmcesupport.c` and (gated on R4) `ethsupport.c`. Add the Ember settings page, the
config keys, the language labels appended to `_base.yml`, `docs/EMBER.md`.

### Phase 4 — Favourites v3 + merged PS1 list + APA HDD

OFAV v3 with the `kind` byte and the v2 migration; the `ISO → VCD → CUE → ELF` FAV ring; the
`gEmberSharePs1List` merged list; `hddsupport.c` (`pfs0:/EMBER/`).

Validate the migration explicitly: take a v2 `favourites.bin` with ISO, VCD **and** app favourites,
confirm all three land on the right stop after the upgrade and that a v3 file still loads on the
next boot.

### Phase 5 — Bonus surface

UDPBD/UDPFS (R5), RetroGEM game-ID parity, `settings.txt` display passthrough as a per-game option.

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
