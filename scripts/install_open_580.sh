#!/usr/bin/env bash
set -euo pipefail

# Unified Host & Guest Installer for open-580-driver (NVIDIA 580.178.04 Open-Shim Bare-Metal / Passthrough Stack)
# Mirrors open-580-driver/scripts/install_open_580.sh for convenience when switching between vGPU and bare-metal modes.

ROLE="host"
RUN_FILE=""
DRIVER_DIR=""
OPEN_580_REPO="${OPEN_580_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../open-580-driver" 2>/dev/null && pwd || true)}"
DRY_RUN=0
PASS_ARGS=()

for arg in "$@"; do
  if [[ "$arg" == "--dry-run" ]]; then
    DRY_RUN=1
  fi
  PASS_ARGS+=("$arg")
done

if [[ -n "$OPEN_580_REPO" && -f "$OPEN_580_REPO/scripts/install_open_580.sh" ]]; then
  exec bash "$OPEN_580_REPO/scripts/install_open_580.sh" "${PASS_ARGS[@]}"
fi

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "[dry-run] Delegate to open-580-driver/scripts/install_open_580.sh ${PASS_ARGS[*]}"
  exit 0
fi

echo "ERROR: Companion open-580-driver repository not found at '${OPEN_580_REPO}'. Set OPEN_580_REPO=/path/to/open-580-driver." >&2
exit 1
