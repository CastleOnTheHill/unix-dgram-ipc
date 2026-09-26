#!/usr/bin/env bash
# ---------------------------------------------------------------------
# wsl-verify.sh —— 在 WSL 里跑一遍完整的「结论流水线」，把结论带回来。
#
# 为什么需要它
# ---------------------------------------------------------------------
# 开发机是 Windows，而本项目要验证的东西（AF_UNIX 数据报语义、
# SCM_CREDENTIALS、flock、真实文件属主、/proc 统计、ASan）**只能**在
# Linux 原生文件系统上跑。所以流程固定是：
#
#     /mnt/d/...（Windows 侧源码）  --rsync-->  $HOME/unix-odmain-ipc
#                                               在 $HOME 里 make
#
# **绝不在 /mnt/c 或 /mnt/d 里 make**：drvfs 没有真实属主、没有真实 flock，
# 跑出来的结果没有意义（集成测试的 t05 靠 flock、t07 靠属主）。
#
# 用法（从 Windows 侧调用）
# ---------------------------------------------------------------------
#   wsl.exe -d Ubuntu-22.04 -u root -- \
#       bash /mnt/d/code/unix_odmain_ipc/scripts/wsl-verify.sh
#
#   # 只跑某几步：给步骤名，见下面 STEP 列表
#   wsl.exe -d Ubuntu-22.04 -u root -- \
#       bash /mnt/d/code/unix_odmain_ipc/scripts/wsl-verify.sh unit integration
#
# 环境变量
#   SRC   源码树（默认本脚本的上一级；在 /mnt/d 下也没关系，只读）
#   DEST  镜像目标（默认 $HOME/unix-odmain-ipc）
#   IPC_LAB_ROOT  集成测试实验目录的父目录（默认 /tmp）
#   SKIP_ASAN=1   跳过 ASan 那一遍（很慢）
#
# ---------------------------------------------------------------------
# 三条纪律（这个脚本的存在就是为了不让它们被违反）
# ---------------------------------------------------------------------
# 1. 每一步都记 exit code，一步失败**不**影响后面继续跑 —— 因为「0 警告但
#    测试失败」和「测试全过但覆盖率不达标」是两种不同的坏消息，一次跑完
#    比修一个看一个省事。
# 2. 每一步都要打印**被测程序自己的判定文本**。一个只打到 usage 就 exit 2
#    的检查，和通过的检查在输出上必须能区分 —— 本仓库真踩过这个坑。
# 3. 最终 VERDICT 里对每一步只允许三种判定：PASS / FAIL / BLOCKED。
#    「没跑」不许写成 PASS。
# ---------------------------------------------------------------------
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${SRC:-$(cd "$HERE/.." && pwd)}"
DEST="${DEST:-$HOME/unix-odmain-ipc}"
IPC_LAB_ROOT="${IPC_LAB_ROOT:-/tmp}"
export IPC_LAB_ROOT

WANT=("$@")
if [ ${#WANT[@]} -eq 0 ]; then
    WANT=(preflight lib separation coverage-selftest unit integration coverage asan)
fi

# 每步的外层时限。用例看门狗只管「用例内部挂起」；这里管的是「还没进到用例
# 就挂住」（构造函数、套件驱动、多进程同步）。超时按失败记，而不是让整条
# 流水线一直挂着 —— 实测踩过：单元测试卡死 12 分钟，日志停在某一行不动，
# 看起来像「还在跑」，只能靠人发现。
UNIT_TIMEOUT="${UNIT_TIMEOUT:-900}"
INTEG_TIMEOUT="${INTEG_TIMEOUT:-1800}"

want() {
    _w="$1"; shift
    for _x in "${WANT[@]}"; do
        [ "$_x" = "$_w" ] && return 0
    done
    return 1
}

# ---- 结论记账 -------------------------------------------------------

V_STEPS=""
V_PASS=0; V_FAIL=0; V_BLOCK=0
VERDICT_LOG="$SRC/.workbuddy/verify/VERDICT.txt"
mkdir -p "$(dirname "$VERDICT_LOG")" 2>/dev/null || VERDICT_LOG=/tmp/VERDICT.txt

Record() {   # Record <step> <PASS|FAIL|BLOCKED> <一行说明>
    V_STEPS="${V_STEPS}$(printf '%-20s %-8s %s\n' "$1" "$2" "$3")
"
    case "$2" in
        PASS)    V_PASS=$((V_PASS + 1)) ;;
        FAIL)    V_FAIL=$((V_FAIL + 1)) ;;
        BLOCKED) V_BLOCK=$((V_BLOCK + 1)) ;;
    esac
}

Section() { printf '\n================================================================\n== %s\n================================================================\n' "$1"; }

# 跑一步：打印命令、跑、记退出码。返回退出码。
Step() {   # Step <name> <cmd...>
    _s_name="$1"; shift
    printf '\n-------- %s --------\n$ %s\n' "$_s_name" "$*"
    "$@"
    _s_rc=$?
    printf '[exit %s] %s\n' "$_s_rc" "$_s_name"
    return "$_s_rc"
}

# ---- 前置 -----------------------------------------------------------

Section "前置检查"

if [ ! -f "$SRC/include/ipc/ipc.h" ]; then
    echo "!! SRC=$SRC 看起来不是源码树"
    Record preflight BLOCKED "SRC 不对"
    printf '%s' "$V_STEPS" >"$VERDICT_LOG"
    exit 2
fi

case "$DEST" in
    /mnt/*)
        echo "!! DEST=$DEST 在 /mnt 下。drvfs 没有真实属主与 flock，"
        echo "   在这里构建/测试没有意义。请把 DEST 设在 \$HOME 之下。"
        Record preflight BLOCKED "DEST 在 drvfs 上"
        printf '%s' "$V_STEPS" >"$VERDICT_LOG"
        exit 2 ;;
esac

if ! command -v cc >/dev/null 2>&1; then
    echo "!! WSL 里没有 C 编译器。先装："
    echo "     apt-get update && apt-get install -y build-essential"
    Record preflight BLOCKED "WSL 里没有 cc"
    printf '%s' "$V_STEPS" >"$VERDICT_LOG"
    exit 2
fi

echo "   uname      : $(uname -srm)"
echo "   cc         : $(cc --version | head -1)"
echo "   谁是 root  : uid=$(id -u)（集成测试的跨 uid 用例需要 root）"
echo "   SRC        : $SRC"
echo "   DEST       : $DEST"
if command -v lcov >/dev/null 2>&1; then
    echo "   lcov       : $(lcov --version 2>&1 | head -1)"
else
    echo "   lcov       : **没装**（覆盖率那一步会 BLOCKED）"
    echo "                apt-get install -y lcov"
fi
Record preflight PASS "CC=$(cc --version | head -1 | cut -c1-40) uid=$(id -u)"

# ---- 镜像 -----------------------------------------------------------

Section "镜像到 Linux 原生文件系统"
mkdir -p "$DEST" || exit 2
if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete \
        --exclude '.git' --exclude 'build' --exclude 'build-asan' \
        --exclude 'build-cov' --exclude '.workbuddy' --exclude '.refs' \
        "$SRC"/ "$DEST"/
else
    echo "   （没有 rsync，退回 tar 管道）"
    ( cd "$SRC" && tar -cf - \
        --exclude=./.git --exclude=./build --exclude=./build-asan \
        --exclude=./build-cov --exclude=./.workbuddy --exclude=./.refs . ) \
        | ( cd "$DEST" && tar -xf - )
fi
cd "$DEST" || exit 2
echo "   镜像完成：$DEST"
find "$DEST" -name '*.c' -o -name '*.h' | wc -l | xargs echo "   C 源文件数:"

# ---- 1) 交付物：0 警告 + 分离性 -------------------------------------

if want lib; then
    Section "步骤 1：构建交付物（0 警告）"
    make clean >/dev/null 2>&1
    if Step "make lib" make lib > /tmp/step_lib.log 2>&1; then
        grep -nE 'warning|error' /tmp/step_lib.log | head -20 || true
        n_warn=$(grep -cE 'warning:' /tmp/step_lib.log || true)
        n_err=$(grep -cE 'error:' /tmp/step_lib.log || true)
        if [ "$n_warn" -eq 0 ] && [ "$n_err" -eq 0 ]; then
            Record lib PASS "0 warning / 0 error"
        else
            Record lib FAIL "warning=$n_warn error=$n_err"
        fi
    else
        cat /tmp/step_lib.log | tail -30
        Record lib FAIL "make lib 失败"
    fi
fi

if want separation; then
    Section "步骤 2：交付物分离性"
    if Step "make check-separation" make check-separation; then
        Record separation PASS "libipc.a 里没有测试用实现"
    else
        Record separation FAIL "交付物里混进了测试用实现"
    fi
    echo
    echo "-- 交付物符号表（前 20 个，人工核对用）--"
    nm --defined-only "$DEST/build/libipc.a" 2>/dev/null | head -20 || true
fi

# ---- 3) 检查器自检 --------------------------------------------------
#
# 两条自检绑在同一个开关（coverage-selftest）下：本仓库的纪律是「任何
# 『检查通过』的结论，都必须先有一个反例证明这个检查能报错」。覆盖率门槛
# 和用例看门狗都属此类 —— 一个卡死的用例必须能被判成失败，而不是把套件
# 拖成「还在跑」。

if want coverage-selftest; then
    Section "步骤 3：自检（证明这些检查真的会报脏）"
    if Step "make coverage-selftest" make coverage-selftest; then
        Record coverage-selftest PASS "4 个对照里 1 个达标、3 个被拒"
    else
        Record coverage-selftest FAIL "门槛逻辑坏了"
    fi
    if Step "make unit-selftest" make unit-selftest; then
        Record unit-selftest PASS "挂起被判为失败（退出码 1）"
    else
        Record unit-selftest FAIL "看门狗没能把挂起判成失败"
    fi
fi

# ---- 4) 单元测试 ----------------------------------------------------

if want unit; then
    Section "步骤 4：白盒单元测试"
    if Step "make unit" timeout "$UNIT_TIMEOUT" make unit > /tmp/step_unit.log 2>&1; then
        tail -25 /tmp/step_unit.log
        # 关键：把被测程序**自己的**汇总行打出来，证明它真的跑到了用例，
        # 而不是提前 usage 退出。这一段是给「验证步骤必须能证明真的跑到
        # 了被测代码」这条纪律看的。
        verdict_line=$(grep -E '^(总|==|通过|[0-9]+ / [0-9]+)|pass|fail' \
            /tmp/step_unit.log | tail -1 | tr '\n' ' ')
        if [ -z "$verdict_line" ]; then
            # make 返回 0 但一行汇总都没打出来 —— 这本身就是可疑信号，
            # 不能当成通过（本仓库真踩过：检查只打到 usage 就 exit 2）。
            Record unit FAIL "退出码 0，但没找到被测程序的汇总行"
        else
            printf '   被测程序自报：%s\n' "$verdict_line"
            Record unit PASS "$verdict_line"
        fi
    else
        tail -40 /tmp/step_unit.log
        printf '   被测程序自报：%s\n' \
            "$(grep -E 'FAIL|失败' /tmp/step_unit.log | head -5)"
        Record unit FAIL "见 /tmp/step_unit.log 与上面的输出"
    fi
fi

# ---- 5) 集成测试 ----------------------------------------------------

if want integration; then
    Section "步骤 5：黑盒集成测试（多进程）"
    export IPC_LAB="${IPC_LAB_ROOT}/ipc-lab"
    if [ "$(id -u)" -ne 0 ]; then
        echo "   注意：当前不是 root，跨 uid 的用例会报 BLOCKED，"
        echo "         整条套件因此以退出码 2 结束（这是设计，不是失败）。"
    fi
    if Step "make integration" timeout "$INTEG_TIMEOUT" make integration \
            > /tmp/step_integ.log 2>&1; then
        tail -20 /tmp/step_integ.log
        Record integration PASS "$(grep -E '^== ' /tmp/step_integ.log | tail -1)"
    else
        rc=$?
        tail -40 /tmp/step_integ.log
        if grep -q 'BLOCKED' /tmp/step_integ.log; then
            Record integration BLOCKED "环境不满足（非 root / 缺权限）"
        else
            Record integration FAIL "有用例失败（rc=$rc）"
        fi
    fi
fi

# ---- 6) 覆盖率 ------------------------------------------------------

if want coverage; then
    Section "步骤 6：真实行覆盖率（门槛 80%）"
    if ! command -v lcov >/dev/null 2>&1; then
        echo "!! 没装 lcov，跳过。apt-get install -y lcov"
        Record coverage BLOCKED "没有 lcov"
    else
        if Step "make coverage" make coverage > /tmp/step_cov.log 2>&1; then
            grep -E 'lines|行覆盖率|达标|Total' /tmp/step_cov.log | tail -10
            Record coverage PASS "$(grep -E '行覆盖率' /tmp/step_cov.log | tail -1)"
        else
            tail -30 /tmp/step_cov.log
            if grep -q '行覆盖率' /tmp/step_cov.log; then
                # 门槛那一关判的不达标 —— 这是**真结论**（数字测出来了，就是低），
                # 不是环境问题，所以记 FAIL 而不是 BLOCKED。
                Record coverage FAIL "$(grep -E '行覆盖率' /tmp/step_cov.log | tail -1)"
            elif grep -q 'BLOCKED' /tmp/step_cov.log; then
                # 数字根本没出来（多半是非 root 导致 integration 那一步没跑成）。
                # 没测出来 ≠ 不达标，记 BLOCKED。
                Record coverage BLOCKED "环境不满足（非 root / 缺权限），覆盖率没测出来"
            else
                Record coverage FAIL "见 /tmp/step_cov.log"
            fi
        fi
    fi
fi

# ---- 7) ASan --------------------------------------------------------

if want asan; then
    if [ "${SKIP_ASAN:-0}" = "1" ]; then
        Record asan BLOCKED "SKIP_ASAN=1"
    else
        Section "步骤 7：ASan + UBSan 重放（换一棵 build-asan 树）"
        export IPC_LAB="${IPC_LAB_ROOT}/ipc-lab-asan"
        # 必须换树重建。make **不**跟踪编译选项的变化：上一次 build-asan 留下的
        # .o 会被直接复用，于是这一遍其实跑在**没开 sanitizer** 的目标码上，
        # 而输出看起来完全正常 —— 这正是本仓库反复强调的那类假结论。
        # 这里用 `clean` 是安全的：2026-09-24 起 clean 只删当前 $(BUILD)，
        # 不会再连带删掉上一步的 build-cov（详见 Makefile 的清理一节）。
        make BUILD=build-asan clean >/dev/null 2>&1 || true
        if Step "make BUILD=build-asan test" \
            make BUILD=build-asan IPC_LAB="$IPC_LAB" \
                 OPT='-O1 -g -fsanitize=address,undefined' test \
            > /tmp/step_asan.log 2>&1; then
            tail -15 /tmp/step_asan.log
            if grep -qiE 'runtime error|AddressSanitizer|LeakSanitizer' \
                /tmp/step_asan.log; then
                grep -iE -A15 'runtime error|AddressSanitizer|LeakSanitizer' \
                    /tmp/step_asan.log | head -60
                Record asan FAIL "sanitizer 报了诊断"
            else
                Record asan PASS "无 sanitizer 诊断"
            fi
        else
            tail -40 /tmp/step_asan.log
            if grep -q 'BLOCKED' /tmp/step_asan.log; then
                Record asan BLOCKED "环境不满足"
            else
                Record asan FAIL "见 /tmp/step_asan.log"
            fi
        fi
    fi
fi

# ---- 结论 -----------------------------------------------------------

Section "结论"
printf '   PASS %d / FAIL %d / BLOCKED %d\n' "$V_PASS" "$V_FAIL" "$V_BLOCK"
echo
printf '%s' "$V_STEPS"
echo
echo "（镜像目录：$DEST）"

{
    echo "libipc 验证结论 —— $(date -u '+%Y-%m-%d %H:%M:%SZ')"
    echo "主机: $(uname -srm)   uid=$(id -u)  cc=$(cc --version | head -1)"
    echo "镜像: $DEST"
    echo
    printf 'PASS %d / FAIL %d / BLOCKED %d\n\n' "$V_PASS" "$V_FAIL" "$V_BLOCK"
    printf '%s' "$V_STEPS"
    echo
    echo "判据提醒："
    echo "  - 任何一步 BLOCKED 都不算通过；「没跑」不等于「没问题」。"
    echo "  - 覆盖率数字要看 make coverage 打出的总行覆盖率那一行，"
    echo "    不要引用更早的结论。"
    echo "  - ASan 那一遍必须每次改码后重跑，不许引用旧结果。"
} >"$VERDICT_LOG" 2>/dev/null || true
echo "   结论已写入: $VERDICT_LOG"

if [ "$V_BLOCK" -ne 0 ]; then
    exit 2
fi
if [ "$V_FAIL" -ne 0 ]; then
    exit 1
fi
exit 0
