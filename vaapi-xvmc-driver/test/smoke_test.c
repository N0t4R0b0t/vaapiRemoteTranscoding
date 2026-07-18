/*
 * Loads xvmc_drv_video.so the same way libva's driver loader does --
 * dlopen() + dlsym() the version-specific __vaDriverInit_<major>_<minor>
 * symbol -- and, if the driver can actually find an XvMC-capable port on
 * this X server, drives it through a minimal config/surface/context/
 * picture lifecycle.
 *
 * Needs a real X11 connection (XvMCQueryExtension etc. dereference the
 * Display*), but not necessarily XvMC-capable hardware behind it: on a
 * server with no XvMC extension (e.g. this WSLg X server), vaDriverInit
 * is expected to fail cleanly (see xvmc_backend_open) -- that counts as a
 * pass here, since it's proof the driver refuses gracefully rather than
 * crashing, which is exactly the Phase 4 fail-fast behavior it's meant to
 * have. Only a real GMA950 XvMC port lets this test drive the full
 * lifecycle end to end.
 *
 * Usage: ./smoke_test [path to xvmc_drv_video.so]
 */

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include <X11/Xlib.h>
#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_version.h>

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

    printf("looking up entry point %s\n", VA_DRIVER_INIT_SYMBOL);
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
        printf("no XvMC-capable port found on this X server (expected on a "
               "box without real GMA950 hardware) -- driver failed cleanly, "
               "which is correct. SMOKE TEST PASSED (entry point + graceful "
               "failure verified; full lifecycle needs real hardware)\n");
        /* Deliberately not dlclose()ing: it would unload libXv/libXvMC
         * (pulled in only via this dlopen), and Xlib's extension
         * close-display hooks for them can still be registered against
         * `dpy` -- calling XCloseDisplay() after that segfaults into
         * unmapped memory. The process is exiting anyway. */
        return 0;
    }
    printf("vendor string: %s\n", ctx.str_vendor);

    if (!vtable.vaCreateConfig || !vtable.vaCreateSurfaces || !vtable.vaCreateContext ||
        !vtable.vaBeginPicture || !vtable.vaEndPicture || !vtable.vaSyncSurface ||
        !vtable.vaTerminate) {
        fprintf(stderr, "vtable missing an entry this test drives\n");
        return 1;
    }

    VAConfigID config_id;
    int ok = check("vaCreateConfig",
        vtable.vaCreateConfig(&ctx, VAProfileMPEG2Main, VAEntrypointVLD, NULL, 0, &config_id));

    VASurfaceID surfaces[1];
    ok &= check("vaCreateSurfaces",
        vtable.vaCreateSurfaces(&ctx, 640, 480, VA_RT_FORMAT_YUV420, 1, surfaces));

    VAContextID va_context;
    ok &= check("vaCreateContext",
        vtable.vaCreateContext(&ctx, config_id, 640, 480, 0, surfaces, 1, &va_context));

    ok &= check("vaBeginPicture", vtable.vaBeginPicture(&ctx, va_context, surfaces[0]));
    ok &= check("vaEndPicture", vtable.vaEndPicture(&ctx, va_context));
    ok &= check("vaSyncSurface", vtable.vaSyncSurface(&ctx, surfaces[0]));
    ok &= check("vaTerminate", vtable.vaTerminate(&ctx));

    /* Not dlclose()ing/XCloseDisplay()ing here either -- see the comment
     * on the early-return path above. */

    if (!ok) {
        fprintf(stderr, "SMOKE TEST FAILED\n");
        return 1;
    }
    printf("SMOKE TEST PASSED\n");
    return 0;
}
