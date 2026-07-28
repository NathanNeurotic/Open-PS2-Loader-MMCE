#ifndef __SUPPORT_BASE_H
#define __SUPPORT_BASE_H

struct neutrino_vmc_args; // full definition in include/system.h (pointer use only here)

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
// Scan a device's game list. sub is the folder-browse subpath below CD/DVD ("" or NULL = the device
// root, the normal case); when set, the CD/DVD scans descend into it, subdirectories are listed as
// GAME_FORMAT_FOLDER rows, and the ul.cfg (USBLD) leg -- a device-root-only concept -- is skipped.
int sbReadList(base_game_info_t **list, const char *prefix, const char *sub, int *fsize, int *gamecount);

// Set the folder-browse subpath the path composers (sbCreatePath / sbPopulateConfig) inject between
// CD|DVD and the game name. A device leg calls this with folderGetSub(mode) before launch / delete /
// rename so a game inside a subfolder resolves to <prefix>CD|DVD/<sub>/<name>. "" or NULL = root.
void sbSetBrowseSub(const char *sub);
int sbPrepare(base_game_info_t *game, config_set_t *configSet, int size_cdvdman, void **cdvdman_irx, int *patchindex);
void sbUnprepare(void *pCommon);
void sbRebuildULCfg(base_game_info_t **list, const char *prefix, int gamecount, int excludeID);
void sbCreatePath(const base_game_info_t *game, char *path, const char *prefix, const char *sep, int part);
void sbDelete(base_game_info_t **list, const char *prefix, const char *sep, int gamecount, int id);
void sbRename(base_game_info_t **list, const char *prefix, const char *sep, int gamecount, int id, char *newname);
config_set_t *sbPopulateConfig(base_game_info_t *game, const char *prefix, const char *sep);
// Enable/disable the per-game size stat inside sbPopulateConfig. Off while scrolling (so the
// disc/media badges paint instantly even over slow SMB); the info screen turns it on to resolve
// #Size on demand. See sbConfigStatSize in supportbase.c. (1 = stat, 0 = skip.)
void sbSetConfigStatSize(int enable);
// Set the console/media display attributes a theme's AttributeImage badges resolve against
// (#System, #Media, #DiscType). ANY itemGetConfig that does NOT go through sbPopulateConfig
// (e.g. the internal-HDD HDL path) MUST call this, or those badges silently never render
// on that device (drawAttributeImage bails at its NULL-value guard). isPS1 -> PS1 (always a CD).
void sbSetDiscAttributes(config_set_t *config, int isPS1, int isCD);
// Append neutrino -mc0/-mc1 VMC args (from the per-game config) for vmcPrefix's device. Call before deinit.
void sbBuildVmcNeutrinoArgs(config_set_t *configSet, const char *vmcPrefix, struct neutrino_vmc_args *vmcArgs);
void sbCreateFolders(const char *path, int createDiscImgFolders);

// ISO9660 filesystem management functions.
u32 sbGetISO9660MaxLBA(const char *path);
int sbProbeISO9660(const char *path, base_game_info_t *game, u32 layer1_offset);

int sbFileExists(const char *path);

// First existing Neutrino core ELF, or NULL. In AUTO mode (gNeutrinoDevice==0): custom gNeutrinoPath
// -> the active game's device (activePrefix, so a neutrino.elf next to the games is found with zero
// config) -> mc0/mc1 install spots. An explicit Device picker ignores activePrefix. Pass NULL when no
// game device applies / the prefix is not POSIX-open-reachable (e.g. raw APA HDD) (#300).
const char *sbResolveNeutrinoPath(const char *activePrefix);

// Structured view of the USER-settable Neutrino launch flags (the catch-all "Launch Args" box).
// The flags OPL emits itself (-bsd/-dvd/-gc/-gsm/-mc/-dbc/-logo) are NOT represented here.
typedef struct
{
    int qb;          // -qb (quick-boot)
    int dbc;         // -dbc (debug colors) -- per-launch override; the global toggle still auto-emits
    int logo;        // -logo (PS2 logo)    -- per-launch override; the global toggle still auto-emits
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

int sbLoadCheats(const char *path, const char *file);
// Newline-separated list of every location the last sbLoadCheats() call probed (#265).
const char *sbGetCheatSearchLog(void);
// Localized "no cheats found" text with the probed locations appended (#265). Static buffer.
const char *sbCheatsNotFoundText(void);
int sbLoadImage(const char *path, const char *file);

#endif
