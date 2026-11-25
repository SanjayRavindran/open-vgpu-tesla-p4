#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#define NVML_SUCCESS                0
#define NVML_ERROR_NOT_SUPPORTED    3
#define NVML_ERROR_TIMEOUT          10
#define NVML_EVENT_XID_CRITICAL_ERR 0x8ULL

#define DEFAULT_NVML_LIB            "libnvidia-ml.so.1"
#define DEFAULT_DEBUGDUMP           "/usr/bin/nvidia-debugdump"
#define DEFAULT_TEMP_DUMP           "/tmp/nvidia-debugdump-temp.log"
#define DEFAULT_WAIT_TIMEOUT_MS     100000U

typedef void *nvmlDevice_t;
typedef void *nvmlEventSet_t;
typedef int nvmlReturn_t;

typedef struct nvmlPciInfo_st {
    char busIdLegacy[16];
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    unsigned int pciDeviceId;
    unsigned int pciSubSystemId;
    char busId[32];
} nvmlPciInfo_t;

typedef struct nvmlEventData_st {
    nvmlDevice_t device;
    unsigned long long eventType;
    unsigned long long eventData;
    unsigned int gpuInstanceId;
    unsigned int computeInstanceId;
} nvmlEventData_t;

typedef struct NvmlApi {
    void *dl;
    nvmlReturn_t (*init_v2)(void);
    nvmlReturn_t (*shutdown)(void);
    const char *(*error_string)(nvmlReturn_t);
    nvmlReturn_t (*device_get_count_v2)(unsigned int *);
    nvmlReturn_t (*device_get_handle_by_index_v2)(unsigned int, nvmlDevice_t *);
    nvmlReturn_t (*device_get_gsp_firmware_mode)(nvmlDevice_t, unsigned int *, unsigned int *);
    nvmlReturn_t (*device_get_pci_info_v3)(nvmlDevice_t, nvmlPciInfo_t *);
    nvmlReturn_t (*device_get_supported_event_types)(nvmlDevice_t, unsigned long long *);
    nvmlReturn_t (*device_register_events)(nvmlDevice_t, unsigned long long, nvmlEventSet_t);
    nvmlReturn_t (*device_get_index)(nvmlDevice_t, unsigned int *);
    nvmlReturn_t (*event_set_create)(nvmlEventSet_t *);
    nvmlReturn_t (*event_set_wait_v2)(nvmlEventSet_t, nvmlEventData_t *, unsigned int);
    nvmlReturn_t (*event_set_free)(nvmlEventSet_t);
} NvmlApi;

static volatile sig_atomic_t g_stop = 0;
static int g_pid_fd = -1;
static pid_t g_daemon_pid = 0;
static char g_pid_path[4096];
static FILE *g_log_fp = NULL;
static nvmlEventSet_t g_event_set = NULL;
static NvmlApi g_nvml;
static int g_nvml_initialized = 0;

static void log_msg(int prio, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    syslog(prio, "%s", buf);
    if (g_log_fp) {
        fprintf(g_log_fp, "%s\n", buf);
        fflush(g_log_fp);
    }
}

static void on_signal(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_stop = 1;
    }
}

static void cleanup_and_exit(int exit_code) {
    if (g_event_set && g_nvml.event_set_free) {
        g_nvml.event_set_free(g_event_set);
        g_event_set = NULL;
    }
    if (g_nvml_initialized && g_nvml.shutdown) {
        nvmlReturn_t rc = g_nvml.shutdown();
        if (rc != NVML_SUCCESS && g_nvml.error_string) {
            log_msg(LOG_ERR, "Failed to shutdown NVML: %s", g_nvml.error_string(rc));
        }
        g_nvml_initialized = 0;
    }
    if (g_pid_fd != -1) {
        if (lockf(g_pid_fd, F_ULOCK, 0) < 0) {
            log_msg(LOG_ERR, "Failed to unlock PID file: %s", strerror(errno));
        }
        if (close(g_pid_fd) < 0) {
            log_msg(LOG_ERR, "Failed to close PID file: %s", strerror(errno));
        }
        g_pid_fd = -1;
        if (g_pid_path[0] && unlink(g_pid_path) < 0) {
            log_msg(LOG_ERR, "Failed to remove PID file: %s", strerror(errno));
        }
    }
    log_msg(LOG_INFO, "Shutdown (%d)", (int)g_daemon_pid);
    closelog();
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
    if (g_nvml.dl) {
        dlclose(g_nvml.dl);
        g_nvml.dl = NULL;
    }
    exit(exit_code);
}

static int load_nvml(void) {
    const char *lib = getenv("OPEN_XID_NVML_LIB");
    if (!lib || !*lib) {
        lib = DEFAULT_NVML_LIB;
    }
    memset(&g_nvml, 0, sizeof(g_nvml));
    g_nvml.dl = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!g_nvml.dl) {
        log_msg(LOG_ERR, "Failed to open NVML library %s: %s", lib, dlerror());
        return -1;
    }
#define LOAD_SYM(field, name)                                                     \
    do {                                                                          \
        g_nvml.field = dlsym(g_nvml.dl, name);                                    \
        if (!g_nvml.field) {                                                      \
            log_msg(LOG_ERR, "Missing NVML symbol %s", name);                     \
            return -1;                                                            \
        }                                                                         \
    } while (0)

    LOAD_SYM(init_v2, "nvmlInit_v2");
    LOAD_SYM(shutdown, "nvmlShutdown");
    LOAD_SYM(error_string, "nvmlErrorString");
    LOAD_SYM(device_get_count_v2, "nvmlDeviceGetCount_v2");
    LOAD_SYM(device_get_handle_by_index_v2, "nvmlDeviceGetHandleByIndex_v2");
    LOAD_SYM(device_get_gsp_firmware_mode, "nvmlDeviceGetGspFirmwareMode");
    LOAD_SYM(device_get_pci_info_v3, "nvmlDeviceGetPciInfo_v3");
    LOAD_SYM(device_get_supported_event_types, "nvmlDeviceGetSupportedEventTypes");
    LOAD_SYM(device_register_events, "nvmlDeviceRegisterEvents");
    LOAD_SYM(device_get_index, "nvmlDeviceGetIndex");
    LOAD_SYM(event_set_create, "nvmlEventSetCreate");
    LOAD_SYM(event_set_wait_v2, "nvmlEventSetWait_v2");
    LOAD_SYM(event_set_free, "nvmlEventSetFree");
#undef LOAD_SYM
    return 0;
}

static char *base64_encode(const uint8_t *in, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_len = ((len + 2) / 3) * 4;
    char *out = (char *)malloc(out_len + 1);
    if (!out) {
        return NULL;
    }
    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t a = in[i++];
        uint32_t b = (i < len) ? in[i++] : 0;
        uint32_t c = (i < len) ? in[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[j++] = tbl[(triple >> 18) & 0x3f];
        out[j++] = tbl[(triple >> 12) & 0x3f];
        out[j++] = tbl[(triple >> 6) & 0x3f];
        out[j++] = tbl[triple & 0x3f];
    }
    size_t rem = len % 3;
    if (rem == 1) {
        out[out_len - 1] = '=';
        out[out_len - 2] = '=';
    } else if (rem == 2) {
        out[out_len - 1] = '=';
    }
    out[out_len] = '\0';
    return out;
}

static void capture_and_log_xid_dump(unsigned int dev_idx, unsigned long long xid) {
    const char *dump_bin = getenv("OPEN_XID_DEBUGDUMP");
    const char *temp_dump = getenv("OPEN_XID_TEMP_DUMP");
    if (!dump_bin || !*dump_bin) dump_bin = DEFAULT_DEBUGDUMP;
    if (!temp_dump || !*temp_dump) temp_dump = DEFAULT_TEMP_DUMP;

    log_msg(LOG_INFO, "XID %llu detected on device index: %u, initiating dump...", xid, dev_idx);

    char *argv_exec[] = {
        (char *)dump_bin,
        (char *)"--ioctl",
        (char *)"--nvlogonly",
        (char *)"-f",
        (char *)temp_dump,
        NULL,
    };
    pid_t child = fork();
    if (child < 0) {
        log_msg(LOG_ERR, "Failed to fork %s", dump_bin);
        return;
    }
    if (child == 0) {
        execv(dump_bin, argv_exec);
        _exit(127);
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            log_msg(LOG_ERR, "Error waiting for child: %s", strerror(errno));
            return;
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        log_msg(LOG_ERR, "Dump tool exited abnormally (%d)", WEXITSTATUS(status));
        return;
    }

    FILE *fp = fopen(temp_dump, "rb");
    if (!fp) {
        log_msg(LOG_ERR, "Failed to open dump file");
        return;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        log_msg(LOG_ERR, "Failed to get dump file size");
        fclose(fp);
        unlink(temp_dump);
        return;
    }
    long sz = ftell(fp);
    if (sz <= 0) {
        log_msg(LOG_ERR, "Invalid dump file size");
        fclose(fp);
        unlink(temp_dump);
        return;
    }
    fseek(fp, 0, SEEK_SET);
    uint8_t *raw = (uint8_t *)malloc((size_t)sz);
    if (!raw) {
        log_msg(LOG_ERR, "Failed to allocate memory");
        fclose(fp);
        unlink(temp_dump);
        return;
    }
    size_t got = fread(raw, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) {
        log_msg(LOG_ERR, "Error reading dump file");
        free(raw);
        unlink(temp_dump);
        return;
    }

    char *b64 = base64_encode(raw, got);
    free(raw);
    if (!b64) {
        log_msg(LOG_ERR, "Failed to allocate memory");
        unlink(temp_dump);
        return;
    }

    log_msg(LOG_INFO, "XID dump start");
    size_t blen = strlen(b64);
    for (size_t off = 0; off < blen; off += 76) {
        char chunk[77];
        size_t n = (blen - off < 76) ? (blen - off) : 76;
        memcpy(chunk, b64 + off, n);
        chunk[n] = '\0';
        log_msg(LOG_INFO, "%s", chunk);
    }
    log_msg(LOG_INFO, "XID dump end");
    free(b64);

    if (unlink(temp_dump) < 0) {
        log_msg(LOG_ERR, "Failed to remove dump file: %s", strerror(errno));
    }
}

int main(int argc, char **argv) {
    int foreground = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--foreground") == 0) {
            foreground = 1;
        } else if (strcmp(argv[i], "--version") == 0) {
            printf("open-nvidia-xid-logd 0.1.0 (580.196.09-vgpu-kvm compatible)\n");
            return 0;
        }
    }
    const char *fg_env = getenv("OPEN_XID_FOREGROUND");
    if (fg_env && strcmp(fg_env, "1") == 0) {
        foreground = 1;
    }

    const char *log_file_env = getenv("OPEN_XID_LOG_FILE");
    const char *runtime_dir = getenv("RUNTIME_DIRECTORY");
    if (!runtime_dir || !*runtime_dir) {
        fprintf(stderr, "open-nvidia-xid-logd: RUNTIME_DIRECTORY must be set\n");
        return 1;
    }

    if (!foreground) {
        pid_t pid = fork();
        if (pid < 0) {
            return 1;
        }
        if (pid > 0) {
            return 0;
        }
        if (setsid() < 0) {
            return 1;
        }
    }
    g_daemon_pid = getpid();

    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (!foreground) {
        umask(0);
        if (chdir("/") != 0) {
            return 1;
        }
        long max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd < 0) max_fd = 1024;
        for (long fd = max_fd; fd >= 0; fd--) {
            close((int)fd);
        }
    }

    setlogmask(LOG_UPTO(LOG_DEBUG));
    openlog("nvidia-xid-logd", LOG_PID, LOG_DAEMON);
    if (log_file_env && *log_file_env) {
        g_log_fp = fopen(log_file_env, "a");
    }

    if (access(runtime_dir, R_OK | W_OK) < 0) {
        log_msg(LOG_ERR, "Unable to access %s: %s", runtime_dir, strerror(errno));
        cleanup_and_exit(1);
    }

    int plen = snprintf(g_pid_path, sizeof(g_pid_path), "%s/%s", runtime_dir, "nvidia-xid-logd.pid");
    if (plen < 0 || (size_t)plen >= sizeof(g_pid_path)) {
        log_msg(LOG_ERR, "Runtime directory path too long");
        g_pid_path[0] = '\0';
        cleanup_and_exit(1);
    }

    g_pid_fd = open(g_pid_path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (g_pid_fd < 0) {
        log_msg(LOG_ERR, "Failed to open PID file: %s", strerror(errno));
        cleanup_and_exit(1);
    }
    if (lockf(g_pid_fd, F_TLOCK, 0) < 0) {
        log_msg(LOG_ERR, "Failed to lock PID file: %s", strerror(errno));
        cleanup_and_exit(1);
    }
    char pid_str[32];
    int n_pid = snprintf(pid_str, sizeof(pid_str), "%d\n", (int)g_daemon_pid);
    if (write(g_pid_fd, pid_str, (size_t)n_pid) != n_pid) {
        log_msg(LOG_ERR, "Failed to update PID file: %s", strerror(errno));
        cleanup_and_exit(1);
    }

    log_msg(LOG_INFO, "Started (%d)", (int)g_daemon_pid);

    if (load_nvml() != 0) {
        cleanup_and_exit(1);
    }

    nvmlReturn_t rc = g_nvml.init_v2();
    if (rc != NVML_SUCCESS) {
        log_msg(LOG_ERR, "Failed to initialize NVML: %s", g_nvml.error_string(rc));
        cleanup_and_exit(1);
    }
    g_nvml_initialized = 1;

    unsigned int count = 0;
    rc = g_nvml.device_get_count_v2(&count);
    if (rc != NVML_SUCCESS) {
        log_msg(LOG_ERR, "Failed to determine the number of available devices: %s",
                g_nvml.error_string(rc));
        cleanup_and_exit(1);
    }

    rc = g_nvml.event_set_create(&g_event_set);
    if (rc != NVML_SUCCESS) {
        log_msg(LOG_ERR, "Failed to create events set: %s", g_nvml.error_string(rc));
        cleanup_and_exit(1);
    }

    for (unsigned int i = 0; i < count; i++) {
        nvmlDevice_t dev = NULL;
        rc = g_nvml.device_get_handle_by_index_v2(i, &dev);
        if (rc != NVML_SUCCESS) {
            log_msg(LOG_ERR, "Failed to get handle for device %u: %s", i, g_nvml.error_string(rc));
            cleanup_and_exit(1);
        }

        unsigned int gsp_enabled = 0, gsp_default = 0;
        rc = g_nvml.device_get_gsp_firmware_mode(dev, &gsp_enabled, &gsp_default);
        if (rc == NVML_ERROR_NOT_SUPPORTED) {
            log_msg(LOG_DEBUG, "Non-GSP GPU detected at device %u. Exiting.", i);
            cleanup_and_exit(0);
        }
        if (rc != NVML_SUCCESS) {
            log_msg(LOG_ERR, "Failed to query gsp mode for device %u: %s", i,
                    g_nvml.error_string(rc));
            cleanup_and_exit(1);
        }

        nvmlPciInfo_t pci;
        memset(&pci, 0, sizeof(pci));
        rc = g_nvml.device_get_pci_info_v3(dev, &pci);
        if (rc != NVML_SUCCESS) {
            log_msg(LOG_ERR, "Failed to query pci info types for device %u: %s", i,
                    g_nvml.error_string(rc));
            cleanup_and_exit(1);
        }

        unsigned long long supported = 0;
        rc = g_nvml.device_get_supported_event_types(dev, &supported);
        if (rc != NVML_SUCCESS) {
            log_msg(LOG_ERR, "Failed to query supported event types for device %u: %s", i,
                    g_nvml.error_string(rc));
            cleanup_and_exit(1);
        }
        if ((supported & NVML_EVENT_XID_CRITICAL_ERR) == 0) {
            log_msg(LOG_ERR, "GPU %s: Xid events not supported on device", pci.busId);
            cleanup_and_exit(1);
        }

        log_msg(LOG_INFO, "GPU %s: Registering for Xid events", pci.busId);
        rc = g_nvml.device_register_events(dev, NVML_EVENT_XID_CRITICAL_ERR, g_event_set);
        if (rc != NVML_SUCCESS) {
            log_msg(LOG_ERR, "GPU %s: Failed to register for Xid events: %s", pci.busId,
                    g_nvml.error_string(rc));
            cleanup_and_exit(1);
        }
    }

    unsigned int wait_ms = DEFAULT_WAIT_TIMEOUT_MS;
    const char *wait_env = getenv("OPEN_XID_WAIT_MS");
    if (wait_env && *wait_env) {
        wait_ms = (unsigned int)strtoul(wait_env, NULL, 10);
    }

    log_msg(LOG_INFO, "Waiting for events...");
    while (!g_stop) {
        nvmlEventData_t ev;
        memset(&ev, 0, sizeof(ev));
        rc = g_nvml.event_set_wait_v2(g_event_set, &ev, wait_ms);
        if (rc == NVML_ERROR_TIMEOUT) {
            continue;
        }
        if (rc != NVML_SUCCESS) {
            log_msg(LOG_ERR, "Failed to wait for events: %s", g_nvml.error_string(rc));
            cleanup_and_exit(1);
        }
        if (ev.eventType == NVML_EVENT_XID_CRITICAL_ERR) {
            unsigned int idx = 0;
            rc = g_nvml.device_get_index(ev.device, &idx);
            if (rc != NVML_SUCCESS) {
                log_msg(LOG_ERR, "Failed to get device index: %s", g_nvml.error_string(rc));
                cleanup_and_exit(1);
            }
            capture_and_log_xid_dump(idx, ev.eventData);
        }
    }

    cleanup_and_exit(0);
    return 0;
}
