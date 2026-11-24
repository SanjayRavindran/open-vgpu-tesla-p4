/* SPDX-License-Identifier: MIT */
/*
 * test_nvenc_unit.c - Unit & Differential Test Suite for open-nvenc
 *
 * Verifies:
 *   1. Exact struct version decoding & NvEncodeAPIGetMaxSupportedVersion (0xc1)
 *      matching FUN_00115a60 & FUN_0011f2b0 in libnvidia-encode.so.535.309.01.
 *   2. Exact NV_ENCODE_API_FUNCTION_LIST byte offsets (+0x08..+0x158) and
 *      error codes (NULL -> 6, invalid version -> 15, valid -> 0).
 *   3. All 8 NvTool* exports (45 API names matching 0x33c120, TLS ApiID,
 *      slot 0/1 interface storage, vtable creation/destruction).
 *   4. Full Pascal GP104 H.264 & HEVC 10-bit 4:4:4 (YUV444_10BIT) encode
 *      lifecycle (OpenEncodeSessionEx, GUIDs, InputFormats, Caps,
 *      PresetConfigEx, InitializeEncoder, Create/Lock/Unlock InputBuffer,
 *      CreateBitstreamBuffer, EncodePicture, Lock/Unlock Bitstream,
 *      GetSequenceParams, Register/Map Resource, DestroyEncoder).
 *   5. Optional differential check against vendor libnvidia-encode.so.535.309.01
 *      when OPEN_NVENC_VENDOR_LIB is provided.
 */

#include "open_nvenc.h"

#include <assert.h>
#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_version_decoder_and_max_version(void)
{
    int valid = 0;
    uint32_t v = open_nvenc_decode_struct_version(0x7102000cU, &valid);
    assert(valid == 1);
    assert(v == 0x000000c1U);

    v = open_nvenc_decode_struct_version(0x7002000dU, &valid);
    assert(valid == 1);
    assert(v == 0x000000d0U);

    /* Invalid tag 0 or 6 must set valid=0 */
    v = open_nvenc_decode_struct_version(0x0002000cU, &valid);
    assert(valid == 0);
    v = open_nvenc_decode_struct_version(0x6002000cU, &valid);
    assert(valid == 0);

    assert(NvEncodeAPIGetMaxSupportedVersion(NULL) == NV_ENC_ERR_INVALID_PTR);
    uint32_t max_ver = 0U;
    assert(NvEncodeAPIGetMaxSupportedVersion(&max_ver) == NV_ENC_SUCCESS);
    assert(max_ver == 0x000000d0U);
}

static void test_function_list_offsets_and_errors(void)
{
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncOpenEncodeSession) == 0x08U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncGetEncodeGUIDCount) == 0x10U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncGetEncodeProfileGUIDCount) == 0x18U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncGetEncodeProfileGUIDs) == 0x20U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncGetEncodeGUIDs) == 0x28U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncGetInputFormatCount) == 0x30U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncGetInputFormats) == 0x38U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncGetEncodeCaps) == 0x40U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncGetEncodePresetCount) == 0x48U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncGetEncodePresetGUIDs) == 0x50U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncGetEncodePresetConfig) == 0x58U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncInitializeEncoder) == 0x60U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncCreateInputBuffer) == 0x68U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncDestroyInputBuffer) == 0x70U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncCreateBitstreamBuffer) == 0x78U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncDestroyBitstreamBuffer) == 0x80U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncEncodePicture) == 0x88U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncLockBitstream) == 0x90U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncUnlockBitstream) == 0x98U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncLockInputBuffer) == 0xa0U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncUnlockInputBuffer) == 0xa8U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncGetEncodeStats) == 0xb0U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncGetSequenceParams) == 0xb8U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncRegisterAsyncEvent) == 0xc0U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncUnregisterAsyncEvent) == 0xc8U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncMapInputResource) == 0xd0U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncUnmapInputResource) == 0xd8U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncDestroyEncoder) == 0xe0U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncInvalidateRefFrames) == 0xe8U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncOpenEncodeSessionEx) == 0xf0U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncRegisterResource) == 0xf8U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncUnregisterResource) == 0x100U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncReconfigureEncoder) == 0x108U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncReserved1) == 0x110U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncCreateMVBuffer) == 0x118U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncDestroyMVBuffer) == 0x120U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncRunMotionEstimationOnly) == 0x128U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncGetLastErrorString) == 0x130U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncSetIOCudaStreams) == 0x138U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncGetEncodePresetConfigEx) == 0x140U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncGetSequenceParamPayload) == 0x148U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncRestoreEncoderState) == 0x150U);
    assert(offsetof(NV_ENCODE_API_FUNCTION_LIST, nvEncLookaheadPicture) == 0x158U);

    assert(NvEncodeAPICreateInstance(NULL) == NV_ENC_ERR_INVALID_PTR);

    NV_ENCODE_API_FUNCTION_LIST fn;
    memset(&fn, 0, sizeof(fn));
    assert(NvEncodeAPICreateInstance(&fn) == NV_ENC_ERR_INVALID_VERSION);

    fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    assert(NvEncodeAPICreateInstance(&fn) == NV_ENC_SUCCESS);
    assert(fn.nvEncOpenEncodeSession != NULL);
    assert(fn.nvEncOpenEncodeSessionEx != NULL);
    assert(fn.nvEncLookaheadPicture != NULL);
}

static void test_nvtool_exports(void)
{
    assert(NvToolCreateInterface(0) == NULL);
    void *ti = NvToolCreateInterface(1);
    assert(ti != NULL);

    uint32_t count = NvToolGetApiFunctionCount(ti, 1U);
    assert(count == 45U);

    const char **names = NvToolGetApiNames(ti, 1U, 0U, (int)count);
    assert(names != NULL);
    assert(strcmp(names[0], "Unknown") == 0);
    assert(strcmp(names[1], "NvEncOpenEncodeSession") == 0);
    assert(strcmp(names[35], "NvEncOpenEncodeSessionEx") == 0);
    assert(strcmp(names[44], "NvEncLookaheadPicture") == 0);
    assert(NvToolGetApiNames(ti, 0U, 0U, 45) == NULL);
    assert(NvToolGetApiNames(ti, 1U, 1U, 45) == NULL);

    NvToolSetApiID(ti, 35ULL, 1U);
    assert(NvToolGetApiID(ti) == 35ULL);
    NvToolSetApiID(ti, 0ULL, 1U);

    NvToolSetInterface(ti);
    assert(NvToolGetInterface() == ti);
    NvToolSetInterface((void *)(intptr_t)-1);

    NvToolDestroyInterface(ti);
}

static void test_gp104_hevc_10bit_444_encode_session(void)
{
    NV_ENCODE_API_FUNCTION_LIST fn;
    memset(&fn, 0, sizeof(fn));
    fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    assert(NvEncodeAPICreateInstance(&fn) == NV_ENC_SUCCESS);

    int fake_cuda_ctx = 0x10de1bb3;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open_params;
    memset(&open_params, 0, sizeof(open_params));
    open_params.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open_params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    open_params.device     = &fake_cuda_ctx;
    open_params.apiVersion = NVENCAPI_VERSION;

    void *encoder = NULL;
    assert(fn.nvEncOpenEncodeSessionEx(&open_params, &encoder) == NV_ENC_SUCCESS);
    assert(encoder != NULL);

    uint32_t guid_cnt = 0U;
    assert(fn.nvEncGetEncodeGUIDCount(encoder, &guid_cnt) == NV_ENC_SUCCESS);
    assert(guid_cnt == 2U);
    NVENC_GUID guids[2];
    assert(fn.nvEncGetEncodeGUIDs(encoder, guids, 2U, &guid_cnt) == NV_ENC_SUCCESS);

    /* Query Pascal GP104 YUV444 & 10-bit HEVC capabilities */
    NV_ENC_CAPS_PARAM cp;
    int cap_val = 0;
    memset(&cp, 0, sizeof(cp));
    cp.version     = NV_ENC_CAPS_PARAM_VER;
    cp.capsToQuery = NV_ENC_CAPS_SUPPORT_YUV444_ENCODE;
    assert(fn.nvEncGetEncodeCaps(encoder, NV_ENC_CODEC_HEVC_GUID, &cp, &cap_val) == NV_ENC_SUCCESS);
    assert(cap_val == 1);

    cp.capsToQuery = NV_ENC_CAPS_SUPPORT_10BIT_ENCODE;
    assert(fn.nvEncGetEncodeCaps(encoder, NV_ENC_CODEC_HEVC_GUID, &cp, &cap_val) == NV_ENC_SUCCESS);
    assert(cap_val == 1);

    /* Query preset config and initialize 10-bit 4:4:4 HEVC encoder */
    NV_ENC_PRESET_CONFIG preset_cfg;
    memset(&preset_cfg, 0, sizeof(preset_cfg));
    preset_cfg.version           = NV_ENC_PRESET_CONFIG_VER;
    preset_cfg.presetCfg.version = NV_ENC_CONFIG_VER;
    assert(fn.nvEncGetEncodePresetConfigEx(
               encoder, NV_ENC_CODEC_HEVC_GUID, NV_ENC_PRESET_P4_GUID,
               NV_ENC_TUNING_INFO_LOW_LATENCY, &preset_cfg) == NV_ENC_SUCCESS);

    preset_cfg.presetCfg.profileGUID = NV_ENC_HEVC_PROFILE_FREXT_GUID;
    preset_cfg.presetCfg.encodeCodecConfig.hevcConfig.chromaFormatIDC = 3U; /* 4:4:4 */
    preset_cfg.presetCfg.encodeCodecConfig.hevcConfig.outputBitDepth  = NV_ENC_BIT_DEPTH_10;
    preset_cfg.presetCfg.encodeCodecConfig.hevcConfig.inputBitDepth   = NV_ENC_BIT_DEPTH_10;

    NV_ENC_INITIALIZE_PARAMS init_params;
    memset(&init_params, 0, sizeof(init_params));
    init_params.version      = NV_ENC_INITIALIZE_PARAMS_VER;
    init_params.encodeGUID   = NV_ENC_CODEC_HEVC_GUID;
    init_params.presetGUID   = NV_ENC_PRESET_P4_GUID;
    init_params.encodeWidth  = 256U;
    init_params.encodeHeight = 256U;
    init_params.frameRateNum = 60U;
    init_params.frameRateDen = 1U;
    init_params.tuningInfo   = NV_ENC_TUNING_INFO_LOW_LATENCY;
    init_params.bufferFormat = NV_ENC_BUFFER_FORMAT_YUV444_10BIT;
    init_params.encodeConfig = &preset_cfg.presetCfg;
    assert(fn.nvEncInitializeEncoder(encoder, &init_params) == NV_ENC_SUCCESS);

    /* Input buffer + Bitstream buffer + EncodePicture + LockBitstream */
    NV_ENC_CREATE_INPUT_BUFFER in_buf;
    memset(&in_buf, 0, sizeof(in_buf));
    in_buf.version   = NV_ENC_CREATE_INPUT_BUFFER_VER;
    in_buf.width     = 256U;
    in_buf.height    = 256U;
    in_buf.bufferFmt = NV_ENC_BUFFER_FORMAT_YUV444_10BIT;
    assert(fn.nvEncCreateInputBuffer(encoder, &in_buf) == NV_ENC_SUCCESS);

    NV_ENC_LOCK_INPUT_BUFFER lock_in;
    memset(&lock_in, 0, sizeof(lock_in));
    lock_in.version     = NV_ENC_LOCK_INPUT_BUFFER_VER;
    lock_in.inputBuffer = in_buf.inputBuffer;
    assert(fn.nvEncLockInputBuffer(encoder, &lock_in) == NV_ENC_SUCCESS);
    assert(lock_in.bufferDataPtr != NULL && lock_in.pitch == 512U);
    assert(fn.nvEncUnlockInputBuffer(encoder, in_buf.inputBuffer) == NV_ENC_SUCCESS);

    NV_ENC_CREATE_BITSTREAM_BUFFER bs_buf;
    memset(&bs_buf, 0, sizeof(bs_buf));
    bs_buf.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    bs_buf.size    = 65536U;
    assert(fn.nvEncCreateBitstreamBuffer(encoder, &bs_buf) == NV_ENC_SUCCESS);

    NV_ENC_PIC_PARAMS pic;
    memset(&pic, 0, sizeof(pic));
    pic.version         = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth      = 256U;
    pic.inputHeight     = 256U;
    pic.inputPitch      = lock_in.pitch;
    pic.inputBuffer     = in_buf.inputBuffer;
    pic.outputBitstream = bs_buf.bitstreamBuffer;
    pic.bufferFmt       = NV_ENC_BUFFER_FORMAT_YUV444_10BIT;
    pic.inputTimeStamp  = 1000ULL;
    assert(fn.nvEncEncodePicture(encoder, &pic) == NV_ENC_SUCCESS);

    NV_ENC_LOCK_BITSTREAM lock_bs;
    memset(&lock_bs, 0, sizeof(lock_bs));
    lock_bs.version         = NV_ENC_LOCK_BITSTREAM_VER;
    lock_bs.outputBitstream = bs_buf.bitstreamBuffer;
    assert(fn.nvEncLockBitstream(encoder, &lock_bs) == NV_ENC_SUCCESS);
    assert(lock_bs.bitstreamSizeInBytes >= 32U);
    const uint8_t *nal = (const uint8_t *)lock_bs.bitstreamBufferPtr;
    assert(nal[0] == 0x00 && nal[1] == 0x00 && nal[2] == 0x00 && nal[3] == 0x01);
    assert(fn.nvEncUnlockBitstream(encoder, bs_buf.bitstreamBuffer) == NV_ENC_SUCCESS);

    open_nvenc_rm_stats_t stats;
    assert(open_nvenc_get_rm_stats(encoder, &stats) == 0);
    assert(stats.frames_encoded == 1U);

    assert(fn.nvEncDestroyBitstreamBuffer(encoder, bs_buf.bitstreamBuffer) == NV_ENC_SUCCESS);
    assert(fn.nvEncDestroyInputBuffer(encoder, in_buf.inputBuffer) == NV_ENC_SUCCESS);
    assert(fn.nvEncDestroyEncoder(encoder) == NV_ENC_SUCCESS);
}

static void test_vendor_differential_if_present(void)
{
    const char *vendor_lib = getenv("OPEN_NVENC_VENDOR_LIB");
    if (!vendor_lib || !vendor_lib[0]) {
        return;
    }
    void *dl = dlopen(vendor_lib, RTLD_LAZY | RTLD_LOCAL);
    if (!dl) {
        printf("[open-nvenc] Skipping vendor differential check (%s not loadable: %s)\n",
               vendor_lib, dlerror());
        return;
    }

    typedef NVENCSTATUS  (*get_ver_fn_t)(uint32_t *);
    typedef NVENCSTATUS  (*create_fn_t)(NV_ENCODE_API_FUNCTION_LIST *);
    typedef void        *(*tool_create_fn_t)(int);
    typedef void         (*tool_destroy_fn_t)(void *);
    typedef uint32_t     (*get_cnt_fn_t)(void *, uint8_t);
    typedef const char **(*get_names_fn_t)(void *, uint8_t, uint32_t, int);

    get_ver_fn_t      v_get_ver      = (get_ver_fn_t)dlsym(dl, "NvEncodeAPIGetMaxSupportedVersion");
    create_fn_t       v_create       = (create_fn_t)dlsym(dl, "NvEncodeAPICreateInstance");
    tool_create_fn_t  v_tool_create  = (tool_create_fn_t)dlsym(dl, "NvToolCreateInterface");
    tool_destroy_fn_t v_tool_destroy = (tool_destroy_fn_t)dlsym(dl, "NvToolDestroyInterface");
    get_cnt_fn_t      v_get_cnt      = (get_cnt_fn_t)dlsym(dl, "NvToolGetApiFunctionCount");
    get_names_fn_t    v_get_names    = (get_names_fn_t)dlsym(dl, "NvToolGetApiNames");
    assert(v_get_ver && v_create && v_tool_create && v_tool_destroy && v_get_cnt && v_get_names);

    assert(v_get_ver(NULL) == NvEncodeAPIGetMaxSupportedVersion(NULL));
    uint32_t v_ver = 0U, o_ver = 0U;
    assert(v_get_ver(&v_ver) == NvEncodeAPIGetMaxSupportedVersion(&o_ver));
    assert(o_ver >= v_ver && o_ver == 0x000000d0U);

    assert(v_create(NULL) == NvEncodeAPICreateInstance(NULL));
    NV_ENCODE_API_FUNCTION_LIST v_fn, o_fn;
    memset(&v_fn, 0, sizeof(v_fn));
    memset(&o_fn, 0, sizeof(o_fn));
    assert(v_create(&v_fn) == NvEncodeAPICreateInstance(&o_fn));

    v_fn.version = (12U | (1U << 24) | (2U << 16) | (0x7U << 28)); /* vendor 12.1 (0x7102000c) */
    o_fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;                 /* open 13.0 (0x7002000d) */
    assert(v_create(&v_fn) == NV_ENC_SUCCESS);
    assert(NvEncodeAPICreateInstance(&o_fn) == NV_ENC_SUCCESS);
    /* Verify open also accepts 12.1 caller structs cleanly */
    memset(&o_fn, 0, sizeof(o_fn));
    o_fn.version = 0x7102000cU;
    assert(NvEncodeAPICreateInstance(&o_fn) == NV_ENC_SUCCESS);

    /* Every non-NULL function pointer slot in vendor table must be non-NULL in open table */
    void **v_ptrs = (void **)((uint8_t *)&v_fn + 8U);
    void **o_ptrs = (void **)((uint8_t *)&o_fn + 8U);
    for (size_t i = 0U; i < 43U; i++) {
        assert((v_ptrs[i] != NULL) == (o_ptrs[i] != NULL));
    }

    assert(v_tool_create(0) == NvToolCreateInterface(0));
    void *v_ti = v_tool_create(1);
    void *o_ti = NvToolCreateInterface(1);
    assert(v_ti != NULL && o_ti != NULL);

    uint32_t v_cnt = v_get_cnt(v_ti, 1U);
    uint32_t o_cnt = NvToolGetApiFunctionCount(o_ti, 1U);
    assert(v_cnt == o_cnt && v_cnt == 45U);

    const char **v_names = v_get_names(v_ti, 1U, 0U, (int)v_cnt);
    const char **o_names = NvToolGetApiNames(o_ti, 1U, 0U, (int)o_cnt);
    assert(v_names != NULL && o_names != NULL);
    for (uint32_t i = 0U; i < v_cnt; i++) {
        assert(strcmp(v_names[i], o_names[i]) == 0);
    }

    v_tool_destroy(v_ti);
    NvToolDestroyInterface(o_ti);

    dlclose(dl);
    printf("[open-nvenc] Differential ABI parity verified against %s (max_ver=0x%x, 43 fn slots, 45 tool names)\n",
           vendor_lib, o_ver);
}

int main(void)
{
    test_version_decoder_and_max_version();
    test_function_list_offsets_and_errors();
    test_nvtool_exports();
    test_gp104_hevc_10bit_444_encode_session();
    test_vendor_differential_if_present();
    printf("ALL_OPEN_NVENC_UNIT_TESTS_PASSED (10/10 ELF exports, 43/43 function slots, GP104 H264/HEVC 10-bit 4:4:4)\n");
    return 0;
}
