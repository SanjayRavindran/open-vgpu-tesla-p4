#ifndef OPEN_VGPU_RM_H
#define OPEN_VGPU_RM_H

#include "../open-vgpu-mgr/nv_vgpu_vfio_abi.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Additional 535.309.01 RM Escapes, Classes, Structs & Controls for Phase 1B */
#ifndef NV_ESC_RM_IDLE_CHANNELS
#define NV_ESC_RM_IDLE_CHANNELS 0x30U
#endif
#ifndef NV_ESC_RM_DUP_OBJECT
#define NV_ESC_RM_DUP_OBJECT    0x34U
#endif

#ifndef NV01_ROOT
#define NV01_ROOT               0x00000000U
#endif
#ifndef NVA082_HOST_VGPU_DEVICE
#define NVA082_HOST_VGPU_DEVICE 0x0000a082U
#endif

/* Control Commands verified from libnvidia-vgpu.so.535.309.01 REA decompilation */
#define NV0000_CTRL_CMD_SYSTEM_GET_P2P_CAPS_V2     0x00000127U
#define NV0000_CTRL_CMD_SYSTEM_GET_P2P_CAPS_MATRIX 0x0000013aU
#define NV0000_CTRL_CMD_CLIENT_SET_META_0901       0x00000901U
#define NV0000_CTRL_CMD_CLIENT_DISABLE_USERD_ISOL  0x00000902U

#define NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE 0x00800289U
#define NV0080_CTRL_CMD_DMA_0x170F                 0x0080170fU
#define NV0080_CTRL_CMD_FB_0x1812                  0x00801812U

#define NV2080_CTRL_CMD_GPU_GET_INFO_V2            0x20800102U
#define NV2080_CTRL_CMD_GPU_GET_SIMULATION_INFO    0x2080012fU
#define NV2080_CTRL_CMD_GR_GET_0x1208              0x20801208U
#define NV2080_CTRL_CMD_GR_GET_0x1210              0x20801210U
#define NV2080_CTRL_CMD_GR_GFXP_PREEMPT            0x20801211U
#define NV2080_CTRL_CMD_BUS_GET_PCI_INFO           0x20801801U
#define NV2080_CTRL_CMD_FB_0x200A                  0x2080200aU
#define NV2080_CTRL_CMD_PERF_0x2A02                0x20802a02U
#define NV2080_CTRL_CMD_PERF_0x3002                0x20803002U
#define NV2080_CTRL_CMD_GR_CTXSW_ZCULL             0x20809004U

#define NV83DE_CTRL_CMD_DEBUG_0x0309               0x83de0309U
#define NV9096_CTRL_CMD_SET_ZBC_0x0107             0x90960107U
#define NVA06C_CTRL_CMD_GPFIFO_SCHEDULE            0xa06c0101U
#define NVA06C_CTRL_CMD_TSG_0x0103                 0xa06c0103U
#define NVA06F_CTRL_CMD_GPFIFO_SCHEDULE            0xa06f0103U

#define NVA081_CTRL_CMD_VGPU_CONFIG_GET_PGPU_INFO   0xa0810112U
#define NVA081_CTRL_CMD_VGPU_CONFIG_GET_SCHED_POL   0xa0810115U

#define NVA082_CTRL_CMD_SET_GUEST_INFO             0xa0820103U
#define NVA082_CTRL_CMD_SET_LICENSE_STATE          0xa0820104U
#define NVA082_CTRL_CMD_GET_ENCODER_CAPACITY       0xa082010dU
#define NVA082_CTRL_CMD_SET_FB_USAGE               0xa0820110U
#define NVA082_CTRL_CMD_GET_P2P_CAP                0xa082012cU

typedef struct {
    NvU32 hClient;
    NvU32 hParent;
    NvU32 hObject;
    NvU32 hClientSrc;
    NvU32 hObjectSrc;
    NvU32 flags;
    NvU32 status;
} NVOS55_DUP_PARAMETERS;

_Static_assert(sizeof(NVOS55_DUP_PARAMETERS) == 0x1cU, "NVOS55_DUP_PARAMETERS must be 28 bytes");

typedef struct {
    NvU32 vgpuType;                   /* +0x00 */
    NvU32 vmPid;                      /* +0x04 */
    NvU32 gfid;                       /* +0x08 */
    NvU32 pad0c;                      /* +0x0c */
    NvU32 swizzId;                    /* +0x10 */
    NvU32 pad14;                      /* +0x14 */
    NvU32 vmIdType;                   /* +0x18 (1 = UUID) */
    NvU32 pad1c;                      /* +0x1c */
    NvU8  vmId[16];                   /* +0x20 */
    NvU32 hPluginFBAllocationClient;  /* +0x30 */
    NvU8  reserved[0x70U - 0x34U];    /* +0x34..0x6f */
} NVA082_ALLOC_PARAMETERS;

_Static_assert(sizeof(NVA082_ALLOC_PARAMETERS) == 0x70U, "NVA082_ALLOC_PARAMETERS must be 112 bytes");

/* Optional syscall hooks for hermetic unit testing without /dev/nvidia* */
typedef struct {
    int (*open_fn)(const char *path, int flags);
    int (*close_fn)(int fd);
    int (*ioctl_fn)(int fd, unsigned long req, void *arg);
} open_vgpu_rm_sys_ops_t;

void open_vgpu_rm_set_sys_ops(const open_vgpu_rm_sys_ops_t *ops);
void open_vgpu_rm_reset_sys_ops(void);
int  open_vgpu_rm_has_custom_sys_ops(void);

#define GP104_MAX_GUEST_CLIENTS 64U

typedef struct {
    int      in_use;
    uint32_t guest_client;
    uint32_t host_client;
} open_vgpu_guest_client_t;

typedef struct {
    int       attached;
    int       ctl_fd;
    int       gpu_fd;
    int       nva082_allocated;
    uint32_t  nva082_status;

    uint32_t  dev_handle;
    uint32_t  vgpu_type_id;
    uint32_t  gpu_id;
    uint32_t  dev_inst;
    uint32_t  minor;
    uint32_t  pci_vendor;
    uint32_t  pci_device;
    uint32_t  pci_subsys_vendor;
    uint32_t  pci_subsys_device;
    uint32_t  virt_mode;

    uint32_t  h_root;
    uint32_t  h_dev;
    uint32_t  h_subdev;
    uint32_t  h_vgpu_cfg;
    uint32_t  h_host_vgpu;

    uint32_t  p2p_cap;
    uint32_t  encoder_capacity;
    uint64_t  live_fb_length;
    uint64_t  live_bar1_length;
    uint32_t  live_frl_config;
    uint32_t  live_max_res_x;
    uint32_t  live_max_res_y;
    char      live_profile_name[32];

    open_vgpu_guest_client_t guest_clients[GP104_MAX_GUEST_CLIENTS];
} open_vgpu_rm_ctx_t;

int       open_vgpu_rm_ctl_ioctl(int fd, uint32_t cmd, uint32_t size, unsigned long req, void *params);
NV_STATUS open_vgpu_rm_alloc_root(int ctl_fd, uint32_t *out_client);
NV_STATUS open_vgpu_rm_alloc_obj(int ctl_fd, uint32_t h_root, uint32_t h_parent, uint32_t h_new,
                                 uint32_t h_class, void *params, uint32_t params_size);
NV_STATUS open_vgpu_rm_control(int ctl_fd, uint32_t h_client, uint32_t h_object, uint32_t cmd,
                               void *params, uint32_t params_size);
NV_STATUS open_vgpu_rm_free(int ctl_fd, uint32_t h_root, uint32_t h_parent, uint32_t h_old);
NV_STATUS open_vgpu_rm_dup_object(int ctl_fd, uint32_t h_client_dst, uint32_t h_parent,
                                  uint32_t h_obj_dst, uint32_t h_client_src, uint32_t h_obj_src,
                                  uint32_t flags);

NV_STATUS open_vgpu_rm_attach(open_vgpu_rm_ctx_t *rm, uint32_t dev_handle, uint32_t vgpu_type_id,
                              const char *bdf, const uint8_t uuid[16], uint32_t vm_pid);
void      open_vgpu_rm_detach(open_vgpu_rm_ctx_t *rm);

NV_STATUS open_vgpu_rm_lookup_or_create_guest_client(open_vgpu_rm_ctx_t *rm, uint32_t guest_client,
                                                     uint32_t *out_host_client, int *out_created);
int       open_vgpu_rm_lookup_guest_client(const open_vgpu_rm_ctx_t *rm, uint32_t guest_client,
                                           uint32_t *out_host_client);
void      open_vgpu_rm_free_guest_client(open_vgpu_rm_ctx_t *rm, uint32_t guest_client);
void      open_vgpu_rm_free_all_guest_clients(open_vgpu_rm_ctx_t *rm);

#ifdef __cplusplus
}
#endif

#endif /* OPEN_VGPU_RM_H */
