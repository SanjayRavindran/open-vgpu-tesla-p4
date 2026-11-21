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
   - Standard compiled ELF binaries/libraries/object archives with **no GPU silicon signature check** — retained initially only for stability, and technically replaceable via clean-room reverse engineering just like the four host userspace binaries we have already 100% open-sourced (`nvidia-vgpud` `137,664 B` -> `open-nvidia-vgpud`, `libnvidia-vgxcfg.so` `59,464 B` -> `libopen-vgxcfg.so`, `nvidia-xid-logd` `14,512 B` -> `open-nvidia-xid-logd`, and `nvidia-vgpu-mgr` `173,904 B` -> `open-nvidia-vgpu-mgr` — `~0.37 MB` total).
   - **Explicit Out-of-Scope Exclusion (Licensing / DRM)**: `nvidia-gridd` (and `libFlxCore64.so` FlexNet licensing / vGPU software entitlement enforcement) implements commercial software licensing/DRM checks and is intentionally **excluded** from clean-room replacement scope; licensing daemons remain vendor-supplied.

---

## 2. Empirical Measurements & Complexity Tiers (Remaining Non-DRM Category #3)

Measured directly on `192.168.1.15` against the remaining closed `535.309.01-vgpu-kvm` and `grid-guest-535.309.01` ELF artifacts (`size -A` `.text` byte counts and `nm -D --defined-only` dynamic exports):

| Tier | Remaining Closed Binary (`535.309.01`) | File Size | `.text` Code Size | Dynamic `T`/`W`/`D` Exports | Est. Effort w/ `rea@6.10.1` | Feasibility & Clean-Room Strategy |
| :--- | :--- | :---: | :---: | :---: | :--- | :--- |
| **Tier 1 (Moderate / Core Subset)** | **`nvidia-smi`** + **`libnvidia-ml.so`** (Host telemetry CLI & NVML) | `0.65` + `1.86 MB` (`2.51 MB`) | `0.36` + `1.24 MB` (`1.60 MB`) | `0` + `332` | **1–2 weeks** *(core GP104 + vGPU subset)* / **4–6 weeks** *(full)* | Dispatches directly to `/dev/nvidiactl` RM controls (`NV0000`/`NV0080`/`NV2080`/`NVA081`/`NVA082`); most RM structs are documented in NVIDIA's open kernel headers. |
| **Tier 2 (Medium-Hard)** | **`libnvidia-vgpu.so.535.309.01`** (Host `vmiop` mediator plugin) | `3.62 MB` | `1.76 MB` | `6,273` | **4–8 weeks** *(GP104 + R535/539 only)* | Core vGPU hypervisor mediator: BAR0 `0x8f000` register traps, ~150+ `"VRPC"` RPC handlers, `VMMU` shadow page tables, `CHIDMGR`, FRL. Scoping strictly to Pascal GP104 and disabling SR-IOV/live-migration cuts >50% of complexity; `nova-vgpu` already establishes the Rust skeleton. |
| **Tier 2 (Hard)** | **Guest `nv-kernel.o_binary`** (AlmaLinux vGPU guest RM) | `61.06 MB` *(multi-chip)* | `11.53 MB` *(all chips)* | Kernel relocatable | **3–6 weeks** *(vGPU guest-only)* | Much simpler than physical host RM: in vGPU guest mode (`virtualizationMode != 0`), RM skips physical clocks, Falcon boot, and WPR, forwarding RPC commands across BAR0 `0x8f000` (`VRPC` shared page) + `nvidia-drm` KMS. |
| **Tier 2 (Very Hard)** | **Host `nv-kernel.o_binary`** (Host GP104 physical RM) | `61.06 MB` *(multi-chip)* | `11.53 MB` *(all chips)* | `50,026` total syms (`204` text syms) | **3–6 months** *(GP104-only RM)* | Initializes physical GP104 hardware, executes signed Falcon ACR/PMU/SEC2/FECS/NVENC firmware, manages physical BAR1/BAR2 MMU, GR netlists, HW contexts, and `NVA081`/`NVA082` host RM control tables. High risk of subtle Win11 WDDM / NVENC regressions. |
| **Tier 3 (Impractical to Decompile)** | Guest userspace compute/graphics (`libnvidia-encode` `0.24 MB`, `libEGL_nvidia` `1.28 MB`, `libnvcuvid` `9.25 MB`, `libcuda` `28.02 MB`, `libnvidia-ptxjitcompiler` `22.28 MB`, `libnvidia-glcore` `43.83 MB`, `libnvidia-glvkspirv` `38.89 MB`) | **`143.79 MB`** | **`74.49 MB`** | `713` (`10+8+38+600+22+16+19`) | **Multiple engineer-years** | Decompiling `74.49 MB` of shader compilers (GLSL/SPIR-V/PTX/SASS), OpenGL 4.6 / Vulkan ICDs, and the full CUDA runtime/JIT compiler is neither maintainable nor practical. For open-source guest graphics, upstream **Mesa (`NVK` + `Zink` + `NAK`)** is the realistic ecosystem path, while closed guest libraries (`libcuda`, `libnvidia-glcore`, `libnvidia-encode`) remain intact for CUDA/NVENC/OpenGL 4.6 compatibility. |

*(Calibration: replacing all four host userspace daemons/libraries — `nvidia-vgpud`, `libnvidia-vgxcfg.so`, `nvidia-xid-logd`, and `nvidia-vgpu-mgr`, totaling `~0.37 MB` — along with `rea@6.10.1` evidence generation, mock RM/VFIO unit tests, and concurrent 50/50 2-VM live hardware verification is already 100% complete and excluded from the remaining effort table above.)*

---

## 3. Recommended Practical Phasing for Remaining Components

1. **Phase 1 — Open Host Telemetry Subset (`nvidia-smi` + `libnvidia-ml.so`)**
   - **Target**: Optional clean-room telemetry tool & NVML shim querying `/dev/nvidiactl` RM controls (`NV0000`/`NV0080`/`NV2080`/`NVA081`/`NVA082`) for GP104 temperature, clocks, VRAM usage, and active vGPU instances.
   - **Risk / Benefit**: Zero impact on the running vGPU data plane since all four host runtime daemons (`open-nvidia-vgpud`, `libopen-vgxcfg.so`, `open-nvidia-xid-logd`, `open-nvidia-vgpu-mgr`) are already 100% open source.
2. **Phase 2 — Open Host `vmiop` Mediator (`libnvidia-vgpu.so` -> `nova-vgpu`)**
   - **Target**: Implement complete `"VRPC"`, `VMMU`, and `CHIDMGR` handlers in `nova-vgpu` (`Rust`), validated via trace-driven differential testing against live `vm-alma` (`535.309.01`) and `vm-win11` (`539.72`).
   - **Outcome**: Entire host userspace virtualization layer becomes 100% open source.
3. **Phase 3 — Optional Long-Term Kernel RM R&D (`nv-kernel.o_binary`)**
   - **Target**: GP104-only open guest vGPU RM (`VRPC` client) or host RM shim interacting with the immutable signed Falcon binaries (`Category #1`). Only advisable as long-term kernel R&D given Windows WDDM and `Selkies` NVENC sensitivity.
4. **Skip Decompiling Tier 3 (`143.79 MB` Compute/Shader Userspace)**
   - Retain official R535/R620/539.72 guest userspace libraries for full CUDA 12.2, `Selkies` PixelFlux 10-bit 4:4:4 NVENC, GTK4 `ngl`, and WDDM 3.1 parity; track upstream **Mesa (`NVK`/`NAK`)** if open-source Vulkan/OpenGL stacks are ever desired on Linux guests.
