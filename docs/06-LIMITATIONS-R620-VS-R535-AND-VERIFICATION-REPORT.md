# 06 — Limitations, R620 vs. R535 Hybrid Comparison, RDP Feature Matrix, Selkies 10-bit 4:4:4 & Live Verification Report

This document summarizes the verified live test results on the **UEFI Coreboot (`df45ea2c49b4-dirty`) Tesla P4 (`GP104GL`, 8 GiB VRAM) KVM host (`192.168.1.24`, `535.309.01-vgpu-kvm`)** running the **2-VM 50/50 CPU (4 vCPU each) & 50/50 GPU (`2× GRID P4-4Q` / `nvidia-65`, 4096 MiB VRAM each)** split, **Selkies + LinuxServer.io PixelFlux `10-bit` & `4:4:4` (`YUV444`) hardware acceleration**, all known limitations compared to NVIDIA R620 (`620.70.06` / `621.14`, Release 21.0), and the complete Linux GNOME RDP vs. Windows 11 RDP feature matrix.

---

## 1. Live 2-VM 50/50 (`GRID P4-4Q`) Test Environment & Verified Results

| Machine | LAN IP & Ports | CPU / RAM / vGPU | Desktop / Driver Stack | Verified 3D, Compute, NVENC/NVDEC, RDP & Container Results |
| :--- | :--- | :--- | :--- | :--- |
| **Host (`h61`)** | `192.168.1.24` (`br0`) | Xeon E3-1275 V2 (`4C/8T`), `14 GiB` RAM, Tesla P4 (`8032 / 8192 MiB` allocated) | Ubuntu 26.04 LTS (`7.0.0-43-generic`), UEFI Coreboot, `535.309.01-vgpu-kvm` | Intel VT-d (`DMAR`) active, persistent `br0`, `vgpu-vms.service` enabled, two `GRID P4-4Q` (`4000 MiB` each) `vgpu` processes active, zero swap thrashing (`6.1 GiB` available host RAM) |
| **`vm-win11`** | `192.168.1.65:3389` (RDP)<br>`192.168.1.65:5985` (WinRM) | `4 vCPU` (50%), `6144 MiB` RAM, **`GRID P4-4Q` (`4096 MiB` VRAM, 50% GPU)** | Windows 11 24H2 Pro, WHQL **`539.72` (`WDDM 3.1`, `31.0.15.3972`)** | `SUBSYS_120610DE` bound (`Status: OK`), `Microsoft Basic Display Adapter` disabled, **TechPowerUp GPU-Z 2.71.0** verified (`NVIDIA GRID P4-4Q`, `GP104`, `2560` Shaders, `4096 MB GDDR5`, DirectX `12 (12_1)`, OpenGL `4.6`, Vulkan `1.3`, CUDA `12.2`, OpenCL, DirectCompute, DirectML), **NVIDIA Control Panel `8.1.969.0`** verified live over RDP, NVENC `12.1`, full Pascal NVDEC table, **Windows RDP Event `170`: `NVIDIA H.264 Encoder MFT` AVC444 LIVE** |
| **`vm-alma`** | `192.168.1.68:3389` (RDP)<br>`https://192.168.1.68:3001` (Selkies) | `4 vCPU` (50%), `5120 MiB` RAM, **`GRID P4-4Q` (`4096 MiB` VRAM, 50% GPU)** | AlmaLinux 10.3 (`6.12.0-264.16.1.el10_3.x86_64_v2`), **Pure Wayland GNOME `49.5` (`--no-x11`, zero host X11/Xwayland)** + `535.309.01` + R620 EGL/GBM overlay | EGL `1.5`, OpenGL `4.6.0`, Vulkan `1.3.242` (`vkcube --wsi wayland`: `~77.7 FPS`), CUDA `12.2`, NVENC `12.1`, full Pascal NVDEC table, GRD CUDA + `AVC444` RDP LIVE, PLUS **Docker `29.10.0` + NVIDIA Container Toolkit `1.20.2` + Selkies `2.2.1` + PixelFlux `2.2.3`** streaming zero-copy **`NVENC H265 4:4:4 10-bit` (`444-10`)** and **`NVENC H264 4:4:4 8-bit` (`444-8`)** |

---

## 2. NVIDIA Container Toolkit + Selkies & LinuxServer.io PixelFlux (`10-bit` & `4:4:4` Color Depth)

Verified live on `vm-alma` (`192.168.1.68`, `GRID P4-4Q`, `535.309.01` + R620 Wayland/GBM overlay) with `lscr.io/linuxserver/webtop:ubuntu-xfce`:

- **`selkies.gpu_probe` inside container:**
  ```text
  GPU: GRID P4-4Q/PCI/SSE2 on /dev/dri/renderD128
  SELKIES_GPU_RENDER_NODE=/dev/dri/renderD128
  SELKIES_GPU_DRIVER=nvidia
  SELKIES_GPU_PRESENT=true
  SELKIES_GPU_ACCELERATED=true
  SELKIES_GPU_GL_VENDOR=nvidia
  SELKIES_GPU_MESA_DRIVER=zink
  SELKIES_GPU_VULKAN_PRESENTS=true
  ```
- **`pixelflux` Hardware Codec & Chroma-Depth Matrix on `GRID P4-4Q`:**
  ```text
  encoders:  {'h264': 'nvenc', 'h265': 'nvenc'}
  fullcolor: ['h264', 'h265']
  formats:   {'h264': ['420-8', '444-8'], 'h265': ['420-8', '420-10', '444-8', '444-10']}
  ```
- **Live Zero-Copy DMA-BUF -> CUDA-Array -> NVENC `H265 4:4:4 10-bit` & `H264 4:4:4 8-bit` Capture Proof:**
  ```text
  [pixelflux] NVENC API version negotiated: 12.1
  [NVENC] H265 initialized (4:4:4 mode: true, chroma convert: kernel).
  [X11] Encoder: NVENC H265 4:4:4 10-bit on GRID P4-4Q (render node 0, nvidia driver), split as the driver picks, 2 engines.
  [X11] Zero-copy capture (DRI3): 1024x768 blitted by the X server into 3 GPU buffers on /dev/dri/renderD128, encoded in place on NVENC.
  [X11] Stream settings active -> Res: 1024x768 | FPS: 60.0 | Stripes: 1 | Mode: H265 (NVENC) FullFrame Streaming | CRF: 25 | Colorspace: I444 (Limited Range)
  [NVENC] dmabuf imported as a CUDA-array frame (1 planes, 1024x768, pitch 0, 4 channels of element format 1): encoding in place.

  [NVENC] H264 initialized (4:4:4 mode: true, chroma convert: hardware).
  [X11] Encoder: NVENC H264 4:4:4 8-bit on GRID P4-4Q (render node 0, nvidia driver), 2 engines.
  [X11] Stream settings active -> Res: 1024x768 | FPS: 60.0 | Stripes: 1 | Mode: H264 (NVENC) FullFrame Streaming | CRF: 25 | Colorspace: I444 (Limited Range)
  [NVENC] dmabuf imported as a CUDA-array frame (1 planes, 1024x768, pitch 0, 4 channels of element format 1): encoding in place.
  ```

---

## 3. What Works vs. What Does NOT Work Compared to Native R620 (`620.70.06` / `621.14`, Release 21.0)

### What Works
- **Pure Wayland GNOME (`49.5`, `--no-x11`, zero host X11/Xwayland)** with R620's Wayland EGL/GBM platform bridge (`libnvidia-egl-wayland 1.1.20`, `libnvidia-egl-wayland2 1.0.2`, `libnvidia-egl-gbm 1.1.3`) overlaid onto `535.309.01`.
- **Hardware-accelerated H.264 `AVC444`/`AVC420` RDP** on both Linux (`gnome-remote-desktop` CUDA + NVENC) and Windows 11 (`NVIDIA H.264 Encoder MFT`).
- **Selkies + LinuxServer.io PixelFlux `10-bit` & `4:4:4` (`YUV444`) container hardware acceleration** via NVIDIA Container Toolkit (`nvidia-ctk` CDI v0.7.0), encoding `NVENC H265 4:4:4 10-bit` (`444-10`) and `NVENC H264 4:4:4 8-bit` (`444-8`).
- **Windows 11 24H2 WDDM `3.1` (`539.72` WHQL)** with full **TechPowerUp GPU-Z 2.71.0**, **NVIDIA Control Panel `8.1.969.0`**, DirectX 11/12 (`FL 12_1`), OpenGL `4.6`, Vulkan `1.3`, OpenCL, DirectCompute, DirectML, CUDA `12.1`/`12.2`, NVENC `12.1`, and NVDEC (`H.264`, `HEVC` 8/10-bit, `VP8`, `VP9`, `MPEG2`, `VC1`, `JPEG`).

### Known Limitations Compared to Native R620
1. **CUDA `12.3`–`13.5` / PTX `8.3+`:** `535.309.01` / `539.72` cap at CUDA `12.2` (max PTX `8.2`). Target **CUDA `12.1` / `12.2`** (or downgrade PTX headers via `scripts/patch_grd_ptx_535.sh`).
2. **NVENC API `12.1` vs `13.x`:** `535` exposes NVENC `12.1`. PixelFlux `2.2.3` and GRD work natively with NVENC `12.1`; standalone CLI builds compiled strictly against NVENC headers `≥13.1` require an NVENC `12.1`-compatible build.
3. **Vulkan `1.3.242` vs `1.4.357`:** `535` userspace ICD is capped at Vulkan `1.3.242`.
4. **Kernel `nvidia-drm` `fbdev=1` & DRM Syncobj Timeline Explicit Sync:** `535.309.01` `nvidia-drm` supports `modeset=1` and lacks R620's `drm_fbdev_*` and `drm_nvidia_semsurf_export_to_syncobj_point_params` ioctls.
5. **Windows HAGS, MCDM, and WSL2 GPU in KVM:** `539.72` leaves HAGS as `AlwaysOff / Enabled: False`, does not support MCDM (which replaces legacy TCC compute-only mode on newer GPUs; WDDM remains Microsoft's active display model), and does not support WSL2 GPU passthrough inside a KVM Windows guest.
6. **Homogeneous vGPU Profiles Only & No Console VNC over Wayland:** Concurrent VMs on Pascal must share one profile size (e.g., `2× GRID P4-4Q`), and Wayland desktops are accessed via RDP or Selkies (`display=off`).
7. **Pascal GP104 Hardware Limits:** No SR-IOV, MIG, Unified Memory (Linux), DLSS 2+, or hardware AV1 encode/decode.
8. **vGPU Licensing (`fastapi-dls` Deployed on Host `https://192.168.1.24:443`):** Without an active DLS/CLS license lease, `535.309.01` / `539.72` transitions from `Unlicensed (Unrestricted)` to `Unlicensed (Restricted)` after ~20 minutes. This is resolved by running `collinwebdesigns/fastapi-dls:latest` (`v2.0.3`) on `https://192.168.1.24:443` and installing client tokens from `https://192.168.1.24/-/client-token` on both `vm-win11` and `vm-alma` (`NVIDIA RTX Virtual Workstation — Licensed`, 90-day rotating leases).
9. **GTK4 Vulkan-on-Wayland Swapchain Bridge (`libnvidia-vulkan-producer.so`):** In `535.309.01`, `libnvidia-vulkan-producer.so` segfaults (`ProducerCleanup`) if GTK4 uses its default `vulkan` GDK renderer against R620's `libnvidia-egl-wayland.so.1.1.20`. Setting `GSK_RENDERER=ngl` globally switches GTK4 applications (`ptyxis`, `nautilus`, `gnome-control-center`) to hardware-accelerated OpenGL 4.6 / EGL on `GRID P4-4Q`.
10. **Windows 11 RDP Dynamic Retina Resolution (`3600 × 2262`) with `AVCHardwareEncodePreferred=1`:** Forcing `NVIDIA H.264 Encoder MFT` on the RDP transport pipe at resolutions exceeding `1920 × 1080` overflows `RDP_GFX` (`0x8007007A` -> client `Error code: 0x200d`). Keeping default RDP codec negotiation at `1920 × 1080` (`dynamic resolution:i:0`, `smart sizing:i:1`) with `UserAuthentication=1` (NLA) prevents `0x200d` while keeping `NVIDIA GRID P4-4Q` active for all DWM/3D/CUDA/NVENC/NVDEC workloads.

---

## 4. Observed Feature Comparison: Linux GNOME RDP vs. Windows 11 RDP

| Feature | Linux GNOME RDP (`gnome-remote-desktop` 49.3 on `vm-alma`) | Windows 11 24H2 RDP (`TermService` / `RdpCoreTS` on `vm-win11`) |
| :--- | :--- | :--- |
| **Graphics Protocol** | `RDPGFX_CAPVERSION_107` (`0x000A0701`), `RDS::Graphics` DVC | `RDPGFX_CAPVERSION_107` (`0x000A0701`), `RDS::Graphics` DVC (`Initial profile: 2048`) |
| **Hardware Video Codec** | H.264 **AVC444 & AVC420** via **CUDA + NVENC** | H.264 **AVC444 & AVC420** via **`NVIDIA H.264 Encoder MFT`** (Event `170`, dynamic QP `22–38`) |
| **Compositor Stack** | Pure Wayland (`gnome-shell --no-x11` / Mutter) + PipeWire virtual monitor | Desktop Window Manager (`dwm.exe`) on WDDM `3.1` (`bEnumerateHWAdapters=1`) |
| **Transport & Security** | TLS + NLA (CredSSP), TCP `:3389` | TLS + NLA (CredSSP), TCP + UDP (`SelectTransport=2`), `:3389` |
| **Dynamic Resize & Multi-Monitor (`disp`)** | Supported (`RDS::DisplayControl`) | Supported (`RDS::DisplayControl`, up to 4 monitors / `5120×2880` / 32-bit) |
| **Audio Playback / Input** | Supported (`AUDIO_PLAYBACK_DVC` / PipeWire) | Supported (`AUDIO_PLAYBACK_DVC` + `LOSSY_DVC` + `audin`) |
| **Clipboard (`cliprdr`)** | Supported (text, images, FUSE file copy/paste) | Supported (text, rich formats, FUSE file copy/paste) |
| **Drive / Printer / Smartcard (`rdpdr`)** | Not implemented in GRD | Supported (`rdpdr` negotiated) |
| **Camera & Media Redirection** | Not implemented in GRD | Supported (`RDCamera_Device_Enumerator`, `RDS::Video::Control/Data`, `Geometry` DVCs) |
| **Companion Web Streaming (`10-bit 4:4:4`)** | **Selkies + PixelFlux `2.2.3`** (`https://192.168.1.68:3001`, **`NVENC H265 4:4:4 10-bit`** & **`NVENC H264 4:4:4 8-bit`**) | N/A |
