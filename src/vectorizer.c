#include "rinha.h"

#include <math.h>
#include <string.h>

static inline float clamp01(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

static inline int d2(const uint8_t *b, int i) {
    return (int)(b[i] - '0') * 10 + (int)(b[i + 1] - '0');
}

static inline int d4(const uint8_t *b, int i) {
    return (int)(b[i] - '0') * 1000 + (int)(b[i + 1] - '0') * 100 +
           (int)(b[i + 2] - '0') * 10 + (int)(b[i + 3] - '0');
}

typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int minute;
} parsed_ts_t;

static inline parsed_ts_t parse_ts(const uint8_t ts[20]) {
    parsed_ts_t p = {
        .year = d4(ts, 0),
        .month = d2(ts, 5),
        .day = d2(ts, 8),
        .hour = d2(ts, 11),
        .minute = d2(ts, 14),
    };
    return p;
}

static inline int sakamoto(int y, int m, int d) {
    static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    int r = (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
    return r < 0 ? r + 7 : r;
}

static inline int weekday_mon0(parsed_ts_t p) {
    return (sakamoto(p.year, p.month, p.day) + 6) % 7;
}

static inline int64_t days_from_civil(int y0, int m0, int d) {
    int64_t y = y0;
    int64_t m = m0;
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    uint64_t yoe = (uint64_t)(y - era * 400);
    uint64_t doy = (153 * (uint64_t)(m > 2 ? m - 3 : m + 9) + 2) / 5 + (uint64_t)d - 1;
    uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static inline int64_t minute_of_day(parsed_ts_t p) {
    return (int64_t)p.hour * 60 + p.minute;
}

static inline bool same_date(parsed_ts_t a, parsed_ts_t b) {
    return a.year == b.year && a.month == b.month && a.day == b.day;
}

static inline int64_t total_minutes(parsed_ts_t p) {
    return days_from_civil(p.year, p.month, p.day) * 1440 + minute_of_day(p);
}

static inline int64_t minutes_diff(parsed_ts_t current, parsed_ts_t last) {
    if (same_date(current, last)) return minute_of_day(current) - minute_of_day(last);
    return total_minutes(current) - total_minutes(last);
}

static inline bool is_unknown_merchant(const payload_t *p) {
    for (size_t i = 0; i < p->known_merchants_len; i++) {
        if (p->known_merchants[i].len == p->merchant_id.len &&
            memcmp(p->known_merchants[i].ptr, p->merchant_id.ptr, p->merchant_id.len) == 0) {
            return false;
        }
    }
    return true;
}

static inline float mcc_risk(const uint8_t mcc[4]) {
    uint32_t x = ((uint32_t)mcc[0] << 24) | ((uint32_t)mcc[1] << 16) | ((uint32_t)mcc[2] << 8) | (uint32_t)mcc[3];
    switch (x) {
        case ((uint32_t)'5' << 24) | ((uint32_t)'4' << 16) | ((uint32_t)'1' << 8) | (uint32_t)'1': return 0.15f;
        case ((uint32_t)'5' << 24) | ((uint32_t)'8' << 16) | ((uint32_t)'1' << 8) | (uint32_t)'2': return 0.30f;
        case ((uint32_t)'5' << 24) | ((uint32_t)'9' << 16) | ((uint32_t)'1' << 8) | (uint32_t)'2': return 0.20f;
        case ((uint32_t)'5' << 24) | ((uint32_t)'9' << 16) | ((uint32_t)'4' << 8) | (uint32_t)'4': return 0.45f;
        case ((uint32_t)'7' << 24) | ((uint32_t)'8' << 16) | ((uint32_t)'0' << 8) | (uint32_t)'1': return 0.80f;
        case ((uint32_t)'7' << 24) | ((uint32_t)'8' << 16) | ((uint32_t)'0' << 8) | (uint32_t)'2': return 0.75f;
        case ((uint32_t)'7' << 24) | ((uint32_t)'9' << 16) | ((uint32_t)'9' << 8) | (uint32_t)'5': return 0.85f;
        case ((uint32_t)'4' << 24) | ((uint32_t)'5' << 16) | ((uint32_t)'1' << 8) | (uint32_t)'1': return 0.35f;
        case ((uint32_t)'5' << 24) | ((uint32_t)'3' << 16) | ((uint32_t)'1' << 8) | (uint32_t)'1': return 0.25f;
        case ((uint32_t)'5' << 24) | ((uint32_t)'9' << 16) | ((uint32_t)'9' << 8) | (uint32_t)'9': return 0.50f;
        default: return 0.50f;
    }
}

// Mantemos o vetor em float para o rerank final e em int16_t para o scan rápido.
// As dimensões seguem a especificação do desafio; o valor -1 em dimensões sem
// last_transaction é preservado porque também aparece nas referências.
static inline void set_dim(float v[DIMS], int16_t q[DIMS], int idx, float value) {
    v[idx] = value;
    q[idx] = quantize_one(value);
}

// Converte a transação recebida em vetor. Essa é a única entrada de negócio da
// consulta; a decisão final vem dos vizinhos no índice de referências.
void vectorize_quantized(const payload_t *p, float v[DIMS], int16_t q[DIMS]) {
    for (int i = 0; i < DIMS; i++) {
        v[i] = 0.0f;
        q[i] = 0;
    }
    parsed_ts_t requested = parse_ts(p->requested_at);

    set_dim(v, q, 0, clamp01(p->amount / MAX_AMOUNT));
    set_dim(v, q, 1, clamp01((float)p->installments / MAX_INSTALLMENTS));

    float amount_vs_avg = p->avg_amount > 0.0f
        ? clamp01((p->amount / p->avg_amount) / AMOUNT_VS_AVG_RATIO)
        : 0.0f;
    set_dim(v, q, 2, amount_vs_avg);

    set_dim(v, q, 3, (float)requested.hour / 23.0f);
    set_dim(v, q, 4, (float)weekday_mon0(requested) / 6.0f);

    if (p->has_last_transaction) {
        if (p->has_last_ts) {
            float diff = (float)minutes_diff(requested, parse_ts(p->last_tx_timestamp));
            set_dim(v, q, 5, clamp01(diff / MAX_MINUTES));
        } else {
            set_dim(v, q, 5, -1.0f);
        }
        set_dim(v, q, 6, p->has_km_current ? clamp01(p->km_from_current / MAX_KM) : -1.0f);
    } else {
        set_dim(v, q, 5, -1.0f);
        set_dim(v, q, 6, -1.0f);
    }

    set_dim(v, q, 7, clamp01(p->km_from_home / MAX_KM));
    set_dim(v, q, 8, clamp01((float)p->tx_count_24h / MAX_TX_COUNT_24H));
    set_dim(v, q, 9, p->is_online ? 1.0f : 0.0f);
    set_dim(v, q, 10, p->card_present ? 1.0f : 0.0f);
    set_dim(v, q, 11, is_unknown_merchant(p) ? 1.0f : 0.0f);
    set_dim(v, q, 12, mcc_risk(p->mcc));
    set_dim(v, q, 13, clamp01(p->merchant_avg_amount / MAX_MERCHANT_AVG_AMOUNT));
}
