#!/usr/bin/env bash
# deploy/proxmox/install.sh — Proxmox VE helper (run on a PVE node as root):
#
#     bash -c "$(curl -fsSL https://raw.githubusercontent.com/N0t4R0b0t/vaapiRemoteTranscoding/main/deploy/proxmox/install.sh)"
#
# Provisions -- or UPDATES -- a Debian LXC running relay-server (the
# H.264-to-MPEG-2 transcode server this driver's H.264 path talks to; see
# the repo README) as a restart-always Docker container.
#
# One entrypoint, two modes (same convention as this user's mtune-mirror
# project's own installer):
#   * no relay-server container yet -> fresh install (create CT, Docker,
#                                       build + run the image)
#   * a relay-server container exists -> update in place (push refreshed
#                                         code, rebuild, restart --
#                                         RELAY_SOURCE/data are preserved)
# Force a mode with `install.sh install` / `install.sh update`, or MODE=env.
#
# GPU_PASSTHROUGH=nvidia NVIDIA_DRIVER_VERSION=<host's exact version>
# additionally passes the host's Nvidia GPU through for hardware-accelerated
# *decode* only (see create-lxc.sh's own comment on this -- NVENC has never
# supported MPEG-2 encode, so that side always stays software regardless).

set -euo pipefail

# --- defaults (override via env) --------------------------------------------
REPO_URL="${REPO_URL:-https://github.com/N0t4R0b0t/vaapiRemoteTranscoding.git}"
REPO_REF="${REPO_REF:-main}"

CT_HOSTNAME="${CT_HOSTNAME:-relay-server}"
CT_CORES="${CT_CORES:-2}"
CT_RAM="${RAM:-1024}"             # MB
CT_DISK="${DISK_GB:-4}"           # GB
CT_STORAGE="${STORAGE:-local-lvm}"
TEMPLATE_STORAGE="${TEMPLATE_STORAGE:-local}"
CT_BRIDGE="${BRIDGE:-vmbr0}"
CT_IP="${CT_IP:-dhcp}"
# Unprivileged even with GPU passthrough -- confirmed working on real
# Proxmox VE 9 + Nvidia hardware (2026-07-18): the real Nvidia device
# nodes are world-accessible (crw-rw-rw-), so an unprivileged CT's own
# UID/GID remapping doesn't get in the way. Override if a specific host
# needs privileged for some other reason.
CT_UNPRIVILEGED="${CT_UNPRIVILEGED:-1}"
CT_TAG="vaapi-relay-server"        # identifies our container for update detection

GPU_PASSTHROUGH="${GPU_PASSTHROUGH:-}"
NVIDIA_DRIVER_VERSION="${NVIDIA_DRIVER_VERSION:-}"

MODE="${MODE:-${1:-auto}}"        # auto | install | update

msg()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m ok\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31merr\033[0m %s\n' "$*" >&2; exit 1; }
trap 'fail "aborted at line $LINENO"' ERR

command -v pct   >/dev/null || fail "pct not found — run this on a Proxmox VE node"
command -v pveam >/dev/null || fail "pveam not found — run this on a Proxmox VE node"

if [ "$GPU_PASSTHROUGH" = "nvidia" ] && [ -z "$NVIDIA_DRIVER_VERSION" ]; then
  fail "GPU_PASSTHROUGH=nvidia requires NVIDIA_DRIVER_VERSION (check nvidia-smi on this host)"
fi

# --- deploy helpers (shared by install & update) ----------------------------
# Push the full relay-server + deploy tree (git metadata not needed inside
# the CT at all -- this streams a plain tar over `pct exec`, no git clone
# inside the container, no separate scp/pct-push round trip either).
deploy_repo() {
  local ctid="$1"
  pct exec "$ctid" -- mkdir -p /opt/vaapiRemoteTranscoding
  tar -C "$HOST_REPO" -cf - relay-server deploy \
    | pct exec "$ctid" -- tar -C /opt/vaapiRemoteTranscoding -xf -
}
run_setup() {
  pct exec "$1" -- env \
    REPO_ROOT=/opt/vaapiRemoteTranscoding \
    GPU_PASSTHROUGH="$GPU_PASSTHROUGH" \
    NVIDIA_DRIVER_VERSION="$NVIDIA_DRIVER_VERSION" \
    bash /opt/vaapiRemoteTranscoding/deploy/proxmox/container-setup.sh
}

# Find an existing relay-server container by its tag; echoes CTID or nothing.
find_existing_ct() {
  local id
  for id in $(pct list | awk 'NR>1 {print $1}'); do
    if pct config "$id" 2>/dev/null | grep -qE "^tags:.*\b${CT_TAG}\b"; then
      printf '%s\n' "$id"; return 0
    fi
  done
  return 1
}

# --- obtain this repo on the host (needed for both modes) -------------------
if [ -n "${REPO_SRC:-}" ]; then
  [ -f "$REPO_SRC/deploy/proxmox/install.sh" ] || fail "REPO_SRC=$REPO_SRC is not a checkout of this repo"
  HOST_REPO="$REPO_SRC"
  msg "Using local repo source: $HOST_REPO"
else
  HOST_REPO="$(mktemp -d)"
  trap 'rm -rf "$HOST_REPO"' EXIT
  msg "Cloning $REPO_URL ($REPO_REF) to host"
  git clone --depth 1 --branch "$REPO_REF" "$REPO_URL" "$HOST_REPO" 2>/dev/null \
    || fail "clone failed — set REPO_URL/REPO_REF, or pass REPO_SRC=<local checkout>"
fi
# shellcheck source=deploy/proxmox/create-lxc.sh
source "$HOST_REPO/deploy/proxmox/create-lxc.sh"

if [ "$GPU_PASSTHROUGH" = "nvidia" ]; then
  preflight_gpu_passthrough || exit 1
fi

# --- resolve mode -----------------------------------------------------------
EXISTING="$(find_existing_ct || true)"
if [ "$MODE" = "auto" ]; then
  [ -n "$EXISTING" ] && MODE="update" || MODE="install"
fi

# ============================================================================
# UPDATE
# ============================================================================
if [ "$MODE" = "update" ]; then
  [ -n "$EXISTING" ] || fail "update requested but no container tagged '$CT_TAG' found"
  CTID="$EXISTING"
  msg "Updating relay-server container $CTID (data/env preserved)"
  deploy_repo "$CTID"
  run_setup "$CTID"
  ok "Update complete for container $CTID"
  exit 0
fi

# ============================================================================
# FRESH INSTALL
# ============================================================================
[ -z "$EXISTING" ] || fail "a relay-server container ($EXISTING) already exists — run 'install.sh update'"

CTID="${CTID:-$(pvesh get /cluster/nextid 2>/dev/null || echo 900)}"
export CTID CT_HOSTNAME CT_CORES CT_RAM CT_DISK CT_STORAGE CT_BRIDGE CT_IP \
       CT_UNPRIVILEGED CT_TAG TEMPLATE_STORAGE GPU_PASSTHROUGH NVIDIA_DRIVER_VERSION

msg "Ensuring Debian base template is present"
TEMPLATE_VOLID="$(ensure_debian_template)"; export TEMPLATE_VOLID
ok "template: $TEMPLATE_VOLID"

msg "Creating LXC $CTID ($CT_HOSTNAME): ${CT_CORES}c / ${CT_RAM}MB / ${CT_DISK}G$( [ "$GPU_PASSTHROUGH" = "nvidia" ] && echo " + Nvidia passthrough" )"
create_container
ok "container $CTID up"

msg "Pushing relay-server + deploy into container at /opt/vaapiRemoteTranscoding"
deploy_repo "$CTID"

msg "Running container setup (Docker, image build, container start)"
run_setup "$CTID"

CT_IP_ADDR="$(pct exec "$CTID" -- hostname -I 2>/dev/null | awk '{print $1}' || true)"
ok "Done. relay-server should be listening on ${CT_IP_ADDR:-<container-ip>}:9100"
if [ "$GPU_PASSTHROUGH" = "nvidia" ]; then
  echo "    GPU passthrough was requested -- verify it's actually working:"
  echo "      pct exec $CTID -- nvidia-smi"
fi
echo "    Replace the bundled test clip by editing /opt/relay-server/data/source.mp4"
echo "    (or set RELAY_SOURCE in /opt/relay-server/env and re-run) inside CT $CTID."
echo ""
echo "Update later with:  install.sh update   (from the PVE host, or just re-run install.sh)"
