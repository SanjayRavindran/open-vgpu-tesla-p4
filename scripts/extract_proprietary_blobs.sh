#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Clean-room extractor for proprietary NVIDIA binaries required at runtime:
#   1) Host 535.309.01-vgpu-kvm: GP104 signed Falcon blobs + vgpuConfig.xml
#   2) Guest 535.309.01-grid: verified Linux guest installer payload (CUDA 12.1/12.2, NVENC 12.1)
#   3) Guest 620.70.06-grid: Wayland/EGL/GBM external platform libraries
#      (wayland620.tgz) overlaid onto 535.309.01 Pascal Linux guests
#   4) Guest 539.72-grid (Windows): verified WHQL Windows 10/11 guest driver payload (optional, uses 7z)
#
# This repository contains ZERO NVIDIA proprietary binaries. Run this script
# locally with official NVIDIA GRID installers obtained under NVIDIA's license terms.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

HOST_RUN=""
GUEST_535_RUN=""
GUEST_620_RUN=""
WIN_539_EXE=""
OUT_DIR="${REPO_ROOT}/blobs"
WORK_DIR="/tmp/open-vgpu-tesla-p4-extract"

EXPECTED_HOST_535_KERNEL_SHA256="8c753d0898c478c56e22958d35aa271d671b3eb58f45069369b1df19480433f0"
EXPECTED_GUEST_535_RUN_SHA256="a5fa966d2de4953b4e7cb8016064bc23a8f9a0cd23f56e10cd122a785426d5ff"
EXPECTED_GUEST_620_RUN_SHA256="75149eb0de2065329ca6f8da1a2c1c2718854964ea1d6c0a314a73edadb99060"
EXPECTED_WIN_539_EXE_SHA256="3271c17b9584280e9f063d46c2925bffa1bc89c6f8862896f841fb1b97664e95"

usage() {
  cat <<EOF
Usage: extract_proprietary_blobs.sh [OPTIONS]

Options:
  --host-run PATH       Path to NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run
  --guest-535-run PATH  Path to NVIDIA-Linux-x86_64-535.309.01-grid.run
  --guest-620-run PATH  Path to NVIDIA-Linux-x86_64-620.70.06-grid.run (for Wayland EGL/GBM overlay)
  --win-539-exe PATH    Path to 539.72_grid_win10_win11_server2019_server2022_dch_64bit_international.exe
  --out-dir DIR         Output directory for git-ignored payloads (default: ${OUT_DIR})
  --work-dir DIR        Temporary extraction directory (default: ${WORK_DIR})
  -h, --help            Show this help message

Example:
  ./scripts/extract_proprietary_blobs.sh \\
    --host-run      /path/to/NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run \\
    --guest-535-run /path/to/NVIDIA-Linux-x86_64-535.309.01-grid.run \\
    --guest-620-run /path/to/NVIDIA-Linux-x86_64-620.70.06-grid.run \\
    --win-539-exe   /path/to/539.72_grid_win10_win11_server2019_server2022_dch_64bit_international.exe
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host-run)      HOST_RUN="$2"; shift 2 ;;
    --guest-535-run) GUEST_535_RUN="$2"; shift 2 ;;
    --guest-620-run) GUEST_620_RUN="$2"; shift 2 ;;
    --win-539-exe)   WIN_539_EXE="$2"; shift 2 ;;
    --out-dir)       OUT_DIR="$2"; shift 2 ;;
    --work-dir)      WORK_DIR="$2"; shift 2 ;;
    -h|--help)       usage; exit 0 ;;
    *)               echo "Unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

if [[ -z "${HOST_RUN}" && -z "${GUEST_535_RUN}" && -z "${GUEST_620_RUN}" && -z "${WIN_539_EXE}" ]]; then
  echo "ERROR: Specify at least one of --host-run, --guest-535-run, --guest-620-run, or --win-539-exe." >&2
  usage
  exit 2
fi

mkdir -p "${OUT_DIR}" "${WORK_DIR}"

if [[ -n "${HOST_RUN}" ]]; then
  [[ -f "${HOST_RUN}" ]] || { echo "ERROR: Missing --host-run: ${HOST_RUN}" >&2; exit 1; }
  echo "[1/4] Extracting host 535.309.01-vgpu-kvm ..."
  HOST_TMP="${WORK_DIR}/host-535"
  rm -rf "${HOST_TMP}"
  sh "${HOST_RUN}" --extract-only --target "${HOST_TMP}" >/dev/null

  NV_KOBJ="${HOST_TMP}/kernel/nvidia/nv-kernel.o_binary"
  [[ -f "${NV_KOBJ}" ]] || { echo "ERROR: Missing ${NV_KOBJ}" >&2; exit 1; }
  GOT_SHA="$(sha256sum "${NV_KOBJ}" | awk '{print $1}')"
  if [[ "${GOT_SHA}" != "${EXPECTED_HOST_535_KERNEL_SHA256}" ]]; then
    echo "ERROR: Host nv-kernel.o_binary SHA-256 mismatch: ${GOT_SHA}" >&2
    exit 1
  fi

  mkdir -p "${OUT_DIR}/host-535"
  cp -f "${HOST_TMP}/vgpuConfig.xml" "${OUT_DIR}/host-535/" 2>/dev/null || true
  cp -f "${NV_KOBJ}" "${OUT_DIR}/host-535/"

  echo "  -> Extracting & verifying GP104 Falcon blobs into firmware-blobs/gp104-vgpu-535/ ..."
  python3 "${REPO_ROOT}/tools/extract_vgpu_535_blobs.py" "${NV_KOBJ}" \
    --manifest "${REPO_ROOT}/firmware-blobs/gp104-vgpu-535/BLOB_MANIFEST.json" \
    --out-dir "${REPO_ROOT}/firmware-blobs/gp104-vgpu-535"
fi

if [[ -n "${GUEST_535_RUN}" ]]; then
  [[ -f "${GUEST_535_RUN}" ]] || { echo "ERROR: Missing --guest-535-run: ${GUEST_535_RUN}" >&2; exit 1; }
  echo "[2/4] Verifying & extracting Linux guest 535.309.01-grid ..."
  GOT_G535_SHA="$(sha256sum "${GUEST_535_RUN}" | awk '{print $1}')"
  if [[ "${GOT_G535_SHA}" != "${EXPECTED_GUEST_535_RUN_SHA256}" ]]; then
    echo "ERROR: Guest 535.309.01-grid.run SHA-256 mismatch: ${GOT_G535_SHA}" >&2
    exit 1
  fi
  GUEST_535_OUT="${OUT_DIR}/guest-535"
  rm -rf "${GUEST_535_OUT}"
  sh "${GUEST_535_RUN}" --extract-only --target "${GUEST_535_OUT}" >/dev/null
  echo "  -> Extracted guest 535.309.01-grid tree into ${GUEST_535_OUT}"
fi

if [[ -n "${GUEST_620_RUN}" ]]; then
  [[ -f "${GUEST_620_RUN}" ]] || { echo "ERROR: Missing --guest-620-run: ${GUEST_620_RUN}" >&2; exit 1; }
  echo "[3/4] Extracting R620 (620.70.06) Wayland EGL/GBM external platform overlay ..."
  GOT_G620_SHA="$(sha256sum "${GUEST_620_RUN}" | awk '{print $1}')"
  if [[ "${GOT_G620_SHA}" != "${EXPECTED_GUEST_620_RUN_SHA256}" ]]; then
    echo "ERROR: Guest 620.70.06-grid.run SHA-256 mismatch: ${GOT_G620_SHA}" >&2
    exit 1
  fi
  G620_TMP="${WORK_DIR}/guest-620"
  rm -rf "${G620_TMP}"
  sh "${GUEST_620_RUN}" --extract-only --target "${G620_TMP}" >/dev/null

  WL_OUT="${OUT_DIR}/wayland620"
  rm -rf "${WL_OUT}"
  mkdir -p "${WL_OUT}"
  for f in \
    "libnvidia-egl-wayland2.so.1.0.2" \
    "libnvidia-egl-wayland.so.1.1.20" \
    "libnvidia-egl-gbm.so.1.1.3" \
    "libnvidia-egl-xcb.so.1.0.6" \
    "libnvidia-egl-xlib.so.1.0.6" \
    "09_nvidia_wayland2.json" \
    "10_nvidia_wayland.json" \
    "15_nvidia_gbm.json" \
    "20_nvidia_xcb.json" \
    "20_nvidia_xlib.json"; do
    [[ -f "${G620_TMP}/${f}" ]] || { echo "ERROR: Missing R620 Wayland asset: ${f}" >&2; exit 1; }
    cp -f "${G620_TMP}/${f}" "${WL_OUT}/"
  done
  tar -C "${WL_OUT}" -czf "${OUT_DIR}/wayland620.tgz" .
  echo "  -> Packaged ${OUT_DIR}/wayland620.tgz (overlay onto 535.309.01 Linux guests)"
fi

if [[ -n "${WIN_539_EXE}" ]]; then
  [[ -f "${WIN_539_EXE}" ]] || { echo "ERROR: Missing --win-539-exe: ${WIN_539_EXE}" >&2; exit 1; }
  command -v 7z >/dev/null 2>&1 || { echo "ERROR: 7z (p7zip-full) is required for --win-539-exe" >&2; exit 1; }
  echo "[4/4] Verifying & extracting Windows guest WHQL 539.72-grid ..."
  GOT_W539_SHA="$(sha256sum "${WIN_539_EXE}" | awk '{print $1}')"
  if [[ "${GOT_W539_SHA}" != "${EXPECTED_WIN_539_EXE_SHA256}" ]]; then
    echo "ERROR: Windows 539.72 .exe SHA-256 mismatch: ${GOT_W539_SHA}" >&2
    exit 1
  fi
  WIN_539_OUT="${OUT_DIR}/win-539"
  rm -rf "${WIN_539_OUT}"
  mkdir -p "${WIN_539_OUT}"
  7z x -y -o"${WIN_539_OUT}" "${WIN_539_EXE}" >/dev/null
  echo "  -> Extracted Windows 539.72 Display.Driver (nvgridsw.inf) into ${WIN_539_OUT}"
fi

echo "Done. Proprietary outputs stored strictly outside git tracking."
