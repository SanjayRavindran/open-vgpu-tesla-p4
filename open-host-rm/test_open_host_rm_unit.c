/* SPDX-License-Identifier: MIT */
/*
 * test_open_host_rm_unit.c - Unit Test Suite for open-host-rm (Tier 2B GP104 Physical RM HAL)
 */

#include "open_host_rm_gp104.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t pmc_boot0;
    uint32_t pmc_enable;
    uint32_t pmc_dev_enable;
    uint32_t pmc_intr_en0;
    uint32_t bar1_block;
    uint32_t bar2_block;
    uint32_t wpr_info;
    uint32_t cstatus_mb;
    uint32_t pmu_cpuctl;
    uint32_t sec2_cpuctl;
} mock_gp104_bar0_t;

static uint32_t mock_bar0_read32(void *priv, uint32_t offset)
{
    mock_gp104_bar0_t *hw = (mock_gp104_bar0_t *)priv;
    switch (offset) {
        case GP104_REG_PMC_BOOT_0:         return hw->pmc_boot0;
        case GP104_REG_PMC_ENABLE:         return hw->pmc_enable;
        case GP104_REG_PFB_FBPA_0_CSTATUS: return hw->cstatus_mb;
        default:                           return 0U;
    }
}

static void mock_bar0_write32(void *priv, uint32_t offset, uint32_t val)
{
    mock_gp104_bar0_t *hw = (mock_gp104_bar0_t *)priv;
    switch (offset) {
        case GP104_REG_PMC_INTR_EN_0:          hw->pmc_intr_en0   = val; break;
        case GP104_REG_PMC_ENABLE:             hw->pmc_enable     = val; break;
        case GP104_REG_PMC_DEVICE_ENABLE:      hw->pmc_dev_enable = val; break;
        case GP104_REG_PBUS_BAR1_BLOCK:        hw->bar1_block     = val; break;
        case GP104_REG_PBUS_BAR2_BLOCK:        hw->bar2_block     = val; break;
        case GP104_REG_PFB_PRI_MMU_WPR_INFO:   hw->wpr_info       = val; break;
        case GP104_REG_Pulp_PMU_FALCON_CPUCTL: hw->pmu_cpuctl     = val; break;
        case GP104_REG_PSEC_FALCON_CPUCTL:     hw->sec2_cpuctl    = val; break;
        default: break;
    }
}

static void test_fw_manifest_and_acr_wpr_layout(void)
{
    size_t cnt = 0U;
    const gp104_falcon_fw_manifest_entry_t *manifest = open_host_rm_gp104_get_fw_manifest(&cnt);
    assert(manifest != NULL && cnt == GP104_FALCON_COUNT);
    assert(strcmp(manifest[0].archive_name, "acr/gp104_acr_bl") == 0);
    assert(manifest[0].rodata_offset == 0x0001fad8U && manifest[0].code_size == 1280U);
    assert(strcmp(manifest[1].archive_name, "pmu/gp104_pmu_bl") == 0);
    assert(manifest[1].rodata_offset == 0x0001ae40U && manifest[1].code_size == 768U);

    gp104_acr_wpr_layout_t layout;
    assert(open_host_rm_gp104_build_acr_wpr_layout(GP104_ACR_WPR1_DEFAULT_BASE,
                                                   GP104_ACR_WPR1_DEFAULT_SIZE,
                                                   &layout) == 0);
    assert(layout.wpr_entry_count == GP104_FALCON_COUNT);
    assert(layout.headers[0].falcon_id == GP104_FALCON_ID_ACR_HS);
    assert(layout.headers[0].lsf_offset == 0x1000U);
    assert((layout.headers[0].flags & 0x05U) == 0x05U);
}

static void test_physical_bringup_and_50_50_vgpu_split(void)
{
    mock_gp104_bar0_t hw;
    memset(&hw, 0, sizeof(hw));
    hw.pmc_boot0  = GP104_PMC_BOOT_0_EXPECTED;
    hw.cstatus_mb = 8192U;

    gp104_host_mmio_ops_t ops = {
        .priv        = &hw,
        .bar0_read32 = mock_bar0_read32,
        .bar0_write32 = mock_bar0_write32
    };

    gp104_host_rm_ctx_t ctx;
    assert(open_host_rm_gpu_bind_event(&ctx, &ops) == 0);
    assert(ctx.bound == 1 && ctx.total_fb_bytes == GP104_TOTAL_FB_BYTES);

    assert(open_host_rm_init_physical_engines(&ctx, 0x1fe000000ULL, 0x1fe010000ULL) == 0);
    assert(hw.pmc_enable == GP104_PMC_ENABLE_BLG_ALL);
    assert(hw.bar1_block == (uint32_t)((0x1fe000000ULL >> 12) | 0x80000000U));
    assert(hw.wpr_info == (uint32_t)(GP104_ACR_WPR1_DEFAULT_BASE >> 12));
    assert(hw.pmu_cpuctl == 0x02U && hw.sec2_cpuctl == 0x02U);

    /* Query 22 supported Tesla P4 vGPU profile IDs */
    uint32_t type_ids[GP104_MAX_VGPU_TYPES];
    uint32_t type_cnt = 0U;
    assert(open_host_rm_vgpu_get_type_ids(&ctx, type_ids, GP104_MAX_VGPU_TYPES, &type_cnt) == 0);
    assert(type_cnt == 22U);

    gp104_vgpu_type_info_t t65;
    t65.vgpu_type_id = 65U;
    assert(open_host_rm_ctrl_dispatch(&ctx, NVA081_CTRL_CMD_VGPU_CONFIG_GET_VGPU_TYPE_INFO,
                                      &t65, sizeof(t65)) == 0);
    assert(strcmp(t65.profile_name, "GRID P4-4Q") == 0);
    assert(t65.usable_fb_bytes == 0xec000000ULL && t65.max_instances == 2U);

    /* Create 2 GRID P4-4Q instances (vm-win11 + vm-alma 50/50 split) */
    const uint8_t uuid_win11[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,1 };
    const uint8_t uuid_alma[16]  = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,2 };
    const uint8_t uuid_extra[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,3 };

    gp104_vgpu_instance_t i0, i1;
    assert(open_host_rm_vgpu_create_request(&ctx, uuid_win11, 65U, &i0) == 0);
    assert(i0.fb_offset == 0ULL && i0.chid_base == 0U && i0.chid_count == 2048U);

    /* Heterogeneous profile (e.g. P4-2Q while P4-4Q is active) must be rejected with -EBUSY */
    assert(open_host_rm_vgpu_create_request(&ctx, uuid_extra, 64U, NULL) == -EBUSY);

    assert(open_host_rm_vgpu_create_request(&ctx, uuid_alma, 65U, &i1) == 0);
    assert(i1.fb_offset == (4096ULL * 1024ULL * 1024ULL));
    assert(i1.chid_base == 2048U && i1.chid_count == 2048U);

    /* 3rd P4-4Q instance must be rejected with -ENOSPC */
    assert(open_host_rm_vgpu_create_request(&ctx, uuid_extra, 65U, NULL) == -ENOSPC);

    uint64_t b1_off = 0ULL, b1_sz = 0ULL;
    assert(open_host_rm_vgpu_get_bar_info(&ctx, uuid_alma, 1U, &b1_off, &b1_sz) == 0);
    assert(b1_off == 256ULL * 1024ULL * 1024ULL && b1_sz == 256ULL * 1024ULL * 1024ULL);

    uint64_t chid_pack = 0ULL;
    assert(open_host_rm_vgpu_process_vf_info(&ctx, uuid_alma, 2U, &chid_pack) == 0);
    assert((uint32_t)(chid_pack & 0xffffffffULL) == 2048U);
    assert((uint32_t)(chid_pack >> 32) == 2048U);

    assert(open_host_rm_vgpu_delete(&ctx, uuid_win11) == 0);
    assert(open_host_rm_vgpu_delete(&ctx, uuid_alma) == 0);
    assert(open_host_rm_gpu_unbind_event(&ctx) == 0);
}

int main(void)
{
    test_fw_manifest_and_acr_wpr_layout();
    test_physical_bringup_and_50_50_vgpu_split();
    printf("ALL_OPEN_HOST_RM_GP104_UNIT_TESTS_PASSED (8/8 LSF WPR headers, 22 vGPU types, 50/50 P4-4Q split)\n");
    return 0;
}
