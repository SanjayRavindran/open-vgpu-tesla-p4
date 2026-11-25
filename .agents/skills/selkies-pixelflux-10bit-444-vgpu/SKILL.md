---
name: selkies-pixelflux-10bit-444-vgpu
description: >-
  Deploy and troubleshoot Docker CE, NVIDIA Container Toolkit (nvidia-ctk CDI),
  and Selkies (lscr.io/linuxserver/webtop) with LinuxServer.io PixelFlux
  hardware acceleration at 10-bit and 4:4:4 (YUV444) color depth on Tesla P4
  (bare-metal or AlmaLinux 10.3 R580 vGPU guest). Use this skill when setting up
  or debugging GPU container passthrough, CDI mode after X11 stripping, el10
  x86-64-v3 ISA errors on Xeon E3-1275 V2, or PixelFlux 10-bit 4:4:4 NVENC.
---

# Selkies & LinuxServer.io PixelFlux `10-bit` & `4:4:4` NVENC Container Setup

## Key Fixes for AlmaLinux 10 (`el10`, `x86_64_v2`) on Ivy Bridge Xeon (`x86-64-v2`)

### 1. Strip `.note.gnu.property` (`x86-64-v3`) from `el10` Container Binaries
Red Hat / AlmaLinux 10 (`el10`) stamps Go binaries in the Docker CE and NVIDIA Container Toolkit packages with an `x86-64-v3` `.note.gnu.property` header, causing `ld-linux-x86-64.so.2` to abort with `CPU ISA level is lower than required` on Ivy Bridge (`x86-64-v2`).
1. Strip `.note.gnu.property` from Docker and NCT binaries:
   ```bash
   for bin in /usr/bin/docker* /usr/bin/containerd* /usr/bin/ctr /usr/bin/runc \
              /usr/libexec/docker/cli-plugins/* /usr/bin/nvidia-ctk \
              /usr/bin/nvidia-container-runtime* /usr/bin/nvidia-container-cli; do
     [ -f "$bin" ] && sudo objcopy --remove-section=.note.gnu.property "$bin" 2>/dev/null || true
   done
   ```
2. If `runc init` (`nsexec-0`) still traps with `SIGILL` due to `CGO` `x86-64-v3` instructions in `el10`'s `runc`, copy the `x86-64-v2`-clean `/usr/bin/runc` from the Ubuntu host (`192.168.1.24:/usr/bin/runc`) to `vm-alma:/usr/bin/runc`.

### 2. Enable CDI Mode After X11 Stripping (`mode = "cdi"`) & Regenerate CDI Specs
When stripping NVIDIA X11 files (`/usr/bin/nvidia-xconfig` removed), `nvidia-container-runtime` in `legacy` or `auto` mode fails container start with `executable file not found: nvidia-xconfig`. Always switch to `cdi` mode and sync both `/etc/cdi/nvidia.yaml` and `/var/run/cdi/nvidia.yaml` after `580.196.07` installation:
```bash
sudo ldconfig
sudo nvidia-ctk config --in-place --set nvidia-container-runtime.mode=cdi
sudo nvidia-ctk runtime configure --runtime=docker
sudo mkdir -p /etc/cdi /var/run/cdi
sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml
sudo cp -f /etc/cdi/nvidia.yaml /var/run/cdi/nvidia.yaml
sudo systemctl restart docker
docker compose up -d --force-recreate
```
*(Do **not** reinstall Docker/NCT packages via `dnf` on `el10`, or `/usr/bin/runc` will regain `x86-64-v3` instructions.)*

### 3. Verify `10-bit` & `4:4:4` (`YUV444`) Hardware Encoding in `selkies-webtop`
1. Ensure `docker-compose.yml` passes:
   ```yaml
   environment:
     - SELKIES_ENCODER=h265enc,h264enc
     - SELKIES_VIDEO_FULLCOLOR=true
     - SELKIES_VIDEO_10BIT=true
     - GBM_BACKENDS_PATH=/usr/lib64/gbm:/usr/lib/x86_64-linux-gnu/gbm
   ```
2. Verify hardware formats and zero-copy DMA-BUF (`DRI3` -> `CUDA-array` -> `NVENC`) inside `selkies-webtop`:
   ```bash
   docker exec selkies-webtop /lsiopy/bin/python3 -c '
   import pixelflux
   sc = pixelflux.ScreenCapture()
   print("encoders:", sc.hardware_encoders)
   print("fullcolor:", sc.hardware_fullcolor)
   print("formats:", sc.hardware_formats)
   '
   ```
   Expected output:
   - `encoders: {'h264': 'nvenc', 'h265': 'nvenc'}`
   - `fullcolor: ['h264', 'h265']`
   - `formats: {'h264': ['420-8', '444-8'], 'h265': ['420-8', '420-10', '444-8', '444-10']}`

### 4. Standalone `open-nvenc` (`NVENC 13.0` / `0xd0`, `NVC2B7`) Integration
1. Install or update `libopen-nvidia-encode.so` on the host/guest, regenerate CDI, and recreate containers:
   ```bash
   sudo bash scripts/install_open_nvenc.sh
   sudo nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml
   sudo mkdir -p /var/run/cdi
   sudo cp -f /etc/cdi/nvidia.yaml /var/run/cdi/nvidia.yaml
   sudo systemctl restart docker
   docker compose up -d --force-recreate
   ```
2. Verify non-16-aligned dimensions (`1784x992` `YUV444_10BIT` / scroll-motion browser surfaces) render cleanly without stride/BL16 artifacts (see `open-nvenc-13-gp104` skill and run `make -C open-nvenc diff-hw-test`).
