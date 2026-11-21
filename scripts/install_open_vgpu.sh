#!/usr/bin/env bash
set -euo pipefail

# Unified Host & Guest Installer for open-vgpu-tesla-p4 (535.309.01-vgpu-kvm + Open Host Daemons & Hybrid Guests)
# Supports:
#   --role host           Ubuntu 24.04/26.04 KVM or Proxmox VE 8/9 host:
#                         - DKMS nvidia-vgpu-kvm/535.309.01 (nvidia.ko + open MIT nvidia-vgpu-vfio.ko)
#                         - All 6 clean-room open-source host binaries:
#                             1. open-nvidia-vgpud (Rust)
#                             2. libopen-vgxcfg.so (C)
#                             3. open-nvidia-xid-logd (C)
#                             4. open-nvidia-vgpu-mgr (C)
#                             5. open-nvidia-smi (C)
#                             6. libopen-nvidia-ml.so (C)
#                         - Optional FastAPI-DLS (--with-dls docker|systemd)
#   --role guest-linux    AlmaLinux 10.3 / RHEL 10 / Ubuntu Linux vGPU guest:
#                         - 535.309.01 GRID guest driver (with Linux 6.12+/7.0+ compat patcher)
#                         - 620.70.06 Wayland/GBM userspace overlay (Pure Wayland --no-x11 --headless)
#                         - GTK4 GSK_RENDERER=ngl + GNOME Remote Desktop CUDA PTX 8.0 patch (:3389)
#                         - FastAPI-DLS client token + optional Selkies PixelFlux 10-bit 4:4:4 (--with-selkies)
#   --role guest-windows  Prints or executes the Windows 11 24H2 WDDM 3.1 (539.72 WHQL) + RDP 0x200d fix provisioning workflow

ROLE=""
RUN_FILE=""
GRID_535_RUN=""
GRID_620_RUN=""
DLS_MODE=""
DLS_URL="https://192.168.1.24"
RDP_USER="zero"
RDP_PASS="${RDP_PASS:-}"
WITH_SELKIES=0
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: install_open_vgpu.sh --role <host|guest-linux|guest-windows> [options]

Roles:
  --role host             Install DKMS 535.309.01-vgpu-kvm + all 6 open-source host daemons on Ubuntu KVM or Proxmox VE
  --role guest-linux      Provision Linux vGPU guest (535.309.01 kernel + R620 Wayland/GBM + headless RDP + DLS + optional Selkies)
  --role guest-windows    Display/run Windows 11 24H2 vGPU WDDM 3.1 (539.72 WHQL) provisioning commands

Host Options (--role host):
  --run-file <path>       Path to NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run
  --with-dls <mode>       Also install FastAPI-DLS license server ('docker' for Ubuntu KVM, 'systemd' for Proxmox VE)
  --dls-url <ip_or_url>   FastAPI-DLS IP/URL (default: https://192.168.1.24)

Linux Guest Options (--role guest-linux):
  --grid-535-run <path>   Path to NVIDIA-Linux-x86_64-535.309.01-grid.run
  --grid-620-run <path>   Path to NVIDIA-Linux-x86_64-620.70.06-grid.run (Wayland/GBM overlay)
  --dls-url <url>         FastAPI-DLS URL (default: https://192.168.1.24)
  --rdp-user <user>       Wayland RDP username (default: zero)
  --rdp-pass <pass>       Wayland RDP password (or set RDP_PASS env var)
  --with-selkies          Also install Docker CE + NVIDIA CDI + Selkies PixelFlux 10-bit 4:4:4

Common Options:
  --dry-run               Validate and print planned actions without modifying the system
  -h, --help              Show this help message
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --role)          ROLE="${2:-}"; shift 2 ;;
    --run-file)      RUN_FILE="${2:-}"; shift 2 ;;
    --grid-535-run)  GRID_535_RUN="${2:-}"; shift 2 ;;
    --grid-620-run)  GRID_620_RUN="${2:-}"; shift 2 ;;
    --with-dls)      DLS_MODE="${2:-}"; shift 2 ;;
    --dls-url)       DLS_URL="${2:-}"; shift 2 ;;
    --rdp-user)      RDP_USER="${2:-}"; shift 2 ;;
    --rdp-pass)      RDP_PASS="${2:-}"; shift 2 ;;
    --with-selkies)  WITH_SELKIES=1; shift ;;
    --dry-run)       DRY_RUN=1; shift ;;
    -h|--help)       usage; exit 0 ;;
    *)               echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

case "$ROLE" in
  host)
    HOST_ARGS=()
    [[ -n "$RUN_FILE" ]] && HOST_ARGS+=(--run-file "$RUN_FILE")
    [[ "$DRY_RUN" -eq 1 ]] && HOST_ARGS+=(--dry-run)
    bash "$SCRIPT_DIR/install_vgpu_host_dkms.sh" "${HOST_ARGS[@]}"
    if [[ -n "$DLS_MODE" ]]; then
      DLS_HOST="${DLS_URL#https://}"
      DLS_HOST="${DLS_HOST#http://}"
      DLS_HOST="${DLS_HOST%%:*}"
      DLS_ARGS=(--mode "$DLS_MODE" --dls-url "$DLS_HOST" --port 443)
      [[ "$DRY_RUN" -eq 1 ]] && DLS_ARGS+=(--dry-run)
      bash "$SCRIPT_DIR/install_fastapi_dls.sh" "${DLS_ARGS[@]}"
    fi
    ;;

  guest-linux)
    GUEST_ARGS=(--dls-url "$DLS_URL" --rdp-user "$RDP_USER")
    [[ -n "$GRID_535_RUN" ]] && GUEST_ARGS+=(--grid-535-run "$GRID_535_RUN")
    [[ -n "$GRID_620_RUN" ]] && GUEST_ARGS+=(--grid-620-run "$GRID_620_RUN")
    [[ -n "$RDP_PASS" ]] && GUEST_ARGS+=(--rdp-pass "$RDP_PASS")
    [[ "$WITH_SELKIES" -eq 1 ]] && GUEST_ARGS+=(--with-selkies)
    [[ "$DRY_RUN" -eq 1 ]] && GUEST_ARGS+=(--dry-run)
    bash "$SCRIPT_DIR/provision_alma_guest.sh" "${GUEST_ARGS[@]}"
    ;;

  guest-windows)
    echo "=== Windows 11 24H2 vGPU Guest Provisioning (539.72 WHQL WDDM 3.1) ==="
    echo "Run inside an elevated PowerShell prompt on the Windows 11 VM:"
    echo "  powershell -ExecutionPolicy Bypass -File scripts\\provision_win11_guest.ps1 -DlsUrl ${DLS_URL}"
    echo "For unattended ISO installation, use: scripts/templates/win11-autounattend.xml"
    ;;

  *)
    echo "ERROR: Specify --role <host|guest-linux|guest-windows>." >&2
    usage >&2
    exit 2
    ;;
esac
