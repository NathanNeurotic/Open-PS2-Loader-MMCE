/*
  Copyright 2026, Open-PS2-Loader contributors
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.
*/

#ifndef __RETROGEM_H
#define __RETROGEM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <tamtypes.h>
#include <stddef.h>

#define RETROGEM_GAMEID_MAX 12

/**
 * Extracts an 11-character PS1 Game ID (e.g., SLUS_000.01 or SLUS-00001) from a .VCD image file
 * using a 4-tier resolution ladder:
 *   1. ISO 9660 SYSTEM.CNF parsing
 *   2. PVD Volume Creation Timestamp lookup
 *   3. Partition / Subpath parsing
 *   4. Filename fallback
 *
 * @param vcdPath Full path to the .VCD file or partition item
 * @param gameID Output buffer for the 11-character Game ID (plus NUL terminator)
 * @param maxLen Size of the output buffer (must be at least 12 bytes)
 * @return 1 on successful extraction, 0 on failure/unresolved
 */
int retrogemGetVcdGameID(const char *vcdPath, char *gameID, size_t maxLen);

/**
 * Renders the RetroGEM optical barcode signal onto the GS framebuffer for the specified number
 * of frames immediately before ELF execution.
 *
 * @param gameID 11-character Game ID ASCII string
 * @param frames Number of frames to hold the barcode (typically 2)
 */
void displayRetroGemGameID(const char *gameID, int frames);

#ifdef __cplusplus
}
#endif

#endif // __RETROGEM_H
