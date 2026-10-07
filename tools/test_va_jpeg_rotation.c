#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_dec_jpeg.h>
#include <va/va_drm.h>

typedef struct {
    VAPictureParameterBufferJPEGBaseline picture;
    VAIQMatrixBufferJPEGBaseline qmatrix;
    VAHuffmanTableBufferJPEGBaseline huffman;
    VASliceParameterBufferJPEGBaseline slice;
    size_t entropy_offset;
    size_t entropy_size;
} JpegInput;

static int read_file(const char *path, uint8_t **data, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }
    long length = ftell(file);
    if (length < 4 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -1;
    }
    uint8_t *buffer = malloc((size_t)length);
    if (!buffer) {
        fclose(file);
        return -1;
    }
    size_t read_size = fread(buffer, 1, (size_t)length, file);
    int failed = ferror(file) || read_size != (size_t)length;
    fclose(file);
    if (failed) {
        free(buffer);
        return -1;
    }
    *data = buffer;
    *size = read_size;
    return 0;
}

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static int parse_jpeg(const uint8_t *data, size_t size, JpegInput *jpeg)
{
    if (!data || !jpeg || size < 4 || data[0] != 0xff || data[1] != 0xd8)
        return -1;
    memset(jpeg, 0, sizeof(*jpeg));
    size_t position = 2;
    int saw_frame = 0;
    int saw_scan = 0;
    uint16_t restart_interval = 0;

    while (position < size) {
        if (data[position++] != 0xff)
            return -1;
        while (position < size && data[position] == 0xff)
            position++;
        if (position >= size)
            return -1;
        uint8_t marker = data[position++];
        if (marker == 0xd9)
            return -1;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd8))
            continue;
        if (position + 2 > size)
            return -1;
        uint16_t segment_length = read_be16(data + position);
        if (segment_length < 2 || segment_length > size - position)
            return -1;
        const uint8_t *segment = data + position + 2;
        size_t payload_size = (size_t)segment_length - 2;

        if (marker == 0xdb) {
            size_t offset = 0;
            while (offset < payload_size) {
                uint8_t info = segment[offset++];
                unsigned int precision = info >> 4;
                unsigned int table = info & 0x0f;
                if (precision != 0 || table >= 4 || payload_size - offset < 64)
                    return -1;
                memcpy(jpeg->qmatrix.quantiser_table[table], segment + offset, 64);
                jpeg->qmatrix.load_quantiser_table[table] = 1;
                offset += 64;
            }
        } else if (marker == 0xc0) {
            if (saw_frame || payload_size < 6 || segment[0] != 8)
                return -1;
            uint16_t height = read_be16(segment + 1);
            uint16_t width = read_be16(segment + 3);
            uint8_t components = segment[5];
            if (components != 3 || payload_size != 6u + 3u * components)
                return -1;
            jpeg->picture.picture_width = width;
            jpeg->picture.picture_height = height;
            jpeg->picture.num_components = components;
            jpeg->picture.color_space = 0;
            for (unsigned int i = 0; i < components; i++) {
                const uint8_t *component = segment + 6 + 3 * i;
                jpeg->picture.components[i].component_id = component[0];
                jpeg->picture.components[i].h_sampling_factor = component[1] >> 4;
                jpeg->picture.components[i].v_sampling_factor = component[1] & 0x0f;
                jpeg->picture.components[i].quantiser_table_selector = component[2];
            }
            saw_frame = 1;
        } else if (marker == 0xc4) {
            size_t offset = 0;
            while (offset < payload_size) {
                if (payload_size - offset < 17)
                    return -1;
                uint8_t info = segment[offset++];
                unsigned int table_class = info >> 4;
                unsigned int table = info & 0x0f;
                if (table_class > 1 || table >= 2)
                    return -1;
                uint8_t *counts = table_class == 0 ?
                    jpeg->huffman.huffman_table[table].num_dc_codes :
                    jpeg->huffman.huffman_table[table].num_ac_codes;
                uint8_t *values = table_class == 0 ?
                    jpeg->huffman.huffman_table[table].dc_values :
                    jpeg->huffman.huffman_table[table].ac_values;
                size_t values_capacity = table_class == 0 ? 12 : 162;
                memcpy(counts, segment + offset, 16);
                offset += 16;
                size_t values_size = 0;
                for (size_t i = 0; i < 16; i++)
                    values_size += counts[i];
                if (values_size > values_capacity ||
                    values_size > payload_size - offset)
                    return -1;
                memcpy(values, segment + offset, values_size);
                offset += values_size;
                jpeg->huffman.load_huffman_table[table] = 1;
            }
        } else if (marker == 0xdd) {
            if (payload_size != 2)
                return -1;
            restart_interval = read_be16(segment);
        } else if (marker == 0xda) {
            if (!saw_frame || payload_size < 4 || segment[0] != 3 ||
                payload_size != 1u + 2u * segment[0] + 3u)
                return -1;
            jpeg->slice.num_components = segment[0];
            for (unsigned int i = 0; i < segment[0]; i++) {
                jpeg->slice.components[i].component_selector = segment[1 + 2 * i];
                jpeg->slice.components[i].dc_table_selector = segment[2 + 2 * i] >> 4;
                jpeg->slice.components[i].ac_table_selector = segment[2 + 2 * i] & 0x0f;
            }
            jpeg->slice.restart_interval = restart_interval;
            jpeg->slice.slice_data_offset = 0;
            jpeg->slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
            jpeg->entropy_offset = position + segment_length;
            saw_scan = 1;
            break;
        }
        position += segment_length;
    }

    if (!saw_frame || !saw_scan || jpeg->picture.picture_width == 0 ||
        jpeg->picture.picture_height == 0)
        return -1;
    uint8_t max_h = 0;
    uint8_t max_v = 0;
    for (size_t i = 0; i < 3; i++) {
        if (jpeg->picture.components[i].h_sampling_factor > max_h)
            max_h = jpeg->picture.components[i].h_sampling_factor;
        if (jpeg->picture.components[i].v_sampling_factor > max_v)
            max_v = jpeg->picture.components[i].v_sampling_factor;
    }
    if (max_h == 0 || max_v == 0)
        return -1;
    uint64_t mcu_columns = ((uint64_t)jpeg->picture.picture_width + max_h * 8u - 1u) /
                           (max_h * 8u);
    uint64_t mcu_rows = ((uint64_t)jpeg->picture.picture_height + max_v * 8u - 1u) /
                        (max_v * 8u);
    if (mcu_columns == 0 || mcu_rows == 0 ||
        mcu_columns > UINT32_MAX / mcu_rows)
        return -1;
    jpeg->slice.num_mcus = (uint32_t)(mcu_columns * mcu_rows);

    position = jpeg->entropy_offset;
    while (position < size) {
        if (data[position++] != 0xff)
            continue;
        size_t marker_start = position - 1;
        while (position < size && data[position] == 0xff)
            position++;
        if (position >= size)
            return -1;
        uint8_t code = data[position++];
        if (code == 0x00 || (code >= 0xd0 && code <= 0xd7))
            continue;
        if (code != 0xd9)
            return -1;
        jpeg->entropy_size = marker_start - jpeg->entropy_offset;
        break;
    }
    if (jpeg->entropy_size == 0 || jpeg->entropy_size > UINT32_MAX)
        return -1;
    jpeg->slice.slice_data_size = (uint32_t)jpeg->entropy_size;
    return 0;
}

static VAStatus create_decode_buffer(VADisplay display, VAContextID context,
                                     VABufferType type, unsigned int size,
                                     void *data, VABufferID *buffer)
{
    return vaCreateBuffer(display, context, type, size, 1, data, buffer);
}

static int write_nv12(VADisplay display, VASurfaceID surface,
                      unsigned int width, unsigned int height,
                      const char *path)
{
    VAImage image;
    VAStatus status = vaDeriveImage(display, surface, &image);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaDeriveImage failed: %d\n", status);
        return -1;
    }
    if (image.format.fourcc != VA_FOURCC_NV12 || image.width < width ||
        image.height < height || image.pitches[0] < width ||
        image.pitches[1] < width || height < 2) {
        fprintf(stderr, "unexpected derived image layout: fourcc=%08x size=%ux%u pitch=%u/%u\n",
                image.format.fourcc, image.width, image.height,
                image.pitches[0], image.pitches[1]);
        vaDestroyImage(display, image.image_id);
        return -1;
    }
    uint64_t y_end = (uint64_t)image.offsets[0] +
                     (uint64_t)(height - 1) * image.pitches[0] + width;
    uint64_t uv_end = (uint64_t)image.offsets[1] +
                      (uint64_t)(height / 2 - 1) * image.pitches[1] + width;
    if (y_end > image.data_size || uv_end > image.data_size) {
        fprintf(stderr,
                "derived NV12 planes exceed image data: y_end=%llu uv_end=%llu size=%u\n",
                (unsigned long long)y_end, (unsigned long long)uv_end,
                image.data_size);
        vaDestroyImage(display, image.image_id);
        return -1;
    }

    void *mapped = NULL;
    status = vaMapBuffer(display, image.buf, &mapped);
    if (status != VA_STATUS_SUCCESS || !mapped) {
        fprintf(stderr, "vaMapBuffer failed: %d\n", status);
        vaDestroyImage(display, image.image_id);
        return -1;
    }
    FILE *file = fopen(path, "wb");
    int failed = !file;
    const uint8_t *base = mapped;
    for (unsigned int row = 0; !failed && row < height; row++) {
        const uint8_t *line = base + image.offsets[0] +
                              (size_t)row * image.pitches[0];
        failed = fwrite(line, 1, width, file) != width;
    }
    for (unsigned int row = 0; !failed && row < height / 2; row++) {
        const uint8_t *line = base + image.offsets[1] +
                              (size_t)row * image.pitches[1];
        failed = fwrite(line, 1, width, file) != width;
    }
    if (file && fclose(file) != 0)
        failed = 1;
    if (vaUnmapBuffer(display, image.buf) != VA_STATUS_SUCCESS)
        failed = 1;
    if (vaDestroyImage(display, image.image_id) != VA_STATUS_SUCCESS)
        failed = 1;
    return failed ? -1 : 0;
}

static int decode(VADisplay display, const uint8_t *data, const JpegInput *jpeg,
                  unsigned int rotation, unsigned int repeats,
                  const char *output_path)
{
    uint32_t rotation_value;
    switch (rotation) {
    case 0: rotation_value = VA_ROTATION_NONE; break;
    case 90: rotation_value = VA_ROTATION_90; break;
    case 180: rotation_value = VA_ROTATION_180; break;
    case 270: rotation_value = VA_ROTATION_270; break;
    default:
        fprintf(stderr, "rotation must be one of 0, 90, 180, 270\n");
        return -1;
    }
    VAConfigAttrib jpeg_cap = {.type = VAConfigAttribDecJPEG};
    VAStatus status = vaGetConfigAttributes(display, VAProfileJPEGBaseline,
                                             VAEntrypointVLD, &jpeg_cap, 1);
    if (status != VA_STATUS_SUCCESS ||
        jpeg_cap.value == VA_ATTRIB_NOT_SUPPORTED ||
        !(jpeg_cap.value & (1u << rotation_value))) {
        fprintf(stderr, "JPEG rotation %u is not advertised (status=%d mask=0x%x)\n",
                rotation, status, jpeg_cap.value);
        return -1;
    }

    VAConfigAttrib config_attrib = {
        .type = VAConfigAttribRTFormat,
        .value = VA_RT_FORMAT_YUV420
    };
    VAConfigID config = VA_INVALID_ID;
    VAContextID context = VA_INVALID_ID;
    VASurfaceID surface = VA_INVALID_SURFACE;
    VABufferID buffers[5] = {VA_INVALID_ID, VA_INVALID_ID, VA_INVALID_ID,
                             VA_INVALID_ID, VA_INVALID_ID};
    int result = -1;
    int cleanup_failed = 0;
    status = vaCreateConfig(display, VAProfileJPEGBaseline, VAEntrypointVLD,
                            &config_attrib, 1, &config);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaCreateConfig failed: %d\n", status);
        goto done;
    }

    unsigned int output_width = jpeg->picture.picture_width;
    unsigned int output_height = jpeg->picture.picture_height;
    if (rotation_value == VA_ROTATION_90 || rotation_value == VA_ROTATION_270) {
        output_width = jpeg->picture.picture_height;
        output_height = jpeg->picture.picture_width;
    }
    status = vaCreateSurfaces(display, VA_RT_FORMAT_YUV420,
                              output_width, output_height, &surface, 1,
                              NULL, 0);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaCreateSurfaces failed: %d\n", status);
        goto done;
    }
    status = vaCreateContext(display, config, output_width, output_height,
                             VA_PROGRESSIVE, &surface, 1, &context);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaCreateContext failed: %d\n", status);
        goto done;
    }

    JpegInput submitted = *jpeg;
    submitted.picture.rotation = rotation_value;
    status = create_decode_buffer(display, context, VAPictureParameterBufferType,
                                  sizeof(submitted.picture), &submitted.picture,
                                  &buffers[0]);
    if (status == VA_STATUS_SUCCESS)
        status = create_decode_buffer(display, context, VAIQMatrixBufferType,
                                      sizeof(submitted.qmatrix), &submitted.qmatrix,
                                      &buffers[1]);
    if (status == VA_STATUS_SUCCESS)
        status = create_decode_buffer(display, context, VAHuffmanTableBufferType,
                                      sizeof(submitted.huffman), &submitted.huffman,
                                      &buffers[2]);
    if (status == VA_STATUS_SUCCESS)
        status = create_decode_buffer(display, context, VASliceParameterBufferType,
                                      sizeof(submitted.slice), &submitted.slice,
                                      &buffers[3]);
    if (status == VA_STATUS_SUCCESS)
        status = create_decode_buffer(display, context, VASliceDataBufferType,
                                      (unsigned int)submitted.entropy_size,
                                      (void *)(data + submitted.entropy_offset),
                                      &buffers[4]);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "JPEG VLD buffer creation failed: %d\n", status);
        goto done;
    }

    for (unsigned int cycle = 0; cycle < repeats; cycle++) {
        status = vaBeginPicture(display, context, surface);
        if (status == VA_STATUS_SUCCESS)
            status = vaRenderPicture(display, context, buffers, 5);
        if (status == VA_STATUS_SUCCESS)
            status = vaEndPicture(display, context);
        if (status == VA_STATUS_SUCCESS)
            status = vaSyncSurface(display, surface);
        if (status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "JPEG rotation %u cycle %u failed: %d\n",
                    rotation, cycle + 1, status);
            goto done;
        }
    }
    if (write_nv12(display, surface, output_width, output_height,
                   output_path) != 0) {
        fprintf(stderr, "failed to write decoded NV12 output\n");
        goto done;
    }
    printf("PASS: JPEG rotation=%u output=%ux%u cycles=%u\n",
           rotation, output_width, output_height, repeats);
    result = 0;

done:
    for (size_t i = 0; i < sizeof(buffers) / sizeof(buffers[0]); i++)
        if (buffers[i] != VA_INVALID_ID &&
            vaDestroyBuffer(display, buffers[i]) != VA_STATUS_SUCCESS) {
            fprintf(stderr, "vaDestroyBuffer failed: id=%u\n", buffers[i]);
            cleanup_failed = 1;
        }
    if (context != VA_INVALID_ID &&
        vaDestroyContext(display, context) != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaDestroyContext failed: id=%u\n", context);
        cleanup_failed = 1;
    }
    if (surface != VA_INVALID_SURFACE &&
        vaDestroySurfaces(display, &surface, 1) != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaDestroySurfaces failed: id=%u\n", surface);
        cleanup_failed = 1;
    }
    if (config != VA_INVALID_ID &&
        vaDestroyConfig(display, config) != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaDestroyConfig failed: id=%u\n", config);
        cleanup_failed = 1;
    }
    if (cleanup_failed)
        result = -1;
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s <jpeg> <rotation-degrees> <cycles> <output.nv12>\n",
                argv[0]);
        return 2;
    }
    char *end = NULL;
    errno = 0;
    unsigned long rotation = strtoul(argv[2], &end, 10);
    if (errno || !end || *end || rotation > UINT32_MAX)
        return 2;
    end = NULL;
    errno = 0;
    unsigned long cycles = strtoul(argv[3], &end, 10);
    if (errno || !end || *end || cycles == 0 || cycles > 100)
        return 2;

    uint8_t *data = NULL;
    size_t size = 0;
    JpegInput jpeg;
    if (read_file(argv[1], &data, &size) != 0 ||
        parse_jpeg(data, size, &jpeg) != 0) {
        fprintf(stderr, "could not parse a baseline three-component JPEG\n");
        free(data);
        return 2;
    }
    int drm_fd = open(getenv("HOBOT_DRM_DEVICE") ?
                      getenv("HOBOT_DRM_DEVICE") : "/dev/dri/renderD128",
                      O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror("open DRM device");
        free(data);
        return 2;
    }
    VADisplay display = vaGetDisplayDRM(drm_fd);
    int major = 0;
    int minor = 0;
    VAStatus status = vaInitialize(display, &major, &minor);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaInitialize failed: %d\n", status);
        close(drm_fd);
        free(data);
        return 1;
    }
    int result = decode(display, data, &jpeg, (unsigned int)rotation,
                        (unsigned int)cycles, argv[4]);
    VAStatus terminate_status = vaTerminate(display);
    if (terminate_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaTerminate failed: %d\n", terminate_status);
        result = 1;
    }
    close(drm_fd);
    free(data);
    return result == 0 ? 0 : 1;
}
