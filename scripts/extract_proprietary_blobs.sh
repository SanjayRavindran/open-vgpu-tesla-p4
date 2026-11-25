#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Clean-room extractor for proprietary NVIDIA R580 binaries supplied locally by the user:
#   1) Host 580.196.09-vgpu-kvm: host kernel archive & libnvidia-vgpu.so + vgpuConfig.xml
#   2) Guest 580.196.07-grid: Linux guest tree with optional nv-pci.c Tesla P4 patch prep
#   3) Guest 582.99-grid (Windows): extracts Display.Driver/nvxdapix.dll (uses 7z) for gridd-unlock-patcher
#   4) Optional legacy 535.309.01-vgpu-kvm: extracts BLOB_MANIFEST.json Falcon binaries into firmware-blobs/gp104-vgpu-535/
#
# This repository contains ZERO NVIDIA proprietary binaries. Run this script
# locally with official NVIDIA GRID installers obtained under NVIDIA's license terms.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

HOST_580_RUN=""
GUEST_580_RUN=""
WIN_582_EXE=""
HOST_535_RUN=""
OUT_DIR="${REPO_ROOT}/blobs"
WORK_DIR="/tmp/open-vgpu-tesla-p4-extract"

EXPECTED_GUEST_580_RUN_SHA256="f4dfeba077f48210a61a2eeec76912a3781419d92d542bbb71d81c15227865ad"
EXPECTED_WIN_582_EXE_SHA256="ab38471bf001ee519499ec1e81a7ad4391e78477f477ccfd5688d5bba0afd168"
EXPECTED_HOST_535_KERNEL_SHA256="8c753d0898c478c56e22958d35aa271d671b3eb58f45069369b1df19480433f0"

usage() {
  cat <<EOF
Usage: extract_proprietary_blobs.sh [OPTIONS]

Options:
  --host-run PATH       Path to NVIDIA-Linux-x86_64-580.196.09-vgpu-kvm.run
  --guest-580-run PATH  Path to NVIDIA-Linux-x86_64-580.196.07-grid.run
  --win-582-exe PATH    Path to 582.99_grid_win10_win11_server2022_server_2025_dch_64bit_international.exe
  --host-535-run PATH   Optional 535.309.01-vgpu-kvm.run for legacy BLOB_MANIFEST.json Falcon extraction
  --out-dir DIR         Output directory for git-ignored payloads (default: ${OUT_DIR})
  --work-dir DIR        Temporary extraction directory (default: ${WORK_DIR})
  -h, --help            Show this help message

Example:
  ./scripts/extract_proprietary_blobs.sh \\
    --host-run      /path/to/NVIDIA-Linux-x86_64-580.196.09-vgpu-kvm.run \\
    --guest-580-run /path/to/NVIDIA-Linux-x86_64-580.196.07-grid.run \\
    --win-582-exe   /path/to/582.99_grid_win10_win11_server2022_server_2025_dch_64bit_international.exe
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host-run)      HOST_580_RUN="$2"; shift 2 ;;
    --guest-580-run) GUEST_580_RUN="$2"; shift 2 ;;
    --win-582-exe)   WIN_582_EXE="$2"; shift 2 ;;
    --host-535-run)  HOST_535_RUN="$2"; shift 2 ;;
    --out-dir)       OUT_DIR="$2"; shift 2 ;;
    --work-dir)      WORK_DIR="$2"; shift 2 ;;
    -h|--help)       usage; exit 0 ;;
    *)               echo "Unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

if [[ -z "${HOST_580_RUN}" && -z "${GUEST_580_RUN}" && -z "${WIN_582_EXE}" && -z "${HOST_535_RUN}" ]]; then
  echo "ERROR: Specify at least one of --host-run, --guest-580-run, --win-582-exe, or --host-535-run." >&2
  usage
  exit 2
fi

mkdir -p "${OUT_DIR}" "${WORK_DIR}"

if [[ -n "${HOST_580_RUN}" ]]; then
  [[ -f "${HOST_580_RUN}" ]] || { echo "ERROR: Missing --host-run: ${HOST_580_RUN}" >&2; exit 1; }
  echo "[R580 Host] Extracting 580.196.09-vgpu-kvm ..."
  HOST_OUT="${OUT_DIR}/host-580"
  rm -rf "${HOST_OUT}"
  sh "${HOST_580_RUN}" --extract-only --target "${HOST_OUT}" >/dev/null
  echo "  -> Extracted host 580.196.09-vgpu-kvm tree into ${HOST_OUT}"
fi

if [[ -n "${GUEST_580_RUN}" ]]; then
  [[ -f "${GUEST_580_RUN}" ]] || { echo "ERROR: Missing --guest-580-run: ${GUEST_580_RUN}" >&2; exit 1; }
  echo "[R580 Linux Guest] Verifying & extracting 580.196.07-grid ..."
  GOT_G580_SHA="$(sha256sum "${GUEST_580_RUN}" | awk '{print $1}')"
  if [[ "${GOT_G580_SHA}" != "${EXPECTED_GUEST_580_RUN_SHA256}" ]]; then
    echo "WARNING: 580.196.07-grid.run SHA-256 differs from reference: ${GOT_G580_SHA}" >&2
  fi
  GUEST_580_OUT="${OUT_DIR}/guest-580"
  rm -rf "${GUEST_580_OUT}"
  sh "${GUEST_580_RUN}" --extract-only --target "${GUEST_580_OUT}" >/dev/null
  if [[ -f "${REPO_ROOT}/tools/patch_r580_guest_nv_pci.py" ]]; then
    python3 "${REPO_ROOT}/tools/patch_r580_guest_nv_pci.py" "${GUEST_580_OUT}/kernel" || true
  fi
  echo "  -> Extracted guest 580.196.07-grid tree into ${GUEST_580_OUT}"
fi

if [[ -n "${WIN_582_EXE}" ]]; then
  [[ -f "${WIN_582_EXE}" ]] || { echo "ERROR: Missing --win-582-exe: ${WIN_582_EXE}" >&2; exit 1; }
  command -v 7z >/dev/null 2>&1 || { echo "ERROR: 7z (p7zip-full) is required for --win-582-exe" >&2; exit 1; }
  echo "[R580 Windows Guest] Verifying & extracting WHQL 582.99-grid ..."
  GOT_W582_SHA="$(sha256sum "${WIN_582_EXE}" | awk '{print $1}')"
  if [[ "${GOT_W582_SHA}" != "${EXPECTED_WIN_582_EXE_SHA256}" ]]; then
    echo "WARNING: Windows 582.99 .exe SHA-256 differs from reference: ${GOT_W582_SHA}" >&2
  fi
  WIN_582_OUT="${OUT_DIR}/win-582"
  rm -rf "${WIN_582_OUT}"
  mkdir -p "${WIN_582_OUT}"
  7z x -y -o"${WIN_582_OUT}" "${WIN_582_EXE}" >/dev/null
  echo "  -> Extracted Windows 582.99 Display.Driver (nvgridsw.inf + nvxdapix.dll) into ${WIN_582_OUT}"
fi

if [[ -n "${HOST_535_RUN}" ]]; then
  [[ -f "${HOST_535_RUN}" ]] || { echo "ERROR: Missing --host-535-run: ${HOST_535_RUN}" >&2; exit 1; }
  echo "[Legacy Baseline] Extracting 535.309.01 Falcon archive blobs ..."
  H535_TMP="${WORK_DIR}/host-535"
  rm -rf "${H535_TMP}"
  sh "${HOST_535_RUN}" --extract-only --target "${H535_TMP}" >/dev/null
  NV_KOBJ="${H535_TMP}/kernel/nvidia/nv-kernel.o_binary"
  GOT_SHA="$(sha256sum "${NV_KOBJ}" | awk '{print $1}')"
  if [[ "${GOT_SHA}" != "${EXPECTED_HOST_535_KERNEL_SHA256}" ]]; then
    echo "ERROR: Host 535 nv-kernel.o_binary SHA-256 mismatch: ${GOT_SHA}" >&2
    exit 1
  fi
  python3 "${REPO_ROOT}/tools/extract_vgpu_535_blobs.py" "${NV_KOBJ}" \
    --manifest "${REPO_ROOT}/firmware-blobs/gp104-vgpu-535/BLOB_MANIFEST.json" \
    --out-dir "${REPO_ROOT}/firmware-blobs/gp104-vgpu-535"
fi

echo "Done. Proprietary outputs stored strictly outside git tracking."
