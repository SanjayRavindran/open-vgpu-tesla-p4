/* SPDX-License-Identifier: MIT */
/*
 * open_nvenc_hevc_hdr.c - Clean-room ITU-T H.265 VPS/SPS/PPS Bitstream Writer
 *
 * Emits Annex-B parameter sets compatible with the GP104 NVC2B7 Falcon HEVC
 * hardware slice syntax for 8-bit / 10-bit 4:2:0 and 4:4:4 profiles.
 */

#include "open_nvenc_hevc_hdr.h"

#include <string.h>

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   byte_pos;
    uint32_t cur_byte;
    int      bit_pos; /* 0..7 bits filled in cur_byte */
    int      overflow;
} hevc_bitwriter_t;

static void bw_init(hevc_bitwriter_t *bw, uint8_t *buf, size_t cap)
{
    bw->buf      = buf;
    bw->cap      = cap;
    bw->byte_pos = 0U;
    bw->cur_byte = 0U;
    bw->bit_pos  = 0;
    bw->overflow = 0;
}

static void bw_put_bit(hevc_bitwriter_t *bw, uint32_t bit)
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

static void bw_put_bits(hevc_bitwriter_t *bw, uint32_t val, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        bw_put_bit(bw, (val >> i) & 1U);
    }
}

static void bw_put_ue(hevc_bitwriter_t *bw, uint32_t val)
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

static void bw_put_se(hevc_bitwriter_t *bw, int32_t val)
{
    uint32_t mapped = (val <= 0) ? (uint32_t)(-2 * val) : (uint32_t)(2 * val - 1);
    bw_put_ue(bw, mapped);
}

static void bw_rbsp_trailing(hevc_bitwriter_t *bw)
{
    bw_put_bit(bw, 1U);
    while (bw->bit_pos != 0 && !bw->overflow) {
        bw_put_bit(bw, 0U);
    }
}

/* Append Annex-B startcode + RBSP with emulation-prevention 0x03 escaping. */
static size_t emit_nal(uint8_t *dst, size_t cap,
                       uint8_t nal_type, const uint8_t *rbsp, size_t rbsp_len)
{
    if (!dst || cap < 6U || !rbsp) {
        return 0U;
    }
    size_t out = 0U;
    dst[out++] = 0x00U;
    dst[out++] = 0x00U;
    dst[out++] = 0x00U;
    dst[out++] = 0x01U;
    dst[out++] = (uint8_t)((nal_type & 0x3fU) << 1);
    dst[out++] = 0x01U; /* nuh_layer_id = 0, nuh_temporal_id_plus1 = 1 */

    int zero_cnt = 0;
    for (size_t i = 0U; i < rbsp_len; i++) {
        uint8_t b = rbsp[i];
        if (zero_cnt == 2 && b <= 0x03U) {
            if (out >= cap) {
                return 0U;
            }
            dst[out++] = 0x03U;
            zero_cnt = 0;
        }
        if (out >= cap) {
            return 0U;
        }
        dst[out++] = b;
        zero_cnt = (b == 0x00U) ? (zero_cnt + 1) : 0;
    }
    return out;
}

static uint8_t compute_level_idc(uint32_t coded_w, uint32_t coded_h,
                                   uint32_t fps_num, uint32_t fps_den)
{
    uint64_t luma_samples = (uint64_t)coded_w * (uint64_t)coded_h;
    uint32_t fn = (fps_num > 0U) ? fps_num : 60U;
    uint32_t fd = (fps_den > 0U) ? fps_den : 1U;
    uint64_t sample_rate = (luma_samples * (uint64_t)fn + (uint64_t)fd - 1ULL) / (uint64_t)fd;

    if (luma_samples <= 552960ULL && sample_rate <= 16588800ULL) {
        return 90U;  /* Level 3.0 */
    }
    if (luma_samples <= 983040ULL && sample_rate <= 33423360ULL) {
        return 93U;  /* Level 3.1 */
    }
    if (luma_samples <= 2228224ULL && sample_rate <= 66846720ULL) {
        return 120U; /* Level 4.0 */
    }
    if (luma_samples <= 2228224ULL && sample_rate <= 133693440ULL) {
        return 123U; /* Level 4.1 */
    }
    if (luma_samples <= 8912896ULL && sample_rate <= 267386880ULL) {
        return 150U; /* Level 5.0 */
    }
    if (luma_samples <= 8912896ULL && sample_rate <= 534773760ULL) {
        return 153U; /* Level 5.1 */
    }
    return 156U;     /* Level 5.2 */
}

static void write_profile_tier_level(hevc_bitwriter_t *bw,
                                     uint32_t chroma_format_idc,
                                     uint32_t bit_depth,
                                     uint32_t tier_flag,
                                     uint8_t level_idc)
{
    uint32_t profile_idc = 1U; /* Main */
    if (chroma_format_idc == 3U) {
        profile_idc = 4U;      /* Format Range Extensions (REXT) */
    } else if (bit_depth == 10U) {
        profile_idc = 2U;      /* Main 10 */
    }

    bw_put_bits(bw, 0U, 2);                   /* general_profile_space */
    bw_put_bit(bw, tier_flag ? 1U : 0U);      /* general_tier_flag */
    bw_put_bits(bw, profile_idc, 5);          /* general_profile_idc */

    uint32_t compat = 1U << (31U - profile_idc);
    bw_put_bits(bw, compat, 32);

    bw_put_bit(bw, 1U);               /* general_progressive_source_flag */
    bw_put_bit(bw, 0U);               /* general_interlaced_source_flag */
    bw_put_bit(bw, 0U);               /* general_non_packed_constraint_flag */
    bw_put_bit(bw, 1U);               /* general_frame_only_constraint_flag */

    if (profile_idc == 4U) {
        bw_put_bit(bw, 1U);                          /* max_12bit */
        bw_put_bit(bw, 1U);                          /* max_10bit */
        bw_put_bit(bw, (bit_depth == 8U) ? 1U : 0U); /* max_8bit */
        bw_put_bit(bw, 0U);                          /* max_422chroma */
        bw_put_bit(bw, 0U);                          /* max_420chroma */
        bw_put_bit(bw, 0U);                          /* max_monochrome */
        bw_put_bit(bw, 0U);                          /* intra */
        bw_put_bit(bw, 0U);                          /* one_picture_only */
        bw_put_bit(bw, 1U);                          /* lower_bit_rate */
        bw_put_bits(bw, 0U, 34);
        bw_put_bit(bw, 0U);                          /* general_inbld_flag */
    } else {
        bw_put_bits(bw, 0U, 32);
        bw_put_bits(bw, 0U, 11);
        bw_put_bit(bw, 0U);                          /* general_inbld_flag */
    }

    bw_put_bits(bw, level_idc, 8);
}

static size_t build_vps_rbsp(uint8_t *rbsp, size_t cap,
                             uint32_t chroma_format_idc, uint32_t bit_depth,
                             uint32_t tier_flag, uint8_t level_idc)
{
    hevc_bitwriter_t bw;
    bw_init(&bw, rbsp, cap);

    bw_put_bits(&bw, 0U, 4);      /* vps_video_parameter_set_id */
    bw_put_bit(&bw, 1U);          /* vps_base_layer_internal_flag */
    bw_put_bit(&bw, 1U);          /* vps_base_layer_available_flag */
    bw_put_bits(&bw, 0U, 6);      /* vps_max_layers_minus1 */
    bw_put_bits(&bw, 0U, 3);      /* vps_max_sub_layers_minus1 */
    bw_put_bit(&bw, 1U);          /* vps_temporal_id_nesting_flag */
    bw_put_bits(&bw, 0xffffU, 16);/* vps_reserved_0xffff_16bits */

    write_profile_tier_level(&bw, chroma_format_idc, bit_depth, tier_flag, level_idc);

    bw_put_bit(&bw, 1U);          /* vps_sub_layer_ordering_info_present_flag */
    bw_put_ue(&bw, 1U);           /* vps_max_dec_pic_buffering_minus1[0] */
    bw_put_ue(&bw, 0U);           /* vps_max_num_reorder_pics[0] */
    bw_put_ue(&bw, 0U);           /* vps_max_latency_increase_plus1[0] */
    bw_put_bits(&bw, 0U, 6);      /* vps_max_layer_id */
    bw_put_ue(&bw, 0U);           /* vps_num_layer_sets_minus1 */
    bw_put_bit(&bw, 0U);          /* vps_timing_info_present_flag */
    bw_put_bit(&bw, 0U);          /* vps_extension_flag */
    bw_rbsp_trailing(&bw);

    return bw.overflow ? 0U : bw.byte_pos;
}

static size_t build_sps_rbsp(uint8_t *rbsp, size_t cap,
                             const open_nvenc_hevc_hdr_params_t *p,
                             uint32_t chroma_format_idc, uint32_t bit_depth,
                             uint8_t level_idc)
{
    uint32_t width   = p->width;
    uint32_t height  = p->height;
    uint32_t coded_w = (width + 31U) & ~31U;
    uint32_t coded_h = (height + 31U) & ~31U;
    uint32_t sub_w = (chroma_format_idc == 1U) ? 2U : 1U;
    uint32_t sub_h = (chroma_format_idc == 1U) ? 2U : 1U;
    int has_crop = (coded_w != width || coded_h != height) ? 1 : 0;

    hevc_bitwriter_t bw;
    bw_init(&bw, rbsp, cap);

    bw_put_bits(&bw, 0U, 4);      /* sps_video_parameter_set_id */
    bw_put_bits(&bw, 0U, 3);      /* sps_max_sub_layers_minus1 */
    bw_put_bit(&bw, 1U);          /* sps_temporal_id_nesting_flag */

    write_profile_tier_level(&bw, chroma_format_idc, bit_depth, p->tier, level_idc);

    bw_put_ue(&bw, 0U);                   /* sps_seq_parameter_set_id */
    bw_put_ue(&bw, chroma_format_idc);    /* chroma_format_idc (1=4:2:0, 3=4:4:4) */
    if (chroma_format_idc == 3U) {
        bw_put_bit(&bw, 0U);              /* separate_colour_plane_flag */
    }
    bw_put_ue(&bw, coded_w);              /* pic_width_in_luma_samples */
    bw_put_ue(&bw, coded_h);              /* pic_height_in_luma_samples */

    bw_put_bit(&bw, has_crop ? 1U : 0U);  /* conformance_window_flag */
    if (has_crop) {
        bw_put_ue(&bw, 0U);
        bw_put_ue(&bw, (coded_w - width) / sub_w);
        bw_put_ue(&bw, 0U);
        bw_put_ue(&bw, (coded_h - height) / sub_h);
    }

    bw_put_ue(&bw, bit_depth - 8U);       /* bit_depth_luma_minus8 */
    bw_put_ue(&bw, bit_depth - 8U);       /* bit_depth_chroma_minus8 */
    bw_put_ue(&bw, 4U);                   /* log2_max_pic_order_cnt_lsb_minus4 */

    bw_put_bit(&bw, 1U);                  /* sps_sub_layer_ordering_info_present_flag */
    bw_put_ue(&bw, 1U);                   /* sps_max_dec_pic_buffering_minus1[0] */
    bw_put_ue(&bw, 0U);                   /* sps_max_num_reorder_pics[0] */
    bw_put_ue(&bw, 0U);                   /* sps_max_latency_increase_plus1[0] */

    /* GP104 NVENC HEVC CTU/TB geometry matching trace_headers on case_A..E */
    bw_put_ue(&bw, 1U);                   /* log2_min_luma_coding_block_size_minus3 (16x16) */
    bw_put_ue(&bw, 1U);                   /* log2_diff_max_min_luma_coding_block_size (32x32) */
    bw_put_ue(&bw, 0U);                   /* log2_min_luma_transform_block_size_minus2 (4x4) */
    bw_put_ue(&bw, 3U);                   /* log2_diff_max_min_luma_transform_block_size (32x32) */
    bw_put_ue(&bw, 3U);                   /* max_transform_hierarchy_depth_inter */
    bw_put_ue(&bw, 0U);                   /* max_transform_hierarchy_depth_intra */

    bw_put_bit(&bw, 0U);                  /* scaling_list_enabled_flag */
    bw_put_bit(&bw, 1U);                  /* amp_enabled_flag */
    bw_put_bit(&bw, 1U);                  /* sample_adaptive_offset_enabled_flag */
    bw_put_bit(&bw, 0U);                  /* pcm_enabled_flag */

    bw_put_ue(&bw, 1U);                   /* num_short_term_ref_pic_sets = 1 */
    bw_put_ue(&bw, 1U);                   /* num_negative_pics = 1 */
    bw_put_ue(&bw, 0U);                   /* num_positive_pics = 0 */
    bw_put_ue(&bw, 0U);                   /* delta_poc_s0_minus1[0] = 0 */
    bw_put_bit(&bw, 1U);                  /* used_by_curr_pic_s0_flag[0] = 1 */

    bw_put_bit(&bw, 0U);                  /* long_term_ref_pics_present_flag */
    bw_put_bit(&bw, 0U);                  /* sps_temporal_mvp_enabled_flag */
    bw_put_bit(&bw, 0U);                  /* strong_intra_smoothing_enabled_flag */

    int has_timing = (p->timing_present || (p->fps_num > 0U && p->fps_den > 0U)) ? 1 : 0;
    int has_vui = (p->video_signal_type_present || p->overscan_present ||
                   p->chroma_loc_present || has_timing) ? 1 : 0;
    bw_put_bit(&bw, has_vui ? 1U : 0U);   /* vui_parameters_present_flag */
    if (has_vui) {
        bw_put_bit(&bw, 0U);                                  /* aspect_ratio_info_present_flag */
        bw_put_bit(&bw, p->overscan_present ? 1U : 0U);       /* overscan_info_present_flag */
        if (p->overscan_present) {
            bw_put_bit(&bw, p->overscan_info ? 1U : 0U);      /* overscan_appropriate_flag */
        }
        bw_put_bit(&bw, p->video_signal_type_present ? 1U : 0U); /* video_signal_type_present_flag */
        if (p->video_signal_type_present) {
            bw_put_bits(&bw, p->video_format & 7U, 3);        /* video_format */
            bw_put_bit(&bw, p->video_full_range ? 1U : 0U);   /* video_full_range_flag */
            bw_put_bit(&bw, p->colour_desc_present ? 1U : 0U);/* colour_description_present_flag */
            if (p->colour_desc_present) {
                bw_put_bits(&bw, p->colour_primaries & 0xffU, 8);
                bw_put_bits(&bw, p->transfer_characteristics & 0xffU, 8);
                bw_put_bits(&bw, p->matrix_coeffs & 0xffU, 8);
            }
        }
        bw_put_bit(&bw, p->chroma_loc_present ? 1U : 0U);     /* chroma_loc_info_present_flag */
        if (p->chroma_loc_present) {
            bw_put_ue(&bw, p->chroma_loc_top);
            bw_put_ue(&bw, p->chroma_loc_bot);
        }
        bw_put_bit(&bw, 0U);                                  /* neutral_chroma_indication_flag */
        bw_put_bit(&bw, 0U);                                  /* field_seq_flag */
        bw_put_bit(&bw, 0U);                                  /* frame_field_info_present_flag */
        bw_put_bit(&bw, 0U);                                  /* default_display_window_flag */
        bw_put_bit(&bw, has_timing ? 1U : 0U);                /* vui_timing_info_present_flag */
        if (has_timing) {
            uint32_t ticks = (p->timing_present && p->num_units_in_tick > 0U)
                                 ? p->num_units_in_tick
                                 : (p->fps_den ? p->fps_den : 1U);
            uint32_t scale = (p->timing_present && p->time_scale > 0U)
                                 ? p->time_scale
                                 : (p->fps_num ? p->fps_num : 60U);
            bw_put_bits(&bw, ticks, 32);                      /* vui_num_units_in_tick */
            bw_put_bits(&bw, scale, 32);                      /* vui_time_scale */
            bw_put_bit(&bw, 0U);                              /* vui_poc_proportional_to_timing_flag */
            bw_put_bit(&bw, 0U);                              /* vui_hrd_parameters_present_flag */
        }
        bw_put_bit(&bw, 0U);                                  /* bitstream_restriction_flag */
    }

    bw_put_bit(&bw, 0U);                  /* sps_extension_present_flag */
    bw_rbsp_trailing(&bw);

    return bw.overflow ? 0U : bw.byte_pos;
}

static size_t build_pps_rbsp(uint8_t *rbsp, size_t cap,
                             uint32_t chroma_format_idc, uint32_t bit_depth)
{
    int32_t chroma_qp_off = (chroma_format_idc == 3U) ? 6 : 0;

    hevc_bitwriter_t bw;
    bw_init(&bw, rbsp, cap);

    bw_put_ue(&bw, 0U);                   /* pps_pic_parameter_set_id */
    bw_put_ue(&bw, 0U);                   /* pps_seq_parameter_set_id */
    bw_put_bit(&bw, 0U);                  /* dependent_slice_segments_enabled_flag */
    bw_put_bit(&bw, 0U);                  /* output_flag_present_flag */
    bw_put_bits(&bw, 0U, 3);              /* num_extra_slice_header_bits */
    bw_put_bit(&bw, 0U);                  /* sign_data_hiding_enabled_flag */
    bw_put_bit(&bw, 1U);                  /* cabac_init_present_flag */
    bw_put_ue(&bw, 0U);                   /* num_ref_idx_l0_default_active_minus1 */
    bw_put_ue(&bw, 0U);                   /* num_ref_idx_l1_default_active_minus1 */
    bw_put_se(&bw, 0);                    /* init_qp_minus26 */
    bw_put_bit(&bw, 0U);                  /* constrained_intra_pred_flag */
    bw_put_bit(&bw, (bit_depth == 8U) ? 1U : 0U); /* transform_skip_enabled_flag */
    bw_put_bit(&bw, 1U);                  /* cu_qp_delta_enabled_flag */
    bw_put_ue(&bw, 0U);                   /* diff_cu_qp_delta_depth */
    bw_put_se(&bw, chroma_qp_off);        /* pps_cb_qp_offset */
    bw_put_se(&bw, chroma_qp_off);        /* pps_cr_qp_offset */
    bw_put_bit(&bw, 0U);                  /* pps_slice_chroma_qp_offsets_present_flag */
    bw_put_bit(&bw, 0U);                  /* weighted_pred_flag */
    bw_put_bit(&bw, 0U);                  /* weighted_bipred_flag */
    bw_put_bit(&bw, 0U);                  /* transquant_bypass_enabled_flag */
    bw_put_bit(&bw, 0U);                  /* tiles_enabled_flag */
    bw_put_bit(&bw, 0U);                  /* entropy_coding_sync_enabled_flag */
    bw_put_bit(&bw, 1U);                  /* pps_loop_filter_across_slices_enabled_flag */
    bw_put_bit(&bw, 1U);                  /* deblocking_filter_control_present_flag */
    bw_put_bit(&bw, 0U);                  /* deblocking_filter_override_enabled_flag */
    bw_put_bit(&bw, 0U);                  /* pps_deblocking_filter_disabled_flag */
    bw_put_se(&bw, 0);                    /* pps_beta_offset_div2 */
    bw_put_se(&bw, 0);                    /* pps_tc_offset_div2 */
    bw_put_bit(&bw, 0U);                  /* pps_scaling_list_data_present_flag */
    bw_put_bit(&bw, 0U);                  /* lists_modification_present_flag */
    bw_put_ue(&bw, 0U);                   /* log2_parallel_merge_level_minus2 */
    bw_put_bit(&bw, 0U);                  /* slice_segment_header_extension_present_flag */
    bw_put_bit(&bw, 0U);                  /* pps_extension_present_flag */
    bw_rbsp_trailing(&bw);

    return bw.overflow ? 0U : bw.byte_pos;
}

size_t open_nvenc_hevc_write_headers_ex(uint8_t *dst, size_t cap,
                                        const open_nvenc_hevc_hdr_params_t *p)
{
    if (!dst || !p || cap < 64U || p->width < 16U || p->height < 16U) {
        return 0U;
    }
    uint32_t chroma_format_idc = (p->chroma_format_idc == 3U) ? 3U : 1U;
    uint32_t bit_depth         = (p->bit_depth == 10U) ? 10U : 8U;

    uint32_t coded_w = (p->width + 31U) & ~31U;
    uint32_t coded_h = (p->height + 31U) & ~31U;
    uint8_t auto_lvl = compute_level_idc(coded_w, coded_h, p->fps_num, p->fps_den);
    uint8_t level_idc = auto_lvl;
    if (p->level_idc >= 30U && p->level_idc <= 186U) {
        level_idc = (uint8_t)p->level_idc;
        if (auto_lvl > level_idc) {
            level_idc = auto_lvl;
        }
    }

    uint8_t rbsp[256];
    size_t off = 0U;

    size_t vps_rbsp_len = build_vps_rbsp(rbsp, sizeof(rbsp), chroma_format_idc, bit_depth,
                                         p->tier, level_idc);
    if (vps_rbsp_len == 0U) {
        return 0U;
    }
    size_t n = emit_nal(dst + off, cap - off, 32U, rbsp, vps_rbsp_len);
    if (n == 0U) {
        return 0U;
    }
    off += n;

    size_t sps_rbsp_len = build_sps_rbsp(rbsp, sizeof(rbsp), p, chroma_format_idc, bit_depth, level_idc);
    if (sps_rbsp_len == 0U) {
        return 0U;
    }
    n = emit_nal(dst + off, cap - off, 33U, rbsp, sps_rbsp_len);
    if (n == 0U) {
        return 0U;
    }
    off += n;

    size_t pps_rbsp_len = build_pps_rbsp(rbsp, sizeof(rbsp), chroma_format_idc, bit_depth);
    if (pps_rbsp_len == 0U) {
        return 0U;
    }
    n = emit_nal(dst + off, cap - off, 34U, rbsp, pps_rbsp_len);
    if (n == 0U) {
        return 0U;
    }
    off += n;

    return off;
}

size_t open_nvenc_hevc_write_headers(uint8_t *dst, size_t cap,
                                     uint32_t width, uint32_t height,
                                     uint32_t chroma_format_idc,
                                     uint32_t bit_depth)
{
    open_nvenc_hevc_hdr_params_t p;
    memset(&p, 0, sizeof(p));
    p.width             = width;
    p.height            = height;
    p.chroma_format_idc = chroma_format_idc;
    p.bit_depth         = bit_depth;
    p.fps_num           = 60U;
    p.fps_den           = 1U;
    return open_nvenc_hevc_write_headers_ex(dst, cap, &p);
}
