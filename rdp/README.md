# RDP Connection Profiles (`mstsc.exe` / FreeRDP3) — 2-VM 50/50 `GRID P4-4Q` Layout

Pre-configured `.rdp` connection profiles for the 2 active Tesla P4 vGPU VMs (`1920x1080`, 32-bit color, LAN `connection type:i:6` for H.264/AVC444 hardware encoding, dynamic resolution, clipboard, and audio redirection) on the UEFI Coreboot host (`192.168.1.24`).

> [!NOTE]
> No passwords are stored in these files. Each VM receives 50% CPU (`4 vCPU`) and 50% GPU (`GRID P4-4Q`, `4096 MiB` VRAM).

| Profile | Guest OS | Default Lab IP | vGPU / CPU / RAM | Hardware RDP & Web Streaming |
| :--- | :--- | :--- | :--- | :--- |
| [`vm-win11.rdp`](./vm-win11.rdp) | Windows 11 24H2 Pro (`539.72` WHQL, WDDM `3.1`) | `192.168.1.65:3389` | `GRID P4-4Q` (`4096 MiB`), `4 vCPU`, `6 GiB` RAM | `NVIDIA H.264 Encoder MFT` (AVC444), GPU-Z `2.71.0` & NVIDIA Control Panel `8.1.969.0` |
| [`vm-alma.rdp`](./vm-alma.rdp) | AlmaLinux 10.3 GNOME `49.5` Pure Wayland (`--no-x11`) | `192.168.1.68:3389` | `GRID P4-4Q` (`4096 MiB`), `4 vCPU`, `5 GiB` RAM | GNOME Remote Desktop CUDA + NVENC (`AVC444`) + Selkies/PixelFlux `10-bit 4:4:4` (`https://192.168.1.68:3001`) |
