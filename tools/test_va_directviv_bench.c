#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <va/va_hobot.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif
#ifndef GL_VIV_direct_texture
#define GL_VIV_direct_texture 1
#define GL_VIV_NV12           0x8FC1
#endif

typedef void (GL_APIENTRYP PFNGLTEXDIRECTVIVMAPPROC) (GLenum Target, GLsizei Width, GLsizei Height, GLenum Format, GLvoid ** Logical, const GLuint * Physical);
typedef void (GL_APIENTRYP PFNGLTEXDIRECTINVALIDATEVIVPROC) (GLenum Target);

static enum AVPixelFormat get_vaapi_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    (void)ctx;
    for (const enum AVPixelFormat *p = pix_fmts; *p != -1; p++) {
        if (*p == AV_PIX_FMT_VAAPI) return *p;
    }
    return AV_PIX_FMT_NONE;
}

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

static double get_time_sec() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    const char *video_path = (argc > 1) ? argv[1] : "/home/rho412/test_1080p60.mp4";
    printf("===============================================================\n");
    printf("RDK-X5 Real VPU H.264 -> DirectVIV -> GLES Benchmark\n");
    printf("Target video: %s\n", video_path);
    printf("===============================================================\n");

    AVFormatContext *fmt_ctx = NULL;
    if (avformat_open_input(&fmt_ctx, video_path, NULL, NULL) < 0 ||
        avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Failed to open video or read stream information: %s\n",
                video_path);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    int video_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }
    if (video_stream_idx < 0) {
        fprintf(stderr, "No video stream found\n");
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    AVCodecParameters *codecpar = fmt_ctx->streams[video_stream_idx]->codecpar;
    int render_width = codecpar->width;
    int render_height = codecpar->height;
    if (render_width <= 0 || render_height <= 0 ||
        render_width > 8192 || render_height > 4096) {
        fprintf(stderr, "Unsupported render size: %dx%d\n",
                render_width, render_height);
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    int drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror("open /dev/dri/card0");
        avformat_close_input(&fmt_ctx);
        return 1;
    }
    struct gbm_device *gbm = gbm_create_device(drm_fd);
    EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    eglInitialize(dpy, NULL, NULL);
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint num_configs = 0;
    EGLConfig configs[64];
    eglGetConfigs(dpy, configs, 64, &num_configs);
    EGLConfig config = configs[1];

    struct gbm_surface *gbm_surf = gbm_surface_create(gbm, render_width,
        render_height, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
    EGLSurface egl_surf = eglCreateWindowSurface(dpy, config, (EGLNativeWindowType)gbm_surf, NULL);

    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, ctx_attribs);
    eglMakeCurrent(dpy, egl_surf, egl_surf, ctx);

    printf("[1] Vivante GLES Ready. Renderer: %s\n", glGetString(GL_RENDERER));

    PFNGLTEXDIRECTVIVMAPPROC glTexDirectVIVMap =
        (PFNGLTEXDIRECTVIVMAPPROC)eglGetProcAddress("glTexDirectVIVMap");
    PFNGLTEXDIRECTINVALIDATEVIVPROC glTexDirectInvalidateVIV =
        (PFNGLTEXDIRECTINVALIDATEVIVPROC)eglGetProcAddress("glTexDirectInvalidateVIV");

    if (!glTexDirectVIVMap) {
        printf("Error: glTexDirectVIVMap symbol not found!\n");
        return 1;
    }

    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_uv");
    glLinkProgram(prog);
    glUseProgram(prog);

    float verts[] = {
        -1.0f, -1.0f,  0.0f, 1.0f,
         1.0f, -1.0f,  1.0f, 1.0f,
        -1.0f,  1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 0.0f
    };
    glViewport(0, 0, render_width, render_height);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts + 2);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    const AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
    AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, codecpar);

    AVBufferRef *hw_device_ctx = NULL;
    if (av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/card0", NULL, 0) < 0) {
        fprintf(stderr, "Failed to create VAAPI hardware device context\n");
        return 1;
    }
    codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    codec_ctx->get_format = get_vaapi_format;

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        fprintf(stderr, "Failed to open codec\n");
        return 1;
    }

    AVHWDeviceContext *hw_dev = (AVHWDeviceContext *)hw_device_ctx->data;
    AVVAAPIDeviceContext *va_ctx = (AVVAAPIDeviceContext *)hw_dev->hwctx;
    VADisplay va_dpy = va_ctx->display;
    printf("[2] FFmpeg VA-API Initialized (VADisplay=%p)\n", va_dpy);

    #define MAX_CACHED_SURFACES 128
    struct TextureEntry {
        GLuint tex;
        uint64_t phys_addr;
    } tex_cache[MAX_CACHED_SURFACES] = {0};

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    int total_frames = 0;
    int64_t expected_frames = fmt_ctx->streams[video_stream_idx]->nb_frames;
    int benchmark_failed = 0;
    int frame_limit_reached = 0;
    const char *validate_pixels_env = getenv("HOBOT_DIRECTVIV_VALIDATE_PIXELS");
    int validate_pixels = validate_pixels_env &&
        strcmp(validate_pixels_env, "1") == 0;
    int pixel_validation_done = 0;
    double start_time = get_time_sec();
    double last_report_time = start_time;
    int report_frames = 0;

    printf("\n[3] Starting Real VPU Decode + DirectVIV Render Pipeline...\n");

    int demux_eof = 0;
    while (!demux_eof) {
        int read_status = av_read_frame(fmt_ctx, pkt);
        int receive_frames = 0;

        if (read_status < 0) {
            if (read_status != AVERROR_EOF) {
                char error_text[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(read_status, error_text, sizeof(error_text));
                fprintf(stderr, "Failed reading input: %s\n", error_text);
                benchmark_failed = 1;
                break;
            }
            demux_eof = 1;
            if (avcodec_send_packet(codec_ctx, NULL) == 0) {
                receive_frames = 1;
            } else {
                fprintf(stderr, "Failed to flush decoder at end of input\n");
                benchmark_failed = 1;
                break;
            }
        } else if (pkt->stream_index == video_stream_idx) {
            if (avcodec_send_packet(codec_ctx, pkt) == 0) {
                receive_frames = 1;
            } else {
                fprintf(stderr, "Failed to submit packet to decoder\n");
                av_packet_unref(pkt);
                benchmark_failed = 1;
                break;
            }
        }

        if (receive_frames) {
            int receive_status;
            while ((receive_status = avcodec_receive_frame(codec_ctx, frame)) == 0) {
                if (frame->format != AV_PIX_FMT_VAAPI) {
                    fprintf(stderr, "Decoder returned unexpected pixel format: %d\n",
                            frame->format);
                    av_frame_unref(frame);
                    benchmark_failed = 1;
                    break;
                }
                if (frame->format == AV_PIX_FMT_VAAPI) {
                    VASurfaceID surf_id = (VASurfaceID)(uintptr_t)frame->data[3];

                    struct hobot_surface_info hinfo = {0};
                    VAStatus va_ret = vaGetHobotSurfaceInfo(va_dpy, surf_id, &hinfo);

                    if (va_ret == VA_STATUS_SUCCESS &&
                        hinfo.phys_addr[0] > 0 && hinfo.phys_addr[1] > 0 &&
                        hinfo.virt_addr[0] && hinfo.virt_addr[1] &&
                        hinfo.width > 0 && hinfo.height > 0 &&
                        hinfo.stride >= hinfo.width &&
                        hinfo.vstride >= hinfo.height) {
                        if (total_frames == 0) {
                            printf("--- First Decoded VPU Surface Info (vaGetHobotSurfaceInfo) ---\n");
                            printf("  VASurfaceID:   %u\n", surf_id);
                            printf("  DMA-BUF fd:    %d\n", hinfo.dma_fd);
                            printf("  Dimensions:    %ux%u, Stride: %u, VStride: %u\n",
                                   hinfo.width, hinfo.height, hinfo.stride, hinfo.vstride);
                            printf("  PHYSICAL ADDR: Y=0x%010llx, UV=0x%010llx\n",
                                   (unsigned long long)hinfo.phys_addr[0], (unsigned long long)hinfo.phys_addr[1]);
                            printf("  VIRTUAL ADDR:  Y=%p, UV=%p\n", hinfo.virt_addr[0], hinfo.virt_addr[1]);
                        }

                        struct TextureEntry *entry = &tex_cache[surf_id % MAX_CACHED_SURFACES];
                        if (entry->tex == 0 || entry->phys_addr != hinfo.phys_addr[0]) {
                            if (entry->tex == 0) glGenTextures(1, &entry->tex);
                            entry->phys_addr = hinfo.phys_addr[0];

                            glBindTexture(GL_TEXTURE_2D, entry->tex);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

                            GLvoid *logical[2] = {
                                hinfo.virt_addr[0],
                                hinfo.virt_addr[1]
                            };
                            GLuint physical[2] = {
                                (GLuint)hinfo.phys_addr[0],
                                (GLuint)hinfo.phys_addr[1]
                            };

                            glTexDirectVIVMap(GL_TEXTURE_2D, hinfo.width, hinfo.height, GL_VIV_NV12, logical, physical);
                        } else {
                            glBindTexture(GL_TEXTURE_2D, entry->tex);
                            if (glTexDirectInvalidateVIV) {
                                glTexDirectInvalidateVIV(GL_TEXTURE_2D);
                            }
                        }

                        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
                        glFinish(); // Ensure GPU finishes before surface is reused

                        GLenum gl_error = glGetError();
                        if (gl_error != GL_NO_ERROR) {
                            fprintf(stderr, "GLES DirectVIV render failed: GL error 0x%x\n",
                                    gl_error);
                            benchmark_failed = 1;
                        } else {
                            if (validate_pixels && !pixel_validation_done) {
                                const GLint sample_points[5][2] = {
                                    {render_width / 4, render_height / 4},
                                    {render_width / 2, render_height / 4},
                                    {render_width / 2, render_height / 2},
                                    {render_width / 4, (render_height * 3) / 4},
                                    {(render_width * 3) / 4, (render_height * 3) / 4}
                                };
                                unsigned char samples[5][4] = {{0}};
                                int nonblack = 0;
                                int varied = 0;

                                for (int i = 0; i < 5; i++) {
                                    glReadPixels(sample_points[i][0],
                                        sample_points[i][1], 1, 1,
                                        GL_RGBA, GL_UNSIGNED_BYTE, samples[i]);
                                    if (glGetError() != GL_NO_ERROR) {
                                        fprintf(stderr, "DirectVIV framebuffer readback failed\n");
                                        benchmark_failed = 1;
                                        break;
                                    }
                                    if (samples[i][0] || samples[i][1] || samples[i][2])
                                        nonblack++;
                                    if (i > 0 && memcmp(samples[0], samples[i], 3) != 0)
                                        varied++;
                                }

                                if (!benchmark_failed && (nonblack < 3 || varied < 2)) {
                                    fprintf(stderr,
                                        "DirectVIV framebuffer samples are blank or uniform\n");
                                    benchmark_failed = 1;
                                }
                                if (!benchmark_failed) {
                                    printf("GPU framebuffer readback passed: %d nonblack samples, %d differ from first\n",
                                        nonblack, varied);
                                    for (int i = 0; i < 5; i++)
                                        printf("  sample %d RGB=%u,%u,%u\n", i,
                                            samples[i][0], samples[i][1],
                                            samples[i][2]);
                                    pixel_validation_done = 1;
                                }
                            }
                            if (benchmark_failed) {
                                av_frame_unref(frame);
                                break;
                            }
                            total_frames++;
                            report_frames++;

                            double cur_time = get_time_sec();
                            if (cur_time - last_report_time >= 1.0) {
                                double fps = report_frames / (cur_time - last_report_time);
                                printf("  [Live Progress] Frame %d, Speed: %.1f FPS\n", total_frames, fps);
                                last_report_time = cur_time;
                                report_frames = 0;
                            }
                        }
                    } else {
                        fprintf(stderr, "vaGetHobotSurfaceInfo failed or returned an invalid surface: status=0x%x phys=0x%llx\n",
                                va_ret,
                                (unsigned long long)hinfo.phys_addr[0]);
                        benchmark_failed = 1;
                    }
                }
                av_frame_unref(frame);
                if (benchmark_failed)
                    break;
            }
            if (benchmark_failed) {
                if (read_status >= 0)
                    av_packet_unref(pkt);
                break;
            }
            int expected_receive_status = demux_eof ? AVERROR_EOF : AVERROR(EAGAIN);
            if (receive_status != expected_receive_status) {
                char error_text[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(receive_status, error_text, sizeof(error_text));
                fprintf(stderr, "Decoder frame receive failed: %s (%d)\n",
                        error_text, receive_status);
                benchmark_failed = 1;
                if (read_status >= 0)
                    av_packet_unref(pkt);
                break;
            }
        }

        if (read_status >= 0)
            av_packet_unref(pkt);
        if (total_frames >= 3000) {
            frame_limit_reached = 1;
            break;
        }
    }

    double total_time = get_time_sec() - start_time;
    double avg_fps = total_frames / total_time;

    if (total_frames == 0 && !benchmark_failed) {
        fprintf(stderr, "Benchmark produced no decoded and rendered frames\n");
        benchmark_failed = 1;
    }
    if (validate_pixels && !pixel_validation_done) {
        fprintf(stderr, "DirectVIV framebuffer pixel validation did not run\n");
        benchmark_failed = 1;
    }

    printf("\n===============================================================\n");
    printf(">>> BENCHMARK COMPLETE <<<\n");
    printf("Total Frames Decoded & Rendered: %d\n", total_frames);
    printf("Render Target:                   %dx%d\n", render_width, render_height);
    if (expected_frames > 0)
        printf("Expected Input Frames:           %lld\n", (long long)expected_frames);
    printf("Total Elapsed Time:              %.2f seconds\n", total_time);
    printf("Average Processing Speed:        %.2f FPS\n", avg_fps);
    if (!benchmark_failed && !frame_limit_reached && expected_frames > 0 &&
        total_frames != expected_frames) {
        fprintf(stderr, "Frame count mismatch: expected %lld, rendered %d\n",
                (long long)expected_frames, total_frames);
        benchmark_failed = 1;
    }
    printf("===============================================================\n");

    for (int i = 0; i < MAX_CACHED_SURFACES; i++) {
        if (tex_cache[i].tex) glDeleteTextures(1, &tex_cache[i].tex);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
    av_buffer_unref(&hw_device_ctx);

    glDeleteProgram(prog);
    eglDestroySurface(dpy, egl_surf);
    gbm_surface_destroy(gbm_surf);
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    gbm_device_destroy(gbm);
    close(drm_fd);

    return benchmark_failed ? 1 : 0;
}
