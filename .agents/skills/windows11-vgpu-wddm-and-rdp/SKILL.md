---
name: windows11-vgpu-wddm-and-rdp
description: >-
  Configure and troubleshoot Windows 11 24H2 WDDM 3.1 (539.72 WHQL) on Tesla P4
  GRID P4-4Q (192.168.1.65), including PCI SUBSYS_120610DE driver binding,
  FastAPI-DLS licensing, AppX NVIDIA Control Panel and GPU-Z verification,
  fixing RDP Error code 0x200d (RDP_GFX 0x8007007A), and managing Terminal
  Services via WinRM. Use this skill when working on the Windows 11 vGPU VM.
---

# Windows 11 24H2 WDDM 3.1, GPU-Z, NVIDIA Control Panel & RDP Operations (`192.168.1.65`)

## Remote Management via WinRM from Host (`192.168.1.24`)
Use `/home/zero/wenv/bin/python` with `pywinrm` (`transport='ntlm'`, `http://192.168.1.65:5985/wsman`, user `zero`, password `<YOUR_WINRM_PASSWORD>`).

## Procedures & Troubleshooting

### 1. Bind `539.72` WHQL Driver When Changing vGPU Profile (`SUBSYS_120610DE` for `GRID P4-4Q`)
If the vGPU profile changes (e.g. `1Q` `SUBSYS_120410DE` -> `4Q` `SUBSYS_120610DE`), Windows enumerates the new PCI Subsystem ID as an unbound `Video Controller (VGA Compatible)`:
```powershell
$inf = (Get-ChildItem "C:\Windows\System32\DriverStore\FileRepository\nvgridsw.inf_amd64_*\nvgridsw.inf" | Select-Object -First 1).FullName
pnputil /add-driver $inf /install
Get-PnpDevice -Class Display -PresentOnly | Select-Object Status, FriendlyName, InstanceId
```

### 2. Launch NVIDIA Control Panel (`8.1.969.0`) & TechPowerUp GPU-Z
In R535/R539 DCH drivers, NVIDIA Control Panel is an AppX package (`NVIDIACorp.NVIDIAControlPanel_8.1.969.0_x64__56jybvy8sckqj`) and does not place `nvcplui.exe` in `C:\Program Files\NVIDIA Corporation\Control Panel Client`.
Launch via its AppX AUMID:
```powershell
Start-Process "explorer.exe" "shell:AppsFolder\NVIDIACorp.NVIDIAControlPanel_56jybvy8sckqj!NVIDIACorp.NVIDIAControlPanel"
Start-Process "C:\Users\Public\Desktop\GPU-Z.exe"
```

### 3. Fix RDP `Error code: 0x200d` (`DecompressionFailed` / `RDP_GFX 0x8007007A`) & Reconnect Delays
- **Cause of `0x200d`**: When a high-DPI Retina macOS RDP client dynamically requests `3600 × 2262` (`RdpCoreTS/Operational` Event `168`) while `AVCHardwareEncodePreferred=1` forces the `NVIDIA H.264 Encoder MFT` on the RDP transport stream, `RDP_GFX` overflows with `0x8007007A` (`ERROR_INSUFFICIENT_BUFFER`).
- **Fix**:
  1. Remove `AVCHardwareEncodePreferred` / `AVC444ModePreferred` overrides under `HKLM:\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services`:
     ```powershell
     Remove-ItemProperty -Path 'HKLM:\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services' `
       -Name 'AVCHardwareEncodePreferred','AVC444ModePreferred','bEnumerateHWAdapters','fEnableHardwareMode' -ErrorAction SilentlyContinue
     ```
  2. Set `.rdp` file to `desktopwidth:i:1920`, `desktopheight:i:1080`, `dynamic resolution:i:0`, `smart sizing:i:1`.
  3. Enable NLA (`UserAuthentication=1` under `HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations\rdp-tcp`) so RDP attaches directly to `zero`'s desktop session without creating a temporary pre-logon session (`"Other user — Welcome"`).
  4. **Never** run `Restart-Service TermService -Force` on a live Windows machine (it breaks `LocalSessionManager`'s service handle with `reason code 12`); reboot the VM cleanly if `TermService` must be restarted.

### 4. Activate vGPU License Against `https://192.168.1.24:443`
```powershell
$tokenDir = "C:\Program Files\NVIDIA Corporation\vGPU Licensing\ClientConfigToken"
New-Item -ItemType Directory -Force -Path $tokenDir | Out-Null
Remove-Item "$tokenDir\*.tok" -Force -ErrorAction SilentlyContinue
curl.exe --insecure -L -X GET https://192.168.1.24/-/client-token `
  -o "$tokenDir\client_configuration_token_$($(Get-Date).tostring('dd-MM-yy-hh-mm-ss')).tok"
Restart-Service NVDisplay.ContainerLocalSystem -Force
```
