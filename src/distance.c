#include "rinha.h"

#include <immintrin.h>
#include <math.h>

// O índice usa int16_t para reduzir memória e tráfego de cache. A escala 10000
// mantém a precisão relevante dos vetores normalizados e permite distância AVX2
// com _mm256_madd_epi16 no scan quente.
int16_t quantize_one(float v) {
    float s = nearbyintf(v * SCALE_F);
    if (s < -32767.0f) s = -32767.0f;
    if (s > 32767.0f) s = 32767.0f;
    return (int16_t)s;
}

void quantize_i16(const float v[DIMS], int16_t q[DIMS]) {
    for (int i = 0; i < DIMS; i++) q[i] = quantize_one(v[i]);
}

static inline int64_t hsum_madd_i64(__m256i d) {
    __m256i madd = _mm256_madd_epi16(d, d);
    __m256i lo = _mm256_cvtepi32_epi64(_mm256_castsi256_si128(madd));
    __m256i hi = _mm256_cvtepi32_epi64(_mm256_extracti128_si256(madd, 1));
    __m256i sum = _mm256_add_epi64(lo, hi);
    __m128i sum_lo = _mm256_castsi256_si128(sum);
    __m128i sum_hi = _mm256_extracti128_si256(sum, 1);
    __m128i pair = _mm_add_epi64(sum_lo, sum_hi);
    __m128i pair_hi = _mm_unpackhi_epi64(pair, pair);
    return _mm_cvtsi128_si64(_mm_add_epi64(pair, pair_hi));
}

int64_t squared_euclidean_i16(const int16_t a[DIMS], const int16_t b[DIMS]) {
    __m256i av = _mm256_loadu_si256((const __m256i *)a);
    __m256i bv = _mm256_loadu_si256((const __m256i *)b);
    __m256i d = _mm256_sub_epi16(av, bv);
    return hsum_madd_i64(d);
}

// Calcula um bloco inteiro contra a mesma query. Prefetch ajuda quando a célula
// tem muitos vetores contíguos; manter os vetores agrupados por célula torna
// esse acesso previsível para cache.
void distances_to_slice_i16(const int16_t query[DIMS], const int16_t (*vectors)[DIMS], size_t len, int64_t *out) {
    const size_t prefetch_ahead = 4;
    __m256i qv = _mm256_loadu_si256((const __m256i *)query);
    bool aligned = (((uintptr_t)vectors) & 31u) == 0;
    for (size_t i = 0; i < len; i++) {
        if (i + prefetch_ahead < len) {
            _mm_prefetch((const char *)(vectors + i + prefetch_ahead), _MM_HINT_T0);
        }
        const __m256i *ptr = (const __m256i *)vectors[i];
        __m256i bv = aligned ? _mm256_load_si256(ptr) : _mm256_loadu_si256(ptr);
        __m256i d = _mm256_sub_epi16(qv, bv);
        out[i] = hsum_madd_i64(d);
    }
}

// Lower bound por caixa do bloco: se a query já está dentro do intervalo de uma
// dimensão, aquela dimensão contribui zero. Isso permite pular blocos que não
// conseguem melhorar o top-k atual.
int64_t lower_bound_block_i16(const int16_t q[DIMS], const bounds_t *block) {
    __m256i qv = _mm256_loadu_si256((const __m256i *)q);
    __m256i lo = _mm256_loadu_si256((const __m256i *)block->min);
    __m256i hi = _mm256_loadu_si256((const __m256i *)block->max);

    __m256i below = _mm256_cmpgt_epi16(lo, qv);
    __m256i above = _mm256_cmpgt_epi16(qv, hi);
    __m256i below_diff = _mm256_sub_epi16(lo, qv);
    __m256i above_diff = _mm256_sub_epi16(qv, hi);
    __m256i diff = _mm256_or_si256(
        _mm256_and_si256(below, below_diff),
        _mm256_and_si256(above, above_diff)
    );
    return hsum_madd_i64(diff);
}
