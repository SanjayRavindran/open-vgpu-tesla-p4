---
name: open-vgpu-daemons-rea-dev
description: >-
  Reverse-engineer, build, differential-test, and deploy clean-room open-source
  replacements for the closed 535.309.01-vgpu-kvm host userspace binaries
  (open-nvidia-vgpud in Rust, libopen-vgxcfg.so in C, open-nvidia-xid-logd in C)
  using rea@6.10.1 on 192.168.1.15. Use whenever regenerating REA evidence,
  modifying open vGPU host daemons, or running differential/mock-NVML tests.
---

# Open-Source vGPU Host Daemons & `rea@6.10.1` Workflow (`192.168.1.15`)

## Component Replacement Matrix
- **Open / Replaced**:
  1. `open-nvidia-vgpud` (`nova-vgpu/src/rm_ioctl.rs`, `nova-vgpu/src/bin/open_nvidia_vgpud.rs`) -> parses `vgpuConfig.xml`, expands GRID license tokens (`QVDWS` -> `Quadro-Virtual-DWS,5.0`), checks `NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE (0x800289)` (`!= 1, 2`), and pushes all 12 Tesla P4 profiles via `0xa0810101` + `0xa0810109`.
  2. `libopen-vgxcfg.so` (`open-vgxcfg/open_vgxcfg.c`) -> zero-dependency XML DOM + all 9 ABI-exact `vgxConfig*` exports (`0xe4` USM struct, `0x210` global settings, `0x18` physGPU/supportedUsm, `0x10` display).
  3. `open-nvidia-xid-logd` (`open-xid-logd/open_xid_logd.c`) -> daemonize/foreground + PID locking + `dlopen("libnvidia-ml.so.1")`, clean non-GSP Pascal exit `0`, Base64 76-char XID debugdump syslog reporter on GSP GPUs.
  4. `nvidia-vgpu-vfio.ko` -> MIT-licensed open KVM/VFIO mdev kernel module packaged via DKMS (`nvidia-vgpu-kvm/535.309.01`).
- **Retained Stable Components**: `nvidia.ko` (`nv-kernel.o_binary`), `nvidia-vgpu-mgr`, `libnvidia-vgpu.so.535.309.01`, `nvidia-smi` / `libnvidia-ml.so.535.309.01` for live VM vGPU data-plane reliability.

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
# Rust open-nvidia-vgpud + RM ioctl unit tests
(cd nova-vgpu && cargo test && cargo build --release --bin open-nvidia-vgpud)
./nova-vgpu/target/release/open-nvidia-vgpud --dry-run --config /path/to/vgpuConfig.xml

# C libopen-vgxcfg.so unit + byte-exact differential test against NVIDIA .so
make -C open-vgxcfg clean all test
make -C open-vgxcfg diff-test ORIG_VGXCFG_SO=/path/to/libnvidia-vgxcfg.so.535.309.01

# C open-nvidia-xid-logd mock-NVML + 76-char Base64 XID dump tests
make -C open-xid-logd clean all test

# Clean build binaries before git commit (CI rejects tracked .so/ELF files)
make -C open-vgxcfg clean && make -C open-xid-logd clean
```
