/*
  Copyright 2026, Open-PS2-Loader contributors
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>

#include "include/opl.h"
#include "include/gui.h"
#include "include/renderman.h"
#include "include/vcdsupport.h"
#include "include/retrogem.h"

// Table of known PS1 disc PVD volume creation timestamps (16-byte ASCII) for discs that lack
// SYSTEM.CNF or boot generic targets like PSX.EXE.
typedef struct
{
    const char timestamp[17]; // 16-char ASCII timestamp + NUL
    const char game_id[12];   // 11-char Game ID + NUL
} ps1_generic_game_id_t;

static const ps1_generic_game_id_t ps1_generic_game_ids[] = {
    // Known PS1 generic volume creation timestamps
    {"1995121500000000", "SLUS_000.01"}, // Example entry template
    {"", ""}                             // Sentinel
};

// Check if a character is valid in a title ID (alpha, digit, '_', '.', '-')
static int isValidIdChar(char c)
{
    return (isalnum((unsigned char)c) || c == '_' || c == '.' || c == '-');
}

// Clean and normalize a string into an 11-char PS1 Game ID (e.g., SLUS_000.01 or SLUS-00001)
static int retrogemCleanTitleID(const char *raw, char *out, size_t maxLen)
{
    size_t i, len;
    char tmp[32];
    const char *p = raw;

    if (raw == NULL || out == NULL || maxLen < 12)
        return 0;
    out[0] = '\0';

    // Skip leading prefix (e.g. cdrom0:\ or cdrom:\ or XX. or SB.)
    if (!strncmp(p, "cdrom0:\\", 8))
        p += 8;
    else if (!strncmp(p, "cdrom:\\", 7))
        p += 7;
    else if (!strncmp(p, "XX.", 3) || !strncmp(p, "SB.", 3))
        p += 3;

    // Skip leading slashes or spaces
    while (*p == '\\' || *p == '/' || *p == ' ')
        p++;

    len = 0;
    while (p[len] != '\0' && p[len] != ';' && p[len] != ' ' && p[len] != '\r' && p[len] != '\n' && len < sizeof(tmp) - 1) {
        tmp[len] = p[len];
        len++;
    }
    tmp[len] = '\0';

    // Strip trailing .ELF or .VCD extension if present
    if (len >= 4 && (!strcasecmp(&tmp[len - 4], ".elf") || !strcasecmp(&tmp[len - 4], ".vcd"))) {
        tmp[len - 4] = '\0';
        len -= 4;
    }

    if (len < 8)
        return 0;

    // Standard PS1 ID shapes: AAAA_NNN.NN (11 chars) or AAAA-NNNNN (10 chars -> format to AAAA_NNN.NN or keep 11)
    if (len >= 11 && isValidIdChar(tmp[0]) && isValidIdChar(tmp[1]) && isValidIdChar(tmp[2]) && isValidIdChar(tmp[3])) {
        for (i = 0; i < 11 && tmp[i] != '\0'; i++)
            out[i] = tmp[i];
        out[i] = '\0';
    } else if (len == 10 && tmp[4] == '-') { // e.g. SLUS-00001 -> SLUS_000.01
        snprintf(out, maxLen, "%.4s_%c%c%c.%c%c", tmp, tmp[5], tmp[6], tmp[7], tmp[8], tmp[9]);
    } else {
        // Copy up to 11 valid characters
        for (i = 0; i < 11 && tmp[i] != '\0'; i++) {
            if (!isValidIdChar(tmp[i]))
                break;
            out[i] = tmp[i];
        }
        out[i] = '\0';
    }

    // Reject generic targets
    if (strlen(out) < 8 || !strcasecmp(out, "PSX.EXE") || !strcasecmp(out, "BOOT.EXE") || !strncmp(out, "???", 3))
        return 0;

    return 1;
}

// Tier 1 & 2 helper: ISO 9660 SYSTEM.CNF parsing and PVD Timestamp lookup
static int retrogemParseIsoSector16(FILE *f, char *gameID, size_t maxLen)
{
    unsigned char sector[2352];
    unsigned char pvd[2048];
    long pvdOffset = -1;
    int sectorSize = 2048;
    int modeOffset = 0;

    // Check Sector 16 for 2048-byte user data (offset 16 * 2048 = 32768)
    if (fseek(f, 32768, SEEK_SET) == 0 && fread(pvd, 1, 2048, f) == 2048) {
        if (pvd[0] == 0x01 && !memcmp(&pvd[1], "CD001", 5)) {
            pvdOffset = 32768;
            sectorSize = 2048;
            modeOffset = 0;
        }
    }

    // Check Sector 16 for 2352-byte RAW sectors (offset 16 * 2352 = 37632 + 16 header = 37648)
    if (pvdOffset < 0) {
        if (fseek(f, 37648, SEEK_SET) == 0 && fread(pvd, 1, 2048, f) == 2048) {
            if (pvd[0] == 0x01 && !memcmp(&pvd[1], "CD001", 5)) {
                pvdOffset = 37648;
                sectorSize = 2352;
                modeOffset = 16;
            }
        }
    }

    if (pvdOffset < 0)
        return 0; // Not a valid ISO 9660 PVD

    // --- Tier 1: Traverse Root Directory for SYSTEM.CNF ---
    // Root Directory Record at PVD offset + 156 (0x9C)
    unsigned int rootLba = (unsigned int)pvd[156 + 2] | ((unsigned int)pvd[156 + 3] << 8) |
                           ((unsigned int)pvd[156 + 4] << 16) | ((unsigned int)pvd[156 + 5] << 24);
    unsigned int rootLen = (unsigned int)pvd[156 + 10] | ((unsigned int)pvd[156 + 11] << 8) |
                           ((unsigned int)pvd[156 + 12] << 16) | ((unsigned int)pvd[156 + 13] << 24);

    long rootSectorOffset = (long)rootLba * sectorSize + modeOffset;
    if (rootLen > 0 && rootLen <= 32768 && fseek(f, rootSectorOffset, SEEK_SET) == 0) {
        unsigned char *dirBuf = (unsigned char *)malloc(rootLen);
        if (dirBuf) {
            if (fread(dirBuf, 1, rootLen, f) == rootLen) {
                size_t off = 0;
                while (off < rootLen) {
                    unsigned char recLen = dirBuf[off];
                    if (recLen == 0) {
                        // Advance to next sector boundary
                        off = ((off / sectorSize) + 1) * sectorSize;
                        continue;
                    }
                    if (off + recLen > rootLen)
                        break;

                    unsigned char nameLen = dirBuf[off + 32];
                    if (nameLen >= 10 && off + 33 + nameLen <= rootLen) {
                        char nameBuf[32];
                        size_t n = (nameLen < sizeof(nameBuf) - 1) ? nameLen : sizeof(nameBuf) - 1;
                        memcpy(nameBuf, &dirBuf[off + 33], n);
                        nameBuf[n] = '\0';

                        if (!strncasecmp(nameBuf, "SYSTEM.CNF", 10)) {
                            // Located SYSTEM.CNF! Get its LBA & size
                            unsigned int sysLba = (unsigned int)dirBuf[off + 2] | ((unsigned int)dirBuf[off + 3] << 8) |
                                                  ((unsigned int)dirBuf[off + 4] << 16) | ((unsigned int)dirBuf[off + 5] << 24);
                            unsigned int sysSize = (unsigned int)dirBuf[off + 10] | ((unsigned int)dirBuf[off + 11] << 8) |
                                                   ((unsigned int)dirBuf[off + 12] << 16) | ((unsigned int)dirBuf[off + 13] << 24);

                            long sysOffset = (long)sysLba * sectorSize + modeOffset;
                            if (sysSize > 0 && sysSize < 4096 && fseek(f, sysOffset, SEEK_SET) == 0) {
                                char *sysBuf = (char *)malloc(sysSize + 1);
                                if (sysBuf) {
                                    if (fread(sysBuf, 1, sysSize, f) == sysSize) {
                                        sysBuf[sysSize] = '\0';
                                        // Parse BOOT line in SYSTEM.CNF
                                        char *bootPtr = strstr(sysBuf, "BOOT");
                                        if (!bootPtr)
                                            bootPtr = strstr(sysBuf, "boot");
                                        if (bootPtr) {
                                            char *eq = strchr(bootPtr, '=');
                                            if (eq && retrogemCleanTitleID(eq + 1, gameID, maxLen)) {
                                                free(sysBuf);
                                                free(dirBuf);
                                                return 1; // Tier 1 Success!
                                            }
                                        }
                                    }
                                    free(sysBuf);
                                }
                            }
                        }
                    }
                    off += recLen;
                }
            }
            free(dirBuf);
        }
    }

    // --- Tier 2: PVD Volume Creation Timestamp Lookup ---
    // 16-byte ASCII creation timestamp at PVD offset 0x32D (813)
    char timestamp[17];
    memcpy(timestamp, &pvd[813], 16);
    timestamp[16] = '\0';

    int idx = 0;
    while (ps1_generic_game_ids[idx].timestamp[0] != '\0') {
        if (!strncmp(timestamp, ps1_generic_game_ids[idx].timestamp, 16)) {
            snprintf(gameID, maxLen, "%s", ps1_generic_game_ids[idx].game_id);
            return 1; // Tier 2 Success!
        }
        idx++;
    }

    return 0;
}

int retrogemGetVcdGameID(const char *vcdPath, char *gameID, size_t maxLen)
{
    if (vcdPath == NULL || gameID == NULL || maxLen < 12)
        return 0;
    gameID[0] = '\0';

    // --- Tier 1 & Tier 2: ISO 9660 SYSTEM.CNF Parsing / PVD Timestamp ---
    FILE *f = fopen(vcdPath, "rb");
    if (f != NULL) {
        int res = retrogemParseIsoSector16(f, gameID, maxLen);
        fclose(f);
        if (res && gameID[0] != '\0')
            return 1;
    }

    // --- Tier 3: Partition / Subpath Parsing ---
    // e.g. PP.SLUS-12345/IMAGE0.VCD or __.POPS/SLUS_123.45.VCD
    const char *p1 = strstr(vcdPath, "PP.");
    if (p1 != NULL && retrogemCleanTitleID(p1 + 3, gameID, maxLen))
        return 1;

    const char *p2 = strrchr(vcdPath, '/');
    if (p2 == NULL)
        p2 = strrchr(vcdPath, '\\');
    if (p2 != NULL) {
        // Try parent directory name
        const char *p3 = p2 - 1;
        while (p3 > vcdPath && *p3 != '/' && *p3 != '\\')
            p3--;
        if (p3 >= vcdPath) {
            char dirName[64];
            size_t dlen = p2 - p3 - 1;
            if (dlen > 0 && dlen < sizeof(dirName)) {
                memcpy(dirName, p3 + 1, dlen);
                dirName[dlen] = '\0';
                if (retrogemCleanTitleID(dirName, gameID, maxLen))
                    return 1;
            }
        }
    }

    // --- Tier 4: Filename Fallback ---
    const char *fname = strrchr(vcdPath, '/');
    if (fname == NULL)
        fname = strrchr(vcdPath, '\\');
    if (fname == NULL)
        fname = strrchr(vcdPath, ':');
    if (fname != NULL)
        fname++;
    else
        fname = vcdPath;

    if (vcdExtractGameId(fname, gameID, (int)maxLen))
        return 1;

    if (retrogemCleanTitleID(fname, gameID, maxLen))
        return 1;

    return 0;
}

static u8 retrogemCalculateCRC(const u8 *data, int len)
{
    int i;
    u8 sum = 0;
    for (i = 0; i < len; i++)
        sum = (u8)(sum + data[i]);
    return (u8)(0x100 - sum);
}

void displayRetroGemGameID(const char *gameID, int frames)
{
    u8 data[64];
    int gidlen, dpos, data_len, xstart, ystart, height;
    int i, j, frame;

    if (gameID == NULL || gameID[0] == '\0')
        return;

    gidlen = (int)strlen(gameID);
    if (gidlen > 11)
        gidlen = 11;
    if (gidlen <= 0)
        return;

    if (frames < 1)
        frames = 1;

    memset(data, 0, sizeof(data));
    dpos = 0;
    data[dpos++] = 0xA5;       // Header
    data[dpos++] = 0x00;       // Address offset
    dpos++;                    // Checksum placeholder (data[2])
    data[dpos++] = (u8)gidlen; // Length byte
    for (i = 0; i < gidlen; i++)
        data[dpos++] = (u8)gameID[i];
    data[dpos++] = 0x00; // Padding
    data[dpos++] = 0xD5; // Footer end word
    data[dpos++] = 0x00; // Padding

    data_len = dpos;
    data[2] = retrogemCalculateCRC(&data[3], data_len - 3);

    xstart = (screenWidth / 2) - (data_len * 8);
    ystart = screenHeight - (((screenHeight / 8) * 2) + 20);
    height = 2;

    for (frame = 0; frame < frames; frame++) {
        guiStartFrame();
        rmDrawRect(0, 0, screenWidth, screenHeight, GS_SETREG_RGBA(0x00, 0x00, 0x00, 0x80));

        for (i = 0; i < data_len; i++) {
            for (j = 7; j >= 0; j--) {
                int x = xstart + (i * 16 + (7 - j) * 2);
                // Clock pixel: Magenta (#FF00FF)
                rmDrawRect(x, ystart, 1, height, GS_SETREG_RGBA(0xFF, 0x00, 0xFF, 0x80));
                // Data bit pixel: Cyan (#00FFFF) for 1, Yellow (#FFFF00) for 0
                u64 bit_color = ((data[i] >> j) & 1) ? GS_SETREG_RGBA(0x00, 0xFF, 0xFF, 0x80) : GS_SETREG_RGBA(0xFF, 0xFF, 0x00, 0x80);
                rmDrawRect(x + 1, ystart, 1, height, bit_color);
            }
        }
        guiEndFrame();
    }

    // 1 clean black frame to ensure graphics buffer is clear before handoff
    guiStartFrame();
    rmDrawRect(0, 0, screenWidth, screenHeight, GS_SETREG_RGBA(0x00, 0x00, 0x00, 0x80));
    guiEndFrame();
}
