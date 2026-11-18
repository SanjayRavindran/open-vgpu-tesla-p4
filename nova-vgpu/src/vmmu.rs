//! Virtual MMU (`VMMU`), Channel ID Virtualization (`CHIDMGR`), and
//! Frame Rate Limiter (`FRL`) reimplemented in clean-room Rust.

use crate::{VgpuError, VgpuResult};
use std::collections::BTreeMap;

/// Default GP104 VMMU segment granularity (256 MiB = `0x1000_0000` bytes).
pub const GP104_VMMU_SEGMENT_SIZE: u64 = 256 * 1024 * 1024;
/// GP104 physical channel count (`NV_PFIFO` = 4096 channels).
pub const GP104_MAX_CHANNELS: u32 = 4096;

/// One mapped VMMU segment backing guest FB or pinned system pages (`FUN_0016a800`).
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct VmmuSegment {
    pub segment_index: u32,
    pub vgpu_id: u32,
    pub phys_offset: u64,
    pub size: u64,
    pub kind_flags: u32,
}

/// Virtual MMU segment & guest-PFN translation table.
#[derive(Debug, Clone)]
pub struct VmmuMapper {
    pub segment_size: u64,
    pub segments: Vec<VmmuSegment>,
    pub gfn_to_hfn: BTreeMap<u64, u64>,
}

impl VmmuMapper {
    pub fn new(segment_size: u64) -> Self {
        Self {
            segment_size: if segment_size == 0 {
                GP104_VMMU_SEGMENT_SIZE
            } else {
                segment_size
            },
            segments: Vec::new(),
            gfn_to_hfn: BTreeMap::new(),
        }
    }

    /// Map a physical `FbSegment` slice into VMMU segments (`0x186a60.c`).
    pub fn map_fb_range(
        &mut self,
        vgpu_id: u32,
        phys_start: u64,
        len: u64,
        kind_flags: u32,
    ) -> VgpuResult<usize> {
        let mut mapped = 0usize;
        let mut off = 0u64;
        while off < len {
            let seg_idx = ((phys_start + off) / self.segment_size) as u32;
            let seg_len = (len - off).min(self.segment_size);
            self.segments.push(VmmuSegment {
                segment_index: seg_idx,
                vgpu_id,
                phys_offset: phys_start + off,
                size: seg_len,
                kind_flags,
            });
            off += seg_len;
            mapped += 1;
        }
        Ok(mapped)
    }

    /// Pin & register a guest PFN -> host PFN translation (`NV_VFIO_VGPU_TRANSLATE_GFN_TO_PFN`).
    pub fn pin_gfn(&mut self, gfn: u64, hfn: u64) {
        self.gfn_to_hfn.insert(gfn, hfn);
    }

    /// Unpin a guest PFN (`NV_VFIO_VGPU_UNPIN_PAGES`).
    pub fn unpin_gfn(&mut self, gfn: u64) -> VgpuResult<u64> {
        self.gfn_to_hfn
            .remove(&gfn)
            .ok_or(VgpuError::InvalidGuestPfn(gfn))
    }

    /// Translate guest PFN to host PFN.
    pub fn translate(&self, gfn: u64) -> Option<u64> {
        self.gfn_to_hfn.get(&gfn).copied()
    }
}

/// Engine family indices managed by `CHIDMGR` (`0x181e50.c`, `0x198d50.c`).
#[derive(Debug, Copy, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub enum EngineKind {
    Gr,
    Copy,
    Nvdec,
    Nvenc,
    Nvjpg,
}

/// Channel ID allocation pairing a guest-visible `guest_chid` with a host `phys_chid`.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct ChannelMapping {
    pub engine: EngineKind,
    pub guest_chid: u32,
    pub phys_chid: u32,
}

/// Clean-room `CHIDMGR` partitioning `GP104` hardware channels across vGPU guests.
#[derive(Debug, Clone)]
pub struct ChidManager {
    pub sys_chid_base: u32,
    pub guest_chid_span: u32,
    next_phys_cursor: u32,
    mappings: BTreeMap<(EngineKind, u32), u32>,
}

impl ChidManager {
    pub fn new(sys_chid_base: u32, guest_chid_span: u32) -> Self {
        Self {
            sys_chid_base,
            guest_chid_span,
            next_phys_cursor: sys_chid_base,
            mappings: BTreeMap::new(),
        }
    }

    /// Allocate and bind `(guest_chid -> phys_chid)` for an engine channel.
    pub fn bind_channel(&mut self, engine: EngineKind, guest_chid: u32) -> VgpuResult<ChannelMapping> {
        if guest_chid >= self.guest_chid_span {
            return Err(VgpuError::InvalidChannel(guest_chid));
        }
        if let Some(&existing) = self.mappings.get(&(engine, guest_chid)) {
            return Ok(ChannelMapping {
                engine,
                guest_chid,
                phys_chid: existing,
            });
        }
        if self.next_phys_cursor >= GP104_MAX_CHANNELS {
            return Err(VgpuError::CapacityExhausted);
        }
        let phys_chid = self.next_phys_cursor;
        self.next_phys_cursor += 1;
        self.mappings.insert((engine, guest_chid), phys_chid);
        Ok(ChannelMapping {
            engine,
            guest_chid,
            phys_chid,
        })
    }

    /// Look up physical channel ID for a guest channel.
    pub fn phys_for_guest(&self, engine: EngineKind, guest_chid: u32) -> Option<u32> {
        self.mappings.get(&(engine, guest_chid)).copied()
    }

    /// Release a channel mapping.
    pub fn release_channel(&mut self, engine: EngineKind, guest_chid: u32) -> bool {
        self.mappings.remove(&(engine, guest_chid)).is_some()
    }
}

/// Frame Rate Limiter (`FRL`) pacing state (`0x166607.c`, `vgpuConfig.xml` `frlConfig`).
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct FrameRateLimiter {
    pub enabled: bool,
    pub target_fps: u32,
    pub frame_interval_ns: u64,
}

impl FrameRateLimiter {
    pub const fn new(enabled: bool, frl_config_fps: u32) -> Self {
        let fps = if frl_config_fps == 0 { 60 } else { frl_config_fps };
        let interval = if enabled {
            1_000_000_000u64 / (fps as u64)
        } else {
            0
        };
        Self {
            enabled,
            target_fps: fps,
            frame_interval_ns: interval,
        }
    }

    /// Return `true` if a frame submission at `now_ns` after `last_frame_ns` is within budget,
    /// or `Err(wait_ns)` indicating how many nanoseconds to throttle.
    pub const fn check_pacing(&self, last_frame_ns: u64, now_ns: u64) -> Result<(), u64> {
        if !self.enabled || self.frame_interval_ns == 0 {
            return Ok(());
        }
        let elapsed = now_ns.saturating_sub(last_frame_ns);
        if elapsed >= self.frame_interval_ns {
            Ok(())
        } else {
            Err(self.frame_interval_ns - elapsed)
        }
    }
}
