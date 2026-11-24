#!/usr/bin/env bash
set -euo pipefail

# Build and install clean-room libopen-nvidia-encode.so (NVENC 13.0 for Pascal GP104)
# onto a Linux vGPU guest, preserving a reversible .vendor.bak copy of the
# vendor libnvidia-encode.so.<ver> binary and refreshing NVIDIA Container Toolkit CDI specs.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

PREBUILT=""
LIBDIR_OVERRIDE=""
MODE="install"
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: install_open_nvenc.sh [options]

Options:
  --prebuilt <path>   Install a prebuilt libopen-nvidia-encode.so instead of building from source
  --libdir <dir>      Override auto-detected system library directory
  --restore           Restore vendor libnvidia-encode.so.<ver>.vendor.bak and refresh linker/CDI
  --dry-run           Print planned actions without modifying the system
  -h, --help          Show this help message and exit
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --prebuilt) PREBUILT="${2:-}"; shift 2 ;;
    --libdir) LIBDIR_OVERRIDE="${2:-}"; shift 2 ;;
    --restore) MODE="restore"; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

detect_libdir() {
  if [ -n "$LIBDIR_OVERRIDE" ]; then
    echo "$LIBDIR_OVERRIDE"
    return
  fi
  if [ -d /usr/lib/x86_64-linux-gnu ] && (ls /usr/lib/x86_64-linux-gnu/libnvidia-encode.so.* >/dev/null 2>&1 || [ -f /etc/debian_version ]); then
    echo "/usr/lib/x86_64-linux-gnu"
    return
  fi
  if [ -d /usr/lib64 ] && (ls /usr/lib64/libnvidia-encode.so.* >/dev/null 2>&1 || [ -f /etc/redhat-release ]); then
    echo "/usr/lib64"
    return
  fi
  echo "/usr/local/lib"
}

find_vendor_target() {
  local libdir="$1"
  local candidate=""
  candidate="$(find "$libdir" -maxdepth 1 \( -type f -o -type l \) -name 'libnvidia-encode.so.[0-9]*' ! -name 'libnvidia-encode.so.1' ! -name '*.vendor.bak' 2>/dev/null | sort | head -n 1 || true)"
  if [ -n "$candidate" ]; then
    echo "$candidate"
    return
  fi
  local bak=""
  bak="$(find "$libdir" -maxdepth 1 -type f -name 'libnvidia-encode.so.*.vendor.bak' 2>/dev/null | sort | head -n 1 || true)"
  if [ -n "$bak" ]; then
    echo "${bak%.vendor.bak}"
    return
  fi
  echo ""
}

refresh_cdi() {
  if ! command -v nvidia-ctk >/dev/null 2>&1; then
    return
  fi
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "[dry-run] nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml /var/run/cdi/nvidia.yaml"
    return
  fi
  if [ -d /etc/cdi ] || mkdir -p /etc/cdi 2>/dev/null; then
    nvidia-ctk cdi generate --output=/etc/cdi/nvidia.yaml >/dev/null 2>&1 || true
  fi
  if [ -d /var/run/cdi ]; then
    nvidia-ctk cdi generate --output=/var/run/cdi/nvidia.yaml >/dev/null 2>&1 || true
  fi
}

LIBDIR="$(detect_libdir)"
VENDOR_TARGET="$(find_vendor_target "$LIBDIR")"

if [ "$MODE" = "restore" ]; then
  if [ -n "$VENDOR_TARGET" ] && [ -f "${VENDOR_TARGET}.vendor.bak" ]; then
    if [ "$DRY_RUN" -eq 1 ]; then
      echo "[dry-run] cp -a ${VENDOR_TARGET}.vendor.bak ${VENDOR_TARGET}"
      echo "[dry-run] ldconfig"
      refresh_cdi
      exit 0
    fi
    cp -a "${VENDOR_TARGET}.vendor.bak" "$VENDOR_TARGET"
    ln -sf "$(basename "$VENDOR_TARGET")" "${LIBDIR}/libnvidia-encode.so.1"
    ln -sf "libnvidia-encode.so.1" "${LIBDIR}/libnvidia-encode.so"
    ldconfig
    refresh_cdi
    echo "Restored vendor NVENC library: $VENDOR_TARGET"
    exit 0
  fi
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "[dry-run] No .vendor.bak found in $LIBDIR"
    exit 0
  fi
  echo "No vendor backup (*.vendor.bak) found in $LIBDIR" >&2
  exit 1
fi

BUILT_TEMP=0
SO_PATH="$PREBUILT"
if [ -z "$SO_PATH" ]; then
  if [ "$DRY_RUN" -eq 1 ]; then
    SO_PATH="${REPO_ROOT}/open-nvenc/libopen-nvidia-encode.so"
    echo "[dry-run] make -C ${REPO_ROOT}/open-nvenc clean all"
  else
    command -v gcc >/dev/null 2>&1 || { echo "gcc is required to build open-nvenc (or pass --prebuilt)" >&2; exit 1; }
    make -C "${REPO_ROOT}/open-nvenc" clean all
    SO_PATH="${REPO_ROOT}/open-nvenc/libopen-nvidia-encode.so"
    BUILT_TEMP=1
  fi
elif [ ! -f "$SO_PATH" ] && [ "$DRY_RUN" -eq 0 ]; then
  echo "Prebuilt library not found: $SO_PATH" >&2
  exit 1
fi

if [ "$DRY_RUN" -eq 1 ]; then
  echo "[dry-run] LIBDIR=${LIBDIR} VENDOR_TARGET=${VENDOR_TARGET:-<none>}"
  if [ -n "$VENDOR_TARGET" ]; then
    echo "[dry-run] backup ${VENDOR_TARGET} -> ${VENDOR_TARGET}.vendor.bak (if missing)"
    echo "[dry-run] install -m 0755 ${SO_PATH} ${VENDOR_TARGET}"
  fi
  echo "[dry-run] install -m 0755 ${SO_PATH} ${LIBDIR}/libopen-nvidia-encode.so"
  echo "[dry-run] ldconfig"
  refresh_cdi
  exit 0
fi

mkdir -p "$LIBDIR"
install -m 0755 "$SO_PATH" "${LIBDIR}/libopen-nvidia-encode.so"

if [ -n "$VENDOR_TARGET" ]; then
  if [ -e "$VENDOR_TARGET" ] && [ ! -e "${VENDOR_TARGET}.vendor.bak" ]; then
    cp -a "$VENDOR_TARGET" "${VENDOR_TARGET}.vendor.bak"
  fi
  install -m 0755 "$SO_PATH" "$VENDOR_TARGET"
  ln -sf "$(basename "$VENDOR_TARGET")" "${LIBDIR}/libnvidia-encode.so.1"
else
  ln -sf "libopen-nvidia-encode.so" "${LIBDIR}/libnvidia-encode.so.1"
fi
ln -sf "libnvidia-encode.so.1" "${LIBDIR}/libnvidia-encode.so"

if [ "$BUILT_TEMP" -eq 1 ]; then
  make -C "${REPO_ROOT}/open-nvenc" clean >/dev/null
fi

ldconfig
refresh_cdi
echo "Installed clean-room libopen-nvidia-encode.so (NVENC 13.0) into $LIBDIR"
