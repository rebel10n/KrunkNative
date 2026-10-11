/* Blocking socket helpers for the HTTP/WebSocket tests (client side runs in a thread). */
#ifndef KN_NET_TEST_UTIL_H
#define KN_NET_TEST_UTIL_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int tcp_connect(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        close(fd);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return fd;
}

static int send_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t w = send(fd, c, n, MSG_NOSIGNAL);
        if (w <= 0) return -1;
        c += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Read until `until` appears (or EOF / timeout). Returns bytes in buf (NUL-terminated). */
static size_t recv_until(int fd, char *buf, size_t cap, const char *until, int timeout_ms) {
    size_t n = 0;
    buf[0] = 0;
    while (n + 1 < cap) {
        if (until && memmem(buf, n, until, strlen(until))) break;
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, timeout_ms) <= 0) break;
        ssize_t r = recv(fd, buf + n, cap - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r;
        buf[n] = 0;
    }
    return n;
}

/* 1 if the peer closed (EOF) within timeout. */
static int wait_eof(int fd, int timeout_ms) {
    char tmp[4096];
    for (;;) {
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, timeout_ms) <= 0) return 0;
        ssize_t r = recv(fd, tmp, sizeof tmp, 0);
        if (r <= 0) return 1;
    }
}

#endif
