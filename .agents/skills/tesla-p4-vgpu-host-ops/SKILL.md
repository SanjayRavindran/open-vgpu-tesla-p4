---
name: tesla-p4-vgpu-host-ops
description: >-
  Manage and troubleshoot the UEFI Coreboot Tesla P4 KVM host (192.168.1.24),
  535.309.01-vgpu-kvm kernel modules, 50/50 GRID P4-4Q (nvidia-65) mdev split
  for vm-win11 (192.168.1.65) and vm-alma (192.168.1.68), and the FastAPI-DLS
  license server (https://192.168.1.24:443). Use this skill whenever inspecting,
  restarting, or debugging the KVM host, vGPU mdev devices, or vGPU licensing.
---

# Tesla P4 KVM vGPU Host & FastAPI-DLS Operations (`192.168.1.24`)

## Host Overview
- **Host IP & Bridge**: `192.168.1.24/24` on `br0` (`MAC d0:50:99:7c:84:dd`, gateway `192.168.1.1` in `/etc/netplan/00-installer-config.yaml`)
- **Firmware & Silicon**: UEFI Coreboot `df45ea2c49b4-dirty` on Gigabyte `H61M-S1 PLUS`, Intel Xeon E3-1275 V2 (`4C/8T`, `x86-64-v2`), Intel VT-d (`DMAR`) active, Above-4G 64-bit BAR1 (`0xfe0000000`)
- **Host Driver & Open Daemons**: DKMS `nvidia-vgpu-kvm/535.309.01` on Ubuntu 26.04 LTS (`7.0.0-43-generic`, `/updates/dkms/{nvidia,nvidia-vgpu-vfio}.ko.zst`), module autoload `/etc/modules-load.d/nvidia-vgpu.conf`, userspace runtime in `/opt/nvidia-vgpu-535/` running all 6 clean-room open-source host binaries (`open-nvidia-vgpud`, `libopen-vgxcfg.so`, `open-nvidia-xid-logd`, `open-nvidia-vgpu-mgr`, `open-nvidia-smi`, `libopen-nvidia-ml.so`) alongside `libnvidia-vgpu.so.535.309.01`
- **Active 50/50 VMs (`vgpu-vms.service` -> `/home/zero/vms/start_all_vms.sh`)**:
  - `vm-alma` (`192.168.1.68`): `4 vCPU`, `5120M` RAM, mdev `11111111-0000-0000-0000-000000000003` (`nvidia-65` / `GRID P4-4Q`, `4096M` VRAM)
  - `vm-win11` (`192.168.1.65`): `4 vCPU`, `6144M` RAM, mdev `11111111-0000-0000-0000-000000000005` (`nvidia-65` / `GRID P4-4Q`, `4096M` VRAM)

## Procedures

### 0. DKMS & Clean Host Reproduction
- Check DKMS status and module paths:
  ```bash
  dkms status
  modinfo -n nvidia nvidia_vgpu_vfio
  ```
- Note: Do **not** restore removed bare-metal R580 configs (`/etc/modules-load.d/nvidia-p4.conf` or unmanaged `extra/nvidia*.ko`); recreate the full host stack cleanly via `scripts/install_vgpu_host_dkms.sh`.

### 1. Verify Host vGPU Kernel Modules & Avoid R580 vs. R535 API Mismatch
If `nvidia-smi` fails or `dmesg` reports `NVRM: API mismatch: the client has the version 580.178.04, but this kernel module has the version 535.309.01`:
1. Ensure no leftover `*.so.580.178.04` libraries exist in `/usr/lib/x86_64-linux-gnu/`.
2. Ensure `/usr/bin/nvidia-smi` and `/usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1` point to `/opt/nvidia-vgpu-535/`:
   ```bash
   sudo rm -f /usr/lib/x86_64-linux-gnu/*580.178.04*
   sudo ln -sf /opt/nvidia-vgpu-535/nvidia-smi /usr/bin/nvidia-smi
   sudo ln -sf /opt/nvidia-vgpu-535/libnvidia-ml.so.535.309.01 /usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1
   sudo ldconfig
   ```
3. Verify both `nvidia-vgpud.service` and `nvidia-vgpu-mgr.service` are active and two `vgpu` processes (`4000MiB` each) appear in `nvidia-smi`.

### 2. Verify or Restart the 50/50 2-VM Split
- Pascal GP104 requires **homogeneous** vGPU profile sizes across concurrent VMs (`2× nvidia-65` / `GRID P4-4Q`).
- Check status:
  ```bash
  sudo systemctl status vgpu-vms.service
  ls -la /sys/bus/mdev/devices/
  nvidia-smi
  ```
- Re-run `/home/zero/vms/start_all_vms.sh` if either VM is stopped.

### 3. FastAPI-DLS License Server (`https://192.168.1.24:443`)
- **Compose Stack**: `/opt/docker/fastapi-dls/docker-compose.yml` (`collinwebdesigns/fastapi-dls:latest`, `VERSION: 2.0.3`, `DLS_URL: 192.168.1.24`, `DLS_PORT: 443`)
- **Source Repo**: `/opt/fastapi-dls`
- **Verification**:
  ```bash
  curl --insecure -s https://192.168.1.24/-/health
  curl --insecure -s "https://192.168.1.24/-/origins?leases=true"
  ```
- **Client Token Download Endpoint**: `https://192.168.1.24/-/client-token`
