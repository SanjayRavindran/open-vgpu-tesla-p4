# `open-vgpu-tesla-p4` — Open-Source Pascal (`GP104` / Tesla P4) R580 KVM vGPU Stack, Pure Wayland/WDDM RDP & Selkies/PixelFlux 10-bit 4:4:4 Toolkit

A dedicated, **open-source** clean-room Rust + C + Linux kernel vGPU research stack and streamlined **R580 (`580.196.09-vgpu-kvm` host / `580.196.07` Linux / `582.99` Windows 11)** **50/50 2-VM (`GRID P4-4Q`, 4 GiB VRAM each)** deployment toolkit featuring X11-stripped Pure Wayland GNOME RDP (`--no-x11 --headless`), Windows 11 WDDM `3.1` RDP, and Selkies + LinuxServer.io PixelFlux (`10-bit` & `4:4:4` `YUV444`) via NVIDIA CDI for **NVIDIA Pascal (`GP104GL` / Tesla P4, `10de:1bb3`)** on our **UEFI Coreboot host (`192.168.1.24`)**.

> [!WARNING]
> **Risk & AI/LLM Notice:** This is experimental systems/kernel software created with extensive assistance from a **Large Language Model (LLM)**. Running out-of-tree kernel modules, patched GPU drivers, or virtualized PCI devices carries risks of kernel panics, BSODs, or data loss. Read [`DISCLAIMER.md`](./DISCLAIMER.md) in full and audit all scripts before use.

> [!NOTE]
> Bare-metal `580.178.04` hybrid driver work lives in the companion [`open-580-driver`](https://github.com/SanjayRavindran/open-580-driver) repository. All R535/R620 hybrid guest workarounds have been retired in favor of native **R580 (`580.196.09` / `580.196.07` / `582.99`)**. This repository contains **zero proprietary NVIDIA binaries** and **zero passwords**.

---

## Repository Structure

| Component | License | Description |
| :--- | :--- | :--- |
| [`nova-vgpu/`](./nova-vgpu/Cargo.toml) | `GPL-2.0` | Standalone safe Rust reimplementation of `nvidia-vgpud` (`open-nvidia-vgpud`), R580 `NVA081` packing (`0x1540` B), `vgpuConfig.xml` (12 Tesla P4 profiles), `nvidia-vgpu-vfio` `'r'`/`'q'` ioctl ABIs, `vmiop` `VER_5_0` lifecycle, GP104 BAR0 `0x8f000` `"VRPC"`, `VMMU`, `CHIDMGR`, `FRL`, clean-room guest (`guest.rs`), and Falcon signature validator. |
| [`open-vgxcfg/`](./open-vgxcfg/Makefile) | `MIT` / `GPL-2.0` | Clean-room C `libopen-vgxcfg.so` (`libnvidia-vgxcfg.so.1`) implementing all `9/9` `vgxConfig*` exports (`0` byte diff). |
| [`open-xid-logd/`](./open-xid-logd/Makefile) | `MIT` / `GPL-2.0` | Clean-room C `open-nvidia-xid-logd` NVML XID event & 76-char Base64 debugdump daemon. |
| [`open-vgpu-mgr/`](./open-vgpu-mgr/Makefile) | `MIT` / `GPL-2.0` | Clean-room C `open-nvidia-vgpu-mgr` KVM/VFIO `vmiop-env` runtime daemon (`60/60` `vmiop_*` symbols, R580 `0x98`-byte `SET_VM_NAME` with `vmIdType = 1U`, `0x1538`-byte `GET_VGPU_TYPE_INFO`, `0x28`-byte `CREATE_DEVICE`, `msync`-safe VRPC tap). |
| [`open-nvml-smi/`](./open-nvml-smi/Makefile) | `MIT` / `GPL-2.0` | Clean-room C `open-nvidia-smi` CLI & `libopen-nvidia-ml.so` (`libnvidia-ml.so.1`) querying `/dev/nvidiactl` & `/dev/nvidiaN` R580 RM controls (`0x2080*`, `0xa081*`). |
| [`open-vgpu-plugin/`](./open-vgpu-plugin/Makefile) | `MIT` / `GPL-2.0` | Clean-room C `libopen-nvidia-vgpu.so` (`VER_5_0`/`VER_2_0` `vmiop` mediator plugin): `47/47` VRPC opcodes, guest PFN pinning, BAR1/USERD MMIO forwarding, `0x50`/`0x9c` static-info serializers, and hybrid interposer (defaulting to `/opt/nvidia-vgpu-580/lib/libnvidia-vgpu.so.580.196.09`). |
| [`open-vgpu-guest/`](./open-vgpu-guest/Makefile) | `MIT` / `GPL-2.0` | Clean-room guest BAR0 `0x8f000` `"VRPC"` client (`open_vgpu_guest_rpc.{h,c}`), `59`-VRPC zero-blob host↔guest loopback test, and open guest kernel RM BAR0 VRPC overlay research. |
| [`open-nvenc/`](./open-nvenc/Makefile) | `MIT` | Clean-room C `libopen-nvidia-encode.so` (`libnvidia-encode.so.1`, `libc.so.6` only) implementing **NVENC `13.0` (`0xd0`)** with `10/10` exports, `43/43` function slots, `45/45` `NvTool` APIs, GP104 `NVC2B7` (`0xc2b7`) + `NVC06F` + `NVC0B5` H.264/HEVC 8/10-bit 4:2:0 & 4:4:4 (`YUV444_10BIT`), and `diff-hw-test`. |
| [`open-host-rm/`](./open-host-rm/Makefile) | `MIT` | Clean-room C `libopen-host-rm-gp104.so` Tier 2B host GP104 physical RM HAL slice (Category #1 signed Falcon WPR1/ACR LSF header builder for `8/8` Falcons, `NV_PMC_ENABLE`/BAR1/BAR2 physical bringup, and 50-50 `GRID P4-4Q` CHID partitioner). |
| [`open-gridd/`](./open-gridd/CMakeLists.txt) | `MIT` | Clean-room C11 + OpenSSL 3.x Linux NLS/DLS licensing daemon (`open-gridd`, GCC 14 clean-compile). |
| [`tools/`](./tools/) | `GPL-2.0` / `MIT` | R580 host XML injector/normalizer (`patch_r580_vgpu_config_xml.py` for `0x1BB3`/`SUBSYS_11D8`), R580 Linux guest `nv-pci.c` open-C allowlist patcher (`patch_r580_guest_nv_pci.py` for `10de:1bb3`), ELF64 `.rodata` bindata parser, and Falcon extractor (`extract_vgpu_535_blobs.py`). |
| [`scripts/`](./scripts/) | `GPL-2.0` | Streamlined R580 host (`install_vgpu_host_dkms.sh`, `install_proxmox_vgpu_host.sh`, `install_open_vgpu.sh`, `install_fastapi_dls.sh`) & guest (`provision_linux_vgpu_guest.sh` with `--grid-580-run` + `--strip-x11` + `--gridd-patcher`, `provision_win11_guest.ps1` with `-PatchedNvxdapixPath`, `setup_selkies_pixelflux_vgpu.sh` CDI mode) automation. |
| [`evidence/vgpu-535-rea610/`](./evidence/vgpu-535-rea610/rea_doctor.json) | Clean-room JSON | Baseline `rea@6.10.1` (`Ghidra 12.2`) static analysis, binary layouts, native exports, and decompilation summaries. |
| [`kmods/nova_vgpu_mdev/`](./kmods/nova_vgpu_mdev/nova_vgpu_mdev.c) & [`kmods/nova_vgpu_guest/`](./kmods/nova_vgpu_guest/nova_vgpu_guest.c) | `GPL-2.0` | Linux `7.0+` host `vfio-mdev` mediator (`vfio_dma_rw`, MSI/INTx, BAR1 `mmap`) & guest `0x8f000` `"VRPC"` PCI driver + [`test_guest_ioctl.c`](./kmods/nova_vgpu_guest/test_guest_ioctl.c). |
| [`docker-compose.yml`](./docker-compose.yml) | `GPL-2.0` | Selkies + LinuxServer.io PixelFlux container stack configured for **`10-bit` & `4:4:4` (`YUV444`) NVENC hardware acceleration** (`SELKIES_ENCODER=h265enc,h264enc`, `SELKIES_VIDEO_FULLCOLOR=true`, `SELKIES_VIDEO_10BIT=true`). |
| [`rdp/`](./rdp/README.md) | `GPL-2.0` | Ready-to-use `.rdp` client profiles (`vm-win11.rdp` & `vm-alma.rdp`) with zero embedded passwords. |
| [`tests/`](./tests/) | `GPL-2.0` | Python unit/hygiene tests + live Linux Wayland (`X11_RUNNING=0`) & Windows 11 R580 verification harnesses. |
| [`docs/`](./docs/06-R580-LIMITATIONS-AND-VERIFICATION-REPORT.md) | `GPL-2.0` | Complete reverse-engineering reports, streamlined R580 host/guest deployment docs, RDP matrix, and live verification reports. |

---

## Active 2-VM 50/50 R580 Architecture on UEFI Coreboot Host (`192.168.1.24`)

| Machine | LAN IP | CPU / RAM / vGPU Profile | Desktop & Hardware Acceleration Verified |
| :--- | :--- | :--- | :--- |
| **Host (`h61`)** | `192.168.1.24` (`br0`) | Intel Xeon E3-1275 V2 (`4C/8T`), `14 GiB` RAM, **UEFI Coreboot (`df45ea2c49b4-dirty`)** + VT-d (`DMAR`) | Ubuntu 26.04 LTS (`7.0.0-44-generic`), **`580.196.09-vgpu-kvm`** (`0xc85b7` bypass + `vgpuConfig.xml` `0x11D8` + `/opt/nvidia-vgpu-580/` open daemons), `fastapi-dls:latest` (`2.0.3`) on `:443`, `GRID P4-4Q` (`nvidia-65`) |
| **`vm-win11`** | `192.168.1.74:3389` (DHCP) / `.65` | `4 vCPU` (50%), `6144 MiB` RAM, **`GRID P4-4Q` (`4096 MiB` VRAM, 50% GPU)** | Windows 11 24H2 Pro, WHQL **`582.99` (`WDDM 3.1`, CUDA `13.0`)** bound natively to `SUBSYS_11D810DE`, `nvxdapix.dll` (`9594600` B) patched via `gridd-unlock-patcher` (**`Licensed`**), hardware `AVC444` RDP (`NVIDIA H.264 Encoder MFT`) |
| **`vm-alma`** | `192.168.1.68:3389`<br>`https://192.168.1.68:3001` | `4 vCPU` (50%), `5120 MiB` RAM, **`GRID P4-4Q` (`4096 MiB` VRAM, 50% GPU)** | AlmaLinux 10.3 (`6.12.0-264.16.1.el10_3.x86_64_v2`), **`580.196.07-grid`** (`nv-pci.c` `10de:1bb3` 2-line open-C patch, `-m=kernel`, **CUDA `13.0`**, **NVENC `13.0`**), X11-stripped **Pure Wayland GNOME `49.5` (`--no-x11 --headless`)**, **`Licensed`** via `gridd-unlock-patcher`, + **Docker + NVIDIA CDI (`mode = "cdi"`) + Selkies PixelFlux (`NVENC H265 4:4:4 10-bit` / `H264 4:4:4 8-bit`)** |

---

## Streamlined R580 Quick-Start (Host & Guests)

### 1. Host Installation (`580.196.09-vgpu-kvm`, Ubuntu KVM or Proxmox VE 8/9)
```bash
sudo bash scripts/install_open_vgpu.sh \
  --role host \
  --run-file /path/to/NVIDIA-Linux-x86_64-580.196.09-vgpu-kvm.run \
  --with-dls docker \
  --dls-url 192.168.1.24
```
*(Automatically applies `0xc85b7` bypass, runs `tools/patch_r580_vgpu_config_xml.py` for `0x11D8` Tesla P4 profiles, installs `/opt/nvidia-vgpu-580/` open daemons + `libnvidia-vgpu.so -> libnvidia-vgpu.so.580.196.09`, and launches FastAPI-DLS `:443`.)*

### 2. Linux Guest Installation (`580.196.07-grid`, Pure Wayland + X11 Strip + CDI)
```bash
sudo bash scripts/provision_linux_vgpu_guest.sh \
  --dls-url https://192.168.1.24 \
  --grid-580-run /path/to/NVIDIA-Linux-x86_64-580.196.07-grid.run \
  --gridd-patcher /path/to/gridd-unlock-patcher \
  --rdp-user zero \
  --rdp-pass '<YOUR_RDP_PASSWORD>' \
  --with-selkies
```
*(Automatically patches `kernel/nvidia/nv-pci.c` for `10de:1bb3`, installs `-m=kernel`, strips NVIDIA X11 artifacts while keeping `10_nvidia_wayland.json` + `15_nvidia_gbm.json`, patches `nvidia-gridd`, fetches DLS token, sets `GSK_RENDERER=ngl`, configures `--no-x11 --headless` `:3389` RDP, and configures Docker CDI `mode = "cdi"`.)*

### 3. Windows 11 24H2 Guest Installation (`582.99` WHQL + `nvxdapix.dll` Patch)
```powershell
powershell -ExecutionPolicy Bypass -File scripts\provision_win11_guest.ps1 `
  -DlsUrl https://192.168.1.24 `
  -DriverInstallerPath C:\path\to\582.99_grid_win10_win11_server2022_server_2025_dch_64bit_international.exe `
  -PatchedNvxdapixPath C:\path\to\nvxdapix_patched.dll
```

---

## Automated Tests & Verification

```bash
# Rust open-nvidia-vgpud + RM ioctl unit tests
(cd nova-vgpu && cargo test --all-targets)

# All 9 clean-room C host & guest component test suites
make -C open-vgxcfg clean all test
make -C open-xid-logd clean all test
make -C open-vgpu-mgr clean all test
make -C open-nvml-smi clean all test
make -C open-vgpu-plugin clean all test
make -C open-vgpu-guest clean all test
make -C open-nvenc clean all test diff-test hw-test diff-hw-test
make -C open-host-rm clean all test
make -C open-gridd clean all test

# Repository hygiene, R580 patcher idempotency, REA evidence & script dry-run tests
python3 -m unittest discover -s tests -v
```

## Documentation
- [01 — Pascal vGPU Static Analysis & Rust Reimplementation (`nova-vgpu`)](./docs/01-PASCAL-VGPU-535-ANALYSIS-AND-RUST.md)
- [02 — Guest vGPU Driver Analysis & Open-Source Kernel Mediator](./docs/02-GUEST-VGPU-DRIVER-ANALYSIS-AND-OPENSOURCE.md)
- [03 — Why R620 (`620.70.06`) Dropped Pascal Kernel Support](./docs/03-GRID-620-PASCAL-SUPPORT-CHECK.md)
- [04 — 2-VM 50/50 (`GRID P4-4Q`) R580 Pure Wayland, Windows 11 (`582.99`) & Selkies Guide](./docs/04-R580-MULTI-VM-WAYLAND-WDDM-RDP.md)
- [06 — R580 Limitations, Feature Matrix & Live Verification Report](./docs/06-R580-LIMITATIONS-AND-VERIFICATION-REPORT.md)
- [07 — Reproducible Ubuntu KVM & Proxmox VE 8/9 Deployment Guide](./docs/07-UBUNTU-AND-PROXMOX-REPRODUCIBLE-DEPLOYMENT.md)
- [08 — Clean-Room Open-Source Host & Guest Daemons (`rea@6.10.1`)](./docs/08-OPEN-SOURCE-HOST-DAEMONS-REA-610.md)
- [09 — Open-Source vs. Closed-Source vGPU Software Architecture Diagrams](./docs/09-OPEN-VS-CLOSED-ARCHITECTURE-DIAGRAMS.md)
- [10 — Open-Source Roadmap, Remaining Blob Matrix & Effort Estimate](./docs/10-OPEN-SOURCE-ROADMAP-AND-EFFORT-ESTIMATE.md)
- [11 — Unified R580 Host & Guest Installation Guide](./docs/11-INSTALLATION-GUIDE-HOST-AND-GUEST.md)
- [12 — Open `vmiop` Plugin (`libopen-nvidia-vgpu.so`) & 47-Opcode Live VRPC Catalog](./docs/12-OPEN-VGPU-PLUGIN-AND-VRPC-TRACE-CATALOG.md)
- [13 — Clean-Room Open Guest vGPU Kernel Driver Research](./docs/13-OPEN-GUEST-VGPU-KERNEL-DRIVER.md)
- [14 — Standalone Open NVENC `13.0` (`0xd0`) / `NVC2B7` (`0xc2b7`) GP104 Protocol](./docs/14-STANDALONE-NVENC-13-GP104-PHASE0-PROTOCOL.md)
- [15 — Standalone Open NVENC `13.0` Usage, Installation & Parity Guide](./docs/15-STANDALONE-NVENC-13-GP104-USAGE.md)
- [Disclaimer, Risk Notice & AI/LLM Usage Disclosure](./DISCLAIMER.md)
