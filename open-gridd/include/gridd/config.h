#ifndef GRIDD_CONFIG_H
#define GRIDD_CONFIG_H

#include "gridd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GRIDD_DEFAULT_CONF_PATH "/etc/nvidia/gridd.conf"
#define GRIDD_DEFAULT_CONF_TEMP "/etc/nvidia/.gridd.conf_temp"
#define GRIDD_DEFAULT_TOKEN_DIR "/etc/nvidia/ClientConfigToken"
#define GRIDD_DEFAULT_STORE_DIR "/var/lib/nvidia/vGPULicensing"
#define GRIDD_DEFAULT_SWCERT_PATH "/etc/nvidia/GridSwCert.txt"

typedef struct {
    uint32_t feature_type;
    bool enable_ui;
    bool ignore_sp;
    bool ignore_node_locked;
    uint32_t license_interval;
    uint32_t marketplace_platform;
    uint32_t wait_to_connect_imds;
    char client_config_token_path[GRIDD_MAX_PATH];
    char proxy_server_address[GRIDD_MAX_PATH];
    char proxy_server_port[64];
    char proxy_user_name[GRIDD_MAX_STR];
    char proxy_credentials_file_path[GRIDD_MAX_PATH];
    char platform_identification[GRIDD_MAX_STR];
    char license_file_path[GRIDD_MAX_PATH];
} gridd_config_t;

void gridd_config_init_defaults(gridd_config_t *cfg);
gridd_status_t gridd_config_load(const char *path, bool is_vgpu_guest, gridd_config_t *cfg);
bool gridd_config_disable_ui(const char *path, const char *temp_path);
const char *gridd_feature_to_product_name(const char *rm_product_name);
uint32_t gridd_map_feature_type(uint32_t configured_feature_type);

#ifdef __cplusplus
}
#endif

#endif /* GRIDD_CONFIG_H */
