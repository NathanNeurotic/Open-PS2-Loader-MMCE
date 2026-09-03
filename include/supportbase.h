#ifndef __SUPPORT_BASE_H
#define __SUPPORT_BASE_H

#include "include/system.h" // neutrino_vmc_args_t

#define UL_GAME_NAME_MAX       32
#define ISO_GAME_NAME_MAX      160
#define ISO_GAME_EXTENSION_MAX 4
#define GAME_STARTUP_MAX       12

#define ISO_GAME_FNAME_MAX (ISO_GAME_NAME_MAX + ISO_GAME_EXTENSION_MAX)

enum GAME_FORMAT {
    GAME_FORMAT_USBLD = 0,
    GAME_FORMAT_OLD_ISO,
    GAME_FORMAT_ISO,
    // A browsable subdirectory row (folder navigation, opt-in). Never launched, never written to the
    // games.bin cache and never stored as a favourite; the dispatch intercepts it to descend instead.
    GAME_FORMAT_FOLDER,
};

typedef struct
{
    char name[ISO_GAME_NAME_MAX + 1]; // MUST be the higher value from UL / ISO
    char startup[GAME_STARTUP_MAX + 1];
    char extension[ISO_GAME_EXTENSION_MAX + 1];
    u8 parts;
    u8 media;
    u8 format;
    u32 sizeMB;
} base_game_info_t;

typedef struct
{
    char name[UL_GAME_NAME_MAX];    // it is not a string but character array, terminating NULL is not necessary
    char magic[3];                  // magic string "ul."
    char startup[GAME_STARTUP_MAX]; // it is not a string but character array, terminating NULL is not necessary
    u8 parts;                       // slice count
    u8 media;                       // Disc type
    u8 unknown[4];                  // Always zero
    u8 Byte08;                      // Always 0x08
    u8 unknown2[10];                // Always zero
} USBExtreme_game_entry_t;

int isValidIsoName(char *name, int *pNameLen);
int sbIsSameSize(const char *prefix, int prevSize);
int sbCreateSemaphore(void);
// sub = current browse subpath below CD/DVD ("" at root); NULL opts the caller OUT of folder rows
// (ETH/SMB). On a TOTAL device-read failure the caller's list is left untouched (last-good list);
// GAME_FORMAT_FOLDER rows, and the ul.cfg (USBLD) leg -- a device-root-only concept -- is skipped
// inside subfolders.
int sbReadList(base_game_info_t **list, const char *prefix, const char *sub, int *fsize, int *gamecount);
// Folder browsing: set the active subpath the path composers inject (see sbBrowseSub in supportbase.c).
void sbSetBrowseSub(const char *sub);
const char *sbGetCheatSearchLog(void);
// "No cheats found" text with the locations actually probed appended (#265).
const char *sbCheatsNotFoundText(void);
int sbCheatsMissingContinue(void *pCommon, int cheatResult);
int sbLoadImage(const char *path, const char *file);
void sbSetDiscAttributes(config_set_t *config, int isPS1, int isCD); // #System/#Media/#DiscType identity stamp
int sbPrepare(base_game_info_t *game, config_set_t *configSet, int size_cdvdman, void **cdvdman_irx, int *patchindex);
void sbUnprepare(void *pCommon);
void sbRebuildULCfg(base_game_info_t **list, const char *prefix, int gamecount, int excludeID);
void sbCreatePath(const base_game_info_t *game, char *path, const char *prefix, const char *sep, int part);
void sbDelete(base_game_info_t **list, const char *prefix, const char *sep, int gamecount, int id);
void sbRename(base_game_info_t **list, const char *prefix, const char *sep, int gamecount, int id, char *newname);
config_set_t *sbPopulateConfig(base_game_info_t *game, const char *prefix, const char *sep);
// Gate for sbPopulateConfig's per-game size stat. OFF while scrolling the game list -- over SMB a
// fresh stat() of an ISO can cost seconds, and the main page only needs the metadata-derived badges
// (#DiscType/#Media/#Format), never #Size. The info screen flips it on via menuRequestInfoSize() so
// #Size still resolves on demand. (1 = stat, 0 = skip.)
void sbSetConfigStatSize(int enable);
int sbConfigStatSizeEnabled(void);
void sbCreateFolders(const char *path, int createDiscImgFolders);

// ISO9660 filesystem management functions.
u32 sbGetISO9660MaxLBA(const char *path);
int sbProbeISO9660(const char *path, base_game_info_t *game, u32 layer1_offset);

int sbLoadCheats(const char *path, const char *file);


int sbFileExists(const char *path);

// First existing Neutrino core ELF, or NULL. In AUTO mode (gNeutrinoDevice==0): custom gNeutrinoPath
// -> the active game's device (activePrefix) -> mc0/mc1 install spots. An explicit Device picker
// ignores activePrefix. Pass NULL when no game device applies.
const char *sbResolveNeutrinoPath(const char *activePrefix);

// Structured view of the USER-settable Neutrino launch flags (the catch-all "Launch Args" box).
typedef struct
{
    int qb;          // -qb (quick-boot)
    int dbc;         // -dbc (debug colors)
    int logo;        // -logo (PS2 logo)
    char cwd[64];    // -cwd=
    char cfg[64];    // -cfg=
    char elf[64];    // -elf=
    char ata0[64];   // -ata0=
    char ata0id[64]; // -ata0id=
    char ata1[64];   // -ata1=
    char extra[64];  // unrecognised/free tokens, space-joined; "--b ..." preserved at the tail
} neutrino_args_t;
// Parse an args string into the struct; assemble it back in a Neutrino-accepted order (--b last).
void neutrinoArgsParse(const char *in, neutrino_args_t *na);
void neutrinoArgsAssemble(const neutrino_args_t *na, char *out, int outSize);

// Fully-formed Neutrino -mcN VMC args for both slots, resolved from the per-game config
// BEFORE deinit frees it. vmcPrefix = the device prefix VMC/ lives under.
void sbBuildVmcNeutrinoArgs(config_set_t *configSet, const char *vmcPrefix, neutrino_vmc_args_t *vmcArgs);

#ifdef RETROACHIEVEMENTS
// RA: load this game's watch list from <path>RA/<file>.wl. Mirrors sbLoadCheats'
// shape (256-byte path, both extension cases). A missing list is not an error.
int sbLoadWatchList(const char *path, const char *file);
#endif

#endif
