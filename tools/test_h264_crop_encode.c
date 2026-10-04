#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_h264.h>

#define CODED_WIDTH 640u
#define CODED_HEIGHT 368u
#define VISIBLE_WIDTH 638u
#define VISIBLE_HEIGHT 360u

VAStatus vaLockSurface(VADisplay display, VASurfaceID surface,
                       unsigned int *fourcc, unsigned int *luma_stride,
                       unsigned int *chroma_u_stride,
                       unsigned int *chroma_v_stride,
                       unsigned int *luma_offset,
                       unsigned int *chroma_u_offset,
                       unsigned int *chroma_v_offset,
                       unsigned int *buffer_name, void **buffer);
VAStatus vaUnlockSurface(VADisplay display, VASurfaceID surface);

static int write_visible_nv12(void *mapping,
                              unsigned int y_stride,
                              unsigned int uv_stride,
                              unsigned int y_offset,
                              unsigned int uv_offset,
                              const char *reference_path)
{
    uint8_t *base = mapping;
    for (unsigned int y = 0; y < VISIBLE_HEIGHT; y++) {
        uint8_t *row = base + y_offset + (size_t)y * y_stride;
        for (unsigned int x = 0; x < VISIBLE_WIDTH; x++) {
            unsigned int tile = ((x / 16u) ^ (y / 16u)) & 1u;
            row[x] = (uint8_t)(tile ? 210u + ((x + y) % 20u) :
                                      24u + ((x * 3u + y * 5u) % 20u));
        }
    }
    for (unsigned int y = 0; y < VISIBLE_HEIGHT / 2u; y++) {
        uint8_t *row = base + uv_offset + (size_t)y * uv_stride;
        for (unsigned int x = 0; x < VISIBLE_WIDTH; x += 2u) {
            unsigned int tile = ((x / 16u) + (y / 8u)) % 4u;
            row[x] = (uint8_t)(48u + tile * 35u);
            row[x + 1u] = (uint8_t)(56u + ((tile * 47u) % 160u));
        }
    }

    FILE *reference = fopen(reference_path, "wb");
    if (!reference) {
        perror("open reference frame");
        return -1;
    }
    int failed = 0;
    for (unsigned int y = 0; y < VISIBLE_HEIGHT; y++) {
        const uint8_t *row = base + y_offset + (size_t)y * y_stride;
        if (fwrite(row, 1, VISIBLE_WIDTH, reference) != VISIBLE_WIDTH) {
            failed = 1;
            break;
        }
    }
    for (unsigned int y = 0; !failed && y < VISIBLE_HEIGHT / 2u; y++) {
        const uint8_t *row = base + uv_offset + (size_t)y * uv_stride;
        if (fwrite(row, 1, VISIBLE_WIDTH, reference) != VISIBLE_WIDTH)
            failed = 1;
    }
    if (fclose(reference) != 0)
        failed = 1;
    if (failed) {
        fprintf(stderr, "failed to write reference NV12 frame\n");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s OUTPUT.h264 REFERENCE.nv12\n", argv[0]);
        return 2;
    }

    const char *device = getenv("HOBOT_DRM_DEVICE");
    if (!device || !*device)
        device = "/dev/dri/card0";
    int drm_fd = open(device, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror("open DRM device");
        return 1;
    }

    int result = 1;
    int initialized = 0;
    int surface_locked = 0;
    int output_written = 0;
    VADisplay display = vaGetDisplayDRM(drm_fd);
    VAConfigID config = VA_INVALID_ID;
    VAContextID context = VA_INVALID_ID;
    VASurfaceID surface = VA_INVALID_SURFACE;
    VABufferID coded = VA_INVALID_ID;
    VABufferID sequence_id = VA_INVALID_ID;
    VABufferID picture_id = VA_INVALID_ID;
    VABufferID slice_id = VA_INVALID_ID;
    FILE *output = NULL;

    if (!display) {
        fprintf(stderr, "vaGetDisplayDRM failed\n");
        goto cleanup;
    }
    int major = 0;
    int minor = 0;
    VAStatus status = vaInitialize(display, &major, &minor);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaInitialize failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }
    initialized = 1;

    VAConfigAttrib attributes[] = {
        { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl, .value = VA_RC_CBR }
    };
    status = vaCreateConfig(display, VAProfileH264High, VAEntrypointEncSlice,
                            attributes, 2, &config);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "H.264 config creation failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }
    status = vaCreateSurfaces(display, VA_RT_FORMAT_YUV420,
                              VISIBLE_WIDTH, VISIBLE_HEIGHT,
                              &surface, 1, NULL, 0);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "visible NV12 surface creation failed: %s\n",
                vaErrorStr(status));
        goto cleanup;
    }
    status = vaCreateContext(display, config, CODED_WIDTH, CODED_HEIGHT,
                             VA_PROGRESSIVE, &surface, 1, &context);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "coded H.264 context creation failed: %s\n",
                vaErrorStr(status));
        goto cleanup;
    }
    status = vaCreateBuffer(display, context, VAEncCodedBufferType,
                            4u * 1024u * 1024u, 1, NULL, &coded);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "coded buffer creation failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }

    unsigned int fourcc = 0;
    unsigned int y_stride = 0;
    unsigned int uv_stride = 0;
    unsigned int y_offset = 0;
    unsigned int uv_offset = 0;
    void *mapping = NULL;
    status = vaLockSurface(display, surface, &fourcc, &y_stride, &uv_stride,
                           NULL, &y_offset, &uv_offset, NULL, NULL, &mapping);
    if (status != VA_STATUS_SUCCESS || fourcc != VA_FOURCC_NV12 || !mapping ||
        y_stride < VISIBLE_WIDTH || uv_stride < VISIBLE_WIDTH ||
        uv_offset <= y_offset) {
        fprintf(stderr, "visible surface map failed: status=%s fourcc=%08x stride=%u/%u offset=%u/%u\n",
                vaErrorStr(status), fourcc, y_stride, uv_stride,
                y_offset, uv_offset);
        goto cleanup;
    }
    surface_locked = 1;
    if (write_visible_nv12(mapping, y_stride, uv_stride, y_offset,
                           uv_offset, argv[2]) != 0)
        goto cleanup;
    status = vaUnlockSurface(display, surface);
    surface_locked = 0;
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaUnlockSurface failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }

    VAEncSequenceParameterBufferH264 sequence = {0};
    sequence.level_idc = 41;
    sequence.intra_period = 1;
    sequence.intra_idr_period = 1;
    sequence.ip_period = 1;
    sequence.bits_per_second = 3000000;
    sequence.max_num_ref_frames = 1;
    sequence.picture_width_in_mbs = CODED_WIDTH / 16u;
    sequence.picture_height_in_mbs = CODED_HEIGHT / 16u;
    sequence.seq_fields.bits.chroma_format_idc = 1;
    sequence.seq_fields.bits.frame_mbs_only_flag = 1;
    sequence.seq_fields.bits.direct_8x8_inference_flag = 1;
    sequence.frame_cropping_flag = 1;
    sequence.frame_crop_left_offset = 1;
    sequence.frame_crop_top_offset = 1;
    sequence.frame_crop_bottom_offset = 3;
    sequence.vui_parameters_present_flag = 1;
    sequence.vui_fields.bits.timing_info_present_flag = 1;
    sequence.vui_fields.bits.fixed_frame_rate_flag = 1;
    sequence.num_units_in_tick = 1;
    sequence.time_scale = 120;
    status = vaCreateBuffer(display, context, VAEncSequenceParameterBufferType,
                            sizeof(sequence), 1, &sequence, &sequence_id);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "sequence buffer creation failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }

    VAEncPictureParameterBufferH264 picture = {0};
    picture.CurrPic.picture_id = surface;
    for (size_t i = 0; i < 16; i++)
        picture.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    picture.coded_buf = coded;
    picture.pic_init_qp = 26;
    picture.num_ref_idx_l0_active_minus1 = 0;
    picture.pic_fields.bits.idr_pic_flag = 1;
    picture.pic_fields.bits.reference_pic_flag = 1;
    picture.pic_fields.bits.deblocking_filter_control_present_flag = 1;
    status = vaCreateBuffer(display, context, VAEncPictureParameterBufferType,
                            sizeof(picture), 1, &picture, &picture_id);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "picture buffer creation failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }

    VAEncSliceParameterBufferH264 slice = {0};
    slice.num_macroblocks = (CODED_WIDTH / 16u) * (CODED_HEIGHT / 16u);
    slice.macroblock_info = VA_INVALID_ID;
    slice.slice_type = 2;
    slice.disable_deblocking_filter_idc = 1;
    status = vaCreateBuffer(display, context, VAEncSliceParameterBufferType,
                            sizeof(slice), 1, &slice, &slice_id);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "slice buffer creation failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }

    status = vaBeginPicture(display, context, surface);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaBeginPicture failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }
    VABufferID render_buffers[] = {sequence_id, picture_id, slice_id};
    status = vaRenderPicture(display, context, render_buffers, 3);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaRenderPicture failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }
    status = vaEndPicture(display, context);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaEndPicture failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }

    VACodedBufferSegment *segment = NULL;
    status = vaMapBuffer(display, coded, (void **)&segment);
    if (status != VA_STATUS_SUCCESS || !segment) {
        fprintf(stderr, "coded buffer map failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }
    output = fopen(argv[1], "wb");
    if (!output) {
        perror("open H.264 output");
        vaUnmapBuffer(display, coded);
        goto cleanup;
    }
    for (VACodedBufferSegment *part = segment; part; part = part->next) {
        if (!part->buf || part->size == 0 ||
            fwrite(part->buf, 1, part->size, output) != part->size) {
            fprintf(stderr, "invalid or unwritable H.264 coded segment\n");
            fclose(output);
            output = NULL;
            vaUnmapBuffer(display, coded);
            goto cleanup;
        }
    }
    if (fclose(output) != 0) {
        output = NULL;
        perror("close H.264 output");
        vaUnmapBuffer(display, coded);
        goto cleanup;
    }
    output = NULL;
    output_written = 1;
    status = vaUnmapBuffer(display, coded);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "coded buffer unmap failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }

    printf("encoded visible=%ux%u coded=%ux%u crop=left1/top1/right0/bottom3 output=%s\n",
           VISIBLE_WIDTH, VISIBLE_HEIGHT, CODED_WIDTH, CODED_HEIGHT, argv[1]);
    result = 0;

cleanup:
    if (output)
        fclose(output);
    if (surface_locked)
        vaUnlockSurface(display, surface);
    if (slice_id != VA_INVALID_ID)
        vaDestroyBuffer(display, slice_id);
    if (picture_id != VA_INVALID_ID)
        vaDestroyBuffer(display, picture_id);
    if (sequence_id != VA_INVALID_ID)
        vaDestroyBuffer(display, sequence_id);
    if (coded != VA_INVALID_ID)
        vaDestroyBuffer(display, coded);
    if (context != VA_INVALID_ID) {
        VAStatus destroy_status = vaDestroyContext(display, context);
        if (destroy_status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "vaDestroyContext failed: %s\n", vaErrorStr(destroy_status));
            result = 1;
        }
    }
    if (surface != VA_INVALID_SURFACE)
        vaDestroySurfaces(display, &surface, 1);
    if (config != VA_INVALID_ID)
        vaDestroyConfig(display, config);
    if (initialized)
        vaTerminate(display);
    close(drm_fd);
    if (result != 0 && output_written)
        unlink(argv[1]);
    return result;
}
