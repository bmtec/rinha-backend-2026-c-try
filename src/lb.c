#include "rinha.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void wait_read(int fd) {
    struct pollfd pfd = {
        .fd = fd,
        .events = POLLIN,
        .revents = 0,
    };
    while (poll(&pfd, 1, -1) < 0 && errno == EINTR) {}
}

static int split_paths(char *input, char **out, int max) {
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(input, ",", &save); tok && n < max; tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t') tok++;
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n')) {
            *--end = '\0';
        }
        if (*tok) out[n++] = tok;
    }
    return n;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int port = env_int("LB_PORT", 9999);
    int backlog = env_int("LB_BACKLOG", 65535);
    int accept_batch = env_int("LB_ACCEPT_BATCH", 64);
    bool set_nodelay = env_int("LB_SET_NODELAY", 1) != 0;
    bool set_qack = env_int("LB_SET_QUICKACK", 1) != 0;
    bool defer_accept = env_int("LB_DEFER_ACCEPT", 1) != 0;

    const char *kind = getenv("CONTROL_SOCKET_KIND");
    bool seqpacket = !(kind && strcmp(kind, "stream") == 0);

    char sockets_buf[512];
    const char *env_sockets = getenv("API_SOCKETS");
    if (!env_sockets || !*env_sockets) env_sockets = "/sockets/api1.sock,/sockets/api2.sock";
    snprintf(sockets_buf, sizeof(sockets_buf), "%s", env_sockets);

    char *paths[16];
    int n = split_paths(sockets_buf, paths, 16);
    if (n <= 0) {
        fprintf(stderr, "[lb-c] no API sockets configured\n");
        return 1;
    }

    int listener = tcp_listener(port, backlog, true);
    if (listener < 0) {
        perror("tcp_listener");
        return 1;
    }
    set_nonblocking(listener);
    if (defer_accept) set_tcp_defer_accept(listener, 1);

    int channels[16];
    for (int i = 0; i < n; i++) {
        for (;;) {
            int fd = uds_connect(paths[i], seqpacket);
            if (fd >= 0) {
                channels[i] = fd;
                fprintf(stderr, "[lb-c] connected to %s\n", paths[i]);
                break;
            }
            sleep_ms(100);
        }
    }

    fprintf(stderr, "[lb-c] listening on :%d, %d backends, batch=%d\n", port, n, accept_batch);

    int rr = 0;
    for (;;) {
        int accepted = 0;
        while (accepted < accept_batch) {
            io_result_t a = accept_nb(listener);
            if (a.kind == IO_WOULD_BLOCK) break;
            if (a.kind != IO_OK) continue;
            int client = (int)a.n;
            accepted++;

            if (set_nodelay) set_tcp_nodelay(client);
            if (set_qack) set_quickack(client);

            int first = rr;
            rr = (rr + 1) % n;
            bool sent = false;
            for (int attempt = 0; attempt < n; attempt++) {
                int ch = channels[(first + attempt) % n];
                if (send_fd_flags(ch, client, MSG_NOSIGNAL | MSG_DONTWAIT)) {
                    sent = true;
                    break;
                }
            }
            if (!sent) (void)send_fd_flags(channels[first], client, MSG_NOSIGNAL);
            close_fd(client);
        }
        if (accepted == 0) wait_read(listener);
    }
}
