//! Clean-room `vmiop` (`VER_5_0` plugin / `VER_2_0` env) ABI and `nvidia-vgpu-mgr`
//! device lifecycle & MMIO/PCI-config emulation orchestrator.

use crate::bar0_emul::{PascalBar0Emulator, TrapOutcome};
use crate::config::{SupportedVgpuEntry, VgpuConfigStore, VgpuProfile};
use crate::pci_cfg::VirtualPciConfig;
use crate::rm_vgpu::{FbSegment, PhysicalVramPartitioner};
use crate::vfio_ioctl::{BarInfoSetDataParams, VgpuEmulSpace, VgpuTypeInfo, NV_VGPU_MAX_BAR_REGION_COUNT};
use crate::vmmu::{ChidManager, FrameRateLimiter, VmmuMapper, GP104_VMMU_SEGMENT_SIZE};
use crate::vrpc::VrpcEngine;
use crate::{VgpuError, VgpuResult};
use std::collections::BTreeMap;

/// Exact signature string checked by `nvidia-vgpu-mgr` (`0x11a6e0.c`).
pub const VMIOP_PLUGIN_SIGNATURE: &[u8; 23] = b"VMIOP_PLUGIN_SIGNATURE\0";
/// Packed `(major=5 << 48) | (minor=0 << 32) | 0xa8` checked at `*plVar3 == 0x50000000000a8`.
pub const VMIOP_PLUGIN_HEADER_V5: u64 = (5u64 << 48) | 0xa8;
pub const VMIOP_PLUGIN_STRUCT_BYTES: usize = 0xa8; // 168 bytes

/// `#[repr(C)]` layout matching `vmiop_display_vmiop_plugin` (`VER_5_0`, `0x590d80`, `168` bytes).
#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct VmiopPluginAbi {
    pub version_and_size: u64,
    pub signature_ptr: usize,
    pub name_ptr: usize,
    pub reserved_18: usize,
    pub reserved_20: usize,
    pub init: usize,
    pub shutdown: usize,
    pub get_attribute: usize,
    pub set_attribute: usize,
    pub reserved_48: usize,
    pub reserved_50: usize,
    pub reserved_58: usize,
    pub migration_prep: usize,
    pub get_migration_state: usize,
    pub read_device_buffer: usize,
    pub write_device_buffer: usize,
    pub get_migration_data_size: usize,
    pub set_migration_state: usize,
    pub notify_migration_stage: usize,
    pub migration_cap_a: usize,
    pub migration_cap_b: usize,
}

const _: () = assert!(core::mem::size_of::<VmiopPluginAbi>() == VMIOP_PLUGIN_STRUCT_BYTES);

/// Live state of a single mediated vGPU device (`vgpu_dev_t` + `libnvidia-vgpu` instance).
#[derive(Debug, Clone)]
pub struct PascalVgpuInstance {
    pub vgpu_id: u32,
    pub mdev_uuid: String,
    pub profile: VgpuProfile,
    pub fb_segment: FbSegment,
    pub pci_cfg: VirtualPciConfig,
    pub bar0: PascalBar0Emulator,
    pub vrpc: VrpcEngine,
    pub vmmu: VmmuMapper,
    pub chidmgr: ChidManager,
    pub frl: FrameRateLimiter,
    pub is_open: bool,
    pub interrupts_injected: u64,
}

impl PascalVgpuInstance {
    /// Handle `NV_VFIO_VGPU_EVENT_VM_REG_ACCESS` for config space or BAR0 MMIO.
    pub fn handle_reg_access(
        &mut self,
        space: VgpuEmulSpace,
        offset: u64,
        width: u32,
        is_write: bool,
        data: u32,
    ) -> VgpuResult<u32> {
        match space {
            VgpuEmulSpace::ConfigSpace => {
                if is_write {
                    self.pci_cfg.write(offset as usize, width as usize, data)?;
                    Ok(0)
                } else {
                    self.pci_cfg.read(offset as usize, width as usize)
                }
            }
            VgpuEmulSpace::Mmio => {
                if width != 4 {
                    return Err(VgpuError::InvalidRegisterAccess(offset, width));
                }
                if is_write {
                    let outcome = self.bar0.write32(offset, data)?;
                    if outcome == TrapOutcome::RpcDoorbellRung {
                        self.interrupts_injected += 1;
                    }
                    Ok(0)
                } else {
                    self.bar0.read32(offset)
                }
            }
            VgpuEmulSpace::IoPort => Err(VgpuError::InvalidRegisterAccess(offset, width)),
        }
    }

    /// Build `NV_VFIO_VGPU_BAR_INFO_SET_DATA_PARAMS` matching `_nv004040vgpu` / `nvidia-vgpu-vfio`.
    ///
    /// BAR0 is trapped (`sparse_count = 0` or optional usermode doorbell sparse area),
    /// BAR1 matches `bar1Length`, and BAR3 is `32 MiB`.
    pub fn bar_info_params(&self) -> BarInfoSetDataParams {
        let mut bar_sizes = [0u64; NV_VGPU_MAX_BAR_REGION_COUNT];
        bar_sizes[0] = self.pci_cfg.bar0_size;
        bar_sizes[1] = self.pci_cfg.bar1_size;
        bar_sizes[3] = self.pci_cfg.bar3_size;
        BarInfoSetDataParams {
            bar_sizes,
            sparse_offsets: [0; 8],
            sparse_sizes: [0; 8],
            sparse_count: 0,
            return_status: 0,
        }
    }
}

/// Clean-room Rust host manager (`nvidia-vgpud` + `nvidia-vgpu-mgr` unified control plane)
/// for a single physical Tesla P4 (`GP104GL`).
#[derive(Debug, Clone)]
pub struct PascalVgpuManager {
    pub vendor_id: u16,
    pub device_id: u16,
    pub config: VgpuConfigStore,
    pub vram: PhysicalVramPartitioner,
    pub instances: BTreeMap<u32, PascalVgpuInstance>,
    next_vgpu_id: u32,
}

impl PascalVgpuManager {
    /// Initialize manager for physical GPU `(vendor_id, device_id)` from `vgpuConfig.xml`.
    pub fn new(
        config: VgpuConfigStore,
        vendor_id: u16,
        device_id: u16,
        ecc_enabled: bool,
    ) -> VgpuResult<Self> {
        // Verify GPU exists in vgpuConfig.xml (`nvidia-vgpud` probe check).
        let _pgpu = config
            .find_pgpu(vendor_id, device_id)
            .ok_or(VgpuError::UnsupportedGpu(vendor_id, device_id))?;
        let hetero = config.global.heterogeneous_multi_vgpu;
        Ok(Self {
            vendor_id,
            device_id,
            config,
            vram: PhysicalVramPartitioner::new_tesla_p4(ecc_enabled, hetero),
            instances: BTreeMap::new(),
            next_vgpu_id: 1,
        })
    }

    /// Generate `type_info_t` records pushed via `NV_VFIO_VGPU_CTLDEV_SET_TYPES_INFO`.
    pub fn build_vfio_type_infos(&self) -> VgpuResult<Vec<VgpuTypeInfo>> {
        let pairs = self
            .config
            .supported_profiles_for_gpu(self.vendor_id, self.device_id)?;
        let mut out = Vec::with_capacity(pairs.len());
        for (sup, prof) in pairs {
            let mut vgpu_name = [0u8; 32];
            let bytes = prof.name.as_bytes();
            let n = bytes.len().min(31);
            vgpu_name[..n].copy_from_slice(&bytes[..n]);
            out.push(VgpuTypeInfo {
                vgpu_type_id: prof.id,
                num_heads: prof.num_heads,
                max_instance: sup.max_vgpus,
                max_resolution_x: prof.display_width,
                max_resolution_y: prof.display_height,
                frl_config: prof.frl_config,
                profile_size_mb: (prof.profile_size >> 20) as u32,
                vgpu_name,
            });
        }
        Ok(out)
    }

    /// Query available mdev instances for `vgpu_type_id` (`NV_VFIO_VGPU_CTLDEV_EVENT_GET_INSTANCES`).
    pub fn available_instances(&self, vgpu_type_id: u32) -> VgpuResult<u32> {
        let (sup, prof) = self.find_supported(vgpu_type_id)?;
        if !self.vram.heterogeneous_allowed {
            if let Some(active) = self.vram.active_type_id {
                if active != vgpu_type_id {
                    return Ok(0);
                }
            }
        }
        let used = self
            .instances
            .values()
            .filter(|inst| inst.profile.id == vgpu_type_id)
            .count() as u32;
        let max_by_type = sup.max_vgpus.saturating_sub(used);
        let used_vram: u64 = self.vram.segments.iter().map(|s| s.total_profile_bytes).sum();
        let free_vram = self.vram.guest_allocatable_bytes().saturating_sub(used_vram);
        let max_by_vram = (free_vram / prof.profile_size) as u32;
        Ok(max_by_type.min(max_by_vram))
    }

    /// Create a mediated vGPU instance (`NV_VFIO_VGPU_CTLDEV_EVENT_CREATE_DEVICE`).
    pub fn create_device(&mut self, vgpu_type_id: u32, mdev_uuid: &str) -> VgpuResult<u32> {
        let (sup, prof) = self.find_supported(vgpu_type_id)?;
        let sup_owned = sup.clone();
        let prof_owned = prof.clone();
        let vgpu_id = self.next_vgpu_id;

        let fb_segment = self.vram.allocate(vgpu_id, &prof_owned, &sup_owned)?;
        let pci_cfg = VirtualPciConfig::from_profile(&prof_owned);
        let bar0 = PascalBar0Emulator::new_gp104(
            fb_segment.usable_fb_bytes,
            self.vram.ecc_enabled && prof_owned.ecc_supported,
        );
        let vrpc = VrpcEngine::new(
            fb_segment.usable_fb_bytes,
            prof_owned.dev_id.device_id,
            prof_owned.dev_id.subsystem_id,
        );
        let mut vmmu = VmmuMapper::new(GP104_VMMU_SEGMENT_SIZE);
        vmmu.map_fb_range(vgpu_id, fb_segment.phys_offset, fb_segment.total_profile_bytes, 0)?;

        // Partition 128 channels per guest instance across GP104's 4096 physical channels.
        let chidmgr = ChidManager::new((vgpu_id - 1) * 128, 128);
        let frl = FrameRateLimiter::new(prof_owned.frame_rate_limiter, prof_owned.frl_config);

        let inst = PascalVgpuInstance {
            vgpu_id,
            mdev_uuid: mdev_uuid.to_string(),
            profile: prof_owned,
            fb_segment,
            pci_cfg,
            bar0,
            vrpc,
            vmmu,
            chidmgr,
            frl,
            is_open: false,
            interrupts_injected: 0,
        };
        self.instances.insert(vgpu_id, inst);
        self.next_vgpu_id += 1;
        Ok(vgpu_id)
    }

    /// Open an existing vGPU device (`NV_VFIO_VGPU_CTLDEV_EVENT_OPEN_DEVICE`).
    pub fn open_device(&mut self, vgpu_id: u32) -> VgpuResult<&mut PascalVgpuInstance> {
        let inst = self
            .instances
            .get_mut(&vgpu_id)
            .ok_or(VgpuError::DeviceNotFound(vgpu_id))?;
        inst.is_open = true;
        Ok(inst)
    }

    /// Destroy a vGPU device and reclaim VRAM (`NV_VFIO_VGPU_CTLDEV_EVENT_DELETE_DEVICE`).
    pub fn delete_device(&mut self, vgpu_id: u32) -> VgpuResult<()> {
        if self.instances.remove(&vgpu_id).is_none() {
            return Err(VgpuError::DeviceNotFound(vgpu_id));
        }
        self.vram.free(vgpu_id);
        Ok(())
    }

    fn find_supported(&self, vgpu_type_id: u32) -> VgpuResult<(&SupportedVgpuEntry, &VgpuProfile)> {
        let pgpu = self
            .config
            .find_pgpu(self.vendor_id, self.device_id)
            .ok_or(VgpuError::UnsupportedGpu(self.vendor_id, self.device_id))?;
        let sup = pgpu
            .supported
            .iter()
            .find(|s| s.vgpu_id == vgpu_type_id)
            .ok_or(VgpuError::ProfileNotFound(vgpu_type_id))?;
        let prof = self
            .config
            .profile_by_id(vgpu_type_id)
            .ok_or(VgpuError::ProfileNotFound(vgpu_type_id))?;
        Ok((sup, prof))
    }
}
