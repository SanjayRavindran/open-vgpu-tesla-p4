---
name: wayland-vgpu-rdp-and-gtk4
description: >-
  Configure and troubleshoot Pure Wayland GNOME 49.5 (--no-x11 --headless),
  direct headless GNOME Remote Desktop RDP (:3389), NVIDIA 580.196.07 GRID
  Wayland/GBM + CUDA 13.0 libraries (with 2-line nv-pci.c 10de:1bb3 patch,
  gridd-unlock-patcher licensing, and full X11 stripping), and GTK4
  GSK_RENDERER=ngl on AlmaLinux 10.3 (192.168.1.68). Use this skill when
  installing R580 Linux vGPU guests, stripping X11, debugging Wayland RDP
  logins, macOS RDP 0x8009030f NLA errors, or GTK4 app rendering.
---

# Pure Wayland GNOME RDP, R580 (`580.196.07`) & X11 Strip (`192.168.1.68`)

## Core Architecture on `vm-alma` (`192.168.1.68`)
- **Driver Stack (`R580` `580.196.07` GRID)**: Native unified `580.196.07` closed-RM kernel modules (`nvidia`, `nvidia_modeset`, `nvidia_drm modeset=1` built via `--dkms -m=kernel --no-questions --ui=none` — **never** `kernel-open` on Pascal GP104) + native R580 Wayland/GBM platform libraries (`10_nvidia_wayland.json`, `15_nvidia_gbm.json`) and **CUDA 13.0** (`13000`).
- **Required 2-Line Open-C `nv-pci.c` Patch (`tools/patch_r580_guest_nv_pci.py`)**:
  `580.196.07` dropped `10de:1bb3` from `_nv054166rm`'s `.rodata` allowlist even though GP104 vGPU RM/Wayland/CUDA 13.0 work cleanly. Extract `NVIDIA-Linux-x86_64-580.196.07-grid.run` and run `python3 tools/patch_r580_guest_nv_pci.py <extracted>/kernel` so `nv_pci_probe()` allows `pci_dev->vendor == 0x10de && pci_dev->device == 0x1bb3` past `rm_is_supported_device()` without modifying proprietary binaries.
- **Zero-X11 Pure Wayland Strip (`--strip-x11` in `provision_linux_vgpu_guest.sh`)**:
  Even with `--no-x11`, NVIDIA `.run` installs `NVIDIA X Server Settings` (`nvidia-settings.desktop`) and X11/Xorg plugins. Strip them completely so only Wayland/GBM remains:
  ```bash
  sudo rm -f \
    /usr/share/applications/nvidia-settings.desktop \
    /usr/bin/nvidia-settings /usr/bin/nvidia-xconfig \
    /usr/lib64/xorg/modules/drivers/nvidia_drv.so \
    /usr/lib64/xorg/modules/extensions/libglxserver_nvidia.so* \
    /usr/lib64/libnvidia-gtk2.so* /usr/lib64/libnvidia-gtk3.so* \
    /usr/share/egl/egl_external_platform.d/20_nvidia_{xcb,xlib}.json \
    /usr/share/egl/egl_external_platform.d/09_nvidia_wayland2.json \
    /usr/lib64/libnvidia-egl-{xcb,xlib,wayland2}.so*
  sudo update-desktop-database /usr/share/applications
  sudo ldconfig
  ```
  *(Important: after removing `/usr/bin/nvidia-xconfig`, switch NVIDIA Container Toolkit to `mode = "cdi"` in `/etc/nvidia-container-runtime/config.toml` so Docker containers like `selkies-webtop` do not fail trying to invoke `nvidia-xconfig`.)*

## Troubleshooting & Fixes

### 1. Fix macOS RDP `"The Credentials Did Not Work"` (`SEC_E_MESSAGE_ALTERED 0x8009030f`)
- **Cause**: System-wide `gnome-remote-desktop.service` (`--system` Remote Login mode) redirects clients to `org.gnome.RemoteDesktop.Handover.desktop` using a one-time SAM token. The macOS Windows App fails NLA MIC verification (`0x8009030f`) on the redirected session and prompts for `zero`, which Handover rejects (`Could not find user in SAM database`).
- **Fix**: Never use `--system` mode. Run a direct `--headless` user session on `:3389`:
  1. Disable system service: `sudo systemctl disable --now gnome-remote-desktop.service`
  2. Enable GDM auto-login for `zero` in `/etc/gdm/custom.conf` (`AutomaticLoginEnable=True`, `AutomaticLogin=zero`).
  3. Configure `/etc/systemd/user/org.gnome.Shell@wayland.service.d/override.conf` (do **not** overwrite `/usr/bin/gnome-shell`, which is an ELF binary on AlmaLinux 10):
     ```ini
     [Service]
     Environment="GSK_RENDERER=ngl"
     ExecStart=
     ExecStart=/usr/bin/gnome-shell --no-x11 --headless
     ```
  4. Configure and start `gnome-remote-desktop-headless.service` as user `zero`:
     ```bash
     grdctl --headless rdp set-port 3389
     grdctl --headless rdp set-tls-cert ~/.local/share/gnome-remote-desktop/tls/tls.crt
     grdctl --headless rdp set-tls-key ~/.local/share/gnome-remote-desktop/tls/tls.key
     grdctl --headless rdp set-credentials zero '<YOUR_RDP_PASSWORD>'
     grdctl --headless rdp enable
     systemctl --user enable --now gnome-remote-desktop-headless.service
     ```

### 2. GTK4 Hardware Acceleration (`GSK_RENDERER=ngl`)
Force GTK4 (`ptyxis`, `nautilus`, `gnome-control-center`) to use OpenGL 4.6 / EGL (`GSK_RENDERER=ngl`) reliably on Wayland:
1. Add `GSK_RENDERER=ngl` to `/etc/environment` and `/etc/profile.d/gsk-renderer.sh`.
2. Create `/etc/systemd/user.conf.d/gsk-renderer.conf`:
   ```ini
   [Manager]
   DefaultEnvironment="GSK_RENDERER=ngl"
   ```
3. Update live user session environment:
   ```bash
   systemctl --user set-environment WAYLAND_DISPLAY=wayland-0 XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=GNOME GSK_RENDERER=ngl
   dbus-update-activation-environment --systemd WAYLAND_DISPLAY XDG_SESSION_TYPE XDG_CURRENT_DESKTOP GSK_RENDERER
   ```

### 3. Activate R580 vGPU License Against `https://192.168.1.24:443` (`gridd-unlock-patcher`)
Patch vendor `/usr/bin/nvidia-gridd` with `/home/zero/guest/gridd-unlock-patcher` and the FastAPI-DLS root CA:
```bash
sudo curl --insecure -fsSL https://192.168.1.24/-/config/root-certificate -o /tmp/dls_root_ca.pem
sudo systemctl stop nvidia-gridd
sudo /path/to/gridd-unlock-patcher -g /usr/bin/nvidia-gridd -c /tmp/dls_root_ca.pem
sudo mkdir -p /etc/nvidia/ClientConfigToken
sudo rm -f /etc/nvidia/ClientConfigToken/*.tok
sudo curl --insecure -s -L -X GET https://192.168.1.24/-/client-token \
  -o /etc/nvidia/ClientConfigToken/client_configuration_token_$(date +%d-%m-%Y-%H-%M-%S).tok
sudo chmod 0644 /etc/nvidia/ClientConfigToken/*.tok
printf 'FeatureType=1\nEnableUI=FALSE\n' | sudo tee /etc/nvidia/gridd.conf >/dev/null
sudo systemctl restart nvidia-gridd
nvidia-smi -q | grep -A 5 "vGPU Software Licensed Product"
```
