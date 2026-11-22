#include "open_vgpu_plugin.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------------
 * Default env stubs matching libnvidia-vgpu.so.535.309.01 VER_2_0 layout
 * ------------------------------------------------------------------------- */
static uint32_t stub_inject_msi(uint64_t dev_handle, uint64_t a, uint32_t b)
{
    (void)dev_handle; (void)a; (void)b;
    return VMIOP_ERROR_NOT_FOUND;
}

static uint32_t stub_update_lfb(void)
{
    return VMIOP_SUCCESS;
}

static uint32_t stub_unpin_pages(uint64_t dev_handle, uint64_t *gpfn, uint64_t unused, uint32_t cnt)
{
    (void)dev_handle; (void)gpfn; (void)unused; (void)cnt;
    return VMIOP_ERROR_NOT_FOUND;
}

static uint32_t stub_get_guest_id_type(void)
{
    return 1U;
}

static uint32_t stub_rsv5(void)
{
    return VMIOP_SUCCESS;
}

vmiop_env_v2_t vmiop_display_vmiop_env = {
    .inject_msi        = stub_inject_msi,
    .update_lfb        = stub_update_lfb,
    .flags             = 0U,
    .pad14             = 0U,
    .unpin_pages       = stub_unpin_pages,
    .get_guest_id_type = stub_get_guest_id_type,
    .reserved28        = stub_rsv5,
    .flags30           = 0x0000000100000001ULL,
};

/* -------------------------------------------------------------------------
 * Hybrid Vendor Backend & Standalone State
 * ------------------------------------------------------------------------- */
#define MAX_PLUGIN_DEVS 16U

typedef struct {
    int               in_use;
    uint32_t          handle;
    uint32_t          vgpu_type_id;
    char              gpu_pci_id[64];
    uint8_t           vgpu_uuid[16];
    gp104_vgpu_dev_t  gp104;
} plug_dev_slot_t;

static pthread_mutex_t       g_plug_lock     = PTHREAD_MUTEX_INITIALIZER;
static int                   g_mode_checked  = 0;
static int                   g_is_hybrid     = 0;
static void                 *g_vendor_dl     = NULL;
static const vmiop_plugin_t *g_vendor_plug   = NULL;
static vmiop_env_v2_t       *g_vendor_env    = NULL;
static plug_dev_slot_t       g_devs[MAX_PLUGIN_DEVS];
static uint32_t              g_mig_supported = 0U;

static void sync_env_to_vendor(void)
{
    if (!g_vendor_env) return;
    g_vendor_env->inject_msi        = vmiop_display_vmiop_env.inject_msi;
    g_vendor_env->update_lfb        = vmiop_display_vmiop_env.update_lfb;
    g_vendor_env->flags             = vmiop_display_vmiop_env.flags;
    g_vendor_env->unpin_pages       = vmiop_display_vmiop_env.unpin_pages;
    g_vendor_env->get_guest_id_type = vmiop_display_vmiop_env.get_guest_id_type;
}

static void plugin_trace_event(const char *ev, uint32_t handle, const char *detail, uint32_t rc)
{
    const char *dir = getenv("OPEN_VGPU_TRACE_DIR");
    if (!dir || dir[0] == '\0') return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/vgpu_plugin_pid%ld.jsonl", dir, (long)getpid());
    FILE *fp = fopen(path, "a");
    if (!fp) return;
    fprintf(fp, "{\"ev\":\"%s\",\"handle\":%u,\"detail\":\"%s\",\"rc\":%u,\"mode\":\"%s\"}\n",
            ev ? ev : "", handle, detail ? detail : "", rc,
            g_is_hybrid ? "hybrid" : "standalone");
    fclose(fp);
}

static void ensure_backend_initialized(void)
{
    if (g_mode_checked) return;
    g_mode_checked = 1;

    const char *mode = getenv("OPEN_VGPU_PLUGIN_MODE");
    const char *vlib = getenv("OPEN_VGPU_VENDOR_LIB");
    if (!vlib || vlib[0] == '\0') {
        vlib = "/opt/nvidia-vgpu-535/lib/libnvidia-vgpu.so.535.309.01";
    }

    int want_hybrid = 0;
    if (mode && strcmp(mode, "standalone") == 0) {
        want_hybrid = 0;
    } else if (mode && strcmp(mode, "hybrid") == 0) {
        want_hybrid = 1;
    } else if (getenv("OPEN_VGPU_VENDOR_LIB") != NULL) {
        want_hybrid = 1;
    }

    if (!want_hybrid) {
        vmiop_log(2, "open-nvidia-vgpu: initialized in standalone clean-room GP104 mode");
        return;
    }

    g_vendor_dl = dlopen(vlib, RTLD_NOW | RTLD_LOCAL);
    if (!g_vendor_dl) {
        vmiop_log(1, "open-nvidia-vgpu: hybrid dlopen(%s) failed: %s; falling back to standalone",
                  vlib, dlerror());
        return;
    }

    const vmiop_plugin_t *vp =
        (const vmiop_plugin_t *)dlvsym(g_vendor_dl, "vmiop_display_vmiop_plugin", "VER_5_0");
    vmiop_env_v2_t *ve =
        (vmiop_env_v2_t *)dlvsym(g_vendor_dl, "vmiop_display_vmiop_env", "VER_2_0");
    if (!vp || !vp->signature || strcmp(vp->signature, VMIOP_PLUGIN_SIGNATURE) != 0) {
        vmiop_log(1, "open-nvidia-vgpu: invalid vendor plugin ABI in %s; closing", vlib);
        dlclose(g_vendor_dl);
        g_vendor_dl = NULL;
        return;
    }

    g_vendor_plug = vp;
    g_vendor_env  = ve;
    sync_env_to_vendor();
    g_is_hybrid = 1;
    vmiop_log(2, "open-nvidia-vgpu: initialized in hybrid trace-verified mode (%s)", vlib);
}

static plug_dev_slot_t *find_or_alloc_slot(uint32_t handle, int create)
{
    for (uint32_t i = 0; i < MAX_PLUGIN_DEVS; i++) {
        if (g_devs[i].in_use && g_devs[i].handle == handle) {
            return &g_devs[i];
        }
    }
    if (!create) return NULL;
    for (uint32_t i = 0; i < MAX_PLUGIN_DEVS; i++) {
        if (!g_devs[i].in_use) {
            memset(&g_devs[i], 0, sizeof(g_devs[i]));
            g_devs[i].in_use = 1;
            g_devs[i].handle = handle;
            g_devs[i].vgpu_type_id = 65U;
            return &g_devs[i];
        }
    }
    return NULL;
}

static uint32_t open_plug_init(uint32_t handle)
{
    pthread_mutex_lock(&g_plug_lock);
    ensure_backend_initialized();
    sync_env_to_vendor();
    const vmiop_plugin_t *vp = g_is_hybrid ? g_vendor_plug : NULL;
    plug_dev_slot_t *slot = vp ? NULL : find_or_alloc_slot(handle, 1);
    pthread_mutex_unlock(&g_plug_lock);

    if (vp && vp->init_routine) {
        uint32_t rc = vp->init_routine(handle);
        plugin_trace_event("plug_init", handle, "init_routine", rc);
        return rc;
    }

    if (!slot) return VMIOP_ERROR_NO_MEMORY;
    gp104_dev_init_defaults(&slot->gp104, handle, slot->vgpu_type_id);
    if (slot->gpu_pci_id[0]) {
        snprintf(slot->gp104.gpu_pci_bdf, sizeof(slot->gp104.gpu_pci_bdf), "%s", slot->gpu_pci_id);
    }
    memcpy(slot->gp104.vgpu_uuid, slot->vgpu_uuid, 16U);
    uint32_t rc = gp104_dev_start(&slot->gp104, &vmiop_display_vmiop_env);
    plugin_trace_event("plug_init", handle, slot->gp104.profile.name, rc);
    return rc;
}

static uint32_t open_plug_shutdown(uint32_t handle)
{
    pthread_mutex_lock(&g_plug_lock);
    ensure_backend_initialized();
    const vmiop_plugin_t *vp = g_is_hybrid ? g_vendor_plug : NULL;
    plug_dev_slot_t *slot = find_or_alloc_slot(handle, 0);
    pthread_mutex_unlock(&g_plug_lock);

    uint32_t rc = VMIOP_SUCCESS;
    if (vp && vp->shutdown_routine) {
        rc = vp->shutdown_routine(handle);
    } else if (slot) {
        gp104_dev_shutdown(&slot->gp104);
    }

    pthread_mutex_lock(&g_plug_lock);
    if (slot) slot->in_use = 0;
    pthread_mutex_unlock(&g_plug_lock);
    plugin_trace_event("plug_shutdown", handle, "shutdown_routine", rc);
    return rc;
}

static uint32_t open_plug_get_attr(uint32_t handle, const char *name,
                                   uint32_t type, void *val, uint32_t len)
{
    pthread_mutex_lock(&g_plug_lock);
    ensure_backend_initialized();
    sync_env_to_vendor();
    const vmiop_plugin_t *vp = g_is_hybrid ? g_vendor_plug : NULL;
    pthread_mutex_unlock(&g_plug_lock);

    if (vp && vp->get_attribute) {
        uint32_t rc = vp->get_attribute(handle, name, type, val, len);
        plugin_trace_event("plug_get_attr", handle, name ? name : "", rc);
        return rc;
    }

    if (!name || !val) return VMIOP_ERROR_INVAL;
    uint32_t rc = VMIOP_ERROR_NOT_FOUND;
    if (strcmp(name, "vmiop_vgpu_cap") == 0 && len >= 8U) {
        *(uint64_t *)val = 1ULL;
        rc = VMIOP_SUCCESS;
    } else if (strcmp(name, "vmiop_vmm_migration_supported") == 0 && len >= 8U) {
        *(uint64_t *)val = g_mig_supported;
        rc = VMIOP_SUCCESS;
    }
    plugin_trace_event("plug_get_attr", handle, name, rc);
    return rc;
}

static uint32_t open_plug_set_attr(uint32_t handle, const char *name,
                                   uint32_t type, void *val, uint32_t len)
{
    pthread_mutex_lock(&g_plug_lock);
    ensure_backend_initialized();
    sync_env_to_vendor();
    const vmiop_plugin_t *vp = g_is_hybrid ? g_vendor_plug : NULL;
    plug_dev_slot_t *slot = vp ? NULL : find_or_alloc_slot(handle, 1);
    pthread_mutex_unlock(&g_plug_lock);

    if (vp && vp->set_attribute) {
        uint32_t rc = vp->set_attribute(handle, name, type, val, len);
        plugin_trace_event("plug_set_attr", handle, name ? name : "", rc);
        return rc;
    }

    if (!name || !val || !slot) return VMIOP_ERROR_INVAL;
    if (strcmp(name, "vgpu_type_id") == 0 && len >= 4U) {
        slot->vgpu_type_id = (len >= 8U) ? (uint32_t)(*(const uint64_t *)val)
                                         : (*(const uint32_t *)val);
    } else if (strcmp(name, "gpu-pci-id") == 0) {
        snprintf(slot->gpu_pci_id, sizeof(slot->gpu_pci_id), "%s", (const char *)val);
    } else if (strcmp(name, "vgpu-dev-name") == 0 && len >= 8U) {
        const uint8_t *const *pp = (const uint8_t *const *)val;
        if (pp && *pp) memcpy(slot->vgpu_uuid, *pp, 16U);
    } else if (strcmp(name, "vmiop_vmm_migration_supported") == 0 && len >= 4U) {
        g_mig_supported = (len >= 8U) ? (uint32_t)(*(const uint64_t *)val)
                                      : (*(const uint32_t *)val);
    }
    plugin_trace_event("plug_set_attr", handle, name, VMIOP_SUCCESS);
    return VMIOP_SUCCESS;
}

static uint32_t open_plug_deliver_msg(uint32_t handle, vmiop_buffer_t *buf)
{
    pthread_mutex_lock(&g_plug_lock);
    ensure_backend_initialized();
    const vmiop_plugin_t *vp = g_is_hybrid ? g_vendor_plug : NULL;
    pthread_mutex_unlock(&g_plug_lock);

    if (vp && vp->deliver_message) {
        return vp->deliver_message(handle, buf);
    }
    (void)handle; (void)buf;
    return VMIOP_SUCCESS;
}

static uint32_t open_plug_reset(uint32_t handle)
{
    pthread_mutex_lock(&g_plug_lock);
    ensure_backend_initialized();
    const vmiop_plugin_t *vp = g_is_hybrid ? g_vendor_plug : NULL;
    plug_dev_slot_t *slot = find_or_alloc_slot(handle, 0);
    pthread_mutex_unlock(&g_plug_lock);

    if (vp && vp->reset_device) {
        uint32_t rc = vp->reset_device(handle);
        plugin_trace_event("plug_reset", handle, "reset_device", rc);
        return rc;
    }
    if (!slot) return VMIOP_ERROR_NOT_FOUND;
    gp104_dev_reset(&slot->gp104);
    plugin_trace_event("plug_reset", handle, "reset_device", VMIOP_SUCCESS);
    return VMIOP_SUCCESS;
}

static uint32_t open_plug_notify_mig(uint32_t handle, uint32_t st)
{
    if (g_is_hybrid && g_vendor_plug && g_vendor_plug->notify_migration_state) {
        return g_vendor_plug->notify_migration_state(handle, st);
    }
    (void)handle; (void)st;
    return VMIOP_SUCCESS;
}

static uint32_t open_plug_read_buf(uint32_t handle, void *buf, uint64_t max_len,
                                   uint64_t *written, uint64_t *pending, void *rsv)
{
    if (g_is_hybrid && g_vendor_plug && g_vendor_plug->read_device_buffer) {
        return g_vendor_plug->read_device_buffer(handle, buf, max_len, written, pending, rsv);
    }
    (void)handle; (void)buf; (void)max_len; (void)rsv;
    if (written) *written = 0ULL;
    if (pending) *pending = 0ULL;
    return VMIOP_SUCCESS;
}

static uint32_t open_plug_write_buf(uint32_t handle, void *buf, uint64_t len, uint64_t rsv)
{
    if (g_is_hybrid && g_vendor_plug && g_vendor_plug->write_device_buffer) {
        return g_vendor_plug->write_device_buffer(handle, buf, len, rsv);
    }
    (void)handle; (void)buf; (void)len; (void)rsv;
    return VMIOP_SUCCESS;
}

static uint32_t open_plug_mig_size(uint32_t handle, uint64_t *size_out)
{
    if (g_is_hybrid && g_vendor_plug && g_vendor_plug->get_migration_data_size) {
        return g_vendor_plug->get_migration_data_size(handle, size_out);
    }
    (void)handle;
    if (size_out) *size_out = 0ULL;
    return VMIOP_SUCCESS;
}

static uint32_t open_plug_rsv_zero(void)
{
    return VMIOP_SUCCESS;
}

vmiop_plugin_t vmiop_display_vmiop_plugin = {
    .length                    = 0xa8U,
    .version                   = 0x50000U,
    .signature                 = VMIOP_PLUGIN_SIGNATURE,
    .name                      = "open-nvidia-vgpu",
    .plugin_class              = 1U,
    .input_classes             = 0x202U,
    .connect_downstream_required = 1U,
    .connect_upstream_required = 0U,
    .init_routine              = open_plug_init,
    .shutdown_routine          = open_plug_shutdown,
    .get_attribute             = open_plug_get_attr,
    .set_attribute             = open_plug_set_attr,
    .deliver_message           = open_plug_deliver_msg,
    .reserved50                = NULL,
    .reserved58                = NULL,
    .reset_device              = open_plug_reset,
    .notify_migration_state    = open_plug_notify_mig,
    .read_device_buffer        = open_plug_read_buf,
    .write_device_buffer       = open_plug_write_buf,
    .get_migration_data_size   = open_plug_mig_size,
    .reserved88                = {
        (void *)open_plug_rsv_zero,
        (void *)open_plug_rsv_zero,
        (void *)open_plug_rsv_zero,
        (void *)open_plug_rsv_zero,
    },
};
