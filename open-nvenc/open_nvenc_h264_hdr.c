/* SPDX-License-Identifier: MIT */
/*
 * open_nvenc_h264_hdr.c - Clean-room ITU-T H.264 SPS/PPS Bitstream Writer
 *
 * Emits Annex-B parameter sets compatible with the GP104 NVC2B7 Falcon H.264
 * hardware slice syntax for 8-bit 4:2:0 (Main) and 4:4:4 (High 4:4:4 Predictive).
 */

#include "open_nvenc_h264_hdr.h"

#include <string.h>

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   byte_pos;
    uint32_t cur_byte;
    int      bit_pos;
    int      overflow;
} h264_bitwriter_t;

static void bw_init(h264_bitwriter_t *bw, uint8_t *buf, size_t cap)
{
    bw->buf      = buf;
    bw->cap      = cap;
    bw->byte_pos = 0U;
    bw->cur_byte = 0U;
    bw->bit_pos  = 0;
    bw->overflow = 0;
}

static void bw_put_bit(h264_bitwriter_t *bw, uint32_t bit)
{
    if (bw->overflow) {
        return;
    }
    bw->cur_byte = (bw->cur_byte << 1) | (bit & 1U);
    bw->bit_pos++;
    if (bw->bit_pos == 8) {
        if (bw->byte_pos >= bw->cap) {
            bw->overflow = 1;
            return;
        }
        bw->buf[bw->byte_pos++] = (uint8_t)bw->cur_byte;
        bw->cur_byte = 0U;
        bw->bit_pos  = 0;
    }
}

static void bw_put_bits(h264_bitwriter_t *bw, uint32_t val, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        bw_put_bit(bw, (val >> i) & 1U);
    }
}

static void bw_put_ue(h264_bitwriter_t *bw, uint32_t val)
{
    uint32_t code = val + 1U;
    int leading_zeros = 0;
    uint32_t tmp = code;
    while (tmp > 1U) {
        tmp >>= 1;
        leading_zeros++;
    }
    for (int i = 0; i < leading_zeros; i++) {
        bw_put_bit(bw, 0U);
    }
    bw_put_bits(bw, code, leading_zeros + 1);
}

static void bw_put_se(h264_bitwriter_t *bw, int32_t val)
{
    uint32_t mapped = (val <= 0) ? (uint32_t)(-2 * val) : (uint32_t)(2 * val - 1);
    bw_put_ue(bw, mapped);
}

static void bw_rbsp_trailing(h264_bitwriter_t *bw)
{
    bw_put_bit(bw, 1U);
    while (bw->bit_pos != 0 && !bw->overflow) {
        bw_put_bit(bw, 0U);
    }
}

static size_t emit_h264_nal(uint8_t *dst, size_t cap,
                            uint8_t nal_header_byte,
                            const uint8_t *rbsp, size_t rbsp_len)
{
    if (!dst || cap < 5U || !rbsp) {
        return 0U;
    }
    size_t out = 0U;
    dst[out++] = 0x00U;
    dst[out++] = 0x00U;
    dst[out++] = 0x00U;
    dst[out++] = 0x01U;
    dst[out++] = nal_header_byte;

    int zeros = 0;
    for (size_t i = 0U; i < rbsp_len; i++) {
        uint8_t b = rbsp[i];
        if (zeros >= 2 && b <= 0x03U) {
            if (out >= cap) {
                return 0U;
            }
            dst[out++] = 0x03U;
            zeros = 0;
        }
        if (out >= cap) {
            return 0U;
        }
        dst[out++] = b;
        zeros = (b == 0x00U) ? (zeros + 1) : 0;
    }
    return out;
}

uint8_t open_nvenc_h264_compute_level_idc(uint32_t width, uint32_t height)
{
    uint32_t mb_w = (width + 15U) >> 4;
    uint32_t mb_h = (height + 15U) >> 4;
    uint32_t n_mbs = mb_w * mb_h;
    if (n_mbs <= 792U) {
        return 21U; /* 2.1 (up to 400p) */
    }
    if (n_mbs <= 3600U) {
        return 31U; /* 3.1 (720p) */
    }
    if (n_mbs <= 8192U) {
        return 41U; /* 4.1 (1080p) */
    }
    return 51U;
}

static size_t build_h264_sps_rbsp(uint8_t *rbsp, size_t cap,
                                  uint32_t width, uint32_t height,
                                  uint32_t chroma_format_idc)
{
    h264_bitwriter_t bw;
    bw_init(&bw, rbsp, cap);

    uint32_t chroma     = (chroma_format_idc == 3U) ? 3U : 1U;
    uint8_t profile_idc = (chroma == 3U) ? 244U : 77U; /* High 4:4:4 Predictive vs Main */
    uint8_t level_idc   = open_nvenc_h264_compute_level_idc(width, height);
    uint8_t constraint  = (chroma == 1U) ? 0x40U : 0x00U; /* constraint_set1_flag for Main */

    bw_put_bits(&bw, profile_idc, 8);
    bw_put_bits(&bw, constraint, 8);
    bw_put_bits(&bw, level_idc, 8);
    bw_put_ue(&bw, 0U); /* seq_parameter_set_id = 0 */

    if (profile_idc >= 100U) {
        bw_put_ue(&bw, chroma); /* chroma_format_idc */
        if (chroma == 3U) {
            bw_put_bit(&bw, 0U); /* separate_colour_plane_flag = 0 */
        }
        bw_put_ue(&bw, 0U); /* bit_depth_luma_minus8 = 0 */
        bw_put_ue(&bw, 0U); /* bit_depth_chroma_minus8 = 0 */
        bw_put_bit(&bw, 0U); /* qpprime_y_zero_transform_bypass_flag = 0 */
        bw_put_bit(&bw, 0U); /* seq_scaling_matrix_present_flag = 0 */
    }

    bw_put_ue(&bw, 4U); /* log2_max_frame_num_minus4 = 4 (8-bit frame_num) */
    bw_put_ue(&bw, 0U); /* pic_order_cnt_type = 0 */
    bw_put_ue(&bw, 4U); /* log2_max_pic_order_cnt_lsb_minus4 = 4 (8-bit poc_lsb) */
    bw_put_ue(&bw, 1U); /* max_num_ref_frames = 1 */
    bw_put_bit(&bw, 0U); /* gaps_in_frame_num_value_allowed_flag = 0 */

    uint32_t mb_w = (width + 15U) >> 4;
    uint32_t mb_h = (height + 15U) >> 4;
    bw_put_ue(&bw, mb_w - 1U); /* pic_width_in_mbs_minus1 */
    bw_put_ue(&bw, mb_h - 1U); /* pic_height_in_map_units_minus1 */
    bw_put_bit(&bw, 1U); /* frame_mbs_only_flag = 1 */
    bw_put_bit(&bw, 1U); /* direct_8x8_inference_flag = 1 */

    uint32_t sub_w = (chroma == 3U) ? 1U : 2U;
    uint32_t sub_h = (chroma == 3U) ? 1U : 2U;
    uint32_t crop_r = ((mb_w * 16U) - width) / sub_w;
    uint32_t crop_b = ((mb_h * 16U) - height) / sub_h;
    if (crop_r != 0U || crop_b != 0U) {
        bw_put_bit(&bw, 1U); /* frame_cropping_flag */
        bw_put_ue(&bw, 0U);
        bw_put_ue(&bw, crop_r);
        bw_put_ue(&bw, 0U);
        bw_put_ue(&bw, crop_b);
    } else {
        bw_put_bit(&bw, 0U);
    }

    bw_put_bit(&bw, 0U); /* vui_parameters_present_flag = 0 */
    bw_rbsp_trailing(&bw);
    return bw.overflow ? 0U : bw.byte_pos;
}

static size_t build_h264_pps_rbsp(uint8_t *rbsp, size_t cap, uint32_t chroma_format_idc)
{
    h264_bitwriter_t bw;
    bw_init(&bw, rbsp, cap);

    uint32_t chroma = (chroma_format_idc == 3U) ? 3U : 1U;
    bw_put_ue(&bw, 0U); /* pic_parameter_set_id */
    bw_put_ue(&bw, 0U); /* seq_parameter_set_id */
    bw_put_bit(&bw, (chroma == 3U) ? 0U : 1U); /* entropy_coding_mode_flag: CAVLC for 4:4:4, CABAC for 4:2:0 */
    bw_put_bit(&bw, 0U); /* bottom_field_pic_order_in_frame_present_flag = 0 */
    bw_put_ue(&bw, 0U); /* num_slice_groups_minus1 = 0 */
    bw_put_ue(&bw, 0U); /* num_ref_idx_l0_default_active_minus1 = 0 */
    bw_put_ue(&bw, 0U); /* num_ref_idx_l1_default_active_minus1 = 0 */
    bw_put_bit(&bw, 0U); /* weighted_pred_flag = 0 */
    bw_put_bits(&bw, 0U, 2); /* weighted_bipred_idc = 0 */
    bw_put_se(&bw, 0); /* pic_init_qp_minus26 = 0 */
    bw_put_se(&bw, 0); /* pic_init_qs_minus26 = 0 */
    bw_put_se(&bw, (chroma == 3U) ? 6 : 0); /* chroma_qp_index_offset */
    bw_put_bit(&bw, 1U); /* deblocking_filter_control_present_flag = 1 */
    bw_put_bit(&bw, 0U); /* constrained_intra_pred_flag = 0 */
    bw_put_bit(&bw, 0U); /* redundant_pic_cnt_present_flag = 0 */
    if (chroma == 3U) {
        bw_put_bit(&bw, 1U); /* transform_8x8_mode_flag = 1 */
        bw_put_bit(&bw, 0U); /* pic_scaling_matrix_present_flag = 0 */
        bw_put_se(&bw, 6);   /* second_chroma_qp_index_offset */
    }
    bw_rbsp_trailing(&bw);
    return bw.overflow ? 0U : bw.byte_pos;
}

size_t open_nvenc_h264_write_headers(uint8_t *out, size_t cap,
                                     uint32_t width, uint32_t height,
                                     uint32_t chroma_format_idc)
{
    uint8_t rbsp[128];
    size_t pos = 0U;

    size_t rlen = build_h264_sps_rbsp(rbsp, sizeof(rbsp), width, height, chroma_format_idc);
    if (rlen == 0U) {
        return 0U;
    }
    size_t n = emit_h264_nal(out + pos, cap - pos, 0x67U, rbsp, rlen);
    if (n == 0U) {
        return 0U;
    }
    pos += n;

    rlen = build_h264_pps_rbsp(rbsp, sizeof(rbsp), chroma_format_idc);
    if (rlen == 0U) {
        return 0U;
    }
    n = emit_h264_nal(out + pos, cap - pos, 0x68U, rbsp, rlen);
    if (n == 0U) {
        return 0U;
    }
    pos += n;
    return pos;
}
