//! Clean-room Rust Guest vGPU Driver for Pascal (`GP104GL` / Tesla P4) KVM guests.
//!
//! Reverse-engineered from `NVIDIA-Linux-x86_64-535.309.01-grid.run`
//! (`kernel/nvidia/nv-kernel.o_binary`: `_nv013341rm`, `_nv013262rm`, `_nv013259rm`,
//! `_nv044238rm`, `_nv045460rm`, `_nv037870rm`) and `kernel-open` (`objvgpu.c`, `rpc.c`).
//!
//! Because all physical GP104 Falcons (`ACR`, `PMU`, `SEC2`, `FECS`, `GPCCS`, `NVENC`,
//! `NVDEC`) live on the host GPU and are authenticated there under `PrivSec = 1`, the
//! guest driver requires **zero NVIDIA-signed Falcon or GSP firmware binaries**. It acts
//! strictly as a paravirtualized `0x8f000` BAR0 + `"VRPC"` (`0x43505256`) shared-ring
//! client.

use crate::bar0_emul::{nv_vgpu_reg, NV_VGPU_BAR0_BASE};
use crate::vfio_ioctl::VgpuEmulSpace;
use crate::vmiop::PascalVgpuInstance;
use crate::vrpc::{VrpcFuncId, VrpcHeader, VRPC_HEADER_BYTES, VRPC_SIGNATURE, VRPC_VERSION_3_0};
use crate::{VgpuError, VgpuResult};
use std::collections::BTreeMap;

/// `NV_PMC_BOOT_0` offset inside BAR0.
pub const NV_PMC_BOOT_0: u64 = 0x0000_0000;
/// `NV_PFB_FBPA_0_CSTATUS` virtualized VRAM size register (in MiB).
pub const NV_PFB_FBPA_0_CSTATUS: u64 = 0x0010_f20c;
/// `NV_PGRAPH` floor-sweeping whitelisted register (`NV_PGRAPH_PRI_GPC0_TPCS_...`).
pub const NV_PGRAPH_GPC0_FS_WHITELIST: u64 = 0x0050_0c30;

/// Encode the low 32 bits of a 4 KiB-aligned guest physical page ring descriptor
/// matching `_nv013341rm` / `_nv013262rm`: `((pfn & 0xfffff) << 12) | 0xb` (or `0x80b` for v2).
pub const fn encode_ring_pte_lo(gpa: u64, ring_v2: bool) -> u32 {
    let pfn_lo = ((gpa >> 12) as u32) & 0x000f_ffff;
    let flags = if ring_v2 { 0x80b } else { 0xb };
    (pfn_lo << 12) | flags
}

/// Encode the high 32 bits of a ring descriptor (`(pfn >> 20) & 0xfffff`).
pub const fn encode_ring_pte_hi(gpa: u64) -> u32 {
    let pfn_hi = (gpa >> 32) as u32;
    pfn_hi & 0x000f_ffff
}

/// Decode a `(lo, hi)` ring PTE pair back into the 4 KiB-aligned guest physical address.
pub const fn decode_ring_pte(lo: u32, hi: u32) -> u64 {
    let pfn = (((hi as u64) & 0x000f_ffff) << 20) | (((lo as u64) >> 12) & 0x000f_ffff);
    pfn << 12
}

/// Combined guest MMIO + 4 KiB DMA physical memory interface backing
/// `SEND_RING`, `RECV_RING`, `EVENT_RING`, and `"VRPC"` staging pages.
pub trait GuestVgpuTransport {
    fn read32(&mut self, offset: u64) -> VgpuResult<u32>;
    fn write32(&mut self, offset: u64, value: u32) -> VgpuResult<()>;
    fn alloc_zeroed_page(&mut self) -> VgpuResult<u64>;
    fn read_page(&self, gpa: u64, out: &mut [u8; 4096]) -> VgpuResult<()>;
    fn write_page(&mut self, gpa: u64, data: &[u8; 4096]) -> VgpuResult<()>;
    fn write_dword(&mut self, gpa: u64, byte_off: usize, value: u32) -> VgpuResult<()>;
    fn read_dword(&self, gpa: u64, byte_off: usize) -> VgpuResult<u32>;
}

/// Static hardware + paravirtual geometry discovered during clean-room guest probe.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct GuestVgpuCaps {
    pub pmc_boot0: u32,
    pub vgpu_config: u32,
    pub ecc_supported: bool,
    pub virtual_fb_bytes: u64,
    pub cstatus_mb: u32,
    pub pgraph_fs_mask: u32,
}

/// Clean-room open-source Pascal vGPU guest driver state.
#[derive(Debug, Clone)]
pub struct NovaGuestDriver {
    pub caps: GuestVgpuCaps,
    pub send_ring_gpa: u64,
    pub recv_ring_gpa: u64,
    pub event_ring_gpa: u64,
    pub staging_gpa: u64,
    pub send_put: u32,
    pub recv_get: u32,
    pub next_seq: u32,
    pub vgx_initialized: bool,
    pub ring_v2: bool,
    pub rpc_calls_completed: u64,
}

fn program_ring_window<T: GuestVgpuTransport>(
    tx: &mut T,
    lo_reg: u64,
    hi_reg: u64,
    ptr_reg: u64,
    gpa: u64,
    ring_v2: bool,
) -> VgpuResult<()> {
    tx.write32(NV_VGPU_BAR0_BASE + lo_reg, 0)?;
    tx.write32(NV_VGPU_BAR0_BASE + ptr_reg, 0)?;
    tx.write32(NV_VGPU_BAR0_BASE + hi_reg, encode_ring_pte_hi(gpa))?;
    tx.write32(
        NV_VGPU_BAR0_BASE + lo_reg,
        encode_ring_pte_lo(gpa, ring_v2),
    )?;
    Ok(())
}

impl NovaGuestDriver {
    /// Probe the virtual GP104 PCI BAR0 window (`0x8f000`) without touching Falcon firmware,
    /// allocate 4 KiB DMA rings, and program `SEND_RING`, `RECV_RING`, and `EVENT_RING`.
    pub fn probe_and_init<T: GuestVgpuTransport>(tx: &mut T, ring_v2: bool) -> VgpuResult<Self> {
        let pmc_boot0 = tx.read32(NV_PMC_BOOT_0)?;
        let chip_arch = (pmc_boot0 >> 20) & 0xfff;
        // Accept Pascal GP104 (0x134) / Pascal family (0x13x)
        if (chip_arch & 0xff0) != 0x130 {
            return Err(VgpuError::UnsupportedGpu(0x10de, chip_arch as u16));
        }

        let vgpu_config = tx.read32(NV_VGPU_BAR0_BASE + nv_vgpu_reg::VGPU_CONFIG)?;
        // Bit 0 in VGPU_CONFIG (0x8f020) indicates active virtualized vGPU BAR0 mediation.
        if (vgpu_config & 1) == 0 {
            return Err(VgpuError::InvalidRegisterAccess(
                NV_VGPU_BAR0_BASE + nv_vgpu_reg::VGPU_CONFIG,
                vgpu_config,
            ));
        }

        let fb_pages_m1 = tx.read32(NV_VGPU_BAR0_BASE + nv_vgpu_reg::VGPU_FB_SIZE_4K_MINUS_1)?;
        let virtual_fb_bytes = ((fb_pages_m1 as u64) + 1) << 12;
        let cstatus_mb = tx.read32(NV_PFB_FBPA_0_CSTATUS)?;
        let pgraph_fs_mask = tx.read32(NV_PGRAPH_GPC0_FS_WHITELIST)?;

        let send_ring_gpa = tx.alloc_zeroed_page()?;
        let recv_ring_gpa = tx.alloc_zeroed_page()?;
        let event_ring_gpa = tx.alloc_zeroed_page()?;
        let staging_gpa = tx.alloc_zeroed_page()?;

        // Matches _nv013341rm initialization sequence on 0x8f000..0x8f03c.
        program_ring_window(
            tx,
            nv_vgpu_reg::SEND_RING_LO,
            nv_vgpu_reg::SEND_RING_HI,
            nv_vgpu_reg::SEND_RING_PUT,
            send_ring_gpa,
            ring_v2,
        )?;
        program_ring_window(
            tx,
            nv_vgpu_reg::RECV_RING_LO,
            nv_vgpu_reg::RECV_RING_HI,
            nv_vgpu_reg::RECV_RING_GET,
            recv_ring_gpa,
            ring_v2,
        )?;
        program_ring_window(
            tx,
            nv_vgpu_reg::EVENT_RING_LO,
            nv_vgpu_reg::EVENT_RING_HI,
            nv_vgpu_reg::EVENT_RING_GET,
            event_ring_gpa,
            false,
        )?;

        tx.write32(NV_VGPU_BAR0_BASE + nv_vgpu_reg::VGPU_INTR_ENABLE, 0x1ff)?;

        Ok(Self {
            caps: GuestVgpuCaps {
                pmc_boot0,
                vgpu_config,
                ecc_supported: (vgpu_config & (1 << 18)) != 0,
                virtual_fb_bytes,
                cstatus_mb,
                pgraph_fs_mask,
            },
            send_ring_gpa,
            recv_ring_gpa,
            event_ring_gpa,
            staging_gpa,
            send_put: 0,
            recv_get: 0,
            next_seq: 1,
            vgx_initialized: false,
            ring_v2,
            rpc_calls_completed: 0,
        })
    }

    /// Issue a synchronous `"VRPC"` call over `SEND_RING` + `0x8f008`/`0x8f050` doorbell
    /// and retrieve the host's response via `RECV_RING` (`_nv013262rm` + `_nv013259rm`).
    pub fn call_vrpc<T: GuestVgpuTransport>(
        &mut self,
        tx: &mut T,
        func: VrpcFuncId,
        payload: &[u8],
    ) -> VgpuResult<(u32, Vec<u8>)> {
        let total_len = VRPC_HEADER_BYTES + payload.len();
        if total_len > 4096 {
            return Err(VgpuError::InvalidVrpcLength(total_len as u32));
        }

        let seq = self.next_seq;
        self.next_seq = self.next_seq.wrapping_add(1);

        let mut page = [0u8; 4096];
        let mut hdr = VrpcHeader::new_request(func, payload.len() as u32);
        hdr.version = VRPC_VERSION_3_0;
        hdr.signature = VRPC_SIGNATURE;
        hdr.sequence = seq;
        hdr.encode(&mut page)?;
        page[VRPC_HEADER_BYTES..total_len].copy_from_slice(payload);
        tx.write_page(self.staging_gpa, &page)?;

        // Write 8-byte descriptor inside SEND_RING[PUT] matching _nv013262rm
        let slot_off = (self.send_put as usize) & 0xff8;
        tx.write_dword(
            self.send_ring_gpa,
            slot_off,
            encode_ring_pte_lo(self.staging_gpa, self.ring_v2),
        )?;
        tx.write_dword(
            self.send_ring_gpa,
            slot_off + 4,
            encode_ring_pte_hi(self.staging_gpa),
        )?;

        self.send_put = (self.send_put + 8) & 0xfff;
        tx.write32(NV_VGPU_BAR0_BASE + nv_vgpu_reg::SEND_RING_PUT, self.send_put)?;
        tx.write32(NV_VGPU_BAR0_BASE + nv_vgpu_reg::RPC_DOORBELL, 1)?;

        let recv_put = tx.read32(NV_VGPU_BAR0_BASE + nv_vgpu_reg::RECV_RING_PUT)?;
        if recv_put == self.recv_get {
            return Err(VgpuError::InvalidVrpcLength(0));
        }
        let recv_off = (self.recv_get as usize) & 0xff8;
        let reply_lo = tx.read_dword(self.recv_ring_gpa, recv_off)?;
        let reply_hi = tx.read_dword(self.recv_ring_gpa, recv_off + 4)?;
        let reply_gpa = decode_ring_pte(reply_lo, reply_hi);
        self.recv_get = (self.recv_get + 8) & 0xfff;
        tx.write32(NV_VGPU_BAR0_BASE + nv_vgpu_reg::RECV_RING_GET, self.recv_get)?;

        let intr = tx.read32(NV_VGPU_BAR0_BASE + nv_vgpu_reg::VGPU_INTR_STATUS)?;
        if intr != 0 {
            tx.write32(NV_VGPU_BAR0_BASE + nv_vgpu_reg::VGPU_INTR_STATUS, intr)?;
        }

        let mut reply_page = [0u8; 4096];
        tx.read_page(reply_gpa, &mut reply_page)?;
        let reply_hdr = VrpcHeader::decode(&reply_page)?;
        if reply_hdr.sequence != seq {
            return Err(VgpuError::InvalidVrpcSignature(reply_hdr.sequence));
        }

        let resp_len = (reply_hdr.total_length as usize).saturating_sub(VRPC_HEADER_BYTES);
        self.rpc_calls_completed += 1;
        Ok((
            reply_hdr.result,
            reply_page[VRPC_HEADER_BYTES..VRPC_HEADER_BYTES + resp_len].to_vec(),
        ))
    }

    /// Handshake guest VGX / kernel identity with the host (`func_id = 1`).
    pub fn set_guest_system_info<T: GuestVgpuTransport>(
        &mut self,
        tx: &mut T,
        vgx_major: u32,
        vgx_minor: u32,
    ) -> VgpuResult<u32> {
        let mut payload = [0u8; 16];
        payload[0..4].copy_from_slice(&vgx_major.to_le_bytes());
        payload[4..8].copy_from_slice(&vgx_minor.to_le_bytes());
        payload[8..12].copy_from_slice(&0x4156_4f4eu32.to_le_bytes()); // "NOVA"
        let (status, _) = self.call_vrpc(tx, VrpcFuncId::SetGuestSystemInfo, &payload)?;
        if status == 0 {
            self.vgx_initialized = true;
        }
        Ok(status)
    }

    /// Query host-partitioned virtual GPU static geometry (`func_id = 0x32`).
    pub fn get_static_info<T: GuestVgpuTransport>(
        &mut self,
        tx: &mut T,
    ) -> VgpuResult<(u32, u64, u16, u16)> {
        let payload = [0u8; 16];
        let (status, resp) = self.call_vrpc(tx, VrpcFuncId::GetStaticInfo535, &payload)?;
        if resp.len() >= 12 {
            let fb_bytes = u64::from_le_bytes(resp[0..8].try_into().unwrap());
            let dev_id = u16::from_le_bytes(resp[8..10].try_into().unwrap());
            let subsys_id = u16::from_le_bytes(resp[10..12].try_into().unwrap());
            Ok((status, fb_bytes, dev_id, subsys_id))
        } else {
            Ok((status, 0, 0, 0))
        }
    }

    /// Cleanly disable ring descriptors on module unload (`0x8f000 = 0`, `0x8f010 = 0`, `0x8f030 = 0`).
    pub fn shutdown<T: GuestVgpuTransport>(&mut self, tx: &mut T) -> VgpuResult<()> {
        tx.write32(NV_VGPU_BAR0_BASE + nv_vgpu_reg::VGPU_INTR_ENABLE, 0)?;
        tx.write32(NV_VGPU_BAR0_BASE + nv_vgpu_reg::SEND_RING_LO, 0)?;
        tx.write32(NV_VGPU_BAR0_BASE + nv_vgpu_reg::RECV_RING_LO, 0)?;
        tx.write32(NV_VGPU_BAR0_BASE + nv_vgpu_reg::EVENT_RING_LO, 0)?;
        Ok(())
    }
}

/// Complete clean-room Guest <-> Host KVM loopback transport connecting
/// [`NovaGuestDriver`] directly to [`PascalVgpuInstance`] across emulated guest RAM pages
/// and trapped `0x8f000` BAR0 MMIO accesses.
pub struct HostLoopbackTransport {
    pub host: PascalVgpuInstance,
    pub pages: BTreeMap<u64, [u8; 4096]>,
    next_gpa: u64,
}

impl HostLoopbackTransport {
    pub fn new(host: PascalVgpuInstance) -> Self {
        Self {
            host,
            pages: BTreeMap::new(),
            next_gpa: 0x0010_0000,
        }
    }

    fn service_pending_send_ring(&mut self) -> VgpuResult<()> {
        let send_gpa = decode_ring_pte(self.host.bar0.send_ring.gpa_lo, self.host.bar0.send_ring.gpa_hi);
        let recv_gpa = decode_ring_pte(self.host.bar0.recv_ring.gpa_lo, self.host.bar0.recv_ring.gpa_hi);
        if send_gpa == 0 || recv_gpa == 0 {
            return Ok(());
        }

        while self.host.bar0.send_ring.get != self.host.bar0.send_ring.put {
            let slot_off = (self.host.bar0.send_ring.get as usize) & 0xff8;
            let lo = self.read_dword(send_gpa, slot_off)?;
            let hi = self.read_dword(send_gpa, slot_off + 4)?;
            let msg_gpa = decode_ring_pte(lo, hi);
            let gfn = msg_gpa >> 12;
            self.host.vmmu.pin_gfn(gfn, gfn + 0x4_0000);

            let mut msg_page = [0u8; 4096];
            self.read_page(msg_gpa, &mut msg_page)?;
            let _status = self.host.vrpc.handle_message(&mut msg_page)?;
            self.write_page(msg_gpa, &msg_page)?;

            let recv_off = (self.host.bar0.recv_ring.put as usize) & 0xff8;
            self.write_dword(recv_gpa, recv_off, lo)?;
            self.write_dword(recv_gpa, recv_off + 4, hi)?;
            self.host.bar0.recv_ring.put = (self.host.bar0.recv_ring.put + 8) & 0xfff;
            self.host.bar0.send_ring.get = (self.host.bar0.send_ring.get + 8) & 0xfff;
            self.host.bar0.vgpu_intr_status |= 0x40;
        }
        Ok(())
    }
}

impl GuestVgpuTransport for HostLoopbackTransport {
    fn read32(&mut self, offset: u64) -> VgpuResult<u32> {
        self.host
            .handle_reg_access(VgpuEmulSpace::Mmio, offset, 4, false, 0)
    }

    fn write32(&mut self, offset: u64, value: u32) -> VgpuResult<()> {
        self.host
            .handle_reg_access(VgpuEmulSpace::Mmio, offset, 4, true, value)?;
        if offset == NV_VGPU_BAR0_BASE + nv_vgpu_reg::RPC_DOORBELL
            || offset == NV_VGPU_BAR0_BASE + nv_vgpu_reg::SEND_RING_PUT
        {
            self.service_pending_send_ring()?;
        }
        Ok(())
    }

    fn alloc_zeroed_page(&mut self) -> VgpuResult<u64> {
        let gpa = self.next_gpa;
        self.next_gpa += 4096;
        let gfn = gpa >> 12;
        self.host.vmmu.pin_gfn(gfn, gfn + 0x4_0000);
        self.pages.insert(gpa, [0u8; 4096]);
        Ok(gpa)
    }

    fn read_page(&self, gpa: u64, out: &mut [u8; 4096]) -> VgpuResult<()> {
        let page = self
            .pages
            .get(&gpa)
            .ok_or(VgpuError::InvalidGuestPfn(gpa >> 12))?;
        out.copy_from_slice(page);
        Ok(())
    }

    fn write_page(&mut self, gpa: u64, data: &[u8; 4096]) -> VgpuResult<()> {
        let page = self
            .pages
            .get_mut(&gpa)
            .ok_or(VgpuError::InvalidGuestPfn(gpa >> 12))?;
        page.copy_from_slice(data);
        Ok(())
    }

    fn write_dword(&mut self, gpa: u64, byte_off: usize, value: u32) -> VgpuResult<()> {
        let page = self
            .pages
            .get_mut(&gpa)
            .ok_or(VgpuError::InvalidGuestPfn(gpa >> 12))?;
        if byte_off + 4 > 4096 {
            return Err(VgpuError::BufferTooSmall);
        }
        page[byte_off..byte_off + 4].copy_from_slice(&value.to_le_bytes());
        Ok(())
    }

    fn read_dword(&self, gpa: u64, byte_off: usize) -> VgpuResult<u32> {
        let page = self
            .pages
            .get(&gpa)
            .ok_or(VgpuError::InvalidGuestPfn(gpa >> 12))?;
        if byte_off + 4 > 4096 {
            return Err(VgpuError::BufferTooSmall);
        }
        Ok(u32::from_le_bytes(
            page[byte_off..byte_off + 4].try_into().unwrap(),
        ))
    }
}
