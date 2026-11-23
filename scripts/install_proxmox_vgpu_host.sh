#!/usr/bin/env bash
set -euo pipefail

# Dedicated End-to-End Proxmox VE 8 / 9 Host Configurator & vGPU Installer
# for NVIDIA Tesla P4 (GP104GL, 10de:1bb3) 535.309.01-vgpu-kvm:
#   1. Optionally configures Proxmox pve-no-subscription APT repository
#   2. Installs proxmox-headers-$(uname -r) / pve-headers-$(uname -r), DKMS, mdevctl, cargo, gcc, python3-venv
#   3. Configures Intel/AMD IOMMU (intel_iommu=on / amd_iommu=on iommu=pt) in GRUB & proxmox-boot-tool (/etc/kernel/cmdline)
#   4. Configures /etc/modules, /etc/modules-load.d/nvidia-vgpu.conf, /etc/modprobe.d/blacklist-nouveau.conf & update-initramfs
#   5. Builds & installs DKMS nvidia-vgpu-kvm/535.309.01 + all 8 clean-room open-source host daemons/libraries
#   6. Installs native systemd FastAPI-DLS license server (/opt/fastapi-dls/.venv)
#   7. Optionally creates or imports the 50/50 GRID P4-4Q VMs (VMID 101 vm-alma + VMID 102 vm-win11)

RUN_FILE=""
DLS_URL=""
DLS_PORT="443"
SKIP_DLS=0
ENABLE_NO_SUB_REPO=0
SETUP_VMS_MODE=""
STORAGE="local-lvm"
BRIDGE="vmbr0"
PCI_BDF="auto"
MDEV_TYPE="nvidia-65"
ALMA_VMID="101"
WIN11_VMID="102"
ALMA_DISK=""
WIN11_DISK=""
SKIP_BUILD_OPEN=0
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: install_proxmox_vgpu_host.sh --run-file <NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run> [options]

Host & Driver Options:
  --run-file <path>              Path to NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run (required unless --dry-run)
  --enable-no-subscription-repo  Configure Proxmox pve-no-subscription APT repo & disable enterprise repo lockouts
  --skip-build-open              Use prebuilt open-source host daemon binaries without invoking cargo/gcc

FastAPI-DLS License Server Options (Native Systemd on Proxmox VE):
  --dls-url <ip_or_host>         Proxmox host LAN IP/hostname for FastAPI-DLS (default: auto-detected via hostname -I)
  --dls-port <port>              FastAPI-DLS HTTPS port (default: 443; use 8443 if port 443 is reserved)
  --skip-dls                     Skip installing FastAPI-DLS license server

Optional Proxmox 50/50 VM Creation / Migration:
  --setup-vms <fresh|import-qcow2>  Also configure VMID 101 (vm-alma) & VMID 102 (vm-win11) with GRID P4-4Q (nvidia-65)
  --storage <name>                  Proxmox storage target for VMs (default: local-lvm)
  --bridge <br>                     Proxmox network bridge for VMs (default: vmbr0)
  --pci-bdf <bdf|auto>              Tesla P4 PCI BDF (default: auto)
  --mdev-type <type>                Mediated device type (default: nvidia-65 = GRID P4-4Q)
  --alma-vmid <id>                  VMID for Linux guest (default: 101)
  --win11-vmid <id>                 VMID for Windows 11 guest (default: 102)
  --alma-disk <path>                AlmaLinux/Linux qcow2 path (required if --setup-vms import-qcow2)
  --win11-disk <path>               Windows 11 qcow2 path (required if --setup-vms import-qcow2)

Common Options:
  --dry-run                      Print exact planned Proxmox VE host configuration commands without modifying the system
  -h, --help                     Show this help message
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --run-file) RUN_FILE="${2:-}"; shift 2 ;;
    --dls-url) DLS_URL="${2:-}"; shift 2 ;;
    --dls-port) DLS_PORT="${2:-}"; shift 2 ;;
    --skip-dls) SKIP_DLS=1; shift ;;
    --enable-no-subscription-repo) ENABLE_NO_SUB_REPO=1; shift ;;
    --setup-vms) SETUP_VMS_MODE="${2:-}"; shift 2 ;;
    --storage) STORAGE="${2:-}"; shift 2 ;;
    --bridge) BRIDGE="${2:-}"; shift 2 ;;
    --pci-bdf) PCI_BDF="${2:-}"; shift 2 ;;
    --mdev-type) MDEV_TYPE="${2:-}"; shift 2 ;;
    --alma-vmid) ALMA_VMID="${2:-}"; shift 2 ;;
    --win11-vmid) WIN11_VMID="${2:-}"; shift 2 ;;
    --alma-disk) ALMA_DISK="${2:-}"; shift 2 ;;
    --win11-disk) WIN11_DISK="${2:-}"; shift 2 ;;
    --skip-build-open) SKIP_BUILD_OPEN=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KVER="$(uname -r)"

if [ -z "$DLS_URL" ]; then
  DETECTED_IP="$(hostname -I 2>/dev/null | awk '{print $1}' || true)"
  DLS_URL="${DETECTED_IP:-192.168.1.24}"
fi
DLS_HOST="${DLS_URL#https://}"
DLS_HOST="${DLS_HOST#http://}"
DLS_HOST="${DLS_HOST%%:*}"

CPU_VENDOR="$(awk -F: '/vendor_id/{gsub(/^[ \t]+/,"",$2); print $2; exit}' /proc/cpuinfo 2>/dev/null || echo "GenuineIntel")"
if [ "$CPU_VENDOR" = "AuthenticAMD" ]; then
  IOMMU_ARGS="amd_iommu=on iommu=pt"
else
  IOMMU_ARGS="intel_iommu=on iommu=pt"
fi

echo "=== Proxmox VE 8/9 Tesla P4 vGPU Host Configurator & Installer (kernel=${KVER}, iommu='${IOMMU_ARGS}') ==="

if [ "$DRY_RUN" -eq 0 ]; then
  if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root (sudo) on the Proxmox VE host." >&2
    exit 1
  fi
  if [ -z "$RUN_FILE" ] || [ ! -f "$RUN_FILE" ]; then
    echo "Missing --run-file <NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run>." >&2
    exit 1
  fi
fi

# 1. Optional Proxmox pve-no-subscription APT repository setup
if [ "$ENABLE_NO_SUB_REPO" -eq 1 ]; then
  CODENAME="$(. /etc/os-release 2>/dev/null && echo "${VERSION_CODENAME:-bookworm}" || echo "bookworm")"
  if [ "$DRY_RUN" -eq 1 ]; then
    echo "[dry-run] Disable pve-enterprise / ceph-enterprise APT sources and enable http://download.proxmox.com/debian/pve ${CODENAME} pve-no-subscription"
  else
    for f in /etc/apt/sources.list.d/pve-enterprise.list /etc/apt/sources.list.d/ceph.list; do
      [ -f "$f" ] && sed -i 's/^deb /#deb /' "$f"
    done
    for f in /etc/apt/sources.list.d/pve-enterprise.sources /etc/apt/sources.list.d/ceph.sources; do
      [ -f "$f" ] && mv -f "$f" "${f}.disabled"
    done
    cat > /etc/apt/sources.list.d/pve-no-subscription.list <<EOF
deb http://download.proxmox.com/debian/pve ${CODENAME} pve-no-subscription
EOF
  fi
fi

# 2. Install Proxmox VE headers, DKMS, build tools, Rust/Cargo & Python venv
if [ "$DRY_RUN" -eq 1 ]; then
  echo "[dry-run] apt-get update && apt-get install -y dkms build-essential proxmox-headers-${KVER} (or pve-headers-${KVER}) mdevctl pciutils curl openssl git python3 python3-venv python3-pip cargo rustc"
else
  DEBIAN_FRONTEND=noninteractive apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    dkms build-essential "proxmox-headers-${KVER}" mdevctl pciutils curl openssl git python3 python3-venv python3-pip cargo rustc || \
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    dkms build-essential "pve-headers-${KVER}" mdevctl pciutils curl openssl git python3 python3-venv python3-pip cargo rustc || \
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    dkms build-essential proxmox-default-headers mdevctl pciutils curl openssl git python3 python3-venv python3-pip cargo rustc
fi

# 3. Configure IOMMU in Proxmox GRUB and/or systemd-boot (proxmox-boot-tool)
if [ "$DRY_RUN" -eq 1 ]; then
  echo "[dry-run] Ensure '${IOMMU_ARGS}' in /etc/default/grub (update-grub) and /etc/kernel/cmdline (proxmox-boot-tool refresh)"
  echo "[dry-run] Ensure vfio, vfio_iommu_type1, vfio_pci, mdev in /etc/modules and run update-initramfs -u -k all"
else
  NEED_BOOT_REFRESH=0
  if [ -f /etc/default/grub ]; then
    if ! grep -q "iommu=pt" /etc/default/grub; then
      sed -i -E "s|^GRUB_CMDLINE_LINUX_DEFAULT=\"([^\"]*)\"|GRUB_CMDLINE_LINUX_DEFAULT=\"\1 ${IOMMU_ARGS}\"|" /etc/default/grub
      NEED_BOOT_REFRESH=1
    fi
    if [ "$NEED_BOOT_REFRESH" -eq 1 ] && command -v update-grub >/dev/null 2>&1; then
      update-grub
    fi
  fi
  if [ -f /etc/kernel/cmdline ]; then
    if ! grep -q "iommu=pt" /etc/kernel/cmdline; then
      sed -i -E "s/$/ ${IOMMU_ARGS}/" /etc/kernel/cmdline
      if command -v proxmox-boot-tool >/dev/null 2>&1; then
        proxmox-boot-tool refresh || true
      fi
    fi
  fi

  touch /etc/modules
  for mod in vfio vfio_iommu_type1 vfio_pci mdev; do
    grep -qxF "$mod" /etc/modules 2>/dev/null || echo "$mod" >> /etc/modules
  done
fi

# 4. Run DKMS + all 8 clean-room open-source host daemons/libraries installer
DKMS_ARGS=()
[ -n "$RUN_FILE" ] && DKMS_ARGS+=(--run-file "$RUN_FILE")
[ "$SKIP_BUILD_OPEN" -eq 1 ] && DKMS_ARGS+=(--skip-build-open)
[ "$DRY_RUN" -eq 1 ] && DKMS_ARGS+=(--dry-run)
bash "$SCRIPT_DIR/install_vgpu_host_dkms.sh" "${DKMS_ARGS[@]}"

if [ "$DRY_RUN" -eq 0 ] && command -v update-initramfs >/dev/null 2>&1; then
  update-initramfs -u -k all
fi

# 5. Install native systemd FastAPI-DLS license server on Proxmox VE
if [ "$SKIP_DLS" -eq 0 ]; then
  DLS_ARGS=(--mode systemd --dls-url "$DLS_HOST" --port "$DLS_PORT")
  [ "$DRY_RUN" -eq 1 ] && DLS_ARGS+=(--dry-run)
  bash "$SCRIPT_DIR/install_fastapi_dls.sh" "${DLS_ARGS[@]}"
fi

# 6. Optionally create or migrate Proxmox 50/50 VMs (VMID 101 vm-alma + VMID 102 vm-win11)
if [ -n "$SETUP_VMS_MODE" ]; then
  VM_ARGS=(
    --mode "$SETUP_VMS_MODE"
    --storage "$STORAGE"
    --bridge "$BRIDGE"
    --pci-bdf "$PCI_BDF"
    --mdev-type "$MDEV_TYPE"
    --alma-vmid "$ALMA_VMID"
    --win11-vmid "$WIN11_VMID"
  )
  [ -n "$ALMA_DISK" ] && VM_ARGS+=(--alma-disk "$ALMA_DISK")
  [ -n "$WIN11_DISK" ] && VM_ARGS+=(--win11-disk "$WIN11_DISK")
  [ "$DRY_RUN" -eq 1 ] && VM_ARGS+=(--dry-run)
  bash "$SCRIPT_DIR/setup_proxmox_vgpu_vms.sh" "${VM_ARGS[@]}"
fi

echo "=== Proxmox VE 8/9 Tesla P4 vGPU Host Configuration Complete ==="
