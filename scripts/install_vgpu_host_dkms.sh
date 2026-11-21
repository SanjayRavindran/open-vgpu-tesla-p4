#!/usr/bin/env bash
set -euo pipefail

# Reproducible DKMS host installer for NVIDIA 535.309.01-vgpu-kvm on:
#   - Ubuntu 24.04 / 26.04 KVM hosts
#   - Proxmox VE 8 / 9 hosts
# Packages nvidia.ko + nvidia-vgpu-vfio.ko under DKMS and installs our clean-room
# open-source host daemons (open-nvidia-vgpud, libopen-vgxcfg.so, open-nvidia-xid-logd,
# open-nvidia-vgpu-mgr) alongside the proven stable libnvidia-vgpu/nvidia-smi host components.

RUN_FILE=""
OPEN_VGPUD=""
OPEN_VGXCFG=""
OPEN_XID_LOGD=""
OPEN_VGPU_MGR=""
SKIP_BUILD_OPEN=0
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: install_vgpu_host_dkms.sh --run-file <NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run> [options]

Options:
  --run-file <path>       Path to NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run (required unless --dry-run)
  --open-vgpud <path>     Prebuilt open-nvidia-vgpud binary (optional; auto-built if cargo present)
  --open-vgxcfg <path>    Prebuilt libopen-vgxcfg.so library (optional; auto-built if gcc present)
  --open-xid-logd <path>  Prebuilt open-nvidia-xid-logd binary (optional; auto-built if gcc present)
  --open-vgpu-mgr <path>  Prebuilt open-nvidia-vgpu-mgr binary (optional; auto-built if gcc present)
  --skip-build-open       Do not invoke cargo/gcc to build missing open daemons
  --dry-run               Print exact planned actions without modifying the system
  -h, --help              Show this help text
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --run-file) RUN_FILE="${2:-}"; shift 2 ;;
    --open-vgpud) OPEN_VGPUD="${2:-}"; shift 2 ;;
    --open-vgxcfg) OPEN_VGXCFG="${2:-}"; shift 2 ;;
    --open-xid-logd) OPEN_XID_LOGD="${2:-}"; shift 2 ;;
    --open-vgpu-mgr) OPEN_VGPU_MGR="${2:-}"; shift 2 ;;
    --skip-build-open) SKIP_BUILD_OPEN=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PKG_NAME="nvidia-vgpu-kvm"
PKG_VER="535.309.01"
DKMS_SRC="/usr/src/${PKG_NAME}-${PKG_VER}"
OPT_DIR="/opt/nvidia-vgpu-535"
KVER="$(uname -r)"

IS_PROXMOX=0
if command -v pveversion >/dev/null 2>&1 || [ -f /etc/pve/local/pve-ssl.pem ]; then
  IS_PROXMOX=1
fi

run_cmd() {
  if [ "$DRY_RUN" -eq 1 ]; then
    printf '[dry-run] %s\n' "$*"
  else
    eval "$@"
  fi
}

if [ "$DRY_RUN" -eq 0 ]; then
  if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root (sudo)." >&2
    exit 1
  fi
  if [ -z "$RUN_FILE" ] || [ ! -f "$RUN_FILE" ]; then
    echo "Missing --run-file <NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run>." >&2
    exit 1
  fi
fi

echo "=== Host vGPU DKMS Installer (${PKG_NAME}/${PKG_VER}, kernel ${KVER}, proxmox=${IS_PROXMOX}) ==="

# 1. Install prerequisite build/DKMS packages
if [ "$IS_PROXMOX" -eq 1 ]; then
  HEADERS_PKG="pve-headers-${KVER}"
  echo "Detected Proxmox VE host; prerequisite packages: dkms build-essential ${HEADERS_PKG} mdevctl"
else
  HEADERS_PKG="linux-headers-${KVER}"
  echo "Detected Ubuntu/Debian KVM host; prerequisite packages: dkms build-essential ${HEADERS_PKG} qemu-system-x86 ovmf swtpm mdevctl"
fi

if [ "$DRY_RUN" -eq 1 ]; then
  echo "[dry-run] apt-get update && apt-get install -y dkms build-essential ${HEADERS_PKG} mdevctl"
else
  DEBIAN_FRONTEND=noninteractive apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq dkms build-essential "${HEADERS_PKG}" mdevctl || \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq dkms build-essential mdevctl
fi

# 2. Build open-source host daemons if not explicitly passed
if [ "$SKIP_BUILD_OPEN" -eq 0 ] && [ "$DRY_RUN" -eq 0 ]; then
  if [ -z "$OPEN_VGPUD" ] && [ -f "$REPO_ROOT/nova-vgpu/Cargo.toml" ] && command -v cargo >/dev/null 2>&1; then
    (cd "$REPO_ROOT/nova-vgpu" && cargo build --release --bin open-nvidia-vgpud)
    OPEN_VGPUD="$REPO_ROOT/nova-vgpu/target/release/open-nvidia-vgpud"
  fi
  if [ -z "$OPEN_VGXCFG" ] && [ -f "$REPO_ROOT/open-vgxcfg/Makefile" ] && command -v gcc >/dev/null 2>&1; then
    make -C "$REPO_ROOT/open-vgxcfg" all
    OPEN_VGXCFG="$REPO_ROOT/open-vgxcfg/libopen-vgxcfg.so"
  fi
  if [ -z "$OPEN_XID_LOGD" ] && [ -f "$REPO_ROOT/open-xid-logd/Makefile" ] && command -v gcc >/dev/null 2>&1; then
    make -C "$REPO_ROOT/open-xid-logd" all
    OPEN_XID_LOGD="$REPO_ROOT/open-xid-logd/open-nvidia-xid-logd"
  fi
  if [ -z "$OPEN_VGPU_MGR" ] && [ -f "$REPO_ROOT/open-vgpu-mgr/Makefile" ] && command -v gcc >/dev/null 2>&1; then
    make -C "$REPO_ROOT/open-vgpu-mgr" all
    OPEN_VGPU_MGR="$REPO_ROOT/open-vgpu-mgr/open-nvidia-vgpu-mgr"
  fi
  if [ -f "$REPO_ROOT/open-nvml-smi/Makefile" ] && command -v gcc >/dev/null 2>&1; then
    make -C "$REPO_ROOT/open-nvml-smi" all
  fi
else
  echo "[plan] Resolve/build open-nvidia-vgpud, libopen-vgxcfg.so, open-nvidia-xid-logd, open-nvidia-vgpu-mgr, open-nvml-smi"
fi

if [ "$DRY_RUN" -eq 0 ]; then
  for req_bin in "$OPEN_VGPUD" "$OPEN_VGXCFG" "$OPEN_XID_LOGD" "$OPEN_VGPU_MGR"; do
    if [ -z "$req_bin" ] || [ ! -f "$req_bin" ]; then
      echo "Missing open-source host daemon artifact ($req_bin). Pass --open-* or install cargo+gcc." >&2
      exit 1
    fi
  done
fi

# 3. Extract NVIDIA .run payload & register DKMS
if [ "$DRY_RUN" -eq 1 ]; then
  echo "[dry-run] Extract ${RUN_FILE:-NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run} -> ${DKMS_SRC}"
  echo "[dry-run] Write ${DKMS_SRC}/dkms.conf (nvidia + nvidia-vgpu-vfio -> /updates/dkms)"
  echo "[dry-run] dkms add -m ${PKG_NAME} -v ${PKG_VER} && dkms build -m ${PKG_NAME} -v ${PKG_VER} && dkms install -m ${PKG_NAME} -v ${PKG_VER}"
  echo "[dry-run] Install ${OPT_DIR}/{nvidia-vgpu-mgr,nvidia-smi,open-nvidia-vgpud->nvidia-vgpud,open-nvidia-xid-logd->nvidia-xid-logd} and lib/"
  echo "[dry-run] Configure /etc/modprobe.d/blacklist-nouveau.conf and /etc/modules-load.d/nvidia-vgpu.conf (remove stale nvidia-p4.conf)"
  echo "[dry-run] Write systemd units: nvidia-vgpud.service (Type=oneshot), nvidia-vgpu-mgr.service, nvidia-xid-logd.service"
  exit 0
fi

TMP_EXTRACT="$(mktemp -d /tmp/vgpu535_dkms.XXXXXX)"
trap 'rm -rf "$TMP_EXTRACT"' EXIT
bash "$RUN_FILE" --extract-only --target "$TMP_EXTRACT/pkg" >/dev/null

# Remove any prior DKMS registration of this version cleanly
if dkms status -m "$PKG_NAME" -v "$PKG_VER" | grep -q "$PKG_NAME"; then
  dkms remove -m "$PKG_NAME" -v "$PKG_VER" --all || true
fi
rm -rf "$DKMS_SRC"
mkdir -p "$DKMS_SRC"
cp -a "$TMP_EXTRACT/pkg/kernel/." "$DKMS_SRC/"

cat >"${DKMS_SRC}/dkms.conf" <<EOF
PACKAGE_NAME="${PKG_NAME}"
PACKAGE_VERSION="${PKG_VER}"
AUTOINSTALL="yes"
# Quote 'make' so DKMS does not inject KERNELRELEASE= and prematurely trigger Kbuild.
MAKE[0]="'make' -j\$(nproc) KERNEL_UNAME=\${kernelver} SYSSRC=\${kernel_source_dir} modules"
CLEAN="'make' KERNEL_UNAME=\${kernelver} SYSSRC=\${kernel_source_dir} clean"
BUILT_MODULE_NAME[0]="nvidia"
BUILT_MODULE_LOCATION[0]="./"
DEST_MODULE_LOCATION[0]="/updates/dkms"
BUILT_MODULE_NAME[1]="nvidia-vgpu-vfio"
BUILT_MODULE_LOCATION[1]="./"
DEST_MODULE_LOCATION[1]="/updates/dkms"
EOF

# Remove unmanaged duplicates under extra/ before and after DKMS install so updates/dkms/ wins
rm -f "/lib/modules/${KVER}/extra/nvidia.ko"* "/lib/modules/${KVER}/extra/nvidia-vgpu-vfio.ko"*
dkms add -m "$PKG_NAME" -v "$PKG_VER"
dkms build -m "$PKG_NAME" -v "$PKG_VER" -k "$KVER"
dkms install --force -m "$PKG_NAME" -v "$PKG_VER" -k "$KVER"
depmod -a "$KVER"

# 4. Install userspace runtime in /opt/nvidia-vgpu-535 (open daemons + stable data plane)
mkdir -p "${OPT_DIR}/lib" /usr/share/nvidia/vgpu /usr/share/nvidia/vgx
install -m 0755 "$OPEN_VGPUD" "${OPT_DIR}/nvidia-vgpud"
install -m 0755 "$OPEN_XID_LOGD" "${OPT_DIR}/nvidia-xid-logd"
install -m 0755 "$OPEN_VGPU_MGR" "${OPT_DIR}/nvidia-vgpu-mgr"
install -m 0755 "$OPEN_VGXCFG" "${OPT_DIR}/lib/libnvidia-vgxcfg.so.${PKG_VER}"
ln -sf "libnvidia-vgxcfg.so.${PKG_VER}" "${OPT_DIR}/lib/libnvidia-vgxcfg.so.1"
ln -sf "libnvidia-vgxcfg.so.${PKG_VER}" "${OPT_DIR}/lib/libnvidia-vgxcfg.so"
if [ -f "$REPO_ROOT/open-nvml-smi/open-nvidia-smi" ] && [ -f "$REPO_ROOT/open-nvml-smi/libopen-nvidia-ml.so" ]; then
  install -m 0755 "$REPO_ROOT/open-nvml-smi/open-nvidia-smi" "${OPT_DIR}/open-nvidia-smi"
  install -m 0755 "$REPO_ROOT/open-nvml-smi/libopen-nvidia-ml.so" "${OPT_DIR}/libopen-nvidia-ml.so"
fi

# Retain stable vmiop plugin, nvidia-smi, and NVML from .run
install -m 0755 "$TMP_EXTRACT/pkg/nvidia-smi" "${OPT_DIR}/nvidia-smi"
install -m 0755 "$TMP_EXTRACT/pkg/libnvidia-vgpu.so.${PKG_VER}" "${OPT_DIR}/lib/libnvidia-vgpu.so.${PKG_VER}"
ln -sf "libnvidia-vgpu.so.${PKG_VER}" "${OPT_DIR}/lib/libnvidia-vgpu.so.1"
ln -sf "libnvidia-vgpu.so.${PKG_VER}" "${OPT_DIR}/lib/libnvidia-vgpu.so"
install -m 0755 "$TMP_EXTRACT/pkg/libnvidia-ml.so.${PKG_VER}" "${OPT_DIR}/lib/libnvidia-ml.so.${PKG_VER}"
ln -sf "libnvidia-ml.so.${PKG_VER}" "${OPT_DIR}/lib/libnvidia-ml.so.1"

if [ -f "$TMP_EXTRACT/pkg/vgpuConfig.xml" ]; then
  install -m 0644 "$TMP_EXTRACT/pkg/vgpuConfig.xml" /usr/share/nvidia/vgpu/vgpuConfig.xml
fi
if [ -f "$TMP_EXTRACT/pkg/nvVgxConfig.xml" ]; then
  install -m 0644 "$TMP_EXTRACT/pkg/nvVgxConfig.xml" /usr/share/nvidia/vgx/nvVgxConfig.xml
fi

# Clean any leftover R580 libraries that would cause NVRM API mismatches
rm -f /usr/lib/x86_64-linux-gnu/*580.* 2>/dev/null || true
ln -sf "${OPT_DIR}/nvidia-smi" /usr/bin/nvidia-smi
ln -sf "${OPT_DIR}/lib/libnvidia-ml.so.${PKG_VER}" /usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1
printf '%s\n' "${OPT_DIR}/lib" > /etc/ld.so.conf.d/nvidia-vgpu-535.conf
ldconfig

# 5. Clean kernel modprobe/autoload config (remove stale bare-metal R580 configs)
rm -f /etc/modules-load.d/nvidia-p4.conf
cat > /etc/modprobe.d/blacklist-nouveau.conf <<'EOF'
# Tesla P4 KVM vGPU host: keep nouveau unbound so DKMS nvidia + nvidia-vgpu-vfio own 10de:1bb3.
blacklist nouveau
blacklist lbm-nouveau
options nouveau modeset=0
alias nouveau off
alias lbm-nouveau off
EOF

cat > /etc/modules-load.d/nvidia-vgpu.conf <<'EOF'
nvidia
nvidia-vgpu-vfio
vfio-pci
mdev
EOF

# 6. Install systemd units
cat > /etc/systemd/system/nvidia-vgpud.service <<EOF
[Unit]
Description=Open-Source NVIDIA vGPU Registration Daemon (${PKG_VER} DKMS)
After=systemd-modules-load.service
Wants=systemd-modules-load.service

[Service]
Type=oneshot
RemainAfterExit=yes
Environment=NV_VGPU_CFG_FILE=/usr/share/nvidia/vgpu/vgpuConfig.xml
ExecStart=${OPT_DIR}/nvidia-vgpud
ExecStartPost=/sbin/modprobe nvidia_vgpu_vfio

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/nvidia-vgpu-mgr.service <<EOF
[Unit]
Description=NVIDIA vGPU Manager Daemon (${PKG_VER})
After=nvidia-vgpud.service
Requires=nvidia-vgpud.service

[Service]
Type=forking
Environment=LD_LIBRARY_PATH=${OPT_DIR}/lib
ExecStart=${OPT_DIR}/nvidia-vgpu-mgr
ExecStopPost=/bin/rm -rf /var/run/nvidia-vgpu-mgr

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/nvidia-xid-logd.service <<EOF
[Unit]
Description=Open-Source NVIDIA XID Logger Daemon (${PKG_VER})
After=systemd-modules-load.service

[Service]
Type=forking
RuntimeDirectory=nvidia-xid-logd
Environment=LD_LIBRARY_PATH=${OPT_DIR}/lib
ExecStart=${OPT_DIR}/nvidia-xid-logd

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable nvidia-vgpud.service nvidia-vgpu-mgr.service nvidia-xid-logd.service
echo "=== Installed ${PKG_NAME}/${PKG_VER} DKMS + open host daemons cleanly ==="
