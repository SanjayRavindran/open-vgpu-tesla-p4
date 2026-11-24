/* SPDX-License-Identifier: MIT */
#ifndef OPEN_NVENC_HEVC_HDR_H
#define OPEN_NVENC_HEVC_HDR_H

#include <stddef.h>
#include <stdint.h>

typedef struct _open_nvenc_hevc_hdr_params {
    uint32_t width;
    uint32_t height;
    uint32_t chroma_format_idc;
    uint32_t bit_depth;
    uint32_t level_idc;     /* 0 = auto from coded_w*coded_h & fps */
    uint32_t tier;          /* 0 = Main, 1 = High */
    uint32_t fps_num;
    uint32_t fps_den;
    uint32_t overscan_present;
    uint32_t overscan_info;
    uint32_t video_signal_type_present;
    uint32_t video_format;
    uint32_t video_full_range;
    uint32_t colour_desc_present;
    uint32_t colour_primaries;
    uint32_t transfer_characteristics;
    uint32_t matrix_coeffs;
    uint32_t chroma_loc_present;
    uint32_t chroma_loc_top;
    uint32_t chroma_loc_bot;
    uint32_t timing_present;
    uint32_t num_units_in_tick;
    uint32_t time_scale;
} open_nvenc_hevc_hdr_params_t;

/*
 * Generate Annex-B HEVC VPS (NAL 32) + SPS (NAL 33) + PPS (NAL 34) matching
 * GP104 NVC2B7 hardware slice parameters, level/tier, and VUI signaling.
 *
 * Returns total bytes written to dst (0 on buffer overflow / invalid params).
 */
size_t open_nvenc_hevc_write_headers_ex(uint8_t *dst, size_t cap,
                                        const open_nvenc_hevc_hdr_params_t *p);

size_t open_nvenc_hevc_write_headers(uint8_t *dst, size_t cap,
                                     uint32_t width, uint32_t height,
                                     uint32_t chroma_format_idc,
                                     uint32_t bit_depth);

#endif /* OPEN_NVENC_HEVC_HDR_H */
