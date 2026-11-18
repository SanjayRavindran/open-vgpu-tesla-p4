use nova_vgpu::config::VgpuConfigStore;
use nova_vgpu::guest::{HostLoopbackTransport, NovaGuestDriver};
use nova_vgpu::vmiop::PascalVgpuManager;
use nova_vgpu::vrpc::VrpcFuncId;
use std::fs;

const LIVE_XML_PATH: &str = "/home/zero/vgpu-work/vgpu-kvm-535.309.01/vgpuConfig.xml";
const GUEST_DEV_PATH: &str = "/dev/nova-vgpu-guest0";

fn main() {
    println!("=== Clean-Room Open-Source Pascal vGPU Guest Driver Demo ===");
    println!("1. Signed Falcon / GSP firmware required by guest: NONE (0 bytes)");

    if let Ok(meta) = fs::metadata(GUEST_DEV_PATH) {
        println!(
            "Found live kernel device {GUEST_DEV_PATH} (len={})",
            meta.len()
        );
    } else {
        println!("   ({GUEST_DEV_PATH} not loaded on VM host; running Host<->Guest clean-room loopback)");
    }

    let xml = match fs::read_to_string(LIVE_XML_PATH) {
        Ok(s) => s,
        Err(_) => {
            println!("Skipping live XML load (file not at {LIVE_XML_PATH}).");
            return;
        }
    };

    let store = VgpuConfigStore::parse_xml(&xml).expect("parse vgpuConfig.xml");
    let mut mgr = PascalVgpuManager::new(store, 0x10de, 0x1bb3, false).expect("init host mgr");
    let vgpu_id = mgr
        .create_device(64, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
        .expect("create P4-2Q");
    let host_inst = mgr.open_device(vgpu_id).expect("open P4-2Q").clone();

    let mut transport = HostLoopbackTransport::new(host_inst);
    let mut guest = NovaGuestDriver::probe_and_init(&mut transport, false)
        .expect("guest probe_and_init");

    println!(
        "2. Guest probed virtual GP104: NV_PMC_BOOT_0=0x{:08x}, VGPU_CONFIG=0x{:08x}, FB={} MiB, ECC={}",
        guest.caps.pmc_boot0,
        guest.caps.vgpu_config,
        guest.caps.virtual_fb_bytes >> 20,
        guest.caps.ecc_supported
    );
    println!(
        "   Ring GPAs programmed @0x8f000..0x8f03c: SEND=0x{:x}, RECV=0x{:x}, EVENT=0x{:x}",
        guest.send_ring_gpa, guest.recv_ring_gpa, guest.event_ring_gpa
    );

    let handshake = guest
        .set_guest_system_info(&mut transport, 535, 309)
        .expect("SetGuestSystemInfo");
    let (static_st, fb_bytes, dev_id, subsys_id) = guest
        .get_static_info(&mut transport)
        .expect("GetStaticInfo535");
    let (root_st, _) = guest
        .call_vrpc(&mut transport, VrpcFuncId::AllocRoot, &[0u8; 16])
        .expect("AllocRoot");
    let (dev_st, _) = guest
        .call_vrpc(&mut transport, VrpcFuncId::AllocDevice535, &[0u8; 16])
        .expect("AllocDevice535");
    let (chan_st, _) = guest
        .call_vrpc(&mut transport, VrpcFuncId::AllocChannelDma, &[0u8; 32])
        .expect("AllocChannelDma");

    println!(
        "3. Guest <-> Host VRPC roundtrips: SetGuestSystemInfo={}, GetStaticInfo535={} (fb=0x{:x}, dev=0x{:04x}, subsys=0x{:04x}), AllocRoot={}, AllocDevice535={}, AllocChannelDma={}",
        handshake, static_st, fb_bytes, dev_id, subsys_id, root_st, dev_st, chan_st
    );
    println!(
        "   Stats: guest_rpcs={}, host_vrpc_processed={}, host_irqs_injected={}, pinned_gfns={}",
        guest.rpc_calls_completed,
        transport.host.vrpc.processed_count,
        transport.host.interrupts_injected,
        transport.host.vmmu.gfn_to_hfn.len()
    );

    guest.shutdown(&mut transport).expect("guest shutdown");
    mgr.delete_device(vgpu_id).expect("cleanup");
}
