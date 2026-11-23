#define _GNU_SOURCE
#include "gridd/config.h"
#include "gridd/log.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void gridd_config_init_defaults(gridd_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->enable_ui = true;
    cfg->wait_to_connect_imds = 120;
    snprintf(cfg->client_config_token_path, sizeof(cfg->client_config_token_path),
             "%s", GRIDD_DEFAULT_TOKEN_DIR);
}

static bool parse_bool_true(const char *val) {
    return val && strcasecmp(val, "TRUE") == 0;
}

static void compact_line(const char *src, char *dst, size_t dst_sz) {
    size_t w = 0;
    for (size_t i = 0; src[i] != '\0' && src[i] != '\n' && src[i] != '#'; ++i) {
        if (src[i] == ' ' || src[i] == '\t' || src[i] == '\r') {
            continue;
        }
        if (w + 1 >= dst_sz) break;
        dst[w++] = src[i];
    }
    dst[w] = '\0';
}

static void apply_kv(gridd_config_t *cfg, const char *key, const char *val) {
    if (!val || !*val) return;

    if (strcmp(key, "FeatureType") == 0) {
        cfg->feature_type = (uint32_t)strtoul(val, NULL, 0);
        LOG_I("Parsed feature type : %u", cfg->feature_type);
    } else if (strcmp(key, "EnableUI") == 0) {
        cfg->enable_ui = parse_bool_true(val);
        LOG_I("Parsed enableUI : %d", cfg->enable_ui ? 1 : 0);
    } else if (strcmp(key, "IgnoreSP") == 0) {
        cfg->ignore_sp = parse_bool_true(val);
        LOG_I("Parsed ignoreSP : %d", cfg->ignore_sp ? 1 : 0);
    } else if (strcmp(key, "IgnoreNodeLocked") == 0) {
        cfg->ignore_node_locked = parse_bool_true(val);
        LOG_I("Parsed ignoreNodeLocked : %d", cfg->ignore_node_locked ? 1 : 0);
    } else if (strcmp(key, "LicenseInterval") == 0) {
        cfg->license_interval = (uint32_t)strtoul(val, NULL, 0);
        LOG_I("Parsed license interval : %u", cfg->license_interval);
    } else if (strcmp(key, "vGamingMarketplace") == 0 || strcmp(key, "MarketplaceLicensing") == 0) {
        cfg->marketplace_platform = (uint32_t)strtoul(val, NULL, 0);
        LOG_I("Parsed vGaming marketplace value : %u", cfg->marketplace_platform);
    } else if (strcmp(key, "ClientConfigTokenPath") == 0) {
        snprintf(cfg->client_config_token_path, sizeof(cfg->client_config_token_path), "%s", val);
        LOG_I("Parsed client configuration token path : %s", cfg->client_config_token_path);
    } else if (strcmp(key, "ProxyServerAddress") == 0) {
        snprintf(cfg->proxy_server_address, sizeof(cfg->proxy_server_address), "%s", val);
        LOG_I("Parsed proxy server address : %s", cfg->proxy_server_address);
    } else if (strcmp(key, "ProxyServerPort") == 0) {
        snprintf(cfg->proxy_server_port, sizeof(cfg->proxy_server_port), "%s", val);
        LOG_I("Parsed proxy server port : %s", cfg->proxy_server_port);
    } else if (strcmp(key, "ProxyUserName") == 0) {
        snprintf(cfg->proxy_user_name, sizeof(cfg->proxy_user_name), "%s", val);
    } else if (strcmp(key, "ProxyCredentialsFilePath") == 0) {
        snprintf(cfg->proxy_credentials_file_path, sizeof(cfg->proxy_credentials_file_path), "%s", val);
    } else if (strcmp(key, "WaitToConnectIMDS") == 0) {
        long w = strtol(val, NULL, 10);
        if (w > 0) {
            if (w < 20) w = 20;
            if (w > 1200) w = 1200;
            cfg->wait_to_connect_imds = (uint32_t)w;
        }
    } else if (strcmp(key, "PlatformIdentification") == 0) {
        snprintf(cfg->platform_identification, sizeof(cfg->platform_identification), "%s", val);
    } else if (strcmp(key, "LicenseFilePath") == 0) {
        snprintf(cfg->license_file_path, sizeof(cfg->license_file_path), "%s", val);
    }
}

gridd_status_t gridd_config_load(const char *path, bool is_vgpu_guest, gridd_config_t *cfg) {
    if (!cfg) return GRIDD_ERR_RM;
    gridd_config_init_defaults(cfg);

    if (is_vgpu_guest) {
        cfg->enable_ui = false;
        cfg->feature_type = GRIDD_FEATURE_VGPU;
    }

    const char *conf_path = (path && *path) ? path : GRIDD_DEFAULT_CONF_PATH;
    FILE *fp = fopen(conf_path, "r");
    if (!fp) {
        LOG_E("Failed to open vGPU license config file : %s error :%s", conf_path, strerror(errno));
        return GRIDD_ERR_INVALID_PARAM;
    }

    char raw[1024];
    char clean[1024];
    while (fgets(raw, sizeof(raw), fp)) {
        compact_line(raw, clean, sizeof(clean));
        if (clean[0] == '\0') continue;
        char *eq = strchr(clean, '=');
        if (!eq || eq == clean || eq[1] == '\0') continue;
        *eq = '\0';
        apply_kv(cfg, clean, eq + 1);
    }
    fclose(fp);

    if (is_vgpu_guest) {
        cfg->feature_type = GRIDD_FEATURE_VGPU;
    } else if (cfg->feature_type == GRIDD_FEATURE_UNLICENSED) {
        LOG_E("Feature Type (FeatureType) not set");
        return GRIDD_ERR_INVALID_PARAM;
    }

    return GRIDD_OK;
}

bool gridd_config_disable_ui(const char *path, const char *temp_path) {
    const char *conf_path = (path && *path) ? path : GRIDD_DEFAULT_CONF_PATH;
    const char *tmp_path = (temp_path && *temp_path) ? temp_path : GRIDD_DEFAULT_CONF_TEMP;

    FILE *fp = fopen(conf_path, "r");
    if (!fp) {
        LOG_D("Could not find vGPU license config file");
        fp = fopen(conf_path, "w");
        if (!fp) {
            LOG_E("Failed to create vGPU license config file");
            return false;
        }
        fwrite("EnableUI=FALSE\n", 1, 15, fp);
        fclose(fp);
        return true;
    }

    FILE *out = fopen(tmp_path, "w");
    if (!out) {
        LOG_E("Failed to create temporary vGPU license config file");
        fclose(fp);
        return false;
    }

    char line[1024];
    bool replaced = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "EnableUI") != NULL) {
            fwrite("EnableUI=FALSE\n", 1, 15, out);
            replaced = true;
        } else {
            fputs(line, out);
        }
    }
    if (!replaced) {
        fwrite("EnableUI=FALSE\n", 1, 15, out);
    }
    fclose(fp);
    fclose(out);

    if (rename(tmp_path, conf_path) != 0) {
        LOG_D("Failed to rename temporary config file to vGPU license config file");
        return false;
    }
    return true;
}

const char *gridd_feature_to_product_name(const char *rm_product_name) {
    if (!rm_product_name) return "UNKNOWN";
    if (strcmp(rm_product_name, "NVIDIA RTX Virtual Workstation") == 0) {
        return "NVIDIA RTX Virtual Workstation";
    }
    if (strcmp(rm_product_name, "NVIDIA Cloud Gaming") == 0) {
        return "NVIDIA vGaming";
    }
    if (strcmp(rm_product_name, "NVIDIA Virtual PC") == 0) {
        return "NVIDIA Virtual PC";
    }
    if (strcmp(rm_product_name, "NVIDIA Virtual Applications") == 0) {
        return "NVIDIA Virtual Applications";
    }
    if (strcmp(rm_product_name, "NVIDIA Virtual Compute Server") == 0) {
        return "NVIDIA Virtual Compute Server";
    }
    return "UNKNOWN";
}

uint32_t gridd_map_feature_type(uint32_t configured_feature_type) {
    static const uint32_t map[4] = {1U, 2U, 3U, 4U};
    if (configured_feature_type >= 1U && configured_feature_type <= 4U) {
        return map[configured_feature_type - 1U];
    }
    return 0U;
}
