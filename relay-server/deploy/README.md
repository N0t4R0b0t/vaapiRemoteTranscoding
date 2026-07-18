# Deploying relay-server

1. Build on the target (or cross-compile): `cargo build --release`, binary at
   `target/release/relay-server`. Copy to `/usr/local/bin/relay-server`.
2. `mkdir -p /etc/relay-server`, copy `systemd/env.example` to
   `/etc/relay-server/env` and fill in values.
3. Copy `systemd/relay-server-pull.service` (Phase 1 test) and/or
   `systemd/relay-server-push.service` (Phase 3 real mode) to
   `/etc/systemd/system/`, then `systemctl daemon-reload && systemctl enable
   --now relay-server-pull` (or `-push`).
4. Copy `avahi/relay-server.service` to `/etc/avahi/services/` so the
   netbook can find the relay as `<hostname>.local:9100` instead of a
   hardcoded IP. Requires avahi-daemon running on the relay host and
   nss-mdns on the netbook (both standard on Arch with avahi installed).
