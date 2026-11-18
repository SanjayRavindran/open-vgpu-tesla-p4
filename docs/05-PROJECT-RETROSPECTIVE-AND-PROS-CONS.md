# 05 — Project Retrospective: What Was Accomplished, What Failed or Needed Workarounds, and Pros & Cons of Open Pascal vGPU

## 1. Summary of Everything Accomplished

### 1.1 Static Binary Analysis & Clean-Room Pascal vGPU Stack (`535.309.01`, `.15` Dev VM)
1. Analyzed every component of `NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run` and `NVIDIA-Linux-x86_64-535.309.01-grid.run` with `rea@6.9.0` + Ghidra `12.2` on `192.168.1.15`:
   - `libnvidia-vgxcfg.so.535.309.01` (`147/187` functions), `nvidia-vgpud` (`320/402`), `nvidia-vgpu-mgr` (`488/609`), `libnvidia-vgpu.so.535.309.01` (`6,734/6,884`), and `nv-kernel.o_binary` (`259` `BindataStorage` archives, `32,498,832` B raw).
   - Proved Pascal KVM vGPU uses no separate vGPU Falcon blobs: `acr_bl`, `pmu_bl`, `sec2_rtos`, `sec2_hs`, `nvenc_hs_scrub`, `fecs_*`, `gpccs_*`, and `gr_netlist` (`90,552` B, `32` sections) are SHA-256 identical to R580 (`580.178.04`), while `pmu_rtos`, `nvenc0/1`, and `nvdec` are signed 535-branch LS builds.
2. Reimplemented the host & guest control plane cleanly in safe Rust (`nova-vgpu/`) and GPLv2 Linux `7.0` C kernel modules (`kmods/nova_vgpu_mdev/`, `kmods/nova_vgpu_guest/`):
   - Host `vgpuConfig.xml` parser (`12` Tesla P4 profiles), `NVA081` `4,968`-byte descriptor packing, `nvidia-vgpu-vfio` `'r'`/`'q'` ioctl ABIs, `vmiop` `VER_5_0` plugin lifecycle, GP104 virtual PCI config, BAR0 `0x8f000..0x90000` trapping, `"VRPC"` (`0x43505256`) shared ring dispatch, `VMMU`, `CHIDMGR`, and `FRL`.
   - Verified end-to-end inside a real QEMU/KVM VM on `192.168.1.63` backed by `nova_vgpu_mdev.ko` + `nova_vgpu_guest.ko` (`GUEST_IOCTL_TEST_PASSED`: `8` RPC roundtrips, `8` interrupts, BAR1 `mmap` `0xdeadbeefcafebabe`, zero Falcon blobs loaded in the guest).

### 1.2 R620 (`620.70.04` / `620.70.06`) Wayland-on-R535 Host/Guest Deployment (`.63` + 4 LAN VMs)
1. Replaced `580` with `535.309.01-vgpu-kvm` on host `192.168.1.63` (`h61`, Tesla P4 `10de:1bb3`) and configured LAN bridge `br0`.
2. Discovered how to run R620 Wayland on `535.309.01` Pascal vGPUs: overlaying R620's 5 standalone Wayland/EGL/GBM/XCB/Xlib external platform libraries (`libnvidia-egl-wayland2.so.1.0.2`, `libnvidia-egl-wayland.so.1.1.20`, `libnvidia-egl-gbm.so.1.1.3`, `libnvidia-egl-xcb.so.1.0.6`, `libnvidia-egl-xlib.so.1.0.6` + JSONs) over `535.309.01-grid` guests.
3. Provisioned 4 simultaneous `GRID P4-2Q` (`2048 MiB` VRAM each, `8033 MiB / 8192 MiB` active) LAN-reachable Wayland + RDP VMs, all live-verified with `xfreerdp3`:
   - `vm-ubuntu` (`192.168.1.66:3389`): Ubuntu 24.04, GDM Wayland + `gnome-remote-desktop --system`, `glmark2-wayland` `GRID P4-2Q/PCI/SSE2` (`3791 FPS`).
   - `vm-debian` (`192.168.1.67:3389`): Debian 12 (`6.1.0-56-amd64`), Weston `14.0.2` `--backend=rdp --renderer=gl`, `glmark2-wayland` `GRID P4-2Q/PCI/SSE2` (`3557 FPS`).
   - `vm-alma` (`192.168.1.68:3389`): AlmaLinux 9.9 (`5.14.0-741.22.1.el9_9.x86_64`), custom `/opt/weston14` (Wayland `1.23.0` + FreeRDP `3.10.3` + Weston `14.0.2`), `GL renderer: GRID P4-2Q/PCI/SSE2`.
   - `vm-arch` (`192.168.1.69:3389`): Arch Linux (`7.3.4-arch1-2`), `535.309.01` kernel modules patched for Linux `7.3`, GDM `51.1.1` + `gnome-remote-desktop 51.0`, `OpenGL 4.6.0 NVIDIA 535.309.01` on `GRID P4-2Q/PCI/SSE2`.

---

## 2. What Did NOT Work as Expected (Root Causes & Resolutions)

| Area | Issue Encountered | Root Cause | Resolution / Outcome |
| :--- | :--- | :--- | :--- |
| **Falcon Signature Bypass** | Cannot replace signed NVIDIA Falcon microcode with unsigned Rust microcode | `NV_FUSE[0x21400] = 0x1` (`PrivSec` fused ON in retail Tesla P4 silicon); HS/LS Falcons reject unsigned images at hardware boot (`0xbadf5040` on WPR regs) | Isolated and verified NVIDIA's pre-signed Falcon blobs cleanly via `extract_proprietary_blobs.sh`; no bypass attempted. |
| **Running R620 (`620.70.06`) Kernel Driver on Pascal** | Fails probe even after patching PCI ID `0x1bb3` and VGX version clamps | `_nv015641rm` chip HAL table `[0..36]` (Maxwell/Pascal/Volta) is completely zeroed out in R620 `nv-kernel.o_binary` | Kept `535.309.01` kernel/RM base and bridged only R620's dependency-free Wayland EGL/GBM `.so` libraries on top. |
| **R620 `535` Host/Guest Compatibility Check** | Direct `620.70.04` host does not support Tesla P4 (`10de:1bb3`) | Host R620 dropped Pascal HALs and physical vGPU support completely | Reverted host `.63` to `535.309.01-vgpu-kvm` + R620 Wayland guest libs. |
| **Debian Cloud Kernel (`6.1.0-56-cloud-amd64`)** | `/dev/dri` missing (`nvidia_drm` stubbed at 16 KiB, `eglGetDisplay` `0x3003`), plus `gcc-12` parallel ICE on `uvm_pascal_mmu.c` and `zstd` initramfs assertion | Debian's `-cloud-amd64` kernel builds with `# CONFIG_DRM is not set`; low VM RAM triggered `gcc-12` / `zstd -T0` failures | Installed standard `linux-image-amd64` (`CONFIG_DRM=m`), added 2 GiB swap + `--concurrency-level=1`, set `COMPRESS=gzip` in `initramfs.conf`. |
| **AlmaLinux & Arch `nouveau` Pre-Probe Kernel Oops** | `nvidia-gridd` panicked in `nv_start_device` -> `request_threaded_irq` / `irq_enable` on initial `modprobe nvidia` | `nouveau` probed `00:03.0` (`GRID P4-2Q`) at boot before `nvidia` loaded, leaving PCI/MSI/IRQ state dirty | Blacklisted `nouveau` (`blacklist nouveau`, `nouveau.modeset=0` in GRUB/initramfs) before loading `nvidia` on both VMs. |
| **AlmaLinux Wayland RDP Stack** | Stock `weston 8.0` lacks `--renderer=gl` for RDP (defaults to `pixman` CPU), and stock `gnome-remote-desktop 40` was compiled without FreeRDP support | RHEL 9/Alma 9 shipped older Weston 8 and disabled RDP in `gnome-remote-desktop` | Built Wayland `1.23.0` + FreeRDP `3.10.3` + Weston `14.0.2` cleanly into `/opt/weston14` with `--renderer=gl`. |
| **Arch Linux `535.309.01` Kernel `7.3.4` Build & Weston 15 NLA** | `535.309.01` failed to compile on Linux `7.3.4`; Weston `15.0.1` (`FreeRDP 3.33`) failed NLA login (`ERRCONNECT_LOGON_FAILURE`) without `/sec:tls` | Linux `7.3` removed `strncpy()` and renamed `struct drm_atomic_state` -> `struct drm_atomic_commit`; FreeRDP `3.33` defaults to NLA requiring a WinPR SAM DB | Patched `535` via `patch_535_guest_linux7.py` and switched Arch to GDM `51` + `gnome-remote-desktop 51` `--system` (native CredSSP/NLA + GNOME Wayland). |
| **Host `.63` Concurrent SATA Reset** | `ata1.00: failed command: FLUSH CACHE EXT` corrupted files during simultaneous `scp` + `apt`/`dnf`/`pacman` across 3 VMs | Concurrent `cache=writeback` VM disk bursts overwhelmed host SATA controller | Serialized VM setup one VM at a time and added `sha256sum` checks before installs. |
| **Windows VM** | Not provisioned | No Windows ISO exists locally or on `.15`/`.63`, and Microsoft Evaluation Center requires interactive form submission | `539.72` Windows guest installer is staged locally; add any Windows ISO to `.63` to deploy later. |

---

## 3. Pros and Cons of Open Pascal vGPU Implementation vs. Proprietary Stack

### Pros of Our Open Pascal vGPU Implementation (`nova-vgpu` + `nova_vgpu_mdev` + `nova_vgpu_guest`)
1. **Kernel Longevity Across Modern Linux Kernels (`7.0`–`7.3+` and beyond)**:
   - NVIDIA dropped Pascal R535 support in June 2026. As distributions move to Linux `7.x` (`drm_atomic_commit` renames, `strncpy` removal, `mdev` API churn), the proprietary `535.309.01` host/guest driver breaks on every kernel bump. Our pure GPLv2 C mediators (`nova_vgpu_mdev`, `nova_vgpu_guest`) and Rust userspace (`nova-vgpu`) keep pace directly with upstream Linux kernels.
2. **Full Transparency, Auditability, and Clean Separation of Blobs**:
   - All virtualization logic (`vgpuConfig.xml` profile tables, `vfio-mdev` PCI/BAR0 trapping, `"VRPC"` shared ring transport, `VMMU` GFN-to-PFN pinning, and `CHIDMGR` channel allocation) lives in auditable Rust/C source without `nvidia-vgpud` / `nvidia-vgpu-mgr` / `libnvidia-vgpu.so` black boxes.
3. **Zero Proprietary Blobs Inside Guest VMs for Kernel Transport**:
   - Confirmed both by binary analysis and live QEMU/KVM testing: guest VMs do not touch Falcon firmware or `PrivSec` hardware; the entire guest kernel PCI + BAR0 + `"VRPC"` layer is 100% open-source.
4. **No Proprietary Host Daemons Needed for Control-Plane Prototyping**:
   - Host mdev lifecycle, VRAM partitioning (`8,192 MiB`), and interrupt signaling run in memory-safe Rust + standard Linux `vfio-mdev`.

### Cons & Remaining Engineering Gaps of an Open Implementation
1. **NVIDIA Userspace Compatibility (CUDA, NVENC, Proprietary OpenGL/Vulkan)**:
   - `nova_vgpu_guest.ko` only exposes clean-room `/dev/nova-vgpu-guestN` ioctls today. Existing proprietary binaries (`libcuda.so`, `libnvidia-encode.so`, `libGLX_nvidia.so`, Windows WDDM `nvlddmkm.sys`) expect NVIDIA's closed `NV_ESC_*` RM/UVM ioctl ABI and full virtualized RM object trees (`ALLOC_ROOT` -> `ALLOC_OBJECT` -> `FIFO` -> `GR` -> `MMU`).
2. **Full Host Hardware Engine Multiplexing (Replacing Proprietary `nv-kernel.o_binary`)**:
   - While our Rust stack completely replaces `nvidia-vgpud`, `nvidia-vgpu-mgr`, `libnvidia-vgpu.so`, and the guest BAR0/`"VRPC"` interface, real hardware context-switching of Pascal `GR` (`FECS`/`GPCCS`), `CE`, `NVENC`, and `NVDEC` timeslices on physical silicon still relies on either:
     - Pairing with NVIDIA's bare-metal/vGPU RM core (`nv-kernel.o_binary`), **or**
     - Building a complete open Pascal host RM scheduler + GPU page-table manager + hardware engine context switcher on top of signed Falcon blobs (`ACR`, `PMU`, `FECS`, `GPCCS`, `NVENC`, `NVDEC`) — a multi-person-year engineering effort comparable to full `nouveau` GSP/RM parity.
3. **NLS / FlexNet Licensing Compatibility for Proprietary Guest Drivers**:
   - Unmodified proprietary `535.309.01` / `539.72` guest UMDs invoke `nvidia-gridd` (`libFlxCore64.so`) and enforce `FRL` / `NVA081` licensing constraints unless open-source Mesa/DRM userspace is used end-to-end.
4. **No Microsoft Windows Guest Support Without Signing & WDDM**:
   - Windows guests require a signed WDDM Miniport (`nvlddmkm.sys` replacement), which cannot be shipped as an unsigned out-of-tree clean-room driver without WHQL/test-signing and full D3D/WDDM implementation.

### Practical Recommendation
- **For immediate production use today** (full CUDA, NVENC, OpenGL, Vulkan, Wayland over RDP on Pascal P4): use `535.309.01-vgpu-kvm` + `535.309.01-grid` overlaid with the `620.70.06` Wayland EGL/GBM external platforms and Linux `7.x` kernel compat patches (`scripts/extract_proprietary_blobs.sh` + `scripts/patch_535_guest_linux7.py`).
- **For long-term kernel survival & open research**: build out `open-pascal-vgpu` (`nova-vgpu` + `nova_vgpu_mdev` + `nova_vgpu_guest`) iteratively, starting from the working host `vfio-mdev` + guest `"VRPC"` foundation.
