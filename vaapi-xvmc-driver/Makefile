# Builds only on Linux with libva-dev and libXvMC-dev installed.
# XvMC historically ships without a pkg-config file, so it's linked directly.

CC       ?= cc
PKGCONF  ?= pkg-config
# -march/-mtune=native: real profiling this session found this driver's
# own software MPEG-2 entropy decode/IDCT to be a dominant real cost on
# the netbook's Atom N270 (Bonnell microarchitecture -- confirmed via
# `gcc -march=native -E -v`, which detects it and enables its real
# instruction set: MMX/SSE/SSE2/SSE3/SSSE3, no SSE4). Always rebuild ON
# whichever machine will run the result (this project's established
# workflow throughout -- source is copied and rebuilt remotely, binaries
# never copied across machines) since -march=native is evaluated at
# build time on whatever CPU actually runs `make`, and a Bonnell binary
# built for a different CPU could use instructions Bonnell doesn't have.
CFLAGS   += -std=gnu11 -Wall -Wextra -fPIC -O3 -march=native -mtune=native $(shell $(PKGCONF) --cflags libva)
LDLIBS   += $(shell $(PKGCONF) --libs libva) -lXvMC -lXvMCW -lXv -lXext -lX11

TARGET := xvmc_drv_video.so
SOURCES := src/xvmc_drv_video.c src/relay_client.c src/xvmc_backend.c src/mpeg2_vld.c src/mpeg2_headers.c src/h264_reconstitute.c
HEADERS := src/relay_client.h src/xvmc_backend.h src/mpeg2_vld.h src/mpeg2_headers.h src/h264_reconstitute.h
SMOKE_TEST := test/smoke_test
RENDER_STRESS_TEST := test/render_stress_test
RELAY_PIXEL_TEST := test/relay_pixel_test
RELAY_PIXEL_TEST_PURE := test/relay_pixel_test_pure
RECONSTRUCT_FRAME := test/reconstruct_frame
RELAY_DISPLAY_TEST := test/relay_display_test
LOCAL_MPEG2_DECODE_ONLY_TEST := test/local_mpeg2_decode_only_test
LOCAL_MPEG2_PLAYER := test/local_mpeg2_player

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -shared -o $@ $(SOURCES) $(LDLIBS)

$(SMOKE_TEST): test/smoke_test.c
	$(CC) $(CFLAGS) -o $@ $< $(shell $(PKGCONF) --libs libva) -ldl -lX11

$(RENDER_STRESS_TEST): test/render_stress_test.c
	$(CC) $(CFLAGS) -o $@ $< $(shell $(PKGCONF) --libs libva) -ldl -lX11

# Not part of `test`: needs a running relay-server push instance and
# XVMC_RELAY_HOST/XVMC_RELAY_PORT/XVMC_SNAPSHOT_DIR env vars, unlike
# smoke_test/render_stress_test which are fully self-contained. Build
# and run manually to visually verify decoded pixels from the relay
# path -- see the file comment for why ffmpeg's CLI can't be used here.
$(RELAY_PIXEL_TEST): test/relay_pixel_test.c test/relay_pixel_test_slices_long.h
	$(CC) $(CFLAGS) -o $@ $< $(shell $(PKGCONF) --libs libva) -ldl -lX11

# Same as RELAY_PIXEL_TEST but drives the relay path with synthetic,
# controlled content instead of real broadcast capture -- see the file
# comment in relay_pixel_test_pure.c.
$(RELAY_PIXEL_TEST_PURE): test/relay_pixel_test_pure.c test/relay_pixel_test_slices_pure.h
	$(CC) $(CFLAGS) -o $@ $< $(shell $(PKGCONF) --libs libva) -ldl -lX11

# Standalone diagnostic: decodes a raw MPEG-2 elementary stream's first
# (intra) picture using this driver's own mpeg2_headers.c/mpeg2_vld.c
# code and writes a full PPM -- entirely in software, no XvMC/X11/
# hardware involved. Used to isolate whether a pixel-level bug (e.g.
# the magenta/pink tint investigation) is in this driver's own decode
# or introduced later by the real XvMC hardware render/display path.
# Usage: ./test/reconstruct_frame <path.m2v> [out.ppm]
$(RECONSTRUCT_FRAME): test/reconstruct_frame.c src/mpeg2_headers.c src/mpeg2_vld.c
	$(CC) $(CFLAGS) -Isrc -o $@ test/reconstruct_frame.c src/mpeg2_headers.c src/mpeg2_vld.c $(shell $(PKGCONF) --libs libva) -lXvMC -lXvMCW

# Not part of `test`: needs a running relay-server push instance and a
# real X11 display -- see the file comment. Uses the driver's new
# vaPutSurface implementation to display live into a real, visible
# window, the same call path a real media player (mpv/ffplay) uses.
$(RELAY_DISPLAY_TEST): test/relay_display_test.c test/relay_pixel_test_slices_long.h
	$(CC) $(CFLAGS) -o $@ $< $(shell $(PKGCONF) --libs libva) -ldl -lX11

# Isolates true decode+render cost (no image transfer at all) against
# a real local .m2v file, to compare fairly against plain software
# decode without the artificial vaGetImage readback cost every
# ffmpeg-based benchmark in this project's performance investigation
# included. Usage: ./local_mpeg2_decode_only_test file.m2v [driver.so]
$(LOCAL_MPEG2_DECODE_ONLY_TEST): test/local_mpeg2_decode_only_test.c src/mpeg2_headers.c
	$(CC) $(CFLAGS) -Isrc -o $@ test/local_mpeg2_decode_only_test.c src/mpeg2_headers.c $(shell $(PKGCONF) --libs libva) -ldl -lX11

# A genuine minimal "native XvMC player" -- decodes a real local .m2v
# file and displays it via vaPutSurface directly (zero-copy, no image
# transfer at all), the same path a real 2005-2008 XvMC-aware media
# player used. Usage: ./local_mpeg2_player file.m2v [fps] [driver.so]
$(LOCAL_MPEG2_PLAYER): test/local_mpeg2_player.c src/mpeg2_headers.c
	$(CC) $(CFLAGS) -Isrc -o $@ test/local_mpeg2_player.c src/mpeg2_headers.c $(shell $(PKGCONF) --libs libva) -ldl -lX11

# LD_PRELOAD shim: many real apps (mpv, Chromium, etc.) hardcode
# vaGetDisplayDRM against a DRM render node for hwaccel, with no
# equivalent to ffmpeg CLI's `-hwaccel_device :0` X11 override -- this
# driver is X11-only by design (see PLAN.md), so those apps never reach
# it at all otherwise. Redirects vaGetDisplayDRM to the real X11
# vaGetDisplay($DISPLAY) instead; VADisplay is opaque to the caller, so
# everything past that point is unaffected. Verified working end-to-end
# with ffmpeg's own -hwaccel_device pointed at a real DRM path (both
# local MPEG-2 and H.264-via-relay) -- see README's app-compatibility
# notes for what's confirmed vs. still open (mpv's H.264 hwdec path
# specifically). Usage: LD_PRELOAD=./tools/vaapi_x11_shim.so <app> ...
VAAPI_X11_SHIM := tools/vaapi_x11_shim.so
$(VAAPI_X11_SHIM): tools/vaapi_x11_shim.c
	$(CC) -shared -fPIC -O2 -o $@ $< $(shell $(PKGCONF) --cflags --libs libva) -lX11

test: $(TARGET) $(SMOKE_TEST) $(RENDER_STRESS_TEST)
	./$(SMOKE_TEST) ./$(TARGET)
	./$(RENDER_STRESS_TEST) ./$(TARGET) 60

clean:
	rm -f $(TARGET) $(SMOKE_TEST) $(RENDER_STRESS_TEST) $(RELAY_PIXEL_TEST) $(RELAY_PIXEL_TEST_PURE) $(RECONSTRUCT_FRAME) $(RELAY_DISPLAY_TEST) $(LOCAL_MPEG2_DECODE_ONLY_TEST) $(LOCAL_MPEG2_PLAYER) $(VAAPI_X11_SHIM)
