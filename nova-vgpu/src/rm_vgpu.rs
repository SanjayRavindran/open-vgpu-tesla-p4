//! Clean-room RM vGPU control abstractions (`0x800289`, `NVA081` `0xa081`,
//! `NVA082` `0xa082`) and Tesla P4 (`GP104GL`, 8 GiB VRAM) framebuffer partitioning.

use crate::config::{SupportedVgpuEntry, VgpuProfile};
use crate::{VgpuError, VgpuResult};

/// RM class IDs observed in `nvidia-vgpud` and `libnvidia-vgpu.so.535.309.01`.
pub const NV_RM_CLASS_SUBDEVICE: u32 = 0x2080;
pub const NV_RM_CLASS_VGPU_CONFIG: u32 = 0xa081;
pub const NV_RM_CLASS_HOST_VGPU_DEVICE: u32 = 0xa082;
pub const NV_RM_CLASS_PHYS_MEM_SUBALLOCATOR: u32 = 0x90e8;

/// RM control command IDs observed in `nvidia-vgpud` (`0x402ca0.c`) and `libnvidia-vgpu.so`.
pub mod rm_ctrl {
    pub const SUBDEVICE_GET_VIRTUALIZATION_MODE: u32 = 0x0080_0289;
    pub const SUBDEVICE_EVENT_NOTIFICATION: u32 = 0x2080_0303;
    pub const SUBDEVICE_ECC_QUERY: u32 = 0x2080_0304;
    pub const VGPU_CONFIG_SET_INFO: u32 = 0xa081_0101;
    pub const VGPU_CONFIG_GET_INFO: u32 = 0xa081_0103;
    pub const VGPU_CONFIG_NOTIFY_START: u32 = 0xa081_0109;
    pub const VGPU_CONFIG_GET_VGPU_TYPE_INFO: u32 = 0xa081_0112;
    pub const VGPU_CONFIG_SET_DOORBELL_EMUL: u32 = 0xa081_0115;
    pub const VGPU_CONFIG_GET_DOORBELL_CAPS: u32 = 0xa081_0116;
    pub const VGPU_CONFIG_SET_FRACTIONAL_MULTI_VGPU: u32 = 0xa081_0119;
    pub const VGPU_CONFIG_GET_MIGRATION_CAPS: u32 = 0xa081_011a;

    pub const HOST_VGPU_DEVICE_SET_GUEST_INFO: u32 = 0xa082_0103;
    pub const HOST_VGPU_DEVICE_SET_FB_USAGE: u32 = 0xa082_0110;
    pub const HOST_VGPU_DEVICE_GET_CHID_INFO: u32 = 0xa082_011a;
    pub const HOST_VGPU_DEVICE_SET_VGPU_DEVICE_INFO: u32 = 0xa082_011f;
    pub const HOST_VGPU_DEVICE_GET_ECC_STATUS: u32 = 0xa082_0123;
    pub const HOST_VGPU_DEVICE_GET_VMMU_SEGMENT_INFO: u32 = 0xa082_0126;
    pub const HOST_VGPU_DEVICE_GET_EXEC_PARTITION: u32 = 0xa082_0129;
    pub const HOST_VGPU_DEVICE_GET_CAPS: u32 = 0xa082_012c;
}

/// Physical Tesla P4 (`GP104GL`) total GDDR5 capacity (8 GiB = `0x2_0000_0000`).
pub const TESLA_P4_TOTAL_VRAM_BYTES: u64 = 8 * 1024 * 1024 * 1024;
/// Host RM overhead is already accounted for inside each profile's `fbReservation`
/// (`8 * 1024 MiB = 8192 MiB`, `4 * 2048 MiB = 8192 MiB`, `1 * 8192 MiB = 8192 MiB`).
pub const TESLA_P4_HOST_RM_RESERVED_BYTES: u64 = 0;
/// ECC checkbit overhead factor (~6.25%, i.e., 1/16th of VRAM when ECC is fused/enabled).
pub const ECC_OVERHEAD_NUMERATOR: u64 = 1;
pub const ECC_OVERHEAD_DENOMINATOR: u64 = 16;

/// One allocated guest VRAM slice (`FbSegment` / `NV90E8` subheap) on the physical GPU.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct FbSegment {
    pub vgpu_id: u32,
    pub phys_offset: u64,
    pub usable_fb_bytes: u64,
    pub reservation_bytes: u64,
    pub total_profile_bytes: u64,
}

/// Physical GPU VRAM partition manager tracking active `GP104` vGPU allocations.
#[derive(Debug, Clone)]
pub struct PhysicalVramPartitioner {
    pub total_vram_bytes: u64,
    pub host_reserved_bytes: u64,
    pub ecc_enabled: bool,
    pub active_type_id: Option<u32>,
    pub heterogeneous_allowed: bool,
    pub segments: Vec<FbSegment>,
}

impl PhysicalVramPartitioner {
    /// Construct a new VRAM partitioner for Tesla P4 (`8 GiB` GDDR5).
    pub fn new_tesla_p4(ecc_enabled: bool, heterogeneous_allowed: bool) -> Self {
        Self {
            total_vram_bytes: TESLA_P4_TOTAL_VRAM_BYTES,
            host_reserved_bytes: TESLA_P4_HOST_RM_RESERVED_BYTES,
            ecc_enabled,
            active_type_id: None,
            heterogeneous_allowed,
            segments: Vec::new(),
        }
    }

    /// Return total physical VRAM available to guest vGPUs after ECC + host RM overhead.
    pub fn guest_allocatable_bytes(&self) -> u64 {
        let post_ecc = if self.ecc_enabled {
            self.total_vram_bytes
                - (self.total_vram_bytes * ECC_OVERHEAD_NUMERATOR / ECC_OVERHEAD_DENOMINATOR)
        } else {
            self.total_vram_bytes
        };
        post_ecc.saturating_sub(self.host_reserved_bytes)
    }

    /// Compute effective usable guest framebuffer size (reduced if ECC is active, matching `0x186a60.c`).
    pub fn effective_guest_fb(profile: &VgpuProfile, ecc_enabled: bool) -> u64 {
        if ecc_enabled && profile.ecc_supported {
            profile.framebuffer
                - (profile.framebuffer * ECC_OVERHEAD_NUMERATOR / ECC_OVERHEAD_DENOMINATOR)
        } else {
            profile.framebuffer
        }
    }

    /// Allocate a contiguous `FbSegment` for a new vGPU instance, validating homogeneous/capacity rules.
    pub fn allocate(
        &mut self,
        vgpu_id: u32,
        profile: &VgpuProfile,
        supported: &SupportedVgpuEntry,
    ) -> VgpuResult<FbSegment> {
        if !self.heterogeneous_allowed {
            if let Some(active) = self.active_type_id {
                if active != profile.id {
                    return Err(VgpuError::HeterogeneousConflict(active, profile.id));
                }
            }
        }
        if self.segments.len() >= supported.max_vgpus as usize {
            return Err(VgpuError::CapacityExhausted);
        }

        // Align segment placement after host RM reserved region, 1 MiB aligned.
        let cursor = self
            .segments
            .last()
            .map(|s| s.phys_offset + s.total_profile_bytes)
            .unwrap_or(self.host_reserved_bytes);
        let align = 1024 * 1024;
        let phys_offset = (cursor + align - 1) & !(align - 1);

        let effective_fb = Self::effective_guest_fb(profile, self.ecc_enabled);
        let needed = profile.profile_size;
        let limit = if self.ecc_enabled {
            self.total_vram_bytes
                - (self.total_vram_bytes * ECC_OVERHEAD_NUMERATOR / ECC_OVERHEAD_DENOMINATOR)
        } else {
            self.total_vram_bytes
        };
        if phys_offset.saturating_add(needed) > limit {
            return Err(VgpuError::OutOfVram);
        }

        let seg = FbSegment {
            vgpu_id,
            phys_offset,
            usable_fb_bytes: effective_fb,
            reservation_bytes: profile.fb_reservation,
            total_profile_bytes: needed,
        };
        self.active_type_id = Some(profile.id);
        self.segments.push(seg);
        Ok(seg)
    }

    /// Free an allocated vGPU `FbSegment`. Clears `active_type_id` when the last instance exits.
    pub fn free(&mut self, vgpu_id: u32) -> bool {
        let before = self.segments.len();
        self.segments.retain(|s| s.vgpu_id != vgpu_id);
        if self.segments.is_empty() {
            self.active_type_id = None;
        }
        self.segments.len() != before
    }
}
