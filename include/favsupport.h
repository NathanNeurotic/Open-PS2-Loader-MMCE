#ifndef __FAVSUPPORT_H
#define __FAVSUPPORT_H

#include "include/iosupport.h"

#define FAV_MODE_UPDATE_DELAY 240
#define FAV_TEXT_MAX          256        // cap on a stored favourite's text length (incl. NUL)
#define FAV_MAX_ITEMS         512        // cap on records read from favourites.bin
#define FAV_MAGIC             0x4641464F // 'OFAV' (little-endian on the EE)
#define FAV_VERSION           3          // v3 widens v2's isVcd byte to a KIND (see FAV_KIND_*)

// Defined in favsupport.c; consumed by opl.c/gui.c (config load/save/default).
extern int gFAVStartMode;

// Which shelf a favourite belongs on, and which core launches it. Stored in the byte OFAV v2 used
// for isVcd, so the on-disk record layout is unchanged and only its meaning widens.
//   ISO -- a PS2 disc game, launched by id through the source's itemLaunch
//   VCD -- a PS1 title launched by NAME through POPSTARTER (itemLaunchVcd)
//   CUE -- a PS1 title launched by NAME through Ember     (itemLaunchCue)
//   ELF -- a homebrew app, launched by id from the APPS tab
// VCD and CUE share the Favourites PS1 shelf; the kind picks the core, exactly as a row's extension
// does on a device page.
enum FAV_KIND {
    FAV_KIND_ISO = 0,
    FAV_KIND_VCD,
    FAV_KIND_CUE,
    FAV_KIND_ELF
};

// The LIB_VIEW_* shelf a kind is displayed on.
int favKindView(int kind);

item_list_t *favGetObject(int initOnly);

// Dynamic mode flags for the currently-selected FAV item (its source's flags). Used by
// itemExecTriangle so a FAV item exposes the same Options menu as its source would.
unsigned char favGetFlags(item_list_t *itemList);

// Source device mode of the FAV item at FAV-list index id, or -1 if out of range. Used by the
// theme engine to draw APP favourites with the apps element (proper art box + overlay).
int favGetItemSourceMode(int id);

// LIB_VIEW_ISO / PS1 / ELF kind of one displayed favorite, or -1 when id is invalid. The Favorites
// All-in-One shelf uses this to keep row-specific menus honest even though the page view is ALL.
int favGetItemView(int id);

// Source device mode of the favourite whose art-cache value (source startup / VCD name) matches,
// or -1 if none does. Used by texcache's cacheGetEffectiveMode so MMCE idle
// deferral, abort, and worker-priority rules follow the device a FAV-tab read
// actually lands on. GUI thread only -- favArray is rebuilt
// unlocked on the IO worker; see favGetArtMode's safety note in favsupport.c.
int favGetArtMode(const char *value);

// R3-toggle helpers (called from opl.c). add/remove rewrite favourites.bin and return 1 on a
// successful write, 0 on failure (so the caller won't set a lying star). add returns 1 if the
// item is already present. removeFavouriteByIdAndText matches mode (BDM-lenient) + id + text + isVcd.
// isVcd = 1 when the favourited item was a PS1/.VCD entry (device was in its L3 VCD view); such
// favourites resolve + launch as POPSTARTER rather than as a disc image. See favsupport.c.
int addFavouriteItem(int mode, int id, int icon_id, int text_id, const char *text, int kind);
int removeFavouriteByIdAndText(int mode, int id, const char *text, int kind);

// Remove the favourite at FAV-list index favIndex (R3 pressed on the Favourites tab).
void favRemoveByIndex(int favIndex);

// Cheap/idempotent: clears the FAV list + schedules its single deferred rebuild.
void loadFavourites(void);

#endif
