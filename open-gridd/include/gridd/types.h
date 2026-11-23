#ifndef GRIDD_TYPES_H
#define GRIDD_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRIDD_MAX_PATH 1024
#define GRIDD_MAX_STR 256
#define GRIDD_MAX_URL 128
#define GRIDD_MAX_NODES 20
#define GRIDD_MAX_GPUS 32
#define GRIDD_MAX_FEATURES 8
#define GRIDD_MAX_TCP_PAYLOAD 16384

typedef enum {
    GRIDD_OK = 0,
    GRIDD_ERR_GENERIC = 1,
    GRIDD_ERR_INVALID_PARAM = 2,
    GRIDD_ERR_AUTH = 3,
    GRIDD_ERR_IO = 4,
    GRIDD_ERR_UNSUPPORTED = 5,
    GRIDD_ERR_RM = 6,
    GRIDD_ERR_PIPE = 7,
    GRIDD_ERR_NO_LEASE = 8,
    GRIDD_ERR_OFFLINE_LEASE = 9,
    GRIDD_ERR_CLOCK_WINDBACK = 11,
    GRIDD_ERR_EXPIRED = 14,
    GRIDD_ERR_REDIRECT = 20,
    GRIDD_ERR_MISMATCH = 22,
    GRIDD_ERR_ENV = 23,
    GRIDD_ERR_FILE_READ = 24,
    GRIDD_ERR_LEASE_EXPIRED = 25,
    GRIDD_ERR_NO_CACHE = 26
} gridd_status_t;

typedef enum {
    GRIDD_FEATURE_UNLICENSED = 0,
    GRIDD_FEATURE_VGPU = 1,
    GRIDD_FEATURE_VWS = 2,
    GRIDD_FEATURE_VGAMING = 3,
    GRIDD_FEATURE_VCS = 4
} gridd_feature_type_t;

typedef enum {
    GRIDD_LM_UNINITIALIZED = 0,
    GRIDD_LM_INITIALIZED = 1,
    GRIDD_LM_ACQUIRING = 2,
    GRIDD_LM_LICENSED = 3,
    GRIDD_LM_RENEWING = 4,
    GRIDD_LM_UNLICENSED = 5,
    GRIDD_LM_RETURNING = 6,
    GRIDD_LM_STOPPED = 7
} gridd_lm_state_t;

typedef enum {
    GRIDD_LICENSE_STATE_UNLICENSED = 1,
    GRIDD_LICENSE_STATE_TRIAL_OR_UNSPECIFIED = 2,
    GRIDD_LICENSE_STATE_LICENSED = 3,
    GRIDD_LICENSE_STATE_PERPETUAL = 4
} gridd_rm_license_state_t;

#ifdef __cplusplus
}
#endif

#endif /* GRIDD_TYPES_H */
