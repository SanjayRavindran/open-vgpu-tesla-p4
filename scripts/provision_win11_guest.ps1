[CmdletBinding()]
param(
    [string]$DlsUrl = "https://192.168.1.24",
    [string]$DriverInstallerPath = "",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

Write-Host "=== Windows 11 24H2 WDDM 3.1 + FastAPI-DLS + RDP Provisioning (DlsUrl=$DlsUrl) ==="

if ($DryRun) {
    Write-Host "[dry-run] 1. Install vGPU WDDM driver ($DriverInstallerPath) or bind nvgridsw.inf via pnputil"
    Write-Host "[dry-run] 2. Fetch client token from $DlsUrl/-/client-token -> C:\Program Files\NVIDIA Corporation\vGPU Licensing\ClientConfigToken"
    Write-Host "[dry-run] 3. Set FeatureType=1 under HKLM:\SOFTWARE\NVIDIA Corporation\Global\GridLicensing and restart NVDisplay.ContainerLocalSystem"
    Write-Host "[dry-run] 4. Clear AVCHardwareEncodePreferred / AVC444ModePreferred policies under Terminal Services (fixes 0x200d / 0x8007007A)"
    Write-Host "[dry-run] 5. Enable NLA UserAuthentication=1 and Windows Firewall Remote Desktop rule (without restarting live TermService)"
    exit 0
}

# 1. Install or rebind 539.72 WHQL GRID WDDM driver for SUBSYS_120610DE (GRID P4-4Q)
if ($DriverInstallerPath -and (Test-Path $DriverInstallerPath)) {
    Write-Host "Installing NVIDIA vGPU WDDM driver from $DriverInstallerPath..."
    Start-Process -FilePath $DriverInstallerPath -ArgumentList "-s", "-noreboot", "-noeula" -Wait
} else {
    $inf = (Get-ChildItem "C:\Windows\System32\DriverStore\FileRepository\nvgridsw.inf_amd64_*\nvgridsw.inf" -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
    if ($inf) {
        Write-Host "Binding DriverStore INF: $inf"
        & pnputil.exe /add-driver $inf /install
    }
}

# 2. Fetch FastAPI-DLS client configuration token and set GridLicensing FeatureType=1
$tokenDir = "C:\Program Files\NVIDIA Corporation\vGPU Licensing\ClientConfigToken"
New-Item -ItemType Directory -Force -Path $tokenDir | Out-Null
Remove-Item "$tokenDir\*.tok" -Force -ErrorAction SilentlyContinue
$stamp = Get-Date -Format "dd-MM-yy-HH-mm-ss"
$tokFile = Join-Path $tokenDir "client_configuration_token_$stamp.tok"
& curl.exe --insecure -fsSL -X GET "$DlsUrl/-/client-token" -o $tokFile

$gridReg = "HKLM:\SOFTWARE\NVIDIA Corporation\Global\GridLicensing"
New-Item -Path $gridReg -Force | Out-Null
Set-ItemProperty -Path $gridReg -Name "FeatureType" -Type DWord -Value 1
if (Get-Service -Name "NVDisplay.ContainerLocalSystem" -ErrorAction SilentlyContinue) {
    Restart-Service -Name "NVDisplay.ContainerLocalSystem" -Force
}

# 3. Clear RDP_GFX 0x200d (0x8007007A) offending hardware-AVC policy overrides
$tsPol = "HKLM:\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services"
if (Test-Path $tsPol) {
    Remove-ItemProperty -Path $tsPol -Name "AVCHardwareEncodePreferred", "AVC444ModePreferred", "bEnumerateHWAdapters", "fEnableHardwareMode" -ErrorAction SilentlyContinue
}

# 4. Enable RDP + NLA (do NOT call Restart-Service TermService -Force on a live desktop!)
Set-ItemProperty -Path "HKLM:\System\CurrentControlSet\Control\Terminal Server" -Name "fDenyTSConnections" -Type DWord -Value 0
Set-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations\rdp-tcp" -Name "UserAuthentication" -Type DWord -Value 1
Enable-NetFirewallRule -DisplayGroup "Remote Desktop" -ErrorAction SilentlyContinue

Write-Host "=== Verification ==="
Get-PnpDevice -Class Display -PresentOnly | Select-Object Status, FriendlyName, InstanceId | Format-Table -AutoSize
$nvsmi = "C:\Windows\System32\nvidia-smi.exe"
if (Test-Path $nvsmi) {
    & $nvsmi -q | Select-String -Pattern "Product Name|Driver Version|License Status|Expiry"
}
Write-Host "Note: Reboot VM if Terminal Services policies were updated."
