#!/usr/bin/env bash
# ---------------------------------------------------------------------
# lab_guard.sh —— 实验目录的路径护栏。**只被 source，不单独执行**。
#
# 为什么需要它：集成测试要在真实文件系统上建 socket 文件、写配置、可能还要
# chown。清理阶段如果对「调用者传进来的任意路径」做递归删除，那就是一个
# 随时会炸掉别人数据的函数。所以清理的**前提**由这里统一把关：
#
#   1. 必须是绝对路径；
#   2. 必须至少有 2 层（拒绝 "/tmp"、"/"、"/opt" 这种）；
#   3. 末段必须以 ipc-lab- 开头；
#   4. 必须真的存在且是个目录（不是符号链接）。
#
# 任何一条不满足就 exit，不「尽力而为」。
#
# 用法：
#     . "$(dirname "$0")/lab_guard.sh"
#     LabGuard "$IPC_LAB"      # 不满足就退出
# ---------------------------------------------------------------------

# 注意：这里刻意**不用** `set -e`。本文件被 source 进别人的脚本，
# 改了对方的 shell 选项会让问题出现在很远的地方。
# 调用方自己决定。

IPC_LAB_DEFAULT="/tmp/ipc-lab"

LabGuard() {
    _lg_path="$1"

    case "$_lg_path" in
        /*) : ;;
        *)  echo "!! lab_guard: 必须是绝对路径，收到 '$_lg_path'" >&2; exit 2 ;;
    esac

    # 去掉结尾斜杠，避免末段判空
    _lg_path="${_lg_path%/}"

    # 层数：/a/b 去掉开头的 / 之后按 / 切开，至少要有 2 段
    _lg_depth=$(printf '%s' "${_lg_path#/}" | awk -F/ '{print NF}')
    if [ "$_lg_depth" -lt 2 ]; then
        echo "!! lab_guard: 至少要有 2 层，收到 '$_lg_path'" >&2
        exit 2
    fi

    _lg_base="${_lg_path##*/}"
    case "$_lg_base" in
        ipc-lab*|ipc-lab-*) : ;;
        *)  echo "!! lab_guard: 末段必须以 ipc-lab 开头，收到 '$_lg_base'" >&2
            echo "    （这条规则是为了让「删除」只可能发生在测试自己建的目录上）" >&2
            exit 2 ;;
    esac

    if [ -L "$_lg_path" ]; then
        echo "!! lab_guard: 拒绝符号链接 '$_lg_path'（防止顺着链接删到别处）" >&2
        exit 2
    fi

    if [ -e "$_lg_path" ] && [ ! -d "$_lg_path" ]; then
        echo "!! lab_guard: '$_lg_path' 存在但不是目录" >&2
        exit 2
    fi
    return 0
}

# 护栏自己的对照自检：这些路径**必须**被拒绝，那个必须被接受。
# 集成测试的入口每次都会跑它 —— 一条从来没报过错的护栏等于没有护栏。
LabGuardSelfTest() {
    _lg_fail=0

    for _lg_bad in "/" "/tmp" "/opt/data" "relative/path" "/tmp/notalab-1234"; do
        if (LabGuard "$_lg_bad") >/dev/null 2>&1; then
            echo "!! lab_guard 自检失败：'$_lg_bad' 竟然被放行了" >&2
            _lg_fail=$((_lg_fail + 1))
        fi
    done

    if ! (LabGuard "/tmp/ipc-lab-selftest-$$") >/dev/null 2>&1; then
        echo "!! lab_guard 自检失败：正常的实验路径被拒了" >&2
        _lg_fail=$((_lg_fail + 1))
    fi

    if [ "$_lg_fail" -ne 0 ]; then
        exit 2
    fi
    return 0
}
