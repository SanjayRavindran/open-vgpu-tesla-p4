#!/usr/bin/env bash
set -euo pipefail

# Universal Multi-Distro Linux vGPU Guest Provisioner for Tesla P4 (GRID P4-4Q / nvidia-65)
# Supports:
#   - Red Hat Enterprise Linux (RHEL 8 / 9 / 10)
#   - AlmaLinux OS (8 / 9 / 10, including AlmaLinux 10.3 x86-64-v2 Xeon E3-1275 V2 ISA handling)
#   - Rocky Linux, Oracle Linux, CentOS Stream, Fedora (Enterprise Linux family)
#   - Ubuntu LTS Releases (20.04, 22.04, 24.04, 26.04 LTS)
#   - Debian Stable / Testing (Debian 11 Bullseye, 12 Bookworm, 13 Trixie)
#
# Configures:
#   1. Distro-specific kernel headers, DKMS/build tools, Wayland/EGL & GNOME Remote Desktop packages
#   2. 535.309.01 GRID guest driver (with automatic Linux 6.12+/7.x+ DRM/strncpy compat patching)
#   3. 620.70.06 (R620) Wayland/GBM userspace overlay into distro library paths (/usr/lib64 or /usr/lib/x86_64-linux-gnu)
#   4. GTK4 GSK_RENDERER=ngl + Pure Wayland GNOME (--no-x11 --headless) + direct headless RDP (:3389) + GRD CUDA PTX 8.0 patch
#   5. FastAPI-DLS client configuration token (/etc/nvidia/ClientConfigToken/*.tok) + nvidia-gridd
#   6. Clean-room libopen-nvidia-encode.so (open-nvenc), optional open guest kernel modules, and optional Selkies PixelFlux 10-bit 4:4:4

DLS_URL="https://192.168.1.24"
GRID_535_RUN=""
GRID_620_RUN=""
RDP_USER="zero"
RDP_PASS="${RDP_PASS:-}"
DISTRO_OVERRIDE=""
WITH_SELKIES=0
WITH_OPEN_GUEST_KMOD=0
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: provision_linux_vgpu_guest.sh [options]

Supported Guest Distributions (auto-detected from /etc/os-release):
  - Enterprise Linux: RHEL, AlmaLinux, Rocky Linux, Oracle Linux, CentOS Stream, Fedora
  - Ubuntu LTS:       Ubuntu 20.04, 22.04, 24.04, 26.04 LTS
  - Debian:           Debian 11 (Bullseye), 12 (Bookworm), 13 (Trixie)

Options:
  --dls-url <url>          FastAPI-DLS base URL (default: https://192.168.1.24)
  --grid-535-run <path>    Path to NVIDIA-Linux-x86_64-535.309.01-grid.run
  --grid-620-run <path>    Path to NVIDIA-Linux-x86_64-620.70.06-grid.run (optional Wayland/GBM overlay)
  --rdp-user <user>        Desktop / RDP username (default: zero)
  --rdp-pass <pass>        RDP password (or set RDP_PASS env var; required unless --dry-run)
  --distro <id>            Override detected distro ID (e.g. rhel, almalinux, rocky, ol, centos, ubuntu, debian)
  --with-selkies           Also configure Docker + NVIDIA Container Toolkit (CDI) + Selkies PixelFlux 10-bit 4:4:4
  --with-open-guest-kmod   Also compile open-gpu-kernel-modules@535.309.01 GP104 BAR0 VRPC guest modules & libopen-nvidia-encode.so
  --dry-run                Print planned distro-specific steps without modifying the system
  -h, --help               Show this help text
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --dls-url) DLS_URL="${2:-}"; shift 2 ;;
    --grid-535-run) GRID_535_RUN="${2:-}"; shift 2 ;;
    --grid-620-run) GRID_620_RUN="${2:-}"; shift 2 ;;
    --rdp-user) RDP_USER="${2:-}"; shift 2 ;;
    --rdp-pass) RDP_PASS="${2:-}"; shift 2 ;;
    --distro) DISTRO_OVERRIDE="${2:-}"; shift 2 ;;
    --with-selkies) WITH_SELKIES=1; shift ;;
    --with-open-guest-kmod) WITH_OPEN_GUEST_KMOD=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
KVER="$(uname -r)"

# Detect distribution family from /etc/os-release (or --distro override)
OS_ID="almalinux"
OS_ID_LIKE="rhel centos fedora"
OS_VERSION_ID="10"
if [ -f /etc/os-release ]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  OS_ID="${ID:-almalinux}"
  OS_ID_LIKE="${ID_LIKE:-}"
  OS_VERSION_ID="${VERSION_ID:-}"
fi
if [ -n "$DISTRO_OVERRIDE" ]; then
  OS_ID="$(echo "$DISTRO_OVERRIDE" | tr '[:upper:]' '[:lower:]')"
fi

DISTRO_FAMILY="el"
PKG_MGR="dnf"
PRIMARY_LIBDIR="/usr/lib64"
GDM_CONF_CANDIDATES=("/etc/gdm/custom.conf")
INITRAMFS_CMD="dracut -f"

case "$OS_ID" in
  ubuntu|debian|linuxmint|pop)
    DISTRO_FAMILY="debian"
    PKG_MGR="apt-get"
    PRIMARY_LIBDIR="/usr/lib/x86_64-linux-gnu"
    GDM_CONF_CANDIDATES=("/etc/gdm3/custom.conf" "/etc/gdm3/daemon.conf" "/etc/gdm/custom.conf")
    INITRAMFS_CMD="update-initramfs -u"
    ;;
  rhel|almalinux|alma|rocky|ol|oracle|centos|fedora|eurolinux|miraclelinux)
    DISTRO_FAMILY="el"
    if command -v dnf >/dev/null 2>&1; then
      PKG_MGR="dnf"
    else
      PKG_MGR="yum"
    fi
    PRIMARY_LIBDIR="/usr/lib64"
    GDM_CONF_CANDIDATES=("/etc/gdm/custom.conf")
    INITRAMFS_CMD="dracut -f"
    ;;
  *)
    if echo "$OS_ID_LIKE" | grep -qiE "debian|ubuntu"; then
      DISTRO_FAMILY="debian"
      PKG_MGR="apt-get"
      PRIMARY_LIBDIR="/usr/lib/x86_64-linux-gnu"
      GDM_CONF_CANDIDATES=("/etc/gdm3/custom.conf" "/etc/gdm3/daemon.conf" "/etc/gdm/custom.conf")
      INITRAMFS_CMD="update-initramfs -u"
    fi
    ;;
esac

if [ "$DRY_RUN" -eq 1 ]; then
  echo "[dry-run] Linux vGPU guest provisioning (distro=${OS_ID}, family=${DISTRO_FAMILY}, pkg=${PKG_MGR}, libdir=${PRIMARY_LIBDIR}, DLS_URL=${DLS_URL}, user=${RDP_USER}, selkies=${WITH_SELKIES})"
  if [ "$DISTRO_FAMILY" = "debian" ]; then
    echo "[dry-run] 1. apt-get install linux-headers-${KVER} (or linux-headers-generic/amd64), build-essential, dkms, gnome-remote-desktop, freerdp3-x11/freerdp2-x11, libegl1, libwayland-*"
  else
    echo "[dry-run] 1. ${PKG_MGR} install kernel-devel-${KVER} (or kernel-devel), kernel-headers, gcc, make, elfutils-libelf-devel, gnome-remote-desktop, freerdp, mesa-libEGL, libwayland-*"
  fi
  echo "[dry-run] 2. Install 535.309.01 GRID driver (with Linux 6.12+/7.x+ compat patcher if needed) + overlay 620.70.06 Wayland/GBM libs into ${PRIMARY_LIBDIR} and ${PRIMARY_LIBDIR}/gbm"
  echo "[dry-run] 3. Configure /etc/modprobe.d/nvidia-drm.conf (options nvidia-drm modeset=1) and run '${INITRAMFS_CMD}'"
  echo "[dry-run] 4. Apply GTK4 GSK_RENDERER=ngl in /etc/environment, /etc/profile.d/gsk-renderer.sh, /etc/systemd/user.conf.d/gsk-renderer.conf"
  echo "[dry-run] 5. Configure pure Wayland GNOME (--no-x11 --headless), GDM auto-login (${GDM_CONF_CANDIDATES[0]}), and direct headless RDP :3389"
  echo "[dry-run] 6. Patch GRD CUDA PTX (patch_grd_ptx_535.sh) + fetch DLS token from ${DLS_URL}/-/client-token"
  if [ "$WITH_SELKIES" -eq 1 ]; then
    echo "[dry-run] 7. Run setup_selkies_pixelflux_vgpu.sh (Docker + NVIDIA CDI + Selkies PixelFlux 10-bit 4:4:4)"
  fi
  if [ "$WITH_OPEN_GUEST_KMOD" -eq 1 ]; then
    echo "[dry-run] 8. Build open-gpu-kernel-modules@535.309.01 GP104 BAR0 VRPC guest modules & libopen-nvidia-encode.so"
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

echo "=== Provisioning Linux vGPU Guest (distro=${OS_ID} ${OS_VERSION_ID}, family=${DISTRO_FAMILY}, kernel=${KVER}) ==="

# 1. Install distro-specific kernel headers, compiler toolchain, Wayland & GNOME Remote Desktop packages
if [ "$DISTRO_FAMILY" = "debian" ]; then
  DEBIAN_FRONTEND=noninteractive apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    "linux-headers-${KVER}" build-essential dkms pkg-config libglvnd-dev curl openssl ca-certificates \
    gnome-remote-desktop libegl1 libwayland-client0 libwayland-server0 libwayland-egl1 || \
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    linux-headers-generic build-essential dkms pkg-config libglvnd-dev curl openssl ca-certificates \
    gnome-remote-desktop libegl1 libwayland-client0 libwayland-server0 libwayland-egl1 || \
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    linux-headers-amd64 build-essential dkms pkg-config libglvnd-dev curl openssl ca-certificates \
    gnome-remote-desktop libegl1 libwayland-client0 libwayland-server0 libwayland-egl1
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq freerdp3-x11 2>/dev/null || \
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq freerdp2-x11 2>/dev/null || true
else
  "$PKG_MGR" install -y -q \
    "kernel-devel-${KVER}" kernel-headers gcc make elfutils-libelf-devel curl openssl \
    gnome-remote-desktop freerdp mesa-libEGL libwayland-client libwayland-server libwayland-egl || \
  "$PKG_MGR" install -y -q \
    kernel-devel kernel-headers gcc make elfutils-libelf-devel curl openssl \
    gnome-remote-desktop freerdp mesa-libEGL libwayland-client libwayland-server libwayland-egl
  "$PKG_MGR" install -y -q epel-release 2>/dev/null || true
  "$PKG_MGR" install -y -q dkms egl-wayland 2>/dev/null || true
fi

# 2. Blacklist nouveau inside guest and enable nvidia-drm modeset=1
cat > /etc/modprobe.d/blacklist-nouveau.conf <<'EOF'
blacklist nouveau
options nouveau modeset=0
EOF

cat > /etc/modprobe.d/nvidia-drm.conf <<'EOF'
options nvidia-drm modeset=1
EOF

# 3. Install 535.309.01 GRID guest driver (auto-applying Linux 7.x+ compat patcher if kernel >= 7.0)
if [ -n "$GRID_535_RUN" ] && [ -f "$GRID_535_RUN" ]; then
  KMAJ="$(echo "$KVER" | cut -d. -f1)"
  if [ "${KMAJ:-6}" -ge 7 ] && [ -f "$SCRIPT_DIR/patch_535_guest_linux7.py" ]; then
    TMP_G535="$(mktemp -d /tmp/grid535.XXXXXX)"
    bash "$GRID_535_RUN" --extract-only --target "$TMP_G535/pkg" >/dev/null
    python3 "$SCRIPT_DIR/patch_535_guest_linux7.py" "$TMP_G535/pkg/kernel" || true
    (cd "$TMP_G535/pkg" && ./nvidia-installer --silent --dkms --no-questions --accept-license)
    rm -rf "$TMP_G535"
  else
    bash "$GRID_535_RUN" --silent --dkms --no-questions --accept-license
  fi
fi

# 4. Overlay 620.70.06 (R620) Wayland/GBM userspace libraries into distro library directory
if [ -n "$GRID_620_RUN" ] && [ -f "$GRID_620_RUN" ]; then
  TMP_R620="$(mktemp -d /tmp/grid620.XXXXXX)"
  trap 'rm -rf "$TMP_R620"' EXIT
  bash "$GRID_620_RUN" --extract-only --target "$TMP_R620/pkg" >/dev/null

  mkdir -p "${PRIMARY_LIBDIR}/gbm" /usr/lib64/gbm /usr/share/egl/egl_external_platform.d
  for lib in libnvidia-egl-gbm.so.* libnvidia-egl-wayland.so.* libnvidia-egl-wayland2.so.*; do
    for f in "$TMP_R620/pkg/$lib"; do
      if [ -f "$f" ]; then
        install -m 0755 "$f" "${PRIMARY_LIBDIR}/"
        if [ "$PRIMARY_LIBDIR" != "/usr/lib64" ] && [ -d /usr/lib64 ]; then
          install -m 0755 "$f" /usr/lib64/ 2>/dev/null || true
        fi
      fi
    done
  done

  ln -sf libnvidia-egl-gbm.so.1.1.3 "${PRIMARY_LIBDIR}/libnvidia-egl-gbm.so.1" 2>/dev/null || true
  ln -sf libnvidia-egl-wayland.so.1.1.20 "${PRIMARY_LIBDIR}/libnvidia-egl-wayland.so.1" 2>/dev/null || true
  ln -sf libnvidia-egl-wayland2.so.1.0.2 "${PRIMARY_LIBDIR}/libnvidia-egl-wayland2.so.1" 2>/dev/null || true

  if [ -f "${PRIMARY_LIBDIR}/libnvidia-allocator.so.1" ]; then
    ln -sf ../libnvidia-allocator.so.1 "${PRIMARY_LIBDIR}/gbm/nvidia-drm_gbm.so"
  fi
  if [ -f /usr/lib64/libnvidia-allocator.so.1 ]; then
    ln -sf ../libnvidia-allocator.so.1 /usr/lib64/gbm/nvidia-drm_gbm.so
  elif [ -f "${PRIMARY_LIBDIR}/libnvidia-allocator.so.1" ]; then
    ln -sf "${PRIMARY_LIBDIR}/libnvidia-allocator.so.1" /usr/lib64/gbm/nvidia-drm_gbm.so
  fi

  for js in 09_nvidia_wayland2.json 10_nvidia_wayland.json 15_nvidia_gbm.json; do
    [ -f "$TMP_R620/pkg/$js" ] && install -m 0644 "$TMP_R620/pkg/$js" /usr/share/egl/egl_external_platform.d/
  done
  ldconfig
fi

eval "$INITRAMFS_CMD" 2>/dev/null || true

# 5. GTK4 GSK_RENDERER=ngl (avoids R535 vulkan-producer segfaults across EL, Ubuntu, and Debian)
grep -qxF 'GSK_RENDERER=ngl' /etc/environment 2>/dev/null || echo 'GSK_RENDERER=ngl' >> /etc/environment
cat > /etc/profile.d/gsk-renderer.sh <<'EOF'
export GSK_RENDERER=ngl
EOF
mkdir -p /etc/systemd/user.conf.d
cat > /etc/systemd/user.conf.d/gsk-renderer.conf <<'EOF'
[Manager]
DefaultEnvironment="GSK_RENDERER=ngl"
EOF

# 6. Pure Wayland headless GNOME Shell override
mkdir -p /etc/systemd/user/org.gnome.Shell@wayland.service.d
cat > /etc/systemd/user/org.gnome.Shell@wayland.service.d/override.conf <<'EOF'
[Service]
Environment="GSK_RENDERER=ngl"
ExecStart=
ExecStart=/usr/bin/gnome-shell --no-x11 --headless
EOF

# 7. Enable GDM auto-login across RHEL/AlmaLinux (/etc/gdm/custom.conf) & Ubuntu/Debian (/etc/gdm3/*.conf)
for gdm_conf in "${GDM_CONF_CANDIDATES[@]}"; do
  if [ -f "$gdm_conf" ]; then
    python3 - "$RDP_USER" "$gdm_conf" <<'PY'
import pathlib, re, sys
user = sys.argv[1]
p = pathlib.Path(sys.argv[2])
txt = p.read_text()
if '[daemon]' not in txt:
    txt += '\n[daemon]\n'
for k, v in [('AutomaticLoginEnable', 'True'), ('AutomaticLogin', user), ('WaylandEnable', 'true')]:
    if re.search(rf'^#?\s*{k}=', txt, re.M):
        txt = re.sub(rf'^#?\s*{k}=.*', f'{k}={v}', txt, count=1, flags=re.M)
    else:
        txt = txt.replace('[daemon]\n', f'[daemon]\n{k}={v}\n', 1)
p.write_text(txt)
PY
  fi
done

# 8. Patch GRD PTX header for R535 CUDA 12.2 across EL and Ubuntu/Debian PTX locations
for ptx in /usr/share/gnome-remote-desktop/grd-cuda-avc-utils_30.ptx \
           /usr/local/share/gnome-remote-desktop/grd-cuda-avc-utils_30.ptx; do
  if [ -f "$ptx" ] && [ -f "$SCRIPT_DIR/patch_grd_ptx_535.sh" ]; then
    bash "$SCRIPT_DIR/patch_grd_ptx_535.sh" "$ptx" || true
  fi
done

# 9. Direct headless RDP (:3389), avoiding --system Handover 0x8009030f bug
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

if command -v firewall-cmd >/dev/null 2>&1; then
  firewall-cmd --permanent --add-port=3389/tcp 2>/dev/null || true
  firewall-cmd --reload 2>/dev/null || true
elif command -v ufw >/dev/null 2>&1; then
  ufw allow 3389/tcp 2>/dev/null || true
fi

# 10. Fetch FastAPI-DLS license token
mkdir -p /etc/nvidia/ClientConfigToken
rm -f /etc/nvidia/ClientConfigToken/*.tok
curl --insecure -fsSL -X GET "${DLS_URL}/-/client-token" \
  -o "/etc/nvidia/ClientConfigToken/client_configuration_token_$(date +%d-%m-%Y-%H-%M-%S).tok"
chmod 0644 /etc/nvidia/ClientConfigToken/*.tok
systemctl enable --now nvidia-gridd 2>/dev/null || true
systemctl restart nvidia-gridd 2>/dev/null || true

# 11. Optional Selkies PixelFlux 10-bit 4:4:4 container stack
if [ "$WITH_SELKIES" -eq 1 ] && [ -f "$SCRIPT_DIR/setup_selkies_pixelflux_vgpu.sh" ]; then
  bash "$SCRIPT_DIR/setup_selkies_pixelflux_vgpu.sh"
fi

# 12. Build clean-room libopen-nvidia-encode.so & optional open guest kernel modules
if [ -f "$REPO_ROOT/open-nvenc/Makefile" ] && command -v gcc >/dev/null 2>&1; then
  make -C "$REPO_ROOT/open-nvenc" clean all
  mkdir -p /usr/local/lib64 /usr/local/lib
  install -m 0755 "$REPO_ROOT/open-nvenc/libopen-nvidia-encode.so" /usr/local/lib64/libopen-nvidia-encode.so 2>/dev/null || true
  install -m 0755 "$REPO_ROOT/open-nvenc/libopen-nvidia-encode.so" /usr/local/lib/libopen-nvidia-encode.so 2>/dev/null || true
  make -C "$REPO_ROOT/open-nvenc" clean
fi

if [ "$WITH_OPEN_GUEST_KMOD" -eq 1 ] && [ -f "$REPO_ROOT/open-vgpu-guest/scripts/build_open_535_guest_kmod.sh" ]; then
  bash "$REPO_ROOT/open-vgpu-guest/scripts/build_open_535_guest_kmod.sh" --workdir /tmp/open-535-guest-build --kver "$KVER"
fi

echo "=== Linux vGPU guest provisioning complete (${OS_ID} ${OS_VERSION_ID}); reboot to start headless Wayland RDP ==="
