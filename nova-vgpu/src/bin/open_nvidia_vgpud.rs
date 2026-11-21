use nova_vgpu::config::VgpuConfigStore;
use nova_vgpu::rm_ioctl::{detect_driver_version, run_vgpud_registration};
use std::env;
use std::fs;
use std::process;

fn main() {
    if let Err(err) = run() {
        eprintln!("open-nvidia-vgpud: {err}");
        process::exit(1);
    }
}

fn run() -> Result<(), String> {
    let args: Vec<String> = env::args().collect();
    let mut cfg_path = env::var("NV_VGPU_CFG_FILE")
        .ok()
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| String::from("/usr/share/nvidia/vgpu/vgpuConfig.xml"));
    let mut dry_run = false;

    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--dry-run" => dry_run = true,
            "--version" => {
                println!("open-nvidia-vgpud 0.1.0 (535.309.01-vgpu-kvm compatible)");
                return Ok(());
            }
            "--config" => {
                i += 1;
                if i >= args.len() {
                    return Err(String::from("--config requires a file path"));
                }
                cfg_path = args[i].clone();
            }
            "-h" | "--help" => {
                println!(
                    "Usage: open-nvidia-vgpud [--config <vgpuConfig.xml>] [--dry-run] [--version]"
                );
                return Ok(());
            }
            other => return Err(format!("unknown argument: {other}")),
        }
        i += 1;
    }

    let xml =
        fs::read_to_string(&cfg_path).map_err(|e| format!("failed to read {cfg_path}: {e}"))?;
    let store = VgpuConfigStore::parse_xml(&xml).map_err(|e| format!("{e:?}"))?;

    if dry_run {
        println!(
            "open-nvidia-vgpud dry-run: parsed {} pGPUs and {} profiles from {}",
            store.pgpus.len(),
            store.profiles.len(),
            cfg_path
        );
        if let Some(p4) = store.find_pgpu(0x10de, 0x1bb3) {
            println!("  Tesla P4 (10de:1bb3): {} supported profiles", p4.supported.len());
        }
        return Ok(());
    }

    #[cfg(not(unix))]
    return Err(String::from("/dev/nvidiactl RM registration is only supported on Linux"));

    #[cfg(unix)]
    {
        use nova_vgpu::rm_ioctl::linux::LinuxRmTransport;

        let proc_ver = fs::read_to_string("/proc/driver/nvidia/version").unwrap_or_default();
        let driver_ver = detect_driver_version(&proc_ver);
        let mut transport = LinuxRmTransport::open()?;
        let summary = run_vgpud_registration(&mut transport, &store, &driver_ver)?;
        for gpu in &summary.gpus {
            if let Some(reason) = &gpu.skipped_reason {
                println!(
                    "GPU {:04x}:{:02x}:{:02x}.0 ({:04x}:{:04x}) skipped: {}",
                    gpu.domain, gpu.bus, gpu.slot, gpu.vendor_id, gpu.device_id, reason
                );
            } else {
                println!(
                    "GPU {:04x}:{:02x}:{:02x}.0 ({:04x}:{:04x}) registered {} vGPU profiles (RM mode {}, driver {})",
                    gpu.domain,
                    gpu.bus,
                    gpu.slot,
                    gpu.vendor_id,
                    gpu.device_id,
                    gpu.profiles_registered,
                    gpu.virtualization_mode,
                    summary.driver_version
                );
            }
        }
        Ok(())
    }
}
