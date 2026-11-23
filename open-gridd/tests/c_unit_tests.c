#define _GNU_SOURCE
#include "gridd/config.h"
#include "gridd/crypto.h"
#include "gridd/json_util.h"
#include "gridd/log.h"
#include "gridd/nls_client.h"
#include "gridd/rm_client.h"
#include "gridd/token.h"
#include "gridd/trusted_store.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "TEST_ASSERT failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1); \
        } \
    } while (0)

static void test_config_and_feature_mapping(void) {
    const char *tmp_conf = "/tmp/gridd_test.conf";
    const char *tmp_swap = "/tmp/.gridd_test.conf_temp";
    FILE *fp = fopen(tmp_conf, "w");
    TEST_ASSERT(fp != NULL);
    fprintf(fp,
            "# Comment\n"
            "  FeatureType = 2 # vWS\n"
            "EnableUI=TRUE\n"
            "IgnoreSP=false\n"
            "LicenseInterval = 1440\n"
            "ProxyServerAddress=proxy.local\n"
            "ProxyServerPort=8080\n");
    fclose(fp);

    gridd_config_t cfg;
    TEST_ASSERT(gridd_config_load(tmp_conf, false, &cfg) == GRIDD_OK);
    TEST_ASSERT(cfg.feature_type == 2U);
    TEST_ASSERT(cfg.enable_ui == true);
    TEST_ASSERT(cfg.ignore_sp == false);
    TEST_ASSERT(cfg.license_interval == 1440U);
    TEST_ASSERT(strcmp(cfg.proxy_server_address, "proxy.local") == 0);
    TEST_ASSERT(strcmp(cfg.proxy_server_port, "8080") == 0);

    TEST_ASSERT(gridd_config_disable_ui(tmp_conf, tmp_swap));
    TEST_ASSERT(gridd_config_load(tmp_conf, false, &cfg) == GRIDD_OK);
    TEST_ASSERT(cfg.enable_ui == false);
    TEST_ASSERT(strcmp(gridd_feature_to_product_name("NVIDIA Cloud Gaming"), "NVIDIA vGaming") == 0);
    TEST_ASSERT(strcmp(gridd_feature_to_product_name("NVIDIA RTX Virtual Workstation"),
                  "NVIDIA RTX Virtual Workstation") == 0);
    TEST_ASSERT(strcmp(gridd_feature_to_product_name("Other"), "UNKNOWN") == 0);

    unlink(tmp_conf);
}

static void test_json_util(void) {
    const char *sample =
        "{\"ref\":\"lease-123\",\"count\":4,\"recommended_lease_renewal\":0.25,"
        "\"feature_expired\":false,\"pem\":\"LINE1\\nLINE2\",\"arr\":[\"a\",\"b\"]}";

    char s[128];
    TEST_ASSERT(gridd_json_get_string(sample, "ref", s, sizeof(s)));
    TEST_ASSERT(strcmp(s, "lease-123") == 0);

    TEST_ASSERT(gridd_json_get_string(sample, "pem", s, sizeof(s)));
    TEST_ASSERT(strcmp(s, "LINE1\nLINE2") == 0);

    uint64_t cnt = 0;
    TEST_ASSERT(gridd_json_get_uint64(sample, "count", &cnt) && cnt == 4);

    double rec = 0.0;
    TEST_ASSERT(gridd_json_get_double(sample, "recommended_lease_renewal", &rec) && rec > 0.24 && rec < 0.26);

    bool fexp = true;
    TEST_ASSERT(gridd_json_get_bool(sample, "feature_expired", &fexp) && fexp == false);

    TEST_ASSERT(gridd_json_get_array_raw(sample, "arr", s, sizeof(s)));
    TEST_ASSERT(strcmp(s, "\"a\",\"b\"") == 0);
}

static void test_crypto_and_trusted_store(void) {
    char verifier[128], challenge[128];
    TEST_ASSERT(gridd_pkce_generate(verifier, sizeof(verifier), challenge, sizeof(challenge)));
    TEST_ASSERT(strlen(verifier) >= 40 && strlen(challenge) >= 40);

    uint8_t key[32] = {1};
    uint8_t iv[12] = {2};
    const char *msg = "trusted-store-payload";
    uint8_t ct[64] = {0}, tag[16] = {0}, pt[64] = {0};
    TEST_ASSERT(gridd_aes256_gcm_encrypt(key, iv, (const uint8_t *)msg, strlen(msg), ct, tag));
    TEST_ASSERT(gridd_aes256_gcm_decrypt(key, iv, ct, strlen(msg), tag, pt));
    TEST_ASSERT(strcmp((const char *)pt, msg) == 0);

    tag[0] ^= 0xffU;
    TEST_ASSERT(!gridd_aes256_gcm_decrypt(key, iv, ct, strlen(msg), tag, pt));

    gridd_trusted_store_t store;
    const char *dir = "/tmp/gridd_store_test";
    TEST_ASSERT(gridd_trusted_store_open(&store, dir, "machine-uuid-1234", "jti-1") == GRIDD_OK);
    snprintf(store.data.origin_ref, sizeof(store.data.origin_ref), "origin-abc");
    store.data.registered = true;
    TEST_ASSERT(gridd_trusted_store_save(&store) == GRIDD_OK);

    gridd_trusted_store_t reopened;
    TEST_ASSERT(gridd_trusted_store_open(&reopened, dir, "machine-uuid-1234", "jti-1") == GRIDD_OK);
    TEST_ASSERT(strcmp(reopened.data.origin_ref, "origin-abc") == 0);
    TEST_ASSERT(reopened.data.registered == true);

    TEST_ASSERT(gridd_trusted_store_check_clock(&reopened, 100U) == GRIDD_ERR_CLOCK_WINDBACK);

    remove("/tmp/gridd_store_test/store_nls.dat");
    remove("/tmp/gridd_store_test/store_nls.tag");
    remove("/tmp/gridd_store_test");
}

typedef struct {
    uint32_t enabled_feature;
    uint32_t license_state;
    bool gsp_enabled;
} mock_rm_state_t;

static int mock_open(void *ctx) { (void)ctx; return 99; }
static void mock_close(void *ctx, int fd) { (void)ctx; (void)fd; }
static uint32_t mock_alloc(void *ctx, int fd, uint32_t hc, uint32_t hp, uint32_t ho,
                           uint32_t cls, void *p, uint32_t sz, uint32_t *out) {
    (void)ctx; (void)fd; (void)hc; (void)hp; (void)ho; (void)cls; (void)p; (void)sz;
    if (out) *out = 0xc1d00001U;
    return 0U;
}
static uint32_t mock_free(void *ctx, int fd, uint32_t hc, uint32_t hp, uint32_t ho) {
    (void)ctx; (void)fd; (void)hc; (void)hp; (void)ho;
    return 0U;
}
static uint32_t mock_ctrl(void *ctx, int fd, uint32_t hc, uint32_t ho,
                          uint32_t cmd, void *params, uint32_t sz) {
    (void)fd; (void)hc; (void)ho; (void)sz;
    mock_rm_state_t *st = (mock_rm_state_t *)ctx;
    uint8_t *b = (uint8_t *)params;
    if (cmd == NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS) {
        int32_t *ids = (int32_t *)params;
        ids[0] = 0x100;
        ids[1] = -1;
    } else if (cmd == NV2080_CTRL_CMD_GPU_GET_LICENSABLE_FEATURES) {
        b[0] = 1;
        uint32_t cnt = 1;
        memcpy(b + 0x84, &cnt, 4);
        uint32_t fcode = 2;
        memcpy(b + 0x88, &fcode, 4);
        snprintf((char *)(b + 0x8c), 255, "NVIDIA RTX Virtual Workstation");
    } else if (cmd == NV2080_CTRL_CMD_GSP_GET_FEATURES) {
        b[4] = st->gsp_enabled ? 1 : 0;
    } else if (cmd == NV2080_CTRL_CMD_GPU_ENABLE_LICENSED_FEATURE) {
        memcpy(&st->enabled_feature, b + 0x80, 4);
    } else if (cmd == NV2080_CTRL_CMD_GPU_SET_LICENSE_STATUS) {
        st->license_state = b[0x88];
    }
    return 0U;
}

static void test_rm_mock(void) {
    mock_rm_state_t mstate = {0};
    gridd_rm_ops_t ops = {
        .open_ctl = mock_open,
        .close_ctl = mock_close,
        .rm_alloc = mock_alloc,
        .rm_free = mock_free,
        .rm_control = mock_ctrl,
        .ctx = &mstate
    };
    gridd_rm_client_t rm;
    TEST_ASSERT(gridd_rm_init(&rm, &ops) == GRIDD_OK);
    TEST_ASSERT(rm.gpu_count == 1);
    TEST_ASSERT(gridd_rm_query_features(&rm) == GRIDD_OK);
    TEST_ASSERT(gridd_rm_gpu_supports_feature(&rm, 0, GRIDD_FEATURE_VWS, NULL));
    TEST_ASSERT(gridd_rm_enable_feature(&rm, GRIDD_FEATURE_VWS, NULL, NULL) == GRIDD_OK);
    TEST_ASSERT(mstate.enabled_feature == GRIDD_FEATURE_VWS);
    TEST_ASSERT(gridd_rm_set_license_status(&rm, GRIDD_FEATURE_VWS, 1900000000U, 3U) == GRIDD_OK);
    TEST_ASSERT(mstate.license_state == 3U);
    gridd_rm_destroy(&rm);
}

static void test_nls_payloads(void) {
    gridd_token_config_t tok;
    memset(&tok, 0, sizeof(tok));
    snprintf(tok.scope_ref_list, sizeof(tok.scope_ref_list), "\"scope-1\"");
    snprintf(tok.fulfillment_class_ref_list, sizeof(tok.fulfillment_class_ref_list), "\"fc-1\"");

    char out[2048];
    TEST_ASSERT(gridd_nls_build_lease_proposal_json(&tok, "NVIDIA RTX Virtual Workstation", 1, 1440,
                                               out, sizeof(out)));
    TEST_ASSERT(strstr(out, "\"duration\":\"P0Y0M0DT0H1440M0S\"") != NULL);
    TEST_ASSERT(strstr(out, "\"proposal_evaluation_mode\":\"ALL_OF\"") != NULL);

    gridd_lease_info_t lease;
    const char *resp =
        "{\"lease\":{\"ref\":\"lease-99\",\"product\":{\"name\":\"NVIDIA RTX Virtual Workstation\"},"
        "\"license_type_qualifiers\":{\"count\":1},\"expires\":\"2099-01-01T00:00:00\","
        "\"recommended_lease_renewal\":0.15,\"lease_state\":\"ACTIVE\",\"feature_expired\":false}}";
    TEST_ASSERT(gridd_nls_parse_lease_response(resp, 1700000000ULL, false, &lease));
    TEST_ASSERT(strcmp(lease.lease_ref, "lease-99") == 0);
    TEST_ASSERT(lease.count == 1U);

    /* FastAPI-DLS acquire & renew formats */
    gridd_lease_info_t dls_lease;
    memset(&dls_lease, 0, sizeof(dls_lease));
    const char *dls_acq =
        "{\"lease_result_list\":[{\"error\":null,\"lease\":{"
        "\"expires\":\"2099-01-01T00:00:00Z\","
        "\"feature_name\":\"GRID-Virtual-WS\","
        "\"product_name\":\"NVIDIA RTX Virtual Workstation\","
        "\"recommended_lease_renewal\":0.15,\"ref\":\"dls-lease-42\"}}]}";
    TEST_ASSERT(gridd_nls_parse_lease_response(dls_acq, 1700000000ULL, false, &dls_lease));
    TEST_ASSERT(strcmp(dls_lease.lease_ref, "dls-lease-42") == 0);
    TEST_ASSERT(strcmp(dls_lease.product_name, "NVIDIA RTX Virtual Workstation") == 0);
    TEST_ASSERT(dls_lease.count == 1U);

    const char *dls_ren =
        "{\"lease_ref\":\"dls-lease-42\",\"expires\":\"2099-01-02T00:00:00.123456\","
        "\"feature_expired\":false,\"recommended_lease_renewal\":0.15}";
    TEST_ASSERT(gridd_nls_parse_lease_response(dls_ren, 1700000000ULL, true, &dls_lease));
    TEST_ASSERT(strcmp(dls_lease.lease_ref, "dls-lease-42") == 0);
    TEST_ASSERT(strcmp(dls_lease.product_name, "NVIDIA RTX Virtual Workstation") == 0);
    TEST_ASSERT(dls_lease.count == 1U);
}

int main(void) {
    gridd_log_init("c_unit_tests", true, true);
    test_config_and_feature_mapping();
    test_json_util();
    test_crypto_and_trusted_store();
    test_rm_mock();
    test_nls_payloads();
    printf("All C unit tests passed.\n");
    return 0;
}
