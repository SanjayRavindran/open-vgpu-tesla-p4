/* SPDX-License-Identifier: MIT */
/*
 * open_vgpu_bar0_rpc.c - Clean-room BAR0 0x8f000 "VRPC" Guest RM Transport
 *
 * Bridges open-gpu-kernel-modules 535.309.01 OBJRPC to NVIDIA KVM vGPU's
 * BAR0 0x8f000..0x8f058 circular SEND_RING/RECV_RING/EVENT_RING transport,
 * pulls VGX 0x23.0x05 consolidated static info (0x50 67,416 B & 0x9c 31,536 B),
 * and serves non-GSP Pascal GP104 vGPU guests with zero proprietary blobs.
 */

#include "core/core.h"
#include "core/locks.h"
#include "os/os.h"
#include "gpu/gpu.h"
#include "gpu/gsp/gsp_static_config.h"
#include "gpu/mem_mgr/mem_desc.h"
#include "rmapi/rmapi.h"
#include "vgpu/rpc.h"
#include "vgpu/vgpu_version.h"
#include "vgpu/vgpu_events.h"
#include "vgpu/open_vgpu_bar0_rpc.h"
#include "nvport/nvport.h"
#include "nvVer.h"
#include "class/cl0000.h"
#include "class/cl0002.h"
#include "class/cl0040.h"
#include "class/cl0070.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "class/cl50a0.h"
#include "class/cl84a0.h"
#include "ctrl/ctrl0080/ctrl0080dma.h"
#include "ctrl/ctrl2080/ctrl2080bus.h"
#include "ctrl/ctrl2080/ctrl2080fb.h"
#include "ctrl/ctrl2080/ctrl2080fifo.h"
#include "ctrl/ctrl2080/ctrl2080internal.h"
#include "kernel/gpu/intr/engine_idx.h"

#define RPC_STRUCTURES
#define RPC_GENERIC_UNION
#include "g_rpc-structures.h"
#undef RPC_STRUCTURES
#undef RPC_GENERIC_UNION

#define RPC_MESSAGE_STRUCTURES
#define RPC_MESSAGE_GENERIC_UNION
#include "g_rpc-message-header.h"
#undef RPC_MESSAGE_STRUCTURES
#undef RPC_MESSAGE_GENERIC_UNION

#include "g_rpc_private.h"

static OPEN_VGPU_BAR0_STATE g_openVgpuBar0States[NV_MAX_DEVICES];

static OPEN_VGPU_BAR0_STATE *_openVgpuGetState(OBJGPU *pGpu)
{
    if (pGpu == NULL || pGpu->gpuInstance >= NV_MAX_DEVICES)
    {
        return NULL;
    }
    return &g_openVgpuBar0States[pGpu->gpuInstance];
}

static void _openVgpuTrackHandle(OPEN_VGPU_BAR0_STATE *pState,
                                 NvHandle hClient, NvHandle hObject, NvHandle hParent)
{
    NvU32 i;
    if (pState == NULL || hObject == 0U)
    {
        return;
    }
    for (i = 0U; i < pState->trackedHandleCount; i++)
    {
        if (pState->trackedHandles[i].hClient == hClient &&
            pState->trackedHandles[i].hObject == hObject)
        {
            pState->trackedHandles[i].hParent = hParent;
            return;
        }
    }
    if (pState->trackedHandleCount < OPEN_VRPC_MAX_TRACKED_HANDLES)
    {
        i = pState->trackedHandleCount++;
        pState->trackedHandles[i].hClient = hClient;
        pState->trackedHandles[i].hObject = hObject;
        pState->trackedHandles[i].hParent = hParent;
    }
}

static NvHandle _openVgpuLookupAndUntrackParent(OPEN_VGPU_BAR0_STATE *pState,
                                                NvHandle hClient, NvHandle hObject)
{
    NvU32 i;
    if (pState == NULL || hObject == 0U || hObject == hClient)
    {
        return 0U;
    }
    for (i = 0U; i < pState->trackedHandleCount; i++)
    {
        if (pState->trackedHandles[i].hClient == hClient &&
            pState->trackedHandles[i].hObject == hObject)
        {
            NvHandle hParent = pState->trackedHandles[i].hParent;
            if (i + 1U < pState->trackedHandleCount)
            {
                pState->trackedHandles[i] = pState->trackedHandles[pState->trackedHandleCount - 1U];
            }
            pState->trackedHandleCount--;
            return hParent;
        }
    }
    return 0U;
}

static NvU32 _openVgpuPteLo(NvU64 dmaAddr)
{
    NvU32 pfnLo = (NvU32)((dmaAddr >> 12) & 0x000fffffULL);
    return (pfnLo << 12) | 0x0000000bU;
}

static NvU32 _openVgpuRingPteLo(NvU64 dmaAddr)
{
    NvU32 pfnLo = (NvU32)((dmaAddr >> 12) & 0x000fffffULL);
    /* Bit 11 (0x800) selects GP_IN_RING required by libnvidia-vgpu 535.309.01 */
    return (pfnLo << 12) | 0x0000080bU;
}

static NvU32 _openVgpuPteHi(NvU64 dmaAddr)
{
    return (NvU32)((dmaAddr >> 32) & 0x000fffffULL);
}

static NvU16 _rd16(const NvU8 *p, NvU32 off)
{
    NvU16 v = 0;
    portMemCopy(&v, sizeof(v), p + off, sizeof(v));
    return v;
}

static NvU32 _rd32(const NvU8 *p, NvU32 off)
{
    NvU32 v = 0;
    portMemCopy(&v, sizeof(v), p + off, sizeof(v));
    return v;
}

static NvU64 _rd64(const NvU8 *p, NvU32 off)
{
    NvU64 v = 0;
    portMemCopy(&v, sizeof(v), p + off, sizeof(v));
    return v;
}

static void _wr64(NvU8 *p, NvU32 off, NvU64 v)
{
    portMemCopy(p + off, sizeof(v), &v, sizeof(v));
}

static void _wr32(NvU8 *p, NvU32 off, NvU32 v)
{
    portMemCopy(p + off, sizeof(v), &v, sizeof(v));
}

static NV_STATUS _openVgpuAcquireRpcLock(OBJGPU *pGpu, NvU32 *pMaskRelease)
{
    *pMaskRelease = 0U;
    if (pGpu == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }
    if (!rmDeviceGpuLockIsOwner(pGpu->gpuInstance))
    {
        return rmGpuGroupLockAcquire(pGpu->gpuInstance,
                                     GPU_LOCK_GRP_SUBDEVICE,
                                     GPU_LOCK_FLAGS_SAFE_LOCK_UPGRADE,
                                     RM_LOCK_MODULES_RPC,
                                     pMaskRelease);
    }
    return NV_OK;
}

static void _openVgpuReleaseRpcLock(NvU32 maskRelease)
{
    if (maskRelease != 0U)
    {
        rmGpuGroupLockRelease(maskRelease, GPUS_LOCK_FLAGS_NONE);
    }
}

static NvBool _openVgpuIsChannelClass(NvU32 hClass)
{
    switch (hClass)
    {
        case 0x906fU:
        case 0xa06fU:
        case 0xa16fU:
        case 0xa26fU:
        case 0xb06fU:
        case 0xc06fU:
        case 0xc36fU:
        case 0xc46fU:
        case 0xc56fU:
        case 0xc572U:
        case 0xc86fU:
            return NV_TRUE;
        default:
            return NV_FALSE;
    }
}

static void _copyAscii(char *dst, NvU32 dstSz, const char *src)
{
    NvU32 i = 0;
    if (dst == NULL || dstSz == 0U)
    {
        return;
    }
    if (src != NULL)
    {
        while (i + 1U < dstSz && src[i] != '\0')
        {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static NV_STATUS _openVgpuAllocDmaPage(OBJGPU *pGpu, OPEN_VGPU_DMA_PAGE *pPage)
{
    NV_STATUS status;
    NvP64 pVa = NvP64_NULL;
    NvP64 pPriv = NvP64_NULL;

    portMemSet(pPage, 0, sizeof(*pPage));

    status = memdescCreate(&pPage->pDesc, pGpu, OPEN_VRPC_PAGE_BYTES, RM_PAGE_SIZE,
                           NV_TRUE, ADDR_SYSMEM, NV_MEMORY_CACHED,
                           MEMDESC_FLAGS_KERNEL_MODE);
    if (status != NV_OK)
    {
        return status;
    }

    memdescSetFlag(pPage->pDesc, MEMDESC_FLAGS_KERNEL_MODE, NV_TRUE);

    status = memdescAlloc(pPage->pDesc);
    if (status != NV_OK)
    {
        memdescDestroy(pPage->pDesc);
        pPage->pDesc = NULL;
        return status;
    }

    status = memdescMap(pPage->pDesc, 0, OPEN_VRPC_PAGE_BYTES, NV_TRUE,
                        NV_PROTECT_WRITEABLE, &pVa, &pPriv);
    if (status != NV_OK || pVa == NvP64_NULL)
    {
        memdescFree(pPage->pDesc);
        memdescDestroy(pPage->pDesc);
        pPage->pDesc = NULL;
        return (status != NV_OK) ? status : NV_ERR_NO_MEMORY;
    }

    pPage->pCpuAddr = (void *)NvP64_VALUE(pVa);
    pPage->pPriv    = (void *)NvP64_VALUE(pPriv);
    pPage->dmaAddr  = (NvU64)memdescGetPhysAddr(pPage->pDesc, AT_GPU, 0);
    portMemSet(pPage->pCpuAddr, 0, OPEN_VRPC_PAGE_BYTES);
    return NV_OK;
}

static void _openVgpuFreeDmaPage(OPEN_VGPU_DMA_PAGE *pPage)
{
    if (pPage->pDesc != NULL)
    {
        if (pPage->pCpuAddr != NULL)
        {
            memdescUnmap(pPage->pDesc, NV_TRUE, 0,
                         NV_PTR_TO_NvP64(pPage->pCpuAddr),
                         NV_PTR_TO_NvP64(pPage->pPriv));
        }
        memdescFree(pPage->pDesc);
        memdescDestroy(pPage->pDesc);
    }
    portMemSet(pPage, 0, sizeof(*pPage));
}

static void _openVgpuProgramRings(OBJGPU *pGpu, OPEN_VGPU_BAR0_STATE *pState)
{
    portMemSet(pState->sendRing.pCpuAddr, 0, OPEN_VRPC_PAGE_BYTES);
    portMemSet(pState->recvRing.pCpuAddr, 0, OPEN_VRPC_PAGE_BYTES);
    portMemSet(pState->eventRing.pCpuAddr, 0, OPEN_VRPC_PAGE_BYTES);
    portAtomicMemoryFenceFull();

    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_GUEST_ID_58, 0x0aU);

    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_SEND_RING_LO, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_SEND_RING_PUT, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_SEND_RING_HI, _openVgpuPteHi(pState->sendRing.dmaAddr));
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_SEND_RING_LO, _openVgpuRingPteLo(pState->sendRing.dmaAddr));

    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_RECV_RING_LO, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_RECV_RING_GET, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_RECV_RING_HI, _openVgpuPteHi(pState->recvRing.dmaAddr));
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_RECV_RING_LO, _openVgpuRingPteLo(pState->recvRing.dmaAddr));

    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_EVENT_RING_LO, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_EVENT_RING_GET, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_EVENT_RING_HI, _openVgpuPteHi(pState->eventRing.dmaAddr));
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_EVENT_RING_LO, _openVgpuPteLo(pState->eventRing.dmaAddr));

    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_INTR_ENABLE, 0x3ffU);
}

static void _openVgpuTeardownRings(OBJGPU *pGpu)
{
    /* Clear *_RING_LO (VALID=0) before *_RING_HI=0 so host doesn't remap on HI write */
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_INTR_ENABLE, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_MSG_RING_INVAL, 1U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_SEND_RING_LO, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_SEND_RING_HI, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_RECV_RING_LO, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_RECV_RING_HI, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_EVENT_RING_LO, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_EVENT_RING_HI, 0U);
}

NvBool openVgpuDetectBar0VirtualMode(OBJGPU *pGpu)
{
    NvU32 vgpuCfg;

    if (pGpu == NULL)
    {
        return NV_FALSE;
    }

    /* Fail closed on bare-metal GPUs: bit 0 is set only inside hypervisor vGPU BAR0 */
    vgpuCfg = GPU_REG_RD32(pGpu, OPEN_NV_VGPU_CONFIG);
    if (vgpuCfg == 0xffffffffU || (vgpuCfg & 1U) == 0U)
    {
        return NV_FALSE;
    }
    return NV_TRUE;
}

NV_STATUS openVgpuBar0RpcSendMessage(OBJGPU *pGpu, OBJRPC *pRpc)
{
    OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
    rpc_message_header_v *pHdr;
    NvU32 totalLen;
    NvU32 copyLen;
    volatile NvU32 *pRing;

    if (pState == NULL || !pState->bActive || pRpc == NULL || pRpc->message_buffer == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }

    pHdr = (rpc_message_header_v *)pRpc->message_buffer;
    totalLen = pHdr->length;
    if (totalLen < sizeof(rpc_message_header_v) || totalLen > OPEN_VRPC_PAGE_BYTES)
    {
        return NV_ERR_INVALID_PARAM_STRUCT;
    }

    if (pHdr->sequence == 0U)
    {
        pHdr->sequence = pState->nextSeq++;
    }
    if (pHdr->function == NV_VGPU_MSG_FUNCTION_IDLE_CHANNELS)
    {
        pHdr->function = OPEN_VRPC_FN_IDLE_CHANNELS;
    }

    copyLen = totalLen;
    if (pHdr->function == OPEN_VRPC_FN_RM_API_CONTROL && totalLen == 0x40U)
    {
        NvU32 ctrlSize = _rd32((const NvU8 *)pRpc->message_buffer + 0x20U, 0x18U);
        if (ctrlSize > (OPEN_VRPC_PAGE_BYTES - 0x40U))
        {
            return NV_ERR_BUFFER_TOO_SMALL;
        }
        copyLen = 0x40U + ctrlSize;
    }

    portMemSet(pState->msgPage.pCpuAddr, 0, OPEN_VRPC_PAGE_BYTES);
    portMemCopy(pState->msgPage.pCpuAddr, copyLen, pRpc->message_buffer, copyLen);
    portAtomicMemoryFenceFull();

    /*
     * In GP_IN_RING mode (0x80b), SEND_RING[0..1] holds the staging page PTE
     * and SEND_RING[2] (+0x08) holds the PUT pointer written in guest RAM.
     */
    pRing = (volatile NvU32 *)pState->sendRing.pCpuAddr;
    pRing[0] = _openVgpuPteLo(pState->msgPage.dmaAddr);
    pRing[1] = _openVgpuPteHi(pState->msgPage.dmaAddr);
    portAtomicMemoryFenceFull();

    pState->sendPut = (pState->sendPut + 8U) & 0xfffU;
    pRing[2] = pState->sendPut;
    portAtomicMemoryFenceFull();

    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_RPC_DOORBELL, 0U);

    return NV_OK;
}

NV_STATUS openVgpuBar0RpcRecvPoll(OBJGPU *pGpu, OBJRPC *pRpc, NvU32 expectedFunc)
{
    OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
    rpc_message_header_v *pMsgHdr;
    volatile NvU32 *pRing;
    NvU32 waited = 0U;
    NvU32 recvPut = 0U;
    NvU32 copyLen;

    (void)expectedFunc;

    if (pState == NULL || !pState->bActive || pRpc == NULL || pRpc->message_buffer == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }

    pRing = (volatile NvU32 *)pState->recvRing.pCpuAddr;
    for (waited = 0U; waited < OPEN_VRPC_POLL_ITERS; waited++)
    {
        portAtomicMemoryFenceFull();
        recvPut = pRing[2]; /* RECV_RING +0x08: PUT */
        if (recvPut != pState->recvGet)
        {
            break;
        }
        osDelayUs(50);
    }

    if (recvPut == pState->recvGet)
    {
        NV_PRINTF(LEVEL_ERROR, "OPEN_VGPU: BAR0 VRPC recv poll timeout (fn=0x%x)\n", expectedFunc);
        return NV_ERR_TIMEOUT;
    }

    portAtomicMemoryFenceFull();
    pState->recvGet = (pState->recvGet + 8U) & 0xfffU;
    pRing[3] = pState->recvGet; /* RECV_RING +0x0c: GET */
    portAtomicMemoryFenceFull();

    pMsgHdr = (rpc_message_header_v *)pState->msgPage.pCpuAddr;
    if (pMsgHdr->signature != OPEN_VRPC_MAGIC)
    {
        return NV_ERR_INVALID_DATA;
    }

    copyLen = pMsgHdr->length;
    if (pMsgHdr->function == OPEN_VRPC_FN_RM_API_CONTROL && copyLen == 0x40U)
    {
        NvU32 ctrlSize = _rd32((const NvU8 *)pState->msgPage.pCpuAddr + 0x20U, 0x18U);
        if (ctrlSize <= (OPEN_VRPC_PAGE_BYTES - 0x40U))
        {
            copyLen = 0x40U + ctrlSize;
        }
        else
        {
            copyLen = OPEN_VRPC_PAGE_BYTES;
        }
    }
    if (copyLen < sizeof(rpc_message_header_v) || copyLen > OPEN_VRPC_PAGE_BYTES)
    {
        copyLen = OPEN_VRPC_PAGE_BYTES;
    }
    portMemCopy(pRpc->message_buffer, OPEN_VRPC_PAGE_BYTES, pState->msgPage.pCpuAddr, copyLen);
    pState->vrpcCalls++;
    return NV_OK;
}

NV_STATUS openVgpuRpcSimpleCall(OBJGPU *pGpu, OBJRPC *pRpc, NvU32 fn,
                                void *payload, NvU32 payloadLen)
{
    NV_STATUS status;
    rpc_message_header_v *pHdr;

    if (pRpc == NULL || pRpc->message_buffer == NULL ||
        sizeof(rpc_message_header_v) + payloadLen > OPEN_VRPC_PAGE_BYTES)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    status = rpcWriteCommonHeader(pGpu, pRpc, fn, payloadLen);
    if (status != NV_OK)
    {
        return status;
    }

    if (payloadLen > 0U && payload != NULL)
    {
        portMemCopy((NvU8 *)pRpc->message_buffer + sizeof(rpc_message_header_v),
                    payloadLen, payload, payloadLen);
    }

    status = rpcSendMessage(pGpu, pRpc);
    if (status != NV_OK)
    {
        return status;
    }

    status = rpcRecvPoll(pGpu, pRpc, fn);
    if (status != NV_OK)
    {
        return status;
    }

    pHdr = (rpc_message_header_v *)pRpc->message_buffer;
    if (payloadLen > 0U && payload != NULL)
    {
        portMemCopy(payload, payloadLen,
                    (NvU8 *)pRpc->message_buffer + sizeof(rpc_message_header_v), payloadLen);
    }

    return pHdr->rpc_result;
}

NV_STATUS openVgpuRpcSetGuestSystemInfoExt(OBJGPU *pGpu, OBJRPC *pRpc, const char *branchVer)
{
    NvU8 branchBuf[264];

    portMemSet(branchBuf, 0, sizeof(branchBuf));
    _copyAscii((char *)branchBuf, 64U, (branchVer != NULL) ? branchVer : "r535_00");
    return openVgpuRpcSimpleCall(pGpu, pRpc, NV_VGPU_MSG_FUNCTION_SET_GUEST_SYSTEM_INFO_EXT,
                                 branchBuf, (NvU32)sizeof(branchBuf));
}

static NV_STATUS _openVgpuPullChunkedStatic(OBJGPU *pGpu, OBJRPC *pRpc, NvU32 fn,
                                           NvU32 totalBytes, NvU8 *outBuf)
{
    NvU8 *chunkBuf;
    NvU32 off = 0U;
    NV_STATUS status = NV_OK;

    chunkBuf = portMemAllocNonPaged(8U + OPEN_VRPC_CHUNK_MAX);
    if (chunkBuf == NULL)
    {
        return NV_ERR_NO_MEMORY;
    }

    while (off < totalBytes)
    {
        NvU32 want = totalBytes - off;
        if (want > OPEN_VRPC_CHUNK_MAX)
        {
            want = OPEN_VRPC_CHUNK_MAX;
        }
        portMemSet(chunkBuf, 0, 8U + want);
        _wr32(chunkBuf, 0U, off);
        _wr32(chunkBuf, 4U, want);

        status = openVgpuRpcSimpleCall(pGpu, pRpc, fn, chunkBuf, 8U + want);
        if (status != NV_OK)
        {
            break;
        }
        portMemCopy(outBuf + off, want, chunkBuf + 8U, want);
        off += want;
    }

    portMemFree(chunkBuf);
    return status;
}

NV_STATUS openVgpuRpcGetConsolidatedStaticInfo(OBJGPU *pGpu, OBJRPC *pRpc)
{
    OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
    NV_STATUS status;
    const NvU8 *core;

    if (pState == NULL || pRpc == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }

    if (pState->staticInfo.pStaticInfoRaw == NULL)
    {
        pState->staticInfo.pStaticInfoRaw = portMemAllocNonPaged(OPEN_VRPC_STATIC_INFO_BYTES);
        if (pState->staticInfo.pStaticInfoRaw == NULL)
        {
            return NV_ERR_NO_MEMORY;
        }
        pState->staticInfo.staticInfoSize = OPEN_VRPC_STATIC_INFO_BYTES;
    }

    status = _openVgpuPullChunkedStatic(pGpu, pRpc, NV_VGPU_MSG_FUNCTION_GET_CONSOLIDATED_STATIC_INFO,
                                        OPEN_VRPC_STATIC_INFO_BYTES,
                                        pState->staticInfo.pStaticInfoRaw);
    if (status != NV_OK)
    {
        return status;
    }

    core = pState->staticInfo.pStaticInfoRaw + OPEN_VRPC_CORE_BLOCK_OFF;
    pState->staticInfo.usableFbBytes   = _rd64(core, 0x00U);
    pState->staticInfo.bar1Bytes       = _rd64(core, 0x08U);
    pState->staticInfo.profileBytes    = _rd64(core, 0x10U);
    pState->staticInfo.vgpuTypeId      = _rd32(core, 0x18U);
    pState->staticInfo.pciDeviceId     = _rd16(core, 0x1cU);
    pState->staticInfo.eccSupported    = core[0x1eU];
    pState->staticInfo.frlFps          = _rd32(core, 0x20U);
    pState->staticInfo.encoderCapacity = _rd32(core, 0x24U);
    pState->staticInfo.subsysId        = _rd32(core, 0x28U);
    pState->staticInfo.virtMode        = _rd32(core, 0x8d4U);
    _copyAscii(pState->staticInfo.hostDriverVer, sizeof(pState->staticInfo.hostDriverVer),
               (const char *)(core + 0x60U));
    _copyAscii(pState->staticInfo.profileName, sizeof(pState->staticInfo.profileName),
               (const char *)(core + 0x260U));

    if (pState->pGspStaticInfo != NULL)
    {
        GspStaticConfigInfo *pGsci = pState->pGspStaticInfo;
        NvU64 fbLen = (pState->staticInfo.usableFbBytes != 0ULL)
                          ? pState->staticInfo.usableFbBytes
                          : pState->virtualFbBytes;
        const char *shortName = (pState->staticInfo.profileName[0] != '\0')
                                    ? pState->staticInfo.profileName
                                    : "GRID P4-4Q";

        pGsci->fb_length    = fbLen;
        pGsci->fb_bus_width = 256U;
        pGsci->fb_ram_type  = 12U;
        pGsci->fbio_mask    = 0xffU;
        pGsci->fbp_mask     = 0x0fU;
        pGsci->l2_cache_size = 2U * 1024U * 1024U;

        pGsci->fbRegionInfoParams.numFBRegions = 1U;
        pGsci->fbRegionInfoParams.fbRegion[0].base              = 0ULL;
        pGsci->fbRegionInfoParams.fbRegion[0].limit             = (fbLen > 0ULL) ? (fbLen - 1ULL) : 0ULL;
        pGsci->fbRegionInfoParams.fbRegion[0].reserved          = 0ULL;
        pGsci->fbRegionInfoParams.fbRegion[0].performance       = 1U;
        pGsci->fbRegionInfoParams.fbRegion[0].supportCompressed = NV_TRUE;
        pGsci->fbRegionInfoParams.fbRegion[0].supportISO        = NV_TRUE;

        pGsci->bIsVgx         = NV_TRUE;
        pGsci->bIsTesla       = NV_TRUE;
        pGsci->vbiosSubVendor = 0x10deU;
        pGsci->vbiosSubDevice = pState->staticInfo.subsysId;
        pGsci->displaylessMaxHeads.numHeads            = 4U;
        pGsci->displaylessMaxHeads.maxNumHeads         = 4U;
        pGsci->displaylessMaxResolution.maxHResolution = 4096U;
        pGsci->displaylessMaxResolution.maxVResolution = 2160U;
        pGsci->displaylessMaxPixels                    = 4096ULL * 2160ULL * 4ULL;

        _copyAscii((char *)pGsci->gpuNameString, sizeof(pGsci->gpuNameString), shortName);
        _copyAscii((char *)pGsci->gpuShortNameString, sizeof(pGsci->gpuShortNameString), shortName);
    }

    NV_PRINTF(LEVEL_WARNING,
              "OPEN_VGPU: 0x50 static info synced (profile=\"%s\" host=\"%s\" fb=0x%llx)\n",
              pState->staticInfo.profileName,
              pState->staticInfo.hostDriverVer,
              (unsigned long long)pState->staticInfo.usableFbBytes);
    return NV_OK;
}

NV_STATUS openVgpuRpcGetConsolidatedGrStaticInfo(OBJGPU *pGpu, OBJRPC *pRpc)
{
    OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
    NV_STATUS status;

    if (pState == NULL || pRpc == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }

    if (pState->staticInfo.pGrStaticInfoRaw == NULL)
    {
        pState->staticInfo.pGrStaticInfoRaw = portMemAllocNonPaged(OPEN_VRPC_GR_STATIC_INFO_BYTES);
        if (pState->staticInfo.pGrStaticInfoRaw == NULL)
        {
            return NV_ERR_NO_MEMORY;
        }
        pState->staticInfo.grStaticInfoSize = OPEN_VRPC_GR_STATIC_INFO_BYTES;
    }

    status = _openVgpuPullChunkedStatic(pGpu, pRpc,
                                        NV_VGPU_MSG_FUNCTION_GET_CONSOLIDATED_GR_STATIC_INFO,
                                        OPEN_VRPC_GR_STATIC_INFO_BYTES,
                                        pState->staticInfo.pGrStaticInfoRaw);
    if (status != NV_OK)
    {
        return status;
    }

    if (pState->pGspStaticInfo != NULL)
    {
        GspStaticConfigInfo *pGsci = pState->pGspStaticInfo;
        NvU32 gpcCount = _rd32(pState->staticInfo.pGrStaticInfoRaw, 0x00d48U);
        NvU32 tpcCount = _rd32(pState->staticInfo.pGrStaticInfoRaw, 0x00d4cU);
        NvU32 cudaCores = _rd32(pState->staticInfo.pGrStaticInfoRaw, 0x00d50U);
        NvU32 i;

        if (gpcCount == 0U || gpcCount > 4U)
        {
            gpcCount = 4U;
        }
        pGsci->gpcInfo.gpcMask = (1U << gpcCount) - 1U;
        for (i = 0U; i < gpcCount && i < MAX_GPC_COUNT; i++)
        {
            pGsci->tpcInfo[i].gpcId   = i;
            pGsci->tpcInfo[i].tpcMask = 0x1fU;
        }
        pGsci->SM_info.version           = 0x61U;
        pGsci->SM_info.maxWarpsPerSM     = 64U;
        pGsci->SM_info.maxThreadsPerWarp = 32U;
        pGsci->SM_info.maxSPPerSM        = 128U;

        NV_PRINTF(LEVEL_WARNING,
                  "OPEN_VGPU: 0x9c gr static info synced (gpc=%u tpc=%u cuda=%u)\n",
                  gpcCount, tpcCount, cudaCores);
    }

    return NV_OK;
}

NV_STATUS openVgpuCreateObject(OBJGPU *pGpu)
{
    OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
    NV_STATUS status;
    NV_STATUS extStatus;
    NV_STATUS sinfoStatus;
    NV_STATUS grStatus;
    NvU32 fbPagesM1;
    NvU8 sysInfo[792];

    if (pState == NULL || !openVgpuDetectBar0VirtualMode(pGpu))
    {
        return NV_OK;
    }
    if (pState->bActive)
    {
        return NV_OK;
    }

    portMemSet(pState, 0, sizeof(*pState));
    pState->vgpuConfig     = GPU_REG_RD32(pGpu, OPEN_NV_VGPU_CONFIG);
    fbPagesM1              = GPU_REG_RD32(pGpu, OPEN_NV_VGPU_FB_PAGES_M1);
    pState->virtualFbBytes = ((NvU64)fbPagesM1 + 1ULL) << 12;
    pState->nextSeq        = 1U;

    pState->pGspStaticInfo = portMemAllocNonPaged(sizeof(GspStaticConfigInfo));
    if (pState->pGspStaticInfo == NULL)
    {
        status = NV_ERR_NO_MEMORY;
        goto fail;
    }
    portMemSet(pState->pGspStaticInfo, 0, sizeof(GspStaticConfigInfo));

    if ((status = _openVgpuAllocDmaPage(pGpu, &pState->sendRing)) != NV_OK ||
        (status = _openVgpuAllocDmaPage(pGpu, &pState->recvRing)) != NV_OK ||
        (status = _openVgpuAllocDmaPage(pGpu, &pState->eventRing)) != NV_OK ||
        (status = _openVgpuAllocDmaPage(pGpu, &pState->msgPage)) != NV_OK)
    {
        goto fail;
    }

    pState->pRpcMessageBuf = portMemAllocNonPaged(OPEN_VRPC_PAGE_BYTES);
    if (pState->pRpcMessageBuf == NULL)
    {
        status = NV_ERR_NO_MEMORY;
        goto fail;
    }

    pState->pRpc = initRpcObject(pGpu);
    if (pState->pRpc == NULL)
    {
        status = NV_ERR_NO_MEMORY;
        goto fail;
    }

    pState->pRpc->message_buffer = (NvU32 *)pState->pRpcMessageBuf;
    pState->pRpc->maxRpcSize     = OPEN_VRPC_PAGE_BYTES;
    rpcSendMessage_FNPTR(pState->pRpc) = openVgpuBar0RpcSendMessage;
    rpcRecvPoll_FNPTR(pState->pRpc)    = openVgpuBar0RpcRecvPoll;

    _openVgpuProgramRings(pGpu, pState);
    pState->bActive = NV_TRUE;

    NV_PRINTF(LEVEL_WARNING,
              "OPEN_VGPU: BAR0 rings programmed (cfg=0x%08x vfb=0x%llx)\n",
              pState->vgpuConfig, (unsigned long long)pState->virtualFbBytes);

    /* Initial VGX 0x23.0x05 handshake (0x01 + 0x40 + 0x50 + 0x9c) */
    portMemSet(sysInfo, 0, sizeof(sysInfo));
    _wr32(sysInfo, 0x00U, VGX_MAJOR_VERSION_NUMBER);
    _wr32(sysInfo, 0x04U, VGX_MINOR_VERSION_NUMBER);
    _wr32(sysInfo, 0x08U, 1U);
    _wr32(sysInfo, 0x14U, 256U);
    _copyAscii((char *)(sysInfo + 0x18U), 256U, NV_VERSION_STRING);
    _wr32(sysInfo, 0x114U, 256U);
    _copyAscii((char *)(sysInfo + 0x118U), 256U, "r535_00");
    _wr32(sysInfo, 0x214U, 256U);
    _copyAscii((char *)(sysInfo + 0x218U), 256U, "GRID");

    status = openVgpuRpcSimpleCall(pGpu, pState->pRpc,
                                   NV_VGPU_MSG_FUNCTION_SET_GUEST_SYSTEM_INFO,
                                   sysInfo, (NvU32)sizeof(sysInfo));
    NV_PRINTF(LEVEL_WARNING, "OPEN_VGPU: 0x01 SET_GUEST_SYSTEM_INFO status=0x%x\n", status);
    if (status == NV_OK)
    {
        pState->bHandshakeDone = NV_TRUE;
        extStatus = openVgpuRpcSetGuestSystemInfoExt(pGpu, pState->pRpc, "r535_00");
        sinfoStatus = openVgpuRpcGetConsolidatedStaticInfo(pGpu, pState->pRpc);
        grStatus = openVgpuRpcGetConsolidatedGrStaticInfo(pGpu, pState->pRpc);
        NV_PRINTF(LEVEL_WARNING,
                  "OPEN_VGPU: handshake ext=0x%x sinfo=0x%x gr=0x%x\n",
                  extStatus, sinfoStatus, grStatus);
    }
    return status;

fail:
    openVgpuDestructObject(pGpu);
    return status;
}

void openVgpuDestructObject(OBJGPU *pGpu)
{
    OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
    NvU8 unloadBuf[8];

    if (pState == NULL)
    {
        return;
    }

    if (pState->bActive && pState->bHandshakeDone && pState->pRpc != NULL)
    {
        portMemSet(unloadBuf, 0, sizeof(unloadBuf));
        (void)openVgpuRpcSimpleCall(pGpu, pState->pRpc,
                                    OPEN_VRPC_FN_UNLOAD_GUEST,
                                    unloadBuf, (NvU32)sizeof(unloadBuf));
    }

    if (pState->bActive)
    {
        _openVgpuTeardownRings(pGpu);
    }

    if (pState->pGspStaticInfo != NULL)
    {
        portMemFree(pState->pGspStaticInfo);
    }
    if (pState->staticInfo.pStaticInfoRaw != NULL)
    {
        portMemFree(pState->staticInfo.pStaticInfoRaw);
    }
    if (pState->staticInfo.pGrStaticInfoRaw != NULL)
    {
        portMemFree(pState->staticInfo.pGrStaticInfoRaw);
    }
    if (pState->pRpc != NULL)
    {
        rpcDestroy(pGpu, pState->pRpc);
        portMemFree(pState->pRpc);
    }
    if (pState->pRpcMessageBuf != NULL)
    {
        portMemFree(pState->pRpcMessageBuf);
    }

    _openVgpuFreeDmaPage(&pState->msgPage);
    _openVgpuFreeDmaPage(&pState->eventRing);
    _openVgpuFreeDmaPage(&pState->recvRing);
    _openVgpuFreeDmaPage(&pState->sendRing);
    portMemSet(pState, 0, sizeof(*pState));
}

OBJRPC *openVgpuGetRpc(OBJGPU *pGpu)
{
    OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
    if (pState == NULL || !pState->bActive)
    {
        return NULL;
    }
    return pState->pRpc;
}

struct _vgpu_static_info *openVgpuGetStaticInfo(OBJGPU *pGpu)
{
    OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
    if (pState == NULL || !pState->bActive)
    {
        return NULL;
    }
    return &pState->staticInfo;
}

GspStaticConfigInfo *openVgpuGetGspStaticInfo(OBJGPU *pGpu)
{
    OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
    if (pState == NULL || !pState->bActive)
    {
        return NULL;
    }
    return pState->pGspStaticInfo;
}

NV_STATUS openVgpuRmApiControl(RM_API *pRmApi, NvHandle hClient, NvHandle hObject,
                               NvU32 cmd, void *pParamStructPtr, NvU32 paramsSize)
{
    OBJGPU *pGpu;
    OBJRPC *pRpc;
    NvU32   lockMask = 0U;
    NV_STATUS status;
    rpc_message_header_v *pHdr;
    NvU8   *wire;

    if (pRmApi == NULL || pRmApi->pPrivateContext == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }
    pGpu = (OBJGPU *)pRmApi->pPrivateContext;
    pRpc = openVgpuGetRpc(pGpu);
    if (pRpc == NULL || pRpc->message_buffer == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }

    /*
     * kbusConstructEngine_IMPL() queries BAR sizes via physical RMAPI before
     * internal client/subdevice handles exist; synthesize from GPUBUSINFO + 0x50.
     */
    if (cmd == NV2080_CTRL_CMD_BUS_GET_PCI_BAR_INFO)
    {
        OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
        NV2080_CTRL_BUS_GET_PCI_BAR_INFO_PARAMS *pBarParams;
        NvU64 bar1Size;

        if (pParamStructPtr == NULL || paramsSize < sizeof(*pBarParams))
        {
            return NV_ERR_INVALID_PARAM_STRUCT;
        }
        pBarParams = (NV2080_CTRL_BUS_GET_PCI_BAR_INFO_PARAMS *)pParamStructPtr;
        portMemSet(pBarParams, 0, sizeof(*pBarParams));
        bar1Size = (pState != NULL && pState->staticInfo.bar1Bytes != 0ULL)
                       ? pState->staticInfo.bar1Bytes
                       : 0x10000000ULL;

        pBarParams->pciBarCount = 3U;
        pBarParams->pciBarInfo[0].barSizeBytes = 0x1000000ULL;
        pBarParams->pciBarInfo[0].barSize      = 16U;
        pBarParams->pciBarInfo[0].barOffset    = (NvU64)pGpu->busInfo.gpuPhysAddr;

        pBarParams->pciBarInfo[1].barSizeBytes = bar1Size;
        pBarParams->pciBarInfo[1].barSize      = (NvU32)(bar1Size >> 20);
        pBarParams->pciBarInfo[1].barOffset    = (NvU64)pGpu->busInfo.gpuPhysFbAddr;

        pBarParams->pciBarInfo[2].barSizeBytes = 0x2000000ULL;
        pBarParams->pciBarInfo[2].barSize      = 32U;
        pBarParams->pciBarInfo[2].barOffset    = (NvU64)pGpu->busInfo.gpuPhysInstAddr;
        return NV_OK;
    }

    /* GSP-only _INTERNAL_ controls absent on legacy 535.309.01 host libnvidia-vgpu.so */
    if (cmd == NV2080_CTRL_CMD_INTERNAL_GPU_GET_CHIP_INFO)
    {
        OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
        NV2080_CTRL_INTERNAL_GPU_GET_CHIP_INFO_PARAMS *pChipParams;
        NvU32 i;

        if (pParamStructPtr == NULL || paramsSize < sizeof(*pChipParams))
        {
            return NV_ERR_INVALID_PARAM_STRUCT;
        }
        pChipParams = (NV2080_CTRL_INTERNAL_GPU_GET_CHIP_INFO_PARAMS *)pParamStructPtr;
        portMemSet(pChipParams, 0, sizeof(*pChipParams));
        pChipParams->bar1Size = (pState != NULL && pState->staticInfo.bar1Bytes != 0ULL)
                                    ? (NvU32)(pState->staticInfo.bar1Bytes >> 20)
                                    : 256U;
        pChipParams->pciDeviceId    = pGpu->idInfo.PCIDeviceID ? pGpu->idInfo.PCIDeviceID : 0x1bb310deU;
        pChipParams->pciSubDeviceId = pGpu->idInfo.PCISubDeviceID ? pGpu->idInfo.PCISubDeviceID : 0x120610deU;
        pChipParams->pciRevisionId  = pGpu->idInfo.PCIRevisionID ? pGpu->idInfo.PCIRevisionID : 0xa1U;
        for (i = 0U; i < NV2080_CTRL_INTERNAL_GET_CHIP_INFO_REG_BASE_MAX; i++)
        {
            pChipParams->regBases[i] = 0xffffffffU;
        }
        pChipParams->regBases[NV_REG_BASE_GR]       = 0x00400000U;
        pChipParams->regBases[NV_REG_BASE_TIMER]    = 0x00009000U;
        pChipParams->regBases[NV_REG_BASE_MASTER]   = 0x00000000U;
        pChipParams->regBases[NV_REG_BASE_USERMODE] = 0x00810000U;
        return NV_OK;
    }

    if (cmd == NV2080_CTRL_CMD_INTERNAL_GPU_GET_USER_REGISTER_ACCESS_MAP ||
        cmd == NV2080_CTRL_CMD_INTERNAL_GET_CONSTRUCTED_FALCON_INFO ||
        cmd == NV2080_CTRL_CMD_INTERNAL_GMMU_GET_STATIC_INFO ||
        cmd == 0x20800a80U ||
        cmd == 0x20802609U ||
        cmd == 0x2080302cU)
    {
        if (pParamStructPtr == NULL || paramsSize == 0U)
        {
            return NV_ERR_INVALID_PARAM_STRUCT;
        }
        portMemSet(pParamStructPtr, 0, paramsSize);
        return NV_OK;
    }

    if (cmd == NV2080_CTRL_CMD_INTERNAL_BIF_GET_STATIC_INFO)
    {
        NV2080_CTRL_INTERNAL_BIF_GET_STATIC_INFO_PARAMS *pBifParams;

        if (pParamStructPtr == NULL || paramsSize < sizeof(*pBifParams))
        {
            return NV_ERR_INVALID_PARAM_STRUCT;
        }
        pBifParams = (NV2080_CTRL_INTERNAL_BIF_GET_STATIC_INFO_PARAMS *)pParamStructPtr;
        portMemSet(pBifParams, 0, sizeof(*pBifParams));
        pBifParams->bPcieGen4Capable      = NV_FALSE;
        pBifParams->bIsC2CLinkUp          = NV_FALSE;
        pBifParams->dmaWindowStartAddress = 0ULL;
        return NV_OK;
    }

    if (cmd == NV2080_CTRL_CMD_INTERNAL_INTR_GET_KERNEL_TABLE)
    {
        NV2080_CTRL_INTERNAL_INTR_GET_KERNEL_TABLE_PARAMS *pIntrParams;
        NvU32 i;

        if (pParamStructPtr == NULL || paramsSize < sizeof(*pIntrParams))
        {
            return NV_ERR_INVALID_PARAM_STRUCT;
        }
        pIntrParams = (NV2080_CTRL_INTERNAL_INTR_GET_KERNEL_TABLE_PARAMS *)pParamStructPtr;
        portMemSet(pIntrParams, 0, sizeof(*pIntrParams));
        pIntrParams->tableLen                 = 4U;
        pIntrParams->table[0].engineIdx       = MC_ENGINE_IDX_VGPU;
        pIntrParams->table[0].pmcIntrMask     = 0U;
        pIntrParams->table[0].vectorStall     = NV_U32_MAX;
        pIntrParams->table[0].vectorNonStall  = NV_U32_MAX;
        pIntrParams->table[1].engineIdx       = MC_ENGINE_IDX_NON_REPLAYABLE_FAULT;
        pIntrParams->table[1].pmcIntrMask     = 0U;
        pIntrParams->table[1].vectorStall     = 64U;
        pIntrParams->table[1].vectorNonStall  = NV_U32_MAX;
        pIntrParams->table[2].engineIdx       = MC_ENGINE_IDX_ACCESS_CNTR;
        pIntrParams->table[2].pmcIntrMask     = 0U;
        pIntrParams->table[2].vectorStall     = 128U;
        pIntrParams->table[2].vectorNonStall  = NV_U32_MAX;
        pIntrParams->table[3].engineIdx       = MC_ENGINE_IDX_REPLAYABLE_FAULT;
        pIntrParams->table[3].pmcIntrMask     = 0U;
        pIntrParams->table[3].vectorStall     = 129U;
        pIntrParams->table[3].vectorNonStall  = NV_U32_MAX;
        for (i = 0U; i < NV2080_INTR_CATEGORY_ENUM_COUNT; i++)
        {
            pIntrParams->subtreeMap[i].subtreeStart = 0U;
            pIntrParams->subtreeMap[i].subtreeEnd   = 0U;
        }
        pIntrParams->subtreeMap[NV2080_INTR_CATEGORY_UVM_SHARED].subtreeStart = 1U;
        pIntrParams->subtreeMap[NV2080_INTR_CATEGORY_UVM_SHARED].subtreeEnd   = 1U;
        pIntrParams->subtreeMap[NV2080_INTR_CATEGORY_UVM_OWNED].subtreeStart  = 2U;
        pIntrParams->subtreeMap[NV2080_INTR_CATEGORY_UVM_OWNED].subtreeEnd    = 2U;
        return NV_OK;
    }

    if (cmd == NV2080_CTRL_CMD_INTERNAL_MEMSYS_GET_STATIC_CONFIG)
    {
        NV2080_CTRL_INTERNAL_MEMSYS_GET_STATIC_CONFIG_PARAMS *pMemSysParams;

        if (pParamStructPtr == NULL || paramsSize < sizeof(*pMemSysParams))
        {
            return NV_ERR_INVALID_PARAM_STRUCT;
        }
        pMemSysParams = (NV2080_CTRL_INTERNAL_MEMSYS_GET_STATIC_CONFIG_PARAMS *)pParamStructPtr;
        portMemSet(pMemSysParams, 0, sizeof(*pMemSysParams));
        pMemSysParams->bOneToOneComptagLineAllocation = NV_TRUE;
        pMemSysParams->bDisableCompbitBacking         = NV_TRUE;
        pMemSysParams->bFbpaPresent                   = NV_TRUE;
        pMemSysParams->l2CacheSize                    = 0x200000ULL;
        pMemSysParams->comprPageSize                  = 0x10000U;
        pMemSysParams->comprPageShift                 = 16U;
        pMemSysParams->ramType                        = NV2080_CTRL_FB_INFO_RAM_TYPE_GDDR5;
        pMemSysParams->ltcCount                       = 8U;
        pMemSysParams->ltsPerLtcCount                 = 4U;
        return NV_OK;
    }

    if (cmd == NV2080_CTRL_CMD_INTERNAL_MEMMGR_GET_VGPU_CONFIG_HOST_RESERVED_FB)
    {
        NV2080_CTRL_INTERNAL_MEMMGR_GET_VGPU_CONFIG_HOST_RESERVED_FB_PARAMS *pFbRes;

        if (pParamStructPtr == NULL || paramsSize < sizeof(*pFbRes))
        {
            return NV_ERR_INVALID_PARAM_STRUCT;
        }
        pFbRes = (NV2080_CTRL_INTERNAL_MEMMGR_GET_VGPU_CONFIG_HOST_RESERVED_FB_PARAMS *)pParamStructPtr;
        pFbRes->hostReservedFb = 0ULL;
        return NV_OK;
    }

    if (cmd == NV2080_CTRL_CMD_INTERNAL_FIFO_GET_NUM_CHANNELS)
    {
        NV2080_CTRL_INTERNAL_FIFO_GET_NUM_CHANNELS_PARAMS *pChans;

        if (pParamStructPtr == NULL || paramsSize < sizeof(*pChans))
        {
            return NV_ERR_INVALID_PARAM_STRUCT;
        }
        pChans = (NV2080_CTRL_INTERNAL_FIFO_GET_NUM_CHANNELS_PARAMS *)pParamStructPtr;
        pChans->numChannels = 4096U;
        return NV_OK;
    }

    if (paramsSize > (OPEN_VRPC_PAGE_BYTES - 0x40U))
    {
        NV_PRINTF(LEVEL_ERROR,
                  "OPEN_VGPU: Control 0x%08x paramsSize=0x%x exceeds single-page VRPC\n",
                  cmd, paramsSize);
        return NV_ERR_BUFFER_TOO_SMALL;
    }

    status = _openVgpuAcquireRpcLock(pGpu, &lockMask);
    if (status != NV_OK)
    {
        return status;
    }

    /* Host 0x1a (_nv005240vgpu) requires header->length == 0x40; params follow at +0x40 */
    status = rpcWriteCommonHeader(pGpu, pRpc, OPEN_VRPC_FN_RM_API_CONTROL, 0x20U);
    if (status == NV_OK)
    {
        wire = (NvU8 *)pRpc->message_buffer + sizeof(rpc_message_header_v);
        _wr32(wire, 0x00U, hClient);
        _wr32(wire, 0x04U, hObject);
        _wr32(wire, 0x08U, cmd);
        _wr32(wire, 0x18U, paramsSize);
        if (paramsSize > 0U && pParamStructPtr != NULL)
        {
            portMemCopy((NvU8 *)pRpc->message_buffer + 0x40U, paramsSize, pParamStructPtr, paramsSize);
        }

        status = rpcSendMessage(pGpu, pRpc);
        if (status == NV_OK)
        {
            status = rpcRecvPoll(pGpu, pRpc, OPEN_VRPC_FN_RM_API_CONTROL);
            if (status == NV_OK)
            {
                pHdr = (rpc_message_header_v *)pRpc->message_buffer;
                status = pHdr->rpc_result;
                if (status == NV_OK && paramsSize > 0U && pParamStructPtr != NULL)
                {
                    if (cmd == NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE &&
                        paramsSize >= sizeof(NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS))
                    {
                        /*
                         * Host libnvidia-vgpu.so (0x1dfa80.c LAB_001e00f3) packs
                         * 0x20801112 as 32 x 23-dword entries (omitting pbdmaFaultIds[2])
                         * followed by numEntries at wire32[0x2e1] and bMore at wire32[0x2e2].
                         */
                        const NvU32 *w32 = (const NvU32 *)((const NvU8 *)pRpc->message_buffer + 0x40U);
                        NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS *pDevTab =
                            (NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS *)pParamStructPtr;
                        NvU32 idx, d;

                        portMemSet(pDevTab, 0, sizeof(*pDevTab));
                        pDevTab->baseIndex  = w32[0];
                        pDevTab->numEntries = w32[0x2e1U];
                        pDevTab->bMore      = ((const NvU8 *)&w32[0x2e2U])[0] ? NV_TRUE : NV_FALSE;
                        for (idx = 0U; idx < NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_MAX_ENTRIES; idx++)
                        {
                            const NvU32 *ent = &w32[idx * 0x17U];
                            for (d = 0U; d < NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_ENGINE_DATA_TYPES; d++)
                            {
                                pDevTab->entries[idx].engineData[d] = ent[1U + d];
                            }
                            pDevTab->entries[idx].pbdmaIds[0]      = ent[0x11U];
                            pDevTab->entries[idx].pbdmaIds[1]      = ent[0x12U];
                            pDevTab->entries[idx].pbdmaFaultIds[0] = ent[0x11U];
                            pDevTab->entries[idx].pbdmaFaultIds[1] = ent[0x12U];
                            pDevTab->entries[idx].numPbdmas        = ent[0x13U];
                            portMemCopy(pDevTab->entries[idx].engineName,
                                        NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_ENGINE_MAX_NAME_LEN,
                                        (const NvU8 *)ent + 0x50U,
                                        NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_ENGINE_MAX_NAME_LEN);
                        }
                        if (pDevTab->numEntries == 0U ||
                            pDevTab->numEntries > NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_MAX_ENTRIES)
                        {
                            pDevTab->numEntries = 1U;
                            pDevTab->bMore      = NV_FALSE;
                            pDevTab->entries[0].numPbdmas        = 1U;
                            pDevTab->entries[0].pbdmaIds[0]      = 0U;
                            pDevTab->entries[0].pbdmaFaultIds[0] = 0U;
                        }
                    }
                    else
                    {
                        portMemCopy(pParamStructPtr, paramsSize, (NvU8 *)pRpc->message_buffer + 0x40U, paramsSize);
                    }
                }
                if (status != NV_OK)
                {
                    NV_PRINTF(LEVEL_ERROR,
                              "OPEN_VGPU: Control(0x%08x, hC=0x%x, hO=0x%x, sz=0x%x) -> 0x%08x\n",
                              cmd, hClient, hObject, paramsSize, status);
                }
            }
        }
    }

    _openVgpuReleaseRpcLock(lockMask);
    return status;
}

NV_STATUS openVgpuRmApiAlloc(RM_API *pRmApi, NvHandle hClient, NvHandle hParent,
                             NvHandle hObject, NvU32 hClass, void *pAllocParams, NvU32 allocParamsSize)
{
    OBJGPU *pGpu;
    OBJRPC *pRpc;
    OPEN_VGPU_BAR0_STATE *pState;
    NvHandle trackedClient = hClient;
    NvHandle trackedParent = hParent;
    NvHandle trackedObj    = hObject;
    NvU32   lockMask = 0U;
    NV_STATUS status;

    if (pRmApi == NULL || pRmApi->pPrivateContext == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }
    pGpu = (OBJGPU *)pRmApi->pPrivateContext;
    pState = _openVgpuGetState(pGpu);
    pRpc = openVgpuGetRpc(pGpu);
    if (pRpc == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }

    status = _openVgpuAcquireRpcLock(pGpu, &lockMask);
    if (status != NV_OK)
    {
        return status;
    }

    if (hClass == NV01_ROOT || hClass == NV01_ROOT_NON_PRIV || hClass == NV01_ROOT_CLIENT)
    {
        NvU8 p[108];
        portMemSet(p, 0, sizeof(p));
        trackedClient = hClient ? hClient : hObject;
        trackedObj    = trackedClient;
        trackedParent = 0U;
        _wr32(p, 0x00U, trackedClient);
        status = openVgpuRpcSimpleCall(pGpu, pRpc, OPEN_VRPC_FN_ALLOC_ROOT, p, (NvU32)sizeof(p));
    }
    else if (hClass == NV01_DEVICE_0)
    {
        NvU8 p[64];
        portMemSet(p, 0, sizeof(p));
        trackedParent = hParent ? hParent : hClient;
        _wr32(p, 0x00U, hClient);
        _wr32(p, 0x04U, hObject);
        _wr32(p, 0x08U, hClass);
        status = openVgpuRpcSimpleCall(pGpu, pRpc, OPEN_VRPC_FN_ALLOC_SHARE_DEVICE, p, (NvU32)sizeof(p));
    }
    else if (hClass == NV20_SUBDEVICE_0)
    {
        NvU8 p[40];
        NvU32 subInst = 0U;
        if (pAllocParams != NULL && allocParamsSize >= sizeof(NvU32))
        {
            subInst = *(const NvU32 *)pAllocParams;
        }
        portMemSet(p, 0, sizeof(p));
        _wr32(p, 0x00U, subInst);
        _wr32(p, 0x08U, hClient);
        _wr32(p, 0x0cU, hParent);
        _wr32(p, 0x10U, hObject);
        _wr32(p, 0x14U, hClass);
        status = openVgpuRpcSimpleCall(pGpu, pRpc, OPEN_VRPC_FN_ALLOC_SUBDEVICE, p, (NvU32)sizeof(p));
    }
    else if (_openVgpuIsChannelClass(hClass))
    {
        NvU8 p[248];
        portMemSet(p, 0, sizeof(p));
        _wr32(p, 0x00U, hClient);
        _wr32(p, 0x04U, hParent);
        _wr32(p, 0x08U, hObject);
        _wr32(p, 0x0cU, hClass);
        if (pAllocParams != NULL && allocParamsSize >= 16U)
        {
            NvU32 copyLen = (allocParamsSize > 200U) ? 200U : allocParamsSize;
            portMemCopy(p + 0x10U, copyLen, pAllocParams, copyLen);
        }
        status = openVgpuRpcSimpleCall(pGpu, pRpc, OPEN_VRPC_FN_ALLOC_CHANNEL_DMA, p, (NvU32)sizeof(p));
    }
    else
    {
        NvU32 copyLen = (pAllocParams != NULL) ? allocParamsSize : 0U;
        NvU32 wireLen = 0x18U + copyLen;
        if (wireLen < 0x50U)
        {
            wireLen = 0x50U;
        }
        if (wireLen > OPEN_VRPC_MAX_PAYLOAD)
        {
            _openVgpuReleaseRpcLock(lockMask);
            return NV_ERR_BUFFER_TOO_SMALL;
        }
        NvU8 *p = portMemAllocNonPaged(wireLen);
        if (p == NULL)
        {
            _openVgpuReleaseRpcLock(lockMask);
            return NV_ERR_NO_MEMORY;
        }
        portMemSet(p, 0, wireLen);
        _wr32(p, 0x00U, hClient);
        _wr32(p, 0x04U, hParent);
        _wr32(p, 0x08U, hObject);
        _wr32(p, 0x0cU, hClass);
        _wr32(p, 0x10U, copyLen);
        if (copyLen > 0U && pAllocParams != NULL)
        {
            portMemCopy(p + 0x18U, copyLen, pAllocParams, copyLen);
        }
        status = openVgpuRpcSimpleCall(pGpu, pRpc, OPEN_VRPC_FN_ALLOC_OBJECT, p, wireLen);
        if (status == NV_OK && copyLen > 0U && pAllocParams != NULL)
        {
            portMemCopy(pAllocParams, copyLen, p + 0x18U, copyLen);
        }
        portMemFree(p);
    }

    if (status == NV_OK)
    {
        _openVgpuTrackHandle(pState, trackedClient, trackedObj, trackedParent);
    }

    _openVgpuReleaseRpcLock(lockMask);
    return status;
}

NV_STATUS openVgpuRmApiFree(RM_API *pRmApi, NvHandle hClient, NvHandle hObject)
{
    OBJGPU *pGpu;
    OBJRPC *pRpc;
    OPEN_VGPU_BAR0_STATE *pState;
    NvHandle hParent;
    NvU8    p[16];
    NvU32   lockMask = 0U;
    NV_STATUS status;

    if (pRmApi == NULL || pRmApi->pPrivateContext == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }
    pGpu = (OBJGPU *)pRmApi->pPrivateContext;
    pState = _openVgpuGetState(pGpu);
    pRpc = openVgpuGetRpc(pGpu);
    if (pRpc == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }

    hParent = _openVgpuLookupAndUntrackParent(pState, hClient, hObject);
    portMemSet(p, 0, sizeof(p));
    _wr32(p, 0x00U, hClient);
    _wr32(p, 0x04U, hParent);
    _wr32(p, 0x08U, hObject);

    status = _openVgpuAcquireRpcLock(pGpu, &lockMask);
    if (status != NV_OK)
    {
        return status;
    }
    status = openVgpuRpcSimpleCall(pGpu, pRpc, OPEN_VRPC_FN_FREE, p, (NvU32)sizeof(p));
    _openVgpuReleaseRpcLock(lockMask);
    return status;
}

NV_STATUS openVgpuRmApiDupObject(RM_API *pRmApi, NvHandle hClient, NvHandle hParent,
                                 NvHandle *phObject, NvHandle hClientSrc, NvHandle hObjectSrc,
                                 NvU32 flags)
{
    OBJGPU *pGpu;
    OBJRPC *pRpc;
    OPEN_VGPU_BAR0_STATE *pState;
    NvU8    p[28];
    NvU32   lockMask = 0U;
    NV_STATUS status;

    if (pRmApi == NULL || pRmApi->pPrivateContext == NULL || phObject == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }
    pGpu = (OBJGPU *)pRmApi->pPrivateContext;
    pState = _openVgpuGetState(pGpu);
    pRpc = openVgpuGetRpc(pGpu);
    if (pRpc == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }

    portMemSet(p, 0, sizeof(p));
    _wr32(p, 0x00U, hClient);
    _wr32(p, 0x04U, hParent);
    _wr32(p, 0x08U, *phObject);
    _wr32(p, 0x0cU, hClientSrc);
    _wr32(p, 0x10U, hObjectSrc);
    _wr32(p, 0x14U, flags);

    status = _openVgpuAcquireRpcLock(pGpu, &lockMask);
    if (status != NV_OK)
    {
        return status;
    }
    status = openVgpuRpcSimpleCall(pGpu, pRpc, OPEN_VRPC_FN_DUP_OBJECT, p, (NvU32)sizeof(p));
    _openVgpuReleaseRpcLock(lockMask);
    if (status == NV_OK)
    {
        _openVgpuTrackHandle(pState, hClient, *phObject, hParent);
    }
    return status;
}

NV_STATUS openVgpuRpcAllocLocalUser(OBJGPU *pGpu, NvHandle hClient, NvHandle hParent,
                                    NvHandle hMemory, MEMORY_DESCRIPTOR *pMemDesc,
                                    NvU64 length, NvU32 attr, NvU32 attr2, NvU32 format)
{
    OBJRPC *pRpc = openVgpuGetRpc(pGpu);
    NvU32   pageCount;
    NvU32   wireLen;
    NvU8   *p;
    NvU32   i;
    NV_STATUS status;

    (void)attr2;
    (void)format;

    if (pRpc == NULL || pMemDesc == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }
    pageCount = (NvU32)pMemDesc->PageCount;
    if (0x38U + pageCount * 8U > OPEN_VRPC_MAX_PAYLOAD)
    {
        return NV_ERR_BUFFER_TOO_SMALL;
    }

    wireLen = 0x38U + pageCount * 8U;
    p = portMemAllocNonPaged(wireLen);
    if (p == NULL)
    {
        return NV_ERR_NO_MEMORY;
    }
    portMemSet(p, 0, wireLen);
    _wr32(p, 0x00U, hClient);
    _wr32(p, 0x04U, hParent);
    _wr32(p, 0x08U, hMemory);
    _wr32(p, 0x0cU, NV01_MEMORY_LIST_SYSTEM);
    _wr32(p, 0x14U, pMemDesc->PteAdjust);
    _wr32(p, 0x18U, attr);
    _wr64(p, 0x20U, length);
    _wr32(p, 0x28U, pageCount);
    if (pageCount > 0U)
    {
        _wr64(p, 0x30U, ((NvU64)memdescGetPhysAddr(pMemDesc, AT_GPU, 0)) >> 12);
        for (i = 0U; i < pageCount; i++)
        {
            NvU64 gpfn = ((NvU64)memdescGetPhysAddr(pMemDesc, AT_GPU, (NvU64)i * RM_PAGE_SIZE)) >> 12;
            _wr64(p, 0x38U + i * 8U, gpfn);
        }
    }

    status = openVgpuRpcSimpleCall(pGpu, pRpc, OPEN_VRPC_FN_ALLOC_MEMORY, p, wireLen);
    portMemFree(p);
    return status;
}

NV_STATUS openVgpuRpcAllocVidmem(OBJGPU *pGpu, NvHandle hClient, NvHandle hParent,
                                 NvHandle hMemory, MEMORY_DESCRIPTOR *pMemDesc,
                                 NvU64 size, NvU32 attr, NvU32 attr2, NvU32 type,
                                 NvU32 flags, NvU32 format, NvU32 comprCovg)
{
    OBJRPC *pRpc = openVgpuGetRpc(pGpu);
    NvU8    p[96];
    NvU64   fbPfn = 0ULL;

    (void)attr2;
    (void)type;

    if (pRpc == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }
    if (pMemDesc != NULL)
    {
        fbPfn = ((NvU64)memdescGetPhysAddr(pMemDesc, AT_GPU, 0)) >> 12;
    }

    portMemSet(p, 0, sizeof(p));
    _wr32(p, 0x00U, hClient);
    _wr32(p, 0x04U, hParent);
    _wr32(p, 0x08U, hMemory);
    _wr32(p, 0x0cU, NV01_MEMORY_LIST_FBMEM);
    _wr32(p, 0x14U, attr);
    _wr32(p, 0x1cU, flags);
    _wr64(p, 0x20U, size);
    _wr32(p, 0x28U, 1U);
    _wr32(p, 0x40U, format);
    _wr32(p, 0x48U, comprCovg);
    _wr64(p, 0x58U, fbPfn);

    return openVgpuRpcSimpleCall(pGpu, pRpc, OPEN_VRPC_FN_ALLOC_VIDMEM, p, (NvU32)sizeof(p));
}

NV_STATUS openVgpuRpcAllocVirtmem(OBJGPU *pGpu, NvHandle hClient, NvHandle hParent,
                                  NvHandle hVASpace, NvHandle hMemory, NvU64 *pOffset,
                                  NvU64 size, NvU32 type, NvU32 flags, NvU64 alignment)
{
    OBJRPC *pRpc = openVgpuGetRpc(pGpu);
    NvU8    p[96];
    NV_STATUS status;

    if (pRpc == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }

    portMemSet(p, 0, sizeof(p));
    _wr32(p, 0x00U, hClient);
    _wr32(p, 0x04U, hParent);
    _wr32(p, 0x08U, hVASpace);
    _wr32(p, 0x0cU, hMemory);
    _wr32(p, 0x10U, type ? type : NV50_MEMORY_VIRTUAL);
    _wr32(p, 0x14U, flags | 0x00080000U);
    _wr64(p, 0x30U, size);
    _wr64(p, 0x38U, alignment);
    if (pOffset != NULL)
    {
        _wr64(p, 0x40U, *pOffset);
    }

    status = openVgpuRpcSimpleCall(pGpu, pRpc, OPEN_VRPC_FN_ALLOC_VIRTMEM, p, (NvU32)sizeof(p));
    if (status == NV_OK && pOffset != NULL)
    {
        *pOffset = _rd64(p, 0x40U);
    }
    return status;
}

NV_STATUS openVgpuRpcMapMemory(OBJGPU *pGpu, NvHandle hClient, NvHandle hDevice,
                               NvHandle hMemory, NvU64 offset, NvU64 length,
                               NvU32 flags, NvU64 *pApertureOffset)
{
    OBJRPC *pRpc = openVgpuGetRpc(pGpu);
    NvU8    p[56];
    NV_STATUS status;

    if (pRpc == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }

    portMemSet(p, 0, sizeof(p));
    _wr32(p, 0x00U, hClient);
    _wr32(p, 0x04U, hDevice);
    _wr32(p, 0x08U, hMemory);
    _wr32(p, 0x0cU, flags);
    _wr64(p, 0x10U, offset);
    _wr64(p, 0x18U, length);

    status = openVgpuRpcSimpleCall(pGpu, pRpc, OPEN_VRPC_FN_MAP_MEMORY, p, (NvU32)sizeof(p));
    if (status == NV_OK && pApertureOffset != NULL)
    {
        *pApertureOffset = pGpu->busInfo.gpuPhysFbAddr + _rd64(p, 0x20U);
    }
    return status;
}

NV_STATUS openVgpuRpcUnmapMemory(OBJGPU *pGpu, NvHandle hClient, NvHandle hDevice,
                                 NvHandle hMemory, NvU32 flags, NvU64 apertureOffset)
{
    OBJRPC *pRpc = openVgpuGetRpc(pGpu);
    NvU8    p[48];

    if (pRpc == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }

    portMemSet(p, 0, sizeof(p));
    _wr32(p, 0x00U, hClient);
    _wr32(p, 0x04U, hDevice);
    _wr32(p, 0x08U, hMemory);
    _wr32(p, 0x0cU, flags);
    _wr64(p, 0x10U, apertureOffset);

    return openVgpuRpcSimpleCall(pGpu, pRpc, OPEN_VRPC_FN_UNMAP_MEMORY, p, (NvU32)sizeof(p));
}

NV_STATUS openVgpuRpcUpdatePde2(OBJGPU *pGpu, NvHandle hClient, NvHandle hDevice,
                                void *pUpdatePde2Params, NvU32 paramsSize)
{
    OBJRPC *pRpc = openVgpuGetRpc(pGpu);
    NvU8    p[64];
    NvU32   copyLen;

    if (pRpc == NULL || pUpdatePde2Params == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }

    portMemSet(p, 0, sizeof(p));
    _wr32(p, 0x00U, hClient);
    _wr32(p, 0x04U, hDevice);
    copyLen = (paramsSize > 0x38U) ? 0x38U : paramsSize;
    if (copyLen > 0U)
    {
        portMemCopy(p + 0x08U, copyLen, pUpdatePde2Params, copyLen);
    }

    return openVgpuRpcSimpleCall(pGpu, pRpc, OPEN_VRPC_FN_UPDATE_PDE_2, p, (NvU32)sizeof(p));
}

NV_STATUS openVgpuRpcAllocContextDma(OBJGPU *pGpu, NvHandle hClient, NvHandle hDevice,
                                    NvHandle hDma, NvU32 hClass, NvU32 flags,
                                    NvHandle hMemory, NvU64 offset, NvU64 limit)
{
    OBJRPC *pRpc = openVgpuGetRpc(pGpu);
    NvU8    p[56];

    if (pRpc == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }

    portMemSet(p, 0, sizeof(p));
    _wr32(p, 0x00U, hDevice);
    _wr32(p, 0x04U, hClient);
    _wr32(p, 0x08U, hDma);
    _wr32(p, 0x0cU, hClass);
    _wr32(p, 0x10U, flags);
    _wr32(p, 0x18U, hMemory);
    _wr64(p, 0x20U, offset);
    _wr64(p, 0x28U, limit);

    return openVgpuRpcSimpleCall(pGpu, pRpc, OPEN_VRPC_FN_ALLOC_CTX_DMA, p, (NvU32)sizeof(p));
}

NvBool vgpuGetPendingEvent(OBJGPU *pGpu, THREAD_STATE_NODE *pThreadState)
{
    OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
    NvU32 put;
    NvU32 get;
    NvU32 intr;

    (void)pThreadState;

    if (pState == NULL || !pState->bActive)
    {
        return NV_FALSE;
    }

    put  = GPU_REG_RD32(pGpu, OPEN_NV_VGPU_EVENT_RING_PUT);
    get  = GPU_REG_RD32(pGpu, OPEN_NV_VGPU_EVENT_RING_GET);
    intr = GPU_REG_RD32(pGpu, OPEN_NV_VGPU_INTR_STATUS);
    if (put == 0xffffffffU || get == 0xffffffffU)
    {
        return NV_FALSE;
    }
    if ((put & 0xff8U) != (get & 0xff8U))
    {
        return NV_TRUE;
    }
    if (intr != 0U && intr != 0xffffffffU)
    {
        return NV_TRUE;
    }
    return NV_FALSE;
}

void vgpuService(OBJGPU *pGpu)
{
    OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
    NvU32 put;
    NvU32 get;
    NvU32 steps = 0U;

    if (pState == NULL || !pState->bActive)
    {
        return;
    }

    put = GPU_REG_RD32(pGpu, OPEN_NV_VGPU_EVENT_RING_PUT);
    get = GPU_REG_RD32(pGpu, OPEN_NV_VGPU_EVENT_RING_GET);
    if (put == 0xffffffffU || get == 0xffffffffU)
    {
        return;
    }

    put &= 0xff8U;
    get &= 0xff8U;
    while (get != put && steps < 512U)
    {
        portAtomicMemoryFenceFull();
        get = (get + 8U) & 0xff8U;
        GPU_REG_WR32(pGpu, OPEN_NV_VGPU_EVENT_RING_GET, get);
        steps++;
    }
}

NV_STATUS vgpuShmIsSwPending(OBJGPU *pGpu, NvU32 *isSwPending)
{
    OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
    NvU32 intr;

    if (isSwPending == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *isSwPending = 0U;
    if (pState == NULL || !pState->bActive)
    {
        return NV_OK;
    }

    intr = GPU_REG_RD32(pGpu, OPEN_NV_VGPU_INTR_STATUS);
    if (intr != 0xffffffffU && (intr & 1U) != 0U)
    {
        *isSwPending = 1U;
    }
    return NV_OK;
}

NV_STATUS vgpuShmIsNonStallEnabled(OBJGPU *pGpu, NvU32 *isNonStallEnabled)
{
    OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
    NvU32 en;

    if (isNonStallEnabled == NULL)
    {
        return NV_ERR_INVALID_ARGUMENT;
    }

    *isNonStallEnabled = 0U;
    if (pState == NULL || !pState->bActive)
    {
        return NV_OK;
    }

    en = GPU_REG_RD32(pGpu, OPEN_NV_VGPU_INTR_ENABLE);
    if (en != 0U && en != 0xffffffffU)
    {
        *isNonStallEnabled = 1U;
    }
    return NV_OK;
}

NV_STATUS vgpuIsNonStallPending(OBJGPU *pGpu, PMC_ENGINE_BITVECTOR pEngines)
{
    OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
    NvU32 intr;

    if (pEngines != NULL)
    {
        bitVectorClrAll(pEngines);
    }

    if (pState == NULL || !pState->bActive || pEngines == NULL)
    {
        return NV_OK;
    }

    intr = GPU_REG_RD32(pGpu, OPEN_NV_VGPU_INTR_STATUS);
    if (intr != 0U && intr != 0xffffffffU)
    {
        bitVectorSet(pEngines, MC_ENGINE_IDX_VGPU);
    }
    return NV_OK;
}

NV_STATUS vgpuServiceNonStall(OBJGPU *pGpu, PMC_ENGINE_BITVECTOR pEngines)
{
    (void)pEngines;
    vgpuService(pGpu);
    return NV_OK;
}
