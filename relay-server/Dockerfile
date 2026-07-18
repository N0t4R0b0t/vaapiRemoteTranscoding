# Multi-stage build: compile relay-server in a full Rust image, build a
# CUDA/NVDEC-capable ffmpeg in a separate stage, then run both in a slim
# image with just their runtime needs.
#
# Debian's stock `ffmpeg` package (bookworm) has no CUDA/NVDEC/NVENC
# support at all -- Debian main doesn't ship the proprietary bits, and
# hwaccel support needs nv-codec-headers (small, just the header
# definitions for NVIDIA's NVDEC/CUVID/NVENC APIs -- NOT the full CUDA
# toolkit, since this pipeline only wraps the video codec hardware
# blocks, never does general CUDA compute) at build time. This is only
# useful with RELAY_HWACCEL=nvdec (see CommonArgs::hwaccel_args in
# main.rs) AND a real Nvidia GPU passed through into this container
# (see deploy/proxmox/create-lxc.sh's GPU_PASSTHROUGH option) -- the
# same binary works fine for the plain-software default too, it just
# never touches the hwaccel path unless asked to.
#
# UNTESTED as deployment infrastructure: unlike the driver/backend work
# in this repo, which is verified against the real target GMA950
# hardware, nobody has run this specific Dockerfile against a real
# Proxmox+Nvidia host yet. Validate on real hardware before relying on
# it -- see deploy/proxmox/create-lxc.sh's own caveats.

FROM rust:1-slim-bookworm AS build
WORKDIR /build
COPY Cargo.toml Cargo.lock ./
COPY src ./src
RUN cargo build --release

FROM debian:bookworm-slim AS ffmpeg-build
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential yasm pkg-config git ca-certificates wget xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Headers-only (no proprietary binaries, no CUDA toolkit) -- defines the
# NVDEC/CUVID/NVENC API structs ffmpeg's configure script looks for.
RUN git clone --depth 1 https://github.com/FFmpeg/nv-codec-headers.git /tmp/nv-codec-headers \
    && make -C /tmp/nv-codec-headers install \
    && rm -rf /tmp/nv-codec-headers

# A specific stable release, not a moving branch -- reproducible builds
# matter more here than always having the newest ffmpeg.
ARG FFMPEG_VERSION=6.1.1
RUN wget -q "https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz" \
    && tar xf "ffmpeg-${FFMPEG_VERSION}.tar.xz" \
    && rm "ffmpeg-${FFMPEG_VERSION}.tar.xz"
WORKDIR /ffmpeg-${FFMPEG_VERSION}
# Deliberately minimal: this pipeline only ever needs H.264 decode (the
# real source codec), mpeg2video encode (this driver's whole reason for
# existing -- see the repo README), the container formats both relay
# modes use, and the cuvid/nvdec/nvenc hwaccel wrappers -- not a
# general-purpose ffmpeg build. Both the H.264 decoder and mpeg2video
# encoder are ffmpeg's own native implementations (no external GPL
# library like libx264 needed), matching how plain `apt-get install
# ffmpeg` already worked before this change.
#
# --enable-parser=h264: --disable-everything disables parsers too, and
# they aren't auto-pulled in as decoder deps. Without it the h264 demuxer
# can't find real frame/NAL boundaries in the raw Annex-B stream --
# confirmed by real testing this caused decode corruption
# (decode_slice_header errors, frame-num mismatches, missing reference
# pictures) even against known-good input, on both software and
# -hwaccel cuda decode paths.
RUN ./configure \
        --disable-everything \
        --enable-nonfree \
        --enable-cuvid --enable-nvdec --enable-nvenc \
        --enable-decoder=h264 \
        --enable-encoder=mpeg2video \
        --enable-demuxer=h264,mpegts,mpegvideo \
        --enable-muxer=mpegts,mpeg2video \
        --enable-protocol=pipe,file \
        --enable-filter=hwdownload,format,scale \
        --enable-parser=h264 \
        --disable-doc --disable-debug \
    && make -j"$(nproc)" \
    && make install

FROM debian:bookworm-slim
# libnvidia-encode/libnvidia-decode themselves come from the Nvidia
# driver passed through at runtime (see create-lxc.sh/install.sh) --
# not installed here. This image only needs ffmpeg's own shared-library
# dependencies, which --disable-everything above already minimized.
COPY --from=ffmpeg-build /usr/local/bin/ffmpeg /usr/local/bin/ffmpeg
COPY --from=build /build/target/release/relay-server /usr/local/bin/relay-server

# Defaults match the README's Usage table; override at `docker run` time.
ENV RELAY_LISTEN=0.0.0.0:9100
ENV RELAY_RESOLUTION=640x480
ENV RELAY_QUALITY=4
ENV RELAY_HWACCEL=none

EXPOSE 9100
ENTRYPOINT ["/usr/local/bin/relay-server"]
# `pull` (fixed RELAY_SOURCE, no driver involved) is the default here and
# is what deploy/proxmox/install.sh relies on implicitly (it doesn't pass
# a mode override) -- it's meant for standalone transcode-pipeline
# testing. `push` is what the driver actually uses for real H.264
# playback (see the README); override with `docker run ... push` for
# that.
CMD ["pull"]
