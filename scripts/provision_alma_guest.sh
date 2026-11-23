#!/usr/bin/env bash
set -euo pipefail

# Backward-compatible wrapper around provision_linux_vgpu_guest.sh for AlmaLinux / RHEL / Enterprise Linux guests.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec bash "$SCRIPT_DIR/provision_linux_vgpu_guest.sh" "$@"
