#ifndef __CONFIG_H
#define __CONFIG_H

// Enum for the different types of config files. Game-specific config files (<game ID>.cfg) will always have an ID of 0.
enum CONFIG_INDEX {
    CONFIG_INDEX_OPL = 0,
    CONFIG_INDEX_LAST,
    CONFIG_INDEX_APPS,
    CONFIG_INDEX_NETWORK,
    CONFIG_INDEX_GAME,

    CONFIG_INDEX_COUNT
};

// Config type bits
#define CONFIG_OPL     (1 << CONFIG_INDEX_OPL)
#define CONFIG_LAST    (1 << CONFIG_INDEX_LAST)
#define CONFIG_APPS    (1 << CONFIG_INDEX_APPS)
#define CONFIG_NETWORK (1 << CONFIG_INDEX_NETWORK)
#define CONFIG_GAME    (1 << CONFIG_INDEX_GAME)
#define CONFIG_ALL     0xFF

// RiptOPL master settings file. Renamed conf_riptopl.cfg -> settings_riptopl.cfg: clearer, and
// namespaced so it won't collide with another app's generic settings.cfg if they ever share a
// folder (e.g. a future $appdir "sidecar" config). configRead() falls back to the legacy name so
// existing installs keep their settings -- the next save writes the new name (auto-migration).
// Keep both as plain string literals so callers can string-concatenate them.
#define CONFIG_OPL_FILENAME        "settings_riptopl.cfg"
#define CONFIG_OPL_FILENAME_LEGACY "conf_riptopl.cfg"

#define CONFIG_SOURCE_DEFAULT 0
#define CONFIG_SOURCE_USER    1
#define CONFIG_SOURCE_DLOAD   2 // Downloaded from the network

// Items for per-game config files.
#define CONFIG_ITEM_NAME             "#Name"
#define CONFIG_ITEM_LONGNAME         "#LongName"
#define CONFIG_ITEM_SIZE             "#Size"
#define CONFIG_ITEM_FORMAT           "#Format"
#define CONFIG_ITEM_MEDIA            "#Media"
#define CONFIG_ITEM_SYSTEM           "#System"   // console axis (PS1/PS2); #Media stays the disc axis (CD/DVD) -- FR #49
#define CONFIG_ITEM_DISCTYPE         "#DiscType" // combined console+media token (PS1CD/PS2CD/PS2DVD) so one AttributeImage glyph distinguishes PS1-CD from PS2-CD (both report #Media=CD) -- issue #49
#define CONFIG_ITEM_STARTUP          "#Startup"
#define CONFIG_ITEM_ALTSTARTUP       "$AltStartup"
#define CONFIG_ITEM_VMC              "$VMC"
#define CONFIG_ITEM_VMC_DISABLE      "$VMCDisable" // per-slot ("$VMCDisable_0"/"_1"): launch WITHOUT this slot's VMC while keeping its card name configured (Neutrino -mc path only)
#define CONFIG_ITEM_COMPAT           "$Compatibility"
#define CONFIG_ITEM_DMA              "$DMA"
#define CONFIG_ITEM_CORE_LOADER      "$CoreLoader"
#define CONFIG_ITEM_NEUTRINO_ARGS    "$NeutrinoArgs"
#define CONFIG_ITEM_NEUTRINO_VIDEO   "$NeutrinoVideo"
#define CONFIG_ITEM_NEUTRINO_GSMCOMP "$NeutrinoGsmComp" // -gsm ":c" field-flip half (0=off, 1-3=type); only emitted when $NeutrinoVideo is set
#define CONFIG_ITEM_NEUTRINO_BSDFS   "$NeutrinoBsdfs"   // -bsdfs override (parity-audit #11): 0=Auto, 1=exfat, 2=hdl, 3=bd; block-backed devices only
#define CONFIG_ITEM_DNAS             "$DNAS"
#define CONFIG_ITEM_CONFIGSOURCE     "$ConfigSource"

#define CONFIG_ITEM_OSD_SETTINGS_LANGID "$CustomLanguageValue"
#define CONFIG_ITEM_OSD_SETTINGS_SOURCE "$CustomLanguageSource"
#define CONFIG_ITEM_OSD_SETTINGS_ENABLE "$OSDSettingsEnable"
#define CONFIG_ITEM_OSD_SETTINGS_TV_ASP "$OSDAspectRatio"
#define CONFIG_ITEM_OSD_SETTINGS_VMODE  "$OSDVideoMode"
// Per-Game GSM keys. -Bat-
#define CONFIG_ITEM_GSMSOURCE           "$GSMSource"
#define CONFIG_ITEM_ENABLEGSM           "$EnableGSM"
#define CONFIG_ITEM_GSMVMODE            "$GSMVMode"
#define CONFIG_ITEM_GSMXOFFSET          "$GSMXOffset"
#define CONFIG_ITEM_GSMYOFFSET          "$GSMYOffset"
#define CONFIG_ITEM_GSMFIELDFIX         "$GSMFIELDFix"

// Per-Game CHEAT keys. -Bat-
#define CONFIG_ITEM_CHEATSSOURCE "$CheatsSource"
#define CONFIG_ITEM_ENABLECHEAT  "$EnableCheat"
#define CONFIG_ITEM_CHEATMODE    "$CheatMode"
#define CONFIG_ITEM_ENABLEIMAGE  "$EnableImage"

#define CONFIG_ITEM_PADEMUSOURCE     "$PADEMUSource"
#define CONFIG_ITEM_ENABLEPADEMU     "$EnablePadEmu"
#define CONFIG_ITEM_PADEMUSETTINGS   "$PadEmuSettings"
#define CONFIG_ITEM_PADMACROSETTINGS "$PadMacroSettings"
#define CONFIG_ITEM_PADMACROSOURCE   "$PadMacroSource"

// OPL config keys
#define CONFIG_OPL_THEME                      "theme"
#define CONFIG_OPL_LANGUAGE                   "language_text"
#define CONFIG_OPL_SCROLLING                  "scrolling"
#define CONFIG_OPL_BGCOLOR                    "bg_color"
#define CONFIG_OPL_TEXTCOLOR                  "text_color"
#define CONFIG_OPL_UI_TEXTCOLOR               "ui_text_color"
#define CONFIG_OPL_SEL_TEXTCOLOR              "sel_text_color"
#define CONFIG_OPL_PLAS_BLEND_COLOR           "plasma_blend_color" // plasma gradient LOW end (parity-audit #15); doubles as the theme-cfg key
#define CONFIG_OPL_ENABLE_NOTIFICATIONS       "enable_notifications"
#define CONFIG_OPL_ENABLE_COVERART            "enable_coverart"
#define CONFIG_OPL_ENABLE_BGART               "enable_bgart"
#define CONFIG_OPL_ENABLE_ART_TAR             "enable_art_tar"
#define CONFIG_OPL_ART_DELAY                  "art_delay"
#define CONFIG_OPL_WIDESCREEN                 "wide_screen"
#define CONFIG_OPL_DEFAULT_GAME_VIEW          "default_game_view"
#define CONFIG_OPL_VMODE                      "vmode"
#define CONFIG_OPL_XOFF                       "xoff"
#define CONFIG_OPL_YOFF                       "yoff"
#define CONFIG_OPL_OVERSCAN                   "overscan"
#define CONFIG_OPL_DISABLE_DEBUG              "disable_debug"
#define CONFIG_OPL_PS2LOGO                    "ps2logo"
#define CONFIG_OPL_DEFAULT_CORE               "default_core"              // global default Loader Core (0=<OPL>, 1=Neutrino); a game's per-game "$CoreLoader" overrides it, absent = follow this
#define CONFIG_OPL_NEUTRINO_VIDEO             "neutrino_video_default"    // global default Neutrino -gsm video mode (0=Off..5=1080i x3); per-game "$NeutrinoVideo" overrides, absent = follow this
#define CONFIG_OPL_NEUTRINO_GSMCOMP           "neutrino_gsm_comp_default" // global default -gsm ":c" field-flip half (0=off, 1-3=type); per-game "$NeutrinoGsmComp" overrides, absent = follow this
#define CONFIG_OPL_NEUTRINO_ARGS              "neutrino_args"
#define CONFIG_OPL_NEUTRINO_PATH              "neutrino_path"
#define CONFIG_OPL_NEUTRINO_DEVICE            "neutrino_device"            // legacy device-INDEX (mc0/mass0/mmce0); read-only, migrated
#define CONFIG_OPL_NEUTRINO_DEVTYPE           "neutrino_devtype"           // device-TYPE (NEUTRINO_DEV_*); the live key
#define CONFIG_OPL_NEUTRINO_ELF_ARG           "neutrino_elf_arg"           // default-on (no UI row): auto-emit -elf=cdrom0:\<startup>;1 (parity Delta-10)
#define CONFIG_OPL_POPSTARTER_PATH            "popstarter_path"            // free-text custom path (used only when device=Custom)
#define CONFIG_OPL_POPSTARTER_DEVICE          "popstarter_device"          // device TYPE holding POPS/POPSTARTER.ELF (POPS_DEV_*)
#define CONFIG_OPL_POPSTARTER_RETROGEM_GAMEID "popstarter_retrogem_gameid" // RetroGEM Game ID optical barcode for VCD launches

#define CONFIG_OPL_BDMA_SOURCE         "bdma_source"
#define CONFIG_OPL_BDMA_APPLY          "bdma_apply_launch"
#define CONFIG_OPL_VCD_HIDE_GAMEID     "vcd_hide_gameid"     // display-only: hide a leading PS1 game-ID prefix from the VCD list
#define CONFIG_OPL_VCD_FIRST_DISC_ONLY "vcd_first_disc_only" // #118: hide discs 2+ of a multi-disc PS1 set from the device VCD lists
#define CONFIG_OPL_HDD_GAME_LIST_CACHE "hdd_game_list_cache"
#define CONFIG_OPL_EXIT_PATH           "exit_path"
#define CONFIG_OPL_AUTO_SORT           "autosort"
#define CONFIG_OPL_AUTO_REFRESH        "autorefresh"
#define CONFIG_OPL_DEFAULT_DEVICE      "default_device"
#define CONFIG_OPL_ENABLE_WRITE        "enable_delete_rename"
#define CONFIG_OPL_HDD_SPINDOWN        "hdd_spindown"
#define CONFIG_OPL_BDM_PREFIX          "usb_prefix" // Leave this "usb" for compatibility
#define CONFIG_OPL_ETH_PREFIX          "eth_prefix"
#define CONFIG_OPL_REMEMBER_LAST       "remember_last"
#define CONFIG_OPL_FOLDER_NAV          "folder_nav"
#define CONFIG_OPL_RUMBLE              "enable_rumble"
#define CONFIG_OPL_AUTOSTART_LAST      "autostart_last"
#define CONFIG_OPL_BDM_MODE            "usb_mode" // Leave this "usb" for compatibility
#define CONFIG_OPL_HDD_MODE            "hdd_mode"
#define CONFIG_OPL_ETH_MODE            "eth_mode"
#define CONFIG_OPL_APP_MODE            "app_mode"
#define CONFIG_OPL_FAV_MODE            "fav_mode"
#define CONFIG_OPL_MMCE_MODE           "mmce_mode"
#define CONFIG_OPL_BDM_CACHE           "bdm_cache"
#define CONFIG_OPL_HDD_CACHE           "hdd_cache"
#define CONFIG_OPL_SMB_CACHE           "smb_cache"
#define CONFIG_OPL_MMCE_PREFIX         "mmce_prefix"
#define CONFIG_OPL_MMCE_SLOT           "mmce_slot"
#define CONFIG_OPL_MMCEIGR_SLOT        "mmceigr_slot"
#define CONFIG_OPL_MMCE_GAMEID         "mmce_gameid"
#define CONFIG_OPL_APPLY_GAMEID        "apply_gameid"
#define CONFIG_OPL_MMCE_WAIT_CYCLES    "mmce_wait_cycles"
#define CONFIG_OPL_MMCE_USE_ALARMS     "mmce_use_alarms"
#define CONFIG_OPL_MMCE_PACING_MIGR    "mmce_pacing_migrated"
#define CONFIG_OPL_ENABLE_USB          "enable_usb"
#define CONFIG_OPL_ENABLE_ILINK        "enable_ilink"
#define CONFIG_OPL_ENABLE_MX4SIO       "enable_mx4sio"
#define CONFIG_OPL_ENABLE_BDMHDD       "enable_bdm_hdd"
#define CONFIG_OPL_ENABLE_UDPBD        "enable_udpbd"
#define CONFIG_OPL_NET_BOOT_PROTOCOL   "net_boot_protocol"
#define CONFIG_OPL_NETWORK_PROTOCOL    "network_protocol"
#define CONFIG_OPL_NET_START_MODE      "net_start_mode"
#define CONFIG_OPL_SMB_DIALECT         "smb_dialect"
#define CONFIG_OPL_SWAP_SEL_BUTTON     "swap_select_btn"
#define CONFIG_OPL_PARENTAL_LOCK_PWD   "parental_lock_password"
#define CONFIG_OPL_SFX                 "enable_sfx"
#define CONFIG_OPL_BOOT_SND            "enable_boot_snd"
#define CONFIG_OPL_BGM                 "enable_bgm"
#define CONFIG_OPL_SFX_VOLUME          "sfx_volume"
#define CONFIG_OPL_BOOT_SND_VOLUME     "boot_snd_volume"
#define CONFIG_OPL_BGM_VOLUME          "bgm_volume"
#define CONFIG_OPL_DEFAULT_BGM_PATH    "default_bgm_path"
#define CONFIG_OPL_XSENSITIVITY        "x_sensitivity"
#define CONFIG_OPL_YSENSITIVITY        "y_sensitivity"
#define CONFIG_OPL_COVERFLOW_COUNT     "coverflow_count"
#define CONFIG_OPL_COVERFLOW_SCALE     "coverflow_scale"
#define CONFIG_OPL_COVERFLOW_ANIM      "coverflow_anim"
#define CONFIG_OPL_COVERFLOW_DIM       "coverflow_dim"

// Network config keys
#define CONFIG_NET_ETH_LINKM          "eth_linkmode"
#define CONFIG_NET_PS2_DHCP           "ps2_ip_use_dhcp"
#define CONFIG_NET_PS2_IP             "ps2_ip_addr"
#define CONFIG_NET_PS2_NETM           "ps2_netmask"
#define CONFIG_NET_PS2_GATEW          "ps2_gateway"
#define CONFIG_NET_PS2_DNS            "ps2_dns"
#define CONFIG_NET_SMB_NB_ADDR        "smb_share_nb_addr"
#define CONFIG_NET_SMB_IP_ADDR        "smb_ip"
#define CONFIG_NET_SMB_NBNS           "smb_share_use_nbns"
#define CONFIG_NET_SMB_SHARE          "smb_share"
#define CONFIG_NET_SMB_USER           "smb_user"
#define CONFIG_NET_SMB_PASSW          "smb_pass"
#define CONFIG_NET_SMB_PORT           "smb_port"
#define CONFIG_NET_NBD_DEFAULT_EXPORT "nbd_default_export"

#define CONFIG_KEY_NAME_LEN  32
#define CONFIG_KEY_VALUE_LEN 256

struct config_value_t
{
    // Including the NULL terminator
    char key[CONFIG_KEY_NAME_LEN];
    char val[CONFIG_KEY_VALUE_LEN];

    struct config_value_t *next;
};

// On-disk syntax of a config file. We support BOTH as first-class citizens and NEVER convert a user's
// file from one to the other -- whatever format we read is the format we write back (see configWrite).
// wOPL migrated to libconfig (their PR #286) and rewrites shared files IN PLACE, so a user who has run
// wOPL once has libconfig where we expect key=value. Honouring their layout is the whole point: we do
// not force ours on top of an existing setup.
#define CFG_FMT_LEGACY    0 // "key=value\r\n" -- OPL's own syntax; also what OPL-Launcher/SAS/XMB read
#define CFG_FMT_LIBCONFIG 1 // "key = value;" -- wOPL/libconfig, groups as "name : { ... };"

typedef struct
{
    int type;
    struct config_value_t *head;
    struct config_value_t *tail;
    char *filename;
    int modified;
    int format; // CFG_FMT_*: latched from the file we READ, honoured by configWrite
    u32 uid;
} config_set_t;

void configInit(char *prefix);
void configSetMove(char *prefix);
void configMove(config_set_t *configSet, const char *fileName);
void configEnd();
config_set_t *configAlloc(int type, config_set_t *configSet, char *fileName);
void configFree(config_set_t *configSet);
// Deep-copy a config set into a fresh standalone (heap) set, NOT registered in configFiles[].
// Used for transient "test launch" so dialog edits never touch the live config. Free with configFree().
config_set_t *configClone(config_set_t *src);
config_set_t *configGetByType(int type);
int configSetStr(config_set_t *configSet, const char *key, const char *value);
int configGetStr(config_set_t *configSet, const char *key, const char **value);
int configGetStrCopy(config_set_t *configSet, const char *key, char *value, int length);
int configSetInt(config_set_t *configSet, const char *key, const int value);
int configGetInt(config_set_t *configSet, const char *key, int *value);
int configSetColor(config_set_t *configSet, const char *key, unsigned char *color);
int configGetColor(config_set_t *configSet, const char *key, unsigned char *color);
int configRemoveKey(config_set_t *configSet, const char *key);
void configMerge(config_set_t *dest, const config_set_t *source);

void configGetDiscIDBinary(config_set_t *configSet, void *dst);

int configRead(config_set_t *configSet);
int configReadBuffer(config_set_t *configSet, const void *buffer, int size);
int configReadMulti(int types);
int configWrite(config_set_t *configSet);
int configWriteMulti(int types);
void configClear(config_set_t *configSet);

void configGetVMC(config_set_t *configSet, char *vmc, int length, int slot);
void configSetVMC(config_set_t *configSet, const char *vmc, int slot);
void configRemoveVMC(config_set_t *configSet, int slot);
void configGetVMCDisable(config_set_t *configSet, int slot, int *disabled);
void configSetVMCDisable(config_set_t *configSet, int slot, int disabled);
void configRemoveVMCDisable(config_set_t *configSet, int slot);

char *configGetDir(void);
void configPrepareNotifications(char *prefix);

#endif
