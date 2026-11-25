#!/usr/bin/env bash
set -euo pipefail

# Unified Host & Guest Installer for open-vgpu-tesla-p4 (R580: 580.196.09 Host + 580.196.07 Linux + 582.99 Windows)
# Supports:
#   --role host           Ubuntu 24.04/26.04 KVM or Proxmox VE 8/9 host:
#                         - DKMS nvidia-vgpu-kvm/580.196.09 (0xc85b7 bypass + 0x11D8 vgpuConfig.xml + open MIT nvidia-vgpu-vfio.ko)
#                         - All clean-room open-source host artifacts in /opt/nvidia-vgpu-580
#                         - Optional FastAPI-DLS (--with-dls docker|systemd)
#   --role guest-linux    AlmaLinux 10.3 / RHEL 10 / Ubuntu / Debian vGPU guest (580.196.07 + nv-pci.c patch + X11 strip + CDI)
#   --role guest-windows  Windows 11 24H2 WDDM 3.1 (582.99 WHQL + nvxdapix.dll patch + RDP 0x200d fix) commands

ROLE=""
RUN_FILE=""
GRID_580_RUN=""
GRIDD_PATCHER=""
DLS_MODE=""
DLS_URL="https://192.168.1.24"
RDP_USER="zero"
RDP_PASS="${RDP_PASS:-}"
KEEP_X11=0
WITH_SELKIES=0
WITH_OPEN_GRIDD=0
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: install_open_vgpu.sh --role <host|guest-linux|guest-windows> [options]

Roles:
  --role host             Install DKMS 580.196.09-vgpu-kvm + clean-room open-source host daemons on Ubuntu KVM or Proxmox VE
  --role guest-linux      Provision R580 Linux vGPU guest (580.196.07 + nv-pci.c patch + pure Wayland X11 strip + DLS + Selkies)
  --role guest-windows    Display Windows 11 24H2 vGPU (582.99 WHQL) provisioning commands

Host Options (--role host):
  --run-file <path>       Path to NVIDIA-Linux-x86_64-580.196.09-vgpu-kvm.run
  --with-dls <mode>       Also install FastAPI-DLS license server ('docker' for Ubuntu KVM, 'systemd' for Proxmox VE)
  --dls-url <ip_or_url>   FastAPI-DLS IP/URL (default: https://192.168.1.24)

Linux Guest Options (--role guest-linux):
  --grid-580-run <path>   Path to NVIDIA-Linux-x86_64-580.196.07-grid.run
  --gridd-patcher <path>  Path to compiled gridd-unlock-patcher binary (patches /usr/bin/nvidia-gridd)
  --dls-url <url>         FastAPI-DLS URL (default: https://192.168.1.24)
  --rdp-user <user>       Wayland RDP username (default: zero)
  --rdp-pass <pass>       Wayland RDP password (or set RDP_PASS env var)
  --keep-x11              Keep NVIDIA X11/Xorg/nvidia-settings files instead of stripping
  --with-open-gridd       Also build and install clean-room open-gridd
  --with-selkies          Also install Docker CE + NVIDIA CDI + Selkies PixelFlux 10-bit 4:4:4

Common Options:
  --dry-run               Validate and print planned actions without modifying the system
  -h, --help              Show this help message
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --role)            ROLE="${2:-}"; shift 2 ;;
    --run-file)        RUN_FILE="${2:-}"; shift 2 ;;
    --grid-580-run)    GRID_580_RUN="${2:-}"; shift 2 ;;
    --gridd-patcher)   GRIDD_PATCHER="${2:-}"; shift 2 ;;
    --with-dls)        DLS_MODE="${2:-}"; shift 2 ;;
    --dls-url)         DLS_URL="${2:-}"; shift 2 ;;
    --rdp-user)        RDP_USER="${2:-}"; shift 2 ;;
    --rdp-pass)        RDP_PASS="${2:-}"; shift 2 ;;
    --keep-x11)        KEEP_X11=1; shift ;;
    --with-open-gridd) WITH_OPEN_GRIDD=1; shift ;;
    --with-selkies)    WITH_SELKIES=1; shift ;;
    --dry-run)         DRY_RUN=1; shift ;;
    -h|--help)         usage; exit 0 ;;
    *)                 echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
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
    [[ -n "$GRID_580_RUN" ]] && GUEST_ARGS+=(--grid-580-run "$GRID_580_RUN")
    [[ -n "$GRIDD_PATCHER" ]] && GUEST_ARGS+=(--gridd-patcher "$GRIDD_PATCHER")
    [[ -n "$RDP_PASS" ]] && GUEST_ARGS+=(--rdp-pass "$RDP_PASS")
    [[ "$KEEP_X11" -eq 1 ]] && GUEST_ARGS+=(--keep-x11)
    [[ "$WITH_OPEN_GRIDD" -eq 1 ]] && GUEST_ARGS+=(--with-open-gridd)
    [[ "$WITH_SELKIES" -eq 1 ]] && GUEST_ARGS+=(--with-selkies)
    [[ "$DRY_RUN" -eq 1 ]] && GUEST_ARGS+=(--dry-run)
    bash "$SCRIPT_DIR/provision_linux_vgpu_guest.sh" "${GUEST_ARGS[@]}"
    ;;

  guest-windows)
    echo "=== Windows 11 24H2 vGPU Guest Provisioning (582.99 WHQL, SUBSYS_11D810DE) ==="
    echo "Run inside an elevated PowerShell prompt on the Windows 11 VM:"
    echo "  powershell -ExecutionPolicy Bypass -File scripts\\provision_win11_guest.ps1 \\"
    echo "    -DlsUrl ${DLS_URL} \\"
    echo "    -DriverInstallerPath C:\\path\\to\\582.99_grid_win10_win11_server2022_server_2025_dch_64bit_international.exe \\"
    echo "    -PatchedNvxdapixPath C:\\path\\to\\nvxdapix_patched.dll"
    echo "For unattended ISO installation, use: scripts/templates/win11-autounattend.xml"
    ;;

  *)
    echo "ERROR: Specify --role <host|guest-linux|guest-windows>." >&2
    usage >&2
    exit 2
    ;;
esac
