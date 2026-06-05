#include "rinha.h"

#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define LB_INSTRUMENT_BUCKETS 2048
#define LB_BATCH_BUCKETS 128

typedef enum {
    LB_STAGE_SEND_FD = 0,
    LB_STAGE_HANDOFF = 1,
    LB_STAGE_POST_ACCEPT = 2,
    LB_STAGE_COUNT = 3,
} lb_stage_t;

static const char *const lb_stage_names[LB_STAGE_COUNT] = {
    "send_fd",
    "handoff",
    "post_accept",
};

static bool g_lb_instrument = false;
static unsigned g_lb_instrument_interval_secs = 5;
static atomic_uint_fast64_t g_lb_hist[LB_STAGE_COUNT][LB_INSTRUMENT_BUCKETS];
static atomic_uint_fast64_t g_lb_batch_hist[LB_BATCH_BUCKETS];

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

static bool env_truthy(const char *key) {
    const char *value = getenv(key);
    if (!value || !*value) return false;
    return strcmp(value, "0") != 0 && strcmp(value, "false") != 0 && strcmp(value, "FALSE") != 0;
}

static inline uint64_t lb_now_ns(void) {
    if (!g_lb_instrument) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ull) + (uint64_t)ts.tv_nsec;
}

static inline void lb_record_ns(lb_stage_t stage, uint64_t elapsed_ns) {
    if (!g_lb_instrument || stage >= LB_STAGE_COUNT) return;
    size_t bucket = (size_t)(elapsed_ns / 1000u);
    if (bucket >= LB_INSTRUMENT_BUCKETS) bucket = LB_INSTRUMENT_BUCKETS - 1;
    atomic_fetch_add_explicit(&g_lb_hist[stage][bucket], 1, memory_order_relaxed);
}

static inline void lb_record_batch(unsigned batch) {
    if (!g_lb_instrument) return;
    if (batch >= LB_BATCH_BUCKETS) batch = LB_BATCH_BUCKETS - 1;
    atomic_fetch_add_explicit(&g_lb_batch_hist[batch], 1, memory_order_relaxed);
}

static void lb_dump_stage(lb_stage_t stage) {
    uint64_t counts[LB_INSTRUMENT_BUCKETS];
    uint64_t total = 0;
    uint64_t weighted = 0;
    uint64_t max_bucket = 0;

    for (size_t i = 0; i < LB_INSTRUMENT_BUCKETS; i++) {
        uint64_t count = atomic_exchange_explicit(&g_lb_hist[stage][i], 0, memory_order_relaxed);
        counts[i] = count;
        total += count;
        weighted += count * i;
        if (count) max_bucket = i;
    }

    if (total == 0) {
        fprintf(stderr, "[lb-inst] stage=%s count=0\n", lb_stage_names[stage]);
        return;
    }

    uint64_t p50_target = (total * 50 + 99) / 100;
    uint64_t p95_target = (total * 95 + 99) / 100;
    uint64_t p99_target = (total * 99 + 99) / 100;
    uint64_t seen = 0;
    uint64_t p50 = 0;
    uint64_t p95 = 0;
    uint64_t p99 = 0;
    bool has_p50 = false;
    bool has_p95 = false;
    bool has_p99 = false;

    for (size_t i = 0; i < LB_INSTRUMENT_BUCKETS; i++) {
        seen += counts[i];
        if (!has_p50 && seen >= p50_target) {
            p50 = i;
            has_p50 = true;
        }
        if (!has_p95 && seen >= p95_target) {
            p95 = i;
            has_p95 = true;
        }
        if (!has_p99 && seen >= p99_target) {
            p99 = i;
            has_p99 = true;
            break;
        }
    }

    double avg = (double)weighted / (double)total;
    fprintf(stderr,
            "[lb-inst] stage=%s count=%" PRIu64 " avg_us=%.2f p50_us=%" PRIu64
            " p95_us=%" PRIu64 " p99_us=%" PRIu64 " max_us=%" PRIu64 "\n",
            lb_stage_names[stage],
            total,
            avg,
            p50,
            p95,
            p99,
            max_bucket);
}

static void lb_dump_batches(void) {
    uint64_t total = 0;
    uint64_t weighted = 0;
    uint64_t max_bucket = 0;

    for (size_t i = 0; i < LB_BATCH_BUCKETS; i++) {
        uint64_t count = atomic_exchange_explicit(&g_lb_batch_hist[i], 0, memory_order_relaxed);
        total += count;
        weighted += count * i;
        if (count) max_bucket = i;
    }

    if (total == 0) {
        fprintf(stderr, "[lb-inst] batch count=0\n");
        return;
    }

    double avg = (double)weighted / (double)total;
    fprintf(stderr,
            "[lb-inst] batch count=%" PRIu64 " avg=%.2f max=%" PRIu64 "\n",
            total,
            avg,
            max_bucket);
}

static void *lb_instrument_loop(void *unused) {
    (void)unused;
    for (;;) {
        sleep(g_lb_instrument_interval_secs);
        for (lb_stage_t stage = 0; stage < LB_STAGE_COUNT; stage++) {
            lb_dump_stage(stage);
        }
        lb_dump_batches();
    }
    return NULL;
}

static void lb_instrument_init(void) {
    if (!env_truthy("LB_INSTRUMENT")) return;

    int interval = env_int("LB_INSTRUMENT_INTERVAL_SECS", 5);
    if (interval > 0) g_lb_instrument_interval_secs = (unsigned)interval;
    g_lb_instrument = true;

    pthread_t thread;
    if (pthread_create(&thread, NULL, lb_instrument_loop, NULL) == 0) {
        pthread_detach(thread);
        fprintf(stderr,
                "[lb-inst] enabled interval=%us; local observability only, payload is not inspected\n",
                g_lb_instrument_interval_secs);
    } else {
        g_lb_instrument = false;
        fprintf(stderr, "[lb-inst] disabled: pthread_create failed\n");
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    lb_instrument_init();
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

    // O LB é propositalmente burro: round-robin simples e passagem do FD para a
    // API escolhida. Ele não lê JSON, não calcula score e não toma decisão de
    // negócio, mantendo a separação exigida pela regra do desafio.
    int rr = 0;
    for (;;) {
        int accepted = 0;
        while (accepted < accept_batch) {
            io_result_t a = accept_nb(listener);
            if (a.kind == IO_WOULD_BLOCK) break;
            if (a.kind != IO_OK) continue;
            int client = (int)a.n;
            uint64_t accepted_ns = lb_now_ns();
            accepted++;

            if (set_nodelay) set_tcp_nodelay(client);
            if (set_qack) set_quickack(client);

            int first = rr;
            rr = (rr + 1) % n;
            bool sent = false;
            uint64_t handoff_start = lb_now_ns();
            for (int attempt = 0; attempt < n; attempt++) {
                int ch = channels[(first + attempt) % n];
                uint64_t send_start = lb_now_ns();
                if (send_fd_flags(ch, client, MSG_NOSIGNAL | MSG_DONTWAIT)) {
                    uint64_t send_done = lb_now_ns();
                    if (send_start && send_done) lb_record_ns(LB_STAGE_SEND_FD, send_done - send_start);
                    sent = true;
                    break;
                }
                uint64_t send_done = lb_now_ns();
                if (send_start && send_done) lb_record_ns(LB_STAGE_SEND_FD, send_done - send_start);
            }
            if (!sent) {
                uint64_t send_start = lb_now_ns();
                (void)send_fd_flags(channels[first], client, MSG_NOSIGNAL);
                uint64_t send_done = lb_now_ns();
                if (send_start && send_done) lb_record_ns(LB_STAGE_SEND_FD, send_done - send_start);
            }
            uint64_t handoff_done = lb_now_ns();
            if (handoff_start && handoff_done) lb_record_ns(LB_STAGE_HANDOFF, handoff_done - handoff_start);
            close_fd(client);
            uint64_t closed_ns = lb_now_ns();
            if (accepted_ns && closed_ns) lb_record_ns(LB_STAGE_POST_ACCEPT, closed_ns - accepted_ns);
        }
        if (accepted > 0) lb_record_batch((unsigned)accepted);
        if (accepted == 0) wait_read(listener);
    }
}
