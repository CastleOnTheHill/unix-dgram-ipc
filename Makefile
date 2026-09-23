# ---------------------------------------------------------------------
# AF_UNIX SOCK_DGRAM 直连 IPC —— 构建入口
#
# 改这个文件前先读这四条纪律：
#
#   1. 构建/测试必须在 **Linux 原生文件系统**上跑，不要在 /mnt/c。
#      drvfs 没有真实属主、没有真实 flock，跑出来的结果没有意义。
#
#   2. 性能构建与 sanitizer 构建**分树**：build/ 与 build-asan/，互不混用。
#      改过代码之后 sanitizer 那一遍必须重跑，不能引用旧结论。
#
#   3. libipc.a **只由 src/ 构成**。tests/ 下的任何东西（尤其是参考宿主
#      ipc_refhost）都不许进交付物。这一条靠 `make check-separation` 来证明，
#      不靠自觉 —— 它会把 libipc.a 的符号表翻一遍，出现测试用实现的名字就报错。
#
#   4. 覆盖率必须是**跑出来的**（gcov/lcov），不是估出来的。
# ---------------------------------------------------------------------

BUILD      ?= build
PREFIX     ?= /usr/local

# 交叉编译前缀，例如 CROSS_COMPILE=x86_64-linux-gnu-
CROSS_COMPILE ?=
CC         := $(CROSS_COMPILE)cc
AR         := $(CROSS_COMPILE)ar
NM         := $(CROSS_COMPILE)nm
RANLIB     := $(CROSS_COMPILE)ranlib

CSTD       := -std=c11
# 警告集合刻意偏严：这个库的很多条不变式（定长缓冲、类型宽度、返回值必须查）
# 都能被编译器顺手兜住。新增 typedef 时若报 missing-prototypes，说明内部头
# 文件里漏了一次声明。
WARN       := -Wall -Wextra -Wpedantic -Werror \
              -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
              -Wpointer-arith -Wcast-align -Wformat -Wformat-security -Wundef \
              -Wvla -Wredundant-decls -Wswitch-enum -Winit-self
DEFS       := -D_GNU_SOURCE
OPT        ?= -O2 -g
CPPFLAGS   := $(DEFS) -Iinclude -Isrc -Itests/support
CFLAGS     := $(CSTD) $(WARN) $(OPT) -fno-common
LDLIBS     := -lpthread

LIB        := $(BUILD)/libipc.a
LIB_SRC    := $(wildcard src/*.c)
LIB_OBJ    := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(LIB_SRC))

SUPPORT_SRC := $(wildcard tests/support/*.c)
UNIT_SRC    := $(wildcard tests/unit/*.c)
BENCH_SRC   := $(wildcard tests/bench/*.c)

UNIT_BIN    := $(BUILD)/run_unit
SUPPORT_OBJ := $(patsubst tests/support/%.c,$(BUILD)/obj/support/%.o,$(SUPPORT_SRC))
UNIT_OBJ    := $(patsubst tests/unit/%.c,$(BUILD)/obj/unit/%.o,$(UNIT_SRC))

.PHONY: all lib test unit integration bench coverage check-separation clean help

all: lib

help:
	@echo "make lib               —— 只构建交付物 libipc.a（不含任何测试用实现）"
	@echo "make unit              —— 构建并运行白盒单元测试"
	@echo "make test              —— 单元 + 黑盒集成测试"
	@echo "make coverage          —— 用 gcov/lcov 出覆盖率报告（需 lcov）"
	@echo "make check-separation  —— 证明 libipc.a 里没有测试用实现"
	@echo "make clean             —— 删掉 \$$(BUILD)"
	@echo ""
	@echo "ASan 那一遍请换个树：make BUILD=build-asan OPT='-O1 -g -fsanitize=address,undefined' test"

# ---------------------------------------------------------------------
# 交付物
# ---------------------------------------------------------------------

lib: $(LIB)

$(LIB): $(LIB_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^
	$(RANLIB) $@
	@echo "== 交付物: $@"

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/support/%.o: tests/support/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/obj/unit/%.o: tests/unit/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# ---------------------------------------------------------------------
# 分离性检查：交付物里不许出现测试用实现
# ---------------------------------------------------------------------

check-separation: $(LIB)
	@if [ -z "$(SUPPORT_SRC)$(UNIT_SRC)" ]; then \
		echo "!! 没有任何测试用源码，这条检查等于没查（不能算通过）"; exit 1; \
	fi
	@echo "== 检查 $(LIB) 的符号表里有没有测试用实现"
	@if $(NM) --defined-only $(LIB) 2>/dev/null | grep -qi 'refhost\|testhost\|utest'; then \
		echo "!! 交付物里混进了测试用实现："; \
		$(NM) --defined-only $(LIB) | grep -i 'refhost\|testhost\|utest'; \
		exit 1; \
	fi
	@echo "   通过：libipc.a 中没有任何 refhost / testhost / utest 符号"
	@echo "   注意这条检查的边界：它只能证明**名字**没混进来。"
	@echo "   真正保证分离的是构建规则（libipc.a 只由 src/*.c 生成）。"

# ---------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------

$(UNIT_BIN): $(UNIT_OBJ) $(SUPPORT_OBJ) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(UNIT_OBJ) $(SUPPORT_OBJ) $(LIB) -o $@ $(LDLIBS)

unit: $(UNIT_BIN)
	@echo "== 白盒单元测试"
	@$(UNIT_BIN)

integration: $(LIB)
	@echo "== 黑盒集成测试（多进程，需要能建 AF_UNIX socket）"
	@bash tests/integration/run_all.sh

test: unit integration

bench: $(LIB)
	@mkdir -p $(BUILD)/bench
	@for s in $(BENCH_SRC); do \
		b=$$(basename $$s .c); \
		$(CC) $(CPPFLAGS) $(CFLAGS) $$s $(LIB) -o $(BUILD)/bench/$$b $(LDLIBS) || exit 1; \
		echo "  构建 $(BUILD)/bench/$$b"; \
	done

# ---------------------------------------------------------------------
# 覆盖率
#
# 纪律：覆盖率必须跑出来。`make coverage` 会把测试**再跑一遍**（用带
# --coverage 的构建），然后在报告里打印总行覆盖率；低于阈值直接失败。
# 不允许把上一次的数字写进报告当结论。
# ---------------------------------------------------------------------

COVERAGE_MIN ?= 80

coverage:
	@which lcov >/dev/null 2>&1 || { \
		echo "!! 没装 lcov。Ubuntu: apt-get install -y lcov"; exit 2; }
	@$(MAKE) BUILD=build-cov OPT='-O0 -g --coverage' clean >/dev/null
	@$(MAKE) BUILD=build-cov OPT='-O0 -g --coverage' unit >/dev/null
	@$(MAKE) BUILD=build-cov OPT='-O0 -g --coverage' integration >/dev/null
	@mkdir -p build-cov/coverage
	@lcov --capture --directory build-cov --output-file build-cov/coverage/all.info \
		--rc lcov_branch_coverage=1 >/dev/null
	@lcov --remove build-cov/coverage/all.info '/usr/*' '*/tests/*' \
		--output-file build-cov/coverage/lib.info --rc lcov_branch_coverage=1 >/dev/null
	@genhtml build-cov/coverage/lib.info --output-directory build-cov/coverage/html \
		--rc lcov_branch_coverage=1 >/dev/null
	@echo "== 覆盖率（src/ 口径）"
	@lcov --summary build-cov/coverage/lib.info 2>&1 | sed 's/^/   /'
	@echo "   HTML 报告: build-cov/coverage/html/index.html"
	@awk -F'[: ]+' '/lines/ { if ($$5+0 < $(COVERAGE_MIN)) exit 1 }' \
		build-cov/coverage/lib.info >/dev/null 2>&1 || \
		{ echo "!! 行覆盖率低于 $(COVERAGE_MIN)% ，不达标"; exit 1; }

# ---------------------------------------------------------------------
# 清理
# ---------------------------------------------------------------------

clean:
	rm -rf $(BUILD)
	rm -rf build-cov
