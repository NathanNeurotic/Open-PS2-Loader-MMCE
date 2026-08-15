/*
  Copyright 2009, Ifcaro & volca
  Licenced under Academic Free License version 3.0
  Review OpenUsbLd README & LICENSE files for further details.
*/

#include "include/opl.h"
#include "include/util.h"
#include "include/ioman.h"
#include "include/system.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <malloc.h>
#include <rom0_info.h>

#include "include/hdd.h"

#include "../modules/isofs/zso.h"

extern int probed_fd;
extern u32 probed_lba;

extern unsigned char icon_sys[];
extern unsigned int size_icon_sys;
extern unsigned char icon_icn[];
extern unsigned int size_icon_icn;

static int mcID = -1;

void guiWarning(const char *text, int count);

int getmcID(void)
{
    return mcID;
}

int getFileSize(int fd)
{
    int size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    return size;
}

static int checkMC()
{
    int mc0_is_ps2card, mc1_is_ps2card;
    int mc0_has_folder, mc1_has_folder;

    if (mcID == -1) {
        mc0_is_ps2card = 0;
        DIR *mc0_root_dir = opendir("mc0:/");
        if (mc0_root_dir != NULL) {
            closedir(mc0_root_dir);
            mc0_is_ps2card = 1;
        }

        mc1_is_ps2card = 0;
        DIR *mc1_root_dir = opendir("mc1:/");
        if (mc1_root_dir != NULL) {
            closedir(mc1_root_dir);
            mc1_is_ps2card = 1;
        }

        mc0_has_folder = 0;
        DIR *mc0_opl_dir = opendir("mc0:OPL/");
        if (mc0_opl_dir != NULL) {
            closedir(mc0_opl_dir);
            mc0_has_folder = 1;
        }

        mc1_has_folder = 0;
        DIR *mc1_opl_dir = opendir("mc1:OPL/");
        if (mc1_opl_dir != NULL) {
            closedir(mc1_opl_dir);
            mc1_has_folder = 1;
        }

        if (mc0_has_folder) {
            mcID = '0';
            return mcID;
        }

        if (mc1_has_folder) {
            mcID = '1';
            return mcID;
        }

        if (mc0_is_ps2card) {
            mcID = '0';
            return mcID;
        }

        if (mc1_is_ps2card) {
            mcID = '1';
            return mcID;
        }
    }
    return mcID;
}

// Write one browser-save file into an existing folder if it is not already there. Never truncates
// an existing one: the icon is static, and rewriting it would be a pointless card write on a device
// whose writes we are otherwise being careful about.
static void writeIconFile(const char *dir, const char *name, const void *data, unsigned int len)
{
    char path[128];
    int fd;

    // dir may or may not carry a trailing slash ("mc0:OPL/" vs "mc0:/APPS/RIPTOPL").
    size_t dlen = strlen(dir);
    const char *sep = (dlen > 0 && (dir[dlen - 1] == '/' || dir[dlen - 1] == ':')) ? "" : "/";
    if (snprintf(path, sizeof(path), "%s%s%s", dir, sep, name) >= (int)sizeof(path))
        return;

    fd = open(path, O_RDONLY);
    if (fd >= 0) {
        close(fd);
        return; // already present
    }

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) {
        write(fd, data, len);
        close(fd);
    }
}

// Give the folder settings ACTUALLY get saved into the browser save icon (opl.icn + icon.sys), so
// the PS2/PS3 Memory Card Manager shows it properly instead of "Corrupted Data" (#353).
//
// Takes the config FILE path and uses its own directory, rather than assuming mc?:OPL/. That
// assumption is why the icon stamping had to be gated to the legacy no-boot-identity case in the
// first place: on an appdir-on-MC boot the settings live beside the ELF, so stamping mc?:OPL/
// sprouted a second, empty OPL folder on the card on every save (FifthFox, 2026-07-16). Aiming at
// the real folder makes the gate unnecessary -- every mc save home gets its icon, and no save home
// gets someone else's.
void checkMCSaveIcons(const char *cfgFilePath)
{
    char dir[128];

    if (cfgFilePath == NULL || strncmp(cfgFilePath, "mc", 2) != 0 || cfgFilePath[2] == 'm')
        return; // not a memory card ("mmce" is a different device entirely)

    const char *slash = strrchr(cfgFilePath, '/');
    if (slash == NULL) {
        // "mc0:settings.cfg" -- the card root, which owns no save folder to decorate.
        return;
    }

    int n = (int)(slash - cfgFilePath);
    if (n <= 0 || n >= (int)sizeof(dir))
        return;
    memcpy(dir, cfgFilePath, n);
    dir[n] = '\0';

    writeIconFile(dir, "opl.icn", icon_icn, size_icon_icn);
    writeIconFile(dir, "icon.sys", icon_sys, size_icon_sys);
}

// Ensure mc?:OPL/ exists and contains browser save icon (opl.icn + icon.sys) so PS2/PS3
// Memory Card Manager displays the save folder with a valid icon instead of 'Corrupted Data' (#353).
void checkMCFolder(void)
{
    char path[32];
    int fd;

    if (checkMC() < 0) {
        return;
    }

    snprintf(path, sizeof(path), "mc%d:OPL/", mcID & 1);
    mkdir(path, 0777);

    snprintf(path, sizeof(path), "mc%d:OPL/opl.icn", mcID & 1);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            write(fd, icon_icn, size_icon_icn);
            close(fd);
        }
    } else {
        close(fd);
    }

    snprintf(path, sizeof(path), "mc%d:OPL/icon.sys", mcID & 1);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) {
            write(fd, icon_sys, size_icon_sys);
            close(fd);
        }
    } else {
        close(fd);
    }
}

static int checkFile(char *path, int mode)
{
    // The "mc?:" wildcard-card resolution is mc-specific: substitute the detected card digit, or bail
    // if no card is present.
    if (strncmp(path, "mc", 2) == 0 && path[2] == 0x3F) {
        if (checkMC() >= 0)
            path[2] = mcID;
        else
            return 0;
    }

    // In create mode, ensure the immediate parent directory exists (create it if not) for any device
    // path shaped "<dev>:/<subdir>/<file>" -- previously this ran for "mc" prefixes only. In practice
    // that shape is the MMCE per-game config target mmceN:/.../CFG/<id>.cfg (the reported case);
    // BDM/HDD homes are written as "massN:OPL/..."/"pfs0:OPL/..." with no slash after the colon and are
    // NOT matched here -- they were never matched before either, and sbCreateFolders remains their
    // safety net. The MMCE failure: the driver's open(O_CREAT) cannot create a file under a missing
    // directory, and sbCreateFolders' mkdir burst return is UNCHECKED while its once-per-card memo
    // latches regardless -- so a single mkdir missed on a busy card leaves CFG permanently absent and
    // every per-game save fails. Before d5150a49 that was swallowed and shown as "saved"; now it
    // honestly errors (#245, AndrewBento, MMCE). Creating the parent here makes the write persist.
    //
    // Only a genuine SUBdirectory is created -- never a bare device root -- because the driver owns the
    // root and its opendir/mkdir semantics differ; `pos > devSlash + 1` requires a path component after
    // "<dev>:/". One extra opendir per O_CREAT write only (never on reads); on MMCE that is one SIO2
    // round-trip on an already user-initiated, infrequent save.
    if (mode & O_CREAT) {
        char *lastSlash = strrchr(path, '/');
        char *lastBackslash = strrchr(path, '\\');
        char *pos = lastSlash;
        if (lastBackslash != NULL && (pos == NULL || lastBackslash > pos))
            pos = lastBackslash;
        const char *colon = strchr(path, ':');
        if (pos != NULL && colon != NULL && pos > colon + 1) {
            char dirPath[256];
            int n = (int)(pos - path);
            if (n < (int)sizeof(dirPath)) {
                memcpy(dirPath, path, n);
                dirPath[n] = '\0';
                // Best-effort: attempt the mkdir only when opendir says the dir is missing, but NEVER
                // fail checkFile on the result. open() is the real source of truth -- if the dir truly
                // could not be made, the open below fails honestly; and if opendir ever mis-reports an
                // existing dir as missing (a device-driver quirk), the harmless mkdir-EEXIST is ignored
                // and the write still proceeds. So this can only ever HELP a write, never block one.
                DIR *dir = opendir(dirPath);
                if (dir == NULL)
                    mkdir(dirPath, 0777);
                else
                    closedir(dir);
            }
        }
    }
    return 1;
}

int openFile(char *path, int mode)
{
    if (checkFile(path, mode))
        return open(path, mode, 0666);
    else
        return -1;
}

void *readFile(char *path, int align, int *size)
{
    void *buffer = NULL;

    int fd = openFile(path, O_RDONLY);
    if (fd >= 0) {
        unsigned int realSize = getFileSize(fd);

        if ((*size > 0) && (*size != realSize)) {
            LOG("UTIL Invalid filesize, expected: %d, got: %d\n", *size, realSize);
            close(fd);
            return NULL;
        }

        if (align > 0)
            buffer = memalign(64, realSize); // The allocation is aligned to aid the DMA transfers
        else
            buffer = malloc(realSize);

        if (!buffer) {
            LOG("UTIL ReadFile: Failed allocation of %d bytes", realSize);
            *size = 0;
        } else {
            int rd = read(fd, buffer, realSize);
            close(fd);
            if (rd != (int)realSize) {
                LOG("UTIL ReadFile: short read %d of %d bytes\n", rd, realSize);
                free(buffer);
                *size = 0;
                return NULL;
            }
            *size = realSize;
        }
    }
    return buffer;
}

int listDir(char *path, const char *separator, int maxElem,
            int (*readEntry)(int index, const char *path, const char *separator, const char *name, unsigned char d_type))
{
    int index = 0;

    if (checkFile(path, O_RDONLY)) {
        DIR *dir = opendir(path);
        struct dirent *dirent;
        if (dir != NULL) {
            while (index < maxElem && (dirent = readdir(dir)) != NULL) {
                index = readEntry(index, path, separator, dirent->d_name, dirent->d_type);
            }

            closedir(dir);
        }
    }
    return index;
}

/* size will be the maximum line size possible */
file_buffer_t *openFileBuffer(char *fpath, int mode, short allocResult, unsigned int size)
{
    file_buffer_t *fileBuffer = NULL;
    unsigned char bom[3];

    int fd = openFile(fpath, mode);
    if (fd >= 0) {
        fileBuffer = (file_buffer_t *)malloc(sizeof(file_buffer_t));
        if (fileBuffer == NULL) {
            close(fd);
            return NULL;
        }
        fileBuffer->buffer = (char *)malloc(size * sizeof(char));
        if (fileBuffer->buffer == NULL) {
            free(fileBuffer);
            close(fd);
            return NULL;
        }
        fileBuffer->size = size;
        fileBuffer->available = 0;
        if (mode == O_RDONLY) {
            fileBuffer->lastPtr = NULL;

            // Check for and skip the UTF-8 BOM sequence.
            if ((read(fd, bom, sizeof(bom)) != 3) ||
                (bom[0] != 0xEF || bom[1] != 0xBB || bom[2] != 0xBF)) {
                // Not BOM, so rewind.
                lseek(fd, 0, SEEK_SET);
            }
        } else
            fileBuffer->lastPtr = fileBuffer->buffer;
        fileBuffer->allocResult = allocResult;
        fileBuffer->fd = fd;
        fileBuffer->mode = mode;
        fileBuffer->writeError = 0;
        fileBuffer->totalQueued = 0;
    }

    return fileBuffer;
}

/* size will be the maximum line size possible */
file_buffer_t *openFileBufferBuffer(short allocResult, const void *buffer, unsigned int size)
{
    file_buffer_t *fileBuffer = NULL;

    fileBuffer = (file_buffer_t *)malloc(sizeof(file_buffer_t));
    if (fileBuffer == NULL)
        return NULL;
    fileBuffer->buffer = (char *)malloc((size + 1) * sizeof(char));
    if (fileBuffer->buffer == NULL) {
        free(fileBuffer);
        return NULL;
    }
    fileBuffer->size = size;
    fileBuffer->available = size;
    fileBuffer->lastPtr = fileBuffer->buffer; // O_RDONLY, but with the data in the buffer.
    fileBuffer->allocResult = allocResult;
    fileBuffer->fd = -1;
    fileBuffer->mode = O_RDONLY;
    fileBuffer->writeError = 0;
    fileBuffer->totalQueued = 0;

    memcpy(fileBuffer->buffer, buffer, size);
    fileBuffer->buffer[size] = '\0';

    return fileBuffer;
}

int readFileBuffer(file_buffer_t *fileBuffer, char **outBuf)
{
    int lineSize = 0, readSize, length;
    char *posLF = NULL;

    while (1) {
        // if lastPtr is set, then we continue the read from this point as reference
        if (fileBuffer->lastPtr) {
            // Calculate the remaining chars to the right of lastPtr
            lineSize = fileBuffer->available - (fileBuffer->lastPtr - fileBuffer->buffer);
            /* LOG("##### Continue read, position: %X (total: %d) line size (\\0 not inc.): %d end: %x\n",
                fileBuffer->lastPtr - fileBuffer->buffer, fileBuffer->available, lineSize, fileBuffer->lastPtr[lineSize]); */
            posLF = strchr(fileBuffer->lastPtr, '\n');
        }

        if (!posLF) { // We can come here either when the buffer is empty, or if the remaining chars don't have a LF

            // if available, we shift the remaining chars to the left ...
            if (lineSize) {
                // LOG("##### LF not found, Shift %d characters from end to beginning\n", lineSize);
                memmove(fileBuffer->buffer, fileBuffer->lastPtr, lineSize);
            }

            // ... and complete the buffer if we're not at EOF
            if (fileBuffer->fd >= 0) {

                // Load as many characters necessary to fill the buffer
                length = fileBuffer->size - lineSize - 1;
                // LOG("##### Asking for %d characters to complete buffer\n", length);
                readSize = read(fileBuffer->fd, fileBuffer->buffer + lineSize, length);
                if (readSize < 0) {
                    close(fileBuffer->fd);
                    fileBuffer->fd = -1;
                    readSize = 0;
                }
                fileBuffer->buffer[lineSize + readSize] = '\0';

                // Search again (from the lastly added chars only), the result will be "analyzed" in next if
                posLF = strchr(fileBuffer->buffer + lineSize, '\n');

                // Now update read context info
                lineSize = lineSize + readSize;
                // LOG("##### %d characters really read, line size now (\\0 not inc.): %d\n", read, lineSize);

                // If buffer not full it means we are at EOF
                if (fileBuffer->fd >= 0 && fileBuffer->size != lineSize + 1) {
                    // LOG("##### Reached EOF\n");
                    close(fileBuffer->fd);
                    fileBuffer->fd = -1;
                }
            }

            fileBuffer->lastPtr = fileBuffer->buffer;
            fileBuffer->available = lineSize;
        }

        if (posLF)
            lineSize = posLF - fileBuffer->lastPtr;

        // Check the previous char (on Windows there are CR/LF instead of single linux LF)
        if (lineSize)
            if (*(fileBuffer->lastPtr + lineSize - 1) == '\r')
                lineSize--;

        fileBuffer->lastPtr[lineSize] = '\0';
        *outBuf = fileBuffer->lastPtr;

        // LOG("##### Result line is \"%s\" size: %d avail: %d pos: %d\n", fileBuffer->lastPtr, lineSize, fileBuffer->available, fileBuffer->lastPtr - fileBuffer->buffer);

        // If we are at EOF and no more chars available to scan, then we are finished
        if (!lineSize && !fileBuffer->available && fileBuffer->fd == -1)
            return 0;

        if (fileBuffer->lastPtr[0] == '#') { // '#' for comment lines
            if (posLF)
                fileBuffer->lastPtr = posLF + 1;
            else
                fileBuffer->lastPtr = NULL;
            continue;
        }

        if (lineSize && fileBuffer->allocResult) {
            *outBuf = (char *)malloc((lineSize + 1) * sizeof(char));
            memcpy(*outBuf, fileBuffer->lastPtr, lineSize + 1);
        }

        // Either move the pointer to next chars, or set it to null to force a whole buffer read (if possible)
        if (posLF)
            fileBuffer->lastPtr = posLF + 1;
        else {
            fileBuffer->lastPtr = NULL;
        }

        return 1;
    }
}

void writeFileBuffer(file_buffer_t *fileBuffer, char *inBuf, int size)
{
    fileBuffer->totalQueued += size; // see util.h -- lets configWrite know whether buffer still holds ALL content
    // LOG("writeFileBuffer avail: %d size: %d\n", fileBuffer->available, size);
    if (fileBuffer->available && fileBuffer->available + size > fileBuffer->size) {
        // LOG("writeFileBuffer flushing: %d\n", fileBuffer->available);
        if (write(fileBuffer->fd, fileBuffer->buffer, fileBuffer->available) != (int)fileBuffer->available)
            fileBuffer->writeError = 1; // short write / wedged device -- surfaced by closeFileBuffer
        fileBuffer->lastPtr = fileBuffer->buffer;
        fileBuffer->available = 0;
    }

    if (size > fileBuffer->size) {
        // LOG("writeFileBuffer direct write: %d\n", size);
        if (write(fileBuffer->fd, inBuf, size) != size)
            fileBuffer->writeError = 1;
    } else {
        memcpy(fileBuffer->lastPtr, inBuf, size);
        fileBuffer->lastPtr += size;
        fileBuffer->available += size;

        // LOG("writeFileBuffer lastPrt: %d\n", (fileBuffer->lastPtr - fileBuffer->buffer));
    }
}

int closeFileBuffer(file_buffer_t *fileBuffer)
{
    if (fileBuffer == NULL)
        return -1; // defensive: current callers null-check first, but never deref a null buffer
    int err = fileBuffer->writeError;
    if (fileBuffer->fd >= 0) {
        if (fileBuffer->mode != O_RDONLY && fileBuffer->available) {
            // LOG("writeFileBuffer final write: %d\n", fileBuffer->available);
            if (write(fileBuffer->fd, fileBuffer->buffer, fileBuffer->available) != (int)fileBuffer->available)
                err = 1;
        }
        if (close(fileBuffer->fd) < 0)
            err = 1;
    }
    free(fileBuffer->buffer);
    free(fileBuffer);
    return err ? -1 : 0;
}

// a simple maximum of two
int max(int a, int b)
{
    return a > b ? a : b;
}

// a simple minimum of two
int min(int a, int b)
{
    return a < b ? a : b;
}

// single digit from hex decode
int fromHex(char digit)
{
    if ((digit >= '0') && (digit <= '9')) {
        return (digit - '0');
    } else if ((digit >= 'A') && (digit <= 'F')) {
        return (digit - 'A' + 10);
    } else if ((digit >= 'a') && (digit <= 'f')) {
        return (digit - 'a' + 10);
    } else
        return -1;
}

static const char htab[16] = "0123456789ABCDEF";
char toHex(int digit)
{
    return htab[digit & 0x0F];
}

static short int ConsoleRegion = CONSOLE_REGION_INVALID;
static char SystemDataFolderPath[] = "BRDATA-SYSTEM";
static char SystemFolderLetter = 'R';

static void UpdateSystemPaths(void)
{
    char regions[CONSOLE_REGION_COUNT] = {'I', 'A', 'E', 'C'};

    SystemFolderLetter = regions[ConsoleRegion];
    SystemDataFolderPath[1] = SystemFolderLetter;
}

int InitConsoleRegionData(void)
{
    int result;
    char romver[16];

    if ((result = ConsoleRegion) < 0) {
        _io_driver driver = {open, close, (int (*)(int, void *, int))read, O_RDONLY};
        GetRomNameWithIODriver(romver, &driver);

        switch (romver[4]) {
            case 'C':
                ConsoleRegion = CONSOLE_REGION_CHINA;
                break;
            case 'E':
                ConsoleRegion = CONSOLE_REGION_EUROPE;
                break;
            case 'H':
            case 'A':
                ConsoleRegion = CONSOLE_REGION_USA;
                break;
            default: // For Japanese and unidentified consoles.
                ConsoleRegion = CONSOLE_REGION_JAPAN;
        }

        result = ConsoleRegion;

        UpdateSystemPaths();
    }

    return result;
}

const char *GetSystemDataPath(void)
{
    return SystemDataFolderPath;
}

char GetSystemFolderLetter(void)
{
    return SystemFolderLetter;
}

int GetSystemRegion(void)
{
    return ConsoleRegion;
}

int CheckPS2Logo(int fd, u32 lba)
{
    u8 logo[12 * 2048] ALIGNED(64);
    void *buffer = logo;
    u32 j, k, w;
    u8 ValidPS2Logo = 0;
    u32 ps2logochecksum = 0;
    char text[1024];

    w = 0;
    memset(logo, 0, sizeof(logo));
    if ((fd > 0) && (lba == 0)) { // BDM_MODE & ETH_MODE
        lseek(fd, 0, SEEK_SET);
        w = read(fd, logo, sizeof(logo)) == sizeof(logo);
    }
    if ((lba > 0) && (fd == 0)) {      // HDD_MODE
        for (k = 0; k < 12 * 4; k++) { // NB: Disc sector size (2048 bytes) and HDD sector size (512 bytes) differ, hence why we multiplied the number of sectors (12) by 4. Exactly 48 HDD sectors fill the 12*2048 buffer; '<= 48' read one sector past the end.
            w = !(hddReadSectors(lba + k, 1, buffer));
            if (!w)
                break;
            buffer += 512;
        }
    }

    if (*(u32 *)logo == ZSO_MAGIC) {
        // initialize ZSO
        ziso_init((ZISO_header *)logo, *(u32 *)((u8 *)logo + sizeof(ZISO_header)));
        probed_fd = fd;
        probed_lba = lba;
        // read ZISO data
        w = (ziso_read_sector(logo, 0, 12) == 12);
    }

    if (w) {
        u8 key = logo[0];
        if (logo[0] != 0) {
            for (j = 0; j < (12 * 2048); j++) {
                logo[j] ^= key;
                logo[j] = (logo[j] << 3) | (logo[j] >> 5);
            }
            for (j = 0; j < (12 * 2048); j++) {
                ps2logochecksum += (u32)logo[j];
            }
            // PS2LOGO NTSC Checksum = 0x120519
            // PS2LOGO PAL  Checksum = 0x1555AB
            snprintf(text, sizeof(text), "%s Disc PS2 Logo(checksum 0x%06X) & %s console ", ((ps2logochecksum == 0x1555AB) ? "PAL" : "NTSC"), ps2logochecksum, ((ConsoleRegion == CONSOLE_REGION_EUROPE) ? "PAL" : "NTSC"));
            if (((ps2logochecksum == 0x1555AB) && (ConsoleRegion == CONSOLE_REGION_EUROPE)) || ((ps2logochecksum == 0x120519) && (ConsoleRegion != CONSOLE_REGION_EUROPE))) {
                ValidPS2Logo = 1;
                strcat(text, "match.");
            } else {
                strcat(text, "don't match!");
            }
            if (gEnableDebug)
                guiWarning(text, 12);
        } else {
            if (gEnableDebug)
                guiWarning("Not a valid PS2 Logo first byte!", 12);
        }
    } else {
        if (gEnableDebug)
            guiWarning("Error reading first 12 disc sectors (PS2 Logo)!", 25);
    }
    return ValidPS2Logo;
}

struct DirentToDelete
{
    struct DirentToDelete *next;
    char *filename;
};

int sysDeleteFolder(const char *folder)
{
    int result;
    char *path;
    struct dirent *dirent;
    DIR *dir;
    struct DirentToDelete *head, *start;

    result = 0;
    start = head = NULL;
    if ((dir = opendir(folder)) != NULL) {
        /* Generate a list of files in the directory. */
        while ((dirent = readdir(dir)) != NULL) {
            if ((strcmp(dirent->d_name, ".") == 0) || ((strcmp(dirent->d_name, "..") == 0)))
                continue;

            path = malloc(strlen(folder) + strlen(dirent->d_name) + 2);
            sprintf(path, "%s/%s", folder, dirent->d_name);

            if (dirent->d_type == DT_DIR) {
                /* Recursive, delete all subfolders */
                int r = sysDeleteFolder(path);
                if (r < 0)
                    result = r;
                free(path);
            } else {
                free(path);
                if (start == NULL) {
                    head = malloc(sizeof(struct DirentToDelete));
                    if (head == NULL)
                        break;
                    start = head;
                } else {
                    if ((head->next = malloc(sizeof(struct DirentToDelete))) == NULL)
                        break;

                    head = head->next;
                }

                head->next = NULL;

                if ((head->filename = malloc(strlen(dirent->d_name) + 1)) != NULL)
                    strcpy(head->filename, dirent->d_name);
                else
                    break;
            }
        }

        closedir(dir);
    } else
        result = 0;

    if (result >= 0) {
        /* Delete the files. */
        for (head = start; head != NULL; head = start) {
            if (head->filename != NULL) {
                if ((path = malloc(strlen(folder) + strlen(head->filename) + 2)) != NULL) {
                    sprintf(path, "%s/%s", folder, head->filename);
                    result = unlink(path);
                    if (result < 0)
                        LOG("sysDeleteFolder: failed to remove %s: %d\n", path, result);

                    free(path);
                }
                free(head->filename);
            }

            start = head->next;
            free(head);
        }

        if (result >= 0) {
            result = rmdir(folder);
            if (result < 0)
                LOG("sysDeleteFolder: failed to rmdir %s: %d\n", folder, result);
        }
    }

    return result;
}

/*----------------------------------------------------------------------------------------*/
/* NOP delay.                                                                             */
/*----------------------------------------------------------------------------------------*/
void delay(int count)
{
    int i, ret;

    for (i = 0; i < count; i++) {
        ret = 0x01000000;
        while (ret--)
            asm("nop\nnop\nnop\nnop");
    }
}
