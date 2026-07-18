/*
 * A genuine minimal "native XvMC player": decodes a real local .m2v
 * file and displays it via vaPutSurface directly into a real window,
 * with real-time pacing -- the same zero-copy path a real 2005-2008
 * XvMC-aware media player (mplayer, xine) used, and the *only* way to
 * avoid this driver's real, measured image-transfer cost entirely.
 *
 * Every ffmpeg-based playback attempt in this investigation (-f xv,
 * -f fbdev, -f sdl2) fundamentally requires pixels back in system
 * memory, because that's how ffmpeg's generic filter/muxer pipeline
 * works -- there is no way to make ffmpeg's own output devices consume
 * an opaque XvMC surface directly. The only way to get true zero-copy
 * decode-to-screen is to not use ffmpeg's output pipeline at all and
 * drive vaPutSurface ourselves, which is what this does.
 *
 * Usage: LIBVA_DRIVER_NAME=xvmc ./local_mpeg2_player file.m2v [fps] [path to xvmc_drv_video.so]
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
    double fps = argc > 2 ? atof(argv[2]) : 23.976;
    const char *so_path = argc > 3 ? argv[3] : "./xvmc_drv_video.so";
    double frame_interval_ms = 1000.0 / fps;

    FILE *fp = fopen(m2v_path, "rb");
    if (!fp) { perror("fopen"); return 1; }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *filebuf = malloc((size_t)fsize);
    fread(filebuf, 1, (size_t)fsize, fp);
    fclose(fp);

    struct mpeg2_header_state state;
    memset(&state, 0, sizeof(state));
    {
        uint32_t consumed_total = 0;
        struct mpeg2_parsed_slice slices[256];
        unsigned int num_slices = 0;
        int consumed = mpeg2_headers_parse_picture(&state, filebuf, (uint32_t)fsize, slices, 256, &num_slices);
        if (consumed <= 0) { fprintf(stderr, "failed to parse first picture\n"); return 1; }
        (void)consumed_total;
    }
    int width = (((int)state.pic_params.horizontal_size + 15) / 16) * 16;
    int height = (((int)state.pic_params.vertical_size + 15) / 16) * 16;
    printf("resolution: %dx%d, pacing at %.3f fps (%.2fms/frame)\n", width, height, fps, frame_interval_ms);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "XOpenDisplay failed\n"); return 1; }
    int screen = DefaultScreen(dpy);

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
    ctx.x11_screen = screen;

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

    /* A real, visible window -- this is the ONLY place pixels ever go.
     * No image transfer, no system-memory copy, no ffmpeg filter/muxer
     * pipeline -- vaPutSurface blits the decoded XvMC surface straight
     * to this window via the real hardware overlay, every frame. */
    Window root = RootWindow(dpy, screen);
    Window win = XCreateSimpleWindow(dpy, root, 100, 100, (unsigned)width, (unsigned)height, 1,
                                      BlackPixel(dpy, screen), BlackPixel(dpy, screen));
    XStoreName(dpy, win, "local_mpeg2_player (zero-copy vaPutSurface)");
    XSelectInput(dpy, win, StructureNotifyMask);
    XMapWindow(dpy, win);
    XEvent ev;
    do { XNextEvent(dpy, &ev); } while (ev.type != MapNotify);
    XSync(dpy, False);

    memset(&state, 0, sizeof(state));
    uint32_t consumed_total = 0;
    int picture_count = 0;
    double decode_render_ms = 0, put_ms = 0;
    double t_start = now_ms();
    double next_frame_due = t_start;

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

        /* Real playback pacing -- sleep until this frame's real
         * presentation time, then display it. Without this, decode
         * (already faster than real-time on its own) would blast
         * through the whole file instantly instead of playing back
         * naturally. */
        next_frame_due += frame_interval_ms;
        double sleep_ms = next_frame_due - now_ms();
        if (sleep_ms > 0)
            usleep((useconds_t)(sleep_ms * 1000));

        double put0 = now_ms();
        vtable.vaPutSurface(&ctx, surfaces[0], (void *)win,
                            0, 0, (unsigned short)width, (unsigned short)height,
                            0, 0, (unsigned short)width, (unsigned short)height,
                            NULL, 0, 0);
        /* Real, necessary backpressure -- confirmed in relay_display_test.c
         * earlier this project: Xlib buffers requests asynchronously, and
         * without forcing each blit to actually complete before queuing
         * the next one, rapid unsynced XvMCPutSurface calls exhaust a
         * real, limited XV resource on this hardware (a real BadAlloc). */
        XSync(dpy, False);
        put_ms += now_ms() - put0;
    }
    double t_total = now_ms() - t_start;

    printf("pictures=%d decode+render=%.1fms putsurface=%.1fms wall=%.1fms (expected ~%.1fms for %.3ffps)\n",
           picture_count, decode_render_ms, put_ms, t_total, picture_count * frame_interval_ms, fps);

    sleep(1); /* leave the last frame visible for a moment before tearing down */
    vtable.vaDestroyContext(&ctx, va_context);
    vtable.vaDestroySurfaces(&ctx, surfaces, 1);
    vtable.vaTerminate(&ctx);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
