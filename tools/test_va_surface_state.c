#include "../src/hobot_drv_video.c"

static int mock_dequeue_result;
static int mock_err_mb;
static int recycled_output_count;
static int mock_initialize_result;
static int mock_configure_result;
static int mock_start_result;
static int mock_stop_result;
static int mock_release_result;
static int mock_stop_calls;
static int mock_release_calls;
static uint8_t mock_frame[64];

hb_s32 hb_mm_mc_initialize(media_codec_context_t *context)
{
    (void)context;
    return mock_initialize_result;
}

hb_s32 hb_mm_mc_configure(media_codec_context_t *context)
{
    (void)context;
    return mock_configure_result;
}

hb_s32 hb_mm_mc_start(media_codec_context_t *context,
                       const mc_av_codec_startup_params_t *info)
{
    (void)context;
    (void)info;
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

int32_t hb_mem_module_close(void)
{
    return 0;
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
    (void)timeout;
    if (mock_dequeue_result != 0)
        return mock_dequeue_result;

    memset(buffer, 0, sizeof(*buffer));
    memset(info, 0, sizeof(*info));
    buffer->vframe_buf.phy_ptr[0] = 1;
    buffer->vframe_buf.vir_ptr[0] = mock_frame;
    buffer->vframe_buf.fd[0] = 7;
    buffer->vframe_buf.size = sizeof(mock_frame);
    info->video_frame_info.err_mb_in_frame_display = mock_err_mb;
    info->video_frame_info.total_mb_in_frame_display = 100;
    return 0;
}

hb_s32 hb_mm_mc_queue_output_buffer(media_codec_context_t *context,
                                     media_codec_buffer_t *buffer,
                                     hb_s32 timeout)
{
    (void)context;
    (void)buffer;
    (void)timeout;
    recycled_output_count++;
    return 0;
}

/* Keep the synthetic anomaly from publishing to the live watchdog IPC file. */
FILE *fopen(const char *path, const char *mode)
{
    (void)path;
    (void)mode;
    return NULL;
}

int main(void)
{
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
    if (status != VA_STATUS_ERROR_TIMEDOUT || !surf->decode_pending || surf->has_preallocated) {
        fprintf(stderr, "pending surface fallback failed: status=%d pending=%d preallocated=%d\n",
                status, surf->decode_pending, surf->has_preallocated);
        pthread_mutex_destroy(&drv.mutex);
        return 1;
    }

    memset(&drv.contexts[1], 0, sizeof(drv.contexts[1]));
    drv.contexts[1].id = 1;
    drv.contexts[1].allocated = 1;
    drv.contexts[1].vpu_initialized = 1;
    drv.contexts[1].vpu_running = 1;
    mock_stop_calls = 0;
    mock_release_calls = 0;
    mock_stop_result = -1;
    mock_release_result = 0;
    status = hobot_vaDestroyContext(&va_ctx, 1);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || !drv.contexts[1].allocated ||
        !drv.contexts[1].vpu_initialized || !drv.contexts[1].vpu_running ||
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
    VAContextID failed_context = VA_INVALID_ID;
    status = hobot_vaCreateContext(&va_ctx, 1, 1280, 720, 0, NULL, 0, &failed_context);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || !drv.contexts[1].allocated ||
        !drv.contexts[1].vpu_initialized || drv.contexts[1].vpu_running) {
        fprintf(stderr, "create cleanup failure state was not preserved: status=%d allocated=%d initialized=%d running=%d\n",
                status, drv.contexts[1].allocated, drv.contexts[1].vpu_initialized,
                drv.contexts[1].vpu_running);
        pthread_mutex_destroy(&drv.mutex);
        return 1;
    }

    mock_release_result = 0;
    status = hobot_vaDestroyContext(&va_ctx, 1);
    if (status != VA_STATUS_SUCCESS || drv.contexts[1].allocated ||
        drv.contexts[1].vpu_initialized) {
        fprintf(stderr, "create cleanup retry failed: status=%d allocated=%d initialized=%d\n",
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
    mock_stop_result = -1;
    mock_release_result = 0;
    status = hobot_vaTerminate(&term_ctx);
    if (status != VA_STATUS_ERROR_OPERATION_FAILED || term_ctx.pDriverData != term_drv ||
        !term_drv->contexts[1].allocated || !term_drv->contexts[1].vpu_initialized ||
        !term_drv->contexts[1].vpu_running) {
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
        term_drv->contexts[1].vpu_running) {
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
    puts("surface-state and VPU teardown regression checks passed");
    return 0;
}
