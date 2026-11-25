#include "open_vgpu_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

static void sw32(uint8_t *p, size_t off, uint32_t v)
{
    memcpy(p + off, &v, 4U);
}

static void sw64(uint8_t *p, size_t off, uint64_t v)
{
    memcpy(p + off, &v, 8U);
}

static void sw16(uint8_t *p, size_t off, uint16_t v)
{
    memcpy(p + off, &v, 2U);
}

void gp104_free_static_info_blobs(gp104_vgpu_dev_t *dev)
{
    if (!dev) {
        return;
    }
    free(dev->static_info_buf);
    dev->static_info_buf = NULL;
    dev->static_info_len = 0U;

    free(dev->gr_static_info_buf);
    dev->gr_static_info_buf = NULL;
    dev->gr_static_info_len = 0U;
}

/*
 * Exact clean-room equivalent of _nv007054vgpu (0x239210.c): packs 8 entries
 * from host RM 0x20800a26 (stride 0x4a dwords = 0x128 bytes, total 0x940 B)
 * into VGX 0x23.0x05 wire format (stride 0x48 dwords = 0x120 bytes, total 0x900 B).
 */
static void pack_gr_gpc_mask_wire(const uint8_t *rm_940, uint8_t *wire_900)
{
    for (size_t g = 0U; g < 8U; g++) {
        const uint32_t *src = (const uint32_t *)(const void *)(rm_940 + g * 0x128U);
        uint32_t *dst       = (uint32_t *)(void *)(wire_900 + g * 0x120U);
        dst[0]    = src[0];
        dst[0x19] = src[0x19];
        for (size_t i = 0U; i < 12U; i++) {
            dst[1U + i]    = src[1U + i];
            dst[0xdU + i]  = src[0xdU + i];
            dst[0x1aU + i] = src[0x1aU + i];
            dst[0x30U + i] = src[0x30U + i];
            dst[0x3cU + i] = src[0x3cU + i];
        }
        memcpy(&dst[0x26], &src[0x26], 0x28U);
    }
}

static uint32_t query_rm_subdev_block(gp104_vgpu_dev_t *dev, uint32_t cmd,
                                      void *buf, uint32_t sz,
                                      uint32_t *ok_mask, uint32_t bit_idx)
{
    if (!dev->rm.attached || dev->rm.ctl_fd < 0 ||
        dev->rm.h_root == 0U || dev->rm.h_subdev == 0U) {
        return NV_ERR_INVALID_STATE;
    }
    uint32_t st = open_vgpu_rm_control(dev->rm.ctl_fd, dev->rm.h_root,
                                       dev->rm.h_subdev, cmd, buf, sz);
    if (st == NV_OK && ok_mask) {
        *ok_mask |= (1U << bit_idx);
    }
    return st;
}

static void populate_vgpu_static_info_0x50(gp104_vgpu_dev_t *dev)
{
    uint8_t *b = dev->static_info_buf;
    memset(b, 0, GP104_VGPU_STATIC_INFO_BYTES);
    dev->static_info_len = GP104_VGPU_STATIC_INFO_BYTES;
    dev->static_info_rm_ok_mask = 0U;

    uint64_t fb      = dev->rm.live_fb_length ? dev->rm.live_fb_length : dev->profile.usable_fb_bytes;
    uint64_t b1      = dev->rm.live_bar1_length ? dev->rm.live_bar1_length : dev->profile.bar1_bytes;
    uint64_t prof_b  = dev->profile.profile_bytes ? dev->profile.profile_bytes : fb;
    uint32_t vid     = dev->rm.pci_vendor ? dev->rm.pci_vendor : dev->profile.vendor_id;
    uint32_t did     = dev->rm.pci_device ? dev->rm.pci_device : dev->profile.device_id;
    uint32_t svid    = dev->rm.pci_subsys_vendor ? dev->rm.pci_subsys_vendor : dev->profile.subsys_vendor_id;
    uint32_t sdid    = dev->profile.subsys_device_id;
    uint32_t enc_cap = dev->rm.encoder_capacity ? dev->rm.encoder_capacity : 100U;
    const char *pname = dev->rm.live_profile_name[0] ? dev->rm.live_profile_name : dev->profile.name;

    /* Block 0 (slot 0x14, +0x00000, 0x08 B) */
    sw32(b, GP104_SINFO_OFF_BLK00_SLOT14 + 0x00U, (uint32_t)(fb >> 20));
    sw32(b, GP104_SINFO_OFF_BLK00_SLOT14 + 0x04U, (uint32_t)(b1 >> 20));

    /* Block 5 (slot 0x0a, +0x00b74, 0x24 B): GP104 silicon / PCI identity */
    uint8_t *blk5 = b + GP104_SINFO_OFF_PCI_BOOT;
    sw32(blk5, 0x00U, dev->pmc_boot0);
    sw32(blk5, 0x04U, dev->pmc_boot42);
    sw32(blk5, 0x08U, (did << 16) | vid);
    sw32(blk5, 0x0cU, (sdid << 16) | svid);
    sw32(blk5, 0x10U, dev->profile.vgpu_type_id);

    /* Block 6 (slot 0x15, +0x00b98, 0xfc8 B): core VGPU_STATIC_INFO fields (_nv007170vgpu) */
    uint8_t *core = b + GP104_SINFO_OFF_CORE;
    sw64(core, 0x00U, fb);
    sw64(core, 0x08U, b1);
    sw64(core, 0x10U, prof_b);
    sw32(core, 0x18U, dev->profile.vgpu_type_id);
    sw16(core, 0x1cU, (uint16_t)did);
    core[0x1eU] = dev->profile.ecc_supported ? 1U : 0U;
    sw32(core, 0x20U, dev->frl_fps);
    sw32(core, 0x24U, enc_cap);
    sw32(core, 0x28U, (sdid << 16) | did);
    sw64(core, 0x40U, dev->rm.p2p_cap);
    snprintf((char *)(core + 0x60U), 255U, "%s", NV_VGPU_DRIVER_VERSION_STR);
    snprintf((char *)(core + 0x160U), 255U, "r580_00");
    snprintf((char *)(core + 0x260U), 255U, "%.254s", pname ? pname : "GRID P4-4Q");
    snprintf((char *)(core + 0x360U), 255U, "open-pascal-vgpu %s", NV_VGPU_DRIVER_VERSION_STR);

    struct utsname uts;
    if (uname(&uts) == 0) {
        snprintf((char *)(core + 0x460U), 255U, "%.48s %.48s %.48s %.48s %.48s",
                 uts.sysname, uts.nodename, uts.release, uts.version, uts.machine);
    }
    if (dev->guest_driver_ver[0]) {
        snprintf((char *)(core + 0xc00U), 127U, "%.31s", dev->guest_driver_ver);
    }
    if (dev->guest_branch_ver[0]) {
        snprintf((char *)(core + 0xe68U), 127U, "%.31s", dev->guest_branch_ver);
    }
    sw32(core, 0x8d4U, dev->rm.virt_mode ? dev->rm.virt_mode : 3U);

    /* Block 8 (slot 0x00, +0x02364, 0x5a0 B): GP104 baseline engine descriptor list */
    uint8_t *eng = b + GP104_SINFO_OFF_ENGINE_LIST;
    static const uint32_t gp104_engines[8] = {
        0x00000001U, /* GR0 */
        0x00000010U, /* CE0 */
        0x00000011U, /* CE1 */
        0x00000012U, /* CE2 */
        0x00000020U, /* NVDEC0 */
        0x00000030U, /* NVENC0 */
        0x00000031U, /* NVENC1 */
        0x00000040U, /* SW */
    };
    sw32(eng, 0x00U, 8U);
    for (size_t i = 0U; i < 8U; i++) {
        sw32(eng, 0x04U + i * 4U, gp104_engines[i]);
    }

    if (!dev->rm.attached || dev->rm.ctl_fd < 0) {
        return;
    }

    /* Live Host RM static-info queries matching _nv011610vgpu (0x17fbf0.c) */
    query_rm_subdev_block(dev, NV2080_CTRL_CMD_NVLINK_GET_NVLINK_CAPS,
                          b + GP104_SINFO_OFF_NVLINK_CAPS, 0x10U,
                          &dev->static_info_rm_ok_mask, 0U);
    query_rm_subdev_block(dev, NV2080_CTRL_CMD_MC_GET_STATIC_INTR_TABLE_1,
                          b + GP104_SINFO_OFF_INTR_TABLE_1, 0x804U,
                          &dev->static_info_rm_ok_mask, 1U);

    uint8_t ltc_tmp[0x14];
    memset(ltc_tmp, 0, sizeof(ltc_tmp));
    if (query_rm_subdev_block(dev, NV2080_CTRL_CMD_FB_GET_LTC_INFO_FOR_FBP,
                              ltc_tmp, sizeof(ltc_tmp),
                              &dev->static_info_rm_ok_mask, 2U) == NV_OK) {
        memcpy(b + GP104_SINFO_OFF_LTC_INFO, ltc_tmp, 0x10U);
    }

    uint8_t fifo_tmp[0xc8c];
    memset(fifo_tmp, 0, sizeof(fifo_tmp));
    if (query_rm_subdev_block(dev, NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE,
                              fifo_tmp, sizeof(fifo_tmp),
                              &dev->static_info_rm_ok_mask, 3U) == NV_OK) {
        memcpy(b + GP104_SINFO_OFF_FIFO_DEV_TABLE, fifo_tmp, sizeof(fifo_tmp));
    }

    query_rm_subdev_block(dev, NV2080_CTRL_CMD_FB_GET_DYNAMIC_BLACKLISTED_PAGES,
                          b + GP104_SINFO_OFF_BLACKLIST_PAGES, 0x410U,
                          &dev->static_info_rm_ok_mask, 4U);
    query_rm_subdev_block(dev, NV2080_CTRL_CMD_MC_GET_STATIC_INTR_TABLE_2,
                          b + GP104_SINFO_OFF_INTR_TABLE_2, 0x204U,
                          &dev->static_info_rm_ok_mask, 5U);
    uint32_t ce_caps[2] = { 9U, 0U }; /* NV2080_ENGINE_TYPE_COPY0 = 9 */
    if (query_rm_subdev_block(dev, NV2080_CTRL_CMD_CE_GET_CAPS_V2,
                              ce_caps, sizeof(ce_caps),
                              &dev->static_info_rm_ok_mask, 6U) == NV_OK) {
        memcpy(b + GP104_SINFO_OFF_CE_CAPS, ce_caps, sizeof(ce_caps));
    }
}

static void populate_gr_static_info_0x9c(gp104_vgpu_dev_t *dev)
{
    uint8_t *g = dev->gr_static_info_buf;
    memset(g, 0, GP104_GR_STATIC_INFO_BYTES);
    dev->gr_static_info_len = GP104_GR_STATIC_INFO_BYTES;
    dev->gr_static_info_rm_ok_mask = 0U;

    /* GP104 Pascal offline baseline inside KGR/GPC sections */
    sw32(g, GP104_GRINFO_OFF_KGR_INFO + 0x00U, dev->pmc_boot0);
    sw32(g, GP104_GRINFO_OFF_KGR_INFO + 0x04U, dev->pmc_boot42);
    sw32(g, GP104_GRINFO_OFF_KGR_INFO + 0x08U, 4U);    /* 4 GPCs */
    sw32(g, GP104_GRINFO_OFF_KGR_INFO + 0x0cU, 20U);   /* 20 TPCs / SMs */
    sw32(g, GP104_GRINFO_OFF_KGR_INFO + 0x10U, 2560U); /* 2560 CUDA cores */
    sw32(g, GP104_GRINFO_OFF_GPC_MASK + 0x00U, 4U);

    if (!dev->rm.attached || dev->rm.ctl_fd < 0) {
        return;
    }

    /* Live Host RM 11 GR static-info queries matching _nv009105vgpu (0x1758d0.c) */
    query_rm_subdev_block(dev, NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_SM_ORDER,
                          g + GP104_GRINFO_OFF_SM_ORDER, 0xd40U,
                          &dev->gr_static_info_rm_ok_mask, 0U);
    query_rm_subdev_block(dev, NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_INFO,
                          g + GP104_GRINFO_OFF_KGR_INFO, 0x5a20U,
                          &dev->gr_static_info_rm_ok_mask, 1U);
    query_rm_subdev_block(dev, NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_ROP_INFO,
                          g + GP104_GRINFO_OFF_ROP_INFO, 0x60U,
                          &dev->gr_static_info_rm_ok_mask, 2U);
    query_rm_subdev_block(dev, NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_PPC_MASK,
                          g + GP104_GRINFO_OFF_PPC_MASK, 0x180U,
                          &dev->gr_static_info_rm_ok_mask, 3U);
    query_rm_subdev_block(dev, NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CTX_BUF_INFO,
                          g + GP104_GRINFO_OFF_CTX_BUF_INFO, 0x640U,
                          &dev->gr_static_info_rm_ok_mask, 4U);
    query_rm_subdev_block(dev, NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_RATE_MOD,
                          g + GP104_GRINFO_OFF_RATE_MOD, 0x48U,
                          &dev->gr_static_info_rm_ok_mask, 5U);

    uint8_t gpc_rm[0x940];
    memset(gpc_rm, 0, sizeof(gpc_rm));
    if (query_rm_subdev_block(dev, NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_GPC_MASK,
                              gpc_rm, sizeof(gpc_rm),
                              &dev->gr_static_info_rm_ok_mask, 6U) == NV_OK) {
        pack_gr_gpc_mask_wire(gpc_rm, g + GP104_GRINFO_OFF_GPC_MASK);
    }

    query_rm_subdev_block(dev, NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_ZCULL,
                          g + GP104_GRINFO_OFF_ZCULL_INFO, 0x140U,
                          &dev->gr_static_info_rm_ok_mask, 7U);
    query_rm_subdev_block(dev, NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_FECS_REC_SZ,
                          g + GP104_GRINFO_OFF_FECS_REC_SZ, 0x20U,
                          &dev->gr_static_info_rm_ok_mask, 8U);
    query_rm_subdev_block(dev, NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_FECS_TRACE,
                          g + GP104_GRINFO_OFF_FECS_TRACE, 0x100U,
                          &dev->gr_static_info_rm_ok_mask, 9U);
    query_rm_subdev_block(dev, NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_PDB_PROPS,
                          g + GP104_GRINFO_OFF_PDB_PROPS, 0x08U,
                          &dev->gr_static_info_rm_ok_mask, 10U);
}

void gp104_build_static_info_blobs(gp104_vgpu_dev_t *dev)
{
    if (!dev) {
        return;
    }
    if (!dev->static_info_buf) {
        dev->static_info_buf = (uint8_t *)calloc(1U, GP104_VGPU_STATIC_INFO_BYTES);
    }
    if (!dev->gr_static_info_buf) {
        dev->gr_static_info_buf = (uint8_t *)calloc(1U, GP104_GR_STATIC_INFO_BYTES);
    }
    if (!dev->static_info_buf || !dev->gr_static_info_buf) {
        gp104_free_static_info_blobs(dev);
        return;
    }
    populate_vgpu_static_info_0x50(dev);
    populate_gr_static_info_0x9c(dev);
}
