/*
  Copyright 2026, Open-PS2-Loader contributors
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.

  Library display state and L3 routing.

  Ordinary game devices follow PS2/PS1 Game Display:
    Both  - separate PS2 and PS1 pages, toggled by L3.
    Mixed - one combined page; L3 cycles Mixed -> PS2 -> PS1.
    PS2   - PS2 only; L3 is completely inert.
    PS1   - PS1 only; L3 is completely inert.

  Favorites and APPS are deliberately independent. Favorites keeps its four-stop ring, cycling
  PS2 -> PS1 -> ELF -> All and opening on PS2. APPS is either one unfiltered list (no L3) or
  Apps/PS1ELF, where [PS1]
  in an ELF's displayed title chooses the PS1ELF side. A PS1 device row still chooses its actual
  POPSTARTER/Ember core from row metadata; the display layer never changes launch semantics.

  Every page's position is remembered across sessions (libViewLoadFromConfig / -StoreToConfig).
*/

#ifndef __LIBVIEW_H
#define __LIBVIEW_H

#include "include/iosupport.h"

enum LIB_VIEW {
    LIB_VIEW_ISO = 0, // PS2 disc games; also the normal-Apps side of a split APPS page
    LIB_VIEW_PS1,     // PS1 titles (POPSTARTER and Ember rows together)
    LIB_VIEW_ELF,     // ELF Favorites shelf
    LIB_VIEW_ALL,     // all Favorite kinds together
    LIB_VIEW_MIXED,   // PS2 + PS1 device rows, or every APPS row
    LIB_VIEW_PS1_ELF, // APPS rows whose displayed title contains [PS1]
    LIB_VIEW_COUNT
};

// Whether a view can exist for a mode. This describes potential backing lists, not necessarily the
// currently enabled L3 ring (libViewRingSize/libViewRingUsable describe that).
int libViewSupported(int mode, int view);

// Number of live L3 stops under the current setting. A value of one means L3 has no hint, sound,
// notification, pause, or state change.
int libViewRingSize(int mode);

// Page-level active view.
int libViewActive(int mode);

// A Favourites source proxy can force a homogeneous source list without changing that source
// page's retained state.
int libListViewActive(const item_list_t *itemList);

// Row-level view/source identity. Mixed pages override these through optional item-list callbacks;
// homogeneous legacy lists inherit their page view and visible id.
int libListRowView(item_list_t *itemList, int id);
int libListSourceId(item_list_t *itemList, int id);

// Whether L3 is actionable on this page under the current independent settings.
int libViewRingUsable(int mode);

// Stage one L3 stop without changing the view that owns the rows currently on screen. The IO
// rebuild commits that target only after it has cleared those rows, so a stale row id can never be
// resolved through the next view's backing array. Returns 1 when a transition was staged.
int libViewStageAdvance(int mode);
int libViewPending(int mode);
int libViewPendingTarget(int mode);
void libViewCommitPending(int mode);
void libViewFinishPending(int mode);

// Remembered L3 position, so every page comes back where the user left it. These only move the
// state in and out of the OPL settings set in memory -- toggling L3 costs nothing, and whoever
// writes that set carries the positions to disk. See the encoding note in libview.c.
void libViewLoadFromConfig(config_set_t *configOPL);
void libViewStoreToConfig(config_set_t *configOPL);

// Dirty protocol consumed by support itemNeedsUpdate callbacks.
int libViewConsumeDirty(int mode);
void libViewMarkDirty(int mode);
void libViewMarkAllDirty(void);

#endif
