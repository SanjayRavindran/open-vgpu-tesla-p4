# 13 — Tier 2: Open Guest vGPU Kernel Driver (`Phase 2A` Architecture & Host↔Guest Loopback)

## 1. Objective & Scope Boundary

Tier 2 replaces the remaining proprietary kernel archive inside the **AlmaLinux 10.3 vGPU guest** (`grid-guest-535.309.01/kernel/nvidia/nv-kernel.o_binary`, `62 MB` across all architectures) with an open-source Pascal `GP104GL` (`GRID P4-4Q`) guest vGPU kernel driver built on:
1. Clean-room guest BAR0 `0x8f000` `"VRPC"` transport (`open-vgpu-guest/` + `kmods/nova_vgpu_guest/`), and
2. NVIDIA's official MIT/GPLv2 **`NVIDIA/open-gpu-kernel-modules` tag `535.309.01`** (commit `15b1a21dfe4eab54a463a0ea3086823ae35fe02e`).

> **Scope Boundary:** In accordance with project policy, `nvidia-gridd` and FlexNet/vGPU licensing/DRM checks remain untouched and out of scope for replacement. On both host (`libopen-nvidia-vgpu.so`) and guest, licensing messages (`0x39` -> `0xa0820104`) and channel caps are preserved faithfully.

---

## 2. Audit of `NVIDIA/open-gpu-kernel-modules@535.309.01` vs. Proprietary Guest `nv-kernel.o_binary`

Because NVIDIA released `535.309.01` as open-source kernel modules alongside the vGPU 16.x Linux guest package, roughly **85% of the guest Resource Manager (`RM`) C code is already open-source**, and already targets the exact same wire protocol (`VGX 0x23.0x05`) as our host `libopen-nvidia-vgpu.so`:

| Component in `NVIDIA/open-gpu-kernel-modules@535.309.01` | Status in Stock `kernel-open` Build | What It Already Provides | Exact Gap for Phase 2B Guest `nvidia.ko` |
|---|---|---|---|
| `src/nvidia/kernel/inc/vgpu/vgpu_version.h` | Present (`0x23.0x05`) | Exact `VGX_MAJOR_VERSION_NUMBER = 0x23`, `VGX_MINOR_VERSION_NUMBER = 0x05` matching `libnvidia-vgpu.so.535.309.01` | None (version matches 1:1) |
| `src/nvidia/kernel/vgpu/nv/rpc.c` (`2,037` lines) | Compiled when `VIRTUALIZATION` + `RPCSTRUCTURECOPY` enabled | VRPC message framing, continuation records, `ALLOC_MEMORY`, `MAP_MEMORY_DMA`, `UNMAP_MEMORY_DMA`, `IDLE_CHANNELS`, `SET_GUEST_SYSTEM_INFO`, `UNLOADING_GUEST_DRIVER` | Wired to GSP-RM `message_queue_priv.h` instead of BAR0 `0x8f000` `"VRPC"` rings |
| `src/nvidia/kernel/vgpu/nv/objvgpu.c` & `virtualization/hypervisor/kvm/kvm.c` | Present | Guest `OBJVGPU` lifecycle + KVM paravirt detection | Requires `RMCFG_FEATURE_VIRTUALIZATION = 1` and BAR0 `0x8f000` doorbell backend |
| `src/nvidia/src/kernel/gpu/*/arch/pascal/` (`12` C files) | Present in tree | Pascal GP100/GP102/GP104/GP10x HAL sources (`kern_gpu_gp100.c`, `kern_bus_gp100.c`, `kernel_ce_gp100.c`, `kernel_fifo_gp102.c`, `kgraphics_gp100.c`, `intr_gp100.c`, `mem_mgr_gp100.c`, `mem_mgr_scrub_gp100.c`, `kern_mem_sys_gp102.c`, `kern_gmmu_gp100.c`, `kern_gmmu_fmt_gp10x.c`, `kernel_nvlink_gp100.c`) | `src/nvidia/generated/rmconfig.h` built for `shipping-gpus-openrm` sets `RMCFG_CHIP_GP104 = 0`, `RMCFG_FEATURE_VIRTUALIZATION = 0`, `RMCFG_FEATURE_GSP_CLIENT_RM = 1` |
| `src/nvidia/kernel/inc/vgpu/rpc_vgpu.h` | Stubbed empty (`{ }`) | Declares the `36` `NV_RM_RPC_*` guest wrappers (`GET_CONSOLIDATED_STATIC_INFO` `0x50`, `GET_CONSOLIDATED_GR_STATIC_INFO` `0x9c`, `ALLOC_VIDMEM` `0x0c`, `DMA_FILL_PTE_MEM` `0x1b`, `UPDATE_PDE_2` `0x38`, `UPDATE_GPU_PDES` `0x3d`, etc.) | Implement bodies using `open_vgpu_guest_rpc` serializers matching Phase 1B-3 wire layouts |

---

## 3. What Phase 2A Delivers Today

### 3.1 Clean-Room Guest VRPC Protocol Library ([`open-vgpu-guest/`](../open-vgpu-guest/))
- [`open_vgpu_guest_rpc.h`](../open-vgpu-guest/open_vgpu_guest_rpc.h) & [`open_vgpu_guest_rpc.c`](../open-vgpu-guest/open_vgpu_guest_rpc.c) (`SPDX-License-Identifier: MIT OR GPL-2.0`):
  - Transport-abstracted (`open_vgpu_guest_transport_t`) guest PCI / BAR0 `0x8f000` `"VRPC"` circular `SEND_RING` / `RECV_RING` / `EVENT_RING` client that can run unchanged in userspace harnesses OR inside Linux kernel `nvidia.ko`.
  - Complete `0x01` (`SET_GUEST_SYSTEM_INFO`, `VGX 0x23.0x05`) + `0x40` (`SET_GUEST_BRANCH_VER`) handshake.
  - Full chunked reassembly & deserializers for **`0x50` `VGPU_STATIC_INFO`** (`17` chunks = `67,416` B = `0x10758` B across `29` sub-blocks) and **`0x9c` `GR_STATIC_INFO`** (`8` chunks = `31,536` B = `0x7b30` B across `11` sub-blocks).
  - Typed guest RM RPC callers for `0x02` (`ALLOC_ROOT`), `0x20` (`ALLOC_DEVICE`), `0x13` (`ALLOC_SUBDEVICE`), `0x0c` (`ALLOC_VIDMEM`), `0x04` (`ALLOC_MEMORY` PFN pinning), `0x34` (`ALLOC_VIRTMEM`), `0x07` (`MAP_MEMORY` BAR1), `0x05` (`ALLOC_CTX_DMA`), `0x0e`/`0x0f` (`MAP`/`UNMAP_MEMORY_DMA`), `0x1b` (`FILL_PTE_MEM`), `0x38` (`UPDATE_PDE_2`), `0x3d` (`UPDATE_GPU_PDES`), `0x06` (`ALLOC_CHANNEL_DMA` + USERD), `0x17` (`IDLE_CHANNELS`), and `0x0a` (`FREE`).

### 3.2 Zero-Blob Host↔Guest Loopback Verification ([`test_host_guest_loopback.c`](../open-vgpu-guest/test_host_guest_loopback.c))
Wires `open_vgpu_guest_rpc.c` directly over emulated PCI config, relocated BAR0/BAR1, and circular guest DMA pages into our host plugin engine (`gp104_vgpu_core.c`, `open_vgpu_rm.c`, `open_vrpc_rm.c`, `open_vrpc_mmu.c`, `open_vgpu_static_info.c`) — proving end-to-end communication across a **100% open-source host + guest stack**:
- Probes `10de:1bb3`, `subsys 10de:1206`, NVIDIA vendor cap `"535.309.01r539_7"`, `NV_PMC_BOOT_0 = 0x134000a1`, and `3776 MiB` (`0xec000000`) usable FB.
- Verifies pre-init rejection (`0x56`), completes `0x01`/`0x40`, pulls all `17` chunks of `0x50` (`67,416` B) & `8` chunks of `0x9c` (`31,536` B), allocates RM client/device/memory/BAR1/PTEs/GPFIFO channel, idles and tears down with **0 leaked regions**, **0 leaked pinned PFNs**, and **`1:1` MSI doorbell delivery count**.

### 3.3 Guest Kernel Transport Module ([`kmods/nova_vgpu_guest/`](../kmods/nova_vgpu_guest/))
- Upgraded [`nova_vgpu_guest.h`](../kmods/nova_vgpu_guest/nova_vgpu_guest.h) and [`test_guest_ioctl.c`](../kmods/nova_vgpu_guest/test_guest_ioctl.c) with `0x50`/`0x9c`/`0x40`/MMU opcodes, `GRID P4-4Q` (`0xec000000` / `0x1206`) & `P4-2Q` geometry support, `--probe-535` multi-chunk verification mode, and verified `kbuild` compilation against Linux **`7.0.0-43-generic`** headers on `192.168.1.15`.
- Correspondingly upgraded host `gp104_vgpu_core.c` (`handle_doorbell_dispatch`) to advance circular `SEND_RING_GET` and `RECV_RING_PUT` descriptors so both circular ring waiters (`nova_vgpu_guest.ko`, real Linux/Windows NVIDIA guests) and single-page test callers work identically.

### 3.4 `535.309.01` Open RM Source Audit Helper ([`prepare_open_535_guest_rm.sh`](../open-vgpu-guest/scripts/prepare_open_535_guest_rm.sh))
Reproducible helper that clones/audits `NVIDIA/open-gpu-kernel-modules@535.309.01` (`15b1a21dfe4eab54a463a0ea3086823ae35fe02e`) for Phase 2B kernel integration.

---

## 4. Phase 2B Next Steps (Live AlmaLinux 10.3 `vm-alma` Kernel RM Bringup)

1. Enable `RMCFG_CHIP_GP104 = 1`, `RMCFG_FEATURE_VIRTUALIZATION = 1`, and `RMCFG_MODULE_RPCSTRUCTURECOPY = 1` in `open-gpu-kernel-modules@535.309.01` (`src/nvidia/generated/` + `nv.c` guest probe guard for `IS_VIRTUAL`).
2. Populate `src/nvidia/kernel/inc/vgpu/rpc_vgpu.h` & BAR0 `0x8f000` ring transport using the exact `VGX 0x23.0x05` serializers verified in Phase 1B-3 and `open_vgpu_guest_rpc.c`.
3. Build `nvidia.ko`, `nvidia-modeset.ko`, and `nvidia-drm.ko` from `open-gpu-kernel-modules@535.309.01` inside AlmaLinux 10.3 (`192.168.1.68`), keeping existing guest `nvidia-gridd` and R620 Wayland userspace intact.
