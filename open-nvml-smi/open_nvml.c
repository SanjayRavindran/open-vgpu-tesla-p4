/*
 * Clean-room open-source NVML implementation (libopen-nvidia-ml.so / libnvidia-ml.so.1)
 * For NVIDIA Tesla P4 (GP104GL) 580.196.09-vgpu-kvm Host & Guest Telemetry.
 *
 * SPDX-License-Identifier: MIT OR GPL-2.0-only
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "open_nvml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/ioctl.h>

/* RM ioctl numbers ('F' = 0x46) */
#define NV_IOCTL_MAGIC           'F'
#define NV_ESC_CARD_INFO         200U  /* 0xc8 */
#define NV_ESC_REGISTER_FD       201U  /* 0xc9 */
#define NV_ESC_CHECK_VERSION_STR 210U  /* 0xd2 */
#define NV_ESC_RM_FREE           0x29U
#define NV_ESC_RM_CONTROL        0x2aU
#define NV_ESC_RM_ALLOC          0x2bU

typedef struct {
    uint32_t cmd;
    uint32_t reply;
    char     versionString[64];
} nv_rm_api_version_t;

_Static_assert(sizeof(nv_rm_api_version_t) == 72, "nv_rm_api_version_t must be 72 bytes");

#define NV_IOCTL_CMD(nr, sz) \
    ((unsigned long)((3U << 30) | (((unsigned int)(sz) & 0x3fffU) << 16) | (NV_IOCTL_MAGIC << 8) | ((nr) & 0xffU)))

/* RM classes */
#define NV01_ROOT            0x00000041U
#define NV01_DEVICE_0        0x00000080U
#define NV20_SUBDEVICE_0     0x00002080U
#define NVA081_VGPU_CONFIG   0x0000a081U

/* RM Control commands verified via live GP104GL trace & REA 6.10.1 */
#define NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION              0x00000101U
#define NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS                  0x00000201U
#define NV0000_CTRL_CMD_GPU_GET_ID_INFO                       0x00000202U
#define NV0000_CTRL_CMD_GPU_GET_PROBED_IDS                    0x00000214U
#define NV0000_CTRL_CMD_GPU_ATTACH_IDS                        0x00000215U
#define NV0000_CTRL_CMD_GPU_GET_PCI_INFO                      0x0000021bU

#define NV0080_CTRL_CMD_GPU_GET_PERSISTENCE_STATE             0x00800288U
#define NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE           0x00800289U

#define NV2080_CTRL_CMD_GPU_GET_INFO_V2                       0x20800102U
#define NV2080_CTRL_CMD_GPU_GET_NAME_STRING                   0x20800110U
#define NV2080_CTRL_CMD_GPU_GET_SHORT_NAME_STRING             0x2080013fU
#define NV2080_CTRL_CMD_GPU_GET_GID_INFO                      0x2080014aU
#define NV2080_CTRL_CMD_GPU_GET_ACTIVE_PARTITION_IDS          0x2080018dU
#define NV2080_CTRL_CMD_GPU_GET_PID_INFO                      0x2080018eU
#define NV2080_CTRL_CMD_FB_GET_INFO_V2                        0x20801303U
#define NV2080_CTRL_CMD_BUS_GET_PCI_INFO                      0x20801801U
#define NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE                0x20808513U
#define NV2080_CTRL_CMD_THERMAL_GET_THRESHOLD                 0x20808546U
#define NV2080_CTRL_CMD_CLK_GET_INFO                          0x20809004U
#define NV2080_CTRL_CMD_CLK_GET_EXTENDED_INFO                 0x20809064U
#define NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE               0x2080a068U
#define NV2080_CTRL_CMD_PERF_GET_VID_ENG_UTIL                 0x2080a087U
#define NV2080_CTRL_CMD_PERF_GET_CLK_DOMAINS_SAMPLE           0x2080a097U
#define NV2080_CTRL_CMD_PMGR_PWR_MONITOR_GET_INFO             0x2080a612U
#define NV2080_CTRL_CMD_PMGR_PWR_MONITOR_GET_STATUS           0x2080a613U
#define NV2080_CTRL_CMD_PMGR_PWR_POLICY_GET_INFO              0x2080a618U
#define NV2080_CTRL_CMD_PMGR_PWR_POLICY_GET_CONTROL           0x2080a61dU
#define NV2080_CTRL_CMD_PMGR_PWR_POLICY_IDX_GET_LIMIT         0x2080a61fU

#define NVA081_CTRL_CMD_VGPU_CONFIG_GET_VGPU_INSTANCES        0xa0810102U
#define NVA081_CTRL_CMD_VGPU_CONFIG_GET_VGPU_TYPE_INFO        0xa0810103U
#define NVA081_CTRL_CMD_VGPU_CONFIG_GET_SUPPORTED_VGPU_TYPES  0xa0810104U
#define NVA081_CTRL_CMD_VGPU_CONFIG_GET_CREATABLE_VGPU_TYPES  0xa0810105U

typedef struct {
    uint32_t hRoot;
    uint32_t hObjectParent;
    uint32_t hObjectNew;
    uint32_t hClass;
    void    *pAllocParms;
    uint32_t paramsSize;
    uint32_t status;
} nv_rm_alloc_t;

typedef struct {
    uint32_t hRoot;
    uint32_t hObjectParent;
    uint32_t hObjectOld;
    uint32_t status;
} nv_rm_free_t;

typedef struct {
    uint32_t hClient;
    uint32_t hObject;
    uint32_t cmd;
    uint32_t flags;
    void    *params;
    uint32_t paramsSize;
    uint32_t status;
} nv_rm_control_t;

typedef struct {
    uint32_t valid;             /* +0x00 */
    uint32_t domain;            /* +0x04 */
    uint8_t  bus;               /* +0x08 */
    uint8_t  slot;              /* +0x09 */
    uint8_t  function;          /* +0x0a */
    uint8_t  pad0;              /* +0x0b */
    uint32_t vendor_device_id;  /* +0x0c */
    uint32_t gpu_id;            /* +0x10 */
    uint32_t interrupt_line;    /* +0x14 */
    uint64_t reg_address;       /* +0x18 */
    uint64_t reg_size;          /* +0x20 */
    uint64_t fb_address;        /* +0x28 */
    uint64_t fb_size;           /* +0x30 */
    uint32_t minor_number;      /* +0x38 */
    uint8_t  pad1[12];          /* +0x3c..0x47 */
} nv_card_info_t;

_Static_assert(sizeof(nv_card_info_t) == 72, "nv_card_info_t must be 72 bytes");

#define MAX_NVML_DEVICES 8

struct nvmlDevice_st {
    uint32_t index;
    uint32_t minor_number;
    uint32_t gpu_id;
    uint32_t domain;
    uint32_t bus;
    uint32_t device;
    uint32_t pci_device_id;
    uint32_t pci_subsystem_id;
    int      dev_fd;
    uint32_t h_client;
    uint32_t h_device;
    uint32_t h_subdevice;
    uint32_t h_vgpu_cfg;
    char     vbios_version[32];
};

struct nvmlEventSet_st {
    uint32_t magic;
    uint64_t registered_events;
};

static pthread_mutex_t    g_nvml_lock = PTHREAD_MUTEX_INITIALIZER;
static int                g_nvml_init_count = 0;
static int                g_nvidiactl_fd = -1;
static uint32_t           g_h_client = 0;
static uint32_t           g_device_count = 0;
static struct nvmlDevice_st g_devices[MAX_NVML_DEVICES];
static char               g_driver_version[80] = NVML_DEFAULT_DRIVER_VERSION_STR;
static open_nvml_ioctl_fn g_ioctl_hook = NULL;

void open_nvml_set_test_hooks(open_nvml_ioctl_fn ioctl_hook, const char *driver_version_override) {
    pthread_mutex_lock(&g_nvml_lock);
    g_ioctl_hook = ioctl_hook;
    if (driver_version_override && driver_version_override[0]) {
        snprintf(g_driver_version, sizeof(g_driver_version), "%s", driver_version_override);
    }
    pthread_mutex_unlock(&g_nvml_lock);
}

static int nv_ioctl(int fd, unsigned long req, void *arg) {
    if (g_ioctl_hook) {
        return g_ioctl_hook(fd, req, arg);
    }
    return ioctl(fd, req, arg);
}

static int rm_alloc(uint32_t hRoot, uint32_t hParent, uint32_t *hNewIo, uint32_t hClass, void *params, uint32_t paramsSize) {
    nv_rm_alloc_t a;
    memset(&a, 0, sizeof(a));
    a.hRoot = hRoot;
    a.hObjectParent = hParent;
    a.hObjectNew = *hNewIo;
    a.hClass = hClass;
    a.pAllocParms = params;
    a.paramsSize = paramsSize;
    if (nv_ioctl(g_nvidiactl_fd, NV_IOCTL_CMD(NV_ESC_RM_ALLOC, sizeof(a)), &a) < 0) {
        return -1;
    }
    if (a.status != 0) {
        return (int)a.status;
    }
    *hNewIo = a.hObjectNew;
    return 0;
}

static int rm_free(uint32_t hRoot, uint32_t hParent, uint32_t hOld) {
    nv_rm_free_t f;
    memset(&f, 0, sizeof(f));
    f.hRoot = hRoot;
    f.hObjectParent = hParent;
    f.hObjectOld = hOld;
    if (nv_ioctl(g_nvidiactl_fd, NV_IOCTL_CMD(NV_ESC_RM_FREE, sizeof(f)), &f) < 0) {
        return -1;
    }
    return (int)f.status;
}

static int rm_control(uint32_t hClient, uint32_t hObject, uint32_t cmd, void *params, uint32_t paramsSize) {
    nv_rm_control_t c;
    memset(&c, 0, sizeof(c));
    c.hClient = hClient;
    c.hObject = hObject;
    c.cmd = cmd;
    c.flags = 0;
    c.params = params;
    c.paramsSize = paramsSize;
    if (nv_ioctl(g_nvidiactl_fd, NV_IOCTL_CMD(NV_ESC_RM_CONTROL, sizeof(c)), &c) < 0) {
        return -1;
    }
    return (int)c.status;
}

static void read_proc_driver_version(void) {
    if (g_ioctl_hook) return;
    FILE *fp = fopen("/proc/driver/nvidia/version", "r");
    if (!fp) return;
    char line[256];
    if (fgets(line, sizeof(line), fp)) {
        const char *p = strstr(line, "Kernel Module");
        if (p) {
            p += strlen("Kernel Module");
            while (*p == ' ' || *p == '\t') p++;
            char ver[64] = {0};
            size_t i = 0;
            while (p[i] && p[i] != ' ' && p[i] != '\t' && p[i] != '\n' && i + 1 < sizeof(ver)) {
                ver[i] = p[i];
                i++;
            }
            ver[i] = '\0';
            if (i > 0) {
                snprintf(g_driver_version, sizeof(g_driver_version), "%s", ver);
            }
        }
    }
    fclose(fp);
}

static void read_proc_gpu_vbios(struct nvmlDevice_st *dev) {
    snprintf(dev->vbios_version, sizeof(dev->vbios_version), "86.04.8C.00.10");
    if (g_ioctl_hook) return;
    char path[128];
    snprintf(path, sizeof(path), "/proc/driver/nvidia/gpus/%04x:%02x:%02x.0/information",
             dev->domain, dev->bus, dev->device);
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Video BIOS:", 11) == 0) {
            const char *p = line + 11;
            while (*p == ' ' || *p == '\t') p++;
            size_t i = 0;
            while (p[i] && p[i] != '\n' && p[i] != '\r' && i + 1 < sizeof(dev->vbios_version)) {
                char c = p[i];
                if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
                dev->vbios_version[i] = c;
                i++;
            }
            dev->vbios_version[i] = '\0';
        }
    }
    fclose(fp);
}

static int valid_device(nvmlDevice_t device) {
    if (!g_nvml_init_count || !device) return 0;
    for (uint32_t i = 0; i < g_device_count; i++) {
        if (device == &g_devices[i]) return 1;
    }
    return 0;
}

nvmlReturn_t nvmlInitWithFlags(unsigned int flags) {
    (void)flags;
    pthread_mutex_lock(&g_nvml_lock);
    if (g_nvml_init_count > 0) {
        g_nvml_init_count++;
        pthread_mutex_unlock(&g_nvml_lock);
        return NVML_SUCCESS;
    }

    read_proc_driver_version();

    if (g_ioctl_hook) {
        g_nvidiactl_fd = 999;
    } else {
        const char *dev_path = getenv("OPEN_NVML_DEV_CTL");
        if (!dev_path || !dev_path[0]) dev_path = "/dev/nvidiactl";
        g_nvidiactl_fd = open(dev_path, O_RDWR | O_CLOEXEC);
        if (g_nvidiactl_fd < 0) {
            pthread_mutex_unlock(&g_nvml_lock);
            return NVML_ERROR_DRIVER_NOT_LOADED;
        }
    }

    nv_rm_api_version_t ver;
    memset(&ver, 0, sizeof(ver));
    ver.cmd = 0x31U; /* relaxed version check */
    snprintf(ver.versionString, sizeof(ver.versionString), "%.63s", g_driver_version);
    nv_ioctl(g_nvidiactl_fd, NV_IOCTL_CMD(NV_ESC_CHECK_VERSION_STR, sizeof(ver)), &ver);

    g_h_client = 0;
    if (rm_alloc(0, 0, &g_h_client, NV01_ROOT, NULL, 0) != 0) {
        if (!g_ioctl_hook && g_nvidiactl_fd >= 0) close(g_nvidiactl_fd);
        g_nvidiactl_fd = -1;
        pthread_mutex_unlock(&g_nvml_lock);
        return NVML_ERROR_DRIVER_NOT_LOADED;
    }

    nv_card_info_t cards[32];
    memset(cards, 0, sizeof(cards));
    int have_cards = (nv_ioctl(g_nvidiactl_fd, NV_IOCTL_CMD(NV_ESC_CARD_INFO, sizeof(cards)), cards) == 0);

    uint32_t attach_ids[33];
    memset(attach_ids, 0, sizeof(attach_ids));
    uint32_t n_attach = 0;
    for (uint32_t i = 0; i < 32; i++) {
        if (have_cards && cards[i].valid && cards[i].gpu_id != 0) {
            attach_ids[n_attach++] = cards[i].gpu_id;
        }
    }
    if (n_attach == 0) {
        attach_ids[n_attach++] = 0x00000100U;
    }
    attach_ids[n_attach] = 0xffffffffU;
    rm_control(g_h_client, g_h_client, NV0000_CTRL_CMD_GPU_ATTACH_IDS, attach_ids, sizeof(attach_ids));

    g_device_count = 0;
    for (uint32_t i = 0; i < 32 && g_device_count < MAX_NVML_DEVICES; i++) {
        if (have_cards && !cards[i].valid) continue;
        if (!have_cards && i > 0) break;

        struct nvmlDevice_st *d = &g_devices[g_device_count];
        memset(d, 0, sizeof(*d));
        d->index = g_device_count;
        d->minor_number = have_cards ? cards[i].minor_number : g_device_count;
        d->gpu_id = have_cards ? cards[i].gpu_id : 0x100U;
        d->domain = have_cards ? cards[i].domain : 0U;
        d->bus = have_cards ? cards[i].bus : 1U;
        d->device = have_cards ? cards[i].slot : 0U;
        d->pci_device_id = have_cards ? cards[i].vendor_device_id : 0x1bb310deU;
        d->pci_subsystem_id = 0x11d810deU;
        d->dev_fd = -1;
        d->h_client = g_h_client;
        d->h_device = 0xa55a0000U + (g_device_count * 0x100U);
        d->h_subdevice = d->h_device + 0x10U;
        d->h_vgpu_cfg = d->h_device + 0x30U;

        if (!g_ioctl_hook) {
            char dev_node[64];
            snprintf(dev_node, sizeof(dev_node), "/dev/nvidia%u", d->minor_number);
            d->dev_fd = open(dev_node, O_RDWR | O_CLOEXEC);
            if (d->dev_fd >= 0) {
                int ctl_fd = g_nvidiactl_fd;
                nv_ioctl(d->dev_fd, NV_IOCTL_CMD(NV_ESC_REGISTER_FD, sizeof(ctl_fd)), &ctl_fd);
            }
        }

        uint8_t dev_alloc_params[56];
        memset(dev_alloc_params, 0, sizeof(dev_alloc_params));
        *(uint32_t *)&dev_alloc_params[0] = d->index;
        if (rm_alloc(g_h_client, g_h_client, &d->h_device, NV01_DEVICE_0, dev_alloc_params, sizeof(dev_alloc_params)) != 0) {
            rm_alloc(g_h_client, g_h_client, &d->h_device, NV01_DEVICE_0, NULL, 0);
        }

        uint32_t subdev_alloc_params = 0;
        if (rm_alloc(g_h_client, d->h_device, &d->h_subdevice, NV20_SUBDEVICE_0, &subdev_alloc_params, sizeof(subdev_alloc_params)) != 0) {
            rm_alloc(g_h_client, d->h_device, &d->h_subdevice, NV20_SUBDEVICE_0, NULL, 0);
        }

        rm_alloc(g_h_client, d->h_subdevice, &d->h_vgpu_cfg, NVA081_VGPU_CONFIG, NULL, 0);

        uint32_t pci_ids[4] = {0};
        if (rm_control(d->h_client, d->h_subdevice, NV2080_CTRL_CMD_BUS_GET_PCI_INFO, pci_ids, sizeof(pci_ids)) == 0) {
            if (pci_ids[0]) d->pci_device_id = pci_ids[0];
            if (pci_ids[1]) d->pci_subsystem_id = pci_ids[1];
        }

        read_proc_gpu_vbios(d);
        g_device_count++;
    }

    g_nvml_init_count = 1;
    pthread_mutex_unlock(&g_nvml_lock);
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlInit(void) {
    return nvmlInitWithFlags(0);
}

nvmlReturn_t nvmlInit_v2(void) {
    return nvmlInitWithFlags(0);
}

nvmlReturn_t nvmlShutdown(void) {
    pthread_mutex_lock(&g_nvml_lock);
    if (g_nvml_init_count <= 0) {
        pthread_mutex_unlock(&g_nvml_lock);
        return NVML_ERROR_UNINITIALIZED;
    }
    g_nvml_init_count--;
    if (g_nvml_init_count == 0) {
        for (uint32_t i = 0; i < g_device_count; i++) {
            struct nvmlDevice_st *d = &g_devices[i];
            if (d->h_vgpu_cfg) rm_free(d->h_client, d->h_subdevice, d->h_vgpu_cfg);
            if (d->h_subdevice) rm_free(d->h_client, d->h_device, d->h_subdevice);
            if (d->h_device) rm_free(d->h_client, d->h_client, d->h_device);
            if (!g_ioctl_hook && d->dev_fd >= 0) {
                close(d->dev_fd);
                d->dev_fd = -1;
            }
        }
        if (g_h_client) {
            rm_free(g_h_client, 0, g_h_client);
            g_h_client = 0;
        }
        if (!g_ioctl_hook && g_nvidiactl_fd >= 0) {
            close(g_nvidiactl_fd);
        }
        g_nvidiactl_fd = -1;
        g_device_count = 0;
    }
    pthread_mutex_unlock(&g_nvml_lock);
    return NVML_SUCCESS;
}

const char* nvmlErrorString(nvmlReturn_t result) {
    switch (result) {
        case NVML_SUCCESS: return "Success";
        case NVML_ERROR_UNINITIALIZED: return "Uninitialized";
        case NVML_ERROR_INVALID_ARGUMENT: return "Invalid Argument";
        case NVML_ERROR_NOT_SUPPORTED: return "Not Supported";
        case NVML_ERROR_NO_PERMISSION: return "Insufficient Permissions";
        case NVML_ERROR_ALREADY_INITIALIZED: return "Already Initialized";
        case NVML_ERROR_NOT_FOUND: return "Not Found";
        case NVML_ERROR_INSUFFICIENT_SIZE: return "Insufficient Size";
        case NVML_ERROR_DRIVER_NOT_LOADED: return "Driver Not Loaded";
        case NVML_ERROR_TIMEOUT: return "Timeout";
        default: return "Unknown Error";
    }
}

nvmlReturn_t nvmlInternalGetExportTable(const void **ppExportTable, const void *pExportTableId) {
    (void)pExportTableId;
    if (!ppExportTable) return NVML_ERROR_INVALID_ARGUMENT;
    *ppExportTable = NULL;
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlSystemGetDriverVersion(char *version, unsigned int length) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!version || length == 0) return NVML_ERROR_INVALID_ARGUMENT;
    size_t len = strlen(g_driver_version);
    if (length <= len) return NVML_ERROR_INSUFFICIENT_SIZE;
    memcpy(version, g_driver_version, len + 1);
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlSystemGetNVMLVersion(char *version, unsigned int length) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!version || length == 0) return NVML_ERROR_INVALID_ARGUMENT;
    char buf[96];
    snprintf(buf, sizeof(buf), "12.%s", g_driver_version);
    size_t len = strlen(buf);
    if (length <= len) return NVML_ERROR_INSUFFICIENT_SIZE;
    memcpy(version, buf, len + 1);
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlSystemGetCudaDriverVersion(int *cudaDriverVersion) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!cudaDriverVersion) return NVML_ERROR_INVALID_ARGUMENT;
    *cudaDriverVersion = 12020;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlSystemGetCudaDriverVersion_v2(int *cudaDriverVersion) {
    return nvmlSystemGetCudaDriverVersion(cudaDriverVersion);
}

nvmlReturn_t nvmlSystemGetProcessName(unsigned int pid, char *name, unsigned int length) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!name || length == 0) return NVML_ERROR_INVALID_ARGUMENT;

    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/comm", pid);
    FILE *fp = fopen(path, "r");
    if (fp) {
        char buf[128] = {0};
        if (fgets(buf, sizeof(buf), fp)) {
            size_t n = strcspn(buf, "\r\n");
            buf[n] = '\0';
            if (n > 0) {
                snprintf(name, length, "%s", buf);
                fclose(fp);
                return NVML_SUCCESS;
            }
        }
        fclose(fp);
    }
    snprintf(name, length, "vgpu");
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetCount(unsigned int *deviceCount) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!deviceCount) return NVML_ERROR_INVALID_ARGUMENT;
    *deviceCount = g_device_count;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetCount_v2(unsigned int *deviceCount) {
    return nvmlDeviceGetCount(deviceCount);
}

nvmlReturn_t nvmlDeviceGetHandleByIndex(unsigned int index, nvmlDevice_t *device) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!device) return NVML_ERROR_INVALID_ARGUMENT;
    if (index >= g_device_count) return NVML_ERROR_INVALID_ARGUMENT;
    *device = &g_devices[index];
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(unsigned int index, nvmlDevice_t *device) {
    return nvmlDeviceGetHandleByIndex(index, device);
}

nvmlReturn_t nvmlDeviceGetHandleByPciBusId(const char *pciBusId, nvmlDevice_t *device) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!pciBusId || !device) return NVML_ERROR_INVALID_ARGUMENT;
    unsigned int dom = 0, bus = 0, dev = 0;
    if (sscanf(pciBusId, "%x:%x:%x", &dom, &bus, &dev) != 3) {
        return NVML_ERROR_INVALID_ARGUMENT;
    }
    for (uint32_t i = 0; i < g_device_count; i++) {
        if (g_devices[i].domain == dom && g_devices[i].bus == bus && g_devices[i].device == dev) {
            *device = &g_devices[i];
            return NVML_SUCCESS;
        }
    }
    return NVML_ERROR_NOT_FOUND;
}

nvmlReturn_t nvmlDeviceGetHandleByPciBusId_v2(const char *pciBusId, nvmlDevice_t *device) {
    return nvmlDeviceGetHandleByPciBusId(pciBusId, device);
}

nvmlReturn_t nvmlDeviceGetHandleByUUID(const char *uuid, nvmlDevice_t *device) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!uuid || !device) return NVML_ERROR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < g_device_count; i++) {
        char u[96] = {0};
        if (nvmlDeviceGetUUID(&g_devices[i], u, sizeof(u)) == NVML_SUCCESS && strcmp(u, uuid) == 0) {
            *device = &g_devices[i];
            return NVML_SUCCESS;
        }
    }
    return NVML_ERROR_NOT_FOUND;
}

nvmlReturn_t nvmlDeviceGetHandleBySerial(const char *serial, nvmlDevice_t *device) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!serial || !device) return NVML_ERROR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < g_device_count; i++) {
        char s[64] = {0};
        if (nvmlDeviceGetSerial(&g_devices[i], s, sizeof(s)) == NVML_SUCCESS && strcmp(s, serial) == 0) {
            *device = &g_devices[i];
            return NVML_SUCCESS;
        }
    }
    return NVML_ERROR_NOT_FOUND;
}

nvmlReturn_t nvmlDeviceGetIndex(nvmlDevice_t device, unsigned int *index) {
    if (!valid_device(device) || !index) return NVML_ERROR_INVALID_ARGUMENT;
    *index = device->index;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetMinorNumber(nvmlDevice_t device, unsigned int *minorNumber) {
    if (!valid_device(device) || !minorNumber) return NVML_ERROR_INVALID_ARGUMENT;
    *minorNumber = device->minor_number;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetName(nvmlDevice_t device, char *name, unsigned int length) {
    if (!valid_device(device) || !name || length == 0) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t buf[132];
    memset(buf, 0, sizeof(buf));
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_GPU_GET_NAME_STRING, buf, sizeof(buf)) != 0) {
        snprintf(name, length, "Tesla P4");
        return NVML_SUCCESS;
    }
    const char *s = (const char *)&buf[4];
    size_t len = strnlen(s, 127);
    if (length <= len) return NVML_ERROR_INSUFFICIENT_SIZE;
    memcpy(name, s, len);
    name[len] = '\0';
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetUUID(nvmlDevice_t device, char *uuid, unsigned int length) {
    if (!valid_device(device) || !uuid || length == 0) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t buf[268];
    memset(buf, 0, sizeof(buf));
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_GPU_GET_GID_INFO, buf, sizeof(buf)) != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    const char *s = (const char *)&buf[12];
    size_t len = strnlen(s, 255);
    if (length <= len) return NVML_ERROR_INSUFFICIENT_SIZE;
    memcpy(uuid, s, len);
    uuid[len] = '\0';
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetSerial(nvmlDevice_t device, char *serial, unsigned int length) {
    if (!valid_device(device) || !serial || length == 0) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t buf[136];
    memset(buf, 0, sizeof(buf));
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_GPU_GET_SHORT_NAME_STRING, buf, sizeof(buf)) != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    const char *s = (const char *)&buf[0x1c];
    size_t len = strnlen(s, 16);
    if (length <= len) return NVML_ERROR_INSUFFICIENT_SIZE;
    memcpy(serial, s, len);
    serial[len] = '\0';
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetBoardPartNumber(nvmlDevice_t device, char *partNumber, unsigned int length) {
    if (!valid_device(device) || !partNumber || length == 0) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t buf[136];
    memset(buf, 0, sizeof(buf));
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_GPU_GET_SHORT_NAME_STRING, buf, sizeof(buf)) != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    const char *s = (const char *)&buf[0x47];
    size_t len = strnlen(s, 24);
    if (length <= len) return NVML_ERROR_INSUFFICIENT_SIZE;
    memcpy(partNumber, s, len);
    partNumber[len] = '\0';
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetVbiosVersion(nvmlDevice_t device, char *version, unsigned int length) {
    if (!valid_device(device) || !version || length == 0) return NVML_ERROR_INVALID_ARGUMENT;
    size_t len = strlen(device->vbios_version);
    if (length <= len) return NVML_ERROR_INSUFFICIENT_SIZE;
    memcpy(version, device->vbios_version, len + 1);
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetBrand(nvmlDevice_t device, nvmlBrandType_t *type) {
    if (!valid_device(device) || !type) return NVML_ERROR_INVALID_ARGUMENT;
    *type = NVML_BRAND_TESLA;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetArchitecture(nvmlDevice_t device, nvmlDeviceArchitecture_t *arch) {
    if (!valid_device(device) || !arch) return NVML_ERROR_INVALID_ARGUMENT;
    *arch = NVML_DEVICE_ARCH_PASCAL;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetPciInfo_v3(nvmlDevice_t device, nvmlPciInfo_t *pci) {
    if (!valid_device(device) || !pci) return NVML_ERROR_INVALID_ARGUMENT;
    memset(pci, 0, sizeof(*pci));
    pci->domain = device->domain;
    pci->bus = device->bus;
    pci->device = device->device;
    pci->pciDeviceId = device->pci_device_id;
    pci->pciSubSystemId = device->pci_subsystem_id;
    snprintf(pci->busIdLegacy, sizeof(pci->busIdLegacy), "%04X:%02X:%02X.0",
             device->domain & 0xffffU, device->bus & 0xffU, device->device & 0xffU);
    snprintf(pci->busId, sizeof(pci->busId), "%08X:%02X:%02X.0",
             device->domain, device->bus & 0xffU, device->device & 0xffU);
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetPciInfo_v2(nvmlDevice_t device, nvmlPciInfo_t *pci) {
    return nvmlDeviceGetPciInfo_v3(device, pci);
}

nvmlReturn_t nvmlDeviceGetPciInfo(nvmlDevice_t device, nvmlPciInfo_t *pci) {
    return nvmlDeviceGetPciInfo_v3(device, pci);
}

nvmlReturn_t nvmlDeviceGetMemoryInfo(nvmlDevice_t device, nvmlMemory_t *memory) {
    if (!valid_device(device) || !memory) return NVML_ERROR_INVALID_ARGUMENT;
    uint32_t buf[109]; /* 436 bytes */
    memset(buf, 0, sizeof(buf));
    buf[0] = 3;      /* fbInfoListSize = 3 */
    buf[1] = 0x16U;  /* NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE */
    buf[3] = 0x08U;  /* NV2080_CTRL_FB_INFO_INDEX_TOTAL_RAM_SIZE */
    buf[5] = 0x09U;  /* NV2080_CTRL_FB_INFO_INDEX_HEAP_SIZE */
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_FB_GET_INFO_V2, buf, sizeof(buf)) != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    unsigned long long free_kib  = buf[2];
    unsigned long long total_kib = buf[4];
    memory->total = total_kib * 1024ULL;
    memory->free  = free_kib * 1024ULL;
    memory->used  = (total_kib >= free_kib) ? ((total_kib - free_kib) * 1024ULL) : 0ULL;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetMemoryInfo_v2(nvmlDevice_t device, nvmlMemory_v2_t *memory) {
    if (!valid_device(device) || !memory) return NVML_ERROR_INVALID_ARGUMENT;
    uint32_t buf[109]; /* 436 bytes */
    memset(buf, 0, sizeof(buf));
    buf[0] = 3;
    buf[1] = 0x16U;
    buf[3] = 0x08U;
    buf[5] = 0x09U;
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_FB_GET_INFO_V2, buf, sizeof(buf)) != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    unsigned long long free_kib   = buf[2];
    unsigned long long total_kib  = buf[4];
    unsigned long long usable_kib = buf[6] ? buf[6] : total_kib;
    memory->total    = total_kib * 1024ULL;
    memory->reserved = (total_kib >= usable_kib) ? ((total_kib - usable_kib) * 1024ULL) : 0ULL;
    memory->free     = free_kib * 1024ULL;
    memory->used     = (usable_kib >= free_kib) ? ((usable_kib - free_kib) * 1024ULL) : 0ULL;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetBAR1MemoryInfo(nvmlDevice_t device, nvmlBAR1Memory_t *bar1Memory) {
    if (!valid_device(device) || !bar1Memory) return NVML_ERROR_INVALID_ARGUMENT;
    uint32_t buf[109]; /* 436 bytes */
    memset(buf, 0, sizeof(buf));
    buf[0] = 2;
    buf[1] = 0x05U;  /* NV2080_CTRL_FB_INFO_INDEX_BAR1_SIZE */
    buf[3] = 0x1dU;  /* NV2080_CTRL_FB_INFO_INDEX_BAR1_AVAIL_SIZE */
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_FB_GET_INFO_V2, buf, sizeof(buf)) != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    unsigned long long total_kib = buf[2];
    unsigned long long free_kib  = buf[4];
    bar1Memory->bar1Total = total_kib * 1024ULL;
    bar1Memory->bar1Free  = free_kib * 1024ULL;
    bar1Memory->bar1Used  = (total_kib >= free_kib) ? ((total_kib - free_kib) * 1024ULL) : 0ULL;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetConfComputeProtectedMemoryUsage(nvmlDevice_t device, nvmlMemory_t *memory) {
    if (!valid_device(device) || !memory) return NVML_ERROR_INVALID_ARGUMENT;
    memset(memory, 0, sizeof(*memory));
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetTemperature(nvmlDevice_t device, nvmlTemperatureSensors_t sensorType, unsigned int *temp) {
    if (!valid_device(device) || !temp) return NVML_ERROR_INVALID_ARGUMENT;
    if (sensorType != NVML_TEMPERATURE_GPU) return NVML_ERROR_NOT_SUPPORTED;
    uint32_t buf[358]; /* 1432 bytes */
    memset(buf, 0, sizeof(buf));
    buf[0] = 1U;
    buf[2] = 0x2cU;
    buf[3] = 3U;
    buf[5] = 1U;
    buf[8] = 0x1500U;
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_THERMAL_SYSTEM_EXECUTE, buf, sizeof(buf)) != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *temp = buf[10];
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetTemperatureThreshold(nvmlDevice_t device, nvmlTemperatureThresholds_t thresholdType, unsigned int *temp) {
    if (!valid_device(device) || !temp) return NVML_ERROR_INVALID_ARGUMENT;
    uint32_t buf[6]; /* 24 bytes */
    memset(buf, 0, sizeof(buf));
    if (thresholdType == NVML_TEMPERATURE_THRESHOLD_SHUTDOWN) {
        buf[0] = 0x16U;
    } else if (thresholdType == NVML_TEMPERATURE_THRESHOLD_SLOWDOWN) {
        buf[0] = 0x0dU;
    } else {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_THERMAL_GET_THRESHOLD, buf, sizeof(buf)) != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *temp = (buf[1] >> 8) & 0xffU;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetFanSpeed(nvmlDevice_t device, unsigned int *speed) {
    if (!valid_device(device) || !speed) return NVML_ERROR_INVALID_ARGUMENT;
    return NVML_ERROR_NOT_SUPPORTED; /* Passive datacenter Tesla P4 */
}

nvmlReturn_t nvmlDeviceGetPowerUsage(nvmlDevice_t device, unsigned int *power) {
    if (!valid_device(device) || !power) return NVML_ERROR_INVALID_ARGUMENT;
    uint32_t buf[773]; /* 3092 bytes */
    memset(buf, 0, sizeof(buf));
    buf[1] = 0x0c9fU;
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_PMGR_PWR_MONITOR_GET_STATUS, buf, sizeof(buf)) != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *power = buf[2]; /* milliwatts */
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetPowerManagementLimit(nvmlDevice_t device, unsigned int *limit) {
    if (!valid_device(device) || !limit) return NVML_ERROR_INVALID_ARGUMENT;
    uint32_t buf[3]; /* 12 bytes */
    memset(buf, 0, sizeof(buf));
    buf[0] = 0xfe01U;
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_PMGR_PWR_POLICY_GET_CONTROL, buf, sizeof(buf)) != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *limit = buf[1];
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetEnforcedPowerLimit(nvmlDevice_t device, unsigned int *limit) {
    if (!valid_device(device) || !limit) return NVML_ERROR_INVALID_ARGUMENT;
    uint32_t buf[2]; /* 8 bytes */
    memset(buf, 0, sizeof(buf));
    buf[0] = 1U;
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_PMGR_PWR_POLICY_IDX_GET_LIMIT, buf, sizeof(buf)) != 0) {
        return nvmlDeviceGetPowerManagementLimit(device, limit);
    }
    *limit = buf[1];
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetPowerManagementDefaultLimit(nvmlDevice_t device, unsigned int *defaultLimit) {
    if (!valid_device(device) || !defaultLimit) return NVML_ERROR_INVALID_ARGUMENT;
    uint32_t buf[1736]; /* 6944 bytes */
    memset(buf, 0, sizeof(buf));
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_PMGR_PWR_POLICY_GET_INFO, buf, sizeof(buf)) != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *defaultLimit = buf[0x1b8 / 4];
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetPowerManagementLimitConstraints(nvmlDevice_t device, unsigned int *minLimit, unsigned int *maxLimit) {
    if (!valid_device(device) || !minLimit || !maxLimit) return NVML_ERROR_INVALID_ARGUMENT;
    uint32_t buf[1736]; /* 6944 bytes */
    memset(buf, 0, sizeof(buf));
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_PMGR_PWR_POLICY_GET_INFO, buf, sizeof(buf)) != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *minLimit = buf[0x1b4 / 4];
    *maxLimit = buf[0x1bc / 4];
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetPerformanceState(nvmlDevice_t device, nvmlPstates_t *pState) {
    if (!valid_device(device) || !pState) return NVML_ERROR_INVALID_ARGUMENT;
    uint32_t mask = 0;
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE, &mask, sizeof(mask)) != 0 || mask == 0) {
        *pState = NVML_PSTATE_UNKNOWN;
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *pState = (nvmlPstates_t)__builtin_ctz(mask);
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetCurrentClocksThrottleReasons(nvmlDevice_t device, unsigned long long *clocksThrottleReasons) {
    if (!valid_device(device) || !clocksThrottleReasons) return NVML_ERROR_INVALID_ARGUMENT;
    *clocksThrottleReasons = 0ULL;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetSupportedClocksThrottleReasons(nvmlDevice_t device, unsigned long long *supportedClocksThrottleReasons) {
    if (!valid_device(device) || !supportedClocksThrottleReasons) return NVML_ERROR_INVALID_ARGUMENT;
    *supportedClocksThrottleReasons = 0x1ffULL;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetClockInfo(nvmlDevice_t device, nvmlClockType_t type, unsigned int *clock) {
    if (!valid_device(device) || !clock) return NVML_ERROR_INVALID_ARGUMENT;
    uint32_t domain = 0;
    uint32_t div = 1000U;
    switch (type) {
        case NVML_CLOCK_GRAPHICS:
        case NVML_CLOCK_SM:
            domain = 0x1000U;
            div = 2000U; /* GP104 2x shader clock in kHz */
            break;
        case NVML_CLOCK_MEM:
            domain = 0x0010U;
            div = 1000U;
            break;
        case NVML_CLOCK_VIDEO:
            domain = 0x8000U;
            div = 2000U;
            break;
        default:
            return NVML_ERROR_INVALID_ARGUMENT;
    }
    uint32_t buf[386]; /* 1544 bytes */
    memset(buf, 0, sizeof(buf));
    buf[1] = 1U;
    buf[3] = domain;
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_CLK_GET_INFO, buf, sizeof(buf)) != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *clock = buf[4] / div;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetMaxClockInfo(nvmlDevice_t device, nvmlClockType_t type, unsigned int *clock) {
    if (!valid_device(device) || !clock) return NVML_ERROR_INVALID_ARGUMENT;
    switch (type) {
        case NVML_CLOCK_GRAPHICS:
        case NVML_CLOCK_SM:
            *clock = 1531U;
            return NVML_SUCCESS;
        case NVML_CLOCK_MEM:
            *clock = 3003U;
            return NVML_SUCCESS;
        case NVML_CLOCK_VIDEO:
            *clock = 1379U;
            return NVML_SUCCESS;
        default:
            return NVML_ERROR_INVALID_ARGUMENT;
    }
}

nvmlReturn_t nvmlDeviceGetClock(nvmlDevice_t device, nvmlClockType_t clockType, nvmlClockId_t clockId, unsigned int *clockMHz) {
    if (!valid_device(device) || !clockMHz) return NVML_ERROR_INVALID_ARGUMENT;
    if (clockId == NVML_CLOCK_ID_CURRENT) {
        return nvmlDeviceGetClockInfo(device, clockType, clockMHz);
    }
    if (clockId == NVML_CLOCK_ID_APP_CLOCK_TARGET || clockId == NVML_CLOCK_ID_APP_CLOCK_DEFAULT) {
        if (clockType == NVML_CLOCK_GRAPHICS || clockType == NVML_CLOCK_SM) {
            *clockMHz = 885U;
            return NVML_SUCCESS;
        }
        if (clockType == NVML_CLOCK_MEM) {
            *clockMHz = 3003U;
            return NVML_SUCCESS;
        }
    }
    if (clockId == NVML_CLOCK_ID_CUSTOMER_BOOST_MAX && (clockType == NVML_CLOCK_GRAPHICS || clockType == NVML_CLOCK_SM)) {
        *clockMHz = 1113U;
        return NVML_SUCCESS;
    }
    return nvmlDeviceGetMaxClockInfo(device, clockType, clockMHz);
}

nvmlReturn_t nvmlDeviceGetApplicationsClock(nvmlDevice_t device, nvmlClockType_t clockType, unsigned int *clockMHz) {
    return nvmlDeviceGetClock(device, clockType, NVML_CLOCK_ID_APP_CLOCK_TARGET, clockMHz);
}

nvmlReturn_t nvmlDeviceGetDefaultApplicationsClock(nvmlDevice_t device, nvmlClockType_t clockType, unsigned int *clockMHz) {
    return nvmlDeviceGetClock(device, clockType, NVML_CLOCK_ID_APP_CLOCK_DEFAULT, clockMHz);
}

nvmlReturn_t nvmlDeviceGetAutoBoostedClocksEnabled(nvmlDevice_t device, nvmlEnableState_t *isEnabled, nvmlEnableState_t *defaultIsEnabled) {
    (void)device; (void)isEnabled; (void)defaultIsEnabled;
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetUtilizationRates(nvmlDevice_t device, nvmlUtilization_t *utilization) {
    if (!valid_device(device) || !utilization) return NVML_ERROR_INVALID_ARGUMENT;
    uint32_t buf[292]; /* 1168 bytes */
    memset(buf, 0, sizeof(buf));
    buf[0] = 2U;
    buf[1] = 0x1000U; /* GPU domain */
    buf[5] = 0x0010U; /* Memory domain */
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_PERF_GET_CLK_DOMAINS_SAMPLE, buf, sizeof(buf)) != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    utilization->gpu    = buf[2];
    utilization->memory = buf[6];
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetEncoderUtilization(nvmlDevice_t device, unsigned int *utilization, unsigned int *samplingPeriodUs) {
    if (!valid_device(device) || !utilization || !samplingPeriodUs) return NVML_ERROR_INVALID_ARGUMENT;
    uint32_t buf[3] = { 1U, 0U, 0U };
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_PERF_GET_VID_ENG_UTIL, buf, sizeof(buf)) != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *utilization      = buf[1];
    *samplingPeriodUs = buf[2];
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetDecoderUtilization(nvmlDevice_t device, unsigned int *utilization, unsigned int *samplingPeriodUs) {
    if (!valid_device(device) || !utilization || !samplingPeriodUs) return NVML_ERROR_INVALID_ARGUMENT;
    uint32_t buf[3] = { 2U, 0U, 0U };
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_PERF_GET_VID_ENG_UTIL, buf, sizeof(buf)) != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    *utilization      = buf[1];
    *samplingPeriodUs = buf[2];
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetJpgUtilization(nvmlDevice_t device, unsigned int *utilization, unsigned int *samplingPeriodUs) {
    (void)device; (void)utilization; (void)samplingPeriodUs;
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetOfaUtilization(nvmlDevice_t device, unsigned int *utilization, unsigned int *samplingPeriodUs) {
    (void)device; (void)utilization; (void)samplingPeriodUs;
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlDeviceGetPersistenceMode(nvmlDevice_t device, nvmlEnableState_t *mode) {
    if (!valid_device(device) || !mode) return NVML_ERROR_INVALID_ARGUMENT;
    uint32_t val = 0;
    if (rm_control(device->h_client, device->h_device, NV0080_CTRL_CMD_GPU_GET_PERSISTENCE_STATE, &val, sizeof(val)) == 0) {
        /* Note: on vGPU host where open-nvidia-vgpud holds device open, report ENABLED */
        *mode = NVML_FEATURE_ENABLED;
        return NVML_SUCCESS;
    }
    *mode = NVML_FEATURE_ENABLED;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetComputeMode(nvmlDevice_t device, nvmlComputeMode_t *mode) {
    if (!valid_device(device) || !mode) return NVML_ERROR_INVALID_ARGUMENT;
    *mode = NVML_COMPUTEMODE_DEFAULT;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetDisplayActive(nvmlDevice_t device, nvmlEnableState_t *isActive) {
    if (!valid_device(device) || !isActive) return NVML_ERROR_INVALID_ARGUMENT;
    *isActive = NVML_FEATURE_DISABLED;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetDisplayMode(nvmlDevice_t device, nvmlEnableState_t *display) {
    if (!valid_device(device) || !display) return NVML_ERROR_INVALID_ARGUMENT;
    *display = NVML_FEATURE_DISABLED;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetEccMode(nvmlDevice_t device, nvmlEnableState_t *current, nvmlEnableState_t *pending) {
    if (!valid_device(device) || !current || !pending) return NVML_ERROR_INVALID_ARGUMENT;
    *current = NVML_FEATURE_DISABLED;
    *pending = NVML_FEATURE_DISABLED;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetMigMode(nvmlDevice_t device, unsigned int *currentMode, unsigned int *pendingMode) {
    (void)device; (void)currentMode; (void)pendingMode;
    return NVML_ERROR_NOT_SUPPORTED;
}

static nvmlReturn_t query_running_processes(nvmlDevice_t device, unsigned int *infoCount, nvmlProcessInfo_v3_t *infos) {
    if (!valid_device(device) || !infoCount) return NVML_ERROR_INVALID_ARGUMENT;
    unsigned int cap = *infoCount;
    unsigned int count = 0;

    uint32_t *pbuf = (uint32_t *)calloc(953, sizeof(uint32_t)); /* 3812 bytes */
    if (!pbuf) return NVML_ERROR_MEMORY;
    pbuf[0] = 1U;
    pbuf[1] = 0xa082U;
    if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_GPU_GET_ACTIVE_PARTITION_IDS, pbuf, 3812U) == 0 && pbuf[2] > 0) {
        uint32_t npids = pbuf[2] > 200U ? 200U : pbuf[2];
        uint8_t *mbuf = (uint8_t *)calloc(1, 14408U); /* 0x3848 bytes */
        if (mbuf) {
            *(uint32_t *)&mbuf[0] = npids;
            for (uint32_t i = 0; i < npids; i++) {
                *(uint32_t *)&mbuf[8 + i * 0x48] = pbuf[3 + i];
            }
            if (rm_control(device->h_client, device->h_subdevice, NV2080_CTRL_CMD_GPU_GET_PID_INFO, mbuf, 14408U) == 0) {
                for (uint32_t i = 0; i < npids; i++) {
                    uint8_t *e = &mbuf[8 + i * 0x48];
                    if (*(uint32_t *)&e[8] != 0) continue;
                    if (infos && count < cap) {
                        infos[count].pid = *(uint32_t *)&e[0];
                        infos[count].usedGpuMemory = *(uint64_t *)&e[0x10] + *(uint64_t *)&e[0x18];
                        infos[count].gpuInstanceId = 0xffffffffU;
                        infos[count].computeInstanceId = 0xffffffffU;
                        infos[count].usedGpuCcProtectedMemory = 0ULL;
                    }
                    count++;
                }
            }
            free(mbuf);
        }
    }
    free(pbuf);

    /* Also check active vGPU instances (0xa0810102) so vGPU workers are always listed */
    uint8_t *vbuf = (uint8_t *)calloc(1, 10760U);
    if (vbuf) {
        if (rm_control(device->h_client, device->h_vgpu_cfg, NVA081_CTRL_CMD_VGPU_CONFIG_GET_VGPU_INSTANCES, vbuf, 10760U) == 0) {
            uint32_t nv = *(uint32_t *)&vbuf[4];
            if (nv > 32U) nv = 32U;
            for (uint32_t i = 0; i < nv; i++) {
                uint8_t *e = &vbuf[8 + i * 0x150];
                uint32_t pid = *(uint32_t *)&e[0x00];
                uint32_t vtype = *(uint32_t *)&e[0x100];
                uint64_t fb = *(uint64_t *)&e[0x138];
                if (fb == 0 && vtype != 0) {
                    unsigned long long type_fb = 0;
                    if (nvmlVgpuTypeGetFramebufferSize(vtype, &type_fb) == NVML_SUCCESS && type_fb > 0) {
                        fb = type_fb;
                    }
                }
                if (pid == 0) continue;
                int dup = 0;
                if (infos) {
                    unsigned int check_n = count < cap ? count : cap;
                    for (unsigned int j = 0; j < check_n; j++) {
                        if (infos[j].pid == pid) { dup = 1; break; }
                    }
                }
                if (!dup) {
                    if (infos && count < cap) {
                        infos[count].pid = pid;
                        infos[count].usedGpuMemory = fb;
                        infos[count].gpuInstanceId = 0xffffffffU;
                        infos[count].computeInstanceId = 0xffffffffU;
                        infos[count].usedGpuCcProtectedMemory = 0ULL;
                    }
                    count++;
                }
            }
        }
        free(vbuf);
    }

    *infoCount = count;
    if (count > cap) return NVML_ERROR_INSUFFICIENT_SIZE;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetComputeRunningProcesses_v3(nvmlDevice_t device, unsigned int *infoCount, nvmlProcessInfo_v3_t *infos) {
    return query_running_processes(device, infoCount, infos);
}

nvmlReturn_t nvmlDeviceGetComputeRunningProcesses_v2(nvmlDevice_t device, unsigned int *infoCount, nvmlProcessInfo_v2_t *infos) {
    if (!infoCount) return NVML_ERROR_INVALID_ARGUMENT;
    unsigned int cap = *infoCount;
    nvmlProcessInfo_v3_t tmp[64];
    unsigned int tmp_cap = cap < 64U ? cap : 64U;
    nvmlReturn_t rc = query_running_processes(device, &tmp_cap, infos ? tmp : NULL);
    *infoCount = tmp_cap;
    if (rc == NVML_SUCCESS && infos) {
        for (unsigned int i = 0; i < tmp_cap; i++) {
            infos[i].pid = tmp[i].pid;
            infos[i].usedGpuMemory = tmp[i].usedGpuMemory;
            infos[i].gpuInstanceId = tmp[i].gpuInstanceId;
            infos[i].computeInstanceId = tmp[i].computeInstanceId;
        }
    }
    return rc;
}

nvmlReturn_t nvmlDeviceGetComputeRunningProcesses(nvmlDevice_t device, unsigned int *infoCount, nvmlProcessInfo_v1_t *infos) {
    if (!infoCount) return NVML_ERROR_INVALID_ARGUMENT;
    unsigned int cap = *infoCount;
    nvmlProcessInfo_v3_t tmp[64];
    unsigned int tmp_cap = cap < 64U ? cap : 64U;
    nvmlReturn_t rc = query_running_processes(device, &tmp_cap, infos ? tmp : NULL);
    *infoCount = tmp_cap;
    if (rc == NVML_SUCCESS && infos) {
        for (unsigned int i = 0; i < tmp_cap; i++) {
            infos[i].pid = tmp[i].pid;
            infos[i].usedGpuMemory = tmp[i].usedGpuMemory;
        }
    }
    return rc;
}

nvmlReturn_t nvmlDeviceGetGraphicsRunningProcesses_v3(nvmlDevice_t device, unsigned int *infoCount, nvmlProcessInfo_v3_t *infos) {
    (void)infos;
    if (!valid_device(device) || !infoCount) return NVML_ERROR_INVALID_ARGUMENT;
    *infoCount = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetGraphicsRunningProcesses_v2(nvmlDevice_t device, unsigned int *infoCount, nvmlProcessInfo_v2_t *infos) {
    (void)infos;
    if (!valid_device(device) || !infoCount) return NVML_ERROR_INVALID_ARGUMENT;
    *infoCount = 0;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetGraphicsRunningProcesses(nvmlDevice_t device, unsigned int *infoCount, nvmlProcessInfo_v1_t *infos) {
    (void)infos;
    if (!valid_device(device) || !infoCount) return NVML_ERROR_INVALID_ARGUMENT;
    *infoCount = 0;
    return NVML_SUCCESS;
}

/* Host vGPU APIs */
nvmlReturn_t nvmlGetVgpuDriverCapabilities(unsigned int capability, unsigned int *capResult) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!capResult) return NVML_ERROR_INVALID_ARGUMENT;
    *capResult = (capability == 0U) ? 1U : 0U;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetVgpuCapabilities(nvmlDevice_t device, unsigned int capability, unsigned int *capResult) {
    if (!valid_device(device) || !capResult) return NVML_ERROR_INVALID_ARGUMENT;
    (void)capability;
    *capResult = 1U;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetHostVgpuMode(nvmlDevice_t device, nvmlHostVgpuMode_t *pHostVgpuMode) {
    if (!valid_device(device) || !pHostVgpuMode) return NVML_ERROR_INVALID_ARGUMENT;
    *pHostVgpuMode = NVML_HOST_VGPU_MODE_NON_SRIOV;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetVirtualizationMode(nvmlDevice_t device, nvmlGpuVirtualizationMode_t *pVirtualMode) {
    if (!valid_device(device) || !pVirtualMode) return NVML_ERROR_INVALID_ARGUMENT;
    uint32_t mode_r580[2] = { NVML_GPU_VIRTUALIZATION_MODE_HOST_VGPU, 0U };
    rm_control(device->h_client, device->h_device, NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE, mode_r580, sizeof(mode_r580));
    *pVirtualMode = (nvmlGpuVirtualizationMode_t)mode_r580[0];
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetSupportedVgpus(nvmlDevice_t device, unsigned int *vgpuCount, nvmlVgpuTypeId_t *vgpuTypeIds) {
    if (!valid_device(device) || !vgpuCount) return NVML_ERROR_INVALID_ARGUMENT;
    uint32_t buf[131]; /* R580: 0x20c bytes */
    memset(buf, 0, sizeof(buf));
    if (rm_control(device->h_client, device->h_vgpu_cfg, NVA081_CTRL_CMD_VGPU_CONFIG_GET_SUPPORTED_VGPU_TYPES, buf, sizeof(buf)) != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    uint32_t count = buf[2];
    if (count > 128U) count = 128U;
    unsigned int cap = *vgpuCount;
    *vgpuCount = count;
    if (!vgpuTypeIds || cap < count) return NVML_ERROR_INSUFFICIENT_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        vgpuTypeIds[i] = buf[3 + i];
    }
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetCreatableVgpus(nvmlDevice_t device, unsigned int *vgpuCount, nvmlVgpuTypeId_t *vgpuTypeIds) {
    if (!valid_device(device) || !vgpuCount) return NVML_ERROR_INVALID_ARGUMENT;
    uint32_t buf[131]; /* R580: 0x20c bytes */
    memset(buf, 0, sizeof(buf));
    if (rm_control(device->h_client, device->h_vgpu_cfg, NVA081_CTRL_CMD_VGPU_CONFIG_GET_CREATABLE_VGPU_TYPES, buf, sizeof(buf)) != 0) {
        return NVML_ERROR_NOT_SUPPORTED;
    }
    uint32_t count = buf[2];
    if (count > 128U) count = 128U;
    unsigned int cap = *vgpuCount;
    *vgpuCount = count;
    if (count == 0) return NVML_SUCCESS;
    if (!vgpuTypeIds || cap < count) return NVML_ERROR_INSUFFICIENT_SIZE;
    for (uint32_t i = 0; i < count; i++) {
        vgpuTypeIds[i] = buf[3 + i];
    }
    return NVML_SUCCESS;
}

static int query_vgpu_type_info(nvmlVgpuTypeId_t vgpuTypeId, uint8_t *out_5432) {
    if (!g_nvml_init_count || g_device_count == 0) return -1;
    memset(out_5432, 0, 0x1538U);
    *(uint32_t *)&out_5432[0] = vgpuTypeId;
    return rm_control(g_devices[0].h_client, g_devices[0].h_vgpu_cfg,
                      NVA081_CTRL_CMD_VGPU_CONFIG_GET_VGPU_TYPE_INFO, out_5432, 0x1538U);
}

nvmlReturn_t nvmlVgpuTypeGetName(nvmlVgpuTypeId_t vgpuTypeId, char *vgpuTypeName, unsigned int *size) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!size) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t buf[0x1538];
    if (query_vgpu_type_info(vgpuTypeId, buf) != 0) return NVML_ERROR_NOT_FOUND;
    const char *s = (const char *)&buf[0x0c];
    size_t len = strnlen(s, 63);
    if (!vgpuTypeName || *size <= len) {
        *size = (unsigned int)(len + 1);
        return NVML_ERROR_INSUFFICIENT_SIZE;
    }
    memcpy(vgpuTypeName, s, len);
    vgpuTypeName[len] = '\0';
    *size = (unsigned int)(len + 1);
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuTypeGetClass(nvmlVgpuTypeId_t vgpuTypeId, char *vgpuTypeClass, unsigned int *size) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!size) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t buf[0x1538];
    if (query_vgpu_type_info(vgpuTypeId, buf) != 0) return NVML_ERROR_NOT_FOUND;
    const char *s = (const char *)&buf[0x4c];
    size_t len = strnlen(s, 63);
    if (!vgpuTypeClass || *size <= len) {
        *size = (unsigned int)(len + 1);
        return NVML_ERROR_INSUFFICIENT_SIZE;
    }
    memcpy(vgpuTypeClass, s, len);
    vgpuTypeClass[len] = '\0';
    *size = (unsigned int)(len + 1);
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuTypeGetDeviceID(nvmlVgpuTypeId_t vgpuTypeId, unsigned long long *deviceID, unsigned long long *subsystemID) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!deviceID || !subsystemID) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t buf[0x1538];
    if (query_vgpu_type_info(vgpuTypeId, buf) != 0) return NVML_ERROR_NOT_FOUND;
    *deviceID = 0x1bb310deULL;
    *subsystemID = 0x11d810deULL;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuTypeGetFramebufferSize(nvmlVgpuTypeId_t vgpuTypeId, unsigned long long *fbSize) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!fbSize) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t buf[0x1538];
    if (query_vgpu_type_info(vgpuTypeId, buf) != 0) return NVML_ERROR_NOT_FOUND;
    *fbSize = *(uint64_t *)&buf[0x1c8];
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuTypeGetNumDisplayHeads(nvmlVgpuTypeId_t vgpuTypeId, unsigned int *numDisplayHeads) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!numDisplayHeads) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t buf[0x1538];
    if (query_vgpu_type_info(vgpuTypeId, buf) != 0) return NVML_ERROR_NOT_FOUND;
    *numDisplayHeads = *(uint32_t *)&buf[0x190];
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuTypeGetResolution(nvmlVgpuTypeId_t vgpuTypeId, unsigned int displayIndex, unsigned int *xdim, unsigned int *ydim) {
    (void)displayIndex;
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!xdim || !ydim) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t buf[0x1538];
    if (query_vgpu_type_info(vgpuTypeId, buf) != 0) return NVML_ERROR_NOT_FOUND;
    *xdim = *(uint32_t *)&buf[0x194];
    *ydim = *(uint32_t *)&buf[0x198];
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuTypeGetLicense(nvmlVgpuTypeId_t vgpuTypeId, char *vgpuTypeLicenseString, unsigned int size) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!vgpuTypeLicenseString || size == 0) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t buf[0x1538];
    if (query_vgpu_type_info(vgpuTypeId, buf) != 0) return NVML_ERROR_NOT_FOUND;
    const char *s = (const char *)&buf[0x10c];
    if (!s[0]) s = "GRID-Virtual-WS,2.0";
    snprintf(vgpuTypeLicenseString, size, "%s", s);
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuTypeGetFrameRateLimit(nvmlVgpuTypeId_t vgpuTypeId, unsigned int *frameRateLimit) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!frameRateLimit) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t buf[0x1538];
    if (query_vgpu_type_info(vgpuTypeId, buf) != 0) return NVML_ERROR_NOT_FOUND;
    *frameRateLimit = *(uint32_t *)&buf[0x1a0];
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuTypeGetMaxInstances(nvmlDevice_t device, nvmlVgpuTypeId_t vgpuTypeId, unsigned int *vgpuInstanceCount) {
    if (!valid_device(device) || !vgpuInstanceCount) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t buf[0x1538];
    if (query_vgpu_type_info(vgpuTypeId, buf) != 0) return NVML_ERROR_NOT_FOUND;
    *vgpuInstanceCount = *(uint32_t *)&buf[0x18c];
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuTypeGetMaxInstancesPerVm(nvmlVgpuTypeId_t vgpuTypeId, unsigned int *vgpuInstanceCountPerVm) {
    (void)vgpuTypeId;
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!vgpuInstanceCountPerVm) return NVML_ERROR_INVALID_ARGUMENT;
    *vgpuInstanceCountPerVm = 1U;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuTypeGetGpuInstanceProfileId(nvmlVgpuTypeId_t vgpuTypeId, unsigned int *gpuInstanceProfileId) {
    (void)vgpuTypeId;
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!gpuInstanceProfileId) return NVML_ERROR_INVALID_ARGUMENT;
    *gpuInstanceProfileId = 0xffffffffU;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuTypeGetCapabilities(nvmlVgpuTypeId_t vgpuTypeId, unsigned int capability, unsigned int *capResult) {
    (void)vgpuTypeId; (void)capability;
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!capResult) return NVML_ERROR_INVALID_ARGUMENT;
    *capResult = 0U;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetActiveVgpus(nvmlDevice_t device, unsigned int *vgpuCount, nvmlVgpuInstance_t *vgpuInstances) {
    if (!valid_device(device) || !vgpuCount) return NVML_ERROR_INVALID_ARGUMENT;
    unsigned int cap = *vgpuCount;
    *vgpuCount = 0;
    uint8_t *buf = (uint8_t *)calloc(1, 10760U);
    if (!buf) return NVML_ERROR_MEMORY;
    if (rm_control(device->h_client, device->h_vgpu_cfg, NVA081_CTRL_CMD_VGPU_CONFIG_GET_VGPU_INSTANCES, buf, 10760U) != 0) {
        free(buf);
        return NVML_ERROR_NOT_SUPPORTED;
    }
    uint32_t count = *(uint32_t *)&buf[4];
    if (count > 32U) count = 32U;
    *vgpuCount = count;
    if (count == 0) {
        free(buf);
        return NVML_SUCCESS;
    }
    if (!vgpuInstances || cap < count) {
        free(buf);
        return NVML_ERROR_INSUFFICIENT_SIZE;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *e = &buf[8 + i * 0x150];
        vgpuInstances[i] = *(uint32_t *)&e[0x00];
    }
    free(buf);
    return NVML_SUCCESS;
}

static int find_vgpu_instance_entry(nvmlVgpuInstance_t vgpuInstance, uint8_t *out_entry_336) {
    if (!g_nvml_init_count || g_device_count == 0) return -1;
    uint8_t *buf = (uint8_t *)calloc(1, 10760U);
    if (!buf) return -1;
    for (uint32_t d = 0; d < g_device_count; d++) {
        memset(buf, 0, 10760U);
        if (rm_control(g_devices[d].h_client, g_devices[d].h_vgpu_cfg,
                       NVA081_CTRL_CMD_VGPU_CONFIG_GET_VGPU_INSTANCES, buf, 10760U) == 0) {
            uint32_t count = *(uint32_t *)&buf[4];
            if (count > 32U) count = 32U;
            for (uint32_t i = 0; i < count; i++) {
                uint8_t *e = &buf[8 + i * 0x150];
                if (*(uint32_t *)&e[0] == vgpuInstance) {
                    memcpy(out_entry_336, e, 0x150);
                    free(buf);
                    return 0;
                }
            }
        }
    }
    free(buf);
    return -1;
}

static int is_printable_ascii_str(const uint8_t *buf, size_t max_len) {
    if (!buf || buf[0] < 0x20 || buf[0] > 0x7e) return 0;
    size_t i = 0;
    for (; i < max_len && buf[i] != 0; i++) {
        if (buf[i] < 0x20 || buf[i] > 0x7e) return 0;
    }
    return (i >= 2);
}

static void format_uuid_or_ascii(const uint8_t *raw16, char *out, unsigned int size) {
    if (is_printable_ascii_str(raw16, 16)) {
        snprintf(out, size, "%.16s", (const char *)raw16);
        return;
    }
    snprintf(out, size,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             raw16[0], raw16[1], raw16[2], raw16[3],
             raw16[4], raw16[5], raw16[6], raw16[7],
             raw16[8], raw16[9], raw16[10], raw16[11],
             raw16[12], raw16[13], raw16[14], raw16[15]);
}

nvmlReturn_t nvmlVgpuInstanceGetVmID(nvmlVgpuInstance_t vgpuInstance, char *vmId, unsigned int size, nvmlVgpuVmIdType_t *vmIdType) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!vmId || !vmIdType || size == 0) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t e[0x150];
    if (find_vgpu_instance_entry(vgpuInstance, e) != 0) return NVML_ERROR_NOT_FOUND;
    uint32_t t = *(uint32_t *)&e[0x48];
    *vmIdType = (t == 0) ? NVML_VGPU_VM_ID_DOMAIN_ID : NVML_VGPU_VM_ID_UUID;
    if (is_printable_ascii_str(&e[0x78], 63)) {
        snprintf(vmId, size, "%.63s", (const char *)&e[0x78]);
    } else {
        format_uuid_or_ascii(&e[0x28], vmId, size);
    }
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuInstanceGetUUID(nvmlVgpuInstance_t vgpuInstance, char *uuid, unsigned int size) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!uuid || size == 0) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t e[0x150];
    if (find_vgpu_instance_entry(vgpuInstance, e) != 0) return NVML_ERROR_NOT_FOUND;
    format_uuid_or_ascii(&e[0x38], uuid, size);
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuInstanceGetMdevUUID(nvmlVgpuInstance_t vgpuInstance, char *mdevUuid, unsigned int size) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!mdevUuid || size == 0) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t e[0x150];
    if (find_vgpu_instance_entry(vgpuInstance, e) != 0) return NVML_ERROR_NOT_FOUND;
    format_uuid_or_ascii(&e[0x120], mdevUuid, size);
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuInstanceGetVmDriverVersion(nvmlVgpuInstance_t vgpuInstance, char *version, unsigned int length) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!version || length == 0) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t e[0x150];
    if (find_vgpu_instance_entry(vgpuInstance, e) != 0) return NVML_ERROR_NOT_FOUND;
    if (is_printable_ascii_str(&e[0x110], 63)) {
        snprintf(version, length, "%.63s", (const char *)&e[0x110]);
    } else {
        snprintf(version, length, "N/A");
    }
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuInstanceGetFbUsage(nvmlVgpuInstance_t vgpuInstance, unsigned long long *fbUsage) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!fbUsage) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t e[0x150];
    if (find_vgpu_instance_entry(vgpuInstance, e) != 0) return NVML_ERROR_NOT_FOUND;
    uint64_t fb = *(uint64_t *)&e[0x138];
    if (fb == 0) {
        uint32_t vtype = *(uint32_t *)&e[0x100];
        if (vtype != 0) {
            unsigned long long type_fb = 0;
            if (nvmlVgpuTypeGetFramebufferSize(vtype, &type_fb) == NVML_SUCCESS) {
                fb = type_fb;
            }
        }
    }
    *fbUsage = fb;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuInstanceGetLicenseStatus(nvmlVgpuInstance_t vgpuInstance, unsigned int *licensed) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!licensed) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t e[0x150];
    if (find_vgpu_instance_entry(vgpuInstance, e) != 0) return NVML_ERROR_NOT_FOUND;
    *licensed = e[0x18] ? 1U : 0U;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuInstanceGetLicenseInfo_v2(nvmlVgpuInstance_t vgpuInstance, nvmlVgpuLicenseInfo_t *licenseInfo) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!licenseInfo) return NVML_ERROR_INVALID_ARGUMENT;
    unsigned int lic = 0;
    nvmlReturn_t rc = nvmlVgpuInstanceGetLicenseStatus(vgpuInstance, &lic);
    if (rc != NVML_SUCCESS) return rc;
    memset(licenseInfo, 0, sizeof(*licenseInfo));
    licenseInfo->isLicensed = (uint8_t)lic;
    licenseInfo->currentState = lic ? 3U : 0U;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuInstanceGetLicenseInfo(nvmlVgpuInstance_t vgpuInstance, nvmlVgpuLicenseInfo_t *licenseInfo) {
    return nvmlVgpuInstanceGetLicenseInfo_v2(vgpuInstance, licenseInfo);
}

nvmlReturn_t nvmlVgpuInstanceGetType(nvmlVgpuInstance_t vgpuInstance, nvmlVgpuTypeId_t *vgpuTypeId) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!vgpuTypeId) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t e[0x150];
    if (find_vgpu_instance_entry(vgpuInstance, e) != 0) return NVML_ERROR_NOT_FOUND;
    *vgpuTypeId = *(uint32_t *)&e[0x100];
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuInstanceGetFrameRateLimit(nvmlVgpuInstance_t vgpuInstance, unsigned int *frameRateLimit) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!frameRateLimit) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t e[0x150];
    if (find_vgpu_instance_entry(vgpuInstance, e) != 0) return NVML_ERROR_NOT_FOUND;
    *frameRateLimit = *(uint32_t *)&e[0x0c];
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuInstanceGetEccMode(nvmlVgpuInstance_t vgpuInstance, nvmlEnableState_t *eccMode) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!eccMode) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t e[0x150];
    if (find_vgpu_instance_entry(vgpuInstance, e) != 0) return NVML_ERROR_NOT_FOUND;
    *eccMode = (*(uint32_t *)&e[0x10]) ? NVML_FEATURE_ENABLED : NVML_FEATURE_DISABLED;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuInstanceGetEncoderCapacity(nvmlVgpuInstance_t vgpuInstance, unsigned int *encoderCapacity) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!encoderCapacity) return NVML_ERROR_INVALID_ARGUMENT;
    uint8_t e[0x150];
    if (find_vgpu_instance_entry(vgpuInstance, e) != 0) return NVML_ERROR_NOT_FOUND;
    *encoderCapacity = *(uint32_t *)&e[0x14];
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuInstanceGetGpuInstanceId(nvmlVgpuInstance_t vgpuInstance, unsigned int *gpuInstanceId) {
    (void)vgpuInstance;
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!gpuInstanceId) return NVML_ERROR_INVALID_ARGUMENT;
    *gpuInstanceId = 0xffffffffU;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlVgpuInstanceGetGpuPciId(nvmlVgpuInstance_t vgpuInstance, char *vgpuPciId, unsigned int *length) {
    (void)vgpuInstance;
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!vgpuPciId || !length || *length < 17U) return NVML_ERROR_INVALID_ARGUMENT;
    snprintf(vgpuPciId, *length, "%08X:%02X:%02X.0",
             g_devices[0].domain, g_devices[0].bus, g_devices[0].device);
    return NVML_SUCCESS;
}

/* GSP & XID Events (compatibility with open-nvidia-xid-logd) */
nvmlReturn_t nvmlDeviceGetGspFirmwareMode(nvmlDevice_t device, unsigned int *isEnabled, unsigned int *defaultMode) {
    if (!valid_device(device)) return NVML_ERROR_INVALID_ARGUMENT;
    if (isEnabled) *isEnabled = 0U;   /* Pascal GP104 is non-GSP */
    if (defaultMode) *defaultMode = 0U;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetGspFirmwareVersion(nvmlDevice_t device, char *version) {
    if (!valid_device(device) || !version) return NVML_ERROR_INVALID_ARGUMENT;
    version[0] = '\0';
    return NVML_ERROR_NOT_SUPPORTED;
}

nvmlReturn_t nvmlEventSetCreate(nvmlEventSet_t *set) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!set) return NVML_ERROR_INVALID_ARGUMENT;
    struct nvmlEventSet_st *s = (struct nvmlEventSet_st *)calloc(1, sizeof(*s));
    if (!s) return NVML_ERROR_MEMORY;
    s->magic = 0x4e564556U;
    *set = s;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceRegisterEvents(nvmlDevice_t device, unsigned long long eventTypes, nvmlEventSet_t set) {
    if (!valid_device(device) || !set) return NVML_ERROR_INVALID_ARGUMENT;
    set->registered_events |= eventTypes;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlDeviceGetSupportedEventTypes(nvmlDevice_t device, unsigned long long *eventTypes) {
    if (!valid_device(device) || !eventTypes) return NVML_ERROR_INVALID_ARGUMENT;
    *eventTypes = 0xffULL;
    return NVML_SUCCESS;
}

nvmlReturn_t nvmlEventSetWait_v2(nvmlEventSet_t set, nvmlEventData_t *data, unsigned int timeoutms) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!set || !data) return NVML_ERROR_INVALID_ARGUMENT;
    if (timeoutms > 0) usleep((timeoutms > 50U ? 50U : timeoutms) * 1000U);
    return NVML_ERROR_TIMEOUT;
}

nvmlReturn_t nvmlEventSetWait(nvmlEventSet_t set, nvmlEventData_t *data, unsigned int timeoutms) {
    return nvmlEventSetWait_v2(set, data, timeoutms);
}

nvmlReturn_t nvmlEventSetFree(nvmlEventSet_t set) {
    if (!g_nvml_init_count) return NVML_ERROR_UNINITIALIZED;
    if (!set) return NVML_ERROR_INVALID_ARGUMENT;
    free(set);
    return NVML_SUCCESS;
}
