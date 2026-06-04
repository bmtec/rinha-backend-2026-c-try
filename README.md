# Rinha Backend 2026 C Lab

Runtime-only C experiment based on the current bmtec topology:

```text
client -> C LB (:9999) -> SCM_RIGHTS fd handoff -> C api1/api2 -> mmap index.bin
```

The lab intentionally copies `index.bin` from the known-good public image and
ports only the hot runtime path to C. This keeps measurements focused on C
networking, parsing, vectorization, and index query cost.

Build and run:

```sh
docker compose build
docker compose up -d
```

This is not the official submission repo.
