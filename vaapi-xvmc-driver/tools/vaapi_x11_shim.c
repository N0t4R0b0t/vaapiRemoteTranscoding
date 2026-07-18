/*
 * LD_PRELOAD shim: intercepts vaGetDisplayDRM(fd) -- the entry point
 * apps use to get a VADisplay from a DRM render node -- and instead
 * returns a VADisplay from the real X11 entry point, vaGetDisplay(dpy),
 * using $DISPLAY. VADisplay is just an opaque handle to any caller; as
 * long as nothing downstream does something DRM-specific with the raw
 * fd itself (dmabuf import/export, PRIME buffer sharing), everything
 * from vaInitialize onward is display-implementation-agnostic and
 * should work identically regardless of which path produced the handle.
 *
 * Point at this driver like normal (LIBVA_DRIVER_NAME=xvmc), then run
 * the real app under LD_PRELOAD=/path/to/vaapi_x11_shim.so.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <va/va.h>
#include <va/va_x11.h>

static Display *g_shim_dpy = NULL;

VADisplay vaGetDisplayDRM(int fd)
{
    (void)fd;
    if (!g_shim_dpy) {
        const char *disp = getenv("DISPLAY");
        g_shim_dpy = XOpenDisplay(disp);
        if (!g_shim_dpy) {
            fprintf(stderr, "[vaapi_x11_shim] XOpenDisplay(%s) failed\n", disp ? disp : "(null)");
            return NULL;
        }
        fprintf(stderr, "[vaapi_x11_shim] redirecting vaGetDisplayDRM(fd=%d) -> "
                        "vaGetDisplay(XOpenDisplay(\"%s\"))\n", fd, disp ? disp : "(default)");
    }
    return vaGetDisplay(g_shim_dpy);
}
