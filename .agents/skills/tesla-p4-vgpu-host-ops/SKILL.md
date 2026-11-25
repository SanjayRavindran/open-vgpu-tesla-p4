---
name: tesla-p4-vgpu-host-ops
description: >-
  Manage and troubleshoot the UEFI Coreboot Tesla P4 KVM host (192.168.1.24),
  580.196.09-vgpu-kvm kernel modules, 50/50 GRID P4-4Q (nvidia-65) mdev split
  for vm-win11 (192.168.1.74 DHCP) and vm-alma (192.168.1.68), and the
  FastAPI-DLS license server (https://192.168.1.24:443). Use this skill
  whenever inspecting, restarting, or debugging the KVM host, vGPU mdev
  devices, open-vgpu-mgr, or vGPU licensing.
---

# Tesla P4 KVM vGPU Host & FastAPI-DLS Operations (`192.168.1.24`)

## Host Overview
- **Host IP & Bridge**: `192.168.1.24/24` on `br0` (`MAC d0:50:99:7c:84:dd`, gateway `192.168.1.1` in `/etc/netplan/00-installer-config.yaml`)
- **Firmware & Silicon**: UEFI Coreboot `df45ea2c49b4-dirty` on Gigabyte `H61M-S1 PLUS`, Intel Xeon E3-1275 V2 (`4C/8T`, `x86-64-v2`), Intel VT-d (`DMAR`) active, Above-4G 64-bit BAR1 (`0xfe0000000`)
- **Host Driver & Open Daemons**: DKMS `nvidia-vgpu-kvm/580.196.09` (`kernel/` closed-RM module + open MIT `nvidia-vgpu-vfio.ko`; **never** `kernel-open` on Pascal GP104) on Ubuntu 26.04 LTS (`7.0.0-43-generic` & `7.0.0-44-generic`, `/updates/dkms/{nvidia,nvidia-vgpu-vfio}.ko.zst`), module autoload `/etc/modules-load.d/nvidia-vgpu.conf`, userspace runtime in `/opt/nvidia-vgpu-580/` running all clean-room open-source host binaries/libraries (`open-nvidia-vgpud` with cold-boot `/dev/nvidiactl` mknod guard, `libopen-vgxcfg.so`, `open-nvidia-xid-logd`, `open-nvidia-vgpu-mgr` with R580 `0x98`-byte `SET_VM_NAME` / `vmIdType = 1U`, `open-nvidia-smi`, `libopen-nvidia-ml.so`, and `libopen-nvidia-vgpu.so` + `libopen-host-rm-gp104.so`).
- **Tesla P4 R580 Compatibility Details**:
  - `nv-kernel.o_binary` has the 11-byte `_nv060499rm` RSA signature validation bypass applied at file offset `0xc85b7` (`.text + 0xc8577`) so Tesla P4 (`10de:1bb3`) profiles load cleanly.
  - `/usr/share/nvidia/vgpu/vgpuConfig.xml` defines all 12 `GRID P4-*` profiles with `subsystemId="0x11D8"` (`tools/patch_r580_vgpu_config_xml.py`) so Windows `582.99` WHQL `nvgridsw.inf` binds natively without breaking Secure Boot.
  - `/opt/nvidia-vgpu-580/lib/libnvidia-vgpu.so` must symlink to `libnvidia-vgpu.so.580.196.09` by default, and `nvidia-vgpu-mgr.service` should set only `Environment=LD_LIBRARY_PATH=/opt/nvidia-vgpu-580/lib` unless hybrid VRPC tracing (`OPEN_VGPU_PLUGIN_LIB` + `OPEN_VGPU_VENDOR_LIB`) is intentionally enabled.
- **Guest Driver Staging Folder on `.24`**: `/home/zero/guest/` contains all R580 guest installers (`NVIDIA-Linux-x86_64-580.196.07-grid.run`, `.deb`, `.rpm`, `582.99...exe`) and `/home/zero/guest/gridd-unlock-patcher/`.
- **Active 50/50 VMs (`vgpu-vms.service` -> `/home/zero/vms/start_all_vms.sh`)**:
  - `vm-alma` (`192.168.1.68`): `4 vCPU`, `5120M` RAM, mdev `11111111-0000-0000-0000-000000000003` (`nvidia-65` / `GRID P4-4Q`, `4096M` VRAM, `580.196.07-grid`)
  - `vm-win11` (`192.168.1.74` via DHCP; `.65` had LAN MAC conflict): `4 vCPU`, `6144M` RAM, mdev `11111111-0000-0000-0000-000000000005` (`nvidia-65` / `GRID P4-4Q`, `4096M` VRAM, `582.99` WHQL)

## Procedures

### 0. DKMS & Clean Host Reproduction
- Check DKMS status and module paths:
  ```bash
  dkms status
  modinfo -n nvidia nvidia_vgpu_vfio
  ```
- Note: When unloading `nvidia` cleanly on `.24`, stop `h61-fan-control.service` first (`sudo systemctl stop h61-fan-control.service`) because it holds `/dev/nvidiactl` & `/dev/nvidia0` open, then restart it after `modprobe nvidia nvidia_vgpu_vfio`.
- If an mdev slot leaks (`available_instances = 0` after destroying mdev while `nvidia-vgpu-mgr` was stopped), reload `nvidia_vgpu_vfio` and `nvidia`:
  ```bash
  sudo systemctl stop nvidia-vgpu-mgr
  sudo modprobe -r nvidia_vgpu_vfio nvidia
  sudo modprobe nvidia nvidia_vgpu_vfio
  sudo systemctl restart nvidia-vgpud nvidia-vgpu-mgr
  ```

### 1. Verify Host vGPU Kernel Modules & Userspace Paths
1. Ensure `/usr/bin/nvidia-smi`, `/usr/bin/nvidia-vgpud`, `/usr/bin/nvidia-vgpu-mgr`, and `/usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1` point to `/opt/nvidia-vgpu-580/`:
   ```bash
   sudo ln -sf /opt/nvidia-vgpu-580/nvidia-smi /usr/bin/nvidia-smi
   sudo ln -sf /opt/nvidia-vgpu-580/lib/libnvidia-ml.so.580.196.09 /usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1
   echo "/opt/nvidia-vgpu-580/lib" | sudo tee /etc/ld.so.conf.d/nvidia-vgpu-580.conf
   sudo ldconfig
   ```
2. Verify `nvidia-vgpud.service`, `nvidia-vgpu-mgr.service`, and `nvidia-xid-logd.service` are active and `nvidia-smi vgpu -s` lists all 12 `GRID P4-*` profiles. Optional live VRPC/BAR0 tracing uses `OPEN_VGPU_PLUGIN_LIB=libopen-nvidia-vgpu.so` + `OPEN_VGPU_VENDOR_LIB=/opt/nvidia-vgpu-580/lib/libnvidia-vgpu.so.580.196.09` + `OPEN_VGPU_TRACE_DIR=/tmp/vgpu-trace` via a transient drop-in under `/run/systemd/system/nvidia-vgpu-mgr.service.d/` (run `sync` after binary installs; never use `/proc/self/mem` on vGPU mmap pages).

### 2. Verify or Restart the 50/50 2-VM Split
- Pascal GP104 requires **homogeneous** vGPU profile sizes across concurrent VMs (`2× nvidia-65` / `GRID P4-4Q`).
- Check status:
  ```bash
  sudo systemctl status vgpu-vms.service
  ls -la /sys/bus/mdev/devices/
  nvidia-smi
  nvidia-smi vgpu -s
  ```
- Re-run `/home/zero/vms/start_all_vms.sh` if either VM is stopped.

### 3. FastAPI-DLS License Server (`https://192.168.1.24:443`)
- **Compose Stack**: `/opt/fastapi-dls/docker-compose.yml` (`collinwebdesigns/fastapi-dls:latest`, v2.x supporting vGPU `17.x`/`18.x`/`19.x` R580, `DLS_URL: 192.168.1.24`, `DLS_PORT: 443`)
- **Verification**:
  ```bash
  curl --insecure -s https://192.168.1.24/-/health
  curl --insecure -s "https://192.168.1.24/-/origins?leases=true"
  ```
- **Endpoints for Guests**:
  - Client token: `https://192.168.1.24/-/client-token`
  - Root CA (for `gridd-unlock-patcher`): `https://192.168.1.24/-/config/root-certificate`
