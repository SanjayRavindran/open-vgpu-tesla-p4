#ifndef OPEN_VGXCFG_H
#define OPEN_VGXCFG_H

#include <stdint.h>

#define VGX_CFG_OK              0
#define VGX_CFG_ERR_XML         1
#define VGX_CFG_ERR_BAD_HANDLE  2
#define VGX_CFG_ERR_MISSING     3
#define VGX_CFG_ERR_SMALL_BUF   4
#define VGX_CFG_ERR_NULL_PARAM  5
#define VGX_CFG_ERR_NOT_FOUND   6
#define VGX_CFG_ERR_OUT_OF_RANGE 7

int vgxConfigOpen(void **handle);
int vgxConfigClose(void *handle);
int vgxConfigReadGlobalSettings(void *handle, void *buf, uint32_t *size);
int vgxConfigWriteGlobalSettings(void *handle);
int vgxConfigReadPhysGpu(void *handle, const uint16_t dev_id[4], void *buf, uint32_t *size);
int vgxConfigReadPhysGpuSupportedUsm(
    void *handle, const uint16_t dev_id[4], uint32_t index, void *buf, uint32_t *size);
int vgxConfigReadUsmTypeByIndex(void *handle, uint32_t index, void *buf, uint32_t *size);
int vgxConfigReadUsmTypeById(void *handle, uint32_t usm_id, void *buf, uint32_t *size);
int vgxConfigReadUsmTypeDisplay(
    void *handle, uint32_t usm_id, uint32_t display_index, void *buf, uint32_t *size);

#endif
