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
