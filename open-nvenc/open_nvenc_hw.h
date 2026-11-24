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
    uint32_t qp;                /* Intra / default constqp (e.g. 24) */
    uint32_t qp_p;              /* Inter-P constqp (0 -> use qp) */
    uint32_t gop_length;        /* 0 -> infinite / 250 */
    int      is_h264;           /* 0 = HEVC, 1 = H.264 */
    uint32_t rc_mode;           /* 0 = ConstQP, 1 = VBR, 2 = CBR */
    uint32_t average_bitrate;   /* bps */
    uint32_t max_bitrate;       /* bps */
    uint32_t vbv_buffer_size;   /* bits */
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t min_qp;
    uint32_t max_qp;
} open_nvenc_hw_init_cfg_t;

int open_nvenc_hw_create(open_nvenc_hw_ctx_t **out_ctx,
                         int ctl_fd, int dev_fd,
                         const open_nvenc_hw_init_cfg_t *cfg,
                         open_nvenc_rm_stats_t *rm_stats,
                         char *err_buf, size_t err_cap);

void open_nvenc_hw_destroy(open_nvenc_hw_ctx_t *ctx,
                           open_nvenc_rm_stats_t *rm_stats);

void open_nvenc_hw_invalidate_refs(open_nvenc_hw_ctx_t *ctx);

void open_nvenc_hw_update_cfg(open_nvenc_hw_ctx_t *ctx,
                              uint32_t gop_length,
                              uint32_t qp,
                              uint32_t qp_p,
                              uint32_t rc_mode,
                              uint32_t average_bitrate,
                              uint32_t max_bitrate,
                              uint32_t vbv_buffer_size,
                              uint32_t fps_num,
                              uint32_t fps_den,
                              int force_idr);

NVENCSTATUS open_nvenc_hw_encode_frame(open_nvenc_hw_ctx_t *ctx,
                                       const uint8_t *src_data,
                                       uint32_t src_pitch,
                                       NV_ENC_BUFFER_FORMAT src_fmt,
                                       const uint32_t chroma_offset[2],
                                       int force_idr,
                                       int emit_spspps,
                                       uint8_t *out_bs,
                                       size_t out_bs_cap,
                                       size_t *out_bs_len,
                                       int *out_is_idr,
                                       uint32_t *out_avg_qp,
                                       open_nvenc_rm_stats_t *rm_stats,
                                       char *err_buf, size_t err_cap);

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
