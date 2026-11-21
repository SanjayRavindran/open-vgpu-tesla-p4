#ifndef VMIOP_ENV_H
#define VMIOP_ENV_H

#include "nv_vgpu_vfio_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VMIOP_PLUGIN_SIGNATURE "VMIOP_PLUGIN_SIGNATURE"
#define VMIOP_MAX_INSTANCES    16U

/* -------------------------------------------------------------------------
 * VMIOP Buffer & Plugin ABI (VER_2_0 env / VER_5_0 plugin)
 * ------------------------------------------------------------------------- */
struct vmiop_buffer;
typedef uint32_t (*vmiop_buffer_release_fn_t)(struct vmiop_buffer *buf);

typedef struct {
    void    *base;
    uint64_t length;
} vmiop_iov_t;

typedef struct vmiop_buffer {
    uint64_t pad0[2];                  /* +0x00 */
    uint32_t src_class;                /* +0x10 */
    uint32_t dst_class;                /* +0x14 */
    vmiop_buffer_release_fn_t release; /* +0x18 */
    uint32_t refcnt;                   /* +0x20 */
    uint32_t element_count;            /* +0x24 */
    vmiop_iov_t *elements;             /* +0x28 */
    uint32_t pad30;                    /* +0x30 (non-zero forces props refresh) */
    uint32_t pad34;                    /* +0x34 */
    uint32_t total_alloc_bytes;        /* +0x38 */
    uint32_t pad3c;                    /* +0x3c */
    /* +0x40: element_count vmiop_iov_t structs followed by payload */
} vmiop_buffer_t;

typedef struct {
    uint32_t length;                     /* +0x00 (0x68 presentation, 0xa8 vgpu) */
    uint32_t version;                    /* +0x04 (0x20000 or 0x50000) */
    const char *signature;               /* +0x08 ("VMIOP_PLUGIN_SIGNATURE") */
    const char *name;                    /* +0x10 */
    uint32_t plugin_class;               /* +0x18 */
    uint32_t input_classes;              /* +0x1c */
    uint32_t connect_downstream_required;/* +0x20 */
    uint32_t connect_upstream_required;  /* +0x24 */
    uint32_t (*init_routine)(uint32_t handle);                 /* +0x28 */
    uint32_t (*shutdown_routine)(uint32_t handle);             /* +0x30 */
    uint32_t (*get_attribute)(uint32_t handle, const char *name,
                              uint32_t type, void *val, uint32_t len); /* +0x38 */
    uint32_t (*set_attribute)(uint32_t handle, const char *name,
                              uint32_t type, void *val, uint32_t len); /* +0x40 */
    uint32_t (*deliver_message)(uint32_t handle, vmiop_buffer_t *buf); /* +0x48 */
    void *reserved50;                    /* +0x50 */
    void *reserved58;                    /* +0x58 */
    uint32_t (*reset_device)(uint32_t handle);                         /* +0x60 */
    uint32_t (*notify_migration_state)(uint32_t handle, uint32_t st);  /* +0x68 */
    uint32_t (*read_device_buffer)(uint32_t handle, void *buf, uint64_t max_len,
                                   uint64_t *written, uint64_t *pending, void *rsv); /* +0x70 */
    uint32_t (*write_device_buffer)(uint32_t handle, void *buf, uint64_t len,
                                    uint64_t rsv); /* +0x78 */
    uint32_t (*get_migration_data_size)(uint32_t handle, uint64_t *size_out); /* +0x80 */
    void *reserved88[4];
} vmiop_plugin_t;

typedef uint32_t (*vmiop_emul_access_fn_t)(void *priv,
                                           uint32_t is_write,
                                           uint32_t emul_space,
                                           uint64_t offset,
                                           uint32_t width,
                                           void *data,
                                           uint32_t *dummy);

typedef struct {
    uint64_t count;
    uint64_t start_gpfn;
} vmiop_dirty_range_t;

/* Console framebuffer geometry exposed by presentation plugin */
typedef struct {
    uint32_t surface_id;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t stride;
    uint32_t bpp;
    uint32_t bytes_per_pixel;
    uint32_t depth;
    uint32_t pos_x;
    uint32_t pos_y;
    uint32_t is_bgr;
    int      started;
    int      blanked;
    uint64_t last_update_ns;
    void    *buffer;
    size_t   buffer_size;
} vmiope_console_state_t;

/* -------------------------------------------------------------------------
 * Exported VMIOP environment API (linked dynamically via -Wl,--export-dynamic)
 * ------------------------------------------------------------------------- */
uint32_t vmiop_get_guest_id_type(void);
uint32_t vmiop_get_page_size(uint64_t *page_size);
uint32_t vmiop_get_guest_id(uint64_t out_uuid[2]);
uint32_t vmiop_memory_alloc_internal(size_t size, void **out_ptr, int zero);
uint32_t vmiop_memory_free_internal(void *ptr, size_t size);
uint32_t vmiop_buffer_alloc(vmiop_buffer_t **out_buf, uint32_t src_class,
                            uint32_t dst_class, uint32_t count, uint32_t data_len);
uint32_t vmiop_buffer_free(vmiop_buffer_t *buf);

uint32_t vmiop_lock_alloc(uint32_t *out_id);
uint32_t vmiop_lock_free(uint32_t id);
uint32_t vmiop_lock(uint32_t id, int trylock);
uint32_t vmiop_unlock(uint32_t id);

uint32_t vmiop_spinlock_alloc(uint32_t *out_id);
uint32_t vmiop_spinlock_free(uint32_t id);
uint32_t vmiop_spin_lock(uint32_t id, int trylock);
uint32_t vmiop_spin_unlock(uint32_t id);

uint32_t vmiop_cv_alloc(uint32_t *out_id);
uint32_t vmiop_cv_free(uint32_t id);
uint32_t vmiop_cv_wait(uint32_t lock_id, uint32_t cv_id, uint64_t abs_ns);
uint32_t vmiop_cv_signal(uint32_t cv_id);
uint32_t vmiop_cv_broadcast(uint32_t cv_id);

uint32_t vmiop_thread_event_alloc(uint32_t *out_id);
uint32_t vmiop_thread_event_free(uint32_t id);
uint32_t vmiop_thread_event_wait(uint32_t id, uint64_t abs_ns, int clear);
uint32_t vmiop_thread_event_post(uint32_t id, int single);
uint32_t vmiop_thread_get_time(uint64_t *out_ns);
uint32_t vmiop_thread_alloc(void *arg, void (*fn)(uint32_t handle, void *arg), uint32_t *out_handle);
uint32_t vmiop_thread_join(uint32_t handle);

uint32_t vmiop_log(uint32_t level, const char *fmt, ...);
uint32_t vmiop_convert_value(uint32_t in_type, const void *in_buf, size_t in_size,
                             uint32_t out_type, void *out_buf, size_t out_size);
uint32_t vmiop_config_get(uint32_t dev_handle, const char *key, char **out_val);
uint32_t vmiop_find_plugin(uint32_t *id_inout, const vmiop_plugin_t **out_plugin);
uint32_t vmiop_deliver_message(uint32_t src_id, vmiop_buffer_t *buf, uint32_t dir);

uint32_t vmiop_register_emul_device(void);
uint32_t vmiop_register_emul_device_v2(uint32_t handle_in, void *priv, void *access_cb,
                                       void *tag, uint32_t *handle_out);
uint32_t vmiop_unregister_emul_device(uint32_t handle);

uint32_t vmiop_create_guest_region(void);
uint32_t vmiop_create_guest_region_v2(uint32_t dev_handle, uint64_t length, void *access_cb,
                                      void *priv, int space, uint32_t *out_id);
uint32_t vmiop_relocate_guest_region(uint32_t region_id, uint64_t start);
uint32_t vmiop_map_guest_region(uint32_t dev_handle_or_id, uint64_t pmmio_start,
                                uint64_t reserved, uint64_t size, uint64_t vmmio_start, int add);
uint32_t vmiop_update_guest_lfb(uint64_t unused, int64_t addr);

uint32_t vmiop_map_guest_memory(void);
uint32_t vmiop_map_guest_memory_v2(uint32_t dev_handle, uint64_t gpa, uint64_t size,
                                   void **out_hva, uint32_t prot_flags, uint32_t *out_id);
uint32_t vmiop_unmap_guest_memory(uint32_t mem_id);

uint32_t vmiop_pin_guest_pages(void);
uint32_t vmiop_pin_guest_pages_v2(uint32_t dev_handle, const uint64_t *gpfn,
                                  uint64_t *hpfn, uint32_t count);

uint32_t vmiop_control_interrupt(uint32_t dev_handle, uint32_t idx, int enable);
uint32_t vmiop_set_su(void);
uint32_t vmiop_drop_su(void);
uint32_t vmiop_set_vm_migration_cap(uint8_t enabled);
uint32_t vmiop_set_swizzid(uint32_t dev_handle, uint8_t enable, uint32_t swizz_id);
uint32_t vmiop_set_guest_dirty_pages(uint64_t count, const vmiop_dirty_range_t *ranges);
uint32_t vmiop_unhandled_config_access(uint32_t dev_handle, int is_write,
                                       uint64_t offset, uint64_t width, void *data);

uint32_t vmiop_vga_init(void);
uint32_t vmiop_vga_shutdown(void);
uint32_t vmiop_vga_set_callback_table(void);
uint32_t vmiop_vga_ioport_access(void);
void     vmiop_vga_set_VGA_state(int state);
int      vmiop_vga_in_VGA_state(void);
uint32_t vmiop_vga_update_frame_buffer(int present);

extern vmiop_plugin_t vmiop_presentation;

/* -------------------------------------------------------------------------
 * Internal daemon / unit-test management helpers
 * ------------------------------------------------------------------------- */
typedef void (*vmiope_log_hook_fn_t)(uint32_t level, const char *label, const char *msg);
typedef int  (*vmiope_vfio_ioctl_fn_t)(int fd, unsigned long req, void *arg);

void vmiope_reset_all(void);
void vmiope_set_log_hook(vmiope_log_hook_fn_t hook);
void vmiope_set_vfio_ioctl_fn(vmiope_vfio_ioctl_fn_t fn);
void vmiope_set_guest_uuid(const uint8_t uuid[VM_UUID_SIZE]);
void vmiope_set_search_path(const char *path);

uint32_t vmiope_initialize_env(void);
void     vmiope_env_lock(int *was_locked_out);
void     vmiope_env_unlock(int was_locked);

int      vmiope_parse_config(const char *cfg, uint32_t inst);
void     vmiope_free_config(uint32_t inst);
const char *vmiope_lookup_config_raw(const char *prefixed_key, uint32_t inst);

/* Worker per-instance state management */
typedef struct {
    int      active;
    uint32_t inst_idx;
    int      vgpu_fd;
    uint16_t vgpu_id;
    uint32_t gpu_pci_id;
    uint32_t gpu_pci_bdf;
    uint32_t pci_domain;
    uint8_t  pci_bus;
    uint8_t  pci_dev;
    uint8_t  pci_func;
    uint8_t  mdev_uuid[VM_UUID_SIZE];
    char     dev_path[128];
    char     config_params[VGPU_CONFIG_PARAMS_MAX_LENGTH];
    uint32_t dev_handle;
    uint32_t pres_handle;
    uint32_t hClient;
    uint32_t hDevice;
    uint32_t hSubDevice;
    uint32_t hVgpuConfig;
    uint32_t hSmcPartRef;
    uint8_t  swizz_enabled;
    uint32_t swizz_id;
    uint32_t teardown_stage;
    uint8_t  shutting_down;
    uint8_t  resetting;

    /* Event fds on /dev/nvidia-vgpu<vgpuId> */
    int fd_shutdown;
    int fd_reg_access;
    int fd_bar_info;
    int fd_reboot;
    int fd_console;
    int fd_attach;
    int fd_mig_state;
    int fd_mig_read;
    int fd_mig_write;
    int fd_mig_size;

    /* Migration & console buffers */
    void *mig_buffer;
    void *staging_buffer;
    uint32_t mig_stage;
    vmiope_console_state_t console;
} vmiope_instance_t;

vmiope_instance_t *vmiope_get_instance(uint32_t inst);
uint32_t vmiope_get_instance_count(void);
void     vmiope_set_instance_count(uint32_t count);
void     vmiope_init_instance_slot(uint32_t inst, int vgpu_fd, uint16_t vgpu_id,
                                   uint32_t gpu_pci_id, uint32_t gpu_pci_bdf,
                                   const uint8_t uuid[VM_UUID_SIZE],
                                   const char *cfg_params);

int      vmiope_register_plugin_internal(const vmiop_plugin_t *plugin, uint32_t *out_handle);
int      vmiope_unregister_plugin_internal(uint32_t handle);
int      vmiope_connect_plugins_internal(uint32_t upstream_id, uint32_t downstream_id);
int      vmiope_process_configuration(const char *cfg, uint32_t vgpu_type_id,
                                      const char *pci_bdf_str, uint32_t inst);

void     vmiope_cleanup_instance(uint32_t inst);
void     vmiope_set_migration_cap_byte(uint8_t val);
uint8_t  vmiope_get_migration_cap_byte(void);

int      vmiope_handle_reg_access(uint32_t inst);
int      vmiope_plugin_reset(uint32_t inst);
int      vmiope_plugin_notify_mig_state(uint32_t inst, uint32_t state);
int      vmiope_plugin_read_dev_buf(uint32_t inst, void *buf, uint64_t max_len,
                                    uint64_t *written, uint64_t *pending);
int      vmiope_plugin_write_dev_buf(uint32_t inst, void *buf, uint64_t len);
int      vmiope_plugin_get_mig_data_size(uint32_t inst, uint64_t *size_out);

/* Helpers to start/stop console & test registered space handlers directly */
void     vmiope_console_start(uint32_t inst);
void     vmiope_console_stop(uint32_t inst);
void     vmiope_console_update_props(uint32_t inst, const uint32_t *props5);
int      vmiope_dispatch_emul_space(uint32_t inst, VGPU_EMUL_SPACE_T space,
                                    uint64_t offset, uint32_t width,
                                    int is_write, uint8_t data[4]);

#ifdef __cplusplus
}
#endif

#endif /* VMIOP_ENV_H */
