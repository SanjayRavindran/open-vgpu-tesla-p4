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
#define OPEN_NV_VGPU_GUEST_ID_58         (OPEN_NV_VGPU_BASE + 0x58U)

#define OPEN_VRPC_MAGIC                  0x43505256U /* "VRPC" */
#define OPEN_VRPC_VER_3_0                0x03000000U
#define OPEN_VRPC_PAGE_BYTES             4096U
#define OPEN_VRPC_CHUNK_MAX              4056U
#define OPEN_VRPC_STATIC_INFO_BYTES      0x10758U    /* 67,416 B */
#define OPEN_VRPC_GR_STATIC_INFO_BYTES   0x07b30U    /* 31,536 B */
#define OPEN_VRPC_CORE_BLOCK_OFF         0x00b98U
#define OPEN_VRPC_POLL_ITERS             40000U      /* 40000 * 50us = 2s */

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
} OPEN_VGPU_BAR0_STATE;

NvBool    openVgpuDetectBar0VirtualMode(struct OBJGPU *pGpu);
NV_STATUS openVgpuCreateObject(struct OBJGPU *pGpu);
void      openVgpuDestructObject(struct OBJGPU *pGpu);
struct OBJRPC            *openVgpuGetRpc(struct OBJGPU *pGpu);
struct _vgpu_static_info *openVgpuGetStaticInfo(struct OBJGPU *pGpu);

NV_STATUS openVgpuBar0RpcSendMessage(struct OBJGPU *pGpu, struct OBJRPC *pRpc);
NV_STATUS openVgpuBar0RpcRecvPoll(struct OBJGPU *pGpu, struct OBJRPC *pRpc, NvU32 expectedFunc);

NV_STATUS openVgpuRpcGetConsolidatedStaticInfo(struct OBJGPU *pGpu, struct OBJRPC *pRpc);
NV_STATUS openVgpuRpcGetConsolidatedGrStaticInfo(struct OBJGPU *pGpu, struct OBJRPC *pRpc);
NV_STATUS openVgpuRpcSetGuestSystemInfoExt(struct OBJGPU *pGpu, struct OBJRPC *pRpc, const char *branchVer);
NV_STATUS openVgpuRpcSimpleCall(struct OBJGPU *pGpu, struct OBJRPC *pRpc, NvU32 fn,
                                void *payload, NvU32 payloadLen);

#endif /* _OPEN_VGPU_BAR0_RPC_H_ */
