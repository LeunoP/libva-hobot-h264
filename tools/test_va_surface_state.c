#include "../src/hobot_drv_video.c"
#include <errno.h>
#include <sys/wait.h>

static int mock_dequeue_result;
static int mock_dequeue_output_calls;
static int mock_dequeue_output_invalid;
static int mock_err_mb;
static int mock_decode_result = HOBOT_DECODE_RESULT_SUCCESS;
static int recycled_output_count;
static int mock_initialize_result;
static int mock_initialize_calls;
static int mock_configure_result;
static int mock_configure_calls;
static mc_h264_profile_t mock_initialized_h264_profile;
static int mock_initialized_h264_gop_preset_idx;
static int mock_start_result;
static int mock_start_calls;
static int mock_stop_result;
static int mock_release_result;
static int mock_stop_calls;
static int mock_release_calls;
static int mock_queue_input_result;
static int mock_queue_input_calls;
static int mock_input_listener_result;
static int mock_suppress_input_callback;
static media_codec_callback_t mock_input_listener;
static hb_ptr mock_input_listener_userdata;
static int mock_rate_control_result;
static int mock_rate_control_calls;
static int mock_vui_get_result;
static int mock_vui_get_calls;
static int mock_vui_set_result;
static int mock_vui_set_calls;
static mc_video_vui_params_t mock_vui_config;
static mc_video_vui_params_t mock_vui_set_config;
static int mock_entropy_set_result;
static int mock_entropy_set_calls;
static mc_h264_entropy_params_t mock_entropy_set_config;
static int mock_transform_set_result;
static int mock_transform_set_calls;
static mc_video_transform_params_t mock_transform_set_config;
static int mock_sao_set_result;
static int mock_sao_set_calls;
static int mock_sao_set_before_configure;
static mc_h265_sao_params_t mock_sao_set_config;
static unsigned int mock_jpeg_quality_factor;
static int mock_jpeg_get_result;
static int mock_jpeg_get_calls;
static int mock_jpeg_set_result;
static int mock_jpeg_set_calls;
static unsigned int mock_jpeg_current_quality;
static unsigned int mock_jpeg_restart_interval;
static uint8_t mock_jpeg_luma_quant_table[64];
static uint8_t mock_jpeg_chroma_quant_table[64];
static int mock_idr_result;
static int mock_idr_calls;
static int mock_dequeue_input_result;
static int mock_dequeue_input_null;
static int mock_dequeue_input_calls;
static int mock_encoder_io;
static unsigned int mock_encoder_input_size = 96;
static int mock_encoder_input_stride = 8;
static int mock_encoder_input_vstride = 8;
static unsigned int mock_encoded_size;
static int mock_queue_output_calls;
static int mock_queue_output_result;
static int mock_queue_output_wait_timeout;
static int mock_mem_flush_result;
static int mock_mem_invalidate_result;
static int mock_mem_flush_calls;
static int mock_mem_invalidate_calls;
static int mock_mem_flush_fd;
static uint64_t mock_mem_flush_offset;
static uint64_t mock_mem_flush_size;
static int mock_mem_invalidate_fd;
static uint64_t mock_mem_invalidate_offset;
static uint64_t mock_mem_invalidate_size;
static int mock_mem_module_open_result;
static int mock_mem_module_open_calls;
static int mock_mem_module_close_result;
static int mock_mem_module_close_calls;
static int mock_mem_free_result;
static int mock_mem_free_calls;
static int mock_mem_free_fd;
static int mock_mem_get_graph_buf_result;
static int mock_mem_get_graph_buf_calls;
static int mock_mem_get_graph_buf_fd;
static int mock_mem_import_graph_buf_result;
static int mock_mem_import_graph_buf_calls;
static int mock_mem_import_bad_metadata;
static int mock_mem_import_next_fd = 200;
static hb_mem_graphic_buf_t mock_external_graph_buf;
static int mock_graph_alloc_enabled;
static int mock_graph_alloc_calls;
static media_codec_context_t *mock_last_output_context;
static uint8_t mock_frame[64];
static uint8_t mock_input[64];
static uint8_t mock_encoder_y[64];
static uint8_t mock_encoder_uv[32];
static uint8_t mock_encoded_output[64];
static uint8_t mock_graph_data[8 * 1024 * 1024];
static pthread_mutex_t mock_dequeue_gate_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mock_dequeue_gate_cond = PTHREAD_COND_INITIALIZER;
static int mock_dequeue_block_enabled;
static int mock_dequeue_respect_timeout;
static int mock_dequeue_entered;
static int mock_dequeue_release;

typedef struct {
    VADriverContextP ctx;
    int failed;
} BufferStressWorker;

typedef struct {
    VADriverContextP ctx;
    int operation;
    VASurfaceID surface;
    VABufferID buffer;
    VAContextID context;
    VAStatus status;
    VASurfaceStatus surface_status;
    uint64_t timeout_ns;
    int started;
    int completed;
} SyncIsolationWorker;

hb_s32 hb_mm_mc_queue_input_buffer(media_codec_context_t *context,
                                    media_codec_buffer_t *buffer,
                                    hb_s32 timeout)
{
    (void)context;
    (void)buffer;
    (void)timeout;
    mock_queue_input_calls++;
    if (mock_queue_input_result == 0 && !mock_suppress_input_callback &&
        mock_input_listener.on_input_buffer_consumed &&
        buffer && buffer->user_ptr)
        mock_input_listener.on_input_buffer_consumed(mock_input_listener_userdata,
                                                      buffer);
    return mock_queue_input_result;
}

hb_s32 hb_mm_mc_set_input_buffer_listener(media_codec_context_t *context,
                                           const media_codec_callback_t *callback,
                                           hb_ptr userdata)
{
    (void)context;
    if (mock_input_listener_result != 0)
        return mock_input_listener_result;
    if (!callback)
        return -1;
    mock_input_listener = *callback;
    mock_input_listener_userdata = userdata;
    return 0;
}

hb_s32 hb_mm_mc_set_rate_control_config(media_codec_context_t *context,
                                         const mc_rate_control_params_t *params)
{
    (void)context;
    mock_rate_control_calls++;
    if (params && params->mode == MC_AV_RC_MODE_MJPEGFIXQP)
        mock_jpeg_quality_factor = params->mjpeg_fixqp_params.quality_factor;
    return mock_rate_control_result;
}

hb_s32 hb_mm_mc_get_vui_config(media_codec_context_t *context,
                                mc_video_vui_params_t *params)
{
    (void)context;
    mock_vui_get_calls++;
    if (mock_vui_get_result != 0)
        return mock_vui_get_result;
    *params = mock_vui_config;
    return 0;
}

hb_s32 hb_mm_mc_set_vui_config(media_codec_context_t *context,
                                const mc_video_vui_params_t *params)
{
    (void)context;
    mock_vui_set_calls++;
    mock_vui_set_config = *params;
    return mock_vui_set_result;
}

hb_s32 hb_mm_mc_get_jpeg_config(media_codec_context_t *context,
                                 mc_jpeg_enc_params_t *params)
{
    (void)context;
    mock_jpeg_get_calls++;
    if (mock_jpeg_get_result != 0)
        return mock_jpeg_get_result;
    memset(params, 0, sizeof(*params));
    params->quality_factor = mock_jpeg_current_quality;
    params->restart_interval = mock_jpeg_restart_interval;
    memcpy(params->luma_quant_table, mock_jpeg_luma_quant_table,
           sizeof(mock_jpeg_luma_quant_table));
    memcpy(params->chroma_quant_table, mock_jpeg_chroma_quant_table,
           sizeof(mock_jpeg_chroma_quant_table));
    return 0;
}

hb_s32 hb_mm_mc_set_jpeg_config(media_codec_context_t *context,
                                 const mc_jpeg_enc_params_t *params)
{
    (void)context;
    mock_jpeg_set_calls++;
    mock_jpeg_quality_factor = params->quality_factor;
    if (mock_jpeg_set_result == 0) {
        mock_jpeg_current_quality = params->quality_factor;
        mock_jpeg_restart_interval = params->restart_interval;
        memcpy(mock_jpeg_luma_quant_table, params->luma_quant_table,
               sizeof(mock_jpeg_luma_quant_table));
        memcpy(mock_jpeg_chroma_quant_table, params->chroma_quant_table,
               sizeof(mock_jpeg_chroma_quant_table));
    }
    return mock_jpeg_set_result;
}

hb_s32 hb_mm_mc_request_idr_frame(media_codec_context_t *context)
{
    (void)context;
    mock_idr_calls++;
    return mock_idr_result;
}

hb_s32 hb_mm_mc_dequeue_input_buffer(media_codec_context_t *context,
                                     media_codec_buffer_t *buffer,
                                     hb_s32 timeout)
{
    (void)context;
    (void)timeout;
    mock_dequeue_input_calls++;
    if (mock_dequeue_input_result != 0)
        return mock_dequeue_input_result;
    memset(buffer, 0, sizeof(*buffer));
    if (mock_encoder_io) {
        buffer->vframe_buf.vir_ptr[0] = mock_encoder_y;
        buffer->vframe_buf.vir_ptr[1] = mock_encoder_uv;
        buffer->vframe_buf.size = mock_encoder_input_size;
        buffer->vframe_buf.stride = mock_encoder_input_stride;
        buffer->vframe_buf.vstride = mock_encoder_input_vstride;
    } else if (!mock_dequeue_input_null) {
        buffer->vstream_buf.vir_ptr = mock_input;
    }
    buffer->vstream_buf.size = sizeof(mock_input);
    return 0;
}

hb_s32 hb_mm_mc_initialize(media_codec_context_t *context)
{
    mock_initialize_calls++;
    if (context && context->encoder && context->codec_id == MEDIA_CODEC_ID_H264) {
        mock_initialized_h264_profile =
            context->video_enc_params.h264_enc_config.h264_profile;
        mock_initialized_h264_gop_preset_idx =
            context->video_enc_params.gop_params.gop_preset_idx;
    }
    return mock_initialize_result;
}

hb_s32 hb_mm_mc_configure(media_codec_context_t *context)
{
    (void)context;
    mock_configure_calls++;
    return mock_configure_result;
}

hb_s32 hb_mm_mc_set_entropy_config(media_codec_context_t *context,
                                   const mc_h264_entropy_params_t *params)
{
    (void)context;
    mock_entropy_set_calls++;
    if (params)
        mock_entropy_set_config = *params;
    return mock_entropy_set_result;
}

hb_s32 hb_mm_mc_set_transform_config(media_codec_context_t *context,
                                     const mc_video_transform_params_t *params)
{
    (void)context;
    mock_transform_set_calls++;
    if (params)
        mock_transform_set_config = *params;
    return mock_transform_set_result;
}

hb_s32 hb_mm_mc_set_sao_config(media_codec_context_t *context,
                               const mc_h265_sao_params_t *params)
{
    (void)context;
    mock_sao_set_calls++;
    mock_sao_set_before_configure = mock_configure_calls == 0;
    if (params)
        mock_sao_set_config = *params;
    return mock_sao_set_result;
}

hb_s32 hb_mm_mc_start(media_codec_context_t *context,
                       const mc_av_codec_startup_params_t *info)
{
    (void)context;
    (void)info;
    mock_start_calls++;
    return mock_start_result;
}

hb_s32 hb_mm_mc_stop(media_codec_context_t *context)
{
    (void)context;
    mock_stop_calls++;
    return mock_stop_result;
}

hb_s32 hb_mm_mc_release(media_codec_context_t *context)
{
    (void)context;
    mock_release_calls++;
    return mock_release_result;
}

int32_t hb_mem_module_open(void)
{
    mock_mem_module_open_calls++;
    return mock_mem_module_open_result;
}

int32_t hb_mem_module_close(void)
{
    mock_mem_module_close_calls++;
    return mock_mem_module_close_result;
}

int32_t hb_mem_flush_buf(int32_t fd, uint64_t offset, uint64_t size)
{
    mock_mem_flush_calls++;
    mock_mem_flush_fd = fd;
    mock_mem_flush_offset = offset;
    mock_mem_flush_size = size;
    return mock_mem_flush_result;
}

int32_t hb_mem_invalidate_buf(int32_t fd, uint64_t offset, uint64_t size)
{
    mock_mem_invalidate_calls++;
    mock_mem_invalidate_fd = fd;
    mock_mem_invalidate_offset = offset;
    mock_mem_invalidate_size = size;
    return mock_mem_invalidate_result;
}

int32_t hb_mem_alloc_graph_buf(int32_t width, int32_t height, int32_t format,
                               int64_t flags, int32_t stride, int32_t vstride,
                               hb_mem_graphic_buf_t *buffer)
{
    (void)flags;
    mock_graph_alloc_calls++;
    if (!mock_graph_alloc_enabled || !buffer || width <= 0 || height <= 0 ||
        stride < width || vstride < height || format != MEM_PIX_FMT_NV12)
        return -1;
    uint64_t y_size = (uint64_t)stride * (uint32_t)vstride;
    uint64_t uv_size = (uint64_t)stride * ((uint32_t)vstride / 2u);
    if (y_size + uv_size > sizeof(mock_graph_data))
        return -1;
    memset(buffer, 0, sizeof(*buffer));
    for (size_t i = 0; i < MAX_GRAPHIC_BUF_COMP; i++)
        buffer->fd[i] = -1;
    buffer->fd[0] = 99;
    buffer->stride = stride;
    buffer->vstride = vstride;
    buffer->size[0] = y_size;
    buffer->size[1] = uv_size;
    buffer->offset[1] = y_size;
    buffer->is_contig = 1;
    buffer->virt_addr[0] = mock_graph_data;
    buffer->virt_addr[1] = mock_graph_data + y_size;
    buffer->phys_addr[0] = 0x100000;
    buffer->phys_addr[1] = 0x100000 + y_size;
    return 0;
}

int32_t hb_mem_get_graph_buf(int32_t fd, hb_mem_graphic_buf_t *buffer)
{
    mock_mem_get_graph_buf_calls++;
    mock_mem_get_graph_buf_fd = fd;
    if (mock_mem_get_graph_buf_result != 0 || fd < 0 || !buffer)
        return -1;
    *buffer = mock_external_graph_buf;
    buffer->fd[0] = fd;
    return 0;
}

int32_t hb_mem_import_graph_buf(hb_mem_graphic_buf_t *buffer,
                                hb_mem_graphic_buf_t *out_buffer)
{
    mock_mem_import_graph_buf_calls++;
    if (mock_mem_import_graph_buf_result != 0 || !buffer || !out_buffer)
        return -1;
    *out_buffer = *buffer;
    out_buffer->fd[0] = mock_mem_import_next_fd++;
    out_buffer->fd[1] = -1;
    if (mock_mem_import_bad_metadata)
        out_buffer->plane_cnt = 1;
    return 0;
}

int32_t hb_mem_free_buf(int32_t fd)
{
    mock_mem_free_calls++;
    mock_mem_free_fd = fd;
    return mock_mem_free_result;
}

int unlink(const char *path)
{
    (void)path;
    return 0;
}

hb_s32 hb_mm_mc_dequeue_output_buffer(media_codec_context_t *context,
                                       media_codec_buffer_t *buffer,
                                       media_codec_output_buffer_info_t *info,
                                       hb_s32 timeout)
{
    (void)context;
    int timed_out = 0;
    pthread_mutex_lock(&mock_dequeue_gate_mutex);
    if (mock_dequeue_block_enabled) {
        mock_dequeue_entered = 1;
        pthread_cond_broadcast(&mock_dequeue_gate_cond);
        if (mock_dequeue_respect_timeout && timeout >= 0) {
            struct timespec deadline;
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += timeout / 1000;
            deadline.tv_nsec += (long)(timeout % 1000) * 1000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }
            int wait_status = 0;
            while (!mock_dequeue_release && wait_status != ETIMEDOUT)
                wait_status = pthread_cond_timedwait(&mock_dequeue_gate_cond,
                                                     &mock_dequeue_gate_mutex,
                                                     &deadline);
            timed_out = !mock_dequeue_release && wait_status == ETIMEDOUT;
        } else {
            while (!mock_dequeue_release)
                pthread_cond_wait(&mock_dequeue_gate_cond,
                                  &mock_dequeue_gate_mutex);
        }
    }
    pthread_mutex_unlock(&mock_dequeue_gate_mutex);
    mock_dequeue_output_calls++;
    if (timed_out)
        return HB_MEDIA_ERR_WAIT_TIMEOUT;
    if (mock_dequeue_result != 0)
        return mock_dequeue_result;

    memset(buffer, 0, sizeof(*buffer));
    memset(info, 0, sizeof(*info));
    if (mock_encoder_io) {
        buffer->vstream_buf.vir_ptr = mock_encoded_output;
        buffer->vstream_buf.size = mock_encoded_size;
        return 0;
    }
    buffer->vframe_buf.phy_ptr[0] = 1;
    buffer->vframe_buf.vir_ptr[0] = mock_frame;
    buffer->vframe_buf.fd[0] = 7;
    buffer->vframe_buf.size = sizeof(mock_frame);
    if (mock_dequeue_output_invalid) {
        buffer->vframe_buf.vir_ptr[0] = NULL;
        buffer->vframe_buf.fd[0] = -1;
    }
    info->video_frame_info.decode_result = mock_decode_result;
    info->video_frame_info.err_mb_in_frame_display = mock_err_mb;
    info->video_frame_info.total_mb_in_frame_display = 100;
    return 0;
}

hb_s32 hb_mm_mc_queue_output_buffer(media_codec_context_t *context,
                                     media_codec_buffer_t *buffer,
                                     hb_s32 timeout)
{
    (void)buffer;
    mock_queue_output_calls++;
    mock_last_output_context = context;
    if (mock_queue_output_wait_timeout) {
        struct timespec pause = {
            .tv_sec = timeout / 1000,
            .tv_nsec = (long)(timeout % 1000) * 1000000L,
        };
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {
            continue;
        }
        return HB_MEDIA_ERR_WAIT_TIMEOUT;
    }
    if (mock_queue_output_result == 0)
        recycled_output_count++;
    return mock_queue_output_result;
}

static int test_decoder_input_queue_failure_preserves_ownership(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->current_render_target = 2;
    hctx->decode_picture_active = 1;
    hctx->dec_in_buf_valid = 1;
    hctx->dec_in_buf_offset = 17;
    hctx->submitted_surfaces[hctx->sub_tail++] = 2;
    drv.surfaces[2].allocated = 1;
    drv.surfaces[2].context_id = 1;
    drv.surfaces[2].decode_pending = 1;

    mock_queue_input_result = -1;
    mock_queue_input_calls = 0;
    VAStatus status = hobot_vaEndPicture(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || !hctx->decode_failed ||
        !hctx->dec_in_buf_valid || hctx->dec_in_buf_offset != 17 ||
        hctx->dec_in_buf.vstream_buf.size != 17 ||
        drv.surfaces[2].decode_pending || !drv.surfaces[2].decode_error ||
        hctx->sub_head != hctx->sub_tail || mock_queue_input_calls != 1) {
        fprintf(stderr, "input queue failure ownership regression: status=%d failed=%d valid=%d offset=%d size=%u pending=%d surface_error=%d queued=%u/%u calls=%d\n",
                status, hctx->decode_failed, hctx->dec_in_buf_valid,
                hctx->dec_in_buf_offset, hctx->dec_in_buf.vstream_buf.size,
                drv.surfaces[2].decode_pending, drv.surfaces[2].decode_error,
                hctx->sub_head, hctx->sub_tail, mock_queue_input_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    status = hobot_vaBeginPicture(&va_ctx, 1, 2);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || mock_queue_input_calls != 1 ||
        !hctx->dec_in_buf_valid || hctx->dec_in_buf_offset != 17) {
        fprintf(stderr, "faulted decoder accepted another picture: status=%d calls=%d valid=%d offset=%d\n",
                status, mock_queue_input_calls, hctx->dec_in_buf_valid,
                hctx->dec_in_buf_offset);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    drv.surfaces[3].allocated = 1;
    drv.surfaces[3].context_id = 1;
    drv.surfaces[3].decode_pending = 1;
    VASurfaceID pending_surface = 3;
    status = hobot_vaDestroySurfaces(&va_ctx, &pending_surface, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || !drv.surfaces[3].allocated ||
        !drv.surfaces[3].decode_pending || drv.surfaces[3].context_id != 1) {
        fprintf(stderr, "pending surface was released: status=%d allocated=%d pending=%d context=%u\n",
                status, drv.surfaces[3].allocated, drv.surfaces[3].decode_pending,
                drv.surfaces[3].context_id);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    pthread_mutex_destroy(&drv.mutex);
    mock_queue_input_result = 0;
    return 1;
}

static int test_surface_recycles_through_original_context(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    drv.contexts[1].allocated = 1;
    drv.contexts[1].vpu_running = 1;
    drv.contexts[1].current_render_target = VA_INVALID_SURFACE;
    drv.contexts[2].allocated = 1;
    drv.contexts[2].vpu_running = 1;
    HobotSurface *surf = &drv.surfaces[4];
    surf->allocated = 1;
    surf->context_id = 1;
    surf->output_context_id = 1;
    surf->has_decoded_frame = 1;
    surf->dma_fd = 9;
    surf->vpu_out_buf.vframe_buf.phy_ptr[0] = 1;
    surf->vpu_out_buf.vframe_buf.size = sizeof(mock_frame);

    mock_queue_output_calls = 0;
    mock_last_output_context = NULL;
    VAStatus status = hobot_vaBeginPicture(&va_ctx, 2, 4);
    if (status != VA_STATUS_SUCCESS || mock_queue_output_calls != 1 ||
        mock_last_output_context != &drv.contexts[1].vpu_ctx ||
        surf->has_decoded_frame || surf->context_id != 2 || !surf->decode_pending) {
        fprintf(stderr, "cross-context surface recycle failed: status=%d calls=%d owner=%u old_ctx=%p got_ctx=%p decoded=%d pending=%d\n",
                status, mock_queue_output_calls, surf->context_id,
                (void *)&drv.contexts[1].vpu_ctx, (void *)mock_last_output_context,
                surf->has_decoded_frame, surf->decode_pending);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    int calls_before = mock_queue_output_calls;
    status = hobot_vaBeginPicture(&va_ctx, 1, 4);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED ||
        mock_queue_output_calls != calls_before || surf->context_id != 2 ||
        !surf->decode_pending || drv.contexts[1].current_render_target != VA_INVALID_SURFACE) {
        fprintf(stderr, "pending surface was reused: status=%d calls=%d/%d owner=%u pending=%d target=%u\n",
                status, mock_queue_output_calls, calls_before,
                surf->context_id, surf->decode_pending,
                drv.contexts[1].current_render_target);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    pthread_mutex_destroy(&drv.mutex);
    recycled_output_count = 0;
    return 1;
}

static int test_encoder_surface_keeps_decoder_buffer_owner(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    drv.contexts[1].allocated = 1;
    drv.contexts[1].vpu_running = 1;
    drv.contexts[2].allocated = 1;
    drv.contexts[2].vpu_running = 1;
    drv.contexts[2].is_encoder = 1;
    HobotSurface *surf = &drv.surfaces[4];
    surf->allocated = 1;
    surf->context_id = 1;
    surf->output_context_id = 1;
    surf->has_decoded_frame = 1;
    surf->dma_fd = 9;
    surf->vpu_out_buf.vframe_buf.phy_ptr[0] = 1;
    surf->vpu_out_buf.vframe_buf.size = sizeof(mock_frame);

    mock_queue_output_calls = 0;
    mock_last_output_context = NULL;
    VAStatus status = hobot_vaBeginPicture(&va_ctx, 2, 4);
    if (status != VA_STATUS_SUCCESS || surf->context_id != 2 ||
        surf->output_context_id != 1 || !surf->has_decoded_frame ||
        mock_queue_output_calls != 0) {
        fprintf(stderr, "encoder begin overwrote decoder buffer owner: status=%d ctx=%u output_ctx=%u frame=%d queues=%d\n",
                status, surf->context_id, surf->output_context_id,
                surf->has_decoded_frame, mock_queue_output_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    status = hobot_vaDestroyContext(&va_ctx, 2);
    if (status != VA_STATUS_SUCCESS || surf->context_id != 0 ||
        surf->output_context_id != 1 || !surf->has_decoded_frame ||
        mock_queue_output_calls != 0) {
        fprintf(stderr, "encoder teardown cleared decoder-owned frame: status=%d ctx=%u output_ctx=%u frame=%d queues=%d\n",
                status, surf->context_id, surf->output_context_id,
                surf->has_decoded_frame, mock_queue_output_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    status = hobot_vaDestroyContext(&va_ctx, 1);
    if (status != VA_STATUS_SUCCESS || surf->output_context_id != 0 ||
        surf->has_decoded_frame || mock_queue_output_calls != 1 ||
        mock_last_output_context != &drv.contexts[1].vpu_ctx) {
        fprintf(stderr, "decoder teardown did not reclaim owned frame: status=%d ctx=%u output_ctx=%u frame=%d queues=%d\n",
                status, surf->context_id, surf->output_context_id,
                surf->has_decoded_frame, mock_queue_output_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static int test_encoder_parameter_buffers_reject_truncation(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    uint8_t short_data[sizeof(VAEncSequenceParameterBufferHEVC)] = {0};
    VABufferID buffer_id = 1;
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->is_encoder = 1;
    hctx->encoder_picture_active = 1;
    drv.buffers[1].allocated = 1;
    drv.buffers[1].data = short_data;

#define EXPECT_TRUNCATED_ENCODER_BUFFER(profile_value, type_value, size_value) \
    do { \
        hctx->profile = (profile_value); \
        drv.buffers[1].type = (type_value); \
        drv.buffers[1].size = (size_value); \
        VAStatus got = hobot_vaRenderPicture(&va_ctx, 1, &buffer_id, 1); \
        if (got != VA_STATUS_ERROR_INVALID_PARAMETER) { \
            fprintf(stderr, "truncated encoder buffer accepted: profile=%d type=%d size=%u status=%d\n", \
                    (int)(profile_value), (int)(type_value), (unsigned int)(size_value), got); \
            pthread_mutex_destroy(&drv.mutex); \
            return 0; \
        } \
    } while (0)

    EXPECT_TRUNCATED_ENCODER_BUFFER(VAProfileH264Main, VAEncPictureParameterBufferType,
                                    sizeof(VAEncPictureParameterBufferH264) - 1);
    EXPECT_TRUNCATED_ENCODER_BUFFER(VAProfileH264Main, VAEncSequenceParameterBufferType,
                                    sizeof(VAEncSequenceParameterBufferH264) - 1);
    EXPECT_TRUNCATED_ENCODER_BUFFER(VAProfileHEVCMain, VAEncPictureParameterBufferType,
                                    sizeof(VAEncPictureParameterBufferHEVC) - 1);
    EXPECT_TRUNCATED_ENCODER_BUFFER(VAProfileHEVCMain, VAEncSequenceParameterBufferType,
                                    sizeof(VAEncSequenceParameterBufferHEVC) - 1);
    EXPECT_TRUNCATED_ENCODER_BUFFER(VAProfileJPEGBaseline, VAEncPictureParameterBufferType,
                                    sizeof(VAEncPictureParameterBufferJPEG) - 1);

    VAEncMiscParameterBuffer *misc = (VAEncMiscParameterBuffer *)short_data;
    misc->type = VAEncMiscParameterTypeRateControl;
    EXPECT_TRUNCATED_ENCODER_BUFFER(VAProfileH264Main, VAEncMiscParameterBufferType,
                                    offsetof(VAEncMiscParameterBuffer, data));
    misc->type = VAEncMiscParameterTypeFrameRate;
    EXPECT_TRUNCATED_ENCODER_BUFFER(VAProfileH264Main, VAEncMiscParameterBufferType,
                                    offsetof(VAEncMiscParameterBuffer, data));
    misc->type = VAEncMiscParameterTypeHRD;
    EXPECT_TRUNCATED_ENCODER_BUFFER(VAProfileH264Main, VAEncMiscParameterBufferType,
                                    offsetof(VAEncMiscParameterBuffer, data));

#undef EXPECT_TRUNCATED_ENCODER_BUFFER
    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static int test_fixed_record_parameters_reject_array_shape(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    VAEncPictureParameterBufferH264 picture = {0};
    uint8_t coded_data[16] = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    for (size_t i = 0; i < sizeof(picture.ReferenceFrames) /
                            sizeof(picture.ReferenceFrames[0]); i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    picture.CurrPic.picture_id = 1;
    drv.surfaces[1].allocated = 1;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->is_encoder = 1;
    hctx->encoder_picture_active = 1;
    hctx->profile = VAProfileH264Main;
    picture.coded_buf = 2;
    drv.buffers[1] = (HobotBuffer){
        .id = 1, .allocated = 1, .type = VAEncPictureParameterBufferType,
        .size = sizeof(picture), .capacity = sizeof(picture),
        .element_size = sizeof(picture) / 2, .num_elements = 2,
        .data = &picture
    };
    drv.buffers[2] = (HobotBuffer){
        .id = 2, .allocated = 1, .type = VAEncCodedBufferType,
        .size = sizeof(coded_data), .capacity = sizeof(coded_data),
        .data = coded_data
    };

    VABufferID buffer_id = 1;
    VAStatus status = hobot_vaRenderPicture(&va_ctx, 1, &buffer_id, 1);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER || hctx->enc_coded_buf != 0) {
        fprintf(stderr, "multi-element H.264 picture parameter was accepted: status=%d coded=%u\n",
                status, hctx->enc_coded_buf);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    drv.buffers[1].element_size = sizeof(picture);
    drv.buffers[1].num_elements = 1;
    status = hobot_vaRenderPicture(&va_ctx, 1, &buffer_id, 1);
    int passed = status == VA_STATUS_SUCCESS && hctx->enc_coded_buf == 2;
    if (!passed)
        fprintf(stderr, "valid single-record H.264 picture parameter was rejected: status=%d coded=%u\n",
                status, hctx->enc_coded_buf);

    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_hevc_main_subset_parameter_validation(void)
{
    if (!hobot_profile_resolution_supported(VAProfileHEVCMain,
                                             VAEntrypointVLD, 8192, 4096) ||
        hobot_profile_resolution_supported(VAProfileHEVCMain,
                                            VAEntrypointVLD, 8194, 4096) ||
        hobot_profile_resolution_supported(VAProfileHEVCMain,
                                            VAEntrypointVLD, 8192, 4098) ||
        !hobot_profile_resolution_supported(VAProfileHEVCMain,
                                             VAEntrypointEncSlice, 3840, 2160) ||
        hobot_profile_resolution_supported(VAProfileHEVCMain,
                                            VAEntrypointEncSlice, 4096, 2160) ||
        !hobot_profile_resolution_supported(VAProfileH264High,
                                             VAEntrypointVLD, 4096, 4096)) {
        fprintf(stderr, "codec-specific picture resolution bounds are inaccurate\n");
        return 0;
    }

    VAPictureParameterBufferHEVC picture = {0};
    picture.pic_width_in_luma_samples = 640;
    picture.pic_height_in_luma_samples = 360;
    picture.pic_fields.bits.chroma_format_idc = 1;
    picture.sps_max_dec_pic_buffering_minus1 = 3;
    picture.log2_max_pic_order_cnt_lsb_minus4 = 4;
    picture.log2_diff_max_min_luma_coding_block_size = 3;
    picture.log2_diff_max_min_transform_block_size = 3;
    picture.st_rps_bits = 12;
    if (!hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "valid HEVC Main picture parameters rejected\n");
        return 0;
    }

    picture.num_short_term_ref_pic_sets = 1;
    picture.st_rps_bits = 3;
    if (!hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "single-set HEVC SPS RPS parameters were rejected\n");
        return 0;
    }
    picture.st_rps_bits = 2049;
    if (hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "oversized single-set HEVC RPS syntax was accepted\n");
        return 0;
    }
    picture.num_short_term_ref_pic_sets = 2;
    picture.st_rps_bits = 0;
    if (!hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "validated two-set HEVC RPS profile was rejected\n");
        return 0;
    }
    picture.num_short_term_ref_pic_sets = 14;
    picture.slice_parsing_fields.bits.IdrPicFlag = 1;
    picture.slice_parsing_fields.bits.RapPicFlag = 1;
    picture.slice_parsing_fields.bits.IntraPicFlag = 1;
    if (!hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "IDR picture with unused additional SPS RPS sets was rejected\n");
        return 0;
    }
    picture.slice_parsing_fields.bits.IdrPicFlag = 0;
    if (!hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "non-IDR picture with additional SPS RPS sets was rejected before slice validation\n");
        return 0;
    }
    picture.num_short_term_ref_pic_sets = 2;
    picture.slice_parsing_fields.bits.IdrPicFlag = 0;
    picture.slice_parsing_fields.bits.RapPicFlag = 0;
    picture.slice_parsing_fields.bits.IntraPicFlag = 0;
    picture.st_rps_bits = 1;
    if (!hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "HEVC bounded two-set inline RPS was rejected before slice validation\n");
        return 0;
    }
    picture.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag = 1;
    picture.st_rps_bits = 6;
    if (!hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "HEVC two-set temporal-MVP inline RPS was rejected\n");
        return 0;
    }
    picture.st_rps_bits = 2049;
    if (hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "oversized HEVC two-set inline RPS was accepted\n");
        return 0;
    }
    picture.num_short_term_ref_pic_sets = 0;
    picture.st_rps_bits = 12;
    picture.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag = 0;

    picture.bit_depth_luma_minus8 = 2;
    if (hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "HEVC Main accepted non-8-bit luma\n");
        return 0;
    }
    picture.bit_depth_luma_minus8 = 0;
    picture.log2_diff_max_min_luma_coding_block_size = 0;
    if (hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "HEVC Main accepted an 8x8 CTB size\n");
        return 0;
    }
    picture.log2_diff_max_min_luma_coding_block_size = 1;
    if (!hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "HEVC Main rejected the minimum 16x16 CTB size\n");
        return 0;
    }
    picture.log2_diff_max_min_luma_coding_block_size = 3;
    picture.pic_fields.bits.tiles_enabled_flag = 1;
    picture.pic_fields.bits.loop_filter_across_tiles_enabled_flag = 1;
    picture.num_tile_columns_minus1 = 1;
    picture.num_tile_rows_minus1 = 1;
    picture.column_width_minus1[0] = 4;
    picture.row_height_minus1[0] = 2;
    if (!hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "valid uniform HEVC tile layout was rejected\n");
        return 0;
    }
    picture.column_width_minus1[0] = 2;
    if (hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "HEVC Main accepted a tile column narrower than 256 samples\n");
        return 0;
    }
    picture.column_width_minus1[0] = 7;
    picture.row_height_minus1[0] = 0;
    picture.log2_diff_max_min_luma_coding_block_size = 2;
    if (hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "HEVC Main accepted a tile row shorter than 64 samples\n");
        return 0;
    }
    picture.row_height_minus1[0] = 1;
    if (!hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "HEVC Main rejected tiles at the minimum 256x64 size\n");
        return 0;
    }
    picture.log2_diff_max_min_luma_coding_block_size = 3;
    picture.column_width_minus1[0] = 4;
    picture.row_height_minus1[0] = 2;
    picture.column_width_minus1[0] = 9;
    if (hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "HEVC tile width leaving an empty final tile was accepted\n");
        return 0;
    }
    picture.column_width_minus1[0] = 5;
    picture.row_height_minus1[0] = 3;
    if (!hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "valid non-uniform HEVC tile layout was rejected\n");
        return 0;
    }
    picture.row_height_minus1[0] = 5;
    if (hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "HEVC tile row leaving an empty final tile was accepted\n");
        return 0;
    }
    picture.row_height_minus1[0] = 3;
    picture.column_width_minus1[0] = 9;
    if (hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "HEVC non-uniform tile layout exceeded the CTB grid\n");
        return 0;
    }
    picture.column_width_minus1[0] = 4;
    picture.row_height_minus1[0] = 2;
    picture.num_tile_columns_minus1 = 10;
    if (hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "HEVC tile count larger than CTB columns was accepted\n");
        return 0;
    }
    picture.pic_fields.bits.tiles_enabled_flag = 0;
    picture.pic_fields.bits.loop_filter_across_tiles_enabled_flag = 0;
    picture.num_tile_columns_minus1 = 0;
    picture.num_tile_rows_minus1 = 0;
    picture.column_width_minus1[0] = 0;
    picture.row_height_minus1[0] = 0;
    picture.pic_fields.bits.pcm_enabled_flag = 1;
    picture.pcm_sample_bit_depth_luma_minus1 = 7;
    picture.pcm_sample_bit_depth_chroma_minus1 = 7;
    picture.log2_min_pcm_luma_coding_block_size_minus3 = 0;
    picture.log2_diff_max_min_pcm_luma_coding_block_size = 2;
    if (!hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "valid 8-bit HEVC PCM parameters were rejected\n");
        return 0;
    }
    picture.pcm_sample_bit_depth_luma_minus1 = 8;
    if (hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "HEVC PCM accepted a sample depth outside the 8-bit Main subset\n");
        return 0;
    }
    picture.pcm_sample_bit_depth_luma_minus1 = 7;
    picture.log2_min_luma_coding_block_size_minus3 = 2;
    picture.log2_diff_max_min_luma_coding_block_size = 1;
    if (hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "HEVC PCM accepted a minimum block smaller than the SPS minimum CB\n");
        return 0;
    }
    picture.log2_min_pcm_luma_coding_block_size_minus3 = 2;
    picture.log2_diff_max_min_pcm_luma_coding_block_size = 1;
    if (hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "HEVC PCM accepted a maximum block larger than the capped CTB size\n");
        return 0;
    }
    picture.log2_diff_max_min_pcm_luma_coding_block_size = 0;
    if (!hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "valid HEVC PCM blocks matching the SPS minimum CB were rejected\n");
        return 0;
    }
    picture.pic_fields.bits.pcm_enabled_flag = 0;
    picture.log2_min_luma_coding_block_size_minus3 = 0;
    picture.log2_diff_max_min_luma_coding_block_size = 3;
    picture.pic_fields.bits.entropy_coding_sync_enabled_flag = 1;
    if (!hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "HEVC Main rejected WPP without tiles\n");
        return 0;
    }
    picture.pic_fields.bits.tiles_enabled_flag = 1;
    picture.num_tile_columns_minus1 = 1;
    picture.num_tile_rows_minus1 = 1;
    picture.column_width_minus1[0] = 4;
    picture.row_height_minus1[0] = 2;
    if (hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "HEVC Main accepted WPP together with tiles\n");
        return 0;
    }
    picture.pic_fields.bits.entropy_coding_sync_enabled_flag = 0;
    if (!hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "HEVC Main rejected tiles without WPP\n");
        return 0;
    }
    picture.pic_fields.bits.tiles_enabled_flag = 0;
    picture.num_tile_columns_minus1 = 0;
    picture.num_tile_rows_minus1 = 0;
    picture.column_width_minus1[0] = 0;
    picture.row_height_minus1[0] = 0;
    if (hobot_hevc_picture_parameters_supported(&picture, 65536, 360)) {
        fprintf(stderr, "HEVC Main accepted dimensions wider than VA fields\n");
        return 0;
    }

    VASliceParameterBufferHEVC slice = {0};
    memset(slice.RefPicList, 0xff, sizeof(slice.RefPicList));
    slice.slice_data_size = 128;
    slice.slice_data_byte_offset = 5;
    slice.LongSliceFlags.fields.LastSliceOfPic = 1;
    slice.LongSliceFlags.fields.slice_type = 2;
    if (!hobot_hevc_slice_parameter_fields_supported(&slice)) {
        fprintf(stderr, "valid HEVC slice fields rejected\n");
        return 0;
    }
    slice.LongSliceFlags.fields.dependent_slice_segment_flag = 1;
    if (!hobot_hevc_slice_parameter_fields_supported(&slice)) {
        fprintf(stderr, "valid dependent HEVC slice fields rejected\n");
        return 0;
    }
    slice.LongSliceFlags.fields.dependent_slice_segment_flag = 0;
    slice.num_entry_point_offsets = 1;
    if (!hobot_hevc_slice_parameter_fields_supported(&slice)) {
        fprintf(stderr, "HEVC WPP entry-point fields were rejected\n");
        return 0;
    }

    picture.pic_width_in_luma_samples = 640;
    picture.pic_height_in_luma_samples = 360;
    picture.log2_min_luma_coding_block_size_minus3 = 0;
    picture.log2_diff_max_min_luma_coding_block_size = 2;
    picture.num_short_term_ref_pic_sets = 0;
    VASliceParameterBufferHEVC slices[2] = {{0}};
    slices[0].slice_data_size = 64;
    slices[0].slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    slices[0].LongSliceFlags.fields.slice_type = 2;
    slices[1].slice_segment_address = 120;
    slices[1].slice_data_size = 48;
    slices[1].slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    slices[1].LongSliceFlags.fields.LastSliceOfPic = 1;
    slices[1].LongSliceFlags.fields.slice_type = 2;

    HobotHevcSliceSequence sequence;
    if (!hobot_hevc_slice_sequence_init(&picture, 2, &sequence) ||
        !hobot_hevc_slice_sequence_add(&sequence, &slices[0], 64) ||
        !hobot_hevc_slice_sequence_add(&sequence, &slices[1], 48) ||
        !hobot_hevc_slice_sequence_complete(&sequence)) {
        fprintf(stderr, "valid HEVC two-slice CTU sequence rejected\n");
        return 0;
    }

    picture.pic_fields.bits.entropy_coding_sync_enabled_flag = 1;
    slices[0].LongSliceFlags.fields.LastSliceOfPic = 1;
    slices[0].num_entry_point_offsets = 5;
    if (!hobot_hevc_slice_sequence_init(&picture, 1, &sequence) ||
        !hobot_hevc_slice_sequence_add(&sequence, &slices[0], 64) ||
        !hobot_hevc_slice_sequence_complete(&sequence)) {
        fprintf(stderr, "valid HEVC WPP entry-point count rejected\n");
        return 0;
    }
    slices[0].num_entry_point_offsets = 12;
    if (!hobot_hevc_slice_sequence_init(&picture, 1, &sequence) ||
        hobot_hevc_slice_sequence_add(&sequence, &slices[0], 64)) {
        fprintf(stderr, "HEVC WPP accepted more entry points than CTB rows allow\n");
        return 0;
    }
    picture.pic_fields.bits.entropy_coding_sync_enabled_flag = 0;
    slices[0].num_entry_point_offsets = 1;
    if (!hobot_hevc_slice_sequence_init(&picture, 1, &sequence) ||
        hobot_hevc_slice_sequence_add(&sequence, &slices[0], 64)) {
        fprintf(stderr, "HEVC accepted entry points with WPP disabled\n");
        return 0;
    }
    slices[0].num_entry_point_offsets = 0;

    picture.pic_fields.bits.tiles_enabled_flag = 1;
    picture.num_tile_columns_minus1 = 1;
    picture.num_tile_rows_minus1 = 1;
    picture.column_width_minus1[0] = 9;
    picture.row_height_minus1[0] = 5;
    slices[0].LongSliceFlags.fields.LastSliceOfPic = 1;
    slices[0].num_entry_point_offsets = 3;
    if (!hobot_hevc_slice_sequence_init(&picture, 1, &sequence) ||
        !hobot_hevc_slice_sequence_add(&sequence, &slices[0], 64) ||
        !hobot_hevc_slice_sequence_complete(&sequence)) {
        fprintf(stderr, "valid HEVC uniform 2x2 tile entry points were rejected\n");
        return 0;
    }
    picture.column_width_minus1[0] = 7;
    picture.row_height_minus1[0] = 3;
    if (!hobot_hevc_slice_sequence_init(&picture, 1, &sequence) ||
        !hobot_hevc_slice_sequence_add(&sequence, &slices[0], 64) ||
        !hobot_hevc_slice_sequence_complete(&sequence)) {
        fprintf(stderr, "valid HEVC non-uniform 2x2 tile entry points were rejected\n");
        return 0;
    }
    picture.column_width_minus1[0] = 19;
    if (hobot_hevc_slice_sequence_init(&picture, 1, &sequence)) {
        fprintf(stderr, "invalid HEVC tile geometry reached slice validation\n");
        return 0;
    }
    picture.column_width_minus1[0] = 9;
    picture.row_height_minus1[0] = 5;
    slices[0].num_entry_point_offsets = 2;
    if (!hobot_hevc_slice_sequence_init(&picture, 1, &sequence) ||
        hobot_hevc_slice_sequence_add(&sequence, &slices[0], 64)) {
        fprintf(stderr, "HEVC tile slice accepted a mismatched entry-point count\n");
        return 0;
    }
    slices[0].num_entry_point_offsets = 3;
    slices[0].LongSliceFlags.fields.LastSliceOfPic = 0;
    slices[1].LongSliceFlags.fields.LastSliceOfPic = 1;
    slices[0].num_entry_point_offsets = 0;
    slices[1].num_entry_point_offsets = 0;
    uint64_t tile_slice_entry_points[2] = {UINT64_MAX, UINT64_MAX};
    if (!hobot_hevc_slice_sequence_init(&picture, 2, &sequence) ||
        !hobot_hevc_slice_sequence_add_with_next(
            &sequence, &slices[0], 64, 1,
            slices[1].slice_segment_address, &tile_slice_entry_points[0]) ||
        tile_slice_entry_points[0] != 1 ||
        !hobot_hevc_slice_sequence_add_with_next(
            &sequence, &slices[1], 48, 0, 0,
            &tile_slice_entry_points[1]) ||
        tile_slice_entry_points[1] != 1 ||
        !hobot_hevc_slice_sequence_complete(&sequence)) {
        fprintf(stderr, "valid two-slice HEVC tile-row sequence was rejected\n");
        return 0;
    }
    if (!hobot_hevc_slice_sequence_init(&picture, 2, &sequence) ||
        hobot_hevc_slice_sequence_add_with_next(
            &sequence, &slices[0], 64, 1, 121, NULL)) {
        fprintf(stderr, "HEVC tiled slice ending inside a tile was accepted\n");
        return 0;
    }
    const uint32_t four_tile_addresses[] = {0, 10, 120, 130};
    VASliceParameterBufferHEVC four_tile_slices[4] = {{0}};
    for (size_t i = 0; i < 4; i++) {
        four_tile_slices[i].slice_segment_address = four_tile_addresses[i];
        four_tile_slices[i].slice_data_size = 32;
        four_tile_slices[i].slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        four_tile_slices[i].LongSliceFlags.fields.slice_type = 2;
        four_tile_slices[i].LongSliceFlags.fields.LastSliceOfPic = i == 3;
    }
    if (!hobot_hevc_slice_sequence_init(&picture, 4, &sequence)) {
        fprintf(stderr, "HEVC four-tile slice sequence initialization failed\n");
        return 0;
    }
    for (size_t i = 0; i < 4; i++) {
        uint64_t expected_entry_points = UINT64_MAX;
        int has_next_slice = i + 1u < 4u;
        uint32_t next_address = has_next_slice ?
            four_tile_addresses[i + 1u] : 0;
        if (!hobot_hevc_slice_sequence_add_with_next(
                &sequence, &four_tile_slices[i], 32, has_next_slice,
                next_address, &expected_entry_points) ||
            expected_entry_points != 0) {
            fprintf(stderr, "HEVC one-slice-per-tile entry-point count was incorrect\n");
            return 0;
        }
    }
    if (!hobot_hevc_slice_sequence_complete(&sequence)) {
        fprintf(stderr, "HEVC four-tile slice sequence was incomplete\n");
        return 0;
    }
    picture.pic_fields.bits.tiles_enabled_flag = 0;
    picture.num_tile_columns_minus1 = 0;
    picture.num_tile_rows_minus1 = 0;
    picture.column_width_minus1[0] = 0;
    picture.row_height_minus1[0] = 0;
    slices[0].num_entry_point_offsets = 0;
    slices[0].LongSliceFlags.fields.LastSliceOfPic = 0;

    VASliceParameterBufferHEVC three_slices[3] = {{0}};
    const uint32_t three_slice_addresses[] = {0, 80, 160};
    for (size_t i = 0; i < 3; i++) {
        memset(three_slices[i].RefPicList, 0xff,
               sizeof(three_slices[i].RefPicList));
        three_slices[i].slice_segment_address = three_slice_addresses[i];
        three_slices[i].slice_data_size = 32;
        three_slices[i].slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        three_slices[i].LongSliceFlags.fields.LastSliceOfPic = (i == 2);
        three_slices[i].LongSliceFlags.fields.slice_type = 2;
    }
    if (!hobot_hevc_slice_sequence_init(&picture, 3, &sequence) ||
        !hobot_hevc_slice_sequence_add(&sequence, &three_slices[0], 32) ||
        !hobot_hevc_slice_sequence_add(&sequence, &three_slices[1], 32) ||
        !hobot_hevc_slice_sequence_add(&sequence, &three_slices[2], 32) ||
        !hobot_hevc_slice_sequence_complete(&sequence)) {
        fprintf(stderr, "valid HEVC three-slice CTU sequence rejected\n");
        return 0;
    }
    picture.slice_parsing_fields.bits.dependent_slice_segments_enabled_flag = 1;
    three_slices[1].LongSliceFlags.fields.dependent_slice_segment_flag = 1;
    three_slices[2].LongSliceFlags.fields.dependent_slice_segment_flag = 1;
    if (!hobot_hevc_slice_sequence_init(&picture, 3, &sequence) ||
        !hobot_hevc_slice_sequence_add(&sequence, &three_slices[0], 32) ||
        !hobot_hevc_slice_sequence_add(&sequence, &three_slices[1], 32) ||
        !hobot_hevc_slice_sequence_add(&sequence, &three_slices[2], 32) ||
        !hobot_hevc_slice_sequence_complete(&sequence)) {
        fprintf(stderr, "valid HEVC dependent slice sequence rejected\n");
        return 0;
    }
    three_slices[2].LongSliceFlags.fields.slice_type = 1;
    if (!hobot_hevc_slice_sequence_init(&picture, 3, &sequence) ||
        !hobot_hevc_slice_sequence_add(&sequence, &three_slices[0], 32) ||
        !hobot_hevc_slice_sequence_add(&sequence, &three_slices[1], 32) ||
        hobot_hevc_slice_sequence_add(&sequence, &three_slices[2], 32)) {
        fprintf(stderr, "HEVC dependent slice changed inherited slice type\n");
        return 0;
    }
    three_slices[2].LongSliceFlags.fields.slice_type = 2;
    three_slices[0].LongSliceFlags.fields.dependent_slice_segment_flag = 1;
    if (!hobot_hevc_slice_sequence_init(&picture, 3, &sequence) ||
        hobot_hevc_slice_sequence_add(&sequence, &three_slices[0], 32)) {
        fprintf(stderr, "HEVC accepted a dependent first slice segment\n");
        return 0;
    }
    three_slices[0].LongSliceFlags.fields.dependent_slice_segment_flag = 0;
    three_slices[1].LongSliceFlags.fields.dependent_slice_segment_flag = 0;
    three_slices[2].LongSliceFlags.fields.dependent_slice_segment_flag = 0;
    picture.slice_parsing_fields.bits.dependent_slice_segments_enabled_flag = 0;
    three_slices[2].LongSliceFlags.fields.LastSliceOfPic = 0;
    if (!hobot_hevc_slice_sequence_init(&picture, 3, &sequence) ||
        !hobot_hevc_slice_sequence_add(&sequence, &three_slices[0], 32) ||
        !hobot_hevc_slice_sequence_add(&sequence, &three_slices[1], 32) ||
        hobot_hevc_slice_sequence_add(&sequence, &three_slices[2], 32)) {
        fprintf(stderr, "HEVC three-slice sequence accepted a missing final-slice flag\n");
        return 0;
    }

    slices[0].LongSliceFlags.fields.LastSliceOfPic = 1;
    if (!hobot_hevc_slice_sequence_init(&picture, 2, &sequence) ||
        hobot_hevc_slice_sequence_add(&sequence, &slices[0], 64)) {
        fprintf(stderr, "HEVC sequence accepted an early last-slice flag\n");
        return 0;
    }
    slices[0].LongSliceFlags.fields.LastSliceOfPic = 0;

    slices[1].slice_segment_address = 0;
    if (!hobot_hevc_slice_sequence_init(&picture, 2, &sequence) ||
        !hobot_hevc_slice_sequence_add(&sequence, &slices[0], 64) ||
        hobot_hevc_slice_sequence_add(&sequence, &slices[1], 48)) {
        fprintf(stderr, "HEVC sequence accepted duplicate/non-increasing CTU addresses\n");
        return 0;
    }
    slices[1].slice_segment_address = 240;
    if (!hobot_hevc_slice_sequence_init(&picture, 2, &sequence) ||
        !hobot_hevc_slice_sequence_add(&sequence, &slices[0], 64) ||
        hobot_hevc_slice_sequence_add(&sequence, &slices[1], 48)) {
        fprintf(stderr, "HEVC sequence accepted an out-of-picture CTU address\n");
        return 0;
    }
    slices[1].slice_segment_address = 120;

    if (!hobot_hevc_slice_sequence_init(&picture, 2, &sequence) ||
        !hobot_hevc_slice_sequence_add(&sequence, &slices[0], 64) ||
        hobot_hevc_slice_sequence_add(&sequence, &slices[1], 47)) {
        fprintf(stderr, "HEVC sequence accepted slice data beyond its buffer\n");
        return 0;
    }

    picture.num_short_term_ref_pic_sets = 14;
    if (!hobot_hevc_slice_sequence_init(&picture, 2, &sequence)) {
        fprintf(stderr, "HEVC two-RPS slice topology could not be initialized\n");
        return 0;
    }

    return 1;
}

static int test_hevc_pcm_sps_serialization(void)
{
    VAPictureParameterBufferHEVC picture = {0};
    picture.pic_width_in_luma_samples = 640;
    picture.pic_height_in_luma_samples = 360;
    picture.pic_fields.bits.chroma_format_idc = 1;
    picture.pic_fields.bits.pcm_enabled_flag = 1;
    picture.pic_fields.bits.pcm_loop_filter_disabled_flag = 1;
    picture.sps_max_dec_pic_buffering_minus1 = 3;
    picture.log2_max_pic_order_cnt_lsb_minus4 = 4;
    picture.log2_diff_max_min_luma_coding_block_size = 3;
    picture.log2_diff_max_min_transform_block_size = 3;
    picture.pcm_sample_bit_depth_luma_minus1 = 7;
    picture.pcm_sample_bit_depth_chroma_minus1 = 7;
    picture.log2_min_pcm_luma_coding_block_size_minus3 = 0;
    picture.log2_diff_max_min_pcm_luma_coding_block_size = 2;
    if (!hobot_hevc_picture_parameters_supported(&picture, 640, 360)) {
        fprintf(stderr, "valid PCM SPS parameters were rejected before serialization\n");
        return 0;
    }

    uint8_t sps[256];
    int sps_size = generate_hevc_sps(&picture, 1, NULL, sps, sizeof(sps));
    uint8_t rbsp[256];
    size_t rbsp_size = 0;
    if (sps_size < 3 ||
        !hobot_hevc_unescape_rbsp(sps + 2, (size_t)sps_size - 2u,
                                  rbsp, sizeof(rbsp), &rbsp_size)) {
        fprintf(stderr, "PCM-enabled HEVC SPS generation or unescape failed\n");
        return 0;
    }

    BitReader reader = {rbsp, rbsp_size * 8u, 0};
    uint32_t value;
    if (!br_skip_bits(&reader, 4u + 3u + 1u + 96u) ||
        !br_read_ue(&reader, &value, NULL, NULL) ||
        !br_read_ue(&reader, &value, NULL, NULL) ||
        !br_read_ue(&reader, &value, NULL, NULL) ||
        !br_read_ue(&reader, &value, NULL, NULL) ||
        !br_skip_bits(&reader, 1) ||
        !br_read_ue(&reader, &value, NULL, NULL) ||
        !br_read_ue(&reader, &value, NULL, NULL) ||
        !br_read_ue(&reader, &value, NULL, NULL) ||
        !br_skip_bits(&reader, 1) ||
        !br_read_ue(&reader, &value, NULL, NULL) ||
        !br_read_ue(&reader, &value, NULL, NULL) ||
        !br_read_ue(&reader, &value, NULL, NULL)) {
        fprintf(stderr, "PCM-enabled HEVC SPS header parse failed\n");
        return 0;
    }
    for (size_t i = 0; i < 6; i++) {
        if (!br_read_ue(&reader, &value, NULL, NULL)) {
            fprintf(stderr, "PCM-enabled HEVC SPS coding syntax is truncated\n");
            return 0;
        }
    }

    uint32_t pcm_enabled;
    uint32_t pcm_luma_depth;
    uint32_t pcm_chroma_depth;
    uint32_t pcm_min_size;
    uint32_t pcm_size_diff;
    uint32_t pcm_filter_disabled;
    uint32_t rps_count;
    if (!br_skip_bits(&reader, 3) ||
        !br_read_bits(&reader, 1, &pcm_enabled) || pcm_enabled != 1 ||
        !br_read_bits(&reader, 4, &pcm_luma_depth) || pcm_luma_depth != 7 ||
        !br_read_bits(&reader, 4, &pcm_chroma_depth) || pcm_chroma_depth != 7 ||
        !br_read_ue(&reader, &pcm_min_size, NULL, NULL) || pcm_min_size != 0 ||
        !br_read_ue(&reader, &pcm_size_diff, NULL, NULL) || pcm_size_diff != 2 ||
        !br_read_bits(&reader, 1, &pcm_filter_disabled) || pcm_filter_disabled != 1 ||
        !br_read_ue(&reader, &rps_count, NULL, NULL) || rps_count != 0) {
        fprintf(stderr, "PCM SPS syntax values or following RPS alignment are incorrect\n");
        return 0;
    }
    return 1;
}

static int test_hevc_tiles_pps_layout(
    const VAPictureParameterBufferHEVC *picture,
    uint32_t expected_uniform_spacing,
    uint32_t expected_column_width_minus1,
    uint32_t expected_row_height_minus1
)
{
    uint8_t pps[256];
    uint8_t rbsp[256];
    size_t rbsp_size = 0;
    int pps_size = generate_hevc_pps(picture, pps, sizeof(pps));
    if (pps_size < 3 ||
        !hobot_hevc_unescape_rbsp(pps + 2, (size_t)pps_size - 2u,
                                  rbsp, sizeof(rbsp), &rbsp_size)) {
        fprintf(stderr, "HEVC tile PPS generation or unescape failed\n");
        return 0;
    }

    BitReader reader = {rbsp, rbsp_size * 8u, 0};
    uint32_t value;
    int32_t signed_value;
    uint32_t tiles_enabled;
    uint32_t entropy_coding_sync_enabled;
    uint32_t uniform_spacing;
    uint32_t loop_filter_across_tiles;
    uint32_t pps_loop_filter_across_slices;
    uint32_t deblocking_filter_control_present;
    uint32_t deblocking_filter_override;
    uint32_t pps_disable_deblocking_filter;
    if (!br_read_ue(&reader, &value, NULL, NULL) || value != 0 ||
        !br_read_ue(&reader, &value, NULL, NULL) || value != 0 ||
        !br_skip_bits(&reader, 2u + 3u + 2u) ||
        !br_read_ue(&reader, &value, NULL, NULL) ||
        !br_read_ue(&reader, &value, NULL, NULL) ||
        !br_read_se(&reader, &signed_value) ||
        !br_skip_bits(&reader, 3) ||
        !br_read_se(&reader, &signed_value) ||
        !br_read_se(&reader, &signed_value) ||
        !br_skip_bits(&reader, 4) ||
        !br_read_bits(&reader, 1, &tiles_enabled) ||
            tiles_enabled != picture->pic_fields.bits.tiles_enabled_flag ||
        !br_read_bits(&reader, 1, &entropy_coding_sync_enabled) ||
            entropy_coding_sync_enabled !=
                picture->pic_fields.bits.entropy_coding_sync_enabled_flag ||
        !br_read_ue(&reader, &value, NULL, NULL) ||
            value != picture->num_tile_columns_minus1 ||
        !br_read_ue(&reader, &value, NULL, NULL) ||
            value != picture->num_tile_rows_minus1 ||
        !br_read_bits(&reader, 1, &uniform_spacing) ||
            uniform_spacing != expected_uniform_spacing ||
        (!uniform_spacing &&
         (!br_read_ue(&reader, &value, NULL, NULL) ||
          value != expected_column_width_minus1 ||
          !br_read_ue(&reader, &value, NULL, NULL) ||
          value != expected_row_height_minus1)) ||
        !br_read_bits(&reader, 1, &loop_filter_across_tiles) ||
            loop_filter_across_tiles !=
                picture->pic_fields.bits.loop_filter_across_tiles_enabled_flag ||
        !br_read_bits(&reader, 1, &pps_loop_filter_across_slices) ||
            pps_loop_filter_across_slices !=
                picture->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag ||
        !br_read_bits(&reader, 1, &deblocking_filter_control_present) ||
            deblocking_filter_control_present != 1 ||
        !br_read_bits(&reader, 1, &deblocking_filter_override) ||
            deblocking_filter_override !=
                picture->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag ||
        !br_read_bits(&reader, 1, &pps_disable_deblocking_filter) ||
            pps_disable_deblocking_filter !=
                picture->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag) {
        fprintf(stderr, "HEVC tile PPS syntax or following fields are misaligned\n");
        return 0;
    }
    return 1;
}

static int test_hevc_tiles_pps_serialization(void)
{
    VAPictureParameterBufferHEVC picture = {0};
    picture.pic_width_in_luma_samples = 640;
    picture.pic_height_in_luma_samples = 360;
    picture.pic_fields.bits.chroma_format_idc = 1;
    picture.log2_diff_max_min_luma_coding_block_size = 3;
    picture.pic_fields.bits.tiles_enabled_flag = 1;
    picture.pic_fields.bits.loop_filter_across_tiles_enabled_flag = 1;
    picture.num_tile_columns_minus1 = 1;
    picture.num_tile_rows_minus1 = 1;
    picture.column_width_minus1[0] = 4;
    picture.row_height_minus1[0] = 2;
    if (!hobot_hevc_picture_parameters_supported(&picture, 640, 360) ||
        !test_hevc_tiles_pps_layout(&picture, 1, 0, 0)) {
        fprintf(stderr, "valid uniform HEVC tile PPS was rejected or misserialized\n");
        return 0;
    }

    picture.column_width_minus1[0] = 5;
    picture.row_height_minus1[0] = 3;
    if (!hobot_hevc_picture_parameters_supported(&picture, 640, 360) ||
        !test_hevc_tiles_pps_layout(&picture, 0, 5, 3)) {
        fprintf(stderr, "valid non-uniform HEVC tile PPS was rejected or misserialized\n");
        return 0;
    }
    return 1;
}

static size_t test_build_two_rps_sao_slice(
    uint8_t *data,
    size_t capacity,
    unsigned int slice_type,
    int temporal_mvp_enabled,
    int collocated_from_l0_flag,
    int sao_luma_flag,
    int sao_chroma_flag
)
{
    if (!data || capacity < 8 || (slice_type != 0 && slice_type != 1))
        return 0;

    memset(data, 0, capacity);
    data[0] = 0x02;
    data[1] = 0x01;
    BitWriter writer = {data + 2, 0};
    bw_put_bit(&writer, 1);       /* first_slice_segment_in_pic_flag */
    bw_put_ue(&writer, 0);        /* slice_pic_parameter_set_id */
    bw_put_ue(&writer, slice_type);
    bw_put_bits(&writer, 1, 8);   /* slice_pic_order_cnt_lsb */
    bw_put_bit(&writer, 1);       /* select an SPS RPS */
    bw_put_bit(&writer, 0);       /* select SPS RPS index 0 */
    if (temporal_mvp_enabled) {
        bw_put_bit(&writer, 1);
    }
    bw_put_bit(&writer, (unsigned int)sao_luma_flag);
    bw_put_bit(&writer, (unsigned int)sao_chroma_flag);
    bw_put_bit(&writer, 0);       /* no active-reference override */
    if (slice_type == 0)
        bw_put_bit(&writer, 0);   /* mvd_l1_zero_flag */
    if (temporal_mvp_enabled && slice_type == 0)
        bw_put_bit(&writer, (unsigned int)collocated_from_l0_flag);
    bw_put_ue(&writer, 0);        /* five_minus_max_num_merge_cand */
    bw_put_se(&writer, 0);        /* slice_qp_delta */
    bw_put_bit(&writer, 1);       /* byte_alignment() */
    while (writer.bit_pos % 8u)
        bw_put_bit(&writer, 0);

    return 2u + writer.bit_pos / 8u;
}

static size_t test_build_two_rps_inline_tmvp_slice(
    uint8_t *data,
    size_t capacity
)
{
    if (!data || capacity < 8)
        return 0;

    memset(data, 0, capacity);
    data[0] = 0x02;
    data[1] = 0x01;
    BitWriter writer = {data + 2, 0};
    bw_put_bit(&writer, 1);       /* first_slice_segment_in_pic_flag */
    bw_put_ue(&writer, 0);        /* slice_pic_parameter_set_id */
    bw_put_ue(&writer, 1);        /* P slice */
    bw_put_bits(&writer, 1, 8);   /* slice_pic_order_cnt_lsb */
    bw_put_bit(&writer, 0);       /* inline short-term RPS */
    bw_put_bit(&writer, 0);       /* no inter-RPS prediction */
    bw_put_ue(&writer, 1);        /* one negative reference */
    bw_put_ue(&writer, 0);        /* no positive references */
    bw_put_ue(&writer, 0);        /* reference POC -1 */
    bw_put_bit(&writer, 1);       /* reference is used by current picture */
    bw_put_bit(&writer, 1);       /* temporal MVP enabled */
    bw_put_bit(&writer, 1);       /* SAO luma */
    bw_put_bit(&writer, 1);       /* SAO chroma */
    bw_put_bit(&writer, 0);       /* no active-reference override */
    bw_put_ue(&writer, 0);        /* five_minus_max_num_merge_cand */
    bw_put_se(&writer, 0);        /* slice_qp_delta */
    bw_put_bit(&writer, 1);       /* byte_alignment() */
    while (writer.bit_pos % 8u)
        bw_put_bit(&writer, 0);

    return 2u + writer.bit_pos / 8u;
}

static size_t test_build_zero_rps_tmvp_slice(
    uint8_t *data,
    size_t capacity
)
{
    if (!data || capacity < 8)
        return 0;

    memset(data, 0, capacity);
    data[0] = 0x02;
    data[1] = 0x01;
    BitWriter writer = {data + 2, 0};
    bw_put_bit(&writer, 1);       /* first_slice_segment_in_pic_flag */
    bw_put_ue(&writer, 0);        /* slice_pic_parameter_set_id */
    bw_put_ue(&writer, 1);        /* P slice */
    bw_put_bits(&writer, 1, 8);   /* slice_pic_order_cnt_lsb */
    bw_put_bit(&writer, 0);       /* inline short-term RPS */
    bw_put_ue(&writer, 1);        /* one negative reference */
    bw_put_ue(&writer, 0);        /* no positive references */
    bw_put_ue(&writer, 0);        /* reference POC -1 */
    bw_put_bit(&writer, 1);       /* reference is used by current picture */
    bw_put_bit(&writer, 1);       /* temporal MVP enabled */
    bw_put_bit(&writer, 1);       /* SAO luma */
    bw_put_bit(&writer, 1);       /* SAO chroma */
    bw_put_bit(&writer, 0);       /* no active-reference override */
    bw_put_ue(&writer, 0);        /* five_minus_max_num_merge_cand */
    bw_put_se(&writer, 0);        /* slice_qp_delta */
    bw_put_bit(&writer, 1);       /* byte_alignment() */
    while (writer.bit_pos % 8u)
        bw_put_bit(&writer, 0);

    return 2u + writer.bit_pos / 8u;
}

static size_t test_build_two_rps_weighted_slice(
    uint8_t *data,
    size_t capacity,
    unsigned int slice_type,
    int first_slice,
    unsigned int segment_address,
    unsigned int address_bits,
    int cabac_init_present,
    int loop_filter_present,
    int32_t slice_qp_delta,
    int32_t l0_weight_delta,
    int32_t l0_offset,
    int32_t l1_weight_delta,
    int32_t l1_offset,
    int chroma_weight_l0_flag,
    int chroma_weight_l1_flag,
    int32_t chroma_l0_weight_delta,
    int32_t chroma_l0_offset,
    int32_t chroma_l1_weight_delta,
    int32_t chroma_l1_offset
)
{
    if (!data || capacity < 16 || (slice_type != 0 && slice_type != 1) ||
        address_bits > 32)
        return 0;

    memset(data, 0, capacity);
    data[0] = 0x02;
    data[1] = 0x01;
    BitWriter writer = {data + 2, 0};
    bw_put_bit(&writer, first_slice);
    bw_put_ue(&writer, 0);
    if (!first_slice)
        bw_put_bits(&writer, segment_address, address_bits);
    bw_put_ue(&writer, slice_type);
    bw_put_bits(&writer, 1, 8);
    bw_put_bit(&writer, 1);
    bw_put_bit(&writer, 0);
    bw_put_bit(&writer, 0);
    if (slice_type == 0)
        bw_put_bit(&writer, 1);
    if (cabac_init_present)
        bw_put_bit(&writer, 0);

    bw_put_ue(&writer, 0);
    bw_put_se(&writer, 0);
    bw_put_bit(&writer, 1);
    bw_put_bit(&writer, chroma_weight_l0_flag);
    bw_put_se(&writer, l0_weight_delta);
    bw_put_se(&writer, l0_offset);
    if (chroma_weight_l0_flag) {
        for (size_t i = 0; i < 2; i++) {
            bw_put_se(&writer, chroma_l0_weight_delta);
            bw_put_se(&writer, chroma_l0_offset);
        }
    }
    if (slice_type == 0) {
        bw_put_bit(&writer, 1);
        bw_put_bit(&writer, chroma_weight_l1_flag);
        bw_put_se(&writer, l1_weight_delta);
        bw_put_se(&writer, l1_offset);
        if (chroma_weight_l1_flag) {
            for (size_t i = 0; i < 2; i++) {
                bw_put_se(&writer, chroma_l1_weight_delta);
                bw_put_se(&writer, chroma_l1_offset);
            }
        }
    }

    bw_put_ue(&writer, 0);
    bw_put_se(&writer, slice_qp_delta);
    if (loop_filter_present)
        bw_put_bit(&writer, 1);
    bw_put_bit(&writer, 1);
    while (writer.bit_pos % 8u)
        bw_put_bit(&writer, 0);

    return 2u + writer.bit_pos / 8u;
}

typedef struct {
    int temporal_mvp_present;
    int temporal_mvp_enabled;
    int active_override;
    uint8_t active_minus1[2];
    int list_modification_present;
    int list_modified[2];
    uint8_t list_entry[2][15];
    int mvd_l1_zero;
} TestHevcInlineSliceOptions;

static size_t test_build_inline_rps_slice(
    uint8_t *data,
    size_t capacity,
    unsigned int slice_type,
    int sps_rps_flag,
    uint32_t negative_count,
    uint32_t positive_count,
    uint32_t delta_poc_minus1,
    int used_by_curr_pic,
    int use_delta,
    const TestHevcInlineSliceOptions *options,
    uint32_t *rps_bits
)
{
    if (!data || capacity < 16 || !rps_bits ||
        negative_count > 15 || positive_count > 15 - negative_count ||
        (slice_type != 0 && slice_type != 1 && slice_type != 2))
        return 0;
    (void)use_delta; /* Direct RPS syntax has no use_delta_flag. */

    TestHevcInlineSliceOptions defaults = {
        .temporal_mvp_present = 1,
        .temporal_mvp_enabled = 1,
    };
    const TestHevcInlineSliceOptions *opts = options ? options : &defaults;

    memset(data, 0, capacity);
    data[0] = 0x02;
    data[1] = 0x01;
    BitWriter writer = {data + 2, 0};
    bw_put_bit(&writer, 1);
    bw_put_ue(&writer, 0);
    bw_put_ue(&writer, slice_type);
    bw_put_bits(&writer, 1, 8);
    bw_put_bit(&writer, (unsigned int)sps_rps_flag);
    *rps_bits = 0;
    if (!sps_rps_flag) {
        size_t rps_start = writer.bit_pos;
        bw_put_ue(&writer, negative_count);
        bw_put_ue(&writer, positive_count);
        for (uint32_t i = 0; i < negative_count + positive_count; i++) {
            bw_put_ue(&writer, delta_poc_minus1);
            bw_put_bit(&writer, (unsigned int)used_by_curr_pic);
        }
        *rps_bits = (uint32_t)(writer.bit_pos - rps_start);
    }
    if (opts->temporal_mvp_present)
        bw_put_bit(&writer, (unsigned int)opts->temporal_mvp_enabled);
    if (slice_type != 2) {
        bw_put_bit(&writer, (unsigned int)opts->active_override);
        if (opts->active_override) {
            bw_put_ue(&writer, opts->active_minus1[0]);
            if (slice_type == 0)
                bw_put_ue(&writer, opts->active_minus1[1]);
        }
        if (opts->list_modification_present &&
            negative_count + positive_count > 1) {
            unsigned int list_count = slice_type == 0 ? 2u : 1u;
            for (unsigned int list = 0; list < list_count; list++) {
                bw_put_bit(&writer, (unsigned int)opts->list_modified[list]);
                if (opts->list_modified[list]) {
                    unsigned int active_count =
                        (unsigned int)opts->active_minus1[list] + 1u;
                    unsigned int entry_bits = 0;
                    for (unsigned int count = negative_count + positive_count - 1;
                         count != 0; count >>= 1)
                        entry_bits++;
                    for (unsigned int i = 0; i < active_count; i++)
                        bw_put_bits(&writer, opts->list_entry[list][i],
                                    entry_bits);
                }
            }
        }
        if (slice_type == 0)
            bw_put_bit(&writer, (unsigned int)opts->mvd_l1_zero);
    }
    if (opts->temporal_mvp_present && opts->temporal_mvp_enabled &&
        slice_type == 0)
        bw_put_bit(&writer, 1); /* collocated_from_l0_flag */
    bw_put_ue(&writer, 0);
    bw_put_se(&writer, 0);
    bw_put_bit(&writer, 1);
    while (writer.bit_pos % 8u)
        bw_put_bit(&writer, 0);

    return 2u + writer.bit_pos / 8u;
}

static size_t test_build_inline_rps_with_unused_following(
    uint8_t *data, size_t capacity, uint32_t *rps_bits
)
{
    if (!data || capacity < 16 || !rps_bits)
        return 0;
    memset(data, 0, capacity);
    data[0] = 0x02;
    data[1] = 0x01;
    BitWriter writer = {data + 2, 0};
    bw_put_bit(&writer, 1);
    bw_put_ue(&writer, 0);
    bw_put_ue(&writer, 1);
    bw_put_bits(&writer, 1, 8);
    bw_put_bit(&writer, 0);
    size_t rps_start = writer.bit_pos;
    bw_put_ue(&writer, 2);
    bw_put_ue(&writer, 0);
    bw_put_ue(&writer, 0);
    bw_put_bit(&writer, 1);
    bw_put_ue(&writer, 0);
    bw_put_bit(&writer, 0);
    *rps_bits = (uint32_t)(writer.bit_pos - rps_start);
    bw_put_bit(&writer, 1);
    bw_put_bit(&writer, 0);
    bw_put_ue(&writer, 0);
    bw_put_se(&writer, 0);
    bw_put_bit(&writer, 1);
    while (writer.bit_pos % 8u)
        bw_put_bit(&writer, 0);
    return 2u + writer.bit_pos / 8u;
}

static int test_hevc_rps_slice_validation(void)
{
    VAPictureParameterBufferHEVC picture = {0};
    picture.num_short_term_ref_pic_sets = 2;
    picture.log2_max_pic_order_cnt_lsb_minus4 = 4;
    picture.pic_fields.bits.chroma_format_idc = 1;
    picture.slice_parsing_fields.bits.IdrPicFlag = 1;
    picture.slice_parsing_fields.bits.RapPicFlag = 1;
    picture.slice_parsing_fields.bits.IntraPicFlag = 1;
    HobotHevcSliceSequence sequence = {
        .picture_ctb_count = 1,
        .slice_count = 1,
    };
    for (size_t i = 0; i < 15; i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    }

    uint8_t idr_data[] = {0x26, 0x01, 0};
    BitWriter idr_writer = {idr_data + 2, 0};
    bw_put_bit(&idr_writer, 1);
    bw_put_bit(&idr_writer, 0);
    bw_put_ue(&idr_writer, 0);
    bw_put_ue(&idr_writer, 2);
    VASliceParameterBufferHEVC slice = {0};
    slice.slice_data_size = sizeof(idr_data);
    slice.slice_data_byte_offset = 3;
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    slice.LongSliceFlags.fields.LastSliceOfPic = 1;
    slice.LongSliceFlags.fields.slice_type = 2;
    if (!hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, idr_data, sizeof(idr_data), &sequence)) {
        fprintf(stderr, "valid IDR frame with two SPS RPS sets was rejected\n");
        return 0;
    }
    picture.num_short_term_ref_pic_sets = 0;
    if (!hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, idr_data, sizeof(idr_data), &sequence)) {
        fprintf(stderr, "valid IDR frame with no SPS RPS sets was rejected\n");
        return 0;
    }
    picture.num_short_term_ref_pic_sets = 2;
    picture.slice_parsing_fields.bits.RapPicFlag = 0;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, idr_data, sizeof(idr_data), &sequence)) {
        fprintf(stderr, "HEVC IDR/RAP flags disagreed with the NAL unit\n");
        return 0;
    }
    picture.slice_parsing_fields.bits.RapPicFlag = 1;

    picture.num_short_term_ref_pic_sets = 1;
    picture.st_rps_bits = 0;
    picture.CurrPic.picture_id = 1;
    picture.CurrPic.pic_order_cnt = 3;
    picture.slice_parsing_fields.bits.IdrPicFlag = 0;
    picture.slice_parsing_fields.bits.RapPicFlag = 1;
    picture.slice_parsing_fields.bits.IntraPicFlag = 1;
    picture.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = 1;
    picture.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;

    uint8_t cra_data[32] = {0x2a, 0x01};
    BitWriter cra_writer = {cra_data + 2, 0};
    bw_put_bit(&cra_writer, 1);       /* first_slice_segment_in_pic_flag */
    bw_put_bit(&cra_writer, 0);       /* no_output_of_prior_pics_flag */
    bw_put_ue(&cra_writer, 0);        /* slice_pic_parameter_set_id */
    bw_put_ue(&cra_writer, 2);        /* I slice */
    bw_put_bits(&cra_writer, 3, 8);   /* slice_pic_order_cnt_lsb */
    bw_put_bit(&cra_writer, 0);       /* inline short-term RPS */
    size_t cra_rps_start = cra_writer.bit_pos;
    bw_put_bit(&cra_writer, 0);       /* inter_ref_pic_set_prediction_flag */
    bw_put_ue(&cra_writer, 0);        /* num_negative_pics */
    bw_put_ue(&cra_writer, 0);        /* num_positive_pics */
    picture.st_rps_bits = (uint32_t)(cra_writer.bit_pos - cra_rps_start);
    bw_put_bit(&cra_writer, 1);       /* slice_sao_luma_flag */
    bw_put_bit(&cra_writer, 1);       /* slice_sao_chroma_flag */
    bw_put_se(&cra_writer, 14);       /* slice_qp_delta */
    bw_put_bit(&cra_writer, 1);       /* loop_filter_across_slices */
    bw_put_bit(&cra_writer, 1);       /* byte_alignment() */
    while (cra_writer.bit_pos % 8u)
        bw_put_bit(&cra_writer, 0);
    size_t cra_size = 2u + cra_writer.bit_pos / 8u;

    memset(&slice, 0, sizeof(slice));
    memset(slice.RefPicList, 0xff, sizeof(slice.RefPicList));
    slice.slice_data_offset = 0;
    slice.slice_data_size = (uint32_t)cra_size;
    slice.slice_data_byte_offset = (uint32_t)cra_size;
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    slice.LongSliceFlags.fields.LastSliceOfPic = 1;
    slice.LongSliceFlags.fields.slice_type = 2;
    slice.LongSliceFlags.fields.slice_sao_luma_flag = 1;
    slice.LongSliceFlags.fields.slice_sao_chroma_flag = 1;
    slice.LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag = 1;
    slice.slice_qp_delta = 14;
    if (!hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, cra_data, cra_size, &sequence)) {
        fprintf(stderr, "Moonlight-style empty-inline-RPS CRA frame was rejected\n");
        return 0;
    }
    picture.slice_parsing_fields.bits.RapPicFlag = 0;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, cra_data, cra_size, &sequence)) {
        fprintf(stderr, "CRA frame with a mismatched RAP flag was accepted\n");
        return 0;
    }

    picture.num_short_term_ref_pic_sets = 0;
    picture.st_rps_bits = 0;
    picture.CurrPic.pic_order_cnt = 32;
    picture.slice_parsing_fields.bits.RapPicFlag = 1;
    picture.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag = 1;
    const int32_t cra_ref_pocs[4] = {22, 28, 26, 24};
    for (size_t i = 0; i < 15; i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    for (size_t i = 0; i < 4; i++) {
        picture.ReferenceFrames[i].picture_id = (VASurfaceID)(10 + i);
        picture.ReferenceFrames[i].pic_order_cnt = cra_ref_pocs[i];
        picture.ReferenceFrames[i].flags = 0;
    }

    uint8_t cra_leading_data[64] = {0x2a, 0x01};
    BitWriter cra_leading_writer = {cra_leading_data + 2, 0};
    bw_put_bit(&cra_leading_writer, 1);       /* first_slice_segment_in_pic_flag */
    bw_put_bit(&cra_leading_writer, 0);       /* no_output_of_prior_pics_flag */
    bw_put_ue(&cra_leading_writer, 0);        /* slice_pic_parameter_set_id */
    bw_put_ue(&cra_leading_writer, 2);        /* I slice */
    bw_put_bits(&cra_leading_writer, 32, 8);  /* slice_pic_order_cnt_lsb */
    bw_put_bit(&cra_leading_writer, 0);       /* inline short-term RPS */
    size_t cra_leading_rps_start = cra_leading_writer.bit_pos;
    bw_put_ue(&cra_leading_writer, 4);        /* num_negative_pics */
    bw_put_ue(&cra_leading_writer, 0);        /* num_positive_pics */
    const uint32_t cra_delta_pocs[4] = {3, 1, 1, 1};
    for (size_t i = 0; i < 4; i++) {
        bw_put_ue(&cra_leading_writer, cra_delta_pocs[i]);
        bw_put_bit(&cra_leading_writer, 0);   /* not used by current CRA */
    }
    picture.st_rps_bits =
        (uint32_t)(cra_leading_writer.bit_pos - cra_leading_rps_start);
    bw_put_bit(&cra_leading_writer, 1);       /* slice_temporal_mvp_enabled_flag */
    bw_put_bit(&cra_leading_writer, 1);       /* slice_sao_luma_flag */
    bw_put_bit(&cra_leading_writer, 1);       /* slice_sao_chroma_flag */
    bw_put_se(&cra_leading_writer, 6);        /* slice_qp_delta */
    bw_put_bit(&cra_leading_writer, 1);       /* loop_filter_across_slices */
    bw_put_bit(&cra_leading_writer, 1);       /* byte_alignment() */
    while (cra_leading_writer.bit_pos % 8u)
        bw_put_bit(&cra_leading_writer, 0);
    size_t cra_leading_size = 2u + cra_leading_writer.bit_pos / 8u;

    memset(&slice, 0, sizeof(slice));
    memset(slice.RefPicList, 0xff, sizeof(slice.RefPicList));
    slice.slice_data_size = (uint32_t)cra_leading_size;
    slice.slice_data_byte_offset = (uint32_t)cra_leading_size;
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    slice.LongSliceFlags.fields.LastSliceOfPic = 1;
    slice.LongSliceFlags.fields.slice_type = 2;
    slice.LongSliceFlags.fields.slice_temporal_mvp_enabled_flag = 1;
    slice.LongSliceFlags.fields.slice_sao_luma_flag = 1;
    slice.LongSliceFlags.fields.slice_sao_chroma_flag = 1;
    slice.LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag = 1;
    slice.slice_qp_delta = 6;
    if (!hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, cra_leading_data, cra_leading_size, &sequence)) {
        fprintf(stderr, "CRA with inline unused following references was rejected\n");
        return 0;
    }
    picture.ReferenceFrames[3].pic_order_cnt = 25;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, cra_leading_data, cra_leading_size, &sequence)) {
        fprintf(stderr, "CRA with a mismatched retained-reference POC was accepted\n");
        return 0;
    }
    picture.ReferenceFrames[3].pic_order_cnt = cra_ref_pocs[3];
    picture.ReferenceFrames[4].picture_id = 14;
    picture.ReferenceFrames[4].pic_order_cnt = 20;
    picture.ReferenceFrames[4].flags = 0;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, cra_leading_data, cra_leading_size, &sequence)) {
        fprintf(stderr, "CRA with an unlisted DPB reference was accepted\n");
        return 0;
    }

    for (size_t i = 0; i < 15; i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    picture.num_short_term_ref_pic_sets = 2;
    picture.st_rps_bits = 0;
    picture.slice_parsing_fields.bits.RapPicFlag = 0;
    picture.slice_parsing_fields.bits.IntraPicFlag = 0;
    picture.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = 0;
    picture.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 0;

    memset(&picture.slice_parsing_fields, 0,
           sizeof(picture.slice_parsing_fields));
    picture.CurrPic.picture_id = 1;
    picture.CurrPic.pic_order_cnt = 1;
    picture.ReferenceFrames[3].picture_id = 7;
    picture.ReferenceFrames[3].pic_order_cnt = 0;
    picture.ReferenceFrames[3].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    uint8_t p_data[64] = {0x02, 0x01};
    memset(&slice, 0, sizeof(slice));
    memset(slice.RefPicList, 0xff, sizeof(slice.RefPicList));
    slice.slice_data_size = sizeof(p_data);
    slice.slice_data_byte_offset = 2;
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    slice.RefPicList[0][0] = 3;
    slice.LongSliceFlags.fields.LastSliceOfPic = 1;
    slice.LongSliceFlags.fields.slice_type = 1;
    BitWriter p_writer = {p_data + 2, 0};
    bw_put_bit(&p_writer, 1);
    bw_put_ue(&p_writer, 0);
    bw_put_ue(&p_writer, 1);
    bw_put_bits(&p_writer, 1, 8);
    bw_put_bit(&p_writer, 1);
    bw_put_bit(&p_writer, 0);
    bw_put_bit(&p_writer, 0);
    bw_put_ue(&p_writer, 0);
    bw_put_se(&p_writer, 0);
    bw_put_bit(&p_writer, 1);
    while (p_writer.bit_pos % 8)
        bw_put_bit(&p_writer, 0);
    slice.slice_data_byte_offset = 2 + p_writer.bit_pos / 8;
    if (!hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, sizeof(p_data), &sequence)) {
        fprintf(stderr, "valid single-reference P frame with RPS index 0 was rejected\n");
        return 0;
    }

    picture.num_short_term_ref_pic_sets = 1;
    uint8_t single_rps_p_data[64] = {0x02, 0x01};
    p_writer = (BitWriter){single_rps_p_data + 2, 0};
    bw_put_bit(&p_writer, 1);
    bw_put_ue(&p_writer, 0);
    bw_put_ue(&p_writer, 1);
    bw_put_bits(&p_writer, 1, 8);
    bw_put_bit(&p_writer, 1);         /* select the single SPS RPS */
    bw_put_bit(&p_writer, 0);         /* no active-reference override */
    bw_put_ue(&p_writer, 0);          /* five_minus_max_num_merge_cand */
    bw_put_se(&p_writer, 0);          /* slice_qp_delta */
    bw_put_bit(&p_writer, 1);         /* byte_alignment() */
    while (p_writer.bit_pos % 8u)
        bw_put_bit(&p_writer, 0);
    slice.slice_data_size = (uint32_t)(2u + p_writer.bit_pos / 8u);
    slice.slice_data_byte_offset = slice.slice_data_size;
    if (!hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, single_rps_p_data,
            slice.slice_data_size, &sequence)) {
        fprintf(stderr, "single-set SPS-selected P frame without an index bit was rejected\n");
        return 0;
    }

    uint8_t generated_sps[256];
    int generated_sps_size = generate_hevc_sps(&picture, 1, NULL,
                                               generated_sps,
                                               sizeof(generated_sps));
    if (generated_sps_size < 3) {
        fprintf(stderr, "single-set HEVC SPS synthesis failed\n");
        return 0;
    }
    uint8_t generated_sps_rbsp[256];
    size_t generated_sps_rbsp_size = 0;
    if (!hobot_hevc_unescape_rbsp(generated_sps + 2,
                                  (size_t)generated_sps_size - 2u,
                                  generated_sps_rbsp,
                                  sizeof(generated_sps_rbsp),
                                  &generated_sps_rbsp_size)) {
        fprintf(stderr, "single-set HEVC SPS could not be unescaped\n");
        return 0;
    }
    BitReader sps_reader = {generated_sps_rbsp,
                            generated_sps_rbsp_size * 8u, 0};
    uint32_t sps_value;
    if (!br_skip_bits(&sps_reader, 4u + 3u + 1u + 96u) ||
        !br_read_ue(&sps_reader, &sps_value, NULL, NULL) ||
        !br_read_ue(&sps_reader, &sps_value, NULL, NULL) ||
        !br_read_ue(&sps_reader, &sps_value, NULL, NULL) ||
        !br_read_ue(&sps_reader, &sps_value, NULL, NULL) ||
        !br_skip_bits(&sps_reader, 1) ||
        !br_read_ue(&sps_reader, &sps_value, NULL, NULL) ||
        !br_read_ue(&sps_reader, &sps_value, NULL, NULL) ||
        !br_read_ue(&sps_reader, &sps_value, NULL, NULL) ||
        !br_skip_bits(&sps_reader, 1) ||
        !br_read_ue(&sps_reader, &sps_value, NULL, NULL) ||
        !br_read_ue(&sps_reader, &sps_value, NULL, NULL) ||
        !br_read_ue(&sps_reader, &sps_value, NULL, NULL)) {
        fprintf(stderr, "single-set HEVC SPS header parse failed\n");
        return 0;
    }
    for (size_t i = 0; i < 6; i++) {
        if (!br_read_ue(&sps_reader, &sps_value, NULL, NULL)) {
            fprintf(stderr, "single-set HEVC SPS coding-block syntax is truncated\n");
            return 0;
        }
    }
    uint32_t synthesized_rps_count;
    if (!br_skip_bits(&sps_reader, 4) ||
        !br_read_ue(&sps_reader, &synthesized_rps_count, NULL, NULL) ||
        synthesized_rps_count != 2) {
        fprintf(stderr, "single-set HEVC picture did not synthesize the indexed two-RPS profile\n");
        return 0;
    }

    size_t original_header_bytes = (size_t)slice.slice_data_byte_offset - 2u;
    uint8_t source_rbsp[128] = {0};
    const uint8_t payload[] = {0x00, 0x00, 0x01, 0xaa, 0x00, 0x00, 0x03, 0x55};
    if (original_header_bytes + sizeof(payload) > sizeof(source_rbsp)) {
        fprintf(stderr, "single-RPS rewrite test payload exceeds its buffer\n");
        return 0;
    }
    memcpy(source_rbsp, single_rps_p_data + 2, original_header_bytes);
    memcpy(source_rbsp + original_header_bytes, payload, sizeof(payload));
    uint8_t source_ebsp[256];
    size_t source_ebsp_size = 0;
    if (!hobot_hevc_escape_rbsp(source_rbsp,
                                original_header_bytes + sizeof(payload),
                                source_ebsp, sizeof(source_ebsp),
                                &source_ebsp_size)) {
        fprintf(stderr, "single-RPS rewrite test source escaping failed\n");
        return 0;
    }
    uint8_t source_nal[258] = {0x02, 0x01};
    memcpy(source_nal + 2, source_ebsp, source_ebsp_size);
    picture.pic_width_in_luma_samples = 640;
    picture.pic_height_in_luma_samples = 360;
    slice.slice_data_size = (uint32_t)(2u + source_ebsp_size);
    uint8_t *rewritten_nal = NULL;
    size_t rewritten_nal_size = 0;
    int rewrite_status = hobot_hevc_rewrite_rps_slice(
        &picture, &slice, source_nal, slice.slice_data_size,
        &rewritten_nal, &rewritten_nal_size);
    if (rewrite_status != 1 || !rewritten_nal || rewritten_nal_size < 3) {
        fprintf(stderr, "single-set SPS-RPS slice rewrite did not produce output\n");
        free(rewritten_nal);
        return 0;
    }
    uint8_t rewritten_rbsp[256];
    size_t rewritten_rbsp_size = 0;
    if (rewritten_nal[0] != source_nal[0] ||
        rewritten_nal[1] != source_nal[1] ||
        !hobot_hevc_unescape_rbsp(rewritten_nal + 2,
                                  rewritten_nal_size - 2u,
                                  rewritten_rbsp, sizeof(rewritten_rbsp),
                                  &rewritten_rbsp_size)) {
        fprintf(stderr, "single-set rewritten NAL header or RBSP is invalid\n");
        free(rewritten_nal);
        return 0;
    }
    size_t insert_bit = 0;
    int sps_rps_selected = 0;
    if (!hobot_hevc_validated_rps_slice_supported_internal(
            &picture, &slice, source_nal, slice.slice_data_size, &sequence,
            NULL, &insert_bit, &sps_rps_selected, NULL, NULL) || !sps_rps_selected ||
        insert_bit >= rewritten_rbsp_size * 8u ||
        ((rewritten_rbsp[insert_bit / 8u] >>
          (7u - (insert_bit % 8u))) & 1u) != 0) {
        fprintf(stderr, "single-set slice did not receive RPS index zero\n");
        free(rewritten_nal);
        return 0;
    }
    size_t rewritten_header_bytes =
        ((size_t)slice.slice_data_byte_offset - 2u);
    size_t alignment_bit = original_header_bytes * 8u;
    while (alignment_bit > 0) {
        size_t bit = alignment_bit - 1u;
        if ((source_rbsp[bit / 8u] &
             (uint8_t)(1u << (7u - (bit % 8u)))) != 0) {
            alignment_bit = bit;
            break;
        }
        alignment_bit--;
    }
    rewritten_header_bytes = (alignment_bit + 9u) / 8u;
    if (rewritten_header_bytes > rewritten_rbsp_size ||
        rewritten_rbsp_size - rewritten_header_bytes != sizeof(payload) ||
        memcmp(rewritten_rbsp + rewritten_header_bytes, payload,
               sizeof(payload)) != 0) {
        fprintf(stderr, "single-set slice rewrite changed the CABAC payload\n");
        free(rewritten_nal);
        return 0;
    }
    free(rewritten_nal);

    uint8_t second_slice_rbsp[128] = {0};
    BitWriter second_slice_writer = {second_slice_rbsp, 0};
    bw_put_bit(&second_slice_writer, 0);      /* not the first slice */
    bw_put_ue(&second_slice_writer, 0);       /* PPS id */
    bw_put_bits(&second_slice_writer, 30, 6); /* CTB address in a 60-CTB picture */
    bw_put_ue(&second_slice_writer, 1);       /* P slice */
    bw_put_bits(&second_slice_writer, 1, 8);  /* POC LSB */
    bw_put_bit(&second_slice_writer, 1);      /* select the single SPS RPS */
    bw_put_bit(&second_slice_writer, 0);      /* no active-reference override */
    bw_put_ue(&second_slice_writer, 0);       /* five_minus_max_num_merge_cand */
    bw_put_se(&second_slice_writer, 0);       /* slice_qp_delta */
    bw_put_bit(&second_slice_writer, 1);      /* byte_alignment() */
    while (second_slice_writer.bit_pos % 8u)
        bw_put_bit(&second_slice_writer, 0);
    size_t second_slice_header_size = second_slice_writer.bit_pos / 8u;
    if (second_slice_header_size != 3u ||
        second_slice_header_size + sizeof(payload) > sizeof(second_slice_rbsp)) {
        fprintf(stderr, "second-slice header layout is unexpected\n");
        return 0;
    }
    memcpy(second_slice_rbsp + second_slice_header_size, payload, sizeof(payload));

    uint8_t second_slice_ebsp[256];
    size_t second_slice_ebsp_size = 0;
    if (!hobot_hevc_escape_rbsp(second_slice_rbsp,
                                second_slice_header_size + sizeof(payload),
                                second_slice_ebsp, sizeof(second_slice_ebsp),
                                &second_slice_ebsp_size)) {
        fprintf(stderr, "second-slice test source escaping failed\n");
        return 0;
    }
    uint8_t second_slice_nal[258] = {0x02, 0x01};
    memcpy(second_slice_nal + 2, second_slice_ebsp, second_slice_ebsp_size);

    picture.pic_width_in_luma_samples = 640;
    picture.pic_height_in_luma_samples = 360;
    picture.log2_min_luma_coding_block_size_minus3 = 0;
    picture.log2_diff_max_min_luma_coding_block_size = 3;
    sequence.picture_ctb_count = 60;
    slice.slice_segment_address = 30;
    slice.slice_data_offset = 0;
    slice.slice_data_size = (uint32_t)(2u + second_slice_ebsp_size);
    slice.slice_data_byte_offset = (uint32_t)(2u + second_slice_header_size);
    size_t second_insert_bit = 0;
    int second_sps_rps_selected = 0;
    if (!hobot_hevc_validated_rps_slice_supported_internal(
            &picture, &slice, second_slice_nal, slice.slice_data_size,
            &sequence, NULL, &second_insert_bit,
            &second_sps_rps_selected, NULL, NULL) ||
        !second_sps_rps_selected || second_insert_bit != 20u) {
        fprintf(stderr, "independent second slice did not select SPS RPS at bit 20\n");
        return 0;
    }

    rewritten_nal = NULL;
    rewritten_nal_size = 0;
    if (hobot_hevc_rewrite_rps_slice(
            &picture, &slice, second_slice_nal, slice.slice_data_size,
            &rewritten_nal, &rewritten_nal_size) != 1 ||
        !rewritten_nal || rewritten_nal_size < 3u) {
        fprintf(stderr, "independent second-slice SPS-RPS rewrite failed\n");
        free(rewritten_nal);
        return 0;
    }

    uint8_t second_rewritten_rbsp[256];
    size_t second_rewritten_rbsp_size = 0;
    if (!hobot_hevc_unescape_rbsp(rewritten_nal + 2,
                                  rewritten_nal_size - 2u,
                                  second_rewritten_rbsp,
                                  sizeof(second_rewritten_rbsp),
                                  &second_rewritten_rbsp_size) ||
        rewritten_nal[0] != second_slice_nal[0] ||
        rewritten_nal[1] != second_slice_nal[1]) {
        fprintf(stderr, "independent second-slice rewritten RBSP is invalid\n");
        free(rewritten_nal);
        return 0;
    }

    BitReader second_rewritten_reader = {
        second_rewritten_rbsp, second_rewritten_rbsp_size * 8u, 0
    };
    uint32_t second_value, second_address, second_first_flag, second_poc_lsb;
    int32_t second_qp_delta;
    if (!br_read_bits(&second_rewritten_reader, 1, &second_first_flag) ||
        second_first_flag != 0 ||
        !br_read_ue(&second_rewritten_reader, &second_value, NULL, NULL) ||
        second_value != 0 ||
        !br_read_bits(&second_rewritten_reader, 6, &second_address) ||
        second_address != 30 ||
        !br_read_ue(&second_rewritten_reader, &second_value, NULL, NULL) ||
        second_value != 1 ||
        !br_read_bits(&second_rewritten_reader, 8, &second_poc_lsb) ||
        second_poc_lsb != 1 ||
        !br_read_bits(&second_rewritten_reader, 1, &second_value) ||
        second_value != 1 ||
        !br_read_bits(&second_rewritten_reader, 1, &second_value) ||
        second_value != 0 ||
        !br_read_bits(&second_rewritten_reader, 1, &second_value) ||
        second_value != 0 ||
        !br_read_ue(&second_rewritten_reader, &second_value, NULL, NULL) ||
        second_value != 0 ||
        !br_read_se(&second_rewritten_reader, &second_qp_delta) ||
        second_qp_delta != 0) {
        fprintf(stderr, "independent second-slice header fields were corrupted\n");
        free(rewritten_nal);
        return 0;
    }

    if (!br_read_bits(&second_rewritten_reader, 1, &second_value) ||
        second_value != 1u) {
        fprintf(stderr, "independent second-slice alignment marker changed\n");
        free(rewritten_nal);
        return 0;
    }
    while (second_rewritten_reader.bit_pos % 8u) {
        if (!br_read_bits(&second_rewritten_reader, 1, &second_value) ||
            second_value != 0u) {
            fprintf(stderr, "independent second-slice alignment padding changed\n");
            free(rewritten_nal);
            return 0;
        }
    }
    size_t second_rewritten_header_size = second_rewritten_reader.bit_pos / 8u;
    if (second_rewritten_header_size != 4u ||
        second_rewritten_header_size > second_rewritten_rbsp_size ||
        second_rewritten_rbsp_size - second_rewritten_header_size != sizeof(payload) ||
        memcmp(second_rewritten_rbsp + second_rewritten_header_size,
               payload, sizeof(payload)) != 0) {
        fprintf(stderr, "independent second-slice payload/alignment changed during rewrite\n");
        free(rewritten_nal);
        return 0;
    }
    free(rewritten_nal);

    picture.CurrPic.pic_order_cnt = 5;
    picture.slice_parsing_fields.bits.cabac_init_present_flag = 1;
    picture.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = 1;
    picture.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;
    for (size_t i = 0; i < 15; i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
        picture.ReferenceFrames[i].pic_order_cnt = 0;
    }
    picture.ReferenceFrames[0].picture_id = 8;
    picture.ReferenceFrames[0].flags = 0;
    picture.ReferenceFrames[1].picture_id = 7;
    picture.ReferenceFrames[1].pic_order_cnt = 4;
    picture.ReferenceFrames[1].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    uint8_t sps_sao_p_data[16] = {0x02, 0x01};
    BitWriter sps_sao_writer = {sps_sao_p_data + 2, 0};
    bw_put_bit(&sps_sao_writer, 1);       /* first slice */
    bw_put_ue(&sps_sao_writer, 0);        /* PPS id */
    bw_put_ue(&sps_sao_writer, 1);        /* P slice */
    bw_put_bits(&sps_sao_writer, 5, 8);   /* POC LSB */
    bw_put_bit(&sps_sao_writer, 1);       /* select the single SPS RPS */
    bw_put_bit(&sps_sao_writer, 1);       /* SAO luma */
    bw_put_bit(&sps_sao_writer, 1);       /* SAO chroma */
    bw_put_bit(&sps_sao_writer, 0);       /* no active-reference override */
    bw_put_bit(&sps_sao_writer, 0);       /* CABAC init */
    bw_put_ue(&sps_sao_writer, 0);        /* five_minus_max_num_merge_cand */
    bw_put_se(&sps_sao_writer, -6);       /* slice_qp_delta */
    bw_put_bit(&sps_sao_writer, 1);       /* loop filter across slices */
    bw_put_bit(&sps_sao_writer, 1);       /* byte_alignment() */
    while (sps_sao_writer.bit_pos % 8u)
        bw_put_bit(&sps_sao_writer, 0);
    size_t sps_sao_size = 2u + sps_sao_writer.bit_pos / 8u;
    memset(&slice, 0, sizeof(slice));
    memset(slice.RefPicList, 0xff, sizeof(slice.RefPicList));
    slice.slice_data_size = (uint32_t)sps_sao_size;
    slice.slice_data_byte_offset = (uint32_t)sps_sao_size;
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    slice.LongSliceFlags.fields.LastSliceOfPic = 1;
    slice.LongSliceFlags.fields.slice_type = 1;
    slice.LongSliceFlags.fields.slice_sao_luma_flag = 1;
    slice.LongSliceFlags.fields.slice_sao_chroma_flag = 1;
    slice.LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag = 1;
    slice.slice_qp_delta = -6;
    slice.RefPicList[0][0] = 1;
    if (sps_sao_size != 6 ||
        !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, sps_sao_p_data, sps_sao_size, &sequence)) {
        fprintf(stderr, "SPS-selected single-reference P slice with SAO was rejected\n");
        return 0;
    }
    picture.pic_width_in_luma_samples = 640;
    picture.pic_height_in_luma_samples = 360;
    uint8_t *sps_sao_rewritten = NULL;
    size_t sps_sao_rewritten_size = 0;
    if (hobot_hevc_rewrite_rps_slice(
            &picture, &slice, sps_sao_p_data, sps_sao_size,
            &sps_sao_rewritten, &sps_sao_rewritten_size) != 1 ||
        !sps_sao_rewritten || sps_sao_rewritten_size < 3) {
        fprintf(stderr, "SPS-selected SAO slice rewrite failed\n");
        free(sps_sao_rewritten);
        return 0;
    }
    uint8_t sps_sao_rbsp[64];
    size_t sps_sao_rbsp_size = 0;
    if (!hobot_hevc_unescape_rbsp(sps_sao_rewritten + 2,
                                  sps_sao_rewritten_size - 2u,
                                  sps_sao_rbsp, sizeof(sps_sao_rbsp),
                                  &sps_sao_rbsp_size) ||
        sps_sao_rbsp_size == 0 ||
        ((sps_sao_rbsp[1] >> 1) & 1u) != 0 ||
        ((sps_sao_rbsp[1] >> 0) & 1u) != 1u ||
        ((sps_sao_rbsp[2] >> 7) & 1u) != 1u ||
        ((sps_sao_rbsp[2] >> 6) & 1u) != 0) {
        fprintf(stderr, "single-RPS index was not inserted before both SAO flags\n");
        free(sps_sao_rewritten);
        return 0;
    }
    free(sps_sao_rewritten);
    picture.ReferenceFrames[1].pic_order_cnt = 3;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, sps_sao_p_data, sps_sao_size, &sequence)) {
        fprintf(stderr, "single-set SPS synthesis accepted a non-adjacent POC reference\n");
        return 0;
    }
    picture.ReferenceFrames[1].pic_order_cnt = 4;
    slice.LongSliceFlags.fields.slice_sao_chroma_flag = 0;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, sps_sao_p_data, sps_sao_size, &sequence)) {
        fprintf(stderr, "SPS-selected P slice accepted mismatched chroma SAO metadata\n");
        return 0;
    }
    slice.LongSliceFlags.fields.slice_sao_chroma_flag = 1;
    slice.RefPicList[0][0] = 0;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, sps_sao_p_data, sps_sao_size, &sequence)) {
        fprintf(stderr, "SPS-selected P slice accepted a non-current DPB reference\n");
        return 0;
    }

    memset(&picture.slice_parsing_fields, 0,
           sizeof(picture.slice_parsing_fields));
    picture.CurrPic.pic_order_cnt = 1;
    picture.num_ref_idx_l0_default_active_minus1 = 1;
    picture.num_ref_idx_l1_default_active_minus1 = 1;
    for (size_t i = 0; i < 15; i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
        picture.ReferenceFrames[i].pic_order_cnt = 0;
    }
    picture.ReferenceFrames[0].picture_id = 10;
    picture.ReferenceFrames[0].pic_order_cnt = 2;
    picture.ReferenceFrames[0].flags = VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
    picture.ReferenceFrames[1].picture_id = 11;
    picture.ReferenceFrames[1].pic_order_cnt = 0;
    picture.ReferenceFrames[1].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    TestHevcInlineSliceOptions sps_b_options = {0};
    uint32_t ignored_sps_rps_bits;
    uint8_t sps_b_data[64];
    size_t sps_b_size = test_build_inline_rps_slice(
        sps_b_data, sizeof(sps_b_data), 0, 1, 0, 0, 0, 0, 0,
        &sps_b_options, &ignored_sps_rps_bits);
    memset(&slice, 0, sizeof(slice));
    memset(slice.RefPicList, 0xff, sizeof(slice.RefPicList));
    slice.slice_data_size = (uint32_t)sps_b_size;
    slice.slice_data_byte_offset = (uint32_t)sps_b_size;
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    slice.LongSliceFlags.fields.LastSliceOfPic = 1;
    slice.LongSliceFlags.fields.slice_type = 0;
    slice.num_ref_idx_l0_active_minus1 = 1;
    slice.num_ref_idx_l1_active_minus1 = 1;
    slice.RefPicList[0][0] = 1;
    slice.RefPicList[0][1] = 0;
    slice.RefPicList[1][0] = 0;
    slice.RefPicList[1][1] = 1;
    if (!sps_b_size || hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, sps_b_data, sps_b_size, &sequence)) {
        fprintf(stderr, "single-set SPS synthesis accepted a multi-reference B RPS\n");
        return 0;
    }

    picture.num_short_term_ref_pic_sets = 0;
    uint8_t inline_b_data[64];
    uint32_t inline_b_rps_bits;
    size_t inline_b_size = test_build_inline_rps_slice(
        inline_b_data, sizeof(inline_b_data), 0, 0, 1, 1, 0, 1, 0,
        &sps_b_options, &inline_b_rps_bits);
    picture.st_rps_bits = inline_b_rps_bits;
    slice.slice_data_size = (uint32_t)inline_b_size;
    slice.slice_data_byte_offset = (uint32_t)inline_b_size;
    if (!inline_b_size || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_b_data, inline_b_size, &sequence)) {
        fprintf(stderr, "inline B RPS rejected POC-ordered DPB references\n");
        return 0;
    }
    slice.RefPicList[0][0] = 0;
    slice.RefPicList[0][1] = 1;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_b_data, inline_b_size, &sequence)) {
        fprintf(stderr, "inline B RPS accepted a non-default L0 ordering\n");
        return 0;
    }

    picture.num_short_term_ref_pic_sets = 1;
    picture.st_rps_bits = 0;
    picture.CurrPic.pic_order_cnt = 2;
    picture.slice_parsing_fields.bits.cabac_init_present_flag = 1;
    picture.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = 1;
    picture.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;
    picture.num_ref_idx_l0_default_active_minus1 = 0;
    picture.num_ref_idx_l1_default_active_minus1 = 0;
    for (size_t i = 0; i < 15; i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
        picture.ReferenceFrames[i].pic_order_cnt = 0;
    }
    picture.ReferenceFrames[0].picture_id = 8;
    picture.ReferenceFrames[0].flags = 0;
    picture.ReferenceFrames[1].picture_id = 7;
    picture.ReferenceFrames[1].pic_order_cnt = 1;
    picture.ReferenceFrames[1].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    uint8_t count1_inline_data[64] = {0x02, 0x01};
    BitWriter count1_inline_writer = {count1_inline_data + 2, 0};
    bw_put_bit(&count1_inline_writer, 1); /* first slice */
    bw_put_ue(&count1_inline_writer, 0);
    bw_put_ue(&count1_inline_writer, 1);
    bw_put_bits(&count1_inline_writer, 2, 8);
    bw_put_bit(&count1_inline_writer, 0); /* inline RPS */
    size_t count1_rps_start = count1_inline_writer.bit_pos;
    bw_put_bit(&count1_inline_writer, 0); /* no inter-RPS prediction */
    bw_put_ue(&count1_inline_writer, 2);  /* two negative references */
    bw_put_ue(&count1_inline_writer, 0);  /* no positive references */
    bw_put_ue(&count1_inline_writer, 0);
    bw_put_bit(&count1_inline_writer, 1); /* POC 1 is used */
    bw_put_ue(&count1_inline_writer, 0);
    bw_put_bit(&count1_inline_writer, 0); /* POC 0 is unused by this picture */
    picture.st_rps_bits = (uint32_t)(count1_inline_writer.bit_pos -
                                     count1_rps_start);
    bw_put_bit(&count1_inline_writer, 1); /* SAO luma */
    bw_put_bit(&count1_inline_writer, 1); /* SAO chroma */
    bw_put_bit(&count1_inline_writer, 0); /* no active-reference override */
    bw_put_bit(&count1_inline_writer, 0); /* cabac_init_flag */
    bw_put_ue(&count1_inline_writer, 0);  /* five_minus_max_num_merge_cand */
    bw_put_se(&count1_inline_writer, 1);  /* slice_qp_delta */
    bw_put_bit(&count1_inline_writer, 1); /* loop_filter_across_slices */
    bw_put_bit(&count1_inline_writer, 1); /* byte_alignment() */
    while (count1_inline_writer.bit_pos % 8u)
        bw_put_bit(&count1_inline_writer, 0);
    size_t count1_inline_size = 2u + count1_inline_writer.bit_pos / 8u;
    memset(&slice, 0, sizeof(slice));
    memset(slice.RefPicList, 0xff, sizeof(slice.RefPicList));
    slice.slice_data_size = (uint32_t)count1_inline_size;
    slice.slice_data_byte_offset = (uint32_t)count1_inline_size;
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    slice.RefPicList[0][0] = 1;
    slice.LongSliceFlags.fields.LastSliceOfPic = 1;
    slice.LongSliceFlags.fields.slice_type = 1;
    slice.LongSliceFlags.fields.slice_sao_luma_flag = 1;
    slice.LongSliceFlags.fields.slice_sao_chroma_flag = 1;
    slice.LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag = 1;
    slice.slice_qp_delta = 1;
    if (picture.st_rps_bits != 9 ||
        !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, count1_inline_data,
            count1_inline_size, &sequence)) {
        fprintf(stderr, "single-set inline P RPS with one unused DPB reference was rejected\n");
        return 0;
    }
    picture.pic_width_in_luma_samples = 640;
    picture.pic_height_in_luma_samples = 360;
    uint8_t *inline_rewrite = NULL;
    size_t inline_rewrite_size = 0;
    if (hobot_hevc_rewrite_rps_slice(
            &picture, &slice, count1_inline_data, count1_inline_size,
            &inline_rewrite, &inline_rewrite_size) != 0 ||
        inline_rewrite || inline_rewrite_size != 0) {
        fprintf(stderr, "inline single-set RPS was unexpectedly rewritten\n");
        free(inline_rewrite);
        return 0;
    }

    picture.CurrPic.pic_order_cnt = 1;
    picture.st_rps_bits = 0;
    picture.slice_parsing_fields.bits.cabac_init_present_flag = 0;
    picture.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = 0;
    picture.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 0;
    for (size_t i = 0; i < 15; i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    picture.ReferenceFrames[3].picture_id = 7;
    picture.ReferenceFrames[3].pic_order_cnt = 0;
    picture.ReferenceFrames[3].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;

    uint8_t inline_data[64];
    uint32_t inline_rps_bits;
    size_t inline_offset = test_build_inline_rps_slice(
        inline_data, sizeof(inline_data), 1, 0, 1, 0, 0, 1, 1,
        NULL, &inline_rps_bits);
    picture.num_short_term_ref_pic_sets = 0;
    picture.st_rps_bits = inline_rps_bits;
    picture.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag = 1;
    slice.LongSliceFlags.fields.slice_temporal_mvp_enabled_flag = 1;
    slice.LongSliceFlags.fields.collocated_from_l0_flag = 1;
    slice.LongSliceFlags.fields.slice_sao_luma_flag = 0;
    slice.LongSliceFlags.fields.slice_sao_chroma_flag = 0;
    slice.LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag = 0;
    slice.slice_qp_delta = 0;
    slice.RefPicList[0][0] = 3;
    slice.slice_data_size = sizeof(inline_data);
    slice.slice_data_byte_offset = (uint32_t)inline_offset;
    if (!inline_offset || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "valid inline-RPS P slice was rejected\n");
        return 0;
    }
    picture.ReferenceFrames[3].flags = VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline negative RPS accepted a mismatched VA POC category\n");
        return 0;
    }
    picture.ReferenceFrames[3].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    slice.RefPicList[0][0] = 4;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline RPS accepted a list index outside its current references\n");
        return 0;
    }
    slice.RefPicList[0][0] = 3;
    picture.ReferenceFrames[3].pic_order_cnt = -1;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline RPS accepted a VA reference with mismatched POC\n");
        return 0;
    }
    picture.ReferenceFrames[3].pic_order_cnt = 0;
    picture.st_rps_bits++;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline RPS accepted a mismatched st_rps_bits length\n");
        return 0;
    }
    picture.st_rps_bits--;

    inline_offset = test_build_inline_rps_slice(
        inline_data, sizeof(inline_data), 1, 1, 0, 0, 0, 1, 1,
        NULL, &inline_rps_bits);
    slice.slice_data_byte_offset = (uint32_t)inline_offset;
    if (!inline_offset || hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "zero-RPS picture accepted an SPS RPS selection flag\n");
        return 0;
    }

    memset(inline_data, 0, sizeof(inline_data));
    inline_data[0] = 0x02;
    inline_data[1] = 0x01;
    BitWriter oversized_rps_writer = {inline_data + 2, 0};
    bw_put_bit(&oversized_rps_writer, 1);
    bw_put_ue(&oversized_rps_writer, 0);
    bw_put_ue(&oversized_rps_writer, 1);
    bw_put_bits(&oversized_rps_writer, 1, 8);
    bw_put_bit(&oversized_rps_writer, 0);
    bw_put_ue(&oversized_rps_writer, 16);
    bw_put_ue(&oversized_rps_writer, 0);
    inline_offset = 2 + oversized_rps_writer.bit_pos / 8;
    picture.st_rps_bits = 0;
    slice.slice_data_byte_offset = (uint32_t)inline_offset;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline RPS accepted more entries than the VA reference array can represent\n");
        return 0;
    }

    inline_offset = test_build_inline_rps_slice(
        inline_data, sizeof(inline_data), 1, 0, 9, 0, 0, 1, 1,
        NULL, &inline_rps_bits);
    picture.st_rps_bits = inline_rps_bits;
    slice.slice_data_byte_offset = (uint32_t)inline_offset;
    if (!inline_offset || hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline RPS accepted more than VA's eight current references\n");
        return 0;
    }

    inline_offset = test_build_inline_rps_slice(
        inline_data, sizeof(inline_data), 1, 0, 1, 0, 0, 0, 0,
        NULL, &inline_rps_bits);
    picture.st_rps_bits = inline_rps_bits;
    slice.slice_data_byte_offset = (uint32_t)inline_offset;
    if (!inline_offset || hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "P slice accepted an RPS with no current reference\n");
        return 0;
    }

    inline_offset = test_build_inline_rps_with_unused_following(
        inline_data, sizeof(inline_data), &inline_rps_bits);
    picture.ReferenceFrames[4].picture_id = 8;
    picture.ReferenceFrames[4].pic_order_cnt = -1;
    picture.ReferenceFrames[4].flags = 0;
    picture.st_rps_bits = inline_rps_bits;
    slice.slice_data_byte_offset = (uint32_t)inline_offset;
    if (!inline_offset || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline RPS rejected an unused following picture retained in the DPB\n");
        return 0;
    }
    picture.ReferenceFrames[4].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline RPS accepted a following picture marked as current\n");
        return 0;
    }
    picture.ReferenceFrames[4].flags = 0;

    inline_offset = test_build_inline_rps_slice(
        inline_data, sizeof(inline_data), 0, 0, 1, 1, 0, 1, 1,
        NULL, &inline_rps_bits);
    picture.ReferenceFrames[4].picture_id = 8;
    picture.ReferenceFrames[4].pic_order_cnt = 2;
    picture.ReferenceFrames[4].flags = VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
    slice.RefPicList[0][0] = 3;
    slice.RefPicList[1][0] = 4;
    picture.st_rps_bits = inline_rps_bits;
    slice.LongSliceFlags.fields.slice_type = 0;
    slice.slice_data_byte_offset = (uint32_t)inline_offset;
    if (!inline_offset || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "valid inline-RPS B slice with negative and positive references was rejected\n");
        return 0;
    }
    slice.RefPicList[1][0] = 5;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline B RPS accepted an L1 index outside its current references\n");
        return 0;
    }
    slice.RefPicList[1][0] = 4;
    inline_data[0] = 0x00;
    if (!hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "valid non-reference TRAIL_N slice was rejected\n");
        return 0;
    }

    for (size_t i = 0; i < 15; i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
        picture.ReferenceFrames[i].pic_order_cnt = 0;
    }
    picture.ReferenceFrames[2].picture_id = 12;
    picture.ReferenceFrames[2].pic_order_cnt = 0;
    picture.ReferenceFrames[2].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    picture.ReferenceFrames[5].picture_id = 15;
    picture.ReferenceFrames[5].pic_order_cnt = -1;
    picture.ReferenceFrames[5].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    picture.ReferenceFrames[8].picture_id = 18;
    picture.ReferenceFrames[8].pic_order_cnt = 2;
    picture.ReferenceFrames[8].flags = VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
    picture.ReferenceFrames[11].picture_id = 21;
    picture.ReferenceFrames[11].pic_order_cnt = 3;
    picture.ReferenceFrames[11].flags = VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
    picture.num_ref_idx_l0_default_active_minus1 = 3;
    picture.num_ref_idx_l1_default_active_minus1 = 3;
    picture.slice_parsing_fields.bits.lists_modification_present_flag = 0;
    slice.num_ref_idx_l0_active_minus1 = 3;
    slice.num_ref_idx_l1_active_minus1 = 0;
    memset(slice.RefPicList, 0xff, sizeof(slice.RefPicList));
    slice.RefPicList[0][0] = 2;
    slice.RefPicList[0][1] = 5;
    slice.RefPicList[0][2] = 8;
    slice.RefPicList[0][3] = 11;
    slice.LongSliceFlags.fields.slice_type = 1;
    inline_offset = test_build_inline_rps_slice(
        inline_data, sizeof(inline_data), 1, 0, 2, 2, 0, 1, 1,
        NULL, &inline_rps_bits);
    picture.st_rps_bits = inline_rps_bits;
    slice.slice_data_byte_offset = (uint32_t)inline_offset;
    if (!inline_offset || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline RPS rejected the default P-list order\n");
        return 0;
    }
    slice.RefPicList[0][0] = 5;
    slice.RefPicList[0][1] = 2;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline RPS accepted a reordered P list without list modification\n");
        return 0;
    }
    slice.RefPicList[0][0] = 2;
    slice.RefPicList[0][1] = 5;

    slice.LongSliceFlags.fields.slice_type = 0;
    slice.num_ref_idx_l1_active_minus1 = 3;
    slice.RefPicList[1][0] = 8;
    slice.RefPicList[1][1] = 11;
    slice.RefPicList[1][2] = 2;
    slice.RefPicList[1][3] = 5;
    inline_offset = test_build_inline_rps_slice(
        inline_data, sizeof(inline_data), 0, 0, 2, 2, 0, 1, 1,
        NULL, &inline_rps_bits);
    picture.st_rps_bits = inline_rps_bits;
    slice.slice_data_byte_offset = (uint32_t)inline_offset;
    if (!inline_offset || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline RPS rejected the default B-list orders\n");
        return 0;
    }
    slice.RefPicList[1][0] = 2;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline RPS accepted a non-default B L1 order\n");
        return 0;
    }
    slice.RefPicList[1][0] = 8;

    TestHevcInlineSliceOptions modified_lists = {
        .temporal_mvp_present = 1,
        .temporal_mvp_enabled = 1,
        .list_modification_present = 1,
        .list_modified = {1, 1},
        .active_minus1 = {3, 3},
        .list_entry = {{1, 0, 3, 2}, {2, 3, 0, 1}},
    };
    picture.slice_parsing_fields.bits.lists_modification_present_flag = 1;
    slice.RefPicList[0][0] = 5;
    slice.RefPicList[0][1] = 2;
    slice.RefPicList[0][2] = 11;
    slice.RefPicList[0][3] = 8;
    slice.RefPicList[1][0] = 2;
    slice.RefPicList[1][1] = 5;
    slice.RefPicList[1][2] = 8;
    slice.RefPicList[1][3] = 11;
    inline_offset = test_build_inline_rps_slice(
        inline_data, sizeof(inline_data), 0, 0, 2, 2, 0, 1, 1,
        &modified_lists, &inline_rps_bits);
    picture.st_rps_bits = inline_rps_bits;
    slice.slice_data_byte_offset = (uint32_t)inline_offset;
    if (!inline_offset || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline RPS rejected valid signaled L0/L1 modifications\n");
        return 0;
    }
    slice.RefPicList[1][0] = 8;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline RPS accepted VA lists that disagree with signaled modifications\n");
        return 0;
    }

    picture.ReferenceFrames[11].picture_id = VA_INVALID_SURFACE;
    picture.ReferenceFrames[11].flags = VA_PICTURE_HEVC_INVALID;
    picture.num_ref_idx_l0_default_active_minus1 = 2;
    picture.num_ref_idx_l1_default_active_minus1 = 2;
    slice.num_ref_idx_l0_active_minus1 = 2;
    slice.num_ref_idx_l1_active_minus1 = 2;
    memset(slice.RefPicList, 0xff, sizeof(slice.RefPicList));
    slice.RefPicList[0][0] = 5;
    slice.RefPicList[0][1] = 8;
    slice.RefPicList[0][2] = 2;
    slice.RefPicList[1][0] = 2;
    slice.RefPicList[1][1] = 5;
    slice.RefPicList[1][2] = 8;
    modified_lists.active_minus1[0] = 2;
    modified_lists.active_minus1[1] = 2;
    modified_lists.list_entry[0][0] = 1;
    modified_lists.list_entry[0][1] = 2;
    modified_lists.list_entry[0][2] = 0;
    modified_lists.list_entry[1][0] = 1;
    modified_lists.list_entry[1][1] = 2;
    modified_lists.list_entry[1][2] = 0;
    inline_offset = test_build_inline_rps_slice(
        inline_data, sizeof(inline_data), 0, 0, 2, 1, 0, 1, 1,
        &modified_lists, &inline_rps_bits);
    picture.st_rps_bits = inline_rps_bits;
    slice.slice_data_byte_offset = (uint32_t)inline_offset;
    if (!inline_offset || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline RPS rejected valid 3-reference L0/L1 modifications\n");
        return 0;
    }
    slice.RefPicList[1][0] = 8;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline RPS accepted a mismatched 3-reference L1 order\n");
        return 0;
    }

    for (size_t i = 0; i < 15; i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    picture.ReferenceFrames[2].picture_id = 12;
    picture.ReferenceFrames[2].pic_order_cnt = 0;
    picture.ReferenceFrames[2].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    picture.ReferenceFrames[5].picture_id = 15;
    picture.ReferenceFrames[5].pic_order_cnt = -1;
    picture.ReferenceFrames[5].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    picture.ReferenceFrames[8].picture_id = 18;
    picture.ReferenceFrames[8].pic_order_cnt = 2;
    picture.ReferenceFrames[8].flags = VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
    picture.num_ref_idx_l0_default_active_minus1 = 2;
    picture.num_ref_idx_l1_default_active_minus1 = 0;
    slice.num_ref_idx_l0_active_minus1 = 2;
    slice.num_ref_idx_l1_active_minus1 = 0;
    slice.LongSliceFlags.fields.slice_type = 1;
    slice.RefPicList[0][0] = 2;
    slice.RefPicList[0][1] = 5;
    slice.RefPicList[0][2] = 8;
    memset(&modified_lists, 0, sizeof(modified_lists));
    modified_lists.temporal_mvp_present = 1;
    modified_lists.temporal_mvp_enabled = 1;
    modified_lists.list_modification_present = 1;
    modified_lists.list_modified[0] = 1;
    modified_lists.active_minus1[0] = 2;
    modified_lists.list_entry[0][0] = 0;
    modified_lists.list_entry[0][1] = 1;
    modified_lists.list_entry[0][2] = 3;
    inline_offset = test_build_inline_rps_slice(
        inline_data, sizeof(inline_data), 1, 0, 2, 1, 0, 1, 1,
        &modified_lists, &inline_rps_bits);
    picture.st_rps_bits = inline_rps_bits;
    slice.slice_data_byte_offset = (uint32_t)inline_offset;
    if (!inline_offset || hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline RPS accepted an unrepresentable list-modification index\n");
        return 0;
    }

    TestHevcInlineSliceOptions active_override = {
        .temporal_mvp_present = 1,
        .temporal_mvp_enabled = 1,
        .active_override = 1,
        .active_minus1 = {1, 0},
        .list_modification_present = 1,
    };
    inline_offset = test_build_inline_rps_slice(
        inline_data, sizeof(inline_data), 1, 0, 2, 1, 0, 1, 1,
        &active_override, &inline_rps_bits);
    picture.st_rps_bits = inline_rps_bits;
    slice.slice_data_byte_offset = (uint32_t)inline_offset;
    if (!inline_offset || hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline RPS accepted an active-reference count mismatch\n");
        return 0;
    }

    for (size_t i = 0; i < 15; i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    picture.ReferenceFrames[2].picture_id = 12;
    picture.ReferenceFrames[2].pic_order_cnt = 0;
    picture.ReferenceFrames[2].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    picture.num_ref_idx_l0_default_active_minus1 = 1;
    picture.num_ref_idx_l1_default_active_minus1 = 0;
    slice.num_ref_idx_l0_active_minus1 = 1;
    slice.num_ref_idx_l1_active_minus1 = 0;
    slice.RefPicList[0][0] = 2;
    slice.RefPicList[0][1] = 2;
    memset(slice.RefPicList[0] + 2, 0xff, 13);
    memset(slice.RefPicList[1], 0xff, sizeof(slice.RefPicList[1]));
    inline_offset = test_build_inline_rps_slice(
        inline_data, sizeof(inline_data), 1, 0, 1, 0, 0, 1, 1,
        NULL, &inline_rps_bits);
    picture.st_rps_bits = inline_rps_bits;
    slice.slice_data_byte_offset = (uint32_t)inline_offset;
    if (!inline_offset || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, inline_data, sizeof(inline_data), &sequence)) {
        fprintf(stderr, "inline RPS rejected the normative repeated single-reference list\n");
        return 0;
    }

    for (size_t i = 0; i < 15; i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
        picture.ReferenceFrames[i].pic_order_cnt = 0;
    }
    picture.ReferenceFrames[3].picture_id = 7;
    picture.ReferenceFrames[3].pic_order_cnt = 0;
    picture.ReferenceFrames[3].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    picture.ReferenceFrames[4].picture_id = 8;
    picture.ReferenceFrames[4].pic_order_cnt = 2;
    picture.ReferenceFrames[4].flags = VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
    picture.num_ref_idx_l0_default_active_minus1 = 0;
    picture.num_ref_idx_l1_default_active_minus1 = 0;
    picture.slice_parsing_fields.bits.lists_modification_present_flag = 0;
    slice.num_ref_idx_l0_active_minus1 = 0;
    slice.num_ref_idx_l1_active_minus1 = 0;
    slice.LongSliceFlags.fields.slice_type = 1;
    memset(slice.RefPicList, 0xff, sizeof(slice.RefPicList));
    slice.RefPicList[0][0] = 3;

    picture.ReferenceFrames[4].picture_id = VA_INVALID_SURFACE;
    picture.ReferenceFrames[4].flags = VA_PICTURE_HEVC_INVALID;
    memset(slice.RefPicList[1], 0xff, sizeof(slice.RefPicList[1]));
    picture.num_short_term_ref_pic_sets = 2;
    picture.st_rps_bits = 0;
    picture.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag = 0;
    slice.LongSliceFlags.fields.slice_temporal_mvp_enabled_flag = 0;
    slice.LongSliceFlags.fields.slice_type = 1;
    picture.slice_parsing_fields.bits.lists_modification_present_flag = 1;
    if (!hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, sizeof(p_data), &sequence)) {
        fprintf(stderr, "single-reference P frame rejected an unused list-modification capability flag\n");
        return 0;
    }
    picture.slice_parsing_fields.bits.lists_modification_present_flag = 0;

    picture.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = 1;
    size_t two_rps_sao_offset = test_build_two_rps_sao_slice(
        p_data, sizeof(p_data), 1, 0, 0, 1, 1);
    slice.slice_data_size = (uint32_t)two_rps_sao_offset;
    slice.slice_data_byte_offset = (uint32_t)two_rps_sao_offset;
    slice.LongSliceFlags.fields.slice_sao_luma_flag = 1;
    slice.LongSliceFlags.fields.slice_sao_chroma_flag = 1;
    if (!two_rps_sao_offset || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, two_rps_sao_offset, &sequence)) {
        fprintf(stderr, "valid two-RPS P slice with luma/chroma SAO was rejected\n");
        return 0;
    }
    slice.LongSliceFlags.fields.slice_sao_luma_flag = 0;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, two_rps_sao_offset, &sequence)) {
        fprintf(stderr, "two-RPS P slice accepted mismatched luma SAO metadata\n");
        return 0;
    }
    slice.LongSliceFlags.fields.slice_sao_luma_flag = 1;
    slice.LongSliceFlags.fields.slice_sao_chroma_flag = 0;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, two_rps_sao_offset, &sequence)) {
        fprintf(stderr, "two-RPS P slice accepted mismatched chroma SAO metadata\n");
        return 0;
    }
    slice.LongSliceFlags.fields.slice_sao_chroma_flag = 1;
    slice.slice_data_byte_offset = (uint32_t)(two_rps_sao_offset - 1u);
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, two_rps_sao_offset, &sequence)) {
        fprintf(stderr, "two-RPS SAO slice header crossed its data boundary\n");
        return 0;
    }
    picture.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = 0;
    slice.slice_data_byte_offset = (uint32_t)two_rps_sao_offset;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, two_rps_sao_offset, &sequence)) {
        fprintf(stderr, "two-RPS slice accepted SAO flags when SPS SAO was disabled\n");
        return 0;
    }
    picture.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = 1;
    picture.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag = 1;
    slice.LongSliceFlags.fields.slice_temporal_mvp_enabled_flag = 1;
    slice.LongSliceFlags.fields.collocated_from_l0_flag = 1;
    slice.collocated_ref_idx = 0;
    two_rps_sao_offset = test_build_two_rps_sao_slice(
        p_data, sizeof(p_data), 1, 1, 1, 1, 1);
    slice.slice_data_size = (uint32_t)two_rps_sao_offset;
    slice.slice_data_byte_offset = (uint32_t)two_rps_sao_offset;
    slice.LongSliceFlags.fields.slice_sao_luma_flag = 1;
    slice.LongSliceFlags.fields.slice_sao_chroma_flag = 1;
    if (!two_rps_sao_offset || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, two_rps_sao_offset, &sequence)) {
        fprintf(stderr, "valid two-RPS P slice with temporal MVP was rejected\n");
        return 0;
    }
    uint8_t *tmvp_rewritten_nal = NULL;
    size_t tmvp_rewritten_nal_size = 0;
    if (hobot_hevc_rewrite_rps_slice(
            &picture, &slice, p_data, two_rps_sao_offset,
            &tmvp_rewritten_nal, &tmvp_rewritten_nal_size) != 1 ||
        !tmvp_rewritten_nal || tmvp_rewritten_nal_size < 3u) {
        fprintf(stderr, "two-RPS temporal-MVP selection rewrite failed\n");
        free(tmvp_rewritten_nal);
        return 0;
    }

    uint8_t inline_tmvp_data[64];
    size_t inline_tmvp_data_size = test_build_two_rps_inline_tmvp_slice(
        inline_tmvp_data, sizeof(inline_tmvp_data));
    VAPictureParameterBufferHEVC inline_source_picture = picture;
    VASliceParameterBufferHEVC inline_source_slice = slice;
    inline_source_picture.st_rps_bits = 7;
    inline_source_slice.slice_data_size = (uint32_t)inline_tmvp_data_size;
    inline_source_slice.slice_data_byte_offset =
        (uint32_t)inline_tmvp_data_size;
    if (!inline_tmvp_data_size || !hobot_hevc_validated_rps_slice_supported(
            &inline_source_picture, &inline_source_slice, inline_tmvp_data,
            inline_tmvp_data_size, &sequence)) {
        fprintf(stderr, "valid inline two-RPS temporal-MVP syntax was rejected\n");
        free(tmvp_rewritten_nal);
        return 0;
    }
    uint8_t *inline_tmvp_rewritten_nal = NULL;
    size_t inline_tmvp_rewritten_nal_size = 0;
    if (hobot_hevc_rewrite_rps_slice(
            &inline_source_picture, &inline_source_slice, inline_tmvp_data,
            inline_tmvp_data_size, &inline_tmvp_rewritten_nal,
            &inline_tmvp_rewritten_nal_size) != 1 ||
        !inline_tmvp_rewritten_nal) {
        fprintf(stderr, "inline two-RPS temporal-MVP rewrite failed\n");
        free(tmvp_rewritten_nal);
        free(inline_tmvp_rewritten_nal);
        return 0;
    }

    VAPictureParameterBufferHEVC inline_tmvp_picture = picture;
    VASliceParameterBufferHEVC inline_tmvp_slice = slice;
    inline_tmvp_picture.num_short_term_ref_pic_sets = 0;
    inline_tmvp_picture.st_rps_bits = 6;
    inline_tmvp_slice.slice_data_size = (uint32_t)tmvp_rewritten_nal_size;
    inline_tmvp_slice.slice_data_byte_offset =
        (uint32_t)tmvp_rewritten_nal_size;
    VASliceParameterBufferHEVC malformed_zero_rps_slice = inline_tmvp_slice;
    malformed_zero_rps_slice.slice_data_size = (uint32_t)inline_tmvp_data_size;
    malformed_zero_rps_slice.slice_data_byte_offset =
        (uint32_t)inline_tmvp_data_size;
    if (hobot_hevc_validated_rps_slice_supported(
            &inline_tmvp_picture, &malformed_zero_rps_slice,
            inline_tmvp_data, inline_tmvp_data_size, &sequence)) {
        fprintf(stderr, "zero-RPS syntax accepted a two-set RPS prediction flag\n");
        free(tmvp_rewritten_nal);
        free(inline_tmvp_rewritten_nal);
        return 0;
    }

    size_t zero_tmvp_data_size = test_build_zero_rps_tmvp_slice(
        inline_tmvp_data, sizeof(inline_tmvp_data));
    VASliceParameterBufferHEVC zero_tmvp_slice = inline_tmvp_slice;
    zero_tmvp_slice.slice_data_size = (uint32_t)zero_tmvp_data_size;
    zero_tmvp_slice.slice_data_byte_offset = (uint32_t)zero_tmvp_data_size;
    if (!zero_tmvp_data_size || !hobot_hevc_validated_rps_slice_supported(
            &inline_tmvp_picture, &zero_tmvp_slice, inline_tmvp_data,
            zero_tmvp_data_size, &sequence)) {
        fprintf(stderr, "valid zero-RPS inline syntax was rejected\n");
        free(tmvp_rewritten_nal);
        free(inline_tmvp_rewritten_nal);
        return 0;
    }
    if (!hobot_hevc_validated_rps_slice_supported(
            &inline_tmvp_picture, &inline_tmvp_slice, tmvp_rewritten_nal,
            tmvp_rewritten_nal_size, &sequence)) {
        fprintf(stderr, "rewritten two-RPS temporal-MVP inline syntax was invalid\n");
        free(tmvp_rewritten_nal);
        free(inline_tmvp_rewritten_nal);
        return 0;
    }
    inline_tmvp_slice.slice_data_size =
        (uint32_t)inline_tmvp_rewritten_nal_size;
    inline_tmvp_slice.slice_data_byte_offset =
        (uint32_t)inline_tmvp_rewritten_nal_size;
    if (!hobot_hevc_validated_rps_slice_supported(
            &inline_tmvp_picture, &inline_tmvp_slice,
            inline_tmvp_rewritten_nal, inline_tmvp_rewritten_nal_size,
            &sequence) ||
        inline_tmvp_rewritten_nal_size != zero_tmvp_data_size ||
        memcmp(inline_tmvp_rewritten_nal, inline_tmvp_data,
               zero_tmvp_data_size) != 0 ||
        tmvp_rewritten_nal_size != inline_tmvp_rewritten_nal_size ||
        memcmp(tmvp_rewritten_nal, inline_tmvp_rewritten_nal,
               tmvp_rewritten_nal_size) != 0) {
        fprintf(stderr, "selected and inline two-RPS rewrites did not produce the expected zero-RPS header\n");
        free(tmvp_rewritten_nal);
        free(inline_tmvp_rewritten_nal);
        return 0;
    }
    free(tmvp_rewritten_nal);
    free(inline_tmvp_rewritten_nal);
    slice.LongSliceFlags.fields.slice_temporal_mvp_enabled_flag = 0;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, two_rps_sao_offset, &sequence)) {
        fprintf(stderr, "two-RPS P slice accepted mismatched temporal MVP metadata\n");
        return 0;
    }
    slice.LongSliceFlags.fields.slice_temporal_mvp_enabled_flag = 1;
    slice.LongSliceFlags.fields.collocated_from_l0_flag = 0;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, two_rps_sao_offset, &sequence)) {
        fprintf(stderr, "two-RPS P slice accepted an invalid inferred collocated list\n");
        return 0;
    }
    slice.LongSliceFlags.fields.collocated_from_l0_flag = 1;
    slice.collocated_ref_idx = 1;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, two_rps_sao_offset, &sequence)) {
        fprintf(stderr, "two-RPS P slice accepted an invalid collocated reference index\n");
        return 0;
    }

    slice.collocated_ref_idx = 0;
    slice.LongSliceFlags.fields.slice_type = 0;
    slice.RefPicList[1][0] = 3;
    two_rps_sao_offset = test_build_two_rps_sao_slice(
        p_data, sizeof(p_data), 0, 1, 1, 1, 1);
    slice.slice_data_size = (uint32_t)two_rps_sao_offset;
    slice.slice_data_byte_offset = (uint32_t)two_rps_sao_offset;
    if (!two_rps_sao_offset || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, two_rps_sao_offset, &sequence)) {
        fprintf(stderr, "valid two-RPS B slice with temporal MVP was rejected\n");
        return 0;
    }
    slice.LongSliceFlags.fields.collocated_from_l0_flag = 0;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, two_rps_sao_offset, &sequence)) {
        fprintf(stderr, "two-RPS B slice accepted mismatched collocated-list metadata\n");
        return 0;
    }
    slice.LongSliceFlags.fields.collocated_from_l0_flag = 1;
    slice.LongSliceFlags.fields.slice_type = 1;
    slice.RefPicList[1][0] = 0xff;
    picture.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag = 0;
    picture.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = 0;
    slice.LongSliceFlags.fields.slice_temporal_mvp_enabled_flag = 0;
    slice.LongSliceFlags.fields.slice_sao_luma_flag = 0;
    slice.LongSliceFlags.fields.slice_sao_chroma_flag = 0;
    slice.slice_data_size = sizeof(p_data);

    picture.pic_fields.bits.weighted_pred_flag = 1;
    size_t weighted_p_offset = test_build_two_rps_weighted_slice(
        p_data, sizeof(p_data), 1, 1, 0, 0, 0, 0, 0, -1, 2, 0, 0, 0, 0,
        0, 0, 0, 0);
    slice.slice_data_byte_offset = (uint32_t)weighted_p_offset;
    if (weighted_p_offset == 0 || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, sizeof(p_data), &sequence)) {
        fprintf(stderr, "valid weighted single-reference P slice was rejected\n");
        return 0;
    }
    slice.slice_data_byte_offset = (uint32_t)(weighted_p_offset - 1u);
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, sizeof(p_data), &sequence)) {
        fprintf(stderr, "weighted P slice header was accepted beyond its byte boundary\n");
        return 0;
    }
    weighted_p_offset = test_build_two_rps_weighted_slice(
        p_data, sizeof(p_data), 1, 1, 0, 0, 0, 0, 0, 128, 0, 0, 0, 0, 0,
        0, 0, 0, 0);
    slice.slice_data_byte_offset = (uint32_t)weighted_p_offset;
    if (weighted_p_offset == 0 || hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, sizeof(p_data), &sequence)) {
        fprintf(stderr, "weighted P slice accepted an out-of-range luma weight\n");
        return 0;
    }
    weighted_p_offset = test_build_two_rps_weighted_slice(
        p_data, sizeof(p_data), 1, 1, 0, 0, 0, 0, 0, -1, 2, 0, 0, 1, 0,
        0, 8, 0, 0);
    slice.slice_data_byte_offset = (uint32_t)weighted_p_offset;
    if (weighted_p_offset == 0 || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, sizeof(p_data), &sequence)) {
        fprintf(stderr, "valid two-RPS weighted P chroma table was rejected\n");
        return 0;
    }
    weighted_p_offset = test_build_two_rps_weighted_slice(
        p_data, sizeof(p_data), 1, 1, 0, 0, 0, 0, 0, -1, 2, 0, 0, 1, 0,
        127, 511, 0, 0);
    slice.slice_data_byte_offset = (uint32_t)weighted_p_offset;
    if (weighted_p_offset == 0 || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, sizeof(p_data), &sequence)) {
        fprintf(stderr, "two-RPS weighted P rejected maximum legal chroma values\n");
        return 0;
    }
    weighted_p_offset = test_build_two_rps_weighted_slice(
        p_data, sizeof(p_data), 1, 1, 0, 0, 0, 0, 0, -1, 2, 0, 0, 1, 0,
        -128, -512, 0, 0);
    slice.slice_data_byte_offset = (uint32_t)weighted_p_offset;
    if (weighted_p_offset == 0 || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, sizeof(p_data), &sequence)) {
        fprintf(stderr, "two-RPS weighted P rejected minimum legal chroma values\n");
        return 0;
    }
    weighted_p_offset = test_build_two_rps_weighted_slice(
        p_data, sizeof(p_data), 1, 1, 0, 0, 0, 0, 0, -1, 2, 0, 0, 1, 0,
        128, 0, 0, 0);
    slice.slice_data_byte_offset = (uint32_t)weighted_p_offset;
    if (weighted_p_offset == 0 || hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, sizeof(p_data), &sequence)) {
        fprintf(stderr, "two-RPS weighted P accepted out-of-range chroma weight\n");
        return 0;
    }
    weighted_p_offset = test_build_two_rps_weighted_slice(
        p_data, sizeof(p_data), 1, 1, 0, 0, 0, 0, 0, -1, 2, 0, 0, 1, 0,
        0, 512, 0, 0);
    slice.slice_data_byte_offset = (uint32_t)weighted_p_offset;
    if (weighted_p_offset == 0 || hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, sizeof(p_data), &sequence)) {
        fprintf(stderr, "two-RPS weighted P accepted out-of-range chroma offset\n");
        return 0;
    }
    weighted_p_offset = test_build_two_rps_weighted_slice(
        p_data, sizeof(p_data), 1, 1, 0, 0, 0, 0, 0, -1, 2, 0, 0, 1, 0,
        -129, 0, 0, 0);
    slice.slice_data_byte_offset = (uint32_t)weighted_p_offset;
    if (weighted_p_offset == 0 || hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, sizeof(p_data), &sequence)) {
        fprintf(stderr, "two-RPS weighted P accepted under-range chroma weight\n");
        return 0;
    }
    weighted_p_offset = test_build_two_rps_weighted_slice(
        p_data, sizeof(p_data), 1, 1, 0, 0, 0, 0, 0, -1, 2, 0, 0, 1, 0,
        0, -513, 0, 0);
    slice.slice_data_byte_offset = (uint32_t)weighted_p_offset;
    if (weighted_p_offset == 0 || hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, sizeof(p_data), &sequence)) {
        fprintf(stderr, "two-RPS weighted P accepted under-range chroma offset\n");
        return 0;
    }
    weighted_p_offset = test_build_two_rps_weighted_slice(
        p_data, sizeof(p_data), 1, 1, 0, 0, 0, 0, 0, -1, 2, 0, 0, 0, 0,
        0, 0, 0, 0);
    slice.slice_data_byte_offset = (uint32_t)weighted_p_offset;
    picture.pic_fields.bits.weighted_pred_flag = 0;

    p_data[3] |= 0x02;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, sizeof(p_data), &sequence)) {
        fprintf(stderr, "HEVC slice with inconsistent SPS RPS index and references was accepted\n");
        return 0;
    }
    p_data[3] &= (uint8_t)~0x02u;
    picture.ReferenceFrames[3].pic_order_cnt = -1;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, p_data, sizeof(p_data), &sequence)) {
        fprintf(stderr, "HEVC RPS accepted a non-consecutive reference POC\n");
        return 0;
    }
    return 1;
}

static size_t test_build_indexed_sps_rps_slice(
    uint8_t *data,
    size_t capacity,
    unsigned int index,
    unsigned int index_bits
)
{
    if (!data || capacity < 8 || index_bits == 0 || index_bits > 6)
        return 0;

    memset(data, 0, capacity);
    data[0] = 0x02;
    data[1] = 0x01;
    BitWriter writer = {data + 2, 0};
    bw_put_bit(&writer, 1);
    bw_put_ue(&writer, 0);
    bw_put_ue(&writer, 1);
    bw_put_bits(&writer, 1, 8);
    bw_put_bit(&writer, 1);
    bw_put_bits(&writer, index, index_bits);
    bw_put_bit(&writer, 1);
    bw_put_bit(&writer, 1);
    bw_put_bit(&writer, 0);
    bw_put_ue(&writer, 0);
    bw_put_se(&writer, 0);
    bw_put_bit(&writer, 1);
    while (writer.bit_pos % 8u)
        bw_put_bit(&writer, 0);

    return 2u + writer.bit_pos / 8u;
}

static int test_hevc_three_sps_rps_index_rewrite(void)
{
    VAPictureParameterBufferHEVC picture = {0};
    picture.CurrPic.picture_id = 1;
    picture.CurrPic.pic_order_cnt = 1;
    picture.pic_width_in_luma_samples = 640;
    picture.pic_height_in_luma_samples = 360;
    picture.log2_max_pic_order_cnt_lsb_minus4 = 4;
    picture.log2_diff_max_min_luma_coding_block_size = 3;
    picture.log2_diff_max_min_transform_block_size = 3;
    picture.num_short_term_ref_pic_sets = 3;
    picture.pic_fields.bits.chroma_format_idc = 1;
    picture.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = 1;
    picture.sps_max_dec_pic_buffering_minus1 = 3;
    for (size_t i = 0; i < 15; i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    picture.ReferenceFrames[3].picture_id = 7;
    picture.ReferenceFrames[3].pic_order_cnt = 0;
    picture.ReferenceFrames[3].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;

    HobotHevcSliceSequence sequence;
    if (!hobot_hevc_slice_sequence_init(&picture, 1, &sequence))
        return 0;

    uint8_t data[64];
    size_t data_size = test_build_indexed_sps_rps_slice(
        data, sizeof(data), 2, 2);
    VASliceParameterBufferHEVC slice = {0};
    slice.slice_data_size = (uint32_t)data_size;
    slice.slice_data_byte_offset = (uint32_t)data_size;
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    slice.LongSliceFlags.fields.LastSliceOfPic = 1;
    slice.LongSliceFlags.fields.slice_type = 1;
    slice.LongSliceFlags.fields.slice_sao_luma_flag = 1;
    slice.LongSliceFlags.fields.slice_sao_chroma_flag = 1;
    slice.RefPicList[0][0] = 3;
    memset(&slice.RefPicList[0][1], 0xff,
           sizeof(slice.RefPicList[0]) - sizeof(slice.RefPicList[0][0]));
    memset(slice.RefPicList[1], 0xff, sizeof(slice.RefPicList[1]));
    if (!data_size || !hobot_hevc_picture_parameters_supported(
            &picture, 640, 360) ||
        !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, data, data_size, &sequence)) {
        fprintf(stderr, "valid three-set HEVC SPS RPS index was rejected\n");
        return 0;
    }

    uint8_t *rewritten = NULL;
    size_t rewritten_size = 0;
    if (hobot_hevc_rewrite_rps_slice(
            &picture, &slice, data, data_size,
            &rewritten, &rewritten_size) != 1 || !rewritten) {
        fprintf(stderr, "three-set HEVC SPS RPS was not rewritten inline\n");
        free(rewritten);
        return 0;
    }

    VAPictureParameterBufferHEVC inline_picture = picture;
    inline_picture.num_short_term_ref_pic_sets = 0;
    inline_picture.st_rps_bits = 6;
    VASliceParameterBufferHEVC inline_slice = slice;
    inline_slice.slice_data_size = (uint32_t)rewritten_size;
    inline_slice.slice_data_byte_offset = (uint32_t)rewritten_size;
    if (!hobot_hevc_validated_rps_slice_supported(
            &inline_picture, &inline_slice, rewritten,
            rewritten_size, &sequence)) {
        fprintf(stderr, "three-set RPS rewrite did not produce valid inline syntax\n");
        free(rewritten);
        return 0;
    }
    free(rewritten);

    uint8_t inline_data[64];
    size_t inline_size = test_build_two_rps_inline_tmvp_slice(
        inline_data, sizeof(inline_data));
    VAPictureParameterBufferHEVC three_set_inline_picture = picture;
    three_set_inline_picture.st_rps_bits = 7;
    three_set_inline_picture.slice_parsing_fields.bits
        .sps_temporal_mvp_enabled_flag = 1;
    VASliceParameterBufferHEVC three_set_inline_slice = slice;
    three_set_inline_slice.slice_data_size = (uint32_t)inline_size;
    three_set_inline_slice.slice_data_byte_offset = (uint32_t)inline_size;
    three_set_inline_slice.LongSliceFlags.fields
        .slice_temporal_mvp_enabled_flag = 1;
    three_set_inline_slice.LongSliceFlags.fields.collocated_from_l0_flag = 1;
    uint8_t *inline_rewritten = NULL;
    size_t inline_rewritten_size = 0;
    if (!inline_size || !hobot_hevc_validated_rps_slice_supported(
            &three_set_inline_picture, &three_set_inline_slice,
            inline_data, inline_size, &sequence) ||
        hobot_hevc_rewrite_rps_slice(
            &three_set_inline_picture, &three_set_inline_slice,
            inline_data, inline_size, &inline_rewritten,
            &inline_rewritten_size) != 1 || !inline_rewritten) {
        fprintf(stderr, "valid inline RPS with three SPS sets was rejected or not rewritten\n");
        free(inline_rewritten);
        return 0;
    }
    VAPictureParameterBufferHEVC rewritten_inline_picture =
        three_set_inline_picture;
    rewritten_inline_picture.num_short_term_ref_pic_sets = 0;
    rewritten_inline_picture.st_rps_bits = 6;
    three_set_inline_slice.slice_data_size = (uint32_t)inline_rewritten_size;
    three_set_inline_slice.slice_data_byte_offset =
        (uint32_t)inline_rewritten_size;
    if (!hobot_hevc_validated_rps_slice_supported(
            &rewritten_inline_picture, &three_set_inline_slice,
            inline_rewritten, inline_rewritten_size, &sequence)) {
        fprintf(stderr, "rewritten three-set inline RPS was invalid under the synthetic SPS\n");
        free(inline_rewritten);
        return 0;
    }
    free(inline_rewritten);

    data_size = test_build_indexed_sps_rps_slice(
        data, sizeof(data), 3, 2);
    slice.slice_data_size = (uint32_t)data_size;
    slice.slice_data_byte_offset = (uint32_t)data_size;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, data, data_size, &sequence)) {
        fprintf(stderr, "out-of-range three-set HEVC SPS RPS index was accepted\n");
        return 0;
    }

    picture.num_short_term_ref_pic_sets = 64;
    data_size = test_build_indexed_sps_rps_slice(data, sizeof(data), 63, 6);
    slice.slice_data_size = (uint32_t)data_size;
    slice.slice_data_byte_offset = (uint32_t)data_size;
    if (!hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, data, data_size, &sequence)) {
        fprintf(stderr, "valid maximum-count HEVC SPS RPS index was rejected\n");
        return 0;
    }

    picture.num_short_term_ref_pic_sets = 63;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, data, data_size, &sequence)) {
        fprintf(stderr, "out-of-range maximum-width HEVC SPS RPS index was accepted\n");
        return 0;
    }
    return 1;
}

static int test_hevc_two_rps_multislice_p_validation(void)
{
    VAPictureParameterBufferHEVC picture = {0};
    picture.CurrPic.picture_id = 1;
    picture.CurrPic.pic_order_cnt = 1;
    picture.pic_width_in_luma_samples = 640;
    picture.pic_height_in_luma_samples = 360;
    picture.log2_diff_max_min_luma_coding_block_size = 3;
    picture.log2_max_pic_order_cnt_lsb_minus4 = 4;
    picture.num_short_term_ref_pic_sets = 2;
    picture.pic_fields.bits.chroma_format_idc = 1;
    picture.ReferenceFrames[3].picture_id = 7;
    picture.ReferenceFrames[3].pic_order_cnt = 0;
    picture.ReferenceFrames[3].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    for (size_t i = 0; i < 15; i++) {
        if (i != 3) {
            picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
            picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
        }
    }

    uint8_t first_data[16] = {0x02, 0x01};
    BitWriter first_writer = {first_data + 2, 0};
    bw_put_bit(&first_writer, 1);
    bw_put_ue(&first_writer, 0);
    bw_put_ue(&first_writer, 1);
    bw_put_bits(&first_writer, 1, 8);
    bw_put_bit(&first_writer, 1);
    bw_put_bit(&first_writer, 0);
    bw_put_bit(&first_writer, 0);
    bw_put_ue(&first_writer, 0);
    bw_put_se(&first_writer, 0);
    bw_put_bit(&first_writer, 1);
    while (first_writer.bit_pos % 8)
        bw_put_bit(&first_writer, 0);

    uint8_t second_data[16] = {0x02, 0x01};
    BitWriter second_writer = {second_data + 2, 0};
    bw_put_bit(&second_writer, 0);
    bw_put_ue(&second_writer, 0);
    bw_put_bits(&second_writer, 30, 6);
    bw_put_ue(&second_writer, 1);
    bw_put_bits(&second_writer, 1, 8);
    bw_put_bit(&second_writer, 1);
    bw_put_bit(&second_writer, 0);
    bw_put_bit(&second_writer, 0);
    bw_put_ue(&second_writer, 0);
    bw_put_se(&second_writer, 0);
    bw_put_bit(&second_writer, 1);
    while (second_writer.bit_pos % 8)
        bw_put_bit(&second_writer, 0);

    VASliceParameterBufferHEVC slices[2] = {{0}};
    for (size_t i = 0; i < 2; i++) {
        memset(slices[i].RefPicList, 0xff, sizeof(slices[i].RefPicList));
        slices[i].slice_data_size = sizeof(first_data);
        slices[i].slice_data_byte_offset = 2 +
            (i == 0 ? first_writer.bit_pos : second_writer.bit_pos) / 8;
        slices[i].slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        slices[i].RefPicList[0][0] = 3;
        slices[i].LongSliceFlags.fields.slice_type = 1;
    }
    slices[1].slice_segment_address = 30;
    slices[1].LongSliceFlags.fields.LastSliceOfPic = 1;

    HobotHevcSliceSequence sequence;
    if (!hobot_hevc_slice_sequence_init(&picture, 2, &sequence) ||
        !hobot_hevc_slice_sequence_add(&sequence, &slices[0], sizeof(first_data)) ||
        !hobot_hevc_slice_sequence_add(&sequence, &slices[1], sizeof(second_data)) ||
        !hobot_hevc_slice_sequence_complete(&sequence) ||
        !hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence) ||
        !hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[1], second_data, sizeof(second_data), &sequence)) {
        fprintf(stderr, "valid two-slice single-reference P picture was rejected\n");
        return 0;
    }

    slices[1].slice_segment_address = 29;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[1], second_data, sizeof(second_data), &sequence)) {
        fprintf(stderr, "HEVC P slice accepted a CTU address inconsistent with its NAL header\n");
        return 0;
    }
    slices[1].slice_segment_address = 30;

    second_data[4] |= 0x08;
    if (!hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[1], second_data, sizeof(second_data), &sequence)) {
        fprintf(stderr, "valid HEVC P slice selecting SPS RPS index 1 was rejected\n");
        return 0;
    }
    return 1;
}

static int test_hevc_rasl_nal_type_validation(void)
{
    VAPictureParameterBufferHEVC picture = {0};
    picture.log2_max_pic_order_cnt_lsb_minus4 = 4;
    picture.pic_fields.bits.chroma_format_idc = 1;
    picture.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;
    picture.CurrPic.picture_id = 1;
    picture.CurrPic.pic_order_cnt = 1;
    picture.slice_parsing_fields.bits.IntraPicFlag = 0;
    HobotHevcSliceSequence sequence = {
        .picture_ctb_count = 1,
        .slice_count = 1,
    };
    for (size_t i = 0; i < 15; i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    picture.ReferenceFrames[0].picture_id = 2;
    picture.ReferenceFrames[0].pic_order_cnt = 0;
    picture.ReferenceFrames[0].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;

    TestHevcInlineSliceOptions options = {0};
    uint8_t data[64];
    uint32_t rps_bits;
    size_t data_size = test_build_inline_rps_slice(
        data, sizeof(data), 1, 0, 1, 0, 0, 1, 0, &options, &rps_bits);
    if (!data_size)
        return 0;
    picture.st_rps_bits = rps_bits;

    VASliceParameterBufferHEVC slice = {0};
    memset(slice.RefPicList, 0xff, sizeof(slice.RefPicList));
    slice.slice_data_size = (uint32_t)data_size;
    slice.slice_data_byte_offset = (uint32_t)data_size;
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    slice.LongSliceFlags.fields.LastSliceOfPic = 1;
    slice.LongSliceFlags.fields.slice_type = 1;
    slice.LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag = 1;
    slice.RefPicList[0][0] = 0;

    const uint8_t rasl_nal_types[] = {8, 9};
    for (size_t i = 0; i < sizeof(rasl_nal_types); i++) {
        data[0] = (uint8_t)(rasl_nal_types[i] << 1);
        if (!hobot_hevc_validated_rps_slice_supported(
                &picture, &slice, data, data_size, &sequence)) {
            fprintf(stderr, "valid RASL NAL type %u was rejected\n",
                    rasl_nal_types[i]);
            return 0;
        }
    }

    const uint8_t radl_nal_types[] = {6, 7};
    for (size_t i = 0; i < sizeof(radl_nal_types); i++) {
        data[0] = (uint8_t)(radl_nal_types[i] << 1);
        if (hobot_hevc_validated_rps_slice_supported(
                &picture, &slice, data, data_size, &sequence)) {
            fprintf(stderr, "unsupported RADL NAL type %u was accepted\n",
                    radl_nal_types[i]);
            return 0;
        }
    }
    return 1;
}

static int test_hevc_two_rps_b_sao_validation(void)
{
    VAPictureParameterBufferHEVC picture = {0};
    picture.CurrPic.picture_id = 1;
    picture.CurrPic.pic_order_cnt = 1;
    picture.pic_width_in_luma_samples = 640;
    picture.pic_height_in_luma_samples = 360;
    picture.log2_diff_max_min_luma_coding_block_size = 3;
    picture.log2_max_pic_order_cnt_lsb_minus4 = 4;
    picture.num_short_term_ref_pic_sets = 2;
    picture.pic_fields.bits.chroma_format_idc = 1;
    picture.slice_parsing_fields.bits.cabac_init_present_flag = 1;
    picture.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = 1;
    picture.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;
    for (size_t i = 0; i < 15; i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    picture.ReferenceFrames[3].picture_id = 7;
    picture.ReferenceFrames[3].pic_order_cnt = 0;
    picture.ReferenceFrames[3].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;

    uint8_t data[32] = {0x02, 0x01};
    BitWriter writer = {data + 2, 0};
    bw_put_bit(&writer, 1);       /* first slice */
    bw_put_ue(&writer, 0);        /* PPS id */
    bw_put_ue(&writer, 0);        /* B slice */
    bw_put_bits(&writer, 1, 8);   /* POC LSB */
    bw_put_bit(&writer, 1);       /* select an SPS RPS */
    bw_put_bit(&writer, 0);       /* select SPS RPS index 0 */
    bw_put_bit(&writer, 1);       /* SAO luma */
    bw_put_bit(&writer, 1);       /* SAO chroma */
    bw_put_bit(&writer, 0);       /* no active-reference override */
    bw_put_bit(&writer, 1);       /* mvd_l1_zero_flag */
    bw_put_bit(&writer, 0);       /* cabac_init_flag */
    bw_put_ue(&writer, 0);        /* five_minus_max_num_merge_cand */
    bw_put_se(&writer, 6);        /* slice_qp_delta */
    bw_put_bit(&writer, 1);       /* loop_filter_across_slices */
    bw_put_bit(&writer, 1);       /* byte_alignment() */
    while (writer.bit_pos % 8u)
        bw_put_bit(&writer, 0);
    size_t data_size = 2u + writer.bit_pos / 8u;

    VASliceParameterBufferHEVC slice = {0};
    memset(slice.RefPicList, 0xff, sizeof(slice.RefPicList));
    slice.slice_data_size = (uint32_t)data_size;
    slice.slice_data_byte_offset = (uint32_t)data_size;
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    slice.RefPicList[0][0] = 3;
    slice.RefPicList[1][0] = 3;
    slice.LongSliceFlags.fields.LastSliceOfPic = 1;
    slice.LongSliceFlags.fields.slice_type = 0;
    slice.LongSliceFlags.fields.slice_sao_luma_flag = 1;
    slice.LongSliceFlags.fields.slice_sao_chroma_flag = 1;
    slice.LongSliceFlags.fields.mvd_l1_zero_flag = 1;
    slice.LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag = 1;
    slice.slice_qp_delta = 6;

    HobotHevcSliceSequence sequence;
    if (!hobot_hevc_slice_sequence_init(&picture, 1, &sequence) ||
        !hobot_hevc_slice_sequence_add(&sequence, &slice, data_size) ||
        !hobot_hevc_slice_sequence_complete(&sequence) ||
        !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, data, data_size, &sequence)) {
        fprintf(stderr, "valid two-RPS B slice with luma/chroma SAO was rejected\n");
        return 0;
    }

    slice.LongSliceFlags.fields.slice_sao_luma_flag = 0;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, data, data_size, &sequence)) {
        fprintf(stderr, "two-RPS B slice accepted mismatched luma SAO metadata\n");
        return 0;
    }
    slice.LongSliceFlags.fields.slice_sao_luma_flag = 1;
    slice.LongSliceFlags.fields.slice_sao_chroma_flag = 0;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, data, data_size, &sequence)) {
        fprintf(stderr, "two-RPS B slice accepted mismatched chroma SAO metadata\n");
        return 0;
    }
    slice.slice_data_byte_offset = (uint32_t)(data_size - 1u);
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, data, data_size, &sequence)) {
        fprintf(stderr, "two-RPS B SAO header crossed its data boundary\n");
        return 0;
    }
    return 1;
}

static int test_hevc_two_rps_multireference_list_modification(void)
{
    VAPictureParameterBufferHEVC picture = {0};
    picture.CurrPic.picture_id = 1;
    picture.CurrPic.pic_order_cnt = 4;
    picture.pic_width_in_luma_samples = 640;
    picture.pic_height_in_luma_samples = 360;
    picture.log2_diff_max_min_luma_coding_block_size = 3;
    picture.log2_max_pic_order_cnt_lsb_minus4 = 4;
    picture.num_short_term_ref_pic_sets = 2;
    picture.num_ref_idx_l0_default_active_minus1 = 3;
    picture.num_ref_idx_l1_default_active_minus1 = 3;
    picture.pic_fields.bits.chroma_format_idc = 1;
    picture.slice_parsing_fields.bits.cabac_init_present_flag = 1;
    picture.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = 1;
    picture.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag = 1;
    picture.slice_parsing_fields.bits.lists_modification_present_flag = 1;
    picture.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;

    const uint8_t ref_slots[] = {2, 5, 7, 9};
    for (size_t i = 0; i < 15; i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    for (size_t i = 0; i < sizeof(ref_slots); i++) {
        VAPictureHEVC *ref = &picture.ReferenceFrames[ref_slots[i]];
        ref->picture_id = (VASurfaceID)(10u + i);
        ref->pic_order_cnt = (int32_t)(3u - i);
        ref->flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    }

    uint8_t data[64] = {0x02, 0x01};
    BitWriter writer = {data + 2, 0};
    bw_put_bit(&writer, 1);       /* first slice */
    bw_put_ue(&writer, 0);        /* PPS id */
    bw_put_ue(&writer, 0);        /* B slice */
    bw_put_bits(&writer, 4, 8);   /* POC LSB */
    bw_put_bit(&writer, 1);       /* select an SPS RPS */
    bw_put_bit(&writer, 0);       /* select SPS RPS index 0 */
    bw_put_bit(&writer, 1);       /* temporal MVP enabled */
    bw_put_bit(&writer, 1);       /* SAO luma */
    bw_put_bit(&writer, 1);       /* SAO chroma */
    bw_put_bit(&writer, 0);       /* no active-reference override */
    bw_put_bit(&writer, 1);       /* modify L0 */
    bw_put_bits(&writer, 2, 2);
    bw_put_bits(&writer, 0, 2);
    bw_put_bits(&writer, 3, 2);
    bw_put_bits(&writer, 1, 2);
    bw_put_bit(&writer, 1);       /* modify L1 */
    bw_put_bits(&writer, 3, 2);
    bw_put_bits(&writer, 1, 2);
    bw_put_bits(&writer, 0, 2);
    bw_put_bits(&writer, 2, 2);
    bw_put_bit(&writer, 1);       /* mvd_l1_zero_flag */
    bw_put_bit(&writer, 0);       /* cabac_init_flag */
    bw_put_bit(&writer, 0);       /* collocated_from_l0_flag */
    bw_put_ue(&writer, 2);        /* collocated_ref_idx */
    bw_put_ue(&writer, 0);        /* five_minus_max_num_merge_cand */
    bw_put_se(&writer, 0);        /* slice_qp_delta */
    bw_put_bit(&writer, 1);       /* loop_filter_across_slices */
    bw_put_bit(&writer, 1);       /* byte_alignment() */
    while (writer.bit_pos % 8u)
        bw_put_bit(&writer, 0);
    size_t data_size = 2u + writer.bit_pos / 8u;

    VASliceParameterBufferHEVC slice = {0};
    memset(slice.RefPicList, 0xff, sizeof(slice.RefPicList));
    slice.slice_data_size = (uint32_t)data_size;
    slice.slice_data_byte_offset = (uint32_t)data_size;
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    slice.num_ref_idx_l0_active_minus1 = 3;
    slice.num_ref_idx_l1_active_minus1 = 3;
    slice.RefPicList[0][0] = ref_slots[2];
    slice.RefPicList[0][1] = ref_slots[0];
    slice.RefPicList[0][2] = ref_slots[3];
    slice.RefPicList[0][3] = ref_slots[1];
    slice.RefPicList[1][0] = ref_slots[3];
    slice.RefPicList[1][1] = ref_slots[1];
    slice.RefPicList[1][2] = ref_slots[0];
    slice.RefPicList[1][3] = ref_slots[2];
    slice.LongSliceFlags.fields.LastSliceOfPic = 1;
    slice.LongSliceFlags.fields.slice_type = 0;
    slice.LongSliceFlags.fields.slice_temporal_mvp_enabled_flag = 1;
    slice.LongSliceFlags.fields.slice_sao_luma_flag = 1;
    slice.LongSliceFlags.fields.slice_sao_chroma_flag = 1;
    slice.LongSliceFlags.fields.mvd_l1_zero_flag = 1;
    slice.LongSliceFlags.fields.collocated_from_l0_flag = 0;
    slice.LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag = 1;
    slice.collocated_ref_idx = 2;

    HobotHevcSliceSequence sequence;
    if (!hobot_hevc_slice_sequence_init(&picture, 1, &sequence) ||
        !hobot_hevc_slice_sequence_add(&sequence, &slice, data_size) ||
        !hobot_hevc_slice_sequence_complete(&sequence) ||
        !hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, data, data_size, &sequence)) {
        fprintf(stderr, "valid four-reference B slice with L0/L1 reordering was rejected\n");
        return 0;
    }

    slice.RefPicList[0][0] = ref_slots[0];
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, data, data_size, &sequence)) {
        fprintf(stderr, "two-RPS B slice accepted a mismatched reordered L0 list\n");
        return 0;
    }
    slice.RefPicList[0][0] = ref_slots[2];

    picture.ReferenceFrames[12].picture_id = 20;
    picture.ReferenceFrames[12].pic_order_cnt = -1;
    picture.ReferenceFrames[12].flags = 0;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slice, data, data_size, &sequence)) {
        fprintf(stderr, "selected SPS RPS accepted an unlisted retained DPB entry\n");
        return 0;
    }
    picture.ReferenceFrames[12].picture_id = VA_INVALID_SURFACE;
    picture.ReferenceFrames[12].flags = VA_PICTURE_HEVC_INVALID;

    uint8_t *rewritten = NULL;
    size_t rewritten_size = 0;
    if (hobot_hevc_rewrite_rps_slice(
            &picture, &slice, data, data_size, &rewritten, &rewritten_size) != 1 ||
        !rewritten || rewritten_size < 3u) {
        fprintf(stderr, "four-reference SPS-RPS slice rewrite failed\n");
        free(rewritten);
        return 0;
    }

    uint8_t rbsp[256];
    size_t rbsp_size = 0;
    BitReader rewritten_reader;
    uint32_t value;
    if (!hobot_hevc_unescape_rbsp(rewritten + 2, rewritten_size - 2u,
                                  rbsp, sizeof(rbsp), &rbsp_size)) {
        fprintf(stderr, "rewritten four-reference RBSP was invalid\n");
        free(rewritten);
        return 0;
    }
    rewritten_reader = (BitReader){rbsp, rbsp_size * 8u, 0};
    uint32_t rps_start, negative_count, positive_count;
    if (!br_skip_bits(&rewritten_reader, 1) ||
        !br_read_ue(&rewritten_reader, &value, NULL, NULL) || value != 0 ||
        !br_read_ue(&rewritten_reader, &value, NULL, NULL) || value != 0 ||
        !br_read_bits(&rewritten_reader, 8, &value) || value != 4 ||
        !br_read_bits(&rewritten_reader, 1, &value) || value != 0) {
        fprintf(stderr, "rewritten slice did not contain inline RPS syntax\n");
        free(rewritten);
        return 0;
    }
    rps_start = (uint32_t)rewritten_reader.bit_pos;
    if (!br_read_ue(&rewritten_reader, &negative_count, NULL, NULL) ||
        negative_count != 4 ||
        !br_read_ue(&rewritten_reader, &positive_count, NULL, NULL) ||
        positive_count != 0) {
        fprintf(stderr, "rewritten RPS lost the four negative references\n");
        free(rewritten);
        return 0;
    }
    for (uint32_t i = 0; i < negative_count; i++) {
        if (!br_read_ue(&rewritten_reader, &value, NULL, NULL) || value != 0 ||
            !br_read_bits(&rewritten_reader, 1, &value) || value != 1) {
            fprintf(stderr, "rewritten RPS corrupted reference %u delta/use flag\n", i);
            free(rewritten);
            return 0;
        }
    }
    uint32_t rps_bits = (uint32_t)(rewritten_reader.bit_pos - rps_start);
    if (!br_read_bits(&rewritten_reader, 1, &value) || value != 1 ||
        !br_read_bits(&rewritten_reader, 1, &value) || value != 1 ||
        !br_read_bits(&rewritten_reader, 1, &value) || value != 1 ||
        !br_read_bits(&rewritten_reader, 1, &value) || value != 0 ||
        !br_read_bits(&rewritten_reader, 1, &value) || value != 1 ||
        !br_read_bits(&rewritten_reader, 2, &value) || value != 2 ||
        !br_read_bits(&rewritten_reader, 2, &value) || value != 0 ||
        !br_read_bits(&rewritten_reader, 2, &value) || value != 3 ||
        !br_read_bits(&rewritten_reader, 2, &value) || value != 1 ||
        !br_read_bits(&rewritten_reader, 1, &value) || value != 1 ||
        !br_read_bits(&rewritten_reader, 2, &value) || value != 3 ||
        !br_read_bits(&rewritten_reader, 2, &value) || value != 1 ||
        !br_read_bits(&rewritten_reader, 2, &value) || value != 0 ||
        !br_read_bits(&rewritten_reader, 2, &value) || value != 2) {
        fprintf(stderr, "rewritten slice did not preserve both modified reference lists\n");
        free(rewritten);
        return 0;
    }

    int32_t rewritten_qp_delta;
    if (!br_read_bits(&rewritten_reader, 1, &value) || value != 1 ||
        !br_read_bits(&rewritten_reader, 1, &value) || value != 0 ||
        !br_read_bits(&rewritten_reader, 1, &value) || value != 0 ||
        !br_read_ue(&rewritten_reader, &value, NULL, NULL) || value != 2 ||
        !br_read_ue(&rewritten_reader, &value, NULL, NULL) || value != 0 ||
        !br_read_se(&rewritten_reader, &rewritten_qp_delta) ||
        rewritten_qp_delta != 0 ||
        !br_read_bits(&rewritten_reader, 1, &value) || value != 1 ||
        !br_read_bits(&rewritten_reader, 1, &value) || value != 1) {
        fprintf(stderr, "rewritten B-slice suffix syntax was corrupted\n");
        free(rewritten);
        return 0;
    }
    while (rewritten_reader.bit_pos % 8u) {
        if (!br_read_bits(&rewritten_reader, 1, &value) || value != 0) {
            fprintf(stderr, "rewritten B-slice alignment padding was invalid\n");
            free(rewritten);
            return 0;
        }
    }
    VAPictureParameterBufferHEVC inline_picture = picture;
    VASliceParameterBufferHEVC inline_slice = slice;
    inline_picture.num_short_term_ref_pic_sets = 0;
    inline_picture.st_rps_bits = rps_bits;
    inline_slice.slice_data_size = (uint32_t)rewritten_size;
    inline_slice.slice_data_byte_offset =
        (uint32_t)(2u + rewritten_reader.bit_pos / 8u);
    if (!hobot_hevc_validated_rps_slice_supported(
            &inline_picture, &inline_slice, rewritten, rewritten_size,
            &sequence)) {
        fprintf(stderr, "rewritten multi-reference inline B slice failed validation\n");
        free(rewritten);
        return 0;
    }
    free(rewritten);
    return 1;
}

static int test_hevc_two_rps_multislice_b_validation(void)
{
    VAPictureParameterBufferHEVC picture = {0};
    picture.CurrPic.picture_id = 1;
    picture.CurrPic.pic_order_cnt = 1;
    picture.pic_width_in_luma_samples = 640;
    picture.pic_height_in_luma_samples = 360;
    picture.log2_diff_max_min_luma_coding_block_size = 3;
    picture.log2_max_pic_order_cnt_lsb_minus4 = 4;
    picture.num_short_term_ref_pic_sets = 2;
    picture.pic_fields.bits.chroma_format_idc = 1;
    picture.slice_parsing_fields.bits.cabac_init_present_flag = 1;
    picture.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;
    picture.ReferenceFrames[3].picture_id = 7;
    picture.ReferenceFrames[3].pic_order_cnt = 0;
    picture.ReferenceFrames[3].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    for (size_t i = 0; i < 15; i++) {
        if (i != 3) {
            picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
            picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
        }
    }

    uint8_t first_data[64] = {0x02, 0x01};
    BitWriter first_writer = {first_data + 2, 0};
    bw_put_bit(&first_writer, 1);
    bw_put_ue(&first_writer, 0);
    bw_put_ue(&first_writer, 0);
    bw_put_bits(&first_writer, 1, 8);
    bw_put_bit(&first_writer, 1);
    bw_put_bit(&first_writer, 0);
    bw_put_bit(&first_writer, 0);
    bw_put_bit(&first_writer, 1);
    bw_put_bit(&first_writer, 0);
    bw_put_ue(&first_writer, 0);
    bw_put_se(&first_writer, 6);
    bw_put_bit(&first_writer, 1);
    bw_put_bit(&first_writer, 1);
    while (first_writer.bit_pos % 8)
        bw_put_bit(&first_writer, 0);

    uint8_t second_data[64] = {0x02, 0x01};
    BitWriter second_writer = {second_data + 2, 0};
    bw_put_bit(&second_writer, 0);
    bw_put_ue(&second_writer, 0);
    bw_put_bits(&second_writer, 30, 6);
    bw_put_ue(&second_writer, 0);
    bw_put_bits(&second_writer, 1, 8);
    bw_put_bit(&second_writer, 1);
    bw_put_bit(&second_writer, 0);
    bw_put_bit(&second_writer, 0);
    bw_put_bit(&second_writer, 1);
    bw_put_bit(&second_writer, 0);
    bw_put_ue(&second_writer, 0);
    bw_put_se(&second_writer, 6);
    bw_put_bit(&second_writer, 1);
    bw_put_bit(&second_writer, 1);
    while (second_writer.bit_pos % 8)
        bw_put_bit(&second_writer, 0);

    VASliceParameterBufferHEVC slices[2] = {{0}};
    for (size_t i = 0; i < 2; i++) {
        memset(slices[i].RefPicList, 0xff, sizeof(slices[i].RefPicList));
        slices[i].slice_data_size = sizeof(first_data);
        slices[i].slice_data_byte_offset = 2 +
            (i == 0 ? first_writer.bit_pos : second_writer.bit_pos) / 8;
        slices[i].slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        slices[i].RefPicList[0][0] = 3;
        slices[i].RefPicList[1][0] = 3;
        slices[i].LongSliceFlags.fields.slice_type = 0;
        slices[i].LongSliceFlags.fields.mvd_l1_zero_flag = 1;
        slices[i].LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag = 1;
        slices[i].slice_qp_delta = 6;
    }
    slices[1].slice_segment_address = 30;
    slices[1].LongSliceFlags.fields.LastSliceOfPic = 1;

    HobotHevcSliceSequence sequence;
    if (!hobot_hevc_slice_sequence_init(&picture, 2, &sequence) ||
        !hobot_hevc_slice_sequence_add(&sequence, &slices[0], sizeof(first_data)) ||
        !hobot_hevc_slice_sequence_add(&sequence, &slices[1], sizeof(second_data)) ||
        !hobot_hevc_slice_sequence_complete(&sequence) ||
        !hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence) ||
        !hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[1], second_data, sizeof(second_data), &sequence)) {
        fprintf(stderr, "valid two-slice single-reference B picture was rejected\n");
        return 0;
    }

    slices[0].RefPicList[1][0] = 4;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence)) {
        fprintf(stderr, "HEVC B slice accepted different L0/L1 references\n");
        return 0;
    }
    slices[0].RefPicList[1][0] = 3;

    slices[0].LongSliceFlags.fields.mvd_l1_zero_flag = 0;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence)) {
        fprintf(stderr, "HEVC B slice accepted mismatched mvd_l1_zero metadata\n");
        return 0;
    }
    slices[0].LongSliceFlags.fields.mvd_l1_zero_flag = 1;

    slices[0].LongSliceFlags.fields.cabac_init_flag = 1;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence)) {
        fprintf(stderr, "HEVC B slice accepted mismatched CABAC-init metadata\n");
        return 0;
    }
    slices[0].LongSliceFlags.fields.cabac_init_flag = 0;

    slices[0].slice_qp_delta = 5;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence)) {
        fprintf(stderr, "HEVC B slice accepted mismatched QP-delta metadata\n");
        return 0;
    }
    slices[0].slice_qp_delta = 6;

    slices[0].LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag = 0;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence)) {
        fprintf(stderr, "HEVC B slice accepted mismatched loop-filter metadata\n");
        return 0;
    }
    slices[0].LongSliceFlags.fields.slice_loop_filter_across_slices_enabled_flag = 1;

    picture.pic_fields.bits.weighted_bipred_flag = 1;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence)) {
        fprintf(stderr, "HEVC B slice was accepted with weighted bi-prediction\n");
        return 0;
    }
    picture.pic_fields.bits.weighted_bipred_flag = 0;

    picture.pic_fields.bits.NoBiPredFlag = 1;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence)) {
        fprintf(stderr, "HEVC B slice was accepted with NoBiPredFlag set\n");
        return 0;
    }
    picture.pic_fields.bits.NoBiPredFlag = 0;

    first_data[3] |= 0x04;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence)) {
        fprintf(stderr, "HEVC B slice accepted active-reference override\n");
        return 0;
    }
    first_data[3] &= (uint8_t)~0x04u;

    first_data[5] |= 0x01;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence)) {
        fprintf(stderr, "HEVC B slice accepted invalid byte-alignment padding\n");
        return 0;
    }

    picture.pic_fields.bits.weighted_bipred_flag = 1;
    size_t first_weighted_offset = test_build_two_rps_weighted_slice(
        first_data, sizeof(first_data), 0, 1, 0, 6, 1, 1, 6,
        -2, 3, 2, -1, 1, 1, 0, 8, 0, -8);
    size_t second_weighted_offset = test_build_two_rps_weighted_slice(
        second_data, sizeof(second_data), 0, 0, 30, 6, 1, 1, 6,
        -2, 3, 2, -1, 1, 1, 0, 8, 0, -8);
    slices[0].slice_data_byte_offset = (uint32_t)first_weighted_offset;
    slices[1].slice_data_byte_offset = (uint32_t)second_weighted_offset;
    int first_weighted_ok = first_weighted_offset != 0 &&
        hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence);
    int second_weighted_ok = second_weighted_offset != 0 &&
        hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[1], second_data, sizeof(second_data), &sequence);
    if (!first_weighted_ok || !second_weighted_ok) {
        fprintf(stderr, "valid weighted two-slice B picture was rejected (first=%d second=%d)\n",
                first_weighted_ok, second_weighted_ok);
        return 0;
    }

    slices[0].slice_data_byte_offset = (uint32_t)(first_weighted_offset - 1u);
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence)) {
        fprintf(stderr, "weighted B slice header was accepted beyond its byte boundary\n");
        return 0;
    }
    first_weighted_offset = test_build_two_rps_weighted_slice(
        first_data, sizeof(first_data), 0, 1, 0, 6, 1, 1, 6,
        -2, 3, 128, 0, 0, 0, 0, 0, 0, 0);
    slices[0].slice_data_byte_offset = (uint32_t)first_weighted_offset;
    if (first_weighted_offset == 0 || hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence)) {
        fprintf(stderr, "weighted B slice accepted an out-of-range L1 weight\n");
        return 0;
    }
    first_weighted_offset = test_build_two_rps_weighted_slice(
        first_data, sizeof(first_data), 0, 1, 0, 6, 1, 1, 6,
        -2, 3, 2, -1, 0, 1, 0, 0, 0, 8);
    slices[0].slice_data_byte_offset = (uint32_t)first_weighted_offset;
    if (first_weighted_offset == 0 || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence)) {
        fprintf(stderr, "valid two-RPS weighted B L1 chroma table was rejected\n");
        return 0;
    }
    first_weighted_offset = test_build_two_rps_weighted_slice(
        first_data, sizeof(first_data), 0, 1, 0, 6, 1, 1, 6,
        -2, 3, 2, -1, 0, 1, 0, 0, 127, 511);
    slices[0].slice_data_byte_offset = (uint32_t)first_weighted_offset;
    if (first_weighted_offset == 0 || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence)) {
        fprintf(stderr, "two-RPS weighted B rejected maximum legal L1 chroma values\n");
        return 0;
    }
    first_weighted_offset = test_build_two_rps_weighted_slice(
        first_data, sizeof(first_data), 0, 1, 0, 6, 1, 1, 6,
        -2, 3, 2, -1, 0, 1, 0, 0, -128, -512);
    slices[0].slice_data_byte_offset = (uint32_t)first_weighted_offset;
    if (first_weighted_offset == 0 || !hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence)) {
        fprintf(stderr, "two-RPS weighted B rejected minimum legal L1 chroma values\n");
        return 0;
    }
    first_weighted_offset = test_build_two_rps_weighted_slice(
        first_data, sizeof(first_data), 0, 1, 0, 6, 1, 1, 6,
        -2, 3, 2, -1, 0, 1, 0, 0, 128, 0);
    slices[0].slice_data_byte_offset = (uint32_t)first_weighted_offset;
    if (first_weighted_offset == 0 || hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence)) {
        fprintf(stderr, "two-RPS weighted B accepted out-of-range L1 chroma weight\n");
        return 0;
    }
    first_weighted_offset = test_build_two_rps_weighted_slice(
        first_data, sizeof(first_data), 0, 1, 0, 6, 1, 1, 6,
        -2, 3, 2, -1, 0, 1, 0, 0, 0, 512);
    slices[0].slice_data_byte_offset = (uint32_t)first_weighted_offset;
    if (first_weighted_offset == 0 || hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence)) {
        fprintf(stderr, "two-RPS weighted B accepted out-of-range L1 chroma offset\n");
        return 0;
    }
    first_weighted_offset = test_build_two_rps_weighted_slice(
        first_data, sizeof(first_data), 0, 1, 0, 6, 1, 1, 6,
        -2, 3, 2, -1, 0, 1, 0, 0, -129, 0);
    slices[0].slice_data_byte_offset = (uint32_t)first_weighted_offset;
    if (first_weighted_offset == 0 || hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence)) {
        fprintf(stderr, "two-RPS weighted B accepted under-range L1 chroma weight\n");
        return 0;
    }
    first_weighted_offset = test_build_two_rps_weighted_slice(
        first_data, sizeof(first_data), 0, 1, 0, 6, 1, 1, 6,
        -2, 3, 2, -1, 0, 1, 0, 0, 0, -513);
    slices[0].slice_data_byte_offset = (uint32_t)first_weighted_offset;
    if (first_weighted_offset == 0 || hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence)) {
        fprintf(stderr, "two-RPS weighted B accepted under-range L1 chroma offset\n");
        return 0;
    }
    return 1;
}

static int test_hevc_two_rps_multislice_idr_validation(void)
{
    VAPictureParameterBufferHEVC picture = {0};
    picture.CurrPic.picture_id = 1;
    picture.pic_width_in_luma_samples = 640;
    picture.pic_height_in_luma_samples = 360;
    picture.log2_diff_max_min_luma_coding_block_size = 3;
    picture.num_short_term_ref_pic_sets = 2;
    picture.pic_fields.bits.chroma_format_idc = 1;
    picture.slice_parsing_fields.bits.IdrPicFlag = 1;
    picture.slice_parsing_fields.bits.RapPicFlag = 1;
    picture.slice_parsing_fields.bits.IntraPicFlag = 1;
    for (size_t i = 0; i < 15; i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    }

    uint8_t first_data[12] = {0x26, 0x01};
    BitWriter first_writer = {first_data + 2, 0};
    bw_put_bit(&first_writer, 1);
    bw_put_bit(&first_writer, 0);
    bw_put_ue(&first_writer, 0);
    bw_put_ue(&first_writer, 2);

    uint8_t second_data[12] = {0x26, 0x01};
    BitWriter second_writer = {second_data + 2, 0};
    bw_put_bit(&second_writer, 0);
    bw_put_bit(&second_writer, 0);
    bw_put_ue(&second_writer, 0);
    bw_put_bits(&second_writer, 30, 6);
    bw_put_ue(&second_writer, 2);

    VASliceParameterBufferHEVC slices[2] = {{0}};
    for (size_t i = 0; i < 2; i++) {
        slices[i].slice_data_size = 12;
        slices[i].slice_data_byte_offset = 5;
        slices[i].slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
        slices[i].LongSliceFlags.fields.slice_type = 2;
    }
    slices[1].slice_segment_address = 30;
    slices[1].LongSliceFlags.fields.LastSliceOfPic = 1;

    HobotHevcSliceSequence sequence;
    if (!hobot_hevc_slice_sequence_init(&picture, 2, &sequence) ||
        !hobot_hevc_slice_sequence_add(&sequence, &slices[0], sizeof(first_data)) ||
        !hobot_hevc_slice_sequence_add(&sequence, &slices[1], sizeof(second_data)) ||
        !hobot_hevc_slice_sequence_complete(&sequence) ||
        !hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[0], first_data, sizeof(first_data), &sequence) ||
        !hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[1], second_data, sizeof(second_data), &sequence)) {
        fprintf(stderr, "valid two-slice IDR with unused SPS RPS sets was rejected\n");
        return 0;
    }

    slices[1].slice_segment_address = 29;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[1], second_data, sizeof(second_data), &sequence)) {
        fprintf(stderr, "HEVC IDR accepted a slice address inconsistent with its NAL header\n");
        return 0;
    }
    slices[1].slice_segment_address = 30;

    uint8_t non_idr_nal_type = second_data[0];
    second_data[0] = 0x02;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[1], second_data, sizeof(second_data), &sequence)) {
        fprintf(stderr, "HEVC multi-slice RPS validator accepted non-IDR input\n");
        return 0;
    }
    second_data[0] = non_idr_nal_type;

    uint8_t non_independent_header = second_data[2];
    second_data[2] |= 0x80;
    if (hobot_hevc_validated_rps_slice_supported(
            &picture, &slices[1], second_data, sizeof(second_data), &sequence)) {
        fprintf(stderr, "HEVC multi-slice RPS validator accepted a wrong first-slice flag\n");
        return 0;
    }
    second_data[2] = non_independent_header;
    return 1;
}

static int test_hevc_main_encoder_parameter_validation(void)
{
    VAEncSequenceParameterBufferHEVC sequence = {0};
    sequence.general_profile_idc = 1;
    sequence.general_level_idc = MC_H265_LEVEL4_1;
    sequence.intra_period = 30;
    sequence.intra_idr_period = 30;
    sequence.ip_period = 1;
    sequence.bits_per_second = 4000000;
    sequence.pic_width_in_luma_samples = 640;
    sequence.pic_height_in_luma_samples = 360;
    sequence.seq_fields.bits.chroma_format_idc = 1;
    sequence.log2_min_luma_coding_block_size_minus3 = 0;
    sequence.log2_diff_max_min_luma_coding_block_size = 3;
    sequence.log2_min_transform_block_size_minus2 = 0;
    sequence.log2_diff_max_min_transform_block_size = 3;

    VAEncPictureParameterBufferHEVC picture = {0};
    picture.coded_buf = 1;
    picture.pic_init_qp = 26;
    picture.pic_fields.bits.coding_type = 1;

    VAEncSliceParameterBufferHEVC slice = {0};
    slice.num_ctu_in_slice = 60;
    slice.slice_type = 2;
    slice.max_num_merge_cand = 5;
    slice.slice_fields.bits.last_slice_of_pic_flag = 1;

    if (!hobot_hevc_encode_sequence_supported(&sequence, 640, 360) ||
        !hobot_hevc_encode_sequence_supported(&sequence, 640, 368) ||
        !hobot_hevc_encode_picture_supported(&picture) ||
        !hobot_hevc_encode_slice_supported(&sequence, &picture, &slice,
                                           640, 360, VA_RC_CBR)) {
        fprintf(stderr, "valid HEVC Main I-frame encode parameters were rejected\n");
        return 0;
    }

    VAEncSequenceParameterBufferHEVC changed_sequence = sequence;
    if (!hobot_hevc_encode_sequence_static_equal(&sequence, &sequence)) {
        fprintf(stderr, "identical HEVC sequence parameters were not considered static-equal\n");
        return 0;
    }
    changed_sequence.intra_period++;
    changed_sequence.bits_per_second += 1000000;
    if (!hobot_hevc_encode_sequence_static_equal(&sequence, &changed_sequence)) {
        fprintf(stderr, "dynamic HEVC rate-control fields were treated as static\n");
        return 0;
    }
    changed_sequence = sequence;
    changed_sequence.pic_width_in_luma_samples++;
    if (hobot_hevc_encode_sequence_static_equal(&sequence, &changed_sequence)) {
        fprintf(stderr, "HEVC coded width change was treated as dynamic\n");
        return 0;
    }
    changed_sequence = sequence;
    changed_sequence.seq_fields.bits.sps_temporal_mvp_enabled_flag = 1;
    if (hobot_hevc_encode_sequence_static_equal(&sequence, &changed_sequence)) {
        fprintf(stderr, "HEVC SPS syntax change was treated as dynamic\n");
        return 0;
    }

    VAEncPictureParameterBufferHEVC wpp_picture = picture;
    wpp_picture.pic_fields.bits.entropy_coding_sync_enabled_flag = 1;
    if (!hobot_hevc_encode_picture_supported(&wpp_picture) ||
        !hobot_hevc_encode_slice_supported(&sequence, &wpp_picture, &slice,
                                           640, 360, VA_RC_CBR) ||
        hobot_hevc_encode_picture_static_equal(&picture, &wpp_picture)) {
        fprintf(stderr, "HEVC WPP encode settings were rejected or treated as dynamic\n");
        return 0;
    }

    mc_video_codec_enc_params_t geometry = {0};
    geometry.width = 640;
    geometry.height = 368;
    if (!hobot_hevc_configure_encoder_geometry(&sequence, &geometry) ||
        geometry.width != 640 || geometry.height != 360 ||
        geometry.frame_cropping_flag != 0 || geometry.crop_rect.width != 0 ||
        geometry.crop_rect.height != 0) {
        fprintf(stderr, "HEVC encoder input dimensions did not retain VA geometry\n");
        return 0;
    }
    VAEncSequenceParameterBufferHEVC aligned_sequence = sequence;
    aligned_sequence.pic_height_in_luma_samples = 368;
    if (!hobot_hevc_configure_encoder_geometry(&aligned_sequence, &geometry) ||
        geometry.frame_cropping_flag != 0 || geometry.crop_rect.width != 0 ||
        geometry.crop_rect.height != 0) {
        fprintf(stderr, "aligned HEVC dimensions unexpectedly enabled SPS cropping\n");
        return 0;
    }

    VAEncSequenceParameterBufferHEVC ctu_boundary_sequence = sequence;
    ctu_boundary_sequence.pic_width_in_luma_samples = 64;
    ctu_boundary_sequence.pic_height_in_luma_samples = 64;
    VAEncSliceParameterBufferHEVC ctu_boundary_slice = slice;
    ctu_boundary_slice.num_ctu_in_slice = 1;
    if (!hobot_hevc_encode_slice_supported(&ctu_boundary_sequence, &picture,
                                           &ctu_boundary_slice, 64, 79,
                                           VA_RC_CBR)) {
        fprintf(stderr, "HEVC CTU validation used padded context dimensions\n");
        return 0;
    }

    sequence.bits_per_second = 0;
    if (!hobot_hevc_encode_sequence_supported(&sequence, 640, 360)) {
        fprintf(stderr, "HEVC sequence with rate-control misc fallback was rejected\n");
        return 0;
    }
    sequence.bits_per_second = 4000000;
    sequence.ip_period = 2;
    if (hobot_hevc_encode_sequence_supported(&sequence, 640, 360)) {
        fprintf(stderr, "HEVC sequence with B-frame GOP period was accepted\n");
        return 0;
    }
    sequence.ip_period = 1;
    sequence.seq_fields.bits.sample_adaptive_offset_enabled_flag = 1;
    if (hobot_hevc_encode_sequence_supported(&sequence, 640, 360)) {
        fprintf(stderr, "unsupported HEVC SAO sequence was accepted\n");
        return 0;
    }
    sequence.seq_fields.bits.sample_adaptive_offset_enabled_flag = 0;
    sequence.general_level_idc = MC_H265_LEVEL5_1;
    sequence.pic_width_in_luma_samples = 3840;
    sequence.pic_height_in_luma_samples = 2160;
    if (!hobot_hevc_encode_sequence_supported(&sequence, 3840, 2160)) {
        fprintf(stderr, "valid HEVC Main Level 5.1 sequence was rejected\n");
        return 0;
    }

    sequence.general_level_idc = MC_H265_LEVEL4_1;
    sequence.pic_width_in_luma_samples = 640;
    sequence.pic_height_in_luma_samples = 360;
    slice.num_ctu_in_slice--;
    if (hobot_hevc_encode_slice_supported(&sequence, &picture, &slice,
                                          640, 360, VA_RC_CBR)) {
        fprintf(stderr, "partial HEVC picture slice was accepted\n");
        return 0;
    }
    slice.num_ctu_in_slice++;
    slice.slice_fields.bits.slice_temporal_mvp_enabled_flag = 1;
    if (hobot_hevc_encode_slice_supported(&sequence, &picture, &slice,
                                          640, 360, VA_RC_CBR)) {
        fprintf(stderr, "HEVC slice enabled temporal MVP without SPS support\n");
        return 0;
    }
    slice.slice_fields.bits.slice_temporal_mvp_enabled_flag = 0;

    VAEncSliceParameterBufferHEVC cqp_slice = slice;
    cqp_slice.slice_qp_delta = 25;
    if (!hobot_hevc_encode_slice_supported(&sequence, &picture, &cqp_slice,
                                           640, 360, VA_RC_CQP) ||
        hobot_hevc_encode_slice_supported(&sequence, &picture, &cqp_slice,
                                          640, 360, VA_RC_CBR)) {
        fprintf(stderr, "HEVC CQP slice delta was rejected or accepted in CBR\n");
        return 0;
    }
    cqp_slice.slice_qp_delta = -27;
    if (hobot_hevc_encode_slice_supported(&sequence, &picture, &cqp_slice,
                                          640, 360, VA_RC_CQP)) {
        fprintf(stderr, "HEVC CQP out-of-range effective QP was accepted\n");
        return 0;
    }

    VAEncPictureParameterBufferHEVC changed_picture = picture;
    changed_picture.pic_fields.bits.transform_skip_enabled_flag = 1;
    if (!hobot_hevc_encode_picture_supported(&changed_picture) ||
        hobot_hevc_encode_picture_static_equal(&picture, &changed_picture)) {
        fprintf(stderr, "HEVC PPS change was not distinguished from per-frame state\n");
        return 0;
    }
    changed_picture = picture;
    changed_picture.pic_init_qp = 40;
    if (!hobot_hevc_encode_picture_static_equal(&picture, &changed_picture)) {
        fprintf(stderr, "per-picture HEVC QP change was treated as a static PPS change\n");
        return 0;
    }
    return 1;
}

static int test_hevc_encoder_sequence_reconfiguration_contract(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    uint8_t coded_data[16] = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->id = 1;
    hctx->is_encoder = 1;
    hctx->encoder_picture_active = 1;
    hctx->encoder_init_deferred = 1;
    hctx->profile = VAProfileHEVCMain;
    hctx->rate_control = VA_RC_CBR;
    hctx->width = 640;
    hctx->height = 360;
    hctx->vpu_ctx.codec_id = MEDIA_CODEC_ID_H265;
    hctx->hevc_encode_sequence_valid = 1;

    VAEncSequenceParameterBufferHEVC sequence = {0};
    sequence.general_profile_idc = 1;
    sequence.general_level_idc = MC_H265_LEVEL4_1;
    sequence.intra_period = 30;
    sequence.intra_idr_period = 30;
    sequence.ip_period = 1;
    sequence.bits_per_second = 4000000;
    sequence.pic_width_in_luma_samples = 640;
    sequence.pic_height_in_luma_samples = 360;
    sequence.seq_fields.bits.chroma_format_idc = 1;
    sequence.log2_diff_max_min_luma_coding_block_size = 3;
    sequence.log2_diff_max_min_transform_block_size = 3;
    hctx->hevc_encode_sequence = sequence;
    hctx->vpu_ctx.video_enc_params.rc_params.h265_cbr_params.bit_rate = 4000;
    hctx->vpu_ctx.video_enc_params.rc_params.h265_cbr_params.intra_period = 30;
    hctx->current_render_target = 1;
    drv.surfaces[1].allocated = 1;
    drv.surfaces[1].width = 640;
    drv.surfaces[1].height = 360;
    drv.surfaces[1].dma_fd = -1;
    drv.buffers[1] = (HobotBuffer){
        .id = 1,
        .allocated = 1,
        .type = VAEncSequenceParameterBufferType,
        .size = sizeof(sequence),
        .capacity = sizeof(sequence),
        .element_size = sizeof(sequence),
        .num_elements = 1,
        .data = &sequence
    };
    VAEncPictureParameterBufferHEVC picture = {0};
    picture.decoded_curr_pic.picture_id = 1;
    for (size_t i = 0; i < sizeof(picture.reference_frames) /
                            sizeof(picture.reference_frames[0]); i++) {
        picture.reference_frames[i].picture_id = VA_INVALID_SURFACE;
        picture.reference_frames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    picture.coded_buf = 4;
    picture.pic_init_qp = 26;
    picture.pic_fields.bits.coding_type = 1;
    drv.buffers[2] = (HobotBuffer){
        .id = 2,
        .allocated = 1,
        .type = VAEncPictureParameterBufferType,
        .size = sizeof(picture),
        .capacity = sizeof(picture),
        .element_size = sizeof(picture),
        .num_elements = 1,
        .data = &picture
    };
    VAEncSliceParameterBufferHEVC slice = {0};
    slice.num_ctu_in_slice = 60;
    slice.slice_type = 2;
    slice.max_num_merge_cand = 5;
    slice.slice_fields.bits.last_slice_of_pic_flag = 1;
    drv.buffers[3] = (HobotBuffer){
        .id = 3,
        .allocated = 1,
        .type = VAEncSliceParameterBufferType,
        .size = sizeof(slice),
        .capacity = sizeof(slice),
        .element_size = sizeof(slice),
        .num_elements = 1,
        .data = &slice
    };
    drv.buffers[4] = (HobotBuffer){
        .id = 4,
        .allocated = 1,
        .type = VAEncCodedBufferType,
        .size = sizeof(coded_data),
        .capacity = sizeof(coded_data),
        .data = coded_data
    };
    VABufferID frame_buffers[] = {1, 2, 3};

    sequence.intra_period = 45;
    sequence.bits_per_second = 5000000;
    int dynamic_supported = hobot_hevc_encode_sequence_supported(
        &sequence, 640, 360);
    int dynamic_static_equal = hobot_hevc_encode_sequence_static_equal(
        &hctx->hevc_encode_sequence, &sequence);
    VAStatus dynamic_status = hobot_vaRenderPicture(
        &va_ctx, 1, frame_buffers, 3);
    VAStatus status = dynamic_status;
    int dynamic_update_applied = dynamic_status == VA_STATUS_SUCCESS &&
        hctx->hevc_encode_sequence.intra_period == 45 &&
        hctx->hevc_encode_sequence.bits_per_second == 5000000 &&
        hctx->vpu_ctx.video_enc_params.rc_params.h265_cbr_params.intra_period == 45 &&
        hctx->vpu_ctx.video_enc_params.rc_params.h265_cbr_params.bit_rate == 5000;

    sequence.seq_fields.bits.sps_temporal_mvp_enabled_flag = 1;
    status = hobot_vaRenderPicture(&va_ctx, 1, frame_buffers, 3);
    int static_update_rejected = status == VA_STATUS_ERROR_ATTR_NOT_SUPPORTED &&
        hctx->hevc_encode_sequence.seq_fields.bits.sps_temporal_mvp_enabled_flag == 0 &&
        hctx->hevc_encode_sequence.intra_period == 45 &&
        hctx->hevc_encode_sequence.bits_per_second == 5000000 &&
        hctx->vpu_ctx.video_enc_params.rc_params.h265_cbr_params.intra_period == 45 &&
        hctx->vpu_ctx.video_enc_params.rc_params.h265_cbr_params.bit_rate == 5000;

    if (!dynamic_update_applied || !static_update_rejected)
        fprintf(stderr,
                "HEVC sequence reconfiguration contract failed: dynamic=%d dynamic_status=%d supported=%d equal=%d static=%d status=%d cached_mvp=%u intra=%u bitrate=%u\n",
                dynamic_update_applied, dynamic_status, dynamic_supported,
                dynamic_static_equal, static_update_rejected, status,
                hctx->hevc_encode_sequence.seq_fields.bits.sps_temporal_mvp_enabled_flag,
                hctx->vpu_ctx.video_enc_params.rc_params.h265_cbr_params.intra_period,
                hctx->vpu_ctx.video_enc_params.rc_params.h265_cbr_params.bit_rate);

    pthread_mutex_destroy(&drv.mutex);
    return dynamic_update_applied && static_update_rejected;
}

static int test_hevc_sps_conformance_window_crop(void)
{
    VAEncSequenceParameterBufferHEVC sequence = {0};
    sequence.pic_width_in_luma_samples = 640;
    sequence.pic_height_in_luma_samples = 360;
    sequence.log2_min_luma_coding_block_size_minus3 = 0;
    sequence.log2_diff_max_min_luma_coding_block_size = 3;

    uint8_t rbsp[64] = {0};
    BitWriter writer = {rbsp, 0};
    bw_put_bits(&writer, 0, 4);
    bw_put_bits(&writer, 0, 3);
    bw_put_bit(&writer, 1);
    bw_put_bits(&writer, 0, 32);
    bw_put_bits(&writer, 0, 32);
    bw_put_bits(&writer, 0, 32);
    bw_put_ue(&writer, 0);
    bw_put_ue(&writer, 1);
    bw_put_ue(&writer, 640);
    bw_put_ue(&writer, 368);
    bw_put_bit(&writer, 0);
    bw_put_bit(&writer, 1);

    uint8_t escaped[128] = {0};
    uint8_t stream[160] = {0, 0, 0, 1, 0x42, 0x01};
    size_t escaped_size = 0;
    size_t rbsp_size = ((size_t)writer.bit_pos + 7u) / 8u;
    if (!hobot_hevc_escape_rbsp(rbsp, rbsp_size, escaped, sizeof(escaped),
                                 &escaped_size) ||
        escaped_size > sizeof(stream) - 6u)
        return 0;

    memcpy(stream + 6, escaped, escaped_size);
    size_t stream_size = 6u + escaped_size;
    uint8_t original_stream[sizeof(stream)];
    memcpy(original_stream, stream, stream_size);
    size_t original_size = stream_size;
    int found_sps = 0;
    VAEncSequenceParameterBufferHEVC unsafe_sequence = sequence;
    unsafe_sequence.pic_height_in_luma_samples = 352;
    if (hobot_hevc_patch_output_sps(stream, &stream_size, sizeof(stream),
                                     &unsafe_sequence, 640, 352, &found_sps)) {
        fprintf(stderr, "HEVC SPS crop accepted a 16-pixel dimension delta\n");
        return 0;
    }

    uint8_t combined_crop_stream[sizeof(stream)];
    memcpy(combined_crop_stream, original_stream, original_size);
    size_t combined_crop_size = original_size;
    if (!hobot_hevc_patch_output_sps(combined_crop_stream,
                                     &combined_crop_size,
                                     sizeof(combined_crop_stream),
                                     &sequence, 638, 360, &found_sps) ||
        !found_sps || combined_crop_size <= original_size) {
        fprintf(stderr, "HEVC SPS crop rejected a bounded 638x360 crop\n");
        return 0;
    }
    uint8_t decoded_crop_rbsp[128];
    size_t decoded_crop_size = 0;
    uint32_t crop_width, crop_height, crop_chroma, crop_flag;
    uint32_t combined_offsets[4] = {0};
    size_t crop_width_start, crop_width_end, crop_height_start, crop_height_end;
    size_t crop_flag_start;
    if (!hobot_hevc_unescape_rbsp(combined_crop_stream + 6,
                                   combined_crop_size - 6,
                                   decoded_crop_rbsp, sizeof(decoded_crop_rbsp),
                                   &decoded_crop_size) ||
        !hobot_hevc_parse_sps_dimensions(
            decoded_crop_rbsp, decoded_crop_size,
            &crop_width, &crop_height,
            &crop_width_start, &crop_width_end,
            &crop_height_start, &crop_height_end,
            &crop_flag_start, &crop_chroma, &crop_flag, combined_offsets) ||
        crop_width != 640 || crop_height != 368 || crop_chroma != 1 ||
        crop_flag != 1 || combined_offsets[0] != 0 ||
        combined_offsets[1] != 1 || combined_offsets[2] != 0 ||
        combined_offsets[3] != 4) {
        fprintf(stderr, "HEVC SPS crop did not encode the requested right/bottom offsets\n");
        return 0;
    }

    uint8_t oversized_crop_stream[sizeof(stream)];
    memcpy(oversized_crop_stream, original_stream, original_size);
    size_t oversized_crop_size = original_size;
    if (hobot_hevc_patch_output_sps(oversized_crop_stream,
                                     &oversized_crop_size,
                                     sizeof(oversized_crop_stream),
                                     &sequence, 624, 360, &found_sps)) {
        fprintf(stderr, "HEVC SPS crop accepted a 16-pixel width delta\n");
        return 0;
    }

    uint8_t tight_combined_stream[sizeof(stream)];
    memcpy(tight_combined_stream, original_stream, original_size);
    size_t tight_combined_size = original_size;
    if (hobot_hevc_patch_output_sps(tight_combined_stream,
                                     &tight_combined_size, original_size,
                                     &sequence, 638, 360, &found_sps) ||
        tight_combined_size != original_size ||
        memcmp(tight_combined_stream, original_stream, original_size) != 0) {
        fprintf(stderr, "HEVC combined crop ignored coded-buffer capacity or partially modified output\n");
        return 0;
    }

    VAEncSequenceParameterBufferHEVC different_grid = sequence;
    different_grid.log2_diff_max_min_luma_coding_block_size = 0;
    memcpy(stream, original_stream, original_size);
    stream_size = original_size;
    if (hobot_hevc_patch_output_sps(stream, &stream_size, sizeof(stream),
                                     &different_grid, 640, 360, &found_sps)) {
        fprintf(stderr, "HEVC SPS crop accepted a changed CTU grid\n");
        return 0;
    }

    uint8_t tight_stream[sizeof(stream)];
    memcpy(tight_stream, original_stream, original_size);
    size_t tight_size = original_size;
    if (hobot_hevc_patch_output_sps(tight_stream, &tight_size, tight_size,
                                     &sequence, 640, 360, &found_sps) ||
        tight_size != original_size ||
        memcmp(tight_stream, original_stream, original_size) != 0) {
        fprintf(stderr, "HEVC SPS crop ignored coded-buffer capacity or partially modified output\n");
        return 0;
    }

    memcpy(stream, original_stream, original_size);
    stream_size = original_size;
    size_t unpatched_size = stream_size;
    if (!hobot_hevc_patch_output_sps(stream, &stream_size, sizeof(stream),
                                      &sequence, 640, 360, &found_sps) ||
        !found_sps || stream_size <= unpatched_size)
        return 0;
    uint8_t decoded_rbsp[128];
    size_t decoded_size = 0;
    uint32_t width, height, chroma, conf_win, crop[4] = {0};
    size_t width_start, width_end, height_start, height_end;
    size_t conf_win_flag_start;
    if (!hobot_hevc_unescape_rbsp(stream + 6, stream_size - 6,
                                   decoded_rbsp, sizeof(decoded_rbsp),
                                   &decoded_size) ||
        !hobot_hevc_parse_sps_dimensions(
            decoded_rbsp, decoded_size, &width, &height,
            &width_start, &width_end, &height_start, &height_end,
            &conf_win_flag_start, &chroma, &conf_win, crop) ||
        width != 640 || height != 368 || chroma != 1 || conf_win != 1 ||
        crop[0] != 0 || crop[1] != 0 || crop[2] != 0 || crop[3] != 4) {
        fprintf(stderr, "HEVC SPS crop did not preserve coded size and signal the requested visible dimensions\n");
        return 0;
    }
    return 1;
}

static int test_hevc_context_rejects_oversized_resolution(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    VAContextID context_id = VA_INVALID_ID;
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    drv.configs[1].allocated = 1;
    drv.configs[1].profile = VAProfileHEVCMain;

    VAStatus status = hobot_vaCreateContext(&va_ctx, 1, 3842, 2160, 0,
                                             NULL, 0, &context_id);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER ||
        context_id != VA_INVALID_ID || drv.contexts[1].allocated) {
        fprintf(stderr, "HEVC context accepted width above advertised maximum\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    status = hobot_vaCreateContext(&va_ctx, 1, 3840, 2162, 0,
                                   NULL, 0, &context_id);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER ||
        context_id != VA_INVALID_ID || drv.contexts[1].allocated) {
        fprintf(stderr, "HEVC context accepted height above advertised maximum\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static int test_h264_encoder_slice_constraints(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    VAEncSliceParameterBufferH264 slice = {0};
    uint32_t misc_storage[(offsetof(VAEncMiscParameterBuffer, data) +
                           sizeof(uint32_t) + sizeof(uint32_t) - 1) /
                          sizeof(uint32_t)] = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->is_encoder = 1;
    hctx->encoder_picture_active = 1;
    hctx->profile = VAProfileH264Main;
    hctx->rate_control = VA_RC_CBR;
    hctx->width = 640;
    hctx->height = 368;

    slice.num_macroblocks = 40u * 23u;
    slice.macroblock_info = VA_INVALID_ID;
    slice.slice_type = 2;
    drv.buffers[1].allocated = 1;
    drv.buffers[1].type = VAEncSliceParameterBufferType;
    drv.buffers[1].data = &slice;
    drv.buffers[1].size = sizeof(slice);
    drv.buffers[1].element_size = sizeof(slice);
    drv.buffers[1].num_elements = 1;
    VABufferID slice_id = 1;

    VAStatus status = hobot_vaRenderPicture(&va_ctx, 1, &slice_id, 1);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "single full-frame H.264 slice was rejected: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    slice.num_macroblocks--;
    hctx->h264_encode_slice_valid = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, &slice_id, 1);
    if (status != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED) {
        fprintf(stderr, "partial-frame H.264 slice was not rejected: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    slice.num_macroblocks++;

    slice.macroblock_address = 1;
    hctx->h264_encode_slice_valid = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, &slice_id, 1);
    if (status != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED) {
        fprintf(stderr, "nonzero H.264 slice start was not rejected: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    slice.macroblock_address = 0;

    slice.macroblock_info = 2;
    hctx->h264_encode_slice_valid = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, &slice_id, 1);
    if (status != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED) {
        fprintf(stderr, "H.264 macroblock map was not rejected: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    slice.macroblock_info = VA_INVALID_ID;

    slice.slice_type = 3;
    hctx->h264_encode_slice_valid = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, &slice_id, 1);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER) {
        fprintf(stderr, "invalid H.264 slice type was accepted: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    slice.slice_type = 2;

    slice.slice_type = 1;
    hctx->h264_encode_slice_valid = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, &slice_id, 1);
    if (status != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED) {
        fprintf(stderr, "unsupported B-slice request was accepted: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    slice.slice_type = 2;

    VABufferID duplicate_slice_ids[] = {1, 1};
    status = hobot_vaRenderPicture(&va_ctx, 1, duplicate_slice_ids, 2);
    if (status != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED) {
        fprintf(stderr, "multiple H.264 slices were not rejected: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    drv.buffers[2].allocated = 1;
    drv.buffers[2].type = VAIQMatrixBufferType;
    drv.buffers[2].data = misc_storage;
    drv.buffers[2].size = sizeof(misc_storage);
    VABufferID unsupported_type_id = 2;
    status = hobot_vaRenderPicture(&va_ctx, 1, &unsupported_type_id, 1);
    if (status != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED) {
        fprintf(stderr, "unsupported H.264 encoder buffer type was silently accepted: status=%d\n",
                status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    VAEncMiscParameterBuffer *misc = (VAEncMiscParameterBuffer *)misc_storage;
    misc->type = VAEncMiscParameterTypeMaxFrameSize;
    drv.buffers[2].type = VAEncMiscParameterBufferType;
    drv.buffers[2].size = sizeof(misc_storage);
    status = hobot_vaRenderPicture(&va_ctx, 1, &unsupported_type_id, 1);
    if (status != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED) {
        fprintf(stderr, "unsupported H.264 misc parameter was silently accepted: status=%d\n",
                status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static int test_h264_cqp_uses_effective_slice_qp(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    VAEncPictureParameterBufferH264 picture = {0};
    VAEncSliceParameterBufferH264 slice = {0};
    uint8_t coded_data[64] = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->id = 1;
    hctx->vpu_running = 1;
    hctx->is_encoder = 1;
    hctx->encoder_picture_active = 1;
    hctx->profile = VAProfileH264High;
    hctx->rate_control = VA_RC_CQP;
    hctx->width = 640;
    hctx->height = 368;
    hctx->current_render_target = 1;
    hctx->vpu_ctx.codec_id = MEDIA_CODEC_ID_H264;
    drv.surfaces[1].allocated = 1;
    drv.surfaces[1].width = 640;
    drv.surfaces[1].height = 368;

    for (size_t i = 0; i < sizeof(picture.ReferenceFrames) /
                            sizeof(picture.ReferenceFrames[0]); i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    picture.CurrPic.picture_id = 1;
    picture.coded_buf = 3;
    picture.pic_init_qp = 26;
    slice.num_macroblocks = 40u * 23u;
    slice.macroblock_info = VA_INVALID_ID;
    slice.slice_type = 0;
    slice.slice_qp_delta = 11;
    drv.buffers[1] = (HobotBuffer){
        .id = 1, .allocated = 1, .type = VAEncPictureParameterBufferType,
        .size = sizeof(picture), .capacity = sizeof(picture),
        .element_size = sizeof(picture), .num_elements = 1, .data = &picture
    };
    drv.buffers[2] = (HobotBuffer){
        .id = 2, .allocated = 1, .type = VAEncSliceParameterBufferType,
        .size = sizeof(slice), .capacity = sizeof(slice),
        .element_size = sizeof(slice), .num_elements = 1, .data = &slice
    };
    drv.buffers[3] = (HobotBuffer){
        .id = 3, .allocated = 1, .type = VAEncCodedBufferType,
        .size = sizeof(coded_data), .capacity = sizeof(coded_data),
        .data = coded_data
    };

    VABufferID ids[] = {1, 2};
    mock_rate_control_result = 0;
    int calls_before = mock_rate_control_calls;
    VAStatus status = hobot_vaRenderPicture(&va_ctx, 1, ids, 2);
    if (status != VA_STATUS_SUCCESS ||
        mock_rate_control_calls != calls_before + 1 ||
        hctx->vpu_ctx.video_enc_params.rc_params.mode != MC_AV_RC_MODE_H264FIXQP ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_fixqp_params.force_qp_I != 37 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_fixqp_params.force_qp_P != 37 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_fixqp_params.force_qp_B != 37) {
        fprintf(stderr, "H.264 CQP did not apply pic_init_qp + slice_qp_delta: status=%d calls=%d qp=%u/%u/%u\n",
                status, mock_rate_control_calls - calls_before,
                hctx->vpu_ctx.video_enc_params.rc_params.h264_fixqp_params.force_qp_I,
                hctx->vpu_ctx.video_enc_params.rc_params.h264_fixqp_params.force_qp_P,
                hctx->vpu_ctx.video_enc_params.rc_params.h264_fixqp_params.force_qp_B);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    picture.pic_init_qp = 50;
    slice.slice_qp_delta = 2;
    hctx->h264_encode_slice_valid = 0;
    hctx->h264_encode_pic_qp_valid = 0;
    hctx->enc_coded_buf = 0;
    calls_before = mock_rate_control_calls;
    status = hobot_vaRenderPicture(&va_ctx, 1, ids, 2);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER ||
        mock_rate_control_calls != calls_before ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_fixqp_params.force_qp_P != 37) {
        fprintf(stderr, "out-of-range H.264 CQP was not rejected transactionally: status=%d calls=%d qp=%u\n",
                status, mock_rate_control_calls - calls_before,
                hctx->vpu_ctx.video_enc_params.rc_params.h264_fixqp_params.force_qp_P);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static int test_h264_hrd_vbv_window(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    uint32_t storage[(offsetof(VAEncMiscParameterBuffer, data) +
                      sizeof(VAEncMiscParameterHRD) + sizeof(uint32_t) - 1) /
                     sizeof(uint32_t)] = {0};
    uint32_t rate_storage[(offsetof(VAEncMiscParameterBuffer, data) +
                           sizeof(VAEncMiscParameterRateControl) + sizeof(uint32_t) - 1) /
                          sizeof(uint32_t)] = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->is_encoder = 1;
    hctx->encoder_picture_active = 1;
    hctx->profile = VAProfileH264High;
    hctx->rate_control = VA_RC_CBR;
    hctx->vpu_ctx.video_enc_params.rc_params.mode = MC_AV_RC_MODE_H264CBR;
    hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate = 5000;
    hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.vbv_buffer_size = 321;

    VAEncMiscParameterBuffer *misc = (VAEncMiscParameterBuffer *)storage;
    misc->type = VAEncMiscParameterTypeHRD;
    VAEncMiscParameterHRD *hrd = (VAEncMiscParameterHRD *)misc->data;
    hrd->buffer_size = 10000000;
    hrd->initial_buffer_fullness = 7500000;
    drv.buffers[1].allocated = 1;
    drv.buffers[1].type = VAEncMiscParameterBufferType;
    drv.buffers[1].data = storage;
    drv.buffers[1].size = sizeof(storage);
    VABufferID id = 1;

    mock_rate_control_result = 0;
    int calls_before = mock_rate_control_calls;
    VAStatus status = hobot_vaRenderPicture(&va_ctx, 1, &id, 1);
    if (status != VA_STATUS_SUCCESS ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.vbv_buffer_size != 2000 ||
        mock_rate_control_calls != calls_before + 1) {
        fprintf(stderr, "H.264 HRD buffer was not mapped to the SDK VBV window: status=%d ms=%d calls=%d\n",
                status,
                hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.vbv_buffer_size,
                mock_rate_control_calls - calls_before);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    hrd->initial_buffer_fullness = hrd->buffer_size + 1u;
    calls_before = mock_rate_control_calls;
    status = hobot_vaRenderPicture(&va_ctx, 1, &id, 1);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER ||
        mock_rate_control_calls != calls_before) {
        fprintf(stderr, "invalid HRD fullness was not rejected before SDK mutation: status=%d calls=%d\n",
                status, mock_rate_control_calls - calls_before);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    hrd->initial_buffer_fullness = 0;
    hrd->buffer_size = 40000000;
    status = hobot_vaRenderPicture(&va_ctx, 1, &id, 1);
    if (status != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED) {
        fprintf(stderr, "unrepresentable SDK VBV window was accepted: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    hrd->buffer_size = 10000000;
    mock_rate_control_result = -1;
    status = hobot_vaRenderPicture(&va_ctx, 1, &id, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.vbv_buffer_size != 2000) {
        fprintf(stderr, "failed H.264 VBV update was not rolled back: status=%d ms=%d\n",
                status,
                hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.vbv_buffer_size);
        pthread_mutex_destroy(&drv.mutex);
        mock_rate_control_result = 0;
        return 0;
    }
    mock_rate_control_result = 0;

    VAEncMiscParameterBuffer *rate_misc = (VAEncMiscParameterBuffer *)rate_storage;
    rate_misc->type = VAEncMiscParameterTypeRateControl;
    ((VAEncMiscParameterRateControl *)rate_misc->data)->bits_per_second = 4000000;
    drv.buffers[2].allocated = 1;
    drv.buffers[2].type = VAEncMiscParameterBufferType;
    drv.buffers[2].data = rate_storage;
    drv.buffers[2].size = sizeof(rate_storage);
    VABufferID reordered_ids[] = {id, 2};
    calls_before = mock_rate_control_calls;
    status = hobot_vaRenderPicture(&va_ctx, 1, reordered_ids, 2);
    if (status != VA_STATUS_SUCCESS ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate != 4000 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.vbv_buffer_size != 2500 ||
        mock_rate_control_calls != calls_before + 2) {
        fprintf(stderr, "HRD conversion depended on buffer order: status=%d bitrate=%u vbv_ms=%d calls=%d\n",
                status,
                hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate,
                hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.vbv_buffer_size,
                mock_rate_control_calls - calls_before);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static int test_encoder_control_errors_are_propagated(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    uint8_t coded_data[16] = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->id = 1;
    hctx->vpu_running = 1;
    hctx->is_encoder = 1;
    hctx->encoder_picture_active = 1;
    hctx->profile = VAProfileH264Main;
    hctx->rate_control = VA_RC_CBR;
    hctx->width = 640;
    hctx->height = 368;
    hctx->current_render_target = 1;
    hctx->vpu_ctx.codec_id = MEDIA_CODEC_ID_H264;
    hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate = 2500;
    hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.frame_rate = 24;
    hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.intra_period = 7;

    VAEncSequenceParameterBufferH264 sequence = {0};
    sequence.bits_per_second = 4000000;
    sequence.intra_period = 30;
    sequence.picture_width_in_mbs = 40;
    sequence.picture_height_in_mbs = 23;
    sequence.level_idc = MC_H264_LEVEL4_1;
    sequence.ip_period = 1;
    sequence.max_num_ref_frames = 1;
    sequence.seq_fields.bits.chroma_format_idc = 1;
    sequence.seq_fields.bits.frame_mbs_only_flag = 1;
    hctx->h264_sequence = sequence;
    hctx->h264_sequence_valid = 1;
    hctx->vpu_ctx.video_enc_params.h264_enc_config.h264_level =
        MC_H264_LEVEL4_1;
    drv.surfaces[1].allocated = 1;
    drv.surfaces[1].width = 640;
    drv.surfaces[1].height = 368;
    drv.buffers[1].allocated = 1;
    drv.buffers[1].type = VAEncSequenceParameterBufferType;
    drv.buffers[1].data = &sequence;
    drv.buffers[1].size = sizeof(sequence);
    drv.buffers[1].element_size = sizeof(sequence);
    drv.buffers[1].num_elements = 1;
    VABufferID sequence_id = 1;

    mock_rate_control_calls = 0;
    mock_rate_control_result = -1;
    VAStatus status = hobot_vaRenderPicture(&va_ctx, 1, &sequence_id, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || mock_rate_control_calls != 1 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate != 2500 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.frame_rate != 24 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.intra_period != 7 ||
        !hctx->h264_sequence_valid ||
        hctx->vpu_ctx.video_enc_params.h264_enc_config.h264_level != MC_H264_LEVEL4_1) {
        fprintf(stderr, "rate-control failure was hidden or state not restored: status=%d calls=%d\n",
                status, mock_rate_control_calls);
        pthread_mutex_destroy(&drv.mutex);
        mock_rate_control_result = 0;
        return 0;
    }

    sequence.bits_per_second = 5000000;
    sequence.intra_period = 45;
    mock_rate_control_result = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, &sequence_id, 1);
    if (status != VA_STATUS_SUCCESS || mock_rate_control_calls != 2 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate != 5000 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.intra_period != 45 ||
        !hctx->h264_sequence_valid ||
        hctx->h264_sequence.level_idc != MC_H264_LEVEL4_1 ||
        hctx->vpu_ctx.video_enc_params.h264_enc_config.h264_level != MC_H264_LEVEL4_1) {
        fprintf(stderr, "retry with corrected H.264 sequence failed: status=%d calls=%d valid=%d level=%u rc_level=%d bitrate=%u intra=%u\n",
                status, mock_rate_control_calls,
                hctx->h264_sequence_valid, hctx->h264_sequence.level_idc,
                hctx->vpu_ctx.video_enc_params.h264_enc_config.h264_level,
                hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate,
                hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.intra_period);
        pthread_mutex_destroy(&drv.mutex);
        mock_rate_control_result = 0;
        return 0;
    }

    uint32_t misc_storage[(offsetof(VAEncMiscParameterBuffer, data) +
                           sizeof(VAEncMiscParameterRateControl) +
                           sizeof(VAEncMiscParameterFrameRate) +
                           sizeof(uint32_t) - 1) / sizeof(uint32_t)] = {0};
    VAEncMiscParameterBuffer *misc = (VAEncMiscParameterBuffer *)misc_storage;
    VAEncMiscParameterRateControl *rate =
        (VAEncMiscParameterRateControl *)misc->data;
    misc->type = VAEncMiscParameterTypeRateControl;
    rate->bits_per_second = 6000000;
    drv.buffers[4].allocated = 1;
    drv.buffers[4].type = VAEncMiscParameterBufferType;
    drv.buffers[4].data = misc_storage;
    drv.buffers[4].size = offsetof(VAEncMiscParameterBuffer, data) + sizeof(*rate);
    VABufferID misc_id = 4;
    mock_rate_control_result = -1;
    status = hobot_vaRenderPicture(&va_ctx, 1, &misc_id, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate != 5000) {
        fprintf(stderr, "misc rate-control failure did not restore bitrate: status=%d bitrate=%u\n",
                status, hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate);
        pthread_mutex_destroy(&drv.mutex);
        mock_rate_control_result = 0;
        return 0;
    }

    VAEncMiscParameterFrameRate *frame_rate =
        (VAEncMiscParameterFrameRate *)misc->data;
    misc->type = VAEncMiscParameterTypeFrameRate;
    frame_rate->framerate = 60;
    drv.buffers[4].size = offsetof(VAEncMiscParameterBuffer, data) + sizeof(*frame_rate);
    mock_rate_control_result = -1;
    status = hobot_vaRenderPicture(&va_ctx, 1, &misc_id, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.frame_rate != 24) {
        fprintf(stderr, "misc frame-rate failure did not restore framerate: status=%d fps=%u\n",
                status, hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.frame_rate);
        pthread_mutex_destroy(&drv.mutex);
        mock_rate_control_result = 0;
        return 0;
    }
    mock_rate_control_result = 0;

    frame_rate->framerate = 241;
    int calls_before_invalid_fps = mock_rate_control_calls;
    status = hobot_vaRenderPicture(&va_ctx, 1, &misc_id, 1);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.frame_rate != 24 ||
        mock_rate_control_calls != calls_before_invalid_fps) {
        fprintf(stderr, "out-of-range misc frame rate was not rejected: status=%d fps=%u calls=%d/%d\n",
                status,
                hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.frame_rate,
                mock_rate_control_calls, calls_before_invalid_fps);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    VAEncPictureParameterBufferH264 picture = {0};
    for (size_t i = 0; i < sizeof(picture.ReferenceFrames) /
                            sizeof(picture.ReferenceFrames[0]); i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    picture.CurrPic.picture_id = 1;
    picture.coded_buf = 3;
    picture.pic_fields.bits.idr_pic_flag = 1;
    drv.buffers[2].allocated = 1;
    drv.buffers[2].type = VAEncPictureParameterBufferType;
    drv.buffers[2].data = &picture;
    drv.buffers[2].size = sizeof(picture);
    drv.buffers[2].element_size = sizeof(picture);
    drv.buffers[2].num_elements = 1;
    drv.buffers[3].allocated = 1;
    drv.buffers[3].type = VAEncCodedBufferType;
    drv.buffers[3].data = coded_data;
    drv.buffers[3].size = sizeof(coded_data);
    drv.buffers[3].capacity = sizeof(coded_data);
    mock_idr_calls = 0;
    mock_idr_result = -1;
    VABufferID picture_id = 2;
    status = hobot_vaRenderPicture(&va_ctx, 1, &picture_id, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || mock_idr_calls != 1 ||
        hctx->enc_coded_buf != 0) {
        fprintf(stderr, "IDR request failure was hidden or coded buffer committed: status=%d calls=%d coded=%u\n",
                status, mock_idr_calls, hctx->enc_coded_buf);
        pthread_mutex_destroy(&drv.mutex);
        mock_idr_result = 0;
        return 0;
    }

    mock_idr_result = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, &picture_id, 1);
    int passed = status == VA_STATUS_SUCCESS && mock_idr_calls == 2 &&
                 hctx->enc_coded_buf == 3;
    if (!passed)
        fprintf(stderr, "successful IDR request failed: status=%d calls=%d coded=%u\n",
                status, mock_idr_calls, hctx->enc_coded_buf);

    mock_rate_control_result = 0;
    mock_idr_result = 0;
    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_h264_sequence_parameters_are_transactional(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->id = 1;
    hctx->encoder_init_deferred = 1;
    hctx->is_encoder = 1;
    hctx->encoder_picture_active = 1;
    hctx->profile = VAProfileH264Main;
    hctx->rate_control = VA_RC_CBR;
    hctx->width = 640;
    hctx->height = 368;
    hctx->current_render_target = 1;
    hctx->vpu_ctx.codec_id = MEDIA_CODEC_ID_H264;
    hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate = 2500;
    hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.frame_rate = 24;
    hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.intra_period = 7;

    drv.surfaces[1].allocated = 1;
    drv.surfaces[1].width = 640;
    drv.surfaces[1].height = 368;
    VAEncSequenceParameterBufferH264 sequence = {0};
    sequence.level_idc = MC_H264_LEVEL4_1;
    sequence.bits_per_second = 5000000;
    sequence.intra_period = 45;
    sequence.ip_period = 1;
    sequence.max_num_ref_frames = 1;
    sequence.picture_width_in_mbs = 40;
    sequence.picture_height_in_mbs = 23;
    sequence.seq_fields.bits.chroma_format_idc = 1;
    sequence.seq_fields.bits.frame_mbs_only_flag = 1;
    sequence.vui_parameters_present_flag = 1;
    sequence.vui_fields.bits.timing_info_present_flag = 1;
    sequence.num_units_in_tick = INT32_MAX;
    sequence.time_scale = 1;
    drv.buffers[1].allocated = 1;
    drv.buffers[1].type = VAEncSequenceParameterBufferType;
    drv.buffers[1].data = &sequence;
    drv.buffers[1].size = sizeof(sequence);
    drv.buffers[1].element_size = sizeof(sequence);
    drv.buffers[1].num_elements = 1;
    VABufferID sequence_id = 1;
    mock_rate_control_calls = 0;
    mock_rate_control_result = 0;

    sequence.ip_period = 2;
    VAStatus status = hobot_vaRenderPicture(&va_ctx, 1, &sequence_id, 1);
    if (status != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED || hctx->h264_sequence_valid ||
        mock_rate_control_calls != 0) {
        fprintf(stderr, "unsupported H.264 B-frame period was accepted: status=%d valid=%d calls=%d\n",
                status, hctx->h264_sequence_valid, mock_rate_control_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    sequence.ip_period = 1;
    sequence.max_num_ref_frames = 2;
    status = hobot_vaRenderPicture(&va_ctx, 1, &sequence_id, 1);
    if (status != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED || hctx->h264_sequence_valid ||
        mock_rate_control_calls != 0) {
        fprintf(stderr, "unsupported H.264 multi-reference sequence was accepted: status=%d valid=%d calls=%d\n",
                status, hctx->h264_sequence_valid, mock_rate_control_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    sequence.max_num_ref_frames = 1;

    status = hobot_vaRenderPicture(&va_ctx, 1, &sequence_id, 1);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER || hctx->h264_sequence_valid ||
        hctx->vpu_ctx.video_enc_params.h264_enc_config.h264_level != 0 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate != 2500 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.frame_rate != 24 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.intra_period != 7 ||
        mock_rate_control_calls != 0) {
        fprintf(stderr, "invalid H.264 timing partially committed encoder state: status=%d valid=%d calls=%d\n",
                status, hctx->h264_sequence_valid, mock_rate_control_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    sequence.num_units_in_tick = 1;
    sequence.time_scale = 482;
    status = hobot_vaRenderPicture(&va_ctx, 1, &sequence_id, 1);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER || hctx->h264_sequence_valid ||
        hctx->vpu_ctx.video_enc_params.h264_enc_config.h264_level != 0 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate != 2500 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.frame_rate != 24 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.intra_period != 7 ||
        mock_rate_control_calls != 0) {
        fprintf(stderr, "out-of-range H.264 timing partially committed encoder state: status=%d valid=%d calls=%d\n",
                status, hctx->h264_sequence_valid, mock_rate_control_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    sequence.num_units_in_tick = 1;
    sequence.time_scale = 60;
    VAEncSequenceParameterBufferH264 duplicate_sequence = sequence;
    duplicate_sequence.level_idc = 99;
    drv.buffers[2].allocated = 1;
    drv.buffers[2].type = VAEncSequenceParameterBufferType;
    drv.buffers[2].data = &duplicate_sequence;
    drv.buffers[2].size = sizeof(duplicate_sequence);
    drv.buffers[2].element_size = sizeof(duplicate_sequence);
    drv.buffers[2].num_elements = 1;
    VABufferID sequence_ids[] = {1, 2};
    status = hobot_vaRenderPicture(&va_ctx, 1, sequence_ids, 2);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER || hctx->h264_sequence_valid ||
        hctx->vpu_ctx.video_enc_params.h264_enc_config.h264_level != 0 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate != 2500 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.frame_rate != 24 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.intra_period != 7 ||
        mock_rate_control_calls != 0) {
        fprintf(stderr, "duplicate H.264 sequence buffers changed encoder state: status=%d valid=%d calls=%d\n",
                status, hctx->h264_sequence_valid, mock_rate_control_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    status = hobot_vaRenderPicture(&va_ctx, 1, &sequence_id, 1);
    if (status != VA_STATUS_SUCCESS || !hctx->h264_sequence_valid ||
        hctx->h264_sequence.level_idc != MC_H264_LEVEL4_1 ||
        hctx->vpu_ctx.video_enc_params.h264_enc_config.h264_level != MC_H264_LEVEL4_1 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate != 5000 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.frame_rate != 30 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.intra_period != 45 ||
        mock_rate_control_calls != 0) {
        fprintf(stderr, "valid H.264 sequence retry failed to commit: status=%d valid=%d calls=%d\n",
                status, hctx->h264_sequence_valid, mock_rate_control_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    sequence.max_num_ref_frames = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, &sequence_id, 1);
    if (status != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED ||
        hctx->h264_sequence.max_num_ref_frames != 1 ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate != 5000 ||
        mock_rate_control_calls != 0) {
        fprintf(stderr, "changed H.264 reference configuration was accepted: status=%d refs=%u calls=%d\n",
                status, hctx->h264_sequence.max_num_ref_frames,
                mock_rate_control_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    sequence.max_num_ref_frames = 1;

    mc_rate_control_params_t committed_rc = hctx->vpu_ctx.video_enc_params.rc_params;
    hctx->h264_sequence_valid = 0;
    hctx->encoder_init_deferred = 0;
    hctx->vpu_running = 1;
    status = hobot_vaRenderPicture(&va_ctx, 1, &sequence_id, 1);
    int passed = status == VA_STATUS_ERROR_ATTR_NOT_SUPPORTED &&
                 !hctx->h264_sequence_valid && mock_rate_control_calls == 0 &&
                 hctx->vpu_ctx.video_enc_params.h264_enc_config.h264_level ==
                     MC_H264_LEVEL4_1 &&
                 memcmp(&hctx->vpu_ctx.video_enc_params.rc_params, &committed_rc,
                        sizeof(committed_rc)) == 0;
    if (!passed)
        fprintf(stderr, "late first H.264 sequence was accepted or mutated state: status=%d valid=%d calls=%d\n",
                status, hctx->h264_sequence_valid, mock_rate_control_calls);

    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_h264_zero_reference_requires_intra_slice(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->id = 1;
    hctx->encoder_init_deferred = 1;
    hctx->is_encoder = 1;
    hctx->encoder_picture_active = 1;
    hctx->profile = VAProfileH264Main;
    hctx->rate_control = VA_RC_CBR;
    hctx->width = 640;
    hctx->height = 368;
    hctx->current_render_target = 1;
    hctx->vpu_ctx.codec_id = MEDIA_CODEC_ID_H264;
    hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.bit_rate = 2500;
    hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.frame_rate = 24;
    hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.intra_period = 7;

    drv.surfaces[1].allocated = 1;
    drv.surfaces[1].width = 640;
    drv.surfaces[1].height = 368;
    VAEncSequenceParameterBufferH264 sequence = {0};
    sequence.level_idc = MC_H264_LEVEL4_1;
    sequence.ip_period = 1;
    sequence.max_num_ref_frames = 0;
    sequence.picture_width_in_mbs = 40;
    sequence.picture_height_in_mbs = 23;
    sequence.seq_fields.bits.chroma_format_idc = 1;
    sequence.seq_fields.bits.frame_mbs_only_flag = 1;
    VAEncSliceParameterBufferH264 slice = {0};
    slice.slice_type = 0;
    slice.num_macroblocks = 40 * 23;
    slice.macroblock_info = VA_INVALID_ID;

    drv.buffers[1].allocated = 1;
    drv.buffers[1].type = VAEncSequenceParameterBufferType;
    drv.buffers[1].data = &sequence;
    drv.buffers[1].size = sizeof(sequence);
    drv.buffers[1].element_size = sizeof(sequence);
    drv.buffers[1].num_elements = 1;
    drv.buffers[2].allocated = 1;
    drv.buffers[2].type = VAEncSliceParameterBufferType;
    drv.buffers[2].data = &slice;
    drv.buffers[2].size = sizeof(slice);
    drv.buffers[2].element_size = sizeof(slice);
    drv.buffers[2].num_elements = 1;

    mock_rate_control_calls = 0;
    mock_rate_control_result = 0;
    VABufferID sequence_then_slice[] = {1, 2};
    VABufferID slice_then_sequence[] = {2, 1};
    VABufferID sequence_id = 1;
    VABufferID slice_id = 2;
    VAStatus status = hobot_vaRenderPicture(&va_ctx, 1, sequence_then_slice, 2);
    if (status != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED || hctx->h264_sequence_valid ||
        mock_rate_control_calls != 0) {
        fprintf(stderr, "zero-reference H.264 P slice was not rejected before sequence commit: status=%d valid=%d calls=%d\n",
                status, hctx->h264_sequence_valid, mock_rate_control_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    status = hobot_vaRenderPicture(&va_ctx, 1, slice_then_sequence, 2);
    if (status != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED || hctx->h264_sequence_valid ||
        mock_rate_control_calls != 0) {
        fprintf(stderr, "zero-reference H.264 P-slice rejection depended on buffer order: status=%d valid=%d calls=%d\n",
                status, hctx->h264_sequence_valid, mock_rate_control_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    status = hobot_vaRenderPicture(&va_ctx, 1, &slice_id, 1);
    if (status != VA_STATUS_SUCCESS || !hctx->h264_encode_slice_valid ||
        hctx->h264_sequence_valid) {
        fprintf(stderr, "staged H.264 P slice was not recorded for later sequence validation: status=%d slice=%d sequence=%d\n",
                status, hctx->h264_encode_slice_valid, hctx->h264_sequence_valid);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    status = hobot_vaRenderPicture(&va_ctx, 1, &slice_id, 1);
    if (status != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED) {
        fprintf(stderr, "H.264 encoder accepted another slice from a separate RenderPicture call: status=%d\n",
                status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    status = hobot_vaRenderPicture(&va_ctx, 1, &sequence_id, 1);
    if (status != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED || hctx->h264_sequence_valid ||
        mock_rate_control_calls != 0) {
        fprintf(stderr, "zero-reference sequence accepted a P slice submitted in an earlier RenderPicture call: status=%d valid=%d calls=%d\n",
                status, hctx->h264_sequence_valid, mock_rate_control_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    hobot_finish_encoder_picture(hctx, 0);
    status = hobot_vaBeginPicture(&va_ctx, 1, 1);
    if (status != VA_STATUS_SUCCESS) {
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    slice.slice_type = 2;
    status = hobot_vaRenderPicture(&va_ctx, 1, &slice_id, 1);
    if (status != VA_STATUS_SUCCESS || !hctx->h264_encode_slice_valid ||
        hctx->h264_sequence_valid) {
        fprintf(stderr, "staged H.264 I slice was not retained before its sequence: status=%d slice=%d sequence=%d\n",
                status, hctx->h264_encode_slice_valid, hctx->h264_sequence_valid);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    status = hobot_vaRenderPicture(&va_ctx, 1, &sequence_id, 1);
    if (status != VA_STATUS_SUCCESS || !hctx->h264_sequence_valid ||
        hctx->h264_sequence.max_num_ref_frames != 0) {
        fprintf(stderr, "zero-reference H.264 I-slice sequence was rejected: status=%d valid=%d refs=%u\n",
                status, hctx->h264_sequence_valid,
                hctx->h264_sequence.max_num_ref_frames);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    hobot_finish_encoder_picture(hctx, 0);
    status = hobot_vaBeginPicture(&va_ctx, 1, 1);
    if (status != VA_STATUS_SUCCESS) {
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    status = hobot_vaEndPicture(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER) {
        fprintf(stderr, "H.264 encoder allowed a picture with no slice parameter: status=%d\n",
                status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    hobot_finish_encoder_picture(hctx, 0);
    status = hobot_vaBeginPicture(&va_ctx, 1, 1);
    if (status != VA_STATUS_SUCCESS) {
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    slice.slice_type = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, &slice_id, 1);
    int passed = status == VA_STATUS_ERROR_ATTR_NOT_SUPPORTED &&
                 hctx->h264_sequence_valid && !hctx->h264_encode_slice_valid &&
                 mock_rate_control_calls == 0;
    if (!passed)
        fprintf(stderr, "stored zero-reference H.264 configuration accepted a P slice: status=%d calls=%d\n",
                status, mock_rate_control_calls);

    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_jpeg_picture_quality_updates_vpu_and_rolls_back_on_error(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    uint8_t coded_data[16] = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->id = 1;
    hctx->vpu_running = 1;
    hctx->is_encoder = 1;
    hctx->encoder_picture_active = 1;
    hctx->profile = VAProfileJPEGBaseline;
    hctx->width = 640;
    hctx->height = 480;
    hctx->vpu_ctx.codec_id = MEDIA_CODEC_ID_JPEG;
    hctx->vpu_ctx.video_enc_params.jpeg_enc_config.quality_factor = 85;

    VAEncPictureParameterBufferJPEG picture = {0};
    picture.picture_width = 640;
    picture.picture_height = 480;
    picture.coded_buf = 2;
    picture.pic_flags.bits.huffman = 1;
    picture.pic_flags.bits.interleaved = 1;
    picture.sample_bit_depth = 8;
    picture.num_scan = 1;
    picture.num_components = 3;
    picture.component_id[0] = 1;
    picture.component_id[1] = 2;
    picture.component_id[2] = 3;
    picture.quantiser_table_selector[0] = 0;
    picture.quantiser_table_selector[1] = 1;
    picture.quantiser_table_selector[2] = 1;
    picture.quality = 37;
    VAQMatrixBufferJPEG qmatrix = {0};
    qmatrix.load_lum_quantiser_matrix = 1;
    qmatrix.load_chroma_quantiser_matrix = 1;
    for (unsigned int i = 0; i < 64; i++) {
        qmatrix.lum_quantiser_matrix[i] = (uint8_t)(i + 1);
        qmatrix.chroma_quantiser_matrix[i] = (uint8_t)(64 - i);
    }
    VAEncSliceParameterBufferJPEG slice = {0};
    slice.restart_interval = 13;
    slice.num_components = 3;
    slice.components[0].component_selector = 1;
    slice.components[1].component_selector = 2;
    slice.components[2].component_selector = 3;
    slice.components[0].dc_table_selector = 0;
    slice.components[0].ac_table_selector = 0;
    slice.components[1].dc_table_selector = 1;
    slice.components[1].ac_table_selector = 1;
    slice.components[2].dc_table_selector = 1;
    slice.components[2].ac_table_selector = 1;
    drv.buffers[1].allocated = 1;
    drv.buffers[1].type = VAEncPictureParameterBufferType;
    drv.buffers[1].data = &picture;
    drv.buffers[1].size = sizeof(picture);
    drv.buffers[1].element_size = sizeof(picture);
    drv.buffers[1].num_elements = 1;
    drv.buffers[2].allocated = 1;
    drv.buffers[2].type = VAEncCodedBufferType;
    drv.buffers[2].data = coded_data;
    drv.buffers[2].size = sizeof(coded_data);
    drv.buffers[2].capacity = sizeof(coded_data);
    drv.buffers[4].allocated = 1;
    drv.buffers[4].type = VAQMatrixBufferType;
    drv.buffers[4].data = &qmatrix;
    drv.buffers[4].size = sizeof(qmatrix);
    drv.buffers[4].element_size = sizeof(qmatrix);
    drv.buffers[4].num_elements = 1;
    drv.buffers[5].allocated = 1;
    drv.buffers[5].type = VAEncSliceParameterBufferType;
    drv.buffers[5].data = &slice;
    drv.buffers[5].size = sizeof(slice);
    drv.buffers[5].element_size = sizeof(slice);
    drv.buffers[5].num_elements = 1;
    VABufferID picture_ids[] = {1, 4, 5};

    mock_jpeg_get_result = 0;
    mock_jpeg_get_calls = 0;
    mock_jpeg_set_result = 0;
    mock_jpeg_set_calls = 0;
    mock_jpeg_current_quality = 85;
    mock_jpeg_restart_interval = 0;
    mock_jpeg_quality_factor = 0;
    memset(mock_jpeg_luma_quant_table, 0, sizeof(mock_jpeg_luma_quant_table));
    memset(mock_jpeg_chroma_quant_table, 0, sizeof(mock_jpeg_chroma_quant_table));
    VAStatus status = hobot_vaRenderPicture(&va_ctx, 1, picture_ids, 3);
    if (status != VA_STATUS_SUCCESS || mock_jpeg_get_calls != 1 ||
        mock_jpeg_set_calls != 1 ||
        mock_jpeg_quality_factor != 37 ||
        hctx->vpu_ctx.video_enc_params.jpeg_enc_config.quality_factor != 37 ||
        mock_jpeg_restart_interval != 13 ||
        hctx->enc_coded_buf != 2 ||
        memcmp(mock_jpeg_luma_quant_table, qmatrix.lum_quantiser_matrix, 64) != 0 ||
        memcmp(mock_jpeg_chroma_quant_table, qmatrix.chroma_quantiser_matrix, 64) != 0) {
        fprintf(stderr, "JPEG picture parameters were not applied: status=%d get/set=%d/%d quality=%u context=%u coded=%u\n",
                status, mock_jpeg_get_calls, mock_jpeg_set_calls, mock_jpeg_quality_factor,
                hctx->vpu_ctx.video_enc_params.jpeg_enc_config.quality_factor,
                hctx->enc_coded_buf);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    picture.quality = 101;
    status = hobot_vaRenderPicture(&va_ctx, 1, picture_ids, 3);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER || mock_jpeg_get_calls != 1 ||
        mock_jpeg_set_calls != 1 ||
        hctx->vpu_ctx.video_enc_params.jpeg_enc_config.quality_factor != 37) {
        fprintf(stderr, "out-of-range JPEG quality was accepted: status=%d get/set=%d/%d quality=%u\n",
                status, mock_jpeg_get_calls, mock_jpeg_set_calls,
                hctx->vpu_ctx.video_enc_params.jpeg_enc_config.quality_factor);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    picture.quality = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, picture_ids, 3);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER || mock_jpeg_get_calls != 1 ||
        mock_jpeg_set_calls != 1 ||
        hctx->vpu_ctx.video_enc_params.jpeg_enc_config.quality_factor != 37) {
        fprintf(stderr, "zero JPEG quality was accepted: status=%d get/set=%d/%d quality=%u\n",
                status, mock_jpeg_get_calls, mock_jpeg_set_calls,
                hctx->vpu_ctx.video_enc_params.jpeg_enc_config.quality_factor);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    picture.quality = 62;
    mock_jpeg_get_result = -1;
    status = hobot_vaRenderPicture(&va_ctx, 1, picture_ids, 3);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || mock_jpeg_get_calls != 2 ||
        mock_jpeg_set_calls != 1 ||
        hctx->vpu_ctx.video_enc_params.jpeg_enc_config.quality_factor != 37) {
        fprintf(stderr, "JPEG config query failure changed state: status=%d get/set=%d/%d quality=%u\n",
                status, mock_jpeg_get_calls, mock_jpeg_set_calls,
                hctx->vpu_ctx.video_enc_params.jpeg_enc_config.quality_factor);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    mock_jpeg_get_result = 0;
    mock_jpeg_set_result = -1;
    status = hobot_vaRenderPicture(&va_ctx, 1, picture_ids, 3);
    int passed = status == VA_STATUS_ERROR_OPERATION_FAILED && mock_jpeg_get_calls == 3 &&
                 mock_jpeg_set_calls == 2 &&
                 hctx->vpu_ctx.video_enc_params.jpeg_enc_config.quality_factor == 37 &&
                 hctx->enc_coded_buf == 2;
    if (!passed)
        fprintf(stderr, "failed JPEG quality update did not preserve state: status=%d get/set=%d/%d quality=%u coded=%u\n",
                status, mock_jpeg_get_calls, mock_jpeg_set_calls,
                hctx->vpu_ctx.video_enc_params.jpeg_enc_config.quality_factor,
                hctx->enc_coded_buf);

    mock_jpeg_set_result = 0;
    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_jpeg_empty_app9_is_stripped_only_when_exact(void)
{
    static const uint8_t vendor_jpeg[] = {
        0xff, 0xd8, 0xff, 0xe9, 0x00, 0x04, 0x00, 0x00,
        0xff, 0xdb, 0x12, 0x34, 0xff, 0xd9
    };
    static const uint8_t expected[] = {
        0xff, 0xd8, 0xff, 0xdb, 0x12, 0x34, 0xff, 0xd9
    };
    uint8_t output[sizeof(vendor_jpeg)];
    memcpy(output, vendor_jpeg, sizeof(vendor_jpeg));
    unsigned int output_size = hobot_jpeg_strip_empty_app9(output, sizeof(vendor_jpeg));
    if (output_size != sizeof(expected) || memcmp(output, expected, sizeof(expected)) != 0) {
        fprintf(stderr, "JPEG empty APP9 marker was not removed intact\n");
        return 0;
    }

    static const uint8_t identified_app9[] = {
        0xff, 0xd8, 0xff, 0xe9, 0x00, 0x06, 0x41, 0x42, 0xff, 0xd9
    };
    memcpy(output, identified_app9, sizeof(identified_app9));
    output_size = hobot_jpeg_strip_empty_app9(output, sizeof(identified_app9));
    if (output_size != sizeof(identified_app9) ||
        memcmp(output, identified_app9, sizeof(identified_app9)) != 0) {
        fprintf(stderr, "JPEG APP9 payload was modified unexpectedly\n");
        return 0;
    }

    static const uint8_t other_app[] = {
        0xff, 0xd8, 0xff, 0xe8, 0x00, 0x04, 0x00, 0x00, 0xff, 0xd9
    };
    memcpy(output, other_app, sizeof(other_app));
    output_size = hobot_jpeg_strip_empty_app9(output, sizeof(other_app));
    if (output_size != sizeof(other_app) || memcmp(output, other_app, sizeof(other_app)) != 0) {
        fprintf(stderr, "non-APP9 marker was modified unexpectedly\n");
        return 0;
    }

    memcpy(output, vendor_jpeg, 7);
    output_size = hobot_jpeg_strip_empty_app9(output, 7);
    if (output_size != 7) {
        fprintf(stderr, "truncated JPEG size was modified unexpectedly\n");
        return 0;
    }
    return 1;
}

static int test_jpeg_huffman_accepts_only_hardware_defaults(void)
{
    VAHuffmanTableBufferJPEGBaseline tables = {0};
    for (unsigned int table = 0; table < 2; table++) {
        tables.load_huffman_table[table] = 1;
        memcpy(tables.huffman_table[table].num_dc_codes,
               jpeg_default_dc_bits[table], 16);
        memcpy(tables.huffman_table[table].dc_values,
               jpeg_default_dc_values[table], 12);
        memcpy(tables.huffman_table[table].num_ac_codes,
               jpeg_default_ac_bits[table], 16);
        memcpy(tables.huffman_table[table].ac_values,
               jpeg_default_ac_values[table], 162);
    }
    if (!hobot_jpeg_huffman_tables_supported(&tables)) {
        fprintf(stderr, "standard JPEG Huffman tables were rejected\n");
        return 0;
    }
    tables.huffman_table[1].ac_values[0] ^= 1;
    if (hobot_jpeg_huffman_tables_supported(&tables)) {
        fprintf(stderr, "custom JPEG Huffman table was silently accepted\n");
        return 0;
    }
    tables.load_huffman_table[1] = 0;
    return hobot_jpeg_huffman_tables_supported(&tables);
}

static int test_jpeg_decode_header_is_bounded_baseline_420(void)
{
    VAPictureParameterBufferJPEGBaseline picture = {0};
    VAIQMatrixBufferJPEGBaseline qmatrix = {0};
    VAHuffmanTableBufferJPEGBaseline huffman = {0};
    VASliceParameterBufferJPEGBaseline slice = {0};
    uint8_t header[1024];
    size_t header_size = 0;

    picture.picture_width = 32;
    picture.picture_height = 16;
    picture.num_components = 3;
    for (unsigned int component = 0; component < 3; component++) {
        picture.components[component].component_id = (uint8_t)(component + 1);
        picture.components[component].h_sampling_factor = component == 0 ? 2 : 1;
        picture.components[component].v_sampling_factor = component == 0 ? 2 : 1;
        picture.components[component].quantiser_table_selector = component == 0 ? 0 : 1;
    }
    for (unsigned int table = 0; table < 2; table++) {
        qmatrix.load_quantiser_table[table] = 1;
        memset(qmatrix.quantiser_table[table], 7 + table, 64);
        huffman.load_huffman_table[table] = 1;
        memcpy(huffman.huffman_table[table].num_dc_codes,
               jpeg_default_dc_bits[table], 16);
        memcpy(huffman.huffman_table[table].dc_values,
               jpeg_default_dc_values[table], 12);
        memcpy(huffman.huffman_table[table].num_ac_codes,
               jpeg_default_ac_bits[table], 16);
        memcpy(huffman.huffman_table[table].ac_values,
               jpeg_default_ac_values[table], 162);
    }
    slice.slice_data_size = 64;
    slice.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    slice.num_components = 3;
    slice.num_mcus = 2;
    for (unsigned int component = 0; component < 3; component++) {
        slice.components[component].component_selector = (uint8_t)(component + 1);
        slice.components[component].dc_table_selector = component == 0 ? 0 : 1;
        slice.components[component].ac_table_selector = component == 0 ? 0 : 1;
    }

    VAStatus status = hobot_jpeg_build_decode_header(
        &picture, &qmatrix, &huffman, &slice, 32, 16,
        header, sizeof(header), &header_size);
    if (status != VA_STATUS_SUCCESS || header_size < 16 ||
        header[0] != 0xff || header[1] != 0xd8 ||
        header[2] != 0xff || header[3] != 0xdb ||
        header[header_size - 14] != 0xff || header[header_size - 13] != 0xda) {
        fprintf(stderr, "valid JPEG baseline 4:2:0 header synthesis failed: status=%d size=%zu\n",
                status, header_size);
        return 0;
    }

    const uint32_t rotations[] = {
        VA_ROTATION_NONE, VA_ROTATION_90,
        VA_ROTATION_180, VA_ROTATION_270
    };
    for (size_t i = 0; i < sizeof(rotations) / sizeof(rotations[0]); i++) {
        picture.rotation = rotations[i];
        int swapped = rotations[i] == VA_ROTATION_90 ||
                      rotations[i] == VA_ROTATION_270;
        status = hobot_jpeg_build_decode_header(
            &picture, &qmatrix, &huffman, &slice,
            swapped ? 16 : 32, swapped ? 32 : 16,
            header, sizeof(header), &header_size);
        if (status != VA_STATUS_SUCCESS) {
            fprintf(stderr, "JPEG rotation %u rejected with swapped geometry: status=%d\n",
                    rotations[i], status);
            return 0;
        }
    }
    picture.rotation = VA_ROTATION_NONE;
    if (hobot_jpeg_build_decode_header(
            &picture, &qmatrix, &huffman, &slice, 16, 32,
            header, sizeof(header), &header_size) !=
        VA_STATUS_ERROR_INVALID_PARAMETER) {
        fprintf(stderr, "JPEG rotation geometry mismatch was accepted\n");
        return 0;
    }
    picture.rotation = 4;
    if (hobot_jpeg_build_decode_header(
            &picture, &qmatrix, &huffman, &slice, 32, 16,
            header, sizeof(header), &header_size) !=
        VA_STATUS_ERROR_ATTR_NOT_SUPPORTED) {
        fprintf(stderr, "unsupported JPEG rotation value was accepted\n");
        return 0;
    }
    picture.rotation = VA_ROTATION_NONE;

    picture.components[0].v_sampling_factor = 1;
    slice.num_mcus = 4;
    if (hobot_jpeg_build_decode_header(
            &picture, &qmatrix, &huffman, &slice, 32, 16,
            header, sizeof(header), &header_size) != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED) {
        fprintf(stderr, "JPEG 4:2:2 sampling was not rejected\n");
        return 0;
    }
    picture.components[0].v_sampling_factor = 2;
    picture.components[1].v_sampling_factor = 2;
    picture.components[2].v_sampling_factor = 2;
    slice.num_mcus = 2;
    if (hobot_jpeg_build_decode_header(
            &picture, &qmatrix, &huffman, &slice, 32, 16,
            header, sizeof(header), &header_size) != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED) {
        fprintf(stderr, "normalized JPEG 4:2:2 sampling was not rejected\n");
        return 0;
    }
    picture.components[0].h_sampling_factor = 1;
    if (hobot_jpeg_build_decode_header(
            &picture, &qmatrix, &huffman, &slice, 32, 16,
            header, sizeof(header), &header_size) != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED) {
        fprintf(stderr, "JPEG 4:4:4 sampling was not rejected\n");
        return 0;
    }
    picture.components[0].h_sampling_factor = 2;
    picture.components[1].v_sampling_factor = 1;
    picture.components[2].v_sampling_factor = 1;
    slice.num_mcus = 2;

    for (unsigned int component = 0; component < 3; component++) {
        picture.components[component].component_id = (uint8_t)component;
        picture.components[component].quantiser_table_selector = 0;
        slice.components[component].component_selector = (uint8_t)component;
    }
    memset(qmatrix.quantiser_table[1], 0, sizeof(qmatrix.quantiser_table[1]));
    status = hobot_jpeg_build_decode_header(
        &picture, &qmatrix, &huffman, &slice, 32, 16,
        header, sizeof(header), &header_size);
    if (status != VA_STATUS_SUCCESS || header[81] != 0 || header[84] != 1 ||
        header[87] != 2 || header[83] != 0 || header[86] != 0 || header[89] != 0) {
        fprintf(stderr, "JPEG 4:2:0 with zero-based component IDs/single quantizer failed: status=%d\n",
                status);
        return 0;
    }

    slice.num_mcus++;
    if (hobot_jpeg_build_decode_header(
            &picture, &qmatrix, &huffman, &slice, 32, 16,
            header, sizeof(header), &header_size) != VA_STATUS_ERROR_INVALID_PARAMETER) {
        fprintf(stderr, "invalid JPEG MCU count was not rejected\n");
        return 0;
    }
    slice.num_mcus--;
    huffman.huffman_table[0].num_dc_codes[0] = 0xff;
    if (hobot_jpeg_build_decode_header(
            &picture, &qmatrix, &huffman, &slice, 32, 16,
            header, sizeof(header), &header_size) != VA_STATUS_ERROR_INVALID_PARAMETER) {
        fprintf(stderr, "malformed JPEG Huffman table was not rejected\n");
        return 0;
    }
    memcpy(huffman.huffman_table[0].num_dc_codes,
           jpeg_default_dc_bits[0], 16);
    if (hobot_jpeg_build_decode_header(
            &picture, &qmatrix, &huffman, &slice, 32, 16,
            header, 8, &header_size) != VA_STATUS_ERROR_MAX_NUM_EXCEEDED) {
        fprintf(stderr, "undersized JPEG header output was not rejected\n");
        return 0;
    }
    return 1;
}

static int test_jpeg_decode_rejected_parameters_do_not_mutate_cache(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    VAPictureParameterBufferJPEGBaseline picture = {0};
    VAIQMatrixBufferJPEGBaseline qmatrix = {0};
    VAHuffmanTableBufferJPEGBaseline huffman = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->profile = VAProfileJPEGBaseline;
    hctx->width = 32;
    hctx->height = 16;
    hctx->jpeg_init_deferred = 1;
    hctx->decode_picture_active = 1;

    qmatrix.load_quantiser_table[0] = 1;
    memset(qmatrix.quantiser_table[0], 7, sizeof(qmatrix.quantiser_table[0]));
    qmatrix.load_quantiser_table[1] = 2;
    drv.buffers[1] = (HobotBuffer){
        .id = 1, .allocated = 1, .type = VAIQMatrixBufferType,
        .size = sizeof(qmatrix), .capacity = sizeof(qmatrix),
        .element_size = sizeof(qmatrix), .num_elements = 1, .data = &qmatrix
    };
    drv.buffers[2] = (HobotBuffer){
        .id = 2, .allocated = 1, .type = VAPictureParameterBufferType,
        .size = sizeof(picture), .capacity = sizeof(picture),
        .element_size = sizeof(picture), .num_elements = 1, .data = &picture
    };

    VABufferID buffer_ids[] = {2, 1};
    VAStatus status = hobot_vaRenderPicture(&va_ctx, 1, buffer_ids, 2);
    int passed = status == VA_STATUS_ERROR_INVALID_PARAMETER &&
        !hctx->jpeg_decode_picture_valid &&
        !hctx->jpeg_decode_qmatrix_valid[0] &&
        hctx->jpeg_decode_qmatrix.quantiser_table[0][0] == 0;
    if (!passed)
        fprintf(stderr, "rejected JPEG parameters changed cached state: status=%d picture=%d qvalid=%u qvalue=%u\n",
                status, hctx->jpeg_decode_picture_valid,
                hctx->jpeg_decode_qmatrix_valid[0],
                hctx->jpeg_decode_qmatrix.quantiser_table[0][0]);

    huffman.load_huffman_table[0] = 1;
    memcpy(huffman.huffman_table[0].num_dc_codes,
           jpeg_default_dc_bits[0], 16);
    memcpy(huffman.huffman_table[0].dc_values,
           jpeg_default_dc_values[0], 12);
    memcpy(huffman.huffman_table[0].num_ac_codes,
           jpeg_default_ac_bits[0], 16);
    memcpy(huffman.huffman_table[0].ac_values,
           jpeg_default_ac_values[0], 162);
    huffman.load_huffman_table[1] = 2;
    drv.buffers[1] = (HobotBuffer){
        .id = 1, .allocated = 1, .type = VAHuffmanTableBufferType,
        .size = sizeof(huffman), .capacity = sizeof(huffman),
        .element_size = sizeof(huffman), .num_elements = 1, .data = &huffman
    };
    VABufferID huffman_id = 1;
    status = hobot_vaRenderPicture(&va_ctx, 1, &huffman_id, 1);
    passed = passed && status == VA_STATUS_ERROR_INVALID_PARAMETER &&
        !hctx->jpeg_decode_huffman_valid[0];
    if (!passed)
        fprintf(stderr, "rejected JPEG Huffman buffer changed cached table 0: status=%d valid=%u\n",
                status, hctx->jpeg_decode_huffman_valid[0]);

    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_jpeg_rotation_mapping_and_deferred_start(void)
{
    const struct {
        uint32_t va_rotation;
        mc_rotate_degree_t sdk_rotation;
    } cases[] = {
        {VA_ROTATION_NONE, MC_CCW_0},
        {VA_ROTATION_90, MC_CCW_270},
        {VA_ROTATION_180, MC_CCW_180},
        {VA_ROTATION_270, MC_CCW_90}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        mc_rotate_degree_t sdk_rotation;
        if (!hobot_jpeg_rotation_to_sdk(cases[i].va_rotation, &sdk_rotation) ||
            sdk_rotation != cases[i].sdk_rotation) {
            fprintf(stderr, "JPEG VA-to-SDK rotation mapping failed for %u\n",
                    cases[i].va_rotation);
            return 0;
        }
    }
    mc_rotate_degree_t unused_rotation;
    if (hobot_jpeg_rotation_to_sdk(4, &unused_rotation)) {
        fprintf(stderr, "unsupported JPEG rotation mapped to SDK\n");
        return 0;
    }

    HobotDriverData drv = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    struct VADriverContext va_ctx = {0};
    va_ctx.pDriverData = &drv;
    drv.configs[1].allocated = 1;
    drv.configs[1].profile = VAProfileJPEGBaseline;
    drv.configs[1].entrypoint = VAEntrypointVLD;

    int saved_initialize_result = mock_initialize_result;
    int saved_initialize_calls = mock_initialize_calls;
    int saved_configure_result = mock_configure_result;
    int saved_configure_calls = mock_configure_calls;
    int saved_start_result = mock_start_result;
    int saved_start_calls = mock_start_calls;
    int saved_stop_result = mock_stop_result;
    int saved_stop_calls = mock_stop_calls;
    int saved_release_result = mock_release_result;
    int saved_release_calls = mock_release_calls;
    mock_initialize_result = 0;
    mock_configure_result = 0;
    mock_start_result = 0;
    mock_stop_result = 0;
    mock_release_result = 0;

    VAContextID context = VA_INVALID_ID;
    VAStatus status = hobot_vaCreateContext(&va_ctx, 1, 640, 480, 0,
                                            NULL, 0, &context);
    if (status != VA_STATUS_SUCCESS || context <= 0 ||
        !drv.contexts[context].jpeg_init_deferred ||
        drv.contexts[context].vpu_initialized || drv.contexts[context].vpu_running ||
        mock_initialize_calls != saved_initialize_calls ||
        mock_configure_calls != saved_configure_calls ||
        mock_start_calls != saved_start_calls) {
        fprintf(stderr, "JPEG context did not defer VPU startup: status=%d context=%u init=%d/%d\n",
                status, context, drv.contexts[context].jpeg_init_deferred,
                mock_initialize_calls - saved_initialize_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    HobotContext *hctx = &drv.contexts[context];
    if (hobot_start_deferred_jpeg_decoder(hctx, VA_ROTATION_90) != 0 ||
        !hctx->vpu_initialized || !hctx->vpu_running ||
        hctx->jpeg_init_deferred || !hctx->jpeg_rotation_fixed ||
        hctx->jpeg_rotation != VA_ROTATION_90 ||
        hctx->vpu_ctx.video_dec_params.jpeg_dec_config.rot_degree != MC_CCW_270 ||
        mock_initialize_calls != saved_initialize_calls + 1 ||
        mock_configure_calls != saved_configure_calls + 1 ||
        mock_start_calls != saved_start_calls + 1 ||
        hobot_start_deferred_jpeg_decoder(hctx, VA_ROTATION_90) != 0 ||
        hobot_start_deferred_jpeg_decoder(hctx, VA_ROTATION_270) == 0 ||
        mock_initialize_calls != saved_initialize_calls + 1) {
        fprintf(stderr, "deferred JPEG startup or fixed-angle contract failed\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    status = hobot_vaDestroyContext(&va_ctx, context);
    if (status != VA_STATUS_SUCCESS || drv.contexts[context].allocated) {
        fprintf(stderr, "rotated JPEG context teardown failed: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    context = VA_INVALID_ID;
    status = hobot_vaCreateContext(&va_ctx, 1, 640, 480, 0,
                                   NULL, 0, &context);
    if (status != VA_STATUS_SUCCESS || context <= 0) {
        fprintf(stderr, "JPEG context recreation failed: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    hctx = &drv.contexts[context];
    mock_configure_result = -7;
    mock_release_result = -11;
    int releases_before_failure = mock_release_calls;
    if (hobot_start_deferred_jpeg_decoder(hctx, VA_ROTATION_270) != -7 ||
        hctx->jpeg_init_deferred || hctx->vpu_running || !hctx->vpu_initialized ||
        !hctx->decode_failed || mock_release_calls != releases_before_failure + 1) {
        fprintf(stderr, "JPEG start failure did not preserve initialized ownership\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    if (hobot_vaDestroyContext(&va_ctx, context) != VA_STATUS_ERROR_OPERATION_FAILED ||
        !hctx->allocated || !hctx->vpu_initialized) {
        fprintf(stderr, "JPEG release failure was not preserved for retry\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    mock_release_result = 0;
    if (hobot_vaDestroyContext(&va_ctx, context) != VA_STATUS_SUCCESS ||
        hctx->allocated || hctx->vpu_initialized) {
        fprintf(stderr, "JPEG release retry failed\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    mock_initialize_result = saved_initialize_result;
    mock_initialize_calls = saved_initialize_calls;
    mock_configure_result = saved_configure_result;
    mock_configure_calls = saved_configure_calls;
    mock_start_result = saved_start_result;
    mock_start_calls = saved_start_calls;
    mock_stop_result = saved_stop_result;
    mock_stop_calls = saved_stop_calls;
    mock_release_result = saved_release_result;
    mock_release_calls = saved_release_calls;
    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static int test_encoder_end_picture_coded_buffer_contract(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    uint8_t coded_data[8];
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->is_encoder = 1;
    hctx->vpu_initialized = 1;
    hctx->vpu_running = 1;
    hctx->current_render_target = VA_INVALID_SURFACE;
    hctx->vpu_ctx.video_enc_params.width = 2;
    hctx->vpu_ctx.video_enc_params.height = 2;

    HobotSurface *surface = &drv.surfaces[1];
    surface->allocated = 1;
    surface->width = 2;
    surface->height = 2;
    surface->stride = 2;

    HobotBuffer *coded = &drv.buffers[2];
    coded->allocated = 1;
    coded->type = VAImageBufferType;
    coded->data = coded_data;
    coded->size = sizeof(coded_data);
    coded->capacity = sizeof(coded_data);

    mock_encoder_io = 1;
    mock_encoder_input_size = sizeof(mock_encoder_y) + sizeof(mock_encoder_uv);
    mock_encoder_input_stride = 8;
    mock_encoder_input_vstride = 8;
    mock_encoded_size = 4;
    mock_dequeue_result = 0;
    mock_dequeue_input_result = 0;
    mock_queue_input_result = 0;
    mock_queue_output_result = 0;
    mock_stop_calls = 0;
    mock_release_calls = 0;
    mock_stop_result = 0;
    mock_release_result = 0;
    mock_dequeue_input_calls = 0;
    mock_dequeue_output_calls = 0;
    mock_queue_input_calls = 0;
    mock_queue_output_calls = 0;
    recycled_output_count = 0;
    for (unsigned int i = 0; i < sizeof(mock_encoded_output); i++)
        mock_encoded_output[i] = (uint8_t)(0x30 + i);

    VAStatus status = hobot_vaBeginPicture(&va_ctx, 1, 1);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "encoder begin failed in coded-buffer contract test: status=%d\n", status);
        goto fail;
    }
    hctx->enc_coded_buf = 2;
    if (hobot_vaBeginPicture(&va_ctx, 1, 1) != VA_STATUS_ERROR_OPERATION_FAILED) {
        fprintf(stderr, "encoder accepted overlapping begin-picture calls\n");
        goto fail;
    }
    VASurfaceID destroy_while_encoding = 1;
    status = hobot_vaDestroySurfaces(&va_ctx, &destroy_while_encoding, 1);
    if (status != VA_STATUS_ERROR_SURFACE_BUSY) {
        fprintf(stderr, "active encoder surface was destroyable: status=%d\n", status);
        goto fail;
    }

    status = hobot_vaEndPicture(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_INVALID_BUFFER || mock_dequeue_input_calls != 0 ||
        mock_queue_input_calls != 0 || mock_dequeue_output_calls != 0) {
        fprintf(stderr, "encoder accepted a coded buffer with the wrong type: status=%d deq_in=%d queue_in=%d deq_out=%d\n",
                status, mock_dequeue_input_calls, mock_queue_input_calls,
                mock_dequeue_output_calls);
        goto fail;
    }

    coded->type = VAEncCodedBufferType;
    coded->data = NULL;
    status = hobot_vaEndPicture(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_INVALID_BUFFER || mock_dequeue_input_calls != 0 ||
        mock_queue_input_calls != 0 || mock_dequeue_output_calls != 0) {
        fprintf(stderr, "encoder accepted a coded buffer without backing storage: status=%d\n", status);
        goto fail;
    }

    coded->data = coded_data;
    coded->size = sizeof(coded_data);
    coded->capacity = sizeof(coded_data) - 1;
    status = hobot_vaEndPicture(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_INVALID_BUFFER || mock_dequeue_input_calls != 0 ||
        mock_queue_input_calls != 0 || mock_dequeue_output_calls != 0) {
        fprintf(stderr, "encoder accepted inconsistent coded buffer capacity: status=%d\n", status);
        goto fail;
    }

    coded->capacity = sizeof(coded_data);
    coded->size = 4;
    coded->coded_segment.size = 3;
    coded->coded_segment.bit_offset = 2;
    coded->coded_segment.status = 4;
    coded->coded_segment.buf = mock_encoded_output;
    coded->coded_segment.next = (VACodedBufferSegment *)coded;
    hctx->vpu_running = 0;
    status = hobot_vaEndPicture(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || coded->coded_segment.size != 3 ||
        coded->coded_segment.bit_offset != 2 || coded->coded_segment.status != 4 ||
        coded->coded_segment.buf != mock_encoded_output ||
        coded->coded_segment.next != (VACodedBufferSegment *)coded ||
        mock_dequeue_input_calls != 0 || mock_queue_input_calls != 0 ||
        mock_dequeue_output_calls != 0) {
        fprintf(stderr, "stopped encoder destroyed prior coded output: status=%d segment=%u\n",
                status, coded->coded_segment.size);
        goto fail;
    }
    hctx->vpu_running = 1;

    surface->width = 1;
    status = hobot_vaEndPicture(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_INVALID_SURFACE || mock_dequeue_input_calls != 0) {
        fprintf(stderr, "encoder accepted a surface smaller than the coded frame: status=%d\n", status);
        goto fail;
    }
    surface->width = 2;

    surface->raw_data = mock_frame;
    surface->raw_data_valid = 1;
    surface->raw_data_size = 1;
    status = hobot_vaEndPicture(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || mock_dequeue_input_calls != 0) {
        fprintf(stderr, "encoder read past undersized staging storage: status=%d\n", status);
        goto fail;
    }
    surface->raw_data = NULL;
    surface->raw_data_valid = 0;
    surface->raw_data_size = 0;

    coded->coded_segment.size = 99;
    coded->coded_segment.bit_offset = 8;
    coded->coded_segment.status = 1;
    coded->coded_segment.buf = coded_data;
    coded->coded_segment.next = (VACodedBufferSegment *)coded;
    memset(coded_data, 0xa5, sizeof(coded_data));

    mock_encoder_input_size = 23;
    mock_encoded_size = 4;
    status = hobot_vaEndPicture(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || coded->coded_segment.size != 0 ||
        mock_dequeue_output_calls != 0 || mock_queue_input_calls != 1) {
        fprintf(stderr, "encoder accepted a truncated input allocation: status=%d deq_out=%d queued_in=%d\n",
                status, mock_dequeue_output_calls, mock_queue_input_calls);
        goto fail;
    }

    mock_encoder_input_size = sizeof(mock_encoder_y) + sizeof(mock_encoder_uv);
    mock_encoder_input_stride = 1;
    status = hobot_vaEndPicture(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || coded->coded_segment.size != 0 ||
        mock_dequeue_output_calls != 0 || mock_queue_input_calls != 2) {
        fprintf(stderr, "encoder accepted an undersized input stride: status=%d deq_out=%d queued_in=%d\n",
                status, mock_dequeue_output_calls, mock_queue_input_calls);
        goto fail;
    }

    mock_encoder_input_stride = 8;
    mock_encoder_input_vstride = 4;
    mock_encoder_input_size = 24;
    mock_encoded_size = sizeof(coded_data) + 1;
    status = hobot_vaEndPicture(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_MAX_NUM_EXCEEDED || coded->coded_segment.size != 0 ||
        coded->coded_segment.bit_offset != 0 || coded->coded_segment.status != 0 ||
        coded->coded_segment.buf != coded_data || coded->coded_segment.next != NULL ||
        memcmp(coded_data, "\xa5\xa5\xa5\xa5\xa5\xa5\xa5\xa5", sizeof(coded_data)) != 0 ||
        mock_queue_output_calls != 1 || recycled_output_count != 1) {
        fprintf(stderr, "oversized encoded output was not rejected intact: status=%d segment=%u recycled=%d/%d\n",
                status, coded->coded_segment.size, mock_queue_output_calls,
                recycled_output_count);
        goto fail;
    }

    status = hobot_vaBeginPicture(&va_ctx, 1, 1);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "encoder begin after oversized output failed: status=%d\n", status);
        goto fail;
    }
    hctx->enc_coded_buf = 2;
    mock_encoded_size = coded->size;
    status = hobot_vaEndPicture(&va_ctx, 1);
    if (status != VA_STATUS_SUCCESS || coded->coded_segment.size != mock_encoded_size ||
        coded->coded_segment.buf != coded_data || coded->coded_segment.next != NULL ||
        memcmp(coded_data, mock_encoded_output, mock_encoded_size) != 0 ||
        mock_queue_output_calls != 2 || recycled_output_count != 2) {
        fprintf(stderr, "valid encoded output was not fully published: status=%d segment=%u expected=%u recycled=%d/%d\n",
                status, coded->coded_segment.size, mock_encoded_size,
                mock_queue_output_calls, recycled_output_count);
        goto fail;
    }
    int output_dequeues = mock_dequeue_output_calls;
    if (hobot_vaEndPicture(&va_ctx, 1) != VA_STATUS_ERROR_OPERATION_FAILED ||
        mock_dequeue_output_calls != output_dequeues) {
        fprintf(stderr, "encoder accepted a duplicate end-picture call\n");
        goto fail;
    }

    status = hobot_vaBeginPicture(&va_ctx, 1, 1);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "encoder begin after successful output failed: status=%d\n", status);
        goto fail;
    }
    hctx->enc_coded_buf = 2;
    coded->coded_segment.size = 99;
    mock_queue_output_result = -1;
    status = hobot_vaEndPicture(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || coded->coded_segment.size != 0 ||
        coded->coded_segment.buf != coded_data || coded->coded_segment.next != NULL ||
        !hctx->encoder_failed || hctx->encoder_picture_active || !hctx->enc_out_buf_valid ||
        hobot_vaBeginPicture(&va_ctx, 1, 1) != VA_STATUS_ERROR_OPERATION_FAILED) {
        fprintf(stderr, "failed encoded-output recycle published a segment: status=%d segment=%u\n",
                status, coded->coded_segment.size);
        goto fail;
    }

    status = hobot_vaDestroyContext(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || !hctx->allocated ||
        !hctx->enc_out_buf_valid || mock_stop_calls != 0 || mock_release_calls != 0) {
        fprintf(stderr, "failed encoder output retry discarded ownership: status=%d retained=%d stop=%d release=%d\n",
                status, hctx->enc_out_buf_valid, mock_stop_calls, mock_release_calls);
        goto fail;
    }
    mock_queue_output_result = 0;
    status = hobot_vaDestroyContext(&va_ctx, 1);
    if (status != VA_STATUS_SUCCESS || hctx->allocated || hctx->enc_out_buf_valid ||
        mock_stop_calls != 1 || mock_release_calls != 1) {
        fprintf(stderr, "encoder output ownership teardown retry failed: status=%d allocated=%d retained=%d stop=%d release=%d\n",
                status, hctx->allocated, hctx->enc_out_buf_valid,
                mock_stop_calls, mock_release_calls);
        goto fail;
    }

    mock_encoder_io = 0;
    mock_encoder_input_size = sizeof(mock_encoder_y) + sizeof(mock_encoder_uv);
    mock_encoder_input_stride = 8;
    mock_encoder_input_vstride = 8;
    mock_queue_output_result = 0;
    mock_queue_input_result = 0;
    mock_queue_output_calls = 0;
    recycled_output_count = 0;
    pthread_mutex_destroy(&drv.mutex);
    return 1;

fail:
    mock_encoder_io = 0;
    mock_encoder_input_size = sizeof(mock_encoder_y) + sizeof(mock_encoder_uv);
    mock_encoder_input_stride = 8;
    mock_encoder_input_vstride = 8;
    mock_queue_output_result = 0;
    mock_queue_input_result = 0;
    mock_queue_output_calls = 0;
    recycled_output_count = 0;
    pthread_mutex_destroy(&drv.mutex);
    return 0;
}

static int test_external_encoder_consumed_callback_is_single_flight(void)
{
    HobotContext hctx = {0};
    media_codec_buffer_t buffer = {0};
    uintptr_t first_generation = hobot_next_external_input_token();
    uintptr_t second_generation = hobot_next_external_input_token();
    if (first_generation == 0 || second_generation == 0 ||
        first_generation == second_generation) {
        fprintf(stderr, "external input callback tokens are not unique\n");
        return 0;
    }
    atomic_init(&hctx.enc_external_input_pending, 17);
    buffer.user_ptr = (hb_ptr)(uintptr_t)16;
    buffer.vframe_buf.src_idx = 3;

    hobot_encoder_input_consumed(&hctx, &buffer);
    if (atomic_load_explicit(&hctx.enc_external_input_pending,
                             memory_order_acquire) != 17) {
        fprintf(stderr, "stale input callback released a newer owner\n");
        return 0;
    }

    buffer.user_ptr = (hb_ptr)(uintptr_t)17;
    hobot_encoder_input_consumed(&hctx, &buffer);
    if (atomic_load_explicit(&hctx.enc_external_input_pending,
                             memory_order_acquire) != 0) {
        fprintf(stderr, "matching input callback did not release its owner\n");
        return 0;
    }

    atomic_store_explicit(&hctx.enc_external_input_pending, 23,
                          memory_order_release);
    buffer.user_ptr = NULL;
    hobot_encoder_input_consumed(&hctx, &buffer);
    buffer.user_ptr = (hb_ptr)(uintptr_t)17;
    hobot_encoder_input_consumed(&hctx, &buffer);
    if (atomic_load_explicit(&hctx.enc_external_input_pending,
                             memory_order_acquire) != 23) {
        fprintf(stderr, "missing or stale callback cleared external ownership\n");
        return 0;
    }

    buffer.user_ptr = (hb_ptr)(uintptr_t)23;
    hobot_encoder_input_consumed(&hctx, &buffer);
    if (atomic_load_explicit(&hctx.enc_external_input_pending,
                             memory_order_acquire) != 0) {
        fprintf(stderr, "new matching callback did not release external ownership\n");
        return 0;
    }
    return 1;
}

static int test_external_encoder_output_error_preserves_surface_ownership(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    uint8_t coded_data[16] = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->id = 1;
    hctx->allocated = 1;
    hctx->is_encoder = 1;
    hctx->profile = VAProfileJPEGBaseline;
    hctx->vpu_initialized = 1;
    hctx->vpu_running = 1;
    hctx->enc_external_enabled = 1;
    hctx->enc_external_surface = VA_INVALID_SURFACE;
    hctx->encoder_picture_active = 1;
    hctx->current_render_target = 1;
    hctx->enc_coded_buf = 2;
    hctx->vpu_ctx.codec_id = MEDIA_CODEC_ID_JPEG;
    hctx->vpu_ctx.video_enc_params.width = 2;
    hctx->vpu_ctx.video_enc_params.height = 2;
    hctx->vpu_ctx.video_enc_params.frame_buf_count = 1;
    atomic_init(&hctx->enc_external_input_pending, 0);

    HobotSurface *surface = &drv.surfaces[1];
    surface->allocated = 1;
    surface->id = 1;
    surface->width = 2;
    surface->height = 2;
    surface->stride = 4;
    surface->raw_data = mock_graph_data;
    surface->raw_data_size = 12;
    surface->raw_data_valid = 1;
    surface->has_preallocated = 1;
    surface->preallocated_gbuf.fd[0] = 7;
    for (size_t i = 1; i < MAX_GRAPHIC_BUF_COMP; i++)
        surface->preallocated_gbuf.fd[i] = -1;
    surface->preallocated_gbuf.stride = 4;
    surface->preallocated_gbuf.vstride = 2;
    surface->preallocated_gbuf.size[0] = 12;
    surface->preallocated_gbuf.offset[1] = 8;
    surface->preallocated_gbuf.is_contig = 1;
    surface->preallocated_gbuf.virt_addr[0] = mock_graph_data;
    surface->preallocated_gbuf.virt_addr[1] = mock_graph_data + 8;
    surface->preallocated_gbuf.phys_addr[0] = 0x1000;
    surface->preallocated_gbuf.phys_addr[1] = 0x1008;

    HobotBuffer *coded = &drv.buffers[2];
    coded->allocated = 1;
    coded->type = VAEncCodedBufferType;
    coded->data = coded_data;
    coded->size = sizeof(coded_data);
    coded->capacity = sizeof(coded_data);

    memset(&mock_input_listener, 0, sizeof(mock_input_listener));
    mock_input_listener.on_input_buffer_consumed = hobot_encoder_input_consumed;
    mock_input_listener_userdata = hctx;
    mock_encoder_io = 1;
    mock_encoder_input_size = 12;
    mock_encoder_input_stride = 4;
    mock_encoder_input_vstride = 4;
    mock_dequeue_input_result = 0;
    mock_queue_input_result = 0;
    mock_dequeue_result = -1;
    mock_dequeue_output_calls = 0;
    mock_queue_input_calls = 0;
    mock_queue_output_calls = 0;
    mock_suppress_input_callback = 0;

    VAStatus status = hobot_vaEndPicture(&va_ctx, 1);
    int callback_released_owner =
        status == VA_STATUS_ERROR_OPERATION_FAILED && hctx->encoder_failed &&
        !hctx->encoder_picture_active &&
        atomic_load_explicit(&hctx->enc_external_input_pending,
                             memory_order_acquire) == 0 &&
        hctx->enc_external_surface == VA_INVALID_SURFACE &&
        !hobot_surface_has_active_encoder(&drv, 1) &&
        mock_queue_input_calls == 1 && mock_dequeue_output_calls == 1;

    hctx->encoder_failed = 0;
    hctx->encoder_picture_active = 1;
    hctx->current_render_target = 1;
    hctx->enc_coded_buf = 2;
    hctx->enc_external_surface = VA_INVALID_SURFACE;
    coded->coded_segment.size = 0;
    mock_suppress_input_callback = 1;
    status = hobot_vaEndPicture(&va_ctx, 1);
    uintptr_t pending = atomic_load_explicit(
        &hctx->enc_external_input_pending, memory_order_acquire);
    int timeout_preserved_owner = status == VA_STATUS_ERROR_TIMEDOUT && pending != 0 &&
        hctx->encoder_failed && !hctx->encoder_picture_active &&
        hctx->enc_external_surface == 1 &&
        hobot_surface_has_active_encoder(&drv, 1);

    if (!callback_released_owner || !timeout_preserved_owner)
        fprintf(stderr, "external encoder output-error ownership failed: callback=%d timeout=%d status=%d pending=%lu surface=%u active=%d\n",
                callback_released_owner, timeout_preserved_owner, status,
                (unsigned long)pending, hctx->enc_external_surface,
                hobot_surface_has_active_encoder(&drv, 1));

    mock_suppress_input_callback = 0;
    mock_input_listener_userdata = NULL;
    memset(&mock_input_listener, 0, sizeof(mock_input_listener));
    mock_encoder_io = 0;
    mock_encoder_input_size = sizeof(mock_encoder_y) + sizeof(mock_encoder_uv);
    mock_encoder_input_stride = 8;
    mock_encoder_input_vstride = 8;
    mock_dequeue_input_result = 0;
    mock_queue_input_result = 0;
    mock_dequeue_result = 0;
    pthread_mutex_destroy(&drv.mutex);
    return callback_released_owner && timeout_preserved_owner;
}

static int test_encoder_input_queue_failure_preserves_ownership(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    uint8_t coded_data[16] = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->is_encoder = 1;
    hctx->vpu_initialized = 1;
    hctx->vpu_running = 1;
    hctx->current_render_target = VA_INVALID_SURFACE;
    hctx->vpu_ctx.video_enc_params.width = 2;
    hctx->vpu_ctx.video_enc_params.height = 2;
    drv.surfaces[1].allocated = 1;
    drv.surfaces[1].width = 2;
    drv.surfaces[1].height = 2;
    drv.buffers[2].allocated = 1;
    drv.buffers[2].type = VAEncCodedBufferType;
    drv.buffers[2].data = coded_data;
    drv.buffers[2].size = sizeof(coded_data);
    drv.buffers[2].capacity = sizeof(coded_data);

    mock_encoder_io = 1;
    mock_encoder_input_size = sizeof(mock_encoder_y) + sizeof(mock_encoder_uv);
    mock_encoder_input_stride = 8;
    mock_encoder_input_vstride = 8;
    mock_encoded_size = 4;
    mock_dequeue_input_result = 0;
    mock_dequeue_result = 0;
    mock_dequeue_input_calls = 0;
    mock_dequeue_output_calls = 0;
    mock_queue_input_calls = 0;
    mock_queue_output_calls = 0;
    mock_queue_input_result = -1;
    mock_queue_output_result = 0;
    mock_stop_calls = 0;
    mock_release_calls = 0;
    mock_stop_result = 0;
    mock_release_result = 0;

    VAStatus status = hobot_vaBeginPicture(&va_ctx, 1, 1);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "encoder begin failed in input ownership test: status=%d\n", status);
        goto fail;
    }
    hctx->enc_coded_buf = 2;
    status = hobot_vaEndPicture(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || hctx->frame_count != 0 ||
        !hctx->encoder_failed || hctx->encoder_picture_active || !hctx->enc_in_buf_valid ||
        mock_dequeue_output_calls != 0 ||
        hobot_vaBeginPicture(&va_ctx, 1, 1) != VA_STATUS_ERROR_OPERATION_FAILED) {
        fprintf(stderr, "failed encoder input queue lost state: status=%d frames=%lu retained=%d failed=%d\n",
                status, (unsigned long)hctx->frame_count, hctx->enc_in_buf_valid,
                hctx->encoder_failed);
        goto fail;
    }

    status = hobot_vaDestroyContext(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || !hctx->allocated ||
        !hctx->enc_in_buf_valid || mock_stop_calls != 0 || mock_release_calls != 0) {
        fprintf(stderr, "failed encoder input retry discarded ownership: status=%d retained=%d stop=%d release=%d\n",
                status, hctx->enc_in_buf_valid, mock_stop_calls, mock_release_calls);
        goto fail;
    }
    mock_queue_input_result = 0;
    status = hobot_vaDestroyContext(&va_ctx, 1);
    if (status != VA_STATUS_SUCCESS || hctx->allocated || hctx->enc_in_buf_valid ||
        mock_stop_calls != 1 || mock_release_calls != 1) {
        fprintf(stderr, "encoder input ownership teardown retry failed: status=%d allocated=%d retained=%d stop=%d release=%d\n",
                status, hctx->allocated, hctx->enc_in_buf_valid,
                mock_stop_calls, mock_release_calls);
        goto fail;
    }

    mock_encoder_io = 0;
    mock_queue_input_result = 0;
    pthread_mutex_destroy(&drv.mutex);
    return 1;

fail:
    mock_encoder_io = 0;
    mock_queue_input_result = 0;
    pthread_mutex_destroy(&drv.mutex);
    return 0;
}

static int test_encoder_terminate_retries_retained_buffers(void)
{
    HobotDriverData *drv = calloc(1, sizeof(*drv));
    struct VADriverContext va_ctx = {0};
    if (!drv)
        return 0;
    if (pthread_mutex_init(&drv->mutex, NULL) != 0) {
        free(drv);
        return 0;
    }
    va_ctx.pDriverData = drv;

    HobotContext *hctx = &drv->contexts[1];
    hctx->allocated = 1;
    hctx->is_encoder = 1;
    hctx->vpu_initialized = 1;
    hctx->vpu_running = 1;
    hctx->enc_in_buf_valid = 1;
    hctx->enc_in_buf.vframe_buf.vir_ptr[0] = mock_encoder_y;
    hctx->enc_in_buf.vframe_buf.vir_ptr[1] = mock_encoder_uv;

    mock_queue_input_calls = 0;
    mock_queue_input_result = -1;
    mock_stop_calls = 0;
    mock_release_calls = 0;
    mock_stop_result = 0;
    mock_release_result = 0;

    VAStatus status = hobot_vaTerminate(&va_ctx);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || va_ctx.pDriverData != drv ||
        !hctx->allocated || !hctx->enc_in_buf_valid || mock_stop_calls != 0 ||
        mock_release_calls != 0) {
        fprintf(stderr, "vaTerminate discarded retained encoder input: status=%d driver=%p retained=%d stop=%d release=%d\n",
                status, va_ctx.pDriverData, hctx->enc_in_buf_valid,
                mock_stop_calls, mock_release_calls);
        goto fail;
    }

    mock_queue_input_result = 0;
    status = hobot_vaTerminate(&va_ctx);
    if (status != VA_STATUS_SUCCESS || va_ctx.pDriverData != NULL ||
        mock_stop_calls != 1 || mock_release_calls != 1) {
        fprintf(stderr, "vaTerminate retained-input retry failed: status=%d driver=%p stop=%d release=%d\n",
                status, va_ctx.pDriverData, mock_stop_calls, mock_release_calls);
        goto fail;
    }

    mock_queue_input_result = 0;
    return 1;

fail:
    mock_queue_input_result = 0;
    if (va_ctx.pDriverData)
        hobot_vaTerminate(&va_ctx);
    return 0;
}

static int test_decoder_terminate_retries_retained_output(void)
{
    HobotDriverData *drv = calloc(1, sizeof(*drv));
    struct VADriverContext va_ctx = {0};
    if (!drv)
        return 0;
    if (pthread_mutex_init(&drv->mutex, NULL) != 0) {
        free(drv);
        return 0;
    }
    va_ctx.pDriverData = drv;

    HobotContext *hctx = &drv->contexts[1];
    hctx->allocated = 1;
    hctx->vpu_initialized = 1;
    hctx->vpu_running = 1;
    HobotSurface *surf = &drv->surfaces[1];
    surf->allocated = 1;
    surf->has_decoded_frame = 1;
    surf->output_context_id = 1;
    surf->vpu_out_buf.vframe_buf.phy_ptr[0] = 1;
    surf->vpu_out_buf.vframe_buf.size = sizeof(mock_frame);
    surf->vpu_out_buf.vframe_buf.fd[0] = 7;

    mock_queue_output_calls = 0;
    mock_queue_output_result = -1;
    mock_stop_calls = 0;
    mock_release_calls = 0;
    mock_stop_result = 0;
    mock_release_result = 0;
    int close_calls_before = mock_mem_module_close_calls;

    VAStatus status = hobot_vaTerminate(&va_ctx);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || va_ctx.pDriverData != drv ||
        !hctx->allocated || !hctx->vpu_initialized || !hctx->vpu_running ||
        !surf->allocated || !surf->has_decoded_frame ||
        surf->output_context_id != 1 ||
        surf->vpu_out_buf.vframe_buf.phy_ptr[0] != 1 ||
        mock_queue_output_calls != 2 || mock_stop_calls != 0 ||
        mock_release_calls != 0 ||
        mock_mem_module_close_calls != close_calls_before) {
        fprintf(stderr, "vaTerminate discarded decoder output ownership after recycle failure: status=%d driver=%p surface=%d owner=%u queue=%d stop=%d release=%d close=%d/%d\n",
                status, va_ctx.pDriverData, surf->has_decoded_frame,
                surf->output_context_id, mock_queue_output_calls,
                mock_stop_calls, mock_release_calls,
                mock_mem_module_close_calls, close_calls_before);
        goto fail;
    }

    mock_queue_output_result = 0;
    status = hobot_vaTerminate(&va_ctx);
    int passed = status == VA_STATUS_SUCCESS && va_ctx.pDriverData == NULL &&
                 mock_queue_output_calls == 3 && mock_stop_calls == 1 &&
                 mock_release_calls == 1 &&
                 mock_mem_module_close_calls == close_calls_before + 1;
    if (!passed)
        fprintf(stderr, "vaTerminate failed to recover retained decoder output: status=%d driver=%p queue=%d stop=%d release=%d close=%d/%d\n",
                status, va_ctx.pDriverData, mock_queue_output_calls,
                mock_stop_calls, mock_release_calls,
                mock_mem_module_close_calls, close_calls_before);

    mock_queue_output_result = 0;
    return passed;

fail:
    mock_queue_output_result = 0;
    mock_stop_result = 0;
    mock_release_result = 0;
    if (va_ctx.pDriverData)
        hobot_vaTerminate(&va_ctx);
    return 0;
}

static int test_buffer_mapping_lifecycle(void)
{
    HobotDriverData *drv = calloc(1, sizeof(*drv));
    struct VADriverContext va_ctx = {0};
    VAImageFormat format = {0};
    VAImage image = {0};
    if (!drv)
        return 0;
    if (pthread_mutex_init(&drv->mutex, NULL) != 0) {
        free(drv);
        return 0;
    }
    va_ctx.pDriverData = drv;

    VABufferID buffer = VA_INVALID_ID;
    VAStatus status = hobot_vaCreateBuffer(&va_ctx, 0, VAImageBufferType,
                                            16, 1, NULL, &buffer);
    if (status != VA_STATUS_SUCCESS)
        goto fail;
    void *first_map = NULL;
    void *second_map = NULL;
    if (hobot_vaMapBuffer(&va_ctx, buffer, &first_map) != VA_STATUS_SUCCESS ||
        hobot_vaMapBuffer(&va_ctx, buffer, &second_map) != VA_STATUS_SUCCESS ||
        first_map != second_map || drv->buffers[buffer].map_count != 2 ||
        hobot_vaBufferSetNumElements(&va_ctx, buffer, 1) != VA_STATUS_SUCCESS ||
        drv->buffers[buffer].map_count != 2 ||
        hobot_vaDestroyBuffer(&va_ctx, buffer) != VA_STATUS_ERROR_OPERATION_FAILED ||
        hobot_vaUnmapBuffer(&va_ctx, buffer) != VA_STATUS_SUCCESS ||
        hobot_vaDestroyBuffer(&va_ctx, buffer) != VA_STATUS_ERROR_OPERATION_FAILED ||
        hobot_vaUnmapBuffer(&va_ctx, buffer) != VA_STATUS_SUCCESS ||
        hobot_vaUnmapBuffer(&va_ctx, buffer) != VA_STATUS_ERROR_OPERATION_FAILED ||
        hobot_vaDestroyBuffer(&va_ctx, buffer) != VA_STATUS_SUCCESS) {
        fprintf(stderr, "mapped buffer lifecycle did not preserve storage or balance mappings\n");
        goto fail;
    }

    format.fourcc = VA_FOURCC_NV12;
    format.byte_order = VA_LSB_FIRST;
    format.bits_per_pixel = 12;
    if (hobot_vaCreateImage(&va_ctx, &format, 64, 64, &image) != VA_STATUS_SUCCESS)
        goto fail;
    if (hobot_vaMapBuffer(&va_ctx, image.buf, &first_map) != VA_STATUS_SUCCESS ||
        hobot_vaDestroyImage(&va_ctx, image.image_id) != VA_STATUS_ERROR_OPERATION_FAILED ||
        hobot_vaUnmapBuffer(&va_ctx, image.buf) != VA_STATUS_SUCCESS ||
        hobot_vaDestroyBuffer(&va_ctx, image.buf) != VA_STATUS_ERROR_OPERATION_FAILED ||
        hobot_vaDestroyImage(&va_ctx, image.image_id) != VA_STATUS_SUCCESS) {
        fprintf(stderr, "image-owned buffer could be invalidated independently of its image\n");
        goto fail;
    }

    if (hobot_vaCreateBuffer(&va_ctx, 0, VAImageBufferType, 8, 1, NULL,
                             &buffer) != VA_STATUS_SUCCESS ||
        hobot_vaMapBuffer(&va_ctx, buffer, &first_map) != VA_STATUS_SUCCESS) {
        fprintf(stderr, "could not prepare mapped-buffer terminate test\n");
        goto fail;
    }
    if (hobot_vaTerminate(&va_ctx) != VA_STATUS_ERROR_OPERATION_FAILED ||
        va_ctx.pDriverData != drv || !drv->buffers[buffer].allocated ||
        drv->buffers[buffer].map_count != 1 ||
        hobot_vaUnmapBuffer(&va_ctx, buffer) != VA_STATUS_SUCCESS ||
        hobot_vaTerminate(&va_ctx) != VA_STATUS_SUCCESS || va_ctx.pDriverData != NULL) {
        fprintf(stderr, "terminate freed driver storage while a buffer remained mapped\n");
        goto fail;
    }
    return 1;

fail:
    if (va_ctx.pDriverData) {
        HobotDriverData *live_drv = va_ctx.pDriverData;
        for (int i = 1; i < MAX_BUFFERS; i++)
            live_drv->buffers[i].map_count = 0;
        hobot_vaTerminate(&va_ctx);
    }
    return 0;
}

static int test_active_encoder_retains_coded_buffer(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    VABufferID buffer = VA_INVALID_ID;
    if (hobot_vaCreateBuffer(&va_ctx, 0, VAEncCodedBufferType, 128, 1,
                             NULL, &buffer) != VA_STATUS_SUCCESS ||
        buffer <= 0 || buffer >= MAX_BUFFERS) {
        fprintf(stderr, "could not create coded buffer for active-picture lifetime test\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    HobotContext *encoder = &drv.contexts[1];
    encoder->allocated = 1;
    encoder->is_encoder = 1;
    encoder->encoder_picture_active = 1;
    encoder->enc_coded_buf = buffer;

    void *data = drv.buffers[buffer].data;
    if (!data || drv.buffers[buffer].type != VAEncCodedBufferType ||
        hobot_vaDestroyBuffer(&va_ctx, buffer) != VA_STATUS_ERROR_OPERATION_FAILED ||
        !drv.buffers[buffer].allocated || drv.buffers[buffer].data != data) {
        fprintf(stderr, "active encoder did not retain its coded buffer\n");
        encoder->encoder_picture_active = 0;
        encoder->enc_coded_buf = 0;
        if (drv.buffers[buffer].allocated)
            hobot_vaDestroyBuffer(&va_ctx, buffer);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    encoder->encoder_picture_active = 0;
    encoder->enc_coded_buf = 0;
    if (hobot_vaDestroyBuffer(&va_ctx, buffer) != VA_STATUS_SUCCESS ||
        drv.buffers[buffer].allocated || drv.buffers[buffer].data != NULL) {
        fprintf(stderr, "coded buffer could not be destroyed after encoder picture completion\n");
        if (drv.buffers[buffer].allocated)
            hobot_vaDestroyBuffer(&va_ctx, buffer);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static int test_render_picture_prevalidates_all_buffers(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    uint8_t parameter_data[sizeof(VAPictureParameterBufferH264)] = {0};
    VABufferID decoder_buffers[] = {1, MAX_BUFFERS};
    VABufferID encoder_buffers[] = {1, 2};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    mock_dequeue_input_calls = 0;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->decode_picture_active = 1;
    hctx->current_render_target = 5;
    hctx->profile = VAProfileH264High;
    hctx->width = 640;
    hctx->height = 368;
    hctx->submitted_surfaces[hctx->sub_tail++] = 5;
    hctx->cached_sps_len = 1;
    hctx->cached_pps_len = 1;
    hctx->cached_sps[0] = 0xa5;
    hctx->cached_pps[0] = 0x5a;
    hctx->headers_sent = 1;
    drv.surfaces[5].allocated = 1;
    drv.surfaces[5].context_id = 1;
    drv.surfaces[5].decode_pending = 1;
    drv.buffers[1].allocated = 1;
    drv.buffers[1].type = VAPictureParameterBufferType;
    drv.buffers[1].data = parameter_data;
    drv.buffers[1].size = sizeof(VAPictureParameterBufferH264);
    drv.buffers[1].element_size = sizeof(VAPictureParameterBufferH264);
    drv.buffers[1].num_elements = 1;
    VAPictureParameterBufferH264 *decode_picture =
        (VAPictureParameterBufferH264 *)parameter_data;
    for (size_t i = 0; i < sizeof(decode_picture->ReferenceFrames) /
                            sizeof(decode_picture->ReferenceFrames[0]); i++) {
        decode_picture->ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        decode_picture->ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    decode_picture->CurrPic.picture_id = 5;
    ((VAPictureParameterBufferH264 *)parameter_data)->picture_width_in_mbs_minus1 = 39;
    ((VAPictureParameterBufferH264 *)parameter_data)->picture_height_in_mbs_minus1 = 22;
    ((VAPictureParameterBufferH264 *)parameter_data)->seq_fields.bits.frame_mbs_only_flag = 1;
    ((VAPictureParameterBufferH264 *)parameter_data)->seq_fields.bits.chroma_format_idc = 1;

    VAStatus status = hobot_vaRenderPicture(&va_ctx, 1, decoder_buffers, 2);
    if (status != VA_STATUS_ERROR_INVALID_BUFFER || hctx->cached_sps_len != 1 ||
        hctx->cached_pps_len != 1 || hctx->cached_sps[0] != 0xa5 ||
        hctx->cached_pps[0] != 0x5a || !hctx->headers_sent ||
        !hctx->decode_picture_active || hctx->current_render_target != 5 ||
        mock_dequeue_input_calls != 0) {
        fprintf(stderr, "decoder render buffers were not prevalidated: status=%d sps=%d pps=%d active=%d target=%u deq=%d\n",
                status, hctx->cached_sps_len, hctx->cached_pps_len,
                hctx->decode_picture_active, hctx->current_render_target,
                mock_dequeue_input_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    decoder_buffers[0] = 1;
    decoder_buffers[1] = 0;
    for (int profile_pass = 0; profile_pass < 2; profile_pass++) {
        hctx->profile = profile_pass == 0 ? VAProfileH264High : VAProfileHEVCMain;
        drv.buffers[1].size = profile_pass == 0 ?
            sizeof(VAPictureParameterBufferH264) - 1 :
            sizeof(VAPictureParameterBufferHEVC) - 1;
        status = hobot_vaRenderPicture(&va_ctx, 1, decoder_buffers, 1);
        if (status != VA_STATUS_ERROR_INVALID_PARAMETER ||
            hctx->cached_sps_len != 1 || hctx->cached_pps_len != 1 ||
            hctx->cached_sps[0] != 0xa5 || hctx->cached_pps[0] != 0x5a ||
            !hctx->headers_sent || !hctx->decode_picture_active ||
            mock_dequeue_input_calls != 0) {
            fprintf(stderr, "truncated decoder picture parameters changed state: profile=%d status=%d sps=%d pps=%d active=%d deq=%d\n",
                    hctx->profile, status, hctx->cached_sps_len,
                    hctx->cached_pps_len, hctx->decode_picture_active,
                    mock_dequeue_input_calls);
            pthread_mutex_destroy(&drv.mutex);
            return 0;
        }
    }

    memset(hctx, 0, sizeof(*hctx));
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->is_encoder = 1;
    hctx->encoder_picture_active = 1;
    hctx->profile = VAProfileH264Main;
    hctx->enc_coded_buf = 77;
    hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.intra_period = 7;
    VAEncSequenceParameterBufferH264 sequence = {0};
    sequence.intra_period = 30;
    sequence.bits_per_second = 4000000;
    drv.buffers[1].type = VAEncSequenceParameterBufferType;
    drv.buffers[1].data = &sequence;
    drv.buffers[1].size = sizeof(sequence);
    drv.buffers[1].element_size = sizeof(sequence);
    drv.buffers[1].num_elements = 1;
    drv.buffers[4].allocated = 1;
    drv.buffers[4].type = VAImageBufferType;
    uint8_t unrelated_data = 0;
    drv.buffers[4].data = &unrelated_data;
    drv.buffers[4].size = sizeof(unrelated_data);

    VAEncPictureParameterBufferH264 picture = {0};
    for (size_t i = 0; i < sizeof(picture.ReferenceFrames) /
                            sizeof(picture.ReferenceFrames[0]); i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    picture.CurrPic.picture_id = 5;
    picture.coded_buf = 4;
    drv.buffers[2].allocated = 1;
    drv.buffers[2].type = VAEncPictureParameterBufferType;
    drv.buffers[2].data = &picture;
    drv.buffers[2].size = sizeof(picture);
    drv.buffers[2].element_size = sizeof(picture);
    drv.buffers[2].num_elements = 1;

    status = hobot_vaRenderPicture(&va_ctx, 1, encoder_buffers, 2);
    if (status != VA_STATUS_ERROR_INVALID_BUFFER ||
        hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.intra_period != 7 ||
        hctx->enc_coded_buf != 77) {
        fprintf(stderr, "encoder render buffers were not prevalidated: status=%d intra=%u coded=%u\n",
                status,
                hctx->vpu_ctx.video_enc_params.rc_params.h264_cbr_params.intra_period,
                hctx->enc_coded_buf);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static void test_h264_set_slice_groups(VAPictureParameterBufferH264 *picture,
                                       uint8_t count)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    picture->num_slice_groups_minus1 = count;
#pragma GCC diagnostic pop
}

static void test_h264_init_context_picture(
    VAPictureParameterBufferH264 *picture)
{
    for (size_t i = 0; i < sizeof(picture->ReferenceFrames) /
                            sizeof(picture->ReferenceFrames[0]); i++) {
        picture->ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture->ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    picture->CurrPic.picture_id = 1;
    picture->picture_width_in_mbs_minus1 = 39;
    picture->picture_height_in_mbs_minus1 = 22;
    picture->seq_fields.bits.frame_mbs_only_flag = 1;
    picture->seq_fields.bits.chroma_format_idc = 1;
}

static int test_h264_parameter_synthesis_rejects_unrepresentable_sps(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    VAPictureParameterBufferH264 picture = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->decode_picture_active = 1;
    hctx->profile = VAProfileH264High;
    hctx->width = 640;
    hctx->height = 360;
    hctx->current_render_target = 1;
    drv.surfaces[1].allocated = 1;
    drv.buffers[1].allocated = 1;
    drv.buffers[1].type = VAPictureParameterBufferType;
    drv.buffers[1].data = &picture;
    drv.buffers[1].size = sizeof(picture);
    drv.buffers[1].element_size = sizeof(picture);
    drv.buffers[1].num_elements = 1;
    VABufferID picture_id = 1;
    mock_dequeue_input_calls = 0;

    test_h264_init_context_picture(&picture);
    VAStatus status = hobot_vaRenderPicture(&va_ctx, 1, &picture_id, 1);
    if (status != VA_STATUS_SUCCESS || hctx->cached_sps_len <= 0 ||
        hctx->cached_pps_len <= 0 || hctx->cached_sps[1] != 100 ||
        mock_dequeue_input_calls != 0) {
        fprintf(stderr, "valid H264 POC-0 picture parameters were rejected: status=%d sps=%d pps=%d\n",
                status, hctx->cached_sps_len, hctx->cached_pps_len);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    for (int invalid_case = 0; invalid_case < 21; invalid_case++) {
        memset(&picture, 0, sizeof(picture));
        test_h264_init_context_picture(&picture);
        switch (invalid_case) {
        case 0:
            picture.seq_fields.bits.pic_order_cnt_type = 1;
            break;
        case 1:
            picture.seq_fields.bits.pic_order_cnt_type = 3;
            break;
        case 2:
            picture.seq_fields.bits.chroma_format_idc = 0;
            break;
        case 3:
            picture.bit_depth_luma_minus8 = 2;
            break;
        case 4:
            picture.seq_fields.bits.log2_max_frame_num_minus4 = 13;
            break;
        case 5:
            picture.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 13;
            break;
        case 6:
            picture.seq_fields.bits.chroma_format_idc = 2;
            break;
        case 7:
            test_h264_set_slice_groups(&picture, 1);
            break;
        case 8:
            picture.pic_fields.bits.redundant_pic_cnt_present_flag = 1;
            break;
        case 9:
            picture.picture_width_in_mbs_minus1 = 40;
            break;
        case 10:
            picture.picture_height_in_mbs_minus1 = 23;
            break;
        case 11:
            picture.num_ref_frames = 17;
            break;
        case 12:
            picture.pic_init_qp_minus26 = -27;
            break;
        case 13:
            picture.pic_init_qp_minus26 = 26;
            break;
        case 14:
            picture.pic_init_qs_minus26 = -27;
            break;
        case 15:
            picture.pic_init_qs_minus26 = 26;
            break;
        case 16:
            picture.chroma_qp_index_offset = -13;
            break;
        case 17:
            picture.chroma_qp_index_offset = 13;
            break;
        case 18:
            picture.second_chroma_qp_index_offset = -13;
            break;
        case 19:
            picture.second_chroma_qp_index_offset = 13;
            break;
        case 20:
            picture.pic_fields.bits.weighted_bipred_idc = 3;
            break;
        }
        hctx->cached_sps_len = 1;
        hctx->cached_pps_len = 1;
        hctx->cached_sps[0] = 0xa5;
        hctx->cached_pps[0] = 0x5a;
        hctx->headers_sent = 1;
        status = hobot_vaRenderPicture(&va_ctx, 1, &picture_id, 1);
        if (status != VA_STATUS_ERROR_INVALID_PARAMETER ||
            hctx->cached_sps_len != 1 || hctx->cached_pps_len != 1 ||
            hctx->cached_sps[0] != 0xa5 || hctx->cached_pps[0] != 0x5a ||
            !hctx->headers_sent || mock_dequeue_input_calls != 0) {
            fprintf(stderr, "unsupported H264 SPS case %d mutated state: status=%d sps=%d pps=%d\n",
                    invalid_case, status, hctx->cached_sps_len,
                    hctx->cached_pps_len);
            pthread_mutex_destroy(&drv.mutex);
            return 0;
        }
    }

    memset(&picture, 0, sizeof(picture));
    test_h264_init_context_picture(&picture);
    picture.seq_fields.bits.pic_order_cnt_type = 2;
    status = hobot_vaRenderPicture(&va_ctx, 1, &picture_id, 1);
    int passed = status == VA_STATUS_SUCCESS && hctx->cached_sps_len > 0 &&
                 hctx->cached_pps_len > 0 && mock_dequeue_input_calls == 0;
    if (!passed)
        fprintf(stderr, "valid H264 POC-2 picture parameters were rejected: status=%d\n", status);

    memset(&picture, 0, sizeof(picture));
    test_h264_init_context_picture(&picture);
    hctx->profile = VAProfileH264Main;
    status = hobot_vaRenderPicture(&va_ctx, 1, &picture_id, 1);
    passed = passed && status == VA_STATUS_SUCCESS && hctx->cached_sps_len > 0 &&
             hctx->cached_pps_len > 0 && hctx->cached_sps[1] == 77 &&
             hctx->cached_pps_len > 0 && mock_dequeue_input_calls == 0;
    if (status != VA_STATUS_SUCCESS)
        fprintf(stderr, "valid H264 Main 4:2:0 defaults were rejected: status=%d\n", status);

    memset(&picture, 0, sizeof(picture));
    test_h264_init_context_picture(&picture);
    hctx->profile = VAProfileH264ConstrainedBaseline;
    status = hobot_vaRenderPicture(&va_ctx, 1, &picture_id, 1);
    passed = passed && status == VA_STATUS_SUCCESS && hctx->cached_sps_len > 3 &&
             hctx->cached_sps[1] == 66 && hctx->cached_sps[2] == 0x40 &&
             hctx->cached_pps_len > 0 && mock_dequeue_input_calls == 0;
    if (status != VA_STATUS_SUCCESS)
        fprintf(stderr, "valid H264 constrained-baseline defaults were rejected: status=%d\n", status);

    for (int invalid_case = 0; invalid_case < 5; invalid_case++) {
        memset(&picture, 0, sizeof(picture));
        test_h264_init_context_picture(&picture);
        switch (invalid_case) {
        case 0:
            picture.seq_fields.bits.frame_mbs_only_flag = 0;
            break;
        case 1:
            picture.pic_fields.bits.entropy_coding_mode_flag = 1;
            break;
        case 2:
            picture.pic_fields.bits.weighted_pred_flag = 1;
            break;
        case 3:
            picture.pic_fields.bits.weighted_bipred_idc = 1;
            break;
        case 4:
            picture.pic_fields.bits.transform_8x8_mode_flag = 1;
            break;
        }
        status = hobot_vaRenderPicture(&va_ctx, 1, &picture_id, 1);
        if (status != VA_STATUS_ERROR_INVALID_PARAMETER || mock_dequeue_input_calls != 0) {
            fprintf(stderr, "unsupported constrained-baseline PPS case %d accepted: status=%d\n",
                    invalid_case, status);
            pthread_mutex_destroy(&drv.mutex);
            return 0;
        }
    }

    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_h264_profile_sps_headers(void)
{
    static const struct {
        VAProfile profile;
        uint8_t profile_idc;
        uint8_t constraint_flags;
    } cases[] = {
        {VAProfileH264ConstrainedBaseline, 66, 0x40},
        {VAProfileH264Main, 77, 0x00},
        {VAProfileH264High, 100, 0x00},
    };
    VAPictureParameterBufferH264 picture = {0};
    picture.seq_fields.bits.chroma_format_idc = 1;
    picture.seq_fields.bits.frame_mbs_only_flag = 1;
    uint8_t sps[256] = {0};

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memset(sps, 0, sizeof(sps));
        int size = generate_h264_sps(&picture, cases[i].profile,
                                     sps, sizeof(sps));
        if (size < 4 || sps[0] != 0x67 || sps[1] != cases[i].profile_idc ||
            sps[2] != cases[i].constraint_flags || sps[3] != 51) {
            fprintf(stderr, "H264 SPS profile header mismatch for VA profile %d: size=%d bytes=%02x/%02x/%02x/%02x\n",
                    cases[i].profile, size, sps[0], sps[1], sps[2], sps[3]);
            return 0;
        }
    }
    if (generate_h264_sps(&picture, VAProfileNone, sps, sizeof(sps)) != 0) {
        fprintf(stderr, "unsupported H264 profile generated an SPS\n");
        return 0;
    }
    return 1;
}

static int build_h264_ref_test_slice(
    const VAPictureParameterBufferH264 *picture,
    VASliceParameterBufferH264 *slice,
    uint8_t nal_header,
    unsigned int slice_type,
    int override_refs,
    unsigned int active_l0,
    unsigned int active_l1,
    uint8_t *stream,
    size_t capacity,
    size_t *stream_size);

static int test_h264_interlaced_picture_parameters(void)
{
    VAPictureParameterBufferH264 picture = {0};
    picture.picture_width_in_mbs_minus1 = 39;
    picture.picture_height_in_mbs_minus1 = 29;
    picture.seq_fields.bits.chroma_format_idc = 1;
    picture.seq_fields.bits.pic_order_cnt_type = 2;
    picture.seq_fields.bits.mb_adaptive_frame_field_flag = 1;
    VAProfile profiles[] = {
        VAProfileH264ConstrainedBaseline,
        VAProfileH264Main,
        VAProfileH264High,
    };
    uint8_t sps[256] = {0};
    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
        if (hobot_h264_picture_parameters_supported(
                &picture, profiles[i], 640, 480) ||
            generate_h264_sps(&picture, profiles[i], sps, sizeof(sps)) != 0) {
            fprintf(stderr, "H264 interlaced/MBAFF mode accepted for profile %d\n",
                    profiles[i]);
            return 0;
        }
    }

    picture.pic_fields.bits.field_pic_flag = 1;
    picture.CurrPic.flags = VA_PICTURE_H264_TOP_FIELD;
    VASliceParameterBufferH264 slice = {0};
    uint8_t stream[64] = {0};
    size_t stream_size = 0;
    if (!build_h264_ref_test_slice(
            &picture, &slice, 0x65, 2, 0, 0, 0,
            stream, sizeof(stream), &stream_size))
        return 0;
    HobotBuffer data = {
        .allocated = 1,
        .type = VASliceDataBufferType,
        .size = (unsigned int)stream_size,
        .data = stream,
    };
    unsigned int slice_type;
    uint32_t l0_override, l1_override;
    if (hobot_h264_slice_ref_override_flags(
            &picture, &slice, &data, &slice_type,
            &l0_override, &l1_override)) {
        fprintf(stderr, "H264 field-picture slice reached the supported parser\n");
        return 0;
    }

    picture.seq_fields.bits.frame_mbs_only_flag = 1;
    picture.pic_fields.bits.field_pic_flag = 0;
    picture.CurrPic.flags = 0;
    if (!hobot_h264_picture_parameters_supported(
            &picture, VAProfileH264Main, 640, 480)) {
        fprintf(stderr, "progressive 640x480 H264 Main parameters rejected\n");
        return 0;
    }

    picture.seq_fields.bits.chroma_format_idc = 0;
    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++) {
        if (hobot_h264_picture_parameters_supported(
                &picture, profiles[i], 640, 480) ||
            generate_h264_sps(&picture, profiles[i], sps, sizeof(sps)) != 0) {
            fprintf(stderr, "H264 monochrome mode accepted for profile %d\n",
                    profiles[i]);
            return 0;
        }
    }
    return 1;
}

static int test_h264_constrained_baseline_output_headers(void)
{
    VAPictureParameterBufferH264 picture = {0};
    picture.seq_fields.bits.chroma_format_idc = 1;
    uint8_t pps[256] = {0};
    int pps_size = generate_h264_pps(&picture, 0, 0, pps, sizeof(pps));
    if (pps_size < 2)
        return 0;

    uint8_t stream[512] = {0};
    size_t stream_size = 0;
    static const uint8_t sps_nal[] = {0x67, 66, 0x00, 41, 0x80};
    static const uint8_t start_code[] = {0, 0, 0, 1};
    memcpy(stream + stream_size, start_code, sizeof(start_code));
    stream_size += sizeof(start_code);
    memcpy(stream + stream_size, sps_nal, sizeof(sps_nal));
    stream_size += sizeof(sps_nal);
    memcpy(stream + stream_size, start_code, sizeof(start_code));
    stream_size += sizeof(start_code);
    memcpy(stream + stream_size, pps, (size_t)pps_size);
    stream_size += (size_t)pps_size;
    size_t original_size = stream_size;

    int found_sps = 0, found_pps = 0;
    int patch_status = hobot_h264_patch_constrained_baseline_headers(
        stream, &stream_size, sizeof(stream), 1, &found_sps, &found_pps);
    if (!patch_status || !found_sps || !found_pps ||
        stream[6] != 0x40 || stream_size > original_size) {
        fprintf(stderr, "H.264 Constrained Baseline output headers were not patched: status=%d found=%d/%d size=%zu/%zu flags=0x%02x\n",
                patch_status, found_sps, found_pps, stream_size,
                original_size, stream[6]);
        return 0;
    }

    size_t prefix_size = 0;
    size_t first_start = hobot_hevc_find_start_code(stream, stream_size, 0,
                                                     &prefix_size);
    if (first_start == SIZE_MAX)
        return 0;
    size_t first_nal = first_start + prefix_size;
    size_t pps_prefix_size = 0;
    size_t pps_prefix = hobot_hevc_find_start_code(
        stream, stream_size, first_nal, &pps_prefix_size);
    if (pps_prefix == SIZE_MAX || pps_prefix + pps_prefix_size + 2u >= stream_size)
        return 0;
    size_t pps_start = pps_prefix + pps_prefix_size;
    size_t rewritten_size = 0;
    uint8_t *rewritten_pps = NULL;
    int changed = 0;
    if (!hobot_h264_strip_constrained_baseline_pps_extension(
            stream + pps_start, stream_size - pps_start,
            &rewritten_pps, &rewritten_size, &changed) || changed ||
        rewritten_pps || rewritten_size != stream_size - pps_start) {
        free(rewritten_pps);
        fprintf(stderr, "stripped H.264 PPS still contains the profile extension\n");
        return 0;
    }

    uint8_t bad_pps[256] = {0};
    picture.pic_fields.bits.transform_8x8_mode_flag = 1;
    int bad_pps_size = generate_h264_pps(&picture, 0, 0, bad_pps, sizeof(bad_pps));
    if (bad_pps_size < 2 ||
        hobot_h264_strip_constrained_baseline_pps_extension(
            bad_pps, (size_t)bad_pps_size, &rewritten_pps,
            &rewritten_size, &changed)) {
        free(rewritten_pps);
        fprintf(stderr, "Constrained Baseline PPS accepted 8x8 transform\n");
        return 0;
    }
    picture.pic_fields.bits.transform_8x8_mode_flag = 0;
    picture.second_chroma_qp_index_offset = 1;
    bad_pps_size = generate_h264_pps(&picture, 0, 0, bad_pps, sizeof(bad_pps));
    if (bad_pps_size < 2 ||
        hobot_h264_strip_constrained_baseline_pps_extension(
            bad_pps, (size_t)bad_pps_size, &rewritten_pps,
            &rewritten_size, &changed)) {
        free(rewritten_pps);
        fprintf(stderr, "Constrained Baseline PPS accepted changed chroma QP semantics\n");
        return 0;
    }
    return 1;
}

static int build_h264_ref_test_slice(
    const VAPictureParameterBufferH264 *picture,
    VASliceParameterBufferH264 *slice,
    uint8_t nal_header,
    unsigned int slice_type,
    int override_refs,
    unsigned int active_l0,
    unsigned int active_l1,
    uint8_t *stream,
    size_t capacity,
    size_t *stream_size)
{
    if (!picture || !slice || !stream || !stream_size ||
        *stream_size >= capacity || slice_type > 9u ||
        active_l0 > 31u || active_l1 > 31u)
        return 0;

    uint8_t rbsp[64] = {0};
    BitWriter bw = {rbsp, 0};
    bw_put_ue(&bw, 0); /* first_mb_in_slice */
    bw_put_ue(&bw, slice_type);
    bw_put_ue(&bw, 0); /* pic_parameter_set_id */
    bw_put_bits(&bw, 0, picture->seq_fields.bits.log2_max_frame_num_minus4 + 4u);
    if (!picture->seq_fields.bits.frame_mbs_only_flag) {
        bw_put_bit(&bw, picture->pic_fields.bits.field_pic_flag);
        if (picture->pic_fields.bits.field_pic_flag) {
            bw_put_bit(&bw, !!(picture->CurrPic.flags &
                               VA_PICTURE_H264_BOTTOM_FIELD));
        }
    }
    if ((nal_header & 0x1fu) == 5u)
        bw_put_ue(&bw, 0); /* idr_pic_id */
    if (picture->seq_fields.bits.pic_order_cnt_type == 0) {
        bw_put_bits(&bw, 0,
                    picture->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4u);
        if (picture->pic_fields.bits.pic_order_present_flag)
            bw_put_se(&bw, 0);
    }

    unsigned int normalized_type = slice_type % 5u;
    uint8_t direct_spatial_mv_pred_flag = 0;
    if (normalized_type == 1u) {
        bw_put_bit(&bw, 1); /* direct_spatial_mv_pred_flag */
        direct_spatial_mv_pred_flag = 1;
    }
    if (normalized_type == 0u || normalized_type == 1u) {
        bw_put_bit(&bw, override_refs);
        if (override_refs) {
            bw_put_ue(&bw, active_l0);
            if (normalized_type == 1u)
                bw_put_ue(&bw, active_l1);
        }
    }

    size_t nal_size = 1u + ((size_t)bw.bit_pos + 7u) / 8u;
    if (nal_size > capacity - *stream_size || nal_size > UINT32_MAX ||
        (size_t)8u + (size_t)bw.bit_pos > UINT16_MAX)
        return 0;
    memset(slice, 0, sizeof(*slice));
    slice->slice_data_offset = (uint32_t)*stream_size;
    slice->slice_data_size = (uint32_t)nal_size;
    slice->slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    slice->slice_data_bit_offset = (uint16_t)(8u + (size_t)bw.bit_pos);
    slice->slice_type = (uint8_t)slice_type;
    slice->direct_spatial_mv_pred_flag = direct_spatial_mv_pred_flag;
    slice->num_ref_idx_l0_active_minus1 = (uint8_t)active_l0;
    slice->num_ref_idx_l1_active_minus1 = (uint8_t)active_l1;
    stream[*stream_size] = nal_header;
    memcpy(stream + *stream_size + 1u, rbsp, nal_size - 1u);
    *stream_size += nal_size;
    return 1;
}

static int test_h264_pps_reference_defaults_from_slice_parameters(void)
{
    HobotDriverData drv = {0};
    VAPictureParameterBufferH264 picture = {0};
    picture.seq_fields.bits.frame_mbs_only_flag = 1;
    picture.seq_fields.bits.pic_order_cnt_type = 2;
    VASliceParameterBufferH264 slices[4] = {0};
    uint8_t stream[256] = {0};
    size_t stream_size = 0;
    if (!build_h264_ref_test_slice(&picture, &slices[0], 0x01, 0, 0, 2, 0,
                                   stream, sizeof(stream), &stream_size) ||
        !build_h264_ref_test_slice(&picture, &slices[1], 0x41, 1, 0, 2, 1,
                                   stream, sizeof(stream), &stream_size) ||
        !build_h264_ref_test_slice(&picture, &slices[2], 0x41, 1, 1, 3, 0,
                                   stream, sizeof(stream), &stream_size) ||
        !build_h264_ref_test_slice(&picture, &slices[3], 0x41, 1, 0, 2, 1,
                                   stream, sizeof(stream), &stream_size))
        return 0;

    const VABufferID ids[] = {1};
    const VABufferID data_ids[] = {2};
    drv.buffers[1] = (HobotBuffer){
        .id = 1, .allocated = 1, .type = VASliceParameterBufferType,
        .size = sizeof(slices), .element_size = sizeof(slices[0]),
        .num_elements = 4, .data = slices
    };
    drv.buffers[2] = (HobotBuffer){
        .id = 2, .allocated = 1, .type = VASliceDataBufferType,
        .size = (unsigned int)stream_size, .data = stream
    };

    unsigned int l0 = UINT_MAX;
    unsigned int l1 = UINT_MAX;
    int headers_present = 0;
    if (!hobot_h264_default_reference_counts(
            &drv, &picture, ids, 1, data_ids, 1,
            &l0, &l1, &headers_present) ||
        !headers_present || l0 != 2 || l1 != 1) {
        fprintf(stderr, "H.264 PPS defaults did not ignore per-slice explicit overrides: L0=%u L1=%u headers=%d\n",
                l0, l1, headers_present);
        return 0;
    }

    const VABufferID invalid_ids[] = {0};
    if (hobot_h264_default_reference_counts(
            &drv, &picture, invalid_ids, 1, data_ids, 1,
            &l0, &l1, &headers_present)) {
        fprintf(stderr, "H.264 PPS helper accepted the reserved buffer ID zero\n");
        return 0;
    }

    VAPictureParameterBufferH264 pps_picture = {0};
    uint8_t pps[256] = {0};
    int pps_size = generate_h264_pps(&pps_picture, l0, l1, pps, sizeof(pps));
    uint8_t pps_rbsp[256];
    size_t pps_rbsp_size = 0;
    if (pps_size < 2 ||
        !hobot_hevc_unescape_rbsp(pps + 1, (size_t)pps_size - 1u,
                                  pps_rbsp, sizeof(pps_rbsp), &pps_rbsp_size))
        return 0;
    BitReader pps_br = {pps_rbsp, pps_rbsp_size * 8u, 0};
    uint32_t ignored, pps_l0, pps_l1;
    if (!br_read_ue(&pps_br, &ignored, NULL, NULL) ||
        !br_read_ue(&pps_br, &ignored, NULL, NULL) ||
        !br_skip_bits(&pps_br, 2) ||
        !br_read_ue(&pps_br, &ignored, NULL, NULL) ||
        !br_read_ue(&pps_br, &pps_l0, NULL, NULL) ||
        !br_read_ue(&pps_br, &pps_l1, NULL, NULL) ||
        pps_l0 != 2 || pps_l1 != 1) {
        fprintf(stderr, "H.264 PPS did not encode the derived L0/L1 defaults\n");
        return 0;
    }

    slices[3].num_ref_idx_l1_active_minus1 = 0;
    if (hobot_h264_default_reference_counts(
            &drv, &picture, ids, 1, data_ids, 1,
            &l0, &l1, &headers_present)) {
        fprintf(stderr, "conflicting inherited H.264 PPS defaults were accepted\n");
        return 0;
    }

    memset(slices, 0, sizeof(slices));
    memset(stream, 0, sizeof(stream));
    stream_size = 0;
    if (!build_h264_ref_test_slice(&picture, &slices[0], 0x01, 0, 1, 0, 0,
                                   stream, sizeof(stream), &stream_size) ||
        !build_h264_ref_test_slice(&picture, &slices[1], 0x41, 1, 1, 2, 1,
                                   stream, sizeof(stream), &stream_size) ||
        !build_h264_ref_test_slice(&picture, &slices[2], 0x41, 1, 1, 4, 0,
                                   stream, sizeof(stream), &stream_size) ||
        !build_h264_ref_test_slice(&picture, &slices[3], 0x01, 0, 1, 1, 0,
                                   stream, sizeof(stream), &stream_size))
        return 0;
    drv.buffers[2].size = (unsigned int)stream_size;
    if (!hobot_h264_default_reference_counts(
            &drv, &picture, ids, 1, data_ids, 1,
            &l0, &l1, &headers_present) ||
        !headers_present || l0 != 0 || l1 != 0) {
        fprintf(stderr, "all-overridden H.264 slices did not permit unused zero PPS defaults: L0=%u L1=%u\n",
                l0, l1);
        return 0;
    }

    return 1;
}

static int read_h264_pps_reference_defaults(const HobotContext *hctx,
                                           unsigned int *l0,
                                           unsigned int *l1)
{
    if (!hctx || !l0 || !l1 || hctx->cached_pps_len <= 1)
        return 0;
    uint8_t rbsp[256];
    size_t rbsp_size = 0;
    if (!hobot_hevc_unescape_rbsp(hctx->cached_pps + 1,
                                  (size_t)hctx->cached_pps_len - 1u,
                                  rbsp, sizeof(rbsp), &rbsp_size))
        return 0;
    BitReader br = {rbsp, rbsp_size * 8u, 0};
    uint32_t ignored, parsed_l0, parsed_l1;
    if (!br_read_ue(&br, &ignored, NULL, NULL) ||
        !br_read_ue(&br, &ignored, NULL, NULL) ||
        !br_skip_bits(&br, 2) ||
        !br_read_ue(&br, &ignored, NULL, NULL) ||
        !br_read_ue(&br, &parsed_l0, NULL, NULL) ||
        !br_read_ue(&br, &parsed_l1, NULL, NULL))
        return 0;
    *l0 = parsed_l0;
    *l1 = parsed_l1;
    return 1;
}

static int h264_input_contains_pps(const HobotContext *hctx,
                                   const media_codec_buffer_t *input)
{
    if (!hctx || !input || !input->vstream_buf.vir_ptr ||
        hctx->cached_pps_len <= 0 || hctx->dec_in_buf_offset < 0)
        return 0;
    const uint8_t *bytes = (const uint8_t *)input->vstream_buf.vir_ptr;
    size_t size = (size_t)hctx->dec_in_buf_offset;
    for (size_t i = 0; i + 4u + (size_t)hctx->cached_pps_len <= size; i++) {
        if (bytes[i] == 0 && bytes[i + 1u] == 0 &&
            bytes[i + 2u] == 0 && bytes[i + 3u] == 1 &&
            memcmp(bytes + i + 4u, hctx->cached_pps,
                   (size_t)hctx->cached_pps_len) == 0)
            return 1;
    }
    return 0;
}

static int test_h264_pps_defaults_with_picture_and_slices_in_one_render(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    VAPictureParameterBufferH264 picture = {0};
    VASliceParameterBufferH264 slice = {0};
    uint8_t stream[64] = {0};
    size_t stream_size = 0;
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->id = 1;
    hctx->profile = VAProfileH264High;
    hctx->width = 640;
    hctx->height = 360;
    hctx->vpu_ctx.video_dec_params.bitstream_buf_size = sizeof(mock_input);
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->current_render_target = 29;
    hctx->decode_picture_active = 1;
    hctx->submitted_surfaces[hctx->sub_tail++ % 128] = 29;
    drv.surfaces[29].allocated = 1;
    drv.surfaces[29].context_id = hctx->id;
    drv.surfaces[29].decode_pending = 1;

    test_h264_init_context_picture(&picture);
    picture.CurrPic.picture_id = hctx->current_render_target;
    picture.seq_fields.bits.pic_order_cnt_type = 2;
    picture.num_ref_frames = 1;
    if (!build_h264_ref_test_slice(&picture, &slice, 0x41, 1, 0, 2, 1,
                                   stream, sizeof(stream), &stream_size)) {
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    drv.buffers[1] = (HobotBuffer){
        .id = 1, .allocated = 1, .type = VAPictureParameterBufferType,
        .size = sizeof(picture), .capacity = sizeof(picture),
        .element_size = sizeof(picture), .num_elements = 1, .data = &picture
    };
    drv.buffers[2] = (HobotBuffer){
        .id = 2, .allocated = 1, .type = VASliceParameterBufferType,
        .size = sizeof(slice), .capacity = sizeof(slice),
        .element_size = sizeof(slice), .num_elements = 1, .data = &slice
    };
    drv.buffers[3] = (HobotBuffer){
        .id = 3, .allocated = 1, .type = VASliceDataBufferType,
        .size = (unsigned int)stream_size, .capacity = (unsigned int)stream_size,
        .data = stream
    };
    VABufferID ids[] = {1, 2, 3};

    VAStatus status = hobot_vaRenderPicture(&va_ctx, 1, ids, 3);
    unsigned int pps_l0 = UINT_MAX;
    unsigned int pps_l1 = UINT_MAX;
    int pps_valid = read_h264_pps_reference_defaults(hctx, &pps_l0, &pps_l1);
    int passed = status == VA_STATUS_SUCCESS && pps_valid &&
        pps_l0 == 2 && pps_l1 == 1 && hctx->dec_in_buf_valid;
    if (!passed)
        fprintf(stderr, "same-call H.264 PPS defaults were not synthesized: status=%d valid=%d L0=%u L1=%u input=%d\n",
                status, pps_valid, pps_l0, pps_l1, hctx->dec_in_buf_valid);

    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_h264_pps_defaults_when_picture_and_slices_are_separate(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    VAPictureParameterBufferH264 picture = {0};
    VASliceParameterBufferH264 slice = {0};
    uint8_t stream[64] = {0};
    size_t stream_size = 0;
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->id = 1;
    hctx->profile = VAProfileH264High;
    hctx->width = 640;
    hctx->height = 360;
    hctx->vpu_ctx.video_dec_params.bitstream_buf_size = sizeof(mock_input);
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->current_render_target = 29;
    hctx->decode_picture_active = 1;
    hctx->submitted_surfaces[hctx->sub_tail++ % 128] = 29;
    drv.surfaces[29].allocated = 1;
    drv.surfaces[29].context_id = hctx->id;
    drv.surfaces[29].decode_pending = 1;

    test_h264_init_context_picture(&picture);
    picture.CurrPic.picture_id = hctx->current_render_target;
    picture.seq_fields.bits.pic_order_cnt_type = 2;
    picture.num_ref_frames = 3;
    if (!build_h264_ref_test_slice(&picture, &slice, 0x41, 1, 0, 2, 1,
                                   stream, sizeof(stream), &stream_size)) {
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    drv.buffers[1] = (HobotBuffer){
        .id = 1, .allocated = 1, .type = VAPictureParameterBufferType,
        .size = sizeof(picture), .capacity = sizeof(picture),
        .element_size = sizeof(picture), .num_elements = 1, .data = &picture
    };
    drv.buffers[2] = (HobotBuffer){
        .id = 2, .allocated = 1, .type = VASliceParameterBufferType,
        .size = sizeof(slice), .capacity = sizeof(slice),
        .element_size = sizeof(slice), .num_elements = 1, .data = &slice
    };
    drv.buffers[3] = (HobotBuffer){
        .id = 3, .allocated = 1, .type = VASliceDataBufferType,
        .size = (unsigned int)stream_size, .capacity = (unsigned int)stream_size,
        .data = stream
    };

    VABufferID picture_ids[] = {1};
    VAStatus status = hobot_vaRenderPicture(&va_ctx, 1, picture_ids, 1);
    unsigned int pps_l0 = UINT_MAX;
    unsigned int pps_l1 = UINT_MAX;
    int initial_pps_valid = status == VA_STATUS_SUCCESS &&
        read_h264_pps_reference_defaults(hctx, &pps_l0, &pps_l1) &&
        pps_l0 == 2 && pps_l1 == 0;

    VABufferID slice_ids[] = {2, 3};
    status = hobot_vaRenderPicture(&va_ctx, 1, slice_ids, 2);
    int updated_pps_valid = status == VA_STATUS_SUCCESS &&
        read_h264_pps_reference_defaults(hctx, &pps_l0, &pps_l1) &&
        pps_l0 == 2 && pps_l1 == 1 && hctx->dec_in_buf_valid &&
        h264_input_contains_pps(hctx, &hctx->dec_in_buf);
    int passed = initial_pps_valid && updated_pps_valid;
    if (!passed)
        fprintf(stderr, "separate-call H.264 PPS defaults failed: initial=%d updated=%d status=%d L0=%u L1=%u input=%d headers=%d\n",
                initial_pps_valid, updated_pps_valid, status,
                pps_l0, pps_l1, hctx->dec_in_buf_valid, hctx->headers_sent);

    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_h264_constrained_baseline_header_rejection(void)
{
    static const uint8_t wrong_profile[] = {0, 0, 1, 0x67, 77, 0x00, 41, 0x80};
    static const uint8_t reserved_bits[] = {0, 0, 1, 0x67, 66, 0x01, 41, 0x80};
    static const uint8_t malformed_header[] = {0, 0, 1, 0xe7, 66, 0x00, 41, 0x80};
    static const uint8_t zero_ref_idc[] = {0, 0, 1, 0x07, 66, 0x00, 41, 0x80};
    const uint8_t *cases[] = {wrong_profile, reserved_bits,
                              malformed_header, zero_ref_idc};
    const size_t sizes[] = {sizeof(wrong_profile), sizeof(reserved_bits),
                            sizeof(malformed_header), sizeof(zero_ref_idc)};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t stream[16] = {0};
        memcpy(stream, cases[i], sizes[i]);
        size_t stream_size = sizes[i];
        int found_sps = 0, found_pps = 0;
        if (hobot_h264_patch_constrained_baseline_headers(
                stream, &stream_size, sizeof(stream), 1,
                &found_sps, &found_pps)) {
            fprintf(stderr, "Constrained Baseline patch accepted malformed SPS case %zu\n", i);
            return 0;
        }
    }
    static const uint8_t no_headers[] = {0, 0, 1, 0x65, 0x80};
    uint8_t stream[sizeof(no_headers)];
    memcpy(stream, no_headers, sizeof(stream));
    size_t stream_size = sizeof(stream);
    int found_sps = 1, found_pps = 1;
    return hobot_h264_patch_constrained_baseline_headers(
               stream, &stream_size, sizeof(stream), 0,
               &found_sps, &found_pps) && !found_sps && !found_pps &&
           !hobot_h264_patch_constrained_baseline_headers(
               stream, &stream_size, sizeof(stream), 1,
               &found_sps, &found_pps);
}

static int test_pending_surface_is_synced_before_reuse(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->current_render_target = VA_INVALID_SURFACE;
    hctx->submitted_surfaces[hctx->sub_tail++] = 1;

    HobotSurface *surf = &drv.surfaces[1];
    surf->allocated = 1;
    surf->context_id = 1;
    surf->decode_pending = 1;

    mock_dequeue_result = 0;
    mock_dequeue_output_invalid = 0;
    mock_err_mb = 0;
    mock_queue_output_result = 0;
    mock_dequeue_output_calls = 0;
    mock_queue_output_calls = 0;
    VAStatus status = hobot_vaBeginPicture(&va_ctx, 1, 1);
    if (status != VA_STATUS_SUCCESS || mock_dequeue_output_calls != 1 ||
        mock_queue_output_calls != 1 || hctx->sub_head != 1 || hctx->sub_tail != 2 ||
        !surf->decode_pending || surf->has_decoded_frame ||
        hctx->current_render_target != 1) {
        fprintf(stderr, "pending surface reuse did not sync/recycle its output: status=%d dequeue=%d queue=%d fifo=%u/%u pending=%d frame=%d target=%u\n",
                status, mock_dequeue_output_calls, mock_queue_output_calls,
                hctx->sub_head, hctx->sub_tail, surf->decode_pending,
                surf->has_decoded_frame, hctx->current_render_target);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static int test_idle_surface_sync_is_ready_without_decoder_owner(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotSurface *surf = &drv.surfaces[1];
    surf->allocated = 1;
    mock_dequeue_output_calls = 0;
    VAStatus status = hobot_vaSyncSurface(&va_ctx, 1);
    if (status != VA_STATUS_SUCCESS || mock_dequeue_output_calls != 0) {
        fprintf(stderr, "idle surface sync requires a decoder owner: status=%d dequeue=%d\n",
                status, mock_dequeue_output_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    drv.contexts[2].allocated = 1;
    drv.contexts[2].vpu_running = 1;
    drv.contexts[2].is_encoder = 1;
    surf->context_id = 2;
    status = hobot_vaSyncSurface(&va_ctx, 1);
    if (status != VA_STATUS_SUCCESS || mock_dequeue_output_calls != 0) {
        fprintf(stderr, "completed encoder surface sync failed: status=%d dequeue=%d\n",
                status, mock_dequeue_output_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    surf->decode_error = 1;
    status = hobot_vaSyncSurface(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_DECODING_ERROR || mock_dequeue_output_calls != 0) {
        fprintf(stderr, "idle surface decode error was not preserved: status=%d dequeue=%d\n",
                status, mock_dequeue_output_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    surf->decode_error = 0;
    surf->context_id = 0;
    surf->width = 64;
    surf->height = 64;
    mock_graph_alloc_enabled = 0;
    mock_graph_alloc_calls = 0;
    struct hobot_surface_info info;
    status = hobot_fill_surface_info(&va_ctx, 1, &info);
    if (status != VA_STATUS_ERROR_ALLOCATION_FAILED || surf->has_preallocated ||
        mock_graph_alloc_calls != 1) {
        fprintf(stderr, "failed idle-surface allocation returned wrong result: status=%d allocated=%d calls=%d\n",
                status, surf->has_preallocated, mock_graph_alloc_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    mock_graph_alloc_enabled = 1;
    mock_graph_alloc_calls = 0;
    status = hobot_fill_surface_info(&va_ctx, 1, &info);
    int passed = status == VA_STATUS_SUCCESS && surf->has_preallocated &&
                 info.virt_addr[0] == mock_graph_data && mock_graph_alloc_calls == 1 &&
                 mock_dequeue_output_calls == 0;
    if (!passed)
        fprintf(stderr, "new idle surface failed lazy backing allocation: status=%d allocated=%d fd=%d graph_alloc=%d dequeue=%d\n",
                status, surf->has_preallocated, info.dma_fd,
                mock_graph_alloc_calls, mock_dequeue_output_calls);
    mock_graph_alloc_enabled = 0;
    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_surface_info_validates_nv12_layout_and_addresses(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    struct hobot_surface_info info;
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotSurface *surf = &drv.surfaces[1];
    surf->allocated = 1;
    surf->width = 64;
    surf->height = 64;
    surf->has_decoded_frame = 1;
    surf->dma_fd = 7;
    surf->stride = 65536;
    surf->vpu_out_buf.vframe_buf.fd[0] = 7;
    surf->vpu_out_buf.vframe_buf.size = UINT32_MAX;
    surf->vpu_out_buf.vframe_buf.stride = 65536;
    surf->vpu_out_buf.vframe_buf.vstride = 65536;
    surf->vpu_out_buf.vframe_buf.phy_ptr[0] = 0x1000;
    surf->vpu_out_buf.vframe_buf.vir_ptr[0] = mock_graph_data;
    VAStatus status = hobot_fill_surface_info(&va_ctx, 1, &info);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED) {
        fprintf(stderr, "surface-info UV offset overflow was accepted: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    surf->has_decoded_frame = 0;
    surf->has_preallocated = 1;
    surf->stride = 65536;
    surf->preallocated_gbuf.fd[0] = 7;
    surf->preallocated_gbuf.fd[1] = -1;
    surf->preallocated_gbuf.stride = 65536;
    surf->preallocated_gbuf.vstride = 65536;
    surf->preallocated_gbuf.phys_addr[0] = 0x1000;
    surf->preallocated_gbuf.phys_addr[1] = 0x2000;
    surf->preallocated_gbuf.virt_addr[0] = mock_graph_data;
    surf->preallocated_gbuf.virt_addr[1] = mock_graph_data + 8;
    status = hobot_fill_surface_info(&va_ctx, 1, &info);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED) {
        fprintf(stderr, "preallocated surface-info UV offset overflow was accepted: status=%d\n",
                status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    surf->has_decoded_frame = 1;
    surf->has_preallocated = 0;
    surf->stride = 64;
    memset(&surf->vpu_out_buf, 0, sizeof(surf->vpu_out_buf));
    surf->vpu_out_buf.vframe_buf.fd[0] = 7;
    surf->vpu_out_buf.vframe_buf.size = 1000;
    surf->vpu_out_buf.vframe_buf.stride = 64;
    surf->vpu_out_buf.vframe_buf.vstride = 64;
    surf->vpu_out_buf.vframe_buf.phy_ptr[0] = 0x1000;
    surf->vpu_out_buf.vframe_buf.vir_ptr[0] = mock_graph_data;
    status = hobot_fill_surface_info(&va_ctx, 1, &info);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED) {
        fprintf(stderr, "undersized decoded surface-info buffer was accepted: status=%d\n",
                status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    surf->vpu_out_buf.vframe_buf.size = 6144;
    surf->vpu_out_buf.vframe_buf.phy_ptr[0] = UINT64_MAX - 100;
    status = hobot_fill_surface_info(&va_ctx, 1, &info);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED) {
        fprintf(stderr, "surface-info physical UV address overflow was accepted: status=%d\n",
                status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    surf->vpu_out_buf.vframe_buf.phy_ptr[0] = 0x1000;
    surf->vpu_out_buf.vframe_buf.phy_ptr[1] = 0x2000;
    surf->vpu_out_buf.vframe_buf.vir_ptr[1] = mock_graph_data + 4096;
    status = hobot_fill_surface_info(&va_ctx, 1, &info);
    int passed = status == VA_STATUS_SUCCESS && info.width == 64 &&
                 info.height == 64 && info.stride == 64 && info.vstride == 64 &&
                 info.phys_addr[0] == 0x1000 && info.phys_addr[1] == 0x2000 &&
                 info.virt_addr[0] == mock_graph_data &&
                 info.virt_addr[1] == mock_graph_data + 4096;
    pthread_mutex_destroy(&drv.mutex);
    if (!passed)
        fprintf(stderr, "valid decoded NV12 surface-info was rejected or changed: status=%d stride=%u vstride=%u phys=%llx/%llx\n",
                status, info.stride, info.vstride,
                (unsigned long long)info.phys_addr[0],
                (unsigned long long)info.phys_addr[1]);
    return passed;
}

static int test_surface_sync_lock_contract(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->submitted_surfaces[hctx->sub_tail++] = 1;
    drv.surfaces[1].allocated = 1;
    drv.surfaces[1].context_id = 1;
    drv.surfaces[1].decode_pending = 1;

    mock_dequeue_result = 0;
    mock_dequeue_output_invalid = 0;
    mock_err_mb = 0;
    mock_dequeue_output_calls = 0;
    mock_queue_output_calls = 0;

    pthread_mutex_lock(&drv.mutex);
    VAStatus status = hobot_vaSyncSurfaceLocked(&drv, 1);
    int locked_try = pthread_mutex_trylock(&drv.mutex);
    if (locked_try == 0)
        pthread_mutex_unlock(&drv.mutex);
    pthread_mutex_unlock(&drv.mutex);
    if (status != VA_STATUS_SUCCESS || locked_try != EBUSY ||
        !drv.surfaces[1].has_decoded_frame || drv.surfaces[1].decode_pending ||
        hctx->sync_active || mock_dequeue_output_calls != 1) {
        fprintf(stderr, "locked surface sync did not retain caller mutex: status=%d try=%d frame=%d pending=%d active=%d dequeue=%d\n",
                status, locked_try, drv.surfaces[1].has_decoded_frame,
                drv.surfaces[1].decode_pending, hctx->sync_active,
                mock_dequeue_output_calls);
        if (drv.sync_cond_initialized)
            pthread_cond_destroy(&drv.sync_cond);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    status = hobot_vaSyncSurface(&va_ctx, 1);
    int unlocked_try = pthread_mutex_trylock(&drv.mutex);
    if (unlocked_try == 0)
        pthread_mutex_unlock(&drv.mutex);
    int passed = status == VA_STATUS_SUCCESS && unlocked_try == 0;
    if (!passed)
        fprintf(stderr, "public surface sync did not release wrapper mutex: status=%d try=%d\n",
                status, unlocked_try);
    if (drv.sync_cond_initialized)
        pthread_cond_destroy(&drv.sync_cond);
    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_va_lock_surface_lifetime(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->id = 1;
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->current_render_target = VA_INVALID_SURFACE;
    HobotSurface *surf = &drv.surfaces[1];
    surf->allocated = 1;
    surf->width = 64;
    surf->height = 64;
    surf->stride = 64;
    surf->dma_fd = 9;
    surf->has_preallocated = 1;
    surf->context_id = 1;
    surf->raw_data_valid = 1;
    surf->preallocated_gbuf.fd[0] = 9;
    surf->preallocated_gbuf.fd[1] = -1;
    surf->preallocated_gbuf.stride = 64;
    surf->preallocated_gbuf.vstride = 64;
    surf->preallocated_gbuf.is_contig = 1;
    surf->preallocated_gbuf.size[0] = 64u * 64u * 3u / 2u;
    surf->preallocated_gbuf.offset[1] = 64u * 64u;
    surf->preallocated_gbuf.virt_addr[0] = mock_graph_data;
    surf->preallocated_gbuf.virt_addr[1] = mock_graph_data + 64u * 64u;
    surf->raw_data_size = 64u * 64u * 3u / 2u;
    drv.images[1].allocated = 1;
    drv.images[1].buf_id = 1;
    drv.images[1].image.width = 64;
    drv.images[1].image.height = 64;
    drv.images[1].image.data_size = surf->raw_data_size;
    drv.images[1].image.pitches[0] = 64;
    drv.images[1].image.offsets[1] = 64u * 64u;
    drv.buffers[1].allocated = 1;
    drv.buffers[1].size = surf->raw_data_size;
    drv.buffers[1].data = mock_graph_data;

    unsigned int fourcc = 0;
    unsigned int stride = 0;
    unsigned int uv_offset = 0;
    unsigned int buffer_name = 0;
    void *buffer = NULL;
    mock_mem_invalidate_calls = 0;
    mock_mem_flush_calls = 0;
    mock_mem_invalidate_result = 0;
    mock_mem_flush_result = 0;
    VAStatus status = hobot_vaLockSurface(&va_ctx, 1, &fourcc, &stride,
                                          NULL, NULL, NULL, &uv_offset,
                                          NULL, &buffer_name, &buffer);
    if (status != VA_STATUS_SUCCESS || surf->lock_count != 1 ||
        surf->va_lock_count != 1 || surf->cpu_access_count != 1 ||
        fourcc != VA_FOURCC_NV12 || stride != 64 || uv_offset != 4096 ||
        buffer_name != 9 || buffer != mock_graph_data ||
        mock_mem_invalidate_calls != 1 || mock_mem_invalidate_fd != 9) {
        fprintf(stderr, "vaLockSurface failed to acquire backing lifetime: status=%d locks=%u fourcc=0x%x stride=%u uv=%u fd=%u ptr=%p\n",
                status, surf->lock_count, fourcc, stride, uv_offset,
                buffer_name, buffer);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    if (hobot_vaUnlockSurface(&va_ctx, 1) != VA_STATUS_SUCCESS ||
        surf->lock_count != 0 || surf->va_lock_count != 0 ||
        surf->cpu_access_count != 0 || surf->raw_data_valid ||
        mock_mem_flush_calls != 1 ||
        hobot_vaUnlockSurface(&va_ctx, 1) != VA_STATUS_ERROR_OPERATION_FAILED) {
        fprintf(stderr, "vaUnlockSurface did not balance CPU access ownership\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    status = hobot_vaLockSurface(&va_ctx, 1, &fourcc, &stride,
                                 NULL, NULL, NULL, &uv_offset,
                                 NULL, &buffer_name, &buffer);
    if (status != VA_STATUS_SUCCESS || surf->lock_count != 1 ||
        surf->va_lock_count != 1 || surf->cpu_access_count != 1) {
        fprintf(stderr, "vaLockSurface could not reacquire after unlock: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    status = hobot_vaLockSurface(&va_ctx, 1, NULL, NULL, NULL, NULL,
                                 NULL, NULL, NULL, NULL, &buffer);
    if (status != VA_STATUS_SUCCESS || surf->lock_count != 2 ||
        surf->va_lock_count != 2 || surf->cpu_access_count != 2 ||
        hobot_vaBeginPicture(&va_ctx, 1, 1) != VA_STATUS_ERROR_SURFACE_BUSY ||
        hobot_vaPutImage(&va_ctx, 1, 1, 0, 0, 64, 64, 0, 0, 64, 64) !=
            VA_STATUS_ERROR_SURFACE_BUSY) {
        fprintf(stderr, "nested surface lock did not block surface mutation/reuse: status=%d locks=%u\n",
                status, surf->lock_count);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    surf->decode_pending = 1;
    mock_dequeue_output_calls = 0;
    mock_queue_output_calls = 0;
    status = hobot_vaSyncSurface(&va_ctx, 1);
    surf->decode_pending = 0;
    if (status != VA_STATUS_ERROR_SURFACE_BUSY || mock_dequeue_output_calls != 0 ||
        mock_queue_output_calls != 0) {
        fprintf(stderr, "surface sync proceeded while a pending surface was locked: status=%d deq=%d queue=%d\n",
                status, mock_dequeue_output_calls, mock_queue_output_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    VASurfaceID surface_id = 1;
    if (hobot_vaDestroySurfaces(&va_ctx, &surface_id, 1) != VA_STATUS_ERROR_SURFACE_BUSY ||
        hobot_vaDestroyContext(&va_ctx, 1) != VA_STATUS_ERROR_SURFACE_BUSY ||
        hobot_vaTerminate(&va_ctx) != VA_STATUS_ERROR_SURFACE_BUSY ||
        va_ctx.pDriverData != &drv) {
        fprintf(stderr, "locked surface teardown was not refused: locks=%u\n", surf->lock_count);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    if (hobot_vaUnlockSurface(&va_ctx, 1) != VA_STATUS_SUCCESS ||
        surf->lock_count != 1 || surf->va_lock_count != 1 ||
        surf->cpu_access_count != 1 ||
        hobot_vaBeginPicture(&va_ctx, 1, 1) != VA_STATUS_ERROR_SURFACE_BUSY ||
        hobot_vaUnlockSurface(&va_ctx, 1) != VA_STATUS_SUCCESS ||
        surf->lock_count != 0 || surf->va_lock_count != 0 ||
        surf->cpu_access_count != 0) {
        fprintf(stderr, "surface lock reference count did not release correctly: locks=%u\n",
                surf->lock_count);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    surf->preallocated_gbuf.fd[1] = 10;
    surf->preallocated_gbuf.size[0] = 64u * 64u;
    surf->preallocated_gbuf.size[1] = 64u * 32u;
    surf->preallocated_gbuf.offset[1] = 0;
    int cache_calls_before = mock_mem_invalidate_calls;
    status = hobot_vaLockSurface(&va_ctx, 1, NULL, NULL, NULL, NULL,
                                 NULL, NULL, NULL, NULL, &buffer);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || surf->lock_count != 0 ||
        surf->va_lock_count != 0 || surf->cpu_access_count != 0 ||
        mock_mem_invalidate_calls != cache_calls_before) {
        fprintf(stderr,
                "vaLockSurface misrepresented separate-FD NV12 planes: status=%d locks=%u\n",
                status, surf->lock_count);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    surf->has_preallocated = 0;
    surf->has_decoded_frame = 1;
    surf->dma_fd = 9;
    surf->vpu_out_buf.vframe_buf.fd[0] = 9;
    surf->vpu_out_buf.vframe_buf.fd[1] = 10;
    surf->vpu_out_buf.vframe_buf.vir_ptr[0] = mock_graph_data;
    surf->vpu_out_buf.vframe_buf.vir_ptr[1] = mock_graph_data + 64u * 64u;
    surf->vpu_out_buf.vframe_buf.stride = 64;
    surf->vpu_out_buf.vframe_buf.vstride = 64;
    surf->vpu_out_buf.vframe_buf.size = 64u * 64u * 3u / 2u;
    status = hobot_vaLockSurface(&va_ctx, 1, NULL, NULL, NULL, NULL,
                                 NULL, NULL, NULL, NULL, &buffer);
    if (status != VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE ||
        surf->lock_count != 0 || surf->va_lock_count != 0 ||
        surf->cpu_access_count != 0 ||
        mock_mem_invalidate_calls != cache_calls_before) {
        fprintf(stderr,
                "vaLockSurface accepted decoded multi-FD NV12 planes: status=%d locks=%u\n",
                status, surf->lock_count);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    surf->has_decoded_frame = 0;
    surf->has_preallocated = 1;

    surf->preallocated_gbuf.fd[1] = -1;
    surf->preallocated_gbuf.size[0] = 64u * 64u * 3u / 2u;
    surf->preallocated_gbuf.size[1] = 0;
    surf->preallocated_gbuf.offset[1] = 64u * 64u;

    surf->has_decoded_frame = 0;
    surf->cpu_cache_flush_pending = 1;
    mock_mem_flush_result = -1;
    mock_mem_flush_calls = 0;
    status = hobot_vaBeginPicture(&va_ctx, 1, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED ||
        !surf->cpu_cache_flush_pending || hctx->decode_picture_active) {
        fprintf(stderr, "decoder began with a pending CPU cache flush: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    mock_mem_flush_result = 0;
    status = hobot_vaBeginPicture(&va_ctx, 1, 1);
    if (status != VA_STATUS_SUCCESS || surf->cpu_cache_flush_pending ||
        mock_mem_flush_calls != 2) {
        fprintf(stderr, "surface remained unavailable after cache flush retry: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    pthread_mutex_lock(&drv.mutex);
    hobot_abort_pending_decode_picture(&drv, hctx);
    pthread_mutex_unlock(&drv.mutex);
    int saved_stop_result = mock_stop_result;
    mock_stop_result = 0;
    VAStatus context_status = hobot_vaDestroyContext(&va_ctx, 1);
    status = hobot_vaDestroySurfaces(&va_ctx, &surface_id, 1);
    int passed = context_status == VA_STATUS_SUCCESS &&
                 surf->context_usage_mask == 0 &&
                 status == VA_STATUS_SUCCESS && !surf->allocated;
    if (!passed)
        fprintf(stderr, "surface cleanup after context teardown failed: context=%d status=%d allocated=%d usage=0x%x\n",
                context_status, status, surf->allocated,
                surf->context_usage_mask);
    mock_stop_result = saved_stop_result;
    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_active_encoder_surface_excludes_competing_access(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *encoder = &drv.contexts[1];
    encoder->allocated = 1;
    encoder->is_encoder = 1;
    encoder->encoder_picture_active = 1;
    encoder->current_render_target = 1;
    atomic_init(&encoder->enc_external_input_pending, 0);

    HobotContext *decoder = &drv.contexts[2];
    decoder->allocated = 1;
    decoder->vpu_running = 1;
    decoder->profile = VAProfileH264Main;
    decoder->width = 64;
    decoder->height = 64;

    HobotSurface *surf = &drv.surfaces[1];
    surf->allocated = 1;
    surf->width = 64;
    surf->height = 64;
    surf->stride = 64;
    surf->context_id = 2;
    surf->output_context_id = 2;
    drv.images[1].allocated = 1;

    unsigned int stride = 0;
    void *pixels = NULL;
    struct hobot_surface_info surface_info;
    VADRMPRIMESurfaceDescriptor prime = {0};
    VASurfaceID surface_id = 1;
    VAStatus begin_status = hobot_vaBeginPicture(&va_ctx, 2, 1);
    VAStatus sync_status = hobot_vaSyncSurface(&va_ctx, 1);
    VAStatus get_image_status = hobot_vaGetImage(&va_ctx, 1, 0, 0, 2, 2, 1);
    VAStatus put_image_status = hobot_vaPutImage(&va_ctx, 1, 1, 0, 0,
                                                 2, 2, 0, 0, 2, 2);
    VAStatus lock_status = hobot_vaLockSurface(&va_ctx, 1, NULL, &stride,
                                               NULL, NULL, NULL, NULL,
                                               NULL, NULL, &pixels);
    VAStatus info_status = hobot_fill_surface_info(&va_ctx, 1, &surface_info);
    VAStatus export_status = hobot_vaExportSurfaceHandle(
        &va_ctx, 1, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, 0, &prime);
    mock_stop_calls = 0;
    VAStatus destroy_context_status = hobot_vaDestroyContext(&va_ctx, 2);
    VAStatus destroy_surface_status = hobot_vaDestroySurfaces(&va_ctx,
                                                              &surface_id, 1);
    VASurfaceStatus query_status_value = VASurfaceReady;
    VAStatus query_status = hobot_vaQuerySurfaceStatus(&va_ctx, 1,
                                                       &query_status_value);

    int blocked = begin_status == VA_STATUS_ERROR_SURFACE_BUSY &&
                  sync_status == VA_STATUS_ERROR_SURFACE_BUSY &&
                  get_image_status == VA_STATUS_ERROR_SURFACE_BUSY &&
                  put_image_status == VA_STATUS_ERROR_SURFACE_BUSY &&
                  lock_status == VA_STATUS_ERROR_SURFACE_BUSY &&
                  info_status == VA_STATUS_ERROR_SURFACE_BUSY &&
                  export_status == VA_STATUS_ERROR_SURFACE_BUSY &&
                  destroy_context_status == VA_STATUS_ERROR_SURFACE_BUSY &&
                  destroy_surface_status == VA_STATUS_ERROR_SURFACE_BUSY &&
                  query_status == VA_STATUS_SUCCESS &&
                  query_status_value == VASurfaceRendering &&
                  mock_stop_calls == 0 && decoder->allocated && surf->allocated;
    if (!blocked) {
        fprintf(stderr,
                "active encoder surface access was not consistently blocked: begin=%d sync=%d get=%d put=%d lock=%d info=%d export=%d destroy_ctx=%d destroy_surf=%d query=%d/%d stop=%d\n",
                begin_status, sync_status, get_image_status, put_image_status,
                lock_status, info_status, export_status, destroy_context_status,
                destroy_surface_status, query_status, query_status_value,
                mock_stop_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    encoder->encoder_picture_active = 0;
    encoder->current_render_target = VA_INVALID_SURFACE;
    encoder->enc_external_surface = 1;
    atomic_store_explicit(&encoder->enc_external_input_pending, 1,
                          memory_order_release);
    if (hobot_vaDestroyContext(&va_ctx, 2) != VA_STATUS_ERROR_SURFACE_BUSY ||
        !decoder->allocated || !surf->allocated) {
        fprintf(stderr, "decoder context teardown raced an outstanding encoder input callback\n");
        atomic_store_explicit(&encoder->enc_external_input_pending, 0,
                              memory_order_release);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    atomic_store_explicit(&encoder->enc_external_input_pending, 0,
                          memory_order_release);
    encoder->enc_external_surface = VA_INVALID_SURFACE;
    decoder->vpu_running = 0;
    int released = hobot_vaDestroyContext(&va_ctx, 2) == VA_STATUS_SUCCESS &&
                   !decoder->allocated &&
                   hobot_vaDestroySurfaces(&va_ctx, &surface_id, 1) ==
                       VA_STATUS_SUCCESS && !surf->allocated;
    if (!released)
        fprintf(stderr, "surface/context remained busy after encoder picture completion\n");
    pthread_mutex_destroy(&drv.mutex);
    return released;
}

static int test_fatal_dequeue_poison_requires_context_teardown(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->vpu_initialized = 1;
    hctx->vpu_running = 1;
    hctx->current_render_target = VA_INVALID_SURFACE;
    hctx->submitted_surfaces[hctx->sub_tail++] = 1;

    HobotSurface *surf = &drv.surfaces[1];
    surf->allocated = 1;
    surf->context_id = 1;
    surf->decode_pending = 1;

    mock_dequeue_result = -1;
    mock_dequeue_output_calls = 0;
    mock_queue_output_result = 0;
    mock_stop_result = 0;
    mock_release_result = 0;
    VAStatus status = hobot_vaSyncSurface(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || !hctx->decode_failed ||
        !surf->decode_pending || !surf->decode_error || mock_dequeue_output_calls != 1) {
        fprintf(stderr, "fatal dequeue did not poison and retain pending ownership: status=%d failed=%d pending=%d error=%d dequeue=%d\n",
                status, hctx->decode_failed, surf->decode_pending, surf->decode_error,
                mock_dequeue_output_calls);
        pthread_mutex_destroy(&drv.mutex);
        mock_dequeue_result = 0;
        return 0;
    }

    if (hobot_vaBeginPicture(&va_ctx, 1, 1) != VA_STATUS_ERROR_OPERATION_FAILED) {
        fprintf(stderr, "poisoned decoder accepted another picture\n");
        pthread_mutex_destroy(&drv.mutex);
        mock_dequeue_result = 0;
        return 0;
    }
    VASurfaceID surface_id = 1;
    status = hobot_vaDestroySurfaces(&va_ctx, &surface_id, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || !surf->allocated ||
        !surf->decode_pending) {
        fprintf(stderr, "pending surface was freed before decoder teardown: status=%d allocated=%d pending=%d\n",
                status, surf->allocated, surf->decode_pending);
        pthread_mutex_destroy(&drv.mutex);
        mock_dequeue_result = 0;
        return 0;
    }

    status = hobot_vaDestroyContext(&va_ctx, 1);
    if (status != VA_STATUS_SUCCESS || hctx->allocated || surf->decode_pending ||
        surf->context_id != 0) {
        fprintf(stderr, "decoder teardown did not release poisoned pending surface: status=%d context=%d pending=%d owner=%u\n",
                status, hctx->allocated, surf->decode_pending, surf->context_id);
        pthread_mutex_destroy(&drv.mutex);
        mock_dequeue_result = 0;
        return 0;
    }

    status = hobot_vaDestroySurfaces(&va_ctx, &surface_id, 1);
    pthread_mutex_destroy(&drv.mutex);
    mock_dequeue_result = 0;
    return status == VA_STATUS_SUCCESS;
}

static int test_sync_preserves_output_on_old_buffer_recycle_failure(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    drv.contexts[1].allocated = 1;
    drv.contexts[1].vpu_running = 1;

    HobotSurface *requested = &drv.surfaces[1];
    requested->allocated = 1;
    requested->context_id = 1;
    requested->decode_pending = 1;

    HobotSurface *queued = &drv.surfaces[2];
    queued->allocated = 1;
    queued->context_id = 1;
    queued->output_context_id = 1;
    queued->has_decoded_frame = 1;
    queued->dma_fd = 5;
    queued->vpu_out_buf.vframe_buf.phy_ptr[0] = 1;
    queued->vpu_out_buf.vframe_buf.size = sizeof(mock_frame);
    drv.contexts[1].submitted_surfaces[0] = 2;
    drv.contexts[1].sub_tail = 1;

    mock_queue_output_calls = 0;
    mock_queue_output_result = -1;
    mock_dequeue_output_calls = 0;
    mock_dequeue_result = 0;
    VAStatus status = hobot_vaSyncSurface(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || mock_queue_output_calls != 2 ||
        mock_dequeue_output_calls != 0 || !queued->has_decoded_frame ||
        queued->dma_fd != 5 || queued->vpu_out_buf.vframe_buf.phy_ptr[0] != 1 ||
        !requested->decode_pending) {
        fprintf(stderr, "old output ownership lost on recycle failure: status=%d q=%d deq=%d held=%d fd=%d phy=%llu pending=%d\n",
                status, mock_queue_output_calls, mock_dequeue_output_calls,
                queued->has_decoded_frame, queued->dma_fd,
                (unsigned long long)queued->vpu_out_buf.vframe_buf.phy_ptr[0],
                requested->decode_pending);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    pthread_mutex_destroy(&drv.mutex);
    mock_queue_output_result = 0;
    mock_dequeue_result = -1;
    return 1;
}

static int test_sync_does_not_recycle_locked_output_surface(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    drv.contexts[1].allocated = 1;
    drv.contexts[1].vpu_running = 1;

    HobotSurface *requested = &drv.surfaces[1];
    requested->allocated = 1;
    requested->context_id = 1;
    requested->decode_pending = 1;
    HobotSurface *locked_output = &drv.surfaces[2];
    locked_output->allocated = 1;
    locked_output->output_context_id = 1;
    locked_output->has_decoded_frame = 1;
    locked_output->lock_count = 1;
    locked_output->dma_fd = 5;
    locked_output->vpu_out_buf.vframe_buf.phy_ptr[0] = 1;
    locked_output->vpu_out_buf.vframe_buf.size = sizeof(mock_frame);
    drv.contexts[1].submitted_surfaces[0] = 2;
    drv.contexts[1].sub_tail = 1;

    mock_queue_output_calls = 0;
    mock_dequeue_output_calls = 0;
    VAStatus status = hobot_vaSyncSurface(&va_ctx, 1);
    int passed = status == VA_STATUS_ERROR_SURFACE_BUSY &&
                 requested->decode_pending && !drv.contexts[1].decode_failed &&
                 !drv.contexts[1].sync_active && locked_output->has_decoded_frame &&
                 locked_output->lock_count == 1 && mock_queue_output_calls == 0 &&
                 mock_dequeue_output_calls == 0;
    if (!passed)
        fprintf(stderr, "sync touched a locked output surface: status=%d pending=%d failed=%d active=%d frame=%d locks=%u queue=%d dequeue=%d\n",
                status, requested->decode_pending, drv.contexts[1].decode_failed,
                drv.contexts[1].sync_active, locked_output->has_decoded_frame,
                locked_output->lock_count, mock_queue_output_calls,
                mock_dequeue_output_calls);
    if (drv.sync_cond_initialized)
        pthread_cond_destroy(&drv.sync_cond);
    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_destroy_context_preserves_pending_surface_on_recycle_failure(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    HobotContext *hctx = &drv.contexts[1];
    hctx->id = 1;
    hctx->allocated = 1;
    hctx->vpu_initialized = 1;
    hctx->vpu_running = 1;
    hctx->dec_in_buf_valid = 1;
    hctx->dec_in_buf_offset = 12;
    drv.surfaces[3].allocated = 1;
    drv.surfaces[3].context_id = 1;
    drv.surfaces[3].decode_pending = 1;

    mock_queue_input_result = -1;
    mock_queue_input_calls = 0;
    mock_stop_calls = 0;
    mock_release_calls = 0;
    VAStatus status = hobot_vaDestroyContext(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || !hctx->allocated ||
        !hctx->dec_in_buf_valid || hctx->dec_in_buf_offset != 12 ||
        !drv.surfaces[3].decode_pending || drv.surfaces[3].context_id != 1 ||
        mock_queue_input_calls != 2 || mock_stop_calls != 0 || mock_release_calls != 0) {
        fprintf(stderr, "destroy recycle failure lost ownership: status=%d allocated=%d input=%d offset=%d pending=%d owner=%u q=%d stop=%d release=%d\n",
                status, hctx->allocated, hctx->dec_in_buf_valid, hctx->dec_in_buf_offset,
                drv.surfaces[3].decode_pending, drv.surfaces[3].context_id,
                mock_queue_input_calls, mock_stop_calls, mock_release_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    mock_queue_input_result = 0;
    mock_stop_result = 0;
    mock_release_result = 0;
    status = hobot_vaDestroyContext(&va_ctx, 1);
    if (status != VA_STATUS_SUCCESS || hctx->allocated || hctx->dec_in_buf_valid ||
        drv.surfaces[3].decode_pending || !drv.surfaces[3].decode_error ||
        drv.surfaces[3].context_id != 0 || mock_stop_calls != 1 || mock_release_calls != 1) {
        fprintf(stderr, "destroy retry failed to detach surfaces: status=%d allocated=%d input=%d pending=%d error=%d owner=%u stop=%d release=%d\n",
                status, hctx->allocated, hctx->dec_in_buf_valid,
                drv.surfaces[3].decode_pending, drv.surfaces[3].decode_error,
                drv.surfaces[3].context_id, mock_stop_calls, mock_release_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    pthread_mutex_destroy(&drv.mutex);
    mock_queue_input_result = 0;
    return 1;
}

static void setup_pending_picture(HobotDriverData *drv, HobotContext *hctx,
                                 VASurfaceID surface)
{
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->current_render_target = surface;
    hctx->decode_picture_active = 1;
    hctx->submitted_surfaces[hctx->sub_tail++ % 128] = surface;
    drv->surfaces[surface].allocated = 1;
    drv->surfaces[surface].context_id = hctx->id;
    drv->surfaces[surface].decode_pending = 1;
}

static int test_abort_pending_picture_across_fifo_wrap(void)
{
    HobotDriverData drv = {0};
    HobotContext *hctx = &drv.contexts[1];
    hctx->sub_head = UINT32_MAX;
    hctx->sub_tail = 0;
    hctx->submitted_surfaces[UINT32_MAX % 128] = 6;
    hctx->current_render_target = 6;
    hctx->decode_picture_active = 1;
    drv.surfaces[6].allocated = 1;
    drv.surfaces[6].decode_pending = 1;

    hobot_abort_pending_decode_picture(&drv, hctx);
    if (hctx->sub_head != hctx->sub_tail || hctx->decode_picture_active ||
        hctx->current_render_target != VA_INVALID_SURFACE ||
        drv.surfaces[6].decode_pending || !drv.surfaces[6].decode_error) {
        fprintf(stderr, "abort at FIFO counter wrap failed: fifo=%u/%u active=%d target=%u pending=%d error=%d\n",
                hctx->sub_head, hctx->sub_tail, hctx->decode_picture_active,
                hctx->current_render_target, drv.surfaces[6].decode_pending,
                drv.surfaces[6].decode_error);
        return 0;
    }
    return 1;
}

static int test_decoder_picture_failure_cleanup(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    HobotContext *hctx = &drv.contexts[1];
    hctx->id = 1;
    hctx->profile = VAProfileH264High;
    setup_pending_picture(&drv, hctx, 6);

    VAStatus status = hobot_vaEndPicture(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || hctx->decode_picture_active ||
        hctx->current_render_target != VA_INVALID_SURFACE || hctx->sub_head != hctx->sub_tail ||
        drv.surfaces[6].decode_pending || !drv.surfaces[6].decode_error) {
        fprintf(stderr, "empty-picture cleanup failed: status=%d active=%d target=%u fifo=%u/%u pending=%d error=%d\n",
                status, hctx->decode_picture_active, hctx->current_render_target,
                hctx->sub_head, hctx->sub_tail, drv.surfaces[6].decode_pending,
                drv.surfaces[6].decode_error);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    memset(hctx, 0, sizeof(*hctx));
    hctx->id = 1;
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->current_render_target = 10;
    hctx->decode_picture_active = 1;
    hctx->submitted_surfaces[hctx->sub_tail++ % 128] = 10;
    hctx->dec_in_buf_valid = 1;
    hctx->dec_in_buf_offset = 4;
    drv.surfaces[10].allocated = 1;
    drv.surfaces[10].context_id = 1;
    drv.surfaces[10].decode_pending = 1;
    mock_queue_input_result = 0;
    mock_queue_input_calls = 0;
    status = hobot_vaEndPicture(&va_ctx, 1);
    if (status != VA_STATUS_SUCCESS || hctx->decode_picture_active ||
        hctx->current_render_target != VA_INVALID_SURFACE || hctx->dec_in_buf_valid ||
        hctx->sub_tail - hctx->sub_head != 1 || !drv.surfaces[10].decode_pending ||
        mock_queue_input_calls != 1) {
        fprintf(stderr, "successful decoder submission state failed: status=%d active=%d target=%u input=%d fifo=%u/%u pending=%d calls=%d\n",
                status, hctx->decode_picture_active, hctx->current_render_target,
                hctx->dec_in_buf_valid, hctx->sub_head, hctx->sub_tail,
                drv.surfaces[10].decode_pending, mock_queue_input_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    memset(hctx, 0, sizeof(*hctx));
    hctx->id = 1;
    hctx->profile = VAProfileH264High;
    setup_pending_picture(&drv, hctx, 7);
    drv.buffers[1].allocated = 1;
    drv.buffers[1].type = VASliceDataBufferType;
    drv.buffers[1].data = mock_input;
    drv.buffers[1].size = 4;
    VABufferID slice = 1;
    mock_dequeue_input_result = -1;
    mock_dequeue_input_null = 0;
    mock_dequeue_input_calls = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, &slice, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || mock_dequeue_input_calls != 5 ||
        hctx->decode_picture_active || hctx->sub_head != hctx->sub_tail ||
        drv.surfaces[7].decode_pending || !drv.surfaces[7].decode_error) {
        fprintf(stderr, "dequeue-failure cleanup failed: status=%d calls=%d active=%d fifo=%u/%u pending=%d error=%d\n",
                status, mock_dequeue_input_calls, hctx->decode_picture_active,
                hctx->sub_head, hctx->sub_tail, drv.surfaces[7].decode_pending,
                drv.surfaces[7].decode_error);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    memset(hctx, 0, sizeof(*hctx));
    hctx->id = 1;
    hctx->profile = VAProfileH264High;
    setup_pending_picture(&drv, hctx, 8);
    mock_dequeue_input_result = 0;
    mock_dequeue_input_null = 1;
    mock_dequeue_input_calls = 0;
    mock_queue_input_result = 0;
    mock_queue_input_calls = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, &slice, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || mock_dequeue_input_calls != 1 ||
        mock_queue_input_calls != 1 || hctx->dec_in_buf_valid || hctx->decode_failed ||
        hctx->decode_picture_active || drv.surfaces[8].decode_pending ||
        !drv.surfaces[8].decode_error) {
        fprintf(stderr, "invalid-input-buffer cleanup failed: status=%d deq=%d q=%d valid=%d failed=%d active=%d pending=%d error=%d\n",
                status, mock_dequeue_input_calls, mock_queue_input_calls,
                hctx->dec_in_buf_valid, hctx->decode_failed,
                hctx->decode_picture_active, drv.surfaces[8].decode_pending,
                drv.surfaces[8].decode_error);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    memset(hctx, 0, sizeof(*hctx));
    hctx->id = 1;
    hctx->profile = VAProfileH264High;
    hctx->vpu_ctx.video_dec_params.bitstream_buf_size = 8;
    setup_pending_picture(&drv, hctx, 9);
    drv.buffers[1].data = mock_input;
    drv.buffers[1].size = 6;
    mock_dequeue_input_null = 0;
    mock_queue_input_calls = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, &slice, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || mock_queue_input_calls != 1 ||
        hctx->dec_in_buf_valid || hctx->decode_failed || hctx->decode_picture_active ||
        drv.surfaces[9].decode_pending || !drv.surfaces[9].decode_error) {
        fprintf(stderr, "overflow cleanup failed: status=%d q=%d valid=%d failed=%d active=%d pending=%d error=%d\n",
                status, mock_queue_input_calls, hctx->dec_in_buf_valid,
                hctx->decode_failed, hctx->decode_picture_active,
                drv.surfaces[9].decode_pending, drv.surfaces[9].decode_error);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    pthread_mutex_destroy(&drv.mutex);
    mock_dequeue_input_result = 0;
    mock_dequeue_input_null = 0;
    mock_queue_input_result = 0;
    return 1;
}

static int test_decoder_h264_slice_parameters(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    uint8_t source[] = {0xaa, 0xbb, 0x65, 0x12, 0x34,
                        0x00, 0x00, 0x01, 0x41, 0x56};
    VASliceParameterBufferH264 slices[2] = {0};
    VABufferID ids[] = {1, 2};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    mock_dequeue_input_result = 0;
    mock_dequeue_input_null = 0;
    mock_dequeue_input_calls = 0;
    mock_queue_input_result = 0;
    mock_queue_input_calls = 0;

    HobotContext *hctx = &drv.contexts[1];
    hctx->id = 1;
    hctx->profile = VAProfileH264High;
    hctx->vpu_ctx.video_dec_params.bitstream_buf_size = sizeof(mock_input);
    setup_pending_picture(&drv, hctx, 20);
    drv.buffers[1] = (HobotBuffer){
        .id = 1, .allocated = 1, .type = VASliceParameterBufferType,
        .size = sizeof(VASliceParameterBufferH264),
        .capacity = sizeof(VASliceParameterBufferH264),
        .element_size = sizeof(VASliceParameterBufferH264),
        .num_elements = 1, .data = slices
    };
    drv.buffers[2] = (HobotBuffer){
        .id = 2, .allocated = 1, .type = VASliceDataBufferType,
        .size = sizeof(source), .capacity = sizeof(source), .data = source
    };

    slices[0].slice_data_offset = 2;
    slices[0].slice_data_size = 3;
    slices[0].slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    memset(mock_input, 0, sizeof(mock_input));
    VAStatus status = hobot_vaRenderPicture(&va_ctx, 1, ids, 2);
    const uint8_t offset_expected[] = {0, 0, 0, 1, 0x65, 0x12, 0x34};
    if (status != VA_STATUS_SUCCESS || hctx->dec_in_buf_offset != sizeof(offset_expected) ||
        memcmp(mock_input, offset_expected, sizeof(offset_expected)) != 0 ||
        mock_dequeue_input_calls != 1) {
        fprintf(stderr, "H.264 slice offset/size assembly failed: status=%d offset=%d dequeues=%d\n",
                status, hctx->dec_in_buf_offset, mock_dequeue_input_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    if (hobot_vaEndPicture(&va_ctx, 1) != VA_STATUS_SUCCESS ||
        mock_queue_input_calls != 1) {
        fprintf(stderr, "single H.264 slice was not submitted cleanly\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    memset(hctx, 0, sizeof(*hctx));
    hctx->id = 1;
    hctx->profile = VAProfileH264High;
    hctx->vpu_ctx.video_dec_params.bitstream_buf_size = sizeof(mock_input);
    setup_pending_picture(&drv, hctx, 27);
    source[2] = 0x61;
    slices[0] = (VASliceParameterBufferH264){
        .slice_data_offset = 2, .slice_data_size = 3,
        .slice_data_flag = VA_SLICE_DATA_FLAG_ALL,
        .slice_type = 1
    };
    drv.buffers[1].size = sizeof(VASliceParameterBufferH264);
    drv.buffers[1].capacity = sizeof(VASliceParameterBufferH264);
    drv.buffers[1].num_elements = 1;
    mock_dequeue_input_calls = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, ids, 2);
    if (status != VA_STATUS_SUCCESS || mock_dequeue_input_calls != 1 ||
        hobot_vaEndPicture(&va_ctx, 1) != VA_STATUS_SUCCESS) {
        fprintf(stderr, "reference H.264 B slice was rejected before VPU submission: status=%d deq=%d\n",
                status, mock_dequeue_input_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    memset(hctx, 0, sizeof(*hctx));
    hctx->id = 1;
    hctx->profile = VAProfileH264High;
    hctx->vpu_ctx.video_dec_params.bitstream_buf_size = sizeof(mock_input);
    setup_pending_picture(&drv, hctx, 28);
    source[2] = 0x01;
    slices[0].slice_type = 1;
    mock_dequeue_input_calls = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, ids, 2);
    if (status != VA_STATUS_SUCCESS || mock_dequeue_input_calls != 1 ||
        hobot_vaEndPicture(&va_ctx, 1) != VA_STATUS_SUCCESS) {
        fprintf(stderr, "non-reference H.264 B slice was rejected: status=%d deq=%d\n",
                status, mock_dequeue_input_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    source[2] = 0x65;

    memset(hctx, 0, sizeof(*hctx));
    hctx->id = 1;
    hctx->profile = VAProfileH264High;
    hctx->vpu_ctx.video_dec_params.bitstream_buf_size = sizeof(mock_input);
    setup_pending_picture(&drv, hctx, 21);
    slices[0] = (VASliceParameterBufferH264){
        .slice_data_offset = 2, .slice_data_size = 2,
        .slice_data_flag = VA_SLICE_DATA_FLAG_ALL
    };
    slices[1] = (VASliceParameterBufferH264){
        .slice_data_offset = 8, .slice_data_size = 2,
        .slice_data_flag = VA_SLICE_DATA_FLAG_ALL
    };
    drv.buffers[1].size = sizeof(slices);
    drv.buffers[1].capacity = sizeof(slices);
    drv.buffers[1].num_elements = 2;
    memset(mock_input, 0, sizeof(mock_input));
    mock_dequeue_input_calls = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, ids, 2);
    const uint8_t packed_expected[] = {
        0, 0, 0, 1, 0x65, 0x12, 0, 0, 0, 1, 0x41, 0x56
    };
    if (status != VA_STATUS_SUCCESS || hctx->dec_in_buf_offset != sizeof(packed_expected) ||
        memcmp(mock_input, packed_expected, sizeof(packed_expected)) != 0 ||
        mock_dequeue_input_calls != 1) {
        fprintf(stderr, "packed H.264 slice assembly failed: status=%d offset=%d\n",
                status, hctx->dec_in_buf_offset);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    mock_queue_input_calls = 0;
    if (hobot_vaEndPicture(&va_ctx, 1) != VA_STATUS_SUCCESS ||
        mock_queue_input_calls != 1) {
        fprintf(stderr, "packed H.264 slices were not submitted cleanly\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    memset(hctx, 0, sizeof(*hctx));
    hctx->id = 1;
    hctx->profile = VAProfileH264High;
    hctx->vpu_ctx.video_dec_params.bitstream_buf_size = sizeof(mock_input);
    setup_pending_picture(&drv, hctx, 24);
    slices[0] = (VASliceParameterBufferH264){
        .slice_data_offset = 2, .slice_data_size = 2,
        .slice_data_flag = VA_SLICE_DATA_FLAG_ALL
    };
    slices[1] = (VASliceParameterBufferH264){
        .slice_data_offset = 8, .slice_data_size = 2,
        .slice_data_flag = VA_SLICE_DATA_FLAG_ALL
    };
    drv.buffers[1].size = sizeof(VASliceParameterBufferH264);
    drv.buffers[1].capacity = sizeof(VASliceParameterBufferH264);
    drv.buffers[1].num_elements = 1;
    drv.buffers[1].data = &slices[0];
    drv.buffers[3] = (HobotBuffer){
        .id = 3, .allocated = 1, .type = VASliceParameterBufferType,
        .size = sizeof(VASliceParameterBufferH264),
        .capacity = sizeof(VASliceParameterBufferH264),
        .element_size = sizeof(VASliceParameterBufferH264),
        .num_elements = 1, .data = &slices[1]
    };
    drv.buffers[4] = (HobotBuffer){
        .id = 4, .allocated = 1, .type = VASliceDataBufferType,
        .size = sizeof(source), .capacity = sizeof(source), .data = source
    };
    VABufferID paired_ids[] = {1, 2, 3, 4};
    memset(mock_input, 0, sizeof(mock_input));
    mock_dequeue_input_calls = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, paired_ids, 4);
    if (status != VA_STATUS_SUCCESS || hctx->dec_in_buf_offset != sizeof(packed_expected) ||
        memcmp(mock_input, packed_expected, sizeof(packed_expected)) != 0 ||
        mock_dequeue_input_calls != 1) {
        fprintf(stderr, "per-slice H.264 buffer pairing failed: status=%d offset=%d\n",
                status, hctx->dec_in_buf_offset);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    mock_queue_input_calls = 0;
    if (hobot_vaEndPicture(&va_ctx, 1) != VA_STATUS_SUCCESS ||
        mock_queue_input_calls != 1) {
        fprintf(stderr, "paired H.264 slices were not submitted cleanly\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    memset(hctx, 0, sizeof(*hctx));
    hctx->id = 1;
    hctx->profile = VAProfileH264High;
    hctx->vpu_ctx.video_dec_params.bitstream_buf_size = sizeof(mock_input);
    setup_pending_picture(&drv, hctx, 22);
    mock_queue_input_calls = 0;
    drv.buffers[1].size = sizeof(VASliceParameterBufferH264);
    drv.buffers[1].capacity = sizeof(VASliceParameterBufferH264);
    drv.buffers[1].num_elements = 1;
    slices[0] = (VASliceParameterBufferH264){
        .slice_data_offset = 2, .slice_data_size = 2,
        .slice_data_flag = VA_SLICE_DATA_FLAG_BEGIN
    };
    memset(mock_input, 0, sizeof(mock_input));
    mock_dequeue_input_calls = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, ids, 2);
    if (status != VA_STATUS_SUCCESS || !hctx->decode_slice_fragment_open ||
        hctx->dec_in_buf_offset != 6 || memcmp(mock_input, "\0\0\0\1\x65\x12", 6) != 0) {
        fprintf(stderr, "H.264 BEGIN fragment assembly failed: status=%d open=%d offset=%d\n",
                status, hctx->decode_slice_fragment_open, hctx->dec_in_buf_offset);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    slices[0].slice_data_offset = 4;
    slices[0].slice_data_size = 2;
    slices[0].slice_data_flag = VA_SLICE_DATA_FLAG_MIDDLE;
    status = hobot_vaRenderPicture(&va_ctx, 1, ids, 2);
    if (status != VA_STATUS_SUCCESS || !hctx->decode_slice_fragment_open ||
        hctx->dec_in_buf_offset != 8 || mock_input[6] != 0x34 || mock_input[7] != 0x00) {
        fprintf(stderr, "H.264 MIDDLE fragment assembly failed: status=%d open=%d offset=%d\n",
                status, hctx->decode_slice_fragment_open, hctx->dec_in_buf_offset);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    slices[0].slice_data_offset = 9;
    slices[0].slice_data_size = 1;
    slices[0].slice_data_flag = VA_SLICE_DATA_FLAG_END;
    status = hobot_vaRenderPicture(&va_ctx, 1, ids, 2);
    if (status != VA_STATUS_SUCCESS || hctx->decode_slice_fragment_open ||
        hctx->dec_in_buf_offset != 9 || mock_input[8] != 0x56) {
        fprintf(stderr, "H.264 END fragment assembly failed: status=%d open=%d offset=%d\n",
                status, hctx->decode_slice_fragment_open, hctx->dec_in_buf_offset);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    status = hobot_vaEndPicture(&va_ctx, 1);
    if (status != VA_STATUS_SUCCESS || mock_queue_input_calls != 1) {
        fprintf(stderr, "complete H.264 fragmented slice submission failed: status=%d queues=%d\n",
                status, mock_queue_input_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    memset(hctx, 0, sizeof(*hctx));
    hctx->id = 1;
    hctx->profile = VAProfileH264High;
    hctx->vpu_ctx.video_dec_params.bitstream_buf_size = sizeof(mock_input);
    setup_pending_picture(&drv, hctx, 23);
    slices[0] = (VASliceParameterBufferH264){
        .slice_data_offset = UINT32_MAX, .slice_data_size = 1,
        .slice_data_flag = VA_SLICE_DATA_FLAG_ALL
    };
    drv.buffers[1].size = sizeof(VASliceParameterBufferH264);
    drv.buffers[1].capacity = sizeof(VASliceParameterBufferH264);
    drv.buffers[1].num_elements = 1;
    mock_dequeue_input_calls = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, ids, 2);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER || mock_dequeue_input_calls != 0 ||
        hctx->dec_in_buf_valid || !hctx->decode_picture_active) {
        fprintf(stderr, "invalid H.264 slice range was not rejected before dequeue: status=%d deq=%d\n",
                status, mock_dequeue_input_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    memset(hctx, 0, sizeof(*hctx));
    hctx->id = 1;
    hctx->profile = VAProfileH264High;
    hctx->vpu_ctx.video_dec_params.bitstream_buf_size = sizeof(mock_input);
    setup_pending_picture(&drv, hctx, 25);
    const uint8_t annexb_source[] = {0x00, 0x00, 0x01, 0x65, 0x7a};
    drv.buffers[5] = (HobotBuffer){
        .id = 5, .allocated = 1, .type = VASliceDataBufferType,
        .size = sizeof(annexb_source), .capacity = sizeof(annexb_source),
        .data = (void *)annexb_source
    };
    VABufferID raw_id = 5;
    memset(mock_input, 0, sizeof(mock_input));
    mock_dequeue_input_calls = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, &raw_id, 1);
    if (status != VA_STATUS_SUCCESS || hctx->dec_in_buf_offset != sizeof(annexb_source) ||
        memcmp(mock_input, annexb_source, sizeof(annexb_source)) != 0 ||
        mock_dequeue_input_calls != 1) {
        fprintf(stderr, "legacy Annex-B H.264 input was changed: status=%d offset=%d\n",
                status, hctx->dec_in_buf_offset);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    mock_queue_input_calls = 0;
    if (hobot_vaEndPicture(&va_ctx, 1) != VA_STATUS_SUCCESS ||
        mock_queue_input_calls != 1) {
        fprintf(stderr, "legacy Annex-B H.264 picture was not submitted cleanly\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    memset(hctx, 0, sizeof(*hctx));
    hctx->id = 1;
    hctx->profile = VAProfileH264High;
    hctx->vpu_ctx.video_dec_params.bitstream_buf_size = sizeof(mock_input);
    setup_pending_picture(&drv, hctx, 26);
    slices[0].slice_data_offset = 2;
    slices[0].slice_data_size = 2;
    slices[0].slice_data_flag = VA_SLICE_DATA_FLAG_BEGIN;
    drv.buffers[1].data = &slices[0];
    drv.buffers[1].size = sizeof(VASliceParameterBufferH264);
    drv.buffers[1].capacity = sizeof(VASliceParameterBufferH264);
    drv.buffers[1].num_elements = 1;
    memset(mock_input, 0, sizeof(mock_input));
    mock_dequeue_input_calls = 0;
    mock_queue_input_calls = 0;
    status = hobot_vaRenderPicture(&va_ctx, 1, ids, 2);
    if (status != VA_STATUS_SUCCESS ||
        hobot_vaEndPicture(&va_ctx, 1) != VA_STATUS_ERROR_INVALID_PARAMETER ||
        hctx->decode_picture_active || hctx->decode_slice_fragment_open ||
        mock_queue_input_calls != 1) {
        fprintf(stderr, "unfinished H.264 fragment was not rejected and recycled: status=%d queues=%d\n",
                status, mock_queue_input_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    pthread_mutex_destroy(&drv.mutex);
    mock_dequeue_input_result = 0;
    mock_dequeue_input_null = 0;
    mock_queue_input_result = 0;
    return 1;
}

static void setup_mock_prime2_nv12(VADRMPRIMESurfaceDescriptor *descriptor,
                                  int fd)
{
    const uint32_t width = 640;
    const uint32_t height = 360;
    const uint32_t stride = 640;
    const uint32_t vstride = 384;
    const uint64_t y_size = (uint64_t)stride * vstride;
    const uint64_t uv_size = (uint64_t)stride * (vstride / 2u);

    memset(&mock_external_graph_buf, 0, sizeof(mock_external_graph_buf));
    for (size_t i = 0; i < MAX_GRAPHIC_BUF_COMP; i++)
        mock_external_graph_buf.fd[i] = -1;
    mock_external_graph_buf.fd[0] = fd;
    mock_external_graph_buf.plane_cnt = 2;
    mock_external_graph_buf.format = MEM_PIX_FMT_NV12;
    mock_external_graph_buf.width = (int32_t)width;
    mock_external_graph_buf.height = (int32_t)height;
    mock_external_graph_buf.stride = (int32_t)stride;
    mock_external_graph_buf.vstride = (int32_t)vstride;
    mock_external_graph_buf.is_contig = 1;
    mock_external_graph_buf.size[0] = y_size;
    mock_external_graph_buf.size[1] = uv_size;
    mock_external_graph_buf.virt_addr[0] = mock_graph_data;
    mock_external_graph_buf.virt_addr[1] = mock_graph_data + y_size;
    mock_external_graph_buf.phys_addr[0] = 0x100000;
    mock_external_graph_buf.phys_addr[1] = 0x100000 + y_size;

    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->fourcc = VA_FOURCC_NV12;
    descriptor->width = width;
    descriptor->height = height;
    descriptor->num_objects = 1;
    descriptor->objects[0].fd = fd;
    descriptor->objects[0].size = (uint32_t)(y_size + uv_size);
    descriptor->objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
    descriptor->num_layers = 1;
    descriptor->layers[0].drm_format = DRM_FORMAT_NV12;
    descriptor->layers[0].num_planes = 2;
    descriptor->layers[0].object_index[0] = 0;
    descriptor->layers[0].object_index[1] = 0;
    descriptor->layers[0].offset[0] = 0;
    descriptor->layers[0].offset[1] = (uint32_t)y_size;
    descriptor->layers[0].pitch[0] = stride;
    descriptor->layers[0].pitch[1] = stride;
}

static int test_prime2_surface_import(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    mock_mem_get_graph_buf_result = 0;
    mock_mem_import_graph_buf_result = 0;
    mock_mem_import_bad_metadata = 0;
    mock_mem_free_result = 0;
    mock_mem_import_next_fd = 200;
    VADRMPRIMESurfaceDescriptor descriptor;
    setup_mock_prime2_nv12(&descriptor, STDERR_FILENO);

    VASurfaceAttrib attrs[2] = {0};
    attrs[0].type = VASurfaceAttribMemoryType;
    attrs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[0].value.type = VAGenericValueTypeInteger;
    attrs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attrs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attrs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[1].value.type = VAGenericValueTypePointer;
    attrs[1].value.value.p = &descriptor;

    VASurfaceID surface = VA_INVALID_SURFACE;
    descriptor.objects[0].drm_format_modifier = 1;
    VAStatus status = hobot_vaCreateSurfaces2(&va_ctx, VA_RT_FORMAT_YUV420,
                                               640, 360, &surface, 1, attrs, 2);
    if (status != VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE ||
        drv.surfaces[1].allocated || mock_mem_import_graph_buf_calls != 0) {
        fprintf(stderr, "non-linear PRIME2 import was not rejected before import: status=%d\n",
                status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    descriptor.objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;

    mock_mem_import_graph_buf_result = -1;
    status = hobot_vaCreateSurfaces2(&va_ctx, VA_RT_FORMAT_YUV420,
                                     640, 360, &surface, 1, attrs, 2);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || drv.surfaces[1].allocated) {
        fprintf(stderr, "HBmem import failure was not propagated: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    mock_mem_import_graph_buf_result = 0;

    int free_calls_before = mock_mem_free_calls;
    status = hobot_vaCreateSurfaces2(&va_ctx, VA_RT_FORMAT_YUV420,
                                     640, 360, &surface, 1, attrs, 2);
    if (status != VA_STATUS_SUCCESS || surface <= 0 || surface >= MAX_SURFACES ||
        !drv.surfaces[surface].allocated || !drv.surfaces[surface].has_preallocated ||
        drv.surfaces[surface].preallocated_gbuf.fd[0] != 200 ||
        drv.surfaces[surface].dma_fd != 200 ||
        drv.surfaces[surface].stride != 640 ||
        drv.surfaces[surface].raw_data_size != 368640 ||
        mock_mem_get_graph_buf_fd != STDERR_FILENO ||
        fcntl(STDERR_FILENO, F_GETFD) < 0 ||
        mock_mem_free_calls != free_calls_before) {
        fprintf(stderr, "valid PRIME2 import failed: status=%d id=%u owned_fd=%d stride=%d size=%u\n",
                status, surface,
                surface < MAX_SURFACES ? drv.surfaces[surface].preallocated_gbuf.fd[0] : -1,
                surface < MAX_SURFACES ? drv.surfaces[surface].stride : -1,
                surface < MAX_SURFACES ? drv.surfaces[surface].raw_data_size : 0);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    status = hobot_vaDestroySurfaces(&va_ctx, &surface, 1);
    if (status != VA_STATUS_SUCCESS || drv.surfaces[surface].allocated ||
        mock_mem_free_calls != free_calls_before + 1 || mock_mem_free_fd != 200) {
        fprintf(stderr, "PRIME2 destroy did not release only its imported reference: status=%d free_fd=%d\n",
                status, mock_mem_free_fd);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    VADRMPRIMESurfaceDescriptor batch[2];
    setup_mock_prime2_nv12(&batch[0], STDERR_FILENO);
    setup_mock_prime2_nv12(&batch[1], STDERR_FILENO);
    batch[1].num_objects = 2;
    attrs[1].value.value.p = batch;
    free_calls_before = mock_mem_free_calls;
    VASurfaceID batch_surfaces[2] = {VA_INVALID_SURFACE, VA_INVALID_SURFACE};
    status = hobot_vaCreateSurfaces2(&va_ctx, VA_RT_FORMAT_YUV420,
                                     640, 360, batch_surfaces, 2, attrs, 2);
    if (status != VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE ||
        drv.surfaces[1].allocated || mock_mem_free_calls != free_calls_before + 1 ||
        mock_mem_free_fd != 201) {
        fprintf(stderr, "partial PRIME2 batch import was not rolled back: status=%d free_fd=%d\n",
                status, mock_mem_free_fd);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    setup_mock_prime2_nv12(&batch[0], STDERR_FILENO);
    setup_mock_prime2_nv12(&batch[1], STDERR_FILENO);
    batch[1].num_objects = 2;
    mock_mem_free_result = -1;
    status = hobot_vaCreateSurfaces2(&va_ctx, VA_RT_FORMAT_YUV420,
                                     640, 360, batch_surfaces, 2, attrs, 2);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED ||
        drv.orphaned_import_count != 1 || drv.orphaned_import_fds[0] != 202) {
        fprintf(stderr, "failed PRIME rollback did not retain its owned FD: status=%d count=%u fd=%d\n",
                status, drv.orphaned_import_count,
                drv.orphaned_import_count ? drv.orphaned_import_fds[0] : -1);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    setup_mock_prime2_nv12(&descriptor, STDERR_FILENO);
    attrs[1].value.value.p = &descriptor;
    mock_mem_import_bad_metadata = 1;
    status = hobot_vaCreateSurfaces2(&va_ctx, VA_RT_FORMAT_YUV420,
                                     640, 360, &surface, 1, attrs, 2);
    mock_mem_import_bad_metadata = 0;
    if (status != VA_STATUS_ERROR_OPERATION_FAILED ||
        drv.orphaned_import_count != 2 ||
        drv.orphaned_import_fds[0] != 202 ||
        drv.orphaned_import_fds[1] != 203) {
        fprintf(stderr,
                "invalid imported metadata failure lost its owned FD: status=%d count=%u fds=%d/%d\n",
                status, drv.orphaned_import_count,
                drv.orphaned_import_count > 0 ? drv.orphaned_import_fds[0] : -1,
                drv.orphaned_import_count > 1 ? drv.orphaned_import_fds[1] : -1);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    HobotDriverData *orphan_drv = calloc(1, sizeof(*orphan_drv));
    struct VADriverContext orphan_ctx = {0};
    if (!orphan_drv || pthread_mutex_init(&orphan_drv->mutex, NULL) != 0) {
        free(orphan_drv);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    orphan_drv->orphaned_import_count = drv.orphaned_import_count;
    memcpy(orphan_drv->orphaned_import_fds, drv.orphaned_import_fds,
           drv.orphaned_import_count * sizeof(drv.orphaned_import_fds[0]));
    drv.orphaned_import_count = 0;
    drv.orphaned_import_fds[0] = -1;
    drv.orphaned_import_fds[1] = -1;
    orphan_ctx.pDriverData = orphan_drv;
    mock_mem_module_close_result = 0;
    if (hobot_vaTerminate(&orphan_ctx) != VA_STATUS_ERROR_OPERATION_FAILED ||
        orphan_ctx.pDriverData != orphan_drv ||
        orphan_drv->orphaned_import_count != 2 ||
        orphan_drv->orphaned_import_fds[0] != 202 ||
        orphan_drv->orphaned_import_fds[1] != 203) {
        fprintf(stderr, "vaTerminate discarded an unreleased PRIME import FD\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    mock_mem_free_result = 0;
    int orphan_free_calls_before = mock_mem_free_calls;
    status = hobot_vaTerminate(&orphan_ctx);
    if (status != VA_STATUS_SUCCESS || orphan_ctx.pDriverData != NULL ||
        mock_mem_free_calls != orphan_free_calls_before + 2 ||
        mock_mem_free_fd != 203) {
        fprintf(stderr, "vaTerminate did not retry retained PRIME import FD: status=%d fd=%d\n",
                status, mock_mem_free_fd);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static int test_surface_creation_validation(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    drv.configs[1].allocated = 1;
    VASurfaceID surface = VA_INVALID_SURFACE;
    VAStatus status = hobot_vaCreateSurfaces2(&va_ctx, VA_RT_FORMAT_YUV420,
                                               4097, 720, &surface, 1, NULL, 0);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER || drv.surfaces[1].allocated) {
        fprintf(stderr, "oversized surface accepted: status=%d allocated=%d\n",
                status, drv.surfaces[1].allocated);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    status = hobot_vaCreateSurfaces2(&va_ctx, VA_RT_FORMAT_YUV420_10,
                                     1280, 720, &surface, 1, NULL, 0);
    if (status != VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT || drv.surfaces[1].allocated) {
        fprintf(stderr, "unsupported 10-bit surface format accepted: status=%d allocated=%d\n",
                status, drv.surfaces[1].allocated);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    status = hobot_vaCreateSurfaces2(&va_ctx, VA_RT_FORMAT_YUV420,
                                     1279, 720, &surface, 1, NULL, 0);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER || drv.surfaces[1].allocated) {
        fprintf(stderr, "odd-width NV12 surface accepted: status=%d allocated=%d\n",
                status, drv.surfaces[1].allocated);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    VASurfaceAttrib attr = {0};
    attr.type = VASurfaceAttribMemoryType;
    attr.value.type = VAGenericValueTypeInteger;
    attr.value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    status = hobot_vaCreateSurfaces2(&va_ctx, VA_RT_FORMAT_YUV420, 1280, 720,
                                     &surface, 1, &attr, 1);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER || drv.surfaces[1].allocated) {
        fprintf(stderr, "PRIME2 without external descriptor was not rejected: status=%d allocated=%d\n",
                status, drv.surfaces[1].allocated);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    attr.value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    status = hobot_vaCreateSurfaces2(&va_ctx, VA_RT_FORMAT_YUV420, 1280, 720,
                                     &surface, 1, &attr, 1);
    if (status != VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE || drv.surfaces[1].allocated) {
        fprintf(stderr, "legacy DRM PRIME memory type was not rejected: status=%d allocated=%d\n",
                status, drv.surfaces[1].allocated);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    attr.type = VASurfaceAttribPixelFormat;
    attr.value.type = (VAGenericValueType)0;
    attr.value.value.i = VA_FOURCC_NV12;
    drv.surfaces[1].context_id = 99;
    status = hobot_vaCreateSurfaces2(&va_ctx, VA_RT_FORMAT_YUV420, 1280, 720,
                                     &surface, 1, &attr, 1);
    if (status != VA_STATUS_SUCCESS || !drv.surfaces[surface].allocated ||
        drv.surfaces[surface].context_id != 0) {
        fprintf(stderr, "NV12 surface creation or slot reset failed: status=%d owner=%u\n",
                status, drv.surfaces[surface].context_id);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    status = hobot_vaDestroySurfaces(&va_ctx, &surface, 1);
    if (status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "surface destroy after zero-typed FOURCC failed: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    VASurfaceAttrib queried[7] = {0};
    unsigned int queried_count = 7;
    status = hobot_vaQuerySurfaceAttributes(&va_ctx, 1, queried, &queried_count);
    int reports_va_memory = 0;
    int reports_prime_import = 0;
    int reports_external_descriptor = 0;
    for (unsigned int i = 0; i < queried_count; i++) {
        if (queried[i].type == VASurfaceAttribMemoryType) {
            reports_va_memory =
                (queried[i].value.value.i & VA_SURFACE_ATTRIB_MEM_TYPE_VA) != 0;
            reports_prime_import =
                (queried[i].value.value.i & VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) != 0;
        } else if (queried[i].type == VASurfaceAttribExternalBufferDescriptor) {
            reports_external_descriptor =
                queried[i].flags == VA_SURFACE_ATTRIB_SETTABLE &&
                queried[i].value.type == VAGenericValueTypePointer;
        }
    }
    if (status != VA_STATUS_SUCCESS || !reports_va_memory || !reports_prime_import ||
        !reports_external_descriptor) {
        fprintf(stderr, "surface memory capabilities are inconsistent: status=%d count=%u va=%d prime=%d descriptor=%d\n",
                status, queried_count, reports_va_memory, reports_prime_import,
                reports_external_descriptor);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    status = hobot_vaCreateSurfaces2(&va_ctx, VA_RT_FORMAT_YUV420, 1280, 720,
                                     &surface, 1, NULL, 0);
    if (status != VA_STATUS_SUCCESS || surface <= 0 || surface >= MAX_SURFACES ||
        !drv.surfaces[surface].allocated || drv.surfaces[surface].stride != 1280 ||
        drv.surfaces[surface].raw_data_size != 1280u * 768u * 3u / 2u) {
        fprintf(stderr, "valid NV12 surface setup failed: status=%d id=%u allocated=%d stride=%d bytes=%u\n",
                status, surface, (surface < MAX_SURFACES) ? drv.surfaces[surface].allocated : 0,
                (surface < MAX_SURFACES) ? drv.surfaces[surface].stride : 0,
                (surface < MAX_SURFACES) ? drv.surfaces[surface].raw_data_size : 0);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    status = hobot_vaDestroySurfaces(&va_ctx, &surface, 1);
    if (status != VA_STATUS_SUCCESS || drv.surfaces[surface].allocated) {
        fprintf(stderr, "surface destroy after validation failed: status=%d allocated=%d\n",
                status, drv.surfaces[surface].allocated);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static int test_context_target_count_is_bounded(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    VASurfaceID only_one_target = 1;
    VAContextID context = VA_INVALID_ID;
    VAStatus status = hobot_vaCreateContext(
        &va_ctx, 1, 1280, 720, VA_PROGRESSIVE,
        &only_one_target, MAX_SURFACES, &context);
    int passed = status == VA_STATUS_ERROR_INVALID_PARAMETER &&
                 context == VA_INVALID_ID;
    if (!passed)
        fprintf(stderr, "oversized context render-target list was not rejected: status=%d context=%u\n",
                status, context);

    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_context_flags_are_validated(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    drv.configs[1].allocated = 1;
    drv.configs[1].profile = VAProfileH264High;
    drv.configs[1].entrypoint = VAEntrypointVLD;

    int saved_initialize_result = mock_initialize_result;
    int saved_initialize_calls = mock_initialize_calls;
    int saved_configure_result = mock_configure_result;
    int saved_configure_calls = mock_configure_calls;
    int saved_start_result = mock_start_result;
    int saved_start_calls = mock_start_calls;
    int saved_stop_result = mock_stop_result;
    int saved_stop_calls = mock_stop_calls;
    int saved_release_result = mock_release_result;
    int saved_release_calls = mock_release_calls;
    mock_initialize_result = 0;
    mock_configure_result = 0;
    mock_start_result = 0;
    mock_stop_result = 0;
    mock_release_result = 0;

    VAContextID context = VA_INVALID_ID;
    VAStatus status = hobot_vaCreateContext(
        &va_ctx, 1, 1280, 720, VA_PROGRESSIVE | 0x2, NULL, 0, &context);
    int passed = status == VA_STATUS_ERROR_INVALID_PARAMETER &&
                 context == VA_INVALID_ID && !drv.contexts[1].allocated &&
                 mock_initialize_calls == saved_initialize_calls &&
                 mock_configure_calls == saved_configure_calls &&
                 mock_start_calls == saved_start_calls;
    if (!passed)
        fprintf(stderr, "unsupported context flags were not rejected before VPU setup: status=%d context=%u init=%d configure=%d start=%d\n",
                status, context, mock_initialize_calls - saved_initialize_calls,
                mock_configure_calls - saved_configure_calls,
                mock_start_calls - saved_start_calls);
    if (context > 0 && context < MAX_CONTEXTS && drv.contexts[context].allocated) {
        if (hobot_vaDestroyContext(&va_ctx, context) != VA_STATUS_SUCCESS)
            passed = 0;
    }

    if (passed) {
        context = VA_INVALID_ID;
        status = hobot_vaCreateContext(&va_ctx, 1, 1280, 720, VA_PROGRESSIVE,
                                       NULL, 0, &context);
        passed = status == VA_STATUS_SUCCESS && context > 0 &&
                 context < MAX_CONTEXTS && drv.contexts[context].allocated &&
                 drv.contexts[context].vpu_ctx.video_dec_params.
                     h264_dec_config.bandwidth_Opt == 1;
        if (!passed)
            fprintf(stderr, "H.264 decoder context config was rejected: status=%d context=%u\n",
                    status, context);
        if (context > 0 && context < MAX_CONTEXTS && drv.contexts[context].allocated &&
            hobot_vaDestroyContext(&va_ctx, context) != VA_STATUS_SUCCESS) {
            fprintf(stderr, "context-flag test teardown failed\n");
            passed = 0;
        }
    }

    if (passed) {
        drv.configs[1].profile = VAProfileHEVCMain;
        context = VA_INVALID_ID;
        status = hobot_vaCreateContext(&va_ctx, 1, 1280, 720, VA_PROGRESSIVE,
                                       NULL, 0, &context);
        passed = status == VA_STATUS_SUCCESS && context > 0 &&
                 context < MAX_CONTEXTS && drv.contexts[context].allocated &&
                 drv.contexts[context].vpu_ctx.video_dec_params.
                     h265_dec_config.bandwidth_Opt == 1;
        if (!passed)
            fprintf(stderr, "HEVC decoder bandwidth optimization was not enabled: status=%d context=%u\n",
                    status, context);
        if (context > 0 && context < MAX_CONTEXTS && drv.contexts[context].allocated &&
            hobot_vaDestroyContext(&va_ctx, context) != VA_STATUS_SUCCESS) {
            fprintf(stderr, "HEVC context teardown failed\n");
            passed = 0;
        }
    }

    mock_initialize_result = saved_initialize_result;
    mock_initialize_calls = saved_initialize_calls;
    mock_configure_result = saved_configure_result;
    mock_configure_calls = saved_configure_calls;
    mock_start_result = saved_start_result;
    mock_start_calls = saved_start_calls;
    mock_stop_result = saved_stop_result;
    mock_stop_calls = saved_stop_calls;
    mock_release_result = saved_release_result;
    mock_release_calls = saved_release_calls;
    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_context_render_target_lifetime(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    drv.configs[1].allocated = 1;
    drv.configs[1].profile = VAProfileH264High;
    drv.configs[1].entrypoint = VAEntrypointVLD;
    for (VASurfaceID surface = 1; surface <= 2; surface++) {
        drv.surfaces[surface].allocated = 1;
        drv.surfaces[surface].width = 1280;
        drv.surfaces[surface].height = 720;
        drv.surfaces[surface].dma_fd = -1;
    }

    int saved_initialize_result = mock_initialize_result;
    int saved_configure_result = mock_configure_result;
    int saved_start_result = mock_start_result;
    int saved_stop_result = mock_stop_result;
    int saved_release_result = mock_release_result;
    mock_initialize_result = 0;
    mock_configure_result = 0;
    mock_start_result = 0;
    mock_stop_result = 0;
    mock_release_result = 0;

    VAContextID context = VA_INVALID_ID;
    VAStatus status = hobot_vaCreateContext(&va_ctx, 1, 1280, 720, 0,
                                            NULL, 0, &context);
    int passed = status == VA_STATUS_SUCCESS && context > 0 &&
                 hobot_vaBeginPicture(&va_ctx, context, 1) == VA_STATUS_SUCCESS;
    VAPictureParameterBufferH264 picture = {0};
    picture.CurrPic.picture_id = 1;
    for (size_t i = 0; i < sizeof(picture.ReferenceFrames) /
                            sizeof(picture.ReferenceFrames[0]); i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    picture.ReferenceFrames[0].picture_id = 2;
    picture.ReferenceFrames[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    picture.picture_width_in_mbs_minus1 = 79;
    picture.picture_height_in_mbs_minus1 = 44;
    picture.seq_fields.bits.frame_mbs_only_flag = 1;
    picture.seq_fields.bits.chroma_format_idc = 1;
    drv.buffers[1].allocated = 1;
    drv.buffers[1].id = 1;
    drv.buffers[1].type = VAPictureParameterBufferType;
    drv.buffers[1].data = &picture;
    drv.buffers[1].size = sizeof(picture);
    drv.buffers[1].element_size = sizeof(picture);
    drv.buffers[1].num_elements = 1;
    VABufferID picture_buffer = 1;
    if (passed) {
        picture.CurrPic.picture_id = 2;
        passed = hobot_vaRenderPicture(&va_ctx, context, &picture_buffer, 1) ==
                     VA_STATUS_ERROR_INVALID_SURFACE &&
                 drv.surfaces[1].context_usage_mask ==
                     hobot_context_usage_bit(context) &&
                 drv.surfaces[2].context_usage_mask == 0 &&
                 !drv.contexts[context].h264_decode_picture_valid;
    }
    picture.CurrPic.picture_id = 1;
    if (passed) {
        picture.ReferenceFrames[0].picture_id = 0;
        passed = hobot_vaRenderPicture(&va_ctx, context, &picture_buffer, 1) ==
                     VA_STATUS_ERROR_INVALID_SURFACE &&
                 drv.surfaces[2].context_usage_mask == 0 &&
                 !drv.contexts[context].h264_decode_picture_valid;
    }
    if (passed) {
        picture.ReferenceFrames[0].picture_id = 3;
        passed = hobot_vaRenderPicture(&va_ctx, context, &picture_buffer, 1) ==
                     VA_STATUS_ERROR_INVALID_SURFACE &&
                 drv.surfaces[2].context_usage_mask == 0 &&
                 !drv.contexts[context].h264_decode_picture_valid;
    }
    if (passed) {
        picture.ReferenceFrames[0].picture_id = MAX_SURFACES;
        passed = hobot_vaRenderPicture(&va_ctx, context, &picture_buffer, 1) ==
                     VA_STATUS_ERROR_INVALID_SURFACE &&
                 drv.surfaces[2].context_usage_mask == 0 &&
                 !drv.contexts[context].h264_decode_picture_valid;
    }
    picture.ReferenceFrames[0].picture_id = 2;
    if (passed)
        passed = hobot_vaRenderPicture(&va_ctx, context, &picture_buffer, 1) ==
                     VA_STATUS_SUCCESS &&
                 drv.surfaces[2].context_usage_mask ==
                     hobot_context_usage_bit(context) &&
                 hobot_vaDestroySurfaces(&va_ctx, (VASurfaceID[]){2}, 1) ==
                     VA_STATUS_ERROR_SURFACE_BUSY;
    VAContextID peer_context = VA_INVALID_ID;
    VASurfaceID peer_target = 1;
    if (passed) {
        status = hobot_vaCreateContext(&va_ctx, 1, 1280, 720, 0,
                                       &peer_target, 1, &peer_context);
        passed = status == VA_STATUS_SUCCESS && peer_context > 0;
    }
    if (passed) {
        passed = drv.surfaces[1].context_usage_mask ==
                     (hobot_context_usage_bit(context) |
                      hobot_context_usage_bit(peer_context)) &&
                 hobot_vaDestroySurfaces(&va_ctx, (VASurfaceID[]){1}, 1) ==
                     VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (peer_context > 0 && drv.contexts[peer_context].allocated) {
        VAStatus peer_destroy = hobot_vaDestroyContext(&va_ctx, peer_context);
        passed = passed && peer_destroy == VA_STATUS_SUCCESS &&
                 drv.surfaces[1].context_usage_mask ==
                     hobot_context_usage_bit(context) &&
                 drv.surfaces[2].context_usage_mask ==
                     hobot_context_usage_bit(context) &&
                 hobot_vaDestroySurfaces(&va_ctx, (VASurfaceID[]){1}, 1) ==
                     VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (context > 0 && drv.contexts[context].allocated) {
        if (drv.contexts[context].decode_picture_active) {
            pthread_mutex_lock(&drv.mutex);
            hobot_abort_pending_decode_picture(&drv, &drv.contexts[context]);
            pthread_mutex_unlock(&drv.mutex);
        }
        passed = hobot_vaDestroyContext(&va_ctx, context) == VA_STATUS_SUCCESS &&
                 drv.surfaces[1].context_usage_mask == 0 &&
                 drv.surfaces[2].context_usage_mask == 0 && passed;
    }
    passed = passed && hobot_vaDestroySurfaces(&va_ctx, (VASurfaceID[]){1}, 1) ==
                          VA_STATUS_SUCCESS &&
             hobot_vaDestroySurfaces(&va_ctx, (VASurfaceID[]){2}, 1) ==
                          VA_STATUS_SUCCESS;
    if (!passed)
        fprintf(stderr, "context surface references did not follow context lifetime: create=%d context=%u peer=%u mask=%u\n",
                status, context, peer_context, drv.surfaces[1].context_usage_mask);

    mock_initialize_result = saved_initialize_result;
    mock_configure_result = saved_configure_result;
    mock_start_result = saved_start_result;
    mock_stop_result = saved_stop_result;
    mock_release_result = saved_release_result;
    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_encoder_picture_reference_lifetime(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    unsigned char coded_data[2] = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    for (VASurfaceID surface = 1; surface <= 4; surface++) {
        drv.surfaces[surface].allocated = 1;
        drv.surfaces[surface].width = 640;
        drv.surfaces[surface].height = 360;
        drv.surfaces[surface].dma_fd = -1;
    }

    HobotContext *h264_context = &drv.contexts[1];
    h264_context->allocated = 1;
    h264_context->is_encoder = 1;
    h264_context->profile = VAProfileH264Main;
    h264_context->width = 640;
    h264_context->height = 360;
    h264_context->encoder_init_deferred = 1;
    h264_context->encoder_picture_active = 1;
    HobotContext *hevc_context = &drv.contexts[2];
    hevc_context->allocated = 1;
    hevc_context->is_encoder = 1;
    hevc_context->profile = VAProfileHEVCMain;
    hevc_context->width = 640;
    hevc_context->height = 360;
    hevc_context->encoder_init_deferred = 1;
    hevc_context->encoder_picture_active = 1;

    VAEncPictureParameterBufferH264 h264_picture = {0};
    h264_picture.CurrPic.picture_id = 1;
    for (size_t i = 0; i < sizeof(h264_picture.ReferenceFrames) /
                            sizeof(h264_picture.ReferenceFrames[0]); i++) {
        h264_picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        h264_picture.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    h264_picture.ReferenceFrames[0].picture_id = 2;
    h264_picture.ReferenceFrames[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    h264_picture.coded_buf = 5;
    drv.buffers[1] = (HobotBuffer){
        .id = 1,
        .allocated = 1,
        .type = VAEncPictureParameterBufferType,
        .size = sizeof(h264_picture),
        .capacity = sizeof(h264_picture),
        .element_size = sizeof(h264_picture),
        .num_elements = 1,
        .data = &h264_picture
    };

    VAEncSequenceParameterBufferHEVC hevc_sequence = {0};
    hevc_sequence.general_profile_idc = 1;
    hevc_sequence.general_level_idc = MC_H265_LEVEL4_1;
    hevc_sequence.intra_period = 30;
    hevc_sequence.intra_idr_period = 30;
    hevc_sequence.ip_period = 1;
    hevc_sequence.bits_per_second = 4000000;
    hevc_sequence.pic_width_in_luma_samples = 640;
    hevc_sequence.pic_height_in_luma_samples = 360;
    hevc_sequence.seq_fields.bits.chroma_format_idc = 1;
    hevc_sequence.log2_diff_max_min_luma_coding_block_size = 3;
    hevc_sequence.log2_diff_max_min_transform_block_size = 3;

    VAEncPictureParameterBufferHEVC hevc_picture = {0};
    hevc_picture.decoded_curr_pic.picture_id = 3;
    for (size_t i = 0; i < sizeof(hevc_picture.reference_frames) /
                            sizeof(hevc_picture.reference_frames[0]); i++) {
        hevc_picture.reference_frames[i].picture_id = VA_INVALID_SURFACE;
        hevc_picture.reference_frames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    hevc_picture.reference_frames[0].picture_id = 4;
    hevc_picture.reference_frames[0].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    hevc_picture.coded_buf = 6;
    hevc_picture.pic_init_qp = 26;
    hevc_picture.pic_fields.bits.coding_type = 1;
    drv.buffers[2] = (HobotBuffer){
        .id = 2,
        .allocated = 1,
        .type = VAEncSequenceParameterBufferType,
        .size = sizeof(hevc_sequence),
        .capacity = sizeof(hevc_sequence),
        .element_size = sizeof(hevc_sequence),
        .num_elements = 1,
        .data = &hevc_sequence
    };
    drv.buffers[3] = (HobotBuffer){
        .id = 3,
        .allocated = 1,
        .type = VAEncPictureParameterBufferType,
        .size = sizeof(hevc_picture),
        .capacity = sizeof(hevc_picture),
        .element_size = sizeof(hevc_picture),
        .num_elements = 1,
        .data = &hevc_picture
    };
    VAEncSliceParameterBufferHEVC hevc_slice = {0};
    hevc_slice.num_ctu_in_slice = 60;
    hevc_slice.slice_type = 2;
    hevc_slice.max_num_merge_cand = 5;
    hevc_slice.slice_fields.bits.last_slice_of_pic_flag = 1;
    drv.buffers[4] = (HobotBuffer){
        .id = 4,
        .allocated = 1,
        .type = VAEncSliceParameterBufferType,
        .size = sizeof(hevc_slice),
        .capacity = sizeof(hevc_slice),
        .element_size = sizeof(hevc_slice),
        .num_elements = 1,
        .data = &hevc_slice
    };
    for (VABufferID coded_id = 5; coded_id <= 6; coded_id++) {
        drv.buffers[coded_id] = (HobotBuffer){
            .id = coded_id,
            .allocated = 1,
            .type = VAEncCodedBufferType,
            .size = 1,
            .capacity = 1,
            .element_size = 1,
            .num_elements = 1,
            .data = &coded_data[coded_id - 5]
        };
    }

    VABufferID h264_buffer = 1;
    VABufferID hevc_buffers[] = {2, 3, 4};
    h264_picture.CurrPic.picture_id = 5;
    VAStatus invalid_h264_current_status = hobot_vaRenderPicture(
        &va_ctx, 1, &h264_buffer, 1);
    h264_picture.CurrPic.picture_id = 1;
    hevc_picture.decoded_curr_pic.picture_id = 5;
    VAStatus invalid_hevc_current_status = hobot_vaRenderPicture(
        &va_ctx, 2, hevc_buffers, 3);
    hevc_picture.decoded_curr_pic.picture_id = 3;
    h264_picture.ReferenceFrames[0].picture_id = 5;
    VAStatus invalid_h264_status = hobot_vaRenderPicture(
        &va_ctx, 1, &h264_buffer, 1);
    h264_picture.ReferenceFrames[0].picture_id = 2;
    hevc_picture.reference_frames[0].picture_id = 5;
    VAStatus invalid_hevc_status = hobot_vaRenderPicture(
        &va_ctx, 2, hevc_buffers, 3);
    int invalid_preserved_state =
        drv.surfaces[2].context_usage_mask == 0 &&
        drv.surfaces[4].context_usage_mask == 0;
    hevc_picture.reference_frames[0].picture_id = 4;
    VAStatus h264_status = hobot_vaRenderPicture(&va_ctx, 1, &h264_buffer, 1);
    VAStatus hevc_status = hobot_vaRenderPicture(&va_ctx, 2, hevc_buffers, 3);
    int passed = invalid_h264_status == VA_STATUS_ERROR_INVALID_SURFACE &&
        invalid_hevc_status == VA_STATUS_ERROR_INVALID_SURFACE &&
        invalid_h264_current_status == VA_STATUS_ERROR_INVALID_SURFACE &&
        invalid_hevc_current_status == VA_STATUS_ERROR_INVALID_SURFACE &&
        invalid_preserved_state &&
        h264_status == VA_STATUS_SUCCESS &&
        hevc_status == VA_STATUS_SUCCESS &&
        drv.surfaces[1].context_usage_mask == hobot_context_usage_bit(1) &&
        drv.surfaces[2].context_usage_mask == hobot_context_usage_bit(1) &&
        drv.surfaces[3].context_usage_mask == hobot_context_usage_bit(2) &&
        drv.surfaces[4].context_usage_mask == hobot_context_usage_bit(2) &&
        hobot_vaDestroySurfaces(&va_ctx, (VASurfaceID[]){2}, 1) ==
            VA_STATUS_ERROR_SURFACE_BUSY &&
        hobot_vaDestroySurfaces(&va_ctx, (VASurfaceID[]){4}, 1) ==
            VA_STATUS_ERROR_SURFACE_BUSY;

    VAStatus h264_destroy = hobot_vaDestroyContext(&va_ctx, 1);
    VAStatus hevc_destroy = hobot_vaDestroyContext(&va_ctx, 2);
    passed = passed && h264_destroy == VA_STATUS_SUCCESS &&
        hevc_destroy == VA_STATUS_SUCCESS;
    for (VASurfaceID surface = 1; surface <= 4; surface++)
        passed = passed && drv.surfaces[surface].context_usage_mask == 0;
    passed = passed && hobot_vaDestroySurfaces(
        &va_ctx, (VASurfaceID[]){1, 2, 3, 4}, 4) == VA_STATUS_SUCCESS;

    if (!passed)
        fprintf(stderr, "encoder picture surfaces did not follow H.264/HEVC context lifetime: invalid=%d/%d current=%d/%d render=%d/%d destroy=%d/%d masks=%u/%u/%u/%u\n",
                invalid_h264_status, invalid_hevc_status,
                invalid_h264_current_status, invalid_hevc_current_status,
                h264_status, hevc_status, h264_destroy, hevc_destroy,
                drv.surfaces[1].context_usage_mask,
                drv.surfaces[2].context_usage_mask,
                drv.surfaces[3].context_usage_mask,
                drv.surfaces[4].context_usage_mask);
    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_hevc_decoder_rejects_unallocated_reference_surface(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    VAPictureParameterBufferHEVC picture = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->decode_picture_active = 1;
    hctx->profile = VAProfileHEVCMain;
    hctx->width = 64;
    hctx->height = 64;
    hctx->current_render_target = 1;
    drv.surfaces[1].allocated = 1;
    picture.CurrPic.picture_id = 1;
    for (size_t i = 0; i < sizeof(picture.ReferenceFrames) /
                            sizeof(picture.ReferenceFrames[0]); i++) {
        picture.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        picture.ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
    }
    picture.ReferenceFrames[0].picture_id = 2;
    picture.ReferenceFrames[0].flags = 0;
    drv.buffers[1] = (HobotBuffer){
        .id = 1,
        .allocated = 1,
        .type = VAPictureParameterBufferType,
        .size = sizeof(picture),
        .capacity = sizeof(picture),
        .element_size = sizeof(picture),
        .num_elements = 1,
        .data = &picture
    };

    VABufferID picture_id = 1;
    picture.CurrPic.picture_id = 2;
    VAStatus invalid_current_status = hobot_vaRenderPicture(
        &va_ctx, 1, &picture_id, 1);
    picture.CurrPic.picture_id = 1;
    VAStatus status = hobot_vaRenderPicture(&va_ctx, 1, &picture_id, 1);
    int passed = invalid_current_status == VA_STATUS_ERROR_INVALID_SURFACE &&
                 status == VA_STATUS_ERROR_INVALID_SURFACE &&
                 !hctx->hevc_decode_picture_valid &&
                 drv.surfaces[2].context_usage_mask == 0;
    if (!passed)
        fprintf(stderr, "HEVC decoder accepted an invalid current/reference surface: current=%d reference=%d valid=%d mask=%u\n",
                invalid_current_status, status, hctx->hevc_decode_picture_valid,
                drv.surfaces[2].context_usage_mask);

    picture.pic_width_in_luma_samples = 64;
    picture.pic_height_in_luma_samples = 64;
    picture.pic_fields.bits.chroma_format_idc = 1;
    picture.log2_diff_max_min_luma_coding_block_size = 1;
    picture.ReferenceFrames[0].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
    drv.surfaces[2].allocated = 1;
    status = hobot_vaRenderPicture(&va_ctx, 1, &picture_id, 1);
    int retained = status == VA_STATUS_SUCCESS &&
        hctx->hevc_decode_picture_valid &&
        drv.surfaces[1].context_usage_mask == hobot_context_usage_bit(1) &&
        drv.surfaces[2].context_usage_mask == hobot_context_usage_bit(1);
    VAStatus destroy_reference = hobot_vaDestroySurfaces(
        &va_ctx, (VASurfaceID[]){2}, 1);
    passed = passed && retained &&
             destroy_reference == VA_STATUS_ERROR_SURFACE_BUSY;
    if (!retained || destroy_reference != VA_STATUS_ERROR_SURFACE_BUSY)
        fprintf(stderr, "HEVC decoder picture did not retain reference surface: status=%d valid=%d current_mask=%u ref_mask=%u destroy=%d\n",
                status, hctx->hevc_decode_picture_valid,
                drv.surfaces[1].context_usage_mask,
                drv.surfaces[2].context_usage_mask, destroy_reference);

    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_api_array_counts_are_bounded(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    VABufferID only_one_buffer = 1;
    VASurfaceID only_one_surface = 1;
    VAStatus render_status = hobot_vaRenderPicture(
        &va_ctx, 1, &only_one_buffer, MAX_BUFFERS);
    VAStatus destroy_status = hobot_vaDestroySurfaces(
        &va_ctx, &only_one_surface, MAX_SURFACES);
    VASurfaceAttrib only_one_attribute = {
        .type = VASurfaceAttribPixelFormat,
        .value = {
            .type = VAGenericValueTypeInteger,
            .value.i = VA_FOURCC_NV12
        }
    };
    VASurfaceID created_surface = VA_INVALID_SURFACE;
    VAStatus surface_attribute_status = hobot_vaCreateSurfaces2(
        &va_ctx, VA_RT_FORMAT_YUV420, 1280, 720, &created_surface, 1,
        &only_one_attribute, UINT_MAX);
    int passed = render_status == VA_STATUS_ERROR_INVALID_PARAMETER &&
                 destroy_status == VA_STATUS_ERROR_INVALID_PARAMETER &&
                 surface_attribute_status == VA_STATUS_ERROR_INVALID_PARAMETER &&
                 created_surface == VA_INVALID_SURFACE &&
                 !drv.surfaces[1].allocated;
    if (!passed)
        fprintf(stderr, "oversized VA-API arrays were not rejected: render=%d destroy=%d surface_attrs=%d surface=%u\n",
                render_status, destroy_status, surface_attribute_status,
                created_surface);

    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_begin_picture_rejects_undersized_surface(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->width = 1280;
    hctx->height = 720;
    hctx->current_render_target = VA_INVALID_SURFACE;
    HobotSurface *surf = &drv.surfaces[1];
    surf->allocated = 1;
    surf->width = 640;
    surf->height = 360;

    VAStatus status = hobot_vaBeginPicture(&va_ctx, 1, 1);
    int passed = status == VA_STATUS_ERROR_INVALID_SURFACE &&
                 !surf->decode_pending && surf->context_id == 0 &&
                 !hctx->decode_picture_active && hctx->sub_tail == 0 &&
                 hctx->current_render_target == VA_INVALID_SURFACE;
    if (!passed) {
        fprintf(stderr, "undersized decode target changed context state: status=%d pending=%d context=%u active=%d tail=%u\n",
                status, surf->decode_pending, surf->context_id,
                hctx->decode_picture_active, hctx->sub_tail);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    surf->width = 1280;
    surf->height = 720;
    status = hobot_vaBeginPicture(&va_ctx, 1, 1);
    if (status != VA_STATUS_SUCCESS || !surf->decode_pending ||
        !hctx->decode_picture_active) {
        fprintf(stderr, "context-sized decode target was rejected: status=%d pending=%d active=%d\n",
                status, surf->decode_pending, hctx->decode_picture_active);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    surf->decode_pending = 0;
    surf->context_id = 0;
    hctx->decode_picture_active = 0;
    hctx->current_render_target = VA_INVALID_SURFACE;
    hctx->sub_head = 0;
    hctx->sub_tail = 0;

    surf->width = 640;
    surf->height = 360;

    hctx->is_encoder = 1;
    status = hobot_vaBeginPicture(&va_ctx, 1, 1);
    passed = status == VA_STATUS_ERROR_INVALID_SURFACE &&
             !hctx->encoder_picture_active && surf->context_id == 0;
    if (!passed)
        fprintf(stderr, "undersized encode target was accepted: status=%d active=%d context=%u\n",
                status, hctx->encoder_picture_active, surf->context_id);

    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_query_surface_status_tracks_pending_work(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    drv.surfaces[1].allocated = 1;

    VASurfaceStatus status_value = VASurfaceRendering;
    VAStatus status = hobot_vaQuerySurfaceStatus(&va_ctx, 1, &status_value);
    if (status != VA_STATUS_SUCCESS || status_value != VASurfaceReady) {
        fprintf(stderr, "idle surface did not report ready: status=%d surface_status=%d\n",
                status, status_value);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    drv.surfaces[1].decode_pending = 1;
    status = hobot_vaQuerySurfaceStatus(&va_ctx, 1, &status_value);
    if (status != VA_STATUS_SUCCESS || status_value != VASurfaceRendering) {
        fprintf(stderr, "pending surface did not report rendering: status=%d surface_status=%d\n",
                status, status_value);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    drv.surfaces[1].decode_pending = 0;
    HobotContext *encoder = &drv.contexts[1];
    atomic_init(&encoder->enc_external_input_pending, 0);
    encoder->allocated = 1;
    encoder->is_encoder = 1;
    encoder->encoder_picture_active = 1;
    encoder->current_render_target = 1;
    status = hobot_vaQuerySurfaceStatus(&va_ctx, 1, &status_value);
    if (status != VA_STATUS_SUCCESS || status_value != VASurfaceRendering) {
        fprintf(stderr, "surface used by an active encoder did not report rendering: status=%d surface_status=%d\n",
                status, status_value);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    encoder->encoder_picture_active = 0;
    encoder->enc_external_enabled = 1;
    encoder->enc_external_surface = 1;
    atomic_store_explicit(&encoder->enc_external_input_pending, 7,
                          memory_order_release);
    status = hobot_vaQuerySurfaceStatus(&va_ctx, 1, &status_value);
    if (status != VA_STATUS_SUCCESS || status_value != VASurfaceRendering) {
        fprintf(stderr, "surface awaiting external encoder consumption did not report rendering: status=%d surface_status=%d\n",
                status, status_value);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    atomic_store_explicit(&encoder->enc_external_input_pending, 0,
                          memory_order_release);
    encoder->enc_external_surface = VA_INVALID_SURFACE;
    drv.surfaces[1].has_decoded_frame = 1;
    status = hobot_vaQuerySurfaceStatus(&va_ctx, 1, &status_value);
    if (status != VA_STATUS_SUCCESS || status_value != VASurfaceReady ||
        hobot_vaQuerySurfaceStatus(&va_ctx, 2, &status_value) !=
            VA_STATUS_ERROR_INVALID_SURFACE || status_value != VASurfaceReady ||
        hobot_vaQuerySurfaceStatus(NULL, 1, &status_value) !=
            VA_STATUS_ERROR_INVALID_CONTEXT) {
        fprintf(stderr, "surface status validation failed: status=%d surface_status=%d\n",
                status, status_value);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static int test_query_surface_error_is_explicitly_unimplemented(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    drv.surfaces[1].allocated = 1;

    void *error_info = (void *)(uintptr_t)1;
    VAStatus status = hobot_vaQuerySurfaceError(
        &va_ctx, 1, VA_STATUS_ERROR_DECODING_ERROR, &error_info);
    int passed = status == VA_STATUS_ERROR_UNIMPLEMENTED && error_info == NULL &&
                 hobot_vaQuerySurfaceError(&va_ctx, 2,
                     VA_STATUS_ERROR_DECODING_ERROR, &error_info) ==
                     VA_STATUS_ERROR_INVALID_SURFACE &&
                 hobot_vaQuerySurfaceError(&va_ctx, 1,
                     VA_STATUS_ERROR_DECODING_ERROR, NULL) ==
                     VA_STATUS_ERROR_INVALID_PARAMETER &&
                 hobot_vaQuerySurfaceError(&va_ctx, 1, VA_STATUS_SUCCESS,
                     &error_info) == VA_STATUS_ERROR_INVALID_PARAMETER &&
                 hobot_vaQuerySurfaceError(NULL, 1,
                     VA_STATUS_ERROR_DECODING_ERROR, &error_info) ==
                     VA_STATUS_ERROR_INVALID_CONTEXT;
    if (!passed)
        fprintf(stderr, "vaQuerySurfaceError did not report its unsupported contract: status=%d info=%p\n",
                status, error_info);
    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_derived_image_maps_surface_storage(void)
{
    enum { WIDTH = 130, HEIGHT = 66, STRIDE = 160, VSTRIDE = 80,
           Y_SIZE = STRIDE * VSTRIDE, UV_SIZE = STRIDE * (VSTRIDE / 2),
           FRAME_SIZE = Y_SIZE + UV_SIZE, IMAGE_SIZE = Y_SIZE + STRIDE * (HEIGHT / 2),
           PEER_STRIDE = 192, PEER_VSTRIDE = 128,
           PEER_SIZE = PEER_STRIDE * PEER_VSTRIDE * 3 / 2 };
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    uint8_t frame[FRAME_SIZE];
    uint8_t peer_data[PEER_SIZE];
    for (size_t i = 0; i < sizeof(frame); i++)
        frame[i] = (uint8_t)(i * 17u + 3u);
    for (size_t i = 0; i < sizeof(peer_data); i++)
        peer_data[i] = (uint8_t)(i * 29u + 11u);
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    VASurfaceID surface = VA_INVALID_SURFACE;
    VAStatus status = hobot_vaCreateSurfaces2(&va_ctx, VA_RT_FORMAT_YUV420,
                                               WIDTH, HEIGHT, &surface, 1, NULL, 0);
    if (status != VA_STATUS_SUCCESS) {
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    HobotSurface *surf = &drv.surfaces[surface];
    surf->has_decoded_frame = 1;
    surf->vpu_out_buf.vframe_buf.vir_ptr[0] = frame;
    surf->vpu_out_buf.vframe_buf.vir_ptr[1] = frame + Y_SIZE;
    surf->vpu_out_buf.vframe_buf.stride = STRIDE;
    surf->vpu_out_buf.vframe_buf.vstride = STRIDE;
    surf->vpu_out_buf.vframe_buf.size = sizeof(frame);
    surf->vpu_out_buf.vframe_buf.fd[0] = 7;
    surf->vpu_out_buf.vframe_buf.fd[1] = 7;
    surf->vpu_out_buf.vframe_buf.compSize[0] = Y_SIZE;
    surf->vpu_out_buf.vframe_buf.compSize[1] = UV_SIZE;

    VAImage first = {0};
    surf->vpu_out_buf.vframe_buf.stride = 64;
    status = hobot_vaDeriveImage(&va_ctx, surface, &first);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || drv.images[1].allocated ||
        drv.buffers[1].allocated || surf->lock_count != 0) {
        fprintf(stderr, "vaDeriveImage accepted an invalid backing stride: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    surf->vpu_out_buf.vframe_buf.stride = STRIDE;

    mock_mem_invalidate_calls = 0;
    mock_mem_flush_calls = 0;
    mock_mem_invalidate_result = -1;
    status = hobot_vaDeriveImage(&va_ctx, surface, &first);
    unsigned int derived_size = drv.buffers[first.buf].size;
    int passed = status == VA_STATUS_SUCCESS && first.pitches[0] == STRIDE &&
                 first.pitches[1] == STRIDE && first.offsets[0] == 0 &&
                 first.offsets[1] == Y_SIZE && first.data_size == IMAGE_SIZE &&
                 drv.buffers[first.buf].data == frame && surf->raw_data == NULL &&
                 surf->lock_count == 1 &&
                 hobot_vaBufferSetNumElements(&va_ctx, first.buf, 1) ==
                     VA_STATUS_ERROR_OPERATION_FAILED &&
                 drv.buffers[first.buf].size == derived_size;
    void *mapped_first = NULL;
    if (passed) {
        status = hobot_vaMapBuffer(&va_ctx, first.buf, &mapped_first);
        passed = status == VA_STATUS_ERROR_OPERATION_FAILED && mapped_first == NULL &&
                 drv.buffers[first.buf].map_count == 0 && surf->cpu_access_count == 0;
    }
    mock_mem_invalidate_result = 0;

    VAImage second = {0};
    if (passed) {
        status = hobot_vaDeriveImage(&va_ctx, surface, &second);
        passed = status == VA_STATUS_SUCCESS && second.buf != first.buf &&
                 drv.buffers[second.buf].data == frame && surf->lock_count == 2;
    }
    if (passed) {
        status = hobot_vaUnlockSurface(&va_ctx, surface);
        passed = status == VA_STATUS_ERROR_OPERATION_FAILED && surf->lock_count == 2 &&
                 surf->va_lock_count == 0 && surf->cpu_access_count == 0;
    }
    VASurfaceID peer_surface = VA_INVALID_SURFACE;
    if (passed) {
        status = hobot_vaCreateSurfaces2(&va_ctx, VA_RT_FORMAT_YUV420,
                                         WIDTH, HEIGHT, &peer_surface, 1, NULL, 0);
        passed = status == VA_STATUS_SUCCESS;
    }
    if (passed) {
        HobotSurface *peer = &drv.surfaces[peer_surface];
        peer->stride = PEER_STRIDE;
        peer->raw_data_size = sizeof(peer_data);
        peer->raw_data = peer_data;
        peer->raw_data_valid = 1;
    }
    void *mapped_again = NULL;
    void *mapped_second = NULL;
    if (passed) {
        passed = hobot_vaMapBuffer(&va_ctx, first.buf, &mapped_first) == VA_STATUS_SUCCESS &&
                 mapped_first == frame &&
                 hobot_vaMapBuffer(&va_ctx, first.buf, &mapped_again) == VA_STATUS_SUCCESS &&
                 mapped_again == frame &&
                 hobot_vaMapBuffer(&va_ctx, second.buf, &mapped_second) == VA_STATUS_SUCCESS &&
                 mapped_second == frame && surf->cpu_access_count == 2 &&
                 drv.buffers[first.buf].map_count == 2 &&
                 drv.buffers[second.buf].map_count == 1 &&
                 mock_mem_invalidate_calls == 2 && mock_mem_invalidate_fd == 7 &&
                 mock_mem_invalidate_offset == 0 && mock_mem_invalidate_size == FRAME_SIZE;
    }
    if (passed) {
        VADRMPRIMESurfaceDescriptor prime_desc = {0};
        struct hobot_surface_info graph_info = {0};
        passed = hobot_vaExportSurfaceHandle(&va_ctx, surface,
                                              VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                              VA_EXPORT_SURFACE_READ_WRITE,
                                              &prime_desc) == VA_STATUS_ERROR_SURFACE_BUSY &&
                 hobot_vaExportSurfaceHandle(&va_ctx, surface,
                                              VA_SURFACE_ATTRIB_MEM_TYPE_HOBOT_GRAPH_BUF,
                                              0, &graph_info) == VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (passed) {
        void *surface_map = NULL;
        passed = hobot_vaLockSurface(&va_ctx, surface, NULL, NULL, NULL, NULL,
                                     NULL, NULL, NULL, NULL, &surface_map) ==
                     VA_STATUS_SUCCESS && surface_map == frame &&
                 surf->cpu_access_count == 3 && surf->va_lock_count == 1 &&
                 mock_mem_invalidate_calls == 2 && mock_mem_flush_calls == 0 &&
                 hobot_vaUnlockSurface(&va_ctx, surface) == VA_STATUS_SUCCESS &&
                 surf->cpu_access_count == 2 && surf->va_lock_count == 0 &&
                 mock_mem_flush_calls == 0;
    }
    if (passed) {
        passed = hobot_vaGetImage(&va_ctx, peer_surface, 0, 0, WIDTH, HEIGHT,
                                  first.image_id) == VA_STATUS_ERROR_SURFACE_BUSY &&
                 hobot_vaPutImage(&va_ctx, peer_surface, first.image_id, 0, 0,
                                  WIDTH, HEIGHT, 0, 0, WIDTH, HEIGHT) ==
                     VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (passed) {
        surf->raw_data_valid = 1;
        ((uint8_t *)mapped_first)[0] = 0xa5;
        passed = hobot_vaUnmapBuffer(&va_ctx, first.buf) == VA_STATUS_SUCCESS &&
                 surf->cpu_access_count == 2 && mock_mem_flush_calls == 0 &&
                 hobot_vaUnmapBuffer(&va_ctx, second.buf) == VA_STATUS_SUCCESS &&
                 surf->cpu_access_count == 1 && mock_mem_flush_calls == 0;
    }
    mock_mem_flush_result = -1;
    if (passed) {
        status = hobot_vaUnmapBuffer(&va_ctx, first.buf);
        passed = status == VA_STATUS_ERROR_OPERATION_FAILED &&
                 surf->cpu_access_count == 0 && drv.buffers[first.buf].map_count == 0 &&
                 !surf->raw_data_valid &&
                 mock_mem_flush_calls == 1 && mock_mem_flush_fd == 7 &&
                 mock_mem_flush_offset == 0 && mock_mem_flush_size == FRAME_SIZE;
    }
    if (passed) {
        status = hobot_vaDestroyImage(&va_ctx, first.image_id);
        passed = status == VA_STATUS_ERROR_OPERATION_FAILED &&
                 drv.images[first.image_id].allocated &&
                 surf->cpu_cache_flush_pending && surf->lock_count == 2 &&
                 mock_mem_flush_calls == 2;
    }
    mock_mem_flush_result = 0;
    if (passed) {
        passed = hobot_vaMapBuffer(&va_ctx, second.buf, &mapped_second) == VA_STATUS_SUCCESS &&
                 !surf->cpu_cache_flush_pending && surf->cpu_access_count == 1 &&
                 hobot_vaUnmapBuffer(&va_ctx, second.buf) == VA_STATUS_SUCCESS &&
                 surf->cpu_access_count == 0 &&
                 mock_mem_invalidate_calls == 3 && mock_mem_flush_calls == 4;
    }
    if (passed) {
        int invalidates_before = mock_mem_invalidate_calls;
        int flushes_before = mock_mem_flush_calls;
        status = hobot_vaGetImage(&va_ctx, peer_surface, 0, 0, WIDTH, HEIGHT,
                                  first.image_id);
        passed = status == VA_STATUS_SUCCESS &&
                 mock_mem_invalidate_calls == invalidates_before + 1 &&
                 mock_mem_flush_calls == flushes_before + 1 &&
                 mock_mem_flush_fd == 7 && mock_mem_flush_size == FRAME_SIZE &&
                 memcmp(frame, peer_data, WIDTH) == 0 &&
                 memcmp(frame + Y_SIZE, peer_data + PEER_STRIDE * HEIGHT, WIDTH) == 0;
        invalidates_before = mock_mem_invalidate_calls;
        if (passed) {
            status = hobot_vaPutImage(&va_ctx, peer_surface, first.image_id, 0, 0,
                                      WIDTH, HEIGHT, 0, 0, WIDTH, HEIGHT);
            passed = status == VA_STATUS_SUCCESS &&
                     mock_mem_invalidate_calls == invalidates_before + 1 &&
                     memcmp(peer_data, frame, WIDTH) == 0 &&
                     memcmp(peer_data + PEER_STRIDE * HEIGHT,
                            frame + Y_SIZE, WIDTH) == 0;
        }
    }
    if (passed) {
        passed = hobot_vaGetImage(&va_ctx, surface, 0, 0, WIDTH, HEIGHT,
                                  first.image_id) == VA_STATUS_ERROR_SURFACE_BUSY &&
                 hobot_vaPutImage(&va_ctx, surface, first.image_id, 0, 0,
                                  WIDTH, HEIGHT, 0, 0, WIDTH, HEIGHT) ==
                     VA_STATUS_ERROR_SURFACE_BUSY;
    }
    if (passed) {
        status = hobot_vaDestroySurfaces(&va_ctx, &surface, 1);
        passed = status == VA_STATUS_ERROR_SURFACE_BUSY && surf->allocated &&
                 drv.images[first.image_id].allocated && surf->lock_count == 2;
    }
    if (passed)
        passed = hobot_vaDestroyImage(&va_ctx, first.image_id) == VA_STATUS_SUCCESS &&
                 surf->lock_count == 1 &&
                 hobot_vaDestroyImage(&va_ctx, second.image_id) == VA_STATUS_SUCCESS &&
                 surf->lock_count == 0;
    surf->has_decoded_frame = 0;
    memset(&surf->vpu_out_buf, 0, sizeof(surf->vpu_out_buf));
    if (peer_surface > 0 && peer_surface < MAX_SURFACES &&
        drv.surfaces[peer_surface].allocated) {
        drv.surfaces[peer_surface].raw_data = NULL;
        VAStatus peer_status = hobot_vaDestroySurfaces(&va_ctx, &peer_surface, 1);
        if (peer_status != VA_STATUS_SUCCESS)
            passed = 0;
    }
    if (passed) {
        status = hobot_vaDestroySurfaces(&va_ctx, &surface, 1);
        passed = status == VA_STATUS_SUCCESS && !surf->allocated;
    }

    VASurfaceID unsupported_surface = VA_INVALID_SURFACE;
    if (passed) {
        status = hobot_vaCreateSurfaces2(&va_ctx, VA_RT_FORMAT_YUV420,
                                         64, 64, &unsupported_surface, 1, NULL, 0);
        passed = status == VA_STATUS_SUCCESS;
    }
    if (passed) {
        uint8_t split_storage[64 * 64 + 64 * 32 + 4096] = {0};
        uint8_t *split_y = split_storage;
        uint8_t *split_uv = split_storage + 8192;
        const unsigned int split_size = 64 * 64 + 64 * 32;
        HobotSurface *unsupported = &drv.surfaces[unsupported_surface];
        unsupported->has_decoded_frame = 1;
        unsupported->vpu_out_buf.vframe_buf.vir_ptr[0] = split_y;
        unsupported->vpu_out_buf.vframe_buf.vir_ptr[1] = split_uv;
        unsupported->vpu_out_buf.vframe_buf.stride = 64;
        unsupported->vpu_out_buf.vframe_buf.vstride = 64;
        unsupported->vpu_out_buf.vframe_buf.size = split_size;
        unsupported->vpu_out_buf.vframe_buf.fd[0] = 8;
        unsupported->vpu_out_buf.vframe_buf.fd[1] = 8;
        unsupported->vpu_out_buf.vframe_buf.compSize[0] = 64 * 64;
        unsupported->vpu_out_buf.vframe_buf.compSize[1] = 64 * 32;
        VAImage rejected = {0};
        status = hobot_vaDeriveImage(&va_ctx, unsupported_surface, &rejected);
        passed = status == VA_STATUS_ERROR_OPERATION_FAILED &&
                 !drv.images[1].allocated && !drv.buffers[1].allocated &&
                 unsupported->lock_count == 0;
        unsupported->has_decoded_frame = 0;
        memset(&unsupported->vpu_out_buf, 0, sizeof(unsupported->vpu_out_buf));
        if (passed)
            passed = hobot_vaDestroySurfaces(&va_ctx, &unsupported_surface, 1) ==
                     VA_STATUS_SUCCESS;
    }

    mock_mem_invalidate_result = 0;
    mock_mem_flush_result = 0;
    pthread_mutex_destroy(&drv.mutex);
    if (!passed)
        fprintf(stderr, "direct vaDeriveImage storage, sync, or busy-state regression failed: status=%d\n",
                status);
    return passed;
}

static int test_derived_image_buffer_handle_lifecycle(void)
{
    enum { WIDTH = 64, HEIGHT = 64, STRIDE = 64, Y_SIZE = STRIDE * HEIGHT,
           FRAME_SIZE = Y_SIZE + STRIDE * (HEIGHT / 2) };
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    uint8_t frame[FRAME_SIZE] = {0};
    int source_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (source_fd < 0 || pthread_mutex_init(&drv.mutex, NULL) != 0) {
        if (source_fd >= 0)
            close(source_fd);
        return 0;
    }
    va_ctx.pDriverData = &drv;

    VASurfaceID surface = VA_INVALID_SURFACE;
    VAStatus status = hobot_vaCreateSurfaces2(&va_ctx, VA_RT_FORMAT_YUV420,
                                               WIDTH, HEIGHT, &surface, 1,
                                               NULL, 0);
    if (status != VA_STATUS_SUCCESS) {
        close(source_fd);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    HobotSurface *surf = &drv.surfaces[surface];
    surf->has_decoded_frame = 1;
    surf->vpu_out_buf.vframe_buf.vir_ptr[0] = frame;
    surf->vpu_out_buf.vframe_buf.vir_ptr[1] = frame + Y_SIZE;
    surf->vpu_out_buf.vframe_buf.phy_ptr[0] = 0x100000;
    surf->vpu_out_buf.vframe_buf.phy_ptr[1] = 0x100000 + Y_SIZE;
    surf->vpu_out_buf.vframe_buf.stride = STRIDE;
    surf->vpu_out_buf.vframe_buf.vstride = STRIDE;
    surf->vpu_out_buf.vframe_buf.size = sizeof(frame);
    surf->vpu_out_buf.vframe_buf.fd[0] = source_fd;
    surf->vpu_out_buf.vframe_buf.fd[1] = source_fd;
    surf->vpu_out_buf.vframe_buf.compSize[0] = Y_SIZE;
    surf->vpu_out_buf.vframe_buf.compSize[1] = STRIDE * (HEIGHT / 2);

    VAImage image = {0};
    status = hobot_vaDeriveImage(&va_ctx, surface, &image);
    int passed = status == VA_STATUS_SUCCESS;
    VABufferInfo info = {0};
    info.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    if (passed) {
        passed = hobot_vaAcquireBufferHandle(&va_ctx, image.buf, &info) ==
                     VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE &&
                 !drv.buffers[image.buf].handle_acquired &&
                 surf->external_handle_count == 0;
    }
    memset(&info, 0, sizeof(info));
    info.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
    if (passed) {
        status = hobot_vaAcquireBufferHandle(&va_ctx, image.buf, &info);
        struct stat st;
        passed = status == VA_STATUS_SUCCESS &&
                 info.handle != (uintptr_t)source_fd &&
                 info.type == VAImageBufferType &&
                 info.mem_type == VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME &&
                 info.mem_size == sizeof(frame) &&
                 fstat((int)info.handle, &st) == 0 &&
                 drv.buffers[image.buf].handle_acquired &&
                 drv.buffers[image.buf].acquired_fd == (int)info.handle &&
                 surf->external_handle_count == 1;
    }
    if (passed) {
        VABufferInfo duplicate = {0};
        duplicate.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
        void *mapped = NULL;
        VADRMPRIMESurfaceDescriptor desc = {0};
        drv.configs[1].allocated = 1;
        drv.configs[1].profile = VAProfileH264Main;
        drv.configs[1].entrypoint = VAEntrypointVLD;
        VAContextID context = VA_INVALID_ID;
        passed = hobot_vaAcquireBufferHandle(&va_ctx, image.buf, &duplicate) ==
                     VA_STATUS_ERROR_SURFACE_BUSY &&
                 hobot_vaMapBuffer(&va_ctx, image.buf, &mapped) ==
                     VA_STATUS_ERROR_SURFACE_BUSY && mapped == NULL &&
                 hobot_vaSyncSurface(&va_ctx, surface) ==
                     VA_STATUS_ERROR_SURFACE_BUSY &&
                 hobot_vaExportSurfaceHandle(
                     &va_ctx, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                     VA_EXPORT_SURFACE_READ_ONLY, &desc) ==
                     VA_STATUS_ERROR_SURFACE_BUSY &&
                 hobot_vaCreateContext(&va_ctx, 1, WIDTH, HEIGHT, 0,
                                       &surface, 1, &context) ==
                     VA_STATUS_ERROR_SURFACE_BUSY &&
                 context == VA_INVALID_ID &&
                 hobot_vaDestroyImage(&va_ctx, image.image_id) ==
                     VA_STATUS_ERROR_OPERATION_FAILED &&
                 hobot_vaDestroySurfaces(&va_ctx, &surface, 1) ==
                     VA_STATUS_ERROR_SURFACE_BUSY;
    }
    uintptr_t acquired_handle = info.handle;
    if (drv.buffers[image.buf].handle_acquired) {
        VAStatus release_status = hobot_vaReleaseBufferHandle(&va_ctx, image.buf);
        passed = passed && release_status == VA_STATUS_SUCCESS &&
                 fcntl((int)acquired_handle, F_GETFD) == -1 && errno == EBADF &&
                 !drv.buffers[image.buf].handle_acquired &&
                 drv.buffers[image.buf].acquired_fd == -1 &&
                 surf->external_handle_count == 0 &&
                 hobot_vaReleaseBufferHandle(&va_ctx, image.buf) ==
                     VA_STATUS_ERROR_OPERATION_FAILED &&
                 hobot_vaSyncSurface(&va_ctx, surface) == VA_STATUS_SUCCESS;
    } else {
        passed = 0;
    }
    if (drv.images[image.image_id].allocated)
        passed = hobot_vaDestroyImage(&va_ctx, image.image_id) ==
                     VA_STATUS_SUCCESS && passed;
    surf->has_decoded_frame = 0;
    memset(&surf->vpu_out_buf, 0, sizeof(surf->vpu_out_buf));
    if (surf->allocated)
        passed = hobot_vaDestroySurfaces(&va_ctx, &surface, 1) ==
                     VA_STATUS_SUCCESS && passed;
    close(source_fd);
    pthread_mutex_destroy(&drv.mutex);
    if (!passed)
        fprintf(stderr, "derived-image DRM PRIME buffer-handle lifecycle failed: status=%d\n",
                status);
    return passed;
}

static int test_dequeued_output_recycle_failure_preserves_ownership(int corrupt_frame)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->current_render_target = 2;
    hctx->decode_picture_active = 0;
    hctx->submitted_surfaces[hctx->sub_tail++] = 2;
    drv.surfaces[2].allocated = 1;
    drv.surfaces[2].context_id = 1;
    drv.surfaces[2].decode_pending = 1;

    mock_dequeue_result = 0;
    mock_dequeue_output_invalid = !corrupt_frame;
    mock_err_mb = corrupt_frame ? 1 : 0;
    mock_dequeue_output_calls = 0;
    mock_queue_output_result = -1;
    mock_queue_output_calls = 0;
    VAStatus status = hobot_vaSyncSurface(&va_ctx, 2);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || !hctx->decode_failed ||
        !hctx->dec_out_buf_valid || hctx->dec_out_buf.vframe_buf.phy_ptr[0] != 1 ||
        hctx->dec_out_buf.vframe_buf.size != sizeof(mock_frame) ||
        (corrupt_frame && (drv.surfaces[2].decode_pending ||
                           !drv.surfaces[2].decode_error || hctx->sub_head != hctx->sub_tail)) ||
        (!corrupt_frame && (!drv.surfaces[2].decode_pending ||
                            drv.surfaces[2].decode_error || hctx->sub_head == hctx->sub_tail))) {
        fprintf(stderr, "dequeued output ownership lost: corrupt=%d status=%d failed=%d retained=%d pending=%d error=%d fifo=%u/%u queues=%d\n",
                corrupt_frame, status, hctx->decode_failed, hctx->dec_out_buf_valid,
                drv.surfaces[2].decode_pending, drv.surfaces[2].decode_error,
                hctx->sub_head, hctx->sub_tail, mock_queue_output_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    drv.surfaces[3].allocated = 1;
    drv.surfaces[3].context_id = 1;
    drv.surfaces[3].decode_pending = 1;
    hctx->submitted_surfaces[hctx->sub_tail % 128] = 3;
    hctx->sub_tail++;
    status = hobot_vaSyncSurface(&va_ctx, 3);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || !hctx->dec_out_buf_valid ||
        mock_dequeue_output_calls != 1 || mock_queue_output_calls != 2) {
        fprintf(stderr, "poisoned decoder dequeued after retaining output: corrupt=%d status=%d retained=%d deq=%d queues=%d\n",
                corrupt_frame, status, hctx->dec_out_buf_valid,
                mock_dequeue_output_calls, mock_queue_output_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    mock_queue_output_result = 0;
    mock_stop_result = 0;
    status = hobot_vaDestroyContext(&va_ctx, 1);
    if (status != VA_STATUS_SUCCESS || hctx->allocated || hctx->dec_out_buf_valid ||
        drv.surfaces[2].decode_pending || !drv.surfaces[2].decode_error ||
        mock_queue_output_calls != 3) {
        fprintf(stderr, "retained output teardown retry failed: corrupt=%d status=%d allocated=%d retained=%d pending=%d error=%d queues=%d\n",
                corrupt_frame, status, hctx->allocated, hctx->dec_out_buf_valid,
                drv.surfaces[2].decode_pending, drv.surfaces[2].decode_error,
                mock_queue_output_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    pthread_mutex_destroy(&drv.mutex);
    mock_dequeue_result = 0;
    mock_dequeue_output_invalid = 0;
    mock_err_mb = 0;
    mock_queue_output_result = 0;
    mock_stop_result = 0;
    return 1;
}

static int test_dequeued_output_timeout_is_retried_on_next_sync(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->submitted_surfaces[hctx->sub_tail++] = 1;
    drv.surfaces[1].allocated = 1;
    drv.surfaces[1].context_id = 1;
    drv.surfaces[1].decode_pending = 1;

    mock_dequeue_result = 0;
    mock_dequeue_output_invalid = 1;
    mock_err_mb = 0;
    mock_dequeue_output_calls = 0;
    mock_queue_output_calls = 0;
    mock_queue_output_result = 0;
    mock_queue_output_wait_timeout = 1;

    VAStatus status = hobot_vaSyncSurface2(&va_ctx, 1, 20000000ULL);
    int retained_after_timeout = status == VA_STATUS_ERROR_TIMEDOUT &&
        !hctx->decode_failed && hctx->dec_out_buf_valid &&
        hctx->dec_out_buf.vframe_buf.phy_ptr[0] == 1 &&
        hctx->dec_out_buf.vframe_buf.size == sizeof(mock_frame) &&
        drv.surfaces[1].decode_pending && !hctx->sync_active &&
        mock_dequeue_output_calls == 1 && mock_queue_output_calls == 1;

    mock_dequeue_output_invalid = 0;
    status = hobot_vaSyncSurface2(&va_ctx, 1, 20000000ULL);
    int retained_after_retry_timeout = status == VA_STATUS_ERROR_TIMEDOUT &&
        !hctx->decode_failed && hctx->dec_out_buf_valid &&
        hctx->dec_out_buf.vframe_buf.phy_ptr[0] == 1 &&
        hctx->dec_out_buf.vframe_buf.size == sizeof(mock_frame) &&
        drv.surfaces[1].decode_pending && !hctx->sync_active &&
        mock_dequeue_output_calls == 1 && mock_queue_output_calls == 2;

    mock_queue_output_wait_timeout = 0;
    status = hobot_vaSyncSurface2(&va_ctx, 1, 1000000000ULL);
    int recovered = status == VA_STATUS_SUCCESS && !hctx->decode_failed &&
        !hctx->dec_out_buf_valid && drv.surfaces[1].has_decoded_frame &&
        !drv.surfaces[1].decode_pending && !hctx->sync_active &&
        mock_dequeue_output_calls == 2 && mock_queue_output_calls == 3;

    if (!retained_after_timeout || !retained_after_retry_timeout || !recovered)
        fprintf(stderr,
                "dequeued output timeout retry failed: retained=%d retry_retained=%d recovered=%d status=%d valid=%d failed=%d dequeue=%d queue=%d\n",
                retained_after_timeout, retained_after_retry_timeout, recovered,
                status, hctx->dec_out_buf_valid, hctx->decode_failed,
                mock_dequeue_output_calls, mock_queue_output_calls);
    if (drv.sync_cond_initialized)
        pthread_cond_destroy(&drv.sync_cond);
    pthread_mutex_destroy(&drv.mutex);
    mock_dequeue_result = 0;
    mock_dequeue_output_invalid = 0;
    mock_err_mb = 0;
    mock_queue_output_result = 0;
    mock_queue_output_wait_timeout = 0;
    return retained_after_timeout && retained_after_retry_timeout && recovered;
}

static int test_video_decode_result_is_validated(void)
{
    const struct {
        int result;
        VAStatus expected_status;
        int expected_error;
        int expected_anomaly;
    } cases[] = {
        {0x00, VA_STATUS_ERROR_DECODING_ERROR, 1, 1},
        {0x02, VA_STATUS_ERROR_DECODING_ERROR, 1, 1},
        {0x10, VA_STATUS_SUCCESS, 0, 1},
        {0x01, VA_STATUS_SUCCESS, 0, 0},
    };
    int passed = 1;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        HobotDriverData drv = {0};
        struct VADriverContext va_ctx = {0};
        if (pthread_mutex_init(&drv.mutex, NULL) != 0)
            return 0;
        va_ctx.pDriverData = &drv;

        HobotContext *hctx = &drv.contexts[1];
        hctx->allocated = 1;
        hctx->vpu_initialized = 1;
        hctx->vpu_running = 1;
        hctx->submitted_surfaces[hctx->sub_tail++] = 1;

        HobotSurface *surf = &drv.surfaces[1];
        surf->allocated = 1;
        surf->context_id = 1;
        surf->decode_pending = 1;

        mock_dequeue_result = 0;
        mock_dequeue_output_invalid = 0;
        mock_decode_result = cases[i].result;
        mock_err_mb = 0;
        mock_dequeue_output_calls = 0;
        mock_queue_output_result = 0;
        mock_queue_output_calls = 0;
        recycled_output_count = 0;
        mock_stop_result = 0;
        mock_release_result = 0;

        VAStatus status = hobot_vaSyncSurface(&va_ctx, 1);
        int frame_state_valid = cases[i].expected_error ?
            (!surf->has_decoded_frame && surf->decode_error &&
             !surf->decode_pending && mock_queue_output_calls == 1 &&
             recycled_output_count == 1) :
            (surf->has_decoded_frame && !surf->decode_error &&
             !surf->decode_pending && surf->dma_fd == 7 &&
             mock_queue_output_calls == 0);
        int result_valid = status == cases[i].expected_status && frame_state_valid &&
            hctx->watchdog_anomaly_count == cases[i].expected_anomaly &&
            mock_dequeue_output_calls == 1 && hctx->sub_head == hctx->sub_tail;
        if (!result_valid) {
            fprintf(stderr,
                    "decoder result handling failed: result=0x%x status=%d pending=%d error=%d frame=%d anomaly=%d recycled=%d\n",
                    cases[i].result, status, surf->decode_pending,
                    surf->decode_error, surf->has_decoded_frame,
                    hctx->watchdog_anomaly_count, recycled_output_count);
            passed = 0;
        }

        VAStatus destroy_status = hobot_vaDestroyContext(&va_ctx, 1);
        if (destroy_status != VA_STATUS_SUCCESS || hctx->allocated ||
            mock_queue_output_calls != 1) {
            fprintf(stderr,
                    "decoder result test teardown failed: result=0x%x status=%d queues=%d\n",
                    cases[i].result, destroy_status, mock_queue_output_calls);
            passed = 0;
        }
        pthread_mutex_destroy(&drv.mutex);
        if (!passed)
            break;
    }

    char watchdog_path[128];
    if (hobot_watchdog_path_for_pid(watchdog_path, sizeof(watchdog_path),
                                    (int)getpid()))
        remove(watchdog_path);
    mock_decode_result = HOBOT_DECODE_RESULT_SUCCESS;
    mock_dequeue_result = 0;
    mock_dequeue_output_invalid = 0;
    mock_err_mb = 0;
    mock_queue_output_result = 0;
    mock_stop_result = 0;
    mock_release_result = 0;
    return passed;
}

static int test_buffer_size_overflow_rejected(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    VABufferID buffer = VA_INVALID_ID;
    VAStatus status = hobot_vaCreateBuffer(&va_ctx, 0, VAImageBufferType,
                                            UINT_MAX, 2, NULL, &buffer);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER || drv.buffers[1].allocated) {
        fprintf(stderr, "overflowing buffer size accepted: status=%d allocated=%d\n",
                status, drv.buffers[1].allocated);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    status = hobot_vaCreateBuffer2(&va_ctx, 0, VAImageBufferType,
                                   UINT_MAX, 2, NULL, NULL, &buffer);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER || drv.buffers[1].allocated) {
        fprintf(stderr, "overflowing buffer2 dimensions accepted: status=%d allocated=%d\n",
                status, drv.buffers[1].allocated);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    const unsigned int invalid_dimensions[][2] = {{64, 0}, {0, 64}};
    for (size_t i = 0; i < sizeof(invalid_dimensions) / sizeof(invalid_dimensions[0]); i++) {
        buffer = VA_INVALID_ID;
        status = hobot_vaCreateBuffer2(&va_ctx, 0, VAImageBufferType,
                                       invalid_dimensions[i][0],
                                       invalid_dimensions[i][1],
                                       NULL, NULL, &buffer);
        if (status != VA_STATUS_ERROR_INVALID_PARAMETER ||
            buffer != VA_INVALID_ID || drv.buffers[1].allocated) {
            fprintf(stderr, "zero-sized buffer2 dimensions accepted: %ux%u status=%d id=%u\n",
                    invalid_dimensions[i][0], invalid_dimensions[i][1], status, buffer);
            pthread_mutex_destroy(&drv.mutex);
            return 0;
        }
    }

    const unsigned int invalid_buffer_sizes[][2] = {{0, 1}, {1, 0}, {0, 0}};
    for (size_t i = 0; i < sizeof(invalid_buffer_sizes) / sizeof(invalid_buffer_sizes[0]); i++) {
        buffer = VA_INVALID_ID;
        status = hobot_vaCreateBuffer(&va_ctx, 0, VAImageBufferType,
                                      invalid_buffer_sizes[i][0],
                                      invalid_buffer_sizes[i][1], NULL, &buffer);
        if (status != VA_STATUS_ERROR_INVALID_PARAMETER ||
            buffer != VA_INVALID_ID || drv.buffers[1].allocated) {
            fprintf(stderr, "zero-capacity buffer accepted: size=%u elements=%u status=%d id=%u\n",
                    invalid_buffer_sizes[i][0], invalid_buffer_sizes[i][1],
                    status, buffer);
            pthread_mutex_destroy(&drv.mutex);
            return 0;
        }
    }

    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static int test_buffer_num_elements_tracks_valid_size(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    const uint8_t initial[16] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16
    };
    VABufferID buffer_id = VA_INVALID_ID;
    VAStatus status = hobot_vaCreateBuffer(&va_ctx, 0, VAImageBufferType,
                                            4, 4, (void *)initial, &buffer_id);
    if (status != VA_STATUS_SUCCESS || buffer_id <= 0 || buffer_id >= MAX_BUFFERS ||
        drv.buffers[buffer_id].size != sizeof(initial) ||
        drv.buffers[buffer_id].capacity != sizeof(initial) ||
        drv.buffers[buffer_id].element_size != 4 ||
        drv.buffers[buffer_id].num_elements != 4) {
        fprintf(stderr, "buffer initial element metadata incorrect: status=%d id=%u\n",
                status, buffer_id);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    VABufferType info_type = VAImageBufferType;
    unsigned int info_size = 0;
    unsigned int info_count = 0;
    status = hobot_vaBufferInfo(&va_ctx, buffer_id, &info_type, &info_size, &info_count);
    if (status != VA_STATUS_SUCCESS || info_type != VAImageBufferType ||
        info_size != sizeof(initial) || info_count != 4) {
        fprintf(stderr, "buffer info callback returned stale metadata: status=%d size=%u count=%u\n",
                status, info_size, info_count);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    status = hobot_vaBufferSetNumElements(&va_ctx, buffer_id, 2);
    if (status != VA_STATUS_SUCCESS || drv.buffers[buffer_id].size != 8 ||
        drv.buffers[buffer_id].capacity != sizeof(initial) ||
        drv.buffers[buffer_id].num_elements != 2 ||
        memcmp(drv.buffers[buffer_id].data, initial, sizeof(initial)) != 0) {
        fprintf(stderr, "buffer valid element count not applied: status=%d size=%u count=%u\n",
                status, drv.buffers[buffer_id].size,
                drv.buffers[buffer_id].num_elements);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    status = hobot_vaBufferSetNumElements(&va_ctx, buffer_id, 5);
    if (status != VA_STATUS_ERROR_MAX_NUM_EXCEEDED ||
        drv.buffers[buffer_id].size != 8 || drv.buffers[buffer_id].num_elements != 2) {
        fprintf(stderr, "buffer capacity overrun accepted: status=%d size=%u count=%u\n",
                status, drv.buffers[buffer_id].size,
                drv.buffers[buffer_id].num_elements);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    status = hobot_vaBufferSetNumElements(&va_ctx, buffer_id, 4);
    if (status != VA_STATUS_SUCCESS || drv.buffers[buffer_id].size != sizeof(initial)) {
        fprintf(stderr, "buffer capacity restore failed: status=%d size=%u\n",
                status, drv.buffers[buffer_id].size);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    hobot_vaDestroyBuffer(&va_ctx, buffer_id);
    status = hobot_vaBufferSetNumElements(&va_ctx, buffer_id, 1);
    if (status != VA_STATUS_ERROR_INVALID_BUFFER ||
        hobot_vaBufferInfo(&va_ctx, buffer_id, &info_type, &info_size, &info_count) !=
            VA_STATUS_ERROR_INVALID_BUFFER) {
        fprintf(stderr, "destroyed buffer remained usable through buffer API: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static int test_coded_buffer_honors_requested_capacity(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    VABufferID buffer_id = VA_INVALID_ID;
    VAStatus status = hobot_vaCreateBuffer(&va_ctx, 0, VAEncCodedBufferType,
                                            127, 1, NULL, &buffer_id);
    if (status != VA_STATUS_SUCCESS || buffer_id == VA_INVALID_ID ||
        drv.buffers[buffer_id].size != 127 || drv.buffers[buffer_id].capacity != 127 ||
        drv.buffers[buffer_id].element_size != 127 ||
        drv.buffers[buffer_id].num_elements != 1 ||
        drv.buffers[buffer_id].coded_segment.buf != drv.buffers[buffer_id].data) {
        fprintf(stderr, "coded buffer did not honor requested capacity: status=%d id=%u size=%u capacity=%u\n",
                status, buffer_id,
                buffer_id < MAX_BUFFERS ? drv.buffers[buffer_id].size : 0,
                buffer_id < MAX_BUFFERS ? drv.buffers[buffer_id].capacity : 0);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    unsigned int size = 0;
    unsigned int num_elements = 0;
    VABufferType type = VAImageBufferType;
    status = hobot_vaBufferInfo(&va_ctx, buffer_id, &type, &size, &num_elements);
    if (status != VA_STATUS_SUCCESS || type != VAEncCodedBufferType ||
        size != 127 || num_elements != 1 ||
        hobot_vaDestroyBuffer(&va_ctx, buffer_id) != VA_STATUS_SUCCESS) {
        fprintf(stderr, "coded buffer info did not report requested capacity: status=%d size=%u elements=%u\n",
                status, size, num_elements);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    buffer_id = VA_INVALID_ID;
    status = hobot_vaCreateBuffer(&va_ctx, 0, VAEncCodedBufferType,
                                  0, 1, NULL, &buffer_id);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER || buffer_id != VA_INVALID_ID) {
        fprintf(stderr, "zero-capacity coded buffer was accepted: status=%d id=%u\n",
                status, buffer_id);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static int test_legacy_surface_attribute_query(void)
{
    struct VADriverVTable vtable = {0};
    struct VADriverContext va_ctx = {0};
    va_ctx.vtable = &vtable;
    int saved_open_result = mock_mem_module_open_result;
    int saved_close_result = mock_mem_module_close_result;
    mock_mem_module_open_result = 0;
    mock_mem_module_close_result = 0;
    VAStatus init_status = __vaDriverInit_1_0(&va_ctx);
    if (init_status != VA_STATUS_SUCCESS || !va_ctx.pDriverData ||
        !vtable.vaGetSurfaceAttributes) {
        mock_mem_module_open_result = saved_open_result;
        mock_mem_module_close_result = saved_close_result;
        return 0;
    }
    HobotDriverData *drv = (HobotDriverData *)va_ctx.pDriverData;
    drv->configs[1].allocated = 1;
    drv->configs[1].profile = VAProfileHEVCMain;
    drv->configs[1].entrypoint = VAEntrypointVLD;

    VASurfaceAttrib attrs[7] = {0};
    VAStatus status = vtable.vaGetSurfaceAttributes(&va_ctx, 1, attrs, 7);
    int passed = status == VA_STATUS_SUCCESS &&
        attrs[0].type == VASurfaceAttribPixelFormat &&
        attrs[0].flags == (VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE) &&
        attrs[0].value.type == VAGenericValueTypeInteger &&
        attrs[0].value.value.i == (int)VA_FOURCC_NV12 &&
        attrs[1].type == VASurfaceAttribMemoryType &&
        attrs[1].value.value.i == (VA_SURFACE_ATTRIB_MEM_TYPE_VA |
                                   VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2) &&
        attrs[2].type == VASurfaceAttribExternalBufferDescriptor &&
        attrs[2].flags == VA_SURFACE_ATTRIB_SETTABLE &&
        attrs[2].value.type == VAGenericValueTypePointer &&
        attrs[2].value.value.p == NULL &&
        attrs[3].type == VASurfaceAttribMinWidth &&
        attrs[3].value.value.i == 64 &&
        attrs[4].type == VASurfaceAttribMinHeight &&
        attrs[4].value.value.i == 64 &&
        attrs[5].type == VASurfaceAttribMaxWidth &&
        attrs[5].value.value.i == 8192 &&
        attrs[6].type == VASurfaceAttribMaxHeight &&
        attrs[6].value.value.i == 4096;

    if (vtable.vaGetSurfaceAttributes(&va_ctx, 1, attrs, 6) !=
            VA_STATUS_ERROR_MAX_NUM_EXCEEDED ||
        vtable.vaGetSurfaceAttributes(&va_ctx, 2, attrs, 7) !=
            VA_STATUS_ERROR_INVALID_CONFIG ||
        vtable.vaGetSurfaceAttributes(&va_ctx, 1, NULL, 7) !=
            VA_STATUS_SUCCESS)
        passed = 0;

    VAStatus terminate_status = vtable.vaTerminate(&va_ctx);
    if (terminate_status != VA_STATUS_SUCCESS || va_ctx.pDriverData != NULL)
        passed = 0;
    if (!passed)
        fprintf(stderr, "legacy surface-attribute vtable query failed: init=%d query=%d terminate=%d\n",
                init_status, status, terminate_status);
    mock_mem_module_open_result = saved_open_result;
    mock_mem_module_close_result = saved_close_result;
    return passed;
}

static int test_unsupported_va_operations_report_status(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    drv.surfaces[1].allocated = 1;

    unsigned int subpicture_count = 7;
    if (hobot_vaQuerySubpictureFormats(&va_ctx, NULL, NULL, &subpicture_count) !=
            VA_STATUS_SUCCESS || subpicture_count != 0 ||
        hobot_vaQuerySubpictureFormats(&va_ctx, NULL, NULL, NULL) !=
            VA_STATUS_ERROR_INVALID_PARAMETER) {
        fprintf(stderr, "unsupported subpicture query contract failed\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    VASubpictureID subpicture = 0;
    VASurfaceID subpicture_target = 1;
    if (hobot_vaCreateSubpicture(&va_ctx, 1, &subpicture) !=
            VA_STATUS_ERROR_UNIMPLEMENTED || subpicture != VA_INVALID_ID ||
        hobot_vaDestroySubpicture(&va_ctx, 1) != VA_STATUS_ERROR_UNIMPLEMENTED ||
        hobot_vaSetSubpictureImage(&va_ctx, 1, 1) != VA_STATUS_ERROR_UNIMPLEMENTED ||
        hobot_vaSetSubpictureChromakey(&va_ctx, 1, 0, 0, 0) !=
            VA_STATUS_ERROR_UNIMPLEMENTED ||
        hobot_vaSetSubpictureGlobalAlpha(&va_ctx, 1, 1.0f) != VA_STATUS_ERROR_UNIMPLEMENTED) {
        fprintf(stderr, "unsupported subpicture operation reported success\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    if (hobot_vaAssociateSubpicture(&va_ctx, 1, &subpicture_target, 1,
                                    0, 0, 2, 2, 0, 0, 2, 2, 0) !=
            VA_STATUS_ERROR_UNIMPLEMENTED ||
        hobot_vaDeassociateSubpicture(&va_ctx, 1, &subpicture_target, 1) !=
            VA_STATUS_ERROR_UNIMPLEMENTED) {
        fprintf(stderr, "unsupported subpicture association reported success\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    VABufferInfo buffer_info = {0};
    VAMFContextID mf_context = 0;
    VAContextID mf_context_id = 1;
    unsigned int processing_rate = 99;
    VACopyObject copy_src = {0};
    VACopyObject copy_dst = {0};
    VACopyOption copy_option = {0};
    drv.configs[1].allocated = 1;
    if (hobot_vaAcquireBufferHandle(&va_ctx, 1, &buffer_info) !=
            VA_STATUS_ERROR_INVALID_BUFFER ||
        hobot_vaReleaseBufferHandle(&va_ctx, 1) !=
            VA_STATUS_ERROR_INVALID_BUFFER ||
        hobot_vaCreateMFContext(&va_ctx, &mf_context) !=
            VA_STATUS_ERROR_UNIMPLEMENTED || mf_context != VA_INVALID_ID ||
        hobot_vaMFAddContext(&va_ctx, 1, 1) != VA_STATUS_ERROR_UNIMPLEMENTED ||
        hobot_vaMFReleaseContext(&va_ctx, 1, 1) != VA_STATUS_ERROR_UNIMPLEMENTED ||
        hobot_vaMFSubmit(&va_ctx, 1, &mf_context_id, 1) !=
            VA_STATUS_ERROR_UNIMPLEMENTED ||
        hobot_vaQueryProcessingRate(&va_ctx, 1, NULL, &processing_rate) !=
            VA_STATUS_ERROR_UNIMPLEMENTED || processing_rate != 0 ||
        hobot_vaCopy(&va_ctx, &copy_dst, &copy_src, copy_option) !=
            VA_STATUS_ERROR_UNIMPLEMENTED) {
        fprintf(stderr, "unsupported extension callback contract failed\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    int display_count = -1;
    if (hobot_vaQueryDisplayAttributes(&va_ctx, NULL, &display_count) !=
            VA_STATUS_SUCCESS || display_count != 0 ||
        hobot_vaGetDisplayAttributes(&va_ctx, NULL, 1) !=
            VA_STATUS_ERROR_INVALID_PARAMETER ||
        hobot_vaGetDisplayAttributes(&va_ctx, NULL, 0) != VA_STATUS_SUCCESS ||
        hobot_vaSetDisplayAttributes(&va_ctx, NULL, 1) !=
            VA_STATUS_ERROR_INVALID_PARAMETER ||
        hobot_vaSetDisplayAttributes(&va_ctx, NULL, 0) != VA_STATUS_SUCCESS) {
        fprintf(stderr, "unsupported display attribute contract failed\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    if (hobot_vaPutSurface(&va_ctx, 1, NULL, 0, 0, 2, 2, 0, 0, 2, 2,
                           NULL, 0, 0) != VA_STATUS_ERROR_UNIMPLEMENTED ||
        hobot_vaPutSurface(&va_ctx, 2, NULL, 0, 0, 2, 2, 0, 0, 2, 2,
                           NULL, 0, 0) != VA_STATUS_ERROR_INVALID_SURFACE ||
        hobot_vaSetImagePalette(&va_ctx, 1, NULL) != VA_STATUS_ERROR_UNIMPLEMENTED) {
        fprintf(stderr, "unsupported render/palette operation reported success\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    drv.buffers[2] = (HobotBuffer){
        .id = 2,
        .allocated = 1,
        .type = VAEncCodedBufferType,
        .size = 32,
        .capacity = 32,
        .element_size = 1,
        .num_elements = 32
    };
    if (hobot_vaBufferSetNumElements(&va_ctx, 2, 16) !=
            VA_STATUS_ERROR_UNIMPLEMENTED ||
        drv.buffers[2].size != 32 || drv.buffers[2].num_elements != 32) {
        fprintf(stderr, "coded-buffer element resize changed fixed capacity\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    VABufferID buffer = VA_INVALID_ID;
    if (hobot_vaCreateBuffer(&va_ctx, 0, VAImageBufferType, 4, 1, NULL, &buffer) !=
            VA_STATUS_SUCCESS ||
        hobot_vaUnmapBuffer(&va_ctx, buffer) != VA_STATUS_ERROR_OPERATION_FAILED ||
        hobot_vaUnmapBuffer(&va_ctx, VA_INVALID_ID) != VA_STATUS_ERROR_INVALID_BUFFER ||
        hobot_vaDestroyBuffer(&va_ctx, VA_INVALID_ID) != VA_STATUS_ERROR_INVALID_BUFFER ||
        hobot_vaDestroyImage(&va_ctx, VA_INVALID_ID) != VA_STATUS_ERROR_INVALID_IMAGE) {
        fprintf(stderr, "invalid buffer/image handle contract failed\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    hobot_vaDestroyBuffer(&va_ctx, buffer);
    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static int test_export_rejects_invalid_flags(void)
{
    enum { WIDTH = 64, HEIGHT = 64, STRIDE = 64, OBJECT_SIZE = STRIDE * HEIGHT * 3 / 2 };
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    VADRMPRIMESurfaceDescriptor descriptor = {0};
    int dma_fd = open("/dev/null", O_RDONLY);
    if (dma_fd < 0 || pthread_mutex_init(&drv.mutex, NULL) != 0) {
        if (dma_fd >= 0) close(dma_fd);
        return 0;
    }

    va_ctx.pDriverData = &drv;
    HobotSurface *surf = &drv.surfaces[1];
    surf->allocated = 1;
    surf->width = WIDTH;
    surf->height = HEIGHT;
    surf->stride = STRIDE;
    surf->dma_fd = dma_fd;
    surf->has_preallocated = 1;
    surf->preallocated_gbuf.fd[0] = dma_fd;
    surf->preallocated_gbuf.fd[1] = -1;
    surf->preallocated_gbuf.stride = STRIDE;
    surf->preallocated_gbuf.vstride = HEIGHT;
    surf->preallocated_gbuf.size[0] = OBJECT_SIZE;
    surf->preallocated_gbuf.offset[1] = STRIDE * HEIGHT;

    VAStatus status = hobot_vaExportSurfaceHandle(
        &va_ctx, 1, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | 0x10u, &descriptor);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER) {
        fprintf(stderr, "unknown export flag was accepted: status=%d\n", status);
        close(dma_fd);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    status = hobot_vaExportSurfaceHandle(
        &va_ctx, 1, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS |
            VA_EXPORT_SURFACE_COMPOSED_LAYERS,
        &descriptor);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER) {
        fprintf(stderr, "conflicting layer flags were accepted: status=%d\n", status);
        close(dma_fd);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    status = hobot_vaExportSurfaceHandle(
        &va_ctx, 1, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &descriptor);
    if (status != VA_STATUS_SUCCESS || descriptor.num_layers != 2 ||
        descriptor.num_objects != 1 || descriptor.layers[0].num_planes != 1 ||
        descriptor.layers[1].num_planes != 1) {
        fprintf(stderr, "valid separate-layer export failed: status=%d layers=%u\n",
                status, descriptor.num_layers);
        if (status == VA_STATUS_SUCCESS)
            for (uint32_t i = 0; i < descriptor.num_objects; i++) close(descriptor.objects[i].fd);
        close(dma_fd);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    for (uint32_t i = 0; i < descriptor.num_objects; i++) close(descriptor.objects[i].fd);

    status = hobot_vaExportSurfaceHandle(
        &va_ctx, 1, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_WRITE | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
        &descriptor);
    int passed = status == VA_STATUS_SUCCESS && descriptor.num_layers == 1 &&
                 descriptor.layers[0].drm_format == VA_FOURCC_NV12 &&
                 descriptor.layers[0].num_planes == 2;
    if (status == VA_STATUS_SUCCESS)
        for (uint32_t i = 0; i < descriptor.num_objects; i++) close(descriptor.objects[i].fd);
    if (!passed)
        fprintf(stderr, "valid composed read/write export failed: status=%d layers=%u\n",
                status, descriptor.num_layers);

    close(dma_fd);
    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_put_image_updates_preallocated_dma_surface(void)
{
    enum { WIDTH = 64, HEIGHT = 64, Y_SIZE = WIDTH * HEIGHT,
           UV_SIZE = Y_SIZE / 2, DATA_SIZE = Y_SIZE + UV_SIZE };
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    uint8_t dma_data[DATA_SIZE];
    uint8_t image_data[DATA_SIZE];

    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    HobotSurface *surf = &drv.surfaces[1];
    surf->allocated = 1;
    surf->width = WIDTH;
    surf->height = HEIGHT;
    surf->stride = WIDTH;
    surf->raw_data_size = DATA_SIZE;
    surf->has_preallocated = 1;
    surf->preallocated_gbuf.fd[0] = 7;
    surf->preallocated_gbuf.fd[1] = -1;
    surf->preallocated_gbuf.stride = WIDTH;
    surf->preallocated_gbuf.vstride = HEIGHT;
    surf->preallocated_gbuf.size[0] = DATA_SIZE;
    surf->preallocated_gbuf.offset[1] = Y_SIZE;
    surf->preallocated_gbuf.virt_addr[0] = dma_data;
    surf->preallocated_gbuf.virt_addr[1] = dma_data + Y_SIZE;
    memset(dma_data, 16, Y_SIZE);
    memset(dma_data + Y_SIZE, 128, UV_SIZE);

    memset(image_data, 0, sizeof(image_data));
    memset(image_data, 201, 2);
    memset(image_data + WIDTH, 202, 2);
    image_data[Y_SIZE] = 60;
    image_data[Y_SIZE + 1] = 70;
    drv.images[1].allocated = 1;
    drv.images[1].buf_id = 1;
    drv.images[1].image.width = WIDTH;
    drv.images[1].image.height = HEIGHT;
    drv.images[1].image.pitches[0] = WIDTH;
    drv.images[1].image.pitches[1] = WIDTH;
    drv.images[1].image.offsets[0] = 0;
    drv.images[1].image.offsets[1] = Y_SIZE;
    drv.images[1].image.data_size = DATA_SIZE;
    drv.buffers[1].allocated = 1;
    drv.buffers[1].data = image_data;
    drv.buffers[1].size = DATA_SIZE;
    mock_mem_flush_calls = 0;
    mock_mem_invalidate_calls = 0;
    mock_mem_flush_result = 0;
    mock_mem_invalidate_result = 0;

    VAStatus status = hobot_vaPutImage(&va_ctx, 1, 1, 0, 0, 2, 2, 2, 2, 2, 2);
    if (status != VA_STATUS_SUCCESS || dma_data[2 * WIDTH + 2] != 201 ||
        dma_data[3 * WIDTH + 2] != 202 || dma_data[0] != 16 ||
        dma_data[Y_SIZE + WIDTH + 2] != 60 ||
        dma_data[Y_SIZE + WIDTH + 3] != 70 || dma_data[Y_SIZE] != 128 ||
        surf->raw_data_dirty || mock_mem_flush_calls != 1 ||
        mock_mem_invalidate_calls != 1) {
        fprintf(stderr, "vaPutImage did not update DMA pixels and preserve outside region: status=%d dirty=%d flush=%d invalidate=%d\n",
                status, surf->raw_data_dirty, mock_mem_flush_calls,
                mock_mem_invalidate_calls);
        free(surf->raw_data);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    mock_mem_flush_result = -1;
    status = hobot_vaPutImage(&va_ctx, 1, 1, 0, 0, 2, 2, 4, 4, 2, 2);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || !surf->raw_data_dirty) {
        fprintf(stderr, "vaPutImage hid DMA flush failure: status=%d dirty=%d\n",
                status, surf->raw_data_dirty);
        free(surf->raw_data);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    mock_mem_flush_result = 0;
    struct hobot_surface_info info;
    status = hobot_fill_surface_info(&va_ctx, 1, &info);
    int passed = status == VA_STATUS_SUCCESS && !surf->raw_data_dirty &&
                 dma_data[4 * WIDTH + 4] == 201 && mock_mem_flush_calls == 3;
    if (passed) {
        status = hobot_vaGetImage(&va_ctx, 1, 0, 0, WIDTH, HEIGHT, 1);
        passed = status == VA_STATUS_SUCCESS && image_data[4 * WIDTH + 4] == 201 &&
                 image_data[Y_SIZE + 2 * WIDTH + 4] == 60;
    }
    free(surf->raw_data);
    pthread_mutex_destroy(&drv.mutex);
    if (!passed) {
        fprintf(stderr, "deferred staging upload or staged vaGetImage failed: status=%d dirty=%d flush=%d\n",
                status, surf->raw_data_dirty, mock_mem_flush_calls);
        return 0;
    }
    return 1;
}

static int test_preallocated_nv12_backing_bounds(void)
{
    enum { WIDTH = 64, HEIGHT = 64, Y_SIZE = WIDTH * HEIGHT,
           UV_SIZE = Y_SIZE / 2, DATA_SIZE = Y_SIZE + UV_SIZE };
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    VADRMPRIMESurfaceDescriptor descriptor = {0};
    uint8_t y_data[DATA_SIZE];
    uint8_t uv_data[UV_SIZE];
    uint8_t staging[DATA_SIZE];
    int fd0 = open("/dev/null", O_RDONLY);
    int fd1 = open("/dev/null", O_RDONLY);
    int descriptor_exported = 0;
    int passed = 0;
    if (fd0 < 0 || fd1 < 0 || fd0 == fd1 ||
        pthread_mutex_init(&drv.mutex, NULL) != 0) {
        if (fd0 >= 0) close(fd0);
        if (fd1 >= 0) close(fd1);
        return 0;
    }

    va_ctx.pDriverData = &drv;
    HobotSurface *surf = &drv.surfaces[1];
    surf->allocated = 1;
    surf->width = WIDTH;
    surf->height = HEIGHT;
    surf->stride = WIDTH;
    surf->raw_data_size = DATA_SIZE;
    surf->raw_data = staging;
    surf->raw_data_dirty = 1;
    surf->has_preallocated = 1;
    surf->dma_fd = fd0;
    surf->preallocated_gbuf.fd[0] = fd0;
    surf->preallocated_gbuf.fd[1] = -1;
    surf->preallocated_gbuf.stride = WIDTH;
    surf->preallocated_gbuf.vstride = HEIGHT;
    surf->preallocated_gbuf.size[0] = Y_SIZE;
    surf->preallocated_gbuf.offset[1] = Y_SIZE;
    surf->preallocated_gbuf.virt_addr[0] = y_data;
    surf->preallocated_gbuf.virt_addr[1] = y_data + Y_SIZE;
    memset(y_data, 0xa5, sizeof(y_data));
    memset(uv_data, 0x5a, sizeof(uv_data));
    memset(staging, 0x3c, sizeof(staging));

    VAStatus status = hobot_upload_staging_to_gbuf(surf);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || !surf->raw_data_dirty ||
        y_data[0] != 0xa5 || y_data[sizeof(y_data) - 1] != 0xa5) {
        fprintf(stderr, "staging upload accepted an undersized shared NV12 object: status=%d\n",
                status);
        goto cleanup;
    }
    surf->raw_data = NULL;
    status = hobot_copy_gbuf_to_staging(surf);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || surf->raw_data != NULL) {
        fprintf(stderr, "staging read accepted an undersized shared NV12 object: status=%d\n",
                status);
        goto cleanup;
    }
    struct hobot_surface_info info;
    status = hobot_fill_surface_info(&va_ctx, 1, &info);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED) {
        fprintf(stderr, "surface-info accepted an undersized shared NV12 object: status=%d\n",
                status);
        goto cleanup;
    }
    status = hobot_vaExportSurfaceHandle(
        &va_ctx, 1, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
        &descriptor);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED) {
        fprintf(stderr, "DRM export accepted an undersized shared NV12 object: status=%d\n",
                status);
        goto cleanup;
    }

    surf->preallocated_gbuf.size[1] = UV_SIZE;
    surf->preallocated_gbuf.offset[1] = 0;
    surf->preallocated_gbuf.is_contig = 0;
    surf->preallocated_gbuf.phys_addr[0] = 0x1000;
    surf->preallocated_gbuf.phys_addr[1] = 0;
    surf->preallocated_gbuf.virt_addr[1] = NULL;
    status = hobot_fill_surface_info(&va_ctx, 1, &info);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED) {
        fprintf(stderr, "surface-info inferred UV from an unverified noncontiguous buffer: status=%d\n",
                status);
        goto cleanup;
    }

    surf->preallocated_gbuf.size[1] = UV_SIZE;
    surf->preallocated_gbuf.offset[1] = 0;
    surf->preallocated_gbuf.is_contig = 1;
    surf->preallocated_gbuf.phys_addr[0] = 0x1000;
    surf->preallocated_gbuf.phys_addr[1] = 0x2000;
    surf->preallocated_gbuf.virt_addr[1] = y_data + Y_SIZE;
    surf->raw_data = staging;
    surf->raw_data_dirty = 1;
    mock_mem_flush_calls = 0;
    mock_mem_flush_result = 0;
    status = hobot_upload_staging_to_gbuf(surf);
    if (status != VA_STATUS_SUCCESS || surf->raw_data_dirty ||
        mock_mem_flush_calls != 1 || y_data[0] != 0x3c ||
        y_data[Y_SIZE] != 0x3c) {
        fprintf(stderr, "valid SDK shared-FD plane-size NV12 upload failed: status=%d flush=%d\n",
                status, mock_mem_flush_calls);
        goto cleanup;
    }
    status = hobot_vaExportSurfaceHandle(
        &va_ctx, 1, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
        &descriptor);
    if (status != VA_STATUS_SUCCESS || descriptor.num_objects != 1 ||
        descriptor.objects[0].size != DATA_SIZE ||
        descriptor.layers[0].offset[1] != Y_SIZE) {
        fprintf(stderr, "valid SDK shared-FD NV12 export failed: status=%d objects=%u size=%u uv=%u\n",
                status, descriptor.num_objects, descriptor.objects[0].size,
                descriptor.layers[0].offset[1]);
        goto cleanup;
    }
    for (uint32_t i = 0; i < descriptor.num_objects; i++)
        close(descriptor.objects[i].fd);
    memset(&descriptor, 0, sizeof(descriptor));

    surf->preallocated_gbuf.fd[1] = fd1;
    surf->preallocated_gbuf.is_contig = 0;
    surf->preallocated_gbuf.virt_addr[1] = uv_data;
    surf->raw_data = staging;
    surf->raw_data_dirty = 1;
    mock_mem_flush_calls = 0;
    mock_mem_flush_result = 0;
    status = hobot_upload_staging_to_gbuf(surf);
    if (status != VA_STATUS_SUCCESS || surf->raw_data_dirty ||
        mock_mem_flush_calls != 2 || y_data[0] != 0x3c || uv_data[0] != 0x3c) {
        fprintf(stderr, "valid separate-FD NV12 upload failed: status=%d flush=%d\n",
                status, mock_mem_flush_calls);
        goto cleanup;
    }

    memset(y_data, 0x21, sizeof(y_data));
    memset(uv_data, 0x42, sizeof(uv_data));
    mock_mem_invalidate_calls = 0;
    mock_mem_invalidate_result = 0;
    status = hobot_copy_gbuf_to_staging(surf);
    if (status != VA_STATUS_SUCCESS || !surf->raw_data ||
        mock_mem_invalidate_calls != 2 ||
        ((uint8_t *)surf->raw_data)[0] != 0x21 ||
        ((uint8_t *)surf->raw_data)[Y_SIZE] != 0x42) {
        fprintf(stderr, "valid separate-FD NV12 read failed: status=%d invalidate=%d\n",
                status, mock_mem_invalidate_calls);
        goto cleanup;
    }

    surf->raw_data_dirty = 0;
    status = hobot_vaExportSurfaceHandle(
        &va_ctx, 1, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
        &descriptor);
    if (status != VA_STATUS_SUCCESS || descriptor.num_objects != 2 ||
        descriptor.objects[0].size != Y_SIZE ||
        descriptor.objects[1].size != UV_SIZE ||
        descriptor.layers[0].object_index[1] != 1 ||
        descriptor.layers[0].offset[1] != 0) {
        fprintf(stderr, "valid separate-FD NV12 export failed: status=%d objects=%u sizes=%u/%u\n",
                status, descriptor.num_objects,
                descriptor.objects[0].size, descriptor.objects[1].size);
        goto cleanup;
    }
    descriptor_exported = 1;
    passed = 1;

cleanup:
    if (descriptor_exported) {
        for (uint32_t i = 0; i < descriptor.num_objects; i++)
            close(descriptor.objects[i].fd);
    }
    free(surf->raw_data == staging ? NULL : surf->raw_data);
    pthread_mutex_destroy(&drv.mutex);
    close(fd1);
    close(fd0);
    return passed;
}

static int test_put_image_lazily_allocates_and_uploads_dma_surface(void)
{
    enum { WIDTH = 64, HEIGHT = 64, DATA_SIZE = WIDTH * HEIGHT * 3 / 2 };
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    uint8_t image_data[DATA_SIZE];
    memset(image_data, 0x5a, sizeof(image_data));

    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    HobotSurface *surf = &drv.surfaces[1];
    surf->allocated = 1;
    surf->width = WIDTH;
    surf->height = HEIGHT;
    surf->stride = WIDTH;
    surf->raw_data_size = DATA_SIZE;
    drv.images[1].allocated = 1;
    drv.images[1].buf_id = 1;
    drv.images[1].image.width = WIDTH;
    drv.images[1].image.height = HEIGHT;
    drv.images[1].image.pitches[0] = WIDTH;
    drv.images[1].image.pitches[1] = WIDTH;
    drv.images[1].image.offsets[1] = WIDTH * HEIGHT;
    drv.images[1].image.data_size = DATA_SIZE;
    drv.buffers[1].allocated = 1;
    drv.buffers[1].data = image_data;
    drv.buffers[1].size = DATA_SIZE;

    mock_graph_alloc_calls = 0;
    mock_graph_alloc_enabled = 0;
    mock_mem_flush_calls = 0;
    mock_mem_flush_result = 0;
    VAStatus status = hobot_vaPutImage(&va_ctx, 1, 1, 0, 0,
                                       WIDTH, HEIGHT, 0, 0, WIDTH, HEIGHT);
    if (status != VA_STATUS_SUCCESS || !surf->raw_data_dirty || surf->has_preallocated) {
        fprintf(stderr, "vaPutImage failed to retain staging before lazy DMA allocation: status=%d dirty=%d allocated=%d\n",
                status, surf->raw_data_dirty, surf->has_preallocated);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    mock_graph_alloc_enabled = 1;
    struct hobot_surface_info info;
    status = hobot_fill_surface_info(&va_ctx, 1, &info);
    int passed = status == VA_STATUS_SUCCESS && surf->has_preallocated &&
                 !surf->raw_data_dirty && mock_graph_alloc_calls == 1 &&
                 mock_mem_flush_calls == 1 && info.virt_addr[0] == mock_graph_data &&
                 info.virt_addr[1] == mock_graph_data + WIDTH * HEIGHT &&
                 memcmp(mock_graph_data, image_data, WIDTH * HEIGHT) == 0 &&
                 memcmp(mock_graph_data + WIDTH * HEIGHT,
                        image_data + WIDTH * HEIGHT, DATA_SIZE - WIDTH * HEIGHT) == 0;
    VAImage derived = {0};
    if (passed) {
        status = hobot_vaDeriveImage(&va_ctx, 1, &derived);
        passed = status == VA_STATUS_SUCCESS && derived.image_id > 0 &&
                 drv.buffers[derived.buf].data == mock_graph_data &&
                 derived.offsets[1] == WIDTH * HEIGHT &&
                 derived.data_size == DATA_SIZE && surf->lock_count == 1;
        if (passed)
            passed = hobot_vaDestroyImage(&va_ctx, derived.image_id) == VA_STATUS_SUCCESS;
    }
    VASurfaceID surface_id = 1;
    if (passed)
        passed = hobot_vaDestroySurfaces(&va_ctx, &surface_id, 1) == VA_STATUS_SUCCESS;
    pthread_mutex_destroy(&drv.mutex);
    mock_graph_alloc_enabled = 0;
    if (!passed)
        fprintf(stderr, "lazy DMA allocation did not receive staged VAImage pixels: status=%d dirty=%d allocations=%d flush=%d\n",
                status, surf->raw_data_dirty, mock_graph_alloc_calls, mock_mem_flush_calls);
    return passed;
}

static int test_put_image_recycles_decoded_surface_before_upload(void)
{
    enum { WIDTH = 64, HEIGHT = 64, Y_SIZE = WIDTH * HEIGHT,
           DATA_SIZE = Y_SIZE + Y_SIZE / 2 };
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    uint8_t image_data[DATA_SIZE];
    memset(image_data, 0, sizeof(image_data));
    memset(image_data, 201, 2);
    memset(image_data + WIDTH, 202, 2);
    image_data[Y_SIZE] = 60;
    image_data[Y_SIZE + 1] = 70;
    memset(mock_graph_data, 33, Y_SIZE);
    memset(mock_graph_data + Y_SIZE, 140, Y_SIZE / 2);

    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    drv.contexts[2].allocated = 1;
    drv.contexts[2].vpu_running = 1;
    HobotSurface *surf = &drv.surfaces[1];
    surf->allocated = 1;
    surf->width = WIDTH;
    surf->height = HEIGHT;
    surf->stride = WIDTH;
    surf->raw_data_size = DATA_SIZE;
    surf->has_decoded_frame = 1;
    surf->output_context_id = 2;
    surf->dma_fd = 8;
    surf->vpu_out_buf.vframe_buf.phy_ptr[0] = 1;
    surf->vpu_out_buf.vframe_buf.size = DATA_SIZE;
    surf->vpu_out_buf.vframe_buf.stride = WIDTH;
    surf->vpu_out_buf.vframe_buf.vstride = HEIGHT;
    surf->vpu_out_buf.vframe_buf.vir_ptr[0] = mock_graph_data;
    surf->vpu_out_buf.vframe_buf.vir_ptr[1] = mock_graph_data + Y_SIZE;
    surf->vpu_out_buf.vframe_buf.fd[0] = 8;
    surf->vpu_out_buf.vframe_buf.compSize[0] = Y_SIZE;
    surf->vpu_out_buf.vframe_buf.compSize[1] = Y_SIZE / 2;
    drv.images[1].allocated = 1;
    drv.images[1].buf_id = 1;
    drv.images[1].image.width = WIDTH;
    drv.images[1].image.height = HEIGHT;
    drv.images[1].image.pitches[0] = WIDTH;
    drv.images[1].image.pitches[1] = WIDTH;
    drv.images[1].image.offsets[1] = Y_SIZE;
    drv.images[1].image.data_size = DATA_SIZE;
    drv.buffers[1].allocated = 1;
    drv.buffers[1].data = image_data;
    drv.buffers[1].size = DATA_SIZE;
    mock_queue_output_calls = 0;
    recycled_output_count = 0;
    mock_queue_output_result = -1;

    VAStatus status = hobot_vaPutImage(&va_ctx, 1, 1, 0, 0, 2, 2, 2, 2, 2, 2);
    int retained_after_failure = status == VA_STATUS_ERROR_OPERATION_FAILED &&
                 mock_queue_output_calls == 2 && recycled_output_count == 0 &&
                 surf->has_decoded_frame && surf->output_context_id == 2 &&
                 surf->raw_data_valid && !surf->raw_data_dirty &&
                 ((uint8_t *)surf->raw_data)[0] == 33 &&
                 ((uint8_t *)surf->raw_data)[Y_SIZE] == 140;
    if (!retained_after_failure) {
        fprintf(stderr, "vaPutImage lost decoded surface state after recycle failure: status=%d queued=%d decoded=%d owner=%u valid=%d dirty=%d\n",
                status, mock_queue_output_calls, surf->has_decoded_frame,
                surf->output_context_id, surf->raw_data_valid, surf->raw_data_dirty);
        if (surf->raw_data)
            free(surf->raw_data);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    mock_queue_output_result = 0;
    status = hobot_vaPutImage(&va_ctx, 1, 1, 0, 0, 2, 2, 2, 2, 2, 2);
    int passed = status == VA_STATUS_SUCCESS && mock_queue_output_calls == 3 &&
                 recycled_output_count == 1 &&
                 mock_last_output_context == &drv.contexts[2].vpu_ctx &&
                 !surf->has_decoded_frame && surf->output_context_id == 0 &&
                 surf->raw_data_dirty && ((uint8_t *)surf->raw_data)[0] == 33 &&
                 ((uint8_t *)surf->raw_data)[2 * WIDTH + 2] == 201 &&
                 ((uint8_t *)surf->raw_data)[3 * WIDTH + 2] == 202 &&
                 ((uint8_t *)surf->raw_data)[Y_SIZE] == 140 &&
                 ((uint8_t *)surf->raw_data)[Y_SIZE + WIDTH + 2] == 60 &&
                 ((uint8_t *)surf->raw_data)[Y_SIZE + WIDTH + 3] == 70;

    surf->has_decoded_frame = 1;
    surf->output_context_id = 2;
    surf->vpu_out_buf.vframe_buf.phy_ptr[0] = 0;
    surf->vpu_out_buf.vframe_buf.size = DATA_SIZE;
    surf->vpu_out_buf.vframe_buf.stride = WIDTH;
    surf->vpu_out_buf.vframe_buf.vstride = HEIGHT;
    surf->vpu_out_buf.vframe_buf.vir_ptr[0] = mock_graph_data;
    surf->vpu_out_buf.vframe_buf.vir_ptr[1] = mock_graph_data + Y_SIZE;
    surf->vpu_out_buf.vframe_buf.compSize[0] = Y_SIZE;
    surf->vpu_out_buf.vframe_buf.compSize[1] = Y_SIZE / 2;
    surf->raw_data_valid = 0;
    status = hobot_vaPutImage(&va_ctx, 1, 1, 0, 0, 2, 2, 2, 2, 2, 2);
    passed = passed && status == VA_STATUS_SUCCESS && mock_queue_output_calls == 3 &&
             recycled_output_count == 1 && !surf->has_decoded_frame &&
             surf->output_context_id == 0 && surf->raw_data_dirty;
    if (surf->raw_data)
        free(surf->raw_data);
    pthread_mutex_destroy(&drv.mutex);
    if (!passed)
        fprintf(stderr, "vaPutImage did not preserve and replace decoded frame safely: status=%d queued=%d decoded=%d owner=%u\n",
                status, mock_queue_output_calls, surf->has_decoded_frame,
                surf->output_context_id);
    return passed;
}

static int test_image_size_and_rectangle_bounds(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    VAImageFormat format = {0};
    format.fourcc = VA_FOURCC_NV12;
    VAImage image = {0};
    VAStatus status = hobot_vaCreateImage(&va_ctx, &format, INT_MAX, INT_MAX, &image);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER || drv.images[1].allocated ||
        drv.buffers[1].allocated) {
        fprintf(stderr, "overflowing image dimensions accepted: status=%d image=%d buffer=%d\n",
                status, drv.images[1].allocated, drv.buffers[1].allocated);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    status = hobot_vaCreateImage(&va_ctx, &format, 65, 64, &image);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER || drv.images[1].allocated ||
        drv.buffers[1].allocated) {
        fprintf(stderr, "odd-width NV12 image accepted: status=%d image=%d buffer=%d\n",
                status, drv.images[1].allocated, drv.buffers[1].allocated);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    status = hobot_vaCreateImage(&va_ctx, &format, 64, 64, &image);
    if (status != VA_STATUS_SUCCESS || image.data_size != 64u * 64u * 3u / 2u ||
        image.buf <= 0 || image.buf >= MAX_BUFFERS || !drv.buffers[image.buf].allocated ||
        hobot_vaDestroyImage(&va_ctx, image.image_id) != VA_STATUS_SUCCESS ||
        drv.images[image.image_id].allocated || drv.buffers[image.buf].allocated) {
        fprintf(stderr, "NV12 image create/destroy path failed: status=%d image=%u buffer=%u\n",
                status, image.image_id, image.buf);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    unsigned char source[24] = {0};
    unsigned char destination[24];
    memset(destination, 0x5a, sizeof(destination));
    drv.surfaces[1].allocated = 1;
    drv.surfaces[1].width = 4;
    drv.surfaces[1].height = 4;
    drv.surfaces[1].stride = 4;
    drv.surfaces[1].raw_data_size = sizeof(destination);
    drv.surfaces[1].raw_data = destination;
    drv.images[1].allocated = 1;
    drv.images[1].image.width = 4;
    drv.images[1].image.height = 4;
    drv.images[1].image.pitches[0] = 4;
    drv.images[1].image.pitches[1] = 4;
    drv.images[1].image.offsets[0] = 0;
    drv.images[1].image.offsets[1] = 16;
    drv.images[1].image.data_size = sizeof(source);
    drv.images[1].buf_id = 1;
    drv.buffers[1].allocated = 1;
    drv.buffers[1].size = sizeof(source);
    drv.buffers[1].capacity = sizeof(source);
    drv.buffers[1].data = source;

    status = hobot_vaPutImage(&va_ctx, 1, 1, INT_MAX - 1, 0,
                              2147483652u, 2, INT_MAX - 1, 0,
                              2147483652u, 2);
    if (status != VA_STATUS_ERROR_INVALID_PARAMETER) {
        fprintf(stderr, "wrapped image rectangle bounds accepted: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    for (size_t i = 0; i < sizeof(destination); i++) {
        if (destination[i] != 0x5a) {
            fprintf(stderr, "invalid image rectangle modified destination\n");
            pthread_mutex_destroy(&drv.mutex);
            return 0;
        }
    }

    drv.buffers[1].size = 8;
    status = hobot_vaPutImage(&va_ctx, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2);
    if (status != VA_STATUS_ERROR_INVALID_BUFFER) {
        fprintf(stderr, "undersized image buffer accepted: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    unsigned char frame_y[16];
    unsigned char frame_uv[8];
    memset(frame_y, 0x11, sizeof(frame_y));
    memset(frame_uv, 0x22, sizeof(frame_uv));
    memset(source, 0x5a, sizeof(source));
    drv.surfaces[1].has_decoded_frame = 1;
    drv.surfaces[1].vpu_out_buf.vframe_buf.vir_ptr[0] = frame_y;
    drv.surfaces[1].vpu_out_buf.vframe_buf.vir_ptr[1] = frame_uv;
    drv.surfaces[1].vpu_out_buf.vframe_buf.stride = 2;
    drv.surfaces[1].vpu_out_buf.vframe_buf.vstride = 4;
    drv.surfaces[1].vpu_out_buf.vframe_buf.size = sizeof(frame_y) + sizeof(frame_uv);
    drv.buffers[1].size = sizeof(source);
    status = hobot_vaGetImage(&va_ctx, 1, 0, 0, 4, 4, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED) {
        fprintf(stderr, "invalid source NV12 stride accepted by vaGetImage: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    for (size_t i = 0; i < sizeof(source); i++) {
        if (source[i] != 0x5a) {
            fprintf(stderr, "invalid vaGetImage source modified its destination\n");
            pthread_mutex_destroy(&drv.mutex);
            return 0;
        }
    }

    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static int test_decoded_nv12_plane_bounds(void)
{
    uint8_t frame[64] = {0};
    HobotSurface surf = {0};
    HobotDecodedNV12Layout layout;

    surf.width = 8;
    surf.height = 4;
    surf.vpu_out_buf.vframe_buf.vir_ptr[0] = frame;
    surf.vpu_out_buf.vframe_buf.vir_ptr[1] = frame + 32;
    surf.vpu_out_buf.vframe_buf.stride = 8;
    surf.vpu_out_buf.vframe_buf.vstride = 8;
    surf.vpu_out_buf.vframe_buf.size = sizeof(frame);
    surf.vpu_out_buf.vframe_buf.compSize[0] = 32;
    surf.vpu_out_buf.vframe_buf.compSize[1] = 32;
    if (hobot_get_decoded_nv12_planes(&surf, &layout) != VA_STATUS_SUCCESS ||
        layout.y_plane != frame || layout.uv_plane != frame + 32 ||
        layout.y_stride != 8 || layout.uv_stride != 8 ||
        layout.vertical_rows != 4 || layout.uv_offset != 32 || !layout.contiguous) {
        fprintf(stderr, "valid decoded NV12 plane layout rejected\n");
        return 0;
    }

    surf.vpu_out_buf.vframe_buf.vir_ptr[1] = frame + 60;
    if (hobot_get_decoded_nv12_planes(&surf, &layout) !=
            VA_STATUS_ERROR_OPERATION_FAILED) {
        fprintf(stderr, "out-of-allocation NV12 UV plane accepted\n");
        return 0;
    }

    surf.vpu_out_buf.vframe_buf.vir_ptr[1] = frame + 16;
    if (hobot_get_decoded_nv12_planes(&surf, &layout) !=
            VA_STATUS_ERROR_OPERATION_FAILED) {
        fprintf(stderr, "overlapping NV12 UV plane accepted\n");
        return 0;
    }

    surf.vpu_out_buf.vframe_buf.vir_ptr[1] = NULL;
    if (hobot_get_decoded_nv12_planes(&surf, &layout) != VA_STATUS_SUCCESS ||
        layout.uv_plane != frame + 32) {
        fprintf(stderr, "valid implicit NV12 UV offset rejected\n");
        return 0;
    }

    surf.vpu_out_buf.vframe_buf.vstride = 12;
    surf.vpu_out_buf.vframe_buf.size = 56;
    surf.vpu_out_buf.vframe_buf.compSize[1] = 24;
    if (hobot_get_decoded_nv12_planes(&surf, &layout) != VA_STATUS_SUCCESS ||
        layout.uv_stride != 12 || layout.vertical_rows != 4 ||
        layout.uv_offset != 32) {
        fprintf(stderr, "independent decoded NV12 chroma pitch was rejected\n");
        return 0;
    }
    return 1;
}

static int test_begin_picture_rejects_stopped_context(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    drv.contexts[1].allocated = 1;
    drv.surfaces[1].allocated = 1;
    drv.contexts[1].current_render_target = VA_INVALID_SURFACE;

    VAStatus status = hobot_vaBeginPicture(&va_ctx, 1, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED ||
        drv.surfaces[1].decode_pending || drv.surfaces[1].context_id != 0 ||
        drv.contexts[1].decode_picture_active || drv.contexts[1].sub_tail != 0 ||
        drv.contexts[1].current_render_target != VA_INVALID_SURFACE) {
        fprintf(stderr, "stopped context mutated begin-picture state: status=%d pending=%d active=%d tail=%u\n",
                status, drv.surfaces[1].decode_pending,
                drv.contexts[1].decode_picture_active, drv.contexts[1].sub_tail);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

static int test_config_capabilities_match_encoder(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    struct VADriverContext empty_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    unsigned int surface_attr_count = 0;
    if (hobot_vaQuerySurfaceAttributes(NULL, 0, NULL, &surface_attr_count) !=
            VA_STATUS_ERROR_INVALID_CONTEXT ||
        hobot_vaQuerySurfaceAttributes(&empty_ctx, 0, NULL,
                                       &surface_attr_count) !=
            VA_STATUS_ERROR_INVALID_CONTEXT) {
        fprintf(stderr, "surface-attribute query did not reject missing driver context\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    VAConfigID missing_context_config = VA_INVALID_ID;
    VAContextID missing_context_id = VA_INVALID_ID;
    VASurfaceID missing_context_surface = 1;
    VABufferID missing_context_buffer = 1;
    int missing_context_num_attribs = 0;
    if (hobot_vaCreateConfig(NULL, VAProfileH264High, VAEntrypointVLD,
                             NULL, 0, &missing_context_config) !=
            VA_STATUS_ERROR_INVALID_CONTEXT ||
        hobot_vaDestroyConfig(&empty_ctx, 1) != VA_STATUS_ERROR_INVALID_CONTEXT ||
        hobot_vaQueryConfigAttributes(NULL, 1, NULL, NULL, NULL,
                                      &missing_context_num_attribs) !=
            VA_STATUS_ERROR_INVALID_CONTEXT ||
        hobot_vaDestroySurfaces(&empty_ctx, &missing_context_surface, 1) !=
            VA_STATUS_ERROR_INVALID_CONTEXT ||
        hobot_vaCreateContext(NULL, 1, 640, 360, 0, NULL, 0,
                              &missing_context_id) !=
            VA_STATUS_ERROR_INVALID_CONTEXT ||
        hobot_vaCreateContext(&empty_ctx, 1, 640, 360, 0, NULL, 0,
                              &missing_context_id) !=
            VA_STATUS_ERROR_INVALID_CONTEXT ||
        hobot_vaDestroyContext(NULL, 1) != VA_STATUS_ERROR_INVALID_CONTEXT ||
        hobot_vaBeginPicture(&empty_ctx, 1, 1) != VA_STATUS_ERROR_INVALID_CONTEXT ||
        hobot_vaRenderPicture(NULL, 1, &missing_context_buffer, 1) !=
            VA_STATUS_ERROR_INVALID_CONTEXT ||
        hobot_vaEndPicture(&empty_ctx, 1) != VA_STATUS_ERROR_INVALID_CONTEXT ||
        missing_context_config != VA_INVALID_ID ||
        missing_context_id != VA_INVALID_ID) {
        fprintf(stderr, "VA callbacks did not reject a missing driver context\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    VAConfigAttrib attrs[] = {
        { .type = VAConfigAttribRTFormat },
        { .type = VAConfigAttribRateControl },
        { .type = VAConfigAttribEncPackedHeaders },
        { .type = VAConfigAttribEncMaxRefFrames },
        { .type = VAConfigAttribPredictionDirection },
        { .type = VAConfigAttribEncMaxSlices },
        { .type = VAConfigAttribEncSliceStructure },
        { .type = VAConfigAttribEncQualityRange }
    };
    VAStatus status = hobot_vaGetConfigAttributes(&va_ctx, VAProfileH264High,
                                                   VAEntrypointEncSlice,
                                                   attrs, sizeof(attrs) / sizeof(attrs[0]));
    if (status != VA_STATUS_SUCCESS || attrs[0].value != VA_RT_FORMAT_YUV420 ||
        attrs[1].value != (VA_RC_CBR | VA_RC_VBR | VA_RC_CQP) ||
        attrs[2].value != VA_ENC_PACKED_HEADER_NONE ||
        attrs[3].value != 1 ||
        attrs[4].value != VA_PREDICTION_DIRECTION_PREVIOUS ||
        attrs[5].value != 1 ||
        attrs[6].value != VA_ATTRIB_NOT_SUPPORTED ||
        attrs[7].value != VA_ATTRIB_NOT_SUPPORTED) {
        fprintf(stderr, "H.264 encoder capabilities are inaccurate: status=%d rt=%u rc=%u packed=%u refs=%u direction=%u max_slices=%u slice=%u quality=%u\n",
                status, attrs[0].value, attrs[1].value, attrs[2].value,
                attrs[3].value, attrs[4].value, attrs[5].value, attrs[6].value,
                attrs[7].value);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    VAConfigAttrib hevc_limits[] = {
        { .type = VAConfigAttribMaxPictureWidth },
        { .type = VAConfigAttribMaxPictureHeight }
    };
    status = hobot_vaGetConfigAttributes(&va_ctx, VAProfileHEVCMain,
                                          VAEntrypointVLD, hevc_limits, 2);
    if (status != VA_STATUS_SUCCESS || hevc_limits[0].value != 8192 ||
        hevc_limits[1].value != 4096) {
        fprintf(stderr, "HEVC picture-size capabilities are inaccurate: status=%d width=%u height=%u\n",
                status, hevc_limits[0].value, hevc_limits[1].value);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    VAConfigAttrib hevc_vld_create_attrs[] = {
        { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 }
    };
    VAConfigID hevc_vld_config = VA_INVALID_ID;
    if (hobot_vaCreateConfig(&va_ctx, VAProfileHEVCMain, VAEntrypointVLD,
                             hevc_vld_create_attrs, 1, &hevc_vld_config) !=
            VA_STATUS_SUCCESS) {
        fprintf(stderr, "HEVC Main VLD config could not be created for surface query\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    VASurfaceAttrib hevc_surface_attrs[7] = {0};
    unsigned int hevc_surface_attr_count =
        sizeof(hevc_surface_attrs) / sizeof(hevc_surface_attrs[0]);
    status = hobot_vaQuerySurfaceAttributes(&va_ctx, hevc_vld_config,
                                             hevc_surface_attrs,
                                             &hevc_surface_attr_count);
    unsigned int hevc_surface_max_width = 0;
    unsigned int hevc_surface_max_height = 0;
    for (unsigned int i = 0; i < hevc_surface_attr_count; i++) {
        if (hevc_surface_attrs[i].type == VASurfaceAttribMaxWidth)
            hevc_surface_max_width = hevc_surface_attrs[i].value.value.i;
        else if (hevc_surface_attrs[i].type == VASurfaceAttribMaxHeight)
            hevc_surface_max_height = hevc_surface_attrs[i].value.value.i;
    }
    VAStatus hevc_vld_destroy_status =
        hobot_vaDestroyConfig(&va_ctx, hevc_vld_config);
    if (status != VA_STATUS_SUCCESS || hevc_surface_max_width != 8192 ||
        hevc_surface_max_height != 4096 ||
        hevc_vld_destroy_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "HEVC surface-size capabilities mismatch: status=%d width=%u height=%u\n",
                status, hevc_surface_max_width, hevc_surface_max_height);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    VAConfigAttrib h264_vld_create_attrs[] = {
        { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 }
    };
    VAConfigID h264_vld_config = VA_INVALID_ID;
    if (hobot_vaCreateConfig(&va_ctx, VAProfileH264High, VAEntrypointVLD,
                             h264_vld_create_attrs, 1, &h264_vld_config) !=
            VA_STATUS_SUCCESS) {
        fprintf(stderr, "H.264 VLD config could not be created for surface query\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    VASurfaceAttrib h264_surface_attrs[7] = {0};
    unsigned int h264_surface_attr_count =
        sizeof(h264_surface_attrs) / sizeof(h264_surface_attrs[0]);
    status = hobot_vaQuerySurfaceAttributes(&va_ctx, h264_vld_config,
                                             h264_surface_attrs,
                                             &h264_surface_attr_count);
    unsigned int h264_surface_max_width = 0;
    unsigned int h264_surface_max_height = 0;
    for (unsigned int i = 0; i < h264_surface_attr_count; i++) {
        if (h264_surface_attrs[i].type == VASurfaceAttribMaxWidth)
            h264_surface_max_width = h264_surface_attrs[i].value.value.i;
        else if (h264_surface_attrs[i].type == VASurfaceAttribMaxHeight)
            h264_surface_max_height = h264_surface_attrs[i].value.value.i;
    }
    VAStatus h264_vld_destroy_status =
        hobot_vaDestroyConfig(&va_ctx, h264_vld_config);
    if (status != VA_STATUS_SUCCESS || h264_surface_max_width != 4096 ||
        h264_surface_max_height != 4096 ||
        h264_vld_destroy_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "H.264 surface-size capabilities mismatch: status=%d width=%u height=%u\n",
                status, h264_surface_max_width, h264_surface_max_height);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    int hevc_entrypoint_count = 0;
    VAEntrypoint hevc_entrypoints[2] = {VAEntrypointEncPicture, VAEntrypointVLD};
    status = hobot_vaQueryConfigEntrypoints(&va_ctx, VAProfileHEVCMain,
                                             hevc_entrypoints,
                                             &hevc_entrypoint_count);
    VAConfigAttrib hevc_encode_attrs[] = {
        { .type = VAConfigAttribRTFormat },
        { .type = VAConfigAttribRateControl },
        { .type = VAConfigAttribEncMaxRefFrames },
        { .type = VAConfigAttribEncMaxSlices },
        { .type = VAConfigAttribEncPackedHeaders },
        { .type = VAConfigAttribPredictionDirection }
    };
    VAStatus hevc_attr_status = hobot_vaGetConfigAttributes(
        &va_ctx, VAProfileHEVCMain, VAEntrypointEncSlice,
        hevc_encode_attrs, sizeof(hevc_encode_attrs) / sizeof(hevc_encode_attrs[0]));
    if (status != VA_STATUS_SUCCESS || hevc_entrypoint_count != 2 ||
        hevc_entrypoints[0] != VAEntrypointVLD ||
        hevc_entrypoints[1] != VAEntrypointEncSlice ||
        hevc_attr_status != VA_STATUS_SUCCESS ||
        hevc_encode_attrs[0].value != VA_RT_FORMAT_YUV420 ||
        hevc_encode_attrs[1].value != (VA_RC_CBR | VA_RC_VBR | VA_RC_CQP) ||
        hevc_encode_attrs[2].value != 1 || hevc_encode_attrs[3].value != 1 ||
        hevc_encode_attrs[4].value != VA_ENC_PACKED_HEADER_NONE ||
        hevc_encode_attrs[5].value != VA_PREDICTION_DIRECTION_PREVIOUS) {
        fprintf(stderr, "HEVC Main encode capabilities are inaccurate: entrypoints=%d status=%d/%d attrs=%u/%u/%u/%u/%u/%u\n",
                hevc_entrypoint_count, status, hevc_attr_status,
                hevc_encode_attrs[0].value, hevc_encode_attrs[1].value,
                hevc_encode_attrs[2].value, hevc_encode_attrs[3].value,
                hevc_encode_attrs[4].value, hevc_encode_attrs[5].value);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    VAConfigAttrib hevc_create_attrs[] = {
        { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl, .value = VA_RC_CBR }
    };
    VAConfigAttrib oversized_config_attrs[MAX_CONFIG_ATTRIBUTES + 1] = {0};
    VAConfigID oversized_config = VA_INVALID_ID;
    if (hobot_vaCreateConfig(&va_ctx, VAProfileHEVCMain,
                             VAEntrypointEncSlice,
                             oversized_config_attrs,
                             MAX_CONFIG_ATTRIBUTES + 1,
                             &oversized_config) !=
            VA_STATUS_ERROR_INVALID_PARAMETER ||
        oversized_config != VA_INVALID_ID) {
        fprintf(stderr, "oversized config attribute list was not rejected\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    VAConfigID hevc_config = VA_INVALID_ID;
    if (hobot_vaCreateConfig(&va_ctx, VAProfileHEVCMain, VAEntrypointEncSlice,
                             hevc_create_attrs, 2, &hevc_config) !=
            VA_STATUS_SUCCESS || hevc_config == VA_INVALID_ID ||
        !drv.configs[hevc_config].allocated ||
        drv.configs[hevc_config].profile != VAProfileHEVCMain ||
        drv.configs[hevc_config].entrypoint != VAEntrypointEncSlice) {
        fprintf(stderr, "HEVC Main CBR encode config was rejected\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    VAProfile queried_config_profile = VAProfileNone;
    VAEntrypoint queried_config_entrypoint = VAEntrypointVLD;
    VAConfigAttrib queried_config_attrs[MAX_CONFIG_ATTRIBUTES] = {0};
    int queried_config_attr_count = MAX_CONFIG_ATTRIBUTES;
    status = hobot_vaQueryConfigAttributes(&va_ctx, hevc_config,
                                            &queried_config_profile,
                                            &queried_config_entrypoint,
                                            queried_config_attrs,
                                            &queried_config_attr_count);
    VAStatus hevc_config_destroy_status =
        hobot_vaDestroyConfig(&va_ctx, hevc_config);
    if (status != VA_STATUS_SUCCESS ||
        queried_config_profile != VAProfileHEVCMain ||
        queried_config_entrypoint != VAEntrypointEncSlice ||
        queried_config_attr_count != 2 ||
        queried_config_attrs[0].type != VAConfigAttribRTFormat ||
        queried_config_attrs[0].value != VA_RT_FORMAT_YUV420 ||
        queried_config_attrs[1].type != VAConfigAttribRateControl ||
        queried_config_attrs[1].value != VA_RC_CBR ||
        hevc_config_destroy_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "HEVC config query did not return its stored attributes: status=%d profile=%d entrypoint=%d count=%d\n",
                status, queried_config_profile, queried_config_entrypoint,
                queried_config_attr_count);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    VAConfigAttrib hevc_cqp_create_attrs[] = {
        { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl, .value = VA_RC_CQP }
    };
    VAConfigID hevc_cqp_config = VA_INVALID_ID;
    if (hobot_vaCreateConfig(&va_ctx, VAProfileHEVCMain,
                             VAEntrypointEncSlice, hevc_cqp_create_attrs, 2,
                             &hevc_cqp_config) != VA_STATUS_SUCCESS ||
        hevc_cqp_config == VA_INVALID_ID ||
        hobot_vaQueryConfigAttributes(&va_ctx, hevc_cqp_config,
                                      &queried_config_profile,
                                      &queried_config_entrypoint,
                                      queried_config_attrs,
                                      &queried_config_attr_count) !=
            VA_STATUS_SUCCESS ||
        queried_config_profile != VAProfileHEVCMain ||
        queried_config_entrypoint != VAEntrypointEncSlice ||
        queried_config_attr_count != 2 ||
        queried_config_attrs[0].type != VAConfigAttribRTFormat ||
        queried_config_attrs[0].value != VA_RT_FORMAT_YUV420 ||
        queried_config_attrs[1].type != VAConfigAttribRateControl ||
        queried_config_attrs[1].value != VA_RC_CQP ||
        hobot_vaDestroyConfig(&va_ctx, hevc_cqp_config) !=
            VA_STATUS_SUCCESS) {
        fprintf(stderr, "HEVC Main CQP encode config create/query/destroy failed\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    attrs[0].value = 0;
    attrs[1].value = 0;
    status = hobot_vaGetConfigAttributes(&va_ctx, VAProfileH264High,
                                          VAEntrypointEncPicture, attrs, 2);
    if (status != VA_STATUS_SUCCESS || attrs[0].value != VA_ATTRIB_NOT_SUPPORTED ||
        attrs[1].value != VA_ATTRIB_NOT_SUPPORTED) {
        fprintf(stderr, "unsupported entrypoint advertised config capabilities\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    VAConfigAttrib profile_check_attr = {
        .type = VAConfigAttribRTFormat,
        .value = VA_RT_FORMAT_YUV420
    };
    VAConfigID unsupported_profile_config = VA_INVALID_ID;
    if (hobot_vaCreateConfig(&va_ctx, VAProfileHEVCMain10, VAEntrypointVLD,
                             &profile_check_attr, 1,
                             &unsupported_profile_config) !=
            VA_STATUS_ERROR_UNSUPPORTED_PROFILE ||
        unsupported_profile_config != VA_INVALID_ID) {
        fprintf(stderr, "unsupported profile returned the wrong config error\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    VAConfigID unsupported_entrypoint_config = VA_INVALID_ID;
    if (hobot_vaCreateConfig(&va_ctx, VAProfileH264High,
                             VAEntrypointEncPicture, &profile_check_attr, 1,
                             &unsupported_entrypoint_config) !=
            VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT ||
        unsupported_entrypoint_config != VA_INVALID_ID) {
        fprintf(stderr, "unsupported entrypoint returned the wrong config error\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    int cb_entrypoint_count = 0;
    VAEntrypoint cb_entrypoints[2] = {VAEntrypointEncPicture, VAEntrypointEncPicture};
    status = hobot_vaQueryConfigEntrypoints(&va_ctx,
        VAProfileH264ConstrainedBaseline, NULL, &cb_entrypoint_count);
    if (status != VA_STATUS_SUCCESS || cb_entrypoint_count != 2 ||
        hobot_vaQueryConfigEntrypoints(&va_ctx, VAProfileH264ConstrainedBaseline,
                                       cb_entrypoints, &cb_entrypoint_count) !=
            VA_STATUS_SUCCESS || cb_entrypoint_count != 2 ||
        cb_entrypoints[0] != VAEntrypointVLD ||
        cb_entrypoints[1] != VAEntrypointEncSlice) {
        fprintf(stderr, "Constrained Baseline entrypoints were not advertised\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    VAConfigAttrib cb_encoder_attrs[] = {
        { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl, .value = VA_RC_CBR }
    };
    VAConfigID cb_encoder_config = VA_INVALID_ID;
    status = hobot_vaGetConfigAttributes(&va_ctx, VAProfileH264ConstrainedBaseline,
                                          VAEntrypointEncSlice, cb_encoder_attrs, 2);
    if (status != VA_STATUS_SUCCESS ||
        cb_encoder_attrs[0].value != VA_RT_FORMAT_YUV420 ||
        cb_encoder_attrs[1].value != (VA_RC_CBR | VA_RC_VBR | VA_RC_CQP)) {
        fprintf(stderr, "Constrained Baseline encoder capabilities are inaccurate\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    cb_encoder_attrs[1].value = VA_RC_CBR;
    if (
        hobot_vaCreateConfig(&va_ctx, VAProfileH264ConstrainedBaseline,
                             VAEntrypointEncSlice, cb_encoder_attrs, 2,
                             &cb_encoder_config) != VA_STATUS_SUCCESS ||
        cb_encoder_config == VA_INVALID_ID ||
        hobot_vaDestroyConfig(&va_ctx, cb_encoder_config) != VA_STATUS_SUCCESS) {
        fprintf(stderr, "Constrained Baseline encoder config was rejected\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    VAConfigAttrib jpeg_rc = { .type = VAConfigAttribRateControl };
    status = hobot_vaGetConfigAttributes(&va_ctx, VAProfileJPEGBaseline,
                                          VAEntrypointEncPicture, &jpeg_rc, 1);
    if (status != VA_STATUS_SUCCESS || jpeg_rc.value != VA_RC_CQP ||
        hobot_vaGetConfigAttributes(&va_ctx, VAProfileH264High,
                                    VAEntrypointEncSlice, NULL, 1) !=
            VA_STATUS_ERROR_INVALID_PARAMETER ||
        hobot_vaGetConfigAttributes(&va_ctx, VAProfileH264High,
                                    VAEntrypointEncSlice, NULL, -1) !=
            VA_STATUS_ERROR_INVALID_PARAMETER) {
        fprintf(stderr, "JPEG/query parameter capability contract failed: status=%d rc=%u\n",
                status, jpeg_rc.value);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    int jpeg_entrypoint_count = 0;
    status = hobot_vaQueryConfigEntrypoints(&va_ctx, VAProfileJPEGBaseline,
                                             NULL, &jpeg_entrypoint_count);
    VAEntrypoint jpeg_entrypoints[2] = {VAEntrypointEncPicture, VAEntrypointVLD};
    if (status != VA_STATUS_SUCCESS || jpeg_entrypoint_count != 2 ||
        hobot_vaQueryConfigEntrypoints(&va_ctx, VAProfileJPEGBaseline,
                                       jpeg_entrypoints, &jpeg_entrypoint_count) !=
            VA_STATUS_SUCCESS || jpeg_entrypoint_count != 2 ||
        jpeg_entrypoints[0] != VAEntrypointVLD ||
        jpeg_entrypoints[1] != VAEntrypointEncPicture) {
        fprintf(stderr, "JPEG decode/encode entrypoints are inaccurate: status=%d count=%d first=%d second=%d\n",
                status, jpeg_entrypoint_count, jpeg_entrypoints[0], jpeg_entrypoints[1]);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    VAConfigAttrib jpeg_decode_attrs[] = {
        { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribDecJPEG, .value = 0 }
    };
    status = hobot_vaGetConfigAttributes(&va_ctx, VAProfileJPEGBaseline,
                                          VAEntrypointVLD, jpeg_decode_attrs, 2);
    const unsigned int jpeg_rotation_mask =
        (1u << (VA_ROTATION_270 + 1u)) - 1u;
    if (status != VA_STATUS_SUCCESS ||
        jpeg_decode_attrs[0].value != VA_RT_FORMAT_YUV420 ||
        jpeg_decode_attrs[1].value != jpeg_rotation_mask) {
        fprintf(stderr, "JPEG VLD capabilities are inaccurate: status=%d rt=%u decode=%u\n",
                status, jpeg_decode_attrs[0].value, jpeg_decode_attrs[1].value);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    VAConfigID read_only_jpeg_config = VA_INVALID_ID;
    if (hobot_vaCreateConfig(&va_ctx, VAProfileJPEGBaseline,
                             VAEntrypointVLD, &jpeg_decode_attrs[1], 1,
                             &read_only_jpeg_config) !=
            VA_STATUS_ERROR_ATTR_NOT_SUPPORTED ||
        read_only_jpeg_config != VA_INVALID_ID) {
        fprintf(stderr, "read-only JPEG capability was accepted as a config input\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    VAConfigID jpeg_decode_config = VA_INVALID_ID;
    if (hobot_vaCreateConfig(&va_ctx, VAProfileJPEGBaseline,
                             VAEntrypointVLD, &jpeg_decode_attrs[0], 1,
                             &jpeg_decode_config) != VA_STATUS_SUCCESS ||
        jpeg_decode_config == VA_INVALID_ID ||
        !drv.configs[jpeg_decode_config].allocated ||
        drv.configs[jpeg_decode_config].entrypoint != VAEntrypointVLD ||
        hobot_vaDestroyConfig(&va_ctx, jpeg_decode_config) != VA_STATUS_SUCCESS) {
        fprintf(stderr, "JPEG VLD config rejected its writable RT format\n");
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    static const struct {
        VAProfile profile;
        VAEntrypoint entrypoint;
        VAConfigAttrib attrib;
    } read_only_attributes[] = {
        {VAProfileJPEGBaseline, VAEntrypointVLD,
         {.type = VAConfigAttribDecJPEG, .value = 0xf}},
        {VAProfileH264High, VAEntrypointEncSlice,
         {.type = VAConfigAttribEncMaxRefFrames, .value = 1}},
        {VAProfileH264High, VAEntrypointEncSlice,
         {.type = VAConfigAttribEncMaxSlices, .value = 1}},
        {VAProfileH264High, VAEntrypointEncSlice,
         {.type = VAConfigAttribPredictionDirection,
          .value = VA_PREDICTION_DIRECTION_PREVIOUS}},
        {VAProfileH264High, VAEntrypointEncSlice,
         {.type = VAConfigAttribEncQuantization,
          .value = VA_ENC_QUANTIZATION_NONE}},
        {VAProfileH264High, VAEntrypointEncSlice,
         {.type = VAConfigAttribEncIntraRefresh,
          .value = VA_ENC_INTRA_REFRESH_NONE}}
    };
    for (size_t i = 0;
         i < sizeof(read_only_attributes) / sizeof(read_only_attributes[0]); i++) {
        VAConfigID read_only_config = VA_INVALID_ID;
        VAConfigAttrib input = read_only_attributes[i].attrib;
        if (hobot_vaCreateConfig(&va_ctx, read_only_attributes[i].profile,
                                 read_only_attributes[i].entrypoint,
                                 &input, 1, &read_only_config) !=
                VA_STATUS_ERROR_ATTR_NOT_SUPPORTED ||
            read_only_config != VA_INVALID_ID) {
            fprintf(stderr, "read-only config attribute %d was accepted\n",
                    read_only_attributes[i].attrib.type);
            pthread_mutex_destroy(&drv.mutex);
            return 0;
        }
    }

    VAConfigAttrib create_attrs[] = {
        { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 },
        { .type = VAConfigAttribRateControl, .value = VA_RC_CBR }
    };
    VAConfigID config = VA_INVALID_ID;
    status = hobot_vaCreateConfig(&va_ctx, VAProfileH264High, VAEntrypointEncSlice,
                                  create_attrs, 2, &config);
    if (status != VA_STATUS_SUCCESS || !drv.configs[config].allocated ||
        drv.configs[config].rate_control != VA_RC_CBR) {
        fprintf(stderr, "supported H.264 CBR config rejected: status=%d id=%u\n",
                status, config);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }
    if (hobot_vaDestroyConfig(&va_ctx, config) != VA_STATUS_SUCCESS) {
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    create_attrs[1].value = VA_RC_CQP;
    status = hobot_vaCreateConfig(&va_ctx, VAProfileH264High,
                                  VAEntrypointEncSlice, create_attrs, 2,
                                  &config);
    if (status != VA_STATUS_SUCCESS || !drv.configs[config].allocated ||
        drv.configs[config].rate_control != VA_RC_CQP ||
        hobot_vaDestroyConfig(&va_ctx, config) != VA_STATUS_SUCCESS) {
        fprintf(stderr, "supported H.264 CQP config rejected: status=%d id=%u\n",
                status, config);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    create_attrs[1].value = VA_RC_VBR;
    status = hobot_vaCreateConfig(&va_ctx, VAProfileH264High, VAEntrypointEncSlice,
                                  create_attrs, 2, &config);
    if (status != VA_STATUS_SUCCESS || config == VA_INVALID_ID ||
        !drv.configs[config].allocated || drv.configs[config].rate_control != VA_RC_VBR ||
        hobot_vaDestroyConfig(&va_ctx, config) != VA_STATUS_SUCCESS) {
        fprintf(stderr, "supported H.264 VBR config rejected: status=%d id=%u\n",
                status, config);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    create_attrs[1].value = VA_RC_AVBR;
    status = hobot_vaCreateConfig(&va_ctx, VAProfileH264High, VAEntrypointEncSlice,
                                  create_attrs, 2, &config);
    if (status != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED || drv.configs[1].allocated) {
        fprintf(stderr, "unsupported H.264 AVBR config accepted: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    create_attrs[0].value = VA_RT_FORMAT_YUV420_10;
    create_attrs[1].value = VA_RC_CBR;
    status = hobot_vaCreateConfig(&va_ctx, VAProfileH264High, VAEntrypointEncSlice,
                                  create_attrs, 2, &config);
    if (status != VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT || drv.configs[1].allocated) {
        fprintf(stderr, "unsupported H.264 RT format accepted: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    VAConfigAttrib unsupported_attr = {
        .type = VAConfigAttribEncQualityRange,
        .value = 1
    };
    status = hobot_vaCreateConfig(&va_ctx, VAProfileH264High, VAEntrypointEncSlice,
                                  &unsupported_attr, 1, &config);
    if (status != VA_STATUS_ERROR_ATTR_NOT_SUPPORTED || drv.configs[1].allocated) {
        fprintf(stderr, "unsupported encoder config attribute accepted: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 0;
    }

    pthread_mutex_destroy(&drv.mutex);
    return 1;
}

enum {
    SYNC_ISOLATION_SYNC,
    SYNC_ISOLATION_SYNC_WAITER,
    SYNC_ISOLATION_QUERY,
    SYNC_ISOLATION_BEGIN,
    SYNC_ISOLATION_SYNC2,
    SYNC_ISOLATION_SYNC_BUFFER
};

static void *sync_isolation_worker(void *opaque)
{
    SyncIsolationWorker *worker = opaque;
    pthread_mutex_lock(&mock_dequeue_gate_mutex);
    worker->started = 1;
    pthread_cond_broadcast(&mock_dequeue_gate_cond);
    pthread_mutex_unlock(&mock_dequeue_gate_mutex);

    if (worker->operation == SYNC_ISOLATION_SYNC2) {
        worker->status = hobot_vaSyncSurface2(worker->ctx, worker->surface,
                                               worker->timeout_ns);
    } else if (worker->operation == SYNC_ISOLATION_SYNC_BUFFER) {
        worker->status = hobot_vaSyncBuffer(worker->ctx, worker->buffer,
                                            worker->timeout_ns);
    } else if (worker->operation == SYNC_ISOLATION_SYNC ||
        worker->operation == SYNC_ISOLATION_SYNC_WAITER) {
        worker->status = hobot_vaSyncSurface(worker->ctx, worker->surface);
    } else if (worker->operation == SYNC_ISOLATION_QUERY) {
        worker->status = hobot_vaQuerySurfaceStatus(worker->ctx, worker->surface,
                                                     &worker->surface_status);
    } else {
        worker->status = hobot_vaBeginPicture(worker->ctx, worker->context,
                                               worker->surface);
    }
    pthread_mutex_lock(&mock_dequeue_gate_mutex);
    worker->completed = 1;
    pthread_cond_broadcast(&mock_dequeue_gate_cond);
    pthread_mutex_unlock(&mock_dequeue_gate_mutex);
    return NULL;
}

static int wait_for_gate_flag(int *flag, int timeout_ms)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&mock_dequeue_gate_mutex);
    int wait_status = 0;
    while (!*flag && wait_status != ETIMEDOUT)
        wait_status = pthread_cond_timedwait(&mock_dequeue_gate_cond,
                                            &mock_dequeue_gate_mutex, &deadline);
    int result = *flag;
    pthread_mutex_unlock(&mock_dequeue_gate_mutex);
    return result;
}

static int test_sync_does_not_block_unrelated_context(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    pthread_t sync_thread;
    pthread_t sync_waiter_thread;
    pthread_t query_thread;
    pthread_t begin_thread;
    int sync_started = 0;
    int sync_waiter_started = 0;
    int query_started = 0;
    int begin_started = 0;
    int sync_waiter_blocked = 0;
    int query_unblocked = 0;
    int begin_waited_for_sync = 0;
    VAStatus destroy_context_status = VA_STATUS_SUCCESS;
    VAStatus destroy_surface_status = VA_STATUS_SUCCESS;
    SyncIsolationWorker sync = {0};
    SyncIsolationWorker sync_waiter = {0};
    SyncIsolationWorker query = {0};
    SyncIsolationWorker begin = {0};

    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->current_render_target = VA_INVALID_SURFACE;
    hctx->submitted_surfaces[hctx->sub_tail++] = 1;
    drv.surfaces[1].allocated = 1;
    drv.surfaces[1].context_id = 1;
    drv.surfaces[1].decode_pending = 1;
    drv.surfaces[2].allocated = 1;
    drv.surfaces[3].allocated = 1;

    mock_dequeue_output_calls = 0;
    mock_dequeue_result = 0;
    mock_dequeue_output_invalid = 0;
    mock_err_mb = 0;
    pthread_mutex_lock(&mock_dequeue_gate_mutex);
    mock_dequeue_block_enabled = 1;
    mock_dequeue_entered = 0;
    mock_dequeue_release = 0;
    pthread_mutex_unlock(&mock_dequeue_gate_mutex);

    sync.ctx = &va_ctx;
    sync.operation = SYNC_ISOLATION_SYNC;
    sync.surface = 1;
    if (pthread_create(&sync_thread, NULL, sync_isolation_worker, &sync) == 0)
        sync_started = 1;
    if (!sync_started || !wait_for_gate_flag(&mock_dequeue_entered, 2000))
        goto release_and_join;

    sync_waiter.ctx = &va_ctx;
    sync_waiter.operation = SYNC_ISOLATION_SYNC_WAITER;
    sync_waiter.surface = 1;
    if (pthread_create(&sync_waiter_thread, NULL, sync_isolation_worker,
                       &sync_waiter) == 0)
        sync_waiter_started = 1;
    if (!sync_waiter_started)
        goto release_and_join;
    sync_waiter_blocked = !wait_for_gate_flag(&sync_waiter.completed, 100);

    destroy_context_status = hobot_vaDestroyContext(&va_ctx, 1);
    VASurfaceID active_surface = 1;
    destroy_surface_status = hobot_vaDestroySurfaces(&va_ctx, &active_surface, 1);

    query.ctx = &va_ctx;
    query.operation = SYNC_ISOLATION_QUERY;
    query.surface = 2;
    if (pthread_create(&query_thread, NULL, sync_isolation_worker, &query) == 0)
        query_started = 1;
    begin.ctx = &va_ctx;
    begin.operation = SYNC_ISOLATION_BEGIN;
    begin.context = 1;
    begin.surface = 3;
    if (pthread_create(&begin_thread, NULL, sync_isolation_worker, &begin) == 0)
        begin_started = 1;
    if (!query_started || !begin_started)
        goto release_and_join;

    query_unblocked = wait_for_gate_flag(&query.completed, 100);
    begin_waited_for_sync = !wait_for_gate_flag(&begin.completed, 100);

release_and_join:
    pthread_mutex_lock(&mock_dequeue_gate_mutex);
    mock_dequeue_release = 1;
    pthread_cond_broadcast(&mock_dequeue_gate_cond);
    pthread_mutex_unlock(&mock_dequeue_gate_mutex);
    if (sync_started) pthread_join(sync_thread, NULL);
    if (sync_waiter_started) pthread_join(sync_waiter_thread, NULL);
    if (query_started) pthread_join(query_thread, NULL);
    if (begin_started) pthread_join(begin_thread, NULL);

    pthread_mutex_lock(&drv.mutex);
    if (hctx->decode_picture_active)
        hobot_abort_pending_decode_picture(&drv, hctx);
    pthread_mutex_unlock(&drv.mutex);

    pthread_mutex_lock(&mock_dequeue_gate_mutex);
    mock_dequeue_block_enabled = 0;
    mock_dequeue_entered = 0;
    mock_dequeue_release = 0;
    pthread_mutex_unlock(&mock_dequeue_gate_mutex);

    int passed = sync_started && sync_waiter_started && query_started && begin_started &&
                 sync_waiter_blocked && sync_waiter.status == VA_STATUS_SUCCESS &&
                 query_unblocked && begin_waited_for_sync &&
                 begin.status == VA_STATUS_SUCCESS &&
                 destroy_context_status == VA_STATUS_ERROR_OPERATION_FAILED &&
                 destroy_surface_status == VA_STATUS_ERROR_SURFACE_BUSY &&
                 query.status == VA_STATUS_SUCCESS &&
                 query.surface_status == VASurfaceReady &&
                 sync.status == VA_STATUS_SUCCESS &&
                 !drv.surfaces[3].decode_pending &&
                 !hctx->decode_picture_active && mock_dequeue_output_calls == 1;
    if (!passed)
        fprintf(stderr, "sync isolation failed: started=%d/%d/%d/%d waiter_blocked=%d early=%d begin_waited=%d destroy=%d/%d query_status=%d begin_status=%d sync_status=%d waiter_status=%d calls=%d\n",
                sync_started, sync_waiter_started, query_started, begin_started,
                sync_waiter_blocked, query_unblocked,
                begin_waited_for_sync, destroy_context_status, destroy_surface_status,
                query.status, begin.status,
                sync.status, sync_waiter.status, mock_dequeue_output_calls);
    if (drv.sync_cond_initialized)
        pthread_cond_destroy(&drv.sync_cond);
    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_va_sync_surface2_timeout_and_completion(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->vpu_running = 1;
    hctx->submitted_surfaces[hctx->sub_tail++] = 1;
    drv.surfaces[1].allocated = 1;
    drv.surfaces[1].context_id = 1;
    drv.surfaces[1].decode_pending = 1;

    mock_dequeue_result = 0;
    mock_dequeue_output_invalid = 0;
    mock_err_mb = 0;
    mock_dequeue_output_calls = 0;
    mock_queue_output_calls = 0;
    pthread_mutex_lock(&mock_dequeue_gate_mutex);
    mock_dequeue_block_enabled = 0;
    mock_dequeue_respect_timeout = 0;
    mock_dequeue_entered = 0;
    mock_dequeue_release = 0;
    pthread_mutex_unlock(&mock_dequeue_gate_mutex);

    VAStatus status = hobot_vaSyncSurface2(&va_ctx, 1, 0);
    int zero_timeout_pending = status == VA_STATUS_ERROR_TIMEDOUT &&
        drv.surfaces[1].decode_pending && !hctx->sync_active &&
        mock_dequeue_output_calls == 0;

    pthread_mutex_lock(&mock_dequeue_gate_mutex);
    mock_dequeue_block_enabled = 1;
    mock_dequeue_respect_timeout = 1;
    pthread_mutex_unlock(&mock_dequeue_gate_mutex);
    struct timespec start;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    status = hobot_vaSyncSurface2(&va_ctx, 1, 20000000ULL);
    clock_gettime(CLOCK_MONOTONIC, &end);
    int64_t elapsed_ns = ((int64_t)end.tv_sec - (int64_t)start.tv_sec) *
                         1000000000LL + end.tv_nsec - start.tv_nsec;
    int dequeue_timeout = status == VA_STATUS_ERROR_TIMEDOUT &&
        elapsed_ns >= 10000000LL && elapsed_ns < 500000000LL &&
        drv.surfaces[1].decode_pending && !hctx->sync_active &&
        mock_dequeue_output_calls == 1;

    pthread_mutex_lock(&mock_dequeue_gate_mutex);
    mock_dequeue_block_enabled = 0;
    mock_dequeue_respect_timeout = 0;
    pthread_mutex_unlock(&mock_dequeue_gate_mutex);
    mock_dequeue_result = 0;
    status = hobot_vaSyncSurface2(&va_ctx, 1, 1000000000ULL);
    int completed = status == VA_STATUS_SUCCESS &&
        drv.surfaces[1].has_decoded_frame && !drv.surfaces[1].decode_pending &&
        !hctx->sync_active && mock_dequeue_output_calls == 2;
    status = hobot_vaSyncSurface2(&va_ctx, 1, 0);
    int ready_zero_timeout = status == VA_STATUS_SUCCESS &&
        mock_dequeue_output_calls == 2;

    drv.surfaces[1].has_decoded_frame = 0;
    drv.surfaces[1].dma_fd = -1;
    drv.surfaces[1].decode_pending = 1;
    drv.surfaces[1].decode_error = 0;
    drv.surfaces[2].allocated = 1;
    drv.surfaces[2].context_id = 1;
    drv.surfaces[2].has_decoded_frame = 1;
    drv.surfaces[2].output_context_id = 1;
    drv.surfaces[2].dma_fd = 7;
    drv.surfaces[2].vpu_out_buf.vframe_buf.phy_ptr[0] = 1;
    drv.surfaces[2].vpu_out_buf.vframe_buf.size = sizeof(mock_frame);
    drv.surfaces[2].vpu_out_buf.vframe_buf.fd[0] = 7;
    hctx->sub_head = 0;
    hctx->sub_tail = 2;
    hctx->submitted_surfaces[0] = 2;
    hctx->submitted_surfaces[1] = 1;
    mock_queue_output_calls = 0;
    status = hobot_vaSyncSurface2(&va_ctx, 1, 0);
    int recycle_timeout_preserves_ownership =
        status == VA_STATUS_ERROR_TIMEDOUT && drv.surfaces[1].decode_pending &&
        drv.surfaces[2].has_decoded_frame &&
        drv.surfaces[2].vpu_out_buf.vframe_buf.phy_ptr[0] == 1 &&
        mock_queue_output_calls == 0 && !hctx->sync_active;

    VAStatus invalid_surface = hobot_vaSyncSurface2(
        &va_ctx, VA_INVALID_SURFACE, VA_TIMEOUT_INFINITE);

    int passed = zero_timeout_pending && dequeue_timeout && completed &&
                 ready_zero_timeout && recycle_timeout_preserves_ownership &&
                 invalid_surface == VA_STATUS_ERROR_INVALID_SURFACE;
    if (!passed)
        fprintf(stderr, "vaSyncSurface2 timeout/completion failed: zero=%d dequeue=%d elapsed=%lld completed=%d ready=%d recycle=%d invalid=%d status=%d calls=%d\n",
                zero_timeout_pending, dequeue_timeout,
                (long long)elapsed_ns, completed, ready_zero_timeout,
                recycle_timeout_preserves_ownership,
                invalid_surface, status, mock_dequeue_output_calls);
    if (drv.sync_cond_initialized)
        pthread_cond_destroy(&drv.sync_cond);
    pthread_mutex_destroy(&drv.mutex);
    pthread_mutex_lock(&mock_dequeue_gate_mutex);
    mock_dequeue_block_enabled = 0;
    mock_dequeue_respect_timeout = 0;
    mock_dequeue_entered = 0;
    mock_dequeue_release = 0;
    pthread_mutex_unlock(&mock_dequeue_gate_mutex);
    return passed;
}

static int test_va_sync_surface2_mutex_timeout(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    drv.surfaces[1].allocated = 1;
    drv.surfaces[1].has_decoded_frame = 1;

    SyncIsolationWorker worker = {
        .ctx = &va_ctx,
        .operation = SYNC_ISOLATION_SYNC2,
        .surface = 1,
        .status = VA_STATUS_ERROR_UNKNOWN,
        .timeout_ns = 20000000ULL
    };
    pthread_t thread;
    pthread_mutex_lock(&drv.mutex);
    struct timespec start;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int created = pthread_create(&thread, NULL, sync_isolation_worker, &worker) == 0;
    int started = created && wait_for_gate_flag(&worker.started, 250);
    int completed = started && wait_for_gate_flag(&worker.completed, 500);
    clock_gettime(CLOCK_MONOTONIC, &end);
    pthread_mutex_unlock(&drv.mutex);
    if (created)
        pthread_join(thread, NULL);

    int64_t elapsed_ns = ((int64_t)end.tv_sec - (int64_t)start.tv_sec) *
                         1000000000LL + end.tv_nsec - start.tv_nsec;
    int passed = started && completed &&
                 worker.status == VA_STATUS_ERROR_TIMEDOUT &&
                 elapsed_ns >= 10000000LL && elapsed_ns < 500000000LL;
    if (!passed)
        fprintf(stderr, "vaSyncSurface2 mutex timeout failed: created=%d started=%d completed=%d status=%d elapsed=%lld\n",
                created, started, completed, worker.status,
                (long long)elapsed_ns);
    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_va_sync_buffer_contract(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    drv.buffers[1].allocated = 1;
    drv.buffers[1].id = 1;
    drv.buffers[1].type = VAImageBufferType;
    drv.buffers[2].allocated = 1;
    drv.buffers[2].id = 2;
    drv.buffers[2].type = VAEncCodedBufferType;

    VAStatus ready_zero = hobot_vaSyncBuffer(&va_ctx, 1, 0);
    VAStatus ready_infinite = hobot_vaSyncBuffer(&va_ctx, 2, VA_TIMEOUT_INFINITE);
    VAStatus invalid = hobot_vaSyncBuffer(&va_ctx, VA_INVALID_ID, 0);
    VAStatus out_of_range = hobot_vaSyncBuffer(&va_ctx, MAX_BUFFERS, 0);
    pthread_mutex_destroy(&drv.mutex);

    int passed = ready_zero == VA_STATUS_SUCCESS &&
                 ready_infinite == VA_STATUS_SUCCESS &&
                 invalid == VA_STATUS_ERROR_INVALID_BUFFER &&
                 out_of_range == VA_STATUS_ERROR_INVALID_BUFFER;
    if (!passed)
        fprintf(stderr, "vaSyncBuffer validation failed: zero=%d infinite=%d invalid=%d range=%d\n",
                ready_zero, ready_infinite, invalid, out_of_range);
    return passed;
}

static int test_va_sync_buffer_mutex_timeout(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    drv.buffers[1].allocated = 1;
    drv.buffers[1].id = 1;

    SyncIsolationWorker worker = {
        .ctx = &va_ctx,
        .operation = SYNC_ISOLATION_SYNC_BUFFER,
        .buffer = 1,
        .status = VA_STATUS_ERROR_UNKNOWN,
        .timeout_ns = 20000000ULL
    };
    pthread_t thread;
    pthread_mutex_lock(&drv.mutex);
    struct timespec start;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int created = pthread_create(&thread, NULL, sync_isolation_worker, &worker) == 0;
    int started = created && wait_for_gate_flag(&worker.started, 250);
    int completed = started && wait_for_gate_flag(&worker.completed, 500);
    clock_gettime(CLOCK_MONOTONIC, &end);
    pthread_mutex_unlock(&drv.mutex);
    if (created)
        pthread_join(thread, NULL);

    int64_t elapsed_ns = ((int64_t)end.tv_sec - (int64_t)start.tv_sec) *
                         1000000000LL + end.tv_nsec - start.tv_nsec;
    int finite_timeout = started && completed &&
                         worker.status == VA_STATUS_ERROR_TIMEDOUT &&
                         elapsed_ns >= 10000000LL && elapsed_ns < 500000000LL;

    pthread_mutex_lock(&mock_dequeue_gate_mutex);
    worker.started = 0;
    worker.completed = 0;
    pthread_mutex_unlock(&mock_dequeue_gate_mutex);
    worker.status = VA_STATUS_ERROR_UNKNOWN;
    worker.timeout_ns = 0;
    pthread_mutex_lock(&drv.mutex);
    clock_gettime(CLOCK_MONOTONIC, &start);
    created = pthread_create(&thread, NULL, sync_isolation_worker, &worker) == 0;
    started = created && wait_for_gate_flag(&worker.started, 250);
    completed = started && wait_for_gate_flag(&worker.completed, 500);
    clock_gettime(CLOCK_MONOTONIC, &end);
    pthread_mutex_unlock(&drv.mutex);
    if (created)
        pthread_join(thread, NULL);
    elapsed_ns = ((int64_t)end.tv_sec - (int64_t)start.tv_sec) *
                 1000000000LL + end.tv_nsec - start.tv_nsec;
    int zero_timeout = started && completed &&
                       worker.status == VA_STATUS_ERROR_TIMEDOUT &&
                       elapsed_ns >= 0 && elapsed_ns < 500000000LL;
    int passed = finite_timeout && zero_timeout;
    if (!passed)
        fprintf(stderr, "vaSyncBuffer mutex timeout failed: finite=%d zero=%d status=%d elapsed=%lld\n",
                finite_timeout, zero_timeout, worker.status,
                (long long)elapsed_ns);
    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static void *buffer_stress_worker(void *opaque)
{
    BufferStressWorker *worker = opaque;
    unsigned char initial_data[32] = {0};
    for (int i = 0; i < 500; i++) {
        VABufferID buffer = VA_INVALID_ID;
        void *mapped = NULL;
        int failed = hobot_vaCreateBuffer(worker->ctx, 0, VAImageBufferType,
                                          sizeof(initial_data), 1, initial_data, &buffer) !=
                     VA_STATUS_SUCCESS;
        if (!failed) {
            failed = hobot_vaMapBuffer(worker->ctx, buffer, &mapped) != VA_STATUS_SUCCESS ||
                     !mapped ||
                     hobot_vaBufferSetNumElements(worker->ctx, buffer, 1) != VA_STATUS_SUCCESS ||
                     hobot_vaUnmapBuffer(worker->ctx, buffer) != VA_STATUS_SUCCESS;
            if (hobot_vaDestroyBuffer(worker->ctx, buffer) != VA_STATUS_SUCCESS)
                failed = 1;
        }
        if (failed) {
            worker->failed = 1;
            return NULL;
        }
    }
    return NULL;
}

static int test_buffer_api_concurrent_access(void)
{
    enum { WORKERS = 8 };
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    pthread_t threads[WORKERS];
    BufferStressWorker workers[WORKERS] = {0};
    int started = 0;

    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;
    for (int i = 0; i < WORKERS; i++) {
        workers[i].ctx = &va_ctx;
        if (pthread_create(&threads[i], NULL, buffer_stress_worker, &workers[i]) != 0)
            break;
        started++;
    }
    for (int i = 0; i < started; i++)
        pthread_join(threads[i], NULL);

    int passed = started == WORKERS;
    for (int i = 0; i < started; i++)
        passed = passed && !workers[i].failed;
    for (int i = 1; i < MAX_BUFFERS; i++)
        passed = passed && !drv.buffers[i].allocated;
    pthread_mutex_destroy(&drv.mutex);
    if (!passed)
        fprintf(stderr, "concurrent buffer create/map/destroy stress failed (threads=%d/%d)\n",
                started, WORKERS);
    return passed;
}

static int test_h264_deferred_encoder_configuration(void)
{
    static const struct {
        VAProfile va_profile;
        mc_h264_profile_t sdk_profile;
    } profile_cases[] = {
        {VAProfileH264ConstrainedBaseline, MC_H264_PROFILE_BP},
        {VAProfileH264Main, MC_H264_PROFILE_MP},
        {VAProfileH264High, MC_H264_PROFILE_HP},
    };
    mc_h264_profile_t sdk_profile;
    for (size_t i = 0; i < sizeof(profile_cases) / sizeof(profile_cases[0]); i++) {
        if (!hobot_h264_sdk_profile(profile_cases[i].va_profile, &sdk_profile) ||
            sdk_profile != profile_cases[i].sdk_profile)
            return 0;
    }
    if (hobot_h264_sdk_profile(VAProfileNone, &sdk_profile))
        return 0;

    VAEncSequenceParameterBufferH264 seq = {0};
    uint32_t fps = 0;
    seq.vui_parameters_present_flag = 1;
    seq.vui_fields.bits.timing_info_present_flag = 1;
    seq.num_units_in_tick = 1;
    seq.time_scale = 120;
    if (hobot_h264_sequence_frame_rate(&seq, &fps) != 1 || fps != 60)
        return 0;
    seq.num_units_in_tick = 1001;
    seq.time_scale = 60000;
    if (hobot_h264_sequence_frame_rate(&seq, &fps) != 1 || fps != 30)
        return 0;
    seq.num_units_in_tick = 1;
    seq.time_scale = 480;
    if (hobot_h264_sequence_frame_rate(&seq, &fps) != 1 || fps != 240)
        return 0;
    seq.time_scale = 482;
    if (hobot_h264_sequence_frame_rate(&seq, &fps) != -1)
        return 0;
    seq.num_units_in_tick = 0;
    if (hobot_h264_sequence_frame_rate(&seq, &fps) != 0 ||
        !hobot_h264_level_supported(MC_H264_LEVEL4_1) ||
        hobot_h264_level_supported(62))
        return 0;

    VAEncSequenceParameterBufferH264 seq_a = {0};
    seq_a.level_idc = MC_H264_LEVEL4_1;
    seq_a.vui_parameters_present_flag = 1;
    seq_a.vui_fields.bits.timing_info_present_flag = 1;
    seq_a.num_units_in_tick = 1;
    seq_a.time_scale = 120;
    VAEncSequenceParameterBufferH264 seq_b = seq_a;
    seq_b.intra_period = 90;
    if (!hobot_h264_sequence_config_equal(&seq_a, &seq_b))
        return 0;
    seq_b.time_scale = 60;
    if (hobot_h264_sequence_config_equal(&seq_a, &seq_b))
        return 0;
    seq_b = seq_a;
    seq_b.level_idc = MC_H264_LEVEL4_2;
    if (hobot_h264_sequence_config_equal(&seq_a, &seq_b))
        return 0;

    VAEncSequenceParameterBufferH264 cropped_seq = {0};
    cropped_seq.picture_width_in_mbs = 40;
    cropped_seq.picture_height_in_mbs = 23;
    cropped_seq.seq_fields.bits.chroma_format_idc = 1;
    cropped_seq.seq_fields.bits.frame_mbs_only_flag = 1;
    cropped_seq.ip_period = 1;
    cropped_seq.max_num_ref_frames = 1;
    cropped_seq.frame_cropping_flag = 1;
    cropped_seq.frame_crop_bottom_offset = 4;
    cropped_seq.level_idc = MC_H264_LEVEL4_1;
    uint32_t coded_width, coded_height, visible_width, visible_height;
    if (!hobot_h264_sequence_dimensions(&cropped_seq, &coded_width,
                                        &coded_height, &visible_width,
                                        &visible_height) ||
        coded_width != 640 || coded_height != 368 ||
        visible_width != 640 || visible_height != 360)
        return 0;

    HobotContext hctx = {0};
    hctx.id = 7;
    hctx.width = 640;
    hctx.height = 368;
    hctx.profile = VAProfileH264High;
    hctx.encoder_init_deferred = 1;
    hctx.h264_sequence_valid = 1;
    hctx.h264_sequence.level_idc = MC_H264_LEVEL4_1;
    hctx.h264_sequence = cropped_seq;
    hctx.vpu_ctx.codec_id = MEDIA_CODEC_ID_H264;
    hctx.vpu_ctx.encoder = 1;
    hctx.vpu_ctx.video_enc_params.enable_user_pts = 1;
    hctx.vpu_ctx.video_enc_params.rc_params.mode = MC_AV_RC_MODE_H264CBR;
    hctx.vpu_ctx.video_enc_params.rc_params.h264_cbr_params.frame_rate = 60;
    HobotSurface cropped_surface = {0};
    cropped_surface.width = 640;
    cropped_surface.height = 360;
    if (!hobot_h264_surface_alignment_candidate(&hctx, &cropped_surface) ||
        !hobot_h264_surface_matches_sequence(&hctx, &cropped_surface))
        return 0;

    VAEncSequenceParameterBufferH264 offset_seq = cropped_seq;
    offset_seq.frame_crop_left_offset = 1;
    offset_seq.frame_crop_top_offset = 1;
    offset_seq.frame_crop_bottom_offset = 3;
    if (!hobot_h264_sequence_dimensions(&offset_seq, &coded_width,
                                        &coded_height, &visible_width,
                                        &visible_height) ||
        visible_width != 638 || visible_height != 360)
        return 0;
    HobotContext offset_ctx = {0};
    offset_ctx.id = 9;
    offset_ctx.width = 640;
    offset_ctx.height = 368;
    offset_ctx.profile = VAProfileH264High;
    offset_ctx.h264_sequence_valid = 1;
    offset_ctx.h264_sequence = offset_seq;
    HobotSurface offset_surface = {0};
    offset_surface.width = visible_width;
    offset_surface.height = visible_height;
    int copy_x, copy_y, copy_width, copy_height;
    if (!hobot_h264_surface_matches_sequence(&offset_ctx, &offset_surface) ||
        !hobot_h264_surface_copy_region(&offset_ctx, &offset_surface,
                                        640, 368, &copy_x, &copy_y,
                                        &copy_width, &copy_height) ||
        copy_x != 2 || copy_y != 2 || copy_width != 638 || copy_height != 360) {
        fprintf(stderr, "H.264 visible input crop offsets were not mapped to coded coordinates\n");
        return 0;
    }

    const uint8_t src_y[16] = {
        10, 11, 12, 13, 20, 21, 22, 23,
        30, 31, 32, 33, 40, 41, 42, 43
    };
    const uint8_t src_uv[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t dst_y[8 * 10];
    uint8_t dst_uv[4 * 10];
    const uint8_t expected_y[8][8] = {
        {10, 10, 10, 11, 12, 13, 13, 13},
        {10, 10, 10, 11, 12, 13, 13, 13},
        {10, 10, 10, 11, 12, 13, 13, 13},
        {20, 20, 20, 21, 22, 23, 23, 23},
        {30, 30, 30, 31, 32, 33, 33, 33},
        {40, 40, 40, 41, 42, 43, 43, 43},
        {40, 40, 40, 41, 42, 43, 43, 43},
        {40, 40, 40, 41, 42, 43, 43, 43}
    };
    const uint8_t expected_uv[4][8] = {
        {1, 2, 1, 2, 3, 4, 3, 4},
        {1, 2, 1, 2, 3, 4, 3, 4},
        {5, 6, 5, 6, 7, 8, 7, 8},
        {5, 6, 5, 6, 7, 8, 7, 8}
    };
    memset(dst_y, 0xee, sizeof(dst_y));
    memset(dst_uv, 0xee, sizeof(dst_uv));
    if (hobot_copy_nv12_to_coded_frame(src_y, src_uv, 4, 4,
                                       dst_y, dst_uv, 10, 10,
                                       8, 8, 2, 2, 4, 4) != 0) {
        fprintf(stderr, "NV12 crop padding copy rejected a valid aligned region\n");
        return 0;
    }
    for (int row = 0; row < 8; row++) {
        if (memcmp(dst_y + row * 10, expected_y[row], 8) != 0 ||
            dst_y[row * 10 + 8] != 0xee || dst_y[row * 10 + 9] != 0xee) {
            fprintf(stderr, "NV12 luma edge replication failed at row %d\n", row);
            return 0;
        }
    }
    for (int row = 0; row < 4; row++) {
        if (memcmp(dst_uv + row * 10, expected_uv[row], 8) != 0 ||
            dst_uv[row * 10 + 8] != 0xee || dst_uv[row * 10 + 9] != 0xee) {
            fprintf(stderr, "NV12 chroma edge replication failed at row %d\n", row);
            return 0;
        }
    }

    mock_initialize_result = 0;
    mock_configure_result = 0;
    mock_start_result = 0;
    mock_stop_result = 0;
    mock_release_result = 0;
    mock_vui_get_result = 0;
    mock_vui_set_result = 0;
    mock_vui_get_calls = 0;
    mock_vui_set_calls = 0;
    mock_initialized_h264_profile = MC_H264_PROFILE_UNSPECIFIED;
    mock_initialized_h264_gop_preset_idx = 0;
    memset(&mock_vui_config, 0, sizeof(mock_vui_config));
    memset(&mock_vui_set_config, 0, sizeof(mock_vui_set_config));

    if (hobot_start_deferred_h264_encoder(&hctx) != 0 ||
        hctx.encoder_init_deferred || !hctx.vpu_initialized || !hctx.vpu_running ||
        mock_vui_get_calls != 1 || mock_vui_set_calls != 1 ||
        hctx.vpu_ctx.video_enc_params.h264_enc_config.h264_level != MC_H264_LEVEL4_1 ||
        mock_initialized_h264_profile != MC_H264_PROFILE_HP ||
        mock_initialized_h264_gop_preset_idx != 9 ||
        hctx.vpu_ctx.video_enc_params.enable_user_pts != 0 ||
        mock_vui_set_config.h264_vui.video_signal_type_present_flag != 1 ||
        mock_vui_set_config.h264_vui.video_full_range_flag != 0 ||
        mock_vui_set_config.h264_vui.sar_width != 1 ||
        mock_vui_set_config.h264_vui.sar_height != 1 ||
        hctx.vpu_ctx.video_enc_params.frame_cropping_flag != 1 ||
        hctx.vpu_ctx.video_enc_params.crop_rect.width != 640 ||
        hctx.vpu_ctx.video_enc_params.crop_rect.height != 360) {
        fprintf(stderr, "deferred H.264 setup did not apply validated level/VUI parameters\n");
        return 0;
    }

    HobotContext intra_ctx = {0};
    intra_ctx.id = 11;
    intra_ctx.width = 640;
    intra_ctx.height = 368;
    intra_ctx.profile = VAProfileH264High;
    intra_ctx.encoder_init_deferred = 1;
    intra_ctx.h264_sequence_valid = 1;
    intra_ctx.h264_sequence = cropped_seq;
    intra_ctx.h264_sequence.max_num_ref_frames = 0;
    intra_ctx.vpu_ctx.codec_id = MEDIA_CODEC_ID_H264;
    intra_ctx.vpu_ctx.encoder = 1;
    intra_ctx.vpu_ctx.video_enc_params.rc_params.mode = MC_AV_RC_MODE_H264CBR;
    mock_initialize_result = 0;
    mock_configure_result = 0;
    mock_start_result = 0;
    mock_stop_result = 0;
    mock_release_result = 0;
    mock_vui_get_result = 0;
    mock_vui_set_result = 0;
    if (hobot_start_deferred_h264_encoder(&intra_ctx) != 0 ||
        mock_initialized_h264_gop_preset_idx != 1 ||
        intra_ctx.vpu_ctx.video_enc_params.gop_params.gop_preset_idx != 1) {
        fprintf(stderr, "zero-reference H.264 sequence did not select all-intra GOP preset 1\n");
        return 0;
    }

    HobotContext cbctx = {0};
    cbctx.id = 10;
    cbctx.width = 640;
    cbctx.height = 360;
    cbctx.profile = VAProfileH264ConstrainedBaseline;
    cbctx.encoder_init_deferred = 1;
    cbctx.vpu_ctx.codec_id = MEDIA_CODEC_ID_H264;
    cbctx.vpu_ctx.encoder = 1;
    cbctx.vpu_ctx.video_enc_params.rc_params.mode = MC_AV_RC_MODE_H264CBR;
    mock_entropy_set_result = 0;
    mock_entropy_set_calls = 0;
    mock_transform_set_result = 0;
    mock_transform_set_calls = 0;
    memset(&mock_entropy_set_config, 0, sizeof(mock_entropy_set_config));
    memset(&mock_transform_set_config, 0, sizeof(mock_transform_set_config));
    if (hobot_start_deferred_h264_encoder(&cbctx) != 0 ||
        mock_initialized_h264_profile != MC_H264_PROFILE_BP ||
        mock_entropy_set_calls != 1 ||
        mock_entropy_set_config.entropy_coding_mode != 0 ||
        mock_transform_set_calls != 1 ||
        mock_transform_set_config.h264_transform.transform_8x8_enable != 0) {
        fprintf(stderr, "Constrained Baseline setup did not force CAVLC and disable 8x8 transform\n");
        return 0;
    }
    mock_entropy_set_result = 0;
    mock_transform_set_result = 0;

    offset_ctx.encoder_init_deferred = 1;
    offset_ctx.vpu_ctx.codec_id = MEDIA_CODEC_ID_H264;
    offset_ctx.vpu_ctx.encoder = 1;
    mock_initialize_result = 0;
    mock_configure_result = 0;
    mock_start_result = 0;
    mock_stop_result = 0;
    mock_release_result = 0;
    mock_vui_get_result = 0;
    mock_vui_set_result = 0;
    if (hobot_start_deferred_h264_encoder(&offset_ctx) != 0 ||
        offset_ctx.vpu_ctx.video_enc_params.crop_rect.x_pos != 2 ||
        offset_ctx.vpu_ctx.video_enc_params.crop_rect.y_pos != 2 ||
        offset_ctx.vpu_ctx.video_enc_params.crop_rect.width != 638 ||
        offset_ctx.vpu_ctx.video_enc_params.crop_rect.height != 360) {
        fprintf(stderr, "deferred H.264 setup did not preserve nonzero crop offsets\n");
        return 0;
    }

    hctx = (HobotContext){0};
    hctx.id = 8;
    hctx.profile = VAProfileH264High;
    hctx.encoder_init_deferred = 1;
    hctx.vpu_ctx.codec_id = MEDIA_CODEC_ID_H264;
    mock_vui_set_result = -1;
    if (hobot_start_deferred_h264_encoder(&hctx) == 0 ||
        hctx.encoder_init_deferred || hctx.vpu_initialized || hctx.vpu_running ||
        !hctx.encoder_failed) {
        fprintf(stderr, "failed deferred H.264 setup did not release and poison context\n");
        return 0;
    }
    mock_vui_set_result = 0;
    return 1;
}

static void initialize_mock_hevc_encoder(HobotContext *hctx)
{
    memset(hctx, 0, sizeof(*hctx));
    hctx->id = 12;
    hctx->width = 640;
    hctx->height = 360;
    hctx->encoder_init_deferred = 1;
    hctx->hevc_encode_sequence_valid = 1;
    hctx->hevc_encode_sequence.general_profile_idc = 1;
    hctx->hevc_encode_sequence.general_level_idc = MC_H265_LEVEL4_1;
    hctx->hevc_encode_sequence.ip_period = 1;
    hctx->hevc_encode_sequence.intra_period = 30;
    hctx->hevc_encode_sequence.intra_idr_period = 30;
    hctx->hevc_encode_sequence.bits_per_second = 4000000;
    hctx->hevc_encode_sequence.pic_width_in_luma_samples = 640;
    hctx->hevc_encode_sequence.pic_height_in_luma_samples = 360;
    hctx->hevc_encode_sequence.seq_fields.bits.chroma_format_idc = 1;
    hctx->hevc_encode_sequence.log2_diff_max_min_luma_coding_block_size = 3;
    hctx->hevc_encode_sequence.log2_diff_max_min_transform_block_size = 3;
    hctx->hevc_encode_picture_valid = 1;
    hctx->hevc_encode_picture.pic_init_qp = 26;
    hctx->hevc_encode_picture.pic_fields.bits.coding_type = 1;
    hctx->vpu_ctx.codec_id = MEDIA_CODEC_ID_H265;
    hctx->vpu_ctx.encoder = 1;
    hctx->vpu_ctx.video_enc_params.rc_params.h265_cbr_params.bit_rate = 4000;
    hctx->vpu_ctx.video_enc_params.rc_params.h265_cbr_params.frame_rate = 30;
}

static int test_hevc_encoder_disables_unsupported_sao(void)
{
    HobotContext hctx;
    initialize_mock_hevc_encoder(&hctx);
    mock_initialize_result = 0;
    mock_configure_result = 0;
    mock_configure_calls = 0;
    mock_sao_set_result = 0;
    mock_sao_set_calls = 0;
    mock_sao_set_before_configure = 0;
    mock_sao_set_config = (mc_h265_sao_params_t){0};
    mock_vui_get_result = 0;
    mock_vui_set_result = 0;
    mock_start_result = 0;
    mock_start_calls = 0;
    if (hobot_start_deferred_hevc_encoder(&hctx) != 0 ||
        mock_sao_set_calls != 1 ||
        !mock_sao_set_before_configure || mock_configure_calls != 1 ||
        mock_sao_set_config.sample_adaptive_offset_enabled_flag != 0 ||
        mock_start_calls != 1 || !hctx.vpu_running) {
        fprintf(stderr, "HEVC encoder did not disable unsupported decoder SAO before start\n");
        return 0;
    }

    initialize_mock_hevc_encoder(&hctx);
    mock_sao_set_result = -1;
    mock_configure_calls = 0;
    mock_sao_set_calls = 0;
    mock_sao_set_before_configure = 0;
    mock_start_calls = 0;
    mock_release_calls = 0;
    if (hobot_start_deferred_hevc_encoder(&hctx) == 0 ||
        mock_sao_set_calls != 1 || !mock_sao_set_before_configure ||
        mock_configure_calls != 0 || mock_start_calls != 0 ||
        mock_release_calls != 1 || hctx.vpu_initialized ||
        hctx.vpu_running || !hctx.encoder_failed) {
        fprintf(stderr, "HEVC SAO setup failure did not abort and release initialization\n");
        return 0;
    }
    mock_sao_set_result = 0;
    return 1;
}

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    int ready;
    int start;
} TraceStressGate;

static void *va_trace_first_use_worker(void *opaque)
{
    TraceStressGate *gate = opaque;
    pthread_mutex_lock(&gate->mutex);
    gate->ready++;
    pthread_cond_broadcast(&gate->condition);
    while (!gate->start)
        pthread_cond_wait(&gate->condition, &gate->mutex);
    pthread_mutex_unlock(&gate->mutex);
    va_trace("concurrent first-use regression");
    return NULL;
}

static int test_va_trace_concurrent_first_use(void)
{
    enum { WORKER_COUNT = 16 };
    pthread_t workers[WORKER_COUNT];
    TraceStressGate gate = {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .condition = PTHREAD_COND_INITIALIZER
    };
    int created = 0;
    for (; created < WORKER_COUNT; created++) {
        if (pthread_create(&workers[created], NULL,
                           va_trace_first_use_worker, &gate) != 0)
            break;
    }
    pthread_mutex_lock(&gate.mutex);
    while (gate.ready < created)
        pthread_cond_wait(&gate.condition, &gate.mutex);
    gate.start = 1;
    pthread_cond_broadcast(&gate.condition);
    pthread_mutex_unlock(&gate.mutex);
    for (int i = 0; i < created; i++)
        pthread_join(workers[i], NULL);
    pthread_cond_destroy(&gate.condition);
    pthread_mutex_destroy(&gate.mutex);
    if (created != WORKER_COUNT) {
        fprintf(stderr, "could not create trace initialization workers\n");
        return 0;
    }
    return 1;
}

static int test_preallocated_surface_free_failure_preserves_ownership(void)
{
    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 0;
    va_ctx.pDriverData = &drv;

    HobotSurface *surf = &drv.surfaces[1];
    surf->allocated = 1;
    surf->has_preallocated = 1;
    surf->preallocated_gbuf.fd[0] = 99;
    VASurfaceID surface_id = 1;
    mock_mem_free_calls = 0;
    mock_mem_free_result = -1;
    VAStatus status = hobot_vaDestroySurfaces(&va_ctx, &surface_id, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || !surf->allocated ||
        !surf->has_preallocated || surf->preallocated_gbuf.fd[0] != 99 ||
        mock_mem_free_calls != 1) {
        fprintf(stderr, "surface destroy lost preallocated-buffer ownership on free failure: status=%d allocated=%d has=%d fd=%d frees=%d\n",
                status, surf->allocated, surf->has_preallocated,
                surf->preallocated_gbuf.fd[0], mock_mem_free_calls);
        pthread_mutex_destroy(&drv.mutex);
        mock_mem_free_result = 0;
        return 0;
    }

    mock_mem_free_result = 0;
    status = hobot_vaDestroySurfaces(&va_ctx, &surface_id, 1);
    int passed = status == VA_STATUS_SUCCESS && !surf->allocated &&
                 !surf->has_preallocated && mock_mem_free_calls == 2;
    if (!passed)
        fprintf(stderr, "surface destroy did not recover after buffer-free retry: status=%d allocated=%d has=%d frees=%d\n",
                status, surf->allocated, surf->has_preallocated,
                mock_mem_free_calls);
    pthread_mutex_destroy(&drv.mutex);
    return passed;
}

static int test_terminate_preallocated_free_failure_is_retryable(void)
{
    HobotDriverData *drv = calloc(1, sizeof(*drv));
    struct VADriverContext va_ctx = {0};
    if (!drv || pthread_mutex_init(&drv->mutex, NULL) != 0) {
        free(drv);
        return 0;
    }
    va_ctx.pDriverData = drv;
    drv->surfaces[1].allocated = 1;
    drv->surfaces[1].has_preallocated = 1;
    drv->surfaces[1].preallocated_gbuf.fd[0] = 99;

    mock_mem_free_calls = 0;
    mock_mem_free_result = -1;
    int close_calls_before = mock_mem_module_close_calls;
    VAStatus status = hobot_vaTerminate(&va_ctx);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || va_ctx.pDriverData != drv ||
        !drv->surfaces[1].allocated || !drv->surfaces[1].has_preallocated ||
        drv->surfaces[1].preallocated_gbuf.fd[0] != 99 ||
        mock_mem_free_calls != 1 ||
        mock_mem_module_close_calls != close_calls_before) {
        fprintf(stderr, "terminate discarded preallocated-buffer ownership on free failure: status=%d data=%p allocated=%d has=%d fd=%d frees=%d closes=%d/%d\n",
                status, va_ctx.pDriverData, drv->surfaces[1].allocated,
                drv->surfaces[1].has_preallocated,
                drv->surfaces[1].preallocated_gbuf.fd[0], mock_mem_free_calls,
                mock_mem_module_close_calls, close_calls_before);
        mock_mem_free_result = 0;
        if (va_ctx.pDriverData)
            hobot_vaTerminate(&va_ctx);
        else
            free(drv);
        return 0;
    }

    mock_mem_free_result = 0;
    status = hobot_vaTerminate(&va_ctx);
    int passed = status == VA_STATUS_SUCCESS && va_ctx.pDriverData == NULL &&
                 mock_mem_free_calls == 2 &&
                 mock_mem_module_close_calls == close_calls_before + 1;
    if (!passed) {
        fprintf(stderr, "terminate did not recover after buffer-free retry: status=%d data=%p frees=%d closes=%d/%d\n",
                status, va_ctx.pDriverData, mock_mem_free_calls,
                mock_mem_module_close_calls, close_calls_before);
        if (va_ctx.pDriverData)
            hobot_vaTerminate(&va_ctx);
    }
    return passed;
}

typedef struct {
    int pid;
    int anomaly_count;
    long timestamp;
    int err_mb;
    int total_mb;
    unsigned long long sequence;
} WatchdogRecord;

static int read_watchdog_record(const char *path, WatchdogRecord *record)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    char line[192];
    size_t length = 0;
    while (length < sizeof(line) - 1) {
        ssize_t bytes = read(fd, line + length, sizeof(line) - 1 - length);
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes <= 0)
            break;
        length += (size_t)bytes;
        if (memchr(line, '\n', length))
            break;
    }
    close(fd);
    if (length == 0)
        return 0;
    line[length] = '\0';
    return sscanf(line, "%d %d %ld %d %d %llu", &record->pid,
                  &record->anomaly_count, &record->timestamp, &record->err_mb,
                  &record->total_mb, &record->sequence) == 6;
}

static void *publish_watchdog_worker(void *opaque)
{
    int anomaly_count = *(int *)opaque;
    hobot_publish_watchdog_state(anomaly_count, anomaly_count, 100);
    return NULL;
}

static int test_watchdog_ipc_is_pid_scoped_and_serialized(void)
{
    char own_path[128];
    char other_path[128];
    int pid = (int)getpid();
    if (!hobot_watchdog_path_for_pid(own_path, sizeof(own_path), pid) ||
        !hobot_watchdog_path_for_pid(other_path, sizeof(other_path), pid + 1) ||
        strcmp(own_path, other_path) == 0) {
        fprintf(stderr, "watchdog telemetry path is not process-scoped\n");
        return 0;
    }

    pthread_mutex_lock(&hobot_watchdog_publish_mutex);
    uint64_t sequence_before = hobot_watchdog_event_sequence;
    pthread_mutex_unlock(&hobot_watchdog_publish_mutex);
    remove(own_path);
    hobot_publish_watchdog_state(1, 1, 100);
    hobot_publish_watchdog_state(2, 2, 100);

    WatchdogRecord record = {0};
    pthread_mutex_lock(&hobot_watchdog_publish_mutex);
    uint64_t sequence_after_pair = hobot_watchdog_event_sequence;
    pthread_mutex_unlock(&hobot_watchdog_publish_mutex);
    if (!read_watchdog_record(own_path, &record) || record.pid != pid ||
        record.anomaly_count != 2 || record.err_mb != 2 ||
        record.total_mb != 100 || record.sequence != sequence_after_pair ||
        sequence_after_pair < sequence_before + 2) {
        fprintf(stderr, "watchdog publication record invalid: pid=%d count=%d err=%d total=%d sequence=%llu expected=%llu\n",
                record.pid, record.anomaly_count, record.err_mb,
                record.total_mb, record.sequence,
                (unsigned long long)sequence_after_pair);
        remove(own_path);
        return 0;
    }

    int counts[] = {3, 4};
    pthread_t workers[2];
    int created = 0;
    for (; created < 2; created++) {
        if (pthread_create(&workers[created], NULL, publish_watchdog_worker,
                           &counts[created]) != 0)
            break;
    }
    for (int i = 0; i < created; i++)
        pthread_join(workers[i], NULL);
    int passed = created == 2 && read_watchdog_record(own_path, &record) &&
                 record.pid == pid && record.total_mb == 100 &&
                 (record.anomaly_count == 3 || record.anomaly_count == 4) &&
                 record.sequence == sequence_after_pair + 2;
    if (!passed)
        fprintf(stderr, "concurrent watchdog publication lost or corrupted an event: created=%d pid=%d count=%d sequence=%llu expected=%llu\n",
                created, record.pid, record.anomaly_count, record.sequence,
                (unsigned long long)(sequence_after_pair + 2));
    if (!passed) {
        remove(own_path);
        return 0;
    }

    pid_t child = fork();
    if (child < 0) {
        perror("fork watchdog publisher");
        remove(own_path);
        return 0;
    }
    if (child == 0) {
        hobot_watchdog_event_sequence = 0;
        hobot_publish_watchdog_state(6, 6, 100);
        _exit(0);
    }

    char child_path[128];
    int child_status = 0;
    pthread_mutex_lock(&hobot_watchdog_publish_mutex);
    uint64_t sequence_before_fork_publish = hobot_watchdog_event_sequence;
    pthread_mutex_unlock(&hobot_watchdog_publish_mutex);
    hobot_publish_watchdog_state(5, 5, 100);
    int waited = waitpid(child, &child_status, 0) == child;
    int child_record_valid = waited && WIFEXITED(child_status) &&
        WEXITSTATUS(child_status) == 0 &&
        hobot_watchdog_path_for_pid(child_path, sizeof(child_path), (int)child) &&
        read_watchdog_record(child_path, &record) && record.pid == child &&
        record.anomaly_count == 6 &&
        record.sequence > sequence_before_fork_publish;
    int parent_record_valid = read_watchdog_record(own_path, &record) &&
        record.pid == pid && record.anomaly_count == 5 &&
        record.sequence == sequence_before_fork_publish + 1;
    if (!child_record_valid || !parent_record_valid)
        fprintf(stderr, "watchdog publishers interfered across processes: child_valid=%d parent_valid=%d child_pid=%d parent_pid=%d\n",
                child_record_valid, parent_record_valid, (int)child, pid);
    if (child_record_valid)
        remove(child_path);
    remove(own_path);
    return child_record_valid && parent_record_valid;
}

int main(void)
{
    struct VADriverContext invalid_init_ctx = {0};
    if (__vaDriverInit_1_0(&invalid_init_ctx) != VA_STATUS_ERROR_INVALID_CONTEXT ||
        mock_mem_module_open_calls != 0) {
        fprintf(stderr, "driver initialization accepted a missing vtable or opened memory module\n");
        return 1;
    }

    struct VADriverContext init_ctx = {0};
    struct VADriverVTable init_vtable = {0};
    init_ctx.vtable = &init_vtable;
    mock_mem_module_open_result = -1;
    mock_mem_module_open_calls = 0;
    mock_mem_module_close_calls = 0;
    VAStatus init_status = __vaDriverInit_1_0(&init_ctx);
    if (init_status != VA_STATUS_ERROR_OPERATION_FAILED || init_ctx.pDriverData ||
        mock_mem_module_open_calls != 1 || mock_mem_module_close_calls != 0) {
        fprintf(stderr, "memory module open failure was not propagated: status=%d data=%p open=%d close=%d\n",
                init_status, init_ctx.pDriverData,
                mock_mem_module_open_calls, mock_mem_module_close_calls);
        return 1;
    }

    mock_mem_module_open_result = 0;
    init_status = __vaDriverInit_1_0(&init_ctx);
    if (init_status != VA_STATUS_SUCCESS || !init_ctx.pDriverData ||
        mock_mem_module_open_calls != 2) {
        fprintf(stderr, "driver initialization failed after module open: status=%d data=%p open=%d\n",
                init_status, init_ctx.pDriverData, mock_mem_module_open_calls);
        return 1;
    }
    init_status = init_vtable.vaTerminate(&init_ctx);
    if (init_status != VA_STATUS_SUCCESS || init_ctx.pDriverData ||
        mock_mem_module_close_calls != 1) {
        fprintf(stderr, "driver termination did not balance module open: status=%d data=%p close=%d\n",
                init_status, init_ctx.pDriverData, mock_mem_module_close_calls);
        return 1;
    }
    mock_mem_module_close_result = -1;
    init_status = __vaDriverInit_1_0(&init_ctx);
    if (init_status != VA_STATUS_SUCCESS || !init_ctx.pDriverData) {
        fprintf(stderr, "driver reinitialization before close-failure test failed: status=%d data=%p\n",
                init_status, init_ctx.pDriverData);
        return 1;
    }
    init_status = init_vtable.vaTerminate(&init_ctx);
    if (init_status != VA_STATUS_ERROR_OPERATION_FAILED || !init_ctx.pDriverData ||
        mock_mem_module_close_calls != 2) {
        fprintf(stderr, "memory module close failure did not preserve retry state: status=%d data=%p close=%d\n",
                init_status, init_ctx.pDriverData, mock_mem_module_close_calls);
        return 1;
    }
    mock_mem_module_close_result = 0;
    init_status = init_vtable.vaTerminate(&init_ctx);
    if (init_status != VA_STATUS_SUCCESS || init_ctx.pDriverData ||
        mock_mem_module_close_calls != 3) {
        fprintf(stderr, "memory module close retry did not finish teardown: status=%d data=%p close=%d\n",
                init_status, init_ctx.pDriverData, mock_mem_module_close_calls);
        return 1;
    }

    if (!test_watchdog_ipc_is_pid_scoped_and_serialized() ||
        !test_video_decode_result_is_validated() ||
        !test_preallocated_surface_free_failure_preserves_ownership() ||
        !test_terminate_preallocated_free_failure_is_retryable() ||
        !test_va_trace_concurrent_first_use() ||
        !test_external_encoder_consumed_callback_is_single_flight() ||
        !test_external_encoder_output_error_preserves_surface_ownership() ||
        !test_h264_deferred_encoder_configuration() ||
        !test_decoder_input_queue_failure_preserves_ownership() ||
        !test_surface_recycles_through_original_context() ||
        !test_encoder_surface_keeps_decoder_buffer_owner() ||
        !test_encoder_parameter_buffers_reject_truncation() ||
        !test_fixed_record_parameters_reject_array_shape() ||
        !test_hevc_main_subset_parameter_validation() ||
        !test_hevc_pcm_sps_serialization() ||
        !test_hevc_tiles_pps_serialization() ||
        !test_hevc_rps_slice_validation() ||
        !test_hevc_three_sps_rps_index_rewrite() ||
        !test_hevc_rasl_nal_type_validation() ||
        !test_hevc_two_rps_multislice_p_validation() ||
        !test_hevc_two_rps_b_sao_validation() ||
        !test_hevc_two_rps_multireference_list_modification() ||
        !test_hevc_two_rps_multislice_b_validation() ||
        !test_hevc_two_rps_multislice_idr_validation() ||
        !test_hevc_main_encoder_parameter_validation() ||
        !test_hevc_encoder_sequence_reconfiguration_contract() ||
        !test_hevc_encoder_disables_unsupported_sao() ||
        !test_hevc_sps_conformance_window_crop() ||
        !test_hevc_context_rejects_oversized_resolution() ||
        !test_h264_encoder_slice_constraints() ||
        !test_h264_cqp_uses_effective_slice_qp() ||
        !test_h264_hrd_vbv_window() ||
        !test_encoder_control_errors_are_propagated() ||
        !test_h264_sequence_parameters_are_transactional() ||
        !test_h264_zero_reference_requires_intra_slice() ||
        !test_jpeg_picture_quality_updates_vpu_and_rolls_back_on_error() ||
        !test_jpeg_empty_app9_is_stripped_only_when_exact() ||
        !test_jpeg_huffman_accepts_only_hardware_defaults() ||
        !test_jpeg_decode_header_is_bounded_baseline_420() ||
        !test_jpeg_decode_rejected_parameters_do_not_mutate_cache() ||
        !test_jpeg_rotation_mapping_and_deferred_start() ||
        !test_encoder_end_picture_coded_buffer_contract() ||
        !test_encoder_input_queue_failure_preserves_ownership() ||
        !test_encoder_terminate_retries_retained_buffers() ||
        !test_decoder_terminate_retries_retained_output() ||
        !test_buffer_mapping_lifecycle() ||
        !test_active_encoder_retains_coded_buffer() ||
        !test_render_picture_prevalidates_all_buffers() ||
        !test_h264_parameter_synthesis_rejects_unrepresentable_sps() ||
        !test_pending_surface_is_synced_before_reuse() ||
        !test_idle_surface_sync_is_ready_without_decoder_owner() ||
        !test_surface_info_validates_nv12_layout_and_addresses() ||
        !test_surface_sync_lock_contract() ||
        !test_va_sync_surface2_timeout_and_completion() ||
        !test_va_sync_surface2_mutex_timeout() ||
        !test_va_sync_buffer_contract() ||
        !test_va_sync_buffer_mutex_timeout() ||
        !test_va_lock_surface_lifetime() ||
        !test_active_encoder_surface_excludes_competing_access() ||
        !test_fatal_dequeue_poison_requires_context_teardown() ||
        !test_sync_preserves_output_on_old_buffer_recycle_failure() ||
        !test_sync_does_not_recycle_locked_output_surface() ||
        !test_destroy_context_preserves_pending_surface_on_recycle_failure() ||
        !test_abort_pending_picture_across_fifo_wrap() ||
        !test_decoder_picture_failure_cleanup() ||
        !test_decoder_h264_slice_parameters() ||
        !test_h264_profile_sps_headers() ||
        !test_h264_interlaced_picture_parameters() ||
        !test_h264_constrained_baseline_output_headers() ||
        !test_h264_pps_reference_defaults_from_slice_parameters() ||
        !test_h264_pps_defaults_with_picture_and_slices_in_one_render() ||
        !test_h264_pps_defaults_when_picture_and_slices_are_separate() ||
        !test_h264_constrained_baseline_header_rejection() ||
        !test_surface_creation_validation() ||
        !test_prime2_surface_import() ||
        !test_dequeued_output_recycle_failure_preserves_ownership(0) ||
        !test_dequeued_output_recycle_failure_preserves_ownership(1) ||
        !test_dequeued_output_timeout_is_retried_on_next_sync() ||
        !test_buffer_size_overflow_rejected() ||
        !test_buffer_num_elements_tracks_valid_size() ||
        !test_coded_buffer_honors_requested_capacity() ||
        !test_legacy_surface_attribute_query() ||
        !test_unsupported_va_operations_report_status() ||
        !test_export_rejects_invalid_flags() ||
        !test_context_target_count_is_bounded() ||
        !test_context_flags_are_validated() ||
        !test_context_render_target_lifetime() ||
        !test_encoder_picture_reference_lifetime() ||
        !test_hevc_decoder_rejects_unallocated_reference_surface() ||
        !test_api_array_counts_are_bounded() ||
        !test_begin_picture_rejects_undersized_surface() ||
        !test_put_image_updates_preallocated_dma_surface() ||
        !test_preallocated_nv12_backing_bounds() ||
        !test_put_image_lazily_allocates_and_uploads_dma_surface() ||
        !test_put_image_recycles_decoded_surface_before_upload() ||
        !test_image_size_and_rectangle_bounds() ||
        !test_decoded_nv12_plane_bounds() ||
        !test_begin_picture_rejects_stopped_context() ||
        !test_query_surface_status_tracks_pending_work() ||
        !test_query_surface_error_is_explicitly_unimplemented() ||
        !test_derived_image_maps_surface_storage() ||
        !test_derived_image_buffer_handle_lifecycle() ||
        !test_config_capabilities_match_encoder() ||
        !test_sync_does_not_block_unrelated_context() ||
        !test_buffer_api_concurrent_access())
        return 1;

    recycled_output_count = 0;
    mock_queue_output_calls = 0;

    HobotDriverData drv = {0};
    struct VADriverContext va_ctx = {0};
    VASurfaceID surface_id = 1;
    VADRMPRIMESurfaceDescriptor descriptor = {0};
    struct hobot_surface_info info = {0};

    if (pthread_mutex_init(&drv.mutex, NULL) != 0)
        return 1;
    va_ctx.pDriverData = &drv;

    HobotContext *hctx = &drv.contexts[1];
    hctx->allocated = 1;
    hctx->vpu_running = 1;

    HobotSurface *surf = &drv.surfaces[surface_id];
    surf->allocated = 1;
    surf->context_id = 1;
    surf->decode_pending = 1;
    hctx->submitted_surfaces[hctx->sub_tail++] = surface_id;

    mock_dequeue_result = 0;
    mock_err_mb = 1;
    VAStatus status = hobot_vaSyncSurface(&va_ctx, surface_id);
    if (status != VA_STATUS_ERROR_DECODING_ERROR || surf->decode_pending ||
        !surf->decode_error || recycled_output_count != 1) {
        fprintf(stderr, "corrupt-output state failed: status=%d pending=%d error=%d recycled=%d\n",
                status, surf->decode_pending, surf->decode_error, recycled_output_count);
        pthread_mutex_destroy(&drv.mutex);
        return 1;
    }

    surf->has_preallocated = 1;
    surf->dma_fd = 7;
    status = hobot_vaExportSurfaceHandle(&va_ctx, surface_id,
                                        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                        VA_EXPORT_SURFACE_READ_ONLY, &descriptor);
    if (status != VA_STATUS_ERROR_DECODING_ERROR) {
        fprintf(stderr, "failed surface was exported: status=%d\n", status);
        pthread_mutex_destroy(&drv.mutex);
        return 1;
    }

    surf->decode_error = 0;
    surf->decode_pending = 1;
    surf->has_preallocated = 0;
    surf->dma_fd = -1;
    mock_dequeue_result = -1;
    status = hobot_fill_surface_info(&va_ctx, surface_id, &info);
    if (status != VA_STATUS_ERROR_DECODING_ERROR || !hctx->decode_failed ||
        !surf->decode_pending || !surf->decode_error || surf->has_preallocated) {
        fprintf(stderr, "fatal dequeue did not preserve pending surface fallback state: status=%d failed=%d pending=%d error=%d preallocated=%d\n",
                status, hctx->decode_failed, surf->decode_pending,
                surf->decode_error, surf->has_preallocated);
        pthread_mutex_destroy(&drv.mutex);
        return 1;
    }

    memset(&drv.contexts[1], 0, sizeof(drv.contexts[1]));
    drv.contexts[1].id = 1;
    drv.contexts[1].allocated = 1;
    drv.contexts[1].vpu_initialized = 1;
    drv.contexts[1].vpu_running = 1;
    drv.surfaces[11].allocated = 1;
    drv.surfaces[11].context_id = 1;
    drv.surfaces[11].decode_pending = 1;
    mock_stop_calls = 0;
    mock_release_calls = 0;
    mock_stop_result = -1;
    mock_release_result = 0;
    status = hobot_vaDestroyContext(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || !drv.contexts[1].allocated ||
        !drv.contexts[1].vpu_initialized || !drv.contexts[1].vpu_running ||
        !drv.surfaces[11].decode_pending || drv.surfaces[11].context_id != 1 ||
        mock_stop_calls != 1 || mock_release_calls != 0) {
        fprintf(stderr, "stop failure state was not preserved: status=%d allocated=%d initialized=%d running=%d stop=%d release=%d\n",
                status, drv.contexts[1].allocated, drv.contexts[1].vpu_initialized,
                drv.contexts[1].vpu_running, mock_stop_calls, mock_release_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 1;
    }

    mock_stop_result = 0;
    mock_release_result = -1;
    status = hobot_vaDestroyContext(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || !drv.contexts[1].allocated ||
        !drv.contexts[1].vpu_initialized || drv.contexts[1].vpu_running ||
        !drv.surfaces[11].decode_pending || drv.surfaces[11].context_id != 1 ||
        mock_stop_calls != 2 || mock_release_calls != 1) {
        fprintf(stderr, "release failure state was not preserved: status=%d allocated=%d initialized=%d running=%d stop=%d release=%d\n",
                status, drv.contexts[1].allocated, drv.contexts[1].vpu_initialized,
                drv.contexts[1].vpu_running, mock_stop_calls, mock_release_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 1;
    }

    mock_release_result = 0;
    status = hobot_vaDestroyContext(&va_ctx, 1);
    if (status != VA_STATUS_SUCCESS || drv.contexts[1].allocated ||
        drv.contexts[1].vpu_initialized || drv.contexts[1].vpu_running ||
        drv.surfaces[11].decode_pending || !drv.surfaces[11].decode_error ||
        drv.surfaces[11].context_id != 0 ||
        mock_stop_calls != 2 || mock_release_calls != 2) {
        fprintf(stderr, "release retry failed: status=%d allocated=%d initialized=%d running=%d stop=%d release=%d\n",
                status, drv.contexts[1].allocated, drv.contexts[1].vpu_initialized,
                drv.contexts[1].vpu_running, mock_stop_calls, mock_release_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 1;
    }

    drv.configs[1].allocated = 1;
    drv.configs[1].profile = VAProfileH264High;
    drv.configs[1].entrypoint = VAEntrypointVLD;
    mock_initialize_result = 0;
    mock_configure_result = -1;
    mock_stop_result = 0;
    mock_release_result = -1;
    VASurfaceID orphan_target = 12;
    VASurfaceID recovered_target = 13;
    drv.surfaces[orphan_target].allocated = 1;
    drv.surfaces[recovered_target].allocated = 1;
    VAContextID failed_context = VA_INVALID_ID;
    status = hobot_vaCreateContext(&va_ctx, 1, 1280, 720, 0,
                                   &orphan_target, 1, &failed_context);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || !drv.contexts[1].allocated ||
        !drv.contexts[1].vpu_initialized || drv.contexts[1].vpu_running ||
        !drv.contexts[1].cleanup_orphaned ||
        drv.surfaces[orphan_target].context_usage_mask !=
            hobot_context_usage_bit(1) ||
        hobot_vaDestroySurfaces(&va_ctx, &orphan_target, 1) !=
            VA_STATUS_ERROR_SURFACE_BUSY) {
        fprintf(stderr, "create cleanup failure state was not preserved: status=%d allocated=%d initialized=%d running=%d\n",
                status, drv.contexts[1].allocated, drv.contexts[1].vpu_initialized,
                drv.contexts[1].vpu_running);
        pthread_mutex_destroy(&drv.mutex);
        return 1;
    }

    mock_configure_result = 0;
    VAContextID recovered_context = VA_INVALID_ID;
    status = hobot_vaCreateContext(&va_ctx, 1, 1280, 720, 0, NULL, 0,
                                   &recovered_context);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || recovered_context != VA_INVALID_ID ||
        !drv.contexts[1].allocated || !drv.contexts[1].vpu_initialized ||
        !drv.contexts[1].cleanup_orphaned || drv.contexts[2].allocated ||
        drv.surfaces[orphan_target].context_usage_mask !=
            hobot_context_usage_bit(1) ||
        mock_release_calls != 4) {
        fprintf(stderr, "failed orphan retry changed preserved state: status=%d id=%u allocated=%d initialized=%d orphan=%d releases=%d\n",
                status, recovered_context, drv.contexts[1].allocated,
                drv.contexts[1].vpu_initialized, drv.contexts[1].cleanup_orphaned,
                mock_release_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 1;
    }

    mock_release_result = 0;
    status = hobot_vaCreateContext(&va_ctx, 1, 1280, 720, 0,
                                   &recovered_target, 1,
                                   &recovered_context);
    if (status != VA_STATUS_SUCCESS || recovered_context != 1 ||
        !drv.contexts[1].allocated || !drv.contexts[1].vpu_running ||
        drv.contexts[1].cleanup_orphaned || mock_release_calls != 5 ||
        drv.surfaces[orphan_target].context_usage_mask != 0 ||
        drv.surfaces[recovered_target].context_usage_mask !=
            hobot_context_usage_bit(recovered_context) ||
        hobot_vaDestroySurfaces(&va_ctx, &orphan_target, 1) !=
            VA_STATUS_SUCCESS ||
        hobot_vaDestroySurfaces(&va_ctx, &recovered_target, 1) !=
            VA_STATUS_ERROR_SURFACE_BUSY) {
        fprintf(stderr, "orphan context recovery failed: status=%d id=%u allocated=%d running=%d orphan=%d releases=%d\n",
                status, recovered_context, drv.contexts[1].allocated,
                drv.contexts[1].vpu_running, drv.contexts[1].cleanup_orphaned,
                mock_release_calls);
        pthread_mutex_destroy(&drv.mutex);
        return 1;
    }
    status = hobot_vaDestroyContext(&va_ctx, recovered_context);
    if (status != VA_STATUS_SUCCESS || drv.contexts[1].allocated ||
        drv.contexts[1].vpu_initialized ||
        drv.surfaces[recovered_target].context_usage_mask != 0 ||
        hobot_vaDestroySurfaces(&va_ctx, &recovered_target, 1) !=
            VA_STATUS_SUCCESS) {
        fprintf(stderr, "recovered context teardown failed: status=%d allocated=%d initialized=%d\n",
                status, drv.contexts[1].allocated, drv.contexts[1].vpu_initialized);
        pthread_mutex_destroy(&drv.mutex);
        return 1;
    }

    HobotDriverData *term_drv = calloc(1, sizeof(*term_drv));
    if (!term_drv || pthread_mutex_init(&term_drv->mutex, NULL) != 0) {
        free(term_drv);
        pthread_mutex_destroy(&drv.mutex);
        return 1;
    }
    struct VADriverContext term_ctx = {0};
    term_ctx.pDriverData = term_drv;
    term_drv->contexts[1].allocated = 1;
    term_drv->contexts[1].vpu_initialized = 1;
    term_drv->contexts[1].vpu_running = 1;
    term_drv->surfaces[1].allocated = 1;
    term_drv->surfaces[1].context_id = 1;
    term_drv->surfaces[1].decode_pending = 1;
    mock_stop_result = -1;
    mock_release_result = 0;
    status = hobot_vaTerminate(&term_ctx);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || term_ctx.pDriverData != term_drv ||
        !term_drv->contexts[1].allocated || !term_drv->contexts[1].vpu_initialized ||
        !term_drv->contexts[1].vpu_running || !term_drv->surfaces[1].decode_pending ||
        term_drv->surfaces[1].context_id != 1) {
        fprintf(stderr, "terminate stop failure state was not preserved: status=%d driver=%p initialized=%d running=%d\n",
                status, term_ctx.pDriverData, term_drv->contexts[1].vpu_initialized,
                term_drv->contexts[1].vpu_running);
        pthread_mutex_destroy(&term_drv->mutex);
        free(term_drv);
        pthread_mutex_destroy(&drv.mutex);
        return 1;
    }

    mock_stop_result = 0;
    mock_release_result = -1;
    status = hobot_vaTerminate(&term_ctx);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || term_ctx.pDriverData != term_drv ||
        !term_drv->contexts[1].allocated || !term_drv->contexts[1].vpu_initialized ||
        term_drv->contexts[1].vpu_running || !term_drv->surfaces[1].decode_pending ||
        term_drv->surfaces[1].context_id != 1) {
        fprintf(stderr, "terminate release failure state was not preserved: status=%d driver=%p initialized=%d running=%d\n",
                status, term_ctx.pDriverData, term_drv->contexts[1].vpu_initialized,
                term_drv->contexts[1].vpu_running);
        pthread_mutex_destroy(&term_drv->mutex);
        free(term_drv);
        pthread_mutex_destroy(&drv.mutex);
        return 1;
    }

    mock_release_result = 0;
    status = hobot_vaTerminate(&term_ctx);
    if (status != VA_STATUS_SUCCESS || term_ctx.pDriverData != NULL) {
        fprintf(stderr, "terminate release retry failed: status=%d driver=%p\n",
                status, term_ctx.pDriverData);
        pthread_mutex_destroy(&drv.mutex);
        return 1;
    }

    pthread_mutex_destroy(&drv.mutex);
    char watchdog_path[128];
    if (hobot_watchdog_path_for_pid(watchdog_path, sizeof(watchdog_path),
                                    (int)getpid()))
        remove(watchdog_path);
    puts("surface-state and VPU teardown regression checks passed");
    return 0;
}
