#include "open_vgpu_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t r32(const uint8_t *p, size_t off)
{
    uint32_t v = 0U;
    memcpy(&v, p + off, 4U);
    return v;
}

static uint64_t r64(const uint8_t *p, size_t off)
{
    uint64_t v = 0ULL;
    memcpy(&v, p + off, 8U);
    return v;
}

static void w32(uint8_t *p, size_t off, uint32_t v)
{
    memcpy(p + off, &v, 4U);
}

static void w64(uint8_t *p, size_t off, uint64_t v)
{
    memcpy(p + off, &v, 8U);
}

int gp104_is_reserved_plugin_handle(uint32_t dev_handle, uint32_t h)
{
    uint32_t base = (dev_handle ? dev_handle : 1U) << 24;
    return h >= (base | 0xff0000U) && h <= (base | 0xffffffU);
}

uint32_t gp104_resolve_host_client(gp104_vgpu_dev_t *dev, uint32_t guest_client, uint32_t *out_hc)
{
    if (!dev || !out_hc) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    if (!dev->rm.attached || dev->rm.ctl_fd < 0) {
        *out_hc = guest_client;
        return NV_OK;
    }
    if (open_vgpu_rm_lookup_guest_client(&dev->rm, guest_client, out_hc)) {
        return NV_OK;
    }
    int created = 0;
    return open_vgpu_rm_lookup_or_create_guest_client(&dev->rm, guest_client, out_hc, &created);
}

void gp104_build_static_info_blobs(gp104_vgpu_dev_t *dev)
{
    if (!dev) {
        return;
    }
    memset(dev->static_info_buf, 0, sizeof(dev->static_info_buf));
    dev->static_info_len = 4096U;
    uint64_t fb = dev->rm.live_fb_length ? dev->rm.live_fb_length : dev->profile.usable_fb_bytes;
    uint64_t b1 = dev->rm.live_bar1_length ? dev->rm.live_bar1_length : dev->profile.bar1_bytes;
    uint32_t dev_id = dev->rm.pci_device ? dev->rm.pci_device : dev->profile.device_id;
    uint32_t sub_id = dev->rm.pci_subsys_device ? dev->rm.pci_subsys_device : dev->profile.subsys_device_id;

    w64(dev->static_info_buf, 0x00, fb);
    w64(dev->static_info_buf, 0x08, b1);
    w32(dev->static_info_buf, 0x10, (sub_id << 16) | dev_id);
    w32(dev->static_info_buf, 0x14, dev->profile.vgpu_type_id);
    w32(dev->static_info_buf, 0x18, dev->frl_fps);
    w32(dev->static_info_buf, 0x1c, dev->rm.p2p_cap);
    w32(dev->static_info_buf, 0x20, dev->rm.encoder_capacity ? dev->rm.encoder_capacity : 100U);
    snprintf((char *)(dev->static_info_buf + 0x40), 32U, "%.31s",
             dev->rm.live_profile_name[0] ? dev->rm.live_profile_name : dev->profile.name);

    memset(dev->gr_static_info_buf, 0, sizeof(dev->gr_static_info_buf));
    dev->gr_static_info_len = 4096U;
    /* GP104 Pascal (SM 6.1, 10 GPC/TPC baseline exposed consistently) */
    w32(dev->gr_static_info_buf, 0x00, dev->pmc_boot0);
    w32(dev->gr_static_info_buf, 0x04, dev->pmc_boot42);
    w32(dev->gr_static_info_buf, 0x08, 0x00000001U); /* initialized flag */
}

static uint32_t forward_rm_ctrl(gp104_vgpu_dev_t *dev, uint32_t guest_client, uint32_t h_obj,
                                uint32_t cmd, void *params, uint32_t params_sz, int use_host_root)
{
    if (!dev->rm.attached || dev->rm.ctl_fd < 0) {
        return NV_OK;
    }
    uint32_t hc = dev->rm.h_root;
    uint32_t obj = h_obj;
    if (!use_host_root) {
        if (!open_vgpu_rm_lookup_guest_client(&dev->rm, guest_client, &hc)) {
            int created = 0;
            NV_STATUS cst = open_vgpu_rm_lookup_or_create_guest_client(&dev->rm, guest_client,
                                                                       &hc, &created);
            if (cst != NV_OK) {
                return cst;
            }
        }
    } else {
        obj = dev->rm.h_root;
    }
    dev->vrpc_rm_forwarded_calls++;
    return open_vgpu_rm_control(dev->rm.ctl_fd, hc, obj, cmd, params, params_sz);
}

static uint32_t handle_simple_rm_ctrl(gp104_vgpu_dev_t *dev, const uint8_t *payload,
                                      uint8_t *out_payload, uint32_t payload_len,
                                      uint32_t cmd, uint32_t expected_min_wire,
                                      uint32_t rm_param_sz, int use_host_root)
{
    if (payload_len < expected_min_wire) {
        return 0xff100002U;
    }
    uint8_t *rm_buf = (uint8_t *)calloc(1U, rm_param_sz ? rm_param_sz : 4U);
    if (!rm_buf) {
        return NV_ERR_NO_MEMORY;
    }

    uint32_t gc = 0U, obj = 0U;
    size_t hdr_skip = use_host_root ? 0U : 8U;
    if (!use_host_root) {
        gc  = r32(payload, 0U);
        obj = r32(payload, 4U);
    }

    size_t copy_len = (payload_len > hdr_skip) ? (payload_len - hdr_skip) : 0U;
    if (copy_len > rm_param_sz) {
        copy_len = rm_param_sz;
    }
    if (copy_len > 0U) {
        memcpy(rm_buf, payload + hdr_skip, copy_len);
    }

    uint32_t st = forward_rm_ctrl(dev, gc, obj, cmd, rm_buf, rm_param_sz, use_host_root);
    if (st == NV_OK && copy_len > 0U) {
        memcpy(out_payload + hdr_skip, rm_buf, copy_len);
    }
    free(rm_buf);
    return st;
}

static uint32_t handle_vrpc_set_guest_info(gp104_vgpu_dev_t *dev, uint8_t *buf, uint32_t tot)
{
    if (dev->guest_vgx_initialized) {
        return 0x40U; /* NV_ERR_INVALID_STATE */
    }
    if (tot >= 0x2cU) {
        dev->guest_vgx_major = r32(buf, 0x20);
        dev->guest_vgx_minor = r32(buf, 0x24);
        dev->guest_os_type   = r32(buf, 0x28);
    }
    if (tot >= 0x54U && buf[0x34] >= 0x20U && buf[0x34] < 0x7fU) {
        snprintf(dev->guest_driver_ver, sizeof(dev->guest_driver_ver), "%.31s",
                 (const char *)(buf + 0x34));
    }
    gp104_build_static_info_blobs(dev);
    dev->guest_vgx_initialized = 1;
    return NV_OK;
}

static uint32_t handle_vrpc_set_guest_info_ext(gp104_vgpu_dev_t *dev, const uint8_t *payload,
                                               uint32_t payload_len)
{
    if (payload_len >= 4U && payload[0] >= 0x20U && payload[0] < 0x7fU) {
        snprintf(dev->guest_branch_ver, sizeof(dev->guest_branch_ver), "%.31s",
                 (const char *)payload);
    }
    if (!dev->rm.nva082_allocated || dev->rm.ctl_fd < 0) {
        return NV_OK;
    }
    uint8_t p[0x50];
    memset(p, 0, sizeof(p));
    p[0] = 1U;
    snprintf((char *)(p + 0x04), 0x10U, "%s", NV_VGPU_DRIVER_VERSION_STR);
    snprintf((char *)(p + 0x24), 0x20U, "%.31s",
             dev->guest_branch_ver[0] ? dev->guest_branch_ver : NV_VGPU_DRIVER_VERSION_STR);
    uint32_t cap = (uint32_t)sizeof(p);
    dev->vrpc_rm_forwarded_calls++;
    return open_vgpu_rm_control(dev->rm.ctl_fd, dev->rm.h_root, dev->rm.h_host_vgpu,
                                NVA082_CTRL_CMD_SET_GUEST_INFO, p, cap);
}

static uint32_t handle_vrpc_set_license_state(gp104_vgpu_dev_t *dev, const uint8_t *payload,
                                              uint32_t payload_len)
{
    if (payload_len < 0x14U && dev->rm.nva082_allocated) {
        return 0xff100002U;
    }
    uint8_t lic_state = (payload_len >= 1U) ? payload[0] : 0U;
    uint32_t d4 = (payload_len >= 8U) ? r32(payload, 0x04) : 0U;
    uint32_t d8 = (payload_len >= 12U) ? r32(payload, 0x08) : 0U;
    uint32_t dc = (payload_len >= 16U) ? r32(payload, 0x0c) : 0U;
    uint8_t exp = (payload_len >= 0x11U) ? payload[0x10] : 0U;
    if (exp >= 5U) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    dev->license_state = lic_state;
    dev->license_expiry_status = exp;

    if (!dev->rm.nva082_allocated || dev->rm.ctl_fd < 0) {
        return NV_OK;
    }
    uint8_t p[0x14];
    memset(p, 0, sizeof(p));
    p[0] = lic_state;
    w32(p, 0x04, d4);
    w32(p, 0x08, d8);
    w32(p, 0x0c, dc);
    w32(p, 0x10, (uint32_t)exp);
    dev->vrpc_rm_forwarded_calls++;
    return open_vgpu_rm_control(dev->rm.ctl_fd, dev->rm.h_root, dev->rm.h_host_vgpu,
                                NVA082_CTRL_CMD_SET_LICENSE_STATE, p, sizeof(p));
}

static uint32_t handle_vrpc_get_encoder_cap(gp104_vgpu_dev_t *dev, uint8_t *payload,
                                            uint32_t payload_len)
{
    if (payload_len < 0x0cU) {
        return 0xff100002U;
    }
    uint32_t cap = dev->rm.encoder_capacity ? dev->rm.encoder_capacity : 100U;
    if (dev->rm.nva082_allocated && dev->rm.ctl_fd >= 0) {
        uint32_t live = 0U;
        dev->vrpc_rm_forwarded_calls++;
        NV_STATUS st = open_vgpu_rm_control(dev->rm.ctl_fd, dev->rm.h_root, dev->rm.h_host_vgpu,
                                            NVA082_CTRL_CMD_GET_ENCODER_CAPACITY,
                                            &live, sizeof(live));
        if (st != NV_OK) {
            return st;
        }
        cap = live;
    }
    w32(payload, 0x08, cap);
    return NV_OK;
}

static uint32_t handle_vrpc_pf_reg_read32(gp104_vgpu_dev_t *dev, uint8_t *payload,
                                          uint32_t payload_len)
{
    if (payload_len < 0x0cU) {
        return 0xff100002U;
    }
    uint64_t off = r64(payload, 0x00);
    uint32_t val = 0U, dummy = 0U;
    uint32_t rc = gp104_bar0_access(dev, 0U, VGPU_EMUL_MMIO, off, 4U, &val, &dummy);
    if (rc != VMIOP_SUCCESS) {
        return vmiop_err_to_rm_status(rc);
    }
    w32(payload, 0x08, val);
    return NV_OK;
}

static uint32_t handle_vrpc_static_chunk(const uint8_t *src, uint32_t src_len,
                                         uint8_t *payload, uint32_t payload_len,
                                         size_t max_payload_avail)
{
    if (payload_len < 8U) {
        return 0xff100002U;
    }
    uint32_t off   = r32(payload, 0x00);
    uint32_t chunk = r32(payload, 0x04);
    if ((size_t)chunk + 8U > max_payload_avail || (size_t)chunk + 8U > (size_t)payload_len) {
        return 0xff100002U;
    }
    memset(payload + 8U, 0, chunk);
    if (off < src_len) {
        uint32_t avail = src_len - off;
        uint32_t n = (chunk < avail) ? chunk : avail;
        memcpy(payload + 8U, src + off, n);
    }
    return NV_OK;
}

static uint32_t handle_vrpc_fb_usage_or_legacy(gp104_vgpu_dev_t *dev, uint8_t *buf,
                                               size_t buf_len, uint32_t tot)
{
    /* Exact 535.309.01 SET_VGPU_FB_USAGE wire message is 40 bytes (0x20 hdr + 8B u64) */
    if (tot == 0x28U) {
        uint64_t used = r64(buf, 0x20);
        dev->guest_fb_usage_bytes = used;
        if (dev->rm.nva082_allocated && dev->rm.ctl_fd >= 0) {
            dev->vrpc_rm_forwarded_calls++;
            return open_vgpu_rm_control(dev->rm.ctl_fd, dev->rm.h_root, dev->rm.h_host_vgpu,
                                        NVA082_CTRL_CMD_SET_FB_USAGE, &used, sizeof(used));
        }
        return NV_OK;
    }
    /* Preserve Phase 1A legacy standalone GET_STATIC_INFO response when tot >= 0x38 */
    if (buf_len >= 0x38U) {
        w64(buf, 0x20, dev->profile.usable_fb_bytes);
        w32(buf, 0x28, ((uint32_t)dev->profile.subsys_device_id << 16) | dev->profile.device_id);
        w32(buf, 0x2c, dev->profile.vgpu_type_id);
        w64(buf, 0x30, dev->profile.bar1_bytes);
    }
    return NV_OK;
}

static uint32_t handle_vrpc_alloc_root(gp104_vgpu_dev_t *dev, const uint8_t *payload,
                                       uint32_t payload_len)
{
    uint32_t gc = (payload_len >= 4U) ? r32(payload, 0U) : 0xc1d00001U;
    if (gp104_is_reserved_plugin_handle(dev->dev_handle, gc)) {
        return 0xff100007U; /* clash with plugin's reserved handle range */
    }
    uint32_t hc = gc;
    int created = 0;
    NV_STATUS st = open_vgpu_rm_lookup_or_create_guest_client(&dev->rm, gc, &hc, &created);
    if (st != NV_OK) {
        return st;
    }
    if (created && dev->rm.attached && dev->rm.ctl_fd >= 0) {
        uint8_t disable_iso = 1U;
        open_vgpu_rm_control(dev->rm.ctl_fd, hc, hc,
                             NV0000_CTRL_CMD_CLIENT_DISABLE_USERD_ISOL,
                             &disable_iso, sizeof(disable_iso));
    }
    return open_vgpu_plugin_record_obj(dev, gc, gc, gc, NV01_ROOT);
}

static uint32_t handle_vrpc_alloc_dev_or_subdev(gp104_vgpu_dev_t *dev, uint32_t fn,
                                                const uint8_t *payload, uint32_t payload_len)
{
    if (payload_len < 0x10U) {
        return open_vgpu_plugin_record_obj(dev,
                                           payload_len >= 4U ? r32(payload, 0U) : 0U,
                                           payload_len >= 8U ? r32(payload, 4U) : 0U,
                                           payload_len >= 12U ? r32(payload, 8U) : 0U, fn);
    }
    if (fn == VRPC_FN_SET_GUEST_FB_INFO) {
        uint32_t gc    = r32(payload, 0x00);
        uint32_t h_dev = r32(payload, 0x04);
        uint32_t h_cls = r32(payload, 0x08);
        if (gp104_is_reserved_plugin_handle(dev->dev_handle, h_dev)) {
            return 0xff100007U;
        }
        if (h_cls == NV01_DEVICE_0 && dev->rm.attached && dev->rm.ctl_fd >= 0) {
            uint32_t hc = 0U;
            if (!open_vgpu_rm_lookup_guest_client(&dev->rm, gc, &hc)) {
                return NV_ERR_OBJECT_NOT_FOUND;
            }
            NV0080_ALLOC_PARAMETERS dp;
            memset(&dp, 0, sizeof(dp));
            dp.deviceId = dev->rm.dev_inst;
            NV_STATUS st = open_vgpu_rm_alloc_obj(dev->rm.ctl_fd, hc, hc, h_dev,
                                                  NV01_DEVICE_0, &dp, sizeof(dp));
            if (st != NV_OK) {
                return st;
            }
        }
        return open_vgpu_plugin_record_obj(dev, gc, gc, h_dev, h_cls ? h_cls : NV01_DEVICE_0);
    }

    /* 0x13 ALLOC_SUBDEVICE: +0x00 subDeviceInst, +0x08 hClient, +0x0c hParent, +0x10 hSubdev, +0x14 hClass */
    uint32_t sub_inst = r32(payload, 0x00);
    uint32_t gc       = r32(payload, 0x08);
    uint32_t h_parent = r32(payload, 0x0c);
    uint32_t h_sub    = r32(payload, 0x10);
    uint32_t h_cls    = (payload_len >= 0x18U) ? r32(payload, 0x14) : NV20_SUBDEVICE_0;
    if (gp104_is_reserved_plugin_handle(dev->dev_handle, h_sub)) {
        return 0xff100007U;
    }
    if (dev->rm.attached && dev->rm.ctl_fd >= 0) {
        uint32_t hc = 0U;
        if (!open_vgpu_rm_lookup_guest_client(&dev->rm, gc, &hc)) {
            return NV_ERR_OBJECT_NOT_FOUND;
        }
        NV2080_ALLOC_PARAMETERS sp;
        memset(&sp, 0, sizeof(sp));
        sp.subDeviceId = sub_inst;
        NV_STATUS st = open_vgpu_rm_alloc_obj(dev->rm.ctl_fd, hc, h_parent, h_sub,
                                              NV20_SUBDEVICE_0, &sp, sizeof(sp));
        if (st != NV_OK) {
            return st;
        }
    }
    return open_vgpu_plugin_record_obj(dev, gc, h_parent, h_sub, h_cls);
}

static uint32_t handle_vrpc_rm_api_control(gp104_vgpu_dev_t *dev, uint8_t *buf,
                                           size_t buf_len, uint32_t tot)
{
    if (tot < 0x40U) {
        return 0xff100002U;
    }
    uint32_t gc       = r32(buf, 0x20);
    uint32_t h_obj    = r32(buf, 0x24);
    uint32_t cmd      = r32(buf, 0x28);
    uint32_t param_sz = r32(buf, 0x38);
    if ((size_t)param_sz > buf_len - 0x40U || (size_t)tot + (size_t)param_sz > buf_len) {
        return 0xff100002U;
    }
    return forward_rm_ctrl(dev, gc, h_obj, cmd, buf + 0x40, param_sz, 0);
}

static uint32_t handle_vrpc_fb_get_info(gp104_vgpu_dev_t *dev, uint8_t *payload,
                                        uint32_t payload_len)
{
    if (payload_len < 0x10U) {
        return 0xff100002U;
    }
    uint32_t count = r32(payload, 0x08);
    if (count == 0U || count > 54U || (size_t)payload_len < 0x10U + (size_t)count * 8U) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    uint64_t usable_kib = dev->profile.usable_fb_bytes >> 10;
    uint64_t used_kib   = dev->guest_fb_usage_bytes >> 10;
    uint64_t free_kib   = (usable_kib > used_kib) ? (usable_kib - used_kib) : 0ULL;
    for (uint32_t i = 0; i < count; i++) {
        size_t off = 0x10U + (size_t)i * 8U;
        uint32_t index = r32(payload, off);
        uint32_t out_v = 0U;
        if (index == 1U || index == 2U) {
            out_v = (uint32_t)usable_kib;
        } else if (index == 3U) {
            out_v = (uint32_t)free_kib;
        }
        w32(payload, off + 4U, out_v);
    }
    return NV_OK;
}

int gp104_vrpc_rm_dispatch(gp104_vgpu_dev_t *dev, uint32_t fn, uint8_t *buf,
                           size_t buf_len, uint32_t tot, uint32_t *out_status)
{
    uint8_t *payload = buf + sizeof(open_vrpc_hdr_t);
    uint32_t plen    = (tot > sizeof(open_vrpc_hdr_t)) ? (tot - (uint32_t)sizeof(open_vrpc_hdr_t)) : 0U;
    size_t   avail   = (buf_len > sizeof(open_vrpc_hdr_t)) ? (buf_len - sizeof(open_vrpc_hdr_t)) : 0U;
    uint32_t st      = 0U;

    switch (fn) {
    case VRPC_FN_SET_GUEST_SYSTEM_INFO:
        st = handle_vrpc_set_guest_info(dev, buf, tot);
        break;
    case VRPC_FN_ALLOC_ROOT:
        st = handle_vrpc_alloc_root(dev, payload, plen);
        break;
    case VRPC_FN_FREE_535: { /* 0x0a */
        uint32_t gc = (plen >= 4U) ? r32(payload, 0U) : 0U;
        uint32_t hp = (plen >= 8U) ? r32(payload, 4U) : 0U;
        uint32_t ho = (plen >= 12U) ? r32(payload, 8U) : 0U;
        if (ho != 0U) {
            gp104_mmu_on_free_obj(dev, gc, ho);
        }
        if (dev->rm.attached && dev->rm.ctl_fd >= 0 && ho != 0U) {
            uint32_t hc = 0U;
            if (open_vgpu_rm_lookup_guest_client(&dev->rm, gc, &hc)) {
                open_vgpu_rm_free(dev->rm.ctl_fd, hc, hp ? hp : hc, ho);
            }
        }
        if (ho == gc && gc != 0U) {
            gp104_mmu_on_free_client(dev, gc);
            open_vgpu_rm_free_guest_client(&dev->rm, gc);
        }
        st = open_vgpu_plugin_free_obj(dev, gc, ho);
        break;
    }
    case VRPC_FN_SET_OBJECT:          /* 0x13 */
    case VRPC_FN_SET_GUEST_FB_INFO:   /* 0x20 */
        st = handle_vrpc_alloc_dev_or_subdev(dev, fn, payload, plen);
        break;
    case VRPC_FN_DUP_OBJECT_535: {    /* 0x15 */
        if (plen < 0x18U) {
            st = 0xff100002U;
            break;
        }
        if (gp104_is_reserved_plugin_handle(dev->dev_handle, r32(payload, 0x08))) {
            st = 0xff100007U;
            break;
        }
        if (dev->rm.attached && dev->rm.ctl_fd >= 0) {
            uint32_t hc_dst = 0U, hc_src = 0U;
            if (!open_vgpu_rm_lookup_guest_client(&dev->rm, r32(payload, 0x00), &hc_dst) ||
                !open_vgpu_rm_lookup_guest_client(&dev->rm, r32(payload, 0x0c), &hc_src)) {
                st = NV_ERR_OBJECT_NOT_FOUND;
                break;
            }
            st = open_vgpu_rm_dup_object(dev->rm.ctl_fd, hc_dst, r32(payload, 0x04),
                                         r32(payload, 0x08), hc_src, r32(payload, 0x10),
                                         r32(payload, 0x14));
        } else {
            st = NV_OK;
        }
        break;
    }
    case VRPC_FN_IDLE_CHANNELS_V535:  /* 0x17 */
        st = NV_OK;
        break;
    case VRPC_FN_RM_API_CONTROL:      /* 0x1a */
        st = handle_vrpc_rm_api_control(dev, buf, buf_len, tot);
        break;
    case VRPC_FN_SET_LICENSE_STATE:   /* 0x39 */
        st = handle_vrpc_set_license_state(dev, payload, plen);
        break;
    case VRPC_FN_SET_SEMA_VALIDATION: /* 0x3b */
        dev->sema_mem_validation = (plen >= 4U) ? r32(payload, 0x00) : 1U;
        st = NV_OK;
        break;
    case VRPC_FN_GET_ENCODER_CAPACITY: /* 0x3e */
        st = handle_vrpc_get_encoder_cap(dev, payload, plen);
        break;
    case VRPC_FN_SET_GUEST_SYSTEM_EXT: /* 0x3f */
        st = handle_vrpc_pf_reg_read32(dev, payload, plen);
        break;
    case VRPC_FN_SET_GUEST_BRANCH_VER: /* 0x40 */
        st = handle_vrpc_set_guest_info_ext(dev, payload, plen);
        break;
    case VRPC_FN_BATCH_50:             /* 0x50 */
        st = handle_vrpc_static_chunk(dev->static_info_buf, dev->static_info_len,
                                      payload, plen, avail);
        break;
    case VRPC_FN_GET_STATIC_INFO:      /* 0x55 */
        st = handle_vrpc_fb_usage_or_legacy(dev, buf, buf_len, tot);
        break;
    case VRPC_FN_GR_CTXSW_ZCULL:       /* 0x5b */
        st = handle_simple_rm_ctrl(dev, payload, payload, plen,
                                   NV2080_CTRL_CMD_GR_CTXSW_ZCULL, 0x10U, 0x608U, 0);
        break;
    case VRPC_FN_FB_GET_INFO_200A:     /* 0x5c */
        st = handle_simple_rm_ctrl(dev, payload, payload, plen,
                                   NV2080_CTRL_CMD_FB_0x200A, 0x10U, 8U, 0);
        break;
    case VRPC_FN_GPFIFO_SCHEDULE: {    /* 0x61 */
        if (plen < 0x10U) {
            st = 0xff100002U;
            break;
        }
        uint32_t subcmd = r32(payload, 0x08);
        if (subcmd != NVA06C_CTRL_CMD_GPFIFO_SCHEDULE &&
            subcmd != NVA06F_CTRL_CMD_GPFIFO_SCHEDULE) {
            st = NV_ERR_INVALID_ARGUMENT;
            break;
        }
        uint16_t sched = payload[0x0c];
        st = forward_rm_ctrl(dev, r32(payload, 0x00), r32(payload, 0x04),
                             subcmd, &sched, sizeof(sched), 0);
        if (st == NV_OK) {
            payload[0x0c] = (uint8_t)(sched & 0xffU);
        }
        break;
    }
    case VRPC_FN_TSG_CTRL:             /* 0x62 */
        st = handle_simple_rm_ctrl(dev, payload, payload, plen,
                                   NVA06C_CTRL_CMD_TSG_0x0103, 0x10U, 8U, 0);
        break;
    case VRPC_FN_SM_DEBUG_CTRL:        /* 0x6e */
        st = handle_simple_rm_ctrl(dev, payload, payload, plen,
                                   NV83DE_CTRL_CMD_DEBUG_0x0309, 0x0cU, 4U, 0);
        break;
    case VRPC_FN_GR_GFXP_PREEMPT:      /* 0x70 */
        st = handle_simple_rm_ctrl(dev, payload, payload, plen,
                                   NV2080_CTRL_CMD_GR_GFXP_PREEMPT, 0x10U, 0x68U, 0);
        break;
    case VRPC_FN_GR_CTRL_1210:         /* 0x71 */
        st = handle_simple_rm_ctrl(dev, payload, payload, plen,
                                   NV2080_CTRL_CMD_GR_GET_0x1210, 0x10U, 0x20U, 0);
        break;
    case VRPC_FN_GR_CTRL_1208:         /* 0x72 */
        st = handle_simple_rm_ctrl(dev, payload, payload, plen,
                                   NV2080_CTRL_CMD_GR_GET_0x1208, 0x10U, 0x18U, 0);
        break;
    case VRPC_FN_DEV_CTRL_1812:        /* 0x78 */
        st = handle_simple_rm_ctrl(dev, payload, payload, plen,
                                   NV0080_CTRL_CMD_FB_0x1812, 0x0cU, 4U, 0);
        break;
    case VRPC_FN_PERF_CTRL_2A02:       /* 0x79 */
        st = handle_simple_rm_ctrl(dev, payload, payload, plen,
                                   NV2080_CTRL_CMD_PERF_0x2A02, 0x10U, 8U, 0);
        break;
    case VRPC_FN_ZBC_CTRL_0107:        /* 0x7a */
        st = handle_simple_rm_ctrl(dev, payload, payload, plen,
                                   NV9096_CTRL_CMD_SET_ZBC_0x0107, 0x40U, 0x38U, 0);
        break;
    case VRPC_FN_PERF_CTRL_3002:       /* 0x7c */
        st = handle_simple_rm_ctrl(dev, payload, payload, plen,
                                   NV2080_CTRL_CMD_PERF_0x3002, 0x10U, 0x1708U, 0);
        break;
    case VRPC_FN_SYS_CTRL_0127:        /* 0x7d */
        st = handle_simple_rm_ctrl(dev, payload, payload, plen,
                                   NV0000_CTRL_CMD_SYSTEM_GET_P2P_CAPS_V2, 0xa4U, 0xa8U, 1);
        break;
    case VRPC_FN_SYS_CTRL_013A:        /* 0x7e */
        st = handle_simple_rm_ctrl(dev, payload, payload, plen,
                                   NV0000_CTRL_CMD_SYSTEM_GET_P2P_CAPS_MATRIX, 0x548U, 0x548U, 1);
        break;
    case VRPC_FN_FB_GET_INFO_8E:       /* 0x8e */
        st = handle_vrpc_fb_get_info(dev, payload, plen);
        break;
    case VRPC_FN_DEV_CTRL_170F:        /* 0x8f */
        st = handle_simple_rm_ctrl(dev, payload, payload, plen,
                                   NV0080_CTRL_CMD_DMA_0x170F, 0x18U, 0x10U, 0);
        break;
    case VRPC_FN_SYNC_9C:              /* 0x9c */
        st = handle_vrpc_static_chunk(dev->gr_static_info_buf, dev->gr_static_info_len,
                                      payload, plen, avail);
        break;
    default:
        return gp104_vrpc_mmu_dispatch(dev, fn, buf, buf_len, tot, out_status);
    }

    *out_status = st;
    return 1;
}
