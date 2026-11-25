---
name: open-vgpu-daemons-rea-dev
description: >-
  Reverse-engineer, build, differential-test, and deploy clean-room open-source
  replacements for the 580.196.09-vgpu-kvm host userspace binaries
  (open-nvidia-vgpud in Rust, libopen-vgxcfg.so in C, open-nvidia-xid-logd in C,
  open-nvidia-vgpu-mgr in C) using rea@6.10.1 on 192.168.1.15. Use whenever
  regenerating REA evidence, modifying open vGPU host daemons, or running
  differential/mock-NVML/mock-VFIO tests.
---

# Open-Source vGPU Host & Guest Components (`rea@6.10.1` Workflow, `192.168.1.15`)

## Component Replacement Matrix
- **Open / Replaced**:
  1. `open-nvidia-vgpud` (`nova-vgpu/src/rm_ioctl.rs`, `nova-vgpu/src/config.rs`, `nova-vgpu/src/bin/open_nvidia_vgpud.rs`) -> native R580 registration daemon (`0x180`-byte `GET_PROBED_IDS`, `0x8`-byte `GET_VIRTUALIZATION_MODE`, `0x1540`-byte `NVA081_CTRL_VGPU_CONFIG_SET_INFO_PARAMS` with 64-byte `vgpuName`/`vgpuClass`), ensures `/dev/nvidiactl` (`195:255`) & `/dev/nvidia0` (`195:0`) exist on cold boot, expands GRID license tokens (`QVDWS` -> `Quadro-Virtual-DWS,5.0`), has no Maxwell/Pascal denylist, and registers all 12 Tesla P4 profiles via `0xa0810101` + `0xa0810109`.
  2. `libopen-vgxcfg.so` (`open-vgxcfg/open_vgxcfg.c`) -> zero-dependency XML DOM + all 9 ABI-exact `vgxConfig*` exports (`0xe4` USM struct, `0x210` global settings, `0x18` physGPU/supportedUsm, `0x10` display).
  3. `open-nvidia-xid-logd` (`open-xid-logd/open_xid_logd.c`) -> daemonize/foreground + PID locking + `dlopen("libnvidia-ml.so.1")`, clean non-GSP Pascal exit `0`, Base64 76-char XID debugdump syslog reporter on GSP GPUs.
  4. `open-nvidia-vgpu-mgr` (`open-vgpu-mgr/open_vgpu_mgr.c`, `vmiop_env.c`, `rm_client.c`, `nv_vgpu_vfio_abi.h`) -> native R580 (`580.196.09`) KVM/VFIO `/dev/nvidia-vgpuctl` & `/dev/nvidia-vgpu%d` daemon (`NV_MAX_VGPU_TYPE_PER_GPU = 0x80`, `NV_VGPU_STRING_BUFFER_SIZE = 64`, `SET_VM_NAME = 0x98` B with `vmIdType = 1U`, `GET_SUPPORTED_VGPU_TYPES = 0x20c` B, `GET_VGPU_TYPE_INFO = 0x1538` B, `CREATE_DEVICE = 0x28` B, `GET_INSTANCES = 0x40c` B, `BAR_INFO_SET_DATA = 0x80` B with `isBar064bit`, `UPDATE_SYSFS_INFO` event, `BUILD_VERSION_V2 = 0x408` B), plus all `60` dynamic `vmiop*` exports + `vmiop_presentation`.
  5. `open-nvidia-smi` & `libopen-nvidia-ml.so` (`open-nvml-smi/open_nvml.c`, `open_nvidia_smi.c`) -> clean-room NVML (`libnvidia-ml.so.1`, `12.580.196.09`) and `nvidia-smi` CLI updated for R580 RM controls (`0x8`-byte `0x00800289`, `0x20c`-byte `0xa0810104`/`0xa0810105`, `0x1538`-byte `0xa0810103`).
  6. `libopen-nvidia-vgpu.so` (`open-vgpu-plugin/`) -> `vmiop_display_vmiop_plugin@@VER_5_0` (`0xa8`B) + `vmiop_display_vmiop_env@@VER_2_0` (`0x38`B) standalone GP104 core + hybrid interposer (`OPEN_VGPU_PLUGIN_LIB`), defaulting `vlib` to `/opt/nvidia-vgpu-580/lib/libnvidia-vgpu.so.580.196.09`.
  7. `open-vgpu-guest` & `nova_vgpu_guest.ko` (`open-vgpu-guest/`, `kmods/nova_vgpu_guest/`) -> clean-room guest BAR0 `0x8f000` VRPC client/loopback suite + open guest kernel module work.
  8. `libopen-nvidia-encode.so` (`open-nvenc/`) -> standalone **NVENC `13.0` (`0xd0`)** clean-room `libnvidia-encode.so.1` replacement linking solely against `libc.so.6`, driving GP104 `NVC2B7` (`0xc2b7`) + `NVC06F` + `NVC0B5` H.264/HEVC 8-bit & 10-bit 4:2:0 / 4:4:4.
  9. `libopen-host-rm-gp104.so` (`open-host-rm/`) -> host GP104 physical RM HAL slice + LSF WPR1/ACR signature verification.
  10. `nvidia-vgpu-vfio.ko` -> MIT-licensed open KVM/VFIO mdev kernel module packaged via DKMS (`nvidia-vgpu-kvm/580.196.09`).
  11. `open-gridd` (`open-gridd/`) -> clean-room C11 + OpenSSL 3.x Linux NLS/DLS licensing daemon (`/usr/bin/nvidia-gridd`, `580.196.07`-compatible `0x00800289` 8-byte fallback) against FastAPI-DLS (`:443`).
- **Retained Host Components**: `nvidia.ko` (`nv-kernel.o_binary` with `0xc85b7` `_nv060499rm` RSA check bypass for Tesla P4 `vgpuConfig.xml`) and `libnvidia-vgpu.so.580.196.09`.

## 1. Running `rea@6.10.1` Static Analysis & Decompilation (`192.168.1.15`)
Always export `GHIDRA_INSTALL_DIR` and pass a fresh absent `--output` dir for `decompile-all`:
```bash
export GHIDRA_INSTALL_DIR=/opt/ghidra_12.2_PUBLIC
rea --version   # 6.10.1
rea analyze <bin> --format json
rea audit-release-exposure <bin> --format json
rea inspect-binary-layout <bin> --format json
rea inspect-native-exports <bin> --format json
rea decompile-all <bin> --output /tmp/decomp_out --format json
```

## 2. Build & Test Commands (`192.168.1.15`)
```bash
# Rust open-nvidia-vgpud + RM ioctl unit tests (native R580)
(cd nova-vgpu && cargo test && cargo build --release --bin open-nvidia-vgpud)
./nova-vgpu/target/release/open-nvidia-vgpud --dry-run --config /path/to/vgpuConfig.xml

# C libopen-vgxcfg.so unit + differential test
make -C open-vgxcfg clean all test
make -C open-vgxcfg diff-test ORIG_VGXCFG_SO=/path/to/libnvidia-vgxcfg.so.580.196.09

# C open-nvidia-xid-logd mock-NVML + Base64 XID dump tests
make -C open-xid-logd clean all test

# C open-nvidia-vgpu-mgr native R580 vmiop-env ABI + mock-RM/VFIO unit tests
make -C open-vgpu-mgr clean all test

# C open-nvidia-smi + libopen-nvidia-ml.so native R580 mock-RM unit tests
make -C open-nvml-smi clean all test

# C libopen-nvidia-vgpu.so plugin unit tests
make -C open-vgpu-plugin clean all test

# C Guest <-> Host VRPC loopback test
make -C open-vgpu-guest clean all test

# C open-nvenc (NVENC 13.0) unit + ABI differential test; add hw-test diff-hw-test on GPU/vGPU guest
make -C open-nvenc clean all test diff-test

# C open-host-rm GP104 physical RM HAL slice unit test
make -C open-host-rm clean all test

# C11 open-gridd NLS/DLS build + C/E2E/static-audit tests
make -C open-gridd clean all test

# Linux 7.0 kbuild check for clean-room guest VRPC kernel module
make -C kmods/nova_vgpu_guest KVER=7.0.0-43-generic clean all

# Clean build binaries before git commit (CI rejects tracked .so/ELF/.ko files)
make -C open-vgxcfg clean && make -C open-xid-logd clean && make -C open-vgpu-mgr clean && make -C open-nvml-smi clean && make -C open-vgpu-plugin clean && make -C open-vgpu-guest clean && make -C open-nvenc clean && make -C open-host-rm clean && make -C open-gridd clean && make -C kmods/nova_vgpu_guest KVER=7.0.0-43-generic clean
```
