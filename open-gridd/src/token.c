#define _GNU_SOURCE
#include "gridd/token.h"
#include "gridd/crypto.h"
#include "gridd/json_util.h"
#include "gridd/log.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static bool ends_with_tok(const char *name) {
    size_t len = strlen(name);
    return len > 4 && strcmp(name + len - 4, ".tok") == 0;
}

bool gridd_token_find_file(const char *path_or_dir, char *out_file, size_t out_sz) {
    if (!out_file || out_sz == 0) return false;
    out_file[0] = '\0';

    const char *target = (path_or_dir && *path_or_dir) ? path_or_dir : "/etc/nvidia/ClientConfigToken";
    struct stat st;
    if (stat(target, &st) != 0) {
        LOG_E("Unable to open directory (%s)", target);
        return false;
    }
    if (S_ISREG(st.st_mode)) {
        snprintf(out_file, out_sz, "%s", target);
        return true;
    }
    if (!S_ISDIR(st.st_mode)) {
        LOG_E("Unable to fetch the client configuration token file");
        return false;
    }

    DIR *dir = opendir(target);
    if (!dir) {
        LOG_E("Unable to open directory (%s)", target);
        return false;
    }

    struct dirent *de;
    bool found = false;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (ends_with_tok(de->d_name)) {
            snprintf(out_file, out_sz, "%s/%s", target, de->d_name);
            found = true;
            break;
        }
    }
    closedir(dir);

    if (!found) {
        LOG_E("Unable to fetch the client configuration token file");
    }
    return found;
}

static void parse_node_urls(const char *node_list_raw, gridd_token_config_t *cfg) {
    const char *p = node_list_raw;
    while (p && *p && cfg->node_count < GRIDD_MAX_NODES) {
        const char *hit = strstr(p, "\"url\"");
        if (!hit) break;
        char url_val[GRIDD_MAX_URL];
        if (gridd_json_get_string(hit - 1, "url", url_val, sizeof(url_val)) && url_val[0] != '\0') {
            snprintf(cfg->node_urls[cfg->node_count], GRIDD_MAX_URL, "%s", url_val);
            cfg->node_count++;
        }
        p = hit + 5;
    }
}

bool gridd_token_parse_and_verify(const char *path_or_dir,
                                  const char *cached_jti,
                                  uint64_t now_epoch,
                                  gridd_token_config_t *out_cfg,
                                  char *err_out, size_t err_sz) {
    if (!out_cfg) return false;
    memset(out_cfg, 0, sizeof(*out_cfg));
    if (err_out && err_sz > 0) err_out[0] = '\0';

    char tok_path[GRIDD_MAX_PATH];
    if (!gridd_token_find_file(path_or_dir, tok_path, sizeof(tok_path))) {
        if (err_out) snprintf(err_out, err_sz, "Unable to fetch the client configuration token file");
        return false;
    }

    FILE *fp = fopen(tok_path, "rb");
    if (!fp) {
        if (err_out) snprintf(err_out, err_sz, "Failed to open token file: %s", strerror(errno));
        return false;
    }

    char jwt[8192];
    size_t n = fread(jwt, 1, sizeof(jwt) - 1, fp);
    fclose(fp);
    jwt[n] = '\0';

    char header_b64[1024];
    char payload_b64[8192];
    char sig_b64[1024];
    if (!gridd_jwt_split(jwt, header_b64, sizeof(header_b64),
                         payload_b64, sizeof(payload_b64),
                         sig_b64, sizeof(sig_b64))) {
        LOG_E("Failed to process JWT token");
        if (err_out) snprintf(err_out, err_sz, "Invalid client configuration token - signature validation failed");
        return false;
    }

    uint8_t sig_raw[1024];
    size_t sig_len = 0;
    if (!gridd_base64_decode(sig_b64, sig_raw, sizeof(sig_raw), &sig_len)) {
        LOG_E("Failed to decode signature from token received");
        if (err_out) snprintf(err_out, err_sz, "Invalid client configuration token - signature validation failed");
        return false;
    }

    char payload_json[8192];
    size_t payload_len = 0;
    if (!gridd_base64_decode(payload_b64, (uint8_t *)payload_json, sizeof(payload_json) - 1, &payload_len)) {
        LOG_E("Failed to decode payload");
        if (err_out) snprintf(err_out, err_sz, "Invalid client configuration token - signature validation failed");
        return false;
    }
    payload_json[payload_len] = '\0';

    char pubkey_pem[2048];
    if (!gridd_json_get_string(payload_json, "service_instance_public_key_pem", pubkey_pem, sizeof(pubkey_pem)) ||
        pubkey_pem[0] == '\0') {
        LOG_E("Could not fetch value for service instance public key");
        if (err_out) snprintf(err_out, err_sz, "Invalid client configuration token - signature validation failed");
        return false;
    }

    char signing_input[9216];
    int si_len = snprintf(signing_input, sizeof(signing_input), "%s.%s", header_b64, payload_b64);
    if (si_len <= 0 || (size_t)si_len >= sizeof(signing_input) ||
        !gridd_verify_pem_signature(pubkey_pem, (const uint8_t *)signing_input, (size_t)si_len, sig_raw, sig_len)) {
        LOG_E("Failed to validate client configuration token");
        if (err_out) snprintf(err_out, err_sz, "Invalid client configuration token - signature validation failed");
        return false;
    }

    if (!gridd_json_get_string(payload_json, "nls_service_instance_ref",
                               out_cfg->service_instance_ref, sizeof(out_cfg->service_instance_ref))) {
        LOG_E("Could not fetch value for service instance reference");
    }

    gridd_json_get_string(payload_json, "jti", out_cfg->jti, sizeof(out_cfg->jti));

    if (!gridd_json_get_uint64(payload_json, "exp", &out_cfg->exp)) {
        if (err_out) snprintf(err_out, err_sz, "Could not fetch value for token expiry time");
        return false;
    }
    if (!gridd_json_get_uint64(payload_json, "nbf", &out_cfg->nbf)) {
        if (err_out) snprintf(err_out, err_sz, "Could not fetch value for token activation time");
        return false;
    }

    uint64_t now = (now_epoch != 0) ? now_epoch : (uint64_t)time(NULL);
    bool same_jti = (cached_jti && cached_jti[0] != '\0' && strcmp(cached_jti, out_cfg->jti) == 0);
    if (!same_jti) {
        if (out_cfg->exp < now) {
            if (err_out) {
                snprintf(err_out, err_sz,
                         "Client configuration token validation failed. Either the token has expired or system clock settings are tampered");
            }
            return false;
        }
        if (now < out_cfg->nbf) {
            if (err_out) {
                snprintf(err_out, err_sz,
                         "Client configuration token validation failed. Either the token is not activated or system clock settings are tampered");
            }
            return false;
        }
    }

    gridd_json_get_array_raw(payload_json, "fulfillment_class_ref_list",
                             out_cfg->fulfillment_class_ref_list, sizeof(out_cfg->fulfillment_class_ref_list));

    if (!gridd_json_get_array_raw(payload_json, "scope_ref_list",
                                  out_cfg->scope_ref_list, sizeof(out_cfg->scope_ref_list)) ||
        out_cfg->scope_ref_list[0] == '\0') {
        if (err_out) snprintf(err_out, err_sz, "Scope reference token is not configured");
        return false;
    }

    char svc_ports_raw[1024];
    if (!gridd_json_get_array_raw(payload_json, "svc_port_set_list", svc_ports_raw, sizeof(svc_ports_raw)) ||
        svc_ports_raw[0] == '\0') {
        if (err_out) snprintf(err_out, err_sz, "Could not fetch value for server port list");
        return false;
    }

    char d_name[64] = {0};
    gridd_json_get_string(svc_ports_raw, "d_name", d_name, sizeof(d_name));
    out_cfg->is_cls = (strncmp(d_name, "CLS", 3) == 0);

    const char *auth_entry = strstr(svc_ports_raw, "\"auth\"");
    uint64_t auth_port = 0;
    if (!auth_entry || !gridd_json_get_uint64(auth_entry, "port", &auth_port) || auth_port == 0) {
        if (err_out) snprintf(err_out, err_sz, "Could not fetch value for authentication server port");
        return false;
    }
    out_cfg->auth_port = (uint32_t)auth_port;

    const char *lease_entry = strstr(svc_ports_raw, "\"lease\"");
    uint64_t lease_port = 0;
    if (!lease_entry || !gridd_json_get_uint64(lease_entry, "port", &lease_port) || lease_port == 0) {
        if (err_out) snprintf(err_out, err_sz, "Could not fetch value for lease server port");
        return false;
    }
    out_cfg->lease_port = (uint32_t)lease_port;

    char node_list_raw[2048];
    if (!gridd_json_get_array_raw(payload_json, "node_url_list", node_list_raw, sizeof(node_list_raw)) ||
        node_list_raw[0] == '\0') {
        if (err_out) snprintf(err_out, err_sz, "Could not fetch value for node URL list");
        return false;
    }

    if (out_cfg->is_cls) {
        gridd_json_get_string(node_list_raw, "url_qr", out_cfg->url_qr, sizeof(out_cfg->url_qr));
    }
    parse_node_urls(node_list_raw, out_cfg);

    if (out_cfg->node_count == 0 || out_cfg->node_urls[0][0] == '\0' ||
        out_cfg->auth_port == 0 || out_cfg->lease_port == 0) {
        if (err_out) snprintf(err_out, err_sz, "Invalid client configuration token node/port configuration");
        return false;
    }

    return true;
}
