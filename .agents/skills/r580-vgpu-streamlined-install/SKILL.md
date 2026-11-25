---
name: r580-vgpu-streamlined-install
description: >-
  Streamlined turnkey installation of the Tesla P4 R580.x vGPU stack across
  host (580.196.09-vgpu-kvm + DKMS + /opt/nvidia-vgpu-580 open daemons),
  Linux guests (580.196.07-grid with open-C nv-pci.c 10de:1bb3 patch,
  gridd-unlock-patcher, pure Wayland X11 stripping, CDI Selkies), and
  Windows 11 guests (582.99 WHQL + nvxdapix.dll patching + RDP 0x200d fix).
  Use whenever reprovisioning R580.x host or guests from scratch.
---

# Streamlined R580.x Tesla P4 vGPU Installation (Host + Linux & Windows Guests)

All R580 guest installers (`NVIDIA-Linux-x86_64-580.196.07-grid.run`, `.rpm`, `.deb`, `582.99_grid_win10_win11_server2022_server_2025_dch_64bit_international.exe`) and compiled `gridd-unlock-patcher` are staged on `192.168.1.24:/home/zero/guest/`.

---

## 1. Host Setup (`580.196.09-vgpu-kvm`, Ubuntu KVM or Proxmox VE 8/9)

Run `install_open_vgpu.sh --role host` (calls `scripts/install_vgpu_host_dkms.sh` + `scripts/install_fastapi_dls.sh`) or `scripts/install_proxmox_vgpu_host.sh`:
```bash
sudo bash scripts/install_open_vgpu.sh \
  --role host \
  --run-file /path/to/NVIDIA-Linux-x86_64-580.196.09-vgpu-kvm.run \
  --with-dls docker \
  --dls-url 192.168.1.24
```
What happens automatically:
1. Patches `kernel/nvidia/nv-kernel.o_binary` at file offset `0xc85b7` (`_nv060499rm` RSA bypass) and registers `nvidia-vgpu-kvm/580.196.09` via DKMS (`nvidia.ko` + open `nvidia-vgpu-vfio.ko`; **never** `kernel-open` on GP104).
2. Builds/installs `/opt/nvidia-vgpu-580/` open daemons (`open-nvidia-vgpud`, `libopen-vgxcfg.so`, `open-nvidia-xid-logd`, `open-nvidia-vgpu-mgr` with R580 `0x98`-byte `SET_VM_NAME` / `vmIdType = 1U`, `open-nvidia-smi`, `libopen-nvidia-ml.so`, `libopen-nvidia-vgpu.so` + `libnvidia-vgpu.so -> libnvidia-vgpu.so.580.196.09`).
3. Runs `tools/patch_r580_vgpu_config_xml.py` to ensure `/usr/share/nvidia/vgpu/vgpuConfig.xml` defines all 12 Tesla P4 (`0x1BB3`) profiles with `subsystemId="0x11D8"`.
4. Deploys `collinwebdesigns/fastapi-dls:latest` (`v2.0.3`+) on `https://<HOST_IP>:443`.

If an mdev slot ever leaks (`available_instances = 0` with no VM running after `vgpu-mgr` stop):
```bash
sudo systemctl stop nvidia-vgpu-mgr vgpu-vms.service
sudo modprobe -r nvidia_vgpu_vfio nvidia
sudo modprobe nvidia nvidia_vgpu_vfio
sudo systemctl restart nvidia-vgpud nvidia-vgpu-mgr
```

---

## 2. Linux Guest Setup (`580.196.07-grid`, Pure Wayland + X11 Strip + CDI)

### Root Cause Why R580 `580.196.07-grid` Needs the 2-Line `nv-pci.c` Patch
In `580.196.07`, NVIDIA removed `10de:1bb3` from the closed `_nv054166rm` `.rodata` allowlist checked by `rm_is_supported_device()` inside `nv_pci_probe()`, even though full GP104 vGPU RM, Wayland/GBM, NVENC, NVDEC, and CUDA `13.0` remain intact. Patching 2 lines in the **open C wrapper** `kernel/nvidia/nv-pci.c` (`tools/patch_r580_guest_nv_pci.py`) lets `nvidia.ko` probe `10de:1bb3` cleanly without touching proprietary `.o_binary` files.

### Automated Provisioning (`provision_linux_vgpu_guest.sh`)
Supports **AlmaLinux / RHEL / Rocky / Oracle / CentOS / Fedora (8–10)**, **Ubuntu LTS**, and **Debian**:
```bash
sudo bash scripts/provision_linux_vgpu_guest.sh \
  --dls-url https://192.168.1.24 \
  --grid-580-run /path/to/NVIDIA-Linux-x86_64-580.196.07-grid.run \
  --gridd-patcher /path/to/gridd-unlock-patcher \
  --rdp-user zero \
  --rdp-pass '<YOUR_RDP_PASSWORD>' \
  --with-selkies
```
What this automates end-to-end:
1. Extracts `580.196.07-grid.run`, runs `python3 tools/patch_r580_guest_nv_pci.py <extracted>/kernel`, and installs with `--silent --dkms -m=kernel --no-questions --ui=none`.
2. **Strips NVIDIA X11 artifacts** (`--strip-x11`, default ON): removes `nvidia-settings.desktop`, `/usr/bin/nvidia-settings`, `/usr/bin/nvidia-xconfig`, `nvidia_drv.so`, `libglxserver_nvidia.so*`, `libnvidia-gtk{2,3}.so*`, `20_nvidia_{xcb,xlib}.json`, `libnvidia-egl-{xcb,xlib}.so*`, and any stale `wayland2` files, keeping only pure Wayland (`10_nvidia_wayland.json`, `15_nvidia_gbm.json`).
3. Fetches `https://<HOST_IP>/-/client-token` -> `/etc/nvidia/ClientConfigToken/*.tok`, sets `FeatureType=1` in `/etc/nvidia/gridd.conf`, fetches `/-/config/root-certificate`, and patches `/usr/bin/nvidia-gridd` with `gridd-unlock-patcher -g /usr/bin/nvidia-gridd -c /tmp/dls_root_ca.pem` (`License Status : Licensed`).
4. Configures headless Pure Wayland GNOME (`gnome-shell --no-x11 --headless`), `GSK_RENDERER=ngl`, and direct `:3389` GRD RDP.
5. Configures Docker + NVIDIA Container Toolkit in **CDI mode** (`nvidia-ctk config --in-place --set nvidia-container-runtime.mode=cdi`, regenerating `/etc/cdi/nvidia.yaml` + `/var/run/cdi/nvidia.yaml`) so `selkies-webtop` runs with `NVENC 13.0` (`444-10`) even after `/usr/bin/nvidia-xconfig` is stripped.

---

## 3. Windows 11 24H2 Guest Setup (`582.99` WHQL + `nvxdapix.dll` Patch)

1. **Extract & Patch `nvxdapix.dll` on the Linux Host (`7z` + `gridd-unlock-patcher`)**:
   ```bash
   7z x -y -o/tmp/win582 /home/zero/guest/582.99_grid_win10_win11_server2022_server_2025_dch_64bit_international.exe Display.Driver/nvxdapix.dll
   curl -sk https://192.168.1.24/-/config/root-certificate -o /tmp/dls_root_ca.pem
   cp /tmp/win582/Display.Driver/nvxdapix.dll /tmp/nvxdapix_patched.dll
   /home/zero/guest/gridd-unlock-patcher/gridd-unlock-patcher -g /tmp/nvxdapix_patched.dll -c /tmp/dls_root_ca.pem
   ```
   *(Reference `582.99` `nvxdapix.dll` byte length: `9594600` B.)*
2. **Run Streamlined PowerShell Provisioner on Windows 11 (Elevated)**:
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\provision_win11_guest.ps1 `
     -DlsUrl https://192.168.1.24 `
     -DriverInstallerPath C:\temp\582.99_grid_win10_win11_server2022_server_2025_dch_64bit_international.exe `
     -PatchedNvxdapixPath C:\temp\nvxdapix_patched.dll
   ```
   - Sorts `nvgridsw.inf` newest-first for `PCI\VEN_10DE&DEV_1BB3&SUBSYS_11D810DE` (`GRID P4-4Q`, Secure Boot intact).
   - Matches ONLY the `582.99` DriverStore `nvxdapix.dll` (filtering `nvgridsw.inf_amd64_*` sorted newest-first + byte length match), takes ownership, backs up `.orig`, installs patched DLL, fetches DLS token, sets `FeatureType=1`, disables `Microsoft Basic Display Adapter`, and fixes RDP `0x200d` + enables NLA.
   - If QEMU OVMF stalls on warm reset after WDDM updates, perform a clean cold VM stop/start (`qmp quit` -> `start_vgpu_vms.sh`).
