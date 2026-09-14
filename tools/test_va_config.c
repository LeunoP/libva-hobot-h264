#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drm.h>

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
    int num_attribs = 1;
    st = vaQueryConfigAttributes(dpy, config, &profile, &entrypoint, &attr, &num_attribs);
    printf("vaQueryConfigAttributes: %s, profile=%d, entrypoint=%d, attrs=%d\n",
           vaErrorStr(st), profile, entrypoint, num_attribs);

    VASurfaceID surface = VA_INVALID_SURFACE;
    unsigned int fourcc = 0;
    unsigned int luma_stride = 0;
    unsigned int chroma_stride = 0;
    unsigned int luma_offset = 0;
    unsigned int chroma_offset = 0;
    void *mapped = NULL;

    st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 64, 64, &surface, 1, NULL, 0);
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
    vaUnlockSurface(dpy, surface);
    vaDestroySurfaces(dpy, &surface, 1);

    VAStatus destroy_st = vaDestroyConfig(dpy, config);
    VAStatus terminate_st = vaTerminate(dpy);
    close(fd);
    return (st == VA_STATUS_SUCCESS && destroy_st == VA_STATUS_SUCCESS &&
            terminate_st == VA_STATUS_SUCCESS) ? 0 : 1;
}
