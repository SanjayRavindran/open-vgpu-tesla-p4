//! Exact `#[repr(C)]` Linux ABI types and `_IOWR` ioctl constants matching
//! `nvidia-vgpu-vfio` (`nv-vgpu-ioctl.h`) and observed in `nvidia-vgpu-mgr` / `nvidia-vgpud`.

use crate::config::VGPU_STRING_BUFFER_SIZE;

/// IOCTL magic for per-vGPU device fd (`/dev/nvidia-vgpu%d`).
pub const NV_VGPU_IOCTL_MAGIC: u8 = b'q'; // 0x71
/// IOCTL magic for vGPU control fd (`/dev/nvidia-vgpu-ctl`).
pub const NV_VGPU_CTL_IOCTL_MAGIC: u8 = b'r'; // 0x72
/// IOCTL magic for NVIDIA RM control fd (`/dev/nvidiactl`).
pub const NV_RM_IOCTL_MAGIC: u8 = b'F'; // 0x46

/// Construct Linux `_IOWR(type, nr, size)` (`dir = 3 = READ|WRITE`).
pub const fn linux_iowr(magic: u8, nr: u8, size: usize) -> u32 {
    ((3u32) << 30) | (((size as u32) & 0x3fff) << 16) | ((magic as u32) << 8) | (nr as u32)
}

/// Top-level ioctl envelope passed to `/dev/nvidia-vgpu-ctl` and `/dev/nvidia-vgpu%d`.
#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct NvVgpuDevIoctlParams {
    pub p_cmd_params: u64,
    pub status: u32,
    pub size: u32,
}

const ENVELOPE_SIZE: usize = core::mem::size_of::<NvVgpuDevIoctlParams>(); // 16 bytes

/// Per-vGPU device ioctl numbers (`_IOWR('q', cmd, 16)` -> `0xc01071xx`).
pub mod dev_ioctl {
    use super::{linux_iowr, ENVELOPE_SIZE, NV_VGPU_IOCTL_MAGIC};

    pub const VERSION_CHECK: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x01, ENVELOPE_SIZE);
    pub const EVENT_CREATE: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x02, ENVELOPE_SIZE);
    pub const EVENT_DESTROY: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x03, ENVELOPE_SIZE);
    pub const GET_EVENT_DATA: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x04, ENVELOPE_SIZE);
    pub const SET_EVENT_DATA: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x05, ENVELOPE_SIZE);
    pub const GET_VF_PCI_CFG: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x06, ENVELOPE_SIZE);
    pub const TRANSLATE_GFN_TO_PFN: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x07, ENVELOPE_SIZE);
    pub const UNPIN_PAGES: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x08, ENVELOPE_SIZE);
    pub const INJECT_INTERRUPT: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x09, ENVELOPE_SIZE);
    pub const UPDATE_MAPPING: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x0a, ENVELOPE_SIZE);
    pub const SET_VM_MIGRATION_CAP: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x0c, ENVELOPE_SIZE);
    pub const GET_GUEST_ID: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x0d, ENVELOPE_SIZE);
    pub const SET_DIRTY_PAGES: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x0f, ENVELOPE_SIZE);
    pub const SET_CONSOLE_SURFACE_PROP: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x10, ENVELOPE_SIZE);
    pub const GET_MIGRATION_STATE: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x11, ENVELOPE_SIZE);
    pub const NOTIFY_MIGRATION_STATE_STATUS: u32 =
        linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x12, ENVELOPE_SIZE);
    pub const GET_MIGRATION_BUFFER_SIZE: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x13, ENVELOPE_SIZE);
    pub const SET_MIGRATION_BUFFER_SIZE: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x14, ENVELOPE_SIZE);
    pub const VF_REG_ACCESS_HW: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x15, ENVELOPE_SIZE);
    pub const MIGRATION_DATA_SIZE: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x16, ENVELOPE_SIZE);
    pub const BAR_INFO_SET_DATA: u32 = linux_iowr(NV_VGPU_IOCTL_MAGIC, 0x17, ENVELOPE_SIZE);
}

/// Control device ioctl numbers (`_IOWR('r', cmd, 16)` -> `0xc01072xx`).
pub mod ctl_ioctl {
    use super::{linux_iowr, ENVELOPE_SIZE, NV_VGPU_CTL_IOCTL_MAGIC};

    pub const SET_TYPES_INFO: u32 = linux_iowr(NV_VGPU_CTL_IOCTL_MAGIC, 0x01, ENVELOPE_SIZE);
    pub const EVENT_CREATE: u32 = linux_iowr(NV_VGPU_CTL_IOCTL_MAGIC, 0x02, ENVELOPE_SIZE);
    pub const EVENT_DESTROY: u32 = linux_iowr(NV_VGPU_CTL_IOCTL_MAGIC, 0x03, ENVELOPE_SIZE);
    pub const GET_EVENT_DATA: u32 = linux_iowr(NV_VGPU_CTL_IOCTL_MAGIC, 0x04, ENVELOPE_SIZE);
    pub const SET_EVENT_DATA: u32 = linux_iowr(NV_VGPU_CTL_IOCTL_MAGIC, 0x05, ENVELOPE_SIZE);
    pub const PROBE_GPU: u32 = linux_iowr(NV_VGPU_CTL_IOCTL_MAGIC, 0x06, ENVELOPE_SIZE);
    pub const UNREGISTER: u32 = linux_iowr(NV_VGPU_CTL_IOCTL_MAGIC, 0x07, ENVELOPE_SIZE);
}

/// `VGPU_EMUL_SPACE_T` from `nv-vgpu-ioctl.h`.
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum VgpuEmulSpace {
    ConfigSpace = 0,
    IoPort = 1,
    Mmio = 2,
}

/// `VGPU_ADDR_TYPE_T` from `nv-vgpu-ioctl.h`.
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum VgpuAddrType {
    GetIova = 0,
    GetHostPfn = 1,
}

/// Control device event IDs (`NV_VFIO_VGPU_CTLDEV_EVENT_*`).
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum CtldevEventType {
    CreateDevice = 1,
    DeleteDevice = 2,
    OpenDevice = 3,
    GetInstances = 4,
}

/// Per-vGPU instance event IDs (`NV_VFIO_VGPU_EVENT_*`).
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum VgpuEventType {
    VmRegAccess = 1,
    VmShutdown = 2,
    VmReboot = 3,
    AttachDevice = 4,
    ConsoleInfo = 5,
    MigrationState = 6,
    ReadDeviceBuffer = 7,
    WriteDeviceBuffer = 8,
    MigrationDataSize = 9,
    BarInfo = 10,
}

/// `type_info_t` passed to `nvidia-vgpu-vfio` to populate sysfs `mdev_supported_types`.
#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct VgpuTypeInfo {
    pub vgpu_type_id: u32,
    pub num_heads: u32,
    pub max_instance: u32,
    pub max_resolution_x: u32,
    pub max_resolution_y: u32,
    pub frl_config: u32,
    pub profile_size_mb: u32,
    pub vgpu_name: [u8; VGPU_STRING_BUFFER_SIZE],
}

pub const NV_VGPU_MAX_BAR_REGION_COUNT: usize = 6;
pub const NV_VGPU_MAX_SPARSE_REGION_COUNT: usize = 8;

/// `NV_VFIO_VGPU_BAR_INFO_SET_DATA_PARAMS` (`NV_VGPU_IOCTL(0x17)`).
#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct BarInfoSetDataParams {
    pub bar_sizes: [u64; NV_VGPU_MAX_BAR_REGION_COUNT],
    pub sparse_offsets: [u64; NV_VGPU_MAX_SPARSE_REGION_COUNT],
    pub sparse_sizes: [u64; NV_VGPU_MAX_SPARSE_REGION_COUNT],
    pub sparse_count: u32,
    pub return_status: u32,
}

/// `NV_VFIO_VGPU_VF_REG_ACCESS_HW_PARAMS` (`NV_VGPU_IOCTL(0x15)`).
#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct VfRegAccessHwParams {
    pub offset: u64,
    pub width: u32,
    pub emul_space: VgpuEmulSpace,
    pub data: [u8; 4],
    pub is_write: u8,
    pub _pad: [u8; 3],
}

/// Region indices encoded into `vma->vm_pgoff` bits `[43:40]` by `nvidia-vgpu-vfio`.
#[repr(u64)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum VgpuDeviceMmapRegion {
    Console = 1,
    Migration = 2,
    Staging = 3,
}

impl VgpuDeviceMmapRegion {
    /// Compute the `pgoff` base for `mmap()` matching `NV_VGPU_DEVICE_*_REGION_BASE`.
    pub const fn pgoff_base(self) -> u64 {
        (self as u64) << 40
    }
}
