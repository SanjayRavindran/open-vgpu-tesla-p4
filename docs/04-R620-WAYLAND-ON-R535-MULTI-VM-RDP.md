# R620 Wayland Stack on R535 Pascal vGPU + Multi-Distro LAN RDP VMs

## 1. Host Setup (`192.168.1.63`, `h61`, Tesla P4 `10de:1bb3`)

- **Host vGPU Driver**: `NVIDIA-GRID-Linux-KVM-535.309.01` (`535.309.01-vgpu-kvm` installed after removing `580`; all 4 active `vgpu` processes running on `0000:01:00.0`).
- **LAN Bridge (`br0`)**: `/etc/netplan/01-br0.yaml` bridges `enp3s0` (`d0:50:99:7c:84:dd`) into `br0` at static `192.168.1.63/24` (gateway `192.168.1.1`, `stp: false`, `forward-delay: 0`). `/etc/qemu/bridge.conf` allows `br0`, with `/usr/lib/qemu/qemu-bridge-helper` setuid root.
- **vGPU Profile**: `nvidia-64` (`GRID P4-2Q`, `2048 MiB` VRAM per VM; 4 instances totaling `8000 MiB` active across the 8192 MiB Tesla P4).
- **Host Launcher**: `/home/zero/vms/start_all_vms.sh` idempotently recreates mdev UUIDs and starts all 4 VMs if any are stopped.

---

## 2. Why R620 (`620.70.06`) Cannot Run Natively on Pascal & How the Wayland Bridge Works

Static binary + Ghidra analysis (`192.168.1.15`) of `NVIDIA-Linux-x86_64-620.70.06-grid.run` (`nv-kernel.o_binary`) and `535.309.01` `libnvidia-vgpu.so`:

1. **Pascal RM HALs Stripped from `620.70.06`**:
   - `_nv015641rm` (`.rodata+0x178a0`, file `0x1028720`, 116 x 12-byte entries read by `_nv029926rm` / `rm_is_supported_device`) has entries `[0..36]` (Maxwell, Pascal `0x13`, Volta) zeroed out (`0x0`); table entries start at `[37]` (`arch=0x16`, `impl=0x02`, Turing `TU102`).
   - Even after patching PCI ID `0x1bb3` (`0x55ca0e0`), arch entry `[17]` (`0x55cc258`), and VGX version clamps (`0x1a0001 -> 0x70001`, `0x1f0001 -> 0x120001`), `620.70.06` kernel modules cleanly compile (`make rc: 0`) but reject `10de:1bb3` at probe time because Pascal hardware initialization HALs are absent.
2. **Wayland Compatibility Bridge (`/home/zero/vms/wayland620.tgz`)**:
   - `readelf -d` and `nm -D -u` on R620's Wayland/EGL external platform libraries (`libnvidia-egl-wayland2.so.1.0.2`, `libnvidia-egl-wayland.so.1.1.20`, `libnvidia-egl-gbm.so.1.1.3`, `libnvidia-egl-xcb.so.1.0.6`, `libnvidia-egl-xlib.so.1.0.6`) showed **zero proprietary NVIDIA library dependencies** — they depend strictly on standard system libraries (`libc`, `libpthread`, `librt`, `libdl`, `libwayland-client`, `libwayland-server`, `libgbm`, `libdrm`) and speak standard EGL External Platform / `nvidia_drm` (`modeset=1`) interfaces.
   - Installing `535.309.01-grid` for Pascal kernel/driver support and overlaying R620's Wayland/GBM external platform libraries + JSON configs (`09_nvidia_wayland2.json`, `10_nvidia_wayland.json`, `15_nvidia_gbm.json`, `20_nvidia_xcb.json`, `20_nvidia_xlib.json`) enables full GPU-accelerated Wayland compositors and clients across modern distributions.

---

## 3. Verified Linux VMs (LAN IPs, Wayland, GPU Acceleration, & RDP)

All VMs use credentials **`zero` / `<lab-password>`** for both SSH (`22`) and RDP (`3389`):

| VM Name | Distro & Kernel | LAN IP (Port 3389) | Wayland & RDP Stack | Observed GPU & Renderer Verification |
| :--- | :--- | :--- | :--- | :--- |
| `vm-ubuntu` | Ubuntu 24.04 | `192.168.1.66:3389` | GDM + `gnome-shell` Wayland (`Type=wayland`) + `gnome-remote-desktop --system` | `GRID P4-2Q` (`535.309.01`); `gnome-shell` + `Xwayland` on GPU (`41 MiB` VRAM); `glmark2-wayland` `GRID P4-2Q/PCI/SSE2` (`3791 FPS`); Vulkan `1.3.242`; live `xfreerdp3` passed. |
| `vm-debian` | Debian 12 (`6.1.0-56-amd64`) | `192.168.1.67:3389` | Weston `14.0.2` (`--backend=rdp --renderer=gl`, FreeRDP `3.10.3`) | `GRID P4-2Q` (`535.309.01`); `/usr/bin/weston` (`29 MiB` VRAM, `GL renderer: GRID P4-2Q/PCI/SSE2`); `glmark2-wayland` `GRID P4-2Q/PCI/SSE2` (`3557 FPS`, Score `3556`); Vulkan `1.3.242`; live `xfreerdp3` passed. |
| `vm-alma` | AlmaLinux 9.9 (`5.14.0-741.22.1.el9_9.x86_64`) | `192.168.1.68:3389` | Custom `/opt/weston14` (Wayland `1.23.0` + FreeRDP `3.10.3` + Weston `14.0.2` `--renderer=gl`) | `GRID P4-2Q` (`535.309.01`); `/opt/weston14/bin/weston` (`27 MiB` VRAM, `GL renderer: GRID P4-2Q/PCI/SSE2`); `eglinfo -B` Wayland platform = `NVIDIA`; Vulkan `1.3.242`; live `xfreerdp3` passed. |
| `vm-arch` | Arch Linux (`7.3.4-arch1-2`) | `192.168.1.69:3389` | GDM `51.1.1` + `gnome-shell` Wayland (`Type=wayland`) + `gnome-remote-desktop 51.0` `--system` | `GRID P4-2Q` (`535.309.01` patched for `7.3.4`); `gnome-shell` + `Xwayland` on GPU (`42 MiB` VRAM); `eglinfo -B` Wayland = `NVIDIA` (`GRID P4-2Q/PCI/SSE2`, OpenGL `4.6.0` / ES `3.2`); Vulkan `1.3.242`; live `xfreerdp3` handover passed. |

### Distro-Specific Fixes Applied

1. **Ubuntu 24.04 (`192.168.1.66`)**:
   - Masked `/etc/udev/rules.d/61-gdm.rules -> /dev/null` and enabled `WaylandEnable=true` / `PreferredDisplayServer=wayland` in `/etc/gdm3/custom.conf` so GDM uses Wayland instead of falling back to X11 alongside the virtual `bochs` VGA adapter.
2. **Debian 12 (`192.168.1.67`)**:
   - Debian's cloud image defaulted to `linux-image-6.1.0-56-cloud-amd64` which builds with `# CONFIG_DRM is not set` (preventing `/dev/dri/renderD128` and `nvidia-drm modeset=1`). Installed standard `linux-image-amd64` (`6.1.0-56-amd64`, `CONFIG_DRM=m`), switched initramfs compression to `COMPRESS=gzip` (to avoid `zstd` assertion on 2.5G VM RAM), and enabled `weston-rdp` (`weston 14.0.2` `--renderer=gl`).
3. **AlmaLinux 9.9 (`192.168.1.68`)**:
   - Blacklisted `nouveau` at boot (`rd.driver.blacklist=nouveau nouveau.modeset=0` + `/etc/modprobe.d/blacklist-nouveau.conf` + `dracut -f`) to prevent `nouveau` from touching `00:03.0` before `nvidia` (which caused a kernel panic in `nv_start_device` -> `request_threaded_irq` after `Failed to enable MSI`).
   - Built Wayland `1.23.0`, Wayland-Protocols `1.38`, FreeRDP `3.10.3`, and Weston `14.0.2` (`--backend=rdp --renderer=gl`) into `/opt/weston14` because RHEL 9's stock `weston 8.0` lacks `gl-renderer` on RDP and stock `gnome-remote-desktop 40` was built without FreeRDP server support.
4. **Arch Linux (`192.168.1.69`)**:
   - Ported `535.309.01` kernel modules to Linux `7.3.4-arch1-2` + GCC `16.2.1` by adding a `nv_compat_strncpy` inline helper (`common/inc/nv-linux.h`, `nvidia-uvm/uvm_linux.h`, `nvidia-modeset/nvidia-modeset-linux.c`) and updating `nvidia-drm/*.{c,h}` for Linux 7.3's `struct drm_atomic_state` -> `struct drm_atomic_commit` rename (including `drm_atomic_commit_{alloc,init,default_clear,default_release,put}` and `drm_atomic_helper_swap_state(state, false)`).
   - Blacklisted `nouveau` in GRUB (`modprobe.blacklist=nouveau nouveau.modeset=0`) + `mkinitcpio -P` to prevent the same boot-time `irq_enable` oops.
   - Configured GDM 51 + `gnome-remote-desktop 51` (`--system`) with `61-gdm.rules` masked for native NLA/CredSSP RDP (`zero` / `<lab-password>`) and Wayland GPU acceleration.

---

## 4. Windows VM Status

- **Gap**: No Windows installation ISO exists on the local workspace (`<local-workspace>`), on `.15`, or on `.63`, and Microsoft's Evaluation Center requires interactive form submission/download outside automated `curl`.
- **Guest Driver Ready**: `NVIDIA-GRID-Linux-KVM-535.309.01-539.72\Guest_Drivers\539.72_grid_win10_win11_server2019_server2022_dch_64bit_international.exe` (`529,598,456` bytes) is verified present locally and pairs directly with the `535.309.01-vgpu-kvm` host driver on `.63`. Once a Windows 10/11/Server 2022 ISO is placed on `.63` (`/home/zero/vms/images/`), a fifth `vm-windows` QEMU guest on `br0` can be installed using a `GRID P4-2Q` slice (stopping one Linux VM or switching profiles to `P4-1Q` to fit 5 VMs in 8 GiB VRAM).
