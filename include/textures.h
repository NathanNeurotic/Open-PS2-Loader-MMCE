#ifndef __TEXTURES_H
#define __TEXTURES_H

enum INTERNAL_TEXTURE {
    LOAD0_ICON = 0,
    LOAD1_ICON,
    LOAD2_ICON,
    LOAD3_ICON,
    LOAD4_ICON,
    LOAD5_ICON,
    LOAD6_ICON,
    LOAD7_ICON,
    BDM_ICON,
    USB_ICON,
    ILINK_ICON,
    MX4SIO_ICON,
    HDD_BD_ICON,
    MMCE_ICON,
    HDD_ICON,
    ETH_ICON,
    UDP_ICON, // UDPBD network-boot device icon (theme override "udp_bd", embedded udp_bd_png)
    APP_ICON,
    INDEX_0,
    INDEX_1,
    INDEX_2,
    INDEX_3,
    INDEX_4,
    LEFT_ICON,
    RIGHT_ICON,
    CROSS_ICON,
    TRIANGLE_ICON,
    CIRCLE_ICON,
    SQUARE_ICON,
    SELECT_ICON,
    START_ICON,
    /* currently unused.
    UP_ICON,
    DOWN_ICON,
    L1_ICON,
    L2_ICON,
    R1_ICON,
    R2_ICON, */
    L3_ICON,     // re-enabled for the VCD "L3 = toggle VCD view" hint
    R3_ICON,     // re-enabled for the Favourites "R3 = Favourite" hint
    FAV_ICON,    // Favourites tab icon
    FAV_MARK,    // star drawn next to favourited items
    SETTINGS_BG, // optional settings/menu background (use_settings_bg theme flag; see guiDrawBGSettings)
    COVER_DEFAULT,
    DISC_DEFAULT,
    SCREEN_DEFAULT,
    INCEBTION_PICTURE,
    IP_PICTURE,
    COVERAPP_DEFAULT,
    MISSING_PICTURE,
    NO_DEVICE_PICTURE,
    NO_RATING_PICTURE,
    SCREENS_OVERLAY,
    ELF_FORMAT,
    HDL_FORMAT,
    ISO_FORMAT,
    VCD_FORMAT, // VCD (PS1-via-POPSTARTER) format glyph -- the VCD equivalent of ISO
    ZSO_FORMAT,
    UL_FORMAT,
    APP_MEDIA,
    CD_MEDIA,
    DVD_MEDIA,
    ASPECT_STD,
    ASPECT_WIDE,
    ASPECT_WIDE1,
    ASPECT_WIDE2,
    DEVICE_1,
    DEVICE_2,
    DEVICE_3,
    DEVICE_4,
    DEVICE_5,
    DEVICE_6,
    DEVICE_ALL,
    RATING_0,
    RATING_1,
    RATING_2,
    RATING_3,
    RATING_4,
    RATING_5,
    SCAN_240P,
    SCAN_240P1,
    SCAN_480I,
    SCAN_480P,
    SCAN_480P1,
    SCAN_480P2,
    SCAN_480P3,
    SCAN_480P4,
    SCAN_480P5,
    SCAN_576I,
    SCAN_576P,
    SCAN_720P,
    SCAN_1080I,
    SCAN_1080I2,
    SCAN_1080P,
    VMODE_MULTI,
    VMODE_NTSC,
    VMODE_PAL,
    LOGO_PICTURE,
    // Optional animated boot-logo frames (embedded build assets). When present
    // they are cycled on the boot splash like the loading icon; when absent the
    // single LOGO_PICTURE is used. Keep contiguous (animation indexes LOGO0+n).
    LOGO0_PICTURE,
    LOGO1_PICTURE,
    LOGO2_PICTURE,
    LOGO3_PICTURE,
    LOGO4_PICTURE,
    LOGO5_PICTURE,
    LOGO6_PICTURE,
    CASE_OVERLAY,
    APPS_CASE_OVERLAY,
    PS1_SYSTEM, // #System console glyph (PS1) -- FR #49
    PS2_SYSTEM, // #System console glyph (PS2)
    // second overlay layer (b2 "foliage") drawn over the case (b1) -- graphics-team two-layer frame
    CASE_OVERLAY2,
    // UDPFS network-boot device icon (theme override "udp_fs", embedded udp_fs_png); UDPBD uses UDP_ICON
    UDPFS_ICON,

    TEXTURES_COUNT
};

#define ERR_BAD_FILE      -1 // open() failed = the file is not there (genuine absence)
#define ERR_READ_STRUCT   -2
#define ERR_INFO_STRUCT   -3
#define ERR_SET_JMP       -4
#define ERR_BAD_DIMENSION -5
#define ERR_MISSING_ALPHA -6
#define ERR_BAD_DEPTH     -7
#define ERR_LOAD_ABORTED  -8
// The file OPENED but its bytes could not be staged/read (read() error on a contended bus, OOM, empty
// file). Distinct from ERR_BAD_FILE (absent) so the VCD art miss-memo caches only GENUINE absence and
// re-probes a transient/present failure instead of false-hiding a real cover (#120). Callers that only
// test the sign are unaffected.
#define ERR_FILE_IO       -9

int texLookupInternalTexId(const char *name);
int texLoadInternal(GSTEXTURE *texture, int texId);
int texLoadFromMemory(GSTEXTURE *texture, const void *buf, u32 size);
int texDiscoverLoad(GSTEXTURE *texture, const char *path, int texId);
void texSetLoadAbortFlag(volatile int *abortRequested);
void texFree(GSTEXTURE *texture);

#endif
