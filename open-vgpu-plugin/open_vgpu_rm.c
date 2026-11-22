#include "open_vgpu_rm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int default_open_fn(const char *path, int flags)
{
    return open(path, flags, 0);
}

static int default_close_fn(int fd)
{
    return close(fd);
}

static int default_ioctl_fn(int fd, unsigned long req, void *arg)
{
    return ioctl(fd, req, arg);
}

static open_vgpu_rm_sys_ops_t g_rm_sys = {
    .open_fn  = default_open_fn,
    .close_fn = default_close_fn,
    .ioctl_fn = default_ioctl_fn,
};
static int g_has_custom_sys_ops = 0;

void open_vgpu_rm_set_sys_ops(const open_vgpu_rm_sys_ops_t *ops)
{
    if (!ops) {
        open_vgpu_rm_reset_sys_ops();
        return;
    }
    g_rm_sys.open_fn  = ops->open_fn  ? ops->open_fn  : default_open_fn;
    g_rm_sys.close_fn = ops->close_fn ? ops->close_fn : default_close_fn;
    g_rm_sys.ioctl_fn = ops->ioctl_fn ? ops->ioctl_fn : default_ioctl_fn;
    g_has_custom_sys_ops = 1;
}

void open_vgpu_rm_reset_sys_ops(void)
{
    g_rm_sys.open_fn  = default_open_fn;
    g_rm_sys.close_fn = default_close_fn;
    g_rm_sys.ioctl_fn = default_ioctl_fn;
    g_has_custom_sys_ops = 0;
}

int open_vgpu_rm_has_custom_sys_ops(void)
{
    return g_has_custom_sys_ops;
}

int open_vgpu_rm_ctl_ioctl(int fd, uint32_t cmd, uint32_t size, unsigned long req, void *params)
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }
    for (;;) {
        int rc;
        if (size <= 0x1000U) {
            rc = g_rm_sys.ioctl_fn(fd, req, params);
        } else {
            nv_ioctl_xfer_t xfer;
            xfer.cmd  = cmd;
            xfer.size = size;
            xfer.ptr  = (NvP64)(uintptr_t)params;
            rc = g_rm_sys.ioctl_fn(fd, NV_IOCTL(NV_ESC_IOCTL_XFER_CMD, sizeof(xfer)), &xfer);
        }
        if (rc >= 0) {
            return rc;
        }
        if (errno != EINTR && errno != EAGAIN) {
            return -1;
        }
    }
}

static void detect_kernel_driver_version(char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%s", NV_VGPU_DRIVER_VERSION_STR);
    if (g_has_custom_sys_ops) {
        return;
    }
    FILE *fp = fopen("/proc/driver/nvidia/version", "r");
    if (!fp) {
        return;
    }
    char line[256];
    if (fgets(line, sizeof(line), fp) != NULL) {
        const char *p = strstr(line, "Kernel Module");
        if (p) {
            p += strlen("Kernel Module");
            while (*p == ' ' || *p == '\t') p++;
            size_t i = 0;
            while (p[i] && p[i] != ' ' && p[i] != '\t' && p[i] != '\n' && i + 1U < out_sz) {
                out[i] = p[i];
                i++;
            }
            out[i] = '\0';
        }
    }
    fclose(fp);
}

NV_STATUS open_vgpu_rm_alloc_root(int ctl_fd, uint32_t *out_client)
{
    if (!out_client || ctl_fd < 0) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    NVOS21_ROOT_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hClass = NV01_ROOT;
    if (open_vgpu_rm_ctl_ioctl(ctl_fd, NV_ESC_RM_ALLOC, sizeof(p),
                               NV_IOCTL(NV_ESC_RM_ALLOC, sizeof(p)), &p) < 0) {
        return NV_ERR_OPERATING_SYSTEM;
    }
    if (p.status == NV_OK) {
        *out_client = p.hObjectNew;
    }
    return p.status;
}

NV_STATUS open_vgpu_rm_alloc_obj(int ctl_fd, uint32_t h_root, uint32_t h_parent, uint32_t h_new,
                                 uint32_t h_class, void *params, uint32_t params_size)
{
    if (ctl_fd < 0 || h_new == 0U) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    NVOS64_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hRoot         = h_root;
    p.hObjectParent = h_parent;
    p.hObjectNew    = h_new;
    p.hClass        = h_class;
    p.pAllocParms   = (NvP64)(uintptr_t)params;
    p.paramsSize    = params_size;
    if (open_vgpu_rm_ctl_ioctl(ctl_fd, NV_ESC_RM_ALLOC, sizeof(p),
                               NV_IOCTL(NV_ESC_RM_ALLOC, sizeof(p)), &p) < 0) {
        return NV_ERR_OPERATING_SYSTEM;
    }
    return p.status;
}

NV_STATUS open_vgpu_rm_control(int ctl_fd, uint32_t h_client, uint32_t h_object, uint32_t cmd,
                               void *params, uint32_t params_size)
{
    if (ctl_fd < 0 || h_client == 0U || h_object == 0U) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    NVOS54_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hClient    = h_client;
    p.hObject    = h_object;
    p.cmd        = cmd;
    p.params     = (NvP64)(uintptr_t)params;
    p.paramsSize = params_size;
    if (open_vgpu_rm_ctl_ioctl(ctl_fd, NV_ESC_RM_CONTROL, sizeof(p),
                               NV_IOCTL(NV_ESC_RM_CONTROL, sizeof(p)), &p) < 0) {
        return NV_ERR_OPERATING_SYSTEM;
    }
    return p.status;
}

NV_STATUS open_vgpu_rm_free(int ctl_fd, uint32_t h_root, uint32_t h_parent, uint32_t h_old)
{
    if (ctl_fd < 0 || h_root == 0U || h_old == 0U) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    NVOS00_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hRoot         = h_root;
    p.hObjectParent = h_parent;
    p.hObjectOld    = h_old;
    if (open_vgpu_rm_ctl_ioctl(ctl_fd, NV_ESC_RM_FREE, sizeof(p),
                               NV_IOCTL(NV_ESC_RM_FREE, sizeof(p)), &p) < 0) {
        return NV_ERR_OPERATING_SYSTEM;
    }
    return p.status;
}

NV_STATUS open_vgpu_rm_dup_object(int ctl_fd, uint32_t h_client_dst, uint32_t h_parent,
                                  uint32_t h_obj_dst, uint32_t h_client_src, uint32_t h_obj_src,
                                  uint32_t flags)
{
    if (ctl_fd < 0 || h_client_dst == 0U || h_obj_dst == 0U ||
        h_client_src == 0U || h_obj_src == 0U) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    NVOS55_DUP_PARAMETERS p;
    memset(&p, 0, sizeof(p));
    p.hClient    = h_client_dst;
    p.hParent    = h_parent;
    p.hObject    = h_obj_dst;
    p.hClientSrc = h_client_src;
    p.hObjectSrc = h_obj_src;
    p.flags      = flags;
    if (open_vgpu_rm_ctl_ioctl(ctl_fd, NV_ESC_RM_DUP_OBJECT, sizeof(p),
                               NV_IOCTL(NV_ESC_RM_DUP_OBJECT, sizeof(p)), &p) < 0) {
        return NV_ERR_OPERATING_SYSTEM;
    }
    return p.status;
}

static int parse_bdf_filter(const char *bdf, unsigned int *dom, unsigned int *bus,
                            unsigned int *slot, unsigned int *fn)
{
    if (!bdf || !bdf[0]) {
        return 0;
    }
    if (sscanf(bdf, "%x:%x:%x.%x", dom, bus, slot, fn) == 4) {
        return 1;
    }
    if (sscanf(bdf, "%x:%x.%x", bus, slot, fn) == 3) {
        *dom = 0U;
        return 1;
    }
    return 0;
}

NV_STATUS open_vgpu_rm_attach(open_vgpu_rm_ctx_t *rm, uint32_t dev_handle, uint32_t vgpu_type_id,
                              const char *bdf, const uint8_t uuid[16], uint32_t vm_pid)
{
    if (!rm) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    if (rm->attached) {
        open_vgpu_rm_detach(rm);
    }
    memset(rm, 0, sizeof(*rm));
    rm->ctl_fd = -1;
    rm->gpu_fd = -1;
    rm->dev_handle = dev_handle;
    rm->vgpu_type_id = vgpu_type_id ? vgpu_type_id : 65U;
    rm->encoder_capacity = 100U;

    int ctl_fd = g_rm_sys.open_fn("/dev/nvidiactl", O_RDWR | O_CLOEXEC);
    if (ctl_fd < 0) {
        return NV_ERR_OPERATING_SYSTEM;
    }
    rm->ctl_fd = ctl_fd;

    nv_ioctl_rm_api_version_t ver;
    memset(&ver, 0, sizeof(ver));
    detect_kernel_driver_version(ver.versionString, sizeof(ver.versionString));
    ver.cmd = 0x31U;
    if (open_vgpu_rm_ctl_ioctl(ctl_fd, NV_ESC_CHECK_VERSION_STR, sizeof(ver),
                               NV_IOCTL(NV_ESC_CHECK_VERSION_STR, sizeof(ver)), &ver) < 0) {
        open_vgpu_rm_detach(rm);
        return NV_ERR_OPERATING_SYSTEM;
    }

    nv_ioctl_card_info_t cards[NV_MAX_PROBED_GPUS];
    memset(cards, 0, sizeof(cards));
    int have_cards = (open_vgpu_rm_ctl_ioctl(ctl_fd, NV_ESC_CARD_INFO, sizeof(cards),
                                             NV_IOCTL(NV_ESC_CARD_INFO, sizeof(cards)), cards) >= 0);

    NV_STATUS st = open_vgpu_rm_alloc_root(ctl_fd, &rm->h_root);
    if (st != NV_OK || rm->h_root == 0U) {
        open_vgpu_rm_detach(rm);
        return st != NV_OK ? st : NV_ERR_GENERIC;
    }

    unsigned int want_dom = 0, want_bus = 0, want_slot = 0, want_fn = 0;
    int have_bdf = parse_bdf_filter(bdf, &want_dom, &want_bus, &want_slot, &want_fn);

    rm->gpu_id = 0U;
    rm->minor  = 0U;
    if (have_cards) {
        for (size_t i = 0; i < NV_MAX_PROBED_GPUS; i++) {
            if (!cards[i].valid || cards[i].gpu_id == 0U) {
                continue;
            }
            if (!have_bdf ||
                (cards[i].domain == want_dom && cards[i].bus == want_bus &&
                 cards[i].slot == want_slot && cards[i].function == want_fn)) {
                rm->gpu_id = cards[i].gpu_id;
                rm->minor  = cards[i].minor_number;
                break;
            }
            if (rm->gpu_id == 0U) {
                rm->gpu_id = cards[i].gpu_id;
                rm->minor  = cards[i].minor_number;
            }
        }
    }
    if (rm->gpu_id == 0U) {
        uint32_t probed[NV_MAX_PROBED_GPUS];
        memset(probed, 0, sizeof(probed));
        st = open_vgpu_rm_control(ctl_fd, rm->h_root, rm->h_root,
                                  NV0000_CTRL_CMD_GPU_GET_PROBED_IDS, probed, sizeof(probed));
        if (st == NV_OK) {
            for (size_t i = 0; i < NV_MAX_PROBED_GPUS; i++) {
                if (probed[i] != 0U && probed[i] != 0xffffffffU) {
                    rm->gpu_id = probed[i];
                    break;
                }
            }
        }
    }
    if (rm->gpu_id == 0U) {
        open_vgpu_rm_detach(rm);
        return NV_ERR_OBJECT_NOT_FOUND;
    }

    uint32_t attach_ids[NV_MAX_PROBED_GPUS + 1];
    memset(attach_ids, 0xff, sizeof(attach_ids));
    attach_ids[0] = rm->gpu_id;
    st = open_vgpu_rm_control(ctl_fd, rm->h_root, rm->h_root,
                              NV0000_CTRL_CMD_GPU_ATTACH_IDS, attach_ids, sizeof(attach_ids));
    if (st != NV_OK) {
        open_vgpu_rm_detach(rm);
        return st;
    }

    NV0000_CTRL_GPU_GET_ID_INFO_PARAMS id_info;
    memset(&id_info, 0, sizeof(id_info));
    id_info.gpuId = rm->gpu_id;
    st = open_vgpu_rm_control(ctl_fd, rm->h_root, rm->h_root,
                              NV0000_CTRL_CMD_GPU_GET_ID_INFO, &id_info, sizeof(id_info));
    if (st == NV_OK) {
        rm->dev_inst = id_info.deviceInstance;
    }

    char gpu_path[64];
    snprintf(gpu_path, sizeof(gpu_path), "/dev/nvidia%u", rm->minor);
    int gpu_fd = g_rm_sys.open_fn(gpu_path, O_RDWR | O_CLOEXEC);
    if (gpu_fd < 0) {
        open_vgpu_rm_detach(rm);
        return NV_ERR_OPERATING_SYSTEM;
    }
    rm->gpu_fd = gpu_fd;
    uint32_t reg_fd = (uint32_t)ctl_fd;
    if (open_vgpu_rm_ctl_ioctl(gpu_fd, NV_ESC_REGISTER_FD, sizeof(reg_fd),
                               NV_IOCTL(NV_ESC_REGISTER_FD, sizeof(reg_fd)), &reg_fd) < 0) {
        open_vgpu_rm_detach(rm);
        return NV_ERR_OPERATING_SYSTEM;
    }

    uint32_t base_h = ((dev_handle ? dev_handle : 1U) << 24) | 0xff0000U;
    rm->h_dev       = base_h | 0x0001U;
    rm->h_subdev    = base_h | 0x0002U;
    rm->h_vgpu_cfg  = 0x2aU;
    rm->h_host_vgpu = base_h | 0xfffdU;

    NV0080_ALLOC_PARAMETERS dev_params;
    memset(&dev_params, 0, sizeof(dev_params));
    dev_params.deviceId = rm->dev_inst;
    st = open_vgpu_rm_alloc_obj(ctl_fd, rm->h_root, rm->h_root, rm->h_dev,
                                NV01_DEVICE_0, &dev_params, sizeof(dev_params));
    if (st != NV_OK) {
        rm->h_dev = 0U;
        open_vgpu_rm_detach(rm);
        return st;
    }

    NV2080_ALLOC_PARAMETERS sub_params;
    memset(&sub_params, 0, sizeof(sub_params));
    sub_params.subDeviceId = 0U;
    st = open_vgpu_rm_alloc_obj(ctl_fd, rm->h_root, rm->h_dev, rm->h_subdev,
                                NV20_SUBDEVICE_0, &sub_params, sizeof(sub_params));
    if (st != NV_OK) {
        rm->h_subdev = 0U;
        open_vgpu_rm_detach(rm);
        return st;
    }

    st = open_vgpu_rm_alloc_obj(ctl_fd, rm->h_root, rm->h_subdev, rm->h_vgpu_cfg,
                                NVA081_VGPU_CONFIG, NULL, 0U);
    if (st != NV_OK) {
        rm->h_vgpu_cfg = 0U;
        open_vgpu_rm_detach(rm);
        return st;
    }

    uint32_t pci_ids[4] = { 0 };
    if (open_vgpu_rm_control(ctl_fd, rm->h_root, rm->h_subdev,
                             NV2080_CTRL_CMD_BUS_GET_PCI_INFO, pci_ids, sizeof(pci_ids)) == NV_OK) {
        rm->pci_vendor        = pci_ids[0] & 0xffffU;
        rm->pci_device        = (pci_ids[0] >> 16) & 0xffffU;
        rm->pci_subsys_vendor = pci_ids[1] & 0xffffU;
        rm->pci_subsys_device = (pci_ids[1] >> 16) & 0xffffU;
    }

    uint32_t vmode = 0U;
    if (open_vgpu_rm_control(ctl_fd, rm->h_root, rm->h_dev,
                             NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE,
                             &vmode, sizeof(vmode)) == NV_OK) {
        rm->virt_mode = vmode;
    }

    NVA081_CTRL_VGPU_CONFIG_GET_VGPU_TYPE_INFO_PARAMS *tinfo =
        (NVA081_CTRL_VGPU_CONFIG_GET_VGPU_TYPE_INFO_PARAMS *)calloc(1U, sizeof(*tinfo));
    if (tinfo) {
        tinfo->vgpuType = rm->vgpu_type_id;
        if (open_vgpu_rm_control(ctl_fd, rm->h_root, rm->h_vgpu_cfg,
                                 NVA081_CTRL_CMD_VGPU_CONFIG_GET_VGPU_TYPE_INFO,
                                 tinfo, (uint32_t)sizeof(*tinfo)) == NV_OK) {
            snprintf(rm->live_profile_name, sizeof(rm->live_profile_name), "%.31s", tinfo->vgpuName);
            NvU64 usable_fb = 0ULL;
            memcpy(&usable_fb, &tinfo->tail[0], sizeof(usable_fb));
            /* +0x188 is total profileLength (e.g. 4 GiB), +0x190 is usable framebuffer (3776 MiB) */
            if (usable_fb != 0ULL && (tinfo->fbLength == 0ULL || usable_fb <= tinfo->fbLength)) {
                rm->live_fb_length = usable_fb;
            } else {
                rm->live_fb_length = tinfo->fbLength;
            }
            rm->live_frl_config  = tinfo->frlConfig;
            rm->live_max_res_x   = tinfo->maxResolutionX;
            rm->live_max_res_y   = tinfo->maxResolutionY;
            rm->live_bar1_length = 256ULL * 1024ULL * 1024ULL;
        }
        free(tinfo);
    }

    NVA082_ALLOC_PARAMETERS nva082;
    memset(&nva082, 0, sizeof(nva082));
    nva082.vgpuType = rm->vgpu_type_id;
    nva082.vmPid    = vm_pid ? vm_pid : (uint32_t)getpid();
    nva082.vmIdType = 1U;
    if (uuid) {
        memcpy(nva082.vmId, uuid, 16U);
    }
    nva082.hPluginFBAllocationClient = rm->h_root;

    rm->nva082_status = open_vgpu_rm_alloc_obj(ctl_fd, rm->h_root, rm->h_subdev, rm->h_host_vgpu,
                                               NVA082_HOST_VGPU_DEVICE, &nva082,
                                               (uint32_t)sizeof(nva082));
    if (rm->nva082_status == NV_OK) {
        rm->nva082_allocated = 1;
        uint8_t p2p_buf[8] = { 0 };
        if (open_vgpu_rm_control(ctl_fd, rm->h_root, rm->h_host_vgpu,
                                 NVA082_CTRL_CMD_GET_P2P_CAP, p2p_buf, sizeof(p2p_buf)) == NV_OK) {
            rm->p2p_cap = p2p_buf[0];
        }
        uint32_t enc = 0U;
        if (open_vgpu_rm_control(ctl_fd, rm->h_root, rm->h_host_vgpu,
                                 NVA082_CTRL_CMD_GET_ENCODER_CAPACITY,
                                 &enc, sizeof(enc)) == NV_OK && enc != 0U) {
            rm->encoder_capacity = enc;
        }
    } else {
        rm->h_host_vgpu = 0U;
        rm->nva082_allocated = 0;
    }

    rm->attached = 1;
    return NV_OK;
}

void open_vgpu_rm_detach(open_vgpu_rm_ctx_t *rm)
{
    if (!rm) {
        return;
    }
    open_vgpu_rm_free_all_guest_clients(rm);
    if (rm->ctl_fd >= 0 && rm->h_root != 0U) {
        if (rm->nva082_allocated && rm->h_host_vgpu != 0U) {
            open_vgpu_rm_free(rm->ctl_fd, rm->h_root, rm->h_subdev, rm->h_host_vgpu);
        }
        if (rm->h_vgpu_cfg != 0U) {
            open_vgpu_rm_free(rm->ctl_fd, rm->h_root, rm->h_subdev, rm->h_vgpu_cfg);
        }
        if (rm->h_subdev != 0U) {
            open_vgpu_rm_free(rm->ctl_fd, rm->h_root, rm->h_dev, rm->h_subdev);
        }
        if (rm->h_dev != 0U) {
            open_vgpu_rm_free(rm->ctl_fd, rm->h_root, rm->h_root, rm->h_dev);
        }
        if (rm->gpu_id != 0U) {
            uint32_t detach_ids[NV_MAX_PROBED_GPUS + 1];
            memset(detach_ids, 0xff, sizeof(detach_ids));
            detach_ids[0] = rm->gpu_id;
            open_vgpu_rm_control(rm->ctl_fd, rm->h_root, rm->h_root,
                                 NV0000_CTRL_CMD_GPU_DETACH_IDS, detach_ids, sizeof(detach_ids));
        }
        open_vgpu_rm_free(rm->ctl_fd, rm->h_root, rm->h_root, rm->h_root);
    }
    if (rm->gpu_fd >= 0) {
        g_rm_sys.close_fn(rm->gpu_fd);
    }
    if (rm->ctl_fd >= 0) {
        g_rm_sys.close_fn(rm->ctl_fd);
    }
    memset(rm, 0, sizeof(*rm));
    rm->ctl_fd = -1;
    rm->gpu_fd = -1;
}

NV_STATUS open_vgpu_rm_lookup_or_create_guest_client(open_vgpu_rm_ctx_t *rm, uint32_t guest_client,
                                                     uint32_t *out_host_client, int *out_created)
{
    if (!rm || guest_client == 0U || !out_host_client) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    if (out_created) {
        *out_created = 0;
    }
    for (size_t i = 0; i < GP104_MAX_GUEST_CLIENTS; i++) {
        if (rm->guest_clients[i].in_use && rm->guest_clients[i].guest_client == guest_client) {
            *out_host_client = rm->guest_clients[i].host_client;
            return NV_OK;
        }
    }
    for (size_t i = 0; i < GP104_MAX_GUEST_CLIENTS; i++) {
        if (!rm->guest_clients[i].in_use) {
            uint32_t hc = guest_client;
            if (rm->attached && rm->ctl_fd >= 0) {
                NV_STATUS st = open_vgpu_rm_alloc_root(rm->ctl_fd, &hc);
                if (st != NV_OK || hc == 0U) {
                    return st != NV_OK ? st : NV_ERR_INSUFFICIENT_RESOURCES;
                }
            }
            rm->guest_clients[i].in_use = 1;
            rm->guest_clients[i].guest_client = guest_client;
            rm->guest_clients[i].host_client  = hc;
            *out_host_client = hc;
            if (out_created) {
                *out_created = 1;
            }
            return NV_OK;
        }
    }
    return NV_ERR_INSUFFICIENT_RESOURCES;
}

int open_vgpu_rm_lookup_guest_client(const open_vgpu_rm_ctx_t *rm, uint32_t guest_client,
                                     uint32_t *out_host_client)
{
    if (!rm || guest_client == 0U || !out_host_client) {
        return 0;
    }
    for (size_t i = 0; i < GP104_MAX_GUEST_CLIENTS; i++) {
        if (rm->guest_clients[i].in_use && rm->guest_clients[i].guest_client == guest_client) {
            *out_host_client = rm->guest_clients[i].host_client;
            return 1;
        }
    }
    return 0;
}

void open_vgpu_rm_free_guest_client(open_vgpu_rm_ctx_t *rm, uint32_t guest_client)
{
    if (!rm || guest_client == 0U) {
        return;
    }
    for (size_t i = 0; i < GP104_MAX_GUEST_CLIENTS; i++) {
        if (rm->guest_clients[i].in_use && rm->guest_clients[i].guest_client == guest_client) {
            if (rm->attached && rm->ctl_fd >= 0 && rm->guest_clients[i].host_client != 0U) {
                open_vgpu_rm_free(rm->ctl_fd, rm->guest_clients[i].host_client,
                                  rm->guest_clients[i].host_client,
                                  rm->guest_clients[i].host_client);
            }
            rm->guest_clients[i].in_use = 0;
            rm->guest_clients[i].guest_client = 0U;
            rm->guest_clients[i].host_client = 0U;
            return;
        }
    }
}

void open_vgpu_rm_free_all_guest_clients(open_vgpu_rm_ctx_t *rm)
{
    if (!rm) {
        return;
    }
    for (size_t i = 0; i < GP104_MAX_GUEST_CLIENTS; i++) {
        if (rm->guest_clients[i].in_use) {
            if (rm->attached && rm->ctl_fd >= 0 && rm->guest_clients[i].host_client != 0U) {
                open_vgpu_rm_free(rm->ctl_fd, rm->guest_clients[i].host_client,
                                  rm->guest_clients[i].host_client,
                                  rm->guest_clients[i].host_client);
            }
            rm->guest_clients[i].in_use = 0;
            rm->guest_clients[i].guest_client = 0U;
            rm->guest_clients[i].host_client = 0U;
        }
    }
}
