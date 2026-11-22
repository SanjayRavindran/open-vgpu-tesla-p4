#ifndef OPEN_VGPU_PLUGIN_H
#define OPEN_VGPU_PLUGIN_H

#include "vmiop_env.h"

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Versioned Plugin Env ABI (vmiop_display_vmiop_env @ VER_2_0, 0x38 bytes)
 * ------------------------------------------------------------------------- */
typedef uint32_t (*vmiop_env_inject_msi_fn_t)(uint64_t dev_handle, uint64_t a, uint32_t b);
typedef uint32_t (*vmiop_env_update_lfb_fn_t)(void);
typedef uint32_t (*vmiop_env_unpin_pages_fn_t)(uint64_t dev_handle, uint64_t *gpfn,
                                               uint64_t unused, uint32_t count);
typedef uint32_t (*vmiop_env_get_guest_id_type_fn_t)(void);
typedef uint32_t (*vmiop_env_rsv5_fn_t)(void);

typedef struct {
    vmiop_env_inject_msi_fn_t         inject_msi;        /* +0x00 */
    vmiop_env_update_lfb_fn_t         update_lfb;        /* +0x08 */
    uint32_t                          flags;             /* +0x10 */
    uint32_t                          pad14;             /* +0x14 */
    vmiop_env_unpin_pages_fn_t        unpin_pages;       /* +0x18 */
    vmiop_env_get_guest_id_type_fn_t  get_guest_id_type; /* +0x20 */
    vmiop_env_rsv5_fn_t               reserved28;        /* +0x28 */
    uint64_t                          flags30;           /* +0x30 (0x00000001_00000001) */
} vmiop_env_v2_t;

_Static_assert(sizeof(vmiop_env_v2_t) == 0x38U, "vmiop_display_vmiop_env VER_2_0 must be 0x38 bytes");
_Static_assert(sizeof(vmiop_plugin_t) == 0xa8U, "vmiop_display_vmiop_plugin VER_5_0 must be 0xa8 bytes");

/* -------------------------------------------------------------------------
 * VRPC Wire Protocol ("VRPC" = 0x43505256, header version 0x03000000)
 * ------------------------------------------------------------------------- */
#define OPEN_VRPC_SIGNATURE       0x43505256U
#define OPEN_VRPC_HEADER_VERSION  0x03000000U
#define OPEN_VRPC_PENDING         0xffffffffU

typedef struct {
    uint32_t version;      /* +0x00 */
    uint32_t signature;    /* +0x04 ("VRPC") */
    uint32_t total_length; /* +0x08 (>= 0x20) */
    uint32_t function;     /* +0x0c */
    uint32_t result;       /* +0x10 (NV_STATUS; 0xffffffff pending) */
    uint32_t spare;        /* +0x14 */
    uint32_t sequence;     /* +0x18 */
    uint32_t flags;        /* +0x1c */
} open_vrpc_hdr_t;

_Static_assert(sizeof(open_vrpc_hdr_t) == 0x20U, "VRPC header must be 32 bytes");

enum {
    VRPC_FN_NOP                    = 0x00U,
    VRPC_FN_SET_GUEST_SYSTEM_INFO  = 0x01U,
    VRPC_FN_ALLOC_ROOT             = 0x02U,
    VRPC_FN_ALLOC_DEVICE_535       = 0x03U,
    VRPC_FN_ALLOC_DEVICE           = 0x04U,
    VRPC_FN_ALLOC_SUBDEVICE        = 0x05U,
    VRPC_FN_ALLOC_MEMORY           = 0x06U,
    VRPC_FN_ALLOC_OBJECT           = 0x07U,
    VRPC_FN_FREE                   = 0x09U,
    VRPC_FN_CONTROL                = 0x0aU,
    VRPC_FN_ALLOC_CHANNEL_DMA      = 0x0bU,
    VRPC_FN_MAP_MEMORY             = 0x0cU,
    VRPC_FN_UNMAP_MEMORY           = 0x0dU,
    VRPC_FN_MAP_MEMORY_DMA         = 0x0eU,
    VRPC_FN_UNMAP_MEMORY_DMA       = 0x0fU,
    VRPC_FN_BIND_CONTEXT_DMA       = 0x10U,
    VRPC_FN_SET_OBJECT             = 0x13U,
    VRPC_FN_IDLE_CHANNELS          = 0x14U,
    VRPC_FN_OP_15                  = 0x15U,
    VRPC_FN_IDLE_CHANNELS_535      = 0x16U,
    VRPC_FN_OP_17                  = 0x17U,
    VRPC_FN_RM_API_CONTROL         = 0x1aU,
    VRPC_FN_OP_1B                  = 0x1bU,
    VRPC_FN_OP_1C                  = 0x1cU,
    VRPC_FN_GET_ENGINE_INFO        = 0x1eU,
    VRPC_FN_SET_GUEST_FB_INFO      = 0x20U,
    VRPC_FN_SET_GUEST_SEMA_MEM     = 0x21U,
    VRPC_FN_UNSET_GUEST_SEMA_MEM   = 0x22U,
    VRPC_FN_SET_GUEST_BAR1_INFO    = 0x24U,
    VRPC_FN_LOG                    = 0x25U,
    VRPC_FN_OP_2B                  = 0x2bU,
    VRPC_FN_GET_STATIC_INFO_535    = 0x32U,
    VRPC_FN_OP_34                  = 0x34U,
    VRPC_FN_OP_38                  = 0x38U,
    VRPC_FN_OP_39                  = 0x39U,
    VRPC_FN_OP_3B                  = 0x3bU,
    VRPC_FN_OP_3D                  = 0x3dU,
    VRPC_FN_OP_3E                  = 0x3eU,
    VRPC_FN_SET_GUEST_SYSTEM_EXT   = 0x3fU,
    VRPC_FN_SET_GUEST_BRANCH_VER   = 0x40U,
    VRPC_FN_GET_STATIC_INFO_2      = 0x4cU,
    VRPC_FN_GET_CONSOL_STATIC_INFO = 0x4fU,
    VRPC_FN_BATCH_50               = 0x50U,
    VRPC_FN_GET_STATIC_INFO        = 0x55U,
    VRPC_FN_UPDATE_BAR_INFO        = 0x56U,
    VRPC_FN_OP_5B                  = 0x5bU,
    VRPC_FN_OP_5C                  = 0x5cU,
    VRPC_FN_OP_61                  = 0x61U,
    VRPC_FN_OP_62                  = 0x62U,
    VRPC_FN_OP_6E                  = 0x6eU,
    VRPC_FN_OP_70                  = 0x70U,
    VRPC_FN_OP_71                  = 0x71U,
    VRPC_FN_OP_72                  = 0x72U,
    VRPC_FN_OP_78                  = 0x78U,
    VRPC_FN_OP_79                  = 0x79U,
    VRPC_FN_OP_7A                  = 0x7aU,
    VRPC_FN_OP_7C                  = 0x7cU,
    VRPC_FN_OP_7D                  = 0x7dU,
    VRPC_FN_OP_7E                  = 0x7eU,
    VRPC_FN_OP_8E                  = 0x8eU,
    VRPC_FN_OP_8F                  = 0x8fU,
    VRPC_FN_SYNC_9C                = 0x9cU,
    VRPC_FN_MAX_OPCODE             = 0xd0U,
};

/* -------------------------------------------------------------------------
 * GP104 Virtualization Constants (BAR0 0x8f000 Mailbox, VMMU, CHIDMGR, FRL)
 * ------------------------------------------------------------------------- */
#define GP104_PCI_CFG_BYTES        4096U
#define GP104_BAR0_SIZE            (16ULL * 1024ULL * 1024ULL)
#define GP104_BAR1_DEFAULT_SIZE    (256ULL * 1024ULL * 1024ULL)
#define GP104_BAR3_SIZE            (32ULL * 1024ULL * 1024ULL)

#define GP104_NV_VGPU_BASE         0x0008f000ULL
#define GP104_NV_VGPU_SIZE         0x00001000ULL

#define NV_VGPU_REG_SEND_RING_LO   0x00U
#define NV_VGPU_REG_SEND_RING_HI   0x04U
#define NV_VGPU_REG_SEND_RING_PUT  0x08U
#define NV_VGPU_REG_SEND_RING_GET  0x0cU
#define NV_VGPU_REG_RECV_RING_LO   0x10U
#define NV_VGPU_REG_RECV_RING_HI   0x14U
#define NV_VGPU_REG_RECV_RING_PUT  0x18U
#define NV_VGPU_REG_RECV_RING_GET  0x1cU
#define NV_VGPU_REG_CONFIG         0x20U
#define NV_VGPU_REG_FB_PAGES_M1    0x24U
#define NV_VGPU_REG_INTR_STATUS    0x28U
#define NV_VGPU_REG_INTR_ENABLE    0x2cU
#define NV_VGPU_REG_EVENT_RING_LO  0x30U
#define NV_VGPU_REG_EVENT_RING_HI  0x34U
#define NV_VGPU_REG_EVENT_RING_PUT 0x38U
#define NV_VGPU_REG_EVENT_RING_GET 0x3cU
#define NV_VGPU_REG_RPC_DOORBELL   0x50U
#define NV_VGPU_REG_MSG_RING_CTL   0x54U
#define NV_VGPU_REG_GUEST_ID_58    0x58U
#define NV_VGPU_REG_GUEST_INFO     0x5cU
#define NV_VGPU_REG_CTL_68         0x68U
#define NV_VGPU_REG_RING2_LO       0x80U
#define NV_VGPU_REG_RING2_HI       0x84U
#define NV_VGPU_REG_RING3_LO       0x88U
#define NV_VGPU_REG_RING3_HI       0x8cU
#define NV_VGPU_REG_RING4_LO       0x90U
#define NV_VGPU_REG_RING4_HI       0x94U
#define NV_VGPU_REG_UVM_SEND_LO    0xb0U
#define NV_VGPU_REG_UVM_SEND_HI    0xb4U
#define NV_VGPU_REG_UVM_RECV_LO    0xb8U
#define NV_VGPU_REG_UVM_RECV_HI    0xbcU
#define NV_VGPU_REG_UVM_DOORBELL   0xc0U
#define NV_VGPU_REG_UVM_MSG_CTL    0xc4U

#define GP104_VMMU_SEG_BYTES       (256ULL * 1024ULL * 1024ULL)
#define GP104_MAX_VMMU_SEGS        32U
#define GP104_MAX_CHANNELS         128U
#define GP104_MAX_RM_OBJECTS       128U
#define GP104_MAX_GUEST_MAPS       32U

typedef struct {
    uint32_t vgpu_type_id;
    const char *name;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsys_vendor_id;
    uint16_t subsys_device_id;
    uint64_t profile_bytes;
    uint64_t usable_fb_bytes;
    uint64_t bar1_bytes;
    uint32_t frl_fps;
    int      ecc_supported;
} gp104_profile_spec_t;

typedef struct {
    uint64_t gpa;
    uint64_t size;
    void    *hva;
    uint32_t mem_id;
    int      mapped;
} gp104_ring_map_t;

typedef struct {
    uint32_t h_client;
    uint32_t h_parent;
    uint32_t h_object;
    uint32_t h_class;
    int      in_use;
} gp104_rm_obj_t;

typedef struct {
    uint32_t dev_handle;
    int      initialized;
    char     gpu_pci_bdf[64];
    uint8_t  vgpu_uuid[16];
    gp104_profile_spec_t profile;

    /* Virtual PCI config space & region handles */
    uint8_t  pci_cfg[GP104_PCI_CFG_BYTES];
    uint32_t bar0_region_id;
    uint32_t bar1_region_id;
    uint32_t bar3_region_id;
    uint64_t bar0_gpa;
    uint64_t bar1_gpa;
    uint64_t bar3_gpa;
    uint8_t  bar1_backing_mapped;

    /* BAR0 registers + NV_VGPU (0x8f000..0x8ffff) mailbox */
    uint32_t pmc_boot0;
    uint32_t pmc_boot42;
    uint32_t pmc_enable;
    uint32_t pmc_intr_0;
    uint32_t pmc_intr_en_0;
    uint32_t vgpu_regs[0x40]; /* dwords 0x00..0xfc inside 0x8f000 */
    uint64_t timer_base_ns;

    gp104_ring_map_t send_ring;
    gp104_ring_map_t recv_ring;
    gp104_ring_map_t event_ring;
    gp104_ring_map_t msg_page;

    /* VRPC, VMMU, CHIDMGR, FRL, RM Object Table */
    int      guest_vgx_initialized;
    uint32_t guest_os_type;
    uint32_t guest_vgx_major;
    uint32_t guest_vgx_minor;
    uint64_t vrpc_total_calls;
    uint64_t vrpc_opcode_counts[VRPC_FN_MAX_OPCODE];
    uint64_t msi_injected_count;

    uint32_t vmmu_seg_count;
    uint64_t vmmu_seg_phys[GP104_MAX_VMMU_SEGS];
    uint8_t  chid_bitmap[GP104_MAX_CHANNELS];
    uint32_t chid_base;
    uint32_t frl_fps;
    uint64_t frl_interval_ns;
    gp104_rm_obj_t rm_objs[GP104_MAX_RM_OBJECTS];
} gp104_vgpu_dev_t;

/* Core clean-room GP104 engine entrypoints */
void     gp104_profile_lookup(uint32_t vgpu_type_id, gp104_profile_spec_t *out);
void     gp104_dev_init_defaults(gp104_vgpu_dev_t *dev, uint32_t dev_handle, uint32_t vgpu_type_id);
uint32_t gp104_dev_start(gp104_vgpu_dev_t *dev, const vmiop_env_v2_t *env);
void     gp104_dev_reset(gp104_vgpu_dev_t *dev);
void     gp104_dev_shutdown(gp104_vgpu_dev_t *dev);

uint32_t gp104_pci_cfg_access(void *priv, uint32_t is_write, uint32_t emul_space,
                              uint64_t offset, uint32_t width, void *data, uint32_t *dummy);
uint32_t gp104_bar0_access(void *priv, uint32_t is_write, uint32_t emul_space,
                           uint64_t offset, uint32_t width, void *data, uint32_t *dummy);

uint32_t gp104_handle_vrpc_buffer(gp104_vgpu_dev_t *dev, uint8_t *buf, size_t buf_len);
int      gp104_vmmu_map_range(gp104_vgpu_dev_t *dev, uint64_t phys_start, uint64_t len);
int      gp104_chid_alloc(gp104_vgpu_dev_t *dev, uint32_t guest_chid, uint32_t *out_phys_chid);
void     gp104_chid_free(gp104_vgpu_dev_t *dev, uint32_t guest_chid);

extern vmiop_env_v2_t vmiop_display_vmiop_env;
extern vmiop_plugin_t vmiop_display_vmiop_plugin;

#ifdef __cplusplus
}
#endif

#endif /* OPEN_VGPU_PLUGIN_H */
