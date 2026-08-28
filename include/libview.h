/*
  Copyright 2026, Open-PS2-Loader contributors
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.

  Library view state: which of a page's several lists is on screen, and the L3 ring that moves
  between them.

  This replaces the binary per-mode VCD flag that used to live in vcdsupport.c. That flag answered
  one yes/no question -- "is this page showing its VCD list?" -- which stopped being expressible the
  moment a page could show a THIRD list (CUE, PS1 via Ember). A retained boolean helper would have
  answered "no" for a CUE page and routed CUE rows down the ISO path, silently. The whole point of
  this file is that every caller now names the view it means.

  A page's ring is the ordered set of views it supports. L3 advances one stop and wraps. The global
  gDefaultGameView setting can PIN every page that has a given stop to it, which makes L3 inert --
  that behaviour is unchanged, only generalised.

  The dirty protocol is unchanged too, because every device support already consumes it: advancing
  the view marks the mode dirty, the support's itemNeedsUpdate consumes the flag and forces one
  rescan, and the deferred IO update rebuilds the submenu.
*/

#ifndef __LIBVIEW_H
#define __LIBVIEW_H

#include "include/iosupport.h"

enum LIB_VIEW {
    LIB_VIEW_ISO = 0, // disc games: PS2 ISO / ZSO / UL / HDL
    LIB_VIEW_VCD,     // PS1 via POPSTARTER (*.VCD)
    LIB_VIEW_CUE,     // PS1 via Ember (a game folder holding *.cue / *.bin / *.exe)
    LIB_VIEW_ELF,     // homebrew ELFs
    LIB_VIEW_COUNT
};

// Is `view` a stop in this mode's ring? This is the ONE place a page's ring is defined; nothing
// else should hard-code which devices have which lists.
int libViewSupported(int mode, int view);

// How many stops this mode's ring has. 1 means there is nothing to toggle: L3 is inert and the
// on-screen hint is suppressed. Callers that used to ask "does this device have a VCD view" to
// decide whether to offer the toggle want THIS, not libViewSupported(mode, LIB_VIEW_VCD) -- the two
// stopped meaning the same thing once a page could have more than two lists.
int libViewRingSize(int mode);

// The view a page is currently showing. Always a stop in that page's ring; LIB_VIEW_ISO for a mode
// with no ring of its own.
int libViewActive(int mode);

// The view an item list is showing. A normal source delegates to libViewActive(mode); a Favourites
// shallow proxy may force a view through item_list_t.viewOverride without disturbing the real
// source page's own L3 state.
int libListViewActive(const item_list_t *itemList);

// L3: advance to the next supported stop, wrapping, and mark the mode dirty. No-op when the global
// default-view setting pins the page, or when the ring has fewer than two stops.
void libViewAdvance(int mode);

// Returns 1 exactly once after an advance (and clears the flag). Call from the support's
// itemNeedsUpdate so the forced rescan cannot be swallowed by per-generation device caching.
int libViewConsumeDirty(int mode);

// Mark one ringed mode dirty after its storage changes. Runtime callers still enqueue that
// support's normal deferred update; this only makes the existing NeedsUpdate path rescan it.
void libViewMarkDirty(int mode);

// Mark every ringed mode dirty (one rescan each) -- used when a global setting changes what the
// lists contain or how they sort. Deliberately side-effect-free: it also runs during early config
// loading, before the IO worker and device modules exist, so runtime callers must additionally
// enqueue the deferred updates themselves (oplQueueLibraryDeviceUpdates).
void libViewMarkAllDirty(void);

#endif
