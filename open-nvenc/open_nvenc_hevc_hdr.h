/* SPDX-License-Identifier: MIT */
#ifndef OPEN_NVENC_HEVC_HDR_H
#define OPEN_NVENC_HEVC_HDR_H

#include <stddef.h>
#include <stdint.h>

/*
 * Generate Annex-B HEVC VPS (NAL 32) + SPS (NAL 33) + PPS (NAL 34) matching
 * GP104 NVC2B7 hardware slice parameters.
 *
 * Returns total bytes written to dst (0 on buffer overflow / invalid params).
 */
size_t open_nvenc_hevc_write_headers(uint8_t *dst, size_t cap,
                                     uint32_t width, uint32_t height,
                                     uint32_t chroma_format_idc,
                                     uint32_t bit_depth);

#endif /* OPEN_NVENC_HEVC_HDR_H */
