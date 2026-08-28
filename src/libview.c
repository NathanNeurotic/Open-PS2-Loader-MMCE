/*
  Copyright 2026, Open-PS2-Loader contributors
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.

  Library view state + the L3 ring. See include/libview.h for the contract.

  Ported from the binary vcdView[]/vcdDirty[] pair that lived in vcdsupport.c. Behaviour is
  deliberately IDENTICAL to that flag: the same two stops on the same pages. What changed is that
  the second stop is now named for what it shows the user (PS1) rather than for one of the two
  cores that can launch a row in it.
*/

#include <string.h>

#include "include/opl.h"        // gDefaultGameView + GAME_VIEW_*
#include "include/bdmsupport.h" // bdmModeIsUDPBD -- the one BDM slot with no PS1 story
#include "include/libview.h"

static unsigned char libView[MODE_COUNT];  // current LIB_VIEW_* per mode (0 == LIB_VIEW_ISO)
static unsigned char libDirty[MODE_COUNT]; // 1 = view just changed -> force one rescan

int libViewSupported(int mode, int view)
{
    if (mode < 0 || mode >= MODE_COUNT || view < 0 || view >= LIB_VIEW_COUNT)
        return 0;

    switch (view) {
        case LIB_VIEW_ISO:
            // Every page's base list.
            //
            // APPS has one list and no ring, so it simply reports its base stop here and L3 stays
            // inert on it -- exactly what the old flag did, and appsupport never asks for the view
            // at all.
            return 1;

        case LIB_VIEW_PS1:
            // USB / exFAT, MMCE, MX4SIO, iLink, SMB, ATA (BDM HDD), APA / PFS HDD, and FAV_MODE.
            //
            // FAV_MODE has a ring of its own: the Favourites tab swaps between disc favourites and
            // PS1 favourites, and its slot is independent of any device's, so toggling Favourites
            // never disturbs a device page's view.
            //
            // UDPBD is excluded, and the reason is POPSTARTER-specific: it cannot bring a network
            // block device back up after its own IOP reset. Ember has no reset and keeps whatever
            // mount it is handed, so an Ember title on UDPBD should in principle work. The stop is
            // still withheld here because a PS1 page on UDPBD would list POPSTARTER titles that
            // cannot launch alongside Ember ones that can -- half a working list is worse than
            // none. Revisit as a per-ROW rule once the Ember launch is hardware-proven, not by
            // flipping this line.
            if (mode >= BDM_MODE && mode <= BDM_MODE_LAST)
                return !bdmModeIsUDPBD(mode);
            return mode == MMCE_MODE || mode == ETH_MODE || mode == HDD_MODE || mode == FAV_MODE;

        default:
            return 0;
    }
}

int libViewRingSize(int mode)
{
    int v, n = 0;

    for (v = 0; v < LIB_VIEW_COUNT; v++) {
        if (libViewSupported(mode, v))
            n++;
    }
    return n;
}

// The view every ringed page is pinned to by the global setting, or -1 when the setting leaves the
// L3 ring free (GAME_VIEW_BOTH). A page that does not have the pinned stop is NOT pinned -- it
// keeps showing its base list, which is what the old flag did for a non-VCD mode under the VCD
// lock, and is what keeps APPS and UDPFS on their normal lists.
static int libViewPinned(void)
{
    if (gDefaultGameView == GAME_VIEW_ISO)
        return LIB_VIEW_ISO;
    if (gDefaultGameView == GAME_VIEW_VCD)
        return LIB_VIEW_PS1;
    return -1;
}

int libViewActive(int mode)
{
    int pin;

    if (mode < 0 || mode >= MODE_COUNT)
        return LIB_VIEW_ISO;

    pin = libViewPinned();
    if (pin >= 0)
        return libViewSupported(mode, pin) ? pin : LIB_VIEW_ISO;

    return libView[mode];
}

int libListViewActive(const item_list_t *itemList)
{
    if (itemList == NULL)
        return LIB_VIEW_ISO;

    // A Favourites shallow proxy forces the view of the favourite it is resolving, so an ISO
    // favourite reads its source's retained ISO array even while that source page shows VCD.
    if (itemList->viewOverride == ITEM_VIEW_FORCE_ISO)
        return LIB_VIEW_ISO;
    if (itemList->viewOverride == ITEM_VIEW_FORCE_VCD)
        return LIB_VIEW_PS1;

    return libViewActive(itemList->mode);
}

void libViewAdvance(int mode)
{
    int cur, step, cand;

    if (mode < 0 || mode >= MODE_COUNT)
        return;
    if (libViewPinned() >= 0)
        return; // globally pinned to one list -> the L3 ring is inert
    if (libViewRingSize(mode) < 2)
        return; // nothing to move between

    cur = libView[mode];
    for (step = 1; step <= LIB_VIEW_COUNT; step++) {
        cand = (cur + step) % LIB_VIEW_COUNT;
        if (libViewSupported(mode, cand)) {
            libView[mode] = (unsigned char)cand;
            break;
        }
    }

    libDirty[mode] = 1;
}

int libViewConsumeDirty(int mode)
{
    if (mode < 0 || mode >= MODE_COUNT || !libDirty[mode])
        return 0;
    libDirty[mode] = 0;
    return 1;
}

void libViewMarkDirty(int mode)
{
    if (mode >= 0 && mode < MODE_COUNT && libViewRingSize(mode) > 1)
        libDirty[mode] = 1;
}

void libViewMarkAllDirty(void)
{
    int m;

    for (m = 0; m < MODE_COUNT; m++)
        libViewMarkDirty(m);
}
