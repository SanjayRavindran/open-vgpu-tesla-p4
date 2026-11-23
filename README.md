# `open-vgpu-tesla-p4` — Open-Source Pascal (`GP104` / Tesla P4) KVM vGPU Stack, Wayland/WDDM RDP & Selkies/PixelFlux 10-bit 4:4:4 Toolkit

A dedicated, **100% open-source** clean-room Rust + Linux kernel vGPU research stack, blob-extraction pipeline, and verified **50/50 2-VM (`GRID P4-4Q`, 4 GiB VRAM each)** R535 + R620 Pure Wayland / Windows 11 WDDM / Selkies + LinuxServer.io PixelFlux (`10-bit` & `4:4:4` `YUV444`) deployment suite for **NVIDIA Pascal (`GP104GL` / Tesla P4, `10de:1bb3`)** on our **UEFI Coreboot host (`192.168.1.24`)**.

> [!WARNING]
> **Risk & AI/LLM Notice:** This is experimental systems/kernel software created with extensive assistance from a **Large Language Model (LLM)**. Running out-of-tree kernel modules, patched GPU drivers, or virtualized PCI devices carries risks of kernel panics, BSODs, or data loss. Read [`DISCLAIMER.md`](./DISCLAIMER.md) in full and audit all scripts before use.

> [!NOTE]
> Bare-metal `580.178.04` hybrid driver work lives in the companion [`open-580-driver`](https://github.com/SanjayRavindran/open-580-driver) repository. The experimental pure `nova-core` / Mesa / Vulkan track and the 3 surplus Linux test VMs have been retired so development focuses strictly on **`open-580-driver`** and **`open-vgpu-tesla-p4`** (Pure Wayland for Linux desktop, WDDM `3.1` for Windows 11, and NVIDIA Container Toolkit for Selkies/PixelFlux `10-bit 4:4:4`). This repository contains **zero proprietary NVIDIA binaries** and **zero passwords**.

---

## Repository Structure

| Component | License | Description |
| :--- | :--- | :--- |
| [`nova-vgpu/`](./nova-vgpu/Cargo.toml) | `GPL-2.0` | Standalone safe Rust reimplementation of `nvidia-vgpud` (`open-nvidia-vgpud`), `vgpuConfig.xml` (12 Tesla P4 profiles), `NVA081` packing, `nvidia-vgpu-vfio` `'r'`/`'q'` ioctl ABIs, `vmiop` `VER_5_0` lifecycle, GP104 virtual PCI config, BAR0 `0x8f000` trapping, `"VRPC"` RPC dispatch, `VMMU`, `CHIDMGR`, `FRL`, clean-room guest driver (`guest.rs`), and Falcon blob signature/TOC validator (`falcon_blobs.rs`). |
| [`open-vgxcfg/`](./open-vgxcfg/Makefile) | `MIT` / `GPL-2.0` | Clean-room C `libopen-vgxcfg.so` (`libnvidia-vgxcfg.so.1`) implementing all `9/9` `vgxConfig*` exports (`0` byte difference vs. `libnvidia-vgxcfg.so.535.309.01`). |
| [`open-xid-logd/`](./open-xid-logd/Makefile) | `MIT` / `GPL-2.0` | Clean-room C `open-nvidia-xid-logd` NVML XID event & 76-char Base64 debugdump daemon. |
| [`open-vgpu-mgr/`](./open-vgpu-mgr/Makefile) | `MIT` / `GPL-2.0` | Clean-room C `open-nvidia-vgpu-mgr` KVM/VFIO `vmiop-env` runtime daemon exporting all `60/60` `vmiop_*` symbols + live `msync`-safe VRPC trace tap. |
| [`open-nvml-smi/`](./open-nvml-smi/Makefile) | `MIT` / `GPL-2.0` | Clean-room C `open-nvidia-smi` CLI & `libopen-nvidia-ml.so` (`libnvidia-ml.so.1`) querying `/dev/nvidiactl` & `/dev/nvidiaN` RM controls (`0x2080*`, `0xa081*`). |
| [`open-vgpu-plugin/`](./open-vgpu-plugin/Makefile) | `MIT` / `GPL-2.0` | Clean-room C `libopen-nvidia-vgpu.so` (`VER_5_0`/`VER_2_0` `vmiop` mediator plugin) implementing all `47/47` cataloged live VRPC opcodes, guest PFN pinning, BAR1/USERD MMIO forwarding, exact VGX `0x23.0x05` `0x50` (`67,416` B) & `0x9c` (`31,536` B) static-info serializers, and hybrid interposer. |
| [`open-vgpu-guest/`](./open-vgpu-guest/Makefile) | `MIT` / `GPL-2.0` | Clean-room guest BAR0 `0x8f000` `"VRPC"` client (`open_vgpu_guest_rpc.{h,c}`), `59`-VRPC zero-blob host↔guest loopback test, and `NVIDIA/open-gpu-kernel-modules@535.309.01` GP104 BAR0 VRPC overlay (`overlay/open_vgpu_bar0_rpc.{h,c}`, `scripts/patch_open_535_guest_rm.py`, `scripts/build_open_535_guest_kmod.sh`). |
| [`open-nvenc/`](./open-nvenc/Makefile) | `MIT` | Clean-room C `libopen-nvidia-encode.so` (`libnvidia-encode.so.1`) implementing all `10/10` ELF exports, `43/43` `NV_ENCODE_API_FUNCTION_LIST` slots, `45/45` `NvTool` API names, Pascal GP104 H.264/HEVC 8/10-bit 4:2:0 & 4:4:4 (`YUV444_10BIT`), and `/dev/nvidiactl` `NVC0B7_VIDEO_ENCODER` (`0xc0b7`) RM bridge. |
| [`open-host-rm/`](./open-host-rm/Makefile) | `MIT` | Clean-room C `libopen-host-rm-gp104.so` Tier 2B host GP104 physical RM HAL slice (Category #1 signed Falcon WPR1/ACR LSF header builder for `8/8` Falcons, `NV_PMC_ENABLE`/BAR1/BAR2 physical bringup, and `22`-profile Tesla P4 vGPU / 50-50 `GRID P4-4Q` `2×2048` CHID partitioner). |
| [`evidence/vgpu-535-rea610/`](./evidence/vgpu-535-rea610/rea_doctor.json) | Clean-room JSON | Machine-readable `rea@6.10.1` (`Ghidra 12.2`) static analysis, binary layout, native exports, release exposure, and decompilation summaries for `535.309.01` host & guest binaries. |
| [`kmods/nova_vgpu_mdev/`](./kmods/nova_vgpu_mdev/nova_vgpu_mdev.c) | `GPL-2.0` | Linux `7.0+` host `vfio-mdev` PCI mediator (`vfio_dma_rw`, MSI/INTx `eventfd_signal`, `16 MiB` BAR1 `mmap`). |
| [`kmods/nova_vgpu_guest/`](./kmods/nova_vgpu_guest/nova_vgpu_guest.c) | `GPL-2.0` | Linux `7.0+` open-source vGPU guest PCI driver (`0x8f000` BAR0/`"VRPC"` shared rings, `GP_IN_RING` `0x80b`, LO-before-HI teardown) + [`test_guest_ioctl.c`](./kmods/nova_vgpu_guest/test_guest_ioctl.c). |
| [`scripts/extract_proprietary_blobs.sh`](./scripts/extract_proprietary_blobs.sh) | `GPL-2.0` | SHA-256-verified extractor for Host `535.309.01` GP104 Falcon blobs, Linux Guest `535.309.01`, R620 (`620.70.06`) Wayland EGL/GBM overlay (`wayland620.tgz`), and Windows Guest `539.72` WHQL. |
| [`scripts/patch_535_guest_linux7.py`](./scripts/patch_535_guest_linux7.py) | `GPL-2.0` | Linux `7.x+` kernel compatibility patcher (`strncpy` + `drm_atomic_commit`) for `535.309.01-grid` Linux guests. |
| [`scripts/patch_grd_ptx_535.sh`](./scripts/patch_grd_ptx_535.sh) | `GPL-2.0` | Downgrades GNOME Remote Desktop's `grd-cuda-avc-utils_30.ptx` header (`.version 8.7 -> 8.0`) for `535`'s CUDA `12.1`/`12.2` PTX `8.2` limit. |
| [`scripts/setup_selkies_pixelflux_vgpu.sh`](./scripts/setup_selkies_pixelflux_vgpu.sh) | `GPL-2.0` | Configures Docker + NVIDIA Container Toolkit (`nvidia-ctk` CDI v0.7.0) inside a `535.309.01` + R620 Wayland/GBM vGPU guest for Selkies/PixelFlux `10-bit 4:4:4` hardware acceleration. |
| [`scripts/start_vgpu_vms.sh`](./scripts/start_vgpu_vms.sh) | `GPL-2.0` | 2-VM 50/50 CPU (`4 vCPU` each) & 50/50 GPU (`2× GRID P4-4Q` / `nvidia-65`, `4096 MiB` VRAM each) launcher for AlmaLinux 10.3 (`192.168.1.68`) and Windows 11 24H2 (`192.168.1.65`). |
| [`docker-compose.yml`](./docker-compose.yml) | `GPL-2.0` | Selkies + LinuxServer.io PixelFlux container stack configured for **`10-bit` & `4:4:4` (`YUV444`) NVENC hardware acceleration** (`SELKIES_ENCODER=h265enc,h264enc`, `SELKIES_VIDEO_FULLCOLOR=true`, `SELKIES_VIDEO_10BIT=true`). |
| [`rdp/`](./rdp/README.md) | `GPL-2.0` | Ready-to-use `.rdp` client profiles (`mstsc.exe`) for the 2 active 50/50 VMs (`vm-win11.rdp` & `vm-alma.rdp`) with LAN H.264/AVC444 settings and zero embedded passwords. |
| [`tests/`](./tests/) | `GPL-2.0` | Python unit/hygiene tests + live Linux Wayland & Windows 11 GPU/NVENC/NVDEC/RDP verification harnesses. |
| [`tools/`](./tools/extract_vgpu_535_blobs.py) | `GPL-2.0` / `MIT` | Clean-room ELF64 `.rodata` bindata parser, Falcon descriptor decoder, and `BLOB_MANIFEST.json`-verified extractor. |
| [`firmware-blobs/gp104-vgpu-535/`](./firmware-blobs/gp104-vgpu-535/README.md) | Clean-room manifest | Metadata manifest (`BLOB_MANIFEST.json`) only; proprietary `.bin` blobs are extracted locally by the user. |
| [`docs/`](./docs/06-LIMITATIONS-R620-VS-R535-AND-VERIFICATION-REPORT.md) | `GPL-2.0` | Complete reverse-engineering reports, UEFI Coreboot host setup, limitations vs. R620, RDP feature matrix, and 2-VM 50/50 + Selkies `10-bit 4:4:4` test results. |

---

## Active 2-VM 50/50 Architecture on UEFI Coreboot Host (`192.168.1.24`)

| Machine | LAN IP | CPU / RAM / vGPU Profile | Desktop & Hardware Acceleration Verified |
| :--- | :--- | :--- | :--- |
| **Host (`h61`)** | `192.168.1.24` (`br0`) | Intel Xeon E3-1275 V2 (`4C/8T`), `14 GiB` RAM, Gigabyte `H61M-S1 PLUS` with **UEFI Coreboot (`df45ea2c49b4-dirty`)** + Intel VT-d (`DMAR`) | Ubuntu 26.04 LTS (`7.0.0-43-generic`), `535.309.01-vgpu-kvm`, persistent `br0`, `vgpu-vms.service` auto-starting both `GRID P4-4Q` VMs (`8032 / 8192 MiB` allocated) |
| **`vm-win11`** | `192.168.1.65:3389` | `4 vCPU` (50%), `6 GiB` RAM, **`GRID P4-4Q` (`4096 MiB` VRAM, 50% GPU)** | Windows 11 24H2 Pro, WHQL **`539.72` (`WDDM 3.1`)** bound to `SUBSYS_120610DE`, `Microsoft Basic Display Adapter` disabled, **TechPowerUp GPU-Z 2.71.0** (`4096 MB GDDR5`, DirectX 12 `12_1`, OpenGL `4.6`, Vulkan, CUDA, OpenCL, DirectCompute, DirectML), **NVIDIA Control Panel (`8.1.969.0`)**, and hardware `AVC444` RDP (`NVIDIA H.264 Encoder MFT`) |
| **`vm-alma`** | `192.168.1.68:3389`<br>`https://192.168.1.68:3001` | `4 vCPU` (50%), `5 GiB` RAM, **`GRID P4-4Q` (`4096 MiB` VRAM, 50% GPU)** | AlmaLinux 10.3 (`6.12.0-264.16.1.el10_3.x86_64_v2`), **Pure Wayland GNOME `49.5` (`--no-x11`, zero host X11/Xwayland)** with R620 EGL/GBM overlay + hardware `AVC444` RDP, PLUS **Docker `29.10.0` + NVIDIA Container Toolkit `1.20.2` + Selkies `2.2.1` + LinuxServer.io PixelFlux `2.2.3`** streaming zero-copy **`NVENC H265 4:4:4 10-bit` (`444-10`)** and **`NVENC H264 4:4:4 8-bit` (`444-8`)** |

---

## How to Extract Proprietary Binary Blobs from NVIDIA Drivers

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

### Step 3 — Enable Pure Wayland GNOME Hardware RDP (`AVC444`) & Selkies/PixelFlux (`10-bit 4:4:4`) on Linux Guest
```bash
sudo ./scripts/patch_grd_ptx_535.sh
sudo ./scripts/setup_selkies_pixelflux_vgpu.sh
docker compose up -d
```

---

## Known Limitations (Summary)

Full side-by-side R620 comparison and root causes are in [`docs/06-LIMITATIONS-R620-VS-R535-AND-VERIFICATION-REPORT.md`](./docs/06-LIMITATIONS-R620-VS-R535-AND-VERIFICATION-REPORT.md):

- **CUDA `12.1` / `12.2` & PTX `≤8.2` ceiling:** No CUDA `12.3`–`13.5` (R620) support; build or target CUDA `12.1`/`12.2`.
- **NVENC API `12.1` ceiling:** Clients demanding NVENC `≥13.1` fail API version checks, whereas PixelFlux `2.2.3` dynamically negotiates `NVENC API 12.1` and encodes `H265 4:4:4 10-bit` and `H264 4:4:4 8-bit` in hardware.
- **Vulkan `1.3.242` ceiling:** No Vulkan `1.4.x` (`1.4.357` in R620).
- **No R620 `nvidia-drm` `fbdev=1` or DRM syncobj timeline ioctls:** `535.309.01` `nvidia-drm` only supports `modeset=1`.
- **Windows 11 `539.72` stays on `WDDM 3.1`:** No vGPU HAGS (`AlwaysOff`), no MCDM (compute-only driver model replacing TCC on newer GPUs; WDDM remains Microsoft's active display model), and no WSL2 GPU inside KVM. When changing vGPU profiles (e.g. `1Q -> 4Q`), run `pnputil /add-driver ... /install` once for the new Subsystem ID (`SUBSYS_120610DE`) and keep `Microsoft Basic Display Adapter` disabled.
- **Homogeneous vGPU profiles only & no Console VNC over Wayland:** Concurrent VMs on Pascal must share one profile size (e.g., `2× GRID P4-4Q`), and Wayland desktops are accessed via RDP (`display=off`).
- **Pascal GP104 silicon limits:** No SR-IOV, MIG, DLSS 2+, or hardware AV1 encode/decode.
- **Unlicensed vGPU timer:** Without an NVIDIA DLS/CLS license server, unlicensed vGPU transitions to restricted throttling after ~20 minutes.

---

## Automated Tests & Live Verification

```bash
# Rust open-nvidia-vgpud + RM ioctl unit tests
(cd nova-vgpu && cargo test --all-targets)

# All 8 clean-room C host & guest component test suites
make -C open-vgxcfg clean all test
make -C open-xid-logd clean all test
make -C open-vgpu-mgr clean all test
make -C open-nvml-smi clean all test
make -C open-vgpu-plugin clean all test
make -C open-vgpu-guest clean all test
make -C open-nvenc clean all test
make -C open-host-rm clean all test

# Repository hygiene, patcher idempotency, REA evidence & script dry-run tests
python3 -m unittest discover -s tests -v
```

## Documentation
- [01 — Pascal vGPU `535.309.01` Static Analysis & Rust Reimplementation](./docs/01-PASCAL-VGPU-535-ANALYSIS-AND-RUST.md)
- [02 — Guest vGPU Driver Analysis & Open-Source Kernel Mediator](./docs/02-GUEST-VGPU-DRIVER-ANALYSIS-AND-OPENSOURCE.md)
- [03 — Why R620 (`620.70.06`) Dropped Pascal Kernel Support](./docs/03-GRID-620-PASCAL-SUPPORT-CHECK.md)
- [04 — 2-VM 50/50 (`GRID P4-4Q`) Pure Wayland, Windows 11 WDDM & Selkies 10-bit 4:4:4 Guide](./docs/04-R620-WAYLAND-ON-R535-MULTI-VM-RDP.md)
- [05 — Complete Project Retrospective & Open vGPU Pros/Cons](./docs/05-PROJECT-RETROSPECTIVE-AND-PROS-CONS.md)
- [06 — Limitations, R620 vs. R535 Hybrid Comparison, RDP Feature Matrix & Live Verification Report](./docs/06-LIMITATIONS-R620-VS-R535-AND-VERIFICATION-REPORT.md)
- [07 — Reproducible Ubuntu KVM & Proxmox VE 8/9 Deployment Guide](./docs/07-UBUNTU-AND-PROXMOX-REPRODUCIBLE-DEPLOYMENT.md)
- [08 — Clean-Room Open-Source Host & Guest Daemons (`rea@6.10.1`)](./docs/08-OPEN-SOURCE-HOST-DAEMONS-REA-610.md)
- [09 — Open-Source vs. Closed-Source vGPU Software Architecture Diagrams](./docs/09-OPEN-VS-CLOSED-ARCHITECTURE-DIAGRAMS.md)
- [10 — Open-Source Roadmap, Remaining Blob Matrix & Effort Estimate](./docs/10-OPEN-SOURCE-ROADMAP-AND-EFFORT-ESTIMATE.md)
- [11 — Unified Host & Guest Installation Guide](./docs/11-INSTALLATION-GUIDE-HOST-AND-GUEST.md)
- [12 — Open `vmiop` Plugin (`libopen-nvidia-vgpu.so`) & 47-Opcode Live VRPC Catalog](./docs/12-OPEN-VGPU-PLUGIN-AND-VRPC-TRACE-CATALOG.md)
- [13 — Clean-Room Open Guest vGPU Kernel Driver (`open-gpu-kernel-modules@535.309.01` GP104 BAR0 VRPC Overlay)](./docs/13-OPEN-GUEST-VGPU-KERNEL-DRIVER.md)
- [Disclaimer, Risk Notice & AI/LLM Usage Disclosure](./DISCLAIMER.md)

---

## Reproducible Ubuntu KVM & Proxmox VE Deployment + Open Host/Guest Stack (`rea@6.10.1`)

All non-DRM host control/runtime/telemetry daemons, the `vmiop` mediator plugin, the guest BAR0 `"VRPC"` kernel driver overlay, the guest NVENC userspace wrapper, and the GP104 physical host RM HAL slice have been reverse-engineered with **`rea@6.10.1`** (`evidence/vgpu-535-rea610/`) and implemented as clean-room open-source components:
- **Clean-Room Open-Source Host & Guest Components**:
  - `nova-vgpu` (`open-nvidia-vgpud`): pure-Rust `/dev/nvidiactl` RM ioctl registrar (`NVA081` `0xa0810101` + `0xa0810109`).
  - `open-vgxcfg` (`libopen-vgxcfg.so`): zero-dependency C library implementing all `9/9` `vgxConfig*` exports (`0` byte differences against `libnvidia-vgxcfg.so.535.309.01`).
  - `open-xid-logd` (`open-nvidia-xid-logd`): clean-room C NVML XID daemon (`mock-NVML` + 76-char Base64 debugdump tested).
  - `open-vgpu-mgr` (`open-nvidia-vgpu-mgr`): clean-room C KVM/VFIO `vmiop-env` runtime daemon exporting all `60/60` `vmiop_*` symbols (`0` symbol diff against `nvidia-vgpu-mgr`).
  - `open-nvml-smi` (`open-nvidia-smi` + `libopen-nvidia-ml.so`): clean-room C NVML (`libnvidia-ml.so.1`) and `nvidia-smi` CLI querying `/dev/nvidiactl` & `/dev/nvidiaN` RM controls (`0x2080*`, `0xa081*`) for GP104 telemetry and active vGPU instances.
  - `open-vgpu-plugin` (`libopen-nvidia-vgpu.so`): clean-room C `vmiop` mediator plugin (`VER_5_0`/`VER_2_0`, `47/47` live VRPC opcodes, PFN pinning, BAR1/USERD MMIO forwarding, exact `0x50` `67,416` B & `0x9c` `31,536` B binary serializers).
  - `open-vgpu-guest` + `kmods/nova_vgpu_guest`: clean-room guest BAR0 `0x8f000` `"VRPC"` client (`59`-VRPC zero-blob host↔guest loopback), `nova_vgpu_guest.ko`, and `NVIDIA/open-gpu-kernel-modules@535.309.01` zero-blob guest `nvidia*.ko` (`Dual MIT/GPL`, `taint: OE`) overlay.
  - `open-nvenc` (`libopen-nvidia-encode.so` / `libnvidia-encode.so.1`): clean-room NVENC 12.1 userspace library (`10/10` exports, `43/43` function slots, `45/45` `NvTool` names, GP104 H.264/HEVC 10-bit 4:4:4 + `/dev/nvidiactl` `0xc0b7` RM bridge).
  - `open-host-rm` (`libopen-host-rm-gp104.so`): clean-room Tier 2B host GP104 physical RM HAL slice (`8/8` Category #1 signed Falcon WPR1/ACR LSF headers, `NV_PMC_ENABLE`/BAR1/BAR2 physical bringup, `22` vGPU types, 50/50 `GRID P4-4Q` split).
  - See [docs/08-OPEN-SOURCE-HOST-DAEMONS-REA-610.md](docs/08-OPEN-SOURCE-HOST-DAEMONS-REA-610.md), [docs/09-OPEN-VS-CLOSED-ARCHITECTURE-DIAGRAMS.md](docs/09-OPEN-VS-CLOSED-ARCHITECTURE-DIAGRAMS.md), and [docs/10-OPEN-SOURCE-ROADMAP-AND-EFFORT-ESTIMATE.md](docs/10-OPEN-SOURCE-ROADMAP-AND-EFFORT-ESTIMATE.md).
- **Unified Host & Guest Installation Scripts**:
  - `scripts/install_open_vgpu.sh`: unified `--role host|guest-linux|guest-windows` installer for the 50/50 vGPU stack.
  - `scripts/install_open_580.sh`: unified `--role host|guest-linux` installer for the bare-metal / PCIe-passthrough `open-580-driver` stack.
  - `scripts/install_vgpu_host_dkms.sh`: clean DKMS `nvidia-vgpu-kvm/535.309.01` + all 6 open host binaries installer for Ubuntu KVM & Proxmox VE 8/9.
  - `scripts/install_fastapi_dls.sh`: FastAPI-DLS v2 (`docker` or native `systemd`).
  - `scripts/setup_proxmox_vgpu_vms.sh`: Proxmox VE `fresh` & `import-qcow2` VM provisioning (`vm-alma` + `vm-win11`).
  - `scripts/provision_alma_guest.sh`, `scripts/provision_win11_guest.ps1`, and `scripts/templates/win11-autounattend.xml`.
  - See [docs/07-UBUNTU-AND-PROXMOX-REPRODUCIBLE-DEPLOYMENT.md](docs/07-UBUNTU-AND-PROXMOX-REPRODUCIBLE-DEPLOYMENT.md) and [docs/11-INSTALLATION-GUIDE-HOST-AND-GUEST.md](docs/11-INSTALLATION-GUIDE-HOST-AND-GUEST.md).
