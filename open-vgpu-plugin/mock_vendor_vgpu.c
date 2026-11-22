#include "open_vgpu_plugin.h"

#include <string.h>

static uint32_t mv_inject_msi(uint64_t h, uint64_t a, uint32_t b)
{
    (void)h; (void)a; (void)b;
    return VMIOP_ERROR_NOT_FOUND;
}
static uint32_t mv_update_lfb(void) { return VMIOP_SUCCESS; }
static uint32_t mv_unpin(uint64_t h, uint64_t *p, uint64_t u, uint32_t c)
{
    (void)h; (void)p; (void)u; (void)c;
    return VMIOP_ERROR_NOT_FOUND;
}
static uint32_t mv_guest_id_type(void) { return 1U; }
static uint32_t mv_rsv5(void) { return VMIOP_SUCCESS; }

vmiop_env_v2_t vmiop_display_vmiop_env = {
    .inject_msi        = mv_inject_msi,
    .update_lfb        = mv_update_lfb,
    .flags             = 0U,
    .pad14             = 0U,
    .unpin_pages       = mv_unpin,
    .get_guest_id_type = mv_guest_id_type,
    .reserved28        = mv_rsv5,
    .flags30           = 0x0000000100000001ULL,
};

static uint32_t g_mv_init_calls = 0U;
static uint32_t g_mv_type_id = 0U;
static uint64_t g_mv_mig_supported = 0ULL;

static uint32_t mv_init(uint32_t handle)
{
    g_mv_init_calls++;
    /* Verify vmiop_display_vmiop_env VER_2_0 callback propagation succeeded */
    if (!vmiop_display_vmiop_env.inject_msi) return VMIOP_ERROR_INVAL;
    if (vmiop_display_vmiop_env.inject_msi(handle, 0ULL, 0U) != VMIOP_SUCCESS) {
        return VMIOP_ERROR_INVAL;
    }
    return VMIOP_SUCCESS;
}

static uint32_t mv_shutdown(uint32_t handle)
{
    (void)handle;
    return VMIOP_SUCCESS;
}

static uint32_t mv_get_attr(uint32_t handle, const char *name, uint32_t t, void *val, uint32_t len)
{
    (void)handle; (void)t;
    if (!name || !val) return VMIOP_ERROR_INVAL;
    if (strcmp(name, "vmiop_vgpu_cap") == 0 && len >= 8U) {
        *(uint64_t *)val = 1ULL;
        return VMIOP_SUCCESS;
    }
    if (strcmp(name, "mv_type_id") == 0 && len >= 4U) {
        *(uint32_t *)val = g_mv_type_id;
        return VMIOP_SUCCESS;
    }
    if (strcmp(name, "mv_init_calls") == 0 && len >= 4U) {
        *(uint32_t *)val = g_mv_init_calls;
        return VMIOP_SUCCESS;
    }
    return VMIOP_ERROR_NOT_FOUND;
}

static uint32_t mv_set_attr(uint32_t handle, const char *name, uint32_t t, void *val, uint32_t len)
{
    (void)handle; (void)t;
    if (!name || !val) return VMIOP_ERROR_INVAL;
    if (strcmp(name, "vgpu_type_id") == 0 && len >= 8U) {
        g_mv_type_id = (uint32_t)(*(const uint64_t *)val);
    } else if (strcmp(name, "vmiop_vmm_migration_supported") == 0 && len >= 8U) {
        g_mv_mig_supported = *(const uint64_t *)val;
    }
    return VMIOP_SUCCESS;
}

static uint32_t mv_deliver(uint32_t h, vmiop_buffer_t *b) { (void)h; (void)b; return VMIOP_SUCCESS; }
static uint32_t mv_reset(uint32_t h) { (void)h; return g_mv_mig_supported ? VMIOP_SUCCESS : VMIOP_ERROR_INVAL; }
static uint32_t mv_mig_st(uint32_t h, uint32_t s) { (void)h; (void)s; return VMIOP_SUCCESS; }
static uint32_t mv_read_buf(uint32_t h, void *b, uint64_t m, uint64_t *w, uint64_t *p, void *r)
{
    (void)h; (void)b; (void)m; (void)r;
    if (w) *w = 0ULL;
    if (p) *p = 0ULL;
    return VMIOP_SUCCESS;
}
static uint32_t mv_write_buf(uint32_t h, void *b, uint64_t l, uint64_t r) { (void)h; (void)b; (void)l; (void)r; return VMIOP_SUCCESS; }
static uint32_t mv_mig_sz(uint32_t h, uint64_t *s) { (void)h; if (s) *s = 4096ULL; return VMIOP_SUCCESS; }

vmiop_plugin_t vmiop_display_vmiop_plugin = {
    .length                    = 0xa8U,
    .version                   = 0x50000U,
    .signature                 = VMIOP_PLUGIN_SIGNATURE,
    .name                      = "mock-vendor-vgpu",
    .plugin_class              = 1U,
    .input_classes             = 0x202U,
    .connect_downstream_required = 1U,
    .connect_upstream_required = 0U,
    .init_routine              = mv_init,
    .shutdown_routine          = mv_shutdown,
    .get_attribute             = mv_get_attr,
    .set_attribute             = mv_set_attr,
    .deliver_message           = mv_deliver,
    .reset_device              = mv_reset,
    .notify_migration_state    = mv_mig_st,
    .read_device_buffer        = mv_read_buf,
    .write_device_buffer       = mv_write_buf,
    .get_migration_data_size   = mv_mig_sz,
};
