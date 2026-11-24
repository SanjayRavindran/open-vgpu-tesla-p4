/* SPDX-License-Identifier: MIT */
#ifndef OPEN_NVENC_HW_H
#define OPEN_NVENC_HW_H

#include "open_nvenc.h"

#include <stddef.h>
#include <stdint.h>

typedef struct _open_nvenc_hw_ctx open_nvenc_hw_ctx_t;

typedef struct _open_nvenc_hw_init_cfg {
    uint32_t width;
    uint32_t height;
    uint32_t chroma_format_idc; /* 1 = 4:2:0, 3 = 4:4:4 */
    uint32_t bit_depth;         /* 8 or 10 */
    uint32_t qp;                /* constqp (e.g. 24) */
} open_nvenc_hw_init_cfg_t;

int open_nvenc_hw_create(open_nvenc_hw_ctx_t **out_ctx,
                         int ctl_fd, int dev_fd,
                         const open_nvenc_hw_init_cfg_t *cfg,
                         open_nvenc_rm_stats_t *rm_stats,
                         char *err_buf, size_t err_cap);

void open_nvenc_hw_destroy(open_nvenc_hw_ctx_t *ctx,
                           open_nvenc_rm_stats_t *rm_stats);

NVENCSTATUS open_nvenc_hw_encode_idr(open_nvenc_hw_ctx_t *ctx,
                                     const uint8_t *src_data,
                                     uint32_t src_pitch,
                                     NV_ENC_BUFFER_FORMAT src_fmt,
                                     uint8_t *out_bs,
                                     size_t out_bs_cap,
                                     size_t *out_bs_len,
                                     open_nvenc_rm_stats_t *rm_stats,
                                     char *err_buf, size_t err_cap);

#endif /* OPEN_NVENC_HW_H */
