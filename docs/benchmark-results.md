# Benchmark results

Real measurements on the real target hardware — an Acer Aspire One netbook,
Intel Atom N270 @ 1.60GHz (Bonnell, single physical core / 2 threads,
MMX/SSE/SSE2/SSE3/SSSE3, no SSE4.1+), 1.4GiB RAM, real Intel GMA950 (i915/945)
XvMC hardware. Built with `-O3 -march=native -mtune=native` for this CPU, as
the driver always is (never copy a prebuilt `.so` between machines — see the
main README).

Test content: `adam_ruins_mid_640x480.m2v`, a real broadcast-derived MPEG-2
capture, mixed I/P pictures, 640×480, 23.976fps, ~10s (239 pictures per this
driver's own picture parser; ffmpeg's demuxer reports 240 frames for the same
file — a one-frame difference in how the two count pictures at the very
start/end of the stream, not a decode discrepancy).

## This driver: real XvMC decode+render, isolated

`test/local_mpeg2_decode_only_test` feeds the file through this driver's real
`vaBeginPicture`/`vaRenderPicture`/`vaEndPicture` path — this driver's own
software entropy-decode (VLD) and IDCT, plus the real hardware
`XvMCRenderSurface` call — with **zero image transfer** (no `vaGetImage`, no
`vaPutSurface`), isolating true decode+render cost from the readback/display
cost a real player would add on top.

Five consecutive runs, same file, same build:

| Run | decode+render | avg/picture |
|---|---|---|
| 1 | 2241.5ms | 9.38ms |
| 2 | 2234.2ms | 9.35ms |
| 3 | 2224.3ms | 9.31ms |
| 4 | 2229.3ms | 9.33ms |
| 5 | 2227.6ms | 9.32ms |

Tight variance (9.31–9.38ms/picture, <1%). **~9.34ms/picture average** →
theoretical max sustained rate of ~107 pictures/sec for decode+render alone,
i.e. this path has roughly **4.5× the headroom needed** for this content's
real 23.976fps playback rate on a single Atom N270 core.

## Plain software decode baseline, same file, same box

`ffmpeg -threads 1 -i adam_ruins_mid_640x480.m2v -f null -` (ffmpeg's own
built-in MPEG-2 software decoder, single-threaded to match this driver's
single-core reality): **1.576s wall** for 240 frames (includes ffmpeg's own
process startup/demuxing overhead, unlike the isolated test above), steady
state ~198fps per ffmpeg's own reporting → **~5.05ms/frame** at steady state.

## XvMC hybrid vs. plain software

**9.34ms/picture (XvMC hybrid) vs. ~5.05ms/frame (plain software steady
state) → this driver's XvMC path runs roughly 85% slower per picture than
plain software decode on this hardware**, consistent with this project's
earlier, broader investigation across the full 640×480–720×576 resolution
range, which found the XvMC hybrid path **consistently 40–70% slower**
depending on resolution (see the main README's "Real hardware constraints"
section) — today's single-resolution measurement lands at the higher end of
that historical range.

This isn't a regression to fix: the real hardware motion-compensation block
still does real work per picture, but this driver's own software
entropy-decode/IDCT (unavoidable — XvMC on this generation only accelerates
motion compensation, not entropy decode) dominates the cost, and the *real*
value of the XvMC path was never raw speed — it's being the only decode path
this chip has for `LIBVA_DRIVER_NAME`-selected playback at all, while freeing
the CPU from the display blit (see `xvmc_backend_put_surface`'s
hardware-scaled blit).

## Reproducing this

```bash
# on the target machine, from a fresh checkout of vaapi-xvmc-driver/
make test/local_mpeg2_decode_only_test
DISPLAY=:0 LIBVA_DRIVER_NAME=xvmc \
  ./test/local_mpeg2_decode_only_test <file>.m2v ./xvmc_drv_video.so
```

Needs a real XvMC-capable X server (`vaDriverInit` fails cleanly, by design,
without one — see the smoke test). For the plain-software comparison:
`ffmpeg -threads 1 -i <file>.m2v -f null -`.

## relay-server: downscaling costs nothing extra — it's faster

The driver's 720×576 hard ceiling (above) means any source larger than that
can only ever reach this hardware via `relay-server`, which transcodes (and,
for a large source, downscales) it into MPEG-2 this driver can decode. The
natural worry is that adding a downscale step piles more work onto an already
software-bound transcode. Measured for real: it does the opposite — a
smaller *output* frame is cheaper to encode, more than offsetting the
identical decode cost of the same large input, so downscaling is a net win,
not "acceptable overhead."

**Host:** `nj-server2.local` CT 114 — a real, currently-deployed
`relay-server` container from this project's Proxmox/Nvidia-passthrough work
(2 vCPU, 1GB RAM; not necessarily representative of every deployment target,
but a real one, not a synthetic benchmark box).

**Source:** synthetic 1920×1080, 25fps, 30s (750 frames) H.264, generated
with the same `testsrc`/`libx264` pattern `deploy/proxmox/container-setup.sh`
already uses for its own demo clip.

**Command:** relay-server's own pull-mode ffmpeg invocation, verbatim (see
`relay-server/src/main.rs`'s `serve_pull_connection`), with `-re` dropped —
`-re` throttles ffmpeg to the source's real-time framerate, which would mask
the actual compute-cost difference being measured here:
```
ffmpeg -i /tmp/bench_1080p.mp4 -c:v mpeg2video -q:v 4 -s <WxH> -f mpegts pipe:1 > /dev/null
```

| Config | `-s` | Wall time (3 runs) | Steady-state fps (verbose run) | speed |
|---|---|---|---|---|
| No real downscale | `1920x1080` | 12.11s / 10.77s / 11.31s (avg 11.4s) | ~69 fps | 2.74x |
| Downscale to driver's target | `640x480` | 8.37s / 6.67s / 7.01s (avg 7.35s) | ~97 fps | 3.88x |

Same 750-frame source, same box, same encoder settings — only the output
size differs. Downscaling to 640×480 ran **~35% faster** (7.35s vs 11.4s
avg), not slower: decode cost is identical either way (same 1080p input),
but `mpeg2video`'s encode cost scales with output pixel count, so encoding a
640×480 frame is substantially cheaper than encoding a 1920×1080 one. At
either setting relay-server here comfortably outpaces real-time (25fps
source, ≥69fps steady-state output) with large margin to spare.

**Conclusion:** the real benefit of routing a large source through
`relay-server` isn't just that it's the only way to reach this driver at all
above 720×576 — the resulting downscale is actively cheaper than a
same-resolution transcode would be, not a cost you pay for the privilege.

Reproduce: generate a large source the same way as above, then time both
`-s` values with `-re` dropped, same box, back to back.
