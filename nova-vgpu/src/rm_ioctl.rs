//! NVIDIA Linux RM character-device (`/dev/nvidiactl`, `/dev/nvidiaN`) ioctl client
//! replicating `nvidia-vgpud` (`535.309.01-vgpu-kvm`) analyzed with REA 6.10.1.

use crate::config::{Nva081SetInfoParams, VgpuConfigStore};
use std::collections::BTreeMap;

/// NVIDIA escape / ioctl numbers (`_IOWR('F', nr, size)`).
pub mod ioctl_nr {
    pub const NV_ESC_CARD_INFO: usize = 0xc900_46c8;
    pub const NV_ESC_REGISTER_FD: usize = 0xc004_46c9;
    pub const NV_ESC_CHECK_VERSION_STR: usize = 0xc048_46d2;
    pub const NV_ESC_RM_FREE: usize = 0xc010_4629;
    pub const NV_ESC_RM_CONTROL: usize = 0xc020_462a;
    pub const NV_ESC_RM_ALLOC_V0: usize = 0xc020_462b;
    pub const NV_ESC_RM_ALLOC: usize = 0xc030_462b;
}

/// RM class IDs and control command IDs used during vGPU type registration.
pub mod rm_id {
    pub const NV01_DEVICE_0: u32 = 0x0000_0080;
    pub const NV20_SUBDEVICE_0: u32 = 0x0000_2080;
    pub const NVA081_VGPU_CONFIG: u32 = 0x0000_a081;

    pub const NV0000_CTRL_GPU_GET_ID_INFO: u32 = 0x0000_0202;
    pub const NV0000_CTRL_GPU_GET_PROBED_IDS: u32 = 0x0000_0214;
    pub const NV0000_CTRL_GPU_ATTACH_IDS: u32 = 0x0000_0215;
    pub const NV0000_CTRL_GPU_DETACH_IDS: u32 = 0x0000_0216;
    pub const NV0000_CTRL_GPU_GET_PCI_INFO: u32 = 0x0000_021b;
    pub const NV0080_CTRL_GPU_GET_VIRTUALIZATION_MODE: u32 = 0x0080_0289;
    pub const NV2080_CTRL_BUS_GET_PCI_INFO: u32 = 0x2080_1801;
    pub const NVA081_CTRL_VGPU_CONFIG_SET_INFO: u32 = 0xa081_0101;
    pub const NVA081_CTRL_VGPU_CONFIG_NOTIFY_START: u32 = 0xa081_0109;
    pub const NVA081_CTRL_VGPU_CONFIG_SET_FRACTIONAL_MULTI_VGPU: u32 = 0xa081_0119;

    pub const NV_ERR_DUPLICATE: u32 = 0x54;
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RmCardInfo {
    pub domain: u32,
    pub bus: u8,
    pub slot: u8,
    pub function: u8,
    pub vendor_id: u16,
    pub device_id: u16,
    pub gpu_id: u32,
    pub minor: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RegisteredGpuSummary {
    pub gpu_id: u32,
    pub domain: u32,
    pub bus: u8,
    pub slot: u8,
    pub vendor_id: u16,
    pub device_id: u16,
    pub subsystem_vendor_id: u16,
    pub subsystem_id: u16,
    pub virtualization_mode: u32,
    pub profiles_registered: usize,
    pub skipped_reason: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct VgpudSummary {
    pub driver_version: String,
    pub h_client: u32,
    pub gpus: Vec<RegisteredGpuSummary>,
}

/// Abstract RM character-device transport so registration logic is testable without `/dev/nvidiactl`.
pub trait RmTransport {
    fn check_version(&mut self, version: &str) -> Result<(), String>;
    fn card_info(&mut self) -> Result<Vec<RmCardInfo>, String>;
    fn alloc_root(&mut self) -> Result<u32, String>;
    fn register_gpu_minor(&mut self, minor: u32) -> Result<(), String>;
    fn close_gpu_minor(&mut self, minor: u32);
    fn alloc_object(
        &mut self,
        h_client: u32,
        h_parent: u32,
        h_new: u32,
        h_class: u32,
        params: Option<&mut [u8]>,
    ) -> Result<u32, String>;
    fn control(
        &mut self,
        h_client: u32,
        h_object: u32,
        cmd: u32,
        params: Option<&mut [u8]>,
    ) -> Result<u32, String>;
    fn free(&mut self, h_client: u32, h_parent: u32, h_old: u32) -> Result<u32, String>;
}

/// Read the active NVIDIA kernel module version from `/proc/driver/nvidia/version`.
pub fn detect_driver_version(proc_version_text: &str) -> String {
    if let Some(idx) = proc_version_text.find("Kernel Module") {
        let rest = &proc_version_text[idx + "Kernel Module".len()..];
        if let Some(ver) = rest.split_whitespace().next() {
            if ver.chars().next().is_some_and(|c| c.is_ascii_digit()) {
                return ver.to_string();
            }
        }
    }
    String::from("535.309.01")
}

/// Execute the full `nvidia-vgpud` (`0x402ca0.c`) GPU enumeration and `0xa0810101`
/// profile registration sequence against any [`RmTransport`].
pub fn run_vgpud_registration<T: RmTransport>(
    transport: &mut T,
    store: &VgpuConfigStore,
    driver_version: &str,
) -> Result<VgpudSummary, String> {
    transport.check_version(driver_version)?;
    let cards = transport.card_info()?;
    let minor_by_gpu: BTreeMap<u32, u32> = cards.iter().map(|c| (c.gpu_id, c.minor)).collect();

    let h_client = transport.alloc_root()?;
    if h_client == 0 {
        return Err(String::from("alloc_root returned handle 0"));
    }

    let result = register_all_gpus(transport, store, &minor_by_gpu, h_client);
    let _ = transport.free(h_client, h_client, h_client);
    result.map(|gpus| VgpudSummary {
        driver_version: driver_version.to_string(),
        h_client,
        gpus,
    })
}

fn register_all_gpus<T: RmTransport>(
    transport: &mut T,
    store: &VgpuConfigStore,
    minor_by_gpu: &BTreeMap<u32, u32>,
    h_client: u32,
) -> Result<Vec<RegisteredGpuSummary>, String> {
    let mut probed = [0u8; 0x100];
    let st = transport.control(
        h_client,
        h_client,
        rm_id::NV0000_CTRL_GPU_GET_PROBED_IDS,
        Some(&mut probed),
    )?;
    if st != 0 {
        return Err(format!("GET_PROBED_IDS failed with RM status 0x{st:x}"));
    }

    let mut gpu_ids = Vec::new();
    for idx in 0..32 {
        let gid = read_u32(&probed, idx * 4);
        if gid == 0xffff_ffff {
            break;
        }
        gpu_ids.push(gid);
    }
    if gpu_ids.is_empty() {
        return Err(String::from("no physical GPUs reported by GET_PROBED_IDS"));
    }

    let mut summaries = Vec::with_capacity(gpu_ids.len());
    for gid in gpu_ids {
        summaries.push(register_single_gpu(
            transport,
            store,
            minor_by_gpu,
            h_client,
            gid,
        )?);
    }
    Ok(summaries)
}

fn register_single_gpu<T: RmTransport>(
    transport: &mut T,
    store: &VgpuConfigStore,
    minor_by_gpu: &BTreeMap<u32, u32>,
    h_client: u32,
    gpu_id: u32,
) -> Result<RegisteredGpuSummary, String> {
    let mut pci_req = [0u8; 0x0c];
    put_u32(&mut pci_req, 0, gpu_id);
    let st = transport.control(
        h_client,
        h_client,
        rm_id::NV0000_CTRL_GPU_GET_PCI_INFO,
        Some(&mut pci_req),
    )?;
    if st != 0 {
        return Err(format!("GET_PCI_INFO(0x{gpu_id:x}) failed: 0x{st:x}"));
    }
    let domain = read_u32(&pci_req, 4);
    let bus_slot = read_u32(&pci_req, 8);
    let bus = (bus_slot & 0xff) as u8;
    let slot = ((bus_slot >> 8) & 0xff) as u8;

    let mut attach_req = [0u8; 0x84];
    put_u32(&mut attach_req, 0, gpu_id);
    put_u32(&mut attach_req, 4, 0xffff_ffff);
    let st = transport.control(
        h_client,
        h_client,
        rm_id::NV0000_CTRL_GPU_ATTACH_IDS,
        Some(&mut attach_req),
    )?;
    if st != 0 {
        return Err(format!("ATTACH_IDS(0x{gpu_id:x}) failed: 0x{st:x}"));
    }

    let minor = minor_by_gpu.get(&gpu_id).copied();
    let res = register_attached_gpu(
        transport, store, minor, h_client, gpu_id, domain, bus, slot,
    );

    if let Some(m) = minor {
        transport.close_gpu_minor(m);
    }
    let mut detach_req = [0u8; 0x80];
    put_u32(&mut detach_req, 0, gpu_id);
    put_u32(&mut detach_req, 4, 0xffff_ffff);
    let _ = transport.control(
        h_client,
        h_client,
        rm_id::NV0000_CTRL_GPU_DETACH_IDS,
        Some(&mut detach_req),
    );
    res
}

#[allow(clippy::too_many_arguments)]
fn register_attached_gpu<T: RmTransport>(
    transport: &mut T,
    store: &VgpuConfigStore,
    minor: Option<u32>,
    h_client: u32,
    gpu_id: u32,
    domain: u32,
    bus: u8,
    slot: u8,
) -> Result<RegisteredGpuSummary, String> {
    if let Some(m) = minor {
        transport.register_gpu_minor(m)?;
    }

    let mut id_info = [0u8; 0x28];
    put_u32(&mut id_info, 0, gpu_id);
    let st = transport.control(
        h_client,
        h_client,
        rm_id::NV0000_CTRL_GPU_GET_ID_INFO,
        Some(&mut id_info),
    )?;
    if st != 0 {
        return Err(format!("GET_ID_INFO(0x{gpu_id:x}) failed: 0x{st:x}"));
    }
    let dev_inst = read_u32(&id_info, 8);

    // NVIDIA uses wrapping add (`(gpu_id + h_client) * 0x100 | suffix`) for unique object handles.
    let base_handle = gpu_id.wrapping_add(h_client).wrapping_shl(8);
    let h_dev = base_handle | 1;
    let h_sub = base_handle | 2;
    let h_cfg = base_handle | 3;

    let mut dev_params = [0u8; 0x28];
    put_u32(&mut dev_params, 0, dev_inst);
    let st = transport.alloc_object(
        h_client,
        h_client,
        h_dev,
        rm_id::NV01_DEVICE_0,
        Some(&mut dev_params),
    )?;
    if st != 0 {
        return Err(format!("alloc NV01_DEVICE_0 failed: 0x{st:x}"));
    }

    let out = (|| -> Result<RegisteredGpuSummary, String> {
        let mut sub_params = [0u8; 4];
        let st = transport.alloc_object(
            h_client,
            h_dev,
            h_sub,
            rm_id::NV20_SUBDEVICE_0,
            Some(&mut sub_params),
        )?;
        if st != 0 {
            return Err(format!("alloc NV20_SUBDEVICE_0 failed: 0x{st:x}"));
        }

        let sub_res = (|| -> Result<RegisteredGpuSummary, String> {
            let mut bus_pci = [0u8; 0x10];
            let st = transport.control(
                h_client,
                h_sub,
                rm_id::NV2080_CTRL_BUS_GET_PCI_INFO,
                Some(&mut bus_pci),
            )?;
            if st != 0 {
                return Err(format!("BUS_GET_PCI_INFO failed: 0x{st:x}"));
            }
            let vendor_id = read_u16(&bus_pci, 0);
            let device_id = read_u16(&bus_pci, 2);
            let subsystem_vendor_id = read_u16(&bus_pci, 4);
            let subsystem_id = read_u16(&bus_pci, 6);

            let mut vmode_buf = [0u8; 4];
            let st = transport.control(
                h_client,
                h_dev,
                rm_id::NV0080_CTRL_GPU_GET_VIRTUALIZATION_MODE,
                Some(&mut vmode_buf),
            )?;
            if st != 0 {
                return Err(format!("GET_VIRTUALIZATION_MODE failed: 0x{st:x}"));
            }
            let virtualization_mode = read_u32(&vmode_buf, 0);

            // Match nvidia-vgpud 0x402ca0 (`1 < uVar - 1`): modes 1 (VGX guest) & 2 (SRIOV guest) skip host vGPU init.
            if matches!(virtualization_mode, 1 | 2) {
                return Ok(RegisteredGpuSummary {
                    gpu_id,
                    domain,
                    bus,
                    slot,
                    vendor_id,
                    device_id,
                    subsystem_vendor_id,
                    subsystem_id,
                    virtualization_mode,
                    profiles_registered: 0,
                    skipped_reason: Some(format!(
                        "GPU is running in guest virtualization mode ({virtualization_mode})"
                    )),
                });
            }

            let Some(pgpu) =
                store.find_pgpu_exact(vendor_id, device_id, subsystem_vendor_id, subsystem_id)
            else {
                return Ok(RegisteredGpuSummary {
                    gpu_id,
                    domain,
                    bus,
                    slot,
                    vendor_id,
                    device_id,
                    subsystem_vendor_id,
                    subsystem_id,
                    virtualization_mode,
                    profiles_registered: 0,
                    skipped_reason: Some(format!(
                        "GPU {vendor_id:04x}:{device_id:04x}/{subsystem_vendor_id:04x}:{subsystem_id:04x} not in vgpuConfig.xml"
                    )),
                });
            };

            let st = transport.alloc_object(
                h_client,
                h_sub,
                h_cfg,
                rm_id::NVA081_VGPU_CONFIG,
                None,
            )?;
            if st != 0 {
                return Err(format!("alloc NVA081_VGPU_CONFIG failed: 0x{st:x}"));
            }

            let cfg_res = (|| -> Result<usize, String> {
                let mut frac = u32::from(pgpu.fractional_multi_vgpu).to_le_bytes();
                let st = transport.control(
                    h_client,
                    h_cfg,
                    rm_id::NVA081_CTRL_VGPU_CONFIG_SET_FRACTIONAL_MULTI_VGPU,
                    Some(&mut frac),
                )?;
                if st != 0 {
                    return Err(format!("SET_FRACTIONAL_MULTI_VGPU failed: 0x{st:x}"));
                }

                let mut registered = 0usize;
                let mut first_flag = true;
                let mut any_ok = false;
                let total = pgpu.supported.len();

                for (idx, sup) in pgpu.supported.iter().enumerate() {
                    let prof = store.profile_by_id(sup.vgpu_id).ok_or_else(|| {
                        format!("profile vgpuId={} missing from vgpuConfig.xml", sup.vgpu_id)
                    })?;
                    let is_last = idx + 1 == total;
                    let mut params = Nva081SetInfoParams::from_profile_for_pgpu(
                        prof,
                        sup,
                        pgpu.dev_id.device_id,
                        is_last,
                        first_flag,
                    );
                    let st = transport.control(
                        h_client,
                        h_cfg,
                        rm_id::NVA081_CTRL_VGPU_CONFIG_SET_INFO,
                        Some(&mut params.raw),
                    )?;
                    if st == 0 {
                        first_flag = false;
                        any_ok = true;
                        registered += 1;
                    } else if st == rm_id::NV_ERR_DUPLICATE {
                        // Already present in RM table; nvidia-vgpud continues loop cleanly.
                    } else {
                        return Err(format!(
                            "SET_INFO(vgpuId={}) failed with RM status 0x{st:x}",
                            prof.id
                        ));
                    }
                }

                if any_ok {
                    let st = transport.control(
                        h_client,
                        h_cfg,
                        rm_id::NVA081_CTRL_VGPU_CONFIG_NOTIFY_START,
                        None,
                    )?;
                    if st != 0 {
                        return Err(format!("NOTIFY_START failed: 0x{st:x}"));
                    }
                }
                Ok(registered)
            })();

            let _ = transport.free(h_client, h_sub, h_cfg);
            let profiles_registered = cfg_res?;
            Ok(RegisteredGpuSummary {
                gpu_id,
                domain,
                bus,
                slot,
                vendor_id,
                device_id,
                subsystem_vendor_id,
                subsystem_id,
                virtualization_mode,
                profiles_registered,
                skipped_reason: None,
            })
        })();

        let _ = transport.free(h_client, h_dev, h_sub);
        sub_res
    })();

    let _ = transport.free(h_client, h_client, h_dev);
    out
}

fn read_u16(buf: &[u8], off: usize) -> u16 {
    u16::from_le_bytes([buf[off], buf[off + 1]])
}

fn read_u32(buf: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([buf[off], buf[off + 1], buf[off + 2], buf[off + 3]])
}

fn put_u32(buf: &mut [u8], off: usize, val: u32) {
    buf[off..off + 4].copy_from_slice(&val.to_le_bytes());
}

#[cfg(unix)]
pub mod linux {
    use super::*;
    use std::ffi::CString;

    extern "C" {
        fn open(pathname: *const i8, flags: i32, ...) -> i32;
        fn close(fd: i32) -> i32;
        fn ioctl(fd: i32, request: usize, ...) -> i32;
    }

    const O_RDWR: i32 = 2;

    pub struct LinuxRmTransport {
        ctl_fd: i32,
        gpu_fds: BTreeMap<u32, i32>,
    }

    impl LinuxRmTransport {
        pub fn open() -> Result<Self, String> {
            let path = CString::new("/dev/nvidiactl").map_err(|e| e.to_string())?;
            let ctl_fd = unsafe { open(path.as_ptr(), O_RDWR) };
            if ctl_fd < 0 {
                return Err(format!(
                    "failed to open /dev/nvidiactl: {}",
                    std::io::Error::last_os_error()
                ));
            }
            Ok(Self {
                ctl_fd,
                gpu_fds: BTreeMap::new(),
            })
        }

        fn do_ioctl(fd: i32, req: usize, arg: *mut core::ffi::c_void, label: &str) -> Result<(), String> {
            let rc = unsafe { ioctl(fd, req, arg) };
            if rc < 0 {
                Err(format!(
                    "{label} ioctl(0x{req:x}) failed: {}",
                    std::io::Error::last_os_error()
                ))
            } else {
                Ok(())
            }
        }
    }

    impl Drop for LinuxRmTransport {
        fn drop(&mut self) {
            for (_, fd) in core::mem::take(&mut self.gpu_fds) {
                if fd >= 0 {
                    unsafe {
                        close(fd);
                    }
                }
            }
            if self.ctl_fd >= 0 {
                unsafe {
                    close(self.ctl_fd);
                }
                self.ctl_fd = -1;
            }
        }
    }

    impl RmTransport for LinuxRmTransport {
        fn check_version(&mut self, version: &str) -> Result<(), String> {
            let mut buf = [0u8; 0x48];
            let bytes = version.as_bytes();
            let n = bytes.len().min(63);
            buf[8..8 + n].copy_from_slice(&bytes[..n]);
            Self::do_ioctl(
                self.ctl_fd,
                ioctl_nr::NV_ESC_CHECK_VERSION_STR,
                buf.as_mut_ptr().cast(),
                "NV_ESC_CHECK_VERSION_STR",
            )
        }

        fn card_info(&mut self) -> Result<Vec<RmCardInfo>, String> {
            let mut buf = [0u8; 0x900];
            Self::do_ioctl(
                self.ctl_fd,
                ioctl_nr::NV_ESC_CARD_INFO,
                buf.as_mut_ptr().cast(),
                "NV_ESC_CARD_INFO",
            )?;
            let mut out = Vec::new();
            for i in 0..32 {
                let off = i * 0x48;
                if buf[off] == 0 {
                    continue;
                }
                out.push(RmCardInfo {
                    domain: read_u32(&buf, off + 4),
                    bus: buf[off + 8],
                    slot: buf[off + 9],
                    function: buf[off + 10],
                    vendor_id: read_u16(&buf, off + 12),
                    device_id: read_u16(&buf, off + 14),
                    gpu_id: read_u32(&buf, off + 16),
                    minor: read_u32(&buf, off + 56),
                });
            }
            Ok(out)
        }

        fn alloc_root(&mut self) -> Result<u32, String> {
            let mut buf = [0u8; 0x20];
            Self::do_ioctl(
                self.ctl_fd,
                ioctl_nr::NV_ESC_RM_ALLOC_V0,
                buf.as_mut_ptr().cast(),
                "NV_ESC_RM_ALLOC_V0(root)",
            )?;
            let status = read_u32(&buf, 24);
            if status != 0 {
                return Err(format!("alloc_root RM status 0x{status:x}"));
            }
            Ok(read_u32(&buf, 8))
        }

        fn register_gpu_minor(&mut self, minor: u32) -> Result<(), String> {
            if self.gpu_fds.contains_key(&minor) {
                return Ok(());
            }
            let dev_path = format!("/dev/nvidia{minor}");
            let cpath = CString::new(dev_path.as_str()).map_err(|e| e.to_string())?;
            let fd = unsafe { open(cpath.as_ptr(), O_RDWR) };
            if fd < 0 {
                return Err(format!(
                    "failed to open {dev_path}: {}",
                    std::io::Error::last_os_error()
                ));
            }
            let mut ctl_arg = self.ctl_fd;
            if let Err(e) = Self::do_ioctl(
                fd,
                ioctl_nr::NV_ESC_REGISTER_FD,
                (&mut ctl_arg as *mut i32).cast(),
                "NV_ESC_REGISTER_FD",
            ) {
                unsafe {
                    close(fd);
                }
                return Err(e);
            }
            self.gpu_fds.insert(minor, fd);
            Ok(())
        }

        fn close_gpu_minor(&mut self, minor: u32) {
            if let Some(fd) = self.gpu_fds.remove(&minor) {
                if fd >= 0 {
                    unsafe {
                        close(fd);
                    }
                }
            }
        }

        fn alloc_object(
            &mut self,
            h_client: u32,
            h_parent: u32,
            h_new: u32,
            h_class: u32,
            params: Option<&mut [u8]>,
        ) -> Result<u32, String> {
            let mut buf = [0u8; 0x30];
            put_u32(&mut buf, 0, h_client);
            put_u32(&mut buf, 4, h_parent);
            put_u32(&mut buf, 8, h_new);
            put_u32(&mut buf, 12, h_class);
            if let Some(p) = params {
                let ptr = p.as_mut_ptr() as u64;
                buf[16..24].copy_from_slice(&ptr.to_le_bytes());
                put_u32(&mut buf, 32, p.len() as u32);
            }
            Self::do_ioctl(
                self.ctl_fd,
                ioctl_nr::NV_ESC_RM_ALLOC,
                buf.as_mut_ptr().cast(),
                "NV_ESC_RM_ALLOC",
            )?;
            Ok(read_u32(&buf, 40))
        }

        fn control(
            &mut self,
            h_client: u32,
            h_object: u32,
            cmd: u32,
            params: Option<&mut [u8]>,
        ) -> Result<u32, String> {
            let mut buf = [0u8; 0x20];
            put_u32(&mut buf, 0, h_client);
            put_u32(&mut buf, 4, h_object);
            put_u32(&mut buf, 8, cmd);
            if let Some(p) = params {
                let ptr = p.as_mut_ptr() as u64;
                buf[16..24].copy_from_slice(&ptr.to_le_bytes());
                put_u32(&mut buf, 24, p.len() as u32);
            }
            Self::do_ioctl(
                self.ctl_fd,
                ioctl_nr::NV_ESC_RM_CONTROL,
                buf.as_mut_ptr().cast(),
                "NV_ESC_RM_CONTROL",
            )?;
            Ok(read_u32(&buf, 28))
        }

        fn free(&mut self, h_client: u32, h_parent: u32, h_old: u32) -> Result<u32, String> {
            let mut buf = [0u8; 0x10];
            put_u32(&mut buf, 0, h_client);
            put_u32(&mut buf, 4, h_parent);
            put_u32(&mut buf, 8, h_old);
            Self::do_ioctl(
                self.ctl_fd,
                ioctl_nr::NV_ESC_RM_FREE,
                buf.as_mut_ptr().cast(),
                "NV_ESC_RM_FREE",
            )?;
            Ok(read_u32(&buf, 12))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Default)]
    struct MockRmTransport {
        h_client: u32,
        virtualization_mode: u32,
        pci_ids: (u16, u16, u16, u16),
        calls: Vec<String>,
        set_info_packets: Vec<Vec<u8>>,
        registered_minors: Vec<u32>,
        closed_minors: Vec<u32>,
    }

    impl RmTransport for MockRmTransport {
        fn check_version(&mut self, version: &str) -> Result<(), String> {
            self.calls.push(format!("check_version:{version}"));
            Ok(())
        }

        fn card_info(&mut self) -> Result<Vec<RmCardInfo>, String> {
            self.calls.push(String::from("card_info"));
            Ok(vec![RmCardInfo {
                domain: 0,
                bus: 1,
                slot: 0,
                function: 0,
                vendor_id: self.pci_ids.0,
                device_id: self.pci_ids.1,
                gpu_id: 0x100,
                minor: 0,
            }])
        }

        fn alloc_root(&mut self) -> Result<u32, String> {
            self.calls.push(String::from("alloc_root"));
            Ok(self.h_client)
        }

        fn register_gpu_minor(&mut self, minor: u32) -> Result<(), String> {
            self.registered_minors.push(minor);
            self.calls.push(format!("register_minor:{minor}"));
            Ok(())
        }

        fn close_gpu_minor(&mut self, minor: u32) {
            self.closed_minors.push(minor);
            self.calls.push(format!("close_minor:{minor}"));
        }

        fn alloc_object(
            &mut self,
            _h_client: u32,
            _h_parent: u32,
            h_new: u32,
            h_class: u32,
            _params: Option<&mut [u8]>,
        ) -> Result<u32, String> {
            self.calls.push(format!("alloc:0x{h_class:x}:0x{h_new:08x}"));
            Ok(0)
        }

        fn control(
            &mut self,
            _h_client: u32,
            _h_object: u32,
            cmd: u32,
            params: Option<&mut [u8]>,
        ) -> Result<u32, String> {
            self.calls.push(format!("ctrl:0x{cmd:x}"));
            match (cmd, params) {
                (rm_id::NV0000_CTRL_GPU_GET_PROBED_IDS, Some(p)) => {
                    put_u32(p, 0, 0x100);
                    put_u32(p, 4, 0xffff_ffff);
                }
                (rm_id::NV0000_CTRL_GPU_GET_PCI_INFO, Some(p)) => {
                    put_u32(p, 4, 0);
                    put_u32(p, 8, 1);
                }
                (rm_id::NV0000_CTRL_GPU_GET_ID_INFO, Some(p)) => {
                    put_u32(p, 8, 0);
                }
                (rm_id::NV2080_CTRL_BUS_GET_PCI_INFO, Some(p)) => {
                    p[0..2].copy_from_slice(&self.pci_ids.0.to_le_bytes());
                    p[2..4].copy_from_slice(&self.pci_ids.1.to_le_bytes());
                    p[4..6].copy_from_slice(&self.pci_ids.2.to_le_bytes());
                    p[6..8].copy_from_slice(&self.pci_ids.3.to_le_bytes());
                }
                (rm_id::NV0080_CTRL_GPU_GET_VIRTUALIZATION_MODE, Some(p)) => {
                    put_u32(p, 0, self.virtualization_mode);
                }
                (rm_id::NVA081_CTRL_VGPU_CONFIG_SET_INFO, Some(p)) => {
                    self.set_info_packets.push(p.to_vec());
                }
                _ => {}
            }
            Ok(0)
        }

        fn free(&mut self, _h_client: u32, _h_parent: u32, h_old: u32) -> Result<u32, String> {
            self.calls.push(format!("free:0x{h_old:08x}"));
            Ok(0)
        }
    }

    fn test_xml() -> String {
        let sig = "ab".repeat(128);
        format!(
            r#"<vgpuconfig><version>1.0</version>
<globalSettings><heterogeneousMultiVgpu>TRUE</heterogeneousMultiVgpu></globalSettings>
<pgpu fractionalMultiVgpu="0">
  <devId vendorId="0x10de" deviceId="0x1bb3" subsystemVendorId="0x10de" subsystemId="0x0"/>
  <supportedVgpu vgpuId="65"><maxVgpus>2</maxVgpus><digest>{sig}</digest></supportedVgpu>
  <supportedVgpu vgpuId="66"><maxVgpus>1</maxVgpus><digest>{sig}</digest></supportedVgpu>
</pgpu>
<vgpuType id="65" name="GRID P4-4Q" class="Quadro">
  <devId vendorId="0x10de" deviceId="0x1bb3" subsystemVendorId="0x10de" subsystemId="0x1206"/>
  <profileSize>0x100000000</profileSize><framebuffer>0xEC000000</framebuffer><fbReservation>0x14000000</fbReservation>
  <license>QVDWS;VWS;VWSExt</license>
</vgpuType>
<vgpuType id="66" name="GRID P4-8Q" class="Quadro">
  <devId vendorId="0x10de" deviceId="0x1bb3" subsystemVendorId="0x10de" subsystemId="0x1207"/>
  <profileSize>0x200000000</profileSize><framebuffer>0x1DC000000</framebuffer><fbReservation>0x24000000</fbReservation>
  <license>QVDWS</license>
</vgpuType>
</vgpuconfig>"#
        )
    }

    #[test]
    fn test_detect_driver_version_parses_proc() {
        let txt = "NVRM version: NVIDIA UNIX x86_64 Kernel Module  535.309.01  Wed Feb 11 00:00:00 UTC 2026\n";
        assert_eq!(detect_driver_version(txt), "535.309.01");
        assert_eq!(detect_driver_version("empty"), "535.309.01");
    }

    #[test]
    fn test_vgpud_full_host_registration_and_wrapping_handles() {
        let store = VgpuConfigStore::parse_xml(&test_xml()).unwrap();
        let mut mock = MockRmTransport {
            h_client: 0xc1d0_001e,
            virtualization_mode: 3, // HOST_VGPU
            pci_ids: (0x10de, 0x1bb3, 0x10de, 0x11d8),
            ..Default::default()
        };

        let summary = run_vgpud_registration(&mut mock, &store, "535.309.01").unwrap();
        assert_eq!(summary.gpus.len(), 1);
        assert_eq!(summary.gpus[0].profiles_registered, 2);
        assert!(summary.gpus[0].skipped_reason.is_none());
        assert_eq!(mock.registered_minors, vec![0]);
        assert_eq!(mock.closed_minors, vec![0]);
        assert!(mock.calls.contains(&String::from("ctrl:0xa0810109")));
        assert_eq!(mock.set_info_packets.len(), 2);
        assert_eq!(mock.set_info_packets[0][0], 1); // first_flag = 1
        assert_eq!(mock.set_info_packets[1][0], 0); // first_flag = 0
        assert_eq!(read_u32(&mock.set_info_packets[0], 0x1360), 1);
        assert_eq!(read_u32(&mock.set_info_packets[1], 0x1360), 2);
    }

    #[test]
    fn test_vgpud_skips_guest_mode_cleanly() {
        let store = VgpuConfigStore::parse_xml(&test_xml()).unwrap();
        let mut mock = MockRmTransport {
            h_client: 0xc1d0_0020,
            virtualization_mode: 1, // VGX guest mode -> must skip cleanly
            pci_ids: (0x10de, 0x1bb3, 0x10de, 0x1206),
            ..Default::default()
        };

        let summary = run_vgpud_registration(&mut mock, &store, "535.309.01").unwrap();
        assert_eq!(summary.gpus[0].profiles_registered, 0);
        assert!(summary.gpus[0].skipped_reason.is_some());
        assert!(!mock.calls.iter().any(|c| c.contains("0xa0810101")));
    }
}
