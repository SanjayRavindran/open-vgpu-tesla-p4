#define _GNU_SOURCE
#include "gridd/http_client.h"
#include "gridd/config.h"
#include "gridd/crypto.h"
#include "gridd/log.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    bool is_https;
    char host[256];
    char port[16];
    char path[2048];
} parsed_url_t;

typedef struct {
    int fd;
    SSL *ssl;
    SSL_CTX *ctx;
} transport_t;

static bool parse_url(const char *url, parsed_url_t *out) {
    if (!url || !out) return false;
    memset(out, 0, sizeof(*out));

    const char *p = NULL;
    if (strncmp(url, "https://", 8) == 0) {
        out->is_https = true;
        p = url + 8;
        snprintf(out->port, sizeof(out->port), "443");
    } else if (strncmp(url, "http://", 7) == 0) {
        out->is_https = false;
        p = url + 7;
        snprintf(out->port, sizeof(out->port), "80");
    } else {
        return false;
    }

    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : (p + strlen(p));
    if (slash) {
        snprintf(out->path, sizeof(out->path), "%s", slash);
    } else {
        snprintf(out->path, sizeof(out->path), "/");
    }

    if (p[0] == '[') {
        const char *rb = strchr(p, ']');
        if (!rb || rb >= host_end) return false;
        size_t hlen = (size_t)(rb - (p + 1));
        if (hlen == 0 || hlen >= sizeof(out->host)) return false;
        memcpy(out->host, p + 1, hlen);
        out->host[hlen] = '\0';
        if (rb + 1 < host_end && rb[1] == ':') {
            size_t plen = (size_t)(host_end - (rb + 2));
            if (plen == 0 || plen >= sizeof(out->port)) return false;
            memcpy(out->port, rb + 2, plen);
            out->port[plen] = '\0';
        }
    } else {
        const char *colon = NULL;
        for (const char *c = p; c < host_end; ++c) {
            if (*c == ':') {
                colon = c;
                break;
            }
        }
        size_t hlen = colon ? (size_t)(colon - p) : (size_t)(host_end - p);
        if (hlen == 0 || hlen >= sizeof(out->host)) return false;
        memcpy(out->host, p, hlen);
        out->host[hlen] = '\0';
        if (colon) {
            size_t plen = (size_t)(host_end - (colon + 1));
            if (plen == 0 || plen >= sizeof(out->port)) return false;
            memcpy(out->port, colon + 1, plen);
            out->port[plen] = '\0';
        }
    }
    return true;
}

static int connect_tcp(const char *host, const char *port, long timeout_sec,
                       char *err_msg, size_t err_sz) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0) {
        snprintf(err_msg, err_sz, "Could not resolve server address");
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        struct timeval tv;
        tv.tv_sec = timeout_sec > 0 ? timeout_sec : 30L;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        if (errno == ETIMEDOUT || errno == EAGAIN) {
            snprintf(err_msg, err_sz, "Could not communicate to server, timeout has been reached");
        } else {
            snprintf(err_msg, err_sz, "Failed to connect to server (%s)", strerror(errno));
        }
    }
    return fd;
}

static bool raw_send_all(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static bool t_send_all(transport_t *t, const char *buf, size_t len) {
    if (!t->ssl) return raw_send_all(t->fd, buf, len);
    size_t sent = 0;
    while (sent < len) {
        int n = SSL_write(t->ssl, buf + sent, (int)(len - sent));
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static ssize_t t_recv(transport_t *t, char *buf, size_t cap) {
    if (!t->ssl) {
        ssize_t n;
        do {
            n = recv(t->fd, buf, cap, 0);
        } while (n < 0 && errno == EINTR);
        return n;
    }
    int n = SSL_read(t->ssl, buf, (int)cap);
    if (n <= 0) return n == 0 ? 0 : -1;
    return (ssize_t)n;
}

static bool proxy_connect_tunnel(int fd, const parsed_url_t *u, const gridd_http_opts_t *opts,
                                 char *err_msg, size_t err_sz) {
    char req[2048];
    char auth_hdr[1024] = {0};
    if (opts->proxy_user[0] != '\0') {
        char creds[600];
        char b64[900];
        snprintf(creds, sizeof(creds), "%s:%s", opts->proxy_user, opts->proxy_password);
        if (gridd_base64_encode((const uint8_t *)creds, strlen(creds), b64, sizeof(b64), false)) {
            snprintf(auth_hdr, sizeof(auth_hdr), "Proxy-Authorization: Basic %s\r\n", b64);
        }
    }

    int rlen = snprintf(req, sizeof(req), "CONNECT %s:%s HTTP/1.1\r\nHost: %s:%s\r\n%s\r\n",
                        u->host, u->port, u->host, u->port, auth_hdr);
    if (rlen <= 0 || (size_t)rlen >= sizeof(req) || !raw_send_all(fd, req, (size_t)rlen)) {
        snprintf(err_msg, err_sz, "Failed to send HTTP CONNECT request");
        return false;
    }

    char resp_buf[4096];
    size_t total = 0;
    while (total + 1U < sizeof(resp_buf)) {
        ssize_t n = recv(fd, resp_buf + total, sizeof(resp_buf) - 1U - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        resp_buf[total] = '\0';
        if (strstr(resp_buf, "\r\n\r\n") != NULL) break;
    }

    int code = 0;
    if (sscanf(resp_buf, "HTTP/%*d.%*d %d", &code) != 1 || code != 200) {
        if (code == 407) {
            snprintf(err_msg, err_sz, "Could not authenticate proxy server");
        } else {
            snprintf(err_msg, err_sz, "Proxy CONNECT failed with HTTP %d", code);
        }
        return false;
    }
    return true;
}

static bool decode_chunked_body(const char *src, size_t src_len, char **out, size_t *out_len) {
    char *dst = (char *)calloc(1, src_len + 1U);
    if (!dst) return false;
    size_t pos = 0;
    size_t w = 0;

    while (pos < src_len) {
        const char *crlf = NULL;
        for (size_t i = pos; i + 1 < src_len; ++i) {
            if (src[i] == '\r' && src[i + 1] == '\n') {
                crlf = src + i;
                break;
            }
        }
        if (!crlf) break;

        char hex_buf[32];
        size_t hlen = (size_t)(crlf - (src + pos));
        if (hlen == 0 || hlen >= sizeof(hex_buf)) break;
        memcpy(hex_buf, src + pos, hlen);
        hex_buf[hlen] = '\0';
        char *semi = strchr(hex_buf, ';');
        if (semi) *semi = '\0';

        unsigned long chunk_sz = strtoul(hex_buf, NULL, 16);
        pos = (size_t)(crlf - src) + 2U;
        if (chunk_sz == 0) break;
        if (pos + chunk_sz > src_len) chunk_sz = src_len - pos;

        memcpy(dst + w, src + pos, chunk_sz);
        w += chunk_sz;
        pos += chunk_sz;
        if (pos + 2U <= src_len && src[pos] == '\r' && src[pos + 1] == '\n') {
            pos += 2U;
        }
    }
    dst[w] = '\0';
    *out = dst;
    *out_len = w;
    return true;
}

void gridd_http_global_init(void) {
    OPENSSL_init_ssl(0, NULL);
}

void gridd_http_global_cleanup(void) {
}

void gridd_http_response_free(gridd_http_response_t *resp) {
    if (!resp) return;
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
}

bool gridd_http_load_proxy_opts(const char *conf_path, gridd_http_opts_t *opts) {
    if (!opts) return false;
    memset(opts, 0, sizeof(*opts));
    opts->timeout_sec = 30;

    gridd_config_t cfg;
    if (gridd_config_load(conf_path, false, &cfg) != GRIDD_OK &&
        cfg.proxy_server_address[0] == '\0') {
        return false;
    }

    snprintf(opts->proxy_host, sizeof(opts->proxy_host), "%s", cfg.proxy_server_address);
    snprintf(opts->proxy_port, sizeof(opts->proxy_port), "%s", cfg.proxy_server_port);
    snprintf(opts->proxy_user, sizeof(opts->proxy_user), "%s", cfg.proxy_user_name);

    if (cfg.proxy_user_name[0] != '\0' && cfg.proxy_credentials_file_path[0] != '\0') {
        FILE *fp = fopen(cfg.proxy_credentials_file_path, "rb");
        if (fp) {
            char b64[1024] = {0};
            size_t n = fread(b64, 1, sizeof(b64) - 1, fp);
            fclose(fp);
            while (n > 0 && (b64[n - 1] == '\n' || b64[n - 1] == '\r' || b64[n - 1] == ' ')) {
                b64[--n] = '\0';
            }
            uint8_t dec[1024] = {0};
            size_t dec_len = 0;
            if (gridd_base64_decode(b64, dec, sizeof(dec) - 1, &dec_len)) {
                dec[dec_len] = '\0';
                snprintf(opts->proxy_password, sizeof(opts->proxy_password), "%s", (const char *)dec);
            }
        }
    } else if (cfg.proxy_server_address[0] != '\0') {
        opts->use_kerberos = true;
    }
    return true;
}

gridd_status_t gridd_http_request(const char *method,
                                  const char *url,
                                  const char **headers,
                                  size_t header_count,
                                  const char *body,
                                  size_t body_len,
                                  const gridd_http_opts_t *opts,
                                  gridd_http_response_t *resp) {
    if (!method || !url || !resp) return GRIDD_ERR_INVALID_PARAM;
    memset(resp, 0, sizeof(*resp));

    parsed_url_t u;
    if (!parse_url(url, &u)) {
        snprintf(resp->err_msg, sizeof(resp->err_msg), "Invalid URL");
        resp->err = GRIDD_ERR_INVALID_PARAM;
        return GRIDD_ERR_INVALID_PARAM;
    }

    long timeout = (opts && opts->timeout_sec > 0) ? opts->timeout_sec : 30L;
    bool using_proxy = (opts && opts->proxy_host[0] != '\0');
    const char *conn_host = using_proxy ? opts->proxy_host : u.host;
    const char *conn_port = (using_proxy && opts->proxy_port[0] != '\0')
                            ? opts->proxy_port
                            : (using_proxy ? "3128" : u.port);

    transport_t t = {.fd = -1, .ssl = NULL, .ctx = NULL};
    t.fd = connect_tcp(conn_host, conn_port, timeout, resp->err_msg, sizeof(resp->err_msg));
    if (t.fd < 0) {
        resp->err = GRIDD_ERR_IO;
        return GRIDD_ERR_IO;
    }

    if (using_proxy && u.is_https) {
        if (!proxy_connect_tunnel(t.fd, &u, opts, resp->err_msg, sizeof(resp->err_msg))) {
            close(t.fd);
            resp->err = GRIDD_ERR_IO;
            return GRIDD_ERR_IO;
        }
    }

    if (u.is_https) {
        t.ctx = SSL_CTX_new(TLS_client_method());
        if (!t.ctx) {
            close(t.fd);
            snprintf(resp->err_msg, sizeof(resp->err_msg), "Failed to create TLS context");
            resp->err = GRIDD_ERR_IO;
            return GRIDD_ERR_IO;
        }
        SSL_CTX_set_min_proto_version(t.ctx, TLS1_2_VERSION);
        SSL_CTX_set_default_verify_paths(t.ctx);
        t.ssl = SSL_new(t.ctx);
        if (!t.ssl) {
            SSL_CTX_free(t.ctx);
            close(t.fd);
            snprintf(resp->err_msg, sizeof(resp->err_msg), "Failed to create TLS session");
            resp->err = GRIDD_ERR_IO;
            return GRIDD_ERR_IO;
        }
        SSL_set_tlsext_host_name(t.ssl, u.host);
        SSL_set_fd(t.ssl, t.fd);
        if (SSL_connect(t.ssl) != 1) {
            SSL_free(t.ssl);
            SSL_CTX_free(t.ctx);
            close(t.fd);
            snprintf(resp->err_msg, sizeof(resp->err_msg), "TLS handshake failed");
            resp->err = GRIDD_ERR_IO;
            return GRIDD_ERR_IO;
        }
    }

    size_t hdr_cap = 4096U;
    char *req_hdr = (char *)calloc(1, hdr_cap);
    if (!req_hdr) {
        if (t.ssl) SSL_free(t.ssl);
        if (t.ctx) SSL_CTX_free(t.ctx);
        close(t.fd);
        resp->err = GRIDD_ERR_GENERIC;
        return GRIDD_ERR_GENERIC;
    }

    int off = snprintf(req_hdr, hdr_cap,
                       "%s %s HTTP/1.1\r\nHost: %s:%s\r\nConnection: close\r\n",
                       method, u.path, u.host, u.port);
    for (size_t i = 0; i < header_count && off > 0 && (size_t)off < hdr_cap; ++i) {
        if (headers && headers[i]) {
            off += snprintf(req_hdr + off, hdr_cap - (size_t)off, "%s\r\n", headers[i]);
        }
    }
    if (body && body_len > 0 && off > 0 && (size_t)off < hdr_cap) {
        off += snprintf(req_hdr + off, hdr_cap - (size_t)off, "Content-Length: %zu\r\n", body_len);
    }
    if (off > 0 && (size_t)off < hdr_cap) {
        off += snprintf(req_hdr + off, hdr_cap - (size_t)off, "\r\n");
    }

    bool send_ok = (off > 0 && (size_t)off < hdr_cap && t_send_all(&t, req_hdr, (size_t)off));
    free(req_hdr);
    if (send_ok && body && body_len > 0) {
        send_ok = t_send_all(&t, body, body_len);
    }
    if (!send_ok) {
        if (t.ssl) SSL_free(t.ssl);
        if (t.ctx) SSL_CTX_free(t.ctx);
        close(t.fd);
        snprintf(resp->err_msg, sizeof(resp->err_msg), "Generic network communication failure");
        resp->err = GRIDD_ERR_IO;
        return GRIDD_ERR_IO;
    }

    size_t rcap = 8192U;
    size_t rlen = 0;
    char *raw = (char *)calloc(1, rcap);
    if (!raw) {
        if (t.ssl) SSL_free(t.ssl);
        if (t.ctx) SSL_CTX_free(t.ctx);
        close(t.fd);
        resp->err = GRIDD_ERR_GENERIC;
        return GRIDD_ERR_GENERIC;
    }

    for (;;) {
        if (rlen + 4096U + 1U > rcap) {
            if (rcap >= (size_t)1 << 22) break;
            rcap *= 2U;
            char *np = (char *)realloc(raw, rcap);
            if (!np) break;
            raw = np;
        }
        ssize_t n = t_recv(&t, raw + rlen, rcap - 1U - rlen);
        if (n <= 0) break;
        rlen += (size_t)n;
        raw[rlen] = '\0';
    }

    if (t.ssl) {
        SSL_shutdown(t.ssl);
        SSL_free(t.ssl);
    }
    if (t.ctx) SSL_CTX_free(t.ctx);
    close(t.fd);

    char *sep = strstr(raw, "\r\n\r\n");
    if (!sep) {
        free(raw);
        snprintf(resp->err_msg, sizeof(resp->err_msg), "Invalid HTTP response from server");
        resp->err = GRIDD_ERR_IO;
        return GRIDD_ERR_IO;
    }

    *sep = '\0';
    const char *hdr_block = raw;
    const char *body_start = sep + 4;
    size_t raw_body_len = rlen - (size_t)(body_start - raw);

    long status = 0;
    if (sscanf(hdr_block, "HTTP/%*d.%*d %ld", &status) != 1) {
        free(raw);
        snprintf(resp->err_msg, sizeof(resp->err_msg), "Malformed HTTP status line");
        resp->err = GRIDD_ERR_IO;
        return GRIDD_ERR_IO;
    }
    resp->status_code = status;

    if (strcasestr(hdr_block, "Transfer-Encoding: chunked") != NULL) {
        decode_chunked_body(body_start, raw_body_len, &resp->body, &resp->body_len);
    } else {
        resp->body = (char *)calloc(1, raw_body_len + 1U);
        if (resp->body) {
            memcpy(resp->body, body_start, raw_body_len);
            resp->body[raw_body_len] = '\0';
            resp->body_len = raw_body_len;
        }
    }
    free(raw);
    resp->err = GRIDD_OK;
    return GRIDD_OK;
}
