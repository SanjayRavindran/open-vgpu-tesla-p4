---
name: wayland-vgpu-rdp-and-gtk4
description: >-
  Configure and troubleshoot Pure Wayland GNOME 49.5 (--no-x11 --headless),
  direct headless GNOME Remote Desktop RDP (:3389), hybrid R535 kernel + R620
  Wayland/GBM userspace libraries, FastAPI-DLS Linux guest licensing, and GTK4
  GSK_RENDERER=ngl on AlmaLinux 10.3 (192.168.1.68). Use this skill when
  debugging Linux Wayland RDP login issues, macOS RDP 0x8009030f NLA errors,
  or GTK4 app crashes on NVIDIA vGPU guests.
---

# Pure Wayland GNOME RDP & GTK4 Acceleration on Tesla P4 vGPU (`192.168.1.68`)

## Core Architecture on `vm-alma` (`192.168.1.68`)
- **Driver Stack**: `535.309.01` kernel modules (`nvidia`, `nvidia_modeset`, `nvidia_drm modeset=1`) + R620 (`620.70.06`) Wayland/GBM platform libraries:
  - `/usr/lib64/libnvidia-egl-gbm.so.1.1.3` (`15_nvidia_gbm.json`)
  - `/usr/lib64/libnvidia-egl-wayland.so.1.1.20` (`10_nvidia_wayland.json`)
  - `/usr/lib64/libnvidia-egl-wayland2.so.1.0.2` (`09_nvidia_wayland2.json`)
  - `/usr/lib64/gbm/nvidia-drm_gbm.so` -> `../libnvidia-allocator.so.1`
- **GRD CUDA PTX Patch**: `/usr/libexec/gnome-remote-desktop-daemon` has its embedded PTX header patched from `.version 8.5` / `.target sm_52` to `.version 8.2` / `.target sm_61` (`scripts/patch_grd_ptx_535.sh`) for `535.309.01` (`CUDA 12.2`) compatibility.

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

### 2. Fix GTK4 Applications (`ptyxis`, `nautilus`, `gnome-control-center`) Segfaulting on Launch
- **Cause**: GTK4 (`gtk4-4.16.7`) defaults to the `vulkan` GDK renderer (`gdk_vulkan_context_check_swapchain`), which calls R535's `libnvidia-vulkan-producer.so` (`ProducerCleanup`) and segfaults (`SIGSEGV`) against R620's `libnvidia-egl-wayland.so.1.1.20`.
- **Fix**: Force GTK4 to use the hardware-accelerated OpenGL 4.6 / EGL renderer (`GSK_RENDERER=ngl`) everywhere:
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

### 3. Activate vGPU License Against `https://192.168.1.24:443`
`nvidia-gridd` can run our clean-room C11 `open-gridd` daemon (`--with-open-gridd`, see `open-gridd-nls-licensing` skill) instead of the proprietary vendor binary:
```bash
sudo mkdir -p /etc/nvidia/ClientConfigToken
sudo rm -f /etc/nvidia/ClientConfigToken/*.tok
sudo curl --insecure -s -L -X GET https://192.168.1.24/-/client-token \
  -o /etc/nvidia/ClientConfigToken/client_configuration_token_$(date +%d-%m-%Y-%H-%M-%S).tok
sudo chmod 744 /etc/nvidia/ClientConfigToken/*.tok
sudo systemctl restart nvidia-gridd
nvidia-smi -q | grep -A 3 "vGPU Software Licensed Product"
```
