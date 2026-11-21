#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *nvmlDevice_t;
typedef void *nvmlEventSet_t;
typedef int nvmlReturn_t;

typedef struct nvmlPciInfo_st {
    char busIdLegacy[16];
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    unsigned int pciDeviceId;
    unsigned int pciSubSystemId;
    char busId[32];
} nvmlPciInfo_t;

typedef struct nvmlEventData_st {
    nvmlDevice_t device;
    unsigned long long eventType;
    unsigned long long eventData;
    unsigned int gpuInstanceId;
    unsigned int computeInstanceId;
} nvmlEventData_t;

static int g_mock_dev = 1;
static int g_mock_set = 2;
static int g_delivered_xid = 0;

static int is_gsp_mode(void) {
    const char *m = getenv("MOCK_NVML_MODE");
    return (m && strcmp(m, "gsp_xid") == 0);
}

nvmlReturn_t nvmlInit_v2(void) {
    g_delivered_xid = 0;
    return 0;
}

nvmlReturn_t nvmlShutdown(void) {
    return 0;
}

const char *nvmlErrorString(nvmlReturn_t r) {
    switch (r) {
    case 0: return "Success";
    case 3: return "Not Supported";
    case 10: return "Timeout";
    default: return "Mock Error";
    }
}

nvmlReturn_t nvmlDeviceGetCount_v2(unsigned int *count) {
    *count = 1;
    return 0;
}

nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(unsigned int idx, nvmlDevice_t *dev) {
    if (idx != 0) return 6;
    *dev = &g_mock_dev;
    return 0;
}

nvmlReturn_t nvmlDeviceGetGspFirmwareMode(nvmlDevice_t dev, unsigned int *en, unsigned int *def) {
    (void)dev;
    if (!is_gsp_mode()) {
        return 3; /* NVML_ERROR_NOT_SUPPORTED (Pascal GP104) */
    }
    *en = 1;
    *def = 1;
    return 0;
}

nvmlReturn_t nvmlDeviceGetPciInfo_v3(nvmlDevice_t dev, nvmlPciInfo_t *pci) {
    (void)dev;
    memset(pci, 0, sizeof(*pci));
    strcpy(pci->busId, "00000000:01:00.0");
    return 0;
}

nvmlReturn_t nvmlDeviceGetSupportedEventTypes(nvmlDevice_t dev, unsigned long long *types) {
    (void)dev;
    *types = 0x8ULL;
    return 0;
}

nvmlReturn_t nvmlDeviceRegisterEvents(nvmlDevice_t dev, unsigned long long types, nvmlEventSet_t set) {
    (void)dev;
    (void)types;
    (void)set;
    return 0;
}

nvmlReturn_t nvmlDeviceGetIndex(nvmlDevice_t dev, unsigned int *idx) {
    (void)dev;
    *idx = 0;
    return 0;
}

nvmlReturn_t nvmlEventSetCreate(nvmlEventSet_t *set) {
    *set = &g_mock_set;
    return 0;
}

nvmlReturn_t nvmlEventSetWait_v2(nvmlEventSet_t set, nvmlEventData_t *data, unsigned int timeout_ms) {
    (void)set;
    if (!g_delivered_xid) {
        g_delivered_xid = 1;
        data->device = &g_mock_dev;
        data->eventType = 0x8ULL;
        data->eventData = 79ULL;
        return 0;
    }
    usleep((timeout_ms > 50 ? 50 : timeout_ms) * 1000U);
    return 10; /* NVML_ERROR_TIMEOUT */
}

nvmlReturn_t nvmlEventSetFree(nvmlEventSet_t set) {
    (void)set;
    return 0;
}
