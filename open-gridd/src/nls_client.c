#define _GNU_SOURCE
#include "gridd/nls_client.h"
#include "gridd/config.h"
#include "gridd/crypto.h"
#include "gridd/json_util.h"
#include "gridd/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static void build_base_url(const char *node_url, uint32_t port, char *out, size_t out_sz) {
    if (strncmp(node_url, "http://", 7) == 0 || strncmp(node_url, "https://", 8) == 0) {
        if (strchr(node_url + 8, ':') != NULL) {
            snprintf(out, out_sz, "%s", node_url);
        } else {
            snprintf(out, out_sz, "%s:%u", node_url, port);
        }
    } else {
        snprintf(out, out_sz, "https://%s:%u", node_url, port);
    }
}

static bool build_environment_object(const gridd_environment_t *env, gridd_json_buf_t *jb) {
    gridd_json_buf_append(jb, "{\"fingerprint\":{\"mac_address_list\":[");
    gridd_json_buf_append(jb, env->mac_address_list);
    gridd_json_buf_append(jb, "]},\"hostname\":");
    gridd_json_buf_append_escaped_str(jb, env->hostname);
    gridd_json_buf_append(jb, ",\"ip_address_list\":[");
    gridd_json_buf_append(jb, env->ip_address_list);
    gridd_json_buf_append(jb, "],\"guest_driver_version\":");
    gridd_json_buf_append_escaped_str(jb, env->guest_driver_version);
    gridd_json_buf_append(jb, ",\"os_platform\":");
    gridd_json_buf_append_escaped_str(jb, env->os_platform);
    gridd_json_buf_append(jb, ",\"os_version\":");
    gridd_json_buf_append_escaped_str(jb, env->os_version);
    gridd_json_buf_append(jb, ",\"host_driver_version\":");
    gridd_json_buf_append_escaped_str(jb, env->host_driver_version);
    gridd_json_buf_append(jb, ",\"gpu_id_list\":[");
    gridd_json_buf_append(jb, env->gpu_id_list);
    gridd_json_buf_append(jb, "],\"client_platform_id\":");
    gridd_json_buf_append_escaped_str(jb, env->client_platform_id);
    gridd_json_buf_append(jb, ",\"hv_platform\":");
    gridd_json_buf_append_escaped_str(jb, env->hv_platform);
    gridd_json_buf_appendf(jb, ",\"cpu_sockets\":%u,\"physical_cores\":%u}",
                           env->cpu_sockets, env->physical_cores);
    return !jb->overflow;
}

static gridd_status_t map_http_status(const gridd_http_response_t *resp, char *err_msg, size_t err_sz) {
    if (resp->status_code == 200) return GRIDD_OK;
    if (resp->status_code == 307) return GRIDD_ERR_REDIRECT;
    if (resp->status_code == 401) return GRIDD_ERR_AUTH;
    if (resp->status_code == 502) {
        if (err_msg) {
            snprintf(err_msg, err_sz, "Proxy responded with bad gateway. The server endpoint is not available");
        }
        return GRIDD_ERR_INVALID_PARAM;
    }

    char msg[256] = {0};
    if (resp->body && gridd_json_get_string(resp->body, "message", msg, sizeof(msg))) {
        if (err_msg) snprintf(err_msg, err_sz, "%s", msg);
        if (strcmp(msg, "no current lease found") == 0 ||
            strcmp(msg, "lease is already released") == 0 ||
            strcmp(msg, "no services instances found for origin") == 0 ||
            strcmp(msg, "No pool features found") == 0) {
            return GRIDD_ERR_NO_LEASE;
        }
    }
    return GRIDD_ERR_INVALID_PARAM;
}

static void handle_redirect_node(gridd_nls_client_t *nls, const char *body) {
    if (!nls || !body) return;
    char cand[GRIDD_MAX_URL] = {0};
    if (gridd_json_get_string(body, "fqdn", cand, sizeof(cand)) && strcmp(cand, "null") != 0) {
        for (uint32_t i = 0; i < nls->token.node_count; ++i) {
            if (strstr(nls->token.node_urls[i], cand) != NULL) {
                nls->active_node_idx = i;
                return;
            }
        }
    }
    if (gridd_json_get_string(body, "ipv4", cand, sizeof(cand)) && strcmp(cand, "null") != 0) {
        for (uint32_t i = 0; i < nls->token.node_count; ++i) {
            if (strstr(nls->token.node_urls[i], cand) != NULL) {
                nls->active_node_idx = i;
                return;
            }
        }
    }
    if (nls->token.node_count > 1) {
        nls->active_node_idx = (nls->active_node_idx + 1U) % nls->token.node_count;
    }
}

gridd_status_t gridd_nls_init(gridd_nls_client_t *nls,
                              const gridd_token_config_t *token,
                              const gridd_environment_t *env,
                              const gridd_http_opts_t *http_opts,
                              const char *store_dir,
                              const char *machine_id,
                              uint32_t license_interval_min) {
    if (!nls || !token || !env) return GRIDD_ERR_INVALID_PARAM;
    memset(nls, 0, sizeof(*nls));
    nls->token = *token;
    nls->env = *env;
    if (http_opts) nls->http_opts = *http_opts;
    else nls->http_opts.timeout_sec = 30;
    nls->license_interval_min = license_interval_min;

    gridd_status_t st = gridd_trusted_store_open(&nls->store, store_dir, machine_id, token->jti);
    if (st == GRIDD_ERR_CLOCK_WINDBACK) {
        return st;
    }
    if (!gridd_trusted_store_match_fingerprint(&nls->store, env)) {
        LOG_N("Client fingerprint mismatch - No valid lease found in local trusted store");
        nls->store.data.lease_list_json[0] = '\0';
        nls->store.data.lease_state[0] = '\0';
    }
    nls->store.data.env = *env;
    gridd_trusted_store_save(&nls->store);
    return GRIDD_OK;
}

static void generate_uuid_v4(char *out, size_t out_sz) {
    if (!out || out_sz < 37U) {
        if (out && out_sz > 0) out[0] = '\0';
        return;
    }
    uint8_t b[16];
    if (!gridd_random_bytes(b, sizeof(b))) {
        out[0] = '\0';
        return;
    }
    b[6] = (uint8_t)((b[6] & 0x0fU) | 0x40U);
    b[8] = (uint8_t)((b[8] & 0x3fU) | 0x80U);
    snprintf(out, out_sz,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

gridd_status_t gridd_nls_register_or_update_origin(gridd_nls_client_t *nls) {
    if (!nls) return GRIDD_ERR_INVALID_PARAM;
    if (nls->store.data.origin_ref[0] == '\0') {
        generate_uuid_v4(nls->store.data.origin_ref, sizeof(nls->store.data.origin_ref));
    }
    const char *node = nls->token.node_urls[nls->active_node_idx];
    char base[256], url[320];
    build_base_url(node, nls->token.auth_port, base, sizeof(base));
    snprintf(url, sizeof(url), "%s/auth/v1/origin", base);

    gridd_json_buf_t jb;
    gridd_json_buf_init(&jb, 2048);
    gridd_json_buf_append(&jb, "{\"service_instance_ref\":");
    gridd_json_buf_append_escaped_str(&jb, nls->token.service_instance_ref);
    gridd_json_buf_append(&jb, ",\"candidate_origin_ref\":");
    gridd_json_buf_append_escaped_str(&jb, nls->store.data.origin_ref);
    gridd_json_buf_append(&jb, ",\"environment\":");
    build_environment_object(&nls->env, &jb);
    gridd_json_buf_appendf(&jb, ",\"registration_pending\":%s,\"update_pending\":false}",
                           nls->store.data.registered ? "false" : "true");

    if (jb.overflow || jb.len >= GRIDD_MAX_TCP_PAYLOAD) {
        gridd_json_buf_free(&jb);
        LOG_E("Request size exceeded maximum support TCP packet size");
        return GRIDD_ERR_INVALID_PARAM;
    }

    const char *hdrs[] = {"Content-Type: application/json"};
    gridd_http_response_t resp;
    gridd_status_t st = gridd_http_request("POST", url, hdrs, 1, jb.data, jb.len, &nls->http_opts, &resp);
    gridd_json_buf_free(&jb);
    if (st != GRIDD_OK) {
        snprintf(nls->last_error, sizeof(nls->last_error), "%s", resp.err_msg);
        gridd_http_response_free(&resp);
        return st;
    }

    st = map_http_status(&resp, nls->last_error, sizeof(nls->last_error));
    if (st == GRIDD_OK) {
        char origin_ref[GRIDD_MAX_STR] = {0};
        if (gridd_json_get_string(resp.body, "origin_ref", origin_ref, sizeof(origin_ref)) &&
            origin_ref[0] != '\0') {
            snprintf(nls->store.data.origin_ref, sizeof(nls->store.data.origin_ref), "%s", origin_ref);
            nls->store.data.registered = true;
            nls->store.data.failed_registration_attempts = 0;
            gridd_trusted_store_save(&nls->store);
        }
    } else {
        if (strstr(nls->last_error, "already in use by different fingerprint") != NULL ||
            strncmp(nls->last_error, "invalid origin environment", 26) == 0) {
            nls->store.data.origin_ref[0] = '\0';
            nls->store.data.registered = false;
        }
        nls->store.data.failed_registration_attempts++;
        gridd_trusted_store_save(&nls->store);
    }

    gridd_http_response_free(&resp);
    return st;
}

gridd_status_t gridd_nls_authenticate(gridd_nls_client_t *nls) {
    if (!nls) return GRIDD_ERR_INVALID_PARAM;
    uint64_t now = (uint64_t)time(NULL);
    if (nls->auth_token[0] != '\0' && nls->auth_token_expires_epoch > now + 30U) {
        return GRIDD_OK;
    }

    if (!nls->store.data.registered || nls->store.data.origin_ref[0] == '\0') {
        gridd_status_t rst = gridd_nls_register_or_update_origin(nls);
        if (rst != GRIDD_OK) return rst;
    }

    char verifier[128], challenge[128];
    if (!gridd_pkce_generate(verifier, sizeof(verifier), challenge, sizeof(challenge))) {
        return GRIDD_ERR_GENERIC;
    }

    const char *node = nls->token.node_urls[nls->active_node_idx];
    char base[256], url[320];
    build_base_url(node, nls->token.auth_port, base, sizeof(base));
    snprintf(url, sizeof(url), "%s/auth/v1/code", base);

    gridd_json_buf_t jb;
    gridd_json_buf_init(&jb, 512);
    gridd_json_buf_append(&jb, "{\"code_challenge\":");
    gridd_json_buf_append_escaped_str(&jb, challenge);
    gridd_json_buf_append(&jb, ",\"origin_ref\":");
    gridd_json_buf_append_escaped_str(&jb, nls->store.data.origin_ref);
    gridd_json_buf_append(&jb, ",\"service_instance_ref\":");
    gridd_json_buf_append_escaped_str(&jb, nls->token.service_instance_ref);
    gridd_json_buf_append(&jb, "}");

    const char *hdrs[] = {"Content-Type: application/json"};
    gridd_http_response_t resp;
    gridd_status_t st = gridd_http_request("POST", url, hdrs, 1, jb.data, jb.len, &nls->http_opts, &resp);
    gridd_json_buf_free(&jb);
    if (st != GRIDD_OK) {
        snprintf(nls->last_error, sizeof(nls->last_error), "%s", resp.err_msg);
        gridd_http_response_free(&resp);
        return st;
    }

    st = map_http_status(&resp, nls->last_error, sizeof(nls->last_error));
    char auth_code[2048] = {0};
    if (st == GRIDD_OK && !gridd_json_get_string(resp.body, "auth_code", auth_code, sizeof(auth_code))) {
        LOG_E("Malformed response - Could not find the expected tag");
        st = GRIDD_ERR_INVALID_PARAM;
    }
    gridd_http_response_free(&resp);
    if (st != GRIDD_OK) return st;

    snprintf(url, sizeof(url), "%s/auth/v1/token", base);
    gridd_json_buf_init(&jb, 512);
    gridd_json_buf_append(&jb, "{\"auth_code\":");
    gridd_json_buf_append_escaped_str(&jb, auth_code);
    gridd_json_buf_append(&jb, ",\"code_verifier\":");
    gridd_json_buf_append_escaped_str(&jb, verifier);
    gridd_json_buf_append(&jb, "}");

    st = gridd_http_request("POST", url, hdrs, 1, jb.data, jb.len, &nls->http_opts, &resp);
    gridd_json_buf_free(&jb);
    if (st != GRIDD_OK) {
        snprintf(nls->last_error, sizeof(nls->last_error), "%s", resp.err_msg);
        gridd_http_response_free(&resp);
        return st;
    }

    st = map_http_status(&resp, nls->last_error, sizeof(nls->last_error));
    if (st == GRIDD_OK) {
        char exp_iso[64] = {0};
        if (!gridd_json_get_string(resp.body, "auth_token", nls->auth_token, sizeof(nls->auth_token))) {
            st = GRIDD_ERR_INVALID_PARAM;
        } else if (gridd_json_get_string(resp.body, "expires", exp_iso, sizeof(exp_iso))) {
            gridd_parse_utc_iso8601(exp_iso, &nls->auth_token_expires_epoch);
        }
    }
    gridd_http_response_free(&resp);
    return st;
}

bool gridd_nls_build_lease_proposal_json(const gridd_token_config_t *token,
                                         const char *product_name,
                                         uint32_t count,
                                         uint32_t duration_min,
                                         char *out_buf,
                                         size_t out_sz) {
    if (!token || !product_name || !out_buf || out_sz == 0) return false;
    gridd_json_buf_t jb;
    gridd_json_buf_init(&jb, 1024);
    gridd_json_buf_append(&jb, "{\"fulfillment_context\":{\"fulfillment_class_ref_list\":[");
    gridd_json_buf_append(&jb, token->fulfillment_class_ref_list);
    gridd_json_buf_append(&jb, "]},\"lease_proposal_list\":[{");
    if (duration_min > 0) {
        gridd_json_buf_appendf(&jb, "\"duration\":\"P0Y0M0DT0H%uM0S\",", duration_min);
    }
    gridd_json_buf_appendf(&jb, "\"license_type_qualifiers\":{\"count\":%u},\"product\":{\"name\":", count);
    gridd_json_buf_append_escaped_str(&jb, product_name);
    gridd_json_buf_append(&jb, "}}],\"proposal_evaluation_mode\":\"ALL_OF\",\"scope_ref_list\":[");
    gridd_json_buf_append(&jb, token->scope_ref_list);
    gridd_json_buf_append(&jb, "]}");

    bool ok = (!jb.overflow && jb.len < out_sz && jb.len < GRIDD_MAX_TCP_PAYLOAD);
    if (ok) {
        memcpy(out_buf, jb.data, jb.len + 1U);
    }
    gridd_json_buf_free(&jb);
    return ok;
}

bool gridd_nls_parse_lease_response(const char *json,
                                    uint64_t now_epoch,
                                    bool is_renew,
                                    gridd_lease_info_t *out_lease) {
    if (!json || !out_lease) return false;
    char state[64] = {0};
    gridd_json_get_string(json, "lease_state", state, sizeof(state));
    if (strcmp(state, "EXPIRED") == 0) {
        return false;
    }

    char prod_buf[GRIDD_MAX_STR] = {0};
    if (gridd_json_get_string(json, "name", prod_buf, sizeof(prod_buf)) ||
        gridd_json_get_string(json, "product_name", prod_buf, sizeof(prod_buf)) ||
        gridd_json_get_string(json, "feature_name", prod_buf, sizeof(prod_buf))) {
        snprintf(out_lease->product_name, sizeof(out_lease->product_name), "%s", prod_buf);
    } else if (!is_renew || out_lease->product_name[0] == '\0') {
        return false;
    }
    uint64_t cnt = 0;
    if (gridd_json_get_uint64(json, "count", &cnt) && cnt > 0) {
        out_lease->count = (uint32_t)cnt;
    } else if (!is_renew || out_lease->count == 0) {
        out_lease->count = 1U;
    }

    char ref[GRIDD_MAX_STR] = {0};
    if (is_renew) {
        if (!gridd_json_get_string(json, "lease_ref", ref, sizeof(ref))) return false;
    } else {
        if (!gridd_json_get_string(json, "ref", ref, sizeof(ref)) &&
            !gridd_json_get_string(json, "lease_ref", ref, sizeof(ref))) {
            return false;
        }
    }
    snprintf(out_lease->lease_ref, sizeof(out_lease->lease_ref), "%s", ref);

    if (!gridd_json_get_string(json, "expires", out_lease->expires_iso, sizeof(out_lease->expires_iso))) {
        return false;
    }
    uint64_t exp_ep = 0;
    if (!gridd_parse_utc_iso8601(out_lease->expires_iso, &exp_ep) || exp_ep <= now_epoch) {
        return false;
    }
    out_lease->expires_epoch = exp_ep;
    out_lease->lease_duration_sec = (double)(exp_ep - now_epoch);

    if (!gridd_json_get_double(json, "recommended_lease_renewal", &out_lease->recommended_renewal)) {
        return false;
    }
    out_lease->feature_expired = false;
    gridd_json_get_bool(json, "feature_expired", &out_lease->feature_expired);
    return true;
}

static gridd_status_t leased_request(gridd_nls_client_t *nls,
                                     const char *method,
                                     const char *path,
                                     const char *body,
                                     gridd_http_response_t *resp) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        gridd_status_t st = gridd_nls_authenticate(nls);
        if (st != GRIDD_OK) return st;

        const char *node = nls->token.node_urls[nls->active_node_idx];
        char base[256], url[512];
        build_base_url(node, nls->token.lease_port, base, sizeof(base));
        snprintf(url, sizeof(url), "%s%s", base, path);

        char auth_hdr[4200];
        snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", nls->auth_token);
        const char *hdrs[] = {"Content-Type: application/json", auth_hdr};

        st = gridd_http_request(method, url, hdrs, 2, body, body ? strlen(body) : 0,
                                &nls->http_opts, resp);
        if (st != GRIDD_OK) {
            snprintf(nls->last_error, sizeof(nls->last_error), "%s", resp->err_msg);
            return st;
        }
        st = map_http_status(resp, nls->last_error, sizeof(nls->last_error));
        if (st == GRIDD_ERR_AUTH && attempt == 0) {
            nls->auth_token[0] = '\0';
            gridd_http_response_free(resp);
            continue;
        }
        if (st == GRIDD_ERR_REDIRECT) {
            handle_redirect_node(nls, resp->body);
        }
        return st;
    }
    return GRIDD_ERR_AUTH;
}

gridd_status_t gridd_nls_acquire_lease(gridd_nls_client_t *nls, const char *product_name, uint32_t count) {
    if (!nls || !product_name) return GRIDD_ERR_INVALID_PARAM;

    char payload[GRIDD_MAX_TCP_PAYLOAD];
    if (!gridd_nls_build_lease_proposal_json(&nls->token, product_name, count,
                                             nls->license_interval_min, payload, sizeof(payload))) {
        return GRIDD_ERR_INVALID_PARAM;
    }

    gridd_http_response_t resp;
    gridd_status_t st = leased_request(nls, "POST", "/leasing/v1/lessor", payload, &resp);
    if (st == GRIDD_OK) {
        if (gridd_nls_parse_lease_response(resp.body, (uint64_t)time(NULL), false, &nls->lease)) {
            nls->has_active_lease = true;
            nls->lease.refresh_count = 0;
            snprintf(nls->store.data.lease_state, sizeof(nls->store.data.lease_state), "ACTIVE");
            snprintf(nls->store.data.lease_list_json, sizeof(nls->store.data.lease_list_json),
                     "%s", resp.body);
            gridd_format_utc_iso8601((uint64_t)time(NULL),
                                     nls->store.data.system_clock_snapshot,
                                     sizeof(nls->store.data.system_clock_snapshot));
            gridd_trusted_store_save(&nls->store);
        } else {
            st = GRIDD_ERR_INVALID_PARAM;
        }
    }
    gridd_http_response_free(&resp);
    return st;
}

gridd_status_t gridd_nls_renew_lease(gridd_nls_client_t *nls, const char *product_name, uint32_t count) {
    (void)product_name;
    (void)count;
    if (!nls || !nls->has_active_lease) return GRIDD_ERR_NO_LEASE;

    char path[384];
    snprintf(path, sizeof(path), "/leasing/v1/lease/%s", nls->lease.lease_ref);

    gridd_http_response_t resp;
    gridd_status_t st = leased_request(nls, "PUT", path, "{}", &resp);
    if (st == GRIDD_OK) {
        if (gridd_nls_parse_lease_response(resp.body, (uint64_t)time(NULL), true, &nls->lease)) {
            nls->lease.refresh_count++;
            gridd_format_utc_iso8601((uint64_t)time(NULL),
                                     nls->store.data.system_clock_snapshot,
                                     sizeof(nls->store.data.system_clock_snapshot));
            gridd_trusted_store_save(&nls->store);
        } else {
            st = GRIDD_ERR_INVALID_PARAM;
        }
    }
    gridd_http_response_free(&resp);
    return st;
}

gridd_status_t gridd_nls_get_active_leases(gridd_nls_client_t *nls, char *out_buf, size_t out_sz) {
    if (!nls) return GRIDD_ERR_INVALID_PARAM;
    gridd_http_response_t resp;
    gridd_status_t st = leased_request(nls, "GET", "/leasing/v1/lessor/leases", NULL, &resp);
    if (st == GRIDD_OK && out_buf && out_sz > 0) {
        gridd_json_get_array_raw(resp.body, "active_lease_list", out_buf, out_sz);
    }
    gridd_http_response_free(&resp);
    return st;
}

gridd_status_t gridd_nls_return_leases(gridd_nls_client_t *nls) {
    if (!nls) return GRIDD_ERR_INVALID_PARAM;
    gridd_http_response_t resp;
    gridd_status_t st = leased_request(nls, "DELETE", "/leasing/v1/lessor/leases", NULL, &resp);
    gridd_http_response_free(&resp);
    if (st == GRIDD_OK || st == GRIDD_ERR_NO_LEASE) {
        nls->has_active_lease = false;
        nls->store.data.lease_list_json[0] = '\0';
        nls->store.data.lease_state[0] = '\0';
        gridd_trusted_store_save(&nls->store);
        LOG_N("License returned successfully.");
        return GRIDD_OK;
    }
    return st;
}

gridd_status_t gridd_nls_shutdown(gridd_nls_client_t *nls) {
    if (!nls) return GRIDD_ERR_INVALID_PARAM;
    gridd_http_response_t resp;
    gridd_status_t st = leased_request(nls, "POST", "/leasing/v1/lessor/shutdown", "{}", &resp);
    gridd_http_response_free(&resp);
    return st;
}

bool gridd_nls_try_nll(const char *conf_path,
                       const char *os_platform,
                       const char *arch,
                       gridd_lease_info_t *out_lease) {
    if (!out_lease) return false;
    gridd_config_t cfg;
    gridd_config_load(conf_path, false, &cfg);
    if (cfg.platform_identification[0] == '\0' || cfg.license_file_path[0] == '\0') {
        return false;
    }

    char expected[256];
    snprintf(expected, sizeof(expected), "%s,%s",
             os_platform ? os_platform : "Linux",
             arch ? arch : "x86_64");
    if (strncmp(cfg.platform_identification, expected, strlen(expected)) != 0) {
        return false;
    }

    FILE *fp = fopen(cfg.license_file_path, "rb");
    if (!fp) return false;
    char buf[8192] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';

    memset(out_lease, 0, sizeof(*out_lease));
    if (!gridd_json_get_string(buf, "name", out_lease->product_name, sizeof(out_lease->product_name))) {
        return false;
    }
    uint64_t cnt = 1;
    gridd_json_get_uint64(buf, "count", &cnt);
    out_lease->count = (uint32_t)cnt;
    if (!gridd_json_get_string(buf, "expires", out_lease->expires_iso, sizeof(out_lease->expires_iso))) {
        return false;
    }
    if (!gridd_parse_utc_iso8601(out_lease->expires_iso, &out_lease->expires_epoch)) {
        return false;
    }
    return out_lease->expires_epoch > (uint64_t)time(NULL);
}
