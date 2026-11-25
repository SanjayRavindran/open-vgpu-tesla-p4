//! Clean-room Rust reimplementation of the NVIDIA Pascal (`GP104GL` / Tesla P4)
//! KVM vGPU software stack targeting R580 (`580.196.09-vgpu-kvm`).
//!
//! Hardware Falcon security (`PrivSec = 1`) requires NVIDIA's signed Falcon
//! binaries (`ACR`, `PMU`, `SEC2`, `FECS`, `GPCCS`, `NVENC`, `NVDEC`, `GR Netlist`),
//! validated cleanly via [`falcon_blobs`]. All host-side vGPU virtualization (`vgpuConfig.xml`,
//! `nvidia-vgpud`, `nvidia-vgpu-vfio` ABI, `vmiop`, BAR0 `0x8f000` trapping, `"VRPC"`,
//! `VMMU`, `CHIDMGR`, and `FRL`) and the guest BAR0/`"VRPC"` driver are implemented cleanly
//! in safe Rust here.

pub mod bar0_emul;
pub mod config;
pub mod falcon_blobs;
pub mod guest;
pub mod pci_cfg;
pub mod rm_ioctl;
pub mod rm_vgpu;
pub mod vfio_ioctl;
pub mod vmiop;
pub mod vmmu;
pub mod vrpc;

/// Unified error status for `nova-vgpu` operations.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum VgpuError {
    MalformedXml(&'static str),
    MalformedBlob(&'static str),
    UnsupportedGpu(u16, u16),
    ProfileNotFound(u32),
    HeterogeneousConflict(u32, u32),
    CapacityExhausted,
    OutOfVram,
    DeviceNotFound(u32),
    InvalidRegisterAccess(u64, u32),
    InvalidVrpcSignature(u32),
    InvalidVrpcLength(u32),
    InvalidGuestPfn(u64),
    InvalidChannel(u32),
    BufferTooSmall,
}

pub type VgpuResult<T> = core::result::Result<T, VgpuError>;

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bar0_emul::{nv_vgpu_reg, NV_VGPU_BAR0_BASE};
    use crate::config::{Nva081SetInfoParams, VgpuConfigStore};
    use crate::vfio_ioctl::{ctl_ioctl, dev_ioctl, VgpuDeviceMmapRegion, VgpuEmulSpace};
    use crate::vmiop::{PascalVgpuManager, VMIOP_PLUGIN_HEADER_V5, VMIOP_PLUGIN_STRUCT_BYTES};
    use crate::vmmu::EngineKind;
    use crate::vrpc::{VrpcFuncId, VrpcHeader, VRPC_HEADER_BYTES, VRPC_SIGNATURE};

    fn sample_p4_xml() -> String {
        let sig_63 = "2f".repeat(128);
        let sig_64 = "2a".repeat(128);
        format!(
            r#"<vgpuconfig>
  <version>1.0</version>
  <globalSettings>
    <heterogeneousMultiVgpu>FALSE</heterogeneousMultiVgpu>
    <pluginSoName>libnvidia-vgpu</pluginSoName>
  </globalSettings>
  <pgpu fractionalMultiVgpu="0">
    <devId vendorId="0x10de" deviceId="0x1BB3" subsystemVendorId="0x10de" subsystemId="0x0" />
    <supportedVgpu vgpuId="63">
      <maxVgpus>8</maxVgpus>
      <digest type="signature">{sig_63}</digest>
    </supportedVgpu>
    <supportedVgpu vgpuId="64">
      <maxVgpus>4</maxVgpus>
      <digest type="signature">{sig_64}</digest>
    </supportedVgpu>
  </pgpu>
  <vgpuType id="63" name="GRID P4-1Q" class="Quadro">
    <devId vendorId="0x10de" deviceId="0x1BB3" subsystemVendorId="0x10de" subsystemId="0x1204" />
    <profileSize>0x40000000</profileSize>
    <framebuffer>0x38000000</framebuffer>
    <fbReservation>0x8000000</fbReservation>
    <numHeads>4</numHeads>
    <display width="5120" height="2880" />
    <maxPixels>18432000</maxPixels>
    <mappableVideoSize>0x400000</mappableVideoSize>
    <frlConfig>0x3c</frlConfig>
    <cudaEnabled>1</cudaEnabled>
    <eccSupported>1</eccSupported>
    <multiVgpuSupported>0</multiVgpuSupported>
    <encoderCapacity>0x64</encoderCapacity>
    <bar1Length>0x100</bar1Length>
    <frame_rate_limiter>1</frame_rate_limiter>
    <gpuInstanceSize>0</gpuInstanceSize>
    <gpuDirectSupported>1</gpuDirectSupported>
    <nvlinkP2PSupported>0</nvlinkP2PSupported>
    <multiVgpuExclusive>1</multiVgpuExclusive>
    <extraParameters>vgpu_extra=1</extraParameters>
    <license>QVDWS;VWS;VWSExt</license>
  </vgpuType>
  <vgpuType id="64" name="GRID P4-2Q" class="Quadro">
    <devId vendorId="0x10de" deviceId="0x1BB3" subsystemVendorId="0x10de" subsystemId="0x1205" />
    <profileSize>0x80000000</profileSize>
    <framebuffer>0x74000000</framebuffer>
    <fbReservation>0xC000000</fbReservation>
    <numHeads>4</numHeads>
    <display width="7680" height="4320" />
    <maxPixels>36864000</maxPixels>
    <mappableVideoSize>0x400000</mappableVideoSize>
    <frlConfig>0x3c</frlConfig>
    <cudaEnabled>1</cudaEnabled>
    <eccSupported>1</eccSupported>
    <multiVgpuSupported>0</multiVgpuSupported>
    <encoderCapacity>0x64</encoderCapacity>
    <bar1Length>0x100</bar1Length>
    <frame_rate_limiter>1</frame_rate_limiter>
    <gpuInstanceSize>0</gpuInstanceSize>
    <gpuDirectSupported>1</gpuDirectSupported>
    <nvlinkP2PSupported>0</nvlinkP2PSupported>
    <multiVgpuExclusive>1</multiVgpuExclusive>
    <extraParameters>vgpu_extra=1</extraParameters>
    <license>QVDWS;VWS;VWSExt</license>
  </vgpuType>
</vgpuconfig>"#
        )
    }

    #[test]
    fn test_ioctl_and_vmiop_abi_constants_match_disassembly() {
        assert_eq!(ctl_ioctl::SET_TYPES_INFO, 0xc010_7201);
        assert_eq!(ctl_ioctl::GET_EVENT_DATA, 0xc010_7204);
        assert_eq!(ctl_ioctl::SET_EVENT_DATA, 0xc010_7205);
        assert_eq!(ctl_ioctl::UNREGISTER, 0xc010_7207);

        assert_eq!(dev_ioctl::VERSION_CHECK, 0xc010_7101);
        assert_eq!(dev_ioctl::VF_REG_ACCESS_HW, 0xc010_7115);
        assert_eq!(dev_ioctl::BAR_INFO_SET_DATA, 0xc010_7117);

        assert_eq!(VgpuDeviceMmapRegion::Console.pgoff_base(), 1u64 << 40);
        assert_eq!(VgpuDeviceMmapRegion::Migration.pgoff_base(), 2u64 << 40);
        assert_eq!(VMIOP_PLUGIN_STRUCT_BYTES, 0xa8);
        assert_eq!(VMIOP_PLUGIN_HEADER_V5, 0x0005_0000_0000_00a8);
    }

    #[test]
    fn test_xml_config_and_nva081_packing() {
        let store = VgpuConfigStore::parse_xml(&sample_p4_xml()).unwrap();
        assert!(!store.global.heterogeneous_multi_vgpu);
        assert_eq!(store.global.plugin_so_name, "libnvidia-vgpu");

        assert!(store.find_pgpu_exact(0x10de, 0x1bb3, 0x10de, 0x11d8).is_some());

        let pairs = store.supported_profiles_for_gpu(0x10de, 0x1bb3).unwrap();
        assert_eq!(pairs.len(), 2);
        assert_eq!(pairs[0].1.name, "GRID P4-1Q");
        assert_eq!(pairs[0].1.dev_id.subsystem_id, 0x1204);

        let packed = Nva081SetInfoParams::from_profile_for_pgpu(
            pairs[0].1,
            pairs[0].0,
            0x1bb3,
            false,
            true,
        );
        assert_eq!(packed.raw[0], 1);
        assert_eq!(u32::from_le_bytes(packed.raw[0x008..0x00c].try_into().unwrap()), 63);
        assert_eq!(packed.raw[0x08c], 0x2f);
        let lic_end = packed.raw[0x10c..0x18c].iter().position(|&b| b == 0).unwrap();
        assert_eq!(
            std::str::from_utf8(&packed.raw[0x10c..0x10c + lic_end]).unwrap(),
            "Quadro-Virtual-DWS,5.0;GRID-Virtual-WS,2.0;GRID-Virtual-WS-Ext,2.0"
        );
        assert_eq!(u64::from_le_bytes(packed.raw[0x1c0..0x1c8].try_into().unwrap()), 0x1bb3);
        let extra_end = packed.raw[0x388..0x1388].iter().position(|&b| b == 0).unwrap();
        assert_eq!(
            std::str::from_utf8(&packed.raw[0x388..0x388 + extra_end]).unwrap(),
            "vgpu_extra=1"
        );
        assert_eq!(u32::from_le_bytes(packed.raw[0x138c..0x1390].try_into().unwrap()), 1);
        assert_eq!(u32::from_le_bytes(packed.raw[0x1398..0x139c].try_into().unwrap()), 1);
        assert_eq!(u32::from_le_bytes(packed.raw[0x1538..0x153c].try_into().unwrap()), 1);
    }

    #[test]
    fn test_vgpu_manager_lifecycle_bar0_vrpc_vmmu_chidmgr() {
        let store = VgpuConfigStore::parse_xml(&sample_p4_xml()).unwrap();
        let mut mgr = PascalVgpuManager::new(store, 0x10de, 0x1bb3, false).unwrap();

        let type_infos = mgr.build_vfio_type_infos().unwrap();
        assert_eq!(type_infos.len(), 2);
        assert_eq!(type_infos[0].profile_size_mb, 1024);

        // Create first P4-2Q (id=64)
        let id1 = mgr
            .create_device(64, "11111111-2222-3333-4444-555555555555")
            .unwrap();
        assert_eq!(mgr.available_instances(64).unwrap(), 3); // 4 * 2048 MiB = 8192 MiB; 3 remaining
        // Homogeneous constraint blocks mixing P4-1Q (63) while P4-2Q (64) is active.
        assert_eq!(mgr.available_instances(63).unwrap(), 0);
        assert_eq!(
            mgr.create_device(63, "22222222-2222-3333-4444-555555555555")
                .unwrap_err(),
            VgpuError::HeterogeneousConflict(64, 63)
        );

        let inst = mgr.open_device(id1).unwrap();
        assert!(inst.is_open);

        // Verify virtual PCI config space identity & BAR probing
        assert_eq!(inst.handle_reg_access(VgpuEmulSpace::ConfigSpace, 0x00, 4, false, 0).unwrap(), 0x1bb3_10de);
        assert_eq!(inst.handle_reg_access(VgpuEmulSpace::ConfigSpace, 0x2c, 4, false, 0).unwrap(), 0x1205_10de);

        // Verify BAR0 traps: NV_PMC_BOOT_0, NV_PFB CSTATUS, NV_PGRAPH write guard + FS whitelist
        assert_eq!(inst.handle_reg_access(VgpuEmulSpace::Mmio, 0x000000, 4, false, 0).unwrap(), 0x1340_00a1);
        assert_eq!(inst.handle_reg_access(VgpuEmulSpace::Mmio, 0x10f20c, 4, false, 0).unwrap(), 0x740); // 1856 MiB
        assert_eq!(inst.handle_reg_access(VgpuEmulSpace::Mmio, 0x500c30, 4, false, 0).unwrap(), 0x000f_0000);
        assert_eq!(inst.handle_reg_access(VgpuEmulSpace::Mmio, 0x400100, 4, false, 0).unwrap(), 0);

        // Program NV_VGPU (0x8f000) SEND_RING and ring RPC doorbell
        inst.handle_reg_access(VgpuEmulSpace::Mmio, NV_VGPU_BAR0_BASE + nv_vgpu_reg::SEND_RING_LO, 4, true, 0x8000_0000).unwrap();
        inst.handle_reg_access(VgpuEmulSpace::Mmio, NV_VGPU_BAR0_BASE + nv_vgpu_reg::RPC_DOORBELL, 4, true, 1).unwrap();
        assert_eq!(inst.interrupts_injected, 1);

        // Verify VRPC message ordering: must send SetGuestSystemInfo (0x01) before GetStaticInfo (0x55)
        let mut msg = [0u8; 64];
        VrpcHeader::new_request(VrpcFuncId::GetStaticInfo, 16).encode(&mut msg).unwrap();
        assert_eq!(inst.vrpc.handle_message(&mut msg).unwrap(), 0x56);

        VrpcHeader::new_request(VrpcFuncId::SetGuestSystemInfo, 0).encode(&mut msg).unwrap();
        assert_eq!(inst.vrpc.handle_message(&mut msg).unwrap(), 0);

        VrpcHeader::new_request(VrpcFuncId::GetStaticInfo, 16).encode(&mut msg).unwrap();
        assert_eq!(inst.vrpc.handle_message(&mut msg).unwrap(), 0);
        let hdr = VrpcHeader::decode(&msg).unwrap();
        assert_eq!(hdr.signature, VRPC_SIGNATURE);
        assert_eq!(hdr.result, 0);
        let fb = u64::from_le_bytes(msg[VRPC_HEADER_BYTES..VRPC_HEADER_BYTES + 8].try_into().unwrap());
        assert_eq!(fb, 0x7400_0000);

        // Verify CHIDMGR, VMMU, and FRL
        let ch = inst.chidmgr.bind_channel(EngineKind::Gr, 5).unwrap();
        assert_eq!(inst.chidmgr.phys_for_guest(EngineKind::Gr, 5), Some(ch.phys_chid));
        inst.vmmu.pin_gfn(0x100, 0x4_0100);
        assert_eq!(inst.vmmu.translate(0x100), Some(0x4_0100));
        assert!(inst.frl.check_pacing(0, 10_000_000).is_err()); // 10ms < 16.6ms (60 FPS)
        assert!(inst.frl.check_pacing(0, 20_000_000).is_ok());

        // Verify BAR_INFO report
        let bar_info = inst.bar_info_params();
        assert_eq!(bar_info.bar_sizes[0], 16 * 1024 * 1024);
        assert_eq!(bar_info.bar_sizes[1], 256 * 1024 * 1024);
        assert_eq!(bar_info.bar_sizes[3], 32 * 1024 * 1024);

        // Delete device and reclaim VRAM
        mgr.delete_device(id1).unwrap();
        assert_eq!(mgr.available_instances(63).unwrap(), 8);
    }

    #[test]
    fn test_clean_room_guest_driver_and_host_loopback() {
        use crate::guest::{
            decode_ring_pte, encode_ring_pte_hi, encode_ring_pte_lo, HostLoopbackTransport,
            NovaGuestDriver,
        };

        // Ring PTE encoding matches _nv013341rm / _nv013262rm
        let sample_gpa = 0x0000_0001_2345_6000u64;
        let lo = encode_ring_pte_lo(sample_gpa, false);
        let hi = encode_ring_pte_hi(sample_gpa);
        assert_eq!(lo & 0xfff, 0xb);
        assert_eq!(decode_ring_pte(lo, hi), sample_gpa);
        assert_eq!(encode_ring_pte_lo(sample_gpa, true) & 0xfff, 0x80b);

        let store = VgpuConfigStore::parse_xml(&sample_p4_xml()).unwrap();
        let mut mgr = PascalVgpuManager::new(store, 0x10de, 0x1bb3, false).unwrap();
        let id = mgr
            .create_device(64, "33333333-4444-5555-6666-777777777777")
            .unwrap();
        let mut host_inst = mgr.open_device(id).unwrap().clone();

        // Bare-metal check: if VGPU_CONFIG bit 0 is 0, guest refuses to attach
        host_inst.bar0.vgpu_config = 0;
        let mut bare_metal = HostLoopbackTransport::new(host_inst.clone());
        assert!(NovaGuestDriver::probe_and_init(&mut bare_metal, false).is_err());

        // Virtualized vGPU check: full probe + ring init + VRPC handshake + static info + allocs
        host_inst.bar0.vgpu_config = 0x201 | (1 << 18);
        let mut transport = HostLoopbackTransport::new(host_inst);
        let mut guest = NovaGuestDriver::probe_and_init(&mut transport, false).unwrap();
        assert_eq!(guest.caps.pmc_boot0, 0x1340_00a1);
        assert_eq!(guest.caps.virtual_fb_bytes, 0x7400_0000);
        assert!(guest.caps.ecc_supported);

        // Pre-handshake RPC is rejected with 0x56 (NV_ERR_NOT_SUPPORTED)
        let (pre_st, _, _, _) = guest.get_static_info(&mut transport).unwrap();
        assert_eq!(pre_st, 0x56);

        // Initialize guest VGX version, then issue GetStaticInfo535 + AllocRoot + RmApiControl
        assert_eq!(guest.set_guest_system_info(&mut transport, 535, 309).unwrap(), 0);
        assert!(guest.vgx_initialized);
        let (st, fb_bytes, dev_id, subsys_id) = guest.get_static_info(&mut transport).unwrap();
        assert_eq!(st, 0);
        assert_eq!(fb_bytes, 0x7400_0000);
        assert_eq!(dev_id, 0x1bb3);
        assert_eq!(subsys_id, 0x1205);

        let (root_st, _) = guest
            .call_vrpc(&mut transport, VrpcFuncId::AllocRoot, &[0u8; 16])
            .unwrap();
        let (ctl_st, _) = guest
            .call_vrpc(&mut transport, VrpcFuncId::RmApiControl, &[0u8; 24])
            .unwrap();
        assert_eq!(root_st, 0);
        assert_eq!(ctl_st, 0);
        assert!(transport.host.interrupts_injected >= 4);

        guest.shutdown(&mut transport).unwrap();
        mgr.delete_device(id).unwrap();
    }
}
