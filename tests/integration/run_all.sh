#!/usr/bin/env bash
# ---------------------------------------------------------------------
# run_all.sh —— 黑盒集成测试入口。多进程、真 socket、真文件属主。
#
# =====================================================================
# 退出码语义（三态，不允许含糊）
# =====================================================================
#   0 —— 全部用例通过
#   1 —— 有用例失败
#   2 —— **没跑成**（BLOCKED）：环境不满足（非 root 而有用例需要 root、
#        缺少被测程序、护栏自检失败）。绝不用 0 表示「有一部分没跑」。
#
# 为什么单独给 BLOCKED 一个退出码：把「没测」报成「通过」是这个项目里
# 最容易被自己骗到的一件事。有了 2，CI 上就会红，而红的时候人去看一眼
# 就知道是环境问题还是代码问题。
#
# =====================================================================
# 非 root 怎么办
# =====================================================================
# 默认把整条套件判为 BLOCKED（退出 2），并且**不列出任何 pass** —— 因为
# 有一部分用例（跨 uid / 属组）在非 root 下根本没法构造，跑出来的
# 「通过」不能代表这套测试的结论。
#
# 想只跑不需要 root 的那部分，显式加环境变量：
#     IPC_INTEG_NONROOT=1 bash tests/integration/run_all.sh
# 这时会打印 `部分运行` 并把需要 root 的用例标成 BLOCKED，退出码按
# 「有没有 BLOCKED」决定（仍有 2）。所以它**不会**把部分运行说成全部通过。
#
# =====================================================================
# 为什么必须跑在 Linux 原生文件系统上
# =====================================================================
# drvfs（/mnt/c）没有真实属主、没有真实 flock。这两样正好是本套件里
# 好几条用例的判据，所以在 /mnt/* 下跑出来的结果没有意义，直接拒绝。
# ---------------------------------------------------------------------
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"

# shellcheck source=lab_guard.sh
. "$HERE/lab_guard.sh"

IPC_TEST_BIN="${IPC_TEST_BIN:-$REPO/build/bin}"
IPC_LAB="${IPC_LAB:-/tmp/ipc-lab}"
export IPC_TEST_BIN IPC_LAB

# ---- 前置检查 -------------------------------------------------------

echo "== 黑盒集成测试"
echo "   被测程序目录: $IPC_TEST_BIN"
echo "   实验根目录:   $IPC_LAB"

# 0) 护栏自检：一条从来没报过错的护栏等于没有护栏
if ! ( LabGuardSelfTest ) ; then
    echo "!! lab_guard 自检失败，不继续"
    exit 2
fi
echo "   lab_guard 自检通过"

# 1) 不能在 /mnt 下跑
case "$IPC_LAB" in
    /mnt/*)
        echo "!! IPC_LAB=$IPC_LAB 在 /mnt 下。drvfs 没有真实属主与 flock，"
        echo "   这个位置跑出来的结果没有意义。请换到 Linux 原生文件系统。"
        exit 2 ;;
esac

# 2) 被测程序必须在
if [ ! -x "$IPC_TEST_BIN/ipc_testmod" ]; then
    echo "!! 找不到 $IPC_TEST_BIN/ipc_testmod"
    echo "   先跑 make tools（或在交叉编译场景下用能产出 Linux 可执行文件的工具链）。"
    echo "   注意：本机如果有产物但**跑不了**，那不是「测试通过」，是 BLOCKED。"
    exit 2
fi

NEED_ROOT_COUNT=0
if [ "$(id -u)" -ne 0 ]; then
    NEED_ROOT_COUNT=$(grep -l '^# NEED_ROOT' "$HERE"/t[0-9]*.sh 2>/dev/null | wc -l)
    if [ "${IPC_INTEG_NONROOT:-0}" != "1" ]; then
        echo
        echo "!! BLOCKED：当前不是 root，而本套件里有 $NEED_ROOT_COUNT 条用例"
        echo "   需要 root 才能构造跨 uid / 属组的场景。"
        echo "   跑不出来就是跑不出来，这里不列出任何 pass ——"
        echo "   部分运行的「通过」不能当作整套测试的结论。"
        echo
        echo "   想只跑不需要 root 的那部分（结果仍会标 BLOCKED）："
        echo "       IPC_INTEG_NONROOT=1 bash tests/integration/run_all.sh"
        exit 2
    fi
    echo "   （部分运行模式：只跑不需要 root 的用例，其余标 BLOCKED）"
fi

mkdir -p "$IPC_LAB" || exit 2
LabGuard "$IPC_LAB" || exit 2

# ---- 执行 -----------------------------------------------------------
#
# 每个 tNN_*.sh 是独立进程，结论靠它自己打出的 `RESULT <name> <verdict>` 行
# 回传（见 common.sh 里为什么不用 exit code）。没打出结论行的用例算 FAIL：
# 「跑完了但没给结论」和「通过」在输出上必须能区分开。

T_PASS=0
T_FAIL=0
T_BLOCKED=0
FAILED_TESTS=""

for t in "$HERE"/t[0-9]*.sh; do
    [ -f "$t" ] || continue
    name="$(basename "$t")"
    printf '\n-- %s\n' "$name"

    out="$(bash "$t" 2>&1)"
    printf '%s\n' "$out"

    result="$(printf '%s\n' "$out" | grep -E '^RESULT ' | tail -1)"
    if [ -z "$result" ]; then
        printf '  FAIL  %s: 这个测试没有打出 RESULT 行，无法判定 —— 算失败\n' \
            "$name"
        T_FAIL=$((T_FAIL + 1))
        FAILED_TESTS="$FAILED_TESTS $name(no-verdict)"
        continue
    fi
    case "$result" in
        *' PASS')    T_PASS=$((T_PASS + 1)) ;;
        *' FAIL')    T_FAIL=$((T_FAIL + 1)); FAILED_TESTS="$FAILED_TESTS $name" ;;
        *' BLOCKED') T_BLOCKED=$((T_BLOCKED + 1)) ;;
        *)
            printf '  FAIL  %s: RESULT 行无法解析：%s\n' "$name" "$result"
            T_FAIL=$((T_FAIL + 1))
            FAILED_TESTS="$FAILED_TESTS $name(bad-verdict)"
            ;;
    esac
done

# ---- 汇总 -----------------------------------------------------------

echo
echo "== 汇总"
printf '   pass %s / fail %s / blocked %s\n' "$T_PASS" "$T_FAIL" "$T_BLOCKED"

if [ -n "$FAILED_TESTS" ]; then
    echo "   失败的用例:$FAILED_TESTS"
fi

if [ "$T_BLOCKED" -ne 0 ]; then
    echo "== BLOCKED：有 $T_BLOCKED 条用例没跑成（环境不满足）。"
    echo "   这不是通过，也不是失败。修好环境后重跑。"
    exit 2
fi

if [ "$T_FAIL" -ne 0 ]; then
    echo "== 有 $T_FAIL 条用例失败"
    exit 1
fi

if [ "$T_PASS" -eq 0 ]; then
    # 一个用例都没跑到，同样不许算通过 —— 这条是踩过的坑：
    # 曾经有一个「跑到了但一个断言都没执行」的检查，输出和通过无法区分。
    echo "== 一条用例都没跑到：不能算通过"
    exit 2
fi

echo "== 全部通过（$T_PASS 条）"
exit 0
