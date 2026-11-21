#ifndef RM_CLIENT_H
#define RM_CLIENT_H

#include "nv_vgpu_vfio_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional hook table for unit tests without /dev/nvidia* or /dev/nvidia-vgpu-vfio. */
typedef struct {
    int (*open_fn)(const char *path, int flags);
    int (*close_fn)(int fd);
    int (*ioctl_fn)(int fd, unsigned long req, void *arg);
    void *(*mmap64_fn)(void *addr, size_t len, int prot, int flags, int fd, long long off);
    int (*munmap_fn)(void *addr, size_t len);
} rm_sys_ops_t;

void rm_set_sys_ops(const rm_sys_ops_t *ops);
void rm_reset_sys_ops(void);

/* Configure paths used for device-node creation & proc lookups (for tests). */
void rm_set_proc_paths(const char *proc_devices, const char *nvidia_params);

/* Device-node helpers matching NVIDIA user-space RM initialization. */
int rm_get_chardev_major(const char *dev_name);
void rm_read_params_file(const char *params_path,
                         unsigned int *uid_out,
                         unsigned int *gid_out,
                         unsigned int *mode_out,
                         int *modify_out);
int rm_ensure_vgpu_chardev(int minor);
int rm_ensure_nvidia_chardev(unsigned int minor);

/* RM client & handle lifecycle */
NV_STATUS rm_alloc_client(uint32_t *out_client);
NV_STATUS rm_alloc_object(uint32_t hClient, uint32_t hParent, uint32_t hNew,
                          uint32_t hClass, void *pParams, uint32_t paramsSize);
NV_STATUS rm_alloc_memory(uint32_t hClient, uint32_t hParent, uint32_t hMemory,
                          uint32_t hClass, uint32_t flags,
                          void **pSysMemInOut, uint64_t *limitInOut);
NV_STATUS rm_control(uint32_t hClient, uint32_t hObject, uint32_t cmd,
                     void *pParams, uint32_t paramsSize);
NV_STATUS rm_free(uint32_t hClient, uint32_t hParent, uint32_t hObject);

NV_STATUS rm_map_memory(uint32_t hClient, uint32_t hDevice, uint32_t hMemory,
                        uint64_t offset, uint64_t length,
                        void **pLinearOut, uint32_t flags);
NV_STATUS rm_unmap_memory(uint32_t hClient, uint32_t hDevice, uint32_t hMemory,
                          void *linearAddr, uint32_t flags);

NV_STATUS rm_alloc_os_event(uint32_t hClient, uint32_t hParent, int *out_fd);
NV_STATUS rm_free_os_event(uint32_t hClient, uint32_t hParent, int fd);

/* Helper wrapper around NV_VGPU_DEV_IOCTL_PARAMS on vgpu-vfio fds. */
NV_STATUS vfio_vgpu_ioctl(int fd, unsigned long req, void *params, uint32_t size);

/* Reset cached RM fds & mappings inside newly forked child process. */
void rm_reset_after_fork(void);

/* Close with EINTR/FD_CLOEXEC helper */
int rm_set_cloexec(int fd);
int rm_open_rw_cloexec(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* RM_CLIENT_H */
