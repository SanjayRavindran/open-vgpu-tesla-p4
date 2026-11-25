# Complete Host & Guest Installation Guide (`open-vgpu-tesla-p4` & `open-580-driver`)

This guide provides streamlined step-by-step instructions to install either the **50/50 Multi-VM `open-vgpu-tesla-p4` (`580.196.09-vgpu-kvm` host + `580.196.07` Linux guest + `582.99` Windows 11 guest)** stack or the **Bare-Metal / PCIe-Passthrough `open-580-driver` (`580.178.04`)** stack across **Host** and **Guest** machines.

---

## 1. Track A — `open-vgpu-tesla-p4` (`580.196.09-vgpu-kvm` 50/50 Multi-VM Host & Guests)

Use this track to partition a single Tesla P4 into **two concurrent `GRID P4-4Q` (`nvidia-65`, `4096 MiB` VRAM each) vGPUs** on **Ubuntu 24.04/26.04 KVM** or **Proxmox VE 8/9**, running our clean-room open-source host & guest components:
1. **`open-nvidia-vgpud`** (`Rust`, `nova-vgpu/`) — R580 `NVA081` profile registrar (`0x1540` B params)
2. **`libopen-vgxcfg.so`** (`C`, `open-vgxcfg/`) — XML DOM configuration library (`9/9` exports)
3. **`open-nvidia-xid-logd`** (`C`, `open-xid-logd/`) — NVML XID event & Base64 debugdump daemon
4. **`open-nvidia-vgpu-mgr`** (`C`, `open-vgpu-mgr/`) — KVM/VFIO `vmiop-env` runtime daemon (`60/60` `vmiop_*` exports, R580 `0x98`-byte `SET_VM_NAME` / `vmIdType = 1U`)
5. **`open-nvidia-smi` & `libopen-nvidia-ml.so`** (`C`, `open-nvml-smi/`) — Clean-room `nvidia-smi` CLI & NVML (`libnvidia-ml.so.1`)
6. **`libopen-nvidia-vgpu.so`** (`C`, `open-vgpu-plugin/`) — Clean-room `vmiop` mediator plugin (`VER_5_0`/`VER_2_0`, `47/47` VRPCs, `0x50`/`0x9c` static info + hybrid interposer defaulting to `/opt/nvidia-vgpu-580/lib/libnvidia-vgpu.so.580.196.09`)
7. **`open-vgpu-guest` & `nova_vgpu_guest.ko`** (`C`, `open-vgpu-guest/`, `kmods/nova_vgpu_guest/`) — Clean-room guest BAR0 `0x8f000` `"VRPC"` client & loopback verification
8. **`libopen-nvidia-encode.so`** (`C`, `open-nvenc/`) — Clean-room NVENC `13.0` (`0xd0`) userspace library (`libnvidia-encode.so.1` SONAME, `libc.so.6` only, `10/10` exports, GP104 `NVC2B7` H.264/HEVC 8/10-bit 4:2:0 & 4:4:4)
9. **`libopen-host-rm-gp104.so`** (`C`, `open-host-rm/`) — Clean-room Tier 2B host GP104 physical RM HAL slice (`8/8` Category #1 signed Falcon WPR1/ACR LSF headers, 50/50 `GRID P4-4Q` split)
10. **`open-gridd`** (`C11`, `open-gridd/`) — Clean-room Linux NLS/DLS licensing daemon

### 1A. Hypervisor Host Installation (`--role host` on Ubuntu KVM or Proxmox VE 8/9)
```bash
# Dry-run check
bash scripts/install_open_vgpu.sh \
  --role host \
  --run-file /path/to/NVIDIA-Linux-x86_64-580.196.09-vgpu-kvm.run \
  --with-dls docker \
  --dls-url 192.168.1.24 \
  --dry-run

# Full Host Install (Ubuntu KVM with Docker FastAPI-DLS)
sudo bash scripts/install_open_vgpu.sh \
  --role host \
  --run-file /path/to/NVIDIA-Linux-x86_64-580.196.09-vgpu-kvm.run \
  --with-dls docker \
  --dls-url 192.168.1.24

# Full Host Install (Proxmox VE 8/9 one-shot host configurator: IOMMU + headers + DKMS + open daemons + systemd FastAPI-DLS + VMs)
sudo bash scripts/install_proxmox_vgpu_host.sh \
  --run-file /path/to/NVIDIA-Linux-x86_64-580.196.09-vgpu-kvm.run \
  --enable-no-subscription-repo \
  --dls-url <PROXMOX_HOST_IP> \
  --setup-vms fresh
```

This automatically applies the `0xc85b7` `_nv060499rm` RSA validation bypass, runs [`tools/patch_r580_vgpu_config_xml.py`](../tools/patch_r580_vgpu_config_xml.py) to guarantee all 12 `GRID P4-*` profiles (`0x1BB3`, `SUBSYS_11D8`), installs `/opt/nvidia-vgpu-580/`, links `libnvidia-vgpu.so -> libnvidia-vgpu.so.580.196.09`, and configures FastAPI-DLS (`:443`). Verify on the host:
```bash
dkms status
systemctl status nvidia-vgpud nvidia-vgpu-mgr nvidia-xid-logd
/opt/nvidia-vgpu-580/open-nvidia-smi
/opt/nvidia-vgpu-580/open-nvidia-smi vgpu -s
```

### 1B. Streamlined Multi-Distro Linux vGPU Guest (`580.196.07` + Pure Wayland X11 Strip + CDI)
On `192.168.1.24`, the R580 guest installers (`NVIDIA-Linux-x86_64-580.196.07-grid.run`, `.rpm`, `.deb`) and `gridd-unlock-patcher` are staged under `/home/zero/guest/`. R580 provides native Wayland/GBM (`10_nvidia_wayland.json` + `15_nvidia_gbm.json`), **CUDA `13.0`**, and **NVENC `13.0`** directly (no R620 overlay or GRD PTX downgrade required).

Run the streamlined provisioner (automatically extracts `--grid-580-run`, applies [`tools/patch_r580_guest_nv_pci.py`](../tools/patch_r580_guest_nv_pci.py) so `nvidia.ko` `-m=kernel` probes `10de:1bb3`, strips NVIDIA X11 files, patches `nvidia-gridd` via `--gridd-patcher`, sets `GSK_RENDERER=ngl` + headless `--no-x11` `:3389` RDP, and configures Docker/Selkies in `mode = "cdi"`):
```bash
sudo bash scripts/provision_linux_vgpu_guest.sh \
  --dls-url https://192.168.1.24 \
  --grid-580-run /path/to/NVIDIA-Linux-x86_64-580.196.07-grid.run \
  --gridd-patcher /path/to/gridd-unlock-patcher \
  --rdp-user zero \
  --rdp-pass '<YOUR_RDP_PASSWORD>' \
  --with-selkies
```
*(Pass `--keep-x11` only if legacy X11/Xorg binaries such as `nvidia-settings` or `nvidia-xconfig` are explicitly desired.)*

### 1C. Windows 11 24H2 vGPU Guest Installation (`--role guest-windows`, `582.99` WHQL)
1. For unattended Windows 11 24H2 ISO installation (with WinRM NTLM and Remote Desktop pre-enabled), attach [`scripts/templates/win11-autounattend.xml`](../scripts/templates/win11-autounattend.xml) (replacing `__ADMIN_PASSWORD__`).
2. Extract & pre-patch `Display.Driver/nvxdapix.dll` (`9594600` B) on the Linux host with `7z` + `gridd-unlock-patcher`:
   ```bash
   7z x -y -o/tmp/win582 /home/zero/guest/582.99_grid_win10_win11_server2022_server_2025_dch_64bit_international.exe Display.Driver/nvxdapix.dll
   curl -sk https://192.168.1.24/-/config/root-certificate -o /tmp/dls_root_ca.pem
   cp /tmp/win582/Display.Driver/nvxdapix.dll /tmp/nvxdapix_patched.dll
   /home/zero/guest/gridd-unlock-patcher/gridd-unlock-patcher -g /tmp/nvxdapix_patched.dll -c /tmp/dls_root_ca.pem
   ```
3. Copy `582.99...exe` and `nvxdapix_patched.dll` into the Windows 11 VM and run in an elevated PowerShell prompt:
   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\provision_win11_guest.ps1 `
     -DlsUrl https://192.168.1.24 `
     -DriverInstallerPath C:\path\to\582.99_grid_win10_win11_server2022_server_2025_dch_64bit_international.exe `
     -PatchedNvxdapixPath C:\path\to\nvxdapix_patched.dll
   ```
   This binds `582.99` WHQL (`SUBSYS_11D810DE` -> `GRID P4-4Q`), replaces ONLY the `582.99` DriverStore `nvxdapix.dll`, downloads the FastAPI-DLS token (`FeatureType=1`), disables `Microsoft Basic Display Adapter`, removes RDP `0x200d` policy overrides, and enables NLA (`UserAuthentication=1`).

---

## 2. Track B — `open-580-driver` (`580.178.04` Bare-Metal Host or PCIe-Passthrough Linux Guest)

Use this track when assigning the entire physical **Tesla P4 (`GP104GL`, `10de:1bb3`)** to a single bare-metal Linux host (`Linux 6.x / 7.0+`) or a single full PCIe-passthrough (`vfio-pci`) Linux VM for **CUDA 13.0**, **NVENC 13.0**, **NVDEC**, **Wayland EGL/GBM**, and **Selkies / PixelFlux 10-bit 4:4:4 (`YUV444`)**.

```bash
# Bare-Metal Host (--role host)
sudo bash scripts/install_open_580.sh \
  --role host \
  --run-file /path/to/NVIDIA-Linux-x86_64-580.178.04.run \
  --with-selkies

# Full PCIe-Passthrough Linux Guest (--role guest-linux)
sudo bash scripts/install_open_580.sh \
  --role guest-linux \
  --run-file /path/to/NVIDIA-Linux-x86_64-580.178.04.run \
  --with-selkies
```
