---
name: proxmox-and-ubuntu-vgpu-repro
description: >-
  Reproducibly deploy or migrate the Tesla P4 50/50 GRID P4-4Q (nvidia-65)
  Windows 11 + AlmaLinux 10.3 vGPU stack on either a fresh Ubuntu KVM host or
  Proxmox VE 8/9, including DKMS packaging, FastAPI-DLS (Docker or systemd),
  VM creation/qcow2 migration, and automated guest provisioning. Use whenever
  setting up a fresh hypervisor or migrating VMs between Ubuntu KVM and Proxmox.
---

# Reproducible Ubuntu KVM & Proxmox VE 8/9 Tesla P4 vGPU Stack (`535.309.01-vgpu-kvm`)

## 1. Host DKMS + Open-Source Daemons (`Ubuntu KVM` OR `Proxmox VE 8/9`)
Both distributions run mainline Linux kernels with IOMMU (`intel_iommu=on iommu=pt`) and UEFI/OVMF.
The installer auto-detects `pveversion` (selecting `pve-headers-$(uname -r)`) vs Ubuntu (`linux-headers-$(uname -r)`), builds `nvidia-vgpu-kvm/535.309.01` under DKMS (`nvidia` + `nvidia-vgpu-vfio`), installs all 6 open-source host binaries (`open-nvidia-vgpud`, `libopen-vgxcfg.so`, `open-nvidia-xid-logd`, `open-nvidia-vgpu-mgr`, `open-nvidia-smi`, `libopen-nvidia-ml.so`), cleans stale R580 files, and enables the systemd services:
```bash
# Unified Host + FastAPI-DLS installer:
sudo bash scripts/install_open_vgpu.sh --role host \
  --run-file /path/to/NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run \
  --with-dls docker --dls-url <HOST_IP>

# Or direct DKMS installer:
sudo bash scripts/install_vgpu_host_dkms.sh \
  --run-file /path/to/NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run
```
Verify:
```bash
dkms status
modinfo -n nvidia nvidia_vgpu_vfio
ls /sys/bus/pci/devices/0000:01:00.0/mdev_supported_types/
```

## 2. FastAPI-DLS License Server
- **Ubuntu KVM (Docker Engine)**:
  ```bash
  sudo bash scripts/install_fastapi_dls.sh --mode docker --dls-url <HOST_IP> --port 443
  ```
- **Proxmox VE (native Python venv + systemd, no Docker needed on PVE host)**:
  ```bash
  sudo bash scripts/install_fastapi_dls.sh --mode systemd --dls-url <HOST_IP> --port 443
  ```

## 3. Create or Migrate the 50/50 VMs (`vm-alma` + `vm-win11`, `nvidia-65` = `GRID P4-4Q`)
- **Proxmox VE 8/9 fresh disks** (`VMID 101` = AlmaLinux 10.3 `4 vCPU / 5120M`, `VMID 102` = Windows 11 `4 vCPU / 6144M` with `OVMF` + `TPM 2.0` + `hostpci0 ...,mdev=nvidia-65,pcie=1`):
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
- **Ubuntu KVM native launch**: `sudo bash scripts/start_vgpu_vms.sh`

## 4. Guest Provisioning & Unattended Windows Install
- **AlmaLinux 10.3** (`535.309.01` kernel + `620.70.06` Wayland/GBM libs, `GSK_RENDERER=ngl`, headless Wayland `--no-x11 --headless`, GRD PTX patch, FastAPI-DLS token, optional Selkies 10-bit 4:4:4):
  ```bash
  sudo bash scripts/provision_alma_guest.sh \
    --dls-url https://<HOST_IP> \
    --grid-535-run /path/to/NVIDIA-Linux-x86_64-535.309.01-grid.run \
    --grid-620-run /path/to/NVIDIA-Linux-x86_64-620.70.06-grid.run \
    --rdp-user zero --rdp-pass '<PASS>' --with-selkies
  ```
- **Windows 11 24H2**: Use `scripts/templates/win11-autounattend.xml` (replace `__ADMIN_PASSWORD__`) for zero-prompt install, then run:
  ```powershell
  powershell -ExecutionPolicy Bypass -File scripts\provision_win11_guest.ps1 -DlsUrl https://<HOST_IP>
  ```
