#define _GNU_SOURCE
#include "gridd/trusted_store.h"
#include "gridd/crypto.h"
#include "gridd/daemon.h"
#include "gridd/json_util.h"
#include "gridd/log.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t g_store_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *STORE_DATA_NAME = "store_nls.dat";
static const char *STORE_TAG_NAME  = "store_nls.tag";

void gridd_format_utc_iso8601(uint64_t epoch, char *out, size_t out_sz) {
    time_t t = (time_t)epoch;
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    strftime(out, out_sz, "%Y-%m-%dT%H:%M:%S", &tm_utc);
}

bool gridd_parse_utc_iso8601(const char *iso, uint64_t *out_epoch) {
    if (!iso || !*iso || !out_epoch) return false;
    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    int y = 0, m = 0, d = 0, H = 0, M = 0, S = 0;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &m, &d, &H, &M, &S) != 6 &&
        sscanf(iso, "%d-%d-%d %d:%d:%d", &y, &m, &d, &H, &M, &S) != 6) {
        return false;
    }
    tm_val.tm_year = y - 1900;
    tm_val.tm_mon = m - 1;
    tm_val.tm_mday = d;
    tm_val.tm_hour = H;
    tm_val.tm_min = M;
    tm_val.tm_sec = S;
    time_t t = timegm(&tm_val);
    if (t == (time_t)-1) return false;
    *out_epoch = (uint64_t)t;
    return true;
}

static bool derive_store_key(const char *machine_id, uint8_t key_out[32]) {
    const char *id = (machine_id && *machine_id) ? machine_id : "nvidia-gridd-default-machine";
    static const uint8_t salt[] = "nvidia-gridd-trusted-store-salt-v1";
    static const uint8_t info[] = "aes-256-gcm-local-store-key";
    return gridd_hkdf_sha256((const uint8_t *)id, strlen(id),
                             salt, sizeof(salt) - 1U,
                             info, sizeof(info) - 1U,
                             key_out, 32);
}

static bool serialize_store_json(const gridd_trusted_store_data_t *d, gridd_json_buf_t *jb) {
    gridd_json_buf_init(jb, 4096);
    gridd_json_buf_append(jb, "{\n");
    gridd_json_buf_appendf(jb, "  \"system_clock_snapshot\": ");
    gridd_json_buf_append_escaped_str(jb, d->system_clock_snapshot);
    gridd_json_buf_append(jb, ",\n  \"jti\": ");
    gridd_json_buf_append_escaped_str(jb, d->jti);

    if (d->origin_ref[0] != '\0') {
        gridd_json_buf_append(jb, ",\n  \"origin\": {\n    \"origin_ref\": ");
        gridd_json_buf_append_escaped_str(jb, d->origin_ref);
        gridd_json_buf_append(jb, ",\n    \"environment\": {\n      \"fingerprint\": { \"mac_address_list\": [");
        gridd_json_buf_append(jb, d->env.mac_address_list);
        gridd_json_buf_append(jb, "] },\n      \"ip_address_list\": [");
        gridd_json_buf_append(jb, d->env.ip_address_list);
        gridd_json_buf_append(jb, "],\n      \"hostname\": ");
        gridd_json_buf_append_escaped_str(jb, d->env.hostname);
        gridd_json_buf_append(jb, ",\n      \"guest_driver_version\": ");
        gridd_json_buf_append_escaped_str(jb, d->env.guest_driver_version);
        gridd_json_buf_append(jb, ",\n      \"os_platform\": ");
        gridd_json_buf_append_escaped_str(jb, d->env.os_platform);
        gridd_json_buf_append(jb, ",\n      \"os_version\": ");
        gridd_json_buf_append_escaped_str(jb, d->env.os_version);
        gridd_json_buf_append(jb, ",\n      \"host_driver_version\": ");
        gridd_json_buf_append_escaped_str(jb, d->env.host_driver_version);
        gridd_json_buf_append(jb, ",\n      \"gpu_id_list\": [");
        gridd_json_buf_append(jb, d->env.gpu_id_list);
        gridd_json_buf_append(jb, "],\n      \"client_platform_id\": ");
        gridd_json_buf_append_escaped_str(jb, d->env.client_platform_id);
        gridd_json_buf_append(jb, ",\n      \"hv_platform\": ");
        gridd_json_buf_append_escaped_str(jb, d->env.hv_platform);
        gridd_json_buf_appendf(jb, ",\n      \"cpu_sockets\": %u,\n      \"physical_cores\": %u\n    }",
                               d->env.cpu_sockets, d->env.physical_cores);
        gridd_json_buf_appendf(jb, ",\n    \"registered\": %s,\n    \"failed_registration_attempts\": %u\n  }",
                               d->registered ? "true" : "false",
                               d->failed_registration_attempts);
    }

    if (d->lease_list_json[0] != '\0') {
        gridd_json_buf_append(jb, ",\n  \"lease_management\": {\n    \"lease_list\": [");
        gridd_json_buf_append(jb, d->lease_list_json);
        gridd_json_buf_append(jb, "],\n    \"lease_state\": ");
        gridd_json_buf_append_escaped_str(jb, d->lease_state);
        gridd_json_buf_append(jb, "\n  }");
    }

    gridd_json_buf_append(jb, "\n}\n");
    return !jb->overflow;
}

static void deserialize_store_json(const char *json, gridd_trusted_store_data_t *d) {
    gridd_json_get_string(json, "system_clock_snapshot", d->system_clock_snapshot, sizeof(d->system_clock_snapshot));
    gridd_json_get_string(json, "jti", d->jti, sizeof(d->jti));
    gridd_json_get_string(json, "origin_ref", d->origin_ref, sizeof(d->origin_ref));
    gridd_json_get_bool(json, "registered", &d->registered);

    uint64_t attempts = 0;
    if (gridd_json_get_uint64(json, "failed_registration_attempts", &attempts)) {
        d->failed_registration_attempts = (uint32_t)attempts;
    }

    gridd_json_get_array_raw(json, "mac_address_list", d->env.mac_address_list, sizeof(d->env.mac_address_list));
    gridd_json_get_array_raw(json, "ip_address_list", d->env.ip_address_list, sizeof(d->env.ip_address_list));
    gridd_json_get_string(json, "hostname", d->env.hostname, sizeof(d->env.hostname));
    gridd_json_get_string(json, "guest_driver_version", d->env.guest_driver_version, sizeof(d->env.guest_driver_version));
    gridd_json_get_string(json, "os_platform", d->env.os_platform, sizeof(d->env.os_platform));
    gridd_json_get_string(json, "os_version", d->env.os_version, sizeof(d->env.os_version));
    gridd_json_get_string(json, "host_driver_version", d->env.host_driver_version, sizeof(d->env.host_driver_version));
    gridd_json_get_array_raw(json, "gpu_id_list", d->env.gpu_id_list, sizeof(d->env.gpu_id_list));
    gridd_json_get_string(json, "client_platform_id", d->env.client_platform_id, sizeof(d->env.client_platform_id));
    gridd_json_get_string(json, "hv_platform", d->env.hv_platform, sizeof(d->env.hv_platform));

    uint64_t sockets = 0, cores = 0;
    if (gridd_json_get_uint64(json, "cpu_sockets", &sockets)) d->env.cpu_sockets = (uint32_t)sockets;
    if (gridd_json_get_uint64(json, "physical_cores", &cores)) d->env.physical_cores = (uint32_t)cores;

    gridd_json_get_array_raw(json, "lease_list", d->lease_list_json, sizeof(d->lease_list_json));
    gridd_json_get_string(json, "lease_state", d->lease_state, sizeof(d->lease_state));
}

static bool write_atomic_bytes(const char *path, const uint8_t *data, size_t len) {
    char tmp[GRIDD_MAX_PATH + 16];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return false;
    size_t written = fwrite(data, 1, len, fp);
    int rc = fclose(fp);
    if (written != len || rc != 0) {
        unlink(tmp);
        return false;
    }
    return rename(tmp, path) == 0;
}

static bool load_encrypted_store(gridd_trusted_store_t *store) {
    FILE *fp_data = fopen(store->store_file, "rb");
    FILE *fp_tag = fopen(store->tag_file, "rb");
    if (!fp_data || !fp_tag) {
        if (fp_data) fclose(fp_data);
        if (fp_tag) fclose(fp_tag);
        return false;
    }

    uint8_t tag[GRIDD_GCM_TAG_LEN];
    if (fread(tag, 1, sizeof(tag), fp_tag) != sizeof(tag)) {
        fclose(fp_data);
        fclose(fp_tag);
        return false;
    }
    fclose(fp_tag);

    fseek(fp_data, 0, SEEK_END);
    long fsz = ftell(fp_data);
    fseek(fp_data, 0, SEEK_SET);
    if (fsz <= (long)GRIDD_GCM_IV_LEN || fsz > 1024L * 1024L) {
        fclose(fp_data);
        return false;
    }

    uint8_t *raw = (uint8_t *)malloc((size_t)fsz);
    if (!raw) {
        fclose(fp_data);
        return false;
    }
    if (fread(raw, 1, (size_t)fsz, fp_data) != (size_t)fsz) {
        free(raw);
        fclose(fp_data);
        return false;
    }
    fclose(fp_data);

    size_t ct_len = (size_t)fsz - GRIDD_GCM_IV_LEN;
    uint8_t *pt = (uint8_t *)calloc(1, ct_len + 1U);
    if (!pt) {
        free(raw);
        return false;
    }

    bool ok = gridd_aes256_gcm_decrypt(store->key, raw, raw + GRIDD_GCM_IV_LEN, ct_len, tag, pt);
    if (ok) {
        deserialize_store_json((const char *)pt, &store->data);
    }
    free(pt);
    free(raw);
    return ok;
}

gridd_status_t gridd_trusted_store_open(gridd_trusted_store_t *store,
                                        const char *store_dir,
                                        const char *machine_id,
                                        const char *jti) {
    if (!store) return GRIDD_ERR_INVALID_PARAM;
    memset(store, 0, sizeof(*store));

    const char *dir = (store_dir && *store_dir) ? store_dir : "/var/lib/nvidia/vGPULicensing";
    gridd_ensure_dir(dir, 0755);

    snprintf(store->store_dir, sizeof(store->store_dir), "%s", dir);
    snprintf(store->store_file, sizeof(store->store_file), "%s/%s", dir, STORE_DATA_NAME);
    snprintf(store->tag_file, sizeof(store->tag_file), "%s/%s", dir, STORE_TAG_NAME);

    if (!derive_store_key(machine_id, store->key)) {
        return GRIDD_ERR_GENERIC;
    }

    pthread_mutex_lock(&g_store_mutex);
    bool loaded = load_encrypted_store(store);
    pthread_mutex_unlock(&g_store_mutex);

    if (!loaded) {
        gridd_format_utc_iso8601((uint64_t)time(NULL),
                                 store->data.system_clock_snapshot,
                                 sizeof(store->data.system_clock_snapshot));
        if (jti && *jti) {
            snprintf(store->data.jti, sizeof(store->data.jti), "%s", jti);
        }
        return gridd_trusted_store_save(store);
    }

    if (jti && *jti && strcmp(store->data.jti, jti) != 0) {
        snprintf(store->data.jti, sizeof(store->data.jti), "%s", jti);
        gridd_trusted_store_save(store);
    }

    return gridd_trusted_store_check_clock(store, (uint64_t)time(NULL));
}

gridd_status_t gridd_trusted_store_save(gridd_trusted_store_t *store) {
    if (!store) return GRIDD_ERR_INVALID_PARAM;
    pthread_mutex_lock(&g_store_mutex);

    gridd_json_buf_t jb;
    if (!serialize_store_json(&store->data, &jb)) {
        gridd_json_buf_free(&jb);
        pthread_mutex_unlock(&g_store_mutex);
        LOG_E("Failed to update local trusted store - Maximum buffer size exceeded");
        return GRIDD_ERR_INVALID_PARAM;
    }

    uint8_t iv[GRIDD_GCM_IV_LEN];
    uint8_t tag[GRIDD_GCM_TAG_LEN];
    uint8_t *blob = (uint8_t *)malloc(GRIDD_GCM_IV_LEN + jb.len + 16U);
    if (!blob || !gridd_random_bytes(iv, sizeof(iv))) {
        free(blob);
        gridd_json_buf_free(&jb);
        pthread_mutex_unlock(&g_store_mutex);
        return GRIDD_ERR_GENERIC;
    }
    memcpy(blob, iv, GRIDD_GCM_IV_LEN);

    bool ok = gridd_aes256_gcm_encrypt(store->key, iv,
                                       (const uint8_t *)jb.data, jb.len,
                                       blob + GRIDD_GCM_IV_LEN, tag);
    size_t total_len = GRIDD_GCM_IV_LEN + jb.len;
    gridd_json_buf_free(&jb);

    if (ok) {
        if (!write_atomic_bytes(store->store_file, blob, total_len)) {
            LOG_E("Failed to update local trusted store %s (%s)", store->store_file, strerror(errno));
            ok = false;
        } else if (!write_atomic_bytes(store->tag_file, tag, sizeof(tag))) {
            LOG_E("Failed to update auth tag file %s (%s)", store->tag_file, strerror(errno));
            ok = false;
        }
    }

    free(blob);
    pthread_mutex_unlock(&g_store_mutex);
    return ok ? GRIDD_OK : GRIDD_ERR_IO;
}

gridd_status_t gridd_trusted_store_check_clock(gridd_trusted_store_t *store, uint64_t now_epoch) {
    if (!store) return GRIDD_ERR_INVALID_PARAM;
    if (store->data.system_clock_snapshot[0] == '\0') return GRIDD_OK;

    uint64_t snap_epoch = 0;
    if (!gridd_parse_utc_iso8601(store->data.system_clock_snapshot, &snap_epoch)) {
        return GRIDD_ERR_CLOCK_WINDBACK;
    }
    if (now_epoch < snap_epoch) {
        LOG_E("Clock windback has been detected");
        return GRIDD_ERR_CLOCK_WINDBACK;
    }
    return GRIDD_OK;
}

bool gridd_trusted_store_match_fingerprint(const gridd_trusted_store_t *store,
                                           const gridd_environment_t *current_env) {
    if (!store || !current_env) return false;
    if (store->data.env.mac_address_list[0] == '\0') return true;
    return strcmp(store->data.env.mac_address_list, current_env->mac_address_list) == 0;
}
