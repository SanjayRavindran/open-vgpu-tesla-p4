#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Configures NVIDIA Container Toolkit (CDI mode) and Docker inside an R580
# (580.196.07-grid) pure Wayland Linux vGPU guest (such as AlmaLinux 10.3)
# for hardware-accelerated Selkies + LinuxServer.io PixelFlux with 10-bit and
# 4:4:4 (YUV444) color depth on GRID P4-4Q without requiring /usr/bin/nvidia-xconfig.
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "ERROR: Run setup_selkies_pixelflux_vgpu.sh as root (sudo)." >&2
  exit 1
fi

# 1. Refresh linker cache after any driver/X11-strip changes
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

# 3. Switch NVIDIA Container Runtime to CDI mode (avoids legacy /usr/bin/nvidia-xconfig lookups),
#    generate both /etc/cdi/nvidia.yaml and /var/run/cdi/nvidia.yaml, and restart Docker.
systemctl enable --now containerd docker
nvidia-ctk config --in-place --set nvidia-container-runtime.mode=cdi
nvidia-ctk runtime configure --runtime=docker
mkdir -p /etc/cdi /var/run/cdi
nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml
cp -f /etc/cdi/nvidia.yaml /var/run/cdi/nvidia.yaml
systemctl restart docker

echo "NVIDIA Container Toolkit CDI configured (mode=cdi) for R580 580.196.07 pure Wayland guest."
echo "Launch or recreate Selkies + PixelFlux (10-bit & 4:4:4 NVENC) with: docker compose up -d --force-recreate"
