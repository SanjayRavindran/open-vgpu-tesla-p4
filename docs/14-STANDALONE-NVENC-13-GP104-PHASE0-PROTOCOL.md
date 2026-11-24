# Phase 0 Protocol Specification: GP104 Standalone NVENC (`NVC2B7`) & NVENC `13.0` (`0xd0`) ABI

- **Target Hardware:** NVIDIA Tesla P4 (`GP104GL`, vGPU profile `nvidia-65` / `GRID P4-4Q`)
- **Host/Guest Driver Reference:** `535.309.01` (`libnvidia-encode.so.535.309.01` + `libnvcuvid.so.535.309.01`)
- **Capture VM:** `zero@192.168.1.15` (VM `1001`)
- **Decoder Tool:** [`open-nvenc/tools/decode_nvenc_trace.py`](../open-nvenc/tools/decode_nvenc_trace.py)
- **Compact Evidence:** [`evidence/nvenc-gp104-phase0/phase0_summary.json`](../evidence/nvenc-gp104-phase0/phase0_summary.json) (`374 KB`)

---

## 1. Summary of Differential Captures

All 5 captures ran live on `192.168.1.15` under `LD_PRELOAD` tracing and produced valid HEVC elementary streams verified with `ffprobe`:

| Case | Resolution & Input | Profile | Frames | Output `.h265` Bytes | HW Status `+0x08` (`encoded_bits`) | Raw HW Slice Bytes (`bits / 8`) | CPU Header Prefix Bytes |
|---|---|---|---|---:|---:|---:|---:|
| `case_A` | `256x256` `nv12` (4:2:0 8-bit) | `main` | 1 IDR | `7,181` | `0xdd00` (`56,576`) | `7,072` | `109` (`0x6d`: VPS+SPS+PPS+SEI) |
| `case_B` | `256x256` `p010le` (4:2:0 10-bit) | `main10` | 1 IDR | `7,210` | `0xdde8` (`56,808`) | `7,101` | `109` (`0x6d`: VPS+SPS+PPS+SEI) |
| `case_C` | `256x256` `yuv444p16le` (4:4:4 10-bit) | `rext` | 1 IDR | `8,503` | `0x10650` (`67,152`) | `8,394` | `109` (`0x6d`: VPS+SPS+PPS+SEI) |
| `case_D` f0 | `256x256` `yuv444p16le` | `rext` | Frame 0 (IDR) | `8,503` (of `10,778`) | `0x10650` (`67,152`) | `8,394` | `109` (`0x6d`: VPS+SPS+PPS+SEI) |
| `case_D` f1 | `256x256` `yuv444p16le` | `rext` | Frame 1 (P) | `2,275` (`10,778 - 8,503`) | `0x46b0` (`18,096`) | `2,262` | `13` (`0x0d`: SEI only) |
| `case_E` | `320x240` `nv12` (4:2:0 8-bit) | `main` | 1 IDR | `7,839` | `0xf190` (`61,840`) | `7,730` | `109` (`0x6d`: VPS+SPS+PPS+SEI) |

---

## 2. RM Allocation Sequence & Virtual Memory Map

### 2.1 Engine Class Correction (`0xc2b7`, NOT `0xc0b7`)

Existing stub headers assumed `NVC0B7_VIDEO_ENCODER` (`0xc0b7`). Live GP104 allocates **`NVC2B7_VIDEO_ENCODER` (`0xc2b7`)**, with RM control `0x906f0101` returning `classId = 0xc2b7, engineId = 0x1c`. Tesla P4 exposes two NVENC engine instances plus a Pascal Copy Engine (`0xc0b5`) used for recon/coloc buffer clearing.

### 2.2 CUDA Interop (`cuGetExportTable`) & Surface Sharing

Before opening NVENC's own RM client, `libnvcuvid` queries CUDA's internal export table (`cuGetExportTable`) for two GUIDs:
- `9af0707b8e2dd84c8e4eb994c82ddc35`
- `b10541e1f7c7c74a9f64f223be99f1e2`

Video surfaces allocated by CUDA under CUDA's RM client (e.g., `0xc1d0003e`, handles `0x5c00008c`, `0x5c00008e`) are imported into NVENC's RM client (`0xc1d00040`) via `NV_ESC_RM_DUP_OBJECT` (`NVOS55`, cmd `0x34`) and mapped into NVENC's GPU VASpace (`0x80000002`) via `NV_ESC_RM_MAP_MEMORY_DMA` (`NVOS46`, cmd `0x57`). Note: for standalone operation without CUDA (`Phase 1`), NVENC can allocate its own vidmem/sysmem buffers directly via `NV_ESC_RM_ALLOC_MEMORY` (`NVOS02`) / `NV_ESC_RM_ALLOC` (`0x2b` / `NVOS21`/`NVOS64`) without `libcuda.so`.

### 2.3 Observed `case_A` RM Object Hierarchy (`hClient = 0xc1d00040`)

1. **Client, Device, Subdevice, VASpace, SW Session:**
   - `0x80000000`: `NV01_DEVICE_0` (`0x80`), parent `0xc1d00040`
   - `0x80000001`: `NV20_SUBDEVICE_0` (`0x2080`), parent `0x80000000`
   - `0x80000002`: `FERMI_VASPACE_A` (`0x70`), parent `0x80000000`
   - `0x8000000e`: `NVENC_SW_SESSION` (`0xa0bc`), parent `0x80000001`
2. **Global Synchronization Semaphores:**
   - `0x8000000b`: `NV01_MEMORY_SYSTEM` (`0x3e`), `0x28000` B sysmem mapped at `0x100000000`
   - `0x8000000c`: duped `0x5c00008c`, `0x28000` B mapped at `0x100200000`
   - `0x8000000d`: `NV01_MEMORY_SYSTEM` (`0x3e`), `0x3000` B sysmem mapped at `0x100028000` (channel semaphore slots at `0x100028000`, `0x100029000`, etc.)
3. **Three GPFIFO Channels (`NVC06F` `0xc06f`):**
   Each channel allocates USERD (`0x3e`, `flags=0x5010`, `limit=0x9f` = `0xa0` B), ContextDMA (`0x2`), `0x50a0` memory object, pushbuffer/GPFIFO sysmem (`0x3e`, `flags=0x2010`, `limit=0x28fff` = `0x29000` B), and error notifier sysmem (`0x3e`, `flags=0x1010`, `limit=0xfff` = `0x1000` B):
   - **NVENC0 Channel `0x80000013`** (`PB = 0x80000011` @ `0x100030000`, `gpFifoOffset = 0x100050000`, `gpFifoEntries = 0x800`):
     - Child engine `0x80000032` (`NVC2B7_VIDEO_ENCODER` `0xc2b7`, empty/default instance 0 params) + event `0x80000033` (`0x79`)
   - **NVENC1 Channel `0x80000038`** (`PB = 0x80000036` @ `0x100060000`, `gpFifoOffset = 0x100080000`, `gpFifoEntries = 0x800`):
     - Child engine `0x80000057` (`NVC2B7_VIDEO_ENCODER` `0xc2b7`, `params` u32s: `size=0x0c`, `0`, `engineInstance=1`) + event `0x80000058` (`0x79`)
   - **Copy Engine Channel `0x8000006e`** (`PB = 0x8000006c` @ `0x100180000`, `gpFifoOffset = 0x1001a0000`, `gpFifoEntries = 0x800`):
     - Child engine `0x8000008d` (`PASCAL_DMA_COPY_A` `0xc0b5`) + event `0x8000008e` (`0x79`)
4. **Descriptor, Vidmem, and I/O Buffers:**
   - `0x80000059..0x80000068`: `16 x 0xe000` B sysmem (`0x3e`) for `pic_setup` ring (`0x100091000..0x100163000`)
   - `0x80000090..0x80000094`: `5 x 0x10000` B vidmem (`0x40`) for colocated MV (`0x90`, `0x92`) & scratch (`0x94`)
   - `0x80000095..0x80000096`: `2 x 0x1000` B sysmem (`0x3e`) (QM/RC metadata bound at `0x0724`)
   - Recon surfaces (`0x80000069`, `0x8000008f`) and current input surfaces (`0x80000097`, `0x800000a7`, `0x800000a8`):
     - `256x256` 4:2:0 8-bit (`case_A`): `0x18000` B
     - `320x240` 4:2:0 8-bit (`case_E`): `0x1e000` B
     - `256x256` 4:2:0 10-bit (`case_B`): `0x30000` B
     - `256x256` 4:4:4 10-bit (`case_C`, `case_D`): `0x60000` B
   - Output / status / aux bundles (4 sets: `{0x98..0x9a}`, `{0x9c..0x9e}`, `{0xa0..0xa2}`, `{0xa4..0xa6}`):
     - Bitstream out (`0x3e`): `0x29000` B (`case_A`), `0x33000` B (`case_B`, `case_E`), `0x65000` B (`case_C`, `case_D`) — matches `pic_setup +0x1a0`
     - Status/feedback (`0x3e`): `0x1000` B
     - Slice/IO aux (`0x3e`): `0x9000` B (remained zero in 1-slice captures)

---

## 3. GPFIFO & Pushbuffer Method Protocol (`NVC06F` + `NVC2B7` + `NVC0B5`)

### 3.1 Channel Ring Submission

- Each GPFIFO entry is 8 bytes at `gpFifoOffset + idx * 8` (in all 3 channels `gpFifoOffset - pb_base == 0x20000`):
  - `e0 = (gpu_va & 0xffffffff) | 0x0`
  - `e1 = ((gpu_va >> 32) & 0xff) | (dwords << 10) | (sync << 31)`
- Submission kicks `GPPut` in USERD (`+0x8c`), while hardware advances `GPGet` (`+0x88`). Observed `userd_ptrs`:
  - After `init`: `gp_get = 4, gp_put = 4` (`GP[0..3]` execute channel/engine `SET_OBJECT` + sync)
  - After frame 0: `gp_get = 6, gp_put = 6` (`GP[4]` = NVENC frame encode batch, `GP[5]` = host WFI/semaphore)

### 3.2 `NVC2B7` Engine Init & Per-Frame Sequence

At channel init (`GP[2]` on both `0x80000013` and `0x80000038`, `subch = 4`):
```text
0x0000 (NVC06F_SET_OBJECT)     = 0x0000c2b7
0x0204 (NV_APP_ID_OR_CTX_0204) = 0x5ffffffe
```

**Critical addressing convention:** All `0x0400..0x074c` surface/buffer offset methods pass **40-bit GPU virtual addresses right-shifted by 8 bits (`gpu_va >> 8`)**. Thus every NVENC GPU VA must be 256-byte aligned.

Verified per-frame `NVC2B7` methods (from `case_A` `0x80000013` and `case_D` F1 `0x80000038`):

| Method Byte | Name | Frame 0 (IDR) Value / Target | Frame 1 (P-frame, `case_D`) Value / Target |
|---:|---|---|---|
| `0x0240..0x0304` | `NV_SEMAPHORE_{A_HI,B_LO,C_PAYLOAD}` + `D_LAUNCH` | Host semaphore waits/releases around execute | Same |
| `0x0200` | `NV_SEMA_OR_NOP_0200` | `0x00000003` | `0x00000003` |
| `0x0700` | `NVC2B7_SET_APPLICATION_ID` | `0x00001104` (HEVC) | `0x00001104` (HEVC) |
| `0x0704` | `NVC2B7_SET_CONTROL_PARAMS` | `0x00000000` (Frame 0) | `0x00000001` (Frame 1) |
| `0x0710` | `NVC2B7_SET_DRV_PIC_SETUP_OFFSET` | `pic_setup >> 8` (`0x80000059`) | `pic_setup >> 8` (`0x8000005a`) |
| `0x0720` | `NVC2B7_SET_SCRATCH_VIDMEM_OFFSET` | `0x1001f0000 >> 8` (`0x80000094`, `0x10000` B) | Same (`0x80000094`) |
| `0x0738` | `NVC2B7_SET_COLOC_MV_BUF_0_OFFSET` | Not emitted on F0 IDR | `0x1001b0000 >> 8` (`0x80000090`, `0x10000` B) |
| `0x073c` | `NVC2B7_SET_COLOC_MV_BUF_1_OFFSET` | Not emitted on F0 IDR | `0x1001d0000 >> 8` (`0x80000092`, `0x10000` B) |
| `0x0724` | `NVC2B7_SET_QM_OR_RC_OFFSET` | `0x100173000 >> 8` (`0x80000095`, `0x1000` B) | Same (`0x80000095`) |
| `0x0730` | `NVC2B7_SET_RECON_PIC_LUMA_OFFSET` | `recon0 >> 8` (`0x80000069`) | `recon1 >> 8` (`0x8000008f`) |
| `0x074c` | `NVC2B7_SET_RECON_PIC_CHROMA_OFFSET` | `recon0 >> 8` (`0x80000069`) | `recon1 >> 8` (`0x8000008f`) |
| `0x0718` | `NVC2B7_SET_STATUS_BUF_OFFSET` | `status0 >> 8` (`0x80000099`) | `status1 >> 8` (`0x8000009d`) |
| `0x071c` | `NVC2B7_SET_OUT_BITSTREAM_OFFSET` | `bs0 >> 8` (`0x80000098`) | `bs1 >> 8` (`0x8000009c`) |
| `0x0734` | `NVC2B7_SET_CUR_PIC_LUMA_OFFSET` | `input0 >> 8` (`0x80000097` / `0x800000a7`) | `input1 >> 8` (`0x800000a8`) |
| `0x0740` | `NVC2B7_SET_CUR_PIC_CHROMA_U_OFFSET` | Same planar/packed surface `>> 8` | Same (`0x800000a8`) |
| `0x0744` | `NVC2B7_SET_CUR_PIC_CHROMA_V_OFFSET` | Same planar/packed surface `>> 8` | Same (`0x800000a8`) |
| `0x070c` | `NVC2B7_SET_SLICE_OR_IO_OFFSET` | `io0 >> 8` (`0x8000009a`, `0x9000` B) | `io1 >> 8` (`0x8000009e`, `0x9000` B) |
| `0x0400..0x043c` | `NVC2B7_SET_REF_PIC_LUMA_0..15` | Not emitted on F0 IDR | `0x0400 = recon0 >> 8` (`0x80000069`) |
| `0x0440..0x047c` | `NVC2B7_SET_REF_PIC_CHROMA_0..15` | Not emitted on F0 IDR | `0x0440 = recon0 >> 8` (`0x80000069`) |
| `0x0300` | `NVC2B7_EXECUTE` | `0x00000000` | `0x00000000` |

### 3.3 `NVC0B5` Copy Engine Clearing (`0x8000006e`)

At session init (`428` methods on `case_A`, `536` on `case_C`/`case_D`), `0x8000006e` binds `0x0000 = 0x0000c0b5` and issues batches of `NVC0B5_LAUNCH_DMA = 0x00000586` (`CONST_FILL`) with `0x0700 = 0` (`Luma = 0`) and alternating chroma fill batches (`0x0700 = 0`, `0x0704 = 0x80`) targeting recon and colocated/scratch regions, followed by semaphore updates `0x0300 = 0x00000008`.

---

## 4. `pic_setup` (`0xc2b70006`) Layout & 4-Way Differential Tables

### 4.1 Full Non-Zero Baseline (`case_A`: HEVC Main, `256x256`, `nv12`, constqp=24, IDR)

Total descriptor allocation is `0xe000` bytes; `case_A` sets 72 non-zero 32-bit dwords:

```text
+0x00000: 0xc2b70006  (magic / version)
+0x00004: 0x00ff00ff  (slot0: ((aligned_h - 1) << 16) | (w - 1))
+0x00008: 0x01000100  (slot0: pitch / aligned dims)
+0x00014: 0x00000080  +0x00018: 0x00000100  +0x0001c: 0x00000140  +0x00020: 0x00000002
+0x00024: 0x00ff00ff  (slot1 dims)
+0x00028: 0x01000100  +0x00038: 0x00000100  +0x00040: 0x00000002
+0x00044: 0x00ff00ff  (slot2 dims)
+0x00048: 0x01000100  +0x00054: 0x00000080  +0x00058: 0x00000100  +0x0005c: 0x00000140  +0x00060: 0x00000002
+0x00064: 0x028364a1  +0x00068: 0x00000200  +0x0006c: 0x04d00000  +0x00070: 0x00000000
+0x00074: 0x131f1800  (contains QP=24=0x18, max_cu/tu/log2 params)
+0x00078: 0x33000000  +0x0007c: 0x00003333  +0x000a4: 0x000000fa
+0x000d4..+0x000e0: 0xfefefefe x 4
+0x00184: 0x00000002  +0x0018c: 0x00005900  +0x00190: 0x00000800  +0x00194: 0x00003800  +0x00198: 0x00005800
+0x0019c: 0x0000cc00  +0x001a0: 0x00029000  (output bitstream alloc size)
+0x001a4: 0x000000ff  +0x001a8: 0x00ffc6a7
+0x001b0..+0x001b8: 0x00000100 x 3
+0x001bc: 0x00901300  (IDR: nal_unit_type = 0x13 = 19 IDR_W_RADL)
+0x002ac: 0x00002100  +0x002b0: 0x00006900  +0x002b4: 0x00010000
+0x002e4: 0x00001100  +0x002e8: 0x00000100  +0x0030c: 0x09e34000
+0x00800: 0x00074002  +0x00804: 0x0c006001  +0x00808: 0x03ff0fff  +0x0080c: 0x28000280
+0x00814: 0xa2400000  +0x0082c: 0x7f1c0800  +0x00830: 0x0000081c  +0x00838: 0x7f7f1c41
+0x0083c: 0x0000411c  +0x00840: 0x00000002  +0x00890: 0x00007c00
+0x03804..+0x03810: 0x55555557 x 4
+0x03818: 0x00000b6d  +0x0381c: 0x0f00f70f  +0x03820: 0x000179ff
+0x03848: 0xffff0000  +0x0384c: 0x023affff
+0x05800: 0xaaff056b  +0x05804: 0x000fffaa  +0x05808: 0x000f0f0f
+0x05900: 0x00980040  +0x05904: 0x18000204
```

### 4.2 Diff 1: `case_A` (4:2:0 8-bit) vs `case_B` (4:2:0 10-bit) — 9 Dwords

- `+0x00018`: `0x00000100` -> `0x00000200` (slot0 luma stride/size doubles for 16-bit P010 container)
- `+0x0001c`: `0x00000140` -> `0x00000280`
- `+0x00038`: `0x00000100` -> `0x00000200`
- `+0x00058`: `0x00000100` -> `0x00000200`
- `+0x0005c`: `0x00000140` -> `0x00000280`
- `+0x00068`: `0x00000200` -> `0x00000240` (bit 6 `0x40` enables 10-bit)
- `+0x0006c`: `0x04d00000` -> `0x04c00000`
- `+0x00078`: `0x33000000` -> `0x33f4f4f4` (`0xf4 = -12 = -QpBdOffset` for 10-bit Y/Cb/Cr)
- `+0x001a0`: `0x00029000` -> `0x00033000` (output bitstream buffer length)

### 4.3 Diff 2: `case_B` (4:2:0 10-bit) vs `case_C` (4:4:4 10-bit) — 14 Dwords

- `+0x00008`, `+0x00028`, `+0x00048`: `0x01000100` -> `0x02000100` (4:4:4 chroma plane height doubles vs 4:2:0)
- `+0x00014`, `+0x00054`: `0x00000100` -> `0x00000000`
- `+0x0001c`, `+0x0005c`: `0x00000280` -> `0x00000400`
- `+0x0003c`: `0x00000000` -> `0x00000400`
- `+0x00020`, `+0x00040`, `+0x00060`: `0x00000002` -> `0x00000302` (`0x0300` marks 4:4:4 chroma subsampling)
- `+0x00064`: `0x028364a1` -> `0x028364a3` (bit 1 `0x02` marks 4:4:4 chroma format)
- `+0x00070`: `0x00000000` -> `0x00003180`
- `+0x001a0`: `0x00033000` -> `0x00065000`

### 4.4 Diff 3: `case_D` Frame 0 (IDR) vs Frame 1 (P-frame) — 45 Dwords

- `+0x000d4`: `0xfefefefe` -> `0xfefefe00` (active slot 0 index)
- `+0x000e4..+0x00170`: all-zero on IDR -> active DPB / L0 / L1 reference lists (`0xffffff01`, `0xffff0100`, `0xffffffff` repeating)
- `+0x00174..+0x00178`: `0x00000000` -> `0xfefefefe`
- `+0x00184`: `0x00000002` -> `0x00010002`
- `+0x001a8`: `0x00ffc6a7` -> `0x00ffc6a4`
- `+0x001bc`: `0x00901300` (`nal_unit_type = 0x13 = 19`) -> `0x00900101` (`nal_unit_type = 0x01` `TRAIL_R`)
- `+0x00288`: `0x00000000` -> `0x00000001` (frame number / POC / P-slice index)
- `+0x05900`: `0x00980040` -> `0x00c00040`
- `+0x05904`: `0x18000204` -> `0x18000200`

### 4.5 Diff 4: `case_A` (`256x256`) vs `case_E` (`320x240`) — 17 Dwords

- `+0x00004`, `+0x00024`, `+0x00044`: `0x00ff00ff` -> `0x00ff013f` (`0x13f = 319 = 320 - 1`, `0x00ff = 255 = align(240, 16) - 1`)
- `+0x00008`, `+0x00028`, `+0x00048`: `0x01000100` -> `0x01400140` (`0x140 = 320`)
- `+0x00014`, `+0x00054`: `0x00000080` -> `0x000000a0`
- `+0x00018`, `+0x00038`, `+0x00058`: `0x00000100` -> `0x00000140`
- `+0x0001c`, `+0x0005c`: `0x00000140` -> `0x00000190`
- `+0x0019c`: `0x0000cc00` -> `0x0000ff00`
- `+0x001a0`: `0x00029000` -> `0x00033000`
- `+0x001a8`: `0x00ffc6a7` -> `0x00ffc7a7`
- `+0x05900`: `0x00980040` -> `0x00980050` (CTU/width bits: `0x40` for `256`, `0x50` for `320`)

---

## 5. Hardware Status & Bitstream Output Assembly

1. **Falcon Hardware Write (`NvEncEncodePicture`):**
   - Falcon writes the raw HEVC slice NAL (`00 00 01 26 01 ...` for IDR or `00 00 01 02 01 ...` for P-frame) starting at **byte offset `+0x100`** (`buf[0xff] == 0x00`, `buf[0x100:0x103] == 00 00 01`) in the output bitstream buffer bound to `0x071c`.
   - Falcon writes the completion status struct into the `0x1000` B status buffer (`0x0718`):
     - `+0x04`: `2` (done)
     - `+0x08`: `encoded_bits` (exact byte-aligned bit count of the HW slice)
     - Also populated: `+0x10`, `+0x14` (`0x130000` IDR / `0x180000` P), `+0x20` (`0xff`), `+0x28`, `+0x30`, `+0x40`, `+0x48`.
2. **CPU Header Assembly (`NvEncLockBitstream`):**
   - On IDR (`case_A..C, D f0, E`), CPU generates `109` bytes (`0x6d`: `VPS` + `SPS` + `PPS` + `PREFIX_SEI`) at `+0x000` and shifts the HW slice left from `+0x100` to `+0x6d` (producing a 4-byte `00 00 00 01 26 01` startcode at `+0x6d`).
   - On P-frame (`case_D f1`), CPU generates `13` bytes (`0x0d`: `PREFIX_SEI` `4e 01 ...`) at `+0x000` and shifts the HW slice left to `+0x0d`.
   - Consequently, a standalone CPU encoder only needs to generate standard ITU-T H.265 `VPS`/`SPS`/`PPS`/`SEI` NAL units (or `VPS`/`SPS`/`PPS` without proprietary SEI) and concatenate the Falcon-encoded slice bytes `[+0x100 .. +0x100 + encoded_bits/8)`.

---

## 6. NVENC `13.0` (`0xd0`) ABI Delta & Phase 1 Plan

### 6.1 Required NVENC `13.0` ABI Updates (`open_nvenc.h` / `open_nvenc.c`)

Currently [`open-nvenc/open_nvenc.h`](../open-nvenc/open_nvenc.h) targets NVENC `12.1` (`NVENCAPI_MAJOR_VERSION 12U`, `NVENCAPI_MINOR_VERSION 1U`, `0x7102000c -> 0xc1`). Disassembly of `Selkies` / `pixelflux` confirmed that NVENC `12.2+` / `13.0` requires:
1. **`NVENCAPI_MAJOR_VERSION 13U`, `NVENCAPI_MINOR_VERSION 0U`** so `NvEncodeAPIGetMaxSupportedVersion(&ver)` returns `(13 << 4) | 0 = 0xd0` (`>= 0xc2` unblocks `420-10` and `444-10` in `pixelflux`).
2. **Versioned Struct Support (`0xd` low nibble):** Accept `13.0` struct versions observed in `pixelflux` (`0x7728850d` `NV_ENCODE_API_FUNCTION_LIST`, `0x7001000d` `NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS`, `0x7004000d` `NV_ENC_INITIALIZE_PARAMS`, `0x7002000d` `NV_ENC_CONFIG`, etc.) via `open_nvenc_decode_struct_version`.
3. **`NV_ENC_CONFIG_HEVC` 12.2+/13.0 Bit Depth Fields:** Replace NVENC `12.1`'s `pixelBitDepthMinus8 : 3` with distinct **`inputBitDepth`** and **`outputBitDepth`** bitfields matching the NVENC `13.0` SDK layout while remaining backwards-compatible when older struct versions are passed.

### 6.2 Phase 1 Implementation Plan (`open-nvenc/open_nvenc.c`)

1. Replace the placeholder `0xc0b7` class ID in `open_nvenc.c` with `NVC2B7_VIDEO_ENCODER` (`0xc2b7`), allocate the single NVENC channel + `NVC0B5` Copy Engine clearing channel, and allocate/DMA-map sysmem (`0x3e`) and vidmem (`0x40`) buffers directly via `/dev/nvidiactl` + `/dev/nvidia0`.
2. Build standalone CPU-side `pic_setup` (`0xc2b70006`) + `0x80000095` builder parameterized by width/height, chroma format (`nv12` / `p010` / `yuv444` 8-bit & 10-bit), and `constqp` QP.
3. Submit real `NVC06F` GPFIFO + `NVC2B7` (`0x0700..0x074c`, `0x0300 = 0`) commands via USERD `GPPut`, poll completion via host semaphore / status buffer `+0x04 == 2`, read `encoded_bits` from `+0x08`, emit spec-compliant H.265 `VPS`/`SPS`/`PPS` + Falcon IDR slice from `+0x100`, and verify offline + live container (`brave-origin` 10-bit 4:4:4) decode with `ffprobe` / `ffmpeg`.
