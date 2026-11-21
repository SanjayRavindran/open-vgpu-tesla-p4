#!/usr/bin/env bash
set -euo pipefail

# Reproducible FastAPI-DLS v2 installer for NVIDIA vGPU licensing:
#   - --mode docker  (Ubuntu KVM hosts with Docker Engine)
#   - --mode systemd (Proxmox VE or Docker-less Linux hosts via Python venv + uvicorn)

MODE="docker"
DLS_URL="192.168.1.24"
DLS_PORT="443"
INSTALL_DIR="/opt/docker/fastapi-dls"
SYSTEMD_DIR="/opt/fastapi-dls"
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: install_fastapi_dls.sh --mode <docker|systemd> --dls-url <HOST_IP_OR_DNS> [options]

Options:
  --mode <docker|systemd>   Deployment mode (default: docker)
  --dls-url <host>          Reachable host IP/FQDN for VMs (default: 192.168.1.24)
  --port <port>             HTTPS port (default: 443)
  --dry-run                 Print configuration & planned actions without modifying system
  -h, --help                Show help
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --mode) MODE="${2:-}"; shift 2 ;;
    --dls-url) DLS_URL="${2:-}"; shift 2 ;;
    --port) DLS_PORT="${2:-}"; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [ "$MODE" != "docker" ] && [ "$MODE" != "systemd" ]; then
  echo "Invalid --mode '$MODE' (must be 'docker' or 'systemd')" >&2
  exit 2
fi

if [ "$DRY_RUN" -eq 1 ]; then
  echo "[dry-run] FastAPI-DLS mode=${MODE} DLS_URL=${DLS_URL} DLS_PORT=${DLS_PORT}"
  if [ "$MODE" = "docker" ]; then
    echo "[dry-run] Generate self-signed RSA/TLS certs under ${INSTALL_DIR}/cert"
    echo "[dry-run] Write ${INSTALL_DIR}/docker-compose.yml and run 'docker compose up -d'"
  else
    echo "[dry-run] Clone https://git.collinwebdesigns.de/oscar.krause/fastapi-dls.git -> ${SYSTEMD_DIR}"
    echo "[dry-run] Create ${SYSTEMD_DIR}/.venv with uvicorn & dependencies, write /etc/systemd/system/fastapi-dls.service"
  fi
  echo "[dry-run] Verify: curl -sk https://${DLS_URL}:${DLS_PORT}/-/health"
  exit 0
fi

if [ "$(id -u)" -ne 0 ]; then
  echo "Run as root (sudo)." >&2
  exit 1
fi

gen_tls_certs() {
  local cert_dir="$1"
  mkdir -p "$cert_dir"
  if [ ! -f "$cert_dir/webserver.key" ] || [ ! -f "$cert_dir/webserver.crt" ]; then
    openssl req -x509 -nodes -days 3650 -newkey rsa:2048 \
      -keyout "$cert_dir/webserver.key" \
      -out "$cert_dir/webserver.crt" \
      -subj "/CN=${DLS_URL}" \
      -addext "subjectAltName=IP:${DLS_URL},DNS:localhost"
  fi
  if [ ! -f "$cert_dir/instance.private.pem" ]; then
    openssl genrsa -out "$cert_dir/instance.private.pem" 2048
    openssl rsa -in "$cert_dir/instance.private.pem" -pubout -out "$cert_dir/instance.public.pem"
  fi
}

if [ "$MODE" = "docker" ]; then
  if ! command -v docker >/dev/null 2>&1; then
    echo "Docker is not installed. Use '--mode systemd' on Proxmox VE or install docker.io first." >&2
    exit 1
  fi
  gen_tls_certs "${INSTALL_DIR}/cert"
  cat > "${INSTALL_DIR}/docker-compose.yml" <<EOF
services:
  fastapi-dls:
    image: collinwebdesigns/fastapi-dls:latest
    container_name: fastapi-dls
    restart: unless-stopped
    environment:
      DLS_URL: "${DLS_URL}"
      DLS_PORT: "${DLS_PORT}"
      TZ: "UTC"
    ports:
      - "${DLS_PORT}:443"
    volumes:
      - "${INSTALL_DIR}/cert:/app/cert"
EOF
  (cd "$INSTALL_DIR" && docker compose up -d)
else
  DEBIAN_FRONTEND=noninteractive apt-get update -qq
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq git python3-venv python3-pip openssl
  if [ ! -d "${SYSTEMD_DIR}/.git" ]; then
    rm -rf "$SYSTEMD_DIR"
    git clone https://git.collinwebdesigns.de/oscar.krause/fastapi-dls.git "$SYSTEMD_DIR"
  fi
  gen_tls_certs "${SYSTEMD_DIR}/app/cert"
  python3 -m venv "${SYSTEMD_DIR}/.venv"
  "${SYSTEMD_DIR}/.venv/bin/pip" install --quiet --upgrade pip
  "${SYSTEMD_DIR}/.venv/bin/pip" install --quiet -r "${SYSTEMD_DIR}/app/requirements.txt" || \
    "${SYSTEMD_DIR}/.venv/bin/pip" install --quiet fastapi uvicorn cryptography python-jose sqlalchemy
  cat > /etc/systemd/system/fastapi-dls.service <<EOF
[Unit]
Description=FastAPI-DLS NVIDIA vGPU License Server
After=network-online.target
Wants=network-online.target

[Service]
WorkingDirectory=${SYSTEMD_DIR}/app
Environment=DLS_URL=${DLS_URL}
Environment=DLS_PORT=${DLS_PORT}
ExecStart=${SYSTEMD_DIR}/.venv/bin/uvicorn main:app --host 0.0.0.0 --port ${DLS_PORT} --ssl-keyfile ${SYSTEMD_DIR}/app/cert/webserver.key --ssl-certfile ${SYSTEMD_DIR}/app/cert/webserver.crt
Restart=always

[Install]
WantedBy=multi-user.target
EOF
  systemctl daemon-reload
  systemctl enable --now fastapi-dls.service
fi

echo "=== FastAPI-DLS deployed (${MODE}); health check: ==="
curl --insecure -fsS "https://127.0.0.1:${DLS_PORT}/-/health" || true
echo ""
