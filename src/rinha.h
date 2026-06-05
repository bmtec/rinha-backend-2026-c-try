#pragma once

#define _GNU_SOURCE

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/types.h>

#define DIMS 16
#define REAL_DIMS 14
#define KNN_K 5
#define MAX_MERCHANTS 16
#define SCALE_F 10000.0f

#define MAX_AMOUNT 10000.0f
#define MAX_INSTALLMENTS 12.0f
#define AMOUNT_VS_AVG_RATIO 10.0f
#define MAX_MINUTES 1440.0f
#define MAX_KM 1000.0f
#define MAX_TX_COUNT_24H 20.0f
#define MAX_MERCHANT_AVG_AMOUNT 10000.0f

#define MAGIC_RINH 0x52494E48u
#define INDEX_VERSION 5u
#define MIN_SUPPORTED_VERSION 3u
#define HEADER_BYTES 20u
#define SECTION_ALIGN 32u
#define BLOCK_SIZE 128u
#define MAX_NPROBE 128u
#define MAX_CENTROIDS 2048u
#define MAX_REPAIR_CANDIDATES 256u
#define SCAN_CHUNK BLOCK_SIZE
#define K_RERANK 5u

typedef struct {
    const uint8_t *ptr;
    size_t len;
} slice_t;

typedef struct {
    float amount;
    uint32_t installments;
    uint8_t requested_at[20];
    float avg_amount;
    uint32_t tx_count_24h;
    slice_t known_merchants[MAX_MERCHANTS];
    size_t known_merchants_len;
    slice_t merchant_id;
    uint8_t mcc[4];
    float merchant_avg_amount;
    bool is_online;
    bool card_present;
    float km_from_home;
    bool has_last_transaction;
    uint8_t last_tx_timestamp[20];
    bool has_last_ts;
    float km_from_current;
    bool has_km_current;
} payload_t;

typedef struct {
    uint32_t start;
    uint32_t count;
    uint32_t block_start;
    uint32_t block_count;
} cell_t;

typedef struct {
    int16_t min[DIMS];
    int16_t max[DIMS];
} bounds_t;

// O arquivo de índice é lido por mmap e acessado por ponteiros para evitar
// cópias no startup. As seções ficam alinhadas para favorecer loads AVX2.
typedef struct {
    size_t num_vectors;
    size_t num_centroids;
    const int16_t (*centroids)[DIMS];
    const cell_t *cells;
    const bounds_t *cell_bounds;
    const bounds_t *blocks;
    const int16_t (*vectors)[DIMS];
    const uint8_t *labels;
    size_t blocks_len;
} ivf_index_t;

// A busca começa com nprobe curto e só expande em casos ambíguos. Isso preserva
// recall onde importa sem pagar o custo da busca larga em toda requisição.
typedef struct {
    size_t nprobe;
    size_t repair_probe;
    size_t repair_candidates;
    uint8_t repair_min;
    uint8_t repair_max;
} query_options_t;

typedef enum {
    IO_OK,
    IO_WOULD_BLOCK,
    IO_EOF,
    IO_ERR
} io_kind_t;

typedef struct {
    io_kind_t kind;
    ssize_t n;
} io_result_t;

typedef enum {
    RECVFD_FD,
    RECVFD_WOULD_BLOCK,
    RECVFD_CLOSED
} recvfd_kind_t;

typedef struct {
    recvfd_kind_t kind;
    int fd;
} recvfd_result_t;

int16_t quantize_one(float v);
void quantize_i16(const float v[DIMS], int16_t q[DIMS]);
int64_t squared_euclidean_i16(const int16_t a[DIMS], const int16_t b[DIMS]);
void distances_to_slice_i16(const int16_t query[DIMS], const int16_t (*vectors)[DIMS], size_t len, int64_t *out);
int64_t lower_bound_block_i16(const int16_t q[DIMS], const bounds_t *block);

bool parse_payload(const uint8_t *buf, size_t len, payload_t *out);
void vectorize_quantized(const payload_t *p, float v[DIMS], int16_t q[DIMS]);

bool index_from_bytes(const uint8_t *data, size_t len, ivf_index_t *out);
uint8_t index_query_quantized(const ivf_index_t *idx, const float v[DIMS], const int16_t q[DIMS], query_options_t opts);

const uint8_t *full_response(size_t fraud_count, size_t *len);
const uint8_t *ready_response(size_t *len);
const uint8_t *notfound_response(size_t *len);

int env_int(const char *key, int def);
size_t env_size(const char *key, size_t def);
void sleep_ms(long ms);
void close_fd(int fd);
void set_nonblocking(int fd);
void set_tcp_nodelay(int fd);
void set_quickack(int fd);
void set_tcp_defer_accept(int fd, int seconds);
int tcp_listener(int port, int backlog, bool reuseport);
int uds_listener(const char *path, int backlog, bool seqpacket);
int uds_connect(const char *path, bool seqpacket);
io_result_t accept_nb(int fd);
io_result_t read_nb(int fd, uint8_t *buf, size_t len);
bool write_all_nb(int fd, const uint8_t *buf, size_t len);
bool send_fd_flags(int channel, int fd, int flags);
recvfd_result_t recv_fd_nb(int channel);
void mlock_best_effort(const void *addr, size_t len);
void madvise_best_effort(const void *addr, size_t len);
int epoll_wait_us(int epfd, struct epoll_event *events, int maxevents, int timeout_us);
