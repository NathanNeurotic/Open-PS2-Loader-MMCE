#ifndef __UTIL_H
#define __UTIL_H

int getmcID(void);
int getFileSize(int fd);
void checkMCFolder(void);
int openFile(char *path, int mode);
void *readFile(char *path, int align, int *size);
int listDir(char *path, const char *separator, int maxElem,
            int (*readEntry)(int index, const char *path, const char *separator, const char *name, unsigned char d_type));

typedef struct
{
    int fd;
    int mode;
    char *buffer;
    unsigned int size;
    unsigned int available;
    char *lastPtr;
    short allocResult;
    int writeError;           // sticky flag: set when any buffered write()/close() fails; returned by closeFileBuffer
    unsigned int totalQueued; // total bytes ever handed to writeFileBuffer; when == available at close
                              // time, the WHOLE content still sits in buffer (no intermediate flush) --
                              // configWrite's read-back verify uses that to capture the intended bytes
} file_buffer_t;

file_buffer_t *openFileBufferBuffer(short allocResult, const void *buffer, unsigned int size);
file_buffer_t *openFileBuffer(char *fpath, int mode, short allocResult, unsigned int size);
int readFileBuffer(file_buffer_t *readContext, char **outBuf);
void writeFileBuffer(file_buffer_t *fileBuffer, char *inBuf, int size);
// Returns 0 on success, -1 if any write()/close() during the buffered write failed (short write,
// wedged device, ...). Read-mode buffers always return 0. Callers that don't care may ignore it.
int closeFileBuffer(file_buffer_t *fileBuffer);

int max(int a, int b);
int min(int a, int b);
int fromHex(char digit);
char toHex(int digit);

enum CONSOLE_REGIONS {
    CONSOLE_REGION_INVALID = -1,
    CONSOLE_REGION_JAPAN = 0,
    CONSOLE_REGION_USA, // USA and HK/SG.
    CONSOLE_REGION_EUROPE,
    CONSOLE_REGION_CHINA,

    CONSOLE_REGION_COUNT
};

int InitConsoleRegionData(void);
const char *GetSystemDataPath(void);
char GetSystemFolderLetter(void);
int sysDeleteFolder(const char *folder);

int CheckPS2Logo(int fd, u32 lba);

void delay(int count);

#endif
