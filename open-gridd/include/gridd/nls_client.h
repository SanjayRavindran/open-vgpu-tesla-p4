#ifndef GRIDD_NLS_CLIENT_H
#define GRIDD_NLS_CLIENT_H

#include "gridd/env_fingerprint.h"
#include "gridd/http_client.h"
#include "gridd/token.h"
#include "gridd/trusted_store.h"
#include "gridd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char lease_ref[GRIDD_MAX_STR];
    char product_name[GRIDD_MAX_STR];
    uint32_t count;
    char expires_iso[64];
    uint64_t expires_epoch;
    double lease_duration_sec;
    double recommended_renewal;
    bool feature_expired;
    uint32_t refresh_count;
} gridd_lease_info_t;

typedef struct {
    gridd_token_config_t token;
    gridd_environment_t env;
    gridd_trusted_store_t store;
    gridd_http_opts_t http_opts;
    uint32_t active_node_idx;
    uint32_t license_interval_min;
    char auth_token[4096];
    uint64_t auth_token_expires_epoch;
    gridd_lease_info_t lease;
    bool has_active_lease;
    char last_error[512];
} gridd_nls_client_t;

gridd_status_t gridd_nls_init(gridd_nls_client_t *nls,
                              const gridd_token_config_t *token,
                              const gridd_environment_t *env,
                              const gridd_http_opts_t *http_opts,
                              const char *store_dir,
                              const char *machine_id,
                              uint32_t license_interval_min);

gridd_status_t gridd_nls_register_or_update_origin(gridd_nls_client_t *nls);
gridd_status_t gridd_nls_authenticate(gridd_nls_client_t *nls);
gridd_status_t gridd_nls_acquire_lease(gridd_nls_client_t *nls, const char *product_name, uint32_t count);
gridd_status_t gridd_nls_renew_lease(gridd_nls_client_t *nls, const char *product_name, uint32_t count);
gridd_status_t gridd_nls_get_active_leases(gridd_nls_client_t *nls, char *out_buf, size_t out_sz);
gridd_status_t gridd_nls_return_leases(gridd_nls_client_t *nls);
gridd_status_t gridd_nls_shutdown(gridd_nls_client_t *nls);

bool gridd_nls_build_lease_proposal_json(const gridd_token_config_t *token,
                                         const char *product_name,
                                         uint32_t count,
                                         uint32_t duration_min,
                                         char *out_buf,
                                         size_t out_sz);

bool gridd_nls_parse_lease_response(const char *json,
                                    uint64_t now_epoch,
                                    bool is_renew,
                                    gridd_lease_info_t *out_lease);

bool gridd_nls_try_nll(const char *conf_path,
                       const char *os_platform,
                       const char *arch,
                       gridd_lease_info_t *out_lease);

#ifdef __cplusplus
}
#endif

#endif /* GRIDD_NLS_CLIENT_H */
