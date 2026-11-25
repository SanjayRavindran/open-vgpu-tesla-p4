---
name: open-gridd-nls-licensing
description: >-
  Build, test, deploy, and troubleshoot the clean-room C11 + OpenSSL 3.x
  open-gridd Linux NLS/DLS licensing daemon (/usr/bin/nvidia-gridd) against
  FastAPI-DLS (:443). Use whenever auditing open-gridd C code, building on
  GCC 14+ (AlmaLinux 10), or comparing open-gridd vs gridd-unlock-patcher on
  R580 Linux vGPU guests.
---

# Clean-Room `open-gridd` Linux NLS/DLS Licensing Daemon (`open-gridd/`)

## Architecture & R580 Status
- **Replaces Vendor `nvidia-gridd`**: Clean-room `~112 KiB` C11 + OpenSSL 3.x daemon installed as `/usr/bin/nvidia-gridd`. Does **not** pin NVIDIA's official root CA and includes GCC 14 `-Werror=format-truncation` safe `snprintf` formatting in `src/http_client.c`.
- **Verified Status**:
  - Verified live (`License Status : Licensed`) on R535 Linux guests (`vm-alma` against `https://192.168.1.24:443`).
  - **R580 (`580.196.07-grid`) Note**: On `580.196.07`, `open-gridd`'s `NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE` (`0x00800289`) RM control currently returns `0x1f` while `open-gridd` R580 guest RM parity is finalized. For production R580 Linux guests (`580.196.07`), `provision_linux_vgpu_guest.sh` supports `--gridd-patcher /path/to/gridd-unlock-patcher` to patch vendor `/usr/bin/nvidia-gridd` with `/-/config/root-certificate` (`License Status : Licensed` verified on `vm-alma`).
- **Protocol & Cryptography**:
  - Parses and verifies RSA-2048 signed JWT client configuration tokens (`.tok` in `/etc/nvidia/ClientConfigToken/`).
  - Executes the NLS/DLS PKCE OAuth flow (`/auth/v1/origin -> /code -> /token`) over HTTPS against FastAPI-DLS (`:443`, `collinwebdesigns/fastapi-dls:latest` v2.x).
  - Acquires and renews leases via `POST /leasing/v1/lessor` & `PUT /leasing/v1/lease/{ref}`.
  - Persists trusted licensing state encrypted with AES-256-GCM.
  - Updates kernel/RM license state via `/dev/nvidiactl` RM ioctls (`0x20808161` feature + `0x20800187` state).

## Build & Unit/Static Audit Tests
```bash
make -C open-gridd clean all test
```
> **Hygiene Reminder**: `open-gridd/tests/test_c_static_audit.py` enforces strict clean-room C coding standards and forbidden-string hygiene across `open-gridd/{docs,include,src,tests,packaging}`. Always run `make -C open-gridd clean all test` after modifying anything under `open-gridd/`.

## Deployment & Token Refresh on Linux vGPU Guests
```bash
sudo mkdir -p /etc/nvidia/ClientConfigToken
sudo rm -f /etc/nvidia/ClientConfigToken/*.tok
sudo curl --insecure -s -L -X GET https://<HOST_IP>/-/client-token \
  -o /etc/nvidia/ClientConfigToken/client_configuration_token_$(date +%d-%m-%Y-%H-%M-%S).tok
sudo chmod 0644 /etc/nvidia/ClientConfigToken/*.tok
printf 'FeatureType=1\nEnableUI=FALSE\n' | sudo tee /etc/nvidia/gridd.conf >/dev/null
sudo systemctl restart nvidia-gridd
nvidia-smi -q | grep -A 5 "vGPU Software Licensed Product"
```
