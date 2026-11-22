# 12 — Open `vmiop` Plugin (`libopen-nvidia-vgpu.so`), Cold-Boot Fixes & Live 2-VM VRPC Trace Catalog

## 1. Overview & Status

This document records the design, binary interface parity, bug fixes, and live 2-VM wire-level traces for **Phase 1A** of `docs/10-OPEN-SOURCE-ROADMAP-AND-EFFORT-ESTIMATE.md`: the clean-room `vmiop` host device mediator (`open-vgpu-plugin/` -> `libopen-nvidia-vgpu.so`), paired with the `open-vgpu-mgr` trace tap (`open-vgpu-mgr/vmiop_env.c`) and the cold-boot `/dev/nvidiactl` character device fix (`nova-vgpu/src/rm_ioctl.rs`).

> **Scope Boundary:** In accordance with project policy, `nvidia-gridd` and NVIDIA vGPU DLS/license enforcement (`libnvidia-vgpu.so` guest license verification inside VM RM RPCs) remain untouched. During live 2-VM operation on `192.168.1.24`, `libopen-nvidia-vgpu.so` runs in **hybrid interposer mode** alongside `/opt/nvidia-vgpu-535/lib/libnvidia-vgpu.so.535.309.01`, while `open-vgpu-plugin/` ALSO provides a **standalone GP104 clean-room core** (`gp104_vgpu_core.c`) verified via self-contained unit tests with zero proprietary plugin dependency.

---

## 2. REA `6.10.1` Evidence & `vmiop` Binary ABI

Generated on `192.168.1.15` via `rea --json` and stored under `evidence/vgpu-535-rea610/`:
- [`libnvidia-vgpu.so.535.309.01.exports.json`](../evidence/vgpu-535-rea610/libnvidia-vgpu.so.535.309.01.exports.json) (`1,180,654` bytes)
- [`libnvidia-vgpu.so.535.309.01.exposure.json`](../evidence/vgpu-535-rea610/libnvidia-vgpu.so.535.309.01.exposure.json) (`9,596` bytes)
- [`libnvidia-vgpu.so.535.309.01.layout.json`](../evidence/vgpu-535-rea610/libnvidia-vgpu.so.535.309.01.layout.json) (`972` bytes)

`open-vgpu-plugin/` exports the exact versioned ELF symbol ABI expected by both upstream QEMU `vmiop` hosts and `open-nvidia-vgpu-mgr` (enforced via `open-vgpu-plugin/vmiop_plugin.map`):

| Symbol | ELF Version | Size | Role |
|---|---|---|---|
| `vmiop_display_vmiop_plugin` | `VER_5_0` | `0xa8` (`168` bytes) | Plugin descriptor (`signature = "VMIOP_PLUGIN_SIGNATURE"`, `version = 0x50000`, `plugin_class = 1`, `input_classes = 0x202`, lifecycle & attribute callbacks) |
| `vmiop_display_vmiop_env` | `VER_2_0` | `0x38` (`56` bytes) | Host callback function table bound at plugin load (`register_emul_device_v2`, `create_guest_region_v2`, `map_guest_memory_v2`, `inject_msi`, `unpin_pages`, etc.) |

### Operating Modes in `libopen-nvidia-vgpu.so`
1. **Standalone GP104 Mode (`gp104_vgpu_core.c`)**: Selected automatically when `OPEN_VGPU_PLUGIN_STANDALONE=1` or when no vendor `.so` is loaded. Emulates PCI config space (`10de:1bb3`, `subsys 10de:1206`), BAR0/BAR1/BAR3 sizing & relocation (`16 MiB` / `256 MiB` / `32 MiB`), BAR0 registers (`NV_PMC_BOOT_0 = 0x134000a1`, `NV_PFB_CSTATUS`, `NV_PTIMER`, `0x088068 = 0x00810005`), BAR0 `0x8f000` mailbox/rings, `VMMU`, `CHIDMGR`, and VRPC dispatch.
2. **Hybrid Interposer Mode (`open_vgpu_plugin.c`)**: Selected by default when `libnvidia-vgpu.so.535.309.01` is present in `OPEN_VGPU_VENDOR_PLUGIN` / `/opt/nvidia-vgpu-535/lib/libnvidia-vgpu.so.535.309.01`. Forwards `vmiop_display_vmiop_plugin` calls and host `vmiop_env_v2_t` bindings transparently while logging structured lifecycle events when `OPEN_VGPU_TRACE_DIR` is set.

---

## 3. Root-Cause Fixes Discovered & Verified on Hardware

### 3.1 Cold-Boot `/dev/nvidiactl` & `/dev/nvidia0` Race (`open-nvidia-vgpud`)
- **Symptom**: On a fresh cold boot before `nvidia-smi` ran, `nvidia-kernel` did not yet auto-create `/dev/nvidiactl` (`195:255`) or `/dev/nvidia0` (`195:0`), causing `open-nvidia-vgpud` (`nvidia-vgpud.service`) to fail on first start.
- **Fix**:
  - `nova-vgpu/src/rm_ioctl.rs`: Added `parse_chardev_major()`, `linux_makedev()`, and `LinuxRmTransport::ensure_nvidia_char_node()` (reading `"nvidia-frontend"` major from `/proc/devices`, defaulting to `195`, creating missing `/dev/nvidiactl` (`255`) and `/dev/nvidiaN` (`N`) nodes prior to `open()`), covered by unit test `test_parse_chardev_major_and_makedev`.
  - `scripts/install_vgpu_host_dkms.sh`: Added `ExecStartPre` mknod guard to `nvidia-vgpud.service`.
- **Verification**: Cold-boot tested on `192.168.1.24`: `nvidia-vgpud` registered all `12` Tesla P4 profiles (`RM mode 3`, `535.309.01`), creating `/sys/bus/pci/devices/0000:01:00.0/mdev_supported_types/nvidia-65` (`GRID P4-4Q`) automatically at boot.

### 3.2 `nvidia_vma_access` Kernel Panic Avoidance in `open-vgpu-mgr` Trace Tap
- **Discovery**: Attempting to read guest mmap'd VFIO/vGPU ring VA pages via `pread(/proc/self/mem, ...)` inside the trace tap triggered a Linux kernel `BUG: kernel NULL pointer dereference` inside `nvidia_vma_access` (`[nvidia]`) called from `__x64_sys_pread64`.
- **Fix**: Removed `/proc/self/mem` completely from `open-vgpu-mgr/vmiop_env.c`; replaced it with direct pointer reads guarded by `msync(page_base, 0x1000, MS_ASYNC) == 0`, eliminating the kernel fault completely while keeping zero overhead when `OPEN_VGPU_TRACE_DIR` is unset.

---

## 4. Live 2-VM Trace Capture (`192.168.1.24`, Concurrent `GRID P4-4Q`)

Captured on `192.168.1.24` with `open-nvidia-vgpu-mgr` + `OPEN_VGPU_PLUGIN_LIB=libopen-nvidia-vgpu.so` + `OPEN_VGPU_TRACE_DIR=/tmp/vgpu-trace` while booting both `nvidia-65` (`GRID P4-4Q`, `4000MiB` each) VMs concurrently to `Licensed` steady state, followed by graceful shutdown via `vgpu-vms.service`:

### 4.1 Plugin Lifecycle Call Sequence (`vgpu_plugin_pid*.jsonl`)
Both `vm-alma` (`PID 3149`) and `vm-win11` (`PID 3202`) executed the exact same `8`-call `VER_5_0` sequence (`rc = 0`, `mode = "hybrid"`):
1. `plug_set_attr("vgpu_type_id")` (`rc=0`)
2. `plug_set_attr("gpu-pci-id")` (`rc=0`)
3. `plug_set_attr("vgpu-dev-name")` (`rc=0`)
4. `plug_get_attr("vmiop_vgpu_cap")` (`rc=0`)
5. `plug_set_attr("vmiop_vmm_migration_supported")` (`rc=0`)
6. `plug_init("init_routine")` (`rc=0`)
7. `plug_reset("reset_device")` (`rc=0`)
8. `plug_shutdown("shutdown_routine")` (`rc=0`)

### 4.2 High-Level Comparison (`vm-alma` vs `vm-win11`)

| Metric | `vm-alma` (`PID 3149`, Guest `535.309.01`) | `vm-win11` (`PID 3202`, Guest `539.72`) |
|---|---|---|
| Guest Profile / License | `nvidia-65` (`GRID P4-4Q`), `Licensed` | `nvidia-65` (`GRID P4-4Q`), `Licensed` |
| `region_create` (`0x1` / `0x2` / `0x3`) | `16 MiB` / `256 MiB` / `32 MiB` | `16 MiB` / `256 MiB` / `32 MiB` |
| Initial Relocated GPAs | `0x81000000` / `0x800000000` / `0x810000000` | `0x81000000` / `0x800000000` / `0x810000000` |
| `region_map` calls (`add=1` / `add=0`) | `22` (`11` / `11`) | `22` (`11` / `11`) |
| `mem_map` / `mem_unmap` calls | `132` / `132` | `23` / `23` |
| `pin_calls` / `pin_pages` | `122` / `59,074` (`~230.8 MiB`) | `111` / `5,693` (`~22.2 MiB`) |
| `msi_count` | `5,001` | `4,996` |
| `pci_cfg` accesses | `2,256` | `2,691` |
| BAR0 `0x8f000..0x8ffff` MMIO accesses | `2,793` | `7,462` |
| Non-`0x8f000` BAR0 MMIO accesses | `30,411` (`4,000` logged, `26,411` suppressed) | `28` logged (`0` suppressed) |
| Unique VRPC Opcodes (`vrpc_req` total) | `34` (`2,764` req / `22` sampled rep) | `39` (`7,430` req / `727` sampled rep) |

> Note on `vrpc_rep` counts: `open-vgpu-mgr` logs `vrpc_rep` only on sampled synchronous ring phases (`doorbell_post` / `mem_unmap`) when status transitions away from `0xffffffff`; every sampled `vrpc_rep` reported `status = 0x0`.

---

## 5. Observed PCI Config, Non-`0x8f000` BAR0 & `0x8f000` Registers

### 5.1 PCI Config Space Reads & Writes (`pci_cfg`)
- **Identity & Subsystem**: `0x00 = 0x1bb310de` (Tesla P4), `0x08 = 0x030000a1` (VGA Controller rev `0xa1`), `0x2c = 0x120610de` (`GRID P4-4Q`), `0x3d = 0x01` (`INTA#`).
- **Capability Chain**: `0x34 = 0xd0` -> NVIDIA vGPU Vendor Cap (`0xd0 = 0x561b6809`: cap ID `0x09`, next `0x68`, len `0x1b`, `'V','F', 0x00, 0x16`, payload `"535.309.01r539_72"` across `0xd7..0xe7`) -> MSI Cap (`0x68 = 0x00800005`, next `0x00`, guest writes MSI vector/addr at `0x6a`, `0x6c`, `0x70`, `0x74`).
- **Observed Non-Zero Device Words (`0x50..0x58`)**: `0x50 = 0x1`, `0x54 = 0x1`, `0x58 = 0x0023d6ce`.

### 5.2 Non-`0x8f000` BAR0 MMIO Accesses (Normalized `bar_off & 0xffffff`)

| BAR0 Offset | Meaning / Role | `vm-alma` (`PID 3149`) | `vm-win11` (`PID 3202`) |
|---|---|---|---|
| `0x000000` | `NV_PMC_BOOT_0` | R `17`x (`0x134000a1`) | R `1`x (`0x134000a1`) |
| `0x000004` | `NV_PMC_ENABLE` | R `5`x (`0x10100`) | R `3`x (`0x10100`) |
| `0x000100` | `NV_PMC_INTR_0` | R `30`x (`0x0`, `0x80000000`) | R `1`x (`0x80000000`) |
| `0x000140` | `NV_PMC_INTR_EN_0` | R `2,939`x (`0x0`, `0x80000000`) | R `5`x (`0x0`, `0x80000000`) |
| `0x0001a0` | PMC guest ack/write | W `2`x (`0x0`, `0x1`) | W `2`x (`0x0`, `0x1`) |
| `0x000a00` | `NV_PMC_BOOT_42` | R `3`x (`0x134a1000`) | R `2`x (`0x134a1000`) |
| `0x001074` | `NV_PBUS` scratch | R `1`x (`0x0`) | R `1`x (`0x0`) |
| `0x002a78` | `NV_PBUS` scratch | R `1`x (`0x0`) | R `1`x (`0x0`) |
| `0x009100` | Timer / config | R `29`x (`0x0`) | — |
| `0x009400` | `NV_PTIMER_TIME_0` | R `318`x (`ns_lo`) | R `4`x (`ns_lo`) |
| `0x009410` | `NV_PTIMER_TIME_1` | R `637`x (`ns_hi`) | — |
| `0x021584` | `NV_PFUSE` / master | R `1`x (`0x0`) | R `1`x (`0x0`) |
| `0x070004` | VBIOS / PRAMIN window | R `3`x (`0x0`), W `3`x (`0x1`) | — |
| `0x070010` | VBIOS / PRAMIN window | R `3`x (`0x0`), W `3`x (`0x1`) | — |
| `0x088068` | PCIe/virtual config | R `1`x (`0x810005`) | R `2`x (`0x810005`) |
| `0x088080`..`0x088430` | PCIe virtual regs (`0x88080`, `0x8814c`, `0x88424`, `0x88430`) | R `1`x each (`0x0`) | R `1`x each (`0x0`) |
| `0x9a0204` | Bar0 probe (`Win11`) | — | R `1`x (`0x0`) |

### 5.3 BAR0 `0x8f000` (`NV_VGPU`) Mailbox Registers

| BAR0 Offset | `vm-alma` (`PID 3149`) | `vm-win11` (`PID 3202`) | Role |
|---|---|---|---|
| `0x8f000` / `0x8f004` | W `2x/1x` (`0x2337e80b` / `0x1`, then `0x0`) | W `2x/1x` (`0xffff880b` / `0x1`, then `0x0`) | Send ring GPA + enable |
| `0x8f010` / `0x8f014` | W `2x/1x` (`0x2338180b` / `0x1`, then `0x0`) | W `2x/1x` (`0xffff780b` / `0x1`, then `0x0`) | Recv ring GPA + enable |
| `0x8f020` | R `6x` (`0x40882f1`), W `1x` (`0x0`) | R `5x` (`0x40882f1`), W `1x` (`0x0`) | `NV_VGPU_CONFIG` (`0x040882f1`) |
| `0x8f028` / `0x8f02c` | R `1x` (`0x0`), W `1x/1x` (`0x0` / `0x3ff`) | R `1x` (`0x0`), W `1x/1x` (`0x0` / `0x3ff`) | Intr status / enable (`0x3ff`) |
| `0x8f030` / `0x8f034` | W `2x/1x` (`0x675300b` / `0x1`, then `0x0`) | W `2x/1x` (`0xffff900b` / `0x1`, then `0x0`) | Event ring GPA + enable |
| `0x8f038` / `0x8f03c` | R `1x` (`0x0`), W `1x` (`0x0`) | R `1x` (`0x0`), W `1x` (`0x0`) | Event ring put / get |
| `0x8f050` | W `2,764x` (`0x0`) | W `7,429x` (`0x0`) | **RPC Doorbell** |
| `0x8f058` / `0x8f05c` | W `1x` (`0xa`), R `1x` (`0x0`) | W `1x` (`0x1a`), R `1x` (`0x0`) | Guest OS tag (`0xa` Linux / `0x1a` Win) & info |
| `0x8f068` | — | W `2x` (`0x0`) | Control reset/ack |
| `0x8f080` / `0x8f084` | W `2x/1x` (`0x2338700b` / `0x1`, then `0x0`) | W `2x/1x` (`0xffff600b` / `0x1`, then `0x0`) | Auxiliary shared ring 2 |
| `0x8f088` / `0x8f08c` | — | W `2x/1x` (`0xffff500b` / `0x1`, then `0x0`) | Auxiliary shared ring 3 (Win11) |
| `0x8f090` / `0x8f094` | W `2x/1x` (`0x8aef00b` / `0x1`, then `0x0`) | W `2x/1x` (`0xffff400b` / `0x1`, then `0x0`) | Auxiliary shared ring 4 |

---

## 6. Complete Live VRPC Opcode Catalog (`PID 3149` + `PID 3202`)

Every VRPC message observed used header version `0x03000000` (bytes `00 00 00 03 56 52 50 43` = `"VRPC"`) and valid response status `0x0`. Below is the complete union of opcodes across both guests (`34` on `vm-alma`, `39` on `vm-win11`, `26` shared + `8` Linux-only + `13` Windows-only = `47` distinct opcodes total):

| Opcode | Plugin Symbol / Role | `vm-alma` (`PID 3149`) req (len) | `vm-win11` (`PID 3202`) req (len) |
|---|---|---|---|
| `0x01` | `VRPC_FN_SET_GUEST_SYSTEM_INFO` | `1` (`824`) | `1` (`824`) |
| `0x02` | `VRPC_FN_ALLOC_ROOT` | `43` (`140`) | `50` (`140`) |
| `0x04` | `VRPC_FN_ALLOC_MEMORY_535` (`ALLOC_MEMORY` + PFN pin) | `122` (`88`) | `31` (`88`) |
| `0x05` | `VRPC_FN_ALLOC_CTX_DMA_535` (`ALLOC_CTX_DMA`) | `5` (`88`) | — |
| `0x06` | `VRPC_FN_ALLOC_CHANNEL_DMA_535` (`ALLOC_CHANNEL_DMA` + USERD) | `42` (`280`) | `31` (`280`) |
| `0x07` | `VRPC_FN_MAP_MEMORY_535` (`MAP_MEMORY` BAR1 + `region_map`) | `3` (`88`) | `6` (`88`) |
| `0x09` | `VRPC_FN_ALLOC_OBJECT_535` (`ALLOC_OBJECT`) | `107` (`112`) | `214` (`112`) |
| `0x0a` | `VRPC_FN_FREE_535` (`FREE`) | `513` (`48`) | `9` (`48`) |
| `0x0c` | `VRPC_FN_ALLOC_VIDMEM_535` (`ALLOC_VIDMEM`) | `449` (`128`) | `9` (`128`) |
| `0x0e` | `VRPC_FN_MAP_MEMORY_DMA` (`NV_ESC_RM_MAP_MEMORY_DMA`) | `539` (`88`) | `54` (`88`) |
| `0x0f` | `VRPC_FN_UNMAP_MEMORY_DMA` (`NV_ESC_RM_UNMAP_MEMORY_DMA`) | `328` (`72`) | — |
| `0x13` | `VRPC_FN_SET_OBJECT` (`ALLOC_SUBDEVICE`) | `47` (`72`) | `50` (`72`) |
| `0x15` | `VRPC_FN_DUP_OBJECT_535` (`DUP_OBJECT`) | `14` (`60`) | `28` (`60`) |
| `0x17` | `VRPC_FN_IDLE_CHANNELS_V535` (`IDLE_CHANNELS`) | `24` (`60`) | `11` (`60`) |
| `0x1a` | `VRPC_FN_RM_API_CONTROL` (`RM_API_CONTROL`) | `78` (`64`) | `125` (`64`) |
| `0x1b` | `VRPC_FN_FILL_PTE_MEM` (`0x00801802`) | — | `3,804` (`136`) |
| `0x1c` | `VRPC_FN_ALLOC_HWRES` (`NV01_MEMORY_HW_RESOURCES` `0xb1`) | — | `17` (`152`) |
| `0x20` | `VRPC_FN_SET_GUEST_FB_INFO` (`ALLOC_DEVICE` `0x80`) | `43` (`96`) | `50` (`96`) |
| `0x21` | `VRPC_FN_DEFERRED_API_535` (`NV5080_CTRL_CMD_DEFERRED_API_V2`) | — | `6` (`128`) |
| `0x2b` | `VRPC_FN_MAP_SEMA_MEM_535` (`MAP_SEMA_MEM`) | — | `5` (`56`) |
| `0x34` | `VRPC_FN_ALLOC_VIRTMEM_535` (`NV50_MEMORY_VIRTUAL` `0x50a0`) | `165` (`128`) | `11` (`128`) |
| `0x38` | `VRPC_FN_UPDATE_PDE_2` (`0x00801809`) | — | `2,777` (`96`) |
| `0x39` | `VRPC_FN_SET_LICENSE_STATE` (`0xa0820104`) | `2` (`52`) | `2` (`52`) |
| `0x3b` | `VRPC_FN_SET_SEMA_VALIDATION` | — | `1` (`36`) |
| `0x3d` | `VRPC_FN_UPDATE_GPU_PDES` (`0x00801808`/`0x0080180a`) | — | `4` (`80`) |
| `0x3e` | `VRPC_FN_GET_ENCODER_CAPACITY` (`0xa082010d`) | `3` (`44`) | — |
| `0x3f` | `VRPC_FN_VGPU_PF_REG_READ32` | `6` (`48`) | `6` (`48`) |
| `0x40` | `VRPC_FN_SET_GUEST_BRANCH_VER` (`0xa0820103`, `"r539_72"`) | `1` (`296`) | `1` (`296`) |
| `0x50` | `VRPC_FN_GET_CONSOL_STATIC_535` (`VGPU_STATIC_INFO`) | `17` (`2560, 4096`) | `17` (`2560, 4096`) |
| `0x55` | `VRPC_FN_GET_STATIC_INFO` (`0xa0820110` on `40`B wire) | — | `3` (`40`) |
| `0x5b` | `VRPC_FN_GR_CTXSW_ZCULL` (`0x20809004`) | — | `23` (`1584`) |
| `0x5c` | `VRPC_FN_FB_GET_INFO_200A` (`0x2080200a`) | `7` (`48`) | `3` (`48`) |
| `0x61` | `VRPC_FN_GPFIFO_SCHEDULE` (`0xa06c0101`/`0xa06f0103`) | `28` (`48`) | `31` (`48`) |
| `0x62` | `VRPC_FN_TSG_CTRL` (`0xa06c0103`) | `2` (`48`) | `4` (`48`) |
| `0x6e` | `VRPC_FN_SM_DEBUG_CTRL` (`0x83de0309`) | `2` (`44`) | — |
| `0x70` | `VRPC_FN_GR_GFXP_PREEMPT` (`0x20801211`) | `3` (`144`) | `7` (`144`) |
| `0x71` | `VRPC_FN_GR_CTRL_1210` (`0x20801210`) | `2` (`72`) | — |
| `0x72` | `VRPC_FN_GR_CTRL_1208` (`0x20801208`) | `3` (`64`) | `3` (`64`) |
| `0x78` | `VRPC_FN_DEV_CTRL_1812` (`0x00801812`) | — | `5` (`44`) |
| `0x79` | `VRPC_FN_PERF_CTRL_2A02` (`0x20802a02`) | `18` (`48`) | `4` (`48`) |
| `0x7a` | `VRPC_FN_ZBC_CTRL_0107` (`0x90960107`) | `135` (`96`) | — |
| `0x7c` | `VRPC_FN_PERF_CTRL_3002` (`0x20803002`) | `2` (`3120`) | — |
| `0x7d` | `VRPC_FN_SYS_CTRL_0127` (`0x00000127`) | — | `1` (`196`) |
| `0x7e` | `VRPC_FN_SYS_CTRL_013A` (`0x0000013a`) | `2` (`1392`) | — |
| `0x8e` | `VRPC_FN_FB_GET_INFO_8E` (`FbGetInfo`) | — | `4` (`476`) |
| `0x8f` | `VRPC_FN_DEV_CTRL_170F` (`0x0080170f`) | — | `14` (`56`) |
| `0x9c` | `VRPC_FN_GET_CONSOL_GR_535` (`GR_STATIC_INFO`) | `8` (`3184, 4096`) | `8` (`3184, 4096`) |

---

## 7. Phase 1B Milestone 1B-1: Host RM Bridge & RM-Backed VRPC Implementation

`open_vgpu_rm.h`, `open_vgpu_rm.c`, and `open_vrpc_rm.c` in [`open-vgpu-plugin/`](../open-vgpu-plugin/) add clean-room host RM integration for standalone `libopen-nvidia-vgpu.so`:

1. **Host RM Session & `NVA081` / `NVA082` Bridge (`open_vgpu_rm.c`)**:
   - Attaches `/dev/nvidiactl` + `/dev/nvidia0`, negotiates `NV_ESC_CHECK_VERSION_STR`, `NV_ESC_CARD_INFO`, `NV01_ROOT`, `NV0000_CTRL_CMD_GPU_ATTACH_IDS`, `NV01_DEVICE_0`, `NV20_SUBDEVICE_0`, `NVA081_VGPU_CONFIG` (`0x2a`), and allocates `NVA082_HOST_VGPU_DEVICE` (`0xa082`, `0x70`-byte params verified from `0x186a60.c`) when an active vGPU instance is present.
   - Maintains guest-to-host `hClient` translation (`GP104_MAX_GUEST_CLIENTS = 64`) and guards reserved plugin handle ranges `((dev_handle << 24) | 0xff0000..0xffffff)` (`0xff100007`).
   - Verified directly via `--live-rm-probe` against real Tesla P4 `nvidia.ko` (`535.309.01`) on `192.168.1.24`:
     `pci=10de:1bb3 subsys=10de:11d8 virt_mode=3 profile="GRID P4-4Q" fb=0xec000000 bar1=0x10000000 frl=60`.
     *(Live hardware probing also revealed that `NVA081_CTRL_CMD_VGPU_CONFIG_GET_VGPU_TYPE_INFO` returns `profileLength` `0x100000000` at `+0x188` and `usable_fb_bytes` `0xec000000` at `+0x190`, which is handled cleanly in `open_vgpu_rm_attach`.)*

2. **32 RM-Backed / Session / Control VRPC Handlers (`open_vrpc_rm.c`)**:
   - **Session & `NVA082`**: `0x01`, `0x39` (validates expiry `< 5` and forwards guest license state unchanged to `0xa0820104`), `0x3b`, `0x3e` (`0xa082010d`), `0x3f` (BAR0 PF `reg_read32`), `0x40` (`0xa0820103`), `0x50` / `0x9c` (chunked static info / GR static info transfers), `0x55` (`0xa0820110` on `40`B wire frame + legacy `56`B compatibility).
   - **RM Client/Object Lifecycle**: `0x02` (`ALLOC_ROOT` + `0x00000902`), `0x0a` (`FREE`), `0x13` (`ALLOC_SUBDEVICE`), `0x15` (`DUP_OBJECT` `0x34`), `0x17` (`IDLE_CHANNELS`), `0x20` (`ALLOC_DEVICE` `NV01_DEVICE_0`).
   - **RM Controls (`_nv000772vgpu` forwarders & FB info)**: `0x1a` (inline `RM_API_CONTROL`), `0x5b` (`0x20809004`), `0x5c` (`0x2080200a`), `0x61` (`0xa06c0101`/`0xa06f0103`), `0x62` (`0xa06c0103`), `0x6e` (`0x83de0309`), `0x70` (`0x20801211`), `0x71` (`0x20801210`), `0x72` (`0x20801208`), `0x78` (`0x00801812`), `0x79` (`0x20802a02`), `0x7a` (`0x90960107`), `0x7c` (`0x20803002`), `0x7d` (`0x00000127`), `0x7e` (`0x0000013a`), `0x8e` (`FbGetInfo`), `0x8f` (`0x0080170f`).

3. **Phase 1B Milestone 1B-2 (`open_vrpc_mmu.c`) — All `47/47` Cataloged VRPCs Implemented**:
   - **Memory & PFN Pinning (`0x04`, `0x05`, `0x0c`, `0x1c`, `0x34`)**: `ALLOC_MEMORY` (`vmiop_pin_guest_pages_v2` -> `NV01_MEMORY_LIST_SYSTEM` `0x81`, with automatic `unpin_pages` on `0x0a` `FREE`, `reset`, and `shutdown`), `ALLOC_CTX_DMA` (`NV01_CONTEXT_DMA`), `ALLOC_VIDMEM` (guest FB PFN bounds check against `usable_fb_bytes`), `ALLOC_HWRES` (`0xb1`), and `ALLOC_VIRTUAL_MEMORY` (`NV50_MEMORY_VIRTUAL` `0x50a0`).
   - **Channel, MMU & PTE Blit (`0x06`, `0x07`, `0x09`, `0x0e`, `0x0f`, `0x1b`, `0x21`, `0x2b`, `0x38`, `0x3d`)**: `ALLOC_CHANNEL_DMA` (exact `0x906f..0xc86f` class validation, power-of-2 `gpFifoEntries`, faithful `0x55` unlicensed channel limit, USERD `0x1000`-byte direct MMIO forwarding), `MAP_MEMORY` (`NV_ESC_RM_MAP_MEMORY` + `0x410114` + BAR1 direct MMIO forwarding), `ALLOC_OBJECT`, `MAP_MEMORY_DMA`/`UNMAP_MEMORY_DMA` (`NV_ESC_RM_MAP_MEMORY_DMA`/`UNMAP_MEMORY_DMA`), `FILL_PTE_MEM` (`0x00801802`), `DEFERRED_API_CONTROL` (`0x50800103` with exact subcmd allowlist), `MAP_SEMA_MEM`, `UPDATE_PDE_2` (`0x00801809`), and `UPDATE_GPU_PDES` (`0x00801808`/`0x0080180a`).
   - **Ref-Counted Direct MMIO Aperture Tracker (`gp104_guest_region_map`)**: Passes `bar1_region_id` / `bar0_region_id` (preserving instance ID encoding in `open-vgpu-mgr`) to `vmiop_map_guest_region` with deduplicated add/remove reference counting and zero-leak cleanup.

4. **Phase 1B Milestone 1B-3 (`open_vgpu_static_info.c`, `gp104_vgpu_core.c`) — Exact `0x50` / `0x9c` Binary Parity & Circular Rings (`COMPLETE`)**:
   - **Exact VGX `0x23.0x05` `0x50` (`VGPU_STATIC_INFO`) Layout**: Dynamically allocates `0x10758` bytes (`67,416` B = `16 * 4056 + 2520`, `17` VRPC chunks) across all `29` sub-blocks Serializer `FUN_001910a0` (`0x00000`..`0x0ccc4`), including core block 6 (`+0x00b98`, `0xfc8` B: `usable_fb_bytes`, `bar1_bytes`, `profile_bytes`, `vgpu_type_id`, `frl_fps`, `encoder_capacity`, driver/branch/profile strings, `uname`, guest strings) and `6` live host RM controls (`0x20803001`, `0x2080170d`, `0x20801328`, `0x20801112`, `0x20801348`, `0x2080170e`, `0x20802a03` 8B `ceEngineType=9`).
   - **Exact VGX `0x23.0x05` `0x9c` (`GR_STATIC_INFO`) Layout**: Dynamically allocates `0x7b30` bytes (`31,536` B = `7 * 4056 + 3144`, `8` VRPC chunks) across all `11` sub-blocks `FUN_00190f30` (`0x0000`..`0x07b28`), including the exact clean-room `_nv007054vgpu` (`0x239210.c`) `0x940` B RM -> `0x900` B wire `GPC_MASK` packer and all `11` host RM `0x20800a22`..`0x20800a48` controls, with deterministic GP104 Pascal defaults (`4` GPCs / `20` TPCs / `2560` CUDA cores) when no `NVA082` session is active.
   - **Bounds & Circular Ring Parity**: `handle_vrpc_static_chunk` enforces exact `NV_VGPU_ERR_INVALID_CHUNK_RANGE` (`0xff000001`, matching `-0xffffff` in `_nv005312vgpu`/`_nv005310vgpu`) on out-of-range offsets or zero lengths; `handle_doorbell_dispatch` advances circular `SEND_RING_GET` and `RECV_RING_PUT` descriptors so both ring-waiting guests (`nova_vgpu_guest.ko`, `open_vgpu_guest_rpc.c`) and single-page test callers work identically.
   - **Live Tesla P4 Hardware RM Verification (`192.168.1.24`, `--live-rm-probe`)**: Verified `static_info=0x10758(67416 B)` with `ok_mask=0x7d` (`6/7` `0x50` controls `NV_OK`: `0x20803001`, `0x20801328`, `0x20801112`, `0x20801348`, `0x2080170e`, `0x20802a03`) and `gr_static_info=0x7b30(31536 B)` (recording `intr1_st=0x56` and `kgr_st=0x56` cleanly on idle bare host without an active VFIO `NVA082` mdev).

5. **Tier 2 Started (`Phase 2A` — [`13-OPEN-GUEST-VGPU-KERNEL-DRIVER.md`](./13-OPEN-GUEST-VGPU-KERNEL-DRIVER.md))**:
   - Delivered `open-vgpu-guest/` shared BAR0 `0x8f000` `"VRPC"` guest client + zero-blob host↔guest loopback (`48` circular VRPCs, zero leaks), `kmods/nova_vgpu_guest/` Linux `7.0` `kbuild` verification, and `open-gpu-kernel-modules@535.309.01` audit helper (`prepare_open_535_guest_rm.sh`).
