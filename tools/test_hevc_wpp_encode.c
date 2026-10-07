#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_hevc.h>

#define WIDTH 640u
#define HEIGHT 360u
#define FRAME_COUNT 2u

VAStatus vaLockSurface(VADisplay display, VASurfaceID surface,
                       unsigned int *fourcc, unsigned int *luma_stride,
                       unsigned int *chroma_u_stride,
                       unsigned int *chroma_v_stride,
                       unsigned int *luma_offset,
                       unsigned int *chroma_u_offset,
                       unsigned int *chroma_v_offset,
                       unsigned int *buffer_name, void **buffer);
VAStatus vaUnlockSurface(VADisplay display, VASurfaceID surface);

static void fill_surface(void *mapping, unsigned int y_stride,
                         unsigned int uv_stride, unsigned int y_offset,
                         unsigned int uv_offset, unsigned int frame_index)
{
    uint8_t *base = mapping;
    for (unsigned int y = 0; y < HEIGHT; y++) {
        uint8_t *row = base + y_offset + (size_t)y * y_stride;
        for (unsigned int x = 0; x < WIDTH; x++)
            row[x] = (uint8_t)(16u + ((x * 3u + y * 5u + frame_index * 37u) % 220u));
    }
    for (unsigned int y = 0; y < HEIGHT / 2u; y++) {
        uint8_t *row = base + uv_offset + (size_t)y * uv_stride;
        for (unsigned int x = 0; x < WIDTH; x += 2u) {
            row[x] = (uint8_t)(48u + ((x / 8u + y / 4u + frame_index * 19u) % 160u));
            row[x + 1u] = (uint8_t)(64u + ((x / 4u + y * 3u + frame_index * 23u) % 160u));
        }
    }
}

static void report_status(const char *operation, VAStatus status)
{
    fprintf(stderr, "%s failed: %s (%d)\n", operation,
            vaErrorStr(status), status);
}

static int parse_qp(const char *text, unsigned int *qp)
{
    if (!text || !qp)
        return 0;
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno || end == text || *end != '\0' || value < 0 || value > 51)
        return 0;
    *qp = (unsigned int)value;
    return 1;
}

static int parse_qp_delta(const char *text, int8_t *delta)
{
    if (!text || !delta)
        return 0;
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno || end == text || *end != '\0' || value < -51 || value > 51)
        return 0;
    *delta = (int8_t)value;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 8) {
        fprintf(stderr,
                "usage: %s OUTPUT.hevc [WPP:0|1] [CBR|VBR|CQP [QP_I [QP_P [DELTA_I [DELTA_P]]]]]\n",
                argv[0]);
        return 2;
    }
    unsigned int wpp = argc == 2 || argv[2][0] == '1' ? 1u : 0u;
    if (argc >= 3 && (argv[2][1] != '\0' ||
                      (argv[2][0] != '0' && argv[2][0] != '1'))) {
        fprintf(stderr, "WPP must be 0 or 1\n");
        return 2;
    }
    int cqp = argc >= 4 && strcmp(argv[3], "CQP") == 0;
    int vbr = argc >= 4 && strcmp(argv[3], "VBR") == 0;
    if (argc >= 4 && !cqp && !vbr && strcmp(argv[3], "CBR") != 0) {
        fprintf(stderr, "rate control must be CBR, VBR or CQP\n");
        return 2;
    }
    unsigned int qp_i = 26;
    unsigned int qp_p = 26;
    int8_t delta_i = 0;
    int8_t delta_p = 0;
    if (argc >= 5 && (!cqp || !parse_qp(argv[4], &qp_i))) {
        fprintf(stderr, "CQP requires QP_I in [0, 51]\n");
        return 2;
    }
    if (argc >= 6 && (!cqp || !parse_qp(argv[5], &qp_p))) {
        fprintf(stderr, "CQP requires QP_P in [0, 51]\n");
        return 2;
    }
    if (argc == 5)
        qp_p = qp_i;
    if (argc >= 7 && (!cqp || !parse_qp_delta(argv[6], &delta_i))) {
        fprintf(stderr, "CQP requires DELTA_I in [-51, 51]\n");
        return 2;
    }
    if (argc >= 8 && (!cqp || !parse_qp_delta(argv[7], &delta_p))) {
        fprintf(stderr, "CQP requires DELTA_P in [-51, 51]\n");
        return 2;
    }
    if (argc == 7)
        delta_p = delta_i;

    const char *device = getenv("HOBOT_DRM_DEVICE");
    if (!device || !*device)
        device = "/dev/dri/renderD128";
    int drm_fd = open(device, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror("open DRM device");
        return 1;
    }

    int result = 1;
    int initialized = 0;
    VASurfaceID locked_surface = VA_INVALID_SURFACE;
    int output_created = 0;
    VADisplay display = vaGetDisplayDRM(drm_fd);
    VAConfigID config = VA_INVALID_ID;
    VAContextID context = VA_INVALID_ID;
    VASurfaceID surfaces[FRAME_COUNT] = {
        VA_INVALID_SURFACE, VA_INVALID_SURFACE
    };
    VABufferID coded[FRAME_COUNT] = {VA_INVALID_ID, VA_INVALID_ID};
    VABufferID picture_ids[FRAME_COUNT] = {VA_INVALID_ID, VA_INVALID_ID};
    VABufferID slice_ids[FRAME_COUNT] = {VA_INVALID_ID, VA_INVALID_ID};
    VABufferID sequence_id = VA_INVALID_ID;
    VABufferID rate_control_ids[FRAME_COUNT] = {
        VA_INVALID_ID, VA_INVALID_ID
    };
    FILE *output = NULL;

    if (!display) {
        fprintf(stderr, "vaGetDisplayDRM failed\n");
        goto cleanup;
    }
    int major = 0;
    int minor = 0;
    VAStatus status = vaInitialize(display, &major, &minor);
    if (status != VA_STATUS_SUCCESS) {
        report_status("vaInitialize", status);
        goto cleanup;
    }
    initialized = 1;

    VAConfigAttrib attributes[] = {
        { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl,
          .value = cqp ? VA_RC_CQP : vbr ? VA_RC_VBR : VA_RC_CBR }
    };
    status = vaCreateConfig(display, VAProfileHEVCMain, VAEntrypointEncSlice,
                            attributes, 2, &config);
    if (status != VA_STATUS_SUCCESS) {
        report_status("HEVC config creation", status);
        goto cleanup;
    }
    status = vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, WIDTH, HEIGHT,
                              surfaces, FRAME_COUNT, NULL, 0);
    if (status != VA_STATUS_SUCCESS) {
        report_status("surface creation", status);
        goto cleanup;
    }
    status = vaCreateContext(display, config, WIDTH, HEIGHT, VA_PROGRESSIVE,
                             surfaces, FRAME_COUNT, &context);
    if (status != VA_STATUS_SUCCESS) {
        report_status("HEVC context creation", status);
        goto cleanup;
    }
    for (unsigned int frame = 0; frame < FRAME_COUNT; frame++) {
        status = vaCreateBuffer(display, context, VAEncCodedBufferType,
                                4u * 1024u * 1024u, 1, NULL, &coded[frame]);
        if (status != VA_STATUS_SUCCESS) {
            report_status("coded buffer creation", status);
            goto cleanup;
        }
    }

    for (unsigned int frame = 0; frame < FRAME_COUNT; frame++) {
        unsigned int fourcc = 0;
        unsigned int y_stride = 0;
        unsigned int uv_stride = 0;
        unsigned int y_offset = 0;
        unsigned int uv_offset = 0;
        void *mapping = NULL;
        status = vaLockSurface(display, surfaces[frame], &fourcc, &y_stride,
                               &uv_stride, NULL, &y_offset, &uv_offset,
                               NULL, NULL, &mapping);
        if (status != VA_STATUS_SUCCESS || fourcc != VA_FOURCC_NV12 ||
            !mapping || y_stride < WIDTH || uv_stride < WIDTH ||
            uv_offset <= y_offset) {
            fprintf(stderr,
                    "surface %u map invalid: status=%s fourcc=%08x stride=%u/%u offset=%u/%u\n",
                    frame, vaErrorStr(status), fourcc, y_stride, uv_stride,
                    y_offset, uv_offset);
            if (status == VA_STATUS_SUCCESS)
                vaUnlockSurface(display, surfaces[frame]);
            goto cleanup;
        }
        locked_surface = surfaces[frame];
        fill_surface(mapping, y_stride, uv_stride, y_offset, uv_offset, frame);
        status = vaUnlockSurface(display, surfaces[frame]);
        locked_surface = VA_INVALID_SURFACE;
        if (status != VA_STATUS_SUCCESS) {
            report_status("surface unlock", status);
            goto cleanup;
        }
    }

    VAEncSequenceParameterBufferHEVC sequence = {0};
    sequence.general_profile_idc = 1;
    sequence.general_level_idc = 123;
    sequence.intra_period = 30;
    sequence.intra_idr_period = 30;
    sequence.ip_period = 1;
    sequence.bits_per_second = cqp ? 0 : 2000000;
    sequence.pic_width_in_luma_samples = WIDTH;
    sequence.pic_height_in_luma_samples = HEIGHT;
    sequence.seq_fields.bits.chroma_format_idc = 1;
    sequence.log2_min_luma_coding_block_size_minus3 = 0;
    sequence.log2_diff_max_min_luma_coding_block_size = 3;
    sequence.log2_min_transform_block_size_minus2 = 0;
    sequence.log2_diff_max_min_transform_block_size = 3;
    status = vaCreateBuffer(display, context, VAEncSequenceParameterBufferType,
                            sizeof(sequence), 1, &sequence, &sequence_id);
    if (status != VA_STATUS_SUCCESS) {
        report_status("sequence buffer creation", status);
        goto cleanup;
    }

    if (vbr) {
        for (unsigned int frame = 0; frame < FRAME_COUNT; frame++) {
            uint8_t rate_control_storage[
                offsetof(VAEncMiscParameterBuffer, data) +
                sizeof(VAEncMiscParameterRateControl)] = {0};
            VAEncMiscParameterBuffer *misc =
                (VAEncMiscParameterBuffer *)rate_control_storage;
            VAEncMiscParameterRateControl rate = {
                .bits_per_second = frame == 0 ? 4000000u : 6000000u,
                .target_percentage = 50,
                .window_size = frame == 0 ? 1000u : 1500u,
            };
            misc->type = VAEncMiscParameterTypeRateControl;
            memcpy(misc->data, &rate, sizeof(rate));
            status = vaCreateBuffer(display, context,
                                    VAEncMiscParameterBufferType,
                                    sizeof(rate_control_storage), 1,
                                    rate_control_storage,
                                    &rate_control_ids[frame]);
            if (status != VA_STATUS_SUCCESS) {
                report_status("VBR rate-control buffer creation", status);
                goto cleanup;
            }
        }
    }

    for (unsigned int frame = 0; frame < FRAME_COUNT; frame++) {
        VAEncPictureParameterBufferHEVC picture = {0};
        picture.decoded_curr_pic.picture_id = surfaces[frame];
        picture.decoded_curr_pic.pic_order_cnt = (int32_t)frame;
        for (size_t i = 0; i < 15; i++) {
            picture.reference_frames[i].picture_id = VA_INVALID_SURFACE;
            picture.reference_frames[i].flags = VA_PICTURE_HEVC_INVALID;
        }
        if (frame > 0) {
            picture.reference_frames[0].picture_id = surfaces[frame - 1u];
            picture.reference_frames[0].pic_order_cnt = (int32_t)(frame - 1u);
            picture.reference_frames[0].flags = 0;
        }
        picture.coded_buf = coded[frame];
        picture.pic_init_qp = frame == 0 ? qp_i : qp_p;
        picture.collocated_ref_pic_index = 0xff;
        picture.pic_fields.bits.coding_type = frame == 0 ? 1u : 2u;
        picture.pic_fields.bits.idr_pic_flag = frame == 0;
        picture.pic_fields.bits.reference_pic_flag = 1;
        picture.pic_fields.bits.entropy_coding_sync_enabled_flag = wpp;
        picture.last_picture = frame + 1u == FRAME_COUNT ?
                               HEVC_LAST_PICTURE_EOSTREAM : 0;
        status = vaCreateBuffer(display, context,
                                VAEncPictureParameterBufferType,
                                sizeof(picture), 1, &picture,
                                &picture_ids[frame]);
        if (status != VA_STATUS_SUCCESS) {
            report_status("picture buffer creation", status);
            goto cleanup;
        }

        VAEncSliceParameterBufferHEVC slice = {0};
        slice.num_ctu_in_slice = 60;
        slice.slice_type = frame == 0 ? 2u : 1u;
        slice.slice_pic_parameter_set_id = 0;
        slice.max_num_merge_cand = 5;
        slice.slice_qp_delta = frame == 0 ? delta_i : delta_p;
        slice.slice_fields.bits.last_slice_of_pic_flag = 1;
        status = vaCreateBuffer(display, context,
                                VAEncSliceParameterBufferType,
                                sizeof(slice), 1, &slice, &slice_ids[frame]);
        if (status != VA_STATUS_SUCCESS) {
            report_status("slice buffer creation", status);
            goto cleanup;
        }
    }

    output = fopen(argv[1], "wb");
    if (!output) {
        perror("open HEVC output");
        goto cleanup;
    }
    output_created = 1;

    size_t output_size = 0;
    for (unsigned int frame = 0; frame < FRAME_COUNT; frame++) {
        status = vaBeginPicture(display, context, surfaces[frame]);
        if (status != VA_STATUS_SUCCESS) {
            report_status("vaBeginPicture", status);
            goto cleanup;
        }
        VABufferID render_buffers[4];
        int render_count = 0;
        if (frame == 0) {
            render_buffers[render_count++] = sequence_id;
        }
        if (rate_control_ids[frame] != VA_INVALID_ID)
            render_buffers[render_count++] = rate_control_ids[frame];
        render_buffers[render_count++] = picture_ids[frame];
        render_buffers[render_count++] = slice_ids[frame];
        status = vaRenderPicture(display, context, render_buffers,
                                 render_count);
        if (status != VA_STATUS_SUCCESS) {
            report_status("vaRenderPicture", status);
            goto cleanup;
        }
        status = vaEndPicture(display, context);
        if (status != VA_STATUS_SUCCESS) {
            report_status("vaEndPicture", status);
            goto cleanup;
        }
        status = vaSyncBuffer(display, coded[frame], VA_TIMEOUT_INFINITE);
        if (status != VA_STATUS_SUCCESS) {
            report_status("coded buffer sync", status);
            goto cleanup;
        }

        VACodedBufferSegment *segment = NULL;
        status = vaMapBuffer(display, coded[frame], (void **)&segment);
        if (status != VA_STATUS_SUCCESS || !segment) {
            report_status("coded buffer map", status);
            if (status == VA_STATUS_SUCCESS)
                vaUnmapBuffer(display, coded[frame]);
            goto cleanup;
        }
        int write_failed = 0;
        size_t frame_size = 0;
        for (VACodedBufferSegment *part = segment; part; part = part->next) {
            if (!part->buf || part->size == 0 ||
                fwrite(part->buf, 1, part->size, output) != part->size) {
                fprintf(stderr, "invalid or unwritable HEVC coded segment\n");
                write_failed = 1;
                break;
            }
            frame_size += part->size;
        }
        VAStatus unmap_status = vaUnmapBuffer(display, coded[frame]);
        if (unmap_status != VA_STATUS_SUCCESS) {
            report_status("coded buffer unmap", unmap_status);
            goto cleanup;
        }
        if (write_failed || frame_size == 0)
            goto cleanup;
        output_size += frame_size;
    }
    if (fclose(output) != 0) {
        output = NULL;
        perror("close HEVC output");
        goto cleanup;
    }
    output = NULL;
    printf("encoded HEVC Main I/P: %ux%u, frames=%u, wpp=%u, rc=%s qp_i=%u+%d qp_p=%u+%d, bytes=%zu, output=%s\n",
           WIDTH, HEIGHT, FRAME_COUNT, wpp, cqp ? "CQP" : vbr ? "VBR" : "CBR",
           qp_i, delta_i, qp_p, delta_p, output_size, argv[1]);
    result = 0;

cleanup:
    if (output)
        fclose(output);
    if (locked_surface != VA_INVALID_SURFACE)
        vaUnlockSurface(display, locked_surface);
    for (unsigned int frame = 0; frame < FRAME_COUNT; frame++) {
        if (slice_ids[frame] != VA_INVALID_ID)
            vaDestroyBuffer(display, slice_ids[frame]);
        if (picture_ids[frame] != VA_INVALID_ID)
            vaDestroyBuffer(display, picture_ids[frame]);
        if (coded[frame] != VA_INVALID_ID)
            vaDestroyBuffer(display, coded[frame]);
    }
    if (sequence_id != VA_INVALID_ID)
        vaDestroyBuffer(display, sequence_id);
    for (unsigned int frame = 0; frame < FRAME_COUNT; frame++) {
        if (rate_control_ids[frame] != VA_INVALID_ID)
            vaDestroyBuffer(display, rate_control_ids[frame]);
    }
    if (context != VA_INVALID_ID) {
        VAStatus destroy_status = vaDestroyContext(display, context);
        if (destroy_status != VA_STATUS_SUCCESS) {
            report_status("vaDestroyContext", destroy_status);
            result = 1;
        }
    }
    for (unsigned int frame = 0; frame < FRAME_COUNT; frame++) {
        if (surfaces[frame] != VA_INVALID_SURFACE)
            vaDestroySurfaces(display, &surfaces[frame], 1);
    }
    if (config != VA_INVALID_ID)
        vaDestroyConfig(display, config);
    if (initialized)
        vaTerminate(display);
    close(drm_fd);
    if (result != 0 && output_created)
        unlink(argv[1]);
    return result;
}
