/*
  RA: the badge in the game list that marks a checked, tracked game.

  After a check the console stores the watch list next to the game as
  `<device>RA/<serial>.wl`. That file's presence is the whole record of
  RA support; there is no separate registry to drift out of sync.

  We look in the same places and in the same order as the loader does
  before launch (`sbLoadWatchList`): the game's own device first, then
  the share. Otherwise the badge would lie, showing what the launch
  cannot find or hiding lists prepared on the PC.

  Recomputed whenever the game list is refreshed, which happens on the
  I/O thread where file operations are safe. It must not be called from
  the menu handler, for the same reason mounting must not.

  Upstream design and implementation: hacan359. Port note: upstream keys
  the slot table on RA_BADGE_SLOTS=4 ("OPL has exactly this many devices"),
  which is not true of this fork -- MODE_COUNT is 14 (eight BDM slots, ETH,
  HDD, APP, MMCE, FAV, UDPFS). With four slots the badge cache silently
  mis-attributes lists across devices.

  This file compiles to nothing unless RETROACHIEVEMENTS=1.
*/

#ifdef RETROACHIEVEMENTS

#include "include/opl.h"
#include "include/util.h"
#include "include/supportbase.h"
#include "include/rabadge.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define RA_BADGE      "RA " /* prefix before the name; ASCII only, the stock \
                               theme font has no other glyphs */
#define RA_BADGE_TEXT (ISO_GAME_NAME_MAX + 8)

struct ra_badge_slot
{
    item_list_t *support;
    int count;
    char *text; /* count strings of RA_BADGE_TEXT each, or NULL */
};

static struct ra_badge_slot g_slots[MODE_COUNT];

static struct ra_badge_slot *slotFor(item_list_t *support)
{
    int i;

    for (i = 0; i < MODE_COUNT; i++)
        if (g_slots[i].support == support)
            return &g_slots[i];

    for (i = 0; i < MODE_COUNT; i++) {
        if (g_slots[i].support == NULL) {
            g_slots[i].support = support;
            return &g_slots[i];
        }
    }

    return NULL;
}

static int watchListExists(const char *prefix, const char *serial)
{
    char path[256];
    struct stat st;

    if (serial == NULL || serial[0] == '\0')
        return 0;

    snprintf(path, sizeof(path), "%sRA/%s.wl", prefix, serial);
    if (stat(path, &st) == 0 && st.st_size > 0)
        return 1;

    /* The same fallback the loader uses: lists prepared on the PC live
       on the share. */
    if (strncmp(prefix, "smb0:", 5) != 0) {
        snprintf(path, sizeof(path), "smb0:RA/%s.wl", serial);
        if (stat(path, &st) == 0 && st.st_size > 0)
            return 1;
    }

    return 0;
}

void raBadgeRefresh(item_list_t *support, int count)
{
    struct ra_badge_slot *slot;
    const char *prefix;
    int i;

    if (support == NULL || support->itemGetName == NULL ||
        support->itemGetStartup == NULL || support->itemGetPrefix == NULL)
        return;

    slot = slotFor(support);
    if (slot == NULL)
        return;

    if (slot->text != NULL) {
        free(slot->text);
        slot->text = NULL;
    }
    slot->count = 0;

    if (count <= 0)
        return;

    prefix = support->itemGetPrefix(support);
    if (prefix == NULL)
        return;

    slot->text = malloc((size_t)count * RA_BADGE_TEXT);
    if (slot->text == NULL)
        return;

    slot->count = count;

    for (i = 0; i < count; i++) {
        char *dst = slot->text + (size_t)i * RA_BADGE_TEXT;
        const char *serial = support->itemGetStartup(support, i);

        if (watchListExists(prefix, serial))
            snprintf(dst, RA_BADGE_TEXT, "%s%s", RA_BADGE,
                     support->itemGetName(support, i));
        else
            dst[0] = '\0'; /* empty means show the plain name */
    }
}

int raBadgeHas(item_list_t *support, int idx)
{
    return raBadgeText(support, idx) != NULL;
}

const char *raBadgeText(item_list_t *support, int idx)
{
    struct ra_badge_slot *slot = NULL;
    char *text;
    int i;

    for (i = 0; i < MODE_COUNT; i++)
        if (g_slots[i].support == support)
            slot = &g_slots[i];

    if (slot == NULL || slot->text == NULL || idx < 0 || idx >= slot->count)
        return NULL;

    text = slot->text + (size_t)idx * RA_BADGE_TEXT;

    return text[0] ? text : NULL;
}

#endif /* RETROACHIEVEMENTS */
