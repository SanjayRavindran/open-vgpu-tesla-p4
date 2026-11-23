#ifndef GRIDD_ENV_FINGERPRINT_H
#define GRIDD_ENV_FINGERPRINT_H

#include "gridd/rm_client.h"
#include "gridd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char mac_address_list[2048];
    char ip_address_list[2048];
    char hostname[256];
    char guest_driver_version[64];
    char os_platform[128];
    char os_version[128];
    char host_driver_version[64];
    char gpu_id_list[2048];
    char client_platform_id[64];
    char hv_platform[64];
    uint32_t cpu_sockets;
    uint32_t physical_cores;
} gridd_environment_t;

gridd_status_t gridd_env_collect(gridd_rm_client_t *rm, gridd_environment_t *env);
bool gridd_env_read_machine_uuid(char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* GRIDD_ENV_FINGERPRINT_H */
