/* SPDX-License-Identifier: MIT */
/*
 * test_nvenc_hw.c - Standalone Live GP104 Hardware HEVC IDR Smoke Test
 *
 * Links ONLY against libopen-nvidia-encode.so (zero libcuda/libnvcuvid deps),
 * encodes 4 real synthetic patterns on /dev/nvidiactl + /dev/nvidia0:
 *   1. 256x256 NV12 (8-bit 4:2:0 Main)
 *   2. 256x256 P010 (10-bit 4:2:0 Main10)
 *   3. 256x256 YUV444 10-bit (REXT)
 *   4. 320x240 NV12 (8-bit 4:2:0 Main)
 */

#include "open_nvenc.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void fill_pattern(uint8_t *buf, uint32_t pitch, uint32_t w, uint32_t h,
                         NV_ENC_BUFFER_FORMAT fmt)
{
    if (fmt == NV_ENC_BUFFER_FORMAT_NV12) {
        uint8_t *y = buf;
        uint8_t *uv = buf + (size_t)pitch * (size_t)h;
        for (uint32_t r = 0; r < h; r++) {
            for (uint32_t c = 0; c < w; c++) {
                int checker = (((r / 16U) + (c / 16U)) & 1U) ? 40 : 0;
                y[(size_t)r * pitch + c] = (uint8_t)((16U + (c * 180U) / w + checker) & 0xffU);
            }
        }
        for (uint32_t r = 0; r < h / 2U; r++) {
            for (uint32_t c = 0; c < w; c += 2U) {
                uv[(size_t)r * pitch + c + 0U] = (uint8_t)(80U + (r * 90U) / (h / 2U));
                uv[(size_t)r * pitch + c + 1U] = (uint8_t)(180U - (c * 80U) / w);
            }
        }
    } else if (fmt == NV_ENC_BUFFER_FORMAT_YUV420_10BIT) {
        uint16_t *y = (uint16_t *)buf;
        uint32_t p16 = pitch / 2U;
        uint16_t *uv = y + (size_t)p16 * (size_t)h;
        for (uint32_t r = 0; r < h; r++) {
            for (uint32_t c = 0; c < w; c++) {
                uint32_t val10 = 64U + ((c * 720U) / w) + ((((r / 16U) + (c / 16U)) & 1U) ? 120U : 0U);
                y[(size_t)r * p16 + c] = (uint16_t)((val10 & 0x3ffU) << 6);
            }
        }
        for (uint32_t r = 0; r < h / 2U; r++) {
            for (uint32_t c = 0; c < w; c += 2U) {
                uint32_t u10 = 320U + (r * 380U) / (h / 2U);
                uint32_t v10 = 700U - (c * 350U) / w;
                uv[(size_t)r * p16 + c + 0U] = (uint16_t)((u10 & 0x3ffU) << 6);
                uv[(size_t)r * p16 + c + 1U] = (uint16_t)((v10 & 0x3ffU) << 6);
            }
        }
    } else if (fmt == NV_ENC_BUFFER_FORMAT_YUV444_10BIT) {
        uint16_t *y = (uint16_t *)buf;
        uint32_t p16 = pitch / 2U;
        uint16_t *u = y + (size_t)p16 * (size_t)h;
        uint16_t *v = u + (size_t)p16 * (size_t)h;
        for (uint32_t r = 0; r < h; r++) {
            for (uint32_t c = 0; c < w; c++) {
                uint32_t y10 = 64U + ((c * 700U) / w) + ((((r / 16U) + (c / 16U)) & 1U) ? 140U : 0U);
                uint32_t u10 = 300U + (r * 420U) / h;
                uint32_t v10 = 750U - (c * 420U) / w;
                y[(size_t)r * p16 + c] = (uint16_t)((y10 & 0x3ffU) << 6);
                u[(size_t)r * p16 + c] = (uint16_t)((u10 & 0x3ffU) << 6);
                v[(size_t)r * p16 + c] = (uint16_t)((v10 & 0x3ffU) << 6);
            }
        }
    }
}

static void run_hw_case(NV_ENCODE_API_FUNCTION_LIST *fn,
                        const char *out_path,
                        uint32_t w, uint32_t h,
                        NV_ENC_BUFFER_FORMAT fmt,
                        NVENC_GUID profile,
                        uint32_t chroma_idc,
                        NV_ENC_BIT_DEPTH bit_depth)
{
    int dummy_dev = 0x10de;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open_p;
    memset(&open_p, 0, sizeof(open_p));
    open_p.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open_p.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    open_p.device     = &dummy_dev;
    open_p.apiVersion = NVENCAPI_VERSION;

    void *enc = NULL;
    assert(fn->nvEncOpenEncodeSessionEx(&open_p, &enc) == NV_ENC_SUCCESS);
    assert(enc != NULL);

    NV_ENC_PRESET_CONFIG preset;
    memset(&preset, 0, sizeof(preset));
    preset.version           = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    assert(fn->nvEncGetEncodePresetConfigEx(
        enc, NV_ENC_CODEC_HEVC_GUID, NV_ENC_PRESET_P4_GUID,
        NV_ENC_TUNING_INFO_LOW_LATENCY, &preset) == NV_ENC_SUCCESS);

    preset.presetCfg.profileGUID                                  = profile;
    preset.presetCfg.encodeCodecConfig.hevcConfig.chromaFormatIDC = chroma_idc;
    preset.presetCfg.encodeCodecConfig.hevcConfig.outputBitDepth  = bit_depth;
    preset.presetCfg.encodeCodecConfig.hevcConfig.inputBitDepth   = bit_depth;
    preset.presetCfg.rcParams.constQP.qpIntra                     = 24U;
    preset.presetCfg.rcParams.constQP.qpInterP                    = 24U;
    preset.presetCfg.rcParams.constQP.qpInterB                    = 24U;

    NV_ENC_INITIALIZE_PARAMS init_p;
    memset(&init_p, 0, sizeof(init_p));
    init_p.version      = NV_ENC_INITIALIZE_PARAMS_VER;
    init_p.encodeGUID   = NV_ENC_CODEC_HEVC_GUID;
    init_p.presetGUID   = NV_ENC_PRESET_P4_GUID;
    init_p.encodeWidth  = w;
    init_p.encodeHeight = h;
    init_p.frameRateNum = 60U;
    init_p.frameRateDen = 1U;
    init_p.tuningInfo   = NV_ENC_TUNING_INFO_LOW_LATENCY;
    init_p.bufferFormat = fmt;
    init_p.encodeConfig = &preset.presetCfg;

    NVENCSTATUS st = fn->nvEncInitializeEncoder(enc, &init_p);
    if (st != NV_ENC_SUCCESS) {
        fprintf(stderr, "FAIL %s InitializeEncoder st=%d err=%s\n",
                out_path, st, fn->nvEncGetLastErrorString(enc));
    }
    assert(st == NV_ENC_SUCCESS);

    NV_ENC_CREATE_INPUT_BUFFER ib_p;
    memset(&ib_p, 0, sizeof(ib_p));
    ib_p.version   = NV_ENC_CREATE_INPUT_BUFFER_VER;
    ib_p.width     = w;
    ib_p.height    = h;
    ib_p.bufferFmt = fmt;
    assert(fn->nvEncCreateInputBuffer(enc, &ib_p) == NV_ENC_SUCCESS);

    NV_ENC_LOCK_INPUT_BUFFER lock_in;
    memset(&lock_in, 0, sizeof(lock_in));
    lock_in.version     = NV_ENC_LOCK_INPUT_BUFFER_VER;
    lock_in.inputBuffer = ib_p.inputBuffer;
    assert(fn->nvEncLockInputBuffer(enc, &lock_in) == NV_ENC_SUCCESS);
    fill_pattern((uint8_t *)lock_in.bufferDataPtr, lock_in.pitch, w, h, fmt);
    assert(fn->nvEncUnlockInputBuffer(enc, ib_p.inputBuffer) == NV_ENC_SUCCESS);

    NV_ENC_CREATE_BITSTREAM_BUFFER bs_p;
    memset(&bs_p, 0, sizeof(bs_p));
    bs_p.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    bs_p.size    = 262144U;
    assert(fn->nvEncCreateBitstreamBuffer(enc, &bs_p) == NV_ENC_SUCCESS);

    NV_ENC_PIC_PARAMS pic;
    memset(&pic, 0, sizeof(pic));
    pic.version         = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth      = w;
    pic.inputHeight     = h;
    pic.inputPitch      = lock_in.pitch;
    pic.inputBuffer     = ib_p.inputBuffer;
    pic.outputBitstream = bs_p.bitstreamBuffer;
    pic.bufferFmt       = fmt;
    pic.encodePicFlags  = NV_ENC_PIC_FLAG_FORCEIDR;
    st = fn->nvEncEncodePicture(enc, &pic);
    if (st != NV_ENC_SUCCESS) {
        fprintf(stderr, "FAIL %s EncodePicture st=%d err=%s\n",
                out_path, st, fn->nvEncGetLastErrorString(enc));
    }
    assert(st == NV_ENC_SUCCESS);

    NV_ENC_LOCK_BITSTREAM lbs;
    memset(&lbs, 0, sizeof(lbs));
    lbs.version         = NV_ENC_LOCK_BITSTREAM_VER;
    lbs.outputBitstream = bs_p.bitstreamBuffer;
    assert(fn->nvEncLockBitstream(enc, &lbs) == NV_ENC_SUCCESS);

    open_nvenc_rm_stats_t stats;
    assert(open_nvenc_get_rm_stats(enc, &stats) == 0);
    assert(stats.hw_encode_active == 1);
    assert(stats.last_hw_encoded_bits > 0U);
    assert(lbs.bitstreamSizeInBytes > 64U);

    FILE *fp = fopen(out_path, "wb");
    assert(fp != NULL);
    assert(fwrite(lbs.bitstreamBufferPtr, 1U, lbs.bitstreamSizeInBytes, fp) == lbs.bitstreamSizeInBytes);
    fclose(fp);

    printf("HW_ENCODE_OK %s: total=%u B, hw_bits=%u, h_client=0x%x, class=0x%x\n",
           out_path, lbs.bitstreamSizeInBytes, stats.last_hw_encoded_bits,
           stats.h_client, stats.nvenc_class);

    assert(fn->nvEncUnlockBitstream(enc, bs_p.bitstreamBuffer) == NV_ENC_SUCCESS);
    assert(fn->nvEncDestroyBitstreamBuffer(enc, bs_p.bitstreamBuffer) == NV_ENC_SUCCESS);
    assert(fn->nvEncDestroyInputBuffer(enc, ib_p.inputBuffer) == NV_ENC_SUCCESS);
    assert(fn->nvEncDestroyEncoder(enc) == NV_ENC_SUCCESS);
}

int main(void)
{
    uint32_t ver = 0U;
    assert(NvEncodeAPIGetMaxSupportedVersion(&ver) == NV_ENC_SUCCESS);
    assert(ver == 0x000000d0U);

    NV_ENCODE_API_FUNCTION_LIST fn;
    memset(&fn, 0, sizeof(fn));
    fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    assert(NvEncodeAPICreateInstance(&fn) == NV_ENC_SUCCESS);

    mkdir("/tmp/open_nvenc_hw_test", 0755);

    run_hw_case(&fn, "/tmp/open_nvenc_hw_test/nv12_256.h265", 256U, 256U,
                NV_ENC_BUFFER_FORMAT_NV12, NV_ENC_HEVC_PROFILE_MAIN_GUID,
                1U, NV_ENC_BIT_DEPTH_8);
    run_hw_case(&fn, "/tmp/open_nvenc_hw_test/p010_256.h265", 256U, 256U,
                NV_ENC_BUFFER_FORMAT_YUV420_10BIT, NV_ENC_HEVC_PROFILE_MAIN10_GUID,
                1U, NV_ENC_BIT_DEPTH_10);
    run_hw_case(&fn, "/tmp/open_nvenc_hw_test/yuv444_10b_256.h265", 256U, 256U,
                NV_ENC_BUFFER_FORMAT_YUV444_10BIT, NV_ENC_HEVC_PROFILE_FREXT_GUID,
                3U, NV_ENC_BIT_DEPTH_10);
    run_hw_case(&fn, "/tmp/open_nvenc_hw_test/nv12_320x240.h265", 320U, 240U,
                NV_ENC_BUFFER_FORMAT_NV12, NV_ENC_HEVC_PROFILE_MAIN_GUID,
                1U, NV_ENC_BIT_DEPTH_8);

    printf("ALL_OPEN_NVENC_HW_TESTS_PASSED\n");
    return 0;
}
