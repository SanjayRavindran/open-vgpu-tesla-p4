/* SPDX-License-Identifier: MIT OR GPL-2.0 */
/*
 * open_vgpu_guest_rpc.h - Clean-room Guest vGPU VRPC Client Library (VGX 0x23.0x05)
 *
 * Implements the guest-side PCI/BAR0 0x8f000 "VRPC" circular ring transport,
 * chunked 0x50 (67,416 B) & 0x9c (31,536 B) consolidated static info deserializers,
 * and typed RM client/device/memory/MMU/channel RPCs for GP104 (Tesla P4) vGPUs.
 */

#ifndef OPEN_VGPU_GUEST_RPC_H
#define OPEN_VGPU_GUEST_RPC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPEN_VGPU_GUEST_VGX_MAJOR        0x23U
#define OPEN_VGPU_GUEST_VGX_MINOR        0x05U
#define OPEN_VGPU_GUEST_VRPC_MAGIC       0x43505256U /* "VRPC" */
#define OPEN_VGPU_GUEST_VRPC_VER_3_0     0x03000000U
#define OPEN_VGPU_GUEST_VRPC_PENDING     0xffffffffU
#define OPEN_VGPU_GUEST_HDR_BYTES        0x20U
#define OPEN_VGPU_GUEST_PAGE_BYTES       4096U
#define OPEN_VGPU_GUEST_MAX_PAYLOAD      (OPEN_VGPU_GUEST_PAGE_BYTES - OPEN_VGPU_GUEST_HDR_BYTES)
#define OPEN_VGPU_GUEST_CHUNK_MAX        4056U
#define OPEN_VGPU_GUEST_STATIC_INFO_LEN  0x10758U
#define OPEN_VGPU_GUEST_GR_INFO_LEN      0x07b30U

#define NV_VGPU_BAR0_PMC_BOOT_0          0x000000ULL
#define NV_VGPU_BAR0_PFB_CSTATUS         0x10f20cULL
#define NV_VGPU_BAR0_VGPU_BASE           0x08f000ULL
#define NV_VGPU_BAR0_SEND_RING_LO        (NV_VGPU_BAR0_VGPU_BASE + 0x00ULL)
#define NV_VGPU_BAR0_SEND_RING_HI        (NV_VGPU_BAR0_VGPU_BASE + 0x04ULL)
#define NV_VGPU_BAR0_SEND_RING_PUT       (NV_VGPU_BAR0_VGPU_BASE + 0x08ULL)
#define NV_VGPU_BAR0_SEND_RING_GET       (NV_VGPU_BAR0_VGPU_BASE + 0x0cULL)
#define NV_VGPU_BAR0_RECV_RING_LO        (NV_VGPU_BAR0_VGPU_BASE + 0x10ULL)
#define NV_VGPU_BAR0_RECV_RING_HI        (NV_VGPU_BAR0_VGPU_BASE + 0x14ULL)
#define NV_VGPU_BAR0_RECV_RING_PUT       (NV_VGPU_BAR0_VGPU_BASE + 0x18ULL)
#define NV_VGPU_BAR0_RECV_RING_GET       (NV_VGPU_BAR0_VGPU_BASE + 0x1cULL)
#define NV_VGPU_BAR0_CONFIG              (NV_VGPU_BAR0_VGPU_BASE + 0x20ULL)
#define NV_VGPU_BAR0_FB_PAGES_M1         (NV_VGPU_BAR0_VGPU_BASE + 0x24ULL)
#define NV_VGPU_BAR0_INTR_STATUS         (NV_VGPU_BAR0_VGPU_BASE + 0x28ULL)
#define NV_VGPU_BAR0_INTR_ENABLE         (NV_VGPU_BAR0_VGPU_BASE + 0x2cULL)
#define NV_VGPU_BAR0_EVENT_RING_LO       (NV_VGPU_BAR0_VGPU_BASE + 0x30ULL)
#define NV_VGPU_BAR0_EVENT_RING_HI       (NV_VGPU_BAR0_VGPU_BASE + 0x34ULL)
#define NV_VGPU_BAR0_EVENT_RING_PUT      (NV_VGPU_BAR0_VGPU_BASE + 0x38ULL)
#define NV_VGPU_BAR0_EVENT_RING_GET      (NV_VGPU_BAR0_VGPU_BASE + 0x3cULL)
#define NV_VGPU_BAR0_RPC_DOORBELL        (NV_VGPU_BAR0_VGPU_BASE + 0x50ULL)
#define NV_VGPU_BAR0_GUEST_ID_58         (NV_VGPU_BAR0_VGPU_BASE + 0x58ULL)

typedef struct {
    void     *priv;
    uint32_t (*pci_cfg_read32)(void *priv, uint64_t offset);
    uint32_t (*bar0_read32)(void *priv, uint64_t offset);
    void     (*bar0_write32)(void *priv, uint64_t offset, uint32_t val);
    void    *(*alloc_dma_page)(void *priv, uint64_t *out_gpa);
    void     (*free_dma_page)(void *priv, void *va, uint64_t gpa);
} open_vgpu_guest_transport_t;

typedef struct {
    open_vgpu_guest_transport_t ops;
    int      initialized;
    uint32_t pci_id;
    uint32_t subsys_id;
    char     vendor_cap_tag[20];
    uint32_t pmc_boot0;
    uint32_t vgpu_config;
    uint32_t cstatus_mb;
    uint64_t virtual_fb_bytes;

    uint8_t *send_ring_va;
    uint64_t send_ring_gpa;
    uint8_t *recv_ring_va;
    uint64_t recv_ring_gpa;
    uint8_t *event_ring_va;
    uint64_t event_ring_gpa;
    uint8_t *msg_page_va;
    uint64_t msg_page_gpa;

    uint32_t send_put;
    uint32_t recv_get;
    uint32_t next_seq;
    uint64_t vrpc_calls;
} open_vgpu_guest_ctx_t;

typedef struct {
    uint64_t usable_fb_bytes;
    uint64_t bar1_bytes;
    uint64_t profile_bytes;
    uint32_t vgpu_type_id;
    uint16_t pci_device_id;
    uint8_t  ecc_supported;
    uint32_t frl_fps;
    uint32_t encoder_capacity;
    uint32_t subsys_id;
    uint32_t virt_mode;
    char     host_driver_ver[64];
    char     host_branch_ver[64];
    char     profile_name[64];
    char     guest_driver_ver[32];
    char     guest_branch_ver[32];
} open_vgpu_guest_static_summary_t;

typedef struct {
    uint32_t pmc_boot0;
    uint32_t pmc_boot42;
    uint32_t gpc_count;
    uint32_t tpc_count;
    uint32_t cuda_cores;
    uint32_t sm_order_word0;
    uint32_t pdb_props_word0;
} open_vgpu_guest_gr_summary_t;

int  open_vgpu_guest_init(open_vgpu_guest_ctx_t *ctx, const open_vgpu_guest_transport_t *ops);
void open_vgpu_guest_shutdown(open_vgpu_guest_ctx_t *ctx);

int  open_vgpu_guest_vrpc_call(open_vgpu_guest_ctx_t *ctx, uint32_t fn,
                               void *payload, uint32_t payload_len,
                               uint32_t *out_status);

int  open_vgpu_guest_handshake(open_vgpu_guest_ctx_t *ctx, const char *driver_ver,
                               const char *branch_ver, uint32_t *out_status);

int  open_vgpu_guest_fetch_static_info(open_vgpu_guest_ctx_t *ctx,
                                       uint8_t out_blob[OPEN_VGPU_GUEST_STATIC_INFO_LEN],
                                       open_vgpu_guest_static_summary_t *out_summary,
                                       uint32_t *out_status);

int  open_vgpu_guest_fetch_gr_static_info(open_vgpu_guest_ctx_t *ctx,
                                          uint8_t out_blob[OPEN_VGPU_GUEST_GR_INFO_LEN],
                                          open_vgpu_guest_gr_summary_t *out_summary,
                                          uint32_t *out_status);

int  open_vgpu_guest_alloc_root(open_vgpu_guest_ctx_t *ctx, uint32_t h_client, uint32_t *out_status);
int  open_vgpu_guest_alloc_device(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                  uint32_t h_device, uint32_t *out_status);
int  open_vgpu_guest_alloc_subdevice(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                     uint32_t h_device, uint32_t h_subdev, uint32_t *out_status);
int  open_vgpu_guest_alloc_vidmem(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                  uint32_t h_device, uint32_t h_mem, uint64_t size,
                                  uint64_t fb_pfn, uint32_t *out_status);
int  open_vgpu_guest_alloc_sysmem(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                  uint32_t h_device, uint32_t h_mem, uint64_t size,
                                  const uint64_t *gpfns, uint32_t page_count,
                                  uint32_t *out_status);
int  open_vgpu_guest_alloc_virtmem(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                   uint32_t h_device, uint32_t h_virt, uint64_t size,
                                   uint32_t *out_status);
int  open_vgpu_guest_map_bar1(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                              uint32_t h_device, uint32_t h_mem, uint64_t length,
                              uint64_t *out_bar1_offset, uint32_t *out_status);
int  open_vgpu_guest_alloc_ctx_dma(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                   uint32_t h_dma, uint32_t h_mem, uint64_t limit,
                                   uint32_t *out_status);
int  open_vgpu_guest_map_dma(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                             uint32_t h_device, uint32_t h_dma, uint32_t h_mem,
                             uint64_t length, uint64_t *inout_dma_offset,
                             uint32_t *out_status);
int  open_vgpu_guest_unmap_dma(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                               uint32_t h_device, uint32_t h_dma, uint32_t h_mem,
                               uint64_t dma_offset, uint32_t *out_status);
int  open_vgpu_guest_fill_pte_mem(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                  uint32_t h_device, uint32_t page_count,
                                  uint32_t *out_status);
int  open_vgpu_guest_update_pde_2(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                  uint32_t h_device, uint32_t *out_status);
int  open_vgpu_guest_update_gpu_pdes(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                     uint32_t h_device, uint64_t dir_phys,
                                     uint32_t is_valid, uint32_t *out_status);
int  open_vgpu_guest_alloc_channel_dma(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                       uint32_t h_parent, uint32_t h_chan,
                                       uint32_t h_class, uint32_t gpfifo_entries,
                                       uint64_t userd_vmmio, uint64_t userd_pmmio,
                                       uint32_t *out_status);
int  open_vgpu_guest_idle_channels(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                                   uint32_t *out_status);
int  open_vgpu_guest_free_obj(open_vgpu_guest_ctx_t *ctx, uint32_t h_client,
                              uint32_t h_parent, uint32_t h_obj, uint32_t *out_status);

#ifdef __cplusplus
}
#endif

#endif /* OPEN_VGPU_GUEST_RPC_H */