#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <drm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <hb_mem_mgr.h>

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("====================================================\n");
    printf("RDK-X5 NV12 DMA-BUF -> DRM KMS Overlay Plane 41 Test\n");
    printf("====================================================\n");

    int drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror("open /dev/dri/card0");
        return 1;
    }

    // DRM Master check / SetClientCap
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1);

    hb_mem_module_open();

    int width = 1920;
    int height = 1080;
    int aligned_w = (width + 15) & ~15;
    int aligned_h = (height + 7) & ~7;

    hb_mem_graphic_buf_t gbuf = {0};
    int64_t mflags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN |
                     HB_MEM_USAGE_HW_VIDEO_CODEC | HB_MEM_USAGE_CACHED |
                     HB_MEM_USAGE_PRIV_HEAP_RESERVED | HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;

    int ret = hb_mem_alloc_graph_buf(width, height, MEM_PIX_FMT_NV12, mflags, aligned_w, aligned_h, &gbuf);
    printf("[1] hb_mem_alloc_graph_buf: ret=%d\n", ret);
    if (ret != 0) {
        printf("Allocation failed!\n");
        return 1;
    }

    printf("--- Buffer Metadata ---\n");
    printf("  width: %d, height: %d\n", gbuf.width, gbuf.height);
    printf("  stride: %d, vstride: %d\n", gbuf.stride, gbuf.vstride);
    printf("  plane_cnt: %d\n", gbuf.plane_cnt);
    printf("  is_contig: %d\n", gbuf.is_contig);
    printf("  fd[0]: %d, fd[1]: %d\n", gbuf.fd[0], gbuf.fd[1]);
    printf("  size[0]: %llu, size[1]: %llu\n", (unsigned long long)gbuf.size[0], (unsigned long long)gbuf.size[1]);
    printf("  virt_addr[0]: %p, virt_addr[1]: %p\n", gbuf.virt_addr[0], gbuf.virt_addr[1]);
    printf("  phys_addr[0]: 0x%llx, phys_addr[1]: 0x%llx\n", (unsigned long long)gbuf.phys_addr[0], (unsigned long long)gbuf.phys_addr[1]);
    printf("  offset[0]: %llu, offset[1]: %llu\n", (unsigned long long)gbuf.offset[0], (unsigned long long)gbuf.offset[1]);

    // Fill with test pattern: Vivid Green in YUV (Y=150, U=40, V=40)
    int y_size = gbuf.stride * (gbuf.vstride > 0 ? gbuf.vstride : aligned_h);
    if (gbuf.virt_addr[0]) {
        memset(gbuf.virt_addr[0], 180, y_size); // Y
    }
    if (gbuf.virt_addr[1]) {
        memset(gbuf.virt_addr[1], 50, y_size / 2); // UV
    } else if (gbuf.virt_addr[0]) {
        // Single virtual buffer fallback
        memset(gbuf.virt_addr[0] + y_size, 50, y_size / 2);
    }

    // Flush cache
    hb_mem_flush_buf(gbuf.fd[0], 0, gbuf.size[0]);

    // Import DMA-BUF to DRM handle
    uint32_t gem_handle0 = 0;
    ret = drmPrimeFDToHandle(drm_fd, gbuf.fd[0], &gem_handle0);
    printf("\n[2] drmPrimeFDToHandle(fd=%d): ret=%d, handle=%u\n", gbuf.fd[0], ret, gem_handle0);
    if (ret != 0) {
        perror("drmPrimeFDToHandle failed");
        return 1;
    }

    uint32_t gem_handle1 = gem_handle0;
    if (gbuf.fd[1] > 0 && gbuf.fd[1] != gbuf.fd[0]) {
        ret = drmPrimeFDToHandle(drm_fd, gbuf.fd[1], &gem_handle1);
        printf("[2b] drmPrimeFDToHandle(fd[1]=%d): ret=%d, handle=%u\n", gbuf.fd[1], ret, gem_handle1);
    }

    // Setup drmModeAddFB2 handles, pitches, offsets
    uint32_t handles[4] = { gem_handle0, gem_handle1, 0, 0 };
    uint32_t pitches[4] = { (uint32_t)gbuf.stride, (uint32_t)gbuf.stride, 0, 0 };
    uint32_t uv_offset = (uint32_t)gbuf.offset[1];
    if (uv_offset == 0 && gem_handle0 == gem_handle1) {
        uv_offset = (uint32_t)y_size;
    }
    uint32_t offsets[4] = { (uint32_t)gbuf.offset[0], uv_offset, 0, 0 };
    printf("  offsets: [0]=%u, [1]=%u\n", offsets[0], offsets[1]);
    printf("  pitches: [0]=%u, [1]=%u\n", pitches[0], pitches[1]);

    uint32_t fb_id = 0;
    ret = drmModeAddFB2(drm_fd, width, height, DRM_FORMAT_NV12, handles, pitches, offsets, &fb_id, 0);
    printf("\n[3] drmModeAddFB2(DRM_FORMAT_NV12): ret=%d, fb_id=%u\n", ret, fb_id);
    if (ret != 0) {
        perror("drmModeAddFB2 failed");
        // Try drmModeAddFB2WithModifiers with LINEAR
        uint64_t modifiers[4] = { DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_LINEAR, 0, 0 };
        ret = drmModeAddFB2WithModifiers(drm_fd, width, height, DRM_FORMAT_NV12, handles, pitches, offsets, modifiers, &fb_id, DRM_MODE_FB_MODIFIERS);
        printf("    drmModeAddFB2WithModifiers(LINEAR): ret=%d, fb_id=%u\n", ret, fb_id);
        if (ret != 0) {
            perror("drmModeAddFB2WithModifiers failed");
        }
    }

    if (fb_id > 0) {
        printf("\n[4] Testing drmModeSetPlane on Plane 41, CRTC 31...\n");
        uint32_t plane_id = 41;
        uint32_t crtc_id = 31;
        int crtc_x = 0, crtc_y = 0;
        int crtc_w = 1920, crtc_h = 1080;
        int src_x = 0, src_y = 0;
        int src_w = width << 16;
        int src_h = height << 16;

        ret = drmModeSetPlane(drm_fd, plane_id, crtc_id, fb_id, 0,
                              crtc_x, crtc_y, crtc_w, crtc_h,
                              src_x, src_y, src_w, src_h);
        printf(">>> drmModeSetPlane result: ret=%d <<<\n", ret);
        if (ret != 0) {
            perror("drmModeSetPlane failed");
        } else {
            printf(">>> [SUCCESS] Plane 41 displayed! Sleeping 3 seconds to observe...\n");
            sleep(3);
            // Disable plane
            drmModeSetPlane(drm_fd, plane_id, crtc_id, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            printf("[5] Plane 41 disabled cleanly.\n");
        }

        drmModeRmFB(drm_fd, fb_id);
    }

    hb_mem_free_buf(gbuf.fd[0]);
    hb_mem_module_close();
    close(drm_fd);
    printf("Test finished.\n");
    return 0;
}
