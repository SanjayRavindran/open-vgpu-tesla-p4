// SPDX-License-Identifier: GPL-2.0
/*
 * nova_vgpu_guest.c - Clean-room open-source Linux guest kernel driver for
 * NVIDIA Pascal (GP104GL / Tesla P4) KVM vGPU devices.
 *
 * Implements the paravirtualized NV_VGPU BAR0 (0x8f000..0x8f054) + "VRPC"
 * SEND_RING/RECV_RING/EVENT_RING RPC transport cleanly without any proprietary
 * NVIDIA nv-kernel.o_binary or signed Falcon/GSP firmware blobs.
 */

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "nova_vgpu_guest.h"

#define DRV_NAME "nova_vgpu_guest"
#define RING_SIZE_BYTES 4096U
#define VRPC_POLL_TIMEOUT_US 2000000U

static bool allow_baremetal_probe;
module_param(allow_baremetal_probe, bool, 0444);
MODULE_PARM_DESC(allow_baremetal_probe,
		 "Refuse probing bare-metal GPUs unless explicitly set (default false)");

struct nova_vgpu_guest_dev {
	struct pci_dev *pdev;
	void __iomem *bar0;
	resource_size_t bar0_len;
	resource_size_t bar1_start;
	resource_size_t bar1_len;

	/* 4 KiB DMA-coherent ring buffers & VRPC staging buffer */
	void *send_ring;
	dma_addr_t send_ring_dma;
	void *recv_ring;
	dma_addr_t recv_ring_dma;
	void *event_ring;
	dma_addr_t event_ring_dma;
	void *vrpc_page;
	dma_addr_t vrpc_page_dma;

	u32 send_put;
	u32 recv_get;
	u32 next_seq;

	u32 pmc_boot0;
	u32 vgpu_config;
	u32 cstatus_mb;
	u64 virtual_fb_bytes;

	atomic64_t rpc_completed;
	atomic64_t irq_count;
	struct mutex rpc_lock;

	int irq;
	struct miscdevice miscdev;
	char devname[32];
};

static atomic_t g_guest_minor_seq = ATOMIC_INIT(0);

static inline u32 nova_ring_pte_lo(dma_addr_t dma)
{
	u32 pfn_lo = (u32)((dma >> 12) & 0x000fffffULL);

	return (pfn_lo << 12) | 0x0000000bU;
}

static inline u32 nova_ring_pte_hi(dma_addr_t dma)
{
	return (u32)((dma >> 32) & 0x000fffffULL);
}

static irqreturn_t nova_vgpu_guest_irq(int irq, void *data)
{
	struct nova_vgpu_guest_dev *gdev = data;
	u32 status;

	status = ioread32(gdev->bar0 + NOVA_NV_VGPU_INTR_STATUS);
	if (!status)
		return IRQ_NONE;

	/* W1C bits [9:6] matching host _nv007414vgpu */
	iowrite32(status & 0x3c0U, gdev->bar0 + NOVA_NV_VGPU_INTR_STATUS);
	atomic64_inc(&gdev->irq_count);
	return IRQ_HANDLED;
}

static void nova_vgpu_program_rings(struct nova_vgpu_guest_dev *gdev)
{
	/* SEND_RING (0x8f000..0x8f008) */
	iowrite32(0, gdev->bar0 + NOVA_NV_VGPU_SEND_RING_LO);
	iowrite32(0, gdev->bar0 + NOVA_NV_VGPU_SEND_RING_PUT);
	iowrite32(nova_ring_pte_hi(gdev->send_ring_dma),
		  gdev->bar0 + NOVA_NV_VGPU_SEND_RING_HI);
	iowrite32(nova_ring_pte_lo(gdev->send_ring_dma),
		  gdev->bar0 + NOVA_NV_VGPU_SEND_RING_LO);

	/* RECV_RING (0x8f010..0x8f01c) */
	iowrite32(0, gdev->bar0 + NOVA_NV_VGPU_RECV_RING_LO);
	iowrite32(0, gdev->bar0 + NOVA_NV_VGPU_RECV_RING_GET);
	iowrite32(nova_ring_pte_hi(gdev->recv_ring_dma),
		  gdev->bar0 + NOVA_NV_VGPU_RECV_RING_HI);
	iowrite32(nova_ring_pte_lo(gdev->recv_ring_dma),
		  gdev->bar0 + NOVA_NV_VGPU_RECV_RING_LO);

	/* EVENT_RING (0x8f030..0x8f03c) */
	iowrite32(0, gdev->bar0 + NOVA_NV_VGPU_EVENT_RING_LO);
	iowrite32(0, gdev->bar0 + NOVA_NV_VGPU_EVENT_RING_GET);
	iowrite32(nova_ring_pte_hi(gdev->event_ring_dma),
		  gdev->bar0 + NOVA_NV_VGPU_EVENT_RING_HI);
	iowrite32(nova_ring_pte_lo(gdev->event_ring_dma),
		  gdev->bar0 + NOVA_NV_VGPU_EVENT_RING_LO);

	iowrite32(0x1ffU, gdev->bar0 + NOVA_NV_VGPU_INTR_ENABLE);
}

static void nova_vgpu_teardown_rings(struct nova_vgpu_guest_dev *gdev)
{
	iowrite32(0, gdev->bar0 + NOVA_NV_VGPU_INTR_ENABLE);
	iowrite32(0, gdev->bar0 + NOVA_NV_VGPU_SEND_RING_LO);
	iowrite32(0, gdev->bar0 + NOVA_NV_VGPU_RECV_RING_LO);
	iowrite32(0, gdev->bar0 + NOVA_NV_VGPU_EVENT_RING_LO);
}

static int nova_vgpu_submit_vrpc(struct nova_vgpu_guest_dev *gdev, u32 func_id,
				 u32 flags, u32 payload_len, void *payload_buf,
				 u32 *out_result)
{
	struct nova_vrpc_header *hdr;
	__le32 *send_slot;
	u32 seq;
	u32 waited = 0;
	u32 recv_put;

	if (payload_len > NOVA_VRPC_MAX_PAYLOAD)
		return -EINVAL;

	mutex_lock(&gdev->rpc_lock);

	seq = gdev->next_seq++;
	memset(gdev->vrpc_page, 0, RING_SIZE_BYTES);
	hdr = (struct nova_vrpc_header *)gdev->vrpc_page;
	hdr->version = NOVA_VRPC_VERSION_3_0;
	hdr->signature = NOVA_VRPC_MAGIC;
	hdr->total_length = NOVA_VRPC_HEADER_BYTES + payload_len;
	hdr->function = func_id;
	hdr->result = 0xffffffffU;
	hdr->spare = 0xffffffffU;
	hdr->sequence = seq;
	hdr->flags = flags;

	if (payload_len && payload_buf)
		memcpy((u8 *)gdev->vrpc_page + NOVA_VRPC_HEADER_BYTES, payload_buf,
		       payload_len);

	wmb();

	/* Populate 8-byte descriptor inside SEND_RING[PUT] matching _nv013262rm */
	send_slot = (__le32 *)((u8 *)gdev->send_ring + (gdev->send_put & 0xff8U));
	send_slot[0] = cpu_to_le32(nova_ring_pte_lo(gdev->vrpc_page_dma));
	send_slot[1] = cpu_to_le32(nova_ring_pte_hi(gdev->vrpc_page_dma));
	wmb();

	gdev->send_put = (gdev->send_put + 8U) & 0xfffU;
	iowrite32(gdev->send_put, gdev->bar0 + NOVA_NV_VGPU_SEND_RING_PUT);
	iowrite32(1U, gdev->bar0 + NOVA_NV_VGPU_RPC_DOORBELL);

	while (waited < VRPC_POLL_TIMEOUT_US) {
		recv_put = ioread32(gdev->bar0 + NOVA_NV_VGPU_RECV_RING_PUT);
		if (recv_put != gdev->recv_get)
			break;
		udelay(50);
		waited += 50;
	}

	if (recv_put == gdev->recv_get) {
		mutex_unlock(&gdev->rpc_lock);
		return -ETIMEDOUT;
	}

	rmb();
	gdev->recv_get = (gdev->recv_get + 8U) & 0xfffU;
	iowrite32(gdev->recv_get, gdev->bar0 + NOVA_NV_VGPU_RECV_RING_GET);

	if (hdr->signature != NOVA_VRPC_MAGIC || hdr->sequence != seq) {
		mutex_unlock(&gdev->rpc_lock);
		return -EPROTO;
	}

	*out_result = hdr->result;
	if (payload_len && payload_buf)
		memcpy(payload_buf, (u8 *)gdev->vrpc_page + NOVA_VRPC_HEADER_BYTES,
		       payload_len);

	atomic64_inc(&gdev->rpc_completed);
	mutex_unlock(&gdev->rpc_lock);
	return 0;
}

static long nova_vgpu_guest_ioctl(struct file *filp, unsigned int cmd,
				  unsigned long arg)
{
	struct nova_vgpu_guest_dev *gdev =
		container_of(filp->private_data, struct nova_vgpu_guest_dev, miscdev);

	switch (cmd) {
	case NOVA_VGPU_GUEST_IOCTL_GET_INFO: {
		struct nova_vgpu_guest_info info = {
			.pmc_boot0 = gdev->pmc_boot0,
			.vgpu_config = gdev->vgpu_config,
			.cstatus_mb = gdev->cstatus_mb,
			.ecc_supported = (gdev->vgpu_config >> 18) & 1U,
			.virtual_fb_bytes = gdev->virtual_fb_bytes,
			.bar1_bytes = (u64)gdev->bar1_len,
			.send_ring_dma = (u64)gdev->send_ring_dma,
			.recv_ring_dma = (u64)gdev->recv_ring_dma,
			.rpc_completed = (u64)atomic64_read(&gdev->rpc_completed),
			.irq_count = (u64)atomic64_read(&gdev->irq_count),
		};

		if (copy_to_user((void __user *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	case NOVA_VGPU_GUEST_IOCTL_VRPC: {
		struct nova_vgpu_guest_vrpc_req req;
		void *kbuf = NULL;
		int ret;

		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;
		if (req.payload_len > NOVA_VRPC_MAX_PAYLOAD)
			return -EINVAL;

		if (req.payload_len > 0) {
			kbuf = kzalloc(req.payload_len, GFP_KERNEL);
			if (!kbuf)
				return -ENOMEM;
			if (copy_from_user(kbuf,
					   (void __user *)(uintptr_t)req.payload_user_ptr,
					   req.payload_len)) {
				kfree(kbuf);
				return -EFAULT;
			}
		}

		ret = nova_vgpu_submit_vrpc(gdev, req.function, req.flags,
					    req.payload_len, kbuf, &req.result);
		if (!ret && req.payload_len > 0 &&
		    copy_to_user((void __user *)(uintptr_t)req.payload_user_ptr,
				 kbuf, req.payload_len))
			ret = -EFAULT;

		kfree(kbuf);
		if (!ret && copy_to_user((void __user *)arg, &req, sizeof(req)))
			return -EFAULT;
		return ret;
	}
	case NOVA_VGPU_GUEST_IOCTL_READ_REG: {
		struct nova_vgpu_guest_reg_op op;

		if (copy_from_user(&op, (void __user *)arg, sizeof(op)))
			return -EFAULT;
		if ((op.offset & 3U) || op.offset >= gdev->bar0_len)
			return -EINVAL;
		op.value = ioread32(gdev->bar0 + op.offset);
		if (copy_to_user((void __user *)arg, &op, sizeof(op)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

static int nova_vgpu_guest_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct nova_vgpu_guest_dev *gdev =
		container_of(filp->private_data, struct nova_vgpu_guest_dev, miscdev);
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long pfn_start = (gdev->bar1_start >> PAGE_SHIFT) + vma->vm_pgoff;

	if (!gdev->bar1_len || (vma->vm_pgoff << PAGE_SHIFT) + size > gdev->bar1_len)
		return -EINVAL;

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	return io_remap_pfn_range(vma, vma->vm_start, pfn_start, size,
				  vma->vm_page_prot);
}

static const struct file_operations nova_vgpu_guest_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = nova_vgpu_guest_ioctl,
	.mmap = nova_vgpu_guest_mmap,
	.llseek = noop_llseek,
};

static int nova_vgpu_guest_probe(struct pci_dev *pdev,
				 const struct pci_device_id *id)
{
	struct nova_vgpu_guest_dev *gdev;
	u32 pmc_boot0, vgpu_cfg, fb_pages_m1;
	int err, minor_idx, nvec;

	err = pcim_enable_device(pdev);
	if (err)
		return err;

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(40));
	if (err)
		return err;

	pci_set_master(pdev);

	gdev = devm_kzalloc(&pdev->dev, sizeof(*gdev), GFP_KERNEL);
	if (!gdev)
		return -ENOMEM;

	gdev->pdev = pdev;
	gdev->next_seq = 1;
	mutex_init(&gdev->rpc_lock);

	gdev->bar0_len = pci_resource_len(pdev, 0);
	gdev->bar1_start = pci_resource_start(pdev, 1);
	gdev->bar1_len = pci_resource_len(pdev, 1);
	gdev->bar0 = pcim_iomap(pdev, 0, gdev->bar0_len);
	if (!gdev->bar0)
		return -ENOMEM;

	pmc_boot0 = ioread32(gdev->bar0 + NOVA_NV_PMC_BOOT_0);
	vgpu_cfg = ioread32(gdev->bar0 + NOVA_NV_VGPU_CONFIG);

	/*
	 * Safety guard: NV_VGPU_CONFIG bit 0 is set only when running inside a
	 * virtualized vGPU VM (emulated by host vmiop/nova-vgpu). On bare-metal
	 * silicon, return -ENODEV so we never disturb a physical GP104.
	 */
	if (!allow_baremetal_probe && (vgpu_cfg & 1U) == 0U) {
		dev_info(&pdev->dev,
			 "NV_VGPU_CONFIG=0x%08x indicates bare-metal GPU; refusing guest bind\n",
			 vgpu_cfg);
		return -ENODEV;
	}

	fb_pages_m1 = ioread32(gdev->bar0 + NOVA_NV_VGPU_FB_SIZE_4K_MINUS_1);
	gdev->pmc_boot0 = pmc_boot0;
	gdev->vgpu_config = vgpu_cfg;
	gdev->cstatus_mb = ioread32(gdev->bar0 + NOVA_NV_PFB_FBPA_0_CSTATUS);
	gdev->virtual_fb_bytes = ((u64)fb_pages_m1 + 1ULL) << 12;

	gdev->send_ring = dmam_alloc_coherent(&pdev->dev, RING_SIZE_BYTES,
					      &gdev->send_ring_dma, GFP_KERNEL);
	gdev->recv_ring = dmam_alloc_coherent(&pdev->dev, RING_SIZE_BYTES,
					      &gdev->recv_ring_dma, GFP_KERNEL);
	gdev->event_ring = dmam_alloc_coherent(&pdev->dev, RING_SIZE_BYTES,
					       &gdev->event_ring_dma, GFP_KERNEL);
	gdev->vrpc_page = dmam_alloc_coherent(&pdev->dev, RING_SIZE_BYTES,
					      &gdev->vrpc_page_dma, GFP_KERNEL);
	if (!gdev->send_ring || !gdev->recv_ring || !gdev->event_ring || !gdev->vrpc_page)
		return -ENOMEM;

	nvec = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_INTX);
	if (nvec < 0)
		return nvec;

	gdev->irq = pci_irq_vector(pdev, 0);
	err = devm_request_irq(&pdev->dev, gdev->irq, nova_vgpu_guest_irq,
			       IRQF_SHARED, DRV_NAME, gdev);
	if (err) {
		pci_free_irq_vectors(pdev);
		return err;
	}

	nova_vgpu_program_rings(gdev);

	minor_idx = atomic_inc_return(&g_guest_minor_seq) - 1;
	snprintf(gdev->devname, sizeof(gdev->devname), "nova-vgpu-guest%d", minor_idx);
	gdev->miscdev.minor = MISC_DYNAMIC_MINOR;
	gdev->miscdev.name = gdev->devname;
	gdev->miscdev.fops = &nova_vgpu_guest_fops;
	gdev->miscdev.parent = &pdev->dev;

	err = misc_register(&gdev->miscdev);
	if (err) {
		nova_vgpu_teardown_rings(gdev);
		pci_free_irq_vectors(pdev);
		return err;
	}

	pci_set_drvdata(pdev, gdev);
	dev_info(&pdev->dev,
		 "attached clean-room GP104 vGPU guest: BOOT0=0x%08x FB=%llu MiB (zero Falcon blobs loaded)\n",
		 gdev->pmc_boot0, gdev->virtual_fb_bytes >> 20);
	return 0;
}

static void nova_vgpu_guest_remove(struct pci_dev *pdev)
{
	struct nova_vgpu_guest_dev *gdev = pci_get_drvdata(pdev);

	if (!gdev)
		return;

	misc_deregister(&gdev->miscdev);
	nova_vgpu_teardown_rings(gdev);
	pci_free_irq_vectors(pdev);
}

static const struct pci_device_id nova_vgpu_guest_ids[] = {
	{ PCI_DEVICE(NOVA_VGPU_PCI_VENDOR_NVIDIA, NOVA_VGPU_PCI_DEVICE_TESLA_P4) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, nova_vgpu_guest_ids);

static struct pci_driver nova_vgpu_guest_driver = {
	.name = DRV_NAME,
	.id_table = nova_vgpu_guest_ids,
	.probe = nova_vgpu_guest_probe,
	.remove = nova_vgpu_guest_remove,
};

module_pci_driver(nova_vgpu_guest_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Clean-Room Pascal vGPU Project");
MODULE_DESCRIPTION("Open-source Linux guest vGPU BAR0/VRPC transport driver for Pascal GP104 (Tesla P4)");
