#include "open_vgpu_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t mono_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0ULL;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void cfg_w16(uint8_t *cfg, size_t off, uint16_t v)
{
    cfg[off]     = (uint8_t)(v & 0xffU);
    cfg[off + 1] = (uint8_t)((v >> 8) & 0xffU);
}

static void cfg_w32(uint8_t *cfg, size_t off, uint32_t v)
{
    cfg[off]     = (uint8_t)(v & 0xffU);
    cfg[off + 1] = (uint8_t)((v >> 8) & 0xffU);
    cfg[off + 2] = (uint8_t)((v >> 16) & 0xffU);
    cfg[off + 3] = (uint8_t)((v >> 24) & 0xffU);
}

static uint32_t cfg_r32(const uint8_t *cfg, size_t off)
{
    return (uint32_t)cfg[off] | ((uint32_t)cfg[off + 1] << 8) |
           ((uint32_t)cfg[off + 2] << 16) | ((uint32_t)cfg[off + 3] << 24);
}

static void buf_w32(uint8_t *buf, size_t off, uint32_t v)
{
    cfg_w32(buf, off, v);
}

static void buf_w64(uint8_t *buf, size_t off, uint64_t v)
{
    buf_w32(buf, off, (uint32_t)(v & 0xffffffffULL));
    buf_w32(buf, off + 4U, (uint32_t)(v >> 32));
}

static uint32_t buf_r32(const uint8_t *buf, size_t off)
{
    return cfg_r32(buf, off);
}

void gp104_profile_lookup(uint32_t vgpu_type_id, gp104_profile_spec_t *out)
{
    static const gp104_profile_spec_t table[] = {
        /* 535.309.01 vgpuConfig.xml Tesla P4 GP104GL (0x10de:0x1bb3) profiles */
        { 63U, "GRID P4-1Q", 0x10deU, 0x1bb3U, 0x10deU, 0x1204U,
          1024ULL * 1024ULL * 1024ULL, 0x3b000000ULL, 256ULL * 1024ULL * 1024ULL, 60U, 1 },
        { 64U, "GRID P4-2Q", 0x10deU, 0x1bb3U, 0x10deU, 0x1205U,
          2048ULL * 1024ULL * 1024ULL, 0x76000000ULL, 256ULL * 1024ULL * 1024ULL, 60U, 1 },
        { 65U, "GRID P4-4Q", 0x10deU, 0x1bb3U, 0x10deU, 0x1206U,
          4096ULL * 1024ULL * 1024ULL, 0xec000000ULL, 256ULL * 1024ULL * 1024ULL, 60U, 1 },
        { 66U, "GRID P4-8Q", 0x10deU, 0x1bb3U, 0x10deU, 0x1207U,
          8192ULL * 1024ULL * 1024ULL, 0x1dc000000ULL, 256ULL * 1024ULL * 1024ULL, 60U, 1 },
        { 60U, "GRID P4-1B", 0x10deU, 0x1bb3U, 0x10deU, 0x1201U,
          1024ULL * 1024ULL * 1024ULL, 0x3b000000ULL, 256ULL * 1024ULL * 1024ULL, 60U, 0 },
        { 61U, "GRID P4-2B", 0x10deU, 0x1bb3U, 0x10deU, 0x1202U,
          2048ULL * 1024ULL * 1024ULL, 0x76000000ULL, 256ULL * 1024ULL * 1024ULL, 60U, 0 },
        { 67U, "GRID P4-1A", 0x10deU, 0x1bb3U, 0x10deU, 0x1208U,
          1024ULL * 1024ULL * 1024ULL, 0x3b000000ULL, 256ULL * 1024ULL * 1024ULL, 60U, 0 },
        { 68U, "GRID P4-2A", 0x10deU, 0x1bb3U, 0x10deU, 0x1209U,
          2048ULL * 1024ULL * 1024ULL, 0x76000000ULL, 256ULL * 1024ULL * 1024ULL, 60U, 0 },
        { 69U, "GRID P4-4A", 0x10deU, 0x1bb3U, 0x10deU, 0x120aU,
          4096ULL * 1024ULL * 1024ULL, 0xec000000ULL, 256ULL * 1024ULL * 1024ULL, 60U, 0 },
        { 70U, "GRID P4-8A", 0x10deU, 0x1bb3U, 0x10deU, 0x120bU,
          8192ULL * 1024ULL * 1024ULL, 0x1dc000000ULL, 256ULL * 1024ULL * 1024ULL, 60U, 0 },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].vgpu_type_id == vgpu_type_id) {
            *out = table[i];
            return;
        }
    }
    *out = table[2]; /* default to GRID P4-4Q */
    out->vgpu_type_id = vgpu_type_id ? vgpu_type_id : 65U;
}

static void gp104_init_pci_cfg(gp104_vgpu_dev_t *dev)
{
    memset(dev->pci_cfg, 0, sizeof(dev->pci_cfg));
    cfg_w16(dev->pci_cfg, 0x00, dev->profile.vendor_id);
    cfg_w16(dev->pci_cfg, 0x02, dev->profile.device_id);
    cfg_w16(dev->pci_cfg, 0x04, 0x0007U); /* I/O + Mem + Bus Master */
    cfg_w16(dev->pci_cfg, 0x06, 0x0010U); /* Capabilities List */
    dev->pci_cfg[0x08] = 0xa1U;           /* GP104 rev A1 */
    dev->pci_cfg[0x09] = 0x00U;
    dev->pci_cfg[0x0a] = 0x00U;           /* VGA Controller (observed 0x030000a1 live) */
    dev->pci_cfg[0x0b] = 0x03U;           /* Display Controller */

    cfg_w32(dev->pci_cfg, 0x10, 0x00000000U); /* BAR0: 32-bit MMIO */
    cfg_w32(dev->pci_cfg, 0x14, 0x0000000cU); /* BAR1: 64-bit prefetchable */
    cfg_w32(dev->pci_cfg, 0x18, 0x00000000U);
    cfg_w32(dev->pci_cfg, 0x1c, 0x0000000cU); /* BAR3: 64-bit prefetchable */
    cfg_w32(dev->pci_cfg, 0x20, 0x00000000U);
    cfg_w32(dev->pci_cfg, 0x24, 0x00000000U);

    cfg_w16(dev->pci_cfg, 0x2c, dev->profile.subsys_vendor_id);
    cfg_w16(dev->pci_cfg, 0x2e, dev->profile.subsys_device_id);

    dev->pci_cfg[0x34] = 0xd0U; /* NVIDIA vGPU vendor cap -> MSI at 0x68 */
    dev->pci_cfg[0x3d] = 0x01U; /* INTA# */

    /* Live-observed internal config words at 0x50..0x58 */
    cfg_w32(dev->pci_cfg, 0x50, 0x00000001U);
    cfg_w32(dev->pci_cfg, 0x54, 0x00000001U);
    cfg_w32(dev->pci_cfg, 0x58, 0x0023d6ceU);

    /* MSI Cap at 0x68 -> next 0x00 (64-bit capable) */
    dev->pci_cfg[0x68] = 0x05U;
    dev->pci_cfg[0x69] = 0x00U;
    cfg_w16(dev->pci_cfg, 0x6a, 0x0080U);

    /* NVIDIA vGPU Vendor Cap at 0xd0 (27 bytes: VF\0\x16 + version tag) */
    dev->pci_cfg[0xd0] = 0x09U;
    dev->pci_cfg[0xd1] = 0x68U;
    dev->pci_cfg[0xd2] = 0x1bU;
    dev->pci_cfg[0xd3] = 0x56U; /* 'V' */
    dev->pci_cfg[0xd4] = 0x46U; /* 'F' */
    dev->pci_cfg[0xd5] = 0x00U;
    dev->pci_cfg[0xd6] = 0x16U;
    memcpy(dev->pci_cfg + 0xd7, "535.309.01r539_72", 17U);
}

void gp104_dev_init_defaults(gp104_vgpu_dev_t *dev, uint32_t dev_handle, uint32_t vgpu_type_id)
{
    memset(dev, 0, sizeof(*dev));
    dev->dev_handle = dev_handle;
    gp104_profile_lookup(vgpu_type_id, &dev->profile);
    gp104_init_pci_cfg(dev);

    dev->pmc_boot0 = 0x134000a1U;  /* GP104 A1 */
    dev->pmc_boot42 = 0x134a1000U;
    dev->pmc_enable = 0x00010100U;
    dev->timer_base_ns = mono_ns();

    /* VGPU_CONFIG (+0x20: 0x040882f1 observed live on P4-4Q) & FB pages-1 (+0x24) */
    dev->vgpu_regs[NV_VGPU_REG_CONFIG >> 2] = 0x040882f1U;
    dev->vgpu_regs[NV_VGPU_REG_FB_PAGES_M1 >> 2] =
        (uint32_t)((dev->profile.usable_fb_bytes - 1ULL) >> 12);

    dev->chid_base = ((dev_handle ? (dev_handle - 1U) : 0U) * GP104_MAX_CHANNELS) % 4096U;
    dev->frl_fps = dev->profile.frl_fps ? dev->profile.frl_fps : 60U;
    dev->frl_interval_ns = 1000000000ULL / dev->frl_fps;
}

int gp104_vmmu_map_range(gp104_vgpu_dev_t *dev, uint64_t phys_start, uint64_t len)
{
    int mapped = 0;
    uint64_t off = 0ULL;
    while (off < len && dev->vmmu_seg_count < GP104_MAX_VMMU_SEGS) {
        uint64_t step = len - off;
        if (step > GP104_VMMU_SEG_BYTES) step = GP104_VMMU_SEG_BYTES;
        dev->vmmu_seg_phys[dev->vmmu_seg_count++] = phys_start + off;
        off += step;
        mapped++;
    }
    return mapped;
}

int gp104_chid_alloc(gp104_vgpu_dev_t *dev, uint32_t guest_chid, uint32_t *out_phys_chid)
{
    if (!dev || guest_chid >= GP104_MAX_CHANNELS) return -1;
    dev->chid_bitmap[guest_chid] = 1U;
    if (out_phys_chid) *out_phys_chid = dev->chid_base + guest_chid;
    return 0;
}

void gp104_chid_free(gp104_vgpu_dev_t *dev, uint32_t guest_chid)
{
    if (dev && guest_chid < GP104_MAX_CHANNELS) {
        dev->chid_bitmap[guest_chid] = 0U;
    }
}

static void unmap_ring(gp104_ring_map_t *ring)
{
    if (ring->mapped && ring->mem_id != 0U) {
        vmiop_unmap_guest_memory(ring->mem_id);
    }
    memset(ring, 0, sizeof(*ring));
}

uint32_t gp104_dev_start(gp104_vgpu_dev_t *dev, const vmiop_env_v2_t *env)
{
    (void)env;
    char *val = NULL;
    if (vmiop_config_get(dev->dev_handle, "frame_rate_limiter", &val) == VMIOP_SUCCESS && val) {
        int en = atoi(val);
        if (en == 0) {
            dev->frl_fps = 0U;
            dev->frl_interval_ns = 0ULL;
        }
        free(val);
    }

    uint32_t out_emul = 0U;
    uint32_t rc = vmiop_register_emul_device_v2(dev->dev_handle, dev, (void *)gp104_pci_cfg_access,
                                                "gp104_pci", &out_emul);
    if (rc != VMIOP_SUCCESS) return rc;

    rc = vmiop_create_guest_region_v2(dev->dev_handle, GP104_BAR0_SIZE, (void *)gp104_bar0_access,
                                      dev, VGPU_EMUL_MMIO, &dev->bar0_region_id);
    if (rc != VMIOP_SUCCESS) return rc;

    rc = vmiop_create_guest_region_v2(dev->dev_handle, dev->profile.bar1_bytes,
                                      (void *)gp104_bar0_access, dev, VGPU_EMUL_MMIO,
                                      &dev->bar1_region_id);
    if (rc != VMIOP_SUCCESS) return rc;

    rc = vmiop_create_guest_region_v2(dev->dev_handle, GP104_BAR3_SIZE,
                                      (void *)gp104_bar0_access, dev, VGPU_EMUL_MMIO,
                                      &dev->bar3_region_id);
    if (rc != VMIOP_SUCCESS) return rc;

    gp104_vmmu_map_range(dev, 0ULL, dev->profile.profile_bytes);
    dev->initialized = 1;
    return VMIOP_SUCCESS;
}

void gp104_dev_reset(gp104_vgpu_dev_t *dev)
{
    unmap_ring(&dev->msg_page);
    unmap_ring(&dev->send_ring);
    unmap_ring(&dev->recv_ring);
    unmap_ring(&dev->event_ring);
    dev->guest_vgx_initialized = 0;
    dev->pmc_intr_0 = 0U;
    dev->pmc_intr_en_0 = 0U;
    memset(dev->chid_bitmap, 0, sizeof(dev->chid_bitmap));
    memset(dev->rm_objs, 0, sizeof(dev->rm_objs));
    gp104_init_pci_cfg(dev);
}

void gp104_dev_shutdown(gp104_vgpu_dev_t *dev)
{
    unmap_ring(&dev->msg_page);
    unmap_ring(&dev->send_ring);
    unmap_ring(&dev->recv_ring);
    unmap_ring(&dev->event_ring);
    if (dev->dev_handle) {
        vmiop_unregister_emul_device(dev->dev_handle);
    }
    dev->initialized = 0;
}

static uint32_t bar_mask_lo(uint64_t size, uint32_t type_bits)
{
    return (uint32_t)(~((uint32_t)(size - 1ULL))) | type_bits;
}

static uint32_t bar_mask_hi(uint64_t size)
{
    return (uint32_t)((~(size - 1ULL)) >> 32);
}

static void relocate_bars_from_cfg(gp104_vgpu_dev_t *dev, uint32_t reg_off)
{
    uint16_t cmd = (uint16_t)(dev->pci_cfg[0x04] | ((uint16_t)dev->pci_cfg[0x05] << 8));
    if ((cmd & 2U) == 0U) return;

    if (reg_off == 0x10U && dev->bar0_region_id) {
        uint32_t raw = cfg_r32(dev->pci_cfg, 0x10);
        if (raw != 0U && raw != 0xfffffff0U) {
            uint64_t base = (uint64_t)(raw & 0xfffffff0U);
            if (base != dev->bar0_gpa) {
                dev->bar0_gpa = base;
                vmiop_relocate_guest_region(dev->bar0_region_id, base);
            }
        }
    } else if ((reg_off == 0x14U || reg_off == 0x18U) && dev->bar1_region_id) {
        uint32_t lo = cfg_r32(dev->pci_cfg, 0x14);
        uint32_t hi = cfg_r32(dev->pci_cfg, 0x18);
        if ((lo & 0xfffffff0U) != 0U && lo != bar_mask_lo(dev->profile.bar1_bytes, 0x0cU)) {
            uint64_t base = ((uint64_t)hi << 32) | (uint64_t)(lo & 0xfffffff0U);
            if (base != dev->bar1_gpa) {
                dev->bar1_gpa = base;
                vmiop_relocate_guest_region(dev->bar1_region_id, base);
            }
        }
    } else if ((reg_off == 0x1cU || reg_off == 0x20U) && dev->bar3_region_id) {
        uint32_t lo = cfg_r32(dev->pci_cfg, 0x1c);
        uint32_t hi = cfg_r32(dev->pci_cfg, 0x20);
        if ((lo & 0xfffffff0U) != 0U && lo != bar_mask_lo(GP104_BAR3_SIZE, 0x0cU)) {
            uint64_t base = ((uint64_t)hi << 32) | (uint64_t)(lo & 0xfffffff0U);
            if (base != dev->bar3_gpa) {
                dev->bar3_gpa = base;
                vmiop_relocate_guest_region(dev->bar3_region_id, base);
            }
        }
    }
}

uint32_t gp104_pci_cfg_access(void *priv, uint32_t is_write, uint32_t emul_space,
                              uint64_t offset, uint32_t width, void *data, uint32_t *dummy)
{
    (void)emul_space;
    if (dummy) *dummy = 0U;
    gp104_vgpu_dev_t *dev = (gp104_vgpu_dev_t *)priv;
    if (!dev || !data || (width != 1U && width != 2U && width != 4U) ||
        offset + width > GP104_PCI_CFG_BYTES) {
        return VMIOP_ERROR_INVAL;
    }

    if (!is_write) {
        uint32_t val = 0U;
        memcpy(&val, dev->pci_cfg + offset, width);
        memcpy(data, &val, width);
        return VMIOP_SUCCESS;
    }

    uint32_t val = 0U;
    memcpy(&val, data, width);

    /* Read-only identity registers */
    if ((offset <= 0x03ULL) || (offset >= 0x08ULL && offset <= 0x0bULL) ||
        (offset >= 0x2cULL && offset <= 0x2fULL)) {
        return VMIOP_SUCCESS;
    }

    if (width == 4U && val == 0xffffffffU) {
        if (offset == 0x10ULL) {
            cfg_w32(dev->pci_cfg, 0x10, bar_mask_lo(GP104_BAR0_SIZE, 0x00U));
            return VMIOP_SUCCESS;
        }
        if (offset == 0x14ULL) {
            cfg_w32(dev->pci_cfg, 0x14, bar_mask_lo(dev->profile.bar1_bytes, 0x0cU));
            return VMIOP_SUCCESS;
        }
        if (offset == 0x18ULL) {
            cfg_w32(dev->pci_cfg, 0x18, bar_mask_hi(dev->profile.bar1_bytes));
            return VMIOP_SUCCESS;
        }
        if (offset == 0x1cULL) {
            cfg_w32(dev->pci_cfg, 0x1c, bar_mask_lo(GP104_BAR3_SIZE, 0x0cU));
            return VMIOP_SUCCESS;
        }
        if (offset == 0x20ULL) {
            cfg_w32(dev->pci_cfg, 0x20, bar_mask_hi(GP104_BAR3_SIZE));
            return VMIOP_SUCCESS;
        }
        if (offset == 0x24ULL || offset == 0x30ULL) {
            cfg_w32(dev->pci_cfg, (size_t)offset, 0U);
            return VMIOP_SUCCESS;
        }
    }

    if ((offset >= 0x04ULL && offset <= 0x07ULL) ||
        (offset >= 0x10ULL && offset <= 0x27ULL) ||
        (offset >= 0x3cULL && offset <= 0x3dULL) ||
        (offset >= 0x62ULL && offset <= 0x77ULL) ||
        (offset >= 0x7cULL)) {
        memcpy(dev->pci_cfg + offset, &val, width);
        if (offset >= 0x10ULL && offset <= 0x23ULL) {
            relocate_bars_from_cfg(dev, (uint32_t)(offset & ~3ULL));
        }
    }
    return VMIOP_SUCCESS;
}

static uint32_t update_ring_mapping(gp104_vgpu_dev_t *dev, gp104_ring_map_t *ring,
                                    uint32_t lo, uint32_t hi)
{
    uint64_t gpa = (((uint64_t)hi & 0xfffffULL) << 32) | (uint64_t)(lo & 0xfffff000U);
    if ((lo & 8U) == 0U) {
        unmap_ring(ring);
        return VMIOP_SUCCESS;
    }
    if (ring->mapped && ring->gpa == gpa) {
        return VMIOP_SUCCESS;
    }
    unmap_ring(ring);
    if ((lo & 3U) == 1U || gpa == 0ULL) {
        ring->gpa = gpa;
        return VMIOP_SUCCESS;
    }
    void *hva = NULL;
    uint32_t mem_id = 0U;
    uint32_t rc = vmiop_map_guest_memory_v2(dev->dev_handle, gpa, 0x1000ULL, &hva, 3U, &mem_id);
    if (rc == VMIOP_SUCCESS) {
        ring->gpa    = gpa;
        ring->size   = 0x1000ULL;
        ring->hva    = hva;
        ring->mem_id = mem_id;
        ring->mapped = 1;
    }
    return rc;
}

static uint32_t record_rm_object(gp104_vgpu_dev_t *dev, uint32_t hc, uint32_t hp,
                                 uint32_t ho, uint32_t cls)
{
    for (size_t i = 0; i < GP104_MAX_RM_OBJECTS; i++) {
        if (dev->rm_objs[i].in_use && dev->rm_objs[i].h_object == ho &&
            dev->rm_objs[i].h_client == hc) {
            return 0U;
        }
    }
    for (size_t i = 0; i < GP104_MAX_RM_OBJECTS; i++) {
        if (!dev->rm_objs[i].in_use) {
            dev->rm_objs[i].in_use = 1;
            dev->rm_objs[i].h_client = hc;
            dev->rm_objs[i].h_parent = hp;
            dev->rm_objs[i].h_object = ho;
            dev->rm_objs[i].h_class = cls;
            return 0U;
        }
    }
    return 0x1eU; /* NV_ERR_INSUFFICIENT_RESOURCES */
}

static uint32_t free_rm_object(gp104_vgpu_dev_t *dev, uint32_t hc, uint32_t ho)
{
    for (size_t i = 0; i < GP104_MAX_RM_OBJECTS; i++) {
        if (dev->rm_objs[i].in_use && dev->rm_objs[i].h_client == hc &&
            dev->rm_objs[i].h_object == ho) {
            dev->rm_objs[i].in_use = 0;
            return 0U;
        }
    }
    return 0U;
}

uint32_t gp104_handle_vrpc_buffer(gp104_vgpu_dev_t *dev, uint8_t *buf, size_t buf_len)
{
    if (!dev || !buf || buf_len < sizeof(open_vrpc_hdr_t)) return 0x1fU;
    uint32_t sig = buf_r32(buf, 0x04);
    uint32_t tot = buf_r32(buf, 0x08);
    uint32_t fn  = buf_r32(buf, 0x0c);
    if (sig != OPEN_VRPC_SIGNATURE || tot < sizeof(open_vrpc_hdr_t) || (size_t)tot > buf_len) {
        buf_w32(buf, 0x04, OPEN_VRPC_SIGNATURE);
        buf_w32(buf, 0x10, 0xff100002U);
        return 0xff100002U;
    }

    dev->vrpc_total_calls++;
    if (fn < VRPC_FN_MAX_OPCODE) {
        dev->vrpc_opcode_counts[fn]++;
    }

    /* Check Linux/vGPU opcode validity per _nv007236vgpu (0x1c4810.c) */
    if (fn == 0x41U || fn == 0x4aU || fn == 0x4cU || fn == 0x67U ||
        (fn > 0x9fU && (fn - 0xa7U) > 0x24U)) {
        buf_w32(buf, 0x10, 0x56U); /* NV_ERR_NOT_SUPPORTED */
        return 0x56U;
    }

    if (!dev->guest_vgx_initialized &&
        fn != VRPC_FN_SET_GUEST_SYSTEM_INFO &&
        fn != VRPC_FN_ALLOC_CHANNEL_DMA &&
        fn != 0x2fU) {
        buf_w32(buf, 0x10, 0x56U);
        return 0x56U;
    }

    uint32_t status = 0U;
    switch (fn) {
    case VRPC_FN_NOP:
        status = 0U;
        break;
    case VRPC_FN_SET_GUEST_SYSTEM_INFO:
        if (dev->guest_vgx_initialized) {
            status = 0x40U; /* NV_ERR_INVALID_STATE */
        } else {
            if (tot >= 0x2cU) {
                dev->guest_vgx_major = buf_r32(buf, 0x20);
                dev->guest_vgx_minor = buf_r32(buf, 0x24);
                dev->guest_os_type   = buf_r32(buf, 0x28);
            }
            dev->guest_vgx_initialized = 1;
            status = 0U;
        }
        break;
    case VRPC_FN_ALLOC_ROOT: {
        uint32_t hc = (tot >= 0x24U) ? buf_r32(buf, 0x20) : 0xc1d00001U;
        status = record_rm_object(dev, hc, hc, hc, 0U);
        break;
    }
    case VRPC_FN_ALLOC_DEVICE_535:
    case VRPC_FN_ALLOC_DEVICE:
    case VRPC_FN_ALLOC_SUBDEVICE:
    case VRPC_FN_ALLOC_MEMORY:
    case VRPC_FN_ALLOC_OBJECT: {
        uint32_t hc  = (tot >= 0x24U) ? buf_r32(buf, 0x20) : 0U;
        uint32_t hp  = (tot >= 0x28U) ? buf_r32(buf, 0x24) : 0U;
        uint32_t ho  = (tot >= 0x2cU) ? buf_r32(buf, 0x28) : 0U;
        uint32_t cls = (tot >= 0x30U) ? buf_r32(buf, 0x2c) : fn;
        status = record_rm_object(dev, hc, hp, ho, cls);
        break;
    }
    case VRPC_FN_FREE: {
        uint32_t hc = (tot >= 0x24U) ? buf_r32(buf, 0x20) : 0U;
        uint32_t ho = (tot >= 0x28U) ? buf_r32(buf, 0x24) : 0U;
        status = free_rm_object(dev, hc, ho);
        break;
    }
    case VRPC_FN_ALLOC_CHANNEL_DMA: {
        uint32_t gchid = (tot >= 0x2cU) ? (buf_r32(buf, 0x28) % GP104_MAX_CHANNELS) : 0U;
        uint32_t pchid = 0U;
        if (gp104_chid_alloc(dev, gchid, &pchid) == 0 && tot >= 0x34U) {
            buf_w32(buf, 0x30, pchid);
        }
        status = 0U;
        break;
    }
    case VRPC_FN_GET_STATIC_INFO_535:
    case VRPC_FN_GET_CONSOL_STATIC_INFO:
    case VRPC_FN_GET_STATIC_INFO:
        if (buf_len >= 0x38U) {
            buf_w64(buf, 0x20, dev->profile.usable_fb_bytes);
            buf_w32(buf, 0x28, ((uint32_t)dev->profile.subsys_device_id << 16) |
                                   dev->profile.device_id);
            buf_w32(buf, 0x2c, dev->profile.vgpu_type_id);
            buf_w64(buf, 0x30, dev->profile.bar1_bytes);
        }
        status = 0U;
        break;
    case VRPC_FN_CONTROL:
    case VRPC_FN_MAP_MEMORY:
    case VRPC_FN_UNMAP_MEMORY:
    case VRPC_FN_MAP_MEMORY_DMA:
    case VRPC_FN_UNMAP_MEMORY_DMA:
    case VRPC_FN_BIND_CONTEXT_DMA:
    case VRPC_FN_SET_OBJECT:
    case VRPC_FN_IDLE_CHANNELS:
    case VRPC_FN_OP_15:
    case VRPC_FN_IDLE_CHANNELS_535:
    case VRPC_FN_OP_17:
    case VRPC_FN_RM_API_CONTROL:
    case VRPC_FN_OP_1B:
    case VRPC_FN_OP_1C:
    case VRPC_FN_GET_ENGINE_INFO:
    case VRPC_FN_SET_GUEST_FB_INFO:
    case VRPC_FN_SET_GUEST_SEMA_MEM:
    case VRPC_FN_UNSET_GUEST_SEMA_MEM:
    case VRPC_FN_SET_GUEST_BAR1_INFO:
    case VRPC_FN_LOG:
    case VRPC_FN_OP_2B:
    case VRPC_FN_OP_34:
    case VRPC_FN_OP_38:
    case VRPC_FN_OP_39:
    case VRPC_FN_OP_3B:
    case VRPC_FN_OP_3D:
    case VRPC_FN_OP_3E:
    case VRPC_FN_SET_GUEST_SYSTEM_EXT:
    case VRPC_FN_SET_GUEST_BRANCH_VER:
    case VRPC_FN_BATCH_50:
    case VRPC_FN_UPDATE_BAR_INFO:
    case VRPC_FN_OP_5B:
    case VRPC_FN_OP_5C:
    case VRPC_FN_OP_61:
    case VRPC_FN_OP_62:
    case VRPC_FN_OP_6E:
    case VRPC_FN_OP_70:
    case VRPC_FN_OP_71:
    case VRPC_FN_OP_72:
    case VRPC_FN_OP_78:
    case VRPC_FN_OP_79:
    case VRPC_FN_OP_7A:
    case VRPC_FN_OP_7C:
    case VRPC_FN_OP_7D:
    case VRPC_FN_OP_7E:
    case VRPC_FN_OP_8E:
    case VRPC_FN_OP_8F:
    case VRPC_FN_SYNC_9C:
        status = 0U;
        break;
    default:
        status = 0x56U; /* NV_ERR_NOT_SUPPORTED */
        break;
    }

    buf_w32(buf, 0x10, status);
    return status;
}

static void handle_doorbell_dispatch(gp104_vgpu_dev_t *dev)
{
    if (dev->msg_page.mapped && dev->msg_page.hva) {
        gp104_handle_vrpc_buffer(dev, (uint8_t *)dev->msg_page.hva, (size_t)dev->msg_page.size);
    } else if (dev->send_ring.mapped && dev->send_ring.hva) {
        /* Fallback when MSG_PAGE was mapped directly on SEND_RING entry 0 or pre-bound */
        uint32_t sig = buf_r32((const uint8_t *)dev->send_ring.hva, 0x04);
        if (sig == OPEN_VRPC_SIGNATURE) {
            gp104_handle_vrpc_buffer(dev, (uint8_t *)dev->send_ring.hva,
                                     (size_t)dev->send_ring.size);
        } else {
            uint32_t msg_lo = buf_r32((const uint8_t *)dev->send_ring.hva, 0x00);
            uint32_t msg_hi = buf_r32((const uint8_t *)dev->send_ring.hva, 0x04);
            if ((msg_lo & 8U) != 0U &&
                update_ring_mapping(dev, &dev->msg_page, msg_lo, msg_hi) == VMIOP_SUCCESS &&
                dev->msg_page.hva) {
                gp104_handle_vrpc_buffer(dev, (uint8_t *)dev->msg_page.hva,
                                         (size_t)dev->msg_page.size);
                if (dev->recv_ring.mapped && dev->recv_ring.hva) {
                    memcpy(dev->recv_ring.hva, dev->send_ring.hva, 8U);
                }
            }
        }
    }

    if (vmiop_display_vmiop_env.inject_msi) {
        vmiop_display_vmiop_env.inject_msi(dev->dev_handle, 0ULL, 0U);
        dev->msi_injected_count++;
    }
}

uint32_t gp104_bar0_access(void *priv, uint32_t is_write, uint32_t emul_space,
                           uint64_t offset, uint32_t width, void *data, uint32_t *dummy)
{
    (void)emul_space;
    if (dummy) *dummy = 0U;
    gp104_vgpu_dev_t *dev = (gp104_vgpu_dev_t *)priv;
    if (!dev || !data || width != 4U) {
        return VMIOP_ERROR_INVAL;
    }
    if (dev->bar0_gpa != 0ULL && offset >= dev->bar0_gpa &&
        offset < dev->bar0_gpa + GP104_BAR0_SIZE) {
        offset -= dev->bar0_gpa;
    }
    if ((offset & 3ULL) != 0ULL) {
        return VMIOP_ERROR_INVAL;
    }

    uint32_t val = 0U;
    if (is_write) memcpy(&val, data, 4U);

    if (offset >= GP104_NV_VGPU_BASE && offset < GP104_NV_VGPU_BASE + GP104_NV_VGPU_SIZE) {
        uint32_t reg = (uint32_t)(offset - GP104_NV_VGPU_BASE);
        uint32_t idx = reg >> 2;
        if (idx >= 0x40U) {
            if (!is_write) memset(data, 0, 4U);
            return VMIOP_SUCCESS;
        }
        if (!is_write) {
            uint32_t r = dev->vgpu_regs[idx];
            if (reg == NV_VGPU_REG_GUEST_INFO) r = 0U;
            memcpy(data, &r, 4U);
            return VMIOP_SUCCESS;
        }
        switch (reg) {
        case NV_VGPU_REG_SEND_RING_LO:
        case NV_VGPU_REG_SEND_RING_HI:
            dev->vgpu_regs[idx] = val;
            return update_ring_mapping(dev, &dev->send_ring,
                                       dev->vgpu_regs[NV_VGPU_REG_SEND_RING_LO >> 2],
                                       dev->vgpu_regs[NV_VGPU_REG_SEND_RING_HI >> 2]);
        case NV_VGPU_REG_RECV_RING_LO:
        case NV_VGPU_REG_RECV_RING_HI:
            dev->vgpu_regs[idx] = val;
            return update_ring_mapping(dev, &dev->recv_ring,
                                       dev->vgpu_regs[NV_VGPU_REG_RECV_RING_LO >> 2],
                                       dev->vgpu_regs[NV_VGPU_REG_RECV_RING_HI >> 2]);
        case NV_VGPU_REG_EVENT_RING_LO:
        case NV_VGPU_REG_EVENT_RING_HI:
            dev->vgpu_regs[idx] = val;
            return update_ring_mapping(dev, &dev->event_ring,
                                       dev->vgpu_regs[NV_VGPU_REG_EVENT_RING_LO >> 2],
                                       dev->vgpu_regs[NV_VGPU_REG_EVENT_RING_HI >> 2]);
        case NV_VGPU_REG_SEND_RING_PUT:
        case NV_VGPU_REG_RECV_RING_GET:
            dev->vgpu_regs[idx] = val & 0xfffffff8U;
            return VMIOP_SUCCESS;
        case NV_VGPU_REG_EVENT_RING_GET:
            dev->vgpu_regs[idx] = val & 0xfffffffcU;
            return VMIOP_SUCCESS;
        case NV_VGPU_REG_CONFIG:
            dev->vgpu_regs[idx] = (dev->vgpu_regs[idx] & ~0x40000U) | (val & 0x40000U);
            return VMIOP_SUCCESS;
        case NV_VGPU_REG_INTR_STATUS:
            dev->vgpu_regs[idx] &= ~(val & 0x3c0U);
            return VMIOP_SUCCESS;
        case NV_VGPU_REG_INTR_ENABLE:
            dev->vgpu_regs[idx] = val & 0x1ffU;
            return VMIOP_SUCCESS;
        case NV_VGPU_REG_RPC_DOORBELL:
            dev->vgpu_regs[idx] = val;
            handle_doorbell_dispatch(dev);
            return VMIOP_SUCCESS;
        case NV_VGPU_REG_MSG_RING_CTL:
            dev->vgpu_regs[idx] = val;
            if ((val & 1U) != 0U) {
                unmap_ring(&dev->msg_page);
            }
            return VMIOP_SUCCESS;
        case NV_VGPU_REG_GUEST_ID_58:
        case NV_VGPU_REG_CTL_68:
        case NV_VGPU_REG_RING2_LO:
        case NV_VGPU_REG_RING2_HI:
        case NV_VGPU_REG_RING3_LO:
        case NV_VGPU_REG_RING3_HI:
        case NV_VGPU_REG_RING4_LO:
        case NV_VGPU_REG_RING4_HI:
            dev->vgpu_regs[idx] = val;
            return VMIOP_SUCCESS;
        default:
            dev->vgpu_regs[idx] = val;
            return VMIOP_SUCCESS;
        }
    }

    if (!is_write) {
        uint32_t out = 0U;
        switch (offset) {
        case 0x000000ULL: out = dev->pmc_boot0; break;
        case 0x000004ULL: out = dev->pmc_enable; break;
        case 0x000100ULL:
        case 0x000104ULL: out = dev->pmc_intr_0; break;
        case 0x000140ULL:
        case 0x000144ULL: out = dev->pmc_intr_en_0; break;
        case 0x000a00ULL: out = dev->pmc_boot42; break;
        case 0x009100ULL: out = 0U; break;
        case 0x009400ULL: { /* NV_PTIMER_TIME_0 */
            uint64_t t = mono_ns() - dev->timer_base_ns;
            out = (uint32_t)(t & 0xffffffffULL);
            break;
        }
        case 0x009410ULL: { /* NV_PTIMER_TIME_1 */
            uint64_t t = mono_ns() - dev->timer_base_ns;
            out = (uint32_t)(t >> 32);
            break;
        }
        case 0x088068ULL: out = 0x00810005U; break;
        case 0x088080ULL:
        case 0x08814cULL:
        case 0x088424ULL:
        case 0x088430ULL:
        case 0x021584ULL:
        case 0x070004ULL:
        case 0x070010ULL:
        case 0x9a0204ULL:
            out = 0U;
            break;
        case 0x10020cULL:
        case 0x10f20cULL:
        case 0x11020cULL:
        case 0x11120cULL:
        case 0x11220cULL:
        case 0x11320cULL:
            out = (uint32_t)(dev->profile.usable_fb_bytes >> 20);
            break;
        case 0x500c30ULL:
        case 0x508c30ULL:
        case 0x510c30ULL:
        case 0x518c30ULL:
            out = 0x000f0000U;
            break;
        default:
            out = 0U;
            break;
        }
        memcpy(data, &out, 4U);
        return VMIOP_SUCCESS;
    }

    if (offset == 0x000004ULL) {
        dev->pmc_enable = val | 0x00010100U;
    } else if (offset == 0x000140ULL || offset == 0x000144ULL) {
        dev->pmc_intr_en_0 = val;
    }
    return VMIOP_SUCCESS;
}
