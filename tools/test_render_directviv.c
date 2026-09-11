#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
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

static const char *vs_src =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "    v_uv = a_uv;\n"
    "}\n";

static const char *fs_src =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_tex, v_uv);\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        printf("Shader compile failed: %s\n", log);
    }
    return s;
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("====================================================\n");
    printf("Vivante DirectVIV NV12 Full GLES Render Pipeline Test\n");
    printf("====================================================\n");

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

    PFNGLTEXDIRECTVIVMAPPROC glTexDirectVIVMap =
        (PFNGLTEXDIRECTVIVMAPPROC)eglGetProcAddress("glTexDirectVIVMap");
    PFNGLTEXDIRECTINVALIDATEVIVPROC glTexDirectInvalidateVIV =
        (PFNGLTEXDIRECTINVALIDATEVIVPROC)eglGetProcAddress("glTexDirectInvalidateVIV");

    // Build shader program
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_uv");
    glLinkProgram(prog);
    glUseProgram(prog);

    hb_mem_module_open();
    int width = 1920, height = 1080;
    int aligned_w = (width + 15) & ~15;
    int aligned_h = (height + 7) & ~7;
    int64_t mflags = HB_MEM_USAGE_CPU_READ_OFTEN | HB_MEM_USAGE_CPU_WRITE_OFTEN |
                     HB_MEM_USAGE_HW_VIDEO_CODEC | HB_MEM_USAGE_CACHED |
                     HB_MEM_USAGE_PRIV_HEAP_RESERVED | HB_MEM_USAGE_GRAPHIC_CONTIGUOUS_BUF;

    hb_mem_graphic_buf_t gbuf = {0};
    hb_mem_alloc_graph_buf(width, height, MEM_PIX_FMT_NV12, mflags, aligned_w, aligned_h, &gbuf);

    // Fill with known pattern: Red in YUV (Y=81, U=90, V=240)
    int y_size = gbuf.stride * (gbuf.vstride > 0 ? gbuf.vstride : aligned_h);
    memset(gbuf.virt_addr[0], 81, y_size);
    memset(gbuf.virt_addr[1], 90, y_size / 4);     // U
    memset(gbuf.virt_addr[1] + y_size / 4, 240, y_size / 4); // V
    hb_mem_flush_buf(gbuf.fd[0], 0, gbuf.size[0]);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    GLvoid *logical[2] = { gbuf.virt_addr[0], gbuf.virt_addr[1] };
    GLuint physical[2] = { (GLuint)gbuf.phys_addr[0], (GLuint)gbuf.phys_addr[1] };
    glTexDirectVIVMap(GL_TEXTURE_2D, width, height, GL_VIV_NV12, logical, physical);
    printf("DirectVIV mapped: err=0x%04x\n", glGetError());

    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);

    // Render quad
    float verts[] = {
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 1.0f,
        -1.0f,  1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 0.0f
    };
    glViewport(0, 0, 64, 64);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    GLenum render_err = glGetError();
    printf(">>> glDrawArrays result: err=0x%04x <<<\n", render_err);

    // Read back a pixel from rendered FBO/surface
    unsigned char pixel[4] = {0};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    printf(">>> Sampled pixel color (RGBA): R=%d, G=%d, B=%d, A=%d <<<\n",
           pixel[0], pixel[1], pixel[2], pixel[3]);

    if (pixel[0] > 100) {
        printf(">>> [PERFECT SUCCESS] Hardware YUV to RGB Conversion Confirmed! R=%d > 100 <<<\n", pixel[0]);
    }

    glDeleteTextures(1, &tex);
    glDeleteProgram(prog);
    hb_mem_free_buf(gbuf.fd[0]);
    hb_mem_module_close();

    eglDestroySurface(dpy, egl_surf);
    gbm_surface_destroy(gbm_surf);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    gbm_device_destroy(gbm);
    close(drm_fd);
    printf("Test completed cleanly.\n");
    return 0;
}
