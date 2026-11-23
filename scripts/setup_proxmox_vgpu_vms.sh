#!/usr/bin/env bash
set -euo pipefail

# Reproducible Proxmox VE 8 / 9 VM setup for the 50/50 Tesla P4 split:
#   - VM 101 (vm-alma): AlmaLinux 10.3, 4 vCPU, 5120 MiB RAM, mdev nvidia-65 (GRID P4-4Q)
#   - VM 102 (vm-win11): Windows 11 24H2, 4 vCPU, 6144 MiB RAM, OVMF + TPM 2.0 + AHCI, mdev nvidia-65
# Supports both migrating existing .qcow2 disks (--mode import-qcow2) and creating clean VMs (--mode fresh).

MODE="fresh"
STORAGE="local-lvm"
BRIDGE="vmbr0"
PCI_BDF="0000:01:00.0"
MDEV_TYPE="nvidia-65"
ALMA_VMID="101"
WIN11_VMID="102"
ALMA_DISK=""
WIN11_DISK=""
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: setup_proxmox_vgpu_vms.sh [options]

Options:
  --mode <fresh|import-qcow2>  Create blank disks ('fresh') or import existing qcow2 ('import-qcow2')
  --storage <name>             Proxmox storage target (default: local-lvm)
  --bridge <br>                Proxmox network bridge (default: vmbr0)
  --pci-bdf <bdf>              Tesla P4 PCI BDF (default: 0000:01:00.0)
  --mdev-type <type>           mdev profile (default: nvidia-65 = GRID P4-4Q)
  --alma-vmid <id>             VMID for vm-alma (default: 101)
  --win11-vmid <id>            VMID for vm-win11 (default: 102)
  --alma-disk <path>           Path to AlmaLinux qcow2 (required for import-qcow2)
  --win11-disk <path>          Path to Windows 11 qcow2 (required for import-qcow2)
  --dry-run                    Print exact qm commands without executing
  -h, --help                   Show help
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --mode) MODE="${2:-}"; shift 2 ;;
    --storage) STORAGE="${2:-}"; shift 2 ;;
    --bridge) BRIDGE="${2:-}"; shift 2 ;;
    --pci-bdf) PCI_BDF="${2:-}"; shift 2 ;;
    --mdev-type) MDEV_TYPE="${2:-}"; shift 2 ;;
    --alma-vmid) ALMA_VMID="${2:-}"; shift 2 ;;
    --win11-vmid) WIN11_VMID="${2:-}"; shift 2 ;;
    --alma-disk) ALMA_DISK="${2:-}"; shift 2 ;;
    --win11-disk) WIN11_DISK="${2:-}"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [ "$MODE" != "fresh" ] && [ "$MODE" != "import-qcow2" ]; then
  echo "Invalid --mode '$MODE'" >&2
  exit 2
fi
if [ "$MODE" = "import-qcow2" ] && { [ -z "$ALMA_DISK" ] || [ -z "$WIN11_DISK" ]; }; then
  echo "--mode import-qcow2 requires both --alma-disk and --win11-disk" >&2
  exit 2
fi

run_qm() {
  if [ "$DRY_RUN" -eq 1 ]; then
    printf '[dry-run] %s\n' "$*"
  else
    eval "$@"
  fi
}

if [ "$DRY_RUN" -eq 0 ]; then
  command -v qm >/dev/null 2>&1 || { echo "qm not found; run on Proxmox VE 8/9 host." >&2; exit 1; }
  if [ "$PCI_BDF" = "auto" ] || [ ! -d "/sys/bus/pci/devices/${PCI_BDF}" ]; then
    DETECTED_BDF="$(lspci -D -d 10de:1bb3 2>/dev/null | awk 'NR==1{print $1}')"
    if [ -n "$DETECTED_BDF" ]; then
      echo "Auto-detected Tesla P4 PCI BDF on Proxmox VE host: ${DETECTED_BDF}"
      PCI_BDF="$DETECTED_BDF"
    fi
  fi
  if [ "$MODE" = "import-qcow2" ]; then
    test -f "$ALMA_DISK" || { echo "Missing --alma-disk: $ALMA_DISK" >&2; exit 1; }
    test -f "$WIN11_DISK" || { echo "Missing --win11-disk: $WIN11_DISK" >&2; exit 1; }
  fi
fi

echo "=== Proxmox VE 50/50 Tesla P4 vGPU Provisioning (mode=${MODE}, pci=${PCI_BDF}, mdev=${MDEV_TYPE}) ==="

# --- VM 101: AlmaLinux 10.3 Wayland + RDP + Selkies ---
run_qm "qm create ${ALMA_VMID} --name vm-alma --machine q35 --bios ovmf --cpu host --cores 4 --sockets 1 --memory 5120 --ostype l26 --vga std --scsihw virtio-scsi-single --net0 virtio=52:54:00:11:22:03,bridge=${BRIDGE} --efidisk0 ${STORAGE}:1,efitype=4m,pre-enrolled-keys=0 --hostpci0 ${PCI_BDF},mdev=${MDEV_TYPE},pcie=1 --onboot 1"

if [ "$MODE" = "import-qcow2" ]; then
  run_qm "qm importdisk ${ALMA_VMID} '${ALMA_DISK}' ${STORAGE}"
  run_qm "qm set ${ALMA_VMID} --virtio0 ${STORAGE}:vm-${ALMA_VMID}-disk-1,discard=on --boot order=virtio0"
else
  run_qm "qm set ${ALMA_VMID} --virtio0 ${STORAGE}:25,discard=on --boot order=virtio0"
fi

# --- VM 102: Windows 11 24H2 WDDM 3.1 + RDP ---
run_qm "qm create ${WIN11_VMID} --name vm-win11 --machine q35 --bios ovmf --cpu host,flags=+hv-evmcs --cores 4 --sockets 1 --memory 6144 --ostype win11 --vga std --net0 e1000e=52:54:00:11:22:05,bridge=${BRIDGE} --efidisk0 ${STORAGE}:1,efitype=4m,pre-enrolled-keys=1 --tpmstate0 ${STORAGE}:1,version=v2.0 --hostpci0 ${PCI_BDF},mdev=${MDEV_TYPE},pcie=1 --onboot 1"

if [ "$MODE" = "import-qcow2" ]; then
  run_qm "qm importdisk ${WIN11_VMID} '${WIN11_DISK}' ${STORAGE}"
  run_qm "qm set ${WIN11_VMID} --sata0 ${STORAGE}:vm-${WIN11_VMID}-disk-1 --boot order=sata0"
else
  run_qm "qm set ${WIN11_VMID} --sata0 ${STORAGE}:64 --boot order=sata0"
fi

echo "=== Proxmox VMs configured (${ALMA_VMID}=vm-alma, ${WIN11_VMID}=vm-win11) ==="
