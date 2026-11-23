#define _GNU_SOURCE
#include "gridd/daemon.h"
#include "gridd/log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop_flag = 0;
static int g_exit_pipe_wr_global = -1;

static void signal_handler(int signo) {
    if (signo == SIGINT || signo == SIGTERM) {
        g_stop_flag = 1;
        if (g_exit_pipe_wr_global >= 0) {
            uint32_t token = 1;
            ssize_t unused = write(g_exit_pipe_wr_global, &token, sizeof(token));
            (void)unused;
        }
    }
}

bool gridd_daemon_stop_requested(void) {
    return g_stop_flag != 0;
}

void gridd_daemon_request_stop(void) {
    g_stop_flag = 1;
    if (g_exit_pipe_wr_global >= 0) {
        uint32_t token = 1;
        ssize_t unused = write(g_exit_pipe_wr_global, &token, sizeof(token));
        (void)unused;
    }
}

gridd_status_t gridd_ensure_dir(const char *path, unsigned int mode) {
    if (!path || !*path) return GRIDD_ERR_INVALID_PARAM;
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) return GRIDD_OK;
        return GRIDD_ERR_IO;
    }

    char tmp[GRIDD_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST) {
                return GRIDD_ERR_IO;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, (mode_t)mode) != 0 && errno != EEXIST) {
        return GRIDD_ERR_IO;
    }
    return GRIDD_OK;
}

gridd_status_t gridd_ensure_state_dirs(void) {
    static const char *dirs[] = {
        "/var/lib/nvidia/GridLicensing",
        "/etc/nvidia/license",
        "/etc/nvidia/ClientConfigToken",
        "/var/lib/nvidia/vGPULicensing",
        "/etc/nvidia/vGPULicense"
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        if (gridd_ensure_dir(dirs[i], 0755) != GRIDD_OK) {
            LOG_E("Failed to ensure directory %s: %s", dirs[i], strerror(errno));
            return GRIDD_ERR_IO;
        }
    }
    return GRIDD_OK;
}

gridd_status_t gridd_daemon_signal_dbus_reload(gridd_daemon_ctx_t *ctx) {
    if (!ctx || ctx->dbus_pipe_wr < 0) return GRIDD_ERR_PIPE;
    uint32_t msg = 1;
    if (write(ctx->dbus_pipe_wr, &msg, sizeof(msg)) != (ssize_t)sizeof(msg)) {
        LOG_E("D-Bus event write failed : %s", strerror(errno));
        return GRIDD_ERR_PIPE;
    }
    return GRIDD_OK;
}

gridd_status_t gridd_daemon_init(gridd_daemon_ctx_t *ctx, bool verbose, bool foreground) {
    if (!ctx) return GRIDD_ERR_INVALID_PARAM;
    memset(ctx, 0, sizeof(*ctx));
    ctx->exit_pipe_rd = -1;
    ctx->exit_pipe_wr = -1;
    ctx->dbus_pipe_rd = -1;
    ctx->dbus_pipe_wr = -1;
    ctx->pid_fd = -1;
    ctx->foreground = foreground;
    g_stop_flag = 0;

    sigset_t block_set;
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGCHLD);
    sigaddset(&block_set, SIGTSTP);
    sigaddset(&block_set, SIGTTOU);
    sigaddset(&block_set, SIGTTIN);
    sigprocmask(SIG_BLOCK, &block_set, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);

    if (!foreground) {
        pid_t pid = fork();
        if (pid < 0) {
            LOG_E("Failed to fork() daemon: %s", strerror(errno));
            return GRIDD_ERR_GENERIC;
        }
        if (pid > 0) {
            exit(0);
        }
    }

    int exit_fds[2] = {-1, -1};
    if (pipe(exit_fds) != 0) {
        LOG_E("Failed to open file for triggering exit events: %s", strerror(errno));
        return GRIDD_ERR_IO;
    }
    ctx->exit_pipe_rd = exit_fds[0];
    ctx->exit_pipe_wr = exit_fds[1];
    g_exit_pipe_wr_global = exit_fds[1];

    int dbus_fds[2] = {-1, -1};
    if (pipe(dbus_fds) == 0) {
        ctx->dbus_pipe_rd = dbus_fds[0];
        ctx->dbus_pipe_wr = dbus_fds[1];
    }

    ctx->pid = (int)getpid();
    if (!foreground) {
        umask(026);
        if (setsid() < 0) {
            LOG_E("Failed to create new daemon session: %s", strerror(errno));
            return GRIDD_ERR_GENERIC;
        }
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        gridd_log_init("nvidia-gridd", verbose, false);
        LOG_I("Verbose syslog connection opened");
        if (chdir("/") < 0) {
            LOG_W("Failed to change working directory: %s", strerror(errno));
        }
    } else {
        gridd_log_init("nvidia-gridd", verbose, true);
    }

    if (!foreground) {
        if (mkdir(GRIDD_PID_DIR, 0755) < 0) {
            if (errno != EEXIST) {
                LOG_W("Failed to create directory %s: %s", GRIDD_PID_DIR, strerror(errno));
            }
            LOG_I("Directory %s will not be removed on exit", GRIDD_PID_DIR);
        } else {
            ctx->created_pid_dir = true;
        }

        if (access(GRIDD_PID_DIR, R_OK | W_OK) < 0) {
            LOG_E("Unable to access %s: %s", GRIDD_PID_DIR, strerror(errno));
            return GRIDD_ERR_IO;
        }

        ctx->pid_fd = open(GRIDD_PID_FILE, O_RDWR | O_CREAT, 0644);
        if (ctx->pid_fd < 0) {
            LOG_E("Failed to open PID file: %s", strerror(errno));
            return GRIDD_ERR_IO;
        }
        if (lockf(ctx->pid_fd, F_TLOCK, 0) < 0) {
            LOG_E("Failed to lock PID file: %s", strerror(errno));
            return GRIDD_ERR_IO;
        }
        char pid_buf[32];
        int plen = snprintf(pid_buf, sizeof(pid_buf), "%d\n", ctx->pid);
        if (plen <= 0 || write(ctx->pid_fd, pid_buf, (size_t)plen) != plen) {
            LOG_E("Failed to update PID file: %s", strerror(errno));
            return GRIDD_ERR_IO;
        }
    }

    LOG_N("Started (%d)", ctx->pid);
    return GRIDD_OK;
}

void gridd_daemon_cleanup(gridd_daemon_ctx_t *ctx, int exit_code) {
    (void)exit_code;
    if (!ctx) return;

    if (ctx->pid_fd >= 0) {
        if (lockf(ctx->pid_fd, F_ULOCK, 0) < 0) {
            LOG_W("Failed to unlock PID file: %s", strerror(errno));
        } else {
            LOG_D("PID file unlocked.");
        }
        close(ctx->pid_fd);
        ctx->pid_fd = -1;
        if (!ctx->foreground) {
            unlink(GRIDD_PID_FILE);
        }
    }
    if (ctx->created_pid_dir && !ctx->foreground) {
        if (rmdir(GRIDD_PID_DIR) != 0 && errno == EACCES) {
            LOG_N("The daemon no longer has permission to remove its runtime data directory %s",
                  GRIDD_PID_DIR);
        }
    }
    if (ctx->exit_pipe_rd >= 0) { close(ctx->exit_pipe_rd); ctx->exit_pipe_rd = -1; }
    if (ctx->exit_pipe_wr >= 0) { close(ctx->exit_pipe_wr); ctx->exit_pipe_wr = -1; }
    if (ctx->dbus_pipe_rd >= 0) { close(ctx->dbus_pipe_rd); ctx->dbus_pipe_rd = -1; }
    if (ctx->dbus_pipe_wr >= 0) { close(ctx->dbus_pipe_wr); ctx->dbus_pipe_wr = -1; }
    g_exit_pipe_wr_global = -1;
    gridd_log_close();
}
