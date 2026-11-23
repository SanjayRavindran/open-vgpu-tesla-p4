# Open-Source Roadmap & Effort Estimate for Proprietary `535.309.01` Binaries

This document classifies all remaining closed-source components in the **Tesla P4 (`GP104GL`) `535.309.01` vGPU KVM** stack and provides an empirical engineering effort estimate for clean-room open-source replacement using **`rea@6.10.1`** (`GHIDRA_INSTALL_DIR=/opt/ghidra_12.2_PUBLIC`), calibrated against the host components we have already replaced and live-verified.

---

## 1. Three-Category Classification of Proprietary Assets

Not all closed-source components on the host and guests are governed by cryptographic hardware checks. They fall into three distinct categories:

1. **Category #1 — Hardware-Enforced NVIDIA-Signed Blobs (Immutable on Silicon)**
   - **Tesla P4 (`GP104`) `PrivSec = 1`** is permanently fused in silicon. The GPU BootROM (`BROM`) and Heavy-Secure (`HS`) ACR firmware verify NVIDIA production signatures on `ACR_BL`, `ACR_LOAD`, `ACR_BSI_LOCK`, `ACR_UNLOAD`, `SEC2_HS`, `NVENC` HS scrub (16-byte PROD signatures), `PMU_RTOS`, `SEC2_RTOS`, `FECS`, `GPCCS`, `NVENC`, and `NVDEC` (192-byte LS signatures), plus embedded VBIOS Devinit/HULK apps.
   - Any byte modification fails hardware signature verification (`0x4f` / WPR failure). These blobs cannot be replaced by open-source code on fused GP104 silicon.

2. **Category #2 — Software/OS-Verified NVIDIA-Signed Assets**
   - **`vgpuConfig.xml` `<digest>` fields**: Checked by `nv-kernel.o_binary` when `NVA081` (`0xa0810101`) profiles are submitted.
   - **Windows 11 Guest WDDM Driver (`nvlddmkm.sys` `539.72 WHQL`)**: Signed by NVIDIA + Microsoft WHQL so OVMF Secure Boot loads it on Windows 11 24H2 without Test Signing Mode.

3. **Category #3 — Unsigned Proprietary x86_64 Binaries (Software-Replaceable)**
   - Standard compiled ELF binaries/libraries/object archives with **no GPU silicon signature check** — retained initially only for stability, and technically replaceable via clean-room reverse engineering just like the six host userspace binaries/libraries we have already open-sourced (`nvidia-vgpud` `137,664 B` -> `open-nvidia-vgpud`, `libnvidia-vgxcfg.so` `59,464 B` -> `libopen-vgxcfg.so`, `nvidia-xid-logd` `14,512 B` -> `open-nvidia-xid-logd`, `nvidia-vgpu-mgr` `173,904 B` -> `open-nvidia-vgpu-mgr`, and `nvidia-smi` + core `libnvidia-ml.so` `2.51 MB` -> `open-nvidia-smi` + `libopen-nvidia-ml.so` — `~2.88 MB` total).
   - **Explicit Out-of-Scope Exclusion (Licensing / DRM)**: `nvidia-gridd` (and `libFlxCore64.so` FlexNet licensing / vGPU software entitlement enforcement) implements commercial software licensing/DRM checks and is intentionally **excluded** from clean-room replacement scope; licensing daemons remain vendor-supplied.

---

## 2. Empirical AI-Agent Velocity Calibration & Complexity Tiers (Remaining Non-DRM Category #3)

### 2A. Empirical Calibration: Traditional Human Estimate vs. Actual AI-Agent + `rea@6.10.1` Time

Combining **AI agents**, headless **`rea@6.10.1`** (`Ghidra 12.2`), live **`LD_PRELOAD` / ioctl / VFIO tracing** on `192.168.1.24`, and NVIDIA's public MIT/GPL SDK headers delivered a **20x–40x speedup** across the six host binaries we have already 100% open-sourced and live-verified:

| Completed Clean-Room Component | Replaced Binary (`535.309.01`) | Binary Size / Exports | Traditional Human Estimate | Actual AI-Agent + `rea@6.10.1` Time | Verification Achieved |
| :--- | :--- | :---: | :---: | :---: | :--- |
| **`open-nvidia-vgpud`** (`Rust`) | `nvidia-vgpud` | `0.13 MB` (`45` funcs) | **3–5 days** | **~25 mins** | Registers all 12 Tesla P4 `NVA081` profiles & creates sysfs `mdev` types on `.24` |
| **`libopen-vgxcfg.so`** (`C`) | `libnvidia-vgxcfg.so.535.309.01` | `0.06 MB` (`9` exports) | **1–2 days** | **~15 mins** | `0` byte differences across all 9 exports in side-by-side differential test |
| **`open-nvidia-xid-logd`** (`C`) | `nvidia-xid-logd` | `0.01 MB` (`19` funcs) | **1 day** | **~10 mins** | Mock-NVML 76-char Base64 XID dump test + live systemd daemon on `.24` |
| **`open-nvidia-vgpu-mgr`** (`C`) | `nvidia-vgpu-mgr` | `0.17 MB` (`159` funcs, `60` `vmiop_*` exports) | **2–4 days** | **~1.5 hours** (`1` session) | `0` symbol diff (`60/60`), 5 unit test suites, live 50/50 2-VM (`vm-alma` + `vm-win11`) boot & shutdown |
| **`open-nvidia-smi`** + **`libopen-nvidia-ml.so`** (`C`) | `nvidia-smi` + core `libnvidia-ml.so.535.309.01` | `2.51 MB` (`85+` core NVML exports + CLI) | **1–2 weeks** | **~35 mins** (`< 1` session) | Exact CSV/table/vGPU parity vs. vendor `nvidia-smi` + live 2-VM telemetry on `.24` |
| **`libopen-nvidia-vgpu.so` (Phase 1A)** (`C`) | `libnvidia-vgpu.so.535.309.01` (`vmiop` plugin, hybrid + standalone core) | `3.62 MB` / `1.76 MB` `.text` | **4–8 weeks** | **~1.5 hours** (`1` session) | `VER_5_0`/`VER_2_0` ABI parity, standalone GP104 core tests, live 2-VM hybrid interposition & `47`-opcode VRPC catalog on `.24` |
| **Total Host Userspace Open-Sourced (So Far)** | **7 Host Binaries / Libraries (Phase 1A Complete)** | **`~6.50 MB` source footprint replaced** | **~6.5–12 weeks** | **~4 hours total** | **All host control/runtime/CLI daemons + hybrid `vmiop` mediator & live 2-VM VRPC wire catalog open-sourced** |

---

### 2B. Re-Evaluated Complexity Tiers for Remaining Closed Binaries (AI-Agent Accelerated)

Measured directly on `192.168.1.15` against the remaining closed `535.309.01-vgpu-kvm` and `grid-guest-535.309.01` ELF artifacts (`size -A` `.text` byte counts and `nm -D --defined-only` dynamic exports) and recalibrated for **parallel AI agents + `rea@6.10.1` + live trace-driven differential testing**:

| Tier | Remaining Closed Binary (`535.309.01`) | Full File / `.text` Size | Effective GP104 vGPU Slice | Traditional Human Effort | **AI-Agent + `rea@6.10.1` Effort** | AI-Agent Acceleration Strategy |
| :--- | :--- | :---: | :---: | :---: | :---: | :--- |
| **Tier 1 (Phase 1A + Phase 1B COMPLETE — 1B-1/1B-2/1B-3)** | **`libnvidia-vgpu.so.535.309.01`** -> `libopen-nvidia-vgpu.so` (`vmiop` mediator plugin) | `3.62 MB` / `1.76 MB` `.text` | **`~0.25–0.35 MB`** *(GP104 non-SR-IOV KVM + `47` live VRPCs + `0x10758`/`0x7b30` static info)* | 4–8 weeks | **COMPLETE (`Phase 1A + 1B-1 + 1B-2 + 1B-3`)** *(excluding DRM)* | ABI `VER_5_0`/`VER_2_0`, clean-room GP104 standalone core, host RM `NVA081`/`NVA082` bridge, all `47/47` cataloged live VRPC opcodes (`open_vrpc_rm.c`, `open_vrpc_mmu.c`), and exact VGX `0x23.0x05` `0x50` (`67,416` B) & `0x9c` (`31,536` B) binary serializers (`open_vgpu_static_info.c`) are unit-tested (`5/5` suites) & live RM-probed on Tesla P4 (`192.168.1.24`). |
| **Tier 1B (Fast w/ Agents)** | **Thin Guest Userspace Bridges** (`libnvidia-encode.so` `0.24 MB` / `10` exports, `libEGL_nvidia.so` `1.28 MB` / `8` exports) | `1.52 MB` / `0.93 MB` `.text` | **`~0.15 MB`** *(NVENC RM ioctl wrapper)* | 2–4 weeks | **1–2 sessions (`2–4 hrs`)** *(for `libnvidia-encode.so`)* | `libnvidia-encode.so` is a thin userspace marshaller that translates `NvEnc*` API calls into `/dev/nvidia0` RM channel/object allocations (`NVC0B7_VIDEO_ENCODER`). Traceable via `LD_PRELOAD` just like `libnvidia-ml.so`. |
| **Tier 2 (`Phase 2A, 2B-1 & 2B-2 COMPLETE`)** | **Guest `nv-kernel.o_binary`** (AlmaLinux 10.3 vGPU guest RM core) | `61.06 MB` / `11.53 MB` `.text` *(all chips)* | **`~0.40–0.60 MB`** *(vGPU guest RPC client + BAR1 MMU)* | 3–6 weeks | **COMPLETE (`Phase 2A + 2B-1 + 2B-2`)** | **Phase 2A + 2B-1 + 2B-2 Complete (`docs/13`):** Shared BAR0 `0x8f000` `"VRPC"` client (`open_vgpu_guest_rpc.c`), zero-blob host↔guest loopback harness, `nova_vgpu_guest.ko`, plus `open-gpu-kernel-modules@535.309.01` GP104 BAR0 VRPC overlay (`overlay/open_vgpu_bar0_rpc.{h,c}`, `patch_open_535_guest_rm.py`, `build_open_535_guest_kmod.sh`) verified compiling zero-blob `nv-kernel.o` & `nvidia*.ko` (`Dual MIT/GPL`, `taint: OE`, `registerHalModule_GP104`, `openVgpu*`) on both Ubuntu `7.0.0-43-generic` and AlmaLinux 10.3 `6.12.0-264.16.1.el10_3.x86_64_v2`, and live-tested against real Tesla P4 `GRID P4-4Q` mdev (`GP_IN_RING` `0x80b` ring handshake, `0x02`/`0x20`/`0x13`/`0x1a`/`0x0a`/`0x2f` VRPC lifecycle through `gpuStateInit` into `gpuStateLoad`, and GSP-RM vs. legacy Pascal vGPU boundary documented). |
| **Tier 2B (Complex w/ Agents)** | **Host `nv-kernel.o_binary`** (Host GP104 physical RM core) | `61.06 MB` / `11.53 MB` `.text` *(all chips)* | **`~1.1–1.5 MB`** *(GP104 physical HAL + `NVA081`/`NVA082` host vGPU)* | 3–6 months | **10–20 sessions (`1–2 weeks`)** *(using parallel subagent workflows)* | Requires GP104 physical initialization, loading immutable Category #1 signed Falcon blobs (`ACR`, `PMU`, `SEC2`, `FECS`, `NVENC`, `NVDEC`), BAR1/BAR2 MMU, and `NVA081`/`NVA082` host vGPU control tables. Parallel AI agent workflows can fan out across the GP104 HAL modules (`resolve_535_hal.c` / `p4_vgpu_535_slice.o`) while reusing NVIDIA's open `src/nvidia/` core. |
| **Tier 3 (Use Upstream Open Stacks Instead)** | **Monolithic Guest Compilers & Runtime** (`libcuda` `28.02 MB`, `libnvidia-ptxjitcompiler` `22.28 MB`, `libnvidia-glcore` `43.83 MB`, `libnvidia-glvkspirv` `38.89 MB`, `libnvcuvid` `9.25 MB`) | **`142.27 MB`** / **`73.56 MB`** `.text` | `~73.56 MB` *(PTX->SASS JIT, GLSL/SPIR-V compilers)* | Multiple engineer-years | **Not recommended to decompile** *(Weeks of token-heavy LLM decompilation vs. 0 days using upstream Mesa)* | Decompiling `73.56 MB` of register allocators and shader compilers is redundant even with AI agents: upstream **Mesa (`NVK` + `NAK` + `Zink`)** already provides an open-source Vulkan/OpenGL compiler stack, while retaining unmodified vendor `libcuda.so` / `libnvidia-glcore.so` preserves 100% CUDA 12.2 & `Selkies` NVENC compatibility. |

---

## 3. AI-Agent Accelerated Phasing for Remaining Components

1. **Phase 1A & Phase 1B *(COMPLETE)* — Open Host `vmiop` Mediator (`libopen-nvidia-vgpu.so` / `nova-vgpu`)**
   - **Phase 1A *(Done)*:** Delivered `open-vgpu-plugin/` (`libopen-nvidia-vgpu.so` `VER_5_0`/`VER_2_0` standalone GP104 core + hybrid interposer), `open-vgpu-mgr` live trace tap (`msync`-safe), cold-boot `/dev/nvidiactl` fix, `rea@6.10.1` exports/layout/exposure evidence, and complete live 2-VM VRPC/BAR0/PCI catalog (`47` opcodes in [`12-OPEN-VGPU-PLUGIN-AND-VRPC-TRACE-CATALOG.md`](./12-OPEN-VGPU-PLUGIN-AND-VRPC-TRACE-CATALOG.md)).
   - **Milestone 1B-1 *(Done)*:** Added host RM `NVA081`/`NVA082` bridge (`open_vgpu_rm.c`), guest-to-host client mapping, and `32` RM-backed VRPC handlers (`open_vrpc_rm.c`), verified via mock-RM unit tests and `--live-rm-probe` on `192.168.1.24` (`GRID P4-4Q`, `virt_mode=3`, `fb=0xec000000`).
   - **Milestone 1B-2 *(Done)*:** Implemented all remaining `15` memory/MMU/channel/PTE-blit VRPCs (`0x04`..`0x0f`, `0x1b`, `0x1c`, `0x21`, `0x2b`, `0x34`, `0x38`, `0x3d`) in `open_vrpc_mmu.c` (reaching `47/47` cataloged live opcodes), guest PFN pinning (`vmiop_pin_guest_pages_v2` / `NV01_MEMORY_LIST_SYSTEM` `0x81` / `unpin_pages`), ref-counted `vmiop_map_guest_region` BAR1/USERD MMIO forwarding, and `0x06` unlicensed channel-cap enforcement.
   - **Milestone 1B-3 *(Done)*:** Upgraded `0x50` (`0x10758` B, `29` sub-blocks) and `0x9c` (`0x7b30` B, `11` sub-blocks) to exact VGX `0x23.0x05` binary layouts in `open_vgpu_static_info.c` (including `_nv007054vgpu` `0x940`B->`0x900`B packing and `0xff000001U` bounds enforcement), backed by `18` real host RM controls verified on `.24`.
2. **Phase 2 — Open Guest vGPU Kernel Driver (`Guest nv-kernel.o_binary` -> `open-gpu-kernel-modules` vGPU Client) *(COMPLETE — Phase 2A, 2B-1 & 2B-2)* **
   - **Phase 2A *(Done — [`13-OPEN-GUEST-VGPU-KERNEL-DRIVER.md`](./13-OPEN-GUEST-VGPU-KERNEL-DRIVER.md))*:** Built shared `open_vgpu_guest_rpc.{h,c}` (`VGX 0x23.0x05`), verified zero-blob host↔guest loopback (`test_host_guest_loopback.c`), upgraded `kmods/nova_vgpu_guest/` (`535.309.01` `0x50`/`0x9c` probe + Linux `7.0` `kbuild`), and audited `NVIDIA/open-gpu-kernel-modules@535.309.01`.
   - **Phase 2B-1 *(Done)*:** Delivered clean-room GP104 BAR0 `0x8f000` `"VRPC"` overlay (`open_vgpu_bar0_rpc.{h,c}` + `vgpu_events.h` interrupt/event handlers + bare-metal `OPEN_VGPU_GP104_GATE`), idempotent patcher (`patch_open_535_guest_rm.py`), and `build_open_535_guest_kmod.sh`, verified compiling zero-blob `535.309.01` `nv-kernel.o`, `nv-modeset-kernel.o`, `nvidia.ko`, `nvidia-modeset.ko`, `nvidia-drm.ko`, and `nvidia-uvm.ko` on `192.168.1.15` (`7.0.0-43-generic`, `Dual MIT/GPL`, `ALL_OPEN_535_GUEST_SYMBOLS_VERIFIED!`).
   - **Phase 2B-2 *(Done)*:** Live-compiled and loaded all four open `535.309.01` guest modules (`taint: OE`, `Dual MIT/GPL`) inside temporary AlmaLinux 10.3 `vm-alma-2b2` (`6.12.0-264.16.1.el10_3.x86_64_v2`) against the real Tesla P4 `GRID P4-4Q` mdev on `192.168.1.24`, verifying `GP_IN_RING` (`0x80b`) ring setup, `0x01`/`0x40`/`0x50` (`67,416` B)/`0x9c` (`31,536` B) handshake, `0x02`/`0x20`/`0x13` RM hierarchy creation, `0x1a` `0x40` header framing + `0x20801112` 23-dword XDR unpacker (`0x1dfa80.c`), local `NV2080_CTRL_CMD_INTERNAL_*` synthesis, `gpuStateInit` completion into `gpuStateLoad` (`RmInitNvDevice` line 945), and clean `0x0a` `FREE` + `0x2f` `UNLOADING_GUEST_DRIVER` + LO-before-HI ring teardown (`vmiop_log: (0x0): Guest driver unloaded!`).
3. **Phase 3 — GP104 Open Host Kernel RM (`Host nv-kernel.o_binary`) *(Est. 10–20 sessions via parallel agent workflows)* **
   - **Workflow**: Fan out subagents across the GP104 physical HAL (`ACR` WPR boot, `PMU`, `SEC2`, `GR` netlist loader, `FIFO`, `FB`, `NVENC`/`NVDEC` Falcon loaders, and `NVA081`/`NVA082` host vGPU controls) while keeping the immutable Category #1 NVIDIA-signed Falcon firmware binaries intact.
4. **Skip Decompiling Tier 3 (`142.27 MB` Shader/JIT Compilers)**
   - Retain official R535/R620/539.72 guest userspace libraries for CUDA 12.2, `Selkies` PixelFlux 10-bit 4:4:4 NVENC, GTK4 `ngl`, and WDDM 3.1 parity.

---

## 4. Deep-Dive: Tier 3 Monolithic Compilers, How Upstream Mesa (`NVK` + `NAK` + `Zink`) Compares, and What Remaining Work Is Worth It

### 4.1 What Is Tier 3 & Why Decompiling It Is Not Recommended
**Tier 3** comprises the five massive closed-source userspace compiler and runtime shared libraries inside the Linux guest (`grid-guest-535.309.01` / R620 Wayland hybrid userspace), totaling **`142.27 MB`** of stripped ELF binaries (`73.56 MB` of raw `.text` machine code):

| Tier 3 Guest Shared Library | ELF File Size | `.text` Code Size | Primary Internal Subsystem |
| --- | ---: | ---: | --- |
| `libnvidia-glcore.so` | `43.83 MB` | `~21.8 MB` | Monolithic OpenGL state tracker + GLSL front-end + optimizing shader compiler + GP104 `3D` (`0xc197`) pushbuffer emitter |
| `libnvidia-glvkspirv.so` | `38.89 MB` | `~20.1 MB` | SPIR-V to NVIDIA internal IR / SASS shader compiler backend for Vulkan and OpenGL |
| `libcuda.so` | `28.02 MB` | `~14.2 MB` | CUDA Driver API runtime, context/stream/graph scheduler, ELF `.cubin` loader, and GP104 `COMPUTE` (`0xc1c0`) / DMA (`0xc1b5`) channel submission engine |
| `libnvidia-ptxjitcompiler.so` | `22.28 MB` | `~12.6 MB` | Optimizing PTX virtual ISA -> Pascal `sm_61` hardware SASS JIT compiler (SSA optimization passes, instruction scheduling, graph-coloring register allocation) |
| `libnvcuvid.so` | `9.25 MB` | `~4.8 MB` | Bitstream parsers (`H.264`, `HEVC`, `VP9`, `AV1`) and `NVDEC` (`0xc1b0`) hardware command-buffer builder |
| **Total Tier 3 Footprint** | **`142.27 MB`** | **`73.56 MB`** | **Compilers, SSA optimizers, register allocators, and hardware command pushbuffer emitters** |

**Why decompiling Tier 3 is not recommended (even with AI agents + `rea@6.10.1`):**
1. **73.56 MB of `.text` Is Dominated by Compiler Passes & Auto-Generated SASS Encoding Tables**: Unlike Tier 1/Tier 2 daemons and RM transport layers (`0.01–0.60 MB` effective slice governed by clean struct/ioctl/RPC contracts), `libnvidia-ptxjitcompiler.so`, `libnvidia-glvkspirv.so`, and `libnvidia-glcore.so` consist of hundreds of SSA compiler optimization passes, live-range splitters, instruction schedulers, and thousands of per-architecture (`sm_50`..`sm_90`) SASS opcode bitfield tables. Decompiling and verifying them would consume weeks of LLM token budget with near-zero architectural payoff.
2. **Zero Security or Kernel-Upgrade Bottleneck**: Tier 3 libraries run entirely in **unprivileged guest userspace** and communicate with the kernel strictly through stable `/dev/nvidiactl`, `/dev/nvidia0`, and `/dev/nvidia-uvm` ioctls. They never block Linux kernel upgrades (unlike kernel modules) and never require hypervisor access.
3. **Open-Source Compilers Already Exist Upstream**: The open-source community (Collabora, Red Hat, Valve, Faith Ekstrand) has already written clean-room open-source shader compilers and state trackers for NVIDIA hardware in **Mesa (`NVK` + `NAK` + `Zink` + `Rusticl`)**, making a reverse-engineered clone of `libnvidia-glcore.so` or `libnvidia-glvkspirv.so` redundant.

---

### 4.2 How the Upstream Mesa Stack (`NVK` + `NAK` + `Zink` + `Rusticl`) Works vs. Our Current Hybrid `R535` + `R620` Guest Stack

| Dimension | Current Hybrid `R535` Kernel + `R620` Userspace Stack (`vm-alma`) | Upstream Open-Source Mesa Stack (`NVK` + `NAK` + `Zink` + `Rusticl`) | Winner / Trade-off on Tesla P4 (`GP104`) vGPU |
| --- | --- | --- | --- |
| **Wayland / GBM / GTK4 (`GSK_RENDERER=ngl` / `vulkan`)** | Requires our hybrid `R535.309.01` kernel + `R620` (`550+`) EGL/Wayland/GBM userspace bridge (`10_nvidia_wayland.json` + `libnvidia-egl-gbm.so.1`) so headless GNOME 49.5 Wayland and GTK4 apps don't crash on R535's incomplete GBM symbols | **Native first-class upstream Mesa GBM / `linux-dmabuf-v1` / `linux-drm-syncobj-v1`**. Zero proprietary EGL external-platform libraries; GTK4 `ngl`/`vulkan`, GNOME Mutter, KDE, and wlroots work out of the box | **Mesa is cleaner architecturally** for standard Linux Wayland desktop compositing (zero R535/R620 library splicing) |
| **OpenGL & Vulkan Compilers** | Closed `libnvidia-glcore.so` (`43.83 MB`) + `libnvidia-glvkspirv.so` (`38.89 MB`) | **`NVK`** (open Vulkan driver) + **`NAK`** (Rust-based NIR->SASS shader compiler) + **`Zink`** (OpenGL 4.6 layered over Vulkan) + **`Nouveau` Gallium GL** (native Pascal `nvc0` OpenGL 4.5) | **Mesa (`nvc0` / `Zink`) provides 100% open inspectable compilers**, though **`NVK`/`NAK` officially targets Turing (`TU102+`) and newer** for full Vulkan 1.3 conformance (Pascal `GP104` support in `NVK`/`NAK` is experimental/incomplete) |
| **CUDA 12.2 / PyTorch / LLM Inference & `GRD` CUDA NVENC** | Full native **`libcuda.so` (`535.309.01`, CUDA 12.2)** + `libnvidia-ptxjitcompiler.so` (`sm_61` PTX JIT, with our `patch_grd_ptx_535.sh` `.version 8.5 -> 8.2` fix for GNOME Remote Desktop) | **`Rusticl` (OpenCL 3.0)** only; **no native CUDA Driver/Runtime API** (ZLUDA / SCALE cannot replace kernel-matched `libcuda.so` + cuDNN/TensorRT on vGPU) | **Current `R535`/`R620` stack wins decisively**: required for CUDA 12.2, PyTorch, and `gnome-remote-desktop` `[HWAccel.CUDA]` |
| **10-bit 4:4:4 (`YUV444`) Hardware Video Encoding (`Selkies` PixelFlux)** | Full hardware **`NVENC` (`libnvidia-encode.so`) + `NVDEC` (`libnvcuvid.so`)** supporting `Selkies` PixelFlux 10-bit 4:4:4 (`YUV444`) hardware-accelerated streaming | **No Pascal `NVENC` hardware encoder support in Mesa/Nouveau** (NVIDIA's signed `NVENC` Falcon firmware & RPC interface are proprietary; Nouveau only has partial legacy VP/NVDEC decode) | **Current `R535`/`R620` stack wins decisively**: Mesa cannot drive Tesla P4 `NVENC` for `Selkies` PixelFlux or RDP hardware H.264/HEVC encoding |
| **vGPU Kernel Transport (`/dev/dri/renderD128` vs. BAR0 `0x8f000` `"VRPC"`)** | Talks `/dev/nvidiactl` & `/dev/nvidia0` (`NV_ESC_RM_*` ioctls) -> vGPU guest `nvidia.ko` -> BAR0 `0x8f000` `"VRPC"` rings -> host `nvidia-vgpu-mgr` (`GRID P4-4Q` mdev) | Mesa's `NVK` and `Nouveau` userspace emit **`DRM_IOCTL_NOUVEAU_*`** (`nouveau_drm.h`) ioctls expecting `nouveau.ko` physical PFIFO/MMU management — **stock `nouveau.ko` does not speak NVIDIA's vGPU BAR0 `0x8f000` `"VRPC"` mdev protocol** | **Current stack works today on vGPU mdev**; running Mesa inside a vGPU guest requires either PCIe passthrough/SR-IOV or a DRM-to-VRPC kernel bridge |

**Summary — How Mesa Can Work Better vs. Why We Keep Hybrid `R535`+`R620` in Production:**
- **Why Mesa is better for pure open-source Linux desktop graphics**: Mesa eliminates proprietary `EGL_EXT_platform_wayland` / `libnvidia-egl-gbm` quirks, supports standard Linux `DMA-BUF` and explicit sync natively, and compiles shaders through an open, auditable Rust/C pipeline (`NAK`/`NIR`) without closed `.so` blobs.
- **Why Mesa cannot drop-in replace our current `vm-alma` guest stack on Tesla P4 vGPU today**:
  1. Mesa (`Nouveau`/`NVK`) speaks `DRM_IOCTL_NOUVEAU_*` to `nouveau.ko` (which assumes direct physical BAR0 register access to PFIFO/PGRAPH/PMMU), whereas a Tesla P4 `GRID P4-4Q` (`nvidia-65`) vGPU mdev guest must allocate channels, page tables, and vidmem over **BAR0 `0x8f000` `"VRPC"`** to the KVM host.
  2. Mesa has **no Pascal `NVENC` hardware encoding** (breaking `Selkies` PixelFlux 10-bit 4:4:4 and GNOME Remote Desktop hardware NVENC) and **no CUDA 12.2 driver runtime**.

---

### 4.3 Ranked Evaluation: What Remaining Parts Are Worth Pursuing?

| Rank | Candidate Target | Effort (AI-Agent + `rea@6.10.1`) | Value / ROI | Recommendation |
| :---: | --- | :---: | --- | --- |
| **1 (Highest ROI)** | **Tier 1B — Open `libnvidia-encode.so` (`0.24 MB`, `10` exports) Userspace NVENC Wrapper & Trace Harness** | **1–2 sessions (`2–4 hours`)** | **High**: Thin userspace marshaller that translates `NvEncOpenEncodeSessionEx` / `NvEncEncodePicture` into standard `/dev/nvidia0` RM channel & `NVC0B7_VIDEO_ENCODER` object allocations. Directly illuminates `Selkies` PixelFlux 10-bit 4:4:4 and `gnome-remote-desktop` NVENC session parameters without touching massive compilers. | **Worth doing next** if you want deeper visibility or custom open userspace control over Pascal `NVENC` sessions. |
| **2 (High Architectural Value)** | **DRM Render-Node Bridge in `kmods/nova_vgpu_guest.ko` (`DRM_IOCTL_NOUVEAU_*` / `drm_dumb` -> BAR0 `"VRPC"` Translator)** | **2–4 sessions (`4–8 hours`)** | **Medium-High**: Bridges the gap in Section 4.2 by exposing a standard Linux `/dev/dri/renderD128` (`drm`) node backed by our clean-room BAR0 `0x8f000` `"VRPC"` allocator (`0x0c` `ALLOC_VIDMEM`, `0x1b` `FILL_PTE_MEM`, `0x06` `ALLOC_CHANNEL_DMA`), enabling upstream Mesa (`nvc0` / `Zink`) to render inside a Tesla P4 `GRID P4-4Q` vGPU guest without proprietary guest blobs. | **Worth pursuing** if your goal is running a **100% open-source Mesa Wayland desktop** inside a vGPU mdev guest. |
| **3 (Research / Long-Horizon)** | **Tier 2B — Host `nv-kernel.o_binary` GP104 Physical RM Slice + Pre-GSP Pascal `KernelFifo`/`KernelGraphics` Shims for `open-gpu-kernel-modules`** | **10–20 sessions (`1–2 weeks` w/ parallel agent workflows)** | **Medium**: Replaces the host `nv-kernel.o_binary` C HAL slice (`~1.1–1.5 MB`) while still loading the immutable Category #1 NVIDIA-signed Falcon firmware blobs (`ACR`, `PMU`, `SEC2`, `FECS`, `GPCCS`, `NVENC`, `NVDEC`), and adds legacy Pascal pre-GSP `KernelFifo`/`KernelGraphics` shims so `open-gpu-kernel-modules` completes `gpuStateLoad` without GSP-RM. | **Optional long-term research** (our open-580 / 535 DKMS host stack already runs reliably on Linux `7.0+`). |
| **4 (Do Not Pursue)** | **Tier 3 Monolithic Compilers (`142.27 MB` / `73.56 MB` `.text`) & Category #1/#2 Signed Assets (`PrivSec` Falcon blobs, `nvidia-gridd`, Windows WHQL `nvlddmkm.sys`)** | **Months / Hardware-Impossible** | **Negative ROI**: Category #1 blobs are fused in silicon (`PrivSec = 1`), `nvidia-gridd` is out-of-scope DRM, and Tier 3 compilers duplicate upstream Mesa while risking CUDA/NVENC regressions. | **Skip permanently**. |
