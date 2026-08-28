/*
  Copyright 2026, Open-PS2-Loader contributors
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.

  CUE (PS1-via-Ember) support. Ember (github.com/Gageformer/Ember) is a native PS1 emulator for the
  PS2. RiptOPL drives ember.elf directly and replaces Ember's own bundled launcher.elf, with the
  author's blessing.

  Every constant and rule here was MEASURED by disassembling the shipped Beta ember.elf (it ships
  unstripped, with DWARF), not read off the README -- the README is thinner and, on the accepted
  file extensions, wrong. The full derivation lives in docs/EMBER-INTEGRATION-PLAN.md Part 0.

  Two facts shape this whole file:

  1. ember.elf performs NO IOP reset. It has sceSifInitRpc/SifLoadModule/SifInitIopHeap and no
     SifIopReset, so it inherits the launcher's live driver stack. The handoff therefore uses
     sysLoadELFKeepIOP() (see sysLaunchEmber in system.c) and the caller must deinit with
     UNMOUNT_EXCEPTION. It is also why Ember can reach any device OPL can mount, unlike POPSTARTER,
     which resets and re-discovers from the memory card.

  2. Ember's game argument is a BARE FOLDER NAME. Its argv scan hard-refuses '/', ':' and '\\'
     anywhere in the argument and refuses a leading "..", then resolves "games/<name>" followed by
     "<name>", both relative to a working directory ps2sdk derives from argv[0]. A path can never
     be passed. That is why the games must live under the ember.elf directory and why there is
     deliberately no POPSTARTER-style device picker for Ember -- such a control could not work.

  POSIX directory IO only (opendir/readdir/closedir), like vcdsupport.c: the newlib port rejects
  direct fileXio use from these paths.
*/

#ifndef __CUESUPPORT_H
#define __CUESUPPORT_H

#include "include/iosupport.h"

// Ember joins argv[1..] with spaces into a 192-byte buffer, then snprintf()s "games/%s" into a
// second 192-byte buffer. A longer name cannot round-trip, so refuse it here with a message rather
// than hand the target something it will silently truncate.
#define CUE_NAME_MAX        192
#define CUE_NAME_LAUNCH_MAX 180 // 192 less "games/" and the NUL, with headroom

// Per-device folder holding ember.elf + bios.bin + games/. The NAME is the only knob Ember's
// argument contract leaves us (the folder must be the ember.elf's own directory), and it becomes a
// setting in a later phase -- read it through cueEmberFolder(), never the macro.
#define EMBER_FOLDER_DEFAULT "EMBER"
#define EMBER_ELF_NAME       "ember.elf"
#define EMBER_BIOS_NAME      "bios.bin"
#define EMBER_GAMES_FOLDER   "games"

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
// a name STARTING with ".." (main+0x4210 -- not only the exact string ".."). Pure string work, no
// device access.
int cueNameLaunchable(const char *name);

#endif
