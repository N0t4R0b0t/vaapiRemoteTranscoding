#!/usr/bin/env bash
# deploy/proxmox/create-lxc.sh — Proxmox HOST side: ensure a Debian template
# exists and create the LXC (+ optional Nvidia GPU device passthrough).
# Sourced by install.sh (uses the CT_*/GPU_* variables it sets) but also
# runnable standalone for debugging.
#
# Requires: pct, pveam (run on a Proxmox VE node as root).

set -euo pipefail

# --- ensure a Debian base template is present --------------------------------
# Prints the template volid (e.g. "local:vztmpl/debian-12-standard_...tar.zst").
# Dynamic, not a hardcoded version: confirmed by real testing that a
# hardcoded template filename goes stale (Proxmox mirrors move to newer
# point releases and drop old ones) -- this always resolves to whatever
# is actually available right now, same idea as any real Debian point
# release upgrade.
ensure_debian_template() {
  local storage="${TEMPLATE_STORAGE:-local}"
  pveam update >/dev/null 2>&1 || true

  local existing
  existing="$(pveam list "$storage" 2>/dev/null | awk '/debian-12-standard/ {print $1}' | sort -V | tail -n1)"
  if [ -n "$existing" ]; then
    printf '%s\n' "$existing"
    return 0
  fi

  local avail
  avail="$(pveam available --section system 2>/dev/null | awk '/debian-12-standard/ {print $2}' | sort -V | tail -n1)"
  [ -n "$avail" ] || { echo "no debian-12-standard template available via pveam" >&2; return 1; }
  pveam download "$storage" "$avail" >&2
  printf '%s\n' "${storage}:vztmpl/${avail}"
}

# --- GPU passthrough preflight (host-side checks, before touching anything) -
# GPU_PASSTHROUGH=nvidia passes the host's Nvidia GPU device nodes through
# into the CT, so relay-server's ffmpeg can use NVDEC for
# hardware-accelerated *decode* of the incoming H.264 source (encode to
# MPEG-2 always stays software -- NVENC has never supported MPEG-2
# output; see relay-server/src/main.rs's CommonArgs::hwaccel_args
# comment). Requires:
#   - The Nvidia driver already installed and working ON THE HOST
#     (`nvidia-smi` succeeds there) -- this CT shares the host kernel
#     (LXC, not a VM), so the kernel module must already be loaded by
#     the host; this script only passes through the resulting device
#     nodes, it doesn't install any driver on the host itself.
#   - NVIDIA_DRIVER_VERSION set to that exact host driver version
#     (`nvidia-smi` on the host shows it) -- container-setup.sh uses it
#     to install a matching userspace driver inside the CT.
# Verified end-to-end on real Proxmox VE 9 + Nvidia Quadro P400
# hardware (2026-07-18): unprivileged containers work fine here (the
# real device nodes are world-accessible, crw-rw-rw-, so an
# unprivileged CT's UID/GID remapping never gets in the way -- no need
# for a privileged container just for this). Device majors are NOT
# fixed/portable constants -- confirmed nvidia-uvm's major varies by
# kernel/driver combo (it requests a dynamic major if a fixed one isn't
# available) -- so passthrough_nvidia_devices queries the live host
# instead of hardcoding a "conventional" number that may simply be
# wrong on a different kernel.
preflight_gpu_passthrough() {
  if [ -z "${NVIDIA_DRIVER_VERSION:-}" ]; then
    echo "GPU_PASSTHROUGH=nvidia requires NVIDIA_DRIVER_VERSION (check nvidia-smi on this host)." >&2
    return 1
  fi
  if ! command -v nvidia-smi >/dev/null 2>&1 || ! nvidia-smi >/dev/null 2>&1; then
    echo "nvidia-smi isn't working on this Proxmox host -- install/fix the Nvidia" >&2
    echo "driver on the HOST first (this script only passes device nodes through," >&2
    echo "it doesn't install a host-side driver)." >&2
    return 1
  fi
  if ! ls /dev/nvidia0 /dev/nvidiactl /dev/nvidia-uvm >/dev/null 2>&1; then
    echo "Expected Nvidia device nodes (/dev/nvidia0, /dev/nvidiactl, /dev/nvidia-uvm)" >&2
    echo "not found on this host despite nvidia-smi working -- unexpected, check manually." >&2
    return 1
  fi
}

# Appends cgroup allow + mount entries for the CT's raw config. `optional`
# on every mount.entry means a missing individual node (e.g. no
# nvidia-modeset on a headless GPU) doesn't fail the whole container start.
passthrough_nvidia_devices() {
  local ctid="$1"
  echo "==> Passing Nvidia GPU device nodes through into CT $ctid" >&2
  local conf="/etc/pve/lxc/${ctid}.conf"
  {
        for dev in /dev/nvidia0 /dev/nvidiactl /dev/nvidia-uvm /dev/nvidia-uvm-tools /dev/dri/renderD128; do
            [ -e "$dev" ] || continue
            local major_hex major_dec
            major_hex="$(stat -c '%t' "$dev")"
            major_dec="$((16#$major_hex))"
            echo "lxc.cgroup2.devices.allow: c ${major_dec}:* rwm"
        done
        echo "lxc.mount.entry: /dev/nvidia0 dev/nvidia0 none bind,optional,create=file"
        echo "lxc.mount.entry: /dev/nvidiactl dev/nvidiactl none bind,optional,create=file"
        echo "lxc.mount.entry: /dev/nvidia-uvm dev/nvidia-uvm none bind,optional,create=file"
        echo "lxc.mount.entry: /dev/nvidia-uvm-tools dev/nvidia-uvm-tools none bind,optional,create=file"
        echo "lxc.mount.entry: /dev/nvidia-modeset dev/nvidia-modeset none bind,optional,create=file"
        echo "lxc.mount.entry: /dev/dri/renderD128 dev/dri/renderD128 none bind,optional,create=file"
        if [ -d /dev/nvidia-caps ]; then
            echo "lxc.mount.entry: /dev/nvidia-caps/ dev/nvidia-caps/ none bind,optional,create=dir"
        fi
  } >> "$conf"
}

# --- create + start the container -------------------------------------------
# Expects: CTID, CT_HOSTNAME, CT_CORES, CT_RAM, CT_DISK, CT_STORAGE, CT_BRIDGE,
#          CT_UNPRIVILEGED, CT_TAG, and TEMPLATE_VOLID (from ensure_debian_template).
create_container() {
  local template="${TEMPLATE_VOLID:?TEMPLATE_VOLID not set}"

  pct create "$CTID" "$template" \
    --hostname     "$CT_HOSTNAME" \
    --unprivileged "$CT_UNPRIVILEGED" \
    --features     nesting=1,keyctl=1 \
    --cores        "$CT_CORES" \
    --memory       "$CT_RAM" \
    --rootfs       "${CT_STORAGE}:${CT_DISK}" \
    --net0         "name=eth0,bridge=${CT_BRIDGE},ip=${CT_IP:-dhcp}" \
    --tags         "$CT_TAG" \
    --onboot       1

  if [ "${GPU_PASSTHROUGH:-}" = "nvidia" ]; then
    passthrough_nvidia_devices "$CTID"
  fi

  pct start "$CTID"

  # Wait for the container's network to come up before we exec into it.
  local tries=0
  until pct exec "$CTID" -- getent hosts deb.debian.org >/dev/null 2>&1; do
    tries=$((tries + 1))
    [ "$tries" -ge 30 ] && { echo "container network did not come up" >&2; return 1; }
    sleep 2
  done
}
