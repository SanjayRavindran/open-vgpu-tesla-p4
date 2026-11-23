/* SPDX-License-Identifier: MIT OR GPL-2.0 */
#include "open_vgpu_guest_rpc.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

static void gw32(uint8_t *p, size_t off, uint32_t v)
{
    memcpy(p + off, &v, 4U);
}

static void gw64(uint8_t *p, size_t off, uint64_t v)
{
    memcpy(p + off, &v, 8U);
}

static uint32_t gr32(const uint8_t *p, size_t off)
{
    uint32_t v = 0U;
    memcpy(&v, p + off, 4U);
    return v;
}

static uint64_t gr64(const uint8_t *p, size_t off)
{
    uint64_t v = 0ULL;
    memcpy(&v, p + off, 8U);
    return v;
}

static uint16_t gr16(const uint8_t *p, size_t off)
{
    uint16_t v = 0U;
    memcpy(&v, p + off, 2U);
    return v;
}

static uint32_t ring_pte_lo(uint64_t gpa)
{
    uint32_t pfn_lo = (uint32_t)((gpa >> 12) & 0x000fffffULL);
    return (pfn_lo << 12) | 0x0000000bU;
}

static uint32_t ring_pte_hi(uint64_t gpa)
{
    return (uint32_t)((gpa >> 32) & 0x000fffffULL);
}

int open_vgpu_guest_init(open_vgpu_guest_ctx_t *ctx, const open_vgpu_guest_transport_t *ops)
{
    if (!ctx || !ops || !ops->bar0_read32 || !ops->bar0_write32 ||
        !ops->alloc_dma_page || !ops->free_dma_page) {
        return -EINVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->ops      = *ops;
    ctx->next_seq = 1U;

    if (ops->pci_cfg_read32) {
        ctx->pci_id    = ops->pci_cfg_read32(ops->priv, 0x00ULL);
        ctx->subsys_id = ops->pci_cfg_read32(ops->priv, 0x2cULL);
        for (size_t i = 0U; i < 4U; i++) {
            uint32_t w = ops->pci_cfg_read32(ops->priv, 0xd7ULL + i * 4ULL);
            memcpy(ctx->vendor_cap_tag + i * 4U, &w, 4U);
        }
        ctx->vendor_cap_tag[16] = '\0';
    }

    ctx->pmc_boot0   = ops->bar0_read32(ops->priv, NV_VGPU_BAR0_PMC_BOOT_0);
    ctx->vgpu_config = ops->bar0_read32(ops->priv, NV_VGPU_BAR0_CONFIG);
    ctx->cstatus_mb  = ops->bar0_read32(ops->priv, NV_VGPU_BAR0_PFB_CSTATUS);
    if ((ctx->vgpu_config & 1U) == 0U) {
        return -ENODEV;
    }

    uint32_t fb_pages_m1 = ops->bar0_read32(ops->priv, NV_VGPU_BAR0_FB_PAGES_M1);
    ctx->virtual_fb_bytes = ((uint64_t)fb_pages_m1 + 1ULL) << 12;

    ctx->send_ring_va  = (uint8_t *)ops->alloc_dma_page(ops->priv, &ctx->send_ring_gpa);
    ctx->recv_ring_va  = (uint8_t *)ops->alloc_dma_page(ops->priv, &ctx->recv_ring_gpa);
    ctx->event_ring_va = (uint8_t *)ops->alloc_dma_page(ops->priv, &ctx->event_ring_gpa);
    ctx->msg_page_va   = (uint8_t *)ops->alloc_dma_page(ops->priv, &ctx->msg_page_gpa);
    if (!ctx->send_ring_va || !ctx->recv_ring_va || !ctx->event_ring_va || !ctx->msg_page_va) {
        open_vgpu_guest_shutdown(ctx);
        return -ENOMEM;
    }

    ops->bar0_write32(ops->priv, NV_VGPU_BAR0_GUEST_ID_58, 0x0aU); /* Linux guest */

    ops->bar0_write32(ops->priv, NV_VGPU_BAR0_SEND_RING_LO, 0U);
    ops->bar0_write32(ops->priv, NV_VGPU_BAR0_SEND_RING_PUT, 0U);
    ops->bar0_write32(ops->priv, NV_VGPU_BAR0_SEND_RING_HI, ring_pte_hi(ctx->send_ring_gpa));
    ops->bar0_write32(ops->priv, NV_VGPU_BAR0_SEND_RING_LO, ring_pte_lo(ctx->send_ring_gpa));

    ops->bar0_write32(ops->priv, NV_VGPU_BAR0_RECV_RING_LO, 0U);
    ops->bar0_write32(ops->priv, NV_VGPU_BAR0_RECV_RING_GET, 0U);
    ops->bar0_write32(ops->priv, NV_VGPU_BAR0_RECV_RING_HI, ring_pte_hi(ctx->recv_ring_gpa));
    ops->bar0_write32(ops->priv, NV_VGPU_BAR0_RECV_RING_LO, ring_pte_lo(ctx->recv_ring_gpa));

    ops->bar0_write32(ops->priv, NV_VGPU_BAR0_EVENT_RING_LO, 0U);
    ops->bar0_write32(ops->priv, NV_VGPU_BAR0_EVENT_RING_GET, 0U);
    ops->bar0_write32(ops->priv, NV_VGPU_BAR0_EVENT_RING_HI, ring_pte_hi(ctx->event_ring_gpa));
    ops->bar0_write32(ops->priv, NV_VGPU_BAR0_EVENT_RING_LO, ring_pte_lo(ctx->event_ring_gpa));

    ops->bar0_write32(ops->priv, NV_VGPU_BAR0_INTR_ENABLE, 0x1ffU);

    ctx->initialized = 1;
    return 0;
}

void open_vgpu_guest_shutdown(open_vgpu_guest_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->initialized && ctx->ops.bar0_write32) {
        ctx->ops.bar0_write32(ctx->ops.priv, NV_VGPU_BAR0_INTR_ENABLE, 0U);
        ctx->ops.bar0_write32(ctx->ops.priv, NV_VGPU_BAR0_SEND_RING_LO, 0U);
        ctx->ops.bar0_write32(ctx->ops.priv, NV_VGPU_BAR0_RECV_RING_LO, 0U);
        ctx->ops.bar0_write32(ctx->ops.priv, NV_VGPU_BAR0_EVENT_RING_LO, 0U);
    }
    if (ctx->ops.free_dma_page) {
        if (ctx->msg_page_va) {
            ctx->ops.free_dma_page(ctx->ops.priv, ctx->msg_page_va, ctx->msg_page_gpa);
        }
        if (ctx->event_ring_va) {
            ctx->ops.free_dma_page(ctx->ops.priv, ctx->event_ring_va, ctx->event_ring_gpa);
        }
        if (ctx->recv_ring_va) {
            ctx->ops.free_dma_page(ctx->ops.priv, ctx->recv_ring_va, ctx->recv_ring_gpa);
        }
        if (ctx->send_ring_va) {
            ctx->ops.free_dma_page(ctx->ops.priv, ctx->send_ring_va, ctx->send_ring_gpa);
        }
    }
    memset(ctx, 0, sizeof(*ctx));
}

int open_vgpu_guest_vrpc_call(open_vgpu_guest_ctx_t *ctx, uint32_t fn,
                              void *payload, uint32_t payload_len,
                              uint32_t *out_status)
{
    if (!ctx || !ctx->initialized || payload_len > OPEN_VGPU_GUEST_MAX_PAYLOAD) {
        return -EINVAL;
    }

    uint8_t *msg = ctx->msg_page_va;
    uint32_t seq = ctx->next_seq++;
    memset(msg, 0, OPEN_VGPU_GUEST_PAGE_BYTES);
    gw32(msg, 0x00U, OPEN_VGPU_GUEST_VRPC_VER_3_0);
    gw32(msg, 0x04U, OPEN_VGPU_GUEST_VRPC_MAGIC);
    gw32(msg, 0x08U, OPEN_VGPU_GUEST_HDR_BYTES + payload_len);
    gw32(msg, 0x0cU, fn);
    gw32(msg, 0x10U, OPEN_VGPU_GUEST_VRPC_PENDING);
    gw32(msg, 0x14U, 0U);
    gw32(msg, 0x18U, seq);
    gw32(msg, 0x1cU, 0U);

    if (payload_len > 0U && payload) {
        memcpy(msg + OPEN_VGPU_GUEST_HDR_BYTES, payload, payload_len);
    }

    uint32_t slot_off = ctx->send_put & 0xff8U;
    gw32(ctx->send_ring_va, slot_off + 0U, ring_pte_lo(ctx->msg_page_gpa));
    gw32(ctx->send_ring_va, slot_off + 4U, ring_pte_hi(ctx->msg_page_gpa));

    ctx->send_put = (ctx->send_put + 8U) & 0xfffU;
    ctx->ops.bar0_write32(ctx->ops.priv, NV_VGPU_BAR0_SEND_RING_PUT, ctx->send_put);
    ctx->ops.bar0_write32(ctx->ops.priv, NV_VGPU_BAR0_RPC_DOORBELL, 1U);

    uint32_t recv_put = ctx->ops.bar0_read32(ctx->ops.priv, NV_VGPU_BAR0_RECV_RING_PUT);
    if (recv_put == ctx->recv_get) {
        return -ETIMEDOUT;
    }
    ctx->recv_get = (ctx->recv_get + 8U) & 0xfffU;
    ctx->ops.bar0_write32(ctx->ops.priv, NV_VGPU_BAR0_RECV_RING_GET, ctx->recv_get);

    if (gr32(msg, 0x04U) != OPEN_VGPU_GUEST_VRPC_MAGIC || gr32(msg, 0x18U) != seq) {
        return -EPROTO;
    }

    if (out_status) {
        *out_status = gr32(msg, 0x10U);
    }
    if (payload_len > 0U && payload) {
        memcpy(payload, msg + OPEN_VGPU_GUEST_HDR_BYTES, payload_len);
    }
    ctx->vrpc_calls++;
    return 0;
}

int open_vgpu_guest_handshake(open_vgpu_guest_ctx_t *ctx, const char *driver_ver,
                              const char *branch_ver, uint32_t *out_status)
{
    uint8_t sys_info[792];
    memset(sys_info, 0, sizeof(sys_info));
    gw32(sys_info, 0x00U, OPEN_VGPU_GUEST_VGX_MAJOR);
    gw32(sys_info, 0x04U, OPEN_VGPU_GUEST_VGX_MINOR);
    gw32(sys_info, 0x08U, 1U); /* Linux x86_64 guest */
    snprintf((char *)(sys_info + 0x14U), 32U, "%.31s",
             driver_ver ? driver_ver : "535.309.01");

    uint32_t st = 0U;
    int rc = open_vgpu_guest_vrpc_call(ctx, 0x01U, sys_info, (uint32_t)sizeof(sys_info), &st);
    if (rc != 0 || st != 0U) {
        if (out_status) *out_status = st;
        return rc;
    }

    uint8_t br[264];
    memset(br, 0, sizeof(br));
    snprintf((char *)br, 32U, "%.31s", branch_ver ? branch_ver : "r535_00");
    rc = open_vgpu_guest_vrpc_call(ctx, 0x40U, br, (uint32_t)sizeof(br), &st);
    if (out_status) *out_status = st;
    return rc;
}

int open_vgpu_guest_fetch_static_info(open_vgpu_guest_ctx_t *ctx,
                                      uint8_t out_blob[OPEN_VGPU_GUEST_STATIC_INFO_LEN],
                                      open_vgpu_guest_static_summary_t *out_summary,
                                      uint32_t *out_status)
{
    if (!out_blob) {
        return -EINVAL;
    }
    memset(out_blob, 0, OPEN_VGPU_GUEST_STATIC_INFO_LEN);

    uint8_t payload[8U + OPEN_VGPU_GUEST_CHUNK_MAX];
    uint32_t off = 0U;
    while (off < OPEN_VGPU_GUEST_STATIC_INFO_LEN) {
        uint32_t want = OPEN_VGPU_GUEST_STATIC_INFO_LEN - off;
        if (want > OPEN_VGPU_GUEST_CHUNK_MAX) {
            want = OPEN_VGPU_GUEST_CHUNK_MAX;
        }
        memset(payload, 0, 8U + want);
        gw32(payload, 0x00U, off);
        gw32(payload, 0x04U, want);

        uint32_t st = 0U;
        int rc = open_vgpu_guest_vrpc_call(ctx, 0x50U, payload, 8U + want, &st);
        if (rc != 0) {
            return rc;
        }
        if (st != 0U) {
            if (out_status) *out_status = st;
            return -EIO;
        }
        memcpy(out_blob + off, payload + 8U, want);
        off += want;
    }

    if (out_status) {
        *out_status = 0U;
    }
    if (out_summary) {
        const uint8_t *core = out_blob + 0x00b98U;
        memset(out_summary, 0, sizeof(*out_summary));
        out_summary->usable_fb_bytes  = gr64(core, 0x00U);
        out_summary->bar1_bytes       = gr64(core, 0x08U);
        out_summary->profile_bytes    = gr64(core, 0x10U);
        out_summary->vgpu_type_id     = gr32(core, 0x18U);
        out_summary->pci_device_id    = gr16(core, 0x1cU);
        out_summary->ecc_supported    = core[0x1eU];
        out_summary->frl_fps          = gr32(core, 0x20U);
        out_summary->encoder_capacity = gr32(core, 0x24U);
        out_summary->subsys_id        = gr32(core, 0x28U);
        out_summary->virt_mode        = gr32(core, 0x8d4U);
        snprintf(out_summary->host_driver_ver, sizeof(out_summary->host_driver_ver),
                 "%.63s", (const char *)(core + 0x60U));
        snprintf(out_summary->host_branch_ver, sizeof(out_summary->host_branch_ver),
                 "%.63s", (const char *)(core + 0x160U));
        snprintf(out_summary->profile_name, sizeof(out_summary->profile_name),
                 "%.63s", (const char *)(core + 0x260U));
        snprintf(out_summary->guest_driver_ver, sizeof(out_summary->guest_driver_ver),
                 "%.31s", (const char *)(core + 0xc00U));
        snprintf(out_summary->guest_branch_ver, sizeof(out_summary->guest_branch_ver),
                 "%.31s", (const char *)(core + 0xe68U));
    }
    return 0;
}

int open_vgpu_guest_fetch_gr_static_info(open_vgpu_guest_ctx_t *ctx,
                                         uint8_t out_blob[OPEN_VGPU_GUEST_GR_INFO_LEN],
                                         open_vgpu_guest_gr_summary_t *out_summary,
                                         uint32_t *out_status)
{
    if (!out_blob) {
        return -EINVAL;
    }
    memset(out_blob, 0, OPEN_VGPU_GUEST_GR_INFO_LEN);

    uint8_t payload[8U + OPEN_VGPU_GUEST_CHUNK_MAX];
    uint32_t off = 0U;
    while (off < OPEN_VGPU_GUEST_GR_INFO_LEN) {
        uint32_t want = OPEN_VGPU_GUEST_GR_INFO_LEN - off;
        if (want > OPEN_VGPU_GUEST_CHUNK_MAX) {
            want = OPEN_VGPU_GUEST_CHUNK_MAX;
        }
        memset(payload, 0, 8U + want);
        gw32(payload, 0x00U, off);
        gw32(payload, 0x04U, want);

        uint32_t st = 0U;
        int rc = open_vgpu_guest_vrpc_call(ctx, 0x9cU, payload, 8U + want, &st);
        if (rc != 0) {
            return rc;
        }
        if (st != 0U) {
            if (out_status) *out_status = st;
            return -EIO;
        }
        memcpy(out_blob + off, payload + 8U, want);
        off += want;
    }

    if (out_status) {
        *out_status = 0U;
    }
    if (out_summary) {
        memset(out_summary, 0, sizeof(*out_summary));
        out_summary->sm_order_word0  = gr32(out_blob, 0x00000U);
        out_summary->pmc_boot0       = gr32(out_blob, 0x00d40U);
        out_summary->pmc_boot42      = gr32(out_blob, 0x00d44U);
        out_summary->gpc_count       = gr32(out_blob, 0x00d48U);
        out_summary->tpc_count       = gr32(out_blob, 0x00d4cU);
        out_summary->cuda_cores      = gr32(out_blob, 0x00d50U);
        out_summary->pdb_props_word0 = gr32(out_blob, 0x07b28U);
    }
    return 0;
}

int open_vgpu_guest_alloc_root(open_vgpu_guest_ctx_t *ctx, uint32_t h_client, uint32_t *out_status)
{
    uint8_t p[108];
    memset(p, 0, sizeof(p));
    gw32(p, 0x00U, h_client);
    return open_vgpu_guest_vrpc_call(ctx, 0x02U, p, (uint32_t)sizeof(p), out_status);
}

int open_vgpu_guest_alloc_device(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                 uint32_t h_device, uint32_t *out_status)
{
    uint8_t p[64];
    memset(p, 0, sizeof(p));
    gw32(p, 0x00U, h_client);
    gw32(p, 0x04U, h_device);
    gw32(p, 0x08U, 0x00000080U); /* NV01_DEVICE_0 */
    return open_vgpu_guest_vrpc_call(ctx, 0x20U, p, (uint32_t)sizeof(p), out_status);
}

int open_vgpu_guest_alloc_subdevice(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                    uint32_t h_device, uint32_t h_subdev, uint32_t *out_status)
{
    uint8_t p[40];
    memset(p, 0, sizeof(p));
    gw32(p, 0x00U, 0U);
    gw32(p, 0x08U, h_client);
    gw32(p, 0x0cU, h_device);
    gw32(p, 0x10U, h_subdev);
    gw32(p, 0x14U, 0x00002080U); /* NV20_SUBDEVICE_0 */
    return open_vgpu_guest_vrpc_call(ctx, 0x13U, p, (uint32_t)sizeof(p), out_status);
}

int open_vgpu_guest_alloc_vidmem(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                 uint32_t h_device, uint32_t h_mem, uint64_t size,
                                 uint64_t fb_pfn, uint32_t *out_status)
{
    uint8_t p[96];
    memset(p, 0, sizeof(p));
    gw32(p, 0x00U, h_client);
    gw32(p, 0x04U, h_device);
    gw32(p, 0x08U, h_mem);
    gw32(p, 0x0cU, 0x00000083U); /* NV01_MEMORY_LIST_FBMEM */
    gw64(p, 0x20U, size);
    gw32(p, 0x28U, 1U);
    gw64(p, 0x58U, fb_pfn);
    return open_vgpu_guest_vrpc_call(ctx, 0x0cU, p, (uint32_t)sizeof(p), out_status);
}

int open_vgpu_guest_alloc_sysmem(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                 uint32_t h_device, uint32_t h_mem, uint64_t size,
                                 const uint64_t *gpfns, uint32_t page_count,
                                 uint32_t *out_status)
{
    if (!gpfns || page_count == 0U || page_count > 64U) {
        return -EINVAL;
    }
    uint32_t plen = 0x38U + page_count * 8U;
    uint8_t p[0x38U + 64U * 8U];
    memset(p, 0, plen);
    gw32(p, 0x00U, h_client);
    gw32(p, 0x04U, h_device);
    gw32(p, 0x08U, h_mem);
    gw32(p, 0x0cU, 0x00000081U); /* NV01_MEMORY_LIST_SYSTEM */
    gw64(p, 0x20U, size);
    gw32(p, 0x28U, page_count);
    for (uint32_t i = 0U; i < page_count; i++) {
        gw64(p, 0x38U + i * 8U, gpfns[i]);
    }
    return open_vgpu_guest_vrpc_call(ctx, 0x04U, p, plen, out_status);
}

int open_vgpu_guest_alloc_virtmem(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                  uint32_t h_device, uint32_t h_virt, uint64_t size,
                                  uint32_t *out_status)
{
    uint8_t p[96];
    memset(p, 0, sizeof(p));
    gw32(p, 0x00U, h_client);
    gw32(p, 0x04U, h_device);
    gw32(p, 0x0cU, h_virt);
    gw32(p, 0x10U, 0x000050a0U); /* NV50_MEMORY_VIRTUAL */
    gw32(p, 0x14U, 0x00080000U); /* virtual flag */
    gw64(p, 0x30U, size);
    return open_vgpu_guest_vrpc_call(ctx, 0x34U, p, (uint32_t)sizeof(p), out_status);
}

int open_vgpu_guest_map_bar1(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                             uint32_t h_device, uint32_t h_mem, uint64_t length,
                             uint64_t *out_bar1_offset, uint32_t *out_status)
{
    uint8_t p[56];
    memset(p, 0, sizeof(p));
    gw32(p, 0x00U, h_client);
    gw32(p, 0x04U, h_device);
    gw32(p, 0x08U, h_mem);
    gw64(p, 0x18U, length);
    uint32_t st = 0U;
    int rc = open_vgpu_guest_vrpc_call(ctx, 0x07U, p, (uint32_t)sizeof(p), &st);
    if (out_status) *out_status = st;
    if (rc == 0 && st == 0U && out_bar1_offset) {
        *out_bar1_offset = gr64(p, 0x20U);
    }
    return rc;
}

int open_vgpu_guest_alloc_ctx_dma(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                  uint32_t h_dma, uint32_t h_mem, uint64_t limit,
                                  uint32_t *out_status)
{
    uint8_t p[56];
    memset(p, 0, sizeof(p));
    gw32(p, 0x04U, h_client);
    gw32(p, 0x08U, h_dma);
    gw32(p, 0x0cU, 0x00000002U); /* NV01_CONTEXT_DMA */
    gw32(p, 0x18U, h_mem);
    gw64(p, 0x28U, limit);
    return open_vgpu_guest_vrpc_call(ctx, 0x05U, p, (uint32_t)sizeof(p), out_status);
}

int open_vgpu_guest_map_dma(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                            uint32_t h_device, uint32_t h_dma, uint32_t h_mem,
                            uint64_t length, uint64_t *inout_dma_offset,
                            uint32_t *out_status)
{
    uint8_t p[56];
    memset(p, 0, sizeof(p));
    gw32(p, 0x00U, h_client);
    gw32(p, 0x04U, h_device);
    gw32(p, 0x08U, h_dma);
    gw32(p, 0x0cU, h_mem);
    gw64(p, 0x18U, length);
    if (inout_dma_offset) {
        gw64(p, 0x28U, *inout_dma_offset);
    }
    uint32_t st = 0U;
    int rc = open_vgpu_guest_vrpc_call(ctx, 0x0eU, p, (uint32_t)sizeof(p), &st);
    if (out_status) *out_status = st;
    if (rc == 0 && st == 0U && inout_dma_offset) {
        *inout_dma_offset = gr64(p, 0x28U);
    }
    return rc;
}

int open_vgpu_guest_unmap_dma(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                              uint32_t h_device, uint32_t h_dma, uint32_t h_mem,
                              uint64_t dma_offset, uint32_t *out_status)
{
    uint8_t p[40];
    memset(p, 0, sizeof(p));
    gw32(p, 0x00U, h_client);
    gw32(p, 0x04U, h_device);
    gw32(p, 0x08U, h_dma);
    gw32(p, 0x0cU, h_mem);
    gw64(p, 0x18U, dma_offset);
    return open_vgpu_guest_vrpc_call(ctx, 0x0fU, p, (uint32_t)sizeof(p), out_status);
}

int open_vgpu_guest_fill_pte_mem(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                 uint32_t h_device, uint32_t page_count,
                                 uint32_t *out_status)
{
    uint8_t p[104];
    memset(p, 0, sizeof(p));
    gw32(p, 0x00U, h_client);
    gw32(p, 0x04U, h_device);
    gw32(p, 0x20U, page_count);
    return open_vgpu_guest_vrpc_call(ctx, 0x1bU, p, (uint32_t)sizeof(p), out_status);
}

int open_vgpu_guest_update_pde_2(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                 uint32_t h_device, uint32_t *out_status)
{
    uint8_t p[64];
    memset(p, 0, sizeof(p));
    gw32(p, 0x00U, h_client);
    gw32(p, 0x04U, h_device);
    return open_vgpu_guest_vrpc_call(ctx, 0x38U, p, (uint32_t)sizeof(p), out_status);
}

int open_vgpu_guest_update_gpu_pdes(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                    uint32_t h_device, uint64_t dir_phys,
                                    uint32_t is_valid, uint32_t *out_status)
{
    uint8_t p[48];
    memset(p, 0, sizeof(p));
    gw64(p, 0x00U, dir_phys);
    gw32(p, 0x08U, is_valid);
    gw32(p, 0x18U, h_client);
    gw32(p, 0x1cU, h_device);
    return open_vgpu_guest_vrpc_call(ctx, 0x3dU, p, (uint32_t)sizeof(p), out_status);
}

int open_vgpu_guest_alloc_channel_dma(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                      uint32_t h_parent, uint32_t h_chan,
                                      uint32_t h_class, uint32_t gpfifo_entries,
                                      uint64_t userd_vmmio, uint64_t userd_pmmio,
                                      uint32_t *out_status)
{
    uint8_t p[248];
    memset(p, 0, sizeof(p));
    gw32(p, 0x00U, h_client);
    gw32(p, 0x04U, h_parent);
    gw32(p, 0x08U, h_chan);
    gw32(p, 0x0cU, h_class);
    gw32(p, 0x28U, gpfifo_entries);
    gw64(p, 0xc0U, userd_vmmio);
    gw64(p, 0xc8U, userd_pmmio);
    return open_vgpu_guest_vrpc_call(ctx, 0x06U, p, (uint32_t)sizeof(p), out_status);
}

int open_vgpu_guest_idle_channels(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                  uint32_t *out_status)
{
    uint8_t p[28];
    memset(p, 0, sizeof(p));
    gw32(p, 0x00U, h_client);
    return open_vgpu_guest_vrpc_call(ctx, 0x17U, p, (uint32_t)sizeof(p), out_status);
}

int open_vgpu_guest_free_obj(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                             uint32_t h_parent, uint32_t h_obj, uint32_t *out_status)
{
    uint8_t p[16];
    memset(p, 0, sizeof(p));
    gw32(p, 0x00U, h_client);
    gw32(p, 0x04U, h_parent);
    gw32(p, 0x08U, h_obj);
    return open_vgpu_guest_vrpc_call(ctx, 0x0aU, p, (uint32_t)sizeof(p), out_status);
}

int open_vgpu_guest_unmap_bar1(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                               uint32_t h_device, uint32_t h_mem, uint32_t flags,
                               uint64_t bar1_offset, uint32_t *out_status)
{
    uint8_t p[48];
    memset(p, 0, sizeof(p));
    gw32(p, 0x00U, h_client);
    gw32(p, 0x04U, h_device);
    gw32(p, 0x08U, h_mem);
    gw32(p, 0x0cU, flags);
    gw64(p, 0x10U, bar1_offset);
    return open_vgpu_guest_vrpc_call(ctx, 0x0dU, p, (uint32_t)sizeof(p), out_status);
}

int open_vgpu_guest_alloc_object(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                 uint32_t h_parent, uint32_t h_obj, uint32_t h_class,
                                 const void *params, uint32_t params_size,
                                 uint32_t *out_status)
{
    uint32_t copy_len = (params != NULL) ? params_size : 0U;
    uint32_t wire_len = 0x18U + copy_len;
    if (wire_len < 0x50U) {
        wire_len = 0x50U;
    }
    if (wire_len > OPEN_VGPU_GUEST_MAX_PAYLOAD) {
        return -EINVAL;
    }
    uint8_t p[512];
    if (wire_len > sizeof(p)) {
        return -EINVAL;
    }
    memset(p, 0, wire_len);
    gw32(p, 0x00U, h_client);
    gw32(p, 0x04U, h_parent);
    gw32(p, 0x08U, h_obj);
    gw32(p, 0x0cU, h_class);
    gw32(p, 0x10U, copy_len);
    if (copy_len > 0U && params != NULL) {
        memcpy(p + 0x18U, params, copy_len);
    }
    return open_vgpu_guest_vrpc_call(ctx, 0x09U, p, wire_len, out_status);
}
