# Reproducible Ubuntu KVM & Proxmox VE 8/9 Tesla P4 vGPU Deployment Guide

This guide documents how to cleanly rebuild or migrate the exact 50/50 **Tesla P4 (`GRID P4-4Q` / `nvidia-65`)** dual-VM virtualization stack (**AlmaLinux 10.3 Wayland/RDP + Selkies** and **Windows 11 24H2 WDDM 3.1/RDP**) on either:
1. A fresh **Ubuntu 24.04 / 26.04 KVM** host, or
2. A fresh **Proxmox VE 8 / 9** hypervisor (either creating fresh VMs or migrating existing `.qcow2` disks from Ubuntu KVM).

---

## 1. Host Prerequisites (Ubuntu KVM & Proxmox VE)

Enable IOMMU and UEFI in BIOS / GRUB (`intel_iommu=on iommu=pt` on Xeon E3-1275 V2 + UEFI Coreboot), ensuring no stale `nouveau` binding:

- **Ubuntu 24.04 / 26.04 KVM**:
  Install KVM/QEMU/OVMF/TPM tools alongside kernel headers (`dkms build-essential linux-headers-$(uname -r) qemu-system-x86 ovmf swtpm mdevctl`).
- **Proxmox VE 8 / 9**:
  Install Proxmox headers and DKMS (`apt-get install -y dkms build-essential pve-headers-$(uname -r) mdevctl`).

---

## 2. Clean Host DKMS Packaging & Open-Source Host Daemons

### Option A: One-Shot Proxmox VE 8 / 9 Host Configurator (`scripts/install_proxmox_vgpu_host.sh`)
On a **Proxmox VE 8 / 9** host, run [`scripts/install_proxmox_vgpu_host.sh`](../scripts/install_proxmox_vgpu_host.sh) to configure Proxmox repositories, `proxmox-headers-$(uname -r)`, GRUB & `proxmox-boot-tool` IOMMU parameters, `/etc/modules`, DKMS `535.309.01` + all 8 clean-room open-source host artifacts, native systemd FastAPI-DLS, and optional 50/50 VM creation in one command:

```bash
sudo bash scripts/install_proxmox_vgpu_host.sh \
  --run-file /path/to/NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run \
  --enable-no-subscription-repo \
  --dls-url <PVE_HOST_IP> \
  --setup-vms fresh
```

### Option B: Direct DKMS Installer (`scripts/install_vgpu_host_dkms.sh` — Ubuntu KVM or Proxmox VE)
Run [`scripts/install_vgpu_host_dkms.sh`](../scripts/install_vgpu_host_dkms.sh) on either Ubuntu KVM or Proxmox VE:

```bash
sudo bash scripts/install_vgpu_host_dkms.sh \
  --run-file /path/to/NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run
```

What [`install_vgpu_host_dkms.sh`](../scripts/install_vgpu_host_dkms.sh) does automatically:
1. Auto-detects **Proxmox VE 8 / 9** (`pveversion` -> `proxmox-headers-$(uname -r)` / `pve-headers-$(uname -r)` / `proxmox-default-headers` + `/proc/cmdline` IOMMU check for both GRUB and `proxmox-boot-tool`) vs **Ubuntu KVM** (`linux-headers-$(uname -r)`).
2. Auto-builds and installs our clean-room open-source host replacements into `/opt/nvidia-vgpu-535`:
   - `open-nvidia-vgpud` (`nova-vgpu` Rust release binary)
   - `libopen-vgxcfg.so` (`open-vgxcfg` C library, `9/9` exports)
   - `open-nvidia-xid-logd` (`open-xid-logd` C daemon)
   - `open-nvidia-vgpu-mgr` (`open-vgpu-mgr` C runtime daemon exporting all `60/60` `vmiop_*` symbols)
   - `open-nvidia-smi` + `libopen-nvidia-ml.so` (`open-nvml-smi` C telemetry CLI & NVML library)
   - `libopen-nvidia-vgpu.so` (`open-vgpu-plugin` C `VER_5_0`/`VER_2_0` `vmiop` mediator plugin implementing all `47/47` cataloged live VRPCs + `0x50`/`0x9c` static info + hybrid interposer wired via `OPEN_VGPU_PLUGIN_LIB`)
   - `libopen-host-rm-gp104.so` (`open-host-rm` C Tier 2B host GP104 physical RM HAL slice)
3. Removes any unmanaged `/lib/modules/$(uname -r)/extra/nvidia*.ko` duplicates and registers **`nvidia-vgpu-kvm/535.309.01`** cleanly under DKMS (`/usr/src/nvidia-vgpu-kvm-535.309.01/dkms.conf`), quoting `'make'` (`MAKE[0]="'make' -j$(nproc) KERNEL_UNAME=${kernelver} SYSSRC=${kernel_source_dir} modules"`) and running `dkms install --force` so DKMS installs signed `.ko.zst` modules into `/lib/modules/$(uname -r)/updates/dkms/` cleanly across kernel upgrades.
4. Replaces legacy bare-metal R580 configs (`/etc/modules-load.d/nvidia-p4.conf` and `nvidia-drm` bare-metal options) with `/etc/modprobe.d/blacklist-nouveau.conf` and `/etc/modules-load.d/nvidia-vgpu.conf` (`nvidia`, `nvidia-vgpu-vfio`, `vfio-pci`, `mdev`).
5. Installs clean systemd units:
   - `nvidia-vgpud.service` (`Type=oneshot`, `RemainAfterExit=yes`, `ExecStart=/opt/nvidia-vgpu-535/nvidia-vgpud`)
   - `nvidia-vgpu-mgr.service` (`Type=forking`, `Requires=nvidia-vgpud.service`)
   - `nvidia-xid-logd.service` (`Type=forking`, `RuntimeDirectory=nvidia-xid-logd`)

Verify before starting VMs:
```bash
dkms status
modinfo -n nvidia nvidia_vgpu_vfio
ls /sys/bus/pci/devices/0000:01:00.0/mdev_supported_types/
cat /sys/bus/pci/devices/0000:01:00.0/mdev_supported_types/nvidia-65/available_instances
```

---

## 3. FastAPI-DLS License Server (Docker or Native Systemd)

Use [`scripts/install_fastapi_dls.sh`](../scripts/install_fastapi_dls.sh):

- **Ubuntu KVM (Docker Engine)**:
  ```bash
  sudo bash scripts/install_fastapi_dls.sh --mode docker --dls-url 192.168.1.24 --port 443
  ```
- **Proxmox VE 8/9 (Native Systemd + Python Venv, No Docker on Host)**:
  ```bash
  sudo bash scripts/install_fastapi_dls.sh --mode systemd --dls-url <PVE_HOST_IP> --port 443
  ```

Verify:
```bash
curl --insecure -s https://<HOST_IP>/-/health
```

---

## 4. VM Creation & Migration (Ubuntu KVM or Proxmox VE 8/9)

### Option A: Ubuntu KVM
Launch the 50/50 split (`4 vCPU` + `GRID P4-4Q` / `nvidia-65` per VM) with [`scripts/start_vgpu_vms.sh`](../scripts/start_vgpu_vms.sh).

### Option B: Proxmox VE 8/9 (`setup_proxmox_vgpu_vms.sh`)
Both fresh VM creation and disk migration from Ubuntu `.qcow2` images are automated in [`scripts/setup_proxmox_vgpu_vms.sh`](../scripts/setup_proxmox_vgpu_vms.sh):

- **Migrate existing Ubuntu KVM `.qcow2` disks**:
  ```bash
  sudo bash scripts/setup_proxmox_vgpu_vms.sh \
    --mode import-qcow2 \
    --alma-disk /path/to/alma/disk.qcow2 \
    --win11-disk /path/to/win11/disk.qcow2 \
    --storage local-lvm --bridge vmbr0 --pci-bdf 0000:01:00.0
  ```
- **Create fresh Proxmox VMs**:
  ```bash
  sudo bash scripts/setup_proxmox_vgpu_vms.sh \
    --mode fresh --storage local-lvm --bridge vmbr0 --pci-bdf 0000:01:00.0
  ```

VM hardware profiles created by default:
| VMID | Name | vCPU | RAM | Firmware / Security | vGPU Mapping | NIC |
| :--- | :--- | :---: | :---: | :--- | :--- | :--- |
| `101` | `vm-alma` | `4` (`host`) | `5120 MiB` | OVMF (`pre-enrolled-keys=0`), Q35 | `0000:01:00.0,mdev=nvidia-65,pcie=1` | `virtio` (`52:54:00:11:22:03`) |
| `102` | `vm-win11` | `4` (`host,+hv-evmcs`) | `6144 MiB` | OVMF SecureBoot + TPM 2.0, Q35 | `0000:01:00.0,mdev=nvidia-65,pcie=1` | `e1000e` (`52:54:00:11:22:05`) |

---

## 5. Guest Provisioning (Multi-Distro Linux Guest & Windows 11 24H2)

- **Multi-Distro Linux Guest (`scripts/provision_linux_vgpu_guest.sh` — RHEL, AlmaLinux, Rocky, Oracle, CentOS, Ubuntu LTS, Debian)**:
  Auto-detects `/etc/os-release` across **Enterprise Linux (`dnf`/`yum`, `/usr/lib64`, `/etc/gdm/custom.conf`, `dracut -f`, `firewall-cmd`)** and **Ubuntu LTS / Debian (`apt-get`, `/usr/lib/x86_64-linux-gnu`, `/etc/gdm3/*.conf`, `update-initramfs -u`, `ufw`)**, installs `535.309.01` GRID kernel modules (with automatic Linux `6.12+`/`7.x+` compatibility patching) + overlays `620.70.06` Wayland/GBM libraries (`libnvidia-egl-gbm`, `libnvidia-egl-wayland`, `libnvidia-egl-wayland2`, `nvidia-drm_gbm.so`), sets `GSK_RENDERER=ngl` globally, enables pure Wayland `--no-x11 --headless` + user-session headless RDP on `:3389`, applies [`scripts/patch_grd_ptx_535.sh`](../scripts/patch_grd_ptx_535.sh), installs the FastAPI-DLS token, builds `libopen-nvidia-encode.so`, and optionally runs [`scripts/setup_selkies_pixelflux_vgpu.sh`](../scripts/setup_selkies_pixelflux_vgpu.sh):
  ```bash
  sudo bash scripts/provision_linux_vgpu_guest.sh \
    --dls-url https://<HOST_IP> \
    --grid-535-run /path/to/NVIDIA-Linux-x86_64-535.309.01-grid.run \
    --grid-620-run /path/to/NVIDIA-Linux-x86_64-620.70.06-grid.run \
    --rdp-user zero --rdp-pass '<PASSWORD>' --with-selkies
  ```
  *(Note: [`scripts/provision_alma_guest.sh`](../scripts/provision_alma_guest.sh) remains available as a backward-compatible wrapper around `provision_linux_vgpu_guest.sh`.)*

- **Windows 11 24H2 (`provision_win11_guest.ps1` + `scripts/templates/win11-autounattend.xml`)**:
  Use [`scripts/templates/win11-autounattend.xml`](../scripts/templates/win11-autounattend.xml) (substituting `__ADMIN_PASSWORD__`) for zero-prompt Win11 installation with WinRM NTLM and RDP pre-enabled, then run:
  ```powershell
  powershell -ExecutionPolicy Bypass -File scripts\provision_win11_guest.ps1 `
    -DlsUrl https://<HOST_IP> `
    -DriverInstallerPath C:\path\to\539.72_grid_win10_win11_64bit_international.exe
  ```
  The script installs the WDDM driver (`SUBSYS_120610DE` -> `GRID P4-4Q`), activates the FastAPI-DLS token, clears `AVCHardwareEncodePreferred` / `AVC444ModePreferred` policies that trigger RDP Error `0x200d` (`RDP_GFX 0x8007007A`), and enables NLA (`UserAuthentication=1`).

---

## 6. Host `.24` Cleanup Summary (`2026-11-21`)

Retired VMs (`/home/zero/vms/{arch,debian,ubuntu,work,images}`), stale bare-metal R580/P4 build backups (`/home/zero/{p4_work,r580-*,open-580-driver*}`), exited bare-metal containers (`jellyfin`, `selkies-webtop`) & unused Docker images, stale `/home/zero/vgpu535-build`, and unmanaged `/lib/modules/7.0.0-43-generic/extra/nvidia*.ko` duplicates were removed from `192.168.1.24`:
- **Root filesystem before cleanup**: `148G used / 74G avail (67%)`
- **Root filesystem after cleanup**: `100G used / 122G avail (46%)` — **48 GiB reclaimed** while keeping `vm-alma`, `vm-win11`, `fastapi-dls`, `wenv`, `freerdp3-h264`, and all unrelated hardware/router projects untouched.
