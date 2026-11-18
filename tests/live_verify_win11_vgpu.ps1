# SPDX-License-Identifier: GPL-2.0-only
# Live verification harness for Windows 11 24H2 + NVIDIA GRID 539.72 Pascal vGPU.
$ErrorActionPreference = "Continue"

Write-Output "=== WINDOWS & GPU PNP DEVICE ==="
Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber | Format-List
Get-PnpDevice -Class Display | Select-Object Status, FriendlyName, InstanceId | Format-Table -AutoSize

Write-Output "=== NVIDIA-SMI ==="
& "C:\Windows\System32\nvidia-smi.exe"

Write-Output "=== CUDA 12.1/12.2 + NVENC 12.1 + NVDEC CODEC MATRIX ==="
$cs = @'
using System;
using System.Runtime.InteropServices;

public class NvCheck {
    [DllImport("nvcuda.dll")] public static extern int cuInit(uint flags);
    [DllImport("nvcuda.dll")] public static extern int cuDriverGetVersion(out int ver);
    [DllImport("nvcuda.dll")] public static extern int cuDeviceGet(out int dev, int ordinal);
    [DllImport("nvcuda.dll")] public static extern int cuCtxCreate_v2(out IntPtr ctx, uint flags, int dev);
    [DllImport("nvEncodeAPI64.dll")] public static extern int NvEncodeAPIGetMaxSupportedVersion(out uint ver);
    [StructLayout(LayoutKind.Sequential)]
    public struct CUVIDDECODECAPS {
        public int eCodecType; public int eChromaFormat; public uint nBitDepthMinus8;
        public uint r1, r2, r3; public byte bIsSupported; public byte nNumNVDECs;
        public ushort nOutputFormatMask; public uint nMaxWidth; public uint nMaxHeight;
        public uint nMaxMBCount; public ushort nMinWidth; public ushort nMinHeight;
        public byte bIsHistogramSupported; public byte nCounterBitDepth; public ushort nMaxHistogramBins;
        public uint p0,p1,p2,p3,p4,p5,p6,p7,p8,p9;
    }
    [DllImport("nvcuvid.dll")] public static extern int cuvidGetDecoderCaps(ref CUVIDDECODECAPS caps);
    public static void Run() {
        int rc = cuInit(0);
        int ver = 0, dev = 0; IntPtr ctx = IntPtr.Zero;
        cuDriverGetVersion(out ver);
        cuDeviceGet(out dev, 0);
        cuCtxCreate_v2(out ctx, 0, dev);
        Console.WriteLine("CUDA: rc=" + rc + " driverVersion=" + ver);
        uint nvVer = 0;
        int encRc = NvEncodeAPIGetMaxSupportedVersion(out nvVer);
        Console.WriteLine("NVENC: rc=" + encRc + " maxApi=" + (nvVer >> 4) + "." + (nvVer & 0xF));
        var codecs = new Tuple<string,int>[] {
            Tuple.Create("MPEG2",1), Tuple.Create("VC1",3), Tuple.Create("H264",4),
            Tuple.Create("JPEG",5), Tuple.Create("HEVC",8), Tuple.Create("VP8",9),
            Tuple.Create("VP9",10), Tuple.Create("AV1",11)
        };
        foreach (var c in codecs) {
            var caps = new CUVIDDECODECAPS { eCodecType = c.Item2, eChromaFormat = 1, nBitDepthMinus8 = 0 };
            int drc = cuvidGetDecoderCaps(ref caps);
            Console.WriteLine("NVDEC " + c.Item1 + ": rc=" + drc + " sup=" + caps.bIsSupported + " max=" + caps.nMaxWidth + "x" + caps.nMaxHeight);
        }
    }
}
'@
Add-Type -TypeDefinition $cs
[NvCheck]::Run()

Write-Output "=== WINDOWS RDP HARDWARE ENCODER EVENTS (RdpCoreTS Event 170 / 162) ==="
Get-WinEvent -LogName "Microsoft-Windows-RemoteDesktopServices-RdpCoreTS/Operational" -MaxEvents 40 |
    Where-Object { $_.Id -in @(162, 170) } |
    Select-Object -First 10 TimeCreated, Id, Message | Format-List
