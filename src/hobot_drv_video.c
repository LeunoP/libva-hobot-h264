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
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
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
#include <stdatomic.h>

static pthread_once_t va_trace_once = PTHREAD_ONCE_INIT;
static int va_trace_enabled;
static _Atomic uintptr_t va_external_input_token_sequence;

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
#define MAX_SURFACE_ATTRIBUTES 3
#define MAX_IMAGES   512
#define HOBOT_WATCHDOG_FALLBACK_THRESHOLD 1
#define HOBOT_WATCHDOG_CLEAN_RESET_SEC 10
#define HOBOT_DECODE_RESULT_SUCCESS 0x01
#define HOBOT_DECODE_RESULT_SUCCESS_WITH_WARNING 0x10
#define HOBOT_HEVC_LEVEL_IDC 153
#define HOBOT_HEVC_MAX_WIDTH 3840
#define HOBOT_HEVC_MAX_HEIGHT 2160
#define HOBOT_HEVC_DECODE_MAX_WIDTH 8192
#define HOBOT_HEVC_DECODE_MAX_HEIGHT 4096

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

static void hobot_profile_max_resolution(VAProfile profile,
                                         VAEntrypoint entrypoint,
                                         unsigned int *max_width,
                                         unsigned int *max_height) {
    *max_width = 4096;
    *max_height = 4096;
    if (profile != VAProfileHEVCMain)
        return;

    if (entrypoint == VAEntrypointVLD) {
        *max_width = HOBOT_HEVC_DECODE_MAX_WIDTH;
        *max_height = HOBOT_HEVC_DECODE_MAX_HEIGHT;
    } else {
        *max_width = HOBOT_HEVC_MAX_WIDTH;
        *max_height = HOBOT_HEVC_MAX_HEIGHT;
    }
}

static int hobot_profile_resolution_supported(VAProfile profile,
                                               VAEntrypoint entrypoint,
                                               int width,
                                               int height) {
    if (width <= 0 || height <= 0)
        return 0;
    unsigned int max_width, max_height;
    hobot_profile_max_resolution(profile, entrypoint,
                                 &max_width, &max_height);
    return (unsigned int)width <= max_width &&
           (unsigned int)height <= max_height;
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

    if (!pic || !out || max_len < 2 ||
        !pic->seq_fields.bits.frame_mbs_only_flag ||
        pic->pic_fields.bits.field_pic_flag ||
        pic->seq_fields.bits.chroma_format_idc != 1 ||
        (pic->CurrPic.flags &
         (VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD)) != 0)
        return 0;

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

    unsigned int max_width_mbs = ((unsigned int)context_width + 15u) / 16u;
    unsigned int max_height_mbs = ((unsigned int)context_height + 15u) / 16u;
    unsigned int picture_height_mbs =
        (unsigned int)pic->picture_height_in_mbs_minus1 + 1u;
    unsigned int current_field_flags = pic->CurrPic.flags &
        (VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD);

    /* libva deprecates this field but still exposes it in the decode API. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    has_slice_groups = pic->num_slice_groups_minus1 != 0;
#pragma GCC diagnostic pop

    if (pic->picture_width_in_mbs_minus1 + 1u > max_width_mbs ||
        picture_height_mbs > max_height_mbs ||
        !pic->seq_fields.bits.frame_mbs_only_flag ||
        pic->pic_fields.bits.field_pic_flag || current_field_flags != 0 ||
        pic->num_ref_frames > 16 ||
        pic->pic_init_qp_minus26 < -26 || pic->pic_init_qp_minus26 > 25 ||
        pic->pic_init_qs_minus26 < -26 || pic->pic_init_qs_minus26 > 25 ||
        pic->chroma_qp_index_offset < -12 ||
        pic->chroma_qp_index_offset > 12 ||
        pic->second_chroma_qp_index_offset < -12 ||
        pic->second_chroma_qp_index_offset > 12 ||
        pic->pic_fields.bits.weighted_bipred_idc > 2 ||
        pic->seq_fields.bits.chroma_format_idc != 1 ||
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

static int hobot_hevc_tile_axis_supported(
    uint64_t unit_count,
    uint64_t tile_count,
    const uint16_t *tile_size_minus1
) {
    if (!tile_size_minus1 || tile_count == 0 || tile_count > unit_count)
        return 0;

    uint64_t consumed_units = 0;
    for (uint64_t tile = 0; tile + 1u < tile_count; tile++) {
        uint64_t tile_size = (uint64_t)tile_size_minus1[tile] + 1u;
        if (tile_size >= unit_count - consumed_units)
            return 0;
        consumed_units += tile_size;
    }
    return consumed_units < unit_count;
}

static int hobot_hevc_tile_layout_supported(
    const VAPictureParameterBufferHEVC *pic
) {
    if (!pic)
        return 0;
    if (!pic->pic_fields.bits.tiles_enabled_flag)
        return 1;

    unsigned int ctb_log2 = 3u + pic->log2_min_luma_coding_block_size_minus3 +
                            pic->log2_diff_max_min_luma_coding_block_size;
    if (ctb_log2 < 4u || ctb_log2 > 6u ||
        pic->num_tile_columns_minus1 >= 19u ||
        pic->num_tile_rows_minus1 >= 21u)
        return 0;

    uint64_t ctb_size = UINT64_C(1) << ctb_log2;
    uint64_t ctb_columns =
        ((uint64_t)pic->pic_width_in_luma_samples + ctb_size - 1u) / ctb_size;
    uint64_t ctb_rows =
        ((uint64_t)pic->pic_height_in_luma_samples + ctb_size - 1u) / ctb_size;
    uint64_t tile_columns = (uint64_t)pic->num_tile_columns_minus1 + 1u;
    uint64_t tile_rows = (uint64_t)pic->num_tile_rows_minus1 + 1u;
    if (tile_columns * tile_rows < 2u ||
        !hobot_hevc_tile_axis_supported(ctb_columns, tile_columns,
                                        pic->column_width_minus1) ||
        !hobot_hevc_tile_axis_supported(ctb_rows, tile_rows,
                                        pic->row_height_minus1))
        return 0;

    uint64_t min_tile_width_ctbs =
        (UINT64_C(256) + ctb_size - 1u) / ctb_size;
    uint64_t min_tile_height_ctbs =
        (UINT64_C(64) + ctb_size - 1u) / ctb_size;
    uint64_t column_start = 0;
    for (uint64_t column = 0; column < tile_columns; column++) {
        uint64_t column_width = column + 1u < tile_columns ?
            (uint64_t)pic->column_width_minus1[column] + 1u :
            ctb_columns - column_start;
        if (column_width < min_tile_width_ctbs)
            return 0;
        column_start += column_width;
    }
    uint64_t row_start = 0;
    for (uint64_t row = 0; row < tile_rows; row++) {
        uint64_t row_height = row + 1u < tile_rows ?
            (uint64_t)pic->row_height_minus1[row] + 1u :
            ctb_rows - row_start;
        if (row_height < min_tile_height_ctbs)
            return 0;
        row_start += row_height;
    }

    return 1;
}

static int hobot_hevc_tile_grid_is_uniform(
    const VAPictureParameterBufferHEVC *pic
) {
    if (!pic || !pic->pic_fields.bits.tiles_enabled_flag ||
        !hobot_hevc_tile_layout_supported(pic))
        return 0;

    unsigned int ctb_log2 = 3u + pic->log2_min_luma_coding_block_size_minus3 +
                            pic->log2_diff_max_min_luma_coding_block_size;
    uint64_t ctb_size = UINT64_C(1) << ctb_log2;
    uint64_t ctb_columns =
        ((uint64_t)pic->pic_width_in_luma_samples + ctb_size - 1u) / ctb_size;
    uint64_t ctb_rows =
        ((uint64_t)pic->pic_height_in_luma_samples + ctb_size - 1u) / ctb_size;
    uint64_t tile_columns = (uint64_t)pic->num_tile_columns_minus1 + 1u;
    uint64_t tile_rows = (uint64_t)pic->num_tile_rows_minus1 + 1u;

    for (uint64_t i = 0; i + 1u < tile_columns; i++) {
        uint64_t start = i * ctb_columns / tile_columns;
        uint64_t end = (i + 1u) * ctb_columns / tile_columns;
        if (end <= start ||
            pic->column_width_minus1[i] != end - start - 1u)
            return 0;
    }
    for (uint64_t i = 0; i + 1u < tile_rows; i++) {
        uint64_t start = i * ctb_rows / tile_rows;
        uint64_t end = (i + 1u) * ctb_rows / tile_rows;
        if (end <= start || pic->row_height_minus1[i] != end - start - 1u)
            return 0;
    }
    return 1;
}

static int hobot_hevc_picture_parameters_supported(
    const VAPictureParameterBufferHEVC *pic,
    int context_width,
    int context_height
) {
    unsigned int min_cb_log2;
    unsigned int ctb_log2;
    unsigned int min_pcm_log2;
    unsigned int max_pcm_log2;

    if (!pic || context_width <= 0 || context_height <= 0)
        return 0;
    if (pic->pic_fields.bits.tiles_enabled_flag &&
        pic->pic_fields.bits.entropy_coding_sync_enabled_flag) {
        va_trace("HEVC Main does not allow WPP with tiles enabled");
        return 0;
    }
    if (pic->num_short_term_ref_pic_sets > 64 ||
        pic->st_rps_bits > 2048u) {
        va_trace("HEVC SPS RPS count is outside the supported picture subset: count=%u",
                 pic->num_short_term_ref_pic_sets);
        return 0;
    }
    if (context_width > UINT16_MAX || context_height > UINT16_MAX ||
        pic->pic_width_in_luma_samples != (uint16_t)context_width ||
        pic->pic_height_in_luma_samples != (uint16_t)context_height ||
        pic->pic_fields.bits.chroma_format_idc != 1 ||
        pic->pic_fields.bits.separate_colour_plane_flag ||
        pic->bit_depth_luma_minus8 != 0 || pic->bit_depth_chroma_minus8 != 0 ||
        pic->slice_parsing_fields.bits.long_term_ref_pics_present_flag ||
        pic->num_long_term_ref_pic_sps != 0 ||
        pic->pic_fields.bits.ReservedBits != 0 ||
        pic->slice_parsing_fields.bits.ReservedBits != 0 ||
        pic->sps_max_dec_pic_buffering_minus1 > 15 ||
        pic->log2_max_pic_order_cnt_lsb_minus4 > 12 ||
        pic->log2_min_luma_coding_block_size_minus3 > 3 ||
        pic->log2_diff_max_min_luma_coding_block_size > 3 ||
        (unsigned int)pic->log2_min_luma_coding_block_size_minus3 +
            pic->log2_diff_max_min_luma_coding_block_size < 1 ||
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
        pic->log2_parallel_merge_level_minus2 > 4 ||
        !hobot_hevc_tile_layout_supported(pic))
        return 0;

    if (pic->pic_fields.bits.pcm_enabled_flag) {
        min_cb_log2 = (unsigned int)pic->log2_min_luma_coding_block_size_minus3 + 3u;
        ctb_log2 = min_cb_log2 +
                   pic->log2_diff_max_min_luma_coding_block_size;
        min_pcm_log2 =
            (unsigned int)pic->log2_min_pcm_luma_coding_block_size_minus3 + 3u;
        max_pcm_log2 = min_pcm_log2 +
                       pic->log2_diff_max_min_pcm_luma_coding_block_size;

        if (pic->pcm_sample_bit_depth_luma_minus1 !=
                (unsigned int)pic->bit_depth_luma_minus8 + 7u ||
            pic->pcm_sample_bit_depth_chroma_minus1 !=
                (unsigned int)pic->bit_depth_chroma_minus8 + 7u ||
            pic->log2_min_pcm_luma_coding_block_size_minus3 > 2u ||
            pic->log2_diff_max_min_pcm_luma_coding_block_size > 2u ||
            min_pcm_log2 < (min_cb_log2 < 5u ? min_cb_log2 : 5u) ||
            max_pcm_log2 > (ctb_log2 < 5u ? ctb_log2 : 5u)) {
            va_trace("HEVC PCM parameters are outside the supported 8-bit Main subset");
            return 0;
        }
    }

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
        slice->LongSliceFlags.fields.color_plane_id != 0 ||
        slice->LongSliceFlags.fields.slice_type > 2 ||
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
    const VAPictureParameterBufferHEVC *picture;
    uint64_t picture_ctb_count;
    uint64_t picture_ctb_rows;
    uint64_t tile_entry_point_count;
    uint64_t current_tile_entry_point_count;
    uint64_t entry_point_count;
    int entropy_coding_sync_enabled;
    int previous_slice_type_valid;
    unsigned int previous_slice_type;
    size_t slice_count;
    size_t next_slice;
    uint32_t previous_slice_address;
} HobotHevcSliceSequence;

static int hobot_hevc_tile_start_index_for_address(
    const VAPictureParameterBufferHEVC *picture,
    uint32_t address,
    uint64_t *tile_index
) {
    if (!picture || !tile_index ||
        !picture->pic_fields.bits.tiles_enabled_flag ||
        !hobot_hevc_tile_layout_supported(picture))
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
    uint64_t tile_columns =
        (uint64_t)picture->num_tile_columns_minus1 + 1u;
    uint64_t tile_rows = (uint64_t)picture->num_tile_rows_minus1 + 1u;
    if (ctb_columns == 0 || ctb_rows == 0 ||
        ctb_columns > UINT64_MAX / ctb_rows ||
        address >= ctb_columns * ctb_rows)
        return 0;

    uint64_t ctb_x = address % ctb_columns;
    uint64_t ctb_y = address / ctb_columns;
    uint64_t column_start = 0;
    uint64_t tile_column = tile_columns;
    for (uint64_t column = 0; column < tile_columns; column++) {
        uint64_t column_width = column + 1u < tile_columns ?
            (uint64_t)picture->column_width_minus1[column] + 1u :
            ctb_columns - column_start;
        if (ctb_x == column_start) {
            tile_column = column;
            break;
        }
        if (ctb_x < column_start + column_width)
            return 0;
        column_start += column_width;
    }

    uint64_t row_start = 0;
    uint64_t tile_row = tile_rows;
    for (uint64_t row = 0; row < tile_rows; row++) {
        uint64_t row_height = row + 1u < tile_rows ?
            (uint64_t)picture->row_height_minus1[row] + 1u :
            ctb_rows - row_start;
        if (ctb_y == row_start) {
            tile_row = row;
            break;
        }
        if (ctb_y < row_start + row_height)
            return 0;
        row_start += row_height;
    }
    if (tile_column >= tile_columns || tile_row >= tile_rows)
        return 0;

    *tile_index = tile_row * tile_columns + tile_column;
    return 1;
}

static int hobot_hevc_tile_slice_entry_point_count(
    const VAPictureParameterBufferHEVC *picture,
    uint32_t slice_start_address,
    int has_next_slice,
    uint32_t next_slice_address,
    uint64_t *entry_point_count
) {
    if (!picture || !entry_point_count)
        return 0;

    uint64_t start_tile;
    if (!hobot_hevc_tile_start_index_for_address(
            picture, slice_start_address, &start_tile))
        return 0;

    uint64_t tile_columns =
        (uint64_t)picture->num_tile_columns_minus1 + 1u;
    uint64_t tile_rows = (uint64_t)picture->num_tile_rows_minus1 + 1u;
    uint64_t total_tiles = tile_columns * tile_rows;
    uint64_t end_tile = total_tiles;
    if (has_next_slice &&
        !hobot_hevc_tile_start_index_for_address(
            picture, next_slice_address, &end_tile))
        return 0;
    if (start_tile >= end_tile || end_tile > total_tiles)
        return 0;

    uint64_t substream_count = 0;
    if (picture->pic_fields.bits.entropy_coding_sync_enabled_flag) {
        unsigned int ctb_log2 = 3u +
            picture->log2_min_luma_coding_block_size_minus3 +
            picture->log2_diff_max_min_luma_coding_block_size;
        uint64_t ctb_size = UINT64_C(1) << ctb_log2;
        uint64_t ctb_rows =
            ((uint64_t)picture->pic_height_in_luma_samples + ctb_size - 1u) /
            ctb_size;
        for (uint64_t tile = start_tile; tile < end_tile; tile++) {
            uint64_t tile_row = tile / tile_columns;
            uint64_t row_start = 0;
            for (uint64_t row = 0; row < tile_row; row++)
                row_start += (uint64_t)picture->row_height_minus1[row] + 1u;
            uint64_t row_height = tile_row + 1u < tile_rows ?
                (uint64_t)picture->row_height_minus1[tile_row] + 1u :
                ctb_rows - row_start;
            if (substream_count > UINT64_MAX - row_height)
                return 0;
            substream_count += row_height;
        }
    } else {
        substream_count = end_tile - start_tile;
    }
    if (substream_count == 0)
        return 0;
    *entry_point_count = substream_count - 1u;
    return 1;
}

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
            picture->log2_diff_max_min_luma_coding_block_size > 3 ||
        !hobot_hevc_tile_layout_supported(picture))
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
    sequence->picture = picture;
    sequence->picture_ctb_rows = ctb_rows;
    sequence->tile_entry_point_count = 0;
    sequence->current_tile_entry_point_count = 0;
    if (picture->pic_fields.bits.tiles_enabled_flag) {
        uint64_t tile_columns =
            (uint64_t)picture->num_tile_columns_minus1 + 1u;
        uint64_t tile_rows = (uint64_t)picture->num_tile_rows_minus1 + 1u;
        uint64_t substream_count = tile_columns *
            (picture->pic_fields.bits.entropy_coding_sync_enabled_flag ?
                ctb_rows : tile_rows);
        if (substream_count < 2u)
            return 0;
        sequence->tile_entry_point_count = substream_count - 1u;
        sequence->current_tile_entry_point_count =
            sequence->tile_entry_point_count;
    }
    sequence->entry_point_count = 0;
    sequence->entropy_coding_sync_enabled =
        picture->pic_fields.bits.entropy_coding_sync_enabled_flag;
    sequence->previous_slice_type_valid = 0;
    sequence->previous_slice_type = 0;
    sequence->slice_count = slice_count;
    sequence->next_slice = 0;
    sequence->previous_slice_address = 0;
    return sequence->picture_ctb_count != 0;
}

static int hobot_hevc_slice_sequence_add_with_next(
    HobotHevcSliceSequence *sequence,
    const VASliceParameterBufferHEVC *slice,
    size_t slice_data_buffer_size,
    int has_next_slice,
    uint32_t next_slice_address,
    uint64_t *tile_entry_point_count
) {
    if (!sequence || !slice || sequence->next_slice >= sequence->slice_count ||
        !hobot_hevc_slice_parameter_fields_supported(slice) ||
        slice->slice_data_offset > slice_data_buffer_size ||
        slice->slice_data_size >
            slice_data_buffer_size - slice->slice_data_offset ||
        slice->entry_offset_to_subset_array != 0 ||
        slice->slice_segment_address >= sequence->picture_ctb_count)
        return 0;

    size_t index = sequence->next_slice;
    if (has_next_slice != (index + 1u < sequence->slice_count))
        return 0;
    if ((index == 0 && slice->slice_segment_address != 0) ||
        (index > 0 &&
         slice->slice_segment_address <= sequence->previous_slice_address) ||
        !!slice->LongSliceFlags.fields.LastSliceOfPic !=
            (index + 1u == sequence->slice_count))
        return 0;

    uint64_t expected_tile_entry_points = 0;
    if (sequence->picture->pic_fields.bits.tiles_enabled_flag) {
        if (!hobot_hevc_tile_slice_entry_point_count(
                sequence->picture, slice->slice_segment_address,
                has_next_slice, next_slice_address,
                &expected_tile_entry_points))
            return 0;
        if (slice->num_entry_point_offsets != 0 &&
            slice->num_entry_point_offsets != expected_tile_entry_points)
            return 0;
    } else if (slice->num_entry_point_offsets != 0) {
        if (!sequence->entropy_coding_sync_enabled ||
            sequence->picture_ctb_rows <= 1 ||
            sequence->entry_point_count > sequence->picture_ctb_rows - 1u ||
            slice->num_entry_point_offsets >
                sequence->picture_ctb_rows - 1u - sequence->entry_point_count)
            return 0;
    }

    if (slice->LongSliceFlags.fields.dependent_slice_segment_flag) {
        if (index == 0 || !sequence->previous_slice_type_valid ||
            slice->LongSliceFlags.fields.slice_type !=
                sequence->previous_slice_type)
            return 0;
    } else {
        sequence->previous_slice_type =
            slice->LongSliceFlags.fields.slice_type;
        sequence->previous_slice_type_valid = 1;
    }

    sequence->current_tile_entry_point_count = expected_tile_entry_points;
    if (tile_entry_point_count)
        *tile_entry_point_count = expected_tile_entry_points;
    sequence->entry_point_count += slice->num_entry_point_offsets;
    sequence->previous_slice_address = slice->slice_segment_address;
    sequence->next_slice++;
    return 1;
}

static inline int hobot_hevc_slice_sequence_add(
    HobotHevcSliceSequence *sequence,
    const VASliceParameterBufferHEVC *slice,
    size_t slice_data_buffer_size
) {
    int has_next_slice = sequence &&
        sequence->next_slice + 1u < sequence->slice_count;
    return hobot_hevc_slice_sequence_add_with_next(
        sequence, slice, slice_data_buffer_size, has_next_slice, 0, NULL);
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
    if (!br || !pic || !rps || pic->num_short_term_ref_pic_sets > 64)
        return 0;

    memset(rps, 0, sizeof(*rps));
    memset(rps->reference_index, 0xff, sizeof(rps->reference_index));
    size_t start = br->bit_pos;
    uint32_t negative_count, positive_count;
    if (pic->num_short_term_ref_pic_sets != 0) {
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

static int hobot_hevc_current_rps_covers_picture(
    const VAPictureParameterBufferHEVC *pic,
    const HobotHevcInlineRps *rps
) {
    if (!pic || !rps || rps->current_count == 0 ||
        rps->entry_count != rps->current_count)
        return 0;

    uint8_t matched[15] = {0};
    size_t reference_count = 0;
    for (size_t i = 0; i < 15; i++) {
        const VAPictureHEVC *ref = &pic->ReferenceFrames[i];
        if (ref->picture_id == VA_INVALID_SURFACE) {
            if (ref->flags != VA_PICTURE_HEVC_INVALID)
                return 0;
            continue;
        }
        uint32_t current_flags = ref->flags &
            (VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE |
             VA_PICTURE_HEVC_RPS_ST_CURR_AFTER);
        if (current_flags == 0)
            return 0;

        size_t match = rps->entry_count;
        for (size_t j = 0; j < rps->entry_count; j++) {
            if (rps->reference_index[j] == i && rps->used[j] &&
                rps->flags[j] == current_flags &&
                rps->poc[j] == ref->pic_order_cnt) {
                if (match != rps->entry_count)
                    return 0;
                match = j;
            }
        }
        if (match == rps->entry_count || matched[match])
            return 0;
        matched[match] = 1;
        reference_count++;
    }

    if (reference_count != rps->entry_count)
        return 0;
    for (size_t i = 0; i < rps->entry_count; i++) {
        if (!matched[i])
            return 0;
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

static int hobot_hevc_cra_unused_rps_matches_picture(
    const VAPictureParameterBufferHEVC *pic,
    const HobotHevcInlineRps *rps
) {
    if (!pic || !rps || rps->current_count != 0 || rps->entry_count > 15)
        return 0;

    uint8_t matched[15] = {0};
    size_t reference_count = 0;
    for (size_t i = 0; i < 15; i++) {
        const VAPictureHEVC *ref = &pic->ReferenceFrames[i];
        if (ref->picture_id == VA_INVALID_SURFACE) {
            if (ref->flags != VA_PICTURE_HEVC_INVALID)
                return 0;
            continue;
        }
        if (ref->flags != 0)
            return 0;

        size_t match = 15;
        for (size_t j = 0; j < rps->entry_count; j++) {
            if (!rps->used[j] && rps->poc[j] == ref->pic_order_cnt) {
                if (match != 15)
                    return 0;
                match = j;
            }
        }
        if (match == 15 || matched[match])
            return 0;
        matched[match] = 1;
        reference_count++;
    }

    for (size_t i = 0; i < rps->entry_count; i++) {
        if (rps->used[i] || !matched[i])
            return 0;
    }
    return reference_count == rps->entry_count;
}

static int hobot_hevc_slice_segment_tail_supported(
    const VAPictureParameterBufferHEVC *pic,
    const VASliceParameterBufferHEVC *slice,
    BitReader *br,
    uint64_t expected_tile_entry_point_count
) {
    if (!pic || !slice || !br)
        return 0;

    if (pic->pic_fields.bits.tiles_enabled_flag ||
        pic->pic_fields.bits.entropy_coding_sync_enabled_flag) {
        uint32_t num_entry_point_offsets;
        if (!br_read_ue(br, &num_entry_point_offsets, NULL, NULL) ||
            (!pic->pic_fields.bits.tiles_enabled_flag &&
             num_entry_point_offsets != slice->num_entry_point_offsets) ||
            (pic->pic_fields.bits.tiles_enabled_flag &&
             num_entry_point_offsets != expected_tile_entry_point_count) ||
            (pic->pic_fields.bits.tiles_enabled_flag &&
             slice->num_entry_point_offsets != 0 &&
             num_entry_point_offsets != slice->num_entry_point_offsets))
            return 0;
        if (num_entry_point_offsets != 0) {
            uint32_t offset_len_minus1;
            if (!br_read_ue(br, &offset_len_minus1, NULL, NULL) ||
                offset_len_minus1 > 31)
                return 0;
            uint64_t total_offset = 0;
            for (uint32_t i = 0; i < num_entry_point_offsets; i++) {
                uint32_t offset_minus1;
                if (!br_read_bits(br, offset_len_minus1 + 1u,
                                  &offset_minus1) ||
                    total_offset > UINT64_MAX -
                        (uint64_t)offset_minus1 - 1u)
                    return 0;
                total_offset += (uint64_t)offset_minus1 + 1u;
            }
            size_t payload_offset = slice->slice_data_byte_offset;
            if (payload_offset > slice->slice_data_size ||
                total_offset > slice->slice_data_size - payload_offset)
                return 0;
        }
    } else if (slice->num_entry_point_offsets != 0) {
        return 0;
    }

    uint32_t alignment_bit;
    if (!br_read_bits(br, 1, &alignment_bit) || alignment_bit != 1)
        return 0;
    while (br->bit_pos % 8u != 0) {
        uint32_t alignment_zero_bit;
        if (!br_read_bits(br, 1, &alignment_zero_bit) || alignment_zero_bit)
            return 0;
    }
    return hobot_hevc_slice_header_offset_valid(br, slice);
}

static int hobot_hevc_validated_rps_slice_supported_internal(
    const VAPictureParameterBufferHEVC *pic,
    const VASliceParameterBufferHEVC *slice,
    const uint8_t *slice_data,
    size_t slice_data_size,
    const HobotHevcSliceSequence *sequence,
    size_t *rps_syntax_start_bit,
    size_t *rps_syntax_end_bit,
    int *sps_rps_selected,
    int *dependent_slice_segment,
    HobotHevcInlineRps *validated_rps
) {
    if (rps_syntax_start_bit)
        *rps_syntax_start_bit = 0;
    if (rps_syntax_end_bit)
        *rps_syntax_end_bit = 0;
    if (sps_rps_selected)
        *sps_rps_selected = 0;
    if (dependent_slice_segment)
        *dependent_slice_segment = 0;
    if (validated_rps)
        memset(validated_rps, 0, sizeof(*validated_rps));

    if (!pic || !slice || !slice_data ||
        pic->num_short_term_ref_pic_sets > 64 ||
        !sequence || sequence->picture_ctb_count == 0 ||
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
        (!is_irap && nal_type > 1 && nal_type != 8 && nal_type != 9) ||
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
    int is_dependent_slice_segment = 0;
    if (!first_slice_segment) {
        if (pic->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag) {
            uint32_t dependent_flag;
            if (!br_read_bits(&br, 1, &dependent_flag))
                return 0;
            is_dependent_slice_segment = dependent_flag != 0;
        } else if (slice->LongSliceFlags.fields
                       .dependent_slice_segment_flag) {
            return 0;
        }
        if (!!slice->LongSliceFlags.fields.dependent_slice_segment_flag !=
            is_dependent_slice_segment)
            return 0;
        unsigned int address_bits = 0;
        uint64_t address_range = sequence->picture_ctb_count - 1u;
        while (address_range != 0) {
            address_bits++;
            address_range >>= 1;
        }
        if (!br_read_bits(&br, address_bits, &value) ||
            value != slice->slice_segment_address)
            return 0;
    } else if (slice->LongSliceFlags.fields.dependent_slice_segment_flag) {
        return 0;
    }
    if (dependent_slice_segment)
        *dependent_slice_segment = is_dependent_slice_segment;
    if (is_dependent_slice_segment)
        return hobot_hevc_slice_segment_tail_supported(
            pic, slice, &br, sequence->current_tile_entry_point_count);

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
            slice_type != 2 ||
            !br_read_bits(&br, poc_lsb_bits, &poc_lsb))
            return 0;
        uint32_t poc_mask = (1u << poc_lsb_bits) - 1u;
        if (((uint32_t)pic->CurrPic.pic_order_cnt & poc_mask) != poc_lsb)
            return 0;
        size_t cra_rps_start_bit = br.bit_pos;
        if (!br_read_bits(&br, 1, &short_term_ref_pic_set_sps_flag) ||
            short_term_ref_pic_set_sps_flag ||
            !hobot_hevc_inline_rps_supported(&br, pic, slice_type,
                                              &inline_rps) ||
            !hobot_hevc_cra_unused_rps_matches_picture(pic, &inline_rps))
            return 0;
        size_t cra_rps_end_bit = br.bit_pos;
        if (!hobot_hevc_inline_ref_pic_lists_supported(
                &br, pic, slice, slice_type, &inline_rps) ||
            !hobot_hevc_slice_header_offset_valid(&br, slice))
            return 0;
        if (pic->num_short_term_ref_pic_sets >= 2) {
            if (rps_syntax_start_bit)
                *rps_syntax_start_bit = cra_rps_start_bit;
            if (rps_syntax_end_bit)
                *rps_syntax_end_bit = cra_rps_end_bit;
            if (validated_rps)
                *validated_rps = inline_rps;
        }
        return 1;
    }

    if (pic->num_short_term_ref_pic_sets == 0 ||
        pic->num_short_term_ref_pic_sets == 1) {
        unsigned int poc_lsb_bits =
            4u + pic->log2_max_pic_order_cnt_lsb_minus4;
        uint32_t poc_lsb;
        HobotHevcInlineRps inline_rps;
        if (!br_read_bits(&br, poc_lsb_bits, &poc_lsb))
            return 0;
        uint32_t poc_mask = (1u << poc_lsb_bits) - 1u;
        if (((uint32_t)pic->CurrPic.pic_order_cnt & poc_mask) != poc_lsb)
            return 0;

        size_t selection_flag_start_bit = 0;
        size_t selection_flag_end_bit = 0;
        selection_flag_start_bit = br.bit_pos;
        if (!br_read_bits(&br, 1, &short_term_ref_pic_set_sps_flag))
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
            if (validated_rps)
                *validated_rps = inline_rps;
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
        if (rps_syntax_start_bit)
            *rps_syntax_start_bit = selection_flag_start_bit;
        if (rps_syntax_end_bit)
            *rps_syntax_end_bit = selection_flag_end_bit;
        if (sps_rps_selected)
            *sps_rps_selected = 1;
        if (validated_rps)
            *validated_rps = inline_rps;
        return 1;
    }

    /* Selected SPS-RPS indices are accepted only for validated P/B syntax. */
    if (pic->slice_parsing_fields.bits.RapPicFlag ||
        (slice_type != 0 && slice_type != 1) ||
        pic->slice_parsing_fields.bits.IntraPicFlag ||
        pic->slice_parsing_fields.bits.long_term_ref_pics_present_flag ||
        pic->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag ||
        pic->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag ||
        pic->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag ||
        pic->slice_parsing_fields.bits.slice_segment_header_extension_present_flag ||
        slice->LongSliceFlags.fields.slice_deblocking_filter_disabled_flag ||
        (slice_type == 0 && pic->pic_fields.bits.NoBiPredFlag))
        return 0;

    size_t selection_flag_start_bit = br.bit_pos;
    if (!short_term_ref_pic_set_sps_flag_read) {
        unsigned int poc_lsb_bits =
            4u + pic->log2_max_pic_order_cnt_lsb_minus4;
        uint32_t poc_lsb;
        if (!br_read_bits(&br, poc_lsb_bits, &poc_lsb))
            return 0;
        uint32_t poc_mask = (1u << poc_lsb_bits) - 1u;
        if (((uint32_t)pic->CurrPic.pic_order_cnt & poc_mask) != poc_lsb)
            return 0;
        selection_flag_start_bit = br.bit_pos;
        if (!br_read_bits(&br, 1, &short_term_ref_pic_set_sps_flag))
            return 0;
    }
    if (!short_term_ref_pic_set_sps_flag) {
        HobotHevcInlineRps inline_rps;
        if (!hobot_hevc_inline_rps_supported(
                &br, pic, slice_type, &inline_rps))
            return 0;
        size_t inline_rps_end_bit = br.bit_pos;
        if (inline_rps.current_count == 0 ||
            !hobot_hevc_inline_ref_pic_lists_supported(
                &br, pic, slice, slice_type, &inline_rps) ||
            !hobot_hevc_slice_header_offset_valid(&br, slice))
            return 0;
        if (pic->num_short_term_ref_pic_sets >= 2) {
            if (rps_syntax_start_bit)
                *rps_syntax_start_bit = selection_flag_start_bit;
            if (rps_syntax_end_bit)
                *rps_syntax_end_bit = inline_rps_end_bit;
            if (validated_rps)
                *validated_rps = inline_rps;
        }
        return 1;
    }
    if (pic->st_rps_bits != 0)
        return 0;
    unsigned int rps_index_bits = hobot_hevc_ceil_log2(
        pic->num_short_term_ref_pic_sets);
    uint32_t rps_index = 0;
    if (rps_index_bits == 0 || rps_index_bits > 6 ||
        !br_read_bits(&br, rps_index_bits, &rps_index) ||
        rps_index >= pic->num_short_term_ref_pic_sets)
        return 0;
    size_t selected_rps_end_bit = br.bit_pos;
    HobotHevcInlineRps selected_rps;
    if (!hobot_hevc_current_rps_from_picture(pic, &selected_rps) ||
        !hobot_hevc_current_rps_covers_picture(pic, &selected_rps) ||
        !hobot_hevc_inline_ref_pic_lists_supported(
            &br, pic, slice, slice_type, &selected_rps))
        return 0;
    if (rps_syntax_start_bit)
        *rps_syntax_start_bit = selection_flag_start_bit;
    if (rps_syntax_end_bit)
        *rps_syntax_end_bit = selected_rps_end_bit;
    if (sps_rps_selected)
        *sps_rps_selected = 1;
    if (validated_rps)
        *validated_rps = selected_rps;

    if (!hobot_hevc_slice_header_offset_valid(&br, slice))
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

    if (slice->LongSliceFlags.fields.slice_temporal_mvp_enabled_flag) {
        unsigned int collocated_active_count = 0;
        if (slice_type == 0) {
            if (!br_read_bits(&br, 1, &value) ||
                value != slice->LongSliceFlags.fields.collocated_from_l0_flag)
                return 0;
            collocated_active_count = value ?
                (unsigned int)slice->num_ref_idx_l0_active_minus1 + 1u :
                (unsigned int)slice->num_ref_idx_l1_active_minus1 + 1u;
        } else if (slice_type != 1 ||
                   !slice->LongSliceFlags.fields.collocated_from_l0_flag) {
            return 0;
        } else {
            collocated_active_count =
                (unsigned int)slice->num_ref_idx_l0_active_minus1 + 1u;
        }
        if (collocated_active_count > 1) {
            uint32_t collocated_ref_idx;
            if (!br_read_ue(&br, &collocated_ref_idx, NULL, NULL) ||
                collocated_ref_idx >= collocated_active_count ||
                collocated_ref_idx != slice->collocated_ref_idx)
                return 0;
        } else if (slice->collocated_ref_idx != 0) {
            return 0;
        }
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

    if (!hobot_hevc_slice_segment_tail_supported(
            pic, slice, &br, sequence->current_tile_entry_point_count))
        return 0;

    if (validated_rps)
        *validated_rps = selected_rps;
    return 1;
}

static int hobot_hevc_validated_rps_slice_supported(
    const VAPictureParameterBufferHEVC *pic,
    const VASliceParameterBufferHEVC *slice,
    const uint8_t *slice_data,
    size_t slice_data_size,
    const HobotHevcSliceSequence *sequence
) {
    return hobot_hevc_validated_rps_slice_supported_internal(
        pic, slice, slice_data, slice_data_size, sequence,
        NULL, NULL, NULL, NULL, NULL);
}

static int hobot_hevc_serialize_inline_rps(
    const VAPictureParameterBufferHEVC *pic,
    const HobotHevcInlineRps *rps,
    uint8_t *output,
    size_t output_capacity,
    size_t *output_bit_count
) {
    if (!pic || !rps || !output || !output_bit_count ||
        output_capacity > SIZE_MAX / 8u || rps->entry_count > 15u ||
        rps->current_count > 8u)
        return 0;

    uint8_t negative[15];
    uint8_t positive[15];
    size_t negative_count = 0;
    size_t positive_count = 0;
    size_t current_count = 0;
    int saw_positive = 0;
    int32_t previous_poc = 0;
    for (size_t i = 0; i < rps->entry_count; i++) {
        int64_t poc = rps->poc[i];
        int64_t current_poc = pic->CurrPic.pic_order_cnt;
        uint32_t expected_flags;
        if (poc < current_poc) {
            if (saw_positive ||
                (negative_count != 0 &&
                 rps->poc[negative[negative_count - 1u]] <= poc))
                return 0;
            negative[negative_count++] = (uint8_t)i;
            expected_flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
        } else if (poc > current_poc) {
            saw_positive = 1;
            if (positive_count != 0 &&
                rps->poc[positive[positive_count - 1u]] >= poc)
                return 0;
            positive[positive_count++] = (uint8_t)i;
            expected_flags = VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
        } else {
            return 0;
        }
        if (rps->used[i] > 1u ||
            rps->flags[i] != (rps->used[i] ? expected_flags : 0u))
            return 0;
        current_count += rps->used[i];
    }
    if (negative_count + positive_count != rps->entry_count ||
        current_count != rps->current_count)
        return 0;

    memset(output, 0, output_capacity);
    size_t bit_pos = 0;
    size_t capacity_bits = output_capacity * 8u;
    if (!hobot_hevc_write_bit(output, capacity_bits, &bit_pos, 0) ||
        !hobot_hevc_write_ue(output, capacity_bits, &bit_pos,
                             (uint32_t)negative_count) ||
        !hobot_hevc_write_ue(output, capacity_bits, &bit_pos,
                             (uint32_t)positive_count))
        return 0;

    previous_poc = pic->CurrPic.pic_order_cnt;
    for (size_t i = 0; i < negative_count; i++) {
        int64_t delta = (int64_t)previous_poc -
                        rps->poc[negative[i]] - 1;
        if (delta < 0 || delta > UINT32_MAX ||
            !hobot_hevc_write_ue(output, capacity_bits, &bit_pos,
                                 (uint32_t)delta) ||
            !hobot_hevc_write_bit(output, capacity_bits, &bit_pos,
                                  rps->used[negative[i]]))
            return 0;
        previous_poc = rps->poc[negative[i]];
    }

    previous_poc = pic->CurrPic.pic_order_cnt;
    for (size_t i = 0; i < positive_count; i++) {
        int64_t delta = (int64_t)rps->poc[positive[i]] -
                        previous_poc - 1;
        if (delta < 0 || delta > UINT32_MAX ||
            !hobot_hevc_write_ue(output, capacity_bits, &bit_pos,
                                 (uint32_t)delta) ||
            !hobot_hevc_write_bit(output, capacity_bits, &bit_pos,
                                  rps->used[positive[i]]))
            return 0;
        previous_poc = rps->poc[positive[i]];
    }

    *output_bit_count = bit_pos;
    return 1;
}

static int hobot_hevc_rewrite_rps_slice_with_entry_points(
    const VAPictureParameterBufferHEVC *pic,
    const VASliceParameterBufferHEVC *slice,
    const uint8_t *slice_data,
    size_t slice_data_size,
    uint64_t expected_tile_entry_point_count,
    uint8_t **rewritten_data,
    size_t *rewritten_size
) {
    if (!rewritten_data || !rewritten_size)
        return -1;
    *rewritten_data = NULL;
    *rewritten_size = 0;
    if (!pic || pic->num_short_term_ref_pic_sets == 0 ||
        pic->num_short_term_ref_pic_sets > 64)
        return 0;

    HobotHevcSliceSequence sequence;
    if (!hobot_hevc_slice_sequence_init(pic, 1, &sequence))
        return -1;
    sequence.current_tile_entry_point_count =
        expected_tile_entry_point_count;
    size_t rps_syntax_start = 0;
    size_t rps_syntax_end = 0;
    int selected_sps_rps = 0;
    int dependent_slice_segment = 0;
    HobotHevcInlineRps validated_rps;
    if (!hobot_hevc_validated_rps_slice_supported_internal(
            pic, slice, slice_data, slice_data_size, &sequence,
            &rps_syntax_start, &rps_syntax_end, &selected_sps_rps,
            &dependent_slice_segment, &validated_rps))
        return -1;
    if (dependent_slice_segment)
        return 0;
    if (pic->num_short_term_ref_pic_sets == 1 && !selected_sps_rps)
        return 0;
    if (pic->num_short_term_ref_pic_sets >= 2 &&
        rps_syntax_end <= rps_syntax_start)
        return 0;

    size_t replace_start = rps_syntax_end;
    size_t replace_end = rps_syntax_end;
    uint8_t replacement_bits[256] = {0};
    size_t replacement_bit_count = 1;
    if (pic->num_short_term_ref_pic_sets >= 2) {
        if (rps_syntax_end <= rps_syntax_start)
            return -1;
        replace_start = rps_syntax_start;
        replace_end = rps_syntax_end;
        if (!hobot_hevc_serialize_inline_rps(
                pic, &validated_rps, replacement_bits,
                sizeof(replacement_bits), &replacement_bit_count))
            return -1;
    }

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
        replace_start > header_bytes * 8u ||
        replace_end > header_bytes * 8u) {
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
    if (alignment_bit == SIZE_MAX || alignment_bit < replace_end ||
        ((alignment_bit + 1u + 7u) & ~(size_t)7u) != header_bits) {
        free(rbsp);
        return -1;
    }

    size_t suffix_bits = alignment_bit - replace_end;
    if (replace_start > SIZE_MAX - replacement_bit_count ||
        replace_start + replacement_bit_count > SIZE_MAX - suffix_bits - 1u) {
        free(rbsp);
        return -1;
    }
    size_t rewritten_header_bits =
        replace_start + replacement_bit_count + suffix_bits + 1u;
    if (rewritten_header_bits > SIZE_MAX - 7u) {
        free(rbsp);
        return -1;
    }
    size_t rewritten_header_bytes = (rewritten_header_bits + 7u) / 8u;
    size_t payload_size = rbsp_size - header_bytes;
    if (payload_size > SIZE_MAX - rewritten_header_bytes) {
        free(rbsp);
        return -1;
    }
    size_t rewritten_rbsp_capacity = rewritten_header_bytes + payload_size;
    if (rewritten_rbsp_capacity == 0)
        rewritten_rbsp_capacity = 1;
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
                                  &dst_bit, rbsp, 0, replace_start) &&
             hobot_hevc_copy_bits(rewritten_rbsp,
                                  rewritten_bit_capacity,
                                  &dst_bit, replacement_bits, 0,
                                  replacement_bit_count) &&
             hobot_hevc_copy_bits(rewritten_rbsp,
                                  rewritten_bit_capacity,
                                  &dst_bit, rbsp, replace_end,
                                  alignment_bit - replace_end) &&
             hobot_hevc_write_bit(rewritten_rbsp,
                                  rewritten_bit_capacity,
                                  &dst_bit, 1);
    while (ok && dst_bit % 8u != 0)
        ok = hobot_hevc_write_bit(rewritten_rbsp,
                                  rewritten_bit_capacity,
                                  &dst_bit, 0);
    rewritten_header_bytes = dst_bit / 8u;
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

static inline int hobot_hevc_rewrite_rps_slice(
    const VAPictureParameterBufferHEVC *pic,
    const VASliceParameterBufferHEVC *slice,
    const uint8_t *slice_data,
    size_t slice_data_size,
    uint8_t **rewritten_data,
    size_t *rewritten_size
) {
    uint64_t expected_tile_entry_point_count = 0;
    if (pic && pic->pic_fields.bits.tiles_enabled_flag &&
        (!slice || !hobot_hevc_tile_slice_entry_point_count(
            pic, slice->slice_segment_address, 0, 0,
            &expected_tile_entry_point_count)))
        return -1;
    return hobot_hevc_rewrite_rps_slice_with_entry_points(
        pic, slice, slice_data, slice_data_size,
        expected_tile_entry_point_count, rewritten_data, rewritten_size);
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
           a->pic_fields.bits.entropy_coding_sync_enabled_flag ==
               b->pic_fields.bits.entropy_coding_sync_enabled_flag &&
           a->pic_fields.bits.cu_qp_delta_enabled_flag ==
               b->pic_fields.bits.cu_qp_delta_enabled_flag &&
           a->scc_fields.value == b->scc_fields.value;
}

static int hobot_hevc_encode_slice_supported(
    const VAEncSequenceParameterBufferHEVC *seq,
    const VAEncPictureParameterBufferHEVC *pic,
    const VAEncSliceParameterBufferHEVC *slice,
    int context_width,
    int context_height,
    unsigned int rate_control
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
        slice->max_num_merge_cand != 5 ||
        (slice->slice_qp_delta != 0 && rate_control != VA_RC_CQP) ||
        (int)pic->pic_init_qp + slice->slice_qp_delta < 0 ||
        (int)pic->pic_init_qp + slice->slice_qp_delta > 51 ||
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

static int generate_h264_pps(const VAPictureParameterBufferH264 *pic,
                             unsigned int default_l0_active_minus1,
                             unsigned int default_l1_active_minus1,
                             uint8_t *out, int max_len) {
    uint8_t rbsp[256] = {0};
    BitWriter bw = {rbsp, 0};

    bw_put_ue(&bw, 0);         // pic_parameter_set_id
    bw_put_ue(&bw, 0);         // seq_parameter_set_id
    bw_put_bits(&bw, pic->pic_fields.bits.entropy_coding_mode_flag, 1);
    bw_put_bits(&bw, pic->pic_fields.bits.pic_order_present_flag, 1);
    bw_put_ue(&bw, 0);         // num_slice_groups_minus1
    bw_put_ue(&bw, default_l0_active_minus1); // VA-API omits the PPS defaults
    bw_put_ue(&bw, default_l1_active_minus1);
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

static int hevc_scaling_matrix_is_default(
    const VAIQMatrixBufferHEVC *matrix) {
    static const uint8_t default_intra[64] = {
        16, 16, 16, 16, 17, 18, 21, 24,
        16, 16, 16, 16, 17, 19, 22, 25,
        16, 16, 17, 18, 20, 22, 25, 29,
        16, 16, 18, 21, 24, 27, 31, 36,
        17, 17, 20, 24, 30, 35, 41, 47,
        18, 19, 22, 27, 35, 44, 54, 65,
        21, 22, 25, 31, 41, 54, 70, 88,
        24, 25, 29, 36, 47, 65, 88, 115,
    };
    static const uint8_t default_inter[64] = {
        16, 16, 16, 16, 17, 18, 20, 24,
        16, 16, 16, 17, 18, 20, 24, 25,
        16, 16, 17, 18, 20, 24, 25, 28,
        16, 17, 18, 20, 24, 25, 28, 33,
        17, 18, 20, 24, 25, 28, 33, 41,
        18, 20, 24, 25, 28, 33, 41, 54,
        20, 24, 25, 28, 33, 41, 54, 71,
        24, 25, 28, 33, 41, 54, 71, 91,
    };

    if (!matrix)
        return 0;

    for (unsigned int matrix_id = 0; matrix_id < 6; matrix_id++) {
        for (unsigned int i = 0; i < 16; i++)
            if (matrix->ScalingList4x4[matrix_id][i] != 16)
                return 0;

        const uint8_t *expected = matrix_id < 3 ? default_intra : default_inter;
        if (memcmp(matrix->ScalingList8x8[matrix_id], expected, 64) != 0 ||
            memcmp(matrix->ScalingList16x16[matrix_id], expected, 64) != 0 ||
            matrix->ScalingListDC16x16[matrix_id] != 16)
            return 0;
    }

    return memcmp(matrix->ScalingList32x32[0], default_intra, 64) == 0 &&
           memcmp(matrix->ScalingList32x32[1], default_inter, 64) == 0 &&
           matrix->ScalingListDC32x32[0] == 16 &&
           matrix->ScalingListDC32x32[1] == 16;
}

static int generate_hevc_sps(const VAPictureParameterBufferHEVC *pic,
                             int profile_idc,
                             const VAIQMatrixBufferHEVC *iq_matrix,
                             uint8_t *out, int max_len) {
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
    if (pic->pic_fields.bits.scaling_list_enabled_flag) {
        if (!hevc_scaling_matrix_is_default(iq_matrix))
            return 0;
        bw_put_bit(&bw, 0);       // sps_scaling_list_data_present_flag
    }
    bw_put_bit(&bw, pic->pic_fields.bits.amp_enabled_flag);
    bw_put_bit(&bw, pic->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag);
    bw_put_bit(&bw, pic->pic_fields.bits.pcm_enabled_flag);
    if (pic->pic_fields.bits.pcm_enabled_flag) {
        bw_put_bits(&bw, pic->pcm_sample_bit_depth_luma_minus1, 4);
        bw_put_bits(&bw, pic->pcm_sample_bit_depth_chroma_minus1, 4);
        bw_put_ue(&bw, pic->log2_min_pcm_luma_coding_block_size_minus3);
        bw_put_ue(&bw, pic->log2_diff_max_min_pcm_luma_coding_block_size);
        bw_put_bit(&bw, pic->pic_fields.bits.pcm_loop_filter_disabled_flag);
    }

    if (pic->num_short_term_ref_pic_sets >= 2) {
        bw_put_ue(&bw, 0);
    } else if (pic->num_short_term_ref_pic_sets == 1) {
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

static int generate_hevc_pps(const VAPictureParameterBufferHEVC *pic,
                             uint8_t *out,
                             int max_len) {
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
    if (pic->pic_fields.bits.tiles_enabled_flag) {
        bw_put_ue(&bw, pic->num_tile_columns_minus1);
        bw_put_ue(&bw, pic->num_tile_rows_minus1);
        int uniform_spacing = hobot_hevc_tile_grid_is_uniform(pic);
        bw_put_bit(&bw, uniform_spacing);
        if (!uniform_spacing) {
            for (size_t i = 0; i < pic->num_tile_columns_minus1; i++)
                bw_put_ue(&bw, pic->column_width_minus1[i]);
            for (size_t i = 0; i < pic->num_tile_rows_minus1; i++)
                bw_put_ue(&bw, pic->row_height_minus1[i]);
        }
        bw_put_bit(&bw, pic->pic_fields.bits.loop_filter_across_tiles_enabled_flag);
    }
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
    uint32_t cpu_access_count;
    uint32_t va_lock_count;
    uint32_t external_handle_count;
    int cpu_cache_flush_pending;
    VAContextID context_id;
    VAContextID output_context_id;
    uint32_t context_usage_mask;
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
    int handle_acquired;
    int acquired_fd;
    void *data;
    VACodedBufferSegment coded_segment;
} HobotBuffer;

static int hobot_buffer_is_single_record(const HobotBuffer *buffer,
                                         size_t record_size) {
    return buffer && buffer->data && buffer->num_elements == 1 &&
           buffer->element_size >= record_size &&
           buffer->size >= buffer->element_size;
}

/* Internal Context Object */
typedef struct {
    VAContextID id;
    int allocated;
    int is_encoder;
    VAProfile profile;
    unsigned int rate_control;
    int width;
    int height;
    int encoder_init_deferred;
    uint32_t encoder_max_bitrate_kbps;
    uint32_t encoder_applied_max_bitrate_kbps;
    int jpeg_init_deferred;
    int jpeg_rotation_fixed;
    uint32_t jpeg_rotation;
    int h264_sequence_valid;
    int h264_constrained_baseline_headers_patched;
    VAEncSequenceParameterBufferH264 h264_sequence;
    int hevc_encode_sequence_valid;
    VAEncSequenceParameterBufferHEVC hevc_encode_sequence;
    int hevc_encode_picture_valid;
    VAEncPictureParameterBufferHEVC hevc_encode_picture;
    int hevc_encode_pic_qp_valid;
    uint8_t hevc_encode_pic_qp;
    int hevc_encode_slice_qp_delta_valid;
    int8_t hevc_encode_slice_qp_delta;
    int hevc_sps_geometry_seen;
    int decode_picture_active;
    int decode_slice_fragment_open;
    int h264_encode_slice_valid;
    uint8_t h264_encode_slice_type;
    int h264_encode_pic_qp_valid;
    uint8_t h264_encode_pic_qp;
    int8_t h264_encode_slice_qp_delta;
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
    VAPictureParameterBufferH264 h264_decode_picture;
    int h264_decode_picture_valid;
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
    hb_mem_graphic_buf_t enc_scratch_buf;
    int enc_scratch_allocated;
    _Atomic uintptr_t enc_external_input_pending;
    VASurfaceID enc_external_surface;
    int enc_external_enabled;
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
    int resources_terminated;
    HobotConfig  configs[MAX_CONFIGS];
    HobotSurface surfaces[MAX_SURFACES];
    HobotBuffer  buffers[MAX_BUFFERS];
    HobotContext contexts[MAX_CONTEXTS];
    HobotImage   images[MAX_IMAGES];
    int orphaned_import_fds[MAX_SURFACES];
    unsigned int orphaned_import_count;
    unsigned int orphaned_import_reserved;
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
        if (hctx->allocated && hctx->is_encoder &&
            ((hctx->encoder_picture_active &&
              hctx->current_render_target == surface) ||
             (atomic_load_explicit(&hctx->enc_external_input_pending,
                                   memory_order_acquire) &&
              hctx->enc_external_surface == surface)))
            return 1;
    }
    return 0;
}

static int hobot_context_surface_used_by_other_encoder(
    const HobotDriverData *drv,
    VAContextID context) {
    for (int s = 1; s < MAX_SURFACES; s++) {
        const HobotSurface *surf = &drv->surfaces[s];
        if (!surf->allocated ||
            (surf->context_id != context && surf->output_context_id != context))
            continue;
        for (int i = 1; i < MAX_CONTEXTS; i++) {
            const HobotContext *hctx = &drv->contexts[i];
            if ((VAContextID)i == context || !hctx->allocated ||
                !hctx->is_encoder)
                continue;
            if ((hctx->encoder_picture_active &&
                 hctx->current_render_target == (VASurfaceID)s) ||
                (atomic_load_explicit(&hctx->enc_external_input_pending,
                                      memory_order_acquire) &&
                 hctx->enc_external_surface == (VASurfaceID)s))
                return 1;
        }
    }
    return 0;
}

static int hobot_buffer_has_active_encoder(const HobotDriverData *drv,
                                           VABufferID buffer) {
    for (int i = 1; i < MAX_CONTEXTS; i++) {
        const HobotContext *hctx = &drv->contexts[i];
        if (hctx->allocated && hctx->is_encoder &&
            hctx->encoder_picture_active && hctx->enc_coded_buf == buffer)
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
        unsigned int slice_type = slice->slice_type % 5u;
        if (slice->slice_type > 9 || slice_type > 2)
            return 0;

        if (slice->slice_data_flag == VA_SLICE_DATA_FLAG_ALL ||
            slice->slice_data_flag == VA_SLICE_DATA_FLAG_BEGIN) {
            size_t nal_header_offset = offset;
            const uint8_t *bytes = (const uint8_t *)data->data;
            if (size >= 4 && bytes[offset] == 0 && bytes[offset + 1] == 0 &&
                bytes[offset + 2] == 0 && bytes[offset + 3] == 1)
                nal_header_offset += 4;
            else if (size >= 3 && bytes[offset] == 0 && bytes[offset + 1] == 0 &&
                     bytes[offset + 2] == 1)
                nal_header_offset += 3;
            if (nal_header_offset >= offset + size)
                return 0;

            uint8_t nal_header = bytes[nal_header_offset];
            unsigned int nal_unit_type = nal_header & 0x1fu;
            if ((nal_header & 0x80u) != 0 ||
                (nal_unit_type != 1u && nal_unit_type != 5u))
                return 0;
        }

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

static int hobot_h264_slice_ref_override_flags(
    const VAPictureParameterBufferH264 *picture,
    const VASliceParameterBufferH264 *slice,
    const HobotBuffer *data,
    unsigned int *slice_type_out,
    uint32_t *l0_override_out,
    uint32_t *l1_override_out)
{
    if (!picture || !slice || !data || !data->data || !slice_type_out ||
        !l0_override_out || !l1_override_out ||
        !picture->seq_fields.bits.frame_mbs_only_flag ||
        picture->pic_fields.bits.field_pic_flag ||
        (picture->CurrPic.flags &
         (VA_PICTURE_H264_TOP_FIELD | VA_PICTURE_H264_BOTTOM_FIELD)) != 0 ||
        slice->slice_data_size == 0 ||
        slice->slice_data_offset > data->size ||
        slice->slice_data_size > data->size - slice->slice_data_offset)
        return 0;

    size_t nal_offset = slice->slice_data_offset;
    size_t nal_size = slice->slice_data_size;
    const uint8_t *bytes = (const uint8_t *)data->data;
    if (nal_size >= 4u && bytes[nal_offset] == 0 &&
        bytes[nal_offset + 1u] == 0 && bytes[nal_offset + 2u] == 0 &&
        bytes[nal_offset + 3u] == 1) {
        nal_offset += 4u;
        nal_size -= 4u;
    } else if (nal_size >= 3u && bytes[nal_offset] == 0 &&
               bytes[nal_offset + 1u] == 0 && bytes[nal_offset + 2u] == 1) {
        nal_offset += 3u;
        nal_size -= 3u;
    }
    if (nal_size < 2u)
        return 0;

    uint8_t nal_header = bytes[nal_offset];
    unsigned int nal_unit_type = nal_header & 0x1fu;
    if ((nal_header & 0x80u) != 0 ||
        (nal_unit_type != 1u && nal_unit_type != 5u))
        return 0;

    uint8_t rbsp[16384];
    size_t ebsp_prefix_size = nal_size < sizeof(rbsp) ? nal_size : sizeof(rbsp);
    size_t rbsp_size = 0;
    if (!hobot_hevc_unescape_rbsp(bytes + nal_offset, ebsp_prefix_size,
                                  rbsp, sizeof(rbsp), &rbsp_size) ||
        rbsp_size < 2u || rbsp_size > SIZE_MAX / 8u)
        return 0;

    BitReader br = {rbsp, rbsp_size * 8u, 8u};
    uint32_t first_mb_in_slice, parsed_slice_type, pps_id;
    if (!br_read_ue(&br, &first_mb_in_slice, NULL, NULL) ||
        first_mb_in_slice != slice->first_mb_in_slice ||
        !br_read_ue(&br, &parsed_slice_type, NULL, NULL) ||
        parsed_slice_type > 9u || slice->slice_type > 9u ||
        parsed_slice_type % 5u != slice->slice_type % 5u ||
        !br_read_ue(&br, &pps_id, NULL, NULL) || pps_id != 0u)
        return 0;

    uint32_t ignored;
    unsigned int frame_num_bits =
        picture->seq_fields.bits.log2_max_frame_num_minus4 + 4u;
    if (frame_num_bits > 16u || !br_read_bits(&br, frame_num_bits, &ignored))
        return 0;

    if (nal_unit_type == 5u && !br_read_ue(&br, &ignored, NULL, NULL))
        return 0;

    if (picture->seq_fields.bits.pic_order_cnt_type == 0) {
        unsigned int poc_bits =
            picture->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4u;
        if (poc_bits > 16u || !br_read_bits(&br, poc_bits, &ignored))
            return 0;
        if (picture->pic_fields.bits.pic_order_present_flag) {
            int32_t delta_pic_order_bottom;
            if (!br_read_se(&br, &delta_pic_order_bottom))
                return 0;
        }
    } else if (picture->seq_fields.bits.pic_order_cnt_type != 2u) {
        return 0;
    }
    if (picture->pic_fields.bits.redundant_pic_cnt_present_flag)
        return 0;

    unsigned int slice_type = parsed_slice_type % 5u;
    if (slice_type == 1u) {
        uint32_t direct_spatial_mv_pred_flag;
        if (!br_read_bits(&br, 1, &direct_spatial_mv_pred_flag) ||
            direct_spatial_mv_pred_flag != slice->direct_spatial_mv_pred_flag)
            return 0;
    }

    uint32_t l0_override = 0;
    uint32_t l1_override = 0;
    if (slice_type == 0u || slice_type == 1u) {
        if (!br_read_bits(&br, 1, &l0_override))
            return 0;
        if (l0_override) {
            uint32_t active_l0;
            if (!br_read_ue(&br, &active_l0, NULL, NULL) || active_l0 > 31u ||
                active_l0 != slice->num_ref_idx_l0_active_minus1)
                return 0;
            if (slice_type == 1u) {
                uint32_t active_l1;
                if (!br_read_ue(&br, &active_l1, NULL, NULL) || active_l1 > 31u ||
                    active_l1 != slice->num_ref_idx_l1_active_minus1)
                    return 0;
                l1_override = 1;
            }
        }
    }

    if (slice->slice_data_bit_offset != 0 &&
        br.bit_pos > slice->slice_data_bit_offset)
        return 0;

    *slice_type_out = slice_type;
    *l0_override_out = l0_override;
    *l1_override_out = l1_override;
    return 1;
}

/* Infer the synthesized PPS defaults only from slice headers that inherit them. */
static int hobot_h264_default_reference_counts(
    const HobotDriverData *drv,
    const VAPictureParameterBufferH264 *picture,
    const VABufferID *slice_param_ids,
    int slice_param_count,
    const VABufferID *slice_data_ids,
    int slice_data_count,
    unsigned int *default_l0_active_minus1,
    unsigned int *default_l1_active_minus1,
    int *slice_headers_present)
{
    if (!drv || !picture || !slice_param_ids || slice_param_count <= 0 ||
        !slice_data_ids || slice_data_count <= 0 ||
        !default_l0_active_minus1 || !default_l1_active_minus1 ||
        !slice_headers_present || slice_param_count > MAX_BUFFERS ||
        slice_data_count > MAX_BUFFERS)
        return 0;
    *slice_headers_present = 0;

    unsigned int l0_value = 0;
    unsigned int l1_value = 0;
    int have_l0_value = 0;
    int have_l1_value = 0;
    int packed_slices = slice_param_count == 1 && slice_data_count == 1;
    if (!packed_slices && slice_param_count != slice_data_count)
        return 0;

    for (int i = 0; i < slice_param_count; i++) {
        VABufferID params_id = slice_param_ids[i];
        VABufferID data_id = slice_data_ids[packed_slices ? 0 : i];
        if (params_id == 0 || params_id == VA_INVALID_ID || params_id >= MAX_BUFFERS ||
            data_id == 0 || data_id == VA_INVALID_ID || data_id >= MAX_BUFFERS)
            return 0;
        const HobotBuffer *params = &drv->buffers[params_id];
        const HobotBuffer *data = &drv->buffers[data_id];
        if (!params->allocated || params->type != VASliceParameterBufferType ||
            !params->data ||
            params->element_size < sizeof(VASliceParameterBufferH264) ||
            params->num_elements == 0 ||
            params->num_elements > params->size / params->element_size ||
            !data->allocated || data->type != VASliceDataBufferType || !data->data ||
            (!packed_slices && params->num_elements != 1))
            return 0;

        for (unsigned int j = 0; j < params->num_elements; j++) {
            const VASliceParameterBufferH264 *slice =
                (const VASliceParameterBufferH264 *)
                    ((const uint8_t *)params->data +
                     (size_t)j * params->element_size);
            if (slice->slice_data_flag != VA_SLICE_DATA_FLAG_ALL &&
                slice->slice_data_flag != VA_SLICE_DATA_FLAG_BEGIN)
                continue;

            unsigned int slice_type;
            uint32_t l0_override, l1_override;
            if (!hobot_h264_slice_ref_override_flags(
                    picture, slice, data, &slice_type,
                    &l0_override, &l1_override))
                return 0;
            *slice_headers_present = 1;

            if ((slice_type == 0u || slice_type == 1u) && !l0_override) {
                unsigned int current_l0 = slice->num_ref_idx_l0_active_minus1;
                if (current_l0 > 31u ||
                    (have_l0_value && current_l0 != l0_value)) {
                    va_trace("H.264 slices disagree on inherited L0 reference count: previous=%u current=%u",
                             l0_value, current_l0);
                    return 0;
                }
                l0_value = current_l0;
                have_l0_value = 1;
            }

            if (slice_type == 1u && !l1_override) {
                unsigned int current_l1 = slice->num_ref_idx_l1_active_minus1;
                if (current_l1 > 31u ||
                    (have_l1_value && current_l1 != l1_value)) {
                    va_trace("H.264 slices disagree on inherited L1 reference count: previous=%u current=%u",
                             l1_value, current_l1);
                    return 0;
                }
                l1_value = current_l1;
                have_l1_value = 1;
            }
        }
    }

    *default_l0_active_minus1 = have_l0_value ? l0_value : 0u;
    *default_l1_active_minus1 = have_l1_value ? l1_value : 0u;
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

static void hobot_encoder_input_consumed(hb_ptr userdata,
                                        media_codec_buffer_t *buffer) {
    HobotContext *hctx = (HobotContext *)userdata;
    if (!hctx || !buffer)
        return;
    uintptr_t token = (uintptr_t)buffer->user_ptr;
    if (token == 0)
        return;
    uintptr_t expected = token;
    VAContextID context_id = hctx->id;
    int src_idx = buffer->vframe_buf.src_idx;
    if (atomic_compare_exchange_strong_explicit(
            &hctx->enc_external_input_pending, &expected, 0,
            memory_order_acq_rel, memory_order_acquire)) {
        va_trace("external encoder input consumed: ctx=%u src_idx=%d token=%lu",
                 context_id, src_idx, (unsigned long)token);
    } else {
        va_trace("stale external encoder callback ignored: ctx=%u src_idx=%d token=%lu pending=%lu",
                 context_id, src_idx, (unsigned long)token,
                 (unsigned long)expected);
    }
}

static uintptr_t hobot_next_external_input_token(void) {
    uintptr_t token;
    do {
        token = atomic_fetch_add_explicit(&va_external_input_token_sequence,
                                          1, memory_order_relaxed) + 1;
    } while (token == 0);
    return token;
}

static int hobot_wait_encoder_input_consumed(HobotContext *hctx,
                                             uintptr_t token,
                                             uint64_t timeout_ns) {
    if (!hctx || token == 0)
        return -1;

    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0 ||
        deadline.tv_sec < 0)
        return -1;
    uint64_t seconds = timeout_ns / 1000000000ULL;
    uint64_t nanoseconds = timeout_ns % 1000000000ULL;
    if (seconds > (uint64_t)(INT64_MAX - (int64_t)deadline.tv_sec))
        return -1;
    deadline.tv_sec += (time_t)seconds;
    deadline.tv_nsec += (long)nanoseconds;
    if (deadline.tv_nsec >= 1000000000L) {
        if (deadline.tv_sec == (time_t)INT64_MAX)
            return -1;
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    for (;;) {
        uintptr_t pending = atomic_load_explicit(
            &hctx->enc_external_input_pending, memory_order_acquire);
        if (pending == 0)
            return 0;
        if (pending != token)
            return -1;

        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            return -1;
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
            return -1;

        struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000L };
        if (nanosleep(&pause, NULL) != 0 && errno != EINTR)
            return -1;
    }
}

static int hobot_finish_external_encoder_input(HobotContext *hctx,
                                               VASurfaceID surface,
                                               uintptr_t token) {
    if (!hctx || !hctx->enc_external_enabled)
        return 0;
    if (hobot_wait_encoder_input_consumed(hctx, token, 2000000000ULL) != 0) {
        fprintf(stderr,
                "[HOBOT-VA] external encoder input consumption timed out for ctx=%u surface=%u\n",
                hctx->id, surface);
        return -1;
    }
    if (hctx->enc_external_surface == surface)
        hctx->enc_external_surface = VA_INVALID_SURFACE;
    return 0;
}

static int hobot_register_encoder_input_listener(HobotContext *hctx) {
    if (!hctx || !hctx->enc_external_enabled)
        return 0;
    media_codec_callback_t callback;
    memset(&callback, 0, sizeof(callback));
    callback.on_input_buffer_consumed = hobot_encoder_input_consumed;
    return hb_mm_mc_set_input_buffer_listener(&hctx->vpu_ctx, &callback, hctx);
}

static int hobot_allocate_encoder_scratch(HobotContext *hctx) {
    if (!hctx || !hctx->enc_external_enabled)
        return 0;
    if (hctx->enc_scratch_allocated)
        return 0;

    int width = hctx->vpu_ctx.video_enc_params.width;
    int height = hctx->vpu_ctx.video_enc_params.height;
    if (width < 2 || height < 2 || (width & 1) || (height & 1) ||
        width > INT_MAX - 63 || height > INT_MAX - 63)
        return -1;

    int aligned_width = (width + 63) & ~63;
    int aligned_height = (height + 63) & ~63;
    int64_t flags = HB_MEM_USAGE_MAP_INITIALIZED |
                    HB_MEM_USAGE_CPU_READ_OFTEN |
                    HB_MEM_USAGE_CPU_WRITE_OFTEN |
                    HB_MEM_USAGE_HW_VIDEO_CODEC |
                    HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF |
                    HB_MEM_USAGE_CACHED;
    hb_mem_graphic_buf_t buffer;
    memset(&buffer, 0, sizeof(buffer));
    for (size_t i = 0; i < MAX_GRAPHIC_BUF_COMP; i++)
        buffer.fd[i] = -1;

    int ret = hb_mem_alloc_graph_buf(width, height, MEM_PIX_FMT_NV12, flags,
                                     aligned_width, aligned_height, &buffer);
    if (buffer.fd[0] >= 0) {
        hctx->enc_scratch_buf = buffer;
        hctx->enc_scratch_allocated = 1;
    }
    uint64_t y_extent = buffer.stride > 0 && buffer.vstride > 0 ?
        (uint64_t)(uint32_t)buffer.stride * (uint32_t)buffer.vstride : 0;
    uint64_t uv_extent = buffer.stride > 0 && buffer.vstride > 0 ?
        (uint64_t)(uint32_t)buffer.stride * ((uint32_t)buffer.vstride / 2u) : 0;
    uint64_t phys_delta = buffer.phys_addr[1] > buffer.phys_addr[0] ?
        buffer.phys_addr[1] - buffer.phys_addr[0] : 0;
    uintptr_t virt_y = (uintptr_t)buffer.virt_addr[0];
    uintptr_t virt_uv = (uintptr_t)buffer.virt_addr[1];
    uint64_t virt_delta = virt_uv > virt_y ? (uint64_t)(virt_uv - virt_y) : 0;
    uint64_t uv_offset = buffer.offset[1] > 0 ? buffer.offset[1] : phys_delta;
    uint64_t y_offset = buffer.offset[0];
    uint64_t object_size = 0;
    if (y_offset <= UINT64_MAX - buffer.size[0] &&
        uv_offset <= UINT64_MAX - buffer.size[1]) {
        uint64_t y_end = y_offset + buffer.size[0];
        uint64_t uv_end = uv_offset + buffer.size[1];
        object_size = y_end > uv_end ? y_end : uv_end;
    }
    if (ret != 0 || buffer.fd[0] < 0 || !buffer.is_contig ||
        (buffer.fd[1] >= 0 && buffer.fd[1] != buffer.fd[0]) ||
        !buffer.virt_addr[0] || !buffer.virt_addr[1] ||
        buffer.phys_addr[0] == 0 || buffer.phys_addr[1] == 0 ||
        buffer.stride < width || buffer.vstride < height ||
        (buffer.stride & 1) || (buffer.vstride & 1) ||
        buffer.size[0] < y_extent || buffer.size[1] < uv_extent ||
        y_offset > UINT64_MAX - y_extent ||
        uv_offset < y_offset + y_extent || phys_delta != uv_offset ||
        virt_delta != uv_offset || object_size == 0 ||
        uv_offset > UINT64_MAX - uv_extent || uv_offset + uv_extent > object_size ||
        object_size > UINT32_MAX) {
        fprintf(stderr,
                "[HOBOT-VA] external encoder scratch allocation/layout invalid: ctx=%u ret=%d fd=%d/%d stride=%d vstride=%d size=%llu/%llu offset=%llu phys=%llx/%llx virt=%p/%p contig=%d planes=%d\n",
                hctx->id, ret, buffer.fd[0], buffer.fd[1],
                buffer.stride, buffer.vstride,
                (unsigned long long)buffer.size[0],
                (unsigned long long)buffer.size[1],
                (unsigned long long)buffer.offset[1],
                (unsigned long long)buffer.phys_addr[0],
                (unsigned long long)buffer.phys_addr[1],
                buffer.virt_addr[0], buffer.virt_addr[1],
                buffer.is_contig, buffer.plane_cnt);
        return -1;
    }
    return 0;
}

static int hobot_free_encoder_scratch(HobotContext *hctx) {
    if (!hctx || !hctx->enc_scratch_allocated)
        return 0;
    int fd = hctx->enc_scratch_buf.fd[0];
    if (fd < 0)
        return -1;
    int ret = hb_mem_free_buf(fd);
    if (ret != 0)
        return ret;
    memset(&hctx->enc_scratch_buf, 0, sizeof(hctx->enc_scratch_buf));
    for (size_t i = 0; i < MAX_GRAPHIC_BUF_COMP; i++)
        hctx->enc_scratch_buf.fd[i] = -1;
    hctx->enc_scratch_allocated = 0;
    return 0;
}

static void hobot_finish_encoder_picture(HobotContext *hctx, int failed) {
    hctx->encoder_picture_active = 0;
    hctx->h264_encode_slice_valid = 0;
    hctx->h264_encode_slice_type = 0;
    hctx->h264_encode_pic_qp_valid = 0;
    hctx->h264_encode_pic_qp = 0;
    hctx->h264_encode_slice_qp_delta = 0;
    hctx->hevc_encode_pic_qp_valid = 0;
    hctx->hevc_encode_pic_qp = 0;
    hctx->hevc_encode_slice_qp_delta_valid = 0;
    hctx->hevc_encode_slice_qp_delta = 0;
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

    uint32_t previous_max_bitrate_kbps =
        hctx->encoder_applied_max_bitrate_kbps;
    int is_vbr = hctx->rate_control == VA_RC_VBR;
    if (is_vbr && hctx->encoder_max_bitrate_kbps == 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (is_vbr && hctx->encoder_max_bitrate_kbps != previous_max_bitrate_kbps) {
        int max_ret = hb_mm_mc_set_max_bit_rate_config(
            &hctx->vpu_ctx, hctx->encoder_max_bitrate_kbps);
        if (max_ret != 0) {
            int rollback_ret = hb_mm_mc_set_max_bit_rate_config(
                &hctx->vpu_ctx, previous_max_bitrate_kbps);
            hctx->encoder_max_bitrate_kbps = previous_max_bitrate_kbps;
            hctx->vpu_ctx.video_enc_params.rc_params = *previous_params;
            if (rollback_ret != 0) {
                hctx->encoder_failed = 1;
                fprintf(stderr,
                        "[HOBOT-VA] VBR maximum bitrate restore failed for ctx=%u: %d\n",
                        hctx->id, rollback_ret);
            }
            fprintf(stderr,
                    "[HOBOT-VA] VBR maximum bitrate update failed for ctx=%u: %d\n",
                    hctx->id, max_ret);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }

    int ret = hb_mm_mc_set_rate_control_config(
        &hctx->vpu_ctx, &hctx->vpu_ctx.video_enc_params.rc_params);
    if (ret == 0) {
        if (is_vbr)
            hctx->encoder_applied_max_bitrate_kbps =
                hctx->encoder_max_bitrate_kbps;
        return VA_STATUS_SUCCESS;
    }

    hctx->vpu_ctx.video_enc_params.rc_params = *previous_params;
    if (is_vbr && hctx->encoder_max_bitrate_kbps != previous_max_bitrate_kbps) {
        int rollback_ret = hb_mm_mc_set_max_bit_rate_config(
            &hctx->vpu_ctx, previous_max_bitrate_kbps);
        if (rollback_ret != 0) {
            hctx->encoder_failed = 1;
            fprintf(stderr,
                    "[HOBOT-VA] VBR maximum bitrate rollback failed for ctx=%u: %d\n",
                    hctx->id, rollback_ret);
        }
        hctx->encoder_max_bitrate_kbps = previous_max_bitrate_kbps;
    }
    fprintf(stderr, "[HOBOT-VA] rate-control update failed for ctx=%u: %d\n",
            hctx->id, ret);
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

static VAStatus hobot_apply_h264_cqp(HobotContext *hctx, uint32_t qp) {
    if (!hctx || hctx->profile == VAProfileHEVCMain || qp > 51u)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    mc_rate_control_params_t previous =
        hctx->vpu_ctx.video_enc_params.rc_params;
    mc_rate_control_params_t pending = previous;
    pending.mode = MC_AV_RC_MODE_H264FIXQP;
    pending.h264_fixqp_params.force_qp_I = qp;
    pending.h264_fixqp_params.force_qp_P = qp;
    pending.h264_fixqp_params.force_qp_B = qp;
    hctx->vpu_ctx.video_enc_params.rc_params = pending;
    return hobot_apply_encoder_rate_control(hctx, &previous);
}

static VAStatus hobot_apply_hevc_cqp(HobotContext *hctx, uint32_t qp) {
    if (!hctx || hctx->profile != VAProfileHEVCMain || qp > 51u)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    mc_rate_control_params_t previous =
        hctx->vpu_ctx.video_enc_params.rc_params;
    mc_rate_control_params_t pending = previous;
    pending.mode = MC_AV_RC_MODE_H265FIXQP;
    pending.h265_fixqp_params.force_qp_I = qp;
    pending.h265_fixqp_params.force_qp_P = qp;
    pending.h265_fixqp_params.force_qp_B = qp;
    hctx->vpu_ctx.video_enc_params.rc_params = pending;
    return hobot_apply_encoder_rate_control(hctx, &previous);
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

typedef struct {
    uint32_t target_bitrate_kbps;
    uint32_t max_bitrate_kbps;
    int32_t window_ms;
} HobotVbrRateControl;

static int hobot_parse_vbr_rate_control(
    const VAEncMiscParameterRateControl *rate,
    HobotVbrRateControl *parsed
) {
    if (rate)
        va_trace("VBR rate control request: max=%u target=%u%% window=%u initial=%u min=%u max=%u flags=0x%x basic=%u icq=%u quality=%u frame_size=%u",
                 rate->bits_per_second, rate->target_percentage,
                 rate->window_size, rate->initial_qp, rate->min_qp,
                 rate->max_qp, rate->rc_flags.value,
                 rate->basic_unit_size, rate->ICQ_quality_factor,
                 rate->quality_factor, rate->target_frame_size);
    if (!rate || !parsed || rate->bits_per_second < 1000u ||
        rate->bits_per_second > 700000000u ||
        rate->target_percentage == 0 || rate->target_percentage > 100u ||
        (rate->window_size != 0 &&
         (rate->window_size < 10u || rate->window_size > 3000u)) ||
        rate->initial_qp > 51u || rate->min_qp > 51u || rate->max_qp > 51u ||
        (rate->min_qp != 0 && rate->max_qp != 0 &&
         rate->min_qp > rate->max_qp) || rate->basic_unit_size != 0 ||
        rate->rc_flags.value != 0 || rate->quality_factor != 0 ||
        rate->target_frame_size != 0)
        return 0;

    for (size_t i = 0; i < sizeof(rate->va_reserved) / sizeof(rate->va_reserved[0]);
         i++) {
        if (rate->va_reserved[i] != 0)
            return 0;
    }

    uint32_t max_kbps = rate->bits_per_second / 1000u;
    uint32_t target_kbps =
        (uint32_t)(((uint64_t)max_kbps * rate->target_percentage + 50u) / 100u);
    if (target_kbps == 0 || target_kbps > max_kbps)
        return 0;

    parsed->target_bitrate_kbps = target_kbps;
    parsed->max_bitrate_kbps = max_kbps;
    parsed->window_ms = rate->window_size == 0 ? 3000 :
                        (int32_t)rate->window_size;
    return 1;
}

static VAStatus hobot_update_vbr_rate_control(
    HobotContext *hctx,
    const VAEncMiscParameterRateControl *rate,
    const HobotVbrRateControl *parsed
) {
    if (!hctx || !rate || !parsed || hctx->rate_control != VA_RC_VBR)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    mc_rate_control_params_t previous =
        hctx->vpu_ctx.video_enc_params.rc_params;
    mc_rate_control_params_t pending = previous;
    if (hctx->profile == VAProfileHEVCMain) {
        mc_h265_avbr_params_t *avbr = &pending.h265_avbr_params;
        if (!hctx->encoder_init_deferred && rate->initial_qp != 0 &&
            rate->initial_qp != avbr->initial_rc_qp)
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        pending.mode = MC_AV_RC_MODE_H265AVBR;
        avbr->bit_rate = parsed->target_bitrate_kbps;
        avbr->vbv_buffer_size = parsed->window_ms;
        if (rate->initial_qp != 0)
            avbr->initial_rc_qp = rate->initial_qp;
        if (rate->min_qp != 0) {
            avbr->min_qp_I = rate->min_qp;
            avbr->min_qp_P = rate->min_qp;
            avbr->min_qp_B = rate->min_qp;
        }
        if (rate->max_qp != 0) {
            avbr->max_qp_I = rate->max_qp;
            avbr->max_qp_P = rate->max_qp;
            avbr->max_qp_B = rate->max_qp;
        }
        if (avbr->min_qp_I > avbr->max_qp_I ||
            avbr->min_qp_P > avbr->max_qp_P ||
            avbr->min_qp_B > avbr->max_qp_B)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    } else {
        mc_h264_avbr_params_t *avbr = &pending.h264_avbr_params;
        if (!hctx->encoder_init_deferred && rate->initial_qp != 0 &&
            rate->initial_qp != avbr->initial_rc_qp)
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        pending.mode = MC_AV_RC_MODE_H264AVBR;
        avbr->bit_rate = parsed->target_bitrate_kbps;
        avbr->vbv_buffer_size = parsed->window_ms;
        if (rate->initial_qp != 0)
            avbr->initial_rc_qp = rate->initial_qp;
        if (rate->min_qp != 0) {
            avbr->min_qp_I = rate->min_qp;
            avbr->min_qp_P = rate->min_qp;
            avbr->min_qp_B = rate->min_qp;
        }
        if (rate->max_qp != 0) {
            avbr->max_qp_I = rate->max_qp;
            avbr->max_qp_P = rate->max_qp;
            avbr->max_qp_B = rate->max_qp;
        }
        if (avbr->min_qp_I > avbr->max_qp_I ||
            avbr->min_qp_P > avbr->max_qp_P ||
            avbr->min_qp_B > avbr->max_qp_B)
            return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    hctx->vpu_ctx.video_enc_params.rc_params = pending;
    hctx->encoder_max_bitrate_kbps = parsed->max_bitrate_kbps;
    VAStatus status = hobot_apply_encoder_rate_control(hctx, &previous);
    if (status != VA_STATUS_SUCCESS)
        return status;
    return VA_STATUS_SUCCESS;
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

static int hobot_h264_encode_reference_pattern_supported(
    const VAEncSequenceParameterBufferH264 *seq
) {
    return seq && seq->ip_period == 1 && seq->max_num_ref_frames <= 1;
}

static int hobot_h264_sequence_config_equal(
    const VAEncSequenceParameterBufferH264 *a,
    const VAEncSequenceParameterBufferH264 *b
) {
    if (a->level_idc != b->level_idc ||
        a->ip_period != b->ip_period ||
        a->max_num_ref_frames != b->max_num_ref_frames ||
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
    if (hctx->rate_control == VA_RC_CQP) {
        mc_h264_fix_qp_params_t *fixqp =
            &mctx->video_enc_params.rc_params.h264_fixqp_params;
        if (fixqp->frame_rate == 0 || fixqp->frame_rate > 240u ||
            fixqp->force_qp_I > 51u || fixqp->force_qp_P > 51u ||
            fixqp->force_qp_B > 51u) {
            fprintf(stderr, "[HOBOT-VA] invalid H.264 FIXQP parameters for ctx=%u\n",
                    hctx->id);
            hctx->encoder_init_deferred = 0;
            hctx->encoder_failed = 1;
            return -1;
        }
        mctx->video_enc_params.rc_params.mode = MC_AV_RC_MODE_H264FIXQP;
    } else if (hctx->rate_control == VA_RC_VBR) {
        mc_h264_avbr_params_t *avbr =
            &mctx->video_enc_params.rc_params.h264_avbr_params;
        if (avbr->bit_rate == 0 || avbr->bit_rate > 700000u ||
            avbr->bit_rate > hctx->encoder_max_bitrate_kbps ||
            avbr->frame_rate == 0 || avbr->frame_rate > 240u ||
            avbr->vbv_buffer_size < 10 || avbr->vbv_buffer_size > 3000 ||
            avbr->min_qp_I > avbr->max_qp_I ||
            avbr->min_qp_P > avbr->max_qp_P ||
            avbr->min_qp_B > avbr->max_qp_B) {
            fprintf(stderr, "[HOBOT-VA] invalid H.264 AVBR parameters for ctx=%u\n",
                    hctx->id);
            hctx->encoder_init_deferred = 0;
            hctx->encoder_failed = 1;
            return -1;
        }
        mctx->video_enc_params.rc_params.mode = MC_AV_RC_MODE_H264AVBR;
    } else {
        mctx->video_enc_params.rc_params.mode = MC_AV_RC_MODE_H264CBR;
    }
    /* VA-API supplies no per-surface timestamp on this path. */
    mctx->video_enc_params.enable_user_pts = 0;
    mctx->video_enc_params.gop_params.gop_preset_idx =
        hctx->h264_sequence_valid &&
        hctx->h264_sequence.max_num_ref_frames == 0 ? 1 : 9;
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

    ret = hobot_register_encoder_input_listener(hctx);
    if (ret != 0)
        goto fail;

    ret = hb_mm_mc_configure(mctx);
    if (ret != 0)
        goto fail;

    ret = hobot_allocate_encoder_scratch(hctx);
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

    mc_av_codec_startup_params_t startup_params = {0};
    ret = hb_mm_mc_start(mctx, &startup_params);
    if (ret != 0)
        goto fail;

    hctx->vpu_running = 1;
    if (hctx->rate_control == VA_RC_VBR) {
        ret = hb_mm_mc_set_max_bit_rate_config(
            mctx, hctx->encoder_max_bitrate_kbps);
        if (ret != 0)
            goto fail;
        hctx->encoder_applied_max_bitrate_kbps =
            hctx->encoder_max_bitrate_kbps;
    }
    hctx->encoder_init_deferred = 0;
    va_trace("H.264 encoder started: ctx=%u level=%u fps=%u full_range=0",
             hctx->id,
             hctx->h264_sequence_valid ? hctx->h264_sequence.level_idc : 0,
             hctx->rate_control == VA_RC_CQP ?
                 hctx->vpu_ctx.video_enc_params.rc_params.h264_fixqp_params.frame_rate :
                 hctx->rate_control == VA_RC_VBR ?
                     hctx->vpu_ctx.video_enc_params.rc_params.h264_avbr_params.frame_rate :
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
    if (!hctx->vpu_running && !hctx->vpu_initialized &&
        hobot_free_encoder_scratch(hctx) != 0) {
        fprintf(stderr, "[HOBOT-VA] deferred encoder scratch cleanup failed for ctx=%u\n",
                hctx->id);
    }
    if (hctx->vpu_running || hctx->vpu_initialized)
        hctx->cleanup_orphaned = 1;
    if (hctx->enc_scratch_allocated)
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
    enc->h265_enc_config.wpp_enable =
        pic->pic_fields.bits.entropy_coding_sync_enabled_flag;

    if (hctx->rate_control == VA_RC_CQP) {
        mc_h265_fix_qp_params_t *fixqp =
            &enc->rc_params.h265_fixqp_params;
        if (hctx->hevc_encode_pic_qp_valid &&
            hctx->hevc_encode_slice_qp_delta_valid) {
            int effective_qp = (int)hctx->hevc_encode_pic_qp +
                               hctx->hevc_encode_slice_qp_delta;
            if (effective_qp < 0 || effective_qp > 51) {
                hctx->encoder_init_deferred = 0;
                hctx->encoder_failed = 1;
                return -1;
            }
            fixqp->force_qp_I = (uint32_t)effective_qp;
            fixqp->force_qp_P = (uint32_t)effective_qp;
        }
        if (seq->intra_period > 0)
            fixqp->intra_period = seq->intra_period;
        enc->rc_params.mode = MC_AV_RC_MODE_H265FIXQP;
        if (fixqp->frame_rate == 0 || fixqp->frame_rate > 240u ||
            fixqp->force_qp_I > 51u || fixqp->force_qp_P > 51u) {
            fprintf(stderr, "[HOBOT-VA] invalid HEVC FIXQP parameters for ctx=%u\n",
                    hctx->id);
            hctx->encoder_init_deferred = 0;
            hctx->encoder_failed = 1;
            return -1;
        }
    } else if (hctx->rate_control == VA_RC_VBR) {
        mc_h265_avbr_params_t *avbr = &enc->rc_params.h265_avbr_params;
        if (avbr->bit_rate == 0 || avbr->bit_rate > 700000u ||
            avbr->bit_rate > hctx->encoder_max_bitrate_kbps ||
            avbr->frame_rate == 0 || avbr->frame_rate > 240u ||
            avbr->vbv_buffer_size < 10 || avbr->vbv_buffer_size > 3000 ||
            avbr->min_qp_I > avbr->max_qp_I ||
            avbr->min_qp_P > avbr->max_qp_P ||
            avbr->min_qp_B > avbr->max_qp_B) {
            fprintf(stderr, "[HOBOT-VA] unsupported HEVC AVBR parameters for ctx=%u\n",
                    hctx->id);
            hctx->encoder_init_deferred = 0;
            hctx->encoder_failed = 1;
            return -1;
        }
        enc->rc_params.mode = MC_AV_RC_MODE_H265AVBR;
    } else {
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
        if (hctx->rate_control == VA_RC_CQP)
            enc->rc_params.h265_fixqp_params.frame_rate = (uint32_t)rounded_fps;
        else if (hctx->rate_control == VA_RC_VBR)
            enc->rc_params.h265_avbr_params.frame_rate = (uint32_t)rounded_fps;
        else
            enc->rc_params.h265_cbr_params.frame_rate = (uint32_t)rounded_fps;
    }

    int ret = hb_mm_mc_initialize(mctx);
    if (ret != 0)
        goto fail;
    hctx->vpu_initialized = 1;

    ret = hobot_register_encoder_input_listener(hctx);
    if (ret != 0)
        goto fail;

    mc_h265_sao_params_t sao = {
        .sample_adaptive_offset_enabled_flag = 0
    };
    ret = hb_mm_mc_set_sao_config(mctx, &sao);
    if (ret != 0)
        goto fail;

    ret = hb_mm_mc_configure(mctx);
    if (ret != 0)
        goto fail;

    ret = hobot_allocate_encoder_scratch(hctx);
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

    mc_av_codec_startup_params_t startup_params = {0};
    ret = hb_mm_mc_start(mctx, &startup_params);
    if (ret != 0)
        goto fail;

    hctx->vpu_running = 1;
    if (hctx->rate_control == VA_RC_VBR) {
        ret = hb_mm_mc_set_max_bit_rate_config(
            mctx, hctx->encoder_max_bitrate_kbps);
        if (ret != 0)
            goto fail;
        hctx->encoder_applied_max_bitrate_kbps =
            hctx->encoder_max_bitrate_kbps;
    }
    hctx->encoder_init_deferred = 0;
    va_trace("HEVC encoder started: ctx=%u level=%u rc=%s qp=%u bitrate=%u kbps fps=%u",
             hctx->id, seq->general_level_idc,
             hctx->rate_control == VA_RC_CQP ? "CQP" :
                 hctx->rate_control == VA_RC_VBR ? "VBR" : "CBR",
             hctx->rate_control == VA_RC_CQP ?
                 enc->rc_params.h265_fixqp_params.force_qp_P : 0,
             hctx->rate_control == VA_RC_CQP ? 0 :
                 hctx->rate_control == VA_RC_VBR ?
                     enc->rc_params.h265_avbr_params.bit_rate :
                     enc->rc_params.h265_cbr_params.bit_rate,
             hctx->rate_control == VA_RC_CQP ?
                 enc->rc_params.h265_fixqp_params.frame_rate :
                 hctx->rate_control == VA_RC_VBR ?
                     enc->rc_params.h265_avbr_params.frame_rate :
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
    if (!hctx->vpu_running && !hctx->vpu_initialized &&
        hobot_free_encoder_scratch(hctx) != 0) {
        fprintf(stderr, "[HOBOT-VA] deferred HEVC scratch cleanup failed for ctx=%u\n",
                hctx->id);
    }
    if (hctx->vpu_running || hctx->vpu_initialized)
        hctx->cleanup_orphaned = 1;
    if (hctx->enc_scratch_allocated)
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
        picture->picture_width == 0 || picture->picture_height == 0 ||
        picture->num_components != 3 || picture->color_space != 0 ||
        slice->slice_data_size == 0 || slice->slice_data_flag != VA_SLICE_DATA_FLAG_ALL ||
        slice->slice_horizontal_position != 0 || slice->slice_vertical_position != 0 ||
        slice->num_components != 3)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (picture->rotation != VA_ROTATION_NONE &&
        picture->rotation != VA_ROTATION_90 &&
        picture->rotation != VA_ROTATION_180 &&
        picture->rotation != VA_ROTATION_270)
        return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
    int swapped = picture->rotation == VA_ROTATION_90 ||
                  picture->rotation == VA_ROTATION_270;
    if ((unsigned int)picture->picture_width !=
            (unsigned int)(swapped ? context_height : context_width) ||
        (unsigned int)picture->picture_height !=
            (unsigned int)(swapped ? context_width : context_height))
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

    uint8_t luma_h = picture->components[0].h_sampling_factor;
    uint8_t luma_v = picture->components[0].v_sampling_factor;
    uint8_t chroma_h = picture->components[1].h_sampling_factor;
    uint8_t chroma_v = picture->components[1].v_sampling_factor;
    uint8_t max_h = luma_h;
    uint8_t max_v = luma_v;
    for (size_t i = 1; i < 3; i++) {
        if (picture->components[i].h_sampling_factor > max_h)
            max_h = picture->components[i].h_sampling_factor;
        if (picture->components[i].v_sampling_factor > max_v)
            max_v = picture->components[i].v_sampling_factor;
    }
    int supported_sampling = luma_h == 2 && luma_v == 2 &&
                             chroma_h == 1 && chroma_v == 1 &&
                             picture->components[2].h_sampling_factor == 1 &&
                             picture->components[2].v_sampling_factor == 1;
    va_trace("JPEG VLD sampling: size=%ux%u components=%u factors=%u/%u,%u/%u,%u/%u mcus=%u",
             picture->picture_width, picture->picture_height,
             picture->num_components,
             picture->components[0].h_sampling_factor,
             picture->components[0].v_sampling_factor,
             picture->components[1].h_sampling_factor,
             picture->components[1].v_sampling_factor,
             picture->components[2].h_sampling_factor,
             picture->components[2].v_sampling_factor,
             slice->num_mcus);
    if (!supported_sampling ||
        picture->components[0].component_id == picture->components[1].component_id ||
        picture->components[0].component_id == picture->components[2].component_id ||
        picture->components[1].component_id == picture->components[2].component_id) {
        va_trace("JPEG VLD sampling pattern is unsupported");
        return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
    }

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

    uint64_t mcu_width_pixels = (uint64_t)max_h * 8u;
    uint64_t mcu_height_pixels = (uint64_t)max_v * 8u;
    uint64_t mcu_width = ((uint64_t)picture->picture_width +
                          mcu_width_pixels - 1u) / mcu_width_pixels;
    uint64_t mcu_height = ((uint64_t)picture->picture_height +
                           mcu_height_pixels - 1u) / mcu_height_pixels;
    if (mcu_width == 0 || mcu_height == 0 || mcu_width > UINT32_MAX / mcu_height ||
        slice->num_mcus != mcu_width * mcu_height) {
        va_trace("JPEG VLD MCU count mismatch: grid=%llux%llu expected=%llu got=%u",
                 (unsigned long long)mcu_width,
                 (unsigned long long)mcu_height,
                 (unsigned long long)(mcu_width * mcu_height),
                 slice->num_mcus);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

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

static int hobot_jpeg_rotation_to_sdk(uint32_t va_rotation,
                                      mc_rotate_degree_t *sdk_rotation)
{
    if (!sdk_rotation)
        return 0;
    switch (va_rotation) {
    case VA_ROTATION_NONE:
        *sdk_rotation = MC_CCW_0;
        return 1;
    case VA_ROTATION_90:
        *sdk_rotation = MC_CCW_270;
        return 1;
    case VA_ROTATION_180:
        *sdk_rotation = MC_CCW_180;
        return 1;
    case VA_ROTATION_270:
        *sdk_rotation = MC_CCW_90;
        return 1;
    default:
        return 0;
    }
}

static int hobot_start_deferred_jpeg_decoder(HobotContext *hctx,
                                              uint32_t va_rotation)
{
    if (!hctx)
        return -1;
    if (!hctx->jpeg_init_deferred)
        return hctx->vpu_running && hctx->jpeg_rotation_fixed &&
               hctx->jpeg_rotation == va_rotation ? 0 : -1;

    mc_rotate_degree_t sdk_rotation;
    if (!hobot_jpeg_rotation_to_sdk(va_rotation, &sdk_rotation))
        return -1;

    media_codec_context_t *mctx = &hctx->vpu_ctx;
    mctx->video_dec_params.jpeg_dec_config.rot_degree = sdk_rotation;
    mctx->video_dec_params.jpeg_dec_config.mir_direction = MC_DIRECTION_NONE;
    mctx->video_dec_params.jpeg_dec_config.frame_crop_enable = 0;

    int ret = hb_mm_mc_initialize(mctx);
    if (ret == 0)
        hctx->vpu_initialized = 1;
    if (ret == 0)
        ret = hb_mm_mc_configure(mctx);
    if (ret == 0)
        ret = hb_mm_mc_start(mctx, NULL);
    if (ret == 0) {
        hctx->vpu_running = 1;
        hctx->jpeg_init_deferred = 0;
        hctx->jpeg_rotation = va_rotation;
        hctx->jpeg_rotation_fixed = 1;
        va_trace("JPEG decoder started: ctx=%u rotation=%u sdk_rotation=%d",
                 hctx->id, va_rotation, sdk_rotation);
        return 0;
    }

    fprintf(stderr, "[HOBOT-VA] deferred JPEG decoder setup failed for ctx=%u: %d\n",
            hctx->id, ret);
    if (hctx->vpu_running) {
        int stop_ret = hb_mm_mc_stop(mctx);
        if (stop_ret == 0)
            hctx->vpu_running = 0;
        else
            fprintf(stderr, "[HOBOT-VA] deferred JPEG stop cleanup failed: %d\n",
                    stop_ret);
    }
    if (hctx->vpu_initialized && !hctx->vpu_running) {
        int release_ret = hb_mm_mc_release(mctx);
        if (release_ret == 0)
            hctx->vpu_initialized = 0;
        else
            fprintf(stderr, "[HOBOT-VA] deferred JPEG release cleanup failed: %d\n",
                    release_ret);
    }
    hctx->jpeg_init_deferred = 0;
    hctx->decode_failed = 1;
    return ret;
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

static int hobot_init_sync_condition(pthread_cond_t *condition) {
    pthread_condattr_t attr;
    if (pthread_condattr_init(&attr) != 0)
        return -1;
    int status = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (status == 0)
        status = pthread_cond_init(condition, &attr);
    pthread_condattr_destroy(&attr);
    return status == 0 ? 0 : -1;
}

static int hobot_ensure_sync_condition(HobotDriverData *drv) {
    if (drv->sync_cond_initialized) return 0;
    if (hobot_init_sync_condition(&drv->sync_cond) != 0) return -1;
    drv->sync_cond_initialized = 1;
    return 0;
}

static int hobot_sync_deadline_after(uint64_t timeout_ns,
                                     struct timespec *deadline) {
    if (!deadline || clock_gettime(CLOCK_MONOTONIC, deadline) != 0 ||
        deadline->tv_sec < 0)
        return -1;
    uint64_t seconds = timeout_ns / 1000000000ULL;
    uint64_t nanoseconds = timeout_ns % 1000000000ULL;
    if (seconds > (uint64_t)(INT64_MAX - (int64_t)deadline->tv_sec))
        return -1;
    deadline->tv_sec += (time_t)seconds;
    deadline->tv_nsec += (long)nanoseconds;
    if (deadline->tv_nsec >= 1000000000L) {
        if (deadline->tv_sec == (time_t)INT64_MAX)
            return -1;
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
    return 0;
}

static int hobot_sync_remaining_ms(const struct timespec *deadline,
                                   int maximum_ms) {
    if (!deadline || maximum_ms <= 0)
        return 0;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    int64_t seconds = (int64_t)deadline->tv_sec - (int64_t)now.tv_sec;
    int64_t nanoseconds = (int64_t)deadline->tv_nsec - (int64_t)now.tv_nsec;
    if (nanoseconds < 0) {
        seconds--;
        nanoseconds += 1000000000LL;
    }
    if (seconds < 0 || (seconds == 0 && nanoseconds == 0))
        return 0;
    if ((uint64_t)seconds > (uint64_t)maximum_ms / 1000u)
        return maximum_ms;
    uint64_t milliseconds = (uint64_t)seconds * 1000u +
        ((uint64_t)nanoseconds / 1000000u) +
        ((uint64_t)nanoseconds % 1000000u != 0);
    if (milliseconds == 0)
        milliseconds = 1;
    return milliseconds > (uint64_t)maximum_ms ? maximum_ms :
           (int)milliseconds;
}

static int hobot_mutex_lock_until(pthread_mutex_t *mutex,
                                  const struct timespec *deadline) {
    if (!deadline)
        return pthread_mutex_lock(mutex);

    int first_attempt = 1;
    for (;;) {
        if (!first_attempt) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
                return errno ? errno : EINVAL;
            if (now.tv_sec > deadline->tv_sec ||
                (now.tv_sec == deadline->tv_sec &&
                 now.tv_nsec >= deadline->tv_nsec))
                return ETIMEDOUT;
        }
        first_attempt = 0;

        int status = pthread_mutex_trylock(mutex);
        if (status != EBUSY)
            return status;

        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            return errno ? errno : EINVAL;
        int64_t seconds = (int64_t)deadline->tv_sec - (int64_t)now.tv_sec;
        int64_t nanoseconds = (int64_t)deadline->tv_nsec - (int64_t)now.tv_nsec;
        if (nanoseconds < 0) {
            seconds--;
            nanoseconds += 1000000000LL;
        }
        if (seconds < 0 || (seconds == 0 && nanoseconds <= 0))
            return ETIMEDOUT;

        struct timespec pause = { .tv_sec = 0, .tv_nsec = 1000000L };
        if (seconds == 0 && nanoseconds < pause.tv_nsec)
            pause.tv_nsec = (long)nanoseconds;
        if (nanosleep(&pause, NULL) != 0 && errno != EINTR)
            return errno ? errno : EINVAL;
    }
}

static int hobot_wait_sync_condition(HobotDriverData *drv,
                                     const struct timespec *deadline) {
    return deadline ? pthread_cond_timedwait(&drv->sync_cond, &drv->mutex,
                                             deadline) :
                      pthread_cond_wait(&drv->sync_cond, &drv->mutex);
}

static int hobot_queue_output_until(HobotContext *hctx,
                                    media_codec_buffer_t *buffer,
                                    const struct timespec *deadline,
                                    int *timed_out) {
    if (timed_out)
        *timed_out = 0;
    if (!deadline) {
        int ret = hb_mm_mc_queue_output_buffer(&hctx->vpu_ctx, buffer, 50);
        if (ret != 0)
            ret = hb_mm_mc_queue_output_buffer(&hctx->vpu_ctx, buffer, 200);
        return ret;
    }

    int ret = HB_MEDIA_ERR_WAIT_TIMEOUT;
    for (int attempt = 0; attempt < 2; attempt++) {
        int timeout_ms = hobot_sync_remaining_ms(deadline, 200);
        if (timeout_ms < 0)
            return -1;
        if (timeout_ms == 0) {
            if (timed_out)
                *timed_out = 1;
            return ret;
        }
        ret = hb_mm_mc_queue_output_buffer(&hctx->vpu_ctx, buffer, timeout_ms);
        if (ret == 0)
            return 0;
    }
    int remaining_ms = hobot_sync_remaining_ms(deadline, 1);
    if (remaining_ms == 0 && timed_out)
        *timed_out = 1;
    return ret;
}

static int hobot_recycle_decoder_output_until(
    HobotContext *hctx,
    media_codec_buffer_t *buffer,
    const struct timespec *deadline,
    int *timed_out) {
    int ret = hobot_queue_output_until(hctx, buffer, deadline, timed_out);
    if (ret == 0)
        return 0;

    if (!hctx->dec_out_buf_valid) {
        hctx->dec_out_buf = *buffer;
        hctx->dec_out_buf_valid = 1;
    } else {
        fprintf(stderr, "[HOBOT-VA] decoder output ownership invariant violated: pending buffer already retained\n");
        hctx->decode_failed = 1;
    }
    if (!timed_out || !*timed_out)
        hctx->decode_failed = 1;
    return ret;
}

static int hobot_retry_decoder_output_until(
    HobotContext *hctx,
    const struct timespec *deadline,
    int *timed_out) {
    if (!hctx->dec_out_buf_valid)
        return 0;
    int ret = hobot_queue_output_until(hctx, &hctx->dec_out_buf,
                                      deadline, timed_out);
    if (ret == 0) {
        hctx->dec_out_buf_valid = 0;
        memset(&hctx->dec_out_buf, 0, sizeof(hctx->dec_out_buf));
    } else if (!timed_out || !*timed_out) {
        hctx->decode_failed = 1;
    }
    return ret;
}

static uint32_t hobot_context_usage_bit(VAContextID context) {
    return context > 0 && context < MAX_CONTEXTS ? (1u << context) : 0;
}

static void hobot_retain_context_surface_usage(HobotDriverData *drv,
                                               VAContextID context,
                                               VASurfaceID surface) {
    uint32_t bit = hobot_context_usage_bit(context);
    if (bit != 0 && surface > 0 && surface < MAX_SURFACES &&
        drv->surfaces[surface].allocated)
        drv->surfaces[surface].context_usage_mask |= bit;
}

static int hobot_surface_id_allocated(const HobotDriverData *drv,
                                      VASurfaceID surface) {
    return drv && surface != VA_INVALID_SURFACE && surface > 0 &&
           surface < MAX_SURFACES && drv->surfaces[surface].allocated;
}

static int hobot_h264_current_surface_valid(
    const HobotDriverData *drv,
    const VAPictureH264 *picture
) {
    return picture && !(picture->flags & VA_PICTURE_H264_INVALID) &&
           hobot_surface_id_allocated(drv, picture->picture_id);
}

static int hobot_hevc_current_surface_valid(
    const HobotDriverData *drv,
    const VAPictureHEVC *picture
) {
    return picture && !(picture->flags & VA_PICTURE_HEVC_INVALID) &&
           hobot_surface_id_allocated(drv, picture->picture_id);
}

static int hobot_h264_reference_surfaces_valid(
    const HobotDriverData *drv,
    const VAPictureH264 *references,
    size_t count
) {
    if (!drv || !references)
        return 0;
    for (size_t i = 0; i < count; i++) {
        if (references[i].flags & VA_PICTURE_H264_INVALID)
            continue;
        if (!hobot_surface_id_allocated(drv, references[i].picture_id))
            return 0;
    }
    return 1;
}

static int hobot_hevc_reference_surfaces_valid(
    const HobotDriverData *drv,
    const VAPictureHEVC *references,
    size_t count
) {
    if (!drv || !references)
        return 0;
    for (size_t i = 0; i < count; i++) {
        if (references[i].flags & VA_PICTURE_HEVC_INVALID)
            continue;
        if (!hobot_surface_id_allocated(drv, references[i].picture_id))
            return 0;
    }
    return 1;
}

static void hobot_retain_h264_picture_surfaces(
    HobotDriverData *drv,
    VAContextID context,
    const VAPictureParameterBufferH264 *picture) {
    if (!picture)
        return;
    if (!(picture->CurrPic.flags & VA_PICTURE_H264_INVALID))
        hobot_retain_context_surface_usage(drv, context,
                                          picture->CurrPic.picture_id);
    for (size_t i = 0; i < sizeof(picture->ReferenceFrames) /
                            sizeof(picture->ReferenceFrames[0]); i++) {
        if (!(picture->ReferenceFrames[i].flags & VA_PICTURE_H264_INVALID))
            hobot_retain_context_surface_usage(
                drv, context, picture->ReferenceFrames[i].picture_id);
    }
}

static void hobot_retain_hevc_picture_surfaces(
    HobotDriverData *drv,
    VAContextID context,
    const VAPictureParameterBufferHEVC *picture) {
    if (!picture)
        return;
    if (!(picture->CurrPic.flags & VA_PICTURE_HEVC_INVALID))
        hobot_retain_context_surface_usage(drv, context,
                                          picture->CurrPic.picture_id);
    for (size_t i = 0; i < sizeof(picture->ReferenceFrames) /
                            sizeof(picture->ReferenceFrames[0]); i++) {
        if (!(picture->ReferenceFrames[i].flags & VA_PICTURE_HEVC_INVALID))
            hobot_retain_context_surface_usage(
                drv, context, picture->ReferenceFrames[i].picture_id);
    }
}

static void hobot_retain_h264_encode_picture_surfaces(
    HobotDriverData *drv,
    VAContextID context,
    const VAEncPictureParameterBufferH264 *picture) {
    if (!picture)
        return;
    if (!(picture->CurrPic.flags & VA_PICTURE_H264_INVALID))
        hobot_retain_context_surface_usage(drv, context,
                                          picture->CurrPic.picture_id);
    for (size_t i = 0; i < sizeof(picture->ReferenceFrames) /
                            sizeof(picture->ReferenceFrames[0]); i++) {
        if (!(picture->ReferenceFrames[i].flags & VA_PICTURE_H264_INVALID))
            hobot_retain_context_surface_usage(
                drv, context, picture->ReferenceFrames[i].picture_id);
    }
}

static void hobot_retain_hevc_encode_picture_surfaces(
    HobotDriverData *drv,
    VAContextID context,
    const VAEncPictureParameterBufferHEVC *picture) {
    if (!picture)
        return;
    if (!(picture->decoded_curr_pic.flags & VA_PICTURE_HEVC_INVALID))
        hobot_retain_context_surface_usage(drv, context,
                                          picture->decoded_curr_pic.picture_id);
    for (size_t i = 0; i < sizeof(picture->reference_frames) /
                            sizeof(picture->reference_frames[0]); i++) {
        if (!(picture->reference_frames[i].flags & VA_PICTURE_HEVC_INVALID))
            hobot_retain_context_surface_usage(
                drv, context, picture->reference_frames[i].picture_id);
    }
}

static void hobot_clear_context_surface_usage(HobotDriverData *drv,
                                               VAContextID context) {
    uint32_t bit = hobot_context_usage_bit(context);
    if (bit == 0)
        return;
    for (int s = 1; s < MAX_SURFACES; s++)
        drv->surfaces[s].context_usage_mask &= ~bit;
}

static void hobot_detach_context_surfaces(HobotDriverData *drv, VAContextID context) {
    hobot_clear_context_surface_usage(drv, context);
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
        if (hobot_free_encoder_scratch(hctx) != 0) {
            fprintf(stderr, "[HOBOT-VA] vaCreateContext: orphan ctx=%d scratch free retry failed\n", i);
            return -1;
        }
        hobot_clear_context_surface_usage(drv, (VAContextID)i);
        memset(hctx, 0, sizeof(*hctx));
        for (size_t fd_index = 0; fd_index < MAX_GRAPHIC_BUF_COMP; fd_index++)
            hctx->enc_scratch_buf.fd[fd_index] = -1;
        atomic_init(&hctx->enc_external_input_pending, 0);
        hctx->enc_external_surface = VA_INVALID_SURFACE;
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
    if (drv->resources_terminated) {
        pthread_mutex_unlock(&drv->mutex);
        goto close_memory_module;
    }
    for (int i = 1; i < MAX_CONTEXTS; i++) {
        if (drv->contexts[i].sync_active || drv->contexts[i].decode_picture_active ||
            atomic_load_explicit(&drv->contexts[i].enc_external_input_pending,
                                 memory_order_acquire)) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    }
    for (int i = 1; i < MAX_BUFFERS; i++) {
        if (drv->buffers[i].allocated &&
            (drv->buffers[i].map_count > 0 ||
             drv->buffers[i].handle_acquired)) {
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
                            } else {
                                fprintf(stderr, "[HOBOT-VA] vaTerminate: hb_mm_mc_release failed: %d for ctx=%d\n", rret, i);
                                term_failed_cleanup = 1;
                            }
                        }
                    }
                }
                if (!term_failed_cleanup &&
                    hobot_free_encoder_scratch(hctx) != 0) {
                    fprintf(stderr, "[HOBOT-VA] vaTerminate: encoder scratch free failed for ctx=%d\n", i);
                    term_failed_cleanup = 1;
                }
                if (!term_failed_cleanup && !hctx->vpu_running &&
                    !hctx->vpu_initialized) {
                    hctx->allocated = 0;
                    hobot_detach_context_surfaces(drv, (VAContextID)i);
                }
            }
        }
        if (term_failed_cleanup) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (drv->orphaned_import_reserved != 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        for (unsigned int i = 0; i < drv->orphaned_import_count; i++) {
            int fd = drv->orphaned_import_fds[i];
            if (fd < 0)
                continue;
            if (hb_mem_free_buf(fd) != 0) {
                fprintf(stderr, "[HOBOT-VA] vaTerminate: orphaned PRIME import free failed for fd=%d\n",
                        fd);
                term_failed_cleanup = 1;
            } else {
                drv->orphaned_import_fds[i] = -1;
            }
        }
        if (term_failed_cleanup) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        drv->orphaned_import_count = 0;
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
        drv->resources_terminated = 1;
        pthread_mutex_unlock(&drv->mutex);

close_memory_module:
        int32_t mem_ret = hb_mem_module_close();
        if (mem_ret != 0) {
            fprintf(stderr, "[HOBOT-VA] vaTerminate: hb_mem_module_close failed: %d\n",
                    mem_ret);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (drv->sync_cond_initialized)
            pthread_cond_destroy(&drv->sync_cond);
        pthread_mutex_destroy(&drv->mutex);
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

    unsigned int max_picture_width, max_picture_height;
    hobot_profile_max_resolution(profile, entrypoint,
                                 &max_picture_width, &max_picture_height);

    for (int i = 0; i < num_attribs; i++) {
        switch (attrib_list[i].type) {
        case VAConfigAttribRTFormat:
            attrib_list[i].value = VA_RT_FORMAT_YUV420;
            break;
        case VAConfigAttribDecJPEG:
            attrib_list[i].value = is_jpeg && entrypoint == VAEntrypointVLD ?
                                   ((1u << (VA_ROTATION_270 + 1u)) - 1u) :
                                   VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribRateControl:
            attrib_list[i].value = is_h264 &&
                                   entrypoint == VAEntrypointEncSlice ?
                                   (VA_RC_CBR | VA_RC_VBR | VA_RC_CQP) :
                                   is_hevc && entrypoint == VAEntrypointEncSlice ?
                                   (VA_RC_CBR | VA_RC_VBR | VA_RC_CQP) :
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
        case VAConfigAttribPredictionDirection:
            attrib_list[i].value = (is_h264 || is_hevc) &&
                                   entrypoint == VAEntrypointEncSlice ?
                                   VA_PREDICTION_DIRECTION_PREVIOUS :
                                   VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncMaxSlices:
            attrib_list[i].value = (is_h264 || is_hevc) &&
                                   entrypoint == VAEntrypointEncSlice ?
                                   1 : VA_ATTRIB_NOT_SUPPORTED;
            break;
        case VAConfigAttribMaxPictureWidth:
            attrib_list[i].value = max_picture_width;
            break;
        case VAConfigAttribMaxPictureHeight:
            attrib_list[i].value = max_picture_height;
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
    unsigned int supported_rate_control = is_enc &&
        profile != VAProfileJPEGBaseline ?
        (VA_RC_CBR | VA_RC_VBR | VA_RC_CQP) : expected_rate_control;
    for (int i = 0; i < num_attribs; i++) {
        switch (attrib_list[i].type) {
        case VAConfigAttribRTFormat:
            if (attrib_list[i].value != VA_RT_FORMAT_YUV420)
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
            break;
        case VAConfigAttribRateControl:
            if (!is_enc || attrib_list[i].value == 0 ||
                (attrib_list[i].value & ~supported_rate_control) != 0 ||
                (attrib_list[i].value & (attrib_list[i].value - 1u)) != 0)
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            expected_rate_control = attrib_list[i].value;
            break;
        case VAConfigAttribEncPackedHeaders:
            if (!is_enc || attrib_list[i].value != VA_ENC_PACKED_HEADER_NONE)
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            break;
        case VAConfigAttribEncInterlaced:
            if (!is_enc || attrib_list[i].value != VA_ENC_INTERLACED_NONE)
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
    VAEntrypoint config_entrypoint = valid_config ?
        drv->configs[config_id].entrypoint : (VAEntrypoint)-1;
    pthread_mutex_unlock(&drv->mutex);
    if (!valid_config) return VA_STATUS_ERROR_INVALID_CONFIG;
    unsigned int max_width, max_height;
    hobot_profile_max_resolution(config_profile, config_entrypoint,
                                 &max_width, &max_height);
    if (!attrib_list) {
        *num_attribs = 7;
        return VA_STATUS_SUCCESS;
    }
    if (*num_attribs < 7) {
        *num_attribs = 7;
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
    attrib_list[idx].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_VA |
                                     VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    idx++;

    attrib_list[idx].type = VASurfaceAttribExternalBufferDescriptor;
    attrib_list[idx].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrib_list[idx].value.type = VAGenericValueTypePointer;
    attrib_list[idx].value.value.p = NULL;
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
    attrib_list[idx].value.value.i = (int)max_width;
    idx++;

    attrib_list[idx].type = VASurfaceAttribMaxHeight;
    attrib_list[idx].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[idx].value.type = VAGenericValueTypeInteger;
    attrib_list[idx].value.value.i = (int)max_height;
    idx++;

    *num_attribs = idx;
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_import_prime2_nv12(
    HobotDriverData *drv,
    const VADRMPRIMESurfaceDescriptor *descriptor,
    unsigned int width,
    unsigned int height,
    hb_mem_graphic_buf_t *imported,
    int *stride_out,
    uint32_t *object_size_out);

static int hobot_reserve_import_cleanup_slots(HobotDriverData *drv,
                                              unsigned int count)
{
    pthread_mutex_lock(&drv->mutex);
    unsigned int occupied = drv->orphaned_import_count +
                            drv->orphaned_import_reserved;
    int available = occupied <= MAX_SURFACES &&
                    count <= MAX_SURFACES - occupied;
    if (available)
        drv->orphaned_import_reserved += count;
    pthread_mutex_unlock(&drv->mutex);
    return available;
}

static void hobot_release_import_cleanup_slots(HobotDriverData *drv,
                                              unsigned int count)
{
    pthread_mutex_lock(&drv->mutex);
    if (drv->orphaned_import_reserved >= count)
        drv->orphaned_import_reserved -= count;
    else
        drv->orphaned_import_reserved = 0;
    pthread_mutex_unlock(&drv->mutex);
}

static int hobot_release_import_fd(HobotDriverData *drv, int fd)
{
    if (fd < 0 || hb_mem_free_buf(fd) == 0)
        return 0;

    pthread_mutex_lock(&drv->mutex);
    int retained = drv->orphaned_import_count < MAX_SURFACES;
    if (retained)
        drv->orphaned_import_fds[drv->orphaned_import_count++] = fd;
    pthread_mutex_unlock(&drv->mutex);
    fprintf(stderr, "[HOBOT-VA] PRIME import reference release failed for fd=%d%s\n",
            fd, retained ? "; retained for vaTerminate retry" : "; retention capacity exhausted");
    return retained ? -1 : -2;
}

static int hobot_release_imported_graph_bufs(HobotDriverData *drv,
                                             hb_mem_graphic_buf_t *buffers,
                                             unsigned int count)
{
    int failed = 0;
    for (unsigned int i = 0; i < count; i++) {
        int fd = buffers[i].fd[0];
        if (fd < 0)
            continue;
        int ret = hobot_release_import_fd(drv, fd);
        if (ret != 0)
            failed = 1;
        if (ret != -2)
            buffers[i].fd[0] = -1;
    }
    return failed;
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
        width < 64 || height < 64 ||
        width > HOBOT_HEVC_DECODE_MAX_WIDTH ||
        height > HOBOT_HEVC_DECODE_MAX_HEIGHT ||
        (width & 1u) != 0 || (height & 1u) != 0 ||
        num_attribs > MAX_SURFACE_ATTRIBUTES ||
        (num_attribs > 0 && !attrib_list)) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }
    if (format != VA_RT_FORMAT_YUV420) {
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    }
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    int memory_type = VA_SURFACE_ATTRIB_MEM_TYPE_VA;
    int memory_type_seen = 0;
    int pixel_format_seen = 0;
    int external_descriptor_seen = 0;
    const VADRMPRIMESurfaceDescriptor *external_descriptors = NULL;
    for (unsigned int i = 0; i < num_attribs; i++) {
        VASurfaceAttrib *attr = &attrib_list[i];
        if (attr->type == VASurfaceAttribPixelFormat) {
            if (pixel_format_seen++)
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            /* Some libva clients leave the generic type zeroed for FOURCC attrs. */
            if ((attr->value.type != VAGenericValueTypeInteger &&
                 attr->value.type != 0) ||
                attr->value.value.i != VA_FOURCC_NV12) {
                return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
            }
        } else if (attr->type == VASurfaceAttribMemoryType) {
            if (memory_type_seen++ ||
                attr->value.type != VAGenericValueTypeInteger) {
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
            memory_type = attr->value.value.i;
            if (memory_type != VA_SURFACE_ATTRIB_MEM_TYPE_VA &&
                memory_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
                return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
            }
        } else if (attr->type == VASurfaceAttribExternalBufferDescriptor) {
            if (external_descriptor_seen++ ||
                attr->value.type != VAGenericValueTypePointer ||
                !attr->value.value.p) {
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
            external_descriptors = attr->value.value.p;
        } else {
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
    }
    if ((memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2 &&
         !external_descriptors) ||
        (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_VA &&
         external_descriptors)) {
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    hb_mem_graphic_buf_t *imported_buffers = NULL;
    int *imported_strides = NULL;
    uint32_t *imported_sizes = NULL;
    int import_cleanup_slots_reserved = 0;
    if (memory_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) {
        imported_buffers = calloc(num_surfaces, sizeof(*imported_buffers));
        imported_strides = calloc(num_surfaces, sizeof(*imported_strides));
        imported_sizes = calloc(num_surfaces, sizeof(*imported_sizes));
        if (!imported_buffers || !imported_strides || !imported_sizes) {
            free(imported_buffers);
            free(imported_strides);
            free(imported_sizes);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        if (!hobot_reserve_import_cleanup_slots(drv, num_surfaces)) {
            free(imported_buffers);
            free(imported_strides);
            free(imported_sizes);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        import_cleanup_slots_reserved = 1;
        for (unsigned int i = 0; i < num_surfaces; i++) {
            VAStatus import_status = hobot_import_prime2_nv12(
                drv, &external_descriptors[i], width, height, &imported_buffers[i],
                &imported_strides[i], &imported_sizes[i]);
            if (import_status != VA_STATUS_SUCCESS) {
                int cleanup_failed = hobot_release_imported_graph_bufs(
                    drv, imported_buffers, i);
                hobot_release_import_cleanup_slots(drv, num_surfaces);
                free(imported_buffers);
                free(imported_strides);
                free(imported_sizes);
                return cleanup_failed ? VA_STATUS_ERROR_OPERATION_FAILED :
                                        import_status;
            }
        }
    }

    VASurfaceID created_ids[MAX_SURFACES];
    pthread_mutex_lock(&drv->mutex);
    unsigned int found = 0;
    for (int s = 1; s < MAX_SURFACES && found < num_surfaces; s++) {
        if (!drv->surfaces[s].allocated)
            created_ids[found++] = (VASurfaceID)s;
    }
    if (found != num_surfaces) {
        pthread_mutex_unlock(&drv->mutex);
        int cleanup_failed = imported_buffers &&
            hobot_release_imported_graph_bufs(drv, imported_buffers, num_surfaces);
        if (import_cleanup_slots_reserved)
            hobot_release_import_cleanup_slots(drv, num_surfaces);
        free(imported_buffers);
        free(imported_strides);
        free(imported_sizes);
        return cleanup_failed ? VA_STATUS_ERROR_OPERATION_FAILED :
                                VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    for (unsigned int i = 0; i < num_surfaces; i++) {
        int s = (int)created_ids[i];
        HobotSurface *surf = &drv->surfaces[s];
        unsigned int aligned_w = (width + 63u) & ~63u;
        unsigned int aligned_h = (height + 63u) & ~63u;
        memset(surf, 0, sizeof(*surf));
        surf->allocated = 1;
        surf->id = (VASurfaceID)s;
        surf->width = width;
        surf->height = height;
        surf->format = format;
        surf->stride = imported_buffers ? imported_strides[i] : (int)aligned_w;
        surf->raw_data_size = imported_buffers ? imported_sizes[i] :
            (uint32_t)((uint64_t)aligned_w * aligned_h * 3 / 2);
        surf->dma_fd = -1;
        surf->has_preallocated = imported_buffers != NULL;
        surf->preallocated_gbuf.fd[0] = -1;
        surf->preallocated_gbuf.fd[1] = -1;
        if (imported_buffers) {
            surf->preallocated_gbuf = imported_buffers[i];
            surf->dma_fd = imported_buffers[i].fd[0];
        }
        surfaces[i] = (VASurfaceID)s;
    }
    va_trace("vaCreateSurfaces2 -> created %u surfaces (first=%u)", num_surfaces, surfaces[0]);
    if (import_cleanup_slots_reserved) {
        if (drv->orphaned_import_reserved >= num_surfaces)
            drv->orphaned_import_reserved -= num_surfaces;
        else
            drv->orphaned_import_reserved = 0;
    }
    pthread_mutex_unlock(&drv->mutex);
    free(imported_buffers);
    free(imported_strides);
    free(imported_sizes);
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
        if (surf->context_usage_mask != 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_SURFACE_BUSY;
        }
        if (surf->lock_count > 0 || surf->external_handle_count > 0) {
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
            surf->cpu_access_count = 0;
            surf->va_lock_count = 0;
            surf->cpu_cache_flush_pending = 0;
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
    *status = (surf->decode_pending ||
               hobot_surface_has_active_encoder(drv, render_target)) ?
              VASurfaceRendering : VASurfaceReady;
    va_trace("vaQuerySurfaceStatus: surf=%u -> %d", render_target, *status);
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaQuerySurfaceError(
    VADriverContextP ctx,
    VASurfaceID render_target,
    VAStatus error_status,
    void **error_info
) {
    if (!ctx || !ctx->pDriverData)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!error_info)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    *error_info = NULL;
    if (error_status != VA_STATUS_ERROR_DECODING_ERROR)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    int valid_surface = render_target > 0 && render_target < MAX_SURFACES &&
                        drv->surfaces[render_target].allocated;
    pthread_mutex_unlock(&drv->mutex);
    if (!valid_surface)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus hobot_vaGetSurfaceAttributes(
    VADriverContextP ctx,
    VAConfigID config,
    VASurfaceAttrib *attrib_list,
    unsigned int num_attribs
) {
    unsigned int queried_attribs = num_attribs;
    return hobot_vaQuerySurfaceAttributes(ctx, config, attrib_list,
                                          &queried_attribs);
}

static VAStatus hobot_vaAcquireBufferHandle(
    VADriverContextP ctx,
    VABufferID buf_id,
    VABufferInfo *buf_info
);

static VAStatus hobot_vaReleaseBufferHandle(VADriverContextP ctx,
                                            VABufferID buf_id);

static VAStatus hobot_vaCreateMFContext(VADriverContextP ctx,
                                        VAMFContextID *mf_context) {
    if (!ctx || !ctx->pDriverData)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!mf_context)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    *mf_context = VA_INVALID_ID;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus hobot_vaMFAddContext(VADriverContextP ctx,
                                     VAMFContextID mf_context,
                                     VAContextID context) {
    (void)mf_context;
    (void)context;
    if (!ctx || !ctx->pDriverData)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus hobot_vaMFReleaseContext(VADriverContextP ctx,
                                         VAMFContextID mf_context,
                                         VAContextID context) {
    (void)mf_context;
    (void)context;
    if (!ctx || !ctx->pDriverData)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus hobot_vaMFSubmit(VADriverContextP ctx,
                                 VAMFContextID mf_context,
                                 VAContextID *contexts,
                                 int num_contexts) {
    (void)mf_context;
    if (!ctx || !ctx->pDriverData)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (num_contexts <= 0 || !contexts)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus hobot_vaQueryProcessingRate(
    VADriverContextP ctx,
    VAConfigID config_id,
    VAProcessingRateParameter *proc_buf,
    unsigned int *processing_rate
) {
    (void)proc_buf;
    if (!ctx || !ctx->pDriverData)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!processing_rate)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    *processing_rate = 0;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    int valid_config = config_id > 0 && config_id < MAX_CONFIGS &&
                       drv->configs[config_id].allocated;
    pthread_mutex_unlock(&drv->mutex);
    if (!valid_config)
        return VA_STATUS_ERROR_INVALID_CONFIG;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus hobot_vaCopy(VADriverContextP ctx,
                             VACopyObject *dst,
                             VACopyObject *src,
                             VACopyOption option) {
    (void)option;
    if (!ctx || !ctx->pDriverData)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!dst || !src)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
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
        picture_width > HOBOT_HEVC_DECODE_MAX_WIDTH ||
        picture_height > HOBOT_HEVC_DECODE_MAX_HEIGHT || num_render_targets < 0 ||
        (flag & ~VA_PROGRESSIVE) != 0 ||
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
        if (drv->surfaces[target].external_handle_count > 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_SURFACE_BUSY;
        }
    }
    if (hobot_reap_orphan_contexts(drv) != 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    HobotConfig *cfg = &drv->configs[config_id];
    if (!hobot_profile_resolution_supported(cfg->profile, cfg->entrypoint,
                                            picture_width, picture_height)) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    }

    for (int i = 1; i < MAX_CONTEXTS; i++) {
        if (!drv->contexts[i].allocated) {
            HobotContext *c = &drv->contexts[i];
            memset(c, 0, sizeof(*c));
            for (size_t fd_index = 0; fd_index < MAX_GRAPHIC_BUF_COMP; fd_index++)
                c->enc_scratch_buf.fd[fd_index] = -1;
            atomic_init(&c->enc_external_input_pending, 0);
            c->enc_external_surface = VA_INVALID_SURFACE;
            c->allocated = 1;
            c->id = (VAContextID)i;
            for (int target_index = 0; target_index < num_render_targets;
                 target_index++)
                hobot_retain_context_surface_usage(
                    drv, (VAContextID)i, render_targets[target_index]);
            c->width = picture_width;
            c->height = picture_height;
            c->profile = cfg->profile;
            c->rate_control = cfg->rate_control;
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
                c->enc_external_enabled =
                    cid == MEDIA_CODEC_ID_H264 || cid == MEDIA_CODEC_ID_H265;
                mctx->video_enc_params.external_frame_buf =
                    c->enc_external_enabled;
                mctx->video_enc_params.gop_params.gop_preset_idx = 9;
                mctx->video_enc_params.gop_params.decoding_refresh_type = 2;

                if (cid == MEDIA_CODEC_ID_H264) {
                    if (cfg->rate_control == VA_RC_CQP) {
                        mctx->video_enc_params.rc_params.mode = MC_AV_RC_MODE_H264FIXQP;
                        mctx->video_enc_params.rc_params.h264_fixqp_params.frame_rate = 30;
                        mctx->video_enc_params.rc_params.h264_fixqp_params.intra_period = 30;
                        mctx->video_enc_params.rc_params.h264_fixqp_params.force_qp_I = 26;
                        mctx->video_enc_params.rc_params.h264_fixqp_params.force_qp_P = 26;
                        mctx->video_enc_params.rc_params.h264_fixqp_params.force_qp_B = 26;
                    } else if (cfg->rate_control == VA_RC_VBR) {
                        mc_h264_avbr_params_t *avbr =
                            &mctx->video_enc_params.rc_params.h264_avbr_params;
                        mctx->video_enc_params.rc_params.mode = MC_AV_RC_MODE_H264AVBR;
                        avbr->bit_rate = 10000;
                        avbr->frame_rate = 30;
                        avbr->intra_period = 30;
                        avbr->intra_qp = 26;
                        avbr->initial_rc_qp = 63;
                        avbr->vbv_buffer_size = 3000;
                        avbr->min_qp_I = 8;
                        avbr->min_qp_P = 8;
                        avbr->min_qp_B = 8;
                        avbr->max_qp_I = 51;
                        avbr->max_qp_P = 51;
                        avbr->max_qp_B = 51;
                        avbr->hvs_qp_scale = 2;
                        avbr->max_delta_qp = 5;
                    } else {
                        mctx->video_enc_params.rc_params.mode = MC_AV_RC_MODE_H264CBR;
                        mctx->video_enc_params.rc_params.h264_cbr_params.bit_rate = 10000;
                        mctx->video_enc_params.rc_params.h264_cbr_params.frame_rate = 30;
                        mctx->video_enc_params.rc_params.h264_cbr_params.intra_period = 30;
                    }
                } else if (cid == MEDIA_CODEC_ID_H265) {
                    if (c->rate_control == VA_RC_CQP) {
                        mctx->video_enc_params.rc_params.mode = MC_AV_RC_MODE_H265FIXQP;
                        mctx->video_enc_params.rc_params.h265_fixqp_params.frame_rate = 30;
                        mctx->video_enc_params.rc_params.h265_fixqp_params.intra_period = 30;
                        mctx->video_enc_params.rc_params.h265_fixqp_params.force_qp_I = 26;
                        mctx->video_enc_params.rc_params.h265_fixqp_params.force_qp_P = 26;
                        mctx->video_enc_params.rc_params.h265_fixqp_params.force_qp_B = 26;
                    } else if (c->rate_control == VA_RC_VBR) {
                        mc_h265_avbr_params_t *avbr =
                            &mctx->video_enc_params.rc_params.h265_avbr_params;
                        mctx->video_enc_params.rc_params.mode = MC_AV_RC_MODE_H265AVBR;
                        avbr->bit_rate = 10000;
                        avbr->frame_rate = 30;
                        avbr->intra_period = 30;
                        avbr->intra_qp = 26;
                        avbr->initial_rc_qp = 63;
                        avbr->vbv_buffer_size = 3000;
                        avbr->min_qp_I = 8;
                        avbr->min_qp_P = 8;
                        avbr->min_qp_B = 8;
                        avbr->max_qp_I = 51;
                        avbr->max_qp_P = 51;
                        avbr->max_qp_B = 51;
                        avbr->hvs_qp_scale = 2;
                        avbr->max_delta_qp = 5;
                    } else {
                        mctx->video_enc_params.rc_params.mode = MC_AV_RC_MODE_H265CBR;
                        mctx->video_enc_params.rc_params.h265_cbr_params.bit_rate = 10000;
                        mctx->video_enc_params.rc_params.h265_cbr_params.frame_rate = 30;
                        mctx->video_enc_params.rc_params.h265_cbr_params.intra_period = 30;
                    }
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
                    if (c->rate_control == VA_RC_VBR)
                        c->encoder_max_bitrate_kbps = 10000;
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
                /* Wave521 stalls the final frame at eight HEVC references with 16 slots. */
                mctx->video_dec_params.frame_buf_count =
                    cid == MEDIA_CODEC_ID_H265 ? 17 : 16;
                if (cid == MEDIA_CODEC_ID_H264) {
                    mctx->video_dec_params.h264_dec_config.reorder_enable = 0;
                    mctx->video_dec_params.h264_dec_config.skip_mode = 0;
                    mctx->video_dec_params.h264_dec_config.bandwidth_Opt = 1;
                } else if (cid == MEDIA_CODEC_ID_H265) {
                    mctx->video_dec_params.h265_dec_config.bandwidth_Opt = 1;
                }

                if (ret == 0 && cid == MEDIA_CODEC_ID_JPEG) {
                    mctx->video_dec_params.jpeg_dec_config.rot_degree = MC_CCW_0;
                    mctx->video_dec_params.jpeg_dec_config.mir_direction = MC_DIRECTION_NONE;
                    mctx->video_dec_params.jpeg_dec_config.frame_crop_enable = 0;
                    c->jpeg_init_deferred = 1;
                } else {
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
                if (!cleanup_failed && hobot_free_encoder_scratch(c) != 0) {
                    fprintf(stderr, "[HOBOT-VA] vaCreateContext: encoder scratch free failed\n");
                    cleanup_failed = 1;
                }
                if (cleanup_failed) {
                    /* Release/stop failed: preserve context allocation and state for later retry/destruction */
                    c->cleanup_orphaned = 1;
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_OPERATION_FAILED;
                }
                hobot_clear_context_surface_usage(drv, (VAContextID)i);
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
        drv->contexts[context].decode_picture_active ||
        atomic_load_explicit(&drv->contexts[context].enc_external_input_pending,
                             memory_order_acquire)) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (hobot_context_has_locked_surfaces(drv, context) ||
        hobot_context_surface_used_by_other_encoder(drv, context)) {
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
        if (hobot_free_encoder_scratch(hctx) != 0) {
            fprintf(stderr, "[HOBOT-VA] vaDestroyContext: encoder scratch free failed for ctx=%d\n",
                    context);
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
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
            drv->buffers[i].handle_acquired = 0;
            drv->buffers[i].acquired_fd = -1;
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
    if (buffer->handle_acquired) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (buffer->is_derived) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
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

static VAStatus hobot_get_derived_image_surface_locked(
    HobotDriverData *drv,
    VABufferID buffer_id,
    HobotSurface **surface_out);
static VAStatus hobot_sync_derived_image_cache_locked(
    HobotDriverData *drv,
    VABufferID buffer_id,
    int invalidate);
static VAStatus hobot_sync_surface_cache_locked(HobotSurface *surface, int invalidate);

static VAStatus hobot_vaMapBuffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf) {
    if (!ctx || !ctx->pDriverData) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!pbuf) return VA_STATUS_ERROR_INVALID_PARAMETER;
    *pbuf = NULL;
    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    if (buf_id > 0 && buf_id < MAX_BUFFERS && drv->buffers[buf_id].allocated) {
        HobotBuffer *buffer = &drv->buffers[buf_id];
        if (buffer->handle_acquired) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_SURFACE_BUSY;
        }
        if (buffer->map_count == UINT32_MAX) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
        }
        if (buffer->is_derived && !buffer->data) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (buffer->is_derived && buffer->map_count == 0) {
            HobotSurface *surface = NULL;
            VAStatus surface_status = hobot_get_derived_image_surface_locked(
                drv, buf_id, &surface);
            if (surface_status != VA_STATUS_SUCCESS) {
                pthread_mutex_unlock(&drv->mutex);
                return surface_status;
            }
            if (surface->cpu_access_count == UINT32_MAX) {
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
            }
            if (surface->cpu_access_count == 0) {
                if (surface->cpu_cache_flush_pending) {
                    VAStatus flush_status = hobot_sync_derived_image_cache_locked(
                        drv, buf_id, 0);
                    if (flush_status != VA_STATUS_SUCCESS) {
                        pthread_mutex_unlock(&drv->mutex);
                        return flush_status;
                    }
                    surface->cpu_cache_flush_pending = 0;
                }
                VAStatus cache_status = hobot_sync_derived_image_cache_locked(
                    drv, buf_id, 1);
                if (cache_status != VA_STATUS_SUCCESS) {
                    pthread_mutex_unlock(&drv->mutex);
                    return cache_status;
                }
            }
            surface->cpu_access_count++;
        }
        if (buffer->type == VAEncCodedBufferType)
            *pbuf = &buffer->coded_segment;
        else
            *pbuf = buffer->data;
        buffer->map_count++;
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
    HobotBuffer *buffer = &drv->buffers[buf_id];
    VAStatus cache_status = VA_STATUS_SUCCESS;
    if (buffer->is_derived && buffer->map_count == 1) {
        HobotSurface *surface = NULL;
        VAStatus surface_status = hobot_get_derived_image_surface_locked(
            drv, buf_id, &surface);
        if (surface_status != VA_STATUS_SUCCESS || surface->cpu_access_count == 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        surface->raw_data_valid = 0;
        if (surface->cpu_access_count == 1) {
            cache_status = hobot_sync_derived_image_cache_locked(drv, buf_id, 0);
            surface->cpu_cache_flush_pending = cache_status != VA_STATUS_SUCCESS;
        }
        surface->cpu_access_count--;
    }
    buffer->map_count--;
    pthread_mutex_unlock(&drv->mutex);
    return cache_status;
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
        if (hobot_buffer_has_active_encoder(drv, buffer_id))
            return VA_STATUS_ERROR_OPERATION_FAILED;
        if (drv->buffers[buffer_id].map_count > 0 ||
            drv->buffers[buffer_id].handle_acquired)
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
        drv->buffers[buffer_id].handle_acquired = 0;
        drv->buffers[buffer_id].acquired_fd = -1;
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
static VAStatus hobot_vaSyncSurfaceUntilLocked(
    HobotDriverData *drv,
    VASurfaceID render_target,
    const struct timespec *deadline);
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

static int hobot_prime2_nv12_metadata_matches(
    const VADRMPRIMESurfaceDescriptor *descriptor,
    unsigned int width,
    unsigned int height,
    const hb_mem_graphic_buf_t *gbuf,
    HobotPreallocatedNV12Layout *layout_out)
{
    if (!descriptor || !gbuf || !layout_out || gbuf->fd[0] < 0 ||
        (gbuf->fd[1] >= 0 && gbuf->fd[1] != gbuf->fd[0]) ||
        gbuf->plane_cnt != 2 || gbuf->format != MEM_PIX_FMT_NV12 ||
        gbuf->width != (int32_t)width || gbuf->height != (int32_t)height ||
        !gbuf->is_contig || !gbuf->virt_addr[0] || !gbuf->virt_addr[1] ||
        gbuf->phys_addr[0] == 0 || gbuf->phys_addr[1] <= gbuf->phys_addr[0])
        return 0;

    uintptr_t virt_y = (uintptr_t)gbuf->virt_addr[0];
    uintptr_t virt_uv = (uintptr_t)gbuf->virt_addr[1];
    if (virt_uv <= virt_y ||
        (uint64_t)(virt_uv - virt_y) != gbuf->phys_addr[1] - gbuf->phys_addr[0])
        return 0;

    HobotSurface candidate = {0};
    candidate.width = width;
    candidate.height = height;
    candidate.stride = gbuf->stride;
    candidate.has_preallocated = 1;
    candidate.preallocated_gbuf = *gbuf;
    HobotPreallocatedNV12Layout layout;
    if (hobot_get_preallocated_nv12_layout(&candidate, &layout) !=
            VA_STATUS_SUCCESS ||
        layout.separate_fds || layout.stride != descriptor->layers[0].pitch[0] ||
        layout.stride != descriptor->layers[0].pitch[1] ||
        layout.y_offset != descriptor->layers[0].offset[0] ||
        layout.uv_offset != descriptor->layers[0].offset[1] ||
        layout.object_size[0] != descriptor->objects[0].size ||
        layout.object_size[0] > UINT32_MAX)
        return 0;

    uint64_t y_extent = (uint64_t)layout.stride * height;
    uint64_t uv_extent = (uint64_t)layout.stride * (height / 2u);
    uint64_t object_size = descriptor->objects[0].size;
    if (layout.y_offset > object_size || y_extent > object_size - layout.y_offset ||
        layout.uv_offset > object_size || uv_extent > object_size - layout.uv_offset)
        return 0;

    *layout_out = layout;
    return 1;
}

static int hobot_get_prime2_graph_metadata(HobotDriverData *drv,
                                           int fd,
                                           hb_mem_graphic_buf_t *metadata)
{
    int ret = hb_mem_get_graph_buf(fd, metadata);
    if (ret == 0)
        return 0;
    if (!drv)
        return ret;

    struct stat target_stat;
    if (fstat(fd, &target_stat) != 0)
        return ret;

    pthread_mutex_lock(&drv->mutex);
    for (int i = 1; i < MAX_SURFACES; i++) {
        HobotSurface *surface = &drv->surfaces[i];
        if (!surface->allocated || !surface->has_preallocated ||
            surface->preallocated_gbuf.fd[0] < 0)
            continue;
        struct stat known_stat;
        if (fstat(surface->preallocated_gbuf.fd[0], &known_stat) != 0 ||
            target_stat.st_dev != known_stat.st_dev ||
            target_stat.st_ino != known_stat.st_ino)
            continue;

        ret = hb_mem_get_graph_buf(surface->preallocated_gbuf.fd[0], metadata);
        if (ret == 0) {
            metadata->fd[0] = fd;
            pthread_mutex_unlock(&drv->mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&drv->mutex);
    return ret;
}

static VAStatus hobot_import_prime2_nv12(
    HobotDriverData *drv,
    const VADRMPRIMESurfaceDescriptor *descriptor,
    unsigned int width,
    unsigned int height,
    hb_mem_graphic_buf_t *imported,
    int *stride_out,
    uint32_t *object_size_out)
{
    if (!descriptor || !imported || !stride_out || !object_size_out)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    memset(imported, 0, sizeof(*imported));
    for (size_t i = 0; i < MAX_GRAPHIC_BUF_COMP; i++)
        imported->fd[i] = -1;

    if (descriptor->fourcc != VA_FOURCC_NV12 ||
        descriptor->width != width || descriptor->height != height ||
        descriptor->num_objects != 1 || descriptor->num_layers != 1 ||
        descriptor->objects[0].fd < 0 || descriptor->objects[0].size == 0 ||
        descriptor->objects[0].drm_format_modifier != DRM_FORMAT_MOD_LINEAR ||
        descriptor->layers[0].drm_format != DRM_FORMAT_NV12 ||
        descriptor->layers[0].num_planes != 2 ||
        descriptor->layers[0].object_index[0] != 0 ||
        descriptor->layers[0].object_index[1] != 0) {
        va_trace("PRIME2 import representation rejected: fourcc=0x%x size=%ux%u objects=%u layers=%u fd=%d bytes=%u modifier=0x%llx layer_format=0x%x planes=%u objects=%u/%u",
                 descriptor->fourcc, descriptor->width, descriptor->height,
                 descriptor->num_objects, descriptor->num_layers,
                 descriptor->objects[0].fd, descriptor->objects[0].size,
                 (unsigned long long)descriptor->objects[0].drm_format_modifier,
                 descriptor->layers[0].drm_format,
                 descriptor->layers[0].num_planes,
                 descriptor->layers[0].object_index[0],
                 descriptor->layers[0].object_index[1]);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    if (descriptor->layers[0].pitch[0] < width ||
        descriptor->layers[0].pitch[0] != descriptor->layers[0].pitch[1] ||
        (descriptor->layers[0].pitch[0] & 1u) != 0 ||
        (descriptor->layers[0].offset[0] & 1u) != 0 ||
        (descriptor->layers[0].offset[1] & 1u) != 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    hb_mem_graphic_buf_t external;
    memset(&external, 0, sizeof(external));
    for (size_t i = 0; i < MAX_GRAPHIC_BUF_COMP; i++)
        external.fd[i] = -1;
    int get_ret = hobot_get_prime2_graph_metadata(
        drv, descriptor->objects[0].fd, &external);
    if (get_ret != 0) {
        va_trace("hb_mem_get_graph_buf rejected PRIME fd=%d ret=%d",
                 descriptor->objects[0].fd, get_ret);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    HobotPreallocatedNV12Layout source_layout;
    if (!hobot_prime2_nv12_metadata_matches(descriptor, width, height,
                                             &external, &source_layout)) {
        va_trace("PRIME2 source metadata mismatch: fd=%d/%d planes=%d format=%d size=%dx%d stride=%d vstride=%d contig=%d phys=0x%llx/0x%llx virt=%p/%p bytes=%llu/%llu offsets=%llu/%llu descriptor offsets=%u/%u pitches=%u/%u object_size=%u",
                 external.fd[0], external.fd[1], external.plane_cnt,
                 external.format, external.width, external.height,
                 external.stride, external.vstride, external.is_contig,
                 (unsigned long long)external.phys_addr[0],
                 (unsigned long long)external.phys_addr[1],
                 external.virt_addr[0], external.virt_addr[1],
                 (unsigned long long)external.size[0],
                 (unsigned long long)external.size[1],
                 (unsigned long long)external.offset[0],
                 (unsigned long long)external.offset[1],
                 descriptor->layers[0].offset[0],
                 descriptor->layers[0].offset[1],
                 descriptor->layers[0].pitch[0],
                 descriptor->layers[0].pitch[1],
                 descriptor->objects[0].size);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    hb_mem_graphic_buf_t imported_candidate;
    memset(&imported_candidate, 0, sizeof(imported_candidate));
    for (size_t i = 0; i < MAX_GRAPHIC_BUF_COMP; i++)
        imported_candidate.fd[i] = -1;
    int import_input_fd = fcntl(descriptor->objects[0].fd,
                                F_DUPFD_CLOEXEC, 0);
    if (import_input_fd < 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    external.fd[0] = import_input_fd;
    int import_ret = hb_mem_import_graph_buf(&external, &imported_candidate);
    if (import_ret != 0) {
        close(import_input_fd);
        va_trace("hb_mem_import_graph_buf failed for fd=%d ret=%d",
                 descriptor->objects[0].fd, import_ret);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (imported_candidate.fd[0] != import_input_fd)
        close(import_input_fd);

    HobotPreallocatedNV12Layout imported_layout;
    if (imported_candidate.fd[0] == descriptor->objects[0].fd) {
        fprintf(stderr, "[HOBOT-VA] refusing PRIME import that aliases the caller FD\n");
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (!hobot_prime2_nv12_metadata_matches(descriptor, width, height,
                                             &imported_candidate,
                                             &imported_layout)) {
        va_trace("PRIME2 imported metadata mismatch: fd=%d/%d planes=%d format=%d size=%dx%d stride=%d vstride=%d contig=%d phys=0x%llx/0x%llx virt=%p/%p bytes=%llu/%llu offsets=%llu/%llu",
                 imported_candidate.fd[0], imported_candidate.fd[1],
                 imported_candidate.plane_cnt, imported_candidate.format,
                 imported_candidate.width, imported_candidate.height,
                 imported_candidate.stride, imported_candidate.vstride,
                 imported_candidate.is_contig,
                 (unsigned long long)imported_candidate.phys_addr[0],
                 (unsigned long long)imported_candidate.phys_addr[1],
                 imported_candidate.virt_addr[0], imported_candidate.virt_addr[1],
                 (unsigned long long)imported_candidate.size[0],
                 (unsigned long long)imported_candidate.size[1],
                 (unsigned long long)imported_candidate.offset[0],
                 (unsigned long long)imported_candidate.offset[1]);
        int free_ret = imported_candidate.fd[0] >= 0 ?
            hobot_release_import_fd(drv, imported_candidate.fd[0]) : -1;
        if (free_ret != 0)
            fprintf(stderr, "[HOBOT-VA] failed to release invalid PRIME import fd=%d\n",
                    imported_candidate.fd[0]);
        return free_ret == 0 ? VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE :
                               VA_STATUS_ERROR_OPERATION_FAILED;
    }

    *stride_out = (int)imported_layout.stride;
    *object_size_out = (uint32_t)imported_layout.object_size[0];
    *imported = imported_candidate;
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_get_derived_image_surface_locked(
    HobotDriverData *drv,
    VABufferID buffer_id,
    HobotSurface **surface_out)
{
    if (!drv || !surface_out)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    for (int i = 1; i < MAX_IMAGES; i++) {
        HobotImage *image = &drv->images[i];
        if (!image->allocated || image->buf_id != buffer_id || image->surface_id == 0)
            continue;
        if (image->surface_id <= 0 || image->surface_id >= MAX_SURFACES ||
            !drv->surfaces[image->surface_id].allocated)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        *surface_out = &drv->surfaces[image->surface_id];
        return VA_STATUS_SUCCESS;
    }
    return VA_STATUS_ERROR_OPERATION_FAILED;
}

static VAStatus hobot_sync_derived_image_cache_locked(
    HobotDriverData *drv,
    VABufferID buffer_id,
    int invalidate)
{
    HobotSurface *surface = NULL;
    VAStatus status = hobot_get_derived_image_surface_locked(
        drv, buffer_id, &surface);
    if (status != VA_STATUS_SUCCESS)
        return status;
    return hobot_sync_surface_cache_locked(surface, invalidate);
}

static VAStatus hobot_sync_surface_cache_locked(HobotSurface *surface, int invalidate)
{
    if (!surface)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    VAStatus status = VA_STATUS_SUCCESS;
    int fd = -1;
    uint64_t size = 0;
    if (surface->has_decoded_frame) {
        const media_codec_buffer_t *frame = &surface->vpu_out_buf;
        if (frame->vframe_buf.fd[1] != 0 &&
            frame->vframe_buf.fd[1] != frame->vframe_buf.fd[0])
            return VA_STATUS_ERROR_OPERATION_FAILED;
        fd = frame->vframe_buf.fd[0];
        size = frame->vframe_buf.size;
    } else if (surface->has_preallocated) {
        HobotPreallocatedNV12Layout layout;
        status = hobot_get_preallocated_nv12_layout(surface, &layout);
        if (status != VA_STATUS_SUCCESS || layout.separate_fds ||
            !surface->preallocated_gbuf.is_contig)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        fd = surface->preallocated_gbuf.fd[0];
        size = layout.object_size[0];
    } else {
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (fd < 0 || size == 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    int ret = invalidate ? hb_mem_invalidate_buf(fd, 0, size) :
                           hb_mem_flush_buf(fd, 0, size);
    if (ret != 0) {
        va_trace("derived image cache %s failed: surface=%u fd=%d size=%llu ret=%d",
                 invalidate ? "invalidate" : "flush", surface->id, fd,
                 (unsigned long long)size, ret);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    return VA_STATUS_SUCCESS;
}

typedef struct HobotExternalNV12Frame {
    unsigned char *y;
    unsigned char *uv;
    uint64_t phy_y;
    uint64_t phy_uv;
    uint32_t y_stride;
    uint32_t uv_stride;
    uint32_t vertical_stride;
    uint32_t size;
    uint32_t y_size;
    uint32_t uv_size;
    int fd;
} HobotExternalNV12Frame;

static int hobot_get_external_nv12_frame(HobotSurface *surface,
                                         HobotExternalNV12Frame *frame) {
    if (!surface || !frame)
        return -1;
    memset(frame, 0, sizeof(*frame));
    frame->fd = -1;

    if (surface->has_decoded_frame) {
        HobotDecodedNV12Layout layout;
        const media_codec_buffer_t *decoded = &surface->vpu_out_buf;
        if (hobot_get_decoded_nv12_planes(surface, &layout) != VA_STATUS_SUCCESS ||
            !layout.contiguous || decoded->vframe_buf.fd[0] < 0 ||
            (decoded->vframe_buf.fd[1] != 0 &&
             decoded->vframe_buf.fd[1] != decoded->vframe_buf.fd[0]) ||
            decoded->vframe_buf.phy_ptr[0] == 0 ||
            decoded->vframe_buf.phy_ptr[1] == 0 ||
            decoded->vframe_buf.phy_ptr[1] <= decoded->vframe_buf.phy_ptr[0] ||
            decoded->vframe_buf.phy_ptr[1] - decoded->vframe_buf.phy_ptr[0] !=
                layout.uv_offset ||
            decoded->vframe_buf.size > UINT32_MAX)
            return -1;
        frame->y = (unsigned char *)layout.y_plane;
        frame->uv = (unsigned char *)layout.uv_plane;
        frame->phy_y = decoded->vframe_buf.phy_ptr[0];
        frame->phy_uv = decoded->vframe_buf.phy_ptr[1];
        frame->y_stride = layout.y_stride;
        frame->uv_stride = layout.uv_stride;
        frame->vertical_stride = layout.vertical_rows;
        frame->size = decoded->vframe_buf.size;
        frame->y_size = (uint32_t)((uint64_t)layout.y_stride * surface->height);
        frame->uv_size = (uint32_t)((uint64_t)layout.uv_stride *
                                    ((surface->height + 1u) / 2u));
        frame->fd = decoded->vframe_buf.fd[0];
    } else if (surface->has_preallocated) {
        HobotPreallocatedNV12Layout layout;
        hb_mem_graphic_buf_t *gbuf = &surface->preallocated_gbuf;
        if (hobot_get_preallocated_nv12_layout(surface, &layout) !=
                VA_STATUS_SUCCESS ||
            layout.separate_fds || !gbuf->is_contig || gbuf->fd[0] < 0 ||
            gbuf->phys_addr[0] == 0 || gbuf->phys_addr[1] == 0 ||
            gbuf->phys_addr[1] <= gbuf->phys_addr[0] ||
            gbuf->phys_addr[1] - gbuf->phys_addr[0] != layout.uv_offset ||
            !gbuf->virt_addr[0] || layout.object_size[0] > UINT32_MAX)
            return -1;
        unsigned char *uv = gbuf->virt_addr[1];
        if (!uv) {
            if (layout.uv_offset < layout.y_offset ||
                layout.uv_offset - layout.y_offset > UINTPTR_MAX -
                    (uintptr_t)gbuf->virt_addr[0])
                return -1;
            uv = (unsigned char *)((uintptr_t)gbuf->virt_addr[0] +
                                   (uintptr_t)(layout.uv_offset - layout.y_offset));
        }
        frame->y = gbuf->virt_addr[0];
        frame->uv = uv;
        frame->phy_y = gbuf->phys_addr[0];
        frame->phy_uv = gbuf->phys_addr[1];
        frame->y_stride = layout.stride;
        frame->uv_stride = layout.stride;
        frame->vertical_stride = layout.vstride;
        frame->size = (uint32_t)layout.object_size[0];
        frame->y_size = (uint32_t)layout.y_extent;
        frame->uv_size = (uint32_t)layout.uv_extent;
        frame->fd = gbuf->fd[0];
    } else {
        return -1;
    }

    if (!frame->y || !frame->uv || frame->fd < 0 ||
        frame->phy_y == 0 || frame->phy_uv == 0 ||
        frame->y_stride < surface->width ||
        frame->uv_stride < surface->width ||
        frame->vertical_stride < surface->height || frame->size == 0 ||
        frame->y_size == 0 || frame->uv_size == 0)
        return -1;
    return hobot_sync_surface_cache_locked(surface, 0) == VA_STATUS_SUCCESS ? 0 : -1;
}

static int hobot_fill_external_frame_info(media_codec_buffer_t *buffer,
                                          const HobotExternalNV12Frame *frame,
                                          int width, int height,
                                          hb_ptr user_ptr) {
    if (!buffer || !frame || width < 2 || height < 2 ||
        frame->y_stride < (uint32_t)width ||
        frame->uv_stride < (uint32_t)width || frame->fd < 0 ||
        frame->size == 0)
        return -1;
    uint64_t y_size = (uint64_t)frame->y_stride * (uint32_t)height;
    uint64_t uv_size = (uint64_t)frame->uv_stride * ((uint32_t)height / 2u);
    if (y_size > frame->size || uv_size > frame->size - y_size ||
        y_size > UINT32_MAX || uv_size > UINT32_MAX ||
        y_size + uv_size > frame->size)
        return -1;

    buffer->type = MC_VIDEO_FRAME_BUFFER;
    buffer->vframe_buf.vir_ptr[0] = frame->y;
    buffer->vframe_buf.vir_ptr[1] = frame->uv;
    buffer->vframe_buf.phy_ptr[0] = frame->phy_y;
    buffer->vframe_buf.phy_ptr[1] = frame->phy_uv;
    buffer->vframe_buf.width = width;
    buffer->vframe_buf.height = height;
    buffer->vframe_buf.pix_fmt = MC_PIXEL_FORMAT_NV12;
    if ((buffer->vframe_buf.stride > 0 &&
         (uint32_t)buffer->vframe_buf.stride != frame->y_stride) ||
        (buffer->vframe_buf.vstride > 0 &&
         (uint32_t)buffer->vframe_buf.vstride != frame->uv_stride))
        return -1;
    buffer->vframe_buf.size = frame->size;
    buffer->user_ptr = user_ptr;
    return 0;
}

static int hobot_get_encoder_scratch_frame(HobotContext *hctx,
                                           HobotExternalNV12Frame *frame) {
    if (!hctx || !frame || !hctx->enc_scratch_allocated)
        return -1;
    hb_mem_graphic_buf_t *gbuf = &hctx->enc_scratch_buf;
    uint32_t width = (uint32_t)hctx->vpu_ctx.video_enc_params.width;
    uint32_t height = (uint32_t)hctx->vpu_ctx.video_enc_params.height;
    HobotSurface scratch_surface;
    memset(&scratch_surface, 0, sizeof(scratch_surface));
    scratch_surface.width = width;
    scratch_surface.height = height;
    scratch_surface.stride = gbuf->stride;
    scratch_surface.has_preallocated = 1;
    scratch_surface.preallocated_gbuf = *gbuf;
    HobotPreallocatedNV12Layout layout;
    if (gbuf->fd[0] < 0 || !gbuf->is_contig ||
        (gbuf->fd[1] >= 0 && gbuf->fd[1] != gbuf->fd[0]) ||
        !gbuf->virt_addr[0] || !gbuf->virt_addr[1] ||
        gbuf->phys_addr[0] == 0 || gbuf->phys_addr[1] == 0 ||
        hobot_get_preallocated_nv12_layout(&scratch_surface, &layout) !=
            VA_STATUS_SUCCESS || layout.object_size[0] > UINT32_MAX)
        return -1;

    memset(frame, 0, sizeof(*frame));
    frame->y = gbuf->virt_addr[0];
    frame->uv = gbuf->virt_addr[1];
    frame->phy_y = gbuf->phys_addr[0];
    frame->phy_uv = gbuf->phys_addr[1];
    frame->y_stride = (uint32_t)gbuf->stride;
    frame->uv_stride = (uint32_t)gbuf->stride;
    frame->vertical_stride = (uint32_t)gbuf->vstride;
    frame->size = (uint32_t)layout.object_size[0];
    frame->y_size = (uint32_t)layout.y_extent;
    frame->uv_size = (uint32_t)layout.uv_extent;
    frame->fd = gbuf->fd[0];
    return 0;
}

static VAStatus hobot_prepare_derived_image_cpu_access_locked(
    HobotDriverData *drv,
    VABufferID buffer_id)
{
    if (buffer_id <= 0 || buffer_id >= MAX_BUFFERS ||
        !drv->buffers[buffer_id].allocated || !drv->buffers[buffer_id].is_derived)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    HobotSurface *surface = NULL;
    VAStatus status = hobot_get_derived_image_surface_locked(
        drv, buffer_id, &surface);
    if (status != VA_STATUS_SUCCESS)
        return status;
    if (surface->cpu_access_count > 0)
        return VA_STATUS_ERROR_SURFACE_BUSY;

    if (surface->cpu_cache_flush_pending) {
        status = hobot_sync_surface_cache_locked(surface, 0);
        if (status != VA_STATUS_SUCCESS)
            return status;
        surface->cpu_cache_flush_pending = 0;
    }
    return hobot_sync_surface_cache_locked(surface, 1);
}

static VAStatus hobot_finish_derived_image_cpu_write_locked(
    HobotDriverData *drv,
    VABufferID buffer_id)
{
    if (buffer_id <= 0 || buffer_id >= MAX_BUFFERS ||
        !drv->buffers[buffer_id].allocated || !drv->buffers[buffer_id].is_derived)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    HobotSurface *surface = NULL;
    VAStatus status = hobot_get_derived_image_surface_locked(
        drv, buffer_id, &surface);
    if (status != VA_STATUS_SUCCESS)
        return status;
    surface->raw_data_valid = 0;
    status = hobot_sync_surface_cache_locked(surface, 0);
    surface->cpu_cache_flush_pending = status != VA_STATUS_SUCCESS;
    return status;
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
            (!hctx->vpu_running && !hctx->encoder_init_deferred &&
             !hctx->jpeg_init_deferred)) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_CONTEXT;
        }
    }
    if (!hctx->vpu_running && !hctx->encoder_init_deferred &&
        !hctx->jpeg_init_deferred) {
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
    if (hobot_surface_has_active_encoder(drv, render_target)) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
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
    if (surf->cpu_cache_flush_pending) {
        VAStatus cache_status = hobot_sync_surface_cache_locked(surf, 0);
        if (cache_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return cache_status;
        }
        surf->cpu_cache_flush_pending = 0;
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
        hobot_retain_context_surface_usage(drv, context, render_target);
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
        } else if (hctx->profile == VAProfileH264ConstrainedBaseline ||
                   hctx->profile == VAProfileH264Main ||
                   hctx->profile == VAProfileH264High) {
            hctx->h264_decode_picture_valid = 0;
            memset(&hctx->h264_decode_picture, 0,
                   sizeof(hctx->h264_decode_picture));
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
        hobot_retain_context_surface_usage(drv, context, render_target);
        surf->context_id = context;
        hctx->current_render_target = render_target;
        hctx->enc_coded_buf = 0;
        hctx->encoder_picture_active = 1;
        hctx->h264_encode_slice_valid = 0;
        hctx->h264_encode_slice_type = 0;
        hctx->h264_encode_pic_qp_valid = 0;
        hctx->h264_encode_pic_qp = 0;
        hctx->h264_encode_slice_qp_delta = 0;
        hctx->hevc_encode_pic_qp_valid = 0;
        hctx->hevc_encode_pic_qp = 0;
        hctx->hevc_encode_slice_qp_delta_valid = 0;
        hctx->hevc_encode_slice_qp_delta = 0;
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
    if (!hctx->vpu_running && !hctx->encoder_init_deferred &&
        !hctx->jpeg_init_deferred) {
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
    VAEncPictureParameterBufferH264 *h264_encode_picture = NULL;
    VAEncSequenceParameterBufferH264 *h264_encode_sequence = NULL;
    VAEncSliceParameterBufferH264 *h264_encode_slice = NULL;
    VAPictureParameterBufferJPEGBaseline *jpeg_decode_picture = NULL;
    VAIQMatrixBufferJPEGBaseline *jpeg_decode_qmatrix = NULL;
    VAHuffmanTableBufferJPEGBaseline *jpeg_decode_huffman = NULL;
    VASliceParameterBufferJPEGBaseline *jpeg_decode_slice = NULL;
    VAPictureParameterBufferHEVC *hevc_decode_picture = NULL;
    VAIQMatrixBufferHEVC *hevc_decode_qmatrix = NULL;
    VAPictureParameterBufferH264 *h264_decode_picture = NULL;
    VAIQMatrixBufferJPEGBaseline jpeg_decode_qmatrix_effective = {0};
    VAHuffmanTableBufferJPEGBaseline jpeg_decode_huffman_effective = {0};
    VAPictureParameterBufferJPEGBaseline jpeg_decode_picture_candidate =
        hctx->jpeg_decode_picture;
    int jpeg_decode_picture_candidate_valid = hctx->jpeg_decode_picture_valid;
    VAIQMatrixBufferJPEGBaseline jpeg_decode_qmatrix_candidate =
        hctx->jpeg_decode_qmatrix;
    uint8_t jpeg_decode_qmatrix_candidate_valid[4];
    memcpy(jpeg_decode_qmatrix_candidate_valid,
           hctx->jpeg_decode_qmatrix_valid,
           sizeof(jpeg_decode_qmatrix_candidate_valid));
    VAHuffmanTableBufferJPEGBaseline jpeg_decode_huffman_candidate =
        hctx->jpeg_decode_huffman;
    uint8_t jpeg_decode_huffman_candidate_valid[2];
    memcpy(jpeg_decode_huffman_candidate_valid,
           hctx->jpeg_decode_huffman_valid,
           sizeof(jpeg_decode_huffman_candidate_valid));
    VASliceParameterBufferJPEGBaseline jpeg_decode_slice_candidate =
        hctx->jpeg_decode_slice;
    int jpeg_decode_slice_candidate_valid = hctx->jpeg_decode_slice_valid;
    uint8_t jpeg_decode_header[1024];
    size_t jpeg_decode_header_size = 0;
    int sequence_parameter_seen = 0;
    int h264_encode_slice_seen = 0;
    const VAEncMiscParameterHRD *h264_hrd = NULL;
    const VAEncMiscParameterHRD *hevc_hrd = NULL;
    int vbr_rate_control_seen = 0;
    uint64_t h264_target_bitrate_bps = hctx->rate_control == VA_RC_CBR ?
        (uint64_t)hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate * 1000u :
        hctx->rate_control == VA_RC_VBR ?
            (uint64_t)hctx->vpu_ctx.video_enc_params.rc_params.h264_avbr_params.bit_rate * 1000u : 0;
    uint64_t hevc_target_bitrate_bps = hctx->rate_control == VA_RC_CBR ?
        (uint64_t)hctx->vpu_ctx.video_enc_params.rc_params.h265_cbr_params.bit_rate * 1000u :
        hctx->rate_control == VA_RC_VBR ?
            (uint64_t)hctx->vpu_ctx.video_enc_params.rc_params.h265_avbr_params.bit_rate * 1000u : 0;
    VABufferID slice_param_ids[MAX_BUFFERS];
    VABufferID slice_data_ids[MAX_BUFFERS];
    uint64_t hevc_tile_entry_point_counts[MAX_BUFFERS] = {0};
    int slice_param_count = 0;
    int slice_data_count = 0;
    unsigned int h264_pps_l0_default = 0;
    unsigned int h264_pps_l1_default = 0;
    int h264_pps_defaults_from_slices = 0;
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
        if (buffer->map_count > 0 || buffer->handle_acquired) {
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

                if (!hobot_buffer_is_single_record(buffer, required_size)) {
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
                             &hctx->hevc_encode_picture, hevc_encode_picture)) ||
                        (hctx->rate_control != VA_RC_CQP &&
                         hctx->hevc_encode_picture_valid &&
                         hctx->hevc_encode_picture.pic_init_qp !=
                             hevc_encode_picture->pic_init_qp)) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                    }
                    if (!hobot_hevc_current_surface_valid(
                            drv, &hevc_encode_picture->decoded_curr_pic)) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_SURFACE;
                    }
                    if (!hobot_hevc_reference_surfaces_valid(
                            drv, hevc_encode_picture->reference_frames,
                            sizeof(hevc_encode_picture->reference_frames) /
                                sizeof(hevc_encode_picture->reference_frames[0]))) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_SURFACE;
                    }
                } else if (hctx->profile != VAProfileJPEGBaseline) {
                    if (h264_encode_picture) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    VAEncPictureParameterBufferH264 *picture =
                        (VAEncPictureParameterBufferH264 *)buffer->data;
                    if (hctx->rate_control == VA_RC_CQP &&
                        picture->pic_init_qp > 51u) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    h264_encode_picture = picture;
                    if (!hobot_h264_current_surface_valid(drv, &picture->CurrPic) ||
                        !hobot_h264_reference_surfaces_valid(
                            drv, picture->ReferenceFrames,
                            sizeof(picture->ReferenceFrames) /
                                sizeof(picture->ReferenceFrames[0]))) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_SURFACE;
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
                if (required_size != 0 &&
                    !hobot_buffer_is_single_record(buffer, required_size)) {
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
                    if ((hctx->rate_control == VA_RC_CBR ||
                         hctx->rate_control == VA_RC_VBR) &&
                        !vbr_rate_control_seen &&
                        hevc_encode_sequence->bits_per_second > 0)
                        hevc_target_bitrate_bps =
                            hevc_encode_sequence->bits_per_second;
                } else if (hctx->profile == VAProfileH264ConstrainedBaseline ||
                           hctx->profile == VAProfileH264Main ||
                           hctx->profile == VAProfileH264High) {
                    h264_encode_sequence =
                        (VAEncSequenceParameterBufferH264 *)buffer->data;
                }
                if ((hctx->profile == VAProfileH264ConstrainedBaseline ||
                     hctx->profile == VAProfileH264Main ||
                     hctx->profile == VAProfileH264High) &&
                    (hctx->rate_control == VA_RC_CBR ||
                     (hctx->rate_control == VA_RC_VBR &&
                      !vbr_rate_control_seen)) &&
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
                    hctx->rate_control == VA_RC_CBR &&
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
                    if (hctx->rate_control == VA_RC_VBR) {
                        HobotVbrRateControl parsed;
                        if (!hobot_parse_vbr_rate_control(
                                (const VAEncMiscParameterRateControl *)misc->data,
                                &parsed)) {
                            pthread_mutex_unlock(&drv->mutex);
                            return VA_STATUS_ERROR_INVALID_PARAMETER;
                        }
                        vbr_rate_control_seen = 1;
                        if (hctx->profile == VAProfileHEVCMain)
                            hevc_target_bitrate_bps =
                                (uint64_t)parsed.target_bitrate_kbps * 1000u;
                        else
                            h264_target_bitrate_bps =
                                (uint64_t)parsed.target_bitrate_kbps * 1000u;
                    }
                    uint32_t requested_bps =
                        ((VAEncMiscParameterRateControl *)misc->data)->bits_per_second;
                    uint32_t requested_kbps = requested_bps / 1000u;
                    if (hctx->rate_control == VA_RC_CQP && requested_bps > 0) {
                        /* CQP ignores target bitrate; per-frame QP comes from picture/slice params. */
                    } else if (hctx->profile == VAProfileHEVCMain && requested_kbps > 0)
                        hevc_target_bitrate_bps = (uint64_t)requested_kbps * 1000u;
                    else if (hctx->rate_control == VA_RC_CBR && requested_kbps > 0)
                        h264_target_bitrate_bps = (uint64_t)requested_kbps * 1000u;
                } else if (misc->type == VAEncMiscParameterTypeHRD) {
                    if (hctx->rate_control == VA_RC_CQP) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                    }
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
                if (!hobot_buffer_is_single_record(
                        buffer, sizeof(VAEncSliceParameterBufferHEVC)) ||
                    hevc_encode_slice) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                hevc_encode_slice =
                    (VAEncSliceParameterBufferHEVC *)buffer->data;
            } else if (buffer->type == VAEncSliceParameterBufferType &&
                       (hctx->profile == VAProfileH264ConstrainedBaseline ||
                        hctx->profile == VAProfileH264Main ||
                        hctx->profile == VAProfileH264High)) {
                if (!hobot_buffer_is_single_record(
                        buffer, sizeof(VAEncSliceParameterBufferH264))) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                if (h264_encode_slice_seen || hctx->h264_encode_slice_valid) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
                }
                h264_encode_slice_seen = 1;

                h264_encode_slice =
                    (VAEncSliceParameterBufferH264 *)buffer->data;
                const VAEncSliceParameterBufferH264 *slice = h264_encode_slice;
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
                if (!hobot_buffer_is_single_record(
                        buffer, sizeof(VAQMatrixBufferJPEG)) || jpeg_qmatrix) {
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
                if (!hobot_buffer_is_single_record(
                        buffer, sizeof(VAHuffmanTableBufferJPEGBaseline)) ||
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
                if (!hobot_buffer_is_single_record(
                        buffer, sizeof(VAEncSliceParameterBufferJPEG)) ||
                    jpeg_slice) {
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
                if (!hobot_buffer_is_single_record(buffer, required_size)) {
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
                    if (!hobot_hevc_current_surface_valid(
                            drv, &hevc_decode_picture->CurrPic) ||
                        hevc_decode_picture->CurrPic.picture_id !=
                            hctx->current_render_target) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_SURFACE;
                    }
                    if (!hobot_hevc_reference_surfaces_valid(
                            drv, hevc_decode_picture->ReferenceFrames,
                            sizeof(hevc_decode_picture->ReferenceFrames) /
                                sizeof(hevc_decode_picture->ReferenceFrames[0]))) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_SURFACE;
                    }
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
                if (hctx->profile == VAProfileH264ConstrainedBaseline ||
                    hctx->profile == VAProfileH264Main ||
                    hctx->profile == VAProfileH264High) {
                    if (h264_decode_picture) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    h264_decode_picture =
                        (VAPictureParameterBufferH264 *)buffer->data;
                }
                if (hctx->profile != VAProfileHEVCMain &&
                    hctx->profile != VAProfileJPEGBaseline &&
                    !hobot_h264_picture_parameters_supported(
                        (VAPictureParameterBufferH264 *)buffer->data,
                        hctx->profile, hctx->width, hctx->height)) {
                    const VAPictureParameterBufferH264 *picture =
                        (const VAPictureParameterBufferH264 *)buffer->data;
                    va_trace("vaRenderPicture: rejected H.264 picture parameters profile=%d frame_only=%u field_pic=%u chroma=%u field_flags=0x%x mbs=%ux%u context=%dx%d",
                             hctx->profile,
                             picture->seq_fields.bits.frame_mbs_only_flag,
                             picture->pic_fields.bits.field_pic_flag,
                             picture->seq_fields.bits.chroma_format_idc,
                             picture->CurrPic.flags &
                                 (VA_PICTURE_H264_TOP_FIELD |
                                  VA_PICTURE_H264_BOTTOM_FIELD),
                             (unsigned int)picture->picture_width_in_mbs_minus1 + 1u,
                             (unsigned int)picture->picture_height_in_mbs_minus1 + 1u,
                             hctx->width, hctx->height);
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                if (hctx->profile != VAProfileHEVCMain &&
                    hctx->profile != VAProfileJPEGBaseline) {
                    const VAPictureParameterBufferH264 *picture =
                        (const VAPictureParameterBufferH264 *)buffer->data;
                    if (!hobot_h264_current_surface_valid(drv, &picture->CurrPic) ||
                        picture->CurrPic.picture_id != hctx->current_render_target) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_SURFACE;
                    }
                }
                if (hctx->profile != VAProfileHEVCMain &&
                    hctx->profile != VAProfileJPEGBaseline &&
                    !hobot_h264_reference_surfaces_valid(
                        drv,
                        ((VAPictureParameterBufferH264 *)buffer->data)->ReferenceFrames,
                        sizeof(((VAPictureParameterBufferH264 *)buffer->data)->ReferenceFrames) /
                            sizeof(((VAPictureParameterBufferH264 *)buffer->data)->ReferenceFrames[0]))) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_SURFACE;
                }
            } else if (hctx->profile == VAProfileJPEGBaseline &&
                       buffer->type == VAIQMatrixBufferType) {
                if (!hobot_buffer_is_single_record(
                        buffer, sizeof(VAIQMatrixBufferJPEGBaseline)) ||
                    jpeg_decode_qmatrix) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                jpeg_decode_qmatrix = (VAIQMatrixBufferJPEGBaseline *)buffer->data;
            } else if (hctx->profile == VAProfileHEVCMain &&
                       buffer->type == VAIQMatrixBufferType) {
                if (!hobot_buffer_is_single_record(
                        buffer, sizeof(VAIQMatrixBufferHEVC)) ||
                    hevc_decode_qmatrix) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                hevc_decode_qmatrix = (VAIQMatrixBufferHEVC *)buffer->data;
            } else if (hctx->profile == VAProfileJPEGBaseline &&
                       buffer->type == VAHuffmanTableBufferType) {
                if (!hobot_buffer_is_single_record(
                        buffer, sizeof(VAHuffmanTableBufferJPEGBaseline)) ||
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

    if (hctx->is_encoder &&
        (hctx->profile == VAProfileH264ConstrainedBaseline ||
         hctx->profile == VAProfileH264Main ||
         hctx->profile == VAProfileH264High)) {
        const VAEncSequenceParameterBufferH264 *effective_sequence =
            h264_encode_sequence ? h264_encode_sequence :
            (hctx->h264_sequence_valid ? &hctx->h264_sequence : NULL);
        int effective_slice_type = h264_encode_slice ?
            h264_encode_slice->slice_type :
            (hctx->h264_encode_slice_valid ? hctx->h264_encode_slice_type : -1);
        if (effective_slice_type >= 0 && effective_sequence &&
            effective_sequence->max_num_ref_frames == 0 &&
            (unsigned int)effective_slice_type % 5u != 2u) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        }
    }

    if (!hctx->is_encoder && hctx->profile == VAProfileJPEGBaseline) {
        if (jpeg_decode_picture) {
            if (jpeg_decode_picture_candidate_valid) {
                va_trace("vaRenderPicture: duplicate JPEG picture parameters");
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
            jpeg_decode_picture_candidate = *jpeg_decode_picture;
            jpeg_decode_picture_candidate_valid = 1;
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
                            memset(jpeg_decode_qmatrix_candidate.quantiser_table[i], 0, 64);
                            jpeg_decode_qmatrix_candidate_valid[i] = 0;
                            continue;
                        }
                        va_trace("vaRenderPicture: malformed JPEG quantizer table=%zu", i);
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    memcpy(jpeg_decode_qmatrix_candidate.quantiser_table[i],
                           jpeg_decode_qmatrix->quantiser_table[i], 64);
                    jpeg_decode_qmatrix_candidate_valid[i] = 1;
                } else if (!jpeg_decode_qmatrix_candidate_valid[i] && table_has_data) {
                    memcpy(jpeg_decode_qmatrix_candidate.quantiser_table[i],
                           jpeg_decode_qmatrix->quantiser_table[i], 64);
                    jpeg_decode_qmatrix_candidate_valid[i] = 1;
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
                    !jpeg_decode_huffman_candidate_valid[table]) {
                    size_t dc_values = 0;
                    size_t ac_values = 0;
                    const typeof(jpeg_decode_huffman->huffman_table[0]) *src =
                        &jpeg_decode_huffman->huffman_table[table];
                    int has_data = 0;
                    const uint8_t *raw = (const uint8_t *)src;
                    for (size_t i = 0; i < sizeof(*src); i++)
                        has_data |= raw[i] != 0;
                    if (!has_data) {
                        memset(&jpeg_decode_huffman_candidate.huffman_table[table],
                               0, sizeof(*src));
                        jpeg_decode_huffman_candidate_valid[table] = 0;
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
                    jpeg_decode_huffman_candidate.huffman_table[table] = *src;
                    jpeg_decode_huffman_candidate_valid[table] = 1;
                }
            }
        }
        if (jpeg_decode_slice) {
            if (jpeg_decode_slice_candidate_valid) {
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
            jpeg_decode_slice_candidate = *jpeg_decode_slice;
            jpeg_decode_slice_candidate_valid = 1;
        }

        jpeg_decode_picture = jpeg_decode_picture_candidate_valid ?
            &jpeg_decode_picture_candidate : NULL;
        jpeg_decode_slice = jpeg_decode_slice_candidate_valid ?
            &jpeg_decode_slice_candidate : NULL;
        if (jpeg_decode_picture && hctx->jpeg_rotation_fixed &&
            jpeg_decode_picture->rotation != hctx->jpeg_rotation) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        }
        for (size_t i = 0; i < 4; i++) {
            if (jpeg_decode_qmatrix_candidate_valid[i]) {
                jpeg_decode_qmatrix_effective.load_quantiser_table[i] = 1;
                memcpy(jpeg_decode_qmatrix_effective.quantiser_table[i],
                       jpeg_decode_qmatrix_candidate.quantiser_table[i], 64);
            }
        }
        for (size_t table = 0; table < 2; table++) {
            if (jpeg_decode_huffman_candidate_valid[table]) {
                jpeg_decode_huffman_effective.load_huffman_table[table] = 1;
                jpeg_decode_huffman_effective.huffman_table[table] =
                    jpeg_decode_huffman_candidate.huffman_table[table];
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
        if (hctx->rate_control == VA_RC_CQP) {
            int effective_qp =
                (int)hevc_encode_picture->pic_init_qp +
                hevc_encode_slice->slice_qp_delta;
            if (effective_qp < 0 || effective_qp > 51) {
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
        }
        if (!hobot_hevc_encode_slice_supported(
                sequence, hevc_encode_picture, hevc_encode_slice,
                hctx->width, hctx->height, hctx->rate_control)) {
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
        hctx->jpeg_decode_picture = jpeg_decode_picture_candidate;
        hctx->jpeg_decode_picture_valid = jpeg_decode_picture_candidate_valid;
        hctx->jpeg_decode_qmatrix = jpeg_decode_qmatrix_candidate;
        memcpy(hctx->jpeg_decode_qmatrix_valid,
               jpeg_decode_qmatrix_candidate_valid,
               sizeof(jpeg_decode_qmatrix_candidate_valid));
        hctx->jpeg_decode_huffman = jpeg_decode_huffman_candidate;
        memcpy(hctx->jpeg_decode_huffman_valid,
               jpeg_decode_huffman_candidate_valid,
               sizeof(jpeg_decode_huffman_candidate_valid));
        hctx->jpeg_decode_slice = jpeg_decode_slice_candidate;
        hctx->jpeg_decode_slice_valid = jpeg_decode_slice_candidate_valid;
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
            int has_next_slice = slice_index + 1u < total_slices;
            uint32_t next_slice_address = 0;
            if (has_next_slice) {
                size_t next_param_index = packed_slices ? 0 : slice_index + 1u;
                size_t next_param_offset = packed_slices ?
                    (slice_index + 1u) * slice_params->element_size : 0;
                const HobotBuffer *next_params =
                    &drv->buffers[slice_param_ids[next_param_index]];
                if (!next_params->data ||
                    next_params->element_size < sizeof(VASliceParameterBufferHEVC) ||
                    next_param_offset > next_params->size ||
                    sizeof(VASliceParameterBufferHEVC) >
                        next_params->size - next_param_offset) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                const VASliceParameterBufferHEVC *next_slice =
                    (const VASliceParameterBufferHEVC *)
                        ((const uint8_t *)next_params->data + next_param_offset);
                next_slice_address = next_slice->slice_segment_address;
            }
            if (!hobot_hevc_slice_sequence_add_with_next(
                    &sequence, slice, slice_data->size,
                    has_next_slice, next_slice_address,
                    &hevc_tile_entry_point_counts[slice_index])) {
                va_trace("vaRenderPicture: invalid HEVC slice sequence at index=%zu address=%u flags=0x%x last=%u data=%u/%u+%u entry_points=%u expected=%llu tiles=%u wpp=%u",
                         slice_index, slice->slice_segment_address,
                         slice->LongSliceFlags.value,
                         slice->LongSliceFlags.fields.LastSliceOfPic,
                         slice->slice_data_flag, slice->slice_data_size,
                         slice->slice_data_offset,
                         slice->num_entry_point_offsets,
                         (unsigned long long)
                             sequence.current_tile_entry_point_count,
                         picture->pic_fields.bits.tiles_enabled_flag,
                         picture->pic_fields.bits.entropy_coding_sync_enabled_flag);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_INVALID_PARAMETER;
            }
            if (!hobot_hevc_validated_rps_slice_supported(
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

    if (!hctx->is_encoder &&
        (hctx->profile == VAProfileH264ConstrainedBaseline ||
         hctx->profile == VAProfileH264Main ||
         hctx->profile == VAProfileH264High) &&
        slice_param_count > 0) {
        const VAPictureParameterBufferH264 *picture = h264_decode_picture ?
            h264_decode_picture :
            (hctx->h264_decode_picture_valid ? &hctx->h264_decode_picture : NULL);
        if (picture) {
            unsigned int default_l0_active_minus1 = 0;
            unsigned int default_l1_active_minus1 = 0;
            int slice_headers_present = 0;
            if (!hobot_h264_default_reference_counts(
                    drv, picture, slice_param_ids, slice_param_count,
                    slice_data_ids, slice_data_count,
                    &default_l0_active_minus1, &default_l1_active_minus1,
                    &slice_headers_present)) {
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            }

            if (slice_headers_present) {
                h264_pps_l0_default = default_l0_active_minus1;
                h264_pps_l1_default = default_l1_active_minus1;
                h264_pps_defaults_from_slices = 1;
            }
        }
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
                    hobot_retain_hevc_encode_picture_surfaces(drv, context,
                                                              pic);
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
                    hobot_retain_h264_encode_picture_surfaces(drv, context,
                                                              pic);
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
                        if (hctx->rate_control == VA_RC_CQP)
                            pending_rc.h265_fixqp_params.intra_period = seq->intra_period;
                        else if (hctx->rate_control == VA_RC_VBR)
                            pending_rc.h265_avbr_params.intra_period = seq->intra_period;
                        else
                            pending_rc.h265_cbr_params.intra_period = seq->intra_period;
                    }
                    if (hctx->rate_control == VA_RC_VBR &&
                        !vbr_rate_control_seen && seq->bits_per_second > 0) {
                        uint32_t kbps = seq->bits_per_second / 1000u;
                        if (kbps == 0 || kbps > 700000u) {
                            pthread_mutex_unlock(&drv->mutex);
                            return VA_STATUS_ERROR_INVALID_PARAMETER;
                        }
                        pending_rc.h265_avbr_params.bit_rate = kbps;
                        hctx->encoder_max_bitrate_kbps = kbps;
                    } else if (hctx->rate_control == VA_RC_CBR &&
                               seq->bits_per_second > 0) {
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
                        if (hctx->rate_control == VA_RC_CQP)
                            pending_rc.h265_fixqp_params.frame_rate = (uint32_t)fps;
                        else if (hctx->rate_control == VA_RC_VBR)
                            pending_rc.h265_avbr_params.frame_rate = (uint32_t)fps;
                        else
                            pending_rc.h265_cbr_params.frame_rate = (uint32_t)fps;
                    }
                } else if (hctx->profile != VAProfileJPEGBaseline) {
                    if (!b->data || b->size < sizeof(VAEncSequenceParameterBufferH264)) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    VAEncSequenceParameterBufferH264 *seq = (VAEncSequenceParameterBufferH264 *)b->data;
                    if (!hobot_h264_level_supported(seq->level_idc) ||
                        !hobot_h264_encode_reference_pattern_supported(seq)) {
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
                        if (hctx->rate_control == VA_RC_CQP)
                            pending_rc.h264_fixqp_params.intra_period = seq->intra_period;
                        else if (hctx->rate_control == VA_RC_VBR)
                            pending_rc.h264_avbr_params.intra_period = seq->intra_period;
                        else
                            pending_rc.h264_cbr_params.intra_period = seq->intra_period;
                    }
                    if (hctx->rate_control == VA_RC_VBR &&
                        !vbr_rate_control_seen && seq->bits_per_second > 0) {
                        uint32_t kbps = seq->bits_per_second / 1000u;
                        if (kbps == 0 || kbps > 700000u) {
                            pthread_mutex_unlock(&drv->mutex);
                            return VA_STATUS_ERROR_INVALID_PARAMETER;
                        }
                        pending_rc.h264_avbr_params.bit_rate = kbps;
                        hctx->encoder_max_bitrate_kbps = kbps;
                    } else if (hctx->rate_control == VA_RC_CBR &&
                               seq->bits_per_second > 0) {
                        pending_rc.h264_cbr_params.bit_rate = seq->bits_per_second / 1000;
                    }
                    uint32_t sequence_fps = 0;
                    int fps_status = hobot_h264_sequence_frame_rate(seq, &sequence_fps);
                    if (fps_status < 0) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    if (fps_status > 0) {
                        if (hctx->rate_control == VA_RC_CQP)
                            pending_rc.h264_fixqp_params.frame_rate = sequence_fps;
                        else if (hctx->rate_control == VA_RC_VBR)
                            pending_rc.h264_avbr_params.frame_rate = sequence_fps;
                        else
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
                    if (hctx->rate_control == VA_RC_VBR) {
                        HobotVbrRateControl parsed;
                        if (!hobot_parse_vbr_rate_control(rc, &parsed)) {
                            pthread_mutex_unlock(&drv->mutex);
                            return VA_STATUS_ERROR_INVALID_PARAMETER;
                        }
                        VAStatus rc_status = hobot_update_vbr_rate_control(
                            hctx, rc, &parsed);
                        if (rc_status != VA_STATUS_SUCCESS) {
                            pthread_mutex_unlock(&drv->mutex);
                            return rc_status;
                        }
                    } else if (hctx->rate_control == VA_RC_CBR && kbps > 0) {
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
                            if (hctx->rate_control == VA_RC_CQP)
                                hctx->vpu_ctx.video_enc_params.rc_params.h264_fixqp_params.frame_rate = fps;
                            else if (hctx->rate_control == VA_RC_VBR)
                                hctx->vpu_ctx.video_enc_params.rc_params.h264_avbr_params.frame_rate = fps;
                            else
                                hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.frame_rate = fps;
                        } else if (hctx->vpu_ctx.codec_id == MEDIA_CODEC_ID_H265) {
                            if (hctx->rate_control == VA_RC_CQP)
                                hctx->vpu_ctx.video_enc_params.rc_params.h265_fixqp_params.frame_rate = fps;
                            else if (hctx->rate_control == VA_RC_VBR)
                                hctx->vpu_ctx.video_enc_params.rc_params.h265_avbr_params.frame_rate = fps;
                            else
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
            if (hctx->rate_control == VA_RC_VBR)
                mctx->video_enc_params.rc_params.h264_avbr_params.vbv_buffer_size =
                    h264_vbv_window_ms;
            else
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
            if (hctx->rate_control == VA_RC_VBR)
                mctx->video_enc_params.rc_params.h265_avbr_params.vbv_buffer_size =
                    hevc_vbv_window_ms;
            else
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
        if (hctx->rate_control == VA_RC_CQP &&
            hctx->vpu_ctx.codec_id == MEDIA_CODEC_ID_H264) {
            int qp_valid = hctx->h264_encode_pic_qp_valid;
            uint8_t pic_qp = hctx->h264_encode_pic_qp;
            int slice_valid = hctx->h264_encode_slice_valid;
            uint8_t slice_type = hctx->h264_encode_slice_type;
            int8_t slice_qp_delta = hctx->h264_encode_slice_qp_delta;
            if (h264_encode_picture) {
                qp_valid = 1;
                pic_qp = h264_encode_picture->pic_init_qp;
            }
            if (h264_encode_slice) {
                slice_valid = 1;
                slice_type = h264_encode_slice->slice_type;
                slice_qp_delta = h264_encode_slice->slice_qp_delta;
            }
            if (qp_valid && slice_valid) {
                int effective_qp = (int)pic_qp + slice_qp_delta;
                if (effective_qp < 0 || effective_qp > 51) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                VAStatus qp_status = hobot_apply_h264_cqp(
                    hctx, (uint32_t)effective_qp);
                if (qp_status != VA_STATUS_SUCCESS) {
                    pthread_mutex_unlock(&drv->mutex);
                    return qp_status;
                }
            }
            hctx->h264_encode_pic_qp_valid = qp_valid;
            hctx->h264_encode_pic_qp = pic_qp;
            hctx->h264_encode_slice_valid = slice_valid;
            hctx->h264_encode_slice_type = slice_type;
            hctx->h264_encode_slice_qp_delta = slice_qp_delta;
        } else if (hctx->rate_control == VA_RC_CQP &&
                   hctx->vpu_ctx.codec_id == MEDIA_CODEC_ID_H265) {
            int qp_valid = hctx->hevc_encode_pic_qp_valid;
            uint8_t pic_qp = hctx->hevc_encode_pic_qp;
            int delta_valid = hctx->hevc_encode_slice_qp_delta_valid;
            int8_t slice_qp_delta = hctx->hevc_encode_slice_qp_delta;
            if (hevc_encode_picture) {
                qp_valid = 1;
                pic_qp = hevc_encode_picture->pic_init_qp;
            }
            if (hevc_encode_slice) {
                delta_valid = 1;
                slice_qp_delta = hevc_encode_slice->slice_qp_delta;
            }
            if (qp_valid && delta_valid) {
                int effective_qp = (int)pic_qp + slice_qp_delta;
                if (effective_qp < 0 || effective_qp > 51) {
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_INVALID_PARAMETER;
                }
                VAStatus qp_status = hobot_apply_hevc_cqp(
                    hctx, (uint32_t)effective_qp);
                if (qp_status != VA_STATUS_SUCCESS) {
                    pthread_mutex_unlock(&drv->mutex);
                    return qp_status;
                }
            }
            hctx->hevc_encode_pic_qp_valid = qp_valid;
            hctx->hevc_encode_pic_qp = pic_qp;
            hctx->hevc_encode_slice_qp_delta_valid = delta_valid;
            hctx->hevc_encode_slice_qp_delta = slice_qp_delta;
        } else if (h264_encode_slice) {
            hctx->h264_encode_slice_valid = 1;
            hctx->h264_encode_slice_type = h264_encode_slice->slice_type;
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
            if (pic->pic_fields.bits.scaling_list_enabled_flag !=
                (hevc_decode_qmatrix != NULL)) {
                va_trace("vaRenderPicture: HEVC scaling-list flag and IQ matrix disagree");
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            }
            if (pic->pic_fields.bits.scaling_list_enabled_flag &&
                !hevc_scaling_matrix_is_default(hevc_decode_qmatrix)) {
                va_trace("vaRenderPicture: non-default HEVC scaling matrices are unsupported");
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            }
            hctx->hevc_decode_picture = *pic;
            hctx->hevc_decode_picture_valid = 1;
            hobot_retain_hevc_picture_surfaces(drv, context, pic);
            uint8_t new_vps[sizeof(hctx->cached_vps)];
            uint8_t new_sps[sizeof(hctx->cached_sps)];
            uint8_t new_pps[sizeof(hctx->cached_pps)];
            int new_vps_len = generate_hevc_vps(pic, 1, new_vps, sizeof(new_vps));
            int new_sps_len = generate_hevc_sps(
                pic, 1, hevc_decode_qmatrix, new_sps, sizeof(new_sps));
            int new_pps_len = generate_hevc_pps(pic, new_pps, sizeof(new_pps));
            if (new_vps_len <= 0 || new_sps_len <= 0 || new_pps_len <= 0) {
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
            }
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
            h264_decode_picture = pic;
            hctx->h264_decode_picture = *pic;
            hctx->h264_decode_picture_valid = 1;
            hobot_retain_h264_picture_surfaces(drv, context, pic);
            unsigned int default_l0_active_minus1 =
                h264_pps_defaults_from_slices ? h264_pps_l0_default :
                ((pic->num_ref_frames >= 3) ? 2 : 0);
            unsigned int default_l1_active_minus1 =
                h264_pps_defaults_from_slices ? h264_pps_l1_default : 0;
            uint8_t new_sps[sizeof(hctx->cached_sps)];
            uint8_t new_pps[sizeof(hctx->cached_pps)];
            int new_sps_len = generate_h264_sps(pic, hctx->profile,
                                                new_sps, sizeof(new_sps));
            int new_pps_len = generate_h264_pps(
                pic, default_l0_active_minus1, default_l1_active_minus1,
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
            if (h264_pps_defaults_from_slices)
                va_trace("vaRenderPicture: H.264 PPS active reference defaults L0=%u L1=%u",
                         default_l0_active_minus1, default_l1_active_minus1);
        }
    }

    if (!hctx->is_encoder && !h264_decode_picture &&
        hctx->h264_decode_picture_valid && h264_pps_defaults_from_slices) {
        uint8_t new_pps[sizeof(hctx->cached_pps)];
        int new_pps_len = generate_h264_pps(
            &hctx->h264_decode_picture, h264_pps_l0_default,
            h264_pps_l1_default, new_pps, sizeof(new_pps));
        if (new_pps_len <= 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        int pps_changed = hobot_header_changed(
            hctx->cached_pps, hctx->cached_pps_len, new_pps, new_pps_len);
        if (pps_changed && hctx->dec_in_buf_valid) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if (pps_changed && hctx->headers_sent) {
            hctx->headers_sent = 0;
            va_trace("vaRenderPicture: H.264 PPS changed; re-injecting parameter sets");
        }
        memcpy(hctx->cached_pps, new_pps, (size_t)new_pps_len);
        hctx->cached_pps_len = new_pps_len;
        va_trace("vaRenderPicture: H.264 PPS active reference defaults L0=%u L1=%u",
                 h264_pps_l0_default, h264_pps_l1_default);
    }

    /* Decoder Second pass: feed slice data (accumulated per picture) */
    int slice_data_index = 0;
    for (int i = 0; i < num_buffers; i++) {
        VABufferID bid = buffers[i];
        HobotBuffer *b = &drv->buffers[bid];
        if (b->type == VASliceDataBufferType) {
                if (hctx->profile == VAProfileJPEGBaseline) {
                    if (slice_data_index++ != 0 || !jpeg_decode_slice ||
                        !jpeg_decode_picture || !jpeg_decode_header_size || !b->data ||
                        jpeg_decode_slice->slice_data_offset > b->size ||
                        jpeg_decode_slice->slice_data_size >
                            b->size - jpeg_decode_slice->slice_data_offset) {
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_INVALID_PARAMETER;
                    }
                    if (hobot_start_deferred_jpeg_decoder(
                            hctx, jpeg_decode_picture->rotation) != 0) {
                        hobot_abort_pending_decode_picture(drv, hctx);
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_OPERATION_FAILED;
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
                size_t slice_data_base_index = (size_t)slice_data_index;
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
                        size_t global_slice_index =
                            (slice_param_count == 1 && slice_data_count == 1) ?
                                (size_t)slice_index :
                                slice_data_base_index + (size_t)slice_index;
                        if (global_slice_index >= MAX_BUFFERS) {
                            int recycle_ret = hobot_recycle_decoder_input(hctx);
                            if (recycle_ret != 0)
                                hctx->decode_failed = 1;
                            hctx->headers_sent = 0;
                            hobot_abort_pending_decode_picture(drv, hctx);
                            pthread_mutex_unlock(&drv->mutex);
                            return VA_STATUS_ERROR_INVALID_PARAMETER;
                        }
                        size_t rewritten_size = 0;
                        int rewrite_status =
                            hobot_hevc_rewrite_rps_slice_with_entry_points(
                            hctx->hevc_decode_picture_valid ?
                                &hctx->hevc_decode_picture : NULL,
                            slice, (const uint8_t *)b->data, b->size,
                            hevc_tile_entry_point_counts[global_slice_index],
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
                            va_trace("vaRenderPicture: rewrote HEVC RPS syntax (sets=%u st_rps_bits=%u offset=%u byte_offset=%u input=%u output=%zu) and realigned slice header",
                                     hctx->hevc_decode_picture.num_short_term_ref_pic_sets,
                                     hctx->hevc_decode_picture.st_rps_bits,
                                     slice->slice_data_offset,
                                     slice->slice_data_byte_offset,
                                     slice->slice_data_size, rewritten_size);
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
        if ((hctx->profile == VAProfileH264ConstrainedBaseline ||
             hctx->profile == VAProfileH264Main ||
             hctx->profile == VAProfileH264High) &&
            !hctx->h264_encode_slice_valid) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if (hctx->rate_control == VA_RC_CQP &&
            hctx->vpu_ctx.codec_id == MEDIA_CODEC_ID_H264 &&
            !hctx->h264_encode_pic_qp_valid) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if (hctx->rate_control == VA_RC_CQP &&
            hctx->vpu_ctx.codec_id == MEDIA_CODEC_ID_H265 &&
            (!hctx->hevc_encode_pic_qp_valid ||
             !hctx->hevc_encode_slice_qp_delta_valid)) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_PARAMETER;
        }
        if (hctx->h264_sequence_valid &&
            hctx->h264_sequence.max_num_ref_frames == 0 &&
            (unsigned int)hctx->h264_encode_slice_type % 5u != 2u) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_ATTR_NOT_SUPPORTED;
        }

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
        media_codec_buffer_t input_slot = in_buf;
        va_trace("encoder slot ctx=%u src_idx=%d stride=%d vstride=%d size=%u frame_end=%d fd=%d/%d",
                 hctx->id, in_buf.vframe_buf.src_idx,
                 in_buf.vframe_buf.stride, in_buf.vframe_buf.vstride,
                 in_buf.vframe_buf.size, in_buf.vframe_buf.frame_end,
                 in_buf.vframe_buf.fd[0], in_buf.vframe_buf.fd[1]);
        if (hctx->enc_external_enabled) {
            int src_idx = in_buf.vframe_buf.src_idx;
            if (src_idx < 0 ||
                src_idx >= (int)hctx->vpu_ctx.video_enc_params.frame_buf_count) {
                fprintf(stderr, "[HOBOT-VA] external encoder returned invalid input slot index %d\n",
                        src_idx);
                int recycle_ret = hobot_recycle_encoder_input(hctx, &input_slot);
                if (recycle_ret != 0)
                    hobot_finish_encoder_picture(hctx, 1);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
        } else if (!in_buf.vframe_buf.vir_ptr[0] ||
                   !in_buf.vframe_buf.vir_ptr[1]) {
            fprintf(stderr, "[HOBOT-VA] dequeue_in returned an invalid NV12 buffer\n");
            int recycle_ret = hobot_recycle_encoder_input(hctx, &input_slot);
            if (recycle_ret != 0) {
                fprintf(stderr, "[HOBOT-VA] invalid input buffer recycle failed: ret=%d\n", recycle_ret);
                hobot_finish_encoder_picture(hctx, 1);
            }
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        if (copy_w < 2 || copy_h < 2 || (copy_w & 1) != 0 ||
            (copy_h & 1) != 0 || (copy_x & 1) != 0 || (copy_y & 1) != 0 ||
            copy_x < 0 || copy_y < 0 || copy_x + copy_w > enc_w ||
            copy_y + copy_h > enc_h) {
            int recycle_ret = hobot_recycle_encoder_input(hctx, &input_slot);
            if (recycle_ret != 0)
                hobot_finish_encoder_picture(hctx, 1);
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_INVALID_SURFACE;
        }

        int dst_y_stride = 0;
        int dst_uv_stride = 0;
        uint64_t dst_y_size = 0;
        uint64_t dst_uv_size = 0;
        if (hctx->enc_external_enabled) {
            int can_pass_surface = src_y && src_uv && copy_x == 0 && copy_y == 0 &&
                                   copy_w == enc_w && copy_h == enc_h;
            HobotExternalNV12Frame frame;
            int direct_surface = 0;
            if (can_pass_surface) {
                if (surf->has_preallocated && surf->raw_data_dirty) {
                    VAStatus upload_status = hobot_upload_staging_to_gbuf(surf);
                    if (upload_status != VA_STATUS_SUCCESS) {
                        int recycle_ret = hobot_recycle_encoder_input(hctx, &input_slot);
                        if (recycle_ret != 0)
                            hobot_finish_encoder_picture(hctx, 1);
                        pthread_mutex_unlock(&drv->mutex);
                        return upload_status;
                    }
                }
                direct_surface = hobot_get_external_nv12_frame(surf, &frame) == 0 &&
                    hobot_fill_external_frame_info(&in_buf, &frame, enc_w, enc_h,
                                                   NULL) == 0;
            }

            if (!direct_surface) {
                if (hobot_get_encoder_scratch_frame(hctx, &frame) != 0) {
                    fprintf(stderr, "[HOBOT-VA] external encoder scratch layout invalid for ctx=%u\n",
                            hctx->id);
                    int recycle_ret = hobot_recycle_encoder_input(hctx, &input_slot);
                    if (recycle_ret != 0)
                        hobot_finish_encoder_picture(hctx, 1);
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_OPERATION_FAILED;
                }
                dst_y_stride = (int)frame.y_stride;
                dst_uv_stride = (int)frame.uv_stride;
                dst_y_size = (uint64_t)(unsigned int)dst_y_stride * (unsigned int)enc_h;
                dst_uv_size = (uint64_t)(unsigned int)dst_uv_stride *
                              ((unsigned int)enc_h / 2u);
                if (dst_y_size > frame.size || dst_uv_size > frame.size - dst_y_size ||
                    dst_y_stride < enc_w || dst_uv_stride < enc_w ||
                    (dst_y_stride & 1) != 0 || (dst_uv_stride & 1) != 0) {
                    int recycle_ret = hobot_recycle_encoder_input(hctx, &input_slot);
                    if (recycle_ret != 0)
                        hobot_finish_encoder_picture(hctx, 1);
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_OPERATION_FAILED;
                }
                if (src_y && src_uv) {
                    if (hobot_copy_nv12_to_coded_frame(
                            src_y, src_uv, src_y_stride, src_uv_stride,
                            frame.y, frame.uv, dst_y_stride, dst_uv_stride,
                            enc_w, enc_h, copy_x, copy_y, copy_w, copy_h) != 0) {
                        int recycle_ret = hobot_recycle_encoder_input(hctx, &input_slot);
                        if (recycle_ret != 0)
                            hobot_finish_encoder_picture(hctx, 1);
                        pthread_mutex_unlock(&drv->mutex);
                        return VA_STATUS_ERROR_OPERATION_FAILED;
                    }
                } else {
                    memset(frame.y, 0x80, (size_t)dst_y_size);
                    memset(frame.uv, 0x80, (size_t)dst_uv_size);
                }
                if (hb_mem_flush_buf(hctx->enc_scratch_buf.fd[0], 0,
                                     frame.size) != 0 ||
                    hobot_fill_external_frame_info(&in_buf, &frame, enc_w, enc_h,
                                                   NULL) != 0) {
                    int recycle_ret = hobot_recycle_encoder_input(hctx, &input_slot);
                    if (recycle_ret != 0)
                        hobot_finish_encoder_picture(hctx, 1);
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_OPERATION_FAILED;
                }
            }

            va_trace("external encoder input ctx=%u surface=%u mode=%s stride=%u vstride=%u",
                     hctx->id, sid, direct_surface ? "direct" : "scratch",
                     frame.y_stride, frame.vertical_stride);

            hctx->enc_external_surface = sid;
        } else {
            if (!in_buf.vframe_buf.vir_ptr[0] || !in_buf.vframe_buf.vir_ptr[1]) {
                fprintf(stderr, "[HOBOT-VA] dequeue_in returned an invalid NV12 buffer\n");
                int recycle_ret = hobot_recycle_encoder_input(hctx, &input_slot);
                if (recycle_ret != 0)
                    hobot_finish_encoder_picture(hctx, 1);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            dst_y_stride = in_buf.vframe_buf.stride > 0 ?
                           in_buf.vframe_buf.stride : enc_w;
            /* The media-codec SDK names the chroma byte pitch vstride. */
            dst_uv_stride = in_buf.vframe_buf.vstride > 0 ?
                            in_buf.vframe_buf.vstride : dst_y_stride;
            dst_y_size = (uint64_t)(unsigned int)dst_y_stride * (unsigned int)enc_h;
            dst_uv_size = (uint64_t)(unsigned int)dst_uv_stride *
                          ((unsigned int)enc_h / 2u);
            int invalid_input_layout = dst_y_stride < enc_w || dst_uv_stride < enc_w ||
                                      (dst_y_stride & 1) != 0 ||
                                      (dst_uv_stride & 1) != 0 ||
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
                int recycle_ret = hobot_recycle_encoder_input(hctx, &input_slot);
                if (recycle_ret != 0)
                    hobot_finish_encoder_picture(hctx, 1);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }

            if (src_y && src_uv) {
                if (hobot_copy_nv12_to_coded_frame(
                        src_y, src_uv, src_y_stride, src_uv_stride,
                        in_buf.vframe_buf.vir_ptr[0],
                        in_buf.vframe_buf.vir_ptr[1], dst_y_stride, dst_uv_stride,
                        enc_w, enc_h, copy_x, copy_y, copy_w, copy_h) != 0) {
                    int recycle_ret = hobot_recycle_encoder_input(hctx, &input_slot);
                    if (recycle_ret != 0)
                        hobot_finish_encoder_picture(hctx, 1);
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_OPERATION_FAILED;
                }
            } else {
                memset(in_buf.vframe_buf.vir_ptr[0], 0x80, (size_t)dst_y_size);
                memset(in_buf.vframe_buf.vir_ptr[1], 0x80, (size_t)dst_uv_size);
            }
        }

        uint64_t current_frame = hctx->frame_count;
        if (!hctx->enc_external_enabled)
            in_buf.vframe_buf.pts = 0;

        uintptr_t input_token = 0;
        if (hctx->enc_external_enabled) {
            input_token = hobot_next_external_input_token();
            in_buf.user_ptr = (hb_ptr)input_token;
            atomic_store_explicit(&hctx->enc_external_input_pending,
                                  input_token, memory_order_release);
        }
        ret = hb_mm_mc_queue_input_buffer(&hctx->vpu_ctx, &in_buf, 1000);
        if (ret != 0) {
            fprintf(stderr, "[HOBOT-VA] queue_in failed: ret=%d\n", ret);
            if (hctx->enc_external_enabled) {
                uintptr_t expected = input_token;
                atomic_compare_exchange_strong_explicit(
                    &hctx->enc_external_input_pending, &expected, 0,
                    memory_order_acq_rel, memory_order_acquire);
            }
            hctx->enc_external_surface = VA_INVALID_SURFACE;
            int recycle_ret = hobot_recycle_encoder_input(
                hctx, hctx->enc_external_enabled ? &input_slot : &in_buf);
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
                int input_ret = recycle_ret == 0 ?
                    hobot_finish_external_encoder_input(hctx, sid, input_token) : -1;
                hobot_finish_encoder_picture(hctx,
                                             recycle_ret != 0 || input_ret != 0);
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
                if (hobot_finish_external_encoder_input(hctx, sid,
                                                        input_token) != 0) {
                    hobot_finish_encoder_picture(hctx, 1);
                    pthread_mutex_unlock(&drv->mutex);
                    return VA_STATUS_ERROR_TIMEDOUT;
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
            if (hobot_finish_external_encoder_input(hctx, sid,
                                                    input_token) != 0) {
                hobot_finish_encoder_picture(hctx, 1);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_TIMEDOUT;
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
            int input_ret = hctx->enc_external_enabled ?
                hobot_finish_external_encoder_input(hctx, sid, input_token) : 0;
            hobot_finish_encoder_picture(hctx, 1);
            pthread_mutex_unlock(&drv->mutex);
            return input_ret != 0 ? VA_STATUS_ERROR_TIMEDOUT :
                                    VA_STATUS_ERROR_OPERATION_FAILED;
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

static VAStatus hobot_vaSyncSurfaceUntilLocked(
    HobotDriverData *drv,
    VASurfaceID render_target,
    const struct timespec *deadline) {
    va_trace("vaSyncSurface: render_target=%u", render_target);
    if (!drv) return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (render_target <= 0 || render_target >= MAX_SURFACES) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    if (!drv->surfaces[render_target].allocated) {
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }
    HobotSurface *surf = &drv->surfaces[render_target];
    if (surf->external_handle_count > 0)
        return VA_STATUS_ERROR_SURFACE_BUSY;
    if (hobot_surface_has_active_encoder(drv, render_target))
        return VA_STATUS_ERROR_SURFACE_BUSY;
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
            int wait_status = hobot_wait_sync_condition(drv, deadline);
            if (wait_status == ETIMEDOUT) {
                if (!surf->decode_pending || surf->decode_error)
                    continue;
                return VA_STATUS_ERROR_TIMEDOUT;
            }
            if (wait_status != 0) {
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            continue;
        }
        if (hctx->decode_failed) {
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (hctx->dec_out_buf_valid) {
            int timed_out = 0;
            int recycle_status = hobot_retry_decoder_output_until(
                hctx, deadline, &timed_out);
            if (recycle_status != 0) {
                return timed_out ? VA_STATUS_ERROR_TIMEDOUT :
                       VA_STATUS_ERROR_OPERATION_FAILED;
            }
        }
        if (hctx->sync_active || hctx->decode_picture_active) {
            int wait_status = hobot_wait_sync_condition(drv, deadline);
            if (wait_status == ETIMEDOUT) {
                if (!surf->decode_pending || surf->decode_error)
                    continue;
                return VA_STATUS_ERROR_TIMEDOUT;
            }
            if (wait_status != 0) {
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
            continue;
        }
        if (deadline) {
            int remaining_ms = hobot_sync_remaining_ms(deadline, 1);
            if (remaining_ms < 0)
                return VA_STATUS_ERROR_OPERATION_FAILED;
            if (remaining_ms == 0)
                return VA_STATUS_ERROR_TIMEDOUT;
        }
        break;
    }
    hctx->sync_active = 1;

    media_codec_context_t *mctx = &hctx->vpu_ctx;
    VAStatus sync_error = VA_STATUS_SUCCESS;

    int max_attempts = 30;
    while (!surf->has_decoded_frame && !surf->decode_error &&
           (deadline || max_attempts-- > 0)) {
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
                int timed_out = 0;
                int qret = hobot_queue_output_until(&drv->contexts[owner],
                                                    &target_surface->vpu_out_buf,
                                                    deadline, &timed_out);
                if (qret != 0) {
                    if (timed_out)
                        return hobot_finish_surface_sync(
                            drv, hctx, VA_STATUS_ERROR_TIMEDOUT);
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

        int dequeue_timeout_ms = 50;
        if (deadline) {
            dequeue_timeout_ms = hobot_sync_remaining_ms(deadline, 50);
            if (dequeue_timeout_ms < 0)
                return hobot_finish_surface_sync(
                    drv, hctx, VA_STATUS_ERROR_OPERATION_FAILED);
            if (dequeue_timeout_ms == 0)
                return hobot_finish_surface_sync(
                    drv, hctx, VA_STATUS_ERROR_TIMEDOUT);
        }
        pthread_mutex_unlock(&drv->mutex);
        int ret = hb_mm_mc_dequeue_output_buffer(mctx, &out_buf, &out_info,
                                                 dequeue_timeout_ms);
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
                /* Return allocated pool buffers even when their frame metadata is invalid. */
                if (out_buf.vframe_buf.phy_ptr[0] != 0 && out_buf.vframe_buf.size > 0) {
                    int timed_out = 0;
                    int qret = hobot_recycle_decoder_output_until(
                        hctx, &out_buf, deadline, &timed_out);
                    if (qret != 0) {
                        if (timed_out)
                            return hobot_finish_surface_sync(
                                drv, hctx, VA_STATUS_ERROR_TIMEDOUT);
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
            int decode_result = HOBOT_DECODE_RESULT_SUCCESS;
            int err_mb = 0;
            int total_mb = 0;
            int disp_idx;
            int deco_idx;
            if (is_jpeg) {
                const mc_mjpeg_jpeg_output_frame_info_t *jpeg_info =
                    &out_info.jpeg_frame_info;
                disp_idx = jpeg_info->frame_display_index;
                deco_idx = -1;
                int swapped = hctx->jpeg_rotation == VA_ROTATION_90 ||
                              hctx->jpeg_rotation == VA_ROTATION_270;
                int input_width = swapped ? hctx->height : hctx->width;
                int input_height = swapped ? hctx->width : hctx->height;
                HobotSurface layout_surface = {0};
                HobotDecodedNV12Layout decoded_layout;
                layout_surface.width = (unsigned int)hctx->width;
                layout_surface.height = (unsigned int)hctx->height;
                layout_surface.vpu_out_buf = out_buf;
                VAStatus layout_status = hobot_get_decoded_nv12_planes(
                    &layout_surface, &decoded_layout);
                if (jpeg_info->decode_result != 1 ||
                    !hctx->jpeg_rotation_fixed ||
                    jpeg_info->display_width != input_width ||
                    jpeg_info->display_height != input_height ||
                    layout_status != VA_STATUS_SUCCESS) {
                    fprintf(stderr,
                            "[HOBOT-VA] JPEG decode failed or layout changed: result=%d stream=%dx%d expected=%dx%d output=%dx%d buffer=%ux%u stride=%d vstride=%d size=%u layout_status=%d\n",
                            jpeg_info->decode_result, jpeg_info->display_width,
                            jpeg_info->display_height, input_width, input_height,
                            hctx->width, hctx->height,
                            out_buf.vframe_buf.width, out_buf.vframe_buf.height,
                            out_buf.vframe_buf.stride, out_buf.vframe_buf.vstride,
                            out_buf.vframe_buf.size, layout_status);
                    if (target > 0 && target < MAX_SURFACES &&
                        drv->surfaces[target].allocated &&
                        drv->surfaces[target].decode_pending) {
                        drv->surfaces[target].decode_pending = 0;
                        drv->surfaces[target].decode_error = 1;
                    }
                    int timed_out = 0;
                    int qret = hobot_recycle_decoder_output_until(
                        hctx, &out_buf, deadline, &timed_out);
                    if (qret != 0) {
                        if (timed_out)
                            return hobot_finish_surface_sync(
                                drv, hctx, VA_STATUS_ERROR_TIMEDOUT);
                        fprintf(stderr, "[HOBOT-VA] JPEG failed output recycle failed: %d\n", qret);
                        hctx->decode_failed = 1;
                        return hobot_finish_surface_sync(
                            drv, hctx, VA_STATUS_ERROR_OPERATION_FAILED);
                    }
                    continue;
                }
            } else {
                decode_result = out_info.video_frame_info.decode_result;
                err_reason = (uint32_t)out_info.video_frame_info.error_reason;
                err_mb = out_info.video_frame_info.err_mb_in_frame_display;
                total_mb = out_info.video_frame_info.total_mb_in_frame_display;
                disp_idx = out_info.video_frame_info.frame_display_index;
                deco_idx = out_info.video_frame_info.frame_decoded_index;
            }

            if (!is_jpeg &&
                (decode_result != HOBOT_DECODE_RESULT_SUCCESS ||
                 err_reason != 0 || err_mb > 0)) {
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
                                "[HOBOT-VA][WATCHDOG] target_surf=%u disp_idx=%d deco_idx=%d decode_result=0x%x\n"
                                "[HOBOT-VA][WATCHDOG] error_reason=0x%08x warn_info=0x%08x\n"
                                "[HOBOT-VA][WATCHDOG] err_mb=%d total_mb=%d (%.1f%% corrupted)\n"
                                "[HOBOT-VA][WATCHDOG] phy=[0x%llx, 0x%llx] fd=%d stride=%d size=%u (%dx%d)\n\n",
                        hctx->watchdog_anomaly_count, HOBOT_WATCHDOG_FALLBACK_THRESHOLD,
                        target, disp_idx, deco_idx, decode_result,
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

            int decode_failed = decode_result != HOBOT_DECODE_RESULT_SUCCESS &&
                                decode_result != HOBOT_DECODE_RESULT_SUCCESS_WITH_WARNING;
            if (!is_jpeg &&
                (decode_failed || err_mb > 0 || (err_reason & 0x00020000))) {
                fprintf(stderr, "[HOBOT-VA][DROP] Dropping failed/corrupt frame (decode_result=0x%x err_mb=%d/%d, err_reason=0x%08x) for target=%u (anomaly=%d/%d)\n",
                        decode_result, err_mb, total_mb, err_reason, target,
                        hctx->watchdog_anomaly_count,
                        HOBOT_WATCHDOG_FALLBACK_THRESHOLD);
                if (target > 0 && target < MAX_SURFACES &&
                    drv->surfaces[target].allocated && drv->surfaces[target].decode_pending) {
                    drv->surfaces[target].decode_pending = 0;
                    drv->surfaces[target].decode_error = 1;
                }
                if (out_buf.vframe_buf.phy_ptr[0] != 0 && out_buf.vframe_buf.size > 0) {
                    int timed_out = 0;
                    int qret = hobot_recycle_decoder_output_until(
                        hctx, &out_buf, deadline, &timed_out);
                    if (qret != 0) {
                        if (timed_out)
                            return hobot_finish_surface_sync(
                                drv, hctx, VA_STATUS_ERROR_TIMEDOUT);
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

static VAStatus hobot_vaSyncSurface2(VADriverContextP ctx,
                                     VASurfaceID surface,
                                     uint64_t timeout_ns) {
    if (!ctx || !ctx->pDriverData)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    struct timespec deadline;
    const struct timespec *deadline_ptr = NULL;
    if (timeout_ns != VA_TIMEOUT_INFINITE) {
        if (hobot_sync_deadline_after(timeout_ns, &deadline) != 0)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        deadline_ptr = &deadline;
    }

    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    int lock_status = hobot_mutex_lock_until(&drv->mutex, deadline_ptr);
    if (lock_status == ETIMEDOUT)
        return VA_STATUS_ERROR_TIMEDOUT;
    if (lock_status != 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    VAStatus status = hobot_vaSyncSurfaceUntilLocked(drv, surface, deadline_ptr);
    pthread_mutex_unlock(&drv->mutex);
    return status;
}

static VAStatus hobot_vaSyncBuffer(VADriverContextP ctx,
                                   VABufferID buffer_id,
                                   uint64_t timeout_ns) {
    if (!ctx || !ctx->pDriverData)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    struct timespec deadline;
    const struct timespec *deadline_ptr = NULL;
    if (timeout_ns != VA_TIMEOUT_INFINITE) {
        if (hobot_sync_deadline_after(timeout_ns, &deadline) != 0)
            return VA_STATUS_ERROR_OPERATION_FAILED;
        deadline_ptr = &deadline;
    }

    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    int lock_status = hobot_mutex_lock_until(&drv->mutex, deadline_ptr);
    if (lock_status == ETIMEDOUT)
        return VA_STATUS_ERROR_TIMEDOUT;
    if (lock_status != 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    if (buffer_id == VA_INVALID_ID || buffer_id >= MAX_BUFFERS ||
        !drv->buffers[buffer_id].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (drv->buffers[buffer_id].handle_acquired) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }

    /* RenderPicture consumes inputs and EndPicture copies output before returning. */
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaSyncSurfaceLocked(HobotDriverData *drv,
                                           VASurfaceID render_target) {
    return hobot_vaSyncSurfaceUntilLocked(drv, render_target, NULL);
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
    if (initial_surface->external_handle_count > 0)
        return VA_STATUS_ERROR_SURFACE_BUSY;
    if (hobot_surface_has_active_encoder(drv, surface))
        return VA_STATUS_ERROR_SURFACE_BUSY;
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
    VAStatus status;
    if (surface <= 0 || surface >= MAX_SURFACES || !drv->surfaces[surface].allocated) {
        status = VA_STATUS_ERROR_INVALID_SURFACE;
    } else {
        HobotSurface *surf = &drv->surfaces[surface];
        if (surf->cpu_access_count > 0) {
            status = VA_STATUS_ERROR_SURFACE_BUSY;
        } else if (surf->cpu_cache_flush_pending &&
                   (status = hobot_sync_surface_cache_locked(surf, 0)) != VA_STATUS_SUCCESS) {
            /* Keep the pending state for the next export attempt. */
        } else {
            surf->cpu_cache_flush_pending = 0;
            status = hobot_fill_surface_info_locked(drv, surface, info);
        }
    }
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
    if (surf->cpu_access_count > 0 || surf->external_handle_count > 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (hobot_surface_has_active_encoder(drv, surface_id)) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
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
    if (surf->cpu_cache_flush_pending) {
        VAStatus flush_status = hobot_sync_surface_cache_locked(surf, 0);
        if (flush_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return flush_status;
        }
        surf->cpu_cache_flush_pending = 0;
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
    HobotImage *img = &drv->images[image];
    HobotSurface *surface = NULL;
    if (img->surface_id != 0) {
        if (img->surface_id <= 0 || img->surface_id >= MAX_SURFACES ||
            !drv->surfaces[img->surface_id].allocated ||
            drv->surfaces[img->surface_id].lock_count == 0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        surface = &drv->surfaces[img->surface_id];
    }
    if (surface && surface->cpu_cache_flush_pending &&
        surface->cpu_access_count == 0) {
        VAStatus flush_status = hobot_sync_surface_cache_locked(surface, 0);
        if (flush_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return flush_status;
        }
        surface->cpu_cache_flush_pending = 0;
    }
    VAStatus buffer_status = hobot_vaDestroyBufferLocked(drv, img->buf_id);
    if (buffer_status != VA_STATUS_SUCCESS) {
        pthread_mutex_unlock(&drv->mutex);
        return buffer_status;
    }
    if (surface)
        surface->lock_count--;
    img->allocated = 0;
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

static VAStatus hobot_vaAcquireBufferHandle(
    VADriverContextP ctx,
    VABufferID buf_id,
    VABufferInfo *buf_info
) {
    if (!ctx || !ctx->pDriverData)
        return VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!buf_info)
        return VA_STATUS_ERROR_INVALID_PARAMETER;
    if (buf_info->mem_type != 0 &&
        buf_info->mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME)
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;

    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    if (buf_id <= 0 || buf_id >= MAX_BUFFERS ||
        !drv->buffers[buf_id].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }

    HobotBuffer *buffer = &drv->buffers[buf_id];
    if (!buffer->is_derived || buffer->type != VAImageBufferType) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
    }
    if (buffer->handle_acquired || buffer->map_count > 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }

    HobotImage *image = NULL;
    for (int i = 1; i < MAX_IMAGES; i++) {
        if (drv->images[i].allocated && drv->images[i].buf_id == buf_id) {
            image = &drv->images[i];
            break;
        }
    }
    if (!image || image->surface_id <= 0 ||
        image->surface_id >= MAX_SURFACES ||
        !drv->surfaces[image->surface_id].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    HobotSurface *surface = &drv->surfaces[image->surface_id];
    if (surface->external_handle_count > 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (surface->cpu_access_count > 0 || surface->decode_error) {
        VAStatus status = surface->decode_error ? VA_STATUS_ERROR_DECODING_ERROR :
                                                  VA_STATUS_ERROR_SURFACE_BUSY;
        pthread_mutex_unlock(&drv->mutex);
        return status;
    }
    if (surface->decode_pending) {
        VAStatus sync_status = hobot_vaSyncSurfaceLocked(drv, image->surface_id);
        if (sync_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return sync_status;
        }
    }
    if (surface->cpu_cache_flush_pending) {
        VAStatus flush_status = hobot_sync_surface_cache_locked(surface, 0);
        if (flush_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return flush_status;
        }
        surface->cpu_cache_flush_pending = 0;
    }

    int surface_fd = -1;
    uint64_t allocation_size = 0;
    if (surface->has_decoded_frame) {
        const media_codec_buffer_t *decoded = &surface->vpu_out_buf;
        HobotDecodedNV12Layout layout;
        VAStatus layout_status = hobot_get_decoded_nv12_planes(surface, &layout);
        if (layout_status != VA_STATUS_SUCCESS || !layout.contiguous ||
            decoded->vframe_buf.fd[0] < 0 ||
            (decoded->vframe_buf.fd[1] != 0 &&
             decoded->vframe_buf.fd[1] != decoded->vframe_buf.fd[0]) ||
            decoded->vframe_buf.phy_ptr[0] == 0 ||
            decoded->vframe_buf.phy_ptr[1] <= decoded->vframe_buf.phy_ptr[0] ||
            decoded->vframe_buf.phy_ptr[1] - decoded->vframe_buf.phy_ptr[0] !=
                layout.uv_offset ||
            buffer->data != layout.y_plane ||
            image->image.format.fourcc != VA_FOURCC_NV12 ||
            image->image.num_planes != 2 ||
            image->image.offsets[0] != 0 ||
            image->image.offsets[1] != layout.uv_offset ||
            image->image.pitches[0] != layout.y_stride ||
            image->image.pitches[1] != layout.uv_stride) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        }
        surface_fd = decoded->vframe_buf.fd[0];
        allocation_size = decoded->vframe_buf.size;
    } else if (surface->has_preallocated) {
        const hb_mem_graphic_buf_t *gbuf = &surface->preallocated_gbuf;
        HobotPreallocatedNV12Layout layout;
        VAStatus layout_status = hobot_get_preallocated_nv12_layout(surface,
                                                                     &layout);
        uint64_t uv_offset = layout_status == VA_STATUS_SUCCESS &&
                             layout.uv_offset >= layout.y_offset ?
                             layout.uv_offset - layout.y_offset : UINT64_MAX;
        if (layout_status != VA_STATUS_SUCCESS || layout.separate_fds ||
            !gbuf->is_contig || gbuf->fd[0] < 0 ||
            gbuf->phys_addr[0] == 0 ||
            gbuf->phys_addr[1] <= gbuf->phys_addr[0] ||
            gbuf->phys_addr[1] - gbuf->phys_addr[0] != uv_offset ||
            buffer->data != gbuf->virt_addr[0] ||
            image->image.format.fourcc != VA_FOURCC_NV12 ||
            image->image.num_planes != 2 ||
            image->image.offsets[0] != 0 ||
            image->image.offsets[1] != uv_offset ||
            image->image.pitches[0] != layout.stride ||
            image->image.pitches[1] != layout.stride) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        }
        surface_fd = gbuf->fd[0];
        allocation_size = layout.object_size[0];
    } else {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
    }

    if (allocation_size == 0 || allocation_size > SIZE_MAX ||
        image->image.data_size > allocation_size ||
        buffer->size < image->image.data_size) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    int export_fd = fcntl(surface_fd, F_DUPFD_CLOEXEC, 0);
    if (export_fd < 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    buffer->handle_acquired = 1;
    buffer->acquired_fd = export_fd;
    surface->external_handle_count++;
    buf_info->handle = (uintptr_t)export_fd;
    buf_info->type = buffer->type;
    buf_info->mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    buf_info->mem_size = (size_t)allocation_size;
    pthread_mutex_unlock(&drv->mutex);
    return VA_STATUS_SUCCESS;
}

static VAStatus hobot_vaReleaseBufferHandle(VADriverContextP ctx,
                                            VABufferID buf_id) {
    if (!ctx || !ctx->pDriverData)
        return VA_STATUS_ERROR_INVALID_CONTEXT;

    HobotDriverData *drv = (HobotDriverData *)ctx->pDriverData;
    pthread_mutex_lock(&drv->mutex);
    if (buf_id <= 0 || buf_id >= MAX_BUFFERS ||
        !drv->buffers[buf_id].allocated) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    HobotBuffer *buffer = &drv->buffers[buf_id];
    if (!buffer->handle_acquired || buffer->acquired_fd < 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    HobotSurface *surface = NULL;
    VAStatus surface_status = hobot_get_derived_image_surface_locked(
        drv, buf_id, &surface);
    if (surface_status != VA_STATUS_SUCCESS) {
        pthread_mutex_unlock(&drv->mutex);
        return surface_status;
    }

    VAStatus cache_status = hobot_sync_surface_cache_locked(surface, 1);
    surface->raw_data_valid = 0;
    surface->raw_data_dirty = 0;
    int export_fd = buffer->acquired_fd;
    buffer->handle_acquired = 0;
    buffer->acquired_fd = -1;
    if (surface->external_handle_count > 0)
        surface->external_handle_count--;
    else
        cache_status = VA_STATUS_ERROR_OPERATION_FAILED;
    if (close(export_fd) != 0) {
        fprintf(stderr, "[HOBOT-VA] vaReleaseBufferHandle: close(%d) failed: %s\n",
                export_fd, strerror(errno));
        cache_status = VA_STATUS_ERROR_OPERATION_FAILED;
    }
    pthread_mutex_unlock(&drv->mutex);
    return cache_status;
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
    if (s->external_handle_count > 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (hobot_surface_has_active_encoder(drv, surface)) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (img->surface_id == surface) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
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
    if (s->cpu_access_count > 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (s->cpu_cache_flush_pending) {
        VAStatus flush_status = hobot_sync_surface_cache_locked(s, 0);
        if (flush_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return flush_status;
        }
        s->cpu_cache_flush_pending = 0;
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
        return img->surface_id != 0 ? VA_STATUS_ERROR_SURFACE_BUSY :
                                      VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (drv->buffers[img->buf_id].handle_acquired) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
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
        VAStatus cache_status = hobot_sync_surface_cache_locked(s, 1);
        if (cache_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return cache_status;
        }
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

    int derived_destination = img->surface_id != 0;
    if (derived_destination) {
        VAStatus cache_status = hobot_prepare_derived_image_cpu_access_locked(
            drv, img->buf_id);
        if (cache_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return cache_status;
        }
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
    if (derived_destination) {
        VAStatus cache_status = hobot_finish_derived_image_cpu_write_locked(
            drv, img->buf_id);
        if (cache_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return cache_status;
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
    HobotSurface *s = &drv->surfaces[surface];
    if (s->external_handle_count > 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (s->decode_error) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_DECODING_ERROR;
    }
    if (s->cpu_cache_flush_pending) {
        VAStatus flush_status = hobot_sync_surface_cache_locked(s, 0);
        if (flush_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return flush_status;
        }
        s->cpu_cache_flush_pending = 0;
    }
    if (hobot_surface_has_active_encoder(drv, surface) ||
        (s->context_id > 0 && s->context_id < MAX_CONTEXTS &&
         drv->contexts[s->context_id].allocated &&
         !drv->contexts[s->context_id].is_encoder &&
         drv->contexts[s->context_id].decode_picture_active &&
         drv->contexts[s->context_id].current_render_target == surface)) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (s->lock_count == UINT32_MAX) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    int img_idx = -1;
    for (int i = 1; i < MAX_IMAGES; i++) {
        if (!drv->images[i].allocated) {
            img_idx = i;
            break;
        }
    }
    int buf_idx = -1;
    for (int i = 1; i < MAX_BUFFERS; i++) {
        if (!drv->buffers[i].allocated) {
            buf_idx = i;
            break;
        }
    }
    if (img_idx < 0 || buf_idx < 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }

    struct hobot_surface_info info;
    VAStatus status = hobot_fill_surface_info_locked(drv, surface, &info);
    if (status != VA_STATUS_SUCCESS) {
        pthread_mutex_unlock(&drv->mutex);
        return status;
    }
    if (!info.virt_addr[0] || !info.virt_addr[1] || info.dma_fd < 0 ||
        info.stride < s->width || info.stride == 0 || info.vstride < s->height) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    uintptr_t y_address = (uintptr_t)info.virt_addr[0];
    uintptr_t uv_address = (uintptr_t)info.virt_addr[1];
    if (uv_address < y_address) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    uint64_t uv_offset = (uint64_t)(uv_address - y_address);
    uint32_t uv_stride = info.stride;
    uint64_t allocation_size = 0;
    uint64_t y_extent = (uint64_t)info.stride * s->height;
    uint64_t uv_extent = (uint64_t)uv_stride * ((s->height + 1u) / 2u);

    if (s->has_decoded_frame) {
        HobotDecodedNV12Layout layout;
        status = hobot_get_decoded_nv12_planes(s, &layout);
        if (status != VA_STATUS_SUCCESS || !layout.contiguous ||
            (s->vpu_out_buf.vframe_buf.fd[1] != 0 &&
             s->vpu_out_buf.vframe_buf.fd[1] != s->vpu_out_buf.vframe_buf.fd[0]) ||
            layout.y_plane != info.virt_addr[0] ||
            layout.uv_plane != info.virt_addr[1] ||
            layout.y_stride != info.stride || layout.uv_stride < s->width ||
            layout.uv_offset != uv_offset) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        uv_stride = layout.uv_stride;
        uv_extent = (uint64_t)uv_stride * ((s->height + 1u) / 2u);
        allocation_size = s->vpu_out_buf.vframe_buf.size;
    } else if (s->has_preallocated) {
        HobotPreallocatedNV12Layout layout;
        status = hobot_get_preallocated_nv12_layout(s, &layout);
        if (status != VA_STATUS_SUCCESS || layout.separate_fds ||
            !s->preallocated_gbuf.is_contig ||
            layout.uv_offset < layout.y_offset ||
            uv_offset != layout.uv_offset - layout.y_offset ||
            info.virt_addr[0] != s->preallocated_gbuf.virt_addr[0] ||
            info.virt_addr[1] != s->preallocated_gbuf.virt_addr[1] ||
            layout.stride != info.stride ||
            layout.y_offset > layout.object_size[0]) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        uv_stride = layout.stride;
        uv_extent = (uint64_t)uv_stride * ((s->height + 1u) / 2u);
        allocation_size = layout.object_size[0] - layout.y_offset;
    } else {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    if (uv_stride < s->width || uv_offset > UINT_MAX || uv_offset < y_extent ||
        uv_offset > UINT64_MAX - uv_extent || uv_offset + uv_extent > allocation_size ||
        uv_offset + uv_extent > UINT_MAX) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    unsigned int image_data_size = (unsigned int)(uv_offset + uv_extent);
    memset(&drv->images[img_idx], 0, sizeof(drv->images[img_idx]));
    memset(&drv->buffers[buf_idx], 0, sizeof(drv->buffers[buf_idx]));
    HobotBuffer *buffer = &drv->buffers[buf_idx];
    buffer->allocated = 1;
    buffer->is_derived = 1;
    buffer->id = (VABufferID)buf_idx;
    buffer->type = VAImageBufferType;
    buffer->size = image_data_size;
    buffer->capacity = image_data_size;
    buffer->element_size = 1;
    buffer->num_elements = image_data_size;
    buffer->data = info.virt_addr[0];

    image->image_id = (VAImageID)img_idx;
    image->format.fourcc = VA_FOURCC_NV12;
    image->format.byte_order = VA_LSB_FIRST;
    image->format.bits_per_pixel = 12;
    image->width = s->width;
    image->height = s->height;
    image->buf = (VABufferID)buf_idx;
    image->data_size = image_data_size;
    image->num_planes = 2;
    image->pitches[0] = info.stride;
    image->pitches[1] = uv_stride;
    image->offsets[0] = 0;
    image->offsets[1] = (unsigned int)uv_offset;

    HobotImage *derived = &drv->images[img_idx];
    derived->allocated = 1;
    derived->id = (VAImageID)img_idx;
    derived->image = *image;
    derived->buf_id = (VABufferID)buf_idx;
    derived->surface_id = surface;
    s->lock_count++;

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
    if (!info.virt_addr[0] || !info.virt_addr[1] ||
        info.stride == 0 || info.vstride == 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    HobotSurface *surf = &drv->surfaces[surface];
    uintptr_t y_address = (uintptr_t)info.virt_addr[0];
    uintptr_t uv_address = (uintptr_t)info.virt_addr[1];
    if (uv_address < y_address) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    uint64_t chroma_offset = (uint64_t)(uv_address - y_address);
    if (surf->has_decoded_frame) {
        int fd0 = surf->vpu_out_buf.vframe_buf.fd[0];
        int fd1 = surf->vpu_out_buf.vframe_buf.fd[1];
        if (fd1 != 0 && fd1 != fd0) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
        }
        uint64_t expected_uv_offset = (uint64_t)info.stride * info.vstride;
        if (chroma_offset != expected_uv_offset) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    } else if (surf->has_preallocated) {
        HobotPreallocatedNV12Layout layout;
        status = hobot_get_preallocated_nv12_layout(surf, &layout);
        if (status != VA_STATUS_SUCCESS || layout.separate_fds ||
            layout.uv_offset < layout.y_offset ||
            layout.stride != info.stride || layout.vstride != info.vstride ||
            chroma_offset != layout.uv_offset - layout.y_offset ||
            chroma_offset > UINT_MAX) {
            pthread_mutex_unlock(&drv->mutex);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
    } else {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (surf->lock_count == UINT32_MAX ||
        surf->va_lock_count == UINT32_MAX ||
        surf->cpu_access_count == UINT32_MAX) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }
    if (surf->cpu_access_count == 0) {
        if (surf->cpu_cache_flush_pending) {
            status = hobot_sync_surface_cache_locked(surf, 0);
            if (status != VA_STATUS_SUCCESS) {
                pthread_mutex_unlock(&drv->mutex);
                return status;
            }
            surf->cpu_cache_flush_pending = 0;
        }
        status = hobot_sync_surface_cache_locked(surf, 1);
        if (status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return status;
        }
    }
    surf->lock_count++;
    surf->va_lock_count++;
    surf->cpu_access_count++;

    if (fourcc) *fourcc = VA_FOURCC_NV12;
    if (luma_stride) *luma_stride = info.stride;
    if (chroma_u_stride) *chroma_u_stride = info.stride;
    if (chroma_v_stride) *chroma_v_stride = info.stride;
    if (luma_offset) *luma_offset = 0;
    if (chroma_u_offset) *chroma_u_offset = (unsigned int)chroma_offset;
    if (chroma_v_offset) *chroma_v_offset = (unsigned int)chroma_offset;
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
    HobotSurface *surf = &drv->surfaces[surface];
    if (surf->va_lock_count == 0 || surf->cpu_access_count == 0 ||
        surf->lock_count == 0) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    surf->raw_data_valid = 0;
    VAStatus cache_status = VA_STATUS_SUCCESS;
    if (surf->cpu_access_count == 1) {
        cache_status = hobot_sync_surface_cache_locked(surf, 0);
        surf->cpu_cache_flush_pending = cache_status != VA_STATUS_SUCCESS;
    }
    surf->va_lock_count--;
    surf->cpu_access_count--;
    surf->lock_count--;
    pthread_mutex_unlock(&drv->mutex);
    return cache_status;
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
        if (hobot_surface_has_active_encoder(drv, surface)) {
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
    if (s->cpu_cache_flush_pending) {
        VAStatus flush_status = hobot_sync_surface_cache_locked(s, 0);
        if (flush_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return flush_status;
        }
        s->cpu_cache_flush_pending = 0;
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
        return img->surface_id != 0 ? VA_STATUS_ERROR_SURFACE_BUSY :
                                      VA_STATUS_ERROR_OPERATION_FAILED;
    }
    if (drv->buffers[img->buf_id].handle_acquired) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_SURFACE_BUSY;
    }
    void *src_data = drv->buffers[img->buf_id].data;

    if (!src_data) {
        pthread_mutex_unlock(&drv->mutex);
        return VA_STATUS_ERROR_INVALID_BUFFER;
    }
    if (img->surface_id != 0) {
        VAStatus cache_status = hobot_prepare_derived_image_cpu_access_locked(
            drv, img->buf_id);
        if (cache_status != VA_STATUS_SUCCESS) {
            pthread_mutex_unlock(&drv->mutex);
            return cache_status;
        }
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

        if (s->vpu_out_buf.vframe_buf.phy_ptr[0] != 0 &&
            s->vpu_out_buf.vframe_buf.size > 0) {
            int qret = hb_mm_mc_queue_output_buffer(&owner->vpu_ctx,
                                                    &s->vpu_out_buf, 50);
            if (qret != 0)
                qret = hb_mm_mc_queue_output_buffer(&owner->vpu_ctx,
                                                    &s->vpu_out_buf, 200);
            if (qret != 0) {
                owner->decode_failed = 1;
                fprintf(stderr, "[HOBOT-VA] vaPutImage: decoded output recycle failed for surface=%u: %d\n",
                        surface, qret);
                pthread_mutex_unlock(&drv->mutex);
                return VA_STATUS_ERROR_OPERATION_FAILED;
            }
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

    HobotDriverData *drv = calloc(1, sizeof(HobotDriverData));
    if (!drv)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    for (int i = 0; i < MAX_CONTEXTS; i++) {
        atomic_init(&drv->contexts[i].enc_external_input_pending, 0);
        drv->contexts[i].enc_external_surface = VA_INVALID_SURFACE;
        for (size_t fd_index = 0; fd_index < MAX_GRAPHIC_BUF_COMP; fd_index++)
            drv->contexts[i].enc_scratch_buf.fd[fd_index] = -1;
    }
    if (pthread_mutex_init(&drv->mutex, NULL) != 0) {
        free(drv);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    if (hobot_init_sync_condition(&drv->sync_cond) != 0) {
        pthread_mutex_destroy(&drv->mutex);
        free(drv);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    drv->sync_cond_initialized = 1;

    int32_t mem_ret = hb_mem_module_open();
    if (mem_ret != 0) {
        fprintf(stderr, "[HOBOT-VA] driver initialization: hb_mem_module_open failed: %d\n",
                mem_ret);
        pthread_cond_destroy(&drv->sync_cond);
        pthread_mutex_destroy(&drv->mutex);
        free(drv);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

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
    vtable->vaQuerySurfaceError = hobot_vaQuerySurfaceError;
    vtable->vaGetSurfaceAttributes = hobot_vaGetSurfaceAttributes;
    vtable->vaAcquireBufferHandle = hobot_vaAcquireBufferHandle;
    vtable->vaReleaseBufferHandle = hobot_vaReleaseBufferHandle;
    vtable->vaCreateMFContext = hobot_vaCreateMFContext;
    vtable->vaMFAddContext = hobot_vaMFAddContext;
    vtable->vaMFReleaseContext = hobot_vaMFReleaseContext;
    vtable->vaMFSubmit = hobot_vaMFSubmit;
    vtable->vaQueryProcessingRate = hobot_vaQueryProcessingRate;
    vtable->vaCopy = hobot_vaCopy;
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
    vtable->vaSyncSurface2 = hobot_vaSyncSurface2;
    vtable->vaSyncBuffer = hobot_vaSyncBuffer;
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
