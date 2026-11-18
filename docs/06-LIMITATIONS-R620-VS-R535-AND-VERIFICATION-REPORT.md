# 06 — Limitations, R620 vs. R535 Hybrid Comparison, RDP Feature Matrix & Live Verification Report

This document summarizes the verified live test results across **5 concurrent vGPU VMs** on a single **NVIDIA Tesla P4 (`GP104GL`, 8 GiB VRAM)** KVM host (`535.309.01-vgpu-kvm`, five `nvidia-63` / `GRID P4-1Q` profiles), our target **CUDA 12.1 / 12.2 + NVENC 12.1** baseline, all known limitations compared to NVIDIA R620 (`620.70.06` Linux / `621.14` Windows, Release 21.0), and the complete Linux GNOME RDP vs. Windows 11 RDP feature matrix.

---

## 1. Live Test Environment & Benchmarks Performed

All 5 VMs were booted simultaneously on `192.168.1.63` (Intel Xeon E3-1275 V2, Ubuntu 26.04 LTS, Linux `7.0.0-43-generic`, QEMU `10.2.1`, host `535.309.01-vgpu-kvm`, `5032 / 8192 MiB` allocated) and tested over bridged LAN (`br0`) using FreeRDP `3.22.0` compiled with H.264/AVC444 support.

| VM | Guest OS & Kernel | Desktop / Driver | X11 / Xwayland | Verified 3D / Compute / Encode / Decode Results |
| :--- | :--- | :--- | :--- | :--- |
| **`vm-ubuntu`** | Ubuntu 24.04 (`6.8.0-149-generic`) | GNOME `46.0` + GRD `46.3`<br>`535.309.01` + R620 EGL/GBM overlay | **0 running** (`--no-x11`, `Xwayland -> /bin/false`, `DISPLAY` unset) | EGL `1.5`, GL `4.6.0`, GLES `3.2`, Vulkan `1.3.242`, **`glmark2-wayland` (`800x600`, 3 scenes) score `678`** (`501–960 FPS`), CUDA `12.2` (`cuInit=0`), NVENC `12.1` + `ffmpeg h264_nvenc` PASS, NVDEC + `ffmpeg h264_cuvid` PASS, GRD CUDA + AVC444 LIVE (`185 MiB` VRAM) |
| **`vm-debian`** | Debian 13.7 (`6.12.117+deb13-amd64`) | GNOME `48.7` + GRD `48.1`<br>`535.309.01` + R620 EGL/GBM overlay | **0 running** (`--no-x11`, `Xwayland -> /bin/false`, `DISPLAY` unset) | EGL `1.5`, GL `4.6.0`, GLES `3.2`, Vulkan `1.3.242`, **`glmark2-wayland` (`800x600`, 3 scenes) score `663`** (`616–730 FPS`), CUDA `12.2`, NVENC `12.1` + `ffmpeg h264_nvenc` PASS, NVDEC + `ffmpeg h264_cuvid` PASS, GRD CUDA + AVC444 LIVE (`185 MiB` VRAM) |
| **`vm-alma`** | AlmaLinux 10.3 (`6.12.0-264.16.1.el10_3.x86_64_v2`) | GNOME `49.5` + GRD `49.3`<br>`535.309.01` + R620 EGL/GBM overlay | **0 running** (`xorg-x11-server-Xwayland` removed, `--no-x11`) | Vulkan `1.3.242` (`vkcube --wsi wayland --c 200`: `2575 ms` / `~77.7 FPS`), CUDA `12.2`, NVENC `12.1` (`CreateInstance=0`), full Pascal NVDEC matrix PASS, GRD CUDA + AVC444 LIVE (`181 MiB` VRAM) |
| **`vm-arch`** | Arch Linux (`7.3.4-arch1-2`) | GNOME `51.1` + GRD `51.0`<br>`535.309.01` + R620 EGL/GBM overlay | **0 running** (`xorg-xwayland` removed, no `/usr/bin/Xwayland`, `--no-x11`) | EGL `1.5`, GL `4.6.0`, Vulkan `1.3.242` (`vkcube --wsi wayland --c 200`: `8142 ms`), CUDA `12.2`, NVENC `12.1` (`CreateInstance=0`), NVDEC + `ffmpeg h264_cuvid` PASS, GRD AVC444 LIVE (`116–136 MiB` VRAM) |
| **`vm-win11`** | Windows 11 24H2 Pro (UEFI + Secure Boot + TPM 2.0) | Official WHQL `539.72` GRID (`WDDM 3.1`, `31.0.15.3972`) | N/A | D3D11/D3D12 (FL `12_1`), `vulkan-1.dll 1.3.300.0`, CUDA `12.2`, NVENC `12.1` (`CreateInstance=0`), full Pascal NVDEC matrix PASS (`H264/HEVC/VP8/VP9/MPEG2/VC1/JPEG`), **Windows RDP Event `170`: `NVIDIA H.264 Encoder MFT` AVC444 LIVE** (`2%` guest / `4%` host encoder utilization) |

**Verified Pascal GP104 NVDEC Hardware Decode Capabilities (Linux & Windows 11):**
- Supported: `H.264` (`4096×4096`), `HEVC` (`8192×8192`), `VP8` (`4096×4096`), `VP9` (`8192×8192`), `MPEG2` (`4080×4080`), `VC1` (`2032×2032`), `JPEG` (`32768×16384`).
- Unsupported in Pascal hardware: `AV1` (`sup=0`).

---

## 2. What Works vs. What Does NOT Work Compared to Native R620 (`620.70.06` / `621.14`, Release 21.0)

NVIDIA dropped Pascal (`GP104`) from R620 (`620.70.04` host), and the `535.309.01` host vGPU Manager enforces a strict VGX RPC protocol window (`0x70001`–`0x120001`), rejecting newer guest drivers at init. Keeping the `535.309.01` Linux / `539.72` Windows core driver (targeting **CUDA 12.1 / 12.2** and **NVENC 12.1**) while overlaying R620's Wayland EGL/GBM external platform libraries (`libnvidia-egl-wayland 1.1.20`, `libnvidia-egl-wayland2 1.0.2`, `libnvidia-egl-gbm 1.1.3`) plus patching GRD's PTX header (`8.7 -> 8.0`) delivers full Pure Wayland GNOME + hardware RDP acceleration with the following boundaries:

### What Works
- **Pure Wayland GNOME (`46`–`51`) with zero X11/Xwayland** across Ubuntu 24.04, Debian 13, AlmaLinux 10.3, and Arch Linux (`7.3` kernel) — broader distro coverage than R620's official RHEL 10 / Ubuntu matrix.
- **R620 Wayland EGL/GBM platform bridge** (`wayland620.tgz`) running on top of `535.309.01`.
- **Hardware-accelerated H.264 AVC444/AVC420 RDP** on both Linux (`gnome-remote-desktop` CUDA + NVENC) and Windows 11 (`NVIDIA H.264 Encoder MFT`).
- **OpenGL `4.6.0` / EGL `1.5` / GLES `3.2`**, **Direct3D 11 & 12 (`FL 12_1`, `WDDM 3.1`)**, **Vulkan `1.3.242`**, **CUDA `12.1`–`12.2` (PTX `≤8.2`)**, **NVENC `12.1`**, and **NVDEC**.

### Known Limitations (What Does NOT Work Compared to R620)
1. **CUDA `12.3`–`13.5` / PTX `8.3+`:** `535.309.01` / `539.72` cap at CUDA `12.2` (max PTX `8.2`). Workloads should target **CUDA 12.1 / 12.2** (or downgrade PTX headers via `scripts/patch_grd_ptx_535.sh`).
2. **NVENC API `13.x` Clients (Arch `ffmpeg 8`):** `535` exposes NVENC `12.1`; bleeding-edge Arch Linux `ffmpeg 8` requires NVENC `≥13.1`, so `ffmpeg -c:v h264_nvenc` fails on Arch (while `h264_cuvid` decode, GRD AVC444, and Ubuntu/Debian `ffmpeg` `h264_nvenc` all pass).
3. **Vulkan `1.4.x` (`1.4.357`):** `535` userspace ICD is capped at Vulkan `1.3.242`.
4. **Kernel `nvidia-drm` `fbdev=1` & DRM Syncobj Timeline Explicit Sync:** `535.309.01` `nvidia-drm` only supports `modeset=1` and lacks R620's `drm_fbdev_*` and `drm_nvidia_semsurf_export_to_syncobj_point_params` / `drm_nvidia_syncobj_get_syncfd_params` ioctls.
5. **Windows HAGS, MCDM, and WSL2 GPU in KVM:** `539.72` leaves Hardware-Accelerated GPU Scheduling (`HAGS`) as `AlwaysOff / Enabled: False`, does not support MCDM (Microsoft Compute Driver Model; Pascal stays on `WDDM 3.1`, which remains Microsoft's active graphics/display model — MCDM replaces NVIDIA's legacy TCC compute-only mode, not WDDM), and does not support WSL2 GPU passthrough inside a KVM Windows guest.
6. **Homogeneous vGPU Profiles Only on Pascal:** Unlike R620 mixed-size time-sliced vGPUs (`2Q + 4Q` simultaneously) or fractional multi-vGPU, Pascal requires identical profile sizes across concurrent VMs on the same GPU (e.g., all 5 VMs on `GRID P4-1Q` / `nvidia-63`).
7. **No Console VNC over Wayland:** R620 adds QEMU console VNC with Wayland; on `535`, VMs run with `vfio-pci,...,display=off -vga std` and Wayland desktops are accessed over RDP.
8. **No vGPU Live Migration with CUDA Profilers / GPU Reset Notifications:** R620-only host/guest protocol extensions.
9. **Pascal GP104 Hardware Limits:** No SR-IOV, no MIG-backed vGPU, no Unified Memory (Linux), no DLSS 2+ (no Tensor cores), and no hardware AV1 encode/decode.
10. **Unlicensed vGPU Timer:** Without an NVIDIA DLS/CLS license server, `535.309.01` / `539.72` transitions from `Unlicensed (Unrestricted)` to `Unlicensed (Restricted)` after ~20 minutes (throttling frame rate/compute until licensed or rebooted).
11. **Inert Distro X11 Packaging Dependencies:** Zero X11/Xwayland processes run on any Linux VM (`X11_RUNNING=0`, `DISPLAY` unset, `gnome-shell --no-x11`), though shared libraries such as `libX11` remain on disk to satisfy distro package dependencies.
12. **Windows 11 `virtio-win 0.1.302` Stability Caveat:** Bleeding-edge `virtio-win 0.1.302` (`viostor.sys` + `netkvm.sys`) with `cache=writeback` and BitLocker caused silent file corruption and `0xFC` BSODs on QEMU `10.2.1` / Linux `7.0`; `vm-win11` uses Microsoft inbox **AHCI (`cache=none`) + `e1000e`** with BitLocker disabled for zero corruption and zero BSODs.

---

## 3. Observed Feature Comparison: Linux GNOME RDP vs. Windows 11 RDP

| Feature | Linux GNOME RDP (`gnome-remote-desktop` 46.3–51.0) | Windows 11 24H2 RDP (`TermService` / `RdpCoreTS`) |
| :--- | :--- | :--- |
| **Graphics Protocol** | `RDPGFX_CAPVERSION_107` (`0x000A0701`), `RDS::Graphics` DVC | `RDPGFX_CAPVERSION_107` (`0x000A0701`), `RDS::Graphics` DVC (`Initial profile: 2048`) |
| **Hardware Video Codec** | H.264 **AVC444 & AVC420** via **CUDA + NVENC** (`~116–185 MiB` VRAM) | H.264 **AVC444 & AVC420** via **`NVIDIA H.264 Encoder MFT`** (Event `170`, dynamic QP `22–38`) |
| **Compositor Stack** | Pure Wayland (`gnome-shell --no-x11` / Mutter) + PipeWire virtual monitor | Desktop Window Manager (`dwm.exe`) on WDDM `3.1` (`bEnumerateHWAdapters=1`) |
| **Transport & Security** | TLS + NLA (CredSSP), TCP `:3389` | TLS + NLA (CredSSP), TCP + UDP (`SelectTransport=2`), `:3389` |
| **Dynamic Resize & Multi-Monitor (`disp`)** | Supported (`RDS::DisplayControl`) | Supported (`RDS::DisplayControl`, configured up to 4 monitors / `5120×2880` / 32-bit) |
| **Audio Playback / Input** | Supported (`AUDIO_PLAYBACK_DVC` / PipeWire) | Supported (`AUDIO_PLAYBACK_DVC` + `LOSSY_DVC` + `audin`) |
| **Clipboard (`cliprdr`)** | Supported (text, images, FUSE file copy/paste) | Supported (text, rich formats, FUSE file copy/paste) |
| **Drive / Printer / Smartcard (`rdpdr`)** | Not implemented in GRD | Supported (`rdpdr` negotiated) |
| **Camera & Media Redirection** | Not implemented in GRD | Supported (`RDCamera_Device_Enumerator`, `RDS::Video::Control/Data`, `Geometry` DVCs) |
| **Hardware Cursor & Touch/Pen** | Standard RDP cursor + `libei`/Mutter input | Dedicated `RDS::MouseCursor`, `CoreInput`, `rdpinpt` DVCs |

---

## 4. Automated & Live Test Suite Summary

| Test Suite | Path | Scope |
| :--- | :--- | :--- |
| **Rust `nova-vgpu` Unit Tests** | `nova-vgpu/src/*.rs` (`cargo test`) | `vgpuConfig.xml` parser (12 P4 profiles), `NVA081` packing, `vfio-mdev` ioctls, `vmiop` lifecycle, virtual PCI/BAR0 `0x8f000`, `"VRPC"` RPC ring, `VMMU`, `CHIDMGR`, `FRL`, guest driver, Falcon blob validator |
| **ELF64 Bindata Analyzer Tests** | `tests/test_nv_bindata_analyzer.py` | Clean-room `.rodata` bindata parser & compression tests |
| **Falcon Descriptor Parser Tests** | `tests/test_nv_fw_descriptor_parser.py` | GP104 Falcon firmware descriptor & netlist unpacker verification |
| **Clean-Room Hygiene & Manifest Tests** | `tests/test_vgpu_manifest_and_tools.py` | Verifies ZERO proprietary NVIDIA binaries in git, validates `BLOB_MANIFEST.json` & extractor CLIs |
| **PTX Patcher, RDP & Script Tests** | `tests/test_grd_ptx_and_rdp_templates.py` | Verifies `patch_grd_ptx_535.sh` downgrade + idempotency, `.rdp` profiles (zero embedded passwords, AVC444 LAN flags), `bash -n` syntax, and `DISCLAIMER.md` completeness |
| **Guest IOCTL C Smoke Test** | `kmods/nova_vgpu_guest/test_guest_ioctl.c` | Userspace ioctl smoke test for clean-room `nova_vgpu_guest` kernel module |
| **Live Linux Wayland Harness** | `tests/live_verify_linux_wayland_vgpu.sh` | Runs inside Linux VMs to audit zero-X11, `nvidia-smi`, CUDA/NVENC/NVDEC, and GRD AVC444 logs |
| **Live Windows 11 Harness** | `tests/live_verify_win11_vgpu.ps1` | Runs inside Windows 11 VM to audit PnP status, `nvidia-smi`, CUDA/NVENC/NVDEC, and `RdpCoreTS` Event `170` |
