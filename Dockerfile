# syntax=docker/dockerfile:1.6
#
# Runtime em C. O índice é gerado durante o build a partir das referências
# públicas do desafio, deixando o container final pronto para atender requisições.

FROM --platform=linux/amd64 gcc:14-bookworm AS build
WORKDIR /app
RUN apt-get update \
 && apt-get install -y --no-install-recommends zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*
COPY Makefile ./
COPY src ./src
RUN make

FROM --platform=linux/amd64 build AS index
ARG CENTROIDS=2048
ARG KMEANS_ITERS=15
ARG BUILDER_THREADS=0
ARG INIT_MODE=rust
COPY resources/references.json.gz /resources/references.json.gz
# Gera /output/index.bin offline. Esta etapa pode gastar CPU/memória porque não
# roda no ambiente limitado da submissão; as APIs só fazem mmap do resultado.
RUN mkdir -p /output \
 && CENTROIDS="$CENTROIDS" KMEANS_ITERS="$KMEANS_ITERS" BUILDER_THREADS="$BUILDER_THREADS" INIT_MODE="$INIT_MODE" \
    /app/out/builder /resources/references.json.gz /output/index.bin

FROM --platform=linux/amd64 debian:bookworm-slim AS api
LABEL org.opencontainers.image.source="https://github.com/bmtec/rinha-backend-2026-c-try" \
      org.opencontainers.image.licenses="MIT"
COPY --from=build /app/out/api /usr/local/bin/api
COPY --from=build /app/out/lb /usr/local/bin/lb
COPY --from=index /output/index.bin /data/index.bin
RUN mkdir -p /sockets
ENV INDEX_PATH=/data/index.bin \
    NPROBE=10 \
    REPAIR_PROBE=48 \
    REPAIR_CANDIDATES=0 \
    REPAIR_MIN=1 \
    REPAIR_MAX=4
