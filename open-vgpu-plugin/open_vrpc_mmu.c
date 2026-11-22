#include "open_vgpu_plugin.h"

#include <stdlib.h>
#include <string.h>

static uint32_t r32(const uint8_t *p, size_t off)
{
    uint32_t v = 0U;
    memcpy(&v, p + off, 4U);
    return v;
}

static uint64_t r64(const uint8_t *p, size_t off)
{
    uint64_t v = 0ULL;
    memcpy(&v, p + off, 8U);
    return v;
}

static void w64(uint8_t *p, size_t off, uint64_t v)
{
    memcpy(p + off, &v, 8U);
}

static int is_valid_channel_class(uint32_t h_class)
{
    static const uint32_t allowed[] = {
        0x906fU, 0xa06fU, 0xa16fU, 0xa26fU, 0xb06fU, 0xc06fU,
        0xc36fU, 0xc46fU, 0xc56fU, 0xc572U, 0xc86fU,
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (allowed[i] == h_class) {
            return 1;
        }
    }
    return 0;
}

static int is_valid_deferred_api_cmd(uint32_t cmd)
{
    static const uint32_t allowed[] = {
        0x2080012bU, 0x2080012cU, 0x2080012dU, 0x20801207U,
        0x2080120aU, 0x2080130fU, 0x20801316U, 0x20801318U,
    };
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (allowed[i] == cmd) {
            return 1;
        }
    }
    return 0;
}

static uint32_t find_parent_device_for_obj(const gp104_vgpu_dev_t *dev, uint32_t gc, uint32_t h_obj)
{
    if (!dev || h_obj == 0U) {
        return 0U;
    }
    for (size_t i = 0; i < GP104_MAX_MEM_RECS; i++) {
        if (dev->mem_recs[i].in_use &&
            dev->mem_recs[i].guest_client == gc &&
            dev->mem_recs[i].h_memory == h_obj &&
            dev->mem_recs[i].h_device != 0U) {
            return dev->mem_recs[i].h_device;
        }
    }
    for (size_t i = 0; i < GP104_MAX_RM_OBJECTS; i++) {
        if (dev->rm_objs[i].in_use &&
            dev->rm_objs[i].h_client == gc &&
            dev->rm_objs[i].h_object == h_obj &&
            dev->rm_objs[i].h_parent != 0U) {
            return dev->rm_objs[i].h_parent;
        }
    }
    return 0U;
}

static gp104_mem_rec_t *find_mem_rec(gp104_vgpu_dev_t *dev, uint32_t gc, uint32_t h_mem)
{
    for (size_t i = 0; i < GP104_MAX_MEM_RECS; i++) {
        if (dev->mem_recs[i].in_use &&
            dev->mem_recs[i].guest_client == gc &&
            dev->mem_recs[i].h_memory == h_mem) {
            return &dev->mem_recs[i];
        }
    }
    return NULL;
}

static gp104_mem_rec_t *find_or_alloc_mem_rec(gp104_vgpu_dev_t *dev, uint32_t gc, uint32_t h_mem)
{
    gp104_mem_rec_t *rec = find_mem_rec(dev, gc, h_mem);
    if (rec) {
        return rec;
    }
    for (size_t i = 0; i < GP104_MAX_MEM_RECS; i++) {
        if (!dev->mem_recs[i].in_use) {
            memset(&dev->mem_recs[i], 0, sizeof(dev->mem_recs[i]));
            dev->mem_recs[i].in_use       = 1;
            dev->mem_recs[i].guest_client = gc;
            dev->mem_recs[i].h_memory     = h_mem;
            return &dev->mem_recs[i];
        }
    }
    return NULL;
}

static gp104_dma_map_t *find_dma_map(gp104_vgpu_dev_t *dev, uint32_t gc, uint32_t h_dma,
                                     uint32_t h_mem, uint64_t dma_off)
{
    for (size_t i = 0; i < GP104_MAX_DMA_MAPS; i++) {
        if (dev->dma_maps[i].in_use &&
            dev->dma_maps[i].guest_client == gc &&
            dev->dma_maps[i].h_dma == h_dma &&
            dev->dma_maps[i].h_memory == h_mem &&
            dev->dma_maps[i].dma_offset == dma_off) {
            return &dev->dma_maps[i];
        }
    }
    return NULL;
}

static gp104_dma_map_t *alloc_dma_map(gp104_vgpu_dev_t *dev)
{
    for (size_t i = 0; i < GP104_MAX_DMA_MAPS; i++) {
        if (!dev->dma_maps[i].in_use) {
            memset(&dev->dma_maps[i], 0, sizeof(dev->dma_maps[i]));
            dev->dma_maps[i].in_use = 1;
            return &dev->dma_maps[i];
        }
    }
    return NULL;
}

static gp104_chan_rec_t *alloc_chan_rec(gp104_vgpu_dev_t *dev)
{
    for (size_t i = 0; i < GP104_MAX_CHAN_RECS; i++) {
        if (!dev->chan_recs[i].in_use) {
            memset(&dev->chan_recs[i], 0, sizeof(dev->chan_recs[i]));
            dev->chan_recs[i].in_use = 1;
            return &dev->chan_recs[i];
        }
    }
    return NULL;
}

static void unpin_and_free_mem_gpfns(gp104_vgpu_dev_t *dev, uint64_t *gpfns, uint32_t count)
{
    if (!gpfns) {
        return;
    }
    if (count > 0U && dev && dev->host_env && dev->host_env->unpin_pages) {
        dev->host_env->unpin_pages(dev->dev_handle, gpfns, 0ULL, count);
    }
    free(gpfns);
}

static void release_mem_rec(gp104_vgpu_dev_t *dev, gp104_mem_rec_t *rec)
{
    if (!dev || !rec || !rec->in_use) {
        return;
    }
    if (rec->bar1_mapped) {
        if (dev->bar1_region_id != 0U && rec->bar1_vmmio != 0ULL && rec->bar1_pmmio != 0ULL) {
            gp104_guest_region_map(dev, dev->bar1_region_id, rec->bar1_pmmio,
                                   rec->bar1_size, rec->bar1_vmmio, 0);
        }
        if (dev->rm.attached && dev->rm.ctl_fd >= 0 && rec->rm_cpu_addr != NULL) {
            uint32_t hc = 0U;
            if (open_vgpu_rm_lookup_guest_client(&dev->rm, rec->guest_client, &hc)) {
                open_vgpu_rm_unmap_memory(dev->rm.ctl_fd, hc,
                                          rec->h_device ? rec->h_device : hc,
                                          rec->h_memory, rec->rm_cpu_addr, 0U);
            }
        }
        if (dev->bar1_allocated_bytes >= rec->bar1_size) {
            dev->bar1_allocated_bytes -= rec->bar1_size;
        } else {
            dev->bar1_allocated_bytes = 0ULL;
        }
        rec->bar1_mapped = 0;
        rec->rm_cpu_addr = NULL;
    }
    if (rec->pinned_gpfns != NULL) {
        unpin_and_free_mem_gpfns(dev, rec->pinned_gpfns, rec->pinned_count);
        rec->pinned_gpfns = NULL;
        rec->pinned_count = 0U;
    }
    rec->in_use = 0;
}

static void release_dma_map(gp104_vgpu_dev_t *dev, gp104_dma_map_t *dm)
{
    if (!dev || !dm || !dm->in_use) {
        return;
    }
    if (dev->rm.attached && dev->rm.ctl_fd >= 0) {
        uint32_t hc = 0U;
        if (open_vgpu_rm_lookup_guest_client(&dev->rm, dm->guest_client, &hc)) {
            open_vgpu_rm_unmap_memory_dma(dev->rm.ctl_fd, hc,
                                          dm->h_device ? dm->h_device : hc,
                                          dm->h_dma, dm->h_memory,
                                          dm->flags, dm->dma_offset);
        }
    }
    dm->in_use = 0;
}

static void release_chan_rec(gp104_vgpu_dev_t *dev, gp104_chan_rec_t *ch)
{
    if (!dev || !ch || !ch->in_use) {
        return;
    }
    if (ch->userd_mapped && dev->bar0_region_id != 0U &&
        ch->userd_vmmio != 0ULL && ch->userd_pmmio != 0ULL) {
        gp104_guest_region_map(dev, dev->bar0_region_id, ch->userd_pmmio,
                               0x1000ULL, ch->userd_vmmio, 0);
        ch->userd_mapped = 0;
    }
    gp104_chid_free(dev, ch->guest_chid);
    if (dev->active_channel_count > 0U) {
        dev->active_channel_count--;
    }
    ch->in_use = 0;
}

uint32_t gp104_guest_region_map(gp104_vgpu_dev_t *dev, uint32_t region_id,
                                uint64_t pmmio, uint64_t size, uint64_t vmmio, int add)
{
    if (!dev || size == 0ULL || region_id == 0U) {
        return VMIOP_ERROR_INVAL;
    }
    uint64_t pmmio_page = pmmio & ~0xfffULL;
    uint64_t vmmio_page = vmmio & ~0xfffULL;
    uint64_t span = (((pmmio + size - 1ULL) >> 12) - (pmmio >> 12) + 1ULL) * 0x1000ULL;

    gp104_mmio_map_t *found = NULL;
    for (size_t i = 0; i < GP104_MAX_MMIO_MAPS; i++) {
        gp104_mmio_map_t *m = &dev->mmio_maps[i];
        if (m->in_use && m->region_id == region_id && m->pmmio_page == pmmio_page &&
            m->vmmio_page == vmmio_page && m->size == span) {
            found = m;
            break;
        }
    }

    if (add != 0) {
        if (found) {
            found->refcnt++;
            return VMIOP_SUCCESS;
        }
        gp104_mmio_map_t *slot = NULL;
        for (size_t i = 0; i < GP104_MAX_MMIO_MAPS; i++) {
            if (!dev->mmio_maps[i].in_use) {
                slot = &dev->mmio_maps[i];
                break;
            }
        }
        if (!slot) {
            return VMIOP_ERROR_NO_MEMORY;
        }
        uint32_t rc = vmiop_map_guest_region(region_id, pmmio_page, 0ULL, span, vmmio_page, 1);
        if (rc != VMIOP_SUCCESS) {
            return rc;
        }
        slot->in_use     = 1;
        slot->region_id  = region_id;
        slot->pmmio_page = pmmio_page;
        slot->vmmio_page = vmmio_page;
        slot->size       = span;
        slot->refcnt     = 1;
        dev->mmio_region_maps_total++;
        return VMIOP_SUCCESS;
    }

    if (!found) {
        return VMIOP_ERROR_NOT_FOUND;
    }
    found->refcnt--;
    if (found->refcnt > 0) {
        return VMIOP_SUCCESS;
    }
    uint32_t rc = vmiop_map_guest_region(region_id, pmmio_page, 0ULL, span, vmmio_page, 0);
    if (rc != VMIOP_SUCCESS) {
        found->refcnt++;
        return rc;
    }
    memset(found, 0, sizeof(*found));
    dev->mmio_region_unmaps_total++;
    return VMIOP_SUCCESS;
}

void gp104_mmu_on_free_obj(gp104_vgpu_dev_t *dev, uint32_t guest_client, uint32_t h_obj)
{
    if (!dev || guest_client == 0U || h_obj == 0U) {
        return;
    }
    for (size_t i = 0; i < GP104_MAX_DMA_MAPS; i++) {
        if (dev->dma_maps[i].in_use && dev->dma_maps[i].guest_client == guest_client &&
            (dev->dma_maps[i].h_memory == h_obj || dev->dma_maps[i].h_dma == h_obj)) {
            release_dma_map(dev, &dev->dma_maps[i]);
        }
    }
    for (size_t i = 0; i < GP104_MAX_CHAN_RECS; i++) {
        if (dev->chan_recs[i].in_use && dev->chan_recs[i].guest_client == guest_client &&
            dev->chan_recs[i].h_channel == h_obj) {
            release_chan_rec(dev, &dev->chan_recs[i]);
        }
    }
    for (size_t i = 0; i < GP104_MAX_MEM_RECS; i++) {
        if (dev->mem_recs[i].in_use && dev->mem_recs[i].guest_client == guest_client &&
            dev->mem_recs[i].h_memory == h_obj) {
            release_mem_rec(dev, &dev->mem_recs[i]);
        }
    }
}

void gp104_mmu_on_free_client(gp104_vgpu_dev_t *dev, uint32_t guest_client)
{
    if (!dev || guest_client == 0U) {
        return;
    }
    for (size_t i = 0; i < GP104_MAX_CHAN_RECS; i++) {
        if (dev->chan_recs[i].in_use && dev->chan_recs[i].guest_client == guest_client) {
            release_chan_rec(dev, &dev->chan_recs[i]);
        }
    }
    for (size_t i = 0; i < GP104_MAX_DMA_MAPS; i++) {
        if (dev->dma_maps[i].in_use && dev->dma_maps[i].guest_client == guest_client) {
            release_dma_map(dev, &dev->dma_maps[i]);
        }
    }
    for (size_t i = 0; i < GP104_MAX_MEM_RECS; i++) {
        if (dev->mem_recs[i].in_use && dev->mem_recs[i].guest_client == guest_client) {
            release_mem_rec(dev, &dev->mem_recs[i]);
        }
    }
}

void gp104_mmu_release_all(gp104_vgpu_dev_t *dev)
{
    if (!dev) {
        return;
    }
    for (size_t i = 0; i < GP104_MAX_CHAN_RECS; i++) {
        if (dev->chan_recs[i].in_use) {
            release_chan_rec(dev, &dev->chan_recs[i]);
        }
    }
    for (size_t i = 0; i < GP104_MAX_DMA_MAPS; i++) {
        if (dev->dma_maps[i].in_use) {
            release_dma_map(dev, &dev->dma_maps[i]);
        }
    }
    for (size_t i = 0; i < GP104_MAX_MEM_RECS; i++) {
        if (dev->mem_recs[i].in_use) {
            release_mem_rec(dev, &dev->mem_recs[i]);
        }
    }
    for (size_t i = 0; i < GP104_MAX_MMIO_MAPS; i++) {
        gp104_mmio_map_t *m = &dev->mmio_maps[i];
        if (m->in_use) {
            vmiop_map_guest_region(m->region_id, m->pmmio_page, 0ULL, m->size, m->vmmio_page, 0);
            dev->mmio_region_unmaps_total++;
            memset(m, 0, sizeof(*m));
        }
    }
    dev->active_channel_count = 0U;
    dev->bar1_allocated_bytes = 0ULL;
    dev->gpu_pde_bound_count  = 0U;
}

static uint32_t handle_vrpc_alloc_memory(gp104_vgpu_dev_t *dev, const uint8_t *payload,
                                         uint32_t plen)
{
    if (plen < 0x38U) {
        return 0xff100002U;
    }
    uint32_t gc        = r32(payload, 0x00);
    uint32_t h_parent  = r32(payload, 0x04);
    uint32_t h_mem     = r32(payload, 0x08);
    uint32_t flags     = r32(payload, 0x10);
    uint32_t page_off  = r32(payload, 0x14);
    uint32_t attr      = r32(payload, 0x18);
    uint64_t length    = r64(payload, 0x20);
    uint32_t page_cnt  = r32(payload, 0x28);
    uint64_t base_pfn  = r64(payload, 0x30);

    if (h_mem == 0U || page_cnt > (1U << 20)) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    if (gp104_is_reserved_plugin_handle(dev->dev_handle, h_mem)) {
        return 0xff100007U;
    }
    if ((uint64_t)page_cnt * 0x1000ULL < (uint64_t)page_off + length) {
        return NV_ERR_INVALID_ARGUMENT;
    }

    gp104_mem_rec_t *rec = find_or_alloc_mem_rec(dev, gc, h_mem);
    if (!rec) {
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }

    uint64_t *gpfns = NULL;
    uint64_t *hpfns = NULL;
    if (page_cnt > 0U) {
        gpfns = (uint64_t *)calloc((size_t)page_cnt, sizeof(uint64_t));
        hpfns = (uint64_t *)calloc((size_t)page_cnt, sizeof(uint64_t));
        if (!gpfns || !hpfns) {
            free(hpfns);
            free(gpfns);
            if (!rec->bar1_mapped && !rec->pinned_gpfns) rec->in_use = 0;
            return NV_ERR_NO_MEMORY;
        }
        if ((size_t)plen >= 0x38U + (size_t)page_cnt * 8U) {
            memcpy(gpfns, payload + 0x38U, (size_t)page_cnt * 8U);
        } else {
            for (uint32_t i = 0; i < page_cnt; i++) {
                gpfns[i] = base_pfn + i;
            }
        }
        uint32_t prc = vmiop_pin_guest_pages_v2(dev->dev_handle, gpfns, hpfns, page_cnt);
        if (prc != VMIOP_SUCCESS) {
            free(hpfns);
            free(gpfns);
            if (!rec->bar1_mapped && !rec->pinned_gpfns) rec->in_use = 0;
            return vmiop_err_to_rm_status(prc);
        }
    }

    uint32_t hc = gc;
    if (dev->rm.attached && dev->rm.ctl_fd >= 0) {
        NV_STATUS st = gp104_resolve_host_client(dev, gc, &hc);
        if (st != NV_OK) {
            unpin_and_free_mem_gpfns(dev, gpfns, page_cnt);
            free(hpfns);
            if (!rec->bar1_mapped && !rec->pinned_gpfns) rec->in_use = 0;
            return st;
        }
        NV_MEMORY_LIST_ALLOCATION_PARAMS params;
        memset(&params, 0, sizeof(params));
        uint32_t rm_parent = (h_parent == 0U || h_parent == gc) ? hc : h_parent;
        params.hClient         = hc;
        params.hParent         = rm_parent;
        params.hObject         = h_mem;
        params.pageOffset      = page_off;
        params.attr            = attr;
        params.pageCount       = page_cnt;
        params.pPageNumberList = (NvP64)(uintptr_t)hpfns;
        params.limit           = (length > 0ULL) ? (length - 1ULL) : 0ULL;
        params.flagsOs02       = (flags & 0x200000U) | (flags & 0xf000U) | 0x40000010U;
        uint64_t guest_uuid[2] = { 0ULL, 0ULL };
        vmiop_get_guest_id(guest_uuid);
        memcpy(params.guestId, guest_uuid, sizeof(params.guestId));

        dev->vrpc_rm_forwarded_calls++;
        st = open_vgpu_rm_alloc_obj(dev->rm.ctl_fd, hc, rm_parent, h_mem,
                                    NV01_MEMORY_LIST_SYSTEM, &params,
                                    (uint32_t)sizeof(params));
        if (st != NV_OK) {
            unpin_and_free_mem_gpfns(dev, gpfns, page_cnt);
            free(hpfns);
            if (!rec->bar1_mapped && !rec->pinned_gpfns) rec->in_use = 0;
            return st;
        }
        rec->h_device = rm_parent;
    } else {
        rec->h_device = h_parent ? h_parent : gc;
    }

    free(hpfns);
    if (rec->pinned_gpfns) {
        unpin_and_free_mem_gpfns(dev, rec->pinned_gpfns, rec->pinned_count);
    }
    rec->h_class      = NV01_MEMORY_LIST_SYSTEM;
    rec->size         = length;
    rec->pinned_count = page_cnt;
    rec->pinned_gpfns = gpfns;
    dev->pinned_pages_total += page_cnt;

    return open_vgpu_plugin_record_obj(dev, gc, h_parent ? h_parent : gc,
                                       h_mem, NV01_MEMORY_LIST_SYSTEM);
}

static uint32_t handle_vrpc_alloc_ctx_dma(gp104_vgpu_dev_t *dev, const uint8_t *payload,
                                          uint32_t plen)
{
    if (plen < 0x38U) {
        return 0xff100002U;
    }
    uint32_t gc    = r32(payload, 0x04);
    uint32_t h_dma = r32(payload, 0x08);
    uint32_t h_cls = r32(payload, 0x0c);
    uint32_t flags = r32(payload, 0x10);
    uint32_t h_mem = r32(payload, 0x18);
    uint64_t off   = r64(payload, 0x20);
    uint64_t lim   = r64(payload, 0x28);

    if (gp104_is_reserved_plugin_handle(dev->dev_handle, h_dma)) {
        return 0xff100007U;
    }
    if (gc == 0U || h_dma == 0U) {
        return NV_ERR_INVALID_ARGUMENT;
    }

    uint32_t cls = h_cls ? h_cls : NV01_CONTEXT_DMA;
    uint32_t h_parent = find_parent_device_for_obj(dev, gc, h_mem);
    if (h_parent == 0U) {
        h_parent = r32(payload, 0x00);
    }

    if (dev->rm.attached && dev->rm.ctl_fd >= 0) {
        uint32_t hc = 0U;
        NV_STATUS st = gp104_resolve_host_client(dev, gc, &hc);
        if (st != NV_OK) {
            return st;
        }
        NV_CONTEXT_DMA_ALLOCATION_PARAMS cp;
        memset(&cp, 0, sizeof(cp));
        cp.flags   = flags;
        cp.hMemory = h_mem;
        cp.offset  = off;
        cp.limit   = lim;

        dev->vrpc_rm_forwarded_calls++;
        st = open_vgpu_rm_alloc_obj(dev->rm.ctl_fd, hc, h_parent ? h_parent : hc,
                                    h_dma, cls, &cp, (uint32_t)sizeof(cp));
        if (st != NV_OK) {
            return st;
        }
    }
    return open_vgpu_plugin_record_obj(dev, gc, h_parent ? h_parent : gc, h_dma, cls);
}

static uint32_t handle_vrpc_alloc_channel_dma(gp104_vgpu_dev_t *dev, const uint8_t *payload,
                                              uint32_t plen)
{
    if (plen < 0xf8U) {
        return 0xff100002U;
    }
    uint32_t gc             = r32(payload, 0x00);
    uint32_t h_parent       = r32(payload, 0x04);
    uint32_t h_chan         = r32(payload, 0x08);
    uint32_t h_cls          = r32(payload, 0x0c);
    uint32_t h_err          = r32(payload, 0x10);
    uint32_t h_buf          = r32(payload, 0x14);
    uint32_t h_userd_mem    = r32(payload, 0x18);
    uint64_t gpfifo_off     = r64(payload, 0x20);
    uint32_t gpfifo_entries = r32(payload, 0x28);
    uint64_t userd_vmmio    = r64(payload, 0xc0);
    uint64_t userd_pmmio    = r64(payload, 0xc8);

    if (!is_valid_channel_class(h_cls)) {
        return 0x22U; /* NV_ERR_INVALID_CLASS */
    }
    if (gpfifo_entries == 0U || (gpfifo_entries & (gpfifo_entries - 1U)) != 0U) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    if (dev->license_state == 4U && dev->active_channel_count >= 0x55U) {
        return NV_ERR_NO_MEMORY; /* Unlicensed 85-channel ceiling per 0x1a0bf0.c */
    }
    if (gp104_is_reserved_plugin_handle(dev->dev_handle, h_chan)) {
        return 0xff100007U;
    }
    if (h_chan == 0U || dev->active_channel_count >= GP104_MAX_CHANNELS) {
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }

    gp104_chan_rec_t *crec = alloc_chan_rec(dev);
    if (!crec) {
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }

    uint32_t guest_chid = UINT32_MAX;
    for (uint32_t i = 0; i < GP104_MAX_CHANNELS; i++) {
        if (dev->chid_bitmap[i] == 0U) {
            guest_chid = i;
            break;
        }
    }
    uint32_t phys_chid = 0U;
    if (guest_chid == UINT32_MAX || gp104_chid_alloc(dev, guest_chid, &phys_chid) != 0) {
        crec->in_use = 0;
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }

    uint32_t hc = gc;
    int rm_allocated = 0;
    if (dev->rm.attached && dev->rm.ctl_fd >= 0) {
        NV_STATUS st = gp104_resolve_host_client(dev, gc, &hc);
        if (st != NV_OK) {
            gp104_chid_free(dev, guest_chid);
            crec->in_use = 0;
            return st;
        }
        uint8_t ch_params[0x90];
        memset(ch_params, 0, sizeof(ch_params));
        memcpy(ch_params + 0x00, &h_err, 4U);
        memcpy(ch_params + 0x04, &h_buf, 4U);
        memcpy(ch_params + 0x08, &gpfifo_off, 8U);
        memcpy(ch_params + 0x10, &gpfifo_entries, 4U);
        memcpy(ch_params + 0x20, &h_userd_mem, 4U);

        dev->vrpc_rm_forwarded_calls++;
        st = open_vgpu_rm_alloc_obj(dev->rm.ctl_fd, hc, h_parent ? h_parent : hc,
                                    h_chan, h_cls, ch_params, (uint32_t)sizeof(ch_params));
        if (st != NV_OK) {
            gp104_chid_free(dev, guest_chid);
            crec->in_use = 0;
            return st;
        }
        rm_allocated = 1;

        if (userd_vmmio != 0ULL && userd_pmmio == 0ULL && h_userd_mem != 0U) {
            uint8_t phys_attr[0x10];
            memset(phys_attr, 0, sizeof(phys_attr));
            if (open_vgpu_rm_control(dev->rm.ctl_fd, hc, h_userd_mem,
                                     NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR,
                                     phys_attr, (uint32_t)sizeof(phys_attr)) == NV_OK) {
                userd_pmmio = r64(phys_attr, 0x08);
            }
        }
    }

    int mapped_userd = 0;
    if (userd_vmmio != 0ULL && userd_pmmio != 0ULL && dev->bar0_region_id != 0U) {
        uint32_t mrc = gp104_guest_region_map(dev, dev->bar0_region_id, userd_pmmio,
                                              0x1000ULL, userd_vmmio, 1);
        if (mrc != VMIOP_SUCCESS) {
            if (rm_allocated) {
                open_vgpu_rm_free(dev->rm.ctl_fd, hc, h_parent ? h_parent : hc, h_chan);
            }
            gp104_chid_free(dev, guest_chid);
            crec->in_use = 0;
            return vmiop_err_to_rm_status(mrc);
        }
        mapped_userd = 1;
    }

    crec->guest_client = gc;
    crec->h_parent     = h_parent ? h_parent : gc;
    crec->h_channel    = h_chan;
    crec->h_class      = h_cls;
    crec->guest_chid   = guest_chid;
    crec->phys_chid    = phys_chid;
    crec->userd_vmmio  = userd_vmmio;
    crec->userd_pmmio  = userd_pmmio;
    crec->userd_mapped = mapped_userd;
    dev->active_channel_count++;

    return open_vgpu_plugin_record_obj(dev, gc, crec->h_parent, h_chan, h_cls);
}

static uint32_t handle_vrpc_map_memory(gp104_vgpu_dev_t *dev, uint8_t *payload, uint32_t plen)
{
    if (plen < 0x38U) {
        return 0xff100002U;
    }
    uint32_t gc       = r32(payload, 0x00);
    uint32_t h_dev    = r32(payload, 0x04);
    uint32_t h_mem    = r32(payload, 0x08);
    uint32_t flags    = r32(payload, 0x0c);
    uint64_t offset   = r64(payload, 0x10);
    uint64_t length   = r64(payload, 0x18);
    uint64_t bar1_off = r64(payload, 0x20);

    if (length == 0ULL || h_mem == 0U) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    if (dev->bar1_allocated_bytes + length > dev->profile.bar1_bytes) {
        return NV_ERR_NO_MEMORY;
    }

    gp104_mem_rec_t *rec = find_or_alloc_mem_rec(dev, gc, h_mem);
    if (!rec) {
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }

    uint32_t hc = gc;
    void *cpu_addr = NULL;
    uint64_t pmmio = 0ULL;
    int rm_mapped = 0;

    if (dev->rm.attached && dev->rm.ctl_fd >= 0) {
        NV_STATUS st = gp104_resolve_host_client(dev, gc, &hc);
        if (st != NV_OK) {
            if (!rec->pinned_gpfns && !rec->bar1_mapped) rec->in_use = 0;
            return st;
        }
        dev->vrpc_rm_forwarded_calls++;
        st = open_vgpu_rm_map_memory(dev->rm.ctl_fd, dev->rm.gpu_fd, hc,
                                     h_dev ? h_dev : hc, h_mem, offset, length,
                                     flags, &cpu_addr);
        if (st != NV_OK) {
            if (!rec->pinned_gpfns && !rec->bar1_mapped) rec->in_use = 0;
            return st;
        }
        rm_mapped = 1;

        uint8_t phys_attr[0x10];
        memset(phys_attr, 0, sizeof(phys_attr));
        if (open_vgpu_rm_control(dev->rm.ctl_fd, hc, h_mem,
                                 NV0041_CTRL_CMD_GET_SURFACE_PHYS_ATTR,
                                 phys_attr, (uint32_t)sizeof(phys_attr)) == NV_OK) {
            pmmio = r64(phys_attr, 0x08);
        }
        if (pmmio == 0ULL) {
            pmmio = offset;
        }
    }

    if (bar1_off == 0ULL) {
        bar1_off = (dev->bar1_allocated_bytes + 0xfffULL) & ~0xfffULL;
    }
    uint64_t vmmio     = dev->bar1_gpa + bar1_off;
    uint64_t map_pmmio = pmmio ? pmmio : (0xfe0000000ULL + bar1_off);

    int mmio_mapped = 0;
    if (dev->bar1_region_id != 0U && (pmmio != 0ULL || dev->bar1_gpa != 0ULL)) {
        uint32_t mrc = gp104_guest_region_map(dev, dev->bar1_region_id, map_pmmio,
                                              length, vmmio, 1);
        if (mrc != VMIOP_SUCCESS) {
            if (rm_mapped) {
                open_vgpu_rm_unmap_memory(dev->rm.ctl_fd, hc, h_dev ? h_dev : hc,
                                          h_mem, cpu_addr, 0U);
            }
            if (!rec->pinned_gpfns && !rec->bar1_mapped) rec->in_use = 0;
            return vmiop_err_to_rm_status(mrc);
        }
        mmio_mapped = 1;
    }

    w64(payload, 0x20, bar1_off);
    rec->h_device    = h_dev ? h_dev : rec->h_device;
    rec->rm_cpu_addr = cpu_addr;
    if (rec->bar1_mapped && dev->bar1_allocated_bytes >= rec->bar1_size) {
        dev->bar1_allocated_bytes -= rec->bar1_size;
    }
    rec->bar1_mapped = mmio_mapped || rm_mapped;
    rec->bar1_offset = bar1_off;
    rec->bar1_vmmio  = vmmio;
    rec->bar1_pmmio  = map_pmmio;
    rec->bar1_size   = length;
    dev->bar1_allocated_bytes += length;
    return NV_OK;
}

static uint32_t handle_vrpc_alloc_object(gp104_vgpu_dev_t *dev, uint8_t *payload, uint32_t plen)
{
    if (plen < 0x50U) {
        return 0xff100002U;
    }
    uint32_t gc        = r32(payload, 0x00);
    uint32_t h_parent  = r32(payload, 0x04);
    uint32_t h_obj     = r32(payload, 0x08);
    uint32_t h_cls     = r32(payload, 0x0c);
    uint32_t params_sz = r32(payload, 0x10);

    if ((size_t)params_sz > (size_t)(plen - 0x18U)) {
        return 0xff100002U;
    }
    if (gp104_is_reserved_plugin_handle(dev->dev_handle, h_obj)) {
        return 0xff100007U;
    }
    if (h_cls == 0xc076U || h_cls == 0xc574U) {
        return NV_ERR_NOT_SUPPORTED;
    }

    if (dev->rm.attached && dev->rm.ctl_fd >= 0) {
        uint32_t hc = 0U;
        NV_STATUS st = gp104_resolve_host_client(dev, gc, &hc);
        if (st != NV_OK) {
            return st;
        }
        dev->vrpc_rm_forwarded_calls++;
        st = open_vgpu_rm_alloc_obj(dev->rm.ctl_fd, hc, h_parent ? h_parent : hc,
                                    h_obj, h_cls, params_sz ? (payload + 0x18U) : NULL, params_sz);
        if (st != NV_OK) {
            return st;
        }
    }
    return open_vgpu_plugin_record_obj(dev, gc, h_parent ? h_parent : gc, h_obj, h_cls);
}

static uint32_t handle_vrpc_alloc_vidmem(gp104_vgpu_dev_t *dev, const uint8_t *payload,
                                         uint32_t plen)
{
    if (plen < 0x60U) {
        return 0xff100002U;
    }
    uint32_t gc       = r32(payload, 0x00);
    uint32_t h_parent = r32(payload, 0x04);
    uint32_t h_mem    = r32(payload, 0x08);
    uint32_t h_cls    = r32(payload, 0x0c);
    uint32_t attr     = r32(payload, 0x14);
    uint32_t flags    = r32(payload, 0x1c);
    uint64_t size     = r64(payload, 0x20);
    uint32_t format   = r32(payload, 0x40);
    uint32_t compr    = r32(payload, 0x48);

    if (gp104_is_reserved_plugin_handle(dev->dev_handle, h_mem)) {
        return 0xff100007U;
    }
    if ((attr & 0x06000000U) != 0U && (attr & 0x06000000U) != 0x06000000U) {
        return NV_ERR_INVALID_ARGUMENT;
    }

    uint64_t usable_fb = dev->profile.usable_fb_bytes;
    uint64_t max_pfn   = (usable_fb >= 0x1000ULL) ? ((usable_fb - 1ULL) >> 12) : 0ULL;
    uint64_t pfn_buf[64];
    uint32_t pfn_cnt = 0U;

    if (plen > 0x60U) {
        size_t avail_pfns = ((size_t)plen - 0x60U) / 8U;
        if (avail_pfns > 64U) avail_pfns = 64U;
        for (size_t i = 0; i < avail_pfns; i++) {
            uint64_t pfn = r64(payload, 0x60U + i * 8U);
            if ((flags & 0x80000U) == 0U && pfn > max_pfn) {
                return NV_ERR_INSUFFICIENT_RESOURCES;
            }
            pfn_buf[pfn_cnt++] = pfn;
        }
    } else {
        uint64_t pfn = r64(payload, 0x58U);
        if ((flags & 0x80000U) == 0U && pfn > max_pfn) {
            return NV_ERR_INSUFFICIENT_RESOURCES;
        }
        pfn_buf[pfn_cnt++] = pfn;
    }

    uint32_t rm_cls = (flags & 0x80000U) ? NV50_MEMORY_VIRTUAL
                                         : (h_cls ? h_cls : NV01_MEMORY_LIST_FBMEM);

    gp104_mem_rec_t *rec = find_or_alloc_mem_rec(dev, gc, h_mem);
    if (!rec) {
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }

    if (dev->rm.attached && dev->rm.ctl_fd >= 0) {
        uint32_t hc = 0U;
        NV_STATUS st = gp104_resolve_host_client(dev, gc, &hc);
        if (st != NV_OK) {
            if (!rec->pinned_gpfns && !rec->bar1_mapped) rec->in_use = 0;
            return st;
        }
        NV_MEMORY_LIST_ALLOCATION_PARAMS params;
        memset(&params, 0, sizeof(params));
        uint32_t rm_parent     = (h_parent == 0U || h_parent == gc) ? hc : h_parent;
        params.hClient         = hc;
        params.hParent         = rm_parent;
        params.hObject         = h_mem;
        params.attr            = attr;
        params.pageCount       = pfn_cnt;
        params.format          = format;
        params.comprCovg       = compr;
        params.pPageNumberList = (NvP64)(uintptr_t)pfn_buf;
        params.limit           = (size > 0ULL) ? (size - 1ULL) : 0ULL;
        params.flagsOs02       = flags;

        dev->vrpc_rm_forwarded_calls++;
        st = open_vgpu_rm_alloc_obj(dev->rm.ctl_fd, hc, rm_parent, h_mem, rm_cls,
                                    &params, (uint32_t)sizeof(params));
        if (st != NV_OK) {
            if (!rec->pinned_gpfns && !rec->bar1_mapped) rec->in_use = 0;
            return st;
        }
        rec->h_device = rm_parent;
    } else {
        rec->h_device = h_parent ? h_parent : gc;
    }

    rec->h_class = rm_cls;
    rec->size    = size;
    return open_vgpu_plugin_record_obj(dev, gc, h_parent ? h_parent : gc, h_mem, rm_cls);
}

static uint32_t handle_vrpc_map_memory_dma(gp104_vgpu_dev_t *dev, uint8_t *payload, uint32_t plen)
{
    if (plen < 0x38U) {
        return 0xff100002U;
    }
    uint32_t gc      = r32(payload, 0x00);
    uint32_t h_dev   = r32(payload, 0x04);
    uint32_t h_dma   = r32(payload, 0x08);
    uint32_t h_mem   = r32(payload, 0x0c);
    uint64_t offset  = r64(payload, 0x10);
    uint64_t length  = r64(payload, 0x18);
    uint32_t flags   = r32(payload, 0x20);
    uint64_t dma_off = r64(payload, 0x28);

    gp104_dma_map_t *dm = alloc_dma_map(dev);
    if (!dm) {
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }

    if (dev->rm.attached && dev->rm.ctl_fd >= 0) {
        uint32_t hc = 0U;
        NV_STATUS st = gp104_resolve_host_client(dev, gc, &hc);
        if (st != NV_OK) {
            dm->in_use = 0;
            return st;
        }
        dev->vrpc_rm_forwarded_calls++;
        st = open_vgpu_rm_map_memory_dma(dev->rm.ctl_fd, hc, h_dev ? h_dev : hc,
                                         h_dma, h_mem, offset, length, flags, &dma_off);
        if (st != NV_OK) {
            dm->in_use = 0;
            return st;
        }
        w64(payload, 0x28, dma_off);
    }

    dm->guest_client = gc;
    dm->h_device     = h_dev;
    dm->h_dma        = h_dma;
    dm->h_memory     = h_mem;
    dm->flags        = flags;
    dm->dma_offset   = dma_off;
    dm->length       = length;
    return NV_OK;
}

static uint32_t handle_vrpc_unmap_memory_dma(gp104_vgpu_dev_t *dev, const uint8_t *payload,
                                             uint32_t plen)
{
    if (plen < 0x28U) {
        return 0xff100002U;
    }
    uint32_t gc      = r32(payload, 0x00);
    uint32_t h_dev   = r32(payload, 0x04);
    uint32_t h_dma   = r32(payload, 0x08);
    uint32_t h_mem   = r32(payload, 0x0c);
    uint32_t flags   = r32(payload, 0x10);
    uint64_t dma_off = r64(payload, 0x18);

    NV_STATUS st = NV_OK;
    if (dev->rm.attached && dev->rm.ctl_fd >= 0) {
        uint32_t hc = 0U;
        st = gp104_resolve_host_client(dev, gc, &hc);
        if (st != NV_OK) {
            return st;
        }
        dev->vrpc_rm_forwarded_calls++;
        st = open_vgpu_rm_unmap_memory_dma(dev->rm.ctl_fd, hc, h_dev ? h_dev : hc,
                                           h_dma, h_mem, flags, dma_off);
        if (st != NV_OK) {
            return st;
        }
    }

    gp104_dma_map_t *dm = find_dma_map(dev, gc, h_dma, h_mem, dma_off);
    if (dm) {
        dm->in_use = 0;
    }
    return NV_OK;
}

static uint32_t handle_vrpc_fill_pte_mem(gp104_vgpu_dev_t *dev, const uint8_t *payload,
                                         uint32_t plen)
{
    if (plen < 0x68U) {
        return 0xff100002U;
    }
    uint32_t gc       = r32(payload, 0x00);
    uint32_t h_dev    = r32(payload, 0x04);
    uint32_t page_cnt = r32(payload, 0x20);
    uint32_t flags    = r32(payload, 0x24);
    uint32_t page_sz  = r32(payload, 0x28);

    if (dev->rm.attached && dev->rm.ctl_fd >= 0) {
        uint32_t hc = 0U;
        NV_STATUS st = gp104_resolve_host_client(dev, gc, &hc);
        if (st != NV_OK) {
            return st;
        }
        uint8_t fill_params[0x60];
        memset(fill_params, 0, sizeof(fill_params));
        memcpy(fill_params + 0x00, &page_cnt, 4U);
        memcpy(fill_params + 0x10, payload + 0x14, 4U); /* hMemPTE / pageStart */
        memcpy(fill_params + 0x14, payload + 0x18, 12U);
        memcpy(fill_params + 0x20, payload + 0x38, 8U);
        memcpy(fill_params + 0x28, payload + 0x30, 8U);
        memcpy(fill_params + 0x44, payload + 0x44, 4U);
        memcpy(fill_params + 0x4c, &flags, 4U);
        memcpy(fill_params + 0x50, payload + 0x4c, 8U);
        (void)page_sz;

        dev->vrpc_rm_forwarded_calls++;
        st = open_vgpu_rm_control(dev->rm.ctl_fd, hc, h_dev ? h_dev : hc,
                                  NV0080_CTRL_CMD_DMA_FILL_PTE_MEM,
                                  fill_params, (uint32_t)sizeof(fill_params));
        if (st != NV_OK) {
            return st;
        }
    }

    dev->pte_fill_count++;
    return NV_OK;
}

static uint32_t handle_vrpc_alloc_hwres(gp104_vgpu_dev_t *dev, uint8_t *payload, uint32_t plen)
{
    if (plen < 0x78U) {
        return 0xff100002U;
    }
    uint32_t gc    = r32(payload, 0x00);
    uint32_t h_dev = r32(payload, 0x04);
    uint32_t h_mem = r32(payload, 0x08);

    if (h_mem == 0U) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    if (gp104_is_reserved_plugin_handle(dev->dev_handle, h_mem)) {
        return 0xff100007U;
    }

    if (dev->rm.attached && dev->rm.ctl_fd >= 0) {
        uint32_t hc = 0U;
        NV_STATUS st = gp104_resolve_host_client(dev, gc, &hc);
        if (st != NV_OK) {
            return st;
        }
        uint8_t hw_params[0x68];
        memcpy(hw_params, payload + 0x10U, sizeof(hw_params));
        dev->vrpc_rm_forwarded_calls++;
        st = open_vgpu_rm_alloc_obj(dev->rm.ctl_fd, hc, h_dev ? h_dev : hc,
                                    h_mem, NV01_MEMORY_HW_RESOURCES,
                                    hw_params, (uint32_t)sizeof(hw_params));
        if (st != NV_OK) {
            return st;
        }
        memcpy(payload + 0x10U, hw_params, sizeof(hw_params));
    }

    return open_vgpu_plugin_record_obj(dev, gc, h_dev ? h_dev : gc,
                                       h_mem, NV01_MEMORY_HW_RESOURCES);
}

static uint32_t handle_vrpc_deferred_api(gp104_vgpu_dev_t *dev, const uint8_t *payload,
                                         uint32_t plen)
{
    if (plen < 0x60U) {
        return 0xff100002U;
    }
    uint32_t gc    = r32(payload, 0x00);
    uint32_t h_obj = r32(payload, 0x04);
    uint32_t cmd   = r32(payload, 0x08);

    if (!is_valid_deferred_api_cmd(cmd)) {
        return NV_ERR_NOT_SUPPORTED;
    }

    if (dev->rm.attached && dev->rm.ctl_fd >= 0) {
        uint32_t hc = 0U;
        NV_STATUS st = gp104_resolve_host_client(dev, gc, &hc);
        if (st != NV_OK) {
            return st;
        }
        uint8_t dparams[0x248];
        memset(dparams, 0, sizeof(dparams));
        memcpy(dparams + 0x00, &hc, 4U);
        memcpy(dparams + 0x04, &cmd, 4U);
        size_t copy_len = (size_t)plen - 0x0cU;
        if (copy_len > 0x240U) {
            copy_len = 0x240U;
        }
        if (copy_len > 0U) {
            memcpy(dparams + 0x08, payload + 0x0cU, copy_len);
        }
        dev->vrpc_rm_forwarded_calls++;
        return open_vgpu_rm_control(dev->rm.ctl_fd, hc, h_obj ? h_obj : hc,
                                    NV5080_CTRL_CMD_DEFERRED_API_V2,
                                    dparams, (uint32_t)sizeof(dparams));
    }
    return NV_OK;
}

static uint32_t handle_vrpc_map_sema_mem(gp104_vgpu_dev_t *dev, uint8_t *payload, uint32_t plen)
{
    if (plen < 0x18U) {
        return 0xff100002U;
    }
    uint32_t gc     = r32(payload, 0x00);
    uint32_t h_dev  = r32(payload, 0x04);
    uint32_t h_mem  = r32(payload, 0x08);
    uint32_t h_vas  = r32(payload, 0x0c);
    uint64_t gpu_va = r64(payload, 0x10);

    gp104_dma_map_t *dm = alloc_dma_map(dev);
    if (!dm) {
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }

    if (dev->rm.attached && dev->rm.ctl_fd >= 0 && h_mem != 0U) {
        uint32_t hc = 0U;
        NV_STATUS st = gp104_resolve_host_client(dev, gc, &hc);
        if (st != NV_OK) {
            dm->in_use = 0;
            return st;
        }
        uint32_t h_dma = h_vas ? h_vas : dev->rm.h_subdev;
        dev->vrpc_rm_forwarded_calls++;
        st = open_vgpu_rm_map_memory_dma(dev->rm.ctl_fd, hc, h_dev ? h_dev : hc,
                                         h_dma ? h_dma : hc, h_mem, 0ULL, 0x1000ULL,
                                         0U, &gpu_va);
        if (st != NV_OK) {
            dm->in_use = 0;
            return st;
        }
        w64(payload, 0x10, gpu_va);
        dm->h_dma = h_dma ? h_dma : hc;
    } else {
        dm->h_dma = h_vas;
    }

    dm->guest_client = gc;
    dm->h_device     = h_dev;
    dm->h_memory     = h_mem;
    dm->dma_offset   = gpu_va;
    dm->length       = 0x1000ULL;
    return NV_OK;
}

static uint32_t handle_vrpc_alloc_virtmem(gp104_vgpu_dev_t *dev, uint8_t *payload, uint32_t plen)
{
    if (plen < 0x60U) {
        return 0xff100002U;
    }
    uint32_t gc        = r32(payload, 0x00);
    uint32_t h_dev     = r32(payload, 0x04);
    uint32_t h_vas     = r32(payload, 0x08);
    uint32_t h_mem     = r32(payload, 0x0c);
    uint32_t type      = r32(payload, 0x10);
    uint32_t flags     = r32(payload, 0x14);
    uint64_t size      = r64(payload, 0x30);
    uint64_t alignment = r64(payload, 0x38);
    uint64_t offset    = r64(payload, 0x40);

    if ((flags & 0x00080000U) == 0U || h_mem == 0U) {
        return NV_ERR_INVALID_ARGUMENT;
    }
    if (gp104_is_reserved_plugin_handle(dev->dev_handle, h_mem)) {
        return 0xff100007U;
    }

    gp104_mem_rec_t *rec = find_or_alloc_mem_rec(dev, gc, h_mem);
    if (!rec) {
        return NV_ERR_INSUFFICIENT_RESOURCES;
    }

    if (dev->rm.attached && dev->rm.ctl_fd >= 0) {
        uint32_t hc = 0U;
        NV_STATUS st = gp104_resolve_host_client(dev, gc, &hc);
        if (st != NV_OK) {
            if (!rec->pinned_gpfns && !rec->bar1_mapped) rec->in_use = 0;
            return st;
        }
        uint8_t virt_params[0x78];
        memset(virt_params, 0, sizeof(virt_params));
        uint32_t owner   = 0xf0f0U;
        uint32_t rm_flag = flags | 0x4000U;
        memcpy(virt_params + 0x00, &owner, 4U);
        memcpy(virt_params + 0x04, &type, 4U);
        memcpy(virt_params + 0x08, &rm_flag, 4U);
        memcpy(virt_params + 0x10, &h_vas, 4U);
        memcpy(virt_params + 0x40, &size, 8U);
        memcpy(virt_params + 0x48, &alignment, 8U);
        memcpy(virt_params + 0x50, &offset, 8U);

        dev->vrpc_rm_forwarded_calls++;
        st = open_vgpu_rm_alloc_obj(dev->rm.ctl_fd, hc, h_dev ? h_dev : hc,
                                    h_mem, NV50_MEMORY_VIRTUAL,
                                    virt_params, (uint32_t)sizeof(virt_params));
        if (st != NV_OK) {
            if (!rec->pinned_gpfns && !rec->bar1_mapped) rec->in_use = 0;
            return st;
        }
        uint64_t out_off = r64(virt_params, 0x50);
        w64(payload, 0x40, out_off);
    }

    rec->h_device = h_dev ? h_dev : gc;
    rec->h_class  = NV50_MEMORY_VIRTUAL;
    rec->size     = size;
    return open_vgpu_plugin_record_obj(dev, gc, h_dev ? h_dev : gc, h_mem, NV50_MEMORY_VIRTUAL);
}

static uint32_t handle_vrpc_update_pde2(gp104_vgpu_dev_t *dev, const uint8_t *payload,
                                        uint32_t plen)
{
    if (plen < 0x40U) {
        return 0xff100002U;
    }
    uint32_t gc    = r32(payload, 0x00);
    uint32_t h_dev = r32(payload, 0x04);

    if (dev->rm.attached && dev->rm.ctl_fd >= 0) {
        uint32_t hc = 0U;
        NV_STATUS st = gp104_resolve_host_client(dev, gc, &hc);
        if (st != NV_OK) {
            return st;
        }
        uint8_t pde_params[0x40];
        memset(pde_params, 0, sizeof(pde_params));
        memcpy(pde_params, payload + 0x08U, 0x38U);
        dev->vrpc_rm_forwarded_calls++;
        st = open_vgpu_rm_control(dev->rm.ctl_fd, hc, h_dev ? h_dev : hc,
                                  NV0080_CTRL_CMD_DMA_UPDATE_PDE_2,
                                  pde_params, (uint32_t)sizeof(pde_params));
        if (st != NV_OK) {
            return st;
        }
    }

    dev->pde_update_count++;
    return NV_OK;
}

static uint32_t handle_vrpc_update_gpu_pdes(gp104_vgpu_dev_t *dev, const uint8_t *payload,
                                            uint32_t plen)
{
    if (plen < 0x30U) {
        return 0xff100002U;
    }
    uint64_t gpu_addr = r64(payload, 0x00);
    uint32_t flags    = r32(payload, 0x08);
    uint32_t level    = r32(payload, 0x0c);
    uint32_t pde_cnt  = r32(payload, 0x10);
    uint32_t page_sz  = r32(payload, 0x14);
    uint32_t gc       = r32(payload, 0x18);
    uint32_t h_dev    = r32(payload, 0x1c);
    uint32_t h_vas    = r32(payload, 0x20);
    uint32_t is_valid = flags & 1U;

    if (dev->rm.attached && dev->rm.ctl_fd >= 0) {
        uint32_t hc = 0U;
        NV_STATUS st = gp104_resolve_host_client(dev, gc, &hc);
        if (st != NV_OK) {
            return st;
        }
        uint8_t pd_params[0x38];
        memset(pd_params, 0, sizeof(pd_params));
        memcpy(pd_params + 0x00, &gpu_addr, 8U);
        memcpy(pd_params + 0x08, &pde_cnt, 4U);
        memcpy(pd_params + 0x0c, &flags, 4U);
        memcpy(pd_params + 0x10, &h_vas, 4U);
        memcpy(pd_params + 0x14, &page_sz, 4U);
        memcpy(pd_params + 0x18, &level, 4U);
        uint32_t cmd = is_valid ? NV0080_CTRL_CMD_DMA_SET_PAGE_DIRECTORY
                                : NV0080_CTRL_CMD_DMA_UNSET_PAGE_DIRECTORY;
        dev->vrpc_rm_forwarded_calls++;
        st = open_vgpu_rm_control(dev->rm.ctl_fd, hc, h_dev ? h_dev : hc,
                                  cmd, pd_params, (uint32_t)sizeof(pd_params));
        if (st != NV_OK) {
            return st;
        }
    }

    if (is_valid) {
        dev->gpu_pde_bound_count++;
    } else if (dev->gpu_pde_bound_count > 0U) {
        dev->gpu_pde_bound_count--;
    }
    return NV_OK;
}

int gp104_vrpc_mmu_dispatch(gp104_vgpu_dev_t *dev, uint32_t fn, uint8_t *buf,
                            size_t buf_len, uint32_t tot, uint32_t *out_status)
{
    (void)buf_len;
    uint8_t *payload = buf + sizeof(open_vrpc_hdr_t);
    uint32_t plen    = (tot > sizeof(open_vrpc_hdr_t))
                           ? (tot - (uint32_t)sizeof(open_vrpc_hdr_t)) : 0U;
    uint32_t st      = 0U;

    switch (fn) {
    case VRPC_FN_ALLOC_MEMORY_535:      /* 0x04 */
        st = handle_vrpc_alloc_memory(dev, payload, plen);
        break;
    case VRPC_FN_ALLOC_CTX_DMA_535:     /* 0x05 */
        st = handle_vrpc_alloc_ctx_dma(dev, payload, plen);
        break;
    case VRPC_FN_ALLOC_CHANNEL_DMA_535: /* 0x06 */
        st = handle_vrpc_alloc_channel_dma(dev, payload, plen);
        break;
    case VRPC_FN_MAP_MEMORY_535:        /* 0x07 */
        st = handle_vrpc_map_memory(dev, payload, plen);
        break;
    case VRPC_FN_ALLOC_OBJECT_535:      /* 0x09 */
        st = handle_vrpc_alloc_object(dev, payload, plen);
        break;
    case VRPC_FN_ALLOC_VIDMEM_535:      /* 0x0c */
        st = handle_vrpc_alloc_vidmem(dev, payload, plen);
        break;
    case VRPC_FN_MAP_MEMORY_DMA:        /* 0x0e */
        st = handle_vrpc_map_memory_dma(dev, payload, plen);
        break;
    case VRPC_FN_UNMAP_MEMORY_DMA:      /* 0x0f */
        st = handle_vrpc_unmap_memory_dma(dev, payload, plen);
        break;
    case VRPC_FN_FILL_PTE_MEM:          /* 0x1b */
        st = handle_vrpc_fill_pte_mem(dev, payload, plen);
        break;
    case VRPC_FN_ALLOC_HWRES:           /* 0x1c */
        st = handle_vrpc_alloc_hwres(dev, payload, plen);
        break;
    case VRPC_FN_DEFERRED_API_535:      /* 0x21 */
        st = handle_vrpc_deferred_api(dev, payload, plen);
        break;
    case VRPC_FN_MAP_SEMA_MEM_535:      /* 0x2b */
        st = handle_vrpc_map_sema_mem(dev, payload, plen);
        break;
    case VRPC_FN_ALLOC_VIRTMEM_535:     /* 0x34 */
        st = handle_vrpc_alloc_virtmem(dev, payload, plen);
        break;
    case VRPC_FN_UPDATE_PDE_2:          /* 0x38 */
        st = handle_vrpc_update_pde2(dev, payload, plen);
        break;
    case VRPC_FN_UPDATE_GPU_PDES:       /* 0x3d */
        st = handle_vrpc_update_gpu_pdes(dev, payload, plen);
        break;
    default:
        return 0;
    }

    *out_status = st;
    return 1;
}
