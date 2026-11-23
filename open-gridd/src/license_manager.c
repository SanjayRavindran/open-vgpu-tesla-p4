#define _GNU_SOURCE
#include "gridd/license_manager.h"
#include "gridd/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

static void write_pipe_sig(int fd) {
    if (fd < 0) return;
    uint32_t v = 1;
    if (write(fd, &v, sizeof(v)) != (ssize_t)sizeof(v)) {
        LOG_E("Event Write failed : %s", strerror(errno));
    }
}

static void update_intervals_from_lease(gridd_license_manager_t *lm) {
    double dur = lm->nls.lease.lease_duration_sec;
    double rec = lm->nls.lease.recommended_renewal;
    if (dur < 10.0) dur = 60.0;
    if (rec <= 0.0 || rec >= 1.0) rec = 0.5;
    uint32_t renew_ms = (uint32_t)(dur * rec * 1000.0);
    if (renew_ms < 5000U) renew_ms = 5000U;
    lm->renew_interval_ms = renew_ms;
}

static void set_state_and_notify(gridd_license_manager_t *lm, gridd_lm_state_t next_state) {
    pthread_mutex_lock(&lm->lock);
    gridd_lm_state_t prev = lm->state;
    lm->state = next_state;
    pthread_mutex_unlock(&lm->lock);

    if (prev != next_state && next_state == GRIDD_LM_LICENSED) {
        write_pipe_sig(lm->event_pipe_wr);
    }
}

static void *lm_worker_main(void *raw) {
    gridd_license_manager_t *lm = (gridd_license_manager_t *)raw;

    while (!lm->stop_requested) {
        pthread_mutex_lock(&lm->lock);
        gridd_lm_state_t target = lm->pending_state;
        uint32_t wait_ms = (lm->state == GRIDD_LM_LICENSED) ? lm->renew_interval_ms : lm->retry_interval_ms;
        pthread_mutex_unlock(&lm->lock);

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(lm->wake_pipe_rd, &rfds);
        struct timeval tv;
        tv.tv_sec = (long)(wait_ms / 1000U);
        tv.tv_usec = (long)((wait_ms % 1000U) * 1000U);
        int rc = select(lm->wake_pipe_rd + 1, &rfds, NULL, NULL,
                        (target == GRIDD_LM_INITIALIZED) ? NULL : &tv);
        if (rc > 0) {
            uint32_t dummy = 0;
            ssize_t unused = read(lm->wake_pipe_rd, &dummy, sizeof(dummy));
            (void)unused;
        }
        if (lm->stop_requested) break;

        pthread_mutex_lock(&lm->lock);
        target = lm->pending_state;
        char prod[GRIDD_MAX_STR];
        snprintf(prod, sizeof(prod), "%s", lm->product_name);
        uint32_t cnt = lm->product_count;
        pthread_mutex_unlock(&lm->lock);

        if (target == GRIDD_LM_ACQUIRING) {
            if (prod[0] == '\0') {
                LOG_W("Product name missing");
                continue;
            }
            if (gridd_nls_acquire_lease(&lm->nls, prod, cnt) == GRIDD_OK) {
                update_intervals_from_lease(lm);
                lm->retry_interval_ms = 10000U;
                pthread_mutex_lock(&lm->lock);
                lm->pending_state = GRIDD_LM_RENEWING;
                pthread_mutex_unlock(&lm->lock);
                set_state_and_notify(lm, GRIDD_LM_LICENSED);
            } else {
                set_state_and_notify(lm, GRIDD_LM_UNLICENSED);
                if (lm->retry_interval_ms < 300000U) lm->retry_interval_ms *= 2U;
            }
        } else if (target == GRIDD_LM_RENEWING) {
            if (gridd_nls_renew_lease(&lm->nls, prod, cnt) == GRIDD_OK) {
                update_intervals_from_lease(lm);
                set_state_and_notify(lm, GRIDD_LM_LICENSED);
            } else {
                LOG_W("Failed to renew license");
                pthread_mutex_lock(&lm->lock);
                lm->pending_state = GRIDD_LM_ACQUIRING;
                pthread_mutex_unlock(&lm->lock);
                set_state_and_notify(lm, GRIDD_LM_UNLICENSED);
            }
        } else if (target == GRIDD_LM_RETURNING) {
            gridd_nls_return_leases(&lm->nls);
            set_state_and_notify(lm, GRIDD_LM_STOPPED);
            break;
        }
    }
    return NULL;
}

gridd_status_t gridd_lm_create(gridd_license_manager_t *lm,
                               const gridd_token_config_t *token,
                               const gridd_environment_t *env,
                               const gridd_http_opts_t *http_opts,
                               const char *store_dir,
                               const char *machine_id,
                               uint32_t license_interval_min) {
    if (!lm) return GRIDD_ERR_INVALID_PARAM;
    memset(lm, 0, sizeof(*lm));
    lm->wake_pipe_rd = -1;
    lm->wake_pipe_wr = -1;
    lm->event_pipe_rd = -1;
    lm->event_pipe_wr = -1;
    lm->state = GRIDD_LM_INITIALIZED;
    lm->pending_state = GRIDD_LM_INITIALIZED;
    lm->retry_interval_ms = 10000U;
    lm->renew_interval_ms = 60000U;

    if (pthread_mutex_init(&lm->lock, NULL) != 0) {
        LOG_E("Mutex init failed : %s", strerror(errno));
        return GRIDD_ERR_GENERIC;
    }

    int wake[2] = {-1, -1}, ev[2] = {-1, -1};
    if (pipe(wake) != 0 || pipe(ev) != 0) {
        LOG_E("Event Init failed : %s", strerror(errno));
        if (wake[0] >= 0) { close(wake[0]); close(wake[1]); }
        if (ev[0] >= 0) { close(ev[0]); close(ev[1]); }
        return GRIDD_ERR_IO;
    }
    lm->wake_pipe_rd = wake[0];
    lm->wake_pipe_wr = wake[1];
    lm->event_pipe_rd = ev[0];
    lm->event_pipe_wr = ev[1];

    gridd_status_t st = gridd_nls_init(&lm->nls, token, env, http_opts,
                                       store_dir, machine_id, license_interval_min);
    if (st != GRIDD_OK) {
        gridd_lm_destroy(lm);
        return st;
    }

    int rc = pthread_create(&lm->thread, NULL, lm_worker_main, lm);
    if (rc != 0) {
        LOG_E("Create thread failed : %s", strerror(rc));
        gridd_lm_destroy(lm);
        return GRIDD_ERR_GENERIC;
    }
    lm->thread_started = true;
    return GRIDD_OK;
}

void gridd_lm_acquire(gridd_license_manager_t *lm, const char *product_name, uint32_t count) {
    if (!lm || !product_name) return;
    pthread_mutex_lock(&lm->lock);
    snprintf(lm->product_name, sizeof(lm->product_name), "%s", product_name);
    lm->product_count = count > 0 ? count : 1U;
    lm->pending_state = GRIDD_LM_ACQUIRING;
    pthread_mutex_unlock(&lm->lock);
    write_pipe_sig(lm->wake_pipe_wr);
}

void gridd_lm_stop(gridd_license_manager_t *lm) {
    if (!lm) return;
    pthread_mutex_lock(&lm->lock);
    lm->stop_requested = true;
    lm->pending_state = GRIDD_LM_RETURNING;
    pthread_mutex_unlock(&lm->lock);
    write_pipe_sig(lm->wake_pipe_wr);
    if (lm->thread_started) {
        pthread_join(lm->thread, NULL);
        lm->thread_started = false;
    }
    gridd_nls_return_leases(&lm->nls);
}

void gridd_lm_destroy(gridd_license_manager_t *lm) {
    if (!lm) return;
    if (lm->thread_started) {
        gridd_lm_stop(lm);
    }
    if (lm->wake_pipe_rd >= 0) close(lm->wake_pipe_rd);
    if (lm->wake_pipe_wr >= 0) close(lm->wake_pipe_wr);
    if (lm->event_pipe_rd >= 0) close(lm->event_pipe_rd);
    if (lm->event_pipe_wr >= 0) close(lm->event_pipe_wr);
    pthread_mutex_destroy(&lm->lock);
}

gridd_lm_state_t gridd_lm_get_state(gridd_license_manager_t *lm) {
    if (!lm) return GRIDD_LM_UNINITIALIZED;
    pthread_mutex_lock(&lm->lock);
    gridd_lm_state_t st = lm->state;
    pthread_mutex_unlock(&lm->lock);
    return st;
}

uint32_t gridd_lm_get_expiry_and_rm_state(gridd_license_manager_t *lm, uint32_t *out_expiry_epoch) {
    if (!lm) return GRIDD_LICENSE_STATE_UNLICENSED;
    pthread_mutex_lock(&lm->lock);
    uint64_t exp = lm->nls.lease.expires_epoch;
    bool active = lm->nls.has_active_lease;
    pthread_mutex_unlock(&lm->lock);
    if (out_expiry_epoch) *out_expiry_epoch = (uint32_t)exp;
    return active ? GRIDD_LICENSE_STATE_LICENSED : GRIDD_LICENSE_STATE_UNLICENSED;
}

uint32_t gridd_lm_get_lease_port(gridd_license_manager_t *lm) {
    return lm ? lm->nls.token.lease_port : 0U;
}

const char *gridd_lm_get_primary_url(gridd_license_manager_t *lm) {
    if (!lm || lm->nls.token.node_count == 0) return NULL;
    return lm->nls.token.node_urls[0];
}

const char *gridd_lm_get_secondary_url(gridd_license_manager_t *lm) {
    if (!lm || lm->nls.token.node_count < 2) return NULL;
    return lm->nls.token.node_urls[1];
}
