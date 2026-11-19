# 04 — 2-VM 50/50 (`GRID P4-4Q`) Pure Wayland, Windows 11 WDDM & Selkies/PixelFlux 10-bit 4:4:4 Deployment Guide

## 1. Host & 50/50 2-VM Architecture (`192.168.1.24`, UEFI Coreboot)

Following the BIOS upgrade to **UEFI Coreboot (`df45ea2c49b4-dirty` on Gigabyte `H61M-S1 PLUS`)**, the physical Tesla P4 KVM host runs at **`192.168.1.24`** (`br0`) and divides its 8 CPU threads and 8 GiB Tesla P4 GPU **50/50** across two production VMs:

| Machine | LAN IP | CPU / RAM / vGPU Profile | OS, Driver & Active Services |
| :--- | :--- | :--- | :--- |
| **KVM Host (`h61`)** | `192.168.1.24` (`br0`) | Intel Xeon E3-1275 V2 (`4C/8T`, `x86-64-v2`), `14 GiB` RAM, Tesla P4 (`8192 MiB`) | Ubuntu 26.04 LTS (`7.0.0-43-generic`), `535.309.01-vgpu-kvm`, Intel VT-d (`DMAR`) active, persistent `/etc/netplan/00-installer-config.yaml` (`br0` pinned to `192.168.1.24/24`), `vgpu-vms.service` enabled on boot |
| **`vm-win11`** | `192.168.1.65` | `4 vCPU` (50%), `6144 MiB` RAM, **`GRID P4-4Q` (`nvidia-65`, `4096 MiB` VRAM, 50% GPU)** | Windows 11 24H2 Pro (UEFI + Secure Boot + `swtpm` TPM 2.0), Microsoft inbox AHCI (`cache=none`) + `e1000e`, WHQL **`539.72` (`WDDM 3.1`)** bound to `SUBSYS_120610DE`, `Microsoft Basic Display Adapter` disabled, GPU-Z `2.71.0`, NVIDIA Control Panel `8.1.969.0`, hardware `AVC444` RDP (`:3389`) |
| **`vm-alma`** | `192.168.1.68` | `4 vCPU` (50%), `5120 MiB` RAM, **`GRID P4-4Q` (`nvidia-65`, `4096 MiB` VRAM, 50% GPU)** | AlmaLinux 10.3 (`6.12.0-264.16.1.el10_3.x86_64_v2`), **Pure Wayland GNOME `49.5` (`--no-x11`, zero host X11/Xwayland)** + R620 EGL/GBM overlay + hardware `AVC444` RDP (`:3389`), PLUS **Docker `29.10.0` + NVIDIA Container Toolkit `1.20.2` + Selkies `2.2.1` + LinuxServer.io PixelFlux `2.2.3`** (`https://192.168.1.68:3001`, **`NVENC H265 4:4:4 10-bit`** & **`NVENC H264 4:4:4 8-bit`**) |

---

## 2. Issues Ironed Out After UEFI Coreboot & 50/50 `GRID P4-4Q` Upgrade

1. **Missing `br0` Bridge & DHCP Client-ID Drift on `.24`:**
   - `/etc/netplan/00-installer-config.yaml` previously only configured `eno0`. Moving `eno0` into `br0` under `systemd-networkd` changed the DHCPv4 IAID and caused the router to assign `.26`/`.27`.
   - **Fix:** Configured `br0` statically at `192.168.1.24/24` with gateway `192.168.1.1` and MAC `d0:50:99:7c:84:dd` in `/etc/netplan/00-installer-config.yaml`, and enabled `/etc/systemd/system/vgpu-vms.service` so both VMs start automatically on host boot.
2. **Host `dmesg` `NVRM: API mismatch (580.178.04 vs 535.309.01)`:**
   - Residual `580.178.04` libraries in `/usr/lib/x86_64-linux-gnu/` were being loaded by `nvidia-cdi-refresh.service` against the `535.309.01-vgpu-kvm` kernel module.
   - **Fix:** Removed leftover `580.178.04` host `.so` files and symlinked `/usr/bin/nvidia-smi` and `/usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1` to `/opt/nvidia-vgpu-535/`.
3. **Windows 11 `GRID P4-4Q` (`SUBSYS_120610DE`), GPU-Z & NVIDIA Control Panel:**
   - Switching `vm-win11` from `GRID P4-1Q` (`SUBSYS_120410DE`) to `GRID P4-4Q` (`SUBSYS_120610DE`) exposed a new PCI Subsystem ID that remained an unbound `Video Controller (VGA Compatible)` while `Microsoft Basic Display Adapter` (`PCI\VEN_1234&DEV_1111`) took over as primary display.
   - **Fix:** Bound `nvgridsw.inf` from DriverStore to `PCI\VEN_10DE&DEV_1BB3&SUBSYS_120610DE` (`pnputil /add-driver ... /install`), removed the phantom `1Q` device, disabled `Microsoft Basic Display Adapter` (`Disable-PnpDevice`), and added a Desktop shortcut for the AppX **NVIDIA Control Panel (`NVIDIACorp.NVIDIAControlPanel_8.1.969.0`)**. Both **TechPowerUp GPU-Z 2.71.0** (`4096 MB GDDR5`, DirectX 12 `12_1`, OpenGL `4.6`, Vulkan, CUDA, OpenCL, DirectCompute, DirectML) and **NVIDIA Control Panel (`539.72 — GRID P4-4Q`)** open cleanly over RDP.
4. **AlmaLinux 10.3 (`x86_64_v2`) Docker + NVIDIA Container Toolkit + Selkies/PixelFlux `10-bit` & `4:4:4`:**
   - Upstream `el10` Docker/NCT binaries carry an `.note.gnu.property` `x86-64-v3` ELF marker that glibc rejects on Ivy Bridge (`x86-64-v2`), and `535.309.01` leaves old `libnvidia-egl-gbm.so.1.1.0` / `libnvidia-egl-wayland.so.1.1.11` files alongside R620's `1.1.3` / `1.1.20`.
   - **Fix ([`scripts/setup_selkies_pixelflux_vgpu.sh`](../scripts/setup_selkies_pixelflux_vgpu.sh)):** Removes superseded `1.1.0`/`1.1.11` EGL files, strips `.note.gnu.property` from container binaries, installs an `x86-64-v2`-clean `/usr/bin/runc`, generates `/etc/cdi/nvidia.yaml` & `/var/run/cdi/nvidia.yaml`, and launches [`docker-compose.yml`](../docker-compose.yml) with `SELKIES_ENCODER=h265enc,h264enc`, `SELKIES_VIDEO_FULLCOLOR=true`, `SELKIES_VIDEO_10BIT=true`, and `GBM_BACKENDS_PATH=/usr/lib64/gbm:/usr/lib/x86_64-linux-gnu/gbm`.
