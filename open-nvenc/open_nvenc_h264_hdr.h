/* SPDX-License-Identifier: MIT */
#ifndef OPEN_NVENC_H264_HDR_H
#define OPEN_NVENC_H264_HDR_H

#include <stddef.h>
#include <stdint.h>

typedef struct _open_nvenc_h264_hdr_params {
    uint32_t width;
    uint32_t height;
    uint32_t chroma_format_idc;
    uint32_t level_idc;
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t video_signal_type_present;
    uint32_t video_format;
    uint32_t video_full_range;
    uint32_t colour_desc_present;
    uint32_t colour_primaries;
    uint32_t transfer_characteristics;
    uint32_t matrix_coeffs;
} open_nvenc_h264_hdr_params_t;

size_t open_nvenc_h264_write_headers_ex(uint8_t *out, size_t cap,
                                        const open_nvenc_h264_hdr_params_t *p);

size_t open_nvenc_h264_write_headers(uint8_t *out, size_t cap,
                                     uint32_t width, uint32_t height,
                                     uint32_t chroma_format_idc);

uint8_t open_nvenc_h264_compute_level_idc_fps(uint32_t width, uint32_t height,
                                              uint32_t fps_num, uint32_t fps_den);

uint8_t open_nvenc_h264_compute_level_idc(uint32_t width, uint32_t height);

#endif /* OPEN_NVENC_H264_HDR_H */
