#!/usr/bin/env bash
set -euo pipefail

# Phase 2B Builder & Symbol Verifier:
# Clones NVIDIA/open-gpu-kernel-modules @ 535.309.01, applies our clean-room
# GP104 BAR0 VRPC overlay & patcher, compiles full open nv-kernel.o +
# nvidia*.ko (zero closed .o_binary blobs), and verifies symbols via modinfo/nm.

TAG="535.309.01"
REPO_URL="https://github.com/NVIDIA/open-gpu-kernel-modules.git"
WORKDIR="/tmp/open-535-guest-build"
KVER="$(uname -r)"
JOBS="$(nproc 2>/dev/null || echo 4)"
DRY_RUN=0
VERIFY_ONLY=0
SKIP_KERNEL_COMPAT=0

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCHER="$SCRIPT_DIR/patch_open_535_guest_rm.py"
OVERLAY_DIR="$(cd "$SCRIPT_DIR/../overlay" && pwd)"

usage() {
    cat <<EOF
Usage: $0 [--dry-run] [--verify-only] [--workdir <dir>] [--kver <kver>] [--jobs <n>] [--skip-kernel-compat]
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run)            DRY_RUN=1; shift ;;
        --verify-only)        VERIFY_ONLY=1; shift ;;
        --workdir)            WORKDIR="${2:-}"; shift 2 ;;
        --kver)               KVER="${2:-}"; shift 2 ;;
        --jobs)               JOBS="${2:-}"; shift 2 ;;
        --skip-kernel-compat) SKIP_KERNEL_COMPAT=1; shift ;;
        -h|--help)            usage; exit 0 ;;
        *)                    echo "Unknown arg: $1" >&2; usage; exit 1 ;;
    esac
done

echo "[build-open-535-guest] Workdir=$WORKDIR KVER=$KVER Jobs=$JOBS"

if [[ $DRY_RUN -eq 1 ]]; then
    echo "[DRY-RUN] Would clone --depth 1 --branch $TAG $REPO_URL into $WORKDIR"
    echo "[DRY-RUN] Would run: python3 $PATCHER --src-dir $WORKDIR --overlay-dir $OVERLAY_DIR"
    echo "[DRY-RUN] Would build src/nvidia, src/nvidia-modeset, and kernel-open (nvidia.ko, nvidia-modeset.ko, nvidia-drm.ko, nvidia-uvm.ko) for $KVER"
    echo "[DRY-RUN] Would verify registerHalModule_GP104 and openVgpu* symbols via modinfo & nm"
    exit 0
fi

if [[ $VERIFY_ONLY -eq 0 ]]; then
    if [[ ! -d "$WORKDIR/.git" ]]; then
        rm -rf "$WORKDIR"
        git clone --depth 1 --branch "$TAG" "$REPO_URL" "$WORKDIR"
    fi

    COMPAT_ARG=""
    if [[ $SKIP_KERNEL_COMPAT -eq 1 ]]; then
        COMPAT_ARG="--skip-kernel-compat"
    fi
    python3 "$PATCHER" --src-dir "$WORKDIR" --overlay-dir "$OVERLAY_DIR" $COMPAT_ARG

    SYSOUT_DIR="/lib/modules/$KVER/build"
    SYSSRC_DIR="/lib/modules/$KVER/source"
    if [[ ! -d "$SYSSRC_DIR" ]]; then
        SYSSRC_DIR="$SYSOUT_DIR"
    fi

    echo "=== Building open-source 535.309.01 modules ($KVER, -j$JOBS) ==="
    make -C "$WORKDIR" -j"$JOBS" \
        KERNEL_UNAME="$KVER" \
        SYSSRC="$SYSSRC_DIR" \
        SYSOUT="$SYSOUT_DIR" \
        modules
fi

echo "=== Verifying built open kernel modules (zero .o_binary blobs) ==="
cd "$WORKDIR"
ls -lh \
    src/nvidia/_out/Linux_x86_64/nv-kernel.o \
    src/nvidia-modeset/_out/Linux_x86_64/nv-modeset-kernel.o \
    kernel-open/nvidia.ko \
    kernel-open/nvidia-modeset.ko \
    kernel-open/nvidia-drm.ko \
    kernel-open/nvidia-uvm.ko

for ko in kernel-open/nvidia.ko kernel-open/nvidia-modeset.ko kernel-open/nvidia-drm.ko kernel-open/nvidia-uvm.ko; do
    echo "--- modinfo $ko ---"
    /sbin/modinfo "$ko" | grep -E "^(filename|license|version|vermagic|srcversion):" || true
done

echo "--- Verify GP104 + BAR0 VRPC symbols in kernel-open/nvidia.ko ---"
for sym in \
    registerHalModule_GP104 \
    openVgpuDetectBar0VirtualMode \
    openVgpuCreateObject \
    openVgpuDestructObject \
    openVgpuBar0RpcSendMessage \
    openVgpuBar0RpcRecvPoll \
    openVgpuGetRpc; do
    nm -n kernel-open/nvidia.ko | grep -E " [tTdDrR] ${sym}$" || {
        echo "ERROR: Missing expected symbol $sym in kernel-open/nvidia.ko!" >&2
        exit 1
    }
done
echo "ALL_OPEN_535_GUEST_SYMBOLS_VERIFIED!"
