/* RA: the "supported by RetroAchievements" badge in the game list.
   See src/rabadge.c */
#ifndef __RABADGE_H__
#define __RABADGE_H__

#include "include/iosupport.h"

/* Rebuilds the badges for one device's list. Call from the I/O thread
   when the game list is refreshed; file operations are safe there. */
void raBadgeRefresh(item_list_t *support, int count);

/* The game name with the badge when the game is checked and supported,
   otherwise NULL, meaning show the plain name. The pointer stays valid
   until the next raBadgeRefresh of the same device. */
const char *raBadgeText(item_list_t *support, int idx);

/* Whether the game is tracked. Used for the mark over the cover. */
int raBadgeHas(item_list_t *support, int idx);

#endif
