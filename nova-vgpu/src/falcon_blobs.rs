//! Clean-room GP104 Falcon firmware header, LS signature footer, and packed
//! `GR` Netlist TOC validators used by `nova-vgpu` when inspecting user-extracted
//! `535.309.01` Pascal Falcon blobs.
//!
//! Host software NEVER computes or bypasses Falcon signatures; it strictly
//! validates the structure and silicon `LsFalconId` of NVIDIA's signed blobs.

use crate::{VgpuError, VgpuResult};

/// Light-Secure signature size verified across GP104 `PMU`, `SEC2`, `NVENC`,
/// `NVDEC`, `FECS`, and `GPCCS`.
pub const LS_SIGNATURE_BYTES: usize = 192;

/// Trailing 4x `u32` XOR mask protecting the LSF metadata footer in
/// `nv-kernel.o_binary` (`[0xb0..0xc0]`).
pub const LSF_FOOTER_XOR_MASK: [u32; 4] = [0xe931_c2b6, 0xd777_b203, 0x69a0_320e, 0x6280_4e8f];

fn read_u32_le(buf: &[u8], off: usize) -> VgpuResult<u32> {
    let bytes: [u8; 4] = buf
        .get(off..off + 4)
        .ok_or(VgpuError::BufferTooSmall)?
        .try_into()
        .map_err(|_| VgpuError::BufferTooSmall)?;
    Ok(u32::from_le_bytes(bytes))
}

/// Light-Secure (LS) Falcon engine IDs managed inside `WPR1`.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
#[repr(u32)]
pub enum LsFalconId {
    Pmu = 0,
    Fecs = 2,
    Gpccs = 3,
    Nvenc = 4,
    Nvdec = 5,
    Sec2 = 7,
}

impl LsFalconId {
    pub const fn from_u32(val: u32) -> Option<Self> {
        match val {
            0 => Some(Self::Pmu),
            2 => Some(Self::Fecs),
            3 => Some(Self::Gpccs),
            4 => Some(Self::Nvenc),
            5 => Some(Self::Nvdec),
            7 => Some(Self::Sec2),
            _ => None,
        }
    }
}

/// 24-byte Falcon bootloader descriptor (`pmu_bl`, `sec2_bl`, `fecs_bl_desc`, `gpccs_bl_desc`).
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct FlcnBlDesc {
    pub start_tag: u32,
    pub boot_vec: u32,
    pub code_size: u32,
    pub data_off: u32,
    pub data_size: u32,
}

impl FlcnBlDesc {
    pub fn parse(data: &[u8]) -> VgpuResult<Self> {
        if data.len() != 24 {
            return Err(VgpuError::MalformedBlob("FlcnBlDesc must be 24 bytes"));
        }
        let start_tag = read_u32_le(data, 0)?;
        Ok(Self {
            start_tag,
            boot_vec: start_tag
                .checked_shl(8)
                .ok_or(VgpuError::MalformedBlob("start_tag shift overflow"))?,
            code_size: read_u32_le(data, 12)?,
            data_off: read_u32_le(data, 16)?,
            data_size: read_u32_le(data, 20)?,
        })
    }
}

/// Decoded RTOS Falcon header (`PMU` / `SEC2` descriptor).
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct RtosDesc {
    pub desc_size: u32,
    pub stored_size: u32,
    pub flags: u32,
    pub changelist: u32,
    pub app_start_addr: u32,
    pub app_entry: u32,
    pub code_offset: u32,
    pub data_offset_span: u32,
    pub total_expected_image_bytes: u32,
    pub num_overlays: u32,
}

impl RtosDesc {
    pub fn parse(data: &[u8]) -> VgpuResult<Self> {
        if data.len() < 0x90 {
            return Err(VgpuError::MalformedBlob("RtosDesc too small"));
        }
        let flags = read_u32_le(data, 8)?;
        let base = if flags <= 2 { 0x54 } else { 0x58 };
        let app_start_addr = read_u32_le(data, base + 4)?;
        let app_entry = read_u32_le(data, base + 8)?;
        let code_offset = read_u32_le(data, base + 12)?;
        let data_offset_span = read_u32_le(data, base + 16)?;
        Ok(Self {
            desc_size: read_u32_le(data, 0)?,
            stored_size: read_u32_le(data, 4)?,
            flags,
            changelist: read_u32_le(data, 12)?,
            app_start_addr,
            app_entry,
            code_offset,
            data_offset_span,
            total_expected_image_bytes: code_offset
                .checked_add(data_offset_span)
                .ok_or(VgpuError::MalformedBlob("RtosDesc size overflow"))?,
            num_overlays: read_u32_le(data, 0x88)?,
        })
    }
}

/// Decoded metadata from a `192`-byte LS Falcon signature wrapper.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct LsfSigInfo {
    pub falcon_id: LsFalconId,
}

impl LsfSigInfo {
    /// Decode `192`-byte `LSF_UCODE_DESC` wrapper by validating `[160..176]` zero
    /// padding and unmasking `[176..192]` with `LSF_FOOTER_XOR_MASK`.
    pub fn parse(data: &[u8]) -> VgpuResult<Self> {
        if data.len() != LS_SIGNATURE_BYTES {
            return Err(VgpuError::MalformedBlob("LS sig must be 192 bytes"));
        }
        for i in 0..4 {
            if read_u32_le(data, 160 + i * 4)? != 0 {
                return Err(VgpuError::MalformedBlob("LS sig non-zero padding"));
            }
        }
        let raw_fid = read_u32_le(data, 176)? ^ LSF_FOOTER_XOR_MASK[0];
        let chk1 = read_u32_le(data, 180)? ^ LSF_FOOTER_XOR_MASK[1];
        if chk1 != 0 {
            return Err(VgpuError::MalformedBlob("LS sig checksum word mismatch"));
        }
        let falcon_id = LsFalconId::from_u32(raw_fid)
            .ok_or(VgpuError::MalformedBlob("Unknown LS Falcon ID"))?;
        Ok(Self { falcon_id })
    }
}

/// Packed `GR` Netlist TOC section record (`12` bytes each inside `gp104_gr_netlist.bin`).
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct GrNetlistSection {
    pub section_type: u32,
    pub size: u32,
    pub offset: u32,
}

/// Parse packed `GR` Netlist header & section directory (`magic == 0`, e.g., `32` entries).
pub fn parse_gr_netlist_toc(data: &[u8], out: &mut [GrNetlistSection]) -> VgpuResult<usize> {
    if data.len() < 8 {
        return Err(VgpuError::MalformedBlob("Netlist too small"));
    }
    let magic = read_u32_le(data, 0)?;
    let count = read_u32_le(data, 4)? as usize;
    if magic != 0 || count == 0 || count > 64 {
        return Err(VgpuError::MalformedBlob("Invalid Netlist TOC header"));
    }
    if 8 + count * 12 > data.len() {
        return Err(VgpuError::MalformedBlob("Netlist TOC truncated"));
    }
    if out.len() < count {
        return Err(VgpuError::BufferTooSmall);
    }
    for (idx, dst) in out[..count].iter_mut().enumerate() {
        let base = 8 + idx * 12;
        let sec_type = read_u32_le(data, base)?;
        let size = read_u32_le(data, base + 4)?;
        let offset = read_u32_le(data, base + 8)?;
        if (offset as usize).saturating_add(size as usize) > data.len() {
            return Err(VgpuError::MalformedBlob("Netlist section out of bounds"));
        }
        *dst = GrNetlistSection {
            section_type: sec_type,
            size,
            offset,
        };
    }
    Ok(count)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decode_ls_sig_footer_roundtrip() {
        let mut sig = [0u8; LS_SIGNATURE_BYTES];
        let w0 = (LsFalconId::Sec2 as u32) ^ LSF_FOOTER_XOR_MASK[0];
        let w1 = 0u32 ^ LSF_FOOTER_XOR_MASK[1];
        sig[176..180].copy_from_slice(&w0.to_le_bytes());
        sig[180..184].copy_from_slice(&w1.to_le_bytes());
        let info = LsfSigInfo::parse(&sig).unwrap();
        assert_eq!(info.falcon_id, LsFalconId::Sec2);
    }

    #[test]
    fn decode_gr_netlist_toc_valid() {
        let mut buf = vec![0u8; 64];
        buf[0..4].copy_from_slice(&0u32.to_le_bytes());
        buf[4..8].copy_from_slice(&2u32.to_le_bytes());
        buf[8..12].copy_from_slice(&1u32.to_le_bytes());
        buf[12..16].copy_from_slice(&16u32.to_le_bytes());
        buf[16..20].copy_from_slice(&32u32.to_le_bytes());
        buf[20..24].copy_from_slice(&0u32.to_le_bytes());
        buf[24..28].copy_from_slice(&8u32.to_le_bytes());
        buf[28..32].copy_from_slice(&48u32.to_le_bytes());
        let mut out = [GrNetlistSection { section_type: 0, size: 0, offset: 0 }; 8];
        let n = parse_gr_netlist_toc(&buf, &mut out).unwrap();
        assert_eq!(n, 2);
        assert_eq!(out[0].size, 16);
        assert_eq!(out[1].offset, 48);
    }
}
