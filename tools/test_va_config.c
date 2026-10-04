#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

/* These backend entry points are exported by libva but omitted from va.h. */
VAStatus vaLockSurface(
    VADisplay dpy,
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
VAStatus vaUnlockSurface(VADisplay dpy, VASurfaceID surface);

int main(void) {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/dri/card0");
        return 1;
    }

    VADisplay dpy = vaGetDisplayDRM(fd);
    if (!dpy) {
        fprintf(stderr, "vaGetDisplayDRM failed\n");
        close(fd);
        return 1;
    }

    int major = 0;
    int minor = 0;
    VAStatus st = vaInitialize(dpy, &major, &minor);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaInitialize failed: %s\n", vaErrorStr(st));
        close(fd);
        return 1;
    }

    int max_profiles = vaMaxNumProfiles(dpy);
    VAProfile *profiles = max_profiles > 0 ?
        calloc((size_t)max_profiles, sizeof(*profiles)) : NULL;
    int profile_count = 0;
    int has_h264_high = 0;
    st = profiles ? vaQueryConfigProfiles(dpy, profiles, &profile_count) :
                    VA_STATUS_ERROR_ALLOCATION_FAILED;
    int profile_count_valid = profile_count >= 0 &&
                              profile_count <= max_profiles;
    for (int i = 0; st == VA_STATUS_SUCCESS && profile_count_valid &&
                    i < profile_count; i++)
        has_h264_high |= profiles[i] == VAProfileH264High;
    free(profiles);
    if (st != VA_STATUS_SUCCESS || !profile_count_valid || !has_h264_high) {
        fprintf(stderr, "vaQueryConfigProfiles failed or omitted H.264 High: %s\n",
                vaErrorStr(st));
        vaTerminate(dpy);
        close(fd);
        return 1;
    }

    int max_entrypoints = vaMaxNumEntrypoints(dpy);
    VAEntrypoint *entrypoints = max_entrypoints > 0 ?
        calloc((size_t)max_entrypoints, sizeof(*entrypoints)) : NULL;
    int entrypoint_count = 0;
    int has_h264_vld = 0;
    st = entrypoints ? vaQueryConfigEntrypoints(dpy, VAProfileH264High,
                                                 entrypoints,
                                                 &entrypoint_count) :
                       VA_STATUS_ERROR_ALLOCATION_FAILED;
    int entrypoint_count_valid = entrypoint_count >= 0 &&
                                 entrypoint_count <= max_entrypoints;
    for (int i = 0; st == VA_STATUS_SUCCESS && entrypoint_count_valid &&
                    i < entrypoint_count; i++)
        has_h264_vld |= entrypoints[i] == VAEntrypointVLD;
    free(entrypoints);
    if (st != VA_STATUS_SUCCESS || !entrypoint_count_valid || !has_h264_vld) {
        fprintf(stderr, "vaQueryConfigEntrypoints failed or omitted H.264 VLD: %s\n",
                vaErrorStr(st));
        vaTerminate(dpy);
        close(fd);
        return 1;
    }

    VAConfigAttrib capabilities[] = {
        { .type = VAConfigAttribRTFormat },
        { .type = VAConfigAttribMaxPictureWidth },
        { .type = VAConfigAttribMaxPictureHeight }
    };
    st = vaGetConfigAttributes(dpy, VAProfileH264High, VAEntrypointVLD,
                               capabilities,
                               sizeof(capabilities) / sizeof(capabilities[0]));
    if (st != VA_STATUS_SUCCESS ||
        capabilities[0].value != VA_RT_FORMAT_YUV420 ||
        capabilities[1].value != 4096 || capabilities[2].value != 4096) {
        fprintf(stderr, "vaGetConfigAttributes returned unexpected H.264 VLD capabilities: status=%s rt=%u max=%ux%u\n",
                vaErrorStr(st), capabilities[0].value,
                capabilities[1].value, capabilities[2].value);
        vaTerminate(dpy);
        close(fd);
        return 1;
    }

    VAConfigAttrib profile_check_attr = {
        .type = VAConfigAttribRTFormat,
        .value = VA_RT_FORMAT_YUV420
    };
    VAConfigID rejected_config = VA_INVALID_ID;
    st = vaCreateConfig(dpy, VAProfileHEVCMain10, VAEntrypointVLD,
                        &profile_check_attr, 1, &rejected_config);
    if (st != VA_STATUS_ERROR_UNSUPPORTED_PROFILE ||
        rejected_config != VA_INVALID_ID) {
        fprintf(stderr, "unsupported profile returned unexpected result: %s id=%u\n",
                vaErrorStr(st), rejected_config);
        vaTerminate(dpy);
        close(fd);
        return 1;
    }
    st = vaCreateConfig(dpy, VAProfileH264High, VAEntrypointEncPicture,
                        &profile_check_attr, 1, &rejected_config);
    if (st != VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT ||
        rejected_config != VA_INVALID_ID) {
        fprintf(stderr, "unsupported entrypoint returned unexpected result: %s id=%u\n",
                vaErrorStr(st), rejected_config);
        vaTerminate(dpy);
        close(fd);
        return 1;
    }

    VAConfigAttrib attr = {
        .type = VAConfigAttribRTFormat,
        .value = VA_RT_FORMAT_YUV420
    };
    VAConfigID config = VA_INVALID_ID;
    st = vaCreateConfig(dpy, VAProfileH264High, VAEntrypointVLD, &attr, 1, &config);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaCreateConfig failed: %s\n", vaErrorStr(st));
        vaTerminate(dpy);
        close(fd);
        return 1;
    }

    VAProfile profile = VAProfileNone;
    VAEntrypoint entrypoint = VAEntrypointVLD;
    int max_config_attributes = vaMaxNumConfigAttributes(dpy);
    VAConfigAttrib *queried_attrs = max_config_attributes > 0 ?
        calloc((size_t)max_config_attributes, sizeof(*queried_attrs)) : NULL;
    int num_attribs = 0;
    st = queried_attrs ? vaQueryConfigAttributes(dpy, config, &profile,
                                                 &entrypoint, queried_attrs,
                                                 &num_attribs) :
                         VA_STATUS_ERROR_ALLOCATION_FAILED;
    int config_query_valid = st == VA_STATUS_SUCCESS &&
        profile == VAProfileH264High && entrypoint == VAEntrypointVLD &&
        num_attribs == 1 && queried_attrs[0].type == VAConfigAttribRTFormat &&
        queried_attrs[0].value == VA_RT_FORMAT_YUV420;
    printf("vaQueryConfigAttributes: %s, profile=%d, entrypoint=%d, attrs=%d\n",
           vaErrorStr(st), profile, entrypoint, num_attribs);
    free(queried_attrs);
    if (!config_query_valid) {
        fprintf(stderr, "vaQueryConfigAttributes returned inconsistent config data\n");
        vaDestroyConfig(dpy, config);
        vaTerminate(dpy);
        close(fd);
        return 1;
    }

    unsigned int num_surface_attrs = 0;
    st = vaQuerySurfaceAttributes(dpy, config, NULL, &num_surface_attrs);
    VASurfaceAttrib *surface_attrs = st == VA_STATUS_SUCCESS && num_surface_attrs > 0 ?
        calloc(num_surface_attrs, sizeof(*surface_attrs)) : NULL;
    if (st != VA_STATUS_SUCCESS || !surface_attrs) {
        fprintf(stderr, "vaQuerySurfaceAttributes sizing query failed: %s count=%u\n",
                vaErrorStr(st), num_surface_attrs);
        free(surface_attrs);
        vaDestroyConfig(dpy, config);
        vaTerminate(dpy);
        close(fd);
        return 1;
    }
    st = vaQuerySurfaceAttributes(dpy, config, surface_attrs,
                                  &num_surface_attrs);
    unsigned int max_surface_width = 0;
    unsigned int max_surface_height = 0;
    for (unsigned int i = 0; st == VA_STATUS_SUCCESS && i < num_surface_attrs; i++) {
        if (surface_attrs[i].type == VASurfaceAttribMaxWidth)
            max_surface_width = (unsigned int)surface_attrs[i].value.value.i;
        else if (surface_attrs[i].type == VASurfaceAttribMaxHeight)
            max_surface_height = (unsigned int)surface_attrs[i].value.value.i;
    }
    free(surface_attrs);
    if (st != VA_STATUS_SUCCESS || max_surface_width != 4096 ||
        max_surface_height != 4096) {
        fprintf(stderr, "vaQuerySurfaceAttributes returned unexpected limits: status=%s max=%ux%u\n",
                vaErrorStr(st), max_surface_width, max_surface_height);
        vaDestroyConfig(dpy, config);
        vaTerminate(dpy);
        close(fd);
        return 1;
    }
    printf("vaQuerySurfaceAttributes: max=%ux%u, attrs=%u\n",
           max_surface_width, max_surface_height, num_surface_attrs);

    VASurfaceID surface = VA_INVALID_SURFACE;
    unsigned int fourcc = 0;
    unsigned int luma_stride = 0;
    unsigned int chroma_stride = 0;
    unsigned int luma_offset = 0;
    unsigned int chroma_offset = 0;
    void *mapped = NULL;

    st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 1920, 1080, &surface, 1, NULL, 0);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaCreateSurfaces failed: %s\n", vaErrorStr(st));
        vaDestroyConfig(dpy, config);
        vaTerminate(dpy);
        close(fd);
        return 1;
    }

    st = vaLockSurface(dpy, surface, &fourcc, &luma_stride, &chroma_stride, NULL,
                       &luma_offset, &chroma_offset, NULL, NULL, &mapped);
    if (st != VA_STATUS_SUCCESS || fourcc != VA_FOURCC_NV12 || !mapped ||
        luma_stride == 0 || chroma_stride != luma_stride ||
        chroma_offset <= luma_offset) {
        fprintf(stderr, "vaLockSurface failed: status=%d fourcc=0x%x stride=%u/%u offsets=%u/%u buffer=%p\n",
                st, fourcc, luma_stride, chroma_stride,
                luma_offset, chroma_offset, mapped);
        vaDestroySurfaces(dpy, &surface, 1);
        vaDestroyConfig(dpy, config);
        vaTerminate(dpy);
        close(fd);
        return 1;
    }
    printf("vaLockSurface: success, fourcc=NV12, stride=%u, chroma_offset=%u, buffer=%p\n",
           luma_stride, chroma_offset, mapped);

    VADRMPRIMESurfaceDescriptor prime = {0};
    st = vaExportSurfaceHandle(dpy, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                               VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                               &prime);
    int export_valid = st == VA_STATUS_SUCCESS && prime.num_objects > 0 &&
                       prime.num_objects <= sizeof(prime.objects) / sizeof(prime.objects[0]) &&
                       prime.num_layers == 2 &&
                       prime.layers[0].num_planes == 1 && prime.layers[1].num_planes == 1 &&
                       prime.layers[0].offset[0] == luma_offset &&
                       prime.layers[1].offset[0] == chroma_offset &&
                       prime.layers[0].pitch[0] == luma_stride &&
                       prime.layers[1].pitch[0] == chroma_stride;
    if (export_valid) {
        uint32_t y_object = prime.layers[0].object_index[0];
        uint32_t uv_object = prime.layers[1].object_index[0];
        export_valid = y_object < prime.num_objects && uv_object < prime.num_objects;
        if (export_valid) {
            export_valid = prime.objects[y_object].size >=
                               prime.layers[0].offset[0] +
                               (uint64_t)prime.layers[0].pitch[0] * 1080 &&
                           prime.objects[uv_object].size >=
                               prime.layers[1].offset[0] +
                               (uint64_t)prime.layers[1].pitch[0] * (1080 / 2);
        }
    }
    if (!export_valid) {
        fprintf(stderr, "vaExportSurfaceHandle failed: status=%d objects=%u layers=%u Y=%u/%u UV=%u/%u pitches=%u/%u\n",
                st, prime.num_objects, prime.num_layers,
                prime.layers[0].object_index[0], prime.layers[0].offset[0],
                prime.layers[1].object_index[0], prime.layers[1].offset[0],
                prime.layers[0].pitch[0], prime.layers[1].pitch[0]);
        for (uint32_t i = 0; i < prime.num_objects &&
                             i < sizeof(prime.objects) / sizeof(prime.objects[0]); i++) {
            if (prime.objects[i].fd >= 0)
                close(prime.objects[i].fd);
        }
        vaDestroySurfaces(dpy, &surface, 1);
        vaDestroyConfig(dpy, config);
        vaTerminate(dpy);
        close(fd);
        return 1;
    }
    printf("vaExportSurfaceHandle: NV12 separate layers, objects=%u, stride=%u, UV offset=%u, size=%u\n",
           prime.num_objects, prime.layers[0].pitch[0], prime.layers[1].offset[0],
           prime.objects[prime.layers[1].object_index[0]].size);
    for (uint32_t i = 0; i < prime.num_objects; i++)
        close(prime.objects[i].fd);

    VADRMPRIMESurfaceDescriptor composed = {0};
    st = vaExportSurfaceHandle(dpy, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                               VA_EXPORT_SURFACE_READ_ONLY, &composed);
    int composed_valid = st == VA_STATUS_SUCCESS && composed.num_objects > 0 &&
                         composed.num_objects <= sizeof(composed.objects) / sizeof(composed.objects[0]) &&
                         composed.num_layers == 1 && composed.layers[0].drm_format == VA_FOURCC_NV12 &&
                         composed.layers[0].num_planes == 2 &&
                         composed.layers[0].pitch[0] == luma_stride &&
                         composed.layers[0].pitch[1] == chroma_stride;
    if (composed_valid) {
        uint32_t y_object = composed.layers[0].object_index[0];
        uint32_t uv_object = composed.layers[0].object_index[1];
        composed_valid = y_object < composed.num_objects && uv_object < composed.num_objects &&
                         composed.layers[0].offset[0] == luma_offset &&
                         composed.layers[0].offset[1] == chroma_offset;
        if (composed_valid) {
            composed_valid = composed.objects[y_object].size >=
                                 composed.layers[0].offset[0] +
                                 (uint64_t)composed.layers[0].pitch[0] * 1080 &&
                             composed.objects[uv_object].size >=
                                 composed.layers[0].offset[1] +
                                 (uint64_t)composed.layers[0].pitch[1] * (1080 / 2);
        }
    }
    if (!composed_valid) {
        fprintf(stderr, "vaExportSurfaceHandle NV12 layer failed: status=%d objects=%u layers=%u planes=%u offsets=%u/%u\n",
                st, composed.num_objects, composed.num_layers,
                composed.layers[0].num_planes, composed.layers[0].offset[0],
                composed.layers[0].offset[1]);
        for (uint32_t i = 0; i < composed.num_objects &&
                             i < sizeof(composed.objects) / sizeof(composed.objects[0]); i++) {
            if (composed.objects[i].fd >= 0)
                close(composed.objects[i].fd);
        }
        vaDestroySurfaces(dpy, &surface, 1);
        vaDestroyConfig(dpy, config);
        vaTerminate(dpy);
        close(fd);
        return 1;
    }
    printf("vaExportSurfaceHandle: NV12 two-plane layer, objects=%u, UV offset=%u\n",
           composed.num_objects, composed.layers[0].offset[1]);
    for (uint32_t i = 0; i < composed.num_objects; i++)
        close(composed.objects[i].fd);

    vaUnlockSurface(dpy, surface);
    vaDestroySurfaces(dpy, &surface, 1);

    VAStatus destroy_st = vaDestroyConfig(dpy, config);
    VAStatus terminate_st = vaTerminate(dpy);
    close(fd);
    return (st == VA_STATUS_SUCCESS && destroy_st == VA_STATUS_SUCCESS &&
            terminate_st == VA_STATUS_SUCCESS) ? 0 : 1;
}
