//! Virtual PCI Configuration Space (`256`-byte Type-0 header + MSI/PM caps)
//! for `GP104GL` vGPUs (`_nv004114vgpu` / `_nv004120vgpu` clean-room reimplementation).

use crate::config::VgpuProfile;
use crate::{VgpuError, VgpuResult};

pub const PCI_CONFIG_SPACE_SIZE: usize = 256;

/// Emulated 256-byte PCI configuration space for a Pascal `GP104` vGPU instance.
#[derive(Debug, Clone)]
pub struct VirtualPciConfig {
    pub space: [u8; PCI_CONFIG_SPACE_SIZE],
    pub bar0_size: u64,
    pub bar1_size: u64,
    pub bar3_size: u64,
}

impl VirtualPciConfig {
    /// Construct a GP104 vGPU PCI config header from a parsed `VgpuProfile`.
    ///
    /// Matches `_nv004120vgpu`:
    /// - Vendor/Device: `0x10de` / `0x1bb3` (`GP104GL` Tesla P4)
    /// - Class Code: `0x030200` (3D controller) for Q/A/B compute/vGPU profiles, or `0x030000` (VGA)
    /// - Subsystem Vendor/Device: `0x10de` / `profile.dev_id.subsystem_id` (e.g., `0x1204` for `P4-1Q`)
    /// - BAR0 = 16 MiB (32-bit non-prefetchable MMIO), BAR1 = `bar1_length_mb` MiB (64-bit prefetchable),
    ///   BAR3 = 32 MiB (64-bit prefetchable).
    pub fn from_profile(profile: &VgpuProfile) -> Self {
        let mut space = [0u8; PCI_CONFIG_SPACE_SIZE];
        put_u16(&mut space, 0x00, profile.dev_id.vendor_id);
        put_u16(&mut space, 0x02, profile.dev_id.device_id);
        // Command = Memory Space | Bus Master (0x0006), Status = Cap List (0x0010)
        put_u16(&mut space, 0x04, 0x0006);
        put_u16(&mut space, 0x06, 0x0010);
        // Revision ID = 0xa1 (GP104 A1), Class = 0x030200 (3D controller)
        space[0x08] = 0xa1;
        space[0x09] = 0x00;
        space[0x0a] = 0x02;
        space[0x0b] = 0x03;

        // BAR0 (32-bit MMIO), BAR1+BAR2 (64-bit prefetchable), BAR3+BAR4 (64-bit prefetchable)
        put_u32(&mut space, 0x10, 0x0000_0000);
        put_u32(&mut space, 0x14, 0x0000_000c);
        put_u32(&mut space, 0x18, 0x0000_0000);
        put_u32(&mut space, 0x1c, 0x0000_000c);
        put_u32(&mut space, 0x20, 0x0000_0000);
        put_u32(&mut space, 0x24, 0x0000_0000);

        put_u16(&mut space, 0x2c, profile.dev_id.subsystem_vendor_id);
        put_u16(&mut space, 0x2e, profile.dev_id.subsystem_id);

        // Capability pointer -> 0x60 (PM), 0x68 (MSI), 0x78 (PCIe)
        space[0x34] = 0x60;
        space[0x3d] = 0x01; // INTA#

        // PM Capability at 0x60 (ID=0x01, next=0x68)
        space[0x60] = 0x01;
        space[0x61] = 0x68;
        put_u16(&mut space, 0x62, 0x0003);

        // MSI Capability at 0x68 (ID=0x05, next=0x00, 64-bit capable = 0x0080)
        space[0x68] = 0x05;
        space[0x69] = 0x00;
        put_u16(&mut space, 0x6a, 0x0080);

        let bar1_bytes = profile.bar1_length_mb.saturating_mul(1024 * 1024);
        Self {
            space,
            bar0_size: 16 * 1024 * 1024,
            bar1_size: if bar1_bytes == 0 {
                256 * 1024 * 1024
            } else {
                bar1_bytes
            },
            bar3_size: 32 * 1024 * 1024,
        }
    }

    /// Read `width` bytes (1, 2, or 4) from virtual PCI config space.
    pub fn read(&self, offset: usize, width: usize) -> VgpuResult<u32> {
        validate_access(offset, width)?;
        let mut buf = [0u8; 4];
        buf[..width].copy_from_slice(&self.space[offset..offset + width]);
        Ok(u32::from_le_bytes(buf))
    }

    /// Handle guest write to virtual PCI config space matching `_nv004114vgpu`.
    ///
    /// Preserves read-only Vendor/Device/Subsystem IDs while allowing BAR sizing probes,
    /// command register, interrupt line, and MSI address/data (`0x68..0x74`).
    pub fn write(&mut self, offset: usize, width: usize, value: u32) -> VgpuResult<()> {
        validate_access(offset, width)?;
        let raw = value.to_le_bytes();
        match offset {
            // Read-only identity registers
            0x00..=0x03 | 0x08..=0x0b | 0x2c..=0x2f => {}
            // BAR0 (16 MiB mask on 0xffff_ffff probe)
            0x10 if width == 4 && value == u32::MAX => {
                let mask = !((self.bar0_size as u32) - 1);
                put_u32(&mut self.space, 0x10, mask);
            }
            // BAR1 low/high (64-bit prefetchable)
            0x14 if width == 4 && value == u32::MAX => {
                let mask = (!((self.bar1_size as u32).wrapping_sub(1))) | 0x0c;
                put_u32(&mut self.space, 0x14, mask);
            }
            0x18 if width == 4 && value == u32::MAX => {
                let hi_mask = (!(self.bar1_size - 1) >> 32) as u32;
                put_u32(&mut self.space, 0x18, hi_mask);
            }
            // BAR3 low/high (64-bit prefetchable)
            0x1c if width == 4 && value == u32::MAX => {
                let mask = (!((self.bar3_size as u32).wrapping_sub(1))) | 0x0c;
                put_u32(&mut self.space, 0x1c, mask);
            }
            0x20 if width == 4 && value == u32::MAX => {
                let hi_mask = (!(self.bar3_size - 1) >> 32) as u32;
                put_u32(&mut self.space, 0x20, hi_mask);
            }
            // Writable command/status, BARs, interrupt line, MSI registers (0x68..0x74)
            0x04..=0x07 | 0x10..=0x27 | 0x3c..=0x3d | 0x6a..=0x77 => {
                self.space[offset..offset + width].copy_from_slice(&raw[..width]);
            }
            _ => {}
        }
        Ok(())
    }

    /// Return `true` when guest enabled MSI (`0x6a` bit 0).
    pub fn msi_enabled(&self) -> bool {
        (u16::from_le_bytes([self.space[0x6a], self.space[0x6b]]) & 1) != 0
    }
}

fn validate_access(offset: usize, width: usize) -> VgpuResult<()> {
    if !matches!(width, 1 | 2 | 4) || offset + width > PCI_CONFIG_SPACE_SIZE {
        return Err(VgpuError::InvalidRegisterAccess(offset as u64, width as u32));
    }
    Ok(())
}

fn put_u16(buf: &mut [u8], off: usize, v: u16) {
    buf[off..off + 2].copy_from_slice(&v.to_le_bytes());
}

fn put_u32(buf: &mut [u8], off: usize, v: u32) {
    buf[off..off + 4].copy_from_slice(&v.to_le_bytes());
}
