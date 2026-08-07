#include "include/opl.h"
#include "include/lang.h"
#include "include/util.h"
#include "include/iosupport.h"
#include "include/system.h"
#include "include/supportbase.h"
#include "include/ioman.h"
#include "modules/iopcore/common/cdvd_config.h"
#include "include/cheatman.h"
#include "include/pggsm.h"
#include "include/cheatman.h"
#include "include/ps2cnf.h"
#include "include/gui.h"

#define NEWLIB_PORT_AWARE
#include <fileXio_rpc.h> // fileXioMount("iso:", ***), fileXioUmount("iso:")
#include <io_common.h>   // FIO_MT_RDONLY
#include <ps2sdkapi.h>   // lseek64

#include "../modules/isofs/zso.h"

/// internal linked list used to populate the list from directory listing
struct game_list_t
{
    base_game_info_t gameinfo;
    struct game_list_t *next;
};

struct game_cache_list
{
    unsigned int count;
    base_game_info_t *games;
};

int sbIsSameSize(const char *prefix, int prevSize)
{
    int size = -1;
    char path[256];
    snprintf(path, sizeof(path), "%sul.cfg", prefix);

    int fd = openFile(path, O_RDONLY);
    if (fd >= 0) {
        size = getFileSize(fd);
        close(fd);
    }

    return size == prevSize;
}

int sbCreateSemaphore(void)
{
    ee_sema_t sema;

    sema.option = sema.attr = 0;
    sema.init_count = 1;
    sema.max_count = 1;
    return CreateSema(&sema);
}

// 0 = Not ISO disc image, GAME_FORMAT_OLD_ISO = legacy ISO disc image (filename follows old naming requirement), GAME_FORMAT_ISO = plain ISO image.
int isValidIsoName(char *name, int *pNameLen)
{
    // Old ISO image naming format: SCUS_XXX.XX.ABCDEFGHIJKLMNOP.iso

    // Minimum is 17 char, GameID (11) + "." (1) + filename (1 min.) + ".iso" (4)
    int size = strlen(name);
    if (strcasecmp(&name[size - 4], ".iso") == 0 || strcasecmp(&name[size - 4], ".zso") == 0) {
        if ((size >= 17) && (name[4] == '_') && (name[8] == '.') && (name[11] == '.')) {
            *pNameLen = size - 16;
            return GAME_FORMAT_OLD_ISO;
        } else if (size >= 5) {
            *pNameLen = size - 4;
            return GAME_FORMAT_ISO;
        }
    }

    return 0;
}

static int GetStartupExecName(const char *path, char *filename, int maxlength)
{
    char ps2disc_boot[CNF_PATH_LEN_MAX] = "";
    const char *key;
    int ret;

    if ((ret = ps2cnfGetBootFile(path, ps2disc_boot)) == 0) {
        int length = 0;
        const char *start;

        /* Skip the device name part of the path ("cdrom0:\"). */
        key = ps2disc_boot;

        for (; *key != ':'; key++) {
            if (*key == '\0') {
                LOG("GetStartupExecName: missing ':' (%s).\n", ps2disc_boot);
                return -1;
            }
        }

        ++key;
        while (*key == '\\') {
            key++;
        }

        start = key;

        while ((*key != ';') && (*key != '\0')) {
            length++;
            key++;
        }

        if (length > maxlength) {
            length = maxlength;
        }

        if (length == 0) {
            LOG("GetStartupExecName: serial len 0 ':' (%s).\n", ps2disc_boot);
            return -1;
        }

        strncpy(filename, start, length);
        filename[length] = '\0';
        LOG("GetStartupExecName: serial len %d %s \n", length, filename);

        return 0;
    } else {
        LOG("GetStartupExecName: Could not get BOOT2 parameter.\n");
        return ret;
    }
}

static void freeISOGameListCache(struct game_cache_list *cache);

static int loadISOGameListCache(const char *path, struct game_cache_list *cache)
{
    char filename[256];
    FILE *file;
    base_game_info_t *games;
    int result, size, count;

    freeISOGameListCache(cache);

    sprintf(filename, "%s/games.bin", path);
    file = fopen(filename, "rb");
    if (file != NULL) {
        fseek(file, 0, SEEK_END);
        size = ftell(file);
        rewind(file);

        count = size / sizeof(base_game_info_t);
        if (count > 0) {
            games = memalign(64, count * sizeof(base_game_info_t));
            if (games != NULL) {
                if (fread(games, sizeof(base_game_info_t), count, file) == count) {
                    LOG("loadISOGameListCache: %d games loaded.\n", count);
                    cache->count = count;
                    cache->games = games;
                    result = 0;
                } else {
                    LOG("loadISOGameListCache: I/O error.\n");
                    free(games);
                    result = EIO;
                }
            } else {
                LOG("loadISOGameListCache: failed to allocate memory.\n");
                result = ENOMEM;
            }
        } else {
            result = -1; // Empty file (should not happen)
        }

        fclose(file);
    } else {
        result = ENOENT;
    }

    return result;
}

static void freeISOGameListCache(struct game_cache_list *cache)
{
    if (cache->games != NULL) {
        free(cache->games);
        cache->games = NULL;
        cache->count = 0;
    }
}

static int updateISOGameList(const char *path, const struct game_cache_list *cache, const struct game_list_t *head, int count)
{
    char filename[256];
    FILE *file;
    const struct game_list_t *game;
    int result, i, j, modified;
    base_game_info_t *list;

    modified = 0;
    if (cache != NULL) {
        if ((head != NULL) && (count > 0)) {
            game = head;

            for (i = 0; i < count; i++) {
                for (j = 0; j < cache->count; j++) {
                    if (strncmp(cache->games[i].name, game->gameinfo.name, ISO_GAME_NAME_MAX + 1) == 0 && strncmp(cache->games[i].extension, game->gameinfo.extension, ISO_GAME_EXTENSION_MAX + 1) == 0)
                        break;
                }

                if (j == cache->count) {
                    LOG("updateISOGameList: game added.\n");
                    modified = 1;
                    break;
                }

                game = game->next;
            }

            if ((!modified) && (count != cache->count)) {
                LOG("updateISOGameList: game removed.\n");
                modified = 1;
            }
        } else {
            modified = 0;
        }
    } else {
        modified = ((head != NULL) && (count > 0)) ? 1 : 0;
    }

    if (!modified)
        return 0;
    LOG("updateISOGameList: caching new game list.\n");

    result = 0;
    sprintf(filename, "%s/games.bin", path);
    if ((head != NULL) && (count > 0)) {
        list = (base_game_info_t *)memalign(64, sizeof(base_game_info_t) * count);

        if (list != NULL) {
            // Convert the linked list into a flat array, for writing performance.
            game = head;
            for (i = 0; (i < count) && (game != NULL); i++, game = game->next) {
                // copy one game, advance
                memcpy(&list[i], &game->gameinfo, sizeof(base_game_info_t));
            }

            file = fopen(filename, "wb");
            if (file != NULL) {
                result = fwrite(list, sizeof(base_game_info_t), count, file) == count ? 0 : EIO;

                fclose(file);

                if (result != 0)
                    remove(filename);
            } else
                result = EIO;

            free(list);
        } else
            result = ENOMEM;
    } else {
        // Last game deleted.
        remove(filename);
    }

    return result;
}

// Queries for the game entry, based on filename. Only the new filename format is supported (filename.ext).
static int queryISOGameListCache(const struct game_cache_list *cache, base_game_info_t *ginfo, const char *filename)
{
    char isoname[ISO_GAME_FNAME_MAX + 1];
    int i;

    for (i = 0; i < cache->count; i++) {
        snprintf(isoname, sizeof(isoname), "%s%s", cache->games[i].name, cache->games[i].extension);

        if (strcmp(filename, isoname) == 0) {
            memcpy(ginfo, &cache->games[i], sizeof(base_game_info_t));
            return 0;
        }
    }

    return ENOENT;
}

static int scanForISO(char *path, char type, struct game_list_t **glist)
{
    int count = 0;
    struct game_cache_list cache = {0, NULL};
    base_game_info_t cachedGInfo;
    char fullpath[256];
    struct dirent *dirent;
    DIR *dir;

    int cacheLoaded = loadISOGameListCache(path, &cache) == 0;

    if ((dir = opendir(path)) != NULL) {
        size_t base_path_len = strlen(path);
        strcpy(fullpath, path);
        fullpath[base_path_len] = '/';

        while ((dirent = readdir(dir)) != NULL) {
            int NameLen;
            int format = isValidIsoName(dirent->d_name, &NameLen);

            if (format <= 0 || NameLen > ISO_GAME_NAME_MAX)
                continue; // Skip files that cannot be supported properly.

            strcpy(fullpath + base_path_len + 1, dirent->d_name);

            struct game_list_t *next = malloc(sizeof(struct game_list_t));
            if (!next)
                break; // Out of memory

            next->next = *glist;
            *glist = next;
            base_game_info_t *game = &next->gameinfo;
            memset(game, 0, sizeof(base_game_info_t));

            if (format == GAME_FORMAT_OLD_ISO) {
                // old iso format can't be cached
                strncpy(game->name, &dirent->d_name[GAME_STARTUP_MAX], NameLen);
                game->name[NameLen] = '\0';
                strncpy(game->startup, dirent->d_name, GAME_STARTUP_MAX - 1);
                game->startup[GAME_STARTUP_MAX - 1] = '\0';
                strncpy(game->extension, &dirent->d_name[GAME_STARTUP_MAX + NameLen], sizeof(game->extension) - 1);
                game->extension[sizeof(game->extension) - 1] = '\0';
            } else if (cacheLoaded && queryISOGameListCache(&cache, &cachedGInfo, dirent->d_name) == 0) {
                // use cached entry
                memcpy(game, &cachedGInfo, sizeof(base_game_info_t));
            } else {
                // need to mount and read SYSTEM.CNF
                char startup[GAME_STARTUP_MAX];
                int MountFD = fileXioMount("iso:", fullpath, FIO_MT_RDONLY);

                if (MountFD < 0 || GetStartupExecName("iso:/SYSTEM.CNF;1", startup, GAME_STARTUP_MAX - 1) != 0) {
                    fileXioUmount("iso:");
                    *glist = next->next;
                    free(next);
                    continue;
                }

                strcpy(game->startup, startup);
                strncpy(game->name, dirent->d_name, NameLen);
                game->name[NameLen] = '\0';
                strncpy(game->extension, &dirent->d_name[NameLen], sizeof(game->extension) - 1);
                game->extension[sizeof(game->extension) - 1] = '\0';

                fileXioUmount("iso:");
            }

            game->parts = 1;
            game->media = type;
            game->format = format;
            game->sizeMB = 0;

            count++;
        }
        closedir(dir);
    }

    if (cacheLoaded) {
        updateISOGameList(path, &cache, *glist, count);
        freeISOGameListCache(&cache);
    } else {
        updateISOGameList(path, NULL, *glist, count);
    }

    return count;
}

int sbReadList(base_game_info_t **list, const char *prefix, int *fsize, int *gamecount)
{
    int fd, size, id = 0, result;
    int count;
    char path[256];

    free(*list);
    *list = NULL;
    *fsize = -1;
    *gamecount = 0;

    // temporary storage for the game names
    struct game_list_t *dlist_head = NULL;

    // count iso games in "cd" directory
    snprintf(path, sizeof(path), "%sCD", prefix);
    count = scanForISO(path, SCECdPS2CD, &dlist_head);

    // count iso games in "dvd" directory
    snprintf(path, sizeof(path), "%sDVD", prefix);
    if ((result = scanForISO(path, SCECdPS2DVD, &dlist_head)) >= 0) {
        count = count < 0 ? result : count + result;
    }

    // count and process games in ul.cfg
    snprintf(path, sizeof(path), "%sul.cfg", prefix);
    fd = openFile(path, O_RDONLY);
    if (fd >= 0) {
        USBExtreme_game_entry_t GameEntry;

        if (count < 0)
            count = 0;
        size = getFileSize(fd);
        *fsize = size;
        count += size / sizeof(USBExtreme_game_entry_t);

        if (count > 0) {
            if ((*list = (base_game_info_t *)malloc(sizeof(base_game_info_t) * count)) != NULL) {
                memset(*list, 0, sizeof(base_game_info_t) * count);

                while (size > 0) {
                    base_game_info_t *g = &(*list)[id++];

                    // populate game entry in list even if entry corrupted
                    read(fd, &GameEntry, sizeof(USBExtreme_game_entry_t));
                    size -= sizeof(USBExtreme_game_entry_t);

                    // to ensure no leaks happen, we copy manually and pad the strings
                    memcpy(g->name, GameEntry.name, UL_GAME_NAME_MAX);
                    g->name[UL_GAME_NAME_MAX] = '\0';
                    memcpy(g->startup, GameEntry.startup, GAME_STARTUP_MAX);
                    g->startup[GAME_STARTUP_MAX] = '\0';
                    g->extension[0] = '\0';
                    g->parts = GameEntry.parts;
                    g->media = GameEntry.media;
                    g->format = GAME_FORMAT_USBLD;
                    g->sizeMB = 0;

                    /* TODO: size calculation is very slow
                    implmented some caching, or do not touch at all */

                    // calculate total size for individual game
                    /*int ulfd = 1;
                    u8 part;
                    unsigned int name_checksum = USBA_crc32(g->name);

                    for (part = 0; part < g->parts && ulfd >= 0; part++) {
                        snprintf(path, sizeof(path), "%sul.%08X.%s.%02x", prefix, name_checksum, g->startup, part);
                        ulfd = openFile(path, O_RDONLY);
                        if (ulfd >= 0) {
                            g->sizeMB += (getFileSize(ulfd) >> 20);
                            close(ulfd);
                        }
                    }*/
                }
            }
        }
        close(fd);
    } else if (count > 0) {
        *list = (base_game_info_t *)malloc(sizeof(base_game_info_t) * count);
    }

    if (*list != NULL) {
        // copy the dlist into the list
        while ((id < count) && dlist_head) {
            // copy one game, advance
            struct game_list_t *cur = dlist_head;
            dlist_head = dlist_head->next;

            memcpy(&(*list)[id++], &cur->gameinfo, sizeof(base_game_info_t));
            free(cur);
        }
    } else
        count = 0;

    if (count > 0)
        *gamecount = count;

    return count;
}

extern int probed_fd;
extern u32 probed_lba;
extern u8 IOBuffer[2048];

static int ProbeZISO(int fd)
{
    struct
    {
        ZISO_header header;
        u32 first_block;
    } ziso_data;
    lseek(fd, 0, SEEK_SET);
    if (read(fd, &ziso_data, sizeof(ziso_data)) == sizeof(ziso_data) && ziso_data.header.magic == ZSO_MAGIC) {
        // initialize ZSO
        ziso_init(&ziso_data.header, ziso_data.first_block);
        // set ISO file descriptor for ZSO reader
        probed_fd = fd;
        probed_lba = 0;
        return 1;
    } else {
        return 0;
    }
}

u32 sbGetISO9660MaxLBA(const char *path)
{
    u32 maxLBA;
    int file;

    if ((file = open(path, O_RDONLY, 0666)) >= 0) {
        if (ProbeZISO(file)) {
            if (ziso_read_sector(IOBuffer, 16, 1) == 1) {
                maxLBA = *(u32 *)(IOBuffer + 80);
            } else {
                maxLBA = 0;
            }
        } else {
            lseek(file, 16 * 2048 + 80, SEEK_SET);
            if (read(file, &maxLBA, sizeof(maxLBA)) != sizeof(maxLBA))
                maxLBA = 0;
        }
        close(file);
    } else {
        maxLBA = 0;
    }

    return maxLBA;
}

int sbProbeISO9660(const char *path, base_game_info_t *game, u32 layer1_offset)
{
    int result = -1, fd;
    char buffer[6];

    result = -1;
    if (game->media == SCECdPS2DVD) { // Only DVDs can have multiple layers.
        if ((fd = open(path, O_RDONLY, 0666)) >= 0) {
            if (ProbeZISO(fd)) {
                if (ziso_read_sector(IOBuffer, layer1_offset, 1) == 1 &&
                    ((IOBuffer[0x00] == 1) && (!strncmp((char *)(&IOBuffer[0x01]), "CD001", 5)))) {
                    result = 0;
                }
            } else {
                if (lseek64(fd, (u64)layer1_offset * 2048, SEEK_SET) == (u64)layer1_offset * 2048) {
                    if ((read(fd, buffer, sizeof(buffer)) == sizeof(buffer)) &&
                        ((buffer[0x00] == 1) && (!strncmp(&buffer[0x01], "CD001", 5)))) {
                        result = 0;
                    }
                }
            }
            close(fd);
        } else
            result = fd;
    }

    return result;
}

static const struct cdvdman_settings_common cdvdman_settings_common_sample = CDVDMAN_SETTINGS_DEFAULT_COMMON;

int sbPrepare(base_game_info_t *game, config_set_t *configSet, int size_cdvdman, void **cdvdman_irx, int *patchindex)
{
    int i;
    struct cdvdman_settings_common *settings;

    int compatmask = 0;
    configGetInt(configSet, CONFIG_ITEM_COMPAT, &compatmask);

    char gameid[5];
    configGetDiscIDBinary(configSet, gameid);

    for (i = 0, settings = NULL; i < size_cdvdman; i += 4) {
        if (!memcmp((void *)((u8 *)cdvdman_irx + i), &cdvdman_settings_common_sample, sizeof(cdvdman_settings_common_sample))) {
            settings = (struct cdvdman_settings_common *)((u8 *)cdvdman_irx + i);
            break;
        }
    }
    if (settings == NULL) {
        LOG("sbPrepare: unable to locate patch zone.\n");
        return -1;
    }

    if (game != NULL) {
        settings->NumParts = game->parts;
        settings->media = game->media;
    }
    settings->flags = 0;

    if (compatmask & COMPAT_MODE_1) {
        settings->flags |= IOPCORE_COMPAT_ACCU_READS;
    }

    if (compatmask & COMPAT_MODE_2) {
        settings->flags |= IOPCORE_COMPAT_ALT_READ;
    }

    if (compatmask & COMPAT_MODE_4) {
        settings->flags |= IOPCORE_COMPAT_0_SKIP_VIDEOS;
    }

    if (compatmask & COMPAT_MODE_5) {
        settings->flags |= IOPCORE_COMPAT_EMU_DVDDL;
    }

    if (compatmask & COMPAT_MODE_6) {
        settings->flags |= IOPCORE_ENABLE_POFF;
    }

    settings->fakemodule_flags = 0;
    settings->fakemodule_flags |= FAKE_MODULE_FLAG_CDVDFSV;
    settings->fakemodule_flags |= FAKE_MODULE_FLAG_CDVDSTM;

    InitGSMConfig(configSet);

    InitCheatsConfig(configSet);

    config_set_t *configGame = configGetByType(CONFIG_GAME);

#ifdef PADEMU
    gPadEmuSource = 0;
    gEnablePadEmu = 0;
    gPadEmuSettings = 0;
    gPadMacroSource = 0;
    gPadMacroSettings = 0;

    if (configGetInt(configSet, CONFIG_ITEM_PADEMUSOURCE, &gPadEmuSource)) {
        configGetInt(configSet, CONFIG_ITEM_ENABLEPADEMU, &gEnablePadEmu);
        configGetInt(configSet, CONFIG_ITEM_PADEMUSETTINGS, &gPadEmuSettings);
    } else {
        configGetInt(configGame, CONFIG_ITEM_ENABLEPADEMU, &gEnablePadEmu);
        configGetInt(configGame, CONFIG_ITEM_PADEMUSETTINGS, &gPadEmuSettings);
    }

    if (configGetInt(configSet, CONFIG_ITEM_PADMACROSOURCE, &gPadMacroSource)) {
        configGetInt(configSet, CONFIG_ITEM_PADMACROSETTINGS, &gPadMacroSettings);
    } else {
        configGetInt(configGame, CONFIG_ITEM_PADMACROSETTINGS, &gPadMacroSettings);
    }

    if (gEnablePadEmu) {
        settings->fakemodule_flags |= FAKE_MODULE_FLAG_USBD;
    }
#endif
    // sanitise the settings
    gOSDLanguageSource = 0;
    gOSDLanguageEnable = 0;
    gOSDLanguageValue = 0;
    gOSDTVAspectRatio = 0;
    gOSDVideOutput = 0;

    if (configGetInt(configSet, CONFIG_ITEM_OSD_SETTINGS_SOURCE, &gOSDLanguageSource)) {
        configGetInt(configSet, CONFIG_ITEM_OSD_SETTINGS_ENABLE, &gOSDLanguageEnable);
        configGetInt(configSet, CONFIG_ITEM_OSD_SETTINGS_LANGID, &gOSDLanguageValue);
        configGetInt(configSet, CONFIG_ITEM_OSD_SETTINGS_TV_ASP, &gOSDTVAspectRatio);
        configGetInt(configSet, CONFIG_ITEM_OSD_SETTINGS_VMODE, &gOSDVideOutput);
    } else {
        configGetInt(configGame, CONFIG_ITEM_OSD_SETTINGS_ENABLE, &gOSDLanguageEnable);
        configGetInt(configGame, CONFIG_ITEM_OSD_SETTINGS_LANGID, &gOSDLanguageValue);
        configGetInt(configGame, CONFIG_ITEM_OSD_SETTINGS_TV_ASP, &gOSDTVAspectRatio);
        configGetInt(configGame, CONFIG_ITEM_OSD_SETTINGS_VMODE, &gOSDVideOutput);
    }

    *patchindex = i;

    // game id
    memcpy(settings->DiscID, gameid, sizeof(settings->DiscID));

    return compatmask;
}

void sbUnprepare(void *pCommon)
{
    memcpy(pCommon, &cdvdman_settings_common_sample, sizeof(struct cdvdman_settings_common));
}

void sbRebuildULCfg(base_game_info_t **list, const char *prefix, int gamecount, int excludeID)
{
    char path[256];
    USBExtreme_game_entry_t GameEntry;
    snprintf(path, sizeof(path), "%sul.cfg", prefix);

    FILE *file = fopen(path, "wb");
    if (file != NULL) {
        int i;
        base_game_info_t *game;

        memset(&GameEntry, 0, sizeof(GameEntry));
        GameEntry.Byte08 = 0x08; // just to be compatible with original ul.cfg
        memcpy(GameEntry.magic, "ul.", 3);

        for (i = 0; i < gamecount; i++) {
            game = &(*list)[i];

            if (game->format == GAME_FORMAT_USBLD && (i != excludeID)) {
                memcpy(GameEntry.startup, game->startup, GAME_STARTUP_MAX);
                memcpy(GameEntry.name, game->name, UL_GAME_NAME_MAX);
                // don't fill last symbol with zero, cause trailing symbol can be useful character
                GameEntry.parts = game->parts;
                GameEntry.media = game->media;

                fwrite(&GameEntry, sizeof(GameEntry), 1, file);
            }
        }

        fclose(file);
    }
}

static void sbCreatePath_name(const base_game_info_t *game, char *path, const char *prefix, const char *sep, int part, const char *game_name)
{
    switch (game->format) {
        case GAME_FORMAT_USBLD:
            snprintf(path, 256, "%sul.%08X.%s.%02x", prefix, USBA_crc32(game_name), game->startup, part);
            break;
        case GAME_FORMAT_ISO:
            snprintf(path, 256, "%s%s%s%s%s", prefix, (game->media == SCECdPS2CD) ? "CD" : "DVD", sep, game_name, game->extension);
            break;
        case GAME_FORMAT_OLD_ISO:
            snprintf(path, 256, "%s%s%s%s.%s%s", prefix, (game->media == SCECdPS2CD) ? "CD" : "DVD", sep, game->startup, game_name, game->extension);
            break;
    }
}

void sbCreatePath(const base_game_info_t *game, char *path, const char *prefix, const char *sep, int part)
{
    sbCreatePath_name(game, path, prefix, sep, part, game->name);
}

void sbDelete(base_game_info_t **list, const char *prefix, const char *sep, int gamecount, int id)
{
    int part;
    char path[256];
    base_game_info_t *game = &(*list)[id];

    for (part = 0; part < game->parts; part++) {
        sbCreatePath(game, path, prefix, sep, part);
        unlink(path);
    }

    if (game->format == GAME_FORMAT_USBLD) {
        sbRebuildULCfg(list, prefix, gamecount, id);
    }
}

void sbRename(base_game_info_t **list, const char *prefix, const char *sep, int gamecount, int id, char *newname)
{
    int part;
    char oldpath[256], newpath[256];
    base_game_info_t *game = &(*list)[id];

    for (part = 0; part < game->parts; part++) {
        sbCreatePath_name(game, oldpath, prefix, sep, part, game->name);
        sbCreatePath_name(game, newpath, prefix, sep, part, newname);
        rename(oldpath, newpath);
    }

    if (game->format == GAME_FORMAT_USBLD) {
        memset(game->name, 0, UL_GAME_NAME_MAX + 1);
        memcpy(game->name, newname, UL_GAME_NAME_MAX);
        sbRebuildULCfg(list, prefix, gamecount, -1);
    }
}

config_set_t *sbPopulateConfig(base_game_info_t *game, const char *prefix, const char *sep)
{
    char path[256];
    struct stat st;

    snprintf(path, sizeof(path), "%sCFG%s%s.cfg", prefix, sep, game->startup);
    config_set_t *config = configAlloc(0, NULL, path);
    configRead(config); // Does not matter if the config file could be loaded or not.

    // Get game size if not already set
    if (game->sizeMB == 0) {
        char gamepath[256];

        if (game->format == GAME_FORMAT_ISO) {
            snprintf(gamepath, sizeof(gamepath), "%s%s%s%s%s%s", prefix, sep, game->media == SCECdPS2CD ? "CD" : "DVD", sep, game->name, game->extension);

            if (stat(gamepath, &st) == 0)
                game->sizeMB = st.st_size >> 20;
        } else if (game->format == GAME_FORMAT_OLD_ISO) {
            snprintf(gamepath, sizeof(gamepath), "%s%s%s%s%s.%s%s", prefix, sep, game->media == SCECdPS2CD ? "CD" : "DVD", sep, game->startup, game->name, game->extension);

            if (stat(gamepath, &st) == 0)
                game->sizeMB = st.st_size >> 20;
        } else if (game->format == GAME_FORMAT_USBLD) {
            // Calculate total size for multi-part USBLD games
            int part;
            unsigned int name_checksum = USBA_crc32(game->name);

            for (part = 0; part < game->parts; part++) {
                snprintf(gamepath, sizeof(gamepath), "%sul.%08X.%s.%02x", prefix, name_checksum, game->startup, part);
                if (stat(gamepath, &st) == 0)
                    game->sizeMB += (st.st_size >> 20);
            }
        }
    }

    configSetStr(config, CONFIG_ITEM_NAME, game->name);
    configSetInt(config, CONFIG_ITEM_SIZE, game->sizeMB);

    if (game->format != GAME_FORMAT_USBLD) {
        if (!strcmp(game->extension, ".iso"))
            configSetStr(config, CONFIG_ITEM_FORMAT, "ISO");
        else if (!strcmp(game->extension, ".zso"))
            configSetStr(config, CONFIG_ITEM_FORMAT, "ZSO");
    } else if (game->format == GAME_FORMAT_USBLD)
        configSetStr(config, CONFIG_ITEM_FORMAT, "UL");

    configSetStr(config, CONFIG_ITEM_MEDIA, game->media == SCECdPS2CD ? "CD" : "DVD");

    configSetStr(config, CONFIG_ITEM_STARTUP, game->startup);

    return config;
}

static void sbCreateFoldersFromList(const char *path, const char **folders)
{
    int i;
    char fullpath[256];

    for (i = 0; folders[i] != NULL; i++) {
        sprintf(fullpath, "%s%s", path, folders[i]);
        mkdir(fullpath, 0777);
    }
}

void sbCreateFolders(const char *path, int createDiscImgFolders)
{
    const char *basicFolders[] = {"CFG", "THM", "LNG", "ART", "VMC", "CHT", "APPS", NULL};
    const char *discImgFolders[] = {"CD", "DVD", NULL};

    sbCreateFoldersFromList(path, basicFolders);

    if (createDiscImgFolders)
        sbCreateFoldersFromList(path, discImgFolders);
}

int sbLoadCheats(const char *path, const char *file)
{
    char cheatfile[64];
    int cheatMode = 0;

    if (GetCheatsEnabled()) {
        snprintf(cheatfile, sizeof(cheatfile), "%sCHT/%s.cht", path, file);
        LOG("Loading Cheat File %s\n", cheatfile);

        if ((cheatMode = load_cheats(cheatfile)) < 0)
            LOG("Error: failed to load cheats\n");
        else {
            LOG("Cheats found\n");
            if ((gAutoLaunchGame == NULL) && (gAutoLaunchBDMGame == NULL) && (cheatMode == 1))
                guiManageCheats();
        }
    }

    return cheatMode;
}

// Δ1 (NHDDL-parity): a resolved neutrino.elf is only USABLE if its install is complete. Neutrino
// chdir()s to the elf's dir (our -cwd) then opens config/system.toml; if absent it falls back to a
// FLAT "system.toml" (SAS layout); if NEITHER loads it returns -1 = black screen post-teardown
// (neutrino ee/loader/src/main.c:486-505). Mirror that EXACT detection so we never reject a valid
// install: accept the elf only when config/system.toml OR flat system.toml sits beside it. A stale
// folder (elf only, no config) is skipped so probing continues to a good install instead of
// shadowing it. Uses its own buffer so the caller's returned path is untouched.
static int sbNeutrinoInstallComplete(const char *elfPath)
{
    if (elfPath == NULL)
        return 0;
    const char *slash = strrchr(elfPath, '/');
    char probe[320]; // longest caller path (~160) + "/config/system.toml" with headroom -- a TRUNCATED
                     // probe would miss a real toml and reject a VALID install (PR #81 review)
    int dirLen;

    if (slash == NULL) // no directory component -- can't derive cwd; don't reject (custom path edge)
        return 1;
    dirLen = (int)(slash - elfPath);

    snprintf(probe, sizeof(probe), "%.*s/config/system.toml", dirLen, elfPath);
    if (sbFileExists(probe))
        return 1;
    snprintf(probe, sizeof(probe), "%.*s/system.toml", dirLen, elfPath);
    return sbFileExists(probe);
}

// Δ5 (NHDDL-parity visibility): a stale/old neutrino folder winning silently was undiagnosable.
// LOG the resolved pick + its version.txt (if present, like NHDDL's splash) at the single return
// point. Returns the path unchanged so call sites read `return sbNeutrinoResolved(path)`.
static const char *sbNeutrinoResolved(const char *path)
{
    if (path == NULL)
        return NULL;
    const char *slash = strrchr(path, '/');
    if (slash != NULL) {
        char vpath[320], ver[128]; // same headroom rationale as sbNeutrinoInstallComplete's probe
        int fd;
        snprintf(vpath, sizeof(vpath), "%.*s/version.txt", (int)(slash - path), path);
        fd = open(vpath, O_RDONLY, 0666);
        if (fd >= 0) {
            int n = read(fd, ver, sizeof(ver) - 1);
            close(fd);
            if (n > 0) {
                char *nl;
                ver[n] = '\0';
                nl = strpbrk(ver, "\r\n");
                if (nl != NULL)
                    *nl = '\0';
                LOG("[NEUTRINO] using %s (%s)\n", path, ver);
                return path;
            }
        }
    }
    LOG("[NEUTRINO] using %s\n", path);
    return path;
}

// Probe the ACTIVE game device for a co-located neutrino.elf: (A) the games-folder prefix, then
// (B) the bare device root. Returns the resolved path of the first COMPLETE install (Δ1), or NULL.
// Shared by the AUTO tier (one candidate among several) and the "Game's Device" pick (the ONLY
// candidate -- a NULL here is surfaced to the user, no MC fallback). activePrefix NULL/"" -> NULL
// (e.g. HDD passes NULL: raw APA is not POSIX-open()-reachable).
static const char *sbNeutrinoProbeGameDevice(const char *activePrefix)
{
    if (activePrefix == NULL || activePrefix[0] == '\0')
        return NULL;

    char devRoot[64]; // bare device-root token, e.g. "mass0:" (everything up to & incl. ':')
    const char *colon = strchr(activePrefix, ':');
    size_t rootLen = (colon != NULL) ? (size_t)(colon - activePrefix + 1) : 0;
    if (rootLen > 0 && rootLen < sizeof(devRoot)) {
        memcpy(devRoot, activePrefix, rootLen);
        devRoot[rootLen] = '\0';
    } else {
        devRoot[0] = '\0';
    }
    const char *bases[2] = {activePrefix, devRoot}; // (A) the games-folder prefix, (B) the device root
    static const char *forms[] = {
        "%sNEUTRINO/neutrino.elf",
        "%sneutrino/neutrino.elf",
        "%sNEUTRINO/NEUTRINO.ELF",
        "%sneutrino/NEUTRINO.ELF",
    };
    static char probe[160];
    for (int b = 0; b < 2; b++) {
        if (bases[b] == NULL || bases[b][0] == '\0')
            continue;
        for (int i = 0; i < (int)(sizeof(forms) / sizeof(forms[0])); i++) {
            snprintf(probe, sizeof(probe), forms[i], bases[b]);
            // Δ1: a stale elf-only folder on the game device must NOT count as a valid install.
            if (sbFileExists(probe) && sbNeutrinoInstallComplete(probe))
                return sbNeutrinoResolved(probe);
        }
    }
    return NULL;
}

// ---- Neutrino launch-args parse / assemble (the "Launch Args" picker) ----------------
// naAppend joins a token onto out with a single separating space; naAppendKV joins "<key><val>".
static void naAppend(char *out, int outSize, const char *tok)
{
    int len = (int)strlen(out);
    if (len >= outSize - 1) // already full (or malformed) -- nothing more fits
        return;
    if (len > 0)
        out[len++] = ' ';
    snprintf(out + len, outSize - len, "%s", tok);
}

static void naAppendKV(char *out, int outSize, const char *key, const char *val)
{
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s%s", key, val);
    naAppend(out, outSize, tmp);
}

int sbFileExists(const char *path)
{
    if (path == NULL)
        return 0;
    int fd = open(path, O_RDONLY, 0666);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

// Resolve the Neutrino core ELF: probe the install locations users actually use (folder-case
// and leading-slash variants on mc0/mc1) and return the first COMPLETE install (Δ1), or NULL.
// Centralised so the bdm + mmce launch paths stay in sync.
const char *sbResolveNeutrinoPath(const char *activePrefix)
{
    // Neutrino Device (General Settings): a driver-accurate device TYPE (NEUTRINO_DEV_*) that holds
    // <root>:/neutrino/neutrino.elf. Resolve the type to its live device-name token(s) -- USB/MX4SIO/
    // exFAT-HDD via the mounted BDM device, MC/MMCE by slot, APA-HDD via the mounted OPL data partition
    // (pfs0:) -- then probe each first. The token has NO trailing ':' so the forms[] below add it.
    // GAME'S DEVICE: resolve ONLY on the active game's own device (co-located neutrino.elf); no
    // legacy-custom-path / MC fallback. A miss returns NULL so the launch path toasts "not found"
    // and aborts in a live menu (every caller handles NULL that way) rather than silently using an
    // MC core the user did not pick.
    if (gNeutrinoDevice == NEUTRINO_DEV_GAME)
        return sbNeutrinoProbeGameDevice(activePrefix);

    char cand[2][BDM_DEVICE_ROOT_MAX];
    int nCand = 0;
    switch (gNeutrinoDevice) {
        case NEUTRINO_DEV_MC:
            snprintf(cand[nCand++], BDM_DEVICE_ROOT_MAX, "mc0");
            snprintf(cand[nCand++], BDM_DEVICE_ROOT_MAX, "mc1");
            break;
        case NEUTRINO_DEV_MMCE:
            snprintf(cand[nCand++], BDM_DEVICE_ROOT_MAX, "mmce0");
            snprintf(cand[nCand++], BDM_DEVICE_ROOT_MAX, "mmce1");
            break;
        case NEUTRINO_DEV_USB:
        case NEUTRINO_DEV_MX4SIO:
        case NEUTRINO_DEV_EXFAT_HDD: {
            int bt = BDM_TYPE_ATA; // NEUTRINO_DEV_EXFAT_HDD
            if (gNeutrinoDevice == NEUTRINO_DEV_USB)
                bt = BDM_TYPE_USB;
            else if (gNeutrinoDevice == NEUTRINO_DEV_MX4SIO)
                bt = BDM_TYPE_SDC;
            char bdmRoot[BDM_DEVICE_ROOT_MAX];
            if (bdmGetDeviceRootByType(bt, bdmRoot, sizeof(bdmRoot))) {
                char *colon = strchr(bdmRoot, ':'); // "massN:/" -> bare "massN" token
                if (colon != NULL)
                    *colon = '\0';
                snprintf(cand[nCand++], BDM_DEVICE_ROOT_MAX, "%s", bdmRoot);
            }
            break;
        }
        case NEUTRINO_DEV_APA_HDD:
            snprintf(cand[nCand++], BDM_DEVICE_ROOT_MAX, "pfs0"); // the already-mounted OPL data partition
            break;
        default: // AUTO -- no explicit device root
            break;
    }
    if (nCand > 0) {
        static const char *forms[] = {
            "%s:NEUTRINO/neutrino.elf",
            "%s:/neutrino/neutrino.elf",
            "%s:/NEUTRINO/neutrino.elf",
            "%s:/neutrino/NEUTRINO.ELF",
            "%s:/NEUTRINO/NEUTRINO.ELF",
            "%s:NEUTRINO/NEUTRINO.ELF",
        };
        static char built[64];
        for (int c = 0; c < nCand; c++) {
            for (int i = 0; i < (int)(sizeof(forms) / sizeof(forms[0])); i++) {
                snprintf(built, sizeof(built), forms[i], cand[c]);
                if (sbFileExists(built) && sbNeutrinoInstallComplete(built))
                    return sbNeutrinoResolved(built);
            }
        }
        // The picked device TYPE had no neutrino.elf -- do NOT dead-end here. Fall through to the AUTO
        // discovery below (legacy custom path -> active game device co-located -> mc0/mc1) so a picker
        // miss degrades to NHDDL-style cross-device discovery instead of returning NULL (which makes
        // bdmLaunchGame drop to a native launch that can die to OSDSYS -- issue #51). The chosen device
        // was tried FIRST above, so an explicit pick is still honoured when it holds the ELF.
    }

    // Auto: a legacy custom path (settings_riptopl.cfg "neutrino_path") wins when it exists;
    // otherwise fall back to the mc0:/mc1: auto-detect candidates below. A custom path that names
    // the elf directly (no dir) is honoured as-is (sbNeutrinoInstallComplete returns 1 for it).
    if (gNeutrinoPath[0] != '\0' && sbFileExists(gNeutrinoPath) && sbNeutrinoInstallComplete(gNeutrinoPath))
        return sbNeutrinoResolved(gNeutrinoPath);

    // PR #300: in AUTO, probe the ACTIVE game device for a co-located neutrino.elf BEFORE the mc0/mc1
    // fallbacks, so a neutrino.elf dropped next to the games (USB/MMCE) just works with zero config.
    // Δ1 (inside the helper): a stale elf-only folder on the game device must NOT shadow a complete
    // mc0/mc1 install below (the "worked once then never" failure). Same probe as NEUTRINO_DEV_GAME,
    // but here a miss falls through to the mc0/mc1 candidates instead of returning NULL.
    {
        const char *gameHit = sbNeutrinoProbeGameDevice(activePrefix);
        if (gameHit != NULL)
            return gameHit;
    }

    static const char *candidates[] = {
        NEUTRINO_PATH,     // mc0:NEUTRINO/neutrino.elf
        NEUTRINO_ALT_PATH, // mc1:NEUTRINO/neutrino.elf
        "mc0:/neutrino/neutrino.elf",
        "mc1:/neutrino/neutrino.elf",
        "mc0:/neutrino/NEUTRINO.ELF",
        "mc1:/neutrino/NEUTRINO.ELF",
        "mc0:NEUTRINO/NEUTRINO.ELF",
        "mc1:/NEUTRINO/NEUTRINO.ELF",
    };
    for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
        if (sbFileExists(candidates[i]) && sbNeutrinoInstallComplete(candidates[i]))
            return sbNeutrinoResolved(candidates[i]);
    }
    LOG("[NEUTRINO] no complete install found (elf without config/system.toml is skipped)\n");
    return NULL;
}

void neutrinoArgsParse(const char *in, neutrino_args_t *na)
{
    memset(na, 0, sizeof(*na));
    if (in == NULL)
        return;

    char buf[256];
    snprintf(buf, sizeof(buf), "%s", in);

    int breakSeen = 0;
    char *tok = strtok(buf, " \t");
    while (tok != NULL) {
        if (breakSeen || strcmp(tok, "--b") == 0) {
            breakSeen = 1; // --b and everything after it is for the ELF: keep verbatim, in order
            naAppend(na->extra, sizeof(na->extra), tok);
        } else if (strcmp(tok, "-qb") == 0) {
            na->qb = 1;
        } else if (strcmp(tok, "-dbc") == 0) {
            na->dbc = 1;
        } else if (strcmp(tok, "-logo") == 0) {
            na->logo = 1;
        } else if (strncmp(tok, "-cwd=", 5) == 0) {
            snprintf(na->cwd, sizeof(na->cwd), "%s", tok + 5);
        } else if (strncmp(tok, "-cfg=", 5) == 0) {
            snprintf(na->cfg, sizeof(na->cfg), "%s", tok + 5);
        } else if (strncmp(tok, "-elf=", 5) == 0) {
            snprintf(na->elf, sizeof(na->elf), "%s", tok + 5);
        } else if (strncmp(tok, "-ata0id=", 8) == 0) {
            snprintf(na->ata0id, sizeof(na->ata0id), "%s", tok + 8);
        } else if (strncmp(tok, "-ata0=", 6) == 0) {
            snprintf(na->ata0, sizeof(na->ata0), "%s", tok + 6);
        } else if (strncmp(tok, "-ata1=", 6) == 0) {
            snprintf(na->ata1, sizeof(na->ata1), "%s", tok + 6);
        } else {
            naAppend(na->extra, sizeof(na->extra), tok); // unknown/future flag -> preserved
        }
        tok = strtok(NULL, " \t");
    }
}

void neutrinoArgsAssemble(const neutrino_args_t *na, char *out, int outSize)
{
    if (out == NULL || outSize <= 0)
        return;
    out[0] = '\0';
    if (na->qb)
        naAppend(out, outSize, "-qb");
    if (na->dbc)
        naAppend(out, outSize, "-dbc");
    if (na->logo)
        naAppend(out, outSize, "-logo");
    if (na->cwd[0])
        naAppendKV(out, outSize, "-cwd=", na->cwd);
    if (na->cfg[0])
        naAppendKV(out, outSize, "-cfg=", na->cfg);
    if (na->elf[0])
        naAppendKV(out, outSize, "-elf=", na->elf);
    if (na->ata0[0])
        naAppendKV(out, outSize, "-ata0=", na->ata0);
    if (na->ata0id[0])
        naAppendKV(out, outSize, "-ata0id=", na->ata0id);
    if (na->ata1[0])
        naAppendKV(out, outSize, "-ata1=", na->ata1);
    if (na->extra[0]) // extra (may hold "--b ...") goes LAST so the break + ELF args stay at the tail
        naAppend(out, outSize, na->extra);
}

// Resolve the per-game VMC slots ($VMC_0/$VMC_1) into discrete Neutrino "-mcN=<prefix>VMC/<name>.bin"
// arg strings (issue #47: pass an OPL-configured VMC to Neutrino on launch). vmcPrefix is the device
// path prefix ending in its separator (e.g. "mass0:/" or "mmce0:/"); the VMC file lives at
// <vmcPrefix>VMC/<name>.bin -- the same location OPL's own mcemu uses, and the same path scheme as
// the already-working -dvd arg. Each configured slot becomes its OWN argv entry in sysLaunchNeutrino,
// so a VMC name containing a space is delivered to Neutrino intact instead of being shredded by the
// whitespace args tokenizer (the root cause of #47). Unconfigured slots are left empty. Call BEFORE
// deinit() frees the config/device data; vmcArgs is caller-owned storage that must outlive the launch.
void sbBuildVmcNeutrinoArgs(config_set_t *configSet, const char *vmcPrefix, neutrino_vmc_args_t *vmcArgs)
{
    int slot;
    char vmcName[32];

    if (vmcArgs == NULL)
        return;
    for (slot = 0; slot < NEUTRINO_VMC_SLOTS; slot++)
        vmcArgs->arg[slot][0] = '\0';

    if (configSet == NULL || vmcPrefix == NULL)
        return;

    for (slot = 0; slot < NEUTRINO_VMC_SLOTS; slot++) {
        vmcName[0] = '\0';
        configGetVMC(configSet, vmcName, sizeof(vmcName), slot);
        if (vmcName[0] != '\0') {
            // Per-slot disable (parity-audit #14): the user toggled this slot off without deleting
            // its card name. Skipping the -mc emission is the whole mechanism -- the empty arg also
            // drops the slot from vmcSlotMask, so the MMCE gameID card-switch re-arms for it and the
            // game sees the real card in that slot.
            int slotDisabled = 0;
            configGetVMCDisable(configSet, slot, &slotDisabled);
            if (slotDisabled) {
                LOG("[NEUTRINO] VMC slot %d (%s) disabled per-game -- launching without it\n", slot, vmcName);
                continue;
            }
            // Δ2 (NHDDL-parity): Neutrino ABORTS the whole boot when a -mcN VMC file can't be opened
            // post-reset (no "boot without VMC" fallback -- neutrino fhi_config.c) = black screen. Verify
            // the .bin exists NOW (mounts are still up; this runs pre-deinit) and skip the arg with a
            // toast instead of handing Neutrino an unopenable path. The game then boots with its real
            // card rather than dying. NHDDL never hits this class -- it emits no -mc args at all.
            char binPath[160]; // matches neutrino_vmc_args_t arg sizing: bdmPrefix(96)+"VMC/"+name(31)+".bin"
            int b = snprintf(binPath, sizeof(binPath), "%sVMC/%s.bin", vmcPrefix, vmcName);
            if (b >= (int)sizeof(binPath) || !sbFileExists(binPath)) {
                LOG("[NEUTRINO] VMC slot %d (%s) not found/oversize -- launching without it\n", slot, vmcName);
                guiWarning(_l(_STR_NEUTRINO_VMC_MISSING), 6);
                vmcArgs->arg[slot][0] = '\0';
                continue;
            }
            int n = snprintf(vmcArgs->arg[slot], sizeof(vmcArgs->arg[slot]), "-mc%d=%s", slot, binPath);
            if (n >= (int)sizeof(vmcArgs->arg[slot])) { // "-mc0=" + path overflowed the arg buffer
                LOG("[NEUTRINO] VMC slot %d arg truncated (%d bytes) -- launching without it\n", slot, n);
                guiWarning(_l(_STR_NEUTRINO_VMC_MISSING), 6);
                vmcArgs->arg[slot][0] = '\0';
            }
        }
    }
}
