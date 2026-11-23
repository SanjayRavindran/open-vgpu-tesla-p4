#ifndef GRIDD_LOG_H
#define GRIDD_LOG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GRIDD_LOG_ERR = 3,
    GRIDD_LOG_WARN = 4,
    GRIDD_LOG_NOTICE = 5,
    GRIDD_LOG_INFO = 6,
    GRIDD_LOG_DEBUG = 7
} gridd_log_level_t;

void gridd_log_init(const char *ident, bool verbose, bool use_stderr_only);
void gridd_log_close(void);
void gridd_log_set_verbose(bool verbose);
void gridd_log(gridd_log_level_t level, const char *fmt, ...);

#define LOG_E(...) gridd_log(GRIDD_LOG_ERR, __VA_ARGS__)
#define LOG_W(...) gridd_log(GRIDD_LOG_WARN, __VA_ARGS__)
#define LOG_N(...) gridd_log(GRIDD_LOG_NOTICE, __VA_ARGS__)
#define LOG_I(...) gridd_log(GRIDD_LOG_INFO, __VA_ARGS__)
#define LOG_D(...) gridd_log(GRIDD_LOG_DEBUG, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* GRIDD_LOG_H */
