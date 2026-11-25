[CmdletBinding()]
param(
    [string]$DlsUrl = "https://192.168.1.24",
    [string]$DriverInstallerPath = "",
    [string]$PatchedNvxdapixPath = "",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

Write-Host "=== Windows 11 24H2 R580 (582.99 WHQL) + FastAPI-DLS + RDP Provisioning (DlsUrl=$DlsUrl) ==="

if ($DryRun) {
    Write-Host "[dry-run] 1. Install 582.99 WHQL vGPU driver ($DriverInstallerPath) or bind newest nvgridsw.inf for SUBSYS_11D810DE"
    Write-Host "[dry-run] 2. Optionally install patched nvxdapix.dll ($PatchedNvxdapixPath) into matching DriverStore nvgridsw.inf_amd64_* folder"
    Write-Host "[dry-run] 3. Fetch client token from $DlsUrl/-/client-token -> C:\Program Files\NVIDIA Corporation\vGPU Licensing\ClientConfigToken"
    Write-Host "[dry-run] 4. Set FeatureType=1 under HKLM:\SOFTWARE\NVIDIA Corporation\Global\GridLicensing and restart NVDisplay.ContainerLocalSystem"
    Write-Host "[dry-run] 5. Clear AVCHardwareEncodePreferred / AVC444ModePreferred policies under Terminal Services (fixes 0x200d / 0x8007007A)"
    Write-Host "[dry-run] 6. Enable NLA UserAuthentication=1 and Windows Firewall Remote Desktop rule (without restarting live TermService)"
    exit 0
}

# 1. Install or rebind 582.99 WHQL GRID WDDM driver for SUBSYS_11D810DE (GRID P4-4Q)
if ($DriverInstallerPath -and (Test-Path $DriverInstallerPath)) {
    Write-Host "Installing NVIDIA R580 WDDM driver from $DriverInstallerPath..."
    Start-Process -FilePath $DriverInstallerPath -ArgumentList "-s", "-noreboot", "-noeula" -Wait
} else {
    $inf = (Get-ChildItem "C:\Windows\System32\DriverStore\FileRepository\nvgridsw.inf_amd64_*\nvgridsw.inf" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
    if ($inf) {
        Write-Host "Binding newest DriverStore INF: $inf"
        & pnputil.exe /add-driver $inf /install
    }
}

# 2. Optional gridd-unlock-patcher nvxdapix.dll replacement in matching 582.99 DriverStore folder
if ($PatchedNvxdapixPath -and (Test-Path $PatchedNvxdapixPath)) {
    $patchedLen = (Get-Item $PatchedNvxdapixPath).Length
    $svcRunning = (Get-Service -Name "NVDisplay.ContainerLocalSystem" -ErrorAction SilentlyContinue |
        Where-Object { $_.Status -eq "Running" })
    if ($svcRunning) {
        Stop-Service -Name "NVDisplay.ContainerLocalSystem" -Force
    }
    $candidates = Get-ChildItem "C:\Windows\System32\DriverStore\FileRepository\nvgridsw.inf_amd64_*\nvxdapix.dll" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending
    $targetDll = ($candidates | Where-Object { $_.Length -eq $patchedLen } | Select-Object -First 1)
    if (-not $targetDll) {
        $targetDll = ($candidates | Select-Object -First 1)
    }
    if ($targetDll) {
        $origBak = "$($targetDll.FullName).orig"
        & takeown.exe /F $targetDll.FullName | Out-Null
        & icacls.exe $targetDll.FullName /grant Administrators:F | Out-Null
        if (-not (Test-Path $origBak)) {
            Copy-Item -Path $targetDll.FullName -Destination $origBak -Force
        }
        Copy-Item -Path $PatchedNvxdapixPath -Destination $targetDll.FullName -Force
        Write-Host "Installed patched nvxdapix.dll -> $($targetDll.FullName)"
    } else {
        Write-Warning "No DriverStore nvxdapix.dll found under nvgridsw.inf_amd64_*"
    }
}

# 3. Fetch FastAPI-DLS client configuration token and set GridLicensing FeatureType=1
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

# 4. Disable virtual VGA Microsoft Basic Display Adapter if present alongside GRID P4-4Q
Get-PnpDevice -Class Display -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.FriendlyName -like "*Basic Display*" -and $_.Status -eq "OK" } |
    ForEach-Object { Disable-PnpDevice -InstanceId $_.InstanceId -Confirm:$false -ErrorAction SilentlyContinue }

# 5. Clear RDP_GFX 0x200d (0x8007007A) offending hardware-AVC policy overrides
$tsPol = "HKLM:\SOFTWARE\Policies\Microsoft\Windows NT\Terminal Services"
if (Test-Path $tsPol) {
    Remove-ItemProperty -Path $tsPol -Name "AVCHardwareEncodePreferred", "AVC444ModePreferred", "bEnumerateHWAdapters", "fEnableHardwareMode" -ErrorAction SilentlyContinue
}

# 6. Enable RDP + NLA (do NOT call Restart-Service TermService -Force on a live desktop!)
Set-ItemProperty -Path "HKLM:\System\CurrentControlSet\Control\Terminal Server" -Name "fDenyTSConnections" -Type DWord -Value 0
Set-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations\rdp-tcp" -Name "UserAuthentication" -Type DWord -Value 1
Enable-NetFirewallRule -DisplayGroup "Remote Desktop" -ErrorAction SilentlyContinue

Write-Host "=== Verification ==="
Get-PnpDevice -Class Display -PresentOnly | Select-Object Status, FriendlyName, InstanceId | Format-Table -AutoSize
$nvsmi = "C:\Windows\System32\nvidia-smi.exe"
if (Test-Path $nvsmi) {
    & $nvsmi -q | Select-String -Pattern "Product Name|Driver Version|CUDA Version|License Status|Expiry"
}
Write-Host "Note: If performing a full VM reboot under QEMU/OVMF after WDDM updates, prefer a cold stop/start cycle if OVMF stalls on warm reset."
