unsigned int patch_IOPRP_image(void *ioprp_image, void *cdvdman_module, unsigned int size_cdvdman);

/* RA disc mode: an IOPRP image that overrides EESYNC only, so the
   console's own CDVDMAN/CDVDFSV out of ROM keep serving cdrom0: -- the
   real disc in the tray. Returns the image size; ask
   raDiscIoprpMaxSize() how much to allocate first. */
unsigned int patch_IOPRP_image_disc(void *ioprp_image);
unsigned int patch_IOPRP_image_disc_size(void);
