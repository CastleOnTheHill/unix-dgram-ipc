# libipc -- AF_UNIX SOCK_DGRAM direct-connect IPC framework
#
# Everything is built out of tree (default ./build) so the source tree stays
# clean.  On WSL this MUST live on a Linux filesystem: the tests exercise
# ownership, flock and permissions, none of which /mnt/c reproduces.

CC      ?= cc
BUILD   ?= build

CSTD     = -std=gnu11
OPT     ?= -O2
WARN     = -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual -Wformat=2 \
           -Wstrict-prototypes -Wmissing-prototypes -Wredundant-decls \
           -Wvla -Wundef -Wwrite-strings
CPPFLAGS = -Iinclude -Isrc -D_GNU_SOURCE
CFLAGS  ?= $(CSTD) $(OPT) -g $(WARN) -fno-omit-frame-pointer
LDFLAGS  = -pthread
LDLIBS   = -pthread

LIB_SRC  = $(sort $(wildcard src/*.c))
LIB_OBJ  = $(patsubst src/%.c,$(BUILD)/obj/%.o,$(LIB_SRC))

UNIT_SRC = $(sort $(wildcard tests/unit/*.c))
UNIT_OBJ = $(patsubst tests/unit/%.c,$(BUILD)/unit/%.o,$(UNIT_SRC))

BIN      = $(BUILD)/bin

.PHONY: all lib unit testmod probes bench check integration clean fmt help

all: lib unit $(BIN)/ipc_testmod probes bench

# ------------------------------------------------------------------ #
# library                                                            #
# ------------------------------------------------------------------ #

lib: $(BUILD)/libipc.a $(BUILD)/libipc.so

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c $< -o $@

$(BUILD)/libipc.a: $(LIB_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/libipc.so: $(LIB_OBJ)
	@mkdir -p $(dir $@)
	$(CC) -shared -o $@ $^ $(LDLIBS)

# ------------------------------------------------------------------ #
# unit tests                                                         #
# ------------------------------------------------------------------ #

unit: $(BIN)/unit_tests

$(BUILD)/unit/%.o: tests/unit/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) -Itests/unit $(CFLAGS) -c $< -o $@

$(BIN)/unit_tests: $(UNIT_OBJ) $(BUILD)/libipc.a
	@mkdir -p $(dir $@)
	$(CC) -o $@ $(UNIT_OBJ) $(BUILD)/libipc.a $(LDFLAGS) $(LDLIBS)

# ------------------------------------------------------------------ #
# helpers: test driver, probes, benchmark                            #
# ------------------------------------------------------------------ #

testmod: $(BIN)/ipc_testmod
$(BIN)/ipc_testmod: tools/ipc_testmod.c $(BUILD)/libipc.a
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(BUILD)/libipc.a $(LDFLAGS) $(LDLIBS)

PROBE_SRC = $(sort $(wildcard probes/*.c))
PROBE_BIN = $(patsubst probes/%.c,$(BIN)/%,$(PROBE_SRC))

probes: $(PROBE_BIN)
$(BIN)/%: probes/%.c $(BUILD)/libipc.a
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(BUILD)/libipc.a $(LDFLAGS) $(LDLIBS)

BENCH_SRC = $(sort $(wildcard tests/bench/*.c))
BENCH_BIN = $(patsubst tests/bench/%.c,$(BIN)/%,$(BENCH_SRC))

bench: $(BENCH_BIN)
$(BIN)/%: tests/bench/%.c $(BUILD)/libipc.a
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(BUILD)/libipc.a $(LDFLAGS) $(LDLIBS)

# ------------------------------------------------------------------ #
# asan variant (separate tree, never mixed with the perf build)       #
# ------------------------------------------------------------------ #

asan:
	$(MAKE) BUILD=build-asan OPT="-O1 -fsanitize=address,undefined -fno-common" \
	        LDFLAGS="-pthread -fsanitize=address,undefined" \
	        LDLIBS="-pthread -fsanitize=address,undefined" all

# ------------------------------------------------------------------ #
# entry points                                                       #
# ------------------------------------------------------------------ #

check: unit
	$(BIN)/unit_tests

integration: all
	bash tests/integration/run_all.sh

clean:
	rm -rf build build-asan

help:
	@echo "targets: lib unit testmod probes bench check integration asan clean"
	@echo "  bench binaries land in $(BIN): ipc_bench (libipc throughput/"
	@echo "  latency), ab_relay (datagram-direct vs stream-through-relay),"
	@echo "  plus the probes/ controls incl. rt_baseline."
