# 13 — Tier 2: Open Guest vGPU Kernel Driver (`Phase 2A` Loopback, `Phase 2B-1` `535.309.01` Kbuild & `Phase 2B-2` Live AlmaLinux 10.3 Verification)

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

## 4. What Phase 2B-1 Delivers Today (Full Open `535.309.01` `nvidia.ko` Build)

### 4.1 Clean-Room BAR0 `0x8f000` `"VRPC"` Guest RM Overlay ([`open-vgpu-guest/overlay/`](../open-vgpu-guest/overlay/))
- [`open_vgpu_bar0_rpc.h`](../open-vgpu-guest/overlay/open_vgpu_bar0_rpc.h) & [`open_vgpu_bar0_rpc.c`](../open-vgpu-guest/overlay/open_vgpu_bar0_rpc.c) (`SPDX-License-Identifier: MIT`):
  - Allocates four `4 KiB` guest system DMA pages (`sendRing`, `recvRing`, `eventRing`, `msgPage`) using `memdescCreate` / `memdescAlloc` / `memdescMap` / `memdescGetPhysAddr(AT_GPU)` and programs BAR0 `0x8f000`..`0x8f058` circular rings (`0x0a` guest ID marker at `0x8f058`, PTE format `(pfnLo << 12) | 0xb`).
  - Hooks `initRpcObject()` and `rpcSendMessage` / `rpcRecvPoll` via `openVgpuBar0RpcSendMessage()` & `openVgpuBar0RpcRecvPoll()` (`"VRPC"` magic `0x43505256`), executes the initial `VGX 0x23.0x05` handshake (`0x01` `SET_GUEST_SYSTEM_INFO`, `0x40` `SET_GUEST_SYSTEM_INFO_EXT`, chunked `0x50` `67,416` B / `0x10758` B, and chunked `0x9c` `31,536` B / `0x7b30` B), and decodes `OPEN_VGPU_STATIC_INFO_DATA`.
  - Implements all six vGPU shared-memory / event-ring interrupt entry points declared by `kernel/inc/vgpu/vgpu_events.h`: `vgpuGetPendingEvent`, `vgpuService`, `vgpuShmIsSwPending`, `vgpuShmIsNonStallEnabled`, `vgpuIsNonStallPending`, and `vgpuServiceNonStall`.

### 4.2 Idempotent Source Patcher & Reproducible Kbuild Verifier ([`open-vgpu-guest/scripts/`](../open-vgpu-guest/scripts/))
- [`patch_open_535_guest_rm.py`](../open-vgpu-guest/scripts/patch_open_535_guest_rm.py): Idempotently patches pristine `NVIDIA/open-gpu-kernel-modules@535.309.01`:
  - Enables `GP104` chip ID (`{ 0x00000013, 0x00000004, 0x0 }`) in `g_hal_archimpl.h`, registers `registerHalModule_GP104()` reusing `rpcHalIfacesSetup_TU102` in `g_hal_register.h` & `g_hal_private.h`, and maps `arch == 0x13 && impl == 0x4` in `g_chips2halspec_nvoc.c`.
  - Injects the `OPEN_VGPU_GP104_GATE` check inside `osapi.c` (`rm_is_supported_device()`): maps `pNv->regs->cpu_address + 0x8f000ULL`, reads `0x20` (`NV_VGPU_CONFIG`), and **refuses bare-metal GP104 silicon** cleanly while accepting virtualized vGPU guests.
  - Wires `IS_VIRTUAL(pGpu)` (`g_gpu_nvoc.h`), `gpu.c` (`gpuDetermineVirtualMode`, `vgpuCreateObject`, `vgpuDestructObject`, `gpuGetStaticInfo`, `gpuGetVgpuRpc`), `objvgpu.c`, `rpc_common.c`, and `rpc_vgpu.h`.
- [`build_open_535_guest_kmod.sh`](../open-vgpu-guest/scripts/build_open_535_guest_kmod.sh): Clones tag `535.309.01`, runs `patch_open_535_guest_rm.py`, builds full open-source `nv-kernel.o`, `nv-modeset-kernel.o`, and `kernel-open/*.ko` (auto-detecting Ubuntu `/lib/modules/$KVER/build` when `/source` is absent), and asserts `modinfo` + `nm` symbols (`ALL_OPEN_535_GUEST_SYMBOLS_VERIFIED!`).

### 4.3 Observed Build Verification (`192.168.1.15`, Linux `7.0.0-43-generic`, GCC `15.2`)
Compiled from pristine `NVIDIA/open-gpu-kernel-modules@535.309.01` with **zero proprietary `.o_binary` archives**:

| Artifact | Size (Bytes) | `modinfo` License & Version | Verified Key Symbols (`nm`) |
| --- | ---: | --- | --- |
| `src/nvidia/_out/Linux_x86_64/nv-kernel.o` | `10,100,080` | Open RM core (`MIT`) | Links `open_vgpu_bar0_rpc.o` cleanly into `nvidia.ko` |
| `src/nvidia-modeset/_out/Linux_x86_64/nv-modeset-kernel.o` | `2,484,936` | Open modeset core (`MIT`) | Linked into `nvidia-modeset.ko` |
| `kernel-open/nvidia.ko` | `25,491,064` | `Dual MIT/GPL`, `535.309.01` | `registerHalModule_GP104`, `openVgpuDetectBar0VirtualMode`, `openVgpuCreateObject`, `openVgpuDestructObject`, `openVgpuBar0RpcSendMessage`, `openVgpuBar0RpcRecvPoll`, `openVgpuGetRpc` |
| `kernel-open/nvidia-modeset.ko` | `3,104,624` | `Dual MIT/GPL`, `535.309.01` | Clean `MODPOST` against open `nvidia.ko` |
| `kernel-open/nvidia-drm.ko` | `4,505,744` | `Dual MIT/GPL`, `535.309.01` | Clean `MODPOST` on `7.0.0-43-generic` |
| `kernel-open/nvidia-uvm.ko` | `57,529,720` | `Dual MIT/GPL`, `535.309.01` | Clean `MODPOST` on `7.0.0-43-generic` |

---

## 5. What Phase 2B-2 Delivers Today (Live AlmaLinux 10.3 `6.12.0-264.16.1.el10_3.x86_64_v2` Verification on Real Tesla P4 `GRID P4-4Q`)

### 5.1 Disposable Copy-on-Write Testbed & In-Guest Compilation
Live hardware testing was executed inside a temporary disposable copy-on-write QCOW2 overlay (`vm-alma-2b2`, `/var/tmp/vm-alma-2b2.qcow2` backed read-only by `/home/zero/vms/alma/disk.qcow2`) attached to a live Tesla P4 `nvidia-65` (`GRID P4-4Q`, `4096 MiB`) mediated device on `192.168.1.24`:
- Compiled all four open-source `535.309.01` kernel modules in-guest against AlmaLinux 10.3 kernel **`6.12.0-264.16.1.el10_3.x86_64_v2`** (`GCC 14.4.1`) with zero `.o_binary` blobs (`nv-kernel.o` `9.7 MiB`, `nv-modeset-kernel.o` `2.4 MiB`, `nvidia.ko` `28 MiB`, `nvidia-modeset.ko` `4.1 MiB`, `nvidia-drm.ko` `4.2 MiB`, `nvidia-uvm.ko` `58 MiB`; `ALL_OPEN_535_GUEST_SYMBOLS_VERIFIED!`).
- Loaded all four open modules (`INSMOD_NVIDIA=OK`, `INSMOD_MODESET=OK`, `INSMOD_DRM=OK`, `INSMOD_UVM=OK`) with `/sys/module/nvidia/version: 535.309.01` and `/sys/module/nvidia/taint: OE` (`Dual MIT/GPL`, zero proprietary `P` taint flag).

### 5.2 Five Hardware BAR0 `0x8f000` `"VRPC"` Protocol Invariants Verified via `rea@6.10.1` + Live `nvidia-vgpu-mgr`
Live testing against the real host `libnvidia-vgpu.so.535.309.01` combined with targeted `rea@6.10.1` decompilation (`0x261260.c`, `0x261750.c`, `0x262150.c`, `0x1ec920.c`, `0x1e9560.c`, `0x1dfa80.c`, `0x18e7e0.c`, `0x1a2f60.c`) established and verified five critical wire protocol invariants in [`open_vgpu_bar0_rpc.c`](../open-vgpu-guest/overlay/open_vgpu_bar0_rpc.c) and [`patch_open_535_guest_rm.py`](../open-vgpu-guest/scripts/patch_open_535_guest_rm.py):

1. **`GP_IN_RING` (`0x0000080b`) Shared-Memory Ring Descriptor Layout (`_nv007409vgpu` @ `0x261750.c`)**:
   - Real `libnvidia-vgpu.so.535.309.01` checks bit 11 (`0x800`, `GP_IN_RING`) on `SEND_RING_LO` (`0x8f000`) and `RECV_RING_LO` (`0x8f010`). When programmed with `(pfnLo << 12) | 0x0000080bU` and `0x3ffU` interrupt enable at `0x8f020`, the host reads the 64-bit `msgPage` PTE from `sendRing[0..1]` (`+0x00`), `SEND_PUT` from `sendRing[2]` (`+0x08`), `RECV_PUT` from `recvRing[2]` (`+0x08`), and `RECV_GET` from `recvRing[3]` (`+0x0c`), triggered by writing token `0U` to `0x8f028` (`NV_VGPU_NOTIFY`).
2. **Clean Ring Invalidation (`0x2f` `UNLOADING_GUEST_DRIVER` + LO-Before-HI Register Clear, `_nv007455vgpu` @ `0x261260.c`)**:
   - Host MMIO handler `_nv007455vgpu` re-validates the ring whenever `*_RING_LO` still has bit 3 (`VALID = 0x8`) set — even on writes to `*_RING_HI`. Sending RPC `0x2f` (`UNLOADING_GUEST_DRIVER`, 8-byte zero payload) and clearing `*_RING_LO = 0` **before** `*_RING_HI = 0` produces clean `vmiop_log: (0x0): Guest driver unloaded!` across repeated `rmmod`/`insmod` hot-reload cycles without rebooting the guest VM.
3. **Early `vgpuCreateObject(pGpu)` Hook Before `gpuCreateChildObjects()`**:
   - `kbusConstructEngine_IMPL` runs during `gpuCreateChildObjects(pGpu, NV_FALSE)` and invokes `GPU_GET_PHYSICAL_RMAPI(pGpu)->Control(NV2080_CTRL_CMD_BUS_GET_PCI_BAR_INFO)`. Calling `vgpuCreateObject(pGpu)` immediately before `gpuCreateChildObjects()` in `gpuConstructPhysical()` ensures the BAR0 VRPC rings and `0x50`/`0x9c` static info caches are active before child engines construct.
4. **`0x1a` (`RM_API_CONTROL`) `0x40`-Byte Header Framing + `0x20801112` Custom 23-Dword XDR Unpacker + `0x0a`/`0x15` Handle Tracking**:
   - Host `_nv005240vgpu` (`0x1ec920.c`) requires `pHdr->length == 0x40U` (`sizeof(rpc_message_header_v) + 0x20U` parameter header), with inline control payload copied at `+0x40U` in the shared 4 KiB RPC page.
   - Host `_nv003641vgpu` (`0x1dfa80.c` `LAB_001e00f3`) packs `NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE` (`0x20801112`) on the wire as `32 x 23` dwords (`0x17` dwords/entry, omitting `pbdmaFaultIds[2]`) followed by `numEntries` at `wire32[0x2e1]` and `bMore` at `wire32[0x2e2]`.
   - Host `_nv010420vgpu` (`0x18e7e0.c`) requires the exact parent handle `hParent` on `0x0a` (`FREE`, tracked in `OPEN_VGPU_HANDLE_REC[1024]`), and `_nv005308vgpu` (`0x1a2f60.c`) handles `0x15` (`DUP_OBJECT`, `0x18`-byte payload).
5. **Local Synthesis of GSP-Only `NV2080_CTRL_CMD_INTERNAL_*` Controls Absent on Legacy `535.309.01` Host RM**:
   - Because legacy non-GSP `535.309.01` host `libnvidia-vgpu.so` rejects GSP-RM internal subdevice controls (`0x20800a*`) with `0xff100001`, `openVgpuRmApiControl()` synthesizes `0x20801803` (`BUS_GET_PCI_BAR_INFO`), `0x20800a36` (`INTERNAL_GPU_GET_CHIP_INFO` with `NV_REG_BASE_GR=0x400000`, `TIMER=0x9000`, `MASTER=0`, `USERMODE=0x810000`), `0x20800a41` (`USER_REGISTER_ACCESS_MAP`), `0x20800a42` (`CONSTRUCTED_FALCON_INFO`), `0x20800a59` (`GMMU_GET_STATIC_INFO`), `0x20800aac` (`BIF_GET_STATIC_INFO`), `0x20800a5c` (`INTR_GET_KERNEL_TABLE`), `0x20800a1c` (`MEMSYS_GET_STATIC_CONFIG`), `0x20800ac5` (`MEMMGR_GET_VGPU_CONFIG_HOST_RESERVED_FB`), `0x20800a61` (`FIFO_GET_NUM_CHANNELS`), and `0x20800a80`/`0x20802609`/`0x2080302c` locally.

### 5.3 Observed Live Execution Milestone & Architectural GSP-RM vs. Legacy Pascal vGPU Boundary
- **Verified Live Milestone (`vm-alma-2b2` `dmesg` + Host `192.168.1.24` `nvidia-vgpu-mgr`)**:
  - Guest `dmesg`:
    - `NVRM: Accepting virtualized Pascal GP104 vGPU guest via clean-room BAR0 VRPC (cfg=0x040882f1)`
    - `NVRM: loading NVIDIA UNIX Open Kernel Module for x86_64  535.309.01  Release Build`
  - Host `nvidia-vgpu-mgr`:
    - `notice: vmiop_log: ######## Guest NVIDIA Driver Information: ########`
    - `notice: vmiop_log: Driver Version: 535.309.01`
    - `notice: vmiop_log: vGPU version: 0x120001`
    - `notice: vmiop_log: (0x0): Guest driver unloaded!` (**zero** `0xff100001` or `0xff100002` VRPC errors on the final build).
  - Execution completed PCI probe, full BAR0 `"VRPC"` handshake (`0x01`, `0x40`, `0x50` `67,416` B, `0x9c` `31,536` B), `gpuConstructPhysical`, `kbusConstructEngine_IMPL`, `gpuStatePreInit`, `_gpuAllocateInternalObjects` (`0x02` `0xc1e00001`, `0x20` `0xcaf00000`, `0x13` `0xcaf00001`), `_gpuInitChipInfo`, **all of `gpuStateInit`** (advancing past `RmInitNvDevice` line `916` into **`gpuStateLoad` at `RmInitNvDevice` line `945`**), and clean `0x0a` `FREE` + `0x2f` `UNLOADING_GUEST_DRIVER` teardown.
- **Architectural Boundary in Upstream `NVIDIA/open-gpu-kernel-modules@535.309.01`**:
  - Upstream `open-gpu-kernel-modules` was engineered exclusively as a **Turing+ (`TU102+`) GSP-client split RM**. Even with `GP104` chip ID registration enabled, its compiled `KernelFifo`, `Intr`, and `KernelGraphics` engine HALs (`intr_tu102.c` `NV_CTRL` subtree/leaf interrupt tree, `kernel_fifo_gv100.c` Volta+ usermode doorbell mapping and GSP channel-count queries, and `kernel_graphics.c`'s 12-control `NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_*` suite) assume a Turing+ GSP-RM firmware backend rather than legacy Pascal `NV_PMC_INTR` + non-GSP vGPU host RM.
  - Production `vm-alma` (`192.168.1.68`) subsequently upgraded to the unified R580 (`580.196.07-grid`) stack (`nv-pci.c` `10de:1bb3` 2-line open-C patch, `-m=kernel`, X11-stripped pure Wayland + CDI Selkies, CUDA `13.0`, `gridd-unlock-patcher`), while `open-vgpu-guest/` provides the complete, live-verified open-source BAR0 `"VRPC"` guest transport and zero-blob RM bridge research.
- **Rank 2 & Rank 3A/3B Enhancements (`2026-11-23`)**:
  - **Rank 2 (`open-vgpu-guest/` & `kmods/nova_vgpu_guest/`)**: Adds `open_vgpu_guest_alloc_object()` (`0x09` `ALLOC_OBJECT`) and `open_vgpu_guest_unmap_bar1()` (`0x0d` `UNMAP_MEMORY`) for all five Pascal GP104 engine classes (`NVC0B7_VIDEO_ENCODER = 0xc0b7`, `NVC1C0_PASCAL_COMPUTE_A = 0xc1c0`, `NVC197_PASCAL_A = 0xc197`, `NVC1B5_DMA_COPY = 0xc1b5`, `NVC1B0_VIDEO_DECODER = 0xc1b0`), `use_gp_in_ring` (`0x80b`) support + LO-before-HI ring teardown in `nova_vgpu_guest.c`, and `--probe-535-engines` in `test_guest_ioctl.c`.
  - **Rank 3A (`patch_open_535_guest_rm.py` & `open_vgpu_bar0_rpc.c`)**: Patches `kfifoRunlistQueryNumChannels_KERNEL` (`kernel_fifo.c:1259`) to query `NV2080_CTRL_CMD_INTERNAL_FIFO_GET_NUM_CHANNELS` when `IS_GSP_CLIENT(pGpu) || IS_VIRTUAL(pGpu)`, patches `intrGetUvmSharedLeafEnDisableMask_TU102` (`intr_tu102.c:760`) to short-circuit `NV_CTRL` leaf assertions when `IS_VIRTUAL_WITHOUT_SRIOV(pGpu)`, and serves all `12` `NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_*` controls (`0x20800a1f..0x20800a3f`) locally from `pState->staticInfo.pGrStaticInfoRaw` (`0x9c` `31,536` B) and GP104 (`4` GPC / `20` TPC) geometry.
  - **Rank 3B (`open-host-rm/`)**: Implements `libopen-host-rm-gp104.so` encapsulating the host physical GP104 RM HAL slice (`8/8` Category #1 signed Falcon WPR1/ACR LSF bootstrap headers matching `BLOB_MANIFEST.json`, physical `NV_PMC_ENABLE`/BAR1/BAR2 bringup, and `22`-type Tesla P4 vGPU / 50-50 `GRID P4-4Q` `2 x 2048` CHID partitioning).
