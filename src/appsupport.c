#include "include/opl.h"
#include "include/lang.h"
#include "include/gui.h"
#include "include/appsupport.h"
#include "include/themes.h"
#include "include/system.h"
#include "include/ioman.h"
#include "include/util.h"

#include "include/bdmsupport.h"
#include "include/ethsupport.h"
#include "include/vcdsupport.h"
#include "include/hddsupport.h"
#include "include/texcache.h"
#include "include/textures.h"

#include <elf-loader.h>

static int appForceUpdate = 1;
static int appItemCount = 0;
static char appStartupPath[256];
static char appResolvedStartupPath[256];
static int *appArtLookupTable;
static int appArtLookupTableSize = 0;

static config_set_t *configApps;
static app_info_t *appsList;

struct app_info_linked
{
    struct app_info_linked *next;
    app_info_t app;
};

// forward declaration
static item_list_t appItemList;

static void appFreeList(void);
static void appFreeArtLookup(void);
static int appBuildArtLookup(void);
static void appFreeLinkedList(struct app_info_linked *appsLinkedList);
static app_info_t *appLookupByStartup(const char *startup);
static void appSetArtSource(app_info_t *app);
static void appSetResolvedStartup(app_info_t *app);

static struct config_value_t *appGetConfigValue(int id)
{
    struct config_value_t *cur = configApps->head;

    while (id--) {
        cur = cur->next;
    }

    return cur;
}

static char *appGetELFName(char *name)
{
    // Looking for the ELF name
    char *pos = strrchr(name, '/');
    if (!pos)
        pos = strrchr(name, ':');
    if (pos) {
        return pos + 1;
    }

    return name;
}

static float appGetELFSize(char *path)
{
    int fd, size;
    float bytesInMiB = 1048576.0f;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        LOG("Failed to open APP %s\n", path);
        return 0.0f;
    }

    size = getFileSize(fd);
    close(fd);

    // Return size in MiB
    return (size / bytesInMiB);
}

static char *appGetBoot(char *device, int max, char *path)
{
    char *pos;

    // Looking for the boot device & filename from the path
    pos = strrchr(path, ':');
    if (pos != NULL) {
        int len = (int)(pos + 1 - path);
        if (len + 1 > max)
            len = max - 1;
        strncpy(device, path, len);
        device[len] = '\0';
    }

    return appGetELFName(path);
}

// POPSTARTER SMB selector detection for APPS: APPS -> SB.<game>.ELF
// Uses VCD_PREFIX_SMB and existing basename helper. Returns 1 if the
// resolved APP startup basename matches SB.*.ELF (case-insensitive).
static int appIsPopstarterSmb(const char *startup)
{
    const char *base;
    if (startup == NULL || startup[0] == '\0')
        return 0;
    base = appGetELFName((char *)startup);
    if (base == NULL || base[0] == '\0')
        return 0;
    if (strncasecmp(base, VCD_PREFIX_SMB, strlen(VCD_PREFIX_SMB)) != 0)
        return 0;
    size_t len = strlen(base);
    size_t pre = strlen(VCD_PREFIX_SMB);
    if (len <= pre + 4)
        return 0;
    const char *dot = strrchr(base, '.');
    if (dot == NULL || strcasecmp(dot, ".ELF") != 0)
        return 0;
    return 1;
}

static unsigned int appHashStartup(const char *value)
{
    unsigned int hash = 5381;

    while (value != NULL && *value != '\0') {
        hash = ((hash << 5) + hash) ^ (unsigned char)*value;
        value++;
    }

    return hash;
}

// Canonical-identity comparison for APP dedup (#253). Identity is the resolved startup (the full
// ELF path incl. its concrete device prefix) -- NEVER the title: two distinct APPs may share a
// title, and one APP may have two titles (conf_apps.cfg key vs title.cfg). `mass:`/`mass?:` legacy
// forms are already resolved to a concrete massN: by appSetResolvedStartup, so the slot prefix also
// keeps the same relative path on two DIFFERENT physical devices distinct. Compared equal here:
// '/' vs '\\', repeated separators, trailing separators, and case (the BDM filesystems OPL scans
// apps from are FAT/exFAT -- case-insensitive; a case-differing pair on a case-sensitive fs would
// have to collide on device+path in every other respect, which does not occur in practice).
static int appStartupKeyEqual(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return 0;
    // An empty startup is NOT an identity: it marks a record whose ELF path could not be resolved
    // (e.g. a conf_apps.cfg line with an empty value), and appBuildArtLookup already skips such
    // records as identity-less. Two of them must never merge -- one would silently vanish from the
    // list (CodeRabbit review of #255).
    if (a[0] == '\0' || b[0] == '\0')
        return 0;

    for (;;) {
        char ca = *a;
        char cb = *b;

        if (ca == '\\')
            ca = '/';
        if (cb == '\\')
            cb = '/';
        if (ca >= 'A' && ca <= 'Z')
            ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z')
            cb += 'a' - 'A';
        if (ca != cb)
            return 0;
        if (ca == '\0')
            return 1;
        a++;
        b++;
        if (ca == '/') {
            // Collapse separator runs; a trailing run is consumed here too, so the loop's next
            // iteration compares '\0' against '\0' -- trailing separators never differentiate.
            while (*a == '/' || *a == '\\')
                a++;
            while (*b == '/' || *b == '\\')
                b++;
        }
    }
}

// Drop duplicate discoveries from the freshly built appsList (#253: the same APP listed several
// times). The list is [legacy conf_apps.cfg entries...][scanned title.cfg entries...]; one APP
// present in BOTH channels (the normal migration path from conf_apps.cfg to per-app title.cfg)
// otherwise appears twice. First occurrence wins: order-stable, and because legacy entries sort
// first, only SCANNED records are ever dropped -- legacy records keep their indices, preserving the
// "legacy id <-> conf_apps.cfg node position" mapping appGetConfigValue relies on, the user's
// conf_apps.cfg edits, and the title that Favourites records bind to. A scanned duplicate still
// donates its argv1 (title.cfg-only metadata) to an empty-handed legacy survivor. Legacy-vs-legacy
// collisions (two conf_apps.cfg keys pointing at the same ELF) are KEPT: dropping one would shift
// the config-node mapping, and a duplicated hand-edited entry is the user's own data.
static int appDroppedCount;
static void appDedupList(void)
{
    appDroppedCount = 0;
    if (appsList == NULL || appItemCount <= 1)
        return;

    int kept = 0;
    for (int i = 0; i < appItemCount; i++) {
        int dup = -1;
        for (int j = 0; j < kept; j++) {
            if (appStartupKeyEqual(appsList[j].startup, appsList[i].startup)) {
                dup = j;
                break;
            }
        }
        if (dup >= 0 && !appsList[i].legacy) {
            appDroppedCount++;
            LOG("APPSUPPORT dedup: dropping scanned '%s' (%s) -- already listed as '%s' (%s)\n",
                appsList[i].title, appsList[i].startup, appsList[dup].title, appsList[dup].startup);
            if (appsList[dup].argv1[0] == '\0' && appsList[i].argv1[0] != '\0') {
                strncpy(appsList[dup].argv1, appsList[i].argv1, APP_ARGV1_MAX + 1);
                appsList[dup].argv1[APP_ARGV1_MAX] = '\0';
            }
            continue;
        }
        if (kept != i)
            memcpy(&appsList[kept], &appsList[i], sizeof(app_info_t));
        kept++;
    }
    appItemCount = kept;
}

// #253 diagnostics: write every entry's resolved identity to <config home>/apps-dump.txt so a
// reporter's file answers "which two prefixes did this app come in through" without a serial
// cable. Called only with debug enabled (gEnableDebug); small text file, rewritten per rebuild.
static void appDumpDiscovery(void)
{
    char path[300];
    FILE *f;

    snprintf(path, sizeof(path), "%sapps-dump.txt", configGetDir());
    f = fopen(path, "w");
    if (f == NULL) {
        LOG("APPSUPPORT unable to write %s\n", path);
        return;
    }
    fprintf(f, "# RiptOPL app discovery dump: %d apps, %d duplicate(s) dropped by dedup\n", appItemCount, appDroppedCount);
    fprintf(f, "# [index] [source] title | path | boot | resolved startup\n");
    for (int i = 0; appsList != NULL && i < appItemCount; i++) {
        fprintf(f, "%3d [%s] %s | %s | %s | %s\n",
                i, appsList[i].legacy ? "legacy" : "scan", appsList[i].title,
                appsList[i].path, appsList[i].boot, appsList[i].startup);
    }
    // Claim success only when the whole file is confirmed on disk -- the same
    // evidence-over-status-codes rule as the config save path (#245).
    if (fclose(f) == 0)
        LOG("APPSUPPORT discovery dump written to %s\n", path);
    else
        LOG("APPSUPPORT discovery dump write FAILED on close: %s\n", path);
}


static const char *appBuildStartupPath(const char *path, const char *boot)
{
    if (path[0] != '\0') {
        const char *leaf = appGetELFName((char *)path);
        if (strcmp(path, boot) == 0 || (leaf != path && strcmp(leaf, boot) == 0))
            return path;

        snprintf(appStartupPath, sizeof(appStartupPath), "%s/%s", path, boot);
        return appStartupPath;
    }

    return boot;
}

static const char *appResolveLegacyMassStartup(const char *startup)
{
    int fd;
    const char *suffix;

    if (startup == NULL)
        return NULL;

    if (strncmp(startup, "mass?:", 6) == 0)
        suffix = startup + 6;
    else if (strncmp(startup, "mass:", 5) == 0)
        suffix = startup + 5;
    else
        return startup;

    for (int i = 0; i < MAX_BDM_DEVICES; i++) {
        snprintf(appResolvedStartupPath, sizeof(appResolvedStartupPath), "mass%d:%s", i, suffix);

        fd = open(appResolvedStartupPath, O_RDONLY);
        if (fd >= 0) {
            close(fd);
            return appResolvedStartupPath;
        }
    }

    return startup;
}

static void appSetArtSource(app_info_t *app)
{
    char *artLookup;

    if (app == NULL)
        return;

    app->artDevice[0] = '\0';
    app->artLookup[0] = '\0';
    app->artMode = -1;

    if (app->startup[0] == '\0')
        return;

    artLookup = appGetBoot(app->artDevice, sizeof(app->artDevice), app->startup);
    if (artLookup != NULL) {
        strncpy(app->artLookup, artLookup, APP_BOOT_MAX + 1);
        app->artLookup[APP_BOOT_MAX] = '\0';
    }

    if (app->artDevice[0] != '\0')
        app->artMode = oplPath2Mode(app->artDevice);
}

static void appSetResolvedStartup(app_info_t *app)
{
    const char *startup;

    if (app == NULL) {
        return;
    }

    startup = appResolveLegacyMassStartup(appBuildStartupPath(app->path, app->boot));
    if (startup == NULL) {
        app->startup[0] = '\0';
        appSetArtSource(app);
        return;
    }

    strncpy(app->startup, startup, APP_STARTUP_MAX);
    app->startup[APP_STARTUP_MAX] = '\0';
    appSetArtSource(app);
}

static void appFreeArtLookup(void)
{
    if (appArtLookupTable != NULL) {
        free(appArtLookupTable);
        appArtLookupTable = NULL;
    }

    appArtLookupTableSize = 0;
}

static int appBuildArtLookup(void)
{
    int size;

    appFreeArtLookup();

    if (appsList == NULL || appItemCount <= 0)
        return 0;

    size = 1;
    while (size < appItemCount * 2)
        size <<= 1;

    if (size < 2)
        size = 2;

    appArtLookupTable = malloc(size * sizeof(int));
    if (appArtLookupTable == NULL)
        return -1;

    for (int i = 0; i < size; i++)
        appArtLookupTable[i] = -1;

    for (int i = 0; i < appItemCount; i++) {
        unsigned int slot;

        if (appsList[i].startup[0] == '\0')
            continue;

        slot = appHashStartup(appsList[i].startup) & (size - 1);
        while (appArtLookupTable[slot] != -1)
            slot = (slot + 1) & (size - 1);

        appArtLookupTable[slot] = i;
    }

    appArtLookupTableSize = size;
    return 0;
}

static app_info_t *appLookupByStartup(const char *startup)
{
    if (startup == NULL || appsList == NULL)
        return NULL;

    if (appArtLookupTable != NULL && appArtLookupTableSize > 0) {
        unsigned int slot = appHashStartup(startup) & (appArtLookupTableSize - 1);

        while (appArtLookupTable[slot] != -1) {
            int index = appArtLookupTable[slot];

            if (!strcmp(appsList[index].startup, startup))
                return &appsList[index];

            slot = (slot + 1) & (appArtLookupTableSize - 1);
        }
    }

    for (int i = 0; i < appItemCount; i++) {
        if (!strcmp(appsList[i].startup, startup))
            return &appsList[i];
    }

    return NULL;
}

int appGetArtMode(const char *startup)
{
    app_info_t *app = appLookupByStartup(startup);

    if (app == NULL)
        return -1;

    return app->artMode;
}

void appInit(item_list_t *itemList)
{
    LOG("APPSUPPORT Init\n");
    appForceUpdate = 1;
    appItemList.delay = gArtDelay;
    if (configApps != NULL)
        configFree(configApps);
    configApps = oplGetLegacyAppsConfig();
    appsList = NULL;
    appItemList.enabled = 1;
}

item_list_t *appGetObject(int initOnly)
{
    if (initOnly && !appItemList.enabled)
        return NULL;
    return &appItemList;
}

static int appNeedsUpdate(item_list_t *itemList)
{
    int update;

    update = 0;
    if (appForceUpdate) {
        appForceUpdate = 0;
        update = 1;
    }
    if (oplShouldAppsUpdate())
        update = 1;

    if (update) {
        if (configApps != NULL)
            configFree(configApps);
        configApps = oplGetLegacyAppsConfig();
    }

    return update;
}

static int addAppsLegacyList(struct app_info_linked **appsLinkedList)
{
    struct config_value_t *cur;
    struct app_info_linked *app;
    int count;

    configClear(configApps);
    configRead(configApps);

    count = 0;
    cur = configApps->head;
    while (cur != NULL) {
        if (*appsLinkedList == NULL) {
            app = malloc(sizeof(struct app_info_linked));
            if (app != NULL) {
                app->next = NULL;
                *appsLinkedList = app;
            }
        } else {
            app = malloc(sizeof(struct app_info_linked));
            if (app != NULL) {
                app->next = *appsLinkedList;
                *appsLinkedList = app;
            }
        }

        if (app == NULL) {
            LOG("APPSUPPORT unable to allocate memory.\n");
            break;
        }

        strncpy(app->app.title, cur->key, APP_TITLE_MAX + 1);
        app->app.title[APP_TITLE_MAX] = '\0';

        // Split the boot filename from the path.
        const char *elfname = appGetELFName(cur->val);
        if (elfname != cur->val) {
            strncpy(app->app.boot, elfname, APP_BOOT_MAX + 1);
            app->app.boot[APP_BOOT_MAX] = '\0';

            int pathlen = (int)(elfname - cur->val) - 1;
            if (cur->val[pathlen] == ':') // Discard only '/'.
                pathlen++;
            if (pathlen > APP_PATH_MAX)
                pathlen = APP_PATH_MAX;
            strncpy(app->app.path, cur->val, pathlen);
            app->app.path[pathlen] = '\0';
        } else {
            // Cannot split boot filename from the path, somehow.
            strncpy(app->app.boot, cur->val, APP_BOOT_MAX + 1);
            app->app.boot[APP_BOOT_MAX] = '\0';
            strncpy(app->app.path, cur->val, APP_PATH_MAX + 1);
            app->app.path[APP_PATH_MAX] = '\0';
        }

        app->app.legacy = 1;
        appSetResolvedStartup(&app->app);
        count++;
        cur = cur->next;
    }

    return count;
}

static int appScanCallback(const char *path, config_set_t *appConfig, void *arg)
{
    struct app_info_linked **appsLinkedList = (struct app_info_linked **)arg;
    struct app_info_linked *app;
    const char *title, *boot, *argv1;

    if (configGetStr(appConfig, APP_CONFIG_TITLE, &title) != 0 && configGetStr(appConfig, APP_CONFIG_BOOT, &boot) != 0) {
        if (*appsLinkedList == NULL) {
            app = malloc(sizeof(struct app_info_linked));
            if (app != NULL) {
                app->next = NULL;
                *appsLinkedList = app;
            }
        } else {
            app = malloc(sizeof(struct app_info_linked));
            if (app != NULL) {
                app->next = *appsLinkedList;
                *appsLinkedList = app;
            }
        }

        if (app == NULL) {
            LOG("APPSUPPORT unable to allocate memory.\n");
            return -1;
        }

        strncpy(app->app.title, title, APP_TITLE_MAX + 1);
        app->app.title[APP_TITLE_MAX] = '\0';
        strncpy(app->app.boot, boot, APP_BOOT_MAX + 1);
        app->app.boot[APP_BOOT_MAX] = '\0';
        strncpy(app->app.path, path, APP_PATH_MAX + 1);
        app->app.path[APP_PATH_MAX] = '\0';
        if (configGetStr(appConfig, APP_CONFIG_ARGV1, &argv1) != 0) {
            strncpy(app->app.argv1, argv1, APP_ARGV1_MAX + 1);
            app->app.argv1[APP_ARGV1_MAX] = '\0';
        } else
            app->app.argv1[0] = '\0';
        app->app.legacy = 0;
        appSetResolvedStartup(&app->app);
        return 0;
    } else {
        LOG("APPSUPPORT item has no boot/title.\n");
        return 1;
    }
}

static int appUpdateItemList(item_list_t *itemList)
{
    struct app_info_linked *appsLinkedList;
    struct app_info_linked *appsLinkedListHead;

    /* Drain any in-flight art worker requests that reference appsList /
     * appArtLookupTable before freeing them; without this the art worker
     * thread (gArtThreadId) can race against the free/realloc below via
     * appGetImage -> appLookupByStartup (use-after-free / data race). */
    cacheAbortMmceImageLoadsTimed(500);
    (void)cacheCancelPendingImageLoadsTimed(500);

    appFreeList();

    appsLinkedList = NULL;

    // Get legacy apps list first, so it is possible to use appGetConfigValue(id).
    appItemCount += addAppsLegacyList(&appsLinkedList);

    // Scan devices for apps.
    appItemCount += oplScanApps(&appScanCallback, &appsLinkedList);
    appsLinkedListHead = appsLinkedList;

    // Generate apps list
    if (appItemCount > 0) {
        appsList = malloc(appItemCount * sizeof(app_info_t));

        if (appsList != NULL) {
            int i;
            struct app_info_linked *app = appsLinkedList;

            for (i = 0; app != NULL; i++, app = app->next) // appsLinkedList contains items in reverse order.
                memcpy(&appsList[appItemCount - i - 1], &app->app, sizeof(app_info_t));

            // One APP discoverable through both channels (conf_apps.cfg + device-scanned title.cfg)
            // must appear ONCE (#253). Runs before the art table is built so its startup hashes map
            // to the deduped rows.
            appDedupList();
        } else {
            LOG("APPSUPPORT unable to allocate memory.\n");
            appItemCount = 0;
        }
    }

    // #253 diagnostics: with debug enabled, dump every entry's resolved identity so a
    // reporter's file answers "which two prefixes did this app come in through" without a
    // serial cable. Tiny text file at the config home, rewritten each rebuild -- including
    // the zero-app rebuild, or a stale dump would outlive the truth (CodeRabbit #262).
    if (gEnableDebug)
        appDumpDiscovery();

    if (appBuildArtLookup() < 0)
        LOG("APPSUPPORT unable to allocate art lookup table.\n");

    appFreeLinkedList(appsLinkedListHead);

    LOG("APPSUPPORT %d apps loaded\n", appItemCount);

    return appItemCount;
}

static void appFreeLinkedList(struct app_info_linked *appsLinkedList)
{
    while (appsLinkedList != NULL) {
        struct app_info_linked *appNext = appsLinkedList->next;
        free(appsLinkedList);
        appsLinkedList = appNext;
    }
}

static void appFreeList(void)
{
    appFreeArtLookup();

    if (appsList != NULL) {
        free(appsList);
        appsList = NULL;
    }

    appItemCount = 0;
}

static int appGetItemCount(item_list_t *itemList)
{
    return appItemCount;
}

static char *appGetItemName(item_list_t *itemList, int id)
{
    return appsList[id].title;
}

static int appGetItemNameLength(item_list_t *itemList, int id)
{
    return CONFIG_KEY_NAME_LEN;
}

/* appGetItemStartup() is called to get the startup path for display & for the art assets.
   The path is used immediately, before a subsequent call to appGetItemStartup(). */
static char *appGetItemStartup(item_list_t *itemList, int id)
{
    return appsList[id].startup;
}

static void appDeleteItem(item_list_t *itemList, int id)
{
    if (appsList[id].legacy) {
        struct config_value_t *cur = appGetConfigValue(id);
        unlink(cur->val);
        cur->key[0] = '\0';
        configApps->modified = 1;
        configWrite(configApps);
    } else {
        sysDeleteFolder(appsList[id].path);
    }

    appForceUpdate = 1;
}

static void appRenameItem(item_list_t *itemList, int id, char *newName)
{
    char value[256];

    if (appsList[id].legacy) {
        struct config_value_t *cur = appGetConfigValue(id);

        strncpy(value, cur->val, sizeof(value));
        configRemoveKey(configApps, cur->key);
        configSetStr(configApps, newName, value);
        configWrite(configApps);
    } else {
        config_set_t *appConfig;

        snprintf(value, sizeof(value), "%s/%s", appsList[id].path, APP_TITLE_CONFIG_FILE);

        appConfig = configAlloc(0, NULL, value);
        if (appConfig != NULL) {
            configRead(appConfig);
            configSetStr(appConfig, APP_CONFIG_TITLE, newName);
            configWrite(appConfig);

            configFree(appConfig);
        }
    }

    appForceUpdate = 1;
}

static void appLaunchItem(item_list_t *itemList, int id, config_set_t *configSet)
{
    int fd;
    char filename[256];
    const char *argv1;

    // Retrieve configuration set by appGetConfig()
    configGetStrCopy(configSet, CONFIG_ITEM_STARTUP, filename, sizeof(filename));

    // If no device number is specified use mass? to auto find device number
    const char *oldPrefix = "mass:";
    const char *newPrefix = "mass?:";

    if (strncmp(filename, oldPrefix, strlen(oldPrefix)) == 0) {
        size_t oldPrefixLen = strlen(oldPrefix);
        size_t newPrefixLen = strlen(newPrefix);
        // The expansion grows the string by (newPrefixLen - oldPrefixLen) bytes; only
        // shift if the result plus its NUL still fits, to avoid a stack overflow when
        // filename is already at its maximum length.
        if (strlen(filename) + (newPrefixLen - oldPrefixLen) < sizeof(filename)) {
            memmove(filename + newPrefixLen, filename + oldPrefixLen, strlen(filename) - oldPrefixLen + 1);
            memcpy(filename, newPrefix, newPrefixLen);
        }
    }

    // If legacy apps state mass? find the first connected mass device with the corresponding filename and set the unit number for launch.
    if (!strncmp("mass?", filename, 5)) {
        for (int i = 0; i < MAX_BDM_DEVICES; i++) {
            filename[4] = i + '0';
            fd = open(filename, O_RDONLY);
            if (fd >= 0) {
                close(fd);
                break;
            }
        }
    }

    fd = open(filename, O_RDONLY);
    if (fd >= 0) {
        int mode, argc = 0;
        char partition[128];
        char altStartup[256];
        char *argv[1];
        close(fd);

        strcpy(partition, "");

        // To keep the necessary device accessible, we will assume the mode that owns the device which contains the file to boot.
        mode = oplPath2Mode(filename);
        if (mode < 0)
            mode = APP_MODE; // Legacy apps mode on memory card (mc?:/*)

        if (mode == HDD_MODE)
            snprintf(partition, sizeof(partition), "%s:", gOPLPart);

        // POPSTARTER SMB via APPS: APPS -> SB.<game>.ELF
        // Run the same SMB environment preparation as ETH VCD.
        // Uses shared helper vcdPreparePopstarterSmbLaunch(ethPrefix).
        if (appIsPopstarterSmb(filename)) {
            vcd_popsnet_ensure_t ens = vcdPreparePopstarterSmbLaunch(ethGetSMBPrefix());
            if (ens == VCD_POPSNET_SMB_MISSING) {
                guiMsgBox(_l(_STR_POPSTARTER_SMB_MISSING), 0, NULL);
                return;
            }
            if (ens == VCD_POPSNET_NEED_STATIC) {
                guiMsgBox(_l(_STR_POPSTARTER_SMB_NEEDS_STATIC), 0, NULL);
                return;
            }
            if (ens == VCD_POPSNET_IO_ERROR || ens == VCD_POPSNET_INVALID) {
                guiMsgBox(_l(_STR_POPSTARTER_NET_ERR), 0, NULL);
                return;
            }
        }

        if (configGetStr(configSet, CONFIG_ITEM_ALTSTARTUP, &argv1) != 0) {
            // Copy before deinit(): argv1 points into the config heap which
            // deinit() -> configEnd() frees just below; passing it to the loader
            // afterward would be a use-after-free (A6).
            snprintf(altStartup, sizeof(altStartup), "%s", argv1);
            argv[0] = altStartup;
            argc = 1;
        }

        deinit(UNMOUNT_EXCEPTION, mode); // CAREFUL: deinit will call appCleanUp, which frees appsList, appArtLookupTable, and configApps
        LoadELFFromFileWithPartition(filename, partition, argc, argv);
    } else
        guiMsgBox(_l(_STR_ERR_FILE_INVALID), 0, NULL);
}

static config_set_t *appGetConfig(item_list_t *itemList, int id)
{
    config_set_t *config;
    char tmp[8];

    if (appsList[id].legacy) {
        struct config_value_t *cur = appGetConfigValue(id);
        config = oplGetLegacyAppsInfo(appGetELFName(cur->val));
        configRead(config);

        configSetStr(config, CONFIG_ITEM_NAME, appGetELFName(cur->val));
        configSetStr(config, CONFIG_ITEM_LONGNAME, cur->key);
        configSetStr(config, CONFIG_ITEM_STARTUP, cur->val);
        configSetStr(config, CONFIG_ITEM_MEDIA, "APP");
        configSetStr(config, CONFIG_ITEM_FORMAT, "ELF");

        if (sbConfigStatSizeEnabled())
            snprintf(tmp, sizeof(tmp), "%.2f", appGetELFSize(cur->val));
        else
            snprintf(tmp, sizeof(tmp), "0.00");
        configSetStr(config, CONFIG_ITEM_SIZE, tmp);
    } else {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", appsList[id].path, APP_TITLE_CONFIG_FILE);

        config = configAlloc(0, NULL, path);
        configRead(config); // Does not matter if the config file could be loaded or not.

        configSetStr(config, CONFIG_ITEM_NAME, appsList[id].boot);
        configSetStr(config, CONFIG_ITEM_LONGNAME, appsList[id].title);
        configSetStr(config, CONFIG_ITEM_ALTSTARTUP, appsList[id].argv1); // reuse AltStartup for argument 1
        snprintf(path, sizeof(path), "%s/%s", appsList[id].path, appsList[id].boot);
        configSetStr(config, CONFIG_ITEM_STARTUP, path);
        configSetStr(config, CONFIG_ITEM_MEDIA, "APP");
        configSetStr(config, CONFIG_ITEM_FORMAT, "ELF");

        if (sbConfigStatSizeEnabled())
            snprintf(tmp, sizeof(tmp), "%.2f", appGetELFSize(path));
        else
            snprintf(tmp, sizeof(tmp), "0.00");
        configSetStr(config, CONFIG_ITEM_SIZE, tmp);
    }
    return config;
}

static int appGetImage(item_list_t *itemList, char *folder, int isRelative, char *value, char *suffix, GSTEXTURE *resultTex, short psm)
{
    app_info_t *app;

    // Absolute (theme-folder) requests (#298): info-page attribute caches (Rating/Players/Vmode/Scan)
    // are created with an absolute prefix and key their request on the attribute VALUE ("4", "ntsc",
    // ...) -- never an app startup -- so the appLookupByStartup gate below must not run for them, or
    // texcache would memoize a bogus miss and the placeholder renders forever. `folder` is the theme
    // path; build it directly like hddGetImage does.
    if (!isRelative) {
        char path[256];
        snprintf(path, sizeof(path), "%s%s_%s", folder, value, suffix);
        return texDiscoverLoad(resultTex, path, -1);
    }

    app = appLookupByStartup(value);
    if (app == NULL || app->artMode < 0)
        return -1;

    if (!strcmp(folder, "ART"))
        return oplGetAppImageByMode(app->artMode, folder, isRelative, app->artLookup[0] != '\0' ? app->artLookup : app->boot, suffix, resultTex, psm);

    return oplGetAppImageByMode(app->artMode, folder, isRelative, value, suffix, resultTex, psm);
}

static int appGetTextId(item_list_t *itemList)
{
    return _STR_APPS;
}

static int appGetIconId(item_list_t *itemList)
{
    return APP_ICON;
}

// This may be called, even if appInit() was not.
static void appCleanUp(item_list_t *itemList, int exception)
{
    if (appItemList.enabled) {
        LOG("APPSUPPORT CleanUp\n");

        appFreeList();
        if (configApps != NULL) {
            configFree(configApps);
            configApps = NULL;
        }
    }
}

// This may be called, even if appInit() was not.
static void appShutdown(item_list_t *itemList)
{
    if (appItemList.enabled) {
        LOG("APPSUPPORT Shutdown\n");

        appFreeList();
        if (configApps != NULL) {
            configFree(configApps);
            configApps = NULL;
        }
    }
}

static item_list_t appItemList = {
    APP_MODE, -1, 0, MODE_FLAG_NO_COMPAT | MODE_FLAG_NO_UPDATE, MENU_MIN_INACTIVE_FRAMES, APP_MODE_UPDATE_DELAY, NULL, NULL, &appGetTextId, NULL, &appInit, &appNeedsUpdate, &appUpdateItemList,
    &appGetItemCount, NULL, &appGetItemName, &appGetItemNameLength, &appGetItemStartup, &appDeleteItem, &appRenameItem, &appLaunchItem,
    &appGetConfig, &appGetImage, &appCleanUp, &appShutdown, NULL, &appGetIconId};
