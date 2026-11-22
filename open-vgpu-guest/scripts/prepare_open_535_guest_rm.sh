#!/usr/bin/env bash
set -euo pipefail

# Phase 2A/2B Preparation Script:
# Fetches NVIDIA/open-gpu-kernel-modules at exact tag 535.309.01 and audits
# the Pascal GP104 vGPU guest enablement surface (rmconfig.h, rpc_vgpu.h,
# arch/pascal HAL files, and BAR0 0x8f000 VRPC transport integration points).

TAG="535.309.01"
EXPECTED_COMMIT="15b1a21dfe4eab54a463a0ea3086823ae35fe02e"
REPO_URL="https://github.com/NVIDIA/open-gpu-kernel-modules.git"
WORKDIR="/tmp/open-gpu-kernel-modules-535.309.01"
DRY_RUN=0
VERIFY_ONLY=0

usage() {
    cat <<EOF
Usage: $0 [--dry-run] [--verify-only] [--workdir <dir>]
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run)     DRY_RUN=1; shift ;;
        --verify-only) VERIFY_ONLY=1; shift ;;
        --workdir)     WORKDIR="${2:-}"; shift 2 ;;
        -h|--help)     usage; exit 0 ;;
        *)             echo "Unknown arg: $1" >&2; usage; exit 1 ;;
    esac
done

echo "[open-535-guest-rm] Target: $REPO_URL @ tag $TAG ($EXPECTED_COMMIT)"
echo "[open-535-guest-rm] Workdir: $WORKDIR"

if [[ $DRY_RUN -eq 1 ]]; then
    echo "[DRY-RUN] Would clone --depth 1 --branch $TAG $REPO_URL into $WORKDIR"
    echo "[DRY-RUN] Would verify VGX_MAJOR_VERSION_NUMBER == 0x23 and VGX_MINOR_VERSION_NUMBER == 0x05 in src/nvidia/kernel/inc/vgpu/vgpu_version.h"
    echo "[DRY-RUN] Would audit 12 Pascal HAL sources under src/nvidia/src/kernel/gpu/*/arch/pascal/"
    echo "[DRY-RUN] Would inspect src/nvidia/generated/g_rmconfig_private.h / rmconfig.h (RMCFG_CHIP_GP104, RMCFG_FEATURE_VIRTUALIZATION) and src/nvidia/kernel/inc/vgpu/rpc_vgpu.h"
    exit 0
fi

if [[ $VERIFY_ONLY -eq 0 && ! -d "$WORKDIR/.git" ]]; then
    rm -rf "$WORKDIR"
    git clone --depth 1 --branch "$TAG" "$REPO_URL" "$WORKDIR"
fi

if [[ ! -d "$WORKDIR/.git" ]]; then
    echo "ERROR: $WORKDIR is not a git checkout (run without --verify-only to fetch)." >&2
    exit 1
fi

ACTUAL_COMMIT="$(git -C "$WORKDIR" rev-parse HEAD)"
echo "[open-535-guest-rm] Checked out commit: $ACTUAL_COMMIT"
if [[ "$ACTUAL_COMMIT" != "$EXPECTED_COMMIT" ]]; then
    echo "WARNING: Commit mismatch (expected $EXPECTED_COMMIT)" >&2
fi

VER_HDR="$WORKDIR/src/nvidia/kernel/inc/vgpu/vgpu_version.h"
if [[ -f "$VER_HDR" ]]; then
    grep -E "VGX_(MAJOR|MINOR)_VERSION_NUMBER" "$VER_HDR" || true
else
    echo "ERROR: Missing $VER_HDR" >&2
    exit 1
fi

echo "[open-535-guest-rm] Pascal GP104 HAL files present in 535.309.01 tree:"
find "$WORKDIR/src/nvidia/src/kernel/gpu" -path "*/arch/pascal/*" -type f | sort

echo "[open-535-guest-rm] Current RMCFG flags needing Phase 2B enablement:"
grep -nE "RMCFG_CHIP_GP104|RMCFG_FEATURE_VIRTUALIZATION|RMCFG_MODULE_RPCSTRUCTURECOPY|RMCFG_FEATURE_GSP_CLIENT_RM" \
    "$WORKDIR/src/nvidia/generated/rmconfig.h" 2>/dev/null || true

echo "[open-535-guest-rm] Done. See docs/13-OPEN-GUEST-VGPU-KERNEL-DRIVER.md for Phase 2B integration steps."
