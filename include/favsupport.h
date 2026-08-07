#ifndef __FAVSUPPORT_H
#define __FAVSUPPORT_H

// STUB (rebuild): the Favourites subsystem returns with checklist item 33. The theme
// engine already knows the FAV element families and the FAV_MODE page identity
// (include/iosupport.h). No FAV list can exist yet, so the theme engine's one real
// call is unreachable; the stub below only satisfies the compiler.
static inline int favGetItemSourceMode(int id)
{
    (void)id;
    return -1;
}

#endif
