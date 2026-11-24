---
name: open-nvenc-13-gp104
description: >-
  Build, differential-test, deploy, and troubleshoot the standalone clean-room
  libopen-nvidia-encode.so (NVENC 13.0 / 0xd0) userspace encoder on Pascal GP104
  (NVC2B7 0xc2b7). Use whenever modifying open-nvenc, testing hardware encode
  parity, deploying to Linux guests or bare-metal hosts, or debugging Selkies
  PixelFlux 10-bit 4:4:4 NVENC streams.
---

# Standalone Open NVENC `13.0` (`0xd0`) on Pascal GP104 (`NVC2B7`)

## Architecture Highlights (`open-nvenc/`)
- **Zero-Vendor-Dependency ELF**: `libopen-nvidia-encode.so` (`libnvidia-encode.so.1` SONAME) links solely against `libc.so.6` and optionally loads `libcuda.so.1` via `dlopen` only when CUDA device/array input surfaces are passed.
- **NVENC `13.0` ABI Coverage**: Exports all `10/10` ELF dynamic symbols (`NvEncodeAPICreateInstance`, `NvEncodeAPIGetMaxSupportedVersion` = `0xd0`, `NvTool*`), all `43/43` `NV_ENCODE_API_FUNCTION_LIST` slots, and all `45/45` `NvTool` API names.
- **Direct GP104 RM Channel Path**: Talks directly to `/dev/nvidiactl` & `/dev/nvidia0` (`0x41`/`0x80`/`0x2080`, `NVC06F` GPFIFO, `NVC0B5` DMA copy, `NVC2B7` `0xc2b7` NVENC Falcon), assembling CPU H.264/HEVC headers around Falcon slice bitstreams.
- **Cached Staging & BL16 Swizzle**: Uses a cached 64-byte-aligned host staging buffer with edge clamping/padding and Pascal 16Bx2 Block-Linear (`BL16`) swizzle before sequential write-combined submission, fixing non-16-aligned dimensions (e.g., `1784x992`) and high-motion scroll glitches.

## Build & Verification Commands
```bash
# Host/sandbox unit + ABI differential test
make -C open-nvenc clean all test diff-test

# Hardware GP104 encode + PSNR/SSIM differential test (on GPU host or vGPU guest)
# All 5 hardware test cases beat vendor 535.309.01 PSNR/SSIM parity
make -C open-nvenc hw-test diff-hw-test
```

## Installation, Rollback & Container CDI Refresh
```bash
# Build, differential-test, and install libopen-nvidia-encode.so
sudo bash scripts/install_open_nvenc.sh

# Rollback to vendor libnvidia-encode.so
sudo bash scripts/install_open_nvenc.sh --restore
```
After updating `/usr/lib64/libnvidia-encode.so*` or `/usr/lib/x86_64-linux-gnu/libnvidia-encode.so*`, refresh NVIDIA CDI specs and restart Docker/Selkies containers:
```bash
sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml
sudo mkdir -p /var/run/cdi
sudo cp -f /etc/cdi/nvidia.yaml /var/run/cdi/nvidia.yaml
sudo systemctl restart docker
```

## Measured VM `1000` (`GRID P4-8Q`, `ffmpeg 8.0.1`) GP104 Codec & Resolution Limits
- **H.264 (`h264_nvenc`)**: Max tested `4096x2160` (`4096x4096` HW cap; `4112x2160` rejected); `214 FPS` 1080p, `150 FPS` 1440p, `61 FPS` 4K UHD, `58 FPS` 4K DCI.
- **HEVC (`hevc_nvenc`, 8/10-bit 4:2:0 & 4:4:4)**: Max tested `8192x4320` (`8192x8192` HW cap); `166 FPS` 1080p NV12, `153 FPS` 1080p P010, `96 FPS` `1784x992` `YUV444_10BIT`, `84 FPS` 1080p `YUV444_10BIT`, `49 FPS` 4K NV12, `20 FPS` 4K `YUV444_10BIT`, `11 FPS` 8K UHD, `10 FPS` 8K DCI.
- **AV1 (`av1_nvenc`)**: Unsupported on Pascal GP104 silicon.
