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
#include "os/os.h"
#include "gpu/gpu.h"
#include "gpu/mem_mgr/mem_desc.h"
#include "vgpu/rpc.h"
#include "vgpu/vgpu_version.h"
#include "vgpu/vgpu_events.h"
#include "vgpu/open_vgpu_bar0_rpc.h"
#include "nvport/nvport.h"

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

static NvU32 _openVgpuPteLo(NvU64 dmaAddr)
{
    NvU32 pfnLo = (NvU32)((dmaAddr >> 12) & 0x000fffffULL);
    return (pfnLo << 12) | 0x0000000bU;
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

static void _wr32(NvU8 *p, NvU32 off, NvU32 v)
{
    portMemCopy(p + off, sizeof(v), &v, sizeof(v));
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
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_GUEST_ID_58, 0x0aU);

    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_SEND_RING_LO, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_SEND_RING_PUT, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_SEND_RING_HI, _openVgpuPteHi(pState->sendRing.dmaAddr));
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_SEND_RING_LO, _openVgpuPteLo(pState->sendRing.dmaAddr));

    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_RECV_RING_LO, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_RECV_RING_GET, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_RECV_RING_HI, _openVgpuPteHi(pState->recvRing.dmaAddr));
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_RECV_RING_LO, _openVgpuPteLo(pState->recvRing.dmaAddr));

    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_EVENT_RING_LO, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_EVENT_RING_GET, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_EVENT_RING_HI, _openVgpuPteHi(pState->eventRing.dmaAddr));
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_EVENT_RING_LO, _openVgpuPteLo(pState->eventRing.dmaAddr));

    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_INTR_ENABLE, 0x1ffU);
}

static void _openVgpuTeardownRings(OBJGPU *pGpu)
{
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_INTR_ENABLE, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_SEND_RING_LO, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_RECV_RING_LO, 0U);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_EVENT_RING_LO, 0U);
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
    NvU32 slotOff;
    NvU32 *pSlot;

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

    portMemSet(pState->msgPage.pCpuAddr, 0, OPEN_VRPC_PAGE_BYTES);
    portMemCopy(pState->msgPage.pCpuAddr, totalLen, pRpc->message_buffer, totalLen);
    portAtomicMemoryFenceFull();

    slotOff = pState->sendPut & 0xff8U;
    pSlot = (NvU32 *)((NvU8 *)pState->sendRing.pCpuAddr + slotOff);
    pSlot[0] = _openVgpuPteLo(pState->msgPage.dmaAddr);
    pSlot[1] = _openVgpuPteHi(pState->msgPage.dmaAddr);
    portAtomicMemoryFenceFull();

    pState->sendPut = (pState->sendPut + 8U) & 0xfffU;
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_SEND_RING_PUT, pState->sendPut);
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_RPC_DOORBELL, 1U);

    return NV_OK;
}

NV_STATUS openVgpuBar0RpcRecvPoll(OBJGPU *pGpu, OBJRPC *pRpc, NvU32 expectedFunc)
{
    OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
    rpc_message_header_v *pMsgHdr;
    NvU32 waited = 0U;
    NvU32 recvPut = 0U;
    NvU32 copyLen;

    (void)expectedFunc;

    if (pState == NULL || !pState->bActive || pRpc == NULL || pRpc->message_buffer == NULL)
    {
        return NV_ERR_INVALID_STATE;
    }

    for (waited = 0U; waited < OPEN_VRPC_POLL_ITERS; waited++)
    {
        recvPut = GPU_REG_RD32(pGpu, OPEN_NV_VGPU_RECV_RING_PUT);
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
    GPU_REG_WR32(pGpu, OPEN_NV_VGPU_RECV_RING_GET, pState->recvGet);

    pMsgHdr = (rpc_message_header_v *)pState->msgPage.pCpuAddr;
    if (pMsgHdr->signature != OPEN_VRPC_MAGIC)
    {
        return NV_ERR_INVALID_DATA;
    }

    copyLen = pMsgHdr->length;
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

    NV_PRINTF(LEVEL_INFO, "OPEN_VGPU: 0x50 static info synced (profile=\"%s\" fb=0x%llx)\n",
              pState->staticInfo.profileName,
              (unsigned long long)pState->staticInfo.usableFbBytes);
    return NV_OK;
}

NV_STATUS openVgpuRpcGetConsolidatedGrStaticInfo(OBJGPU *pGpu, OBJRPC *pRpc)
{
    OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);

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

    return _openVgpuPullChunkedStatic(pGpu, pRpc,
                                      NV_VGPU_MSG_FUNCTION_GET_CONSOLIDATED_GR_STATIC_INFO,
                                      OPEN_VRPC_GR_STATIC_INFO_BYTES,
                                      pState->staticInfo.pGrStaticInfoRaw);
}

NV_STATUS openVgpuCreateObject(OBJGPU *pGpu)
{
    OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
    NV_STATUS status;
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

    /* Initial VGX 0x23.0x05 handshake (0x01 + 0x40 + 0x50 + 0x9c) */
    portMemSet(sysInfo, 0, sizeof(sysInfo));
    _wr32(sysInfo, 0x00U, VGX_MAJOR_VERSION_NUMBER);
    _wr32(sysInfo, 0x04U, VGX_MINOR_VERSION_NUMBER);
    _wr32(sysInfo, 0x08U, 1U);
    _copyAscii((char *)(sysInfo + 0x14U), 32U, "535.309.01");

    status = openVgpuRpcSimpleCall(pGpu, pState->pRpc,
                                   NV_VGPU_MSG_FUNCTION_SET_GUEST_SYSTEM_INFO,
                                   sysInfo, (NvU32)sizeof(sysInfo));
    if (status == NV_OK)
    {
        openVgpuRpcSetGuestSystemInfoExt(pGpu, pState->pRpc, "r535_00");
        openVgpuRpcGetConsolidatedStaticInfo(pGpu, pState->pRpc);
        openVgpuRpcGetConsolidatedGrStaticInfo(pGpu, pState->pRpc);
    }
    return NV_OK;

fail:
    openVgpuDestructObject(pGpu);
    return status;
}

void openVgpuDestructObject(OBJGPU *pGpu)
{
    OPEN_VGPU_BAR0_STATE *pState = _openVgpuGetState(pGpu);
    if (pState == NULL)
    {
        return;
    }

    if (pState->bActive)
    {
        _openVgpuTeardownRings(pGpu);
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
