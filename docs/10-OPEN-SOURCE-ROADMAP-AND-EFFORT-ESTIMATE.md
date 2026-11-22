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
| **Tier 1 (Phase 1A Done / Phase 1B Remaining)** | **`libnvidia-vgpu.so.535.309.01`** -> `libopen-nvidia-vgpu.so` (`vmiop` mediator plugin) | `3.62 MB` / `1.76 MB` `.text` | **`~0.25–0.35 MB`** *(GP104 non-SR-IOV KVM + `47` live VRPCs cataloged)* | 4–8 weeks | **Phase 1A Done (`~1.5 hrs`)**; **2–3 sessions** remaining for full non-proprietary standalone 2-VM parity (`Phase 1B`, excluding DRM) | **Phase 1A Complete:** ABI `VER_5_0`/`VER_2_0`, clean-room GP104 standalone core (`open-vgpu-plugin/`), and live 2-VM trace capture (`34` Linux + `39` Windows = `47` distinct VRPCs, BAR0 `0x8f000`, PCI vendor cap) are verified on `.24`. Remaining **Phase 1B** fills out per-opcode RM payload handlers for all `47` cataloged VRPCs so standalone mode boots Linux + Windows without delegating non-DRM calls to the vendor `.so`. |
| **Tier 1B (Fast w/ Agents)** | **Thin Guest Userspace Bridges** (`libnvidia-encode.so` `0.24 MB` / `10` exports, `libEGL_nvidia.so` `1.28 MB` / `8` exports) | `1.52 MB` / `0.93 MB` `.text` | **`~0.15 MB`** *(NVENC RM ioctl wrapper)* | 2–4 weeks | **1–2 sessions (`2–4 hrs`)** *(for `libnvidia-encode.so`)* | `libnvidia-encode.so` is a thin userspace marshaller that translates `NvEnc*` API calls into `/dev/nvidia0` RM channel/object allocations (`NVC0B7_VIDEO_ENCODER`). Traceable via `LD_PRELOAD` just like `libnvidia-ml.so`. |
| **Tier 2 (Moderate w/ Agents)** | **Guest `nv-kernel.o_binary`** (AlmaLinux 10.3 vGPU guest RM core) | `61.06 MB` / `11.53 MB` `.text` *(all chips)* | **`~0.40–0.60 MB`** *(vGPU guest RPC client + BAR1 MMU)* | 3–6 weeks | **2–4 sessions (`1–2 days`)** | In vGPU guest mode (`virtualizationMode == VGPU`), RM skips physical clocks, Falcon boot, and WPR, acting as a paravirtualized RPC client over BAR0 `0x8f000`. NVIDIA's open `535.309.01` GSP-RM client (`src/nvidia/`) already contains ~85% of the C source; AI agents only need to bridge the vGPU BAR0 `VRPC` ring and ~15 GP104 guest HAL stubs. |
| **Tier 2B (Complex w/ Agents)** | **Host `nv-kernel.o_binary`** (Host GP104 physical RM core) | `61.06 MB` / `11.53 MB` `.text` *(all chips)* | **`~1.1–1.5 MB`** *(GP104 physical HAL + `NVA081`/`NVA082` host vGPU)* | 3–6 months | **10–20 sessions (`1–2 weeks`)** *(using parallel subagent workflows)* | Requires GP104 physical initialization, loading immutable Category #1 signed Falcon blobs (`ACR`, `PMU`, `SEC2`, `FECS`, `NVENC`, `NVDEC`), BAR1/BAR2 MMU, and `NVA081`/`NVA082` host vGPU control tables. Parallel AI agent workflows can fan out across the GP104 HAL modules (`resolve_535_hal.c` / `p4_vgpu_535_slice.o`) while reusing NVIDIA's open `src/nvidia/` core. |
| **Tier 3 (Use Upstream Open Stacks Instead)** | **Monolithic Guest Compilers & Runtime** (`libcuda` `28.02 MB`, `libnvidia-ptxjitcompiler` `22.28 MB`, `libnvidia-glcore` `43.83 MB`, `libnvidia-glvkspirv` `38.89 MB`, `libnvcuvid` `9.25 MB`) | **`142.27 MB`** / **`73.56 MB`** `.text` | `~73.56 MB` *(PTX->SASS JIT, GLSL/SPIR-V compilers)* | Multiple engineer-years | **Not recommended to decompile** *(Weeks of token-heavy LLM decompilation vs. 0 days using upstream Mesa)* | Decompiling `73.56 MB` of register allocators and shader compilers is redundant even with AI agents: upstream **Mesa (`NVK` + `NAK` + `Zink`)** already provides an open-source Vulkan/OpenGL compiler stack, while retaining unmodified vendor `libcuda.so` / `libnvidia-glcore.so` preserves 100% CUDA 12.2 & `Selkies` NVENC compatibility. |

---

## 3. AI-Agent Accelerated Phasing for Remaining Components

1. **Phase 1A *(COMPLETE)* & Phase 1B *(Milestone 1B-1 Complete / 1B-2 Remaining)* — Open Host `vmiop` Mediator (`libopen-nvidia-vgpu.so` / `nova-vgpu`)**
   - **Phase 1A *(Done)*:** Delivered `open-vgpu-plugin/` (`libopen-nvidia-vgpu.so` `VER_5_0`/`VER_2_0` standalone GP104 core + hybrid interposer), `open-vgpu-mgr` live trace tap (`msync`-safe), cold-boot `/dev/nvidiactl` fix, `rea@6.10.1` exports/layout/exposure evidence, and complete live 2-VM VRPC/BAR0/PCI catalog (`47` opcodes in [`12-OPEN-VGPU-PLUGIN-AND-VRPC-TRACE-CATALOG.md`](./12-OPEN-VGPU-PLUGIN-AND-VRPC-TRACE-CATALOG.md)).
   - **Milestone 1B-1 *(Done)*:** Added host RM `NVA081`/`NVA082` bridge (`open_vgpu_rm.c`), guest-to-host client mapping, and `32` RM-backed VRPC handlers (`open_vrpc_rm.c`), verified via mock-RM unit tests and `--live-rm-probe` on `192.168.1.24` (`GRID P4-4Q`, `virt_mode=3`, `fb=0xec000000`).
   - **Milestone 1B-2 *(Est. 1–2 sessions remaining)*:** Implement the remaining `15` memory/MMU/channel/PTE-blit VRPCs (`0x04`..`0x0f`, `0x1b`, `0x1c`, `0x21`, `0x2b`, `0x34`, `0x38`, `0x3d`) plus `vmiop_map_guest_region` BAR1/USERD forwarding for standalone 2-VM boot (keeping vendor DLS licensing excluded per policy).
2. **Phase 2 — Open Guest vGPU Kernel Driver (`Guest nv-kernel.o_binary` -> `open-gpu-kernel-modules` vGPU Client) *(Est. 2–4 sessions)* **
   - **Workflow**: Adapt NVIDIA's open `535.309.01` kernel RM (`src/nvidia/`) so its `vgpu_rpc` transport talks over the guest's BAR0 `0x8f000` shared page instead of GSP-RM, verified live on `vm-alma` (`192.168.1.68`).
   - **Outcome**: 100% open-source Linux guest kernel modules (`nvidia.ko`, `nvidia-modeset.ko`, `nvidia-drm.ko`) with zero `.o_binary` blobs inside the AlmaLinux guest.
3. **Phase 3 — GP104 Open Host Kernel RM (`Host nv-kernel.o_binary`) *(Est. 10–20 sessions via parallel agent workflows)* **
   - **Workflow**: Fan out subagents across the GP104 physical HAL (`ACR` WPR boot, `PMU`, `SEC2`, `GR` netlist loader, `FIFO`, `FB`, `NVENC`/`NVDEC` Falcon loaders, and `NVA081`/`NVA082` host vGPU controls) while keeping the immutable Category #1 NVIDIA-signed Falcon firmware binaries intact.
4. **Skip Decompiling Tier 3 (`142.27 MB` Shader/JIT Compilers)**
   - Retain official R535/R620/539.72 guest userspace libraries for CUDA 12.2, `Selkies` PixelFlux 10-bit 4:4:4 NVENC, GTK4 `ngl`, and WDDM 3.1 parity.
