#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hb_media_codec.h>

int main() {
    media_codec_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.codec_id = MEDIA_CODEC_ID_H264;
    ctx.encoder = 0;
    ctx.video_dec_params.feed_mode = MC_FEEDING_MODE_FRAME_SIZE;
    ctx.video_dec_params.pix_fmt = MC_PIXEL_FORMAT_NV12;
    ctx.video_dec_params.bitstream_buf_size = 1024 * 1024;
    ctx.video_dec_params.bitstream_buf_count = 3;
    ctx.video_dec_params.frame_buf_count = 5;

    int ret = hb_mm_mc_initialize(&ctx);
    printf("hb_mm_mc_initialize: %d\n", ret);
    if (ret == 0) {
        ret = hb_mm_mc_configure(&ctx);
        printf("hb_mm_mc_configure: %d\n", ret);
        ret = hb_mm_mc_start(&ctx, NULL);
        printf("hb_mm_mc_start: %d\n", ret);
        hb_mm_mc_stop(&ctx);
        hb_mm_mc_release(&ctx);
        printf("SUCCESS! VPU MediaCodec C API initialized and released successfully!\n");
    }
    return 0;
}
