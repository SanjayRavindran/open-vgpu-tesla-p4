# Open-Source Host Daemon Replacements (`rea@6.10.1` Analysis & Verification)

This document details the clean-room open-source replacement of the self-contained closed userspace binaries in NVIDIA `535.309.01-vgpu-kvm`, using **`rea@6.10.1`** (`GHIDRA_INSTALL_DIR=/opt/ghidra_12.2_PUBLIC`) on `192.168.1.15` and live hardware verification on the Tesla P4 KVM host `192.168.1.24`.

All static analysis evidence JSONs (`analyze`, `audit-release-exposure`, `inspect-binary-layout`, `inspect-native-exports`, `decompile-all`, and `rea_doctor`) are archived under [`evidence/vgpu-535-rea610/`](../evidence/vgpu-535-rea610/).

---

## 1. Component Strategy: Replaced vs. Intentionally Retained

To maximize open-source transparency without destabilizing live VM GPU virtualization:

| Host Component | Status | Language / License | Role |
| :--- | :--- | :--- | :--- |
| **`open-nvidia-vgpud`** | **Replaced (Open)** | Rust / GPL-2.0 (`nova-vgpu`) | Parses `/usr/share/nvidia/vgpu/vgpuConfig.xml`, queries RM PCI IDs/mode, pushes all `NVA081` (`0xa0810101`) vGPU profile definitions and `NOTIFY_START` (`0xa0810109`) to `/dev/nvidiactl` |
| **`libopen-vgxcfg.so`** | **Replaced (Open)** | C (`open-vgxcfg`) | Self-contained XML DOM parser exporting all 9 ABI-exact `vgxConfig*` functions (`soname libnvidia-vgxcfg.so.1`) |
| **`open-nvidia-xid-logd`** | **Replaced (Open)** | C (`open-xid-logd`) | Daemonizes, locks `$RUNTIME_DIRECTORY/nvidia-xid-logd.pid`, dynamically loads `libnvidia-ml.so.1`, exits cleanly (`0`) on non-GSP GPUs (`NVML_ERROR_NOT_SUPPORTED`), or logs 76-char Base64 `nvidia-debugdump` XID reports on GSP GPUs |
| **`open-nvidia-vgpu-mgr`** | **Replaced (Open)** | C (`open-vgpu-mgr`) | Full KVM/VFIO `vmiop-env` runtime daemon: binds `/dev/nvidia-vgpuctl` & `/dev/nvidia-vgpu%d`, forks per-VM `vgpu` worker processes, manages `/dev/nvidiactl` RM client (`NV_ESC_IOCTL_XFER_CMD`, `NV01_MEMORY_LIST_SYSTEM`), and exports all `60` dynamic `vmiop_*` symbols + `vmiop_presentation` to `libnvidia-vgpu.so` |
| **`open-nvidia-smi`** & **`libopen-nvidia-ml.so`** | **Replaced (Open)** | C (`open-nvml-smi`) | Clean-room NVML (`libnvidia-ml.so.1` SONAME) & `nvidia-smi` CLI: queries `/dev/nvidiactl` & `/dev/nvidiaN` RM controls (`0x2080*` + `0xa081*`) for GP104 temperature, power, clocks, FB/BAR1 memory, active `vgpu` processes, and supported/creatable/active vGPU instances |
| **`nvidia-vgpu-vfio.ko`** | **Open (DKMS)** | C / MIT (`535.309.01`) | Packaged alongside `nvidia.ko` in `/usr/src/nvidia-vgpu-kvm-535.309.01` for automatic rebuilds across Ubuntu & Proxmox kernels |
| **`libopen-nvidia-vgpu.so`** (`open-vgpu-plugin`) | **Replaced (Phase 1A & Phase 1B COMPLETE — 1B-1/1B-2/1B-3)** | C (`open-vgpu-plugin`) | Implements `vmiop_display_vmiop_plugin@@VER_5_0` (`0xa8`B) & `vmiop_display_vmiop_env@@VER_2_0` (`0x38`B), clean-room GP104 PCI/BAR0/VMMU/CHIDMGR core (`gp104_vgpu_core.c`), host RM `NVA081`/`NVA082` bridge (`open_vgpu_rm.c`), all `47/47` live VRPC handlers (`open_vrpc_rm.c`, `open_vrpc_mmu.c`), exact `0x10758`B `0x50` & `0x7b30`B `0x9c` static-info binary layouts (`open_vgpu_static_info.c`), plus live hybrid interposition/tracing alongside `libnvidia-vgpu.so.535.309.01` |
| **Open Guest vGPU Driver (`open-vgpu-guest` + `nova_vgpu_guest.ko` + `535.309.01` RM overlay)** | **Tier 2 (`Phase 2A, 2B-1 & 2B-2 Complete`)** | C (`MIT / GPL-2.0`) | Clean-room guest BAR0 `0x8f000` `"VRPC"` client (`open_vgpu_guest_rpc.c`), zero-blob host↔guest loopback (`test_host_guest_loopback.c`), `nova_vgpu_guest.ko`, and clean-room `535.309.01` RM overlay (`overlay/open_vgpu_bar0_rpc.{h,c}`, `patch_open_535_guest_rm.py`, `build_open_535_guest_kmod.sh`) verified compiling open `nv-kernel.o` & `nvidia*.ko` (`Dual MIT/GPL`, `taint: OE`) on both Ubuntu `7.0.0-43-generic` and AlmaLinux 10.3 `6.12.0-264.16.1.el10_3.x86_64_v2`, and live-tested against real Tesla P4 `GRID P4-4Q` mdev (`GP_IN_RING` `0x80b` handshake, `0x02`/`0x20`/`0x13`/`0x1a`/`0x0a`/`0x2f` VRPC lifecycle through `gpuStateInit` into `gpuStateLoad`) — see [`13-OPEN-GUEST-VGPU-KERNEL-DRIVER.md`](./13-OPEN-GUEST-VGPU-KERNEL-DRIVER.md) |
| **`nvidia.ko`** (`nv-kernel.o_binary`) | Retained (Stable) | Proprietary kernel core | Handles Pascal Falcon signed blobs (`ACR`, `PMU`, `SEC2`, `FECS`) and RM IOCTL dispatch |

---

## 2. `open-nvidia-vgpud` (Rust) — REA 6.10.1 Findings

Static decompilation of `nvidia-vgpud` (`0x401e20`, `0x4020a0`, `0x402ca0`, `0x4039f0`, `0x404440`, `0x40b950`, `0x40c7e0`, `0x40cea0`, `0x40dfc0`) revealed:

1. **Synchronous Oneshot Design**: `main` calls `0x402ca0` once to register profiles in kernel RM and immediately exits `0`; systemd runs `open-nvidia-vgpud` as `Type=oneshot` with `RemainAfterExit=yes`.
2. **Exact `/dev/nvidiactl` & `/dev/nvidiaN` Sequence** (implemented in [`nova-vgpu/src/rm_ioctl.rs`](../nova-vgpu/src/rm_ioctl.rs) and verified live on `535.309.01`):
   - `NV_ESC_CHECK_VERSION_STR` (`0xc04846d2`, `cmd=0`) with the running kernel version parsed from `/proc/driver/nvidia/version` (`535.309.01`), after `ensure_nvidia_char_node()` guarantees `/dev/nvidiactl` (`195:255`) and `/dev/nvidia0` (`195:0`) exist on cold boot via `/proc/devices` + `mknod` (covered by unit test `test_parse_chardev_major_and_makedev`)
   - `NV_ESC_CARD_INFO` (`0xc90046c8`, `32 * 0x48` bytes) mapping `gpu_id -> minor`
   - `NV_ESC_RM_ALLOC_V0` (`0xc020462b`, `32` bytes) allocating root `h_client`
   - `NV0000_CTRL_CMD_GPU_GET_PROBED_IDS` (`0x214`) -> `GET_PCI_INFO` (`0x21b`) -> `ATTACH_IDS` (`0x215`)
   - `open("/dev/nvidia0")` + `NV_ESC_REGISTER_FD` (`0xc00446c9`)
   - `GET_ID_INFO` (`0x202`) -> `dev_inst`, then `NV_ESC_RM_ALLOC` (`0xc030462b`, `48`-byte `NVOS64`) for `NV01_DEVICE_0` (`0x80`), `NV20_SUBDEVICE_0` (`0x2080`), using wrapping handle math `(gpu_id.wrapping_add(h_client) << 8) | suffix`
   - `NV2080_CTRL_CMD_BUS_GET_PCI_INFO` (`0x20801801`) extracting exact `[vendorId, deviceId, subsystemVendorId, subsystemId]` (`10de:1bb3:10de:11d8` on Tesla P4)
   - `NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE` (`0x800289` on `h_device`) returns **`3` (`HOST_VGPU`)** on the KVM host — matching `nvidia-vgpud`'s check skipping only guest modes `1` and `2`
   - `NVA081_VGPU_CONFIG` (`0xa081`) alloc on `h_subdev` -> `0xa0810119` (`SET_FRACTIONAL_MULTI_VGPU`) -> loop of `0xa0810101` (`4968`-byte `Nva081SetInfoParams`) -> `0xa0810109` (`NOTIFY_START`) -> `NV_ESC_RM_FREE` + `0x216` detach
3. **Full `Nva081SetInfoParams` (`0x1368` bytes) & License Token Expansion (`0x4039f0`)**:
   - Semicolon-separated XML `<license>` short tokens expand before RM submission:
     - `VPC` -> `GRID-Virtual-PC,2.0`
     - `VWS` -> `GRID-Virtual-WS,2.0`
     - `VWSExt` -> `GRID-Virtual-WS-Ext,2.0`
     - `VApps` -> `GRID-Virtual-Apps,3.0`
     - `QVDWS` -> `Quadro-Virtual-DWS,5.0`
     - `VGaming` -> `GRID-vGaming,8.0`
     - `VCS` -> `NVIDIA-vComputeServer,9.0`
   - Added missing offsets `+0x16c` (`gpuInstanceSize`), `+0x180` (`pgpu_device_id`), `+0x344` (`extraParameters`), `+0x1348` (`gpuDirectSupported`), `+0x134c` (`nvlinkP2PSupported`), `+0x1350` (`multiVgpuExclusive`), and wildcard `subsystemId == 0` pGPU lookup.

---

## 3. `libopen-vgxcfg.so` (C) — Differential Parity

Implemented in [`open-vgxcfg/open_vgxcfg.c`](../open-vgxcfg/open_vgxcfg.c) with a self-contained XML DOM parser and all 9 exports (`vgxConfigOpen`, `vgxConfigClose`, `vgxConfigReadGlobalSettings` (`0x210`), `vgxConfigWriteGlobalSettings`, `vgxConfigReadPhysGpu` (`0x18`), `vgxConfigReadPhysGpuSupportedUsm` (`0x18`), `vgxConfigReadUsmTypeByIndex` (`0xe4`), `vgxConfigReadUsmTypeById` (`0xe4`), `vgxConfigReadUsmTypeDisplay` (`0x10`)).

- Notice: Reverse-engineering `vgxConfigClose` (`0x103540`) showed NVIDIA's `.so` does not clear its global handle pointer after freeing, triggering a glibc double-free if closed twice; `libopen-vgxcfg.so` safely clears `g_active_handle = NULL` so duplicate closes return status `2` (`VGX_CFG_ERR_BAD_HANDLE`).
- Differential harness [`open-vgxcfg/test_vgxcfg_diff.c`](../open-vgxcfg/test_vgxcfg_diff.c) dynamically loads both NVIDIA's `.so` and `libopen-vgxcfg.so` side-by-side and verified **0 byte differences across all 9 exports** (size queries, small-buffer truncation status `4`, missing entries status `6`/`7`, valid reads).

---

## 4. `open-nvidia-xid-logd` (C) — Mock-NVML & Live Behavior

Implemented in [`open-xid-logd/open_xid_logd.c`](../open-xid-logd/open_xid_logd.c) matching `0x401430..0x4020f3`:
- Supports daemonized (`fork` + `setsid`, `Type=forking` systemd unit) and `--foreground` modes.
- Dynamically opens `libnvidia-ml.so.1` via `dlopen` (or `$OPEN_XID_NVML_LIB` in automated tests).
- Automated mock-NVML test suite ([`open-xid-logd/test_xid_logd.sh`](../open-xid-logd/test_xid_logd.sh)) verifies:
  1. Non-GSP GPU (`NVML_ERROR_NOT_SUPPORTED`) exits `0` cleanly and unlinks the locked PID file.
  2. GSP XID `79` delivery executes debugdump, Base64-encodes output into exact 76-character chunks bracketed by `XID dump start` / `XID dump end`, unlinks the temporary file, and shuts down cleanly on `SIGTERM`.
  3. On `535.309.01` live on `.24`, `nvmlDeviceGetGspFirmwareMode` returns `NVML_SUCCESS`, so `nvidia-xid-logd.service` remains active listening for XID critical events on `00000000:01:00.0` and exits cleanly on `SIGTERM` with PID file removal.

---

## 5. `open-nvidia-vgpu-mgr` (C) — Clean-Room `vmiop-env` Runtime Daemon

Implemented in [`open-vgpu-mgr/`](../open-vgpu-mgr/) (`nv_vgpu_vfio_abi.h`, `rm_client.h`, `rm_client.c`, `vmiop_env.h`, `vmiop_env.c`, `open_vgpu_mgr.c`, `test_vgpu_mgr_unit.c`, `Makefile`) from **`rea@6.10.1`** static analysis (`159` functions analyzed, `115` decompiled under `evidence/vgpu-535-rea610/nvidia-vgpu-mgr.*.json`):

1. **Master Control Loop (`0x103d70`, `0x111260..0x112b90`)**:
   - Daemonizes, locks `/var/run/nvidia-vgpu-mgr/nvidia-vgpu-mgr.pid`, opens `/dev/nvidia-vgpuctl` + `/dev/nvidiactl`, verifies `NV_VFIO_VGPU_CTLDEV_VERSION_CHECK` (`0xc0107208`), allocates RM OS events `0xdae00401`/`0xdae00402` (GPU bind/unbind) and `vgpuctl` events `1..4` (`OPEN_DEVICE`, `CREATE_DEVICE`, `DELETE_DEVICE`, `GET_TYPES_INSTANCES`), and handles `NVA081` queries (`GET_PGPU_INFO` `0xa0810112`, `GET_CREATABLE_PLACEMENTS` `0xa081011c`, `UPDATE_PLACEMENT_INFO` `0xa081011e`, `GET_VGPU_TYPE_INFO` `0xa0810103`, `CREATE_LVGPU` `0xa081011f`, `DELETE_LVGPU` `0xa081011d`).
   - On `NV_VFIO_VGPU_CTL_EVENT_OPEN_DEVICE`, forks a per-VM `vgpu` worker process (`prctl(PR_SET_NAME, "vgpu")`), resets parent RM state, parses `/proc/<qemu_pid>/cmdline` (`-uuid`, `-name`), and format-packs the physical GPU PCI BDF (`%02x:%02x:%02x.%01x`, `00:01:00.0`).
2. **RM Escape Client (`0x104920..0x109c20`)**:
   - Automatically wraps ioctl payloads `> 0x100` bytes (such as `4960`-byte `GET_VGPU_TYPE_INFO` and `1168`-byte `GET_BAR_INFO`) in `NV_ESC_IOCTL_XFER_CMD` (`0xc01046d3`).
   - Maps pinned guest PFNs via `NV01_MEMORY_LIST_SYSTEM` (`0x81`, `0x80`-byte header + PFN array) and tracks both `mmap64` user VAs and kernel `mp.pLinearAddress` handles so `NV_ESC_RM_UNMAP_MEMORY` (`0x4f`) unmaps cleanly on `fdType == 1` (`/dev/nvidiactl`) and `fdType == 2` (`/dev/nvidiaN`).
3. **Full `vmiop_*` Runtime ABI (`60` Original Exports Matched)**:
   - Exports all `60` `vmiop_*` symbols (`diff -u` zero differences against NVIDIA's `nvidia-vgpu-mgr`), dynamically loads `libnvidia-vgpu.so` (`vmiop_display_vmiop_plugin@@VER_5_0` and `vmiop_display_vmiop_env@@VER_2_0`), and provides the built-in `vmiop_presentation` VNC/console plugin (`0x68` bytes, `version=0x20000`, `class=9`).
   - Unprivileged unit test suite (`make -C open-vgpu-mgr clean all test`) validates all 6 test suites (`test_abi_and_error_table`, `test_memory_buffers_and_conversion`, `test_sync_and_threads`, `test_config_plugins_and_presentation`, `test_emul_regions_pin_map_and_rm`, `test_trace_tap_and_vrpc_capture`) with `-Wall -Wextra -Werror`. Supports zero-overhead runtime `OPEN_VGPU_PLUGIN_LIB` selection and `OPEN_VGPU_TRACE_DIR` JSONL capture (using `msync(..., MS_ASYNC)` + direct `memcpy`, avoiding `/proc/self/mem` `nvidia_vma_access` faults).

---

## 6. End-to-End Live Verification on `192.168.1.24` (`2026-11-21`)

With `nvidia-vgpu-kvm/535.309.01` installed under DKMS (`updates/dkms/nvidia.ko.zst`, `updates/dkms/nvidia-vgpu-vfio.ko.zst`) and all four open host binaries active (`open-nvidia-vgpud`, `libopen-vgxcfg.so`, `open-nvidia-xid-logd`, `open-nvidia-vgpu-mgr`):
1. **`nvidia-vgpud.service`**: Registered `12` Tesla P4 profiles (`RM mode 3`, driver `535.309.01`), creating all `12` sysfs types under `/sys/bus/pci/devices/0000:01:00.0/mdev_supported_types/` including `nvidia-65` (`GRID P4-4Q`, `available_instances = 2`).
2. **`nvidia-xid-logd.service`**: Started cleanly (`Waiting for events...` on `00000000:01:00.0`), exited cleanly on `SIGTERM`, and unlinked its locked PID file.
3. **`vm-alma` (`192.168.1.68`) + `vm-win11` (`192.168.1.65`) Concurrent 50/50 Boot**:
   - Host `nvidia-smi` showed both `vgpu` processes (`4000MiB` each, `8032MiB / 8192MiB`).
   - `vm-alma`: `GRID P4-4Q` (`535.309.01`, `Licensed`), pure Wayland `--no-x11 --headless`, `[HWAccel.CUDA] Initialization of CUDA was successful` in `gnome-remote-desktop-daemon`, `GSK_RENDERER=ngl`, `selkies-webtop` healthy, and FreeRDP `:3389` desktop verified via screenshot.
   - `vm-win11`: `GRID P4-4Q` (`539.72`, `Licensed`), `Get-PnpDevice` status `OK`, WinRM NTLM active, and FreeRDP `:3389` NLA login (`zero`) verified via screenshot without error `0x200d`.
   - Both VMs were gracefully shut down afterward cleanly returning the Tesla P4 to idle (`0` QEMU processes).

---

## 7. `libopen-nvidia-vgpu.so` (Phase 1A & Phase 1B Complete) & Tier 2 Guest Driver (`Phase 2A, 2B-1 & 2B-2 Complete`) (`2026-11-23`)

Implemented in [`open-vgpu-plugin/`](../open-vgpu-plugin/) (`open_vgpu_plugin.h`, `gp104_vgpu_core.c`, `open_vgpu_rm.h`, `open_vgpu_rm.c`, `open_vrpc_rm.c`, `open_vrpc_mmu.c`, `open_vgpu_static_info.c`, `open_vgpu_plugin.c`, `vmiop_plugin.map`, `mock_vendor_vgpu.c`, `test_vgpu_plugin_unit.c`, `Makefile`) and cataloged in [`12-OPEN-VGPU-PLUGIN-AND-VRPC-TRACE-CATALOG.md`](./12-OPEN-VGPU-PLUGIN-AND-VRPC-TRACE-CATALOG.md):
- Exports `vmiop_display_vmiop_plugin@@VER_5_0` (`0xa8` bytes) and `vmiop_display_vmiop_env@@VER_2_0` (`0x38` bytes), verified by `make -C open-vgpu-plugin clean all test` (`5/5` suites: `test_standalone_gp104_end_to_end`, `test_hybrid_interposer_and_trace`, `test_rm_backed_vrpc_and_nva082_lifecycle`, `test_mmu_channels_pfn_pin_and_bar1_apertures`, `test_consolidated_static_info_v23_05_binary_parity`).
- **Phase 1B Milestone 1B-1 (`open_vgpu_rm.c`, `open_vrpc_rm.c`)**: Adds `/dev/nvidiactl` + `NVA081`/`NVA082` host RM bridge, guest-to-host `hClient` mapping, and `32` RM-backed session/object/control VRPC handlers (`0x01..0x9c`), verified against mock-RM unit tests and `--live-rm-probe` against real Tesla P4 `nvidia.ko` (`535.309.01`) on `192.168.1.24`.
- **Phase 1B Milestone 1B-2 (`open_vrpc_mmu.c`)**: Completes all remaining `15` memory, MMU, channel, and PTE-blit VRPC opcodes (`0x04`..`0x0f`, `0x1b`, `0x1c`, `0x21`, `0x2b`, `0x34`, `0x38`, `0x3d` — `47/47` live cataloged opcodes implemented), guest PFN pinning (`vmiop_pin_guest_pages_v2` / `NV01_MEMORY_LIST_SYSTEM` `0x81` / `unpin_pages`), ref-counted `vmiop_map_guest_region` BAR1/USERD MMIO aperture forwarding, and faithful `0x06` unlicensed channel-cap enforcement (`0x55` max -> `NV_ERR_NO_MEMORY`).
- **Phase 1B Milestone 1B-3 (`open_vgpu_static_info.c`)**: Upgrades `0x50` (`VGPU_STATIC_INFO`) to exact `0x10758`-byte (`67,416` B, `29` sub-blocks) and `0x9c` (`GR_STATIC_INFO`) to exact `0x7b30`-byte (`31,536` B, `11` sub-blocks) VGX `0x23.0x05` wire formats (including the exact `_nv007054vgpu` `0x940`B->`0x900`B `GPC_MASK` packer and `0xff000001U` bounds semantics), populated live via `18` real host RM controls (`0x20800a22`..`0x20800a48`, `0x2080170d`, `0x2080170e`, `0x20801112`, `0x20801348`, `0x20802a03`, `0x20801328`, `0x20803001`) and verified via `--live-rm-probe` on `192.168.1.24`.
- **Tier 2 Phase 2A, Phase 2B-1 & Phase 2B-2 ([`open-vgpu-guest/`](../open-vgpu-guest/) & [`13-OPEN-GUEST-VGPU-KERNEL-DRIVER.md`](./13-OPEN-GUEST-VGPU-KERNEL-DRIVER.md))**: Implements shared MIT/GPL-2.0 guest VRPC circular ring client (`open_vgpu_guest_rpc.c`), zero-blob host↔guest loopback verification (`test_host_guest_loopback.c`), Linux `7.0` `kbuild`-verified `nova_vgpu_guest.ko`, plus the clean-room `NVIDIA/open-gpu-kernel-modules@535.309.01` GP104 BAR0 VRPC overlay (`overlay/open_vgpu_bar0_rpc.{h,c}`, `scripts/patch_open_535_guest_rm.py`, `scripts/build_open_535_guest_kmod.sh`). Verified building zero-blob open `nv-kernel.o`, `nv-modeset-kernel.o`, `nvidia.ko`, `nvidia-modeset.ko`, `nvidia-drm.ko`, and `nvidia-uvm.ko` with `Dual MIT/GPL` license (`taint: OE`) and `ALL_OPEN_535_GUEST_SYMBOLS_VERIFIED!` on both Ubuntu `7.0.0-43-generic` (`192.168.1.15`) and AlmaLinux 10.3 `6.12.0-264.16.1.el10_3.x86_64_v2` (`vm-alma-2b2`), and live-tested against the real Tesla P4 `GRID P4-4Q` mdev on `192.168.1.24` (`GP_IN_RING` `0x80b` ring setup, `0x01`/`0x40`/`0x50`/`0x9c` handshake, `0x02`/`0x20`/`0x13` RM hierarchy allocation, `0x1a` `0x40` control framing + `0x20801112` 23-dword XDR unpacker, `gpuStateInit` completion into `gpuStateLoad`, and clean `0x0a` `FREE` + `0x2f` `UNLOADING_GUEST_DRIVER` teardown).
- Cold-boot `/dev/nvidiactl` fix verified across a real power cycle on `192.168.1.24`.
- Deployed alongside `open-nvidia-vgpu-mgr` in hybrid interposer mode on `192.168.1.24`: concurrently booted `vm-alma` (`PID 3149`, `535.309.01`, `Licensed`) and `vm-win11` (`PID 3202`, `539.72`, `Licensed`), capturing `34` Linux VRPC opcodes (`2,764` calls) and `39` Windows VRPC opcodes (`7,430` calls) — `47` distinct opcodes total — plus live PCI config (`0x030000a1`, vendor cap `535.309.01r539_72`) and BAR0 `0x8f000` constants fed back into `gp104_vgpu_core.c`.
- **Rank 1 (`open-nvenc/`), Rank 2 (`open-vgpu-guest/` + `kmods/nova_vgpu_guest/`), and Rank 3 (`open-vgpu-guest/overlay/` + `open-host-rm/`) Complete (`2026-11-23`)**:
  - **Rank 1 ([`open-nvenc/`](../open-nvenc/))**: Clean-room `libopen-nvidia-encode.so` (`SONAME libnvidia-encode.so.1`) verified via `rea@6.10.1` (`evidence/vgpu-535-rea610/libnvidia-encode.*.json`, `248,480` B, `.text` `177,986` B) exporting all `10/10` ELF dynamic symbols (`NvEncodeAPICreateInstance`, `NvEncodeAPIGetMaxSupportedVersion` returning `0xc1`, and all `8` `NvTool*` profiler hooks with all `45` API names at `0x33c120`), exact `43`-slot `NV_ENCODE_API_FUNCTION_LIST` (`+0x08`..`+0x158`), Pascal `GP104` H.264/HEVC 8-bit & 10-bit 4:2:0 / 4:4:4 (`YUV444_10BIT`) encoder session/bitstream engine + `/dev/nvidiactl` `NVC0B7_VIDEO_ENCODER` (`0xc0b7`) RM bridge + differential test against `libnvidia-encode.so.535.309.01`.
  - **Rank 2 ([`open-vgpu-guest/`](../open-vgpu-guest/) & [`kmods/nova_vgpu_guest/`](../kmods/nova_vgpu_guest/))**: Adds `open_vgpu_guest_alloc_object()` (`0x09` `ALLOC_OBJECT`) and `open_vgpu_guest_unmap_bar1()` (`0x0d` `UNMAP_MEMORY`) supporting all five Pascal GP104 engine object classes (`NVC0B7_VIDEO_ENCODER = 0xc0b7`, `NVC1C0_PASCAL_COMPUTE_A = 0xc1c0`, `NVC197_PASCAL_A = 0xc197`, `NVC1B5_DMA_COPY = 0xc1b5`, `NVC1B0_VIDEO_DECODER = 0xc1b0`), `use_gp_in_ring` (`0x80b`) module parameter + LO-before-HI ring teardown in `nova_vgpu_guest.ko`, and `--probe-535-engines` in `test_guest_ioctl.c`.
  - **Rank 3A & 3B ([`open-vgpu-guest/overlay/`](../open-vgpu-guest/overlay/) & [`open-host-rm/`](../open-host-rm/))**: Adds pre-GSP Pascal `KernelFifo` (`kfifoRunlistQueryNumChannels_KERNEL`), `Intr` (`intrGetUvmSharedLeafEnDisableMask_TU102`), and `KernelGraphics` (`NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_*` backed by `0x9c` `GR_STATIC_INFO`) guest RM shims, plus the Tier 2B host GP104 physical RM HAL slice (`libopen-host-rm-gp104.so`) implementing Category #1 immutable signed Falcon WPR1/ACR LSF header construction (`8/8` Falcons matching `BLOB_MANIFEST.json`), `NV_PMC_ENABLE` / BAR1 / BAR2 physical bringup, and `22`-type Tesla P4 vGPU / 50-50 `GRID P4-4Q` (`4096` CHIDs -> `2 x 2048`) partitioning.
