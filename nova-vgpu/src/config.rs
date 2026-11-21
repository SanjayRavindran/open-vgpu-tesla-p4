//! Clean-room Rust parser for NVIDIA `vgpuConfig.xml` (`libnvidia-vgxcfg.so` reimplementation)
//! and `NVA081_CTRL_VGPU_CONFIG_SET_INFO_PARAMS` (`0x1368` bytes) builder.

use crate::{VgpuError, VgpuResult};

/// Maximum length of profile name / class strings in `type_info_t` and `NVA081`.
pub const VGPU_STRING_BUFFER_SIZE: usize = 32;
/// Length of the binary RSA config signature digest per `<supportedVgpu>` entry.
pub const VGPU_DIGEST_BYTES: usize = 128;

/// Global settings parsed from `<globalSettings>` in `vgpuConfig.xml`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GlobalVgpuSettings {
    pub heterogeneous_multi_vgpu: bool,
    pub plugin_so_name: String,
}

/// PCI device ID tuple from `<devId>` elements.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct PciDevId {
    pub vendor_id: u16,
    pub device_id: u16,
    pub subsystem_vendor_id: u16,
    pub subsystem_id: u16,
}

/// Complete vGPU profile definition parsed from `<vgpuType>` in `vgpuConfig.xml`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VgpuProfile {
    pub id: u32,
    pub name: String,
    pub class_name: String,
    pub dev_id: PciDevId,
    pub profile_size: u64,
    pub framebuffer: u64,
    pub fb_reservation: u64,
    pub gsp_heap_size: u64,
    pub num_heads: u32,
    pub display_width: u32,
    pub display_height: u32,
    pub max_pixels: u32,
    pub mappable_video_size: u64,
    pub frl_config: u32,
    pub frame_rate_limiter: bool,
    pub cuda_enabled: bool,
    pub ecc_supported: bool,
    pub multi_vgpu_supported: bool,
    pub encoder_capacity: u32,
    pub bar1_length_mb: u64,
    pub license: String,
    pub gpu_instance_size: u32,
    pub gpu_direct_supported: u32,
    pub nvlink_p2p_supported: u32,
    pub multi_vgpu_exclusive: u32,
    pub extra_parameters: String,
}

/// Per-physical-GPU supported vGPU entry from `<pgpu>/<supportedVgpu>`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SupportedVgpuEntry {
    pub vgpu_id: u32,
    pub max_vgpus: u32,
    pub digest: [u8; VGPU_DIGEST_BYTES],
}

/// Physical GPU entry from `<pgpu>` in `vgpuConfig.xml`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PhysicalGpuConfig {
    pub dev_id: PciDevId,
    pub fractional_multi_vgpu: bool,
    pub supported: Vec<SupportedVgpuEntry>,
}

/// Parsed in-memory representation of `vgpuConfig.xml`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VgpuConfigStore {
    pub global: GlobalVgpuSettings,
    pub pgpus: Vec<PhysicalGpuConfig>,
    pub profiles: Vec<VgpuProfile>,
}

impl VgpuConfigStore {
    /// Parse a `vgpuConfig.xml` document cleanly without external XML libraries.
    pub fn parse_xml(xml: &str) -> VgpuResult<Self> {
        let mut global = GlobalVgpuSettings {
            heterogeneous_multi_vgpu: false,
            plugin_so_name: String::from("libnvidia-vgpu"),
        };
        if let Some(gs) = extract_block(xml, "<globalSettings", "</globalSettings>") {
            if let Some(v) = extract_tag_text(gs, "heterogeneousMultiVgpu") {
                global.heterogeneous_multi_vgpu = parse_bool_str(v)?;
            }
            if let Some(v) = extract_tag_text(gs, "pluginSoName") {
                global.plugin_so_name = v.to_string();
            }
        }

        let mut pgpus = Vec::new();
        let mut cursor = 0usize;
        while let Some((start, end, body)) = find_next_block(&xml[cursor..], "<pgpu", "</pgpu>") {
            let pgpu = parse_pgpu_block(body)?;
            pgpus.push(pgpu);
            cursor += end;
            let _ = start;
        }

        let mut profiles = Vec::new();
        let mut pcursor = 0usize;
        while let Some((_start, end, body)) =
            find_next_block(&xml[pcursor..], "<vgpuType", "</vgpuType>")
        {
            profiles.push(parse_vgpu_type_block(body)?);
            pcursor += end;
        }

        Ok(Self {
            global,
            pgpus,
            profiles,
        })
    }

    /// Find a physical GPU entry matching PCI vendor/device ID (e.g., `0x10de`, `0x1bb3` for Tesla P4).
    pub fn find_pgpu(&self, vendor_id: u16, device_id: u16) -> Option<&PhysicalGpuConfig> {
        self.pgpus
            .iter()
            .find(|p| p.dev_id.vendor_id == vendor_id && p.dev_id.device_id == device_id)
    }

    /// Find a physical GPU matching full 4-tuple PCI ID, supporting `subsystemId == 0` wildcard
    /// matching as observed in `nvidia-vgpud` (`0x404440.c`).
    pub fn find_pgpu_exact(
        &self,
        vendor_id: u16,
        device_id: u16,
        subsystem_vendor_id: u16,
        subsystem_id: u16,
    ) -> Option<&PhysicalGpuConfig> {
        self.pgpus.iter().find(|p| {
            p.dev_id.vendor_id == vendor_id
                && p.dev_id.device_id == device_id
                && p.dev_id.subsystem_vendor_id == subsystem_vendor_id
                && (p.dev_id.subsystem_id == 0 || p.dev_id.subsystem_id == subsystem_id)
        })
    }

    /// Look up a `VgpuProfile` by numeric `vgpu_id`.
    pub fn profile_by_id(&self, id: u32) -> Option<&VgpuProfile> {
        self.profiles.iter().find(|p| p.id == id)
    }

    /// Return all `(SupportedVgpuEntry, &VgpuProfile)` pairs supported on `(vendor_id, device_id)`.
    pub fn supported_profiles_for_gpu(
        &self,
        vendor_id: u16,
        device_id: u16,
    ) -> VgpuResult<Vec<(&SupportedVgpuEntry, &VgpuProfile)>> {
        let pgpu = self
            .find_pgpu(vendor_id, device_id)
            .ok_or(VgpuError::UnsupportedGpu(vendor_id, device_id))?;
        let mut out = Vec::with_capacity(pgpu.supported.len());
        for sup in &pgpu.supported {
            let prof = self
                .profile_by_id(sup.vgpu_id)
                .ok_or(VgpuError::ProfileNotFound(sup.vgpu_id))?;
            out.push((sup, prof));
        }
        Ok(out)
    }
}

/// Expand semicolon-separated short GRID license tokens into the full feature strings
/// expected by `nvidia.ko` / `nvidia-vgpu-mgr` (exact logic from `nvidia-vgpud` `0x4039f0.c`).
pub fn expand_license_string(raw: &str) -> VgpuResult<String> {
    let mut parts = Vec::new();
    for tok in raw.split(';').map(str::trim).filter(|t| !t.is_empty()) {
        let mapped = match tok {
            "VPC" => "GRID-Virtual-PC,2.0",
            "VWS" => "GRID-Virtual-WS,2.0",
            "VWSExt" => "GRID-Virtual-WS-Ext,2.0",
            "VApps" => "GRID-Virtual-Apps,3.0",
            "QVDWS" => "Quadro-Virtual-DWS,5.0",
            "VGaming" => "GRID-vGaming,8.0",
            "VCS" => "NVIDIA-vComputeServer,9.0",
            other if other.contains(',') => other,
            _ => return Err(VgpuError::MalformedXml("unknown GRID license token")),
        };
        parts.push(mapped);
    }
    if parts.is_empty() {
        return Err(VgpuError::MalformedXml("empty GRID license string"));
    }
    Ok(parts.join(";"))
}

/// Wire-accurate layout of `NVA081_CTRL_VGPU_CONFIG_SET_INFO_PARAMS` (`0x1368` = `4968` bytes)
/// observed in `nvidia-vgpud` (`0x402ca0.c`) for RM control `0xa0810101`.
#[repr(C)]
#[derive(Clone)]
pub struct Nva081SetInfoParams {
    pub raw: [u8; 0x1368],
}

impl Nva081SetInfoParams {
    /// Pack a `VgpuProfile` and `SupportedVgpuEntry` into the `0x1368`-byte RM struct.
    pub fn from_profile(
        profile: &VgpuProfile,
        supported: &SupportedVgpuEntry,
        is_last: bool,
        first_flag: bool,
    ) -> Self {
        Self::from_profile_for_pgpu(
            profile,
            supported,
            profile.dev_id.device_id,
            is_last,
            first_flag,
        )
    }

    /// Pack a `VgpuProfile` and `SupportedVgpuEntry` for a specific physical GPU device ID.
    pub fn from_profile_for_pgpu(
        profile: &VgpuProfile,
        supported: &SupportedVgpuEntry,
        pgpu_device_id: u16,
        is_last: bool,
        first_flag: bool,
    ) -> Self {
        let mut raw = [0u8; 0x1368];
        // Offsets relative to local_13a8 in nvidia-vgpud (0x402ca0.c):
        // +0x000: u8 first_flag (local_1a5a)
        // +0x008: u32 vgpu_type_id (local_13a0)
        // +0x00c: char[32] name (local_139c)
        // +0x02c: char[32] class (local_137c)
        // +0x04c: [u8; 128] digest (local_135c)
        // +0x0cc: char[128] expanded license (local_12dc)
        // +0x14c: u32 max_instance (local_125c)
        // +0x150: u32 num_heads (local_1258)
        // +0x154: u32 display_width (local_1254)
        // +0x158: u32 display_height (local_1250)
        // +0x15c: u32 max_pixels (local_124c)
        // +0x160: u32 frl_config (local_1248)
        // +0x164: u32 cuda_enabled (local_1244)
        // +0x168: u32 ecc_supported (local_1240)
        // +0x16c: u32 gpu_instance_size (local_123c)
        // +0x170: u32 multi_vgpu_supported (local_1238)
        // +0x178: u64 ((device_id << 16) | subsystem_id) (local_1230)
        // +0x180: u64 pgpu_device_id (local_1228)
        // +0x188: u64 profile_size (local_1220)
        // +0x190: u64 framebuffer (local_1218)
        // +0x198: u64 gsp_heap_size (local_1210)
        // +0x1a0: u64 fb_reservation (local_1208)
        // +0x1a8: u64 mappable_video_size (local_1200)
        // +0x1b0: u32 encoder_capacity (local_11f8)
        // +0x1b8: u64 bar1_length_mb (local_11f0)
        // +0x1c0: u32 frame_rate_limiter (local_11e8)
        // +0x344: char[4096] extra_parameters (local_1064)
        // +0x1348: u32 gpu_direct_supported (local_60)
        // +0x134c: u32 nvlink_p2p_supported (local_5c)
        // +0x1350: u32 multi_vgpu_exclusive (local_58)
        // +0x1360: u32 end_state (1 = normal, 2 = last) (local_48)
        let expanded_license =
            expand_license_string(&profile.license).unwrap_or_else(|_| profile.license.clone());
        raw[0] = u8::from(first_flag);
        put_u32(&mut raw, 0x008, profile.id);
        put_cstr(&mut raw, 0x00c, 32, &profile.name);
        put_cstr(&mut raw, 0x02c, 32, &profile.class_name);
        raw[0x04c..0x04c + VGPU_DIGEST_BYTES].copy_from_slice(&supported.digest);
        put_cstr(&mut raw, 0x0cc, 128, &expanded_license);
        put_u32(&mut raw, 0x14c, supported.max_vgpus);
        put_u32(&mut raw, 0x150, profile.num_heads);
        put_u32(&mut raw, 0x154, profile.display_width);
        put_u32(&mut raw, 0x158, profile.display_height);
        put_u32(&mut raw, 0x15c, profile.max_pixels);
        put_u32(&mut raw, 0x160, profile.frl_config);
        put_u32(&mut raw, 0x164, u32::from(profile.cuda_enabled));
        put_u32(&mut raw, 0x168, u32::from(profile.ecc_supported));
        put_u32(&mut raw, 0x16c, profile.gpu_instance_size);
        put_u32(&mut raw, 0x170, u32::from(profile.multi_vgpu_supported));
        let packed_dev =
            ((profile.dev_id.device_id as u64) << 16) | (profile.dev_id.subsystem_id as u64);
        put_u64(&mut raw, 0x178, packed_dev);
        put_u64(&mut raw, 0x180, pgpu_device_id as u64);
        put_u64(&mut raw, 0x188, profile.profile_size);
        put_u64(&mut raw, 0x190, profile.framebuffer);
        put_u64(&mut raw, 0x198, profile.gsp_heap_size);
        put_u64(&mut raw, 0x1a0, profile.fb_reservation);
        put_u64(&mut raw, 0x1a8, profile.mappable_video_size);
        put_u32(&mut raw, 0x1b0, profile.encoder_capacity);
        put_u64(&mut raw, 0x1b8, profile.bar1_length_mb);
        put_u32(&mut raw, 0x1c0, u32::from(profile.frame_rate_limiter));
        if !profile.extra_parameters.is_empty() {
            put_cstr(&mut raw, 0x344, 4096, &profile.extra_parameters);
        }
        put_u32(&mut raw, 0x1348, profile.gpu_direct_supported);
        put_u32(&mut raw, 0x134c, profile.nvlink_p2p_supported);
        put_u32(&mut raw, 0x1350, profile.multi_vgpu_exclusive);
        put_u32(&mut raw, 0x1360, if is_last { 2 } else { 1 });
        Self { raw }
    }
}

fn put_u32(buf: &mut [u8], off: usize, val: u32) {
    buf[off..off + 4].copy_from_slice(&val.to_le_bytes());
}

fn put_u64(buf: &mut [u8], off: usize, val: u64) {
    buf[off..off + 8].copy_from_slice(&val.to_le_bytes());
}

fn put_cstr(buf: &mut [u8], off: usize, cap: usize, s: &str) {
    let bytes = s.as_bytes();
    let n = bytes.len().min(cap.saturating_sub(1));
    buf[off..off + n].copy_from_slice(&bytes[..n]);
}

fn parse_pgpu_block(block: &str) -> VgpuResult<PhysicalGpuConfig> {
    let open_tag = block.split('>').next().unwrap_or("");
    let fractional_multi_vgpu = extract_attr(open_tag, "fractionalMultiVgpu")
        .map(parse_bool_str)
        .transpose()?
        .unwrap_or(false);
    let dev_id = parse_dev_id(block)?;
    let mut supported = Vec::new();
    let mut pos = 0usize;
    while let Some((_s, end, sbody)) =
        find_next_block(&block[pos..], "<supportedVgpu", "</supportedVgpu>")
    {
        let sopen = sbody.split('>').next().unwrap_or("");
        let vgpu_id = parse_u32_str(
            extract_attr(sopen, "vgpuId").ok_or(VgpuError::MalformedXml("missing vgpuId"))?,
        )?;
        let max_vgpus = parse_u32_str(
            extract_tag_text(sbody, "maxVgpus")
                .ok_or(VgpuError::MalformedXml("missing maxVgpus"))?,
        )?;
        let digest_hex = extract_tag_text(sbody, "digest")
            .ok_or(VgpuError::MalformedXml("missing digest"))?;
        let digest = decode_hex_128(digest_hex)?;
        supported.push(SupportedVgpuEntry {
            vgpu_id,
            max_vgpus,
            digest,
        });
        pos += end;
    }
    Ok(PhysicalGpuConfig {
        dev_id,
        fractional_multi_vgpu,
        supported,
    })
}

fn parse_vgpu_type_block(block: &str) -> VgpuResult<VgpuProfile> {
    let open_tag = block.split('>').next().unwrap_or("");
    let id =
        parse_u32_str(extract_attr(open_tag, "id").ok_or(VgpuError::MalformedXml("missing id"))?)?;
    let name = extract_attr(open_tag, "name")
        .ok_or(VgpuError::MalformedXml("missing name"))?
        .to_string();
    let class_name = extract_attr(open_tag, "class").unwrap_or("NVS").to_string();
    let dev_id = parse_dev_id(block)?;

    let (display_width, display_height) = if let Some(disp) = find_tag_open(block, "<display") {
        let w = extract_attr(disp, "width").map(parse_u32_str).transpose()?.unwrap_or(0);
        let h = extract_attr(disp, "height").map(parse_u32_str).transpose()?.unwrap_or(0);
        (w, h)
    } else {
        (0, 0)
    };

    Ok(VgpuProfile {
        id,
        name,
        class_name,
        dev_id,
        profile_size: tag_u64(block, "profileSize")?,
        framebuffer: tag_u64(block, "framebuffer")?,
        fb_reservation: tag_u64(block, "fbReservation")?,
        gsp_heap_size: extract_tag_text(block, "gspHeapSize")
            .map(parse_u64_str)
            .transpose()?
            .unwrap_or(0),
        num_heads: tag_u32(block, "numHeads").unwrap_or(1),
        display_width,
        display_height,
        max_pixels: tag_u32(block, "maxPixels").unwrap_or(0),
        mappable_video_size: tag_u64(block, "mappableVideoSize").unwrap_or(0),
        frl_config: tag_u32(block, "frlConfig").unwrap_or(0),
        frame_rate_limiter: extract_tag_text(block, "frame_rate_limiter")
            .map(parse_bool_str)
            .transpose()?
            .unwrap_or(false),
        cuda_enabled: extract_tag_text(block, "cudaEnabled")
            .map(parse_bool_str)
            .transpose()?
            .unwrap_or(false),
        ecc_supported: extract_tag_text(block, "eccSupported")
            .map(parse_bool_str)
            .transpose()?
            .unwrap_or(false),
        multi_vgpu_supported: extract_tag_text(block, "multiVgpuSupported")
            .map(parse_bool_str)
            .transpose()?
            .unwrap_or(false),
        encoder_capacity: tag_u32(block, "encoderCapacity").unwrap_or(0x64),
        bar1_length_mb: tag_u64(block, "bar1Length").unwrap_or(0x100),
        license: extract_tag_text(block, "license").unwrap_or("").to_string(),
        gpu_instance_size: tag_u32(block, "gpuInstanceSize").unwrap_or(0),
        gpu_direct_supported: tag_u32(block, "gpuDirectSupported").unwrap_or(0),
        nvlink_p2p_supported: tag_u32(block, "nvlinkP2PSupported").unwrap_or(0),
        multi_vgpu_exclusive: tag_u32(block, "multiVgpuExclusive").unwrap_or(0),
        extra_parameters: extract_tag_text(block, "extraParameters")
            .unwrap_or("")
            .to_string(),
    })
}

fn parse_dev_id(block: &str) -> VgpuResult<PciDevId> {
    let tag = find_tag_open(block, "<devId").ok_or(VgpuError::MalformedXml("missing devId"))?;
    Ok(PciDevId {
        vendor_id: parse_u16_str(
            extract_attr(tag, "vendorId").ok_or(VgpuError::MalformedXml("vendorId"))?,
        )?,
        device_id: parse_u16_str(
            extract_attr(tag, "deviceId").ok_or(VgpuError::MalformedXml("deviceId"))?,
        )?,
        subsystem_vendor_id: parse_u16_str(
            extract_attr(tag, "subsystemVendorId")
                .ok_or(VgpuError::MalformedXml("subsystemVendorId"))?,
        )?,
        subsystem_id: parse_u16_str(
            extract_attr(tag, "subsystemId").ok_or(VgpuError::MalformedXml("subsystemId"))?,
        )?,
    })
}

fn find_next_block<'a>(haystack: &'a str, open: &str, close: &str) -> Option<(usize, usize, &'a str)> {
    let s = haystack.find(open)?;
    let e_rel = haystack[s..].find(close)?;
    let e = s + e_rel + close.len();
    Some((s, e, &haystack[s..e]))
}

fn extract_block<'a>(haystack: &'a str, open: &str, close: &str) -> Option<&'a str> {
    find_next_block(haystack, open, close).map(|(_, _, b)| b)
}

fn find_tag_open<'a>(haystack: &'a str, prefix: &str) -> Option<&'a str> {
    let s = haystack.find(prefix)?;
    let e_rel = haystack[s..].find('>')?;
    Some(&haystack[s..s + e_rel + 1])
}

fn extract_tag_text<'a>(haystack: &'a str, tag: &str) -> Option<&'a str> {
    let open_pat = format!("<{}", tag);
    let close_pat = format!("</{}>", tag);
    let s = haystack.find(&open_pat)?;
    let gt = haystack[s..].find('>')? + s + 1;
    let e = haystack[gt..].find(&close_pat)? + gt;
    Some(haystack[gt..e].trim())
}

fn extract_attr<'a>(tag: &'a str, attr: &str) -> Option<&'a str> {
    let pat_dq = format!("{}=\"", attr);
    if let Some(i) = tag.find(&pat_dq) {
        let start = i + pat_dq.len();
        let end = tag[start..].find('"')? + start;
        return Some(&tag[start..end]);
    }
    let pat_sq = format!("{}='", attr);
    if let Some(i) = tag.find(&pat_sq) {
        let start = i + pat_sq.len();
        let end = tag[start..].find('\'')? + start;
        return Some(&tag[start..end]);
    }
    None
}

fn tag_u32(block: &str, tag: &str) -> VgpuResult<u32> {
    let s = extract_tag_text(block, tag).ok_or(VgpuError::MalformedXml("missing u32 tag"))?;
    parse_u32_str(s)
}

fn tag_u64(block: &str, tag: &str) -> VgpuResult<u64> {
    let s = extract_tag_text(block, tag).ok_or(VgpuError::MalformedXml("missing u64 tag"))?;
    parse_u64_str(s)
}

fn parse_u64_str(s: &str) -> VgpuResult<u64> {
    let t = s.trim();
    if let Some(hex) = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        u64::from_str_radix(hex, 16).map_err(|_| VgpuError::MalformedXml("bad hex u64"))
    } else {
        t.parse::<u64>()
            .map_err(|_| VgpuError::MalformedXml("bad dec u64"))
    }
}

fn parse_u32_str(s: &str) -> VgpuResult<u32> {
    let v = parse_u64_str(s)?;
    u32::try_from(v).map_err(|_| VgpuError::MalformedXml("u32 overflow"))
}

fn parse_u16_str(s: &str) -> VgpuResult<u16> {
    let v = parse_u64_str(s)?;
    u16::try_from(v).map_err(|_| VgpuError::MalformedXml("u16 overflow"))
}

fn parse_bool_str(s: &str) -> VgpuResult<bool> {
    match s.trim().to_ascii_lowercase().as_str() {
        "1" | "true" => Ok(true),
        "0" | "false" => Ok(false),
        _ => Err(VgpuError::MalformedXml("bad boolean")),
    }
}

fn decode_hex_128(hex: &str) -> VgpuResult<[u8; VGPU_DIGEST_BYTES]> {
    let clean = hex.trim();
    if clean.len() != VGPU_DIGEST_BYTES * 2 {
        return Err(VgpuError::MalformedXml("digest must be 256 hex chars"));
    }
    let mut out = [0u8; VGPU_DIGEST_BYTES];
    for (i, dst) in out.iter_mut().enumerate() {
        *dst = u8::from_str_radix(&clean[i * 2..i * 2 + 2], 16)
            .map_err(|_| VgpuError::MalformedXml("bad hex digest"))?;
    }
    Ok(out)
}
