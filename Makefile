CC ?= gcc
OUT ?= out
CFLAGS ?= -O3 -march=haswell -mavx2 -mfma -flto -fno-plt -DNDEBUG -Wall -Wextra -Wno-unused-parameter
BUILDER_CFLAGS ?= $(CFLAGS) -ffp-contract=off
LDFLAGS ?= -flto

COMMON = src/distance.c src/index.c src/net.c src/parser.c src/responses.c src/vectorizer.c

.PHONY: all clean

all: $(OUT)/api $(OUT)/lb $(OUT)/builder

$(OUT):
	mkdir -p $(OUT)

$(OUT)/api: $(COMMON) src/api.c src/rinha.h | $(OUT)
	$(CC) $(CFLAGS) $(COMMON) src/api.c -o $@ $(LDFLAGS)

$(OUT)/lb: src/net.c src/lb.c src/rinha.h | $(OUT)
	$(CC) $(CFLAGS) src/net.c src/lb.c -o $@ $(LDFLAGS) -pthread

$(OUT)/builder: src/distance.c src/builder.c src/rinha.h | $(OUT)
	$(CC) $(BUILDER_CFLAGS) src/distance.c src/builder.c -o $@ $(LDFLAGS) -pthread -lz -lm

clean:
	rm -rf $(OUT)
