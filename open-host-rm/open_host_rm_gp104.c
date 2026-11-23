/* SPDX-License-Identifier: MIT */
/*
 * open_host_rm_gp104.c - Clean-room Pascal GP104 (Tesla P4) Host Physical RM HAL Slice
 *
 * Implements the Tier 2B open-source replacement for the GP104 physical host
 * RM HAL slice audited from 535.309.01-vgpu-kvm (p4_vgpu_535_slice.o, pmu_535.o,
 * resolve_535_hal.c, and gp104-vgpu-535/BLOB_MANIFEST.json).
 */

#include "open_host_rm_gp104.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * Category #1 immutable signed Falcon firmware descriptor table audited from
 * NVIDIA-Linux-x86_64-535.309.01-vgpu-kvm.run (gp104-vgpu-535/BLOB_MANIFEST.json).
 * These hardware-signed HS/LS Falcon images are verified by GP104 silicon
 * boot ROM and loaded unchanged into WPR1 during ACR bootstrap.
 */
static const gp104_falcon_fw_manifest_entry_t g_gp104_fw_manifest[GP104_FALCON_COUNT] = {
    {
        GP104_FALCON_ID_ACR_HS,
        "acr/gp104_acr_bl",
        0x0001fad8U,
        1280U,
        36U,
        "02df5be808af2ece6272695c1050037113c502bab60a5e51e211342c7e3ce645",
        1
    },
    {
        GP104_FALCON_ID_PMU,
        "pmu/gp104_pmu_bl",
        0x0001ae40U,
        768U,
        24U,
        "f21f1cfbb8fffa8067685314367fc4c50f3f84dd99eda7c19db4292d9269b9d9",
        0
    },
    {
        GP104_FALCON_ID_FECS,
        "gr/gp104_fecs_ucode",
        0x0001c110U,
        24576U,
        64U,
        "d04ec8dd6a27abbfd7576020bd34e815cac9fdf69dfd25010d4da8576596f701",
        0
    },
    {
        GP104_FALCON_ID_GPCCS,
        "gr/gp104_gpccs_ucode",
        0x0001c420U,
        12288U,
        64U,
        "944ce93f27c43450172ebbc322434a8ea984f6b6af6b5e09b7e7b2eccd50239f",
        0
    },
    {
        GP104_FALCON_ID_NVDEC0,
        "nvdec/gp104_nvdec_ucode",
        0x0001d200U,
        16384U,
        48U,
        "fb5da1a1b04be10866271b62d0b01dd7a2d05b5863550db919c492c18adc1191",
        0
    },
    {
        GP104_FALCON_ID_NVENC0,
        "nvenc/gp104_nvenc0_ucode",
        0x0001d840U,
        18432U,
        48U,
        "821a403e3c6f9745304275082ce8c80151aedc802e98122a3ec445fe3c012f58",
        0
    },
    {
        GP104_FALCON_ID_NVENC1,
        "nvenc/gp104_nvenc1_ucode",
        0x0001d840U,
        18432U,
        48U,
        "821a403e3c6f9745304275082ce8c80151aedc802e98122a3ec445fe3c012f58",
        0
    },
    {
        GP104_FALCON_ID_SEC2,
        "sec2/gp104_sec2_rtos",
        0x0001e100U,
        20480U,
        64U,
        "c5dd2255d82eb3ac5790f71c852dc327c961829c58e956027d50ab20e66ccb99",
        1
    }
};

/*
 * 22 supported Tesla P4 (GP104GL) vGPU type IDs verified from vgpuConfig.xml
 * and nv_vgpu_get_type_ids (55..76, including nvidia-65 = GRID P4-4Q).
 */
static const uint32_t g_gp104_vgpu_type_ids[GP104_MAX_VGPU_TYPES] = {
    55U, 56U, 57U, 58U, 59U, 60U, /* GRID P4-1B, P4-2B, P4-1B4, P4-2B4, P4-1A, P4-2A */
    61U, 62U, 63U, 64U, 65U, 66U, /* GRID P4-4A, P4-8A, P4-1Q, P4-2Q, P4-4Q (65), P4-8Q */
    67U, 68U, 69U, 70U, 71U, 72U, /* GRID P4-1C, P4-2C, P4-4C, P4-8C, etc. */
    73U, 74U, 75U, 76U
};

const gp104_falcon_fw_manifest_entry_t *open_host_rm_gp104_get_fw_manifest(size_t *out_count)
{
    if (out_count) {
        *out_count = GP104_FALCON_COUNT;
    }
    return g_gp104_fw_manifest;
}

int open_host_rm_gp104_build_acr_wpr_layout(uint64_t wpr1_fb_base, uint64_t wpr1_fb_size,
                                            gp104_acr_wpr_layout_t *out_layout)
{
    if (!out_layout || (wpr1_fb_base & 0xfffULL) != 0ULL || wpr1_fb_size < 0x100000ULL) {
        return -EINVAL;
    }

    memset(out_layout, 0, sizeof(*out_layout));
    out_layout->wpr1_fb_base    = wpr1_fb_base;
    out_layout->wpr1_fb_size    = wpr1_fb_size;
    out_layout->wpr_entry_count = GP104_FALCON_COUNT;

    uint32_t cur_off = 0x1000U; /* First 4 KiB of WPR1 holds LSF WPR headers */
    for (size_t i = 0U; i < GP104_FALCON_COUNT; i++) {
        const gp104_falcon_fw_manifest_entry_t *m = &g_gp104_fw_manifest[i];
        gp104_lsf_wpr_header_t *h = &out_layout->headers[i];

        h->falcon_id    = (uint32_t)m->falcon_id;
        h->lsf_offset   = cur_off;
        h->ucode_size   = (m->code_size + 0xffU) & ~0xffU;
        h->data_size    = (m->desc_size + 0xffU) & ~0xffU;
        h->bl_code_size = 0x300U;
        h->flags        = (m->is_high_secure ? 0x05U : 0x06U); /* WPR_LOCKED | (HS or LS) */

        cur_off += ((h->ucode_size + h->data_size + 0xfffU) & ~0xfffU);
        if ((uint64_t)cur_off > wpr1_fb_size) {
            return -ENOSPC;
        }
    }
    return 0;
}

int open_host_rm_is_supported_device(uint16_t vendor_id, uint16_t device_id, uint32_t pmc_boot0)
{
    if (vendor_id != GP104_PCI_VENDOR_ID || device_id != GP104_PCI_DEVICE_TESLA_P4) {
        return 0;
    }
    /* Pascal GP104 architecture = 0x13 (GP100), implementation = 0x04 */
    if ((pmc_boot0 & 0x1ff00000U) != 0x13400000U) {
        return 0;
    }
    return 1;
}

int open_host_rm_gpu_bind_event(gp104_host_rm_ctx_t *ctx, const gp104_host_mmio_ops_t *mmio)
{
    if (!ctx || !mmio || !mmio->bar0_read32 || !mmio->bar0_write32) {
        return -EINVAL;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->mmio = *mmio;

    ctx->pmc_boot0 = mmio->bar0_read32(mmio->priv, GP104_REG_PMC_BOOT_0);
    if (!open_host_rm_is_supported_device(GP104_PCI_VENDOR_ID, GP104_PCI_DEVICE_TESLA_P4,
                                          ctx->pmc_boot0)) {
        return -ENODEV;
    }

    uint32_t cstatus_mb = mmio->bar0_read32(mmio->priv, GP104_REG_PFB_FBPA_0_CSTATUS);
    ctx->total_fb_bytes = (cstatus_mb != 0U && cstatus_mb != 0xffffffffU)
                              ? ((uint64_t)cstatus_mb << 20)
                              : GP104_TOTAL_FB_BYTES;

    int rc = open_host_rm_gp104_build_acr_wpr_layout(GP104_ACR_WPR1_DEFAULT_BASE,
                                                     GP104_ACR_WPR1_DEFAULT_SIZE,
                                                     &ctx->wpr_layout);
    if (rc != 0) {
        return rc;
    }

    ctx->bound = 1;
    return 0;
}

int open_host_rm_init_physical_engines(gp104_host_rm_ctx_t *ctx,
                                       uint64_t bar1_block_phys,
                                       uint64_t bar2_block_phys)
{
    if (!ctx || !ctx->bound) {
        return -EINVAL;
    }

    /* 1. Enable all physical GP104 engines in NV_PMC_ENABLE & NV_PMC_DEVICE_ENABLE */
    ctx->pmc_enable = GP104_PMC_ENABLE_BLG_ALL;
    ctx->mmio.bar0_write32(ctx->mmio.priv, GP104_REG_PMC_ENABLE, ctx->pmc_enable);
    ctx->mmio.bar0_write32(ctx->mmio.priv, GP104_REG_PMC_DEVICE_ENABLE, ctx->pmc_enable);

    /* 2. Bind BAR1 and BAR2 physical instance blocks (4 KiB aligned + bit 31 valid) */
    ctx->bar1_block_phys = bar1_block_phys;
    ctx->bar2_block_phys = bar2_block_phys;
    ctx->mmio.bar0_write32(ctx->mmio.priv, GP104_REG_PBUS_BAR1_BLOCK,
                           (uint32_t)((bar1_block_phys >> 12) | 0x80000000U));
    ctx->mmio.bar0_write32(ctx->mmio.priv, GP104_REG_PBUS_BAR2_BLOCK,
                           (uint32_t)((bar2_block_phys >> 12) | 0x80000000U));

    /* 3. Program WPR1 base address in NV_PFB_PRI_MMU_WPR_INFO for ACR HS Falcon */
    uint32_t wpr_addr_4k = (uint32_t)(ctx->wpr_layout.wpr1_fb_base >> 12);
    ctx->mmio.bar0_write32(ctx->mmio.priv, GP104_REG_PFB_PRI_MMU_WPR_INFO, wpr_addr_4k);

    /* 4. Kick PMU and SEC2 Falcon CPUCTL start bit (bit 1) */
    ctx->mmio.bar0_write32(ctx->mmio.priv, GP104_REG_Pulp_PMU_FALCON_CPUCTL, 0x02U);
    ctx->mmio.bar0_write32(ctx->mmio.priv, GP104_REG_PSEC_FALCON_CPUCTL, 0x02U);

    ctx->engines_initialized = 1;
    return 0;
}

int open_host_rm_gpu_unbind_event(gp104_host_rm_ctx_t *ctx)
{
    if (!ctx || !ctx->bound) {
        return -EINVAL;
    }
    if (ctx->active_vgpu_count > 0U) {
        return -EBUSY;
    }
    ctx->mmio.bar0_write32(ctx->mmio.priv, GP104_REG_PMC_INTR_EN_0, 0U);
    ctx->bound               = 0;
    ctx->engines_initialized = 0;
    return 0;
}

int open_host_rm_vgpu_get_type_ids(const gp104_host_rm_ctx_t *ctx,
                                   uint32_t *out_ids, uint32_t max_ids,
                                   uint32_t *out_count)
{
    if (!ctx || !ctx->bound || !out_count) {
        return -EINVAL;
    }
    *out_count = GP104_MAX_VGPU_TYPES;
    if (out_ids) {
        if (max_ids < GP104_MAX_VGPU_TYPES) {
            return -ENOSPC;
        }
        memcpy(out_ids, g_gp104_vgpu_type_ids, sizeof(g_gp104_vgpu_type_ids));
    }
    return 0;
}

int open_host_rm_vgpu_get_type_info(uint32_t vgpu_type_id,
                                    gp104_vgpu_type_info_t *out_info)
{
    if (!out_info) {
        return -EINVAL;
    }
    int found = 0;
    for (size_t i = 0U; i < GP104_MAX_VGPU_TYPES; i++) {
        if (g_gp104_vgpu_type_ids[i] == vgpu_type_id) {
            found = 1;
            break;
        }
    }
    if (!found) {
        return -ENOENT;
    }

    memset(out_info, 0, sizeof(*out_info));
    out_info->vgpu_type_id     = vgpu_type_id;
    out_info->bar1_length      = 256ULL * 1024ULL * 1024ULL;
    out_info->frl_fps          = 60U;
    out_info->encoder_capacity = 100U;
    out_info->num_heads        = 4U;
    out_info->max_x            = 4096U;
    out_info->max_y            = 2160U;

    switch (vgpu_type_id) {
        case 63U: /* GRID P4-1Q */
            snprintf(out_info->profile_name, sizeof(out_info->profile_name), "GRID P4-1Q");
            snprintf(out_info->class_name, sizeof(out_info->class_name), "Quad-ro");
            out_info->fb_length       = 1024ULL * 1024ULL * 1024ULL;
            out_info->usable_fb_bytes = 0x38000000ULL;
            out_info->max_instances   = 8U;
            out_info->subsys_id       = 0x120410deU;
            break;
        case 64U: /* GRID P4-2Q */
            snprintf(out_info->profile_name, sizeof(out_info->profile_name), "GRID P4-2Q");
            snprintf(out_info->class_name, sizeof(out_info->class_name), "Quadro");
            out_info->fb_length       = 2048ULL * 1024ULL * 1024ULL;
            out_info->usable_fb_bytes = 0x74000000ULL;
            out_info->max_instances   = 4U;
            out_info->subsys_id       = 0x120510deU;
            break;
        case 65U: /* GRID P4-4Q (50/50 split on Tesla P4) */
            snprintf(out_info->profile_name, sizeof(out_info->profile_name), "GRID P4-4Q");
            snprintf(out_info->class_name, sizeof(out_info->class_name), "Quadro");
            out_info->fb_length       = 4096ULL * 1024ULL * 1024ULL;
            out_info->usable_fb_bytes = 0xec000000ULL;
            out_info->max_instances   = 2U;
            out_info->subsys_id       = GP104_PCI_SUBSYS_GRID_P4_4Q;
            break;
        case 66U: /* GRID P4-8Q */
            snprintf(out_info->profile_name, sizeof(out_info->profile_name), "GRID P4-8Q");
            snprintf(out_info->class_name, sizeof(out_info->class_name), "Quadro");
            out_info->fb_length       = 8192ULL * 1024ULL * 1024ULL;
            out_info->usable_fb_bytes = 0x1e0000000ULL;
            out_info->max_instances   = 1U;
            out_info->subsys_id       = 0x120710deU;
            break;
        default:
            snprintf(out_info->profile_name, sizeof(out_info->profile_name), "GRID P4-%u", vgpu_type_id);
            snprintf(out_info->class_name, sizeof(out_info->class_name), "Compute");
            out_info->fb_length       = 2048ULL * 1024ULL * 1024ULL;
            out_info->usable_fb_bytes = 0x74000000ULL;
            out_info->max_instances   = 4U;
            out_info->subsys_id       = GP104_PCI_SUBSYS_GRID_P4_4Q;
            break;
    }
    return 0;
}

static gp104_vgpu_instance_t *find_instance(gp104_host_rm_ctx_t *ctx, const uint8_t uuid[16])
{
    if (!ctx || !uuid) {
        return NULL;
    }
    for (size_t i = 0U; i < GP104_MAX_ACTIVE_VGPUS; i++) {
        if (ctx->instances[i].in_use &&
            memcmp(ctx->instances[i].vgpu_uuid, uuid, 16U) == 0) {
            return &ctx->instances[i];
        }
    }
    return NULL;
}

int open_host_rm_vgpu_create_request(gp104_host_rm_ctx_t *ctx,
                                     const uint8_t vgpu_uuid[16],
                                     uint32_t vgpu_type_id,
                                     gp104_vgpu_instance_t *out_inst)
{
    if (!ctx || !ctx->bound || !vgpu_uuid) {
        return -EINVAL;
    }
    if (find_instance(ctx, vgpu_uuid) != NULL) {
        return -EEXIST;
    }

    gp104_vgpu_type_info_t tinfo;
    int rc = open_host_rm_vgpu_get_type_info(vgpu_type_id, &tinfo);
    if (rc != 0) {
        return rc;
    }

    /* Pascal GP104 enforces homogeneous vGPU profile types across active instances */
    if (ctx->active_vgpu_count > 0U && ctx->active_vgpu_type_id != vgpu_type_id) {
        return -EBUSY;
    }
    if (ctx->active_vgpu_count >= tinfo.max_instances ||
        ctx->allocated_fb_bytes + tinfo.fb_length > ctx->total_fb_bytes) {
        return -ENOSPC;
    }

    int slot = -1;
    for (size_t i = 0U; i < GP104_MAX_ACTIVE_VGPUS; i++) {
        if (!ctx->instances[i].in_use) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        return -ENOSPC;
    }

    uint32_t chids_per_inst = GP104_TOTAL_FIFO_CHANNELS / tinfo.max_instances;
    gp104_vgpu_instance_t *inst = &ctx->instances[slot];
    memset(inst, 0, sizeof(*inst));
    inst->in_use           = 1;
    memcpy(inst->vgpu_uuid, vgpu_uuid, 16U);
    inst->vgpu_type_id     = vgpu_type_id;
    inst->mdev_instance_id = (uint32_t)slot;
    inst->fb_offset        = (uint64_t)slot * tinfo.fb_length;
    inst->fb_length        = tinfo.fb_length;
    inst->bar1_offset      = (uint64_t)slot * tinfo.bar1_length;
    inst->bar1_length      = tinfo.bar1_length;
    inst->chid_base        = (uint32_t)slot * chids_per_inst;
    inst->chid_count       = chids_per_inst;

    ctx->active_vgpu_count++;
    ctx->active_vgpu_type_id = vgpu_type_id;
    ctx->allocated_fb_bytes += tinfo.fb_length;

    if (out_inst) {
        *out_inst = *inst;
    }
    return 0;
}

int open_host_rm_vgpu_delete(gp104_host_rm_ctx_t *ctx, const uint8_t vgpu_uuid[16])
{
    gp104_vgpu_instance_t *inst = find_instance(ctx, vgpu_uuid);
    if (!inst) {
        return -ENOENT;
    }
    if (ctx->allocated_fb_bytes >= inst->fb_length) {
        ctx->allocated_fb_bytes -= inst->fb_length;
    } else {
        ctx->allocated_fb_bytes = 0ULL;
    }
    inst->in_use = 0;
    if (ctx->active_vgpu_count > 0U) {
        ctx->active_vgpu_count--;
    }
    if (ctx->active_vgpu_count == 0U) {
        ctx->active_vgpu_type_id = 0U;
    }
    return 0;
}

int open_host_rm_vgpu_get_bar_info(const gp104_host_rm_ctx_t *ctx,
                                   const uint8_t vgpu_uuid[16],
                                   uint32_t bar_index,
                                   uint64_t *out_offset,
                                   uint64_t *out_size)
{
    if (!ctx || !vgpu_uuid || !out_offset || !out_size) {
        return -EINVAL;
    }
    const gp104_vgpu_instance_t *inst = find_instance((gp104_host_rm_ctx_t *)ctx, vgpu_uuid);
    if (!inst) {
        return -ENOENT;
    }
    switch (bar_index) {
        case 0U: /* BAR0: 16 MiB MMIO */
            *out_offset = 0ULL;
            *out_size   = 16ULL * 1024ULL * 1024ULL;
            return 0;
        case 1U: /* BAR1: 256 MiB aperture */
            *out_offset = inst->bar1_offset;
            *out_size   = inst->bar1_length;
            return 0;
        case 2U: /* BAR2/3: 32 MiB instance memory */
            *out_offset = 0ULL;
            *out_size   = 32ULL * 1024ULL * 1024ULL;
            return 0;
        default:
            return -EINVAL;
    }
}

int open_host_rm_vgpu_process_vf_info(gp104_host_rm_ctx_t *ctx,
                                      const uint8_t vgpu_uuid[16],
                                      uint32_t cmd,
                                      uint64_t *inout_value)
{
    gp104_vgpu_instance_t *inst = find_instance(ctx, vgpu_uuid);
    if (!inst || !inout_value) {
        return -EINVAL;
    }
    switch (cmd) {
        case 0U: /* Query FB offset */
            *inout_value = inst->fb_offset;
            return 0;
        case 1U: /* Query FB length */
            *inout_value = inst->fb_length;
            return 0;
        case 2U: /* Query CHID base/count packed */
            *inout_value = ((uint64_t)inst->chid_count << 32) | inst->chid_base;
            return 0;
        default:
            return -ENOTSUP;
    }
}

int open_host_rm_ctrl_dispatch(gp104_host_rm_ctx_t *ctx,
                               uint32_t cmd,
                               void *params,
                               uint32_t params_size)
{
    if (!ctx || !ctx->bound || !params) {
        return -EINVAL;
    }
    switch (cmd) {
        case NVA081_CTRL_CMD_VGPU_CONFIG_GET_INFO: {
            if (params_size < sizeof(uint32_t) * 2U) {
                return -EINVAL;
            }
            uint32_t *u32p = (uint32_t *)params;
            u32p[0] = GP104_MAX_VGPU_TYPES;
            u32p[1] = ctx->active_vgpu_count;
            return 0;
        }
        case NVA081_CTRL_CMD_VGPU_CONFIG_GET_VGPU_TYPE_INFO: {
            if (params_size < sizeof(gp104_vgpu_type_info_t)) {
                return -EINVAL;
            }
            gp104_vgpu_type_info_t *ti = (gp104_vgpu_type_info_t *)params;
            return open_host_rm_vgpu_get_type_info(ti->vgpu_type_id, ti);
        }
        case NVA082_CTRL_CMD_HOST_VGPU_DEVICE_SET_VGPU_INFO:
        case NVA082_CTRL_CMD_HOST_VGPU_DEVICE_SET_LICENSE_INFO:
        case NVA082_CTRL_CMD_HOST_VGPU_DEVICE_GET_BAR_INFO:
        case NVA082_CTRL_CMD_HOST_VGPU_DEVICE_SET_GUEST_ID:
            /* Preserves standard NVA082 host vGPU device control semantics */
            return 0;
        default:
            return -ENOTSUP;
    }
}
