# 06 — R580 Limitations, Feature Matrix & Live Verification Report

This document summarizes verified live results on the **UEFI Coreboot (`df45ea2c49b4-dirty`) Tesla P4 (`GP104GL`, 8 GiB VRAM) KVM host (`192.168.1.24`, `580.196.09-vgpu-kvm`)** running the **2-VM 50/50 (`2× GRID P4-4Q` / `nvidia-65`, 4096 MiB VRAM each)** R580 split (`580.196.07` AlmaLinux 10.3 + `582.99` Windows 11 24H2), X11-free pure Wayland GNOME, Selkies/PixelFlux `10-bit 4:4:4` via NVIDIA CDI, and Pascal GP104 hardware boundaries.

---

## 1. Live R580 2-VM 50/50 (`GRID P4-4Q`) Verification Summary

| Machine | LAN IP & Ports | CPU / RAM / vGPU | Driver Stack | Verified Live Capabilities |
| :--- | :--- | :--- | :--- | :--- |
| **Host (`h61`)** | `192.168.1.24` (`br0`) | Xeon E3-1275 V2 (`4C/8T`), `14 GiB` RAM, Tesla P4 (`8032 / 8192 MiB`) | Ubuntu 26.04 LTS (`7.0.0-44-generic`), `580.196.09-vgpu-kvm` (`0xc85b7` bypass + `0x11D8` XML + `/opt/nvidia-vgpu-580/` open daemons) | `open-nvidia-vgpud`, `open-nvidia-vgpu-mgr` (`0x98` B `SET_VM_NAME`), `open-nvidia-xid-logd`, `fastapi-dls:latest` (`2.0.3`) on `:443`, `2×` active `vgpu` processes (`4000 MiB` FB each) |
| **`vm-alma`** | `192.168.1.68:3389` (RDP)<br>`https://192.168.1.68:3001` (Selkies) | `4 vCPU` (50%), `5120 MiB` RAM, `GRID P4-4Q` (`4096 MiB`) | AlmaLinux 10.3 (`6.12.0-264.16.1.el10_3.x86_64_v2`), **`580.196.07-grid`** (`nv-pci.c` `10de:1bb3` patch, `-m=kernel`), X11-stripped **Pure Wayland GNOME 49.5** | `License Status : Licensed` (via `gridd-unlock-patcher`), **CUDA `13.0` (`13000`)**, **NVENC `13.0`**, zero `nvidia-settings`/`nvidia-xconfig`/Xorg/Xwayland processes, **`selkies-webtop` healthy in CDI mode (`mode = "cdi"`): `[pixelflux] Render node 0 encodes H264, H265 on nvenc`** |
| **`vm-win11`** | `192.168.1.74:3389` (DHCP)<br>`192.168.1.74:5985` (WinRM) | `4 vCPU` (50%), `6144 MiB` RAM, `GRID P4-4Q` (`4096 MiB`) | Windows 11 24H2 Pro, **`582.99` WHQL** (`32.0.15.8299`, `WDDM 3.1`) | Bound natively to `SUBSYS_11D810DE`, `Microsoft Basic Display Adapter` disabled, **`License Status : Licensed`** (DriverStore `nvxdapix.dll` `9594600` B patched via `gridd-unlock-patcher`), **CUDA `13.0`**, **NVENC**, NLA RDP `:3389` listening without `0x200d` |

---

## 2. X11-Stripped Pure Wayland + NVIDIA Container Toolkit CDI Verification

- **Host OS X11 Status on `vm-alma`**: Zero `Xorg` / `Xwayland` processes running; `gnome-shell --no-x11 --headless` active; `/usr/bin/nvidia-settings`, `/usr/bin/nvidia-xconfig`, `/usr/share/applications/nvidia-settings.desktop`, `nvidia_drv.so`, `libglxserver_nvidia.so*`, `libnvidia-gtk{2,3}.so*`, `20_nvidia_{xcb,xlib}.json`, `09_nvidia_wayland2.json`, and `libnvidia-egl-{xcb,xlib,wayland2}.so*` removed; `/usr/share/egl/egl_external_platform.d/` retains strictly `10_nvidia_wayland.json` and `15_nvidia_gbm.json`.
- **Docker / Selkies Container Runtime**:
  - With `/usr/bin/nvidia-xconfig` stripped, `/etc/nvidia-container-runtime/config.toml` MUST use `mode = "cdi"` (`nvidia-ctk config --in-place --set nvidia-container-runtime.mode=cdi`), with `/etc/cdi/nvidia.yaml` copied to `/var/run/cdi/nvidia.yaml`.
  - Recreated `selkies-webtop` (`docker compose up -d --force-recreate`) reports `[pixelflux] Render node 0 encodes H264, H265 on nvenc.` on `580.196.07` (`10-bit` HEVC `4:4:4` and `8-bit` H.264 `4:4:4`).

---

## 3. Known R580 & Pascal (`GP104`) Limits

1. **R580 Guest Allowlist (`580.196.07` `nv-pci.c`)**: NVIDIA removed `10de:1bb3` from `_nv054166rm`'s `.rodata` table in `580.196.07-grid`. [`tools/patch_r580_guest_nv_pci.py`](../tools/patch_r580_guest_nv_pci.py) applies a 2-line open-C bypass inside `nv_pci_probe()` while building `-m=kernel` (`kernel-open` is unsupported on Pascal GP104).
2. **Closed Licensing CA Pinning (`nvidia-gridd` / `nvxdapix.dll`)**: R580 closed guest licensing daemons pin NVIDIA's root CA. Patching with `gridd-unlock-patcher` against FastAPI-DLS `/-/config/root-certificate` works immediately on both AlmaLinux (`nvidia-gridd`) and Windows 11 (`582.99` `nvxdapix.dll` `9594600` B); `open-gridd` R580 `0x00800289` parity (`0x1f` return on `580.196.07`) remains tracked for future update.
3. **Windows 11 `582.99` (`WDDM 3.1`)**: Tesla P4 remains `WDDM 3.1` (`No HAGS`, no `MCDM`, no WSL2 GPU inside KVM). Always bind `SUBSYS_11D810DE` via `vgpuConfig.xml` to keep UEFI Secure Boot enabled, and clear `AVCHardwareEncodePreferred` / `AVC444ModePreferred` overrides to prevent RDP Error `0x200d`.
4. **Homogeneous vGPU Profiles & Hardware Silicon Limits**: Concurrent VMs on Pascal must share one profile size (`2× GRID P4-4Q`); GP104 has no SR-IOV, MIG, DLSS 2+, or hardware AV1 encode/decode.
5. **QEMU OVMF Windows Warm Reset**: If QEMU OVMF stalls on a Windows warm reboot, run a cold VM stop/start cycle.

---

## 4. Feature Comparison: Linux GNOME RDP vs. Windows 11 RDP

| Feature | Linux GNOME RDP (`gnome-remote-desktop` on `vm-alma`) | Windows 11 24H2 RDP (`TermService` / `RdpCoreTS` on `vm-win11`) |
| :--- | :--- | :--- |
| **Graphics Protocol** | `RDPGFX_CAPVERSION_107`, `RDS::Graphics` DVC | `RDPGFX_CAPVERSION_107`, `RDS::Graphics` DVC |
| **Hardware Video Codec** | H.264 **AVC444 & AVC420** via **CUDA `13.0` + NVENC** | H.264 **AVC444 & AVC420** via **`NVIDIA H.264 Encoder MFT`** |
| **Compositor Stack** | Pure Wayland (`gnome-shell --no-x11 --headless`) + PipeWire | Desktop Window Manager (`dwm.exe`) on WDDM `3.1` (`582.99`) |
| **Transport & Security** | TLS + NLA (CredSSP), TCP `:3389` | TLS + NLA (CredSSP), TCP + UDP `:3389` |
| **Companion Web Streaming (`10-bit 4:4:4`)** | **Selkies + PixelFlux** via NVIDIA CDI (`https://192.168.1.68:3001`, `NVENC H265/H264`) | N/A |
