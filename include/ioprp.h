unsigned int patch_IOPRP_image(void *ioprp_image, void *cdvdman_module, unsigned int size_cdvdman);

#ifdef RETROACHIEVEMENTS
unsigned int patch_IOPRP_image_disc_size(void);
unsigned int patch_IOPRP_image_disc(void *image);
#endif
