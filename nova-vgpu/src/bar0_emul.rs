//! Pascal (`GP104`) BAR0 MMIO trap table and register virtualization
//! (`_nv004044vgpu`, `_nv007333vgpu`, `_nv007414vgpu`, `_nv007794vgpu`, `_nv008136vgpu`).

use crate::vrpc::VgpuRingState;
use crate::{VgpuError, VgpuResult};

/// Virtualized `NV_VGPU` BAR0 window base on Pascal (non-SR-IOV, `_nv007333vgpu`).
pub const NV_VGPU_BAR0_BASE: u64 = 0x0008_f000;
pub const NV_VGPU_BAR0_SIZE: u64 = 0x0000_1000;

/// Offsets inside `NV_VGPU` (`0x8f000..0x90000`) observed in `_nv007414vgpu` (`0x262150.c`).
pub mod nv_vgpu_reg {
    pub const SEND_RING_LO: u64 = 0x00;
    pub const SEND_RING_HI: u64 = 0x04;
    pub const SEND_RING_PUT: u64 = 0x08;
    pub const SEND_RING_GET: u64 = 0x0c;
    pub const RECV_RING_LO: u64 = 0x10;
    pub const RECV_RING_HI: u64 = 0x14;
    pub const RECV_RING_PUT: u64 = 0x18;
    pub const RECV_RING_GET: u64 = 0x1c;
    pub const VGPU_CONFIG: u64 = 0x20;
    pub const VGPU_FB_SIZE_4K_MINUS_1: u64 = 0x24;
    pub const VGPU_INTR_STATUS: u64 = 0x28;
    pub const VGPU_INTR_ENABLE: u64 = 0x2c;
    pub const EVENT_RING_LO: u64 = 0x30;
    pub const EVENT_RING_HI: u64 = 0x34;
    pub const EVENT_RING_PUT: u64 = 0x38;
    pub const EVENT_RING_GET: u64 = 0x3c;
    pub const RPC_DOORBELL: u64 = 0x50;
    pub const MSG_RING_CTL: u64 = 0x54;
    pub const SCRATCH_STATUS: u64 = 0x5c;
}

/// Whitelisted read-only `NV_PGRAPH` TPC/SM floor-sweeping offsets (`_nv008136vgpu` / `0x26a050.c`).
const PGRAPH_FS_READ_WHITELIST: &[u64] = &[
    0x500c30, 0x500c34, 0x500c38, 0x502608, 0x508c30, 0x508c34, 0x508c38, 0x50a608, 0x510c30,
    0x510c34, 0x510c38, 0x512608, 0x518c30, 0x518c34, 0x518c38, 0x51a608, 0x520c30, 0x520c34,
    0x520c38, 0x522608, 0x528c30, 0x528c34, 0x528c38, 0x52a608,
];

/// Read-only `NV_PFB_FBPA_*_CSTATUS` offsets returning virtualized VRAM size (`_nv007794vgpu`).
const PFB_CSTATUS_OFFSETS: &[u64] = &[0x10f20c, 0x11020c, 0x11120c, 0x11220c, 0x11320c];

/// Outcome of a trapped BAR0 register access.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum TrapOutcome {
    Handled,
    /// Guest rang `NV_VGPU` RPC doorbell (`0x8f050`) — host RPC worker must drain `SEND_RING`.
    RpcDoorbellRung,
    /// Guest toggled ECC mode in `VGPU_CONFIG` (`0x8f020` bit 18).
    EccModeUpdated(bool),
}

/// Emulated Pascal `GP104` BAR0 register state for one vGPU instance.
#[derive(Debug, Clone)]
pub struct PascalBar0Emulator {
    pub pmc_boot0: u32,
    pub pmc_boot42: u32,
    pub pmc_enable: u32,
    pub pmc_intr_0: u32,
    pub vgpu_config: u32,
    pub vgpu_fb_pages_minus_1: u32,
    pub vgpu_intr_status: u32,
    pub vgpu_intr_enable: u32,
    pub vgpu_msg_ring_ctl: u32,
    pub vgpu_scratch: u32,
    pub send_ring: VgpuRingState,
    pub recv_ring: VgpuRingState,
    pub event_ring: VgpuRingState,
    pub fb_size_mb_reg: u32,
    pub ecc_enabled_by_guest: bool,
    pub phys_pgraph_fs_default: u32,
}

impl PascalBar0Emulator {
    /// Create a BAR0 emulator for a Tesla P4 (`GP104`) vGPU with `usable_fb_bytes`.
    pub fn new_gp104(usable_fb_bytes: u64, ecc_supported: bool) -> Self {
        let fb_pages_m1 = ((usable_fb_bytes.saturating_sub(1)) >> 12) as u32;
        let fb_mb = (usable_fb_bytes >> 20) as u32;
        // Base VGPU_CONFIG = 0x201 | optional ECC capability bit (1<<18)
        let vgpu_config = 0x201 | if ecc_supported { 1 << 18 } else { 0 };
        Self {
            pmc_boot0: 0x1340_00a1, // GP104 A1
            pmc_boot42: (0x13 << 24) | (0x4 << 20) | (0xa << 16) | (1 << 12),
            pmc_enable: 0x0001_0100, // *_nv008153vgpu: *puVar1 | 0x10100
            pmc_intr_0: 0,
            vgpu_config,
            vgpu_fb_pages_minus_1: fb_pages_m1,
            vgpu_intr_status: 0,
            vgpu_intr_enable: 0,
            vgpu_msg_ring_ctl: 0,
            vgpu_scratch: 0,
            send_ring: VgpuRingState {
                entry_size: 0x1000,
                capacity_bytes: 0x1000,
                ..Default::default()
            },
            recv_ring: VgpuRingState {
                entry_size: 0x1000,
                capacity_bytes: 0x1000,
                ..Default::default()
            },
            event_ring: VgpuRingState {
                entry_size: 0x28,
                capacity_bytes: 0x1000,
                ..Default::default()
            },
            fb_size_mb_reg: fb_mb,
            ecc_enabled_by_guest: ecc_supported,
            phys_pgraph_fs_default: 0x000f_0000,
        }
    }

    /// Emulate a 32-bit BAR0 read (`_nv009111vgpu` -> `_nv004044vgpu`).
    pub fn read32(&self, offset: u64) -> VgpuResult<u32> {
        if (offset & 3) != 0 || offset >= 0x0100_0000 {
            return Err(VgpuError::InvalidRegisterAccess(offset, 4));
        }
        let val = match offset {
            // NV_PMC (0x0000..0x1000)
            0x000000 => self.pmc_boot0,
            0x000004 => self.pmc_enable,
            0x000100 | 0x000104 | 0x000140 | 0x000144 => self.pmc_intr_0,
            0x000a00 => self.pmc_boot42,
            0x000000..=0x000fff => 0,
            // NV_UFLUSH (0x70000..0x70010, _nv007800vgpu)
            0x070000 | 0x070004 | 0x070008 | 0x07000c | 0x070010 => 0,
            0x070000..=0x070fff => u32::MAX,
            // NV_VGPU (0x8f000..0x90000, _nv007414vgpu)
            0x08f000..=0x08ffff => self.read_nv_vgpu(offset - NV_VGPU_BAR0_BASE),
            // NV_PFB / FBPA CSTATUS virtualized VRAM size (_nv007794vgpu)
            o if PFB_CSTATUS_OFFSETS.contains(&o) => self.fb_size_mb_reg,
            0x100000..=0x1203ff => 0,
            // NV_PGRAPH (0x400000..0x600000, _nv008136vgpu)
            o if PGRAPH_FS_READ_WHITELIST.contains(&o) => self.phys_pgraph_fs_default,
            0x400000..=0x5fffff => 0,
            _ => 0,
        };
        Ok(val)
    }

    /// Emulate a 32-bit BAR0 write (`_nv009111vgpu` -> `_nv004044vgpu`).
    pub fn write32(&mut self, offset: u64, value: u32) -> VgpuResult<TrapOutcome> {
        if (offset & 3) != 0 || offset >= 0x0100_0000 {
            return Err(VgpuError::InvalidRegisterAccess(offset, 4));
        }
        match offset {
            0x000004 => {
                self.pmc_enable = value | 0x0001_0100;
                Ok(TrapOutcome::Handled)
            }
            0x070000..=0x070fff => Ok(TrapOutcome::Handled),
            0x08f000..=0x08ffff => self.write_nv_vgpu(offset - NV_VGPU_BAR0_BASE, value),
            // NV_PGRAPH writes are silently absorbed/protected unless NvWatch is active (_nv008136vgpu)
            0x400000..=0x5fffff => Ok(TrapOutcome::Handled),
            _ => Ok(TrapOutcome::Handled),
        }
    }

    fn read_nv_vgpu(&self, reg: u64) -> u32 {
        match reg {
            nv_vgpu_reg::SEND_RING_LO => self.send_ring.gpa_lo,
            nv_vgpu_reg::SEND_RING_HI => self.send_ring.gpa_hi,
            nv_vgpu_reg::SEND_RING_PUT => self.send_ring.put,
            nv_vgpu_reg::SEND_RING_GET => self.send_ring.get,
            nv_vgpu_reg::RECV_RING_LO => self.recv_ring.gpa_lo,
            nv_vgpu_reg::RECV_RING_HI => self.recv_ring.gpa_hi,
            nv_vgpu_reg::RECV_RING_PUT => self.recv_ring.put,
            nv_vgpu_reg::RECV_RING_GET => self.recv_ring.get,
            nv_vgpu_reg::VGPU_CONFIG => self.vgpu_config,
            nv_vgpu_reg::VGPU_FB_SIZE_4K_MINUS_1 => self.vgpu_fb_pages_minus_1,
            nv_vgpu_reg::VGPU_INTR_STATUS => self.vgpu_intr_status,
            nv_vgpu_reg::VGPU_INTR_ENABLE => self.vgpu_intr_enable,
            nv_vgpu_reg::EVENT_RING_LO => self.event_ring.gpa_lo,
            nv_vgpu_reg::EVENT_RING_HI => self.event_ring.gpa_hi,
            nv_vgpu_reg::EVENT_RING_PUT => self.event_ring.put,
            nv_vgpu_reg::EVENT_RING_GET => self.event_ring.get,
            nv_vgpu_reg::MSG_RING_CTL => self.vgpu_msg_ring_ctl,
            nv_vgpu_reg::SCRATCH_STATUS => self.vgpu_scratch,
            _ => 0,
        }
    }

    fn write_nv_vgpu(&mut self, reg: u64, value: u32) -> VgpuResult<TrapOutcome> {
        match reg {
            nv_vgpu_reg::SEND_RING_LO => {
                self.send_ring.gpa_lo = value;
                Ok(TrapOutcome::Handled)
            }
            nv_vgpu_reg::SEND_RING_HI => {
                self.send_ring.gpa_hi = value;
                Ok(TrapOutcome::Handled)
            }
            nv_vgpu_reg::SEND_RING_PUT => {
                self.send_ring.put = value & 0xffff_fff8;
                Ok(TrapOutcome::Handled)
            }
            nv_vgpu_reg::RECV_RING_LO => {
                self.recv_ring.gpa_lo = value;
                Ok(TrapOutcome::Handled)
            }
            nv_vgpu_reg::RECV_RING_HI => {
                self.recv_ring.gpa_hi = value;
                Ok(TrapOutcome::Handled)
            }
            nv_vgpu_reg::RECV_RING_GET => {
                self.recv_ring.get = value & 0xffff_fff8;
                Ok(TrapOutcome::Handled)
            }
            nv_vgpu_reg::VGPU_CONFIG => {
                let ecc_bit = 1u32 << 18;
                if ((self.vgpu_config ^ value) & ecc_bit) != 0 {
                    let ecc_on = (value & ecc_bit) != 0;
                    self.ecc_enabled_by_guest = ecc_on;
                    self.vgpu_config = (self.vgpu_config & !ecc_bit) | (value & ecc_bit);
                    return Ok(TrapOutcome::EccModeUpdated(ecc_on));
                }
                Ok(TrapOutcome::Handled)
            }
            nv_vgpu_reg::VGPU_INTR_STATUS => {
                // Write-1-to-clear interrupt status bits [9:6] matching 0x262150.c
                let w1c = value & 0x3c0;
                self.vgpu_intr_status &= !w1c;
                Ok(TrapOutcome::Handled)
            }
            nv_vgpu_reg::VGPU_INTR_ENABLE => {
                self.vgpu_intr_enable = value & 0x1ff;
                Ok(TrapOutcome::Handled)
            }
            nv_vgpu_reg::EVENT_RING_LO => {
                self.event_ring.gpa_lo = value;
                Ok(TrapOutcome::Handled)
            }
            nv_vgpu_reg::EVENT_RING_HI => {
                self.event_ring.gpa_hi = value;
                Ok(TrapOutcome::Handled)
            }
            nv_vgpu_reg::EVENT_RING_GET => {
                self.event_ring.get = value & 0xffff_fffc;
                Ok(TrapOutcome::Handled)
            }
            nv_vgpu_reg::RPC_DOORBELL => Ok(TrapOutcome::RpcDoorbellRung),
            nv_vgpu_reg::MSG_RING_CTL => {
                self.vgpu_msg_ring_ctl = value;
                Ok(TrapOutcome::Handled)
            }
            _ => Ok(TrapOutcome::Handled),
        }
    }
}
