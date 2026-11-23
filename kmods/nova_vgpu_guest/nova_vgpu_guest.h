/* SPDX-License-Identifier: GPL-2.0 */
/*
 * nova_vgpu_guest.h - UAPI & hardware definitions for the clean-room open-source
 * NVIDIA Pascal (GP104GL / Tesla P4) KVM vGPU guest kernel driver.
 *
 * Requires ZERO proprietary NVIDIA Falcon or GSP firmware binaries inside the guest.
 */

#ifndef _NOVA_VGPU_GUEST_H
#define _NOVA_VGPU_GUEST_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <linux/ioctl.h>
#include <linux/types.h>
#endif

#define NOVA_VGPU_PCI_VENDOR_NVIDIA      0x10deU
#define NOVA_VGPU_PCI_DEVICE_TESLA_P4    0x1bb3U

/* Verified GP104 virtualized BAR0 offsets (no Falcon registers used by guest) */
#define NOVA_NV_PMC_BOOT_0               0x00000000U
#define NOVA_NV_VGPU_BASE                0x0008f000U
#define NOVA_NV_VGPU_SEND_RING_LO        (NOVA_NV_VGPU_BASE + 0x00U)
#define NOVA_NV_VGPU_SEND_RING_HI        (NOVA_NV_VGPU_BASE + 0x04U)
#define NOVA_NV_VGPU_SEND_RING_PUT       (NOVA_NV_VGPU_BASE + 0x08U)
#define NOVA_NV_VGPU_SEND_RING_GET       (NOVA_NV_VGPU_BASE + 0x0cU)
#define NOVA_NV_VGPU_RECV_RING_LO        (NOVA_NV_VGPU_BASE + 0x10U)
#define NOVA_NV_VGPU_RECV_RING_HI        (NOVA_NV_VGPU_BASE + 0x14U)
#define NOVA_NV_VGPU_RECV_RING_PUT       (NOVA_NV_VGPU_BASE + 0x18U)
#define NOVA_NV_VGPU_RECV_RING_GET       (NOVA_NV_VGPU_BASE + 0x1cU)
#define NOVA_NV_VGPU_CONFIG              (NOVA_NV_VGPU_BASE + 0x20U)
#define NOVA_NV_VGPU_FB_SIZE_4K_MINUS_1  (NOVA_NV_VGPU_BASE + 0x24U)
#define NOVA_NV_VGPU_INTR_STATUS         (NOVA_NV_VGPU_BASE + 0x28U)
#define NOVA_NV_VGPU_INTR_ENABLE         (NOVA_NV_VGPU_BASE + 0x2cU)
#define NOVA_NV_VGPU_EVENT_RING_LO       (NOVA_NV_VGPU_BASE + 0x30U)
#define NOVA_NV_VGPU_EVENT_RING_HI       (NOVA_NV_VGPU_BASE + 0x34U)
#define NOVA_NV_VGPU_EVENT_RING_PUT      (NOVA_NV_VGPU_BASE + 0x38U)
#define NOVA_NV_VGPU_EVENT_RING_GET      (NOVA_NV_VGPU_BASE + 0x3cU)
#define NOVA_NV_VGPU_RPC_DOORBELL        (NOVA_NV_VGPU_BASE + 0x50U)
#define NOVA_NV_VGPU_MSG_RING_CTL        (NOVA_NV_VGPU_BASE + 0x54U)
#define NOVA_NV_VGPU_GUEST_ID_58         (NOVA_NV_VGPU_BASE + 0x58U)
#define NOVA_NV_PFB_FBPA_0_CSTATUS       0x0010f20cU

/* Pascal GP104 (Tesla P4) RM Channel & Engine Object Classes */
#define NOVA_VGPU_CLASS_CHANNEL_GPFIFO   0x0000c06fU /* NVC06F_PASCAL_CHANNEL_GPFIFO_A */
#define NOVA_VGPU_CLASS_VIDEO_ENCODER    0x0000c0b7U /* NVC0B7_VIDEO_ENCODER (NVENC) */
#define NOVA_VGPU_CLASS_VIDEO_DECODER    0x0000c1b0U /* NVC1B0_VIDEO_DECODER (NVDEC) */
#define NOVA_VGPU_CLASS_DMA_COPY         0x0000c1b5U /* NVC1B5_DMA_COPY (CE) */
#define NOVA_VGPU_CLASS_3D               0x0000c197U /* NVC197_PASCAL_A (3D/Graphics) */
#define NOVA_VGPU_CLASS_COMPUTE          0x0000c1c0U /* NVC1C0_PASCAL_COMPUTE_A (CUDA) */

/* "VRPC" wire header constants matching _nv044238rm & rpcWriteCommonHeader */
#define NOVA_VRPC_MAGIC                  0x43505256U /* 'V','R','P','C' */
#define NOVA_VRPC_VERSION_3_0            0x03000000U
#define NOVA_VRPC_HEADER_BYTES           32U
#define NOVA_VRPC_MAX_PAYLOAD            (4096U - NOVA_VRPC_HEADER_BYTES)
#define NOVA_VRPC_CHUNK_MAX              4056U
#define NOVA_VGPU_STATIC_INFO_BYTES      0x10758U /* 67,416 B (0x50 VGX 0x23.0x05) */
#define NOVA_VGPU_GR_STATIC_INFO_BYTES   0x07b30U /* 31,536 B (0x9c VGX 0x23.0x05) */

#define NOVA_VRPC_FUNC_SET_GUEST_INFO       0x01U
#define NOVA_VRPC_FUNC_ALLOC_ROOT           0x02U
#define NOVA_VRPC_FUNC_ALLOC_DEVICE         0x03U
#define NOVA_VRPC_FUNC_ALLOC_MEMORY         0x04U
#define NOVA_VRPC_FUNC_ALLOC_CTX_DMA        0x05U
#define NOVA_VRPC_FUNC_ALLOC_CHANNEL_DMA    0x06U
#define NOVA_VRPC_FUNC_MAP_MEMORY           0x07U
#define NOVA_VRPC_FUNC_ALLOC_OBJECT         0x09U
#define NOVA_VRPC_FUNC_FREE                 0x0aU
#define NOVA_VRPC_FUNC_ALLOC_VIDMEM         0x0cU
#define NOVA_VRPC_FUNC_UNMAP_MEMORY         0x0dU
#define NOVA_VRPC_FUNC_ALLOC_SUBDEVICE_535  0x13U
#define NOVA_VRPC_FUNC_IDLE_CHANNELS        0x16U
#define NOVA_VRPC_FUNC_IDLE_CHANNELS_535    0x17U
#define NOVA_VRPC_FUNC_RM_CONTROL           0x1aU
#define NOVA_VRPC_FUNC_FILL_PTE_MEM         0x1bU
#define NOVA_VRPC_FUNC_ALLOC_DEVICE_535     0x20U
#define NOVA_VRPC_FUNC_UNLOAD_GUEST         0x2fU
#define NOVA_VRPC_FUNC_GET_STATIC_INFO      0x32U
#define NOVA_VRPC_FUNC_ALLOC_VIRTMEM        0x34U
#define NOVA_VRPC_FUNC_UPDATE_PDE_2         0x38U
#define NOVA_VRPC_FUNC_UPDATE_GPU_PDES      0x3dU
#define NOVA_VRPC_FUNC_SET_GUEST_BRANCH_VER 0x40U
#define NOVA_VRPC_FUNC_GET_CONSOL_STATIC    0x50U
#define NOVA_VRPC_FUNC_GET_CONSOL_GR        0x9cU

struct nova_vrpc_header {
	__u32 version;
	__u32 signature;
	__u32 total_length;
	__u32 function;
	__u32 result;
	__u32 spare;
	__u32 sequence;
	__u32 flags;
};

struct nova_vgpu_guest_info {
	__u32 pmc_boot0;
	__u32 vgpu_config;
	__u32 cstatus_mb;
	__u32 ecc_supported;
	__u64 virtual_fb_bytes;
	__u64 bar1_bytes;
	__u64 send_ring_dma;
	__u64 recv_ring_dma;
	__u64 rpc_completed;
	__u64 irq_count;
};

struct nova_vgpu_guest_vrpc_req {
	__u32 function;
	__u32 flags;
	__u32 payload_len;
	__u32 result;
	__u64 payload_user_ptr;
};

struct nova_vgpu_guest_reg_op {
	__u32 offset;
	__u32 value;
};

#define NOVA_VGPU_GUEST_IOC_MAGIC        'N'
#define NOVA_VGPU_GUEST_IOCTL_GET_INFO   _IOR(NOVA_VGPU_GUEST_IOC_MAGIC, 0x01, struct nova_vgpu_guest_info)
#define NOVA_VGPU_GUEST_IOCTL_VRPC       _IOWR(NOVA_VGPU_GUEST_IOC_MAGIC, 0x02, struct nova_vgpu_guest_vrpc_req)
#define NOVA_VGPU_GUEST_IOCTL_READ_REG   _IOWR(NOVA_VGPU_GUEST_IOC_MAGIC, 0x03, struct nova_vgpu_guest_reg_op)

#endif /* _NOVA_VGPU_GUEST_H */
