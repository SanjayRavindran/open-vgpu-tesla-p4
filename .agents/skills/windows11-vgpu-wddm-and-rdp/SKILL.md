---
name: windows11-vgpu-wddm-and-rdp
description: >-
  Configure and troubleshoot Windows 11 24H2 WDDM 3.2 (582.99 WHQL) on Tesla P4
  GRID P4-4Q (192.168.1.74 DHCP / 192.168.1.65 fallback), including PCI
  SUBSYS_11D810DE driver binding, FastAPI-DLS licensing with
  gridd-unlock-patcher (nvxdapix.dll 9594600 B), NVIDIA Control Panel and GPU-Z
  verification, fixing RDP Error code 0x200d (RDP_GFX 0x8007007A), and managing
  Terminal Services via WinRM. Use this skill when working on the Windows 11
  vGPU VM.
---

# Windows 11 24H2 (`582.99` WHQL), `gridd-unlock-patcher` & RDP Operations

## Remote Management via WinRM from Host (`192.168.1.24`)
Use `/home/zero/wenv/bin/python` with `pywinrm` (`transport='ntlm'`, `http://192.168.1.74:5985/wsman` via DHCP or `192.168.1.65` if static IP has no LAN collision, user `zero`, password `<YOUR_WINRM_PASSWORD>`).
- Windows driver installer `582.99_grid_win10_win11_server2022_server_2025_dch_64bit_international.exe` and `gridd-unlock-patcher` are staged in `.24:/home/zero/guest/`.
- **LAN IP Note**: If another device on the LAN claims `192.168.1.65`, Windows falls back to APIPA (`169.254.x.x`) when pinned statically. Switching `Ethernet` to DHCP cleanly resolves this (`192.168.1.74` on lab router; verify current lease with `arp -n` / `ip neigh` on `.24`).

## Procedures & Troubleshooting

### 1. Bind `582.99` WHQL Driver (`PCI\VEN_10DE&DEV_1BB3&SUBSYS_11D810DE`)
- R580 (`582.99`) `nvgridsw.inf` removed the legacy `SUBSYS_1204..120B` `GRID P4-*` entries while retaining WHQL-signed **`PCI\VEN_10DE&DEV_1BB3&SUBSYS_11D810DE`** (`Section003`, `"NVIDIA Tesla P4"`, containing all vGPU and MFT components).
- Ensure `.24:/usr/share/nvidia/vgpu/vgpuConfig.xml` sets `subsystemId="0x11D8"` on `GRID P4-*` profiles (automated via `tools/patch_r580_vgpu_config_xml.py`) so Windows enumerates `SUBSYS_11D810DE` natively without breaking INF WHQL signatures (preserving Secure Boot).
```powershell
$inf = (Get-ChildItem "C:\Windows\System32\DriverStore\FileRepository\nvgridsw.inf_amd64_*\nvgridsw.inf" |
  Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
pnputil /add-driver $inf /install
Get-PnpDevice -Class Display -PresentOnly | Select-Object Status, FriendlyName, InstanceId
```
*(Always sort `nvgridsw.inf_amd64_*` by `LastWriteTime -Descending` so old `539.72` DriverStore entries are not picked first!)*

### 2. Patch `582.99` `nvxdapix.dll` with `gridd-unlock-patcher` for `fastapi-dls`
Closed R570/R580 Windows GRID drivers pin NVIDIA's licensing CA inside `nvxdapix.dll`:
1. On the `.24` host, extract `Display.Driver/nvxdapix.dll` (`9594600` B in `582.99`), fetch the FastAPI-DLS Root CA, and patch it using `/home/zero/guest/gridd-unlock-patcher`:
   ```bash
   7z x -y -o/tmp/win582 /home/zero/guest/582.99_grid_win10_win11_server2022_server_2025_dch_64bit_international.exe Display.Driver/nvxdapix.dll
   curl -sk https://192.168.1.24/-/config/root-certificate -o /tmp/dls_root_ca.pem
   cp /tmp/win582/Display.Driver/nvxdapix.dll /tmp/nvxdapix_patched.dll
   /home/zero/guest/gridd-unlock-patcher/gridd-unlock-patcher -g /tmp/nvxdapix_patched.dll -c /tmp/dls_root_ca.pem
   ```
2. Automated on Windows via `scripts\provision_win11_guest.ps1 -DlsUrl https://192.168.1.24 -PatchedNvxdapixPath C:\temp\nvxdapix_patched.dll`, which stops `NVDisplay.ContainerLocalSystem`, targets ONLY the matching `582.99` DriverStore folder (`Length == 9594600` / newest `LastWriteTime`), runs `takeown` + `icacls`, backs up `.orig`, copies the patched DLL, refreshes the token, sets `FeatureType=1`, and restarts `NVDisplay.ContainerLocalSystem` (`License Status : Licensed`).
3. If QEMU OVMF stalls during a Windows warm reboot, perform a clean cold VM stop/start (`qmp quit` -> `start_vgpu_vms.sh`).

### 3. Launch NVIDIA Control Panel & TechPowerUp GPU-Z
In DCH drivers (`582.99`), NVIDIA Control Panel is an AppX package (`NVIDIACorp.NVIDIAControlPanel_*_x64__56jybvy8sckqj`) and does not place `nvcplui.exe` in `C:\Program Files\NVIDIA Corporation\Control Panel Client`.
Launch via its AppX AUMID:
```powershell
Start-Process "explorer.exe" "shell:AppsFolder\NVIDIACorp.NVIDIAControlPanel_56jybvy8sckqj!NVIDIAControlPanel"
Start-Process "C:\Users\Public\Desktop\GPU-Z.exe"
```

### 4. Fix RDP `Error code: 0x200d` (`DecompressionFailed` / `RDP_GFX 0x8007007A`) & Reconnect Delays
- **Cause of `0x200d`**: When a high-DPI Retina macOS RDP client dynamically requests `3600 × 2262` (`RdpCoreTS/Operational` Event `168`) while `AVCHardwareEncodePreferred=1` forces the `NVIDIA H.264 Encoder MFT` on the RDP transport stream, `RDP_GFX` overflows with `0x8007007A` (`ERROR_INSUFFICIENT_BUFFER`).
- **Fix**:
  1. Remove `AVCHardwareEncodePreferred` / `AVC444ModePreferred` overrides under `HKLM:\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services`:
     ```powershell
     Remove-ItemProperty -Path 'HKLM:\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services' `
       -Name 'AVCHardwareEncodePreferred','AVC444ModePreferred','bEnumerateHWAdapters','fEnableHardwareMode' -ErrorAction SilentlyContinue
     ```
  2. Set `.rdp` file to `desktopwidth:i:1920`, `desktopheight:i:1080`, `dynamic resolution:i:0`, `smart sizing:i:1`.
  3. Enable NLA (`UserAuthentication=1` under `HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations\rdp-tcp`) so RDP attaches directly to `zero`'s desktop session without creating a temporary pre-logon session (`"Other user — Welcome"`).
  4. **Never** run `Restart-Service TermService -Force` on a live Windows machine (it breaks `LocalSessionManager`'s service handle with `reason code 12`); reboot cleanly if `TermService` must be restarted.
