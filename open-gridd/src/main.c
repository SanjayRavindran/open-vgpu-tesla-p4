#define _GNU_SOURCE
#include "gridd/config.h"
#include "gridd/csp_platform.h"
#include "gridd/daemon.h"
#include "gridd/dbus_server.h"
#include "gridd/env_fingerprint.h"
#include "gridd/http_client.h"
#include "gridd/license_manager.h"
#include "gridd/log.h"
#include "gridd/nls_client.h"
#include "gridd/rm_client.h"
#include "gridd/token.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

static bool select_product_for_feature(gridd_rm_client_t *rm,
                                       uint32_t mapped_feature,
                                       char *product_out, size_t prod_sz,
                                       uint32_t *count_out) {
    if (!rm || !product_out || !count_out) return false;
    *count_out = 0;
    product_out[0] = '\0';

    if (gridd_rm_query_features(rm) != GRIDD_OK) {
        return false;
    }

    for (uint32_t i = 0; i < rm->gpu_count; ++i) {
        uint32_t fidx = 0;
        if (!gridd_rm_gpu_supports_feature(rm, i, mapped_feature, &fidx)) {
            continue;
        }
        const char *raw_name = rm->gpus[i].features[fidx].product_name;
        const char *norm = gridd_feature_to_product_name(raw_name);
        snprintf(product_out, prod_sz, "%s", norm);
        if (strstr(raw_name, "NVIDIA Virtual Compute Server") == NULL) {
            *count_out = 1U;
            return true;
        }
        (*count_out)++;
    }
    return *count_out > 0;
}

static void drain_pipe(int fd) {
    uint32_t tmp = 0;
    ssize_t unused = read(fd, &tmp, sizeof(tmp));
    (void)unused;
}

static bool wait_for_token(gridd_daemon_ctx_t *daemon, const char *token_path) {
    uint32_t wait_ms = 10000U;
    uint32_t elapsed_ms = 0U;

    while (!gridd_daemon_stop_requested() && elapsed_ms < 300000U) {
        char found[GRIDD_MAX_PATH];
        if (gridd_token_find_file(token_path, found, sizeof(found))) {
            return true;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(daemon->exit_pipe_rd, &rfds);
        struct timeval tv = {.tv_sec = (long)(wait_ms / 1000U), .tv_usec = 0};
        int rc = select(daemon->exit_pipe_rd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0 && errno != EINTR) break;
        if (gridd_daemon_stop_requested()) break;
        elapsed_ms += wait_ms;
        wait_ms *= 2U;
    }
    return false;
}

int main(int argc, char **argv) {
    bool verbose = false;
    bool foreground = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-v") == 0) verbose = true;
        else if (strcmp(argv[i], "-f") == 0) foreground = true;
    }

    gridd_http_global_init();
    gridd_daemon_ctx_t daemon;
    if (gridd_daemon_init(&daemon, verbose, foreground) != GRIDD_OK) {
        gridd_daemon_cleanup(&daemon, 1);
        gridd_http_global_cleanup();
        return 1;
    }

    int exit_code = 0;
    gridd_rm_client_t rm;
    if (gridd_rm_init(&rm, NULL) != GRIDD_OK) {
        gridd_daemon_cleanup(&daemon, 1);
        gridd_http_global_cleanup();
        return 1;
    }

    gridd_rm_set_sw_persistence(&rm, true);
    gridd_rm_start_unlicensed_monitor(&rm, daemon.exit_pipe_rd);

    gridd_config_t cfg;
    gridd_config_load(GRIDD_DEFAULT_CONF_PATH, rm.is_vgpu_guest, &cfg);
    uint32_t active_feature = cfg.feature_type;

    pthread_t dbus_tid = 0;
    bool dbus_started = false;
    gridd_license_manager_t lm;
    bool lm_started = false;

    gridd_dbus_ctx_t dbus_ctx = {
        .lm = NULL,
        .active_feature_type = &active_feature,
        .dbus_reload_pipe_wr = daemon.dbus_pipe_wr,
        .stop_pipe_rd = daemon.exit_pipe_rd
    };
    if (cfg.enable_ui && daemon.dbus_pipe_rd >= 0) {
        if (pthread_create(&dbus_tid, NULL, gridd_dbus_thread_main, &dbus_ctx) == 0) {
            dbus_started = true;
        } else {
            LOG_E("Failed to setup D-Bus connection");
        }
    }

    if (gridd_ensure_state_dirs() != GRIDD_OK) {
        exit_code = 1;
        goto shutdown;
    }

    if (cfg.marketplace_platform != 0) {
        if (gridd_csp_detect_and_verify(cfg.marketplace_platform,
                                        GRIDD_DEFAULT_SWCERT_PATH,
                                        NULL,
                                        cfg.wait_to_connect_imds)) {
            uint32_t mapped = gridd_map_feature_type(cfg.feature_type);
            if (mapped == 0) mapped = GRIDD_FEATURE_VWS;
            bool reboot_req = false;
            uint32_t running_feat = mapped;
            if (gridd_rm_enable_feature(&rm, mapped, &running_feat, &reboot_req) == GRIDD_OK &&
                !reboot_req) {
                gridd_rm_set_license_status(&rm, mapped, 0, GRIDD_LICENSE_STATE_LICENSED);
                gridd_config_disable_ui(GRIDD_DEFAULT_CONF_PATH, GRIDD_DEFAULT_CONF_TEMP);
            }
            goto shutdown;
        }
    }

    gridd_environment_t env;
    gridd_env_collect(&rm, &env);

    gridd_lease_info_t nll_lease;
    if (!cfg.ignore_node_locked &&
        gridd_nls_try_nll(GRIDD_DEFAULT_CONF_PATH, env.os_platform, "x86_64", &nll_lease)) {
        uint32_t mapped = gridd_map_feature_type(cfg.feature_type);
        bool reboot_req = false;
        if (gridd_rm_enable_feature(&rm, mapped, NULL, &reboot_req) == GRIDD_OK && !reboot_req) {
            gridd_rm_set_license_status(&rm, mapped, (uint32_t)nll_lease.expires_epoch,
                                        GRIDD_LICENSE_STATE_LICENSED);
            gridd_config_disable_ui(GRIDD_DEFAULT_CONF_PATH, GRIDD_DEFAULT_CONF_TEMP);
        }
        goto shutdown;
    }

    char tok_path[GRIDD_MAX_PATH];
    if (!gridd_token_find_file(cfg.client_config_token_path, tok_path, sizeof(tok_path))) {
        if (!wait_for_token(&daemon, cfg.client_config_token_path)) {
            LOG_E("Valid license settings not found. Please configure settings to use NVIDIA License System (NLS). NVIDIA Legacy License Server is not supported.");
            goto shutdown;
        }
    }

    gridd_token_config_t tok;
    char tok_err[512] = {0};
    if (!gridd_token_parse_and_verify(cfg.client_config_token_path, NULL, 0, &tok, tok_err, sizeof(tok_err))) {
        LOG_E("Failed to read configurations from client configuration token (Error: %s)", tok_err);
        goto shutdown;
    }
    LOG_N("NLS initialized");
    if (!rm.is_vgpu_guest || access(GRIDD_DEFAULT_CONF_PATH, F_OK) == 0) {
        gridd_config_disable_ui(GRIDD_DEFAULT_CONF_PATH, GRIDD_DEFAULT_CONF_TEMP);
    }

    gridd_http_opts_t http_opts;
    gridd_http_load_proxy_opts(GRIDD_DEFAULT_CONF_PATH, &http_opts);

    if (gridd_lm_create(&lm, &tok, &env, &http_opts, GRIDD_DEFAULT_STORE_DIR,
                        env.client_platform_id, cfg.license_interval) != GRIDD_OK) {
        LOG_E("Failed to setup cloud License Manager");
        exit_code = 1;
        goto shutdown;
    }
    lm_started = true;
    dbus_ctx.lm = &lm;

    char prod_name[GRIDD_MAX_STR];
    uint32_t prod_count = 1;
    uint32_t mapped_feat = gridd_map_feature_type(cfg.feature_type);
    if (select_product_for_feature(&rm, mapped_feat, prod_name, sizeof(prod_name), &prod_count)) {
        gridd_lm_acquire(&lm, prod_name, prod_count);
    }

    while (!gridd_daemon_stop_requested()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(daemon.exit_pipe_rd, &rfds);
        int max_fd = daemon.exit_pipe_rd;
        if (daemon.dbus_pipe_rd >= 0) {
            FD_SET(daemon.dbus_pipe_rd, &rfds);
            if (daemon.dbus_pipe_rd > max_fd) max_fd = daemon.dbus_pipe_rd;
        }
        if (lm.event_pipe_rd >= 0) {
            FD_SET(lm.event_pipe_rd, &rfds);
            if (lm.event_pipe_rd > max_fd) max_fd = lm.event_pipe_rd;
        }

        LOG_D("Waiting on cloud license state machine events");
        int rc = select(max_fd + 1, &rfds, NULL, NULL, NULL);
        if (rc < 0) {
            if (errno == EINTR) continue;
            LOG_E("Wait failed (%s)!", strerror(errno));
            exit_code = 4;
            break;
        }
        if (FD_ISSET(daemon.exit_pipe_rd, &rfds) || gridd_daemon_stop_requested()) {
            break;
        }
        if (lm.event_pipe_rd >= 0 && FD_ISSET(lm.event_pipe_rd, &rfds)) {
            drain_pipe(lm.event_pipe_rd);
            if (gridd_lm_get_state(&lm) == GRIDD_LM_LICENSED) {
                uint32_t exp_ep = 0;
                uint32_t rm_state = gridd_lm_get_expiry_and_rm_state(&lm, &exp_ep);
                uint8_t target_rm_state = (exp_ep > 0U && rm_state == GRIDD_LICENSE_STATE_LICENSED)
                                          ? (uint8_t)GRIDD_LICENSE_STATE_TRIAL_OR_UNSPECIFIED
                                          : (uint8_t)rm_state;
                gridd_rm_set_license_status(&rm, mapped_feat, exp_ep, target_rm_state);
                const char *srv = gridd_lm_get_primary_url(&lm);
                LOG_N("License acquired successfully. (Info: %s, %s; Expiry epoch: %u)",
                      srv ? srv : "NLS", prod_name, exp_ep);
            }
        }
        if (daemon.dbus_pipe_rd >= 0 && FD_ISSET(daemon.dbus_pipe_rd, &rfds)) {
            drain_pipe(daemon.dbus_pipe_rd);
            uint32_t old_feat = cfg.feature_type;
            if (gridd_config_load(GRIDD_DEFAULT_CONF_PATH, rm.is_vgpu_guest, &cfg) == GRIDD_OK &&
                cfg.feature_type != old_feat) {
                active_feature = cfg.feature_type;
                mapped_feat = gridd_map_feature_type(cfg.feature_type);
                bool reboot_req = false;
                gridd_rm_enable_feature(&rm, mapped_feat, NULL, &reboot_req);
                if (!reboot_req &&
                    select_product_for_feature(&rm, mapped_feat, prod_name, sizeof(prod_name), &prod_count)) {
                    gridd_lm_acquire(&lm, prod_name, prod_count);
                }
            }
        }
    }

shutdown:
    if (lm_started) {
        gridd_lm_destroy(&lm);
    }
    if (dbus_started) {
        pthread_cancel(dbus_tid);
        pthread_join(dbus_tid, NULL);
    }
    gridd_rm_destroy(&rm);
    gridd_daemon_cleanup(&daemon, exit_code);
    gridd_http_global_cleanup();
    return exit_code;
}
