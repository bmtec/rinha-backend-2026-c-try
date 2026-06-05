#include "rinha.h"

#include <errno.h>
#include <fcntl.h>
#include <immintrin.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_EVENTS 1024
#define READ_CHUNK 4096
#define MAX_FD_SLOTS 65536
#define CONN_BUF_CAP (16 * 1024)
#define LISTEN_TOKEN UINT64_MAX

typedef struct {
    uint8_t buf[CONN_BUF_CAP];
    size_t len;
} conn_t;

typedef struct {
    conn_t **slots;
    conn_t **pool;
    size_t pool_len;
    size_t pool_cap;
} conn_table_t;

typedef enum {
    PARSE_NEED,
    PARSE_READY,
    PARSE_NOTFOUND,
    PARSE_FRAUD
} parse_kind_t;

typedef struct {
    parse_kind_t kind;
    size_t consumed;
    size_t body_off;
    size_t body_len;
} request_parse_t;

static conn_t *conn_new(void) {
    conn_t *c = malloc(sizeof(conn_t));
    if (!c) abort();
    c->len = 0;
    return c;
}

static void conn_table_init(conn_table_t *t, size_t pool_cap) {
    t->slots = calloc(MAX_FD_SLOTS, sizeof(conn_t *));
    t->pool = calloc(pool_cap ? pool_cap : 1, sizeof(conn_t *));
    if (!t->slots || !t->pool) abort();
    t->pool_cap = pool_cap;
    t->pool_len = 0;
    size_t initial = pool_cap < 128 ? pool_cap : 128;
    for (size_t i = 0; i < initial; i++) t->pool[t->pool_len++] = conn_new();
}

static bool conn_insert(conn_table_t *t, int fd) {
    if (fd < 0 || fd >= MAX_FD_SLOTS) return false;
    conn_t *c = t->pool_len ? t->pool[--t->pool_len] : conn_new();
    c->len = 0;
    t->slots[fd] = c;
    return true;
}

static conn_t *conn_get(conn_table_t *t, int fd) {
    if (fd < 0 || fd >= MAX_FD_SLOTS) return NULL;
    return t->slots[fd];
}

static void conn_remove(conn_table_t *t, int fd) {
    if (fd < 0 || fd >= MAX_FD_SLOTS) return;
    conn_t *c = t->slots[fd];
    if (!c) return;
    t->slots[fd] = NULL;
    c->len = 0;
    if (t->pool_len < t->pool_cap) {
        t->pool[t->pool_len++] = c;
    } else {
        free(c);
    }
}

static bool request_path_is(const uint8_t *buf, size_t hdr_end, const char *want) {
    const uint8_t *p1 = memchr(buf, ' ', hdr_end);
    if (!p1) return false;
    p1++;
    const uint8_t *p2 = memchr(p1, ' ', (size_t)(buf + hdr_end - p1));
    if (!p2) return false;
    size_t n = (size_t)(p2 - p1);
    size_t want_len = strlen(want);
    return n == want_len && memcmp(p1, want, want_len) == 0;
}

static bool ascii_eq_ci(uint8_t a, uint8_t b) {
    if (a >= 'A' && a <= 'Z') a = (uint8_t)(a + 32);
    return a == b;
}

static const uint8_t *find_ci(const uint8_t *hay, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || hlen < nlen) return NULL;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j = 0;
        while (j < nlen && ascii_eq_ci(hay[i + j], (uint8_t)needle[j])) j++;
        if (j == nlen) return hay + i;
    }
    return NULL;
}

static bool content_length(const uint8_t *headers, size_t len, size_t *out) {
    const char *k1 = "Content-Length:";
    const char *k2 = "content-length:";
    const uint8_t *p = memmem(headers, len, k1, strlen(k1));
    size_t klen = strlen(k1);
    if (!p) {
        p = memmem(headers, len, k2, strlen(k2));
        klen = strlen(k2);
    }
    if (!p) {
        p = find_ci(headers, len, k2);
        klen = strlen(k2);
    }
    if (!p) return false;
    size_t i = (size_t)(p - headers) + klen;
    while (i < len && (headers[i] == ' ' || headers[i] == '\t')) i++;
    size_t v = 0;
    bool saw = false;
    while (i < len && headers[i] >= '0' && headers[i] <= '9') {
        v = v * 10 + (size_t)(headers[i] - '0');
        i++;
        saw = true;
    }
    if (!saw) return false;
    *out = v;
    return true;
}

static request_parse_t parse_request(const uint8_t *buf, size_t len) {
    const uint8_t *hdr = memmem(buf, len, "\r\n\r\n", 4);
    if (!hdr) return (request_parse_t){.kind = PARSE_NEED};
    size_t hdr_end = (size_t)(hdr - buf) + 4;
    if (buf[0] == 'P') {
        size_t cl = 0;
        (void)content_length(buf, hdr_end, &cl);
        size_t total = hdr_end + cl;
        if (len < total) return (request_parse_t){.kind = PARSE_NEED};
        return (request_parse_t){.kind = PARSE_FRAUD, .consumed = total, .body_off = hdr_end, .body_len = cl};
    }
    if (buf[0] == 'G') {
        if (request_path_is(buf, hdr_end, "/ready")) {
            return (request_parse_t){.kind = PARSE_READY, .consumed = hdr_end};
        }
        return (request_parse_t){.kind = PARSE_NOTFOUND, .consumed = hdr_end};
    }
    return (request_parse_t){.kind = PARSE_NOTFOUND, .consumed = hdr_end};
}

static size_t score_body(const uint8_t *body, size_t len, const ivf_index_t *index, query_options_t opts) {
    payload_t p;
    if (!parse_payload(body, len, &p)) return 0;
    float v[DIMS];
    int16_t q[DIMS];
    vectorize_quantized(&p, v, q);
    return index_query_quantized(index, v, q, opts);
}

static bool handle_client(int fd, conn_t *conn, const ivf_index_t *index, query_options_t opts) {
    for (;;) {
        if (conn->len == CONN_BUF_CAP) return false;
        size_t cap = CONN_BUF_CAP - conn->len;
        if (cap > READ_CHUNK) cap = READ_CHUNK;
        io_result_t r = read_nb(fd, conn->buf + conn->len, cap);
        if (r.kind == IO_OK) {
            conn->len += (size_t)r.n;
            if ((size_t)r.n < cap) break;
            continue;
        }
        if (r.kind == IO_WOULD_BLOCK) break;
        return false;
    }
    if (conn->len == 0) return true;

    size_t start = 0;
    while (start < conn->len) {
        request_parse_t p = parse_request(conn->buf + start, conn->len - start);
        if (p.kind == PARSE_NEED) break;
        size_t out_len = 0;
        const uint8_t *out = NULL;
        if (p.kind == PARSE_READY) {
            out = ready_response(&out_len);
        } else if (p.kind == PARSE_NOTFOUND) {
            out = notfound_response(&out_len);
        } else {
            size_t fc = score_body(conn->buf + start + p.body_off, p.body_len, index, opts);
            out = full_response(fc, &out_len);
        }
        if (!write_all_nb(fd, out, out_len)) return false;
        start += p.consumed;
    }

    if (start > 0) {
        if (start == conn->len) {
            conn->len = 0;
        } else {
            memmove(conn->buf, conn->buf + start, conn->len - start);
            conn->len -= start;
        }
    }
    return true;
}

static int wait_events(int epfd, struct epoll_event *events, int spin_us, int idle_us) {
    if (spin_us <= 0) return epoll_wait_us(epfd, events, MAX_EVENTS, idle_us > 0 ? idle_us : -1);
    int n = epoll_wait_us(epfd, events, MAX_EVENTS, 0);
    if (n != 0) return n;

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        n = epoll_wait_us(epfd, events, MAX_EVENTS, 0);
        if (n != 0) return n;
        _mm_pause();
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_us = (long)(now.tv_sec - start.tv_sec) * 1000000L +
                          (long)(now.tv_nsec - start.tv_nsec) / 1000L;
        if (elapsed_us >= spin_us) break;
    }
    return epoll_wait_us(epfd, events, MAX_EVENTS, idle_us > 0 ? idle_us : -1);
}

static bool is_control_fd(const int *controls, size_t controls_len, int fd, size_t *pos) {
    for (size_t i = 0; i < controls_len; i++) {
        if (controls[i] == fd) {
            if (pos) *pos = i;
            return true;
        }
    }
    return false;
}

static bool drain_control(int channel, int epfd, conn_table_t *conns, const ivf_index_t *index, query_options_t opts) {
    for (;;) {
        recvfd_result_t r = recv_fd_nb(channel);
        if (r.kind == RECVFD_WOULD_BLOCK) return true;
        if (r.kind == RECVFD_CLOSED) return false;
        int fd = r.fd;
        set_nonblocking(fd);
        struct epoll_event ev = {
            .events = EPOLLIN,
            .data.u64 = (uint64_t)fd,
        };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0 || !conn_insert(conns, fd)) {
            close_fd(fd);
            continue;
        }
        conn_t *c = conn_get(conns, fd);
        bool keep = c && handle_client(fd, c, index, opts);
        if (!keep) {
            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
            conn_remove(conns, fd);
            close_fd(fd);
        }
    }
}

static void warm_up(const ivf_index_t *index, query_options_t opts) {
    size_t count = env_size("API_WARMUP_QUERIES", 2048);
    uint8_t acc = 0;
    for (size_t i = 0; i < count; i++) {
        float v[DIMS] = {0};
        int16_t q[DIMS] = {0};
        for (size_t d = 0; d < REAL_DIMS; d++) {
            v[d] = (float)((i * 131 + d * 17) % 1000) / 1000.0f;
        }
        if ((i & 3u) == 0) {
            v[5] = -1.0f;
            v[6] = -1.0f;
        }
        quantize_i16(v, q);
        acc ^= index_query_quantized(index, v, q, opts);
    }
    asm volatile("" : "+r"(acc));
}

static void fd_worker(const char *socket_path, int backlog, const ivf_index_t *index, query_options_t opts) {
    const char *kind = getenv("CONTROL_SOCKET_KIND");
    bool seqpacket = !(kind && strcmp(kind, "stream") == 0);

    int listener = uds_listener(socket_path, backlog, seqpacket);
    if (listener < 0) {
        perror("uds_listener");
        exit(1);
    }
    set_nonblocking(listener);

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        perror("epoll_create1");
        exit(1);
    }
    struct epoll_event ev = {
        .events = EPOLLIN,
        .data.u64 = LISTEN_TOKEN,
    };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listener, &ev) < 0) {
        perror("epoll_ctl listener");
        exit(1);
    }

    int controls[64];
    size_t controls_len = 0;
    conn_table_t conns;
    conn_table_init(&conns, env_size("CONN_POOL_CAP", 512));
    struct epoll_event events[MAX_EVENTS];
    int spin_us = env_int("EPOLL_SPIN_US", 30);
    int idle_us = env_int("EPOLL_IDLE_US", 80);

    for (;;) {
        int n = wait_events(epfd, events, spin_us, idle_us);
        if (n < 0) {
            if (errno == EINTR) continue;
            continue;
        }
        for (int i = 0; i < n; i++) {
            uint64_t token = events[i].data.u64;
            if (token == LISTEN_TOKEN) {
                for (;;) {
                    io_result_t a = accept_nb(listener);
                    if (a.kind != IO_OK) break;
                    int fd = (int)a.n;
                    struct epoll_event cev = {
                        .events = EPOLLIN,
                        .data.u64 = (uint64_t)fd,
                    };
                    if (controls_len < 64 && epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &cev) == 0) {
                        controls[controls_len++] = fd;
                    } else {
                        close_fd(fd);
                    }
                }
                continue;
            }

            int fd = (int)token;
            size_t control_pos = 0;
            if (is_control_fd(controls, controls_len, fd, &control_pos)) {
                if (!drain_control(fd, epfd, &conns, index, opts)) {
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                    close_fd(fd);
                    controls[control_pos] = controls[--controls_len];
                }
                continue;
            }

            conn_t *c = conn_get(&conns, fd);
            bool keep = c && handle_client(fd, c, index, opts);
            if (!keep) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                conn_remove(&conns, fd);
                close_fd(fd);
            }
        }
    }
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    const char *index_path = getenv("INDEX_PATH");
    if (!index_path || !*index_path) index_path = "/data/index.bin";
    int nprobe = env_int("NPROBE", 10);
    int repair_probe = env_int("REPAIR_PROBE", 48);
    int repair_candidates = env_int("REPAIR_CANDIDATES", 0);
    int repair_min = env_int("REPAIR_MIN", 1);
    int repair_max = env_int("REPAIR_MAX", 4);
    if (repair_probe < nprobe) repair_probe = nprobe;
    if (repair_candidates < 0) repair_candidates = 0;
    if (repair_candidates > (int)MAX_REPAIR_CANDIDATES) repair_candidates = (int)MAX_REPAIR_CANDIDATES;
    if (repair_min < 0) repair_min = 0;
    if (repair_min > KNN_K) repair_min = KNN_K;
    if (repair_max < repair_min) repair_max = repair_min;
    if (repair_max > KNN_K) repair_max = KNN_K;
    int backlog = env_int("LISTEN_BACKLOG", 4096);
    const char *api_socket = getenv("API_SOCKET");
    if (!api_socket || !*api_socket) {
        fprintf(stderr, "[api-c] API_SOCKET is required in this lab\n");
        return 1;
    }

    int fd = open(index_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        perror("open index");
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        perror("fstat index");
        return 1;
    }
    size_t len = (size_t)st.st_size;
    uint8_t *data = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED) {
        perror("mmap index");
        return 1;
    }
    madvise_best_effort(data, len);
    mlock_best_effort(data, len);

    volatile uint8_t warm = 0;
    for (size_t off = 0; off < len; off += 4096) warm ^= data[off];
    asm volatile("" : "+r"(warm));

    ivf_index_t index;
    if (!index_from_bytes(data, len, &index)) {
        fprintf(stderr, "[api-c] invalid index\n");
        return 1;
    }
    query_options_t opts = {
        .nprobe = (size_t)nprobe,
        .repair_probe = (size_t)repair_probe,
        .repair_candidates = (size_t)repair_candidates,
        .repair_min = (uint8_t)repair_min,
        .repair_max = (uint8_t)repair_max,
    };
    warm_up(&index, opts);

    fprintf(stderr,
            "[api-c] index %zu vectors, nprobe=%zu, repair=%zu+bbox%zu/%u-%u, socket=%s\n",
            index.num_vectors,
            opts.nprobe,
            opts.repair_probe,
            opts.repair_candidates,
            opts.repair_min,
            opts.repair_max,
            api_socket);
    fd_worker(api_socket, backlog, &index, opts);
    return 0;
}
