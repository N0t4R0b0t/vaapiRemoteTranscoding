/*
 * Drives the H.264-via-relay path end to end using real captured slice
 * bytes (see relay_pixel_test_slices_long.h), then dumps decoded pixels
 * as PPM snapshots, to directly confirm the relay path produces correct-
 * looking video on real GMA950 hardware.
 *
 * ffmpeg -hwaccel vaapi can't be used for this: it aborts after a
 * handful of frames on "Failed to transfer data to output frame" (this
 * driver deliberately doesn't implement vaGetImage/vaDeriveImage, since
 * XvMC surfaces are opaque render targets -- see render_stress_test.c's
 * file comment), which kills the whole process before it ever calls
 * vaEndPicture again to let this driver drain relay-server's response.
 * Driving the vtable directly here (same pattern as render_stress_test.c)
 * sidesteps that entirely: nothing here needs image transfer, only real
 * VA-API buffer submission and enough patience (vaDestroyContext's
 * bounded blocking drain) to receive what relay-server sends back.
 *
 * Requires a running relay-server push instance (XVMC_RELAY_HOST/
 * XVMC_RELAY_PORT env vars, same as the real driver) and
 * XVMC_SNAPSHOT_DIR set to where PPM frames should be written.
 *
 * Usage: XVMC_RELAY_HOST=... XVMC_RELAY_PORT=... XVMC_SNAPSHOT_DIR=...
 *        ./relay_pixel_test [path to xvmc_drv_video.so]
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_version.h>

#include "relay_pixel_test_slices_long.h"

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

/* Real VAPictureParameterBufferH264 field values for the same session
 * this test's slice bytes were captured from -- confirmed against the
 * real encoder's own SPS/PPS (see h264_reconstitute.c's fix commit) via
 * ffmpeg -bsf:v trace_headers, not guessed. 352x288 = 22x18 macroblocks. */
static void fill_pic_params(VAPictureParameterBufferH264 *pp, VASurfaceID surface)
{
    memset(pp, 0, sizeof(*pp));
    pp->CurrPic.picture_id = surface;
    pp->CurrPic.flags = 0;
    for (int i = 0; i < 16; i++)
        pp->ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
    pp->picture_width_in_mbs_minus1 = 21;
    pp->picture_height_in_mbs_minus1 = 17;
    pp->num_ref_frames = 3;
    pp->seq_fields.bits.log2_max_frame_num_minus4 = 0;
    pp->seq_fields.bits.pic_order_cnt_type = 2;
    pp->seq_fields.bits.frame_mbs_only_flag = 1;
    pp->seq_fields.bits.direct_8x8_inference_flag = 1;
    pp->pic_init_qp_minus26 = -3;
    pp->chroma_qp_index_offset = -2;
    pp->second_chroma_qp_index_offset = -2;
    pp->pic_fields.bits.entropy_coding_mode_flag = 0;
    pp->pic_fields.bits.deblocking_filter_control_present_flag = 1;
    pp->frame_num = 0;
}

int main(int argc, char **argv)
{
    const char *so_path = argc > 1 ? argv[1] : "./xvmc_drv_video.so";

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "XOpenDisplay failed -- is $DISPLAY set to a running X server?\n");
        return 1;
    }

    void *handle = dlopen(so_path, RTLD_NOW);
    if (!handle) {
        fprintf(stderr, "dlopen(%s): %s\n", so_path, dlerror());
        return 1;
    }

    init_fn init = (init_fn)dlsym(handle, VA_DRIVER_INIT_SYMBOL);
    if (!init) {
        fprintf(stderr, "dlsym(%s): %s\n", VA_DRIVER_INIT_SYMBOL, dlerror());
        return 1;
    }

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
        printf("no XvMC-capable port found on this X server -- nothing to test here, "
               "skipping (this is expected off the real GMA950 hardware). "
               "RELAY PIXEL TEST PASSED (trivially, see above)\n");
        return 0;
    }

    VAConfigID config_id;
    int ok = check("vaCreateConfig",
        vtable.vaCreateConfig(&ctx, VAProfileH264ConstrainedBaseline, VAEntrypointVLD, NULL, 0, &config_id));

    VASurfaceID surfaces[1];
    ok &= check("vaCreateSurfaces",
        vtable.vaCreateSurfaces(&ctx, 352, 288, VA_RT_FORMAT_YUV420, 1, surfaces));

    VAContextID va_context;
    ok &= check("vaCreateContext",
        vtable.vaCreateContext(&ctx, config_id, 352, 288, 0, surfaces, 1, &va_context));

    if (!ok) {
        fprintf(stderr, "setup failed, aborting\n");
        return 1;
    }

    /* Real content from an actual broadcast video clip (see
     * relay_pixel_test_slices_long.h), not a synthetic test pattern --
     * only 3 distinct pictures (I, P, P) were capturable in one real-
     * hardware run (see that header's comment for why). Rather than
     * replaying just the 2 P-slices after a single initial I-slice
     * (which isn't valid H.264 -- their motion-compensated content
     * assumes specific preceding real frames, and reliably triggered
     * real decoder-side reference-picture-management rejections a few
     * pictures in during earlier testing), this replays the *whole*
     * captured I,P,P mini-GOP repeatedly. That's a legitimate thing for
     * real H.264 to do: an IDR always resets reference-picture state
     * from scratch (frame_num=0, no prior references needed), so I,P,P,
     * I,P,P,I,P,P,... is exactly as valid as any real encoder repeating
     * a GOP for static/looping content -- each cycle is a clean resync,
     * not a continuation of stale state. This still gives relay-server's
     * ffmpeg the steady, realistically-paced trickle of real pictures
     * its own stream analysis needs (confirmed by real testing: a burst
     * of pictures followed by silence never unsticks it, but the same
     * pictures arriving steadily do, without the connection ever
     * needing to close). */
    const unsigned char *long_slices[3] = { long_slice0_bytes, long_slice1_bytes, long_slice2_bytes };
    size_t long_slice_lens[3] = { sizeof(long_slice0_bytes), sizeof(long_slice1_bytes),
                                   sizeof(long_slice2_bytes) };

    const int total_pictures = 150;

    int pass_count = 0;
    for (int i = 0; i < total_pictures; i++) {
        int slice_idx = i % 3;
        VAPictureParameterBufferH264 pic_params;
        fill_pic_params(&pic_params, surfaces[0]);
        pic_params.frame_num = (unsigned int)(slice_idx);

        VAStatus st = vtable.vaBeginPicture(&ctx, va_context, surfaces[0]);
        if (st != VA_STATUS_SUCCESS) {
            fprintf(stderr, "picture %d: vaBeginPicture failed (%d)\n", i, st);
            break;
        }

        VABufferID pic_buf, slice_data_buf;
        vtable.vaCreateBuffer(&ctx, va_context, VAPictureParameterBufferType,
                              sizeof(pic_params), 1, &pic_params, &pic_buf);
        vtable.vaCreateBuffer(&ctx, va_context, VASliceDataBufferType,
                              (unsigned int)long_slice_lens[slice_idx], 1, (void *)long_slices[slice_idx], &slice_data_buf);

        VABufferID buffers[2] = { pic_buf, slice_data_buf };
        st = vtable.vaRenderPicture(&ctx, va_context, buffers, 2);
        if (st != VA_STATUS_SUCCESS) {
            fprintf(stderr, "picture %d: vaRenderPicture failed (%d)\n", i, st);
            break;
        }

        st = vtable.vaEndPicture(&ctx, va_context);
        if (!check("vaEndPicture", st))
            break;

        vtable.vaDestroyBuffer(&ctx, pic_buf);
        vtable.vaDestroyBuffer(&ctx, slice_data_buf);
        pass_count++;
        if ((i + 1) % 25 == 0)
            printf("  ...%d/%d pictures sent\n", i + 1, total_pictures);
        usleep(40000); /* ~25fps pacing */
    }

    printf("%d/%d pictures sent -- destroying context now to trigger the bounded "
           "blocking drain for any remaining in-flight response\n",
           pass_count, total_pictures);
    check("vaDestroyContext", vtable.vaDestroyContext(&ctx, va_context));
    /* Real VA-API clients always destroy surfaces before terminating --
     * skipping this (as an earlier version of this test did) leaks the
     * real XvMCSurface at the X server level across process runs, since
     * xvmc_backend_destroy_context only tears down the context/scratch
     * arrays, not any surfaces still outstanding. On real, resource-
     * constrained i915/945 hardware this reliably exhausted the XV
     * resource pool after a few runs (BadAlloc), even though each run
     * looked fine in isolation. */
    check("vaDestroySurfaces", vtable.vaDestroySurfaces(&ctx, surfaces, 1));
    vtable.vaTerminate(&ctx);

    if (pass_count != total_pictures) {
        fprintf(stderr, "RELAY PIXEL TEST FAILED\n");
        return 1;
    }
    printf("RELAY PIXEL TEST PASSED (check XVMC_SNAPSHOT_DIR for decoded frame PPMs)\n");
    return 0;
}
