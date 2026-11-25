#define _GNU_SOURCE
#include "gridd/rm_client.h"
#include "gridd/daemon.h"
#include "gridd/log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>

/* Standard Linux NVIDIA RM ioctl parameter blocks */
typedef struct {
    uint32_t hRoot;
    uint32_t hObjectParent;
    uint32_t hObjectNew;
    uint32_t hClass;
    uint64_t pAllocParms;
    uint32_t paramsSize;
    uint32_t status;
} nv_ioctl_rm_alloc_t;

typedef struct {
    uint32_t hRoot;
    uint32_t hObjectParent;
    uint32_t hObjectOld;
    uint32_t status;
} nv_ioctl_rm_free_t;

typedef struct {
    uint32_t hClient;
    uint32_t hObject;
    uint32_t cmd;
    uint32_t flags;
    uint64_t params;
    uint32_t paramsSize;
    uint32_t status;
} nv_ioctl_rm_control_t;

_Static_assert(sizeof(nv_ioctl_rm_alloc_t) == 32, "nv_ioctl_rm_alloc_t size must match 32B");
_Static_assert(sizeof(nv_ioctl_rm_free_t) == 16, "nv_ioctl_rm_free_t size must match 16B");
_Static_assert(sizeof(nv_ioctl_rm_control_t) == 32, "nv_ioctl_rm_control_t size must match 32B");

#define NV_ESC_CARD_INFO_IOCTL    0xc90046c8U
#define NV_ESC_REGISTER_FD_IOCTL  0xc00446c9U

typedef struct {
    uint32_t cmd;
    uint32_t reply;
    char versionString[64];
} nv_rm_api_version_t;

typedef struct {
    uint32_t valid;
    uint32_t domain;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t pad0;
    uint32_t vendor_device_id;
    uint32_t gpu_id;
    uint32_t interrupt_line;
    uint64_t reg_address;
    uint64_t reg_size;
    uint64_t fb_address;
    uint64_t fb_size;
    uint32_t minor_number;
    uint8_t pad1[12];
} nv_rm_card_info_t;

_Static_assert(sizeof(nv_rm_api_version_t) == 72, "nv_rm_api_version_t size must match 72B");
_Static_assert(sizeof(nv_rm_card_info_t) == 72, "nv_rm_card_info_t size must match 72B");

static int g_gpu_fds[GRIDD_MAX_GPUS] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};
static size_t g_gpu_fd_count = 0;

static void close_gpu_fds(void) {
    for (size_t i = 0; i < g_gpu_fd_count && i < GRIDD_MAX_GPUS; ++i) {
        if (g_gpu_fds[i] >= 0) {
            close(g_gpu_fds[i]);
            g_gpu_fds[i] = -1;
        }
    }
    g_gpu_fd_count = 0;
}

static void register_gpu_minor_fd(int ctl_fd, uint32_t minor) {
    if (g_gpu_fd_count >= GRIDD_MAX_GPUS) return;
    char dev_path[64];
    snprintf(dev_path, sizeof(dev_path), "/dev/nvidia%u", minor);
    int gfd = open(dev_path, O_RDWR | O_CLOEXEC);
    if (gfd < 0) return;
    int arg = ctl_fd;
    if (ioctl(gfd, (unsigned long)NV_ESC_REGISTER_FD_IOCTL, &arg) == 0) {
        g_gpu_fds[g_gpu_fd_count++] = gfd;
    } else {
        close(gfd);
    }
}

static void read_proc_driver_version(char *out, size_t out_sz) {
    snprintf(out, out_sz, "580.196.07");
    FILE *fp = fopen("/proc/driver/nvidia/version", "r");
    if (!fp) return;
    char line[256];
    if (fgets(line, sizeof(line), fp)) {
        const char *p = strstr(line, "Kernel Module");
        if (p) {
            p += 13;
            while (*p == ' ' || *p == '\t') ++p;
            size_t i = 0;
            while (p[i] && p[i] != ' ' && p[i] != '\t' && p[i] != '\n' && i + 1 < out_sz) {
                out[i] = p[i];
                ++i;
            }
            if (i > 0) out[i] = '\0';
        }
    }
    fclose(fp);
}

static int default_open_ctl(void *ctx) {
    (void)ctx;
    int fd = open("/dev/nvidiactl", O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;

    nv_rm_api_version_t ver;
    memset(&ver, 0, sizeof(ver));
    ver.cmd = 0x31U;
    read_proc_driver_version(ver.versionString, sizeof(ver.versionString));
    ioctl(fd, (unsigned long)NV_ESC_CHECK_VERSION_STR, &ver);

    nv_rm_card_info_t cards[GRIDD_MAX_GPUS];
    memset(cards, 0, sizeof(cards));
    close_gpu_fds();
    if (ioctl(fd, (unsigned long)NV_ESC_CARD_INFO_IOCTL, cards) == 0) {
        for (size_t i = 0; i < GRIDD_MAX_GPUS; ++i) {
            if (cards[i].valid) {
                register_gpu_minor_fd(fd, cards[i].minor_number);
            }
        }
    }
    if (g_gpu_fd_count == 0) {
        register_gpu_minor_fd(fd, 0U);
    }
    return fd;
}

static void default_close_ctl(void *ctx, int ctl_fd) {
    (void)ctx;
    close_gpu_fds();
    if (ctl_fd >= 0) close(ctl_fd);
}

static uint32_t default_rm_alloc(void *ctx, int ctl_fd, uint32_t h_client, uint32_t h_parent,
                                 uint32_t h_object, uint32_t h_class, void *params,
                                 uint32_t params_size, uint32_t *out_client) {
    (void)ctx;
    nv_ioctl_rm_alloc_t req;
    memset(&req, 0, sizeof(req));
    req.hRoot = h_client;
    req.hObjectParent = h_parent;
    req.hObjectNew = h_object;
    req.hClass = h_class;
    req.pAllocParms = (uint64_t)(uintptr_t)params;
    req.paramsSize = params_size;
    if (ioctl(ctl_fd, (unsigned long)NV_ESC_RM_ALLOC, &req) < 0) {
        return 0x59U;
    }
    if (out_client) *out_client = req.hObjectNew;
    return req.status;
}

static uint32_t default_rm_free(void *ctx, int ctl_fd, uint32_t h_client, uint32_t h_parent,
                                uint32_t h_object) {
    (void)ctx;
    nv_ioctl_rm_free_t req;
    memset(&req, 0, sizeof(req));
    req.hRoot = h_client;
    req.hObjectParent = h_parent;
    req.hObjectOld = h_object;
    if (ioctl(ctl_fd, (unsigned long)NV_ESC_RM_FREE, &req) < 0) {
        return 0x59U;
    }
    return req.status;
}

static uint32_t default_rm_control(void *ctx, int ctl_fd, uint32_t h_client, uint32_t h_object,
                                   uint32_t cmd, void *params, uint32_t params_size) {
    (void)ctx;
    nv_ioctl_rm_control_t req;
    memset(&req, 0, sizeof(req));
    req.hClient = h_client;
    req.hObject = h_object;
    req.cmd = cmd;
    req.params = (uint64_t)(uintptr_t)params;
    req.paramsSize = params_size;
    if (ioctl(ctl_fd, (unsigned long)NV_ESC_RM_CONTROL, &req) < 0) {
        return 0x59U;
    }
    return req.status;
}

static const gridd_rm_ops_t g_default_rm_ops = {
    .open_ctl = default_open_ctl,
    .close_ctl = default_close_ctl,
    .rm_alloc = default_rm_alloc,
    .rm_free = default_rm_free,
    .rm_control = default_rm_control,
    .ctx = NULL
};

const gridd_rm_ops_t *gridd_rm_default_ops(void) {
    return &g_default_rm_ops;
}

static uint32_t rm_ctrl(gridd_rm_client_t *c, uint32_t obj, uint32_t cmd, void *params, uint32_t sz) {
    return c->ops->rm_control(c->ops->ctx, c->ctl_fd, c->h_client, obj, cmd, params, sz);
}

static uint32_t rm_alloc(gridd_rm_client_t *c, uint32_t parent, uint32_t obj, uint32_t cls,
                         void *params, uint32_t sz, uint32_t *out_client) {
    return c->ops->rm_alloc(c->ops->ctx, c->ctl_fd, c->h_client, parent, obj, cls,
                             params, sz, out_client);
}

gridd_status_t gridd_rm_init(gridd_rm_client_t *client, const gridd_rm_ops_t *ops) {
    if (!client) return GRIDD_ERR_RM;
    memset(client, 0, sizeof(*client));
    client->ops = ops ? ops : &g_default_rm_ops;
    client->ctl_fd = client->ops->open_ctl(client->ops->ctx);
    if (client->ctl_fd < 0) {
        LOG_E("Failed to initialise RM client");
        return GRIDD_ERR_RM;
    }

    uint32_t h_root = 0;
    uint32_t st = rm_alloc(client, 0, 0, 0x00000000U, NULL, 0, &h_root);
    if (st != 0 || h_root == 0) {
        LOG_E("Failed to initialise RM client");
        client->ops->close_ctl(client->ops->ctx, client->ctl_fd);
        client->ctl_fd = -1;
        return GRIDD_ERR_RM;
    }
    client->h_client = h_root;

    /* Attach all GPUs (0x215, 0x84 bytes) */
    uint8_t attach_buf[0x84];
    memset(attach_buf, 0, sizeof(attach_buf));
    uint32_t all_mask[2] = {0x0000ffffU, 0xffffffffU};
    memcpy(attach_buf, all_mask, sizeof(all_mask));
    st = rm_ctrl(client, client->h_client, NV0000_CTRL_CMD_GPU_ATTACH_IDS, attach_buf, sizeof(attach_buf));
    if (st != 0) {
        LOG_E("Failed to attach IDs, err code: 0x%x", st);
        gridd_rm_destroy(client);
        return GRIDD_ERR_RM;
    }

    /* Enumerate attached GPU IDs (0x201, 0x80 bytes) */
    int32_t gpu_ids[GRIDD_MAX_GPUS];
    for (size_t i = 0; i < GRIDD_MAX_GPUS; ++i) gpu_ids[i] = -1;
    st = rm_ctrl(client, client->h_client, NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS, gpu_ids, sizeof(gpu_ids));
    if (st != 0) {
        LOG_E("Failed to get attached IDs, err code: 0x%x", st);
        gridd_rm_destroy(client);
        return GRIDD_ERR_RM;
    }

    uint32_t count = 0;
    while (count < GRIDD_MAX_GPUS && gpu_ids[count] != -1) {
        count++;
    }
    client->gpu_count = count;
    LOG_I("Found %u GPU(s)", count);

    for (uint32_t i = 0; i < count; ++i) {
        gridd_rm_gpu_t *g = &client->gpus[i];
        g->gpu_id = (uint32_t)gpu_ids[i];

        uint8_t id_info[0x28];
        memset(id_info, 0, sizeof(id_info));
        memcpy(id_info, &g->gpu_id, 4);
        st = rm_ctrl(client, client->h_client, NV0000_CTRL_CMD_GPU_GET_ID_INFO, id_info, sizeof(id_info));
        if (st != 0) {
            LOG_E("FAILED to get details of GPU (0x%x), err: 0x%x", g->gpu_id, st);
            gridd_rm_destroy(client);
            return GRIDD_ERR_RM;
        }
        memcpy(&g->gpu_instance, id_info + 8, 4);

        uint8_t dev_alloc[0x28];
        memset(dev_alloc, 0, sizeof(dev_alloc));
        memcpy(dev_alloc, &g->gpu_instance, 4);
        g->h_device = 0xdae00100U | i;
        st = rm_alloc(client, client->h_client, g->h_device, 0x0080U, dev_alloc, sizeof(dev_alloc), NULL);
        if (st != 0) {
            uint8_t dev_alloc_lg[128];
            memset(dev_alloc_lg, 0, sizeof(dev_alloc_lg));
            memcpy(dev_alloc_lg, &g->gpu_instance, 4);
            st = rm_alloc(client, client->h_client, g->h_device, 0x0080U, dev_alloc_lg, sizeof(dev_alloc_lg), NULL);
        }
        if (st != 0) {
            LOG_E("Failed to allocate device: %u, err code: 0x%x", g->gpu_instance, st);
            g->h_device = 0;
            gridd_rm_destroy(client);
            return GRIDD_ERR_RM;
        }

        g->h_subdevice = 0xdae00200U | i;
        st = rm_alloc(client, g->h_device, g->h_subdevice, 0x2080U, NULL, 0, NULL);
        if (st != 0) {
            LOG_E("Failed to allocate subdevice for device:%u, err code: 0x%x", g->gpu_instance, st);
            g->h_subdevice = 0;
            gridd_rm_destroy(client);
            return GRIDD_ERR_RM;
        }

        uint32_t vmode = 0;
        st = rm_ctrl(client, g->h_device, NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE, &vmode, sizeof(vmode));
        if (st == 0x34U) {
            /* R580+ widened NV0080_CTRL_GPU_GET_VIRTUALIZATION_MODE_PARAMS to 8 bytes. */
            uint32_t vmode_r580[2] = {0, 0};
            st = rm_ctrl(client, g->h_device, NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE,
                         vmode_r580, sizeof(vmode_r580));
            if (st == 0) {
                vmode = vmode_r580[0];
            }
        }
        if (st != 0) {
            LOG_E("Failed to determine virtualization mode for device %u, err code 0x%x", i, st);
            gridd_rm_destroy(client);
            return GRIDD_ERR_RM;
        }
        g->virtualization_mode = vmode;
        if (vmode == NV_VGPU_VIRTUALIZATION_MODE_GUEST) {
            client->is_vgpu_guest = true;
            g->h_vgpu = 0xdae00300U | i;
            st = rm_alloc(client, g->h_device, g->h_vgpu, 0xa080U, NULL, 0, NULL);
            if (st != 0) {
                LOG_E("Failed to allocate vGPU for device:%u, err code: 0x%x", g->gpu_instance, st);
                g->h_vgpu = 0;
                gridd_rm_destroy(client);
                return GRIDD_ERR_RM;
            }
        }
    }

    return GRIDD_OK;
}

void gridd_rm_destroy(gridd_rm_client_t *client) {
    if (!client || !client->ops) return;
    if (client->ctl_fd >= 0) {
        for (uint32_t i = 0; i < client->gpu_count; ++i) {
            gridd_rm_gpu_t *g = &client->gpus[i];
            if (g->h_vgpu) client->ops->rm_free(client->ops->ctx, client->ctl_fd, client->h_client, g->h_device, g->h_vgpu);
            if (g->h_subdevice) client->ops->rm_free(client->ops->ctx, client->ctl_fd, client->h_client, g->h_device, g->h_subdevice);
            if (g->h_device) client->ops->rm_free(client->ops->ctx, client->ctl_fd, client->h_client, client->h_client, g->h_device);
        }
        if (client->h_client) {
            client->ops->rm_free(client->ops->ctx, client->ctl_fd, client->h_client, 0, client->h_client);
        }
        client->ops->close_ctl(client->ops->ctx, client->ctl_fd);
        client->ctl_fd = -1;
    }
    client->h_client = 0;
    client->gpu_count = 0;
}

gridd_status_t gridd_rm_set_sw_persistence(gridd_rm_client_t *client, bool enable) {
    if (!client) return GRIDD_ERR_RM;
    uint32_t val = enable ? 1U : 0U;
    for (uint32_t i = 0; i < client->gpu_count; ++i) {
        uint32_t st = rm_ctrl(client, client->gpus[i].h_device,
                              NV0080_CTRL_CMD_GPU_MODIFY_SW_STATE_PERSISTENCE, &val, sizeof(val));
        if (st != 0) {
            LOG_E("Failed to modify persistence state, err code: 0x%x", st);
            return GRIDD_ERR_RM;
        }
    }
    return GRIDD_OK;
}

gridd_status_t gridd_rm_query_features(gridd_rm_client_t *client) {
    if (!client) return GRIDD_ERR_RM;
    bool any_licensable = false;

    for (uint32_t i = 0; i < client->gpu_count; ++i) {
        uint8_t buf[0x3b8];
        memset(buf, 0, sizeof(buf));
        uint32_t st = rm_ctrl(client, client->gpus[i].h_subdevice,
                              NV2080_CTRL_CMD_GPU_GET_LICENSABLE_FEATURES, buf, sizeof(buf));
        if (st != 0) {
            LOG_E("Failed to read licensable features for GPU %u, err code: 0x%x", i, st);
            return GRIDD_ERR_RM;
        }

        gridd_rm_gpu_t *g = &client->gpus[i];
        g->licensable = (buf[0] != 0);
        if (g->licensable) any_licensable = true;

        uint32_t feat_cnt = 0;
        memcpy(&feat_cnt, buf + 0x84, 4);
        if (feat_cnt > GRIDD_MAX_FEATURES) feat_cnt = GRIDD_MAX_FEATURES;
        g->feature_count = feat_cnt;

        for (uint32_t f = 0; f < feat_cnt; ++f) {
            size_t off = 0x88 + (size_t)f * 0x110;
            if (off + 0x110 > sizeof(buf)) break;
            memcpy(&g->features[f].feature_code, buf + off, 4);
            if (buf[off + 0x86U] >= 0x20U && buf[off + 0x86U] < 0x7fU) {
                memcpy(g->features[f].product_name, buf + off + 0x86U, 128);
                g->features[f].product_name[128] = '\0';
                g->features[f].active = (buf[off + 0x05U] == 1);
            } else {
                memcpy(g->features[f].product_name, buf + off + 4, 255);
                g->features[f].product_name[255] = '\0';
                g->features[f].active = (buf[off + 0x104U] == 1);
            }
        }
    }

    if (!any_licensable) {
        LOG_E("Licensing not supported for GPUs in the system");
        return GRIDD_ERR_UNSUPPORTED;
    }
    return GRIDD_OK;
}

bool gridd_rm_gpu_supports_feature(gridd_rm_client_t *client, uint32_t gpu_idx,
                                   uint32_t feature_code, uint32_t *out_feat_idx) {
    if (!client || gpu_idx >= client->gpu_count) return false;
    gridd_rm_gpu_t *g = &client->gpus[gpu_idx];

    if (feature_code != 0 && g->virtualization_mode != NV_VGPU_VIRTUALIZATION_MODE_GUEST) {
        uint8_t gsp_buf[0x48];
        memset(gsp_buf, 0, sizeof(gsp_buf));
        uint32_t st = rm_ctrl(client, g->h_subdevice, NV2080_CTRL_CMD_GSP_GET_FEATURES,
                              gsp_buf, sizeof(gsp_buf));
        if (st != 0) {
            LOG_E("Failed to read GSP firmware features for GPU 0x%x, err code: 0x%x",
                  g->gpu_id >> 16, st);
            return false;
        }
        bool gsp_enabled = (gsp_buf[4] != 0);
        if (!gsp_enabled && feature_code == GRIDD_FEATURE_VCS) {
            LOG_E("NVIDIA Virtual Compute Server feature needs GSP firmware enabled. Enable GSP firmware to use this feature.");
            return false;
        }
        if (gsp_enabled && feature_code != GRIDD_FEATURE_VCS) {
            LOG_E("Invalid feature type %u requested for the underlying GSP firmware configuration. Disable GSP firmware to use this feature.",
                  feature_code);
            return false;
        }
    }

    if (!g->licensable || g->feature_count == 0) return false;
    for (uint32_t f = 0; f < g->feature_count; ++f) {
        if (g->features[f].feature_code == feature_code) {
            if (out_feat_idx) *out_feat_idx = f;
            return true;
        }
    }
    return false;
}

gridd_status_t gridd_rm_query_pci_ids(gridd_rm_client_t *client) {
    if (!client) return GRIDD_ERR_RM;
    for (uint32_t i = 0; i < client->gpu_count; ++i) {
        uint8_t pci_buf[0x10];
        memset(pci_buf, 0, sizeof(pci_buf));
        uint32_t st = rm_ctrl(client, client->gpus[i].h_subdevice,
                              NV2080_CTRL_CMD_BUS_GET_PCI_INFO, pci_buf, sizeof(pci_buf));
        if (st != 0) {
            LOG_E("Failed to read PCI Id for GPU %u, err code: 0x%x", i, st);
            return GRIDD_ERR_RM;
        }
        uint32_t dev_vendor = 0;
        memcpy(&dev_vendor, pci_buf + 0, 4);
        client->gpus[i].pci_vendor_id = (uint16_t)(dev_vendor & 0xffffU);
        client->gpus[i].pci_device_id = (uint16_t)((dev_vendor >> 16) & 0xffffU);
        memcpy(&client->gpus[i].pci_domain, pci_buf + 4, 4);
        client->gpus[i].pci_bus = pci_buf[8];
        client->gpus[i].pci_slot = pci_buf[9];
        client->gpus[i].pci_queried = true;
    }
    return GRIDD_OK;
}

gridd_status_t gridd_rm_get_csp_id(gridd_rm_client_t *client, uint32_t gpu_idx, uint32_t *out_csp_id) {
    if (!client || gpu_idx >= client->gpu_count || !out_csp_id) return GRIDD_ERR_RM;
    uint32_t csp_id = 0;
    uint32_t st = rm_ctrl(client, client->gpus[gpu_idx].h_subdevice,
                          NV2080_CTRL_CMD_GPU_GET_CSP_IDENTIFIER, &csp_id, sizeof(csp_id));
    if (st != 0) {
        LOG_E("Failed to get the CSP identifier %u, err code: 0x%x", 0U, st);
        return GRIDD_ERR_RM;
    }
    *out_csp_id = csp_id;
    return GRIDD_OK;
}

gridd_status_t gridd_rm_enable_feature(gridd_rm_client_t *client, uint32_t feature_code,
                                       uint32_t *out_active_feature, bool *out_reboot_required) {
    if (!client) return GRIDD_ERR_RM;
    if (out_reboot_required) *out_reboot_required = false;
    if (out_active_feature) *out_active_feature = feature_code;

    uint32_t matched = 0;
    for (uint32_t i = 0; i < client->gpu_count; ++i) {
        if (!gridd_rm_gpu_supports_feature(client, i, feature_code, NULL)) continue;
        matched++;

        uint8_t buf[0x84];
        memset(buf, 0, sizeof(buf));
        memcpy(buf + 0x80, &feature_code, 4);
        uint32_t st = rm_ctrl(client, client->gpus[i].h_subdevice,
                              NV2080_CTRL_CMD_GPU_ENABLE_LICENSED_FEATURE, buf, sizeof(buf));
        if (st != 0 && st != 0x1fU) {
            LOG_E("Failed to enable features for GPU %u, err code: 0x%x", i, st);
            return GRIDD_ERR_RM;
        }
        if (st == 0) {
            uint32_t active_feat = feature_code;
            memcpy(&active_feat, buf + 0x80, 4);
            if (out_active_feature) *out_active_feature = active_feat;
            if (active_feat != feature_code) {
                if (out_reboot_required) *out_reboot_required = true;
                LOG_N("Your system is currently running %u. Restart your system for enabling a different feature",
                      active_feat);
                return GRIDD_OK;
            }
        }
    }
    return matched > 0 ? GRIDD_OK : GRIDD_ERR_INVALID_PARAM;
}

gridd_status_t gridd_rm_set_license_status(gridd_rm_client_t *client, uint32_t feature_code,
                                           uint32_t expiry_epoch, uint8_t state) {
    if (!client) return GRIDD_ERR_RM;
    bool err = false;
    for (uint32_t i = 0; i < client->gpu_count; ++i) {
        if (!gridd_rm_gpu_supports_feature(client, i, feature_code, NULL)) continue;
        uint8_t buf[0x8c];
        memset(buf, 0, sizeof(buf));
        buf[0x80] = 1;
        memcpy(buf + 0x84, &expiry_epoch, 4);
        buf[0x88] = state;
        uint32_t st = rm_ctrl(client, client->gpus[i].h_subdevice,
                              NV2080_CTRL_CMD_GPU_SET_LICENSE_STATUS, buf, sizeof(buf));
        if (st != 0) {
            LOG_E("Failed to set license status for GPU %u, err code: 0x%x", i, st);
            err = true;
        }
    }
    return err ? GRIDD_ERR_RM : GRIDD_OK;
}

gridd_status_t gridd_rm_disable_feature(gridd_rm_client_t *client, uint32_t feature_code) {
    if (!client) return GRIDD_ERR_RM;
    bool err = false;
    for (uint32_t i = 0; i < client->gpu_count; ++i) {
        if (!gridd_rm_gpu_supports_feature(client, i, feature_code, NULL)) continue;
        uint32_t feat = feature_code;
        uint32_t st = rm_ctrl(client, client->gpus[i].h_subdevice,
                              NV2080_CTRL_CMD_GPU_DISABLE_LICENSED_FEATURE, &feat, sizeof(feat));
        if (st != 0) {
            LOG_E("Failed to disable licensed feature %u for GPU %u, err code: 0x%x",
                  feature_code, i, st);
            err = true;
        }
    }
    return err ? GRIDD_ERR_RM : GRIDD_OK;
}

gridd_status_t gridd_rm_get_telemetry(gridd_rm_client_t *client,
                                      char *guest_ver, size_t guest_sz,
                                      char *host_ver, size_t host_sz) {
    if (!client) return GRIDD_ERR_RM;
    if (guest_ver && guest_sz > 0) {
        guest_ver[0] = '\0';
        char driver_ver[64] = {0};
        char build_ver[64] = {0};
        char title[64] = {0};
        struct {
            uint32_t size;
            uint32_t pad;
            uint64_t pDriverVersionBuffer;
            uint64_t pVersionBuffer;
            uint64_t pTitleBuffer;
            uint64_t reserved;
        } ver_req;
        memset(&ver_req, 0, sizeof(ver_req));
        ver_req.size = sizeof(driver_ver);
        ver_req.pDriverVersionBuffer = (uint64_t)(uintptr_t)driver_ver;
        ver_req.pVersionBuffer = (uint64_t)(uintptr_t)build_ver;
        ver_req.pTitleBuffer = (uint64_t)(uintptr_t)title;

        uint32_t st = rm_ctrl(client, client->h_client,
                              NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION, &ver_req, 0x28);
        if (st == 0) {
            snprintf(guest_ver, guest_sz, "%s", driver_ver);
        } else {
            LOG_E("Failed to fetch telemetry data, err code: 0x%x", st);
        }
    }
    if (host_ver && host_sz > 0) {
        if (client->is_vgpu_guest) {
            uint8_t hbuf[0x508];
            memset(hbuf, 0, sizeof(hbuf));
            uint32_t st = rm_ctrl(client, client->h_client,
                                  NV0000_CTRL_CMD_SYSTEM_GET_HOST_VERSION, hbuf, sizeof(hbuf));
            if (st == 0) {
                hbuf[sizeof(hbuf) - 1] = '\0';
                snprintf(host_ver, host_sz, "%s", (const char *)hbuf);
            } else {
                LOG_E("Failed to fetch host driver version, err code: 0x%x", st);
                snprintf(host_ver, host_sz, "NA");
            }
        } else {
            snprintf(host_ver, host_sz, "NA");
        }
    }
    return GRIDD_OK;
}

gridd_status_t gridd_rm_get_unlicensed_state(gridd_rm_client_t *client, uint32_t gpu_idx,
                                             uint32_t *out_state) {
    if (!client || gpu_idx >= client->gpu_count || !out_state) return GRIDD_ERR_RM;
    uint8_t buf[0x14];
    memset(buf, 0, sizeof(buf));
    uint32_t st = rm_ctrl(client, client->gpus[gpu_idx].h_subdevice,
                          NV2080_CTRL_CMD_GPU_GET_UNLICENSED_STATE, buf, sizeof(buf));
    if (st != 0) {
        LOG_E("Failed to read unlicensed state machine states, err code: 0x%x", st);
        return GRIDD_ERR_RM;
    }
    memcpy(out_state, buf, 4);
    return GRIDD_OK;
}

typedef struct {
    uint32_t gpu_idx;
    int stop_pipe_rd;
    const gridd_rm_ops_t *ops;
} monitor_arg_t;

static bool nvidia_proc_present(void) {
    DIR *d = opendir("/proc/driver/nvidia");
    if (!d) return false;
    struct dirent *de;
    bool found = false;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

static void *unlicensed_monitor_thread(void *raw) {
    monitor_arg_t *arg = (monitor_arg_t *)raw;
    uint32_t last_state = 0;

    while (!gridd_daemon_stop_requested()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(arg->stop_pipe_rd, &rfds);
        struct timeval tv = {.tv_sec = 30, .tv_usec = 0};
        int rc = select(arg->stop_pipe_rd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            LOG_E("Wait on license state change failed with error: %s", strerror(errno));
            break;
        }
        if (rc > 0 || gridd_daemon_stop_requested()) {
            break;
        }
        if (!nvidia_proc_present()) {
            continue;
        }

        gridd_rm_client_t rm;
        if (gridd_rm_init(&rm, arg->ops) != GRIDD_OK) {
            continue;
        }
        uint32_t state = 0;
        gridd_status_t st = gridd_rm_get_unlicensed_state(&rm, arg->gpu_idx, &state);
        gridd_rm_destroy(&rm);
        if (st != GRIDD_OK) {
            continue;
        }
        if (state != last_state) {
            if (state == 3) {
                LOG_N("Valid GRID license not found. GPU features and performance are restricted. To enable full functionality please configure licensing details.");
            } else if (state == 0) {
                LOG_E("Your system is in unlicensed unknown state");
            } else if (state == 4) {
                LOG_N("Valid GRID license not found. GPU features and performance are fully restricted. To enable full functionality please configure licensing details.");
                break;
            } else if (state == 5) {
                break;
            }
            last_state = state;
        }
    }

    free(arg);
    return NULL;
}

int gridd_rm_start_unlicensed_monitor(gridd_rm_client_t *client, int stop_pipe_rd) {
    if (!client || stop_pipe_rd < 0) return GRIDD_ERR_RM;
    if (gridd_rm_query_features(client) != GRIDD_OK) {
        return GRIDD_ERR_RM;
    }

    uint32_t target_gpu = 0;
    bool found_licensable = false;
    for (uint32_t i = 0; i < client->gpu_count; ++i) {
        if (!client->gpus[i].licensable) continue;
        found_licensable = true;
        target_gpu = i;
        for (uint32_t f = 0; f < client->gpus[i].feature_count; ++f) {
            if (client->gpus[i].features[f].active) {
                return GRIDD_OK;
            }
        }
        break;
    }
    if (!found_licensable) {
        return GRIDD_ERR_RM;
    }

    monitor_arg_t *arg = (monitor_arg_t *)calloc(1, sizeof(*arg));
    if (!arg) return GRIDD_ERR_RM;
    arg->gpu_idx = target_gpu;
    arg->stop_pipe_rd = stop_pipe_rd;
    arg->ops = client->ops;

    pthread_t tid;
    int rc = pthread_create(&tid, NULL, unlicensed_monitor_thread, arg);
    if (rc != 0) {
        LOG_E("Failed to create wait thread, error: %u", (unsigned int)rc);
        free(arg);
        return GRIDD_ERR_RM;
    }
    pthread_detach(tid);
    return GRIDD_OK;
}
