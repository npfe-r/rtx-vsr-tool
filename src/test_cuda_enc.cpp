#include <cstdio>
#include <cstring>
#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

static int test_hwframe_init(AVBufferRef* hwDev, enum AVPixelFormat fmt, enum AVPixelFormat sw_fmt, const char* label) {
    AVBufferRef* hwfc_ref = av_hwframe_ctx_alloc(hwDev);
    AVHWFramesContext* hwfc = (AVHWFramesContext*)hwfc_ref->data;
    hwfc->format    = fmt;
    hwfc->sw_format = sw_fmt;
    hwfc->width     = 1920;
    hwfc->height    = 1080;
    hwfc->initial_pool_size = 4;
    int ret = av_hwframe_ctx_init(hwfc_ref);
    printf("  %-25s ret=%d\n", label, ret);
    fflush(stdout);
    if (ret >= 0) {
        AVFrame* frame = av_frame_alloc();
        ret = av_hwframe_get_buffer(hwfc_ref, frame, 0);
        printf("    av_hwframe_get_buffer: ret=%d\n", ret);
        fflush(stdout);
        if (ret >= 0) {
            printf("    data[0]=%p linesize[0]=%d\n", frame->data[0], frame->linesize[0]);
            printf("    data[1]=%p linesize[1]=%d\n", frame->data[1], frame->linesize[1]);
            printf("    hw_frames_ctx=%p buf[0]=%p\n", (void*)frame->hw_frames_ctx, (void*)frame->buf[0]);
            fflush(stdout);
        }
        av_frame_free(&frame);
    }
    av_buffer_unref(&hwfc_ref);
    return ret;
}

int main() {
    printf("CUDA hwcontext diagnostic\n");
    fflush(stdout);

    // Step 1: Create CUDA hwdevice
    AVBufferRef* hwDev = nullptr;
    int ret = av_hwdevice_ctx_create(&hwDev, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
    printf("av_hwdevice_ctx_create(CUDA): ret=%d hwDev=%p\n", ret, (void*)hwDev);
    fflush(stdout);
    if (ret < 0) {
        char err[256];
        av_strerror(ret, err, sizeof(err));
        printf("  error: %s\n", err);
        fflush(stdout);
        return 1;
    }

    // Step 2: Test different hwframe format combos
    printf("\nTesting hwframe format combos:\n");
    fflush(stdout);

    test_hwframe_init(hwDev, AV_PIX_FMT_CUDA, AV_PIX_FMT_NV12, "CUDA/NV12");
    test_hwframe_init(hwDev, AV_PIX_FMT_NV12, AV_PIX_FMT_NV12, "NV12/NV12");
    test_hwframe_init(hwDev, AV_PIX_FMT_CUDA, AV_PIX_FMT_CUDA, "CUDA/CUDA");
    test_hwframe_init(hwDev, AV_PIX_FMT_NV12, AV_PIX_FMT_CUDA, "NV12/CUDA");

    printf("\nPixel format info:\n");
    fflush(stdout);
    {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(AV_PIX_FMT_CUDA);
        printf("  AV_PIX_FMT_CUDA=%d desc_flags=0x%x\n", AV_PIX_FMT_CUDA, d ? d->flags : -1);
        d = av_pix_fmt_desc_get(AV_PIX_FMT_NV12);
        printf("  AV_PIX_FMT_NV12=%d desc_flags=0x%x\n", AV_PIX_FMT_NV12, d ? d->flags : -1);
        fflush(stdout);
    }

    printf("\nDone.\n");
    fflush(stdout);
    av_buffer_unref(&hwDev);
    return 0;
}
