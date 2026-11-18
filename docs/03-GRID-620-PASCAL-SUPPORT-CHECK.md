# 10 — GRID 620.70 (host 620.70.04 / guest 620.70.06 / Windows 621.14): does it support Pascal?

Packages: `NVIDIA-GRID-Linux-KVM-620.70.04-620.70.06-621.14/` (host `NVIDIA-Linux-x86_64-620.70.04-vgpu-kvm.run`, guest `NVIDIA-Linux-x86_64-620.70.06-grid.run`, Windows `621.14_grid_win10_win11_server2022_server_2025_dch_64bit_international.exe`). Extracted on `192.168.1.15` under `/home/zero/vgpu-work/{vgpu-kvm-620.70.04,grid-guest-620.70.06}`.

## Observed facts

| Check | Result |
| :--- | :--- |
| Host `vgpuConfig.xml` Pascal device IDs (`0x1B..`, `0x1C..`, `0x1D..`) | **none** |
| Host `vgpuConfig.xml` `GRID P4` vGPU types | **0** |
| Host `vgpuConfig.xml` `1BB3` text matches | 3 lines, all are substrings of hex `<digest type="signature">` blobs (lines 9197, 10371, 10590) — false positives, not device IDs |
| Host `supported-gpus.json`, `0x1BB3` | `Tesla P4`, `legacybranch: 580.xx`, features `kernelopen` |
| Guest `supported-gpus.json`, `0x1BB3` | `Tesla P4`, `legacybranch: 580.xx`, features `kernelopen` |
| Guest 620 firmware dir | `gsp_ga10x.bin`, `gsp_tu10x.bin`, `ucodes_ga10x.bin`, `ucodes_tu10x.bin` (no Pascal blobs) |

`legacybranch: 580.xx` means NVIDIA's own metadata routes Tesla P4 to the 580 driver branch, i.e. 620 does not claim it.

## Inconclusive check (do not rely on it)

Counting `kern_gpu_gp100.c`-style filename strings in the proprietary `kernel/nvidia/nv-kernel.o_binary` returned 0 for 620 host, 620 guest **and 535 guest**. Since 535 is known to support Pascal, the binary is simply stripped of those strings; this test says nothing about Pascal HAL presence in 620.

## Consequences

- The 620 host cannot create Pascal vGPU types: no P4 profiles exist, and each profile in `vgpuConfig.xml` carries a signed digest, which must not be forged.
- The 620 guest driver does not list the P4 as supported.
- Nothing in these packages shows Pascal + Wayland working. That claim is unverified.
