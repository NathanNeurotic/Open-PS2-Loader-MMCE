/*
  Copyright 2009, Ifcaro & volca
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.
*/

#include "include/opl.h"
#include "include/diag.h"
#include "include/menusys.h"
#include "include/supportbase.h" // sbSetConfigStatSize -- defer the #Size stat off the scroll path
#include "include/iosupport.h"
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
#include "include/favsupport.h"   // gFAVStartMode -- the Favourites tab counts as "something to show"
#include "include/folderbrowse.h" // menuFolderResetLeaving -- leave a device page at its folder root
#include "include/vcdsupport.h"   // vcdViewActive() -- VCD launches never use Neutrino
#include "include/bdmsupport.h"   // bdmSupportIsUDPBD() -- UDPBD games are Neutrino-only
#include <assert.h>

enum MENU_IDs {
    MENU_SETTINGS = 0,
    MENU_GAME_SOURCES,
    MENU_INTERFACE,
    MENU_DISPLAY,
    MENU_GAME_LAUNCHING,
    MENU_POPSTARTER, // NOTE(rebuild): MENU_MMCE joins with checklist item 1
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
    // NOTE(rebuild): MENU_POPSTARTER (items 12-20) and MENU_MMCE (item 1) join here with their steps.
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
static int menuSaveResult; // last per-game configWrite() result, published by _menuSaveConfig
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

static void menuRenameGame(submenu_list_t **submenu)
{
    if (!selected_item->item->current) {
        return;
    }

    if (!gEnableWrite)
        return;

    item_list_t *support = selected_item->item->userdata;

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

// Waited-on load (the DIALOG path: menuLoadConfig / gameMenuLoadConfig). Deliberately keeps the
// original shape -- the read runs UNDER menuSemaId -- because a caller is blocked in
// guiHandleDeferedIO on actionStatus and will hand whatever this publishes straight to
// itemLaunch()/guiGameLoadConfig(), neither of which tolerates a NULL config_set_t. Publishing
// unconditionally is what makes that safe, and the caller already shows a "loading settings"
// overlay, so the brief freeze here is by design and pre-existing.
static void _menuLoadConfig()
{
    WaitSema(menuSemaId);
    if (!itemConfig) {
        item_list_t *list = selected_item->item->userdata;
        itemConfig = list->itemGetConfig(list, itemConfigId);
    }
    actionStatus = 0;
    SignalSema(menuSemaId);
}

// Browse-time load (the SETTLE path: menuRenderElements -> _menuRequestConfig each frame). NOBODY
// waits on this one -- actionStatus is already 0 on that path -- so it can afford to do the read
// OUTSIDE menuSemaId, which is the whole point: menuRenderElements takes that sema every frame,
// and itemGetConfig is a per-game CFG open/read on the game's device, so holding it across the read
// froze rendering and input for the entire read every time the cursor settled on a row (visible on
// a slow USB stick with art off; worse with art on, when the same device is also serving covers).
// Snapshot under the sema, read outside it, re-check and publish under it again -- the same shape
// as _menuResolveInfoSize below.
static void _menuLoadConfigAsync()
{
    item_list_t *list = NULL;
    config_set_t *loadedConfig = NULL;
    int configId = -1;

    WaitSema(menuSemaId);
    if (itemConfig == NULL && selected_item != NULL && selected_item->item != NULL && itemConfigId >= 0) {
        list = selected_item->item->userdata;
        configId = itemConfigId;
    }
    SignalSema(menuSemaId);

    // Nothing wanted (already cached, or the id was invalidated while this request sat in the
    // queue). Deliberately does NOT touch actionStatus: this variant is only ever queued when no
    // waiter exists, and a dialog that armed one in the meantime queues its own _menuLoadConfig.
    if (list == NULL)
        return;

    loadedConfig = list->itemGetConfig(list, configId);

    // Publish only if this is still exactly what the menu wants. The id alone is NOT enough: ids are
    // per-list indices, so a tab switch mid-read yields the same number from a different device --
    // re-check the owning list too, or device A's per-game config lands on device B's row.
    WaitSema(menuSemaId);
    if (loadedConfig != NULL && itemConfig == NULL && itemConfigId == configId &&
        selected_item != NULL && selected_item->item != NULL && selected_item->item->userdata == list) {
        itemConfig = loadedConfig;
        loadedConfig = NULL;
    }
    SignalSema(menuSemaId);

    if (loadedConfig != NULL)
        configFree(loadedConfig); // lost the race -- drop it, never publish another row's config
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

    // itemGetConfig runs OUTSIDE the semaphore on purpose: it is the slow call (a CFG read plus the
    // ISO stat this whole mechanism exists to defer), and holding menuSemaId across it would block
    // the GUI thread for exactly as long as the stat we are trying to keep off the scroll path.
    sbSetConfigStatSize(1);
    loadedConfig = list->itemGetConfig(list, configId);
    sbSetConfigStatSize(0);

    if (loadedConfig == NULL)
        return;

    // Re-check the selection under the sema: if the user scrolled away while the stat ran, the
    // freshly loaded config belongs to a row that is no longer current -- drop it rather than
    // publish someone else's #Size.
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

// Queued when the info screen opens: resolve #Size for the current item without blocking the UI.
void menuRequestInfoSize(void)
{
    ioPutRequest(IO_CUSTOM_SIMPLEACTION, &_menuResolveInfoSize);
}

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
        // gLastSaveErrno was latched inside configWrite at the real failure site.
        setErrorMessagePathCode(_STR_ERROR_SAVING_SETTINGS_TO, itemConfig ? itemConfig->filename : "", gLastSaveErrno);
}

// Every path out of here MUST either queue a load that will clear actionStatus, or clear it itself.
// The old version had two paths that did neither: the selection changed but the idle-frame gate was
// not met, or the row was current with itemConfig still NULL. Both left actionStatus=1 with nothing
// in flight, so guiHandleDeferedIO span on a load that was never requested -- an outright hang
// before rebuild-38, and a 30s freeze plus a bogus error toast after it.
// NOTE(rebuild): the fork also swaps list->delay for MENU_APP_CONFIG_IDLE_FRAMES on APP_MODE. That
// constant does not exist here and is a separate tuning change, so the idle gate is left exactly as
// it was; only the completion contract is repaired.
static void _menuRequestConfig()
{
    int shouldQueueLoad = 0;
    int waiterPending;

    WaitSema(menuSemaId);
    // Which load variant to queue. A waiter (dialog path) needs the publish-unconditionally
    // in-sema load; the browse/settle path has none and gets the async one that keeps the sema
    // free across the device read. Read under the sema with everything else it decides on.
    waiterPending = (actionStatus != 0);
    if (selected_item == NULL || selected_item->item == NULL || selected_item->item->current == NULL) {
        // The list can be rebuilt out from under us between the GUI's check and this callback.
        actionStatus = 0;
    } else if (itemConfigId != selected_item->item->current->item.id) {
        if (itemConfig) {
            configFree(itemConfig);
            itemConfig = NULL;
        }
        item_list_t *list = selected_item->item->userdata;
        if (itemConfigId == -1 || guiInactiveFrames >= list->delay) {
            itemConfigId = selected_item->item->current->item.id;
            shouldQueueLoad = 1;
        } else
            actionStatus = 0; // still settling: nothing queued, so release the waiter now
    } else if (itemConfig == NULL && actionStatus != 0) {
        shouldQueueLoad = 1; // right row, but its config never landed -- ask again
    } else
        actionStatus = 0;

    SignalSema(menuSemaId);

    // Queue OUTSIDE the sema: both load variants take it too.
    if (shouldQueueLoad && ioPutRequest(IO_CUSTOM_SIMPLEACTION, waiterPending ? (void *)&_menuLoadConfig : (void *)&_menuLoadConfigAsync) != IO_OK) {
        WaitSema(menuSemaId);
        if (itemConfig == NULL)
            itemConfigId = -1; // let the next pass retry rather than caching a row that never loaded
        actionStatus = 0;
        SignalSema(menuSemaId);
    }
}

config_set_t *menuLoadConfig()
{
    actionStatus = 1;
    itemConfigId = -1;
    guiHandleDeferedIO(&actionStatus, _l(_STR_LOADING_SETTINGS), IO_CUSTOM_SIMPLEACTION, &_menuRequestConfig, OPL_DEFERRED_IO_TIMEOUT_MS);
    return itemConfig;
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
    submenuAppendItem(&mainMenu, -1, NULL, MENU_LAUNCH_PS2_DISC, _STR_LAUNCH_PS2_DISC);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_SETTINGS, _STR_SETTINGS);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_GAME_SOURCES, _STR_GAME_SOURCES);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_INTERFACE, _STR_INTERFACE_SETTINGS);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_DISPLAY, _STR_DISPLAY_SETTINGS);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_GAME_LAUNCHING, _STR_GAME_LAUNCHING);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_POPSTARTER, _STR_POPSTARTER);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_NETWORK, _STR_MENU_NETWORK);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_CONTROLLER, _STR_MENU_CONTROLLER);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_AUDIO, _STR_MENU_AUDIO);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_SECURITY, _STR_SECURITY_SETTINGS);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_ADVANCED, _STR_ADVANCED_SETTINGS);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_TOOLS, _STR_TOOLS);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_ABOUT, _STR_ABOUT);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_SAVE_CHANGES, _STR_SAVE_CHANGES);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_EXIT, _STR_EXIT);
    submenuAppendItem(&mainMenu, -1, NULL, MENU_POWER_OFF, _STR_POWEROFF);

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
    submenuAppendItem(&gameMenu, -1, NULL, GAME_COMPAT_SETTINGS, _STR_COMPAT_SETTINGS);
    submenuAppendItem(&gameMenu, -1, NULL, GAME_CHEAT_SETTINGS, _STR_CHEAT_SETTINGS);
    submenuAppendItem(&gameMenu, -1, NULL, GAME_GSM_SETTINGS, _STR_GSCONFIG);
    submenuAppendItem(&gameMenu, -1, NULL, GAME_VMC_SETTINGS, _STR_VMC_SCREEN);
#ifdef PADEMU
    submenuAppendItem(&gameMenu, -1, NULL, GAME_PADEMU_SETTINGS, _STR_PADEMUCONFIG);
    submenuAppendItem(&gameMenu, -1, NULL, GAME_PADMACRO_SETTINGS, _STR_PADMACROCONFIG);
#endif
    submenuAppendItem(&gameMenu, -1, NULL, GAME_OSD_LANGUAGE_SETTINGS, _STR_OSD_SETTINGS);
    submenuAppendItem(&gameMenu, -1, NULL, GAME_SAVE_CHANGES, _STR_SAVE_CHANGES);
    submenuAppendItem(&gameMenu, -1, NULL, GAME_TEST_CHANGES, _STR_TEST);
    submenuAppendItem(&gameMenu, -1, NULL, GAME_REMOVE_CHANGES, _STR_REMOVE_ALL_SETTINGS);
    if (gEnableWrite) {
        submenuAppendItem(&gameMenu, -1, NULL, GAME_RENAME_GAME, _STR_RENAME);
        submenuAppendItem(&gameMenu, -1, NULL, GAME_DELETE_GAME, _STR_DELETE);
    }

    gameMenuCurrent = gameMenu;
}

void menuInitAppMenu(void)
{
    if (appMenu)
        submenuDestroy(&appMenu);

    // initialize the menu
    submenuAppendItem(&appMenu, -1, NULL, 0, _STR_RENAME);
    submenuAppendItem(&appMenu, -1, NULL, 1, _STR_DELETE);

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
    // Find the first menu in the list that is visible and set it as the active menu.
    if (menu == NULL)
        return;

    menu_list_t *cur = menu;
    while (cur->item->visible == 0 && cur->next)
        cur = cur->next;

    if (cur->item->visible == 0) {
        // No visible menu was found, just set the current menu to the first one in the list.
        selected_item = menu;
    } else
        selected_item = cur;
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

// True once the global menu list contains a registered device/mode tab (returns menu != NULL).
// initSupport uses the FALSE case to spot "user enabled the FIRST tab from the start menu",
// where nothing would otherwise take them to it (#254).
int menuHasRegisteredItems(void)
{
    return menu != NULL;
}

static submenu_list_t *submenuAllocItem(int icon_id, char *text, int id, int text_id)
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
    it->item.favourited = 0;
    it->item.isFolder = 0;
    submenuRebuildCache(it);

    return it;
}

submenu_list_t *submenuAppendItem(submenu_list_t **submenu, int icon_id, char *text, int id, int text_id)
{
    if (*submenu == NULL) {
        *submenu = submenuAllocItem(icon_id, text, id, text_id);
        return *submenu;
    }

    submenu_list_t *cur = *submenu;

    // traverse till the end
    while (cur->next)
        cur = cur->next;

    // create new item
    submenu_list_t *newitem = submenuAllocItem(icon_id, text, id, text_id);

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

// Sorts the given submenu by comparing the on-screen titles
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
    if (cur) {
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
    if (cur) {
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
    } else { // wrap to end
        menuLastPage();
    }
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
    // Optional themed settings background (use_settings_bg): guiDrawBGSettings draws it and
    // returns non-zero; 0 means the theme has none, so fall back to the plasma. dia.c already
    // did this for dialogs -- these three menu screens did not, so a theme that shipped a
    // settings background got it on its dialogs and the plasma on its menus.
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
        if (cp == (MENU_ABOUT - 1))
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

                // SMB, UDPFS and UDPBD share the one SMAP NIC and cannot coexist -- each loader
                // refuses to start while either of the others is resident, and each loads its IOP
                // chain once per boot with no unload path. So once a stack is up, choosing a different
                // protocol changes the CONFIG and nothing else: the NIC keeps running whatever it
                // started with, for the rest of the session.
                //
                // The choice has just been written, which makes this the first moment it is safe to
                // act on. Offer to leave OPL so the user can start it again on the protocol they
                // picked. Doing this from the picker instead would have torn OPL down before Save
                // Changes ever ran and thrown the choice away -- those dialogs only touch RAM.
                //
                // sysExecExit is the Exit menu item's own path (deinit + exit), deliberately not a new
                // one: OPL has no in-place relaunch, and inventing an ELF-reload + IOP-reset path
                // underneath a settings screen would be a brand-new way to reach a black screen.
                // Declining costs nothing -- the saved protocol applies at the user's next launch.
                if (guiNetProtocolNeedsRestart() && guiMsgBox(_l(_STR_NETBOOT_RESTART), 1, NULL))
                    sysExecExit();
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
        // Check if there is anything to show the user, at all. This MUST list every tab that can
        // hold rows, or the user gets trapped in the settings menu with no way back to the list:
        // gFAVStartMode was missing, so a Favourites-only setup could not escape at all.
        // bdmEffectiveStartMode() rather than raw gBDMStartMode: UDPBD floors the BDM start mode, so
        // a UDPBD-only rig has rows while gBDMStartMode itself is still disabled.
        // NOTE(rebuild): the fork also ORs gMMCEStartMode here; that global arrives with checklist
        // item 1, and until then no MMCE tab can hold rows, so omitting it changes nothing.
        if (gAPPStartMode || gETHStartMode || bdmEffectiveStartMode() || gHDDStartMode || gFAVStartMode) {
            guiSwitchScreen(GUI_SCREEN_MAIN);
            refreshMenuPosition();
        }
    }
}

static void menuRenderElements(theme_element_t *elem)
{
    // selected_item can't be NULL here as we only allow to switch to "Main" rendering when there is at least one device activated
    _menuRequestConfig();

    WaitSema(menuSemaId);

    while (elem) {
        if (elem->drawElem)
            elem->drawElem(selected_item, selected_item->item->current, itemConfig, elem);

        elem = elem->next;
    }
    SignalSema(menuSemaId);
}

// Per-page element families fall back to the GAMES family when the theme defines none. Neither
// built-in theme (<OPL>, <Coverflow>) declares a single appsInfo*/favsInfo*/vcdInfo* element, and
// validateBackgroundElems only auto-adds a background to a NON-empty info group -- so dispatching
// straight to .first handed menuRenderElements a NULL list and the info screen (Square) drew
// NOTHING for apps, favourites and the PS1/VCD view. The main families are unaffected in practice
// (validateBackgroundElems seeds them a background, so .first is never NULL) but take the same
// guard so the two paths cannot drift again.
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

static theme_elems_t *menuGetMainElems(item_list_t *list)
{
    if (list != NULL && vcdViewActive(list->mode))
        return gTheme->vcdMainElems.first ? &gTheme->vcdMainElems : &gTheme->mainElems;
    if (list != NULL && list->mode == FAV_MODE)
        return gTheme->favsMainElems.first ? &gTheme->favsMainElems : &gTheme->mainElems;
    if (list != NULL && list->mode == APP_MODE)
        return gTheme->appsMainElems.first ? &gTheme->appsMainElems : &gTheme->mainElems;
    return &gTheme->mainElems;
}

void menuRenderMain(void)
{
    item_list_t *list = selected_item->item->userdata;

    if (vcdViewActive(list->mode)) {
        // VCD/PS1 listings render with the vcd family (vcdMain*; each slot falls back at parse time to
        // appsMain* then main*). The VCD list uses its OWN items-list slot (vcdItemsList) so it keeps a
        // SEPARATE cover cache from the device's ISO list -- the view reuses the device's game list
        // (same item ids), so a shared cache thrashes on every L3 toggle. This also covers the
        // Favourites tab's own VCD view: VCD favourites render with the PS1 family, not the favs one.
        menuRenderElements(menuGetMainElems(list)->first);
        gTheme->itemsList = thmResolveItemsList(&gTheme->vcdMainElems, gTheme->vcdItemsList ? gTheme->vcdItemsList : gTheme->gamesItemsList, selected_item->item->icon_id);
    } else if (list->mode == FAV_MODE) {
        menuRenderElements(menuGetMainElems(list)->first);
        gTheme->itemsList = thmResolveItemsList(&gTheme->favsMainElems, gTheme->favsItemsList ? gTheme->favsItemsList : gTheme->gamesItemsList, selected_item->item->icon_id);
    } else if (list->mode == APP_MODE) {
        menuRenderElements(menuGetMainElems(list)->first);
        gTheme->itemsList = thmResolveItemsList(&gTheme->appsMainElems, gTheme->appsItemsList ? gTheme->appsItemsList : gTheme->gamesItemsList, selected_item->item->icon_id);
    } else {
        menuRenderElements(menuGetMainElems(list)->first);
        // Always falls back to gamesItemsList, never NULL: the scroll/paging code (menuNextV/PrevV/
        // Page) derefs gTheme->itemsList->extended with no NULL check.
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
    } else if (getKeyOn(KEY_L3)) { // toggle VCD view (disc list <-> POPS/*.VCD; item 12)
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

void menuRenderInfo(void)
{
    item_list_t *list = selected_item->item->userdata;

    if (vcdViewActive(list->mode)) {
        menuRenderElements(menuGetInfoElems(list)->first);
        gTheme->itemsList = thmResolveItemsList(&gTheme->vcdInfoElems, gTheme->vcdItemsList ? gTheme->vcdItemsList : gTheme->gamesItemsList, selected_item->item->icon_id);
    } else if (list->mode == FAV_MODE) {
        menuRenderElements(menuGetInfoElems(list)->first);
        gTheme->itemsList = thmResolveItemsList(&gTheme->favsInfoElems, gTheme->favsItemsList ? gTheme->favsItemsList : gTheme->gamesItemsList, selected_item->item->icon_id);
    } else if (list->mode == APP_MODE) {
        menuRenderElements(menuGetInfoElems(list)->first);
        gTheme->itemsList = thmResolveItemsList(&gTheme->appsInfoElems, gTheme->appsItemsList ? gTheme->appsItemsList : gTheme->gamesItemsList, selected_item->item->icon_id);
    } else {
        menuRenderElements(menuGetInfoElems(list)->first);
        gTheme->itemsList = thmResolveItemsList(&gTheme->infoElems, gTheme->gamesItemsList, selected_item->item->icon_id);
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
    // Optional themed settings background (use_settings_bg): guiDrawBGSettings draws it and
    // returns non-zero; 0 means the theme has none, so fall back to the plasma. dia.c already
    // did this for dialogs -- these three menu screens did not, so a theme that shipped a
    // settings background got it on its dialogs and the plasma on its menus.
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

// Is the effective launch core for the selected game Neutrino? Per-game Cheats/GSM/PADEMU/OSD are
// OPL-core features, so under Neutrino they must say so rather than opening a panel that cannot
// take effect (checklist item 22's guidance half).
static int gameMenuCoreIsNeutrino(void)
{
    int coreLoader = gDefaultCoreLoader; // no per-game $CoreLoader key -> follow the global default core
    if (itemConfig != NULL)
        configGetInt(itemConfig, CONFIG_ITEM_CORE_LOADER, &coreLoader);
    // VCD (PS1) games launch ONLY via POPSTARTER -- never Neutrino -- so a keyless VCD must not
    // inherit a Neutrino global default here, or its Cheats/GSM/OSD menu entries get blocked with
    // the wrong "not available under Neutrino" message. SMB is the same shape: ethsupport has no
    // Neutrino launch leg, the effective core is always <OPL>, so its panels ARE live and must not
    // be blocked under a Neutrino global default.
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
                guiGameShowGSConfig(0);
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
            // #245: claim "saved" only when BOTH writes actually persisted. This modal was
            // UNCONDITIONAL, so a failed write showed "Game settings saved" immediately followed
            // by the async "error writing settings!" toast -- the exact contradiction Andrew
            // reported. On failure the error toast already names the path and errno; don't also
            // lie that it saved.
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
    // Optional themed settings background (use_settings_bg): guiDrawBGSettings draws it and
    // returns non-zero; 0 means the theme has none, so fall back to the plasma. dia.c already
    // did this for dialogs -- these three menu screens did not, so a theme that shipped a
    // settings background got it on its dialogs and the plasma on its menus.
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
