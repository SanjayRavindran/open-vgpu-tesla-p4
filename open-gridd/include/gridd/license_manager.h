#ifndef GRIDD_LICENSE_MANAGER_H
#define GRIDD_LICENSE_MANAGER_H

#include "gridd/nls_client.h"
#include "gridd/types.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gridd_lm_state_t state;
    gridd_lm_state_t pending_state;
    pthread_mutex_t lock;
    pthread_t thread;
    bool thread_started;
    bool stop_requested;
    int wake_pipe_rd;
    int wake_pipe_wr;
    int event_pipe_rd;
    int event_pipe_wr;
    uint32_t retry_interval_ms;
    uint32_t renew_interval_ms;
    char product_name[GRIDD_MAX_STR];
    uint32_t product_count;
    gridd_nls_client_t nls;
} gridd_license_manager_t;

gridd_status_t gridd_lm_create(gridd_license_manager_t *lm,
                               const gridd_token_config_t *token,
                               const gridd_environment_t *env,
                               const gridd_http_opts_t *http_opts,
                               const char *store_dir,
                               const char *machine_id,
                               uint32_t license_interval_min);

void gridd_lm_destroy(gridd_license_manager_t *lm);
void gridd_lm_acquire(gridd_license_manager_t *lm, const char *product_name, uint32_t count);
void gridd_lm_stop(gridd_license_manager_t *lm);

gridd_lm_state_t gridd_lm_get_state(gridd_license_manager_t *lm);
uint32_t gridd_lm_get_expiry_and_rm_state(gridd_license_manager_t *lm, uint32_t *out_expiry_epoch);
uint32_t gridd_lm_get_lease_port(gridd_license_manager_t *lm);
const char *gridd_lm_get_primary_url(gridd_license_manager_t *lm);
const char *gridd_lm_get_secondary_url(gridd_license_manager_t *lm);

#ifdef __cplusplus
}
#endif

#endif /* GRIDD_LICENSE_MANAGER_H */
