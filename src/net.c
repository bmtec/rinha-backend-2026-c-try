#include "rinha.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef SO_REUSEPORT
#define SO_REUSEPORT 15
#endif

int env_int(const char *key, int def) {
    const char *v = getenv(key);
    if (!v || !*v) return def;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    return end && *end == '\0' ? (int)n : def;
}

size_t env_size(const char *key, size_t def) {
    const char *v = getenv(key);
    if (!v || !*v) return def;
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    return end && *end == '\0' ? (size_t)n : def;
}

void sleep_ms(long ms) {
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000L,
    };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}

void close_fd(int fd) {
    if (fd >= 0) close(fd);
}

void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void set_flag(int fd, int level, int opt) {
    int one = 1;
    (void)setsockopt(fd, level, opt, &one, sizeof(one));
}

void set_tcp_nodelay(int fd) {
    set_flag(fd, IPPROTO_TCP, TCP_NODELAY);
}

void set_quickack(int fd) {
#ifdef TCP_QUICKACK
    set_flag(fd, IPPROTO_TCP, TCP_QUICKACK);
#endif
}

void set_tcp_defer_accept(int fd, int seconds) {
#ifdef TCP_DEFER_ACCEPT
    (void)setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &seconds, sizeof(seconds));
#endif
}

int tcp_listener(int port, int backlog, bool reuseport) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    set_flag(fd, SOL_SOCKET, SO_REUSEADDR);
    if (reuseport) set_flag(fd, SOL_SOCKET, SO_REUSEPORT);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int uds_listener(const char *path, int backlog, bool seqpacket) {
    unlink(path);
    int type = seqpacket ? SOCK_SEQPACKET : SOCK_STREAM;
    int fd = socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t n = strlen(path);
    if (n >= sizeof(addr.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(addr.sun_path, path, n + 1);
    socklen_t len = (socklen_t)(sizeof(sa_family_t) + n + 1);
    if (bind(fd, (struct sockaddr *)&addr, len) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, backlog) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int uds_connect(const char *path, bool seqpacket) {
    int type = seqpacket ? SOCK_SEQPACKET : SOCK_STREAM;
    int fd = socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t n = strlen(path);
    if (n >= sizeof(addr.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(addr.sun_path, path, n + 1);
    socklen_t len = (socklen_t)(sizeof(sa_family_t) + n + 1);
    if (connect(fd, (struct sockaddr *)&addr, len) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

io_result_t accept_nb(int fd) {
    int c = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (c >= 0) return (io_result_t){.kind = IO_OK, .n = c};
    if (errno == EAGAIN || errno == EWOULDBLOCK) return (io_result_t){.kind = IO_WOULD_BLOCK, .n = -1};
    return (io_result_t){.kind = IO_ERR, .n = -1};
}

io_result_t read_nb(int fd, uint8_t *buf, size_t len) {
    for (;;) {
        ssize_t n = recv(fd, buf, len, 0);
        if (n > 0) return (io_result_t){.kind = IO_OK, .n = n};
        if (n == 0) return (io_result_t){.kind = IO_EOF, .n = 0};
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return (io_result_t){.kind = IO_WOULD_BLOCK, .n = -1};
        return (io_result_t){.kind = IO_ERR, .n = -1};
    }
}

bool write_all_nb(int fd, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

bool send_fd_flags(int channel, int fd, int flags) {
    char byte = 0;
    struct iovec iov = {
        .iov_base = &byte,
        .iov_len = 1,
    };
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } cmsg;
    memset(&cmsg, 0, sizeof(cmsg));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg.buf;
    msg.msg_controllen = sizeof(cmsg.buf);

    struct cmsghdr *hdr = CMSG_FIRSTHDR(&msg);
    hdr->cmsg_level = SOL_SOCKET;
    hdr->cmsg_type = SCM_RIGHTS;
    hdr->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(hdr), &fd, sizeof(int));
    msg.msg_controllen = CMSG_SPACE(sizeof(int));

    for (;;) {
        ssize_t rc = sendmsg(channel, &msg, flags);
        if (rc >= 0) return true;
        if (errno == EINTR) continue;
        return false;
    }
}

recvfd_result_t recv_fd_nb(int channel) {
    char byte = 0;
    struct iovec iov = {
        .iov_base = &byte,
        .iov_len = 1,
    };
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int))];
    } cmsg;
    memset(&cmsg, 0, sizeof(cmsg));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg.buf;
    msg.msg_controllen = sizeof(cmsg.buf);

    for (;;) {
        ssize_t n = recvmsg(channel, &msg, MSG_DONTWAIT);
        if (n == 0) return (recvfd_result_t){.kind = RECVFD_CLOSED, .fd = -1};
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return (recvfd_result_t){.kind = RECVFD_WOULD_BLOCK, .fd = -1};
            return (recvfd_result_t){.kind = RECVFD_CLOSED, .fd = -1};
        }
        struct cmsghdr *hdr = CMSG_FIRSTHDR(&msg);
        if (!hdr) continue;
        if (hdr->cmsg_level == SOL_SOCKET && hdr->cmsg_type == SCM_RIGHTS) {
            int fd = -1;
            memcpy(&fd, CMSG_DATA(hdr), sizeof(int));
            return (recvfd_result_t){.kind = RECVFD_FD, .fd = fd};
        }
    }
}

void mlock_best_effort(const void *addr, size_t len) {
    (void)mlock(addr, len);
}

void madvise_best_effort(const void *addr, size_t len) {
    (void)madvise((void *)addr, len, MADV_WILLNEED);
}

int epoll_wait_us(int epfd, struct epoll_event *events, int maxevents, int timeout_us) {
    if (timeout_us < 0) return epoll_wait(epfd, events, maxevents, -1);
    if (timeout_us == 0) return epoll_wait(epfd, events, maxevents, 0);
#ifdef SYS_epoll_pwait2
    struct timespec ts = {
        .tv_sec = timeout_us / 1000000,
        .tv_nsec = (long)(timeout_us % 1000000) * 1000L,
    };
    int rc = (int)syscall(SYS_epoll_pwait2, epfd, events, maxevents, &ts, NULL, sizeof(sigset_t));
    if (rc >= 0 || errno != ENOSYS) return rc;
#endif
    int timeout_ms = (timeout_us + 999) / 1000;
    return epoll_wait(epfd, events, maxevents, timeout_ms);
}
