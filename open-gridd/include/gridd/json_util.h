#ifndef GRIDD_JSON_UTIL_H
#define GRIDD_JSON_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool overflow;
} gridd_json_buf_t;

void gridd_json_buf_init(gridd_json_buf_t *buf, size_t initial_cap);
void gridd_json_buf_free(gridd_json_buf_t *buf);
bool gridd_json_buf_append(gridd_json_buf_t *buf, const char *str);
bool gridd_json_buf_appendf(gridd_json_buf_t *buf, const char *fmt, ...);
bool gridd_json_buf_append_escaped_str(gridd_json_buf_t *buf, const char *str);

bool gridd_json_get_string(const char *json, const char *key, char *out, size_t out_sz);
bool gridd_json_get_raw_value(const char *json, const char *key, char *out, size_t out_sz);
bool gridd_json_get_uint64(const char *json, const char *key, uint64_t *out);
bool gridd_json_get_int64(const char *json, const char *key, int64_t *out);
bool gridd_json_get_double(const char *json, const char *key, double *out);
bool gridd_json_get_bool(const char *json, const char *key, bool *out);
bool gridd_json_get_array_raw(const char *json, const char *key, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* GRIDD_JSON_UTIL_H */
