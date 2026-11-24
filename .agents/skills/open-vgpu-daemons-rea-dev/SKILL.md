---
name: open-vgpu-daemons-rea-dev
description: >-
  Reverse-engineer, build, differential-test, and deploy clean-room open-source
  replacements for the closed 535.309.01-vgpu-kvm host userspace binaries
  (open-nvidia-vgpud in Rust, libopen-vgxcfg.so in C, open-nvidia-xid-logd in C,
  open-nvidia-vgpu-mgr in C) using rea@6.10.1 on 192.168.1.15. Use whenever
  regenerating REA evidence, modifying open vGPU host daemons, or running
  differential/mock-NVML/mock-VFIO tests.
---

# Open-Source vGPU Host & Guest Components (`rea@6.10.1` Workflow, `192.168.1.15`)

## Component Replacement Matrix
- **Open / Replaced**:
  1. `open-nvidia-vgpud` (`nova-vgpu/src/rm_ioctl.rs`, `nova-vgpu/src/bin/open_nvidia_vgpud.rs`) -> parses `vgpuConfig.xml`, ensures `/dev/nvidiactl` (`195:255`) & `/dev/nvidia0` (`195:0`) exist on cold boot (`ensure_nvidia_char_node()`), expands GRID license tokens (`QVDWS` -> `Quadro-Virtual-DWS,5.0`), checks `NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE (0x800289)` (`!= 1, 2`), and pushes all 12 Tesla P4 profiles via `0xa0810101` + `0xa0810109`.
  2. `libopen-vgxcfg.so` (`open-vgxcfg/open_vgxcfg.c`) -> zero-dependency XML DOM + all 9 ABI-exact `vgxConfig*` exports (`0xe4` USM struct, `0x210` global settings, `0x18` physGPU/supportedUsm, `0x10` display).
  3. `open-nvidia-xid-logd` (`open-xid-logd/open_xid_logd.c`) -> daemonize/foreground + PID locking + `dlopen("libnvidia-ml.so.1")`, clean non-GSP Pascal exit `0`, Base64 76-char XID debugdump syslog reporter on GSP GPUs.
  4. `open-nvidia-vgpu-mgr` (`open-vgpu-mgr/open_vgpu_mgr.c`, `vmiop_env.c`, `rm_client.c`) -> KVM/VFIO `/dev/nvidia-vgpuctl` & `/dev/nvidia-vgpu%d` master/worker runtime daemon, `/dev/nvidiactl` RM client (`NV_ESC_IOCTL_XFER_CMD`, `NV01_MEMORY_LIST_SYSTEM`), zero-overhead `OPEN_VGPU_PLUGIN_LIB` + `OPEN_VGPU_TRACE_DIR` (`msync` + `memcpy`, **never `/proc/self/mem`** on vGPU mmap pages), and all `60` dynamic `vmiop*` exports + `vmiop_presentation`.
  5. `open-nvidia-smi` & `libopen-nvidia-ml.so` (`open-nvml-smi/open_nvml.c`, `open_nvidia_smi.c`) -> clean-room NVML (`libnvidia-ml.so.1`) and `nvidia-smi` CLI querying `/dev/nvidiactl` & `/dev/nvidiaN` RM controls (`0x2080*`, `0xa081*`) for GP104 telemetry and vGPU instances.
  6. `libopen-nvidia-vgpu.so` (`open-vgpu-plugin/open_vgpu_plugin.c`, `gp104_vgpu_core.c`, `open_vgpu_rm.c`, `open_vrpc_rm.c`, `open_vrpc_mmu.c`, `open_vgpu_static_info.c`) -> `vmiop_display_vmiop_plugin@@VER_5_0` (`0xa8`B) + `vmiop_display_vmiop_env@@VER_2_0` (`0x38`B) standalone GP104 core, host RM `NVA081`/`NVA082` bridge, all `47/47` cataloged live VRPC opcodes (with PFN pinning & BAR1/USERD MMIO aperture forwarding), exact VGX `0x23.0x05` `0x50` (`0x10758`B) & `0x9c` (`0x7b30`B) binary static-info serializers, plus hybrid trace interposer alongside `libnvidia-vgpu.so.535.309.01`.
  7. `open-vgpu-guest` (`open-vgpu-guest/open_vgpu_guest_rpc.{h,c}`, `test_host_guest_loopback.c`, `overlay/open_vgpu_bar0_rpc.{h,c}`, `scripts/prepare_open_535_guest_rm.sh`, `scripts/patch_open_535_guest_rm.py`, `scripts/build_open_535_guest_kmod.sh`) + `nova_vgpu_guest.ko` (`kmods/nova_vgpu_guest/`) -> clean-room guest BAR0 `0x8f000` VRPC client/loopback suite (`59` circular VRPCs including `0x09` `ALLOC_OBJECT` for `NVC0B7_VIDEO_ENCODER`/`NVC1C0_COMPUTE`/`NVC197_3D`/`NVC1B5_DMA_COPY`/`NVC1B0_VIDEO_DECODER` and `0x0d` `UNMAP_MEMORY`) + full zero-blob `NVIDIA/open-gpu-kernel-modules@535.309.01` `nv-kernel.o` & `nvidia*.ko` (`Dual MIT/GPL`, `taint: OE`, `registerHalModule_GP104`, `openVgpu*`, `kernel_fifo.c`/`intr_tu102.c`/`KGR_GET_*` pre-GSP shims) builder verified on Ubuntu `7.0.0-43-generic` and live-tested on AlmaLinux 10.3 `6.12.0-264.16.1.el10_3.x86_64_v2`.
  8. `libopen-nvidia-encode.so` (`open-nvenc/open_nvenc.{h,c}`, `open_nvenc_hw.c`, `test_nvenc_unit.c`) -> standalone **NVENC `13.0` (`0xd0`)** clean-room `libnvidia-encode.so.1` replacement linking solely against `libc.so.6` (with optional `dlopen("libcuda.so.1")`), exporting all `10/10` ELF dynamic symbols (`NvEncodeAPICreateInstance`, `NvEncodeAPIGetMaxSupportedVersion` = `0xd0`, `NvTool*` with all `45/45` API names) and all `43/43` `NV_ENCODE_API_FUNCTION_LIST` slots (`+0x08..+0x158`), driving GP104 `NVC2B7` (`0xc2b7`) + `NVC06F` + `NVC0B5` directly with a cached 64B staging buffer and Pascal 16Bx2 BL16 swizzle for H.264/HEVC 8-bit & 10-bit 4:2:0 / 4:4:4 (`YUV444_10BIT`).
  9. `libopen-host-rm-gp104.so` (`open-host-rm/open_host_rm_gp104.{h,c}`, `test_open_host_rm_unit.c`) -> host GP104 physical RM HAL slice building Category #1 immutable signed Falcon WPR1/ACR LSF descriptors (`8/8` Falcons matching `BLOB_MANIFEST.json`), `NV_PMC_ENABLE`/BAR1/BAR2 physical bringup, and `22`-type Tesla P4 vGPU / 50-50 `GRID P4-4Q` partitioning.
  10. `nvidia-vgpu-vfio.ko` -> MIT-licensed open KVM/VFIO mdev kernel module packaged via DKMS (`nvidia-vgpu-kvm/535.309.01`).
  11. `open-gridd` (`open-gridd/`) -> clean-room C11 + OpenSSL 3.x Linux NLS/DLS licensing daemon (`/usr/bin/nvidia-gridd`), replacing vendor `nvidia-gridd` with RSA-2048 `.tok` JWT verification, PKCE auth against FastAPI-DLS (`:443`), AES-256-GCM trusted store, and RM `0x20808161`/`0x20800187` status reporting.
- **Retained Stable Components**: `nvidia.ko` (`nv-kernel.o_binary`) and `libnvidia-vgpu.so.535.309.01` (used behind `libopen-nvidia-vgpu.so` in hybrid mode for live 2-VM VRPC/DRM parity).

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

# C open-nvidia-vgpu-mgr vmiop-env ABI + mock-RM/VFIO unit tests
make -C open-vgpu-mgr clean all test

# C open-nvidia-smi + libopen-nvidia-ml.so mock-RM unit tests
make -C open-nvml-smi clean all test

# C libopen-nvidia-vgpu.so standalone GP104 + 47/47 VRPCs + 0x50/0x9c binary parity + hybrid trace-interposer (5 suites)
make -C open-vgpu-plugin clean all test

# C Guest <-> Host zero-blob VRPC loopback test
make -C open-vgpu-guest clean all test

# C open-nvenc (NVENC 13.0) unit + ABI differential test; add hw-test diff-hw-test on GPU/vGPU guest
make -C open-nvenc clean all test diff-test

# C open-host-rm GP104 physical RM HAL slice unit test
make -C open-host-rm clean all test

# C11 open-gridd NLS/DLS build + unit/static-audit tests
cmake -S open-gridd -B open-gridd/build && cmake --build open-gridd/build
cd open-gridd && python3 -B -m unittest discover -s tests -v

# Linux 7.0 kbuild check for clean-room guest VRPC kernel module
make -C kmods/nova_vgpu_guest KVER=7.0.0-43-generic clean all

# Patch & build full open-source 535.309.01 guest nvidia*.ko (zero .o_binary blobs)
bash open-vgpu-guest/scripts/build_open_535_guest_kmod.sh --workdir /tmp/open-535-guest-build --kver 7.0.0-43-generic
rm -rf /tmp/open-535-guest-build

# Clean build binaries before git commit (CI rejects tracked .so/ELF/.ko files)
make -C open-vgxcfg clean && make -C open-xid-logd clean && make -C open-vgpu-mgr clean && make -C open-nvml-smi clean && make -C open-vgpu-plugin clean && make -C open-vgpu-guest clean && make -C open-nvenc clean && make -C open-host-rm clean && make -C kmods/nova_vgpu_guest KVER=7.0.0-43-generic clean
```
