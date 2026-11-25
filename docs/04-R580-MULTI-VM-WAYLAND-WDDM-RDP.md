# 04 — 2-VM 50/50 (`GRID P4-4Q`) Pure Wayland R580 (`580.196.07`), Windows 11 (`582.99`) & Selkies/PixelFlux Deployment Guide

## 1. Host & 50/50 2-VM Architecture (`192.168.1.24`, UEFI Coreboot)

Following the R580 upgrade on the **UEFI Coreboot (`df45ea2c49b4-dirty` on Gigabyte `H61M-S1 PLUS`)** hypervisor, the physical Tesla P4 KVM host runs at **`192.168.1.24`** (`br0`) and divides its 8 CPU threads and 8 GiB Tesla P4 GPU **50/50** across two R580 production VMs:

| Machine | LAN IP | CPU / RAM / vGPU Profile | OS, R580 Driver & Active Services |
| :--- | :--- | :--- | :--- |
| **KVM Host (`h61`)** | `192.168.1.24` (`br0`) | Intel Xeon E3-1275 V2 (`4C/8T`, `x86-64-v2`), `14 GiB` RAM, Tesla P4 (`8192 MiB`) | Ubuntu 26.04 LTS (`7.0.0-43/44-generic`), **`580.196.09-vgpu-kvm`** (`0xc85b7` RSA bypass + `vgpuConfig.xml` `0x11D8` + `/opt/nvidia-vgpu-580/` open daemons), Intel VT-d (`DMAR`), `fastapi-dls:latest` (`2.0.3`) on `:443`, `vgpu-vms.service` enabled on boot |
| **`vm-alma`** | `192.168.1.68` | `4 vCPU` (50%), `5120 MiB` RAM, **`GRID P4-4Q` (`nvidia-65`, `4096 MiB` VRAM, 50% GPU)** | AlmaLinux 10.3 (`6.12.0-264.16.1.el10_3.x86_64_v2`), **`580.196.07-grid`** (`nv-pci.c` `10de:1bb3` 2-line open-C patch, `-m=kernel`, **CUDA `13.0`**, **NVENC `13.0`**), **Pure Wayland GNOME `49.5` (`--no-x11 --headless`)** with all NVIDIA X11 artifacts stripped + `gridd-unlock-patcher` (`Licensed`) + headless `:3389` RDP + **Docker + NVIDIA CDI (`mode = "cdi"`) + Selkies PixelFlux (`10-bit 4:4:4`)** |
| **`vm-win11`** | `192.168.1.74` (DHCP) / `.65` | `4 vCPU` (50%), `6144 MiB` RAM, **`GRID P4-4Q` (`nvidia-65`, `4096 MiB` VRAM, 50% GPU)** | Windows 11 24H2 Pro (UEFI + Secure Boot + `swtpm` TPM 2.0), **`582.99` WHQL (`WDDM 3.1`, CUDA `13.0`)** bound natively to `PCI\VEN_10DE&DEV_1BB3&SUBSYS_11D810DE`, DriverStore `nvxdapix.dll` patched via `gridd-unlock-patcher` (`Licensed`), `Microsoft Basic Display Adapter` disabled, hardware `AVC444` RDP (`:3389`) |

---

## 2. Streamlined R580 Deployment Flow (Host & Guests)

### 2.1 Host (`580.196.09-vgpu-kvm`)
- Automated by [`scripts/install_open_vgpu.sh`](../scripts/install_open_vgpu.sh) `--role host` (`scripts/install_vgpu_host_dkms.sh` + `scripts/install_fastapi_dls.sh`) or [`scripts/install_proxmox_vgpu_host.sh`](../scripts/install_proxmox_vgpu_host.sh):
  1. Applies the 11-byte `_nv060499rm` RSA signature validation bypass at `nv-kernel.o_binary` offset `0xc85b7`.
  2. Patches `/usr/share/nvidia/vgpu/vgpuConfig.xml` via [`tools/patch_r580_vgpu_config_xml.py`](../tools/patch_r580_vgpu_config_xml.py) so all 12 `GRID P4-*` profiles (`0x1BB3`) present `subsystemId="0x11D8"`.
  3. Builds & installs clean-room host daemons into `/opt/nvidia-vgpu-580/` (`open-nvidia-vgpud`, `libopen-vgxcfg.so`, `open-nvidia-xid-logd`, `open-nvidia-vgpu-mgr` with `0x98`-byte `SET_VM_NAME` / `vmIdType = 1U`, `open-nvidia-smi`, `libopen-nvidia-ml.so`, `libopen-nvidia-vgpu.so`, `libopen-host-rm-gp104.so`) and symlinks `/opt/nvidia-vgpu-580/lib/libnvidia-vgpu.so -> libnvidia-vgpu.so.580.196.09`.
  4. Configures `collinwebdesigns/fastapi-dls:latest` (`2.0.3`+) on `https://<HOST_IP>:443`.

### 2.2 Linux Guest (`580.196.07-grid`, Pure Wayland + X11 Strip + CDI Selkies)
Automated by [`scripts/provision_linux_vgpu_guest.sh`](../scripts/provision_linux_vgpu_guest.sh):
1. **Open-C `nv-pci.c` Allowlist Patch ([`tools/patch_r580_guest_nv_pci.py`](../tools/patch_r580_guest_nv_pci.py))**: `580.196.07-grid` removed `10de:1bb3` from `_nv054166rm`'s `.rodata` table while keeping GP104 vGPU RM/Wayland/CUDA 13.0 intact. Patching 2 open-C lines in `kernel/nvidia/nv-pci.c` lets `nvidia.ko` (`-m=kernel`, never `kernel-open`) probe `10de:1bb3` cleanly.
2. **X11 Artifact Stripping (`--strip-x11`, default enabled)**: Removes `nvidia-settings.desktop`, `/usr/bin/nvidia-{settings,xconfig}`, `nvidia_drv.so`, `libglxserver_nvidia.so*`, `libnvidia-gtk{2,3}.so*`, `20_nvidia_{xcb,xlib}.json`, `libnvidia-egl-{xcb,xlib}.so*`, and stale `wayland2` files, keeping only pure Wayland (`10_nvidia_wayland.json` + `15_nvidia_gbm.json`).
3. **Licensing (`gridd-unlock-patcher`)**: Pulls `/-/client-token`, sets `FeatureType=1` in `/etc/nvidia/gridd.conf`, pulls `/-/config/root-certificate`, and patches vendor `/usr/bin/nvidia-gridd` cleanly (`License Status : Licensed`).
4. **Pure Wayland GNOME + Direct Headless `:3389` RDP + GTK4 `GSK_RENDERER=ngl`**: Uses `/usr/bin/gnome-shell --no-x11 --headless` with direct `gnome-remote-desktop-headless.service` (avoiding `--system` Handover `0x8009030f` on macOS RDP clients).
5. **Docker + NVIDIA Container Toolkit (`mode = "cdi"`)**: [`scripts/setup_selkies_pixelflux_vgpu.sh`](../scripts/setup_selkies_pixelflux_vgpu.sh) explicitly configures `nvidia-ctk config --in-place --set nvidia-container-runtime.mode=cdi` so `selkies-webtop` runs seamlessly without `/usr/bin/nvidia-xconfig`, syncing `/etc/cdi/nvidia.yaml` -> `/var/run/cdi/nvidia.yaml` and preserving `x86-64-v2`-clean `/usr/bin/runc`.

### 2.3 Windows 11 24H2 Guest (`582.99` WHQL, `SUBSYS_11D810DE`)
Automated by [`scripts/provision_win11_guest.ps1`](../scripts/provision_win11_guest.ps1):
1. Installs `582.99` WHQL (or binds the newest `nvgridsw.inf`) natively onto `PCI\VEN_10DE&DEV_1BB3&SUBSYS_11D810DE` (`Section003`, `"NVIDIA Tesla P4"`, keeping Secure Boot intact).
2. Replaces ONLY the matching `582.99` DriverStore `nvxdapix.dll` (`9594600` B, patched via `gridd-unlock-patcher` with `/-/config/root-certificate`), refreshes the FastAPI-DLS client token, and sets `FeatureType=1` (`License Status : Licensed`).
3. Disables `Microsoft Basic Display Adapter`, removes `AVCHardwareEncodePreferred` / `AVC444ModePreferred` policy overrides that cause RDP Error `0x200d` (`RDP_GFX 0x8007007A`), and enables NLA (`UserAuthentication=1`). *(Uses DHCP `192.168.1.74` when `.65` conflicts on the LAN.)*
