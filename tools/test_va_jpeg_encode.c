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
#include <va/va_enc_jpeg.h>

VAStatus vaLockSurface(
    VADisplay display,
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
);
VAStatus vaUnlockSurface(VADisplay display, VASurfaceID surface);

static int parse_uint(const char *text, unsigned int *value)
{
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(text, &end, 10);
    if (errno || !end || *end != '\0' || parsed > UINT32_MAX)
        return -1;
    *value = (unsigned int)parsed;
    return 0;
}

static void fill_nv12(void *base, unsigned int width, unsigned int height,
                      unsigned int y_stride, unsigned int uv_stride,
                      unsigned int y_offset, unsigned int uv_offset)
{
    uint8_t *pixels = base;
    for (unsigned int row = 0; row < height; row++) {
        uint8_t *y = pixels + y_offset + (size_t)row * y_stride;
        for (unsigned int col = 0; col < width; col++)
            y[col] = (uint8_t)(16u + ((col * 3u + row * 5u) % 220u));
    }
    for (unsigned int row = 0; row < height / 2u; row++) {
        uint8_t *uv = pixels + uv_offset + (size_t)row * uv_stride;
        for (unsigned int col = 0; col < width; col += 2u) {
            uv[col] = (uint8_t)(48u + ((row + col) % 160u));
            uv[col + 1u] = (uint8_t)(64u + ((row * 3u + col) % 160u));
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 5) {
        fprintf(stderr, "usage: %s QUALITY OUTPUT.jpg [WIDTH HEIGHT]\n", argv[0]);
        return 2;
    }

    unsigned int quality = 0;
    unsigned int width = 640;
    unsigned int height = 480;
    if (parse_uint(argv[1], &quality) != 0 || quality < 1 || quality > 100 ||
        (argc >= 4 && parse_uint(argv[3], &width) != 0) ||
        (argc >= 5 && parse_uint(argv[4], &height) != 0) ||
        width < 64 || width > 4096 || height < 64 || height > 4096 ||
        (width & 1u) || (height & 1u)) {
        fprintf(stderr, "quality must be 1..100; dimensions must be even and 64..4096\n");
        return 2;
    }

    const char *device_path = getenv("HOBOT_DRM_DEVICE");
    if (!device_path || !*device_path)
        device_path = "/dev/dri/card0";
    int drm_fd = open(device_path, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror("open DRM device");
        return 1;
    }

    int result = 1;
    int initialized = 0;
    int surface_created = 0;
    int locked = 0;
    VADisplay display = vaGetDisplayDRM(drm_fd);
    VAConfigID config = VA_INVALID_ID;
    VAContextID context = VA_INVALID_ID;
    VASurfaceID surface = VA_INVALID_SURFACE;
    VABufferID coded_buffer = VA_INVALID_ID;
    VABufferID picture_buffer = VA_INVALID_ID;
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
        { .type = VAConfigAttribRateControl, .value = VA_RC_CQP }
    };
    status = vaCreateConfig(display, VAProfileJPEGBaseline, VAEntrypointEncPicture,
                            attributes, sizeof(attributes) / sizeof(attributes[0]),
                            &config);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "JPEG config creation failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }

    status = vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, width, height,
                              &surface, 1, NULL, 0);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "NV12 surface creation failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }
    surface_created = 1;

    status = vaCreateContext(display, config, (int)width, (int)height,
                             VA_PROGRESSIVE, NULL, 0, &context);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "JPEG encoder context creation failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }

    status = vaCreateBuffer(display, context, VAEncCodedBufferType,
                            8u * 1024u * 1024u, 1, NULL, &coded_buffer);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "coded buffer creation failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }

    unsigned int fourcc = 0;
    unsigned int y_stride = 0;
    unsigned int uv_stride = 0;
    unsigned int y_offset = 0;
    unsigned int uv_offset = 0;
    void *mapped_surface = NULL;
    status = vaLockSurface(display, surface, &fourcc, &y_stride, &uv_stride, NULL,
                           &y_offset, &uv_offset, NULL, NULL, &mapped_surface);
    if (status != VA_STATUS_SUCCESS || fourcc != VA_FOURCC_NV12 || !mapped_surface ||
        y_stride < width || uv_stride < width || uv_offset <= y_offset) {
        fprintf(stderr, "NV12 surface mapping failed: status=%s format=0x%x stride=%u/%u offset=%u/%u ptr=%p\n",
                vaErrorStr(status), fourcc, y_stride, uv_stride,
                y_offset, uv_offset, mapped_surface);
        goto cleanup;
    }
    locked = 1;
    fill_nv12(mapped_surface, width, height, y_stride, uv_stride,
              y_offset, uv_offset);
    status = vaUnlockSurface(display, surface);
    locked = 0;
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaUnlockSurface failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }

    VAEncPictureParameterBufferJPEG picture = {0};
    picture.reconstructed_picture = VA_INVALID_SURFACE;
    picture.picture_width = (uint16_t)width;
    picture.picture_height = (uint16_t)height;
    picture.coded_buf = coded_buffer;
    picture.pic_flags.bits.huffman = 1;
    picture.pic_flags.bits.interleaved = 1;
    picture.sample_bit_depth = 8;
    picture.num_scan = 1;
    picture.num_components = 3;
    picture.component_id[0] = 1;
    picture.component_id[1] = 2;
    picture.component_id[2] = 3;
    picture.quantiser_table_selector[0] = 0;
    picture.quantiser_table_selector[1] = 1;
    picture.quantiser_table_selector[2] = 1;
    picture.quality = (uint8_t)quality;
    status = vaCreateBuffer(display, context, VAEncPictureParameterBufferType,
                            sizeof(picture), 1, &picture, &picture_buffer);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "JPEG picture buffer creation failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }

    status = vaBeginPicture(display, context, surface);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaBeginPicture failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }
    status = vaRenderPicture(display, context, &picture_buffer, 1);
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
    status = vaMapBuffer(display, coded_buffer, (void **)&segment);
    if (status != VA_STATUS_SUCCESS || !segment) {
        fprintf(stderr, "coded buffer mapping failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }
    output = fopen(argv[2], "wb");
    if (!output) {
        perror("open JPEG output");
        vaUnmapBuffer(display, coded_buffer);
        goto cleanup;
    }
    for (VACodedBufferSegment *part = segment; part; part = part->next) {
        if (!part->buf || part->size == 0 ||
            fwrite(part->buf, 1, part->size, output) != part->size) {
            fprintf(stderr, "invalid or unwritable coded segment\n");
            fclose(output);
            output = NULL;
            vaUnmapBuffer(display, coded_buffer);
            goto cleanup;
        }
    }
    if (fclose(output) != 0) {
        output = NULL;
        perror("close JPEG output");
        vaUnmapBuffer(display, coded_buffer);
        goto cleanup;
    }
    output = NULL;
    status = vaUnmapBuffer(display, coded_buffer);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "coded buffer unmap failed: %s\n", vaErrorStr(status));
        goto cleanup;
    }

    printf("encoded JPEG quality=%u size=%ux%u output=%s\n",
           quality, width, height, argv[2]);
    result = 0;

cleanup:
    if (output)
        fclose(output);
    if (locked)
        vaUnlockSurface(display, surface);
    if (picture_buffer != VA_INVALID_ID)
        vaDestroyBuffer(display, picture_buffer);
    if (coded_buffer != VA_INVALID_ID)
        vaDestroyBuffer(display, coded_buffer);
    if (context != VA_INVALID_ID)
        vaDestroyContext(display, context);
    if (surface_created)
        vaDestroySurfaces(display, &surface, 1);
    if (config != VA_INVALID_ID)
        vaDestroyConfig(display, config);
    if (initialized)
        vaTerminate(display);
    close(drm_fd);
    return result;
}
