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
#   3. libipc.a **只由 src/ 下的 .c 构成**。tests/ 下的任何东西（尤其是参考
#      宿主 tests/support/refhost）都不许进交付物。这一条靠
#      `make check-separation` 来证明，不靠自觉 —— 它会把 libipc.a 的符号表
#      翻一遍，出现 refhost / utest 这类名字、或者出现 main()，就报错。
#
#      【2026-09-23 清理】参考宿主一度同时存在于 include/ipc/ipc_refhost.h 与
#      tests/support/refhost.h 两处，公开头目录里那份是迁移时忘了删的旧副本。
#      两份内容已经不一致，留着只会让人照着旧的那份写适配层。
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
# tests/tools/ 下的程序自带 main()，是给黑盒集成测试用的独立进程。
# 它们**不能**放在 tests/support/ 里：那个目录的 .c 会一起链进 run_unit，
# 于是出现两个 main。分开不是为了整齐，是为了能链得上。
TOOLS_SRC   := $(wildcard tests/tools/*.c)

UNIT_BIN    := $(BUILD)/run_unit
TOOLS_BIN   := $(patsubst tests/tools/%.c,$(BUILD)/bin/%,$(TOOLS_SRC))
SUPPORT_OBJ := $(patsubst tests/support/%.c,$(BUILD)/obj/support/%.o,$(SUPPORT_SRC))
UNIT_OBJ    := $(patsubst tests/unit/%.c,$(BUILD)/obj/unit/%.o,$(UNIT_SRC))

.PHONY: all lib test unit integration tools bench coverage cov-threshold \
        coverage-selftest check-separation dev-check clean help

all: lib

help:
	@echo "make lib               —— 只构建交付物 libipc.a（不含任何测试用实现）"
	@echo "make unit              —— 构建并运行白盒单元测试"
	@echo "make integration       —— 构建集成测试用的独立进程 + 跑黑盒测试"
	@echo "make test              —— 单元 + 黑盒集成测试"
	@echo "make coverage          —— 用 gcov/lcov 出覆盖率报告并检查门槛（需 lcov）"
	@echo "make coverage-selftest —— 证明覆盖率门槛真的会判不达标"
	@echo "make check-separation  —— 证明 libipc.a 里没有测试用实现"
	@echo "make dev-check         —— 【没有 C 工具链时的降级检查】见下"
	@echo "make clean             —— 删掉 \$$(BUILD)"
	@echo ""
	@echo "ASan 那一遍请换个树：make BUILD=build-asan OPT='-O1 -g -fsanitize=address,undefined' test"
	@echo ""
	@echo "关于 dev-check：它走的是 .workbuddy/checks/ 下的降级脚本，只在"
	@echo "「这台机器上装不了/用不了 C 工具链」时才用。它用 ziglang 做交叉编译，"
	@echo "能给的是**编译、链接、符号表、shell 语法**四类结论；"
	@echo "它给不出「测试通过」和「覆盖率达标」—— 那两句必须真的跑起来才有。"
	@echo "能正常 make test 的时候不要用它，两者不能互相替代。"

# ---------------------------------------------------------------------
# 降级检查（本机没有 C 工具链时的替代手段）
#
# 使用前提：.workbuddy/ 目录存在（它不进仓库）。所以这个目标**不在**任何
# 默认路径上，只能显式调；缺脚本时明确报错退出，不静默跳过。
#
# 它自己的对照组用 `bash .workbuddy/checks/zcc.sh --control` 与
# `python .workbuddy/checks/symcheck.py --control` 跑 —— 这两条也必须过，
# 否则「检查通过」没有意义。
# ---------------------------------------------------------------------

dev-check:
	@if [ ! -x .workbuddy/checks/zcc.sh ]; then \
		echo "!! 找不到 .workbuddy/checks/zcc.sh（该目录不进仓库）"; \
		echo "   这个目标是给「本机没有 C 工具链」的开发机用的降级手段。"; \
		exit 2; fi
	@echo "== 降级检查的对照组（先证明检查器能报脏）"
	@bash .workbuddy/checks/zcc.sh --control || exit 1
	@python3 .workbuddy/checks/symcheck.py --control || exit 1
	@echo ""
	@bash .workbuddy/checks/zcc.sh

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
	@if $(NM) --defined-only $(LIB) 2>/dev/null | grep -q ' [Tt] main$$'; then \
		echo "!! 交付物里出现了 main()：说明有测试程序被链进了静态库"; \
		$(NM) --defined-only $(LIB) | grep ' [Tt] main$$'; \
		exit 1; \
	fi
	@echo "   通过：libipc.a 中没有 main()"
	@echo "   注意这两条检查的边界：它们只能证明**名字**没混进来。"
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

# 集成测试用的独立进程：参考宿主 + 库 + 各自的 main()。
$(BUILD)/bin/%: tests/tools/%.c $(SUPPORT_OBJ) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(SUPPORT_OBJ) $(LIB) -o $@ $(LDLIBS)
	@echo "  构建 $@"

tools: $(TOOLS_BIN)
	@if [ -z "$(TOOLS_SRC)" ]; then \
		echo "!! tests/tools/ 下没有源码，集成测试跑不起来"; exit 1; \
	fi

integration: $(LIB) tools
	@echo "== 黑盒集成测试（多进程，需要能建 AF_UNIX socket）"
	@IPC_TEST_BIN=$(BUILD)/bin IPC_TEST_BUILD=$(BUILD) \
		bash tests/integration/run_all.sh

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
#
# 【2026-09-23 修复】原先的门槛判断是拿 awk 去扫 lcov 的 .info 文本、
# 找带 "lines" 字样的行 —— 而 .info 里根本没有那种行，于是 awk 永远
# exit 0，`|| { ... exit 1; }` 永远不触发。那是一条**不可能失败**的检查，
# 比没有检查更糟：它会让人以为门槛被守着。
# 现在改成从 .info 里汇总 LH/LF 两个计数再比，并且用 `make coverage-selftest`
# 证明它确实会判不达标。
# ---------------------------------------------------------------------

COVERAGE_MIN ?= 80
COV_INFO     ?= build-cov/coverage/lib.info

coverage:
	@which lcov >/dev/null 2>&1 || { \
		echo "!! 没装 lcov。Ubuntu: apt-get install -y lcov"; exit 2; }
	@$(MAKE) BUILD=build-cov OPT='-O0 -g --coverage' clean >/dev/null
	@echo "== 重跑白盒单元测试（带 --coverage 的树）"
	@$(MAKE) BUILD=build-cov OPT='-O0 -g --coverage' unit
	@echo "== 重跑黑盒集成测试（带 --coverage 的树）"
	@$(MAKE) BUILD=build-cov OPT='-O0 -g --coverage' integration
	@mkdir -p build-cov/coverage
	@lcov --capture --directory build-cov --output-file build-cov/coverage/all.info \
		--rc lcov_branch_coverage=1 >/dev/null
	@lcov --remove build-cov/coverage/all.info '/usr/*' '*/tests/*' \
		--output-file $(COV_INFO) --rc lcov_branch_coverage=1 >/dev/null
	@genhtml $(COV_INFO) --output-directory build-cov/coverage/html \
		--rc lcov_branch_coverage=1 >/dev/null
	@echo "== 覆盖率（src/ 口径，已剔除 tests/ 与系统头）"
	@lcov --summary $(COV_INFO) 2>&1 | sed 's/^/   /'
	@echo "   HTML 报告: build-cov/coverage/html/index.html"
	@$(MAKE) --no-print-directory cov-threshold

cov-threshold:
	@if [ ! -f "$(COV_INFO)" ]; then \
		echo "!! 找不到 $(COV_INFO)，没有数据不能算达标"; exit 1; \
	fi
	@LH=$$(awk -F: '/^LH:/{s+=$$2} END{printf "%d", s+0}' $(COV_INFO)); \
	LF=$$(awk -F: '/^LF:/{s+=$$2} END{printf "%d", s+0}' $(COV_INFO)); \
	if [ "$$LF" -eq 0 ]; then \
		echo "!! 覆盖率数据为空（LF=0）：测试没跑到任何 src/ 里的行，判不达标"; \
		exit 1; \
	fi; \
	awk -v lh="$$LH" -v lf="$$LF" -v min="$(COVERAGE_MIN)" 'BEGIN { \
		p = 100.0 * lh / lf; \
		printf "   行覆盖率: %d/%d = %.2f%%（门槛 %s%%）\n", lh, lf, p, min; \
		exit (p < min) ? 1 : 0; }' \
	|| { echo "!! 行覆盖率低于 $(COVERAGE_MIN)% ，不达标"; exit 1; }; \
	echo "   达标"

# 门槛逻辑的自检。按本仓库的纪律：任何「检查通过」的结论，都必须先有
# 一个反例证明这个检查**能报错** —— 上面刚修的那条就是教训。
coverage-selftest:
	@tmp=$$(mktemp -d) || exit 1; \
	printf 'SF:src/fake.c\nLF:100\nLH:95\nend_of_record\n' > $$tmp/pass.info; \
	printf 'SF:src/fake.c\nLF:100\nLH:10\nend_of_record\n' > $$tmp/fail.info; \
	printf 'SF:src/fake.c\nLF:0\nLH:0\nend_of_record\n'  > $$tmp/empty.info; \
	failures=0; \
	if $(MAKE) --no-print-directory COV_INFO=$$tmp/pass.info cov-threshold >/dev/null 2>&1; then \
		echo "   95%  -> 正确判为达标"; \
	else \
		echo "!! 95% 竟然被判为不达标，门槛逻辑坏了"; failures=$$((failures+1)); \
	fi; \
	if $(MAKE) --no-print-directory COV_INFO=$$tmp/fail.info cov-threshold >/dev/null 2>&1; then \
		echo "!! 10% 竟然被判为达标 —— 门槛形同虚设"; failures=$$((failures+1)); \
	else \
		echo "   10%  -> 正确判为不达标"; \
	fi; \
	if $(MAKE) --no-print-directory COV_INFO=$$tmp/empty.info cov-threshold >/dev/null 2>&1; then \
		echo "!! 空数据竟然被判为达标"; failures=$$((failures+1)); \
	else \
		echo "   LF=0 -> 正确判为不达标"; \
	fi; \
	if $(MAKE) --no-print-directory COV_INFO=$$tmp/nonexistent.info cov-threshold \
		>/dev/null 2>&1; then \
		echo "!! 缺文件竟然被判为达标"; failures=$$((failures+1)); \
	else \
		echo "   缺文件 -> 正确判为不达标"; \
	fi; \
	rm -rf $$tmp; \
	if [ $$failures -ne 0 ]; then \
		echo "!! 覆盖率门槛自检失败 $$failures 项"; exit 1; \
	fi; \
	echo "== 覆盖率门槛自检通过：4 个对照里 1 个达标、3 个被正确拒绝"

# ---------------------------------------------------------------------
# 清理
# ---------------------------------------------------------------------

clean:
	rm -rf $(BUILD)
	rm -rf build-cov
