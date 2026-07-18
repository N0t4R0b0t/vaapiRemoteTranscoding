# VA-API Network Relay Driver — Project Plan
### Transparent hardware-decode-alike VA-API driver for Acer Aspire One (GMA 950 / 945GSE)

## Goal
Build a VA-API driver (`libva` backend `.so`) that any standard app (mpv, ffmpeg,
Chromium) can select via `LIBVA_DRIVER_NAME`, which transparently:
1. Forwards incoming compressed H.264 bitstream to a remote transcode server
2. Server decodes H.264 and re-encodes to MPEG-2
3. Driver receives the compressed MPEG-2 stream back over the network
4. Driver decodes locally via XvMC (the only real hardware video path GMA 950 has)
5. Hands the app a normal `VASurfaceID`, indistinguishable from real hw decode

## Why this approach (context/decisions already made)
- GMA 950 (945GSE) has **no** VA-API-capable decode hardware — only an MPEG-2
  motion-comp block, exposed via the old XvMC API. This is a hard
  hardware limit, not a driver gap — confirmed against `vainfo` showing nothing.
  (**Correction, Phase 0**: originally assumed iDCT was also in hardware;
  confirmed by direct port probing that it isn't — this chip generation is
  MoComp-only, so the driver does IDCT in software too. See Phase 0/2.5 below.)
- H.264 hardware decode cannot be bolted onto XvMC — different transform (DCT
  vs H.264 integer transform), different motion-comp precision (half-pel
  bilinear vs quarter-pel 6-tap) — the silicon physically can't do it.
- A Broadcom Crystal HD card is the only true local hw-H.264 option, but
  requires sourcing discontinued hardware — deprioritized in favor of network
  offload, which is more flexible and already validated works well.
- Shipping **raw decoded YUV** over the network (a naive "remote VA-API")
  was rejected — ~90x more bandwidth than shipping a compressed bitstream.
  Compressed-stream handoff (H.264 in → MPEG-2 out) is the efficient design.
- Practical resolution ceiling is untested but expected around 480–576p;
  720p likely to stutter since XvMC only offloads iDCT/MC, not entropy
  (VLC) decode, which scales with pixel count and stays 100% CPU-bound.
  This is the same gap nVidia ION was built to solve for Atom netbooks.
- Transcode server: R520 (Proxmox, plenty of headroom) is the primary
  candidate; a Pi 4/3B+ (hardware H.264 decode via V4L2 M2M) is a viable
  lighter-weight alternative; Pi 5 has **no** hw decode block, not worth it;
  avoid sharing the existing Home Assistant Pi 5 to prevent resource
  contention with HA.

## Phased plan (roughly in build/test order)

### Phase 0 — Validate assumptions (before writing any driver code)
- [x] Confirm XvMC still works standalone on current archlinux32 install.
      It was bit-rotted, but not for a codec/driver reason: the `intel` DDX
      driver ships with XvMC disabled by default. Fixed by adding
      `Option "XvMC" "true"` to `/etc/X11/xorg.conf.d/20-intel.conf` and
      creating `/etc/X11/XvMCConfig` pointing at `libIntelXvMC.so` — neither
      existed. After that, `XVideo-MotionCompensation` shows up as a real
      X11 extension and `XvMCListSurfaceTypes` finds a real port.
      **Correction to the plan**: every port on this hardware enumerates as
      MoComp-only (probed all 16 ports directly) — there is no IDCT-capable
      XvMC surface type on 915/945-generation chips at all, contradicting
      this doc's original "iDCT + motion-comp block" framing above. The
      driver has to do entropy decode *and* IDCT in software, handing the
      MoComp hardware already-spatial-domain blocks (see Phase 2.5).
- [ ] Benchmark real resolution ceiling: transcode test clips at 640x480,
      720x540, 960x540, 1280x720 from the R520, play each via
      `mplayer -vo xvmc -benchmark`, record dropped frames. This sets the
      realistic target resolution for the pipeline rather than guessing.

### Phase 1 — Network transcode relay (no libva involved yet)
- [ ] Plain client/server: server (R520 LXC) takes an H.264 source URL,
      transcodes to MPEG-2, serves over MPEG-TS (HTTP or UDP).
- [ ] Netbook-side plain socket client that pulls the MPEG-2 stream and
      pipes it to `mplayer -vo xvmc -` to confirm the pipe works end-to-end.
- [ ] Wrap as systemd service on the R520 side; test cold-start latency.
- [ ] Decide TCP vs UDP MPEG-TS depending on observed wifi reliability.
- [ ] mDNS/Avahi autodiscovery for the relay service (straightforward, low risk).

### Phase 2 — Stub VA-API driver skeleton (testable on any x86_64 box, no netbook needed)
- [ ] Study reference implementations: `libva-v4l2-request` (jernejsk, compact
      non-Intel/AMD example) and Mesa's Gallium VA frontend
      (`src/gallium/frontends/va`) for how to map VA calls to an arbitrary backend.
- [ ] Implement driver ABI entry points (`vaDriverInit`, config/context/surface
      creation, `vaBeginPicture`/`vaRenderPicture`/`vaEndPicture`/`vaSyncSurface`)
      with a **fake decode** (solid color / test pattern surface).
- [ ] Validate against `vainfo` and `ffmpeg -hwaccel vaapi` on any x86_64 Linux
      box using `LIBVA_DRIVER_NAME` + `LIBVA_DRIVERS_PATH`.
- [ ] Handle bitstream reconstitution: apps may hand slice/param buffers as
      Annex-B (start codes) or avcC (length-prefixed) — normalize both into a
      continuous stream ffmpeg can ingest on the server side.

### Phase 2.5 — Real MPEG-2 decode via XvMC, standalone (no network yet)
Decouples the two hard problems in this project: whether the driver can
actually drive XvMC hardware, versus whether the H.264-relay/network
plumbing works. This phase only touches the former, and is directly
testable on the real GMA950 with a local MPEG-2 file — no relay-server,
no network, no H.264 involved. It's also not optional groundwork Phase 3
can skip: XvMC-decode-via-VA-API *is* the last leg of the full pipeline
("decode via XvMC, hand back a VASurfaceID"), so this phase builds and
verifies that leg in isolation before Phase 3 adds the network/H.264 side
on top of it.
- [x] Wire the driver's config/context/surface/buffer lifecycle to real
      XvMC calls (`XvMCCreateContext`/`CreateSurface`/`CreateMacroBlocks`/
      `CreateBlocks`, port discovery via `XvQueryAdaptors` +
      `XvMCListSurfaceTypes`) instead of the Phase 2 fake-decode stubs.
      Done in `xvmc_backend.[ch]`; compiles/links against real libva 1.23 +
      libXvMC headers, and `test/smoke_test.c` confirms the driver loads,
      resolves its entry point, and fails cleanly (not a crash) when no
      XvMC port exists -- but this has only run against a non-XvMC X
      server (WSLg); port discovery and the actual XvMC calls are
      unverified against real GMA950 hardware.
- [x] **Correction to the original plan**: the driver must advertise and
      implement `VAEntrypointVLD` for MPEG-2, not `VAEntrypointIDCT`/
      `VAEntrypointMoComp`. Real client software (ffmpeg/mpv) only ever
      drives VLD for MPEG-2 -- it hands the driver raw compressed slice
      bits and expects full entropy decode. The IDCT/MoComp entrypoints
      VA-API also defines (and which map almost 1:1 onto XvMC's
      `XvMCMacroBlock`/`XvMCBlockArray` structs) are vestigial: nothing
      modern produces the pre-decoded macroblock buffers they'd need.
      This means the driver has to embed its own MPEG-2 software entropy
      decoder feeding XvMC's hardware IDCT+MC, not just repackage
      already-parsed buffers.
- [x] **The actual hard part**: `mpeg2_vld_decode_slice()` in
      `mpeg2_vld.[ch]` is real, not a stub. MPEG-2 entropy decode
      (macroblock_type, motion vectors, coded_block_pattern, DCT run/level)
      ported from FFmpeg's `libavcodec/mpeg12dec.c` + `mpeg12data.c`/etc.
      (LGPL v2.1+), verified table-by-table byte-exact against fetched
      FFmpeg source rather than hand-rolled. Since Phase 0 found this
      hardware is MoComp-only (not IDCT-capable as originally assumed), a
      software IDCT stage was also added, ported from `libmpeg2` (**GPL
      v2** — a materially different, stronger copyleft obligation than the
      FFmpeg LGPL code already here; the repo owner needs to decide how to
      handle this before any distribution, not resolved here — see the
      provenance comment block in `mpeg2_vld.c`).

      Both I-frame and P-frame decode are verified correct against real
      per-macroblock ground truth, not just macroblock counts: built a
      purpose-instrumented reference FFmpeg (cloned upstream, minimal
      `./configure --disable-everything --enable-decoder=mpeg2video ...`
      build, with bit-position tracing added directly into
      `mpeg_decode_mb`/`mpeg2_decode_block_non_intra`) to get authoritative
      per-macroblock/per-block consumed-bit-counts to diff against. That
      process caught and fixed a real bug: MPEG-2's non-intra DCT block
      decode has a special 2-bit first-coefficient shortcut ("1" + sign =
      run=0, level=±1) that is bit-identical to the general table's
      End-of-Block code and disambiguated only by position (first code
      read vs. subsequent) -- missing it caused any block whose true first
      coefficient was exactly ±1 to be misread as empty.
- [x] Validated with `vainfo` and `ffmpeg -hwaccel vaapi -hwaccel_device :0
      -i test.ts -f null -` on the real Aspire One (the explicit
      `-hwaccel_device :0` matters -- ffmpeg defaults to a DRM `VADisplay`,
      which this X11-only driver can't use; without it, init fails with a
      driver-side segfault that looks like a driver bug but isn't one).
      Confirmed exactly 1200/1200 macroblocks (correct for 640x480) on the
      frames processed. **Update:** the `correction data buffer overflow`/
      `intel_batchbuffer.c` assert described below as unresolved was a
      resource-exhaustion issue from calling the debug snapshot path
      unconditionally on every picture, including ones where nothing
      decoded -- fixed by gating it on `mb_count > 0` (see
      `maybe_snapshot_surface` in `xvmc_drv_video.c`). Full multi-frame
      playback via this local path is confirmed working on real hardware
      (see Phase 3, done).

### Phase 3 — Wire in the network + H.264 relay path (done)
- [x] Handle bitstream reconstitution: apps may hand H.264 slice/param
      buffers as Annex-B (start codes) or avcC (length-prefixed) —
      normalize both into a continuous stream ffmpeg can ingest on the
      server side. Done in `h264_reconstitute.[ch]`.
- [x] Replace the app's H.264 buffers with: forward reconstituted
      bitstream to relay server (`relay_client.[ch]`, now wired into
      `xvmc_relay_end_picture` in `xvmc_drv_video.c`), receive MPEG-2
      back, feed it through the already-working Phase 2.5 XvMC decode
      path to populate the real `VASurfaceID`. Verified end-to-end on
      real hardware. Known gap: the local XvMC context is created at the
      app's native H.264 resolution, not relay-server's transcoded
      output resolution -- see README's "Real hardware constraints"
      section.
- [ ] Build internal frame buffering / a small ring of decoded surfaces so
      `vaSyncSurface` mostly returns from local buffer rather than blocking on
      a fresh network round trip per frame — this is the core latency-hiding
      logic and the most novel engineering left once Phase 2.5's XvMC path
      and its MPEG-2 decode are working.
- [ ] This phase is where most debugging time will go — expect segfaults
      surfacing inside the calling app's process (mpv/ffmpeg), not your own
      binary; plan on `gdb --args mpv --hwdec=vaapi ...` sessions.

### Phase 4 — Fallback / robustness
- [ ] **Init-time**: quick connect/health-check to relay server in
      `vaInitialize`/`vaCreateConfig`; fail fast with a clean VA_STATUS error
      if unreachable, so the app's own existing hwaccel fallback (mpv
      `--hwdec=vaapi,auto`, ffmpeg auto-drop-to-software) takes over — don't
      duplicate logic that already exists one layer up.
- [ ] **Mid-stream** (stretch goal, not v1): circuit-breaker pattern — after
      N consecutive failures/timeouts talking to the relay within a session,
      stop retrying network and drop to a local software decode path
      (would require embedding libavcodec — roughly doubles driver scope,
      treat as optional).

### Phase 5 — Port to the Atom / real-world test
- [ ] Cross-compile or build natively on the Aspire One (slow, single core —
      expect long link times).
- [ ] Real-world playback test: YouTube (via `yt-dlp`/`streamlink` feeding
      the relay) or other live H.264 sources, `LIBVA_DRIVER_NAME` selecting
      the new driver, confirm mpv/ffmpeg pick it up with zero extra config.
- [ ] Since the driver only activates when explicitly selected via
      `LIBVA_DRIVER_NAME`, this phase carries low risk to normal netbook use.

## Open decisions to make in Claude Code
- Relay server target: R520 LXC vs a dedicated Pi 4/3B+ (hw H.264 decode,
  lower power, but one more physical box).
- ~~Push vs pull architecture for the relay~~ — both modes shipped.
  Push (the driver streams its live H.264 bitstream to relay-server) is
  what real playback actually uses (see Phase 3, done); pull (a fixed
  `RELAY_SOURCE`, one ffmpeg process per client connection) was kept as
  a standalone test harness that doesn't need the driver at all.
- Target resolution/bitrate defaults once Phase 0 benchmarking is done.
- ~~Repo structure~~ — decided: single repo, two components —
  `relay-server/` (Rust, ffmpeg wrapper + systemd unit) and
  `vaapi-xvmc-driver/` (C, the `.so`). Driver is C rather than Rust because
  the VADriverVTable population is C-ABI-shaped regardless of language, and
  C sidesteps the panic-across-FFI hazard a Rust entry point would need
  `catch_unwind` for. Development moves to a Linux machine with libva-dev/
  libXvMC-dev/a Rust toolchain before either component can actually be built.
