#ifndef XVMC_BACKEND_H
#define XVMC_BACKEND_H

#include <stdint.h>

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/XvMClib.h>

/* Fixed capacity is fine here: real GMA950 XvMC only ever supports a
 * handful of concurrent surfaces (driver-dependent, historically ~8), so
 * this comfortably covers any real usage without needing a dynamic table. */
#define XVMC_BACKEND_MAX_SURFACES 32

/* Real hardware ceiling, found by bisection on real i915/945 XvMC:
 * 720x576 succeeds, 720x608 and 736x480 both fail (so it's independent
 * width/height caps, not a total-macroblock-count or total-area limit)
 * -- exactly classic PAL broadcast SD, this hardware generation's
 * overlay/motion-comp engine's real native limit. Above this,
 * XvMCCreateContext/XvMCCreateSurface don't return a VA-API-visible
 * failure at all -- they raise a real X BadValue *protocol* error,
 * which Xlib's default handler treats as fatal (exit(), killing the
 * whole calling process, not just this call) unless the ceiling is
 * checked first and the X call is never made. */
#define XVMC_BACKEND_MAX_WIDTH 720
#define XVMC_BACKEND_MAX_HEIGHT 576

struct xvmc_backend {
    Display *dpy;
    int screen;

    XvPortID port;
    int surface_type_id;
    int mc_type; /* codec | accel bits from XvMCSurfaceInfo, kept for logging */
    int surface_flags; /* XvMCSurfaceInfo.flags -- XVMC_INTRA_UNSIGNED etc. */

    int context_created;
    int width;
    int height;
    XvMCContext context;
    unsigned int max_macroblocks;
    XvMCMacroBlockArray macroblocks;
    XvMCBlockArray blocks;

    XvMCSurface surfaces[XVMC_BACKEND_MAX_SURFACES];
    int surface_in_use[XVMC_BACKEND_MAX_SURFACES];

    /* Lazily created on first vaGetImage-style readback and kept for the
     * backend's lifetime (see xvmc_backend_get_surface_rgb) -- avoids
     * paying XCreateWindow/XDestroyWindow overhead on every single
     * decoded frame, since real callers (ffmpeg's frame-transfer path)
     * may call this once per frame. Sized to the largest readback ever
     * requested so far (see xvmc_backend_get_surface_nv12's dst_width/
     * dst_height) -- grown via XResizeWindow if a later call ever needs
     * more, never shrunk. Real callers request one stable size for the
     * whole session (from vaCreateImage, itself called once), so this
     * only grows at most once in practice: from be->width/height (if
     * xvmc_backend_get_surface_rgb's debug snapshot path runs first) up
     * to whatever a real vaGetImage caller's declared image size is. */
    Window scratch_win;
    int scratch_win_created;
    int scratch_win_width;
    int scratch_win_height;

    /* Tracks which caller-owned window (xvmc_backend_put_surface's
     * `draw`) has already been sent an EWMH _NET_WM_STATE_FULLSCREEN
     * request, so it's only sent once per window instead of every
     * frame (the WM's decoration-removal transition only needs to
     * happen once; XMoveResizeWindow still runs every call to keep
     * reclaiming the size/position). None (0) means not yet sent. */
    Window fullscreen_win;

    /* MIT-SHM image for xvmc_backend_get_surface_rgb's readback --
     * XShmGetImage transfers pixel data through a shared memory segment
     * instead of the core X protocol, avoiding a real, measured
     * bottleneck (confirmed by real profiling: plain XGetImage of a
     * 640x480 frame took 25-45ms, dominating vaGetImage's total cost).
     * Created lazily and reused for the backend's lifetime as long as a
     * later request doesn't need it bigger (same "grows at most once in
     * practice" reasoning as scratch_win_width/height above -- shm_width/
     * height track the size it's currently allocated for; a bigger
     * request tears down and recreates it). shm_available is set false
     * (and this whole path skipped in favor of the plain XGetImage
     * fallback) if MIT-SHM setup ever fails -- confirmed present on this
     * real X server, but not guaranteed on every one this driver might
     * run against. */
    XShmSegmentInfo shm_info;
    XImage *shm_image;
    int shm_attempted;
    int shm_available;
    int shm_width;
    int shm_height;
};

/*
 * Scans Xv adaptors on `dpy`/`screen` for an MPEG-2, 4:2:0-capable XvMC
 * port, preferring one with hardware IDCT+MC acceleration and falling back
 * to motion-comp-only if that's all this X server advertises. Returns 0 on
 * success, -1 if no such port exists (e.g. no XvMC-capable hardware/driver
 * behind this X server at all).
 */
int xvmc_backend_open(struct xvmc_backend *be, Display *dpy, int screen);

/* Returned by xvmc_backend_create_context instead of -1 when width/height
 * exceed XVMC_BACKEND_MAX_WIDTH/HEIGHT -- lets the driver layer map this
 * specific case to VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED rather than
 * the generic VA_STATUS_ERROR_ALLOCATION_FAILED used for other failures. */
#define XVMC_BACKEND_RESOLUTION_UNSUPPORTED (-2)

/*
 * Lazily creates the single XvMCContext this driver supports, sized for
 * width/height, plus its macroblock/block scratch arrays (sized for the
 * worst case: every macroblock intra, all 6 4:2:0 blocks present). A
 * no-op if already created. Must happen before the first
 * xvmc_backend_create_surface() call -- XvMCCreateSurface takes a
 * context, unlike VA-API's own vaCreateSurfaces/vaCreateContext ordering.
 *
 * Checks width/height against the real hardware ceiling (see
 * XVMC_BACKEND_MAX_WIDTH/HEIGHT above) before making any X call, and
 * returns XVMC_BACKEND_RESOLUTION_UNSUPPORTED without touching the X
 * connection at all if they're exceeded -- calling XvMCCreateContext
 * past the real ceiling doesn't fail gracefully, it raises a fatal X
 * protocol error that kills the whole process, so this check is the
 * only thing standing between a real caller (ffmpeg, mpv, ...)
 * requesting a large resolution and it silently dying instead of
 * getting an ordinary VAStatus error back. Returns 0 on success, -1 on
 * any other real failure (XvMCCreateContext/MacroBlocks/Blocks itself
 * failing).
 */
int xvmc_backend_create_context(struct xvmc_backend *be, int width, int height);
void xvmc_backend_destroy_context(struct xvmc_backend *be);

/* Returns a surface index (usable directly as a VASurfaceID) or -1. */
int xvmc_backend_create_surface(struct xvmc_backend *be);
void xvmc_backend_destroy_surface(struct xvmc_backend *be, int surface_index);

/*
 * Debug tooling only: blits a decoded surface onto a real window via
 * XvMCPutSurface and reads the result back with XGetImage, writing it
 * out as a binary PPM (P6) file. Lets real decoded output from the
 * relay path be visually inspected/compared against a reference decode.
 * Returns 0 on success, -1 on failure (does not fail the caller's real
 * work -- intended to be best-effort, gated by an env var).
 */
int xvmc_backend_snapshot_surface(struct xvmc_backend *be, int surface_index, const char *path);

/*
 * Real vaGetImage backing: blits a decoded surface via XvMCPutSurface
 * onto a persistent scratch window (created lazily, reused across
 * calls) and reads it back with XGetImage into a caller-provided RGB24
 * buffer (be->width * be->height * 3 bytes, no cropping -- this driver
 * always transfers the whole frame, matching every real caller seen so
 * far). XvMC has no direct "read the decoded surface" API of its own;
 * this PutSurface+GetImage round trip is the same technique already
 * verified pixel-accurate by xvmc_backend_snapshot_surface, just
 * without writing a file. The caller (xvmc_GetImage) converts this RGB
 * data to whatever VAImageFormat the client actually asked for.
 * Returns 0 on success, -1 on failure.
 */
int xvmc_backend_get_surface_rgb(struct xvmc_backend *be, int surface_index, unsigned char *rgb_out);

/*
 * Same real capture path as xvmc_backend_get_surface_rgb, but converts
 * directly to NV12 (one full-resolution Y plane, one half-resolution
 * interleaved-UV plane, both caller-allocated, sized for dst_width x
 * dst_height) in a single pass over the raw XImage bytes -- avoids a
 * real, measured cost confirmed by profiling: extracting to an
 * intermediate RGB24 buffer and then converting that to NV12 as two
 * separate full-image passes. This is what xvmc_GetImage actually uses;
 * xvmc_backend_get_surface_rgb remains for the snapshot debug tool,
 * which genuinely wants RGB at the native decode size for its PPM
 * output.
 *
 * dst_width/dst_height need not match be->width/be->height (the actual
 * local decode resolution) -- when they differ, the source surface is
 * scaled to dst_width x dst_height by real XvMC hardware as part of the
 * same XvMCPutSurface blit already used to get pixels onto the scratch
 * window at all (see acquire_surface_image), so this is a genuine
 * hardware-scaled upscale/downscale, not a CPU-side resize -- it costs
 * nothing beyond what the unscaled path already paid. This is what lets
 * a caller whose declared VAImage size differs from this driver's
 * actual local (e.g. relay-resolution-driven) decode size still get a
 * correctly-sized image back instead of VA_STATUS_ERROR_INVALID_PARAMETER.
 * Returns 0 on success, -1 on failure.
 */
int xvmc_backend_get_surface_nv12(struct xvmc_backend *be, int surface_index,
                                   uint8_t *y_out, uint8_t *uv_out,
                                   int dst_width, int dst_height);

/*
 * Debug tooling: blits a decoded surface onto the persistent scratch
 * window (same window xvmc_backend_get_surface_nv12 uses) via a real
 * XvMCPutSurface -- genuine hardware overlay display, zero-copy -- but
 * never reads any pixels back at all. Used to let real video display
 * correctly (via this real hardware blit) while a caller's own
 * vaGetImage skips its own expensive readback+conversion entirely,
 * confirming/exploiting that this driver's own transfer code is cheap
 * (the real cost lives in the CPU-side readback, not the display blit
 * itself). Returns 0 on success, -1 on failure.
 */
int xvmc_backend_display_only(struct xvmc_backend *be, int surface_index);

/*
 * Real display path for a real caller's own window: blits a decoded
 * surface onto `draw` via XvMCPutSurface. Unlike xvmc_backend_snapshot_surface,
 * this never creates its own window or reads pixels back -- the caller
 * (a real media player) already owns and manages `draw`.
 *
 * The caller's own destx/desty/destw/desth are ignored entirely.
 * Instead, `draw` itself is force-resized/moved to cover the whole
 * screen (XMoveResizeWindow -- allowed unconditionally since it's the
 * window's own owning connection making the request, regardless of
 * what the WM would otherwise permit) every call, and the source frame
 * is letterboxed/pillarboxed within it to preserve aspect ratio. The
 * first time a given `draw` is seen, an EWMH _NET_WM_STATE_FULLSCREEN
 * ClientMessage is also sent to the root window asking the WM to drop
 * `draw`'s decorations (title bar/border) and treat it as a genuine
 * fullscreen window -- honored by effectively every EWMH-compliant WM,
 * unlike raw XMoveResizeWindow alone, which a reparenting WM will
 * still wrap in its own decorated frame. The caller never observes any
 * of this -- it keeps issuing vaPutSurface calls exactly as it always
 * did, unaware its own window is now screen-sized, undecorated, and
 * always-fullscreen. Returns 0 on success, -1 on failure.
 */
int xvmc_backend_put_surface(struct xvmc_backend *be, int surface_index, Drawable draw,
                              short srcx, short srcy, unsigned short srcw, unsigned short srch,
                              short destx, short desty, unsigned short destw, unsigned short desth);

#endif
