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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Vendor-specific memory type for vaExportSurfaceHandle to retrieve Hobot graphics buffer details.
 * FourCC: 'H' 'O' 'B' '1' (0x484F4231)
 */
#define VA_SURFACE_ATTRIB_MEM_TYPE_HOBOT_GRAPH_BUF 0x484F4231

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

#ifdef HOBOT_DRIVER_BUILD
/**
 * Exported symbol in hobot_drv_video.so for direct C linkage or backwards compatibility.
 */
VAStatus vaGetHobotSurfaceInfo(
    VADisplay dpy,
    VASurfaceID surface,
    struct hobot_surface_info *info
);
#else
/**
 * Query Hobot surface hardware information via standard libva dispatch (vaExportSurfaceHandle).
 *
 * This uses the official VA-API driver dispatch mechanism:
 *   vaExportSurfaceHandle(dpy, surface, VA_SURFACE_ATTRIB_MEM_TYPE_HOBOT_GRAPH_BUF, 0, info)
 *
 * Clean, standard, and portable: zero external dependencies (no dlopen, no dlsym, no dlfcn.h).
 */
static inline VAStatus vaGetHobotSurfaceInfo(
    VADisplay dpy,
    VASurfaceID surface,
    struct hobot_surface_info *info
) {
    return vaExportSurfaceHandle(dpy, surface,
                                 VA_SURFACE_ATTRIB_MEM_TYPE_HOBOT_GRAPH_BUF,
                                 0,
                                 (void *)info);
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* _VA_HOBOT_H_ */
