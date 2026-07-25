#ifndef __MENUSYS_H
#define __MENUSYS_H

#include "include/config.h"

struct UIItem;

/// a single submenu item
typedef struct submenu_item
{
    /// Icon used for rendering of this item
    int icon_id;

    /// item description
    char *text;

    /// item description in localized form (used if value is not negative)
    int text_id;

    /// item id (MUST BE VALID, we assert it is != -1 to optimize rendering)
    int id;

    int *cache_id;
    int *cache_uid;

    /// back-pointer to the producing item_list_t (Favourites proxies to it); NULL otherwise
    void *owner;
    /// 1 when the item is marked as a favourite (a star is drawn next to it)
    int favourited;
    /// 1 when this row is a browsable folder (GAME_FORMAT_FOLDER), not a launchable game. The
    /// dispatch descends into it instead of launching; the renderer marks it and skips its cover.
    int isFolder;
} submenu_item_t;

typedef struct submenu_list
{
    struct submenu_item item;

    struct submenu_list *prev, *next;
} submenu_list_t;

typedef struct menu_hint_item
{
    int icon_id;
    int text_id;

    struct menu_hint_item *next;
} menu_hint_item_t;

/// a single menu item. Linked list impl (for the ease of rendering)
typedef struct menu_item
{
    /// Icon used for rendering of this item
    int icon_id;

    /// item description
    char *text;

    /// item description in localised form (used if value is not negative)
    int text_id;

    // Indicates if the menu item should be drawn or not.
    int visible;

    void *userdata;

    /// submenu, selection and page start (only used in static mode)
    struct submenu_list *submenu, *current, *pagestart, *last;

    short remindLast;

    void (*refresh)(struct menu_item *curMenu);

    void (*execCross)(struct menu_item *curMenu);

    void (*execTriangle)(struct menu_item *curMenu);

    void (*execCircle)(struct menu_item *curMenu);

    void (*execSquare)(struct menu_item *curMenu);

    /// Favourites toggle (R3); NULL when the mode has no FAV action
    void (*fav)(struct menu_item *curMenu);

    /// VCD-view toggle (L3); NULL when the mode has no VCD view
    void (*toggleView)(struct menu_item *curMenu);

    /// hint list
    struct menu_hint_item *hints;
} menu_item_t;

typedef struct menu_list
{
    struct menu_item *item;

    struct menu_list *prev, *next;
} menu_list_t;

void menuInit();
void menuEnd();
void menuReinitMainMenu(void);
void menuInitGameMenu(void);
void menuInitAppMenu(void);

void menuAppendItem(menu_item_t *item);

void submenuRebuildCache(submenu_list_t *submenu);
submenu_list_t *submenuAppendItem(submenu_list_t **submenu, int icon_id, char *text, int id, int text_id, void *owner);
submenu_list_t *submenuFindItemByIdAndText(submenu_list_t *submenu, int id, const char *text);
void submenuRemoveItem(submenu_list_t **submenu, int id);
void submenuDestroy(submenu_list_t **submenu);
// `mode` = the owning device's list mode (menuItem.userdata->mode), so a VCD view with "hide game ID"
// on sorts by the RENDERED title instead of the raw filename's invisible game-ID prefix (#195).
// Pass -1 when no mode applies (non-device submenus).
void submenuSort(submenu_list_t **submenu, int mode);

char *submenuItemGetText(submenu_item_t *it);
char *menuItemGetText(menu_item_t *it);
config_set_t *menuLoadConfig();
config_set_t *menuLoadConfigDirect(void);
void menuRequestInfoSize(void);
config_set_t *gameMenuLoadConfig(struct UIItem *ui);
int menuSaveConfig();

void menuRenderMain();
void menuRenderMenu();
void menuRenderInfo();
void menuRenderGameMenu();
void menuRenderAppMenu();
void menuHandleInputMain();
void menuHandleInputMenu();
void menuHandleInputInfo();
void menuHandleInputGameMenu();
void menuHandleInputAppMenu();

// True once the menu list contains a registered device/mode tab (false while it is still empty).
int menuHasRegisteredItems(void);

// Sets the selected item if it is found in the menu list
void menuSetSelectedItem(menu_item_t *item);

void menuAddHint(menu_item_t *menu, int text_id, int icon_id);
void menuRemoveHints(menu_item_t *menu);

int menuSetParentalLockCheckState(int enabled);
int menuCheckParentalLock(void);

#endif
