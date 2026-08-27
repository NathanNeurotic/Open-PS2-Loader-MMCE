#include <kernel.h>
#include <string.h>

#include "include/ioprp.h"

struct romdir_entry
{
    char fileName[10];
    unsigned short int extinfo_size;
    unsigned int fileSize;
};

/* Pointers externally compiled modules */
extern unsigned char cdvdfsv_irx[];
extern unsigned int size_cdvdfsv_irx;

extern unsigned char eesync_irx[];
extern unsigned int size_eesync_irx;

extern unsigned char IOPRP_img[];
extern unsigned int size_IOPRP_img;

/* RA: a second IOPRP variant, for launching the disc in the tray.

   OPL always feeds UDNL an IOPRP image that overrides the console's
   CDVDMAN with its own, image-reading one -- that is how a game runs
   off USB or a share. For a real disc we want the opposite: leave the
   ROM's CDVDMAN and CDVDFSV alone, so cdrom0: keeps pointing at the
   drive, and override nothing but EESYNC.

   The IOP reset itself stays exactly as it is (ResetIopSpecial through
   imgdrv), so ee_core, its IOP modules and the patches all survive.
   This is the trick neutrino uses for its default -dvd=no mode; see
   ee/loader/src/ioprp.c there, ioprp_img_dvd. */

#define EXTINFO_TYPE_DATE    1
#define EXTINFO_TYPE_VERSION 2
#define EXTINFO_TYPE_COMMENT 3

struct extinfo_entry
{
    unsigned short int value; /* only used by the version type */
    unsigned char ext_length; /* bytes of data following this entry */
    unsigned char type;
};

struct ioprp_ext_disc
{
    struct extinfo_entry reset_date_ext;
    unsigned int reset_date;

    struct extinfo_entry eesync_date_ext;
    unsigned int eesync_date;
    struct extinfo_entry eesync_version_ext;
    struct extinfo_entry eesync_comment_ext;
    char eesync_comment[8];
};

struct ioprp_img_disc
{
    struct romdir_entry romdir[5];
    struct ioprp_ext_disc ext;
};

/* The ROMDIR entry describes the romdir array itself, which is why the
   array has to sit at the top of the image. EESYNC carries size 0 here:
   patch_EESYNC fills it in with the real module. */
static const struct ioprp_img_disc ioprp_img_disc = {
    {{"RESET", 8, 0},
     {"ROMDIR", 0, 0x10 * 5},
     {"EXTINFO", 0, sizeof(struct ioprp_ext_disc)},
     {"EESYNC", 24, 0},
     {"", 0, 0}},
    {{0, 4, EXTINFO_TYPE_DATE},
     0x20260827,
     {0, 4, EXTINFO_TYPE_DATE},
     0x20260827,
     {0x9999, 0, EXTINFO_TYPE_VERSION},
     {0, 8, EXTINFO_TYPE_COMMENT},
     "SyncEE"}};

/* Local function prototypes */
static unsigned int patch_image(void *ioprp_image, const unsigned char *base, void *cdvdman_module, unsigned int size_cdvdman);
static inline void patch_CDVDMAN(void *image_offset, struct romdir_entry *entryinfo, void *cdvdman, unsigned int size);
static inline void patch_CDVDFSV(void *image_offset, struct romdir_entry *entryinfo);
static inline void patch_EESYNC(void *image_offset, struct romdir_entry *entryinfo);
static inline void Align_offsets(void *base_address, unsigned int *offset_in, const struct romdir_entry *romdir_in, unsigned int *offset_out, struct romdir_entry *romdir_out);

/*----------------------------------------------------------------------------------------
    Replace modules in a IOPRP image.
------------------------------------------------------------------------------------------*/
unsigned int patch_IOPRP_image(void *ioprp_image, void *cdvdman_module, unsigned int size_cdvdman)
{
    return patch_image(ioprp_image, IOPRP_img, cdvdman_module, size_cdvdman);
}

/* RA: disc mode. No CDVDMAN/CDVDFSV in the template, so nothing to
   replace them with either. */
unsigned int patch_IOPRP_image_disc(void *ioprp_image)
{
    return patch_image(ioprp_image, (const unsigned char *)&ioprp_img_disc, NULL, 0);
}

unsigned int patch_IOPRP_image_disc_size(void)
{
    return sizeof(ioprp_img_disc) + size_eesync_irx + 256;
}

static unsigned int patch_image(void *ioprp_image, const unsigned char *base, void *cdvdman_module, unsigned int size_cdvdman)
{
    unsigned int offset_in, offset_out; /* For processing purposes */
    const struct romdir_entry *romdir_in;
    struct romdir_entry *romdir_out;

    offset_in = 0;
    offset_out = 0;

    romdir_in = (const struct romdir_entry *)base;
    romdir_out = (struct romdir_entry *)ioprp_image;

    while (romdir_in->fileName[0] != '\0') {
        memset(romdir_out, 0, sizeof(struct romdir_entry));

        if (!strcmp(romdir_in->fileName, "CDVDMAN") && cdvdman_module != NULL)
            patch_CDVDMAN((void *)((u8 *)ioprp_image + offset_out), romdir_out, cdvdman_module, size_cdvdman);
        else if (!strcmp(romdir_in->fileName, "CDVDFSV"))
            patch_CDVDFSV((void *)((u8 *)ioprp_image + offset_out), romdir_out);
        else if (!strcmp(romdir_in->fileName, "EESYNC"))
            patch_EESYNC((void *)((u8 *)ioprp_image + offset_out), romdir_out);
        else {                                                                                                       /* Other modules that should not be tampered with */
            memcpy((void *)((u8 *)ioprp_image + offset_out), (const void *)(base + offset_in), romdir_in->fileSize); /* Copy the file/entry's contents. */
            romdir_out->fileSize = romdir_in->fileSize;                                                              /* Copy the file size. */
        }

        /* For ALL modules */
        strcpy(romdir_out->fileName, romdir_in->fileName);                          /* Copy module name */
        romdir_out->extinfo_size = romdir_in->extinfo_size;                         /* Copy EXTINFO size */
        Align_offsets(ioprp_image, &offset_in, romdir_in, &offset_out, romdir_out); /* Align all addresses to a multiple of 16 */

        romdir_in++;
        romdir_out++;
    }

    return offset_out;
}

/*----------------------------------------------------------------------------------------
    Patch CDVDMAN in an IOPRP image
------------------------------------------------------------------------------------------*/
static inline void patch_CDVDMAN(void *image_offset, struct romdir_entry *entryinfo, void *cdvdman, unsigned int size)
{
    memcpy(image_offset, cdvdman, size);
    entryinfo->fileSize = size;
}

/*----------------------------------------------------------------------------------------
    Patch CDVDFSV in an IOPRP image
------------------------------------------------------------------------------------------*/
static inline void patch_CDVDFSV(void *image_offset, struct romdir_entry *entryinfo)
{
    memcpy(image_offset, cdvdfsv_irx, size_cdvdfsv_irx);
    entryinfo->fileSize = size_cdvdfsv_irx;
}

/*----------------------------------------------------------------------------------------
    Patch EESYNC in an IOPRP image
------------------------------------------------------------------------------------------*/
static inline void patch_EESYNC(void *image_offset, struct romdir_entry *entryinfo)
{
    memcpy(image_offset, eesync_irx, size_eesync_irx);
    entryinfo->fileSize = size_eesync_irx;
}

/*----------------------------------------------------------------------------------------
    Align offsets to multiples of 16, filling the gaps created with 0s.
------------------------------------------------------------------------------------------*/
static inline void Align_offsets(void *base_address, unsigned int *offset_in, const struct romdir_entry *romdir_in, unsigned int *offset_out, struct romdir_entry *romdir_out)
{
    /* For ALL modules; Align all addresses to a multiple of 16 */
    if (offset_in != NULL) {
        if ((romdir_in->fileSize & 0xF) == 0)
            *offset_in += romdir_in->fileSize;
        else
            *offset_in += (romdir_in->fileSize + 0xF) & ~0xF;
    }

    if (offset_out != NULL) {
        if ((romdir_out->fileSize & 0xF) == 0)
            *offset_out += romdir_out->fileSize;
        else { /* Fill the alignment gap with 0s */
            register unsigned int new_filesize = (romdir_out->fileSize + 0xF) & ~0xF;
            memset((void *)((u32)base_address + (*offset_out) + romdir_out->fileSize), 0, new_filesize - romdir_out->fileSize);
            *offset_out += new_filesize;
        }
    }
}
