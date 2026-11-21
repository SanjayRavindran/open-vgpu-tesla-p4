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
| **`nvidia-vgpu-vfio.ko`** | **Open (DKMS)** | C / MIT (`535.309.01`) | Packaged alongside `nvidia.ko` in `/usr/src/nvidia-vgpu-kvm-535.309.01` for automatic rebuilds across Ubuntu & Proxmox kernels |
| **`nvidia.ko`** (`nv-kernel.o_binary`) | Retained (Stable) | Proprietary kernel core | Handles Pascal Falcon signed blobs (`ACR`, `PMU`, `SEC2`, `FECS`) and RM IOCTL dispatch |
| **`nvidia-vgpu-mgr`** & **`libnvidia-vgpu.so.535.309.01`** | Retained (Stable) | Proprietary `vmiop` plugin | Provides proven, production-stable 50/50 multi-VM scheduler, BAR0/VRPC mediation, and Win11/AlmaLinux 10 runtime stability |
| **`nvidia-smi`** & **`libnvidia-ml.so.535.309.01`** | Retained (Stable) | Proprietary management | Version-locked `535.309.01` telemetry (`API mismatch` prevention) |

---

## 2. `open-nvidia-vgpud` (Rust) — REA 6.10.1 Findings

Static decompilation of `nvidia-vgpud` (`0x401e20`, `0x4020a0`, `0x402ca0`, `0x4039f0`, `0x404440`, `0x40b950`, `0x40c7e0`, `0x40cea0`, `0x40dfc0`) revealed:

1. **Synchronous Oneshot Design**: `main` calls `0x402ca0` once to register profiles in kernel RM and immediately exits `0`; systemd runs `open-nvidia-vgpud` as `Type=oneshot` with `RemainAfterExit=yes`.
2. **Exact `/dev/nvidiactl` & `/dev/nvidiaN` Sequence** (implemented in [`nova-vgpu/src/rm_ioctl.rs`](../nova-vgpu/src/rm_ioctl.rs) and verified live on `535.309.01`):
   - `NV_ESC_CHECK_VERSION_STR` (`0xc04846d2`, `cmd=0`) with the running kernel version parsed from `/proc/driver/nvidia/version` (`535.309.01`)
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

## 5. End-to-End Live Verification on `192.168.1.24` (`2026-11-21`)

With `nvidia-vgpu-kvm/535.309.01` installed under DKMS (`updates/dkms/nvidia.ko.zst`, `updates/dkms/nvidia-vgpu-vfio.ko.zst`) and all three open host binaries active:
1. **`nvidia-vgpud.service`**: Registered `12` Tesla P4 profiles (`RM mode 3`, driver `535.309.01`), creating all `12` sysfs types under `/sys/bus/pci/devices/0000:01:00.0/mdev_supported_types/` including `nvidia-65` (`GRID P4-4Q`, `available_instances = 2`).
2. **`nvidia-xid-logd.service`**: Started cleanly (`Waiting for events...` on `00000000:01:00.0`), exited cleanly on `SIGTERM`, and unlinked its locked PID file.
3. **`vm-alma` (`192.168.1.68`) + `vm-win11` (`192.168.1.65`) Concurrent 50/50 Boot**:
   - Host `nvidia-smi` showed both `vgpu` processes (`4000MiB` each, `8032MiB / 8192MiB`).
   - `vm-alma`: `GRID P4-4Q` (`535.309.01`, `Licensed`), pure Wayland `--no-x11 --headless`, `[HWAccel.CUDA] Initialization of CUDA was successful` in `gnome-remote-desktop-daemon`, `GSK_RENDERER=ngl`, `selkies-webtop` healthy, and FreeRDP `:3389` desktop verified via screenshot.
   - `vm-win11`: `GRID P4-4Q` (`539.72`, `Licensed`), `Get-PnpDevice` status `OK`, WinRM NTLM active, and FreeRDP `:3389` NLA login (`zero`) verified via screenshot without error `0x200d`.
   - Both VMs were gracefully shut down afterward cleanly returning the Tesla P4 to idle (`0` QEMU processes).
