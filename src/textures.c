#include "include/opl.h"
#include "include/textures.h"
#include "include/util.h"
#include "include/ioman.h"
#include <errno.h> // errno/ENOENT in the staging-arm open classifier (transitively via opl.h, but be explicit)
#include <kernel.h>
#include <png.h>

extern void *load0_png;
extern void *load1_png;
extern void *load2_png;
extern void *load3_png;
extern void *load4_png;
extern void *load5_png;
extern void *load6_png;
extern void *load7_png;
extern void *usb_png; // Leave BDM Icon as usb.png to maintain theme compat
extern void *usb_bd_png;
extern void *ilk_bd_png;
extern void *m4s_bd_png;
extern void *hdd_bd_png;
extern void *mmce_png;
extern void *hdd_png;
extern void *eth_png;
extern void *udp_bd_png;
extern void *udp_fs_png;
extern void *app_png;
extern void *Index_0_png;
extern void *Index_1_png;
extern void *Index_2_png;
extern void *Index_3_png;
extern void *Index_4_png;

extern void *left_png;
extern void *right_png;
extern void *cross_png;
extern void *triangle_png;
extern void *circle_png;
extern void *square_png;
extern void *select_png;
extern void *start_png;
/* currently unused.
extern void *up_png;
extern void *down_png;
extern void *L1_png;
extern void *L2_png;
extern void *R1_png;
extern void *R2_png; */
extern void *L3_png;
extern void *R3_png;
extern void *fav_png;
extern void *fav_mark_png;

extern void *cover_png;
extern void *disc_png;
extern void *screen_png;
extern void *incebtion_png;
extern void *ip_png;
extern void *coverapp_png;
extern void *missing_png;
extern void *screens_png;

extern void *ELF_png;
extern void *HDL_png;
extern void *ISO_png;
extern void *VCD_png;
extern void *ZSO_png;
extern void *UL_png;
extern void *APPS_png;
extern void *CD_png;
extern void *DVD_png;
extern void *Aspect_s_png;
extern void *Aspect_w_png;
extern void *Aspect_w1_png;
extern void *Aspect_w2_png;
extern void *Rating_0_png;
extern void *Rating_1_png;
extern void *Rating_2_png;
extern void *Rating_3_png;
extern void *Rating_4_png;
extern void *Rating_5_png;
extern void *Scan_240p_png;
extern void *Scan_240p1_png;
extern void *Scan_480i_png;
extern void *Scan_480p_png;
extern void *Scan_480p1_png;
extern void *Scan_480p2_png;
extern void *Scan_480p3_png;
extern void *Scan_480p4_png;
extern void *Scan_480p5_png;
extern void *Scan_576i_png;
extern void *Scan_576p_png;
extern void *Scan_720p_png;
extern void *Scan_1080i_png;
extern void *Scan_1080i2_png;
extern void *Scan_1080p_png;
extern void *Vmode_multi_png;
extern void *Vmode_ntsc_png;
extern void *Vmode_pal_png;

extern void *logo_png;
extern void *logo0_png;
extern void *logo1_png;
extern void *logo2_png;
extern void *logo3_png;
extern void *case_png;
extern void *PS1_png;
extern void *PS2_png;
extern void *case_overlay_png;

// Not related to screen size, just to limit at some point
static int maxSize = 720 * 512 * 4;
// Per-read() chunk for staging an MMCE or BDM-mass cover into RAM. Each MMCE read() is one
// COMPLETE mmceman FS RPC
// over the shared SIO2 channel (command handshake + bulk DMA + trailer + a card-side seek/read-ahead
// prime), and mmceman has NO read-size cap (its length field is a full 32 bits), so a big chunk is
// served in ONE RPC. At 4 KB a ~100 KB cover cost ~25 RPCs; 32 KB serves it in ~4 -- an ~8x cut in
// that fixed per-RPC overhead (the actual cause of slow MMCE art), moving the same bytes and keeping
// the card's sequential read-ahead saturated. This is the same "few large reads" the in-game mmcedrv
// block driver uses. Keep chunks at 32 KB so an abort reaches a checkpoint comfortably inside the
// MMCE launch/theme drain budgets. A one-shot getStat+single-read is rejected for the same reason.
#define TEX_MMCE_STAGE_READ_SIZE 32768

// Hard cap on the staged payload. Legitimate art is <= a few MB even at 1080p; anything bigger is
// corrupt or misplaced content, and letting the doubling buffer chase it would eat the 32 MB EE
// RAM before malloc can fail (CodeRabbit review of #306 -- vetted, accepted).
#define TEX_STAGE_MAX_SIZE (8 * 1024 * 1024)

typedef struct
{
    int id;
    char *name;
    void **texture;
} texture_t;

typedef struct
{
    u8 red;
    u8 green;
    u8 blue;
    u8 alpha;
} png_clut_t;

typedef struct
{
    png_colorp palette;
    int numPalette;
    int numTrans;
    png_bytep trans;
} png_texture_t;

static png_texture_t pngTexture;
static volatile int *gTexAbortFlag;
static int gTexAbortThreadId = -1;

static texture_t internalDefault[TEXTURES_COUNT] = {
    {LOAD0_ICON, "load0", &load0_png},
    {LOAD1_ICON, "load1", &load1_png},
    {LOAD2_ICON, "load2", &load2_png},
    {LOAD3_ICON, "load3", &load3_png},
    {LOAD4_ICON, "load4", &load4_png},
    {LOAD5_ICON, "load5", &load5_png},
    {LOAD6_ICON, "load6", &load6_png},
    {LOAD7_ICON, "load7", &load7_png},
    {BDM_ICON, "usb", &usb_png},
    {USB_ICON, "usb_bd", &usb_bd_png},
    {ILINK_ICON, "ilk_bd", &ilk_bd_png},
    {MX4SIO_ICON, "m4s_bd", &m4s_bd_png},
    {HDD_BD_ICON, "hdd_bd", &hdd_bd_png},
    {MMCE_ICON, "mmce", &mmce_png},
    {HDD_ICON, "hdd", &hdd_png},
    {ETH_ICON, "eth", &eth_png},
    {UDP_ICON, "udp_bd", &udp_bd_png},
    {APP_ICON, "app", &app_png},
    {INDEX_0, "Index_0", &Index_0_png},
    {INDEX_1, "Index_1", &Index_1_png},
    {INDEX_2, "Index_2", &Index_2_png},
    {INDEX_3, "Index_3", &Index_3_png},
    {INDEX_4, "Index_4", &Index_4_png},
    {LEFT_ICON, "left", &left_png},
    {RIGHT_ICON, "right", &right_png},
    {CROSS_ICON, "cross", &cross_png},
    {TRIANGLE_ICON, "triangle", &triangle_png},
    {CIRCLE_ICON, "circle", &circle_png},
    {SQUARE_ICON, "square", &square_png},
    {SELECT_ICON, "select", &select_png},
    {START_ICON, "start", &start_png},
    /* currently unused.
    {UP_ICON, "up", &up_png},
    {DOWN_ICON, "down", &down_png},
    {L1_ICON, "L1", &L1_png},
    {L2_ICON, "L2", &L2_png},
    {R1_ICON, "R1", &R1_png},
    {R2_ICON, "R2", &R2_png}, */
    {L3_ICON, "L3", &L3_png},
    {R3_ICON, "R3", &R3_png},
    {FAV_ICON, "fav", &fav_png},
    {FAV_MARK, "fav_mark", &fav_mark_png},
    {SETTINGS_BG, "settings_bg", NULL}, // NULL: no embedded default yet -- theme-supplied via use_settings_bg
    {COVER_DEFAULT, "cover", &cover_png},
    {DISC_DEFAULT, "disc", &disc_png},
    {SCREEN_DEFAULT, "screen", &screen_png},
    {INCEBTION_PICTURE, "incebtion", &incebtion_png}, // default theme background (conf_theme_OPL.cfg: default=incebtion)
    {IP_PICTURE, "ip", &ip_png},
    {COVERAPP_DEFAULT, "coverapp", &coverapp_png},
    {MISSING_PICTURE, "missing", &missing_png},
    // no_Device fallback dropped with the device indicator; not embedded.
    {NO_DEVICE_PICTURE, "no_Device", NULL},
    {NO_RATING_PICTURE, "no_Rating", &Rating_0_png}, // byte-identical to Rating_0: share the embedded blob (name kept for theme default=no_Rating + disk override)
    {SCREENS_OVERLAY, "screens", &screens_png},
    {ELF_FORMAT, "ELF", &ELF_png},
    {HDL_FORMAT, "HDL", &HDL_png},
    {ISO_FORMAT, "ISO", &ISO_png},
    {VCD_FORMAT, "VCD", &VCD_png},
    {ZSO_FORMAT, "ZSO", &ZSO_png},
    {UL_FORMAT, "UL", &UL_png},
    {APP_MEDIA, "APP", &APPS_png},
    {CD_MEDIA, "CD", &CD_png},
    {DVD_MEDIA, "DVD", &DVD_png},
    {ASPECT_STD, "Aspect_s", &Aspect_s_png},
    {ASPECT_WIDE, "Aspect_w", &Aspect_w_png},
    {ASPECT_WIDE1, "Aspect_w1", &Aspect_w1_png},
    {ASPECT_WIDE2, "Aspect_w2", &Aspect_w2_png},
    // Device-indicator icons removed from the embedded themes' info page, so no longer
    // embedded (dropped from PNG_ASSETS). Enum + name kept (texId is positional), data NULL.
    {DEVICE_1, "Device_1", NULL},
    {DEVICE_2, "Device_2", NULL},
    {DEVICE_3, "Device_3", NULL},
    {DEVICE_4, "Device_4", NULL},
    {DEVICE_5, "Device_5", NULL},
    {DEVICE_6, "Device_6", NULL},
    {DEVICE_ALL, "Device_all", NULL},
    {RATING_0, "Rating_0", &Rating_0_png},
    {RATING_1, "Rating_1", &Rating_1_png},
    {RATING_2, "Rating_2", &Rating_2_png},
    {RATING_3, "Rating_3", &Rating_3_png},
    {RATING_4, "Rating_4", &Rating_4_png},
    {RATING_5, "Rating_5", &Rating_5_png},
    {SCAN_240P, "Scan_240p", &Scan_240p_png},
    {SCAN_240P1, "Scan_240p1", &Scan_240p1_png},
    {SCAN_480I, "Scan_480i", &Scan_480i_png},
    {SCAN_480P, "Scan_480p", &Scan_480p_png},
    {SCAN_480P1, "Scan_480p1", &Scan_480p1_png},
    {SCAN_480P2, "Scan_480p2", &Scan_480p2_png},
    {SCAN_480P3, "Scan_480p3", &Scan_480p3_png},
    {SCAN_480P4, "Scan_480p4", &Scan_480p4_png},
    {SCAN_480P5, "Scan_480p5", &Scan_480p5_png},
    {SCAN_576I, "Scan_576i", &Scan_576i_png},
    {SCAN_576P, "Scan_576p", &Scan_576p_png},
    {SCAN_720P, "Scan_720p", &Scan_720p_png},
    {SCAN_1080I, "Scan_1080i", &Scan_1080i_png},
    {SCAN_1080I2, "Scan_1080i2", &Scan_1080i2_png},
    {SCAN_1080P, "Scan_1080p", &Scan_1080p_png},
    {VMODE_MULTI, "Vmode_multi", &Vmode_multi_png},
    {VMODE_NTSC, "Vmode_ntsc", &Vmode_ntsc_png},
    {VMODE_PAL, "Vmode_pal", &Vmode_pal_png},
    {LOGO_PICTURE, "logo", &logo_png},
    // Animated boot-logo frames (embedded build assets, gfx/logo0.png..logo6.png).
    {LOGO0_PICTURE, "logo0", &logo0_png},
    {LOGO1_PICTURE, "logo1", &logo1_png},
    {LOGO2_PICTURE, "logo2", &logo2_png},
    {LOGO3_PICTURE, "logo3", &logo3_png},
    // logo4/5/6 are byte-identical to logo3/2/1 (the boot animation ping-pongs 0..3..0):
    // share the embedded blob -- names kept so a disk theme can still override by filename.
    {LOGO4_PICTURE, "logo4", &logo3_png},
    {LOGO5_PICTURE, "logo5", &logo2_png},
    {LOGO6_PICTURE, "logo6", &logo1_png},
    {CASE_OVERLAY, "case", &case_png},
    // apps_case retired -- both built-in themes now use case + case_overlay; aliased to case so an
    // external theme that still names overlay=apps_case keeps a frame (no separate bitmap embedded).
    {APPS_CASE_OVERLAY, "apps_case", &case_png},
    {PS1_SYSTEM, "PS1", &PS1_png}, // #System console glyphs (FR #49); names match the #System values
    {PS2_SYSTEM, "PS2", &PS2_png},
    {CASE_OVERLAY2, "case_overlay", &case_overlay_png}, // b2 foliage layer (two-layer frame)
    {UDPFS_ICON, "udp_fs", &udp_fs_png},
};

int texLookupInternalTexId(const char *name)
{
    int i;
    int result = -1;

    for (i = 0; i < TEXTURES_COUNT; i++) {
        if (!strcmp(name, internalDefault[i].name)) {
            result = internalDefault[i].id;
            break;
        }
    }

    return result;
}

static int texSizeValidate(int width, int height, u8 psm)
{
    if (width > 1024 || height > 1024)
        return -1;

    if (gsKit_texture_size(width, height, (int)psm) > maxSize)
        return -1;

    return 0;
}

static void texPrepare(GSTEXTURE *texture)
{
    texture->Width = 0;                              // Must be set by loader
    texture->Height = 0;                             // Must be set by loader
    texture->PSM = GS_PSM_CT24;                      // Must be set by loader
    texture->ClutPSM = 0;                            // Default, can be set by loader
    texture->TBW = 0;                                // gsKit internal value
    texture->Mem = NULL;                             // Must be allocated by loader
    texture->Clut = NULL;                            // Default, can be set by loader
    texture->Vram = 0;                               // VRAM allocation handled by texture manager
    texture->VramClut = 0;                           // VRAM allocation handled by texture manager
    texture->Filter = GS_FILTER_LINEAR;              // Default
    texture->ClutStorageMode = GS_CLUT_STORAGE_CSM1; // Default
    // Do not load the texture to VRAM directly, only load it to EE RAM
    texture->Delayed = 1;
}

void texFree(GSTEXTURE *texture)
{
    if (texture->Mem) {
        free(texture->Mem);
        texture->Mem = NULL;
    }
    if (texture->Clut) {
        free(texture->Clut);
        texture->Clut = NULL;
    }
}

typedef struct
{
    int fd;
    int position;
    int length;
    unsigned char buffer[4096];
} tex_file_reader_t;

static int texEnd(png_structp pngPtr, png_infop infoPtr, void *pFileBuffer, int fd, int status)
{
    if (pFileBuffer != NULL)
        free(pFileBuffer);

    if (infoPtr != NULL)
        png_destroy_read_struct(&pngPtr, &infoPtr, (png_infopp)NULL);

    if (fd >= 0)
        close(fd);

    return status;
}

static int texLoadAbortRequested(void)
{
    return gTexAbortFlag != NULL && gTexAbortThreadId == GetThreadId() && *gTexAbortFlag != 0;
}

void texSetLoadAbortFlag(volatile int *abortRequested)
{
    gTexAbortFlag = abortRequested;
    gTexAbortThreadId = abortRequested != NULL ? GetThreadId() : -1;
}

static void texReadMemFunction(png_structp pngPtr, png_bytep data, png_size_t length)
{
    void **PngBufferPtr = png_get_io_ptr(pngPtr);

    memcpy(data, *PngBufferPtr, length);
    *PngBufferPtr = (u8 *)(*PngBufferPtr) + length;
}

// Bounded variant for decoding a PNG out of a fixed-size RAM buffer (e.g. a .tar art entry, which is
// user-supplied and may be truncated/corrupt). Unlike texReadMemFunction it refuses to read past the
// buffer: an over-read png_errors -> longjmp into texLoadAll's setjmp cleanup, which frees the texture.
typedef struct
{
    const u8 *ptr;
    u32 remaining;
} tex_mem_bounded_t;

static void texReadMemBoundedFunction(png_structp pngPtr, png_bytep data, png_size_t length)
{
    tex_mem_bounded_t *r = png_get_io_ptr(pngPtr);

    if (length > r->remaining)
        png_error(pngPtr, "png read past buffer");

    memcpy(data, r->ptr, length);
    r->ptr += length;
    r->remaining -= (u32)length;
}

static void texReadFileFunction(png_structp pngPtr, png_bytep data, png_size_t length)
{
    tex_file_reader_t *reader = png_get_io_ptr(pngPtr);

    while (length > 0) {
        int available;
        int chunk;

        if (texLoadAbortRequested())
            png_error(pngPtr, "png read aborted");

        if (reader->position >= reader->length) {
            reader->length = read(reader->fd, reader->buffer, sizeof(reader->buffer));
            reader->position = 0;

            if (reader->length <= 0)
                png_error(pngPtr, "png read failed");
        }

        available = reader->length - reader->position;
        chunk = available < (int)length ? available : (int)length;
        memcpy(data, &reader->buffer[reader->position], chunk);

        data += chunk;
        length -= chunk;
        reader->position += chunk;
    }
}

// Does a failed staged open mean "the art is not there" (stop asking) or "this attempt failed"
// (worth another try)?
//
// MMCE is the only device where the question has a trustworthy answer. mmceman used to return one
// bare -1 for six different failures -- fd-pool exhausted, three packet timeouts, a garbled reply,
// AND the card's genuine not-found -- so a contention storm branded every browsed game's art
// nonexistent for the session; its paired patch makes ENOENT mean specifically not-found, which is
// what the transient lane was built for.
//
// BDM/USB gets no such guarantee and never did. ps2sdk's bdmfs_fatfs returns the NEGATED FatFs
// result rather than an errno (fs_driver.c: `ret = -ret;`), so the value that reaches errno is a
// FatFs code wearing an errno's clothes -- reading it as ENOENT, EINTR or anything else is
// guesswork about a mapping neither side promises. Do what the hardware-approved rebuild-66 did on
// EVERY device: a failed open means no art. The asymmetry of being wrong decides it -- calling
// present art absent costs one placeholder until the next list rebuild clears the memo, while
// calling absent art transient costs repeated reads of a file that does not exist, on the very
// device the user is browsing, for most of a library (most games ship no cover at all).
static int texStagedOpenIsAbsence(const char *filePath, int err)
{
    if (filePath != NULL && !strncmp(filePath, "mmce", 4))
        return err == ENOENT;

    return 1;
}

static int texShouldUseMemoryReader(const char *filePath)
{
    // #296 baseline restore: stage BDM USB/MX4SIO art ("massN:" and the bare "mass:") whole-file
    // into RAM too, not just MMCE. uOPL/official read each cover in ONE transfer; our streaming
    // reader pays one read() RPC per small chunk DURING decode (texReadFileFunction), so USB
    // covers decode at chunk granularity. One staged read per cover is uOPL's single biggest USB
    // win. HDD (pfs:) and ETH (smb:) keep the streaming reader: different perf profiles, no
    // evidence staging helps there.
    return filePath != NULL && (!strncmp(filePath, "mmce0:", 6) || !strncmp(filePath, "mmce1:", 6) || !strncmp(filePath, "mass", 4));
}

static int texStageExternalFileIntoMemory(int fd, void **buffer, u32 *stagedSize, int shortReadIsEof)
{
    int bytesRead = 0;
    int capacity;
    unsigned char *fileBuffer;

    if (buffer == NULL || stagedSize == NULL)
        return ERR_FILE_IO;

    // FAST PATH -- what official OPL does, and what the loop below cannot do on MMCE. If the device
    // can report a size, take it: allocate exactly that and read it in ONE call. The chunked loop
    // costs several read() round trips per cover plus a realloc-and-copy every time the doubling
    // buffer grows, and on USB those round trips are the expensive part. shortReadIsEof marks the
    // MMCE arm, whose driver does not support SEEK_END (returning <= 0) -- it keeps the loop, which
    // is the reason the loop exists at all. Any device that answers the seek gets the cheap path,
    // and one that answers with something implausible falls through to the loop untouched.
    if (!shortReadIsEof) {
        int size = lseek(fd, 0, SEEK_END);
        if (lseek(fd, 0, SEEK_SET) == 0 && size > 0 && size <= TEX_STAGE_MAX_SIZE) {
            fileBuffer = (unsigned char *)malloc(size);
            if (fileBuffer != NULL) {
                int got = read(fd, fileBuffer, size);
                if (got == size) {
                    *buffer = fileBuffer;
                    *stagedSize = (u32)size;
                    return 0;
                }
                // Short or failed read: fall back to the loop rather than guess. Rewind first, or
                // the retry would resume mid-file and stage a truncated PNG.
                free(fileBuffer);
                if (lseek(fd, 0, SEEK_SET) != 0)
                    return ERR_FILE_IO;
            }
        } else if (lseek(fd, 0, SEEK_SET) != 0) {
            return ERR_FILE_IO; // could not get back to the start; the loop below would read garbage
        }
    }

    // Do NOT size the file with lseek(SEEK_END) first: the MMCE newlib device (mmceman) does not
    // support SEEK_END, so it returned <= 0 and EVERY MMCE cover failed here with ERR_BAD_FILE (ISO
    // and VCD alike) while USB -- which streamed via read() only -- worked. Grow a heap buffer as we
    // read the file sequentially (read()-only, the same access the old USB streaming reader uses)
    // and stop at EOF, so no up-front file length is needed. We keep the bulk-into-RAM staging
    // (the reason this path exists: MMCE streaming reads during decode are unreliable, and USB
    // streaming pays one read() RPC per chunk -- #296) -- only the size probe changes.
    capacity = TEX_MMCE_STAGE_READ_SIZE * 2; // 64 KB; doubles on demand for larger covers
    fileBuffer = malloc(capacity);
    if (fileBuffer == NULL)
        return ERR_FILE_IO;

    for (;;) {
        int result;

        if (texLoadAbortRequested()) {
            free(fileBuffer);
            return ERR_LOAD_ABORTED;
        }

        if (capacity - bytesRead < TEX_MMCE_STAGE_READ_SIZE) {
            unsigned char *grown;

            if (capacity >= TEX_STAGE_MAX_SIZE) {
                // Over the staged-payload cap: not art anyone ships, and not transient.
                LOG("texStage: refusing oversized art file (> %d bytes staged)\n", TEX_STAGE_MAX_SIZE);
                free(fileBuffer);
                return ERR_BAD_FILE;
            }

            grown = realloc(fileBuffer, capacity * 2);
            if (grown == NULL) {
                free(fileBuffer);
                return ERR_FILE_IO;
            }
            fileBuffer = grown;
            capacity *= 2;
        }

        result = read(fd, fileBuffer + bytesRead, TEX_MMCE_STAGE_READ_SIZE);
        if (result < 0) {
            free(fileBuffer);
            return ERR_FILE_IO; // read() error (e.g. contended-bus EIO) -- the file EXISTS, retry later
        }
        bytesRead += result;

        if (result == 0)
            break; // real EOF, every device

        // For MMCE a SHORT read also means EOF -- break without the extra read()==0 RPC (and the
        // buffer realloc it would trigger). This is safe ONLY because of how mmceman serves an FS
        // read (verified against ps2-mmce/mmceman mmce_fs_read + mmce_sio2_rx_mixed @ db3e93f0):
        // the card always transfers the FULL requested size on the wire and reports the valid byte
        // count separately, and any mid-transfer stall returns -1 (handled above) -- never a
        // partial positive count. So result<SIZE is only ever the card's file running out, i.e.
        // EOF; there is no "short now, more later" case. Non-mmce readers (BDM mass, gated by the
        // caller's shortReadIsEof) make no such guarantee, so they keep reading until the
        // result==0 EOF above instead of breaking on a short count.
        if (shortReadIsEof && result < TEX_MMCE_STAGE_READ_SIZE)
            break;
    }

    if (bytesRead == 0) {
        free(fileBuffer);
        return ERR_FILE_IO; // empty file -> present but not a valid PNG (don't memoize as absent)
    }

    *buffer = fileBuffer;
    *stagedSize = (u32)bytesRead;
    return 0;
}

static void texPrepareClut(GSTEXTURE *texture, int clutEntries)
{
    png_clut_t *clut = (png_clut_t *)texture->Clut;
    int paletteEntries = pngTexture.numPalette < clutEntries ? pngTexture.numPalette : clutEntries;

    memset(&clut[paletteEntries], 0, (clutEntries - paletteEntries) * sizeof(clut[0]));

    for (int i = 0; i < paletteEntries; i++) {
        clut[i].red = pngTexture.palette[i].red;
        clut[i].green = pngTexture.palette[i].green;
        clut[i].blue = pngTexture.palette[i].blue;
        clut[i].alpha = (i < pngTexture.numTrans) ? (pngTexture.trans[i] >> 1) : 0x80;
    }

    if (clutEntries == 256) {
        for (int i = 0; i < paletteEntries; i++) {
            if ((i & 0x18) == 8) {
                png_clut_t tmp = clut[i];
                clut[i] = clut[i + 8];
                clut[i + 8] = tmp;
            }
        }
    }
}

static void texReadPixels4Row(GSTEXTURE *texture, png_bytep rowData, int row)
{
    int rowBytes = texture->Width / 2;
    unsigned char *pixel = (unsigned char *)texture->Mem + (row * rowBytes);

    memcpy(pixel, rowData, rowBytes);

    for (int i = 0; i < rowBytes; i++)
        pixel[i] = (pixel[i] << 4) | (pixel[i] >> 4);
}

static void texReadPixels8Row(GSTEXTURE *texture, png_bytep rowData, int row)
{
    unsigned char *pixel = (unsigned char *)texture->Mem + (row * texture->Width);
    memcpy(pixel, rowData, texture->Width);
}

static void texReadPixels24Row(GSTEXTURE *texture, png_bytep rowData, int row)
{
    struct pixel3
    {
        unsigned char r, g, b;
    };
    struct pixel3 *pixels = (struct pixel3 *)texture->Mem + (row * texture->Width);

    for (int i = 0; i < texture->Width; i++)
        memcpy(&pixels[i], &rowData[4 * i], 3);
}

static void texReadPixels32Row(GSTEXTURE *texture, png_bytep rowData, int row)
{
    struct pixel
    {
        unsigned char r, g, b, a;
    };
    struct pixel *pixels = (struct pixel *)texture->Mem + (row * texture->Width);

    for (int i = 0; i < texture->Width; i++) {
        memcpy(&pixels[i], &rowData[4 * i], 3);
        pixels[i].a = rowData[4 * i + 3] >> 1;
    }
}

static int texReadData(GSTEXTURE *texture, png_structp pngPtr, png_infop infoPtr, int passes,
                       png_bytep volatile *bufSlot,
                       void (*texPngReadRow)(GSTEXTURE *texture, png_bytep rowData, int row))
{
    int rowBytes = png_get_rowbytes(pngPtr, infoPtr);
    size_t size = gsKit_texture_size_ee(texture->Width, texture->Height, texture->PSM);
    png_bytep rowBuffer;

    texture->Mem = memalign(128, size);

    if (!texture->Mem) {
        texFree(texture); // free the already-allocated paletted-PNG Clut before bailing (Gemini review);
                          // matches the rowBuffer-OOM path just below
        LOG("TEXTURES PngReadData: Failed to allocate %d bytes\n", size);
        return ERR_FILE_IO; // OOM mid-decode: the file EXISTS, this is transient -- don't memoize as absent
    }

    // Every decode buffer is mirrored into *bufSlot (the caller's setjmp-frame slot) so a
    // png_read_row longjmp on corrupt data cannot leak it, and cleared again before local frees.
    if (passes > 1) {
        // Adam7-interlaced: libpng's pass merge ("sparkle" mode) revisits every row on each of the 7
        // passes, so all rows must stay resident -- buffer the whole raw image, let libpng merge into
        // it, then convert row-by-row. Transient rowBytes*Height (~188 KB for a 184x256 cover), freed
        // before return; the common non-interlaced case keeps the single-row streaming loop below.
        png_bytep raw = malloc((size_t)rowBytes * texture->Height);
        if (!raw) {
            texFree(texture);
            LOG("TEXTURES PngReadData: Failed to allocate interlaced buffer (%d x %d)\n", rowBytes, texture->Height);
            return ERR_FILE_IO; // OOM mid-decode (see above)
        }
        *bufSlot = raw;
        for (int pass = 0; pass < passes; pass++) {
            for (int row = 0; row < texture->Height; row++) {
                if (texLoadAbortRequested()) {
                    *bufSlot = NULL;
                    free(raw);
                    texFree(texture);
                    return ERR_LOAD_ABORTED;
                }
                png_read_row(pngPtr, raw + (size_t)row * rowBytes, NULL);
            }
        }
        for (int row = 0; row < texture->Height; row++)
            texPngReadRow(texture, raw + (size_t)row * rowBytes, row);
        *bufSlot = NULL;
        free(raw);
        png_read_end(pngPtr, NULL);
        return 0;
    }

    rowBuffer = malloc(rowBytes);
    if (!rowBuffer) {
        texFree(texture);
        LOG("TEXTURES PngReadData: Failed to allocate memory for PNG row\n");
        return ERR_FILE_IO; // OOM mid-decode (see above)
    }
    *bufSlot = rowBuffer;

    for (int row = 0; row < texture->Height; row++) {
        if (texLoadAbortRequested()) {
            *bufSlot = NULL;
            free(rowBuffer);
            texFree(texture);
            return ERR_LOAD_ABORTED;
        }

        png_read_row(pngPtr, rowBuffer, NULL);
        texPngReadRow(texture, rowBuffer, row);
    }

    *bufSlot = NULL;
    free(rowBuffer);
    png_read_end(pngPtr, NULL);

    return 0;
}

static int texLoadAll(GSTEXTURE *texture, const char *filePath, int texId, const void *memBuf, u32 memSize)
{
    int fd = -1;
    int bitDepth, colorType, interlaceType;
    int result;
    png_structp pngPtr = NULL;
    png_infop infoPtr = NULL;
    png_voidp readData = NULL;
    png_rw_ptr readFunction = NULL;
    png_uint_32 pngWidth, pngHeight;
    tex_file_reader_t fileReader;
    tex_mem_bounded_t memReader;
    void *PngFileBufferPtr;
    void *pFileBuffer = NULL;
    void (*texPngReadRow)(GSTEXTURE * texture, png_bytep rowData, int row);
    // The decode buffer texReadData allocates (streaming rowBuffer OR the Adam7 whole-image buffer).
    // Owned by THIS frame so the setjmp handler can free it: png_read_row longjmps here on corrupt
    // data, which would otherwise skip texReadData's local free and leak the buffer -- up to
    // rowBytes*Height for an interlaced file, one leak per corrupt PNG (CodeRabbit review of #247;
    // the streaming rowBuffer had the same pre-existing leak). volatile: written between setjmp and
    // longjmp, read after.
    png_bytep volatile decodeBuf = NULL;

    texPrepare(texture);

    if (memBuf) {
        // Decode straight from a RAM buffer (e.g. a .tar art entry) via the bounded memory reader.
        memReader.ptr = (const u8 *)memBuf;
        memReader.remaining = memSize;
        readData = &memReader;
        readFunction = &texReadMemBoundedFunction;
    } else if (filePath) {
        if (texShouldUseMemoryReader(filePath)) {
            errno = 0;
            fd = open(filePath, O_RDONLY, 0);
            if (fd < 0) {
                // Memoize as PERMANENTLY absent (ERR_BAD_FILE -> the fail-epoch memo) only on a real
                // ENOENT. This is the MMCE/USB staging arm, and mmceman used to return ONE bare -1
                // for six different open failures -- fd-pool exhausted, three packet timeouts, a
                // garbled reply, AND the card's genuine not-found -- so a rapid-nav contention storm
                // branded every browsed game's art "nonexistent" for the whole session (HW batch S3:
                // art dies, never recovers). The paired mmceman patch makes the driver return
                // -ENOENT only for the card's explicit not-found reply; every other failure lands in
                // the transient lane (ERR_FILE_IO, retried after a short delay) so
                // art self-heals when the bus quiets. The same self-heal applies to BDM USB (#296):
                // a contended-bus open failure there must not brand the cover absent either. If the
                // newlib glue ever maps errno unfaithfully, the degradation is a delayed
                // re-probe -- never a permanent failure. The generic loose-file
                // branch below keeps its unconditional ERR_BAD_FILE (HDD/ETH semantics unchanged).
                LOG("texLoadAll: staged art open failed: %s (errno %d)\n", filePath, errno);
                return texStagedOpenIsAbsence(filePath, errno) ? ERR_BAD_FILE : ERR_FILE_IO;
            }

            // mmceman's short-read==EOF guarantee (see the staging function) is MMCE-only; BDM
            // mass must stage until a real result==0 EOF.
            u32 stagedSize = 0;
            result = texStageExternalFileIntoMemory(fd, &pFileBuffer, &stagedSize, !strncmp(filePath, "mmce", 4));

            close(fd);
            fd = -1;
            if (result < 0) {
                LOG("texLoadAll: failed to stage file %s\n", filePath);
                return result;
            }
            // Decode through the BOUNDED memory reader, not texReadMemFunction's raw memcpy: the
            // staged buffer is user-supplied media and may be truncated/corrupt, and libpng keeps
            // requesting bytes until it has a full stream -- an unbounded reader would walk past
            // the staged allocation (CodeRabbit review of #306; pre-existing on the MMCE arm since
            // the staging path landed, this PR only widened the exposure to USB). Over-read here
            // png_errors -> the setjmp cleanup below frees the texture, same as the tar arm.
            memReader.ptr = (const u8 *)pFileBuffer;
            memReader.remaining = stagedSize;
            readData = &memReader;
            readFunction = &texReadMemBoundedFunction;
        } else {
            fd = open(filePath, O_RDONLY, 0);
            if (fd < 0) {
                // Diagnostic (debug builds only): make a stale/mis-keyed ART folder self-evident.
                // The "no art on HDD" report is usually content -- grep this path to compare the active
                // PS2 startup or VCD filename/ID key with the files on the current OPL data mount.
                LOG("texLoadAll: art file not found: %s\n", filePath);
                return ERR_BAD_FILE;
            }

            memset(&fileReader, 0, sizeof(fileReader));
            fileReader.fd = fd;
            readData = &fileReader;
            readFunction = &texReadFileFunction;
        }
    } else {
        if (texId == -1 || !internalDefault[texId].texture)
            return ERR_BAD_FILE;

        PngFileBufferPtr = internalDefault[texId].texture;
        readData = &PngFileBufferPtr;
        readFunction = &texReadMemFunction;
    }

    pngPtr = png_create_read_struct(PNG_LIBPNG_VER_STRING, (png_voidp)NULL, NULL, NULL);
    if (!pngPtr)
        return texEnd(pngPtr, infoPtr, pFileBuffer, fd, ERR_READ_STRUCT);

    infoPtr = png_create_info_struct(pngPtr);
    if (!infoPtr)
        return texEnd(pngPtr, infoPtr, pFileBuffer, fd, ERR_INFO_STRUCT);

    if (setjmp(png_jmpbuf(pngPtr))) {
        /* Always free texture->Mem / texture->Clut on any longjmp (decode error or abort).
           Capture the abort flag once to avoid a TOCTOU between the texFree and the return. */
        int aborted = texLoadAbortRequested();
        if (decodeBuf != NULL)
            free((void *)decodeBuf); // texReadData's in-flight decode buffer (see declaration above)
        texFree(texture);
        return texEnd(pngPtr, infoPtr, pFileBuffer, fd, aborted ? ERR_LOAD_ABORTED : ERR_SET_JMP);
    }

    png_set_read_fn(pngPtr, readData, readFunction);
    png_set_sig_bytes(pngPtr, 0);
    png_read_info(pngPtr, infoPtr);

    png_get_IHDR(pngPtr, infoPtr, &pngWidth, &pngHeight, &bitDepth, &colorType, &interlaceType, NULL, NULL);
    if (bitDepth > 8)
        return texEnd(pngPtr, infoPtr, pFileBuffer, fd, ERR_BAD_DEPTH);

    texture->Width = pngWidth;
    texture->Height = pngHeight;

    if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA || bitDepth < 4) {
        png_set_expand(pngPtr);
        if (png_get_valid(pngPtr, infoPtr, PNG_INFO_tRNS))
            png_set_tRNS_to_alpha(pngPtr);
    }

    // Grayscale (1ch) and gray+alpha (2ch) PNGs must be promoted to RGB/RGBA: png_set_expand does NOT
    // change the color type for non-tRNS gray, so without this they fall through the color-type switch
    // below to ERR_BAD_DEPTH and fail to load (port of wOPL 8a1a583 / issue #225).
    if (colorType == PNG_COLOR_TYPE_GRAY || colorType == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(pngPtr);

    // Adam7-interlaced PNGs: libpng must be told to MERGE the seven progressive passes, and that merge
    // needs every row resident across passes -- the single-row streaming loop in texReadData cannot do
    // it. Register the handling here (libpng requires it BEFORE png_read_update_info) and hand the pass
    // count to texReadData, which buffers the whole image when passes > 1. Without this the seven
    // passes decode as sequential rows: the cover renders as a stack of smaller-to-bigger copies of
    // itself (maintainer HW report, 2026-07-21) -- a regression vs upstream's whole-image
    // png_read_image, introduced with the streamed row decode.
    int pngPasses = png_set_interlace_handling(pngPtr);

    png_set_filler(pngPtr, 0xff, PNG_FILLER_AFTER);
    png_read_update_info(pngPtr, infoPtr);

    switch (png_get_color_type(pngPtr, infoPtr)) {
        case PNG_COLOR_TYPE_RGB_ALPHA:
            texture->PSM = GS_PSM_CT32;
            texPngReadRow = &texReadPixels32Row;
            break;
        case PNG_COLOR_TYPE_RGB:
            texture->PSM = GS_PSM_CT24;
            texPngReadRow = &texReadPixels24Row;
            break;
        case PNG_COLOR_TYPE_PALETTE:
            pngTexture.palette = NULL;
            pngTexture.numPalette = 0;
            pngTexture.trans = NULL;
            pngTexture.numTrans = 0;

            png_get_PLTE(pngPtr, infoPtr, &pngTexture.palette, &pngTexture.numPalette);
            png_get_tRNS(pngPtr, infoPtr, &pngTexture.trans, &pngTexture.numTrans, NULL);
            texture->ClutPSM = GS_PSM_CT32;

            if (bitDepth == 4) {
                texture->PSM = GS_PSM_T4;
                texture->Clut = memalign(128, gsKit_texture_size_ee(8, 2, GS_PSM_CT32));
                if (texture->Clut == NULL)
                    return texEnd(pngPtr, infoPtr, pFileBuffer, fd, ERR_FILE_IO); // Clut OOM: file present -> transient, not absent
                memset(texture->Clut, 0, gsKit_texture_size_ee(8, 2, GS_PSM_CT32));
                texPrepareClut(texture, 16);
                texPngReadRow = &texReadPixels4Row;
            } else if (bitDepth == 8) {
                texture->PSM = GS_PSM_T8;
                texture->Clut = memalign(128, gsKit_texture_size_ee(16, 16, GS_PSM_CT32));
                if (texture->Clut == NULL)
                    return texEnd(pngPtr, infoPtr, pFileBuffer, fd, ERR_FILE_IO); // Clut OOM: file present -> transient, not absent
                memset(texture->Clut, 0, gsKit_texture_size_ee(16, 16, GS_PSM_CT32));
                texPrepareClut(texture, 256);
                texPngReadRow = &texReadPixels8Row;
            } else
                return texEnd(pngPtr, infoPtr, pFileBuffer, fd, ERR_BAD_DEPTH);
            break;
        default:
            return texEnd(pngPtr, infoPtr, pFileBuffer, fd, ERR_BAD_DEPTH);
    }

    if (texSizeValidate(texture->Width, texture->Height, texture->PSM) < 0) {
        texFree(texture);
        return texEnd(pngPtr, infoPtr, pFileBuffer, fd, ERR_BAD_DIMENSION);
    }

    result = texReadData(texture, pngPtr, infoPtr, pngPasses, &decodeBuf, texPngReadRow);
    return texEnd(pngPtr, infoPtr, pFileBuffer, fd, result);
}

static int texLoad(GSTEXTURE *texture, const char *filePath)
{
    return texLoadAll(texture, filePath, -1, NULL, 0);
}

int texLoadInternal(GSTEXTURE *texture, int texId)
{
    return texLoadAll(texture, NULL, texId, NULL, 0);
}

// Decode a PNG already resident in a RAM buffer (e.g. a .tar art entry pulled by tarGet). The buffer
// stays owned by the caller (free it after this returns); the decoded pixels are copied into texture.
int texLoadFromMemory(GSTEXTURE *texture, const void *buf, u32 size)
{
    return texLoadAll(texture, NULL, -1, buf, size);
}

int texDiscoverLoad(GSTEXTURE *texture, const char *path, int texId)
{
    char filePath[256];
    int result;


    LOG("texDiscoverLoad(%s)\n", path);

    if (texId != -1)
        snprintf(filePath, sizeof(filePath), "%s%s.%s", path, internalDefault[texId].name, "png");
    else
        snprintf(filePath, sizeof(filePath), "%s.%s", path, "png");

    result = texLoad(texture, filePath);
    return (result >= 0) ? 0 : result;
}
