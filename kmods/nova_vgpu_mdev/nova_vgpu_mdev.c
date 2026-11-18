// SPDX-License-Identifier: GPL-2.0
/*
 * nova_vgpu_mdev.c - Clean-room Linux 7.0 vfio-mdev host mediator for
 * NVIDIA Pascal (GP104GL / Tesla P4) open-source vGPU end-to-end KVM testing.
 *
 * Exposes a virtual PCI GP104GL mdev ("nvidia-64" / GRID P4-2Q) with emulated
 * BAR0 NV_VGPU (0x8f000..0x8f05c) + NV_PMC + NV_PFB + NV_PGRAPH registers,
 * 16 MiB mmap-capable BAR1 aperture, MSI/INTx interrupt injection via eventfd,
 * and guest DMA ring transfers via vfio_dma_rw() — without interfering with the
 * host physical GPU's active driver.
 */

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/eventfd.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mdev.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vfio.h>
#include <linux/vmalloc.h>

#define DRV_NAME "nova_vgpu_mdev"

#define NOVA_PCI_VENDOR_NVIDIA      0x10deU
#define NOVA_PCI_DEVICE_TESLA_P4    0x1bb3U
#define NOVA_PCI_SUBSYS_P4_2Q       0x1205U

#define NOVA_BAR0_SIZE              0x01000000UL /* 16 MiB */
#define NOVA_BAR1_SIZE              0x01000000UL /* 16 MiB */
#define NOVA_CFG_SPACE_SIZE         256U
#define NOVA_VFIO_REGION_SHIFT      40
#define NOVA_VFIO_REGION_OFFSET(i)  ((u64)(i) << NOVA_VFIO_REGION_SHIFT)
#define NOVA_VFIO_OFFSET_TO_INDEX(p) ((unsigned int)((p) >> NOVA_VFIO_REGION_SHIFT))
#define NOVA_VFIO_OFFSET_TO_OFF(p)  ((p) & ((1ULL << NOVA_VFIO_REGION_SHIFT) - 1ULL))

#define NOVA_NV_PMC_BOOT_0          0x00000000U
#define NOVA_NV_PMC_ENABLE          0x00000004U
#define NOVA_NV_PMC_BOOT_42         0x00000a00U
#define NOVA_NV_VGPU_BASE           0x0008f000U
#define NOVA_NV_PFB_FBPA_0_CSTATUS  0x0010f20cU
#define NOVA_NV_PGRAPH_FS_0         0x00500c30U

#define NOVA_VGPU_SEND_RING_LO      0x00U
#define NOVA_VGPU_SEND_RING_HI      0x04U
#define NOVA_VGPU_SEND_RING_PUT     0x08U
#define NOVA_VGPU_SEND_RING_GET     0x0cU
#define NOVA_VGPU_RECV_RING_LO      0x10U
#define NOVA_VGPU_RECV_RING_HI      0x14U
#define NOVA_VGPU_RECV_RING_PUT     0x18U
#define NOVA_VGPU_RECV_RING_GET     0x1cU
#define NOVA_VGPU_CONFIG            0x20U
#define NOVA_VGPU_FB_SIZE_4K_M1     0x24U
#define NOVA_VGPU_INTR_STATUS       0x28U
#define NOVA_VGPU_INTR_ENABLE       0x2cU
#define NOVA_VGPU_EVENT_RING_LO     0x30U
#define NOVA_VGPU_EVENT_RING_HI     0x34U
#define NOVA_VGPU_EVENT_RING_PUT    0x38U
#define NOVA_VGPU_EVENT_RING_GET    0x3cU
#define NOVA_VGPU_RPC_DOORBELL      0x50U
#define NOVA_VGPU_MSG_RING_CTL      0x54U

#define NOVA_VRPC_MAGIC             0x43505256U /* 'V','R','P','C' */
#define NOVA_VRPC_HEADER_BYTES      32U
#define NOVA_P4_2Q_FB_BYTES         0x74000000ULL /* 1856 MiB */

/* MSI capability at config offset 0x68 (64-bit, 1 vector) */
#define NOVA_PCI_MSI_CAP_OFF        0x68U

struct nova_vrpc_header {
	__le32 version;
	__le32 signature;
	__le32 total_length;
	__le32 function;
	__le32 result;
	__le32 spare;
	__le32 sequence;
	__le32 flags;
};

struct nova_mdev_state {
	struct vfio_device vdev;
	struct mdev_device *mdev;
	struct mutex lock;

	u8 vcfg[NOVA_CFG_SPACE_SIZE];
	void *bar1_mem;

	u32 pmc_boot0;
	u32 pmc_boot42;
	u32 pmc_enable;
	u32 vgpu_config;
	u32 vgpu_fb_pages_m1;
	u32 vgpu_intr_status;
	u32 vgpu_intr_enable;
	u32 vgpu_msg_ring_ctl;
	u32 cstatus_mb;
	u32 pgraph_fs;

	u32 send_ring_lo;
	u32 send_ring_hi;
	u32 send_ring_put;
	u32 send_ring_get;

	u32 recv_ring_lo;
	u32 recv_ring_hi;
	u32 recv_ring_put;
	u32 recv_ring_get;

	u32 event_ring_lo;
	u32 event_ring_hi;
	u32 event_ring_put;
	u32 event_ring_get;

	bool guest_vgx_initialized;
	u64 rpc_processed;

	struct eventfd_ctx *intx_trigger;
	struct eventfd_ctx *msi_trigger;
	bool intx_masked;
};

static u32 g_phys_pmc_boot0 = 0x134000a1U;
static atomic_t g_active_instances = ATOMIC_INIT(0);
static struct platform_device *g_pdev;
static struct mdev_parent g_parent;

static struct mdev_type g_type_p4_2q = {
	.sysfs_name = "nvidia-64",
	.pretty_name = "GRID P4-2Q (clean-room open mediator)",
};

static struct mdev_type *g_mdev_types[] = {
	&g_type_p4_2q,
};

static inline u64 decode_ring_gpa(u32 lo, u32 hi)
{
	return (((u64)(hi & 0x000fffffU)) << 32) | ((u64)(lo & 0xfffff000U));
}

static void nova_mdev_reset_state(struct nova_mdev_state *s)
{
	s->pmc_boot0 = g_phys_pmc_boot0;
	s->pmc_boot42 = 0x134a1000U;
	s->pmc_enable = 0x00010100U;
	s->vgpu_config = 0x00040201U; /* Bit 0 active, Bit 18 ECC supported */
	s->vgpu_fb_pages_m1 = (u32)((NOVA_P4_2Q_FB_BYTES - 1ULL) >> 12);
	s->vgpu_intr_status = 0;
	s->vgpu_intr_enable = 0;
	s->vgpu_msg_ring_ctl = 0;
	s->cstatus_mb = (u32)(NOVA_P4_2Q_FB_BYTES >> 20);
	s->pgraph_fs = 0x000f0000U;

	s->send_ring_lo = 0;
	s->send_ring_hi = 0;
	s->send_ring_put = 0;
	s->send_ring_get = 0;
	s->recv_ring_lo = 0;
	s->recv_ring_hi = 0;
	s->recv_ring_put = 0;
	s->recv_ring_get = 0;
	s->event_ring_lo = 0;
	s->event_ring_hi = 0;
	s->event_ring_put = 0;
	s->event_ring_get = 0;
	s->guest_vgx_initialized = false;
	s->intx_masked = false;
}

static void nova_mdev_init_vcfg(struct nova_mdev_state *s)
{
	memset(s->vcfg, 0, sizeof(s->vcfg));

	/* Vendor=0x10de, Device=0x1bb3 (GP104GL Tesla P4) */
	*(__le16 *)&s->vcfg[PCI_VENDOR_ID] = cpu_to_le16(NOVA_PCI_VENDOR_NVIDIA);
	*(__le16 *)&s->vcfg[PCI_DEVICE_ID] = cpu_to_le16(NOVA_PCI_DEVICE_TESLA_P4);
	*(__le16 *)&s->vcfg[PCI_STATUS] = cpu_to_le16(PCI_STATUS_CAP_LIST);
	s->vcfg[PCI_REVISION_ID] = 0xa1;
	s->vcfg[PCI_CLASS_PROG] = 0x00;
	s->vcfg[PCI_CLASS_DEVICE] = 0x02;     /* 3D controller subclass */
	s->vcfg[PCI_CLASS_DEVICE + 1] = 0x03; /* Display controller base class */
	s->vcfg[PCI_HEADER_TYPE] = PCI_HEADER_TYPE_NORMAL;

	/* BAR0 (16 MiB 32-bit MEM) and BAR1 (16 MiB 32-bit MEM) */
	*(__le32 *)&s->vcfg[PCI_BASE_ADDRESS_0] = cpu_to_le32(0x00000000U);
	*(__le32 *)&s->vcfg[PCI_BASE_ADDRESS_1] = cpu_to_le32(0x00000000U);

	*(__le16 *)&s->vcfg[PCI_SUBSYSTEM_VENDOR_ID] = cpu_to_le16(NOVA_PCI_VENDOR_NVIDIA);
	*(__le16 *)&s->vcfg[PCI_SUBSYSTEM_ID] = cpu_to_le16(NOVA_PCI_SUBSYS_P4_2Q);

	s->vcfg[PCI_CAPABILITY_LIST] = NOVA_PCI_MSI_CAP_OFF;
	s->vcfg[PCI_INTERRUPT_PIN] = 0x01; /* INTA# */

	/* MSI Capability @ 0x68: ID=0x05, next=0, ctrl=0x0080 (64-bit, 1 vec) */
	s->vcfg[NOVA_PCI_MSI_CAP_OFF + PCI_CAP_LIST_ID] = PCI_CAP_ID_MSI;
	s->vcfg[NOVA_PCI_MSI_CAP_OFF + PCI_CAP_LIST_NEXT] = 0x00;
	*(__le16 *)&s->vcfg[NOVA_PCI_MSI_CAP_OFF + PCI_MSI_FLAGS] =
		cpu_to_le16(PCI_MSI_FLAGS_64BIT);
}

static void nova_mdev_signal_irq(struct nova_mdev_state *s)
{
	u16 msi_flags = le16_to_cpu(*(__le16 *)&s->vcfg[NOVA_PCI_MSI_CAP_OFF + PCI_MSI_FLAGS]);

	if ((msi_flags & PCI_MSI_FLAGS_ENABLE) && s->msi_trigger) {
		eventfd_signal(s->msi_trigger);
		return;
	}
	if (s->intx_trigger && !s->intx_masked)
		eventfd_signal(s->intx_trigger);
}

static void nova_mdev_process_vrpc_ring(struct nova_mdev_state *s)
{
	u64 send_gpa = decode_ring_gpa(s->send_ring_lo, s->send_ring_hi);
	u64 recv_gpa = decode_ring_gpa(s->recv_ring_lo, s->recv_ring_hi);
	u8 *page_buf;

	if (!send_gpa || !recv_gpa || s->send_ring_get == s->send_ring_put)
		return;

	page_buf = kmalloc(4096, GFP_KERNEL);
	if (!page_buf)
		return;

	while (s->send_ring_get != s->send_ring_put) {
		__le32 desc[2];
		u32 slot_off = s->send_ring_get & 0xff8U;
		u32 recv_off = s->recv_ring_put & 0xff8U;
		u64 msg_gpa;
		struct nova_vrpc_header *hdr;
		u32 func_id, sig, status;
		int ret;

		ret = vfio_dma_rw(&s->vdev, send_gpa + slot_off, desc, sizeof(desc), false);
		if (ret) {
			pr_err(DRV_NAME ": vfio_dma_rw SEND_RING read failed (%d)\n", ret);
			break;
		}

		msg_gpa = decode_ring_gpa(le32_to_cpu(desc[0]), le32_to_cpu(desc[1]));
		ret = vfio_dma_rw(&s->vdev, msg_gpa, page_buf, 4096, false);
		if (ret) {
			pr_err(DRV_NAME ": vfio_dma_rw staging read @0x%llx failed (%d)\n",
			       msg_gpa, ret);
			break;
		}

		hdr = (struct nova_vrpc_header *)page_buf;
		sig = le32_to_cpu(hdr->signature);
		func_id = le32_to_cpu(hdr->function);

		if (sig != NOVA_VRPC_MAGIC) {
			status = 0x0000001fU; /* NV_ERR_INVALID_ARGUMENT */
		} else if (func_id != 0x01U && !s->guest_vgx_initialized) {
			status = 0x00000056U; /* NV_ERR_NOT_SUPPORTED until SetGuestSystemInfo */
		} else {
			switch (func_id) {
			case 0x01U: /* SET_GUEST_SYSTEM_INFO */
				s->guest_vgx_initialized = true;
				status = 0U;
				break;
			case 0x32U: /* GET_STATIC_INFO (R535) */
			case 0x55U: { /* legacy alias */
				__le64 fb_le = cpu_to_le64(NOVA_P4_2Q_FB_BYTES);
				__le16 dev_le = cpu_to_le16(NOVA_PCI_DEVICE_TESLA_P4);
				__le16 sub_le = cpu_to_le16(NOVA_PCI_SUBSYS_P4_2Q);

				memcpy(page_buf + NOVA_VRPC_HEADER_BYTES, &fb_le, sizeof(fb_le));
				memcpy(page_buf + NOVA_VRPC_HEADER_BYTES + 8, &dev_le, sizeof(dev_le));
				memcpy(page_buf + NOVA_VRPC_HEADER_BYTES + 10, &sub_le, sizeof(sub_le));
				status = 0U;
				break;
			}
			case 0x00U: /* NOP */
			case 0x02U: /* ALLOC_ROOT */
			case 0x03U: /* ALLOC_DEVICE */
			case 0x04U: /* ALLOC_MEMORY */
			case 0x06U: /* ALLOC_CHANNEL_DMA */
			case 0x07U: /* MAP_MEMORY */
			case 0x09U: /* ALLOC_OBJECT */
			case 0x0aU: /* FREE */
			case 0x16U: /* IDLE_CHANNELS */
			case 0x1aU: /* RM_CONTROL / DMA_CONTROL */
			case 0x3fU: /* SET_GUEST_SYSTEM_INFO_EXT */
			case 0x4cU: /* GET_STATIC_INFO2 */
			case 0x4fU: /* GET_CONSOLIDATED_STATIC_INFO */
				status = 0U;
				break;
			default:
				status = 0x00000056U;
				break;
			}
		}

		hdr->result = cpu_to_le32(status);
		ret = vfio_dma_rw(&s->vdev, msg_gpa, page_buf, 4096, true);
		if (ret) {
			pr_err(DRV_NAME ": vfio_dma_rw staging write failed (%d)\n", ret);
			break;
		}

		ret = vfio_dma_rw(&s->vdev, recv_gpa + recv_off, desc, sizeof(desc), true);
		if (ret) {
			pr_err(DRV_NAME ": vfio_dma_rw RECV_RING write failed (%d)\n", ret);
			break;
		}

		s->recv_ring_put = (s->recv_ring_put + 8U) & 0xfffU;
		s->send_ring_get = (s->send_ring_get + 8U) & 0xfffU;
		s->vgpu_intr_status |= 0x40U;
		s->rpc_processed++;
		nova_mdev_signal_irq(s);
	}

	kfree(page_buf);
}

static u32 nova_mdev_bar0_read32(struct nova_mdev_state *s, u64 off)
{
	if (off >= NOVA_NV_VGPU_BASE && off < NOVA_NV_VGPU_BASE + 0x1000U) {
		switch (off - NOVA_NV_VGPU_BASE) {
		case NOVA_VGPU_SEND_RING_LO:  return s->send_ring_lo;
		case NOVA_VGPU_SEND_RING_HI:  return s->send_ring_hi;
		case NOVA_VGPU_SEND_RING_PUT: return s->send_ring_put;
		case NOVA_VGPU_SEND_RING_GET: return s->send_ring_get;
		case NOVA_VGPU_RECV_RING_LO:  return s->recv_ring_lo;
		case NOVA_VGPU_RECV_RING_HI:  return s->recv_ring_hi;
		case NOVA_VGPU_RECV_RING_PUT: return s->recv_ring_put;
		case NOVA_VGPU_RECV_RING_GET: return s->recv_ring_get;
		case NOVA_VGPU_CONFIG:        return s->vgpu_config;
		case NOVA_VGPU_FB_SIZE_4K_M1: return s->vgpu_fb_pages_m1;
		case NOVA_VGPU_INTR_STATUS:   return s->vgpu_intr_status;
		case NOVA_VGPU_INTR_ENABLE:   return s->vgpu_intr_enable;
		case NOVA_VGPU_EVENT_RING_LO: return s->event_ring_lo;
		case NOVA_VGPU_EVENT_RING_HI: return s->event_ring_hi;
		case NOVA_VGPU_EVENT_RING_PUT:return s->event_ring_put;
		case NOVA_VGPU_EVENT_RING_GET:return s->event_ring_get;
		case NOVA_VGPU_MSG_RING_CTL:  return s->vgpu_msg_ring_ctl;
		default:                      return 0U;
		}
	}

	switch (off) {
	case NOVA_NV_PMC_BOOT_0:         return s->pmc_boot0;
	case NOVA_NV_PMC_ENABLE:         return s->pmc_enable;
	case NOVA_NV_PMC_BOOT_42:        return s->pmc_boot42;
	case NOVA_NV_PFB_FBPA_0_CSTATUS: return s->cstatus_mb;
	case NOVA_NV_PGRAPH_FS_0:        return s->pgraph_fs;
	default:                         return 0U;
	}
}

static void nova_mdev_bar0_write32(struct nova_mdev_state *s, u64 off, u32 val)
{
	if (off >= NOVA_NV_VGPU_BASE && off < NOVA_NV_VGPU_BASE + 0x1000U) {
		switch (off - NOVA_NV_VGPU_BASE) {
		case NOVA_VGPU_SEND_RING_LO:  s->send_ring_lo = val; break;
		case NOVA_VGPU_SEND_RING_HI:  s->send_ring_hi = val; break;
		case NOVA_VGPU_SEND_RING_PUT:
			s->send_ring_put = val & 0xfffU;
			nova_mdev_process_vrpc_ring(s);
			break;
		case NOVA_VGPU_RECV_RING_LO:  s->recv_ring_lo = val; break;
		case NOVA_VGPU_RECV_RING_HI:  s->recv_ring_hi = val; break;
		case NOVA_VGPU_RECV_RING_GET: s->recv_ring_get = val & 0xfffU; break;
		case NOVA_VGPU_INTR_STATUS:   s->vgpu_intr_status &= ~(val & 0x3c0U); break;
		case NOVA_VGPU_INTR_ENABLE:   s->vgpu_intr_enable = val & 0x1ffU; break;
		case NOVA_VGPU_EVENT_RING_LO: s->event_ring_lo = val; break;
		case NOVA_VGPU_EVENT_RING_HI: s->event_ring_hi = val; break;
		case NOVA_VGPU_EVENT_RING_GET:s->event_ring_get = val & 0xfffU; break;
		case NOVA_VGPU_RPC_DOORBELL:
			nova_mdev_process_vrpc_ring(s);
			break;
		case NOVA_VGPU_MSG_RING_CTL:  s->vgpu_msg_ring_ctl = val; break;
		default: break;
		}
		return;
	}

	if (off == NOVA_NV_PMC_ENABLE)
		s->pmc_enable = val | 0x00010100U;
}

static ssize_t nova_vfio_cfg_rw(struct nova_mdev_state *s, char *buf,
				size_t count, loff_t pos, bool is_write)
{
	if (pos < 0 || pos >= NOVA_CFG_SPACE_SIZE || count > NOVA_CFG_SPACE_SIZE ||
	    pos + count > NOVA_CFG_SPACE_SIZE)
		return -EINVAL;

	if (!is_write) {
		memcpy(buf, &s->vcfg[pos], count);
		return count;
	}

	if (count > 4)
		return -EINVAL;

	/* Handle standard PCI 32-bit BAR0 / BAR1 sizing masks */
	if ((pos == PCI_BASE_ADDRESS_0 || pos == PCI_BASE_ADDRESS_1) && count == 4) {
		u32 val = le32_to_cpu(*(__le32 *)buf);
		u32 mask = ~(u32)(NOVA_BAR0_SIZE - 1UL);

		val &= mask;
		*(__le32 *)&s->vcfg[pos] = cpu_to_le32(val);
		return 4;
	}

	/* Protect read-only vendor/device/class/subsys IDs, allow command/MSI/latency writes */
	if (pos >= PCI_BASE_ADDRESS_2 && pos < PCI_BASE_ADDRESS_0 + 24)
		return count;
	if (pos == PCI_VENDOR_ID || pos == PCI_DEVICE_ID ||
	    pos == PCI_SUBSYSTEM_VENDOR_ID || pos == PCI_SUBSYSTEM_ID)
		return count;

	memcpy(&s->vcfg[pos], buf, count);
	return count;
}

static int nova_vfio_open_device(struct vfio_device *vdev)
{
	struct nova_mdev_state *s = container_of(vdev, struct nova_mdev_state, vdev);

	mutex_lock(&s->lock);
	nova_mdev_init_vcfg(s);
	nova_mdev_reset_state(s);
	mutex_unlock(&s->lock);
	return 0;
}

static void nova_vfio_close_device(struct vfio_device *vdev)
{
	struct nova_mdev_state *s = container_of(vdev, struct nova_mdev_state, vdev);
	u64 bar1_sample = 0;

	mutex_lock(&s->lock);
	if (s->bar1_mem)
		bar1_sample = *(u64 *)s->bar1_mem;
	pr_info(DRV_NAME ": close mdev (rpcs=%llu, bar1[0]=0x%016llx)\n",
		s->rpc_processed, bar1_sample);

	if (s->intx_trigger) {
		eventfd_ctx_put(s->intx_trigger);
		s->intx_trigger = NULL;
	}
	if (s->msi_trigger) {
		eventfd_ctx_put(s->msi_trigger);
		s->msi_trigger = NULL;
	}
	nova_mdev_reset_state(s);
	mutex_unlock(&s->lock);
}

static ssize_t nova_vfio_read(struct vfio_device *vdev, char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct nova_mdev_state *s = container_of(vdev, struct nova_mdev_state, vdev);
	unsigned int index = NOVA_VFIO_OFFSET_TO_INDEX(*ppos);
	u64 off = NOVA_VFIO_OFFSET_TO_OFF(*ppos);
	u8 kbuf[8];
	ssize_t ret;

	mutex_lock(&s->lock);
	switch (index) {
	case VFIO_PCI_CONFIG_REGION_INDEX:
		if (off >= NOVA_CFG_SPACE_SIZE || count > NOVA_CFG_SPACE_SIZE ||
		    off + count > NOVA_CFG_SPACE_SIZE) {
			ret = -EINVAL;
			break;
		}
		ret = copy_to_user(buf, &s->vcfg[off], count) ? -EFAULT : count;
		break;
	case VFIO_PCI_BAR0_REGION_INDEX:
		if (off + count > NOVA_BAR0_SIZE || count > 8 || (count != 1 && count != 2 && count != 4 && count != 8)) {
			ret = -EINVAL;
			break;
		}
		if (count < 4) {
			u32 v = nova_mdev_bar0_read32(s, off & ~3ULL) >> ((off & 3ULL) * 8);

			memcpy(kbuf, &v, count);
			ret = copy_to_user(buf, kbuf, count) ? -EFAULT : count;
		} else {
			u32 v = nova_mdev_bar0_read32(s, off & ~3ULL);

			memcpy(kbuf, &v, 4);
			if (count == 8) {
				u32 hi = nova_mdev_bar0_read32(s, (off & ~3ULL) + 4);

				memcpy(kbuf + 4, &hi, 4);
			}
			ret = copy_to_user(buf, kbuf, count) ? -EFAULT : count;
		}
		break;
	case VFIO_PCI_BAR1_REGION_INDEX:
		if (off + count > NOVA_BAR1_SIZE)
			ret = -EINVAL;
		else
			ret = copy_to_user(buf, (u8 *)s->bar1_mem + off, count) ? -EFAULT : count;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&s->lock);

	if (ret > 0)
		*ppos += ret;
	return ret;
}

static ssize_t nova_vfio_write(struct vfio_device *vdev, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct nova_mdev_state *s = container_of(vdev, struct nova_mdev_state, vdev);
	unsigned int index = NOVA_VFIO_OFFSET_TO_INDEX(*ppos);
	u64 off = NOVA_VFIO_OFFSET_TO_OFF(*ppos);
	u8 kbuf[8];
	ssize_t ret;

	mutex_lock(&s->lock);
	switch (index) {
	case VFIO_PCI_CONFIG_REGION_INDEX:
		if (count > sizeof(kbuf)) {
			ret = -EINVAL;
			break;
		}
		if (copy_from_user(kbuf, buf, count)) {
			ret = -EFAULT;
			break;
		}
		ret = nova_vfio_cfg_rw(s, kbuf, count, off, true);
		break;
	case VFIO_PCI_BAR0_REGION_INDEX:
		if (off + count > NOVA_BAR0_SIZE || count > 8 || (off & 3)) {
			ret = -EINVAL;
			break;
		}
		if (copy_from_user(kbuf, buf, count)) {
			ret = -EFAULT;
			break;
		}
		{
			u32 v = 0;
			memcpy(&v, kbuf, min_t(size_t, count, 4));
			nova_mdev_bar0_write32(s, off, v);
			if (count == 8) {
				u32 hi = 0;
				memcpy(&hi, kbuf + 4, 4);
				nova_mdev_bar0_write32(s, off + 4, hi);
			}
			ret = count;
		}
		break;
	case VFIO_PCI_BAR1_REGION_INDEX:
		if (off + count > NOVA_BAR1_SIZE)
			ret = -EINVAL;
		else
			ret = copy_from_user((u8 *)s->bar1_mem + off, buf, count) ? -EFAULT : count;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&s->lock);

	if (ret > 0)
		*ppos += ret;
	return ret;
}

static int nova_vfio_mmap(struct vfio_device *vdev, struct vm_area_struct *vma)
{
	struct nova_mdev_state *s = container_of(vdev, struct nova_mdev_state, vdev);
	unsigned int index = vma->vm_pgoff >> (NOVA_VFIO_REGION_SHIFT - PAGE_SHIFT);
	unsigned long pgoff = vma->vm_pgoff & ((1UL << (NOVA_VFIO_REGION_SHIFT - PAGE_SHIFT)) - 1UL);

	if (index != VFIO_PCI_BAR1_REGION_INDEX)
		return -EINVAL;
	if ((pgoff << PAGE_SHIFT) + (vma->vm_end - vma->vm_start) > NOVA_BAR1_SIZE)
		return -EINVAL;

	return remap_vmalloc_range(vma, s->bar1_mem, pgoff);
}

static int nova_vfio_set_irqs(struct nova_mdev_state *s, struct vfio_irq_set *hdr,
			      void __user *data)
{
	struct eventfd_ctx **target;

	if (hdr->index == VFIO_PCI_INTX_IRQ_INDEX)
		target = &s->intx_trigger;
	else if (hdr->index == VFIO_PCI_MSI_IRQ_INDEX)
		target = &s->msi_trigger;
	else
		return -EINVAL;

	if (hdr->flags & VFIO_IRQ_SET_ACTION_MASK) {
		if (hdr->index == VFIO_PCI_INTX_IRQ_INDEX)
			s->intx_masked = true;
		return 0;
	}
	if (hdr->flags & VFIO_IRQ_SET_ACTION_UNMASK) {
		if (hdr->index == VFIO_PCI_INTX_IRQ_INDEX)
			s->intx_masked = false;
		return 0;
	}
	if (!(hdr->flags & VFIO_IRQ_SET_ACTION_TRIGGER))
		return -EINVAL;

	if (hdr->flags & VFIO_IRQ_SET_DATA_NONE) {
		if (hdr->count == 0) {
			if (*target) {
				eventfd_ctx_put(*target);
				*target = NULL;
			}
		} else if (*target) {
			eventfd_signal(*target);
		}
		return 0;
	}

	if (hdr->flags & VFIO_IRQ_SET_DATA_BOOL) {
		u8 trigger = 0;

		if (hdr->count && copy_from_user(&trigger, data, 1))
			return -EFAULT;
		if (trigger && *target)
			eventfd_signal(*target);
		return 0;
	}

	if (hdr->flags & VFIO_IRQ_SET_DATA_EVENTFD) {
		s32 fd = -1;
		struct eventfd_ctx *ctx;

		if (hdr->count == 0)
			return 0;
		if (copy_from_user(&fd, data, sizeof(fd)))
			return -EFAULT;
		if (*target) {
			eventfd_ctx_put(*target);
			*target = NULL;
		}
		if (fd < 0)
			return 0;
		ctx = eventfd_ctx_fdget(fd);
		if (IS_ERR(ctx))
			return PTR_ERR(ctx);
		*target = ctx;
		return 0;
	}

	return -EINVAL;
}

static int nova_vfio_get_region_info_caps(struct vfio_device *vdev,
					struct vfio_region_info *info,
					struct vfio_info_cap *caps)
{
	info->cap_offset = 0;
	info->offset = NOVA_VFIO_REGION_OFFSET(info->index);

	switch (info->index) {
	case VFIO_PCI_BAR0_REGION_INDEX:
		info->size = NOVA_BAR0_SIZE;
		info->flags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE;
		return 0;
	case VFIO_PCI_BAR1_REGION_INDEX:
		info->size = NOVA_BAR1_SIZE;
		info->flags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE |
			      VFIO_REGION_INFO_FLAG_MMAP;
		return 0;
	case VFIO_PCI_CONFIG_REGION_INDEX:
		info->size = NOVA_CFG_SPACE_SIZE;
		info->flags = VFIO_REGION_INFO_FLAG_READ | VFIO_REGION_INFO_FLAG_WRITE;
		return 0;
	default:
		if (info->index >= VFIO_PCI_NUM_REGIONS)
			return -EINVAL;
		info->size = 0;
		info->flags = 0;
		return 0;
	}
}

static long nova_vfio_ioctl(struct vfio_device *vdev, unsigned int cmd, unsigned long arg)
{
	struct nova_mdev_state *s = container_of(vdev, struct nova_mdev_state, vdev);
	unsigned long minsz;

	switch (cmd) {
	case VFIO_DEVICE_GET_INFO: {
		struct vfio_device_info info;

		minsz = offsetofend(struct vfio_device_info, num_irqs);
		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;
		if (info.argsz < minsz)
			return -EINVAL;
		info.flags = VFIO_DEVICE_FLAGS_PCI | VFIO_DEVICE_FLAGS_RESET;
		info.num_regions = VFIO_PCI_NUM_REGIONS;
		info.num_irqs = VFIO_PCI_NUM_IRQS;
		if (copy_to_user((void __user *)arg, &info, minsz))
			return -EFAULT;
		return 0;
	}
	case VFIO_DEVICE_GET_REGION_INFO: {
		struct vfio_region_info info;
		struct vfio_info_cap caps = { .buf = NULL, .size = 0 };
		int ret;

		minsz = offsetofend(struct vfio_region_info, offset);
		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;
		if (info.argsz < minsz)
			return -EINVAL;
		ret = nova_vfio_get_region_info_caps(vdev, &info, &caps);
		if (ret)
			return ret;
		if (copy_to_user((void __user *)arg, &info, minsz))
			return -EFAULT;
		return 0;
	}
	case VFIO_DEVICE_GET_IRQ_INFO: {
		struct vfio_irq_info info;

		minsz = offsetofend(struct vfio_irq_info, count);
		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;
		if (info.argsz < minsz || info.index >= VFIO_PCI_NUM_IRQS)
			return -EINVAL;

		if (info.index == VFIO_PCI_INTX_IRQ_INDEX) {
			info.count = 1;
			info.flags = VFIO_IRQ_INFO_EVENTFD | VFIO_IRQ_INFO_MASKABLE |
				     VFIO_IRQ_INFO_AUTOMASKED;
		} else if (info.index == VFIO_PCI_MSI_IRQ_INDEX) {
			info.count = 1;
			info.flags = VFIO_IRQ_INFO_EVENTFD;
		} else {
			info.count = 0;
			info.flags = 0;
		}
		if (copy_to_user((void __user *)arg, &info, minsz))
			return -EFAULT;
		return 0;
	}
	case VFIO_DEVICE_SET_IRQS: {
		struct vfio_irq_set hdr;
		size_t data_size = 0;
		int ret;

		minsz = offsetofend(struct vfio_irq_set, count);
		if (copy_from_user(&hdr, (void __user *)arg, minsz))
			return -EFAULT;
		ret = vfio_set_irqs_validate_and_prepare(&hdr, 1, VFIO_PCI_NUM_IRQS, &data_size);
		if (ret)
			return ret;
		mutex_lock(&s->lock);
		ret = nova_vfio_set_irqs(s, &hdr, (void __user *)(arg + minsz));
		mutex_unlock(&s->lock);
		return ret;
	}
	case VFIO_DEVICE_RESET:
		mutex_lock(&s->lock);
		nova_mdev_init_vcfg(s);
		nova_mdev_reset_state(s);
		mutex_unlock(&s->lock);
		return 0;
	default:
		return -ENOTTY;
	}
}

static void nova_vfio_release(struct vfio_device *vdev)
{
	struct nova_mdev_state *s = container_of(vdev, struct nova_mdev_state, vdev);

	vfree(s->bar1_mem);
	s->bar1_mem = NULL;
}

static const struct vfio_device_ops nova_vfio_dev_ops = {
	.name = "nova-vgpu-vfio-mdev",
	.release = nova_vfio_release,
	.bind_iommufd = vfio_iommufd_emulated_bind,
	.unbind_iommufd = vfio_iommufd_emulated_unbind,
	.attach_ioas = vfio_iommufd_emulated_attach_ioas,
	.detach_ioas = vfio_iommufd_emulated_detach_ioas,
	.open_device = nova_vfio_open_device,
	.close_device = nova_vfio_close_device,
	.read = nova_vfio_read,
	.write = nova_vfio_write,
	.ioctl = nova_vfio_ioctl,
	.get_region_info_caps = nova_vfio_get_region_info_caps,
	.mmap = nova_vfio_mmap,
};

static int nova_mdev_probe(struct mdev_device *mdev)
{
	struct nova_mdev_state *s;
	int ret;

	s = vfio_alloc_device(nova_mdev_state, vdev, &mdev->dev, &nova_vfio_dev_ops);
	if (IS_ERR(s))
		return PTR_ERR(s);

	s->bar1_mem = vmalloc_user(NOVA_BAR1_SIZE);
	if (!s->bar1_mem) {
		vfio_put_device(&s->vdev);
		return -ENOMEM;
	}

	s->mdev = mdev;
	mutex_init(&s->lock);
	nova_mdev_init_vcfg(s);
	nova_mdev_reset_state(s);

	ret = vfio_register_emulated_iommu_dev(&s->vdev);
	if (ret) {
		vfio_put_device(&s->vdev);
		return ret;
	}

	dev_set_drvdata(&mdev->dev, s);
	atomic_inc(&g_active_instances);
	pr_info(DRV_NAME ": created clean-room GRID P4-2Q mdev %pUl\n", &mdev->uuid);
	return 0;
}

static void nova_mdev_remove(struct mdev_device *mdev)
{
	struct nova_mdev_state *s = dev_get_drvdata(&mdev->dev);

	if (!s)
		return;

	vfio_unregister_group_dev(&s->vdev);
	vfio_put_device(&s->vdev);
	atomic_dec(&g_active_instances);
	pr_info(DRV_NAME ": removed clean-room GRID P4-2Q mdev %pUl\n", &mdev->uuid);
}

static unsigned int nova_mdev_get_available(struct mdev_type *mtype)
{
	int used = atomic_read(&g_active_instances);

	return used >= 4 ? 0U : (unsigned int)(4 - used);
}

static ssize_t nova_mdev_show_description(struct mdev_type *mtype, char *buf)
{
	return sysfs_emit(buf,
			  "name=GRID P4-2Q,framebuffer=1856M,max_instance=4,pci=0x10de:0x1bb3\n");
}

static struct mdev_driver nova_vgpu_mdev_driver = {
	.device_api = VFIO_DEVICE_API_PCI_STRING,
	.max_instances = 4,
	.probe = nova_mdev_probe,
	.remove = nova_mdev_remove,
	.get_available = nova_mdev_get_available,
	.show_description = nova_mdev_show_description,
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
	},
};

static int __init nova_vgpu_mdev_init(void)
{
	struct pci_dev *pdev;
	int ret;

	/* Read-only physical Tesla P4 discovery without disturbing live nvidia.ko */
	pdev = pci_get_device(NOVA_PCI_VENDOR_NVIDIA, NOVA_PCI_DEVICE_TESLA_P4, NULL);
	if (pdev) {
		resource_size_t bar0_phys = pci_resource_start(pdev, 0);

		if (bar0_phys) {
			void __iomem *mmio = ioremap(bar0_phys, 4);

			if (mmio) {
				g_phys_pmc_boot0 = readl(mmio);
				iounmap(mmio);
			}
		}
		pr_info(DRV_NAME ": observed physical Tesla P4 at %s (BOOT0=0x%08x)\n",
			pci_name(pdev), g_phys_pmc_boot0);
		pci_dev_put(pdev);
	}

	g_pdev = platform_device_register_simple("nova_vgpu_parent", -1, NULL, 0);
	if (IS_ERR(g_pdev))
		return PTR_ERR(g_pdev);

	ret = mdev_register_driver(&nova_vgpu_mdev_driver);
	if (ret) {
		platform_device_unregister(g_pdev);
		return ret;
	}

	ret = mdev_register_parent(&g_parent, &g_pdev->dev, &nova_vgpu_mdev_driver,
				   g_mdev_types, ARRAY_SIZE(g_mdev_types));
	if (ret) {
		mdev_unregister_driver(&nova_vgpu_mdev_driver);
		platform_device_unregister(g_pdev);
		return ret;
	}

	pr_info(DRV_NAME ": registered mdev parent (type nvidia-64, GRID P4-2Q)\n");
	return 0;
}

static void __exit nova_vgpu_mdev_exit(void)
{
	mdev_unregister_parent(&g_parent);
	mdev_unregister_driver(&nova_vgpu_mdev_driver);
	platform_device_unregister(g_pdev);
}

module_init(nova_vgpu_mdev_init);
module_exit(nova_vgpu_mdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Clean-Room Pascal vGPU Project");
MODULE_DESCRIPTION("Linux 7.0 vfio-mdev host mediator for clean-room Pascal GP104 vGPU KVM testing");
