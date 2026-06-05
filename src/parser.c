#include "rinha.h"

#include <ctype.h>
#include <string.h>

static inline size_t skip_ws(const uint8_t *b, size_t len, size_t i) {
    while (i < len) {
        uint8_t c = b[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            i++;
        } else {
            break;
        }
    }
    return i;
}

static bool find_value(const uint8_t *b, size_t len, size_t base, const char *key, size_t *out) {
    size_t klen = strlen(key);
    if (base >= len) return false;
    void *p = memmem(b + base, len - base, key, klen);
    if (!p) return false;
    *out = (size_t)((const uint8_t *)p - b) + klen;
    return true;
}

static bool parse_f32_at(const uint8_t *b, size_t len, size_t i, float *out) {
    i = skip_ws(b, len, i);
    bool neg = false;
    if (i < len && b[i] == '-') {
        neg = true;
        i++;
    }
    float int_part = 0.0f;
    bool saw = false;
    while (i < len && b[i] >= '0' && b[i] <= '9') {
        int_part = int_part * 10.0f + (float)(b[i] - '0');
        i++;
        saw = true;
    }
    float value = int_part;
    if (i < len && b[i] == '.') {
        i++;
        float frac = 0.0f;
        float scale = 1.0f;
        while (i < len && b[i] >= '0' && b[i] <= '9') {
            frac = frac * 10.0f + (float)(b[i] - '0');
            scale *= 10.0f;
            i++;
            saw = true;
        }
        value += frac / scale;
    }
    if (!saw) return false;
    *out = neg ? -value : value;
    return true;
}

static bool parse_u32_at(const uint8_t *b, size_t len, size_t i, uint32_t *out) {
    i = skip_ws(b, len, i);
    uint32_t v = 0;
    bool saw = false;
    while (i < len && b[i] >= '0' && b[i] <= '9') {
        v = v * 10u + (uint32_t)(b[i] - '0');
        i++;
        saw = true;
    }
    if (!saw) return false;
    *out = v;
    return true;
}

static bool parse_bool_at(const uint8_t *b, size_t len, size_t i, bool *out) {
    i = skip_ws(b, len, i);
    if (i + 4 <= len && memcmp(b + i, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (i + 5 <= len && memcmp(b + i, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_string_at(const uint8_t *b, size_t len, size_t i, slice_t *out, size_t *next) {
    i = skip_ws(b, len, i);
    if (i >= len || b[i] != '"') return false;
    size_t start = i + 1;
    const uint8_t *end = memchr(b + start, '"', len - start);
    if (!end) return false;
    out->ptr = b + start;
    out->len = (size_t)(end - (b + start));
    if (next) *next = (size_t)(end - b) + 1;
    return true;
}

static bool copy_fixed(slice_t s, uint8_t *out, size_t n) {
    if (s.len < n) return false;
    memcpy(out, s.ptr, n);
    return true;
}

static bool parse_string_array(const uint8_t *b, size_t len, size_t i, payload_t *out) {
    i = skip_ws(b, len, i);
    if (i >= len || b[i] != '[') return false;
    i++;
    out->known_merchants_len = 0;
    for (;;) {
        i = skip_ws(b, len, i);
        if (i >= len) return false;
        if (b[i] == ']') return true;
        if (b[i] == ',') {
            i++;
            continue;
        }
        slice_t s;
        size_t next;
        if (!parse_string_at(b, len, i, &s, &next)) return false;
        if (out->known_merchants_len < MAX_MERCHANTS) {
            out->known_merchants[out->known_merchants_len++] = s;
        }
        i = next;
    }
}

// Parser especializado para o formato do desafio. Ele extrai somente os campos
// necessários para montar o vetor; não mantém cópia do JSON inteiro e não usa
// lookup por payload de teste.
bool parse_payload(const uint8_t *buf, size_t len, payload_t *out) {
    memset(out, 0, sizeof(*out));

    const uint8_t *txp = memmem(buf, len, "\"transaction\":", 14);
    const uint8_t *custp = memmem(buf, len, "\"customer\":", 11);
    const uint8_t *merchp = memmem(buf, len, "\"merchant\":", 11);
    const uint8_t *termp = memmem(buf, len, "\"terminal\":", 11);
    const uint8_t *lastp = memmem(buf, len, "\"last_transaction\":", 19);
    if (!txp || !custp || !merchp || !termp || !lastp) return false;

    size_t tx = (size_t)(txp - buf);
    size_t cust = (size_t)(custp - buf);
    size_t merch = (size_t)(merchp - buf);
    size_t term = (size_t)(termp - buf);
    size_t last = (size_t)(lastp - buf);
    size_t p;
    slice_t s;

    if (!find_value(buf, len, tx, "\"amount\":", &p) || !parse_f32_at(buf, len, p, &out->amount)) return false;
    if (!find_value(buf, len, tx, "\"installments\":", &p) || !parse_u32_at(buf, len, p, &out->installments)) return false;
    if (!find_value(buf, len, tx, "\"requested_at\":", &p) || !parse_string_at(buf, len, p, &s, NULL) || !copy_fixed(s, out->requested_at, 20)) return false;

    if (!find_value(buf, len, cust, "\"avg_amount\":", &p) || !parse_f32_at(buf, len, p, &out->avg_amount)) return false;
    if (!find_value(buf, len, cust, "\"tx_count_24h\":", &p) || !parse_u32_at(buf, len, p, &out->tx_count_24h)) return false;
    if (!find_value(buf, len, cust, "\"known_merchants\":", &p) || !parse_string_array(buf, len, p, out)) return false;

    if (!find_value(buf, len, merch, "\"id\":", &p) || !parse_string_at(buf, len, p, &out->merchant_id, NULL)) return false;
    if (!find_value(buf, len, merch, "\"mcc\":", &p) || !parse_string_at(buf, len, p, &s, NULL) || !copy_fixed(s, out->mcc, 4)) return false;
    if (!find_value(buf, len, merch, "\"avg_amount\":", &p) || !parse_f32_at(buf, len, p, &out->merchant_avg_amount)) return false;

    if (!find_value(buf, len, term, "\"is_online\":", &p) || !parse_bool_at(buf, len, p, &out->is_online)) return false;
    if (!find_value(buf, len, term, "\"card_present\":", &p) || !parse_bool_at(buf, len, p, &out->card_present)) return false;
    if (!find_value(buf, len, term, "\"km_from_home\":", &p) || !parse_f32_at(buf, len, p, &out->km_from_home)) return false;

    size_t last_val = skip_ws(buf, len, last + 19);
    if (last_val + 4 <= len && memcmp(buf + last_val, "null", 4) == 0) {
        out->has_last_transaction = false;
        return true;
    }

    out->has_last_transaction = true;
    if (!find_value(buf, len, last, "\"timestamp\":", &p) || !parse_string_at(buf, len, p, &s, NULL) || !copy_fixed(s, out->last_tx_timestamp, 20)) return false;
    out->has_last_ts = true;
    if (!find_value(buf, len, last, "\"km_from_current\":", &p) || !parse_f32_at(buf, len, p, &out->km_from_current)) return false;
    out->has_km_current = true;
    return true;
}
