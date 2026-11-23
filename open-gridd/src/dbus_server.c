#define _GNU_SOURCE
#include "gridd/dbus_server.h"
#include "gridd/daemon.h"
#include "gridd/log.h"

#include <dlfcn.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct DBusConnection DBusConnection;
typedef struct DBusMessage DBusMessage;
typedef struct DBusWatch DBusWatch;

typedef struct {
    const char *name;
    const char *message;
    unsigned int dummy1;
    void *padding1;
} DBusError;

typedef struct {
    uint32_t dummy1;
    void *dummy2;
    uint32_t dummy3;
    int dummy4;
    int dummy5;
    int dummy6;
    int dummy7;
    int dummy8;
    int dummy9;
    int dummy10;
    int dummy11;
    int pad1;
    void *pad2;
    void *pad3;
} DBusMessageIter;

typedef unsigned int (*dbus_add_watch_fn)(DBusWatch *watch, void *data);
typedef void (*dbus_remove_watch_fn)(DBusWatch *watch, void *data);
typedef void (*dbus_watch_toggled_fn)(DBusWatch *watch, void *data);

typedef struct {
    void *handle;
    DBusConnection *(*bus_get)(int type, DBusError *error);
    void (*error_init)(DBusError *error);
    int (*error_is_set)(const DBusError *error);
    void (*error_free)(DBusError *error);
    int (*bus_request_name)(DBusConnection *conn, const char *name, unsigned int flags, DBusError *err);
    int (*msg_is_method_call)(DBusMessage *msg, const char *iface, const char *method);
    DBusMessage *(*msg_new_method_return)(DBusMessage *method_call);
    int (*iter_get_arg_type)(DBusMessageIter *iter);
    void (*iter_get_basic)(DBusMessageIter *iter, void *value);
    int (*iter_init)(DBusMessage *msg, DBusMessageIter *iter);
    void (*iter_init_append)(DBusMessage *msg, DBusMessageIter *iter);
    int (*iter_append_basic)(DBusMessageIter *iter, int type, const void *value);
    void (*msg_unref)(DBusMessage *msg);
    int (*conn_send)(DBusConnection *conn, DBusMessage *msg, uint32_t *serial);
    void (*conn_flush)(DBusConnection *conn);
    int (*conn_read_write)(DBusConnection *conn, int timeout_ms);
    DBusMessage *(*conn_pop_message)(DBusConnection *conn);
    int (*conn_dispatch)(DBusConnection *conn);
    int (*conn_set_watch_functions)(DBusConnection *conn,
                                    dbus_add_watch_fn add,
                                    dbus_remove_watch_fn remove,
                                    dbus_watch_toggled_fn toggled,
                                    void *data, void (*free_fn)(void *));
    int (*watch_get_unix_fd)(DBusWatch *watch);
    unsigned int (*watch_get_flags)(DBusWatch *watch);
    int (*watch_get_enabled)(DBusWatch *watch);
    int (*watch_handle)(DBusWatch *watch, unsigned int flags);

    DBusWatch *watches[8];
    int watch_count;
} dbus_api_t;

static unsigned int on_add_watch(DBusWatch *w, void *data) {
    dbus_api_t *api = (dbus_api_t *)data;
    if (api->watch_count < 8) {
        api->watches[api->watch_count++] = w;
    }
    return 1;
}

static void on_remove_watch(DBusWatch *w, void *data) {
    dbus_api_t *api = (dbus_api_t *)data;
    for (int i = 0; i < api->watch_count; ++i) {
        if (api->watches[i] == w) {
            api->watches[i] = api->watches[--api->watch_count];
            break;
        }
    }
}

static void on_watch_toggled(DBusWatch *w, void *data) {
    (void)w;
    (void)data;
}

static bool load_dbus(dbus_api_t *api) {
    memset(api, 0, sizeof(*api));
    api->handle = dlopen("libdbus-1.so.3", RTLD_LAZY);
    if (!api->handle) {
        LOG_E("Couldn't open libdbus-1.so.3");
        return false;
    }

#define LOAD_SYM(field, sym) \
    do { \
        void *s_ = dlsym(api->handle, sym); \
        if (!s_) { \
            LOG_E("Required libdbus symbol \"%s\" was not found", sym); \
            dlclose(api->handle); \
            api->handle = NULL; \
            return false; \
        } \
        memcpy(&api->field, &s_, sizeof(s_)); \
    } while (0)

    LOAD_SYM(bus_get, "dbus_bus_get");
    LOAD_SYM(error_init, "dbus_error_init");
    LOAD_SYM(error_is_set, "dbus_error_is_set");
    LOAD_SYM(error_free, "dbus_error_free");
    LOAD_SYM(bus_request_name, "dbus_bus_request_name");
    LOAD_SYM(msg_is_method_call, "dbus_message_is_method_call");
    LOAD_SYM(msg_new_method_return, "dbus_message_new_method_return");
    LOAD_SYM(iter_get_arg_type, "dbus_message_iter_get_arg_type");
    LOAD_SYM(iter_get_basic, "dbus_message_iter_get_basic");
    LOAD_SYM(iter_init, "dbus_message_iter_init");
    LOAD_SYM(iter_init_append, "dbus_message_iter_init_append");
    LOAD_SYM(iter_append_basic, "dbus_message_iter_append_basic");
    LOAD_SYM(msg_unref, "dbus_message_unref");
    LOAD_SYM(conn_send, "dbus_connection_send");
    LOAD_SYM(conn_flush, "dbus_connection_flush");
    LOAD_SYM(conn_read_write, "dbus_connection_read_write");
    LOAD_SYM(conn_pop_message, "dbus_connection_pop_message");
    LOAD_SYM(conn_dispatch, "dbus_connection_dispatch");
    LOAD_SYM(conn_set_watch_functions, "dbus_connection_set_watch_functions");
    LOAD_SYM(watch_get_unix_fd, "dbus_watch_get_unix_fd");
    LOAD_SYM(watch_get_flags, "dbus_watch_get_flags");
    LOAD_SYM(watch_get_enabled, "dbus_watch_get_enabled");
    LOAD_SYM(watch_handle, "dbus_watch_handle");
#undef LOAD_SYM

    return true;
}

static void handle_method_call(dbus_api_t *api, DBusConnection *conn, DBusMessage *msg,
                               gridd_dbus_ctx_t *ctx) {
    DBusMessageIter iter;
    if (!api->iter_init(msg, &iter)) {
        LOG_W("D-Bus message has no arguments");
        return;
    }

    int arg_type = api->iter_get_arg_type(&iter);
    int32_t cmd = 0;
    const char *str_arg = NULL;
    if (arg_type == 'i') {
        api->iter_get_basic(&iter, &cmd);
    } else if (arg_type == 's') {
        api->iter_get_basic(&iter, &str_arg);
    } else {
        LOG_W("D-Bus message received does not have a valid argument");
        return;
    }

    DBusMessage *reply = api->msg_new_method_return(msg);
    if (!reply) return;
    DBusMessageIter riter;
    api->iter_init_append(reply, &riter);

    if (str_arg) {
        const char *val = "Not Configured";
        if (strcmp(str_arg, "PrimaryServerAddress") == 0 && ctx->lm) {
            const char *u = gridd_lm_get_primary_url(ctx->lm);
            if (u) val = u;
        } else if (strcmp(str_arg, "SecondaryServerAddress") == 0 && ctx->lm) {
            const char *u = gridd_lm_get_secondary_url(ctx->lm);
            if (u) val = u;
        }
        api->iter_append_basic(&riter, 's', &val);
    } else {
        int32_t out_val = 0;
        if (cmd == 1 && ctx->lm) {
            out_val = (int32_t)gridd_lm_get_state(ctx->lm);
        } else if (cmd == 2) {
            uint32_t sig = 1;
            if (ctx->dbus_reload_pipe_wr >= 0) {
                ssize_t unused = write(ctx->dbus_reload_pipe_wr, &sig, sizeof(sig));
                (void)unused;
            }
            out_val = 0;
        } else if (cmd == 3 && ctx->active_feature_type) {
            out_val = (int32_t)(*ctx->active_feature_type);
        } else if (cmd == 4 && ctx->lm) {
            out_val = (int32_t)gridd_lm_get_lease_port(ctx->lm);
        }
        api->iter_append_basic(&riter, 'i', &out_val);
    }

    if (!api->conn_send(conn, reply, NULL)) {
        LOG_E("Could not send D-Bus message");
    } else {
        api->conn_flush(conn);
    }
    api->msg_unref(reply);
}

void *gridd_dbus_thread_main(void *arg) {
    gridd_dbus_ctx_t *ctx = (gridd_dbus_ctx_t *)arg;
    if (!ctx) return NULL;

    dbus_api_t api;
    if (!load_dbus(&api)) return NULL;

    DBusError err;
    api.error_init(&err);

    DBusConnection *conn = NULL;
    while (!gridd_daemon_stop_requested()) {
        conn = api.bus_get(1 /* DBUS_BUS_SYSTEM */, &err);
        if (!api.error_is_set(&err) && conn != NULL) break;
        api.error_free(&err);
        sleep(1);
    }
    if (!conn || gridd_daemon_stop_requested()) {
        if (api.handle) dlclose(api.handle);
        return NULL;
    }

    int req_rc = api.bus_request_name(conn, GRIDD_DBUS_BUS_NAME, 2 /* REPLACE_EXISTING */, &err);
    if (api.error_is_set(&err)) {
        LOG_E("Error requesting D-Bus name (%s)", err.message ? err.message : "unknown");
        api.error_free(&err);
    }
    if (req_rc != 1) {
        LOG_E("D-Bus name requested not found");
        dlclose(api.handle);
        return NULL;
    }

    api.conn_read_write(conn, 0);
    if (!api.conn_set_watch_functions(conn, on_add_watch, on_remove_watch, on_watch_toggled, &api, NULL)) {
        LOG_E("Could not set D-Bus watch functions");
        dlclose(api.handle);
        return NULL;
    }

    while (!gridd_daemon_stop_requested()) {
        struct pollfd pfds[9];
        int nfds = 0;
        DBusWatch *pwatches[8] = {0};

        if (ctx->stop_pipe_rd >= 0) {
            pfds[nfds].fd = ctx->stop_pipe_rd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        for (int i = 0; i < api.watch_count && nfds < 9; ++i) {
            if (!api.watch_get_enabled(api.watches[i])) continue;
            int fd = api.watch_get_unix_fd(api.watches[i]);
            unsigned int wf = api.watch_get_flags(api.watches[i]);
            short ev = 0;
            if (wf & 1U) ev |= POLLIN;
            if (wf & 2U) ev |= POLLOUT;
            pfds[nfds].fd = fd;
            pfds[nfds].events = ev;
            pfds[nfds].revents = 0;
            pwatches[nfds] = api.watches[i];
            nfds++;
        }

        if (poll(pfds, (nfds_t)nfds, -1) <= 0) break;
        if (ctx->stop_pipe_rd >= 0 && (pfds[0].revents & POLLIN) != 0) break;

        for (int i = 0; i < nfds; ++i) {
            if (!pwatches[i] || pfds[i].revents == 0) continue;
            unsigned int flags = 0;
            if (pfds[i].revents & POLLIN) flags |= 1U;
            if (pfds[i].revents & POLLOUT) flags |= 2U;
            if (pfds[i].revents & POLLERR) flags |= 4U;
            if (pfds[i].revents & POLLHUP) flags |= 8U;
            api.watch_handle(pwatches[i], flags);
        }

        while (api.conn_dispatch(conn) == 0) {}
        DBusMessage *msg = api.conn_pop_message(conn);
        if (msg) {
            if (api.msg_is_method_call(msg, GRIDD_DBUS_INTERFACE, GRIDD_DBUS_METHOD)) {
                handle_method_call(&api, conn, msg, ctx);
            }
            api.msg_unref(msg);
        }
    }

    dlclose(api.handle);
    return NULL;
}
