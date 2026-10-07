#define _GNU_SOURCE
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drmcommon.h>
#include <va/va_hobot.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/pixdesc.h>
#include <errno.h>

static enum AVPixelFormat get_vaapi_format(AVCodecContext *context,
                                            const enum AVPixelFormat *formats)
{
    (void)context;
    for (const enum AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE; format++) {
        if (*format == AV_PIX_FMT_VAAPI)
            return *format;
    }
    return AV_PIX_FMT_NONE;
}

static int check_export(VADisplay display, VASurfaceID surface, int separate_layers)
{
    VADRMPRIMESurfaceDescriptor descriptor = {0};
    uint32_t flags = VA_EXPORT_SURFACE_READ_ONLY;
    if (separate_layers)
        flags |= VA_EXPORT_SURFACE_SEPARATE_LAYERS;

    VAStatus status = vaExportSurfaceHandle(display, surface,
                                            VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                            flags, &descriptor);
    int valid = status == VA_STATUS_SUCCESS && descriptor.num_objects > 0 &&
                descriptor.num_objects <= sizeof(descriptor.objects) / sizeof(descriptor.objects[0]);
    if (valid && separate_layers) {
        valid = descriptor.num_layers == 2 && descriptor.layers[0].num_planes == 1 &&
                descriptor.layers[1].num_planes == 1;
    } else if (valid) {
        valid = descriptor.num_layers == 1 && descriptor.layers[0].drm_format == VA_FOURCC_NV12 &&
                descriptor.layers[0].num_planes == 2;
    }

    if (valid) {
        const uint32_t planes = separate_layers ? 2 : descriptor.layers[0].num_planes;
        for (uint32_t plane = 0; plane < planes && valid; plane++) {
            uint32_t layer = separate_layers ? plane : 0;
            uint32_t plane_index = separate_layers ? 0 : plane;
            uint32_t object = descriptor.layers[layer].object_index[plane_index];
            uint64_t offset = descriptor.layers[layer].offset[plane_index];
            uint64_t pitch = descriptor.layers[layer].pitch[plane_index];
            uint64_t rows = plane == 0 ? descriptor.height : (descriptor.height + 1u) / 2u;
            valid = object < descriptor.num_objects && pitch > 0 && rows > 0 &&
                    pitch <= UINT64_MAX / rows;
            if (valid) {
                uint64_t extent = pitch * rows;
                uint64_t object_size = descriptor.objects[object].size;
                valid = offset <= object_size && extent <= object_size - offset;
            }
        }
    }

    if (status == VA_STATUS_SUCCESS && descriptor.num_objects <= 4) {
        for (uint32_t i = 0; i < descriptor.num_objects; i++)
            close(descriptor.objects[i].fd);
    }
    if (!valid) {
        fprintf(stderr, "PRIME export failed: status=%d separate=%d objects=%u layers=%u\n",
                status, separate_layers, descriptor.num_objects, descriptor.num_layers);
        return -1;
    }

    return 0;
}

static int check_buffer_handle(VADisplay display, VASurfaceID surface)
{
    VAImage image = {0};
    image.image_id = VA_INVALID_ID;
    VAStatus status = vaDeriveImage(display, surface, &image);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "vaDeriveImage failed before handle acquire: status=%d\n",
                status);
        return -1;
    }

    VABufferInfo info = {0};
    info.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    status = vaAcquireBufferHandle(display, image.buf, &info);
    int acquired = status == VA_STATUS_SUCCESS;
    int valid = acquired && info.handle <= INT32_MAX &&
                info.type == VAImageBufferType &&
                info.mem_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME &&
                info.mem_size >= image.data_size;
    if (valid) {
        struct stat st;
        valid = fstat((int)info.handle, &st) == 0 &&
                vaSyncSurface(display, surface) == VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (acquired) {
        int handle = (int)info.handle;
        VAStatus release_status = vaReleaseBufferHandle(display, image.buf);
        valid = valid && release_status == VA_STATUS_SUCCESS &&
                fcntl(handle, F_GETFD) == -1 && errno == EBADF;
        if (release_status != VA_STATUS_SUCCESS)
            fprintf(stderr, "vaReleaseBufferHandle failed: status=%d\n",
                    release_status);
    }

    VAStatus destroy_status = vaDestroyImage(display, image.image_id);
    valid = valid && destroy_status == VA_STATUS_SUCCESS;
    if (!valid)
        fprintf(stderr, "DRM PRIME buffer-handle lifecycle failed: acquire=%d\n",
                status);
    return valid ? 0 : -1;
}

static int receive_frames(AVCodecContext *decoder, VADisplay display,
                          unsigned long long *decoded_frames,
                          unsigned long long *sync_timeouts)
{
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return AVERROR(ENOMEM);

    int result = 0;
    int status;
    while ((status = avcodec_receive_frame(decoder, frame)) >= 0) {
        if (frame->format != AV_PIX_FMT_VAAPI) {
            fprintf(stderr, "decoder returned non-VAAPI frame format %s\n",
                    av_get_pix_fmt_name(frame->format));
            result = AVERROR_INVALIDDATA;
            break;
        }

        VASurfaceID surface = (VASurfaceID)(uintptr_t)frame->data[3];
        status = vaSyncSurface2(display, surface, 1000000ULL);
        if (status == VA_STATUS_ERROR_TIMEDOUT) {
            (*sync_timeouts)++;
            status = vaSyncSurface2(display, surface, VA_TIMEOUT_INFINITE);
        }
        if (status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "surface synchronization failed: status=%d\n", status);
            result = AVERROR_EXTERNAL;
            break;
        }
        if (*decoded_frames == 0) {
            struct hobot_surface_info info = {0};
            status = vaGetHobotSurfaceInfo(display, surface, &info);
            if (status != VA_STATUS_SUCCESS) {
                fprintf(stderr, "surface-info query failed: status=%d\n", status);
                result = AVERROR_EXTERNAL;
                break;
            }
            printf("first decoded surface metadata: %ux%u y_pitch=%u vertical_stride=%u uv_offset=%llu\n",
                   info.width, info.height, info.stride, info.vstride,
                   (unsigned long long)((uintptr_t)info.virt_addr[1] -
                                        (uintptr_t)info.virt_addr[0]));
        }
        if (check_export(display, surface, 1) != 0 ||
            check_export(display, surface, 0) != 0 ||
            check_buffer_handle(display, surface) != 0) {
            result = AVERROR_EXTERNAL;
            break;
        }
        (*decoded_frames)++;
        av_frame_unref(frame);
    }

    if (status < 0 && status != AVERROR(EAGAIN) && status != AVERROR_EOF && result == 0)
        result = status;
    av_frame_free(&frame);
    return result;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/home/rho412/test_1080p60.mp4";
    AVFormatContext *format = NULL;
    AVCodecContext *decoder = NULL;
    AVBufferRef *device = NULL;
    AVPacket *packet = NULL;
    int result = 1;
    unsigned long long decoded_frames = 0;
    unsigned long long sync_timeouts = 0;
    int video_stream = -1;
    int read_status;
    const char *drm_device = getenv("HOBOT_DRM_DEVICE");
    if (!drm_device || !*drm_device)
        drm_device = "/dev/dri/renderD128";

    if (avformat_open_input(&format, path, NULL, NULL) < 0 ||
        avformat_find_stream_info(format, NULL) < 0) {
        fprintf(stderr, "could not open input: %s\n", path);
        goto cleanup;
    }
    video_stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video_stream < 0) {
        fprintf(stderr, "input has no video stream\n");
        goto cleanup;
    }

    const AVCodec *codec = avcodec_find_decoder(format->streams[video_stream]->codecpar->codec_id);
    decoder = avcodec_alloc_context3(codec);
    if (!codec || !decoder ||
        avcodec_parameters_to_context(decoder, format->streams[video_stream]->codecpar) < 0 ||
        av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, drm_device, NULL, 0) < 0) {
        fprintf(stderr, "could not initialize VAAPI decoder\n");
        goto cleanup;
    }
    decoder->hw_device_ctx = av_buffer_ref(device);
    decoder->get_format = get_vaapi_format;
    if (!decoder->hw_device_ctx || avcodec_open2(decoder, codec, NULL) < 0) {
        fprintf(stderr, "could not open VAAPI decoder\n");
        goto cleanup;
    }

    AVHWDeviceContext *device_context = (AVHWDeviceContext *)device->data;
    AVVAAPIDeviceContext *vaapi_context = (AVVAAPIDeviceContext *)device_context->hwctx;
    packet = av_packet_alloc();
    if (!packet) {
        fprintf(stderr, "could not allocate packet\n");
        goto cleanup;
    }

    while ((read_status = av_read_frame(format, packet)) >= 0) {
        if (packet->stream_index == video_stream) {
            int status = avcodec_send_packet(decoder, packet);
            if (status == AVERROR(EAGAIN)) {
                if (receive_frames(decoder, vaapi_context->display,
                                   &decoded_frames, &sync_timeouts) < 0)
                    goto cleanup;
                status = avcodec_send_packet(decoder, packet);
            }
            if (status < 0 || receive_frames(decoder, vaapi_context->display,
                                             &decoded_frames,
                                             &sync_timeouts) < 0) {
                fprintf(stderr, "hardware decode or PRIME validation failed\n");
                goto cleanup;
            }
        }
        av_packet_unref(packet);
    }

    if (read_status != AVERROR_EOF) {
        fprintf(stderr, "input read failed: %d\n", read_status);
        goto cleanup;
    }

    int flush_status = avcodec_send_packet(decoder, NULL);
    if (flush_status == AVERROR(EAGAIN)) {
        if (receive_frames(decoder, vaapi_context->display, &decoded_frames,
                           &sync_timeouts) < 0)
            goto cleanup;
        flush_status = avcodec_send_packet(decoder, NULL);
    }
    if (flush_status < 0 && flush_status != AVERROR_EOF) {
        fprintf(stderr, "decoder flush failed: %d\n", flush_status);
        goto cleanup;
    }
    if (receive_frames(decoder, vaapi_context->display, &decoded_frames,
                       &sync_timeouts) < 0)
        goto cleanup;

    if (decoded_frames > 0) {
        printf("validated %llu decoded %dx%d frames with timed sync, PRIME exports, and DRM buffer-handle acquire/release (%llu timeouts retried)\n",
               decoded_frames, decoder->width, decoder->height, sync_timeouts);
        result = 0;
    } else {
        fprintf(stderr, "decoder produced no VAAPI frame\n");
    }

cleanup:
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    av_buffer_unref(&device);
    avformat_close_input(&format);
    return result;
}
