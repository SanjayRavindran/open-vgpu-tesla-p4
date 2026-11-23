#ifndef GRIDD_DBUS_SERVER_H
#define GRIDD_DBUS_SERVER_H

#include "gridd/license_manager.h"
#include "gridd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GRIDD_DBUS_BUS_NAME "nvidia.grid.server"
#define GRIDD_DBUS_INTERFACE "nvidia.grid.license"
#define GRIDD_DBUS_METHOD "GridLicenseState"

typedef struct {
    gridd_license_manager_t *lm;
    const uint32_t *active_feature_type;
    int dbus_reload_pipe_wr;
    int stop_pipe_rd;
} gridd_dbus_ctx_t;

void *gridd_dbus_thread_main(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* GRIDD_DBUS_SERVER_H */
