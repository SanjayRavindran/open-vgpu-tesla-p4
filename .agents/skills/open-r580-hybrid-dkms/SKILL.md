---
name: open-r580-hybrid-dkms
description: >-
  Build, verify, install, or uninstall the bare-metal open-580-driver hybrid
  DKMS stack (580.178.04 open-gpu-kernel-modules C shim + Pascal GP104
  nv-kernel.o_binary) on Linux 7.0+ kernels and run the CUDA, NVENC, and
  NVDEC smoke tests. Use this skill when working with open-r580-driver,
  dkms-compat, or bare-metal Tesla P4 driver builds.
---

# Open-Shim R580 Hybrid DKMS Driver Runbook (`open-r580-driver/`)

## Workflow

### 1. Extract `580.178.04` Payloads
```bash
bash open-r580-driver/extract_r580_binaries.sh --download --output /tmp/r580_pkg
```

### 2. Dry-Run Kbuild Check on Current or Upgraded Kernel (`Linux 7.x`)
Before installing or rebooting into a new kernel:
```bash
bash open-r580-driver/open_r580_dkms_builder.sh --driver-dir /tmp/r580_pkg --dry-run
bash dkms-compat/verify_r580_kbuild.sh
```

### 3. Full DKMS + Non-Clobbering Userspace Install (Including Wayland EGL/GBM & CDI)
```bash
sudo bash open-r580-driver/open_r580_dkms_builder.sh \
  --driver-dir /tmp/r580_pkg \
  --install --with-userspace
sudo update-initramfs -u
sudo modprobe nvidia nvidia-modeset nvidia-drm nvidia-uvm
```

### 4. Run Automated Unit Tests & C Smoke Tests
```bash
python3 -m unittest discover -s tests -v
gcc -O2 open-r580-driver/p4_cuda_nvenc_smoke.c -ldl -o /tmp/p4_cuda_nvenc_smoke
gcc -O2 open-r580-driver/p4_nvdec_smoke.c -ldl -o /tmp/p4_nvdec_smoke
/tmp/p4_cuda_nvenc_smoke
/tmp/p4_nvdec_smoke
```
