#!/usr/bin/env bash
# deploy/proxmox/container-setup.sh — runs INSIDE the container (as root).
# Installs Docker (+ Nvidia userspace driver/toolkit if GPU_PASSTHROUGH is
# set), builds the relay-server image, and (re)starts it as a
# restart-always container.
#
# Assumes this repo is already present at $REPO_ROOT (install.sh pushes or
# clones it before invoking this). Idempotent: safe to re-run.

set -euo pipefail

REPO_ROOT="${REPO_ROOT:-/opt/vaapiRemoteTranscoding}"
APP_DIR=/opt/relay-server
GPU_PASSTHROUGH="${GPU_PASSTHROUGH:-}"
# Must exactly match the Proxmox host's own installed driver version --
# this CT shares the host's kernel (LXC, not a VM), so the kernel module
# is already loaded by the host; this only installs matching userspace
# libraries. Find the host's version with `nvidia-smi` (top-right
# "Driver Version") or `cat /proc/driver/nvidia/version` on the host.
NVIDIA_DRIVER_VERSION="${NVIDIA_DRIVER_VERSION:-}"

log()  { printf '==> %s\n' "$*"; }
warn() { printf '!!  %s\n' "$*" >&2; }

log "Installing Docker + git"
apt-get update
# ffmpeg here is only for this script's own one-time demo-clip generation
# below (needs libx264/aac/mp4, a full feature set) -- deliberately
# separate from relay-server's own Docker image, whose ffmpeg is a
# minimal, purpose-built one (h264 decode + mpeg2video encode + optional
# nvenc/nvdec, see relay-server/Dockerfile's own comment) that doesn't
# have those encoders at all. gnupg: confirmed by real testing that the
# base Debian 12 LXC template does NOT include it -- needed below for
# the nvidia-container-toolkit apt repo's signing key, even when
# GPU_PASSTHROUGH isn't used (cheap to always have, simpler than
# conditionally installing it later).
apt-get install -y --no-install-recommends ca-certificates curl git docker.io ffmpeg gnupg
systemctl enable --now docker

if [ "$GPU_PASSTHROUGH" = "nvidia" ]; then
    if [ -z "$NVIDIA_DRIVER_VERSION" ]; then
        echo "GPU_PASSTHROUGH=nvidia requires NVIDIA_DRIVER_VERSION set to the" >&2
        echo "Proxmox host's own driver version (check with nvidia-smi on the host)." >&2
        exit 1
    fi

    # Idempotent: confirmed by real testing that re-running this whole
    # section unconditionally on every `install.sh update` re-downloads
    # the ~400MB driver installer every time AND hits a real gpg
    # failure on a second run in a non-interactive exec context (see
    # below) -- skip entirely once nvidia-smi already reports the
    # exact version we'd install anyway.
    current_version="$(nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null || true)"
    if [ "$current_version" = "$NVIDIA_DRIVER_VERSION" ]; then
        log "Nvidia userspace driver $NVIDIA_DRIVER_VERSION already installed, skipping"
    else
        log "Installing matching Nvidia userspace driver (version $NVIDIA_DRIVER_VERSION)"
        # --no-kernel-module: the kernel module is the HOST's (this CT
        # shares its kernel, being LXC not a VM) -- installing a second one
        # in here would conflict with it. Only the matching userspace
        # libraries (libcuda, libnvidia-encode, libnvidia-decode, etc.) are
        # actually needed for anything running in this container to talk to
        # the device nodes create-lxc.sh already passed through.
        RUNFILE="NVIDIA-Linux-x86_64-${NVIDIA_DRIVER_VERSION}.run"
        # --http1.1: confirmed by real testing this ~400MB download reliably
        # fails partway through ("HTTP/2 stream 1 was not closed cleanly")
        # over HTTP/2 in this environment; forcing HTTP/1.1 avoided it
        # every time in the same real test. --retry as defense in depth for
        # any other transient failure.
        curl --http1.1 --retry 3 --retry-delay 2 -fSL -o "/tmp/$RUNFILE" \
            "https://us.download.nvidia.com/XFree86/Linux-x86_64/${NVIDIA_DRIVER_VERSION}/${RUNFILE}"
        # --silent alone is sufficient: it implies non-interactive AND
        # license acceptance in every version of this installer. Deliberately
        # not adding other flags (e.g. --no-questions, --accept-license) seen
        # in some online guides -- unconfirmed whether they're real options
        # on the current installer, and an unrecognized flag would fail the
        # whole install; --no-kernel-module alone is well-established.
        sh "/tmp/$RUNFILE" --no-kernel-module --silent
        rm -f "/tmp/$RUNFILE"
    fi

    if [ -x /usr/bin/nvidia-ctk ] || [ -x /usr/local/bin/nvidia-ctk ]; then
        log "nvidia-container-toolkit already installed, skipping"
    else
        log "Installing nvidia-container-toolkit"
        # --batch --yes --pinentry-mode loopback: confirmed by real
        # testing that plain `gpg --dearmor` fails with "cannot open
        # '/dev/tty': No such device or address" when this script runs
        # in a non-interactive exec context (e.g. `pct exec` without a
        # real pty attached, which is how automated/update runs and
        # some remote-orchestration tools invoke this) -- gpg tries to
        # open a tty for a status prompt regardless of there being
        # nothing to actually confirm for a plain --dearmor. These
        # flags force it to skip that entirely.
        curl -fsSL https://nvidia.github.io/libnvidia-container/gpgkey \
            | gpg --batch --yes --pinentry-mode loopback --dearmor -o /usr/share/keyrings/nvidia-container-toolkit-keyring.gpg
        curl -fsSL https://nvidia.github.io/libnvidia-container/stable/deb/nvidia-container-toolkit.list \
            | sed 's#deb https://#deb [signed-by=/usr/share/keyrings/nvidia-container-toolkit-keyring.gpg] https://#g' \
            > /etc/apt/sources.list.d/nvidia-container-toolkit.list
        apt-get update
        apt-get install -y nvidia-container-toolkit
        nvidia-ctk runtime configure --runtime=docker
        # Confirmed by real testing: nvidia-container-toolkit's default
        # cgroup-device-rule management (via eBPF, bpf_prog_query) fails
        # with "operation not permitted" from inside a nested container
        # (Docker running inside this LXC CT) -- it isn't allowed to touch
        # cgroup device filters at that nesting depth. The standard, known
        # fix: disable the toolkit's own cgroup management and rely on the
        # lxc.cgroup2.devices.allow rules create-lxc.sh already set up at
        # the outer (CT-vs-host) level instead.
        sed -i 's/#no-cgroups = false/no-cgroups = true/' /etc/nvidia-container-runtime/config.toml
        systemctl restart docker
    fi

    log "Verifying the passed-through GPU is visible in this CT"
    nvidia-smi || {
        echo "nvidia-smi failed -- GPU passthrough isn't working correctly." >&2
        echo "Check create-lxc.sh's device passthrough config in /etc/pve/lxc/<CTID>.conf" >&2
        echo "on the Proxmox host, and that NVIDIA_DRIVER_VERSION matches the host exactly." >&2
        exit 1
    }

    log "Verifying Docker itself can see the GPU (docker run --gpus all)"
    docker run --rm --gpus all nvidia/cuda:12.4.0-base-ubuntu22.04 nvidia-smi || {
        echo "docker run --gpus all failed -- nvidia-container-toolkit isn't working" >&2
        echo "correctly even though host-level nvidia-smi succeeded above." >&2
        exit 1
    }
fi

log "Building relay-server:local image"
docker build -t relay-server:local "$REPO_ROOT/relay-server"

mkdir -p "$APP_DIR/data"

if [ ! -f "$APP_DIR/data/source.mp4" ]; then
    log "No source clip present, generating a test pattern (replace this with real footage later)"
    # Runs on the CT's own full-featured ffmpeg (installed above), not
    # relay-server:local's minimal one -- that image's ffmpeg has no
    # libx264/aac at all, only what its own transcode work needs.
    ffmpeg -loglevel warning \
        -f lavfi -i testsrc=size=640x480:rate=25:duration=60 \
        -f lavfi -i sine=frequency=440:duration=60 \
        -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest \
        "$APP_DIR/data/source.mp4"
fi

if [ ! -f "$APP_DIR/env" ]; then
    cat > "$APP_DIR/env" <<EOF
RELAY_LISTEN=0.0.0.0:9100
RELAY_RESOLUTION=640x480
RELAY_QUALITY=4
RELAY_SOURCE=/data/source.mp4
EOF
fi

# Idempotent update of just this one line, independent of whether the
# env file already existed -- so re-running this script after changing
# GPU_PASSTHROUGH updates the running behavior without wiping any other
# customization (e.g. a real RELAY_SOURCE) already in that file.
sed -i '/^RELAY_HWACCEL=/d' "$APP_DIR/env"
if [ "$GPU_PASSTHROUGH" = "nvidia" ]; then
    echo "RELAY_HWACCEL=nvdec" >> "$APP_DIR/env"
else
    echo "RELAY_HWACCEL=none" >> "$APP_DIR/env"
fi

log "(Re)starting relay-server container"
docker rm -f relay-server >/dev/null 2>&1 || true
GPU_DOCKER_ARGS=()
if [ "$GPU_PASSTHROUGH" = "nvidia" ]; then
    GPU_DOCKER_ARGS=(--gpus all)
fi
docker run -d \
    --name relay-server \
    --restart unless-stopped \
    -p 9100:9100 \
    -v "$APP_DIR/data:/data" \
    --env-file "$APP_DIR/env" \
    "${GPU_DOCKER_ARGS[@]}" \
    relay-server:local

log "relay-server container running:"
docker ps --filter name=relay-server
