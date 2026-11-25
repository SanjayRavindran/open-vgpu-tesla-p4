/*
 * Unit & Mock-RM Differential Tests for libopen-nvidia-ml.so and open-nvidia-smi
 * Verifies exact GP104GL (Tesla P4) RM control structures and CLI formatting.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "open_nvml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

typedef struct {
    uint32_t hRoot;
    uint32_t hObjectParent;
    uint32_t hObjectNew;
    uint32_t hClass;
    void    *pAllocParms;
    uint32_t paramsSize;
    uint32_t status;
} mock_rm_alloc_t;

typedef struct {
    uint32_t hRoot;
    uint32_t hObjectParent;
    uint32_t hObjectOld;
    uint32_t status;
} mock_rm_free_t;

typedef struct {
    uint32_t hClient;
    uint32_t hObject;
    uint32_t cmd;
    uint32_t flags;
    void    *params;
    uint32_t paramsSize;
    uint32_t status;
} mock_rm_control_t;

typedef struct {
    uint32_t valid;
    uint32_t domain;
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  function;
    uint8_t  pad0;
    uint32_t vendor_device_id;
    uint32_t gpu_id;
    uint32_t interrupt_line;
    uint64_t reg_address;
    uint64_t reg_size;
    uint64_t fb_address;
    uint64_t fb_size;
    uint32_t minor_number;
    uint8_t  pad1[12];
} mock_card_info_t;

static int mock_rm_ioctl(int fd, unsigned long request, void *arg) {
    (void)fd;
    uint8_t nr = request & 0xffU;
    uint8_t type = (request >> 8) & 0xffU;
    if (type != 'F' || !arg) return -1;

    if (nr == 210U) { /* NV_ESC_CHECK_VERSION_STR */
        return 0;
    }
    if (nr == 200U) { /* NV_ESC_CARD_INFO */
        mock_card_info_t *cards = (mock_card_info_t *)arg;
        memset(cards, 0, sizeof(mock_card_info_t) * 32);
        cards[0].valid = 1U;
        cards[0].domain = 0U;
        cards[0].bus = 1U;
        cards[0].slot = 0U;
        cards[0].vendor_device_id = 0x1bb310deU;
        cards[0].gpu_id = 0x100U;
        cards[0].interrupt_line = 38U;
        cards[0].reg_address = 0xdf000000ULL;
        cards[0].reg_size = 0x01000000ULL;
        cards[0].fb_address = 0xfe0000000ULL;
        cards[0].fb_size = 0x10000000ULL;
        cards[0].minor_number = 0U;
        return 0;
    }
    if (nr == 0x2bU) { /* NV_ESC_RM_ALLOC */
        mock_rm_alloc_t *a = (mock_rm_alloc_t *)arg;
        if (a->hClass == 0x41U && a->hObjectNew == 0) {
            a->hObjectNew = 0xc1d001feU;
        }
        a->status = 0;
        return 0;
    }
    if (nr == 0x29U) { /* NV_ESC_RM_FREE */
        mock_rm_free_t *f = (mock_rm_free_t *)arg;
        f->status = 0;
        return 0;
    }
    if (nr == 0x2aU) { /* NV_ESC_RM_CONTROL */
        mock_rm_control_t *c = (mock_rm_control_t *)arg;
        c->status = 0;
        uint8_t *p = (uint8_t *)c->params;
        uint32_t *u32 = (uint32_t *)c->params;
        switch (c->cmd) {
            case 0x00000215U: /* ATTACH_IDS */
                return 0;
            case 0x20801801U: /* BUS_GET_PCI_INFO */
                u32[0] = 0x1bb310deU;
                u32[1] = 0x11d810deU;
                u32[2] = 0xa1U;
                u32[3] = 0x1bb3U;
                return 0;
            case 0x20800110U: /* GPU_GET_NAME_STRING */
                snprintf((char *)&p[4], 128, "Tesla P4");
                return 0;
            case 0x2080014aU: /* GPU_GET_GID_INFO */
                u32[2] = 41U;
                snprintf((char *)&p[12], 256, "GPU-cef98b3e-4422-7b2b-6e5f-e6e3ba03f603");
                return 0;
            case 0x2080013fU: /* SHORT_NAME_STRING / SERIAL / PART */
                snprintf((char *)&p[0x1c], 16, "0322418175550");
                snprintf((char *)&p[0x47], 24, "900-2G414-0000-001");
                return 0;
            case 0x20801303U: /* FB_GET_INFO_V2 */
                assert(c->paramsSize == 1028U);
                if (u32[0] == 3U) {
                    u32[2] = 0x7f7cd4U; /* Free KiB: 8355028 */
                    u32[4] = 0x800000U; /* Total KiB: 8388608 (8192 MiB) */
                    u32[6] = 0x7fff80U; /* Usable KiB: 8388480 */
                } else if (u32[0] == 2U) {
                    u32[2] = 262144U;   /* BAR1 Total KiB: 256 MiB */
                    u32[4] = 259904U;   /* BAR1 Free KiB: 254 MiB */
                }
                return 0;
            case 0x20800513U: /* THERMAL_SYSTEM_EXECUTE */
                u32[10] = 36U;
                return 0;
            case 0x20808546U: /* THERMAL_GET_THRESHOLD */
                u32[1] = (u32[0] == 0x16U) ? (94U << 8) : (91U << 8);
                return 0;
            case 0x2080a613U: /* PWR_MONITOR_GET_STATUS */
                assert(c->paramsSize == 16916U);
                u32[2] = 10150U; /* 10.15 W */
                return 0;
            case 0x2080a61dU: /* PWR_POLICY_GET_CONTROL */
            case 0x2080a61fU: /* PWR_POLICY_IDX_GET_LIMIT */
                u32[1] = 75000U; /* 75.00 W */
                return 0;
            case 0x2080a618U: /* PWR_POLICY_GET_INFO */
                assert(c->paramsSize == 8620U);
                u32[0x228 / 4] = 60000U;
                u32[0x22c / 4] = 75000U;
                u32[0x230 / 4] = 75000U;
                return 0;
            case 0x20802068U: /* PERF_GET_CURRENT_PSTATE */
                u32[0] = 0x100U; /* P8 */
                return 0;
            case 0x20809004U: /* CLK_GET_INFO */
                if (u32[3] == 0x1000U) u32[4] = 911000U;      /* 455 MHz (2x domain) */
                else if (u32[3] == 0x0010U) u32[4] = 405000U; /* 405 MHz */
                else if (u32[3] == 0x8000U) u32[4] = 911000U; /* 455 MHz */
                return 0;
            case 0x2080a097U: /* PERF_GET_CLK_DOMAINS_SAMPLE */
                u32[2] = 7U; /* GPU util 7% */
                u32[6] = 3U; /* Mem util 3% */
                return 0;
            case 0x20802087U: /* PERF_GET_VID_ENG_UTIL */
                u32[1] = (u32[0] == 1U) ? 12U : 5U;
                u32[2] = 167000U;
                return 0;
            case 0x00800288U: /* GET_PERSISTENCE_STATE */
                u32[0] = 1U;
                return 0;
            case 0x00800289U: /* GET_VIRTUALIZATION_MODE */
                u32[0] = 3U; /* HOST_VGPU */
                return 0;
            case 0x2080018dU: /* GET_ACTIVE_PARTITION_IDS */
                u32[2] = 2U;
                u32[3] = 88904U;
                u32[4] = 88970U;
                return 0;
            case 0x2080018eU: /* GET_PID_INFO */
                for (uint32_t i = 0; i < 2; i++) {
                    uint8_t *e = &p[8 + i * 0x48];
                    *(uint32_t *)&e[0] = (i == 0) ? 88904U : 88970U;
                    *(uint32_t *)&e[8] = 0U;
                    *(uint64_t *)&e[0x10] = 4000ULL * 1024ULL * 1024ULL;
                    *(uint64_t *)&e[0x18] = 0ULL;
                }
                return 0;
            case 0xa0810104U: /* GET_SUPPORTED_VGPU_TYPES */
                u32[2] = 2U;
                u32[3] = 0x3fU; /* GRID P4-1Q */
                u32[4] = 0x41U; /* GRID P4-4Q */
                return 0;
            case 0xa0810105U: /* GET_CREATABLE_VGPU_TYPES */
                u32[2] = 1U;
                u32[3] = 0x41U;
                return 0;
            case 0xa0810103U: /* GET_VGPU_TYPE_INFO */
                if (u32[0] == 0x41U) {
                    snprintf((char *)&p[0x0c], 64, "GRID P4-4Q");
                    snprintf((char *)&p[0x4c], 64, "Quadro");
                    *(uint32_t *)&p[0x18c] = 2U;
                    *(uint32_t *)&p[0x190] = 4U;
                    *(uint32_t *)&p[0x194] = 7680U;
                    *(uint32_t *)&p[0x198] = 4320U;
                    *(uint32_t *)&p[0x1a0] = 60U;
                    *(uint64_t *)&p[0x1c8] = 4294967296ULL;
                } else {
                    snprintf((char *)&p[0x0c], 64, "GRID P4-1Q");
                    snprintf((char *)&p[0x4c], 64, "Quadro");
                    *(uint32_t *)&p[0x18c] = 8U;
                    *(uint32_t *)&p[0x190] = 4U;
                    *(uint32_t *)&p[0x194] = 5120U;
                    *(uint32_t *)&p[0x198] = 2880U;
                    *(uint32_t *)&p[0x1a0] = 60U;
                    *(uint64_t *)&p[0x1c8] = 1073741824ULL;
                }
                return 0;
            case 0xa0810102U: /* GET_VGPU_INSTANCES */
                u32[1] = 2U;
                for (uint32_t i = 0; i < 2; i++) {
                    uint8_t *e = &p[8 + i * 0x150];
                    *(uint32_t *)&e[0x00] = (i == 0) ? 88904U : 88970U;
                    *(uint32_t *)&e[0x04] = (i == 0) ? 88890U : 88950U;
                    *(uint32_t *)&e[0x08] = 1U;
                    *(uint32_t *)&e[0x0c] = 60U;
                    *(uint32_t *)&e[0x10] = 0U;
                    *(uint32_t *)&e[0x14] = 100U;
                    e[0x18] = 1U;
                    snprintf((char *)&e[0x38], 16, (i == 0) ? "vgpu-uuid-1" : "vgpu-uuid-2");
                    *(uint32_t *)&e[0x48] = 1U;
                    snprintf((char *)&e[0x78], 128, (i == 0) ? "vm-alma" : "vm-win11");
                    *(uint32_t *)&e[0x100] = 0x41U;
                    snprintf((char *)&e[0x110], 16, (i == 0) ? "580.196.07" : "582.99");
                    snprintf((char *)&e[0x120], 16, (i == 0) ? "mdev-uuid-1" : "mdev-uuid-2");
                    *(uint64_t *)&e[0x138] = 4000ULL * 1024ULL * 1024ULL;
                }
                return 0;
            default:
                return 0;
        }
    }
    return -1;
}

static void test_nvml_core_telemetry(void) {
    open_nvml_set_test_hooks(mock_rm_ioctl, "580.196.09");
    assert(nvmlInit_v2() == NVML_SUCCESS);

    char drv[80] = {0}, nvmlv[80] = {0};
    assert(nvmlSystemGetDriverVersion(drv, sizeof(drv)) == NVML_SUCCESS);
    assert(strcmp(drv, "580.196.09") == 0);
    assert(nvmlSystemGetNVMLVersion(nvmlv, sizeof(nvmlv)) == NVML_SUCCESS);
    assert(strcmp(nvmlv, "12.580.196.09") == 0);

    int cuda_ver = 0;
    assert(nvmlSystemGetCudaDriverVersion(&cuda_ver) == NVML_SUCCESS && cuda_ver == 13000);

    unsigned int cnt = 0;
    assert(nvmlDeviceGetCount_v2(&cnt) == NVML_SUCCESS);
    assert(cnt == 1U);

    nvmlDevice_t dev = NULL;
    assert(nvmlDeviceGetHandleByIndex_v2(0, &dev) == NVML_SUCCESS);
    assert(dev != NULL);

    char name[64] = {0}, uuid[96] = {0}, serial[64] = {0}, part[64] = {0}, vbios[32] = {0};
    assert(nvmlDeviceGetName(dev, name, sizeof(name)) == NVML_SUCCESS);
    assert(strcmp(name, "Tesla P4") == 0);
    assert(nvmlDeviceGetUUID(dev, uuid, sizeof(uuid)) == NVML_SUCCESS);
    assert(strcmp(uuid, "GPU-cef98b3e-4422-7b2b-6e5f-e6e3ba03f603") == 0);
    assert(nvmlDeviceGetSerial(dev, serial, sizeof(serial)) == NVML_SUCCESS);
    assert(strcmp(serial, "0322418175550") == 0);
    assert(nvmlDeviceGetBoardPartNumber(dev, part, sizeof(part)) == NVML_SUCCESS);
    assert(strcmp(part, "900-2G414-0000-001") == 0);
    assert(nvmlDeviceGetVbiosVersion(dev, vbios, sizeof(vbios)) == NVML_SUCCESS);
    assert(strcmp(vbios, "86.04.8C.00.10") == 0);

    nvmlPciInfo_t pci = {0};
    assert(nvmlDeviceGetPciInfo_v3(dev, &pci) == NVML_SUCCESS);
    assert(strcmp(pci.busId, "00000000:01:00.0") == 0);
    assert(pci.pciDeviceId == 0x1bb310deU);
    assert(pci.pciSubSystemId == 0x11d810deU);

    nvmlMemory_t mem = {0};
    assert(nvmlDeviceGetMemoryInfo(dev, &mem) == NVML_SUCCESS);
    assert(mem.total == 8589934592ULL);
    assert(mem.free  == 8555548672ULL);
    assert(mem.used  == 34385920ULL);

    nvmlMemory_v2_t mem2 = { .version = nvmlMemory_v2 };
    assert(nvmlDeviceGetMemoryInfo_v2(dev, &mem2) == NVML_SUCCESS);
    assert(mem2.total    == 8589934592ULL);
    assert(mem2.reserved == 131072ULL);
    assert(mem2.free     == 8555548672ULL);
    assert(mem2.used     == 34254848ULL);

    nvmlBAR1Memory_t bar1 = {0};
    assert(nvmlDeviceGetBAR1MemoryInfo(dev, &bar1) == NVML_SUCCESS);
    assert(bar1.bar1Total == 268435456ULL);

    unsigned int temp = 0, shut = 0, slow = 0, pwr = 0, plim = 0, minl = 0, maxl = 0;
    assert(nvmlDeviceGetTemperature(dev, NVML_TEMPERATURE_GPU, &temp) == NVML_SUCCESS && temp == 36U);
    assert(nvmlDeviceGetTemperatureThreshold(dev, NVML_TEMPERATURE_THRESHOLD_SHUTDOWN, &shut) == NVML_SUCCESS && shut == 94U);
    assert(nvmlDeviceGetTemperatureThreshold(dev, NVML_TEMPERATURE_THRESHOLD_SLOWDOWN, &slow) == NVML_SUCCESS && slow == 91U);
    assert(nvmlDeviceGetPowerUsage(dev, &pwr) == NVML_SUCCESS && pwr == 10150U);
    assert(nvmlDeviceGetPowerManagementLimit(dev, &plim) == NVML_SUCCESS && plim == 75000U);
    assert(nvmlDeviceGetPowerManagementLimitConstraints(dev, &minl, &maxl) == NVML_SUCCESS && minl == 60000U && maxl == 75000U);

    nvmlPstates_t pstate = NVML_PSTATE_UNKNOWN;
    assert(nvmlDeviceGetPerformanceState(dev, &pstate) == NVML_SUCCESS && pstate == NVML_PSTATE_8);

    unsigned int c_gr = 0, c_mem = 0, c_vid = 0;
    assert(nvmlDeviceGetClockInfo(dev, NVML_CLOCK_GRAPHICS, &c_gr) == NVML_SUCCESS && c_gr == 455U);
    assert(nvmlDeviceGetClockInfo(dev, NVML_CLOCK_MEM, &c_mem) == NVML_SUCCESS && c_mem == 405U);
    assert(nvmlDeviceGetClockInfo(dev, NVML_CLOCK_VIDEO, &c_vid) == NVML_SUCCESS && c_vid == 455U);

    nvmlUtilization_t util = {0};
    assert(nvmlDeviceGetUtilizationRates(dev, &util) == NVML_SUCCESS && util.gpu == 7U && util.memory == 3U);

    assert(nvmlShutdown() == NVML_SUCCESS);
    printf("  [PASS] test_nvml_core_telemetry\n");
}

static void test_nvml_vgpu_and_processes(void) {
    open_nvml_set_test_hooks(mock_rm_ioctl, "580.196.09");
    assert(nvmlInit_v2() == NVML_SUCCESS);
    nvmlDevice_t dev = NULL;
    assert(nvmlDeviceGetHandleByIndex_v2(0, &dev) == NVML_SUCCESS);

    nvmlVgpuTypeId_t types[16];
    unsigned int tcnt = 16;
    assert(nvmlDeviceGetSupportedVgpus(dev, &tcnt, types) == NVML_SUCCESS && tcnt == 2U);

    char tname[64] = {0}, tcls[64] = {0};
    unsigned int nlen = sizeof(tname), clen = sizeof(tcls), max_inst = 0;
    unsigned long long fb = 0;
    assert(nvmlVgpuTypeGetName(0x41U, tname, &nlen) == NVML_SUCCESS && strcmp(tname, "GRID P4-4Q") == 0);
    assert(nvmlVgpuTypeGetClass(0x41U, tcls, &clen) == NVML_SUCCESS && strcmp(tcls, "Quadro") == 0);
    assert(nvmlVgpuTypeGetMaxInstances(dev, 0x41U, &max_inst) == NVML_SUCCESS && max_inst == 2U);
    assert(nvmlVgpuTypeGetFramebufferSize(0x41U, &fb) == NVML_SUCCESS && fb == 4294967296ULL);

    nvmlVgpuInstance_t insts[16];
    unsigned int icnt = 16;
    assert(nvmlDeviceGetActiveVgpus(dev, &icnt, insts) == NVML_SUCCESS && icnt == 2U);
    assert(insts[0] == 88904U && insts[1] == 88970U);

    char vmid[64] = {0}, gdrv[64] = {0};
    nvmlVgpuVmIdType_t idt = NVML_VGPU_VM_ID_DOMAIN_ID;
    unsigned int lic = 0;
    unsigned long long fbu = 0;
    nvmlVgpuTypeId_t vtid = 0;
    assert(nvmlVgpuInstanceGetType(88904U, &vtid) == NVML_SUCCESS && vtid == 0x41U);
    assert(nvmlVgpuInstanceGetVmID(88904U, vmid, sizeof(vmid), &idt) == NVML_SUCCESS && strcmp(vmid, "vm-alma") == 0);
    assert(nvmlVgpuInstanceGetVmDriverVersion(88904U, gdrv, sizeof(gdrv)) == NVML_SUCCESS && strcmp(gdrv, "580.196.07") == 0);
    assert(nvmlVgpuInstanceGetVmDriverVersion(88970U, gdrv, sizeof(gdrv)) == NVML_SUCCESS && strcmp(gdrv, "582.99") == 0);
    assert(nvmlVgpuInstanceGetLicenseStatus(88904U, &lic) == NVML_SUCCESS && lic == 1U);
    assert(nvmlVgpuInstanceGetFbUsage(88904U, &fbu) == NVML_SUCCESS && fbu == 4000ULL * 1024ULL * 1024ULL);

    nvmlProcessInfo_v3_t procs[16];
    unsigned int pcnt = 16;
    assert(nvmlDeviceGetComputeRunningProcesses_v3(dev, &pcnt, procs) == NVML_SUCCESS && pcnt == 2U);
    assert(procs[0].pid == 88904U && procs[1].pid == 88970U);

    assert(nvmlShutdown() == NVML_SUCCESS);
    printf("  [PASS] test_nvml_vgpu_and_processes\n");
}

int main(void) {
    printf("Running open-nvml-smi unit tests...\n");
    test_nvml_core_telemetry();
    test_nvml_vgpu_and_processes();
    printf("ALL open-nvml-smi UNIT TESTS PASSED!\n");
    return 0;
}
