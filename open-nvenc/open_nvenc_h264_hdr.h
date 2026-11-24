/* SPDX-License-Identifier: MIT */
#ifndef OPEN_NVENC_H264_HDR_H
#define OPEN_NVENC_H264_HDR_H

#include <stddef.h>
#include <stdint.h>

size_t open_nvenc_h264_write_headers(uint8_t *out, size_t cap,
                                     uint32_t width, uint32_t height,
                                     uint32_t chroma_format_idc);

uint8_t open_nvenc_h264_compute_level_idc(uint32_t width, uint32_t height);

#endif /* OPEN_NVENC_H264_HDR_H */
