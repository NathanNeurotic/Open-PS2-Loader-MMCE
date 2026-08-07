#ifndef BDMA_EMBED_H
#define BDMA_EMBED_H

// Embedded BDMAssault variant driver pairs (modules/bdmassault, gzipped at build time -- see
// PROVENANCE.md there for bytes/licenses). Pasted to mc?:/POPSTARTER/ by vcdEquipBdma's
// final-fallback leg when no user-supplied copy exists on any seek-path device (maintainer
// directive 2026-07-21, POPSLoader parity). bdma_usbd_usb_gz serves BOTH the usbexfat and mx4sio
// modes (byte-identical upstream blobs, deduped -- keep in sync with PROVENANCE.md on re-vendor).
#include "include/extern_irx.h" // IMPORT_BIN2C

IMPORT_BIN2C(bdma_usbd_usb_gz);
IMPORT_BIN2C(bdma_usbhdfsd_usbexfat_gz);
IMPORT_BIN2C(bdma_usbhdfsd_mx4sio_gz);
IMPORT_BIN2C(bdma_usbd_mmce_gz);
IMPORT_BIN2C(bdma_usbhdfsd_mmce_gz);
IMPORT_BIN2C(bdma_usbd_ata_gz);
IMPORT_BIN2C(bdma_usbhdfsd_ata_gz);

#endif
