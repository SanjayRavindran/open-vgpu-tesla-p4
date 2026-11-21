#!/usr/bin/env bash
set -euo pipefail

# Reproducible AlmaLinux 10.3 guest provisioning for Tesla P4 GRID P4-4Q:
#   - 535.309.01 GRID kernel module + 620.70.06 Wayland/GBM userspace libraries
#   - FastAPI-DLS client token installation (/etc/nvidia/ClientConfigToken/)
#   - Pure Wayland GNOME (--no-x11 --headless) + direct headless RDP (:3389)
#   - GTK4 GSK_RENDERER=ngl override (avoids Vulkan producer segfault)
#   - Optional Selkies PixelFlux 10-bit 4:4:4 container stack

DLS_URL="https://192.168.1.24"
GRID_535_RUN=""
GRID_620_RUN=""
RDP_USER="zero"
RDP_PASS="${RDP_PASS:-}"
WITH_SELKIES=0
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: provision_alma_guest.sh [options]

Options:
  --dls-url <url>        FastAPI-DLS base URL (default: https://192.168.1.24)
  --grid-535-run <path>  Path to NVIDIA-Linux-x86_64-535.309.01-grid.run
  --grid-620-run <path>  Path to NVIDIA-Linux-x86_64-620.70.06-grid.run (optional Wayland overlay)
  --rdp-user <user>      Desktop / RDP username (default: zero)
  --rdp-pass <pass>      RDP password (or set RDP_PASS env var; required unless --dry-run)
  --with-selkies         Also configure Docker CE + NVIDIA CDI + Selkies 10-bit 4:4:4
  --dry-run              Print planned steps without modifying the system
  -h, --help             Show help
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --dls-url) DLS_URL="${2:-}"; shift 2 ;;
    --grid-535-run) GRID_535_RUN="${2:-}"; shift 2 ;;
    --grid-620-run) GRID_620_RUN="${2:-}"; shift 2 ;;
    --rdp-user) RDP_USER="${2:-}"; shift 2 ;;
    --rdp-pass) RDP_PASS="${2:-}"; shift 2 ;;
    --with-selkies) WITH_SELKIES=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ "$DRY_RUN" -eq 1 ]; then
  echo "[dry-run] AlmaLinux 10.3 guest provisioning (DLS_URL=${DLS_URL}, user=${RDP_USER}, selkies=${WITH_SELKIES})"
  echo "[dry-run] 1. Install kernel-devel, gnome-remote-desktop, freerdp, openssl, curl, mesa-libEGL, libwayland-*"
  echo "[dry-run] 2. Install 535.309.01 GRID driver + overlay 620.70.06 Wayland/GBM libs (libnvidia-egl-gbm/wayland/wayland2, nvidia-drm_gbm.so)"
  echo "[dry-run] 3. Configure /etc/modprobe.d/nvidia-drm.conf (options nvidia-drm modeset=1)"
  echo "[dry-run] 4. Apply GTK4 GSK_RENDERER=ngl in /etc/environment, /etc/profile.d/gsk-renderer.sh, /etc/systemd/user.conf.d/gsk-renderer.conf"
  echo "[dry-run] 5. Configure pure Wayland GNOME (--no-x11 --headless) + direct headless RDP :3389 (no --system Handover)"
  echo "[dry-run] 6. Patch GRD CUDA PTX (patch_grd_ptx_535.sh) + fetch DLS token from ${DLS_URL}/-/client-token"
  if [ "$WITH_SELKIES" -eq 1 ]; then
    echo "[dry-run] 7. Run setup_selkies_pixelflux_vgpu.sh (Docker CE + CDI + el10 .note.gnu.property fix)"
  fi
  exit 0
fi

if [ "$(id -u)" -ne 0 ]; then
  echo "Run as root (sudo)." >&2
  exit 1
fi
if [ -z "$RDP_PASS" ]; then
  echo "Provide --rdp-pass <password> or RDP_PASS env var (never hardcoded)." >&2
  exit 2
fi

dnf install -y -q kernel-devel kernel-headers gcc make elfutils-libelf-devel curl openssl gnome-remote-desktop freerdp

cat > /etc/modprobe.d/nvidia-drm.conf <<'EOF'
options nvidia-drm modeset=1
EOF

if [ -n "$GRID_535_RUN" ] && [ -f "$GRID_535_RUN" ]; then
  bash "$GRID_535_RUN" --silent --dkms --no-questions --accept-license
fi

if [ -n "$GRID_620_RUN" ] && [ -f "$GRID_620_RUN" ]; then
  TMP_R620="$(mktemp -d /tmp/grid620.XXXXXX)"
  trap 'rm -rf "$TMP_R620"' EXIT
  bash "$GRID_620_RUN" --extract-only --target "$TMP_R620/pkg" >/dev/null
  for lib in libnvidia-egl-gbm.so.* libnvidia-egl-wayland.so.* libnvidia-egl-wayland2.so.*; do
    for f in "$TMP_R620/pkg/$lib"; do
      [ -f "$f" ] && install -m 0755 "$f" /usr/lib64/
    done
  done
  mkdir -p /usr/lib64/gbm /usr/share/egl/egl_external_platform.d
  if [ -f /usr/lib64/libnvidia-allocator.so.1 ]; then
    ln -sf ../libnvidia-allocator.so.1 /usr/lib64/gbm/nvidia-drm_gbm.so
  fi
  for js in 09_nvidia_wayland2.json 10_nvidia_wayland.json 15_nvidia_gbm.json; do
    [ -f "$TMP_R620/pkg/$js" ] && install -m 0644 "$TMP_R620/pkg/$js" /usr/share/egl/egl_external_platform.d/
  done
  ldconfig
fi

# GTK4 GSK_RENDERER=ngl (avoids R535 vulkan-producer segfaults)
grep -qxF 'GSK_RENDERER=ngl' /etc/environment 2>/dev/null || echo 'GSK_RENDERER=ngl' >> /etc/environment
cat > /etc/profile.d/gsk-renderer.sh <<'EOF'
export GSK_RENDERER=ngl
EOF
mkdir -p /etc/systemd/user.conf.d
cat > /etc/systemd/user.conf.d/gsk-renderer.conf <<'EOF'
[Manager]
DefaultEnvironment="GSK_RENDERER=ngl"
EOF

# Pure Wayland headless GNOME Shell override
mkdir -p /etc/systemd/user/org.gnome.Shell@wayland.service.d
cat > /etc/systemd/user/org.gnome.Shell@wayland.service.d/override.conf <<'EOF'
[Service]
Environment="GSK_RENDERER=ngl"
ExecStart=
ExecStart=/usr/bin/gnome-shell --no-x11 --headless
EOF

# Enable GDM auto-login for target desktop user
if [ -f /etc/gdm/custom.conf ]; then
  python3 - "$RDP_USER" <<'PY'
import pathlib, re, sys
user = sys.argv[1]
p = pathlib.Path('/etc/gdm/custom.conf')
txt = p.read_text()
if '[daemon]' not in txt:
    txt += '\n[daemon]\n'
for k, v in [('AutomaticLoginEnable', 'True'), ('AutomaticLogin', user)]:
    if re.search(rf'^{k}=', txt, re.M):
        txt = re.sub(rf'^{k}=.*', f'{k}={v}', txt, flags=re.M)
    else:
        txt = txt.replace('[daemon]\n', f'[daemon]\n{k}={v}\n', 1)
p.write_text(txt)
PY
fi

# Patch GRD PTX header for R535 CUDA 12.2 if script & daemon exist
if [ -f "$SCRIPT_DIR/patch_grd_ptx_535.sh" ] && [ -f /usr/libexec/gnome-remote-desktop-daemon ]; then
  bash "$SCRIPT_DIR/patch_grd_ptx_535.sh" || true
fi

# Direct headless RDP (:3389), avoiding --system Handover 0x8009030f bug
systemctl disable --now gnome-remote-desktop.service 2>/dev/null || true
loginctl enable-linger "$RDP_USER"
USER_HOME="$(getent passwd "$RDP_USER" | cut -d: -f6)"
TLS_DIR="${USER_HOME}/.local/share/gnome-remote-desktop/tls"
sudo -u "$RDP_USER" mkdir -p "$TLS_DIR"
if [ ! -f "${TLS_DIR}/tls.crt" ]; then
  sudo -u "$RDP_USER" openssl req -x509 -nodes -days 3650 -newkey rsa:2048 \
    -keyout "${TLS_DIR}/tls.key" -out "${TLS_DIR}/tls.crt" -subj "/CN=$(hostname)"
fi

USER_UID="$(id -u "$RDP_USER")"
sudo -u "$RDP_USER" XDG_RUNTIME_DIR="/run/user/${USER_UID}" bash -c "
  grdctl --headless rdp set-port 3389
  grdctl --headless rdp set-tls-cert '${TLS_DIR}/tls.crt'
  grdctl --headless rdp set-tls-key '${TLS_DIR}/tls.key'
  grdctl --headless rdp set-credentials '${RDP_USER}' '${RDP_PASS}'
  grdctl --headless rdp disable-view-only || true
  grdctl --headless rdp enable
"
firewall-cmd --permanent --add-port=3389/tcp 2>/dev/null || true
firewall-cmd --reload 2>/dev/null || true

# Fetch FastAPI-DLS license token
mkdir -p /etc/nvidia/ClientConfigToken
rm -f /etc/nvidia/ClientConfigToken/*.tok
curl --insecure -fsSL -X GET "${DLS_URL}/-/client-token" \
  -o "/etc/nvidia/ClientConfigToken/client_configuration_token_$(date +%d-%m-%Y-%H-%M-%S).tok"
chmod 0644 /etc/nvidia/ClientConfigToken/*.tok
systemctl restart nvidia-gridd 2>/dev/null || true

if [ "$WITH_SELKIES" -eq 1 ] && [ -f "$SCRIPT_DIR/setup_selkies_pixelflux_vgpu.sh" ]; then
  bash "$SCRIPT_DIR/setup_selkies_pixelflux_vgpu.sh"
fi

echo "=== AlmaLinux 10.3 guest provisioning complete; reboot to start headless Wayland RDP ==="
