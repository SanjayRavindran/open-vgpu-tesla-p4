#include "vmiop_env.h"
#include "rm_client.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Mock Infrastructure for Unprivileged Differential / ABI Unit Tests
 * ------------------------------------------------------------------------- */
static char g_last_log_lbl[32];
static char g_last_log_msg[256];
static uint32_t g_last_log_level = 99U;

static void test_log_hook(uint32_t level, const char *label, const char *msg)
{
    g_last_log_level = level;
    snprintf(g_last_log_lbl, sizeof(g_last_log_lbl), "%s", label ? label : "");
    snprintf(g_last_log_msg, sizeof(g_last_log_msg), "%s", msg ? msg : "");
}

/* Mock RM & VFIO ioctl tracking */
static int g_mock_vgpu_fd = 103;
static unsigned long g_last_vfio_req = 0;
static int g_mock_inject_count = 0;
static int g_mock_pin_calls = 0;
static int g_mock_unpin_calls = 0;
static int g_mock_mmio_adds = 0;
static int g_mock_mmio_removes = 0;
static int g_mock_xfer_calls = 0;
static void *g_mock_mmap_ptr = (void *)0x7f0000001000ULL;
static int g_mock_munmap_calls = 0;

static int mock_open(const char *path, int flags)
{
    (void)flags;
    if (strcmp(path, "/dev/nvidiactl") == 0 || strncmp(path, "/dev/nvidia", 11) == 0) {
        return open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
    return -1;
}

static int mock_close(int fd)
{
    return (fd >= 0) ? close(fd) : 0;
}

static int mock_rm_ioctl(int fd, unsigned long req, void *arg)
{
    (void)fd;
    unsigned int nr = _IOC_NR(req);
    if (nr == NV_ESC_IOCTL_XFER_CMD) {
        nv_ioctl_xfer_t *x = (nv_ioctl_xfer_t *)arg;
        (void)x;
        return 0;
    }
    if (nr == NV_ESC_RM_ALLOC) {
        NVOS21_ROOT_PARAMETERS *p = (NVOS21_ROOT_PARAMETERS *)arg;
        if (p->hClass == 0) {
            p->hObjectNew = 0xc1d00001U;
            p->status = NV_OK;
        } else {
            NVOS64_PARAMETERS *p64 = (NVOS64_PARAMETERS *)arg;
            p64->status = NV_OK;
        }
        return 0;
    }
    if (nr == NV_ESC_RM_CONTROL) {
        NVOS54_PARAMETERS *p = (NVOS54_PARAMETERS *)arg;
        if (p->cmd == NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS) {
            int32_t *ids = (int32_t *)(uintptr_t)p->params;
            memset(ids, 0xff, p->paramsSize);
            ids[0] = 0x100;
        } else if (p->cmd == NV0000_CTRL_CMD_GPU_GET_ID_INFO) {
            NV0000_CTRL_GPU_GET_ID_INFO_PARAMS *id =
                (NV0000_CTRL_GPU_GET_ID_INFO_PARAMS *)(uintptr_t)p->params;
            id->deviceInstance = 0;
            id->subDeviceInstance = 0;
        } else if (p->cmd == NV0000_CTRL_CMD_OS_UNIX_EXPORT_OBJECT_TO_FD) {
            NV0000_CTRL_OS_UNIX_EXPORT_OBJECT_TO_FD_PARAMS *ep =
                (NV0000_CTRL_OS_UNIX_EXPORT_OBJECT_TO_FD_PARAMS *)(uintptr_t)p->params;
            ep->fdType = 1U;
        } else if (p->cmd == 0xa0810103U) {
            assert(p->paramsSize == sizeof(NVA081_CTRL_VGPU_CONFIG_GET_VGPU_TYPE_INFO_PARAMS));
            g_mock_xfer_calls++;
        }
        p->status = NV_OK;
        return 0;
    }
    if (nr == NV_ESC_RM_ALLOC_MEMORY) {
        NVOS02_PARAMETERS *p = (NVOS02_PARAMETERS *)arg;
        p->status = NV_OK;
        return 0;
    }
    if (nr == NV_ESC_RM_MAP_MEMORY) {
        NVOS33_PARAMETERS *p = (NVOS33_PARAMETERS *)arg;
        p->pLinearAddress = (NvP64)0xdeadbeef0000ULL;
        p->status = NV_OK;
        return 0;
    }
    if (nr == NV_ESC_RM_UNMAP_MEMORY) {
        NVOS34_PARAMETERS *p = (NVOS34_PARAMETERS *)arg;
        assert(p->pLinearAddress == (NvP64)0xdeadbeef0000ULL);
        p->status = NV_OK;
        return 0;
    }
    if (nr == NV_ESC_RM_FREE) {
        NVOS00_PARAMETERS *p = (NVOS00_PARAMETERS *)arg;
        p->status = NV_OK;
        return 0;
    }
    if (nr == NV_ESC_ALLOC_OS_EVENT || nr == NV_ESC_FREE_OS_EVENT) {
        nv_ioctl_alloc_os_event_t *ev = (nv_ioctl_alloc_os_event_t *)arg;
        ev->status = NV_OK;
        return 0;
    }
    if (nr == NV_ESC_CARD_INFO) {
        nv_ioctl_card_info_t *ci = (nv_ioctl_card_info_t *)arg;
        memset(ci, 0, sizeof(nv_ioctl_card_info_t) * NV_MAX_PROBED_GPUS);
        ci[0].valid = 1;
        ci[0].gpu_id = 0x100;
        ci[0].minor_number = 0;
        return 0;
    }
    if (nr == NV_ESC_REGISTER_FD || nr == NV_ESC_CHECK_VERSION_STR) {
        return 0;
    }
    return 0;
}

static void *mock_mmap64(void *addr, size_t len, int prot, int flags, int fd, long long off)
{
    (void)addr; (void)len; (void)prot; (void)flags; (void)fd; (void)off;
    return g_mock_mmap_ptr;
}

static int mock_munmap(void *addr, size_t len)
{
    (void)addr; (void)len;
    g_mock_munmap_calls++;
    return 0;
}

static int mock_vfio_ioctl(int fd, unsigned long req, void *arg)
{
    (void)fd;
    NV_VGPU_DEV_IOCTL_PARAMS *wrap = (NV_VGPU_DEV_IOCTL_PARAMS *)arg;
    g_last_vfio_req = req;
    wrap->status = NV_OK;
    if (req == NV_VFIO_VGPU_INJECT_INTERRUPT) {
        g_mock_inject_count++;
    } else if (req == NV_VFIO_VGPU_PIN_PAGES) {
        g_mock_pin_calls++;
        NV_VFIO_VGPU_PIN_PAGES_PARAMS *pp =
            (NV_VFIO_VGPU_PIN_PAGES_PARAMS *)(uintptr_t)wrap->pCmdParams;
        const uint64_t *in = (const uint64_t *)(uintptr_t)pp->pGuestPfnMem;
        uint64_t *out = (uint64_t *)(uintptr_t)pp->pHostPfnMem;
        for (uint32_t i = 0; i < pp->pageCount; i++) {
            out[i] = in[i] + 0x100000ULL;
        }
    } else if (req == NV_VFIO_VGPU_UNPIN_PAGES) {
        g_mock_unpin_calls++;
    } else if (req == NV_VFIO_VGPU_ADD_MMIO_MAPPING) {
        NV_VFIO_VGPU_MMIO_MAPPING_PARAMS *mp =
            (NV_VFIO_VGPU_MMIO_MAPPING_PARAMS *)(uintptr_t)wrap->pCmdParams;
        assert(mp->physicalMMIOStart == 0xfe000000ULL &&
               mp->virtualMMIOStart == 0xc0000000ULL &&
               mp->mmioSize == 0x1000ULL);
        g_mock_mmio_adds++;
    } else if (req == NV_VFIO_VGPU_REMOVE_MMIO_MAPPING) {
        NV_VFIO_VGPU_MMIO_MAPPING_PARAMS *mp =
            (NV_VFIO_VGPU_MMIO_MAPPING_PARAMS *)(uintptr_t)wrap->pCmdParams;
        assert(mp->physicalMMIOStart == 0xfe000000ULL &&
               mp->virtualMMIOStart == 0xc0000000ULL &&
               mp->mmioSize == 0x1000ULL);
        g_mock_mmio_removes++;
    } else if (req == NV_VFIO_VGPU_VF_REG_ACCESS_HW) {
        NV_VFIO_VGPU_VF_REG_ACCESS_HW_PARAMS *rp =
            (NV_VFIO_VGPU_VF_REG_ACCESS_HW_PARAMS *)(uintptr_t)wrap->pCmdParams;
        if (!rp->isWrite) {
            rp->data[0] = 0xde; rp->data[1] = 0x10;
            rp->data[2] = 0xb3; rp->data[3] = 0x1b;
        }
    }
    return 0;
}

/* Mock upstream plugin + emulated access callbacks */
static uint32_t g_last_msg_op = 0;
static uint32_t g_last_emul_write_val = 0;
static int      g_emul_read_fail = 0;

static uint32_t mock_upstream_deliver(uint32_t handle, vmiop_buffer_t *buf)
{
    (void)handle;
    const uint8_t *p = (const uint8_t *)buf->elements[0].base;
    g_last_msg_op = *(const uint32_t *)(p + 0x18);
    return VMIOP_SUCCESS;
}

static vmiop_plugin_t g_mock_upstream_plugin = {
    .length = 0xa8U,
    .version = 0x50000U,
    .signature = VMIOP_PLUGIN_SIGNATURE,
    .name = "mock-nvidia-vgpu",
    .plugin_class = 1U,
    .input_classes = 2U,
    .connect_downstream_required = 1U,
    .connect_upstream_required = 0U,
    .deliver_message = mock_upstream_deliver,
};

static uint8_t g_emul_backing[256];
static uint32_t mock_emul_cb(void *priv, uint32_t is_write, uint32_t emul_space,
                             uint64_t offset, uint32_t width, void *data, uint32_t *dummy)
{
    (void)priv;
    if (dummy) *dummy = 0;
    uint64_t rel = (emul_space == VGPU_EMUL_MMIO && offset >= 0xf0000000ULL)
                       ? (offset - 0xf0000000ULL) : offset;
    if (rel + width > sizeof(g_emul_backing)) return VMIOP_ERROR_INVAL;
    if (is_write) {
        memcpy(g_emul_backing + rel, data, width);
        memcpy(&g_last_emul_write_val, data, width < 4 ? width : 4);
        return VMIOP_SUCCESS;
    }
    if (g_emul_read_fail) return VMIOP_ERROR_NOT_FOUND;
    memcpy(data, g_emul_backing + rel, width);
    return VMIOP_SUCCESS;
}

/* -------------------------------------------------------------------------
 * Unit Tests
 * ------------------------------------------------------------------------- */
static void test_abi_and_error_table(void)
{
    assert(sizeof(NV_VGPU_DEV_IOCTL_PARAMS) == 16U);
    assert(sizeof(type_info_t) == 0x5cU);
    assert(sizeof(NV_VFIO_VGPU_CTLDEV_SET_TYPES_INFO_PARAMS) == 24U);
    assert(sizeof(NV_VFIO_VGPU_CTLDEV_GET_EVENT_DATA_PARAMS) == 0x634U);
    assert(sizeof(NV_VFIO_VGPU_CTLDEV_SET_EVENT_DATA_PARAMS) == 0x41cU);
    assert(sizeof(NV_VFIO_VGPU_BAR_INFO_SET_DATA_PARAMS) == 0x80U);
    assert(sizeof(NVA081_CTRL_VGPU_CONFIG_GET_SUPPORTED_VGPU_TYPES_PARAMS) == 0x20cU);
    assert(sizeof(NVA081_CTRL_VGPU_CONFIG_GET_VGPU_TYPE_INFO_PARAMS) == 0x1538U);
    assert(sizeof(NV0000_CTRL_VGPU_CREATE_DEVICE_PARAMS) == 0x28U);
    assert(sizeof(NV0000_CTRL_VGPU_GET_INSTANCES_PARAMS) == 0x40cU);
    assert(sizeof(NV_VFIO_VGPU_PIN_PAGES_PARAMS) == 0x18U);
    assert(sizeof(NV_VFIO_VGPU_UNPIN_PAGES_PARAMS) == 0x10U);
    assert(sizeof(NV_VFIO_VGPU_REG_ACCESS_GET_DATA_PARAMS) == 0x18U);
    assert(sizeof(NV0080_ALLOC_PARAMETERS) == 0x38U);
    assert(offsetof(NV0080_ALLOC_PARAMETERS, deviceId) == 0U);
    assert(offsetof(NV0080_ALLOC_PARAMETERS, hClientShare) == 4U);
    assert(sizeof(NV0005_ALLOC_PARAMETERS) == 0x18U);
    assert(sizeof(NVA081_CTRL_VGPU_CONFIG_GET_BAR_INFO_PARAMS) == 0x490U);

    assert(vmiop_err_to_rm_status(0) == 0x00U);
    assert(vmiop_err_to_rm_status(1) == 0x1fU);
    assert(vmiop_err_to_rm_status(2) == 0x1aU);
    assert(vmiop_err_to_rm_status(3) == 0x5bU);
    assert(vmiop_err_to_rm_status(4) == 0x1eU);
    assert(vmiop_err_to_rm_status(5) == 0x57U);
    assert(vmiop_err_to_rm_status(6) == 0x1eU);
    assert(vmiop_err_to_rm_status(7) == 0x65U);
    assert(vmiop_err_to_rm_status(99) == NV_ERR_GENERIC);
    printf("  [PASS] test_abi_and_error_table\n");
}

static void test_memory_buffers_and_conversion(void)
{
    vmiope_reset_all();
    vmiope_set_log_hook(test_log_hook);

    assert(vmiop_get_guest_id_type() == 1U);
    uint64_t ps = 0;
    assert(vmiop_get_page_size(&ps) == 0 && ps == 0x1000ULL);

    uint8_t uuid[VM_UUID_SIZE] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    uint64_t out_u[2] = {0, 0};
    vmiope_set_guest_uuid(uuid);
    assert(vmiop_get_guest_id(NULL) == VMIOP_ERROR_INVAL);
    assert(vmiop_get_guest_id(out_u) == VMIOP_SUCCESS);
    assert(memcmp(out_u, uuid, VM_UUID_SIZE) == 0);

    void *p = NULL;
    assert(vmiop_memory_alloc_internal(64, &p, 1) == 0 && p != NULL);
    assert(vmiop_memory_free_internal(p, 64) == 0);
    assert(vmiop_memory_free_internal(NULL, 64) == 1);

    vmiop_buffer_t *buf = NULL;
    assert(vmiop_buffer_alloc(&buf, 9, 1, 1, 0x38) == 0 && buf != NULL);
    assert(buf->src_class == 9U && buf->dst_class == 1U);
    assert(buf->refcnt == 1U && buf->element_count == 1U);
    assert(buf->release == vmiop_buffer_free);
    assert(buf->elements == (vmiop_iov_t *)((uint8_t *)buf + 0x40));
    assert(buf->elements[0].base == (void *)((uint8_t *)buf + 0x50));
    assert(buf->elements[0].length == 0x38ULL);
    buf->refcnt = 2U;
    assert(vmiop_buffer_free(buf) == 0); /* decrements to 1 */
    assert(vmiop_buffer_free(buf) == 0); /* frees */

    uint64_t uval = 0;
    int64_t ival = -42;
    char sbuf[32];
    assert(vmiop_convert_value(2, "0x1234", 7, 0, &uval, 8) == 0 && uval == 0x1234ULL);
    assert(vmiop_convert_value(2, "077", 4, 0, &uval, 8) == 0 && uval == 63ULL);
    assert(vmiop_convert_value(1, &ival, 8, 0, &uval, 8) == VMIOP_ERROR_NO_MEMORY);
    assert(vmiop_convert_value(1, &ival, 8, 2, sbuf, sizeof(sbuf)) == 0 && strcmp(sbuf, "-42") == 0);
    assert(vmiop_convert_value(2, "hello", 6, 2, sbuf, 3) == VMIOP_ERROR_NO_MEMORY);

    vmiop_log(2, "unit log %d", 7);
    assert(g_last_log_level == 2U && strcmp(g_last_log_lbl, "notice") == 0);
    assert(strstr(g_last_log_msg, "unit log 7") != NULL);
    printf("  [PASS] test_memory_buffers_and_conversion\n");
}

static uint32_t g_thr_seen_handle = 0;
static void test_thread_worker(uint32_t h, void *arg)
{
    uint32_t *ev = (uint32_t *)arg;
    g_thr_seen_handle = h;
    vmiop_thread_event_post(*ev, 1);
}

static void test_sync_and_threads(void)
{
    vmiope_reset_all();
    uint32_t lk = 0, sp = 0, cv = 0, tev = 0, th = 0;
    assert(vmiop_lock_alloc(&lk) == 0 && lk > 0);
    assert(vmiop_lock(lk, 1) == 0);
    assert(vmiop_lock(lk, 1) == VMIOP_ERROR_TIMEOUT);
    assert(vmiop_unlock(lk) == 0);

    assert(vmiop_cv_alloc(&cv) == 0);
    uint64_t now = 0;
    assert(vmiop_thread_get_time(&now) == 0 && now > 0);
    assert(vmiop_lock(lk, 0) == 0);
    assert(vmiop_cv_wait(lk, cv, now + 2000000ULL) == VMIOP_ERROR_TIMEOUT);
    assert(vmiop_unlock(lk) == 0);
    assert(vmiop_cv_signal(cv) == 0);
    assert(vmiop_cv_broadcast(cv) == 0);
    assert(vmiop_cv_free(cv) == 0);
    assert(vmiop_lock_free(lk) == 0);

    assert(vmiop_spinlock_alloc(&sp) == 0);
    assert(vmiop_spin_lock(sp, 1) == 0);
    assert(vmiop_spin_lock(sp, 1) == VMIOP_ERROR_TIMEOUT);
    assert(vmiop_spin_unlock(sp) == 0);
    assert(vmiop_spinlock_free(sp) == 0);

    assert(vmiop_thread_event_alloc(&tev) == 0);
    assert(vmiop_thread_alloc(&tev, test_thread_worker, &th) == 0 && th > 0);
    assert(vmiop_thread_event_wait(tev, UINT64_MAX, 1) == 0);
    assert(vmiop_thread_join(th) == 0);
    assert(g_thr_seen_handle == th);
    assert(vmiop_thread_event_free(tev) == 0);
    printf("  [PASS] test_sync_and_threads\n");
}

static void test_config_plugins_and_presentation(void)
{
    vmiope_reset_all();
    vmiope_set_vfio_ioctl_fn(mock_vfio_ioctl);
    uint8_t uuid[VM_UUID_SIZE] = {0};
    vmiope_init_instance_slot(0, g_mock_vgpu_fd, 0, 0x100, 0x0100U, uuid,
                              "vgpu_type_id=65, frame_rate_limiter=0, max_res=3840x2160");
    vmiope_instance_t *in = vmiope_get_instance(0);

    uint32_t up_h = 0, pres_h = 0;
    assert(vmiope_register_plugin_internal(&g_mock_upstream_plugin, &up_h) == 0);
    in->dev_handle = up_h;
    assert(vmiope_register_plugin_internal(&vmiop_presentation, &pres_h) == 0);
    in->pres_handle = pres_h;
    assert(vmiope_connect_plugins_internal(up_h, pres_h) == 0);

    assert(vmiope_parse_config(in->config_params, 0) == 0);
    char *val = NULL;
    assert(vmiop_config_get(up_h, "frame_rate_limiter", &val) == 0 && strcmp(val, "0") == 0);
    free(val);
    assert(vmiop_config_get(up_h, "missing_key", &val) == VMIOP_ERROR_NOT_FOUND && val == NULL);

    uint32_t fid = 0;
    const vmiop_plugin_t *fp = NULL;
    assert(vmiop_find_plugin(&fid, &fp) == 0 && fid == up_h);
    assert(vmiop_find_plugin(&fid, &fp) == 0 && fid == pres_h);
    assert(vmiop_find_plugin(&fid, &fp) == VMIOP_ERROR_NOT_FOUND);

    vmiope_console_start(0);
    assert(in->console.started == 1 && g_last_msg_op == 7U);

    uint8_t fb[16];
    in->console.buffer = fb;
    in->console.buffer_size = sizeof(fb);

    vmiop_buffer_t *msg = NULL;
    assert(vmiop_buffer_alloc(&msg, 1, 9, 1, 0x48) == 0);
    uint8_t *mp = (uint8_t *)msg->elements[0].base;
    *(uint32_t *)(mp + 0x08) = 0x24U;
    *(uint32_t *)(mp + 0x0c) = 1U;
    *(uint32_t *)(mp + 0x18) = 4U; /* config update */
    *(uint32_t *)(mp + 0x1c) = 0x14U;
    *(uint32_t *)(mp + 0x20) = 1U;
    *(uint32_t *)(mp + 0x24 + 0x00) = 1U;  /* surface_id */
    *(uint32_t *)(mp + 0x24 + 0x08) = 2U;  /* width */
    *(uint32_t *)(mp + 0x24 + 0x0c) = 2U;  /* height */
    *(uint32_t *)(mp + 0x24 + 0x10) = 4U;  /* format -> 32 bpp */
    assert(vmiop_deliver_message(up_h, msg, 0) == 0);
    assert(in->console.width == 2U && in->console.height == 2U && in->console.bpp == 32U);
    assert(g_last_msg_op == 3U); /* triggered config request on first delivery */
    vmiop_buffer_free(msg);

    vmiope_console_stop(0);
    assert(in->console.started == 0 && g_last_msg_op == 7U);
    in->console.buffer = NULL;
    printf("  [PASS] test_config_plugins_and_presentation\n");
}

static void test_emul_regions_pin_map_and_rm(void)
{
    vmiope_reset_all();
    rm_reset_sys_ops();
    vmiope_set_vfio_ioctl_fn(mock_vfio_ioctl);

    rm_sys_ops_t ops = {
        .open_fn = mock_open,
        .close_fn = mock_close,
        .ioctl_fn = mock_rm_ioctl,
        .mmap64_fn = mock_mmap64,
        .munmap_fn = mock_munmap,
    };
    rm_set_sys_ops(&ops);

    g_mock_inject_count = 0;
    g_mock_pin_calls = 0;
    g_mock_unpin_calls = 0;
    g_mock_mmio_adds = 0;
    g_mock_mmio_removes = 0;
    g_mock_munmap_calls = 0;
    g_mock_xfer_calls = 0;

    uint8_t uuid[VM_UUID_SIZE] = {0};
    vmiope_init_instance_slot(0, g_mock_vgpu_fd, 0, 0x100, 0x0100U, uuid, "vgpu_type_id=65");
    vmiope_instance_t *in = vmiope_get_instance(0);
    in->dev_handle = 10U;
    assert(rm_alloc_client(&in->hClient) == NV_OK);
    in->hDevice = 0xde000001U;
    NV0080_ALLOC_PARAMETERS devp;
    memset(&devp, 0, sizeof(devp));
    devp.deviceId = 0;
    devp.hClientShare = in->hClient;
    assert(rm_alloc_object(in->hClient, in->hClient, in->hDevice,
                           NV01_DEVICE_0, &devp, sizeof(devp)) == NV_OK);

    uint32_t emul_h = 0;
    assert(vmiop_register_emul_device_v2(10U, in, (void *)(uintptr_t)mock_emul_cb,
                                         (void *)(uintptr_t)0x1, &emul_h) == 0);
    uint8_t wdata[4] = {0x11, 0x22, 0x33, 0x44};
    assert(vmiope_dispatch_emul_space(0, VGPU_EMUL_CONFIG_SPACE, 4, 4, 1, wdata) == 0);
    assert(g_last_emul_write_val == 0x44332211U);
    uint8_t rdata[4] = {0};
    g_emul_read_fail = 1;
    assert(vmiope_dispatch_emul_space(0, VGPU_EMUL_CONFIG_SPACE, 4, 4, 0, rdata) == 0);
    assert(memcmp(rdata, "\xff\xff\xff\xff", 4) == 0);
    g_emul_read_fail = 0;

    uint32_t reg_id = 0;
    assert(vmiop_create_guest_region_v2(10U, 0x1000, (void *)(uintptr_t)mock_emul_cb,
                                        in, VGPU_EMUL_MMIO, &reg_id) == 0);
    assert(vmiop_relocate_guest_region(reg_id, 0xf0000000ULL) == 0);
    assert(vmiope_dispatch_emul_space(0, VGPU_EMUL_MMIO, 0xf0000008ULL, 2, 1, wdata) == 0);
    assert(g_emul_backing[8] == 0x11 && g_emul_backing[9] == 0x22);
    assert(vmiop_relocate_guest_region(reg_id, UINT64_MAX) == 0);

    assert(vmiop_map_guest_region(10U, 0xfe000000ULL, 0, 0x1000, 0xc0000000ULL, 1) == 0);
    assert(vmiop_map_guest_region(10U, 0xfe000000ULL, 0, 0x1000, 0xc0000000ULL, 0) == 0);
    assert(g_mock_mmio_adds == 1 && g_mock_mmio_removes == 1);

    uint64_t gpfns[3] = {0x100, 0x101, 0x102}, hpfns[3] = {0};
    assert(vmiop_pin_guest_pages_v2(10U, gpfns, hpfns, 3) == 0);
    assert(hpfns[0] == 0x100100ULL && g_mock_pin_calls == 1);

    void *hva = NULL;
    uint32_t mem_id = 0;
    assert(vmiop_map_guest_memory_v2(10U, 0x200000ULL, 0x2000ULL, &hva, 3, &mem_id) == 0);
    assert(hva == g_mock_mmap_ptr && mem_id != 0);
    assert(vmiop_unmap_guest_memory(mem_id) == 0);
    assert(g_mock_munmap_calls == 1 && g_mock_unpin_calls == 1);

    assert(vmiop_control_interrupt(10U, 0, 1) == 0 && g_mock_inject_count == 1);
    uint32_t cfg_val = 0;
    assert(vmiop_unhandled_config_access(10U, 0, 0, 4, &cfg_val) == 0 && cfg_val == 0x1bb310deU);

    assert(vmiop_unregister_emul_device(emul_h) == 0);

    /* Exercise 4960-byte RM control payload (GET_VGPU_TYPE_INFO) */
    NVA081_CTRL_VGPU_CONFIG_GET_VGPU_TYPE_INFO_PARAMS big;
    memset(&big, 0, sizeof(big));
    assert(rm_control(in->hClient, in->hSubDevice, 0xa0810103U, &big, sizeof(big)) == NV_OK);
    assert(g_mock_xfer_calls == 1);

    /* Temporary proc/devices + nvidia/params parser test */
    char pdev[] = "/tmp/test_proc_dev_XXXXXX";
    char pprm[] = "/tmp/test_nv_params_XXXXXX";
    int fd1 = mkstemp(pdev);
    int fd2 = mkstemp(pprm);
    assert(fd1 >= 0 && fd2 >= 0);
    FILE *f1 = fdopen(fd1, "w");
    FILE *f2 = fdopen(fd2, "w");
    fprintf(f1, "Character devices:\n 236 nvidia-vgpu-vfio\n 195 nvidia-frontend\n");
    fprintf(f2, "DeviceFileUID: 0\nDeviceFileGID: 44\nDeviceFileMode: 432\nModifyDeviceFiles: 1\n");
    fclose(f1);
    fclose(f2);
    rm_set_proc_paths(pdev, pprm);
    assert(rm_get_chardev_major("nvidia-vgpu-vfio") == 236);
    unsigned int uid, gid, mode;
    int mod;
    rm_read_params_file(pprm, &uid, &gid, &mode, &mod);
    assert(uid == 0 && gid == 44 && mode == 432 && mod == 1);
    rm_set_proc_paths("/proc/devices", "/proc/driver/nvidia/params");
    unlink(pdev);
    unlink(pprm);

    rm_reset_sys_ops();
    printf("  [PASS] test_emul_regions_pin_map_and_rm\n");
}

static uint8_t *g_vrpc_test_page = NULL;

static uint32_t mock_vrpc_doorbell_cb(void *priv, uint32_t is_write, uint32_t space,
                                      uint64_t off, uint32_t width, void *data, uint32_t *dummy)
{
    (void)priv; (void)space; (void)width;
    if (dummy) *dummy = 0;
    if (is_write && (off & 0xffffffULL) == 0x8f050ULL && g_vrpc_test_page != NULL) {
        uint32_t ok = 0U;
        memcpy(g_vrpc_test_page + 0x10, &ok, 4U);
        memcpy(g_vrpc_test_page + 0x20, "\xaa\xbb\xcc\xdd", 4U);
    } else if (!is_write) {
        uint32_t val = 0x1bb310deU;
        memcpy(data, &val, 4U);
    }
    return 0U;
}

static void test_trace_tap_and_vrpc_capture(void)
{
    char dir_tmpl[] = "/tmp/test_vgpu_trace_XXXXXX";
    char *tdir = mkdtemp(dir_tmpl);
    assert(tdir != NULL);

    vmiope_reset_all();
    setenv("OPEN_VGPU_TRACE_DIR", tdir, 1);

    uint8_t vrpc_page[0x1000];
    memset(vrpc_page, 0, sizeof(vrpc_page));
    uint32_t ver = 0x03000000U, sig = 0x43505256U, len = 0x28U, fn = 0x1aU;
    uint32_t pending = 0xffffffffU, seq = 7U;
    memcpy(vrpc_page + 0x00, &ver, 4U);
    memcpy(vrpc_page + 0x04, &sig, 4U);
    memcpy(vrpc_page + 0x08, &len, 4U);
    memcpy(vrpc_page + 0x0c, &fn, 4U);
    memcpy(vrpc_page + 0x10, &pending, 4U);
    memcpy(vrpc_page + 0x18, &seq, 4U);
    memcpy(vrpc_page + 0x20, "\x11\x22\x33\x44", 4U);
    g_vrpc_test_page = vrpc_page;

    void *saved_mmap_ptr = g_mock_mmap_ptr;
    g_mock_mmap_ptr = vrpc_page;

    rm_sys_ops_t ops = {
        .open_fn = mock_open,
        .close_fn = mock_close,
        .ioctl_fn = mock_rm_ioctl,
        .mmap64_fn = mock_mmap64,
        .munmap_fn = mock_munmap,
    };
    rm_set_sys_ops(&ops);
    vmiope_set_vfio_ioctl_fn(mock_vfio_ioctl);

    uint8_t uuid[VM_UUID_SIZE] = {0};
    vmiope_init_instance_slot(0, g_mock_vgpu_fd, 0, 0x100, 0x0100U, uuid, "vgpu_type_id=65");
    vmiope_instance_t *in = vmiope_get_instance(0);
    in->dev_handle = 11U;
    assert(rm_alloc_client(&in->hClient) == NV_OK);
    in->hDevice = 0xde000002U;
    NV0080_ALLOC_PARAMETERS devp;
    memset(&devp, 0, sizeof(devp));
    assert(rm_alloc_object(in->hClient, in->hClient, in->hDevice,
                           NV01_DEVICE_0, &devp, sizeof(devp)) == NV_OK);

    uint32_t emul_h = 0, reg_id = 0, mem_id = 0;
    void *hva = NULL;
    assert(vmiop_register_emul_device_v2(11U, in, (void *)(uintptr_t)mock_vrpc_doorbell_cb,
                                         "pci_cfg", &emul_h) == 0);
    assert(vmiop_create_guest_region_v2(11U, 0x1000000ULL, (void *)(uintptr_t)mock_vrpc_doorbell_cb,
                                        in, VGPU_EMUL_MMIO, &reg_id) == 0);
    assert(vmiop_relocate_guest_region(reg_id, 0xc0000000ULL) == 0);
    assert(vmiop_map_guest_memory_v2(11U, 0x200000ULL, 0x1000ULL, &hva, 3U, &mem_id) == 0);
    assert(hva == vrpc_page);

    uint8_t cfg_val[4] = {0};
    assert(vmiope_dispatch_emul_space(0, VGPU_EMUL_CONFIG_SPACE, 0ULL, 4U, 0, cfg_val) == 0);
    uint8_t bell[4] = {1, 0, 0, 0};
    assert(vmiope_dispatch_emul_space(0, VGPU_EMUL_MMIO, 0xc008f050ULL, 4U, 1, bell) == 0);

    assert(vmiop_unmap_guest_memory(mem_id) == 0);
    vmiope_reset_all();
    unsetenv("OPEN_VGPU_TRACE_DIR");
    g_mock_mmap_ptr = saved_mmap_ptr;
    g_vrpc_test_page = NULL;
    rm_reset_sys_ops();

    char tpath[512];
    snprintf(tpath, sizeof(tpath), "%s/vgpu_trace_pid%ld.jsonl", tdir, (long)getpid());
    FILE *fp = fopen(tpath, "r");
    assert(fp != NULL);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1U, fp);
    buf[n] = '\0';
    fclose(fp);
    assert(strstr(buf, "\"ev\":\"pci_cfg\"") != NULL);
    assert(strstr(buf, "\"ev\":\"mmio\"") != NULL);
    assert(strstr(buf, "\"ev\":\"vrpc_req\"") != NULL);
    assert(strstr(buf, "\"ev\":\"vrpc_rep\"") != NULL);
    assert(strstr(buf, "\"func\":\"0x1a\"") != NULL);
    assert(strstr(buf, "\"status\":\"0x0\"") != NULL);
    assert(strstr(buf, "\"ev\":\"inst_summary\"") != NULL);
    unlink(tpath);
    rmdir(tdir);
    printf("  [PASS] test_trace_tap_and_vrpc_capture\n");
}

int main(void)
{
    printf("Running open-nvidia-vgpu-mgr unit tests...\n");
    test_abi_and_error_table();
    test_memory_buffers_and_conversion();
    test_sync_and_threads();
    test_config_plugins_and_presentation();
    test_emul_regions_pin_map_and_rm();
    test_trace_tap_and_vrpc_capture();
    printf("ALL open-nvidia-vgpu-mgr UNIT TESTS PASSED!\n");
    return 0;
}
