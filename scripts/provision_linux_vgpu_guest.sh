#!/usr/bin/env bash
set -euo pipefail

# Universal Multi-Distro Linux R580 vGPU Guest Provisioner for Tesla P4 (GRID P4-4Q / nvidia-65)
# Supports:
#   - Red Hat Enterprise Linux (RHEL 8 / 9 / 10)
#   - AlmaLinux OS (8 / 9 / 10, including AlmaLinux 10.3 x86-64-v2 Xeon E3-1275 V2 ISA handling)
#   - Rocky Linux, Oracle Linux, CentOS Stream, Fedora (Enterprise Linux family)
#   - Ubuntu LTS Releases (20.04, 22.04, 24.04, 26.04 LTS)
#   - Debian Stable / Testing (Debian 11 Bullseye, 12 Bookworm, 13 Trixie)
#
# Configures:
#   1. Distro-specific kernel headers, DKMS/build tools, Wayland/EGL & GNOME Remote Desktop packages
#   2. R580 (580.196.07-grid.run) unified driver with 2-line open-C nv-pci.c Tesla P4 (10de:1bb3) patch (-m=kernel)
#   3. Pure Wayland X11 strip (default ON: removes nvidia-settings, nvidia-xconfig, Xorg drivers & EGL X11 JSONs)
#   4. GTK4 GSK_RENDERER=ngl + Pure Wayland GNOME (--no-x11 --headless) + direct headless RDP (:3389)
#   5. FastAPI-DLS client token + FeatureType=1 + gridd-unlock-patcher (or optional clean-room open-gridd)
#   6. Clean-room libopen-nvidia-encode.so (open-nvenc 13.0) and optional Selkies PixelFlux 10-bit 4:4:4 (CDI mode)

DLS_URL="https://192.168.1.24"
GRID_580_RUN=""
GRIDD_PATCHER=""
RDP_USER="zero"
RDP_PASS="${RDP_PASS:-}"
DISTRO_OVERRIDE=""
STRIP_X11=1
WITH_SELKIES=0
WITH_OPEN_GRIDD=0
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
  --grid-580-run <path>    Path to NVIDIA-Linux-x86_64-580.196.07-grid.run (auto-patches nv-pci.c for 10de:1bb3)
  --gridd-patcher <path>   Path to compiled gridd-unlock-patcher binary (patches /usr/bin/nvidia-gridd CA pin)
  --rdp-user <user>        Desktop / RDP username (default: zero)
  --rdp-pass <pass>        RDP password (or set RDP_PASS env var; required unless --dry-run)
  --distro <id>            Override detected distro ID (e.g. rhel, almalinux, rocky, ol, centos, ubuntu, debian)
  --strip-x11              Strip NVIDIA X11 binaries, Xorg modules & EGL XCB/Xlib layers (default: enabled)
  --keep-x11               Keep NVIDIA X11/Xorg/nvidia-settings files instead of stripping
  --with-selkies           Also configure Docker + NVIDIA Container Toolkit (CDI mode) + Selkies PixelFlux 10-bit 4:4:4
  --with-open-gridd        Also build and install clean-room open-gridd (OpenSSL 3.x C11) to replace /usr/bin/nvidia-gridd
  --dry-run                Print planned distro-specific steps without modifying the system
  -h, --help               Show this help text
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --dls-url) DLS_URL="${2:-}"; shift 2 ;;
    --grid-580-run) GRID_580_RUN="${2:-}"; shift 2 ;;
    --gridd-patcher) GRIDD_PATCHER="${2:-}"; shift 2 ;;
    --rdp-user) RDP_USER="${2:-}"; shift 2 ;;
    --rdp-pass) RDP_PASS="${2:-}"; shift 2 ;;
    --distro) DISTRO_OVERRIDE="${2:-}"; shift 2 ;;
    --strip-x11) STRIP_X11=1; shift ;;
    --keep-x11) STRIP_X11=0; shift ;;
    --with-selkies) WITH_SELKIES=1; shift ;;
    --with-open-gridd) WITH_OPEN_GRIDD=1; shift ;;
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
  echo "[dry-run] Linux R580 vGPU guest provisioning (distro=${OS_ID}, family=${DISTRO_FAMILY}, pkg=${PKG_MGR}, libdir=${PRIMARY_LIBDIR}, DLS_URL=${DLS_URL}, user=${RDP_USER}, strip_x11=${STRIP_X11}, selkies=${WITH_SELKIES})"
  if [ "$DISTRO_FAMILY" = "debian" ]; then
    echo "[dry-run] 1. apt-get install linux-headers-${KVER}, build-essential, dkms, gnome-remote-desktop, freerdp3-x11/freerdp2-x11, libegl1, libwayland-*"
  else
    echo "[dry-run] 1. ${PKG_MGR} install kernel-devel-${KVER}, kernel-headers, gcc, make, elfutils-libelf-devel, gnome-remote-desktop, freerdp, mesa-libEGL, libwayland-*"
  fi
  echo "[dry-run] 2. Extract 580.196.07-grid.run, patch kernel/nvidia/nv-pci.c (10de:1bb3 via tools/patch_r580_guest_nv_pci.py), install --silent --dkms -m=kernel"
  echo "[dry-run] 3. Configure /etc/modprobe.d/nvidia-drm.conf (options nvidia-drm modeset=1) and run '${INITRAMFS_CMD}'"
  if [ "$STRIP_X11" -eq 1 ]; then
    echo "[dry-run] 4. Strip NVIDIA X11 artifacts (nvidia-settings, nvidia-xconfig, nvidia_drv.so, libglxserver_nvidia, libnvidia-gtk*, 20_nvidia_{xcb,xlib}.json, stale wayland2), keeping 10_nvidia_wayland.json + 15_nvidia_gbm.json"
  fi
  echo "[dry-run] 5. Apply GTK4 GSK_RENDERER=ngl and pure Wayland GNOME (--no-x11 --headless) + GDM auto-login (${GDM_CONF_CANDIDATES[0]}) + direct headless RDP :3389"
  echo "[dry-run] 6. Fetch DLS client token (${DLS_URL}/-/client-token), write /etc/nvidia/gridd.conf (FeatureType=1), apply gridd-unlock-patcher or open-gridd"
  if [ "$WITH_SELKIES" -eq 1 ]; then
    echo "[dry-run] 7. Run setup_selkies_pixelflux_vgpu.sh (Docker + NVIDIA Container Toolkit CDI mode + Selkies PixelFlux 10-bit 4:4:4)"
  fi
  if [ "$WITH_OPEN_GRIDD" -eq 1 ]; then
    echo "[dry-run] 8. Build & install clean-room open-gridd (backing up vendor /usr/bin/nvidia-gridd -> .vendor.bak)"
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

echo "=== Provisioning Linux R580 vGPU Guest (distro=${OS_ID} ${OS_VERSION_ID}, family=${DISTRO_FAMILY}, kernel=${KVER}) ==="

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

# 3. Extract 580.196.07 GRID guest driver, apply open-C nv-pci.c Tesla P4 patch, and install (-m=kernel)
if [ -n "$GRID_580_RUN" ] && [ -f "$GRID_580_RUN" ]; then
  TMP_G580="$(mktemp -d /tmp/grid580.XXXXXX)"
  trap 'rm -rf "$TMP_G580"' EXIT
  bash "$GRID_580_RUN" --extract-only --target "$TMP_G580/pkg" >/dev/null
  if [ -f "$REPO_ROOT/tools/patch_r580_guest_nv_pci.py" ]; then
    python3 "$REPO_ROOT/tools/patch_r580_guest_nv_pci.py" "$TMP_G580/pkg/kernel"
  fi
  (cd "$TMP_G580/pkg" && ./nvidia-installer --silent --dkms -m=kernel --no-questions --ui=none --no-x-check --no-nouveau-check)
  ldconfig
fi

# 4. Strip NVIDIA X11 artifacts for pure Wayland guest (retains 10_nvidia_wayland.json + 15_nvidia_gbm.json)
if [ "$STRIP_X11" -eq 1 ]; then
  rm -f \
    /usr/share/applications/nvidia-settings.desktop \
    /usr/share/man/man1/nvidia-settings.1* \
    /usr/share/man/man1/nvidia-xconfig.1* \
    /usr/bin/nvidia-settings \
    /usr/bin/nvidia-xconfig \
    /usr/lib64/xorg/modules/drivers/nvidia_drv.so \
    /usr/lib/xorg/modules/drivers/nvidia_drv.so \
    /usr/lib64/xorg/modules/extensions/libglxserver_nvidia.so* \
    /usr/lib/xorg/modules/extensions/libglxserver_nvidia.so* \
    /usr/lib64/libnvidia-gtk2.so* \
    /usr/lib64/libnvidia-gtk3.so* \
    /usr/lib/x86_64-linux-gnu/libnvidia-gtk2.so* \
    /usr/lib/x86_64-linux-gnu/libnvidia-gtk3.so* \
    /usr/share/egl/egl_external_platform.d/20_nvidia_xcb.json \
    /usr/share/egl/egl_external_platform.d/20_nvidia_xlib.json \
    /usr/share/egl/egl_external_platform.d/09_nvidia_wayland2.json \
    /usr/lib64/libnvidia-egl-xcb.so* \
    /usr/lib64/libnvidia-egl-xlib.so* \
    /usr/lib64/libnvidia-egl-wayland2.so* \
    /usr/lib/x86_64-linux-gnu/libnvidia-egl-xcb.so* \
    /usr/lib/x86_64-linux-gnu/libnvidia-egl-xlib.so* \
    /usr/lib/x86_64-linux-gnu/libnvidia-egl-wayland2.so* 2>/dev/null || true
  update-desktop-database /usr/share/applications 2>/dev/null || true
  ldconfig
fi

eval "$INITRAMFS_CMD" 2>/dev/null || true

# 5. GTK4 GSK_RENDERER=ngl (OpenGL 4.6 / EGL hardware renderer on Wayland)
grep -qxF 'GSK_RENDERER=ngl' /etc/environment 2>/dev/null || echo 'GSK_RENDERER=ngl' >> /etc/environment
cat > /etc/profile.d/gsk-renderer.sh <<'EOF'
export GSK_RENDERER=ngl
EOF
mkdir -p /etc/systemd/user.conf.d
cat > /etc/systemd/user.conf.d/gsk-renderer.conf <<'EOF'
[Manager]
DefaultEnvironment="GSK_RENDERER=ngl"
EOF

# 6. Pure Wayland headless GNOME Shell override (--no-x11)
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

# 8. Direct headless RDP (:3389), avoiding --system Handover 0x8009030f bug
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

# 9. Fetch FastAPI-DLS client token and configure FeatureType=1
mkdir -p /etc/nvidia/ClientConfigToken
rm -f /etc/nvidia/ClientConfigToken/*.tok
curl --insecure -fsSL -X GET "${DLS_URL}/-/client-token" \
  -o "/etc/nvidia/ClientConfigToken/client_configuration_token_$(date +%d-%m-%Y-%H-%M-%S).tok"
chmod 0644 /etc/nvidia/ClientConfigToken/*.tok
if [ ! -f /etc/nvidia/gridd.conf ]; then
  printf 'FeatureType=1\nEnableUI=FALSE\n' > /etc/nvidia/gridd.conf
fi

if [ "$WITH_OPEN_GRIDD" -eq 1 ] && [ -d "$REPO_ROOT/open-gridd" ]; then
  if [ "$DISTRO_FAMILY" = "debian" ]; then
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq cmake libssl-dev || true
  else
    "$PKG_MGR" install -y -q cmake openssl-devel || true
  fi
  if [ -f /usr/bin/nvidia-gridd ] && [ ! -f /usr/bin/nvidia-gridd.vendor.bak ]; then
    cp -a /usr/bin/nvidia-gridd /usr/bin/nvidia-gridd.vendor.bak
  fi
  if command -v cmake >/dev/null 2>&1; then
    make -C "$REPO_ROOT/open-gridd" clean all
    install -m 0755 "$REPO_ROOT/open-gridd/build/nvidia-gridd" /usr/bin/nvidia-gridd
    make -C "$REPO_ROOT/open-gridd" clean
  else
    gcc -std=c11 -O2 -Wall -Wextra -Werror -Wpedantic -I"$REPO_ROOT/open-gridd/include" \
      "$REPO_ROOT/open-gridd/src/"*.c -o /usr/bin/nvidia-gridd -lssl -lcrypto -lpthread -ldl
    chmod 0755 /usr/bin/nvidia-gridd
  fi
elif [ -n "$GRIDD_PATCHER" ] && [ -x "$GRIDD_PATCHER" ] && [ -f /usr/bin/nvidia-gridd ]; then
  systemctl stop nvidia-gridd 2>/dev/null || true
  [ -f /usr/bin/nvidia-gridd.vendor.orig ] || cp -a /usr/bin/nvidia-gridd /usr/bin/nvidia-gridd.vendor.orig
  curl --insecure -fsSL "${DLS_URL}/-/config/root-certificate" -o /tmp/dls_root_ca.pem
  "$GRIDD_PATCHER" -g /usr/bin/nvidia-gridd -c /tmp/dls_root_ca.pem
  rm -f /tmp/dls_root_ca.pem
fi

systemctl daemon-reload 2>/dev/null || true
systemctl enable --now nvidia-gridd 2>/dev/null || true
systemctl restart nvidia-gridd 2>/dev/null || true

# 10. Optional Selkies PixelFlux 10-bit 4:4:4 container stack (CDI mode for X11-stripped guests)
if [ "$WITH_SELKIES" -eq 1 ] && [ -f "$SCRIPT_DIR/setup_selkies_pixelflux_vgpu.sh" ]; then
  bash "$SCRIPT_DIR/setup_selkies_pixelflux_vgpu.sh"
fi

# 11. Install clean-room libopen-nvidia-encode.so (NVENC 13.0)
if [ -f "$SCRIPT_DIR/install_open_nvenc.sh" ] && [ -f "$REPO_ROOT/open-nvenc/Makefile" ] && command -v gcc >/dev/null 2>&1; then
  bash "$SCRIPT_DIR/install_open_nvenc.sh"
elif [ -f "$REPO_ROOT/open-nvenc/Makefile" ] && command -v gcc >/dev/null 2>&1; then
  make -C "$REPO_ROOT/open-nvenc" clean all
  mkdir -p /usr/local/lib64 /usr/local/lib
  install -m 0755 "$REPO_ROOT/open-nvenc/libopen-nvidia-encode.so" /usr/local/lib64/libopen-nvidia-encode.so 2>/dev/null || true
  install -m 0755 "$REPO_ROOT/open-nvenc/libopen-nvidia-encode.so" /usr/local/lib/libopen-nvidia-encode.so 2>/dev/null || true
  make -C "$REPO_ROOT/open-nvenc" clean
fi

echo "=== Linux R580 vGPU guest provisioning complete (${OS_ID} ${OS_VERSION_ID}); reboot to start headless Wayland RDP ==="
