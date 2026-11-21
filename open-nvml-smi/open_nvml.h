/*
 * Clean-room open-source NVML header (open_nvml.h)
 * Verified against NVIDIA 535.309.01-vgpu-kvm RM ioctls & REA 6.10.1 analysis.
 */
#ifndef OPEN_NVML_H
#define OPEN_NVML_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVML_API_VERSION                12
#define NVML_API_VERSION_STR            "12.535.309.01"
#define NVML_DEFAULT_DRIVER_VERSION_STR "535.309.01"

#define NVML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE 80
#define NVML_SYSTEM_NVML_VERSION_BUFFER_SIZE   80
#define NVML_DEVICE_NAME_BUFFER_SIZE           64
#define NVML_DEVICE_NAME_V2_BUFFER_SIZE        96
#define NVML_DEVICE_UUID_BUFFER_SIZE           80
#define NVML_DEVICE_UUID_V2_BUFFER_SIZE        96
#define NVML_DEVICE_SERIAL_BUFFER_SIZE         30
#define NVML_DEVICE_VBIOS_VERSION_BUFFER_SIZE  32
#define NVML_DEVICE_PART_NUMBER_BUFFER_SIZE    80
#define NVML_DEVICE_INFOROM_VERSION_BUFFER_SIZE 16
#define NVML_DEVICE_PCI_BUS_ID_BUFFER_SIZE     32
#define NVML_DEVICE_PCI_BUS_ID_BUFFER_V2_SIZE  16
#define NVML_GRID_LICENSE_BUFFER_SIZE          128

#define NVML_INIT_FLAG_NO_GPUS   1U
#define NVML_INIT_FLAG_NO_ATTACH 2U

#define NVML_VALUE_NOT_AVAILABLE (-1ULL)

typedef enum {
    NVML_SUCCESS                         = 0,
    NVML_ERROR_UNINITIALIZED             = 1,
    NVML_ERROR_INVALID_ARGUMENT          = 2,
    NVML_ERROR_NOT_SUPPORTED             = 3,
    NVML_ERROR_NO_PERMISSION             = 4,
    NVML_ERROR_ALREADY_INITIALIZED       = 5,
    NVML_ERROR_NOT_FOUND                 = 6,
    NVML_ERROR_INSUFFICIENT_SIZE         = 7,
    NVML_ERROR_INSUFFICIENT_POWER        = 8,
    NVML_ERROR_DRIVER_NOT_LOADED         = 9,
    NVML_ERROR_TIMEOUT                   = 10,
    NVML_ERROR_IRQ_ISSUE                 = 11,
    NVML_ERROR_LIBRARY_NOT_FOUND         = 12,
    NVML_ERROR_FUNCTION_NOT_FOUND        = 13,
    NVML_ERROR_CORRUPTED_INFOROM         = 14,
    NVML_ERROR_GPU_IS_LOST               = 15,
    NVML_ERROR_RESET_REQUIRED            = 16,
    NVML_ERROR_OPERATING_SYSTEM          = 17,
    NVML_ERROR_LIB_RM_VERSION_MISMATCH   = 18,
    NVML_ERROR_IN_USE                    = 19,
    NVML_ERROR_MEMORY                    = 20,
    NVML_ERROR_NO_DATA                   = 21,
    NVML_ERROR_VGPU_ECC_NOT_SUPPORTED    = 22,
    NVML_ERROR_INSUFFICIENT_RESOURCES    = 23,
    NVML_ERROR_FREQ_NOT_SUPPORTED        = 24,
    NVML_ERROR_ARGUMENT_VERSION_MISMATCH = 25,
    NVML_ERROR_DEPRECATED                = 26,
    NVML_ERROR_NOT_READY                 = 27,
    NVML_ERROR_GPU_NOT_FOUND             = 28,
    NVML_ERROR_INVALID_STATE             = 29,
    NVML_ERROR_UNKNOWN                   = 999
} nvmlReturn_t;

typedef enum {
    NVML_FEATURE_DISABLED = 0,
    NVML_FEATURE_ENABLED  = 1
} nvmlEnableState_t;

typedef enum {
    NVML_COMPUTEMODE_DEFAULT          = 0,
    NVML_COMPUTEMODE_EXCLUSIVE_THREAD = 1,
    NVML_COMPUTEMODE_PROHIBITED       = 2,
    NVML_COMPUTEMODE_EXCLUSIVE_PROCESS = 3
} nvmlComputeMode_t;

typedef enum {
    NVML_CLOCK_GRAPHICS = 0,
    NVML_CLOCK_SM       = 1,
    NVML_CLOCK_MEM      = 2,
    NVML_CLOCK_VIDEO    = 3,
    NVML_CLOCK_COUNT    = 4
} nvmlClockType_t;

typedef enum {
    NVML_CLOCK_ID_CURRENT            = 0,
    NVML_CLOCK_ID_APP_CLOCK_TARGET   = 1,
    NVML_CLOCK_ID_APP_CLOCK_DEFAULT  = 2,
    NVML_CLOCK_ID_CUSTOMER_BOOST_MAX = 3
} nvmlClockId_t;

typedef enum {
    NVML_TEMPERATURE_GPU   = 0,
    NVML_TEMPERATURE_COUNT = 1
} nvmlTemperatureSensors_t;

typedef enum {
    NVML_TEMPERATURE_THRESHOLD_SHUTDOWN      = 0,
    NVML_TEMPERATURE_THRESHOLD_SLOWDOWN      = 1,
    NVML_TEMPERATURE_THRESHOLD_MEM_MAX       = 2,
    NVML_TEMPERATURE_THRESHOLD_GPU_MAX       = 3,
    NVML_TEMPERATURE_THRESHOLD_ACOUSTIC_MIN  = 4,
    NVML_TEMPERATURE_THRESHOLD_ACOUSTIC_CURR = 5,
    NVML_TEMPERATURE_THRESHOLD_ACOUSTIC_MAX  = 6
} nvmlTemperatureThresholds_t;

typedef enum {
    NVML_PSTATE_0       = 0,
    NVML_PSTATE_1       = 1,
    NVML_PSTATE_2       = 2,
    NVML_PSTATE_3       = 3,
    NVML_PSTATE_4       = 4,
    NVML_PSTATE_5       = 5,
    NVML_PSTATE_6       = 6,
    NVML_PSTATE_7       = 7,
    NVML_PSTATE_8       = 8,
    NVML_PSTATE_9       = 9,
    NVML_PSTATE_10      = 10,
    NVML_PSTATE_11      = 11,
    NVML_PSTATE_12      = 12,
    NVML_PSTATE_13      = 13,
    NVML_PSTATE_14      = 14,
    NVML_PSTATE_15      = 15,
    NVML_PSTATE_UNKNOWN = 32
} nvmlPstates_t;

typedef enum {
    NVML_BRAND_UNKNOWN = 0,
    NVML_BRAND_QUADRO  = 1,
    NVML_BRAND_TESLA   = 2,
    NVML_BRAND_NVS     = 3,
    NVML_BRAND_GRID    = 4,
    NVML_BRAND_GEFORCE = 5,
    NVML_BRAND_TITAN   = 6
} nvmlBrandType_t;

typedef uint32_t nvmlDeviceArchitecture_t;
#define NVML_DEVICE_ARCH_KEPLER  2U
#define NVML_DEVICE_ARCH_MAXWELL 3U
#define NVML_DEVICE_ARCH_PASCAL  4U
#define NVML_DEVICE_ARCH_VOLTA   5U
#define NVML_DEVICE_ARCH_TURING  6U
#define NVML_DEVICE_ARCH_AMPERE  7U
#define NVML_DEVICE_ARCH_ADA     8U
#define NVML_DEVICE_ARCH_HOPPER  9U

typedef enum {
    NVML_HOST_VGPU_MODE_NON_SRIOV = 0,
    NVML_HOST_VGPU_MODE_SRIOV     = 1
} nvmlHostVgpuMode_t;

typedef enum {
    NVML_GPU_VIRTUALIZATION_MODE_NONE        = 0,
    NVML_GPU_VIRTUALIZATION_MODE_PASSTHROUGH = 1,
    NVML_GPU_VIRTUALIZATION_MODE_VGPU        = 2,
    NVML_GPU_VIRTUALIZATION_MODE_HOST_VGPU   = 3,
    NVML_GPU_VIRTUALIZATION_MODE_HOST_VSGA   = 4
} nvmlGpuVirtualizationMode_t;

typedef enum {
    NVML_VGPU_VM_ID_DOMAIN_ID = 0,
    NVML_VGPU_VM_ID_UUID      = 1
} nvmlVgpuVmIdType_t;

typedef struct nvmlDevice_st*   nvmlDevice_t;
typedef struct nvmlEventSet_st* nvmlEventSet_t;
typedef unsigned int            nvmlVgpuTypeId_t;
typedef unsigned int            nvmlVgpuInstance_t;

typedef struct {
    char         busIdLegacy[NVML_DEVICE_PCI_BUS_ID_BUFFER_V2_SIZE];
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    unsigned int pciDeviceId;
    unsigned int pciSubSystemId;
    char         busId[NVML_DEVICE_PCI_BUS_ID_BUFFER_SIZE];
} nvmlPciInfo_t;

typedef struct {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
} nvmlMemory_t;

typedef struct {
    unsigned int       version;
    unsigned long long total;
    unsigned long long reserved;
    unsigned long long free;
    unsigned long long used;
} nvmlMemory_v2_t;

#define nvmlMemory_v2 0x02000028U

typedef struct {
    unsigned long long bar1Total;
    unsigned long long bar1Free;
    unsigned long long bar1Used;
} nvmlBAR1Memory_t;

typedef struct {
    unsigned int gpu;
    unsigned int memory;
} nvmlUtilization_t;

typedef struct {
    unsigned int       pid;
    unsigned long long usedGpuMemory;
} nvmlProcessInfo_v1_t;

typedef struct {
    unsigned int       pid;
    unsigned long long usedGpuMemory;
    unsigned int       gpuInstanceId;
    unsigned int       computeInstanceId;
} nvmlProcessInfo_t;

typedef nvmlProcessInfo_t nvmlProcessInfo_v2_t;

typedef struct {
    unsigned int       pid;
    unsigned long long usedGpuMemory;
    unsigned int       gpuInstanceId;
    unsigned int       computeInstanceId;
    unsigned long long usedGpuCcProtectedMemory;
} nvmlProcessInfo_v3_t;

typedef struct {
    nvmlDevice_t       device;
    unsigned long long eventType;
    unsigned long long eventData;
    unsigned int       gpuInstanceId;
    unsigned int       computeInstanceId;
} nvmlEventData_t;

typedef struct {
    uint32_t     year;
    uint16_t     month;
    uint16_t     day;
    uint16_t     hour;
    uint16_t     min;
    uint16_t     sec;
    uint8_t      status;
} nvmlVgpuLicenseExpiry_t;

typedef struct {
    uint8_t                 isLicensed;
    nvmlVgpuLicenseExpiry_t licenseExpiry;
    uint32_t                currentState;
} nvmlVgpuLicenseInfo_t;

/* Core NVML API declarations */
nvmlReturn_t nvmlInit(void);
nvmlReturn_t nvmlInit_v2(void);
nvmlReturn_t nvmlInitWithFlags(unsigned int flags);
nvmlReturn_t nvmlShutdown(void);
const char*  nvmlErrorString(nvmlReturn_t result);

nvmlReturn_t nvmlSystemGetDriverVersion(char *version, unsigned int length);
nvmlReturn_t nvmlSystemGetNVMLVersion(char *version, unsigned int length);
nvmlReturn_t nvmlSystemGetCudaDriverVersion(int *cudaDriverVersion);
nvmlReturn_t nvmlSystemGetCudaDriverVersion_v2(int *cudaDriverVersion);
nvmlReturn_t nvmlSystemGetProcessName(unsigned int pid, char *name, unsigned int length);

nvmlReturn_t nvmlDeviceGetCount(unsigned int *deviceCount);
nvmlReturn_t nvmlDeviceGetCount_v2(unsigned int *deviceCount);
nvmlReturn_t nvmlDeviceGetHandleByIndex(unsigned int index, nvmlDevice_t *device);
nvmlReturn_t nvmlDeviceGetHandleByIndex_v2(unsigned int index, nvmlDevice_t *device);
nvmlReturn_t nvmlDeviceGetHandleByPciBusId(const char *pciBusId, nvmlDevice_t *device);
nvmlReturn_t nvmlDeviceGetHandleByPciBusId_v2(const char *pciBusId, nvmlDevice_t *device);
nvmlReturn_t nvmlDeviceGetHandleByUUID(const char *uuid, nvmlDevice_t *device);
nvmlReturn_t nvmlDeviceGetHandleBySerial(const char *serial, nvmlDevice_t *device);
nvmlReturn_t nvmlDeviceGetIndex(nvmlDevice_t device, unsigned int *index);
nvmlReturn_t nvmlDeviceGetMinorNumber(nvmlDevice_t device, unsigned int *minorNumber);

nvmlReturn_t nvmlDeviceGetName(nvmlDevice_t device, char *name, unsigned int length);
nvmlReturn_t nvmlDeviceGetUUID(nvmlDevice_t device, char *uuid, unsigned int length);
nvmlReturn_t nvmlDeviceGetSerial(nvmlDevice_t device, char *serial, unsigned int length);
nvmlReturn_t nvmlDeviceGetBoardPartNumber(nvmlDevice_t device, char *partNumber, unsigned int length);
nvmlReturn_t nvmlDeviceGetVbiosVersion(nvmlDevice_t device, char *version, unsigned int length);
nvmlReturn_t nvmlDeviceGetBrand(nvmlDevice_t device, nvmlBrandType_t *type);
nvmlReturn_t nvmlDeviceGetArchitecture(nvmlDevice_t device, nvmlDeviceArchitecture_t *arch);
nvmlReturn_t nvmlDeviceGetPciInfo(nvmlDevice_t device, nvmlPciInfo_t *pci);
nvmlReturn_t nvmlDeviceGetPciInfo_v2(nvmlDevice_t device, nvmlPciInfo_t *pci);
nvmlReturn_t nvmlDeviceGetPciInfo_v3(nvmlDevice_t device, nvmlPciInfo_t *pci);

nvmlReturn_t nvmlDeviceGetMemoryInfo(nvmlDevice_t device, nvmlMemory_t *memory);
nvmlReturn_t nvmlDeviceGetMemoryInfo_v2(nvmlDevice_t device, nvmlMemory_v2_t *memory);
nvmlReturn_t nvmlDeviceGetBAR1MemoryInfo(nvmlDevice_t device, nvmlBAR1Memory_t *bar1Memory);

nvmlReturn_t nvmlDeviceGetTemperature(nvmlDevice_t device, nvmlTemperatureSensors_t sensorType, unsigned int *temp);
nvmlReturn_t nvmlDeviceGetTemperatureThreshold(nvmlDevice_t device, nvmlTemperatureThresholds_t thresholdType, unsigned int *temp);
nvmlReturn_t nvmlDeviceGetFanSpeed(nvmlDevice_t device, unsigned int *speed);
nvmlReturn_t nvmlDeviceGetPowerUsage(nvmlDevice_t device, unsigned int *power);
nvmlReturn_t nvmlDeviceGetPowerManagementLimit(nvmlDevice_t device, unsigned int *limit);
nvmlReturn_t nvmlDeviceGetEnforcedPowerLimit(nvmlDevice_t device, unsigned int *limit);
nvmlReturn_t nvmlDeviceGetPowerManagementDefaultLimit(nvmlDevice_t device, unsigned int *defaultLimit);
nvmlReturn_t nvmlDeviceGetPowerManagementLimitConstraints(nvmlDevice_t device, unsigned int *minLimit, unsigned int *maxLimit);
nvmlReturn_t nvmlDeviceGetPerformanceState(nvmlDevice_t device, nvmlPstates_t *pState);
nvmlReturn_t nvmlDeviceGetClockInfo(nvmlDevice_t device, nvmlClockType_t type, unsigned int *clock);
nvmlReturn_t nvmlDeviceGetMaxClockInfo(nvmlDevice_t device, nvmlClockType_t type, unsigned int *clock);
nvmlReturn_t nvmlDeviceGetClock(nvmlDevice_t device, nvmlClockType_t clockType, nvmlClockId_t clockId, unsigned int *clockMHz);
nvmlReturn_t nvmlDeviceGetApplicationsClock(nvmlDevice_t device, nvmlClockType_t clockType, unsigned int *clockMHz);
nvmlReturn_t nvmlDeviceGetDefaultApplicationsClock(nvmlDevice_t device, nvmlClockType_t clockType, unsigned int *clockMHz);
nvmlReturn_t nvmlDeviceGetUtilizationRates(nvmlDevice_t device, nvmlUtilization_t *utilization);
nvmlReturn_t nvmlDeviceGetEncoderUtilization(nvmlDevice_t device, unsigned int *utilization, unsigned int *samplingPeriodUs);
nvmlReturn_t nvmlDeviceGetDecoderUtilization(nvmlDevice_t device, unsigned int *utilization, unsigned int *samplingPeriodUs);

nvmlReturn_t nvmlDeviceGetPersistenceMode(nvmlDevice_t device, nvmlEnableState_t *mode);
nvmlReturn_t nvmlDeviceGetComputeMode(nvmlDevice_t device, nvmlComputeMode_t *mode);
nvmlReturn_t nvmlDeviceGetDisplayActive(nvmlDevice_t device, nvmlEnableState_t *isActive);
nvmlReturn_t nvmlDeviceGetDisplayMode(nvmlDevice_t device, nvmlEnableState_t *display);
nvmlReturn_t nvmlDeviceGetEccMode(nvmlDevice_t device, nvmlEnableState_t *current, nvmlEnableState_t *pending);
nvmlReturn_t nvmlDeviceGetMigMode(nvmlDevice_t device, unsigned int *currentMode, unsigned int *pendingMode);

nvmlReturn_t nvmlDeviceGetComputeRunningProcesses(nvmlDevice_t device, unsigned int *infoCount, nvmlProcessInfo_v1_t *infos);
nvmlReturn_t nvmlDeviceGetComputeRunningProcesses_v2(nvmlDevice_t device, unsigned int *infoCount, nvmlProcessInfo_v2_t *infos);
nvmlReturn_t nvmlDeviceGetComputeRunningProcesses_v3(nvmlDevice_t device, unsigned int *infoCount, nvmlProcessInfo_v3_t *infos);
nvmlReturn_t nvmlDeviceGetGraphicsRunningProcesses(nvmlDevice_t device, unsigned int *infoCount, nvmlProcessInfo_v1_t *infos);
nvmlReturn_t nvmlDeviceGetGraphicsRunningProcesses_v2(nvmlDevice_t device, unsigned int *infoCount, nvmlProcessInfo_v2_t *infos);
nvmlReturn_t nvmlDeviceGetGraphicsRunningProcesses_v3(nvmlDevice_t device, unsigned int *infoCount, nvmlProcessInfo_v3_t *infos);

/* vGPU Host APIs */
nvmlReturn_t nvmlGetVgpuDriverCapabilities(unsigned int capability, unsigned int *capResult);
nvmlReturn_t nvmlDeviceGetVgpuCapabilities(nvmlDevice_t device, unsigned int capability, unsigned int *capResult);
nvmlReturn_t nvmlDeviceGetHostVgpuMode(nvmlDevice_t device, nvmlHostVgpuMode_t *pHostVgpuMode);
nvmlReturn_t nvmlDeviceGetVirtualizationMode(nvmlDevice_t device, nvmlGpuVirtualizationMode_t *pVirtualMode);
nvmlReturn_t nvmlDeviceGetSupportedVgpus(nvmlDevice_t device, unsigned int *vgpuCount, nvmlVgpuTypeId_t *vgpuTypeIds);
nvmlReturn_t nvmlDeviceGetCreatableVgpus(nvmlDevice_t device, unsigned int *vgpuCount, nvmlVgpuTypeId_t *vgpuTypeIds);
nvmlReturn_t nvmlDeviceGetActiveVgpus(nvmlDevice_t device, unsigned int *vgpuCount, nvmlVgpuInstance_t *vgpuInstances);

nvmlReturn_t nvmlVgpuTypeGetName(nvmlVgpuTypeId_t vgpuTypeId, char *vgpuTypeName, unsigned int *size);
nvmlReturn_t nvmlVgpuTypeGetClass(nvmlVgpuTypeId_t vgpuTypeId, char *vgpuTypeClass, unsigned int *size);
nvmlReturn_t nvmlVgpuTypeGetDeviceID(nvmlVgpuTypeId_t vgpuTypeId, unsigned long long *deviceID, unsigned long long *subsystemID);
nvmlReturn_t nvmlVgpuTypeGetFramebufferSize(nvmlVgpuTypeId_t vgpuTypeId, unsigned long long *fbSize);
nvmlReturn_t nvmlVgpuTypeGetNumDisplayHeads(nvmlVgpuTypeId_t vgpuTypeId, unsigned int *numDisplayHeads);
nvmlReturn_t nvmlVgpuTypeGetResolution(nvmlVgpuTypeId_t vgpuTypeId, unsigned int displayIndex, unsigned int *xdim, unsigned int *ydim);
nvmlReturn_t nvmlVgpuTypeGetLicense(nvmlVgpuTypeId_t vgpuTypeId, char *vgpuTypeLicenseString, unsigned int size);
nvmlReturn_t nvmlVgpuTypeGetFrameRateLimit(nvmlVgpuTypeId_t vgpuTypeId, unsigned int *frameRateLimit);
nvmlReturn_t nvmlVgpuTypeGetMaxInstances(nvmlDevice_t device, nvmlVgpuTypeId_t vgpuTypeId, unsigned int *vgpuInstanceCount);
nvmlReturn_t nvmlVgpuTypeGetMaxInstancesPerVm(nvmlVgpuTypeId_t vgpuTypeId, unsigned int *vgpuInstanceCountPerVm);
nvmlReturn_t nvmlVgpuTypeGetGpuInstanceProfileId(nvmlVgpuTypeId_t vgpuTypeId, unsigned int *gpuInstanceProfileId);
nvmlReturn_t nvmlVgpuTypeGetCapabilities(nvmlVgpuTypeId_t vgpuTypeId, unsigned int capability, unsigned int *capResult);

nvmlReturn_t nvmlVgpuInstanceGetVmID(nvmlVgpuInstance_t vgpuInstance, char *vmId, unsigned int size, nvmlVgpuVmIdType_t *vmIdType);
nvmlReturn_t nvmlVgpuInstanceGetUUID(nvmlVgpuInstance_t vgpuInstance, char *uuid, unsigned int size);
nvmlReturn_t nvmlVgpuInstanceGetMdevUUID(nvmlVgpuInstance_t vgpuInstance, char *mdevUuid, unsigned int size);
nvmlReturn_t nvmlVgpuInstanceGetVmDriverVersion(nvmlVgpuInstance_t vgpuInstance, char *version, unsigned int length);
nvmlReturn_t nvmlVgpuInstanceGetFbUsage(nvmlVgpuInstance_t vgpuInstance, unsigned long long *fbUsage);
nvmlReturn_t nvmlVgpuInstanceGetLicenseStatus(nvmlVgpuInstance_t vgpuInstance, unsigned int *licensed);
nvmlReturn_t nvmlVgpuInstanceGetLicenseInfo(nvmlVgpuInstance_t vgpuInstance, nvmlVgpuLicenseInfo_t *licenseInfo);
nvmlReturn_t nvmlVgpuInstanceGetLicenseInfo_v2(nvmlVgpuInstance_t vgpuInstance, nvmlVgpuLicenseInfo_t *licenseInfo);
nvmlReturn_t nvmlVgpuInstanceGetType(nvmlVgpuInstance_t vgpuInstance, nvmlVgpuTypeId_t *vgpuTypeId);
nvmlReturn_t nvmlVgpuInstanceGetFrameRateLimit(nvmlVgpuInstance_t vgpuInstance, unsigned int *frameRateLimit);
nvmlReturn_t nvmlVgpuInstanceGetEccMode(nvmlVgpuInstance_t vgpuInstance, nvmlEnableState_t *eccMode);
nvmlReturn_t nvmlVgpuInstanceGetEncoderCapacity(nvmlVgpuInstance_t vgpuInstance, unsigned int *encoderCapacity);
nvmlReturn_t nvmlVgpuInstanceGetGpuInstanceId(nvmlVgpuInstance_t vgpuInstance, unsigned int *gpuInstanceId);
nvmlReturn_t nvmlVgpuInstanceGetGpuPciId(nvmlVgpuInstance_t vgpuInstance, char *vgpuPciId, unsigned int *length);

/* GSP / XID Events */
nvmlReturn_t nvmlDeviceGetGspFirmwareMode(nvmlDevice_t device, unsigned int *isEnabled, unsigned int *defaultMode);
nvmlReturn_t nvmlDeviceGetGspFirmwareVersion(nvmlDevice_t device, char *version);
nvmlReturn_t nvmlEventSetCreate(nvmlEventSet_t *set);
nvmlReturn_t nvmlDeviceRegisterEvents(nvmlDevice_t device, unsigned long long eventTypes, nvmlEventSet_t set);
nvmlReturn_t nvmlDeviceGetSupportedEventTypes(nvmlDevice_t device, unsigned long long *eventTypes);
nvmlReturn_t nvmlEventSetWait(nvmlEventSet_t set, nvmlEventData_t *data, unsigned int timeoutms);
nvmlReturn_t nvmlEventSetWait_v2(nvmlEventSet_t set, nvmlEventData_t *data, unsigned int timeoutms);
nvmlReturn_t nvmlEventSetFree(nvmlEventSet_t set);

/* Test / Mock injection hook for offline unit tests */
typedef int (*open_nvml_ioctl_fn)(int fd, unsigned long request, void *arg);
void open_nvml_set_test_hooks(open_nvml_ioctl_fn ioctl_hook, const char *driver_version_override);

#ifdef __cplusplus
}
#endif

#endif /* OPEN_NVML_H */
