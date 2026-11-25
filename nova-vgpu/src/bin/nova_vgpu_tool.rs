//! Safe inspection & verification CLI for `nova-vgpu` on a Pascal KVM host.
//!
//! Never writes to BAR0 registers or unloads active drivers; performs read-only PCI sysfs
//! inspection, parses `vgpuConfig.xml`, validates extracted GP104 Falcon blobs using
//! `nova_vgpu::falcon_blobs`, and runs an end-to-end virtual `GP104` vGPU simulation.

use nova_vgpu::falcon_blobs::{
    parse_gr_netlist_toc, FlcnBlDesc, GrNetlistSection, LsFalconId, LsfSigInfo, RtosDesc,
    LS_SIGNATURE_BYTES,
};
use nova_vgpu::bar0_emul::{nv_vgpu_reg, NV_VGPU_BAR0_BASE};
use nova_vgpu::config::{Nva081SetInfoParams, VgpuConfigStore};
use nova_vgpu::vfio_ioctl::VgpuEmulSpace;
use nova_vgpu::vmiop::PascalVgpuManager;
use nova_vgpu::vmmu::EngineKind;
use nova_vgpu::vrpc::{VrpcFuncId, VrpcHeader, VRPC_HEADER_BYTES};
use std::fs;
use std::path::Path;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let xml_path = args
        .get(1)
        .map(String::as_str)
        .unwrap_or("/usr/share/nvidia/vgpu/vgpuConfig.xml");
    let blobs_dir = args
        .get(2)
        .map(String::as_str)
        .unwrap_or("/home/zero/vgpu-work/gp104-vgpu");

    println!("=== 1. Read-Only VM PCI Sysfs Check (0000:01:00.0) ===");
    probe_sysfs_pci("/sys/bus/pci/devices/0000:01:00.0");

    println!("\n=== 2. Parse vgpuConfig.xml ({}) ===", xml_path);
    let xml = fs::read_to_string(xml_path).expect("failed to read vgpuConfig.xml");
    let store = VgpuConfigStore::parse_xml(&xml).expect("failed to parse vgpuConfig.xml");
    println!(
        "Global: heterogeneous_multi_vgpu={}, plugin_so={}",
        store.global.heterogeneous_multi_vgpu, store.global.plugin_so_name
    );

    let mut mgr = PascalVgpuManager::new(store.clone(), 0x10de, 0x1bb3, false)
        .expect("Tesla P4 (10de:1bb3) missing in vgpuConfig.xml");
    let types = mgr.build_vfio_type_infos().expect("build_vfio_type_infos");
    println!("Tesla P4 (10de:1bb3) supported vGPU types: {}", types.len());
    for (sup, prof) in store.supported_profiles_for_gpu(0x10de, 0x1bb3).unwrap() {
        let packed = Nva081SetInfoParams::from_profile(prof, sup, false, true);
        println!(
            "  id={:<3} {:<12} class={:<6} subsys=0x{:04x} profile={}MiB fb={}MiB res={}MiB max={} cuda={} ecc={} frl={}fps bar1={}MiB nva081_bytes={}",
            prof.id,
            prof.name,
            prof.class_name,
            prof.dev_id.subsystem_id,
            prof.profile_size >> 20,
            prof.framebuffer >> 20,
            prof.fb_reservation >> 20,
            sup.max_vgpus,
            prof.cuda_enabled as u8,
            prof.ecc_supported as u8,
            prof.frl_config,
            prof.bar1_length_mb,
            packed.raw.len()
        );
    }

    println!("\n=== 3. Verify GP104 Falcon Blobs via nova-pascal ({}) ===", blobs_dir);
    verify_falcon_blobs(Path::new(blobs_dir));

    println!("\n=== 4. End-to-End Clean-Room Rust GP104 vGPU Lifecycle Simulation ===");
    let vgpu_id = mgr
        .create_device(64, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
        .expect("create GRID P4-2Q");
    let inst = mgr.open_device(vgpu_id).expect("open vGPU");
    let boot0 = inst.handle_reg_access(VgpuEmulSpace::Mmio, 0x000000, 4, false, 0).unwrap();
    let fb_mb = inst.handle_reg_access(VgpuEmulSpace::Mmio, 0x10f20c, 4, false, 0).unwrap();
    inst.handle_reg_access(VgpuEmulSpace::Mmio, NV_VGPU_BAR0_BASE + nv_vgpu_reg::RPC_DOORBELL, 4, true, 1).unwrap();

    let mut rpc_buf = [0u8; 64];
    VrpcHeader::new_request(VrpcFuncId::SetGuestSystemInfo, 0).encode(&mut rpc_buf).unwrap();
    let st_init = inst.vrpc.handle_message(&mut rpc_buf).unwrap();
    VrpcHeader::new_request(VrpcFuncId::GetStaticInfo, 16).encode(&mut rpc_buf).unwrap();
    let st_info = inst.vrpc.handle_message(&mut rpc_buf).unwrap();
    let guest_fb = u64::from_le_bytes(rpc_buf[VRPC_HEADER_BYTES..VRPC_HEADER_BYTES + 8].try_into().unwrap());
    let ch = inst.chidmgr.bind_channel(EngineKind::Nvenc, 0).unwrap();
    inst.vmmu.pin_gfn(0x2000, 0x10_2000);
    println!(
        "Simulated GRID P4-2Q (vgpu_id={}): NV_PMC_BOOT_0=0x{:08x}, virtual_fb={}MiB, vrpc_init={}, vrpc_info={} (fb=0x{:x}), nvenc_ch={}->{}, gfn0x2000->0x{:x}, irqs={}",
        vgpu_id,
        boot0,
        fb_mb,
        st_init,
        st_info,
        guest_fb,
        ch.guest_chid,
        ch.phys_chid,
        inst.vmmu.translate(0x2000).unwrap(),
        inst.interrupts_injected
    );
    mgr.delete_device(vgpu_id).unwrap();
    println!("Cleaned up vGPU {}; remaining P4-2Q capacity = {}.", vgpu_id, mgr.available_instances(64).unwrap());
}

fn probe_sysfs_pci(dev_dir: &str) {
    let p = Path::new(dev_dir);
    if !p.exists() {
        println!("Sysfs device {} not present on this host.", dev_dir);
        return;
    }
    for attr in ["vendor", "device", "subsystem_vendor", "subsystem_device", "current_link_speed", "current_link_width"] {
        if let Ok(val) = fs::read_to_string(p.join(attr)) {
            println!("  {}: {}", attr, val.trim());
        }
    }
    if let Ok(drv) = fs::read_link(p.join("driver")) {
        println!("  active_driver: {}", drv.file_name().unwrap_or_default().to_string_lossy());
    }
}

fn verify_falcon_blobs(root: &Path) {
    let sig_checks = [
        ("pmu/gp104_pmu_ls_sig.bin", LsFalconId::Pmu),
        ("sec2/gp104_sec2_rtos/entry_02_id_0x00.bin", LsFalconId::Sec2),
        ("nvenc/gp104_nvenc_ls_sig.bin", LsFalconId::Nvenc),
        ("gr/gp104_fecs_ls_sig.bin", LsFalconId::Fecs),
        ("gr/gp104_gpccs_ls_sig.bin", LsFalconId::Gpccs),
    ];
    for (rel, expected) in sig_checks {
        let bytes = fs::read(root.join(rel)).expect(rel);
        assert_eq!(bytes.len(), LS_SIGNATURE_BYTES);
        let parsed = LsfSigInfo::parse(&bytes).expect(rel);
        assert_eq!(parsed.falcon_id, expected);
        println!("  LS sig OK: {} -> {:?}", rel, parsed.falcon_id);
    }

    let pmu_bl = fs::read(root.join("pmu/gp104_pmu_bl/entry_01_id_0x00.bin")).unwrap();
    let bl_desc = FlcnBlDesc::parse(&pmu_bl).unwrap();
    println!("  PMU BL desc: boot_vec=0x{:x}, code_size={}", bl_desc.boot_vec, bl_desc.code_size);

    let sec2_desc_bytes = fs::read(root.join("sec2/gp104_sec2_rtos/entry_01_id_0x00.bin")).unwrap();
    let sec2_desc = RtosDesc::parse(&sec2_desc_bytes).unwrap();
    println!("  SEC2 RTOS desc: app_entry=0x{:x}, overlays={}", sec2_desc.app_entry, sec2_desc.num_overlays);

    let netlist = fs::read(root.join("gr/gp104_gr_netlist.bin")).unwrap();
    let mut sections = [GrNetlistSection { section_type: 0, size: 0, offset: 0 }; 64];
    let cnt = parse_gr_netlist_toc(&netlist, &mut sections).unwrap();
    println!("  GR Netlist TOC: {} sections (total {} bytes)", cnt, netlist.len());
}
