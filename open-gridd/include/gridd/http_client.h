#ifndef GRIDD_HTTP_CLIENT_H
#define GRIDD_HTTP_CLIENT_H

#include "gridd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char proxy_host[GRIDD_MAX_PATH];
    char proxy_port[64];
    char proxy_user[GRIDD_MAX_STR];
    char proxy_password[GRIDD_MAX_STR];
    bool use_kerberos;
    long timeout_sec;
} gridd_http_opts_t;

typedef struct {
    long status_code;
    char *body;
    size_t body_len;
    gridd_status_t err;
    char err_msg[256];
} gridd_http_response_t;

void gridd_http_global_init(void);
void gridd_http_global_cleanup(void);
void gridd_http_response_free(gridd_http_response_t *resp);

bool gridd_http_load_proxy_opts(const char *conf_path, gridd_http_opts_t *opts);

gridd_status_t gridd_http_request(const char *method,
                                  const char *url,
                                  const char **headers,
                                  size_t header_count,
                                  const char *body,
                                  size_t body_len,
                                  const gridd_http_opts_t *opts,
                                  gridd_http_response_t *resp);

#ifdef __cplusplus
}
#endif

#endif /* GRIDD_HTTP_CLIENT_H */
