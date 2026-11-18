# RDP Connection Profiles (`mstsc.exe` / FreeRDP3)

Pre-configured `.rdp` connection profiles for the 5 concurrent Tesla P4 vGPU VMs (`1920x1080`, 32-bit color, LAN `connection type:i:6` for H.264/AVC444 hardware encoding, dynamic resolution, clipboard, and audio redirection).

> [!NOTE]
> No passwords are stored in these files. Update `full address:s:...` and `username:s:...` to match your LAN IP and VM account.

| Profile | Guest OS | Default Lab IP | Hardware RDP Encoder |
| :--- | :--- | :--- | :--- |
| [`vm-win11.rdp`](./vm-win11.rdp) | Windows 11 24H2 Pro (`539.72` WHQL) | `192.168.1.65:3389` | `NVIDIA H.264 Encoder MFT` (AVC444) |
| [`vm-ubuntu.rdp`](./vm-ubuntu.rdp) | Ubuntu 24.04 GNOME `46.0` Pure Wayland | `192.168.1.66:3389` | GNOME Remote Desktop CUDA + NVENC (AVC444) |
| [`vm-debian.rdp`](./vm-debian.rdp) | Debian 13.7 GNOME `48.7` Pure Wayland | `192.168.1.67:3389` | GNOME Remote Desktop CUDA + NVENC (AVC444) |
| [`vm-alma.rdp`](./vm-alma.rdp) | AlmaLinux 10.3 GNOME `49.5` Pure Wayland | `192.168.1.68:3389` | GNOME Remote Desktop CUDA + NVENC (AVC444) |
| [`vm-arch.rdp`](./vm-arch.rdp) | Arch Linux GNOME `51.1` Pure Wayland | `192.168.1.69:3389` | GNOME Remote Desktop CUDA + NVENC (AVC444) |

### Connecting from Linux (FreeRDP3 with AVC444)
```bash
xfreerdp3 /v:192.168.1.65 /u:zero /gfx:AVC444 /dynamic-resolution /cert:ignore
```
