#ifndef GRIDD_CSP_PLATFORM_H
#define GRIDD_CSP_PLATFORM_H

#include "gridd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GRIDD_CSP_NONE = 0,
    GRIDD_CSP_AZURE = 1,
    GRIDD_CSP_AWS_EC2 = 2,
    GRIDD_CSP_GCP = 3,
    GRIDD_CSP_ALIBABA_ECS = 4,
    GRIDD_CSP_TENCENT = 5,
    GRIDD_CSP_TENCENT_IEG = 6
} gridd_csp_platform_id_t;

typedef struct {
    gridd_csp_platform_id_t platform_id;
    uint32_t csp_id;
    const char *name;
    bool requires_swcert;
} gridd_csp_info_t;

const gridd_csp_info_t *gridd_csp_find_by_platform_id(uint32_t platform_id);
bool gridd_csp_detect_and_verify(uint32_t platform_id,
                                 const char *swcert_path,
                                 const char *imds_base_url,
                                 uint32_t wait_to_connect_sec);

#ifdef __cplusplus
}
#endif

#endif /* GRIDD_CSP_PLATFORM_H */
