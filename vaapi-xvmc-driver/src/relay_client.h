#ifndef RELAY_CLIENT_H
#define RELAY_CLIENT_H

#include <stddef.h>
#include <sys/types.h>

/*
 * Talks to relay-server's push mode over a single TCP connection:
 * write() sends H.264 bitstream bytes, read() receives MPEG-2/MPEG-TS
 * bytes back. Connection setup only -- the driver owns framing/threading
 * around this (Phase 3: forwarding bitstream + the surface ring buffer).
 */

struct relay_conn {
    int fd;
};

/* host may be a hostname (including "*.local" mDNS names -- resolved via
 * the normal NSS resolver, no mDNS code needed here) or a literal IP. */
int relay_connect(struct relay_conn *conn, const char *host, unsigned short port);

/* Both block; caller is expected to run these from its own I/O thread(s). */
ssize_t relay_send(struct relay_conn *conn, const void *buf, size_t len);
ssize_t relay_recv(struct relay_conn *conn, void *buf, size_t len);

/*
 * Non-blocking readiness check (poll() with a caller-supplied timeout in
 * milliseconds; 0 means "check and return immediately"). Returns 1 if a
 * subsequent relay_recv() would return data without blocking, 0 if the
 * timeout elapsed with nothing available, -1 on error. Exists so a
 * caller can drain whatever has arrived so far without blocking its own
 * forward progress -- see xvmc_drv_video.c's H.264 relay path, which
 * must keep sending pictures continuously rather than stall waiting for
 * each one's response (relay-server's ffmpeg needs several pictures'
 * worth of data before its own stream analysis will let it produce any
 * output at all, so blocking per-picture deadlocks).
 */
int relay_poll_readable(struct relay_conn *conn, int timeout_ms);

void relay_close(struct relay_conn *conn);

#endif
