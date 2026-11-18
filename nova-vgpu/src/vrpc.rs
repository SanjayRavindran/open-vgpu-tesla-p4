//! Clean-room Guest <-> Host `"VRPC"` (`0x43505256`) RPC protocol and shared-ring
//! engine reverse-engineered from `libnvidia-vgpu.so.535.309.01` (`0x1c4870.c`, `0x1c73d0.c`).

use crate::{VgpuError, VgpuResult};

/// `"VRPC"` magic in little-endian (`'V','R','P','C'`).
pub const VRPC_SIGNATURE: u32 = 0x4350_5256;
/// Observed RPC protocol version (`0x0300_0000` in `FUN_001c73d0`).
pub const VRPC_VERSION_3_0: u32 = 0x0300_0000;
/// Size of the VRPC message header in bytes (`0x20`).
pub const VRPC_HEADER_BYTES: usize = 32;

/// Observed VRPC function IDs from `535.309.01` `nv-kernel.o_binary` & `_nv011617vgpu`.
#[repr(u32)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum VrpcFuncId {
    Nop = 0x00,
    SetGuestSystemInfo = 0x01,
    AllocRoot = 0x02,
    AllocDevice535 = 0x03,
    AllocDevice = 0x04,
    AllocSubdevice = 0x05,
    AllocMemory = 0x06,
    AllocObject = 0x07,
    Free = 0x09,
    Control = 0x0a,
    AllocChannelDma = 0x0b,
    MapMemory = 0x0c,
    UnmapMemory = 0x0d,
    MapMemoryDma = 0x0e,
    UnmapMemoryDma = 0x0f,
    BindContextDma = 0x10,
    SetObject = 0x13,
    IdleChannels = 0x14,
    IdleChannels535 = 0x16,
    RmApiControl = 0x1a,
    GetEngineInfo = 0x1e,
    SetGuestFbInfo = 0x20,
    SetGuestSemaMem = 0x21,
    UnsetGuestSemaMem = 0x22,
    SetGuestBar1Info = 0x24,
    Log = 0x25,
    GetStaticInfo535 = 0x32,
    SetGuestSystemInfoExt = 0x3f,
    GetStaticInfo2 = 0x4c,
    GetConsolidatedStaticInfo = 0x4f,
    GetStaticInfo = 0x55,
    UpdateBarInfo = 0x56,
    Unknown(u32),
}

impl VrpcFuncId {
    pub const fn from_u32(val: u32) -> Self {
        match val {
            0x00 => Self::Nop,
            0x01 => Self::SetGuestSystemInfo,
            0x02 => Self::AllocRoot,
            0x03 => Self::AllocDevice535,
            0x04 => Self::AllocDevice,
            0x05 => Self::AllocSubdevice,
            0x06 => Self::AllocMemory,
            0x07 => Self::AllocObject,
            0x09 => Self::Free,
            0x0a => Self::Control,
            0x0b => Self::AllocChannelDma,
            0x0c => Self::MapMemory,
            0x0d => Self::UnmapMemory,
            0x0e => Self::MapMemoryDma,
            0x0f => Self::UnmapMemoryDma,
            0x10 => Self::BindContextDma,
            0x13 => Self::SetObject,
            0x14 => Self::IdleChannels,
            0x16 => Self::IdleChannels535,
            0x1a => Self::RmApiControl,
            0x1e => Self::GetEngineInfo,
            0x20 => Self::SetGuestFbInfo,
            0x21 => Self::SetGuestSemaMem,
            0x22 => Self::UnsetGuestSemaMem,
            0x24 => Self::SetGuestBar1Info,
            0x25 => Self::Log,
            0x32 => Self::GetStaticInfo535,
            0x3f => Self::SetGuestSystemInfoExt,
            0x4c => Self::GetStaticInfo2,
            0x4f => Self::GetConsolidatedStaticInfo,
            0x55 => Self::GetStaticInfo,
            0x56 => Self::UpdateBarInfo,
            other => Self::Unknown(other),
        }
    }

    pub const fn as_u32(self) -> u32 {
        match self {
            Self::Nop => 0x00,
            Self::SetGuestSystemInfo => 0x01,
            Self::AllocRoot => 0x02,
            Self::AllocDevice535 => 0x03,
            Self::AllocDevice => 0x04,
            Self::AllocSubdevice => 0x05,
            Self::AllocMemory => 0x06,
            Self::AllocObject => 0x07,
            Self::Free => 0x09,
            Self::Control => 0x0a,
            Self::AllocChannelDma => 0x0b,
            Self::MapMemory => 0x0c,
            Self::UnmapMemory => 0x0d,
            Self::MapMemoryDma => 0x0e,
            Self::UnmapMemoryDma => 0x0f,
            Self::BindContextDma => 0x10,
            Self::SetObject => 0x13,
            Self::IdleChannels => 0x14,
            Self::IdleChannels535 => 0x16,
            Self::RmApiControl => 0x1a,
            Self::GetEngineInfo => 0x1e,
            Self::SetGuestFbInfo => 0x20,
            Self::SetGuestSemaMem => 0x21,
            Self::UnsetGuestSemaMem => 0x22,
            Self::SetGuestBar1Info => 0x24,
            Self::Log => 0x25,
            Self::GetStaticInfo535 => 0x32,
            Self::SetGuestSystemInfoExt => 0x3f,
            Self::GetStaticInfo2 => 0x4c,
            Self::GetConsolidatedStaticInfo => 0x4f,
            Self::GetStaticInfo => 0x55,
            Self::UpdateBarInfo => 0x56,
            Self::Unknown(v) => v,
        }
    }
}

/// 32-byte VRPC message header (`FUN_001c73d0`: `+0x00..+0x20`).
#[repr(C)]
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub struct VrpcHeader {
    pub version: u32,
    pub signature: u32,
    pub total_length: u32,
    pub function: u32,
    pub result: u32,
    pub spare: u32,
    pub sequence: u32,
    pub flags: u32,
}

impl VrpcHeader {
    /// Construct a new outbound/request VRPC header (`result = u32::MAX` pending).
    pub fn new_request(function: VrpcFuncId, payload_len: u32) -> Self {
        Self {
            version: VRPC_VERSION_3_0,
            signature: VRPC_SIGNATURE,
            total_length: VRPC_HEADER_BYTES as u32 + payload_len,
            function: function.as_u32(),
            result: u32::MAX,
            spare: u32::MAX,
            sequence: 0,
            flags: 0,
        }
    }

    /// Encode header into the first 32 bytes of `buf`.
    pub fn encode(&self, buf: &mut [u8]) -> VgpuResult<()> {
        if buf.len() < VRPC_HEADER_BYTES {
            return Err(VgpuError::BufferTooSmall);
        }
        buf[0x00..0x04].copy_from_slice(&self.version.to_le_bytes());
        buf[0x04..0x08].copy_from_slice(&self.signature.to_le_bytes());
        buf[0x08..0x0c].copy_from_slice(&self.total_length.to_le_bytes());
        buf[0x0c..0x10].copy_from_slice(&self.function.to_le_bytes());
        buf[0x10..0x14].copy_from_slice(&self.result.to_le_bytes());
        buf[0x14..0x18].copy_from_slice(&self.spare.to_le_bytes());
        buf[0x18..0x1c].copy_from_slice(&self.sequence.to_le_bytes());
        buf[0x1c..0x20].copy_from_slice(&self.flags.to_le_bytes());
        Ok(())
    }

    /// Decode and validate a 32-byte VRPC header (`signature == 0x43505256`).
    pub fn decode(buf: &[u8]) -> VgpuResult<Self> {
        if buf.len() < VRPC_HEADER_BYTES {
            return Err(VgpuError::BufferTooSmall);
        }
        let version = u32::from_le_bytes(buf[0x00..0x04].try_into().unwrap());
        let signature = u32::from_le_bytes(buf[0x04..0x08].try_into().unwrap());
        if signature != VRPC_SIGNATURE {
            return Err(VgpuError::InvalidVrpcSignature(signature));
        }
        let total_length = u32::from_le_bytes(buf[0x08..0x0c].try_into().unwrap());
        if (total_length as usize) < VRPC_HEADER_BYTES || (total_length as usize) > buf.len() {
            return Err(VgpuError::InvalidVrpcLength(total_length));
        }
        Ok(Self {
            version,
            signature,
            total_length,
            function: u32::from_le_bytes(buf[0x0c..0x10].try_into().unwrap()),
            result: u32::from_le_bytes(buf[0x10..0x14].try_into().unwrap()),
            spare: u32::from_le_bytes(buf[0x14..0x18].try_into().unwrap()),
            sequence: u32::from_le_bytes(buf[0x18..0x1c].try_into().unwrap()),
            flags: u32::from_le_bytes(buf[0x1c..0x20].try_into().unwrap()),
        })
    }
}

/// Ring descriptor matching `SEND_RING` / `RECV_RING` / `EVENT_RING` in `NV_VGPU` (`0x8f000`).
#[derive(Debug, Copy, Clone, PartialEq, Eq, Default)]
pub struct VgpuRingState {
    pub gpa_lo: u32,
    pub gpa_hi: u32,
    pub put: u32,
    pub get: u32,
    pub entry_size: u32,
    pub capacity_bytes: u32,
}

impl VgpuRingState {
    pub const fn gpa(&self) -> u64 {
        ((self.gpa_hi as u64) << 32) | (self.gpa_lo as u64)
    }

    pub const fn is_active(&self) -> bool {
        self.gpa() != 0 && self.entry_size != 0
    }
}

/// Clean-room host VRPC state machine handling guest initialization & command dispatch.
#[derive(Debug, Clone)]
pub struct VrpcEngine {
    pub guest_vgx_initialized: bool,
    pub guest_driver_loaded: bool,
    pub guest_fb_bytes: u64,
    pub pci_device_id: u16,
    pub pci_subsystem_id: u16,
    pub processed_count: u64,
}

impl VrpcEngine {
    pub fn new(guest_fb_bytes: u64, pci_device_id: u16, pci_subsystem_id: u16) -> Self {
        Self {
            guest_vgx_initialized: false,
            guest_driver_loaded: false,
            guest_fb_bytes,
            pci_device_id,
            pci_subsystem_id,
            processed_count: 0,
        }
    }

    /// Verify if a VRPC function ID is permitted on Pascal Linux/Compute (`_nv007236vgpu`).
    pub fn is_supported_on_vgpu(func_id: u32) -> bool {
        !matches!(func_id, 0x41 | 0x4a | 0x4c | 0x67) && (func_id <= 0x9f || func_id - 0xa7 <= 0x24)
    }

    /// Process a single raw VRPC message buffer in-place, returning `NV_STATUS` (`0` = `NV_OK`).
    pub fn handle_message(&mut self, msg_buf: &mut [u8]) -> VgpuResult<u32> {
        let mut hdr = VrpcHeader::decode(msg_buf)?;
        if !Self::is_supported_on_vgpu(hdr.function) {
            hdr.result = 0x56; // NV_ERR_NOT_SUPPORTED
            hdr.encode(msg_buf)?;
            return Ok(hdr.result);
        }

        let func = VrpcFuncId::from_u32(hdr.function);
        if func != VrpcFuncId::SetGuestSystemInfo && !self.guest_vgx_initialized {
            // Matches 0x1c4870.c: "VGPU message %d failed, guest VGX version not initialized..."
            hdr.result = 0x56;
            hdr.encode(msg_buf)?;
            return Ok(hdr.result);
        }

        let status = match func {
            VrpcFuncId::SetGuestSystemInfo => {
                if self.guest_vgx_initialized {
                    0x40 // NV_ERR_INVALID_STATE
                } else {
                    self.guest_vgx_initialized = true;
                    self.guest_driver_loaded = true;
                    0
                }
            }
            VrpcFuncId::GetStaticInfo | VrpcFuncId::GetStaticInfo535 | VrpcFuncId::GetStaticInfo2 => {
                // Populate guest FB size & virtual PCI IDs in payload if buffer is large enough.
                if msg_buf.len() >= VRPC_HEADER_BYTES + 16 {
                    msg_buf[VRPC_HEADER_BYTES..VRPC_HEADER_BYTES + 8]
                        .copy_from_slice(&self.guest_fb_bytes.to_le_bytes());
                    msg_buf[VRPC_HEADER_BYTES + 8..VRPC_HEADER_BYTES + 10]
                        .copy_from_slice(&self.pci_device_id.to_le_bytes());
                    msg_buf[VRPC_HEADER_BYTES + 10..VRPC_HEADER_BYTES + 12]
                        .copy_from_slice(&self.pci_subsystem_id.to_le_bytes());
                }
                0
            }
            VrpcFuncId::Nop
            | VrpcFuncId::AllocRoot
            | VrpcFuncId::AllocDevice535
            | VrpcFuncId::AllocDevice
            | VrpcFuncId::AllocSubdevice
            | VrpcFuncId::AllocMemory
            | VrpcFuncId::AllocObject
            | VrpcFuncId::Free
            | VrpcFuncId::Control
            | VrpcFuncId::AllocChannelDma
            | VrpcFuncId::MapMemory
            | VrpcFuncId::UnmapMemory
            | VrpcFuncId::MapMemoryDma
            | VrpcFuncId::UnmapMemoryDma
            | VrpcFuncId::BindContextDma
            | VrpcFuncId::SetObject
            | VrpcFuncId::IdleChannels
            | VrpcFuncId::IdleChannels535
            | VrpcFuncId::RmApiControl
            | VrpcFuncId::GetEngineInfo
            | VrpcFuncId::SetGuestFbInfo
            | VrpcFuncId::SetGuestSemaMem
            | VrpcFuncId::UnsetGuestSemaMem
            | VrpcFuncId::SetGuestBar1Info
            | VrpcFuncId::Log
            | VrpcFuncId::SetGuestSystemInfoExt
            | VrpcFuncId::GetConsolidatedStaticInfo
            | VrpcFuncId::UpdateBarInfo => 0,
            VrpcFuncId::Unknown(_) => 0x56,
        };

        self.processed_count += 1;
        hdr.result = status;
        hdr.encode(msg_buf)?;
        Ok(status)
    }
}
