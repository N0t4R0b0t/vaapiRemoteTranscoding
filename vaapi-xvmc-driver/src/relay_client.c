#include "relay_client.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int relay_connect(struct relay_conn *conn, const char *host, unsigned short port)
{
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *result = NULL;
    int rc = getaddrinfo(host, port_str, &hints, &result);
    if (rc != 0) {
        fprintf(stderr, "relay_connect: getaddrinfo(%s): %s\n", host, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = result; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);

    if (fd < 0) {
        fprintf(stderr, "relay_connect: could not connect to %s:%u: %s\n",
                host, port, strerror(errno));
        return -1;
    }

    /* This protocol is exactly what Nagle's algorithm hurts: the driver
     * sends one picture's H.264 slice data, then immediately sends a
     * small, separate AUD to mark the boundary (see
     * xvmc_relay_end_picture), and needs the corresponding MPEG-2
     * back promptly to keep decoding -- Nagle would try to coalesce
     * that second small write with the next one or wait for an ACK,
     * adding real latency to every single picture. Best-effort: a
     * platform where TCP_NODELAY isn't supported would just keep
     * Nagle's default behavior, not fail the connection over it. */
    int one = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0)
        fprintf(stderr, "relay_connect: setsockopt(TCP_NODELAY) failed: %s (continuing anyway)\n",
                strerror(errno));

    conn->fd = fd;
    return 0;
}

ssize_t relay_send(struct relay_conn *conn, const void *buf, size_t len)
{
    const char *p = buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(conn->fd, p + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return (ssize_t)sent;
}

ssize_t relay_recv(struct relay_conn *conn, void *buf, size_t len)
{
    for (;;) {
        ssize_t n = recv(conn->fd, buf, len, 0);
        if (n < 0 && errno == EINTR)
            continue;
        return n;
    }
}

int relay_poll_readable(struct relay_conn *conn, int timeout_ms)
{
    struct pollfd pfd = { .fd = conn->fd, .events = POLLIN, .revents = 0 };
    for (;;) {
        int rc = poll(&pfd, 1, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (rc == 0)
            return 0;
        return (pfd.revents & (POLLIN | POLLHUP | POLLERR)) ? 1 : 0;
    }
}

void relay_close(struct relay_conn *conn)
{
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
}
