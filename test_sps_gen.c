#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t *buf;
    int bit_pos;
} BitWriter;

static void put_bit(BitWriter *bw, int bit) {
    int byte_idx = bw->bit_pos / 8;
    int bit_idx = 7 - (bw->bit_pos % 8);
    if (bit)
        bw->buf[byte_idx] |= (1 << bit_idx);
    else
        bw->buf[byte_idx] &= ~(1 << bit_idx);
    bw->bit_pos++;
}

static void put_bits(BitWriter *bw, uint32_t val, int n) {
    for (int i = n - 1; i >= 0; i--) {
        put_bit(bw, (val >> i) & 1);
    }
}

static void put_ue(BitWriter *bw, uint32_t val) {
    uint32_t temp = val + 1;
    int leading_zeros = 0;
    while ((temp >> (leading_zeros + 1)) != 0) {
        leading_zeros++;
    }
    for (int i = 0; i < leading_zeros; i++) put_bit(bw, 0);
    put_bit(bw, 1);
    for (int i = leading_zeros - 1; i >= 0; i--) {
        put_bit(bw, (temp >> i) & 1);
    }
}

static void put_se(BitWriter *bw, int32_t val) {
    uint32_t ue = (val <= 0) ? (-val * 2) : (val * 2 - 1);
    put_ue(bw, ue);
}

int main() {
    uint8_t sps_buf[64] = {0};
    BitWriter bw = {sps_buf, 0};

    // NAL Header for SPS: forbidden_zero(1)=0, nal_ref_idc(2)=3, nal_unit_type(5)=7 -> 0x67
    put_bits(&bw, 0x67, 8);
    put_bits(&bw, 100, 8); // profile_idc (High)
    put_bits(&bw, 0, 8);   // constraint flags
    put_bits(&bw, 40, 8);  // level_idc (4.0)
    put_ue(&bw, 0);        // seq_parameter_set_id

    put_ue(&bw, 1);        // chroma_format_idc (1=4:2:0)
    put_ue(&bw, 0);        // bit_depth_luma_minus8
    put_ue(&bw, 0);        // bit_depth_chroma_minus8
    put_bits(&bw, 0, 1);   // qpprime_y_zero_transform_bypass_flag
    put_bits(&bw, 0, 1);   // seq_scaling_matrix_present_flag

    put_ue(&bw, 0);        // log2_max_frame_num_minus4
    put_ue(&bw, 0);        // pic_order_cnt_type
    put_ue(&bw, 0);        // log2_max_pic_order_cnt_lsb_minus4
    put_ue(&bw, 4);        // max_num_ref_frames
    put_bits(&bw, 0, 1);   // gaps_in_frame_num_value_allowed_flag

    put_ue(&bw, 119);      // pic_width_in_mbs_minus1 (120*16 = 1920)
    put_ue(&bw, 67);       // pic_height_in_map_units_minus1 (68*16 = 1088)
    put_bits(&bw, 1, 1);   // frame_mbs_only_flag
    put_bits(&bw, 1, 1);   // direct_8x8_inference_flag
    put_bits(&bw, 1, 1);   // frame_cropping_flag
    put_ue(&bw, 0);        // crop_left
    put_ue(&bw, 0);        // crop_right
    put_ue(&bw, 0);        // crop_top
    put_ue(&bw, 4);        // crop_bottom (1088 - 8 = 1080)
    put_bits(&bw, 0, 1);   // vui_parameters_present_flag
    put_bits(&bw, 1, 1);   // rbsp_stop_one_bit

    int bytes = (bw.bit_pos + 7) / 8;
    printf("Generated SPS (%d bytes): ", bytes);
    for (int i = 0; i < bytes; i++) printf("%02x ", sps_buf[i]);
    printf("\n");
    return 0;
}
