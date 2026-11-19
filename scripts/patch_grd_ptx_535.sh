#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Downgrades GNOME Remote Desktop's CUDA AVC helper PTX header (.version 8.[3-9])
# to .version 8.0 so NVIDIA R535 (CUDA 12.1/12.2, max PTX 8.2) loads the kernel
# cleanly via cuModuleLoadData for hardware-accelerated H.264 AVC444 RDP.
set -euo pipefail

PTX_FILE="${1:-/usr/share/gnome-remote-desktop/grd-cuda-avc-utils_30.ptx}"

if [[ ! -f "${PTX_FILE}" ]]; then
  echo "ERROR: PTX file not found: ${PTX_FILE}" >&2
  exit 1
fi

if grep -Eq '^\.version[[:space:]]+8\.[3-9]' "${PTX_FILE}"; then
  cp -n "${PTX_FILE}" "${PTX_FILE}.orig" 2>/dev/null || true
  sed -i -E 's/^(\.version[[:space:]]+)8\.[3-9]/\18.0/' "${PTX_FILE}"
  echo "Patched ${PTX_FILE} -> $(grep -m1 '^\.version' "${PTX_FILE}")"
else
  echo "No patch needed for ${PTX_FILE} ($(grep -m1 '^\.version' "${PTX_FILE}" || echo 'no .version header'))"
fi
