/* SPDX-License-Identifier: MIT */
/*
 * open_host_rm_gp104.h - Clean-room Pascal GP104 (Tesla P4) Host Physical RM HAL Slice
 *
 * Implements the Tier 2B open-source replacement for the GP104 physical host
 * RM slice audited from 535.309.01-vgpu-kvm (p4_vgpu_535_slice.o, pmu_535.o,
 * resolve_535_hal.c, and gp104-vgpu-535/BLOB_MANIFEST.json):
 *   1. Category #1 immutable signed Falcon firmware descriptor & WPR1/ACR
 *      Light-Secure Falcon (LSF) bootstrap table builder (ACR, PMU, SEC2,
 *      FECS, GPCCS, NVENC0/1, NVDEC0).
 *   2. GP104 physical BAR0 (NV_PMC_BOOT_0 = 0x134000a1, NV_PMC_ENABLE,
 *      NV_PBUS_BAR1_BLOCK, NV_PBUS_BAR2_BLOCK, NV_PFB_PRI_MMU_WPR_INFO)
 *      hardware bringup & 4096-channel FIFO runlist partitioner.
 *   3. Physical host vGPU lifecycle & NVA081/NVA082 control dispatch
 *      (nv_vgpu_get_type_ids, nv_vgpu_get_type_info, nv_vgpu_create_request,
 *      nv_vgpu_delete, nv_vgpu_get_bar_info, nv_vgpu_process_vf_info).
 */

#ifndef OPEN_HOST_RM_GP104_H
#define OPEN_HOST_RM_GP104_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Physical GP104 (Tesla P4) PCI & BAR0 Register Map */
#define GP104_PCI_VENDOR_ID              0x10deU
#define GP104_PCI_DEVICE_TESLA_P4        0x1bb3U
#define GP104_PCI_SUBSYS_TESLA_P4        0x11d810deU
#define GP104_PCI_SUBSYS_GRID_P4_4Q      0x120610deU
#define GP104_PMC_BOOT_0_EXPECTED        0x134000a1U

#define GP104_REG_PMC_BOOT_0             0x00000000U
#define GP104_REG_PMC_INTR_EN_0          0x00000140U
#define GP104_REG_PMC_ENABLE             0x00000200U
#define GP104_REG_PMC_DEVICE_ENABLE      0x00000600U
#define GP104_REG_PBUS_BAR1_BLOCK        0x00001704U
#define GP104_REG_PBUS_BAR2_BLOCK        0x00001714U
#define GP104_REG_PFIFO_RUNLIST_BASE     0x00002270U
#define GP104_REG_PFIFO_RUNLIST_SUBMIT   0x00002274U
#define GP104_REG_PFB_PRI_MMU_WPR_INFO   0x00100cd4U
#define GP104_REG_PFB_FBPA_0_CSTATUS     0x0010f20cU
#define GP104_REG_Pulp_PMU_FALCON_CPUCTL 0x0010a100U
#define GP104_REG_PSEC_FALCON_CPUCTL     0x00840100U

/* NV_PMC_ENABLE engine bits for Pascal GP104 */
#define GP104_PMC_ENABLE_PMEDIA          (1U << 4)
#define GP104_PMC_ENABLE_PFIFO           (1U << 8)
#define GP104_PMC_ENABLE_PGRAPH          (1U << 12)
#define GP104_PMC_ENABLE_PPMI            (1U << 14)
#define GP104_PMC_ENABLE_PFB             (1U << 20)
#define GP104_PMC_ENABLE_CE0             (1U << 21)
#define GP104_PMC_ENABLE_CE1             (1U << 22)
#define GP104_PMC_ENABLE_NVDEC0          (1U << 15)
#define GP104_PMC_ENABLE_NVENC0          (1U << 18)
#define GP104_PMC_ENABLE_NVENC1          (1U << 19)
#define GP104_PMC_ENABLE_SEC2            (1U << 24)
#define GP104_PMC_ENABLE_BLG_ALL \
    (GP104_PMC_ENABLE_PMEDIA | GP104_PMC_ENABLE_PFIFO | GP104_PMC_ENABLE_PGRAPH | \
     GP104_PMC_ENABLE_PPMI   | GP104_PMC_ENABLE_PFB   | GP104_PMC_ENABLE_CE0    | \
     GP104_PMC_ENABLE_CE1    | GP104_PMC_ENABLE_NVDEC0| GP104_PMC_ENABLE_NVENC0 | \
     GP104_PMC_ENABLE_NVENC1 | GP104_PMC_ENABLE_SEC2)

/* Category #1 Signed Falcon Engine IDs (LSF / ACR WPR1) */
typedef enum {
    GP104_FALCON_ID_PMU    = 1,
    GP104_FALCON_ID_FECS   = 2,
    GP104_FALCON_ID_GPCCS  = 3,
    GP104_FALCON_ID_NVDEC0 = 4,
    GP104_FALCON_ID_NVENC0 = 5,
    GP104_FALCON_ID_NVENC1 = 6,
    GP104_FALCON_ID_SEC2   = 7,
    GP104_FALCON_ID_ACR_HS = 8,
    GP104_FALCON_COUNT     = 8
} gp104_falcon_id_t;

typedef struct {
    gp104_falcon_id_t falcon_id;
    const char       *archive_name;
    uint32_t          rodata_offset;
    uint32_t          code_size;
    uint32_t          desc_size;
    const char       *code_sha256;
    int               is_high_secure;
} gp104_falcon_fw_manifest_entry_t;

typedef struct {
    uint32_t falcon_id;
    uint32_t lsf_offset;
    uint32_t ucode_size;
    uint32_t data_size;
    uint32_t bl_code_size;
    uint32_t flags; /* bit 0: HS_SIGNED, bit 1: LS_MANAGED, bit 2: WPR_LOCKED */
} gp104_lsf_wpr_header_t;

typedef struct {
    uint64_t               wpr1_fb_base;
    uint64_t               wpr1_fb_size;
    uint32_t               wpr_entry_count;
    gp104_lsf_wpr_header_t headers[GP104_FALCON_COUNT];
} gp104_acr_wpr_layout_t;

/* Host physical vGPU profile & VF partition structures */
#define GP104_MAX_VGPU_TYPES             22U
#define GP104_MAX_ACTIVE_VGPUS           8U
#define GP104_TOTAL_FIFO_CHANNELS        4096U
#define GP104_TOTAL_FB_BYTES             (8192ULL * 1024ULL * 1024ULL)
#define GP104_ACR_WPR1_DEFAULT_BASE      0x1ff800000ULL /* Top 8 MiB of 8 GiB FB */
#define GP104_ACR_WPR1_DEFAULT_SIZE      0x000800000ULL /* 8 MiB WPR1 region */

/* Host NVA081 / NVA082 control commands */
#define NVA081_CTRL_CMD_VGPU_CONFIG_GET_INFO              0xa0810101U
#define NVA081_CTRL_CMD_VGPU_CONFIG_GET_VGPU_TYPE_INFO    0xa0810103U
#define NVA082_CTRL_CMD_HOST_VGPU_DEVICE_SET_VGPU_INFO    0xa0820101U
#define NVA082_CTRL_CMD_HOST_VGPU_DEVICE_SET_LICENSE_INFO 0xa0820104U
#define NVA082_CTRL_CMD_HOST_VGPU_DEVICE_GET_BAR_INFO     0xa0820108U
#define NVA082_CTRL_CMD_HOST_VGPU_DEVICE_SET_GUEST_ID     0xa082010bU

typedef struct {
    uint32_t vgpu_type_id;
    char     profile_name[32];
    char     class_name[16];
    uint64_t fb_length;
    uint64_t usable_fb_bytes;
    uint64_t bar1_length;
    uint32_t max_instances;
    uint32_t frl_fps;
    uint32_t encoder_capacity;
    uint32_t subsys_id;
    uint32_t num_heads;
    uint32_t max_x;
    uint32_t max_y;
} gp104_vgpu_type_info_t;

typedef struct {
    int      in_use;
    uint8_t  vgpu_uuid[16];
    uint32_t vgpu_type_id;
    uint32_t mdev_instance_id;
    uint64_t fb_offset;
    uint64_t fb_length;
    uint64_t bar1_offset;
    uint64_t bar1_length;
    uint32_t chid_base;
    uint32_t chid_count;
} gp104_vgpu_instance_t;

typedef struct {
    void     *priv;
    uint32_t (*bar0_read32)(void *priv, uint32_t offset);
    void     (*bar0_write32)(void *priv, uint32_t offset, uint32_t val);
} gp104_host_mmio_ops_t;

typedef struct {
    gp104_host_mmio_ops_t  mmio;
    int                    bound;
    int                    engines_initialized;
    uint32_t               pmc_boot0;
    uint32_t               pmc_enable;
    uint64_t               total_fb_bytes;
    uint64_t               bar1_block_phys;
    uint64_t               bar2_block_phys;
    gp104_acr_wpr_layout_t wpr_layout;
    uint32_t               active_vgpu_count;
    uint32_t               active_vgpu_type_id;
    uint64_t               allocated_fb_bytes;
    gp104_vgpu_instance_t  instances[GP104_MAX_ACTIVE_VGPUS];
} gp104_host_rm_ctx_t;

/* Category #1 signed Falcon firmware manifest & WPR1 builder */
const gp104_falcon_fw_manifest_entry_t *open_host_rm_gp104_get_fw_manifest(size_t *out_count);
int open_host_rm_gp104_build_acr_wpr_layout(uint64_t wpr1_fb_base, uint64_t wpr1_fb_size,
                                            gp104_acr_wpr_layout_t *out_layout);

/* Physical GP104 host RM HAL lifecycle & engine bringup */
int open_host_rm_is_supported_device(uint16_t vendor_id, uint16_t device_id, uint32_t pmc_boot0);
int open_host_rm_gpu_bind_event(gp104_host_rm_ctx_t *ctx, const gp104_host_mmio_ops_t *mmio);
int open_host_rm_gpu_unbind_event(gp104_host_rm_ctx_t *ctx);
int open_host_rm_init_physical_engines(gp104_host_rm_ctx_t *ctx,
                                       uint64_t bar1_block_phys,
                                       uint64_t bar2_block_phys);

/* Host vGPU mdev/VF type queries & instance partitioning (matching p4_vgpu_535_slice.o exports) */
int open_host_rm_vgpu_get_type_ids(const gp104_host_rm_ctx_t *ctx,
                                   uint32_t *out_ids, uint32_t max_ids,
                                   uint32_t *out_count);
int open_host_rm_vgpu_get_type_info(uint32_t vgpu_type_id,
                                    gp104_vgpu_type_info_t *out_info);
int open_host_rm_vgpu_create_request(gp104_host_rm_ctx_t *ctx,
                                     const uint8_t vgpu_uuid[16],
                                     uint32_t vgpu_type_id,
                                     gp104_vgpu_instance_t *out_inst);
int open_host_rm_vgpu_delete(gp104_host_rm_ctx_t *ctx,
                             const uint8_t vgpu_uuid[16]);
int open_host_rm_vgpu_get_bar_info(const gp104_host_rm_ctx_t *ctx,
                                   const uint8_t vgpu_uuid[16],
                                   uint32_t bar_index,
                                   uint64_t *out_offset,
                                   uint64_t *out_size);
int open_host_rm_vgpu_process_vf_info(gp104_host_rm_ctx_t *ctx,
                                      const uint8_t vgpu_uuid[16],
                                      uint32_t cmd,
                                      uint64_t *inout_value);
int open_host_rm_ctrl_dispatch(gp104_host_rm_ctx_t *ctx,
                               uint32_t cmd,
                               void *params,
                               uint32_t params_size);

#ifdef __cplusplus
}
#endif

#endif /* OPEN_HOST_RM_GP104_H */
