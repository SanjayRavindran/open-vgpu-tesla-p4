/* SPDX-License-Identifier: MIT */
/*
 * open_nvenc.h - Clean-room Open-Source NVENC 13.0 Userspace API & GP104 RM Engine
 *
 * Drop-in ABI-compatible replacement & standalone hardware encoder for
 * libnvidia-encode.so (SONAME libnvidia-encode.so.1, NVENC API 13.0 / 0xd0)
 * on NVIDIA Tesla P4 (Pascal GP104 / GRID P4-4Q vGPU, NVC2B7_VIDEO_ENCODER 0xc2b7).
 *
 * Exports all 10 ELF dynamic symbols verified via rea@6.10.1:
 *   - NvEncodeAPICreateInstance
 *   - NvEncodeAPIGetMaxSupportedVersion
 *   - NvToolCreateInterface / NvToolDestroyInterface
 *   - NvToolGetInterface / NvToolSetInterface
 *   - NvToolGetApiID / NvToolSetApiID
 *   - NvToolGetApiFunctionCount / NvToolGetApiNames
 */

#ifndef OPEN_NVENC_H
#define OPEN_NVENC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NVENC 13.0 ABI constants (NvEncodeAPIGetMaxSupportedVersion -> 0x000000d0) */
#define NVENCAPI_MAJOR_VERSION 13U
#define NVENCAPI_MINOR_VERSION 0U
#define NVENCAPI_VERSION       (NVENCAPI_MAJOR_VERSION | (NVENCAPI_MINOR_VERSION << 24))

#define NVENCAPI_STRUCT_VERSION(ver) \
    ((uint32_t)NVENCAPI_VERSION | (((uint32_t)(ver) & 0xffffU) << 16) | (0x7U << 28))

#define NV_ENCODE_API_FUNCTION_LIST_VER           NVENCAPI_STRUCT_VERSION(2U)
#define NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER  NVENCAPI_STRUCT_VERSION(1U)
#define NV_ENC_CAPS_PARAM_VER                     NVENCAPI_STRUCT_VERSION(1U)
#define NV_ENC_CONFIG_VER                         (NVENCAPI_STRUCT_VERSION(9U) | (1U << 31))
#define NV_ENC_PRESET_CONFIG_VER                  (NVENCAPI_STRUCT_VERSION(5U) | (1U << 31))
#define NV_ENC_INITIALIZE_PARAMS_VER              (NVENCAPI_STRUCT_VERSION(7U) | (1U << 31))
#define NV_ENC_CREATE_INPUT_BUFFER_VER            NVENCAPI_STRUCT_VERSION(1U)
#define NV_ENC_LOCK_INPUT_BUFFER_VER              NVENCAPI_STRUCT_VERSION(1U)
#define NV_ENC_CREATE_BITSTREAM_BUFFER_VER        NVENCAPI_STRUCT_VERSION(1U)
#define NV_ENC_LOCK_BITSTREAM_VER                 (NVENCAPI_STRUCT_VERSION(2U) | (1U << 31))
#define NV_ENC_PIC_PARAMS_VER                     (NVENCAPI_STRUCT_VERSION(7U) | (1U << 31))
#define NV_ENC_REGISTER_RESOURCE_VER              NVENCAPI_STRUCT_VERSION(5U)
#define NV_ENC_MAP_INPUT_RESOURCE_VER             NVENCAPI_STRUCT_VERSION(4U)
#define NV_ENC_RECONFIGURE_PARAMS_VER             (NVENCAPI_STRUCT_VERSION(1U) | (1U << 31))
#define NV_ENC_STAT_VER                           NVENCAPI_STRUCT_VERSION(1U)
#define NV_ENC_SEQUENCE_PARAM_PAYLOAD_VER         NVENCAPI_STRUCT_VERSION(1U)
#define NV_ENC_CREATE_MV_BUFFER_VER               NVENCAPI_STRUCT_VERSION(1U)
#define NV_ENC_MEONLY_PARAMS_VER                  NVENCAPI_STRUCT_VERSION(3U)
#define NV_ENC_RESTORE_ENCODER_STATE_PARAMS_VER   NVENCAPI_STRUCT_VERSION(1U)
#define NV_ENC_LOOKAHEAD_PIC_PARAMS_VER           NVENCAPI_STRUCT_VERSION(1U)

/* Pascal GP104 RM classes & ioctls (verified via Phase 0 hardware trace) */
#define OPEN_NVENC_RM_CLASS_ROOT_CLIENT        0x00000041U /* NV01_ROOT_CLIENT */
#define OPEN_NVENC_RM_CLASS_DEVICE_0           0x00000080U /* NV01_DEVICE_0 */
#define OPEN_NVENC_RM_CLASS_SUBDEVICE_0        0x00002080U /* NV20_SUBDEVICE_0 */
#define OPEN_NVENC_RM_CLASS_CTXDMA             0x00000002U /* NV01_CONTEXT_DMA */
#define OPEN_NVENC_RM_CLASS_SYSMEM             0x0000003eU /* NV01_MEMORY_SYSTEM */
#define OPEN_NVENC_RM_CLASS_VIDMEM             0x00000040U /* NV01_MEMORY_LOCAL_USER */
#define OPEN_NVENC_RM_CLASS_VASPACE            0x00000070U /* FERMI_VASPACE_A */
#define OPEN_NVENC_RM_CLASS_EVENT              0x00000079U /* NV01_EVENT_OS_EVENT */
#define OPEN_NVENC_RM_CLASS_MEM_VIRTUAL        0x000050a0U /* NV50_MEMORY_VIRTUAL */
#define OPEN_NVENC_RM_CLASS_NVENC_SW_SESSION   0x0000a0bcU /* NVENC_SW_SESSION */
#define OPEN_NVENC_RM_CLASS_CHANNEL_GPFIFO     0x0000c06fU /* NVC06F_PASCAL_CHANNEL_GPFIFO_A */
#define OPEN_NVENC_RM_CLASS_COPY_ENGINE        0x0000c0b5U /* PASCAL_DMA_COPY_A */
#define OPEN_NVENC_RM_CLASS_VIDEO_ENCODER      0x0000c2b7U /* NVC2B7_VIDEO_ENCODER (Pascal GP104) */

#define OPEN_NVENC_RM_CTRL_FIFO_DEV_INFO_TABLE 0x20801112U
#define OPEN_NVENC_RM_CTRL_VGPU_ENC_CAPACITY   0xa0820110U

/* Status codes */
typedef enum _NVENCSTATUS {
    NV_ENC_SUCCESS                      = 0,
    NV_ENC_ERR_NO_ENCODE_DEVICE         = 1,
    NV_ENC_ERR_UNSUPPORTED_DEVICE       = 2,
    NV_ENC_ERR_INVALID_ENCODERDEVICE    = 3,
    NV_ENC_ERR_INVALID_DEVICE           = 4,
    NV_ENC_ERR_DEVICE_NOT_EXIST         = 5,
    NV_ENC_ERR_INVALID_PTR              = 6,
    NV_ENC_ERR_INVALID_EVENT            = 7,
    NV_ENC_ERR_INVALID_PARAM            = 8,
    NV_ENC_ERR_INVALID_CALL             = 9,
    NV_ENC_ERR_OUT_OF_MEMORY            = 10,
    NV_ENC_ERR_ENCODER_NOT_INITIALIZED  = 11,
    NV_ENC_ERR_UNSUPPORTED_PARAM        = 12,
    NV_ENC_ERR_LOCK_BUSY                = 13,
    NV_ENC_ERR_NOT_ENOUGH_BUFFER        = 14,
    NV_ENC_ERR_INVALID_VERSION          = 15,
    NV_ENC_ERR_MAP_FAILED               = 16,
    NV_ENC_ERR_NEED_MORE_INPUT          = 17,
    NV_ENC_ERR_ENCODER_BUSY             = 18,
    NV_ENC_ERR_EVENT_NOT_REGISTERD      = 19,
    NV_ENC_ERR_GENERIC                  = 20,
    NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY  = 21,
    NV_ENC_ERR_UNIMPLEMENTED            = 22,
    NV_ENC_ERR_RESOURCE_REGISTER_FAILED = 23,
    NV_ENC_ERR_RESOURCE_NOT_REGISTERED  = 24,
    NV_ENC_ERR_RESOURCE_NOT_MAPPED      = 25,
    NV_ENC_ERR_NEED_MORE_OUTPUT         = 26
} NVENCSTATUS;

typedef enum _NV_ENC_DEVICE_TYPE {
    NV_ENC_DEVICE_TYPE_DIRECTX = 0x0,
    NV_ENC_DEVICE_TYPE_CUDA    = 0x1,
    NV_ENC_DEVICE_TYPE_OPENGL  = 0x2
} NV_ENC_DEVICE_TYPE;

typedef enum _NV_ENC_BUFFER_FORMAT {
    NV_ENC_BUFFER_FORMAT_UNDEFINED    = 0x00000000U,
    NV_ENC_BUFFER_FORMAT_NV12         = 0x00000001U,
    NV_ENC_BUFFER_FORMAT_YV12         = 0x00000010U,
    NV_ENC_BUFFER_FORMAT_IYUV         = 0x00000100U,
    NV_ENC_BUFFER_FORMAT_YUV444       = 0x00001000U, /* 8-bit 4:4:4 (Selkies / GRD) */
    NV_ENC_BUFFER_FORMAT_YUV420_10BIT = 0x00010000U, /* 10-bit 4:2:0 P010 */
    NV_ENC_BUFFER_FORMAT_YUV444_10BIT = 0x00100000U, /* 10-bit 4:4:4 (Selkies PixelFlux 10-bit 4:4:4) */
    NV_ENC_BUFFER_FORMAT_ARGB         = 0x01000000U,
    NV_ENC_BUFFER_FORMAT_ARGB10       = 0x02000000U,
    NV_ENC_BUFFER_FORMAT_AYUV         = 0x04000000U,
    NV_ENC_BUFFER_FORMAT_ABGR         = 0x10000000U,
    NV_ENC_BUFFER_FORMAT_ABGR10       = 0x20000000U
} NV_ENC_BUFFER_FORMAT;

typedef enum _NV_ENC_BIT_DEPTH {
    NV_ENC_BIT_DEPTH_INVALID = 0,
    NV_ENC_BIT_DEPTH_8       = 8,
    NV_ENC_BIT_DEPTH_10      = 10
} NV_ENC_BIT_DEPTH;

typedef enum _NV_ENC_INPUT_RESOURCE_TYPE {
    NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX       = 0x0,
    NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR = 0x1,
    NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY     = 0x2,
    NV_ENC_INPUT_RESOURCE_TYPE_OPENGL_TEX    = 0x3
} NV_ENC_INPUT_RESOURCE_TYPE;

typedef enum _NV_ENC_PIC_TYPE {
    NV_ENC_PIC_TYPE_P             = 0x0,
    NV_ENC_PIC_TYPE_B             = 0x1,
    NV_ENC_PIC_TYPE_I             = 0x2,
    NV_ENC_PIC_TYPE_IDR           = 0x3,
    NV_ENC_PIC_TYPE_BI            = 0x4,
    NV_ENC_PIC_TYPE_SKIPPED       = 0x5,
    NV_ENC_PIC_TYPE_INTRA_REFRESH = 0x6,
    NV_ENC_PIC_TYPE_UNKNOWN       = 0xFF
} NV_ENC_PIC_TYPE;

typedef enum _NV_ENC_PIC_FLAGS {
    NV_ENC_PIC_FLAG_FORCEINTRA    = 0x01U,
    NV_ENC_PIC_FLAG_FORCEIDR      = 0x02U,
    NV_ENC_PIC_FLAG_OUTPUT_SPSPPS = 0x04U,
    NV_ENC_PIC_FLAG_EOS           = 0x08U
} NV_ENC_PIC_FLAGS;

typedef enum _NV_ENC_PARAMS_RC_MODE {
    NV_ENC_PARAMS_RC_CONSTQP          = 0x0,
    NV_ENC_PARAMS_RC_VBR              = 0x1,
    NV_ENC_PARAMS_RC_CBR              = 0x2,
    NV_ENC_PARAMS_RC_CBR_LOWDELAY_HQ  = 0x8,
    NV_ENC_PARAMS_RC_CBR_HQ           = 0x10,
    NV_ENC_PARAMS_RC_VBR_HQ           = 0x20
} NV_ENC_PARAMS_RC_MODE;

typedef enum _NV_ENC_CAPS {
    NV_ENC_CAPS_NUM_MAX_BFRAMES                  = 0,
    NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES      = 1,
    NV_ENC_CAPS_SUPPORT_FIELD_ENCODING           = 2,
    NV_ENC_CAPS_SUPPORT_MONOCHROME               = 3,
    NV_ENC_CAPS_SUPPORT_FMO                      = 4,
    NV_ENC_CAPS_SUPPORT_QPELMV                   = 5,
    NV_ENC_CAPS_SUPPORT_BDIRECT_MODE             = 6,
    NV_ENC_CAPS_SUPPORT_CABAC                    = 7,
    NV_ENC_CAPS_SUPPORT_ADAPTIVE_TRANSFORM       = 8,
    NV_ENC_CAPS_SUPPORT_STEREO_MVC               = 9,
    NV_ENC_CAPS_NUM_MAX_TEMPORAL_LAYERS          = 10,
    NV_ENC_CAPS_SUPPORT_HIERARCHICAL_PFRAMES     = 11,
    NV_ENC_CAPS_SUPPORT_HIERARCHICAL_BFRAMES     = 12,
    NV_ENC_CAPS_LEVEL_MAX                        = 13,
    NV_ENC_CAPS_LEVEL_MIN                        = 14,
    NV_ENC_CAPS_SEPARATE_COLOUR_PLANE            = 15,
    NV_ENC_CAPS_WIDTH_MAX                        = 16,
    NV_ENC_CAPS_HEIGHT_MAX                       = 17,
    NV_ENC_CAPS_SUPPORT_TEMPORAL_SVC             = 18,
    NV_ENC_CAPS_SUPPORT_DYN_RES_CHANGE           = 19,
    NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE       = 20,
    NV_ENC_CAPS_SUPPORT_DYN_FORCE_CONSTQP        = 21,
    NV_ENC_CAPS_SUPPORT_DYN_RCMODE_CHANGE        = 22,
    NV_ENC_CAPS_SUPPORT_SUBFRAME_READBACK        = 23,
    NV_ENC_CAPS_SUPPORT_CONSTRAINED_ENCODING     = 24,
    NV_ENC_CAPS_SUPPORT_INTRA_REFRESH            = 25,
    NV_ENC_CAPS_SUPPORT_CUSTOM_VBV_BUF_SIZE      = 26,
    NV_ENC_CAPS_SUPPORT_DYNAMIC_SLICE_MODE       = 27,
    NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION     = 28,
    NV_ENC_CAPS_PREPROC_SUPPORT                  = 29,
    NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT             = 30,
    NV_ENC_CAPS_MB_NUM_MAX                       = 31,
    NV_ENC_CAPS_MB_PER_SEC_MAX                   = 32,
    NV_ENC_CAPS_SUPPORT_YUV444_ENCODE            = 33,
    NV_ENC_CAPS_SUPPORT_LOSSLESS_ENCODE          = 34,
    NV_ENC_CAPS_SUPPORT_SAO                      = 35,
    NV_ENC_CAPS_SUPPORT_MEONLY_MODE              = 36,
    NV_ENC_CAPS_SUPPORT_LOOKAHEAD                = 37,
    NV_ENC_CAPS_SUPPORT_TEMPORAL_AQ              = 38,
    NV_ENC_CAPS_SUPPORT_10BIT_ENCODE             = 39,
    NV_ENC_CAPS_NUM_MAX_LTR_FRAMES               = 40,
    NV_ENC_CAPS_SUPPORT_WEIGHTED_PREDICTION      = 41,
    NV_ENC_CAPS_DYNAMIC_QUERY_ENCODER_CAPACITY   = 42,
    NV_ENC_CAPS_SUPPORT_BFRAME_REF_MODE          = 43,
    NV_ENC_CAPS_SUPPORT_EMPHASIS_LEVEL_MAP       = 44,
    NV_ENC_CAPS_WIDTH_MIN                        = 45,
    NV_ENC_CAPS_HEIGHT_MIN                       = 46,
    NV_ENC_CAPS_SUPPORT_MULTIPLE_REF_FRAMES      = 47,
    NV_ENC_CAPS_SUPPORT_ALPHA_LAYER_ENCODING     = 48,
    NV_ENC_CAPS_NUM_ENCODER_ENGINES              = 49,
    NV_ENC_CAPS_SINGLE_SLICE_INTRA_REFRESH       = 50,
    NV_ENC_CAPS_EXPOSED_COUNT                    = 51
} NV_ENC_CAPS;

typedef enum _NV_ENC_TUNING_INFO {
    NV_ENC_TUNING_INFO_UNDEFINED         = 0,
    NV_ENC_TUNING_INFO_HIGH_QUALITY      = 1,
    NV_ENC_TUNING_INFO_LOW_LATENCY       = 2,
    NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY = 3,
    NV_ENC_TUNING_INFO_LOSSLESS          = 4
} NV_ENC_TUNING_INFO;

typedef struct _NVENC_GUID {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} NVENC_GUID;

/* Standard NVENC Codec, Profile, and Preset GUIDs */
extern const NVENC_GUID NV_ENC_CODEC_H264_GUID;
extern const NVENC_GUID NV_ENC_CODEC_HEVC_GUID;

extern const NVENC_GUID NV_ENC_CODEC_PROFILE_AUTOSELECT_GUID;
extern const NVENC_GUID NV_ENC_H264_PROFILE_BASELINE_GUID;
extern const NVENC_GUID NV_ENC_H264_PROFILE_MAIN_GUID;
extern const NVENC_GUID NV_ENC_H264_PROFILE_HIGH_GUID;
extern const NVENC_GUID NV_ENC_H264_PROFILE_HIGH_444_GUID;

extern const NVENC_GUID NV_ENC_HEVC_PROFILE_MAIN_GUID;
extern const NVENC_GUID NV_ENC_HEVC_PROFILE_MAIN10_GUID;
extern const NVENC_GUID NV_ENC_HEVC_PROFILE_FREXT_GUID;

extern const NVENC_GUID NV_ENC_PRESET_P1_GUID;
extern const NVENC_GUID NV_ENC_PRESET_P2_GUID;
extern const NVENC_GUID NV_ENC_PRESET_P3_GUID;
extern const NVENC_GUID NV_ENC_PRESET_P4_GUID;
extern const NVENC_GUID NV_ENC_PRESET_P5_GUID;
extern const NVENC_GUID NV_ENC_PRESET_P6_GUID;
extern const NVENC_GUID NV_ENC_PRESET_P7_GUID;
extern const NVENC_GUID NV_ENC_PRESET_LOW_LATENCY_HQ_GUID;
extern const NVENC_GUID NV_ENC_PRESET_LOW_LATENCY_HP_GUID;
extern const NVENC_GUID NV_ENC_PRESET_LOSSLESS_DEFAULT_GUID;

typedef void *NV_ENC_INPUT_PTR;
typedef void *NV_ENC_OUTPUT_PTR;
typedef void *NV_ENC_REGISTERED_PTR;
typedef void *NV_ENC_CUSTREAM_PTR;

typedef struct _NV_ENC_QP {
    uint32_t qpInterP;
    uint32_t qpInterB;
    uint32_t qpIntra;
} NV_ENC_QP;

typedef struct _NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS {
    uint32_t           version;
    NV_ENC_DEVICE_TYPE deviceType;
    void              *device;
    void              *reserved;
    uint32_t           apiVersion;
    uint32_t           reserved1[253];
    void              *reserved2[64];
} NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS;

typedef struct _NV_ENC_CAPS_PARAM {
    uint32_t    version;
    NV_ENC_CAPS capsToQuery;
    uint32_t    reserved[62];
} NV_ENC_CAPS_PARAM;

typedef struct _NV_ENC_CONFIG_H264 {
    uint32_t enableStereoMVC : 1;
    uint32_t hierarchicalPFrames : 1;
    uint32_t hierarchicalBFrames : 1;
    uint32_t outputBufferingPeriodSEI : 1;
    uint32_t outputPictureTimingSEI : 1;
    uint32_t outputAUD : 1;
    uint32_t disableSPSPPS : 1;
    uint32_t outputFramePackingSEI : 1;
    uint32_t outputRecoveryPointSEI : 1;
    uint32_t enableIntraRefresh : 1;
    uint32_t enableConstrainedEncoding : 1;
    uint32_t repeatSPSPPS : 1;
    uint32_t enableVFR : 1;
    uint32_t enableLTR : 1;
    uint32_t qpPrimeYZeroTransformBypassFlag : 1;
    uint32_t useConstrainedIntraPred : 1;
    uint32_t enableFillerDataInsertion : 1;
    uint32_t reservedBitFields : 15;
    uint32_t level;
    uint32_t idrPeriod;
    uint32_t separateColourPlaneFlag;
    uint32_t disableDeblockingFilterIDC;
    uint32_t numTemporalLayers;
    uint32_t spsId;
    uint32_t ppsId;
    uint32_t adaptiveTransformMode;
    uint32_t fmoMode;
    uint32_t bdirectMode;
    uint32_t entropyCodingMode;
    uint32_t stereoMode;
    uint32_t intraRefreshPeriod;
    uint32_t intraRefreshCnt;
    uint32_t maxNumRefFrames;
    uint32_t sliceMode;
    uint32_t sliceModeData;
    uint32_t chromaFormatIDC; /* 1 = 4:2:0, 3 = 4:4:4 (YUV444) */
    uint32_t maxTemporalLayers;
    uint32_t reserved1[260];
} NV_ENC_CONFIG_H264;

typedef struct _NV_ENC_CONFIG_HEVC_VUI_PARAMETERS {
    uint32_t overscanInfoPresentFlag;
    uint32_t overscanInfo;
    uint32_t videoSignalTypePresentFlag;
    uint32_t videoFormat;
    uint32_t videoFullRangeFlag;
    uint32_t colourDescriptionPresentFlag;
    uint32_t colourPrimaries;
    uint32_t transferCharacteristics;
    uint32_t colourMatrix;
    uint32_t chromaSampleLocationFlag;
    uint32_t chromaSampleLocationTop;
    uint32_t chromaSampleLocationBot;
    uint32_t bitstreamRestrictionFlag;
    uint32_t timingInfoPresentFlag;
    uint32_t numUnitInTicks;
    uint32_t timeScale;
    uint32_t reserved[12];
} NV_ENC_CONFIG_HEVC_VUI_PARAMETERS;

/*
 * Exact upstream x86_64 NVENC 13.0 layout:
 *   sizeof(NV_ENC_CONFIG_HEVC) == 1560
 *   offsetof(outputBitDepth)   == 200
 *   offsetof(inputBitDepth)    == 204
 * Note: bits 11..13 at +0x10 preserve pixelBitDepthMinus8 alias for 12.1 callers.
 */
typedef struct _NV_ENC_CONFIG_HEVC {
    uint32_t level;
    uint32_t tier;
    uint32_t minCUSize;
    uint32_t maxCUSize;
    uint32_t useConstrainedIntraPred : 1;
    uint32_t disableDeblockAcrossSliceBoundary : 1;
    uint32_t outputBufferingPeriodSEI : 1;
    uint32_t outputPictureTimingSEI : 1;
    uint32_t outputAUD : 1;
    uint32_t enableLTR : 1;
    uint32_t disableSPSPPS : 1;
    uint32_t repeatSPSPPS : 1;
    uint32_t enableIntraRefresh : 1;
    uint32_t chromaFormatIDC : 2; /* 1 = 4:2:0, 3 = 4:4:4 (YUV444) */
    uint32_t pixelBitDepthMinus8 : 3; /* Legacy 12.1 field / 13.0 reserved3 */
    uint32_t enableFillerDataInsertion : 1;
    uint32_t enableConstrainedEncoding : 1;
    uint32_t enableAlphaLayerEncoding : 1;
    uint32_t singleSliceIntraRefresh : 1;
    uint32_t outputRecoveryPointSEI : 1;
    uint32_t outputTimeCodeSEI : 1;
    uint32_t enableTemporalSVC : 1;
    uint32_t enableMVHEVC : 1;
    uint32_t outputHevc3DReferenceDisplayInfo : 1;
    uint32_t outputMaxCll : 1;
    uint32_t outputMasteringDisplay : 1;
    uint32_t reserved0 : 1;
    uint32_t enableRefPicListModification : 1;
    uint32_t reserved : 5;
    uint32_t idrPeriod;
    uint32_t intraRefreshPeriod;
    uint32_t intraRefreshCnt;
    uint32_t maxNumRefFramesInDPB;
    uint32_t ltrNumFrames;
    uint32_t vpsId;
    uint32_t spsId;
    uint32_t ppsId;
    uint32_t sliceMode;
    uint32_t sliceModeData;
    uint32_t maxTemporalLayersMinus1;
    NV_ENC_CONFIG_HEVC_VUI_PARAMETERS hevcVUIParameters;
    uint32_t ltrTrustMode;
    uint32_t useBFramesAsRef;
    uint32_t numRefL0;
    uint32_t numRefL1;
    uint32_t tfLevel;
    uint32_t disableDeblockingFilterIDC;
    NV_ENC_BIT_DEPTH outputBitDepth;
    NV_ENC_BIT_DEPTH inputBitDepth;
    uint32_t numTemporalLayers;
    uint32_t numViews;
    uint32_t reserved1[208];
    void    *reserved2[64];
} NV_ENC_CONFIG_HEVC;

/* Exact upstream x86_64 NVENC 13.0 layout: sizeof == 128 */
typedef struct _NV_ENC_RC_PARAMS {
    uint32_t  version;
    uint32_t  rateControlMode;
    NV_ENC_QP constQP;
    uint32_t  averageBitRate;
    uint32_t  maxBitRate;
    uint32_t  vbvBufferSize;
    uint32_t  vbvInitialDelay;
    uint32_t  enableMinQP : 1;
    uint32_t  enableMaxQP : 1;
    uint32_t  enableInitialRCQP : 1;
    uint32_t  enableAQ : 1;
    uint32_t  reservedBitField1 : 1;
    uint32_t  enableLookahead : 1;
    uint32_t  disableIadapt : 1;
    uint32_t  disableBadapt : 1;
    uint32_t  enableTemporalAQ : 1;
    uint32_t  zeroReorderDelay : 1;
    uint32_t  enableNonRefP : 1;
    uint32_t  strictGOPTarget : 1;
    uint32_t  aqStrength : 4;
    uint32_t  enableExtLookahead : 1;
    uint32_t  reservedBitFields : 15;
    NV_ENC_QP minQP;
    NV_ENC_QP maxQP;
    NV_ENC_QP initialRCQP;
    uint32_t  temporallayerIdxMask;
    uint8_t   temporalLayerQP[8];
    uint8_t   targetQuality;
    uint8_t   targetQualityLSB;
    uint16_t  lookaheadDepth;
    uint8_t   lowDelayKeyFrameScale;
    int8_t    yDcQPIndexOffset;
    int8_t    uDcQPIndexOffset;
    int8_t    vDcQPIndexOffset;
    uint32_t  qpMapMode;
    uint32_t  multiPass;
    uint32_t  alphaLayerBitrateRatio;
    int8_t    cbQPIndexOffset;
    int8_t    crQPIndexOffset;
    uint16_t  reserved2;
    uint32_t  lookaheadLevel;
    uint8_t   viewBitrateRatios[7];
    uint8_t   reserved3;
    uint32_t  reserved1;
} NV_ENC_RC_PARAMS;

/* Exact upstream x86_64 NVENC 13.0 layout: sizeof == 3584, rcParams == 40, encodeCodecConfig == 168 */
typedef struct _NV_ENC_CONFIG {
    uint32_t         version;
    NVENC_GUID       profileGUID;
    uint32_t         gopLength;
    int32_t          frameIntervalP;
    uint32_t         monoChromeEncoding;
    uint32_t         frameFieldMode;
    uint32_t         mvPrecision;
    NV_ENC_RC_PARAMS rcParams;
    union {
        NV_ENC_CONFIG_H264 h264Config;
        NV_ENC_CONFIG_HEVC hevcConfig;
        uint32_t           reserved[320];
        uint32_t           av1SizePad[448]; /* matches AV1 union member size (1792 B) */
    } encodeCodecConfig;
    uint32_t reserved[278];
    void    *reserved2[64];
} NV_ENC_CONFIG;

/* Exact upstream x86_64 NVENC 13.0 layout: sizeof == 5128, presetCfg == 8 */
typedef struct _NV_ENC_PRESET_CONFIG {
    uint32_t      version;
    NV_ENC_CONFIG presetCfg;
    uint32_t      reserved1[255];
    void         *reserved2[64];
} NV_ENC_PRESET_CONFIG;

typedef struct _NVENC_EXTERNAL_ME_HINT_COUNTS_PER_BLOCKTYPE {
    uint32_t numCandsPerBlk16x16 : 4;
    uint32_t numCandsPerBlk16x8  : 4;
    uint32_t numCandsPerBlk8x16  : 4;
    uint32_t numCandsPerBlk8x8   : 4;
    uint32_t numCandsPerSb       : 8;
    uint32_t reserved            : 8;
    uint32_t reserved1[3];
} NVENC_EXTERNAL_ME_HINT_COUNTS_PER_BLOCKTYPE;

/* Exact upstream x86_64 NVENC 13.0 layout: sizeof == 1800, encodeConfig == 88, tuningInfo == 136, bufferFormat == 140 */
typedef struct _NV_ENC_INITIALIZE_PARAMS {
    uint32_t           version;
    NVENC_GUID         encodeGUID;
    NVENC_GUID         presetGUID;
    uint32_t           encodeWidth;
    uint32_t           encodeHeight;
    uint32_t           darWidth;
    uint32_t           darHeight;
    uint32_t           frameRateNum;
    uint32_t           frameRateDen;
    uint32_t           enableEncodeAsync;
    uint32_t           enablePTD;
    uint32_t           reportSliceOffsets : 1;
    uint32_t           enableSubFrameWrite : 1;
    uint32_t           enableExternalMEHints : 1;
    uint32_t           enableMEOnlyMode : 1;
    uint32_t           enableWeightedPrediction : 1;
    uint32_t           splitEncodeMode : 4;
    uint32_t           enableOutputInVidmem : 1;
    uint32_t           enableReconFrameOutput : 1;
    uint32_t           enableOutputStats : 1;
    uint32_t           enableUniDirectionalB : 1;
    uint32_t           reservedBitFields : 19;
    uint32_t           privDataSize;
    uint32_t           reserved;
    void              *privData;
    NV_ENC_CONFIG     *encodeConfig;
    uint32_t           maxEncodeWidth;
    uint32_t           maxEncodeHeight;
    NVENC_EXTERNAL_ME_HINT_COUNTS_PER_BLOCKTYPE maxMEHintCountsPerBlock[2];
    NV_ENC_TUNING_INFO tuningInfo;
    NV_ENC_BUFFER_FORMAT bufferFormat;
    uint32_t           numStateBuffers;
    uint32_t           outputStatsLevel;
    uint32_t           reserved1[284];
    void              *reserved2[64];
} NV_ENC_INITIALIZE_PARAMS;

typedef struct _NV_ENC_CREATE_INPUT_BUFFER {
    uint32_t             version;
    uint32_t             width;
    uint32_t             height;
    uint32_t             memoryHeap;
    NV_ENC_BUFFER_FORMAT bufferFmt;
    uint32_t             reserved;
    NV_ENC_INPUT_PTR     inputBuffer;
    void                *pSysMemBuffer;
    uint32_t             reserved1[57];
    void                *reserved2[64];
} NV_ENC_CREATE_INPUT_BUFFER;

typedef struct _NV_ENC_LOCK_INPUT_BUFFER {
    uint32_t         version;
    uint32_t         doNotWait : 1;
    uint32_t         reservedBitFields : 31;
    NV_ENC_INPUT_PTR inputBuffer;
    void            *bufferDataPtr;
    uint32_t         pitch;
    uint32_t         reserved1[62];
    void            *reserved2[64];
} NV_ENC_LOCK_INPUT_BUFFER;

typedef struct _NV_ENC_CREATE_BITSTREAM_BUFFER {
    uint32_t          version;
    uint32_t          size;
    uint32_t          memoryHeap;
    uint32_t          reserved;
    NV_ENC_OUTPUT_PTR bitstreamBuffer;
    void             *bitstreamBufferPtr;
    uint32_t          reserved1[58];
    void             *reserved2[64];
} NV_ENC_CREATE_BITSTREAM_BUFFER;

/* Exact upstream x86_64 NVENC 13.0 layout: sizeof == 1544, outputBitstream == 8, bitstreamSizeInBytes == 36, bitstreamBufferPtr == 56, pictureType == 64 */
typedef struct _NV_ENC_LOCK_BITSTREAM {
    uint32_t          version;
    uint32_t          doNotWait : 1;
    uint32_t          ltrFrame : 1;
    uint32_t          getRCStats : 1;
    uint32_t          reservedBitFields : 29;
    NV_ENC_OUTPUT_PTR outputBitstream;
    uint32_t         *sliceOffsets;
    uint32_t          frameIdx;
    uint32_t          hwEncodeStatus;
    uint32_t          numSlices;
    uint32_t          bitstreamSizeInBytes;
    uint64_t          outputTimeStamp;
    uint64_t          outputDuration;
    void             *bitstreamBufferPtr;
    NV_ENC_PIC_TYPE   pictureType;
    uint32_t          pictureStruct;
    uint32_t          frameAvgQP;
    uint32_t          frameSatd;
    uint32_t          ltrFrameIdx;
    uint32_t          ltrFrameBitmap;
    uint32_t          temporalId;
    uint32_t          intraMBCount;
    uint32_t          interMBCount;
    int32_t           averageMVX;
    int32_t           averageMVY;
    uint32_t          alphaLayerSizeInBytes;
    uint32_t          outputStatsPtrSize;
    uint32_t          reserved;
    void             *outputStatsPtr;
    uint32_t          frameIdxDisplay;
    uint32_t          reserved1[219];
    void             *reserved2[63];
    uint32_t          reservedInternal[8];
} NV_ENC_LOCK_BITSTREAM;

/* Exact upstream x86_64 NVENC 13.0 layout: sizeof == 3360, inputBuffer == 40, outputBitstream == 48, bufferFmt == 64, pictureType == 72 */
typedef struct _NV_ENC_PIC_PARAMS {
    uint32_t             version;
    uint32_t             inputWidth;
    uint32_t             inputHeight;
    uint32_t             inputPitch;
    uint32_t             encodePicFlags;
    uint32_t             frameIdx;
    uint64_t             inputTimeStamp;
    uint64_t             inputDuration;
    NV_ENC_INPUT_PTR     inputBuffer;
    NV_ENC_OUTPUT_PTR    outputBitstream;
    void                *completionEvent;
    NV_ENC_BUFFER_FORMAT bufferFmt;
    uint32_t             pictureStruct;
    NV_ENC_PIC_TYPE      pictureType;
    uint32_t             pictureTypePad;
    uint8_t              codecPicParams[1632];
    int8_t              *qpDeltaMap;
    uint32_t             qpDeltaMapSize;
    uint32_t             reservedBitFields;
    uint16_t             meHintRefPicDist[2];
    int32_t              diffPicNumHint;
    NV_ENC_INPUT_PTR     alphaBuffer;
    void                *meExternalSbHints;
    uint32_t             meSbHintsCount;
    uint32_t             stateBufferIdx;
    NV_ENC_OUTPUT_PTR    outputReconBuffer;
    uint32_t             reserved3[284];
    void                *reserved6[57];
} NV_ENC_PIC_PARAMS;

/* Exact upstream x86_64 NVENC 13.0 layout: sizeof == 1536, resourceToRegister == 24, registeredResource == 32, bufferFormat == 40 */
typedef struct _NV_ENC_REGISTER_RESOURCE {
    uint32_t                   version;
    NV_ENC_INPUT_RESOURCE_TYPE resourceType;
    uint32_t                   width;
    uint32_t                   height;
    uint32_t                   pitch;
    uint32_t                   subResourceIndex;
    void                      *resourceToRegister;
    NV_ENC_REGISTERED_PTR      registeredResource;
    NV_ENC_BUFFER_FORMAT       bufferFormat;
    uint32_t                   bufferUsage;
    void                      *pInputFencePoint;
    uint32_t                   chromaOffset[2];
    uint32_t                   chromaOffsetIn[2];
    uint32_t                   reserved1[244];
    void                      *reserved2[61];
} NV_ENC_REGISTER_RESOURCE;

/* Exact upstream x86_64 NVENC 13.0 layout: sizeof == 1544, registeredResource == 16, mappedResource == 24, mappedBufferFmt == 32 */
typedef struct _NV_ENC_MAP_INPUT_RESOURCE {
    uint32_t              version;
    uint32_t              subResourceIndex;
    void                 *inputResource;
    NV_ENC_REGISTERED_PTR registeredResource;
    NV_ENC_INPUT_PTR      mappedResource;
    NV_ENC_BUFFER_FORMAT  mappedBufferFmt;
    uint32_t              reserved1[251];
    void                 *reserved2[63];
} NV_ENC_MAP_INPUT_RESOURCE;

typedef struct _NV_ENC_RECONFIGURE_PARAMS {
    uint32_t                 version;
    NV_ENC_INITIALIZE_PARAMS reInitEncodeParams;
    uint32_t                 resetEncoder : 1;
    uint32_t                 forceIDR : 1;
    uint32_t                 reserved : 30;
} NV_ENC_RECONFIGURE_PARAMS;

typedef struct _NV_ENC_STAT {
    uint32_t          version;
    uint32_t          reserved;
    NV_ENC_OUTPUT_PTR outputBitStream;
    uint32_t          bitStreamSize;
    uint32_t          picType;
    uint32_t          lastValidByteOffset;
    uint32_t          sliceOffsets[16];
    uint32_t          picIdx;
    uint32_t          reserved1[233];
    void             *reserved2[64];
} NV_ENC_STAT;

typedef struct _NV_ENC_SEQUENCE_PARAM_PAYLOAD {
    uint32_t  version;
    uint32_t  inBufferSize;
    uint32_t  spsId;
    uint32_t  ppsId;
    void     *spsppsBuffer;
    uint32_t *outSPSPPSPayloadSize;
    uint32_t  reserved[250];
    void     *reserved2[64];
} NV_ENC_SEQUENCE_PARAM_PAYLOAD;

/* NV_ENCODE_API_FUNCTION_LIST - 43 slots at +0x08..+0x158 */
typedef struct _NV_ENCODE_API_FUNCTION_LIST {
    uint32_t version;
    uint32_t reserved;
    NVENCSTATUS (*nvEncOpenEncodeSession)(void *device, uint32_t deviceType, void **encoder);
    NVENCSTATUS (*nvEncGetEncodeGUIDCount)(void *encoder, uint32_t *encodeGUIDCount);
    NVENCSTATUS (*nvEncGetEncodeProfileGUIDCount)(void *encoder, NVENC_GUID encodeGUID, uint32_t *encodeProfileGUIDCount);
    NVENCSTATUS (*nvEncGetEncodeProfileGUIDs)(void *encoder, NVENC_GUID encodeGUID, NVENC_GUID *profileGUIDs, uint32_t guidArraySize, uint32_t *GUIDCount);
    NVENCSTATUS (*nvEncGetEncodeGUIDs)(void *encoder, NVENC_GUID *GUIDs, uint32_t guidArraySize, uint32_t *GUIDCount);
    NVENCSTATUS (*nvEncGetInputFormatCount)(void *encoder, NVENC_GUID encodeGUID, uint32_t *inputFmtCount);
    NVENCSTATUS (*nvEncGetInputFormats)(void *encoder, NVENC_GUID encodeGUID, NV_ENC_BUFFER_FORMAT *inputFmts, uint32_t inputFmtArraySize, uint32_t *inputFmtCount);
    NVENCSTATUS (*nvEncGetEncodeCaps)(void *encoder, NVENC_GUID encodeGUID, NV_ENC_CAPS_PARAM *capsParam, int *capsVal);
    NVENCSTATUS (*nvEncGetEncodePresetCount)(void *encoder, NVENC_GUID encodeGUID, uint32_t *encodePresetGUIDCount);
    NVENCSTATUS (*nvEncGetEncodePresetGUIDs)(void *encoder, NVENC_GUID encodeGUID, NVENC_GUID *presetGUIDs, uint32_t guidArraySize, uint32_t *encodePresetGUIDCount);
    NVENCSTATUS (*nvEncGetEncodePresetConfig)(void *encoder, NVENC_GUID encodeGUID, NVENC_GUID presetGUID, NV_ENC_PRESET_CONFIG *presetConfig);
    NVENCSTATUS (*nvEncInitializeEncoder)(void *encoder, NV_ENC_INITIALIZE_PARAMS *createEncodeParams);
    NVENCSTATUS (*nvEncCreateInputBuffer)(void *encoder, NV_ENC_CREATE_INPUT_BUFFER *createInputBufferParams);
    NVENCSTATUS (*nvEncDestroyInputBuffer)(void *encoder, NV_ENC_INPUT_PTR inputBuffer);
    NVENCSTATUS (*nvEncCreateBitstreamBuffer)(void *encoder, NV_ENC_CREATE_BITSTREAM_BUFFER *createBitstreamBufferParams);
    NVENCSTATUS (*nvEncDestroyBitstreamBuffer)(void *encoder, NV_ENC_OUTPUT_PTR bitstreamBuffer);
    NVENCSTATUS (*nvEncEncodePicture)(void *encoder, NV_ENC_PIC_PARAMS *encodePicParams);
    NVENCSTATUS (*nvEncLockBitstream)(void *encoder, NV_ENC_LOCK_BITSTREAM *lockBitstreamBufferParams);
    NVENCSTATUS (*nvEncUnlockBitstream)(void *encoder, NV_ENC_OUTPUT_PTR bitstreamBuffer);
    NVENCSTATUS (*nvEncLockInputBuffer)(void *encoder, NV_ENC_LOCK_INPUT_BUFFER *lockInputBufferParams);
    NVENCSTATUS (*nvEncUnlockInputBuffer)(void *encoder, NV_ENC_INPUT_PTR inputBuffer);
    NVENCSTATUS (*nvEncGetEncodeStats)(void *encoder, NV_ENC_STAT *encodeStats);
    NVENCSTATUS (*nvEncGetSequenceParams)(void *encoder, NV_ENC_SEQUENCE_PARAM_PAYLOAD *sequenceParamPayload);
    NVENCSTATUS (*nvEncRegisterAsyncEvent)(void *encoder, void *eventParams);
    NVENCSTATUS (*nvEncUnregisterAsyncEvent)(void *encoder, void *eventParams);
    NVENCSTATUS (*nvEncMapInputResource)(void *encoder, NV_ENC_MAP_INPUT_RESOURCE *mapInputResParams);
    NVENCSTATUS (*nvEncUnmapInputResource)(void *encoder, NV_ENC_INPUT_PTR mappedInputBuffer);
    NVENCSTATUS (*nvEncDestroyEncoder)(void *encoder);
    NVENCSTATUS (*nvEncInvalidateRefFrames)(void *encoder, uint64_t invalidRefFrameTimeStamp);
    NVENCSTATUS (*nvEncOpenEncodeSessionEx)(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS *openSessionExParams, void **encoder);
    NVENCSTATUS (*nvEncRegisterResource)(void *encoder, NV_ENC_REGISTER_RESOURCE *registerResParams);
    NVENCSTATUS (*nvEncUnregisterResource)(void *encoder, NV_ENC_REGISTERED_PTR registeredResource);
    NVENCSTATUS (*nvEncReconfigureEncoder)(void *encoder, NV_ENC_RECONFIGURE_PARAMS *reInitEncodeParams);
    NVENCSTATUS (*nvEncReserved1)(void *encoder, void *reservedParams);
    NVENCSTATUS (*nvEncCreateMVBuffer)(void *encoder, void *createMVBufferParams);
    NVENCSTATUS (*nvEncDestroyMVBuffer)(void *encoder, NV_ENC_OUTPUT_PTR mvBuffer);
    NVENCSTATUS (*nvEncRunMotionEstimationOnly)(void *encoder, void *meOnlyParams);
    const char *(*nvEncGetLastErrorString)(void *encoder);
    NVENCSTATUS (*nvEncSetIOCudaStreams)(void *encoder, NV_ENC_CUSTREAM_PTR inputStream, NV_ENC_CUSTREAM_PTR outputStream);
    NVENCSTATUS (*nvEncGetEncodePresetConfigEx)(void *encoder, NVENC_GUID encodeGUID, NVENC_GUID presetGUID, NV_ENC_TUNING_INFO tuningInfo, NV_ENC_PRESET_CONFIG *presetConfig);
    NVENCSTATUS (*nvEncGetSequenceParamPayload)(void *encoder, void *spsppsPayload);
    NVENCSTATUS (*nvEncRestoreEncoderState)(void *encoder, void *restoreStateParams);
    NVENCSTATUS (*nvEncLookaheadPicture)(void *encoder, void *lookaheadParams);
    void       *reserved2[275];
} NV_ENCODE_API_FUNCTION_LIST;

/* NvTool profiler C++ vtable interface (0x12b570..0x12b6a0) */
typedef struct _NV_TOOL_VTABLE {
    void         (*pfnDtor0)(void *self);
    void         (*pfnDtor1)(void *self);
    void         (*pfnSetApiID)(void *self, uint64_t apiId, uint8_t domain);
    uint64_t     (*pfnGetApiID)(void *self);
    uint32_t     (*pfnGetApiFunctionCount)(void *self, uint8_t domain);
    const char **(*pfnGetApiNames)(void *self, uint8_t domain, uint32_t startIdx, int count);
} NV_TOOL_VTABLE;

typedef struct _NV_TOOL_INTERFACE {
    const NV_TOOL_VTABLE *vtable;
    uint64_t              api_id;
    uint64_t              domain;
    uint64_t              reserved;
} NV_TOOL_INTERFACE;

/* All 10 exported ELF dynamic symbols of libnvidia-encode.so */
NVENCSTATUS  NvEncodeAPICreateInstance(NV_ENCODE_API_FUNCTION_LIST *functionList);
NVENCSTATUS  NvEncodeAPIGetMaxSupportedVersion(uint32_t *version);
void         NvToolSetInterface(void *iface);
void        *NvToolGetInterface(void);
void        *NvToolCreateInterface(int type);
void         NvToolDestroyInterface(void *pToolIface);
void         NvToolSetApiID(void *pToolIface, uint64_t apiId, uint8_t domain);
uint64_t     NvToolGetApiID(void *pToolIface);
uint32_t     NvToolGetApiFunctionCount(void *pToolIface, uint8_t domain);
const char **NvToolGetApiNames(void *pToolIface, uint8_t domain, uint32_t startIdx, int count);

/* Clean-room helper & RM telemetry inspection API */
typedef struct _open_nvenc_rm_stats {
    uint32_t h_client;
    uint32_t h_device;
    uint32_t h_subdevice;
    uint32_t h_channel;
    uint32_t h_nvenc_obj;
    uint32_t nvenc_class;
    uint32_t encoder_capacity;
    uint32_t rm_alloc_count;
    uint32_t rm_ctrl_count;
    uint32_t rm_free_count;
    uint32_t frames_encoded;
    int      rm_connected;
    int      hw_encode_active;
    uint32_t last_hw_encoded_bits;
} open_nvenc_rm_stats_t;

uint32_t open_nvenc_decode_struct_version(uint32_t struct_ver, int *out_valid);
int      open_nvenc_get_rm_stats(void *encoder, open_nvenc_rm_stats_t *out_stats);

#ifdef __cplusplus
}
#endif

#endif /* OPEN_NVENC_H */
