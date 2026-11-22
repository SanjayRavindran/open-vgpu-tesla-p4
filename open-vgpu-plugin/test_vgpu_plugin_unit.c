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
static uint32_t g_pin_calls = 0U;
static uint64_t g_pinned_pages = 0ULL;
static uint32_t g_unpin_calls = 0U;
static uint64_t g_unpinned_pages = 0ULL;
static uint32_t g_region_map_adds = 0U;
static uint32_t g_region_map_removes = 0U;
static uint32_t g_last_map_region_id = 0U;
static uint64_t g_last_map_vmmio = 0ULL;
static uint64_t g_last_map_pmmio = 0ULL;
static uint64_t g_last_map_size = 0ULL;

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

uint32_t vmiop_get_guest_id(uint64_t out_uuid[2])
{
    if (!out_uuid) return VMIOP_ERROR_INVAL;
    out_uuid[0] = 0x1111111122222222ULL;
    out_uuid[1] = 0x3333333344444444ULL;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_pin_guest_pages_v2(uint32_t dev_handle, const uint64_t *gpfn,
                                  uint64_t *hpfn, uint32_t count)
{
    (void)dev_handle;
    if (!gpfn || !hpfn) return VMIOP_ERROR_INVAL;
    for (uint32_t i = 0; i < count; i++) {
        hpfn[i] = gpfn[i] + 0x100000ULL;
    }
    g_pin_calls++;
    g_pinned_pages += count;
    return VMIOP_SUCCESS;
}

uint32_t vmiop_map_guest_region(uint32_t region_id, uint64_t pmmio_start,
                                uint64_t reserved, uint64_t size,
                                uint64_t vmmio_start, int add)
{
    (void)reserved;
    if (region_id == 0U || size == 0ULL) return VMIOP_ERROR_INVAL;
    g_last_map_region_id = region_id;
    g_last_map_pmmio     = pmmio_start;
    g_last_map_vmmio     = vmmio_start;
    g_last_map_size      = size;
    if (add == 1) {
        g_region_map_adds++;
    } else if (add == 0) {
        g_region_map_removes++;
    } else {
        return VMIOP_ERROR_INVAL;
    }
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
    (void)h; (void)p; (void)u;
    g_unpin_calls++;
    g_unpinned_pages += c;
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
    static const struct { uint32_t fn; uint32_t tot; } live_ops[] = {
        { VRPC_FN_FILL_PTE_MEM,         136U },
        { VRPC_FN_UPDATE_PDE_2,         96U  },
        { VRPC_FN_SET_GUEST_BRANCH_VER, 296U },
        { VRPC_FN_BATCH_50,             64U  },
        { VRPC_FN_SYNC_9C,              64U  },
    };
    for (size_t i = 0; i < sizeof(live_ops) / sizeof(live_ops[0]); i++) {
        w32(msg, 0x08, live_ops[i].tot);
        w32(msg, 0x0c, live_ops[i].fn);
        w32(msg, 0x10, OPEN_VRPC_PENDING);
        if (live_ops[i].fn == VRPC_FN_BATCH_50 || live_ops[i].fn == VRPC_FN_SYNC_9C) {
            w32(msg, 0x20, 0U);
            w32(msg, 0x24, 24U);
        }
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
    uint32_t map_mem_calls;
    uint32_t unmap_mem_calls;
    uint32_t map_dma_calls;
    uint32_t unmap_dma_calls;
    uint32_t alloc_list_sys_calls;
    uint32_t alloc_ctx_dma_calls;
    uint32_t alloc_chan_calls;
    uint32_t alloc_vidmem_calls;
    uint32_t alloc_hwres_calls;
    uint32_t alloc_virtmem_calls;
    uint32_t alloc_obj_calls;
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
        if (p->hClass == NVA081_VGPU_CONFIG) {
            g_mock_rm.saw_nva081++;
        } else if (p->hClass == NVA082_HOST_VGPU_DEVICE) {
            assert(p->paramsSize == 0x70U);
            NVA082_ALLOC_PARAMETERS *ap = (NVA082_ALLOC_PARAMETERS *)(uintptr_t)p->pAllocParms;
            assert(ap->vgpuType == 65U);
            assert(ap->vmIdType == 1U);
            g_mock_rm.saw_nva082++;
        } else if (p->hClass == NV01_DEVICE_0 || p->hClass == NV20_SUBDEVICE_0) {
            /* device / subdevice tracked separately */
        } else if (p->hClass == NV01_MEMORY_LIST_SYSTEM) {
            g_mock_rm.alloc_list_sys_calls++;
        } else if (p->hClass == NV01_CONTEXT_DMA) {
            g_mock_rm.alloc_ctx_dma_calls++;
        } else if (p->hClass == 0xc06fU || p->hClass == 0xa06fU || p->hClass == 0x906fU) {
            g_mock_rm.alloc_chan_calls++;
        } else if (p->hClass == NV01_MEMORY_LIST_FBMEM) {
            g_mock_rm.alloc_vidmem_calls++;
        } else if (p->hClass == NV01_MEMORY_HW_RESOURCES) {
            g_mock_rm.alloc_hwres_calls++;
        } else if (p->hClass == NV50_MEMORY_VIRTUAL) {
            g_mock_rm.alloc_virtmem_calls++;
        } else {
            g_mock_rm.alloc_obj_calls++;
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
    if (nr == NV_ESC_RM_MAP_MEMORY) {
        NVOS33_PARAMETERS *p = (NVOS33_PARAMETERS *)arg;
        g_mock_rm.map_mem_calls++;
        p->pLinearAddress = 0x7f001000ULL;
        p->status = NV_OK;
        return 0;
    }
    if (nr == NV_ESC_RM_UNMAP_MEMORY) {
        NVOS34_PARAMETERS *p = (NVOS34_PARAMETERS *)arg;
        g_mock_rm.unmap_mem_calls++;
        p->status = NV_OK;
        return 0;
    }
    if (nr == NV_ESC_RM_MAP_MEMORY_DMA) {
        NVOS46_PARAMETERS *p = (NVOS46_PARAMETERS *)arg;
        g_mock_rm.map_dma_calls++;
        if (p->dmaOffset == 0ULL) {
            p->dmaOffset = 0x200000000ULL;
        }
        p->status = NV_OK;
        return 0;
    }
    if (nr == NV_ESC_RM_UNMAP_MEMORY_DMA) {
        NVOS47_PARAMETERS *p = (NVOS47_PARAMETERS *)arg;
        g_mock_rm.unmap_dma_calls++;
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
        } else if (p->cmd == NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR) {
            uint64_t pmmio = 0xfe0010000ULL;
            memcpy((uint8_t *)prm + 0x08, &pmmio, sizeof(pmmio));
        } else if (p->cmd == NV2080_CTRL_CMD_NVLINK_GET_NVLINK_CAPS) {
            w32((uint8_t *)prm, 0x00, 0x11223344U);
        } else if (p->cmd == NV2080_CTRL_CMD_MC_GET_STATIC_INTR_TABLE_1) {
            w32((uint8_t *)prm, 0x00, 0x55aa170dU);
        } else if (p->cmd == NV2080_CTRL_CMD_MC_GET_STATIC_INTR_TABLE_2) {
            w32((uint8_t *)prm, 0x00, 0x66bb170eU);
        } else if (p->cmd == NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE) {
            w32((uint8_t *)prm, 0x00, 0x77cc1112U);
        } else if (p->cmd == NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_SM_ORDER) {
            w32((uint8_t *)prm, 0x00, 0x13400a2aU);
        } else if (p->cmd == NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_INFO) {
            w32((uint8_t *)prm, 0x00, 0x13400a22U);
        } else if (p->cmd == NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_GPC_MASK) {
            uint32_t *g = (uint32_t *)prm;
            for (uint32_t e = 0; e < 8U; e++) {
                uint32_t *row = g + e * 0x4aU;
                row[0]    = 0xa0000000U | e;
                row[1]    = 0xb0000000U | e;
                row[0x19] = 0xc0000000U | e;
                row[0x26] = 0xd0000000U | e;
                row[0x3c] = 0xe0000000U | e;
            }
        } else if (p->cmd == NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_PDB_PROPS) {
            w32((uint8_t *)prm, 0x00, 0x000a48ffU);
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
    w32(vbuf, 0x20, GP104_SINFO_OFF_CORE + 0x200U);
    w32(vbuf, 0x24, 256U);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(strcmp((char *)(vbuf + 0x28 + 0x60), "GRID P4-4Q") == 0);

    init_vrpc(vbuf, VRPC_FN_SYNC_9C, 3184U);
    w32(vbuf, 0x20, GP104_GRINFO_OFF_SM_ORDER);
    w32(vbuf, 0x24, 64U);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(r32(vbuf, 0x28) == 0x13400a2aU);

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

static void w64(uint8_t *p, size_t off, uint64_t v)
{
    memcpy(p + off, &v, 8U);
}

static uint64_t r64(const uint8_t *p, size_t off)
{
    uint64_t v = 0ULL;
    memcpy(&v, p + off, 8U);
    return v;
}

static void test_mmu_channels_pfn_pin_and_bar1_apertures(void)
{
    memset(&g_mock_rm, 0, sizeof(g_mock_rm));
    g_mock_rm.next_host_client = 0xc1d02000U;
    g_pin_calls          = 0U;
    g_pinned_pages       = 0ULL;
    g_unpin_calls        = 0U;
    g_unpinned_pages     = 0ULL;
    g_region_map_adds    = 0U;
    g_region_map_removes = 0U;
    g_last_map_region_id = 0U;
    g_last_map_vmmio     = 0ULL;
    g_last_map_pmmio     = 0ULL;
    g_last_map_size      = 0ULL;

    open_vgpu_rm_sys_ops_t ops = {
        .open_fn = mock_rm_open, .close_fn = mock_rm_close, .ioctl_fn = mock_rm_ioctl
    };
    open_vgpu_rm_set_sys_ops(&ops);

    static gp104_vgpu_dev_t dev;
    gp104_dev_init_defaults(&dev, 1U, 65U);
    snprintf(dev.gpu_pci_bdf, sizeof(dev.gpu_pci_bdf), "0000:01:00.0");

    vmiop_env_v2_t env;
    bind_env(&env);
    assert(gp104_dev_start(&dev, &env) == VMIOP_SUCCESS);

    /* Simulate PCI relocation & non-zero instance byte (inst=1) in VMIOP region IDs */
    dev.bar0_gpa       = 0x81000000ULL;
    dev.bar1_gpa       = 0x800000000ULL;
    dev.bar0_region_id = 0x01000001U;
    dev.bar1_region_id = 0x01000002U;

    static uint8_t vbuf[8192];
    uint32_t g_client = 0xc1d00010U, g_dev = 0xaa100001U, g_sub = 0xaa100002U;

    init_vrpc(vbuf, VRPC_FN_SET_GUEST_SYSTEM_INFO, 824U);
    w32(vbuf, 0x20, 0x23U);
    w32(vbuf, 0x24, 0x05U);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);

    init_vrpc(vbuf, VRPC_FN_ALLOC_ROOT, 140U);
    w32(vbuf, 0x20, g_client);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);

    init_vrpc(vbuf, VRPC_FN_SET_GUEST_FB_INFO, 96U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    w32(vbuf, 0x28, NV01_DEVICE_0);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);

    init_vrpc(vbuf, VRPC_FN_SET_OBJECT, 72U);
    w32(vbuf, 0x28, g_client);
    w32(vbuf, 0x2c, g_dev);
    w32(vbuf, 0x30, g_sub);
    w32(vbuf, 0x34, NV20_SUBDEVICE_0);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);

    /* 1) 0x04 ALLOC_MEMORY: page/length bounds check + PFN pinning + 0x81 alloc */
    uint32_t h_sys_mem = 0xbb100001U;
    init_vrpc(vbuf, VRPC_FN_ALLOC_MEMORY_535, 120U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    w32(vbuf, 0x28, h_sys_mem);
    w32(vbuf, 0x34, 0x100U);          /* pageOffset */
    w64(vbuf, 0x40, 0x4000ULL);       /* length */
    w32(vbuf, 0x48, 2U);              /* too few pages: 2 * 4096 < 0x4100 */
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_ERR_INVALID_ARGUMENT);

    init_vrpc(vbuf, VRPC_FN_ALLOC_MEMORY_535, 0x58U + 5U * 8U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    w32(vbuf, 0x28, h_sys_mem);
    w32(vbuf, 0x34, 0x100U);
    w64(vbuf, 0x40, 0x4000ULL);
    w32(vbuf, 0x48, 5U);
    for (uint32_t i = 0; i < 5U; i++) {
        w64(vbuf, 0x58U + i * 8U, 0x200ULL + i);
    }
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_pin_calls == 1U && g_pinned_pages == 5ULL);
    assert(g_mock_rm.alloc_list_sys_calls == 1U);

    /* 2) 0x05 ALLOC_CTX_DMA */
    init_vrpc(vbuf, VRPC_FN_ALLOC_CTX_DMA_535, 88U);
    w32(vbuf, 0x24, g_client);
    w32(vbuf, 0x28, 0x01ff0005U);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == 0xff100007U);

    w32(vbuf, 0x28, 0xdd100001U);
    w32(vbuf, 0x2c, NV01_CONTEXT_DMA);
    w32(vbuf, 0x38, h_sys_mem);
    w64(vbuf, 0x48, 0x3fffULL);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.alloc_ctx_dma_calls == 1U);

    /* 3) 0x06 ALLOC_CHANNEL_DMA + USERD direct MMIO map + unlicensed channel limit */
    uint32_t h_chan = 0xcc100001U;
    init_vrpc(vbuf, VRPC_FN_ALLOC_CHANNEL_DMA_535, 280U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    w32(vbuf, 0x28, h_chan);
    w32(vbuf, 0x2c, 0xdeadU); /* bad class */
    w32(vbuf, 0x48, 64U);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == 0x22U);

    w32(vbuf, 0x2c, 0xc06fU);
    w32(vbuf, 0x48, 3U); /* non-power-of-2 */
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_ERR_INVALID_ARGUMENT);

    w32(vbuf, 0x48, 64U);
    w64(vbuf, 0xe0, 0x81fe00000ULL); /* userd vmmio */
    w64(vbuf, 0xe8, 0xfefdf2000ULL); /* userd pmmio */
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.alloc_chan_calls == 1U);
    assert(g_region_map_adds == 1U && g_last_map_region_id == 0x01000001U);
    assert(g_last_map_vmmio == 0x81fe00000ULL && g_last_map_pmmio == 0xfefdf2000ULL);
    assert(g_last_map_size == 0x1000ULL && dev.active_channel_count == 1U);

    dev.license_state = 4U;
    uint32_t saved_cnt = dev.active_channel_count;
    dev.active_channel_count = 0x55U;
    w32(vbuf, 0x28, 0xcc100056U);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_ERR_NO_MEMORY);
    dev.active_channel_count = saved_cnt;
    dev.license_state = 1U;

    init_vrpc(vbuf, VRPC_FN_FREE_535, 48U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    w32(vbuf, 0x28, h_chan);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_region_map_removes == 1U && dev.active_channel_count == 0U);

    /* 4) 0x07 MAP_MEMORY BAR1 aperture forwarding & ref-counted deduplication */
    uint32_t h_bar1_mem = 0xbb100002U;
    init_vrpc(vbuf, VRPC_FN_MAP_MEMORY_535, 88U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    w32(vbuf, 0x28, h_bar1_mem);
    w64(vbuf, 0x38, 512ULL * 1024ULL * 1024ULL); /* > 256 MiB BAR1 */
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_ERR_NO_MEMORY);

    w64(vbuf, 0x38, 0x10000ULL);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.map_mem_calls == 1U);
    assert(g_mock_rm.last_ctrl_cmd == NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR);
    assert(g_region_map_adds == 2U && g_last_map_region_id == 0x01000002U);
    assert(g_last_map_pmmio == 0xfe0010000ULL && g_last_map_vmmio == 0x800000000ULL);
    assert(g_last_map_size == 0x10000ULL && r64(vbuf, 0x40) == 0ULL);

    assert(gp104_guest_region_map(&dev, dev.bar1_region_id, 0xfe0010000ULL,
                                  0x10000ULL, 0x800000000ULL, 1) == VMIOP_SUCCESS);
    assert(g_region_map_adds == 2U); /* deduplicated */
    assert(gp104_guest_region_map(&dev, dev.bar1_region_id, 0xfe0010000ULL,
                                  0x10000ULL, 0x800000000ULL, 0) == VMIOP_SUCCESS);
    assert(g_region_map_removes == 1U); /* still refcnt=1 */

    init_vrpc(vbuf, VRPC_FN_FREE_535, 48U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    w32(vbuf, 0x28, h_bar1_mem);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_region_map_removes == 2U && g_mock_rm.unmap_mem_calls == 1U);
    assert(dev.bar1_allocated_bytes == 0ULL);

    /* 5) 0x09 ALLOC_OBJECT */
    init_vrpc(vbuf, VRPC_FN_ALLOC_OBJECT_535, 112U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    w32(vbuf, 0x28, 0xee100001U);
    w32(vbuf, 0x2c, 0xc076U); /* unsupported vGPU class */
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_ERR_NOT_SUPPORTED);

    w32(vbuf, 0x2c, 0xc197U);
    w32(vbuf, 0x30, 1024U); /* oversized params */
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == 0xff100002U);

    w32(vbuf, 0x30, 0U);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    w32(vbuf, 0x28, 0xee100002U);
    w32(vbuf, 0x2c, KEPLER_CHANNEL_GROUP_A);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.alloc_obj_calls == 2U);

    /* 6) 0x0c ALLOC_VIDMEM + guest FB PFN range check */
    init_vrpc(vbuf, VRPC_FN_ALLOC_VIDMEM_535, 128U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    w32(vbuf, 0x28, 0xbb10000cU);
    w32(vbuf, 0x2c, NV01_MEMORY_LIST_FBMEM);
    w32(vbuf, 0x34, 0x02000000U); /* invalid attr bit combination */
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_ERR_INVALID_ARGUMENT);

    w32(vbuf, 0x34, 0U);
    w64(vbuf, 0x40, 0x10000ULL);
    w64(vbuf, 0x78, 0xfffffULL); /* out of 3776 MiB guest FB range */
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_ERR_INSUFFICIENT_RESOURCES);

    w64(vbuf, 0x78, 0x100ULL); /* valid guest FB PFN */
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.alloc_vidmem_calls == 1U);

    /* 7) 0x0e MAP_MEMORY_DMA & 0x0f UNMAP_MEMORY_DMA & unpin on 0x04 memory free */
    init_vrpc(vbuf, VRPC_FN_MAP_MEMORY_DMA, 88U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    w32(vbuf, 0x28, 0xdd100001U);
    w32(vbuf, 0x2c, h_sys_mem);
    w64(vbuf, 0x38, 0x4000ULL);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.map_dma_calls == 1U && r64(vbuf, 0x48) == 0x200000000ULL);

    init_vrpc(vbuf, VRPC_FN_UNMAP_MEMORY_DMA, 72U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    w32(vbuf, 0x28, 0xdd100001U);
    w32(vbuf, 0x2c, h_sys_mem);
    w64(vbuf, 0x38, 0x200000000ULL);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.unmap_dma_calls == 1U);

    init_vrpc(vbuf, VRPC_FN_FREE_535, 48U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    w32(vbuf, 0x28, h_sys_mem);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_unpin_calls == 1U && g_unpinned_pages == 5ULL);

    /* 8) 0x1b FILL_PTE_MEM */
    init_vrpc(vbuf, VRPC_FN_FILL_PTE_MEM, 136U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    w32(vbuf, 0x40, 4U);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.last_ctrl_cmd == NV0080_CTRL_CMD_DMA_FILL_PTE_MEM);
    assert(dev.pte_fill_count == 1ULL);

    /* 9) 0x1c ALLOC_HWRES */
    init_vrpc(vbuf, VRPC_FN_ALLOC_HWRES, 152U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    w32(vbuf, 0x28, 0x01ff001cU);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == 0xff100007U);
    w32(vbuf, 0x28, 0xbb10001cU);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.alloc_hwres_calls == 1U);

    /* 10) 0x21 DEFERRED_API_CONTROL */
    init_vrpc(vbuf, VRPC_FN_DEFERRED_API_535, 128U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_sub);
    w32(vbuf, 0x28, 0xdeadbeefU);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_ERR_NOT_SUPPORTED);
    w32(vbuf, 0x28, 0x2080012bU);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.last_ctrl_cmd == NV5080_CTRL_CMD_DEFERRED_API_V2);

    /* 11) 0x2b MAP_SEMA_MEM */
    init_vrpc(vbuf, VRPC_FN_MAP_SEMA_MEM_535, 56U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    w32(vbuf, 0x28, 0xbb10002bU);
    w32(vbuf, 0x2c, g_sub);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.map_dma_calls == 2U && r64(vbuf, 0x30) == 0x200000000ULL);

    /* 12) 0x34 ALLOC_VIRTUAL_MEMORY */
    init_vrpc(vbuf, VRPC_FN_ALLOC_VIRTMEM_535, 128U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    w32(vbuf, 0x2c, 0xbb100034U);
    w32(vbuf, 0x34, 0U); /* missing 0x80000 virtual flag */
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_ERR_INVALID_ARGUMENT);
    w32(vbuf, 0x34, 0x00080000U);
    w64(vbuf, 0x50, 0x100000ULL);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.alloc_virtmem_calls == 1U);

    /* 13) 0x38 UPDATE_PDE_2 */
    init_vrpc(vbuf, VRPC_FN_UPDATE_PDE_2, 96U);
    w32(vbuf, 0x20, g_client);
    w32(vbuf, 0x24, g_dev);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.last_ctrl_cmd == NV0080_CTRL_CMD_DMA_UPDATE_PDE_2);
    assert(dev.pde_update_count == 1ULL);

    /* 14) 0x3d UPDATE_GPU_PDES */
    init_vrpc(vbuf, VRPC_FN_UPDATE_GPU_PDES, 80U);
    w64(vbuf, 0x20, 0x400000000ULL);
    w32(vbuf, 0x28, 1U); /* isValid = 1 */
    w32(vbuf, 0x38, g_client);
    w32(vbuf, 0x3c, g_dev);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.last_ctrl_cmd == NV0080_CTRL_CMD_DMA_SET_PAGE_DIRECTORY);
    assert(dev.gpu_pde_bound_count == 1U);

    w32(vbuf, 0x28, 0U); /* isValid = 0 */
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
    assert(g_mock_rm.last_ctrl_cmd == NV0080_CTRL_CMD_DMA_UNSET_PAGE_DIRECTORY);
    assert(dev.gpu_pde_bound_count == 0U);

    gp104_dev_shutdown(&dev);
    open_vgpu_rm_reset_sys_ops();
    assert(g_mock_rm.open_fds == 0);
    assert(g_region_map_adds == g_region_map_removes);
    assert(g_pinned_pages == g_unpinned_pages);
    assert(g_mock_rm.unmap_dma_calls == 2U);
    printf("  [PASS] test_mmu_channels_pfn_pin_and_bar1_apertures\n");
}

static void test_consolidated_static_info_v23_05_binary_parity(void)
{
    memset(&g_mock_rm, 0, sizeof(g_mock_rm));
    g_mock_rm.next_host_client = 0xc1d03000U;
    open_vgpu_rm_sys_ops_t ops = {
        .open_fn = mock_rm_open, .close_fn = mock_rm_close, .ioctl_fn = mock_rm_ioctl
    };
    open_vgpu_rm_set_sys_ops(&ops);

    static gp104_vgpu_dev_t dev;
    gp104_dev_init_defaults(&dev, 1U, 65U);
    snprintf(dev.gpu_pci_bdf, sizeof(dev.gpu_pci_bdf), "0000:01:00.0");

    vmiop_env_v2_t env;
    bind_env(&env);
    assert(gp104_dev_start(&dev, &env) == VMIOP_SUCCESS);

    static uint8_t vbuf[4096];
    init_vrpc(vbuf, VRPC_FN_SET_GUEST_SYSTEM_INFO, 824U);
    w32(vbuf, 0x20, 0x23U);
    w32(vbuf, 0x24, 0x05U);
    w32(vbuf, 0x28, 0x01U);
    snprintf((char *)(vbuf + 0x34), 32U, "535.309.01");
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);

    init_vrpc(vbuf, VRPC_FN_SET_GUEST_BRANCH_VER, 296U);
    snprintf((char *)(vbuf + 0x20), 32U, "r535_00");
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);

    /* Pull all 17 chunks of 0x50 (16 * 4056 + 2520 = 67416 = 0x10758 B) */
    uint8_t *assembled_50 = (uint8_t *)calloc(1U, GP104_VGPU_STATIC_INFO_BYTES);
    assert(assembled_50 != NULL);
    uint32_t off = 0U;
    uint32_t chunks_50 = 0U;
    while (off < GP104_VGPU_STATIC_INFO_BYTES) {
        uint32_t want = GP104_VGPU_STATIC_INFO_BYTES - off;
        if (want > GP104_STATIC_INFO_CHUNK_MAX) {
            want = GP104_STATIC_INFO_CHUNK_MAX;
        }
        init_vrpc(vbuf, VRPC_FN_BATCH_50, 0x28U + want);
        w32(vbuf, 0x20, off);
        w32(vbuf, 0x24, want);
        assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
        memcpy(assembled_50 + off, vbuf + 0x28U, want);
        off += want;
        chunks_50++;
    }
    assert(chunks_50 == 17U && off == GP104_VGPU_STATIC_INFO_BYTES);
    assert(r32(assembled_50, GP104_SINFO_OFF_NVLINK_CAPS) == 0x11223344U);
    assert(r64(assembled_50, GP104_SINFO_OFF_CORE + 0x00U) == 0xec000000ULL);
    assert(r64(assembled_50, GP104_SINFO_OFF_CORE + 0x08U) == 256ULL * 1024ULL * 1024ULL);
    assert(strcmp((char *)(assembled_50 + GP104_SINFO_OFF_CORE + 0x60U), "535.309.01") == 0);
    assert(strcmp((char *)(assembled_50 + GP104_SINFO_OFF_CORE + 0x260U), "GRID P4-4Q") == 0);
    assert(strcmp((char *)(assembled_50 + GP104_SINFO_OFF_CORE + 0xc00U), "535.309.01") == 0);
    assert(strcmp((char *)(assembled_50 + GP104_SINFO_OFF_CORE + 0xe68U), "r535_00") == 0);
    assert(r32(assembled_50, GP104_SINFO_OFF_INTR_TABLE_1) == 0x55aa170dU);
    assert(r32(assembled_50, GP104_SINFO_OFF_FIFO_DEV_TABLE) == 0x77cc1112U);
    assert(r32(assembled_50, GP104_SINFO_OFF_INTR_TABLE_2) == 0x66bb170eU);
    free(assembled_50);

    /* Bounds enforcement matching _nv005312vgpu -> 0xff000001U */
    init_vrpc(vbuf, VRPC_FN_BATCH_50, 0x28U + 16U);
    w32(vbuf, 0x20, GP104_VGPU_STATIC_INFO_BYTES);
    w32(vbuf, 0x24, 16U);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_VGPU_ERR_INVALID_CHUNK_RANGE);
    init_vrpc(vbuf, VRPC_FN_BATCH_50, 0x28U);
    w32(vbuf, 0x20, 0U);
    w32(vbuf, 0x24, 0U);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_VGPU_ERR_INVALID_CHUNK_RANGE);

    /* Pull all 8 chunks of 0x9c (7 * 4056 + 3144 = 31536 = 0x7b30 B) */
    uint8_t *assembled_9c = (uint8_t *)calloc(1U, GP104_GR_STATIC_INFO_BYTES);
    assert(assembled_9c != NULL);
    off = 0U;
    uint32_t chunks_9c = 0U;
    while (off < GP104_GR_STATIC_INFO_BYTES) {
        uint32_t want = GP104_GR_STATIC_INFO_BYTES - off;
        if (want > GP104_STATIC_INFO_CHUNK_MAX) {
            want = GP104_STATIC_INFO_CHUNK_MAX;
        }
        init_vrpc(vbuf, VRPC_FN_SYNC_9C, 0x28U + want);
        w32(vbuf, 0x20, off);
        w32(vbuf, 0x24, want);
        assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_OK);
        memcpy(assembled_9c + off, vbuf + 0x28U, want);
        off += want;
        chunks_9c++;
    }
    assert(chunks_9c == 8U && off == GP104_GR_STATIC_INFO_BYTES);
    assert(r32(assembled_9c, GP104_GRINFO_OFF_SM_ORDER) == 0x13400a2aU);
    assert(r32(assembled_9c, GP104_GRINFO_OFF_KGR_INFO) == 0x13400a22U);
    /* Verify exact _nv007054vgpu (0x239210.c) packed entry 7 at stride 0x120 B */
    const uint32_t *wire_g7 = (const uint32_t *)(const void *)(assembled_9c + GP104_GRINFO_OFF_GPC_MASK + 7U * 0x120U);
    assert(wire_g7[0] == 0xa0000007U && wire_g7[1] == 0xb0000007U &&
           wire_g7[0x19] == 0xc0000007U && wire_g7[0x26] == 0xd0000007U &&
           wire_g7[0x3c] == 0xe0000007U);
    assert(r32(assembled_9c, GP104_GRINFO_OFF_PDB_PROPS) == 0x000a48ffU);
    free(assembled_9c);

    init_vrpc(vbuf, VRPC_FN_SYNC_9C, 0x28U + 16U);
    w32(vbuf, 0x20, GP104_GR_STATIC_INFO_BYTES - 8U);
    w32(vbuf, 0x24, 16U);
    assert(gp104_handle_vrpc_buffer(&dev, vbuf, sizeof(vbuf)) == NV_VGPU_ERR_INVALID_CHUNK_RANGE);

    gp104_dev_shutdown(&dev);
    assert(dev.static_info_buf == NULL && dev.gr_static_info_buf == NULL);
    open_vgpu_rm_reset_sys_ops();
    printf("  [PASS] test_consolidated_static_info_v23_05_binary_parity\n");
}

static int run_live_rm_probe(void)
{
    open_vgpu_rm_reset_sys_ops();
    static gp104_vgpu_dev_t dev;
    gp104_dev_init_defaults(&dev, 9U, 65U);
    snprintf(dev.gpu_pci_bdf, sizeof(dev.gpu_pci_bdf), "0000:01:00.0");
    uint8_t uuid[16] = { 0x11,0x11,0x11,0x11, 0,0, 0,0, 0,0, 0,0,0,0,0,0xfe };
    NV_STATUS st = open_vgpu_rm_attach(&dev.rm, 9U, 65U, "0000:01:00.0", uuid, (uint32_t)getpid());
    if (st != NV_OK || !dev.rm.attached) {
        fprintf(stderr, "live-rm-probe: open_vgpu_rm_attach failed status=0x%x\n", st);
        open_vgpu_rm_detach(&dev.rm);
        gp104_free_static_info_blobs(&dev);
        return 1;
    }

    uint8_t p2p_v2[0xa8];
    memset(p2p_v2, 0, sizeof(p2p_v2));
    NV_STATUS p2p_st = open_vgpu_rm_control(dev.rm.ctl_fd, dev.rm.h_root, dev.rm.h_root,
                                            NV0000_CTRL_CMD_SYSTEM_GET_P2P_CAPS_V2,
                                            p2p_v2, sizeof(p2p_v2));

    gp104_build_static_info_blobs(&dev);

    uint8_t kgr_probe[0xd40];
    memset(kgr_probe, 0, sizeof(kgr_probe));
    NV_STATUS kgr_st = open_vgpu_rm_control(dev.rm.ctl_fd, dev.rm.h_root, dev.rm.h_subdev,
                                            NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_SM_ORDER,
                                            kgr_probe, sizeof(kgr_probe));
    uint8_t intr1_probe[0x804];
    memset(intr1_probe, 0, sizeof(intr1_probe));
    NV_STATUS intr1_st = open_vgpu_rm_control(dev.rm.ctl_fd, dev.rm.h_root, dev.rm.h_subdev,
                                              NV2080_CTRL_CMD_MC_GET_STATIC_INTR_TABLE_1,
                                              intr1_probe, sizeof(intr1_probe));

    printf("LIVE_RM_PROBE: pci=%04x:%04x subsys=%04x:%04x virt_mode=%u profile=\"%s\" fb=0x%llx bar1=0x%llx frl=%u nva082=%d(0x%x) p2p_v2_st=0x%x static_info=0x%x(%u B) ok_mask=0x%02x gr_static_info=0x%x(%u B) gr_ok_mask=0x%03x kgr_st=0x%x intr1_st=0x%x\n",
           dev.rm.pci_vendor, dev.rm.pci_device, dev.rm.pci_subsys_vendor, dev.rm.pci_subsys_device,
           dev.rm.virt_mode, dev.rm.live_profile_name, (unsigned long long)dev.rm.live_fb_length,
           (unsigned long long)dev.rm.live_bar1_length, dev.rm.live_frl_config,
           dev.rm.nva082_allocated, dev.rm.nva082_status, p2p_st,
           dev.static_info_len, dev.static_info_len, dev.static_info_rm_ok_mask,
           dev.gr_static_info_len, dev.gr_static_info_len, dev.gr_static_info_rm_ok_mask,
           kgr_st, intr1_st);

    int ok = (dev.rm.pci_vendor == 0x10deU && dev.rm.pci_device == 0x1bb3U &&
              dev.rm.virt_mode == 3U && strcmp(dev.rm.live_profile_name, "GRID P4-4Q") == 0 &&
              dev.rm.live_fb_length == 0xec000000ULL &&
              dev.rm.live_bar1_length == 256ULL * 1024ULL * 1024ULL &&
              dev.static_info_len == GP104_VGPU_STATIC_INFO_BYTES &&
              dev.gr_static_info_len == GP104_GR_STATIC_INFO_BYTES &&
              dev.static_info_rm_ok_mask != 0U);
    gp104_free_static_info_blobs(&dev);
    open_vgpu_rm_detach(&dev.rm);
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
    test_mmu_channels_pfn_pin_and_bar1_apertures();
    test_consolidated_static_info_v23_05_binary_parity();
    printf("ALL open-nvidia-vgpu PLUGIN UNIT TESTS PASSED!\n");
    return 0;
}
