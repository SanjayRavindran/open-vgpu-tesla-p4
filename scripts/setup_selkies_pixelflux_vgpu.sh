#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Configures NVIDIA Container Toolkit (CDI v0.7.0) and Docker inside a
# 535.309.01-grid + R620 Wayland/GBM Linux vGPU guest (such as AlmaLinux 10.3)
# for hardware-accelerated Selkies + LinuxServer.io PixelFlux with 10-bit and
# 4:4:4 (YUV444) color depth on GRID P4-4Q.
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "ERROR: Run setup_selkies_pixelflux_vgpu.sh as root (sudo)." >&2
  exit 1
fi

# 1. Remove superseded 535.309.01 EGL Wayland/GBM libraries if R620 overlay is present
for libdir in /usr/lib64 /usr/lib/x86_64-linux-gnu; do
  if [[ -f "${libdir}/libnvidia-egl-gbm.so.1.1.3" ]]; then
    rm -f "${libdir}/libnvidia-egl-gbm.so.1.1.0"
  fi
  if [[ -f "${libdir}/libnvidia-egl-wayland.so.1.1.20" ]]; then
    rm -f "${libdir}/libnvidia-egl-wayland.so.1.1.11"
  fi
done
ldconfig

# 2. On x86-64-v2 CPUs (e.g. Ivy Bridge Xeon E3-1275 V2) running el10 packages
#    (AlmaLinux 10, RHEL 10, Rocky 10, CentOS Stream 10), strip the .note.gnu.property
#    x86-64-v3 ELF marker from Go/C container binaries.
if grep -qE "(AlmaLinux|Red Hat|Rocky|Oracle|CentOS).*10" /etc/os-release 2>/dev/null; then
  for b in /usr/bin/docker* /usr/bin/containerd* /usr/bin/runc /usr/bin/ctr \
           /usr/bin/nvidia-ctk /usr/bin/nvidia-container* /usr/bin/nvidia-cdi-hook \
           /usr/lib64/libnvidia-container*.so* /usr/libexec/docker/cli-plugins/*; do
    [[ -f "${b}" && ! -L "${b}" ]] && objcopy --remove-section=.note.gnu.property "${b}" 2>/dev/null || true
  done
fi

# 3. Configure Docker NVIDIA runtime and generate both /etc/cdi and /var/run/cdi specs
systemctl enable --now containerd docker
nvidia-ctk runtime configure --runtime=docker
mkdir -p /etc/cdi /var/run/cdi
nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml
cp -f /etc/cdi/nvidia.yaml /var/run/cdi/nvidia.yaml
systemctl restart docker

echo "NVIDIA Container Toolkit CDI configured for 535.309.01 + R620 Wayland/GBM."
echo "Launch Selkies + PixelFlux (10-bit & 4:4:4 NVENC) with: docker compose up -d"
