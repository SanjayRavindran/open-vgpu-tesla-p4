#ifndef GRIDD_DAEMON_H
#define GRIDD_DAEMON_H

#include "gridd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GRIDD_PID_DIR "/var/run/nvidia-gridd"
#define GRIDD_PID_FILE "/var/run/nvidia-gridd/nvidia-gridd.pid"

typedef struct {
    int exit_pipe_rd;
    int exit_pipe_wr;
    int dbus_pipe_rd;
    int dbus_pipe_wr;
    int pid_fd;
    int pid;
    bool created_pid_dir;
    bool foreground;
} gridd_daemon_ctx_t;

gridd_status_t gridd_daemon_init(gridd_daemon_ctx_t *ctx, bool verbose, bool foreground);
void gridd_daemon_cleanup(gridd_daemon_ctx_t *ctx, int exit_code);
bool gridd_daemon_stop_requested(void);
void gridd_daemon_request_stop(void);
gridd_status_t gridd_daemon_signal_dbus_reload(gridd_daemon_ctx_t *ctx);
gridd_status_t gridd_ensure_state_dirs(void);
gridd_status_t gridd_ensure_dir(const char *path, unsigned int mode);

#ifdef __cplusplus
}
#endif

#endif /* GRIDD_DAEMON_H */
