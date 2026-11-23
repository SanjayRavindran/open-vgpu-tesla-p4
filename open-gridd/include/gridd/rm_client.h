#ifndef GRIDD_RM_CLIENT_H
#define GRIDD_RM_CLIENT_H

#include "gridd/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NV_ESC_RM_ALLOC            0xc020462bU
#define NV_ESC_RM_FREE             0xc0104629U
#define NV_ESC_RM_CONTROL          0xc020462aU
#define NV_ESC_CHECK_VERSION_STR   0xc04846d2U
#define NV_ESC_CARD_INFO           0xc00446c9U
#define NV_ESC_SYS_PARAMS          0xc00846d6U
#define NV_ESC_ATTACH_GPUS_TO_FD   0xc00c46d1U
#define NV_ESC_STATUS_CODE         0xc01046d3U

#define NV0000_CTRL_CMD_SYSTEM_GET_BUILD_VERSION   0x00000101U
#define NV0000_CTRL_CMD_SYSTEM_GET_HOST_VERSION    0x00000133U
#define NV0000_CTRL_CMD_GPU_GET_ATTACHED_IDS       0x00000201U
#define NV0000_CTRL_CMD_GPU_GET_ID_INFO            0x00000202U
#define NV0000_CTRL_CMD_GPU_ATTACH_IDS             0x00000215U

#define NV0080_CTRL_CMD_GPU_MODIFY_SW_STATE_PERSISTENCE 0x00800287U
#define NV0080_CTRL_CMD_GPU_GET_VIRTUALIZATION_MODE     0x00800289U

#define NV2080_CTRL_CMD_GPU_DISABLE_LICENSED_FEATURE    0x20800164U
#define NV2080_CTRL_CMD_GPU_ENABLE_LICENSED_FEATURE     0x20800186U
#define NV2080_CTRL_CMD_GPU_SET_LICENSE_STATUS          0x20800187U
#define NV2080_CTRL_CMD_GPU_GET_CSP_IDENTIFIER          0x20800189U
#define NV2080_CTRL_CMD_GPU_GET_UNLICENSED_STATE        0x2080018cU
#define NV2080_CTRL_CMD_BUS_GET_PCI_INFO                0x20801801U
#define NV2080_CTRL_CMD_GSP_GET_FEATURES                0x20803601U
#define NV2080_CTRL_CMD_GPU_GET_LICENSABLE_FEATURES     0x20808161U

#define NV_VGPU_VIRTUALIZATION_MODE_GUEST 2U
#define NV_PCI_DEVID_TESLA_M60            0x13f2U

typedef struct {
    uint32_t feature_code;
    char product_name[256];
    bool active;
} gridd_rm_feature_info_t;

typedef struct {
    uint32_t gpu_id;
    uint32_t gpu_instance;
    uint32_t h_device;
    uint32_t h_subdevice;
    uint32_t h_vgpu;
    uint32_t virtualization_mode;
    uint16_t pci_vendor_id;
    uint16_t pci_device_id;
    uint32_t pci_domain;
    uint8_t pci_bus;
    uint8_t pci_slot;
    bool pci_queried;
    bool licensable;
    uint32_t feature_count;
    gridd_rm_feature_info_t features[GRIDD_MAX_FEATURES];
} gridd_rm_gpu_t;

typedef struct gridd_rm_ops {
    int (*open_ctl)(void *ctx);
    void (*close_ctl)(void *ctx, int ctl_fd);
    uint32_t (*rm_alloc)(void *ctx, int ctl_fd, uint32_t h_client, uint32_t h_parent,
                         uint32_t h_object, uint32_t h_class, void *params, uint32_t params_size,
                         uint32_t *out_client);
    uint32_t (*rm_free)(void *ctx, int ctl_fd, uint32_t h_client, uint32_t h_parent, uint32_t h_object);
    uint32_t (*rm_control)(void *ctx, int ctl_fd, uint32_t h_client, uint32_t h_object,
                           uint32_t cmd, void *params, uint32_t params_size);
    void *ctx;
} gridd_rm_ops_t;

typedef struct {
    const gridd_rm_ops_t *ops;
    int ctl_fd;
    uint32_t h_client;
    uint32_t gpu_count;
    gridd_rm_gpu_t gpus[GRIDD_MAX_GPUS];
    bool is_vgpu_guest;
} gridd_rm_client_t;

const gridd_rm_ops_t *gridd_rm_default_ops(void);
gridd_status_t gridd_rm_init(gridd_rm_client_t *client, const gridd_rm_ops_t *ops);
void gridd_rm_destroy(gridd_rm_client_t *client);

gridd_status_t gridd_rm_set_sw_persistence(gridd_rm_client_t *client, bool enable);
gridd_status_t gridd_rm_query_features(gridd_rm_client_t *client);
bool gridd_rm_gpu_supports_feature(gridd_rm_client_t *client, uint32_t gpu_idx,
                                   uint32_t feature_code, uint32_t *out_feat_idx);
gridd_status_t gridd_rm_query_pci_ids(gridd_rm_client_t *client);
gridd_status_t gridd_rm_get_csp_id(gridd_rm_client_t *client, uint32_t gpu_idx, uint32_t *out_csp_id);
gridd_status_t gridd_rm_enable_feature(gridd_rm_client_t *client, uint32_t feature_code,
                                       uint32_t *out_active_feature, bool *out_reboot_required);
gridd_status_t gridd_rm_set_license_status(gridd_rm_client_t *client, uint32_t feature_code,
                                           uint32_t expiry_epoch, uint8_t state);
gridd_status_t gridd_rm_disable_feature(gridd_rm_client_t *client, uint32_t feature_code);
gridd_status_t gridd_rm_get_telemetry(gridd_rm_client_t *client,
                                      char *guest_ver, size_t guest_sz,
                                      char *host_ver, size_t host_sz);
gridd_status_t gridd_rm_get_unlicensed_state(gridd_rm_client_t *client, uint32_t gpu_idx,
                                             uint32_t *out_state);
int gridd_rm_start_unlicensed_monitor(gridd_rm_client_t *client, int stop_pipe_rd);

#ifdef __cplusplus
}
#endif

#endif /* GRIDD_RM_CLIENT_H */
