/*
  Copyright 2009, Ifcaro & volca
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.
*/

#include "include/opl.h"
#include "include/diag.h"
#include "include/menusys.h"
#include "include/iosupport.h"
#include "include/supportbase.h" // sbSetConfigStatSize -- defer the #Size stat off the scroll path
#include "include/favsupport.h"
#include "include/bdmsupport.h"
#include "include/vcdsupport.h" // vcdViewActive -- VCD view renders with the apps element family
#include "include/folderbrowse.h"
#include "include/renderman.h"
#include "include/fntsys.h"
#include "include/lang.h"
#include "include/themes.h"
#include "include/pad.h"
#include "include/gui.h"
#include "include/guigame.h"
#include "include/system.h"
#include "include/ioman.h"
#include "include/sound.h"
#include "include/texcache.h"
#include <assert.h>

enum MENU_IDs {
    MENU_SETTINGS = 0,
    MENU_GAME_SOURCES,
    MENU_INTERFACE,
    MENU_DISPLAY,
    MENU_GAME_LAUNCHING,
    MENU_POPSTARTER,
    MENU_MMCE,
    MENU_NETWORK,
    MENU_CONTROLLER,
    MENU_AUDIO,
    MENU_SECURITY,
    MENU_ADVANCED,
    MENU_TOOLS,
    MENU_ABOUT,
    MENU_SAVE_CHANGES,
    MENU_EXIT,
    MENU_POWER_OFF,
    MENU_LAUNCH_PS2_DISC
};

enum GAME_MENU_IDs {
    GAME_COMPAT_SETTINGS = 0,
    GAME_CHEAT_SETTINGS,
    GAME_GSM_SETTINGS,
    GAME_VMC_SETTINGS,
#ifdef PADEMU
    GAME_PADEMU_SETTINGS,
    GAME_PADMACRO_SETTINGS,
#endif
    GAME_OSD_LANGUAGE_SETTINGS,
    GAME_SAVE_CHANGES,
    GAME_TEST_CHANGES,
    GAME_REMOVE_CHANGES,
    GAME_RENAME_GAME,
    GAME_DELETE_GAME,
};

// global menu variables
static menu_list_t *menu;
static menu_list_t *selected_item;

static int actionStatus;
static int itemConfigId;
static config_set_t *itemConfig;

static u8 parentalLockCheckEnabled = 1;

// "main menu submenu"
static submenu_list_t *mainMenu;
// active item in the main menu
static submenu_list_t *mainMenuCurrent;

// "game settings submenu"
static submenu_list_t *gameMenu;
// active item in game settings
static submenu_list_t *gameMenuCurrent;

static submenu_list_t *appMenu;
static submenu_list_t *appMenuCurrent;

static s32 menuSemaId = -1;
static s32 menuListSemaId = -1;
static ee_sema_t menuSema;

#define MENU_MMCE_CONFIG_IDLE_FRAMES 20
#define MENU_APP_CONFIG_IDLE_FRAMES  1

static int menuCanRequestItemConfig(item_list_t *list)
{
    if (list != NULL && list->mode == APP_MODE)
        return guiInactiveFrames >= MENU_APP_CONFIG_IDLE_FRAMES;

    // Keep automatic MMCE metadata off the active-scroll path.
    if (list != NULL && list->mode == MMCE_MODE)
        return guiInactiveFrames >= MENU_MMCE_CONFIG_IDLE_FRAMES;

    return 1;
}

static void menuRenameGame(submenu_list_t **submenu)
{
    if (!selected_item->item->current) {
        return;
    }

    if (!gEnableWrite)
        return;

    item_list_t *support = selected_item->item->userdata;

    // Favourites: rename is blocked from the FAV tab and on any favourited source item.
    if (support && support->mode == FAV_MODE) {
        char text[128];
        snprintf(text, sizeof(text), _l(_STR_FAV_MSG), _l(_STR_RENAME));
        guiMsgBox(text, 0, NULL);
        return;
    }
    if (selected_item->item->current->item.favourited) {
        char text[128];
        snprintf(text, sizeof(text), _l(_STR_FAV_PERSISTENCE_MSG), _l(_STR_RENAME));
        guiMsgBox(text, 0, NULL);
        return;
    }

    if (support) {
        if (support->itemRename) {
            if (menuCheckParentalLock() == 0) {
                sfxPlay(SFX_MESSAGE);
                int nameLength = support->itemGetNameLength(support, selected_item->item->current->item.id);
                char newName[nameLength];
                strncpy(newName, selected_item->item->current->item.text, nameLength);
                if (guiShowKeyboard(newName, nameLength)) {
                    guiSwitchScreen(GUI_SCREEN_MAIN);
                    submenuDestroy(submenu);

                    // Only rename the file if the name changed; trying to rename a file with a file name that hasn't changed can cause the file
                    // to be deleted on certain file systems.
                    if (strcmp(newName, selected_item->item->current->item.text) != 0) {
                        support->itemRename(support, selected_item->item->current->item.id, newName);
                        ioPutRequest(IO_MENU_UPDATE_DEFFERED, &support->mode);
                    }
                }
            }
        }
    } else
        guiMsgBox("NULL Support object. Please report", 0, NULL);
}

static void menuDeleteGame(submenu_list_t **submenu)
{
    if (!selected_item->item->current)
        return;

    if (!gEnableWrite)
        return;

    item_list_t *support = selected_item->item->userdata;

    // Favourites: delete is blocked from the FAV tab and on any favourited source item.
    if (support && support->mode == FAV_MODE) {
        char text[128];
        snprintf(text, sizeof(text), _l(_STR_FAV_MSG), _l(_STR_DELETE));
        guiMsgBox(text, 0, NULL);
        return;
    }
    if (selected_item->item->current->item.favourited) {
        char text[128];
        snprintf(text, sizeof(text), _l(_STR_FAV_PERSISTENCE_MSG), _l(_STR_DELETE));
        guiMsgBox(text, 0, NULL);
        return;
    }

    if (support) {
        if (support->itemDelete) {
            if (menuCheckParentalLock() == 0) {
                if (guiMsgBox(_l(_STR_DELETE_WARNING), 1, NULL)) {
                    guiSwitchScreen(GUI_SCREEN_MAIN);
                    submenuDestroy(submenu);
                    support->itemDelete(support, selected_item->item->current->item.id);
                    ioPutRequest(IO_MENU_UPDATE_DEFFERED, &support->mode);
                }
            }
        }
    } else
        guiMsgBox("NULL Support object. Please report", 0, NULL);
}

static void _menuLoadConfig()
{
    int blockingLoad = 0;
    item_list_t *list = NULL;
    config_set_t *loadedConfig = NULL;
    int configId = -1;

    WaitSema(menuSemaId);
    blockingLoad = actionStatus != 0;
    if (!itemConfig && selected_item != NULL && selected_item->item->current != NULL && itemConfigId >= 0) {
        list = selected_item->item->userdata;
        configId = itemConfigId;
    }
    SignalSema(menuSemaId);

    // NAV-PARITY TEST BUILD (#340): no art-queue drain before a per-item config read.

    if (list != NULL)
        loadedConfig = list->itemGetConfig(list, configId);

    WaitSema(menuSemaId);
    if (!itemConfig && loadedConfig != NULL && itemConfigId == configId)
        itemConfig = loadedConfig;
    else if (loadedConfig != NULL)
        configFree(loadedConfig);
    actionStatus = 0;
    SignalSema(menuSemaId);
}

// Opening the info screen needs #Size, which the scroll-time config load deliberately skips
// (sbConfigStatSize off -> no slow per-game stat while browsing). Rebuild the current item's
// config once with the size resolved and swap it in, but only if the selection is unchanged --
// the user may have scrolled away before this IO request ran. game->sizeMB is cached afterwards,
// so subsequent scrolls/info views show the size with no further stat.
static void _menuResolveInfoSize()
{
    item_list_t *list = NULL;
    config_set_t *loadedConfig = NULL;
    int configId = -1;

    WaitSema(menuSemaId);
    if (selected_item != NULL && selected_item->item != NULL && selected_item->item->current != NULL && itemConfigId >= 0 && itemConfigId == selected_item->item->current->item.id) {
        list = selected_item->item->userdata;
        configId = itemConfigId;
    }
    SignalSema(menuSemaId);

    if (list == NULL || configId < 0)
        return;

    sbSetConfigStatSize(1);
    loadedConfig = list->itemGetConfig(list, configId);
    sbSetConfigStatSize(0);

    if (loadedConfig == NULL)
        return;

    WaitSema(menuSemaId);
    if (selected_item != NULL && selected_item->item != NULL && selected_item->item->current != NULL && itemConfigId == configId && itemConfigId == selected_item->item->current->item.id) {
        if (itemConfig != NULL)
            configFree(itemConfig);
        itemConfig = loadedConfig;
        loadedConfig = NULL;
    }
    SignalSema(menuSemaId);

    if (loadedConfig != NULL)
        configFree(loadedConfig);
}

static int menuSaveResult = 0; // carries configWrite's success out of the deferred _menuSaveConfig callback

static void _menuSaveConfig()
{
    int result;

    WaitSema(menuSemaId);
    result = configWrite(itemConfig);
    itemConfigId = -1;       // to invalidate cache and force reload
    menuSaveResult = result; // publish BEFORE actionStatus=0 -- that flag is the waiter's release signal
    actionStatus = 0;
    SignalSema(menuSemaId);

    if (!result)
        // #245 (AndrewBento, MMCE): name WHERE and the errno instead of a bare "Error writing
        // settings!". itemConfig->filename is the per-game write target (e.g. mmceN:/CFG/<id>.cfg) and
        // gLastSaveErrno was latched inside configWrite at the real failure site -- together they
        // distinguish a genuine mmceman write/close failure (nonzero errno: EIO/ENOSPC/ENOENT/EROFS)
        // from our strict close check tripping on a write that actually landed (errno 0). Matches the
        // diagnostic the global saveConfig failure already prints.
        setErrorMessagePathCode(_STR_ERROR_SAVING_SETTINGS_TO, itemConfig ? itemConfig->filename : "", gLastSaveErrno);
}

static void _menuRequestConfig()
{
    int shouldQueueLoad = 0;
    int visibleLoad = 0;

    WaitSema(menuSemaId);
    if (selected_item == NULL || selected_item->item == NULL || selected_item->item->current == NULL) {
        actionStatus = 0;
    } else if (itemConfigId != selected_item->item->current->item.id) {
        if (itemConfig) {
            configFree(itemConfig);
            itemConfig = NULL;
        }
        item_list_t *list = selected_item->item->userdata;
        // NAV-PARITY TEST BUILD (#340): official gates the per-item CFG load on list->delay alone
        // (no APP_MODE 1-frame fast path) and has no "retry a NULL load" re-queue branch.
        int configIdleFrames = list->delay;

        if (itemConfigId == -1 || guiInactiveFrames >= configIdleFrames) {
            itemConfigId = selected_item->item->current->item.id;
            shouldQueueLoad = 1;
            visibleLoad = actionStatus != 0;
        }
    } else
        actionStatus = 0;

    SignalSema(menuSemaId);

    int queueResult = IO_OK;
    if (shouldQueueLoad) {
        queueResult = ioPutRequest(IO_CUSTOM_SIMPLEACTION, &_menuLoadConfig);
    }

    if (queueResult != IO_OK) {
        WaitSema(menuSemaId);
        if (itemConfig == NULL)
            itemConfigId = -1;
        actionStatus = 0;
        SignalSema(menuSemaId);
    }
}

static config_set_t *menuLoadConfigDirectInternal(void)
{
    config_set_t *loadedConfig = NULL;
    config_set_t *result = NULL;
    item_list_t *list = NULL;
    int configId = -1;

    WaitSema(menuSemaId);
    if (selected_item != NULL && selected_item->item != NULL && selected_item->item->current != NULL) {
        list = selected_item->item->userdata;
        configId = selected_item->item->current->item.id;

        if (itemConfigId == configId && itemConfig != NULL) {
            result = itemConfig;
        } else {
            if (itemConfig != NULL) {
                configFree(itemConfig);
                itemConfig = NULL;
            }
            itemConfigId = configId;
        }
    } else {
        if (itemConfig != NULL) {
            configFree(itemConfig);
            itemConfig = NULL;
        }
        itemConfigId = -1;
    }
    SignalSema(menuSemaId);

    if (result != NULL || list == NULL || configId < 0)
        return result;

    if (list->mode == MMCE_MODE) {
        // Request an abort without delaying the explicit config read.
        cacheAbortMmceImageLoadsTimed(0);
    } else
        (void)cacheCancelPendingImageLoadsTimed(MENU_MIN_INACTIVE_FRAMES);
    loadedConfig = list->itemGetConfig(list, configId);

    WaitSema(menuSemaId);
    if (itemConfigId == configId && itemConfig == NULL)
        itemConfig = loadedConfig;
    else if (loadedConfig != NULL)
        configFree(loadedConfig);
    result = itemConfig;
    SignalSema(menuSemaId);

    return result;
}

config_set_t *menuLoadConfig()
{
    actionStatus = 1;
    itemConfigId = -1;
    guiHandleDeferedIO(&actionStatus, _l(_STR_LOADING_SETTINGS), IO_CUSTOM_SIMPLEACTION, &_menuRequestConfig, OPL_DEFERRED_IO_TIMEOUT_MS);
    return itemConfig;
}

config_set_t *menuLoadConfigDirect(void)
{
    actionStatus = 0;
    return menuLoadConfigDirectInternal();
}

// Queued when the info screen opens: resolve #Size for the current item without blocking the UI.
void menuRequestInfoSize(void)
{
    ioPutRequest(IO_CUSTOM_SIMPLEACTION, &_menuResolveInfoSize);
}

// we don't want a pop up when transitioning to or refreshing Game Menu gui.
config_set_t *gameMenuLoadConfig(struct UIItem *ui)
{
    actionStatus = 1;
    itemConfigId = -1;
    guiGameHandleDeferedIO(&actionStatus, ui, IO_CUSTOM_SIMPLEACTION, &_menuRequestConfig);
    return itemConfig;
}

int menuSaveConfig()
{
    actionStatus = 1;
    menuSaveResult = 0;
    guiHandleDeferedIO(&actionStatus, _l(_STR_SAVING_SETTINGS), IO_CUSTOM_SIMPLEACTION, &_menuSaveConfig, OPL_DEFERRED_IO_TIMEOUT_MS);
    return menuSaveResult;
}

static void menuInitMainMenu(void)
{
    if (mainMenu)
        submenuDestroy(&mainMenu);

    // initialize the menu
    submenuAppendItem(&mainMenu, -1, NULL, MENU_LAUNCH_PS2_DISC, _STR_LAUNCH_PS2_DISC, NULL);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_SETTINGS, _STR_SETTINGS, NULL);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_GAME_SOURCES, _STR_GAME_SOURCES, NULL);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_INTERFACE, _STR_INTERFACE_SETTINGS, NULL);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_DISPLAY, _STR_DISPLAY_SETTINGS, NULL);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_GAME_LAUNCHING, _STR_GAME_LAUNCHING, NULL);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_POPSTARTER, _STR_POPSTARTER, NULL);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_MMCE, _STR_MMCE, NULL);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_NETWORK, _STR_MENU_NETWORK, NULL);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_CONTROLLER, _STR_MENU_CONTROLLER, NULL);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_AUDIO, _STR_MENU_AUDIO, NULL);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_SECURITY, _STR_SECURITY_SETTINGS, NULL);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_ADVANCED, _STR_ADVANCED_SETTINGS, NULL);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_TOOLS, _STR_TOOLS, NULL);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_ABOUT, _STR_ABOUT, NULL);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_SAVE_CHANGES, _STR_SAVE_CHANGES, NULL);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_EXIT, _STR_EXIT, NULL);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_POWER_OFF, _STR_POWEROFF, NULL);

    mainMenuCurrent = mainMenu;
}

void menuReinitMainMenu(void)
{
    menuInitMainMenu();
}

void menuInitGameMenu(void)
{
    if (gameMenu)
        submenuDestroy(&gameMenu);

    // initialize the menu
    submenuAppendItem(&gameMenu, -1, NULL, GAME_COMPAT_SETTINGS, _STR_COMPAT_SETTINGS, NULL);
    submenuAppendItem(&gameMenu, -1, NULL, GAME_CHEAT_SETTINGS, _STR_CHEAT_SETTINGS, NULL);
    submenuAppendItem(&gameMenu, -1, NULL, GAME_GSM_SETTINGS, _STR_GSCONFIG, NULL);
    submenuAppendItem(&gameMenu, -1, NULL, GAME_VMC_SETTINGS, _STR_VMC_SCREEN, NULL);
#ifdef PADEMU
    submenuAppendItem(&gameMenu, -1, NULL, GAME_PADEMU_SETTINGS, _STR_CONTROLLER_EMULATION, NULL);
    submenuAppendItem(&gameMenu, -1, NULL, GAME_PADMACRO_SETTINGS, _STR_CONTROLLER_MACROS, NULL);
#endif
    submenuAppendItem(&gameMenu, -1, NULL, GAME_OSD_LANGUAGE_SETTINGS, _STR_OSD_SETTINGS, NULL);
    submenuAppendItem(&gameMenu, -1, NULL, GAME_SAVE_CHANGES, _STR_SAVE_CHANGES, NULL);
    submenuAppendItem(&gameMenu, -1, NULL, GAME_TEST_CHANGES, _STR_TEST, NULL);
    submenuAppendItem(&gameMenu, -1, NULL, GAME_REMOVE_CHANGES, _STR_REMOVE_ALL_SETTINGS, NULL);
    if (gEnableWrite) {
        submenuAppendItem(&gameMenu, -1, NULL, GAME_RENAME_GAME, _STR_RENAME, NULL);
        submenuAppendItem(&gameMenu, -1, NULL, GAME_DELETE_GAME, _STR_DELETE, NULL);
    }

    gameMenuCurrent = gameMenu;
}

void menuInitAppMenu(void)
{
    if (appMenu)
        submenuDestroy(&appMenu);

    // initialize the menu
    submenuAppendItem(&appMenu, -1, NULL, 0, _STR_RENAME, NULL);
    submenuAppendItem(&appMenu, -1, NULL, 1, _STR_DELETE, NULL);

    appMenuCurrent = appMenu;
}

// -------------------------------------------------------------------------------------------
// ---------------------------------------- Menu manipulation --------------------------------
// -------------------------------------------------------------------------------------------
void menuInit()
{
    menu = NULL;
    selected_item = NULL;
    itemConfigId = -1;
    itemConfig = NULL;
    mainMenu = NULL;
    mainMenuCurrent = NULL;
    gameMenu = NULL;
    gameMenuCurrent = NULL;
    appMenu = NULL;
    appMenuCurrent = NULL;
    menuInitMainMenu();

    // Create once; recreating on a second menuInit would leak the prior semaphore.
    if (menuSemaId < 0) {
        menuSema.init_count = 1;
        menuSema.max_count = 1;
        menuSema.option = 0;
        menuSemaId = CreateSema(&menuSema);
    }
    if (menuListSemaId < 0) {
        menuListSemaId = sbCreateSemaphore();
    }
}

void menuEnd()
{
    // destroy menu
    menu_list_t *cur = menu;

    while (cur) {
        menu_list_t *td = cur;
        cur = cur->next;

        if (td->item)
            submenuDestroy(&(td->item->submenu));

        menuRemoveHints(td->item);

        free(td);
    }

    submenuDestroy(&mainMenu);
    submenuDestroy(&gameMenu);
    submenuDestroy(&appMenu);

    if (itemConfig) {
        configFree(itemConfig);
        itemConfig = NULL;
    }

    DeleteSema(menuSemaId);
    menuSemaId = -1;
    DeleteSema(menuListSemaId);
    menuListSemaId = -1;
}

static menu_list_t *AllocMenuItem(menu_item_t *item)
{
    menu_list_t *it;

    it = malloc(sizeof(menu_list_t));

    it->prev = NULL;
    it->next = NULL;
    it->item = item;

    return it;
}

void menuAppendItem(menu_item_t *item)
{
    assert(item);

    WaitSema(menuListSemaId);

    if (menu == NULL) {
        menu = AllocMenuItem(item);
        selected_item = menu;
    } else {
        menu_list_t *cur = menu;

        // traverse till the end
        while (cur->next)
            cur = cur->next;

        // create new item
        menu_list_t *newitem = AllocMenuItem(item);

        // link
        cur->next = newitem;
        newitem->prev = cur;
    }

    SignalSema(menuListSemaId);
}

static void refreshMenuPosition(void)
{
    if (menu == NULL)
        return;

    // Nad #6: this runs on EVERY start-menu exit -- if the current tab is still visible there is
    // nothing to refresh, so KEEP it. The old unconditional reseat below positionally dumped the
    // selection onto the first visible tab (BDM devices register first, so typically MX4SIO) on
    // every settings/start-menu round trip, overriding the user's Default Menu landing.
    if (selected_item != NULL && selected_item->item != NULL && selected_item->item->visible)
        return;

    // Find the first menu in the list that is visible and set it as the active menu.
    menu_list_t *cur = menu;
    while (cur->item->visible == 0 && cur->next)
        cur = cur->next;

    if (cur->item->visible == 0) {
        // No visible menu was found, just set the current menu to the first one in the list.
        selected_item = menu;
    } else {
        selected_item = cur;
    }
}

void submenuRebuildCache(submenu_list_t *submenu)
{
    while (submenu) {
        if (submenu->item.cache_id)
            free(submenu->item.cache_id);
        if (submenu->item.cache_uid)
            free(submenu->item.cache_uid);

        int size = gTheme->gameCacheCount * sizeof(int);
        submenu->item.cache_id = malloc(size);
        memset(submenu->item.cache_id, -1, size);
        submenu->item.cache_uid = malloc(size);
        memset(submenu->item.cache_uid, -1, size);

        submenu = submenu->next;
    }
}

static submenu_list_t *submenuAllocItem(int icon_id, char *text, int id, int text_id, void *owner)
{
    submenu_list_t *it = (submenu_list_t *)malloc(sizeof(submenu_list_t));

    it->prev = NULL;
    it->next = NULL;
    it->item.icon_id = icon_id;
    it->item.text = text;
    it->item.text_id = text_id;
    it->item.id = id;
    it->item.cache_id = NULL;
    it->item.cache_uid = NULL;
    it->item.owner = owner;
    it->item.favourited = 0;
    it->item.isFolder = 0;
    submenuRebuildCache(it);

    return it;
}

submenu_list_t *submenuAppendItem(submenu_list_t **submenu, int icon_id, char *text, int id, int text_id, void *owner)
{
    if (*submenu == NULL) {
        *submenu = submenuAllocItem(icon_id, text, id, text_id, owner);
        return *submenu;
    }

    submenu_list_t *cur = *submenu;

    // traverse till the end
    while (cur->next)
        cur = cur->next;

    // create new item
    submenu_list_t *newitem = submenuAllocItem(icon_id, text, id, text_id, owner);

    // link
    cur->next = newitem;
    newitem->prev = cur;

    return newitem;
}

// Linear search by id + text. Favourites uses this to locate the source-list item so the
// favourited flag (star) stays in sync on both the FAV copy and the source copy. Both text
// args are NULL-guarded before strcmp.
submenu_list_t *submenuFindItemByIdAndText(submenu_list_t *submenu, int id, const char *text)
{
    submenu_list_t *cur = submenu;
    while (cur) {
        if (cur->item.id == id && text && cur->item.text && !strcmp(cur->item.text, text))
            return cur;
        cur = cur->next;
    }
    return NULL;
}

static void submenuDestroyItem(submenu_list_t *submenu)
{
    free(submenu->item.cache_id);
    free(submenu->item.cache_uid);

    free(submenu);
}

void submenuRemoveItem(submenu_list_t **submenu, int id)
{
    submenu_list_t *cur = *submenu;
    submenu_list_t *prev = NULL;

    while (cur) {
        if (cur->item.id == id) {
            submenu_list_t *next = cur->next;

            if (prev)
                prev->next = cur->next;

            if (*submenu == cur)
                *submenu = next;

            submenuDestroyItem(cur);

            cur = next;
        } else {
            prev = cur;
            cur = cur->next;
        }
    }
}

void submenuDestroy(submenu_list_t **submenu)
{
    // destroy sub menu
    submenu_list_t *cur = *submenu;

    while (cur) {
        submenu_list_t *td = cur;
        cur = cur->next;

        submenuDestroyItem(td);
    }

    *submenu = NULL;
}

void menuAddHint(menu_item_t *menu, int text_id, int icon_id)
{
    // allocate a new hint item
    menu_hint_item_t *hint = malloc(sizeof(menu_hint_item_t));

    hint->text_id = text_id;
    hint->icon_id = icon_id;
    hint->next = NULL;

    if (menu->hints) {
        menu_hint_item_t *top = menu->hints;

        // rewind to end
        for (; top->next; top = top->next)
            ;

        top->next = hint;
    } else {
        menu->hints = hint;
    }
}

void menuRemoveHints(menu_item_t *menu)
{
    while (menu->hints) {
        menu_hint_item_t *hint = menu->hints;
        menu->hints = hint->next;
        free(hint);
    }
}

char *menuItemGetText(menu_item_t *it)
{
    if (it->text_id >= 0)
        return _l(it->text_id);
    else
        return it->text;
}

char *submenuItemGetText(submenu_item_t *it)
{
    if (it->text_id >= 0)
        return _l(it->text_id);
    else
        return it->text;
}

static void swap(submenu_list_t *a, submenu_list_t *b)
{
    submenu_list_t *pa, *nb;
    pa = a->prev;
    nb = b->next;

    a->next = nb;
    b->prev = pa;
    b->next = a;
    a->prev = b;

    if (pa)
        pa->next = b;

    if (nb)
        nb->prev = a;
}

// Sorts the given submenu by comparing the ON-SCREEN titles. `mode` is the owning device's list mode
// (menuItem.userdata->mode) so a VCD view with "hide game ID" on sorts by what is actually RENDERED:
// vcdDisplayName() skips the leading "SLES_123.45." prefix at draw time, so comparing the raw stored
// filenames here ordered the list by an invisible publisher code -- alphabetical to nobody (#195). It
// returns `text` unchanged for every non-VCD list (and when the prefix is shown), so this is a no-op
// for PS2/apps/folders. Pass -1 when no mode applies. MUST stay in the same collation the VCD scan
// sort uses (vcdEntryCmp, strcasecmp on the same display-adjusted key) or the two disagree and this
// menu-level pass -- which runs LAST -- silently undoes the backing array's order.
void submenuSort(submenu_list_t **submenu, int mode)
{
    // a simple bubblesort
    // *submenu = mergeSort(*submenu);
    submenu_list_t *head;
    int sorted = 0;

    if ((submenu == NULL) || (*submenu == NULL) || ((*submenu)->next == NULL))
        return;

    head = *submenu;

    while (!sorted) {
        sorted = 1;

        submenu_list_t *tip = head;

        while (tip->next) {
            submenu_list_t *nxt = tip->next;

            // Compare the RENDERED text, not the stored name (see the header comment): a VCD view with
            // "hide game ID" on draws each row past its game-ID prefix, so the raw filename is the wrong
            // sort key. vcdDisplayName is a pure pointer-bump (no copy) and returns the input unchanged
            // for every other list.
            const char *txt1 = vcdDisplayName(mode, submenuItemGetText(&tip->item));
            const char *txt2 = vcdDisplayName(mode, submenuItemGetText(&nxt->item));

            // Folder browsing: folders group ahead of games; within each group sort by title.
            int cmp;
            if (tip->item.isFolder != nxt->item.isFolder)
                cmp = tip->item.isFolder ? -1 : 1;
            else
                cmp = strcasecmp(txt1, txt2);

            if (cmp > 0) {
                swap(tip, nxt);

                if (tip == head)
                    head = nxt;

                sorted = 0;
            } else {
                tip = tip->next;
            }
        }
    }

    *submenu = head;
}

// Folder browsing: return the device page we are leaving to its folder root, so a device is never
// parked inside a subfolder while off-screen. This keeps folder navigation a per-visit affair and
// guarantees the Favourites tab / last-played always resolve against a device's root list. It also
// frees the single shared breadcrumb buffer for the next device by restoring the device-name title.
static void menuFolderResetLeaving(struct menu_list *leaving)
{
    if (leaving == NULL || leaving->item == NULL || leaving->item->userdata == NULL)
        return;
    item_list_t *support = (item_list_t *)leaving->item->userdata;
    if (!folderModeSupported(support->mode) || folderDepth(support->mode) == 0)
        return;
    folderReset(support->mode);
    leaving->item->text = NULL;
    leaving->item->text_id = support->itemTextId(support); // restore the device name (was the breadcrumb)
    // Queue the rebuild now (folderReset marked the mode dirty) so the device is back at its root list
    // promptly -- the Favourites tab / last-played resolve against that root, not the subfolder we left.
    ioPutRequest(IO_MENU_UPDATE_DEFFERED, &support->mode);
}

static void menuNextH()
{
    struct menu_list *next = selected_item->next;
    while (next != NULL && next->item->visible == 0)
        next = next->next;

    // If we found a valid menu transition to it.
    if (next != NULL) {
        menuFolderResetLeaving(selected_item);
        selected_item = next;
        itemConfigId = -1;
        sfxPlay(SFX_CURSOR);
    }
}

static void menuPrevH()
{
    struct menu_list *prev = selected_item->prev;
    while (prev != NULL && prev->item->visible == 0)
        prev = prev->prev;

    if (prev != NULL) {
        menuFolderResetLeaving(selected_item);
        selected_item = prev;
        itemConfigId = -1;
        sfxPlay(SFX_CURSOR);
    }
}

static void menuFirstPage()
{
    submenu_list_t *cur = selected_item->item->current;
    if (cur && cur != selected_item->item->submenu) {
        if (cur->prev) {
            sfxPlay(SFX_CURSOR);
        }

        selected_item->item->current = selected_item->item->submenu;
        selected_item->item->pagestart = selected_item->item->current;
    }
}

static void menuLastPage()
{
    submenu_list_t *cur = selected_item->item->current;
    if (cur && cur->next) {
        if (cur->next) {
            sfxPlay(SFX_CURSOR);
        }
        while (cur->next)
            cur = cur->next; // go to end

        selected_item->item->current = cur;

        int itms = ((items_list_t *)gTheme->itemsList->extended)->displayedItems;
        while (--itms && cur->prev) // and move back to have a full page
            cur = cur->prev;

        selected_item->item->pagestart = cur;
    }
}

static void menuNextV()
{
    submenu_list_t *cur = selected_item->item->current;

    if (cur && cur->next) {
        selected_item->item->current = cur->next;
        sfxPlay(SFX_CURSOR);
        // coverflow slide animation; the wrap branch below stays instant
        if (gTheme->coverflow)
            thmTriggerCoverflowAnim(1);

        // if the current item is beyond the page start, move the page start one page down
        cur = selected_item->item->pagestart;
        int itms = ((items_list_t *)gTheme->itemsList->extended)->displayedItems + 1;
        while (--itms && cur)
            if (selected_item->item->current == cur)
                return;
            else
                cur = cur->next;

        selected_item->item->pagestart = selected_item->item->current;
    } else { // wrap to start
        menuFirstPage();
        /*
          Animate the wrap too, but ONLY in coverflow (#271).

          The carousel's visible window already wraps -- drawCoverFlow fans covers[] out through
          menu->item->last / ->submenu -- so last->first is a single VISUAL step there, exactly like
          every other move. Leaving it instant (10c19f1b's documented intent) therefore singled out
          the two entries at the seam: stepping onto the first game, and the step immediately after
          it, got no slide and no scale transfer while every other step did. That is what the
          reporter sees as the selection "clipping/skipping on the first game and the one following
          it", and it is also the most likely source of "the 3D effect drops to a flat 2D scroll" --
          because at the seam it literally does.

          The flat list views keep the instant jump: there the wrap really is an N-item page jump,
          not one step, so a one-step slide would misrepresent it.
        */
        if (gTheme->coverflow)
            thmTriggerCoverflowAnim(1);
    }
}

static void menuPrevV()
{
    submenu_list_t *cur = selected_item->item->current;

    if (cur && cur->prev) {
        selected_item->item->current = cur->prev;
        sfxPlay(SFX_CURSOR);

        // if the current item is on the page start, move the page start one page up
        if (selected_item->item->pagestart == cur) {
            int itms = ((items_list_t *)gTheme->itemsList->extended)->displayedItems + 1; // +1 because the selection will move as well
            while (--itms && selected_item->item->pagestart->prev)
                selected_item->item->pagestart = selected_item->item->pagestart->prev;
        }

        // coverflow slide animation; the wrap branch below stays instant
        if (gTheme->coverflow)
            thmTriggerCoverflowAnim(-1);
    } else { // wrap to end
        menuLastPage();
        // Mirror of the wrap in menuNextV -- see the rationale there (#271).
        if (gTheme->coverflow)
            thmTriggerCoverflowAnim(-1);
    }
}

static void menuNextPage()
{
    submenu_list_t *cur = selected_item->item->pagestart;
    int displayed = ((items_list_t *)gTheme->itemsList->extended)->displayedItems;

    // Probe to the item one row past the bottom of the current page. If the end comes first, the
    // whole list already fits on screen -> R1 is a no-op (don't over-scroll a sub-page list, which
    // previously collapsed it to just the last item -- #48).
    submenu_list_t *probe = cur;
    int n = displayed;
    while (n-- && probe)
        probe = probe->next;

    if (cur && probe) {
        int itms = displayed + 1;
        sfxPlay(SFX_CURSOR);

        while (--itms && cur->next)
            cur = cur->next;

        selected_item->item->current = cur;
        selected_item->item->pagestart = selected_item->item->current;
    }
}

static void menuPrevPage()
{
    submenu_list_t *cur = selected_item->item->pagestart;

    if (cur && cur->prev) {
        int itms = ((items_list_t *)gTheme->itemsList->extended)->displayedItems + 1;
        sfxPlay(SFX_CURSOR);

        while (--itms && cur->prev)
            cur = cur->prev;

        selected_item->item->current = cur;
        selected_item->item->pagestart = selected_item->item->current;
    }
    // else: already on the first page -> no action. Page scroll CLAMPS at the
    // boundary (like R1/menuNextPage), it does not wrap. The old wrap called
    // menuLastPage(), which on a multi-page list flung L1 to the LAST page and on
    // a single-page list jumped the SELECTION to the last item -- both reported in
    // issue #48 (the prior #48 fix only clamped R1, leaving this L1 wrap behind).
}

// True once the global menu list contains a registered device/mode tab (returns menu != NULL).
// initSupport uses the FALSE case to spot "user enabled the FIRST tab from the start menu",
// where nothing would otherwise take them to it (#254).
int menuHasRegisteredItems(void)
{
    return menu != NULL;
}

void menuSetSelectedItem(menu_item_t *item)
{
    menu_list_t *itm = menu;

    while (itm) {
        if (itm->item == item) {
            selected_item = itm;
            return;
        }

        itm = itm->next;
    }
}

void menuRenderMenu()
{
    if (guiDrawBGSettings() == 0)
        guiDrawBGPlasma();

    if (!mainMenu)
        return;

    // draw the animated menu
    if (!mainMenuCurrent)
        mainMenuCurrent = mainMenu;

    submenu_list_t *it = mainMenu;

    // calculate the number of items
    int count = 0;
    int sitem = 0;
    for (; it; count++, it = it->next) {
        if (it == mainMenuCurrent)
            sitem = count;
    }

    int spacing = 25;
    int y = (gTheme->usedHeight >> 1) - (spacing * (count >> 1));
    int cp = 0; // current position
    for (it = mainMenu; it; it = it->next, cp++) {
        // render, advance
        fntRenderString(gTheme->fonts[0], 320, y, ALIGN_CENTER, 0, 0, submenuItemGetText(&it->item), (cp == sitem) ? gTheme->selTextColor : gTheme->textColor);
        y += spacing;
        if (it->item.id == MENU_TOOLS) // extra half-gap before the About/Save/Exit/PowerOff tail block
            y += spacing / 2;
    }

    // hints
    guiDrawSubMenuHints();
}

int menuSetParentalLockCheckState(int enabled)
{
    int wasEnabled;

    wasEnabled = parentalLockCheckEnabled;
    parentalLockCheckEnabled = enabled ? 1 : 0;

    return wasEnabled;
}

int menuCheckParentalLock(void)
{
    const char *parentalLockPassword;
    char password[CONFIG_KEY_VALUE_LEN];
    int result;

    result = 0; // Default to unlocked.
    if (parentalLockCheckEnabled) {
        config_set_t *configOPL = configGetByType(CONFIG_OPL);

        // Prompt for password, only if one was set.
        if (configGetStr(configOPL, CONFIG_OPL_PARENTAL_LOCK_PWD, &parentalLockPassword) && (parentalLockPassword[0] != '\0')) {
            password[0] = '\0';
            if (diaShowKeyb(password, CONFIG_KEY_VALUE_LEN, 1, _l(_STR_PARENLOCK_ENTER_PASSWORD_TITLE))) {
                if (strncmp(parentalLockPassword, password, CONFIG_KEY_VALUE_LEN) == 0) {
                    result = 0;
                    parentalLockCheckEnabled = 0; // Stop asking for the password.
                } else if (strncmp(OPL_PARENTAL_LOCK_MASTER_PASS, password, CONFIG_KEY_VALUE_LEN) == 0) {
                    guiMsgBox(_l(_STR_PARENLOCK_DISABLE_WARNING), 0, NULL);

                    configRemoveKey(configOPL, CONFIG_OPL_PARENTAL_LOCK_PWD);
                    saveConfig(CONFIG_OPL, 1);

                    result = 0;
                    parentalLockCheckEnabled = 0; // Stop asking for the password.
                } else {
                    guiMsgBox(_l(_STR_PARENLOCK_PASSWORD_INCORRECT), 0, NULL);
                    result = EACCES;
                }
            } else // User aborted.
                result = EACCES;
        }
    }

    return result;
}

void menuHandleInputMenu()
{
    if (!mainMenu)
        return;

    if (!mainMenuCurrent)
        mainMenuCurrent = mainMenu;

    if (getKey(KEY_UP)) {
        sfxPlay(SFX_CURSOR);
        if (mainMenuCurrent->prev)
            mainMenuCurrent = mainMenuCurrent->prev;
        else // rewind to the last item
            while (mainMenuCurrent->next)
                mainMenuCurrent = mainMenuCurrent->next;
    }

    if (getKey(KEY_DOWN)) {
        sfxPlay(SFX_CURSOR);
        if (mainMenuCurrent->next)
            mainMenuCurrent = mainMenuCurrent->next;
        else
            mainMenuCurrent = mainMenu;
    }

    if (getKeyOn(gSelectButton)) {
        // execute the item via looking at the id of it
        int id = mainMenuCurrent->item.id;

        sfxPlay(SFX_CONFIRM);

        if (id == MENU_LAUNCH_PS2_DISC) {
            if (sysLaunchDisc() < 0) // success never returns; <0 means no/!PS2 disc -> stay in OPL
                guiMsgBox(_l(_STR_DISC_LAUNCH_ERR), 0, NULL);
        } else if (id == MENU_SETTINGS) {
            if (menuCheckParentalLock() == 0)
                guiShowConfig();
        } else if (id == MENU_GAME_SOURCES) {
            if (menuCheckParentalLock() == 0)
                guiShowDeviceConfig();
        } else if (id == MENU_INTERFACE) {
            if (menuCheckParentalLock() == 0)
                guiShowUIConfig();
        } else if (id == MENU_DISPLAY) {
            if (menuCheckParentalLock() == 0)
                guiShowDisplayConfig();
        } else if (id == MENU_GAME_LAUNCHING) {
            if (menuCheckParentalLock() == 0)
                guiShowLaunchConfig();
        } else if (id == MENU_POPSTARTER) {
            if (menuCheckParentalLock() == 0)
                guiShowVcdConfig();
        } else if (id == MENU_MMCE) {
            if (menuCheckParentalLock() == 0)
                guiShowMmceConfig();
        } else if (id == MENU_NETWORK) {
            if (menuCheckParentalLock() == 0)
                guiShowNetConfig();
        } else if (id == MENU_CONTROLLER) {
            if (menuCheckParentalLock() == 0)
                guiShowControllerConfig();
        } else if (id == MENU_AUDIO) {
            if (menuCheckParentalLock() == 0)
                guiShowAudioConfig();
        } else if (id == MENU_SECURITY) {
            if (menuCheckParentalLock() == 0)
                guiShowSecurityConfig();
        } else if (id == MENU_ADVANCED) {
            if (menuCheckParentalLock() == 0)
                guiShowAdvancedConfig();
        } else if (id == MENU_TOOLS) {
            if (menuCheckParentalLock() == 0)
                guiShowToolsConfig();
        } else if (id == MENU_ABOUT) {
            guiShowAbout();
        } else if (id == MENU_SAVE_CHANGES) {
            if (menuCheckParentalLock() == 0) {
                guiGameSaveOSDLanguageGlobalConfig(configGetByType(CONFIG_GAME));
#ifdef PADEMU
                guiGameSavePadEmuGlobalConfig(configGetByType(CONFIG_GAME));
                guiGameSavePadMacroGlobalConfig(configGetByType(CONFIG_GAME));
#endif
                saveConfig(CONFIG_OPL | CONFIG_NETWORK | CONFIG_GAME, 1);
                menuSetParentalLockCheckState(1); // Re-enable parental lock check.
            }
        } else if (id == MENU_EXIT) {
            if (guiMsgBox(_l(_STR_CONFIRMATION_EXIT), 1, NULL))
                sysExecExit();
        } else if (id == MENU_POWER_OFF) {
            if (guiMsgBox(_l(_STR_CONFIRMATION_POFF), 1, NULL))
                sysPowerOff();
        }

        // so the exit press wont propagate twice
        readPads();
    }

    if (getKeyOn(KEY_START) || getKeyOn(gSelectButton == KEY_CIRCLE ? KEY_CROSS : KEY_CIRCLE)) {
        // Check if there is anything to show the user, at all. BDM uses the EFFECTIVE mode: a
        // selected UDPBD protocol floors it to Auto (its tab can exist even with raw BDM = Off).
        if (gAPPStartMode || gETHStartMode || bdmEffectiveStartMode() || gHDDStartMode || gMMCEStartMode || gFAVStartMode) {
            guiSwitchScreen(GUI_SCREEN_MAIN);
            refreshMenuPosition();
        }
    }
}

static void menuRenderElements(theme_elems_t *elems, int allowItemConfig, config_set_t *renderConfig)
{
    // selected_item can't be NULL here as we only allow to switch to "Main" rendering when there is at least one device activated
    theme_element_t *elem = elems->first;
    // NAV-PARITY TEST BUILD (#340): official calls _menuRequestConfig() unconditionally from the
    // render path -- no needsItemConfig theme flag, no menuCanRequestItemConfig settle gate.
    if (allowItemConfig)
        _menuRequestConfig();

    WaitSema(menuSemaId);

    /* _menuRequestConfig() may have configFree()'d and NULLed itemConfig on the
     * frame the selection changed.  The caller captured renderConfig from
     * itemConfig BEFORE that call, so using it now would be a use-after-free.
     * Re-read the live pointer under the lock; the draw path tolerates NULL. */
    if (allowItemConfig)
        renderConfig = itemConfig;

    while (elem) {
        if (elem->drawElem)
            elem->drawElem(selected_item, selected_item->item->current, renderConfig, elem);

        elem = elem->next;
    }
    SignalSema(menuSemaId);
}

void menuRenderMain(void)
{
    item_list_t *list = selected_item->item->userdata;
    // MMCE main-screen item config re-enabled (#49): the b8bff90e-era hard-disable predates every
    // piece of today's gating (worker settles, the 20-frame idle gate, the #120 hardening) and it
    // meant the main-page #DiscType/#System badges could NEVER resolve on MMCE. The request now
    // flows through menuCanRequestItemConfig's settled gate: at most one paced CFG open per settled
    // selection -- the same browse-time read the FAV tab has always done for MMCE-sourced items.
    int allowItemConfig = 1;
    config_set_t *renderConfig = itemConfig;

    if (vcdViewActive(list->mode)) {
        // VCD/PS1 listings render with the vcd family (vcdMain*; each slot falls back at parse time to
        // appsMain* then main*). The VCD list uses its OWN items-list slot (vcdItemsList) so it keeps a
        // SEPARATE cover cache from the device's ISO list -- the view reuses the device's game list
        // (same item ids), so a shared cache thrashes on every L3 toggle. Falls back to the games list
        // when the theme defines no 4th ItemsList (never NULL). This also covers the Favourites tab's
        // own VCD view (its L3 toggle): VCD favourites render with the PS1 family, not the favs family.
        menuRenderElements(&gTheme->vcdMainElems, allowItemConfig, renderConfig);
        gTheme->itemsList = thmResolveItemsList(&gTheme->vcdMainElems, gTheme->vcdItemsList ? gTheme->vcdItemsList : gTheme->gamesItemsList, selected_item->item->icon_id);
    } else if (list->mode == FAV_MODE) {
        // Favourites (ISO view) render with the theme's favs family (favsMain*); falls back to the game
        // elements when the theme defines no favsMain override.
        menuRenderElements(&gTheme->favsMainElems, allowItemConfig, renderConfig);
        // Fall back to the games list (the slot every real theme populates -- the 1st/only ItemsList)
        // when the theme defines no favs/apps/vcd ItemsList, so gTheme->itemsList stays set: the
        // scroll/paging code (menuNextV/PrevV/Page) derefs gTheme->itemsList->extended without a NULL check.
        // thmResolveItemsList overrides the slot with a devices=-filtered ItemsList when one covers this
        // page's device icon -- the same rule drawItemsList gates on, so paging math matches the drawn rows.
        gTheme->itemsList = thmResolveItemsList(&gTheme->favsMainElems, gTheme->favsItemsList ? gTheme->favsItemsList : gTheme->gamesItemsList, selected_item->item->icon_id);
    } else if (list->mode == APP_MODE) {
        menuRenderElements(&gTheme->appsMainElems, allowItemConfig, renderConfig);
        gTheme->itemsList = thmResolveItemsList(&gTheme->appsMainElems, gTheme->appsItemsList ? gTheme->appsItemsList : gTheme->gamesItemsList, selected_item->item->icon_id);
    } else {
        menuRenderElements(&gTheme->mainElems, allowItemConfig, renderConfig);
        gTheme->itemsList = thmResolveItemsList(&gTheme->mainElems, gTheme->gamesItemsList, selected_item->item->icon_id);
    }
}

// Coverflow rotates the nav axis on the MAIN screen only: Left/Right step through the
// carousel (a vertical list move) while Up/Down switch device menus. Non-coverflow themes
// behave exactly as before. The info screen is intentionally NOT rotated (menuHandleInputInfo).
static void menuNavigateLeft()
{
    if (gTheme->coverflow)
        menuPrevV();
    else
        menuPrevH();
}

static void menuNavigateRight()
{
    if (gTheme->coverflow)
        menuNextV();
    else
        menuNextH();
}

static void menuNavigateUp()
{
    if (gTheme->coverflow)
        menuPrevH();
    else
        menuPrevV();
}

static void menuNavigateDown()
{
    if (gTheme->coverflow)
        menuNextH();
    else
        menuNextV();
}

void menuHandleInputMain()
{
    if (getKey(KEY_LEFT)) {
        menuNavigateLeft();
    } else if (getKey(KEY_RIGHT)) {
        menuNavigateRight();
    } else if (getKey(KEY_UP)) {
        menuNavigateUp();
    } else if (getKey(KEY_DOWN)) {
        menuNavigateDown();
    } else if (getKeyOn(KEY_CROSS)) {
        selected_item->item->execCross(selected_item->item);
    } else if (getKeyOn(KEY_TRIANGLE)) {
        selected_item->item->execTriangle(selected_item->item);
    } else if (getKeyOn(KEY_CIRCLE)) {
        selected_item->item->execCircle(selected_item->item);
    } else if (getKeyOn(KEY_SQUARE)) {
        selected_item->item->execSquare(selected_item->item);
    } else if (getKeyOn(KEY_START)) {
        // reinit main menu - show/hide items valid in the active context
        menuInitMainMenu();
        guiSwitchScreen(GUI_SCREEN_MENU);
    } else if (getKeyOn(KEY_SELECT)) {
        selected_item->item->refresh(selected_item->item);
    } else if (getKey(KEY_L1)) {
        menuPrevPage();
    } else if (getKey(KEY_R1)) {
        menuNextPage();
    } else if (getKeyOn(KEY_L2)) { // home
        menuFirstPage();
    } else if (getKeyOn(KEY_R2)) { // end
        menuLastPage();
    } else if (getKeyOn(KEY_R3)) { // toggle favourite
        if (selected_item->item->fav)
            selected_item->item->fav(selected_item->item);
    } else if (getKeyOn(KEY_L3)) { // toggle VCD view (disc list <-> POPS/*.VCD)
        if (selected_item->item->toggleView)
            selected_item->item->toggleView(selected_item->item);
    }

    // Last Played Auto Start
    if (RemainSecs < 0) {
        DisableCron = 1; // Disable Counter
        if (gSelectButton == KEY_CIRCLE)
            selected_item->item->execCircle(selected_item->item);
        else
            selected_item->item->execCross(selected_item->item);
    }
}

// Info-element family for a list: mirrors menuRenderInfo's dispatch (VCD view first -- it also
// covers the Favourites tab's L3 VCD view -- then FAV/APP, else the games family).
//
// A page-specific info family (VCD / Favourites / Apps) falls back to the theme's OWN games info family
// when the LOADED theme does not define it -- NEVER to the built-in/baked theme (gTheme is always the
// current theme, and every theme parses its families from a blank slate, themes.c). A custom
// conf_theme.cfg that omits e.g. the favs-info section leaves favsInfoElems EMPTY (validateBackgroundElems
// adds a background ONLY to an already-non-empty info family), so without this the Favourites info page
// rendered NOTHING (brenotomaz, #213: "on the favorites info page, they don't load at all"). Rule
// (NathanNeurotic): fav info defaults to MAIN info when the custom theme doesn't provide it; a custom
// theme uses its OWN assets entirely, just like APPS -- never our baked assets. Empty-family-gated, so a
// theme that DOES define the page keeps its own layout byte-for-byte.
static theme_elems_t *menuGetInfoElems(item_list_t *list)
{
    if (list != NULL && vcdViewActive(list->mode))
        return gTheme->vcdInfoElems.first ? &gTheme->vcdInfoElems : &gTheme->infoElems;
    if (list != NULL && list->mode == FAV_MODE)
        return gTheme->favsInfoElems.first ? &gTheme->favsInfoElems : &gTheme->infoElems;
    if (list != NULL && list->mode == APP_MODE)
        return gTheme->appsInfoElems.first ? &gTheme->appsInfoElems : &gTheme->infoElems;
    return &gTheme->infoElems;
}

void menuRenderInfo(void)
{
    item_list_t *list = selected_item->item->userdata;

    menuRenderElements(menuGetInfoElems(list), 1, itemConfig);

    if (vcdViewActive(list->mode)) {
        // The VCD list uses vcdItemsList (its own cover cache); falls back to the games list when
        // absent so itemsList is never NULL. Also covers the Favourites tab's VCD view (L3 toggle).
        // Resolution goes through thmResolveItemsList against the MAIN family (info families define
        // no lists): paging on the info screen must count the rows of the list the MAIN screen drew.
        gTheme->itemsList = thmResolveItemsList(&gTheme->vcdMainElems, gTheme->vcdItemsList ? gTheme->vcdItemsList : gTheme->gamesItemsList, selected_item->item->icon_id);
    } else if (list->mode == FAV_MODE) {
        gTheme->itemsList = thmResolveItemsList(&gTheme->favsMainElems, gTheme->favsItemsList ? gTheme->favsItemsList : gTheme->gamesItemsList, selected_item->item->icon_id);
    } else if (list->mode == APP_MODE) {
        gTheme->itemsList = thmResolveItemsList(&gTheme->appsMainElems, gTheme->appsItemsList ? gTheme->appsItemsList : gTheme->gamesItemsList, selected_item->item->icon_id);
    } else {
        gTheme->itemsList = thmResolveItemsList(&gTheme->mainElems, gTheme->gamesItemsList, selected_item->item->icon_id);
    }
}

void menuHandleInputInfo()
{
    if (getKeyOn(KEY_CROSS)) {
        if (gSelectButton == KEY_CIRCLE)
            guiSwitchScreen(GUI_SCREEN_MAIN);
        else
            selected_item->item->execCross(selected_item->item);
    } else if (getKey(KEY_UP)) {
        menuPrevV();
    } else if (getKey(KEY_DOWN)) {
        menuNextV();
    } else if (getKeyOn(KEY_CIRCLE)) {
        if (gSelectButton == KEY_CROSS)
            guiSwitchScreen(GUI_SCREEN_MAIN);
        else
            selected_item->item->execCircle(selected_item->item);
    } else if (getKey(KEY_L1)) {
        menuPrevPage();
    } else if (getKey(KEY_R1)) {
        menuNextPage();
    } else if (getKeyOn(KEY_L2)) {
        menuFirstPage();
    } else if (getKeyOn(KEY_R2)) {
        menuLastPage();
    }
}

void menuRenderGameMenu()
{
    if (guiDrawBGSettings() == 0)
        guiDrawBGPlasma();

    if (!gameMenu)
        return;

    // If the device menu that has the selected game suddenly goes invisible (device was removed), switch
    // back to the game list menu.
    if (selected_item->item->visible == 0) {
        guiSwitchScreen(GUI_SCREEN_MAIN);
        return;
    }

    // If we enter the game settings menu and there's no selected item bail out. I'm not entirely sure how we get into
    // this state but it seems to happen on some consoles when transitioning from the game settings menu back to the game
    // list menu.
    if (selected_item->item->current == NULL)
        return;

    // draw the animated menu
    if (!gameMenuCurrent)
        gameMenuCurrent = gameMenu;

    submenu_list_t *it = gameMenu;

    // calculate the number of items
    int count = 0;
    int sitem = 0;
    for (; it; count++, it = it->next) {
        if (it == gameMenuCurrent)
            sitem = count;
    }

    int spacing = 25;
    int y = (gTheme->usedHeight >> 1) - (spacing * (count >> 1));
    int cp = 0; // current position

    // game title
    fntRenderString(gTheme->fonts[0], 320, 20, ALIGN_CENTER, 0, 0, selected_item->item->current->item.text, gTheme->selTextColor);

    // config source
    char *cfgSource = gameConfigSource();
    fntRenderString(gTheme->fonts[0], 320, 40, ALIGN_CENTER, 0, 0, cfgSource, gTheme->textColor);

    // settings list
    for (it = gameMenu; it; it = it->next, cp++) {
        // render, advance
        fntRenderString(gTheme->fonts[0], 320, y, ALIGN_CENTER, 0, 0, submenuItemGetText(&it->item), (cp == sitem) ? gTheme->selTextColor : gTheme->textColor);
        y += spacing;
        if (cp == (GAME_SAVE_CHANGES - 1) || cp == (GAME_REMOVE_CHANGES - 1))
            y += spacing / 2;
    }

    // hints
    guiDrawSubMenuHints();
}

// Core-aware game menu: the Neutrino core ignores OPL's GSM, Cheats, PADEMU and
// OSD-language settings (see docs/NEUTRINO.md), so opening those panels for a
// Neutrino game would only edit dead options. Returns 1 when the selected game's
// Loader Core is Neutrino. (VMC and Compatibility stay available -- both are
// honored under Neutrino, with one exception: APA HDD launches emit no -mc args,
// because Neutrino has no APA/pfs backing store -- hddsupport toasts about it.)
static int gameMenuCoreIsNeutrino(void)
{
    int coreLoader = gDefaultCoreLoader; // no per-game $CoreLoader key -> follow the global default core
    if (itemConfig != NULL)
        configGetInt(itemConfig, CONFIG_ITEM_CORE_LOADER, &coreLoader);
    // VCD (PS1) games launch ONLY via POPSTARTER -- never Neutrino -- so a keyless VCD must not
    // inherit a Neutrino global default here, or its Cheats/GSM/OSD menu entries get blocked with
    // the wrong "not available under Neutrino" message (same exemption as the compat dialog's
    // coreNeverNeutrino flag in guigame.c). SMB is the same shape: ethsupport has no Neutrino
    // launch leg, the effective core is always <OPL>, so its Cheats/GSM/PADEMU panels ARE live
    // and must not grey under a Neutrino global default.
    if (selected_item != NULL && selected_item->item != NULL) {
        item_list_t *support = (item_list_t *)selected_item->item->userdata;
        if (support != NULL && (vcdViewActive(support->mode) || support->mode == ETH_MODE))
            return 0;
    }
    // UDPBD games are Neutrino-only even while $CoreLoader is still its OPL default.
    if (!coreLoader && selected_item != NULL && selected_item->item != NULL)
        coreLoader = bdmSupportIsUDPBD(selected_item->item->userdata);
    return coreLoader;
}

void menuHandleInputGameMenu()
{
    if (!gameMenu)
        return;

    if (!gameMenuCurrent)
        gameMenuCurrent = gameMenu;

    if (getKey(KEY_UP)) {
        sfxPlay(SFX_CURSOR);
        if (gameMenuCurrent->prev)
            gameMenuCurrent = gameMenuCurrent->prev;
        else // rewind to the last item
            while (gameMenuCurrent->next)
                gameMenuCurrent = gameMenuCurrent->next;
    }

    if (getKey(KEY_DOWN)) {
        sfxPlay(SFX_CURSOR);
        if (gameMenuCurrent->next)
            gameMenuCurrent = gameMenuCurrent->next;
        else
            gameMenuCurrent = gameMenu;
    }

    if (getKeyOn(gSelectButton)) {
        // execute the item via looking at the id of it
        int menuID = gameMenuCurrent->item.id;

        sfxPlay(SFX_CONFIRM);

        if (menuID == GAME_COMPAT_SETTINGS) {
            guiGameShowCompatConfig(selected_item->item->current->item.id, selected_item->item->userdata, itemConfig);
        } else if (menuID == GAME_CHEAT_SETTINGS) {
            if (gameMenuCoreIsNeutrino())
                guiMsgBox(_l(_STR_NEUTRINO_SETTING_NA), 0, NULL);
            else
                guiGameShowCheatConfig();
        } else if (menuID == GAME_GSM_SETTINGS) {
            if (gameMenuCoreIsNeutrino())
                guiMsgBox(_l(_STR_NEUTRINO_SETTING_NA), 0, NULL);
            else
                guiGameShowGSConfig();
        } else if (menuID == GAME_VMC_SETTINGS) {
            guiGameShowVMCMenu(selected_item->item->current->item.id, selected_item->item->userdata);
#ifdef PADEMU
        } else if (menuID == GAME_PADEMU_SETTINGS) {
            if (gameMenuCoreIsNeutrino())
                guiMsgBox(_l(_STR_NEUTRINO_SETTING_NA), 0, NULL);
            else
                guiGameShowPadEmuConfig(0);
        } else if (menuID == GAME_PADMACRO_SETTINGS) {
            if (gameMenuCoreIsNeutrino())
                guiMsgBox(_l(_STR_NEUTRINO_SETTING_NA), 0, NULL);
            else
                guiGameShowPadMacroConfig(0);
#endif
        } else if (menuID == GAME_OSD_LANGUAGE_SETTINGS) {
            if (gameMenuCoreIsNeutrino())
                guiMsgBox(_l(_STR_NEUTRINO_SETTING_NA), 0, NULL);
            else
                guiGameShowOSDLanguageConfig(0);
        } else if (menuID == GAME_SAVE_CHANGES) {
            if (guiGameSaveConfig(itemConfig, selected_item->item->userdata))
                configSetInt(itemConfig, CONFIG_ITEM_CONFIGSOURCE, CONFIG_SOURCE_USER);
            int okItem = menuSaveConfig();           // per-game CFG/<id>.cfg (itemConfig)
            int okGame = saveConfig(CONFIG_GAME, 0); // the global game config set
            // #245: claim "saved" only when BOTH writes actually persisted. The modal used to be
            // UNCONDITIONAL, so a failed mmceN: write showed "Game settings saved" IMMEDIATELY
            // followed by the async "error writing settings!" toast -- the exact contradiction Andrew
            // reported. On failure the error toast (raised in _menuSaveConfig / saveConfig, now with
            // the path + errno) already explains why; don't also lie that it saved.
            if (okItem && okGame)
                guiMsgBox(_l(_STR_GAME_SETTINGS_SAVED), 0, NULL);
            guiGameLoadConfig(selected_item->item->userdata, gameMenuLoadConfig(NULL));
        } else if (menuID == GAME_TEST_CHANGES) {
            guiGameTestSettings(selected_item->item->current->item.id, selected_item->item->userdata, itemConfig);
        } else if (menuID == GAME_REMOVE_CHANGES) {
            if (guiGameShowRemoveSettings(itemConfig, configGetByType(CONFIG_GAME))) {
                guiGameLoadConfig(selected_item->item->userdata, gameMenuLoadConfig(NULL));
            }
        } else if (menuID == GAME_RENAME_GAME) {
            menuRenameGame(&gameMenu);
        } else if (menuID == GAME_DELETE_GAME) {
            menuDeleteGame(&gameMenu);
        }
        // so the exit press wont propagate twice
        readPads();
    }

    if (getKeyOn(KEY_START) || getKeyOn(gSelectButton == KEY_CIRCLE ? KEY_CROSS : KEY_CIRCLE)) {
        guiSwitchScreen(GUI_SCREEN_MAIN);
    }
}

void menuRenderAppMenu()
{
    if (guiDrawBGSettings() == 0)
        guiDrawBGPlasma();

    if (!appMenu)
        return;

    // draw the animated menu
    if (!appMenuCurrent)
        appMenuCurrent = appMenu;

    submenu_list_t *it = appMenu;

    // calculate the number of items
    int count = 0;
    int sitem = 0;
    for (; it; count++, it = it->next) {
        if (it == appMenuCurrent)
            sitem = count;
    }

    int spacing = 25;
    int y = (gTheme->usedHeight >> 1) - (spacing * (count >> 1));
    int cp = 0; // current position

    // app title
    fntRenderString(gTheme->fonts[0], 320, 20, ALIGN_CENTER, 0, 0, selected_item->item->current->item.text, gTheme->selTextColor);

    for (it = appMenu; it; it = it->next, cp++) {
        // render, advance
        fntRenderString(gTheme->fonts[0], 320, y, ALIGN_CENTER, 0, 0, submenuItemGetText(&it->item), (cp == sitem) ? gTheme->selTextColor : gTheme->textColor);
        y += spacing;
    }

    // hints
    guiDrawSubMenuHints();
}

void menuHandleInputAppMenu()
{
    if (!appMenu)
        return;

    if (!appMenuCurrent)
        appMenuCurrent = appMenu;

    if (getKey(KEY_UP)) {
        sfxPlay(SFX_CURSOR);
        if (appMenuCurrent->prev)
            appMenuCurrent = appMenuCurrent->prev;
        else // rewind to the last item
            while (appMenuCurrent->next)
                appMenuCurrent = appMenuCurrent->next;
    }

    if (getKey(KEY_DOWN)) {
        sfxPlay(SFX_CURSOR);
        if (appMenuCurrent->next)
            appMenuCurrent = appMenuCurrent->next;
        else
            appMenuCurrent = appMenu;
    }

    if (getKeyOn(gSelectButton)) {
        // execute the item via looking at the id of it
        int menuID = appMenuCurrent->item.id;

        sfxPlay(SFX_CONFIRM);

        if (menuID == 0) {
            menuRenameGame(&appMenu);
        } else if (menuID == 1) {
            menuDeleteGame(&appMenu);
        }
        // so the exit press wont propagate twice
        readPads();
    }

    if (getKeyOn(KEY_START) || getKeyOn(gSelectButton == KEY_CIRCLE ? KEY_CROSS : KEY_CIRCLE)) {
        guiSwitchScreen(GUI_SCREEN_MAIN);
    }
}
