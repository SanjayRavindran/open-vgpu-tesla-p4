# Standalone Clean-Room NVENC 13.0 for Pascal GP104 vGPU Guests (`open-nvenc`)

`open-nvenc` builds `libopen-nvidia-encode.so` (`SONAME libnvidia-encode.so.1`), a clean-room, open-source implementation of NVIDIA Encode API `13.0` (`0xd0`) for Tesla P4 (Pascal GP104) vGPU guests running on 535.309.01-vgpu-kvm hosts.

Because the vendor 535.309.01 guest library caps `libnvidia-encode.so.1` at NVENC `12.1` (`0xc1`) and leaves newer NVENC 13.0 callers unable to initialize on R535 guests, `open-nvenc` implements the GP104 hardware NVENC channel directly over `/dev/nvidiactl` and `/dev/nvidia0` RM ioctls while keeping the standard `libnvidia-encode.so.1` C ABI (`NvEncodeAPICreateInstance`, `NvEncodeAPIGetMaxSupportedVersion`).

## Architecture Highlights

- **Zero Proprietary or CUDA Link Dependencies:** `ldd libopen-nvidia-encode.so` depends only on `libc.so.6` (`-ldl` at build time for optional runtime `libcuda.so.1` resolution when an application passes `CUDADEVICEPTR` or `CUDAARRAY` surfaces).
- **Direct GP104 RM / Falcon Channel:**
  - Opens RM root/device/subdevice (`0x41`, `0x80`, `0x2080`), allocates GP104 VASpace (`0x70`), channel GPFIFO (`0xc06f`) with `NV_ESC_RM_MAP_MEMORY` sub-page `USERD` offset handling, optional Pascal DMA Copy Engine (`0xc0b5` when `OPEN_NVENC_ENABLE_CE` is set), and GP104 NVENC engine (`0xc2b7`, `NVC2B7_VIDEO_ENCODER`), plus `NV906F_CTRL_CMD_GET_CLASS_ENGINEID` (`0x906f0101`).
  - Submits 64-bit GPFIFO pushbuffers with `NVC2B7` Falcon methods (`0x0200` codec mode, `0x0700..0x074c` pic setup / scratch / recon / bitstream / input surfaces, `0x0300` execute, and `0x0240..0x0248`/`0x0304` semaphore release) and polls completion semaphores in pinned sysmem.
- **Supported Codecs, Formats & Rate Control:**
  - **HEVC (`NVC2B7`, `0x3`):** Main (8-bit 4:2:0 `NV12`/`IYUV`/`YV12`/`ARGB`/`ABGR`), Main 10 (`YUV420_10BIT`/`ARGB10`/`ABGR10`), Range Extensions / Rext (`YUV444` 8-bit & `YUV444_10BIT`), including non-32-aligned widths (e.g. `1784x992`) and open I/P GOPs.
  - **H.264 (`NVC2B7`, `0x1`):** Main 4:2:0 (CABAC) and High 4:4:4 Predictive (8-bit 4:4:4 CAVLC).
  - **Rate Control:** `CONSTQP`, `VBR`, `CBR`, `CBR_LOWDELAY_HQ`, `CBR_HQ`, `VBR_HQ` with closed-loop sliding-window QP adaptation.
  - **CUDA Interop:** `NvEncSetIOCudaStreams`, `NvEncRegisterResource` / `NvEncMapInputResource` for `CUDADEVICEPTR` and `CUDAARRAY` via runtime `dlopen("libcuda.so.1")` (re-syncing persistently mapped CUDA staging buffers on each `nvEncEncodePicture`).

## Building & Testing

```bash
cd open-nvenc
make clean all
make test        # Offline ABI + bitstream header + struct validation (no GPU needed)
make hw-test     # Live GP104 hardware encode across 16 HEVC/H.264 8-bit/10-bit 4:2:0/4:4:4 cases
make diff-test   # Differential ABI & NvTool check against vendor 535.309.01
make diff-hw-test # Live GP104 bug-for-bug hardware parity benchmark vs vendor blob (scrolling/cursor/PaintOver/H.264)
```

`OPEN_NVENC_NVIDIACTL_PATH=/nonexistent make test` runs the offline unit suite without `/dev/nvidiactl`.

## Installing on a Linux vGPU Guest

Use `scripts/install_open_nvenc.sh` to build `libopen-nvidia-encode.so`, preserve a `.vendor.bak` copy of the vendor `libnvidia-encode.so.<ver>` binary, install the clean-room library in its place, update dynamic linker caches, and refresh NVIDIA Container Toolkit CDI specs (when `nvidia-ctk` is present):

```bash
sudo bash scripts/install_open_nvenc.sh
```

Options:

- `--prebuilt /path/to/libopen-nvidia-encode.so` — Install an already-compiled library on guests without `gcc`.
- `--libdir /path/to/libdir` — Override auto-detected library directory (`/usr/lib/x86_64-linux-gnu` or `/usr/lib64`).
- `--dry-run` — Print planned actions without modifying the system.
- `--restore` — Restore `libnvidia-encode.so.<ver>.vendor.bak`, run `ldconfig`, and refresh CDI.

For custom staged installs without touching vendor paths:

```bash
make -C open-nvenc PREFIX=/usr/local LIBDIR=/usr/local/lib install
```

## Verifying Upstream FFmpeg & Selkies PixelFlux 10-bit 4:4:4

### FFmpeg 8.x (NVENC 13.0)

```bash
# 10-bit 4:4:4 HEVC VBR at 1920x1080
ffmpeg -y -f lavfi -i testsrc2=size=1920x1080:rate=30:duration=2 \
  -pix_fmt yuv444p10le -c:v hevc_nvenc -profile:v rext -rc vbr -b:v 4M out_y444_10b.mkv

# 10-bit 4:4:4 HEVC at non-32-aligned width (1784x992)
ffmpeg -y -f lavfi -i testsrc2=size=1784x992:rate=30:duration=2 \
  -pix_fmt yuv444p10le -c:v hevc_nvenc -profile:v rext -rc constqp -qp 20 out_1784.mkv

# H.264 VBR at 1280x720
ffmpeg -y -f lavfi -i testsrc2=size=1280x720:rate=30:duration=2 \
  -pix_fmt yuv420p -c:v h264_nvenc -rc vbr -b:v 2M out_h264.mkv
```

Set `OPEN_NVENC_TRACE_DIR=/tmp/nvenc_trace` to confirm `open_nvenc_trace.jsonl` records session open/destroy events.

### Selkies / PixelFlux 2.2.1 (`brave-origin` / LinuxServer Webtop)

After installing into the host guest library path and refreshing CDI specs, restart the container so it picks up the replaced `libnvidia-encode.so.535.309.01`:

```bash
sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml
sudo nvidia-ctk cdi generate --output=/var/run/cdi/nvidia.yaml
docker restart brave-origin
```

Verify inside the container that `NvEncodeAPIGetMaxSupportedVersion` reports `0xd0` (`13.0`) and that PixelFlux encodes 10-bit 4:4:4 HEVC on hardware (`NvEncodeAPICreateInstance` + `YUV444_10BIT`).

## Verified Differential Hardware Benchmarks & Peak GP104 Throughput

Measured live on GP104 (`GRID P4-4Q` / `GRID P4-8Q`, `535.309.01` vs. clean-room `open-nvenc` `13.0`):

1. **Bug-for-Bug Differential Suite (`make diff-hw-test` vs. `535.309.01` & `620.70.06` `0xd1`):**
   - `HEVC 10-bit 4:4:4` (`1784x992`, PixelFlux PaintOver + `InvalidateRefFrames`): VENDOR `156.4–181.5 FPS`, `PSNR=44.42 dB (min=41.82)` vs. **OPEN `98.2–108.3 FPS`, `PSNR=47.09 dB (min=43.71)`, `SSIM=0.9925`, `level=123 bt709/tv`, `0` warnings**.
   - `HEVC ARGB 8-bit 4:4:4` (`1784x992`): VENDOR `145.2–161.5 FPS`, `PSNR=39.09 dB` vs. **OPEN `72.7–76.7 FPS`, `PSNR=42.76 dB`, `level=123`, `0` warnings**.
   - `H.264 NV12` (`1784x992`): VENDOR `231.2–269.1 FPS`, `PSNR=46.02 dB` vs. **OPEN `276.3–312.4 FPS`, `PSNR=47.73 dB`, `level=42`, `0` warnings**.
   - `H.264 YUV444` (`1784x992`): VENDOR `150.5–167.4 FPS`, `PSNR=44.68 dB` vs. **OPEN `174.8–184.2 FPS`, `PSNR=47.12 dB`, `level=42`, `0` warnings**.
   - `HEVC NV12 720p` (`VBR -> CBR` live reconfigure): VENDOR `360.5–398.1 FPS`, `PSNR=31.79 dB` vs. **OPEN `458.0–503.1 FPS`, `PSNR=50.05 dB`, `0` warnings**.
2. **Steady-State `ffmpeg 8.0.1` Encode Limits (`GRID P4-8Q`, `-preset p3 -tune ull`):**
   - **H.264:** up to `4096x2160` (`214 FPS` 1080p, `150 FPS` 1440p, `61 FPS` 4K UHD, `58 FPS` 4K DCI).
   - **HEVC:** up to `8192x4320` (`166 FPS` 1080p NV12, `153 FPS` 1080p P010, `112 FPS` 1440p NV12, `49 FPS` 4K NV12, `38 FPS` 4K P010, `96 FPS` `1784x992` 10-bit 4:4:4, `84 FPS` 1080p 10-bit 4:4:4, `20 FPS` 4K 10-bit 4:4:4, `11 FPS` 8K UHD, `10 FPS` 8K DCI).
