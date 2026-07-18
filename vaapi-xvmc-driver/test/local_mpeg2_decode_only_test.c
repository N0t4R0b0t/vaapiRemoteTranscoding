/*
 * Isolates the TRUE cost of decode+render (this driver's own software
 * entropy-decode/IDCT, plus the real XvMCRenderSurface hardware call)
 * with ZERO image transfer -- unlike every ffmpeg-based benchmark used
 * so far in this investigation, which always calls vaGetImage once per
 * frame (PutSurface+XShmGetImage+RGB->NV12 conversion, ~30-35ms/frame)
 * because ffmpeg's own pipeline wants decoded frames back in system
 * memory even when the output is /dev/null. A real hardware-accelerated
 * player displaying to screen never pays that cost at all (it uses
 * vaPutSurface to blit directly, no readback) -- so every "-f null -"
 * timing measured against this driver so far included a real, large,
 * artificial cost no actual playback scenario needs.
 *
 * Parses a real local .m2v file with this driver's own mpeg2_headers.c
 * (the exact same VAPictureParameterBufferMPEG2/VAIQMatrixBufferMPEG2/
 * VASliceParameterBufferMPEG2 structures a real VA-API client would
 * hand this driver) and feeds it through vaBeginPicture/vaRenderPicture/
 * vaEndPicture only -- no vaGetImage, no vaPutSurface, nothing but real
 * decode+render, timed.
 *
 * Usage: LIBVA_DRIVER_NAME=xvmc ./local_mpeg2_decode_only_test file.m2v [path to xvmc_drv_video.so]
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <X11/Xlib.h>
#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_version.h>

#include "mpeg2_headers.h"

#define STR_(x) #x
#define STR(x) STR_(x)
#define VA_DRIVER_INIT_NAME_(major, minor) __vaDriverInit_##major##_##minor
#define VA_DRIVER_INIT_NAME(major, minor) VA_DRIVER_INIT_NAME_(major, minor)
#define VA_DRIVER_INIT_SYMBOL STR(VA_DRIVER_INIT_NAME(VA_MAJOR_VERSION, VA_MINOR_VERSION))

typedef VAStatus (*init_fn)(VADriverContextP);

static int check(const char *what, VAStatus status)
{
    printf("%-20s -> %d%s\n", what, status, status == VA_STATUS_SUCCESS ? "" : "  <-- FAIL");
    return status == VA_STATUS_SUCCESS;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    const char *m2v_path = argc > 1 ? argv[1] : "test.m2v";
    const char *so_path = argc > 2 ? argv[2] : "./xvmc_drv_video.so";

    FILE *fp = fopen(m2v_path, "rb");
    if (!fp) { perror("fopen"); return 1; }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *filebuf = malloc((size_t)fsize);
    fread(filebuf, 1, (size_t)fsize, fp);
    fclose(fp);

    /* Parse the whole file up front (not timed -- this is pure header
     * parsing, not the decode/render work being measured) to learn the
     * real resolution and collect every picture's real slice data. */
    struct mpeg2_header_state state;
    memset(&state, 0, sizeof(state));
    uint32_t consumed_total = 0;
    int picture_count = 0;
    while (consumed_total < (uint32_t)fsize) {
        uint32_t remaining = (uint32_t)fsize - consumed_total;
        struct mpeg2_parsed_slice slices[256];
        unsigned int num_slices = 0;
        int consumed = mpeg2_headers_parse_picture(
            &state, filebuf + consumed_total, remaining, slices, 256, &num_slices);
        if (consumed <= 0) break;
        picture_count++;
        consumed_total += (uint32_t)consumed;
        if (picture_count == 1) break; /* just need resolution from picture 1 here */
    }
    int width = (((int)state.pic_params.horizontal_size + 15) / 16) * 16;
    int height = (((int)state.pic_params.vertical_size + 15) / 16) * 16;
    printf("resolution: %dx%d\n", width, height);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "XOpenDisplay failed\n"); return 1; }

    void *handle = dlopen(so_path, RTLD_NOW);
    if (!handle) { fprintf(stderr, "dlopen(%s): %s\n", so_path, dlerror()); return 1; }
    init_fn init = (init_fn)dlsym(handle, VA_DRIVER_INIT_SYMBOL);
    if (!init) { fprintf(stderr, "dlsym: %s\n", dlerror()); return 1; }

    struct VADriverContext ctx;
    struct VADriverVTable vtable;
    memset(&ctx, 0, sizeof(ctx));
    memset(&vtable, 0, sizeof(vtable));
    ctx.vtable = &vtable;
    ctx.native_dpy = dpy;
    ctx.x11_screen = DefaultScreen(dpy);

    VAStatus init_status = init(&ctx);
    printf("%-20s -> %d\n", "vaDriverInit", init_status);
    if (init_status != VA_STATUS_SUCCESS) {
        printf("no XvMC-capable port found -- skipping (expected off real hardware)\n");
        return 0;
    }

    VAConfigID config_id;
    int ok = check("vaCreateConfig",
        vtable.vaCreateConfig(&ctx, VAProfileMPEG2Main, VAEntrypointVLD, NULL, 0, &config_id));
    VASurfaceID surfaces[1];
    ok &= check("vaCreateSurfaces",
        vtable.vaCreateSurfaces(&ctx, width, height, VA_RT_FORMAT_YUV420, 1, surfaces));
    VAContextID va_context;
    ok &= check("vaCreateContext",
        vtable.vaCreateContext(&ctx, config_id, width, height, 0, surfaces, 1, &va_context));
    if (!ok) { fprintf(stderr, "setup failed\n"); return 1; }

    /* Now re-parse the file for real, this time timing only the actual
     * vaBeginPicture/vaRenderPicture/vaEndPicture work -- no image
     * transfer of any kind. */
    memset(&state, 0, sizeof(state));
    consumed_total = 0;
    picture_count = 0;
    long total_slices = 0;
    double decode_render_ms = 0;

    double t_start = now_ms();
    while (consumed_total < (uint32_t)fsize) {
        uint32_t remaining = (uint32_t)fsize - consumed_total;
        struct mpeg2_parsed_slice slices[256];
        unsigned int num_slices = 0;
        int consumed = mpeg2_headers_parse_picture(
            &state, filebuf + consumed_total, remaining, slices, 256, &num_slices);
        if (consumed <= 0) break;
        consumed_total += (uint32_t)consumed;
        picture_count++;

        double p0 = now_ms();
        vtable.vaBeginPicture(&ctx, va_context, surfaces[0]);

        VABufferID pic_buf, iq_buf = 0;
        vtable.vaCreateBuffer(&ctx, va_context, VAPictureParameterBufferType,
                              sizeof(state.pic_params), 1, &state.pic_params, &pic_buf);
        VABufferID setup_bufs[2];
        int num_setup = 0;
        setup_bufs[num_setup++] = pic_buf;
        if (state.has_iq_matrix) {
            vtable.vaCreateBuffer(&ctx, va_context, VAIQMatrixBufferType,
                                  sizeof(state.iq_matrix), 1, &state.iq_matrix, &iq_buf);
            setup_bufs[num_setup++] = iq_buf;
        }
        vtable.vaRenderPicture(&ctx, va_context, setup_bufs, num_setup);
        vtable.vaDestroyBuffer(&ctx, pic_buf);
        if (iq_buf) vtable.vaDestroyBuffer(&ctx, iq_buf);

        for (unsigned int s = 0; s < num_slices; s++) {
            struct mpeg2_parsed_slice *sl = &slices[s];
            total_slices++;
            VASliceParameterBufferMPEG2 sp;
            memset(&sp, 0, sizeof(sp));
            sp.slice_data_size = sl->size;
            sp.slice_data_offset = 0;
            sp.macroblock_offset = sl->macroblock_bit_offset;
            sp.slice_horizontal_position = sl->slice_horizontal_position;
            sp.slice_vertical_position = sl->slice_vertical_position;
            sp.quantiser_scale_code = sl->quantiser_scale_code;
            sp.intra_slice_flag = sl->intra_slice_flag;

            VABufferID slice_param_buf, slice_data_buf;
            vtable.vaCreateBuffer(&ctx, va_context, VASliceParameterBufferType,
                                  sizeof(sp), 1, &sp, &slice_param_buf);
            vtable.vaCreateBuffer(&ctx, va_context, VASliceDataBufferType,
                                  sl->size, 1, (void *)sl->data, &slice_data_buf);
            VABufferID render_bufs[2] = { slice_param_buf, slice_data_buf };
            vtable.vaRenderPicture(&ctx, va_context, render_bufs, 2);
            vtable.vaDestroyBuffer(&ctx, slice_param_buf);
            vtable.vaDestroyBuffer(&ctx, slice_data_buf);
        }

        vtable.vaEndPicture(&ctx, va_context);
        decode_render_ms += now_ms() - p0;
    }
    double t_total = now_ms() - t_start;

    printf("pictures=%d slices=%ld decode+render=%.1fms (avg %.2fms/pic) wall=%.1fms\n",
           picture_count, total_slices, decode_render_ms,
           picture_count ? decode_render_ms / picture_count : 0.0, t_total);

    vtable.vaDestroyContext(&ctx, va_context);
    vtable.vaDestroySurfaces(&ctx, surfaces, 1);
    vtable.vaTerminate(&ctx);
    return 0;
}
