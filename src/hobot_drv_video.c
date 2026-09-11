/*
 * D-Robotics RDK-X5 VPU VA-API Driver (libva-hobot)
 *
 * Notice: This code was developed and optimized by AI (Google DeepMind Antigravity / Gemini)
 * in collaboration with LeunoP for the D-Robotics RDK-X5 platform.
 * (해당 코드는 AI에 의해 작성 및 최적화되었습니다.)
 *
 * Licensed under the MIT License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>
#include <va/va_enc_jpeg.h>
#include <va/va_dec_hevc.h>
#include <va/va_dec_jpeg.h>
#include <hb_media_codec.h>
#include <hb_media_error.h>
#include <hb_mem_mgr.h>

#define HOBOT_VA_DRIVER_STR "D-Robotics RDK-X5 VPU VA-API Driver 0.4.0 (Decode + Encode)"
#define MAX_SURFACES 512
#define MAX_BUFFERS  4096
#define MAX_CONTEXTS 16
#define MAX_CONFIGS  64
#define MAX_IMAGES   512

static VAProfile supported_profiles[] = {
    VAProfileH264ConstrainedBaseline,
    VAProfileH264Main,
    VAProfileH264High,
    /* HEVC/H265 removed: VPU decoding produces corrupted output */
    VAProfileJPEGBaseline
};

#define NUM_SUPPORTED_PROFILES (sizeof(supported_profiles) / sizeof(supported_profiles[0]))

/* H.264 Bitstream Synthesizer (for Decoder SPS/PPS synthesis) */
typedef struct {
    uint8_t *buf;
    int bit_pos;
} BitWriter;

static void bw_put_bit(BitWriter *bw, int bit) {
    int byte_idx = bw->bit_pos / 8;
    int bit_idx = 7 - (bw->bit_pos % 8);
    if (bit)
        bw->buf[byte_idx] |= (1 << bit_idx);
    else
        bw->buf[byte_idx] &= ~(1 << bit_idx);
    bw->bit_pos++;
}

static void bw_put_bits(BitWriter *bw, uint32_t val, int n) {
    for (int i = n - 1; i >= 0; i--) {
        bw_put_bit(bw, (val >> i) & 1);
    }
}

static void bw_put_ue(BitWriter *bw, uint32_t val) {
    uint32_t temp = val + 1;
    int leading_zeros = 0;
    while ((temp >> (leading_zeros + 1)) != 0) {
        leading_zeros++;
    }
    for (int i = 0; i < leading_zeros; i++) bw_put_bit(bw, 0);
    bw_put_bit(bw, 1);
    for (int i = leading_zeros - 1; i >= 0; i--) {
        bw_put_bit(bw, (temp >> i) & 1);
    }
}

static void bw_put_se(BitWriter *bw, int32_t val) {
    uint32_t ue = (val <= 0) ? (-val * 2) : (val * 2 - 1);
    bw_put_ue(bw, ue);
}

static int add_emulation_prevention(const uint8_t *src, int src_len, uint8_t *dst, int dst_max) {
    int dst_idx = 0;
    int zero_count = 0;
    for (int i = 0; i < src_len && dst_idx < dst_max; i++) {
        uint8_t b = src[i];
        if (zero_count == 2 && b <= 3) {
            dst[dst_idx++] = 0x03;
            zero_count = 0;
        }
        dst[dst_idx++] = b;
        if (b == 0) zero_count++;
        else zero_count = 0;
    }
    return dst_idx;
}

static int generate_h264_sps(VAPictureParameterBufferH264 *pic, uint8_t *out, int max_len) {
    uint8_t rbsp[256] = {0};
    BitWriter bw = {rbsp, 0};

    bw_put_bits(&bw, 100, 8);  // High Profile
    bw_put_bits(&bw, 0, 8);    // constraint flags
    bw_put_bits(&bw, 51, 8);   // Level 5.1 (supports up to 4K / 1080p 120fps)
    bw_put_ue(&bw, 0);         // seq_parameter_set_id

    bw_put_ue(&bw, pic->seq_fields.bits.chroma_format_idc ? pic->seq_fields.bits.chroma_format_idc : 1);
    bw_put_ue(&bw, pic->bit_depth_luma_minus8);
    bw_put_ue(&bw, pic->bit_depth_chroma_minus8);
    bw_put_bits(&bw, 0, 1);    // qpprime_y_zero_transform_bypass_flag
    bw_put_bits(&bw, 0, 1);    // seq_scaling_matrix_present_flag

    bw_put_ue(&bw, pic->seq_fields.bits.log2_max_frame_num_minus4);
    bw_put_ue(&bw, pic->seq_fields.bits.pic_order_cnt_type);
    if (pic->seq_fields.bits.pic_order_cnt_type == 0) {
        bw_put_ue(&bw, pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
    }
    bw_put_ue(&bw, pic->num_ref_frames > 0 ? pic->num_ref_frames : 2);
    bw_put_bits(&bw, pic->seq_fields.bits.gaps_in_frame_num_value_allowed_flag, 1);

    bw_put_ue(&bw, pic->picture_width_in_mbs_minus1);
    bw_put_ue(&bw, pic->picture_height_in_mbs_minus1);
    bw_put_bits(&bw, pic->seq_fields.bits.frame_mbs_only_flag, 1);
    if (!pic->seq_fields.bits.frame_mbs_only_flag) {
        bw_put_bits(&bw, pic->seq_fields.bits.mb_adaptive_frame_field_flag, 1);
    }
    bw_put_bits(&bw, pic->seq_fields.bits.direct_8x8_inference_flag, 1);
    bw_put_bits(&bw, 0, 1);    // frame_cropping_flag
    bw_put_bits(&bw, 0, 1);    // vui_parameters_present_flag
    bw_put_bits(&bw, 1, 1);    // rbsp_stop_one_bit

    out[0] = 0x67; // NAL header SPS (forbidden_zero=0, nal_ref_idc=3, nal_unit_type=7)
    int rbsp_len = (bw.bit_pos + 7) / 8;
    return 1 + add_emulation_prevention(rbsp, rbsp_len, out + 1, max_len - 1);
}

static int generate_h264_pps(VAPictureParameterBufferH264 *pic, uint8_t *out, int max_len) {
    uint8_t rbsp[256] = {0};
    BitWriter bw = {rbsp, 0};

    bw_put_ue(&bw, 0);         // pic_parameter_set_id
    bw_put_ue(&bw, 0);         // seq_parameter_set_id
    bw_put_bits(&bw, pic->pic_fields.bits.entropy_coding_mode_flag, 1);
    bw_put_bits(&bw, pic->pic_fields.bits.pic_order_present_flag, 1);
    bw_put_ue(&bw, 0);         // num_slice_groups_minus1
    bw_put_ue(&bw, 0);         // num_ref_idx_l0_default_active_minus1
    bw_put_ue(&bw, 0);         // num_ref_idx_l1_default_active_minus1
    bw_put_bits(&bw, pic->pic_fields.bits.weighted_pred_flag, 1);
    bw_put_bits(&bw, pic->pic_fields.bits.weighted_bipred_idc, 2);
    bw_put_se(&bw, pic->pic_init_qp_minus26);
    bw_put_se(&bw, pic->pic_init_qs_minus26);
    bw_put_se(&bw, pic->chroma_qp_index_offset);
    bw_put_bits(&bw, pic->pic_fields.bits.deblocking_filter_control_present_flag, 1);
    bw_put_bits(&bw, pic->pic_fields.bits.constrained_intra_pred_flag, 1);
    bw_put_bits(&bw, 0, 1);    // redundant_pic_cnt_present_flag
    bw_put_bits(&bw, pic->pic_fields.bits.transform_8x8_mode_flag, 1);
    bw_put_bits(&bw, 0, 1);    // pic_scaling_matrix_present_flag
    bw_put_se(&bw, pic->second_chroma_qp_index_offset);
    bw_put_bits(&bw, 1, 1);    // rbsp_stop_one_bit

    out[0] = 0x68; // NAL header PPS (forbidden_zero=0, nal_ref_idc=3, nal_unit_type=8)
    int rbsp_len = (bw.bit_pos + 7) / 8;
    return 1 + add_emulation_prevention(rbsp, rbsp_len, out + 1, max_len - 1);
}

static int generate_hevc_vps(int profile_idc, uint8_t *out, int max_len) {
    uint8_t rbsp[128] = {0};
    BitWriter bw = {rbsp, 0};

    bw_put_bits(&bw, 0x4001, 16); // NAL header VPS (type 32)
    bw_put_bits(&bw, 0, 4);       // vps_video_parameter_set_id
    bw_put_bit(&bw, 1);           // vps_base_layer_internal_flag
    bw_put_bit(&bw, 1);           // vps_base_layer_available_flag
    bw_put_bits(&bw, 0, 6);       // vps_max_layers_minus1
    bw_put_bits(&bw, 0, 3);       // vps_max_sub_layers_minus1
    bw_put_bit(&bw, 1);           // vps_temporal_id_nesting_flag
    bw_put_bits(&bw, 0xffff, 16); // reserved

    // profile_tier_level
    bw_put_bits(&bw, 0, 2);       // general_profile_space
    bw_put_bit(&bw, 0);           // general_tier_flag
    bw_put_bits(&bw, profile_idc, 5); // general_profile_idc (1=Main, 2=Main10)
    bw_put_bits(&bw, (profile_idc == 1) ? 0x60000000 : 0x20000000, 32); // compatibility flags
    bw_put_bit(&bw, 1);           // progressive_source
    bw_put_bit(&bw, 0);           // interlaced_source
    bw_put_bit(&bw, 1);           // non_packed_constraint
    bw_put_bit(&bw, 1);           // frame_only_constraint
    for (int i = 0; i < 44; i++) bw_put_bit(&bw, 0); // reserved
    bw_put_bits(&bw, 123, 8);     // general_level_idc (4.1)

    bw_put_bit(&bw, 0);           // vps_sub_layer_ordering_info_present_flag
    bw_put_ue(&bw, 4);            // vps_max_dec_pic_buffering_minus1
    bw_put_ue(&bw, 2);            // vps_max_num_reorder_pics
    bw_put_ue(&bw, 0);            // vps_max_latency_increase_plus1
    bw_put_bits(&bw, 0, 6);       // vps_max_layer_id
    bw_put_ue(&bw, 0);            // vps_num_layer_sets_minus1
    bw_put_bit(&bw, 0);           // vps_timing_info_present_flag
    bw_put_bit(&bw, 0);           // vps_extension_flag
    bw_put_bit(&bw, 1);           // rbsp_trailing_bits

    int rbsp_len = (bw.bit_pos + 7) / 8;
    return add_emulation_prevention(rbsp, rbsp_len, out, max_len);
}

static int generate_hevc_sps(VAPictureParameterBufferHEVC *pic, int profile_idc, uint8_t *out, int max_len) {
    uint8_t rbsp[256] = {0};
    BitWriter bw = {rbsp, 0};

    bw_put_bits(&bw, 0x4201, 16); // NAL header SPS (type 33)
    bw_put_bits(&bw, 0, 4);       // sps_video_parameter_set_id
    bw_put_bits(&bw, 0, 3);       // sps_max_sub_layers_minus1
    bw_put_bit(&bw, 1);           // sps_temporal_id_nesting_flag

    // profile_tier_level
    bw_put_bits(&bw, 0, 2);       // general_profile_space
    bw_put_bit(&bw, 0);           // general_tier_flag
    bw_put_bits(&bw, profile_idc, 5); // general_profile_idc
    bw_put_bits(&bw, (profile_idc == 1) ? 0x60000000 : 0x20000000, 32);
    bw_put_bit(&bw, 1);           // progressive_source
    bw_put_bit(&bw, 0);           // interlaced_source
    bw_put_bit(&bw, 1);           // non_packed_constraint
    bw_put_bit(&bw, 1);           // frame_only_constraint
    for (int i = 0; i < 44; i++) bw_put_bit(&bw, 0);
    bw_put_bits(&bw, 123, 8);     // general_level_idc (4.1)

    bw_put_ue(&bw, 0);            // sps_seq_parameter_set_id
    bw_put_ue(&bw, pic->pic_fields.bits.chroma_format_idc ? pic->pic_fields.bits.chroma_format_idc : 1);
    bw_put_ue(&bw, pic->pic_width_in_luma_samples ? pic->pic_width_in_luma_samples : 1920);
    bw_put_ue(&bw, pic->pic_height_in_luma_samples ? pic->pic_height_in_luma_samples : 1080);
    bw_put_bit(&bw, 0);           // conformance_window_flag
    bw_put_ue(&bw, pic->bit_depth_luma_minus8);
    bw_put_ue(&bw, pic->bit_depth_chroma_minus8);
    bw_put_ue(&bw, pic->log2_max_pic_order_cnt_lsb_minus4 > 0 ? pic->log2_max_pic_order_cnt_lsb_minus4 : 4);
    bw_put_bit(&bw, 0);           // sps_sub_layer_ordering_info_present_flag
    bw_put_ue(&bw, pic->sps_max_dec_pic_buffering_minus1 > 0 ? pic->sps_max_dec_pic_buffering_minus1 : 4);
    bw_put_ue(&bw, 2);            // sps_max_num_reorder_pics
    bw_put_ue(&bw, 0);            // sps_max_latency_increase_plus1

    bw_put_ue(&bw, pic->log2_min_luma_coding_block_size_minus3);
    bw_put_ue(&bw, pic->log2_diff_max_min_luma_coding_block_size > 0 ? pic->log2_diff_max_min_luma_coding_block_size : 2);
    bw_put_ue(&bw, pic->log2_min_transform_block_size_minus2);
    bw_put_ue(&bw, pic->log2_diff_max_min_transform_block_size > 0 ? pic->log2_diff_max_min_transform_block_size : 3);
    bw_put_ue(&bw, pic->max_transform_hierarchy_depth_inter > 0 ? pic->max_transform_hierarchy_depth_inter : 2);
    bw_put_ue(&bw, pic->max_transform_hierarchy_depth_intra > 0 ? pic->max_transform_hierarchy_depth_intra : 2);

    bw_put_bit(&bw, pic->pic_fields.bits.scaling_list_enabled_flag);
    bw_put_bit(&bw, pic->pic_fields.bits.amp_enabled_flag);
    bw_put_bit(&bw, pic->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag);
    bw_put_bit(&bw, pic->pic_fields.bits.pcm_enabled_flag);

    bw_put_ue(&bw, 0);            // num_short_term_ref_pic_sets
    bw_put_bit(&bw, pic->slice_parsing_fields.bits.long_term_ref_pics_present_flag);
    bw_put_bit(&bw, pic->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag);
    bw_put_bit(&bw, pic->pic_fields.bits.strong_intra_smoothing_enabled_flag);
    bw_put_bit(&bw, 0);           // vui_parameters_present_flag
    bw_put_bit(&bw, 0);           // sps_extension_present_flag
    bw_put_bit(&bw, 1);           // rbsp_trailing_bits

    int rbsp_len = (bw.bit_pos + 7) / 8;
    return add_emulation_prevention(rbsp, rbsp_len, out, max_len);
}

static int generate_hevc_pps(VAPictureParameterBufferHEVC *pic, uint8_t *out, int max_len) {
    uint8_t rbsp[128] = {0};
    BitWriter bw = {rbsp, 0};

    bw_put_bits(&bw, 0x4401, 16); // NAL header PPS (type 34)
    bw_put_ue(&bw, 0);            // pps_pic_parameter_set_id
    bw_put_ue(&bw, 0);            // pps_seq_parameter_set_id
    bw_put_bit(&bw, pic->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag);
    bw_put_bit(&bw, pic->slice_parsing_fields.bits.output_flag_present_flag);
    bw_put_bits(&bw, pic->num_extra_slice_header_bits, 3);
    bw_put_bit(&bw, pic->pic_fields.bits.sign_data_hiding_enabled_flag);
    bw_put_bit(&bw, pic->slice_parsing_fields.bits.cabac_init_present_flag);
    bw_put_ue(&bw, pic->num_ref_idx_l0_default_active_minus1);
    bw_put_ue(&bw, pic->num_ref_idx_l1_default_active_minus1);
    bw_put_se(&bw, pic->init_qp_minus26);
    bw_put_bit(&bw, pic->pic_fields.bits.constrained_intra_pred_flag);
    bw_put_bit(&bw, pic->pic_fields.bits.transform_skip_enabled_flag);
    bw_put_bit(&bw, pic->pic_fields.bits.cu_qp_delta_enabled_flag);
    if (pic->pic_fields.bits.cu_qp_delta_enabled_flag) {
        bw_put_ue(&bw, pic->diff_cu_qp_delta_depth);
    }
    bw_put_se(&bw, pic->pps_cb_qp_offset);
    bw_put_se(&bw, pic->pps_cr_qp_offset);
    bw_put_bit(&bw, pic->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag);
    bw_put_bit(&bw, pic->pic_fields.bits.weighted_pred_flag);
    bw_put_bit(&bw, pic->pic_fields.bits.weighted_bipred_flag);
    bw_put_bit(&bw, pic->pic_fields.bits.transquant_bypass_enabled_flag);
    bw_put_bit(&bw, pic->pic_fields.bits.tiles_enabled_flag);
    bw_put_bit(&bw, pic->pic_fields.bits.entropy_coding_sync_enabled_flag);
    bw_put_bit(&bw, pic->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag);
    bw_put_bit(&bw, 1);           // deblocking_filter_control_present_flag
    bw_put_bit(&bw, pic->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag);
    bw_put_bit(&bw, pic->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag);
    if (!pic->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag) {
        bw_put_se(&bw, pic->pps_beta_offset_div2);
        bw_put_se(&bw, pic->pps_tc_offset_div2);
    }
    bw_put_bit(&bw, 0);           // pps_scaling_list_data_present_flag
    bw_put_bit(&bw, pic->slice_parsing_fields.bits.lists_modification_present_flag);
    bw_put_ue(&bw, pic->log2_parallel_merge_level_minus2);
    bw_put_bit(&bw, pic->slice_parsing_fields.bits.slice_segment_header_extension_present_flag);
    bw_put_bit(&bw, 0);           // pps_extension_present_flag
    bw_put_bit(&bw, 1);           // rbsp_trailing_bits

    int rbsp_len = (bw.bit_pos + 7) / 8;
    return add_emulation_prevention(rbsp, rbsp_len, out, max_len);
}

/* Config Object */
typedef struct {
    VAConfigID id;
    int allocated;
    VAProfile profile;
    VAEntrypoint entrypoint;
    unsigned int rate_control;
    unsigned int rt_format;
} HobotConfig;

/* Internal Surface Object */
typedef struct {
    VASurfaceID id;
    int allocated;
    unsigned int width;
    unsigned int height;
    unsigned int format;
    int dma_fd;
    int stride;
    media_codec_buffer_t vpu_out_buf;
    int has_decoded_frame;
    void *raw_data;
    uint32_t raw_data_size;
    VAContextID context_id;
} HobotSurface;

/* Internal Buffer Object */
typedef struct {
    VABufferID id;
    int allocated;
    int is_derived;
    VABufferType type;
    unsigned int size;
    void *data;
    VACodedBufferSegment coded_segment;
} HobotBuffer;

/* Internal Context Object */
typedef struct {
    VAContextID id;
    int allocated;
    int is_encoder;
    VAProfile profile;
    int width;
    int height;
    media_codec_context_t vpu_ctx;
    int vpu_running;
    VASurfaceID current_render_target;
    VABufferID enc_coded_buf;
    int headers_sent;
    uint8_t cached_vps[128];
    int cached_vps_len;
    uint8_t cached_sps[128];
    int cached_sps_len;
    uint8_t cached_pps[128];
    int cached_pps_len;
    uint64_t frame_count;
    VASurfaceID submitted_surfaces[128];
    uint32_t sub_head;
    uint32_t sub_tail;
    media_codec_buffer_t dec_in_buf;
    int dec_in_buf_valid;
    int dec_in_buf_offset;
} HobotContext;

/* Internal Image Object */
typedef struct {
    VAImageID id;
    int allocated;
    VAImage image;
    VABufferID buf_id;
    VASurfaceID surface_id;
} HobotImage;

/* Main Driver Private Data */
typedef struct {
    int initialized;
    HobotConfig  configs[MAX_CONFIGS];
    HobotSurface surfaces[MAX_SURFACES];
    HobotBuffer  buffers[MAX_BUFFERS];
    HobotContext contexts[MAX_CONTEXTS];
    HobotImage   images[MAX_IMAGES];
} HobotDriverData;

static VAStatus hobot_vaTerminate(VADriverContextP ctx) {
    if (!ctx) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (ctx->pDriverData) {
        HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
        for (int i = 0; i < MAX_CONTEXTS; i++) {
            if (drv->contexts[i].allocated && drv->contexts[i].vpu_running) {
                hb_mm_mc_stop(&drv->contexts[i].vpu_ctx);
                hb_mm_mc_release(&drv->contexts[i].vpu_ctx);
                drv->contexts[i].vpu_running = 0;
            }
        }
        for (int i = 0; i < MAX_SURFACES; i++) {
            if (drv->surfaces[i].allocated && drv->surfaces[i].raw_data) {
                free(drv->surfaces[i].raw_data);
                drv->surfaces[i].raw_data = NULL;
            }
        }
        for (int i = 0; i < MAX_BUFFERS; i++) {
            if (drv->buffers[i].allocated && !drv->buffers[i].is_derived && drv->buffers[i].data) {
                free(drv->buffers[i].data);
                drv->buffers[i].data = NULL;
            }
        }
        free(drv);
        ctx->pDriverData = NULL;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaQueryConfigProfiles(
    VADriverContextP ctx,
    VAProfile *profile_list,
    int *num_profiles
) {
    if (!num_profiles) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!profile_list) {
        *num_profiles = NUM_SUPPORTED_PROFILES;
        return VA_STATUS_SUCCESS;
    }

    int count = 0;
    for (size_t i = 0; i < NUM_SUPPORTED_PROFILES; i++) {
        profile_list[count++] = supported_profiles[i];
    }
    *num_profiles = count;
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaQueryConfigEntrypoints(
    VADriverContextP ctx,
    VAProfile profile,
    VAEntrypoint *entrypoint_list,
    int *num_entrypoints
) {
    if (!num_entrypoints) return VA_STATUS_ERROR_INVALID_PARAMETER;
    
    int valid = 0;
    for (size_t i = 0; i < NUM_SUPPORTED_PROFILES; i++) {
        if (supported_profiles[i] == profile) {
            valid = 1;
            break;
        }
    }
    if (!valid) {
        *num_entrypoints = 0;
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    if (!entrypoint_list) {
        *num_entrypoints = 2;
        return VA_STATUS_SUCCESS;
    }

    entrypoint_list[0] = VAEntrypointVLD;
    if (profile == VAProfileJPEGBaseline) {
        entrypoint_list[1] = VAEntrypointEncPicture;
    } else {
        entrypoint_list[1] = VAEntrypointEncSlice;
    }
    *num_entrypoints = 2;
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaGetConfigAttributes(
    VADriverContextP ctx,
    VAProfile profile,
    VAEntrypoint entrypoint,
    VAConfigAttrib *attrib_list,
    int num_attribs
) {
    if (!attrib_list) return VA_STATUS_ERROR_INVALID_PARAMETER;

    for (int i = 0; i < num_attribs; i++) {
        switch (attrib_list[i].type) {
        case VAConfigAttribRTFormat:
            if (profile == VAProfileHEVCMain10) {
                attrib_list[i].value = VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10;
            } else {
                attrib_list[i].value = VA_RT_FORMAT_YUV420;
            }
            break;
        case VAConfigAttribRateControl:
            attrib_list[i].value = VA_RC_CBR | VA_RC_VBR | VA_RC_CQP;
            break;
        case VAConfigAttribEncPackedHeaders:
            attrib_list[i].value = VA_ENC_PACKED_HEADER_NONE;
            break;
        case VAConfigAttribEncMaxRefFrames:
            attrib_list[i].value = 1;
            break;
        case VAConfigAttribMaxPictureWidth:
            attrib_list[i].value = 4096;
            break;
        case VAConfigAttribMaxPictureHeight:
            attrib_list[i].value = 4096;
            break;
        case VAConfigAttribEncSliceStructure:
            attrib_list[i].value = VA_ENC_SLICE_STRUCTURE_POWER_OF_TWO_ROWS |
                                   VA_ENC_SLICE_STRUCTURE_EQUAL_ROWS |
                                   VA_ENC_SLICE_STRUCTURE_ARBITRARY_MACROBLOCKS;
            break;
        case VAConfigAttribEncQualityRange:
            attrib_list[i].value = 1;
            break;
        case VAConfigAttribEncInterlaced:
            attrib_list[i].value = VA_ENC_INTERLACED_NONE;
            break;
        case VAConfigAttribEncQuantization:
            attrib_list[i].value = VA_ENC_QUANTIZATION_NONE;
            break;
        case VAConfigAttribEncIntraRefresh:
            attrib_list[i].value = VA_ENC_INTRA_REFRESH_NONE;
            break;
        default:
            attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        }
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaCreateConfig(
    VADriverContextP ctx,
    VAProfile profile,
    VAEntrypoint entrypoint,
    VAConfigAttrib *attrib_list,
    int num_attribs,
    VAConfigID *config_id
) {
    if (!config_id) return VA_STATUS_ERROR_INVALID_PARAMETER;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;

    int is_vld = (entrypoint == VAEntrypointVLD);
    int is_enc = (entrypoint == VAEntrypointEncSlice || entrypoint == VAEntrypointEncPicture);
    if (!is_vld && !is_enc) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }

    for (int i = 1; i < MAX_CONFIGS; i++) {
        if (!drv->configs[i].allocated) {
            drv->configs[i].allocated = 1;
            drv->configs[i].id = (VAConfigID)i;
            drv->configs[i].profile = profile;
            drv->configs[i].entrypoint = entrypoint;
            drv->configs[i].rate_control = VA_RC_CBR;
            drv->configs[i].rt_format = VA_RT_FORMAT_YUV420;

            if (attrib_list) {
                for (int a = 0; a < num_attribs; a++) {
                    if (attrib_list[a].type == VAConfigAttribRateControl) {
                        drv->configs[i].rate_control = attrib_list[a].value;
                    } else if (attrib_list[a].type == VAConfigAttribRTFormat) {
                        drv->configs[i].rt_format = attrib_list[a].value;
                    }
                }
            }
            *config_id = (VAConfigID)i;
            return VA_STATUS_SUCCESS;
        }
    }
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

static VAStatus hobot_vaDestroyConfig(VADriverContextP ctx, VAConfigID config_id) {
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (config_id > 0 && config_id < MAX_CONFIGS) {
        drv->configs[config_id].allocated = 0;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaQueryConfigAttributes(
    VADriverContextP ctx,
    VAConfigID config_id,
    VAProfile *profile,
    VAEntrypoint *entrypoint,
    VAConfigAttrib *attrib_list,
    int *num_attribs
) {
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (config_id <= 0 || config_id >= MAX_CONFIGS || !drv->configs[config_id].allocated) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    HobotConfig *cfg = &drv->configs[config_id];
    if (profile) *profile = cfg->profile;
    if (entrypoint) *entrypoint = cfg->entrypoint;
    if (attrib_list && num_attribs && *num_attribs > 0) {
        return hobot_vaGetConfigAttributes(ctx, cfg->profile, cfg->entrypoint, attrib_list, *num_attribs);
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaQuerySurfaceAttributes(
    VADriverContextP ctx,
    VAConfigID config_id,
    VASurfaceAttrib *attrib_list,
    unsigned int *num_attribs
) {
    if (!num_attribs) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!attrib_list) {
        *num_attribs = 6;
        return VA_STATUS_SUCCESS;
    }

    int idx = 0;
    attrib_list[idx].type = VASurfaceAttribPixelFormat;
    attrib_list[idx].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
    attrib_list[idx].value.type = VAGenericValueTypeInteger;
    attrib_list[idx].value.value.i = VA_FOURCC_NV12;
    idx++;

    attrib_list[idx].type = VASurfaceAttribMemoryType;
    attrib_list[idx].flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE;
    attrib_list[idx].value.type = VAGenericValueTypeInteger;
    attrib_list[idx].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA | VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    idx++;

    attrib_list[idx].type = VASurfaceAttribMinWidth;
    attrib_list[idx].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[idx].value.type = VAGenericValueTypeInteger;
    attrib_list[idx].value.value.i = 64;
    idx++;

    attrib_list[idx].type = VASurfaceAttribMinHeight;
    attrib_list[idx].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[idx].value.type = VAGenericValueTypeInteger;
    attrib_list[idx].value.value.i = 64;
    idx++;

    attrib_list[idx].type = VASurfaceAttribMaxWidth;
    attrib_list[idx].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[idx].value.type = VAGenericValueTypeInteger;
    attrib_list[idx].value.value.i = 4096;
    idx++;

    attrib_list[idx].type = VASurfaceAttribMaxHeight;
    attrib_list[idx].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[idx].value.type = VAGenericValueTypeInteger;
    attrib_list[idx].value.value.i = 4096;
    idx++;

    *num_attribs = idx;
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaCreateSurfaces2(
    VADriverContextP ctx,
    unsigned int format,
    unsigned int width,
    unsigned int height,
    VASurfaceID *surfaces,
    unsigned int num_surfaces,
    VASurfaceAttrib *attrib_list,
    unsigned int num_attribs
) {
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    for (unsigned int i = 0; i < num_surfaces; i++) {
        int found = 0;
        for (int s = 1; s < MAX_SURFACES; s++) {
            if (!drv->surfaces[s].allocated) {
                unsigned int aligned_w = (width + 63) & ~63;
                unsigned int aligned_h = (height + 63) & ~63;
                void *raw = calloc(1, aligned_w * aligned_h * 3 / 2);
                if (!raw) {
                    return VA_STATUS_ERROR_ALLOCATION_FAILED;
                }
                drv->surfaces[s].allocated = 1;
                drv->surfaces[s].id = (VASurfaceID)s;
                drv->surfaces[s].width = width;
                drv->surfaces[s].height = height;
                drv->surfaces[s].format = format;
                drv->surfaces[s].stride = aligned_w;
                drv->surfaces[s].raw_data_size = aligned_w * aligned_h * 3 / 2;
                drv->surfaces[s].raw_data = raw;
                drv->surfaces[s].dma_fd = -1;
                drv->surfaces[s].has_decoded_frame = 0;
                memset(&drv->surfaces[s].vpu_out_buf, 0, sizeof(drv->surfaces[s].vpu_out_buf));
                surfaces[i] = (VASurfaceID)s;
                found = 1;
                break;
            }
        }
        if (!found) {
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaCreateSurfaces(
    VADriverContextP ctx,
    int width,
    int height,
    int format,
    int num_surfaces,
    VASurfaceID *surfaces
) {
    return hobot_vaCreateSurfaces2(ctx, format, width, height, surfaces, num_surfaces, NULL, 0);
}

static VAStatus hobot_vaDestroySurfaces(
    VADriverContextP ctx,
    VASurfaceID *surfaces,
    int num_surfaces
) {
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    for (int i = 0; i < num_surfaces; i++) {
        VASurfaceID s = surfaces[i];
        if (s > 0 && s < MAX_SURFACES) {
            HobotSurface *surf = &drv->surfaces[s];
            if (surf->has_decoded_frame) {
                VAContextID cid = surf->context_id;
                int qret = -1;
                if (cid > 0 && cid < MAX_CONTEXTS && drv->contexts[cid].allocated && drv->contexts[cid].vpu_running && !drv->contexts[cid].is_encoder) {
                    qret = hb_mm_mc_queue_output_buffer(&drv->contexts[cid].vpu_ctx, &surf->vpu_out_buf, 50);
                } else {
                    for (int c = 1; c < MAX_CONTEXTS; c++) {
                        if (drv->contexts[c].allocated && drv->contexts[c].vpu_running && !drv->contexts[c].is_encoder) {
                            qret = hb_mm_mc_queue_output_buffer(&drv->contexts[c].vpu_ctx, &surf->vpu_out_buf, 50);
                            break;
                        }
                    }
                }
                if (qret != 0) {
                    fprintf(stderr, "[HOBOT-VA] vaDestroySurfaces: queue_output_buffer ret=%d for surf=%d\n", qret, s);
                }
                surf->has_decoded_frame = 0;
                memset(&surf->vpu_out_buf, 0, sizeof(surf->vpu_out_buf));
            }
            if (surf->raw_data) {
                free(surf->raw_data);
                surf->raw_data = NULL;
            }
            surf->allocated = 0;
            surf->dma_fd = -1;
            surf->context_id = 0;
        }
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaQuerySurfaceStatus(
    VADriverContextP ctx,
    VASurfaceID render_target,
    VASurfaceStatus *status
) {
    if (!status) return VA_STATUS_ERROR_INVALID_PARAMETER;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (render_target <= 0 || render_target >= MAX_SURFACES || !drv->surfaces[render_target].allocated) {
        *status = VASurfaceReady;
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    HobotSurface *surf = &drv->surfaces[render_target];
    if (surf->has_decoded_frame) {
        *status = VASurfaceReady;
    } else {
        *status = VASurfaceRendering;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaCreateContext(
    VADriverContextP ctx,
    VAConfigID config_id,
    int picture_width,
    int picture_height,
    int flag,
    VASurfaceID *render_targets,
    int num_render_targets,
    VAContextID *context
) {
    if (!context) return VA_STATUS_ERROR_INVALID_PARAMETER;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (config_id <= 0 || config_id >= MAX_CONFIGS || !drv->configs[config_id].allocated) {
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    HobotConfig *cfg = &drv->configs[config_id];

    for (int i = 1; i < MAX_CONTEXTS; i++) {
        if (!drv->contexts[i].allocated) {
            HobotContext *c = &drv->contexts[i];
            memset(c, 0, sizeof(*c));
            c->allocated = 1;
            c->id = (VAContextID)i;
            c->width = picture_width;
            c->height = picture_height;
            c->profile = cfg->profile;
            c->current_render_target = VA_INVALID_SURFACE;
            c->enc_coded_buf = 0;
            c->frame_count = 0;

            int is_enc = (cfg->entrypoint == VAEntrypointEncSlice || cfg->entrypoint == VAEntrypointEncPicture);
            c->is_encoder = is_enc;

            media_codec_context_t *mctx = &c->vpu_ctx;
            memset(mctx, 0, sizeof(*mctx));

            media_codec_id_t cid = MEDIA_CODEC_ID_H264;
            if (cfg->profile == VAProfileHEVCMain || cfg->profile == VAProfileHEVCMain10) {
                cid = MEDIA_CODEC_ID_H265;
            } else if (cfg->profile == VAProfileJPEGBaseline) {
                cid = MEDIA_CODEC_ID_JPEG;
            }

            if (is_enc) {
                int ret = hb_mm_mc_get_default_context(cid, 1, mctx);
                if (ret != 0) {
                    mctx->codec_id = cid;
                    mctx->encoder = 1;
                }
                mctx->video_enc_params.width = picture_width;
                mctx->video_enc_params.height = picture_height;
                mctx->video_enc_params.pix_fmt = MC_PIXEL_FORMAT_NV12;
                mctx->video_enc_params.bitstream_buf_size = 4 * 1024 * 1024;
                mctx->video_enc_params.bitstream_buf_count = 5;
                mctx->video_enc_params.frame_buf_count = 5;
                mctx->video_enc_params.external_frame_buf = 0;
                mctx->video_enc_params.gop_params.gop_preset_idx = 9;
                mctx->video_enc_params.gop_params.decoding_refresh_type = 2;

                if (cid == MEDIA_CODEC_ID_H264) {
                    mctx->video_enc_params.rc_params.mode = MC_AV_RC_MODE_H264CBR;
                    mctx->video_enc_params.rc_params.h264_cbr_params.bit_rate = 10000; // 10 Mbps default
                    mctx->video_enc_params.rc_params.h264_cbr_params.frame_rate = 30;
                    mctx->video_enc_params.rc_params.h264_cbr_params.intra_period = 30;
                } else if (cid == MEDIA_CODEC_ID_H265) {
                    mctx->video_enc_params.rc_params.mode = MC_AV_RC_MODE_H265CBR;
                    mctx->video_enc_params.rc_params.h265_cbr_params.bit_rate = 10000;
                    mctx->video_enc_params.rc_params.h265_cbr_params.frame_rate = 30;
                    mctx->video_enc_params.rc_params.h265_cbr_params.intra_period = 30;
                } else if (cid == MEDIA_CODEC_ID_JPEG) {
                    mctx->video_enc_params.rc_params.mode = MC_AV_RC_MODE_MJPEGFIXQP;
                    mctx->video_enc_params.rc_params.mjpeg_fixqp_params.frame_rate = 30;
                    mctx->video_enc_params.rc_params.mjpeg_fixqp_params.quality_factor = 85;
                }

                ret = hb_mm_mc_initialize(mctx);
                if (ret == 0) {
                    ret = hb_mm_mc_configure(mctx);
                    if (ret == 0) {
                        ret = hb_mm_mc_start(mctx, NULL);
                        if (ret == 0) {
                            c->vpu_running = 1;
                        }
                    }
                }
            } else {
                /* Decoder */
                mctx->codec_id = cid;
                mctx->encoder = 0;
                mctx->video_dec_params.feed_mode = MC_FEEDING_MODE_FRAME_SIZE;
                mctx->video_dec_params.pix_fmt = MC_PIXEL_FORMAT_NV12;
                mctx->video_dec_params.bitstream_buf_size = 4 * 1024 * 1024;
                mctx->video_dec_params.bitstream_buf_count = 16;
                mctx->video_dec_params.frame_buf_count = 16;
                if (cid == MEDIA_CODEC_ID_H264) {
                    mctx->video_dec_params.h264_dec_config.reorder_enable = 0;
                    mctx->video_dec_params.h264_dec_config.skip_mode = 0;
                    mctx->video_dec_params.h264_dec_config.bandwidth_Opt = 0;
                }

                int ret = hb_mm_mc_initialize(mctx);
                if (ret == 0) {
                    ret = hb_mm_mc_configure(mctx);
                    if (ret == 0) {
                        ret = hb_mm_mc_start(mctx, NULL);
                        if (ret == 0) {
                            c->vpu_running = 1;
                        }
                    }
                }
            }

            *context = (VAContextID)i;
            return VA_STATUS_SUCCESS;
        }
    }
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

static VAStatus hobot_vaDestroyContext(VADriverContextP ctx, VAContextID context) {
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (context > 0 && context < MAX_CONTEXTS && drv->contexts[context].allocated) {
        HobotContext *hctx = &drv->contexts[context];
        if (hctx->vpu_running) {
            if (!hctx->is_encoder) {
                if (hctx->dec_in_buf_valid) {
                    hb_mm_mc_queue_input_buffer(&hctx->vpu_ctx, &hctx->dec_in_buf, 50);
                    hctx->dec_in_buf_valid = 0;
                    hctx->dec_in_buf_offset = 0;
                }
                for (int s = 1; s < MAX_SURFACES; s++) {
                    if (drv->surfaces[s].allocated && drv->surfaces[s].context_id == context) {
                        if (drv->surfaces[s].has_decoded_frame) {
                            int qret = hb_mm_mc_queue_output_buffer(&hctx->vpu_ctx, &drv->surfaces[s].vpu_out_buf, 50);
                            if (qret != 0) {
                                fprintf(stderr, "[HOBOT-VA] vaDestroyContext: queue_output_buffer ret=%d for surf=%d\n", qret, s);
                            }
                            drv->surfaces[s].has_decoded_frame = 0;
                            drv->surfaces[s].dma_fd = -1;
                            memset(&drv->surfaces[s].vpu_out_buf, 0, sizeof(drv->surfaces[s].vpu_out_buf));
                        }
                        drv->surfaces[s].context_id = 0;
                    }
                }
            }
            hb_mm_mc_stop(&hctx->vpu_ctx);
            hb_mm_mc_release(&hctx->vpu_ctx);
            hctx->vpu_running = 0;
        }
        hctx->allocated = 0;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaCreateBuffer(
    VADriverContextP ctx,
    VAContextID context,
    VABufferType type,
    unsigned int size,
    unsigned int num_elements,
    void *data,
    VABufferID *buf_id
) {
    if (!buf_id) return VA_STATUS_ERROR_INVALID_PARAMETER;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;

    for (int i = 1; i < MAX_BUFFERS; i++) {
        if (!drv->buffers[i].allocated) {
            drv->buffers[i].allocated = 1;
            drv->buffers[i].is_derived = 0;
            drv->buffers[i].id = (VABufferID)i;
            drv->buffers[i].type = type;
            drv->buffers[i].size = size * num_elements;
            memset(&drv->buffers[i].coded_segment, 0, sizeof(drv->buffers[i].coded_segment));

            if (type == VAEncCodedBufferType) {
                if (drv->buffers[i].size < 2 * 1024 * 1024) {
                    drv->buffers[i].size = 2 * 1024 * 1024;
                }
                drv->buffers[i].data = malloc(drv->buffers[i].size);
                if (!drv->buffers[i].data) {
                    drv->buffers[i].allocated = 0;
                    return VA_STATUS_ERROR_ALLOCATION_FAILED;
                }
                drv->buffers[i].coded_segment.buf = drv->buffers[i].data;
                drv->buffers[i].coded_segment.size = 0;
                drv->buffers[i].coded_segment.bit_offset = 0;
                drv->buffers[i].coded_segment.status = 0;
                drv->buffers[i].coded_segment.next = NULL;
            } else {
                drv->buffers[i].data = malloc(drv->buffers[i].size);
                if (!drv->buffers[i].data) {
                    drv->buffers[i].allocated = 0;
                    return VA_STATUS_ERROR_ALLOCATION_FAILED;
                }
                if (data) {
                    memcpy(drv->buffers[i].data, data, drv->buffers[i].size);
                }
            }
            *buf_id = (VABufferID)i;
            return VA_STATUS_SUCCESS;
        }
    }
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

static VAStatus hobot_vaCreateBuffer2(
    VADriverContextP ctx,
    VAContextID context,
    VABufferType type,
    unsigned int width,
    unsigned int height,
    unsigned int *unit_size,
    unsigned int *pitch,
    VABufferID *buf_id
) {
    if (unit_size) *unit_size = 1;
    if (pitch) *pitch = width;
    return hobot_vaCreateBuffer(ctx, context, type, width * height, 1, NULL, buf_id);
}

static VAStatus hobot_vaBufferSetNumElements(
    VADriverContextP ctx,
    VABufferID buf_id,
    unsigned int num_elements
) {
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaMapBuffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf) {
    if (!pbuf) return VA_STATUS_ERROR_INVALID_PARAMETER;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (buf_id > 0 && buf_id < MAX_BUFFERS && drv->buffers[buf_id].allocated) {
        if (drv->buffers[buf_id].type == VAEncCodedBufferType) {
            *pbuf = &drv->buffers[buf_id].coded_segment;
        } else {
            *pbuf = drv->buffers[buf_id].data;
        }
        return VA_STATUS_SUCCESS;
    }
    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus hobot_vaUnmapBuffer(VADriverContextP ctx, VABufferID buf_id) {
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaDestroyBuffer(VADriverContextP ctx, VABufferID buffer_id) {
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (buffer_id > 0 && buffer_id < MAX_BUFFERS && drv->buffers[buffer_id].allocated) {
        if (!drv->buffers[buffer_id].is_derived && drv->buffers[buffer_id].data) {
            free(drv->buffers[buffer_id].data);
            drv->buffers[buffer_id].data = NULL;
        }
        drv->buffers[buffer_id].allocated = 0;
        drv->buffers[buffer_id].is_derived = 0;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaBeginPicture(
    VADriverContextP ctx,
    VAContextID context,
    VASurfaceID render_target
) {
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (context <= 0 || context >= MAX_CONTEXTS || !drv->contexts[context].allocated) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    HobotContext *hctx = &drv->contexts[context];
    hctx->current_render_target = render_target;
    hctx->enc_coded_buf = 0;

    if (render_target <= 0 || render_target >= MAX_SURFACES || !drv->surfaces[render_target].allocated) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    HobotSurface *surf = &drv->surfaces[render_target];
    surf->context_id = context;
    if (!hctx->is_encoder) {
        if (hctx->dec_in_buf_valid) {
            hb_mm_mc_queue_input_buffer(&hctx->vpu_ctx, &hctx->dec_in_buf, 50);
            hctx->dec_in_buf_valid = 0;
            hctx->dec_in_buf_offset = 0;
        }
        if (surf->has_decoded_frame) {
            int qret = hb_mm_mc_queue_output_buffer(&hctx->vpu_ctx, &surf->vpu_out_buf, 50);
            if (qret != 0) {
                fprintf(stderr, "[HOBOT-VA] vaBeginPicture: queue_output_buffer failed: %d\n", qret);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            surf->has_decoded_frame = 0;
            surf->dma_fd = -1;
            memset(&surf->vpu_out_buf, 0, sizeof(surf->vpu_out_buf));
        }
        if ((hctx->sub_tail - hctx->sub_head) >= 128) {
            fprintf(stderr, "[HOBOT-VA] submitted_surfaces FIFO overflow (tail=%u head=%u)\n",
                    hctx->sub_tail, hctx->sub_head);
            return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
        }
        hctx->submitted_surfaces[hctx->sub_tail++ % 128] = render_target;
        hctx->frame_count++;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaRenderPicture(
    VADriverContextP ctx,
    VAContextID context,
    VABufferID *buffers,
    int num_buffers
) {
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (context <= 0 || context >= MAX_CONTEXTS || !drv->contexts[context].vpu_running) {
        return VA_STATUS_SUCCESS;
    }

    HobotContext *hctx = &drv->contexts[context];
    media_codec_context_t *mctx = &hctx->vpu_ctx;

    if (hctx->is_encoder) {
        for (int i = 0; i < num_buffers; i++) {
            VABufferID bid = buffers[i];
            if (bid <= 0 || bid >= MAX_BUFFERS || !drv->buffers[bid].allocated) continue;
            HobotBuffer *b = &drv->buffers[bid];

            if (b->type == VAEncPictureParameterBufferType) {
                if (hctx->profile == VAProfileHEVCMain || hctx->profile == VAProfileHEVCMain10) {
                    VAEncPictureParameterBufferHEVC *pic = (VAEncPictureParameterBufferHEVC *)b->data;
                    hctx->enc_coded_buf = pic->coded_buf;
                    if (pic->pic_fields.bits.idr_pic_flag) {
                        hb_mm_mc_request_idr_frame(&hctx->vpu_ctx);
                    }
                } else if (hctx->profile == VAProfileJPEGBaseline) {
                    VAEncPictureParameterBufferJPEG *pic = (VAEncPictureParameterBufferJPEG *)b->data;
                    hctx->enc_coded_buf = pic->coded_buf;
                } else {
                    VAEncPictureParameterBufferH264 *pic = (VAEncPictureParameterBufferH264 *)b->data;
                    hctx->enc_coded_buf = pic->coded_buf;
                    if (pic->pic_fields.bits.idr_pic_flag) {
                        hb_mm_mc_request_idr_frame(&hctx->vpu_ctx);
                    }
                }
            } else if (b->type == VAEncSequenceParameterBufferType) {
                if (hctx->profile == VAProfileHEVCMain || hctx->profile == VAProfileHEVCMain10) {
                    VAEncSequenceParameterBufferHEVC *seq = (VAEncSequenceParameterBufferHEVC *)b->data;
                    if (seq->intra_period > 0) {
                        hctx->vpu_ctx.video_enc_params.rc_params.h265_cbr_params.intra_period = seq->intra_period;
                    }
                    if (seq->bits_per_second > 0) {
                        hctx->vpu_ctx.video_enc_params.rc_params.h265_cbr_params.bit_rate = seq->bits_per_second / 1000;
                    }
                } else if (hctx->profile != VAProfileJPEGBaseline) {
                    VAEncSequenceParameterBufferH264 *seq = (VAEncSequenceParameterBufferH264 *)b->data;
                    if (seq->intra_period > 0) {
                        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.intra_period = seq->intra_period;
                    }
                    if (seq->bits_per_second > 0) {
                        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate = seq->bits_per_second / 1000;
                    }
                }
                hb_mm_mc_set_rate_control_config(&hctx->vpu_ctx, &hctx->vpu_ctx.video_enc_params.rc_params);
            } else if (b->type == VAEncMiscParameterBufferType) {
                VAEncMiscParameterBuffer *misc = (VAEncMiscParameterBuffer *)b->data;
                if (misc->type == VAEncMiscParameterTypeRateControl) {
                    VAEncMiscParameterRateControl *rc = (VAEncMiscParameterRateControl *)misc->data;
                    uint32_t kbps = rc->bits_per_second / 1000;
                    if (kbps > 0) {
                        if (hctx->vpu_ctx.codec_id == MEDIA_CODEC_ID_H264) {
                            hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate = kbps;
                        } else if (hctx->vpu_ctx.codec_id == MEDIA_CODEC_ID_H265) {
                            hctx->vpu_ctx.video_enc_params.rc_params.h265_cbr_params.bit_rate = kbps;
                        }
                        hb_mm_mc_set_rate_control_config(&hctx->vpu_ctx, &hctx->vpu_ctx.video_enc_params.rc_params);
                    }
                } else if (misc->type == VAEncMiscParameterTypeFrameRate) {
                    VAEncMiscParameterFrameRate *fr = (VAEncMiscParameterFrameRate *)misc->data;
                    uint32_t num = fr->framerate & 0xffff;
                    uint32_t den = (fr->framerate >> 16) & 0xffff;
                    if (den == 0) den = 1;
                    uint32_t fps = num / den;
                    if (fps > 0) {
                        if (hctx->vpu_ctx.codec_id == MEDIA_CODEC_ID_H264) {
                            hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.frame_rate = fps;
                        } else if (hctx->vpu_ctx.codec_id == MEDIA_CODEC_ID_H265) {
                            hctx->vpu_ctx.video_enc_params.rc_params.h265_cbr_params.frame_rate = fps;
                        }
                        hb_mm_mc_set_rate_control_config(&hctx->vpu_ctx, &hctx->vpu_ctx.video_enc_params.rc_params);
                    }
                }
            }
        }
        return VA_STATUS_SUCCESS;
    }

    /* Decoder First pass: parse picture parameters if present */
    for (int i = 0; i < num_buffers; i++) {
        VABufferID bid = buffers[i];
        if (bid > 0 && bid < MAX_BUFFERS && drv->buffers[bid].allocated) {
            HobotBuffer *b = &drv->buffers[bid];
            if (b->type == VAPictureParameterBufferType) {
                if (hctx->profile == VAProfileHEVCMain || hctx->profile == VAProfileHEVCMain10) {
                    if (b->size >= sizeof(VAPictureParameterBufferHEVC)) {
                        VAPictureParameterBufferHEVC *pic = (VAPictureParameterBufferHEVC *)b->data;
                        int prof_idc = (hctx->profile == VAProfileHEVCMain10) ? 2 : 1;
                        hctx->cached_vps_len = generate_hevc_vps(prof_idc, hctx->cached_vps, sizeof(hctx->cached_vps));
                        hctx->cached_sps_len = generate_hevc_sps(pic, prof_idc, hctx->cached_sps, sizeof(hctx->cached_sps));
                        hctx->cached_pps_len = generate_hevc_pps(pic, hctx->cached_pps, sizeof(hctx->cached_pps));
                    }
                } else if (hctx->profile != VAProfileJPEGBaseline) {
                    if (b->size >= sizeof(VAPictureParameterBufferH264)) {
                        VAPictureParameterBufferH264 *pic = (VAPictureParameterBufferH264 *)b->data;
                        hctx->cached_sps_len = generate_h264_sps(pic, hctx->cached_sps, sizeof(hctx->cached_sps));
                        hctx->cached_pps_len = generate_h264_pps(pic, hctx->cached_pps, sizeof(hctx->cached_pps));
                    }
                }
            }
        }
    }

    /* Decoder Second pass: feed slice data (accumulated per picture) */
    for (int i = 0; i < num_buffers; i++) {
        VABufferID bid = buffers[i];
        if (bid > 0 && bid < MAX_BUFFERS && drv->buffers[bid].allocated) {
            HobotBuffer *b = &drv->buffers[bid];
            if (b->type == VASliceDataBufferType) {
                if (!hctx->dec_in_buf_valid) {
                    memset(&hctx->dec_in_buf, 0, sizeof(hctx->dec_in_buf));
                    int ret = -1;
                    for (int retry = 0; retry < 5; retry++) {
                        ret = hb_mm_mc_dequeue_input_buffer(mctx, &hctx->dec_in_buf, 50);
                        if (ret == 0 && hctx->dec_in_buf.vstream_buf.vir_ptr) break;
                    }
                    if (ret != 0 || !hctx->dec_in_buf.vstream_buf.vir_ptr) {
                        fprintf(stderr, "[HOBOT-VA] dequeue_input_buffer failed: %d\n", ret);
                        return VA_STATUS_ERROR_OPERATION_FAILED;
                    }
                    hctx->dec_in_buf_valid = 1;
                    hctx->dec_in_buf_offset = 0;

                    uint8_t *dst = (uint8_t *)hctx->dec_in_buf.vstream_buf.vir_ptr;
                    if (!hctx->headers_sent) {
                        if (hctx->cached_vps_len > 0) {
                            dst[hctx->dec_in_buf_offset++] = 0x00; dst[hctx->dec_in_buf_offset++] = 0x00;
                            dst[hctx->dec_in_buf_offset++] = 0x00; dst[hctx->dec_in_buf_offset++] = 0x01;
                            memcpy(dst + hctx->dec_in_buf_offset, hctx->cached_vps, hctx->cached_vps_len);
                            hctx->dec_in_buf_offset += hctx->cached_vps_len;
                        }
                        if (hctx->cached_sps_len > 0) {
                            dst[hctx->dec_in_buf_offset++] = 0x00; dst[hctx->dec_in_buf_offset++] = 0x00;
                            dst[hctx->dec_in_buf_offset++] = 0x00; dst[hctx->dec_in_buf_offset++] = 0x01;
                            memcpy(dst + hctx->dec_in_buf_offset, hctx->cached_sps, hctx->cached_sps_len);
                            hctx->dec_in_buf_offset += hctx->cached_sps_len;
                        }
                        if (hctx->cached_pps_len > 0) {
                            dst[hctx->dec_in_buf_offset++] = 0x00; dst[hctx->dec_in_buf_offset++] = 0x00;
                            dst[hctx->dec_in_buf_offset++] = 0x00; dst[hctx->dec_in_buf_offset++] = 0x01;
                            memcpy(dst + hctx->dec_in_buf_offset, hctx->cached_pps, hctx->cached_pps_len);
                            hctx->dec_in_buf_offset += hctx->cached_pps_len;
                        }
                        if (hctx->cached_sps_len > 0 && hctx->cached_pps_len > 0) {
                            hctx->headers_sent = 1;
                        }
                    }
                }

                uint8_t *dst = (uint8_t *)hctx->dec_in_buf.vstream_buf.vir_ptr;
                uint8_t *src = (uint8_t *)b->data;
                int src_size = b->size;
                int max_cap = mctx->video_dec_params.bitstream_buf_size > 0 ?
                              mctx->video_dec_params.bitstream_buf_size : (4 * 1024 * 1024);

                int has_start_code = 0;
                if (src_size >= 4 && src[0] == 0 && src[1] == 0 && (src[2] == 1 || (src[2] == 0 && src[3] == 1))) {
                    has_start_code = 1;
                }

                int needed = (has_start_code ? 0 : 4) + src_size;
                if (hctx->dec_in_buf_offset + needed > max_cap) {
                    fprintf(stderr, "[HOBOT-VA] bitstream buffer overflow! offset=%d needed=%d cap=%d\n",
                            hctx->dec_in_buf_offset, needed, max_cap);
                    return VA_STATUS_ERROR_OPERATION_FAILED;
                }

                if (!has_start_code) {
                    dst[hctx->dec_in_buf_offset++] = 0x00;
                    dst[hctx->dec_in_buf_offset++] = 0x00;
                    dst[hctx->dec_in_buf_offset++] = 0x00;
                    dst[hctx->dec_in_buf_offset++] = 0x01;
                }

                memcpy(dst + hctx->dec_in_buf_offset, src, src_size);
                hctx->dec_in_buf_offset += src_size;
            }
        }
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaEndPicture(VADriverContextP ctx, VAContextID context) {
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (context <= 0 || context >= MAX_CONTEXTS || !drv->contexts[context].allocated) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    HobotContext *hctx = &drv->contexts[context];

    if (hctx->is_encoder) {
        VASurfaceID sid = hctx->current_render_target;
        if (sid <= 0 || sid >= MAX_SURFACES || !drv->surfaces[sid].allocated) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        HobotSurface *surf = &drv->surfaces[sid];

        VABufferID cid = hctx->enc_coded_buf;
        if (cid <= 0 || cid >= MAX_BUFFERS || !drv->buffers[cid].allocated) {
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        HobotBuffer *coded = &drv->buffers[cid];

        if (!hctx->vpu_running) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        media_codec_buffer_t in_buf;
        memset(&in_buf, 0, sizeof(in_buf));
        int ret = hb_mm_mc_dequeue_input_buffer(&hctx->vpu_ctx, &in_buf, 1000);
        if (ret != 0) {
            fprintf(stderr, "[HOBOT-VA] dequeue_in failed: ret=%d\n", ret);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        int enc_w = hctx->vpu_ctx.video_enc_params.width;
        int enc_h = hctx->vpu_ctx.video_enc_params.height;
        int copy_w = (surf->width < enc_w) ? surf->width : enc_w;
        int copy_h = (surf->height < enc_h) ? surf->height : enc_h;
        int src_stride = surf->stride > 0 ? surf->stride : surf->width;
        int dst_stride = in_buf.vframe_buf.stride > 0 ? in_buf.vframe_buf.stride : enc_w;

        if (surf->raw_data) {
            for (int r = 0; r < copy_h; r++) {
                memcpy((char *)in_buf.vframe_buf.vir_ptr[0] + r * dst_stride,
                       (char *)surf->raw_data + r * src_stride,
                       copy_w);
            }
            char *src_uv = (char *)surf->raw_data + src_stride * surf->height;
            for (int r = 0; r < copy_h / 2; r++) {
                memcpy((char *)in_buf.vframe_buf.vir_ptr[1] + r * dst_stride,
                       src_uv + r * src_stride,
                       copy_w);
            }
        } else if (surf->has_decoded_frame && surf->vpu_out_buf.vframe_buf.vir_ptr[0]) {
            int dec_stride = surf->vpu_out_buf.vframe_buf.stride > 0 ? surf->vpu_out_buf.vframe_buf.stride : copy_w;
            for (int r = 0; r < copy_h; r++) {
                memcpy((char *)in_buf.vframe_buf.vir_ptr[0] + r * dst_stride,
                       (char *)surf->vpu_out_buf.vframe_buf.vir_ptr[0] + r * dec_stride,
                       copy_w);
            }
            if (surf->vpu_out_buf.vframe_buf.vir_ptr[1]) {
                for (int r = 0; r < copy_h / 2; r++) {
                    memcpy((char *)in_buf.vframe_buf.vir_ptr[1] + r * dst_stride,
                           (char *)surf->vpu_out_buf.vframe_buf.vir_ptr[1] + r * dec_stride,
                           copy_w);
                }
            }
        } else {
            memset(in_buf.vframe_buf.vir_ptr[0], 0x80, dst_stride * copy_h);
            memset(in_buf.vframe_buf.vir_ptr[1], 0x80, dst_stride * copy_h / 2);
        }

        uint64_t current_frame = hctx->frame_count++;
        in_buf.vframe_buf.pts = current_frame * 33333;

        ret = hb_mm_mc_queue_input_buffer(&hctx->vpu_ctx, &in_buf, 1000);
        if (ret != 0) {
            fprintf(stderr, "[HOBOT-VA] queue_in failed: ret=%d\n", ret);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        media_codec_output_buffer_info_t out_info;
        media_codec_buffer_t out_buf;
        memset(&out_info, 0, sizeof(out_info));
        memset(&out_buf, 0, sizeof(out_buf));

        ret = hb_mm_mc_dequeue_output_buffer(&hctx->vpu_ctx, &out_buf, &out_info, 2000);
        if (ret == 0 && out_buf.vstream_buf.size > 0) {
            if (current_frame < 3 || current_frame % 60 == 0) {
                fprintf(stderr, "[HOBOT-VA] vaEndPicture: frame %lu encoded successfully (%u bytes)\n",
                        (unsigned long)current_frame, out_buf.vstream_buf.size);
            }
            uint32_t stream_size = out_buf.vstream_buf.size;
            if (stream_size > coded->size) stream_size = coded->size;
            memcpy(coded->data, out_buf.vstream_buf.vir_ptr, stream_size);
            coded->coded_segment.size = stream_size;
            coded->coded_segment.bit_offset = 0;
            coded->coded_segment.status = 0;
            coded->coded_segment.buf = coded->data;
            coded->coded_segment.next = NULL;

            hb_mm_mc_queue_output_buffer(&hctx->vpu_ctx, &out_buf, 100);
        } else {
            fprintf(stderr, "[HOBOT-VA] vaEndPicture: dequeue_output failed frame %lu ret=%d size=%d\n",
                    (unsigned long)current_frame, ret, out_buf.vstream_buf.size);
            coded->coded_segment.size = 0;
        }

        return VA_STATUS_SUCCESS;
    }

    /* Decoder: queue assembled picture input buffer to VPU */
    if (hctx->dec_in_buf_valid && hctx->dec_in_buf_offset > 0) {
        hctx->dec_in_buf.vstream_buf.size = hctx->dec_in_buf_offset;
        hctx->dec_in_buf.vstream_buf.pts = (uint64_t)hctx->frame_count;
        int qret = hb_mm_mc_queue_input_buffer(&hctx->vpu_ctx, &hctx->dec_in_buf, 100);
        hctx->dec_in_buf_valid = 0;
        hctx->dec_in_buf_offset = 0;
        if (qret != 0) {
            fprintf(stderr, "[HOBOT-VA] vaEndPicture: queue_input_buffer failed: %d\n", qret);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaSyncSurface(VADriverContextP ctx, VASurfaceID render_target) {
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (render_target <= 0 || render_target >= MAX_SURFACES || !drv->surfaces[render_target].allocated) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    HobotSurface *surf = &drv->surfaces[render_target];
    if (surf->has_decoded_frame && surf->dma_fd >= 0) {
        return VA_STATUS_SUCCESS;
    }

    VAContextID cid = surf->context_id;
    HobotContext *hctx = NULL;
    if (cid > 0 && cid < MAX_CONTEXTS && drv->contexts[cid].allocated && drv->contexts[cid].vpu_running && !drv->contexts[cid].is_encoder) {
        hctx = &drv->contexts[cid];
    } else {
        for (int i = 1; i < MAX_CONTEXTS; i++) {
            if (drv->contexts[i].allocated && drv->contexts[i].vpu_running && !drv->contexts[i].is_encoder) {
                hctx = &drv->contexts[i];
                break;
            }
        }
    }

    if (!hctx) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    media_codec_context_t *mctx = &hctx->vpu_ctx;

    int max_attempts = 30;
    while (!surf->has_decoded_frame && max_attempts-- > 0) {
        media_codec_output_buffer_info_t out_info;
        media_codec_buffer_t out_buf;
        memset(&out_info, 0, sizeof(out_info));
        memset(&out_buf, 0, sizeof(out_buf));

        int ret = hb_mm_mc_dequeue_output_buffer(mctx, &out_buf, &out_info, 50);
        if (ret == 0) {
            VASurfaceID target = 0;
            if (hctx->sub_head != hctx->sub_tail) {
                target = hctx->submitted_surfaces[hctx->sub_head++ % 128];
            } else {
                target = render_target;
            }
            if (target > 0 && target < MAX_SURFACES && drv->surfaces[target].allocated) {
                HobotSurface *tsurf = &drv->surfaces[target];
                if (tsurf->has_decoded_frame) {
                    hb_mm_mc_queue_output_buffer(mctx, &tsurf->vpu_out_buf, 50);
                }
                tsurf->vpu_out_buf = out_buf;
                tsurf->has_decoded_frame = 1;
                tsurf->dma_fd = out_buf.vframe_buf.fd[0];
                tsurf->stride = out_buf.vframe_buf.stride;
            }
        } else if (ret == HB_MEDIA_ERR_WAIT_TIMEOUT) {
            continue;
        } else {
            fprintf(stderr, "[HOBOT-VA] vaSyncSurface: dequeue_output_buffer error %d\n", ret);
            break;
        }
    }

    if (surf->has_decoded_frame && surf->dma_fd >= 0) {
        return VA_STATUS_SUCCESS;
    }

    return VA_STATUS_ERROR_TIMEDOUT;
}

/* Zero-Copy Export Surface Handle for mpv and Chromium (DRM PRIME 2) */
static VAStatus hobot_vaExportSurfaceHandle(
    VADriverContextP ctx,
    VASurfaceID surface_id,
    uint32_t mem_type,
    uint32_t flags,
    void *descriptor
) {
    if (!descriptor) return VA_STATUS_ERROR_INVALID_PARAMETER;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;

    if (surface_id <= 0 || surface_id >= MAX_SURFACES || !drv->surfaces[surface_id].allocated) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    HobotSurface *surf = &drv->surfaces[surface_id];
    if (surf->dma_fd < 0 && !surf->has_decoded_frame) {
        hobot_vaSyncSurface(ctx, surface_id);
    }
    if (surf->dma_fd < 0) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    int exp_fd = dup(surf->dma_fd);
    if (exp_fd < 0) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    uint32_t pitch = surf->stride > 0 ? surf->stride : surf->width;
    uint32_t vstride = (surf->vpu_out_buf.vframe_buf.vstride > 0) ?
                       surf->vpu_out_buf.vframe_buf.vstride : ((surf->height + 7) & ~7);
    uint32_t buf_size = (surf->vpu_out_buf.vframe_buf.size > 0) ?
                        surf->vpu_out_buf.vframe_buf.size : (pitch * vstride * 3 / 2);
    uint32_t uv_offset = 0;
    if (surf->vpu_out_buf.vframe_buf.vir_ptr[0] && surf->vpu_out_buf.vframe_buf.vir_ptr[1] &&
        surf->vpu_out_buf.vframe_buf.vir_ptr[1] > surf->vpu_out_buf.vframe_buf.vir_ptr[0]) {
        uv_offset = (uint32_t)(surf->vpu_out_buf.vframe_buf.vir_ptr[1] - surf->vpu_out_buf.vframe_buf.vir_ptr[0]);
    } else {
        uv_offset = pitch * vstride;
    }

    VADRMPRIMESurfaceDescriptor *desc = (VADRMPRIMESurfaceDescriptor *)descriptor;
    desc->fourcc = VA_FOURCC_NV12;
    desc->width = surf->width;
    desc->height = surf->height;
    desc->num_objects = 1;
    desc->objects[0].fd = exp_fd;
    desc->objects[0].size = buf_size;
    desc->objects[0].drm_format_modifier = 0;

    desc->num_layers = 1;
    desc->layers[0].drm_format = VA_FOURCC_NV12;
    desc->layers[0].num_planes = 2;
    desc->layers[0].object_index[0] = 0;
    desc->layers[0].offset[0] = 0;
    desc->layers[0].pitch[0] = pitch;

    desc->layers[0].object_index[1] = 0;
    desc->layers[0].offset[1] = uv_offset;
    desc->layers[0].pitch[1] = pitch;

    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaCreateImage(
    VADriverContextP ctx,
    VAImageFormat *format,
    int width,
    int height,
    VAImage *image
) {
    if (!format || !image) return VA_STATUS_ERROR_INVALID_PARAMETER;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;

    for (int i = 1; i < MAX_IMAGES; i++) {
        if (!drv->images[i].allocated) {
            drv->images[i].allocated = 1;
            drv->images[i].id = (VAImageID)i;

            unsigned int stride = (width + 15) & ~15;
            unsigned int data_size = stride * height * 3 / 2;

            VABufferID buf_id;
            VAStatus st = hobot_vaCreateBuffer(ctx, 0, VAImageBufferType, data_size, 1, NULL, &buf_id);
            if (st != VA_STATUS_SUCCESS) {
                drv->images[i].allocated = 0;
                return st;
            }

            drv->images[i].buf_id = buf_id;
            drv->images[i].surface_id = 0;
            drv->images[i].image.image_id = (VAImageID)i;
            drv->images[i].image.format = *format;
            drv->images[i].image.width = width;
            drv->images[i].image.height = height;
            drv->images[i].image.buf = buf_id;
            drv->images[i].image.data_size = data_size;
            drv->images[i].image.num_planes = 2;
            drv->images[i].image.pitches[0] = stride;
            drv->images[i].image.pitches[1] = stride;
            drv->images[i].image.offsets[0] = 0;
            drv->images[i].image.offsets[1] = stride * height;

            *image = drv->images[i].image;
            return VA_STATUS_SUCCESS;
        }
    }
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

static VAStatus hobot_vaDestroyImage(VADriverContextP ctx, VAImageID image) {
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (image > 0 && image < MAX_IMAGES && drv->images[image].allocated) {
        hobot_vaDestroyBuffer(ctx, drv->images[image].buf_id);
        drv->images[image].allocated = 0;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaGetImage(
    VADriverContextP ctx,
    VASurfaceID surface,
    int x,
    int y,
    unsigned int width,
    unsigned int height,
    VAImageID image
) {
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (surface <= 0 || surface >= MAX_SURFACES || !drv->surfaces[surface].allocated) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (image <= 0 || image >= MAX_IMAGES || !drv->images[image].allocated) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    HobotSurface *s = &drv->surfaces[surface];
    HobotImage *img = &drv->images[image];
    void *dst_data = drv->buffers[img->buf_id].data;

    if (!s->has_decoded_frame) {
        hobot_vaSyncSurface(ctx, surface);
    }

    if (s->has_decoded_frame && s->vpu_out_buf.vframe_buf.vir_ptr[0] && dst_data) {
        unsigned char *y_src = s->vpu_out_buf.vframe_buf.vir_ptr[0];
        unsigned char *uv_src = s->vpu_out_buf.vframe_buf.vir_ptr[1];
        unsigned int src_stride = s->vpu_out_buf.vframe_buf.stride;
        unsigned int dst_stride = img->image.pitches[0] > 0 ? img->image.pitches[0] : s->width;
        unsigned int copy_w = (s->width < img->image.width) ? s->width : img->image.width;
        unsigned int copy_h = (s->height < img->image.height) ? s->height : img->image.height;

        for (unsigned int r = 0; r < copy_h; r++) {
            memcpy((char *)dst_data + r * dst_stride, y_src + r * src_stride, copy_w);
        }
        if (uv_src) {
            unsigned int dst_uv_offset = img->image.offsets[1] > 0 ? img->image.offsets[1] : (dst_stride * copy_h);
            for (unsigned int r = 0; r < copy_h / 2; r++) {
                memcpy((char *)dst_data + dst_uv_offset + r * dst_stride, uv_src + r * src_stride, copy_w);
            }
        }
    } else if (s->raw_data && dst_data) {
        uint32_t copy_bytes = s->raw_data_size;
        if (copy_bytes > drv->buffers[img->buf_id].size) {
            copy_bytes = drv->buffers[img->buf_id].size;
        }
        memcpy(dst_data, s->raw_data, copy_bytes);
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaDeriveImage(
    VADriverContextP ctx,
    VASurfaceID surface,
    VAImage *image
) {
    if (!image) return VA_STATUS_ERROR_INVALID_PARAMETER;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (surface <= 0 || surface >= MAX_SURFACES || !drv->surfaces[surface].allocated) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    HobotSurface *s = &drv->surfaces[surface];
    if (!s->has_decoded_frame) {
        hobot_vaSyncSurface(ctx, surface);
    }

    int img_idx = -1;
    for (int i = 1; i < MAX_IMAGES; i++) {
        if (!drv->images[i].allocated) {
            img_idx = i;
            break;
        }
    }
    if (img_idx < 0) return VA_STATUS_ERROR_ALLOCATION_FAILED;

    int buf_idx = -1;
    for (int i = 1; i < MAX_BUFFERS; i++) {
        if (!drv->buffers[i].allocated) {
            buf_idx = i;
            break;
        }
    }
    if (buf_idx < 0) return VA_STATUS_ERROR_ALLOCATION_FAILED;

    drv->buffers[buf_idx].allocated = 1;
    drv->buffers[buf_idx].is_derived = 1;
    drv->buffers[buf_idx].id = (VABufferID)buf_idx;
    drv->buffers[buf_idx].type = VAImageBufferType;
    drv->buffers[buf_idx].size = s->raw_data_size;
    drv->buffers[buf_idx].data = s->raw_data;

    image->image_id = (VAImageID)img_idx;
    image->format.fourcc = VA_FOURCC_NV12;
    image->format.byte_order = VA_LSB_FIRST;
    image->format.bits_per_pixel = 12;
    image->width = s->width;
    image->height = s->height;
    image->buf = (VABufferID)buf_idx;
    image->num_planes = 2;
    image->pitches[0] = s->width;
    image->offsets[0] = 0;
    image->pitches[1] = s->width;
    image->offsets[1] = s->width * s->height;
    image->data_size = s->raw_data_size;

    drv->images[img_idx].allocated = 1;
    drv->images[img_idx].id = (VAImageID)img_idx;
    drv->images[img_idx].image = *image;
    drv->images[img_idx].buf_id = (VABufferID)buf_idx;
    drv->images[img_idx].surface_id = surface;

    if (s->has_decoded_frame && s->vpu_out_buf.vframe_buf.vir_ptr[0] && s->raw_data) {
        int vpu_stride = s->vpu_out_buf.vframe_buf.stride > 0 ? s->vpu_out_buf.vframe_buf.stride : s->width;
        if (vpu_stride == s->width) {
            memcpy(s->raw_data, s->vpu_out_buf.vframe_buf.vir_ptr[0], s->width * s->height);
            if (s->vpu_out_buf.vframe_buf.vir_ptr[1]) {
                memcpy((char *)s->raw_data + s->width * s->height, s->vpu_out_buf.vframe_buf.vir_ptr[1], s->width * s->height / 2);
            }
        } else {
            for (int r = 0; r < s->height; r++) {
                memcpy((char *)s->raw_data + r * s->width,
                       (char *)s->vpu_out_buf.vframe_buf.vir_ptr[0] + r * vpu_stride,
                       s->width);
            }
            if (s->vpu_out_buf.vframe_buf.vir_ptr[1]) {
                char *dst_uv = (char *)s->raw_data + s->width * s->height;
                char *src_uv = (char *)s->vpu_out_buf.vframe_buf.vir_ptr[1];
                for (int r = 0; r < s->height / 2; r++) {
                    memcpy(dst_uv + r * s->width,
                           src_uv + r * vpu_stride,
                           s->width);
                }
            }
        }
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaQueryImageFormats(
    VADriverContextP ctx,
    VAImageFormat *format_list,
    int *num_formats
) {
    if (!num_formats) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (!format_list) {
        *num_formats = 1;
        return VA_STATUS_SUCCESS;
    }
    format_list[0].fourcc = VA_FOURCC_NV12;
    format_list[0].byte_order = VA_LSB_FIRST;
    format_list[0].bits_per_pixel = 12;
    *num_formats = 1;
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaQuerySubpictureFormats(
    VADriverContextP ctx,
    VAImageFormat *format_list,
    unsigned int *flags,
    unsigned int *num_formats
) {
    if (num_formats) *num_formats = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaCreateSubpicture(
    VADriverContextP ctx,
    VAImageID image,
    VASubpictureID *subpicture
) {
    if (subpicture) *subpicture = (VASubpictureID)1;
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaDestroySubpicture(VADriverContextP ctx, VASubpictureID subpicture) {
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaSetSubpictureImage(VADriverContextP ctx, VASubpictureID subpicture, VAImageID image) {
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaSetSubpictureChromakey(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    unsigned int chromakey_min,
    unsigned int chromakey_max,
    unsigned int chromakey_mask
) {
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaSetSubpictureGlobalAlpha(VADriverContextP ctx, VASubpictureID subpicture, float global_alpha) {
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaAssociateSubpicture(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    VASurfaceID *target_surfaces,
    int num_surfaces,
    short src_x,
    short src_y,
    unsigned short src_width,
    unsigned short src_height,
    short dest_x,
    short dest_y,
    unsigned short dest_width,
    unsigned short dest_height,
    unsigned int flags
) {
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaDeassociateSubpicture(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    VASurfaceID *target_surfaces,
    int num_surfaces
) {
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaQueryDisplayAttributes(
    VADriverContextP ctx,
    VADisplayAttribute *attr_list,
    int *num_attributes
) {
    if (num_attributes) *num_attributes = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaGetDisplayAttributes(
    VADriverContextP ctx,
    VADisplayAttribute *attr_list,
    int num_attributes
) {
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaSetDisplayAttributes(
    VADriverContextP ctx,
    VADisplayAttribute *attr_list,
    int num_attributes
) {
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaLockSurface(
    VADriverContextP ctx,
    VASurfaceID surface,
    unsigned int *fourcc,
    unsigned int *luma_stride,
    unsigned int *chroma_u_stride,
    unsigned int *chroma_v_stride,
    unsigned int *luma_offset,
    unsigned int *chroma_u_offset,
    unsigned int *chroma_v_offset,
    unsigned int *buffer_name,
    void **buffer
) {
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus hobot_vaUnlockSurface(VADriverContextP ctx, VASurfaceID surface) {
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaSetImagePalette(VADriverContextP ctx, VAImageID image, unsigned char *palette) {
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaPutImage(
    VADriverContextP ctx,
    VASurfaceID surface,
    VAImageID image,
    int src_x,
    int src_y,
    unsigned int src_width,
    unsigned int src_height,
    int dest_x,
    int dest_y,
    unsigned int dest_width,
    unsigned int dest_height
) {
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (surface <= 0 || surface >= MAX_SURFACES || !drv->surfaces[surface].allocated) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (image <= 0 || image >= MAX_IMAGES || !drv->images[image].allocated) {
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }
    HobotSurface *s = &drv->surfaces[surface];
    HobotImage *img = &drv->images[image];
    void *src_data = drv->buffers[img->buf_id].data;

    if (s->raw_data && src_data) {
        memcpy(s->raw_data, src_data, s->raw_data_size);
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaPutSurface(
    VADriverContextP ctx,
    VASurfaceID surface,
    void* draw, /* Drawable of window system */
    short srcx,
    short srcy,
    unsigned short srcw,
    unsigned short srch,
    short destx,
    short desty,
    unsigned short destw,
    unsigned short desth,
    VARectangle *cliprects,
    unsigned int number_cliprects,
    unsigned int flags
) {
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (surface <= 0 || surface >= MAX_SURFACES || !drv->surfaces[surface].allocated) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    return hobot_vaSyncSurface(ctx, surface);
}

/* Driver Initialization Entrypoints */
VAStatus __vaDriverInit_1_0(VADriverContextP ctx);
VAStatus __vaDriverInit_0_32(VADriverContextP ctx);

static VAStatus hobot_init_driver(VADriverContextP ctx) {
    if (!ctx) return VA_STATUS_ERROR_INVALID_CONTEXT;

    HobotDriverData *drv = calloc(1, sizeof(HobotDriverData));
    if (!drv) return VA_STATUS_ERROR_ALLOCATION_FAILED;
    drv->initialized = 1;

    ctx->pDriverData = (void *)drv;
    ctx->version_major = 1;
    ctx->version_minor = 14;
    ctx->max_profiles = NUM_SUPPORTED_PROFILES;
    ctx->max_entrypoints = 2;
    ctx->max_attributes = 16;
    ctx->max_image_formats = 1;
    ctx->max_subpic_formats = 1;
    ctx->str_vendor = HOBOT_VA_DRIVER_STR;

    struct VADriverVTable *vtable = ctx->vtable;
    vtable->vaTerminate = hobot_vaTerminate;
    vtable->vaQueryConfigProfiles = hobot_vaQueryConfigProfiles;
    vtable->vaQueryConfigEntrypoints = hobot_vaQueryConfigEntrypoints;
    vtable->vaGetConfigAttributes = hobot_vaGetConfigAttributes;
    vtable->vaCreateConfig = hobot_vaCreateConfig;
    vtable->vaDestroyConfig = hobot_vaDestroyConfig;
    vtable->vaQueryConfigAttributes = hobot_vaQueryConfigAttributes;
    vtable->vaQuerySurfaceAttributes = hobot_vaQuerySurfaceAttributes;
    vtable->vaCreateSurfaces = hobot_vaCreateSurfaces;
    vtable->vaCreateSurfaces2 = hobot_vaCreateSurfaces2;
    vtable->vaDestroySurfaces = hobot_vaDestroySurfaces;
    vtable->vaQuerySurfaceStatus = hobot_vaQuerySurfaceStatus;
    vtable->vaCreateContext = hobot_vaCreateContext;
    vtable->vaDestroyContext = hobot_vaDestroyContext;
    vtable->vaCreateBuffer = hobot_vaCreateBuffer;
    vtable->vaCreateBuffer2 = hobot_vaCreateBuffer2;
    vtable->vaBufferSetNumElements = hobot_vaBufferSetNumElements;
    vtable->vaMapBuffer = hobot_vaMapBuffer;
    vtable->vaUnmapBuffer = hobot_vaUnmapBuffer;
    vtable->vaDestroyBuffer = hobot_vaDestroyBuffer;
    vtable->vaBeginPicture = hobot_vaBeginPicture;
    vtable->vaRenderPicture = hobot_vaRenderPicture;
    vtable->vaEndPicture = hobot_vaEndPicture;
    vtable->vaSyncSurface = hobot_vaSyncSurface;
    vtable->vaPutSurface = hobot_vaPutSurface;
    vtable->vaExportSurfaceHandle = hobot_vaExportSurfaceHandle;
    vtable->vaQueryImageFormats = hobot_vaQueryImageFormats;
    vtable->vaQuerySubpictureFormats = hobot_vaQuerySubpictureFormats;
    vtable->vaCreateSubpicture = hobot_vaCreateSubpicture;
    vtable->vaDestroySubpicture = hobot_vaDestroySubpicture;
    vtable->vaSetSubpictureImage = hobot_vaSetSubpictureImage;
    vtable->vaSetSubpictureChromakey = hobot_vaSetSubpictureChromakey;
    vtable->vaSetSubpictureGlobalAlpha = hobot_vaSetSubpictureGlobalAlpha;
    vtable->vaAssociateSubpicture = hobot_vaAssociateSubpicture;
    vtable->vaDeassociateSubpicture = hobot_vaDeassociateSubpicture;
    vtable->vaQueryDisplayAttributes = hobot_vaQueryDisplayAttributes;
    vtable->vaGetDisplayAttributes = hobot_vaGetDisplayAttributes;
    vtable->vaSetDisplayAttributes = hobot_vaSetDisplayAttributes;
    vtable->vaLockSurface = hobot_vaLockSurface;
    vtable->vaUnlockSurface = hobot_vaUnlockSurface;
    vtable->vaCreateImage = hobot_vaCreateImage;
    vtable->vaDestroyImage = hobot_vaDestroyImage;
    vtable->vaSetImagePalette = hobot_vaSetImagePalette;
    vtable->vaGetImage = hobot_vaGetImage;
    vtable->vaPutImage = hobot_vaPutImage;
    vtable->vaDeriveImage = hobot_vaDeriveImage;

    return VA_STATUS_SUCCESS;
}

VAStatus __vaDriverInit_1_0(VADriverContextP ctx) {
    return hobot_init_driver(ctx);
}

VAStatus __vaDriverInit_0_32(VADriverContextP ctx) {
    return hobot_init_driver(ctx);
}
