#ifndef OPL_LOG_H
#define OPL_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

// Log levels
#define LOG_LEVEL_ERROR 0
#define LOG_LEVEL_WARN  1
#define LOG_LEVEL_INFO  2
#define LOG_LEVEL_DEBUG 3
#define LOG_LEVEL_TRACE 4

#ifndef OPL_LOG_LEVEL
#ifdef __DEBUG
#define OPL_LOG_LEVEL LOG_LEVEL_DEBUG
#else
#define OPL_LOG_LEVEL LOG_LEVEL_INFO
#endif
#endif

void log_init(void);
void log_print(int level, const char *fmt, ...);
void log_dump_to_fd(int fd);

// Macros
#if OPL_LOG_LEVEL >= LOG_LEVEL_ERROR
#define LOG_ERR(fmt, ...) log_print(LOG_LEVEL_ERROR, "ERR: " fmt, ##__VA_ARGS__)
#else
#define LOG_ERR(fmt, ...) do {} while(0)
#endif

#if OPL_LOG_LEVEL >= LOG_LEVEL_WARN
#define LOG_WARN(fmt, ...) log_print(LOG_LEVEL_WARN, "WRN: " fmt, ##__VA_ARGS__)
#else
#define LOG_WARN(fmt, ...) do {} while(0)
#endif

#if OPL_LOG_LEVEL >= LOG_LEVEL_INFO
#define LOG_INFO(fmt, ...) log_print(LOG_LEVEL_INFO, "INF: " fmt, ##__VA_ARGS__)
#else
#define LOG_INFO(fmt, ...) do {} while(0)
#endif

#if OPL_LOG_LEVEL >= LOG_LEVEL_DEBUG
#define LOG_DEBUG(fmt, ...) log_print(LOG_LEVEL_DEBUG, "DBG: " fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) do {} while(0)
#endif

#if OPL_LOG_LEVEL >= LOG_LEVEL_TRACE
#define LOG_TRACE(fmt, ...) log_print(LOG_LEVEL_TRACE, "TRC: " fmt, ##__VA_ARGS__)
#else
#define LOG_TRACE(fmt, ...) do {} while(0)
#endif

#ifdef __cplusplus
}
#endif

#endif // OPL_LOG_H
