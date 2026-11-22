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

int main(void)
{
    printf("Running open-nvidia-vgpu plugin unit tests...\n");
    test_standalone_gp104_end_to_end();
    test_hybrid_interposer_and_trace();
    printf("ALL open-nvidia-vgpu PLUGIN UNIT TESTS PASSED!\n");
    return 0;
}
