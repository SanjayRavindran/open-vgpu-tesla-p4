# `open-vgpu-tesla-p4` — Open-Source Pascal (`GP104` / Tesla P4) KVM vGPU Stack & Wayland/RDP Toolkit

A dedicated, **100% open-source** clean-room Rust + Linux kernel vGPU research stack, blob-extraction pipeline, and verified R535 + R620 Wayland / Windows 11 RDP deployment suite for **NVIDIA Pascal (`GP104GL` / Tesla P4, `10de:1bb3`)**, built around our **CUDA `12.1` / `12.2` + NVENC `12.1`** baseline after NVIDIA dropped Pascal from R620.

> [!WARNING]
> **Risk & AI/LLM Notice:** This is experimental systems/kernel software created with extensive assistance from a **Large Language Model (LLM)**. Running out-of-tree kernel modules, patched GPU drivers, or virtualized PCI devices carries risks of kernel panics, BSODs, or data loss. Read [`DISCLAIMER.md`](./DISCLAIMER.md) in full and audit all scripts before use.

> [!NOTE]
> Bare-metal `580.178.04` hybrid driver work remains in the separate [`open-580-driver`](https://github.com/SanjayRavindran/open-580-driver) repository. This repository contains **zero proprietary NVIDIA binaries** and **zero passwords**.

---

## Repository Structure

| Component | License | Description |
| :--- | :--- | :--- |
| [`nova-vgpu/`](./nova-vgpu/Cargo.toml) | `GPL-2.0` | Standalone safe Rust reimplementation of `nvidia-vgpud`, `vgpuConfig.xml` (12 Tesla P4 profiles), `NVA081` packing, `nvidia-vgpu-vfio` `'r'`/`'q'` ioctl ABIs, `vmiop` `VER_5_0` lifecycle, GP104 virtual PCI config, BAR0 `0x8f000` trapping, `"VRPC"` RPC dispatch, `VMMU`, `CHIDMGR`, `FRL`, clean-room guest driver (`guest.rs`), and Falcon blob signature/TOC validator (`falcon_blobs.rs`). |
| [`kmods/nova_vgpu_mdev/`](./kmods/nova_vgpu_mdev/nova_vgpu_mdev.c) | `GPL-2.0` | Linux `7.0+` host `vfio-mdev` PCI mediator (`vfio_dma_rw`, MSI/INTx `eventfd_signal`, `16 MiB` BAR1 `mmap`). |
| [`kmods/nova_vgpu_guest/`](./kmods/nova_vgpu_guest/nova_vgpu_guest.c) | `GPL-2.0` | Linux `7.0+` open-source vGPU guest PCI driver (`0x8f000` BAR0/`"VRPC"` shared rings) + [`test_guest_ioctl.c`](./kmods/nova_vgpu_guest/test_guest_ioctl.c). |
| [`scripts/extract_proprietary_blobs.sh`](./scripts/extract_proprietary_blobs.sh) | `GPL-2.0` | SHA-256-verified extractor for Host `535.309.01` GP104 Falcon blobs, Linux Guest `535.309.01`, R620 (`620.70.06`) Wayland EGL/GBM overlay (`wayland620.tgz`), and Windows Guest `539.72` WHQL. |
| [`scripts/patch_535_guest_linux7.py`](./scripts/patch_535_guest_linux7.py) | `GPL-2.0` | Linux `7.3+` kernel compatibility patcher (`strncpy` + `drm_atomic_commit`) for `535.309.01-grid` Linux guests. |
| [`scripts/patch_grd_ptx_535.sh`](./scripts/patch_grd_ptx_535.sh) | `GPL-2.0` | Downgrades GNOME Remote Desktop's `grd-cuda-avc-utils_30.ptx` header (`.version 8.7 -> 8.0`) for `535`'s CUDA `12.1`/`12.2` PTX `8.2` limit. |
| [`scripts/start_vgpu_vms.sh`](./scripts/start_vgpu_vms.sh) | `GPL-2.0` | 5-VM concurrent QEMU/KVM launcher (`5× GRID P4-1Q` / `nvidia-63`: Ubuntu 24.04, Debian 13, AlmaLinux 10.3, Arch Linux, Windows 11 24H2). |
| [`rdp/`](./rdp/README.md) | `GPL-2.0` | Ready-to-use `.rdp` client profiles (`mstsc.exe`) for all 5 VMs with LAN H.264/AVC444 settings and zero embedded passwords. |
| [`tests/`](./tests/) | `GPL-2.0` | Python unit/hygiene tests + live Linux Wayland & Windows 11 GPU/NVENC/NVDEC/RDP verification harnesses. |
| [`tools/`](./tools/extract_vgpu_535_blobs.py) | `GPL-2.0` / `MIT` | Clean-room ELF64 `.rodata` bindata parser, Falcon descriptor decoder, and `BLOB_MANIFEST.json`-verified extractor. |
| [`firmware-blobs/gp104-vgpu-535/`](./firmware-blobs/gp104-vgpu-535/README.md) | Clean-room manifest | Metadata manifest (`BLOB_MANIFEST.json`) only; proprietary `.bin` blobs are extracted locally by the user. |
| [`docs/`](./docs/06-LIMITATIONS-R620-VS-R535-AND-VERIFICATION-REPORT.md) | `GPL-2.0` | Complete reverse-engineering reports, limitations vs. R620, RDP feature matrix, and 5-VM test results. |

---

## How to Extract Proprietary Binary Blobs from NVIDIA Drivers

Because Pascal GP104 hardware `PrivSec` (`NV_FUSE[0x21400] = 0x1`) enforces NVIDIA-signed Falcon firmware, and because NVIDIA's proprietary drivers cannot be redistributed in git, extract the required binaries locally from the official NVIDIA GRID installers you obtain under NVIDIA's license terms:

### Step 1 — Obtain Official NVIDIA GRID Installers
- **Host vGPU KVM (`535.309.01`):** `NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run`
- **Linux Guest (`535.309.01` — CUDA `12.1`/`12.2`, NVENC `12.1`):** `NVIDIA-Linux-x86_64-535.309.01-grid.run`
- **Linux Guest R620 (`620.70.06` — Wayland EGL/GBM overlay only):** `NVIDIA-Linux-x86_64-620.70.06-grid.run`
- **Windows Guest (`539.72` WHQL — optional, requires `7z`):** `539.72_grid_win10_win11_server2019_server2022_dch_64bit_international.exe`

### Step 2 — Run the SHA-256-Verified Extraction Script
```bash
./scripts/extract_proprietary_blobs.sh \
  --host-run      /path/to/NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run \
  --guest-535-run /path/to/NVIDIA-Linux-x86_64-535.309.01-grid.run \
  --guest-620-run /path/to/NVIDIA-Linux-x86_64-620.70.06-grid.run \
  --win-539-exe   /path/to/539.72_grid_win10_win11_server2019_server2022_dch_64bit_international.exe
```

What gets populated (all git-ignored):
- `firmware-blobs/gp104-vgpu-535/` — GP104 signed Falcon blobs (`acr`, `pmu`, `sec2`, `nvenc`, `nvdec`, `gr`, plus 32 unpacked `gr/netlist_sections/`), verified against `BLOB_MANIFEST.json`.
- `blobs/host-535/` — Host `vgpuConfig.xml` and `nv-kernel.o_binary`.
- `blobs/guest-535/` — Linux `535.309.01-grid` guest driver tree (patch for Linux `7.3+` with `python3 scripts/patch_535_guest_linux7.py blobs/guest-535/kernel`).
- `blobs/wayland620.tgz` — R620 (`620.70.06`) Wayland/EGL/GBM platform `.so` + `.json` overlay (`libnvidia-egl-wayland 1.1.20`, `libnvidia-egl-wayland2 1.0.2`, `libnvidia-egl-gbm 1.1.3`) to unpack on top of `535.309.01` Linux guests.
- `blobs/win-539/` — Extracted WHQL Windows 10/11 `539.72` driver (`Display.Driver/nvgridsw.inf`).

### Step 3 — Enable Pure Wayland GNOME Hardware RDP (`AVC444`) on Linux Guests
Inside each Linux guest after installing `535.309.01-grid` + unpacking `wayland620.tgz` and setting `options nvidia-drm modeset=1`:
```bash
sudo ./scripts/patch_grd_ptx_535.sh
```
This downgrades GNOME Remote Desktop's CUDA PTX header (`8.7 -> 8.0`) so `535.309.01` (CUDA `12.1`/`12.2`, max PTX `8.2`) loads the AVC444 encoder kernels cleanly.

---

## Known Limitations (Summary)

Full side-by-side R620 comparison and root causes are in [`docs/06-LIMITATIONS-R620-VS-R535-AND-VERIFICATION-REPORT.md`](./docs/06-LIMITATIONS-R620-VS-R535-AND-VERIFICATION-REPORT.md):

- **CUDA `12.1` / `12.2` & PTX `≤8.2` ceiling:** No CUDA `12.3`–`13.5` (R620) support; build or target CUDA `12.1`/`12.2`.
- **NVENC API `12.1` ceiling:** Clients demanding NVENC `≥13.1` (such as Arch Linux's `ffmpeg 8` CLI) fail `h264_nvenc` API checks, though GRD AVC444, `h264_cuvid` decode, Windows `NVIDIA H.264 Encoder MFT`, and Ubuntu/Debian `ffmpeg` all work.
- **Vulkan `1.3.242` ceiling:** No Vulkan `1.4.x` (`1.4.357` in R620).
- **No R620 `nvidia-drm` `fbdev=1` or DRM syncobj timeline ioctls:** `535.309.01` `nvidia-drm` only supports `modeset=1`.
- **Windows 11 `539.72` stays on `WDDM 3.1`:** No vGPU HAGS (`AlwaysOff`), no MCDM (compute-only driver model replacing TCC on newer GPUs; WDDM itself remains Microsoft's active display model), and no WSL2 GPU inside KVM.
- **Homogeneous vGPU profiles only & no Console VNC over Wayland:** All concurrent VMs on Pascal must share one profile size (e.g., `5× GRID P4-1Q`), and Wayland desktops are accessed via RDP (`display=off`).
- **Pascal GP104 silicon limits:** No SR-IOV, MIG, DLSS 2+, or hardware AV1 encode/decode.
- **Unlicensed vGPU timer:** Without an NVIDIA DLS/CLS license server, unlicensed vGPU transitions to restricted throttling after ~20 minutes.
- **Windows 11 storage/NIC stability:** Avoid `virtio-win 0.1.302` (`viostor.sys`/`netkvm.sys` corruption + `0xFC` BSODs); use Microsoft inbox **AHCI (`cache=none`) + `e1000e`** with BitLocker disabled.

---

## Automated Tests & Live Verification Performed

### 1. Run Open-Source Unit & Hygiene Tests (Zero Proprietary Blobs Required)
```bash
cd nova-vgpu && cargo test --verbose
cd .. && python3 -m unittest discover -s tests -v
```

### 2. Validate Extracted Blobs with `nova_vgpu_tool`
```bash
cd nova-vgpu
cargo run --bin nova_vgpu_tool -- ../blobs/host-535/vgpuConfig.xml ../firmware-blobs/gp104-vgpu-535
cargo run --bin nova_vgpu_guest_tool
```

### 3. Live 5-VM Hardware Verification Summary
Verified simultaneously on Tesla P4 (`5× GRID P4-1Q`, `5032 / 8192 MiB`) via [`tests/live_verify_linux_wayland_vgpu.sh`](./tests/live_verify_linux_wayland_vgpu.sh) and [`tests/live_verify_win11_vgpu.ps1`](./tests/live_verify_win11_vgpu.ps1):
- **Ubuntu 24.04 (GNOME `46.0`):** Pure Wayland (`X11_RUNNING=0`), `glmark2-wayland` **`678`**, CUDA `12.2`, `ffmpeg h264_nvenc` + `h264_cuvid` PASS, GRD CUDA + AVC444 LIVE.
- **Debian 13.7 (GNOME `48.7`):** Pure Wayland (`X11_RUNNING=0`), `glmark2-wayland` **`663`**, CUDA `12.2`, `ffmpeg h264_nvenc` + `h264_cuvid` PASS, GRD CUDA + AVC444 LIVE.
- **AlmaLinux 10.3 (GNOME `49.5`):** Pure Wayland (`X11_RUNNING=0`), `vkcube --wsi wayland` (`~77.7 FPS`), CUDA `12.2`, NVENC `12.1` + NVDEC PASS, GRD CUDA + AVC444 LIVE.
- **Arch Linux (`7.3.4`, GNOME `51.1`):** Pure Wayland (`X11_RUNNING=0`, no `Xwayland` binary), `vkcube --wsi wayland` PASS, CUDA `12.2`, NVENC `12.1` + `ffmpeg h264_cuvid` PASS, GRD AVC444 LIVE.
- **Windows 11 24H2 (`539.72` WHQL):** `WDDM 3.1`, D3D11/12 (`FL 12_1`), `vulkan-1.dll 1.3.300.0`, CUDA `12.2`, NVENC `12.1`, full Pascal NVDEC (`H264/HEVC/VP8/VP9/MPEG2/VC1/JPEG`), Windows RDP Event `170` **`NVIDIA H.264 Encoder MFT`** AVC444 LIVE (`2%` guest / `4%` host encoder util).

---

## Documentation
- [01 — Pascal vGPU `535.309.01` Static Analysis & Rust Reimplementation](./docs/01-PASCAL-VGPU-535-ANALYSIS-AND-RUST.md)
- [02 — Guest vGPU Driver Analysis & Open-Source Kernel Mediator](./docs/02-GUEST-VGPU-DRIVER-ANALYSIS-AND-OPENSOURCE.md)
- [03 — Why R620 (`620.70.06`) Dropped Pascal Kernel Support](./docs/03-GRID-620-PASCAL-SUPPORT-CHECK.md)
- [04 — R620 Wayland on R535 Multi-VM LAN & RDP Guide](./docs/04-R620-WAYLAND-ON-R535-MULTI-VM-RDP.md)
- [05 — Complete Project Retrospective & Open vGPU Pros/Cons](./docs/05-PROJECT-RETROSPECTIVE-AND-PROS-CONS.md)
- [06 — Limitations, R620 vs. R535 Hybrid Comparison, RDP Feature Matrix & Live Verification Report](./docs/06-LIMITATIONS-R620-VS-R535-AND-VERIFICATION-REPORT.md)
- [Disclaimer, Risk Notice & AI/LLM Usage Disclosure](./DISCLAIMER.md)
