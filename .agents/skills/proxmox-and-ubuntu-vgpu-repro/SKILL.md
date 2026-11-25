---
name: proxmox-and-ubuntu-vgpu-repro
description: >-
  Reproducibly deploy or migrate the Tesla P4 50/50 GRID P4-4Q (nvidia-65)
  Windows 11 + AlmaLinux 10.3 vGPU stack (R580: 580.196.09 host / 580.196.07
  Linux guest / 582.99 Windows guest) on either a fresh Ubuntu KVM host or
  Proxmox VE 8/9, including DKMS packaging, XML SUBSYS_11D8 patching, FastAPI-DLS
  (Docker or systemd), VM creation/qcow2 migration, and automated R580 guest
  provisioning. Use whenever setting up a fresh hypervisor or migrating VMs.
---

# Reproducible Ubuntu KVM & Proxmox VE 8/9 Tesla P4 R580 vGPU Stack

## 1. Host DKMS + Open-Source Daemons (`Ubuntu KVM` OR `Proxmox VE 8/9`)
Both distributions run mainline Linux kernels with IOMMU (`intel_iommu=on iommu=pt`) and UEFI/OVMF.
`install_vgpu_host_dkms.sh` auto-detects `pveversion` vs Ubuntu, builds `nvidia-vgpu-kvm/580.196.09` from `kernel/` (with `_nv060499rm` RSA signature bypass at `0xc85b7`), patches `/usr/share/nvidia/vgpu/vgpuConfig.xml` via `tools/patch_r580_vgpu_config_xml.py` (`10de:1bb3`, `subsystemId="0x11D8"`), installs all clean-room open-source host artifacts into `/opt/nvidia-vgpu-580/` (`open-nvidia-vgpud`, `libopen-vgxcfg.so`, `open-nvidia-xid-logd`, `open-nvidia-vgpu-mgr` with `0x98`-byte `SET_VM_NAME`, `open-nvidia-smi`, `libopen-nvidia-ml.so`, `libopen-nvidia-vgpu.so`, `libopen-host-rm-gp104.so`), removes deprecated R535 libraries, and configures clean `systemd` units (`nvidia-vgpu-mgr.service` using `LD_LIBRARY_PATH=/opt/nvidia-vgpu-580/lib`):
```bash
# Dedicated One-Shot Proxmox VE 8/9 Host Configurator:
sudo bash scripts/install_proxmox_vgpu_host.sh \
  --run-file /path/to/NVIDIA-Linux-x86_64-580.196.09-vgpu-kvm.run \
  --enable-no-subscription-repo \
  --dls-url <HOST_IP> \
  --setup-vms fresh

# Unified Host + FastAPI-DLS installer (Ubuntu KVM or Proxmox VE):
sudo bash scripts/install_open_vgpu.sh --role host \
  --run-file /path/to/NVIDIA-Linux-x86_64-580.196.09-vgpu-kvm.run \
  --with-dls docker --dls-url <HOST_IP>

# Or direct DKMS installer:
sudo bash scripts/install_vgpu_host_dkms.sh \
  --run-file /path/to/NVIDIA-Linux-x86_64-580.196.09-vgpu-kvm.run
```
Verify:
```bash
dkms status
modinfo -n nvidia nvidia_vgpu_vfio
ls /sys/bus/pci/devices/0000:01:00.0/mdev_supported_types/
```

## 2. FastAPI-DLS License Server (v2.x, supports vGPU `17.x`/`18.x`/`19.x` R580)
- **Ubuntu KVM (Docker Engine)**:
  ```bash
  sudo bash scripts/install_fastapi_dls.sh --mode docker --dls-url <HOST_IP> --port 443
  ```
- **Proxmox VE (native Python venv + systemd, no Docker needed on PVE host)**:
  ```bash
  sudo bash scripts/install_fastapi_dls.sh --mode systemd --dls-url <HOST_IP> --port 443
  ```

## 3. Create or Migrate the 50/50 VMs (`vm-alma` + `vm-win11`, `nvidia-65` = `GRID P4-4Q`)
- **Proxmox VE 8/9 fresh disks**:
  ```bash
  sudo bash scripts/setup_proxmox_vgpu_vms.sh --mode fresh --storage local-lvm --bridge vmbr0
  ```
- **Proxmox VE 8/9 migration from Ubuntu KVM `.qcow2` disks**:
  ```bash
  sudo bash scripts/setup_proxmox_vgpu_vms.sh --mode import-qcow2 \
    --alma-disk /path/to/alma/disk.qcow2 \
    --win11-disk /path/to/win11/disk.qcow2 \
    --storage local-lvm --bridge vmbr0
  ```
- **Ubuntu KVM native launch**: `sudo bash /home/zero/vms/start_all_vms.sh`

## 4. Streamlined R580 Guest Driver Installation & Licensing (Staged in `.24:/home/zero/guest/`)
- **Linux Guests (`580.196.07-grid.run`)**: Automatically patches `kernel/nvidia/nv-pci.c` (`10de:1bb3`), builds `-m=kernel` DKMS, strips NVIDIA X11 artifacts (`--strip-x11` default), applies `gridd-unlock-patcher`, configures pure Wayland GNOME (`--no-x11 --headless`) + `GSK_RENDERER=ngl`, and configures Selkies CDI (`mode = "cdi"`):
  ```bash
  sudo bash scripts/provision_linux_vgpu_guest.sh \
    --dls-url https://<HOST_IP> \
    --grid-580-run /path/to/NVIDIA-Linux-x86_64-580.196.07-grid.run \
    --gridd-patcher /path/to/gridd-unlock-patcher \
    --rdp-user zero --rdp-pass '<YOUR_RDP_PASSWORD>' --with-selkies
  ```
- **Windows 11 24H2 (`582.99` WHQL)**: Use `scripts/templates/win11-autounattend.xml` (replace `__ADMIN_PASSWORD__`), pre-patch `Display.Driver/nvxdapix.dll` on the host with `gridd-unlock-patcher`, then run in elevated PowerShell:
  ```powershell
  powershell -ExecutionPolicy Bypass -File scripts\provision_win11_guest.ps1 `
    -DlsUrl https://<HOST_IP> `
    -DriverInstallerPath C:\path\to\582.99_grid_win10_win11_server2022_server_2025_dch_64bit_international.exe `
    -PatchedNvxdapixPath C:\path\to\nvxdapix_patched.dll
  ```
