#define _GNU_SOURCE
#include "gridd/json_util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void gridd_json_buf_init(gridd_json_buf_t *buf, size_t initial_cap) {
    if (!buf) return;
    buf->len = 0;
    buf->cap = initial_cap > 0 ? initial_cap : 256;
    buf->overflow = false;
    buf->data = (char *)calloc(1, buf->cap);
    if (!buf->data) {
        buf->cap = 0;
        buf->overflow = true;
    }
}

void gridd_json_buf_free(gridd_json_buf_t *buf) {
    if (!buf) return;
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    buf->overflow = false;
}

static bool ensure_cap(gridd_json_buf_t *buf, size_t extra) {
    if (!buf || buf->overflow) return false;
    if (buf->len + extra + 1 <= buf->cap) return true;
    size_t new_cap = buf->cap > 0 ? buf->cap : 256;
    while (new_cap < buf->len + extra + 1) {
        if (new_cap > (size_t)1 << 24) {
            buf->overflow = true;
            return false;
        }
        new_cap *= 2;
    }
    char *p = (char *)realloc(buf->data, new_cap);
    if (!p) {
        buf->overflow = true;
        return false;
    }
    buf->data = p;
    buf->cap = new_cap;
    return true;
}

bool gridd_json_buf_append(gridd_json_buf_t *buf, const char *str) {
    if (!str) return true;
    size_t slen = strlen(str);
    if (!ensure_cap(buf, slen)) return false;
    memcpy(buf->data + buf->len, str, slen);
    buf->len += slen;
    buf->data[buf->len] = '\0';
    return true;
}

bool gridd_json_buf_appendf(gridd_json_buf_t *buf, const char *fmt, ...) {
    if (!buf || buf->overflow || !fmt) return false;
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) {
        va_end(ap2);
        buf->overflow = true;
        return false;
    }
    if (!ensure_cap(buf, (size_t)needed)) {
        va_end(ap2);
        return false;
    }
    vsnprintf(buf->data + buf->len, (size_t)needed + 1, fmt, ap2);
    va_end(ap2);
    buf->len += (size_t)needed;
    return true;
}

bool gridd_json_buf_append_escaped_str(gridd_json_buf_t *buf, const char *str) {
    if (!gridd_json_buf_append(buf, "\"")) return false;
    if (str) {
        for (const char *p = str; *p; ++p) {
            unsigned char c = (unsigned char)*p;
            switch (c) {
            case '"':  if (!gridd_json_buf_append(buf, "\\\"")) return false; break;
            case '\\': if (!gridd_json_buf_append(buf, "\\\\")) return false; break;
            case '\n': if (!gridd_json_buf_append(buf, "\\n")) return false; break;
            case '\r': if (!gridd_json_buf_append(buf, "\\r")) return false; break;
            case '\t': if (!gridd_json_buf_append(buf, "\\t")) return false; break;
            default:
                if (c < 0x20) {
                    if (!gridd_json_buf_appendf(buf, "\\u%04x", (unsigned int)c)) return false;
                } else {
                    char ch[2] = {(char)c, '\0'};
                    if (!gridd_json_buf_append(buf, ch)) return false;
                }
                break;
            }
        }
    }
    return gridd_json_buf_append(buf, "\"");
}

static const char *skip_ws(const char *p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
        ++p;
    }
    return p;
}

static const char *find_key_value(const char *json, const char *key) {
    if (!json || !key) return NULL;
    size_t key_len = strlen(key);
    const char *p = json;
    bool in_string = false;
    while (*p) {
        if (*p == '"' && (p == json || p[-1] != '\\')) {
            if (!in_string) {
                const char *start = p + 1;
                const char *end = start;
                while (*end && !(*end == '"' && end[-1] != '\\')) {
                    if (*end == '\\' && end[1]) end += 2;
                    else ++end;
                }
                if (*end == '"') {
                    size_t tok_len = (size_t)(end - start);
                    const char *after = skip_ws(end + 1);
                    if (tok_len == key_len && memcmp(start, key, key_len) == 0 && *after == ':') {
                        return skip_ws(after + 1);
                    }
                    p = end + 1;
                    continue;
                }
            }
            in_string = !in_string;
        } else if (in_string && *p == '\\' && p[1]) {
            p += 2;
            continue;
        }
        ++p;
    }
    return NULL;
}

static bool unescape_json_string(const char *start, const char *end, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return false;
    size_t w = 0;
    for (const char *p = start; p < end; ++p) {
        char ch = *p;
        if (ch == '\\' && p + 1 < end) {
            ++p;
            switch (*p) {
            case '"':  ch = '"'; break;
            case '\\': ch = '\\'; break;
            case '/':  ch = '/'; break;
            case 'n':  ch = '\n'; break;
            case 'r':  ch = '\r'; break;
            case 't':  ch = '\t'; break;
            case 'b':  ch = '\b'; break;
            case 'f':  ch = '\f'; break;
            default:   ch = *p; break;
            }
        }
        if (w + 1 >= out_sz) {
            out[w] = '\0';
            return false;
        }
        out[w++] = ch;
    }
    out[w] = '\0';
    return true;
}

bool gridd_json_get_string(const char *json, const char *key, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return false;
    out[0] = '\0';
    const char *v = find_key_value(json, key);
    if (!v || *v != '"') return false;
    const char *start = v + 1;
    const char *end = start;
    while (*end) {
        if (*end == '\\' && end[1]) {
            end += 2;
            continue;
        }
        if (*end == '"') break;
        ++end;
    }
    if (*end != '"') return false;
    return unescape_json_string(start, end, out, out_sz);
}

static const char *scan_composite_end(const char *v, char open_ch, char close_ch) {
    int depth = 0;
    bool in_str = false;
    for (const char *p = v; *p; ++p) {
        if (*p == '\\' && in_str && p[1]) {
            ++p;
            continue;
        }
        if (*p == '"') {
            in_str = !in_str;
            continue;
        }
        if (!in_str) {
            if (*p == open_ch) depth++;
            else if (*p == close_ch) {
                depth--;
                if (depth == 0) return p + 1;
            }
        }
    }
    return NULL;
}

bool gridd_json_get_raw_value(const char *json, const char *key, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return false;
    out[0] = '\0';
    const char *v = find_key_value(json, key);
    if (!v) return false;
    const char *end = v;
    if (*v == '"') {
        return gridd_json_get_string(json, key, out, out_sz);
    } else if (*v == '{') {
        end = scan_composite_end(v, '{', '}');
    } else if (*v == '[') {
        end = scan_composite_end(v, '[', ']');
    } else {
        while (*end && *end != ',' && *end != '}' && *end != ']' && !isspace((unsigned char)*end)) {
            ++end;
        }
    }
    if (!end || end <= v) return false;
    size_t len = (size_t)(end - v);
    if (len >= out_sz) return false;
    memcpy(out, v, len);
    out[len] = '\0';
    return true;
}

bool gridd_json_get_array_raw(const char *json, const char *key, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return false;
    out[0] = '\0';
    const char *v = find_key_value(json, key);
    if (!v || *v != '[') return false;
    const char *end = scan_composite_end(v, '[', ']');
    if (!end || end < v + 2) return false;
    const char *inner = v + 1;
    size_t len = (size_t)((end - 1) - inner);
    if (len >= out_sz) return false;
    memcpy(out, inner, len);
    out[len] = '\0';
    return true;
}

bool gridd_json_get_uint64(const char *json, const char *key, uint64_t *out) {
    char buf[64];
    if (!gridd_json_get_raw_value(json, key, buf, sizeof(buf))) return false;
    if (!isdigit((unsigned char)buf[0])) return false;
    char *endp = NULL;
    unsigned long long val = strtoull(buf, &endp, 10);
    if (endp == buf) return false;
    if (out) *out = (uint64_t)val;
    return true;
}

bool gridd_json_get_int64(const char *json, const char *key, int64_t *out) {
    char buf[64];
    if (!gridd_json_get_raw_value(json, key, buf, sizeof(buf))) return false;
    char *endp = NULL;
    long long val = strtoll(buf, &endp, 10);
    if (endp == buf) return false;
    if (out) *out = (int64_t)val;
    return true;
}

bool gridd_json_get_double(const char *json, const char *key, double *out) {
    char buf[64];
    if (!gridd_json_get_raw_value(json, key, buf, sizeof(buf))) return false;
    char *endp = NULL;
    double val = strtod(buf, &endp);
    if (endp == buf) return false;
    if (out) *out = val;
    return true;
}

bool gridd_json_get_bool(const char *json, const char *key, bool *out) {
    char buf[16];
    if (!gridd_json_get_raw_value(json, key, buf, sizeof(buf))) return false;
    if (strcmp(buf, "true") == 0) {
        if (out) *out = true;
        return true;
    }
    if (strcmp(buf, "false") == 0) {
        if (out) *out = false;
        return true;
    }
    return false;
}
