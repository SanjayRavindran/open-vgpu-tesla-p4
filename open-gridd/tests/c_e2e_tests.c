#define _GNU_SOURCE
#include "gridd/csp_platform.h"
#include "gridd/env_fingerprint.h"
#include "gridd/http_client.h"
#include "gridd/license_manager.h"
#include "gridd/log.h"
#include "gridd/nls_client.h"
#include "gridd/token.h"
#include "gridd/trusted_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>

#define TEST_ASSERT(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "TEST_ASSERT failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            exit(1); \
        } \
    } while (0)

static void build_test_env(gridd_environment_t *env) {
    memset(env, 0, sizeof(*env));
    snprintf(env->mac_address_list, sizeof(env->mac_address_list), "\"02:42:ac:11:00:02\"");
    snprintf(env->ip_address_list, sizeof(env->ip_address_list), "\"127.0.0.1\"");
    snprintf(env->hostname, sizeof(env->hostname), "e2e-test-host");
    snprintf(env->guest_driver_version, sizeof(env->guest_driver_version), "550.54.14");
    snprintf(env->os_platform, sizeof(env->os_platform), "Ubuntu 24.04");
    snprintf(env->os_version, sizeof(env->os_version), "24.04 LTS");
    snprintf(env->host_driver_version, sizeof(env->host_driver_version), "NA");
    snprintf(env->gpu_id_list, sizeof(env->gpu_id_list), "\"10DE-2230\"");
    snprintf(env->client_platform_id, sizeof(env->client_platform_id), "e2e-machine-uuid-001");
    snprintf(env->hv_platform, sizeof(env->hv_platform), "BareMetal");
    env->cpu_sockets = 1;
    env->physical_cores = 8;
}

int main(int argc, char **argv) {
    const char *tok_path = NULL;
    const char *store_dir = NULL;
    const char *imds_url = NULL;
    const char *imds_pub = NULL;

    for (int i = 1; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--nls-tok") == 0) tok_path = argv[i + 1];
        else if (strcmp(argv[i], "--store-dir") == 0) store_dir = argv[i + 1];
        else if (strcmp(argv[i], "--imds-url") == 0) imds_url = argv[i + 1];
        else if (strcmp(argv[i], "--imds-pub") == 0) imds_pub = argv[i + 1];
    }

    if (!tok_path || !store_dir || !imds_url || !imds_pub) {
        fprintf(stderr, "Usage: %s --nls-tok <path> --store-dir <dir> --imds-url <url> --imds-pub <path>\n", argv[0]);
        return 2;
    }

    gridd_http_global_init();
    gridd_log_init("c_e2e_tests", true, true);

    /* 1. Verify RSA-2048 JWT Client Configuration Token */
    gridd_token_config_t tok;
    char err[512] = {0};
    TEST_ASSERT(gridd_token_parse_and_verify(tok_path, NULL, 0, &tok, err, sizeof(err)));
    TEST_ASSERT(tok.is_cls == true);
    TEST_ASSERT(tok.node_count >= 1U);
    TEST_ASSERT(tok.auth_port > 0U && tok.lease_port > 0U);

    /* 2. Full NLS REST lifecycle + encrypted local trusted store */
    gridd_environment_t env;
    build_test_env(&env);
    gridd_http_opts_t http_opts = {.timeout_sec = 10};

    gridd_nls_client_t nls;
    TEST_ASSERT(gridd_nls_init(&nls, &tok, &env, &http_opts, store_dir, env.client_platform_id, 1440U) == GRIDD_OK);
    TEST_ASSERT(gridd_nls_register_or_update_origin(&nls) == GRIDD_OK);
    TEST_ASSERT(nls.store.data.registered == true);
    TEST_ASSERT(nls.store.data.origin_ref[0] != '\0');

    TEST_ASSERT(gridd_nls_authenticate(&nls) == GRIDD_OK);
    TEST_ASSERT(nls.auth_token[0] != '\0');

    TEST_ASSERT(gridd_nls_acquire_lease(&nls, "NVIDIA RTX Virtual Workstation", 1U) == GRIDD_OK);
    TEST_ASSERT(nls.has_active_lease == true);
    TEST_ASSERT(nls.lease.lease_ref[0] != '\0');

    TEST_ASSERT(gridd_nls_renew_lease(&nls, "NVIDIA RTX Virtual Workstation", 1U) == GRIDD_OK);
    TEST_ASSERT(nls.lease.refresh_count == 1U);

    char active_list[1024] = {0};
    TEST_ASSERT(gridd_nls_get_active_leases(&nls, active_list, sizeof(active_list)) == GRIDD_OK);
    TEST_ASSERT(strstr(active_list, nls.lease.lease_ref) != NULL);

    /* Reopen encrypted store independently & verify persistence + clock windback protection */
    gridd_trusted_store_t reopened;
    TEST_ASSERT(gridd_trusted_store_open(&reopened, store_dir, env.client_platform_id, tok.jti) == GRIDD_OK);
    TEST_ASSERT(strcmp(reopened.data.origin_ref, nls.store.data.origin_ref) == 0);
    TEST_ASSERT(strcmp(reopened.data.lease_state, "ACTIVE") == 0);
    TEST_ASSERT(gridd_trusted_store_check_clock(&reopened, 1000ULL) == GRIDD_ERR_CLOCK_WINDBACK);

    TEST_ASSERT(gridd_nls_return_leases(&nls) == GRIDD_OK);
    TEST_ASSERT(nls.has_active_lease == false);
    TEST_ASSERT(gridd_nls_shutdown(&nls) == GRIDD_OK);

    /* 3. Threaded License Manager end-to-end acquire + event pipe notification */
    gridd_license_manager_t lm;
    TEST_ASSERT(gridd_lm_create(&lm, &tok, &env, &http_opts, store_dir, env.client_platform_id, 60U) == GRIDD_OK);
    gridd_lm_acquire(&lm, "NVIDIA RTX Virtual Workstation", 1U);

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(lm.event_pipe_rd, &rfds);
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    int sel = select(lm.event_pipe_rd + 1, &rfds, NULL, NULL, &tv);
    TEST_ASSERT(sel > 0);
    TEST_ASSERT(gridd_lm_get_state(&lm) == GRIDD_LM_LICENSED);
    TEST_ASSERT(gridd_lm_get_lease_port(&lm) == tok.lease_port);
    TEST_ASSERT(gridd_lm_get_primary_url(&lm) != NULL);

    uint32_t exp_ep = 0;
    TEST_ASSERT(gridd_lm_get_expiry_and_rm_state(&lm, &exp_ep) == GRIDD_LICENSE_STATE_LICENSED);
    TEST_ASSERT(exp_ep > (uint32_t)time(NULL));
    gridd_lm_destroy(&lm);

    /* 4. Live AWS EC2 IMDSv2 + signature verification */
    TEST_ASSERT(gridd_csp_detect_and_verify(GRIDD_CSP_AWS_EC2, imds_pub, imds_url, 20U) == true);

    gridd_http_global_cleanup();
    printf("All C end-to-end integration tests passed.\n");
    return 0;
}
