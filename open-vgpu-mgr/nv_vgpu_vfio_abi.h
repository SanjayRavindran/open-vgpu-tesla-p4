#ifndef NV_VGPU_VFIO_ABI_H
#define NV_VGPU_VFIO_ABI_H

#include <stdint.h>
#include <stddef.h>
#include <sys/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Open MIT-compatible ABI definitions from kernel/nvidia-vgpu-vfio/nv-vgpu-ioctl.h
 * and open-gpu-kernel-modules RM ioctl interfaces (535.309.01).
 * ========================================================================= */

typedef uint8_t  NvU8;
typedef uint16_t NvU16;
typedef uint32_t NvU32;
typedef uint64_t NvU64;
typedef uint64_t NvP64;
typedef uint8_t  NvBool;
typedef uint32_t NV_STATUS;

#define NV_OK                        0x00000000U
#define NV_ERR_BUSY_RETRY            0x00000003U
#define NV_ERR_INVALID_ARGUMENT      0x0000001fU
#define NV_ERR_NO_MEMORY             0x0000001aU
#define NV_ERR_INSUFFICIENT_RESOURCES 0x0000005bU
#define NV_ERR_GENERIC               0x0000ffffU
#define NV_ERR_OPERATING_SYSTEM      0x00000059U
#define NV_ERR_NOT_SUPPORTED         0x00000056U
#define NV_ERR_OBJECT_NOT_FOUND      0x00000057U
#define NV_ERR_INVALID_STATE         0x00000028U
#define NV_ERR_TIMEOUT               0x00000065U

#define NV_VGPU_DRIVER_VERSION_STR   "535.309.01"

/* nvidia-vgpu-vfio ioctl envelope */
typedef struct {
    NvP64     pCmdParams;
    NV_STATUS status;
    NvU32     size;
} NV_VGPU_DEV_IOCTL_PARAMS;

_Static_assert(sizeof(NV_VGPU_DEV_IOCTL_PARAMS) == 16, "NV_VGPU_DEV_IOCTL_PARAMS size");

#define NV_VGPU_IOCTL_MAGIC          'q'
#define NV_VGPU_CTL_IOCTL_MAGIC      'r'
#define NV_VGPU_IOCTL(cmd)           _IOWR(NV_VGPU_IOCTL_MAGIC, (cmd), NV_VGPU_DEV_IOCTL_PARAMS)
#define NV_VGPU_CTL_IOCTL(cmd)       _IOWR(NV_VGPU_CTL_IOCTL_MAGIC, (cmd), NV_VGPU_DEV_IOCTL_PARAMS)

/* Spaces and address types */
typedef enum {
    VGPU_EMUL_CONFIG_SPACE = 0,
    VGPU_EMUL_IO_PORT      = 1,
    VGPU_EMUL_MMIO         = 2
} VGPU_EMUL_SPACE_T;

typedef enum {
    GET_IOVA     = 0,
    GET_HOST_PFN = 1
} VGPU_ADDR_TYPE_T;

enum {
    NV_VFIO_DEVICE_STATE_NONE                     = 0,
    NV_VFIO_DEVICE_STATE_RUNNING                  = 1,
    NV_VFIO_DEVICE_STATE_MIGRATION_PRECOPY_ACTIVE = 2,
    NV_VFIO_DEVICE_STATE_MIGRATION_STOPNCOPY_ACTIVE = 3,
    NV_VFIO_DEVICE_STATE_MIGRATION_RESUME         = 4,
};

/* vGPU device events */
#define NV_VFIO_VGPU_EVENT_VM_REG_ACCESS         1
#define NV_VFIO_VGPU_EVENT_VM_SHUTDOWN           2
#define NV_VFIO_VGPU_EVENT_VM_REBOOT             3
#define NV_VFIO_VGPU_EVENT_ATTACH_DEVICE         4
#define NV_VFIO_VGPU_EVENT_CONSOLE_INFO          5
#define NV_VFIO_VGPU_EVENT_MIGRATION_STATE       6
#define NV_VFIO_VGPU_EVENT_READ_DEVICE_BUFFER    7
#define NV_VFIO_VGPU_EVENT_WRITE_DEVICE_BUFFER   8
#define NV_VFIO_VGPU_EVENT_MIGRATION_DATA_SIZE   9
#define NV_VFIO_VGPU_EVENT_BAR_INFO              10
#define NV_VFIO_VGPU_EVENT_MAX_COUNT             11

/* vGPU ctldev events */
#define NV_VFIO_VGPU_CTLDEV_EVENT_CREATE_DEVICE  1
#define NV_VFIO_VGPU_CTLDEV_EVENT_DELETE_DEVICE  2
#define NV_VFIO_VGPU_CTLDEV_EVENT_OPEN_DEVICE    3
#define NV_VFIO_VGPU_CTLDEV_EVENT_GET_INSTANCES  4
#define NV_VFIO_VGPU_CTLDEV_EVENT_MAX_COUNT      5

#define VM_UUID_SIZE                             16
#define VGPU_UUID_SIZE                           16
#define VGPU_CONFIG_PARAMS_MAX_LENGTH            1024

#define NV_VFIO_VGPU_MIGRATION_REGION_SIZE_MAX   (16 * 1024 * 1024)
#define NV_VFIO_VGPU_MIGRATION_REGION_DATA_OFFSET 0x1000ULL
#define NV_VFIO_VGPU_MIGRATION_REGION_SIZE_MMAP  (NV_VFIO_VGPU_MIGRATION_REGION_SIZE_MAX - 0x1000)
#define NV_VFIO_VGPU_CONSOLE_SURFACE_SIZE_MAX    (16 * 1024 * 1024)

#define NV_VGPU_DEVICE_CONSOLE_REGION_OFFSET     0x10000000000ULL
#define NV_VGPU_DEVICE_MIGRATION_REGION_OFFSET   0x20000000000ULL
#define NV_VGPU_DEVICE_STAGING_REGION_OFFSET     0x30000000000ULL

#define NV_MAX_VGPU_TYPE_PER_GPU                 0x40
#define NV_VGPU_STRING_BUFFER_SIZE               32
#define NV_VERSION_STRING_LENGTH                 256
#define NV_VGPU_MAX_BAR_REGION_COUNT             4
#define NV_VGPU_MAX_SPARSE_REGION_COUNT          5
#define NV_MAX_PROBED_GPUS                       32

typedef struct {
    NvU32 vgpu_type_id;
    NvU32 num_heads;
    NvU32 max_instance;
    NvU32 max_resolutionx;
    NvU32 max_resolutiony;
    NvU32 frl_config;
    NvU32 profileSize;
    char  vgpu_name[NV_VGPU_STRING_BUFFER_SIZE];
} type_info_t;

_Static_assert(sizeof(type_info_t) == 60, "type_info_t size");

/* Ctldev ioctl commands */
#define NV_VFIO_VGPU_CTLDEV_SET_TYPES_INFO       NV_VGPU_CTL_IOCTL(0x1)
typedef struct {
    NvU32        gpu_pci_id;
    NvU32        num_vgpu_types;
    NvU32        first_vf_offset;
    NvBool       sriov_enabled;
    type_info_t *type_info;
} NV_VFIO_VGPU_CTLDEV_SET_TYPES_INFO_PARAMS;

_Static_assert(sizeof(NV_VFIO_VGPU_CTLDEV_SET_TYPES_INFO_PARAMS) == 24, "SET_TYPES_INFO size");

#define NV_VFIO_VGPU_CTLDEV_EVENT_CREATE         NV_VGPU_CTL_IOCTL(0x2)
#define NV_VFIO_VGPU_CTLDEV_EVENT_DESTROY        NV_VGPU_CTL_IOCTL(0x3)

typedef struct {
    NvU32 event_type;
} NV_VFIO_VGPU_CTLDEV_EVENT_PARAMS;

#define NV_VFIO_VGPU_CTLDEV_GET_EVENT_DATA       NV_VGPU_CTL_IOCTL(0x4)
typedef struct {
    NvU32 event_type;
    NvU32 gpu_pci_id;
    NvU32 gpu_pci_bdf;
    NvU8  vgpu_name[VM_UUID_SIZE];
    NvU8  vgpu_params[VGPU_CONFIG_PARAMS_MAX_LENGTH];
    NvU32 qemu_pid;
    NvU32 vgpu_id;
    NvU32 vgpu_type_id;
    NvU32 num_vgpu_types;
    NvU32 vgpu_type_ids[NV_MAX_VGPU_TYPE_PER_GPU];
} NV_VFIO_VGPU_CTLDEV_GET_EVENT_DATA_PARAMS;

_Static_assert(sizeof(NV_VFIO_VGPU_CTLDEV_GET_EVENT_DATA_PARAMS) == 0x52c, "GET_EVENT_DATA size");

#define NV_VFIO_VGPU_CTLDEV_SET_EVENT_DATA       NV_VGPU_CTL_IOCTL(0x5)
typedef struct {
    NvU32 event_type;
    NvU32 return_status;
    NvU16 vgpu_id;
    NvU32 num_vgpu_types;
    NvU32 vgpu_type_ids[NV_MAX_VGPU_TYPE_PER_GPU];
    NvU32 available_instances[NV_MAX_VGPU_TYPE_PER_GPU];
} NV_VFIO_VGPU_CTLDEV_SET_EVENT_DATA_PARAMS;

_Static_assert(sizeof(NV_VFIO_VGPU_CTLDEV_SET_EVENT_DATA_PARAMS) == 0x210, "SET_EVENT_DATA size");

#define NV_VFIO_VGPU_CTLDEV_PROBE_GPU            NV_VGPU_CTL_IOCTL(0x6)
typedef struct {
    NvU32  num_pgpus;
    NvBool is_daemon_init;
    NvU32  gpu_id[NV_MAX_PROBED_GPUS];
} NV_VFIO_VGPU_CTLDEV_PROBE_GPU_PARAMS;

_Static_assert(sizeof(NV_VFIO_VGPU_CTLDEV_PROBE_GPU_PARAMS) == 0x88, "PROBE_GPU size");

#define NV_VFIO_VGPU_CTLDEV_UNREGISTER           NV_VGPU_CTL_IOCTL(0x7)
typedef struct {
    NvU32 gpu_id;
} NV_VFIO_VGPU_CTLDEV_UNREGISTER_PARAMS;

#define NV_VFIO_VGPU_CTLDEV_VERSION_CHECK        NV_VGPU_CTL_IOCTL(0x8)
typedef struct {
    char version_string[NV_VERSION_STRING_LENGTH];
} NV_VFIO_VGPU_CTLDEV_VERSION_CHECK_PARAMS;

/* Per-device vgpu ioctl commands */
#define NV_VFIO_VGPU_EVENT_CREATE                NV_VGPU_IOCTL(0x01)
#define NV_VFIO_VGPU_EVENT_DESTROY               NV_VGPU_IOCTL(0x02)
typedef struct {
    NvU32 eventType;
} NV_VFIO_VGPU_EVENT_PARAMS;

#define NV_VFIO_VGPU_PIN_PAGES                   NV_VGPU_IOCTL(0x03)
typedef struct {
    NvP64            pGuestPfnMem __attribute__((aligned(8)));
    NvP64            pHostPfnMem  __attribute__((aligned(8)));
    NvU32            pageCount;
    VGPU_ADDR_TYPE_T addrType;
} NV_VFIO_VGPU_PIN_PAGES_PARAMS;

_Static_assert(sizeof(NV_VFIO_VGPU_PIN_PAGES_PARAMS) == 0x18, "PIN_PAGES size");

#define NV_VFIO_VGPU_UNPIN_PAGES                 NV_VGPU_IOCTL(0x04)
typedef struct {
    NvP64            pGuestPfnMem __attribute__((aligned(8)));
    NvU32            pageCount;
    VGPU_ADDR_TYPE_T addrType;
} NV_VFIO_VGPU_UNPIN_PAGES_PARAMS;

_Static_assert(sizeof(NV_VFIO_VGPU_UNPIN_PAGES_PARAMS) == 0x10, "UNPIN_PAGES size");

#define NV_VFIO_VGPU_REG_ACCESS_GET_DATA         NV_VGPU_IOCTL(0x05)
typedef struct {
    NvU64             offset;
    NvU32             width;
    VGPU_EMUL_SPACE_T emulSpace;
    NvU8              data[4];
    NvBool            isWrite;
} NV_VFIO_VGPU_REG_ACCESS_GET_DATA_PARAMS;

_Static_assert(sizeof(NV_VFIO_VGPU_REG_ACCESS_GET_DATA_PARAMS) == 0x18, "REG_ACCESS_GET_DATA size");

#define NV_VFIO_VGPU_REG_ACCESS_SET_DATA         NV_VGPU_IOCTL(0x06)
typedef struct {
    NvU8      data[4];
    NV_STATUS returnStatus;
} NV_VFIO_VGPU_REG_ACCESS_SET_DATA_PARAMS;

_Static_assert(sizeof(NV_VFIO_VGPU_REG_ACCESS_SET_DATA_PARAMS) == 0x8, "REG_ACCESS_SET_DATA size");

#define NV_VFIO_VGPU_ADD_MMIO_MAPPING            NV_VGPU_IOCTL(0x07)
#define NV_VFIO_VGPU_REMOVE_MMIO_MAPPING         NV_VGPU_IOCTL(0x08)
typedef struct {
    NvU64 virtualMMIOStart  __attribute__((aligned(8)));
    NvU64 physicalMMIOStart __attribute__((aligned(8)));
    NvU64 mmioSize          __attribute__((aligned(8)));
} NV_VFIO_VGPU_MMIO_MAPPING_PARAMS;

_Static_assert(sizeof(NV_VFIO_VGPU_MMIO_MAPPING_PARAMS) == 0x18, "MMIO_MAPPING size");

#define NV_VFIO_VGPU_INJECT_INTERRUPT            NV_VGPU_IOCTL(0x09)
#define NV_VFIO_VGPU_NOTIFY_POWER_OP             NV_VGPU_IOCTL(0x0B)
typedef struct {
    NV_STATUS returnStatus;
} NV_VFIO_VGPU_NOTIFY_POWER_OP_PARAMS;

#define NV_VFIO_VGPU_GET_ATTACH_DEVICE_DATA      NV_VGPU_IOCTL(0x0C)
typedef struct {
    NvU8  mdevUuid[VM_UUID_SIZE];
    NvU8  configParams[VGPU_CONFIG_PARAMS_MAX_LENGTH];
    NvU32 qemuPid;
    NvU16 vgpuId;
    NvU32 gpuPciId;
    NvU32 gpuPciBdf;
} NV_VFIO_VGPU_GET_ATTACH_DEVICE_DATA_PARAMS;

_Static_assert(sizeof(NV_VFIO_VGPU_GET_ATTACH_DEVICE_DATA_PARAMS) == 0x420, "GET_ATTACH_DEVICE_DATA size");

#define NV_VFIO_VGPU_ATTACH_DEVICE_ACK           NV_VGPU_IOCTL(0x0D)
typedef struct {
    NvU32     instanceId;
    NV_STATUS returnStatus;
} NV_VFIO_VGPU_ATTACH_DEVICE_ACK_PARAMS;

#define NV_VFIO_VGPU_ALLOC_CONSOLE_BUFFER        NV_VGPU_IOCTL(0x0E)
#define NV_VFIO_VGPU_FREE_CONSOLE_BUFFER         NV_VGPU_IOCTL(0x0F)
typedef struct {
    NvU64 size;
} NV_VFIO_VGPU_CONSOLE_BUFFER_PARAMS;

#define NV_VFIO_VGPU_SET_CONSOLE_SURFACE_PROP    NV_VGPU_IOCTL(0x10)
typedef struct {
    NvU32 width;
    NvU32 height;
    NvU32 depth;
    NvU32 size;
    NvU32 lineSize;
} NV_VFIO_VGPU_CONSOLE_SURFACE_PARAMS;

_Static_assert(sizeof(NV_VFIO_VGPU_CONSOLE_SURFACE_PARAMS) == 0x14, "CONSOLE_SURFACE size");

#define NV_VFIO_VGPU_GET_MIGRATION_STATE         NV_VGPU_IOCTL(0x11)
typedef struct {
    NvU32 deviceState;
} NV_VFIO_VGPU_MIGRATION_STATE_PARAMS;

#define NV_VFIO_VGPU_NOTIFY_MIGRATION_STATE_STATUS NV_VGPU_IOCTL(0x12)
typedef struct {
    NV_STATUS returnStatus;
} NV_VFIO_VGPU_NOTIFY_MIGRATION_STATE_STATUS_PARAMS;

#define NV_VFIO_VGPU_GET_MIGRATION_BUFFER_SIZE   NV_VGPU_IOCTL(0x13)
typedef struct {
    NvU64 bufferSize;
} NV_VFIO_VGPU_GET_MIGRATION_BUFFER_SIZE_PARAMS;

#define NV_VFIO_VGPU_SET_MIGRATION_BUFFER_SIZE   NV_VGPU_IOCTL(0x14)
typedef struct {
    NvU64     written;
    NvU64     pending;
    NV_STATUS returnStatus;
} NV_VFIO_VGPU_SET_MIGRATION_BUFFER_SIZE_PARAMS;

_Static_assert(sizeof(NV_VFIO_VGPU_SET_MIGRATION_BUFFER_SIZE_PARAMS) == 0x18, "SET_MIGRATION_BUFFER_SIZE size");

#define NV_VFIO_VGPU_VF_REG_ACCESS_HW            NV_VGPU_IOCTL(0x15)
typedef NV_VFIO_VGPU_REG_ACCESS_GET_DATA_PARAMS NV_VFIO_VGPU_VF_REG_ACCESS_HW_PARAMS;

#define NV_VFIO_VGPU_MIGRATION_DATA_SIZE         NV_VGPU_IOCTL(0x16)
typedef struct {
    NvU64     size;
    NV_STATUS returnStatus;
} NV_VFIO_VGPU_MIGRATION_DATA_SIZE_PARAMS;

#define NV_VFIO_VGPU_BAR_INFO_SET_DATA           NV_VGPU_IOCTL(0x17)
typedef struct {
    NvU64     barSizes[NV_VGPU_MAX_BAR_REGION_COUNT];
    NvU64     sparseOffsets[NV_VGPU_MAX_SPARSE_REGION_COUNT];
    NvU64     sparseSizes[NV_VGPU_MAX_SPARSE_REGION_COUNT];
    NvU32     sparseCount;
    NV_STATUS returnStatus;
} NV_VFIO_VGPU_BAR_INFO_SET_DATA_PARAMS;

_Static_assert(sizeof(NV_VFIO_VGPU_BAR_INFO_SET_DATA_PARAMS) == 0x78, "BAR_INFO_SET_DATA size");

/* =========================================================================
 * Open NVIDIA RM /dev/nvidiactl & /dev/nvidiaN Escape Ioctl Definitions
 * ========================================================================= */
#define NV_IOCTL_MAGIC 'F'
#define NV_IOCTL(nr, sz) _IOC(_IOC_READ | _IOC_WRITE, NV_IOCTL_MAGIC, (nr), (sz))

#define NV_ESC_RM_ALLOC_MEMORY   0x27
#define NV_ESC_RM_FREE           0x29
#define NV_ESC_RM_CONTROL        0x2a
#define NV_ESC_RM_ALLOC          0x2b
#define NV_ESC_RM_MAP_MEMORY     0x4e
#define NV_ESC_RM_UNMAP_MEMORY   0x4f
#define NV_ESC_RM_UPDATE_DEVICE_MAPPING_INFO 0x5e
#define NV_ESC_CARD_INFO         0xc8
#define NV_ESC_REGISTER_FD       0xc9
#define NV_ESC_ALLOC_OS_EVENT    0xce
#define NV_ESC_FREE_OS_EVENT     0xcf
#define NV_ESC_STATUS_CODE       0xd1
#define NV_ESC_CHECK_VERSION_STR 0xd2
#define NV_ESC_IOCTL_XFER_CMD    0xd3
#define NV_ESC_ATTACH_GPUS_TO_FD 0xd4
#define NV_ESC_QUERY_DEVICE_INTR 0xd5
#define NV_ESC_SYS_PARAMS        0xd6
#define NV_ESC_NUMA_INFO         0xd7

typedef struct {
    NvU32 cmd;
    NvU32 size;
    NvP64 ptr;
} nv_ioctl_xfer_t;

_Static_assert(sizeof(nv_ioctl_xfer_t) == 16, "nv_ioctl_xfer_t size");

typedef struct {
    NvU32 cmd;
    NvU32 reply;
    char  versionString[64];
} nv_ioctl_rm_api_version_t;

_Static_assert(sizeof(nv_ioctl_rm_api_version_t) == 0x48, "rm_api_version size");

typedef struct {
    NvBool valid;
    NvU8   pad0[3];
    NvU32  domain;
    NvU8   bus;
    NvU8   slot;
    NvU8   function;
    NvU8   pad1;
    NvU16  vendor_id;
    NvU16  device_id;
    NvU32  gpu_id;
    NvU32  interrupt_line;
    NvU64  reg_address;
    NvU64  reg_size;
    NvU64  fb_address;
    NvU64  fb_size;
    NvU32  minor_number;
    NvU8   dev_name[12];
} nv_ioctl_card_info_t;

_Static_assert(sizeof(nv_ioctl_card_info_t) == 0x48, "nv_ioctl_card_info_t size");

typedef struct {
    NvU32 hRoot;
    NvU32 hObjectParent;
    NvU32 hObjectNew;
    NvU32 hClass;
    NvP64 pAllocParms;
    NvU32 paramsSize;
    NvU32 status;
} NVOS21_ROOT_PARAMETERS;

_Static_assert(sizeof(NVOS21_ROOT_PARAMETERS) == 0x20, "NVOS21_ROOT size");

typedef struct {
    NvU32 hRoot;
    NvU32 hObjectParent;
    NvU32 hObjectNew;
    NvU32 hClass;
    NvP64 pAllocParms;
    NvP64 pRightsRequested;
    NvU32 paramsSize;
    NvU32 flags;
    NvU32 status;
    NvU32 pad;
} NVOS64_PARAMETERS;

_Static_assert(sizeof(NVOS64_PARAMETERS) == 0x30, "NVOS64 size");

typedef struct {
    NvU32 hRoot;
    NvU32 hObjectParent;
    NvU32 hObjectNew;
    NvU32 hClass;
    NvU32 flags;
    NvU32 pad0;
    NvP64 pMemory;
    NvU64 limit;
    NvU32 status;
    NvU32 pad1;
    int   fd;
    NvU32 pad2;
} NVOS02_PARAMETERS;

_Static_assert(sizeof(NVOS02_PARAMETERS) == 0x38, "NVOS02 size");

typedef struct {
    NvU32 hRoot;
    NvU32 hObjectParent;
    NvU32 hObjectOld;
    NvU32 status;
} NVOS00_PARAMETERS;

_Static_assert(sizeof(NVOS00_PARAMETERS) == 0x10, "NVOS00 size");

typedef struct {
    NvU32 hClient;
    NvU32 hObject;
    NvU32 cmd;
    NvU32 flags;
    NvP64 params;
    NvU32 paramsSize;
    NvU32 status;
} NVOS54_PARAMETERS;

_Static_assert(sizeof(NVOS54_PARAMETERS) == 0x20, "NVOS54 size");

typedef struct {
    NvU32 hClient;
    NvU32 hDevice;
    NvU32 hMemory;
    NvU32 pad0;
    NvU64 offset;
    NvU64 length;
    NvP64 pLinearAddress;
    NvU32 status;
    NvU32 flags;
    int   fd;
    NvU32 pad1;
} NVOS33_PARAMETERS;

_Static_assert(sizeof(NVOS33_PARAMETERS) == 0x38, "NVOS33 size");

typedef struct {
    NvU32 hClient;
    NvU32 hDevice;
    NvU32 hMemory;
    NvU32 pad0;
    NvP64 pLinearAddress;
    NvU32 status;
    NvU32 flags;
} NVOS34_PARAMETERS;

_Static_assert(sizeof(NVOS34_PARAMETERS) == 0x20, "NVOS34 size");

typedef struct {
    NvU32 hClient;
    NvU32 hDevice;
    NvU32 hMemory;
    NvU32 pad0;
    NvP64 pNewCpuAddress;
    NvP64 pOldCpuAddress;
    NvU32 status;
    NvU32 pad1;
} NVOS56_PARAMETERS;

_Static_assert(sizeof(NVOS56_PARAMETERS) == 0x28, "NVOS56 size");

typedef struct {
    NvU32 hClient;
    NvU32 hDevice;
    NvU32 fd;
    NvU32 status;
} nv_ioctl_alloc_os_event_t;

_Static_assert(sizeof(nv_ioctl_alloc_os_event_t) == 0x10, "alloc_os_event size");

/* RM Classes */
#define NV01_EVENT_OS_EVENT         0x00000079U
#define NV01_EVENT                  0x00000005U
#define NV01_DEVICE_0               0x00000080U
#define NV01_MEMORY_LIST_SYSTEM     0x00000081U
#define NV20_SUBDEVICE_0            0x00002080U
#define NVA081_VGPU_CONFIG          0x0000a081U
#define AMPERE_SMC_PARTITION_REF    0x0000c637U

#define NV01_EVENT_WITHOUT_EVENT_DATA 0x10000000U

/* RM Control Commands */
#define NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION_V2 0x0000013eU
#define NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS        0x00000201U
#define NV0000_CTRL_CMD_GPU_GET_ID_INFO             0x00000202U
#define NV0000_CTRL_CMD_GPU_GET_PROBED_IDS          0x00000214U
#define NV0000_CTRL_CMD_GPU_ATTACH_IDS              0x00000215U
#define NV0000_CTRL_CMD_GPU_DETACH_IDS              0x00000216U
#define NV0000_CTRL_CMD_GPU_GET_PCI_INFO            0x0000021bU

#define NV0000_CTRL_CMD_EVENT_SET_NOTIFICATION      0x00000501U
#define NV0000_CTRL_CMD_EVENT_GET_EVENT_DATA        0x00000502U

#define NV0000_CTRL_CMD_VGPU_CREATE_DEVICE          0x00000c02U
#define NV0000_CTRL_CMD_VGPU_GET_INSTANCES          0x00000c03U
#define NV0000_CTRL_CMD_VGPU_DELETE_DEVICE          0x00000c04U
#define NV0000_CTRL_CMD_VGPU_NOTIFY_EVENT           0x00000c05U

#define NV0000_CTRL_CMD_OS_UNIX_EXPORT_OBJECT_TO_FD 0x00000d01U
#define NV0000_CTRL_CMD_OS_UNIX_IMPORT_OBJECT       0x00000d02U

#define NV2080_CTRL_CMD_GPU_SET_PERSISTENCE_STATE   0x00800287U
#define NV2080_CTRL_CMD_GPU_GET_PERSISTENCE_STATE   0x00800288U
#define NV2080_CTRL_CMD_GPU_GET_SRIOV_CAPS          0x00800291U

#define NVA081_CTRL_CMD_VGPU_CONFIG_GET_VGPU_TYPE_INFO        0xa0810103U
#define NVA081_CTRL_CMD_VGPU_CONFIG_GET_SUPPORTED_VGPU_TYPES  0xa0810104U
#define NVA081_CTRL_CMD_VGPU_CONFIG_SET_VM_NAME               0xa0810120U
#define NVA081_CTRL_CMD_VGPU_CONFIG_GET_BAR_INFO              0xa0810121U

/* RM parameter structures verified against vGPU 535.309.01 */
typedef struct {
    NvU32 deviceId;
    NvU32 hClientShare;
    NvU32 hTargetClient;
    NvU32 hTargetDevice;
    NvU32 flags;
    NvU64 vaSpaceSize;
    NvU64 vaStartInternal;
    NvU64 vaLimitInternal;
    NvU32 vaMode;
} NV0080_ALLOC_PARAMETERS;

typedef struct {
    NvU32 subDeviceId;
} NV2080_ALLOC_PARAMETERS;

typedef struct {
    NvU32 hParentClient;
    NvU32 hSrcResource;
    NvU32 hClass;
    NvU32 notifyIndex;
    NvP64 data;
} NV0005_ALLOC_PARAMETERS;

_Static_assert(sizeof(NV0005_ALLOC_PARAMETERS) == 0x18, "NV0005_ALLOC size");

typedef struct {
    char  driverVersionBuffer[256];
    char  versionBuffer[256];
    char  titleBuffer[256];
    NvU32 changelistNumber;
    NvU32 officialChangelistNumber;
} NV0000_CTRL_SYSTEM_GET_BUILD_VERSION_V2_PARAMS;

_Static_assert(sizeof(NV0000_CTRL_SYSTEM_GET_BUILD_VERSION_V2_PARAMS) == 0x308, "BUILD_VERSION_V2 size");

typedef struct {
    NvU32 gpuId;
    NvU32 gpuFlags;
    NvU32 deviceInstance;
    NvU32 subDeviceInstance;
    NvU8  pad[24];
} NV0000_CTRL_GPU_GET_ID_INFO_PARAMS;

_Static_assert(sizeof(NV0000_CTRL_GPU_GET_ID_INFO_PARAMS) == 0x28, "GPU_GET_ID_INFO size");

typedef struct {
    NvU32 gpuId;
    NvU32 domain;
    NvU16 bus;
    NvU16 slot;
} NV0000_CTRL_GPU_GET_PCI_INFO_PARAMS;

_Static_assert(sizeof(NV0000_CTRL_GPU_GET_PCI_INFO_PARAMS) == 0x0c, "GPU_GET_PCI_INFO size");

typedef struct {
    NvU32 totalVgpuCount;
    NvU32 firstVfOffset;
    NvU8  raw[0x43 - 8];
    NvU8  sriovEnabled;
    NvU8  tail[0x48 - 0x44];
} NV2080_CTRL_GPU_GET_SRIOV_CAPS_PARAMS;

_Static_assert(sizeof(NV2080_CTRL_GPU_GET_SRIOV_CAPS_PARAMS) == 0x48, "GET_SRIOV_CAPS size");

typedef struct {
    NvU32 vgpuType;
    NvU32 numVgpuTypes;
    NvU32 vgpuTypes[NV_MAX_VGPU_TYPE_PER_GPU];
} NVA081_CTRL_VGPU_CONFIG_GET_SUPPORTED_VGPU_TYPES_PARAMS;

_Static_assert(sizeof(NVA081_CTRL_VGPU_CONFIG_GET_SUPPORTED_VGPU_TYPES_PARAMS) == 0x108, "GET_SUPPORTED_VGPU_TYPES size");

typedef struct {
    NvU32 vgpuType;             /* +0x000 */
    NvU8  pad0[8];              /* +0x004 */
    char  vgpuName[32];         /* +0x00c */
    NvU8  raw_mid[0x14c - 0x2c];/* +0x02c */
    NvU32 maxInstance;          /* +0x14c */
    NvU32 numHeads;             /* +0x150 */
    NvU32 maxResolutionX;       /* +0x154 */
    NvU32 maxResolutionY;       /* +0x158 */
    NvU32 pad1;                 /* +0x15c */
    NvU32 frlConfig;            /* +0x160 */
    NvU8  raw_mid2[0x188 - 0x164]; /* +0x164 */
    NvU64 fbLength;             /* +0x188 */
    NvU8  tail[0x1360 - 0x190]; /* +0x190 */
} NVA081_CTRL_VGPU_CONFIG_GET_VGPU_TYPE_INFO_PARAMS;

_Static_assert(sizeof(NVA081_CTRL_VGPU_CONFIG_GET_VGPU_TYPE_INFO_PARAMS) == 0x1360, "GET_VGPU_TYPE_INFO size");

typedef struct {
    NvU8 mdevUuid[VM_UUID_SIZE];
    char vmName[128];
} NVA081_CTRL_VGPU_CONFIG_SET_VM_NAME_PARAMS;

_Static_assert(sizeof(NVA081_CTRL_VGPU_CONFIG_SET_VM_NAME_PARAMS) == 0x90, "SET_VM_NAME size");

typedef struct {
    NvU32 gpuPciId;
    NvU8  mdevUuid[VM_UUID_SIZE];
    NvU8  configParams[VGPU_CONFIG_PARAMS_MAX_LENGTH];
    NvU32 pad0;
    NvU64 barSizes[NV_VGPU_MAX_BAR_REGION_COUNT];
    NvU64 sparseOffsets[NV_VGPU_MAX_SPARSE_REGION_COUNT];
    NvU64 sparseSizes[NV_VGPU_MAX_SPARSE_REGION_COUNT];
    NvU32 sparseCount;
    NvU32 pad1;
} NVA081_CTRL_VGPU_CONFIG_GET_BAR_INFO_PARAMS;

_Static_assert(sizeof(NVA081_CTRL_VGPU_CONFIG_GET_BAR_INFO_PARAMS) == 0x490, "GET_BAR_INFO size");

typedef struct {
    NvU8  mdevUuid[VM_UUID_SIZE];
    NvU32 gpuPciId;
    NvU32 gpuPciBdf;
    NvU32 vgpuTypeId;
    NvU16 vgpuId;
    NvU16 pad;
} NV0000_CTRL_VGPU_CREATE_DEVICE_PARAMS;

_Static_assert(sizeof(NV0000_CTRL_VGPU_CREATE_DEVICE_PARAMS) == 0x20, "VGPU_CREATE_DEVICE size");

typedef struct __attribute__((packed)) {
    NvU8  mdevUuid[VM_UUID_SIZE];
    NvU16 vgpuId;
} NV0000_CTRL_VGPU_DELETE_DEVICE_PARAMS;

_Static_assert(sizeof(NV0000_CTRL_VGPU_DELETE_DEVICE_PARAMS) == 0x12, "VGPU_DELETE_DEVICE size");

typedef struct {
    NvU32 gpuPciId;
    NvU32 gpuPciBdf;
    NvU32 numVgpuTypes;
    NvU32 vgpuTypeIds[NV_MAX_VGPU_TYPE_PER_GPU];
    NvU32 availableInstances[NV_MAX_VGPU_TYPE_PER_GPU];
} NV0000_CTRL_VGPU_GET_INSTANCES_PARAMS;

_Static_assert(sizeof(NV0000_CTRL_VGPU_GET_INSTANCES_PARAMS) == 0x20c, "VGPU_GET_INSTANCES size");

typedef struct {
    NvU32 status;
    NvU32 gpuPciId;
} NV0000_CTRL_VGPU_NOTIFY_EVENT_PARAMS;

_Static_assert(sizeof(NV0000_CTRL_VGPU_NOTIFY_EVENT_PARAMS) == 0x08, "VGPU_NOTIFY_EVENT size");

typedef struct {
    NvU32 notifyIndex;
    NvU32 action;
} NV0000_CTRL_EVENT_SET_NOTIFICATION_PARAMS;

typedef struct {
    NvU32 event;
    NvU32 gpuId;
} NV0000_CTRL_EVENT_GET_EVENT_DATA_PARAMS;

typedef struct {
    NvU32 memoryHandle;
    NvU32 flags;
    NvU32 fdType;
} NV0000_CTRL_OS_UNIX_EXPORT_OBJECT_TO_FD_PARAMS;

_Static_assert(sizeof(NV0000_CTRL_OS_UNIX_EXPORT_OBJECT_TO_FD_PARAMS) == 0x0c, "EXPORT_OBJECT_TO_FD size");

/* vmiop error codes and RM status translation (0x207a0 table) */
typedef enum {
    VMIOP_SUCCESS          = 0,
    VMIOP_ERROR_INVAL      = 1,
    VMIOP_ERROR_NO_MEMORY  = 2,
    VMIOP_ERROR_RESOURCE   = 3,
    VMIOP_ERROR_NOT_ALLOWED = 4,
    VMIOP_ERROR_NOT_FOUND  = 5,
    VMIOP_ERROR_EXISTS     = 6,
    VMIOP_ERROR_TIMEOUT    = 7,
} vmiop_error_t;

static inline NV_STATUS vmiop_err_to_rm_status(uint32_t err)
{
    static const NV_STATUS map[8] = {
        0x00U, 0x1fU, 0x1aU, 0x5bU, 0x1eU, 0x57U, 0x1eU, 0x65U
    };
    return (err < 8U) ? map[err] : NV_ERR_GENERIC;
}

#ifdef __cplusplus
}
#endif

#endif /* NV_VGPU_VFIO_ABI_H */
