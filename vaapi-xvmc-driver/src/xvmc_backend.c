#include "xvmc_backend.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/ipc.h>
#include <sys/shm.h>

#include <X11/Xutil.h>
#include <X11/extensions/Xvlib.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/XvMC.h>

/*
 * XvMCPutSurface returning Success only means the X server accepted the
 * request -- it says nothing about whether the real overlay hardware
 * has actually finished compositing into readable memory yet. Confirmed
 * a real, live race by real testing: reading pixels back (via
 * xvmc_backend_get_surface_rgb, used by the new vaGetImage path) right
 * after PutSurface+XSync showed the classic Xv colorkey color (a
 * distinctive solid green) instead of real decoded content on many
 * frames, only catching real content once incidental delays elsewhere
 * gave the hardware enough time to catch up -- exactly the signature of
 * reading before the overlay compositing completed, not a color-space
 * bug. XvMCGetSurfaceStatus's XVMC_DISPLAYING bit is specifically for
 * this: it stays set for as long as a surface is actively being
 * displayed by a real XvMCPutSurface call, so polling it to clear
 * before reading pixels back is the correct wait, unlike XSync (which
 * only waits for the X server's request queue, not real hardware
 * completion). Bounded to avoid hanging forever if something's
 * genuinely wrong -- better to return possibly-stale pixels than to
 * block a real caller (e.g. ffmpeg's decode thread) indefinitely. */
static void wait_for_display_done(struct xvmc_backend *be, int surface_index)
{
    int prof = getenv("XVMC_PROFILE") != NULL;
    int i;
    for (i = 0; i < 200; i++) {
        int status = 0;
        if (XvMCGetSurfaceStatus(be->dpy, &be->surfaces[surface_index], &status) != Success) {
            if (prof) fprintf(stderr, "[prof] wait_for_display_done: GetSurfaceStatus failed at iter %d\n", i);
            return;
        }
        if (!(status & XVMC_DISPLAYING)) {
            if (prof) fprintf(stderr, "[prof] wait_for_display_done: cleared after %d iters (~%dms)\n", i, i);
            return;
        }
        usleep(1000);
    }
    fprintf(stderr, "xvmc_backend: wait_for_display_done: timed out waiting for "
                     "XVMC_DISPLAYING to clear on surface %d\n", surface_index);
}

/* mc_type packs codec (low 16 bits, XVMC_MPEG_2 etc.) and acceleration
 * level (high 16 bits, XVMC_MOCOMP=0 or XVMC_IDCT) into one field -- see
 * X11/extensions/XvMC.h. */
static int find_port(Display *dpy, int screen, XvPortID *out_port,
                      int *out_surface_type_id, int *out_mc_type,
                      int *out_surface_flags, int require_idct)
{
    Window root = RootWindow(dpy, screen);
    unsigned int num_adaptors = 0;
    XvAdaptorInfo *adaptors = NULL;

    if (XvQueryAdaptors(dpy, root, &num_adaptors, &adaptors) != Success)
        return -1;

    int found = 0;
    for (unsigned int a = 0; a < num_adaptors && !found; a++) {
        XvAdaptorInfo *adaptor = &adaptors[a];
        for (unsigned long p = 0; p < adaptor->num_ports && !found; p++) {
            XvPortID port = adaptor->base_id + p;
            int num_types = 0;
            XvMCSurfaceInfo *types = XvMCListSurfaceTypes(dpy, port, &num_types);
            if (!types)
                continue;

            for (int s = 0; s < num_types; s++) {
                int codec = types[s].mc_type & 0xffff;
                int accel = types[s].mc_type & 0xffff0000;
                if (codec != XVMC_MPEG_2 || types[s].chroma_format != XVMC_CHROMA_FORMAT_420)
                    continue;
                if (require_idct && accel != XVMC_IDCT)
                    continue;

                *out_port = port;
                *out_surface_type_id = types[s].surface_type_id;
                *out_mc_type = types[s].mc_type;
                *out_surface_flags = types[s].flags;
                found = 1;
                break;
            }
            XFree(types);
        }
    }

    if (adaptors)
        XvFreeAdaptorInfo(adaptors);
    return found ? 0 : -1;
}

int xvmc_backend_open(struct xvmc_backend *be, Display *dpy, int screen)
{
    memset(be, 0, sizeof(*be));
    be->dpy = dpy;
    be->screen = screen;

    int event_base, err_base;
    if (!XvMCQueryExtension(dpy, &event_base, &err_base)) {
        fprintf(stderr, "xvmc_backend: XvMC extension not present on this X server\n");
        return -1;
    }

    if (find_port(dpy, screen, &be->port, &be->surface_type_id, &be->mc_type,
                  &be->surface_flags, 1) != 0 &&
        find_port(dpy, screen, &be->port, &be->surface_type_id, &be->mc_type,
                  &be->surface_flags, 0) != 0) {
        fprintf(stderr, "xvmc_backend: no MPEG-2 4:2:0 XvMC port found\n");
        return -1;
    }

    fprintf(stderr, "xvmc_backend: using port %lu, surface_type_id %d, mc_type 0x%x (%s), "
                     "flags 0x%x (XVMC_INTRA_UNSIGNED %s)\n",
            (unsigned long)be->port, be->surface_type_id, be->mc_type,
            (be->mc_type & 0xffff0000) == XVMC_IDCT ? "IDCT" : "MoComp",
            be->surface_flags,
            (be->surface_flags & XVMC_INTRA_UNSIGNED) ? "set" : "NOT set");
    return 0;
}

int xvmc_backend_create_context(struct xvmc_backend *be, int width, int height)
{
    if (be->context_created)
        return 0;

    if (width > XVMC_BACKEND_MAX_WIDTH || height > XVMC_BACKEND_MAX_HEIGHT) {
        fprintf(stderr, "xvmc_backend: %dx%d exceeds this real hardware's XvMC "
                         "ceiling (%dx%d, classic PAL broadcast SD) -- refusing "
                         "before making the X call that would otherwise raise a "
                         "fatal protocol error\n",
                width, height, XVMC_BACKEND_MAX_WIDTH, XVMC_BACKEND_MAX_HEIGHT);
        return XVMC_BACKEND_RESOLUTION_UNSUPPORTED;
    }

    if (XvMCCreateContext(be->dpy, be->port, be->surface_type_id,
                           width, height, XVMC_DIRECT, &be->context) != Success) {
        fprintf(stderr, "xvmc_backend: XvMCCreateContext failed\n");
        return -1;
    }

    be->max_macroblocks = (unsigned int)(((width + 15) / 16) * ((height + 15) / 16));

    if (XvMCCreateMacroBlocks(be->dpy, &be->context, be->max_macroblocks, &be->macroblocks) != Success) {
        fprintf(stderr, "xvmc_backend: XvMCCreateMacroBlocks failed\n");
        XvMCDestroyContext(be->dpy, &be->context);
        return -1;
    }
    if (XvMCCreateBlocks(be->dpy, &be->context, be->max_macroblocks * 6, &be->blocks) != Success) {
        fprintf(stderr, "xvmc_backend: XvMCCreateBlocks failed\n");
        XvMCDestroyMacroBlocks(be->dpy, &be->macroblocks);
        XvMCDestroyContext(be->dpy, &be->context);
        return -1;
    }

    be->width = width;
    be->height = height;
    be->context_created = 1;
    return 0;
}

void xvmc_backend_destroy_context(struct xvmc_backend *be)
{
    if (!be->context_created)
        return;

    /* Every surface still marked in-use is invalidated the moment the
     * owning XvMCContext is destroyed below (XvMC surfaces can't outlive
     * their context) -- so leaving them marked in-use here would be
     * inconsistent even if no caller ever leaked them. Real, confirmed
     * consequence when this wasn't done: a caller (ffmpeg) that
     * recreates its hwaccel context after an error (e.g. the relay
     * connection dying mid-stream) without first calling
     * xvmc_DestroySurfaces on the old ones exhausted the fixed 32-slot
     * pool (XVMC_BACKEND_MAX_SURFACES) on its very next attempt, since
     * nothing here ever freed the old, now-orphaned slots. */
    for (int i = 0; i < XVMC_BACKEND_MAX_SURFACES; i++)
        xvmc_backend_destroy_surface(be, i);

    if (be->scratch_win_created) {
        XDestroyWindow(be->dpy, be->scratch_win);
        be->scratch_win_created = 0;
    }
    if (be->shm_image) {
        XShmDetach(be->dpy, &be->shm_info);
        XDestroyImage(be->shm_image);
        shmdt(be->shm_info.shmaddr);
        shmctl(be->shm_info.shmid, IPC_RMID, NULL);
        be->shm_image = NULL;
    }
    /* Reset alongside shm_image -- otherwise a subsequent context (see
     * above: same real scenario that leaked surfaces) would see
     * shm_attempted/shm_available still set from the old context and
     * skip re-creating the XShm image entirely, then crash dereferencing
     * the now-NULL be->shm_image on the first XShmGetImage call. */
    be->shm_attempted = 0;
    be->shm_available = 0;
    be->shm_width = 0;
    be->shm_height = 0;

    XvMCDestroyBlocks(be->dpy, &be->blocks);
    XvMCDestroyMacroBlocks(be->dpy, &be->macroblocks);
    XvMCDestroyContext(be->dpy, &be->context);
    be->context_created = 0;
}

int xvmc_backend_create_surface(struct xvmc_backend *be)
{
    if (!be->context_created) {
        fprintf(stderr, "xvmc_backend: create_surface called before create_context\n");
        return -1;
    }

    for (int i = 0; i < XVMC_BACKEND_MAX_SURFACES; i++) {
        if (be->surface_in_use[i])
            continue;
        if (XvMCCreateSurface(be->dpy, &be->context, &be->surfaces[i]) != Success) {
            fprintf(stderr, "xvmc_backend: XvMCCreateSurface failed\n");
            return -1;
        }
        be->surface_in_use[i] = 1;
        return i;
    }

    fprintf(stderr, "xvmc_backend: out of surface slots (max %d)\n", XVMC_BACKEND_MAX_SURFACES);
    return -1;
}

void xvmc_backend_destroy_surface(struct xvmc_backend *be, int surface_index)
{
    if (surface_index < 0 || surface_index >= XVMC_BACKEND_MAX_SURFACES)
        return;
    if (!be->surface_in_use[surface_index])
        return;
    XvMCDestroySurface(be->dpy, &be->surfaces[surface_index]);
    be->surface_in_use[surface_index] = 0;
}

/* Extracts an 8-bit channel from a packed pixel value given that
 * channel's mask, regardless of which bit position/width the X server's
 * visual happens to use for it (e.g. 5/6/5 vs 8/8/8 packing). */
static unsigned int channel_from_mask(unsigned long pixel, unsigned long mask)
{
    if (mask == 0)
        return 0;
    unsigned int shift = 0;
    while (!((mask >> shift) & 1))
        shift++;
    unsigned int width = 0;
    while ((mask >> (shift + width)) & 1)
        width++;
    unsigned long value = (pixel & mask) >> shift;
    unsigned long max_value = (1UL << width) - 1;
    return (unsigned int)(value * 255 / max_value);
}

/* XGetPixel recomputes each channel's shift/width from its mask on
 * every single call, and channel_from_mask above does the same with a
 * bit-counting loop -- fine for a one-shot debug snapshot, but real
 * testing confirmed this is a genuinely significant bottleneck once
 * this same path runs once per decoded frame during real playback
 * (vaGetImage, see xvmc_backend_get_surface_rgb). Real hardware here
 * has always reported the same fixed layout (confirmed via the
 * one-time debug print in xvmc_backend_snapshot_surface: depth=24
 * bpp=32, LSB-first, red=0xff0000 green=0xff00 blue=0xff) -- for
 * exactly that layout, read each row's raw bytes directly instead.
 * Falls back to the general (slow) per-pixel path for any other
 * layout, which real testing here has never actually seen but isn't
 * safe to assume unconditionally on a different X server/visual. */
static void fill_rgb_from_ximage(XImage *img, int width, int height, unsigned char *rgb_out)
{
    if (img->bits_per_pixel == 32 && img->byte_order == LSBFirst &&
        img->red_mask == 0xff0000UL && img->green_mask == 0xff00UL && img->blue_mask == 0xffUL) {
        for (int y = 0; y < height; y++) {
            const unsigned char *row = (const unsigned char *)img->data + (size_t)y * img->bytes_per_line;
            unsigned char *dst_row = rgb_out + (size_t)y * width * 3;
            for (int x = 0; x < width; x++) {
                const unsigned char *px = row + (size_t)x * 4;
                dst_row[x * 3 + 0] = px[2]; /* red byte */
                dst_row[x * 3 + 1] = px[1]; /* green byte */
                dst_row[x * 3 + 2] = px[0]; /* blue byte */
            }
        }
        return;
    }

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            unsigned long pixel = XGetPixel(img, x, y);
            unsigned char *dst = rgb_out + (size_t)(y * width + x) * 3;
            dst[0] = (unsigned char)channel_from_mask(pixel, img->red_mask);
            dst[1] = (unsigned char)channel_from_mask(pixel, img->green_mask);
            dst[2] = (unsigned char)channel_from_mask(pixel, img->blue_mask);
        }
    }
}

int xvmc_backend_snapshot_surface(struct xvmc_backend *be, int surface_index, const char *path)
{
    if (surface_index < 0 || surface_index >= XVMC_BACKEND_MAX_SURFACES ||
        !be->surface_in_use[surface_index]) {
        fprintf(stderr, "xvmc_backend_snapshot_surface: invalid surface %d\n", surface_index);
        return -1;
    }

    /* XvMCPutSurface needs a real, mapped (visible) Window -- confirmed
     * by real testing that an offscreen Pixmap target reliably fails
     * with BadAlloc on this real i915 hardware, even though nothing in
     * the XvMC API contract itself requires a Window specifically. Real
     * Xv/XvMC display consumers (video players) always put to an
     * onscreen window for exactly this reason; this is the first thing
     * in this project that's ever needed to actually *display* a
     * surface rather than just render into it, so this gap was never
     * hit before. */
    /* override_redirect bypasses the window manager's own placement
     * policy entirely -- confirmed necessary by real testing: a plain
     * XCreateSimpleWindow's (0,0) position request was silently ignored
     * by the real desktop's window manager (xfwm4), which placed the
     * window somewhere else on screen, so reading back from root at
     * (0,0) just captured the desktop underneath instead of this
     * window. */
    Window root = RootWindow(be->dpy, be->screen);
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    Window win = XCreateWindow(be->dpy, root, 0, 0,
                                (unsigned int)be->width, (unsigned int)be->height,
                                0, CopyFromParent, InputOutput, CopyFromParent,
                                CWOverrideRedirect, &attrs);
    if (!win) {
        fprintf(stderr, "xvmc_backend_snapshot_surface: XCreateWindow failed\n");
        return -1;
    }
    /* Select for MapNotify BEFORE mapping -- an override_redirect window
     * maps immediately with no window-manager round trip, so registering
     * interest afterward can lose the race and miss the event entirely,
     * hanging forever in the wait loop below (confirmed by real
     * testing). */
    XSelectInput(be->dpy, win, StructureNotifyMask);
    XMapWindow(be->dpy, win);
    XRaiseWindow(be->dpy, win);
    /* Block until the window is actually viewable -- XvMCPutSurface
     * against a not-yet-mapped window is exactly the kind of thing that
     * silently misbehaves rather than erroring cleanly. */
    XEvent ev;
    do {
        XNextEvent(be->dpy, &ev);
    } while (ev.type != MapNotify);

    Status st = XvMCPutSurface(be->dpy, &be->surfaces[surface_index], win,
                                0, 0, (unsigned short)be->width, (unsigned short)be->height,
                                0, 0, (unsigned short)be->width, (unsigned short)be->height,
                                XVMC_FRAME_PICTURE);
    if (st != Success) {
        fprintf(stderr, "xvmc_backend_snapshot_surface: XvMCPutSurface failed (%d)\n", (int)st);
        XDestroyWindow(be->dpy, win);
        return -1;
    }
    XSync(be->dpy, False);
    wait_for_display_done(be, surface_index);

    /* Read from the root window at the target window's screen position
     * rather than the window itself -- a real, uniformly color-keyed
     * magenta/pink cast (R and B channels always pinned at 255) seen in
     * every early decoded snapshot was first suspected to be an overlay-
     * vs-backing-store capture issue (true hardware video overlay
     * composites to the display scanout directly, bypassing a window's
     * own backing store). That theory is now disproven: once
     * override_redirect (below) guaranteed this window actually sits at
     * screen (0,0) -- confirmed by this same root read finally showing
     * this window's own content instead of the desktop underneath it --
     * the magenta cast was still there, identical, meaning the content
     * XvMCPutSurface actually wrote really is wrong, not just
     * miscaptured. The real bug is upstream of display entirely (almost
     * certainly a chroma reconstruction issue in mpeg2_vld.c, not yet
     * root-caused). Reading via root is kept anyway since it's provably
     * correct now and doesn't depend on the window manager leaving this
     * window on top. */
    XImage *img = XGetImage(be->dpy, root, 0, 0, (unsigned int)be->width,
                             (unsigned int)be->height, AllPlanes, ZPixmap);
    XDestroyWindow(be->dpy, win);
    if (!img) {
        fprintf(stderr, "xvmc_backend_snapshot_surface: XGetImage failed\n");
        return -1;
    }
    static int printed_format = 0;
    if (!printed_format) {
        printed_format = 1;
        fprintf(stderr, "[snapshot] depth=%d bpp=%d byte_order=%d bitmap_bit_order=%d "
                         "red_mask=0x%lx green_mask=0x%lx blue_mask=0x%lx\n",
                img->depth, img->bits_per_pixel, img->byte_order, img->bitmap_bit_order,
                img->red_mask, img->green_mask, img->blue_mask);
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "xvmc_backend_snapshot_surface: fopen(%s) failed\n", path);
        XDestroyImage(img);
        return -1;
    }
    fprintf(fp, "P6\n%d %d\n255\n", be->width, be->height);
    unsigned char *rgb = malloc((size_t)be->width * be->height * 3);
    if (rgb) {
        fill_rgb_from_ximage(img, be->width, be->height, rgb);
        fwrite(rgb, 1, (size_t)be->width * be->height * 3, fp);
        free(rgb);
    }
    fclose(fp);
    XDestroyImage(img);
    return 0;
}

/* Asks the WM (via the standard EWMH mechanism) to make `win` a real,
 * undecorated fullscreen window -- a raw XMoveResizeWindow alone still
 * leaves a reparenting WM's own title-bar/border frame wrapped around
 * it. Real players like mpv/vlc use this exact same ClientMessage for
 * their own fullscreen toggle, so any EWMH-compliant WM (which is
 * effectively all of them, including xfwm4, confirmed on this real
 * netbook) already knows how to honor it. Best-effort: if the WM
 * ignores it, XMoveResizeWindow (done every frame regardless) still
 * gets the size/position right, just with decorations left on. */
static void request_wm_fullscreen(struct xvmc_backend *be, Window win)
{
    Atom wm_state = XInternAtom(be->dpy, "_NET_WM_STATE", False);
    Atom wm_fullscreen = XInternAtom(be->dpy, "_NET_WM_STATE_FULLSCREEN", False);
    if (wm_state == None || wm_fullscreen == None)
        return;

    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = wm_state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 1; /* _NET_WM_STATE_ADD */
    ev.xclient.data.l[1] = (long)wm_fullscreen;
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = 1; /* source indication: normal application */

    Window root = RootWindow(be->dpy, be->screen);
    XSendEvent(be->dpy, root, False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    XFlush(be->dpy);
}

int xvmc_backend_put_surface(struct xvmc_backend *be, int surface_index, Drawable draw,
                              short srcx, short srcy, unsigned short srcw, unsigned short srch,
                              short destx, short desty, unsigned short destw, unsigned short desth)
{
    if (surface_index < 0 || surface_index >= XVMC_BACKEND_MAX_SURFACES ||
        !be->surface_in_use[surface_index]) {
        fprintf(stderr, "xvmc_backend_put_surface: invalid surface %d\n", surface_index);
        return -1;
    }
    (void)destx; (void)desty; (void)destw; (void)desth;

    if (be->fullscreen_win != draw) {
        request_wm_fullscreen(be, draw);
        be->fullscreen_win = draw;
    }

    /* Force `draw` itself -- the caller's own window, the one it thinks
     * it's rendering into -- to cover the whole screen. This is the
     * caller's own window being resized by its own X connection, which
     * X permits unconditionally regardless of what the WM would
     * otherwise allow; the caller never finds out and keeps calling
     * vaPutSurface exactly as it always did, unaware its window now
     * spans the whole display. Done every call (not just once) so a
     * WM or user un-maximizing it gets silently overridden back on the
     * next frame. */
    int screen_w = DisplayWidth(be->dpy, be->screen);
    int screen_h = DisplayHeight(be->dpy, be->screen);
    XMoveResizeWindow(be->dpy, draw, 0, 0, (unsigned int)screen_w, (unsigned int)screen_h);

    /* Ignore the caller's own dest rect entirely and letterbox/
     * pillarbox the source frame to fill that now-screen-sized window
     * while preserving its aspect ratio, centered. XvMC scales this in
     * hardware as part of the same blit, so this costs nothing extra
     * over blitting at the caller's original size. */
    double scale = screen_w / (double)be->width;
    if (be->height * scale > screen_h)
        scale = screen_h / (double)be->height;
    unsigned short dw = (unsigned short)(be->width * scale + 0.5);
    unsigned short dh = (unsigned short)(be->height * scale + 0.5);
    short dx = (short)((screen_w - dw) / 2);
    short dy = (short)((screen_h - dh) / 2);

    Status st = XvMCPutSurface(be->dpy, &be->surfaces[surface_index], draw,
                                srcx, srcy, srcw, srch,
                                dx, dy, dw, dh,
                                XVMC_FRAME_PICTURE);
    if (st != Success) {
        fprintf(stderr, "xvmc_backend_put_surface: XvMCPutSurface failed (%d)\n", (int)st);
        return -1;
    }
    return 0;
}

static int ensure_scratch_window(struct xvmc_backend *be, int need_width, int need_height)
{
    if (be->scratch_win_created) {
        if (need_width <= be->scratch_win_width && need_height <= be->scratch_win_height)
            return 0;
        /* Grow (never shrink) -- see this field's header comment for why
         * this only happens at most once in practice. */
        int new_w = need_width > be->scratch_win_width ? need_width : be->scratch_win_width;
        int new_h = need_height > be->scratch_win_height ? need_height : be->scratch_win_height;
        XResizeWindow(be->dpy, be->scratch_win, (unsigned int)new_w, (unsigned int)new_h);
        XSync(be->dpy, False);
        be->scratch_win_width = new_w;
        be->scratch_win_height = new_h;
        return 0;
    }

    /* Same override_redirect + pre-map StructureNotifyMask technique
     * verified necessary and sufficient by xvmc_backend_snapshot_surface
     * -- see that function's real-testing notes on window-manager
     * placement and MapNotify race conditions. */
    Window root = RootWindow(be->dpy, be->screen);
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    Window win = XCreateWindow(be->dpy, root, 0, 0,
                                (unsigned int)need_width, (unsigned int)need_height,
                                0, CopyFromParent, InputOutput, CopyFromParent,
                                CWOverrideRedirect, &attrs);
    if (!win) {
        fprintf(stderr, "xvmc_backend: ensure_scratch_window: XCreateWindow failed\n");
        return -1;
    }
    XSelectInput(be->dpy, win, StructureNotifyMask);
    XMapWindow(be->dpy, win);
    XEvent ev;
    do {
        XNextEvent(be->dpy, &ev);
    } while (ev.type != MapNotify);

    be->scratch_win = win;
    be->scratch_win_created = 1;
    be->scratch_win_width = need_width;
    be->scratch_win_height = need_height;
    return 0;
}

/* Sets up a MIT-SHM-backed XImage once, reused for the backend's whole
 * lifetime by xvmc_backend_get_surface_rgb. Real, measured win over
 * plain XGetImage: transfers the pixel data through a shared memory
 * segment both processes can read directly, instead of serializing it
 * through the core X protocol -- confirmed via real profiling that
 * plain XGetImage of one 640x480 frame took 25-45ms, the single
 * largest cost in the whole vaGetImage path. Sets be->shm_available
 * false (falling back to the always-correct, just slower, XGetImage
 * path) if anything here fails, since MIT-SHM -- confirmed present on
 * the real X server this driver was developed against -- isn't
 * guaranteed on every X server this driver might run against. */
static void ensure_shm_image(struct xvmc_backend *be, int need_width, int need_height)
{
    if (need_width < be->shm_width) need_width = be->shm_width; /* never shrink */
    if (need_height < be->shm_height) need_height = be->shm_height;
    if (need_width < be->width) need_width = be->width;
    if (need_height < be->height) need_height = be->height;

    if (be->shm_attempted) {
        if (!be->shm_available)
            return; /* already know MIT-SHM isn't usable, no point retrying at any size */
        if (need_width <= be->shm_width && need_height <= be->shm_height)
            return;
        /* Grow: tear down and recreate at the new, larger size. See
         * scratch_win_width/height's comment for why this only happens
         * at most once in practice. */
        XShmDetach(be->dpy, &be->shm_info);
        XDestroyImage(be->shm_image);
        shmdt(be->shm_info.shmaddr);
        be->shm_image = NULL;
        be->shm_available = 0;
    }
    be->shm_attempted = 1;
    be->shm_width = need_width;
    be->shm_height = need_height;

    if (!XShmQueryExtension(be->dpy)) {
        fprintf(stderr, "xvmc_backend: MIT-SHM not available, falling back to XGetImage\n");
        return;
    }

    XImage *img = XShmCreateImage(be->dpy, DefaultVisual(be->dpy, be->screen), 24, ZPixmap,
                                   NULL, &be->shm_info, (unsigned int)be->shm_width, (unsigned int)be->shm_height);
    if (!img) {
        fprintf(stderr, "xvmc_backend: XShmCreateImage failed, falling back to XGetImage\n");
        return;
    }

    be->shm_info.shmid = shmget(IPC_PRIVATE, (size_t)img->bytes_per_line * (size_t)img->height,
                                 IPC_CREAT | 0600);
    if (be->shm_info.shmid < 0) {
        fprintf(stderr, "xvmc_backend: shmget failed (%s), falling back to XGetImage\n", strerror(errno));
        XDestroyImage(img);
        return;
    }
    be->shm_info.shmaddr = shmat(be->shm_info.shmid, NULL, 0);
    if (be->shm_info.shmaddr == (void *)-1) {
        fprintf(stderr, "xvmc_backend: shmat failed (%s), falling back to XGetImage\n", strerror(errno));
        shmctl(be->shm_info.shmid, IPC_RMID, NULL);
        XDestroyImage(img);
        return;
    }
    img->data = be->shm_info.shmaddr;
    be->shm_info.readOnly = False;

    if (!XShmAttach(be->dpy, &be->shm_info)) {
        fprintf(stderr, "xvmc_backend: XShmAttach failed, falling back to XGetImage\n");
        shmdt(be->shm_info.shmaddr);
        shmctl(be->shm_info.shmid, IPC_RMID, NULL);
        XDestroyImage(img);
        return;
    }
    XSync(be->dpy, False);
    /* Marking the segment for destruction now is safe and standard
     * practice -- the kernel keeps it alive until both this process and
     * the X server (which attached its own mapping in XShmAttach) detach
     * it, so it won't actually go away underneath either side. */
    shmctl(be->shm_info.shmid, IPC_RMID, NULL);

    be->shm_image = img;
    be->shm_available = 1;
}

static double prof_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* Shared by xvmc_backend_get_surface_rgb and xvmc_backend_get_surface_nv12:
 * blits `surface_index` onto the scratch window, scaled to dst_width x
 * dst_height (real hardware scaling as part of the same XvMCPutSurface
 * call -- pass be->width/be->height for a native-size, unscaled read,
 * as xvmc_backend_get_surface_rgb always does), and reads it back at
 * that size, preferring XShmGetImage (real, measured win over plain
 * XGetImage -- see ensure_shm_image's comment) and falling back to the
 * always-correct, just slower, core-protocol XGetImage if MIT-SHM isn't
 * available. Returns the XImage (be->shm_image, reused across calls,
 * or a throwaway one the caller must XDestroyImage) via *out_using_shm,
 * or NULL on failure. `*t0`/`*t1`/`*t2` receive profiling timestamps
 * when non-NULL (XVMC_PROFILE); callers add their own conversion-step
 * timestamp after this returns. */
static XImage *acquire_surface_image(struct xvmc_backend *be, int surface_index,
                                      int dst_width, int dst_height,
                                      int *out_using_shm, double *t0, double *t1, double *t2)
{
    int prof = getenv("XVMC_PROFILE") != NULL;
    if (prof && t0) *t0 = prof_now_ms();

    if (surface_index < 0 || surface_index >= XVMC_BACKEND_MAX_SURFACES ||
        !be->surface_in_use[surface_index]) {
        fprintf(stderr, "acquire_surface_image: invalid surface %d\n", surface_index);
        return NULL;
    }
    if (ensure_scratch_window(be, dst_width, dst_height) != 0)
        return NULL;

    Status st = XvMCPutSurface(be->dpy, &be->surfaces[surface_index], be->scratch_win,
                                0, 0, (unsigned short)be->width, (unsigned short)be->height,
                                0, 0, (unsigned short)dst_width, (unsigned short)dst_height,
                                XVMC_FRAME_PICTURE);
    if (st != Success) {
        fprintf(stderr, "acquire_surface_image: XvMCPutSurface failed (%d)\n", (int)st);
        return NULL;
    }
    XSync(be->dpy, False);
    if (prof && t1) *t1 = prof_now_ms();
    wait_for_display_done(be, surface_index);
    if (prof && t2) *t2 = prof_now_ms();

    Window root = RootWindow(be->dpy, be->screen);
    ensure_shm_image(be, dst_width, dst_height);

    XImage *img;
    int using_shm = be->shm_available;
    if (using_shm) {
        if (!XShmGetImage(be->dpy, root, be->shm_image, 0, 0, AllPlanes)) {
            fprintf(stderr, "acquire_surface_image: XShmGetImage failed, "
                             "falling back to XGetImage this call\n");
            using_shm = 0;
        } else {
            img = be->shm_image;
        }
    }
    if (!using_shm) {
        img = XGetImage(be->dpy, root, 0, 0, (unsigned int)dst_width,
                         (unsigned int)dst_height, AllPlanes, ZPixmap);
        if (!img) {
            fprintf(stderr, "acquire_surface_image: XGetImage failed\n");
            return NULL;
        }
    }
    *out_using_shm = using_shm;
    return img;
}

int xvmc_backend_get_surface_rgb(struct xvmc_backend *be, int surface_index, unsigned char *rgb_out)
{
    int prof = getenv("XVMC_PROFILE") != NULL;
    double t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0;
    int using_shm = 0;

    XImage *img = acquire_surface_image(be, surface_index, be->width, be->height,
                                         &using_shm, &t0, &t1, &t2);
    if (!img)
        return -1;
    if (prof) t3 = prof_now_ms();

    fill_rgb_from_ximage(img, be->width, be->height, rgb_out);
    if (!using_shm)
        XDestroyImage(img);
    if (prof) {
        t4 = prof_now_ms();
        fprintf(stderr, "[prof] get_surface_rgb: putsurface+sync=%.1fms wait_display=%.1fms "
                        "%s=%.1fms convert=%.1fms total=%.1fms\n",
                t1 - t0, t2 - t1, using_shm ? "xshmgetimage" : "xgetimage", t3 - t2, t4 - t3, t4 - t0);
    }
    return 0;
}

static inline uint8_t clamp_u8_be(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

#if defined(__SSSE3__)
#include <emmintrin.h>
#include <tmmintrin.h>

/* SIMD Y-plane computation (4 pixels/instruction) -- real profiling
 * identified this conversion as a genuine, non-trivial cost during any
 * transfer-based playback path (see xvmc_GetImage's provenance notes).
 * Uses SSE2/SSSE3 intrinsics rather than hand-written assembly: they
 * compile to the exact same instructions with far less risk of a
 * transcription bug in something this correctness-critical, and this
 * project has held strict bit-exact verification through every prior
 * optimization -- confirmed identical to the scalar computation below
 * via the same checksum/pixel-diff methodology before deploying.
 *
 * Per pixel, computes (77*R + 150*G + 29*B) >> 8 exactly like the
 * scalar path, just four pixels per iteration: unpacks each 4-byte
 * BGRA pixel to 16-bit lanes, multiply-accumulates B+G and R+A
 * (A's weight is 0) via pmaddwd, horizontally adds those two partial
 * sums per pixel, shifts, and packs back down to bytes. The intended
 * result is always naturally in 0-255 range for real 0-255 input (the
 * weights sum to exactly 256), so the packing here can't actually
 * overflow -- unlike the scalar path's clamp_u8_be, which exists only
 * as a defensive fallback-path safety net, not because real overflow
 * is expected.
 *
 * A dedicated pass over the whole Y plane, separate from chroma's
 * scalar 2x2-box-average pass below (rather than one fully fused
 * pass) -- chroma's box averaging doesn't vectorize cleanly at a
 * useful width here, and real testing confirmed the wide, low-
 * overhead SIMD win on Y more than pays for reading each row's bytes
 * a second time in the (separate, scalar) chroma pass. */
static void fill_y_plane_simd(const unsigned char *base, int stride, int width, int height, uint8_t *y_out)
{
    const __m128i weights = _mm_set_epi16(0, 77, 150, 29, 0, 77, 150, 29);
    const __m128i zero = _mm_setzero_si128();

    for (int y = 0; y < height; y++) {
        const unsigned char *row = base + (size_t)y * stride;
        uint8_t *y_row = y_out + (size_t)y * width;
        int x = 0;
        for (; x + 4 <= width; x += 4) {
            __m128i pix = _mm_loadu_si128((const __m128i *)(row + (size_t)x * 4));
            __m128i lo16 = _mm_unpacklo_epi8(pix, zero);
            __m128i hi16 = _mm_unpackhi_epi8(pix, zero);
            __m128i madd_lo = _mm_madd_epi16(lo16, weights);
            __m128i madd_hi = _mm_madd_epi16(hi16, weights);
            __m128i y32 = _mm_hadd_epi32(madd_lo, madd_hi);
            y32 = _mm_srli_epi32(y32, 8);
            __m128i y16 = _mm_packs_epi32(y32, y32);
            __m128i y8 = _mm_packus_epi16(y16, y16);
            uint32_t y4 = (uint32_t)_mm_cvtsi128_si32(y8);
            memcpy(y_row + x, &y4, 4);
        }
        /* Remainder -- never actually taken given width is always a
         * multiple of 16 (macroblock-aligned), kept only for safety if
         * this is ever reused against a non-macroblock-aligned width. */
        for (; x < width; x++) {
            const unsigned char *px = row + (size_t)x * 4;
            y_row[x] = clamp_u8_be((77 * px[2] + 150 * px[1] + 29 * px[0]) >> 8);
        }
    }
}

/* SIMD chroma computation, 4 output UV-blocks (8 source pixels, 2 rows)
 * per iteration -- same bit-exact-verified-first discipline as
 * fill_y_plane_simd above. The 2x2 box average doesn't reduce to a
 * single wide horizontal-add the way Y's per-pixel weighting does
 * (each output needs 4 *different* source pixels summed, 2 from each
 * row), so this sums adjacent pixel pairs within each row first
 * (unpack + shift-by-8-bytes + add, since pmaddwd/hadd can't directly
 * add non-adjacent lanes), then adds the two rows, then applies the
 * same BT.601 forward transform as the scalar path via pmaddwd+hadd,
 * mirroring fill_y_plane_simd's own structure. packus_epi16 at the end
 * saturates exactly like clamp_u8_be (verified, not assumed, by the
 * standalone fuzz test this was checked against before being wired in
 * here -- including deliberately adversarial all-0/all-255/checkerboard
 * input to stress the saturation path, since Cb/Cr's weights include
 * negative terms unlike Y's all-positive ones). */
static void fill_uv_row_simd(const unsigned char *row0, const unsigned char *row1, int width, uint8_t *uv_row)
{
    const __m128i cb_weights = _mm_set_epi16(0, -43, -85, 128, 0, -43, -85, 128);
    const __m128i cr_weights = _mm_set_epi16(0, 128, -107, -21, 0, 128, -107, -21);
    const __m128i zero = _mm_setzero_si128();
    const __m128i bias128 = _mm_set1_epi32(128);

    int x = 0;
    for (; x + 8 <= width; x += 8) {
        __m128i r0a = _mm_loadu_si128((const __m128i *)(row0 + (size_t)x * 4));
        __m128i r0b = _mm_loadu_si128((const __m128i *)(row0 + (size_t)(x + 4) * 4));
        __m128i r1a = _mm_loadu_si128((const __m128i *)(row1 + (size_t)x * 4));
        __m128i r1b = _mm_loadu_si128((const __m128i *)(row1 + (size_t)(x + 4) * 4));

        __m128i r0a_lo = _mm_unpacklo_epi8(r0a, zero), r0a_hi = _mm_unpackhi_epi8(r0a, zero);
        __m128i r0b_lo = _mm_unpacklo_epi8(r0b, zero), r0b_hi = _mm_unpackhi_epi8(r0b, zero);
        __m128i r1a_lo = _mm_unpacklo_epi8(r1a, zero), r1a_hi = _mm_unpackhi_epi8(r1a, zero);
        __m128i r1b_lo = _mm_unpacklo_epi8(r1b, zero), r1b_hi = _mm_unpackhi_epi8(r1b, zero);

        __m128i s0_01 = _mm_add_epi16(r0a_lo, _mm_srli_si128(r0a_lo, 8));
        __m128i s0_23 = _mm_add_epi16(r0a_hi, _mm_srli_si128(r0a_hi, 8));
        __m128i s0_45 = _mm_add_epi16(r0b_lo, _mm_srli_si128(r0b_lo, 8));
        __m128i s0_67 = _mm_add_epi16(r0b_hi, _mm_srli_si128(r0b_hi, 8));
        __m128i s1_01 = _mm_add_epi16(r1a_lo, _mm_srli_si128(r1a_lo, 8));
        __m128i s1_23 = _mm_add_epi16(r1a_hi, _mm_srli_si128(r1a_hi, 8));
        __m128i s1_45 = _mm_add_epi16(r1b_lo, _mm_srli_si128(r1b_lo, 8));
        __m128i s1_67 = _mm_add_epi16(r1b_hi, _mm_srli_si128(r1b_hi, 8));

        __m128i t01 = _mm_add_epi16(s0_01, s1_01);
        __m128i t23 = _mm_add_epi16(s0_23, s1_23);
        __m128i t45 = _mm_add_epi16(s0_45, s1_45);
        __m128i t67 = _mm_add_epi16(s0_67, s1_67);

        t01 = _mm_srli_epi16(t01, 2);
        t23 = _mm_srli_epi16(t23, 2);
        t45 = _mm_srli_epi16(t45, 2);
        t67 = _mm_srli_epi16(t67, 2);

        __m128i c_01_23 = _mm_unpacklo_epi64(t01, t23);
        __m128i c_45_67 = _mm_unpacklo_epi64(t45, t67);

        __m128i cb_pa = _mm_madd_epi16(c_01_23, cb_weights);
        __m128i cb_pb = _mm_madd_epi16(c_45_67, cb_weights);
        __m128i cb_ha = _mm_hadd_epi32(cb_pa, cb_pa);
        __m128i cb_hb = _mm_hadd_epi32(cb_pb, cb_pb);
        __m128i cb32 = _mm_unpacklo_epi64(cb_ha, cb_hb);
        cb32 = _mm_add_epi32(_mm_srai_epi32(cb32, 8), bias128);

        __m128i cr_pa = _mm_madd_epi16(c_01_23, cr_weights);
        __m128i cr_pb = _mm_madd_epi16(c_45_67, cr_weights);
        __m128i cr_ha = _mm_hadd_epi32(cr_pa, cr_pa);
        __m128i cr_hb = _mm_hadd_epi32(cr_pb, cr_pb);
        __m128i cr32 = _mm_unpacklo_epi64(cr_ha, cr_hb);
        cr32 = _mm_add_epi32(_mm_srai_epi32(cr32, 8), bias128);

        __m128i cb16 = _mm_packs_epi32(cb32, cb32);
        __m128i cr16 = _mm_packs_epi32(cr32, cr32);
        __m128i cb8 = _mm_packus_epi16(cb16, cb16);
        __m128i cr8 = _mm_packus_epi16(cr16, cr16);

        __m128i interleaved = _mm_unpacklo_epi8(cb8, cr8);
        _mm_storel_epi64((__m128i *)(uv_row + x), interleaved);
    }
    /* Remainder -- never actually taken given width is always a
     * multiple of 16 (macroblock-aligned), kept only for safety if
     * this is ever reused against a non-macroblock-aligned width. */
    for (; x < width; x += 2) {
        int have_x1 = (x + 1 < width);
        const unsigned char *p00 = row0 + (size_t)x * 4;
        const unsigned char *p01 = have_x1 ? row0 + (size_t)(x + 1) * 4 : p00;
        const unsigned char *p10 = row1 + (size_t)x * 4;
        const unsigned char *p11 = have_x1 ? row1 + (size_t)(x + 1) * 4 : p10;
        int r = (p00[2] + p01[2] + p10[2] + p11[2]) / 4;
        int g = (p00[1] + p01[1] + p10[1] + p11[1]) / 4;
        int b = (p00[0] + p01[0] + p10[0] + p11[0]) / 4;
        uv_row[x] = clamp_u8_be(128 + ((-43 * r - 85 * g + 128 * b) >> 8));
        if (have_x1)
            uv_row[x + 1] = clamp_u8_be(128 + ((128 * r - 107 * g - 21 * b) >> 8));
    }
}
#endif

/* Same fused RGB-extraction + RGB->NV12 conversion as fill_nv12_from_ximage
 * (see that function's comment), but bounded to rows [y_start, y_end) --
 * y_start/y_end must both be even (chroma is subsampled 2x vertically,
 * so a row range has to fall on a chroma-pair boundary) so that
 * fill_nv12_from_ximage can split the image across threads without any
 * two threads touching the same output bytes: each row range's Y-plane
 * output (y_out + y_start*width .. y_end*width) and UV-plane output
 * (uv_out + (y_start/2)*width .. (y_end/2)*width) are disjoint from
 * every other range's. */
static void fill_nv12_from_ximage_range(XImage *img, int width, int y_start, int y_end,
                                         uint8_t *y_out, uint8_t *uv_out)
{
    if (img->bits_per_pixel == 32 && img->byte_order == LSBFirst &&
        img->red_mask == 0xff0000UL && img->green_mask == 0xff00UL && img->blue_mask == 0xffUL) {
        /* Genuinely one pass: processes two source rows per iteration,
         * computing Y for every pixel in both rows and accumulating
         * each 2x2 block's Cb/Cr as it goes, so every row's bytes are
         * only read once. An earlier version of this function computed
         * Y and UV in two separate loops, each scanning the whole
         * image -- confirmed by real profiling to be no faster than
         * the two-pass RGB-buffer version it was meant to replace,
         * since it was still two full passes over the pixel data, just
         * without the intermediate buffer. */
#if defined(__SSSE3__)
        fill_y_plane_simd((const unsigned char *)img->data + (size_t)y_start * img->bytes_per_line,
                           img->bytes_per_line, width, y_end - y_start, y_out + (size_t)y_start * width);
#endif
        for (int y = y_start; y < y_end; y += 2) {
            const unsigned char *row0 = (const unsigned char *)img->data + (size_t)y * img->bytes_per_line;
            int have_row1 = (y + 1 < y_end);
            const unsigned char *row1 = have_row1
                ? (const unsigned char *)img->data + (size_t)(y + 1) * img->bytes_per_line
                : row0;
#if !defined(__SSSE3__)
            uint8_t *y_row0 = y_out + (size_t)y * width;
            uint8_t *y_row1 = y_out + (size_t)(y + 1) * width;
#endif
            uint8_t *uv_row = uv_out + (size_t)(y / 2) * width;

#if defined(__SSSE3__)
            if (have_row1) {
                fill_uv_row_simd(row0, row1, width, uv_row);
                continue;
            }
#endif

            for (int x = 0; x < width; x += 2) {
                int have_x1 = (x + 1 < width);
                const unsigned char *p00 = row0 + (size_t)x * 4;
                const unsigned char *p01 = have_x1 ? row0 + (size_t)(x + 1) * 4 : p00;
                const unsigned char *p10 = row1 + (size_t)x * 4;
                const unsigned char *p11 = have_x1 ? row1 + (size_t)(x + 1) * 4 : p10;

#if !defined(__SSSE3__)
                y_row0[x] = clamp_u8_be((77 * p00[2] + 150 * p00[1] + 29 * p00[0]) >> 8);
                if (have_x1)
                    y_row0[x + 1] = clamp_u8_be((77 * p01[2] + 150 * p01[1] + 29 * p01[0]) >> 8);
                if (have_row1) {
                    y_row1[x] = clamp_u8_be((77 * p10[2] + 150 * p10[1] + 29 * p10[0]) >> 8);
                    if (have_x1)
                        y_row1[x + 1] = clamp_u8_be((77 * p11[2] + 150 * p11[1] + 29 * p11[0]) >> 8);
                }
#endif

                int r = (p00[2] + p01[2] + p10[2] + p11[2]) / 4;
                int g = (p00[1] + p01[1] + p10[1] + p11[1]) / 4;
                int b = (p00[0] + p01[0] + p10[0] + p11[0]) / 4;
                uv_row[x] = clamp_u8_be(128 + ((-43 * r - 85 * g + 128 * b) >> 8));
                if (have_x1)
                    uv_row[x + 1] = clamp_u8_be(128 + ((128 * r - 107 * g - 21 * b) >> 8));
            }
        }
        return;
    }

    /* Slow, general fallback -- extracts via XGetPixel/channel_from_mask
     * per pixel, same as fill_rgb_from_ximage's fallback, converting to
     * NV12 in the same pass rather than through an RGB buffer. */
    for (int y = y_start; y < y_end; y++) {
        for (int x = 0; x < width; x++) {
            unsigned long pixel = XGetPixel(img, x, y);
            int r = (int)channel_from_mask(pixel, img->red_mask);
            int g = (int)channel_from_mask(pixel, img->green_mask);
            int b = (int)channel_from_mask(pixel, img->blue_mask);
            y_out[(size_t)y * width + x] = clamp_u8_be((77 * r + 150 * g + 29 * b) >> 8);
        }
    }
    for (int y = y_start; y < y_end; y += 2) {
        for (int x = 0; x < width; x += 2) {
            int sum_r = 0, sum_g = 0, sum_b = 0, n = 0;
            for (int dy = 0; dy < 2 && y + dy < y_end; dy++) {
                for (int dx = 0; dx < 2 && x + dx < width; dx++) {
                    unsigned long pixel = XGetPixel(img, x + dx, y + dy);
                    sum_r += (int)channel_from_mask(pixel, img->red_mask);
                    sum_g += (int)channel_from_mask(pixel, img->green_mask);
                    sum_b += (int)channel_from_mask(pixel, img->blue_mask);
                    n++;
                }
            }
            int cb = 128 + ((-43 * (sum_r / n) - 85 * (sum_g / n) + 128 * (sum_b / n)) >> 8);
            int cr = 128 + ((128 * (sum_r / n) - 107 * (sum_g / n) - 21 * (sum_b / n)) >> 8);
            uint8_t *dst = uv_out + (size_t)(y / 2) * width + (x / 2) * 2;
            dst[0] = clamp_u8_be(cb);
            dst[1] = clamp_u8_be(cr);
        }
    }
}

struct fill_nv12_thread_args {
    XImage *img;
    int width, y_start, y_end;
    uint8_t *y_out, *uv_out;
};

static void *fill_nv12_thread_entry(void *arg)
{
    struct fill_nv12_thread_args *a = arg;
    fill_nv12_from_ximage_range(a->img, a->width, a->y_start, a->y_end, a->y_out, a->uv_out);
    return NULL;
}

static int g_fill_nv12_ncpus = 1;

static void fill_nv12_detect_ncpus(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    g_fill_nv12_ncpus = (n >= 1) ? (int)n : 1;
}

/* Splits the conversion across this machine's logical CPUs (queried once
 * and cached -- it can't change at runtime) via fill_nv12_from_ximage_range,
 * each on its own row range. Real, measured win on the real netbook: this
 * is pure CPU-bound work (no X11/XvMC calls at all, unlike the rest of
 * this driver, which needs XVMC_SERIALIZE's single-connection
 * serialization) with disjoint per-thread output, so it's safe to
 * parallelize without touching that locking at all. Below a fixed row
 * threshold, or on a single-CPU machine, just runs single-threaded --
 * thread creation itself isn't free, and a small image's conversion
 * finishes before that overhead would pay for itself. */
static void fill_nv12_from_ximage(XImage *img, int width, int height,
                                   uint8_t *y_out, uint8_t *uv_out)
{
    static pthread_once_t ncpus_once = PTHREAD_ONCE_INIT;
    pthread_once(&ncpus_once, fill_nv12_detect_ncpus);

#define FILL_NV12_MIN_ROWS_PER_THREAD 128
    int nthreads = g_fill_nv12_ncpus;
    if (nthreads > 4) nthreads = 4; /* diminishing returns past this on any real target hardware */
    if (nthreads < 1) nthreads = 1;
    if (height / nthreads < FILL_NV12_MIN_ROWS_PER_THREAD)
        nthreads = height / FILL_NV12_MIN_ROWS_PER_THREAD;
    if (nthreads < 1) nthreads = 1;

    if (nthreads == 1) {
        fill_nv12_from_ximage_range(img, width, 0, height, y_out, uv_out);
        return;
    }

    /* Row ranges must start/end on even rows (see
     * fill_nv12_from_ximage_range's comment) -- round the per-thread
     * row count up to the next even number so every split point is
     * even; the last range just takes whatever remains (never zero:
     * the height/nthreads clamp above guarantees at least
     * FILL_NV12_MIN_ROWS_PER_THREAD rows per range, so this loop always
     * produces exactly `nthreads` ranges, never fewer). First build the
     * full list of ranges, THEN spawn/run them -- computing "is this
     * the last range" from the finished list rather than from the loop
     * index avoids a range ever being both spawned as a thread AND
     * re-run in the calling thread. */
    int rows_per_thread = ((height / nthreads) + 1) & ~1;
    struct fill_nv12_thread_args args[4];
    int n = 0, y = 0;
    for (int i = 0; i < nthreads; i++) {
        int y_end = (i == nthreads - 1) ? height : y + rows_per_thread;
        if (y_end > height) y_end = height;
        args[n++] = (struct fill_nv12_thread_args){ img, width, y, y_end, y_out, uv_out };
        y = y_end;
    }

    pthread_t threads[3]; /* n-1 spawned; this thread does the last range itself */
    for (int i = 0; i < n - 1; i++)
        pthread_create(&threads[i], NULL, fill_nv12_thread_entry, &args[i]);
    /* This thread does the last range itself instead of spawning one
     * more -- no reason to pay for an extra thread when this one is
     * sitting idle waiting anyway. */
    fill_nv12_thread_entry(&args[n - 1]);
    for (int i = 0; i < n - 1; i++)
        pthread_join(threads[i], NULL);
}

int xvmc_backend_get_surface_nv12(struct xvmc_backend *be, int surface_index,
                                   uint8_t *y_out, uint8_t *uv_out,
                                   int dst_width, int dst_height)
{
    int prof = getenv("XVMC_PROFILE") != NULL;
    double t0 = 0, t1 = 0, t2 = 0, t3 = 0, t4 = 0;
    int using_shm = 0;

    XImage *img = acquire_surface_image(be, surface_index, dst_width, dst_height,
                                         &using_shm, &t0, &t1, &t2);
    if (!img)
        return -1;
    if (prof) t3 = prof_now_ms();

    fill_nv12_from_ximage(img, dst_width, dst_height, y_out, uv_out);
    if (!using_shm)
        XDestroyImage(img);
    if (prof) {
        t4 = prof_now_ms();
        fprintf(stderr, "[prof] get_surface_nv12: putsurface+sync=%.1fms wait_display=%.1fms "
                        "%s=%.1fms convert=%.1fms total=%.1fms\n",
                t1 - t0, t2 - t1, using_shm ? "xshmgetimage" : "xgetimage", t3 - t2, t4 - t3, t4 - t0);
    }
    return 0;
}

int xvmc_backend_display_only(struct xvmc_backend *be, int surface_index)
{
    if (surface_index < 0 || surface_index >= XVMC_BACKEND_MAX_SURFACES ||
        !be->surface_in_use[surface_index]) {
        fprintf(stderr, "xvmc_backend_display_only: invalid surface %d\n", surface_index);
        return -1;
    }
    if (ensure_scratch_window(be, be->width, be->height) != 0)
        return -1;

    Status st = XvMCPutSurface(be->dpy, &be->surfaces[surface_index], be->scratch_win,
                                0, 0, (unsigned short)be->width, (unsigned short)be->height,
                                0, 0, (unsigned short)be->width, (unsigned short)be->height,
                                XVMC_FRAME_PICTURE);
    if (st != Success) {
        fprintf(stderr, "xvmc_backend_display_only: XvMCPutSurface failed (%d)\n", (int)st);
        return -1;
    }
    XSync(be->dpy, False);
    return 0;
}
