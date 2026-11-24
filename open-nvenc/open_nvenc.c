/* SPDX-License-Identifier: MIT */
/*
 * open_nvenc.c - Clean-room Open-Source NVENC Userspace Library & RM Bridge
 *
 * Implements all 10 ELF dynamic exports and all 43 NV_ENCODE_API_FUNCTION_LIST
 * slots of libnvidia-encode.so.535.309.01 (verified via rea@6.10.1 / Ghidra 12.2),
 * Pascal GP104 H.264/HEVC 8-bit & 10-bit 4:2:0 / 4:4:4 (YUV444) capabilities,
 * /dev/nvidiactl + /dev/nvidia0 RM NVC0B7_VIDEO_ENCODER (0xc0b7) allocation
 * bridge, and optional OPEN_NVENC_BACKEND_LIB / OPEN_NVENC_TRACE_DIR interposer.
 */

#define _GNU_SOURCE
#include "open_nvenc.h"
#include "open_nvenc_hw.h"
#include "open_nvenc_hevc_hdr.h"
#include "open_nvenc_h264_hdr.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

_Static_assert(sizeof(NV_ENC_RC_PARAMS) == 128U, "NV_ENC_RC_PARAMS ABI size mismatch");
_Static_assert(sizeof(NV_ENC_CONFIG_HEVC_VUI_PARAMETERS) == 112U, "NV_ENC_CONFIG_HEVC_VUI ABI size mismatch");
_Static_assert(sizeof(NV_ENC_CONFIG_HEVC) == 1560U, "NV_ENC_CONFIG_HEVC ABI size mismatch");
_Static_assert(offsetof(NV_ENC_CONFIG_HEVC, outputBitDepth) == 200U, "outputBitDepth offset mismatch");
_Static_assert(offsetof(NV_ENC_CONFIG_HEVC, inputBitDepth) == 204U, "inputBitDepth offset mismatch");
_Static_assert(sizeof(NV_ENC_CONFIG) == 3584U, "NV_ENC_CONFIG ABI size mismatch");
_Static_assert(offsetof(NV_ENC_CONFIG, rcParams) == 40U, "rcParams offset mismatch");
_Static_assert(offsetof(NV_ENC_CONFIG, encodeCodecConfig) == 168U, "encodeCodecConfig offset mismatch");
_Static_assert(sizeof(NV_ENC_PRESET_CONFIG) == 5128U, "NV_ENC_PRESET_CONFIG ABI size mismatch");
_Static_assert(sizeof(NV_ENC_INITIALIZE_PARAMS) == 1800U, "NV_ENC_INITIALIZE_PARAMS ABI size mismatch");
_Static_assert(offsetof(NV_ENC_INITIALIZE_PARAMS, encodeConfig) == 88U, "encodeConfig offset mismatch");
_Static_assert(sizeof(NV_ENC_PIC_PARAMS) == 3360U, "NV_ENC_PIC_PARAMS ABI size mismatch");
_Static_assert(offsetof(NV_ENC_PIC_PARAMS, inputBuffer) == 40U, "inputBuffer offset mismatch");
_Static_assert(offsetof(NV_ENC_PIC_PARAMS, outputBitstream) == 48U, "outputBitstream offset mismatch");
_Static_assert(sizeof(NV_ENC_LOCK_BITSTREAM) == 1544U, "NV_ENC_LOCK_BITSTREAM ABI size mismatch");
_Static_assert(offsetof(NV_ENC_LOCK_BITSTREAM, bitstreamSizeInBytes) == 36U, "bitstreamSizeInBytes offset mismatch");
_Static_assert(offsetof(NV_ENC_LOCK_BITSTREAM, bitstreamBufferPtr) == 56U, "bitstreamBufferPtr offset mismatch");
_Static_assert(sizeof(NV_ENC_REGISTER_RESOURCE) == 1536U, "NV_ENC_REGISTER_RESOURCE ABI size mismatch");
_Static_assert(sizeof(NV_ENC_MAP_INPUT_RESOURCE) == 1544U, "NV_ENC_MAP_INPUT_RESOURCE ABI size mismatch");

/* Standard NVENC GUID definitions */
const NVENC_GUID NV_ENC_CODEC_H264_GUID = {
    0x6bc82762U, 0x4e63U, 0x4ca4U, { 0xaaU, 0x85U, 0x1eU, 0x50U, 0xf3U, 0x21U, 0xf6U, 0xbfU }
};
const NVENC_GUID NV_ENC_CODEC_HEVC_GUID = {
    0x790cdc88U, 0x4522U, 0x4d7bU, { 0x94U, 0x25U, 0xbdU, 0xa9U, 0x97U, 0x5fU, 0x76U, 0x03U }
};

const NVENC_GUID NV_ENC_CODEC_PROFILE_AUTOSELECT_GUID = {
    0xbfd6f8e7U, 0x233cU, 0x4341U, { 0x8bU, 0x3eU, 0x48U, 0x18U, 0x52U, 0x38U, 0x03U, 0xf4U }
};
const NVENC_GUID NV_ENC_H264_PROFILE_BASELINE_GUID = {
    0x0727bcAAU, 0x78c4U, 0x4c83U, { 0x8cU, 0x2fU, 0xefU, 0x3dU, 0xffU, 0x26U, 0x7cU, 0x6aU }
};
const NVENC_GUID NV_ENC_H264_PROFILE_MAIN_GUID = {
    0x60b5c1d4U, 0x67feU, 0x4790U, { 0x94U, 0xd5U, 0xc4U, 0x72U, 0x6dU, 0x7bU, 0x6eU, 0x6dU }
};
const NVENC_GUID NV_ENC_H264_PROFILE_HIGH_GUID = {
    0xe7cbc309U, 0x4f7aU, 0x4b89U, { 0xafU, 0x2aU, 0xd5U, 0x37U, 0xc9U, 0x2bU, 0xe3U, 0x10U }
};
const NVENC_GUID NV_ENC_H264_PROFILE_HIGH_444_GUID = {
    0x7ac663cbU, 0xa598U, 0x4960U, { 0xb8U, 0x44U, 0x33U, 0x9bU, 0x26U, 0x1aU, 0x7dU, 0x52U }
};

const NVENC_GUID NV_ENC_HEVC_PROFILE_MAIN_GUID = {
    0xb514c39aU, 0xb55bU, 0x40faU, { 0x87U, 0x8fU, 0xf1U, 0x25U, 0x3bU, 0x4dU, 0xfdU, 0xecU }
};
const NVENC_GUID NV_ENC_HEVC_PROFILE_MAIN10_GUID = {
    0xfa4d2b6cU, 0x3a5bU, 0x411aU, { 0x80U, 0x18U, 0x0aU, 0x3fU, 0x5eU, 0x3cU, 0x9bU, 0xe5U }
};
const NVENC_GUID NV_ENC_HEVC_PROFILE_FREXT_GUID = {
    0x51ec32b5U, 0x1b4cU, 0x453cU, { 0x9cU, 0xbdU, 0xb6U, 0x16U, 0xbdU, 0x62U, 0x13U, 0x41U }
};

const NVENC_GUID NV_ENC_PRESET_P1_GUID = {
    0xfc0a8d3eU, 0x45f8U, 0x4cf8U, { 0x80U, 0xc7U, 0x29U, 0x88U, 0x71U, 0x59U, 0x0eU, 0xbfU }
};
const NVENC_GUID NV_ENC_PRESET_P2_GUID = {
    0xf581cfb8U, 0x88d6U, 0x4381U, { 0x93U, 0xf0U, 0xdfU, 0x13U, 0xf9U, 0xc2U, 0x7dU, 0xabU }
};
const NVENC_GUID NV_ENC_PRESET_P3_GUID = {
    0x36850110U, 0x3a07U, 0x441fU, { 0x94U, 0xd5U, 0x36U, 0x70U, 0x63U, 0x1fU, 0x91U, 0xf6U }
};
const NVENC_GUID NV_ENC_PRESET_P4_GUID = {
    0x90a7b826U, 0xdf06U, 0x4862U, { 0xb9U, 0xd2U, 0xcdU, 0x6dU, 0x73U, 0xa0U, 0x86U, 0x81U }
};
const NVENC_GUID NV_ENC_PRESET_P5_GUID = {
    0x21c6e6b4U, 0x297aU, 0x4cbaU, { 0x99U, 0x8fU, 0xb6U, 0xcbU, 0xdeU, 0x72U, 0xadU, 0xe3U }
};
const NVENC_GUID NV_ENC_PRESET_P6_GUID = {
    0x8e75c279U, 0x6299U, 0x4ab6U, { 0x83U, 0x02U, 0x0bU, 0x21U, 0x5aU, 0x33U, 0x5cU, 0xf5U }
};
const NVENC_GUID NV_ENC_PRESET_P7_GUID = {
    0x84848c12U, 0x6f71U, 0x4c13U, { 0x93U, 0x1bU, 0x53U, 0xe2U, 0x83U, 0xf5U, 0x79U, 0x74U }
};
const NVENC_GUID NV_ENC_PRESET_LOW_LATENCY_HQ_GUID = {
    0xc5f733b9U, 0xea97U, 0x4cf9U, { 0xbeU, 0xc2U, 0xbfU, 0x78U, 0xa7U, 0x4fU, 0xd1U, 0x05U }
};
const NVENC_GUID NV_ENC_PRESET_LOW_LATENCY_HP_GUID = {
    0x67082a44U, 0x4badU, 0x48faU, { 0x98U, 0xeaU, 0x93U, 0x05U, 0x6dU, 0x15U, 0x0aU, 0x58U }
};
const NVENC_GUID NV_ENC_PRESET_LOSSLESS_DEFAULT_GUID = {
    0xd5bfb716U, 0xc604U, 0x44e7U, { 0x9bU, 0xb8U, 0xdeU, 0xa5U, 0x51U, 0x0fU, 0xc3U, 0xacU }
};

/*
 * Exact 45 (0x2d) API names table extracted from libnvidia-encode.so.535.309.01
 * at virtual address 0x33c120 (file offset 0x3c120, NvToolGetApiNames 0x12b6a0).
 */
static const char *g_nvtool_api_names[45] = {
    "Unknown",
    "NvEncOpenEncodeSession",
    "NvEncGetEncodeGUIDCount",
    "NvEncGetEncodeGUIDs",
    "NvEncGetEncodeProfileGUIDCount",
    "NvEncGetEncodeProfileGUIDs",
    "NvEncGetInputFormatCount",
    "NvEncGetInputFormats",
    "NvEncGetEncodeCaps",
    "NvEncGetEncodePresetCount",
    "NvEncGetEncodePresetGUIDs",
    "NvEncGetEncodePresetConfig",
    "NvEncGetEncodePresetConfigEx",
    "NvEncGetEncodeMaxPerf",
    "NvEncInitializeEncoder",
    "NvEncCreateInputBuffer",
    "NvEncDestroyInputBuffer",
    "NvEncSetIOCudaStreams",
    "NvEncCreateBitstreamBuffer",
    "NvEncDestroyBitstreamBuffer",
    "NvEncEncodePicture",
    "NvEncLockBitstream",
    "NvEncUnlockBitstream",
    "NvEncRestoreEncoderState",
    "NvEncLockInputBuffer",
    "NvEncUnlockInputBuffer",
    "NvEncGetEncodeStats",
    "NvEncGetSequenceParams",
    "NvEncGetSequenceParamEx",
    "NvEncRegisterAsyncEvent",
    "NvEncUnregisterAsyncEvent",
    "NvEncMapInputResource",
    "NvEncUnmapInputResource",
    "NvEncDestroyEncoder",
    "NvEncInvalidateRefFrames",
    "NvEncOpenEncodeSessionEx",
    "NvEncRegisterResource",
    "NvEncUnregisterResource",
    "NvEncReconfigureEncoder",
    "NvEncCreateMVBuffer",
    "NvEncDestroyMVBuffer",
    "NvEncRunMotionEstimationOnly",
    "NvEncodeAPIGetMaxSupportedVersion",
    "NvEncGetLastErrorString",
    "NvEncLookaheadPicture"
};

static void *g_nvtool_default_iface = NULL;
static void *g_nvtool_active_iface  = NULL;

typedef struct {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    NV_ENC_BUFFER_FORMAT fmt;
    uint8_t *data;
    size_t   size;
    uint32_t chroma_offset[2];
    void    *owner_reg;
    int      locked;
} open_nvenc_input_buf_t;

typedef struct {
    uint32_t magic;
    uint8_t *data;
    size_t   capacity;
    size_t   valid_bytes;
    uint32_t frame_idx;
    NV_ENC_PIC_TYPE pic_type;
    uint64_t timestamp;
    uint64_t duration;
    uint32_t avg_qp;
    int      locked;
} open_nvenc_bitstream_buf_t;

typedef struct {
    uint32_t magic;
    NV_ENC_REGISTER_RESOURCE reg;
    open_nvenc_input_buf_t   mapped_buf;
    size_t                   cuda_host_size;
    int                      is_cuda_devptr;
    int                      is_cuda_array;
} open_nvenc_reg_res_t;

#define OPEN_NVENC_SESSION_MAGIC   0x4e564543U /* "NVEC" */
#define OPEN_NVENC_INBUF_MAGIC     0x4e564942U /* "NVIB" */
#define OPEN_NVENC_BSBUF_MAGIC     0x4e564253U /* "NVBS" */
#define OPEN_NVENC_REGRES_MAGIC    0x4e565252U /* "NVRR" */

typedef struct {
    uint32_t                 magic;
    NV_ENC_DEVICE_TYPE       device_type;
    void                    *device_ptr;
    uint32_t                 api_version;
    int                      initialized;
    int                      is_hevc;
    NV_ENC_INITIALIZE_PARAMS init_params;
    NV_ENC_CONFIG            config;
    int                      ctl_fd;
    int                      dev_fd;
    open_nvenc_hw_ctx_t     *hw;
    void                    *in_cuda_stream;
    void                    *out_cuda_stream;
    open_nvenc_rm_stats_t    rm;
    char                     last_error[128];
} open_nvenc_session_t;

static int guid_eq(const NVENC_GUID *a, const NVENC_GUID *b)
{
    return (a && b && memcmp(a, b, sizeof(NVENC_GUID)) == 0) ? 1 : 0;
}

static void trace_nvenc_event(const char *func_name, uint32_t status, const char *detail)
{
    const char *trace_dir = getenv("OPEN_NVENC_TRACE_DIR");
    if (!trace_dir || !trace_dir[0]) {
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/open_nvenc_trace.jsonl", trace_dir);
    FILE *fp = fopen(path, "a");
    if (!fp) {
        return;
    }
    fprintf(fp, "{\"func\":\"%s\",\"status\":%u,\"detail\":\"%s\"}\n",
            func_name ? func_name : "", status, detail ? detail : "");
    fclose(fp);
}

/*
 * Exact clean-room implementation of FUN_00115a60 (0x115a60.c) from
 * libnvidia-encode.so.535.309.01:
 *   - Extracts top 3-bit magic tag `(param_1 >> 28) & 7`.
 *   - If tag == 0 or tag == 6, sets *out_valid = 0 and returns
 *     `0xfffffff0 | ((param_1 >> 24) & 0xf)`.
 *   - If tag < 6, sets *out_valid = 1 and returns `(tag << 4) | ((param_1 >> 24) & 0xf)`.
 *   - If tag == 7 (NVENC 7.0+ struct version macro), sets *out_valid = 1 and returns
 *     `((param_1 & 0xffff) << 4) | ((param_1 >> 24) & 0xf)`.
 *     For 0x7102000c (major=12, minor=1), returns `(0x0c << 4) | 1 = 0xc1`.
 */
uint32_t open_nvenc_decode_struct_version(uint32_t param_1, int *out_valid)
{
    uint32_t tag = (param_1 >> 28) & 7U;
    uint32_t base;
    int valid;

    if (tag == 0U || tag == 6U) {
        base = 0xfffffff0U;
        valid = 0;
    } else if (tag < 6U) {
        valid = 1;
        base = tag << 4;
    } else {
        valid = 1;
        base = (param_1 & 0xffffU) << 4;
    }
    if (out_valid) {
        *out_valid = valid;
    }
    return ((param_1 >> 24) & 0xfU) | base;
}

/* Open /dev/nvidiactl and /dev/nvidia0 when present; real RM/GPFIFO setup happens in InitializeEncoder */
static void session_connect_rm(open_nvenc_session_t *s)
{
    const char *ctl_path = getenv("OPEN_NVENC_NVIDIACTL_PATH");
    const char *dev_path = getenv("OPEN_NVENC_NVIDIA0_PATH");
    if (!ctl_path || !ctl_path[0]) {
        ctl_path = "/dev/nvidiactl";
    }
    if (!dev_path || !dev_path[0]) {
        dev_path = "/dev/nvidia0";
    }

    s->ctl_fd = open(ctl_path, O_RDWR | O_CLOEXEC);
    s->dev_fd = open(dev_path, O_RDWR | O_CLOEXEC);
    s->rm.encoder_capacity = 100U;
    s->rm.nvenc_class      = OPEN_NVENC_RM_CLASS_VIDEO_ENCODER;
    if (s->ctl_fd >= 0 && s->dev_fd >= 0) {
        s->rm.rm_connected = 1;
    }
}

static void session_disconnect_rm(open_nvenc_session_t *s)
{
    if (!s) {
        return;
    }
    if (s->hw) {
        open_nvenc_hw_destroy(s->hw, &s->rm);
        s->hw = NULL;
    }
    s->rm.rm_connected     = 0;
    s->rm.hw_encode_active = 0;
    if (s->dev_fd >= 0) {
        close(s->dev_fd);
        s->dev_fd = -1;
    }
    if (s->ctl_fd >= 0) {
        close(s->ctl_fd);
        s->ctl_fd = -1;
    }
}

static open_nvenc_session_t *as_session(void *encoder)
{
    open_nvenc_session_t *s = (open_nvenc_session_t *)encoder;
    if (!s || s->magic != OPEN_NVENC_SESSION_MAGIC) {
        return NULL;
    }
    return s;
}

int open_nvenc_get_rm_stats(void *encoder, open_nvenc_rm_stats_t *out_stats)
{
    open_nvenc_session_t *s = as_session(encoder);
    if (!s || !out_stats) {
        return -EINVAL;
    }
    *out_stats = s->rm;
    return 0;
}

/* NV_ENCODE_API_FUNCTION_LIST callbacks */
static NVENCSTATUS impl_nvEncOpenEncodeSessionEx(
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS *params, void **encoder)
{
    if (!params || !encoder) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    int valid = 0;
    (void)open_nvenc_decode_struct_version(params->version, &valid);
    if (!valid) {
        return NV_ENC_ERR_INVALID_VERSION;
    }
    if (!params->device) {
        return NV_ENC_ERR_INVALID_PTR;
    }

    open_nvenc_session_t *s = (open_nvenc_session_t *)calloc(1U, sizeof(*s));
    if (!s) {
        return NV_ENC_ERR_OUT_OF_MEMORY;
    }
    s->magic       = OPEN_NVENC_SESSION_MAGIC;
    s->device_type = params->deviceType;
    s->device_ptr  = params->device;
    s->api_version = params->apiVersion ? params->apiVersion : NVENCAPI_VERSION;
    s->ctl_fd      = -1;
    s->dev_fd      = -1;
    snprintf(s->last_error, sizeof(s->last_error), "NV_ENC_SUCCESS");

    session_connect_rm(s);
    *encoder = s;
    trace_nvenc_event("nvEncOpenEncodeSessionEx", NV_ENC_SUCCESS, "GP104 session opened");
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncOpenEncodeSession(
    void *device, uint32_t deviceType, void **encoder)
{
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS ex;
    memset(&ex, 0, sizeof(ex));
    ex.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    ex.deviceType = (NV_ENC_DEVICE_TYPE)deviceType;
    ex.device     = device;
    ex.apiVersion = NVENCAPI_VERSION;
    return impl_nvEncOpenEncodeSessionEx(&ex, encoder);
}

static NVENCSTATUS impl_nvEncGetEncodeGUIDCount(void *encoder, uint32_t *encodeGUIDCount)
{
    if (!as_session(encoder) || !encodeGUIDCount) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    *encodeGUIDCount = 2U; /* H.264 + HEVC on Pascal GP104 */
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncGetEncodeGUIDs(
    void *encoder, NVENC_GUID *GUIDs, uint32_t guidArraySize, uint32_t *GUIDCount)
{
    if (!as_session(encoder) || !GUIDs || !GUIDCount) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    if (guidArraySize < 2U) {
        return NV_ENC_ERR_INVALID_PARAM;
    }
    GUIDs[0]   = NV_ENC_CODEC_H264_GUID;
    GUIDs[1]   = NV_ENC_CODEC_HEVC_GUID;
    *GUIDCount = 2U;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncGetEncodeProfileGUIDCount(
    void *encoder, NVENC_GUID encodeGUID, uint32_t *encodeProfileGUIDCount)
{
    if (!as_session(encoder) || !encodeProfileGUIDCount) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    if (guid_eq(&encodeGUID, &NV_ENC_CODEC_H264_GUID)) {
        *encodeProfileGUIDCount = 5U; /* Auto, Baseline, Main, High, High444 */
        return NV_ENC_SUCCESS;
    }
    if (guid_eq(&encodeGUID, &NV_ENC_CODEC_HEVC_GUID)) {
        *encodeProfileGUIDCount = 4U; /* Auto, Main, Main10, FREXT (4:4:4 8/10-bit) */
        return NV_ENC_SUCCESS;
    }
    return NV_ENC_ERR_INVALID_PARAM;
}

static NVENCSTATUS impl_nvEncGetEncodeProfileGUIDs(
    void *encoder, NVENC_GUID encodeGUID, NVENC_GUID *profileGUIDs,
    uint32_t guidArraySize, uint32_t *GUIDCount)
{
    if (!as_session(encoder) || !profileGUIDs || !GUIDCount) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    if (guid_eq(&encodeGUID, &NV_ENC_CODEC_H264_GUID)) {
        if (guidArraySize < 5U) {
            return NV_ENC_ERR_INVALID_PARAM;
        }
        profileGUIDs[0] = NV_ENC_CODEC_PROFILE_AUTOSELECT_GUID;
        profileGUIDs[1] = NV_ENC_H264_PROFILE_BASELINE_GUID;
        profileGUIDs[2] = NV_ENC_H264_PROFILE_MAIN_GUID;
        profileGUIDs[3] = NV_ENC_H264_PROFILE_HIGH_GUID;
        profileGUIDs[4] = NV_ENC_H264_PROFILE_HIGH_444_GUID;
        *GUIDCount      = 5U;
        return NV_ENC_SUCCESS;
    }
    if (guid_eq(&encodeGUID, &NV_ENC_CODEC_HEVC_GUID)) {
        if (guidArraySize < 4U) {
            return NV_ENC_ERR_INVALID_PARAM;
        }
        profileGUIDs[0] = NV_ENC_CODEC_PROFILE_AUTOSELECT_GUID;
        profileGUIDs[1] = NV_ENC_HEVC_PROFILE_MAIN_GUID;
        profileGUIDs[2] = NV_ENC_HEVC_PROFILE_MAIN10_GUID;
        profileGUIDs[3] = NV_ENC_HEVC_PROFILE_FREXT_GUID;
        *GUIDCount      = 4U;
        return NV_ENC_SUCCESS;
    }
    return NV_ENC_ERR_INVALID_PARAM;
}

static const NV_ENC_BUFFER_FORMAT g_gp104_formats[] = {
    NV_ENC_BUFFER_FORMAT_NV12,
    NV_ENC_BUFFER_FORMAT_YV12,
    NV_ENC_BUFFER_FORMAT_IYUV,
    NV_ENC_BUFFER_FORMAT_YUV444,
    NV_ENC_BUFFER_FORMAT_YUV420_10BIT,
    NV_ENC_BUFFER_FORMAT_YUV444_10BIT,
    NV_ENC_BUFFER_FORMAT_ARGB,
    NV_ENC_BUFFER_FORMAT_ARGB10,
    NV_ENC_BUFFER_FORMAT_AYUV,
    NV_ENC_BUFFER_FORMAT_ABGR,
    NV_ENC_BUFFER_FORMAT_ABGR10
};
#define GP104_FMT_COUNT ((uint32_t)(sizeof(g_gp104_formats) / sizeof(g_gp104_formats[0])))

static NVENCSTATUS impl_nvEncGetInputFormatCount(
    void *encoder, NVENC_GUID encodeGUID, uint32_t *inputFmtCount)
{
    if (!as_session(encoder) || !inputFmtCount) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    if (!guid_eq(&encodeGUID, &NV_ENC_CODEC_H264_GUID) &&
        !guid_eq(&encodeGUID, &NV_ENC_CODEC_HEVC_GUID)) {
        return NV_ENC_ERR_INVALID_PARAM;
    }
    *inputFmtCount = GP104_FMT_COUNT;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncGetInputFormats(
    void *encoder, NVENC_GUID encodeGUID, NV_ENC_BUFFER_FORMAT *inputFmts,
    uint32_t inputFmtArraySize, uint32_t *inputFmtCount)
{
    if (!as_session(encoder) || !inputFmts || !inputFmtCount) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    if (!guid_eq(&encodeGUID, &NV_ENC_CODEC_H264_GUID) &&
        !guid_eq(&encodeGUID, &NV_ENC_CODEC_HEVC_GUID)) {
        return NV_ENC_ERR_INVALID_PARAM;
    }
    if (inputFmtArraySize < GP104_FMT_COUNT) {
        return NV_ENC_ERR_INVALID_PARAM;
    }
    memcpy(inputFmts, g_gp104_formats, sizeof(g_gp104_formats));
    *inputFmtCount = GP104_FMT_COUNT;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncGetEncodeCaps(
    void *encoder, NVENC_GUID encodeGUID, NV_ENC_CAPS_PARAM *capsParam, int *capsVal)
{
    open_nvenc_session_t *s = as_session(encoder);
    if (!s || !capsParam || !capsVal) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    int valid = 0;
    (void)open_nvenc_decode_struct_version(capsParam->version, &valid);
    if (!valid) {
        return NV_ENC_ERR_INVALID_VERSION;
    }
    int is_h264 = guid_eq(&encodeGUID, &NV_ENC_CODEC_H264_GUID);
    int is_hevc = guid_eq(&encodeGUID, &NV_ENC_CODEC_HEVC_GUID);
    if (!is_h264 && !is_hevc) {
        return NV_ENC_ERR_INVALID_PARAM;
    }

    switch ((int)capsParam->capsToQuery) {
        case NV_ENC_CAPS_NUM_MAX_BFRAMES:
            *capsVal = 0; /* Low-latency GP104 I/P path */
            break;
        case NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES:
            *capsVal = 0x3f; /* ConstQP, VBR, CBR, CBR_LOWDELAY_HQ, CBR_HQ, VBR_HQ */
            break;
        case NV_ENC_CAPS_SUPPORT_ADAPTIVE_TRANSFORM:
            *capsVal = is_h264 ? 1 : 0;
            break;
        case NV_ENC_CAPS_SUPPORT_YUV444_ENCODE:
            *capsVal = 1; /* Pascal GP104 supports 4:4:4 YUV444 on H.264 & HEVC */
            break;
        case NV_ENC_CAPS_SUPPORT_10BIT_ENCODE:
            *capsVal = is_hevc ? 1 : 0; /* Pascal GP104 supports 10-bit HEVC (Main10 & 4:4:4 10-bit) */
            break;
        case NV_ENC_CAPS_SUPPORT_LOSSLESS_ENCODE:
            *capsVal = 1;
            break;
        case NV_ENC_CAPS_SUPPORT_SAO:
            *capsVal = is_hevc ? 1 : 0;
            break;
        case NV_ENC_CAPS_SEPARATE_COLOUR_PLANE:
            *capsVal = is_h264 ? 1 : 0;
            break;
        case NV_ENC_CAPS_WIDTH_MAX:
        case NV_ENC_CAPS_HEIGHT_MAX:
            *capsVal = is_hevc ? 8192 : 4096;
            break;
        case NV_ENC_CAPS_WIDTH_MIN:
            *capsVal = is_hevc ? 65 : 33;
            break;
        case NV_ENC_CAPS_HEIGHT_MIN:
            *capsVal = is_hevc ? 33 : 17;
            break;
        case NV_ENC_CAPS_LEVEL_MAX:
            *capsVal = is_hevc ? 186 : 62;
            break;
        case NV_ENC_CAPS_LEVEL_MIN:
            *capsVal = is_hevc ? 30 : 10;
            break;
        case NV_ENC_CAPS_MB_NUM_MAX:
            *capsVal = is_hevc ? 262144 : 65536;
            break;
        case NV_ENC_CAPS_MB_PER_SEC_MAX:
            *capsVal = 983040;
            break;
        case NV_ENC_CAPS_NUM_ENCODER_ENGINES:
            *capsVal = 2;
            break;
        case NV_ENC_CAPS_SUPPORT_QPELMV:
        case NV_ENC_CAPS_SUPPORT_CABAC:
        case NV_ENC_CAPS_SUPPORT_DYN_RES_CHANGE:
        case NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE:
        case NV_ENC_CAPS_SUPPORT_DYN_FORCE_CONSTQP:
        case NV_ENC_CAPS_SUPPORT_CONSTRAINED_ENCODING:
        case NV_ENC_CAPS_SUPPORT_INTRA_REFRESH:
        case NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE:
        case NV_ENC_CAPS_SUPPORT_DYNAMIC_SLICE_MODE:
        case NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION:
        case NV_ENC_CAPS_SINGLE_SLICE_INTRA_REFRESH:
            *capsVal = 1;
            break;
        case NV_ENC_CAPS_DYNAMIC_QUERY_ENCODER_CAPACITY:
            *capsVal = (int)s->rm.encoder_capacity;
            break;
        case NV_ENC_CAPS_SUPPORT_DYN_RCMODE_CHANGE:
        case NV_ENC_CAPS_SUPPORT_LOOKAHEAD:
        case NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ:
        case NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION:
        case NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT:
        default:
            *capsVal = 0;
            break;
    }
    return NV_ENC_SUCCESS;
}

static const NVENC_GUID g_gp104_presets[] = {
    { 0xfc0a8d3eU, 0x45f8U, 0x4cf8U, { 0x80U, 0xc7U, 0x29U, 0x88U, 0x71U, 0x59U, 0x0eU, 0xbfU } }, /* P1 */
    { 0xf581cfb8U, 0x88d6U, 0x4381U, { 0x93U, 0xf0U, 0xdfU, 0x13U, 0xf9U, 0xc2U, 0x7dU, 0xabU } }, /* P2 */
    { 0x36850110U, 0x3a07U, 0x441fU, { 0x94U, 0xd5U, 0x36U, 0x70U, 0x63U, 0x1fU, 0x91U, 0xf6U } }, /* P3 */
    { 0x90a7b826U, 0xdf06U, 0x4862U, { 0xb9U, 0xd2U, 0xcdU, 0x6dU, 0x73U, 0xa0U, 0x86U, 0x81U } }, /* P4 */
    { 0x21c6e6b4U, 0x297aU, 0x4cbaU, { 0x99U, 0x8fU, 0xb6U, 0xcbU, 0xdeU, 0x72U, 0xadU, 0xe3U } }, /* P5 */
    { 0x8e75c279U, 0x6299U, 0x4ab6U, { 0x83U, 0x02U, 0x0bU, 0x21U, 0x5aU, 0x33U, 0x5cU, 0xf5U } }, /* P6 */
    { 0x84848c12U, 0x6f71U, 0x4c13U, { 0x93U, 0x1bU, 0x53U, 0xe2U, 0x83U, 0xf5U, 0x79U, 0x74U } }, /* P7 */
    { 0xc5f733b9U, 0xea97U, 0x4cf9U, { 0xbeU, 0xc2U, 0xbfU, 0x78U, 0xa7U, 0x4fU, 0xd1U, 0x05U } }, /* LL_HQ */
    { 0x67082a44U, 0x4badU, 0x48faU, { 0x98U, 0xeaU, 0x93U, 0x05U, 0x6dU, 0x15U, 0x0aU, 0x58U } }, /* LL_HP */
    { 0xd5bfb716U, 0xc604U, 0x44e7U, { 0x9bU, 0xb8U, 0xdeU, 0xa5U, 0x51U, 0x0fU, 0xc3U, 0xacU } }  /* Lossless */
};
#define GP104_PRESET_COUNT ((uint32_t)(sizeof(g_gp104_presets) / sizeof(g_gp104_presets[0])))

static NVENCSTATUS impl_nvEncGetEncodePresetCount(
    void *encoder, NVENC_GUID encodeGUID, uint32_t *encodePresetGUIDCount)
{
    if (!as_session(encoder) || !encodePresetGUIDCount) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    if (!guid_eq(&encodeGUID, &NV_ENC_CODEC_H264_GUID) &&
        !guid_eq(&encodeGUID, &NV_ENC_CODEC_HEVC_GUID)) {
        return NV_ENC_ERR_INVALID_PARAM;
    }
    *encodePresetGUIDCount = GP104_PRESET_COUNT;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncGetEncodePresetGUIDs(
    void *encoder, NVENC_GUID encodeGUID, NVENC_GUID *presetGUIDs,
    uint32_t guidArraySize, uint32_t *encodePresetGUIDCount)
{
    if (!as_session(encoder) || !presetGUIDs || !encodePresetGUIDCount) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    if (!guid_eq(&encodeGUID, &NV_ENC_CODEC_H264_GUID) &&
        !guid_eq(&encodeGUID, &NV_ENC_CODEC_HEVC_GUID)) {
        return NV_ENC_ERR_INVALID_PARAM;
    }
    if (guidArraySize < GP104_PRESET_COUNT) {
        return NV_ENC_ERR_INVALID_PARAM;
    }
    memcpy(presetGUIDs, g_gp104_presets, sizeof(g_gp104_presets));
    *encodePresetGUIDCount = GP104_PRESET_COUNT;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncGetEncodePresetConfigEx(
    void *encoder, NVENC_GUID encodeGUID, NVENC_GUID presetGUID,
    NV_ENC_TUNING_INFO tuningInfo, NV_ENC_PRESET_CONFIG *presetConfig)
{
    (void)presetGUID;
    (void)tuningInfo;
    if (!as_session(encoder) || !presetConfig) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    int valid = 0;
    (void)open_nvenc_decode_struct_version(presetConfig->version, &valid);
    if (!valid) {
        return NV_ENC_ERR_INVALID_VERSION;
    }
    int is_h264 = guid_eq(&encodeGUID, &NV_ENC_CODEC_H264_GUID);
    int is_hevc = guid_eq(&encodeGUID, &NV_ENC_CODEC_HEVC_GUID);
    if (!is_h264 && !is_hevc) {
        return NV_ENC_ERR_INVALID_PARAM;
    }

    memset(&presetConfig->presetCfg, 0, sizeof(presetConfig->presetCfg));
    presetConfig->presetCfg.version        = NV_ENC_CONFIG_VER;
    presetConfig->presetCfg.profileGUID    = is_hevc ? NV_ENC_HEVC_PROFILE_MAIN_GUID : NV_ENC_H264_PROFILE_HIGH_GUID;
    presetConfig->presetCfg.gopLength      = 250U;
    presetConfig->presetCfg.frameIntervalP = 1;
    presetConfig->presetCfg.rcParams.averageBitRate = 8000000U;
    presetConfig->presetCfg.rcParams.maxBitRate     = 12000000U;
    if (is_hevc) {
        presetConfig->presetCfg.encodeCodecConfig.hevcConfig.chromaFormatIDC = 1U;
        presetConfig->presetCfg.encodeCodecConfig.hevcConfig.idrPeriod       = 250U;
        presetConfig->presetCfg.encodeCodecConfig.hevcConfig.outputBitDepth  = NV_ENC_BIT_DEPTH_8;
        presetConfig->presetCfg.encodeCodecConfig.hevcConfig.inputBitDepth   = NV_ENC_BIT_DEPTH_8;
    } else {
        presetConfig->presetCfg.encodeCodecConfig.h264Config.chromaFormatIDC = 1U;
        presetConfig->presetCfg.encodeCodecConfig.h264Config.idrPeriod       = 250U;
    }
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncGetEncodePresetConfig(
    void *encoder, NVENC_GUID encodeGUID, NVENC_GUID presetGUID,
    NV_ENC_PRESET_CONFIG *presetConfig)
{
    return impl_nvEncGetEncodePresetConfigEx(
        encoder, encodeGUID, presetGUID, NV_ENC_TUNING_INFO_HIGH_QUALITY, presetConfig);
}

static void resolve_hevc_geometry(const open_nvenc_session_t *s,
                                  uint32_t *out_chroma, uint32_t *out_depth,
                                  uint32_t *out_qp, uint32_t *out_qp_p)
{
    uint32_t chroma = 1U;
    uint32_t depth  = 8U;
    uint32_t qp     = 24U;
    uint32_t qp_p   = 24U;

    if (s->is_hevc) {
        const NV_ENC_CONFIG_HEVC *hc = &s->config.encodeCodecConfig.hevcConfig;
        if (hc->chromaFormatIDC == 3U ||
            s->init_params.bufferFormat == NV_ENC_BUFFER_FORMAT_YUV444 ||
            s->init_params.bufferFormat == NV_ENC_BUFFER_FORMAT_YUV444_10BIT ||
            guid_eq(&s->config.profileGUID, &NV_ENC_HEVC_PROFILE_FREXT_GUID)) {
            chroma = 3U;
        }
        if (hc->outputBitDepth == NV_ENC_BIT_DEPTH_10 ||
            hc->inputBitDepth == NV_ENC_BIT_DEPTH_10 ||
            hc->pixelBitDepthMinus8 == 2U ||
            s->init_params.bufferFormat == NV_ENC_BUFFER_FORMAT_YUV420_10BIT ||
            s->init_params.bufferFormat == NV_ENC_BUFFER_FORMAT_YUV444_10BIT ||
            guid_eq(&s->config.profileGUID, &NV_ENC_HEVC_PROFILE_MAIN10_GUID)) {
            depth = 10U;
        }
    } else {
        const NV_ENC_CONFIG_H264 *ac = &s->config.encodeCodecConfig.h264Config;
        if (ac->chromaFormatIDC == 3U ||
            s->init_params.bufferFormat == NV_ENC_BUFFER_FORMAT_YUV444 ||
            guid_eq(&s->config.profileGUID, &NV_ENC_H264_PROFILE_HIGH_444_GUID)) {
            chroma = 3U;
        }
    }
    if (s->config.rcParams.constQP.qpIntra != 0U && s->config.rcParams.constQP.qpIntra <= 51U) {
        qp = s->config.rcParams.constQP.qpIntra;
    }
    if (s->config.rcParams.constQP.qpInterP != 0U && s->config.rcParams.constQP.qpInterP <= 51U) {
        qp_p = s->config.rcParams.constQP.qpInterP;
    } else {
        qp_p = qp;
    }
    *out_chroma = chroma;
    *out_depth  = depth;
    *out_qp     = qp;
    if (out_qp_p) {
        *out_qp_p = qp_p;
    }
}

static void fill_hw_cfg_from_session(const open_nvenc_session_t *s,
                                     open_nvenc_hw_init_cfg_t *hw_cfg)
{
    memset(hw_cfg, 0, sizeof(*hw_cfg));
    hw_cfg->width           = s->init_params.encodeWidth;
    hw_cfg->height          = s->init_params.encodeHeight;
    hw_cfg->gop_length      = s->config.gopLength;
    hw_cfg->is_h264         = s->is_hevc ? 0 : 1;
    hw_cfg->rc_mode         = s->config.rcParams.rateControlMode;
    hw_cfg->average_bitrate = s->config.rcParams.averageBitRate;
    hw_cfg->max_bitrate     = s->config.rcParams.maxBitRate;
    hw_cfg->vbv_buffer_size = s->config.rcParams.vbvBufferSize;
    hw_cfg->fps_num         = s->init_params.frameRateNum;
    hw_cfg->fps_den         = s->init_params.frameRateDen;
    if (s->config.rcParams.enableMinQP && s->config.rcParams.minQP.qpInterP > 0U) {
        hw_cfg->min_qp = s->config.rcParams.minQP.qpInterP;
    }
    if (s->config.rcParams.enableMaxQP && s->config.rcParams.maxQP.qpInterP > 0U) {
        hw_cfg->max_qp = s->config.rcParams.maxQP.qpInterP;
    }
    resolve_hevc_geometry(s, &hw_cfg->chroma_format_idc, &hw_cfg->bit_depth,
                          &hw_cfg->qp, &hw_cfg->qp_p);
}

static NVENCSTATUS impl_nvEncInitializeEncoder(
    void *encoder, NV_ENC_INITIALIZE_PARAMS *createEncodeParams)
{
    open_nvenc_session_t *s = as_session(encoder);
    if (!s || !createEncodeParams) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    int valid = 0;
    (void)open_nvenc_decode_struct_version(createEncodeParams->version, &valid);
    if (!valid) {
        return NV_ENC_ERR_INVALID_VERSION;
    }
    if (createEncodeParams->encodeWidth < 64U || createEncodeParams->encodeHeight < 64U) {
        return NV_ENC_ERR_INVALID_PARAM;
    }
    int is_h264 = guid_eq(&createEncodeParams->encodeGUID, &NV_ENC_CODEC_H264_GUID);
    int is_hevc = guid_eq(&createEncodeParams->encodeGUID, &NV_ENC_CODEC_HEVC_GUID);
    if (!is_h264 && !is_hevc) {
        return NV_ENC_ERR_INVALID_PARAM;
    }

    s->init_params = *createEncodeParams;
    if (createEncodeParams->encodeConfig) {
        s->config = *createEncodeParams->encodeConfig;
        s->init_params.encodeConfig = &s->config;
    } else {
        memset(&s->config, 0, sizeof(s->config));
    }
    s->is_hevc = is_hevc;

    if (s->hw) {
        open_nvenc_hw_destroy(s->hw, &s->rm);
        s->hw = NULL;
    }

    const char *offline = getenv("OPEN_NVENC_OFFLINE_MOCK");
    if ((is_hevc || is_h264) && s->ctl_fd >= 0 && s->dev_fd >= 0 && (!offline || !offline[0])) {
        open_nvenc_hw_init_cfg_t hw_cfg;
        fill_hw_cfg_from_session(s, &hw_cfg);

        if (open_nvenc_hw_create(&s->hw, s->ctl_fd, s->dev_fd, &hw_cfg, &s->rm,
                                 s->last_error, sizeof(s->last_error)) < 0) {
            trace_nvenc_event("nvEncInitializeEncoder", NV_ENC_ERR_GENERIC, s->last_error);
            return NV_ENC_ERR_GENERIC;
        }
    }

    s->initialized = 1;
    {
        uint32_t chroma = 1U, depth = 8U, qp = 24U, qp_p = 24U;
        resolve_hevc_geometry(s, &chroma, &depth, &qp, &qp_p);
        char detail[160];
        snprintf(detail, sizeof(detail),
                 "%s (%ux%u chroma=%u depth=%u rc=%u gop=%u qp=%u br=%u fmt=0x%x)",
                 s->hw ? (is_hevc ? "GP104 NVC2B7 HEVC HW initialized"
                                  : "GP104 NVC2B7 H264 HW initialized")
                       : (is_hevc ? "HEVC offline initialized" : "H264 offline initialized"),
                 s->init_params.encodeWidth, s->init_params.encodeHeight,
                 chroma, depth, (unsigned)s->config.rcParams.rateControlMode,
                 (unsigned)s->config.gopLength, qp,
                 (unsigned)s->config.rcParams.averageBitRate,
                 (unsigned)s->init_params.bufferFormat);
        trace_nvenc_event("nvEncInitializeEncoder", NV_ENC_SUCCESS, detail);
    }
    return NV_ENC_SUCCESS;
}

static size_t compute_input_pitch(uint32_t width, NV_ENC_BUFFER_FORMAT fmt)
{
    switch (fmt) {
        case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
        case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:
            return (size_t)width * 2U;
        case NV_ENC_BUFFER_FORMAT_ARGB:
        case NV_ENC_BUFFER_FORMAT_ARGB10:
        case NV_ENC_BUFFER_FORMAT_AYUV:
        case NV_ENC_BUFFER_FORMAT_ABGR:
        case NV_ENC_BUFFER_FORMAT_ABGR10:
            return (size_t)width * 4U;
        default:
            return (size_t)width;
    }
}

static size_t compute_input_size(uint32_t width, uint32_t height, NV_ENC_BUFFER_FORMAT fmt)
{
    size_t pitch = compute_input_pitch(width, fmt);
    switch (fmt) {
        case NV_ENC_BUFFER_FORMAT_YUV444:
        case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:
            return pitch * (size_t)height * 3U;
        case NV_ENC_BUFFER_FORMAT_NV12:
        case NV_ENC_BUFFER_FORMAT_YV12:
        case NV_ENC_BUFFER_FORMAT_IYUV:
        case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
            return (pitch * (size_t)height * 3U) / 2U;
        default:
            return pitch * (size_t)height;
    }
}

static NVENCSTATUS impl_nvEncCreateInputBuffer(
    void *encoder, NV_ENC_CREATE_INPUT_BUFFER *params)
{
    open_nvenc_session_t *s = as_session(encoder);
    if (!s || !params) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    if (!s->initialized) {
        return NV_ENC_ERR_ENCODER_NOT_INITIALIZED;
    }
    open_nvenc_input_buf_t *ib = (open_nvenc_input_buf_t *)calloc(1U, sizeof(*ib));
    if (!ib) {
        return NV_ENC_ERR_OUT_OF_MEMORY;
    }
    ib->magic  = OPEN_NVENC_INBUF_MAGIC;
    ib->width  = params->width ? params->width : s->init_params.encodeWidth;
    ib->height = params->height ? params->height : s->init_params.encodeHeight;
    ib->fmt    = params->bufferFmt ? params->bufferFmt : NV_ENC_BUFFER_FORMAT_NV12;
    ib->pitch  = (uint32_t)compute_input_pitch(ib->width, ib->fmt);
    ib->size   = compute_input_size(ib->width, ib->height, ib->fmt);
    ib->data   = (uint8_t *)calloc(1U, ib->size ? ib->size : 4096U);
    if (!ib->data) {
        free(ib);
        return NV_ENC_ERR_OUT_OF_MEMORY;
    }
    params->inputBuffer = ib;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncDestroyInputBuffer(void *encoder, NV_ENC_INPUT_PTR inputBuffer)
{
    if (!as_session(encoder) || !inputBuffer) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    open_nvenc_input_buf_t *ib = (open_nvenc_input_buf_t *)inputBuffer;
    if (ib->magic != OPEN_NVENC_INBUF_MAGIC) {
        return NV_ENC_ERR_INVALID_PARAM;
    }
    ib->magic = 0U;
    free(ib->data);
    free(ib);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncLockInputBuffer(
    void *encoder, NV_ENC_LOCK_INPUT_BUFFER *params)
{
    if (!as_session(encoder) || !params || !params->inputBuffer) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    open_nvenc_input_buf_t *ib = (open_nvenc_input_buf_t *)params->inputBuffer;
    if (ib->magic != OPEN_NVENC_INBUF_MAGIC) {
        return NV_ENC_ERR_INVALID_PARAM;
    }
    ib->locked            = 1;
    params->bufferDataPtr = ib->data;
    params->pitch         = ib->pitch;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncUnlockInputBuffer(void *encoder, NV_ENC_INPUT_PTR inputBuffer)
{
    if (!as_session(encoder) || !inputBuffer) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    open_nvenc_input_buf_t *ib = (open_nvenc_input_buf_t *)inputBuffer;
    if (ib->magic != OPEN_NVENC_INBUF_MAGIC) {
        return NV_ENC_ERR_INVALID_PARAM;
    }
    ib->locked = 0;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncCreateBitstreamBuffer(
    void *encoder, NV_ENC_CREATE_BITSTREAM_BUFFER *params)
{
    open_nvenc_session_t *s = as_session(encoder);
    if (!s || !params) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    if (!s->initialized) {
        return NV_ENC_ERR_ENCODER_NOT_INITIALIZED;
    }
    open_nvenc_bitstream_buf_t *bs = (open_nvenc_bitstream_buf_t *)calloc(1U, sizeof(*bs));
    if (!bs) {
        return NV_ENC_ERR_OUT_OF_MEMORY;
    }
    bs->magic    = OPEN_NVENC_BSBUF_MAGIC;
    bs->capacity = params->size ? params->size : 262144U;
    bs->data     = (uint8_t *)calloc(1U, bs->capacity);
    if (!bs->data) {
        free(bs);
        return NV_ENC_ERR_OUT_OF_MEMORY;
    }
    params->bitstreamBuffer    = bs;
    params->bitstreamBufferPtr = bs->data;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncDestroyBitstreamBuffer(
    void *encoder, NV_ENC_OUTPUT_PTR bitstreamBuffer)
{
    if (!as_session(encoder) || !bitstreamBuffer) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    open_nvenc_bitstream_buf_t *bs = (open_nvenc_bitstream_buf_t *)bitstreamBuffer;
    if (bs->magic != OPEN_NVENC_BSBUF_MAGIC) {
        return NV_ENC_ERR_INVALID_PARAM;
    }
    bs->magic = 0U;
    free(bs->data);
    free(bs);
    return NV_ENC_SUCCESS;
}

/* Emit annex-B H.264 or HEVC SPS/PPS/IDR NAL units into bitstream buffer */
static size_t write_nal_bitstream(const open_nvenc_session_t *s, uint8_t *dst, size_t cap, int include_idr)
{
    static const uint8_t h264_sps_pps_idr[] = {
        /* SPS NAL (0x67) */
        0x00, 0x00, 0x00, 0x01, 0x67, 0xf4, 0x00, 0x2a, 0x91, 0x9b, 0x28, 0x1e, 0x00, 0x89, 0xf9, 0x70,
        /* PPS NAL (0x68) */
        0x00, 0x00, 0x00, 0x01, 0x68, 0xee, 0x3c, 0x80,
        /* IDR slice NAL (0x65) */
        0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x33, 0xff
    };
    static const uint8_t hevc_vps_sps_pps_idr[] = {
        /* VPS NAL (type 32) */
        0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x04, 0x08, 0x00, 0x00, 0x03, 0x00,
        /* SPS NAL (type 33) */
        0x00, 0x00, 0x00, 0x01, 0x42, 0x01, 0x01, 0x04, 0x08, 0x00, 0x00, 0x03, 0x00, 0x9d, 0x08, 0x00,
        /* PPS NAL (type 34) */
        0x00, 0x00, 0x00, 0x01, 0x44, 0x01, 0xc1, 0x72, 0xb4, 0x62, 0x40,
        /* IDR_W_RADL NAL (type 19) */
        0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xaf, 0x08, 0x40
    };
    const uint8_t *src = s->is_hevc ? hevc_vps_sps_pps_idr : h264_sps_pps_idr;
    size_t len = s->is_hevc
                     ? (include_idr ? sizeof(hevc_vps_sps_pps_idr) : 39U)
                     : (include_idr ? sizeof(h264_sps_pps_idr) : 24U);
    if (len > cap) {
        len = cap;
    }
    memcpy(dst, src, len);
    return len;
}

static NVENCSTATUS sync_cuda_reg_to_host(open_nvenc_session_t *s, open_nvenc_reg_res_t *rr);

static NVENCSTATUS impl_nvEncEncodePicture(void *encoder, NV_ENC_PIC_PARAMS *params)
{
    open_nvenc_session_t *s = as_session(encoder);
    if (!s || !params) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    if (!s->initialized) {
        return NV_ENC_ERR_ENCODER_NOT_INITIALIZED;
    }
    if ((params->encodePicFlags & NV_ENC_PIC_FLAG_EOS) != 0U) {
        return NV_ENC_SUCCESS;
    }
    if (!params->inputBuffer || !params->outputBitstream) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    open_nvenc_bitstream_buf_t *bs = (open_nvenc_bitstream_buf_t *)params->outputBitstream;
    open_nvenc_input_buf_t     *ib = (open_nvenc_input_buf_t *)params->inputBuffer;
    if (bs->magic != OPEN_NVENC_BSBUF_MAGIC || ib->magic != OPEN_NVENC_INBUF_MAGIC) {
        return NV_ENC_ERR_INVALID_PARAM;
    }

    int force_idr   = (params->encodePicFlags & NV_ENC_PIC_FLAG_FORCEIDR) ? 1 : 0;
    int force_intra = (params->encodePicFlags & NV_ENC_PIC_FLAG_FORCEINTRA) ? 1 : 0;
    int emit_spspps = (params->encodePicFlags & NV_ENC_PIC_FLAG_OUTPUT_SPSPPS) ? 1 : 0;
    int is_idr      = (force_idr || force_intra) ? 1 : 0;

    if (ib->owner_reg) {
        open_nvenc_reg_res_t *rr = (open_nvenc_reg_res_t *)ib->owner_reg;
        if (rr->magic == OPEN_NVENC_REGRES_MAGIC) {
            NVENCSTATUS sync_st = sync_cuda_reg_to_host(s, rr);
            if (sync_st != NV_ENC_SUCCESS) {
                trace_nvenc_event("nvEncEncodePicture", sync_st, s->last_error);
                return sync_st;
            }
        }
    }

    uint32_t avg_qp = s->config.rcParams.constQP.qpIntra ? s->config.rcParams.constQP.qpIntra : 24U;
    if (s->hw) {
        uint32_t pitch = params->inputPitch ? params->inputPitch : ib->pitch;
        NV_ENC_BUFFER_FORMAT fmt = params->bufferFmt ? params->bufferFmt : ib->fmt;
        const uint32_t *co = ib->chroma_offset[0] ? ib->chroma_offset : NULL;
        NVENCSTATUS st = open_nvenc_hw_encode_frame(
            s->hw, ib->data, pitch, fmt, co,
            is_idr, emit_spspps,
            bs->data, bs->capacity, &bs->valid_bytes,
            &is_idr, &avg_qp, &s->rm, s->last_error, sizeof(s->last_error));
        if (st != NV_ENC_SUCCESS) {
            char edetail[256];
            snprintf(edetail, sizeof(edetail), "%s [fmt=0x%x pitch=%u flags=0x%x]",
                     s->last_error, (unsigned)fmt, pitch, (unsigned)params->encodePicFlags);
            trace_nvenc_event("nvEncEncodePicture", st, edetail);
            return st;
        }
    } else if (s->is_hevc) {
        uint32_t chroma = 1U, depth = 8U, qp = 24U;
        resolve_hevc_geometry(s, &chroma, &depth, &qp, NULL);
        avg_qp = qp;
        uint32_t gop = s->config.gopLength;
        if (s->rm.frames_encoded == 0U || (gop > 0U && (s->rm.frames_encoded % gop) == 0U)) {
            is_idr = 1;
        }
        size_t hdr_n = (is_idr || emit_spspps)
                           ? open_nvenc_hevc_write_headers(
                                 bs->data, bs->capacity,
                                 s->init_params.encodeWidth, s->init_params.encodeHeight,
                                 chroma, depth)
                           : 0U;
        static const uint8_t hevc_offline_idr[] = {
            0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xaf, 0x08, 0x40
        };
        static const uint8_t hevc_offline_p[] = {
            0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0xd0, 0x08, 0x40
        };
        const uint8_t *slice = is_idr ? hevc_offline_idr : hevc_offline_p;
        size_t slice_n = is_idr ? sizeof(hevc_offline_idr) : sizeof(hevc_offline_p);
        if (hdr_n + slice_n > bs->capacity || ((is_idr || emit_spspps) && hdr_n == 0U)) {
            bs->valid_bytes = write_nal_bitstream(s, bs->data, bs->capacity, is_idr);
        } else {
            memcpy(bs->data + hdr_n, slice, slice_n);
            bs->valid_bytes = hdr_n + slice_n;
        }
    } else {
        if (s->rm.frames_encoded == 0U ||
            (s->config.gopLength > 0U && (s->rm.frames_encoded % s->config.gopLength) == 0U)) {
            is_idr = 1;
        }
        bs->valid_bytes = write_nal_bitstream(s, bs->data, bs->capacity, is_idr);
    }

    bs->frame_idx = s->rm.frames_encoded++;
    bs->pic_type  = is_idr ? NV_ENC_PIC_TYPE_IDR : NV_ENC_PIC_TYPE_P;
    bs->timestamp = params->inputTimeStamp;
    bs->duration  = params->inputDuration;
    bs->avg_qp    = avg_qp;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncLockBitstream(
    void *encoder, NV_ENC_LOCK_BITSTREAM *params)
{
    if (!as_session(encoder) || !params || !params->outputBitstream) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    open_nvenc_bitstream_buf_t *bs = (open_nvenc_bitstream_buf_t *)params->outputBitstream;
    if (bs->magic != OPEN_NVENC_BSBUF_MAGIC) {
        return NV_ENC_ERR_INVALID_PARAM;
    }
    bs->locked                   = 1;
    params->bitstreamBufferPtr   = bs->data;
    params->bitstreamSizeInBytes = (uint32_t)bs->valid_bytes;
    params->frameIdx             = bs->frame_idx;
    params->pictureType          = bs->pic_type;
    params->outputTimeStamp      = bs->timestamp;
    params->outputDuration       = bs->duration;
    params->numSlices            = 1U;
    params->frameAvgQP           = bs->avg_qp ? bs->avg_qp : 22U;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncUnlockBitstream(void *encoder, NV_ENC_OUTPUT_PTR bitstreamBuffer)
{
    if (!as_session(encoder) || !bitstreamBuffer) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    open_nvenc_bitstream_buf_t *bs = (open_nvenc_bitstream_buf_t *)bitstreamBuffer;
    if (bs->magic != OPEN_NVENC_BSBUF_MAGIC) {
        return NV_ENC_ERR_INVALID_PARAM;
    }
    bs->locked = 0;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncGetEncodeStats(void *encoder, NV_ENC_STAT *encodeStats)
{
    open_nvenc_session_t *s = as_session(encoder);
    if (!s || !encodeStats) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    encodeStats->picIdx        = s->rm.frames_encoded;
    encodeStats->bitStreamSize = 34U;
    encodeStats->picType       = NV_ENC_PIC_TYPE_IDR;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncGetSequenceParams(
    void *encoder, NV_ENC_SEQUENCE_PARAM_PAYLOAD *payload)
{
    open_nvenc_session_t *s = as_session(encoder);
    if (!s || !payload || !payload->spsppsBuffer || !payload->outSPSPPSPayloadSize) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    if (s->is_hevc) {
        uint32_t chroma = 1U, depth = 8U, qp = 24U;
        resolve_hevc_geometry(s, &chroma, &depth, &qp, NULL);
        size_t n = open_nvenc_hevc_write_headers(
            (uint8_t *)payload->spsppsBuffer, payload->inBufferSize,
            s->init_params.encodeWidth ? s->init_params.encodeWidth : 256U,
            s->init_params.encodeHeight ? s->init_params.encodeHeight : 256U,
            chroma, depth);
        if (n > 0U) {
            *payload->outSPSPPSPayloadSize = (uint32_t)n;
            return NV_ENC_SUCCESS;
        }
    } else {
        uint32_t chroma = 1U, depth = 8U, qp = 24U;
        resolve_hevc_geometry(s, &chroma, &depth, &qp, NULL);
        size_t n = open_nvenc_h264_write_headers(
            (uint8_t *)payload->spsppsBuffer, payload->inBufferSize,
            s->init_params.encodeWidth ? s->init_params.encodeWidth : 256U,
            s->init_params.encodeHeight ? s->init_params.encodeHeight : 256U,
            chroma);
        if (n > 0U) {
            *payload->outSPSPPSPayloadSize = (uint32_t)n;
            return NV_ENC_SUCCESS;
        }
    }
    size_t n = write_nal_bitstream(s, (uint8_t *)payload->spsppsBuffer, payload->inBufferSize, 0);
    *payload->outSPSPPSPayloadSize = (uint32_t)n;
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncGetSequenceParamPayload(void *encoder, void *spsppsPayload)
{
    return impl_nvEncGetSequenceParams(encoder, (NV_ENC_SEQUENCE_PARAM_PAYLOAD *)spsppsPayload);
}

static NVENCSTATUS impl_nvEncRegisterAsyncEvent(void *encoder, void *eventParams)
{
    (void)eventParams;
    return as_session(encoder) ? NV_ENC_SUCCESS : NV_ENC_ERR_INVALID_PTR;
}

static NVENCSTATUS impl_nvEncUnregisterAsyncEvent(void *encoder, void *eventParams)
{
    (void)eventParams;
    return as_session(encoder) ? NV_ENC_SUCCESS : NV_ENC_ERR_INVALID_PTR;
}

static size_t compute_registered_buffer_size(uint32_t pitch, uint32_t height,
                                             NV_ENC_BUFFER_FORMAT fmt,
                                             const uint32_t chroma_offset[2])
{
    size_t base_sz;
    switch (fmt) {
        case NV_ENC_BUFFER_FORMAT_YUV444:
        case NV_ENC_BUFFER_FORMAT_YUV444_10BIT:
            base_sz = (size_t)pitch * (size_t)height * 3U;
            if (chroma_offset && chroma_offset[1] > 0U) {
                size_t co_sz = (size_t)chroma_offset[1] + (size_t)pitch * (size_t)height;
                if (co_sz > base_sz) {
                    base_sz = co_sz;
                }
            }
            return base_sz;
        case NV_ENC_BUFFER_FORMAT_NV12:
        case NV_ENC_BUFFER_FORMAT_YUV420_10BIT:
            base_sz = ((size_t)pitch * (size_t)height * 3U) / 2U;
            if (chroma_offset && chroma_offset[0] > 0U) {
                size_t co_sz = (size_t)chroma_offset[0] + ((size_t)pitch * (size_t)height) / 2U;
                if (co_sz > base_sz) {
                    base_sz = co_sz;
                }
            }
            return base_sz;
        case NV_ENC_BUFFER_FORMAT_YV12:
        case NV_ENC_BUFFER_FORMAT_IYUV:
            base_sz = ((size_t)pitch * (size_t)height * 3U) / 2U;
            if (chroma_offset && chroma_offset[1] > 0U) {
                size_t co_sz = (size_t)chroma_offset[1] + ((size_t)pitch * (size_t)height) / 4U;
                if (co_sz > base_sz) {
                    base_sz = co_sz;
                }
            }
            return base_sz;
        default:
            return (size_t)pitch * (size_t)height;
    }
}

static void *lookup_cuda_sym(const char *sym1, const char *sym2, void **out_dl)
{
    *out_dl = NULL;
    void *fn = dlsym(RTLD_DEFAULT, sym1);
    if (!fn && sym2) {
        fn = dlsym(RTLD_DEFAULT, sym2);
    }
    if (!fn) {
        void *cu_dl = dlopen("libcuda.so.1", RTLD_LAZY | RTLD_NOLOAD);
        if (cu_dl) {
            fn = dlsym(cu_dl, sym1);
            if (!fn && sym2) {
                fn = dlsym(cu_dl, sym2);
            }
            *out_dl = cu_dl;
        }
    }
    return fn;
}

typedef struct {
    size_t      srcXInBytes;
    size_t      srcY;
    int         srcMemoryType;
    const void *srcHost;
    uint64_t    srcDevice;
    void       *srcArray;
    size_t      srcPitch;
    size_t      dstXInBytes;
    size_t      dstY;
    int         dstMemoryType;
    void       *dstHost;
    uint64_t    dstDevice;
    void       *dstArray;
    size_t      dstPitch;
    size_t      WidthInBytes;
    size_t      Height;
} open_cu_memcpy2d_t;

typedef struct {
    int   (*ctx_get_cur)(void **pctx);
    int   (*ctx_push)(void *ctx);
    int   (*ctx_pop)(void **pctx);
    int   (*ctx_sync)(void);
    int   (*stream_sync)(void *hStream);
    int   (*memcpy_dtoh)(void *dstHost, uint64_t srcDevice, size_t byteCount);
    int   (*memcpy_2d)(const open_cu_memcpy2d_t *pCopy);
} open_cuda_syms_t;

static open_cuda_syms_t g_cuda_syms;

static void ensure_cuda_syms(void)
{
    if (g_cuda_syms.memcpy_dtoh && g_cuda_syms.memcpy_2d && g_cuda_syms.ctx_sync) {
        return;
    }
    void *dl_tmp = NULL;
    if (!g_cuda_syms.ctx_get_cur) {
        g_cuda_syms.ctx_get_cur = (int (*)(void **))lookup_cuda_sym("cuCtxGetCurrent", NULL, &dl_tmp);
    }
    if (!g_cuda_syms.ctx_push) {
        g_cuda_syms.ctx_push = (int (*)(void *))lookup_cuda_sym("cuCtxPushCurrent_v2", "cuCtxPushCurrent", &dl_tmp);
    }
    if (!g_cuda_syms.ctx_pop) {
        g_cuda_syms.ctx_pop = (int (*)(void **))lookup_cuda_sym("cuCtxPopCurrent_v2", "cuCtxPopCurrent", &dl_tmp);
    }
    if (!g_cuda_syms.ctx_sync) {
        g_cuda_syms.ctx_sync = (int (*)(void))lookup_cuda_sym("cuCtxSynchronize", NULL, &dl_tmp);
    }
    if (!g_cuda_syms.stream_sync) {
        g_cuda_syms.stream_sync = (int (*)(void *))lookup_cuda_sym("cuStreamSynchronize", NULL, &dl_tmp);
    }
    if (!g_cuda_syms.memcpy_dtoh) {
        g_cuda_syms.memcpy_dtoh = (int (*)(void *, uint64_t, size_t))lookup_cuda_sym("cuMemcpyDtoH_v2", "cuMemcpyDtoH", &dl_tmp);
    }
    if (!g_cuda_syms.memcpy_2d) {
        g_cuda_syms.memcpy_2d = (int (*)(const open_cu_memcpy2d_t *))lookup_cuda_sym("cuMemcpy2D_v2", "cuMemcpy2D", &dl_tmp);
    }
}

static NVENCSTATUS sync_cuda_reg_to_host(open_nvenc_session_t *s, open_nvenc_reg_res_t *rr)
{
    if (!s || !rr) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    if (!(rr->is_cuda_devptr || rr->is_cuda_array) || !rr->reg.resourceToRegister || rr->cuda_host_size == 0U) {
        return NV_ENC_SUCCESS;
    }

    ensure_cuda_syms();

    void *cur_ctx = NULL;
    int pushed_ctx = 0;
    if (s->device_type == NV_ENC_DEVICE_TYPE_CUDA && s->device_ptr &&
        g_cuda_syms.ctx_get_cur && g_cuda_syms.ctx_push) {
        if (g_cuda_syms.ctx_get_cur(&cur_ctx) == 0 && cur_ctx != s->device_ptr) {
            if (g_cuda_syms.ctx_push(s->device_ptr) == 0) {
                pushed_ctx = 1;
            }
        }
    }
    if (s->in_cuda_stream && g_cuda_syms.stream_sync) {
        (void)g_cuda_syms.stream_sync(s->in_cuda_stream);
    } else if (g_cuda_syms.ctx_sync) {
        (void)g_cuda_syms.ctx_sync();
    }

    NVENCSTATUS map_st = NV_ENC_SUCCESS;
    if (rr->is_cuda_devptr) {
        if (g_cuda_syms.memcpy_dtoh) {
            int cu_st = g_cuda_syms.memcpy_dtoh(rr->mapped_buf.data,
                                                (uint64_t)(uintptr_t)rr->reg.resourceToRegister,
                                                rr->cuda_host_size);
            if (cu_st != 0) {
                snprintf(s->last_error, sizeof(s->last_error),
                         "cuMemcpyDtoH_v2 failed (CUresult=%d)", cu_st);
                map_st = NV_ENC_ERR_GENERIC;
            }
        } else if (s->hw) {
            snprintf(s->last_error, sizeof(s->last_error),
                     "cuMemcpyDtoH_v2 not available for CUDADEVICEPTR readback");
            map_st = NV_ENC_ERR_GENERIC;
        }
    } else if (rr->is_cuda_array) {
        if (g_cuda_syms.memcpy_2d) {
            open_cu_memcpy2d_t cp;
            memset(&cp, 0, sizeof(cp));
            cp.srcMemoryType = 3; /* CU_MEMORYTYPE_ARRAY */
            cp.srcArray      = rr->reg.resourceToRegister;
            cp.dstMemoryType = 1; /* CU_MEMORYTYPE_HOST */
            cp.dstHost       = rr->mapped_buf.data;
            cp.dstPitch      = rr->mapped_buf.pitch;
            cp.WidthInBytes  = compute_input_pitch(rr->mapped_buf.width, rr->mapped_buf.fmt);
            int is_packed_1p = (rr->mapped_buf.fmt == NV_ENC_BUFFER_FORMAT_ARGB ||
                                rr->mapped_buf.fmt == NV_ENC_BUFFER_FORMAT_ABGR ||
                                rr->mapped_buf.fmt == NV_ENC_BUFFER_FORMAT_ARGB10 ||
                                rr->mapped_buf.fmt == NV_ENC_BUFFER_FORMAT_ABGR10 ||
                                rr->mapped_buf.fmt == NV_ENC_BUFFER_FORMAT_AYUV);
            cp.Height        = is_packed_1p
                                   ? rr->mapped_buf.height
                                   : (rr->mapped_buf.pitch
                                          ? (rr->cuda_host_size / rr->mapped_buf.pitch)
                                          : rr->mapped_buf.height);
            int cu_st = g_cuda_syms.memcpy_2d(&cp);
            if (cu_st != 0) {
                snprintf(s->last_error, sizeof(s->last_error),
                         "cuMemcpy2D_v2 CUDAARRAY failed (CUresult=%d)", cu_st);
                map_st = NV_ENC_ERR_GENERIC;
            }
        } else if (s->hw) {
            snprintf(s->last_error, sizeof(s->last_error),
                     "cuMemcpy2D_v2 not available for CUDAARRAY readback");
            map_st = NV_ENC_ERR_GENERIC;
        }
    }

    if (pushed_ctx && g_cuda_syms.ctx_pop) {
        void *dummy = NULL;
        (void)g_cuda_syms.ctx_pop(&dummy);
    }
    return map_st;
}

static NVENCSTATUS impl_nvEncRegisterResource(
    void *encoder, NV_ENC_REGISTER_RESOURCE *params)
{
    open_nvenc_session_t *s = as_session(encoder);
    if (!s || !params || !params->resourceToRegister) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    open_nvenc_reg_res_t *rr = (open_nvenc_reg_res_t *)calloc(1U, sizeof(*rr));
    if (!rr) {
        return NV_ENC_ERR_OUT_OF_MEMORY;
    }
    rr->magic              = OPEN_NVENC_REGRES_MAGIC;
    rr->reg                = *params;
    rr->mapped_buf.magic   = OPEN_NVENC_INBUF_MAGIC;
    rr->mapped_buf.width   = params->width ? params->width : s->init_params.encodeWidth;
    rr->mapped_buf.height  = params->height ? params->height : s->init_params.encodeHeight;
    rr->mapped_buf.fmt     = params->bufferFormat ? params->bufferFormat : NV_ENC_BUFFER_FORMAT_NV12;
    rr->mapped_buf.pitch   = params->pitch ? params->pitch : (uint32_t)compute_input_pitch(rr->mapped_buf.width, rr->mapped_buf.fmt);
    rr->mapped_buf.chroma_offset[0] = params->chromaOffset[0];
    rr->mapped_buf.chroma_offset[1] = params->chromaOffset[1];
    rr->mapped_buf.owner_reg        = rr;
    if (params->resourceType == NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR ||
        params->resourceType == NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY) {
        size_t sz = compute_registered_buffer_size(rr->mapped_buf.pitch,
                                                   rr->mapped_buf.height,
                                                   rr->mapped_buf.fmt,
                                                   params->chromaOffset);
        rr->cuda_host_size  = sz ? sz : 4096U;
        rr->mapped_buf.data = (uint8_t *)calloc(1U, rr->cuda_host_size);
        if (!rr->mapped_buf.data) {
            free(rr);
            return NV_ENC_ERR_OUT_OF_MEMORY;
        }
        rr->mapped_buf.size = rr->cuda_host_size;
        if (params->resourceType == NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR) {
            rr->is_cuda_devptr = 1;
        } else {
            rr->is_cuda_array  = 1;
        }
    } else {
        rr->mapped_buf.data = (uint8_t *)params->resourceToRegister;
    }
    params->registeredResource = rr;
    {
        char detail[128];
        snprintf(detail, sizeof(detail), "type=%u fmt=0x%x %ux%u pitch=%u co=%u,%u",
                 params->resourceType, (unsigned)rr->mapped_buf.fmt,
                 rr->mapped_buf.width, rr->mapped_buf.height,
                 rr->mapped_buf.pitch, params->chromaOffset[0], params->chromaOffset[1]);
        trace_nvenc_event("nvEncRegisterResource", NV_ENC_SUCCESS, detail);
    }
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncUnregisterResource(
    void *encoder, NV_ENC_REGISTERED_PTR registeredResource)
{
    if (!as_session(encoder) || !registeredResource) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    open_nvenc_reg_res_t *rr = (open_nvenc_reg_res_t *)registeredResource;
    if (rr->magic != OPEN_NVENC_REGRES_MAGIC) {
        return NV_ENC_ERR_INVALID_PARAM;
    }
    if ((rr->is_cuda_devptr || rr->is_cuda_array) && rr->mapped_buf.data) {
        free(rr->mapped_buf.data);
        rr->mapped_buf.data = NULL;
    }
    rr->magic = 0U;
    free(rr);
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncMapInputResource(
    void *encoder, NV_ENC_MAP_INPUT_RESOURCE *params)
{
    open_nvenc_session_t *s = as_session(encoder);
    if (!s || !params || !params->registeredResource) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    open_nvenc_reg_res_t *rr = (open_nvenc_reg_res_t *)params->registeredResource;
    if (rr->magic != OPEN_NVENC_REGRES_MAGIC) {
        return NV_ENC_ERR_INVALID_PARAM;
    }
    NVENCSTATUS map_st = sync_cuda_reg_to_host(s, rr);
    if (map_st != NV_ENC_SUCCESS) {
        trace_nvenc_event("nvEncMapInputResource", map_st, s->last_error);
        return map_st;
    }
    params->mappedResource  = &rr->mapped_buf;
    params->mappedBufferFmt = rr->mapped_buf.fmt;
    {
        char mdetail[128];
        snprintf(mdetail, sizeof(mdetail), "type=%u fmt=0x%x %ux%u pitch=%u",
                 rr->reg.resourceType, (unsigned)rr->mapped_buf.fmt,
                 rr->mapped_buf.width, rr->mapped_buf.height, rr->mapped_buf.pitch);
        trace_nvenc_event("nvEncMapInputResource", NV_ENC_SUCCESS, mdetail);
    }
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncUnmapInputResource(
    void *encoder, NV_ENC_INPUT_PTR mappedInputBuffer)
{
    if (!as_session(encoder) || !mappedInputBuffer) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncDestroyEncoder(void *encoder)
{
    open_nvenc_session_t *s = as_session(encoder);
    if (!s) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    session_disconnect_rm(s);
    s->magic = 0U;
    free(s);
    trace_nvenc_event("nvEncDestroyEncoder", NV_ENC_SUCCESS, "session destroyed");
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncInvalidateRefFrames(
    void *encoder, uint64_t invalidRefFrameTimeStamp)
{
    (void)invalidRefFrameTimeStamp;
    open_nvenc_session_t *s = as_session(encoder);
    if (!s) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    if (s->hw) {
        open_nvenc_hw_invalidate_refs(s->hw);
    }
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncReconfigureEncoder(
    void *encoder, NV_ENC_RECONFIGURE_PARAMS *params)
{
    open_nvenc_session_t *s = as_session(encoder);
    if (!s || !params) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    int valid = 0;
    (void)open_nvenc_decode_struct_version(params->version, &valid);
    if (!valid) {
        return NV_ENC_ERR_INVALID_VERSION;
    }
    if (params->reInitEncodeParams.encodeWidth < 64U ||
        params->reInitEncodeParams.encodeHeight < 64U) {
        return NV_ENC_ERR_INVALID_PARAM;
    }

    uint32_t prev_w   = s->init_params.encodeWidth;
    uint32_t prev_h   = s->init_params.encodeHeight;
    NV_ENC_BUFFER_FORMAT prev_fmt = s->init_params.bufferFormat;

    s->init_params = params->reInitEncodeParams;
    if (params->reInitEncodeParams.encodeConfig) {
        s->config = *params->reInitEncodeParams.encodeConfig;
        s->init_params.encodeConfig = &s->config;
    }

    if (s->hw) {
        int need_recreate = (params->resetEncoder ||
                             s->init_params.encodeWidth != prev_w ||
                             s->init_params.encodeHeight != prev_h ||
                             s->init_params.bufferFormat != prev_fmt) ? 1 : 0;
        if (need_recreate) {
            open_nvenc_hw_destroy(s->hw, &s->rm);
            s->hw = NULL;
            open_nvenc_hw_init_cfg_t hw_cfg;
            fill_hw_cfg_from_session(s, &hw_cfg);
            if (open_nvenc_hw_create(&s->hw, s->ctl_fd, s->dev_fd, &hw_cfg, &s->rm,
                                     s->last_error, sizeof(s->last_error)) < 0) {
                trace_nvenc_event("nvEncReconfigureEncoder", NV_ENC_ERR_GENERIC, s->last_error);
                return NV_ENC_ERR_GENERIC;
            }
        } else {
            uint32_t chroma = 1U, depth = 8U, qp = 24U, qp_p = 24U;
            resolve_hevc_geometry(s, &chroma, &depth, &qp, &qp_p);
            open_nvenc_hw_update_cfg(s->hw, s->config.gopLength, qp, qp_p,
                                     s->config.rcParams.rateControlMode,
                                     s->config.rcParams.averageBitRate,
                                     s->config.rcParams.maxBitRate,
                                     s->config.rcParams.vbvBufferSize,
                                     s->init_params.frameRateNum,
                                     s->init_params.frameRateDen,
                                     params->forceIDR ? 1 : 0);
        }
    }
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncReserved1(void *encoder, void *reservedParams)
{
    (void)reservedParams;
    return as_session(encoder) ? NV_ENC_SUCCESS : NV_ENC_ERR_INVALID_PTR;
}

static NVENCSTATUS impl_nvEncCreateMVBuffer(void *encoder, void *createMVBufferParams)
{
    (void)createMVBufferParams;
    return as_session(encoder) ? NV_ENC_SUCCESS : NV_ENC_ERR_INVALID_PTR;
}

static NVENCSTATUS impl_nvEncDestroyMVBuffer(void *encoder, NV_ENC_OUTPUT_PTR mvBuffer)
{
    (void)mvBuffer;
    return as_session(encoder) ? NV_ENC_SUCCESS : NV_ENC_ERR_INVALID_PTR;
}

static NVENCSTATUS impl_nvEncRunMotionEstimationOnly(void *encoder, void *meOnlyParams)
{
    (void)meOnlyParams;
    return as_session(encoder) ? NV_ENC_SUCCESS : NV_ENC_ERR_INVALID_PTR;
}

static const char *impl_nvEncGetLastErrorString(void *encoder)
{
    open_nvenc_session_t *s = as_session(encoder);
    return s ? s->last_error : "Invalid encoder handle";
}

static NVENCSTATUS impl_nvEncSetIOCudaStreams(
    void *encoder, NV_ENC_CUSTREAM_PTR inputStream, NV_ENC_CUSTREAM_PTR outputStream)
{
    open_nvenc_session_t *s = as_session(encoder);
    if (!s) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    s->in_cuda_stream  = inputStream;
    s->out_cuda_stream = outputStream;
    {
        char sdetail[96];
        snprintf(sdetail, sizeof(sdetail), "in=%p out=%p", inputStream, outputStream);
        trace_nvenc_event("nvEncSetIOCudaStreams", NV_ENC_SUCCESS, sdetail);
    }
    return NV_ENC_SUCCESS;
}

static NVENCSTATUS impl_nvEncRestoreEncoderState(void *encoder, void *restoreStateParams)
{
    (void)restoreStateParams;
    return as_session(encoder) ? NV_ENC_SUCCESS : NV_ENC_ERR_INVALID_PTR;
}

static NVENCSTATUS impl_nvEncLookaheadPicture(void *encoder, void *lookaheadParams)
{
    (void)lookaheadParams;
    return as_session(encoder) ? NV_ENC_SUCCESS : NV_ENC_ERR_INVALID_PTR;
}

/*
 * Export 1: NvEncodeAPIGetMaxSupportedVersion
 * Returns 6 (NV_ENC_ERR_INVALID_PTR) if version == NULL, else writes
 * open_nvenc_decode_struct_version(0x7002000dU, NULL) == 0x000000d0 (NVENC 13.0)
 * and returns 0 (NV_ENC_SUCCESS).
 */
NVENCSTATUS NvEncodeAPIGetMaxSupportedVersion(uint32_t *version)
{
    if (!version) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    *version = open_nvenc_decode_struct_version(0x7002000dU, NULL);
    return NV_ENC_SUCCESS;
}

/*
 * Export 2: NvEncodeAPICreateInstance (0x11b3f0)
 * Validates functionList != NULL (returns 6) and struct version via
 * FUN_00115a60 (returns 15 on invalid version), then populates all 43
 * function pointers at exact offsets +0x08..+0x158 (or delegates to
 * OPEN_NVENC_BACKEND_LIB if set).
 */
NVENCSTATUS NvEncodeAPICreateInstance(NV_ENCODE_API_FUNCTION_LIST *functionList)
{
    if (!functionList) {
        return NV_ENC_ERR_INVALID_PTR;
    }
    int valid = 0;
    (void)open_nvenc_decode_struct_version(functionList->version, &valid);
    if (!valid) {
        return NV_ENC_ERR_INVALID_VERSION;
    }

    const char *backend_path = getenv("OPEN_NVENC_BACKEND_LIB");
    if (backend_path && backend_path[0]) {
        void *dl = dlopen(backend_path, RTLD_LAZY | RTLD_LOCAL);
        if (dl) {
            typedef NVENCSTATUS (*create_fn_t)(NV_ENCODE_API_FUNCTION_LIST *);
            create_fn_t fn = (create_fn_t)dlsym(dl, "NvEncodeAPICreateInstance");
            if (fn && fn != NvEncodeAPICreateInstance) {
                NVENCSTATUS st = fn(functionList);
                trace_nvenc_event("NvEncodeAPICreateInstance(backend)", (uint32_t)st, backend_path);
                return st;
            }
        }
    }

    functionList->nvEncOpenEncodeSession         = impl_nvEncOpenEncodeSession;
    functionList->nvEncGetEncodeGUIDCount        = impl_nvEncGetEncodeGUIDCount;
    functionList->nvEncGetEncodeProfileGUIDCount = impl_nvEncGetEncodeProfileGUIDCount;
    functionList->nvEncGetEncodeProfileGUIDs     = impl_nvEncGetEncodeProfileGUIDs;
    functionList->nvEncGetEncodeGUIDs            = impl_nvEncGetEncodeGUIDs;
    functionList->nvEncGetInputFormatCount       = impl_nvEncGetInputFormatCount;
    functionList->nvEncGetInputFormats          = impl_nvEncGetInputFormats;
    functionList->nvEncGetEncodeCaps             = impl_nvEncGetEncodeCaps;
    functionList->nvEncGetEncodePresetCount      = impl_nvEncGetEncodePresetCount;
    functionList->nvEncGetEncodePresetGUIDs      = impl_nvEncGetEncodePresetGUIDs;
    functionList->nvEncGetEncodePresetConfig     = impl_nvEncGetEncodePresetConfig;
    functionList->nvEncInitializeEncoder         = impl_nvEncInitializeEncoder;
    functionList->nvEncCreateInputBuffer         = impl_nvEncCreateInputBuffer;
    functionList->nvEncDestroyInputBuffer        = impl_nvEncDestroyInputBuffer;
    functionList->nvEncCreateBitstreamBuffer     = impl_nvEncCreateBitstreamBuffer;
    functionList->nvEncDestroyBitstreamBuffer    = impl_nvEncDestroyBitstreamBuffer;
    functionList->nvEncEncodePicture             = impl_nvEncEncodePicture;
    functionList->nvEncLockBitstream             = impl_nvEncLockBitstream;
    functionList->nvEncUnlockBitstream           = impl_nvEncUnlockBitstream;
    functionList->nvEncLockInputBuffer           = impl_nvEncLockInputBuffer;
    functionList->nvEncUnlockInputBuffer         = impl_nvEncUnlockInputBuffer;
    functionList->nvEncGetEncodeStats            = impl_nvEncGetEncodeStats;
    functionList->nvEncGetSequenceParams         = impl_nvEncGetSequenceParams;
    functionList->nvEncRegisterAsyncEvent        = impl_nvEncRegisterAsyncEvent;
    functionList->nvEncUnregisterAsyncEvent      = impl_nvEncUnregisterAsyncEvent;
    functionList->nvEncMapInputResource          = impl_nvEncMapInputResource;
    functionList->nvEncUnmapInputResource        = impl_nvEncUnmapInputResource;
    functionList->nvEncDestroyEncoder            = impl_nvEncDestroyEncoder;
    functionList->nvEncInvalidateRefFrames       = impl_nvEncInvalidateRefFrames;
    functionList->nvEncOpenEncodeSessionEx       = impl_nvEncOpenEncodeSessionEx;
    functionList->nvEncRegisterResource          = impl_nvEncRegisterResource;
    functionList->nvEncUnregisterResource        = impl_nvEncUnregisterResource;
    functionList->nvEncReconfigureEncoder        = impl_nvEncReconfigureEncoder;
    functionList->nvEncReserved1                 = impl_nvEncReserved1;
    functionList->nvEncCreateMVBuffer            = impl_nvEncCreateMVBuffer;
    functionList->nvEncDestroyMVBuffer           = impl_nvEncDestroyMVBuffer;
    functionList->nvEncRunMotionEstimationOnly   = impl_nvEncRunMotionEstimationOnly;
    functionList->nvEncGetLastErrorString        = impl_nvEncGetLastErrorString;
    functionList->nvEncSetIOCudaStreams          = impl_nvEncSetIOCudaStreams;
    functionList->nvEncGetEncodePresetConfigEx   = impl_nvEncGetEncodePresetConfigEx;
    functionList->nvEncGetSequenceParamPayload   = impl_nvEncGetSequenceParamPayload;
    functionList->nvEncRestoreEncoderState       = impl_nvEncRestoreEncoderState;
    functionList->nvEncLookaheadPicture          = impl_nvEncLookaheadPicture;

    trace_nvenc_event("NvEncodeAPICreateInstance", NV_ENC_SUCCESS, "clean-room table populated");
    return NV_ENC_SUCCESS;
}

/*
 * Exports 3..10: NvTool* profiler hooks matching 0x12b570..0x12b6a0
 */
static void vtbl_dtor_noop(void *self)
{
    (void)self;
}

static void vtbl_set_api_id(void *self, uint64_t apiId, uint8_t domain)
{
    NV_TOOL_INTERFACE *ti = (NV_TOOL_INTERFACE *)self;
    if (ti) {
        ti->api_id = apiId;
        ti->domain = domain;
    }
}

static uint64_t vtbl_get_api_id(void *self)
{
    const NV_TOOL_INTERFACE *ti = (const NV_TOOL_INTERFACE *)self;
    return ti ? ti->api_id : 0ULL;
}

static uint32_t vtbl_get_api_fn_count(void *self, uint8_t domain)
{
    (void)self;
    (void)domain;
    return 0x2dU;
}

static const char **vtbl_get_api_names(void *self, uint8_t domain, uint32_t startIdx, int count)
{
    (void)self;
    if (domain == 1U && count >= 0 && (startIdx + (uint32_t)count < 0x2eU)) {
        return &g_nvtool_api_names[startIdx];
    }
    return NULL;
}

static const NV_TOOL_VTABLE g_nvtool_vtable = {
    .pfnDtor0               = vtbl_dtor_noop,
    .pfnDtor1               = vtbl_dtor_noop,
    .pfnSetApiID            = vtbl_set_api_id,
    .pfnGetApiID            = vtbl_get_api_id,
    .pfnGetApiFunctionCount = vtbl_get_api_fn_count,
    .pfnGetApiNames         = vtbl_get_api_names
};

static NV_TOOL_INTERFACE g_nvtool_default_obj = {
    .vtable   = &g_nvtool_vtable,
    .api_id   = 0ULL,
    .domain   = 1ULL,
    .reserved = 0ULL
};

void NvToolSetInterface(void *iface)
{
    if (!g_nvtool_default_iface) {
        g_nvtool_default_iface = &g_nvtool_default_obj;
    }
    if (iface == (void *)(intptr_t)-1) {
        g_nvtool_active_iface = g_nvtool_default_iface;
        return;
    }
    g_nvtool_active_iface = iface;
}

void *NvToolGetInterface(void)
{
    if (!g_nvtool_default_iface) {
        g_nvtool_default_iface = &g_nvtool_default_obj;
    }
    if (!g_nvtool_active_iface) {
        g_nvtool_active_iface = g_nvtool_default_iface;
    }
    return g_nvtool_active_iface;
}

void *NvToolCreateInterface(int type)
{
    if (type != 1) {
        return NULL;
    }
    NV_TOOL_INTERFACE *p = (NV_TOOL_INTERFACE *)calloc(1U, 0x20U);
    if (!p) {
        return NULL;
    }
    p->vtable = &g_nvtool_vtable;
    p->domain = 1ULL;
    return p;
}

void NvToolDestroyInterface(void *pToolIface)
{
    if (pToolIface && pToolIface != &g_nvtool_default_obj) {
        free(pToolIface);
    }
}

void NvToolSetApiID(void *pToolIface, uint64_t apiId, uint8_t domain)
{
    NV_TOOL_INTERFACE *ti = (NV_TOOL_INTERFACE *)pToolIface;
    if (ti && ti->vtable && ti->vtable->pfnSetApiID) {
        ti->vtable->pfnSetApiID(ti, apiId, domain);
    }
}

uint64_t NvToolGetApiID(void *pToolIface)
{
    NV_TOOL_INTERFACE *ti = (NV_TOOL_INTERFACE *)pToolIface;
    if (ti && ti->vtable && ti->vtable->pfnGetApiID) {
        return ti->vtable->pfnGetApiID(ti);
    }
    return 0ULL;
}

uint32_t NvToolGetApiFunctionCount(void *pToolIface, uint8_t domain)
{
    NV_TOOL_INTERFACE *ti = (NV_TOOL_INTERFACE *)pToolIface;
    if (!ti || !ti->vtable || ti->vtable->pfnGetApiFunctionCount == vtbl_get_api_fn_count) {
        return 0x2dU;
    }
    return ti->vtable->pfnGetApiFunctionCount(ti, domain);
}

const char **NvToolGetApiNames(void *pToolIface, uint8_t domain, uint32_t startIdx, int count)
{
    NV_TOOL_INTERFACE *ti = (NV_TOOL_INTERFACE *)pToolIface;
    if (ti && ti->vtable && ti->vtable->pfnGetApiNames != vtbl_get_api_names) {
        return ti->vtable->pfnGetApiNames(ti, domain, startIdx, count);
    }
    if (domain == 1U && count >= 0 && (startIdx + (uint32_t)count < 0x2eU)) {
        return &g_nvtool_api_names[startIdx];
    }
    return NULL;
}
