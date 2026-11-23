#define _GNU_SOURCE
#include "gridd/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>

static bool g_stderr_only = false;
static bool g_verbose = false;
static bool g_opened = false;

void gridd_log_init(const char *ident, bool verbose, bool use_stderr_only) {
    g_verbose = verbose;
    g_stderr_only = use_stderr_only;
    if (!use_stderr_only) {
        setlogmask(verbose ? LOG_UPTO(LOG_DEBUG) : LOG_UPTO(LOG_INFO));
        openlog(ident ? ident : "nvidia-gridd", 0, LOG_LOCAL0);
        g_opened = true;
    }
}

void gridd_log_close(void) {
    if (g_opened) {
        closelog();
        g_opened = false;
    }
}

void gridd_log_set_verbose(bool verbose) {
    g_verbose = verbose;
    if (g_opened) {
        setlogmask(verbose ? LOG_UPTO(LOG_DEBUG) : LOG_UPTO(LOG_INFO));
    }
}

void gridd_log(gridd_log_level_t level, const char *fmt, ...) {
    if (!fmt) {
        return;
    }
    if (!g_verbose && level == GRIDD_LOG_DEBUG) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    if (g_stderr_only || !g_opened) {
        fprintf(stderr, "[gridd:%d] ", (int)level);
        vfprintf(stderr, fmt, ap);
        size_t len = 0;
        while (fmt[len]) len++;
        if (len == 0 || fmt[len - 1] != '\n') {
            fputc('\n', stderr);
        }
    } else {
        vsyslog((int)level, fmt, ap);
    }
    va_end(ap);
}
