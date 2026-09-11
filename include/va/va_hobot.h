/*
 * D-Robotics RDK-X5 VPU VA-API Extension Interface
 *
 * Provides direct access to physical memory addresses and hardware surface
 * metadata for Vivante DirectVIV (GL_VIV_direct_texture) zero-copy rendering.
 *
 * Licensed under the MIT License.
 */

#ifndef _VA_HOBOT_H_
#define _VA_HOBOT_H_

#include <va/va.h>
#include <stdint.h>
#include <dlfcn.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Surface hardware information required for Vivante DirectVIV zero-copy mapping.
 */
struct hobot_surface_info {
    uint64_t phys_addr[2];  /**< Physical base addresses: [0] = Y plane, [1] = UV plane */
    void *virt_addr[2];      /**< Kernel-mapped virtual addresses: [0] = Y plane, [1] = UV plane */
    uint32_t stride;        /**< Stride (pitch) in bytes */
    uint32_t vstride;       /**< Vertical stride (height alignment) in lines */
    uint32_t width;         /**< Picture width in pixels */
    uint32_t height;        /**< Picture height in pixels */
    int dma_fd;             /**< DMA-BUF file descriptor */
};

typedef VAStatus (*vaGetHobotSurfaceInfo_fn)(
    VADisplay dpy,
    VASurfaceID surface,
    struct hobot_surface_info *info
);

#ifdef HOBOT_DRIVER_BUILD
/**
 * Query internal surface memory descriptors (physical address, virtual address, stride).
 * Exported by hobot_drv_video.so.
 */
VAStatus vaGetHobotSurfaceInfo(
    VADisplay dpy,
    VASurfaceID surface,
    struct hobot_surface_info *info
);
#else
/**
 * Inline dispatch wrapper that dynamically resolves vaGetHobotSurfaceInfo
 * from the loaded VA-API driver without requiring link-time -lhobot_drv_video.
 */
static inline VAStatus vaGetHobotSurfaceInfo(
    VADisplay dpy,
    VASurfaceID surface,
    struct hobot_surface_info *info
) {
    static vaGetHobotSurfaceInfo_fn pfn = NULL;
    if (!pfn) {
        pfn = (vaGetHobotSurfaceInfo_fn)dlsym(RTLD_DEFAULT, "vaGetHobotSurfaceInfo");
        if (!pfn) {
            void *handle = dlopen("/usr/lib/aarch64-linux-gnu/dri/hobot_drv_video.so", RTLD_LAZY | RTLD_NOLOAD);
            if (handle) {
                pfn = (vaGetHobotSurfaceInfo_fn)dlsym(handle, "vaGetHobotSurfaceInfo");
            }
        }
    }
    if (!pfn) return VA_STATUS_ERROR_UNKNOWN;
    return pfn(dpy, surface, info);
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* _VA_HOBOT_H_ */
