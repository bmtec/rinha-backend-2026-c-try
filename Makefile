CC ?= gcc
OUT ?= out
CFLAGS ?= -O3 -march=haswell -mavx2 -mfma -flto -fno-plt -DNDEBUG -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -flto

COMMON = src/distance.c src/index.c src/net.c src/parser.c src/responses.c src/vectorizer.c

.PHONY: all clean

all: $(OUT)/api $(OUT)/lb

$(OUT):
	mkdir -p $(OUT)

$(OUT)/api: $(COMMON) src/api.c src/rinha.h | $(OUT)
	$(CC) $(CFLAGS) $(COMMON) src/api.c -o $@ $(LDFLAGS)

$(OUT)/lb: src/net.c src/lb.c src/rinha.h | $(OUT)
	$(CC) $(CFLAGS) src/net.c src/lb.c -o $@ $(LDFLAGS)

clean:
	rm -rf $(OUT)
