/*
 * libva backend driver for GMA950/945GSE, loaded by libva as
 * xvmc_drv_video.so when an app sets LIBVA_DRIVER_NAME=xvmc.
 *
 * Two decode paths, both ending in the same local XvMC pipeline
 * (mpeg2_vld_decode_slice + render_slice_macroblocks/XvMCRenderSurface),
 * verified working on the real GMA950 netbook (I- and P-frames both
 * correct against real per-macroblock ground truth):
 *
 *   - MPEG-2 profiles (VAProfileMPEG2Simple/Main): real client software
 *     (ffmpeg/mpv) hands this driver parsed VAPictureParameterBufferMPEG2/
 *     VAIQMatrixBufferMPEG2/VASliceParameterBufferMPEG2 buffers directly
 *     via vaRenderPicture -- see xvmc_RenderPicture's VASliceDataBufferType
 *     case.
 *   - H.264 profiles (VAProfileH264*): this driver has no local H.264
 *     decode of any kind. Raw H.264 slice bytes are accumulated per
 *     picture (xvmc_RenderPicture, when context_profile is H.264) and
 *     forwarded over relay_client.[ch] to relay-server's push mode, which
 *     transcodes to a raw MPEG-2 elementary stream and streams it back on
 *     the same connection. mpeg2_headers.[ch] parses the MPEG-2
 *     sequence/picture/slice headers out of what comes back (relay-server
 *     doesn't send VA-API buffers, just raw bytes -- something has to
 *     parse them client-side), producing exactly the structures
 *     mpeg2_vld_decode_slice already expects, so the rest of the pipeline
 *     is unchanged from the local MPEG-2 path. See xvmc_relay_end_picture.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_version.h>

#include "h264_reconstitute.h"
#include "mpeg2_headers.h"
#include "mpeg2_vld.h"
#include "relay_client.h"
#include "xvmc_backend.h"

/*
 * This driver keeps all its real state in one struct reached through
 * ctx->pDriverData, does raw Xlib/XvMC calls (XvMCPutSurface, XGetImage,
 * etc, none of it thread-safe on their own), and even calls some of its
 * own vtable-registered functions directly as plain C calls internally
 * (xvmc_CreateImage -> xvmc_CreateBuffer, xvmc_DestroyImage ->
 * xvmc_DestroyBuffer). None of that was ever going to be safe if called
 * from more than one thread at once -- confirmed by real testing: a
 * real ffmpeg run (multi-threaded H.264 decode by default) reliably
 * segfaulted a few frames into real vaGetImage-based playback, and
 * disappeared completely under `-threads 1`. Real VA-API drivers are
 * expected to tolerate being called from a caller's worker threads;
 * this driver was never designed with that in mind and has no reason
 * to be internally concurrent anyway (a single old netbook's XvMC
 * hardware is the real bottleneck either way), so the correct fix is
 * coarse-grained: serialize every entry point with one recursive lock
 * (recursive because of the direct internal calls above -- a plain
 * mutex would deadlock on self-reentry) rather than trying to reason
 * about which specific pairs of calls can actually race.
 */
static pthread_mutex_t g_xvmc_driver_lock;
static pthread_once_t g_xvmc_driver_lock_once = PTHREAD_ONCE_INIT;

static void xvmc_driver_lock_init(void)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_xvmc_driver_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void xvmc_driver_unlock_cleanup(int *guard)
{
    (void)guard;
    pthread_mutex_unlock(&g_xvmc_driver_lock);
}

/* Placed as the first statement of every vtable-registered function:
 * locks on entry, and unlocks automatically on every exit path (any
 * return, or falling off the end) via the cleanup attribute, so no
 * individual return statement needs to remember to unlock. */
#define XVMC_SERIALIZE() \
    pthread_once(&g_xvmc_driver_lock_once, xvmc_driver_lock_init); \
    pthread_mutex_lock(&g_xvmc_driver_lock); \
    int xvmc_lock_guard_ __attribute__((cleanup(xvmc_driver_unlock_cleanup))); \
    (void)xvmc_lock_guard_

/*
 * libva's driver loader dlsym()s a version-specific entry point named
 * __vaDriverInit_<major>_<minor> (confirmed against Mesa's installed
 * radeonsi/nouveau _drv_video.so on the WSL build machine -- there is no
 * VA_DRIVER_INIT_FUNC macro provided by <va/va_backend.h> to do this for
 * us, despite some older docs/drivers assuming otherwise). Building this
 * token-pasted name from VA_MAJOR_VERSION/VA_MINOR_VERSION keeps it correct
 * for whatever libva is installed on the build machine.
 */
#define VA_DRIVER_INIT_NAME_(major, minor) __vaDriverInit_##major##_##minor
#define VA_DRIVER_INIT_NAME(major, minor) VA_DRIVER_INIT_NAME_(major, minor)
#define VA_DRIVER_INIT_ENTRY VA_DRIVER_INIT_NAME(VA_MAJOR_VERSION, VA_MINOR_VERSION)

struct xvmc_buffer {
    VABufferType type;
    unsigned int size;
    unsigned int num_elements;
    void *data;
    int in_use;
};

/* Only a handful of configs are ever realistically live at once (real
 * clients create one, maybe two across a session) -- a small fixed array
 * avoids needing a grow/realloc path like alloc_buffer_slot's. */
#define XVMC_MAX_CONFIGS 4

struct xvmc_config {
    int in_use;
    VAProfile profile;
};

/* Real callers (ffmpeg's vaapi hwaccel frame transfer) only ever need
 * one or two images alive at once (one being read via vaGetImage while
 * a previous one is being unmapped/destroyed) -- a small fixed array
 * avoids needing alloc_buffer_slot's grow/realloc path. */
#define XVMC_MAX_IMAGES 8

struct xvmc_image {
    int in_use;
    VAImage image;
};

struct xvmc_driver_data {
    struct xvmc_backend backend;

    struct xvmc_buffer *buffers;
    unsigned int buffers_capacity;

    struct xvmc_config configs[XVMC_MAX_CONFIGS];
    struct xvmc_image images[XVMC_MAX_IMAGES];
    /* Only one XvMCContext is ever created (xvmc_CreateContext always
     * returns context=0), so a single field is enough -- no per-context
     * table needed. Set from the config used to create it. */
    VAProfile context_profile;
    /* Set by the most recent xvmc_CreateConfig call. Real callers create
     * exactly one config per session (matches this driver's own
     * single-context model everywhere else), and always call it before
     * vaCreateSurfaces -- which, unlike vaCreateContext, has no config_id
     * parameter to learn the profile from directly. xvmc_CreateSurfaces
     * needs to know the profile *before* context_profile is set (that
     * only happens in xvmc_CreateContext, which real callers invoke
     * after vaCreateSurfaces) to decide whether the caller's own
     * width/height can be trusted -- see relay_resolution_width/height
     * below. */
    VAProfile last_config_profile;

    /* For an H.264 profile, the local XvMC backend context must be sized
     * for whatever resolution relay-server will actually transcode down
     * to (RELAY_RESOLUTION on the server side) -- NOT the app's native
     * H.264 picture_width/picture_height, which vaCreateSurfaces/
     * vaCreateContext otherwise receive and which can legitimately be
     * much larger. Mirrored here via XVMC_RELAY_RESOLUTION (same "WxH"
     * format, same default as relay-server's own) since the driver has
     * no way to ask relay-server what it's configured for -- the two
     * must be kept in sync by whoever deploys them, same as
     * XVMC_RELAY_HOST/PORT already have to match relay-server's listen
     * address. Parsed once in vaInitialize. */
    int relay_resolution_width;
    int relay_resolution_height;

    /* Per-picture state, reset in vaBeginPicture. */
    int render_target;
    unsigned int mb_count;
    /* Debug tooling only: bumped once per successfully decoded picture
     * when XVMC_SNAPSHOT_DIR is set, so each frame gets a distinct
     * filename -- see maybe_snapshot_surface. */
    unsigned int snapshot_frame_count;
    VAPictureParameterBufferMPEG2 pic_params;
    int has_pic_params;
    VAIQMatrixBufferMPEG2 iq_matrix;
    int has_iq_matrix;

    /* A VASliceParameterBufferType buffer is immediately followed by the
     * VASliceDataBufferType it describes (standard VA-API convention) --
     * stash it here until that slice-data buffer shows up. */
    VASliceParameterBufferMPEG2 *pending_slices;
    unsigned int num_pending_slices;

    /* H.264 relay path (see the file-level comment above). */
    char *relay_host;
    unsigned short relay_port;
    struct relay_conn relay;
    int relay_connected;

    /* When XVMC_RELAY_DUMP names a path, every byte this driver sends
     * to relay-server (SPS, PPS, each frame, each AUD) is also written
     * here, byte for byte -- lets the exact reconstituted Annex-B stream
     * be inspected offline (ffprobe/ffmpeg) without needing the network
     * round trip or relay-server at all. Debug tooling only. */
    FILE *relay_dump_fp;
    /* Same idea, mirrored for the other direction: when XVMC_RELAY_RECV_DUMP
     * names a path, every raw byte relay_recv() returns (the MPEG-2
     * elementary stream relay-server sends back) is written here, byte
     * for byte, before this driver's own mpeg2_headers.c/mpeg2_vld.c
     * ever touch it -- lets a real client-side parse/decode failure be
     * reproduced and inspected offline instead of only live. */
    FILE *relay_recv_dump_fp;

    /* VAPictureParameterBufferH264, captured so h264_synthesize_sps/pps
     * (see h264_reconstitute.[ch]) can build the SPS/PPS NAL units real
     * VA-API clients never send as raw bytes -- relay-server's standalone
     * ffmpeg needs them to make sense of the slice NALs at all. */
    VAPictureParameterBufferH264 h264_pic_params;
    int has_h264_pic_params;
    /* Sent once per relay connection (see xvmc_relay_end_picture), not
     * once per picture -- reset alongside relay_connected. */
    int h264_sps_pps_sent;

    /* Accumulates one picture's H.264 slice bytes across possibly
     * multiple xvmc_RenderPicture calls, reset in xvmc_BeginPicture. */
    uint8_t *h264_frame_buf;
    size_t h264_frame_len;
    size_t h264_frame_cap;

    /* Bytes received from relay-server that haven't yet formed a
     * complete picture per mpeg2_headers_parse_picture -- persists
     * across xvmc_EndPicture calls (a relay_recv() can return less than
     * one full picture's worth, or more than one). */
    uint8_t *relay_recv_buf;
    size_t relay_recv_len;
    size_t relay_recv_cap;

    /* Sequence-level state (quant matrices, sizes) parsed out of the
     * relay stream persists across pictures within a session, same as
     * real MPEG-2 streams only send a fresh sequence header
     * occasionally, not every picture. */
    struct mpeg2_header_state relay_header_state;

    /* mpeg2_headers.c always reports forward/backward_reference_picture
     * as VA_INVALID_SURFACE (relay-server doesn't hand this driver real
     * VASurfaceIDs for its own internal MPEG-2 reference pictures --
     * there are none to hand, it's not a VA-API client), so P-frames in
     * the relay path never had a real reference surface for motion
     * compensation at all, silently, until real testing with a real
     * media player pulling every frame (not just frame 0, always an
     * I-frame, which is all any snapshot in this project ever visually
     * checked) finally exposed it as macroblock-level garbage on
     * inter-coded content. Tracks this driver's own most recently
     * rendered relay-path surface to use as the real forward reference
     * for the next P-picture -- B-frames are NOT handled (no backward
     * reference tracking), since relay-server's real encodes seen so
     * far never produce them. */
    int relay_last_decoded_surface;

    /* FIFO of VASurfaceIDs, one push per H.264 picture actually sent to
     * relay-server (see xvmc_relay_end_picture), one pop per complete
     * decoded picture drained back (see drain_relay_pictures).
     * Necessary because relay-server's transcode is pipelined/lagged --
     * its own ffmpeg needs several pictures of lookahead before
     * returning anything -- so the decoded picture that comes back
     * during any given drain call does NOT correspond to whatever
     * VASurfaceID happens to be "current" (drv->render_target) at that
     * moment; it corresponds to whichever picture was sent that many
     * pictures ago. Confirmed a real, necessary fix by real testing: a
     * real ffmpeg-driven playback run showed decoded content landing on
     * the wrong surface (a torn mix of colorkey background and garbled
     * macroblock data, worsening over time as the lag grew) without
     * this queue -- this driver's own test harnesses never caught it
     * since they only ever captured whatever was most recently
     * rendered, without checking it matched a specific request. */
#define XVMC_RELAY_SURFACE_QUEUE_CAP 256
    int relay_surface_queue[XVMC_RELAY_SURFACE_QUEUE_CAP];
    unsigned int relay_surface_queue_count;
    unsigned int relay_surface_queue_pos; /* index of the oldest entry */
};

/* Pushed once per picture actually sent to relay-server. Drops the
 * oldest entry on overflow (logging loudly) rather than crashing or
 * blocking -- 256 pictures of relay lookahead is already far beyond
 * anything real testing has seen, so overflow would indicate something
 * seriously wrong upstream, not a real capacity need. */
static void relay_surface_queue_push(struct xvmc_driver_data *drv, int surface)
{
    if (drv->relay_surface_queue_count == XVMC_RELAY_SURFACE_QUEUE_CAP) {
        fprintf(stderr, "relay_surface_queue_push: overflow, dropping oldest entry\n");
        drv->relay_surface_queue_pos =
            (drv->relay_surface_queue_pos + 1) % XVMC_RELAY_SURFACE_QUEUE_CAP;
        drv->relay_surface_queue_count--;
    }
    unsigned int idx = (drv->relay_surface_queue_pos + drv->relay_surface_queue_count) %
                        XVMC_RELAY_SURFACE_QUEUE_CAP;
    drv->relay_surface_queue[idx] = surface;
    drv->relay_surface_queue_count++;
}

/* Pops the surface a decoded picture should render into. Returns -1 if
 * the queue is empty (should only happen if this driver's own
 * push/pop bookkeeping has drifted -- callers fall back to
 * drv->render_target, matching this function's pre-fix behavior, and
 * log loudly since it means the mapping this fix exists for isn't
 * being maintained correctly). */
static int relay_surface_queue_pop(struct xvmc_driver_data *drv)
{
    if (drv->relay_surface_queue_count == 0)
        return -1;
    int surface = drv->relay_surface_queue[drv->relay_surface_queue_pos];
    drv->relay_surface_queue_pos =
        (drv->relay_surface_queue_pos + 1) % XVMC_RELAY_SURFACE_QUEUE_CAP;
    drv->relay_surface_queue_count--;
    return surface;
}

static VABufferID alloc_buffer_slot(struct xvmc_driver_data *drv)
{
    for (unsigned int i = 0; i < drv->buffers_capacity; i++) {
        if (!drv->buffers[i].in_use)
            return i;
    }

    unsigned int new_capacity = drv->buffers_capacity ? drv->buffers_capacity * 2 : 16;
    struct xvmc_buffer *grown = realloc(drv->buffers, new_capacity * sizeof(*grown));
    if (!grown)
        return (VABufferID)-1;
    memset(grown + drv->buffers_capacity, 0, (new_capacity - drv->buffers_capacity) * sizeof(*grown));

    VABufferID id = drv->buffers_capacity;
    drv->buffers = grown;
    drv->buffers_capacity = new_capacity;
    return id;
}

static VAStatus xvmc_Terminate(VADriverContextP ctx)
{
    XVMC_SERIALIZE();
    struct xvmc_driver_data *drv = ctx->pDriverData;
    if (drv) {
        for (unsigned int i = 0; i < drv->buffers_capacity; i++)
            free(drv->buffers[i].data);
        free(drv->buffers);
        xvmc_backend_destroy_context(&drv->backend);
        if (drv->relay_connected)
            relay_close(&drv->relay);
        if (drv->relay_dump_fp)
            fclose(drv->relay_dump_fp);
        if (drv->relay_recv_dump_fp)
            fclose(drv->relay_recv_dump_fp);
        free(drv->relay_host);
        free(drv->h264_frame_buf);
        free(drv->relay_recv_buf);
        free(drv);
    }
    ctx->pDriverData = NULL;
    return VA_STATUS_SUCCESS;
}

/* MPEG-2 is the only thing XvMC can actually decode locally -- H.264
 * profiles are accepted too, but only because the relay path (see
 * xvmc_RenderPicture/xvmc_EndPicture's context_profile branch) forwards
 * the H.264 bitstream to relay-server and feeds the MPEG-2 that comes
 * back through this same local decode path; there is no local H.264
 * decode of any kind. */
static int profile_is_h264(VAProfile profile)
{
    return profile == VAProfileH264ConstrainedBaseline ||
           profile == VAProfileH264Main ||
           profile == VAProfileH264High;
}

static VAStatus xvmc_QueryConfigProfiles(
    VADriverContextP ctx, VAProfile *profile_list, int *num_profiles)
{
    XVMC_SERIALIZE();
    (void)ctx;
    profile_list[0] = VAProfileMPEG2Simple;
    profile_list[1] = VAProfileMPEG2Main;
    profile_list[2] = VAProfileH264ConstrainedBaseline;
    profile_list[3] = VAProfileH264Main;
    profile_list[4] = VAProfileH264High;
    *num_profiles = 5;
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_QueryConfigEntrypoints(
    VADriverContextP ctx, VAProfile profile,
    VAEntrypoint *entrypoint_list, int *num_entrypoints)
{
    XVMC_SERIALIZE();
    (void)ctx;
    (void)profile;
    /* VLD, not IDCT/MoComp: real client software (ffmpeg/mpv) only ever
     * drives VAEntrypointVLD for MPEG-2 -- it hands raw compressed slice
     * bits and expects the driver to fully entropy-decode them. The
     * IDCT/MoComp entrypoints VA-API also defines are vestigial; nothing
     * modern produces the pre-decoded macroblock buffers they expect. */
    entrypoint_list[0] = VAEntrypointVLD;
    *num_entrypoints = 1;
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_GetConfigAttributes(
    VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint,
    VAConfigAttrib *attrib_list, int num_attribs)
{
    XVMC_SERIALIZE();
    (void)ctx;
    (void)profile;
    (void)entrypoint;
    for (int i = 0; i < num_attribs; i++) {
        if (attrib_list[i].type == VAConfigAttribRTFormat)
            attrib_list[i].value = VA_RT_FORMAT_YUV420;
        else
            attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_CreateConfig(
    VADriverContextP ctx, VAProfile profile, VAEntrypoint entrypoint,
    VAConfigAttrib *attrib_list, int num_attribs, VAConfigID *config_id)
{
    XVMC_SERIALIZE();
    (void)entrypoint; /* VLD is the only entrypoint offered for any profile */
    (void)attrib_list;
    (void)num_attribs;
    struct xvmc_driver_data *drv = ctx->pDriverData;

    if (profile != VAProfileMPEG2Simple && profile != VAProfileMPEG2Main &&
        !profile_is_h264(profile))
        return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;

    for (unsigned int i = 0; i < XVMC_MAX_CONFIGS; i++) {
        if (drv->configs[i].in_use)
            continue;
        drv->configs[i].in_use = 1;
        drv->configs[i].profile = profile;
        drv->last_config_profile = profile;
        *config_id = i;
        return VA_STATUS_SUCCESS;
    }
    return VA_STATUS_ERROR_ALLOCATION_FAILED;
}

static VAStatus xvmc_DestroyConfig(VADriverContextP ctx, VAConfigID config_id)
{
    XVMC_SERIALIZE();
    struct xvmc_driver_data *drv = ctx->pDriverData;
    if (config_id >= XVMC_MAX_CONFIGS || !drv->configs[config_id].in_use)
        return VA_STATUS_ERROR_INVALID_CONFIG;
    drv->configs[config_id].in_use = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_CreateSurfaces(
    VADriverContextP ctx, int width, int height, int format,
    int num_surfaces, VASurfaceID *surfaces)
{
    XVMC_SERIALIZE();
    (void)format;
    struct xvmc_driver_data *drv = ctx->pDriverData;

    /* XvMCCreateSurface needs a context, unlike VA-API's own
     * CreateSurfaces-before-CreateContext ordering -- create it here,
     * lazily, the first time we learn width/height.
     *
     * For an H.264 profile, width/height here are the app's *native*
     * H.264 picture size, not what relay-server will actually send
     * back (RELAY_RESOLUTION) -- the local backend context has to be
     * sized for the latter, since that's the real resolution XvMC will
     * decode. See relay_resolution_width/height's comment. */
    int create_width = width, create_height = height;
    if (profile_is_h264(drv->last_config_profile)) {
        create_width = drv->relay_resolution_width;
        create_height = drv->relay_resolution_height;
    }
    int ctx_status = xvmc_backend_create_context(&drv->backend, create_width, create_height);
    if (ctx_status == XVMC_BACKEND_RESOLUTION_UNSUPPORTED)
        return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
    if (ctx_status != 0)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    for (int i = 0; i < num_surfaces; i++) {
        int index = xvmc_backend_create_surface(&drv->backend);
        if (index < 0) {
            for (int j = 0; j < i; j++)
                xvmc_backend_destroy_surface(&drv->backend, (int)surfaces[j]);
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
        surfaces[i] = (VASurfaceID)index;
    }
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_CreateSurfaces2(
    VADriverContextP ctx, unsigned int format, unsigned int width, unsigned int height,
    VASurfaceID *surfaces, unsigned int num_surfaces,
    VASurfaceAttrib *attrib_list, unsigned int num_attribs)
{
    XVMC_SERIALIZE();
    /* libva's public vaCreateSurfaces() calls this vtable entry instead of
     * the plain vaCreateSurfaces above whenever the caller passes any
     * attributes (real clients like ffmpeg's vaapi hwaccel always do, to
     * request a pixel format/memory type) -- this driver has no per-surface
     * attribute-driven behavior to offer (XvMC surfaces are opaque, fixed
     * by xvmc_backend_create_context/create_surface), so this just ignores
     * the attributes and shares the same logic as xvmc_CreateSurfaces. */
    (void)attrib_list;
    (void)num_attribs;
    return xvmc_CreateSurfaces(ctx, (int)width, (int)height, (int)format,
                                (int)num_surfaces, surfaces);
}

static VAStatus xvmc_DestroySurfaces(
    VADriverContextP ctx, VASurfaceID *surface_list, int num_surfaces)
{
    XVMC_SERIALIZE();
    struct xvmc_driver_data *drv = ctx->pDriverData;
    for (int i = 0; i < num_surfaces; i++)
        xvmc_backend_destroy_surface(&drv->backend, (int)surface_list[i]);
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_CreateContext(
    VADriverContextP ctx, VAConfigID config_id, int picture_width,
    int picture_height, int flag, VASurfaceID *render_targets,
    int num_render_targets, VAContextID *context)
{
    XVMC_SERIALIZE();
    (void)flag;
    (void)render_targets;
    (void)num_render_targets;
    struct xvmc_driver_data *drv = ctx->pDriverData;

    if (config_id >= XVMC_MAX_CONFIGS || !drv->configs[config_id].in_use)
        return VA_STATUS_ERROR_INVALID_CONFIG;
    drv->context_profile = drv->configs[config_id].profile;

    /* The XvMC backend context (real hardware port/surfaces) is still
     * needed even for an H.264 config: the relay path (see
     * xvmc_RenderPicture/xvmc_EndPicture) forwards H.264 to relay-server
     * and decodes the MPEG-2 that comes back through this same local
     * XvMC context, unchanged.
     *
     * For H.264, picture_width/picture_height are the app's native
     * source resolution, not relay-server's transcoded output size --
     * the backend must be sized for the latter (see
     * xvmc_CreateSurfaces's identical override and
     * relay_resolution_width/height's comment), so neither the
     * lazy-create call nor the mismatch check below can compare against
     * picture_width/height directly for this profile. */
    int is_h264 = profile_is_h264(drv->context_profile);
    int want_width = is_h264 ? drv->relay_resolution_width : picture_width;
    int want_height = is_h264 ? drv->relay_resolution_height : picture_height;
    if (!drv->backend.context_created) {
        /* Some callers create the context before any surfaces. */
        int ctx_status = xvmc_backend_create_context(&drv->backend, want_width, want_height);
        if (ctx_status == XVMC_BACKEND_RESOLUTION_UNSUPPORTED)
            return VA_STATUS_ERROR_RESOLUTION_NOT_SUPPORTED;
        if (ctx_status != 0)
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
    } else if (drv->backend.width != want_width || drv->backend.height != want_height) {
        fprintf(stderr, "xvmc_CreateContext: %dx%d doesn't match surfaces created at %dx%d\n",
                want_width, want_height, drv->backend.width, drv->backend.height);
        return VA_STATUS_ERROR_INVALID_VALUE;
    }

    *context = 0; /* single XvMC context supported */
    return VA_STATUS_SUCCESS;
}

/* Forward-declared: defined near xvmc_relay_end_picture below, used here
 * to drain any pictures relay-server sends back after the last one this
 * driver forwarded (see that function's comment for why draining can't
 * happen synchronously per-picture). A bounded blocking wait here, at
 * teardown, is the only place this driver ever blocks on the relay
 * connection -- appropriate since there's nothing left to send. */
static VAStatus drain_relay_pictures(struct xvmc_driver_data *drv, int poll_timeout_ms, int dbg);

static VAStatus xvmc_DestroyContext(VADriverContextP ctx, VAContextID context)
{
    XVMC_SERIALIZE();
    (void)context;
    struct xvmc_driver_data *drv = ctx->pDriverData;
    if (profile_is_h264(drv->context_profile) && drv->relay_connected) {
        int dbg = getenv("XVMC_RELAY_DEBUG") != NULL;
        /* relay-server's ffmpeg (spawned fresh per connection) needs a
         * real, sustained trickle of pictures arriving over real wall-
         * clock time -- typically a few seconds -- before its own
         * stream analysis lets it produce any output at all, confirmed
         * by real testing (this is NOT about picture count in isolation:
         * bursts of many pictures followed by silence never unstick it,
         * but the same pictures arriving steadily do, without ever
         * needing the connection to close). A real, continuously-decoding
         * session satisfies this naturally over its whole lifetime; this
         * bounded drain at teardown is just to catch whatever's still
         * genuinely in flight for the last few pictures, not to wait out
         * that initial warm-up -- 8s comfortably covers the observed
         * warm-up time with margin, without hanging teardown forever if
         * the connection is already dead. */
        drain_relay_pictures(drv, 8000, dbg);
    }
    xvmc_backend_destroy_context(&drv->backend);
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_CreateBuffer(
    VADriverContextP ctx, VAContextID context, VABufferType type,
    unsigned int size, unsigned int num_elements, void *data,
    VABufferID *buf_id)
{
    XVMC_SERIALIZE();
    (void)context;
    struct xvmc_driver_data *drv = ctx->pDriverData;

    VABufferID id = alloc_buffer_slot(drv);
    if (id == (VABufferID)-1)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    unsigned int elements = num_elements ? num_elements : 1;
    /* size*elements as size_t, not unsigned int: a 32-bit product can
     * wrap to a small value while callers elsewhere (e.g.
     * xvmc_RenderPicture) recompute this same product in 64-bit,
     * leading to a huge over-read/over-copy against a tiny allocation. */
    size_t total_size = (size_t)size * (size_t)elements;
    void *copy = malloc(total_size);
    if (!copy)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    if (data)
        memcpy(copy, data, total_size);

    drv->buffers[id].type = type;
    drv->buffers[id].size = size;
    drv->buffers[id].num_elements = elements;
    drv->buffers[id].data = copy;
    drv->buffers[id].in_use = 1;

    *buf_id = id;
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_DestroyBuffer(VADriverContextP ctx, VABufferID buf_id)
{
    XVMC_SERIALIZE();
    struct xvmc_driver_data *drv = ctx->pDriverData;
    if (buf_id >= drv->buffers_capacity || !drv->buffers[buf_id].in_use)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    free(drv->buffers[buf_id].data);
    memset(&drv->buffers[buf_id], 0, sizeof(drv->buffers[buf_id]));
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_MapBuffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf)
{
    XVMC_SERIALIZE();
    struct xvmc_driver_data *drv = ctx->pDriverData;
    if (buf_id >= drv->buffers_capacity || !drv->buffers[buf_id].in_use)
        return VA_STATUS_ERROR_INVALID_BUFFER;
    *pbuf = drv->buffers[buf_id].data;
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_UnmapBuffer(VADriverContextP ctx, VABufferID buf_id)
{
    XVMC_SERIALIZE();
    (void)ctx;
    (void)buf_id;
    return VA_STATUS_SUCCESS;
}

/* Resolves a picture's forward/backward reference surfaces and its
 * picture_structure, from the given VAPictureParameterBufferMPEG2. Takes
 * pic_params explicitly (rather than reading drv->pic_params directly)
 * because the relay path (xvmc_relay_end_picture) has its own separate
 * picture-params source (parsed by mpeg2_headers.c out of relay-returned
 * bytes, not supplied by vaRenderPicture) -- this stays reusable for
 * both. */
static void resolve_references(
    struct xvmc_driver_data *drv, const VAPictureParameterBufferMPEG2 *pic_params, int has_pic_params,
    unsigned int *picture_structure, XvMCSurface **forward, XvMCSurface **backward)
{
    struct xvmc_backend *be = &drv->backend;
    *forward = NULL;
    *backward = NULL;
    if (has_pic_params) {
        VASurfaceID fwd = pic_params->forward_reference_picture;
        VASurfaceID bwd = pic_params->backward_reference_picture;
        if (fwd != VA_INVALID_SURFACE && (int)fwd < XVMC_BACKEND_MAX_SURFACES)
            *forward = &be->surfaces[fwd];
        if (bwd != VA_INVALID_SURFACE && (int)bwd < XVMC_BACKEND_MAX_SURFACES)
            *backward = &be->surfaces[bwd];
    }
    /* MPEG-2's picture_structure field (1=top field, 2=bottom field,
     * 3=frame) matches XVMC_TOP_FIELD/XVMC_BOTTOM_FIELD/XVMC_FRAME_PICTURE
     * numerically -- no translation table needed. */
    *picture_structure = has_pic_params
        ? pic_params->picture_coding_extension.bits.picture_structure
        : XVMC_FRAME_PICTURE;
}

/* Renders one slice's worth of already-decoded macroblocks
 * [first_macroblock, first_macroblock+num_macroblocks) via
 * XvMCRenderSurface. Shared by both the local MPEG-2 decode path
 * (xvmc_RenderPicture) and the relay path (xvmc_relay_end_picture) --
 * calling XvMCRenderSurface in small per-slice chunks like this, rather
 * than once for a whole picture, is required to stay within the real
 * i915 XvMC library's fixed 8KB GPU-command batch buffer (see the
 * batch-buffer-overflow fix's commit message for the full root-cause
 * writeup). */
static VAStatus render_slice_macroblocks(
    struct xvmc_driver_data *drv, unsigned int picture_structure,
    XvMCSurface *forward, XvMCSurface *backward,
    unsigned int first_macroblock, unsigned int num_macroblocks, int target_surface)
{
    if (num_macroblocks == 0)
        return VA_STATUS_SUCCESS;

    struct xvmc_backend *be = &drv->backend;
    XvMCSurface *target = &be->surfaces[target_surface];
    Status rst = XvMCRenderSurface(
        be->dpy, &be->context, picture_structure, target, forward, backward,
        0, num_macroblocks, first_macroblock, &be->macroblocks, &be->blocks);
    if (rst != Success) {
        fprintf(stderr, "render_slice_macroblocks: XvMCRenderSurface failed (%d)\n", rst);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    return VA_STATUS_SUCCESS;
}

/* Debug tooling only, gated by XVMC_SNAPSHOT_DIR: syncs the render
 * target (XvMCFlushSurface is a documented no-op on this backend, see
 * xvmc_EndPicture's comment, but XvMCSyncSurface actually waits for the
 * GPU, which a snapshot needs to avoid racing in-flight rendering) and
 * blits it to a PPM file via xvmc_backend_snapshot_surface. This is the
 * only way this project can currently look at actual decoded pixels
 * from the relay path -- correctness elsewhere has always been checked
 * against macroblock-level ground truth instead, since
 * vaGetImage/vaDeriveImage are deliberately unimplemented (XvMC surfaces
 * are opaque render targets, see test/render_stress_test.c's file
 * comment). Errors are logged but never fail the caller -- this must
 * never break real decoding, only observe it. `target_surface` is the
 * surface actually rendered into for this picture -- for the relay
 * path that's the queue-popped surface (see relay_surface_queue_pop),
 * NOT necessarily drv->render_target, since the two can legitimately
 * differ once relay-server's pipeline lag comes into play. */
static void maybe_snapshot_surface(struct xvmc_driver_data *drv, int target_surface)
{
    const char *dir = getenv("XVMC_SNAPSHOT_DIR");
    if (!dir)
        return;

    struct xvmc_backend *be = &drv->backend;
    XvMCSurface *target = &be->surfaces[target_surface];
    XvMCSyncSurface(be->dpy, target);

    char path[4096];
    snprintf(path, sizeof(path), "%s/frame%05u.ppm", dir, drv->snapshot_frame_count);
    if (xvmc_backend_snapshot_surface(be, target_surface, path) == 0)
        drv->snapshot_frame_count++;
}

static VAStatus xvmc_BeginPicture(
    VADriverContextP ctx, VAContextID context, VASurfaceID render_target)
{
    XVMC_SERIALIZE();
    (void)context;
    struct xvmc_driver_data *drv = ctx->pDriverData;
    drv->render_target = (int)render_target;
    drv->mb_count = 0;
    drv->has_pic_params = 0;
    drv->has_iq_matrix = 0;
    drv->num_pending_slices = 0;
    drv->pending_slices = NULL;
    drv->h264_frame_len = 0;
    return VA_STATUS_SUCCESS;
}

/* Appends `len` bytes to drv->h264_frame_buf, growing it as needed.
 * Returns 0 on success, -1 on allocation failure. */
static int append_h264_bytes(struct xvmc_driver_data *drv, const void *data, size_t len)
{
    if (drv->h264_frame_len + len > drv->h264_frame_cap) {
        size_t new_cap = drv->h264_frame_cap ? drv->h264_frame_cap * 2 : 65536;
        while (new_cap < drv->h264_frame_len + len)
            new_cap *= 2;
        uint8_t *grown = realloc(drv->h264_frame_buf, new_cap);
        if (!grown)
            return -1;
        drv->h264_frame_buf = grown;
        drv->h264_frame_cap = new_cap;
    }
    memcpy(drv->h264_frame_buf + drv->h264_frame_len, data, len);
    drv->h264_frame_len += len;
    return 0;
}

/* Appends `len` bytes to drv->relay_recv_buf, growing it as needed (same
 * pattern as append_h264_bytes). Returns 0 on success, -1 on allocation
 * failure. */
static int append_relay_recv_bytes(struct xvmc_driver_data *drv, const void *data, size_t len)
{
    if (drv->relay_recv_len + len > drv->relay_recv_cap) {
        size_t new_cap = drv->relay_recv_cap ? drv->relay_recv_cap * 2 : 65536;
        while (new_cap < drv->relay_recv_len + len)
            new_cap *= 2;
        uint8_t *grown = realloc(drv->relay_recv_buf, new_cap);
        if (!grown)
            return -1;
        drv->relay_recv_buf = grown;
        drv->relay_recv_cap = new_cap;
    }
    memcpy(drv->relay_recv_buf + drv->relay_recv_len, data, len);
    drv->relay_recv_len += len;
    return 0;
}

/* Forwards the accumulated H.264 frame to relay-server (connecting
 * lazily on first use), then blocks reading relay_recv() rounds into
 * drv->relay_recv_buf until mpeg2_headers_parse_picture can extract one
 * complete picture's worth of MPEG-2 slices out of it, decoding each
 * through the same mpeg2_vld_decode_slice + render_slice_macroblocks
 * pipeline the local MPEG-2 path already uses. This is synchronous --
 * one network round trip per input picture -- by design for this stage;
 * hiding that latency behind a ring buffer of decoded surfaces is
 * explicitly out of scope here (a later phase, once this path is proven
 * correct). */
static VAStatus xvmc_relay_end_picture(VADriverContextP ctx, struct xvmc_driver_data *drv)
{
    (void)ctx;
    int dbg = getenv("XVMC_RELAY_DEBUG") != NULL;
    if (dbg) { fprintf(stderr, "[relay] xvmc_relay_end_picture: enter, h264_frame_len=%zu\n", drv->h264_frame_len); fflush(stderr); }

    if (!drv->relay_connected) {
        if (relay_connect(&drv->relay, drv->relay_host, drv->relay_port) != 0) {
            fprintf(stderr, "xvmc_relay_end_picture: relay_connect(%s:%u) failed\n",
                    drv->relay_host, drv->relay_port);
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        drv->relay_connected = 1;
        drv->h264_sps_pps_sent = 0;
        if (dbg) { fprintf(stderr, "[relay] connected\n"); fflush(stderr); }

        const char *dump_path = getenv("XVMC_RELAY_DUMP");
        if (dump_path && !drv->relay_dump_fp) {
            drv->relay_dump_fp = fopen(dump_path, "wb");
            if (!drv->relay_dump_fp)
                fprintf(stderr, "xvmc_relay_end_picture: fopen(%s) for dump failed\n", dump_path);
        }
        const char *recv_dump_path = getenv("XVMC_RELAY_RECV_DUMP");
        if (recv_dump_path && !drv->relay_recv_dump_fp) {
            drv->relay_recv_dump_fp = fopen(recv_dump_path, "wb");
            if (!drv->relay_recv_dump_fp)
                fprintf(stderr, "xvmc_relay_end_picture: fopen(%s) for recv dump failed\n", recv_dump_path);
        }
    }

    /* Real VA-API clients never hand this driver SPS/PPS NAL units --
     * only the parsed VAPictureParameterBufferH264 fields, since real
     * hardware decoders configure themselves from that directly. But
     * relay-server's ffmpeg is a standalone Annex-B decoder that needs a
     * real SPS/PPS to interpret the slice NALs at all, so synthesize and
     * send them once per connection, before the first frame. */
    if (!drv->h264_sps_pps_sent) {
        if (!drv->has_h264_pic_params) {
            fprintf(stderr, "xvmc_relay_end_picture: no VAPictureParameterBufferH264 "
                             "captured yet, can't synthesize SPS/PPS\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        uint8_t sps[128], pps[128];
        int sps_len = h264_synthesize_sps(&drv->h264_pic_params, drv->context_profile,
                                           sps, (int)sizeof(sps));
        /* num_ref_idx_l0_default_active_minus1 isn't carried by
         * VAPictureParameterBufferH264 directly, and sampling any single
         * slice's *effective* active count is unreliable (that value is
         * very commonly an explicit per-slice override, not the true
         * default -- confirmed by real testing: the first slice seen is
         * often an I-slice where the field is meaningless, and even a
         * later P-slice can carry an override that doesn't match the
         * real default). num_ref_frames IS captured directly, and
         * "default to using every available reference frame" is the
         * conventional real-encoder behavior confirmed against a real
         * capture (num_ref_frames=3 -> real PPS default=2). Baseline
         * profile has no B-frames/L1 list, so l1's default is always 0. */
        uint8_t num_ref_idx_l0_default_active_minus1 =
            drv->h264_pic_params.num_ref_frames > 0
                ? (uint8_t)(drv->h264_pic_params.num_ref_frames - 1)
                : 0;
        int pps_len = h264_synthesize_pps(&drv->h264_pic_params,
                                           num_ref_idx_l0_default_active_minus1,
                                           0,
                                           pps, (int)sizeof(pps));
        if (sps_len < 0 || pps_len < 0) {
            fprintf(stderr, "xvmc_relay_end_picture: SPS/PPS synthesis failed\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (relay_send(&drv->relay, sps, (size_t)sps_len) != sps_len ||
            relay_send(&drv->relay, pps, (size_t)pps_len) != pps_len) {
            fprintf(stderr, "xvmc_relay_end_picture: relay_send (SPS/PPS) failed\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (drv->relay_dump_fp) {
            fwrite(sps, 1, (size_t)sps_len, drv->relay_dump_fp);
            fwrite(pps, 1, (size_t)pps_len, drv->relay_dump_fp);
        }
        drv->h264_sps_pps_sent = 1;
        if (dbg) { fprintf(stderr, "[relay] sent SPS(%d)+PPS(%d)\n", sps_len, pps_len); fflush(stderr); }
    }

    if (drv->h264_frame_len > 0) {
        ssize_t sent = relay_send(&drv->relay, drv->h264_frame_buf, drv->h264_frame_len);
        if (sent < 0 || (size_t)sent != drv->h264_frame_len) {
            fprintf(stderr, "xvmc_relay_end_picture: relay_send failed\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (drv->relay_dump_fp)
            fwrite(drv->h264_frame_buf, 1, drv->h264_frame_len, drv->relay_dump_fp);
        if (dbg) { fprintf(stderr, "[relay] sent frame, %zu bytes\n", drv->h264_frame_len); fflush(stderr); }

        /* Records which surface THIS picture's eventual decoded result
         * must render into, once relay-server gets around to returning
         * it (see relay_surface_queue_push's comment and
         * drain_relay_pictures) -- relay-server transcodes 1:1,
         * preserving order, so the Nth picture sent corresponds to the
         * Nth picture popped back out. */
        relay_surface_queue_push(drv, drv->render_target);

        /* Annex-B has no NAL length field -- a demuxer only knows the
         * slice NAL(s) just sent are complete once it sees the start of
         * the *next* NAL, or EOF. Since this protocol sends exactly one
         * picture per round trip and then blocks for the result, without
         * this relay-server's ffmpeg would hang forever unable to
         * confirm the slice is finished (confirmed by real testing: see
         * h264_reconstitute.h's file comment). Sending an AUD
         * immediately after gives it that boundary without waiting for
         * a real next frame or connection teardown. */
        uint8_t aud[5];
        int aud_len = h264_write_access_unit_delimiter(aud, (int)sizeof(aud));
        if (relay_send(&drv->relay, aud, (size_t)aud_len) != aud_len) {
            fprintf(stderr, "xvmc_relay_end_picture: relay_send (AUD) failed\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (drv->relay_dump_fp) {
            fwrite(aud, 1, (size_t)aud_len, drv->relay_dump_fp);
            fflush(drv->relay_dump_fp);
        }
        if (dbg) { fprintf(stderr, "[relay] sent AUD\n"); fflush(stderr); }
    }

    /* Never block here waiting for THIS picture's decoded result:
     * relay-server spawns a fresh ffmpeg per connection, and its own
     * stream analysis (avformat_find_stream_info) needs several
     * pictures' worth of real data before it'll produce any output at
     * all -- confirmed by real testing, where blocking per-picture
     * deadlocked forever (each side waiting on the other) regardless of
     * probe-size ffmpeg flags or Annex-B framing fixes (AUD insertion),
     * since neither addresses avformat needing multiple actual pictures.
     * Draining non-blockingly here means this driver keeps sending
     * pictures continuously across repeated xvmc_EndPicture calls,
     * giving relay-server's ffmpeg the multiple pictures its analysis
     * phase needs; whatever it eventually decodes gets rendered on a
     * later call to this function once available (see
     * drain_relay_pictures), and any still in flight when the app tears
     * down the context get a final bounded wait in xvmc_DestroyContext. */
    if (getenv("XVMC_PROFILE")) {
        struct timespec ts0, ts1;
        clock_gettime(CLOCK_MONOTONIC, &ts0);
        VAStatus rst = drain_relay_pictures(drv, 0, dbg);
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        double ms = (double)(ts1.tv_sec - ts0.tv_sec) * 1000.0 +
                    (double)(ts1.tv_nsec - ts0.tv_nsec) / 1e6;
        fprintf(stderr, "[prof] drain_relay_pictures: %.1fms\n", ms);
        return rst;
    }
    return drain_relay_pictures(drv, 0, dbg);
}

/* Parses and renders as many complete pictures as are currently
 * buffered in drv->relay_recv_buf, then either:
 *   - poll_timeout_ms < 0: blocks on relay_recv() for more (used only
 *     at teardown, see xvmc_DestroyContext, since nothing more will
 *     ever be sent past that point);
 *   - poll_timeout_ms >= 0: only reads more if relay_poll_readable()
 *     says data is available within that many milliseconds, otherwise
 *     returns immediately (used after every picture sent during normal
 *     operation, see xvmc_relay_end_picture, so this driver's caller
 *     never stalls waiting for a specific picture's response).
 * Returns VA_STATUS_SUCCESS whether or not any picture was actually
 * available -- "nothing to drain right now" is not a failure. */
static double prof_now_ms_drv(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static VAStatus drain_relay_pictures(struct xvmc_driver_data *drv, int poll_timeout_ms, int dbg)
{
    int prof = getenv("XVMC_PROFILE") != NULL;
    double prof_decode_ms = 0, prof_render_ms = 0, prof_recv_ms = 0, prof_poll_ms = 0;
    int prof_pictures = 0;

    struct mpeg2_parsed_slice slices[64];
    for (;;) {
        unsigned int num_slices = 0;
        int consumed = mpeg2_headers_parse_picture(
            &drv->relay_header_state, drv->relay_recv_buf, (uint32_t)drv->relay_recv_len,
            slices, 64, &num_slices);
        if (dbg) { fprintf(stderr, "[relay] parse_picture consumed=%d recv_len=%zu\n", consumed, drv->relay_recv_len); fflush(stderr); }
        if (consumed < 0) {
            fprintf(stderr, "drain_relay_pictures: MPEG-2 header parse error\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }

        if (consumed > 0) {
            /* This picture's decoded content must land on the surface
             * ffmpeg (or any real VA-API caller) originally requested
             * for it via vaBeginPicture -- NOT whatever drv->render_target
             * currently is, since relay-server's pipelined/lagged
             * transcode means "currently" can be many pictures further
             * along than whichever picture this one actually is. See
             * relay_surface_queue_pop's comment. Falling back to
             * drv->render_target on an empty queue matches this
             * function's pre-fix behavior (better than refusing to
             * render at all), but should never actually happen in
             * normal operation. */
            int target_surface = relay_surface_queue_pop(drv);
            if (target_surface < 0) {
                fprintf(stderr, "drain_relay_pictures: surface queue empty, "
                                 "falling back to render_target=%d\n", drv->render_target);
                target_surface = drv->render_target;
            }

            unsigned int picture_structure;
            XvMCSurface *forward, *backward;
            resolve_references(drv, &drv->relay_header_state.pic_params,
                                drv->relay_header_state.has_pic_params,
                                &picture_structure, &forward, &backward);
            /* mpeg2_headers.c always reports both reference fields as
             * invalid (see relay_last_decoded_surface's comment) --
             * override with this driver's own tracking for P-pictures.
             * B-pictures are intentionally left with no reference at all
             * (not currently handled; see the same comment). */
            int32_t picture_coding_type = drv->relay_header_state.pic_params.picture_coding_type;
            if (picture_coding_type == 2 /* P */ && drv->relay_last_decoded_surface >= 0)
                forward = &drv->backend.surfaces[drv->relay_last_decoded_surface];

            unsigned int block_cursor = 0;
            drv->mb_count = 0;
            prof_pictures++;
            for (unsigned int s = 0; s < num_slices; s++) {
                struct mpeg2_parsed_slice *sl = &slices[s];
                double d0 = prof ? prof_now_ms_drv() : 0;
                int decoded = mpeg2_vld_decode_slice(
                    sl->data, sl->size, sl->macroblock_bit_offset,
                    sl->slice_horizontal_position, sl->slice_vertical_position,
                    sl->quantiser_scale_code, sl->intra_slice_flag,
                    &drv->relay_header_state.pic_params,
                    drv->relay_header_state.has_iq_matrix ? &drv->relay_header_state.iq_matrix : NULL,
                    &drv->backend.macroblocks, drv->mb_count,
                    &drv->backend.blocks, &block_cursor,
                    (drv->backend.surface_flags & XVMC_INTRA_UNSIGNED) != 0);
                if (prof) prof_decode_ms += prof_now_ms_drv() - d0;
                if (decoded < 0) {
                    fprintf(stderr, "drain_relay_pictures: slice decode failed\n");
                    continue;
                }
                if (decoded > 0) {
                    double r0 = prof ? prof_now_ms_drv() : 0;
                    VAStatus rst = render_slice_macroblocks(
                        drv, picture_structure, forward, backward,
                        drv->mb_count, (unsigned int)decoded, target_surface);
                    if (prof) prof_render_ms += prof_now_ms_drv() - r0;
                    if (rst != VA_STATUS_SUCCESS)
                        return rst;
                }
                drv->mb_count += (unsigned int)decoded;
            }
            /* This picture is now a valid reference for whatever P-picture
             * comes next (I-pictures are references too, of course --
             * only B-pictures, never produced here, wouldn't be). */
            if (drv->mb_count > 0)
                drv->relay_last_decoded_surface = target_surface;
            /* Only worth a snapshot if something was actually rendered
             * this picture -- calling it unconditionally, including for
             * pictures where every slice failed to decode (confirmed by
             * real testing: a stream with many back-to-back corrupt
             * pictures can call this hundreds of times in a burst),
             * exhausts the real i915 XvMC backend's XV resources fast
             * enough to crash the whole X connection (BadAlloc). */
            if (drv->mb_count > 0)
                maybe_snapshot_surface(drv, target_surface);

            memmove(drv->relay_recv_buf, drv->relay_recv_buf + consumed,
                    drv->relay_recv_len - (size_t)consumed);
            drv->relay_recv_len -= (size_t)consumed;
            continue; /* there may be another complete picture already buffered */
        }

        /* consumed == 0: not enough data yet for a full picture. */
        if (poll_timeout_ms >= 0) {
            double p0 = prof ? prof_now_ms_drv() : 0;
            int ready = relay_poll_readable(&drv->relay, poll_timeout_ms);
            if (prof) prof_poll_ms += prof_now_ms_drv() - p0;
            if (ready <= 0) {
                if (prof && (prof_pictures || prof_decode_ms > 0 || prof_render_ms > 0))
                    fprintf(stderr, "[prof] drain breakdown: pictures=%d decode=%.1fms "
                                     "render=%.1fms poll=%.1fms recv=%.1fms\n",
                            prof_pictures, prof_decode_ms, prof_render_ms, prof_poll_ms, prof_recv_ms);
                return VA_STATUS_SUCCESS; /* nothing available right now -- don't block */
            }
        }

        uint8_t chunk[4096];
        if (dbg) { fprintf(stderr, "[relay] recv...\n"); fflush(stderr); }
        double n0 = prof ? prof_now_ms_drv() : 0;
        ssize_t got = relay_recv(&drv->relay, chunk, sizeof(chunk));
        if (prof) prof_recv_ms += prof_now_ms_drv() - n0;
        if (dbg) { fprintf(stderr, "[relay] recv returned %zd\n", got); fflush(stderr); }
        if (got <= 0) {
            fprintf(stderr, "drain_relay_pictures: relay_recv failed or connection closed\n");
            return VA_STATUS_ERROR_OPERATION_FAILED;
        }
        if (drv->relay_recv_dump_fp) {
            fwrite(chunk, 1, (size_t)got, drv->relay_recv_dump_fp);
            fflush(drv->relay_recv_dump_fp);
        }
        if (append_relay_recv_bytes(drv, chunk, (size_t)got) != 0) {
            fprintf(stderr, "drain_relay_pictures: out of memory buffering relay response\n");
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
        }
    }
}

static VAStatus xvmc_RenderPicture(
    VADriverContextP ctx, VAContextID context, VABufferID *buffers,
    int num_buffers)
{
    XVMC_SERIALIZE();
    (void)context;
    struct xvmc_driver_data *drv = ctx->pDriverData;

    for (int i = 0; i < num_buffers; i++) {
        VABufferID id = buffers[i];
        if (id >= drv->buffers_capacity || !drv->buffers[id].in_use) {
            fprintf(stderr, "xvmc_RenderPicture: invalid buffer id %u\n", id);
            continue;
        }
        struct xvmc_buffer *buf = &drv->buffers[id];

        switch (buf->type) {
        case VAPictureParameterBufferType:
            if (profile_is_h264(drv->context_profile)) {
                memcpy(&drv->h264_pic_params, buf->data, sizeof(drv->h264_pic_params));
                drv->has_h264_pic_params = 1;
            } else {
                memcpy(&drv->pic_params, buf->data, sizeof(drv->pic_params));
                drv->has_pic_params = 1;
            }
            break;

        case VAIQMatrixBufferType:
            memcpy(&drv->iq_matrix, buf->data, sizeof(drv->iq_matrix));
            drv->has_iq_matrix = 1;
            break;

        case VASliceParameterBufferType:
            /* Not needed for H.264: real client software's own
             * VASliceParameterBufferH264 fields aren't something this
             * driver has any use for -- relay-server's ffmpeg does all
             * H.264 parsing, this driver only ever forwards raw bytes.
             * (num_ref_idx_l{0,1}_active_minus1 looks tempting to sample
             * from here for the PPS's own default -- see
             * h264_synthesize_pps's caller -- but it's unreliable: this
             * field reflects each slice's *effective* active count,
             * which is very commonly an explicit per-slice override, not
             * the true PPS default; sampling whichever slice happens to
             * arrive first can silently pick up an override instead of
             * the default, exactly reproducing this bug rather than
             * fixing it, confirmed by real testing.) Discarded rather
             * than stored to avoid stashing a wrongly-typed pointer in
             * the MPEG-2-shaped pending_slices field. */
            if (!profile_is_h264(drv->context_profile)) {
                drv->pending_slices = buf->data;
                drv->num_pending_slices = buf->num_elements;
            }
            break;

        case VASliceDataBufferType: {
            if (profile_is_h264(drv->context_profile)) {
                /* VASliceDataBufferType for H.264 already contains the
                 * complete original NAL bytes (1-byte header + escaped
                 * payload) -- confirmed against FFmpeg's own
                 * vaapi_h264.c (nal->raw_data/raw_size) and va.h's
                 * VASliceParameterBufferH264.slice_data_bit_offset doc
                 * comment. Only the Annex-B start code is missing, since
                 * VA-API itself never frames slice buffers that way
                 * (real hardware decoders don't need start codes). One
                 * VASliceDataBufferType is treated as one NAL here,
                 * which holds for the common single-slice-per-buffer
                 * case real encoders produce. */
                uint8_t start_code[3];
                h264_write_start_code(start_code);
                if (append_h264_bytes(drv, start_code, sizeof(start_code)) != 0 ||
                    append_h264_bytes(drv, buf->data, (size_t)buf->size * buf->num_elements) != 0) {
                    fprintf(stderr, "xvmc_RenderPicture: out of memory buffering H.264 frame\n");
                    return VA_STATUS_ERROR_ALLOCATION_FAILED;
                }
                break;
            }
            if (!drv->has_pic_params || drv->num_pending_slices == 0) {
                fprintf(stderr, "xvmc_RenderPicture: slice data with no pending "
                                 "slice-parameter buffer or picture params\n");
                break;
            }
            unsigned int picture_structure;
            XvMCSurface *forward, *backward;
            resolve_references(drv, &drv->pic_params, drv->has_pic_params,
                                &picture_structure, &forward, &backward);

            unsigned int block_cursor = drv->mb_count * 6;
            size_t slice_buf_total = (size_t)buf->size * (size_t)buf->num_elements;
            for (unsigned int s = 0; s < drv->num_pending_slices; s++) {
                VASliceParameterBufferMPEG2 *sp = &drv->pending_slices[s];
                /* slice_data_offset/slice_data_size come from the VA-API
                 * caller, not this driver -- bounds-check against the
                 * slice-data buffer's real allocated size before using
                 * them, rather than trusting them to describe a range
                 * actually inside buf->data. */
                if ((size_t)sp->slice_data_offset > slice_buf_total ||
                    (size_t)sp->slice_data_size > slice_buf_total - sp->slice_data_offset) {
                    fprintf(stderr, "xvmc_RenderPicture: slice %u offset/size (%u/%u) "
                                     "exceeds slice-data buffer (%zu bytes)\n",
                            s, sp->slice_data_offset, sp->slice_data_size, slice_buf_total);
                    continue;
                }
                const uint8_t *slice_bytes = (const uint8_t *)buf->data + sp->slice_data_offset;
                int decoded = mpeg2_vld_decode_slice(
                    slice_bytes, sp->slice_data_size, sp->macroblock_offset,
                    sp->slice_horizontal_position, sp->slice_vertical_position,
                    sp->quantiser_scale_code, sp->intra_slice_flag,
                    &drv->pic_params, drv->has_iq_matrix ? &drv->iq_matrix : NULL,
                    &drv->backend.macroblocks, drv->mb_count,
                    &drv->backend.blocks, &block_cursor,
                    (drv->backend.surface_flags & XVMC_INTRA_UNSIGNED) != 0);
                if (decoded < 0) {
                    fprintf(stderr, "xvmc_RenderPicture: slice decode failed\n");
                    continue;
                }
                if (decoded > 0) {
                    /* Render this slice's macroblocks immediately, in a
                     * small per-slice chunk, rather than accumulating the
                     * whole picture and rendering once in
                     * xvmc_EndPicture -- see render_slice_macroblocks's
                     * comment for why (the real i915 XvMC library's fixed
                     * 8KB GPU-command batch buffer). */
                    VAStatus rst = render_slice_macroblocks(
                        drv, picture_structure, forward, backward,
                        drv->mb_count, (unsigned int)decoded, drv->render_target);
                    if (rst != VA_STATUS_SUCCESS)
                        return rst;
                }
                drv->mb_count += (unsigned int)decoded;
            }
            drv->num_pending_slices = 0;
            drv->pending_slices = NULL;
            break;
        }

        default:
            /* Buffer types not needed yet (subpicture, etc). */
            break;
        }
    }

    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_EndPicture(VADriverContextP ctx, VAContextID context)
{
    XVMC_SERIALIZE();
    (void)context;
    struct xvmc_driver_data *drv = ctx->pDriverData;

    if (profile_is_h264(drv->context_profile))
        return xvmc_relay_end_picture(ctx, drv);

    if (getenv("MPEG2_VLD_DEBUG_MBCOUNT"))
        fprintf(stderr, "MBCOUNT %u\n", drv->mb_count);

    if (drv->mb_count == 0) {
        fprintf(stderr, "xvmc_EndPicture: no macroblocks decoded this picture "
                         "-- nothing was rendered\n");
        return VA_STATUS_SUCCESS;
    }

    /* Rendering already happened incrementally, per slice, back in
     * xvmc_RenderPicture (see the comment there for why: the real i915
     * XvMC backend's internal GPU-command batch buffer is a fixed 8KB
     * with no internal chunking, and a whole frame's macroblocks in one
     * XvMCRenderSurface call reliably overflows it). All that's left
     * here is XvMCFlushSurface, which the XvMC API contract calls for
     * even though this specific backend's implementation is a documented
     * no-op (confirmed against xf86-video-intel source) -- kept for
     * correctness against the public API rather than this one backend's
     * implementation detail. */
    struct xvmc_backend *be = &drv->backend;
    XvMCSurface *target = &be->surfaces[drv->render_target];
    Status st = XvMCFlushSurface(be->dpy, target);
    if (st != Success) {
        fprintf(stderr, "xvmc_EndPicture: XvMCFlushSurface failed (%d)\n", st);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }
    maybe_snapshot_surface(drv, drv->render_target);
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_SyncSurface(VADriverContextP ctx, VASurfaceID render_target)
{
    XVMC_SERIALIZE();
    struct xvmc_driver_data *drv = ctx->pDriverData;
    if ((int)render_target < 0 || (int)render_target >= XVMC_BACKEND_MAX_SURFACES)
        return VA_STATUS_ERROR_INVALID_SURFACE;
    if (XvMCSyncSurface(drv->backend.dpy, &drv->backend.surfaces[render_target]) != Success)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_QuerySurfaceStatus(
    VADriverContextP ctx, VASurfaceID render_target, VASurfaceStatus *status)
{
    XVMC_SERIALIZE();
    struct xvmc_driver_data *drv = ctx->pDriverData;
    if ((int)render_target < 0 || (int)render_target >= XVMC_BACKEND_MAX_SURFACES) {
        *status = VASurfaceReady;
        return VA_STATUS_ERROR_INVALID_SURFACE;
    }

    int xvmc_status = 0;
    XvMCGetSurfaceStatus(drv->backend.dpy, &drv->backend.surfaces[render_target], &xvmc_status);
    *status = (xvmc_status & (XVMC_RENDERING | XVMC_DISPLAYING)) ? VASurfaceRendering : VASurfaceReady;
    return VA_STATUS_SUCCESS;
}

/*
 * Real display path: blits a decoded surface directly onto the
 * caller's own X11 window via XvMCPutSurface. This is what lets a real
 * media player (mpv/mplayer/ffplay -- anything using vaPutSurface's
 * classic X11 display path rather than pulling frames back to system
 * memory) actually show video through this driver, without needing
 * vaGetImage/vaDeriveImage at all -- those remain deliberately
 * unimplemented (see their own comments): XvMC surfaces are opaque,
 * direct-to-display render targets, and this is the intended way to
 * consume one. cliprects/flags are accepted per the VA-API contract
 * but not supported -- no real caller has needed clipping/de-
 * interlacing flags against this driver's small surface set so far.
 */
static VAStatus xvmc_PutSurface(
    VADriverContextP ctx, VASurfaceID surface, void *draw,
    short srcx, short srcy, unsigned short srcw, unsigned short srch,
    short destx, short desty, unsigned short destw, unsigned short desth,
    VARectangle *cliprects, unsigned int number_cliprects, unsigned int flags)
{
    XVMC_SERIALIZE();
    (void)cliprects;
    (void)number_cliprects;
    (void)flags;

    struct xvmc_driver_data *drv = ctx->pDriverData;
    if ((int)surface < 0 || (int)surface >= XVMC_BACKEND_MAX_SURFACES)
        return VA_STATUS_ERROR_INVALID_SURFACE;

    if (xvmc_backend_put_surface(&drv->backend, (int)surface, (Drawable)(uintptr_t)draw,
                                  srcx, srcy, srcw, srch, destx, desty, destw, desth) != 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_QuerySurfaceAttributes(
    VADriverContextP ctx, VAConfigID config,
    VASurfaceAttrib *attrib_list, unsigned int *num_attribs)
{
    XVMC_SERIALIZE();
    struct xvmc_driver_data *drv = ctx->pDriverData;

    /* Matches XVMC_BACKEND_MAX_WIDTH/HEIGHT (xvmc_backend.h) -- confirmed
     * by real bisection on the GMA950 hardware this driver targets
     * (720x576 succeeds, anything past it makes XvMCCreateContext raise
     * a fatal X protocol error instead of failing gracefully). A
     * well-behaved caller like ffmpeg's vaapi hwaccel queries this
     * before ever calling vaCreateSurfaces and self-rejects cleanly
     * using these bounds; xvmc_backend_create_context enforces the same
     * ceiling directly as a backstop for callers that don't.
     *
     * For an H.264 config, though, this cap does NOT apply to the app's
     * own picture size: relay-server transcodes it down to
     * RELAY_RESOLUTION before this driver's local XvMC context ever
     * sees it (see xvmc_CreateSurfaces/xvmc_CreateContext's
     * relay_resolution_width/height override). Advertising the raw
     * hardware ceiling here for H.264 would make a well-behaved caller
     * like ffmpeg self-reject any H.264 source above 720x576 before
     * ever giving that override a chance to run -- confirmed for real:
     * a native 1280x960 H.264 source, with RELAY_RESOLUTION/
     * XVMC_RELAY_RESOLUTION both correctly set to 640x480, was still
     * rejected client-side ("Hardware does not support image size
     * 1280x960") purely because of this query. So for H.264 configs,
     * advertise a generous ceiling instead -- large enough for any real
     * source, since the actual local decode resolution is governed
     * entirely by relay_resolution_width/height, not this attribute. */
    int is_h264_config = config < XVMC_MAX_CONFIGS && drv->configs[config].in_use &&
                          profile_is_h264(drv->configs[config].profile);
    int max_width = is_h264_config ? 4096 : XVMC_BACKEND_MAX_WIDTH;
    int max_height = is_h264_config ? 4096 : XVMC_BACKEND_MAX_HEIGHT;

    /* PixelFormat reports NV12 purely for size/format negotiation --
     * XvMC surfaces are opaque render targets (never mapped to system
     * memory via vaGetImage, which this driver correctly reports
     * UNIMPLEMENTED), so this is a nominal format label, not a real
     * memory layout guarantee. */
    const unsigned int count = 5;
    if (!attrib_list) {
        *num_attribs = count;
        return VA_STATUS_SUCCESS;
    }
    if (*num_attribs < count) {
        *num_attribs = count;
        return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
    }

    attrib_list[0].type = VASurfaceAttribPixelFormat;
    attrib_list[0].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[0].value.type = VAGenericValueTypeInteger;
    attrib_list[0].value.value.i = VA_FOURCC_NV12;

    attrib_list[1].type = VASurfaceAttribMinWidth;
    attrib_list[1].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[1].value.type = VAGenericValueTypeInteger;
    attrib_list[1].value.value.i = 16;

    attrib_list[2].type = VASurfaceAttribMaxWidth;
    attrib_list[2].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[2].value.type = VAGenericValueTypeInteger;
    attrib_list[2].value.value.i = max_width;

    attrib_list[3].type = VASurfaceAttribMinHeight;
    attrib_list[3].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[3].value.type = VAGenericValueTypeInteger;
    attrib_list[3].value.value.i = 16;

    attrib_list[4].type = VASurfaceAttribMaxHeight;
    attrib_list[4].flags = VA_SURFACE_ATTRIB_GETTABLE;
    attrib_list[4].value.type = VAGenericValueTypeInteger;
    attrib_list[4].value.value.i = max_height;

    *num_attribs = count;
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_QueryConfigAttributes(
    VADriverContextP ctx, VAConfigID config_id,
    VAProfile *profile, VAEntrypoint *entrypoint,
    VAConfigAttrib *attrib_list, int *num_attribs)
{
    XVMC_SERIALIZE();
    struct xvmc_driver_data *drv = ctx->pDriverData;
    if (config_id >= XVMC_MAX_CONFIGS || !drv->configs[config_id].in_use)
        return VA_STATUS_ERROR_INVALID_CONFIG;
    *profile = drv->configs[config_id].profile;
    *entrypoint = VAEntrypointVLD;
    if (*num_attribs > 0) {
        attrib_list[0].type = VAConfigAttribRTFormat;
        attrib_list[0].value = VA_RT_FORMAT_YUV420;
    }
    *num_attribs = 1;
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_BufferSetNumElements(
    VADriverContextP ctx, VABufferID buf_id, unsigned int num_elements)
{
    XVMC_SERIALIZE();
    struct xvmc_driver_data *drv = ctx->pDriverData;
    if (buf_id >= drv->buffers_capacity || !drv->buffers[buf_id].in_use)
        return VA_STATUS_ERROR_INVALID_BUFFER;

    struct xvmc_buffer *buf = &drv->buffers[buf_id];
    if (num_elements <= buf->num_elements) {
        buf->num_elements = num_elements;
        return VA_STATUS_SUCCESS;
    }

    /* size_t, not unsigned int -- see xvmc_CreateBuffer's identical
     * overflow note. */
    size_t new_size = (size_t)buf->size * (size_t)num_elements;
    void *grown = realloc(buf->data, new_size);
    if (!grown)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    buf->data = grown;
    buf->num_elements = num_elements;
    return VA_STATUS_SUCCESS;
}

/* NV12 is the one format this driver transfers surfaces into -- see
 * xvmc_GetImage's comment for why (it's what the real PutSurface+
 * GetImage round trip this driver already has working produces most
 * directly, and what real callers like ffmpeg's vaapi hwaccel transfer
 * path expect by default). */
static VAStatus xvmc_QueryImageFormats(
    VADriverContextP ctx, VAImageFormat *format_list, int *num_formats)
{
    XVMC_SERIALIZE();
    (void)ctx;
    memset(&format_list[0], 0, sizeof(format_list[0]));
    format_list[0].fourcc = VA_FOURCC_NV12;
    format_list[0].byte_order = VA_LSB_FIRST;
    format_list[0].bits_per_pixel = 12;
    *num_formats = 1;
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_CreateImage(
    VADriverContextP ctx, VAImageFormat *format,
    int width, int height, VAImage *image)
{
    XVMC_SERIALIZE();
    struct xvmc_driver_data *drv = ctx->pDriverData;
    if (format->fourcc != VA_FOURCC_NV12)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

    int slot = -1;
    for (int i = 0; i < XVMC_MAX_IMAGES; i++) {
        if (!drv->images[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    /* NV12: one full-resolution Y plane, one half-resolution
     * interleaved-UV plane (see xvmc_GetImage's rgb_to_nv12 for the
     * matching conversion). */
    uint32_t y_size = (uint32_t)width * (uint32_t)height;
    uint32_t uv_size = (uint32_t)width * (uint32_t)((height + 1) / 2);
    uint32_t data_size = y_size + uv_size;

    VABufferID buf_id;
    VAStatus st = xvmc_CreateBuffer(ctx, 0, VAImageBufferType, 1, data_size, NULL, &buf_id);
    if (st != VA_STATUS_SUCCESS)
        return st;

    VAImage *img = &drv->images[slot].image;
    memset(img, 0, sizeof(*img));
    img->image_id = (VAImageID)slot;
    img->format = *format;
    img->buf = buf_id;
    img->width = (uint16_t)width;
    img->height = (uint16_t)height;
    img->data_size = data_size;
    img->num_planes = 2;
    img->pitches[0] = (uint32_t)width;
    img->pitches[1] = (uint32_t)width;
    img->offsets[0] = 0;
    img->offsets[1] = y_size;
    drv->images[slot].in_use = 1;

    *image = *img;
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_DeriveImage(
    VADriverContextP ctx, VASurfaceID surface, VAImage *image)
{
    XVMC_SERIALIZE();
    (void)ctx;
    (void)surface;
    (void)image;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus xvmc_DestroyImage(VADriverContextP ctx, VAImageID image)
{
    XVMC_SERIALIZE();
    struct xvmc_driver_data *drv = ctx->pDriverData;
    if (image >= XVMC_MAX_IMAGES || !drv->images[image].in_use)
        return VA_STATUS_ERROR_INVALID_IMAGE;
    xvmc_DestroyBuffer(ctx, drv->images[image].image.buf);
    memset(&drv->images[image], 0, sizeof(drv->images[image]));
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_SetImagePalette(
    VADriverContextP ctx, VAImageID image, unsigned char *palette)
{
    XVMC_SERIALIZE();
    (void)ctx;
    (void)image;
    (void)palette;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/*
 * Real image-transfer path: the only way this driver can pull pixel
 * data out of an opaque XvMC surface, since XvMC itself has no direct
 * "read the decoded surface" API -- reuses the same PutSurface+
 * GetImage round trip xvmc_backend_snapshot_surface already verified
 * pixel-accurate, converting directly to NV12 (the one format
 * xvmc_QueryImageFormats/xvmc_CreateImage support) in one pass
 * (xvmc_backend_get_surface_nv12) rather than through an intermediate
 * RGB buffer -- confirmed by real profiling to be a real, avoidable
 * extra full-image pass. This is what lets real callers like ffmpeg's
 * vaapi hwaccel frame-transfer path (vaapi_transfer_data_from, which
 * falls back to CreateImage+GetImage once vaDeriveImage reports
 * unimplemented) actually get decoded frames back, instead of failing
 * outright as before.
 */
static VAStatus xvmc_GetImage(
    VADriverContextP ctx, VASurfaceID surface,
    int x, int y, unsigned int width, unsigned int height, VAImageID image)
{
    XVMC_SERIALIZE();
    struct xvmc_driver_data *drv = ctx->pDriverData;

    if (image >= XVMC_MAX_IMAGES || !drv->images[image].in_use)
        return VA_STATUS_ERROR_INVALID_IMAGE;
    if ((int)surface < 0 || (int)surface >= XVMC_BACKEND_MAX_SURFACES ||
        !drv->backend.surface_in_use[surface])
        return VA_STATUS_ERROR_INVALID_SURFACE;
    /* This driver only ever transfers the whole frame -- matches every
     * real caller seen so far (ffmpeg always requests the full decoded
     * picture), and xvmc_backend_get_surface_nv12 has no cropping. Note
     * width/height need NOT match drv->backend.width/height (the actual
     * local decode resolution) -- xvmc_backend_get_surface_nv12 scales
     * to whatever's requested here via real XvMC hardware scaling (see
     * its own comment), which is what lets an H.264 profile whose native
     * resolution differs from XVMC_RELAY_RESOLUTION still read back a
     * correctly-sized image instead of failing here. */
    if (x != 0 || y != 0)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    VAImage *img = &drv->images[image].image;
    if (img->format.fourcc != VA_FOURCC_NV12)
        return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
    /* width/height (the caller's vaGetImage args) must still match the
     * VAImage's own declared size -- that's a real invariant (the
     * output buffer below is sized for img->width/height), just no
     * longer tied to the backend's actual decode resolution. */
    if ((int)width != img->width || (int)height != img->height)
        return VA_STATUS_ERROR_INVALID_PARAMETER;

    if (XvMCSyncSurface(drv->backend.dpy, &drv->backend.surfaces[surface]) != Success)
        return VA_STATUS_ERROR_OPERATION_FAILED;

    uint8_t *buf_data = (uint8_t *)drv->buffers[img->buf].data;
    uint8_t *y_out = buf_data + img->offsets[0];
    uint8_t *uv_out = buf_data + img->offsets[1];

    /* Debug tooling: real profiling found the mandatory PutSurface+
     * XShmGetImage+colorspace-conversion round trip (see
     * xvmc_backend_get_surface_nv12) accounting for ~84% of this whole
     * pipeline's real CPU cost -- this GMA950's XvMC was only ever
     * designed for "decode straight to the screen," never "hand the
     * pixels back," and there's no way to make that round trip itself
     * cheap. XVMC_FAKE_GETIMAGE skips the expensive readback+conversion
     * and fills a fixed solid-color placeholder for the *caller*
     * instead -- but still does a REAL XvMCPutSurface to the scratch
     * window first (xvmc_backend_display_only), so the actual decoded
     * video displays correctly via genuine hardware overlay the whole
     * time, orchestrated by the real caller's (e.g. ffmpeg's) own
     * demux/decode timing. This "tricks" a caller like ffmpeg into
     * paying only the cheap display cost while still driving real
     * playback -- ffmpeg's own downstream consumer of the (fake) pixel
     * data (e.g. its xv output window) will show the placeholder, not
     * real content; only the scratch window shows the real video. Not
     * a real feature -- would make transcoding/filtering/
     * screenshotting via vaGetImage produce nonsense if left on by
     * default. */
    if (getenv("XVMC_FAKE_GETIMAGE")) {
        xvmc_backend_display_only(&drv->backend, (int)surface);
        /* Cycle luma by frame count instead of a fixed color -- still
         * O(1) per frame (one value computed once, then memset), so no
         * real CPU cost added, but a static color and a genuinely
         * "frozen" bug look identical from the outside; a value that
         * visibly progresses with real playback proves this path is
         * still being driven once per real frame, not stuck. */
        static unsigned int fake_frame_count = 0;
        uint8_t luma = (uint8_t)(80 + (fake_frame_count++ % 128));
        /* Sized for img->width/height -- the actual output buffer's
         * dimensions (see xvmc_CreateImage) -- not drv->backend.width/
         * height, which can now legitimately be smaller (H.264 profile,
         * XVMC_RELAY_RESOLUTION). Using the backend's size here would
         * underfill (or, if backend were ever larger, overflow) img's
         * real buffer. */
        int y_size = img->width * img->height;
        int uv_size = img->width * ((img->height + 1) / 2);
        memset(y_out, luma, (size_t)y_size);
        for (int i = 0; i < uv_size; i += 2) {
            uv_out[i] = 220;     /* Cb: strong blue */
            uv_out[i + 1] = 80;  /* Cr */
        }
        return VA_STATUS_SUCCESS;
    }

    if (xvmc_backend_get_surface_nv12(&drv->backend, (int)surface, y_out, uv_out,
                                       img->width, img->height) != 0)
        return VA_STATUS_ERROR_OPERATION_FAILED;
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_PutImage(
    VADriverContextP ctx, VASurfaceID surface, VAImageID image,
    int src_x, int src_y, unsigned int src_width, unsigned int src_height,
    int dest_x, int dest_y, unsigned int dest_width, unsigned int dest_height)
{
    XVMC_SERIALIZE();
    (void)ctx;
    (void)surface;
    (void)image;
    (void)src_x;
    (void)src_y;
    (void)src_width;
    (void)src_height;
    (void)dest_x;
    (void)dest_y;
    (void)dest_width;
    (void)dest_height;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus xvmc_QuerySubpictureFormats(
    VADriverContextP ctx, VAImageFormat *format_list, unsigned int *flags,
    unsigned int *num_formats)
{
    XVMC_SERIALIZE();
    (void)ctx;
    (void)format_list;
    (void)flags;
    *num_formats = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_CreateSubpicture(
    VADriverContextP ctx, VAImageID image, VASubpictureID *subpicture)
{
    XVMC_SERIALIZE();
    (void)ctx;
    (void)image;
    (void)subpicture;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus xvmc_DestroySubpicture(
    VADriverContextP ctx, VASubpictureID subpicture)
{
    XVMC_SERIALIZE();
    (void)ctx;
    (void)subpicture;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus xvmc_SetSubpictureImage(
    VADriverContextP ctx, VASubpictureID subpicture, VAImageID image)
{
    XVMC_SERIALIZE();
    (void)ctx;
    (void)subpicture;
    (void)image;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus xvmc_SetSubpictureChromakey(
    VADriverContextP ctx, VASubpictureID subpicture,
    unsigned int chromakey_min, unsigned int chromakey_max,
    unsigned int chromakey_mask)
{
    XVMC_SERIALIZE();
    (void)ctx;
    (void)subpicture;
    (void)chromakey_min;
    (void)chromakey_max;
    (void)chromakey_mask;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus xvmc_SetSubpictureGlobalAlpha(
    VADriverContextP ctx, VASubpictureID subpicture, float global_alpha)
{
    XVMC_SERIALIZE();
    (void)ctx;
    (void)subpicture;
    (void)global_alpha;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus xvmc_AssociateSubpicture(
    VADriverContextP ctx, VASubpictureID subpicture,
    VASurfaceID *target_surfaces, int num_surfaces,
    short src_x, short src_y, unsigned short src_width, unsigned short src_height,
    short dest_x, short dest_y, unsigned short dest_width, unsigned short dest_height,
    unsigned int flags)
{
    XVMC_SERIALIZE();
    (void)ctx;
    (void)subpicture;
    (void)target_surfaces;
    (void)num_surfaces;
    (void)src_x;
    (void)src_y;
    (void)src_width;
    (void)src_height;
    (void)dest_x;
    (void)dest_y;
    (void)dest_width;
    (void)dest_height;
    (void)flags;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus xvmc_DeassociateSubpicture(
    VADriverContextP ctx, VASubpictureID subpicture,
    VASurfaceID *target_surfaces, int num_surfaces)
{
    XVMC_SERIALIZE();
    (void)ctx;
    (void)subpicture;
    (void)target_surfaces;
    (void)num_surfaces;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus xvmc_QueryDisplayAttributes(
    VADriverContextP ctx, VADisplayAttribute *attr_list, int *num_attributes)
{
    XVMC_SERIALIZE();
    (void)ctx;
    (void)attr_list;
    *num_attributes = 0;
    return VA_STATUS_SUCCESS;
}

static VAStatus xvmc_GetDisplayAttributes(
    VADriverContextP ctx, VADisplayAttribute *attr_list, int num_attributes)
{
    XVMC_SERIALIZE();
    (void)ctx;
    (void)attr_list;
    (void)num_attributes;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus xvmc_SetDisplayAttributes(
    VADriverContextP ctx, VADisplayAttribute *attr_list, int num_attributes)
{
    XVMC_SERIALIZE();
    (void)ctx;
    (void)attr_list;
    (void)num_attributes;
    return VA_STATUS_ERROR_UNIMPLEMENTED;
}

VAStatus VA_DRIVER_INIT_ENTRY(VADriverContextP ctx)
{
    struct xvmc_driver_data *drv_data = calloc(1, sizeof(*drv_data));
    if (!drv_data)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    drv_data->relay_last_decoded_surface = -1; /* 0 is a valid surface index, so calloc's 0 can't mean "none" */

    if (xvmc_backend_open(&drv_data->backend, (Display *)ctx->native_dpy, ctx->x11_screen) != 0) {
        /* No XvMC-capable port on this X server/hardware -- fail vaInitialize
         * cleanly so the app's own hwaccel fallback takes over (Phase 4),
         * rather than pretending this driver works. */
        free(drv_data);
        return VA_STATUS_ERROR_OPERATION_FAILED;
    }

    /* XVMC_RELAY_HOST/XVMC_RELAY_PORT: where relay-server's push mode is
     * listening, for the H.264 profile path (see xvmc_relay_end_picture).
     * Read once here rather than per-connection since real clients create
     * one context per session. Defaults match relay-server's own
     * documented default listen address. */
    const char *relay_host_env = getenv("XVMC_RELAY_HOST");
    drv_data->relay_host = strdup(relay_host_env ? relay_host_env : "127.0.0.1");
    if (!drv_data->relay_host) {
        xvmc_backend_destroy_context(&drv_data->backend);
        free(drv_data);
        return VA_STATUS_ERROR_ALLOCATION_FAILED;
    }
    const char *relay_port_env = getenv("XVMC_RELAY_PORT");
    drv_data->relay_port = (unsigned short)(relay_port_env ? atoi(relay_port_env) : 9100);

    /* XVMC_RELAY_RESOLUTION: must match relay-server's own --resolution/
     * RELAY_RESOLUTION for the H.264 profile path (see
     * last_config_profile's comment above for why). Same "WxH" format
     * and same default as relay-server's own default. Malformed values
     * (no 'x', non-numeric, zero) silently fall back to the default
     * rather than failing vaInitialize outright -- this only affects
     * the H.264 path, and a real MPEG-2-only session should never be
     * blocked by a misconfigured relay setting it'll never use. */
    drv_data->relay_resolution_width = 640;
    drv_data->relay_resolution_height = 480;
    const char *relay_res_env = getenv("XVMC_RELAY_RESOLUTION");
    if (relay_res_env) {
        int w = 0, h = 0;
        if (sscanf(relay_res_env, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            drv_data->relay_resolution_width = w;
            drv_data->relay_resolution_height = h;
        } else {
            fprintf(stderr, "xvmc_drv_video: XVMC_RELAY_RESOLUTION=\"%s\" is not "
                             "valid \"WxH\", falling back to 640x480\n", relay_res_env);
        }
    }

    ctx->version_major = VA_MAJOR_VERSION;
    ctx->version_minor = VA_MINOR_VERSION;
    ctx->max_profiles = 5;
    ctx->max_entrypoints = 1;
    ctx->max_attributes = 1;
    /* libva's vaInitialize treats these as capacity hints and fails
     * ("Failed to define max_image_formats/max_subpic_formats in init")
     * if they're 0, even though this driver genuinely has zero formats to
     * report via vaQueryImageFormats/vaQuerySubpictureFormats. */
    ctx->max_image_formats = 1;
    ctx->max_subpic_formats = 1;
    ctx->max_display_attributes = 1;
    ctx->str_vendor = "GMA950 network-relay XvMC driver (local MPEG-2 + H.264-via-relay)";
    ctx->pDriverData = drv_data;

    struct VADriverVTable *vtable = ctx->vtable;
    vtable->vaTerminate = xvmc_Terminate;
    vtable->vaQueryConfigProfiles = xvmc_QueryConfigProfiles;
    vtable->vaQueryConfigEntrypoints = xvmc_QueryConfigEntrypoints;
    vtable->vaGetConfigAttributes = xvmc_GetConfigAttributes;
    vtable->vaCreateConfig = xvmc_CreateConfig;
    vtable->vaDestroyConfig = xvmc_DestroyConfig;
    vtable->vaQueryConfigAttributes = xvmc_QueryConfigAttributes;
    vtable->vaCreateSurfaces = xvmc_CreateSurfaces;
    vtable->vaDestroySurfaces = xvmc_DestroySurfaces;
    vtable->vaCreateContext = xvmc_CreateContext;
    vtable->vaDestroyContext = xvmc_DestroyContext;
    vtable->vaCreateBuffer = xvmc_CreateBuffer;
    vtable->vaBufferSetNumElements = xvmc_BufferSetNumElements;
    vtable->vaMapBuffer = xvmc_MapBuffer;
    vtable->vaUnmapBuffer = xvmc_UnmapBuffer;
    vtable->vaDestroyBuffer = xvmc_DestroyBuffer;
    vtable->vaBeginPicture = xvmc_BeginPicture;
    vtable->vaRenderPicture = xvmc_RenderPicture;
    vtable->vaEndPicture = xvmc_EndPicture;
    vtable->vaSyncSurface = xvmc_SyncSurface;
    vtable->vaPutSurface = xvmc_PutSurface;
    vtable->vaQuerySurfaceStatus = xvmc_QuerySurfaceStatus;
    vtable->vaQuerySurfaceAttributes = xvmc_QuerySurfaceAttributes;
    vtable->vaCreateSurfaces2 = xvmc_CreateSurfaces2;
    vtable->vaQueryImageFormats = xvmc_QueryImageFormats;
    vtable->vaCreateImage = xvmc_CreateImage;
    vtable->vaDeriveImage = xvmc_DeriveImage;
    vtable->vaDestroyImage = xvmc_DestroyImage;
    vtable->vaSetImagePalette = xvmc_SetImagePalette;
    vtable->vaGetImage = xvmc_GetImage;
    vtable->vaPutImage = xvmc_PutImage;
    vtable->vaQuerySubpictureFormats = xvmc_QuerySubpictureFormats;
    vtable->vaCreateSubpicture = xvmc_CreateSubpicture;
    vtable->vaDestroySubpicture = xvmc_DestroySubpicture;
    vtable->vaSetSubpictureImage = xvmc_SetSubpictureImage;
    vtable->vaSetSubpictureChromakey = xvmc_SetSubpictureChromakey;
    vtable->vaSetSubpictureGlobalAlpha = xvmc_SetSubpictureGlobalAlpha;
    vtable->vaAssociateSubpicture = xvmc_AssociateSubpicture;
    vtable->vaDeassociateSubpicture = xvmc_DeassociateSubpicture;
    vtable->vaQueryDisplayAttributes = xvmc_QueryDisplayAttributes;
    vtable->vaGetDisplayAttributes = xvmc_GetDisplayAttributes;
    vtable->vaSetDisplayAttributes = xvmc_SetDisplayAttributes;

    return VA_STATUS_SUCCESS;
}
