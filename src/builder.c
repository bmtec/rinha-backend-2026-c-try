#include "rinha.h"

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#define DEFAULT_CENTROIDS 2048u
#define DEFAULT_ITERS 15u

typedef struct {
    const float (*vectors)[DIMS];
    const float (*centroids)[DIMS];
    uint32_t *assign;
    size_t start;
    size_t end;
    size_t k;
} assign_job_t;

typedef struct {
    float dist;
    float v[DIMS];
    uint8_t label;
} sort_item_t;

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static uint64_t splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

typedef struct {
    uint32_t state[16];
    uint32_t buf[64];
    size_t index;
} rust_rng_t;

static inline uint32_t rotl32(uint32_t x, unsigned n) {
    return (x << n) | (x >> (32u - n));
}

static inline void chacha_qr(uint32_t x[16], size_t a, size_t b, size_t c, size_t d) {
    x[a] += x[b]; x[d] = rotl32(x[d] ^ x[a], 16);
    x[c] += x[d]; x[b] = rotl32(x[b] ^ x[c], 12);
    x[a] += x[b]; x[d] = rotl32(x[d] ^ x[a], 8);
    x[c] += x[d]; x[b] = rotl32(x[b] ^ x[c], 7);
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void rust_seed_from_u64(uint64_t seed, uint8_t out[32]) {
    const uint64_t mul = 6364136223846793005ull;
    const uint64_t inc = 11634580027462260723ull;
    for (size_t i = 0; i < 8; i++) {
        seed = seed * mul + inc;
        uint32_t xorshifted = (uint32_t)(((seed >> 18) ^ seed) >> 27);
        uint32_t rot = (uint32_t)(seed >> 59);
        uint32_t x = (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
        out[i * 4] = (uint8_t)(x & 0xffu);
        out[i * 4 + 1] = (uint8_t)((x >> 8) & 0xffu);
        out[i * 4 + 2] = (uint8_t)((x >> 16) & 0xffu);
        out[i * 4 + 3] = (uint8_t)((x >> 24) & 0xffu);
    }
}

static void rust_rng_refill(rust_rng_t *rng) {
    for (uint32_t block = 0; block < 4; block++) {
        uint32_t x[16];
        memcpy(x, rng->state, sizeof(x));
        x[12] += block;
        if (x[12] < block) x[13]++;

        uint32_t working[16];
        memcpy(working, x, sizeof(working));
        for (size_t i = 0; i < 6; i++) {
            chacha_qr(working, 0, 4, 8, 12);
            chacha_qr(working, 1, 5, 9, 13);
            chacha_qr(working, 2, 6, 10, 14);
            chacha_qr(working, 3, 7, 11, 15);
            chacha_qr(working, 0, 5, 10, 15);
            chacha_qr(working, 1, 6, 11, 12);
            chacha_qr(working, 2, 7, 8, 13);
            chacha_qr(working, 3, 4, 9, 14);
        }
        for (size_t i = 0; i < 16; i++) rng->buf[block * 16u + i] = working[i] + x[i];
    }
    rng->state[12] += 4u;
    if (rng->state[12] < 4u) rng->state[13]++;
}

static void rust_rng_seed(rust_rng_t *rng, uint64_t seed) {
    uint8_t key[32];
    rust_seed_from_u64(seed, key);
    rng->state[0] = 0x61707865u;
    rng->state[1] = 0x3320646eu;
    rng->state[2] = 0x79622d32u;
    rng->state[3] = 0x6b206574u;
    for (size_t i = 0; i < 8; i++) rng->state[4 + i] = read_le32(key + i * 4);
    rng->state[12] = 0;
    rng->state[13] = 0;
    rng->state[14] = 0;
    rng->state[15] = 0;
    memset(rng->buf, 0, sizeof(rng->buf));
    rng->index = 64;
}

static uint32_t rust_rng_next_u32(rust_rng_t *rng) {
    if (rng->index >= 64) {
        rust_rng_refill(rng);
        rng->index = 0;
    }
    return rng->buf[rng->index++];
}

static uint64_t rust_rng_next_u64(rust_rng_t *rng) {
    if (rng->index < 63) {
        uint64_t lo = rng->buf[rng->index];
        uint64_t hi = rng->buf[rng->index + 1];
        rng->index += 2;
        return lo | (hi << 32);
    }
    if (rng->index >= 64) {
        rust_rng_refill(rng);
        rng->index = 2;
        return (uint64_t)rng->buf[0] | ((uint64_t)rng->buf[1] << 32);
    }
    uint64_t lo = rng->buf[63];
    rust_rng_refill(rng);
    rng->index = 1;
    return lo | ((uint64_t)rng->buf[0] << 32);
}

static uint32_t rust_rng_gen_range_u32(rust_rng_t *rng, uint32_t ubound) {
    uint32_t zone = (ubound << __builtin_clz(ubound)) - 1u;
    for (;;) {
        uint32_t v = rust_rng_next_u32(rng);
        uint64_t prod = (uint64_t)v * (uint64_t)ubound;
        uint32_t lo = (uint32_t)prod;
        uint32_t hi = (uint32_t)(prod >> 32);
        if (lo <= zone) return hi;
    }
}

static inline float sqdist_f32(const float a[DIMS], const float b[DIMS]) {
    float acc = 0.0f;
    for (size_t i = 0; i < DIMS; i++) {
        float d = a[i] - b[i];
        acc += d * d;
    }
    return acc;
}

static uint8_t *read_gzip_all(const char *path, size_t *len_out) {
    gzFile gz = gzopen(path, "rb");
    if (!gz) {
        fprintf(stderr, "[builder-c] cannot open %s\n", path);
        exit(1);
    }

    size_t cap = 64u << 20;
    size_t len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) die("malloc gzip buffer");

    for (;;) {
        if (len == cap) {
            cap *= 2;
            uint8_t *next = realloc(buf, cap);
            if (!next) die("realloc gzip buffer");
            buf = next;
        }
        int n = gzread(gz, buf + len, (unsigned int)((cap - len) > (1u << 20) ? (1u << 20) : (cap - len)));
        if (n > 0) {
            len += (size_t)n;
            continue;
        }
        if (n == 0) break;
        int err = 0;
        const char *why = gzerror(gz, &err);
        fprintf(stderr, "[builder-c] gzip read error: %s\n", why ? why : "unknown");
        exit(1);
    }
    gzclose(gz);
    *len_out = len;
    return buf;
}

static size_t count_vectors(const uint8_t *buf, size_t len) {
    const char needle[] = "\"vector\":[";
    size_t nlen = sizeof(needle) - 1;
    size_t count = 0;
    const uint8_t *p = buf;
    size_t left = len;
    while (left >= nlen) {
        void *hit = memmem(p, left, needle, nlen);
        if (!hit) break;
        const uint8_t *h = hit;
        count++;
        size_t used = (size_t)(h - p) + nlen;
        p += used;
        left -= used;
    }
    return count;
}

static inline void skip_ws(const uint8_t **p, const uint8_t *end) {
    while (*p < end) {
        uint8_t c = **p;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            (*p)++;
        } else {
            break;
        }
    }
}

static float parse_float_ref(const uint8_t **p, const uint8_t *end) {
    skip_ws(p, end);
    int neg = 0;
    if (*p < end && **p == '-') {
        neg = 1;
        (*p)++;
    }
    double value = 0.0;
    while (*p < end && **p >= '0' && **p <= '9') {
        value = value * 10.0 + (double)(**p - '0');
        (*p)++;
    }
    if (*p < end && **p == '.') {
        (*p)++;
        double frac = 0.0;
        double scale = 1.0;
        while (*p < end && **p >= '0' && **p <= '9') {
            frac = frac * 10.0 + (double)(**p - '0');
            scale *= 10.0;
            (*p)++;
        }
        value += frac / scale;
    }
    if (neg) value = -value;
    return (float)value;
}

// O builder só consome o conjunto público de referências: vetor e rótulo.
// Nenhum payload de teste entra aqui; o arquivo gerado é uma estrutura de busca
// genérica para KNN sobre os vetores de referência.
static void parse_references(const uint8_t *buf, size_t len, float (**vectors_out)[DIMS], uint8_t **labels_out, size_t *n_out) {
    const char vector_key[] = "\"vector\":[";
    const char label_key[] = "\"label\":\"";
    const uint8_t *end = buf + len;
    size_t n = count_vectors(buf, len);
    if (n == 0) {
        fprintf(stderr, "[builder-c] no reference vectors found\n");
        exit(1);
    }

    float (*vectors)[DIMS] = aligned_alloc(32, n * sizeof(*vectors));
    uint8_t *labels = malloc(n);
    if (!vectors || !labels) die("alloc references");

    const uint8_t *p = buf;
    for (size_t i = 0; i < n; i++) {
        void *vh = memmem(p, (size_t)(end - p), vector_key, sizeof(vector_key) - 1);
        if (!vh) {
            fprintf(stderr, "[builder-c] vector %zu not found\n", i);
            exit(1);
        }
        p = (const uint8_t *)vh + sizeof(vector_key) - 1;
        for (size_t d = 0; d < DIMS; d++) vectors[i][d] = 0.0f;
        for (size_t d = 0; d < REAL_DIMS; d++) {
            vectors[i][d] = parse_float_ref(&p, end);
            skip_ws(&p, end);
            if (d + 1 < REAL_DIMS && p < end && *p == ',') p++;
        }

        void *lh = memmem(p, (size_t)(end - p), label_key, sizeof(label_key) - 1);
        if (!lh) {
            fprintf(stderr, "[builder-c] label %zu not found\n", i);
            exit(1);
        }
        p = (const uint8_t *)lh + sizeof(label_key) - 1;
        labels[i] = (p + 5 <= end && memcmp(p, "fraud", 5) == 0) ? 1u : 0u;
    }

    *vectors_out = vectors;
    *labels_out = labels;
    *n_out = n;
}

static void *assign_worker(void *arg) {
    assign_job_t *job = arg;
    for (size_t i = job->start; i < job->end; i++) {
        uint32_t best = 0;
        float best_d = FLT_MAX;
        for (size_t c = 0; c < job->k; c++) {
            float d = sqdist_f32(job->vectors[i], job->centroids[c]);
            if (d < best_d) {
                best_d = d;
                best = (uint32_t)c;
            }
        }
        job->assign[i] = best;
    }
    return NULL;
}

static size_t builder_threads(void) {
    const char *v = getenv("BUILDER_THREADS");
    if (v && *v) {
        long n = strtol(v, NULL, 10);
        if (n > 0) return (size_t)n;
    }
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    return cpus > 0 ? (size_t)cpus : 1u;
}

static void assign_all(const float (*vectors)[DIMS], size_t n, const float (*centroids)[DIMS], size_t k, uint32_t *assign) {
    size_t threads = builder_threads();
    if (threads > n) threads = n;
    if (threads == 0) threads = 1;

    pthread_t *tids = malloc(threads * sizeof(*tids));
    assign_job_t *jobs = calloc(threads, sizeof(*jobs));
    if (!tids || !jobs) die("alloc assign jobs");

    size_t chunk = (n + threads - 1) / threads;
    for (size_t t = 0; t < threads; t++) {
        size_t start = t * chunk;
        size_t end = start + chunk;
        if (end > n) end = n;
        jobs[t] = (assign_job_t){
            .vectors = vectors,
            .centroids = centroids,
            .assign = assign,
            .start = start,
            .end = end,
            .k = k,
        };
        if (pthread_create(&tids[t], NULL, assign_worker, &jobs[t]) != 0) die("pthread_create");
    }
    for (size_t t = 0; t < threads; t++) {
        if (pthread_join(tids[t], NULL) != 0) die("pthread_join");
    }
    free(tids);
    free(jobs);
}

// K-means é feito no build da imagem, fora do limite de CPU do runtime. A
// inicialização determinística torna o index.bin reproduzível entre máquinas.
static float (*build_centroids(const float (*vectors)[DIMS], size_t n, size_t k, size_t iters))[DIMS] {
    float (*centroids)[DIMS] = aligned_alloc(32, k * sizeof(*centroids));
    uint32_t *idx = malloc(n * sizeof(*idx));
    uint32_t *assign = malloc(n * sizeof(*assign));
    double (*sums)[DIMS] = calloc(k, sizeof(*sums));
    uint32_t *counts = calloc(k, sizeof(*counts));
    if (!centroids || !idx || !assign || !sums || !counts) die("alloc kmeans");

    for (size_t i = 0; i < n; i++) idx[i] = (uint32_t)i;
    const char *init_mode = getenv("INIT_MODE");
    bool rust_init = !init_mode || !*init_mode || strcmp(init_mode, "rust") == 0;
    uint64_t splitmix_rng = MAGIC_RINH;
    rust_rng_t rust_rng;
    if (rust_init) rust_rng_seed(&rust_rng, MAGIC_RINH);
    fprintf(stderr, "[builder-c] centroid init: %s\n", rust_init ? "rust-std-rng" : "splitmix64");
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = rust_init
            ? (size_t)rust_rng_gen_range_u32(&rust_rng, (uint32_t)(i + 1))
            : (size_t)(splitmix64(&splitmix_rng) % (i + 1));
        uint32_t tmp = idx[i];
        idx[i] = idx[j];
        idx[j] = tmp;
    }
    for (size_t c = 0; c < k; c++) memcpy(centroids[c], vectors[idx[c]], sizeof(centroids[c]));

    for (size_t it = 0; it < iters; it++) {
        assign_all(vectors, n, centroids, k, assign);
        memset(sums, 0, k * sizeof(*sums));
        memset(counts, 0, k * sizeof(*counts));
        for (size_t i = 0; i < n; i++) {
            uint32_t a = assign[i];
            for (size_t d = 0; d < DIMS; d++) sums[a][d] += (double)vectors[i][d];
            counts[a]++;
        }
        for (size_t c = 0; c < k; c++) {
            if (counts[c] > 0) {
                double denom = (double)counts[c];
                for (size_t d = 0; d < DIMS; d++) centroids[c][d] = (float)(sums[c][d] / denom);
            } else {
                size_t r = rust_init
                    ? (size_t)(rust_rng_next_u64(&rust_rng) % (uint64_t)n)
                    : (size_t)(splitmix64(&splitmix_rng) % n);
                memcpy(centroids[c], vectors[r], sizeof(centroids[c]));
            }
        }
        fprintf(stderr, "[builder-c] k-means iter %zu/%zu\n", it + 1, iters);
    }

    free(idx);
    free(assign);
    free(sums);
    free(counts);
    return centroids;
}

static int cmp_sort_item(const void *a, const void *b) {
    const sort_item_t *x = a;
    const sort_item_t *y = b;
    if (x->dist < y->dist) return -1;
    if (x->dist > y->dist) return 1;
    return 0;
}

// Dentro de cada célula, vetores mais próximos do centróide vêm primeiro. Isso
// melhora localidade e tende a preencher o top-k cedo, aumentando os descartes
// por bound durante o scan.
static void sort_cells(float (*vectors)[DIMS], uint8_t *labels, const uint32_t *starts, const uint32_t *counts, const float (*centroids)[DIMS], size_t k) {
    uint32_t max_count = 0;
    for (size_t c = 0; c < k; c++) {
        if (counts[c] > max_count) max_count = counts[c];
    }
    sort_item_t *tmp = malloc((size_t)max_count * sizeof(*tmp));
    if (!tmp && max_count > 0) die("alloc sort tmp");

    for (size_t c = 0; c < k; c++) {
        size_t start = starts[c];
        size_t count = counts[c];
        if (count <= 1) continue;
        for (size_t i = 0; i < count; i++) {
            tmp[i].dist = sqdist_f32(vectors[start + i], centroids[c]);
            memcpy(tmp[i].v, vectors[start + i], sizeof(tmp[i].v));
            tmp[i].label = labels[start + i];
        }
        qsort(tmp, count, sizeof(*tmp), cmp_sort_item);
        for (size_t i = 0; i < count; i++) {
            memcpy(vectors[start + i], tmp[i].v, sizeof(tmp[i].v));
            labels[start + i] = tmp[i].label;
        }
    }
    free(tmp);
}

static void pad_to_align(FILE *f, size_t *pos, size_t align) {
    static const uint8_t zeroes[SECTION_ALIGN] = {0};
    size_t next = (*pos + align - 1u) & ~(align - 1u);
    if (next > *pos) {
        size_t pad = next - *pos;
        if (fwrite(zeroes, 1, pad, f) != pad) die("write pad");
        *pos = next;
    }
}

static void write_u32(FILE *f, uint32_t v, size_t *pos) {
    uint8_t b[4] = {
        (uint8_t)(v & 0xffu),
        (uint8_t)((v >> 8) & 0xffu),
        (uint8_t)((v >> 16) & 0xffu),
        (uint8_t)((v >> 24) & 0xffu),
    };
    if (fwrite(b, 1, sizeof(b), f) != sizeof(b)) die("write u32");
    *pos += sizeof(b);
}

static void write_i16_vec(FILE *f, const int16_t v[DIMS], size_t *pos) {
    uint8_t b[DIMS * 2];
    for (size_t i = 0; i < DIMS; i++) {
        uint16_t x = (uint16_t)v[i];
        b[i * 2] = (uint8_t)(x & 0xffu);
        b[i * 2 + 1] = (uint8_t)((x >> 8) & 0xffu);
    }
    if (fwrite(b, 1, sizeof(b), f) != sizeof(b)) die("write i16 vec");
    *pos += sizeof(b);
}

static bounds_t bounds_for(const float (*vectors)[DIMS], size_t start, size_t count) {
    bounds_t b;
    for (size_t d = 0; d < DIMS; d++) {
        b.min[d] = INT16_MAX;
        b.max[d] = INT16_MIN;
    }
    if (count == 0) {
        memset(&b, 0, sizeof(b));
        return b;
    }
    for (size_t i = 0; i < count; i++) {
        for (size_t d = 0; d < DIMS; d++) {
            int16_t q = quantize_one(vectors[start + i][d]);
            if (q < b.min[d]) b.min[d] = q;
            if (q > b.max[d]) b.max[d] = q;
        }
    }
    return b;
}

// Formato final do index.bin: cabeçalho, centróides, células, bounds, vetores
// quantizados e labels. As seções alinhadas reduzem penalidade em loads SIMD.
static void write_index(const char *path, const float (*centroids)[DIMS], size_t k,
                        const uint32_t *starts, const uint32_t *counts,
                        const float (*vectors)[DIMS], const uint8_t *labels, size_t n) {
    uint32_t *block_starts = calloc(k, sizeof(*block_starts));
    uint32_t *block_counts = calloc(k, sizeof(*block_counts));
    bounds_t *cell_bounds = calloc(k, sizeof(*cell_bounds));
    if (!block_starts || !block_counts || !cell_bounds) die("alloc bounds");

    size_t blocks_len = 0;
    for (size_t c = 0; c < k; c++) {
        block_starts[c] = (uint32_t)blocks_len;
        block_counts[c] = (counts[c] + BLOCK_SIZE - 1u) / BLOCK_SIZE;
        blocks_len += block_counts[c];
    }
    bounds_t *blocks = calloc(blocks_len ? blocks_len : 1, sizeof(*blocks));
    if (!blocks) die("alloc block bounds");

    for (size_t c = 0; c < k; c++) {
        size_t start = starts[c];
        size_t count = counts[c];
        cell_bounds[c] = bounds_for(vectors, start, count);
        for (size_t b = 0; b < block_counts[c]; b++) {
            size_t off = start + b * BLOCK_SIZE;
            size_t left = count - b * BLOCK_SIZE;
            size_t nblock = left > BLOCK_SIZE ? BLOCK_SIZE : left;
            blocks[block_starts[c] + b] = bounds_for(vectors, off, nblock);
        }
    }

    FILE *f = fopen(path, "wb");
    if (!f) die("create index");
    size_t pos = 0;
    write_u32(f, MAGIC_RINH, &pos);
    write_u32(f, INDEX_VERSION, &pos);
    write_u32(f, (uint32_t)n, &pos);
    write_u32(f, (uint32_t)k, &pos);
    write_u32(f, DIMS, &pos);
    pad_to_align(f, &pos, SECTION_ALIGN);

    for (size_t c = 0; c < k; c++) {
        int16_t q[DIMS];
        quantize_i16(centroids[c], q);
        write_i16_vec(f, q, &pos);
    }
    for (size_t c = 0; c < k; c++) {
        write_u32(f, starts[c], &pos);
        write_u32(f, counts[c], &pos);
        write_u32(f, block_starts[c], &pos);
        write_u32(f, block_counts[c], &pos);
    }
    pad_to_align(f, &pos, SECTION_ALIGN);

    for (size_t c = 0; c < k; c++) {
        write_i16_vec(f, cell_bounds[c].min, &pos);
        write_i16_vec(f, cell_bounds[c].max, &pos);
    }
    pad_to_align(f, &pos, SECTION_ALIGN);

    for (size_t b = 0; b < blocks_len; b++) {
        write_i16_vec(f, blocks[b].min, &pos);
        write_i16_vec(f, blocks[b].max, &pos);
    }
    pad_to_align(f, &pos, SECTION_ALIGN);

    for (size_t i = 0; i < n; i++) {
        int16_t q[DIMS];
        quantize_i16(vectors[i], q);
        write_i16_vec(f, q, &pos);
    }
    if (fwrite(labels, 1, n, f) != n) die("write labels");
    if (fclose(f) != 0) die("close index");

    free(block_starts);
    free(block_counts);
    free(cell_bounds);
    free(blocks);
}

static size_t env_usize(const char *key, size_t def) {
    const char *v = getenv(key);
    if (!v || !*v) return def;
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    return end && *end == '\0' && n > 0 ? (size_t)n : def;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: builder <references.json.gz> <output_path>\n");
        return 2;
    }

    size_t raw_len = 0;
    fprintf(stderr, "[builder-c] reading %s\n", argv[1]);
    uint8_t *raw = read_gzip_all(argv[1], &raw_len);

    float (*vectors)[DIMS] = NULL;
    uint8_t *labels = NULL;
    size_t n = 0;
    parse_references(raw, raw_len, &vectors, &labels, &n);
    free(raw);
    fprintf(stderr, "[builder-c] parsed %zu vectors\n", n);

    size_t k = env_usize("CENTROIDS", DEFAULT_CENTROIDS);
    size_t iters = env_usize("KMEANS_ITERS", DEFAULT_ITERS);
    if (k > MAX_CENTROIDS) k = MAX_CENTROIDS;
    if (k > n) k = n;
    fprintf(stderr, "[builder-c] k-means: %zu centroids, %zu iters, %zu threads\n", k, iters, builder_threads());
    float (*centroids)[DIMS] = build_centroids(vectors, n, k, iters);

    uint32_t *assign = malloc(n * sizeof(*assign));
    uint32_t *counts = calloc(k, sizeof(*counts));
    uint32_t *starts = calloc(k, sizeof(*starts));
    uint32_t *cursor = calloc(k, sizeof(*cursor));
    float (*sorted)[DIMS] = aligned_alloc(32, n * sizeof(*sorted));
    uint8_t *sorted_labels = malloc(n);
    if (!assign || !counts || !starts || !cursor || !sorted || !sorted_labels) die("alloc sorted");

    fprintf(stderr, "[builder-c] final assignment\n");
    assign_all(vectors, n, centroids, k, assign);
    for (size_t i = 0; i < n; i++) counts[assign[i]]++;
    uint32_t acc = 0;
    for (size_t c = 0; c < k; c++) {
        starts[c] = acc;
        cursor[c] = acc;
        acc += counts[c];
    }
    for (size_t i = 0; i < n; i++) {
        uint32_t c = assign[i];
        uint32_t pos = cursor[c]++;
        memcpy(sorted[pos], vectors[i], sizeof(sorted[pos]));
        sorted_labels[pos] = labels[i];
    }
    free(vectors);
    free(labels);
    free(assign);
    free(cursor);

    fprintf(stderr, "[builder-c] sorting cells\n");
    sort_cells(sorted, sorted_labels, starts, counts, centroids, k);

    fprintf(stderr, "[builder-c] writing %s\n", argv[2]);
    write_index(argv[2], centroids, k, starts, counts, sorted, sorted_labels, n);
    fprintf(stderr, "[builder-c] done\n");

    free(centroids);
    free(counts);
    free(starts);
    free(sorted);
    free(sorted_labels);
    return 0;
}
