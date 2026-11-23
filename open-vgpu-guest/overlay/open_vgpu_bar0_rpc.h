/* SPDX-License-Identifier: MIT */
/*
 * open_vgpu_bar0_rpc.h - Clean-room BAR0 0x8f000 "VRPC" Guest Transport &
 * VGX 0x23.0x05 Static-Info Deserializer for open-gpu-kernel-modules 535.309.01
 */

#ifndef _OPEN_VGPU_BAR0_RPC_H_
#define _OPEN_VGPU_BAR0_RPC_H_

#include "nvtypes.h"
#include "nvstatus.h"

struct OBJGPU;
struct OBJRPC;
struct MEMORY_DESCRIPTOR;
struct GspStaticConfigInfo_t;
struct _RM_API;

#define OPEN_NV_VGPU_BASE                0x0008f000U
#define OPEN_NV_VGPU_SEND_RING_LO        (OPEN_NV_VGPU_BASE + 0x00U)
#define OPEN_NV_VGPU_SEND_RING_HI        (OPEN_NV_VGPU_BASE + 0x04U)
#define OPEN_NV_VGPU_SEND_RING_PUT       (OPEN_NV_VGPU_BASE + 0x08U)
#define OPEN_NV_VGPU_SEND_RING_GET       (OPEN_NV_VGPU_BASE + 0x0cU)
#define OPEN_NV_VGPU_RECV_RING_LO        (OPEN_NV_VGPU_BASE + 0x10U)
#define OPEN_NV_VGPU_RECV_RING_HI        (OPEN_NV_VGPU_BASE + 0x14U)
#define OPEN_NV_VGPU_RECV_RING_PUT       (OPEN_NV_VGPU_BASE + 0x18U)
#define OPEN_NV_VGPU_RECV_RING_GET       (OPEN_NV_VGPU_BASE + 0x1cU)
#define OPEN_NV_VGPU_CONFIG              (OPEN_NV_VGPU_BASE + 0x20U)
#define OPEN_NV_VGPU_FB_PAGES_M1         (OPEN_NV_VGPU_BASE + 0x24U)
#define OPEN_NV_VGPU_INTR_STATUS         (OPEN_NV_VGPU_BASE + 0x28U)
#define OPEN_NV_VGPU_INTR_ENABLE         (OPEN_NV_VGPU_BASE + 0x2cU)
#define OPEN_NV_VGPU_EVENT_RING_LO       (OPEN_NV_VGPU_BASE + 0x30U)
#define OPEN_NV_VGPU_EVENT_RING_HI       (OPEN_NV_VGPU_BASE + 0x34U)
#define OPEN_NV_VGPU_EVENT_RING_PUT      (OPEN_NV_VGPU_BASE + 0x38U)
#define OPEN_NV_VGPU_EVENT_RING_GET      (OPEN_NV_VGPU_BASE + 0x3cU)
#define OPEN_NV_VGPU_RPC_DOORBELL        (OPEN_NV_VGPU_BASE + 0x50U)
#define OPEN_NV_VGPU_MSG_RING_INVAL      (OPEN_NV_VGPU_BASE + 0x54U)
#define OPEN_NV_VGPU_GUEST_ID_58         (OPEN_NV_VGPU_BASE + 0x58U)

#define OPEN_VRPC_MAGIC                  0x43505256U /* "VRPC" */
#define OPEN_VRPC_VER_3_0                0x03000000U
#define OPEN_VRPC_PAGE_BYTES             4096U
#define OPEN_VRPC_HDR_BYTES              0x20U
#define OPEN_VRPC_MAX_PAYLOAD            (OPEN_VRPC_PAGE_BYTES - OPEN_VRPC_HDR_BYTES)
#define OPEN_VRPC_CHUNK_MAX              4056U
#define OPEN_VRPC_STATIC_INFO_BYTES      0x10758U    /* 67,416 B */
#define OPEN_VRPC_GR_STATIC_INFO_BYTES   0x07b30U    /* 31,536 B */
#define OPEN_VRPC_CORE_BLOCK_OFF         0x00b98U
#define OPEN_VRPC_POLL_ITERS             40000U      /* 40000 * 50us = 2s */

/* Live VGX 0x23.0x05 BAR0 VRPC opcodes verified against 535.309.01 host/plugin */
#define OPEN_VRPC_FN_SET_GUEST_INFO      0x01U
#define OPEN_VRPC_FN_ALLOC_ROOT          0x02U
#define OPEN_VRPC_FN_ALLOC_MEMORY        0x04U
#define OPEN_VRPC_FN_ALLOC_CTX_DMA       0x05U
#define OPEN_VRPC_FN_ALLOC_CHANNEL_DMA   0x06U
#define OPEN_VRPC_FN_MAP_MEMORY          0x07U
#define OPEN_VRPC_FN_ALLOC_OBJECT        0x09U
#define OPEN_VRPC_FN_FREE                0x0aU
#define OPEN_VRPC_FN_ALLOC_VIDMEM        0x0cU
#define OPEN_VRPC_FN_UNMAP_MEMORY        0x0dU
#define OPEN_VRPC_FN_ALLOC_SUBDEVICE     0x13U
#define OPEN_VRPC_FN_DUP_OBJECT          0x15U
#define OPEN_VRPC_FN_IDLE_CHANNELS       0x17U
#define OPEN_VRPC_FN_RM_API_CONTROL      0x1aU
#define OPEN_VRPC_FN_ALLOC_SHARE_DEVICE  0x20U
#define OPEN_VRPC_FN_UNLOAD_GUEST        0x2fU
#define OPEN_VRPC_FN_ALLOC_VIRTMEM       0x34U
#define OPEN_VRPC_FN_UPDATE_PDE_2        0x38U
#define OPEN_VRPC_FN_SET_GUEST_INFO_EXT  0x40U
#define OPEN_VRPC_FN_GET_STATIC_INFO     0x50U
#define OPEN_VRPC_FN_GET_GR_STATIC_INFO  0x9cU

#define OPEN_VRPC_MAX_TRACKED_HANDLES    1024U

typedef struct
{
    NvHandle hClient;
    NvHandle hObject;
    NvHandle hParent;
} OPEN_VGPU_HANDLE_REC;

typedef struct _vgpu_static_info
{
    NvU64 usableFbBytes;
    NvU64 bar1Bytes;
    NvU64 profileBytes;
    NvU32 vgpuTypeId;
    NvU16 pciDeviceId;
    NvU8  eccSupported;
    NvU32 frlFps;
    NvU32 encoderCapacity;
    NvU32 subsysId;
    NvU32 virtMode;
    char  hostDriverVer[64];
    char  profileName[64];
    NvU8 *pStaticInfoRaw;
    NvU32 staticInfoSize;
    NvU8 *pGrStaticInfoRaw;
    NvU32 grStaticInfoSize;
} OPEN_VGPU_STATIC_INFO_DATA;

typedef struct
{
    struct MEMORY_DESCRIPTOR *pDesc;
    void                     *pCpuAddr;
    void                     *pPriv;
    NvU64                     dmaAddr;
} OPEN_VGPU_DMA_PAGE;

typedef struct
{
    NvBool               bActive;
    NvBool               bHandshakeDone;
    NvU32                vgpuConfig;
    NvU64                virtualFbBytes;
    struct OBJRPC       *pRpc;
    void                *pRpcMessageBuf;
    OPEN_VGPU_DMA_PAGE   sendRing;
    OPEN_VGPU_DMA_PAGE   recvRing;
    OPEN_VGPU_DMA_PAGE   eventRing;
    OPEN_VGPU_DMA_PAGE   msgPage;
    NvU32                sendPut;
    NvU32                recvGet;
    NvU32                nextSeq;
    NvU64                vrpcCalls;
    OPEN_VGPU_STATIC_INFO_DATA staticInfo;
    struct GspStaticConfigInfo_t *pGspStaticInfo;
    OPEN_VGPU_HANDLE_REC trackedHandles[OPEN_VRPC_MAX_TRACKED_HANDLES];
    NvU32                trackedHandleCount;
} OPEN_VGPU_BAR0_STATE;

NvBool    openVgpuDetectBar0VirtualMode(struct OBJGPU *pGpu);
NV_STATUS openVgpuCreateObject(struct OBJGPU *pGpu);
void      openVgpuDestructObject(struct OBJGPU *pGpu);
struct OBJRPC               *openVgpuGetRpc(struct OBJGPU *pGpu);
struct _vgpu_static_info    *openVgpuGetStaticInfo(struct OBJGPU *pGpu);
struct GspStaticConfigInfo_t *openVgpuGetGspStaticInfo(struct OBJGPU *pGpu);

NV_STATUS openVgpuBar0RpcSendMessage(struct OBJGPU *pGpu, struct OBJRPC *pRpc);
NV_STATUS openVgpuBar0RpcRecvPoll(struct OBJGPU *pGpu, struct OBJRPC *pRpc, NvU32 expectedFunc);

NV_STATUS openVgpuRmApiAlloc(struct _RM_API *pRmApi, NvHandle hClient, NvHandle hParent,
                             NvHandle hObject, NvU32 hClass, void *pAllocParams, NvU32 allocParamsSize);
NV_STATUS openVgpuRmApiControl(struct _RM_API *pRmApi, NvHandle hClient, NvHandle hObject,
                               NvU32 cmd, void *pParamStructPtr, NvU32 paramsSize);
NV_STATUS openVgpuRmApiFree(struct _RM_API *pRmApi, NvHandle hClient, NvHandle hObject);
NV_STATUS openVgpuRmApiDupObject(struct _RM_API *pRmApi, NvHandle hClient, NvHandle hParent,
                                 NvHandle *phObject, NvHandle hClientSrc, NvHandle hObjectSrc,
                                 NvU32 flags);

NV_STATUS openVgpuRpcGetConsolidatedStaticInfo(struct OBJGPU *pGpu, struct OBJRPC *pRpc);
NV_STATUS openVgpuRpcGetConsolidatedGrStaticInfo(struct OBJGPU *pGpu, struct OBJRPC *pRpc);
NV_STATUS openVgpuRpcSetGuestSystemInfoExt(struct OBJGPU *pGpu, struct OBJRPC *pRpc, const char *branchVer);
NV_STATUS openVgpuRpcSimpleCall(struct OBJGPU *pGpu, struct OBJRPC *pRpc, NvU32 fn,
                                void *payload, NvU32 payloadLen);

NV_STATUS openVgpuRpcAllocLocalUser(struct OBJGPU *pGpu, NvHandle hClient, NvHandle hParent,
                                    NvHandle hMemory, struct MEMORY_DESCRIPTOR *pMemDesc,
                                    NvU64 length, NvU32 attr, NvU32 attr2, NvU32 format);
NV_STATUS openVgpuRpcAllocVidmem(struct OBJGPU *pGpu, NvHandle hClient, NvHandle hParent,
                                 NvHandle hMemory, struct MEMORY_DESCRIPTOR *pMemDesc,
                                 NvU64 size, NvU32 attr, NvU32 attr2, NvU32 type,
                                 NvU32 flags, NvU32 format, NvU32 comprCovg);
NV_STATUS openVgpuRpcAllocVirtmem(struct OBJGPU *pGpu, NvHandle hClient, NvHandle hParent,
                                  NvHandle hVASpace, NvHandle hMemory, NvU64 *pOffset,
                                  NvU64 size, NvU32 type, NvU32 flags, NvU64 alignment);
NV_STATUS openVgpuRpcMapMemory(struct OBJGPU *pGpu, NvHandle hClient, NvHandle hDevice,
                               NvHandle hMemory, NvU64 offset, NvU64 length,
                               NvU32 flags, NvU64 *pApertureOffset);
NV_STATUS openVgpuRpcUnmapMemory(struct OBJGPU *pGpu, NvHandle hClient, NvHandle hDevice,
                                 NvHandle hMemory, NvU32 flags, NvU64 apertureOffset);
NV_STATUS openVgpuRpcUpdatePde2(struct OBJGPU *pGpu, NvHandle hClient, NvHandle hDevice,
                                void *pUpdatePde2Params, NvU32 paramsSize);
NV_STATUS openVgpuRpcAllocContextDma(struct OBJGPU *pGpu, NvHandle hClient, NvHandle hDevice,
                                    NvHandle hDma, NvU32 hClass, NvU32 flags,
                                    NvHandle hMemory, NvU64 offset, NvU64 limit);

#endif /* _OPEN_VGPU_BAR0_RPC_H_ */
