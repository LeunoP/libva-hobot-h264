#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_hobot.h>

static int surface_info_matches(const struct hobot_surface_info *source,
                                const struct hobot_surface_info *imported)
{
    return source->phys_addr[0] != 0 && source->phys_addr[1] > source->phys_addr[0] &&
           source->virt_addr[0] != NULL && source->virt_addr[1] != NULL &&
           imported->phys_addr[0] == source->phys_addr[0] &&
           imported->phys_addr[1] == source->phys_addr[1] &&
           imported->stride == source->stride &&
           imported->vstride == source->vstride &&
           imported->width == source->width && imported->height == source->height;
}

static uint8_t expected_y(unsigned int x, unsigned int y)
{
    return (uint8_t)(x * 13u + y * 7u + 19u);
}

static uint8_t expected_uv(unsigned int x, unsigned int y)
{
    return (uint8_t)(x * 5u + y * 11u + 73u);
}

static int check_buffer_handle(VADisplay display, VASurfaceID surface,
                               const VAImage *image)
{
    VABufferInfo info = {0};
    info.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    VAStatus status = vaAcquireBufferHandle(display, image->buf, &info);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "preallocated surface buffer-handle acquire failed: %d\n",
                status);
        return 0;
    }

    int handle = info.handle <= INT32_MAX ? (int)info.handle : -1;
    struct stat st;
    int valid = handle >= 0 && info.type == VAImageBufferType &&
                info.mem_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME &&
                info.mem_size >= image->data_size && fstat(handle, &st) == 0 &&
                vaSyncSurface(display, surface) == VA_STATUS_ERROR_SURFACE_BUSY;
    VAStatus release_status = vaReleaseBufferHandle(display, image->buf);
    if (release_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "preallocated surface buffer-handle release failed: %d\n",
                release_status);
        valid = 0;
    } else if (handle < 0 || fcntl(handle, F_GETFD) != -1 || errno != EBADF) {
        fprintf(stderr, "released buffer-handle FD remains open or invalid\n");
        valid = 0;
    }
    return valid;
}

static int check_surface_pixels(VADisplay display, VASurfaceID surface,
                                unsigned int width, unsigned int height,
                                int write_pattern)
{
    VAImage image = {0};
    image.image_id = VA_INVALID_ID;
    int derived = 0;
    int mapped = 0;
    void *data = NULL;
    int valid = 0;
    VAStatus status = vaDeriveImage(display, surface, &image);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "surface image derivation failed: surface=%u status=%d\n",
                surface, status);
        goto cleanup;
    }
    derived = 1;
    if (image.format.fourcc != VA_FOURCC_NV12 || image.num_planes != 2 ||
        image.pitches[0] < width || image.pitches[1] < width) {
        fprintf(stderr, "unexpected derived NV12 layout: fourcc=0x%x planes=%u pitches=%u/%u\n",
                image.format.fourcc, image.num_planes,
                image.pitches[0], image.pitches[1]);
        goto cleanup;
    }
    uint64_t y_end = (uint64_t)image.offsets[0] +
                     (uint64_t)image.pitches[0] * (height - 1u) + width;
    uint64_t uv_end = (uint64_t)image.offsets[1] +
                      (uint64_t)image.pitches[1] * (height / 2u - 1u) + width;
    if (y_end > image.data_size || uv_end > image.data_size) {
        fprintf(stderr, "derived NV12 planes exceed image size: y_end=%llu uv_end=%llu bytes=%u\n",
                (unsigned long long)y_end, (unsigned long long)uv_end,
                image.data_size);
        goto cleanup;
    }
    if (!check_buffer_handle(display, surface, &image))
        goto cleanup;

    status = vaMapBuffer(display, image.buf, &data);
    if (status != VA_STATUS_SUCCESS || !data) {
        fprintf(stderr, "derived NV12 map failed: status=%d data=%p\n", status, data);
        goto cleanup;
    }
    mapped = 1;
    uint8_t *base = data;
    uint8_t *y_plane = base + image.offsets[0];
    uint8_t *uv_plane = base + image.offsets[1];
    for (unsigned int y = 0; y < height; y++) {
        for (unsigned int x = 0; x < width; x++) {
            uint8_t expected = expected_y(x, y);
            if (write_pattern) {
                y_plane[(size_t)y * image.pitches[0] + x] = expected;
            } else if (y_plane[(size_t)y * image.pitches[0] + x] != expected) {
                fprintf(stderr, "imported Y mismatch at (%u,%u)\n", x, y);
                goto cleanup;
            }
        }
    }
    for (unsigned int y = 0; y < height / 2u; y++) {
        for (unsigned int x = 0; x < width; x++) {
            uint8_t expected = expected_uv(x, y);
            if (write_pattern) {
                uv_plane[(size_t)y * image.pitches[1] + x] = expected;
            } else if (uv_plane[(size_t)y * image.pitches[1] + x] != expected) {
                fprintf(stderr, "imported UV mismatch at (%u,%u)\n", x, y);
                goto cleanup;
            }
        }
    }
    valid = 1;

cleanup:
    if (mapped) {
        status = vaUnmapBuffer(display, image.buf);
        if (status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "derived NV12 unmap failed: status=%d\n", status);
            valid = 0;
        }
    }
    if (derived) {
        status = vaDestroyImage(display, image.image_id);
        if (status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "derived NV12 image destroy failed: status=%d\n", status);
            valid = 0;
        }
    }
    return valid;
}

static int check_put_image_roundtrip(VADisplay display, VASurfaceID surface)
{
    VAImageFormat format = {0};
    format.fourcc = VA_FOURCC_NV12;
    VAImage image = {0};
    image.image_id = VA_INVALID_ID;
    int created = 0;
    int mapped = 0;
    void *data = NULL;
    int valid = 0;
    VAStatus status = vaCreateImage(display, &format, 64, 64, &image);
    if (status != VA_STATUS_SUCCESS || image.format.fourcc != VA_FOURCC_NV12 ||
        image.num_planes != 2 || image.pitches[0] < 64 || image.pitches[1] < 64) {
        fprintf(stderr, "NV12 patch image creation failed: status=%d\n", status);
        goto cleanup;
    }
    created = 1;
    status = vaMapBuffer(display, image.buf, &data);
    if (status != VA_STATUS_SUCCESS || !data) {
        fprintf(stderr, "NV12 patch image map failed: status=%d\n", status);
        goto cleanup;
    }
    mapped = 1;
    uint8_t *base = data;
    uint8_t *y_plane = base + image.offsets[0];
    uint8_t *uv_plane = base + image.offsets[1];
    for (unsigned int y = 0; y < 64; y++)
        for (unsigned int x = 0; x < 64; x++)
            y_plane[(size_t)y * image.pitches[0] + x] = 211;
    for (unsigned int y = 0; y < 32; y++)
        for (unsigned int x = 0; x < 64; x++)
            uv_plane[(size_t)y * image.pitches[1] + x] = 97;
    status = vaUnmapBuffer(display, image.buf);
    mapped = 0;
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "NV12 patch image unmap failed: status=%d\n", status);
        goto cleanup;
    }
    status = vaPutImage(display, surface, image.image_id,
                        0, 0, 64, 64, 32, 32, 64, 64);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaPutImage to imported surface failed: %d\n", status);
        goto cleanup;
    }

    VAImage verify = {0};
    verify.image_id = VA_INVALID_ID;
    status = vaDeriveImage(display, surface, &verify);
    if (status != VA_STATUS_SUCCESS || verify.format.fourcc != VA_FOURCC_NV12 ||
        verify.num_planes != 2 || verify.pitches[0] < 640 ||
        verify.pitches[1] < 640 ||
        (uint64_t)verify.offsets[0] + (uint64_t)verify.pitches[0] * 360u >
            verify.data_size ||
        (uint64_t)verify.offsets[1] + (uint64_t)verify.pitches[1] * 180u >
            verify.data_size) {
        fprintf(stderr, "surface derive after vaPutImage failed: %d\n", status);
        if (status == VA_STATUS_SUCCESS)
            vaDestroyImage(display, verify.image_id);
        goto cleanup;
    }
    void *verify_data = NULL;
    status = vaMapBuffer(display, verify.buf, &verify_data);
    if (status != VA_STATUS_SUCCESS || !verify_data) {
        fprintf(stderr, "surface map after vaPutImage failed: %d\n", status);
        vaDestroyImage(display, verify.image_id);
        goto cleanup;
    }
    uint8_t *verify_base = verify_data;
    uint8_t patched_y = verify_base[verify.offsets[0] +
                                    (size_t)40 * verify.pitches[0] + 40];
    uint8_t untouched_y = verify_base[verify.offsets[0] +
                                      (size_t)10 * verify.pitches[0] + 10];
    uint8_t patched_uv = verify_base[verify.offsets[1] +
                                     (size_t)20 * verify.pitches[1] + 40];
    uint8_t untouched_uv = verify_base[verify.offsets[1] +
                                       (size_t)5 * verify.pitches[1] + 10];
    VAStatus unmap_status = vaUnmapBuffer(display, verify.buf);
    VAStatus destroy_status = vaDestroyImage(display, verify.image_id);
    if (unmap_status != VA_STATUS_SUCCESS || destroy_status != VA_STATUS_SUCCESS ||
        patched_y != 211 || untouched_y != expected_y(10, 10) ||
        patched_uv != 97 || untouched_uv != expected_uv(10, 5)) {
        fprintf(stderr, "vaPutImage roundtrip mismatch: Y=%u/%u UV=%u/%u unmap=%d destroy=%d\n",
                patched_y, untouched_y, patched_uv, untouched_uv,
                unmap_status, destroy_status);
        goto cleanup;
    }
    valid = 1;

cleanup:
    if (mapped)
        vaUnmapBuffer(display, image.buf);
    if (created)
        vaDestroyImage(display, image.image_id);
    return valid;
}

int main(void)
{
    const char *device = getenv("HOBOT_DRM_DEVICE");
    if (!device || !*device)
        device = "/dev/dri/renderD128";
    int drm_fd = open(device, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror("open DRM device");
        return 1;
    }

    VADisplay display = vaGetDisplayDRM(drm_fd);
    int major = 0;
    int minor = 0;
    if (!display || vaInitialize(display, &major, &minor) != VA_STATUS_SUCCESS) {
        fprintf(stderr, "VA initialization failed on %s\n", device);
        close(drm_fd);
        return 1;
    }

    VASurfaceID source_surface = VA_INVALID_SURFACE;
    VASurfaceID imported_surface = VA_INVALID_SURFACE;
    int exported_fd = -1;
    int result = 1;
    VAStatus status = vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, 640, 360,
                                       &source_surface, 1, NULL, 0);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "source surface creation failed: %d\n", status);
        goto cleanup;
    }

    struct hobot_surface_info source_info = {0};
    status = vaGetHobotSurfaceInfo(display, source_surface, &source_info);
    if (status != VA_STATUS_SUCCESS || source_info.dma_fd < 0) {
        fprintf(stderr, "source surface allocation failed: status=%d fd=%d\n",
                status, source_info.dma_fd);
        goto cleanup;
    }
    void *decode_error_info = (void *)(uintptr_t)1;
    status = vaQuerySurfaceError(display, source_surface,
                                 VA_STATUS_ERROR_DECODING_ERROR,
                                 &decode_error_info);
    if (status != VA_STATUS_ERROR_UNIMPLEMENTED || decode_error_info != NULL) {
        fprintf(stderr, "unsupported decode-error query was not explicit: status=%d info=%p\n",
                status, decode_error_info);
        goto cleanup;
    }
    if (!check_surface_pixels(display, source_surface, 640, 360, 1))
        goto cleanup;

    VADRMPRIMESurfaceDescriptor descriptor = {0};
    status = vaExportSurfaceHandle(display, source_surface,
                                   VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                   VA_EXPORT_SURFACE_READ_WRITE |
                                       VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                   &descriptor);
    if (status != VA_STATUS_SUCCESS || descriptor.num_objects != 1 ||
        descriptor.num_layers != 1 || descriptor.layers[0].num_planes != 2 ||
        descriptor.objects[0].fd < 0) {
        fprintf(stderr, "source PRIME2 export failed: status=%d objects=%u layers=%u\n",
                status, descriptor.num_objects, descriptor.num_layers);
        goto cleanup;
    }
    exported_fd = descriptor.objects[0].fd;

    VASurfaceAttrib attrs[2] = {0};
    attrs[0].type = VASurfaceAttribMemoryType;
    attrs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[0].value.type = VAGenericValueTypeInteger;
    attrs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attrs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attrs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[1].value.type = VAGenericValueTypePointer;
    attrs[1].value.value.p = &descriptor;
    status = vaCreateSurfaces(display, VA_RT_FORMAT_YUV420, 640, 360,
                              &imported_surface, 1, attrs, 2);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "PRIME2 surface import failed: %d\n", status);
        goto cleanup;
    }

    close(exported_fd);
    exported_fd = -1;
    struct hobot_surface_info imported_info = {0};
    status = vaGetHobotSurfaceInfo(display, imported_surface, &imported_info);
    if (status != VA_STATUS_SUCCESS ||
        !surface_info_matches(&source_info, &imported_info)) {
        fprintf(stderr, "imported surface did not preserve shared NV12 layout: status=%d\n",
                status);
        goto cleanup;
    }
    if (!check_surface_pixels(display, imported_surface, 640, 360, 0))
        goto cleanup;

    status = vaDestroySurfaces(display, &source_surface, 1);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "source surface destruction failed: %d\n", status);
        goto cleanup;
    }
    source_surface = VA_INVALID_SURFACE;
    struct hobot_surface_info surviving_info = {0};
    status = vaGetHobotSurfaceInfo(display, imported_surface, &surviving_info);
    if (status != VA_STATUS_SUCCESS ||
        !surface_info_matches(&imported_info, &surviving_info)) {
        fprintf(stderr, "imported reference did not survive source destruction: status=%d\n",
                status);
        goto cleanup;
    }
    if (!check_surface_pixels(display, imported_surface, 640, 360, 0))
        goto cleanup;
    if (!check_put_image_roundtrip(display, imported_surface))
        goto cleanup;

    printf("PRIME2 NV12 import passed: %ux%u stride=%u vstride=%u shared_phys=0x%llx, buffer-handle lifecycle, import lifetime, and vaPutImage Y/UV roundtrip verified\n",
           imported_info.width, imported_info.height, imported_info.stride,
           imported_info.vstride,
           (unsigned long long)imported_info.phys_addr[0]);
    result = 0;

cleanup:
    if (exported_fd >= 0)
        close(exported_fd);
    if (imported_surface != VA_INVALID_SURFACE)
        vaDestroySurfaces(display, &imported_surface, 1);
    if (source_surface != VA_INVALID_SURFACE)
        vaDestroySurfaces(display, &source_surface, 1);
    vaTerminate(display);
    close(drm_fd);
    return result;
}
