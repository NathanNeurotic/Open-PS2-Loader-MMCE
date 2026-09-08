/*
  Copyright 2026, Open-PS2-Loader contributors
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.
*/

#include "include/opl.h"
#include "include/libview.h"

// Both-mode device state, the Favorites ring, and the APPS split ring. Zero -- LIB_VIEW_ISO, so
// PS2 for a device or Favorites and Apps for the APPS tab -- is the wanted FIRST-RUN default
// everywhere: Favorites' ring reads PS2 -> PS1 -> ELF -> All from there, so it opens on the plain
// PS2 shelf and the all-in-one one is a press away. A position remembered from an earlier session
// (libViewLoadFromConfig) overrides this, so it only decides where a fresh config starts.
static unsigned char retainedView[MODE_COUNT];

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
        // Explicit order: PS2 -> PS1 -> ELF -> All -> PS2. The final arm is the fallback for any
        // value outside the ring (a hand-edited remembered position), and lands on the default.
        pendingView[mode] = (cur == LIB_VIEW_ISO) ? LIB_VIEW_PS1 :
                            (cur == LIB_VIEW_PS1) ? LIB_VIEW_ELF :
                            (cur == LIB_VIEW_ELF) ? LIB_VIEW_ALL :
                                                    LIB_VIEW_ISO;
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

/*
  Remembering where the user left each page.

  Both rings are per-mode arrays, so the obvious encoding -- a config key per mode -- would be
  thirty keys for one idea the user thinks of as "where I left that page". Instead each ring is one
  fixed-width string, ONE CHARACTER PER MODE, indexed by the IO_MODES enum: '0'..'5' is a LIB_VIEW_*
  value, '-' means nothing is remembered for that mode and the compiled default stands.

  Nothing here writes to disk, and nothing runs while browsing: L3 only moves the values in RAM.
  The state is folded into the OPL settings set the next time that set is written (_saveConfig),
  which is the only cost this feature has. That also means the store runs on the deferred-IO thread
  while L3 runs on the GUI thread; both only touch single bytes, so the worst a race can do is
  persist one page's position a moment early or late.
*/
#define LIB_VIEW_STATE_LEN  (MODE_COUNT + 1)
#define LIB_VIEW_STATE_NONE '-'

// `set` (optional) marks which modes hold a real value; without it every mode is written.
static void libViewEncodeRing(char *out, const unsigned char *view, const unsigned char *set)
{
    int mode;

    for (mode = 0; mode < MODE_COUNT; mode++)
        out[mode] = (set == NULL || set[mode]) ? (char)('0' + view[mode]) : LIB_VIEW_STATE_NONE;
    out[MODE_COUNT] = '\0';
}

static void libViewDecodeRing(const char *in, unsigned char *view, unsigned char *set)
{
    int mode;

    // A short string -- an older config, or a hand-edited one -- simply leaves the modes it does
    // not reach at their defaults, so the format can grow with the mode enum without a migration.
    for (mode = 0; mode < MODE_COUNT && in[mode] != '\0'; mode++) {
        int value = in[mode] - '0';

        // Refuse a view this mode cannot display. A remembered position outlives the setting that
        // produced it (PS2/PS1 Game Display switched from Mixed to Both, Applications Display
        // switched back to one list), and restoring it blindly would strand a page on a view with
        // no backing list. An unusable value is dropped, not clamped: the default is always valid.
        if (value < 0 || value >= LIB_VIEW_COUNT || !libViewSupported(mode, value))
            continue;
        view[mode] = (unsigned char)value;
        if (set != NULL)
            set[mode] = 1;
    }
}

void libViewLoadFromConfig(config_set_t *configOPL)
{
    const char *value;

    if (configOPL == NULL)
        return;

    value = NULL;
    if (configGetStr(configOPL, CONFIG_OPL_LIB_VIEW_RETAINED, &value) && value != NULL)
        libViewDecodeRing(value, retainedView, NULL);

    // The Mixed ring carries its own "has been visited" flag: an unvisited page must enter on the
    // combined list the first time (libViewMixedActive), and restoring a position has to satisfy
    // that flag too or the restore would be overwritten on the first read.
    value = NULL;
    if (configGetStr(configOPL, CONFIG_OPL_LIB_VIEW_MIXED, &value) && value != NULL)
        libViewDecodeRing(value, mixedView, mixedViewInitialized);
}

void libViewStoreToConfig(config_set_t *configOPL)
{
    char encoded[LIB_VIEW_STATE_LEN];

    if (configOPL == NULL)
        return;

    libViewEncodeRing(encoded, retainedView, NULL);
    configSetStr(configOPL, CONFIG_OPL_LIB_VIEW_RETAINED, encoded);

    libViewEncodeRing(encoded, mixedView, mixedViewInitialized);
    configSetStr(configOPL, CONFIG_OPL_LIB_VIEW_MIXED, encoded);
}
