#define _GNU_SOURCE
#include "gridd/csp_platform.h"
#include "gridd/config.h"
#include "gridd/crypto.h"
#include "gridd/http_client.h"
#include "gridd/json_util.h"
#include "gridd/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define GRIDD_SWCERT_MAX_BYTES 16384U

static const gridd_csp_info_t g_csp_registry[] = {
    {GRIDD_CSP_AZURE,       3U, "Microsoft Azure", true},
    {GRIDD_CSP_AWS_EC2,     1U, "Amazon EC2",      true},
    {GRIDD_CSP_GCP,         5U, "Google Cloud",    false},
    {GRIDD_CSP_ALIBABA_ECS, 2U, "Alibaba ECS",     true},
    {GRIDD_CSP_TENCENT,     0U, "Tencent",         false},
    {GRIDD_CSP_TENCENT_IEG, 0U, "Tencent IEG",     true}
};

const gridd_csp_info_t *gridd_csp_find_by_platform_id(uint32_t platform_id) {
    for (size_t i = 0; i < sizeof(g_csp_registry) / sizeof(g_csp_registry[0]); ++i) {
        if ((uint32_t)g_csp_registry[i].platform_id == platform_id) {
            return &g_csp_registry[i];
        }
    }
    return NULL;
}

static char *read_swcert_file(const char *path) {
    const char *p = (path && *path) ? path : GRIDD_DEFAULT_SWCERT_PATH;
    FILE *fp = fopen(p, "rb");
    if (!fp) {
        LOG_E("Failed to read certs (Error: %s)", strerror(errno));
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > GRIDD_SWCERT_MAX_BYTES) {
        LOG_E("Maximum supported file size has exceeded");
        fclose(fp);
        return NULL;
    }
    char *buf = (char *)calloc(1, (size_t)sz + 1U);
    if (!buf) {
        LOG_E("Failed to allocate memory");
        fclose(fp);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    return buf;
}

static bool aws_imds_verify(const char *swcert_pem, const char *imds_base_url, uint32_t wait_sec) {
    const char *base = (imds_base_url && *imds_base_url) ? imds_base_url : "http://169.254.169.254";
    uint32_t attempts = (wait_sec >= 20U && wait_sec <= 1200U) ? (wait_sec / 2U) : 60U;

    char token_url[256], doc_url[256], sig_url[256];
    snprintf(token_url, sizeof(token_url), "%s/latest/api/token", base);
    snprintf(doc_url, sizeof(doc_url), "%s/latest/dynamic/instance-identity/document", base);
    snprintf(sig_url, sizeof(sig_url), "%s/latest/dynamic/instance-identity/signature", base);

    gridd_http_opts_t opts = {.timeout_sec = 5};
    char imds_token[512] = {0};

    for (uint32_t i = 0; i < attempts; ++i) {
        const char *ttl_hdr[] = {"X-aws-ec2-metadata-token-ttl-seconds: 20"};
        gridd_http_response_t resp;
        if (gridd_http_request("PUT", token_url, ttl_hdr, 1, "", 0, &opts, &resp) == GRIDD_OK &&
            resp.status_code == 200 && resp.body && resp.body[0] != '\0') {
            snprintf(imds_token, sizeof(imds_token), "%s", resp.body);
            gridd_http_response_free(&resp);
            break;
        }
        gridd_http_response_free(&resp);
        sleep(2);
    }

    if (imds_token[0] == '\0') {
        LOG_E("Maximum number of retry attempts reached");
        return false;
    }

    char token_hdr[600];
    snprintf(token_hdr, sizeof(token_hdr), "X-aws-ec2-metadata-token: %s", imds_token);
    const char *hdrs[] = {token_hdr};

    gridd_http_response_t doc_resp = {0};
    gridd_http_response_t sig_resp = {0};
    bool ok = false;

    if (gridd_http_request("GET", doc_url, hdrs, 1, NULL, 0, &opts, &doc_resp) != GRIDD_OK ||
        doc_resp.status_code != 200) {
        goto cleanup;
    }
    if (gridd_http_request("GET", sig_url, hdrs, 1, NULL, 0, &opts, &sig_resp) != GRIDD_OK ||
        sig_resp.status_code != 200) {
        goto cleanup;
    }

    char region[128] = {0};
    if (!gridd_json_get_string(doc_resp.body, "region", region, sizeof(region))) {
        LOG_E("Malformed response - Could not find the expected tag");
        goto cleanup;
    }
    LOG_I("The value for region from instance identity document is %s", region);

    if (swcert_pem && strstr(swcert_pem, "BEGIN PKCS7") != NULL) {
        char verified[8192];
        ok = gridd_verify_pkcs7_pem(sig_resp.body, swcert_pem, verified, sizeof(verified));
    } else if (swcert_pem) {
        uint8_t sig_raw[1024];
        size_t sig_len = 0;
        if (gridd_base64_decode(sig_resp.body, sig_raw, sizeof(sig_raw), &sig_len)) {
            ok = gridd_verify_pem_signature(swcert_pem,
                                            (const uint8_t *)doc_resp.body, doc_resp.body_len,
                                            sig_raw, sig_len);
        }
    }

cleanup:
    gridd_http_response_free(&doc_resp);
    gridd_http_response_free(&sig_resp);
    return ok;
}

bool gridd_csp_detect_and_verify(uint32_t platform_id,
                                 const char *swcert_path,
                                 const char *imds_base_url,
                                 uint32_t wait_to_connect_sec) {
    const gridd_csp_info_t *info = gridd_csp_find_by_platform_id(platform_id);
    if (!info) return false;

    LOG_N("Platform initialized (%u)", platform_id);
    char *cert_pem = NULL;
    if (info->requires_swcert) {
        cert_pem = read_swcert_file(swcert_path);
        if (!cert_pem) {
            LOG_E("Marketplace detection failed (%u) - Could not read public certificate", platform_id);
            return false;
        }
    }

    bool verified = false;
    if (info->platform_id == GRIDD_CSP_AWS_EC2) {
        verified = aws_imds_verify(cert_pem, imds_base_url, wait_to_connect_sec);
    } else {
        verified = (cert_pem != NULL || !info->requires_swcert);
    }

    free(cert_pem);
    if (verified) {
        LOG_N("Platform detection successful (%u)", platform_id);
    } else {
        LOG_E("Platform detection failed (%u)", platform_id);
    }
    return verified;
}
