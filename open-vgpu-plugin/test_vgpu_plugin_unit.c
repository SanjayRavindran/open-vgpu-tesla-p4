#include "open_vgpu_plugin.h"

#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Mock Host VMIOP Environment Exports (resolved via -rdynamic by plugin .so)
 * ------------------------------------------------------------------------- */
static void *g_cfg_priv = NULL;
static vmiop_emul_access_fn_t g_cfg_cb = NULL;

typedef struct {
    uint64_t start;
    uint64_t len;
    void    *priv;
    vmiop_emul_access_fn_t cb;
} mock_region_t;

static mock_region_t g_regions[16];
static uint32_t g_region_cnt = 0U;
static uint8_t  g_guest_pages[4][0x1000];
static uint64_t g_guest_gpas[4] = { 0x100000ULL, 0x101000ULL, 0x102000ULL, 0x103000ULL };
static uint32_t g_msi_count = 0U;
static uint32_t g_last_mig_size_calls = 0U;

uint32_t vmiop_log(uint32_t level, const char *fmt, ...)
{
    (void)level; (void)fmt;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_config_get(uint32_t dev_handle, const char *key, char **out_val)
{
    (void)dev_handle;
    if (!out_val || !key) return VMIOP_ERROR_INVAL;
    if (strcmp(key, "frame_rate_limiter") == 0) {
        *out_val = strdup("1");
        return VMIOP_SUCCESS;
    }
    *out_val = NULL;
    return VMIOP_ERROR_NOT_FOUND;
}

uint32_t vmiop_register_emul_device_v2(uint32_t handle_in, void *priv, void *access_cb,
                                       void *tag, uint32_t *handle_out)
{
    (void)tag;
    g_cfg_priv = priv;
    g_cfg_cb   = (vmiop_emul_access_fn_t)access_cb;
    if (handle_out) *handle_out = handle_in;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_unregister_emul_device(uint32_t handle)
{
    (void)handle;
    g_cfg_priv = NULL;
    g_cfg_cb   = NULL;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_create_guest_region_v2(uint32_t dev_handle, uint64_t length, void *access_cb,
                                      void *priv, int space, uint32_t *out_id)
{
    (void)dev_handle; (void)space;
    if (g_region_cnt >= 16U || !out_id) return VMIOP_ERROR_NO_MEMORY;
    uint32_t idx = g_region_cnt++;
    g_regions[idx].start = UINT64_MAX;
    g_regions[idx].len   = length;
    g_regions[idx].priv  = priv;
    g_regions[idx].cb    = (vmiop_emul_access_fn_t)access_cb;
    *out_id = idx + 1U;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_relocate_guest_region(uint32_t region_id, uint64_t start)
{
    if (region_id == 0U || region_id > g_region_cnt) return VMIOP_ERROR_NOT_FOUND;
    g_regions[region_id - 1U].start = start;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_map_guest_memory_v2(uint32_t dev_handle, uint64_t gpa, uint64_t size,
                                   void **out_hva, uint32_t prot_flags, uint32_t *out_id)
{
    (void)dev_handle; (void)size; (void)prot_flags;
    for (uint32_t i = 0; i < 4U; i++) {
        if (g_guest_gpas[i] == gpa) {
            *out_hva = g_guest_pages[i];
            *out_id  = i + 1U;
            return VMIOP_SUCCESS;
        }
    }
    return VMIOP_ERROR_NOT_FOUND;
}

uint32_t vmiop_unmap_guest_memory(uint32_t mem_id)
{
    (void)mem_id;
    return VMIOP_SUCCESS;
}

static uint32_t host_inject_msi(uint64_t h, uint64_t a, uint32_t b)
{
    (void)h; (void)a; (void)b;
    g_msi_count++;
    return VMIOP_SUCCESS;
}
static uint32_t host_update_lfb(void) { return VMIOP_SUCCESS; }
static uint32_t host_unpin_pages(uint64_t h, uint64_t *p, uint64_t u, uint32_t c)
{
    (void)h; (void)p; (void)u; (void)c;
    return VMIOP_SUCCESS;
}
static uint32_t host_guest_id_type(void) { return 1U; }

/* -------------------------------------------------------------------------
 * Unit Tests: Standalone GP104 + Hybrid Trace-Interposer + ABI Layout
 * ------------------------------------------------------------------------- */
static void w32(uint8_t *p, size_t off, uint32_t v)
{
    p[off] = (uint8_t)(v & 0xffU);
    p[off + 1] = (uint8_t)((v >> 8) & 0xffU);
    p[off + 2] = (uint8_t)((v >> 16) & 0xffU);
    p[off + 3] = (uint8_t)((v >> 24) & 0xffU);
}

static uint32_t r32(const uint8_t *p, size_t off)
{
    return (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
           ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
}

static void bind_env(vmiop_env_v2_t *env)
{
    env->inject_msi        = host_inject_msi;
    env->update_lfb        = host_update_lfb;
    env->unpin_pages       = host_unpin_pages;
    env->get_guest_id_type = host_guest_id_type;
}

static void test_standalone_gp104_end_to_end(void)
{
    g_region_cnt = 0U;
    g_msi_count  = 0U;
    memset(g_guest_pages, 0, sizeof(g_guest_pages));
    setenv("OPEN_VGPU_PLUGIN_MODE", "standalone", 1);
    unsetenv("OPEN_VGPU_TRACE_DIR");

    void *dl = dlopen("./libopen-nvidia-vgpu.so", RTLD_NOW);
    assert(dl != NULL);

    const vmiop_plugin_t *plug =
        (const vmiop_plugin_t *)dlvsym(dl, "vmiop_display_vmiop_plugin", "VER_5_0");
    assert(plug != NULL);
    assert(plug->length == 0xa8U);
    assert(plug->version == 0x50000U);
    assert(strcmp(plug->signature, VMIOP_PLUGIN_SIGNATURE) == 0);
    assert(plug->plugin_class == 1U && plug->input_classes == 0x202U);

    vmiop_env_v2_t *env = (vmiop_env_v2_t *)dlvsym(dl, "vmiop_display_vmiop_env", "VER_2_0");
    assert(env != NULL);
    bind_env(env);

    uint32_t dev_h = 1U;
    uint64_t type64 = 65ULL; /* GRID P4-4Q */
    assert(plug->set_attribute(dev_h, "vgpu_type_id", 0U, &type64, 8U) == VMIOP_SUCCESS);
    char bdf[64] = "0000:01:00.0";
    assert(plug->set_attribute(dev_h, "gpu-pci-id", 2U, bdf, sizeof(bdf)) == VMIOP_SUCCESS);
    static uint8_t uuid[16] = { 0x11, 0x11, 0x11, 0x11 };
    const uint8_t *uuid_p = uuid;
    assert(plug->set_attribute(dev_h, "vgpu-dev-name", 3U, &uuid_p, 0x10U) == VMIOP_SUCCESS);

    uint64_t cap = 0ULL;
    assert(plug->get_attribute(dev_h, "vmiop_vgpu_cap", 0U, &cap, 8U) == VMIOP_SUCCESS);
    assert((cap & 1ULL) == 1ULL);

    assert(plug->init_routine(dev_h) == VMIOP_SUCCESS);
    assert(g_cfg_cb != NULL && g_region_cnt == 3U);

    /* PCI config identity + live vendor cap + BAR0 sizing & relocation */
    uint32_t val = 0U, dummy = 0U;
    assert(g_cfg_cb(g_cfg_priv, 0U, VGPU_EMUL_CONFIG_SPACE, 0x00ULL, 4U, &val, &dummy) == 0U);
    assert(val == 0x1bb310deU);
    assert(g_cfg_cb(g_cfg_priv, 0U, VGPU_EMUL_CONFIG_SPACE, 0x08ULL, 4U, &val, &dummy) == 0U);
    assert(val == 0x030000a1U);
    assert(g_cfg_cb(g_cfg_priv, 0U, VGPU_EMUL_CONFIG_SPACE, 0x2cULL, 4U, &val, &dummy) == 0U);
    assert(val == 0x120610deU); /* GRID P4-4Q subsys */
    assert(g_cfg_cb(g_cfg_priv, 0U, VGPU_EMUL_CONFIG_SPACE, 0xd0ULL, 4U, &val, &dummy) == 0U);
    assert(val == 0x561b6809U);

    uint32_t probe = 0xffffffffU;
    assert(g_cfg_cb(g_cfg_priv, 1U, VGPU_EMUL_CONFIG_SPACE, 0x10ULL, 4U, &probe, &dummy) == 0U);
    assert(g_cfg_cb(g_cfg_priv, 0U, VGPU_EMUL_CONFIG_SPACE, 0x10ULL, 4U, &val, &dummy) == 0U);
    assert(val == 0xff000000U); /* 16 MiB BAR0 */

    uint32_t bar0_base = 0xc0000000U;
    assert(g_cfg_cb(g_cfg_priv, 1U, VGPU_EMUL_CONFIG_SPACE, 0x10ULL, 4U, &bar0_base, &dummy) == 0U);
    assert(g_regions[0].start == 0xc0000000ULL);

    /* BAR0 NV_PMC_BOOT_0 (both relative & relocated 0xc0000000 GPA), NV_PFB_CSTATUS, and 0x088068/0x8f020/0x8f080 */
    vmiop_emul_access_fn_t bar0_cb = g_regions[0].cb;
    void *bar0_priv = g_regions[0].priv;
    assert(bar0_cb(bar0_priv, 0U, VGPU_EMUL_MMIO, 0x000000ULL, 4U, &val, &dummy) == 0U);
    assert(val == 0x134000a1U);
    val = 0U;
    assert(bar0_cb(bar0_priv, 0U, VGPU_EMUL_MMIO, 0xc0000000ULL, 4U, &val, &dummy) == 0U);
    assert(val == 0x134000a1U);
    assert(bar0_cb(bar0_priv, 0U, VGPU_EMUL_MMIO, 0x10020cULL, 4U, &val, &dummy) == 0U);
    assert(val == 0xec0U); /* 3776 MiB usable FB for P4-4Q */
    assert(bar0_cb(bar0_priv, 0U, VGPU_EMUL_MMIO, 0x088068ULL, 4U, &val, &dummy) == 0U);
    assert(val == 0x00810005U);
    assert(bar0_cb(bar0_priv, 0U, VGPU_EMUL_MMIO, 0x8f020ULL, 4U, &val, &dummy) == 0U);
    assert(val == 0x040882f1U);
    uint32_t ring2_lo = 0xffff600bU;
    assert(bar0_cb(bar0_priv, 1U, VGPU_EMUL_MMIO, 0x8f080ULL, 4U, &ring2_lo, &dummy) == 0U);
    val = 0U;
    assert(bar0_cb(bar0_priv, 0U, VGPU_EMUL_MMIO, 0x8f080ULL, 4U, &val, &dummy) == 0U);
    assert(val == ring2_lo);

    /* Program SEND_RING (0x100000) & RECV_RING (0x101000); SEND_RING points to MSG_PAGE (0x102000) */
    w32(g_guest_pages[0], 0x00, (uint32_t)(g_guest_gpas[2] | 8U));
    w32(g_guest_pages[0], 0x04, 0U);
    uint32_t rlo = (uint32_t)(g_guest_gpas[0] | 0x808U), rhi = 0U;
    assert(bar0_cb(bar0_priv, 1U, VGPU_EMUL_MMIO, 0x8f000ULL, 4U, &rlo, &dummy) == 0U);
    assert(bar0_cb(bar0_priv, 1U, VGPU_EMUL_MMIO, 0x8f004ULL, 4U, &rhi, &dummy) == 0U);
    rlo = (uint32_t)(g_guest_gpas[1] | 0x808U);
    assert(bar0_cb(bar0_priv, 1U, VGPU_EMUL_MMIO, 0x8f010ULL, 4U, &rlo, &dummy) == 0U);
    assert(bar0_cb(bar0_priv, 1U, VGPU_EMUL_MMIO, 0x8f014ULL, 4U, &rhi, &dummy) == 0U);

    /* 1) VRPC GET_STATIC_INFO before SET_GUEST_SYSTEM_INFO -> 0x56 */
    uint8_t *msg = g_guest_pages[2];
    w32(msg, 0x00, OPEN_VRPC_HEADER_VERSION);
    w32(msg, 0x04, OPEN_VRPC_SIGNATURE);
    w32(msg, 0x08, 0x40U);
    w32(msg, 0x0c, VRPC_FN_GET_STATIC_INFO);
    w32(msg, 0x10, OPEN_VRPC_PENDING);
    uint32_t bell = 1U;
    assert(bar0_cb(bar0_priv, 1U, VGPU_EMUL_MMIO, 0x8f050ULL, 4U, &bell, &dummy) == 0U);
    assert(r32(msg, 0x10) == 0x56U);
    assert(g_msi_count == 1U);

    /* 2) SET_GUEST_SYSTEM_INFO -> 0x00 */
    w32(msg, 0x0c, VRPC_FN_SET_GUEST_SYSTEM_INFO);
    w32(msg, 0x10, OPEN_VRPC_PENDING);
    assert(bar0_cb(bar0_priv, 1U, VGPU_EMUL_MMIO, 0x8f050ULL, 4U, &bell, &dummy) == 0U);
    assert(r32(msg, 0x10) == 0U);

    /* 3) GET_STATIC_INFO -> 0x00 + populates FB/subsys */
    w32(msg, 0x0c, VRPC_FN_GET_STATIC_INFO);
    w32(msg, 0x10, OPEN_VRPC_PENDING);
    assert(bar0_cb(bar0_priv, 1U, VGPU_EMUL_MMIO, 0x8f050ULL, 4U, &bell, &dummy) == 0U);
    assert(r32(msg, 0x10) == 0U);
    assert(r32(msg, 0x20) == 0xec000000U);

    /* 4) Live-observed top VRPC opcodes (0x1b, 0x38, 0x40, 0x50, 0x9c) -> 0x00 */
    static const uint32_t live_ops[] = {
        VRPC_FN_OP_1B, VRPC_FN_OP_38, VRPC_FN_SET_GUEST_BRANCH_VER,
        VRPC_FN_BATCH_50, VRPC_FN_SYNC_9C
    };
    for (size_t i = 0; i < sizeof(live_ops) / sizeof(live_ops[0]); i++) {
        w32(msg, 0x0c, live_ops[i]);
        w32(msg, 0x10, OPEN_VRPC_PENDING);
        assert(bar0_cb(bar0_priv, 1U, VGPU_EMUL_MMIO, 0x8f050ULL, 4U, &bell, &dummy) == 0U);
        assert(r32(msg, 0x10) == 0U);
    }

    assert(plug->reset_device(dev_h) == VMIOP_SUCCESS);
    assert(plug->shutdown_routine(dev_h) == VMIOP_SUCCESS);
    dlclose(dl);
    printf("  [PASS] test_standalone_gp104_end_to_end\n");
}

static void test_hybrid_interposer_and_trace(void)
{
    g_msi_count = 0U;
    g_last_mig_size_calls = 0U;

    char dir_tmpl[] = "/tmp/test_plug_trace_XXXXXX";
    char *tdir = mkdtemp(dir_tmpl);
    assert(tdir != NULL);

    setenv("OPEN_VGPU_PLUGIN_MODE", "hybrid", 1);
    setenv("OPEN_VGPU_VENDOR_LIB", "./mock_vendor_vgpu.so", 1);
    setenv("OPEN_VGPU_TRACE_DIR", tdir, 1);

    void *dl = dlopen("./libopen-nvidia-vgpu.so", RTLD_NOW);
    assert(dl != NULL);

    const vmiop_plugin_t *plug =
        (const vmiop_plugin_t *)dlvsym(dl, "vmiop_display_vmiop_plugin", "VER_5_0");
    vmiop_env_v2_t *env = (vmiop_env_v2_t *)dlvsym(dl, "vmiop_display_vmiop_env", "VER_2_0");
    assert(plug != NULL && env != NULL);
    bind_env(env);

    uint32_t dev_h = 2U;
    uint64_t type64 = 66ULL;
    assert(plug->set_attribute(dev_h, "vgpu_type_id", 0U, &type64, 8U) == VMIOP_SUCCESS);

    uint64_t cap = 0ULL;
    assert(plug->get_attribute(dev_h, "vmiop_vgpu_cap", 0U, &cap, 8U) == VMIOP_SUCCESS);
    assert(cap == 1ULL);
    uint64_t one = 1ULL;
    assert(plug->set_attribute(dev_h, "vmiop_vmm_migration_supported", 0U, &one, 8U) == VMIOP_SUCCESS);

    /* mock_vendor_vgpu.so verifies inject_msi was propagated through VER_2_0 table */
    assert(plug->init_routine(dev_h) == VMIOP_SUCCESS);
    assert(g_msi_count == 1U);

    uint32_t got_type = 0U;
    assert(plug->get_attribute(dev_h, "mv_type_id", 0U, &got_type, 4U) == VMIOP_SUCCESS);
    assert(got_type == 66U);
    assert(plug->reset_device(dev_h) == VMIOP_SUCCESS);

    uint64_t mig_sz = 0ULL;
    assert(plug->get_migration_data_size(dev_h, &mig_sz) == VMIOP_SUCCESS);
    assert(mig_sz == 4096ULL);
    assert(plug->shutdown_routine(dev_h) == VMIOP_SUCCESS);

    char tpath[512];
    snprintf(tpath, sizeof(tpath), "%s/vgpu_plugin_pid%ld.jsonl", tdir, (long)getpid());
    FILE *fp = fopen(tpath, "r");
    assert(fp != NULL);
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1U, fp);
    buf[n] = '\0';
    fclose(fp);
    assert(strstr(buf, "\"ev\":\"plug_init\"") != NULL);
    assert(strstr(buf, "\"mode\":\"hybrid\"") != NULL);
    unlink(tpath);
    rmdir(tdir);

    unsetenv("OPEN_VGPU_TRACE_DIR");
    unsetenv("OPEN_VGPU_VENDOR_LIB");
    unsetenv("OPEN_VGPU_PLUGIN_MODE");
    dlclose(dl);
    printf("  [PASS] test_hybrid_interposer_and_trace\n");
}

/* -------------------------------------------------------------------------
 * Mock Host RM Bridge Tests (/dev/nvidiactl + /dev/nvidia0 + NVA081/NVA082)
 * ------------------------------------------------------------------------- */
static struct {
    int open_fds;
    int saw_ver_check;
    int saw_card_info;
    int saw_reg_fd;
    int saw_nva081;
    int saw_nva082;
    int saw_xfer;
    uint32_t next_host_client;
    uint32_t last_ctrl_cmd;
    uint32_t last_lic_state;
    uint64_t last_fb_usage;
    uint32_t ctrl_calls;
    uint32_t free_calls;
    uint32_t dup_calls;
} g_mock_rm;

static int mock_rm_open(const char *path, int flags)
{
    (void)flags;
    if (strcmp(path, "/dev/nvidiactl") == 0) {
        g_mock_rm.open_fds++;
        return 100;
    }
    if (strcmp(path, "/dev/nvidia0") == 0) {
        g_mock_rm.open_fds++;
        return 101;
    }
    return -1;
}

static int mock_rm_close(int fd)
{
    if (fd == 100 || fd == 101) {
        g_mock_rm.open_fds--;
    }
    return 0;
}

static int mock_rm_ioctl(int fd, unsigned long req, void *arg)
{
    (void)fd;
    uint32_t nr = (uint32_t)_IOC_NR(req);
    if (nr == NV_ESC_CHECK_VERSION_STR) {
        g_mock_rm.saw_ver_check++;
        return 0;
    }
    if (nr == NV_ESC_CARD_INFO) {
        nv_ioctl_card_info_t *cards = (nv_ioctl_card_info_t *)arg;
        g_mock_rm.saw_card_info++;
        cards[0].valid        = 1U;
        cards[0].domain       = 0U;
        cards[0].bus          = 1U;
        cards[0].slot         = 0U;
        cards[0].function     = 0U;
        cards[0].vendor_id    = 0x10deU;
        cards[0].device_id    = 0x1bb3U;
        cards[0].gpu_id       = 0x100U;
        cards[0].minor_number = 0U;
        return 0;
    }
    if (nr == NV_ESC_REGISTER_FD) {
        g_mock_rm.saw_reg_fd++;
        return 0;
    }
    if (nr == NV_ESC_IOCTL_XFER_CMD) {
        nv_ioctl_xfer_t *x = (nv_ioctl_xfer_t *)arg;
        g_mock_rm.saw_xfer++;
        (void)x;
        return 0;
    }
    if (nr == NV_ESC_RM_ALLOC) {
        uint32_t sz = (uint32_t)_IOC_SIZE(req);
        if (sz == sizeof(NVOS21_ROOT_PARAMETERS)) {
            NVOS21_ROOT_PARAMETERS *p = (NVOS21_ROOT_PARAMETERS *)arg;
            p->hObjectNew = g_mock_rm.next_host_client++;
            p->status     = NV_OK;
            return 0;
        }
        NVOS64_PARAMETERS *p = (NVOS64_PARAMETERS *)arg;
        if (p->hClass == NVA081_VGPU_CONFIG) g_mock_rm.saw_nva081++;
        if (p->hClass == NVA082_HOST_VGPU_DEVICE) {
            assert(p->paramsSize == 0x70U);
            NVA082_ALLOC_PARAMETERS *ap = (NVA082_ALLOC_PARAMETERS *)(uintptr_t)p->pAllocParms;
            assert(ap->vgpuType == 65U);
            assert(ap->vmIdType == 1U);
            g_mock_rm.saw_nva082++;
        }
        p->status = NV_OK;
        return 0;
    }
    if (nr == NV_ESC_RM_FREE) {
        NVOS00_PARAMETERS *p = (NVOS00_PARAMETERS *)arg;
        g_mock_rm.free_calls++;
        p->status = NV_OK;
        return 0;
    }
    if (nr == NV_ESC_RM_DUP_OBJECT) {
        NVOS55_DUP_PARAMETERS *p = (NVOS55_DUP_PARAMETERS *)arg;
        g_mock_rm.dup_calls++;
        p->status = NV_OK;
        return 0;
    }
    if (nr == NV_ESC_RM_CONTROL) {
        NVOS54_PARAMETERS *p = (NVOS54_PARAMETERS *)arg;
        void *prm = (void *)(uintptr_t)p->params;
        g_mock_rm.ctrl_calls++;
        g_mock_rm.last_ctrl_cmd = p->cmd;
        if (p->cmd == NV0000_CTRL_CMD_GPU_GET_ID_INFO) {
            NV0000_CTRL_GPU_GET_ID_INFO_PARAMS *id = (NV0000_CTRL_GPU_GET_ID_INFO_PARAMS *)prm;
            id->deviceInstance = 0U;
        } else if (p->cmd == NV2080_CTRL_CMD_BUS_GET_PCI_INFO) {
            uint32_t *ids = (uint32_t *)prm;
            ids[0] = 0x1bb310deU;
            ids[1] = 0x11d810deU;
        } else if (p->cmd == NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE) {
            *(uint32_t *)prm = 3U; /* HOST_VGPU */
        } else if (p->cmd == NVA081_CTRL_CMD_VGPU_CONFIG_GET_VGPU_TYPE_INFO) {
            NVA081_CTRL_VGPU_CONFIG_GET_VGPU_TYPE_INFO_PARAMS *ti =
                (NVA081_CTRL_VGPU_CONFIG_GET_VGPU_TYPE_INFO_PARAMS *)prm;
            snprintf(ti->vgpuName, sizeof(ti->vgpuName), "GRID P4-4Q");
            ti->fbLength  = 0x100000000ULL;
            ti->frlConfig = 60U;
            uint64_t usable = 0xec000000ULL;
            memcpy(&ti->tail[0], &usable, sizeof(usable));
        } else if (p->cmd == NVA082_CTRL_CMD_GET_P2P_CAP) {
            *(uint8_t *)prm = 1U;
        } else if (p->cmd == NVA082_CTRL_CMD_GET_ENCODER_CAPACITY) {
            *(uint32_t *)prm = 85U;
        } else if (p->cmd == NVA082_CTRL_CMD_SET_LICENSE_STATE) {
            g_mock_rm.last_lic_state = *(const uint8_t *)prm;
        } else if (p->cmd == NVA082_CTRL_CMD_SET_FB_USAGE) {
            g_mock_rm.last_fb_usage = *(const uint64_t *)prm;
        }
        p->status = NV_OK;
        return 0;
    }
    return -1;
}

static void init_vrpc(uint8_t *buf, uint32_t fn, uint32_t tot)
{
    memset(buf, 0, tot);
    w32(buf, 0x00, OPEN_VRPC_HEADER_VERSION);
    w32(buf, 0x04, OPEN_VRPC_SIGNATURE);
    w32(buf, 0x08, tot);
    w32(buf, 0x0c, fn);
    w32(buf, 0x10, OPEN_VRPC_PENDING);
}

static void test_rm_backed_vrpc_and_nva082_lifecycle(void)
{
    memset(&g_mock_rm, 0, sizeof(g_mock_rm));
    g_mock_rm.next_host_client = 0xc1d01000U;
    open_vgpu_rm_sys_ops_t ops = {
        .open_fn = mock_rm_open, .close_fn = mock_rm_close, .ioctl_fn = mock_rm_ioctl
    };
    open_vgpu_rm_set_sys_ops(&ops);

    gp104_vgpu_dev_t dev;
    gp104_dev_init_defaults(&dev, 1U, 65U);
    snprintf(dev.gpu_pci_bdf, sizeof(dev.gpu_pci_bdf), "0000:01:00.0");
    dev.vgpu_uuid[0] = 0x11U;

    vmiop_env_v2_t env;
    bind_env(&env);
    assert(gp104_dev_start(&dev, &env) == VMIOP_SUCCESS);
    assert(dev.rm.attached == 1 && dev.rm.nva082_allocated == 1);
    assert(g_mock_rm.saw_ver_check == 1 && g_mock_rm.saw_card_info == 1 && g_mock_rm.saw_reg_fd == 1);
    assert(g_mock_rm.saw_nva081 == 1 && g_mock_rm.saw_nva082 == 1);
    assert(dev.rm.virt_mode == 3U && dev.rm.encoder_capacity == 85U);
    assert(strcmp(dev.rm.live_profile_name, "GRID P4-4Q") == 0);

    /* Exercise xfer (>0x1000) path cleanly */
    uint8_t dummy_big[0x1200];
    memset(dummy_big, 0, sizeof(dummy_big));
    assert(open_vgpu_rm_ctl_ioctl(dev.rm.ctl_fd, 0x99U, sizeof(dummy_big), 0UL, dummy_big) == 0);
    assert(g_mock_rm.saw_xfer == 1);

    static uint8_t vbuf[8192];

    /* 0x01 SET_GUEST_SYSTEM_INFO + 0x40 branch string -> 0xa0820103 */
    init_vrpc(vbuf, VRPC_FN_SET_GUEST_SYSTEM_INFO, 824U);
    w32(vbuf, 0x20, 0x23U);
    w32(vbuf, 0x24, 0x05U);
    w32(vbuf, 0x28, 0x01U);
    snprintf((char *)(vbuf + 0x34), 32U, "535.309.01");
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);

    init_vrpc(vbuf, VRPC_FN_SET_GUEST_BRANCH_VER, 296U);
    snprintf((char *)(vbuf + 0x20), 32U, "r539_72");
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.last_ctrl_cmd == NVA082_CTRL_CMD_SET_GUEST_INFO);

    /* 0x39 SET_LICENSE_STATE: invalid expiry rejected; valid state forwarded unchanged */
    init_vrpc(vbuf, VRPC_FN_SET_LICENSE_STATE, 52U);
    vbuf[0x20] = 4U;
    vbuf[0x30] = 99U;
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_ERR_INVALID_ARGUMENT);
    vbuf[0x30] = 1U;
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.last_ctrl_cmd == NVA082_CTRL_CMD_SET_LICENSE_STATE);
    assert(g_mock_rm.last_lic_state == 4U);

    /* 0x3e GET_ENCODER_CAPACITY, 0x3f PF_REG_READ32, 0x55 SET_FB_USAGE, 0x50/0x9c */
    init_vrpc(vbuf, VRPC_FN_GET_ENCODER_CAPACITY, 44U);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(r32(vbuf, 0x28) == 85U);

    init_vrpc(vbuf, VRPC_FN_VGPU_PF_REG_READ32, 48U);
    w32(vbuf, 0x20, 0x00000000U); /* NV_PMC_BOOT_0 */
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(r32(vbuf, 0x28) == 0x134000a1U);

    init_vrpc(vbuf, VRPC_FN_GET_STATIC_INFO, 40U);
    uint64_t used_fb = 0x12340000ULL;
    memcpy(vbuf + 0x20, &used_fb, sizeof(used_fb));
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.last_fb_usage == used_fb);

    init_vrpc(vbuf, VRPC_FN_BATCH_50, 2560U);
    w32(vbuf, 0x20, 0U);
    w32(vbuf, 0x24, 128U);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(strcmp((char *)(vbuf + 0x68), "GRID P4-4Q") == 0);

    init_vrpc(vbuf, VRPC_FN_SYNC_9C, 3184U);
    w32(vbuf, 0x20, 0U);
    w32(vbuf, 0x24, 64U);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(r32(vbuf, 0x28) == 0x134000a1U);

    /* 0x02 ALLOC_ROOT (clash + ok), 0x20 ALLOC_DEV, 0x13 ALLOC_SUBDEV */
    init_vrpc(vbuf, VRPC_FN_ALLOC_ROOT, 140U);
    w32(vbuf, 0x20, 0x01ff0001U);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == 0xff100007U);

    uint32_t g_client = 0xc1d00005U, g_dev = 0xaa000001U, g_sub = 0xaa000002U;
    w32(vbuf, 0x20, g_client);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.last_ctrl_cmd == NV0000_CTRL_CMD_CLIENT_DISABLE_USERD_ISOL);

    init_vrpc(vbuf, VRPC_FN_SET_GUEST_FB_INFO, 96U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    w32(vbuf, 0x28, NV01_DEVICE_0);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);

    init_vrpc(vbuf, VRPC_FN_SET_OBJECT, 72U);
    w32(vbuf, 0x20, 0U);
    w32(vbuf, 0x28, g_client);
    w32(vbuf, 0x2c, g_dev);
    w32(vbuf, 0x30, g_sub);
    w32(vbuf, 0x34, NV20_SUBDEVICE_0);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);

    /* 0x5b..0x8f RM controls + 0x1a inline RM_API_CONTROL + 0x15 DUP + 0x0a FREE */
    struct { uint32_t fn; uint32_t tot; uint32_t cmd; } rm_cases[] = {
        { VRPC_FN_GR_CTXSW_ZCULL,  1584U, NV2080_CTRL_CMD_GR_CTXSW_ZCULL },
        { VRPC_FN_FB_GET_INFO_200A,  48U, NV2080_CTRL_CMD_FB_0x200A },
        { VRPC_FN_TSG_CTRL,          48U, NVA06C_CTRL_CMD_TSG_0x0103 },
        { VRPC_FN_SM_DEBUG_CTRL,     44U, NV83DE_CTRL_CMD_DEBUG_0x0309 },
        { VRPC_FN_GR_GFXP_PREEMPT,  144U, NV2080_CTRL_CMD_GR_GFXP_PREEMPT },
        { VRPC_FN_GR_CTRL_1210,      72U, NV2080_CTRL_CMD_GR_GET_0x1210 },
        { VRPC_FN_GR_CTRL_1208,      64U, NV2080_CTRL_CMD_GR_GET_0x1208 },
        { VRPC_FN_DEV_CTRL_1812,     44U, NV0080_CTRL_CMD_FB_0x1812 },
        { VRPC_FN_PERF_CTRL_2A02,    48U, NV2080_CTRL_CMD_PERF_0x2A02 },
        { VRPC_FN_ZBC_CTRL_0107,     96U, NV9096_CTRL_CMD_SET_ZBC_0x0107 },
        { VRPC_FN_PERF_CTRL_3002,  3120U, NV2080_CTRL_CMD_PERF_0x3002 },
        { VRPC_FN_SYS_CTRL_0127,    196U, NV0000_CTRL_CMD_SYSTEM_GET_P2P_CAPS_V2 },
        { VRPC_FN_SYS_CTRL_013A,   1392U, NV0000_CTRL_CMD_SYSTEM_GET_P2P_CAPS_MATRIX },
        { VRPC_FN_DEV_CTRL_170F,     56U, NV0080_CTRL_CMD_DMA_0x170F },
    };
    for (size_t i = 0; i < sizeof(rm_cases) / sizeof(rm_cases[0]); i++) {
        init_vrpc(vbuf, rm_cases[i].fn, rm_cases[i].tot);
        w32(vbuf, 0x20, g_client);
        w32(vbuf, 0x24, g_sub);
        assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
        assert(g_mock_rm.last_ctrl_cmd == rm_cases[i].cmd);
    }

    init_vrpc(vbuf, VRPC_FN_GPFIFO_SCHEDULE, 48U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_sub);
    w32(vbuf, 0x28, 0xdeadbeefU);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_ERR_INVALID_ARGUMENT);
    w32(vbuf, 0x28, NVA06F_CTRL_CMD_GPFIFO_SCHEDULE);
    vbuf[0x2c] = 1U;
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);

    init_vrpc(vbuf, VRPC_FN_FB_GET_INFO_8E, 64U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_sub);
    w32(vbuf, 0x28, 1U);
    w32(vbuf, 0x30, 1U);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(r32(vbuf, 0x34) == (uint32_t)(0xec000000ULL >> 10));

    init_vrpc(vbuf, VRPC_FN_RM_API_CONTROL, 64U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_sub);
    w32(vbuf, 0x28, NV2080_CTRL_CMD_BUS_GET_PCI_INFO);
    w32(vbuf, 0x38, 16U);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(r32(vbuf, 0x40) == 0x1bb310deU);

    init_vrpc(vbuf, VRPC_FN_DUP_OBJECT_535, 60U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    w32(vbuf, 0x28, 0xbb000001U);
    w32(vbuf, 0x2c, g_client);
    w32(vbuf, 0x30, g_sub);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.dup_calls == 1);

    init_vrpc(vbuf, VRPC_FN_FREE_535, 48U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_client);
    w32(vbuf, 0x28, g_client);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);

    gp104_dev_shutdown(&dev);
    open_vgpu_rm_reset_sys_ops();
    assert(g_mock_rm.open_fds == 0);
    assert(g_mock_rm.free_calls >= 5);
    printf("  [PASS] test_rm_backed_vrpc_and_nva082_lifecycle\n");
}

static int run_live_rm_probe(void)
{
    open_vgpu_rm_reset_sys_ops();
    open_vgpu_rm_ctx_t rm;
    memset(&rm, 0, sizeof(rm));
    rm.ctl_fd = -1;
    rm.gpu_fd = -1;
    uint8_t uuid[16] = { 0x11,0x11,0x11,0x11, 0,0, 0,0, 0,0, 0,0,0,0,0,0xfe };
    NV_STATUS st = open_vgpu_rm_attach(&rm, 9U, 65U, "0000:01:00.0", uuid, (uint32_t)getpid());
    if (st != NV_OK || !rm.attached) {
        fprintf(stderr, "live-rm-probe: open_vgpu_rm_attach failed status=0x%x\n", st);
        open_vgpu_rm_detach(&rm);
        return 1;
    }

    uint8_t p2p_v2[0xa8];
    memset(p2p_v2, 0, sizeof(p2p_v2));
    NV_STATUS p2p_st = open_vgpu_rm_control(rm.ctl_fd, rm.h_root, rm.h_root,
                                            NV0000_CTRL_CMD_SYSTEM_GET_P2P_CAPS_V2,
                                            p2p_v2, sizeof(p2p_v2));
    printf("LIVE_RM_PROBE: pci=%04x:%04x subsys=%04x:%04x virt_mode=%u profile=\"%s\" fb=0x%llx bar1=0x%llx frl=%u nva082=%d(0x%x) p2p_v2_st=0x%x\n",
           rm.pci_vendor, rm.pci_device, rm.pci_subsys_vendor, rm.pci_subsys_device,
           rm.virt_mode, rm.live_profile_name, (unsigned long long)rm.live_fb_length,
           (unsigned long long)rm.live_bar1_length, rm.live_frl_config,
           rm.nva082_allocated, rm.nva082_status, p2p_st);

    int ok = (rm.pci_vendor == 0x10deU && rm.pci_device == 0x1bb3U &&
              rm.virt_mode == 3U && strcmp(rm.live_profile_name, "GRID P4-4Q") == 0 &&
              rm.live_fb_length == 0xec000000ULL &&
              rm.live_bar1_length == 256ULL * 1024ULL * 1024ULL);
    open_vgpu_rm_detach(&rm);
    return ok ? 0 : 2;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--live-rm-probe") == 0) {
        return run_live_rm_probe();
    }
    printf("Running open-nvidia-vgpu plugin unit tests...\n");
    test_standalone_gp104_end_to_end();
    test_hybrid_interposer_and_trace();
    test_rm_backed_vrpc_and_nva082_lifecycle();
    printf("ALL open-nvidia-vgpu PLUGIN UNIT TESTS PASSED!\n");
    return 0;
}
