#ifndef GRIDD_TOKEN_H
#define GRIDD_TOKEN_H

#include "gridd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char node_urls[GRIDD_MAX_NODES][GRIDD_MAX_URL];
    uint32_t node_count;
    char url_qr[GRIDD_MAX_URL];
    uint32_t auth_port;
    uint32_t lease_port;
    bool is_cls;
    char service_instance_ref[GRIDD_MAX_STR];
    char jti[GRIDD_MAX_STR];
    char scope_ref_list[1024];
    char fulfillment_class_ref_list[1024];
    uint64_t nbf;
    uint64_t exp;
} gridd_token_config_t;

bool gridd_token_find_file(const char *path_or_dir, char *out_file, size_t out_sz);

bool gridd_token_parse_and_verify(const char *path_or_dir,
                                  const char *cached_jti,
                                  uint64_t now_epoch,
                                  gridd_token_config_t *out_cfg,
                                  char *err_out, size_t err_sz);

#ifdef __cplusplus
}
#endif

#endif /* GRIDD_TOKEN_H */
