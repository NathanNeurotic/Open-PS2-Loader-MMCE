/*
  Copyright 2026, Open-PS2-Loader contributors
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.
*/

#include "include/opl.h"
#include "include/libview.h"

// Both-mode device state, the Favorites ring, and the APPS split ring. Zero is the wanted default
// for ordinary devices/APPS (PS2/Apps); Favorites intentionally begins at All.
static unsigned char retainedView[MODE_COUNT] = {[FAV_MODE] = LIB_VIEW_ALL};

// Mixed has a different three-stop ring from Both. Keep its position separately so switching the
// setting to Mixed always enters the combined list the first time, while later visits retain the
// user's Mixed/PS2/PS1 position.
static unsigned char mixedView[MODE_COUNT];
static unsigned char mixedViewInitialized[MODE_COUNT];
static unsigned char libDirty[MODE_COUNT];
static unsigned char pendingView[MODE_COUNT];
static unsigned char pendingViewValid[MODE_COUNT];
static unsigned char pendingViewUsesMixedRing[MODE_COUNT];

static int libViewIsDeviceMode(int mode)
{
    if (mode >= BDM_MODE && mode <= BDM_MODE_LAST)
        return 1;
    return mode == MMCE_MODE || mode == ETH_MODE || mode == HDD_MODE || mode == UDPFS_MODE;
}

int libViewSupported(int mode, int view)
{
    if (mode < 0 || mode >= MODE_COUNT || view < 0 || view >= LIB_VIEW_COUNT)
        return 0;

    switch (view) {
        case LIB_VIEW_ISO:
            return 1;
        case LIB_VIEW_PS1:
            return libViewIsDeviceMode(mode) || mode == FAV_MODE;
        case LIB_VIEW_ELF:
        case LIB_VIEW_ALL:
            return mode == FAV_MODE;
        case LIB_VIEW_MIXED:
            return libViewIsDeviceMode(mode) || mode == APP_MODE;
        case LIB_VIEW_PS1_ELF:
            return mode == APP_MODE;
        default:
            return 0;
    }
}

int libViewRingSize(int mode)
{
    if (mode < 0 || mode >= MODE_COUNT)
        return 1;
    if (mode == FAV_MODE)
        return 4;
    if (mode == APP_MODE)
        return gAppsDisplay == APPS_DISPLAY_SPLIT ? 2 : 1;
    if (!libViewIsDeviceMode(mode))
        return 1;
    if (gDefaultGameView == GAME_VIEW_BOTH)
        return 2;
    if (gDefaultGameView == GAME_VIEW_MIXED)
        return 3;
    return 1;
}

static int libViewMixedActive(int mode)
{
    if (!mixedViewInitialized[mode]) {
        mixedView[mode] = LIB_VIEW_MIXED;
        mixedViewInitialized[mode] = 1;
    }
    return mixedView[mode];
}

int libViewActive(int mode)
{
    if (mode < 0 || mode >= MODE_COUNT)
        return LIB_VIEW_ISO;

    // These two pages do not inherit the ordinary-device setting.
    if (mode == FAV_MODE)
        return retainedView[mode];
    if (mode == APP_MODE)
        return gAppsDisplay == APPS_DISPLAY_SPLIT ? retainedView[mode] : LIB_VIEW_MIXED;

    if (!libViewIsDeviceMode(mode))
        return LIB_VIEW_ISO;

    switch (gDefaultGameView) {
        case GAME_VIEW_ISO:
            return LIB_VIEW_ISO;
        case GAME_VIEW_VCD:
            return LIB_VIEW_PS1;
        case GAME_VIEW_MIXED:
            return libViewMixedActive(mode);
        case GAME_VIEW_BOTH:
        default:
            return retainedView[mode] == LIB_VIEW_PS1 ? LIB_VIEW_PS1 : LIB_VIEW_ISO;
    }
}

int libListViewActive(const item_list_t *itemList)
{
    if (itemList == NULL)
        return LIB_VIEW_ISO;
    if (itemList->viewOverride == ITEM_VIEW_FORCE_ISO)
        return LIB_VIEW_ISO;
    if (itemList->viewOverride == ITEM_VIEW_FORCE_PS1)
        return LIB_VIEW_PS1;
    return libViewActive(itemList->mode);
}

int libListRowView(item_list_t *itemList, int id)
{
    int view;

    if (itemList == NULL)
        return LIB_VIEW_ISO;
    if (itemList->itemGetView != NULL) {
        view = itemList->itemGetView(itemList, id);
        if (view >= 0 && view < LIB_VIEW_COUNT)
            return view;
    }
    return libListViewActive(itemList);
}

int libListSourceId(item_list_t *itemList, int id)
{
    if (itemList != NULL && itemList->itemGetSourceId != NULL)
        return itemList->itemGetSourceId(itemList, id);
    return id;
}

int libViewRingUsable(int mode)
{
    return mode >= 0 && mode < MODE_COUNT && libViewRingSize(mode) > 1;
}

int libViewStageAdvance(int mode)
{
    int cur;

    if (!libViewRingUsable(mode) || pendingViewValid[mode])
        return 0;

    cur = libViewActive(mode);
    if (mode == FAV_MODE) {
        // Explicit order: All -> PS2 -> PS1 -> ELF -> All.
        pendingView[mode] = (cur == LIB_VIEW_ALL) ? LIB_VIEW_ISO :
                            (cur == LIB_VIEW_ISO) ? LIB_VIEW_PS1 :
                            (cur == LIB_VIEW_PS1) ? LIB_VIEW_ELF :
                                                    LIB_VIEW_ALL;
    } else if (mode == APP_MODE) {
        pendingView[mode] = cur == LIB_VIEW_PS1_ELF ? LIB_VIEW_ISO : LIB_VIEW_PS1_ELF;
    } else if (gDefaultGameView == GAME_VIEW_MIXED) {
        pendingView[mode] = (cur == LIB_VIEW_MIXED) ? LIB_VIEW_ISO :
                            (cur == LIB_VIEW_ISO)   ? LIB_VIEW_PS1 :
                                                      LIB_VIEW_MIXED;
    } else {
        pendingView[mode] = cur == LIB_VIEW_PS1 ? LIB_VIEW_ISO : LIB_VIEW_PS1;
    }

    pendingViewUsesMixedRing[mode] = mode != FAV_MODE && mode != APP_MODE && gDefaultGameView == GAME_VIEW_MIXED;
    pendingViewValid[mode] = 1;
    libDirty[mode] = 1;
    return 1;
}

int libViewPending(int mode)
{
    return mode >= 0 && mode < MODE_COUNT && pendingViewValid[mode];
}

int libViewPendingTarget(int mode)
{
    return libViewPending(mode) ? pendingView[mode] : libViewActive(mode);
}

void libViewCommitPending(int mode)
{
    if (!libViewPending(mode))
        return;

    if (pendingViewUsesMixedRing[mode])
        mixedView[mode] = pendingView[mode];
    else
        retainedView[mode] = pendingView[mode];
}

void libViewFinishPending(int mode)
{
    if (mode >= 0 && mode < MODE_COUNT)
        pendingViewValid[mode] = 0;
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
    if (mode >= 0 && mode < MODE_COUNT)
        libDirty[mode] = 1;
}

void libViewMarkAllDirty(void)
{
    int mode;
    for (mode = 0; mode < MODE_COUNT; mode++)
        libDirty[mode] = 1;
}
