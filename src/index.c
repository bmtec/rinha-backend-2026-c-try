#include "rinha.h"

#include <float.h>
#include <limits.h>
#include <string.h>

typedef struct {
    int64_t dist;
    // Internal vector/centroid index. Avoid naming this "payload" so it cannot
    // be confused with the HTTP transaction payload from the challenge input.
    uint32_t idx;
} pair_i64_u32_t;

typedef struct {
    float dist;
    uint8_t label;
} pair_f32_u8_t;

static uint8_t rerank_candidates(const ivf_index_t *idx, const float vector[DIMS],
                                 const pair_i64_u32_t cand[K_RERANK], size_t filled, uint8_t *bits_out);

static inline uint32_t read_u32(const uint8_t *b, size_t off) {
    uint32_t v;
    memcpy(&v, b + off, sizeof(v));
    return v;
}

static inline size_t align_up(size_t off, size_t align) {
    return (off + align - 1u) & ~(align - 1u);
}

bool index_from_bytes(const uint8_t *data, size_t len, ivf_index_t *out) {
    if (len < HEADER_BYTES) return false;
    uint32_t magic = read_u32(data, 0);
    uint32_t version = read_u32(data, 4);
    size_t num_vectors = read_u32(data, 8);
    size_t num_centroids = read_u32(data, 12);
    size_t dims = read_u32(data, 16);
    if (magic != MAGIC_RINH || version < MIN_SUPPORTED_VERSION || version > INDEX_VERSION || dims != DIMS) return false;

    bool aligned = version >= 4;
    size_t centroids_bytes = num_centroids * DIMS * sizeof(int16_t);
    size_t cells_bytes = num_centroids * sizeof(cell_t);
    size_t vectors_bytes = num_vectors * DIMS * sizeof(int16_t);
    size_t labels_bytes = num_vectors;

    size_t centroids_off = aligned ? align_up(HEADER_BYTES, SECTION_ALIGN) : HEADER_BYTES;
    size_t cells_off = centroids_off + centroids_bytes;
    size_t cells_end = cells_off + cells_bytes;
    if (cells_end > len) return false;

    const cell_t *cells = (const cell_t *)(const void *)(data + cells_off);
    size_t num_blocks = 0;
    for (size_t i = 0; i < num_centroids; i++) {
        size_t start = cells[i].start;
        size_t count = cells[i].count;
        if (start + count > num_vectors) return false;
        size_t end = (size_t)cells[i].block_start + (size_t)cells[i].block_count;
        if (end > num_blocks) num_blocks = end;
    }

    const bounds_t *cell_bounds = NULL;
    size_t blocks_base_off = cells_end;
    if (version >= 5) {
        size_t cell_bounds_off = aligned ? align_up(cells_end, SECTION_ALIGN) : cells_end;
        size_t cell_bounds_end = cell_bounds_off + num_centroids * sizeof(bounds_t);
        if (cell_bounds_end > len) return false;
        cell_bounds = (const bounds_t *)(const void *)(data + cell_bounds_off);
        blocks_base_off = cell_bounds_end;
    }

    size_t blocks_off = aligned ? align_up(blocks_base_off, SECTION_ALIGN) : blocks_base_off;
    size_t blocks_bytes = num_blocks * sizeof(bounds_t);
    size_t vectors_off = aligned ? align_up(blocks_off + blocks_bytes, SECTION_ALIGN) : blocks_off + blocks_bytes;
    size_t labels_off = vectors_off + vectors_bytes;
    if (labels_off + labels_bytes > len) return false;

    out->num_vectors = num_vectors;
    out->num_centroids = num_centroids;
    out->centroids = (const int16_t (*)[DIMS])(const void *)(data + centroids_off);
    out->cells = cells;
    out->cell_bounds = cell_bounds;
    out->blocks = (const bounds_t *)(const void *)(data + blocks_off);
    out->vectors = (const int16_t (*)[DIMS])(const void *)(data + vectors_off);
    out->labels = data + labels_off;
    out->blocks_len = num_blocks;
    return true;
}

static inline bool centroid_less(pair_i64_u32_t a, pair_i64_u32_t b) {
    return a.dist < b.dist || (a.dist == b.dist && a.idx < b.idx);
}

static void sort_centroids(pair_i64_u32_t best[MAX_NPROBE], size_t cap) {
    for (size_t i = 1; i < cap; i++) {
        pair_i64_u32_t item = best[i];
        size_t j = i;
        while (j > 0 && centroid_less(item, best[j - 1])) {
            best[j] = best[j - 1];
            j--;
        }
        best[j] = item;
    }
}

static void worst_centroid(const pair_i64_u32_t best[MAX_NPROBE], size_t cap, size_t *idx, int64_t *dist) {
    size_t worst_idx = 0;
    int64_t worst_dist = best[0].dist;
    uint32_t worst_idx_value = best[0].idx;
    for (size_t i = 1; i < cap; i++) {
        if (best[i].dist > worst_dist || (best[i].dist == worst_dist && best[i].idx > worst_idx_value)) {
            worst_idx = i;
            worst_dist = best[i].dist;
            worst_idx_value = best[i].idx;
        }
    }
    *idx = worst_idx;
    *dist = worst_dist;
}

static void fill_best_centroids(const int64_t *cdist, size_t len, size_t cap, pair_i64_u32_t best[MAX_NPROBE]) {
    cap = cap > MAX_NPROBE ? MAX_NPROBE : cap;
    if (cap > len) cap = len;
    if (cap == 0) return;
    size_t worst_idx = 0;
    int64_t worst_dist = INT64_MIN;
    for (size_t i = 0; i < cap; i++) {
        best[i].dist = cdist[i];
        best[i].idx = (uint32_t)i;
        if (cdist[i] >= worst_dist) {
            worst_idx = i;
            worst_dist = cdist[i];
        }
    }
    for (size_t i = cap; i < len; i++) {
        if (cdist[i] < worst_dist) {
            best[worst_idx].dist = cdist[i];
            best[worst_idx].idx = (uint32_t)i;
            worst_centroid(best, cap, &worst_idx, &worst_dist);
        }
    }
    sort_centroids(best, cap);
}

static inline void insert_cand(pair_i64_u32_t arr[K_RERANK], size_t *len, int64_t dist, uint32_t idx) {
    if (*len < K_RERANK) {
        size_t i = *len;
        while (i > 0 && arr[i - 1].dist > dist) {
            arr[i] = arr[i - 1];
            i--;
        }
        arr[i].dist = dist;
        arr[i].idx = idx;
        (*len)++;
    } else if (dist < arr[K_RERANK - 1].dist) {
        size_t i = K_RERANK - 1;
        while (i > 0 && arr[i - 1].dist > dist) {
            arr[i] = arr[i - 1];
            i--;
        }
        arr[i].dist = dist;
        arr[i].idx = idx;
    }
}

static inline void insert_topk_f32(pair_f32_u8_t arr[KNN_K], size_t *len, float dist, uint8_t label) {
    if (*len < KNN_K) {
        size_t i = *len;
        while (i > 0 && arr[i - 1].dist > dist) {
            arr[i] = arr[i - 1];
            i--;
        }
        arr[i].dist = dist;
        arr[i].label = label;
        (*len)++;
    } else if (dist < arr[KNN_K - 1].dist) {
        size_t i = KNN_K - 1;
        while (i > 0 && arr[i - 1].dist > dist) {
            arr[i] = arr[i - 1];
            i--;
        }
        arr[i].dist = dist;
        arr[i].label = label;
    }
}

static void scan_centroid(const ivf_index_t *idx, const int16_t q[DIMS], size_t centroid,
                          pair_i64_u32_t cand[K_RERANK], size_t *filled, int64_t scratch[SCAN_CHUNK]) {
    cell_t cell = idx->cells[centroid];
    size_t start = cell.start;
    size_t count = cell.count;
    if (start + count > idx->num_vectors) return;
    size_t block_start = cell.block_start;
    size_t block_count = cell.block_count;
    if (block_start + block_count > idx->blocks_len) return;

    for (size_t rel_block = 0; rel_block < block_count; rel_block++) {
        size_t local_off = rel_block * BLOCK_SIZE;
        if (local_off >= count) break;
        size_t n = count - local_off;
        if (n > BLOCK_SIZE) n = BLOCK_SIZE;
        if (n > SCAN_CHUNK) n = SCAN_CHUNK;

        if (*filled == K_RERANK) {
            int64_t bound = lower_bound_block_i16(q, &idx->blocks[block_start + rel_block]);
            if (bound >= cand[K_RERANK - 1].dist) continue;
        }

        size_t off = start + local_off;
        distances_to_slice_i16(q, idx->vectors + off, n, scratch);
        for (size_t j = 0; j < n; j++) {
            int64_t d = scratch[j];
            if (*filled < K_RERANK || d < cand[K_RERANK - 1].dist) {
                insert_cand(cand, filled, d, (uint32_t)(off + j));
            }
        }
    }
}

static void scan_centroid_range(const ivf_index_t *idx, const int16_t q[DIMS],
                                const pair_i64_u32_t best[MAX_NPROBE], size_t from, size_t to,
                                pair_i64_u32_t cand[K_RERANK], size_t *filled, int64_t scratch[SCAN_CHUNK]) {
    for (size_t b = from; b < to; b++) {
        scan_centroid(idx, q, best[b].idx, cand, filled, scratch);
    }
}

static inline void mark_centroid(uint64_t words[MAX_CENTROIDS / 64], size_t c) {
    words[c >> 6] |= 1ull << (c & 63u);
}

static inline bool centroid_marked(const uint64_t words[MAX_CENTROIDS / 64], size_t c) {
    return (words[c >> 6] & (1ull << (c & 63u))) != 0;
}

static void insert_repair_cell(pair_i64_u32_t arr[MAX_REPAIR_CANDIDATES], size_t *len, size_t cap, int64_t dist, uint32_t idx) {
    if (cap == 0) return;
    if (*len < cap) {
        size_t i = *len;
        while (i > 0 && arr[i - 1].dist > dist) {
            arr[i] = arr[i - 1];
            i--;
        }
        arr[i].dist = dist;
        arr[i].idx = idx;
        (*len)++;
    } else if (dist < arr[cap - 1].dist) {
        size_t i = cap - 1;
        while (i > 0 && arr[i - 1].dist > dist) {
            arr[i] = arr[i - 1];
            i--;
        }
        arr[i].dist = dist;
        arr[i].idx = idx;
    }
}

static uint8_t repair_by_cell_bounds(const ivf_index_t *idx, const int16_t q[DIMS], const float v[DIMS],
                                     const pair_i64_u32_t best[MAX_NPROBE], size_t scanned_count,
                                     pair_i64_u32_t cand[K_RERANK], size_t *filled, int64_t scratch[SCAN_CHUNK],
                                     size_t repair_candidates) {
    if (!idx->cell_bounds || repair_candidates == 0) return rerank_candidates(idx, v, cand, *filled, NULL);
    size_t nc = idx->num_centroids < MAX_CENTROIDS ? idx->num_centroids : MAX_CENTROIDS;
    if (repair_candidates > MAX_REPAIR_CANDIDATES) repair_candidates = MAX_REPAIR_CANDIDATES;

    uint64_t scanned[MAX_CENTROIDS / 64] = {0};
    for (size_t i = 0; i < scanned_count; i++) mark_centroid(scanned, best[i].idx);

    pair_i64_u32_t repairs[MAX_REPAIR_CANDIDATES];
    for (size_t i = 0; i < repair_candidates; i++) {
        repairs[i].dist = INT64_MAX;
        repairs[i].idx = 0;
    }
    size_t repair_len = 0;
    for (size_t c = 0; c < nc; c++) {
        if (centroid_marked(scanned, c)) continue;
        int64_t bound = lower_bound_block_i16(q, &idx->cell_bounds[c]);
        insert_repair_cell(repairs, &repair_len, repair_candidates, bound, (uint32_t)c);
    }
    for (size_t i = 0; i < repair_len; i++) {
        scan_centroid(idx, q, repairs[i].idx, cand, filled, scratch);
    }
    return rerank_candidates(idx, v, cand, *filled, NULL);
}

static uint8_t rerank_candidates(const ivf_index_t *idx, const float vector[DIMS],
                                 const pair_i64_u32_t cand[K_RERANK], size_t filled, uint8_t *bits_out) {
    pair_f32_u8_t top[KNN_K];
    for (size_t i = 0; i < KNN_K; i++) {
        top[i].dist = FLT_MAX;
        top[i].label = 0;
    }
    size_t tfill = 0;
    for (size_t c = 0; c < filled; c++) {
        size_t vi = cand[c].idx;
        const int16_t *vec = idx->vectors[vi];
        float d = 0.0f;
        for (size_t i = 0; i < DIMS; i++) {
            float x = (float)vec[i] / SCALE_F;
            float e = vector[i] - x;
            d += e * e;
        }
        if (tfill < KNN_K || d < top[KNN_K - 1].dist) {
            insert_topk_f32(top, &tfill, d, idx->labels[vi]);
        }
    }

    uint8_t fraud = 0;
    uint8_t bits = 0;
    for (size_t i = 0; i < tfill; i++) {
        if (top[i].label == 1) {
            fraud++;
            bits |= (uint8_t)(1u << i);
        }
    }
    if (bits_out) *bits_out = bits;
    return fraud;
}

static inline bool is_ambiguous(uint8_t fraud, uint8_t min, uint8_t max) {
    return fraud >= min && fraud <= max;
}

uint8_t index_query_quantized(const ivf_index_t *idx, const float v[DIMS], const int16_t q[DIMS], query_options_t opts) {
    size_t nc = idx->num_centroids < MAX_CENTROIDS ? idx->num_centroids : MAX_CENTROIDS;
    size_t probe = opts.nprobe;
    if (probe < 1) probe = 1;
    if (probe > MAX_NPROBE) probe = MAX_NPROBE;
    if (probe > nc) probe = nc;
    size_t adaptive_probe = opts.repair_probe;
    if (adaptive_probe < probe) adaptive_probe = probe;
    if (adaptive_probe > MAX_NPROBE) adaptive_probe = MAX_NPROBE;
    if (adaptive_probe > nc) adaptive_probe = nc;
    size_t initial_probe = probe;

    int64_t cdist[MAX_CENTROIDS];
    pair_i64_u32_t best[MAX_NPROBE];
    pair_i64_u32_t cand[K_RERANK];
    int64_t scratch[SCAN_CHUNK];

    for (size_t i = 0; i < MAX_NPROBE; i++) {
        best[i].dist = INT64_MAX;
        best[i].idx = 0;
    }
    for (size_t i = 0; i < K_RERANK; i++) {
        cand[i].dist = INT64_MAX;
        cand[i].idx = 0;
    }

    distances_to_slice_i16(q, idx->centroids, nc, cdist);
    fill_best_centroids(cdist, nc, initial_probe, best);

    size_t filled = 0;
    scan_centroid_range(idx, q, best, 0, probe, cand, &filled, scratch);
    uint8_t fraud = rerank_candidates(idx, v, cand, filled, NULL);

    if (adaptive_probe > probe && is_ambiguous(fraud, opts.repair_min, opts.repair_max)) {
        fill_best_centroids(cdist, nc, adaptive_probe, best);
        scan_centroid_range(idx, q, best, probe, adaptive_probe, cand, &filled, scratch);
        fraud = rerank_candidates(idx, v, cand, filled, NULL);
    }

    if (opts.repair_candidates > 0 && is_ambiguous(fraud, opts.repair_min, opts.repair_max)) {
        fraud = repair_by_cell_bounds(
            idx,
            q,
            v,
            best,
            adaptive_probe,
            cand,
            &filled,
            scratch,
            opts.repair_candidates
        );
    }

    return fraud;
}
