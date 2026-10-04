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
#include <stddef.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <time.h>
#include <libdrm/drm_fourcc.h>
#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_drmcommon.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>
#include <va/va_enc_jpeg.h>
#include <va/va_dec_hevc.h>
#include <va/va_dec_jpeg.h>
#define HOBOT_DRIVER_BUILD 1
#include "../include/va/va_hobot.h"
#include <hb_media_codec.h>
#include <hb_media_error.h>
#include <hb_mem_mgr.h>
#include <stdarg.h>
#include <pthread.h>

static pthread_once_t va_trace_once = PTHREAD_ONCE_INIT;
static int va_trace_enabled;

static void va_trace_initialize(void) {
    va_trace_enabled = (getenv("LIBVA_TRACE") != NULL) ||
                       (getenv("LIBVA_DEBUG") != NULL);
}

static inline void va_trace(const char *fmt, ...) {
    if (pthread_once(&va_trace_once, va_trace_initialize) != 0 ||
        !va_trace_enabled)
        return;

    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[VA-TRACE] ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

#define HOBOT_VA_DRIVER_STR "D-Robotics RDK-X5 VPU VA-API Driver 0.4.0 (Decode + Encode)"
#define MAX_SURFACES 512
#define MAX_BUFFERS  4096
#define MAX_CONTEXTS 16
#define MAX_CONFIGS  64
#define MAX_CONFIG_ATTRIBUTES 16
#define MAX_IMAGES   512
#define HOBOT_WATCHDOG_FALLBACK_THRESHOLD 1
#define HOBOT_WATCHDOG_CLEAN_RESET_SEC 10
#define HOBOT_HEVC_LEVEL_IDC 153
#define HOBOT_HEVC_MAX_WIDTH 3840
#define HOBOT_HEVC_MAX_HEIGHT 2160

static VAProfile supported_profiles[] = {
    VAProfileH264ConstrainedBaseline,
    VAProfileH264Main,
    VAProfileH264High,
    VAProfileHEVCMain,
    VAProfileJPEGBaseline
};

#define NUM_SUPPORTED_PROFILES (sizeof(supported_profiles) / sizeof(supported_profiles[0]))

static int hobot_profile_supported(VAProfile profile) {
    for (size_t i = 0; i < NUM_SUPPORTED_PROFILES; i++) {
        if (supported_profiles[i] == profile) return 1;
    }
    return 0;
}

static int hobot_profile_resolution_supported(VAProfile profile,
                                               int width,
                                               int height) {
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return 0;
    if (profile == VAProfileHEVCMain)
        return width <= HOBOT_HEVC_MAX_WIDTH && height <= HOBOT_HEVC_MAX_HEIGHT;
    return 1;
}

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

typedef struct {
    const uint8_t *buf;
    size_t bit_count;
    size_t bit_pos;
} BitReader;

static int br_read_bits(BitReader *br, unsigned int count, uint32_t *value) {
    if (!br || !value || count > 32 || br->bit_pos > br->bit_count ||
        count > br->bit_count - br->bit_pos)
        return 0;

    uint32_t result = 0;
    for (unsigned int i = 0; i < count; i++) {
        size_t pos = br->bit_pos++;
        result = (result << 1) |
            ((br->buf[pos / 8] >> (7u - (unsigned int)(pos % 8))) & 1u);
    }
    *value = result;
    return 1;
}

static int br_skip_bits(BitReader *br, size_t count) {
    if (!br || br->bit_pos > br->bit_count || count > br->bit_count - br->bit_pos)
        return 0;
    br->bit_pos += count;
    return 1;
}

static int br_read_ue(BitReader *br, uint32_t *value,
                      size_t *start_pos, size_t *end_pos) {
    if (!br || !value)
        return 0;
    size_t start = br->bit_pos;
    unsigned int leading_zero_bits = 0;
    uint32_t bit;
    while (1) {
        if (!br_read_bits(br, 1, &bit))
            return 0;
        if (bit)
            break;
        if (++leading_zero_bits > 31)
            return 0;
    }

    uint32_t suffix = 0;
    if (leading_zero_bits && !br_read_bits(br, leading_zero_bits, &suffix))
        return 0;
    uint64_t decoded = ((UINT64_C(1) << leading_zero_bits) - 1u) + suffix;
    if (decoded > UINT32_MAX)
        return 0;
    *value = (uint32_t)decoded;
    if (start_pos)
        *start_pos = start;
    if (end_pos)
        *end_pos = br->bit_pos;
    return 1;
}

static int br_read_se(BitReader *br, int32_t *value) {
    uint32_t code_num;
    if (!value || !br_read_ue(br, &code_num, NULL, NULL))
        return 0;

    int64_t decoded = (code_num & 1u) ?
        ((int64_t)code_num + 1) / 2 : -(int64_t)(code_num / 2u);
    if (decoded < INT32_MIN || decoded > INT32_MAX)
        return 0;
    *value = (int32_t)decoded;
    return 1;
}

static int hobot_hevc_slice_header_offset_valid(
    const BitReader *br,
    const VASliceParameterBufferHEVC *slice
) {
    if (!br || !slice || slice->slice_data_byte_offset < 2u)
        return 0;
    size_t rbsp_header_bytes =
        (size_t)slice->slice_data_byte_offset - 2u;
    return rbsp_header_bytes <= SIZE_MAX / 8u &&
           br->bit_pos <= rbsp_header_bytes * 8u;
}

static size_t hobot_hevc_find_start_code(const uint8_t *data, size_t size,
                                          size_t from, size_t *prefix_size) {
    if (!data || !prefix_size || from > size)
        return SIZE_MAX;
    for (size_t i = from; i + 3 <= size; i++) {
        if (i + 4 <= size && data[i] == 0 && data[i + 1] == 0 &&
            data[i + 2] == 0 && data[i + 3] == 1) {
            *prefix_size = 4;
            return i;
        }
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            *prefix_size = 3;
            return i;
        }
    }
    return SIZE_MAX;
}

static int hobot_hevc_unescape_rbsp(const uint8_t *ebsp, size_t ebsp_size,
                                    uint8_t *rbsp, size_t capacity,
                                    size_t *rbsp_size) {
    if ((!ebsp && ebsp_size) || !rbsp || !rbsp_size)
        return 0;
    size_t out = 0;
    unsigned int zero_count = 0;
    for (size_t i = 0; i < ebsp_size; i++) {
        uint8_t byte = ebsp[i];
        if (zero_count == 2 && byte == 3) {
            zero_count = 0;
            continue;
        }
        if (out >= capacity)
            return 0;
        rbsp[out++] = byte;
        zero_count = byte == 0 ? zero_count + 1u : 0u;
    }
    *rbsp_size = out;
    return 1;
}

static int hobot_hevc_escape_rbsp(const uint8_t *rbsp, size_t rbsp_size,
                                  uint8_t *ebsp, size_t capacity,
                                  size_t *ebsp_size) {
    if ((!rbsp && rbsp_size) || !ebsp || !ebsp_size)
        return 0;
    size_t out = 0;
    unsigned int zero_count = 0;
    for (size_t i = 0; i < rbsp_size; i++) {
        uint8_t byte = rbsp[i];
        if (zero_count == 2 && byte <= 3) {
            if (out >= capacity)
                return 0;
            ebsp[out++] = 3;
            zero_count = 0;
        }
        if (out >= capacity)
            return 0;
        ebsp[out++] = byte;
        zero_count = byte == 0 ? zero_count + 1u : 0u;
    }
    *ebsp_size = out;
    return 1;
}

static int hobot_hevc_write_bit(uint8_t *data, size_t capacity_bits,
                                size_t *bit_pos, unsigned int bit) {
    if (!data || !bit_pos || *bit_pos >= capacity_bits)
        return 0;
    if (bit)
        data[*bit_pos / 8u] |= (uint8_t)(1u << (7u - (*bit_pos % 8u)));
    (*bit_pos)++;
    return 1;
}

static int hobot_hevc_write_ue(uint8_t *data, size_t capacity_bits,
                               size_t *bit_pos, uint32_t value) {
    uint64_t code = (uint64_t)value + 1u;
    unsigned int leading_zero_bits = 0;
    for (uint64_t temp = code; temp > 1u; temp >>= 1)
        leading_zero_bits++;
    for (unsigned int i = 0; i < leading_zero_bits; i++) {
        if (!hobot_hevc_write_bit(data, capacity_bits, bit_pos, 0))
            return 0;
    }
    for (int i = (int)leading_zero_bits; i >= 0; i--) {
        if (!hobot_hevc_write_bit(data, capacity_bits, bit_pos,
                                  (unsigned int)((code >> i) & 1u)))
            return 0;
    }
    return 1;
}

static int hobot_hevc_copy_bits(uint8_t *dst, size_t dst_capacity_bits,
                                size_t *dst_bit_pos, const uint8_t *src,
                                size_t src_bit_start, size_t bit_count) {
    if (!dst || !dst_bit_pos || !src || *dst_bit_pos > dst_capacity_bits ||
        bit_count > dst_capacity_bits - *dst_bit_pos ||
        src_bit_start > SIZE_MAX - bit_count)
        return 0;
    for (size_t i = 0; i < bit_count; i++) {
        size_t src_pos = src_bit_start + i;
        unsigned int bit = (src[src_pos / 8u] >>
                            (7u - (unsigned int)(src_pos % 8u))) & 1u;
        if (!hobot_hevc_write_bit(dst, dst_capacity_bits, dst_bit_pos, bit))
            return 0;
    }
    return 1;
}

static int hobot_hevc_parse_sps_dimensions(
    uint8_t *rbsp,
    size_t rbsp_size,
    uint32_t *width,
    uint32_t *height,
    size_t *width_start,
    size_t *width_end,
    size_t *height_start,
    size_t *height_end,
    size_t *conf_win_flag_start,
    uint32_t *chroma_format_idc,
    uint32_t *conf_win_flag,
    uint32_t crop_offsets[4]
)
{
    if (!rbsp || !width || !height || !width_start || !width_end ||
        !height_start || !height_end || !conf_win_flag_start ||
        !chroma_format_idc ||
        !conf_win_flag || !crop_offsets || rbsp_size > SIZE_MAX / 8u)
        return 0;

    BitReader br = {rbsp, rbsp_size * 8u, 0};
    uint32_t max_sub_layers_minus1;
    if (!br_skip_bits(&br, 4) ||
        !br_read_bits(&br, 3, &max_sub_layers_minus1) ||
        !br_skip_bits(&br, 1) || !br_skip_bits(&br, 96))
        return 0;

    uint32_t sub_layer_profile_present[7] = {0};
    uint32_t sub_layer_level_present[7] = {0};
    for (uint32_t i = 0; i < max_sub_layers_minus1; i++) {
        if (!br_read_bits(&br, 1, &sub_layer_profile_present[i]) ||
            !br_read_bits(&br, 1, &sub_layer_level_present[i]))
            return 0;
    }
    if (max_sub_layers_minus1 > 0 &&
        !br_skip_bits(&br, (size_t)(8u - max_sub_layers_minus1) * 2u))
        return 0;
    for (uint32_t i = 0; i < max_sub_layers_minus1; i++) {
        if (sub_layer_profile_present[i] && !br_skip_bits(&br, 88))
            return 0;
        if (sub_layer_level_present[i] && !br_skip_bits(&br, 8))
            return 0;
    }

    uint32_t ignored;
    if (!br_read_ue(&br, &ignored, NULL, NULL) ||
        !br_read_ue(&br, chroma_format_idc, NULL, NULL) ||
        *chroma_format_idc > 3 ||
        (*chroma_format_idc == 3 && !br_skip_bits(&br, 1)) ||
        !br_read_ue(&br, width, width_start, width_end) ||
        !br_read_ue(&br, height, height_start, height_end))
        return 0;
    *conf_win_flag_start = br.bit_pos;
    if (!br_read_bits(&br, 1, conf_win_flag))
        return 0;
    if (*conf_win_flag) {
        for (size_t i = 0; i < 4; i++) {
            if (!br_read_ue(&br, &crop_offsets[i], NULL, NULL))
                return 0;
        }
    }
    return *width > 0 && *height > 0;
}

static int hobot_hevc_same_ctu_grid(
    uint32_t coded_width,
    uint32_t coded_height,
    uint32_t visible_width,
    uint32_t visible_height,
    const VAEncSequenceParameterBufferHEVC *seq
)
{
    if (!seq)
        return 0;
    unsigned int ctb_log2 = 3u + seq->log2_min_luma_coding_block_size_minus3 +
                            seq->log2_diff_max_min_luma_coding_block_size;
    if (ctb_log2 > 6)
        return 0;
    uint64_t ctb_size = UINT64_C(1) << ctb_log2;
    uint64_t coded_columns = ((uint64_t)coded_width + ctb_size - 1u) / ctb_size;
    uint64_t visible_columns = ((uint64_t)visible_width + ctb_size - 1u) / ctb_size;
    uint64_t coded_rows = ((uint64_t)coded_height + ctb_size - 1u) / ctb_size;
    uint64_t visible_rows = ((uint64_t)visible_height + ctb_size - 1u) / ctb_size;
    return coded_columns == visible_columns && coded_rows == visible_rows;
}

static int hobot_hevc_patch_sps_nal(
    const uint8_t *nal,
    size_t nal_size,
    const VAEncSequenceParameterBufferHEVC *seq,
    uint32_t requested_width,
    uint32_t requested_height,
    uint8_t **patched_nal,
    size_t *patched_nal_size
)
{
    if (!nal || nal_size < 3 || !seq || !patched_nal || !patched_nal_size ||
        ((nal[0] >> 1) & 0x3fu) != 33)
        return 0;
    *patched_nal = NULL;
    *patched_nal_size = nal_size;

    size_t ebsp_size = nal_size - 2u;
    uint8_t *rbsp = malloc(ebsp_size ? ebsp_size : 1u);
    if (!rbsp)
        return 0;
    size_t rbsp_size = 0;
    if (!hobot_hevc_unescape_rbsp(nal + 2, ebsp_size, rbsp, ebsp_size,
                                  &rbsp_size)) {
        free(rbsp);
        return 0;
    }

    uint32_t coded_width, coded_height, chroma_format_idc, conf_win_flag;
    uint32_t crop_offsets[4] = {0};
    size_t width_start, width_end, height_start, height_end;
    size_t conf_win_flag_start;
    if (!hobot_hevc_parse_sps_dimensions(
            rbsp, rbsp_size, &coded_width, &coded_height,
            &width_start, &width_end, &height_start, &height_end,
            &conf_win_flag_start, &chroma_format_idc, &conf_win_flag,
            crop_offsets)) {
        free(rbsp);
        return 0;
    }

    uint32_t visible_width = coded_width;
    uint32_t visible_height = coded_height;
    if (conf_win_flag) {
        uint32_t sub_width = chroma_format_idc == 1 || chroma_format_idc == 2 ? 2u : 1u;
        uint32_t sub_height = chroma_format_idc == 1 ? 2u : 1u;
        uint64_t crop_width = ((uint64_t)crop_offsets[0] + crop_offsets[1]) * sub_width;
        uint64_t crop_height = ((uint64_t)crop_offsets[2] + crop_offsets[3]) * sub_height;
        if (crop_width >= coded_width || crop_height >= coded_height) {
            free(rbsp);
            return 0;
        }
        visible_width -= (uint32_t)crop_width;
        visible_height -= (uint32_t)crop_height;
    }

    if (visible_width == requested_width && visible_height == requested_height) {
        va_trace("HEVC SPS geometry already matches visible size: coded=%ux%u visible=%ux%u",
                 coded_width, coded_height, visible_width, visible_height);
        free(rbsp);
        return 1;
    }
    va_trace("HEVC SPS crop requested: coded=%ux%u visible=%ux%u target=%ux%u conf_win=%u",
             coded_width, coded_height, visible_width, visible_height,
             requested_width, requested_height, conf_win_flag);
    uint32_t crop_width = coded_width >= requested_width ?
        coded_width - requested_width : UINT32_MAX;
    uint32_t crop_height = coded_height >= requested_height ?
        coded_height - requested_height : UINT32_MAX;
    if (conf_win_flag || crop_width >= 16u || crop_height >= 16u ||
        (requested_width & 1u) != 0 || (requested_height & 1u) != 0 ||
        chroma_format_idc != 1 || crop_width % 2u != 0 ||
        crop_height % 2u != 0 ||
        !hobot_hevc_same_ctu_grid(coded_width, coded_height,
                                   requested_width, requested_height, seq)) {
        free(rbsp);
        return 0;
    }

    uint32_t sub_width = 2u;
    uint32_t sub_height = 2u;
    uint32_t crop_offsets_to_write[4] = {
        0,
        crop_width / sub_width,
        0,
        crop_height / sub_height
    };

    size_t rbsp_nonzero_size = rbsp_size;
    while (rbsp_nonzero_size > 0 && rbsp[rbsp_nonzero_size - 1u] == 0)
        rbsp_nonzero_size--;
    if (rbsp_nonzero_size == 0) {
        free(rbsp);
        return 0;
    }
    uint8_t last_byte = rbsp[rbsp_nonzero_size - 1u];
    unsigned int trailing_zero_bits = 0;
    while (((last_byte >> trailing_zero_bits) & 1u) == 0u)
        trailing_zero_bits++;
    size_t rbsp_payload_bits = (rbsp_nonzero_size - 1u) * 8u +
                               (8u - trailing_zero_bits);
    size_t trailing_zero_bytes = rbsp_size - rbsp_nonzero_size;
    if (conf_win_flag_start >= rbsp_payload_bits ||
        conf_win_flag_start + 1u > rbsp_payload_bits) {
        free(rbsp);
        return 0;
    }

    size_t inserted_bits = 1u;
    for (size_t i = 0; i < 4; i++) {
        uint64_t code = (uint64_t)crop_offsets_to_write[i] + 1u;
        size_t leading_zero_bits = 0;
        for (uint64_t temp = code; temp > 1u; temp >>= 1)
            leading_zero_bits++;
        inserted_bits += 2u * leading_zero_bits + 1u;
    }
    if (rbsp_payload_bits > SIZE_MAX - inserted_bits + 1u) {
        free(rbsp);
        return 0;
    }
    size_t patched_payload_bits = rbsp_payload_bits + inserted_bits - 1u;
    if (patched_payload_bits > SIZE_MAX - 7u) {
        free(rbsp);
        return 0;
    }
    size_t patched_rbsp_size = (patched_payload_bits + 7u) / 8u;
    if (patched_rbsp_size > SIZE_MAX - trailing_zero_bytes ||
        patched_rbsp_size + trailing_zero_bytes > (SIZE_MAX - 2u) / 2u) {
        free(rbsp);
        return 0;
    }
    size_t patched_ebsp_capacity = patched_rbsp_size * 2u +
                                   trailing_zero_bytes + 2u;
    uint8_t *patched_rbsp = calloc(patched_rbsp_size ? patched_rbsp_size : 1u, 1u);
    uint8_t *patched_ebsp = malloc(patched_ebsp_capacity);
    if (!patched_rbsp || !patched_ebsp) {
        free(patched_rbsp);
        free(patched_ebsp);
        free(rbsp);
        return 0;
    }

    size_t bit_pos = 0;
    size_t patched_capacity_bits = patched_rbsp_size * 8u;
    int success = hobot_hevc_copy_bits(
                      patched_rbsp, patched_capacity_bits, &bit_pos,
                      rbsp, 0, conf_win_flag_start) &&
                  hobot_hevc_write_bit(patched_rbsp, patched_capacity_bits,
                                       &bit_pos, 1u);
    for (size_t i = 0; success && i < 4; i++) {
        success = hobot_hevc_write_ue(patched_rbsp, patched_capacity_bits,
                                      &bit_pos, crop_offsets_to_write[i]);
    }
    success = success && hobot_hevc_copy_bits(
                  patched_rbsp, patched_capacity_bits, &bit_pos,
                  rbsp, conf_win_flag_start + 1u,
                  rbsp_payload_bits - conf_win_flag_start - 1u) &&
              bit_pos == patched_payload_bits;

    size_t patched_ebsp_size = 0;
    if (success)
        success = hobot_hevc_escape_rbsp(
            patched_rbsp, patched_rbsp_size, patched_ebsp,
            patched_ebsp_capacity, &patched_ebsp_size);
    if (success && patched_ebsp_size <= SIZE_MAX - trailing_zero_bytes - 2u) {
        size_t result_size = 2u + patched_ebsp_size + trailing_zero_bytes;
        uint8_t *result = malloc(result_size);
        if (!result) {
            success = 0;
        } else {
            memcpy(result, nal, 2u);
            memcpy(result + 2u, patched_ebsp, patched_ebsp_size);
            memset(result + 2u + patched_ebsp_size, 0,
                   trailing_zero_bytes);
            *patched_nal = result;
            *patched_nal_size = result_size;
        }
    } else {
        success = 0;
    }
    va_trace("HEVC SPS conformance-window crop %s: target=%ux%u right=%u bottom=%u",
             success ? "applied" : "rejected", requested_width,
             requested_height, crop_offsets_to_write[1],
             crop_offsets_to_write[3]);
    free(patched_rbsp);
    free(patched_ebsp);
    free(rbsp);
    return success;
}

static int hobot_hevc_patch_output_sps(
    uint8_t *data,
    size_t *size,
    size_t capacity,
    const VAEncSequenceParameterBufferHEVC *seq,
    uint32_t requested_width,
    uint32_t requested_height,
    int *found_sps
)
{
    if (!data || !size || *size > capacity || !seq || !found_sps)
        return 0;
    *found_sps = 0;
    size_t scan = 0;
    while (scan < *size) {
        size_t prefix_size = 0;
        size_t start = hobot_hevc_find_start_code(data, *size, scan,
                                                   &prefix_size);
        if (start == SIZE_MAX)
            break;
        size_t nal_start = start + prefix_size;
        size_t next_prefix_size = 0;
        size_t next = hobot_hevc_find_start_code(data, *size, nal_start,
                                                  &next_prefix_size);
        (void)next_prefix_size;
        size_t nal_end = next == SIZE_MAX ? *size : next;
        if (nal_end > nal_start && nal_end - nal_start >= 2u &&
            ((data[nal_start] >> 1) & 0x3fu) == 33u) {
            *found_sps = 1;
            uint8_t *patched_nal = NULL;
            size_t patched_nal_size = 0;
            if (!hobot_hevc_patch_sps_nal(
                    data + nal_start, nal_end - nal_start, seq,
                    requested_width, requested_height, &patched_nal,
                    &patched_nal_size))
                return 0;
            if (patched_nal) {
                size_t original_nal_size = nal_end - nal_start;
                size_t unchanged_size = *size - original_nal_size;
                if (unchanged_size > capacity ||
                    patched_nal_size > capacity - unchanged_size) {
                    free(patched_nal);
                    return 0;
                }
                size_t new_size = unchanged_size + patched_nal_size;
                memmove(data + nal_start + patched_nal_size, data + nal_end,
                        *size - nal_end);
                memcpy(data + nal_start, patched_nal, patched_nal_size);
                free(patched_nal);
                *size = new_size;
                nal_end = nal_start + patched_nal_size;
            }
        }
        if (nal_end >= *size)
            break;
        scan = nal_end;
    }
    return 1;
}

static int hobot_h264_rbsp_trailing_bits(BitReader br)
{
    uint32_t stop_bit;
    if (!br_read_bits(&br, 1, &stop_bit) || stop_bit != 1)
        return 0;
    while (br.bit_pos < br.bit_count) {
        uint32_t padding_bit;
        if (!br_read_bits(&br, 1, &padding_bit) || padding_bit != 0)
            return 0;
    }
    return 1;
}

static int hobot_h264_strip_constrained_baseline_pps_extension(
    const uint8_t *nal,
    size_t nal_size,
    uint8_t **rewritten_nal,
    size_t *rewritten_nal_size,
    int *changed
)
{
    if (!nal || nal_size < 2u || !rewritten_nal || !rewritten_nal_size ||
        !changed || (nal[0] & 0x80u) != 0 || (nal[0] & 0x60u) == 0 ||
        (nal[0] & 0x1fu) != 8u || nal_size - 1u > 1024u)
        return 0;

    *rewritten_nal = NULL;
    *rewritten_nal_size = nal_size;
    *changed = 0;
    uint8_t rbsp[1024];
    size_t rbsp_size = 0;
    if (!hobot_hevc_unescape_rbsp(nal + 1u, nal_size - 1u,
                                  rbsp, sizeof(rbsp), &rbsp_size) ||
        rbsp_size == 0 || rbsp_size > SIZE_MAX / 8u)
        return 0;

    BitReader br = {rbsp, rbsp_size * 8u, 0};
    uint32_t pps_id, sps_id, entropy_coding_mode, ignored_flag;
    uint32_t slice_groups, refs_l0, refs_l1, weighted_pred, weighted_bipred;
    uint32_t deblocking, constrained_intra, redundant_pic_cnt;
    int32_t pic_init_qp, pic_init_qs, chroma_qp_offset;
    if (!br_read_ue(&br, &pps_id, NULL, NULL) || pps_id > 255u ||
        !br_read_ue(&br, &sps_id, NULL, NULL) || sps_id > 31u ||
        !br_read_bits(&br, 1, &entropy_coding_mode) || entropy_coding_mode != 0 ||
        !br_read_bits(&br, 1, &ignored_flag) ||
        !br_read_ue(&br, &slice_groups, NULL, NULL) || slice_groups != 0 ||
        !br_read_ue(&br, &refs_l0, NULL, NULL) || refs_l0 > 31u ||
        !br_read_ue(&br, &refs_l1, NULL, NULL) || refs_l1 > 31u ||
        !br_read_bits(&br, 1, &weighted_pred) || weighted_pred != 0 ||
        !br_read_bits(&br, 2, &weighted_bipred) || weighted_bipred != 0 ||
        !br_read_se(&br, &pic_init_qp) || pic_init_qp < -26 || pic_init_qp > 25 ||
        !br_read_se(&br, &pic_init_qs) || pic_init_qs < -26 || pic_init_qs > 25 ||
        !br_read_se(&br, &chroma_qp_offset) ||
        chroma_qp_offset < -12 || chroma_qp_offset > 12 ||
        !br_read_bits(&br, 1, &deblocking) ||
        !br_read_bits(&br, 1, &constrained_intra) ||
        !br_read_bits(&br, 1, &redundant_pic_cnt) || redundant_pic_cnt != 0)
        return 0;

    size_t prefix_bits = br.bit_pos;
    if (hobot_h264_rbsp_trailing_bits(br))
        return 1;

    uint32_t transform_8x8, scaling_matrix;
    int32_t second_chroma_qp_offset;
    if (!br_read_bits(&br, 1, &transform_8x8) || transform_8x8 != 0 ||
        !br_read_bits(&br, 1, &scaling_matrix) || scaling_matrix != 0 ||
        !br_read_se(&br, &second_chroma_qp_offset) ||
        second_chroma_qp_offset != chroma_qp_offset ||
        !hobot_h264_rbsp_trailing_bits(br))
        return 0;

    uint8_t stripped_rbsp[1024] = {0};
    size_t stripped_bit_pos = 0;
    size_t capacity_bits = sizeof(stripped_rbsp) * 8u;
    if (!hobot_hevc_copy_bits(stripped_rbsp, capacity_bits,
                              &stripped_bit_pos, rbsp, 0, prefix_bits) ||
        !hobot_hevc_write_bit(stripped_rbsp, capacity_bits,
                              &stripped_bit_pos, 1u))
        return 0;
    while (stripped_bit_pos % 8u != 0) {
        if (!hobot_hevc_write_bit(stripped_rbsp, capacity_bits,
                                  &stripped_bit_pos, 0u))
            return 0;
    }

    size_t stripped_rbsp_size = stripped_bit_pos / 8u;
    uint8_t stripped_ebsp[1024];
    size_t stripped_ebsp_size = 0;
    if (!hobot_hevc_escape_rbsp(stripped_rbsp, stripped_rbsp_size,
                                stripped_ebsp, sizeof(stripped_ebsp),
                                &stripped_ebsp_size) ||
        stripped_ebsp_size == SIZE_MAX)
        return 0;

    uint8_t *result = malloc(stripped_ebsp_size + 1u);
    if (!result)
        return 0;
    result[0] = nal[0];
    memcpy(result + 1u, stripped_ebsp, stripped_ebsp_size);
    *rewritten_nal = result;
    *rewritten_nal_size = stripped_ebsp_size + 1u;
    *changed = 1;
    return 1;
}

static int hobot_h264_patch_constrained_baseline_headers(
    uint8_t *data,
    size_t *size,
    size_t capacity,
    int require_headers,
    int *found_sps,
    int *found_pps
)
{
    if (!data || !size || *size > capacity || !found_sps || !found_pps)
        return 0;
    *found_sps = 0;
    *found_pps = 0;

    size_t scan = 0;
    while (scan < *size) {
        size_t prefix_size = 0;
        size_t start = hobot_hevc_find_start_code(data, *size, scan,
                                                   &prefix_size);
        if (start == SIZE_MAX)
            break;
        size_t nal_start = start + prefix_size;
        size_t next_prefix_size = 0;
        size_t next = hobot_hevc_find_start_code(data, *size, nal_start,
                                                  &next_prefix_size);
        (void)next_prefix_size;
        size_t nal_end = next == SIZE_MAX ? *size : next;
        if (nal_end > nal_start && (data[nal_start] & 0x1fu) == 7u) {
            if ((data[nal_start] & 0x80u) != 0 ||
                (data[nal_start] & 0x60u) == 0 ||
                nal_end - nal_start < 4u || data[nal_start + 1u] != 66u ||
                (data[nal_start + 2u] & 0x03u) != 0)
                return 0;
            data[nal_start + 2u] |= 0x40u;
            *found_sps = 1;
        } else if (nal_end > nal_start &&
                   (data[nal_start] & 0x1fu) == 8u) {
            uint8_t *rewritten_nal = NULL;
            size_t rewritten_nal_size = 0;
            int changed = 0;
            if (!hobot_h264_strip_constrained_baseline_pps_extension(
                    data + nal_start, nal_end - nal_start, &rewritten_nal,
                    &rewritten_nal_size, &changed))
                return 0;
            *found_pps = 1;
            if (changed) {
                size_t original_nal_size = nal_end - nal_start;
                size_t unchanged_size = *size - original_nal_size;
                if (unchanged_size > capacity ||
                    rewritten_nal_size > capacity - unchanged_size) {
                    free(rewritten_nal);
                    return 0;
                }
                size_t new_size = unchanged_size + rewritten_nal_size;
                memmove(data + nal_start + rewritten_nal_size, data + nal_end,
                        *size - nal_end);
                memcpy(data + nal_start, rewritten_nal, rewritten_nal_size);
                free(rewritten_nal);
                *size = new_size;
                nal_end = nal_start + rewritten_nal_size;
            }
        }
        if (nal_end >= *size)
            break;
        scan = nal_end;
    }
    return !require_headers || (*found_sps && *found_pps);
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

static int generate_h264_sps(VAPictureParameterBufferH264 *pic, VAProfile profile,
                             uint8_t *out, int max_len) {
    uint8_t rbsp[256] = {0};
    BitWriter bw = {rbsp, 0};
    uint8_t profile_idc;
    uint8_t constraint_flags = 0;
    int high_profile_syntax;

    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
        profile_idc = 66;
        constraint_flags = 0x40; /* constraint_set1_flag */
        high_profile_syntax = 0;
        break;
    case VAProfileH264Main:
        profile_idc = 77;
        high_profile_syntax = 0;
        break;
    case VAProfileH264High:
        profile_idc = 100;
        high_profile_syntax = 1;
        break;
    default:
        return 0;
    }

    bw_put_bits(&bw, profile_idc, 8);
    bw_put_bits(&bw, constraint_flags, 8);
    bw_put_bits(&bw, 51, 8);   // Level 5.1 (supports up to 4K / 1080p 120fps)
    bw_put_ue(&bw, 0);         // seq_parameter_set_id

    if (high_profile_syntax) {
        bw_put_ue(&bw, pic->seq_fields.bits.chroma_format_idc);
        bw_put_ue(&bw, pic->bit_depth_luma_minus8);
        bw_put_ue(&bw, pic->bit_depth_chroma_minus8);
        bw_put_bits(&bw, 0, 1);    // qpprime_y_zero_transform_bypass_flag
        bw_put_bits(&bw, 0, 1);    // seq_scaling_matrix_present_flag
    }

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

static int hobot_h264_picture_parameters_supported(
    const VAPictureParameterBufferH264 *pic,
    VAProfile profile,
    int context_width,
    int context_height
) {
    int has_slice_groups;

    if (!pic || context_width <= 0 || context_height <= 0 ||
        context_width > 4096 || context_height > 4096) {
        return 0;
    }

    unsigned int height_alignment =
        pic->seq_fields.bits.frame_mbs_only_flag ? 16u : 32u;
    unsigned int max_width_mbs = ((unsigned int)context_width + 15u) / 16u;
    unsigned int max_height_map_units =
        ((unsigned int)context_height + height_alignment - 1u) /
        height_alignment;

    /* libva deprecates this field but still exposes it in the decode API. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    has_slice_groups = pic->num_slice_groups_minus1 != 0;
#pragma GCC diagnostic pop

    if (pic->picture_width_in_mbs_minus1 + 1u > max_width_mbs ||
        pic->picture_height_in_mbs_minus1 + 1u > max_height_map_units ||
        pic->num_ref_frames > 16 ||
        pic->seq_fields.bits.chroma_format_idc > 1 ||
        (profile == VAProfileH264High &&
         pic->seq_fields.bits.chroma_format_idc != 1) ||
        pic->seq_fields.bits.residual_colour_transform_flag ||
        pic->bit_depth_luma_minus8 != 0 || pic->bit_depth_chroma_minus8 != 0 ||
        pic->seq_fields.bits.pic_order_cnt_type == 1 ||
        pic->seq_fields.bits.pic_order_cnt_type > 2 ||
        pic->seq_fields.bits.log2_max_frame_num_minus4 > 12 ||
        has_slice_groups ||
        pic->pic_fields.bits.redundant_pic_cnt_present_flag) {
        return 0;
    }

    if (profile == VAProfileH264ConstrainedBaseline &&
        (!pic->seq_fields.bits.frame_mbs_only_flag ||
         pic->pic_fields.bits.entropy_coding_mode_flag ||
         pic->pic_fields.bits.weighted_pred_flag ||
         pic->pic_fields.bits.weighted_bipred_idc != 0 ||
         pic->pic_fields.bits.transform_8x8_mode_flag)) {
        return 0;
    }

    /* VA's decode picture structure has no POC type-1 offset-cycle fields. */
    if (pic->seq_fields.bits.pic_order_cnt_type == 0 &&
        pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 > 12) {
        return 0;
    }
    return 1;
}

static int hobot_hevc_picture_parameters_supported(
    const VAPictureParameterBufferHEVC *pic,
    int context_width,
    int context_height
) {
    if (!pic || context_width <= 0 || context_height <= 0)
        return 0;
    if (pic->num_short_term_ref_pic_sets > 64 ||
        (pic->num_short_term_ref_pic_sets > 2 &&
         (!pic->slice_parsing_fields.bits.IdrPicFlag ||
          !pic->slice_parsing_fields.bits.RapPicFlag ||
          !pic->slice_parsing_fields.bits.IntraPicFlag ||
          pic->st_rps_bits != 0)) ||
        (pic->num_short_term_ref_pic_sets == 1 &&
         pic->st_rps_bits > 2048u)) {
        va_trace("HEVC SPS RPS count is outside the supported picture subset: count=%u",
                 pic->num_short_term_ref_pic_sets);
        return 0;
    }
    if (pic->num_short_term_ref_pic_sets == 2 && pic->st_rps_bits != 0)
        return 0;
    if (context_width > UINT16_MAX || context_height > UINT16_MAX ||
        pic->pic_width_in_luma_samples != (uint16_t)context_width ||
        pic->pic_height_in_luma_samples != (uint16_t)context_height ||
        pic->pic_fields.bits.chroma_format_idc != 1 ||
        pic->pic_fields.bits.separate_colour_plane_flag ||
        pic->bit_depth_luma_minus8 != 0 || pic->bit_depth_chroma_minus8 != 0 ||
        pic->pic_fields.bits.scaling_list_enabled_flag ||
        pic->pic_fields.bits.pcm_enabled_flag ||
        pic->pic_fields.bits.tiles_enabled_flag ||
        pic->pic_fields.bits.entropy_coding_sync_enabled_flag ||
        pic->slice_parsing_fields.bits.long_term_ref_pics_present_flag ||
        pic->num_long_term_ref_pic_sps != 0 ||
        pic->pic_fields.bits.ReservedBits != 0 ||
        pic->slice_parsing_fields.bits.ReservedBits != 0 ||
        pic->sps_max_dec_pic_buffering_minus1 > 15 ||
        pic->log2_max_pic_order_cnt_lsb_minus4 > 12 ||
        pic->log2_min_luma_coding_block_size_minus3 > 3 ||
        pic->log2_diff_max_min_luma_coding_block_size > 3 ||
        (unsigned int)pic->log2_min_luma_coding_block_size_minus3 +
            pic->log2_diff_max_min_luma_coding_block_size > 3 ||
        pic->log2_min_transform_block_size_minus2 > 3 ||
        pic->log2_diff_max_min_transform_block_size > 3 ||
        (unsigned int)pic->log2_min_transform_block_size_minus2 +
            pic->log2_diff_max_min_transform_block_size > 3 ||
        pic->max_transform_hierarchy_depth_inter > 5 ||
        pic->max_transform_hierarchy_depth_intra > 5 ||
        pic->num_ref_idx_l0_default_active_minus1 > 14 ||
        pic->num_ref_idx_l1_default_active_minus1 > 14 ||
        pic->num_extra_slice_header_bits > 7 ||
        pic->init_qp_minus26 < -26 || pic->init_qp_minus26 > 25 ||
        pic->pps_cb_qp_offset < -12 || pic->pps_cb_qp_offset > 12 ||
        pic->pps_cr_qp_offset < -12 || pic->pps_cr_qp_offset > 12 ||
        pic->pps_beta_offset_div2 < -6 || pic->pps_beta_offset_div2 > 6 ||
        pic->pps_tc_offset_div2 < -6 || pic->pps_tc_offset_div2 > 6 ||
        pic->log2_parallel_merge_level_minus2 > 4)
        return 0;

    for (size_t i = 0; i < sizeof(pic->va_reserved) / sizeof(pic->va_reserved[0]); i++) {
        if (pic->va_reserved[i] != 0)
            return 0;
    }
    for (size_t i = 0; i < sizeof(pic->CurrPic.va_reserved) /
                            sizeof(pic->CurrPic.va_reserved[0]); i++) {
        if (pic->CurrPic.va_reserved[i] != 0)
            return 0;
    }
    for (size_t frame = 0; frame < 15; frame++) {
        for (size_t i = 0; i < sizeof(pic->ReferenceFrames[frame].va_reserved) /
                                sizeof(pic->ReferenceFrames[frame].va_reserved[0]); i++) {
            if (pic->ReferenceFrames[frame].va_reserved[i] != 0)
                return 0;
        }
    }
    return 1;
}

static int hobot_hevc_slice_parameter_fields_supported(
    const VASliceParameterBufferHEVC *slice
) {
    if (!slice || slice->slice_data_flag != VA_SLICE_DATA_FLAG_ALL ||
        slice->slice_data_size == 0 ||
        slice->LongSliceFlags.fields.dependent_slice_segment_flag ||
        slice->LongSliceFlags.fields.color_plane_id != 0 ||
        slice->LongSliceFlags.fields.slice_type > 2 ||
        slice->num_entry_point_offsets != 0 ||
        slice->slice_data_byte_offset > slice->slice_data_size ||
        slice->LongSliceFlags.fields.reserved != 0)
        return 0;
    for (size_t i = 0; i < sizeof(slice->va_reserved) / sizeof(slice->va_reserved[0]); i++) {
        if (slice->va_reserved[i] != 0)
            return 0;
    }
    return 1;
}

typedef struct {
    uint64_t picture_ctb_count;
    size_t slice_count;
    size_t next_slice;
    uint32_t previous_slice_address;
} HobotHevcSliceSequence;

static int hobot_hevc_slice_sequence_init(
    const VAPictureParameterBufferHEVC *picture,
    size_t slice_count,
    HobotHevcSliceSequence *sequence
) {
    if (!picture || !sequence || slice_count == 0 ||
        picture->pic_width_in_luma_samples == 0 ||
        picture->pic_height_in_luma_samples == 0 ||
        picture->log2_min_luma_coding_block_size_minus3 > 3 ||
        picture->log2_diff_max_min_luma_coding_block_size > 3 ||
        (unsigned int)picture->log2_min_luma_coding_block_size_minus3 +
            picture->log2_diff_max_min_luma_coding_block_size > 3)
        return 0;

    unsigned int ctb_log2 = 3u +
        picture->log2_min_luma_coding_block_size_minus3 +
        picture->log2_diff_max_min_luma_coding_block_size;
    uint64_t ctb_size = UINT64_C(1) << ctb_log2;
    uint64_t ctb_columns =
        ((uint64_t)picture->pic_width_in_luma_samples + ctb_size - 1u) /
        ctb_size;
    uint64_t ctb_rows =
        ((uint64_t)picture->pic_height_in_luma_samples + ctb_size - 1u) /
        ctb_size;
    if (ctb_columns == 0 || ctb_rows == 0 ||
        ctb_columns > UINT64_MAX / ctb_rows)
        return 0;

    sequence->picture_ctb_count = ctb_columns * ctb_rows;
    sequence->slice_count = slice_count;
    sequence->next_slice = 0;
    sequence->previous_slice_address = 0;
    return sequence->picture_ctb_count != 0;
}

static int hobot_hevc_slice_sequence_add(
    HobotHevcSliceSequence *sequence,
    const VASliceParameterBufferHEVC *slice,
    size_t slice_data_buffer_size
) {
    if (!sequence || !slice || sequence->next_slice >= sequence->slice_count ||
        !hobot_hevc_slice_parameter_fields_supported(slice) ||
        slice->slice_data_offset > slice_data_buffer_size ||
        slice->slice_data_size >
            slice_data_buffer_size - slice->slice_data_offset ||
        slice->slice_segment_address >= sequence->picture_ctb_count)
        return 0;

    size_t index = sequence->next_slice;
    if ((index == 0 && slice->slice_segment_address != 0) ||
        (index > 0 &&
         slice->slice_segment_address <= sequence->previous_slice_address) ||
        !!slice->LongSliceFlags.fields.LastSliceOfPic !=
            (index + 1 == sequence->slice_count))
        return 0;

    sequence->previous_slice_address = slice->slice_segment_address;
    sequence->next_slice++;
    return 1;
}

static int hobot_hevc_slice_sequence_complete(
    const HobotHevcSliceSequence *sequence
) {
    return sequence && sequence->next_slice == sequence->slice_count;
}

static int hobot_hevc_read_weight_value(BitReader *br, int32_t minimum,
                                         int32_t maximum) {
    int32_t value;
    return br_read_se(br, &value) && value >= minimum && value <= maximum;
}

static int hobot_hevc_read_chroma_weight_component(BitReader *br) {
    return hobot_hevc_read_weight_value(br, -128, 127) &&
           hobot_hevc_read_weight_value(br, -512, 511);
}

typedef struct {
    uint32_t entry_count;
    uint32_t current_count;
    int32_t poc[15];
    uint32_t flags[15];
    uint8_t used[15];
    uint8_t reference_index[15];
} HobotHevcInlineRps;

static int hobot_hevc_inline_rps_supported(BitReader *br,
                                           const VAPictureParameterBufferHEVC *pic,
                                           unsigned int slice_type,
                                           HobotHevcInlineRps *rps) {
    if (!br || !pic || !rps || pic->num_short_term_ref_pic_sets > 1)
        return 0;

    memset(rps, 0, sizeof(*rps));
    memset(rps->reference_index, 0xff, sizeof(rps->reference_index));
    size_t start = br->bit_pos;
    uint32_t negative_count, positive_count;
    if (pic->num_short_term_ref_pic_sets == 1) {
        uint32_t inter_ref_pic_set_prediction_flag;
        if (!br_read_bits(br, 1, &inter_ref_pic_set_prediction_flag) ||
            inter_ref_pic_set_prediction_flag)
            return 0;
    }
    if (!br_read_ue(br, &negative_count, NULL, NULL) ||
        !br_read_ue(br, &positive_count, NULL, NULL) ||
        negative_count > 15 || positive_count > 15 - negative_count)
        return 0;

    uint32_t used_count = 0;
    uint32_t entry_count = negative_count + positive_count;
    int64_t cumulative_delta = 0;
    for (uint32_t i = 0; i < entry_count; i++) {
        if (i == negative_count)
            cumulative_delta = 0;
        uint32_t delta_poc_minus1, used_by_curr_pic;
        if (!br_read_ue(br, &delta_poc_minus1, NULL, NULL) ||
            !br_read_bits(br, 1, &used_by_curr_pic))
            return 0;

        cumulative_delta += (int64_t)delta_poc_minus1 + 1;
        int64_t poc = i < negative_count ?
            (int64_t)pic->CurrPic.pic_order_cnt - cumulative_delta :
            (int64_t)pic->CurrPic.pic_order_cnt + cumulative_delta;
        if (poc < INT32_MIN || poc > INT32_MAX)
            return 0;
        rps->poc[i] = (int32_t)poc;
        rps->flags[i] = i < negative_count ?
            VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE :
            VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
        rps->used[i] = (uint8_t)used_by_curr_pic;
        used_count += used_by_curr_pic;
        if (!used_by_curr_pic)
            rps->flags[i] = 0;
    }

    if (br->bit_pos - start != pic->st_rps_bits ||
        used_count > 8 || (slice_type != 2 && used_count == 0))
        return 0;
    rps->entry_count = entry_count;
    rps->current_count = used_count;

    uint32_t mapped_current_count = 0;
    for (size_t i = 0; i < 15; i++) {
        const VAPictureHEVC *ref = &pic->ReferenceFrames[i];
        if (ref->picture_id == VA_INVALID_SURFACE) {
            if (ref->flags != VA_PICTURE_HEVC_INVALID)
                return 0;
            continue;
        }
        if (ref->flags & (VA_PICTURE_HEVC_INVALID |
                          VA_PICTURE_HEVC_FIELD_PIC |
                          VA_PICTURE_HEVC_BOTTOM_FIELD |
                          VA_PICTURE_HEVC_LONG_TERM_REFERENCE |
                          VA_PICTURE_HEVC_RPS_LT_CURR))
            return 0;
        const uint32_t known_flags =
            VA_PICTURE_HEVC_INVALID | VA_PICTURE_HEVC_FIELD_PIC |
            VA_PICTURE_HEVC_BOTTOM_FIELD | VA_PICTURE_HEVC_LONG_TERM_REFERENCE |
            VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE |
            VA_PICTURE_HEVC_RPS_ST_CURR_AFTER | VA_PICTURE_HEVC_RPS_LT_CURR;
        if (ref->flags & ~known_flags)
            return 0;

        uint32_t current_flags = ref->flags &
            (VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE |
             VA_PICTURE_HEVC_RPS_ST_CURR_AFTER);
        if (current_flags == (VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE |
                              VA_PICTURE_HEVC_RPS_ST_CURR_AFTER))
            return 0;
        if (current_flags == 0)
            continue;

        int matched = 0;
        for (uint32_t j = 0; j < entry_count; j++) {
            if (rps->used[j] && rps->flags[j] == current_flags &&
                rps->poc[j] == ref->pic_order_cnt) {
                if (matched)
                    return 0;
                if (rps->reference_index[j] != 0xffu)
                    return 0;
                matched = 1;
                rps->reference_index[j] = (uint8_t)i;
            }
        }
        if (!matched)
            return 0;
        mapped_current_count++;
    }
    if (mapped_current_count != used_count)
        return 0;

    return 1;
}

static int hobot_hevc_current_rps_from_picture(
    const VAPictureParameterBufferHEVC *pic,
    HobotHevcInlineRps *rps
) {
    if (!pic || !rps)
        return 0;

    uint8_t before[15];
    uint8_t after[15];
    size_t before_count = 0;
    size_t after_count = 0;
    const uint32_t known_flags =
        VA_PICTURE_HEVC_INVALID | VA_PICTURE_HEVC_FIELD_PIC |
        VA_PICTURE_HEVC_BOTTOM_FIELD | VA_PICTURE_HEVC_LONG_TERM_REFERENCE |
        VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE |
        VA_PICTURE_HEVC_RPS_ST_CURR_AFTER | VA_PICTURE_HEVC_RPS_LT_CURR;

    memset(rps, 0, sizeof(*rps));
    memset(rps->reference_index, 0xff, sizeof(rps->reference_index));
    for (size_t i = 0; i < 15; i++) {
        const VAPictureHEVC *ref = &pic->ReferenceFrames[i];
        if (ref->picture_id == VA_INVALID_SURFACE) {
            if (ref->flags != VA_PICTURE_HEVC_INVALID)
                return 0;
            continue;
        }
        if (ref->flags & (VA_PICTURE_HEVC_INVALID |
                          VA_PICTURE_HEVC_FIELD_PIC |
                          VA_PICTURE_HEVC_BOTTOM_FIELD |
                          VA_PICTURE_HEVC_LONG_TERM_REFERENCE |
                          VA_PICTURE_HEVC_RPS_LT_CURR) ||
            (ref->flags & ~known_flags))
            return 0;

        uint32_t current_flags = ref->flags &
            (VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE |
             VA_PICTURE_HEVC_RPS_ST_CURR_AFTER);
        if (current_flags == (VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE |
                              VA_PICTURE_HEVC_RPS_ST_CURR_AFTER))
            return 0;
        if (current_flags == VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE) {
            if (ref->pic_order_cnt >= pic->CurrPic.pic_order_cnt ||
                before_count >= 15)
                return 0;
            before[before_count++] = (uint8_t)i;
        } else if (current_flags == VA_PICTURE_HEVC_RPS_ST_CURR_AFTER) {
            if (ref->pic_order_cnt <= pic->CurrPic.pic_order_cnt ||
                after_count >= 15)
                return 0;
            after[after_count++] = (uint8_t)i;
        }
    }

    for (size_t i = 1; i < before_count; i++) {
        uint8_t index = before[i];
        size_t j = i;
        while (j > 0 &&
               pic->ReferenceFrames[before[j - 1]].pic_order_cnt <
                   pic->ReferenceFrames[index].pic_order_cnt) {
            before[j] = before[j - 1];
            j--;
        }
        before[j] = index;
    }
    for (size_t i = 1; i < after_count; i++) {
        uint8_t index = after[i];
        size_t j = i;
        while (j > 0 &&
               pic->ReferenceFrames[after[j - 1]].pic_order_cnt >
                   pic->ReferenceFrames[index].pic_order_cnt) {
            after[j] = after[j - 1];
            j--;
        }
        after[j] = index;
    }

    for (size_t i = 1; i < before_count; i++) {
        if (pic->ReferenceFrames[before[i - 1]].pic_order_cnt ==
            pic->ReferenceFrames[before[i]].pic_order_cnt)
            return 0;
    }
    for (size_t i = 1; i < after_count; i++) {
        if (pic->ReferenceFrames[after[i - 1]].pic_order_cnt ==
            pic->ReferenceFrames[after[i]].pic_order_cnt)
            return 0;
    }

    rps->entry_count = (uint32_t)(before_count + after_count);
    rps->current_count = rps->entry_count;
    if (rps->current_count > 8)
        return 0;
    for (size_t i = 0; i < before_count + after_count; i++) {
        uint8_t ref_index = i < before_count ? before[i] :
            after[i - before_count];
        const VAPictureHEVC *ref = &pic->ReferenceFrames[ref_index];
        rps->poc[i] = ref->pic_order_cnt;
        rps->flags[i] = i < before_count ?
            VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE :
            VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
        rps->used[i] = 1;
        rps->reference_index[i] = ref_index;
    }
    return 1;
}

static int hobot_hevc_synthesized_single_rps_matches_picture(
    const VAPictureParameterBufferHEVC *pic,
    const HobotHevcInlineRps *rps
) {
    return pic && rps && rps->entry_count == 1 &&
           rps->current_count == 1 && rps->used[0] &&
           rps->flags[0] == VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE &&
           rps->reference_index[0] < 15 &&
           (int64_t)pic->CurrPic.pic_order_cnt - (int64_t)rps->poc[0] == 1;
}

static unsigned int hobot_hevc_ceil_log2(uint32_t value) {
    unsigned int bits = 0;
    if (value > 1)
        value--;
    for (; value != 0; value >>= 1)
        bits++;
    return bits;
}

static int hobot_hevc_inline_ref_pic_lists_supported(
    BitReader *br,
    const VAPictureParameterBufferHEVC *pic,
    const VASliceParameterBufferHEVC *slice,
    unsigned int slice_type,
    const HobotHevcInlineRps *rps
) {
    if (!br || !pic || !slice || !rps ||
        pic->slice_parsing_fields.bits.long_term_ref_pics_present_flag)
        return 0;

    uint32_t slice_temporal_mvp_enabled_flag = 0;
    if (pic->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag) {
        if (!br_read_bits(br, 1, &slice_temporal_mvp_enabled_flag) ||
            slice_temporal_mvp_enabled_flag !=
                slice->LongSliceFlags.fields.slice_temporal_mvp_enabled_flag)
            return 0;
    } else if (slice->LongSliceFlags.fields.slice_temporal_mvp_enabled_flag) {
        return 0;
    }

    uint32_t sao_luma_flag = 0;
    uint32_t sao_chroma_flag = 0;
    if (pic->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag) {
        if (!br_read_bits(br, 1, &sao_luma_flag) ||
            !br_read_bits(br, 1, &sao_chroma_flag) ||
            sao_luma_flag != slice->LongSliceFlags.fields.slice_sao_luma_flag ||
            sao_chroma_flag != slice->LongSliceFlags.fields.slice_sao_chroma_flag)
            return 0;
    } else if (slice->LongSliceFlags.fields.slice_sao_luma_flag ||
               slice->LongSliceFlags.fields.slice_sao_chroma_flag) {
        return 0;
    }

    unsigned int active_count[2] = {0, 0};
    if (slice_type == 0 || slice_type == 1) {
        uint32_t active_override_flag;
        if (!br_read_bits(br, 1, &active_override_flag))
            return 0;
        active_count[0] =
            (unsigned int)pic->num_ref_idx_l0_default_active_minus1 + 1u;
        if (slice_type == 0)
            active_count[1] =
                (unsigned int)pic->num_ref_idx_l1_default_active_minus1 + 1u;
        if (active_override_flag) {
            uint32_t active_minus1;
            if (!br_read_ue(br, &active_minus1, NULL, NULL) || active_minus1 > 14)
                return 0;
            active_count[0] = active_minus1 + 1u;
            if (slice_type == 0) {
                if (!br_read_ue(br, &active_minus1, NULL, NULL) ||
                    active_minus1 > 14)
                    return 0;
                active_count[1] = active_minus1 + 1u;
            }
        }
        if (active_count[0] == 0 || active_count[0] > 15 ||
            active_count[0] - 1u != slice->num_ref_idx_l0_active_minus1 ||
            (slice_type == 0 &&
             (active_count[1] == 0 || active_count[1] > 15 ||
              active_count[1] - 1u !=
                  slice->num_ref_idx_l1_active_minus1)) ||
            (slice_type == 1 &&
             slice->num_ref_idx_l1_active_minus1 != 0 &&
             slice->num_ref_idx_l1_active_minus1 !=
                 pic->num_ref_idx_l1_default_active_minus1))
            return 0;
    } else if (slice->num_ref_idx_l0_active_minus1 != 0 ||
               slice->num_ref_idx_l1_active_minus1 != 0) {
        return 0;
    }

    uint8_t list_entry[2][15] = {{0}};
    uint32_t list_modified[2] = {0, 0};
    if ((slice_type == 0 || slice_type == 1) && rps->current_count == 0)
        return 0;
    if ((slice_type == 0 || slice_type == 1) &&
        pic->slice_parsing_fields.bits.lists_modification_present_flag &&
        rps->current_count > 1) {
        unsigned int entry_bits = hobot_hevc_ceil_log2(rps->current_count);
        unsigned int list_count = slice_type == 0 ? 2u : 1u;
        for (unsigned int list = 0; list < list_count; list++) {
            if (!br_read_bits(br, 1, &list_modified[list]))
                return 0;
            if (!list_modified[list])
                continue;
            for (unsigned int i = 0; i < active_count[list]; i++) {
                uint32_t index;
                if (!br_read_bits(br, entry_bits, &index) ||
                    index >= rps->current_count)
                    return 0;
                list_entry[list][i] = (uint8_t)index;
            }
        }
    }

    uint8_t base_before[15];
    uint8_t base_after[15];
    size_t before_count = 0;
    size_t after_count = 0;
    for (uint32_t i = 0; i < rps->entry_count; i++) {
        if (!rps->used[i])
            continue;
        if (rps->reference_index[i] >= 15)
            return 0;
        if (rps->flags[i] == VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE)
            base_before[before_count++] = rps->reference_index[i];
        else if (rps->flags[i] == VA_PICTURE_HEVC_RPS_ST_CURR_AFTER)
            base_after[after_count++] = rps->reference_index[i];
        else
            return 0;
    }
    if (before_count + after_count != rps->current_count)
        return 0;

    unsigned int list_count = slice_type == 0 ? 2u :
                              slice_type == 1 ? 1u : 0u;
    for (unsigned int list = 0; list < list_count; list++) {
        unsigned int active = active_count[list];
        unsigned int temporary_count = active > rps->current_count ?
            active : rps->current_count;
        uint8_t temporary[15];
        size_t temporary_size = 0;
        while (temporary_size < temporary_count) {
            const uint8_t *first = list == 0 ? base_before : base_after;
            size_t first_count = list == 0 ? before_count : after_count;
            const uint8_t *second = list == 0 ? base_after : base_before;
            size_t second_count = list == 0 ? after_count : before_count;
            for (size_t i = 0; i < first_count &&
                               temporary_size < temporary_count; i++)
                temporary[temporary_size++] = first[i];
            for (size_t i = 0; i < second_count &&
                               temporary_size < temporary_count; i++)
                temporary[temporary_size++] = second[i];
        }

        for (unsigned int i = 0; i < active; i++) {
            uint32_t temporary_index = list_modified[list] ?
                list_entry[list][i] : i;
            if (temporary_index >= temporary_size ||
                slice->RefPicList[list][i] != temporary[temporary_index])
                return 0;
        }
        for (unsigned int i = active; i < 15; i++) {
            if (slice->RefPicList[list][i] != 0xffu)
                return 0;
        }
    }
    for (unsigned int list = list_count; list < 2; list++) {
        for (size_t i = 0; i < 15; i++) {
            if (slice->RefPicList[list][i] != 0xffu)
                return 0;
        }
    }
    return 1;
}

static int hobot_hevc_parse_pred_weight_table(BitReader *br,
                                               unsigned int slice_type,
                                               unsigned int chroma_array_type) {
    uint32_t luma_log2_weight_denom;
    int32_t delta_chroma_log2_weight_denom = 0;
    uint32_t luma_weight_l0_flag, chroma_weight_l0_flag = 0;
    uint32_t luma_weight_l1_flag = 0, chroma_weight_l1_flag = 0;

    if (!br_read_ue(br, &luma_log2_weight_denom, NULL, NULL) ||
        luma_log2_weight_denom > 7)
        return 0;
    if (chroma_array_type != 0 &&
        (!br_read_se(br, &delta_chroma_log2_weight_denom) ||
         delta_chroma_log2_weight_denom < -7 ||
         delta_chroma_log2_weight_denom > 7 ||
         (int32_t)luma_log2_weight_denom +
             delta_chroma_log2_weight_denom < 0 ||
         (int32_t)luma_log2_weight_denom +
             delta_chroma_log2_weight_denom > 7))
        return 0;

    if (!br_read_bits(br, 1, &luma_weight_l0_flag) ||
        (chroma_array_type != 0 &&
         !br_read_bits(br, 1, &chroma_weight_l0_flag)))
        return 0;
    if ((luma_weight_l0_flag &&
         (!hobot_hevc_read_weight_value(br, -128, 127) ||
          !hobot_hevc_read_weight_value(br, -128, 127))))
        return 0;
    if (chroma_weight_l0_flag) {
        for (unsigned int component = 0; component < 2; component++) {
            if (!hobot_hevc_read_chroma_weight_component(br))
                return 0;
        }
    }

    if (slice_type == 0) {
        if (!br_read_bits(br, 1, &luma_weight_l1_flag) ||
            (chroma_array_type != 0 &&
             !br_read_bits(br, 1, &chroma_weight_l1_flag)))
            return 0;
        if (luma_weight_l1_flag &&
            (!hobot_hevc_read_weight_value(br, -128, 127) ||
             !hobot_hevc_read_weight_value(br, -128, 127)))
            return 0;
        if (chroma_weight_l1_flag) {
            for (unsigned int component = 0; component < 2; component++) {
                if (!hobot_hevc_read_chroma_weight_component(br))
                    return 0;
            }
        }
    }
    return 1;
}

static int hobot_hevc_validated_rps_slice_supported_internal(
    const VAPictureParameterBufferHEVC *pic,
    const VASliceParameterBufferHEVC *slice,
    const uint8_t *slice_data,
    size_t slice_data_size,
    const HobotHevcSliceSequence *sequence,
    size_t *sps_rps_flag_end_bit,
    int *sps_rps_selected
) {
    if (sps_rps_flag_end_bit)
        *sps_rps_flag_end_bit = 0;
    if (sps_rps_selected)
        *sps_rps_selected = 0;

    if (!pic || !slice || !slice_data ||
        (pic->num_short_term_ref_pic_sets != 0 &&
         pic->num_short_term_ref_pic_sets != 1 &&
         pic->num_short_term_ref_pic_sets != 2) ||
        !sequence || sequence->picture_ctb_count == 0 ||
        (pic->num_short_term_ref_pic_sets == 2 && pic->st_rps_bits != 0) ||
        pic->pic_fields.bits.chroma_format_idc != 1 ||
        pic->pic_fields.bits.separate_colour_plane_flag ||
        pic->bit_depth_luma_minus8 != 0 ||
        pic->bit_depth_chroma_minus8 != 0 ||
        pic->log2_max_pic_order_cnt_lsb_minus4 > 12 ||
        !hobot_hevc_slice_parameter_fields_supported(slice) ||
        slice->slice_data_offset > slice_data_size ||
        slice->slice_data_size > slice_data_size - slice->slice_data_offset)
        return 0;

    const uint8_t *nal = slice_data + slice->slice_data_offset;
    size_t nal_size = slice->slice_data_size;
    size_t prefix_size = 0;
    size_t prefix_offset = hobot_hevc_find_start_code(
        nal, nal_size, 0, &prefix_size);
    if (prefix_offset != SIZE_MAX) {
        nal += prefix_offset + prefix_size;
        nal_size -= prefix_offset + prefix_size;
    }
    if (nal_size < 3 || (nal[0] & 0x80u) != 0 || (nal[1] & 0x07u) == 0)
        return 0;

    unsigned int nal_type = (nal[0] >> 1) & 0x3fu;
    unsigned int layer_id = ((unsigned int)(nal[0] & 1u) << 5) |
                            ((unsigned int)nal[1] >> 3);
    int is_idr = nal_type == 19 || nal_type == 20;
    int is_cra = nal_type == 21;
    int is_irap = nal_type >= 16 && nal_type <= 23;
    if (layer_id != 0 ||
        is_idr != (int)pic->slice_parsing_fields.bits.IdrPicFlag ||
        is_irap != (int)pic->slice_parsing_fields.bits.RapPicFlag ||
        (is_irap && !is_idr && !is_cra) ||
        (!is_irap && nal_type > 1) ||
        pic->CurrPic.picture_id == VA_INVALID_SURFACE ||
        (pic->CurrPic.flags & (VA_PICTURE_HEVC_INVALID |
                               VA_PICTURE_HEVC_FIELD_PIC |
                               VA_PICTURE_HEVC_BOTTOM_FIELD |
                               VA_PICTURE_HEVC_LONG_TERM_REFERENCE)))
        return 0;

    uint8_t rbsp[256];
    size_t rbsp_size = 0;
    size_t payload_size = nal_size - 2u;
    if (payload_size > sizeof(rbsp))
        payload_size = sizeof(rbsp);
    if (!hobot_hevc_unescape_rbsp(nal + 2, payload_size,
                                  rbsp, sizeof(rbsp), &rbsp_size) ||
        rbsp_size == 0 || rbsp_size > SIZE_MAX / 8u)
        return 0;

    BitReader br = {rbsp, rbsp_size * 8u, 0};
    uint32_t first_slice_segment, value;
    if (!br_read_bits(&br, 1, &first_slice_segment) ||
        !!first_slice_segment != (slice->slice_segment_address == 0))
        return 0;
    if (is_irap && !br_skip_bits(&br, 1))
        return 0;
    if (!br_read_ue(&br, &value, NULL, NULL) || value != 0)
        return 0;
    if (!first_slice_segment) {
        if (pic->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag) {
            uint32_t dependent_slice_segment;
            if (!br_read_bits(&br, 1, &dependent_slice_segment) ||
                dependent_slice_segment != 0)
                return 0;
        }
        unsigned int address_bits = 0;
        uint64_t address_range = sequence->picture_ctb_count - 1u;
        while (address_range != 0) {
            address_bits++;
            address_range >>= 1;
        }
        if (!br_read_bits(&br, address_bits, &value) ||
            value != slice->slice_segment_address)
            return 0;
    }
    if (!br_skip_bits(&br, pic->num_extra_slice_header_bits) ||
        !br_read_ue(&br, &value, NULL, NULL) ||
        value != slice->LongSliceFlags.fields.slice_type)
        return 0;
    if (pic->slice_parsing_fields.bits.output_flag_present_flag &&
        !br_skip_bits(&br, 1))
        return 0;
    if (pic->pic_fields.bits.separate_colour_plane_flag &&
        !br_skip_bits(&br, 2))
        return 0;

    if (is_idr) {
        if (slice->LongSliceFlags.fields.slice_type != 2 ||
            !pic->slice_parsing_fields.bits.IntraPicFlag ||
            pic->st_rps_bits != 0 ||
            !hobot_hevc_slice_header_offset_valid(&br, slice))
            return 0;
        for (size_t i = 0; i < 15; i++) {
            if (pic->ReferenceFrames[i].picture_id != VA_INVALID_SURFACE ||
                !(pic->ReferenceFrames[i].flags & VA_PICTURE_HEVC_INVALID))
                return 0;
        }
        return 1;
    }

    unsigned int slice_type = slice->LongSliceFlags.fields.slice_type;
    uint32_t short_term_ref_pic_set_sps_flag = 0;
    int short_term_ref_pic_set_sps_flag_read = 0;
    if (is_cra) {
        unsigned int poc_lsb_bits =
            4u + pic->log2_max_pic_order_cnt_lsb_minus4;
        uint32_t poc_lsb;
        HobotHevcInlineRps inline_rps;
        if (!pic->slice_parsing_fields.bits.RapPicFlag ||
            !pic->slice_parsing_fields.bits.IntraPicFlag ||
            slice_type != 2 || pic->num_short_term_ref_pic_sets != 1 ||
            !br_read_bits(&br, poc_lsb_bits, &poc_lsb))
            return 0;
        uint32_t poc_mask = (1u << poc_lsb_bits) - 1u;
        if (((uint32_t)pic->CurrPic.pic_order_cnt & poc_mask) != poc_lsb ||
            !br_read_bits(&br, 1, &short_term_ref_pic_set_sps_flag) ||
            short_term_ref_pic_set_sps_flag ||
            !hobot_hevc_inline_rps_supported(&br, pic, slice_type,
                                              &inline_rps) ||
            inline_rps.entry_count != 0 || inline_rps.current_count != 0 ||
            !hobot_hevc_inline_ref_pic_lists_supported(
                &br, pic, slice, slice_type, &inline_rps) ||
            !hobot_hevc_slice_header_offset_valid(&br, slice))
            return 0;
        for (size_t i = 0; i < 15; i++) {
            if (pic->ReferenceFrames[i].picture_id != VA_INVALID_SURFACE ||
                pic->ReferenceFrames[i].flags != VA_PICTURE_HEVC_INVALID)
                return 0;
        }
        return 1;
    }

    if (pic->num_short_term_ref_pic_sets == 0 ||
        pic->num_short_term_ref_pic_sets == 1) {
        unsigned int poc_lsb_bits =
            4u + pic->log2_max_pic_order_cnt_lsb_minus4;
        uint32_t poc_lsb;
        HobotHevcInlineRps inline_rps;
        size_t selection_flag_end_bit = 0;
        if (!br_read_bits(&br, poc_lsb_bits, &poc_lsb))
            return 0;
        uint32_t poc_mask = (1u << poc_lsb_bits) - 1u;
        if (((uint32_t)pic->CurrPic.pic_order_cnt & poc_mask) != poc_lsb ||
            !br_read_bits(&br, 1, &short_term_ref_pic_set_sps_flag))
            return 0;
        short_term_ref_pic_set_sps_flag_read = 1;
        if (short_term_ref_pic_set_sps_flag)
            selection_flag_end_bit = br.bit_pos;
        if (!short_term_ref_pic_set_sps_flag) {
            if (!hobot_hevc_inline_rps_supported(&br, pic, slice_type,
                                                &inline_rps))
                return 0;
            if (!hobot_hevc_inline_ref_pic_lists_supported(
                    &br, pic, slice, slice_type, &inline_rps))
                return 0;
            if (!hobot_hevc_slice_header_offset_valid(&br, slice))
                return 0;
            return 1;
        }
        if (pic->num_short_term_ref_pic_sets != 1 ||
            pic->st_rps_bits != 0 ||
            !hobot_hevc_current_rps_from_picture(pic, &inline_rps) ||
            !hobot_hevc_synthesized_single_rps_matches_picture(pic, &inline_rps) ||
            !hobot_hevc_inline_ref_pic_lists_supported(
                &br, pic, slice, slice_type, &inline_rps) ||
            !hobot_hevc_slice_header_offset_valid(&br, slice))
            return 0;
        if (sps_rps_flag_end_bit)
            *sps_rps_flag_end_bit = selection_flag_end_bit;
        if (sps_rps_selected)
            *sps_rps_selected = 1;
        return 1;
    }

    /* One current reference makes the PPS list-modification flag inert here. */
    if (pic->slice_parsing_fields.bits.RapPicFlag ||
        (slice_type != 0 && slice_type != 1) ||
        pic->slice_parsing_fields.bits.IntraPicFlag ||
        pic->slice_parsing_fields.bits.long_term_ref_pics_present_flag ||
        pic->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag ||
        pic->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag ||
        pic->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag ||
        pic->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag ||
        pic->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag ||
        pic->slice_parsing_fields.bits.slice_segment_header_extension_present_flag ||
        slice->LongSliceFlags.fields.slice_temporal_mvp_enabled_flag ||
        slice->LongSliceFlags.fields.slice_sao_luma_flag ||
        slice->LongSliceFlags.fields.slice_sao_chroma_flag ||
        slice->LongSliceFlags.fields.slice_deblocking_filter_disabled_flag ||
        slice->num_ref_idx_l0_active_minus1 != 0 ||
        slice->num_ref_idx_l1_active_minus1 != 0 ||
        pic->num_ref_idx_l0_default_active_minus1 != 0 ||
        pic->num_ref_idx_l1_default_active_minus1 != 0 ||
        (slice_type == 0 && pic->pic_fields.bits.NoBiPredFlag))
        return 0;

    if (!short_term_ref_pic_set_sps_flag_read) {
        unsigned int poc_lsb_bits =
            4u + pic->log2_max_pic_order_cnt_lsb_minus4;
        uint32_t poc_lsb;
        if (!br_read_bits(&br, poc_lsb_bits, &poc_lsb))
            return 0;
        uint32_t poc_mask = (1u << poc_lsb_bits) - 1u;
        if (((uint32_t)pic->CurrPic.pic_order_cnt & poc_mask) != poc_lsb ||
            !br_read_bits(&br, 1, &short_term_ref_pic_set_sps_flag))
            return 0;
    }
    if (!short_term_ref_pic_set_sps_flag ||
        (pic->num_short_term_ref_pic_sets == 2 &&
         (!br_read_bits(&br, 1, &value) || value != 0)) ||
        !hobot_hevc_slice_header_offset_valid(&br, slice))
        return 0;

    uint32_t num_ref_idx_active_override_flag;
    if (!br_read_bits(&br, 1, &num_ref_idx_active_override_flag) ||
        num_ref_idx_active_override_flag)
        return 0;

    if (slice_type == 0) {
        uint32_t mvd_l1_zero_flag;
        if (!br_read_bits(&br, 1, &mvd_l1_zero_flag) ||
            mvd_l1_zero_flag !=
                slice->LongSliceFlags.fields.mvd_l1_zero_flag)
            return 0;
    } else if (slice->LongSliceFlags.fields.mvd_l1_zero_flag) {
        return 0;
    }

    if (pic->slice_parsing_fields.bits.cabac_init_present_flag) {
        uint32_t cabac_init_flag;
        if (!br_read_bits(&br, 1, &cabac_init_flag) ||
            cabac_init_flag != slice->LongSliceFlags.fields.cabac_init_flag)
            return 0;
    } else if (slice->LongSliceFlags.fields.cabac_init_flag) {
        return 0;
    }

    unsigned int chroma_array_type =
        pic->pic_fields.bits.separate_colour_plane_flag ? 0u :
        pic->pic_fields.bits.chroma_format_idc;
    if (chroma_array_type > 3u)
        return 0;
    int weighted_pred_table_present =
        (slice_type == 1 && pic->pic_fields.bits.weighted_pred_flag) ||
        (slice_type == 0 && pic->pic_fields.bits.weighted_bipred_flag);
    if (weighted_pred_table_present &&
        (!hobot_hevc_parse_pred_weight_table(
             &br, slice_type, chroma_array_type) ||
         !hobot_hevc_slice_header_offset_valid(&br, slice)))
        return 0;

    if (!br_read_ue(&br, &value, NULL, NULL) || value > 4 ||
        value != slice->five_minus_max_num_merge_cand)
        return 0;
    int32_t slice_qp_delta;
    if (!br_read_se(&br, &slice_qp_delta) ||
        slice_qp_delta != slice->slice_qp_delta)
        return 0;

    if (pic->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag) {
        uint32_t slice_loop_filter_across_slices_enabled_flag;
        if (!br_read_bits(&br, 1,
                          &slice_loop_filter_across_slices_enabled_flag) ||
            slice_loop_filter_across_slices_enabled_flag !=
                slice->LongSliceFlags.fields
                    .slice_loop_filter_across_slices_enabled_flag)
            return 0;
    } else if (slice->LongSliceFlags.fields
                   .slice_loop_filter_across_slices_enabled_flag) {
        return 0;
    }

    uint32_t alignment_bit;
    if (!br_read_bits(&br, 1, &alignment_bit) || alignment_bit != 1)
        return 0;
    while (br.bit_pos % 8u != 0) {
        uint32_t alignment_zero_bit;
        if (!br_read_bits(&br, 1, &alignment_zero_bit) || alignment_zero_bit)
            return 0;
    }

    if (!hobot_hevc_slice_header_offset_valid(&br, slice))
        return 0;

    int reference_index = slice->RefPicList[0][0];
    if (reference_index >= 15)
        return 0;
    if (slice_type == 0 && slice->RefPicList[1][0] != reference_index)
        return 0;
    for (size_t i = 1; i < 15; i++) {
        if (slice->RefPicList[0][i] != 0xffu ||
            slice->RefPicList[1][i] != 0xffu)
            return 0;
    }
    if (slice_type == 1 && slice->RefPicList[1][0] != 0xffu)
        return 0;

    int valid_reference_count = 0;
    for (size_t i = 0; i < 15; i++) {
        const VAPictureHEVC *ref = &pic->ReferenceFrames[i];
        if (ref->picture_id == VA_INVALID_SURFACE) {
            if (!(ref->flags & VA_PICTURE_HEVC_INVALID))
                return 0;
            continue;
        }
        if ((ref->flags & (VA_PICTURE_HEVC_INVALID |
                           VA_PICTURE_HEVC_FIELD_PIC |
                           VA_PICTURE_HEVC_BOTTOM_FIELD |
                           VA_PICTURE_HEVC_LONG_TERM_REFERENCE)) ||
            (int)i != reference_index ||
            (int64_t)pic->CurrPic.pic_order_cnt - ref->pic_order_cnt != 1)
            return 0;
        valid_reference_count++;
    }
    return valid_reference_count == 1;
}

static int hobot_hevc_validated_rps_slice_supported(
    const VAPictureParameterBufferHEVC *pic,
    const VASliceParameterBufferHEVC *slice,
    const uint8_t *slice_data,
    size_t slice_data_size,
    const HobotHevcSliceSequence *sequence
) {
    return hobot_hevc_validated_rps_slice_supported_internal(
        pic, slice, slice_data, slice_data_size, sequence, NULL, NULL);
}

static int hobot_hevc_rewrite_single_rps_slice(
    const VAPictureParameterBufferHEVC *pic,
    const VASliceParameterBufferHEVC *slice,
    const uint8_t *slice_data,
    size_t slice_data_size,
    uint8_t **rewritten_data,
    size_t *rewritten_size
) {
    if (!rewritten_data || !rewritten_size)
        return -1;
    *rewritten_data = NULL;
    *rewritten_size = 0;
    if (!pic || pic->num_short_term_ref_pic_sets != 1)
        return 0;

    HobotHevcSliceSequence sequence;
    if (!hobot_hevc_slice_sequence_init(pic, 1, &sequence))
        return -1;
    size_t insert_bit = 0;
    int selected_sps_rps = 0;
    if (!hobot_hevc_validated_rps_slice_supported_internal(
            pic, slice, slice_data, slice_data_size, &sequence,
            &insert_bit, &selected_sps_rps))
        return -1;
    if (!selected_sps_rps)
        return 0;

    const uint8_t *slice_bytes = slice_data + slice->slice_data_offset;
    size_t prefix_size = 0;
    size_t prefix_offset = hobot_hevc_find_start_code(
        slice_bytes, slice->slice_data_size, 0, &prefix_size);
    size_t nal_offset = prefix_offset == SIZE_MAX ? 0 :
                        prefix_offset + prefix_size;
    if (nal_offset > slice->slice_data_size ||
        slice->slice_data_size - nal_offset < 3 ||
        slice->slice_data_byte_offset < 2)
        return -1;

    const uint8_t *nal = slice_bytes + nal_offset;
    size_t nal_size = slice->slice_data_size - nal_offset;
    size_t rbsp_capacity = nal_size - 2u;
    if (rbsp_capacity == 0 || rbsp_capacity > (SIZE_MAX - 16u) / 2u)
        return -1;
    uint8_t *rbsp = malloc(rbsp_capacity);
    if (!rbsp)
        return -1;
    size_t rbsp_size = 0;
    if (!hobot_hevc_unescape_rbsp(nal + 2, nal_size - 2u,
                                  rbsp, rbsp_capacity, &rbsp_size)) {
        free(rbsp);
        return -1;
    }

    size_t header_bytes = (size_t)slice->slice_data_byte_offset - 2u;
    if (header_bytes == 0 || header_bytes > rbsp_size ||
        header_bytes > SIZE_MAX / 8u ||
        insert_bit >= header_bytes * 8u) {
        free(rbsp);
        return -1;
    }
    size_t header_bits = header_bytes * 8u;
    size_t alignment_bit = SIZE_MAX;
    for (size_t bit = header_bits; bit > 0; bit--) {
        size_t pos = bit - 1u;
        if ((rbsp[pos / 8u] & (uint8_t)(1u << (7u - (pos % 8u)))) != 0) {
            alignment_bit = pos;
            break;
        }
    }
    if (alignment_bit == SIZE_MAX || alignment_bit < insert_bit ||
        ((alignment_bit + 1u + 7u) & ~(size_t)7u) != header_bits) {
        free(rbsp);
        return -1;
    }

    if (rbsp_size == SIZE_MAX) {
        free(rbsp);
        return -1;
    }
    size_t rewritten_rbsp_capacity = rbsp_size + 1u;
    if (rewritten_rbsp_capacity > SIZE_MAX / 8u) {
        free(rbsp);
        return -1;
    }
    size_t rewritten_bit_capacity = rewritten_rbsp_capacity * 8u;
    uint8_t *rewritten_rbsp = calloc(rewritten_rbsp_capacity, 1u);
    if (!rewritten_rbsp) {
        free(rbsp);
        return -1;
    }
    size_t dst_bit = 0;
    int ok = hobot_hevc_copy_bits(rewritten_rbsp,
                                  rewritten_bit_capacity,
                                  &dst_bit, rbsp, 0, insert_bit) &&
             hobot_hevc_write_bit(rewritten_rbsp,
                                  rewritten_bit_capacity,
                                  &dst_bit, 0) &&
             hobot_hevc_copy_bits(rewritten_rbsp,
                                  rewritten_bit_capacity,
                                  &dst_bit, rbsp, insert_bit,
                                  alignment_bit - insert_bit) &&
             hobot_hevc_write_bit(rewritten_rbsp,
                                  rewritten_bit_capacity,
                                  &dst_bit, 1);
    while (ok && dst_bit % 8u != 0)
        ok = hobot_hevc_write_bit(rewritten_rbsp,
                                  rewritten_bit_capacity,
                                  &dst_bit, 0);
    size_t rewritten_header_bytes = dst_bit / 8u;
    size_t payload_size = rbsp_size - header_bytes;
    size_t rewritten_rbsp_size = 0;
    if (ok && rewritten_header_bytes <= rewritten_rbsp_capacity &&
        payload_size <= rewritten_rbsp_capacity - rewritten_header_bytes &&
        payload_size <= SIZE_MAX - rewritten_header_bytes) {
        rewritten_rbsp_size = rewritten_header_bytes + payload_size;
        memcpy(rewritten_rbsp + rewritten_header_bytes,
               rbsp + header_bytes, payload_size);
    } else {
        ok = 0;
    }
    free(rbsp);
    if (!ok) {
        free(rewritten_rbsp);
        return -1;
    }

    if (rewritten_rbsp_capacity > (SIZE_MAX - nal_offset - 2u) / 2u) {
        free(rewritten_rbsp);
        return -1;
    }
    size_t escaped_capacity = rewritten_rbsp_capacity * 2u;
    uint8_t *escaped = malloc(escaped_capacity);
    if (!escaped) {
        free(rewritten_rbsp);
        return -1;
    }
    size_t escaped_size = 0;
    if (!hobot_hevc_escape_rbsp(rewritten_rbsp,
                                rewritten_rbsp_size,
                                escaped, escaped_capacity, &escaped_size)) {
        free(escaped);
        free(rewritten_rbsp);
        return -1;
    }
    free(rewritten_rbsp);

    if (escaped_size > SIZE_MAX - nal_offset - 2u) {
        free(escaped);
        return -1;
    }
    size_t output_size = nal_offset + 2u + escaped_size;
    uint8_t *output = malloc(output_size);
    if (!output) {
        free(escaped);
        return -1;
    }
    memcpy(output, slice_bytes, nal_offset + 2u);
    memcpy(output + nal_offset + 2u, escaped, escaped_size);
    free(escaped);

    *rewritten_data = output;
    *rewritten_size = output_size;
    return 1;
}

static int hobot_hevc_idr_slice_supported(
    const VAPictureParameterBufferHEVC *pic,
    const VASliceParameterBufferHEVC *slice,
    const uint8_t *slice_data,
    size_t slice_data_size,
    const HobotHevcSliceSequence *sequence
) {
    if (!pic || !slice || !slice_data || !sequence ||
        sequence->picture_ctb_count == 0 ||
        pic->num_short_term_ref_pic_sets == 0 || pic->st_rps_bits != 0 ||
        !pic->slice_parsing_fields.bits.IdrPicFlag ||
        !pic->slice_parsing_fields.bits.RapPicFlag ||
        !pic->slice_parsing_fields.bits.IntraPicFlag ||
        pic->CurrPic.picture_id == VA_INVALID_SURFACE ||
        (pic->CurrPic.flags & (VA_PICTURE_HEVC_INVALID |
                               VA_PICTURE_HEVC_FIELD_PIC |
                               VA_PICTURE_HEVC_BOTTOM_FIELD |
                               VA_PICTURE_HEVC_LONG_TERM_REFERENCE)) ||
        !hobot_hevc_slice_parameter_fields_supported(slice) ||
        slice->slice_data_offset > slice_data_size ||
        slice->slice_data_size > slice_data_size - slice->slice_data_offset)
        return 0;

    for (size_t i = 0; i < 15; i++) {
        if (pic->ReferenceFrames[i].picture_id != VA_INVALID_SURFACE ||
            !(pic->ReferenceFrames[i].flags & VA_PICTURE_HEVC_INVALID))
            return 0;
    }

    const uint8_t *nal = slice_data + slice->slice_data_offset;
    size_t nal_size = slice->slice_data_size;
    size_t prefix_size = 0;
    size_t prefix_offset = hobot_hevc_find_start_code(
        nal, nal_size, 0, &prefix_size);
    if (prefix_offset != SIZE_MAX) {
        nal += prefix_offset + prefix_size;
        nal_size -= prefix_offset + prefix_size;
    }
    if (nal_size < 3 || (nal[0] & 0x80u) != 0 || (nal[1] & 0x07u) == 0)
        return 0;

    unsigned int nal_type = (nal[0] >> 1) & 0x3fu;
    unsigned int layer_id = ((unsigned int)(nal[0] & 1u) << 5) |
                            ((unsigned int)nal[1] >> 3);
    if ((nal_type != 19 && nal_type != 20) || layer_id != 0)
        return 0;

    uint8_t rbsp[64];
    size_t rbsp_size = 0;
    size_t payload_size = nal_size - 2u;
    if (payload_size > sizeof(rbsp))
        payload_size = sizeof(rbsp);
    if (!hobot_hevc_unescape_rbsp(nal + 2, payload_size,
                                  rbsp, sizeof(rbsp), &rbsp_size) ||
        rbsp_size == 0 || rbsp_size > SIZE_MAX / 8u)
        return 0;

    BitReader br = {rbsp, rbsp_size * 8u, 0};
    uint32_t first_slice_segment, value;
    if (!br_read_bits(&br, 1, &first_slice_segment) ||
        !!first_slice_segment != (slice->slice_segment_address == 0) ||
        !br_skip_bits(&br, 1) ||
        !br_read_ue(&br, &value, NULL, NULL) || value != 0)
        return 0;

    if (!first_slice_segment) {
        if (pic->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag) {
            uint32_t dependent_slice_segment;
            if (!br_read_bits(&br, 1, &dependent_slice_segment) ||
                dependent_slice_segment != 0)
                return 0;
        }

        unsigned int address_bits = 0;
        uint64_t address_range = sequence->picture_ctb_count - 1u;
        while (address_range != 0) {
            address_bits++;
            address_range >>= 1;
        }
        if (!br_read_bits(&br, address_bits, &value) ||
            value != slice->slice_segment_address)
            return 0;
    }

    if (!br_skip_bits(&br, pic->num_extra_slice_header_bits) ||
        !br_read_ue(&br, &value, NULL, NULL) || value != 2)
        return 0;
    if (pic->slice_parsing_fields.bits.output_flag_present_flag &&
        !br_skip_bits(&br, 1))
        return 0;
    if (pic->pic_fields.bits.separate_colour_plane_flag &&
        !br_skip_bits(&br, 2))
        return 0;

    return slice->LongSliceFlags.fields.slice_type == 2 &&
           hobot_hevc_slice_header_offset_valid(&br, slice);
}

static int hobot_hevc_encode_level_supported(unsigned int level_idc)
{
    switch (level_idc) {
    case 0:
    case MC_H265_LEVEL1:
    case MC_H265_LEVEL2:
    case MC_H265_LEVEL2_1:
    case MC_H265_LEVEL3:
    case MC_H265_LEVEL3_1:
    case MC_H265_LEVEL4:
    case MC_H265_LEVEL4_1:
    case MC_H265_LEVEL5:
    case MC_H265_LEVEL5_1:
        return 1;
    default:
        return 0;
    }
}

static int hobot_hevc_encode_sequence_supported(
    const VAEncSequenceParameterBufferHEVC *seq,
    int context_width,
    int context_height
)
{
    if (!seq || context_width <= 0 || context_height <= 0 ||
        context_width > HOBOT_HEVC_MAX_WIDTH + 15 ||
        context_height > HOBOT_HEVC_MAX_HEIGHT + 15 ||
        seq->pic_width_in_luma_samples == 0 ||
        seq->pic_height_in_luma_samples == 0 ||
        seq->pic_width_in_luma_samples > HOBOT_HEVC_MAX_WIDTH ||
        seq->pic_height_in_luma_samples > HOBOT_HEVC_MAX_HEIGHT ||
        (seq->pic_width_in_luma_samples & 1u) != 0 ||
        (seq->pic_height_in_luma_samples & 1u) != 0 ||
        (seq->pic_width_in_luma_samples > (uint32_t)context_width ?
            seq->pic_width_in_luma_samples - (uint32_t)context_width :
            (uint32_t)context_width - seq->pic_width_in_luma_samples) >= 16u ||
        (seq->pic_height_in_luma_samples > (uint32_t)context_height ?
            seq->pic_height_in_luma_samples - (uint32_t)context_height :
            (uint32_t)context_height - seq->pic_height_in_luma_samples) >= 16u ||
        seq->general_profile_idc != 1 || seq->general_tier_flag != 0 ||
        !hobot_hevc_encode_level_supported(seq->general_level_idc) ||
        seq->seq_fields.bits.chroma_format_idc != 1 ||
        seq->seq_fields.bits.separate_colour_plane_flag ||
        seq->seq_fields.bits.bit_depth_luma_minus8 != 0 ||
        seq->seq_fields.bits.bit_depth_chroma_minus8 != 0 ||
        seq->seq_fields.bits.scaling_list_enabled_flag ||
        seq->seq_fields.bits.sample_adaptive_offset_enabled_flag ||
        seq->seq_fields.bits.pcm_enabled_flag ||
        seq->seq_fields.bits.hierachical_flag ||
        seq->seq_fields.bits.reserved_bits != 0 ||
        seq->ip_period != 1 || seq->intra_period > 2047 ||
        seq->intra_idr_period > 2047 || seq->bits_per_second > 700000000u ||
        (seq->bits_per_second > 0 && seq->bits_per_second < 1000u) ||
        seq->log2_min_luma_coding_block_size_minus3 > 3 ||
        seq->log2_diff_max_min_luma_coding_block_size > 3 ||
        (unsigned int)seq->log2_min_luma_coding_block_size_minus3 +
            seq->log2_diff_max_min_luma_coding_block_size > 3 ||
        seq->log2_min_transform_block_size_minus2 > 3 ||
        seq->log2_diff_max_min_transform_block_size > 3 ||
        (unsigned int)seq->log2_min_transform_block_size_minus2 +
            seq->log2_diff_max_min_transform_block_size > 3 ||
        seq->max_transform_hierarchy_depth_inter > 5 ||
        seq->max_transform_hierarchy_depth_intra > 5 ||
        seq->pcm_sample_bit_depth_luma_minus1 != 0 ||
        seq->pcm_sample_bit_depth_chroma_minus1 != 0 ||
        seq->log2_min_pcm_luma_coding_block_size_minus3 != 0 ||
        seq->log2_max_pcm_luma_coding_block_size_minus3 != 0 ||
        seq->scc_fields.value != 0 ||
        (seq->vui_parameters_present_flag &&
         (seq->vui_fields.bits.field_seq_flag ||
          seq->vui_fields.bits.bitstream_restriction_flag ||
          seq->vui_fields.bits.tiles_fixed_structure_flag)) ||
        (seq->vui_parameters_present_flag &&
         seq->vui_fields.bits.aspect_ratio_info_present_flag &&
         seq->aspect_ratio_idc == 255 &&
         (seq->sar_width == 0 || seq->sar_height == 0 ||
          seq->sar_width > UINT16_MAX || seq->sar_height > UINT16_MAX)) ||
        (seq->vui_parameters_present_flag &&
         seq->vui_fields.bits.vui_timing_info_present_flag &&
         (seq->vui_num_units_in_tick == 0 || seq->vui_time_scale == 0)))
        return 0;

    for (size_t i = 0; i < sizeof(seq->va_reserved) / sizeof(seq->va_reserved[0]); i++) {
        if (seq->va_reserved[i] != 0)
            return 0;
    }
    return 1;
}

static int hobot_hevc_configure_encoder_geometry(
    const VAEncSequenceParameterBufferHEVC *seq,
    mc_video_codec_enc_params_t *enc
)
{
    if (!seq || !enc || seq->pic_width_in_luma_samples == 0 ||
        seq->pic_height_in_luma_samples == 0 ||
        (seq->pic_width_in_luma_samples & 1u) != 0 ||
        (seq->pic_height_in_luma_samples & 1u) != 0)
        return 0;

    uint32_t width = seq->pic_width_in_luma_samples;
    uint32_t height = seq->pic_height_in_luma_samples;
    enc->width = (int)width;
    enc->height = (int)height;

    enc->frame_cropping_flag = 0;
    memset(&enc->crop_rect, 0, sizeof(enc->crop_rect));
    return 1;
}

static int hobot_hevc_encode_sequence_static_equal(
    const VAEncSequenceParameterBufferHEVC *a,
    const VAEncSequenceParameterBufferHEVC *b
)
{
    if (!a || !b)
        return 0;
    return a->general_profile_idc == b->general_profile_idc &&
           a->general_level_idc == b->general_level_idc &&
           a->general_tier_flag == b->general_tier_flag &&
           a->ip_period == b->ip_period &&
           a->intra_idr_period == b->intra_idr_period &&
           a->pic_width_in_luma_samples == b->pic_width_in_luma_samples &&
           a->pic_height_in_luma_samples == b->pic_height_in_luma_samples &&
           a->seq_fields.value == b->seq_fields.value &&
           a->log2_min_luma_coding_block_size_minus3 ==
               b->log2_min_luma_coding_block_size_minus3 &&
           a->log2_diff_max_min_luma_coding_block_size ==
               b->log2_diff_max_min_luma_coding_block_size &&
           a->log2_min_transform_block_size_minus2 ==
               b->log2_min_transform_block_size_minus2 &&
           a->log2_diff_max_min_transform_block_size ==
               b->log2_diff_max_min_transform_block_size &&
           a->max_transform_hierarchy_depth_inter ==
               b->max_transform_hierarchy_depth_inter &&
           a->max_transform_hierarchy_depth_intra ==
               b->max_transform_hierarchy_depth_intra &&
           a->pcm_sample_bit_depth_luma_minus1 == b->pcm_sample_bit_depth_luma_minus1 &&
           a->pcm_sample_bit_depth_chroma_minus1 == b->pcm_sample_bit_depth_chroma_minus1 &&
           a->log2_min_pcm_luma_coding_block_size_minus3 ==
               b->log2_min_pcm_luma_coding_block_size_minus3 &&
           a->log2_max_pcm_luma_coding_block_size_minus3 ==
               b->log2_max_pcm_luma_coding_block_size_minus3 &&
           a->vui_parameters_present_flag == b->vui_parameters_present_flag &&
           a->vui_fields.value == b->vui_fields.value &&
           a->aspect_ratio_idc == b->aspect_ratio_idc &&
           a->sar_width == b->sar_width && a->sar_height == b->sar_height &&
           a->vui_num_units_in_tick == b->vui_num_units_in_tick &&
           a->vui_time_scale == b->vui_time_scale &&
           a->min_spatial_segmentation_idc == b->min_spatial_segmentation_idc &&
           a->max_bytes_per_pic_denom == b->max_bytes_per_pic_denom &&
           a->max_bits_per_min_cu_denom == b->max_bits_per_min_cu_denom &&
           a->scc_fields.value == b->scc_fields.value &&
           memcmp(a->va_reserved, b->va_reserved, sizeof(a->va_reserved)) == 0;
}

static int hobot_hevc_encode_picture_supported(
    const VAEncPictureParameterBufferHEVC *pic
)
{
    if (!pic || (pic->pic_fields.bits.coding_type != 1 &&
                 pic->pic_fields.bits.coding_type != 2) ||
        (pic->pic_fields.bits.idr_pic_flag &&
         pic->pic_fields.bits.coding_type != 1) ||
        pic->num_tile_columns_minus1 != 0 || pic->num_tile_rows_minus1 != 0 ||
        pic->pic_fields.bits.dependent_slice_segments_enabled_flag ||
        pic->pic_fields.bits.weighted_pred_flag ||
        pic->pic_fields.bits.weighted_bipred_flag ||
        pic->pic_fields.bits.transquant_bypass_enabled_flag ||
        pic->pic_fields.bits.tiles_enabled_flag ||
        pic->pic_fields.bits.entropy_coding_sync_enabled_flag ||
        pic->pic_fields.bits.scaling_list_data_present_flag ||
        pic->pic_fields.bits.screen_content_flag ||
        pic->pic_fields.bits.enable_gpu_weighted_prediction ||
        pic->pic_fields.bits.reserved != 0 || pic->va_byte_reserved != 0 ||
        pic->scc_fields.value != 0 || pic->pic_init_qp > 51 ||
        pic->num_ref_idx_l0_default_active_minus1 != 0 ||
        pic->num_ref_idx_l1_default_active_minus1 != 0 ||
        pic->slice_pic_parameter_set_id != 0 ||
        pic->pps_cb_qp_offset != 0 || pic->pps_cr_qp_offset != 0 ||
        pic->diff_cu_qp_delta_depth != 0 ||
        pic->log2_parallel_merge_level_minus2 > 4 ||
        pic->last_picture > (HEVC_LAST_PICTURE_EOSEQ | HEVC_LAST_PICTURE_EOSTREAM) ||
        pic->pic_fields.bits.sign_data_hiding_enabled_flag ||
        pic->pic_fields.bits.constrained_intra_pred_flag ||
        pic->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag ||
        pic->pic_fields.bits.no_output_of_prior_pics_flag)
        return 0;

    for (size_t i = 0; i < sizeof(pic->va_reserved) / sizeof(pic->va_reserved[0]); i++) {
        if (pic->va_reserved[i] != 0)
            return 0;
    }
    for (size_t i = 0; i < sizeof(pic->decoded_curr_pic.va_reserved) /
                            sizeof(pic->decoded_curr_pic.va_reserved[0]); i++) {
        if (pic->decoded_curr_pic.va_reserved[i] != 0)
            return 0;
    }
    for (size_t frame = 0; frame < 15; frame++) {
        for (size_t i = 0; i < sizeof(pic->reference_frames[frame].va_reserved) /
                                sizeof(pic->reference_frames[frame].va_reserved[0]); i++) {
            if (pic->reference_frames[frame].va_reserved[i] != 0)
                return 0;
        }
    }
    return 1;
}

static int hobot_hevc_encode_picture_static_equal(
    const VAEncPictureParameterBufferHEVC *a,
    const VAEncPictureParameterBufferHEVC *b
)
{
    return a && b &&
           a->pic_init_qp == b->pic_init_qp &&
           a->diff_cu_qp_delta_depth == b->diff_cu_qp_delta_depth &&
           a->pps_cb_qp_offset == b->pps_cb_qp_offset &&
           a->pps_cr_qp_offset == b->pps_cr_qp_offset &&
           a->num_tile_columns_minus1 == b->num_tile_columns_minus1 &&
           a->num_tile_rows_minus1 == b->num_tile_rows_minus1 &&
           a->log2_parallel_merge_level_minus2 ==
               b->log2_parallel_merge_level_minus2 &&
           a->num_ref_idx_l0_default_active_minus1 ==
               b->num_ref_idx_l0_default_active_minus1 &&
           a->num_ref_idx_l1_default_active_minus1 ==
               b->num_ref_idx_l1_default_active_minus1 &&
           a->slice_pic_parameter_set_id == b->slice_pic_parameter_set_id &&
           a->pic_fields.bits.transform_skip_enabled_flag ==
               b->pic_fields.bits.transform_skip_enabled_flag &&
           a->pic_fields.bits.cu_qp_delta_enabled_flag ==
               b->pic_fields.bits.cu_qp_delta_enabled_flag &&
           a->scc_fields.value == b->scc_fields.value;
}

static int hobot_hevc_encode_slice_supported(
    const VAEncSequenceParameterBufferHEVC *seq,
    const VAEncPictureParameterBufferHEVC *pic,
    const VAEncSliceParameterBufferHEVC *slice,
    int context_width,
    int context_height
)
{
    if (!hobot_hevc_encode_picture_supported(pic) || !slice ||
        !hobot_hevc_encode_sequence_supported(seq, context_width, context_height) ||
        (slice->slice_type != 1 && slice->slice_type != 2) ||
        slice->slice_pic_parameter_set_id != pic->slice_pic_parameter_set_id ||
        slice->slice_segment_address != 0 ||
        !slice->slice_fields.bits.last_slice_of_pic_flag ||
        slice->slice_fields.bits.dependent_slice_segment_flag ||
        slice->slice_fields.bits.colour_plane_id != 0 ||
        slice->slice_fields.bits.slice_sao_luma_flag ||
        slice->slice_fields.bits.slice_sao_chroma_flag ||
        slice->slice_fields.bits.num_ref_idx_active_override_flag ||
        slice->slice_fields.bits.mvd_l1_zero_flag ||
        slice->slice_fields.bits.cabac_init_flag ||
        slice->slice_fields.bits.slice_deblocking_filter_disabled_flag != 0 ||
        slice->slice_fields.bits.slice_loop_filter_across_slices_enabled_flag ||
        slice->max_num_merge_cand != 5 || slice->slice_qp_delta != 0 ||
        slice->num_ref_idx_l0_active_minus1 != 0 ||
        slice->num_ref_idx_l1_active_minus1 != 0 ||
        (slice->slice_fields.bits.slice_temporal_mvp_enabled_flag &&
         !seq->seq_fields.bits.sps_temporal_mvp_enabled_flag) ||
        (pic->pic_fields.bits.coding_type == 1 &&
         slice->slice_fields.bits.slice_temporal_mvp_enabled_flag) ||
        slice->slice_cb_qp_offset != 0 || slice->slice_cr_qp_offset != 0 ||
        slice->slice_beta_offset_div2 != 0 || slice->slice_tc_offset_div2 != 0 ||
        slice->pred_weight_table_bit_offset != 0 ||
        slice->pred_weight_table_bit_length != 0 ||
        (slice->slice_fields.value & ~UINT32_C(0x3fff)) != 0)
        return 0;

    unsigned int ctb_log2 = 3u + seq->log2_min_luma_coding_block_size_minus3 +
                            seq->log2_diff_max_min_luma_coding_block_size;
    if (ctb_log2 > 6)
        return 0;
    uint64_t ctb_size = UINT64_C(1) << ctb_log2;
    uint64_t ctb_columns = ((uint64_t)seq->pic_width_in_luma_samples + ctb_size - 1u) /
                           ctb_size;
    uint64_t ctb_rows = ((uint64_t)seq->pic_height_in_luma_samples + ctb_size - 1u) /
                        ctb_size;
    if (ctb_columns == 0 || ctb_rows == 0 ||
        ctb_columns > UINT32_MAX / ctb_rows ||
        slice->num_ctu_in_slice != ctb_columns * ctb_rows ||
        (pic->pic_fields.bits.coding_type == 1 && slice->slice_type != 2) ||
        (pic->pic_fields.bits.coding_type == 2 && slice->slice_type != 1))
        return 0;

    for (size_t i = 0; i < sizeof(slice->va_reserved) / sizeof(slice->va_reserved[0]); i++) {
        if (slice->va_reserved[i] != 0)
            return 0;
    }
    return 1;
}

static int generate_h264_pps(VAPictureParameterBufferH264 *pic,
                             unsigned int default_l0_active_minus1,
                             uint8_t *out, int max_len) {
    uint8_t rbsp[256] = {0};
    BitWriter bw = {rbsp, 0};

    bw_put_ue(&bw, 0);         // pic_parameter_set_id
    bw_put_ue(&bw, 0);         // seq_parameter_set_id
    bw_put_bits(&bw, pic->pic_fields.bits.entropy_coding_mode_flag, 1);
    bw_put_bits(&bw, pic->pic_fields.bits.pic_order_present_flag, 1);
    bw_put_ue(&bw, 0);         // num_slice_groups_minus1
    bw_put_ue(&bw, default_l0_active_minus1); // VA-API omits the PPS default
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

static int generate_hevc_vps(const VAPictureParameterBufferHEVC *pic,
                             int profile_idc, uint8_t *out, int max_len) {
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
    bw_put_bits(&bw, HOBOT_HEVC_LEVEL_IDC, 8); // HEVC Main Level 5.1

    bw_put_bit(&bw, 0);           // vps_sub_layer_ordering_info_present_flag
    bw_put_ue(&bw, pic->sps_max_dec_pic_buffering_minus1);
    bw_put_ue(&bw, pic->pic_fields.bits.NoPicReorderingFlag ? 0 : 2);
    bw_put_ue(&bw, 1);            // vps_max_latency_increase_plus1
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
    bw_put_bits(&bw, HOBOT_HEVC_LEVEL_IDC, 8); // HEVC Main Level 5.1

    bw_put_ue(&bw, 0);            // sps_seq_parameter_set_id
    bw_put_ue(&bw, pic->pic_fields.bits.chroma_format_idc);
    bw_put_ue(&bw, pic->pic_width_in_luma_samples);
    bw_put_ue(&bw, pic->pic_height_in_luma_samples);
    bw_put_bit(&bw, 0);           // conformance_window_flag
    bw_put_ue(&bw, pic->bit_depth_luma_minus8);
    bw_put_ue(&bw, pic->bit_depth_chroma_minus8);
    bw_put_ue(&bw, pic->log2_max_pic_order_cnt_lsb_minus4);
    bw_put_bit(&bw, 0);           // sps_sub_layer_ordering_info_present_flag
    bw_put_ue(&bw, pic->sps_max_dec_pic_buffering_minus1);
    bw_put_ue(&bw, pic->pic_fields.bits.NoPicReorderingFlag ? 0 : 2);
    bw_put_ue(&bw, 1);            // sps_max_latency_increase_plus1

    bw_put_ue(&bw, pic->log2_min_luma_coding_block_size_minus3);
    bw_put_ue(&bw, pic->log2_diff_max_min_luma_coding_block_size);
    bw_put_ue(&bw, pic->log2_min_transform_block_size_minus2);
    bw_put_ue(&bw, pic->log2_diff_max_min_transform_block_size);
    bw_put_ue(&bw, pic->max_transform_hierarchy_depth_inter);
    bw_put_ue(&bw, pic->max_transform_hierarchy_depth_intra);

    bw_put_bit(&bw, pic->pic_fields.bits.scaling_list_enabled_flag);
    bw_put_bit(&bw, pic->pic_fields.bits.amp_enabled_flag);
    bw_put_bit(&bw, pic->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag);
    bw_put_bit(&bw, pic->pic_fields.bits.pcm_enabled_flag);

    /* Reconstruct only the validated one-reference and two-set subsets.
     * Other SPS RPS layouts are accepted only on independent IDR pictures. */
    if (pic->num_short_term_ref_pic_sets == 2 ||
        pic->num_short_term_ref_pic_sets == 1) {
        bw_put_ue(&bw, 2);        // num_short_term_ref_pic_sets
        bw_put_ue(&bw, 1);        // RPS 0: one negative reference
        bw_put_ue(&bw, 0);        // delta_poc_s0_minus1[0]
        bw_put_bit(&bw, 1);       // used_by_curr_pic_s0_flag[0]
        bw_put_ue(&bw, 1);        // RPS 1: one negative reference
        bw_put_ue(&bw, 0);        // delta_poc_s0_minus1[0]
        bw_put_bit(&bw, 0);       // used_by_curr_pic_s0_flag[0]
    } else {
        bw_put_ue(&bw, 0);        // num_short_term_ref_pic_sets
    }
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
    int num_attribs;
    VAConfigAttrib attribs[MAX_CONFIG_ATTRIBUTES];
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
    int decode_pending;
    int decode_error;
    hb_mem_graphic_buf_t preallocated_gbuf;
    int has_preallocated;
    void *raw_data;
    uint32_t raw_data_size;
    int raw_data_valid;
    int raw_data_dirty;
    uint32_t lock_count;
    VAContextID context_id;
    VAContextID output_context_id;
} HobotSurface;

/* Internal Buffer Object */
typedef struct {
    VABufferID id;
    int allocated;
    int is_derived;
    VABufferType type;
    unsigned int size;
    unsigned int capacity;
    unsigned int element_size;
    unsigned int num_elements;
    uint32_t map_count;
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
    int encoder_init_deferred;
    int h264_sequence_valid;
    int h264_constrained_baseline_headers_patched;
    VAEncSequenceParameterBufferH264 h264_sequence;
    int hevc_encode_sequence_valid;
    VAEncSequenceParameterBufferHEVC hevc_encode_sequence;
    int hevc_encode_picture_valid;
    VAEncPictureParameterBufferHEVC hevc_encode_picture;
    int hevc_sps_geometry_seen;
    int decode_picture_active;
    int decode_slice_fragment_open;
    VAPictureParameterBufferHEVC hevc_decode_picture;
    int hevc_decode_picture_valid;
    VAPictureParameterBufferJPEGBaseline jpeg_decode_picture;
    int jpeg_decode_picture_valid;
    VAIQMatrixBufferJPEGBaseline jpeg_decode_qmatrix;
    uint8_t jpeg_decode_qmatrix_valid[4];
    VAHuffmanTableBufferJPEGBaseline jpeg_decode_huffman;
    uint8_t jpeg_decode_huffman_valid[2];
    VASliceParameterBufferJPEGBaseline jpeg_decode_slice;
    int jpeg_decode_slice_valid;
    int jpeg_decode_data_submitted;
    media_codec_context_t vpu_ctx;
    int vpu_initialized;
    int vpu_running;
    int decode_failed;
    int sync_active;
    int cleanup_orphaned;
    int dec_out_buf_valid;
    media_codec_buffer_t dec_out_buf;
    int encoder_picture_active;
    int encoder_failed;
    int enc_in_buf_valid;
    media_codec_buffer_t enc_in_buf;
    int enc_out_buf_valid;
    media_codec_buffer_t enc_out_buf;
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
    int watchdog_trace_countdown;
    int watchdog_anomaly_count;
    time_t last_anomaly_sec;
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
    pthread_mutex_t mutex;
    pthread_cond_t sync_cond;
    int sync_cond_initialized;
} HobotDriverData;

static void hobot_signal_sync_waiters(HobotDriverData *drv) {
    if (drv->sync_cond_initialized)
        pthread_cond_broadcast(&drv->sync_cond);
}

static int hobot_context_has_locked_surfaces(const HobotDriverData *drv,
                                             VAContextID context) {
    for (int i = 1; i < MAX_SURFACES; i++) {
        const HobotSurface *surf = &drv->surfaces[i];
        if (surf->allocated && surf->lock_count > 0 &&
            (surf->context_id == context || surf->output_context_id == context))
            return 1;
    }
    return 0;
}

static int hobot_surface_has_active_encoder(const HobotDriverData *drv,
                                            VASurfaceID surface) {
    for (int i = 1; i < MAX_CONTEXTS; i++) {
        const HobotContext *hctx = &drv->contexts[i];
        if (hctx->allocated && hctx->is_encoder && hctx->encoder_picture_active &&
            hctx->current_render_target == surface)
            return 1;
    }
    return 0;
}

static void hobot_abort_pending_decode_picture(HobotDriverData *drv,
                                                HobotContext *hctx) {
    VASurfaceID surface = hctx->current_render_target;
    if (surface > 0 && surface < MAX_SURFACES && drv->surfaces[surface].allocated) {
        drv->surfaces[surface].decode_pending = 0;
        drv->surfaces[surface].decode_error = 1;
    }
    if (hctx->sub_tail != hctx->sub_head &&
        hctx->submitted_surfaces[(hctx->sub_tail - 1) % 128] == surface) {
        hctx->sub_tail--;
    }
    hctx->current_render_target = VA_INVALID_SURFACE;
    hctx->decode_picture_active = 0;
    hctx->decode_slice_fragment_open = 0;
    hobot_signal_sync_waiters(drv);
}

static int hobot_has_annexb_start_code(const uint8_t *data, size_t size) {
    return data && ((size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) ||
                    (size >= 4 && data[0] == 0 && data[1] == 0 &&
                     data[2] == 0 && data[3] == 1));
}

static int hobot_validate_h264_slice_group(const HobotBuffer *params,
                                            const HobotBuffer *data,
                                            int fragment_open,
                                            int *fragment_open_after) {
    if (!params || !data || !fragment_open_after || !params->data || !data->data ||
        params->element_size < sizeof(VASliceParameterBufferH264) ||
        params->num_elements == 0 ||
        params->num_elements > params->size / params->element_size)
        return 0;

    for (unsigned int i = 0; i < params->num_elements; i++) {
        const VASliceParameterBufferH264 *slice =
            (const VASliceParameterBufferH264 *)
                ((const uint8_t *)params->data + (size_t)i * params->element_size);
        size_t offset = slice->slice_data_offset;
        size_t size = slice->slice_data_size;
        if (size == 0 || offset > data->size || size > data->size - offset)
            return 0;

        switch (slice->slice_data_flag) {
        case VA_SLICE_DATA_FLAG_ALL:
            if (fragment_open)
                return 0;
            break;
        case VA_SLICE_DATA_FLAG_BEGIN:
            if (fragment_open)
                return 0;
            fragment_open = 1;
            break;
        case VA_SLICE_DATA_FLAG_MIDDLE:
            if (!fragment_open)
                return 0;
            break;
        case VA_SLICE_DATA_FLAG_END:
            if (!fragment_open)
                return 0;
            fragment_open = 0;
            break;
        default:
            return 0;
        }
    }
    *fragment_open_after = fragment_open;
    return 1;
}

static int hobot_recycle_decoder_input(HobotContext *hctx) {
    if (!hctx->dec_in_buf_valid) return 0;
    int ret = hb_mm_mc_queue_input_buffer(&hctx->vpu_ctx, &hctx->dec_in_buf, 50);
    if (ret != 0) {
        hctx->decode_failed = 1;
        return ret;
    }
    hctx->dec_in_buf_valid = 0;
    hctx->dec_in_buf_offset = 0;
    return 0;
}

static int hobot_recycle_decoder_output(HobotContext *hctx,
                                        media_codec_buffer_t *buffer) {
    int ret = hb_mm_mc_queue_output_buffer(&hctx->vpu_ctx, buffer, 50);
    if (ret != 0)
        ret = hb_mm_mc_queue_output_buffer(&hctx->vpu_ctx, buffer, 200);
    if (ret == 0) return 0;

    /* The caller holds the driver mutex; a failed recycle poisons the decoder,
     * and later sync attempts must not dequeue another output buffer. */
    if (!hctx->dec_out_buf_valid) {
        hctx->dec_out_buf = *buffer;
        hctx->dec_out_buf_valid = 1;
    } else {
        fprintf(stderr, "[HOBOT-VA] decoder output ownership invariant violated: pending buffer already retained\n");
    }
    hctx->decode_failed = 1;
    return ret;
}

static int hobot_retry_decoder_output(HobotContext *hctx) {
    if (!hctx->dec_out_buf_valid) return 0;
    int ret = hb_mm_mc_queue_output_buffer(&hctx->vpu_ctx, &hctx->dec_out_buf, 50);
    if (ret != 0)
        ret = hb_mm_mc_queue_output_buffer(&hctx->vpu_ctx, &hctx->dec_out_buf, 200);
    if (ret == 0) {
        hctx->dec_out_buf_valid = 0;
        memset(&hctx->dec_out_buf, 0, sizeof(hctx->dec_out_buf));
    }
    return ret;
}

static int hobot_recycle_encoder_input(HobotContext *hctx,
                                      media_codec_buffer_t *buffer) {
    int ret = hb_mm_mc_queue_input_buffer(&hctx->vpu_ctx, buffer, 50);
    if (ret != 0)
        ret = hb_mm_mc_queue_input_buffer(&hctx->vpu_ctx, buffer, 200);
    if (ret == 0) return 0;

    if (!hctx->enc_in_buf_valid) {
        hctx->enc_in_buf = *buffer;
        hctx->enc_in_buf_valid = 1;
    } else {
        fprintf(stderr, "[HOBOT-VA] encoder input ownership invariant violated: pending buffer already retained\n");
    }
    hctx->encoder_failed = 1;
    return ret;
}

static int hobot_retry_encoder_input(HobotContext *hctx) {
    if (!hctx->enc_in_buf_valid) return 0;
    int ret = hb_mm_mc_queue_input_buffer(&hctx->vpu_ctx, &hctx->enc_in_buf, 50);
    if (ret != 0)
        ret = hb_mm_mc_queue_input_buffer(&hctx->vpu_ctx, &hctx->enc_in_buf, 200);
    if (ret == 0) {
        hctx->enc_in_buf_valid = 0;
        memset(&hctx->enc_in_buf, 0, sizeof(hctx->enc_in_buf));
    }
    return ret;
}

static int hobot_recycle_encoder_output(HobotContext *hctx,
                                       media_codec_buffer_t *buffer) {
    int ret = hb_mm_mc_queue_output_buffer(&hctx->vpu_ctx, buffer, 50);
    if (ret != 0)
        ret = hb_mm_mc_queue_output_buffer(&hctx->vpu_ctx, buffer, 200);
    if (ret == 0) return 0;

    if (!hctx->enc_out_buf_valid) {
        hctx->enc_out_buf = *buffer;
        hctx->enc_out_buf_valid = 1;
    } else {
        fprintf(stderr, "[HOBOT-VA] encoder output ownership invariant violated: pending buffer already retained\n");
    }
    hctx->encoder_failed = 1;
    return ret;
}

static int hobot_retry_encoder_output(HobotContext *hctx) {
    if (!hctx->enc_out_buf_valid) return 0;
    int ret = hb_mm_mc_queue_output_buffer(&hctx->vpu_ctx, &hctx->enc_out_buf, 50);
    if (ret != 0)
        ret = hb_mm_mc_queue_output_buffer(&hctx->vpu_ctx, &hctx->enc_out_buf, 200);
    if (ret == 0) {
        hctx->enc_out_buf_valid = 0;
        memset(&hctx->enc_out_buf, 0, sizeof(hctx->enc_out_buf));
    }
    return ret;
}

static void hobot_finish_encoder_picture(HobotContext *hctx, int failed) {
    hctx->encoder_picture_active = 0;
    hctx->current_render_target = VA_INVALID_SURFACE;
    hctx->enc_coded_buf = 0;
    if (failed)
        hctx->encoder_failed = 1;
}

static VAStatus hobot_apply_encoder_rate_control(
    HobotContext *hctx,
    const mc_rate_control_params_t *previous_params
) {
    if (hctx->encoder_init_deferred)
        return VA_STATUS_SUCCESS;

    int ret = hb_mm_mc_set_rate_control_config(
        &hctx->vpu_ctx, &hctx->vpu_ctx.video_enc_params.rc_params);
    if (ret == 0)
        return VA_STATUS_SUCCESS;

    hctx->vpu_ctx.video_enc_params.rc_params = *previous_params;
    fprintf(stderr, "[HOBOT-VA] rate-control update failed for ctx=%u: %d\n",
            hctx->id, ret);
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

static int hobot_hrd_vbv_window_ms(
    uint64_t bit_rate_bps,
    const VAEncMiscParameterHRD *hrd,
    int32_t *window_ms
) {
    if (!hrd || !window_ms || hrd->buffer_size == 0 ||
        hrd->initial_buffer_fullness > hrd->buffer_size)
        return -1;
    if (bit_rate_bps == 0)
        return 0;

    uint64_t numerator = (uint64_t)hrd->buffer_size * 1000u;
    uint64_t duration_ms = (numerator + bit_rate_bps - 1u) / bit_rate_bps;
    if (duration_ms < 10u || duration_ms > 3000u)
        return 0;

    *window_ms = (int32_t)duration_ms;
    return 1;
}

static int hobot_h264_level_supported(uint8_t level_idc) {
    switch (level_idc) {
    case 0:
    case MC_H264_LEVEL1:
    case MC_H264_LEVEL1b:
    case MC_H264_LEVEL1_1:
    case MC_H264_LEVEL1_2:
    case MC_H264_LEVEL1_3:
    case MC_H264_LEVEL2:
    case MC_H264_LEVEL2_1:
    case MC_H264_LEVEL2_2:
    case MC_H264_LEVEL3:
    case MC_H264_LEVEL3_1:
    case MC_H264_LEVEL3_2:
    case MC_H264_LEVEL4:
    case MC_H264_LEVEL4_1:
    case MC_H264_LEVEL4_2:
    case MC_H264_LEVEL5:
    case MC_H264_LEVEL5_1:
    case MC_H264_LEVEL5_2:
        return 1;
    default:
        return 0;
    }
}

static int hobot_h264_sequence_config_equal(
    const VAEncSequenceParameterBufferH264 *a,
    const VAEncSequenceParameterBufferH264 *b
) {
    if (a->level_idc != b->level_idc ||
        a->picture_width_in_mbs != b->picture_width_in_mbs ||
        a->picture_height_in_mbs != b->picture_height_in_mbs ||
        a->seq_fields.bits.chroma_format_idc != b->seq_fields.bits.chroma_format_idc ||
        a->seq_fields.bits.frame_mbs_only_flag != b->seq_fields.bits.frame_mbs_only_flag ||
        a->frame_cropping_flag != b->frame_cropping_flag ||
        a->frame_crop_left_offset != b->frame_crop_left_offset ||
        a->frame_crop_right_offset != b->frame_crop_right_offset ||
        a->frame_crop_top_offset != b->frame_crop_top_offset ||
        a->frame_crop_bottom_offset != b->frame_crop_bottom_offset ||
        a->vui_parameters_present_flag != b->vui_parameters_present_flag)
        return 0;
    if (!a->vui_parameters_present_flag)
        return 1;

    uint32_t vui_mask = 0x0000ffffu;
    if ((a->vui_fields.value & vui_mask) != (b->vui_fields.value & vui_mask))
        return 0;
    if (a->vui_fields.bits.aspect_ratio_info_present_flag &&
        (a->aspect_ratio_idc != b->aspect_ratio_idc ||
         a->sar_width != b->sar_width || a->sar_height != b->sar_height))
        return 0;
    if (a->vui_fields.bits.timing_info_present_flag &&
        (a->num_units_in_tick != b->num_units_in_tick ||
         a->time_scale != b->time_scale))
        return 0;
    return 1;
}

static int hobot_h264_sequence_dimensions(
    const VAEncSequenceParameterBufferH264 *seq,
    uint32_t *coded_width,
    uint32_t *coded_height,
    uint32_t *visible_width,
    uint32_t *visible_height
) {
    if (!seq || !coded_width || !coded_height || !visible_width ||
        !visible_height || seq->picture_width_in_mbs == 0 ||
        seq->picture_height_in_mbs == 0 ||
        seq->seq_fields.bits.chroma_format_idc != 1 ||
        !seq->seq_fields.bits.frame_mbs_only_flag ||
        seq->frame_cropping_flag > 1)
        return 0;

    uint64_t width = (uint64_t)seq->picture_width_in_mbs * 16u;
    uint64_t height = (uint64_t)seq->picture_height_in_mbs * 16u;
    uint64_t crop_width = 0;
    uint64_t crop_height = 0;
    if (seq->frame_cropping_flag) {
        crop_width = ((uint64_t)seq->frame_crop_left_offset +
                      seq->frame_crop_right_offset) * 2u;
        crop_height = ((uint64_t)seq->frame_crop_top_offset +
                       seq->frame_crop_bottom_offset) * 2u;
    }
    if (width > UINT32_MAX || height > UINT32_MAX ||
        crop_width >= width || crop_height >= height)
        return 0;

    *coded_width = (uint32_t)width;
    *coded_height = (uint32_t)height;
    *visible_width = (uint32_t)(width - crop_width);
    *visible_height = (uint32_t)(height - crop_height);
    return (*visible_width & 1u) == 0 && (*visible_height & 1u) == 0;
}

static int hobot_h264_surface_matches_sequence(
    const HobotContext *hctx,
    const HobotSurface *surface
) {
    if (!hctx || !surface || !hctx->h264_sequence_valid)
        return 0;
    uint32_t coded_width, coded_height, visible_width, visible_height;
    if (!hobot_h264_sequence_dimensions(&hctx->h264_sequence,
                                         &coded_width, &coded_height,
                                         &visible_width, &visible_height) ||
        coded_width != (uint32_t)hctx->width ||
        coded_height != (uint32_t)hctx->height)
        return 0;

    return (surface->width >= coded_width && surface->height >= coded_height) ||
           (surface->width == visible_width && surface->height == visible_height);
}

static int hobot_h264_surface_copy_region(
    const HobotContext *hctx,
    const HobotSurface *surface,
    int coded_width,
    int coded_height,
    int *copy_x,
    int *copy_y,
    int *copy_width,
    int *copy_height
) {
    if (!hctx || !surface || !copy_x || !copy_y || !copy_width ||
        !copy_height || coded_width <= 0 || coded_height <= 0 ||
        (coded_width & 1) != 0 || (coded_height & 1) != 0)
        return 0;

    if (surface->width >= (unsigned int)coded_width &&
        surface->height >= (unsigned int)coded_height) {
        *copy_x = 0;
        *copy_y = 0;
        *copy_width = coded_width;
        *copy_height = coded_height;
        return 1;
    }

    if (!hctx->h264_sequence_valid)
        return 0;
    uint32_t sequence_width, sequence_height, visible_width, visible_height;
    if (!hobot_h264_sequence_dimensions(&hctx->h264_sequence,
                                         &sequence_width, &sequence_height,
                                         &visible_width, &visible_height) ||
        sequence_width != (uint32_t)coded_width ||
        sequence_height != (uint32_t)coded_height ||
        surface->width != visible_width || surface->height != visible_height)
        return 0;

    uint64_t x = (uint64_t)hctx->h264_sequence.frame_crop_left_offset * 2u;
    uint64_t y = (uint64_t)hctx->h264_sequence.frame_crop_top_offset * 2u;
    if (x + surface->width > (uint32_t)coded_width ||
        y + surface->height > (uint32_t)coded_height ||
        (x & 1u) != 0 || (y & 1u) != 0)
        return 0;

    *copy_x = (int)x;
    *copy_y = (int)y;
    *copy_width = (int)surface->width;
    *copy_height = (int)surface->height;
    return 1;
}

static int hobot_copy_nv12_to_coded_frame(
    const unsigned char *src_y,
    const unsigned char *src_uv,
    unsigned int src_y_stride,
    unsigned int src_uv_stride,
    unsigned char *dst_y,
    unsigned char *dst_uv,
    int dst_y_stride,
    int dst_uv_stride,
    int coded_width,
    int coded_height,
    int copy_x,
    int copy_y,
    int copy_width,
    int copy_height
) {
    if (!src_y || !src_uv || !dst_y || !dst_uv || coded_width < 2 ||
        coded_height < 2 || dst_y_stride < coded_width ||
        dst_uv_stride < coded_width || src_y_stride < (unsigned int)copy_width ||
        src_uv_stride < (unsigned int)copy_width || (coded_width & 1) != 0 ||
        (coded_height & 1) != 0 || copy_width < 2 || copy_height < 2 ||
        (copy_width & 1) != 0 || (copy_height & 1) != 0 || copy_x < 0 ||
        copy_y < 0 || (copy_x & 1) != 0 || (copy_y & 1) != 0 ||
        copy_x > coded_width - copy_width ||
        copy_y > coded_height - copy_height)
        return -1;

    for (int row = 0; row < coded_height; row++) {
        int source_row = row - copy_y;
        if (source_row < 0)
            source_row = 0;
        else if (source_row >= copy_height)
            source_row = copy_height - 1;
        const unsigned char *source = src_y +
            (size_t)source_row * src_y_stride;
        unsigned char *destination = dst_y +
            (size_t)row * (size_t)dst_y_stride;
        if (copy_x > 0)
            memset(destination, source[0], (size_t)copy_x);
        memcpy(destination + copy_x, source, (size_t)copy_width);
        int right_start = copy_x + copy_width;
        if (right_start < coded_width)
            memset(destination + right_start, source[copy_width - 1],
                   (size_t)(coded_width - right_start));
    }

    for (int row = 0; row < coded_height / 2; row++) {
        int source_row = row - copy_y / 2;
        if (source_row < 0)
            source_row = 0;
        else if (source_row >= copy_height / 2)
            source_row = copy_height / 2 - 1;
        const unsigned char *source = src_uv +
            (size_t)source_row * src_uv_stride;
        unsigned char *destination = dst_uv +
            (size_t)row * (size_t)dst_uv_stride;
        for (int x = 0; x < copy_x; x += 2) {
            destination[x] = source[0];
            destination[x + 1] = source[1];
        }
        memcpy(destination + copy_x, source, (size_t)copy_width);
        for (int x = copy_x + copy_width; x < coded_width; x += 2) {
            destination[x] = source[copy_width - 2];
            destination[x + 1] = source[copy_width - 1];
        }
    }
    return 0;
}

static int hobot_h264_surface_alignment_candidate(
    const HobotContext *hctx,
    const HobotSurface *surface
) {
    if (!hctx || !surface || hctx->width <= 0 || hctx->height <= 0 ||
        (hctx->width & 15) != 0 || (hctx->height & 15) != 0 ||
        (surface->width & 1u) != 0 || (surface->height & 1u) != 0)
        return 0;

    uint32_t width_diff = surface->width > (uint32_t)hctx->width ?
        surface->width - (uint32_t)hctx->width :
        (uint32_t)hctx->width - surface->width;
    uint32_t height_diff = surface->height > (uint32_t)hctx->height ?
        surface->height - (uint32_t)hctx->height :
        (uint32_t)hctx->height - surface->height;
    return width_diff < 16 && height_diff < 16;
}

static int hobot_hevc_surface_alignment_candidate(
    const HobotContext *hctx,
    const HobotSurface *surface
) {
    if (!hctx || !surface || hctx->width <= 0 || hctx->height <= 0 ||
        (surface->width & 1u) != 0 || (surface->height & 1u) != 0)
        return 0;

    uint32_t width_diff = surface->width > (uint32_t)hctx->width ?
        surface->width - (uint32_t)hctx->width :
        (uint32_t)hctx->width - surface->width;
    uint32_t height_diff = surface->height > (uint32_t)hctx->height ?
        surface->height - (uint32_t)hctx->height :
        (uint32_t)hctx->height - surface->height;
    return width_diff < 16 && height_diff < 16;
}

static int hobot_h264_sequence_frame_rate(
    const VAEncSequenceParameterBufferH264 *seq,
    uint32_t *fps
) {
    if (!seq->vui_parameters_present_flag ||
        !seq->vui_fields.bits.timing_info_present_flag ||
        seq->num_units_in_tick == 0 || seq->time_scale == 0)
        return 0;

    uint64_t denominator = (uint64_t)seq->num_units_in_tick * 2u;
    uint64_t rounded = ((uint64_t)seq->time_scale + denominator / 2u) / denominator;
    /* The media-codec H.264 rate-control interface accepts 1..240 fps. */
    if (rounded == 0 || rounded > 240u)
        return -1;
    *fps = (uint32_t)rounded;
    return 1;
}

static int hobot_h264_sdk_profile(VAProfile profile,
                                  mc_h264_profile_t *sdk_profile)
{
    if (!sdk_profile)
        return 0;
    switch (profile) {
    case VAProfileH264ConstrainedBaseline:
        *sdk_profile = MC_H264_PROFILE_BP;
        return 1;
    case VAProfileH264Main:
        *sdk_profile = MC_H264_PROFILE_MP;
        return 1;
    case VAProfileH264High:
        *sdk_profile = MC_H264_PROFILE_HP;
        return 1;
    default:
        return 0;
    }
}

static int hobot_start_deferred_h264_encoder(HobotContext *hctx) {
    if (!hctx->encoder_init_deferred)
        return hctx->vpu_running ? 0 : -1;

    media_codec_context_t *mctx = &hctx->vpu_ctx;
    /* VA-API supplies no per-surface timestamp on this path. */
    mctx->video_enc_params.enable_user_pts = 0;
    if (hctx->h264_sequence_valid) {
        const VAEncSequenceParameterBufferH264 *seq = &hctx->h264_sequence;
        uint32_t coded_width, coded_height, visible_width, visible_height;
        if (!hobot_h264_sequence_dimensions(seq, &coded_width, &coded_height,
                                            &visible_width, &visible_height) ||
            coded_width != (uint32_t)hctx->width ||
            coded_height != (uint32_t)hctx->height) {
            fprintf(stderr, "[HOBOT-VA] deferred H.264 sequence dimensions no longer match ctx=%u\n",
                    hctx->id);
            hctx->encoder_init_deferred = 0;
            hctx->encoder_failed = 1;
            return -1;
        }
        mctx->video_enc_params.h264_enc_config.h264_level =
            (mc_h264_level_t)seq->level_idc;
        mctx->video_enc_params.frame_cropping_flag = seq->frame_cropping_flag;
        memset(&mctx->video_enc_params.crop_rect, 0,
               sizeof(mctx->video_enc_params.crop_rect));
        if (seq->frame_cropping_flag) {
            mctx->video_enc_params.crop_rect.x_pos =
                seq->frame_crop_left_offset * 2u;
            mctx->video_enc_params.crop_rect.y_pos = seq->frame_crop_top_offset * 2u;
            mctx->video_enc_params.crop_rect.width = visible_width;
            mctx->video_enc_params.crop_rect.height = visible_height;
        }
    }

    mc_h264_profile_t sdk_profile;
    if (!hobot_h264_sdk_profile(hctx->profile, &sdk_profile)) {
        fprintf(stderr, "[HOBOT-VA] unsupported H.264 encoder profile for ctx=%u: %d\n",
                hctx->id, hctx->profile);
        hctx->encoder_init_deferred = 0;
        hctx->encoder_failed = 1;
        return -1;
    }
    mctx->video_enc_params.h264_enc_config.h264_profile = sdk_profile;

    int ret = hb_mm_mc_initialize(mctx);
    if (ret != 0)
        goto fail;
    hctx->vpu_initialized = 1;

    ret = hb_mm_mc_configure(mctx);
    if (ret != 0)
        goto fail;

    if (hctx->profile == VAProfileH264ConstrainedBaseline) {
        mc_h264_entropy_params_t entropy = { .entropy_coding_mode = 0 };
        mc_video_transform_params_t transform = {0};
        transform.h264_transform.transform_8x8_enable = 0;
        ret = hb_mm_mc_set_entropy_config(mctx, &entropy);
        if (ret != 0)
            goto fail;
        ret = hb_mm_mc_set_transform_config(mctx, &transform);
        if (ret != 0)
            goto fail;
    }

    mc_video_vui_params_t vui;
    memset(&vui, 0, sizeof(vui));
    ret = hb_mm_mc_get_vui_config(mctx, &vui);
    if (ret != 0)
        goto fail;

    vui.h264_vui.video_signal_type_present_flag = 1;
    vui.h264_vui.video_format = 5;
    vui.h264_vui.video_full_range_flag = 0;
    vui.h264_vui.aspect_ratio_info_present_flag = 0;
    vui.h264_vui.aspect_ratio_idc = 1;
    vui.h264_vui.sar_width = 1;
    vui.h264_vui.sar_height = 1;
    if (hctx->h264_sequence_valid) {
        const VAEncSequenceParameterBufferH264 *seq = &hctx->h264_sequence;
        vui.h264_vui.aspect_ratio_info_present_flag =
            seq->vui_parameters_present_flag &&
            seq->vui_fields.bits.aspect_ratio_info_present_flag;
        if (vui.h264_vui.aspect_ratio_info_present_flag) {
            vui.h264_vui.aspect_ratio_idc = seq->aspect_ratio_idc;
            if (seq->aspect_ratio_idc == 255) {
                vui.h264_vui.sar_width = (uint16_t)seq->sar_width;
                vui.h264_vui.sar_height = (uint16_t)seq->sar_height;
            }
        }
        vui.h264_vui.vui_timing_info_present_flag =
            seq->vui_parameters_present_flag &&
            seq->vui_fields.bits.timing_info_present_flag;
        if (vui.h264_vui.vui_timing_info_present_flag) {
            vui.h264_vui.vui_num_units_in_tick = seq->num_units_in_tick;
            vui.h264_vui.vui_time_scale = seq->time_scale;
            vui.h264_vui.vui_fixed_frame_rate_flag =
                seq->vui_fields.bits.fixed_frame_rate_flag;
        }
        vui.h264_vui.bitstream_restriction_flag =
            seq->vui_parameters_present_flag &&
            seq->vui_fields.bits.bitstream_restriction_flag;
    }
    ret = hb_mm_mc_set_vui_config(mctx, &vui);
    if (ret != 0)
        goto fail;

    ret = hb_mm_mc_start(mctx, NULL);
    if (ret != 0)
        goto fail;

    hctx->vpu_running = 1;
    hctx->encoder_init_deferred = 0;
    va_trace("H.264 encoder started: ctx=%u level=%u fps=%u full_range=0",
             hctx->id,
             hctx->h264_sequence_valid ? hctx->h264_sequence.level_idc : 0,
             hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.frame_rate);
    return 0;

fail:
    fprintf(stderr, "[HOBOT-VA] deferred H.264 encoder setup failed for ctx=%u: %d\n",
            hctx->id, ret);
    if (hctx->vpu_running) {
        int stop_ret = hb_mm_mc_stop(mctx);
        if (stop_ret == 0)
            hctx->vpu_running = 0;
        else
            fprintf(stderr, "[HOBOT-VA] deferred encoder stop cleanup failed: %d\n", stop_ret);
    }
    if (hctx->vpu_initialized && !hctx->vpu_running) {
        int release_ret = hb_mm_mc_release(mctx);
        if (release_ret == 0)
            hctx->vpu_initialized = 0;
        else
            fprintf(stderr, "[HOBOT-VA] deferred encoder release cleanup failed: %d\n", release_ret);
    }
    if (hctx->vpu_running || hctx->vpu_initialized)
        hctx->cleanup_orphaned = 1;
    hctx->encoder_init_deferred = 0;
    hctx->encoder_failed = 1;
    return ret;
}

static int hobot_start_deferred_hevc_encoder(HobotContext *hctx)
{
    if (!hctx->encoder_init_deferred)
        return hctx->vpu_running ? 0 : -1;
    if (!hctx->hevc_encode_sequence_valid || !hctx->hevc_encode_picture_valid) {
        fprintf(stderr, "[HOBOT-VA] deferred HEVC encoder has no sequence/picture parameters for ctx=%u\n",
                hctx->id);
        hctx->encoder_init_deferred = 0;
        hctx->encoder_failed = 1;
        return -1;
    }

    media_codec_context_t *mctx = &hctx->vpu_ctx;
    const VAEncSequenceParameterBufferHEVC *seq = &hctx->hevc_encode_sequence;
    const VAEncPictureParameterBufferHEVC *pic = &hctx->hevc_encode_picture;
    if (!hobot_hevc_encode_sequence_supported(seq, hctx->width, hctx->height) ||
        !hobot_hevc_encode_picture_supported(pic)) {
        hctx->encoder_init_deferred = 0;
        hctx->encoder_failed = 1;
        return -1;
    }

    mc_video_codec_enc_params_t *enc = &mctx->video_enc_params;
    if (!hobot_hevc_configure_encoder_geometry(seq, enc)) {
        fprintf(stderr, "[HOBOT-VA] invalid deferred HEVC encoder geometry for ctx=%u\n",
                hctx->id);
        hctx->encoder_init_deferred = 0;
        hctx->encoder_failed = 1;
        return -1;
    }
    enc->enable_user_pts = 0;
    enc->h265_enc_config.main_still_picture_profile_enable = 0;
    enc->h265_enc_config.h265_level =
        (mc_h265_level_t)seq->general_level_idc;
    enc->h265_enc_config.h265_tier = seq->general_tier_flag;
    enc->h265_enc_config.transform_skip_enabled_flag =
        pic->pic_fields.bits.transform_skip_enabled_flag;
    enc->h265_enc_config.lossless_mode = 0;
    enc->h265_enc_config.tmvp_enable =
        seq->seq_fields.bits.sps_temporal_mvp_enabled_flag;
    enc->h265_enc_config.wpp_enable = 0;

    if (enc->rc_params.h265_cbr_params.bit_rate == 0 ||
        enc->rc_params.h265_cbr_params.bit_rate > 700000u) {
        fprintf(stderr, "[HOBOT-VA] unsupported HEVC CBR bitrate for ctx=%u: %u kbps\n",
                hctx->id, enc->rc_params.h265_cbr_params.bit_rate);
        hctx->encoder_init_deferred = 0;
        hctx->encoder_failed = 1;
        return -1;
    }
    enc->rc_params.mode = MC_AV_RC_MODE_H265CBR;
    if (enc->rc_params.h265_cbr_params.frame_rate == 0 ||
        enc->rc_params.h265_cbr_params.frame_rate > 240u) {
        fprintf(stderr, "[HOBOT-VA] unsupported HEVC CBR frame rate for ctx=%u: %u fps\n",
                hctx->id, enc->rc_params.h265_cbr_params.frame_rate);
        hctx->encoder_init_deferred = 0;
        hctx->encoder_failed = 1;
        return -1;
    }
    if (seq->vui_parameters_present_flag &&
        seq->vui_fields.bits.vui_timing_info_present_flag) {
        uint64_t rounded_fps =
            ((uint64_t)seq->vui_time_scale + seq->vui_num_units_in_tick / 2u) /
            seq->vui_num_units_in_tick;
        if (rounded_fps == 0 || rounded_fps > 240u) {
            fprintf(stderr, "[HOBOT-VA] unsupported HEVC VUI frame rate for ctx=%u\n",
                    hctx->id);
            hctx->encoder_init_deferred = 0;
            hctx->encoder_failed = 1;
            return -1;
        }
        enc->rc_params.h265_cbr_params.frame_rate = (uint32_t)rounded_fps;
    }

    int ret = hb_mm_mc_initialize(mctx);
    if (ret != 0)
        goto fail;
    hctx->vpu_initialized = 1;

    mc_h265_sao_params_t sao = {
        .sample_adaptive_offset_enabled_flag = 0
    };
    ret = hb_mm_mc_set_sao_config(mctx, &sao);
    if (ret != 0)
        goto fail;

    ret = hb_mm_mc_configure(mctx);
    if (ret != 0)
        goto fail;

    mc_video_vui_params_t vui;
    memset(&vui, 0, sizeof(vui));
    ret = hb_mm_mc_get_vui_config(mctx, &vui);
    if (ret != 0)
        goto fail;

    vui.h265_vui.video_signal_type_present_flag = 1;
    vui.h265_vui.video_format = 5;
    vui.h265_vui.video_full_range_flag = 0;
    vui.h265_vui.aspect_ratio_info_present_flag =
        seq->vui_parameters_present_flag &&
        seq->vui_fields.bits.aspect_ratio_info_present_flag;
    vui.h265_vui.aspect_ratio_idc = seq->aspect_ratio_idc ? seq->aspect_ratio_idc : 1;
    vui.h265_vui.sar_width = 1;
    vui.h265_vui.sar_height = 1;
    if (vui.h265_vui.aspect_ratio_info_present_flag && seq->aspect_ratio_idc == 255) {
        vui.h265_vui.sar_width = (uint16_t)seq->sar_width;
        vui.h265_vui.sar_height = (uint16_t)seq->sar_height;
    }
    vui.h265_vui.vui_timing_info_present_flag =
        seq->vui_parameters_present_flag &&
        seq->vui_fields.bits.vui_timing_info_present_flag;
    if (vui.h265_vui.vui_timing_info_present_flag) {
        vui.h265_vui.vui_num_units_in_tick = seq->vui_num_units_in_tick;
        vui.h265_vui.vui_time_scale = seq->vui_time_scale;
    }
    ret = hb_mm_mc_set_vui_config(mctx, &vui);
    if (ret != 0)
        goto fail;

    ret = hb_mm_mc_start(mctx, NULL);
    if (ret != 0)
        goto fail;

    hctx->vpu_running = 1;
    hctx->encoder_init_deferred = 0;
    va_trace("HEVC encoder started: ctx=%u level=%u bitrate=%u kbps fps=%u",
             hctx->id, seq->general_level_idc,
             enc->rc_params.h265_cbr_params.bit_rate,
             enc->rc_params.h265_cbr_params.frame_rate);
    return 0;

fail:
    fprintf(stderr, "[HOBOT-VA] deferred HEVC encoder setup failed for ctx=%u: %d\n",
            hctx->id, ret);
    if (hctx->vpu_running) {
        int stop_ret = hb_mm_mc_stop(mctx);
        if (stop_ret == 0)
            hctx->vpu_running = 0;
        else
            fprintf(stderr, "[HOBOT-VA] deferred HEVC stop cleanup failed: %d\n", stop_ret);
    }
    if (hctx->vpu_initialized && !hctx->vpu_running) {
        int release_ret = hb_mm_mc_release(mctx);
        if (release_ret == 0)
            hctx->vpu_initialized = 0;
        else
            fprintf(stderr, "[HOBOT-VA] deferred HEVC release cleanup failed: %d\n", release_ret);
    }
    if (hctx->vpu_running || hctx->vpu_initialized)
        hctx->cleanup_orphaned = 1;
    hctx->encoder_init_deferred = 0;
    hctx->encoder_failed = 1;
    return ret;
}

static const uint8_t jpeg_default_dc_bits[2][16] = {
    {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0},
    {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0}
};
static const uint8_t jpeg_default_dc_values[2][12] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}
};
static const uint8_t jpeg_default_ac_bits[2][16] = {
    {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125},
    {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119}
};
static const uint8_t jpeg_default_ac_values[2][162] = {
    {
        1, 2, 3, 0, 4, 17, 5, 18, 33, 49, 65, 6, 19, 81, 97, 7,
        34, 113, 20, 50, 129, 145, 161, 8, 35, 66, 177, 193, 21, 82,
        209, 240, 36, 51, 98, 114, 130, 9, 10, 22, 23, 24, 25, 26, 37,
        38, 39, 40, 41, 42, 52, 53, 54, 55, 56, 57, 58, 67, 68, 69, 70,
        71, 72, 73, 74, 83, 84, 85, 86, 87, 88, 89, 90, 99, 100, 101,
        102, 103, 104, 105, 106, 115, 116, 117, 118, 119, 120, 121, 122,
        131, 132, 133, 134, 135, 136, 137, 138, 146, 147, 148, 149, 150,
        151, 152, 153, 154, 162, 163, 164, 165, 166, 167, 168, 169, 170,
        178, 179, 180, 181, 182, 183, 184, 185, 186, 194, 195, 196, 197,
        198, 199, 200, 201, 202, 210, 211, 212, 213, 214, 215, 216, 217,
        218, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 241, 242,
        243, 244, 245, 246, 247, 248, 249, 250
    },
    {
        0, 1, 2, 3, 17, 4, 5, 33, 49, 6, 18, 65, 81, 7, 97, 113,
        19, 34, 50, 129, 8, 20, 66, 145, 161, 177, 193, 9, 35, 51, 82,
        240, 21, 98, 114, 209, 10, 22, 36, 52, 225, 37, 241, 23, 24, 25,
        26, 38, 39, 40, 41, 42, 53, 54, 55, 56, 57, 58, 67, 68, 69, 70,
        71, 72, 73, 74, 83, 84, 85, 86, 87, 88, 89, 90, 99, 100, 101,
        102, 103, 104, 105, 106, 115, 116, 117, 118, 119, 120, 121, 122,
        130, 131, 132, 133, 134, 135, 136, 137, 138, 146, 147, 148, 149,
        150, 151, 152, 153, 154, 162, 163, 164, 165, 166, 167, 168, 169,
        170, 178, 179, 180, 181, 182, 183, 184, 185, 186, 194, 195, 196,
        197, 198, 199, 200, 201, 202, 210, 211, 212, 213, 214, 215, 216,
        217, 218, 226, 227, 228, 229, 230, 231, 232, 233, 234, 242, 243,
        244, 245, 246, 247, 248, 249, 250
    }
};

static int hobot_jpeg_huffman_tables_supported(
    const VAHuffmanTableBufferJPEGBaseline *tables
) {
    if (!tables) return 0;
    for (unsigned int table = 0; table < 2; table++) {
        if (tables->load_huffman_table[table] > 1) return 0;
        if (!tables->load_huffman_table[table]) continue;
        const typeof(tables->huffman_table[0]) *actual = &tables->huffman_table[table];
        if (memcmp(actual->num_dc_codes, jpeg_default_dc_bits[table], 16) != 0 ||
            memcmp(actual->dc_values, jpeg_default_dc_values[table], 12) != 0 ||
            memcmp(actual->num_ac_codes, jpeg_default_ac_bits[table], 16) != 0 ||
            memcmp(actual->ac_values, jpeg_default_ac_values[table], 162) != 0 ||
            actual->pad[0] != 0 || actual->pad[1] != 0)
            return 0;
    }
    return 1;
}

static int hobot_jpeg_picture_supported(const HobotContext *hctx,
                                        const VAEncPictureParameterBufferJPEG *pic) {
    if (!hctx || !pic) return 0;
    unsigned int unsupported = 0;
    if (pic->picture_width != hctx->width || pic->picture_height != hctx->height) unsupported |= 1u << 0;
    if (pic->sample_bit_depth != 8) unsupported |= 1u << 1;
    if (pic->num_scan != 1 || pic->num_components != 3) unsupported |= 1u << 2;
    if (pic->pic_flags.bits.profile != 0 || pic->pic_flags.bits.progressive != 0 ||
        pic->pic_flags.bits.huffman != 1 || pic->pic_flags.bits.differential != 0)
        unsupported |= 1u << 3;
    if (pic->component_id[0] != 1 || pic->component_id[1] != 2 || pic->component_id[2] != 3)
        unsupported |= 1u << 4;
    if (pic->quantiser_table_selector[0] != 0 ||
        pic->quantiser_table_selector[1] != 1 ||
        pic->quantiser_table_selector[2] != 1)
        unsupported |= 1u << 5;
    if (unsupported)
        va_trace("vaRenderPicture: JPEG picture unsupported mask=0x%x", unsupported);
    return unsupported == 0;
}

static unsigned int hobot_jpeg_strip_empty_app9(uint8_t *data, unsigned int size) {
    static const uint8_t empty_app9[] = {
        0xff, 0xd8, 0xff, 0xe9, 0x00, 0x04, 0x00, 0x00
    };
    const unsigned int marker_size = sizeof(empty_app9) - 2;

    if (!data || size < sizeof(empty_app9) ||
        memcmp(data, empty_app9, sizeof(empty_app9)) != 0)
        return size;

    memmove(data + 2, data + 2 + marker_size, size - 2 - marker_size);
    return size - marker_size;
}

typedef struct HobotJpegWriter {
    uint8_t *data;
    size_t capacity;
    size_t size;
} HobotJpegWriter;

static int hobot_jpeg_writer_append(HobotJpegWriter *writer,
                                    const void *data, size_t size) {
    if (!writer || (size > 0 && !data) || writer->size > writer->capacity ||
        size > writer->capacity - writer->size)
        return 0;
    if (size > 0)
        memcpy(writer->data + writer->size, data, size);
    writer->size += size;
    return 1;
}

static int hobot_jpeg_writer_byte(HobotJpegWriter *writer, uint8_t value) {
    return hobot_jpeg_writer_append(writer, &value, sizeof(value));
}

static int hobot_jpeg_writer_be16(HobotJpegWriter *writer, uint16_t value) {
    uint8_t bytes[2] = {(uint8_t)(value >> 8), (uint8_t)value};
    return hobot_jpeg_writer_append(writer, bytes, sizeof(bytes));
}

static int hobot_jpeg_writer_segment(HobotJpegWriter *writer, uint8_t marker,
                                     const uint8_t *payload, size_t payload_size) {
    if (payload_size > UINT16_MAX - 2u)
        return 0;
    return hobot_jpeg_writer_byte(writer, 0xff) &&
           hobot_jpeg_writer_byte(writer, marker) &&
           hobot_jpeg_writer_be16(writer, (uint16_t)(payload_size + 2u)) &&
           hobot_jpeg_writer_append(writer, payload, payload_size);
}

static int hobot_jpeg_huffman_decode_table_valid(
    const uint8_t counts[16], const uint8_t *values, size_t values_capacity,
    int ac_table, size_t *value_count
) {
    size_t count = 0;
    int code_slots = 1;
    for (size_t i = 0; i < 16; i++) {
        count += counts[i];
        code_slots = code_slots * 2 - counts[i];
        if (code_slots < 0)
            return 0;
    }
    if (count == 0 || count > values_capacity || code_slots == 0)
        return 0;
    for (size_t i = 0; i < count; i++) {
        if (ac_table) {
            uint8_t run = values[i] >> 4;
            uint8_t size = values[i] & 0x0f;
            if (values[i] != 0x00 && values[i] != 0xf0 &&
                (size == 0 || size > 10 || run > 15))
                return 0;
        } else if (values[i] > 11) {
            return 0;
        }
    }
    *value_count = count;
    return 1;
}

static VAStatus hobot_jpeg_build_decode_header(
    const VAPictureParameterBufferJPEGBaseline *picture,
    const VAIQMatrixBufferJPEGBaseline *qmatrix,
    const VAHuffmanTableBufferJPEGBaseline *huffman,
    const VASliceParameterBufferJPEGBaseline *slice,
    int context_width, int context_height,
    uint8_t *header, size_t header_capacity, size_t *header_size
) {
    if (!picture || !qmatrix || !huffman || !slice || !header || !header_size ||
        context_width <= 0 || context_height <= 0 ||
        picture->picture_width != context_width ||
        picture->picture_height != context_height ||
        picture->picture_width == 0 || picture->picture_height == 0 ||
        picture->num_components != 3 || picture->color_space != 0 ||
        picture->rotation != VA_ROTATION_NONE ||
        slice->slice_data_size == 0 || slice->slice_data_flag != VA_SLICE_DATA_FLAG_ALL ||
        slice->slice_horizontal_position != 0 || slice->slice_vertical_position != 0 ||
        slice->num_components != 3)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    for (size_t i = 0; i < sizeof(picture->va_reserved) / sizeof(picture->va_reserved[0]); i++)
        if (picture->va_reserved[i] != 0)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    for (size_t i = 0; i < sizeof(qmatrix->va_reserved) / sizeof(qmatrix->va_reserved[0]); i++)
        if (qmatrix->va_reserved[i] != 0)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    for (size_t i = 0; i < sizeof(huffman->va_reserved) / sizeof(huffman->va_reserved[0]); i++)
        if (huffman->va_reserved[i] != 0)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    for (size_t i = 0; i < sizeof(slice->va_reserved) / sizeof(slice->va_reserved[0]); i++)
        if (slice->va_reserved[i] != 0)
            return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (picture->components[0].component_id == picture->components[1].component_id ||
        picture->components[0].component_id == picture->components[2].component_id ||
        picture->components[1].component_id == picture->components[2].component_id ||
        picture->components[0].h_sampling_factor != 2 ||
        picture->components[0].v_sampling_factor != 2 ||
        picture->components[1].h_sampling_factor != 1 ||
        picture->components[1].v_sampling_factor != 1 ||
        picture->components[2].h_sampling_factor != 1 ||
        picture->components[2].v_sampling_factor != 1)
        return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;

    uint8_t needed_quant[4] = {0};
    uint8_t needed_huffman[2] = {0};
    uint8_t matched_components[3] = {0};
    for (size_t i = 0; i < 3; i++) {
        unsigned int quant = picture->components[i].quantiser_table_selector;
        if (quant >= 4)
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        needed_quant[quant] = 1;

        int component_index = -1;
        for (size_t j = 0; j < 3; j++) {
            if (slice->components[j].component_selector ==
                picture->components[i].component_id) {
                if (component_index >= 0)
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                component_index = (int)j;
            }
        }
        if (component_index < 0 || matched_components[component_index])
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        matched_components[component_index] = 1;
        const typeof(slice->components[0]) *scan =
            &slice->components[component_index];
        if (scan->dc_table_selector >= 2 || scan->ac_table_selector >= 2)
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        needed_huffman[scan->dc_table_selector] = 1;
        needed_huffman[scan->ac_table_selector] = 1;
    }

    uint64_t mcu_width = ((uint64_t)picture->picture_width + 15u) / 16u;
    uint64_t mcu_height = ((uint64_t)picture->picture_height + 15u) / 16u;
    if (mcu_width == 0 || mcu_height == 0 || mcu_width > UINT32_MAX / mcu_height ||
        slice->num_mcus != mcu_width * mcu_height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    size_t dc_count[2] = {0, 0};
    size_t ac_count[2] = {0, 0};
    for (size_t table = 0; table < 2; table++) {
        if (huffman->load_huffman_table[table] > 1)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        if (!needed_huffman[table])
            continue;
        if (huffman->huffman_table[table].pad[0] != 0 ||
            huffman->huffman_table[table].pad[1] != 0 ||
            !hobot_jpeg_huffman_decode_table_valid(
                huffman->huffman_table[table].num_dc_codes,
                huffman->huffman_table[table].dc_values, 12, 0,
                &dc_count[table]) ||
            !hobot_jpeg_huffman_decode_table_valid(
                huffman->huffman_table[table].num_ac_codes,
                huffman->huffman_table[table].ac_values, 162, 1,
                &ac_count[table]))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    for (size_t table = 0; table < 4; table++) {
        if (needed_quant[table] &&
            memchr(qmatrix->quantiser_table[table], 0,
                   sizeof(qmatrix->quantiser_table[table])))
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    HobotJpegWriter writer = {header, header_capacity, 0};
    static const uint8_t soi[] = {0xff, 0xd8};
    if (!hobot_jpeg_writer_append(&writer, soi, sizeof(soi)))
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

    uint8_t dqt[4 * 65];
    size_t dqt_size = 0;
    for (size_t table = 0; table < 4; table++) {
        if (!needed_quant[table])
            continue;
        dqt[dqt_size++] = (uint8_t)table;
        memcpy(dqt + dqt_size, qmatrix->quantiser_table[table], 64);
        dqt_size += 64;
    }
    if (!hobot_jpeg_writer_segment(&writer, 0xdb, dqt, dqt_size))
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

    uint8_t sof[15];
    size_t offset = 0;
    sof[offset++] = 8;
    sof[offset++] = (uint8_t)(picture->picture_height >> 8);
    sof[offset++] = (uint8_t)picture->picture_height;
    sof[offset++] = (uint8_t)(picture->picture_width >> 8);
    sof[offset++] = (uint8_t)picture->picture_width;
    sof[offset++] = 3;
    for (size_t i = 0; i < 3; i++) {
        sof[offset++] = picture->components[i].component_id;
        sof[offset++] = (uint8_t)((picture->components[i].h_sampling_factor << 4) |
                                  picture->components[i].v_sampling_factor);
        sof[offset++] = picture->components[i].quantiser_table_selector;
    }
    if (!hobot_jpeg_writer_segment(&writer, 0xc0, sof, offset))
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

    uint8_t dht[2 * (1 + 16 + 12 + 1 + 16 + 162)];
    size_t dht_size = 0;
    for (size_t table = 0; table < 2; table++) {
        if (!needed_huffman[table])
            continue;
        dht[dht_size++] = (uint8_t)table;
        memcpy(dht + dht_size, huffman->huffman_table[table].num_dc_codes, 16);
        dht_size += 16;
        memcpy(dht + dht_size, huffman->huffman_table[table].dc_values,
               dc_count[table]);
        dht_size += dc_count[table];
        dht[dht_size++] = (uint8_t)(0x10u | table);
        memcpy(dht + dht_size, huffman->huffman_table[table].num_ac_codes, 16);
        dht_size += 16;
        memcpy(dht + dht_size, huffman->huffman_table[table].ac_values,
               ac_count[table]);
        dht_size += ac_count[table];
    }
    if (!hobot_jpeg_writer_segment(&writer, 0xc4, dht, dht_size))
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

    if (slice->restart_interval > 0) {
        uint8_t dri[2] = {(uint8_t)(slice->restart_interval >> 8),
                          (uint8_t)slice->restart_interval};
        if (!hobot_jpeg_writer_segment(&writer, 0xdd, dri, sizeof(dri)))
            return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    uint8_t sos[10] = {3};
    size_t sos_offset = 1;
    for (size_t i = 0; i < 3; i++) {
        const typeof(slice->components[0]) *scan = &slice->components[i];
        sos[sos_offset++] = scan->component_selector;
        sos[sos_offset++] = (uint8_t)((scan->dc_table_selector << 4) |
                                      scan->ac_table_selector);
    }
    sos[sos_offset++] = 0;
    sos[sos_offset++] = 63;
    sos[sos_offset++] = 0;
    if (!hobot_jpeg_writer_segment(&writer, 0xda, sos, sizeof(sos)))
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

    *header_size = writer.size;
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_apply_jpeg_parameters(
    HobotContext *hctx,
    unsigned int quality,
    const VAQMatrixBufferJPEG *qmatrix,
    const VAEncSliceParameterBufferJPEG *slice
) {
    mc_jpeg_enc_params_t params;
    memset(&params, 0, sizeof(params));
    int ret = hb_mm_mc_get_jpeg_config(&hctx->vpu_ctx, &params);
    if (ret != 0) {
        fprintf(stderr, "[HOBOT-VA] JPEG config query failed for ctx=%u: %d\n",
                hctx->id, ret);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    params.quality_factor = quality;
    if (qmatrix && qmatrix->load_lum_quantiser_matrix)
        memcpy(params.luma_quant_table, qmatrix->lum_quantiser_matrix,
               sizeof(params.luma_quant_table));
    if (qmatrix && qmatrix->load_chroma_quantiser_matrix)
        memcpy(params.chroma_quant_table, qmatrix->chroma_quantiser_matrix,
               sizeof(params.chroma_quant_table));
    if (slice)
        params.restart_interval = slice->restart_interval;

    ret = hb_mm_mc_set_jpeg_config(&hctx->vpu_ctx, &params);
    if (ret != 0) {
        fprintf(stderr, "[HOBOT-VA] JPEG parameter update failed for ctx=%u: %d\n",
                hctx->id, ret);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    hctx->vpu_ctx.video_enc_params.jpeg_enc_config.quality_factor = quality;
    if (slice)
        hctx->vpu_ctx.video_enc_params.jpeg_enc_config.restart_interval =
            slice->restart_interval;
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_finish_surface_sync(HobotDriverData *drv,
                                         HobotContext *hctx,
                                         VAStatus status) {
    hctx->sync_active = 0;
    hobot_signal_sync_waiters(drv);
    return status;
}

static int hobot_ensure_sync_condition(HobotDriverData *drv) {
    if (drv->sync_cond_initialized) return 0;
    if (pthread_cond_init(&drv->sync_cond, NULL) != 0) return -1;
    drv->sync_cond_initialized = 1;
    return 0;
}

static void hobot_detach_context_surfaces(HobotDriverData *drv, VAContextID context) {
    for (int s = 1; s < MAX_SURFACES; s++) {
        HobotSurface *surf = &drv->surfaces[s];
        if (!surf->allocated ||
            (surf->context_id != context && surf->output_context_id != context)) continue;
        if (surf->context_id == context && surf->decode_pending) {
            surf->decode_pending = 0;
            surf->decode_error = 1;
        }
        if (surf->output_context_id == context && surf->has_decoded_frame) {
            surf->has_decoded_frame = 0;
            surf->dma_fd = -1;
            memset(&surf->vpu_out_buf, 0, sizeof(surf->vpu_out_buf));
        }
        if (surf->context_id == context) surf->context_id = 0;
        if (surf->output_context_id == context) surf->output_context_id = 0;
    }
}

static int hobot_reap_orphan_contexts(HobotDriverData *drv) {
    for (int i = 1; i < MAX_CONTEXTS; i++) {
        HobotContext *hctx = &drv->contexts[i];
        if (!hctx->allocated || !hctx->cleanup_orphaned) continue;
        if (hctx->vpu_running) {
            int ret = hb_mm_mc_stop(&hctx->vpu_ctx);
            if (ret != 0) {
                fprintf(stderr, "[HOBOT-VA] vaCreateContext: orphan ctx=%d stop retry failed: %d\n", i, ret);
                return -1;
            }
            hctx->vpu_running = 0;
        }
        if (hctx->vpu_initialized) {
            int ret = hb_mm_mc_release(&hctx->vpu_ctx);
            if (ret != 0) {
                fprintf(stderr, "[HOBOT-VA] vaCreateContext: orphan ctx=%d release retry failed: %d\n", i, ret);
                return -1;
            }
            hctx->vpu_initialized = 0;
        }
        memset(hctx, 0, sizeof(*hctx));
    }
    return 0;
}

static pthread_mutex_t hobot_watchdog_publish_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t hobot_watchdog_event_sequence;

static uint64_t hobot_watchdog_sequence_seed(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

static int hobot_watchdog_path_for_pid(char *path, size_t path_size, int pid) {
    if (!path || path_size == 0 || pid <= 0)
        return 0;
    int length = snprintf(path, path_size, "/dev/shm/hobot_va_watchdog.%d", pid);
    return length >= 0 && (size_t)length < path_size;
}

static void hobot_publish_watchdog_state(int anomaly_count, int err_mb, int total_mb) {
    char final_path[128];
    char tmp_path[160];
    int pid = (int)getpid();
    if (!hobot_watchdog_path_for_pid(final_path, sizeof(final_path), pid))
        return;

    int path_length = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.XXXXXX", final_path);
    if (path_length < 0 || (size_t)path_length >= sizeof(tmp_path) ||
        pthread_mutex_lock(&hobot_watchdog_publish_mutex) != 0)
        return;

    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        pthread_mutex_unlock(&hobot_watchdog_publish_mutex);
        return;
    }

    FILE *wfp = fdopen(fd, "w");
    if (!wfp) {
        close(fd);
        unlink(tmp_path);
        pthread_mutex_unlock(&hobot_watchdog_publish_mutex);
        return;
    }

    if (hobot_watchdog_event_sequence == 0)
        hobot_watchdog_event_sequence = hobot_watchdog_sequence_seed();
    uint64_t sequence = ++hobot_watchdog_event_sequence;
    int write_failed = fprintf(wfp, "%d %d %ld %d %d %llu\n", pid,
                               anomaly_count, (long)time(NULL), err_mb,
                               total_mb, (unsigned long long)sequence) < 0;
    if (fclose(wfp) != 0)
        write_failed = 1;

    if (write_failed || rename(tmp_path, final_path) != 0)
        unlink(tmp_path);
    pthread_mutex_unlock(&hobot_watchdog_publish_mutex);
}

static int hobot_header_changed(const uint8_t *old_data, int old_len,
                                const uint8_t *new_data, int new_len) {
    if (old_len != new_len) return 1;
    if (new_len <= 0) return 0;
    return memcmp(old_data, new_data, (size_t)new_len) != 0;
}

static VAStatus hobot_vaTerminate(VADriverContextP ctx) {
    if (!ctx) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (ctx->pDriverData) {
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    for (int i = 1; i < MAX_CONTEXTS; i++) {
        if (drv->contexts[i].sync_active || drv->contexts[i].decode_picture_active) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    for (int i = 1; i < MAX_BUFFERS; i++) {
        if (drv->buffers[i].allocated && drv->buffers[i].map_count > 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    for (int i = 1; i < MAX_SURFACES; i++) {
        if (drv->surfaces[i].allocated && drv->surfaces[i].lock_count > 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_SURFACE_BUSY;
        }
    }
    int term_failed_cleanup = 0;
        for (int i = 0; i < MAX_CONTEXTS; i++) {
            HobotContext *hctx = &drv->contexts[i];
            if (hctx->allocated) {
                if (hctx->vpu_running || hctx->vpu_initialized) {
                    if (hctx->vpu_running && hctx->is_encoder) {
                        if (hobot_retry_encoder_input(hctx) != 0) {
                            fprintf(stderr, "[HOBOT-VA] vaTerminate: retained encoder input recycle failed for ctx=%d\n", i);
                            term_failed_cleanup = 1;
                        }
                        if (hobot_retry_encoder_output(hctx) != 0) {
                            fprintf(stderr, "[HOBOT-VA] vaTerminate: retained encoder output recycle failed for ctx=%d\n", i);
                            term_failed_cleanup = 1;
                        }
                    }
                    if (hctx->vpu_running && !hctx->is_encoder) {
                        if (hobot_retry_decoder_output(hctx) != 0) {
                            fprintf(stderr, "[HOBOT-VA] vaTerminate: retained output buffer recycle failed for ctx=%d\n", i);
                            term_failed_cleanup = 1;
                        }
                        if (hctx->dec_in_buf_valid) {
                            int in_qret = hb_mm_mc_queue_input_buffer(&hctx->vpu_ctx, &hctx->dec_in_buf, 50);
                            if (in_qret != 0) {
                                in_qret = hb_mm_mc_queue_input_buffer(&hctx->vpu_ctx, &hctx->dec_in_buf, 200);
                            }
                            if (in_qret == 0) {
                                hctx->dec_in_buf_valid = 0;
                                hctx->dec_in_buf_offset = 0;
                            } else {
                                fprintf(stderr, "[HOBOT-VA] vaTerminate: queue_input_buffer permanently failed for ctx=%d\n", i);
                                term_failed_cleanup = 1;
                            }
                        }
                        for (int s = 1; s < MAX_SURFACES; s++) {
                            HobotSurface *surf = &drv->surfaces[s];
                            if (surf->allocated && surf->output_context_id == (VAContextID)i) {
                                if (surf->has_decoded_frame &&
                                    surf->vpu_out_buf.vframe_buf.phy_ptr[0] != 0 &&
                                    surf->vpu_out_buf.vframe_buf.size > 0) {
                                    int qret = hb_mm_mc_queue_output_buffer(&hctx->vpu_ctx, &surf->vpu_out_buf, 50);
                                    if (qret != 0) {
                                        qret = hb_mm_mc_queue_output_buffer(&hctx->vpu_ctx, &surf->vpu_out_buf, 200);
                                    }
                                    if (qret == 0) {
                                        surf->has_decoded_frame = 0;
                                        surf->dma_fd = -1;
                                        surf->output_context_id = 0;
                                        memset(&surf->vpu_out_buf, 0, sizeof(surf->vpu_out_buf));
                                    } else {
                                        fprintf(stderr, "[HOBOT-VA] vaTerminate: queue_output_buffer permanently failed for surf=%d, preserving buffer ownership\n", s);
                                        surf->decode_error = 1;
                                        term_failed_cleanup = 1;
                                    }
                                }
                            }
                        }
                    }
                    if (!term_failed_cleanup) {
                        if (hctx->vpu_running) {
                            int sret = hb_mm_mc_stop(&hctx->vpu_ctx);
                            if (sret == 0) {
                                hctx->vpu_running = 0;
                            } else {
                                fprintf(stderr, "[HOBOT-VA] vaTerminate: hb_mm_mc_stop failed: %d for ctx=%d\n", sret, i);
                                term_failed_cleanup = 1;
                            }
                        }
                        if (hctx->vpu_initialized && !term_failed_cleanup) {
                            int rret = hb_mm_mc_release(&hctx->vpu_ctx);
                            if (rret == 0) {
                                hctx->vpu_initialized = 0;
                                hctx->allocated = 0;
                                hobot_detach_context_surfaces(drv, (VAContextID)i);
                            } else {
                                fprintf(stderr, "[HOBOT-VA] vaTerminate: hb_mm_mc_release failed: %d for ctx=%d\n", rret, i);
                                term_failed_cleanup = 1;
                            }
                        }
                        if (!term_failed_cleanup && !hctx->vpu_running &&
                            !hctx->vpu_initialized) {
                            hctx->allocated = 0;
                            hobot_detach_context_surfaces(drv, (VAContextID)i);
                        }
                    }
                } else {
                    hctx->allocated = 0;
                    hobot_detach_context_surfaces(drv, (VAContextID)i);
                }
            }
        }
        if (term_failed_cleanup) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        for (int i = 0; i < MAX_SURFACES; i++) {
            HobotSurface *surf = &drv->surfaces[i];
            if (surf->allocated) {
                if (surf->has_preallocated) {
                    int32_t free_ret = surf->preallocated_gbuf.fd[0] >= 0
                        ? hb_mem_free_buf(surf->preallocated_gbuf.fd[0]) : -1;
                    if (free_ret != 0) {
                        fprintf(stderr, "[HOBOT-VA] vaTerminate: preallocated buffer free failed for surface=%d fd=%d ret=%d\n",
                                i, surf->preallocated_gbuf.fd[0], free_ret);
                        term_failed_cleanup = 1;
                        continue;
                    }
                    surf->has_preallocated = 0;
                    memset(&surf->preallocated_gbuf, 0, sizeof(surf->preallocated_gbuf));
                }
                if (surf->raw_data) {
                    free(surf->raw_data);
                    surf->raw_data = NULL;
                }
                surf->decode_pending = 0;
                surf->decode_error = 0;
                surf->allocated = 0;
            }
        }
        if (term_failed_cleanup) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        for (int i = 0; i < MAX_BUFFERS; i++) {
            if (drv->buffers[i].allocated && !drv->buffers[i].is_derived && drv->buffers[i].data) {
                free(drv->buffers[i].data);
                drv->buffers[i].data = NULL;
            }
        }
        pthread_mutex_unlock(&drv->mutex);
        if (drv->sync_cond_initialized)
            pthread_cond_destroy(&drv->sync_cond);
        pthread_mutex_destroy(&drv->mutex);
        free(drv);
        ctx->pDriverData = NULL;
        int32_t mem_ret = hb_mem_module_close();
        if (mem_ret != 0) {
            fprintf(stderr, "[HOBOT-VA] vaTerminate: hb_mem_module_close failed: %d\n",
                    mem_ret);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaQueryConfigProfiles(
    VADriverContextP ctx,
    VAProfile *profile_list,
    int *num_profiles
) {
    va_trace("vaQueryConfigProfiles: profile_list=%p, num_profiles=%p", profile_list, num_profiles);
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
    va_trace("vaQueryConfigEntrypoints: profile=%d, list=%p, num=%p", profile, entrypoint_list, num_entrypoints);
    if (!num_entrypoints) return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (!hobot_profile_supported(profile)) {
        *num_entrypoints = 0;
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    }

    int entrypoint_count = 2;
    if (!entrypoint_list) {
        *num_entrypoints = entrypoint_count;
        return VA_STATUS_SUCCESS;
    }

    if (profile == VAProfileJPEGBaseline) {
        entrypoint_list[0] = VAEntrypointVLD;
        entrypoint_list[1] = VAEntrypointEncPicture;
    } else {
        entrypoint_list[0] = VAEntrypointVLD;
        if (entrypoint_count == 2)
            entrypoint_list[1] = VAEntrypointEncSlice;
    }
    *num_entrypoints = entrypoint_count;
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaGetConfigAttributes(
    VADriverContextP ctx,
    VAProfile profile,
    VAEntrypoint entrypoint,
    VAConfigAttrib *attrib_list,
    int num_attribs
) {
    va_trace("vaGetConfigAttributes: profile=%d, entrypoint=%d, num_attribs=%d", profile, entrypoint, num_attribs);
    if (num_attribs < 0 || (num_attribs > 0 && !attrib_list))
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    int is_h264 = profile == VAProfileH264ConstrainedBaseline ||
                  profile == VAProfileH264Main || profile == VAProfileH264High;
    int is_hevc = profile == VAProfileHEVCMain;
    int is_jpeg = profile == VAProfileJPEGBaseline;
    int is_encoder = (is_h264 && entrypoint == VAEntrypointEncSlice) ||
                     (is_hevc && entrypoint == VAEntrypointEncSlice) ||
                     (is_jpeg && entrypoint == VAEntrypointEncPicture);
    int supported_pair = hobot_profile_supported(profile) &&
                         ((entrypoint == VAEntrypointVLD && (!is_jpeg || profile == VAProfileJPEGBaseline)) ||
                          is_encoder);
    if (!supported_pair) {
        for (int i = 0; i < num_attribs; i++)
            attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
        return VA_STATUS_SUCCESS;
    }

    for (int i = 0; i < num_attribs; i++) {
        switch (attrib_list[i].type) {
        case VAConfigAttribRTFormat:
            attrib_list[i].value = VA_RT_FORMAT_YUV420;
            break;
        case VAConfigAttribDecJPEG:
            attrib_list[i].value = is_jpeg && entrypoint == VAEntrypointVLD ?
                                   (1u << VA_ROTATION_NONE) :
                                   VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribRateControl:
            attrib_list[i].value = (is_h264 || is_hevc) &&
                                   entrypoint == VAEntrypointEncSlice ?
                                   VA_RC_CBR :
                                   (is_jpeg && entrypoint == VAEntrypointEncPicture ?
                                    VA_RC_CQP : VA_ATTRIB_NOT_SUPPORTED);
            break;
        case VAConfigAttribEncPackedHeaders:
            attrib_list[i].value = is_encoder ? VA_ENC_PACKED_HEADER_NONE :
                                   VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncMaxRefFrames:
            attrib_list[i].value = (is_h264 || is_hevc) &&
                                   entrypoint == VAEntrypointEncSlice ?
                                   1 : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncMaxSlices:
            attrib_list[i].value = (is_h264 || is_hevc) &&
                                   entrypoint == VAEntrypointEncSlice ?
                                   1 : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribMaxPictureWidth:
            attrib_list[i].value = profile == VAProfileHEVCMain ?
                                   HOBOT_HEVC_MAX_WIDTH : 4096;
            break;
        case VAConfigAttribMaxPictureHeight:
            attrib_list[i].value = profile == VAProfileHEVCMain ?
                                   HOBOT_HEVC_MAX_HEIGHT : 4096;
            break;
        case VAConfigAttribEncSliceStructure:
            attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncQualityRange:
            attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncInterlaced:
            attrib_list[i].value = is_encoder ? VA_ENC_INTERLACED_NONE :
                                   VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncQuantization:
            attrib_list[i].value = is_encoder ? VA_ENC_QUANTIZATION_NONE :
                                   VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncIntraRefresh:
            attrib_list[i].value = is_encoder ? VA_ENC_INTRA_REFRESH_NONE :
                                   VA_ATTRIB_NOT_SUPPORTED;
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
    va_trace("vaCreateConfig: profile=%d, entrypoint=%d, num_attribs=%d", profile, entrypoint, num_attribs);
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!config_id || num_attribs < 0 || num_attribs > MAX_CONFIG_ATTRIBUTES ||
        (num_attribs > 0 && !attrib_list))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;

    int is_vld = (entrypoint == VAEntrypointVLD);
    int is_enc = (entrypoint == VAEntrypointEncSlice || entrypoint == VAEntrypointEncPicture);
    if (!hobot_profile_supported(profile))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
    if (!is_vld && !is_enc) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }
    if (profile == VAProfileJPEGBaseline &&
        entrypoint != VAEntrypointEncPicture && entrypoint != VAEntrypointVLD) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }
    if (profile == VAProfileHEVCMain && entrypoint != VAEntrypointVLD &&
        entrypoint != VAEntrypointEncSlice) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }
    if (profile != VAProfileJPEGBaseline && entrypoint == VAEntrypointEncPicture) {
        return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
    }

    unsigned int expected_rate_control = is_enc ?
        (profile == VAProfileJPEGBaseline ? VA_RC_CQP : VA_RC_CBR) :
        VA_ATTRIB_NOT_SUPPORTED;
    for (int i = 0; i < num_attribs; i++) {
        switch (attrib_list[i].type) {
        case VAConfigAttribRTFormat:
            if (attrib_list[i].value != VA_RT_FORMAT_YUV420)
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
            break;
        case VAConfigAttribRateControl:
            if (!is_enc || attrib_list[i].value != expected_rate_control)
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            break;
        case VAConfigAttribDecJPEG:
            if (profile != VAProfileJPEGBaseline || entrypoint != VAEntrypointVLD ||
                attrib_list[i].value != (1u << VA_ROTATION_NONE))
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncPackedHeaders:
            if (!is_enc || attrib_list[i].value != VA_ENC_PACKED_HEADER_NONE)
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncInterlaced:
            if (!is_enc || attrib_list[i].value != VA_ENC_INTERLACED_NONE)
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncQuantization:
            if (!is_enc || attrib_list[i].value != VA_ENC_QUANTIZATION_NONE)
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncIntraRefresh:
            if (!is_enc || attrib_list[i].value != VA_ENC_INTRA_REFRESH_NONE)
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            break;
        default:
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        }
    }

    pthread_mutex_lock(&drv->mutex);
    for (int i = 1; i < MAX_CONFIGS; i++) {
        if (!drv->configs[i].allocated) {
            drv->configs[i].allocated = 1;
            drv->configs[i].id = (VAConfigID)i;
            drv->configs[i].profile = profile;
            drv->configs[i].entrypoint = entrypoint;
            drv->configs[i].rate_control = expected_rate_control;
            drv->configs[i].rt_format = VA_RT_FORMAT_YUV420;
            drv->configs[i].num_attribs = num_attribs;

            if (attrib_list) {
                memcpy(drv->configs[i].attribs, attrib_list,
                       (size_t)num_attribs * sizeof(attrib_list[0]));
                for (int a = 0; a < num_attribs; a++) {
                    if (attrib_list[a].type == VAConfigAttribRateControl) {
                        drv->configs[i].rate_control = attrib_list[a].value;
                    } else if (attrib_list[a].type == VAConfigAttribRTFormat) {
                        drv->configs[i].rt_format = attrib_list[a].value;
                    }
                }
            }
            *config_id = (VAConfigID)i;
            va_trace("vaCreateConfig -> id=%u", (unsigned int)i);
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_SUCCESS;
        }
    }
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

static VAStatus hobot_vaDestroyConfig(
    VADriverContextP ctx,
    VAConfigID config_id
) {
    va_trace("vaDestroyConfig: id=%u", config_id);
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    if (config_id <= 0 || config_id >= MAX_CONFIGS || !drv->configs[config_id].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    drv->configs[config_id].allocated = 0;
    pthread_mutex_unlock(&drv->mutex);
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
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!num_attribs) return VA_STATUS_ERROR_INVALID_PARAMETER;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;

    pthread_mutex_lock(&drv->mutex);
    if (config_id <= 0 || config_id >= MAX_CONFIGS || !drv->configs[config_id].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    HobotConfig config = drv->configs[config_id];
    pthread_mutex_unlock(&drv->mutex);

    if (config.num_attribs > 0 && !attrib_list)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (profile) *profile = config.profile;
    if (entrypoint) *entrypoint = config.entrypoint;
    if (config.num_attribs > 0)
        memcpy(attrib_list, config.attribs,
               (size_t)config.num_attribs * sizeof(config.attribs[0]));
    *num_attribs = config.num_attribs;
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaQuerySurfaceAttributes(
    VADriverContextP ctx,
    VAConfigID config_id,
    VASurfaceAttrib *attrib_list,
    unsigned int *num_attribs
) {
    va_trace("vaQuerySurfaceAttributes: config_id=%u, attrib_list=%p, num_attribs=%p (%u)",
             config_id, attrib_list, num_attribs, (num_attribs ? *num_attribs : 0));
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!num_attribs) return VA_STATUS_ERROR_INVALID_PARAMETER;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    int valid_config = config_id > 0 && config_id < MAX_CONFIGS &&
                       drv->configs[config_id].allocated;
    VAProfile config_profile = valid_config ? drv->configs[config_id].profile :
                                              VAProfileNone;
    pthread_mutex_unlock(&drv->mutex);
    if (!valid_config) return VA_STATUS_ERROR_INVALID_CONFIG;
    if (!attrib_list) {
        *num_attribs = 6;
        return VA_STATUS_SUCCESS;
    }
    if (*num_attribs < 6) {
        *num_attribs = 6;
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
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
    attrib_list[idx].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
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
    attrib_list[idx].value.value.i = config_profile == VAProfileHEVCMain ?
                                     HOBOT_HEVC_MAX_WIDTH : 4096;
    idx++;

    attrib_list[idx].type = VASurfaceAttribMaxHeight;
    attrib_list[idx].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[idx].value.type = VAGenericValueTypeInteger;
    attrib_list[idx].value.value.i = config_profile == VAProfileHEVCMain ?
                                     HOBOT_HEVC_MAX_HEIGHT : 4096;
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
    va_trace("vaCreateSurfaces2: num=%u, %ux%u, format=0x%x, num_attribs=%u",
             num_surfaces, width, height, format, num_attribs);
    if (!ctx || !ctx->pDriverData) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (!surfaces || num_surfaces == 0 || num_surfaces >= MAX_SURFACES ||
        width < 64 || height < 64 || width > 4096 || height > 4096 ||
        (width & 1u) != 0 || (height & 1u) != 0 ||
        (num_attribs > 0 && !attrib_list)) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (format != VA_RT_FORMAT_YUV420) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    for (unsigned int i = 0; i < num_attribs; i++) {
        VASurfaceAttrib *attr = &attrib_list[i];
        if (attr->type == VASurfaceAttribPixelFormat) {
            /* Some libva clients leave the generic type zeroed for FOURCC attrs. */
            if ((attr->value.type != VAGenericValueTypeInteger &&
                 attr->value.type != 0) ||
                attr->value.value.i != VA_FOURCC_NV12) {
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
            }
        } else if (attr->type == VASurfaceAttribMemoryType) {
            if (attr->value.type != VAGenericValueTypeInteger ||
                attr->value.value.i != VA_SURFACE_ATTRIB_MEM_TYPE_VA) {
                return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
            }
        } else {
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
    }
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    for (unsigned int i = 0; i < num_surfaces; i++) {
        int found = 0;
        for (int s = 1; s < MAX_SURFACES; s++) {
            if (!drv->surfaces[s].allocated) {
                unsigned int aligned_w = (width + 63u) & ~63u;
                unsigned int aligned_h = (height + 63u) & ~63u;
                drv->surfaces[s].allocated = 1;
                drv->surfaces[s].id = (VASurfaceID)s;
                drv->surfaces[s].width = width;
                drv->surfaces[s].height = height;
                drv->surfaces[s].format = format;
                drv->surfaces[s].stride = (int)aligned_w;
                drv->surfaces[s].raw_data_size = (uint32_t)((uint64_t)aligned_w * aligned_h * 3 / 2);
                drv->surfaces[s].raw_data = NULL;
                drv->surfaces[s].raw_data_valid = 0;
                drv->surfaces[s].raw_data_dirty = 0;
                drv->surfaces[s].has_decoded_frame = 0;
                drv->surfaces[s].context_id = 0;
                drv->surfaces[s].output_context_id = 0;
                drv->surfaces[s].decode_pending = 0;
                drv->surfaces[s].decode_error = 0;
                drv->surfaces[s].dma_fd = -1;
                memset(&drv->surfaces[s].vpu_out_buf, 0, sizeof(drv->surfaces[s].vpu_out_buf));
                drv->surfaces[s].has_preallocated = 0;
                memset(&drv->surfaces[s].preallocated_gbuf, 0, sizeof(drv->surfaces[s].preallocated_gbuf));

                surfaces[i] = (VASurfaceID)s;
                found = 1;
                break;
            }
        }
        if (!found) {
            for (unsigned int j = 0; j < i; j++) {
                VASurfaceID created = surfaces[j];
                if (created <= 0 || created >= MAX_SURFACES) continue;

                HobotSurface *surf = &drv->surfaces[created];
                if (surf->has_preallocated && surf->preallocated_gbuf.fd[0] >= 0) {
                    hb_mem_free_buf(surf->preallocated_gbuf.fd[0]);
                }
                free(surf->raw_data);
                memset(surf, 0, sizeof(*surf));
                surf->dma_fd = -1;
            }
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }
    va_trace("vaCreateSurfaces2 -> created %u surfaces (first=%u)", num_surfaces, surfaces[0]);
    pthread_mutex_unlock(&drv->mutex);
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
    va_trace("vaCreateSurfaces: num=%d, %dx%d, format=0x%x", num_surfaces, width, height, format);
    return hobot_vaCreateSurfaces2(ctx, format, width, height, surfaces, num_surfaces, NULL, 0);
}

static VAStatus hobot_vaDestroySurfaces(
    VADriverContextP ctx,
    VASurfaceID *surfaces,
    int num_surfaces
) {
    va_trace("vaDestroySurfaces: num=%d", num_surfaces);
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!surfaces || num_surfaces <= 0 || num_surfaces >= MAX_SURFACES)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    for (int i = 0; i < num_surfaces; i++) {
        VASurfaceID surface = surfaces[i];
        if (surface <= 0 || surface >= MAX_SURFACES || !drv->surfaces[surface].allocated) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        for (int j = 0; j < i; j++) {
            if (surfaces[j] == surface) {
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
        }
        for (int j = 1; j < MAX_IMAGES; j++) {
            if (drv->images[j].allocated && drv->images[j].surface_id == surface) {
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_SURFACE_BUSY;
            }
        }
        HobotSurface *surf = &drv->surfaces[surface];
        if (surf->lock_count > 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_SURFACE_BUSY;
        }
        if (hobot_surface_has_active_encoder(drv, surface)) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_SURFACE_BUSY;
        }
        VAContextID owners[] = { surf->context_id, surf->output_context_id };
        for (size_t j = 0; j < sizeof(owners) / sizeof(owners[0]); j++) {
            VAContextID owner = owners[j];
            if (owner > 0 && owner < MAX_CONTEXTS &&
                (drv->contexts[owner].sync_active ||
                 drv->contexts[owner].decode_picture_active)) {
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_SURFACE_BUSY;
            }
        }
    }
    VAStatus overall_status = VA_STATUS_SUCCESS;
    for (int i = 0; i < num_surfaces; i++) {
        VASurfaceID s = surfaces[i];
        {
            HobotSurface *surf = &drv->surfaces[s];
            if (surf->decode_pending) {
                fprintf(stderr, "[HOBOT-VA] vaDestroySurfaces: refusing to free pending decode surface=%u\n", s);
                overall_status = VA_STATUS_ERROR_OPERATION_FAILED;
                continue;
            }
            if (surf->has_decoded_frame) {
                VAContextID cid = surf->output_context_id;
                int qret = 0;
                if (surf->vpu_out_buf.vframe_buf.phy_ptr[0] != 0 && surf->vpu_out_buf.vframe_buf.size > 0) {
                    if (cid > 0 && cid < MAX_CONTEXTS && drv->contexts[cid].allocated && drv->contexts[cid].vpu_running && !drv->contexts[cid].is_encoder) {
                        qret = hb_mm_mc_queue_output_buffer(&drv->contexts[cid].vpu_ctx, &surf->vpu_out_buf, 50);
                        if (qret != 0) {
                            qret = hb_mm_mc_queue_output_buffer(&drv->contexts[cid].vpu_ctx, &surf->vpu_out_buf, 200);
                        }
                    } else {
                        /* Owner context is already stopped/released; instance buffers freed by VPU driver */
                        qret = 0;
                    }
                }
                if (qret == 0) {
                    surf->has_decoded_frame = 0;
                    surf->dma_fd = -1;
                    surf->output_context_id = 0;
                    memset(&surf->vpu_out_buf, 0, sizeof(surf->vpu_out_buf));
                } else {
                    fprintf(stderr, "[HOBOT-VA] vaDestroySurfaces: queue_output_buffer failed for surf=%d, preserving ownership\n", s);
                    surf->decode_error = 1;
                    overall_status = VA_STATUS_ERROR_OPERATION_FAILED;
                    continue; /* Do NOT clear surface slot if buffer recycle failed! */
                }
            }
            if (surf->has_preallocated) {
                int32_t free_ret = surf->preallocated_gbuf.fd[0] >= 0
                    ? hb_mem_free_buf(surf->preallocated_gbuf.fd[0]) : -1;
                if (free_ret != 0) {
                    fprintf(stderr, "[HOBOT-VA] vaDestroySurfaces: preallocated buffer free failed for surf=%d fd=%d ret=%d\n",
                            s, surf->preallocated_gbuf.fd[0], free_ret);
                    overall_status = VA_STATUS_ERROR_OPERATION_FAILED;
                    continue;
                }
                surf->has_preallocated = 0;
                memset(&surf->preallocated_gbuf, 0, sizeof(surf->preallocated_gbuf));
            }
            if (surf->raw_data) {
                free(surf->raw_data);
                surf->raw_data = NULL;
            }
            surf->decode_pending = 0;
            surf->decode_error = 0;
            surf->allocated = 0;
            surf->dma_fd = -1;
            surf->context_id = 0;
            surf->output_context_id = 0;
        }
    }
    pthread_mutex_unlock(&drv->mutex);
    return overall_status;
}

static VAStatus hobot_vaQuerySurfaceStatus(
    VADriverContextP ctx,
    VASurfaceID render_target,
    VASurfaceStatus *status
) {
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!status) return VA_STATUS_ERROR_INVALID_PARAMETER;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    if (render_target <= 0 || render_target >= MAX_SURFACES || !drv->surfaces[render_target].allocated) {
        *status = VASurfaceReady;
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    HobotSurface *surf = &drv->surfaces[render_target];
    *status = surf->decode_pending ? VASurfaceRendering : VASurfaceReady;
    va_trace("vaQuerySurfaceStatus: surf=%u -> %d", render_target, *status);
    pthread_mutex_unlock(&drv->mutex);
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
    va_trace("vaCreateContext: config_id=%u, %dx%d, flag=0x%x, num_targets=%d",
             config_id, picture_width, picture_height, flag, num_render_targets);
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!context || picture_width <= 0 || picture_height <= 0 ||
        picture_width > 4096 || picture_height > 4096 || num_render_targets < 0 ||
        num_render_targets >= MAX_SURFACES ||
        (num_render_targets > 0 && !render_targets)) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    if (config_id <= 0 || config_id >= MAX_CONFIGS || !drv->configs[config_id].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_CONFIG;
    }
    for (int i = 0; i < num_render_targets; i++) {
        VASurfaceID target = render_targets[i];
        if (target <= 0 || target >= MAX_SURFACES || !drv->surfaces[target].allocated) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
    }
    if (hobot_reap_orphan_contexts(drv) != 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    HobotConfig *cfg = &drv->configs[config_id];
    if (!hobot_profile_resolution_supported(cfg->profile,
                                            picture_width, picture_height)) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

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
            c->watchdog_anomaly_count = 0;
            c->last_anomaly_sec = 0;
            c->watchdog_trace_countdown = 0;
            int is_enc = (cfg->entrypoint == VAEntrypointEncSlice || cfg->entrypoint == VAEntrypointEncPicture);
            c->is_encoder = is_enc;

            media_codec_context_t *mctx = &c->vpu_ctx;
            memset(mctx, 0, sizeof(*mctx));

            media_codec_id_t cid = MEDIA_CODEC_ID_H264;
            if (cfg->profile == VAProfileHEVCMain) {
                cid = MEDIA_CODEC_ID_H265;
            } else if (cfg->profile == VAProfileJPEGBaseline) {
                cid = MEDIA_CODEC_ID_JPEG;
            }

            int ret = 0;
            if (is_enc) {
                ret = hb_mm_mc_get_default_context(cid, 1, mctx);
                if (ret != 0) {
                    va_trace("vaCreateContext: get_default_context failed (%d), using manual context", ret);
                    mctx->codec_id = cid;
                    mctx->encoder = 1;
                    ret = 0;
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
                    mctx->video_enc_params.h265_enc_config.h265_level =
                        MC_H265_LEVEL_UNSPECIFIED;
                    mctx->video_enc_params.h265_enc_config.main_still_picture_profile_enable = 0;
                    mctx->video_enc_params.h265_enc_config.wpp_enable = 0;
                } else if (cid == MEDIA_CODEC_ID_JPEG) {
                    mctx->video_enc_params.rc_params.mode = MC_AV_RC_MODE_MJPEGFIXQP;
                    mctx->video_enc_params.rc_params.mjpeg_fixqp_params.frame_rate = 30;
                    mctx->video_enc_params.jpeg_enc_config.quality_factor = 85;
                }

                if (cid == MEDIA_CODEC_ID_H264 || cid == MEDIA_CODEC_ID_H265) {
                    c->encoder_init_deferred = 1;
                } else {
                    ret = hb_mm_mc_initialize(mctx);
                    if (ret == 0) {
                        c->vpu_initialized = 1;
                        ret = hb_mm_mc_configure(mctx);
                        if (ret == 0) {
                            ret = hb_mm_mc_start(mctx, NULL);
                            if (ret == 0) {
                                c->vpu_running = 1;
                            }
                        }
                    }
                }
            } else {
                /* Decoder */
                if (cid == MEDIA_CODEC_ID_JPEG)
                    ret = hb_mm_mc_get_default_context(cid, 0, mctx);
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

                if (ret == 0)
                    ret = hb_mm_mc_initialize(mctx);
                if (ret == 0) {
                    c->vpu_initialized = 1;
                    ret = hb_mm_mc_configure(mctx);
                    if (ret == 0) {
                        ret = hb_mm_mc_start(mctx, NULL);
                        if (ret == 0) {
                            c->vpu_running = 1;
                        }
                    }
                }
            }

            if (ret != 0) {
                fprintf(stderr, "[HOBOT-VA] vaCreateContext: codec setup failed: %d\n", ret);
                int cleanup_failed = 0;
                if (c->vpu_running) {
                    int sret = hb_mm_mc_stop(mctx);
                    if (sret == 0) {
                        c->vpu_running = 0;
                    } else {
                        fprintf(stderr, "[HOBOT-VA] vaCreateContext: hb_mm_mc_stop failed: %d\n", sret);
                        cleanup_failed = 1;
                    }
                }
                if (c->vpu_initialized && !cleanup_failed) {
                    int rret = hb_mm_mc_release(mctx);
                    if (rret == 0) {
                        c->vpu_initialized = 0;
                    } else {
                        fprintf(stderr, "[HOBOT-VA] vaCreateContext: hb_mm_mc_release failed: %d\n", rret);
                        cleanup_failed = 1;
                    }
                }
                if (cleanup_failed) {
                    /* Release/stop failed: preserve context allocation and state for later retry/destruction */
                    c->cleanup_orphaned = 1;
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_OPERATION_FAILED;
                }
                memset(c, 0, sizeof(*c));
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }

            *context = (VAContextID)i;
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_SUCCESS;
        }
    }
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

static VAStatus hobot_vaDestroyContext(VADriverContextP ctx, VAContextID context) {
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (context <= 0 || context >= MAX_CONTEXTS) return VA_STATUS_ERROR_INVALID_CONTEXT;
    pthread_mutex_lock(&drv->mutex);
    if (!drv->contexts[context].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (drv->contexts[context].sync_active ||
        drv->contexts[context].decode_picture_active) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (hobot_context_has_locked_surfaces(drv, context)) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    {
        HobotContext *hctx = &drv->contexts[context];
        int failed_recycle = 0;
        if (hctx->vpu_running || hctx->vpu_initialized) {
            if (hctx->vpu_running && hctx->is_encoder) {
                if (hobot_retry_encoder_input(hctx) != 0) {
                    fprintf(stderr, "[HOBOT-VA] vaDestroyContext: retained encoder input recycle failed for ctx=%d\n", context);
                    failed_recycle = 1;
                }
                if (hobot_retry_encoder_output(hctx) != 0) {
                    fprintf(stderr, "[HOBOT-VA] vaDestroyContext: retained encoder output recycle failed for ctx=%d\n", context);
                    failed_recycle = 1;
                }
            }
            if (hctx->vpu_running && !hctx->is_encoder) {
                if (hobot_retry_decoder_output(hctx) != 0) {
                    fprintf(stderr, "[HOBOT-VA] vaDestroyContext: retained output buffer recycle failed for ctx=%d\n", context);
                    failed_recycle = 1;
                }
                if (hctx->dec_in_buf_valid) {
                    int in_qret = hb_mm_mc_queue_input_buffer(&hctx->vpu_ctx, &hctx->dec_in_buf, 50);
                    if (in_qret != 0) {
                        in_qret = hb_mm_mc_queue_input_buffer(&hctx->vpu_ctx, &hctx->dec_in_buf, 200);
                    }
                    if (in_qret == 0) {
                        hctx->dec_in_buf_valid = 0;
                        hctx->dec_in_buf_offset = 0;
                    } else {
                        fprintf(stderr, "[HOBOT-VA] vaDestroyContext: queue_input_buffer failed: %d\n", in_qret);
                        failed_recycle = 1;
                    }
                }
                for (int s = 1; s < MAX_SURFACES; s++) {
                    if (drv->surfaces[s].allocated && drv->surfaces[s].output_context_id == context) {
                        if (drv->surfaces[s].has_decoded_frame) {
                            int qret = 0;
                            if (drv->surfaces[s].vpu_out_buf.vframe_buf.phy_ptr[0] != 0 &&
                                drv->surfaces[s].vpu_out_buf.vframe_buf.size > 0) {
                                qret = hb_mm_mc_queue_output_buffer(&hctx->vpu_ctx, &drv->surfaces[s].vpu_out_buf, 50);
                                if (qret != 0) {
                                    fprintf(stderr, "[HOBOT-VA] vaDestroyContext: queue_output_buffer ret=%d for surf=%d, retrying...\n", qret, s);
                                    qret = hb_mm_mc_queue_output_buffer(&hctx->vpu_ctx, &drv->surfaces[s].vpu_out_buf, 200);
                                }
                            }
                            if (qret == 0) {
                                drv->surfaces[s].has_decoded_frame = 0;
                                drv->surfaces[s].dma_fd = -1;
                                drv->surfaces[s].output_context_id = 0;
                                memset(&drv->surfaces[s].vpu_out_buf, 0, sizeof(drv->surfaces[s].vpu_out_buf));
                            } else {
                                fprintf(stderr, "[HOBOT-VA] vaDestroyContext: queue_output_buffer permanently failed for surf=%d, preserving buffer ownership\n", s);
                                drv->surfaces[s].decode_error = 1;
                                failed_recycle = 1;
                            }
                        }
                    }
                }
            }
            if (failed_recycle) {
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            if (hctx->vpu_running) {
                int sret = hb_mm_mc_stop(&hctx->vpu_ctx);
                if (sret == 0) {
                    hctx->vpu_running = 0;
                } else {
                    fprintf(stderr, "[HOBOT-VA] vaDestroyContext: hb_mm_mc_stop failed: %d for ctx=%d\n", sret, context);
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_OPERATION_FAILED;
                }
            }
            if (hctx->vpu_initialized) {
                int rret = hb_mm_mc_release(&hctx->vpu_ctx);
                if (rret == 0) {
                    hctx->vpu_initialized = 0;
                } else {
                    fprintf(stderr, "[HOBOT-VA] vaDestroyContext: hb_mm_mc_release failed: %d for ctx=%d\n", rret, context);
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_OPERATION_FAILED;
                }
            }
        }
        hobot_detach_context_surfaces(drv, context);
        hctx->allocated = 0;
        hctx->cleanup_orphaned = 0;
        pthread_mutex_unlock(&drv->mutex);
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaCreateBufferLocked(
    HobotDriverData *drv,
    VABufferType type,
    unsigned int size,
    unsigned int num_elements,
    void *data,
    VABufferID *buf_id
) {
    if (!buf_id) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (size == 0 || num_elements == 0 || size > UINT_MAX / num_elements)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    for (int i = 1; i < MAX_BUFFERS; i++) {
        if (!drv->buffers[i].allocated) {
            drv->buffers[i].allocated = 1;
            drv->buffers[i].is_derived = 0;
            drv->buffers[i].data = NULL;
            drv->buffers[i].id = (VABufferID)i;
            drv->buffers[i].type = type;
            drv->buffers[i].size = size * num_elements;
            drv->buffers[i].capacity = drv->buffers[i].size;
            drv->buffers[i].element_size = size;
            drv->buffers[i].num_elements = num_elements;
            drv->buffers[i].map_count = 0;
            memset(&drv->buffers[i].coded_segment, 0, sizeof(drv->buffers[i].coded_segment));

            if (type == VAEncCodedBufferType) {
                drv->buffers[i].data = malloc(drv->buffers[i].capacity);
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
                drv->buffers[i].data = malloc(drv->buffers[i].capacity);
                if (!drv->buffers[i].data) {
                    drv->buffers[i].allocated = 0;
                    return VA_STATUS_ERROR_ALLOCATION_FAILED;
                }
                if (data && drv->buffers[i].capacity > 0) {
                    memcpy(drv->buffers[i].data, data, drv->buffers[i].capacity);
                }
            }
            *buf_id = (VABufferID)i;
            return VA_STATUS_SUCCESS;
        }
    }
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
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
    (void)context;
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    VAStatus status = hobot_vaCreateBufferLocked(drv, type, size, num_elements,
                                                  data, buf_id);
    pthread_mutex_unlock(&drv->mutex);
    va_trace("vaCreateBuffer: context=%u type=%u element_size=%u elements=%u status=%d id=%u",
             context, type, size, num_elements, status,
             status == VA_STATUS_SUCCESS ? *buf_id : VA_INVALID_ID);
    return status;
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
    if (width == 0 || height == 0 || width > UINT_MAX / height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (unit_size) *unit_size = 1;
    if (pitch) *pitch = width;
    return hobot_vaCreateBuffer(ctx, context, type, width * height, 1, NULL, buf_id);
}

static VAStatus hobot_vaBufferSetNumElements(
    VADriverContextP ctx,
    VABufferID buf_id,
    unsigned int num_elements
) {
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    if (buf_id <= 0 || buf_id >= MAX_BUFFERS || !drv->buffers[buf_id].allocated)
        goto invalid_buffer;

    HobotBuffer *buffer = &drv->buffers[buf_id];
    if (buffer->type == VAEncCodedBufferType)
        goto unimplemented;
    if (buffer->element_size == 0) {
        if (num_elements != 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        buffer->size = 0;
        buffer->num_elements = 0;
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_SUCCESS;
    }
    if (num_elements > buffer->capacity / buffer->element_size)
        goto too_many_elements;

    buffer->num_elements = num_elements;
    buffer->size = buffer->element_size * num_elements;
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_SUCCESS;

invalid_buffer:
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_ERROR_INVALID_BUFFER;
unimplemented:
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_ERROR_UNIMPLEMENTED;
too_many_elements:
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

static VAStatus hobot_vaMapBuffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf) {
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!pbuf) return VA_STATUS_ERROR_INVALID_PARAMETER;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    if (buf_id > 0 && buf_id < MAX_BUFFERS && drv->buffers[buf_id].allocated) {
        if (drv->buffers[buf_id].map_count == UINT32_MAX) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
        }
        if (drv->buffers[buf_id].type == VAEncCodedBufferType) {
            *pbuf = &drv->buffers[buf_id].coded_segment;
        } else {
            *pbuf = drv->buffers[buf_id].data;
        }
        drv->buffers[buf_id].map_count++;
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_SUCCESS;
    }
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus hobot_vaUnmapBuffer(VADriverContextP ctx, VABufferID buf_id) {
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    if (buf_id <= 0 || buf_id >= MAX_BUFFERS || !drv->buffers[buf_id].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (drv->buffers[buf_id].map_count == 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    drv->buffers[buf_id].map_count--;
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaBufferInfo(
    VADriverContextP ctx,
    VABufferID buf_id,
    VABufferType *type,
    unsigned int *size,
    unsigned int *num_elements
) {
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    if (buf_id <= 0 || buf_id >= MAX_BUFFERS || !drv->buffers[buf_id].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    HobotBuffer *buffer = &drv->buffers[buf_id];
    if (type) *type = buffer->type;
    if (size) *size = buffer->size;
    if (num_elements) *num_elements = buffer->num_elements;
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaDestroyBufferLocked(HobotDriverData *drv, VABufferID buffer_id) {
    if (buffer_id > 0 && buffer_id < MAX_BUFFERS && drv->buffers[buffer_id].allocated) {
        if (drv->buffers[buffer_id].map_count > 0)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        if (!drv->buffers[buffer_id].is_derived && drv->buffers[buffer_id].data) {
            free(drv->buffers[buffer_id].data);
        }
        drv->buffers[buffer_id].data = NULL;
        drv->buffers[buffer_id].allocated = 0;
        drv->buffers[buffer_id].is_derived = 0;
        drv->buffers[buffer_id].size = 0;
        drv->buffers[buffer_id].capacity = 0;
        drv->buffers[buffer_id].element_size = 0;
        drv->buffers[buffer_id].num_elements = 0;
        drv->buffers[buffer_id].map_count = 0;
        return VA_STATUS_SUCCESS;
    }
    return VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus hobot_vaDestroyBuffer(VADriverContextP ctx, VABufferID buffer_id) {
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    for (int i = 1; i < MAX_IMAGES; i++) {
        if (drv->images[i].allocated && drv->images[i].buf_id == buffer_id) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    VAStatus status = hobot_vaDestroyBufferLocked(drv, buffer_id);
    pthread_mutex_unlock(&drv->mutex);
    return status;
}

static VAStatus hobot_vaSyncSurface(VADriverContextP ctx, VASurfaceID render_target);
static VAStatus hobot_vaSyncSurfaceLocked(HobotDriverData *drv,
                                           VASurfaceID render_target);
typedef struct HobotDecodedNV12Layout {
    const unsigned char *y_plane;
    const unsigned char *uv_plane;
    uint32_t y_stride;
    uint32_t uv_stride;
    uint32_t vertical_rows;
    uint64_t uv_offset;
    int contiguous;
} HobotDecodedNV12Layout;

static VAStatus hobot_get_decoded_nv12_planes(
    HobotSurface *surface,
    HobotDecodedNV12Layout *layout);

typedef struct HobotPreallocatedNV12Layout {
    uint32_t stride;
    uint32_t vstride;
    uint64_t y_offset;
    uint64_t uv_offset;
    uint64_t y_extent;
    uint64_t uv_extent;
    uint64_t object_size[2];
    int separate_fds;
} HobotPreallocatedNV12Layout;

static VAStatus hobot_get_preallocated_nv12_layout(
    const HobotSurface *surf,
    HobotPreallocatedNV12Layout *layout)
{
    if (!surf || !layout || !surf->has_preallocated ||
        surf->width == 0 || surf->height == 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    const hb_mem_graphic_buf_t *gbuf = &surf->preallocated_gbuf;
    uint32_t stride = gbuf->stride > 0 ? (uint32_t)gbuf->stride :
                      (surf->stride > 0 ? (uint32_t)surf->stride : surf->width);
    uint32_t vstride = gbuf->vstride > 0 ? (uint32_t)gbuf->vstride : surf->height;
    if (gbuf->fd[0] < 0 || stride < surf->width || vstride < surf->height ||
        (stride & 1u) != 0 || (vstride & 1u) != 0 ||
        gbuf->size[0] == 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    uint64_t y_extent = (uint64_t)stride * vstride;
    uint64_t uv_extent = (uint64_t)stride * ((uint64_t)vstride / 2u);
    uint64_t y_offset = gbuf->offset[0];
    int separate_fds = gbuf->fd[1] >= 0 && gbuf->fd[1] != gbuf->fd[0];
    if (y_offset > UINT64_MAX - y_extent)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    uint64_t y_end = y_offset + y_extent;

    uint64_t physical_delta = 0;
    int have_physical_delta = 0;
    if (!separate_fds && gbuf->is_contig &&
        gbuf->phys_addr[0] > 0 && gbuf->phys_addr[1] > 0) {
        if (gbuf->phys_addr[1] <= gbuf->phys_addr[0])
            return VA_STATUS_ERROR_OPERATION_FAILED;
        physical_delta = gbuf->phys_addr[1] - gbuf->phys_addr[0];
        have_physical_delta = 1;
    }

    uint64_t uv_offset;
    if (separate_fds) {
        uv_offset = gbuf->offset[1];
    } else if (gbuf->offset[1] > 0) {
        uv_offset = gbuf->offset[1];
    } else if (have_physical_delta) {
        if (y_offset > UINT64_MAX - physical_delta)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        uv_offset = y_offset + physical_delta;
    } else {
        uv_offset = y_end;
    }
    if (uv_offset > UINT64_MAX - uv_extent)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    uint64_t uv_end = uv_offset + uv_extent;
    uint64_t object_size0 = 0;
    uint64_t object_size1 = 0;
    if (separate_fds) {
        if (gbuf->size[1] == 0 || y_end > gbuf->size[0] ||
            uv_end > gbuf->size[1])
            return VA_STATUS_ERROR_OPERATION_FAILED;
        object_size0 = gbuf->size[0];
        object_size1 = gbuf->size[1];
    } else if (gbuf->size[1] > 0) {
        if (gbuf->size[0] < y_extent || gbuf->size[1] < uv_extent ||
            uv_offset < y_end ||
            y_offset > UINT64_MAX - gbuf->size[0] ||
            uv_offset > UINT64_MAX - gbuf->size[1])
            return VA_STATUS_ERROR_OPERATION_FAILED;
        uint64_t y_allocation_end = y_offset + gbuf->size[0];
        uint64_t uv_allocation_end = uv_offset + gbuf->size[1];
        object_size0 = y_allocation_end > uv_allocation_end ?
                       y_allocation_end : uv_allocation_end;
    } else {
        if (y_end > gbuf->size[0] || uv_end > gbuf->size[0] ||
            uv_offset < y_end)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        object_size0 = gbuf->size[0];
    }

    if (!separate_fds && have_physical_delta &&
        (uv_offset < y_offset || physical_delta != uv_offset - y_offset))
        return VA_STATUS_ERROR_OPERATION_FAILED;

    memset(layout, 0, sizeof(*layout));
    layout->stride = stride;
    layout->vstride = vstride;
    layout->y_offset = y_offset;
    layout->uv_offset = uv_offset;
    layout->y_extent = y_extent;
    layout->uv_extent = uv_extent;
    layout->object_size[0] = object_size0;
    layout->object_size[1] = object_size1;
    layout->separate_fds = separate_fds;
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_upload_staging_to_gbuf(HobotSurface *surf)
{
    if (!surf || !surf->raw_data || !surf->raw_data_dirty || !surf->has_preallocated)
        return VA_STATUS_SUCCESS;

    hb_mem_graphic_buf_t *gbuf = &surf->preallocated_gbuf;
    HobotPreallocatedNV12Layout layout;
    if (hobot_get_preallocated_nv12_layout(surf, &layout) != VA_STATUS_SUCCESS)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    uint32_t src_stride = surf->stride > 0 ? (uint32_t)surf->stride : surf->width;
    uint32_t dst_stride = layout.stride;
    uint64_t src_y_size = (uint64_t)src_stride * surf->height;
    uint64_t src_uv_size = (uint64_t)src_stride * (surf->height / 2u);
    uint64_t src_total = src_y_size + src_uv_size;
    uint8_t *dst_y = gbuf->virt_addr[0];
    uint8_t *dst_uv = gbuf->virt_addr[1];

    if (src_stride < surf->width || (src_stride & 1u) != 0 ||
        src_total > surf->raw_data_size || !dst_y) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!dst_uv) {
        if (layout.separate_fds || !gbuf->is_contig ||
            layout.uv_offset < layout.y_offset ||
            layout.uv_offset - layout.y_offset > UINTPTR_MAX - (uintptr_t)dst_y)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        dst_uv = (uint8_t *)((uintptr_t)dst_y +
                             (uintptr_t)(layout.uv_offset - layout.y_offset));
    }

    const uint8_t *src_y = surf->raw_data;
    const uint8_t *src_uv = src_y + src_y_size;
    for (uint32_t row = 0; row < surf->height; row++)
        memcpy(dst_y + (size_t)row * dst_stride,
               src_y + (size_t)row * src_stride, surf->width);
    for (uint32_t row = 0; row < surf->height / 2u; row++)
        memcpy(dst_uv + (size_t)row * dst_stride,
               src_uv + (size_t)row * src_stride, surf->width);

    if (gbuf->fd[0] < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    if (layout.separate_fds) {
        if (hb_mem_flush_buf(gbuf->fd[0], 0, layout.object_size[0]) != 0 ||
            hb_mem_flush_buf(gbuf->fd[1], 0, layout.object_size[1]) != 0)
            return VA_STATUS_ERROR_OPERATION_FAILED;
    } else {
        if (hb_mem_flush_buf(gbuf->fd[0], 0, layout.object_size[0]) != 0)
            return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    surf->raw_data_dirty = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_copy_gbuf_to_staging(HobotSurface *surf)
{
    if (!surf || !surf->has_preallocated)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    hb_mem_graphic_buf_t *gbuf = &surf->preallocated_gbuf;
    HobotPreallocatedNV12Layout layout;
    if (hobot_get_preallocated_nv12_layout(surf, &layout) != VA_STATUS_SUCCESS)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    uint32_t src_stride = layout.stride;
    uint32_t dst_stride = surf->stride > 0 ? (uint32_t)surf->stride : surf->width;
    uint64_t dst_total = (uint64_t)dst_stride * surf->height +
                         (uint64_t)dst_stride * (surf->height / 2u);
    uint8_t *src_y = gbuf->virt_addr[0];
    uint8_t *src_uv = gbuf->virt_addr[1];

    if (dst_stride < surf->width || (dst_stride & 1u) != 0 ||
        dst_total > surf->raw_data_size || !src_y) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!src_uv) {
        if (layout.separate_fds || !gbuf->is_contig ||
            layout.uv_offset < layout.y_offset ||
            layout.uv_offset - layout.y_offset > UINTPTR_MAX - (uintptr_t)src_y)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        src_uv = (uint8_t *)((uintptr_t)src_y +
                             (uintptr_t)(layout.uv_offset - layout.y_offset));
    }

    if (layout.separate_fds) {
        if (hb_mem_invalidate_buf(gbuf->fd[0], 0, layout.object_size[0]) != 0 ||
            hb_mem_invalidate_buf(gbuf->fd[1], 0, layout.object_size[1]) != 0)
            return VA_STATUS_ERROR_OPERATION_FAILED;
    } else {
        if (hb_mem_invalidate_buf(gbuf->fd[0], 0, layout.object_size[0]) != 0)
            return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (!surf->raw_data) {
        surf->raw_data = calloc(1, surf->raw_data_size);
        if (!surf->raw_data)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    uint8_t *dst_uv = (uint8_t *)surf->raw_data + (size_t)dst_stride * surf->height;
    for (uint32_t row = 0; row < surf->height; row++)
        memcpy((uint8_t *)surf->raw_data + (size_t)row * dst_stride,
               src_y + (size_t)row * src_stride, surf->width);
    for (uint32_t row = 0; row < surf->height / 2u; row++)
        memcpy(dst_uv + (size_t)row * dst_stride,
               src_uv + (size_t)row * src_stride, surf->width);

    surf->raw_data_valid = 1;
    surf->raw_data_dirty = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaBeginPicture(
    VADriverContextP ctx,
    VAContextID context,
    VASurfaceID render_target
) {
    va_trace("vaBeginPicture: context=%u, render_target=%u", context, render_target);
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (context <= 0 || context >= MAX_CONTEXTS) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    int sync_retry = 0;
retry_surface_sync:
    pthread_mutex_lock(&drv->mutex);
    if (!drv->contexts[context].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    HobotContext *hctx = &drv->contexts[context];
    while (hctx->sync_active) {
        if (pthread_cond_wait(&drv->sync_cond, &drv->mutex) != 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (!hctx->allocated ||
            (!hctx->vpu_running && !hctx->encoder_init_deferred)) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_CONTEXT;
        }
    }
    if (!hctx->vpu_running && !hctx->encoder_init_deferred) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!hctx->is_encoder && hctx->decode_failed) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!hctx->is_encoder && hctx->decode_picture_active) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (hctx->is_encoder && (hctx->encoder_failed || hctx->encoder_picture_active ||
                             hctx->enc_in_buf_valid || hctx->enc_out_buf_valid)) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (render_target <= 0 || render_target >= MAX_SURFACES || !drv->surfaces[render_target].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    HobotSurface *surf = &drv->surfaces[render_target];
    int surface_size_valid = surf->width >= (unsigned int)hctx->width &&
                             surf->height >= (unsigned int)hctx->height;
    if (!surface_size_valid && hctx->is_encoder &&
        hctx->vpu_ctx.codec_id == MEDIA_CODEC_ID_H264)
        surface_size_valid = hobot_h264_surface_alignment_candidate(hctx, surf);
    if (!surface_size_valid && hctx->is_encoder &&
        hctx->vpu_ctx.codec_id == MEDIA_CODEC_ID_H265)
        surface_size_valid = hobot_hevc_surface_alignment_candidate(hctx, surf);
    if (!surface_size_valid) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (surf->lock_count > 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (!hctx->is_encoder) {
        if (surf->decode_pending) {
            int same_context = surf->context_id == context;
            pthread_mutex_unlock(&drv->mutex);
            if (!same_context)
                return VA_STATUS_ERROR_OPERATION_FAILED;
            if (sync_retry++)
                return VA_STATUS_ERROR_OPERATION_FAILED;
            VAStatus sync_status = hobot_vaSyncSurface(ctx, render_target);
            if (sync_status != VA_STATUS_SUCCESS)
                return sync_status;
            goto retry_surface_sync;
        }
        if ((hctx->sub_tail - hctx->sub_head) >= 128) {
            fprintf(stderr, "[HOBOT-VA] submitted_surfaces FIFO overflow (tail=%u head=%u)\n",
                    hctx->sub_tail, hctx->sub_head);
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
        }
        VAContextID owner_id = surf->output_context_id;
        if (surf->has_decoded_frame &&
            (owner_id <= 0 || owner_id >= MAX_CONTEXTS ||
             !drv->contexts[owner_id].allocated ||
             !drv->contexts[owner_id].vpu_running ||
             drv->contexts[owner_id].is_encoder ||
             drv->contexts[owner_id].sync_active ||
             drv->contexts[owner_id].decode_picture_active)) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_CONTEXT;
        }
        if (hctx->dec_in_buf_valid) {
            int in_qret = hb_mm_mc_queue_input_buffer(&hctx->vpu_ctx, &hctx->dec_in_buf, 50);
            if (in_qret != 0) {
                fprintf(stderr, "[HOBOT-VA] vaBeginPicture: leftover queue_input_buffer failed: %d\n", in_qret);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            hctx->dec_in_buf_valid = 0;
            hctx->dec_in_buf_offset = 0;
        }
        if (surf->has_decoded_frame) {
            int qret = 0;
            if (surf->vpu_out_buf.vframe_buf.phy_ptr[0] != 0 &&
                surf->vpu_out_buf.vframe_buf.size > 0) {
                qret = hb_mm_mc_queue_output_buffer(&drv->contexts[owner_id].vpu_ctx,
                                                    &surf->vpu_out_buf, 50);
            }
            if (qret != 0) {
                fprintf(stderr, "[HOBOT-VA] vaBeginPicture: queue_output_buffer failed: %d\n", qret);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            surf->has_decoded_frame = 0;
            surf->dma_fd = -1;
            surf->output_context_id = 0;
            memset(&surf->vpu_out_buf, 0, sizeof(surf->vpu_out_buf));
        }
        surf->context_id = context;
        hctx->current_render_target = render_target;
        hctx->enc_coded_buf = 0;
        surf->decode_pending = 1;
        surf->decode_error = 0;
        hctx->decode_slice_fragment_open = 0;
        if (hctx->profile == VAProfileHEVCMain) {
            hctx->hevc_decode_picture_valid = 0;
            memset(&hctx->hevc_decode_picture, 0,
                   sizeof(hctx->hevc_decode_picture));
        }
        if (hctx->profile == VAProfileJPEGBaseline) {
            hctx->jpeg_decode_picture_valid = 0;
            hctx->jpeg_decode_slice_valid = 0;
            hctx->jpeg_decode_data_submitted = 0;
            memset(&hctx->jpeg_decode_picture, 0,
                   sizeof(hctx->jpeg_decode_picture));
            memset(&hctx->jpeg_decode_slice, 0,
                   sizeof(hctx->jpeg_decode_slice));
        }
        hctx->submitted_surfaces[hctx->sub_tail++ % 128] = render_target;
        hctx->decode_picture_active = 1;
        hctx->frame_count++;
    } else {
        if (surf->decode_pending) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        surf->context_id = context;
        hctx->current_render_target = render_target;
        hctx->enc_coded_buf = 0;
        hctx->encoder_picture_active = 1;
    }
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaRenderPicture(
    VADriverContextP ctx,
    VAContextID context,
    VABufferID *buffers,
    int num_buffers
) {
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (context <= 0 || context >= MAX_CONTEXTS)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (num_buffers < 0 || num_buffers >= MAX_BUFFERS ||
        (num_buffers > 0 && !buffers))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    HobotContext *hctx = &drv->contexts[context];
    pthread_mutex_lock(&drv->mutex);
    if (!hctx->allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (hctx->sync_active) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!hctx->vpu_running && !hctx->encoder_init_deferred) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!hctx->is_encoder && hctx->decode_failed) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!hctx->is_encoder && !hctx->decode_picture_active) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (hctx->is_encoder && (hctx->encoder_failed || !hctx->encoder_picture_active)) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    va_trace("vaRenderPicture: context=%u buffers=%d profile=%d encoder=%d",
             context, num_buffers, hctx->profile, hctx->is_encoder);

    VAEncPictureParameterBufferJPEG *jpeg_picture = NULL;
    VAQMatrixBufferJPEG *jpeg_qmatrix = NULL;
    VAHuffmanTableBufferJPEGBaseline *jpeg_huffman = NULL;
    VAEncSliceParameterBufferJPEG *jpeg_slice = NULL;
    VAEncSequenceParameterBufferHEVC *hevc_encode_sequence = NULL;
    VAEncPictureParameterBufferHEVC *hevc_encode_picture = NULL;
    VAEncSliceParameterBufferHEVC *hevc_encode_slice = NULL;
    VAPictureParameterBufferJPEGBaseline *jpeg_decode_picture = NULL;
    VAIQMatrixBufferJPEGBaseline *jpeg_decode_qmatrix = NULL;
    VAHuffmanTableBufferJPEGBaseline *jpeg_decode_huffman = NULL;
    VASliceParameterBufferJPEGBaseline *jpeg_decode_slice = NULL;
    VAPictureParameterBufferHEVC *hevc_decode_picture = NULL;
    VAIQMatrixBufferJPEGBaseline jpeg_decode_qmatrix_effective = {0};
    VAHuffmanTableBufferJPEGBaseline jpeg_decode_huffman_effective = {0};
    uint8_t jpeg_decode_header[1024];
    size_t jpeg_decode_header_size = 0;
    int sequence_parameter_seen = 0;
    int h264_encode_slice_seen = 0;
    const VAEncMiscParameterHRD *h264_hrd = NULL;
    const VAEncMiscParameterHRD *hevc_hrd = NULL;
    uint64_t h264_target_bitrate_bps =
        (uint64_t)hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate * 1000u;
    uint64_t hevc_target_bitrate_bps =
        (uint64_t)hctx->vpu_ctx.video_enc_params.rc_params.h265_cbr_params.bit_rate * 1000u;
    VABufferID slice_param_ids[MAX_BUFFERS];
    VABufferID slice_data_ids[MAX_BUFFERS];
    int slice_param_count = 0;
    int slice_data_count = 0;
    const size_t misc_header_size = offsetof(VAEncMiscParameterBuffer, data);
    for (int i = 0; i < num_buffers; i++) {
        VABufferID bid = buffers[i];
        if (bid <= 0 || bid >= MAX_BUFFERS || !drv->buffers[bid].allocated) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }

        HobotBuffer *buffer = &drv->buffers[bid];
        va_trace("vaRenderPicture: buffer id=%u type=%u size=%u element=%u count=%u",
                 bid, buffer->type, buffer->size, buffer->element_size,
                 buffer->num_elements);
        if (buffer->map_count > 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (buffer->size > 0 && !buffer->data) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }

        if (hctx->is_encoder) {
            size_t required_size = 0;
            if (buffer->type == VAEncPictureParameterBufferType) {
                if (hctx->profile == VAProfileHEVCMain)
                    required_size = sizeof(VAEncPictureParameterBufferHEVC);
                else if (hctx->profile == VAProfileJPEGBaseline)
                    required_size = sizeof(VAEncPictureParameterBufferJPEG);
                else
                    required_size = sizeof(VAEncPictureParameterBufferH264);

                if (buffer->size < required_size) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }

                if (hctx->profile == VAProfileHEVCMain) {
                    if (buffer->num_elements != 1 ||
                        buffer->element_size < sizeof(VAEncPictureParameterBufferHEVC) ||
                        hevc_encode_picture) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    hevc_encode_picture =
                        (VAEncPictureParameterBufferHEVC *)buffer->data;
                    if (!hobot_hevc_encode_picture_supported(hevc_encode_picture) ||
                        (hctx->hevc_encode_picture_valid &&
                         !hobot_hevc_encode_picture_static_equal(
                             &hctx->hevc_encode_picture, hevc_encode_picture))) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                    }
                }

                VABufferID coded_id;
                if (hctx->profile == VAProfileHEVCMain)
                    coded_id = ((VAEncPictureParameterBufferHEVC *)buffer->data)->coded_buf;
                else if (hctx->profile == VAProfileJPEGBaseline) {
                    VAEncPictureParameterBufferJPEG *picture =
                        (VAEncPictureParameterBufferJPEG *)buffer->data;
                    if (picture->quality < 1 || picture->quality > 100 ||
                        !hobot_jpeg_picture_supported(hctx, picture) || jpeg_picture) {
                        va_trace("vaRenderPicture: unsupported JPEG picture quality=%u size=%ux%u ctx=%dx%d depth=%u components=%u scans=%u flags=%x/%u/%u/%u/%u/%u ids=%u,%u,%u qsel=%u,%u,%u",
                                 picture->quality, picture->picture_width,
                                 picture->picture_height, hctx->width, hctx->height,
                                 picture->sample_bit_depth, picture->num_components,
                                 picture->num_scan, picture->pic_flags.value,
                                 picture->pic_flags.bits.profile,
                                 picture->pic_flags.bits.progressive,
                                 picture->pic_flags.bits.huffman,
                                 picture->pic_flags.bits.interleaved,
                                 picture->pic_flags.bits.differential,
                                 picture->component_id[0], picture->component_id[1],
                                 picture->component_id[2],
                                 picture->quantiser_table_selector[0],
                                 picture->quantiser_table_selector[1],
                                 picture->quantiser_table_selector[2]);
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    jpeg_picture = picture;
                    coded_id = ((VAEncPictureParameterBufferJPEG *)buffer->data)->coded_buf;
                } else
                    coded_id = ((VAEncPictureParameterBufferH264 *)buffer->data)->coded_buf;

                if (coded_id <= 0 || coded_id >= MAX_BUFFERS ||
                    !drv->buffers[coded_id].allocated ||
                    drv->buffers[coded_id].type != VAEncCodedBufferType ||
                    !drv->buffers[coded_id].data ||
                    drv->buffers[coded_id].size == 0 ||
                    drv->buffers[coded_id].capacity < drv->buffers[coded_id].size) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_BUFFER;
                }
            } else if (buffer->type == VAEncSequenceParameterBufferType) {
                if (hctx->profile == VAProfileJPEGBaseline) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                }
                if (hctx->profile != VAProfileJPEGBaseline) {
                    if (sequence_parameter_seen) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    sequence_parameter_seen = 1;
                }
                if (hctx->profile == VAProfileHEVCMain)
                    required_size = sizeof(VAEncSequenceParameterBufferHEVC);
                else if (hctx->profile != VAProfileJPEGBaseline)
                    required_size = sizeof(VAEncSequenceParameterBufferH264);
                if (required_size != 0 && buffer->size < required_size) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                if (hctx->profile == VAProfileHEVCMain) {
                    if (buffer->num_elements != 1 ||
                        buffer->element_size < sizeof(VAEncSequenceParameterBufferHEVC)) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    hevc_encode_sequence =
                        (VAEncSequenceParameterBufferHEVC *)buffer->data;
                    if (!hobot_hevc_encode_sequence_supported(
                            hevc_encode_sequence, hctx->width, hctx->height) ||
                        (hctx->hevc_encode_sequence_valid &&
                         !hobot_hevc_encode_sequence_static_equal(
                             &hctx->hevc_encode_sequence, hevc_encode_sequence))) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                    }
                    if (hevc_encode_sequence->bits_per_second > 0)
                        hevc_target_bitrate_bps =
                            hevc_encode_sequence->bits_per_second;
                }
                if ((hctx->profile == VAProfileH264ConstrainedBaseline ||
                     hctx->profile == VAProfileH264Main ||
                     hctx->profile == VAProfileH264High) &&
                    ((VAEncSequenceParameterBufferH264 *)buffer->data)->bits_per_second > 0) {
                    uint32_t target_kbps =
                        ((VAEncSequenceParameterBufferH264 *)buffer->data)->bits_per_second / 1000u;
                    if (target_kbps > 0)
                        h264_target_bitrate_bps = (uint64_t)target_kbps * 1000u;
                }
            } else if (buffer->type == VAEncMiscParameterBufferType) {
                if (hctx->profile == VAProfileJPEGBaseline) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                }
                if (buffer->size < misc_header_size) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                VAEncMiscParameterBuffer *misc = (VAEncMiscParameterBuffer *)buffer->data;
                size_t payload_size = buffer->size - misc_header_size;
                if ((misc->type == VAEncMiscParameterTypeRateControl &&
                     payload_size < sizeof(VAEncMiscParameterRateControl)) ||
                    (misc->type == VAEncMiscParameterTypeFrameRate &&
                     payload_size < sizeof(VAEncMiscParameterFrameRate)) ||
                    (misc->type == VAEncMiscParameterTypeHRD &&
                     payload_size < sizeof(VAEncMiscParameterHRD))) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                if (hctx->profile == VAProfileHEVCMain &&
                    misc->type == VAEncMiscParameterTypeRateControl &&
                    (((VAEncMiscParameterRateControl *)misc->data)->bits_per_second >
                         700000000u ||
                     (((VAEncMiscParameterRateControl *)misc->data)->bits_per_second > 0 &&
                      ((VAEncMiscParameterRateControl *)misc->data)->bits_per_second < 1000u))) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                if (misc->type != VAEncMiscParameterTypeRateControl &&
                    misc->type != VAEncMiscParameterTypeFrameRate &&
                    misc->type != VAEncMiscParameterTypeHRD) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                }
                if (misc->type == VAEncMiscParameterTypeRateControl) {
                    uint32_t requested_bps =
                        ((VAEncMiscParameterRateControl *)misc->data)->bits_per_second;
                    uint32_t requested_kbps = requested_bps / 1000u;
                    if (hctx->profile == VAProfileHEVCMain && requested_kbps > 0)
                        hevc_target_bitrate_bps = (uint64_t)requested_kbps * 1000u;
                    else if (requested_kbps > 0)
                        h264_target_bitrate_bps = (uint64_t)requested_kbps * 1000u;
                } else if (misc->type == VAEncMiscParameterTypeHRD) {
                    if (hctx->profile == VAProfileHEVCMain) {
                        if (hevc_hrd) {
                            pthread_mutex_unlock(&drv->mutex);
                            return VA_STATUS_ERROR_INVALID_PARAMETER;
                        }
                        hevc_hrd = (const VAEncMiscParameterHRD *)misc->data;
                    } else if (hctx->profile == VAProfileH264ConstrainedBaseline ||
                               hctx->profile == VAProfileH264Main ||
                               hctx->profile == VAProfileH264High) {
                        if (h264_hrd) {
                            pthread_mutex_unlock(&drv->mutex);
                            return VA_STATUS_ERROR_INVALID_PARAMETER;
                        }
                        h264_hrd = (const VAEncMiscParameterHRD *)misc->data;
                    } else {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                    }
                    const VAEncMiscParameterHRD *hrd =
                        hctx->profile == VAProfileHEVCMain ? hevc_hrd : h264_hrd;
                    for (size_t j = 0;
                         j < sizeof(hrd->va_reserved) / sizeof(hrd->va_reserved[0]);
                         j++) {
                        if (hrd->va_reserved[j] != 0) {
                            pthread_mutex_unlock(&drv->mutex);
                            return VA_STATUS_ERROR_INVALID_PARAMETER;
                        }
                    }
                    if (hrd->buffer_size == 0 ||
                        hrd->initial_buffer_fullness > hrd->buffer_size) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                }
            } else if (buffer->type == VAEncSliceParameterBufferType &&
                       hctx->profile == VAProfileHEVCMain) {
                if (!buffer->data ||
                    buffer->size < sizeof(VAEncSliceParameterBufferHEVC) ||
                    buffer->element_size < sizeof(VAEncSliceParameterBufferHEVC) ||
                    buffer->num_elements != 1 || hevc_encode_slice) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                hevc_encode_slice =
                    (VAEncSliceParameterBufferHEVC *)buffer->data;
            } else if (buffer->type == VAEncSliceParameterBufferType &&
                       (hctx->profile == VAProfileH264ConstrainedBaseline ||
                        hctx->profile == VAProfileH264Main ||
                        hctx->profile == VAProfileH264High)) {
                if (!buffer->data ||
                    buffer->size < sizeof(VAEncSliceParameterBufferH264) ||
                    buffer->element_size < sizeof(VAEncSliceParameterBufferH264) ||
                    buffer->num_elements != 1) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                if (h264_encode_slice_seen) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                }
                h264_encode_slice_seen = 1;

                const VAEncSliceParameterBufferH264 *slice =
                    (const VAEncSliceParameterBufferH264 *)buffer->data;
                uint64_t mb_width = ((uint64_t)hctx->width + 15u) / 16u;
                uint64_t mb_height = ((uint64_t)hctx->height + 15u) / 16u;
                if (mb_width == 0 || mb_height == 0 ||
                    mb_width > UINT32_MAX / mb_height ||
                    (slice->slice_type > 2 &&
                     (slice->slice_type < 5 || slice->slice_type > 7))) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                if (slice->slice_type % 5u != 0 &&
                    slice->slice_type % 5u != 2u) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                }
                if (slice->macroblock_address != 0 ||
                    slice->num_macroblocks != mb_width * mb_height ||
                    slice->macroblock_info != VA_INVALID_ID) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                }
            } else if (hctx->profile == VAProfileJPEGBaseline &&
                       buffer->type == VAQMatrixBufferType) {
                if (buffer->size < sizeof(VAQMatrixBufferJPEG) || jpeg_qmatrix) {
                    va_trace("vaRenderPicture: invalid JPEG QMatrix buffer size=%u", buffer->size);
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                jpeg_qmatrix = (VAQMatrixBufferJPEG *)buffer->data;
                if (jpeg_qmatrix->load_lum_quantiser_matrix < 0 ||
                    jpeg_qmatrix->load_lum_quantiser_matrix > 1 ||
                    jpeg_qmatrix->load_chroma_quantiser_matrix < 0 ||
                    jpeg_qmatrix->load_chroma_quantiser_matrix > 1) {
                    va_trace("vaRenderPicture: invalid JPEG QMatrix load flags=%d/%d",
                             jpeg_qmatrix->load_lum_quantiser_matrix,
                             jpeg_qmatrix->load_chroma_quantiser_matrix);
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                if ((jpeg_qmatrix->load_lum_quantiser_matrix &&
                     memchr(jpeg_qmatrix->lum_quantiser_matrix, 0,
                            sizeof(jpeg_qmatrix->lum_quantiser_matrix))) ||
                    (jpeg_qmatrix->load_chroma_quantiser_matrix &&
                     memchr(jpeg_qmatrix->chroma_quantiser_matrix, 0,
                            sizeof(jpeg_qmatrix->chroma_quantiser_matrix)))) {
                    va_trace("vaRenderPicture: zero entry in loaded JPEG QMatrix");
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
            } else if (hctx->profile == VAProfileJPEGBaseline &&
                       buffer->type == VAHuffmanTableBufferType) {
                if (buffer->size < sizeof(VAHuffmanTableBufferJPEGBaseline) ||
                    jpeg_huffman) {
                    va_trace("vaRenderPicture: invalid JPEG Huffman buffer size=%u", buffer->size);
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                jpeg_huffman = (VAHuffmanTableBufferJPEGBaseline *)buffer->data;
                if (!hobot_jpeg_huffman_tables_supported(jpeg_huffman)) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                }
            } else if (hctx->profile == VAProfileJPEGBaseline &&
                       buffer->type == VAEncSliceParameterBufferType) {
                if (buffer->size < sizeof(VAEncSliceParameterBufferJPEG) ||
                    buffer->num_elements != 1 || jpeg_slice) {
                    va_trace("vaRenderPicture: invalid JPEG slice buffer size=%u elements=%u",
                             buffer->size, buffer->num_elements);
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                jpeg_slice = (VAEncSliceParameterBufferJPEG *)buffer->data;
                if (jpeg_slice->num_components != 3 ||
                    jpeg_slice->components[0].component_selector != 1 ||
                    jpeg_slice->components[1].component_selector != 2 ||
                    jpeg_slice->components[2].component_selector != 3 ||
                    jpeg_slice->components[0].dc_table_selector != 0 ||
                    jpeg_slice->components[0].ac_table_selector != 0 ||
                    jpeg_slice->components[1].dc_table_selector != 1 ||
                    jpeg_slice->components[1].ac_table_selector != 1 ||
                    jpeg_slice->components[2].dc_table_selector != 1 ||
                    jpeg_slice->components[2].ac_table_selector != 1) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                }
            } else {
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            }
        } else {
            if (buffer->type == VAPictureParameterBufferType) {
                size_t required_size = 0;
                if (hctx->profile == VAProfileHEVCMain)
                    required_size = sizeof(VAPictureParameterBufferHEVC);
                else if (hctx->profile == VAProfileJPEGBaseline)
                    required_size = sizeof(VAPictureParameterBufferJPEGBaseline);
                else
                    required_size = sizeof(VAPictureParameterBufferH264);
                if (!buffer->data || buffer->size < required_size) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                if (hctx->profile == VAProfileHEVCMain) {
                    if (hevc_decode_picture) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    hevc_decode_picture =
                        (VAPictureParameterBufferHEVC *)buffer->data;
                }
                if (hctx->profile == VAProfileJPEGBaseline) {
                    if (jpeg_decode_picture) {
                        va_trace("vaRenderPicture: duplicate JPEG picture parameters");
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    jpeg_decode_picture =
                        (VAPictureParameterBufferJPEGBaseline *)buffer->data;
                }
                if (hctx->profile != VAProfileHEVCMain &&
                    hctx->profile != VAProfileJPEGBaseline &&
                    !hobot_h264_picture_parameters_supported(
                        (VAPictureParameterBufferH264 *)buffer->data,
                        hctx->profile, hctx->width, hctx->height)) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
            } else if (hctx->profile == VAProfileJPEGBaseline &&
                       buffer->type == VAIQMatrixBufferType) {
                if (!buffer->data ||
                    buffer->size < sizeof(VAIQMatrixBufferJPEGBaseline) ||
                    jpeg_decode_qmatrix) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                jpeg_decode_qmatrix = (VAIQMatrixBufferJPEGBaseline *)buffer->data;
            } else if (hctx->profile == VAProfileJPEGBaseline &&
                       buffer->type == VAHuffmanTableBufferType) {
                if (!buffer->data ||
                    buffer->size < sizeof(VAHuffmanTableBufferJPEGBaseline) ||
                    jpeg_decode_huffman) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                jpeg_decode_huffman = (VAHuffmanTableBufferJPEGBaseline *)buffer->data;
            } else if (buffer->type == VASliceParameterBufferType) {
                if (hctx->profile == VAProfileJPEGBaseline) {
                    if (!buffer->data ||
                        buffer->element_size < sizeof(VASliceParameterBufferJPEGBaseline) ||
                        buffer->num_elements != 1 ||
                        buffer->size < buffer->element_size || jpeg_decode_slice) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    jpeg_decode_slice =
                        (VASliceParameterBufferJPEGBaseline *)buffer->data;
                    slice_param_ids[slice_param_count++] = bid;
                    continue;
                }
                if (hctx->profile != VAProfileHEVCMain &&
                    hctx->profile != VAProfileH264ConstrainedBaseline &&
                    hctx->profile != VAProfileH264Main &&
                    hctx->profile != VAProfileH264High) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                }
                size_t min_slice_parameter_size =
                    hctx->profile == VAProfileHEVCMain ?
                    sizeof(VASliceParameterBufferHEVC) :
                    sizeof(VASliceParameterBufferH264);
                if (!buffer->data || buffer->element_size < min_slice_parameter_size ||
                    buffer->num_elements == 0 ||
                    buffer->num_elements > buffer->size / buffer->element_size) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                if (hctx->profile == VAProfileHEVCMain) {
                    if (buffer->num_elements > MAX_BUFFERS) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
                    }
                    for (unsigned int slice_index = 0;
                         slice_index < buffer->num_elements; slice_index++) {
                        const VASliceParameterBufferHEVC *slice =
                            (const VASliceParameterBufferHEVC *)
                                ((const uint8_t *)buffer->data +
                                 (size_t)slice_index * buffer->element_size);
                        if (!hobot_hevc_slice_parameter_fields_supported(slice)) {
                            va_trace("vaRenderPicture: unsupported HEVC slice fields index=%u flag=%u size=%u offset=%u byte_offset=%u dependent=%u color_plane=%u slice_type=%u entry_offsets=%u reserved=%u",
                                     slice_index, slice->slice_data_flag,
                                     slice->slice_data_size,
                                     slice->slice_data_offset,
                                     slice->slice_data_byte_offset,
                                     slice->LongSliceFlags.fields.dependent_slice_segment_flag,
                                     slice->LongSliceFlags.fields.color_plane_id,
                                     slice->LongSliceFlags.fields.slice_type,
                                     slice->num_entry_point_offsets,
                                     slice->LongSliceFlags.fields.reserved);
                            pthread_mutex_unlock(&drv->mutex);
                            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                        }
                    }
                }
                slice_param_ids[slice_param_count++] = bid;
            } else if (buffer->type == VASliceDataBufferType) {
                if (buffer->size == 0 || buffer->size > INT_MAX || !buffer->data) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                slice_data_ids[slice_data_count++] = bid;
            } else if (hctx->profile == VAProfileJPEGBaseline) {
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            }
        }
    }

    if (!hctx->is_encoder && hctx->profile == VAProfileJPEGBaseline) {
        if (jpeg_decode_picture) {
                if (hctx->jpeg_decode_picture_valid) {
                va_trace("vaRenderPicture: duplicate JPEG picture parameters");
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
            hctx->jpeg_decode_picture = *jpeg_decode_picture;
            hctx->jpeg_decode_picture_valid = 1;
        }
        if (jpeg_decode_qmatrix) {
            for (size_t i = 0; i < 4; i++) {
                if (jpeg_decode_qmatrix->load_quantiser_table[i] > 1) {
                    va_trace("vaRenderPicture: invalid JPEG quantizer load flag table=%zu value=%u",
                             i, jpeg_decode_qmatrix->load_quantiser_table[i]);
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                int table_has_data =
                    !memchr(jpeg_decode_qmatrix->quantiser_table[i], 0, 64);
                if (jpeg_decode_qmatrix->load_quantiser_table[i]) {
                    if (!table_has_data) {
                        int all_zero = 1;
                        for (size_t j = 0; j < 64; j++)
                            all_zero &= jpeg_decode_qmatrix->quantiser_table[i][j] == 0;
                        if (all_zero) {
                            memset(hctx->jpeg_decode_qmatrix.quantiser_table[i], 0, 64);
                            hctx->jpeg_decode_qmatrix_valid[i] = 0;
                            continue;
                        }
                        va_trace("vaRenderPicture: malformed JPEG quantizer table=%zu", i);
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    memcpy(hctx->jpeg_decode_qmatrix.quantiser_table[i],
                           jpeg_decode_qmatrix->quantiser_table[i], 64);
                    hctx->jpeg_decode_qmatrix_valid[i] = 1;
                } else if (!hctx->jpeg_decode_qmatrix_valid[i] && table_has_data) {
                    memcpy(hctx->jpeg_decode_qmatrix.quantiser_table[i],
                           jpeg_decode_qmatrix->quantiser_table[i], 64);
                    hctx->jpeg_decode_qmatrix_valid[i] = 1;
                }
            }
        }
        if (jpeg_decode_huffman) {
            for (size_t table = 0; table < 2; table++) {
                if (jpeg_decode_huffman->load_huffman_table[table] > 1) {
                    va_trace("vaRenderPicture: invalid JPEG Huffman load flag table=%zu value=%u",
                             table, jpeg_decode_huffman->load_huffman_table[table]);
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                if (jpeg_decode_huffman->load_huffman_table[table] ||
                    !hctx->jpeg_decode_huffman_valid[table]) {
                    size_t dc_values = 0;
                    size_t ac_values = 0;
                    const typeof(jpeg_decode_huffman->huffman_table[0]) *src =
                        &jpeg_decode_huffman->huffman_table[table];
                    int has_data = 0;
                    const uint8_t *raw = (const uint8_t *)src;
                    for (size_t i = 0; i < sizeof(*src); i++)
                        has_data |= raw[i] != 0;
                    if (!has_data) {
                        memset(&hctx->jpeg_decode_huffman.huffman_table[table],
                               0, sizeof(*src));
                        hctx->jpeg_decode_huffman_valid[table] = 0;
                        continue;
                    }
                    if (src->pad[0] != 0 || src->pad[1] != 0 ||
                        !hobot_jpeg_huffman_decode_table_valid(
                            src->num_dc_codes, src->dc_values, 12, 0,
                            &dc_values) ||
                        !hobot_jpeg_huffman_decode_table_valid(
                            src->num_ac_codes, src->ac_values, 162, 1,
                            &ac_values)) {
                        if (jpeg_decode_huffman->load_huffman_table[table]) {
                            va_trace("vaRenderPicture: invalid loaded JPEG Huffman table=%zu",
                                     table);
                            pthread_mutex_unlock(&drv->mutex);
                            return VA_STATUS_ERROR_INVALID_PARAMETER;
                        }
                        continue;
                    }
                    (void)dc_values;
                    (void)ac_values;
                    hctx->jpeg_decode_huffman.huffman_table[table] = *src;
                    hctx->jpeg_decode_huffman_valid[table] = 1;
                }
            }
        }
        if (jpeg_decode_slice) {
            if (hctx->jpeg_decode_slice_valid) {
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
            hctx->jpeg_decode_slice = *jpeg_decode_slice;
            hctx->jpeg_decode_slice_valid = 1;
        }

        jpeg_decode_picture = hctx->jpeg_decode_picture_valid ?
            &hctx->jpeg_decode_picture : NULL;
        jpeg_decode_slice = hctx->jpeg_decode_slice_valid ?
            &hctx->jpeg_decode_slice : NULL;
        for (size_t i = 0; i < 4; i++) {
            if (hctx->jpeg_decode_qmatrix_valid[i]) {
                jpeg_decode_qmatrix_effective.load_quantiser_table[i] = 1;
                memcpy(jpeg_decode_qmatrix_effective.quantiser_table[i],
                       hctx->jpeg_decode_qmatrix.quantiser_table[i], 64);
            }
        }
        for (size_t table = 0; table < 2; table++) {
            if (hctx->jpeg_decode_huffman_valid[table]) {
                jpeg_decode_huffman_effective.load_huffman_table[table] = 1;
                jpeg_decode_huffman_effective.huffman_table[table] =
                    hctx->jpeg_decode_huffman.huffman_table[table];
            }
        }
        jpeg_decode_qmatrix = &jpeg_decode_qmatrix_effective;
        jpeg_decode_huffman = &jpeg_decode_huffman_effective;
    }

    int32_t h264_vbv_window_ms = 0;
    if (h264_hrd) {
        int hrd_status = hobot_hrd_vbv_window_ms(
            h264_target_bitrate_bps, h264_hrd, &h264_vbv_window_ms);
        if (hrd_status < 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if (hrd_status == 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        }
    }
    int32_t hevc_vbv_window_ms = 0;
    if (hevc_hrd) {
        int hrd_status = hobot_hrd_vbv_window_ms(
            hevc_target_bitrate_bps, hevc_hrd, &hevc_vbv_window_ms);
        if (hrd_status < 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if (hrd_status == 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        }
    }

    if (hctx->is_encoder && hctx->profile == VAProfileHEVCMain) {
        const VAEncSequenceParameterBufferHEVC *sequence = hevc_encode_sequence;
        if (!sequence && hctx->hevc_encode_sequence_valid)
            sequence = &hctx->hevc_encode_sequence;
        if (!sequence || !hevc_encode_picture || !hevc_encode_slice) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if (!hobot_hevc_encode_slice_supported(
                sequence, hevc_encode_picture, hevc_encode_slice,
                hctx->width, hctx->height)) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        }
    }

    if (!hctx->is_encoder && hctx->profile == VAProfileJPEGBaseline) {
        if (slice_param_count > 1 || slice_data_count > 1 ||
            (slice_param_count == 1 && !jpeg_decode_slice)) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if (slice_data_count == 1) {
            if (hctx->jpeg_decode_data_submitted || !jpeg_decode_picture ||
                !jpeg_decode_slice || !jpeg_decode_qmatrix ||
                !jpeg_decode_huffman) {
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
            HobotBuffer *jpeg_data = &drv->buffers[slice_data_ids[0]];
            if (jpeg_decode_slice->slice_data_offset > jpeg_data->size ||
                jpeg_decode_slice->slice_data_size == 0 ||
                jpeg_decode_slice->slice_data_size >
                    jpeg_data->size - jpeg_decode_slice->slice_data_offset) {
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
            VAStatus jpeg_status = hobot_jpeg_build_decode_header(
                jpeg_decode_picture, jpeg_decode_qmatrix, jpeg_decode_huffman,
                jpeg_decode_slice, hctx->width, hctx->height,
                jpeg_decode_header, sizeof(jpeg_decode_header),
                &jpeg_decode_header_size);
            if (jpeg_status != VA_STATUS_SUCCESS) {
                pthread_mutex_unlock(&drv->mutex);
                return jpeg_status;
            }
        }
    } else if (!hctx->is_encoder && hctx->profile == VAProfileHEVCMain &&
               (slice_param_count > 0 || slice_data_count > 0)) {
        int packed_slices = slice_param_count == 1 && slice_data_count == 1;
        if (slice_param_count == 0 || slice_data_count == 0 ||
            (!packed_slices && slice_param_count != slice_data_count)) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }

        size_t total_slices = 0;
        if (packed_slices) {
            total_slices = drv->buffers[slice_param_ids[0]].num_elements;
        } else {
            total_slices = (size_t)slice_param_count;
            for (int i = 0; i < slice_param_count; i++) {
                if (drv->buffers[slice_param_ids[i]].num_elements != 1) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
            }
        }
        if (total_slices == 0 || total_slices > MAX_BUFFERS) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
        }

        const VAPictureParameterBufferHEVC *picture = hevc_decode_picture ?
            hevc_decode_picture :
            (hctx->hevc_decode_picture_valid ? &hctx->hevc_decode_picture : NULL);
        HobotHevcSliceSequence sequence;
        if (!picture ||
            !hobot_hevc_slice_sequence_init(picture, total_slices, &sequence)) {
            va_trace("vaRenderPicture: unsupported HEVC slice sequence or picture layout");
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        }

        for (size_t slice_index = 0; slice_index < total_slices; slice_index++) {
            size_t param_index = packed_slices ? 0 : slice_index;
            size_t data_index = packed_slices ? 0 : slice_index;
            const HobotBuffer *slice_params =
                &drv->buffers[slice_param_ids[param_index]];
            const HobotBuffer *slice_data = &drv->buffers[slice_data_ids[data_index]];
            const VASliceParameterBufferHEVC *slice =
                (const VASliceParameterBufferHEVC *)
                    ((const uint8_t *)slice_params->data +
                     (packed_slices ? slice_index * slice_params->element_size : 0));
            if (!hobot_hevc_slice_sequence_add(
                    &sequence, slice, slice_data->size)) {
                va_trace("vaRenderPicture: invalid HEVC slice sequence at index=%zu",
                         slice_index);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
            if (picture->num_short_term_ref_pic_sets > 2 &&
                !hobot_hevc_idr_slice_supported(
                    picture, slice, (const uint8_t *)slice_data->data,
                    slice_data->size, &sequence)) {
                va_trace("vaRenderPicture: HEVC picture with unmodeled SPS RPS is not a validated independent IDR slice at index=%zu",
                         slice_index);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            }
            if (picture->num_short_term_ref_pic_sets <= 2 &&
                !hobot_hevc_validated_rps_slice_supported(
                    picture, slice, (const uint8_t *)slice_data->data,
                    slice_data->size, &sequence)) {
                va_trace("vaRenderPicture: HEVC RPS slice is outside the validated syntax subset at index=%zu",
                         slice_index);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            }
        }
        if (!hobot_hevc_slice_sequence_complete(&sequence)) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
    } else if (!hctx->is_encoder && slice_param_count > 0) {
        int packed_slices = slice_param_count == 1 && slice_data_count == 1;
        if (slice_param_count != slice_data_count && !packed_slices) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if (slice_param_count == slice_data_count && !packed_slices) {
            for (int i = 0; i < slice_param_count; i++) {
                if (drv->buffers[slice_param_ids[i]].num_elements != 1) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
            }
        }
        int fragment_open = hctx->decode_slice_fragment_open;
        for (int i = 0; i < slice_data_count; i++) {
            int param_index = packed_slices ? 0 : i;
            if (!hobot_validate_h264_slice_group(
                    &drv->buffers[slice_param_ids[param_index]],
                    &drv->buffers[slice_data_ids[i]], fragment_open,
                    &fragment_open)) {
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
        }
    } else if (!hctx->is_encoder && hctx->decode_slice_fragment_open &&
               slice_data_count > 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (hctx->is_encoder && hctx->profile == VAProfileJPEGBaseline &&
        (jpeg_picture || jpeg_qmatrix || jpeg_slice)) {
        unsigned int quality = jpeg_picture ? jpeg_picture->quality :
            hctx->vpu_ctx.video_enc_params.jpeg_enc_config.quality_factor;
        VAStatus jpeg_status = hobot_apply_jpeg_parameters(
            hctx, quality, jpeg_qmatrix, jpeg_slice);
        if (jpeg_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return jpeg_status;
        }
    }

    media_codec_context_t *mctx = &hctx->vpu_ctx;

    if (hctx->is_encoder) {
        for (int i = 0; i < num_buffers; i++) {
            VABufferID bid = buffers[i];
            HobotBuffer *b = &drv->buffers[bid];

            if (b->type == VAEncPictureParameterBufferType) {
                size_t required_size;
                if (hctx->profile == VAProfileHEVCMain) {
                    required_size = sizeof(VAEncPictureParameterBufferHEVC);
                } else if (hctx->profile == VAProfileJPEGBaseline) {
                    required_size = sizeof(VAEncPictureParameterBufferJPEG);
                } else {
                    required_size = sizeof(VAEncPictureParameterBufferH264);
                }
                if (!b->data || b->size < required_size) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                if (hctx->profile == VAProfileHEVCMain) {
                    VAEncPictureParameterBufferHEVC *pic = (VAEncPictureParameterBufferHEVC *)b->data;
                    if (pic->pic_fields.bits.idr_pic_flag && hctx->vpu_running) {
                        int ret = hb_mm_mc_request_idr_frame(&hctx->vpu_ctx);
                        if (ret != 0) {
                            fprintf(stderr, "[HOBOT-VA] IDR request failed for ctx=%u: %d\n",
                                    context, ret);
                            pthread_mutex_unlock(&drv->mutex);
                            return VA_STATUS_ERROR_OPERATION_FAILED;
                        }
                    }
                    hctx->enc_coded_buf = pic->coded_buf;
                    hctx->hevc_encode_picture = *pic;
                    hctx->hevc_encode_picture_valid = 1;
                } else if (hctx->profile == VAProfileJPEGBaseline) {
                    VAEncPictureParameterBufferJPEG *pic = (VAEncPictureParameterBufferJPEG *)b->data;
                    hctx->enc_coded_buf = pic->coded_buf;
                } else {
                    VAEncPictureParameterBufferH264 *pic = (VAEncPictureParameterBufferH264 *)b->data;
                    if (pic->pic_fields.bits.idr_pic_flag && hctx->vpu_running) {
                        int ret = hb_mm_mc_request_idr_frame(&hctx->vpu_ctx);
                        if (ret != 0) {
                            fprintf(stderr, "[HOBOT-VA] IDR request failed for ctx=%u: %d\n",
                                    context, ret);
                            pthread_mutex_unlock(&drv->mutex);
                            return VA_STATUS_ERROR_OPERATION_FAILED;
                        }
                    }
                    hctx->enc_coded_buf = pic->coded_buf;
                }
            } else if (b->type == VAEncSequenceParameterBufferType) {
                mc_rate_control_params_t previous_rc =
                    hctx->vpu_ctx.video_enc_params.rc_params;
                mc_rate_control_params_t pending_rc = previous_rc;
                VAEncSequenceParameterBufferH264 *pending_h264_sequence = NULL;
                if (hctx->profile == VAProfileHEVCMain) {
                    if (!b->data || b->size < sizeof(VAEncSequenceParameterBufferHEVC)) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    VAEncSequenceParameterBufferHEVC *seq = (VAEncSequenceParameterBufferHEVC *)b->data;
                    if (!hobot_hevc_encode_sequence_supported(
                             seq, hctx->width, hctx->height) ||
                         (hctx->hevc_encode_sequence_valid &&
                          !hobot_hevc_encode_sequence_static_equal(
                              &hctx->hevc_encode_sequence, seq))) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                    }
                    if (seq->intra_period > 0) {
                        pending_rc.h265_cbr_params.intra_period = seq->intra_period;
                    }
                    if (seq->bits_per_second > 0) {
                        pending_rc.h265_cbr_params.bit_rate = seq->bits_per_second / 1000u;
                    }
                    if (seq->vui_parameters_present_flag &&
                        seq->vui_fields.bits.vui_timing_info_present_flag) {
                        uint64_t fps =
                            ((uint64_t)seq->vui_time_scale +
                             seq->vui_num_units_in_tick / 2u) /
                            seq->vui_num_units_in_tick;
                        if (fps == 0 || fps > 240u) {
                            pthread_mutex_unlock(&drv->mutex);
                            return VA_STATUS_ERROR_INVALID_PARAMETER;
                        }
                        pending_rc.h265_cbr_params.frame_rate = (uint32_t)fps;
                    }
                } else if (hctx->profile != VAProfileJPEGBaseline) {
                    if (!b->data || b->size < sizeof(VAEncSequenceParameterBufferH264)) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    VAEncSequenceParameterBufferH264 *seq = (VAEncSequenceParameterBufferH264 *)b->data;
                    if (!hobot_h264_level_supported(seq->level_idc)) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                    }
                    if (!hctx->h264_sequence_valid &&
                        !hctx->encoder_init_deferred) {
                        va_trace("vaRenderPicture: first H.264 sequence arrived after encoder startup");
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                    }
                    uint32_t coded_width, coded_height, visible_width, visible_height;
                    if (!hobot_h264_sequence_dimensions(seq, &coded_width,
                                                       &coded_height, &visible_width,
                                                       &visible_height) ||
                        coded_width != (uint32_t)hctx->width ||
                        coded_height != (uint32_t)hctx->height) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    VASurfaceID target_id = hctx->current_render_target;
                    if (target_id <= 0 || target_id >= MAX_SURFACES ||
                        !drv->surfaces[target_id].allocated) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_SURFACE;
                    }
                    HobotSurface *target = &drv->surfaces[target_id];
                    if (!(target->width >= coded_width && target->height >= coded_height) &&
                        !(target->width == visible_width &&
                          target->height == visible_height)) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_SURFACE;
                    }
                    if (seq->vui_parameters_present_flag &&
                        seq->vui_fields.bits.aspect_ratio_info_present_flag &&
                        seq->aspect_ratio_idc == 255 &&
                        (seq->sar_width == 0 || seq->sar_height == 0 ||
                         seq->sar_width > UINT16_MAX || seq->sar_height > UINT16_MAX)) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    if (seq->vui_parameters_present_flag &&
                        seq->vui_fields.bits.timing_info_present_flag &&
                        (seq->num_units_in_tick == 0 || seq->time_scale == 0 ||
                         seq->num_units_in_tick > INT32_MAX ||
                         seq->time_scale > INT32_MAX)) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    if (hctx->h264_sequence_valid &&
                        !hobot_h264_sequence_config_equal(&hctx->h264_sequence, seq)) {
                        va_trace("vaRenderPicture: H.264 level/VUI change requires a new context");
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                    }
                    pending_h264_sequence = seq;
                    if (seq->intra_period > 0) {
                        pending_rc.h264_cbr_params.intra_period = seq->intra_period;
                    }
                    if (seq->bits_per_second > 0) {
                        pending_rc.h264_cbr_params.bit_rate = seq->bits_per_second / 1000;
                    }
                    uint32_t sequence_fps = 0;
                    int fps_status = hobot_h264_sequence_frame_rate(seq, &sequence_fps);
                    if (fps_status < 0) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    if (fps_status > 0) {
                        pending_rc.h264_cbr_params.frame_rate = sequence_fps;
                    }
                }
                hctx->vpu_ctx.video_enc_params.rc_params = pending_rc;
                VAStatus rc_status = hobot_apply_encoder_rate_control(hctx, &previous_rc);
                if (rc_status != VA_STATUS_SUCCESS) {
                    pthread_mutex_unlock(&drv->mutex);
                    return rc_status;
                }
                if (pending_h264_sequence) {
                    if (!hctx->h264_sequence_valid) {
                        hctx->h264_sequence = *pending_h264_sequence;
                        hctx->h264_sequence_valid = 1;
                    }
                    hctx->vpu_ctx.video_enc_params.h264_enc_config.h264_level =
                        (mc_h264_level_t)pending_h264_sequence->level_idc;
                } else if (hctx->profile == VAProfileHEVCMain) {
                    VAEncSequenceParameterBufferHEVC *seq =
                        (VAEncSequenceParameterBufferHEVC *)b->data;
                    hctx->hevc_encode_sequence = *seq;
                    hctx->hevc_encode_sequence_valid = 1;
                }
            } else if (b->type == VAEncMiscParameterBufferType) {
                const size_t misc_header_size = offsetof(VAEncMiscParameterBuffer, data);
                if (!b->data || b->size < misc_header_size) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                VAEncMiscParameterBuffer *misc = (VAEncMiscParameterBuffer *)b->data;
                if (misc->type == VAEncMiscParameterTypeRateControl) {
                    if (b->size - misc_header_size < sizeof(VAEncMiscParameterRateControl)) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    VAEncMiscParameterRateControl *rc = (VAEncMiscParameterRateControl *)misc->data;
                    uint32_t kbps = rc->bits_per_second / 1000;
                    if (hctx->profile == VAProfileHEVCMain &&
                        rc->bits_per_second > 0 && kbps == 0) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    if (kbps > 0) {
                        mc_rate_control_params_t previous_rc =
                            hctx->vpu_ctx.video_enc_params.rc_params;
                        if (hctx->vpu_ctx.codec_id == MEDIA_CODEC_ID_H264) {
                            hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate = kbps;
                        } else if (hctx->vpu_ctx.codec_id == MEDIA_CODEC_ID_H265) {
                            hctx->vpu_ctx.video_enc_params.rc_params.h265_cbr_params.bit_rate = kbps;
                        }
                        VAStatus rc_status = hobot_apply_encoder_rate_control(hctx, &previous_rc);
                        if (rc_status != VA_STATUS_SUCCESS) {
                            pthread_mutex_unlock(&drv->mutex);
                            return rc_status;
                        }
                    }
                } else if (misc->type == VAEncMiscParameterTypeFrameRate) {
                    if (b->size - misc_header_size < sizeof(VAEncMiscParameterFrameRate)) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    VAEncMiscParameterFrameRate *fr = (VAEncMiscParameterFrameRate *)misc->data;
                    uint32_t num = fr->framerate & 0xffff;
                    uint32_t den = (fr->framerate >> 16) & 0xffff;
                    if (den == 0) den = 1;
                    uint32_t fps = (num + den / 2u) / den;
                    if (fps > 240u) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    if (fps > 0) {
                        mc_rate_control_params_t previous_rc =
                            hctx->vpu_ctx.video_enc_params.rc_params;
                        if (hctx->vpu_ctx.codec_id == MEDIA_CODEC_ID_H264) {
                            hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.frame_rate = fps;
                        } else if (hctx->vpu_ctx.codec_id == MEDIA_CODEC_ID_H265) {
                            hctx->vpu_ctx.video_enc_params.rc_params.h265_cbr_params.frame_rate = fps;
                        }
                        VAStatus rc_status = hobot_apply_encoder_rate_control(hctx, &previous_rc);
                        if (rc_status != VA_STATUS_SUCCESS) {
                            pthread_mutex_unlock(&drv->mutex);
                            return rc_status;
                        }
                    }
                } else if (misc->type == VAEncMiscParameterTypeHRD) {
                    /* Apply after the loop so HRD is independent of buffer order. */
                }
            }
        }
        if (h264_hrd) {
            mc_rate_control_params_t previous_rc = mctx->video_enc_params.rc_params;
            /* The SDK exposes VBV duration but no initial CPB fullness control. */
            mctx->video_enc_params.rc_params.h264_cbr_params.vbv_buffer_size =
                h264_vbv_window_ms;
            VAStatus rc_status = hobot_apply_encoder_rate_control(hctx, &previous_rc);
            if (rc_status != VA_STATUS_SUCCESS) {
                pthread_mutex_unlock(&drv->mutex);
                return rc_status;
            }
            va_trace("vaRenderPicture: H.264 VBV window set to %d ms", h264_vbv_window_ms);
        }
        if (hevc_hrd) {
            mc_rate_control_params_t previous_rc = mctx->video_enc_params.rc_params;
            mctx->video_enc_params.rc_params.h265_cbr_params.vbv_buffer_size =
                hevc_vbv_window_ms;
            VAStatus rc_status = hobot_apply_encoder_rate_control(hctx, &previous_rc);
            if (rc_status != VA_STATUS_SUCCESS) {
                pthread_mutex_unlock(&drv->mutex);
                return rc_status;
            }
            va_trace("vaRenderPicture: HEVC VBV window set to %d ms",
                     hevc_vbv_window_ms);
        }
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_SUCCESS;
    }

    /* Decoder First pass: parse picture parameters if present */
    for (int i = 0; i < num_buffers; i++) {
        VABufferID bid = buffers[i];
        HobotBuffer *b = &drv->buffers[bid];
        if (b->type != VAPictureParameterBufferType)
            continue;

        if (hctx->profile == VAProfileHEVCMain) {
            VAPictureParameterBufferHEVC *pic = (VAPictureParameterBufferHEVC *)b->data;
            if (!hobot_hevc_picture_parameters_supported(
                    pic, hctx->width, hctx->height)) {
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            }
            hctx->hevc_decode_picture = *pic;
            hctx->hevc_decode_picture_valid = 1;
            uint8_t new_vps[sizeof(hctx->cached_vps)];
            uint8_t new_sps[sizeof(hctx->cached_sps)];
            uint8_t new_pps[sizeof(hctx->cached_pps)];
            int new_vps_len = generate_hevc_vps(pic, 1, new_vps, sizeof(new_vps));
            int new_sps_len = generate_hevc_sps(pic, 1, new_sps, sizeof(new_sps));
            int new_pps_len = generate_hevc_pps(pic, new_pps, sizeof(new_pps));
            int sequence_changed =
                hobot_header_changed(hctx->cached_vps, hctx->cached_vps_len,
                                     new_vps, new_vps_len) ||
                hobot_header_changed(hctx->cached_sps, hctx->cached_sps_len,
                                     new_sps, new_sps_len) ||
                hobot_header_changed(hctx->cached_pps, hctx->cached_pps_len,
                                     new_pps, new_pps_len);
            if (sequence_changed && hctx->headers_sent) {
                hctx->headers_sent = 0;
                va_trace("vaRenderPicture: HEVC sequence changed; re-injecting parameter sets");
            }
            memcpy(hctx->cached_vps, new_vps, (size_t)new_vps_len);
            memcpy(hctx->cached_sps, new_sps, (size_t)new_sps_len);
            memcpy(hctx->cached_pps, new_pps, (size_t)new_pps_len);
            hctx->cached_vps_len = new_vps_len;
            hctx->cached_sps_len = new_sps_len;
            hctx->cached_pps_len = new_pps_len;
        } else if (hctx->profile != VAProfileJPEGBaseline) {
            VAPictureParameterBufferH264 *pic = (VAPictureParameterBufferH264 *)b->data;
            /* VA-API does not expose the PPS default reference count.
             * Wave521 needs a non-zero default for the multi-reference
             * 1080p streams that otherwise fail with error 0x3006. */
            unsigned int default_l0_active_minus1 =
                (pic->num_ref_frames >= 3) ? 2 : 0;
            uint8_t new_sps[sizeof(hctx->cached_sps)];
            uint8_t new_pps[sizeof(hctx->cached_pps)];
            int new_sps_len = generate_h264_sps(pic, hctx->profile,
                                                new_sps, sizeof(new_sps));
            int new_pps_len = generate_h264_pps(
                pic, default_l0_active_minus1,
                new_pps, sizeof(new_pps));
            int sequence_changed =
                hobot_header_changed(hctx->cached_sps, hctx->cached_sps_len,
                                     new_sps, new_sps_len) ||
                hobot_header_changed(hctx->cached_pps, hctx->cached_pps_len,
                                     new_pps, new_pps_len);
            if (sequence_changed && hctx->headers_sent) {
                hctx->headers_sent = 0;
                va_trace("vaRenderPicture: H.264 sequence changed; re-injecting parameter sets");
            }
            memcpy(hctx->cached_sps, new_sps, (size_t)new_sps_len);
            memcpy(hctx->cached_pps, new_pps, (size_t)new_pps_len);
            hctx->cached_sps_len = new_sps_len;
            hctx->cached_pps_len = new_pps_len;
        }
    }

    /* Decoder Second pass: feed slice data (accumulated per picture) */
    int slice_data_index = 0;
    for (int i = 0; i < num_buffers; i++) {
        VABufferID bid = buffers[i];
        HobotBuffer *b = &drv->buffers[bid];
        if (b->type == VASliceDataBufferType) {
                if (hctx->profile == VAProfileJPEGBaseline) {
                    if (slice_data_index++ != 0 || !jpeg_decode_slice ||
                        !jpeg_decode_header_size || !b->data ||
                        jpeg_decode_slice->slice_data_offset > b->size ||
                        jpeg_decode_slice->slice_data_size >
                            b->size - jpeg_decode_slice->slice_data_offset) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    if (!hctx->dec_in_buf_valid) {
                        memset(&hctx->dec_in_buf, 0, sizeof(hctx->dec_in_buf));
                        int ret = -1;
                        for (int retry = 0; retry < 5; retry++) {
                            ret = hb_mm_mc_dequeue_input_buffer(mctx,
                                                               &hctx->dec_in_buf, 50);
                            if (ret == 0)
                                break;
                        }
                        if (ret != 0) {
                            fprintf(stderr, "[HOBOT-VA] JPEG dequeue_input_buffer failed: %d\n", ret);
                            hobot_abort_pending_decode_picture(drv, hctx);
                            pthread_mutex_unlock(&drv->mutex);
                            return VA_STATUS_ERROR_OPERATION_FAILED;
                        }
                        hctx->dec_in_buf_valid = 1;
                        hctx->dec_in_buf_offset = 0;
                    }

                    size_t input_capacity = hctx->dec_in_buf.vstream_buf.size;
                    size_t configured_capacity = mctx->video_dec_params.bitstream_buf_size > 0 ?
                        (size_t)mctx->video_dec_params.bitstream_buf_size : input_capacity;
                    if (configured_capacity < input_capacity)
                        input_capacity = configured_capacity;
                    size_t scan_size = jpeg_decode_slice->slice_data_size;
                    if (!hctx->dec_in_buf.vstream_buf.vir_ptr ||
                        input_capacity == 0 || jpeg_decode_header_size > input_capacity ||
                        scan_size > input_capacity - jpeg_decode_header_size ||
                        jpeg_decode_header_size + scan_size > INT_MAX) {
                        fprintf(stderr,
                                "[HOBOT-VA] JPEG input buffer too small (header=%zu scan=%zu capacity=%zu)\n",
                                jpeg_decode_header_size, scan_size, input_capacity);
                        int recycle_ret = hobot_recycle_decoder_input(hctx);
                        if (recycle_ret != 0) {
                            fprintf(stderr, "[HOBOT-VA] JPEG input buffer recycle failed: %d\n",
                                    recycle_ret);
                            hctx->decode_failed = 1;
                        }
                        hobot_abort_pending_decode_picture(drv, hctx);
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
                    }

                    uint8_t *dst = (uint8_t *)hctx->dec_in_buf.vstream_buf.vir_ptr;
                    memcpy(dst, jpeg_decode_header, jpeg_decode_header_size);
                    memcpy(dst + jpeg_decode_header_size,
                           (const uint8_t *)b->data +
                               jpeg_decode_slice->slice_data_offset,
                           scan_size);
                    hctx->dec_in_buf_offset =
                        (int)(jpeg_decode_header_size + scan_size);
                    hctx->jpeg_decode_data_submitted = 1;
                    continue;
                }

                HobotBuffer *slice_params = NULL;
                if (slice_param_count == 1 && slice_data_count == 1) {
                    slice_params = &drv->buffers[slice_param_ids[0]];
                } else if (slice_param_count > 0) {
                    slice_params = &drv->buffers[slice_param_ids[slice_data_index]];
                }
                slice_data_index++;

                if (!hctx->dec_in_buf_valid) {
                    memset(&hctx->dec_in_buf, 0, sizeof(hctx->dec_in_buf));
                    int ret = -1;
                    for (int retry = 0; retry < 5; retry++) {
                        ret = hb_mm_mc_dequeue_input_buffer(mctx, &hctx->dec_in_buf, 50);
                        if (ret == 0) break;
                    }
                    if (ret != 0) {
                        fprintf(stderr, "[HOBOT-VA] dequeue_input_buffer failed: %d\n", ret);
                        hctx->headers_sent = 0;
                        hobot_abort_pending_decode_picture(drv, hctx);
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_OPERATION_FAILED;
                    }
                    hctx->dec_in_buf_valid = 1;
                    hctx->dec_in_buf_offset = 0;
                    if (!hctx->dec_in_buf.vstream_buf.vir_ptr) {
                        int recycle_ret = hobot_recycle_decoder_input(hctx);
                        if (recycle_ret != 0) {
                            fprintf(stderr, "[HOBOT-VA] invalid input buffer recycle failed: %d\n", recycle_ret);
                        }
                        hctx->headers_sent = 0;
                        hobot_abort_pending_decode_picture(drv, hctx);
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_OPERATION_FAILED;
                    }

                    uint8_t *dst = (uint8_t *)hctx->dec_in_buf.vstream_buf.vir_ptr;
                    int max_cap = mctx->video_dec_params.bitstream_buf_size > 0 ?
                                  mctx->video_dec_params.bitstream_buf_size : (4 * 1024 * 1024);
                    size_t header_size = 0;
                    if (hctx->cached_vps_len > 0) header_size += 4u + (size_t)hctx->cached_vps_len;
                    if (hctx->cached_sps_len > 0) header_size += 4u + (size_t)hctx->cached_sps_len;
                    if (hctx->cached_pps_len > 0) header_size += 4u + (size_t)hctx->cached_pps_len;
                    if (max_cap <= 0 || header_size > (size_t)max_cap) {
                        int recycle_ret = hobot_recycle_decoder_input(hctx);
                        if (recycle_ret != 0) {
                            fprintf(stderr, "[HOBOT-VA] header buffer recycle failed: %d\n", recycle_ret);
                        }
                        hctx->headers_sent = 0;
                        hobot_abort_pending_decode_picture(drv, hctx);
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_OPERATION_FAILED;
                    }
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
                int max_cap = mctx->video_dec_params.bitstream_buf_size > 0 ?
                              mctx->video_dec_params.bitstream_buf_size : (4 * 1024 * 1024);

                if (hctx->dec_in_buf_offset < 0 || max_cap <= 0) {
                    int recycle_ret = hobot_recycle_decoder_input(hctx);
                    if (recycle_ret != 0) {
                        fprintf(stderr, "[HOBOT-VA] invalid decoder input recycle failed: %d\n", recycle_ret);
                    }
                    hctx->headers_sent = 0;
                    hobot_abort_pending_decode_picture(drv, hctx);
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }

                unsigned int slice_count = slice_params ? slice_params->num_elements : 1;
                int fragment_open = hctx->decode_slice_fragment_open;
                for (unsigned int slice_index = 0; slice_index < slice_count; slice_index++) {
                    size_t data_offset = 0;
                    size_t data_size = b->size;
                    uint32_t data_flag = VA_SLICE_DATA_FLAG_ALL;
                    if (slice_params) {
                        if (hctx->profile == VAProfileHEVCMain) {
                            const VASliceParameterBufferHEVC *slice =
                                (const VASliceParameterBufferHEVC *)
                                    ((const uint8_t *)slice_params->data +
                                     (size_t)slice_index * slice_params->element_size);
                            data_offset = slice->slice_data_offset;
                            data_size = slice->slice_data_size;
                            data_flag = slice->slice_data_flag;
                        } else {
                            const VASliceParameterBufferH264 *slice =
                                (const VASliceParameterBufferH264 *)
                                    ((const uint8_t *)slice_params->data +
                                     (size_t)slice_index * slice_params->element_size);
                            data_offset = slice->slice_data_offset;
                            data_size = slice->slice_data_size;
                            data_flag = slice->slice_data_flag;
                        }
                    }

                    const uint8_t *src = (const uint8_t *)b->data + data_offset;
                    uint8_t *rewritten_data = NULL;
                    if (hctx->profile == VAProfileHEVCMain && slice_params) {
                        const VASliceParameterBufferHEVC *slice =
                            (const VASliceParameterBufferHEVC *)
                                ((const uint8_t *)slice_params->data +
                                 (size_t)slice_index * slice_params->element_size);
                        size_t rewritten_size = 0;
                        int rewrite_status = hobot_hevc_rewrite_single_rps_slice(
                            hctx->hevc_decode_picture_valid ?
                                &hctx->hevc_decode_picture : NULL,
                            slice, (const uint8_t *)b->data, b->size,
                            &rewritten_data, &rewritten_size);
                        if (rewrite_status < 0) {
                            int recycle_ret = hobot_recycle_decoder_input(hctx);
                            if (recycle_ret != 0)
                                hctx->decode_failed = 1;
                            hctx->headers_sent = 0;
                            hobot_abort_pending_decode_picture(drv, hctx);
                            pthread_mutex_unlock(&drv->mutex);
                            return VA_STATUS_ERROR_OPERATION_FAILED;
                        }
                        if (rewrite_status > 0) {
                            src = rewritten_data;
                            data_size = rewritten_size;
                            va_trace("vaRenderPicture: inserted single-SPS-RPS index bit and realigned HEVC slice header");
                        }
                    }
                    int has_start_code = hobot_has_annexb_start_code(src, data_size);
                    int add_start_code =
                        (data_flag == VA_SLICE_DATA_FLAG_ALL ||
                         data_flag == VA_SLICE_DATA_FLAG_BEGIN) && !has_start_code;
                    size_t prefix_size = add_start_code ? 4u : 0u;
                    size_t output_offset = (size_t)hctx->dec_in_buf_offset;
                    if (output_offset > (size_t)max_cap ||
                        prefix_size > (size_t)max_cap - output_offset ||
                        data_size > (size_t)max_cap - output_offset - prefix_size) {
                        fprintf(stderr, "[HOBOT-VA] bitstream buffer overflow! offset=%d needed=%zu cap=%d\n",
                                hctx->dec_in_buf_offset, prefix_size + data_size, max_cap);
                        free(rewritten_data);
                        int recycle_ret = hobot_recycle_decoder_input(hctx);
                        if (recycle_ret != 0) {
                            fprintf(stderr, "[HOBOT-VA] overflow buffer recycle failed: %d\n", recycle_ret);
                        }
                        hctx->headers_sent = 0;
                        hobot_abort_pending_decode_picture(drv, hctx);
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_OPERATION_FAILED;
                    }

                    if (add_start_code) {
                        dst[hctx->dec_in_buf_offset++] = 0x00;
                        dst[hctx->dec_in_buf_offset++] = 0x00;
                        dst[hctx->dec_in_buf_offset++] = 0x00;
                        dst[hctx->dec_in_buf_offset++] = 0x01;
                    }
                    memcpy(dst + hctx->dec_in_buf_offset, src, data_size);
                    hctx->dec_in_buf_offset += (int)data_size;
                    free(rewritten_data);

                    if (data_flag == VA_SLICE_DATA_FLAG_BEGIN)
                        fragment_open = 1;
                    else if (data_flag == VA_SLICE_DATA_FLAG_END)
                        fragment_open = 0;
                    hctx->decode_slice_fragment_open = fragment_open;
                }
        }
    }
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaEndPicture(VADriverContextP ctx, VAContextID context) {
    va_trace("vaEndPicture: context=%u", context);
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    if (context <= 0 || context >= MAX_CONTEXTS) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    HobotContext *hctx = &drv->contexts[context];
    pthread_mutex_lock(&drv->mutex);
    if (!hctx->allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }
    if (hctx->sync_active) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!hctx->is_encoder && hctx->decode_failed) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (hctx->is_encoder && (hctx->encoder_failed || !hctx->encoder_picture_active)) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (hctx->is_encoder) {
        VASurfaceID sid = hctx->current_render_target;
        if (sid <= 0 || sid >= MAX_SURFACES || !drv->surfaces[sid].allocated) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        HobotSurface *surf = &drv->surfaces[sid];

        VABufferID cid = hctx->enc_coded_buf;
        if (cid <= 0 || cid >= MAX_BUFFERS || !drv->buffers[cid].allocated) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }
        HobotBuffer *coded = &drv->buffers[cid];
        if (coded->map_count > 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (coded->type != VAEncCodedBufferType || !coded->data ||
            coded->size == 0 || coded->capacity < coded->size) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_BUFFER;
        }

        int enc_w = hctx->vpu_ctx.video_enc_params.width;
        int enc_h = hctx->vpu_ctx.video_enc_params.height;
        if (enc_w <= 0 || enc_h <= 0 || (enc_w & 1) != 0 || (enc_h & 1) != 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        int full_surface = surf->width >= (unsigned int)enc_w &&
                           surf->height >= (unsigned int)enc_h;
        int cropped_h264_surface = hctx->vpu_ctx.codec_id == MEDIA_CODEC_ID_H264 &&
                                   hobot_h264_surface_matches_sequence(hctx, surf);
        int aligned_hevc_surface = hctx->vpu_ctx.codec_id == MEDIA_CODEC_ID_H265 &&
                                   hctx->hevc_encode_sequence_valid &&
                                   (surf->width <= (unsigned int)enc_w ?
                                        (unsigned int)enc_w - surf->width < 16u :
                                        surf->width - (unsigned int)enc_w < 16u) &&
                                   (surf->height <= (unsigned int)enc_h ?
                                        (unsigned int)enc_h - surf->height < 16u :
                                        surf->height - (unsigned int)enc_h < 16u);
        if (!full_surface && !cropped_h264_surface && !aligned_hevc_surface) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        int copy_x = 0;
        int copy_y = 0;
        int copy_w = enc_w;
        int copy_h = enc_h;
        if (!full_surface &&
            !hobot_h264_surface_copy_region(hctx, surf, enc_w, enc_h,
                                            &copy_x, &copy_y,
                                            &copy_w, &copy_h) &&
            !aligned_hevc_surface) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (!full_surface && aligned_hevc_surface) {
            if (surf->width < 2 || surf->height < 2 ||
                (surf->width & 1u) != 0 || (surf->height & 1u) != 0) {
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_INVALID_SURFACE;
            }
            copy_w = (int)(surf->width < (unsigned int)enc_w ? surf->width : (unsigned int)enc_w);
            copy_h = (int)(surf->height < (unsigned int)enc_h ? surf->height : (unsigned int)enc_h);
        }

        const unsigned char *src_y = NULL;
        const unsigned char *src_uv = NULL;
        unsigned int src_y_stride = 0;
        unsigned int src_uv_stride = 0;
        if (surf->has_decoded_frame) {
            HobotDecodedNV12Layout src_layout;
            VAStatus layout_status = hobot_get_decoded_nv12_planes(
                surf, &src_layout);
            if (layout_status != VA_STATUS_SUCCESS) {
                pthread_mutex_unlock(&drv->mutex);
                return layout_status;
            }
            src_y = src_layout.y_plane;
            src_uv = src_layout.uv_plane;
            src_y_stride = src_layout.y_stride;
            src_uv_stride = src_layout.uv_stride;
        } else {
            if (!surf->raw_data_valid && surf->has_preallocated) {
                VAStatus copy_status = hobot_copy_gbuf_to_staging(surf);
                if (copy_status != VA_STATUS_SUCCESS) {
                    pthread_mutex_unlock(&drv->mutex);
                    return copy_status;
                }
            }
            if (surf->raw_data_valid) {
                src_y_stride = surf->stride > 0 ? (unsigned int)surf->stride : surf->width;
                src_uv_stride = src_y_stride;
                uint64_t src_y_size = (uint64_t)src_y_stride * surf->height;
                uint64_t src_uv_size = (uint64_t)src_uv_stride * (surf->height / 2u);
                if (!surf->raw_data || src_y_stride < surf->width ||
                    src_uv_stride < surf->width || (src_y_stride & 1u) != 0 ||
                    (src_uv_stride & 1u) != 0 ||
                    src_y_size > surf->raw_data_size ||
                    src_uv_size > surf->raw_data_size - src_y_size) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_OPERATION_FAILED;
                }
                src_y = surf->raw_data;
                src_uv = src_y + (size_t)src_y_size;
            }
        }

        if (hctx->encoder_init_deferred) {
            int start_ret = hctx->vpu_ctx.codec_id == MEDIA_CODEC_ID_H265 ?
                hobot_start_deferred_hevc_encoder(hctx) :
                hobot_start_deferred_h264_encoder(hctx);
            if (start_ret != 0) {
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
        }
        if (!hctx->vpu_running) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        coded->coded_segment.size = 0;
        coded->coded_segment.bit_offset = 0;
        coded->coded_segment.status = 0;
        coded->coded_segment.buf = coded->data;
        coded->coded_segment.next = NULL;

        media_codec_buffer_t in_buf;
        memset(&in_buf, 0, sizeof(in_buf));
        int ret = hb_mm_mc_dequeue_input_buffer(&hctx->vpu_ctx, &in_buf, 1000);
        if (ret != 0) {
            fprintf(stderr, "[HOBOT-VA] dequeue_in failed: ret=%d\n", ret);
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (!in_buf.vframe_buf.vir_ptr[0] || !in_buf.vframe_buf.vir_ptr[1]) {
            fprintf(stderr, "[HOBOT-VA] dequeue_in returned an invalid NV12 buffer\n");
            int recycle_ret = hobot_recycle_encoder_input(hctx, &in_buf);
            if (recycle_ret != 0) {
                fprintf(stderr, "[HOBOT-VA] invalid input buffer recycle failed: ret=%d\n", recycle_ret);
                hobot_finish_encoder_picture(hctx, 1);
            }
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        int dst_y_stride = in_buf.vframe_buf.stride > 0 ? in_buf.vframe_buf.stride : enc_w;
        /* The media-codec SDK names the chroma byte pitch vstride. */
        int dst_uv_stride = in_buf.vframe_buf.vstride > 0 ? in_buf.vframe_buf.vstride : dst_y_stride;
        uint64_t dst_y_size = (uint64_t)(unsigned int)dst_y_stride * (unsigned int)enc_h;
        uint64_t dst_uv_size = (uint64_t)(unsigned int)dst_uv_stride * ((unsigned int)enc_h / 2u);
        int invalid_input_layout = dst_y_stride < enc_w || dst_uv_stride < enc_w ||
                                  (dst_y_stride & 1) != 0 || (dst_uv_stride & 1) != 0 ||
                                  dst_y_size > in_buf.vframe_buf.size ||
                                  dst_uv_size > in_buf.vframe_buf.size - dst_y_size ||
                                  (in_buf.vframe_buf.width > 0 &&
                                   in_buf.vframe_buf.width < enc_w) ||
                                  (in_buf.vframe_buf.height > 0 &&
                                   in_buf.vframe_buf.height < enc_h) ||
                                  (in_buf.vframe_buf.compSize[0] > 0 &&
                                   dst_y_size > in_buf.vframe_buf.compSize[0]) ||
                                  (in_buf.vframe_buf.compSize[1] > 0 &&
                                   dst_uv_size > in_buf.vframe_buf.compSize[1]);
        if (invalid_input_layout) {
            fprintf(stderr,
                    "[HOBOT-VA] encoder input NV12 layout rejected: frame=%dx%d layout=%dx%d y_stride=%d uv_stride=%d size=%llu compSize=%llu/%llu required=%llu/%llu\n",
                    enc_w, enc_h,
                    (int)in_buf.vframe_buf.width, (int)in_buf.vframe_buf.height,
                    dst_y_stride, dst_uv_stride,
                    (unsigned long long)in_buf.vframe_buf.size,
                    (unsigned long long)in_buf.vframe_buf.compSize[0],
                    (unsigned long long)in_buf.vframe_buf.compSize[1],
                    (unsigned long long)dst_y_size,
                    (unsigned long long)dst_uv_size);
            int recycle_ret = hobot_recycle_encoder_input(hctx, &in_buf);
            if (recycle_ret != 0)
                fprintf(stderr, "[HOBOT-VA] invalid input layout recycle failed: ret=%d\n", recycle_ret);
            if (recycle_ret != 0)
                hobot_finish_encoder_picture(hctx, 1);
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        if (copy_w < 2 || copy_h < 2 || (copy_w & 1) != 0 ||
            (copy_h & 1) != 0 || (copy_x & 1) != 0 || (copy_y & 1) != 0 ||
            copy_x < 0 || copy_y < 0 || copy_x + copy_w > enc_w ||
            copy_y + copy_h > enc_h) {
            int recycle_ret = hobot_recycle_encoder_input(hctx, &in_buf);
            if (recycle_ret != 0)
                hobot_finish_encoder_picture(hctx, 1);
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }

        if (src_y && src_uv) {
            if (hobot_copy_nv12_to_coded_frame(
                    src_y, src_uv, src_y_stride, src_uv_stride,
                    in_buf.vframe_buf.vir_ptr[0],
                    in_buf.vframe_buf.vir_ptr[1], dst_y_stride, dst_uv_stride,
                    enc_w, enc_h, copy_x, copy_y, copy_w, copy_h) != 0) {
                int recycle_ret = hobot_recycle_encoder_input(hctx, &in_buf);
                if (recycle_ret != 0)
                    hobot_finish_encoder_picture(hctx, 1);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
        } else {
            memset(in_buf.vframe_buf.vir_ptr[0], 0x80, (size_t)dst_y_size);
            memset(in_buf.vframe_buf.vir_ptr[1], 0x80, (size_t)dst_uv_size);
        }

        uint64_t current_frame = hctx->frame_count;
        in_buf.vframe_buf.pts = 0;

        ret = hb_mm_mc_queue_input_buffer(&hctx->vpu_ctx, &in_buf, 1000);
        if (ret != 0) {
            fprintf(stderr, "[HOBOT-VA] queue_in failed: ret=%d\n", ret);
            int recycle_ret = hobot_recycle_encoder_input(hctx, &in_buf);
            if (recycle_ret != 0)
                hobot_finish_encoder_picture(hctx, 1);
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        hctx->frame_count++;

        media_codec_buffer_t out_buf;
        media_codec_output_buffer_info_t info;
        memset(&out_buf, 0, sizeof(out_buf));
        memset(&info, 0, sizeof(info));

        ret = hb_mm_mc_dequeue_output_buffer(&hctx->vpu_ctx, &out_buf, &info, 2000);
        if (ret == 0) {
            if (out_buf.vstream_buf.size == 0 || !out_buf.vstream_buf.vir_ptr) {
                fprintf(stderr, "[HOBOT-VA] dequeue_out returned an empty encoded buffer\n");
                int recycle_ret = hobot_recycle_encoder_output(hctx, &out_buf);
                if (recycle_ret != 0) {
                    fprintf(stderr, "[HOBOT-VA] empty output buffer recycle failed: ret=%d\n", recycle_ret);
                }
                hobot_finish_encoder_picture(hctx, recycle_ret != 0);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }

            size_t output_size = out_buf.vstream_buf.size;
            if (output_size > coded->size) {
                fprintf(stderr, "[HOBOT-VA] encoded output (%zu bytes) exceeds coded buffer capacity (%u bytes)\n",
                        output_size, coded->size);
                int recycle_ret = hobot_recycle_encoder_output(hctx, &out_buf);
                if (recycle_ret != 0) {
                    fprintf(stderr, "[HOBOT-VA] oversized output buffer recycle failed: ret=%d\n", recycle_ret);
                    hobot_finish_encoder_picture(hctx, 1);
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_OPERATION_FAILED;
                }
                hobot_finish_encoder_picture(hctx, 0);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
            }

            memcpy(coded->data, out_buf.vstream_buf.vir_ptr, output_size);
            int hevc_sps_found = 0;
            int hevc_sps_valid = 1;
            int h264_sps_found = 0;
            int h264_pps_found = 0;
            int h264_sps_valid = 1;
            if (hctx->profile == VAProfileHEVCMain) {
                int visible_geometry_differs =
                    hctx->hevc_encode_sequence_valid &&
                    (copy_w != (int)hctx->hevc_encode_sequence.pic_width_in_luma_samples ||
                     copy_h != (int)hctx->hevc_encode_sequence.pic_height_in_luma_samples);
                hevc_sps_valid = hctx->hevc_encode_sequence_valid;
                if (hevc_sps_valid && visible_geometry_differs)
                    hevc_sps_valid = hobot_hevc_patch_output_sps(
                        coded->data, &output_size, coded->capacity,
                        &hctx->hevc_encode_sequence,
                        (uint32_t)copy_w, (uint32_t)copy_h,
                        &hevc_sps_found);
                if (hevc_sps_valid && visible_geometry_differs && !hevc_sps_found &&
                    !hctx->hevc_sps_geometry_seen)
                    hevc_sps_valid = 0;
            }
            if (hctx->profile == VAProfileH264ConstrainedBaseline)
                h264_sps_valid = hobot_h264_patch_constrained_baseline_headers(
                    coded->data, &output_size, coded->capacity,
                    !hctx->h264_constrained_baseline_headers_patched,
                    &h264_sps_found, &h264_pps_found);
            if (hctx->profile == VAProfileJPEGBaseline)
                output_size = hobot_jpeg_strip_empty_app9(coded->data, output_size);
            int recycle_ret = hobot_recycle_encoder_output(hctx, &out_buf);
            if (recycle_ret != 0) {
                fprintf(stderr, "[HOBOT-VA] encoded output buffer recycle failed: ret=%d\n", recycle_ret);
                hobot_finish_encoder_picture(hctx, 1);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            if (!hevc_sps_valid) {
                fprintf(stderr, "[HOBOT-VA] HEVC output SPS geometry could not be verified or safely corrected\n");
                hobot_finish_encoder_picture(hctx, 1);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            if (hevc_sps_found)
                hctx->hevc_sps_geometry_seen = 1;
            if (!h264_sps_valid) {
                fprintf(stderr, "[HOBOT-VA] H.264 Constrained Baseline output parameter sets failed validation\n");
                hobot_finish_encoder_picture(hctx, 1);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            if (h264_sps_found && h264_pps_found)
                hctx->h264_constrained_baseline_headers_patched = 1;
            coded->coded_segment.size = (unsigned int)output_size;
            va_trace("vaEndPicture: frame %lu encoded successfully (%zu bytes)",
                     (unsigned long)current_frame, output_size);
        } else {
            fprintf(stderr, "[HOBOT-VA] vaEndPicture: dequeue_output failed frame %lu ret=%d size=%d\n",
                    (unsigned long)current_frame, ret, out_buf.vstream_buf.size);
            hobot_finish_encoder_picture(hctx, 1);
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        hobot_finish_encoder_picture(hctx, 0);
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_SUCCESS;
    }

    /* Decoder: queue assembled picture input buffer to VPU */
    if (!hctx->decode_picture_active) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (hctx->decode_slice_fragment_open) {
        fprintf(stderr, "[HOBOT-VA] vaEndPicture: incomplete H.264 slice fragment\n");
        hctx->headers_sent = 0;
        int recycle_ret = hobot_recycle_decoder_input(hctx);
        if (recycle_ret != 0) {
            fprintf(stderr, "[HOBOT-VA] incomplete-fragment input recycle failed: %d\n",
                    recycle_ret);
        }
        hobot_abort_pending_decode_picture(drv, hctx);
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (!hctx->dec_in_buf_valid || hctx->dec_in_buf_offset <= 0) {
        fprintf(stderr, "[HOBOT-VA] vaEndPicture: no decoder bitstream was assembled\n");
        hctx->headers_sent = 0;
        int recycle_ret = hobot_recycle_decoder_input(hctx);
        if (recycle_ret != 0) {
            fprintf(stderr, "[HOBOT-VA] empty-picture input recycle failed: %d\n", recycle_ret);
        }
        hobot_abort_pending_decode_picture(drv, hctx);
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (hctx->profile == VAProfileJPEGBaseline) {
        size_t input_capacity = hctx->dec_in_buf.vstream_buf.size;
        size_t configured_capacity = hctx->vpu_ctx.video_dec_params.bitstream_buf_size > 0 ?
            (size_t)hctx->vpu_ctx.video_dec_params.bitstream_buf_size : input_capacity;
        if (configured_capacity < input_capacity)
            input_capacity = configured_capacity;
        if (!hctx->dec_in_buf.vstream_buf.vir_ptr || hctx->dec_in_buf_offset < 0 ||
            (size_t)hctx->dec_in_buf_offset > input_capacity ||
            input_capacity - (size_t)hctx->dec_in_buf_offset < 2u) {
            fprintf(stderr, "[HOBOT-VA] JPEG input buffer has no room for EOI\n");
            int recycle_ret = hobot_recycle_decoder_input(hctx);
            if (recycle_ret != 0) {
                fprintf(stderr, "[HOBOT-VA] JPEG EOI buffer recycle failed: %d\n", recycle_ret);
                hctx->decode_failed = 1;
            }
            hobot_abort_pending_decode_picture(drv, hctx);
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
        }
        uint8_t *dst = (uint8_t *)hctx->dec_in_buf.vstream_buf.vir_ptr;
        dst[hctx->dec_in_buf_offset++] = 0xff;
        dst[hctx->dec_in_buf_offset++] = 0xd9;
    }

    if (hctx->dec_in_buf_valid && hctx->dec_in_buf_offset > 0) {
        hctx->dec_in_buf.vstream_buf.size = hctx->dec_in_buf_offset;
        hctx->dec_in_buf.vstream_buf.pts = (uint64_t)hctx->frame_count;
        int qret = hb_mm_mc_queue_input_buffer(&hctx->vpu_ctx, &hctx->dec_in_buf, 100);
        if (qret != 0) {
            fprintf(stderr, "[HOBOT-VA] vaEndPicture: queue_input_buffer failed: %d\n", qret);
            /* Queue ownership is undocumented on error: retain the dequeued buffer
             * and poison this decoder until teardown rather than risk reuse/double-queue. */
            hctx->decode_failed = 1;
            hobot_abort_pending_decode_picture(drv, hctx);
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        hctx->dec_in_buf_valid = 0;
        hctx->dec_in_buf_offset = 0;
    }

    hctx->current_render_target = VA_INVALID_SURFACE;
    hctx->decode_picture_active = 0;
    hctx->decode_slice_fragment_open = 0;
    hobot_signal_sync_waiters(drv);

    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaSyncSurfaceLocked(HobotDriverData *drv,
                                           VASurfaceID render_target) {
    va_trace("vaSyncSurface: render_target=%u", render_target);
    if (!drv) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (render_target <= 0 || render_target >= MAX_SURFACES) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (!drv->surfaces[render_target].allocated) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    HobotSurface *surf = &drv->surfaces[render_target];
    if (surf->lock_count > 0 && surf->decode_pending) {
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (surf->decode_pending && hobot_ensure_sync_condition(drv) != 0) {
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    VAContextID cid;
    HobotContext *hctx;
    for (;;) {
        if (!surf->allocated) {
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        if (surf->decode_error) {
            return VA_STATUS_ERROR_DECODING_ERROR;
        }
        if (!surf->decode_pending) {
            return VA_STATUS_SUCCESS;
        }

        cid = surf->context_id;
        if (cid <= 0 || cid >= MAX_CONTEXTS ||
            !drv->contexts[cid].allocated || !drv->contexts[cid].vpu_running ||
            drv->contexts[cid].is_encoder) {
            va_trace("vaSyncSurface: no active owner context for surface=%u (context=%u)",
                     render_target, cid);
            return VA_STATUS_ERROR_INVALID_CONTEXT;
        }
        hctx = &drv->contexts[cid];
        if (hctx->sync_active) {
            if (pthread_cond_wait(&drv->sync_cond, &drv->mutex) != 0) {
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            continue;
        }
        if (hctx->decode_failed || hctx->dec_out_buf_valid) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (hctx->sync_active || hctx->decode_picture_active) {
            if (pthread_cond_wait(&drv->sync_cond, &drv->mutex) != 0) {
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            continue;
        }
        break;
    }
    hctx->sync_active = 1;

    media_codec_context_t *mctx = &hctx->vpu_ctx;
    VAStatus sync_error = VA_STATUS_SUCCESS;

    int max_attempts = 30;
    while (!surf->has_decoded_frame && !surf->decode_error && max_attempts-- > 0) {
        VASurfaceID target = hctx->sub_head != hctx->sub_tail ?
                             hctx->submitted_surfaces[hctx->sub_head % 128] : render_target;
        if (target <= 0 || target >= MAX_SURFACES || !drv->surfaces[target].allocated) {
            hctx->decode_failed = 1;
            fprintf(stderr, "[HOBOT-VA] vaSyncSurface: invalid queued target %u\n", target);
            return hobot_finish_surface_sync(drv, hctx, VA_STATUS_ERROR_OPERATION_FAILED);
        }
        HobotSurface *target_surface = &drv->surfaces[target];
        if (target_surface->lock_count > 0) {
            return hobot_finish_surface_sync(drv, hctx,
                                             VA_STATUS_ERROR_SURFACE_BUSY);
        }
        if (target_surface->has_decoded_frame) {
            VAContextID owner = target_surface->output_context_id;
            if (owner <= 0 || owner >= MAX_CONTEXTS || !drv->contexts[owner].allocated ||
                !drv->contexts[owner].vpu_running || drv->contexts[owner].is_encoder ||
                (owner != cid &&
                 (drv->contexts[owner].sync_active ||
                  drv->contexts[owner].decode_picture_active))) {
                return hobot_finish_surface_sync(drv, hctx, VA_STATUS_ERROR_INVALID_CONTEXT);
            }
            if (target_surface->vpu_out_buf.vframe_buf.phy_ptr[0] != 0 &&
                target_surface->vpu_out_buf.vframe_buf.size > 0) {
                int qret = hb_mm_mc_queue_output_buffer(&drv->contexts[owner].vpu_ctx,
                                                        &target_surface->vpu_out_buf, 50);
                if (qret != 0) {
                    qret = hb_mm_mc_queue_output_buffer(&drv->contexts[owner].vpu_ctx,
                                                        &target_surface->vpu_out_buf, 200);
                }
                if (qret != 0) {
                    fprintf(stderr, "[HOBOT-VA] vaSyncSurface: queued target recycle failed: %d\n", qret);
                    return hobot_finish_surface_sync(drv, hctx, VA_STATUS_ERROR_OPERATION_FAILED);
                }
            }
            target_surface->has_decoded_frame = 0;
            target_surface->dma_fd = -1;
            target_surface->output_context_id = 0;
            memset(&target_surface->vpu_out_buf, 0, sizeof(target_surface->vpu_out_buf));
        }

        media_codec_output_buffer_info_t out_info;
        media_codec_buffer_t out_buf;
        memset(&out_info, 0, sizeof(out_info));
        memset(&out_buf, 0, sizeof(out_buf));

        pthread_mutex_unlock(&drv->mutex);
        int ret = hb_mm_mc_dequeue_output_buffer(mctx, &out_buf, &out_info, 50);
        pthread_mutex_lock(&drv->mutex);
        if (!hctx->allocated || !hctx->vpu_running || !hctx->sync_active ||
            !surf->allocated || surf->context_id != cid || !surf->decode_pending) {
            hctx->decode_failed = 1;
            if (surf->allocated && surf->decode_pending)
                surf->decode_error = 1;
            return hobot_finish_surface_sync(drv, hctx, VA_STATUS_ERROR_OPERATION_FAILED);
        }
        if (ret == 0) {
            /* Empty or invalid buffers can be emitted during decoder renegotiation. */
            if (out_buf.vframe_buf.phy_ptr[0] == 0 ||
                out_buf.vframe_buf.size == 0 ||
                out_buf.vframe_buf.vir_ptr[0] == NULL ||
                out_buf.vframe_buf.fd[0] < 0) {
                va_trace("vaSyncSurface: dropped invalid/hollow out_buf (phy=0x%llx, size=%u, vir=%p, fd=%d)",
                         (unsigned long long)out_buf.vframe_buf.phy_ptr[0],
                         out_buf.vframe_buf.size,
                         out_buf.vframe_buf.vir_ptr[0],
                         out_buf.vframe_buf.fd[0]);
                /* If buffer has valid physical address, it was dequeued from VPU output pool;
                 * recycle it back to prevent pool starvation (Codex finding 1). */
                if (out_buf.vframe_buf.phy_ptr[0] != 0 && out_buf.vframe_buf.size > 0) {
                    int qret = hobot_recycle_decoder_output(hctx, &out_buf);
                    if (qret != 0) {
                        fprintf(stderr, "[HOBOT-VA] vaSyncSurface: recycle invalid buffer failed: %d\n", qret);
                        return hobot_finish_surface_sync(drv, hctx, VA_STATUS_ERROR_OPERATION_FAILED);
                    }
                }
                continue;
            }

            if (hctx->sub_head != hctx->sub_tail) {
                hctx->sub_head++;
            }

            int is_jpeg = hctx->profile == VAProfileJPEGBaseline;
            uint32_t err_reason = 0;
            int err_mb = 0;
            int total_mb = 0;
            int disp_idx;
            int deco_idx;
            if (is_jpeg) {
                const mc_mjpeg_jpeg_output_frame_info_t *jpeg_info =
                    &out_info.jpeg_frame_info;
                disp_idx = jpeg_info->frame_display_index;
                deco_idx = -1;
                if (jpeg_info->decode_result != 1 ||
                    jpeg_info->display_width != hctx->width ||
                    jpeg_info->display_height != hctx->height) {
                    fprintf(stderr,
                            "[HOBOT-VA] JPEG decode failed or dimensions changed: result=%d display=%dx%d expected=%dx%d\n",
                            jpeg_info->decode_result, jpeg_info->display_width,
                            jpeg_info->display_height, hctx->width, hctx->height);
                    if (target > 0 && target < MAX_SURFACES &&
                        drv->surfaces[target].allocated &&
                        drv->surfaces[target].decode_pending) {
                        drv->surfaces[target].decode_pending = 0;
                        drv->surfaces[target].decode_error = 1;
                    }
                    int qret = hobot_recycle_decoder_output(hctx, &out_buf);
                    if (qret != 0) {
                        fprintf(stderr, "[HOBOT-VA] JPEG failed output recycle failed: %d\n", qret);
                        hctx->decode_failed = 1;
                        return hobot_finish_surface_sync(
                            drv, hctx, VA_STATUS_ERROR_OPERATION_FAILED);
                    }
                    continue;
                }
            } else {
                err_reason = (uint32_t)out_info.video_frame_info.error_reason;
                err_mb = out_info.video_frame_info.err_mb_in_frame_display;
                total_mb = out_info.video_frame_info.total_mb_in_frame_display;
                disp_idx = out_info.video_frame_info.frame_display_index;
                deco_idx = out_info.video_frame_info.frame_decoded_index;
            }

            if (!is_jpeg && (err_reason != 0 || err_mb > 0)) {
                struct timespec now_ts;
                clock_gettime(CLOCK_MONOTONIC, &now_ts);
                if (hctx->last_anomaly_sec > 0 &&
                    (now_ts.tv_sec - hctx->last_anomaly_sec) >= HOBOT_WATCHDOG_CLEAN_RESET_SEC) {
                    fprintf(stderr, "[HOBOT-VA][WATCHDOG] 10s clean period elapsed. Anomaly count reset from %d to 0.\n",
                            hctx->watchdog_anomaly_count);
                    hctx->watchdog_anomaly_count = 0;
                }
                hctx->last_anomaly_sec = now_ts.tv_sec;
                hctx->watchdog_anomaly_count++;

                hctx->watchdog_trace_countdown = 20;
                fprintf(stderr, "\n[HOBOT-VA][WATCHDOG] >>> VPU ANOMALY DETECTED (#%d/%d) <<<\n"
                                "[HOBOT-VA][WATCHDOG] target_surf=%u disp_idx=%d deco_idx=%d\n"
                                "[HOBOT-VA][WATCHDOG] error_reason=0x%08x warn_info=0x%08x\n"
                                "[HOBOT-VA][WATCHDOG] err_mb=%d total_mb=%d (%.1f%% corrupted)\n"
                                "[HOBOT-VA][WATCHDOG] phy=[0x%llx, 0x%llx] fd=%d stride=%d size=%u (%dx%d)\n\n",
                        hctx->watchdog_anomaly_count, HOBOT_WATCHDOG_FALLBACK_THRESHOLD,
                        target, disp_idx, deco_idx,
                        err_reason, (uint32_t)out_info.video_frame_info.warn_info,
                        err_mb, total_mb,
                        total_mb > 0 ? ((double)err_mb * 100.0 / total_mb) : 0.0,
                        (unsigned long long)out_buf.vframe_buf.phy_ptr[0],
                        (unsigned long long)out_buf.vframe_buf.phy_ptr[1],
                        out_buf.vframe_buf.fd[0],
                        out_buf.vframe_buf.stride,
                        out_buf.vframe_buf.size,
                        out_buf.vframe_buf.width,
                        out_buf.vframe_buf.height);

                hobot_publish_watchdog_state(hctx->watchdog_anomaly_count, err_mb, total_mb);
            } else if (!is_jpeg && hctx->watchdog_anomaly_count > 0) {
                struct timespec now_ts;
                clock_gettime(CLOCK_MONOTONIC, &now_ts);
                if (hctx->last_anomaly_sec > 0 &&
                    (now_ts.tv_sec - hctx->last_anomaly_sec) >= HOBOT_WATCHDOG_CLEAN_RESET_SEC) {
                    fprintf(stderr, "[HOBOT-VA][WATCHDOG] 10s clean playback elapsed. Resetting anomaly count (%d -> 0).\n",
                            hctx->watchdog_anomaly_count);
                    hctx->watchdog_anomaly_count = 0;
                }
            } else if (!is_jpeg && hctx->watchdog_trace_countdown > 0) {
                hctx->watchdog_trace_countdown--;
                fprintf(stderr, "[HOBOT-VA][POST-WD #%02d] target_surf=%u disp_idx=%d deco_idx=%d err_reason=0x%08x err_mb=%d/%d phy=[0x%llx, 0x%llx] fd=%d size=%u\n",
                        20 - hctx->watchdog_trace_countdown,
                        target, disp_idx, deco_idx,
                        err_reason, err_mb, total_mb,
                        (unsigned long long)out_buf.vframe_buf.phy_ptr[0],
                        (unsigned long long)out_buf.vframe_buf.phy_ptr[1],
                        out_buf.vframe_buf.fd[0],
                        out_buf.vframe_buf.size);
            }

            /* Phase A: Frame Drop Experiment (Discard corrupted frames) */
            if (!is_jpeg && (err_mb > 0 || (err_reason & 0x00020000))) {
                fprintf(stderr, "[HOBOT-VA][DROP] Dropping corrupted frame (err_mb=%d/%d, err_reason=0x%08x) for target=%u (anomaly=%d/%d)\n",
                        err_mb, total_mb, err_reason, target, hctx->watchdog_anomaly_count,
                        HOBOT_WATCHDOG_FALLBACK_THRESHOLD);
                if (target > 0 && target < MAX_SURFACES &&
                    drv->surfaces[target].allocated && drv->surfaces[target].decode_pending) {
                    drv->surfaces[target].decode_pending = 0;
                    drv->surfaces[target].decode_error = 1;
                }
                if (out_buf.vframe_buf.phy_ptr[0] != 0 && out_buf.vframe_buf.size > 0) {
                    int qret = hobot_recycle_decoder_output(hctx, &out_buf);
                    if (qret != 0) {
                        fprintf(stderr, "[HOBOT-VA] drop frame: queue_output_buffer failed: %d\n", qret);
                        return hobot_finish_surface_sync(drv, hctx, VA_STATUS_ERROR_OPERATION_FAILED);
                    }
                }
                continue;
            }

            if (target > 0 && target < MAX_SURFACES && drv->surfaces[target].allocated) {
                HobotSurface *tsurf = &drv->surfaces[target];
                tsurf->vpu_out_buf = out_buf;
                tsurf->has_decoded_frame = 1;
                tsurf->raw_data_valid = 0;
                tsurf->raw_data_dirty = 0;
                tsurf->output_context_id = cid;
                tsurf->decode_pending = 0;
                tsurf->decode_error = 0;
                tsurf->dma_fd = out_buf.vframe_buf.fd[0];
                tsurf->stride = out_buf.vframe_buf.stride;
            }
        } else if (ret == HB_MEDIA_ERR_WAIT_TIMEOUT) {
            continue;
        } else {
            fprintf(stderr, "[HOBOT-VA] vaSyncSurface: dequeue_output_buffer error %d\n", ret);
            hctx->decode_failed = 1;
            sync_error = VA_STATUS_ERROR_OPERATION_FAILED;
            if (surf->decode_pending)
                surf->decode_error = 1;
            uint32_t pending_count = hctx->sub_tail - hctx->sub_head;
            for (uint32_t i = 0; i < pending_count; i++) {
                VASurfaceID pending_id = hctx->submitted_surfaces[(hctx->sub_head + i) % 128];
                if (pending_id > 0 && pending_id < MAX_SURFACES &&
                    drv->surfaces[pending_id].allocated &&
                    drv->surfaces[pending_id].context_id == cid &&
                    drv->surfaces[pending_id].decode_pending) {
                    drv->surfaces[pending_id].decode_error = 1;
                }
            }
            break;
        }
    }

    if (surf->has_decoded_frame && surf->dma_fd >= 0) {
        return hobot_finish_surface_sync(drv, hctx, VA_STATUS_SUCCESS);
    }
    if (sync_error != VA_STATUS_SUCCESS) {
        return hobot_finish_surface_sync(drv, hctx, sync_error);
    }
    if (surf->decode_error) {
        return hobot_finish_surface_sync(drv, hctx, VA_STATUS_ERROR_DECODING_ERROR);
    }
    if (hctx->decode_failed) {
        return hobot_finish_surface_sync(drv, hctx, VA_STATUS_ERROR_OPERATION_FAILED);
    }

    return hobot_finish_surface_sync(drv, hctx, VA_STATUS_ERROR_TIMEDOUT);
}

static VAStatus hobot_vaSyncSurface(VADriverContextP ctx, VASurfaceID render_target) {
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    VAStatus status = hobot_vaSyncSurfaceLocked(drv, render_target);
    pthread_mutex_unlock(&drv->mutex);
    return status;
}

static VAStatus hobot_fill_surface_info_locked(
    HobotDriverData *drv,
    VASurfaceID surface,
    struct hobot_surface_info *info
) {
    if (!drv || !info) return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (surface <= 0 || surface >= MAX_SURFACES || !drv->surfaces[surface].allocated) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    HobotSurface *initial_surface = &drv->surfaces[surface];
    int needs_sync = initial_surface->decode_pending;

    VAStatus sync_status = VA_STATUS_SUCCESS;
    if (needs_sync) {
        sync_status = hobot_vaSyncSurfaceLocked(drv, surface);
    }

    if (surface <= 0 || surface >= MAX_SURFACES || !drv->surfaces[surface].allocated) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    HobotSurface *surf = &drv->surfaces[surface];

    if (surf->decode_error) {
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    if (!surf->has_decoded_frame && surf->decode_pending) {
        return sync_status == VA_STATUS_SUCCESS ? VA_STATUS_ERROR_TIMEDOUT : sync_status;
    }

    if (!surf->has_decoded_frame && !surf->has_preallocated) {
        if (needs_sync && sync_status == VA_STATUS_SUCCESS) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        unsigned int aligned_w = (surf->width + 63u) & ~63u;
        unsigned int aligned_h = (surf->height + 63u) & ~63u;
        int64_t mflags = HB_MEM_USAGE_CPU_READ_OFTEN |
                         HB_MEM_USAGE_CPU_WRITE_OFTEN |
                         HB_MEM_USAGE_HW_VIDEO_CODEC |
                         HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;
        hb_mem_graphic_buf_t gbuf;
        memset(&gbuf, 0, sizeof(gbuf));
        for (size_t i = 0; i < MAX_GRAPHIC_BUF_COMP; i++) {
            gbuf.fd[i] = -1;
        }
        int gret = hb_mem_alloc_graph_buf((int32_t)surf->width, (int32_t)surf->height,
                                          MEM_PIX_FMT_NV12, mflags,
                                          (int32_t)aligned_w, (int32_t)aligned_h, &gbuf);
        if (gret != 0 || gbuf.fd[0] < 0) {
            va_trace("hobot_fill_surface_info: lazy graph buffer allocation failed surf=%u ret=%d",
                     surface, gret);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        surf->dma_fd = gbuf.fd[0];
        surf->stride = gbuf.stride;
        surf->preallocated_gbuf = gbuf;
        surf->has_preallocated = 1;
        va_trace("hobot_fill_surface_info: lazy graph buffer allocated surf=%u fd=%d stride=%d",
                 surface, gbuf.fd[0], gbuf.stride);
    }

    HobotPreallocatedNV12Layout preallocated_layout = {0};
    int has_preallocated_layout = 0;
    if (!surf->has_decoded_frame && surf->has_preallocated) {
        VAStatus layout_status = hobot_get_preallocated_nv12_layout(
            surf, &preallocated_layout);
        if (layout_status != VA_STATUS_SUCCESS)
            return layout_status;
        has_preallocated_layout = 1;
    }

    if (!surf->has_decoded_frame && surf->raw_data_dirty) {
        VAStatus upload_status = hobot_upload_staging_to_gbuf(surf);
        if (upload_status != VA_STATUS_SUCCESS) {
            return upload_status;
        }
    }

    memset(info, 0, sizeof(*info));
    info->width = surf->width;
    info->height = surf->height;
    info->stride = surf->stride > 0 ? (uint32_t)surf->stride : surf->width;
    info->dma_fd = surf->dma_fd;

    HobotDecodedNV12Layout decoded_layout = {0};
    if (surf->has_decoded_frame) {
        info->phys_addr[0] = surf->vpu_out_buf.vframe_buf.phy_ptr[0];
        info->phys_addr[1] = surf->vpu_out_buf.vframe_buf.phy_ptr[1];
        VAStatus layout_status = hobot_get_decoded_nv12_planes(
            surf, &decoded_layout);
        if (layout_status != VA_STATUS_SUCCESS)
            return layout_status;
        uint64_t represented_uv_offset =
            (uint64_t)decoded_layout.y_stride * decoded_layout.vertical_rows;
        if (decoded_layout.y_stride != decoded_layout.uv_stride ||
            decoded_layout.vertical_rows == 0 ||
            (decoded_layout.contiguous &&
             represented_uv_offset != decoded_layout.uv_offset) ||
            decoded_layout.uv_offset > UINT32_MAX)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        info->stride = decoded_layout.y_stride;
        info->vstride = decoded_layout.vertical_rows;
        info->virt_addr[0] = (void *)decoded_layout.y_plane;
        info->virt_addr[1] = (void *)decoded_layout.uv_plane;
        if (surf->vpu_out_buf.vframe_buf.fd[0] >= 0) {
            info->dma_fd = surf->vpu_out_buf.vframe_buf.fd[0];
        }
    } else if (!surf->has_decoded_frame && surf->has_preallocated) {
        info->stride = preallocated_layout.stride;
        info->vstride = preallocated_layout.vstride;
        info->phys_addr[0] = surf->preallocated_gbuf.phys_addr[0];
        info->phys_addr[1] = surf->preallocated_gbuf.phys_addr[1];
        info->virt_addr[0] = (void *)surf->preallocated_gbuf.virt_addr[0];
        info->virt_addr[1] = (void *)surf->preallocated_gbuf.virt_addr[1];
        if (surf->preallocated_gbuf.fd[0] >= 0) {
            info->dma_fd = surf->preallocated_gbuf.fd[0];
        }
    }

    uint64_t decoded_uv_offset = surf->has_decoded_frame ?
                                 decoded_layout.uv_offset :
                                 (uint64_t)info->stride * info->vstride;
    uint64_t preallocated_uv_delta = has_preallocated_layout &&
        !preallocated_layout.separate_fds ?
        preallocated_layout.uv_offset - preallocated_layout.y_offset : 0;
    if (info->phys_addr[1] == 0 && info->phys_addr[0] != 0) {
        uint64_t uv_delta = 0;
        if (surf->has_decoded_frame) {
            uintptr_t y_address = (uintptr_t)info->virt_addr[0];
            uintptr_t uv_address = (uintptr_t)info->virt_addr[1];
            int separate_fd = surf->vpu_out_buf.vframe_buf.fd[1] != 0 &&
                              surf->vpu_out_buf.vframe_buf.fd[1] !=
                                  surf->vpu_out_buf.vframe_buf.fd[0];
            if (!decoded_layout.contiguous ||
                !info->virt_addr[0] || !info->virt_addr[1] ||
                uv_address < y_address || separate_fd ||
                (uint64_t)(uv_address - y_address) != decoded_uv_offset) {
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            uv_delta = decoded_uv_offset;
        } else if (has_preallocated_layout) {
            if (preallocated_layout.separate_fds ||
                !surf->preallocated_gbuf.is_contig)
                return VA_STATUS_ERROR_OPERATION_FAILED;
            uv_delta = preallocated_uv_delta;
        }
        if (UINT64_MAX - info->phys_addr[0] < uv_delta)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        info->phys_addr[1] = info->phys_addr[0] + uv_delta;
    }
    if (info->virt_addr[1] == NULL && info->virt_addr[0] != NULL) {
        uint64_t uv_delta = 0;
        if (surf->has_decoded_frame) {
            int separate_fd = surf->vpu_out_buf.vframe_buf.fd[1] != 0 &&
                              surf->vpu_out_buf.vframe_buf.fd[1] !=
                                  surf->vpu_out_buf.vframe_buf.fd[0];
            if (separate_fd)
                return VA_STATUS_ERROR_OPERATION_FAILED;
            uv_delta = decoded_uv_offset;
        } else if (has_preallocated_layout) {
            if (preallocated_layout.separate_fds ||
                !surf->preallocated_gbuf.is_contig)
                return VA_STATUS_ERROR_OPERATION_FAILED;
            uv_delta = preallocated_uv_delta;
        }
        uintptr_t y_address = (uintptr_t)info->virt_addr[0];
        if (uv_delta > UINTPTR_MAX - y_address)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        info->virt_addr[1] = (void *)(y_address + (uintptr_t)uv_delta);
    }

    va_trace("hobot_fill_surface_info: surf=%u, %ux%u, stride=%u, vstride=%u, phys=[0x%lx, 0x%lx], virt=[%p, %p], fd=%d",
             surface, info->width, info->height, info->stride, info->vstride,
             (unsigned long)info->phys_addr[0], (unsigned long)info->phys_addr[1],
             info->virt_addr[0], info->virt_addr[1], info->dma_fd);

    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_fill_surface_info(
    VADriverContextP ctx,
    VASurfaceID surface,
    struct hobot_surface_info *info
) {
    if (!ctx || !ctx->pDriverData || !info) return VA_STATUS_ERROR_INVALID_PARAMETER;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    VAStatus status = hobot_fill_surface_info_locked(drv, surface, info);
    pthread_mutex_unlock(&drv->mutex);
    return status;
}

/* Zero-Copy Export Surface Handle for mpv, Chromium (DRM PRIME 2) and DirectVIV */
static VAStatus hobot_vaExportSurfaceHandle(
    VADriverContextP ctx,
    VASurfaceID surface_id,
    uint32_t mem_type,
    uint32_t flags,
    void *descriptor
) {
    va_trace("vaExportSurfaceHandle: surface=%u, mem_type=0x%x, flags=0x%x, desc=%p",
             surface_id, mem_type, flags, descriptor);
    if (!descriptor) return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (mem_type == VA_SURFACE_ATTRIB_MEM_TYPE_HOBOT_GRAPH_BUF) {
        return hobot_fill_surface_info(ctx, surface_id, (struct hobot_surface_info *)descriptor);
    } else if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
        va_trace("vaExportSurfaceHandle: unsupported mem_type=0x%x (only DRM_PRIME_2 supported)", mem_type);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    const uint32_t layer_flags = flags & (VA_EXPORT_SURFACE_SEPARATE_LAYERS |
                                          VA_EXPORT_SURFACE_COMPOSED_LAYERS);
    const uint32_t known_flags = VA_EXPORT_SURFACE_READ_WRITE |
                                 VA_EXPORT_SURFACE_SEPARATE_LAYERS |
                                 VA_EXPORT_SURFACE_COMPOSED_LAYERS;
    if ((flags & ~known_flags) != 0 ||
        layer_flags == (VA_EXPORT_SURFACE_SEPARATE_LAYERS |
                        VA_EXPORT_SURFACE_COMPOSED_LAYERS)) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;

    pthread_mutex_lock(&drv->mutex);
    if (surface_id <= 0 || surface_id >= MAX_SURFACES || !drv->surfaces[surface_id].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        va_trace("vaExportSurfaceHandle: surface=%u NOT ALLOCATED", surface_id);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    HobotSurface *surf = &drv->surfaces[surface_id];
    if (surf->decode_pending) {
        va_trace("vaExportSurfaceHandle: surface=%u syncing for decoded frame...", surface_id);
        VAStatus sync_status = hobot_vaSyncSurfaceLocked(drv, surface_id);
        if (sync_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return sync_status;
        }
    }
    if (!surf->allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (surf->decode_error) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    if (!surf->has_decoded_frame && !surf->has_preallocated) {
        struct hobot_surface_info info;
        VAStatus prepare_status = hobot_fill_surface_info_locked(drv, surface_id, &info);
        if (prepare_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return prepare_status;
        }
    }

    if (!surf->has_decoded_frame && surf->raw_data_dirty) {
        VAStatus upload_status = hobot_upload_staging_to_gbuf(surf);
        if (upload_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return upload_status;
        }
    }
    if (surf->decode_error) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    if (surf->decode_pending && !surf->has_decoded_frame) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_TIMEDOUT;
    }
    if ((!surf->has_decoded_frame && !surf->has_preallocated) || surf->dma_fd < 0) {
        va_trace("vaExportSurfaceHandle: surface=%u dma_fd < 0 (has_frame=%d, preallocated=%d) -> FAIL",
                 surface_id, surf->has_decoded_frame, surf->has_preallocated);
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    HobotPreallocatedNV12Layout preallocated_layout = {0};
    int has_preallocated_layout = 0;
    if (!surf->has_decoded_frame && surf->has_preallocated) {
        VAStatus layout_status = hobot_get_preallocated_nv12_layout(
            surf, &preallocated_layout);
        if (layout_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return layout_status;
        }
        has_preallocated_layout = 1;
    }

    uint32_t pitch = surf->stride > 0 ? (uint32_t)surf->stride : surf->width;
    uint32_t uv_pitch = pitch;
    uint32_t vstride = surf->has_preallocated && surf->preallocated_gbuf.vstride > 0 ?
                       (uint32_t)surf->preallocated_gbuf.vstride :
                       ((surf->height + 63u) & ~63u);
    uint32_t uv_offset = 0;
    uint32_t y_offset = 0;
    uint32_t uv_object_index = 0;
    uint32_t num_objects = 1;
    int object_fds[2] = {surf->dma_fd, -1};
    uint64_t object_sizes[2] = {0, 0};
    uint64_t y_plane_size = (uint64_t)pitch * vstride;
    uint64_t uv_plane_size = (uint64_t)pitch * ((vstride + 1u) / 2u);
    if (has_preallocated_layout) {
        pitch = preallocated_layout.stride;
        uv_pitch = preallocated_layout.stride;
        vstride = preallocated_layout.vstride;
        y_plane_size = preallocated_layout.y_extent;
        uv_plane_size = preallocated_layout.uv_extent;
    }

    if (surf->has_decoded_frame) {
        /* Reject unverified multi-FD layout from decoder to prevent memory corruption.
         * In SDK mc_video_frame_buffer_info, fd[3] default is 0. Any non-zero fd[1] different from
         * fd[0] indicates an unverified multi-FD layout. */
        if (surf->vpu_out_buf.vframe_buf.fd[1] != 0 &&
            surf->vpu_out_buf.vframe_buf.fd[1] != surf->vpu_out_buf.vframe_buf.fd[0]) {
            va_trace("vaExportSurfaceHandle: unverified multi-FD decoded frame (fd0=%d, fd1=%d)",
                     surf->vpu_out_buf.vframe_buf.fd[0], surf->vpu_out_buf.vframe_buf.fd[1]);
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        }

        HobotDecodedNV12Layout decoded_layout;
        VAStatus layout_status = hobot_get_decoded_nv12_planes(surf, &decoded_layout);
        if (layout_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return layout_status;
        }
        if (!decoded_layout.contiguous) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        }
        pitch = decoded_layout.y_stride;
        uv_pitch = decoded_layout.uv_stride;
        y_plane_size = (uint64_t)pitch * surf->height;
        uv_plane_size = (uint64_t)uv_pitch * ((surf->height + 1u) / 2u);

        /* Verify contiguous DMA allocation metadata:
         * 1) vir_ptr[0] and vir_ptr[1] must be valid with vir_ptr[1] > vir_ptr[0].
         * 2) phy_ptr[0] and phy_ptr[1] must be valid with phy_ptr[1] > phy_ptr[0].
         * 3) Virtual offset MUST match physical offset to prove same contiguous DMA mapping. */
        if (!surf->vpu_out_buf.vframe_buf.vir_ptr[0] ||
            !surf->vpu_out_buf.vframe_buf.vir_ptr[1] ||
            (uintptr_t)surf->vpu_out_buf.vframe_buf.vir_ptr[1] <=
            (uintptr_t)surf->vpu_out_buf.vframe_buf.vir_ptr[0] ||
            (uintptr_t)surf->vpu_out_buf.vframe_buf.vir_ptr[1] -
            (uintptr_t)surf->vpu_out_buf.vframe_buf.vir_ptr[0] > UINT32_MAX ||
            surf->vpu_out_buf.vframe_buf.phy_ptr[0] == 0 ||
            surf->vpu_out_buf.vframe_buf.phy_ptr[1] <= surf->vpu_out_buf.vframe_buf.phy_ptr[0] ||
            ((uint64_t)surf->vpu_out_buf.vframe_buf.phy_ptr[1] - (uint64_t)surf->vpu_out_buf.vframe_buf.phy_ptr[0]) !=
            ((uint64_t)(uintptr_t)surf->vpu_out_buf.vframe_buf.vir_ptr[1] - (uint64_t)(uintptr_t)surf->vpu_out_buf.vframe_buf.vir_ptr[0])) {
            va_trace("vaExportSurfaceHandle: cannot prove contiguous DMA UV offset for decoded frame");
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        uv_offset = (uint32_t)((uintptr_t)surf->vpu_out_buf.vframe_buf.vir_ptr[1] -
                               (uintptr_t)surf->vpu_out_buf.vframe_buf.vir_ptr[0]);
    } else if (!surf->has_decoded_frame && surf->has_preallocated) {
        hb_mem_graphic_buf_t *gbuf = &surf->preallocated_gbuf;
        if (!has_preallocated_layout ||
            preallocated_layout.y_offset > UINT32_MAX ||
            preallocated_layout.uv_offset > UINT32_MAX) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        y_offset = (uint32_t)preallocated_layout.y_offset;
        uv_offset = (uint32_t)preallocated_layout.uv_offset;
        if (preallocated_layout.separate_fds) {
            num_objects = 2;
            uv_object_index = 1;
            object_fds[1] = gbuf->fd[1];
        }
        object_sizes[0] = preallocated_layout.object_size[0];
        if (num_objects == 2) {
            object_sizes[1] = preallocated_layout.object_size[1];
        }
    } else {
        uv_offset = (uint32_t)y_plane_size;
    }

    if (surf->has_decoded_frame) {
        object_sizes[0] = surf->vpu_out_buf.vframe_buf.size > 0 ?
                          surf->vpu_out_buf.vframe_buf.size : y_plane_size + uv_plane_size;
    }
    if (object_sizes[0] == 0 || object_sizes[0] > UINT32_MAX || object_sizes[1] > UINT32_MAX) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    int exp_fds[2] = {-1, -1};
    exp_fds[0] = dup(object_fds[0]);
    if (exp_fds[0] < 0) {
        va_trace("vaExportSurfaceHandle: dup(dma_fd=%d) failed", object_fds[0]);
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (num_objects == 2) {
        exp_fds[1] = dup(object_fds[1]);
        if (exp_fds[1] < 0) {
            close(exp_fds[0]);
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    uint64_t phys_addr = 0;
    if (surf->has_decoded_frame && surf->vpu_out_buf.vframe_buf.phy_ptr[0] > 0) {
        phys_addr = surf->vpu_out_buf.vframe_buf.phy_ptr[0];
    } else if (surf->has_preallocated && surf->preallocated_gbuf.phys_addr[0] > 0) {
        phys_addr = surf->preallocated_gbuf.phys_addr[0];
    }

    VADRMPRIMESurfaceDescriptor *desc = (VADRMPRIMESurfaceDescriptor *)descriptor;
    memset(desc, 0, sizeof(*desc));
    desc->fourcc = VA_FOURCC_NV12;
    desc->width = surf->width;
    desc->height = surf->height;
    desc->num_objects = num_objects;
    desc->objects[0].fd = exp_fds[0];
    desc->objects[0].size = (uint32_t)object_sizes[0];
    desc->objects[0].drm_format_modifier = 0; /* DRM_FORMAT_MOD_LINEAR */
    if (num_objects == 2) {
        desc->objects[1].fd = exp_fds[1];
        desc->objects[1].size = (uint32_t)object_sizes[1];
        desc->objects[1].drm_format_modifier = 0;
    }

    if (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) {
        /* Kodi's EGL importer consumes one single-plane layer per texture. */
        desc->num_layers = 2;

        desc->layers[0].drm_format = DRM_FORMAT_R8;
        desc->layers[0].num_planes = 1;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0] = y_offset;
        desc->layers[0].pitch[0] = pitch;

        desc->layers[1].drm_format = DRM_FORMAT_GR88;
        desc->layers[1].num_planes = 1;
        desc->layers[1].object_index[0] = uv_object_index;
        desc->layers[1].offset[0] = uv_offset;
        desc->layers[1].pitch[0] = uv_pitch;
    } else {
        desc->num_layers = 1;
        desc->layers[0].drm_format = VA_FOURCC_NV12;
        desc->layers[0].num_planes = 2;
        desc->layers[0].object_index[0] = 0;
        desc->layers[0].offset[0] = y_offset;
        desc->layers[0].pitch[0] = pitch;

        desc->layers[0].object_index[1] = uv_object_index;
        desc->layers[0].offset[1] = uv_offset;
        desc->layers[0].pitch[1] = uv_pitch;
    }

    /* Validate all layers and planes with checked arithmetic to prevent buffer overruns */
    for (uint32_t l = 0; l < desc->num_layers; l++) {
        for (uint32_t p = 0; p < desc->layers[l].num_planes; p++) {
            uint32_t obj_idx = desc->layers[l].object_index[p];
            if (obj_idx >= desc->num_objects) {
                close(exp_fds[0]);
                if (num_objects == 2) close(exp_fds[1]);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
            uint32_t p_offset = desc->layers[l].offset[p];
            uint32_t p_pitch = desc->layers[l].pitch[p];
            uint32_t p_height = (p == 1 || desc->layers[l].drm_format == DRM_FORMAT_GR88) ?
                                ((desc->height + 1u) / 2u) : desc->height;
            uint64_t plane_required = (uint64_t)p_offset + (uint64_t)p_pitch * p_height;
            if (plane_required > desc->objects[obj_idx].size) {
                va_trace("vaExportSurfaceHandle: bounds check failed for layer %u plane %u (req=%lu > size=%u)",
                         l, p, (unsigned long)plane_required, desc->objects[obj_idx].size);
                close(exp_fds[0]);
                if (num_objects == 2) close(exp_fds[1]);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
        }
    }

    va_trace("vaExportSurfaceHandle -> success: surf=%u, dma_fd=%d, exp_fd=%d, objects=%u, phys=0x%lx, %ux%u",
             surface_id, surf->dma_fd, exp_fds[0], num_objects,
             (unsigned long)phys_addr, desc->width, desc->height);
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaCreateImage(
    VADriverContextP ctx,
    VAImageFormat *format,
    int width,
    int height,
    VAImage *image
) {
    va_trace("vaCreateImage: %dx%d, format=0x%x", width, height, (format ? format->fourcc : 0));
    if (!ctx || !ctx->pDriverData || !format || !image || width <= 0 || height <= 0 ||
        (width & 1) != 0 || (height & 1) != 0 ||
        format->fourcc != VA_FOURCC_NV12) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);

    uint64_t stride64 = ((uint64_t)(unsigned int)width + 15u) & ~UINT64_C(15);
    uint64_t luma_size64 = stride64 * (uint64_t)(unsigned int)height;
    uint64_t data_size64 = luma_size64 + luma_size64 / 2;
    if (stride64 > UINT_MAX || luma_size64 > UINT_MAX || data_size64 > UINT_MAX) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    for (int i = 1; i < MAX_IMAGES; i++) {
        if (!drv->images[i].allocated) {
            memset(&drv->images[i], 0, sizeof(drv->images[i]));
            drv->images[i].allocated = 1;
            drv->images[i].id = (VAImageID)i;

            unsigned int stride = (unsigned int)stride64;
            unsigned int data_size = (unsigned int)data_size64;

            VABufferID buf_id;
            VAStatus st = hobot_vaCreateBufferLocked(drv, VAImageBufferType,
                                                      data_size, 1, NULL, &buf_id);
            if (st != VA_STATUS_SUCCESS) {
                drv->images[i].allocated = 0;
                pthread_mutex_unlock(&drv->mutex);
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
            va_trace("vaCreateImage -> id=%u, buf_id=%u", (unsigned int)i, (unsigned int)buf_id);
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_SUCCESS;
        }
    }
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

static VAStatus hobot_vaDestroyImage(VADriverContextP ctx, VAImageID image) {
    va_trace("vaDestroyImage: id=%u", (unsigned int)image);
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    if (image <= 0 || image >= MAX_IMAGES || !drv->images[image].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }
    VAStatus buffer_status = hobot_vaDestroyBufferLocked(drv, drv->images[image].buf_id);
    if (buffer_status != VA_STATUS_SUCCESS) {
        pthread_mutex_unlock(&drv->mutex);
        return buffer_status;
    }
    drv->images[image].allocated = 0;
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_get_decoded_nv12_planes(
    HobotSurface *surface,
    HobotDecodedNV12Layout *layout
) {
    if (!surface || !layout || surface->width == 0 || surface->height == 0 ||
        !surface->vpu_out_buf.vframe_buf.vir_ptr[0] ||
        surface->vpu_out_buf.vframe_buf.size == 0) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    uint32_t y_stride = surface->vpu_out_buf.vframe_buf.stride > 0 ?
                        (uint32_t)surface->vpu_out_buf.vframe_buf.stride : surface->width;
    /* In mc_video_frame_buffer_info_t, vstride is the chroma pitch in bytes. */
    uint32_t uv_stride = surface->vpu_out_buf.vframe_buf.vstride > 0 ?
                         (uint32_t)surface->vpu_out_buf.vframe_buf.vstride : y_stride;
    uint64_t y_extent = (uint64_t)y_stride * surface->height;
    uint64_t uv_extent = (uint64_t)uv_stride * ((surface->height + 1u) / 2u);
    uint64_t allocation_size = surface->vpu_out_buf.vframe_buf.size;
    const unsigned char *y = surface->vpu_out_buf.vframe_buf.vir_ptr[0];
    const unsigned char *uv = surface->vpu_out_buf.vframe_buf.vir_ptr[1];
    uint32_t y_component_size = surface->vpu_out_buf.vframe_buf.compSize[0];
    uint32_t uv_component_size = surface->vpu_out_buf.vframe_buf.compSize[1];

    if (y_stride < surface->width || uv_stride < surface->width ||
        (y_stride & 1u) != 0 || (uv_stride & 1u) != 0 ||
        y_extent > allocation_size || uv_extent > allocation_size ||
        (y_component_size > 0 && y_extent > y_component_size) ||
        (uv_component_size > 0 && uv_extent > uv_component_size)) {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    uint64_t uv_offset = 0;
    int contiguous = 1;
    if (uv) {
        uintptr_t y_addr = (uintptr_t)y;
        uintptr_t uv_addr = (uintptr_t)uv;
        if (uv_addr >= y_addr &&
            (uint64_t)(uv_addr - y_addr) <= allocation_size) {
            uv_offset = (uint64_t)(uv_addr - y_addr);
            if (uv_offset < y_extent || uv_offset > allocation_size ||
                uv_extent > allocation_size - uv_offset ||
                (y_component_size > 0 && uv_offset < y_component_size))
                return VA_STATUS_ERROR_OPERATION_FAILED;
        } else if (y_component_size > 0 && uv_component_size > 0 &&
                   y_component_size <= allocation_size &&
                   uv_component_size <= allocation_size - y_component_size) {
            contiguous = 0;
            uv_offset = y_component_size;
        } else {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    } else {
        if (y_component_size == 0 || y_component_size < y_extent ||
            y_component_size > allocation_size) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        uv_offset = y_component_size;
    }
    if (uv_offset > allocation_size ||
        (contiguous && uv_extent > allocation_size - uv_offset) ||
        (uv_component_size > 0 && uv_component_size > allocation_size - uv_offset))
        return VA_STATUS_ERROR_OPERATION_FAILED;

    if (!uv) {
        uintptr_t y_addr = (uintptr_t)y;
        if (uv_offset > SIZE_MAX || uv_offset > UINTPTR_MAX - y_addr)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        uv = (const unsigned char *)(y_addr + (uintptr_t)uv_offset);
    }

    uint32_t vertical_rows = 0;
    if (y_component_size > 0 && y_component_size % y_stride == 0) {
        vertical_rows = y_component_size / y_stride;
    } else if (contiguous && uv_offset % y_stride == 0) {
        vertical_rows = (uint32_t)(uv_offset / y_stride);
    }
    if (vertical_rows != 0 &&
        (vertical_rows < surface->height || (vertical_rows & 1u) != 0))
        return VA_STATUS_ERROR_OPERATION_FAILED;

    layout->y_plane = y;
    layout->uv_plane = uv;
    layout->y_stride = y_stride;
    layout->uv_stride = uv_stride;
    layout->vertical_rows = vertical_rows;
    layout->uv_offset = uv_offset;
    layout->contiguous = contiguous;
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
    va_trace("vaGetImage: surf=%u, image=%u, %ux%u", surface, image, width, height);
    if (!ctx || !ctx->pDriverData || x < 0 || y < 0 || width == 0 || height == 0) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;

    pthread_mutex_lock(&drv->mutex);
    if (surface <= 0 || surface >= MAX_SURFACES || !drv->surfaces[surface].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (image <= 0 || image >= MAX_IMAGES || !drv->images[image].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    HobotSurface *s = &drv->surfaces[surface];
    HobotImage *img = &drv->images[image];
    if (s->decode_error) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    int needs_sync = s->decode_pending ||
                     (!s->has_decoded_frame && !s->raw_data_valid && !s->has_preallocated);
    if (needs_sync) {
        VAStatus sync_status = hobot_vaSyncSurfaceLocked(drv, surface);
        if (sync_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return sync_status;
        }
    }

    if (surface <= 0 || surface >= MAX_SURFACES || !drv->surfaces[surface].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (image <= 0 || image >= MAX_IMAGES || !drv->images[image].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }

    s = &drv->surfaces[surface];
    img = &drv->images[image];
    if (s->decode_error) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    if ((x & 1) != 0 || (y & 1) != 0 || (width & 1) != 0 || (height & 1) != 0 ||
        (unsigned int)x + width > s->width || (unsigned int)y + height > s->height ||
        width > img->image.width || height > img->image.height ||
        img->buf_id <= 0 || img->buf_id >= MAX_BUFFERS ||
        !drv->buffers[img->buf_id].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (drv->buffers[img->buf_id].map_count > 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (drv->buffers[img->buf_id].size < img->image.data_size) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    void *dst_data = drv->buffers[img->buf_id].data;
    if (!dst_data) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    const unsigned char *y_src = NULL;
    const unsigned char *uv_src = NULL;
    uint32_t src_y_stride = 0;
    uint32_t src_uv_stride = 0;
    if (s->has_decoded_frame) {
        HobotDecodedNV12Layout src_layout;
        VAStatus layout_status = hobot_get_decoded_nv12_planes(
            s, &src_layout);
        if (layout_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return layout_status;
        }
        y_src = src_layout.y_plane;
        uv_src = src_layout.uv_plane;
        src_y_stride = src_layout.y_stride;
        src_uv_stride = src_layout.uv_stride;
    } else {
        if (!s->raw_data_valid && s->has_preallocated) {
            VAStatus copy_status = hobot_copy_gbuf_to_staging(s);
            if (copy_status != VA_STATUS_SUCCESS) {
                pthread_mutex_unlock(&drv->mutex);
                return copy_status;
            }
        }
        src_y_stride = s->stride > 0 ? (uint32_t)s->stride : s->width;
        src_uv_stride = src_y_stride;
        uint64_t staging_size = (uint64_t)src_y_stride * s->height * 3u / 2u;
        if (!s->raw_data_valid || !s->raw_data || src_y_stride < s->width ||
            staging_size > s->raw_data_size) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        y_src = s->raw_data;
        uv_src = y_src + (size_t)src_y_stride * s->height;
    }

    {
        unsigned char *dst = (unsigned char *)dst_data;
        uint32_t dst_y_stride = img->image.pitches[0];
        uint32_t dst_uv_stride = img->image.pitches[1];

        unsigned char *dst_uv = dst + img->image.offsets[1];
        for (unsigned int r = 0; r < height; r++) {
            memcpy(dst + r * dst_y_stride,
                   y_src + ((unsigned int)y + r) * src_y_stride + (unsigned int)x,
                   width);
        }
        for (unsigned int r = 0; r < (height / 2); r++) {
            memcpy(dst_uv + r * dst_uv_stride,
                   uv_src + ((unsigned int)y / 2 + r) * src_uv_stride + (unsigned int)x,
                   width);
        }
    }
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaDeriveImage(
    VADriverContextP ctx,
    VASurfaceID surface,
    VAImage *image
) {
    va_trace("vaDeriveImage: surf=%u", surface);
    if (!ctx || !ctx->pDriverData || !image) return VA_STATUS_ERROR_INVALID_PARAMETER;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    memset(image, 0, sizeof(*image));
    pthread_mutex_lock(&drv->mutex);
    if (surface <= 0 || surface >= MAX_SURFACES || !drv->surfaces[surface].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    HobotSurface *initial_surface = &drv->surfaces[surface];
    int needs_sync = initial_surface->decode_pending ||
                     (!initial_surface->has_decoded_frame &&
                      !initial_surface->raw_data_valid &&
                      !initial_surface->has_preallocated);
    if (needs_sync) {
        VAStatus sync_status = hobot_vaSyncSurfaceLocked(drv, surface);
        if (sync_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return sync_status;
        }
    }

    if (surface <= 0 || surface >= MAX_SURFACES || !drv->surfaces[surface].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    HobotSurface *s = &drv->surfaces[surface];
    if (s->decode_error) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }

    const unsigned char *src_y = NULL;
    const unsigned char *src_uv = NULL;
    unsigned int src_y_stride = 0;
    unsigned int src_uv_stride = 0;
    if (s->has_decoded_frame) {
        HobotDecodedNV12Layout src_layout;
        VAStatus layout_status = hobot_get_decoded_nv12_planes(
            s, &src_layout);
        if (layout_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return layout_status;
        }
        src_y = src_layout.y_plane;
        src_uv = src_layout.uv_plane;
        src_y_stride = src_layout.y_stride;
        src_uv_stride = src_layout.uv_stride;
    } else {
        if (!s->raw_data_valid && s->has_preallocated) {
            VAStatus copy_status = hobot_copy_gbuf_to_staging(s);
            if (copy_status != VA_STATUS_SUCCESS) {
                pthread_mutex_unlock(&drv->mutex);
                return copy_status;
            }
        }
        src_y_stride = s->stride > 0 ? (unsigned int)s->stride : s->width;
        src_uv_stride = src_y_stride;
        uint64_t staging_size = (uint64_t)src_y_stride * s->height * 3u / 2u;
        if (!s->raw_data_valid || !s->raw_data || src_y_stride < s->width ||
            staging_size > s->raw_data_size) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        src_y = s->raw_data;
        src_uv = src_y + (size_t)src_y_stride * s->height;
    }

    unsigned int image_stride = s->stride > 0 ? (unsigned int)s->stride : s->width;
    uint64_t image_data_size = (uint64_t)image_stride * s->height * 3 / 2;
    if (image_stride < s->width || image_data_size > s->raw_data_size ||
        image_data_size > UINT_MAX) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (!s->raw_data) {
        s->raw_data = calloc(1, s->raw_data_size);
        if (!s->raw_data) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }

    int img_idx = -1;
    for (int i = 1; i < MAX_IMAGES; i++) {
        if (!drv->images[i].allocated) {
            img_idx = i;
            break;
        }
    }
    if (img_idx < 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    int buf_idx = -1;
    for (int i = 1; i < MAX_BUFFERS; i++) {
        if (!drv->buffers[i].allocated) {
            buf_idx = i;
            break;
        }
    }
    if (buf_idx < 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    drv->buffers[buf_idx].allocated = 1;
    drv->buffers[buf_idx].is_derived = 1;
    drv->buffers[buf_idx].id = (VABufferID)buf_idx;
    drv->buffers[buf_idx].type = VAImageBufferType;
    drv->buffers[buf_idx].size = s->raw_data_size;
    drv->buffers[buf_idx].capacity = s->raw_data_size;
    drv->buffers[buf_idx].element_size = 1;
    drv->buffers[buf_idx].num_elements = s->raw_data_size;
    drv->buffers[buf_idx].map_count = 0;
    drv->buffers[buf_idx].data = s->raw_data;

    image->image_id = (VAImageID)img_idx;
    image->format.fourcc = VA_FOURCC_NV12;
    image->format.byte_order = VA_LSB_FIRST;
    image->format.bits_per_pixel = 12;
    image->width = s->width;
    image->height = s->height;
    image->buf = (VABufferID)buf_idx;
    image->num_planes = 2;
    image->pitches[0] = image_stride;
    image->offsets[0] = 0;
    image->pitches[1] = image_stride;
    image->offsets[1] = (unsigned int)((uint64_t)image_stride * s->height);
    image->data_size = s->raw_data_size;

    drv->images[img_idx].allocated = 1;
    drv->images[img_idx].id = (VAImageID)img_idx;
    drv->images[img_idx].image = *image;
    drv->images[img_idx].buf_id = (VABufferID)buf_idx;
    drv->images[img_idx].surface_id = surface;

    if (s->has_decoded_frame && s->vpu_out_buf.vframe_buf.vir_ptr[0] && s->raw_data) {
        unsigned char *dst_y = s->raw_data;
        unsigned char *dst_uv = dst_y + image->offsets[1];
        for (unsigned int r = 0; r < s->height; r++) {
            memcpy(dst_y + (size_t)r * image_stride,
                   src_y + (size_t)r * src_y_stride, s->width);
        }
        for (unsigned int r = 0; r < s->height / 2; r++) {
            memcpy(dst_uv + (size_t)r * image_stride,
                   src_uv + (size_t)r * src_uv_stride, s->width);
        }
        s->raw_data_valid = 1;
    }

    pthread_mutex_unlock(&drv->mutex);
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
    if (!num_formats) return VA_STATUS_ERROR_INVALID_PARAMETER;
    *num_formats = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaCreateSubpicture(
    VADriverContextP ctx,
    VAImageID image,
    VASubpictureID *subpicture
) {
    if (!subpicture) return VA_STATUS_ERROR_INVALID_PARAMETER;
    *subpicture = VA_INVALID_ID;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus hobot_vaDestroySubpicture(VADriverContextP ctx, VASubpictureID subpicture) {
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus hobot_vaSetSubpictureImage(VADriverContextP ctx, VASubpictureID subpicture, VAImageID image) {
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus hobot_vaSetSubpictureChromakey(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    unsigned int chromakey_min,
    unsigned int chromakey_max,
    unsigned int chromakey_mask
) {
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus hobot_vaSetSubpictureGlobalAlpha(VADriverContextP ctx, VASubpictureID subpicture, float global_alpha) {
    return VA_STATUS_ERROR_UNIMPLEMENTED;
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
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus hobot_vaDeassociateSubpicture(
    VADriverContextP ctx,
    VASubpictureID subpicture,
    VASurfaceID *target_surfaces,
    int num_surfaces
) {
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus hobot_vaQueryDisplayAttributes(
    VADriverContextP ctx,
    VADisplayAttribute *attr_list,
    int *num_attributes
) {
    if (!num_attributes) return VA_STATUS_ERROR_INVALID_PARAMETER;
    *num_attributes = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaGetDisplayAttributes(
    VADriverContextP ctx,
    VADisplayAttribute *attr_list,
    int num_attributes
) {
    if (num_attributes < 0 || (num_attributes > 0 && !attr_list))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    return num_attributes == 0 ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
}

static VAStatus hobot_vaSetDisplayAttributes(
    VADriverContextP ctx,
    VADisplayAttribute *attr_list,
    int num_attributes
) {
    if (num_attributes < 0 || (num_attributes > 0 && !attr_list))
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    return num_attributes == 0 ? VA_STATUS_SUCCESS : VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
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
    if (!ctx || !ctx->pDriverData || surface <= 0 ||
        (!fourcc && !luma_stride && !chroma_u_stride && !chroma_v_stride &&
         !luma_offset && !chroma_u_offset && !chroma_v_offset &&
         !buffer_name && !buffer)) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    struct hobot_surface_info info;
    pthread_mutex_lock(&drv->mutex);
    VAStatus status = hobot_fill_surface_info_locked(drv, surface, &info);
    if (status != VA_STATUS_SUCCESS) {
        pthread_mutex_unlock(&drv->mutex);
        return status;
    }
    if (!info.virt_addr[0] || info.stride == 0 || info.vstride == 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    HobotSurface *surf = &drv->surfaces[surface];
    if (surf->has_decoded_frame) {
        uintptr_t y_address = (uintptr_t)info.virt_addr[0];
        uintptr_t uv_address = (uintptr_t)info.virt_addr[1];
        uint64_t expected_uv_offset = (uint64_t)info.stride * info.vstride;
        if (!info.virt_addr[1] || uv_address < y_address ||
            (uint64_t)(uv_address - y_address) != expected_uv_offset) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    if (surf->lock_count == UINT32_MAX) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    surf->lock_count++;

    if (fourcc) *fourcc = VA_FOURCC_NV12;
    if (luma_stride) *luma_stride = info.stride;
    if (chroma_u_stride) *chroma_u_stride = info.stride;
    if (chroma_v_stride) *chroma_v_stride = info.stride;
    if (luma_offset) *luma_offset = 0;
    if (chroma_u_offset) *chroma_u_offset = info.stride * info.vstride;
    if (chroma_v_offset) *chroma_v_offset = info.stride * info.vstride;
    if (buffer_name) {
        *buffer_name = info.dma_fd >= 0 ? (unsigned int)info.dma_fd : 0;
    }
    if (buffer) *buffer = info.virt_addr[0];
    va_trace("vaLockSurface -> success: surf=%u, %ux%u, stride=%u, vstride=%u, fd=%d",
             surface, info.width, info.height, info.stride, info.vstride, info.dma_fd);
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaUnlockSurface(VADriverContextP ctx, VASurfaceID surface) {
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    if (surface <= 0 || surface >= MAX_SURFACES || !drv->surfaces[surface].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    if (drv->surfaces[surface].lock_count == 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    drv->surfaces[surface].lock_count--;
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaSetImagePalette(VADriverContextP ctx, VAImageID image, unsigned char *palette) {
    return VA_STATUS_ERROR_UNIMPLEMENTED;
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
    if (!ctx || !ctx->pDriverData || src_x < 0 || src_y < 0 || dest_x < 0 || dest_y < 0 ||
        src_width == 0 || src_height == 0 || (src_x & 1) != 0 || (src_y & 1) != 0 ||
        (src_width & 1) != 0 || (src_height & 1) != 0 || (dest_x & 1) != 0 ||
        (dest_y & 1) != 0 || (dest_width != src_width) || (dest_height != src_height)) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    int sync_retry = 0;
    pthread_mutex_lock(&drv->mutex);
    HobotSurface *s;
    for (;;) {
        if (surface <= 0 || surface >= MAX_SURFACES || !drv->surfaces[surface].allocated) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }
        s = &drv->surfaces[surface];
        if (s->lock_count > 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_SURFACE_BUSY;
        }
        if (!s->decode_pending) break;
        if (sync_retry++) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_SURFACE_BUSY;
        }
        VAStatus sync_status = hobot_vaSyncSurfaceLocked(drv, surface);
        if (sync_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return sync_status;
        }
    }
    if (s->decode_error) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    if (image <= 0 || image >= MAX_IMAGES || !drv->images[image].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_IMAGE;
    }
    HobotImage *img = &drv->images[image];
    if ((uint64_t)(unsigned int)src_x + src_width > img->image.width ||
        (uint64_t)(unsigned int)src_y + src_height > img->image.height ||
        (uint64_t)(unsigned int)dest_x + dest_width > s->width ||
        (uint64_t)(unsigned int)dest_y + dest_height > s->height ||
        img->buf_id <= 0 || img->buf_id >= MAX_BUFFERS ||
        !drv->buffers[img->buf_id].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (drv->buffers[img->buf_id].size < img->image.data_size) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (drv->buffers[img->buf_id].map_count > 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    void *src_data = drv->buffers[img->buf_id].data;

    if (!src_data) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    unsigned int dst_stride = s->stride > 0 ? (unsigned int)s->stride : s->width;
    uint64_t staging_size = (uint64_t)dst_stride * s->height * 3u / 2u;
    if (dst_stride < s->width || staging_size > s->raw_data_size ||
        staging_size > UINT_MAX) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!s->raw_data) {
        s->raw_data = calloc(1, s->raw_data_size);
        if (!s->raw_data) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }

    if (s->has_decoded_frame) {
        VAContextID owner_id = s->output_context_id;
        if (owner_id <= 0 || owner_id >= MAX_CONTEXTS ||
            !drv->contexts[owner_id].allocated || !drv->contexts[owner_id].vpu_running ||
            drv->contexts[owner_id].is_encoder) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_CONTEXT;
        }
        HobotContext *owner = &drv->contexts[owner_id];
        if (owner->sync_active || owner->decode_picture_active) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        if (!s->raw_data_valid) {
            HobotDecodedNV12Layout decoded_layout;
            VAStatus layout_status = hobot_get_decoded_nv12_planes(
                s, &decoded_layout);
            if (layout_status != VA_STATUS_SUCCESS) {
                pthread_mutex_unlock(&drv->mutex);
                return layout_status;
            }
            for (unsigned int row = 0; row < s->height; row++)
                memcpy((unsigned char *)s->raw_data + (size_t)row * dst_stride,
                       decoded_layout.y_plane + (size_t)row * decoded_layout.y_stride,
                       s->width);
            unsigned char *staging_uv = (unsigned char *)s->raw_data +
                                        (size_t)dst_stride * s->height;
            for (unsigned int row = 0; row < s->height / 2u; row++)
                memcpy(staging_uv + (size_t)row * dst_stride,
                       decoded_layout.uv_plane + (size_t)row * decoded_layout.uv_stride,
                       s->width);
            s->raw_data_valid = 1;
        }

        int qret = hb_mm_mc_queue_output_buffer(&owner->vpu_ctx, &s->vpu_out_buf, 50);
        if (qret != 0)
            qret = hb_mm_mc_queue_output_buffer(&owner->vpu_ctx, &s->vpu_out_buf, 200);
        if (qret != 0) {
            owner->decode_failed = 1;
            fprintf(stderr, "[HOBOT-VA] vaPutImage: decoded output recycle failed for surface=%u: %d\n",
                    surface, qret);
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        s->has_decoded_frame = 0;
        s->dma_fd = -1;
        s->output_context_id = 0;
        memset(&s->vpu_out_buf, 0, sizeof(s->vpu_out_buf));
    } else if (!s->raw_data_valid && s->has_preallocated) {
        VAStatus copy_status = hobot_copy_gbuf_to_staging(s);
        if (copy_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return copy_status;
        }
    } else if (!s->raw_data_valid) {
        s->raw_data_valid = 1;
    }

    unsigned int src_y_stride = img->image.pitches[0];
    unsigned int src_uv_stride = img->image.pitches[1];
    unsigned char *src_y_plane = (unsigned char *)src_data + img->image.offsets[0] +
                                  (size_t)src_y * src_y_stride + (unsigned int)src_x;
    unsigned char *dst_y_plane = (unsigned char *)s->raw_data +
                                 (size_t)dest_y * dst_stride + (unsigned int)dest_x;
    for (unsigned int row = 0; row < src_height; row++) {
        memcpy(dst_y_plane + (size_t)row * dst_stride,
               src_y_plane + (size_t)row * src_y_stride, src_width);
    }

    unsigned char *src_uv_plane = (unsigned char *)src_data + img->image.offsets[1] +
                                  (size_t)(src_y / 2) * src_uv_stride + (unsigned int)src_x;
    unsigned char *dst_uv_plane = (unsigned char *)s->raw_data +
                                  (size_t)dst_stride * s->height +
                                  (size_t)(dest_y / 2) * dst_stride + (unsigned int)dest_x;
    for (unsigned int row = 0; row < src_height / 2; row++) {
        memcpy(dst_uv_plane + (size_t)row * dst_stride,
               src_uv_plane + (size_t)row * src_uv_stride, src_width);
    }
    s->raw_data_valid = 1;
    s->raw_data_dirty = 1;
    VAStatus upload_status = hobot_upload_staging_to_gbuf(s);
    pthread_mutex_unlock(&drv->mutex);
    return upload_status;
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
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    int surface_valid = surface > 0 && surface < MAX_SURFACES &&
                        drv->surfaces[surface].allocated;
    pthread_mutex_unlock(&drv->mutex);
    if (!surface_valid) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/* Driver Initialization Entrypoints */
VAStatus __vaDriverInit_1_0(VADriverContextP ctx);
VAStatus __vaDriverInit_0_32(VADriverContextP ctx);

static VAStatus hobot_init_driver(VADriverContextP ctx) {
    if (!ctx || !ctx->vtable) return VA_STATUS_ERROR_INVALID_CONTEXT;

    int32_t mem_ret = hb_mem_module_open();
    if (mem_ret != 0) {
        fprintf(stderr, "[HOBOT-VA] driver initialization: hb_mem_module_open failed: %d\n",
                mem_ret);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    HobotDriverData *drv = calloc(1, sizeof(HobotDriverData));
    if (!drv) {
        mem_ret = hb_mem_module_close();
        if (mem_ret != 0)
            fprintf(stderr, "[HOBOT-VA] driver initialization: hb_mem_module_close after allocation failure failed: %d\n",
                    mem_ret);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (pthread_mutex_init(&drv->mutex, NULL) != 0) {
        free(drv);
        mem_ret = hb_mem_module_close();
        if (mem_ret != 0)
            fprintf(stderr, "[HOBOT-VA] driver initialization: hb_mem_module_close after mutex failure failed: %d\n",
                    mem_ret);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (pthread_cond_init(&drv->sync_cond, NULL) != 0) {
        pthread_mutex_destroy(&drv->mutex);
        free(drv);
        mem_ret = hb_mem_module_close();
        if (mem_ret != 0)
            fprintf(stderr, "[HOBOT-VA] driver initialization: hb_mem_module_close after condition failure failed: %d\n",
                    mem_ret);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    drv->sync_cond_initialized = 1;
    drv->initialized = 1;

    ctx->pDriverData = (void *)drv;
    ctx->version_major = 1;
    ctx->version_minor = 14;
    ctx->max_profiles = NUM_SUPPORTED_PROFILES;
    ctx->max_entrypoints = 2;
    ctx->max_attributes = MAX_CONFIG_ATTRIBUTES;
    ctx->max_image_formats = 1;
    /* libva rejects zero initialization maxima; the query still reports no formats. */
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
    vtable->vaBufferInfo = hobot_vaBufferInfo;
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

VAStatus vaGetHobotSurfaceInfo(
    VADisplay dpy,
    VASurfaceID surface,
    struct hobot_surface_info *info
) {
    if (!dpy || !info) return VA_STATUS_ERROR_INVALID_PARAMETER;

    struct VADisplayContext *disp_ctx = (struct VADisplayContext *)dpy;
    if (disp_ctx->vadpy_magic != VA_DISPLAY_MAGIC) {
        return VA_STATUS_ERROR_INVALID_DISPLAY;
    }
    VADriverContextP drv_ctx = disp_ctx->pDriverContext;
    if (!drv_ctx || !drv_ctx->pDriverData) {
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    }

    return hobot_fill_surface_info(drv_ctx, surface, info);
}

VAStatus __vaDriverInit_1_0(VADriverContextP ctx) {
    return hobot_init_driver(ctx);
}

VAStatus __vaDriverInit_0_32(VADriverContextP ctx) {
    return hobot_init_driver(ctx);
}
