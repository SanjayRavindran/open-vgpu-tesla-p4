---
name: open-gridd-nls-licensing
description: >-
  Build, test, deploy, and troubleshoot the clean-room C11 + OpenSSL 3.x
  open-gridd Linux NLS/DLS licensing daemon (/usr/bin/nvidia-gridd) against
  FastAPI-DLS (:443). Use whenever provisioning Linux vGPU guests with
  --with-open-gridd, auditing open-gridd C code, or debugging vGPU licensing.
---

# Clean-Room `open-gridd` Linux NLS/DLS Licensing Daemon (`open-gridd/`)

## Architecture
- **Replaces Vendor `nvidia-gridd`**: Replaces the closed `5.31 MiB` vendor ELF with a `~112 KiB` clean-room C11 + OpenSSL 3.x daemon installed as `/usr/bin/nvidia-gridd`.
- **Protocol & Cryptography**:
  - Parses and verifies RSA-2048 signed JWT client configuration tokens (`.tok` in `/etc/nvidia/ClientConfigToken/`).
  - Executes the NLS/DLS PKCE OAuth flow (`/auth/v1/origin -> /code -> /token`) over HTTPS against FastAPI-DLS (`:443`).
  - Acquires and renews leases via `POST /leasing/v1/lessor` & `PUT /leasing/v1/lease/{ref}`.
  - Persists trusted licensing state encrypted with AES-256-GCM.
  - Updates kernel/RM license state via `/dev/nvidiactl` RM ioctls (`0x20808161` feature + `0x20800187` state) so `nvidia-smi -q` reports `License Status : Licensed`.

## Build & Unit/Static Audit Tests
```bash
cmake -S open-gridd -B open-gridd/build && cmake --build open-gridd/build
cd open-gridd && python3 -B -m unittest discover -s tests -v
```
> **Hygiene Reminder**: `open-gridd/tests/test_c_static_audit.py` enforces strict clean-room C coding standards and forbidden-string hygiene across `open-gridd/{docs,include,src,tests,packaging}`. Always run the `open-gridd` test suite after modifying anything under `open-gridd/`.

## Deployment & Live Verification on Linux vGPU Guests
1. Provision with `--with-open-gridd`:
   ```bash
   sudo bash scripts/provision_linux_vgpu_guest.sh \
     --dls-url https://<HOST_IP> \
     --grid-535-run /path/to/NVIDIA-Linux-x86_64-535.309.01-grid.run \
     --grid-620-run /path/to/NVIDIA-Linux-x86_64-620.70.06-grid.run \
     --rdp-user zero --rdp-pass '<YOUR_RDP_PASSWORD>' \
     --with-open-gridd
   ```
2. Refresh client token and restart service manually if needed:
   ```bash
   sudo mkdir -p /etc/nvidia/ClientConfigToken
   sudo rm -f /etc/nvidia/ClientConfigToken/*.tok
   sudo curl --insecure -s -L -X GET https://<HOST_IP>/-/client-token \
     -o /etc/nvidia/ClientConfigToken/client_configuration_token_$(date +%d-%m-%Y-%H-%M-%S).tok
   sudo chmod 744 /etc/nvidia/ClientConfigToken/*.tok
   sudo systemctl restart nvidia-gridd
   nvidia-smi -q | grep -A 3 "vGPU Software Licensed Product"
   ```
