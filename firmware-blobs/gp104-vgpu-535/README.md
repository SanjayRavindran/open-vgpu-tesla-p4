# Isolated `GP104` Pascal Falcon Blobs from vGPU Host `535.309.01`

This directory holds the signed NVIDIA Falcon firmware binaries extracted from `NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run` (`kernel/nvidia/nv-kernel.o_binary`) for `GP104` (`HAL_IMPL = 0x1f`) by executing the decompiled Physical HAL initializers (`_nv003382rm`, `_nv003443rm`, `_nv003426rm`, `_nv003378rm`, `_nv003419rm`, `_nv003313rm`, `_nv003317rm`).

## Key Finding

Pascal vGPU (`535.309.01`) does **not** use separate vGPU-specific Falcon firmware:
- `acr/gp104_acr_bl`, `pmu/gp104_pmu_bl`, `sec2/gp104_sec2_rtos`, `sec2/gp104_sec2_hs`, `nvenc/gp104_nvenc_hs_scrub`, `gr/gp104_fecs_*`, `gr/gp104_gpccs_*`, and `gr/gp104_gr_netlist.bin` (`90,552` B, `Jun 28 2019`) are **SHA-256 identical** to the R580 (`580.178.04`) blobs in [`../gp104/`](../firmware-blobs/gp104/BLOB_MANIFEST.json).
- `pmu/gp104_pmu_rtos` (`294,400` B), `nvenc/gp104_nvenc0/1_ucode` (`230,912` B), and `nvdec/gp104_nvdec_ucode` (`110,080` B) are 535-branch signed builds carrying valid `192`-byte `LsfSigInfo` footers (`fid=0`, `fid=4`, `fid=5`).

See [`BLOB_MANIFEST.json`](../firmware-blobs/gp104-vgpu-535/BLOB_MANIFEST.json) for full SHA-256 digests and `.rodata` offsets, and [`../../docs/08-PASCAL-VGPU-535-ANALYSIS-AND-RUST.md`](../docs/08-PASCAL-VGPU-535-ANALYSIS-AND-RUST.md) for full analysis.
