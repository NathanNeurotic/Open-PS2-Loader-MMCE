#include "include/opl.h"
#include "include/log.h"
#include "include/ioman.h"
#include <stdio.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

static int log_fd = -1;
static int log_try_open_counter = 0;

#define LOG_MEM_SIZE 8192
static char log_mem[LOG_MEM_SIZE];
static int log_mem_ptr = 0;
static int log_mem_full = 0;

void log_init(void)
{
    // Try to open log file on mass storage
    // We assume mass: is available if USB modules are loaded.
    // If not, this might fail, which is fine.
    log_fd = open("mass:/opl.log", O_WRONLY | O_CREAT | O_TRUNC);
    if (log_fd >= 0) {
        // Write header
        char header[128];
        snprintf(header, sizeof(header), "OPL Log Started. Version: %s\n", OPL_VERSION);
        write(log_fd, header, strlen(header));
    }
}

void log_print(int level, const char *fmt, ...)
{
    char buf[1024];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // Always print to IO manager (console/debug)
    ioPrintf("%s", buf);

    // Try to open file if not open
    if (log_fd < 0) {
        log_try_open_counter++;
        if ((log_try_open_counter % 50) == 1) {
             log_fd = open("mass:/opl.log", O_WRONLY | O_CREAT | O_APPEND);
             if (log_fd >= 0) {
                  // Dump memory buffer to file
                  if (log_mem_full) {
                      write(log_fd, log_mem + log_mem_ptr, LOG_MEM_SIZE - log_mem_ptr);
                  }
                  write(log_fd, log_mem, log_mem_ptr);
             }
        }
    }

    // Write to file if open
    if (log_fd >= 0) {
        write(log_fd, buf, strlen(buf));
    }

    // Write to memory buffer
    int len = strlen(buf);
    if (len > 0) {
        int i;
        for (i = 0; i < len; i++) {
            log_mem[log_mem_ptr] = buf[i];
            log_mem_ptr++;
            if (log_mem_ptr >= LOG_MEM_SIZE) {
                log_mem_ptr = 0;
                log_mem_full = 1;
            }
        }
    }
}

void log_dump_to_fd(int fd)
{
    char header[] = "\n--- LAST LOG LINES ---\n";
    write(fd, header, strlen(header));

    if (log_mem_full) {
        write(fd, log_mem + log_mem_ptr, LOG_MEM_SIZE - log_mem_ptr);
    }
    write(fd, log_mem, log_mem_ptr);

    char footer[] = "\n--- END LOG ---\n";
    write(fd, footer, strlen(footer));
}
