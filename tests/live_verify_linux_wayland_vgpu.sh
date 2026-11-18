#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Live verification harness for Linux Pure Wayland GNOME + Pascal vGPU guests.
set -euo pipefail

echo "=== OS & KERNEL ==="
grep -E '^(PRETTY_NAME|VERSION_ID)=' /etc/os-release || true
uname -a

echo "=== X11 / XWAYLAND ZERO-PROCESS AUDIT ==="
X11_PROCS="$(pgrep -af 'Xorg|Xwayland|xbrlapi' || true)"
if [[ -n "${X11_PROCS}" ]]; then
  echo "FAIL: Found running X11/Xwayland processes:"
  echo "${X11_PROCS}"
else
  echo "PASS: X11_RUNNING=0 (zero Xorg/Xwayland processes)"
fi

echo "=== WAYLAND ENVIRONMENT ==="
systemctl --user show-environment 2>/dev/null | grep -E '^(WAYLAND_DISPLAY|XDG_SESSION_TYPE|DISPLAY)=' || true

echo "=== NVIDIA-SMI ==="
nvidia-smi

echo "=== CUDA 12.1/12.2 + NVENC 12.1 + NVDEC CODEC MATRIX ==="
python3 - <<'PY'
import ctypes

cuda = ctypes.CDLL("libcuda.so.1")
assert cuda.cuInit(0) == 0, "cuInit failed"
ver = ctypes.c_int(0)
cuda.cuDriverGetVersion(ctypes.byref(ver))
dev = ctypes.c_int(0)
assert cuda.cuDeviceGet(ctypes.byref(dev), 0) == 0
name = ctypes.create_string_buffer(128)
cuda.cuDeviceGetName(name, 128, dev)
ctx = ctypes.c_void_p()
assert cuda.cuCtxCreate_v2(ctypes.byref(ctx), 0, dev) == 0
print(f"CUDA PASS: driver={ver.value} device={name.value.decode()}")

nvenc = ctypes.CDLL("libnvidia-encode.so.1")
max_ver = ctypes.c_uint32(0)
assert nvenc.NvEncodeAPIGetMaxSupportedVersion(ctypes.byref(max_ver)) == 0
print(f"NVENC PASS: max_api={max_ver.value >> 4}.{max_ver.value & 0xF}")

cuvid = ctypes.CDLL("libnvcuvid.so.1")
class CUVIDDECODECAPS(ctypes.Structure):
    _fields_ = [
        ("eCodecType", ctypes.c_int), ("eChromaFormat", ctypes.c_int), ("nBitDepthMinus8", ctypes.c_uint),
        ("reserved1", ctypes.c_uint * 3), ("bIsSupported", ctypes.c_ubyte), ("nNumNVDECs", ctypes.c_ubyte),
        ("nOutputFormatMask", ctypes.c_ushort), ("nMaxWidth", ctypes.c_uint), ("nMaxHeight", ctypes.c_uint),
        ("nMaxMBCount", ctypes.c_uint), ("nMinWidth", ctypes.c_ushort), ("nMinHeight", ctypes.c_ushort),
        ("bIsHistogramSupported", ctypes.c_ubyte), ("nCounterBitDepth", ctypes.c_ubyte),
        ("nMaxHistogramBins", ctypes.c_ushort), ("reserved3", ctypes.c_uint * 10),
    ]
codecs = [("MPEG2", 1), ("VC1", 3), ("H264", 4), ("JPEG", 5), ("HEVC", 8), ("VP8", 9), ("VP9", 10), ("AV1", 11)]
for cname, cid in codecs:
    caps = CUVIDDECODECAPS(eCodecType=cid, eChromaFormat=1, nBitDepthMinus8=0)
    rc = cuvid.cuvidGetDecoderCaps(ctypes.byref(caps))
    print(f"NVDEC {cname:5s}: rc={rc} supported={caps.bIsSupported} max={caps.nMaxWidth}x{caps.nMaxHeight}")
PY

echo "=== GNOME REMOTE DESKTOP (GRD) CUDA / AVC444 LOGS ==="
journalctl -u gnome-remote-desktop -n 40 --no-pager 2>/dev/null | grep -iE 'CUDA|AVC444|RDPGFX|H264' || true
