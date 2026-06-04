# syntax=docker/dockerfile:1.6
#
# C runtime lab. The index is copied from the known-good Rust image so this
# repo can isolate API/LB runtime cost without rebuilding the dataset pipeline.

FROM --platform=linux/amd64 gcc:14-bookworm AS build
WORKDIR /app
COPY Makefile ./
COPY src ./src
RUN make

FROM --platform=linux/amd64 ghcr.io/bmtec/rinha-backend-2026:0e80820d6664ca744e45f8896650faf44caccb3c AS index

FROM --platform=linux/amd64 debian:bookworm-slim AS api
LABEL org.opencontainers.image.source="https://github.com/bmtec/rinha-backend-2026-c-try" \
      org.opencontainers.image.licenses="MIT"
COPY --from=build /app/out/api /usr/local/bin/api
COPY --from=build /app/out/lb /usr/local/bin/lb
COPY --from=index /data/index.bin /data/index.bin
RUN mkdir -p /sockets
ENV INDEX_PATH=/data/index.bin NPROBE=10
