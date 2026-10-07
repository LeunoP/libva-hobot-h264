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

static int supports_entrypoint(VADisplay dpy, VAProfile profile,
                               VAEntrypoint required)
{
    int max_entrypoints = vaMaxNumEntrypoints(dpy);
    VAEntrypoint *entrypoints = max_entrypoints > 0 ?
        calloc((size_t)max_entrypoints, sizeof(*entrypoints)) : NULL;
    if (!entrypoints)
        return 0;

    int count = 0;
    VAStatus st = vaQueryConfigEntrypoints(dpy, profile, entrypoints, &count);
    int found = st == VA_STATUS_SUCCESS && count >= 0 &&
                count <= max_entrypoints;
    int matched = 0;
    for (int i = 0; found && i < count; i++)
        matched |= entrypoints[i] == required;
    free(entrypoints);
    return found && matched;
}

static int check_advertised_matrix(VADisplay dpy)
{
    static const struct {
        VAProfile profile;
        VAEntrypoint entrypoints[2];
        const char *name;
    } expected[] = {
        {VAProfileH264ConstrainedBaseline,
         {VAEntrypointVLD, VAEntrypointEncSlice}, "H.264 Constrained Baseline"},
        {VAProfileH264Main,
         {VAEntrypointVLD, VAEntrypointEncSlice}, "H.264 Main"},
        {VAProfileH264High,
         {VAEntrypointVLD, VAEntrypointEncSlice}, "H.264 High"},
        {VAProfileHEVCMain,
         {VAEntrypointVLD, VAEntrypointEncSlice}, "HEVC Main"},
        {VAProfileJPEGBaseline,
         {VAEntrypointVLD, VAEntrypointEncPicture}, "JPEG Baseline"},
    };
    const size_t expected_count = sizeof(expected) / sizeof(expected[0]);
    int max_profiles = vaMaxNumProfiles(dpy);
    VAProfile *profiles = max_profiles > 0 ?
        calloc((size_t)max_profiles, sizeof(*profiles)) : NULL;
    if (!profiles) {
        fprintf(stderr, "could not allocate advertised-profile list\n");
        return 0;
    }

    int profile_count = 0;
    VAStatus status = vaQueryConfigProfiles(dpy, profiles, &profile_count);
    if (status != VA_STATUS_SUCCESS || profile_count < 0 ||
        profile_count > max_profiles || (size_t)profile_count != expected_count) {
        fprintf(stderr, "profile enumeration mismatch: status=%s count=%d expected=%zu\n",
                vaErrorStr(status), profile_count, expected_count);
        free(profiles);
        return 0;
    }

    int profile_seen[5] = {0};
    for (int i = 0; i < profile_count; i++) {
        size_t matched = expected_count;
        for (size_t j = 0; j < expected_count; j++) {
            if (profiles[i] == expected[j].profile) {
                matched = j;
                break;
            }
        }
        if (matched == expected_count || profile_seen[matched]++) {
            fprintf(stderr, "unexpected or duplicate VA profile: %d\n", profiles[i]);
            free(profiles);
            return 0;
        }
    }
    free(profiles);

    int max_entrypoints = vaMaxNumEntrypoints(dpy);
    VAEntrypoint *entrypoints = max_entrypoints > 0 ?
        calloc((size_t)max_entrypoints, sizeof(*entrypoints)) : NULL;
    if (!entrypoints) {
        fprintf(stderr, "could not allocate advertised-entrypoint list\n");
        return 0;
    }
    for (size_t i = 0; i < expected_count; i++) {
        int count = 0;
        status = vaQueryConfigEntrypoints(dpy, expected[i].profile,
                                          entrypoints, &count);
        if (status != VA_STATUS_SUCCESS || count != 2 || count > max_entrypoints) {
            fprintf(stderr, "%s entrypoint enumeration mismatch: status=%s count=%d expected=2\n",
                    expected[i].name, vaErrorStr(status), count);
            free(entrypoints);
            return 0;
        }

        int entrypoint_seen[2] = {0};
        for (int j = 0; j < count; j++) {
            int matched = -1;
            for (int k = 0; k < 2; k++) {
                if (entrypoints[j] == expected[i].entrypoints[k]) {
                    matched = k;
                    break;
                }
            }
            if (matched < 0 || entrypoint_seen[matched]++) {
                fprintf(stderr, "%s advertises an unexpected or duplicate entrypoint: %d\n",
                        expected[i].name, entrypoints[j]);
                free(entrypoints);
                return 0;
            }
        }
    }
    free(entrypoints);
    printf("VA capability matrix verified: %zu profiles and %zu profile-entrypoint pairs\n",
           expected_count, expected_count * 2u);
    return 1;
}

static int check_encoder_attributes(VADisplay dpy, VAProfile profile,
                                    VAEntrypoint entrypoint,
                                    VAConfigAttrib *attrs,
                                    const unsigned int *expected,
                                    size_t count, const char *label)
{
    VAStatus st = vaGetConfigAttributes(dpy, profile, entrypoint, attrs,
                                        (int)count);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "%s vaGetConfigAttributes failed: %s\n",
                label, vaErrorStr(st));
        return 0;
    }
    for (size_t i = 0; i < count; i++) {
        if (attrs[i].value != expected[i]) {
            fprintf(stderr, "%s attribute %d mismatch: got=%u expected=%u\n",
                    label, attrs[i].type, attrs[i].value, expected[i]);
            return 0;
        }
    }
    return 1;
}

static VAStatus query_surface_limits(VADisplay dpy, VAConfigID config,
                                     unsigned int *max_width,
                                     unsigned int *max_height,
                                     unsigned int *attr_count)
{
    if (!max_width || !max_height || !attr_count)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    unsigned int count = 0;
    VAStatus st = vaQuerySurfaceAttributes(dpy, config, NULL, &count);
    if (st != VA_STATUS_SUCCESS || count == 0)
        return st != VA_STATUS_SUCCESS ? st : VA_STATUS_ERROR_OPERATION_FAILED;

    VASurfaceAttrib *attrs = calloc(count, sizeof(*attrs));
    if (!attrs)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    st = vaQuerySurfaceAttributes(dpy, config, attrs, &count);
    *max_width = 0;
    *max_height = 0;
    if (st == VA_STATUS_SUCCESS) {
        for (unsigned int i = 0; i < count; i++) {
            if (attrs[i].type == VASurfaceAttribMaxWidth)
                *max_width = (unsigned int)attrs[i].value.value.i;
            else if (attrs[i].type == VASurfaceAttribMaxHeight)
                *max_height = (unsigned int)attrs[i].value.value.i;
        }
    }
    *attr_count = count;
    free(attrs);
    return st;
}

int main(void) {
    const char *device = getenv("HOBOT_DRM_DEVICE");
    if (!device || !*device)
        device = "/dev/dri/renderD128";
    int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror(device);
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
    if (!check_advertised_matrix(dpy)) {
        vaTerminate(dpy);
        close(fd);
        return 1;
    }

    int max_profiles = vaMaxNumProfiles(dpy);
    VAProfile *profiles = max_profiles > 0 ?
        calloc((size_t)max_profiles, sizeof(*profiles)) : NULL;
    int profile_count = 0;
    int has_h264_high = 0;
    int has_hevc_main = 0;
    st = profiles ? vaQueryConfigProfiles(dpy, profiles, &profile_count) :
                    VA_STATUS_ERROR_ALLOCATION_FAILED;
    int profile_count_valid = profile_count >= 0 &&
                              profile_count <= max_profiles;
    for (int i = 0; st == VA_STATUS_SUCCESS && profile_count_valid &&
                    i < profile_count; i++) {
        has_h264_high |= profiles[i] == VAProfileH264High;
        has_hevc_main |= profiles[i] == VAProfileHEVCMain;
    }
    free(profiles);
    if (st != VA_STATUS_SUCCESS || !profile_count_valid || !has_h264_high ||
        !has_hevc_main) {
        fprintf(stderr, "vaQueryConfigProfiles failed or omitted H.264 High/HEVC Main: %s\n",
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

    VAEntrypoint *hevc_entrypoints = max_entrypoints > 0 ?
        calloc((size_t)max_entrypoints, sizeof(*hevc_entrypoints)) : NULL;
    int hevc_entrypoint_count = 0;
    int has_hevc_vld = 0;
    st = hevc_entrypoints ? vaQueryConfigEntrypoints(
        dpy, VAProfileHEVCMain, hevc_entrypoints, &hevc_entrypoint_count) :
        VA_STATUS_ERROR_ALLOCATION_FAILED;
    int hevc_entrypoint_count_valid = hevc_entrypoint_count >= 0 &&
                                     hevc_entrypoint_count <= max_entrypoints;
    for (int i = 0; st == VA_STATUS_SUCCESS && hevc_entrypoint_count_valid &&
                    i < hevc_entrypoint_count; i++)
        has_hevc_vld |= hevc_entrypoints[i] == VAEntrypointVLD;
    free(hevc_entrypoints);
    if (st != VA_STATUS_SUCCESS || !hevc_entrypoint_count_valid ||
        !has_hevc_vld) {
        fprintf(stderr, "vaQueryConfigEntrypoints failed or omitted HEVC Main VLD: %s\n",
                vaErrorStr(st));
        vaTerminate(dpy);
        close(fd);
        return 1;
    }

    static const struct {
        VAProfile profile;
        VAEntrypoint entrypoint;
        const char *name;
    } encoder_entrypoints[] = {
        {VAProfileH264ConstrainedBaseline, VAEntrypointVLD, "H.264 Constrained Baseline VLD"},
        {VAProfileH264ConstrainedBaseline, VAEntrypointEncSlice, "H.264 Constrained Baseline"},
        {VAProfileH264Main, VAEntrypointVLD, "H.264 Main VLD"},
        {VAProfileH264Main, VAEntrypointEncSlice, "H.264 Main"},
        {VAProfileH264High, VAEntrypointEncSlice, "H.264 High"},
        {VAProfileHEVCMain, VAEntrypointEncSlice, "HEVC Main"},
        {VAProfileJPEGBaseline, VAEntrypointVLD, "JPEG Baseline VLD"},
        {VAProfileJPEGBaseline, VAEntrypointEncPicture, "JPEG Baseline"},
    };
    for (size_t i = 0; i < sizeof(encoder_entrypoints) / sizeof(encoder_entrypoints[0]); i++) {
        if (!supports_entrypoint(dpy, encoder_entrypoints[i].profile,
                                 encoder_entrypoints[i].entrypoint)) {
            fprintf(stderr, "%s encoder entrypoint is missing\n",
                    encoder_entrypoints[i].name);
            vaTerminate(dpy);
            close(fd);
            return 1;
        }
    }

    VAConfigAttrib h264_encoder_attrs[] = {
        {.type = VAConfigAttribRTFormat},
        {.type = VAConfigAttribRateControl},
        {.type = VAConfigAttribEncPackedHeaders},
        {.type = VAConfigAttribEncMaxRefFrames},
        {.type = VAConfigAttribPredictionDirection},
        {.type = VAConfigAttribEncMaxSlices},
        {.type = VAConfigAttribEncSliceStructure},
        {.type = VAConfigAttribEncQualityRange},
        {.type = VAConfigAttribEncInterlaced},
        {.type = VAConfigAttribEncQuantization},
        {.type = VAConfigAttribEncIntraRefresh},
        {.type = VAConfigAttribMaxPictureWidth},
        {.type = VAConfigAttribMaxPictureHeight},
    };
    const unsigned int h264_encoder_expected[] = {
        VA_RT_FORMAT_YUV420, VA_RC_CBR | VA_RC_VBR | VA_RC_CQP,
        VA_ENC_PACKED_HEADER_NONE, 1,
        VA_PREDICTION_DIRECTION_PREVIOUS, 1, VA_ATTRIB_NOT_SUPPORTED,
        VA_ATTRIB_NOT_SUPPORTED, VA_ENC_INTERLACED_NONE,
        VA_ENC_QUANTIZATION_NONE, VA_ENC_INTRA_REFRESH_NONE, 4096, 4096,
    };
    static const VAProfile h264_encoder_profiles[] = {
        VAProfileH264ConstrainedBaseline, VAProfileH264Main, VAProfileH264High
    };
    static const char *h264_encoder_names[] = {
        "H.264 Constrained Baseline EncSlice", "H.264 Main EncSlice",
        "H.264 High EncSlice"
    };
    for (size_t i = 0;
         i < sizeof(h264_encoder_profiles) / sizeof(h264_encoder_profiles[0]); i++) {
        if (!check_encoder_attributes(dpy, h264_encoder_profiles[i],
                                      VAEntrypointEncSlice, h264_encoder_attrs,
                                      h264_encoder_expected,
                                      sizeof(h264_encoder_expected) /
                                          sizeof(h264_encoder_expected[0]),
                                      h264_encoder_names[i])) {
            vaTerminate(dpy);
            close(fd);
            return 1;
        }
    }

    VAConfigAttrib vbr_attributes[] = {
        { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl, .value = VA_RC_VBR },
    };
    for (size_t i = 0;
         i < sizeof(h264_encoder_profiles) / sizeof(h264_encoder_profiles[0]); i++) {
        VAConfigID vbr_config = VA_INVALID_ID;
        st = vaCreateConfig(dpy, h264_encoder_profiles[i],
                            VAEntrypointEncSlice, vbr_attributes,
                            sizeof(vbr_attributes) / sizeof(vbr_attributes[0]),
                            &vbr_config);
        if (st != VA_STATUS_SUCCESS) {
            fprintf(stderr, "%s VBR config creation failed: %s\n",
                    h264_encoder_names[i], vaErrorStr(st));
            vaTerminate(dpy);
            close(fd);
            return 1;
        }
        st = vaDestroyConfig(dpy, vbr_config);
        if (st != VA_STATUS_SUCCESS) {
            fprintf(stderr, "%s VBR config destruction failed: %s\n",
                    h264_encoder_names[i], vaErrorStr(st));
            vaTerminate(dpy);
            close(fd);
            return 1;
        }
    }

    VAConfigAttrib cqp_attributes[] = {
        { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl, .value = VA_RC_CQP },
    };
    for (size_t i = 0;
         i < sizeof(h264_encoder_profiles) / sizeof(h264_encoder_profiles[0]); i++) {
        VAConfigID cqp_config = VA_INVALID_ID;
        st = vaCreateConfig(dpy, h264_encoder_profiles[i], VAEntrypointEncSlice,
                            cqp_attributes,
                            sizeof(cqp_attributes) / sizeof(cqp_attributes[0]),
                            &cqp_config);
        if (st != VA_STATUS_SUCCESS) {
            fprintf(stderr, "%s CQP config creation failed: %s\n",
                    h264_encoder_names[i], vaErrorStr(st));
            vaTerminate(dpy);
            close(fd);
            return 1;
        }
        st = vaDestroyConfig(dpy, cqp_config);
        if (st != VA_STATUS_SUCCESS) {
            fprintf(stderr, "%s CQP config destruction failed: %s\n",
                    h264_encoder_names[i], vaErrorStr(st));
            vaTerminate(dpy);
            close(fd);
            return 1;
        }
    }

    VAConfigAttrib hevc_encoder_attrs[] = {
        {.type = VAConfigAttribRTFormat},
        {.type = VAConfigAttribRateControl},
        {.type = VAConfigAttribEncPackedHeaders},
        {.type = VAConfigAttribEncMaxRefFrames},
        {.type = VAConfigAttribPredictionDirection},
        {.type = VAConfigAttribEncMaxSlices},
        {.type = VAConfigAttribEncSliceStructure},
        {.type = VAConfigAttribEncQualityRange},
        {.type = VAConfigAttribEncInterlaced},
        {.type = VAConfigAttribEncQuantization},
        {.type = VAConfigAttribEncIntraRefresh},
        {.type = VAConfigAttribMaxPictureWidth},
        {.type = VAConfigAttribMaxPictureHeight},
    };
    const unsigned int hevc_encoder_expected[] = {
        VA_RT_FORMAT_YUV420, VA_RC_CBR | VA_RC_VBR | VA_RC_CQP,
        VA_ENC_PACKED_HEADER_NONE, 1,
        VA_PREDICTION_DIRECTION_PREVIOUS, 1, VA_ATTRIB_NOT_SUPPORTED,
        VA_ATTRIB_NOT_SUPPORTED, VA_ENC_INTERLACED_NONE,
        VA_ENC_QUANTIZATION_NONE, VA_ENC_INTRA_REFRESH_NONE, 3840, 2160,
    };
    if (!check_encoder_attributes(dpy, VAProfileHEVCMain,
                                  VAEntrypointEncSlice, hevc_encoder_attrs,
                                  hevc_encoder_expected,
                                  sizeof(hevc_encoder_expected) /
                                      sizeof(hevc_encoder_expected[0]),
                                  "HEVC Main EncSlice")) {
        vaTerminate(dpy);
        close(fd);
        return 1;
    }
    VAConfigID hevc_cqp_config = VA_INVALID_ID;
    st = vaCreateConfig(dpy, VAProfileHEVCMain, VAEntrypointEncSlice,
                        cqp_attributes,
                        sizeof(cqp_attributes) / sizeof(cqp_attributes[0]),
                        &hevc_cqp_config);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "HEVC Main CQP config creation failed: %s\n",
                vaErrorStr(st));
        vaTerminate(dpy);
        close(fd);
        return 1;
    }
    st = vaDestroyConfig(dpy, hevc_cqp_config);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "HEVC Main CQP config destruction failed: %s\n",
                vaErrorStr(st));
        vaTerminate(dpy);
        close(fd);
        return 1;
    }
    VAConfigID hevc_vbr_config = VA_INVALID_ID;
    st = vaCreateConfig(dpy, VAProfileHEVCMain, VAEntrypointEncSlice,
                        vbr_attributes,
                        sizeof(vbr_attributes) / sizeof(vbr_attributes[0]),
                        &hevc_vbr_config);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "HEVC Main VBR config creation failed: %s\n",
                vaErrorStr(st));
        vaTerminate(dpy);
        close(fd);
        return 1;
    }
    st = vaDestroyConfig(dpy, hevc_vbr_config);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "HEVC Main VBR config destruction failed: %s\n",
                vaErrorStr(st));
        vaTerminate(dpy);
        close(fd);
        return 1;
    }

    VAConfigAttrib jpeg_encoder_attrs[] = {
        {.type = VAConfigAttribRTFormat},
        {.type = VAConfigAttribRateControl},
        {.type = VAConfigAttribEncPackedHeaders},
        {.type = VAConfigAttribEncMaxRefFrames},
        {.type = VAConfigAttribPredictionDirection},
        {.type = VAConfigAttribEncMaxSlices},
        {.type = VAConfigAttribEncInterlaced},
        {.type = VAConfigAttribEncQuantization},
        {.type = VAConfigAttribEncIntraRefresh},
    };
    const unsigned int jpeg_encoder_expected[] = {
        VA_RT_FORMAT_YUV420, VA_RC_CQP, VA_ENC_PACKED_HEADER_NONE,
        VA_ATTRIB_NOT_SUPPORTED, VA_ATTRIB_NOT_SUPPORTED,
        VA_ATTRIB_NOT_SUPPORTED, VA_ENC_INTERLACED_NONE,
        VA_ENC_QUANTIZATION_NONE, VA_ENC_INTRA_REFRESH_NONE,
    };
    if (!check_encoder_attributes(dpy, VAProfileJPEGBaseline,
                                  VAEntrypointEncPicture, jpeg_encoder_attrs,
                                  jpeg_encoder_expected,
                                  sizeof(jpeg_encoder_expected) /
                                      sizeof(jpeg_encoder_expected[0]),
                                  "JPEG Baseline EncPicture")) {
        vaTerminate(dpy);
        close(fd);
        return 1;
    }
    printf("Encoder capabilities: H.264 Constrained Baseline/Main/High CBR/VBR/CQP, HEVC Main CBR/VBR/CQP, and JPEG Baseline verified\n");

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

    VAConfigAttrib hevc_capabilities[] = {
        { .type = VAConfigAttribRTFormat },
        { .type = VAConfigAttribMaxPictureWidth },
        { .type = VAConfigAttribMaxPictureHeight }
    };
    st = vaGetConfigAttributes(dpy, VAProfileHEVCMain, VAEntrypointVLD,
                               hevc_capabilities,
                               sizeof(hevc_capabilities) /
                                   sizeof(hevc_capabilities[0]));
    if (st != VA_STATUS_SUCCESS ||
        hevc_capabilities[0].value != VA_RT_FORMAT_YUV420 ||
        hevc_capabilities[1].value != 8192 ||
        hevc_capabilities[2].value != 4096) {
        fprintf(stderr, "vaGetConfigAttributes returned unexpected HEVC Main VLD capabilities: status=%s rt=%u max=%ux%u\n",
                vaErrorStr(st), hevc_capabilities[0].value,
                hevc_capabilities[1].value, hevc_capabilities[2].value);
        vaTerminate(dpy);
        close(fd);
        return 1;
    }

    VAConfigAttrib hevc_attr = {
        .type = VAConfigAttribRTFormat,
        .value = VA_RT_FORMAT_YUV420
    };
    VAConfigID hevc_config = VA_INVALID_ID;
    st = vaCreateConfig(dpy, VAProfileHEVCMain, VAEntrypointVLD,
                        &hevc_attr, 1, &hevc_config);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "HEVC Main VLD vaCreateConfig failed: %s\n",
                vaErrorStr(st));
        vaTerminate(dpy);
        close(fd);
        return 1;
    }
    unsigned int hevc_surface_width = 0;
    unsigned int hevc_surface_height = 0;
    unsigned int hevc_surface_attr_count = 0;
    st = query_surface_limits(dpy, hevc_config, &hevc_surface_width,
                              &hevc_surface_height,
                              &hevc_surface_attr_count);
    VAStatus hevc_destroy_status = vaDestroyConfig(dpy, hevc_config);
    if (st != VA_STATUS_SUCCESS || hevc_surface_width != 8192 ||
        hevc_surface_height != 4096 ||
        hevc_destroy_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "HEVC Main surface limits mismatch: status=%s max=%ux%u attrs=%u\n",
                vaErrorStr(st), hevc_surface_width, hevc_surface_height,
                hevc_surface_attr_count);
        vaTerminate(dpy);
        close(fd);
        return 1;
    }
    printf("HEVC Main VLD: config max=%ux%u, surface max=%ux%u, attrs=%u\n",
           hevc_capabilities[1].value, hevc_capabilities[2].value,
           hevc_surface_width, hevc_surface_height,
           hevc_surface_attr_count);

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

    unsigned int max_surface_width = 0;
    unsigned int max_surface_height = 0;
    unsigned int num_surface_attrs = 0;
    st = query_surface_limits(dpy, config, &max_surface_width,
                              &max_surface_height, &num_surface_attrs);
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
    st = vaUnlockSurface(dpy, surface);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaUnlockSurface failed before export: %s\n", vaErrorStr(st));
        vaDestroySurfaces(dpy, &surface, 1);
        vaDestroyConfig(dpy, config);
        vaTerminate(dpy);
        close(fd);
        return 1;
    }

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

    vaDestroySurfaces(dpy, &surface, 1);

    VAStatus destroy_st = vaDestroyConfig(dpy, config);
    VAStatus terminate_st = vaTerminate(dpy);
    close(fd);
    return (st == VA_STATUS_SUCCESS && destroy_st == VA_STATUS_SUCCESS &&
            terminate_st == VA_STATUS_SUCCESS) ? 0 : 1;
}
