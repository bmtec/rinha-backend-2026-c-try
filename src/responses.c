#include "rinha.h"

// O fraud_score só pode assumir 6 valores porque vem de 5 vizinhos. Montar a
// resposta HTTP inteira de antemão evita snprintf, heap e cálculo de tamanho no
// hot path.
static const uint8_t R0[] = "HTTP/1.1 200 OK\r\nContent-Length:35\r\n\r\n{\"approved\":true,\"fraud_score\":0.0}";
static const uint8_t R1[] = "HTTP/1.1 200 OK\r\nContent-Length:35\r\n\r\n{\"approved\":true,\"fraud_score\":0.2}";
static const uint8_t R2[] = "HTTP/1.1 200 OK\r\nContent-Length:35\r\n\r\n{\"approved\":true,\"fraud_score\":0.4}";
static const uint8_t R3[] = "HTTP/1.1 200 OK\r\nContent-Length:36\r\n\r\n{\"approved\":false,\"fraud_score\":0.6}";
static const uint8_t R4[] = "HTTP/1.1 200 OK\r\nContent-Length:36\r\n\r\n{\"approved\":false,\"fraud_score\":0.8}";
static const uint8_t R5[] = "HTTP/1.1 200 OK\r\nContent-Length:36\r\n\r\n{\"approved\":false,\"fraud_score\":1.0}";
static const uint8_t READY[] = "HTTP/1.1 200 OK\r\nContent-Length:0\r\n\r\n";
static const uint8_t NOTFOUND[] = "HTTP/1.1 404 Not Found\r\nContent-Length:0\r\n\r\n";

static const uint8_t *RESP[] = {R0, R1, R2, R3, R4, R5};
static const size_t RESP_LEN[] = {
    sizeof(R0) - 1, sizeof(R1) - 1, sizeof(R2) - 1,
    sizeof(R3) - 1, sizeof(R4) - 1, sizeof(R5) - 1,
};

const uint8_t *full_response(size_t fraud_count, size_t *len) {
    if (fraud_count > 5) fraud_count = 0;
    *len = RESP_LEN[fraud_count];
    return RESP[fraud_count];
}

const uint8_t *ready_response(size_t *len) {
    *len = sizeof(READY) - 1;
    return READY;
}

const uint8_t *notfound_response(size_t *len) {
    *len = sizeof(NOTFOUND) - 1;
    return NOTFOUND;
}
