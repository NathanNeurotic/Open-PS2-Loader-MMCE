/*
  Copyright 2026, Open-PS2-Loader contributors
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.

  CUE (PS1-via-Ember) support. Ember (github.com/Gageformer/Ember) is a native PS1 emulator for the
  PS2. RiptOPL drives ember.elf directly and replaces Ember's own bundled launcher.elf, with the
  author's blessing.

  Ember's library sits at <device>:/EMBER/, the exact peer of POPSTARTER's <device>:/POPS/ -- one
  folder at each activated device's root, sought the same way on every device class. That is where
  the resemblance ENDS, and the difference is not cosmetic:

      POPS/   holds loose *.VCD FILES        -> scan for files, identity = basename minus ".VCD"
      EMBER/  holds ember.elf, bios.bin and
              a games/ folder of per-game
              DIRECTORIES                    -> scan a subfolder for DIRECTORIES, identity = the
                                                directory name (there is no extension to strip)

  WHICH LAYOUTS EMBER ACCEPTS, AND WHY WE LIST ONLY ONE. Ember's resolver is looser than its README:
  it tries "games/<arg>" and then "<arg>", and io_find_disc takes EITHER a directory containing a
  *.cue / *.bin / *.exe OR such a file directly. So all four of these boot:

      1. games/<Name>/            a directory holding the image   <- the README layout
      2. games/<Name>.cue         a loose image file
      3. <Name>/                  a directory beside ember.elf     <- via the "retry arg as path" leg
      4. <Name>.cue               a loose file beside ember.elf    <- same leg

  We deliberately list ONLY layout 1, because only layout 1 saves. main() calls memcard_init exactly
  once, and always with the COMPOSED path "games/<arg>" -- never with wherever the disc was actually
  found. The per-game cards are then "games/<arg>/MC1.vmc" and "MC2.vmc". So:

      - layout 2 puts the cards under a FILE ("games/Spyro.cue/MC1.vmc"), which cannot exist;
      - layouts 3 and 4 put the cards in "games/<Name>/", a directory unrelated to the disc that
        was actually mounted from EMBER/<Name>/.

  Those titles play and silently cannot save, which is a worse experience than not being offered --
  and the user would blame RiptOPL, not the layout. Listing only layout 1 means every row we show is
  a row that fully works. Do NOT "fix" this by widening the scan; widen it only if Ember's memory
  card resolution changes to follow the resolved disc.

  Note also that io_find_disc does not recurse: an image at games/<Name>/disc1/game.cue is NOT found.
  One level only.

  ONE PS1 LIST, TWO CORES. A device page has two stops -- PS2 discs and PS1 -- and the PS1 stop
  lists both cores' titles together, sorted as one library:

      Spyro 2 (Ripto's Rage)        <- POPS/Spyro 2 (Ripto's Rage).VCD   -> POPSTARTER
      Spyro 2 (Ripto's Rage)        <- EMBER/games/Spyro 2 (Ripto's Rage)/ -> Ember

  Both rows behave identically to the user; which core runs one is a property of the ROW, resolved
  at launch, and never of the page. Ask cueIsCueEntry(row) -- never the view.

  Every Ember constant and rule below was MEASURED by disassembling the shipped Beta ember.elf (it
  ships unstripped, with DWARF), not read off the README -- the README is thinner and, on the
  accepted file extensions, wrong. The derivation is in docs/EMBER-INTEGRATION-PLAN.md Part 0.

  Two facts shape this file:

  1. ember.elf performs NO IOP reset. It has sceSifInitRpc/SifLoadModule/SifInitIopHeap and no
     SifIopReset, so it inherits the launcher's live driver stack. The handoff therefore uses
     sysLoadELFKeepIOP() (sysLaunchEmber in system.c) and the caller must deinit with
     UNMOUNT_EXCEPTION. It is also why Ember can reach any device OPL can mount, unlike POPSTARTER,
     which resets and re-discovers from the memory card.

  2. Ember's game argument is a BARE FOLDER NAME. Its argv scan hard-refuses '/', ':' and '\\'
     anywhere in the argument and refuses a leading "..", then resolves "games/<name>" followed by
     "<name>", both relative to a working directory ps2sdk derives from argv[0]. A path can never
     be passed. That is why the games live under the ember.elf's own directory, and why there is
     deliberately no POPSTARTER-style "Ember device" picker -- such a control could not work.

  POSIX directory IO only (opendir/readdir/closedir), like vcdsupport.c: the newlib port rejects
  direct fileXio use from these paths.
*/

#ifndef __CUESUPPORT_H
#define __CUESUPPORT_H

#include "include/iosupport.h"
#include "include/supportbase.h" // base_game_info_t

// Ember joins argv[1..] with spaces into a 192-byte buffer, then snprintf()s "games/%s" into a
// second 192-byte buffer. A longer name cannot round-trip, so refuse it with a message rather than
// hand the target something it will silently truncate.
#define CUE_NAME_MAX        192
#define CUE_NAME_LAUNCH_MAX 180 // 192 less "games/" and the NUL, with headroom
#define CUE_MAX_ITEMS       2048

// Per-device folder holding ember.elf + bios.bin + games/, the peer of POPS_FOLDER. The NAME is the
// only knob Ember's argument contract leaves us (the library must be the ember.elf's own
// directory) -- read it through cueEmberFolder(), never the macro.
#define EMBER_FOLDER_DEFAULT "EMBER"
#define EMBER_ELF_NAME       "ember.elf"
#define EMBER_BIOS_NAME      "bios.bin"
#define EMBER_GAMES_FOLDER   "games"

// The extension stamped into base_game_info_t for an Ember row. This is the ROW-KIND discriminator
// that makes one merged PS1 list possible: a .VCD row launches through POPSTARTER, a .CUE row
// through Ember, and nothing else about them differs. ISO_GAME_EXTENSION_MAX is 4, so it fits.
#define CUE_ROW_EXTENSION ".CUE"

typedef struct
{
    char name[CUE_NAME_MAX]; // game FOLDER name under EMBER/games/ -- the launch argument and identity
} cue_entry_t;

// The active per-device Ember folder name. Never NULL.
const char *cueEmberFolder(void);

// Build "<devPrefix><EmberFolder><sep>ember.elf" into out; returns 1 when that file exists, else 0.
// devPrefix is a device ROOT ending in its separator ("mass0:/", "mmce0:/", an SMB prefix ending
// "\\"), NOT gBDMPrefix -- Ember reads its own directory and nothing else.
int cueResolveEmber(const char *devPrefix, char *out, int outSize);

// As cueResolveEmber, for the user-supplied bios.bin Ember needs at launch. A missing BIOS drops
// Ember to the PS1 shell with no explanation, so callers check this BEFORE deinit and say so.
int cueResolveEmberBios(const char *devPrefix, char *out, int outSize);

// Build "<devPrefix><EmberFolder><sep>games" into out (no trailing separator, ready for opendir).
void cueBuildGamesDir(const char *devPrefix, char *out, int outSize);

// 1 when `name` is legal as Ember's game argument. Mirrors Ember's own checks exactly rather than a
// tidied version of them: any '/', ':' or '\\' refuses (its char scan at main+0x3d48), and so does
// a name STARTING with ".." (main+0x4210 -- not only the exact string ".."). Pure string work.
int cueNameLaunchable(const char *name);

// Scan "<devPrefix><EmberFolder>/games/" for game SUBDIRECTORIES. Returns the count; *outList is a
// calloc'd cue_entry_t array the caller frees. Returns -1 only when the directory could not be READ
// (contended bus): an absent EMBER folder is 0, "readable, nothing here", exactly as the VCD scan
// treats an absent POPS folder -- so a device with only one of the two never looks like a failure.
int cueScanDir(const char *devPrefix, cue_entry_t **outList);

// Does this game folder actually hold something Ember can mount -- a *.cue, *.bin or *.exe at its
// top level? Costs ONE directory read, so callers use it on the LAUNCH path only (before deinit,
// while a dialog can still be drawn) and never on the scan path, where it would be one directory
// read per row on every refresh -- unaffordable on MMCE and SMB. An empty or mis-filled folder is a
// user mistake rather than a normal state, so paying for it once per launch is the right trade.
// Returns 1 when an image is present, 0 when the folder is readable and holds none, and 1 when the
// folder cannot be read at all (never block a launch on a probe that itself failed).
int cueGameHasImage(const char *devPrefix, const char *name);

// 1 when a published row is an Ember title rather than a POPSTARTER one. THE row-kind test; use it
// for launch dispatch and art fallback instead of asking which view the page is on.
int cueIsCueEntry(const base_game_info_t *game);

// Which core owns the PS1 row called `name` in an already-published list: 1 = Ember, 0 = POPSTARTER,
// -1 = not found. Pure memory scan, no device access -- for callers like a device getImage that are
// handed a name rather than the row.
int cueRowIsCueByName(const base_game_info_t *games, int count, const char *name);

// Rename an Ember game by renaming its FOLDER under EMBER/games/. The folder name IS the identity
// -- launch argument, art key, config key -- so this is the exact peer of vcdRenameFile, which
// renames the .VCD file. Returns 0 on success, -1 otherwise (bad name, folder absent, or the
// filesystem refusing a directory rename). Nothing is moved on failure.
int cueRenameGame(const char *devPrefix, const char *oldName, const char *newName);

// Fill a base_game_info_t list with the device's COMPLETE PS1 library: POPSTARTER *.VCD entries and
// Ember game folders, merged and sorted together as one list (frees *outGames first, memalign'd
// like sbReadList). Returns the count, or -1 when EITHER half could not read the device -- the
// caller then keeps its last-good list rather than blanking a page over a transient wedge.
// This is the ONE place the two libraries are unioned.
//
// devPrefix is the device ROOT for BOTH libraries -- POPS/ and EMBER/ are peers there, on every
// device class. See the note at the top of this header for why the root, and not a configurable
// games prefix, is the right home for them.
int ps1FillGameList(const char *devPrefix, base_game_info_t **outGames);

// PS1 (Ember) cover FALLBACK inside the game's own folder: "<games>/<name>/cover.png", then
// "<games>/<name>/<name>.png". Cover/icon suffixes only; a device getImage calls this ONLY after
// its own <dev>ART/<name>_<suffix>.png misses, and only for a CUE row. The POPSTARTER peer is
// vcdLoadPopsCover. Returns texDiscoverLoad's result (>= 0 hit, negative miss).
int cueLoadFolderCover(const char *devPrefix, const char *value, const char *suffix, GSTEXTURE *resultTex);

#endif
