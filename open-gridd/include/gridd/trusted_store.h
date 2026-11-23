#ifndef GRIDD_TRUSTED_STORE_H
#define GRIDD_TRUSTED_STORE_H

#include "gridd/env_fingerprint.h"
#include "gridd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char system_clock_snapshot[32];
    char jti[GRIDD_MAX_STR];
    char origin_ref[GRIDD_MAX_STR];
    bool registered;
    uint32_t failed_registration_attempts;
    gridd_environment_t env;
    char lease_list_json[4096];
    char lease_state[32];
} gridd_trusted_store_data_t;

typedef struct {
    char store_dir[GRIDD_MAX_PATH];
    char store_file[GRIDD_MAX_PATH];
    char tag_file[GRIDD_MAX_PATH];
    uint8_t key[32];
    gridd_trusted_store_data_t data;
} gridd_trusted_store_t;

gridd_status_t gridd_trusted_store_open(gridd_trusted_store_t *store,
                                        const char *store_dir,
                                        const char *machine_id,
                                        const char *jti);

gridd_status_t gridd_trusted_store_save(gridd_trusted_store_t *store);

gridd_status_t gridd_trusted_store_check_clock(gridd_trusted_store_t *store, uint64_t now_epoch);
bool gridd_trusted_store_match_fingerprint(const gridd_trusted_store_t *store,
                                           const gridd_environment_t *current_env);
void gridd_format_utc_iso8601(uint64_t epoch, char *out, size_t out_sz);
bool gridd_parse_utc_iso8601(const char *iso, uint64_t *out_epoch);

#ifdef __cplusplus
}
#endif

#endif /* GRIDD_TRUSTED_STORE_H */
