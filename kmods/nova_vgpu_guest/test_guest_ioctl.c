// SPDX-License-Identifier: GPL-2.0
/*
 * test_guest_ioctl.c - Guest userspace verification binary for /dev/nova-vgpu-guest0
 * inside a live QEMU/KVM VM backed by nova_vgpu_mdev.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "nova_vgpu_guest.h"

static void fail(const char *msg)
{
	perror(msg);
	exit(1);
}

static int issue_vrpc(int fd, uint32_t func, uint32_t flags, void *payload, uint32_t len,
		      uint32_t *out_res)
{
	struct nova_vgpu_guest_vrpc_req req = {
		.function = func,
		.flags = flags,
		.payload_len = len,
		.result = 0xffffffffU,
		.payload_user_ptr = (uint64_t)(uintptr_t)payload,
	};

	if (ioctl(fd, NOVA_VGPU_GUEST_IOCTL_VRPC, &req) < 0)
		return -1;
	*out_res = req.result;
	return 0;
}

int main(void)
{
	struct nova_vgpu_guest_info info;
	struct nova_vgpu_guest_reg_op rop;
	uint8_t payload[64];
	uint32_t res = 0;
	uint64_t fb_bytes = 0;
	uint16_t dev_id = 0, subsys_id = 0;
	volatile uint64_t *bar1;
	uint64_t readback;
	int fd;

	fd = open("/dev/nova-vgpu-guest0", O_RDWR);
	if (fd < 0)
		fail("open /dev/nova-vgpu-guest0");

	if (ioctl(fd, NOVA_VGPU_GUEST_IOCTL_GET_INFO, &info) < 0)
		fail("GET_INFO initial");

	printf("GET_INFO: pmc_boot0=0x%08x vgpu_config=0x%08x cstatus=%uMiB ecc=%u fb=0x%llx bar1=0x%llx\n",
	       info.pmc_boot0, info.vgpu_config, info.cstatus_mb, info.ecc_supported,
	       (unsigned long long)info.virtual_fb_bytes, (unsigned long long)info.bar1_bytes);

	if (info.pmc_boot0 != 0x134000a1U || (info.vgpu_config & 1U) == 0 ||
	    info.virtual_fb_bytes != 0x74000000ULL) {
		fprintf(stderr, "Unexpected GET_INFO geometry!\n");
		return 1;
	}

	uint32_t regs[] = {
		NOVA_NV_PMC_BOOT_0,
		NOVA_NV_VGPU_CONFIG,
		NOVA_NV_PFB_FBPA_0_CSTATUS,
		0x00500c30U,
	};
	for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		rop.offset = regs[i];
		rop.value = 0;
		if (ioctl(fd, NOVA_VGPU_GUEST_IOCTL_READ_REG, &rop) < 0)
			fail("READ_REG");
		printf("READ_REG[0x%06x] = 0x%08x\n", rop.offset, rop.value);
	}

	/* 1. GET_STATIC_INFO before SetGuestSystemInfo must return 0x56 */
	memset(payload, 0, sizeof(payload));
	if (issue_vrpc(fd, NOVA_VRPC_FUNC_GET_STATIC_INFO, 0, payload, 16, &res) < 0)
		fail("VRPC pre-init GET_STATIC_INFO");
	printf("VRPC pre-init GET_STATIC_INFO result=0x%x (expected 0x56)\n", res);
	if (res != 0x56U)
		return 1;

	/* 2. SET_GUEST_SYSTEM_INFO handshake */
	memset(payload, 0, sizeof(payload));
	uint32_t vgx_maj = 535, vgx_min = 309, tag = 0x41564f4eU;
	memcpy(payload + 0, &vgx_maj, 4);
	memcpy(payload + 4, &vgx_min, 4);
	memcpy(payload + 8, &tag, 4);
	if (issue_vrpc(fd, NOVA_VRPC_FUNC_SET_GUEST_INFO, 0, payload, 16, &res) < 0 || res != 0) {
		fprintf(stderr, "SET_GUEST_SYSTEM_INFO failed: res=0x%x errno=%d\n", res, errno);
		return 1;
	}
	printf("VRPC SET_GUEST_SYSTEM_INFO result=0x%x\n", res);

	/* 3. GET_STATIC_INFO after handshake */
	memset(payload, 0, sizeof(payload));
	if (issue_vrpc(fd, NOVA_VRPC_FUNC_GET_STATIC_INFO, 0, payload, 16, &res) < 0 || res != 0) {
		fprintf(stderr, "GET_STATIC_INFO failed: res=0x%x\n", res);
		return 1;
	}
	memcpy(&fb_bytes, payload + 0, 8);
	memcpy(&dev_id, payload + 8, 2);
	memcpy(&subsys_id, payload + 10, 2);
	printf("VRPC GET_STATIC_INFO result=0x%x fb=0x%llx dev=0x%04x subsys=0x%04x\n",
	       res, (unsigned long long)fb_bytes, dev_id, subsys_id);
	if (fb_bytes != 0x74000000ULL || dev_id != 0x1bb3U || subsys_id != 0x1205U)
		return 1;

	/* 4. AllocRoot, AllocDevice, AllocMemory, AllocChannelDma, RmControl */
	uint32_t funcs[] = {
		NOVA_VRPC_FUNC_ALLOC_ROOT,
		NOVA_VRPC_FUNC_ALLOC_DEVICE,
		NOVA_VRPC_FUNC_ALLOC_MEMORY,
		NOVA_VRPC_FUNC_ALLOC_CHANNEL_DMA,
		NOVA_VRPC_FUNC_RM_CONTROL,
	};
	for (size_t i = 0; i < sizeof(funcs) / sizeof(funcs[0]); i++) {
		memset(payload, 0, sizeof(payload));
		if (issue_vrpc(fd, funcs[i], 0, payload, 24, &res) < 0 || res != 0) {
			fprintf(stderr, "VRPC func=0x%x failed: res=0x%x\n", funcs[i], res);
			return 1;
		}
		printf("VRPC func=0x%02x result=0x%x\n", funcs[i], res);
	}

	/* 5. BAR1 write-combine mmap write + readback */
	bar1 = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (bar1 == MAP_FAILED)
		fail("mmap BAR1");
	bar1[0] = 0xdeadbeefcafebabeULL;
	__sync_synchronize();
	readback = bar1[0];
	printf("BAR1_MMAP_OK readback=0x%016llx\n", (unsigned long long)readback);
	if (readback != 0xdeadbeefcafebabeULL)
		return 1;
	munmap((void *)bar1, 4096);

	if (ioctl(fd, NOVA_VGPU_GUEST_IOCTL_GET_INFO, &info) < 0)
		fail("GET_INFO final");
	printf("FINAL_STATS: rpc_completed=%llu irq_count=%llu\n",
	       (unsigned long long)info.rpc_completed,
	       (unsigned long long)info.irq_count);

	close(fd);
	return 0;
}
