#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <hb_mem_mgr.h>

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif
#ifndef GL_VIV_direct_texture
#define GL_VIV_direct_texture 1
#define GL_VIV_NV12           0x8FC1
#endif

typedef void (GL_APIENTRYP PFNGLTEXDIRECTVIVMAPPROC) (GLenum Target, GLsizei Width, GLsizei Height, GLenum Format, GLvoid ** Logical, const GLuint * Physical);
typedef void (GL_APIENTRYP PFNGLTEXDIRECTINVALIDATEVIVPROC) (GLenum Target);

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("==================================================\n");
    printf("Vivante DirectVIV (glTexDirectVIVMap) NV12 Test\n");
    printf("==================================================\n");

    int drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    struct gbm_device *gbm = gbm_create_device(drm_fd);
    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    eglInitialize(dpy, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint num_configs = 0;
    EGLConfig configs[64];
    eglGetConfigs(dpy, configs, 64, &num_configs);
    EGLConfig config = configs[1];

    struct gbm_surface *gbm_surf = gbm_surface_create(gbm, 64, 64, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
    EGLSurface egl_surf = eglCreateWindowSurface(dpy, config, (EGLNativeWindowType)gbm_surf, NULL);

    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attribs);
    eglMakeCurrent(dpy, egl_surf, egl_surf, ctx);

    printf("[1] EGL & GLES Ready (Renderer: %s)\n", glGetString(GL_RENDERER));

    PFNGLTEXDIRECTVIVMAPPROC glTexDirectVIVMap =
        (PFNGLTEXDIRECTVIVMAPPROC)eglGetProcAddress("glTexDirectVIVMap");
    PFNGLTEXDIRECTINVALIDATEVIVPROC glTexDirectInvalidateVIV =
        (PFNGLTEXDIRECTINVALIDATEVIVPROC)eglGetProcAddress("glTexDirectInvalidateVIV");

    printf("  glTexDirectVIVMap fn pointer: %p\n", glTexDirectVIVMap);
    if (!glTexDirectVIVMap) {
        printf("glTexDirectVIVMap not found!\n");
        return 1;
    }

    hb_mem_module_open();
    int width = 1920, height = 1080;
    int aligned_w = (width + 15) & ~15;
    int aligned_h = (height + 7) & ~7;
    int64_t mflags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN |
                     HB_MEM_USAGE_HW_VIDEO_CODEC | HB_MEM_USAGE_CACHED |
                     HB_MEM_USAGE_PRIV_HEAP_RESERVED | HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;

    hb_mem_graphic_buf_t gbuf = {0};
    int ret = hb_mem_alloc_graph_buf(width, height, MEM_PIX_FMT_NV12, mflags, aligned_w, aligned_h, &gbuf);
    printf("[2] hb_mem_alloc_graph_buf: ret=%d, is_contig=%d\n", ret, gbuf.is_contig);
    printf("    virt_addr: [0]=%p, [1]=%p\n", gbuf.virt_addr[0], gbuf.virt_addr[1]);
    printf("    phys_addr: [0]=0x%llx, [1]=0x%llx\n", (unsigned long long)gbuf.phys_addr[0], (unsigned long long)gbuf.phys_addr[1]);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    GLvoid *logical[2] = { gbuf.virt_addr[0], gbuf.virt_addr[1] };
    GLuint physical[2] = { (GLuint)gbuf.phys_addr[0], (GLuint)gbuf.phys_addr[1] };

    printf("[3] Calling glTexDirectVIVMap(GL_TEXTURE_2D, %d, %d, GL_VIV_NV12, ...)...\n", width, height);
    glTexDirectVIVMap(GL_TEXTURE_2D, width, height, GL_VIV_NV12, logical, physical);
    GLenum err = glGetError();
    printf(">>> glTexDirectVIVMap result: glGetError() = 0x%04x <<<\n", err);

    if (err == GL_NO_ERROR) {
        printf(">>> [SUCCESS] DirectVIV NV12 Texture Created Successfully! <<<\n");
        if (glTexDirectInvalidateVIV) {
            glTexDirectInvalidateVIV(GL_TEXTURE_2D);
            printf(">>> glTexDirectInvalidateVIV called cleanly. <<<\n");
        }
    }

    glDeleteTextures(1, &tex);
    hb_mem_free_buf(gbuf.fd[0]);
    hb_mem_module_close();

    eglDestroySurface(dpy, egl_surf);
    gbm_surface_destroy(gbm_surf);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    gbm_device_destroy(gbm);
    close(drm_fd);
    printf("DirectVIV test finished cleanly.\n");
    return 0;
}
