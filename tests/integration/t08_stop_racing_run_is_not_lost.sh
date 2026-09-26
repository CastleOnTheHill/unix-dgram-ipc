#!/usr/bin/env bash
# ---------------------------------------------------------------------
# t08 —— 「停止请求早于 Run() 到达」不许被吃掉（黑盒回归）
#
# 想证明的这件事：一个只发一条就退的进程，**每一轮**都必须退得出来。
#
# 为什么值得单独一条用例：这是一个真实存在过、而且能把进程**永久挂死**的
# bug 的回归。宿主线程跑 IpcRefHostRun()，主线程跑脚本、收尾时调
# IpcRefHostStop() 再 pthread_join(宿主线程)。如果 Stop 已经执行完、而宿主
# 线程还没被调度进 Run()，老代码里的 Run() 会把停止位**无条件清零** ——
# 停止请求被整个吃掉：select 线程按 200ms 超时无限转，两个 join 互等，
# 进程再也退不出去。
#
# 复现的要点是「让脚本跑完并 Stop 的速度快过新线程被调度」。这里把被测进程
# 钉到单核（taskset -c 0）把这个窗口变成必然：钉核之后老代码 40/40 稳定
# 复现；不钉核时只在负载下偶发（在集成测试里就表现为某条用例里
# 「发送方进程 4s 内没结束」，而且只在脚本里没有 sleep 的进程上出现）。
#
# 所以这条用例**必须真的重复多轮**：单轮通过什么都说明不了，它的失败形态
# 本来就是偶发 —— 掷一次骰子不叫验证。
#
# 轮数可用 IPC_STOP_RACE_ROUNDS 调整。
# ---------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lab_guard.sh
. "$HERE/lab_guard.sh"
# shellcheck source=common.sh
. "$HERE/common.sh"

TCase t08_stop_racing_run_is_not_lost

if ! command -v taskset >/dev/null 2>&1; then
    # 不钉核这个窗口就只是偶发，用例会退化成掷骰子 —— 与其给一个不稳的
    # 结论，不如明说「没跑成」。
    printf '  BLOCKED  %s: 没有 taskset（util-linux 的一部分），钉不了核\n' \
        "$T_CASE"
    printf '           不钉核这个竞态只是偶发，跑出来的结果没有判定力\n'
    T_CASE_BLOCKED=1
    TFinish
fi

ROUNDS="${IPC_STOP_RACE_ROUNDS:-25}"

LabNew || { TFail "建实验目录失败"; TFinish; }

WriteConf "$ConfPath" racens beta alpha

# ---- beta：长命对端，保证每一轮 post 都有落点 ----
: >"$JournalB"
{
    printf 'handler 7 note\n'
    printf 'sleep 30000\n'
} >"$LAB/b.script"

StartMod "$JournalB" "$LAB" beta --conf "$ConfPath" --module beta \
    --ns racens --script "$LAB/b.script"
PID_B="$LAST_PID"
if ! JWait "$JournalB" '^READY beta ' 3000; then
    TFail "beta 没能进入 READY"
    StopMod "$PID_B"; LabClean; TFinish
fi

# ---- alpha：最短脚本。没有 sleep，所以它会和宿主线程抢着跑完 ----
: >"$LAB/a.script"
{
    printf 'post beta 7 x\n'
    printf 'stop\n'
} >"$LAB/a.script"

# 有界等待 + 超时取证。
#
# 这里不用 WaitMod，唯一理由是它超时时**先杀进程**，而这条用例最需要在
# 杀之前看一眼那几条线程停在哪：那正是区分「先到的 Stop 被吃掉」和
# 「别的原因卡住」的证据。gdb 在最小环境里不一定有，/proc 一定有。
WaitOrDump() {
    _wd_pid="$1"; _wd_ms="${2:-3000}"; _wd_spent=0

    while kill -0 "$_wd_pid" 2>/dev/null; do
        if [ "$_wd_spent" -ge "$_wd_ms" ]; then
            printf '  !! 进程 %s 在 %sms 内没退出，各线程现场：\n' \
                "$_wd_pid" "$_wd_ms"
            for _t in /proc/"$_wd_pid"/task/*; do
                [ -r "$_t/wchan" ] || continue
                printf '       tid=%-7s comm=%-14s wchan=%s\n' \
                    "${_t##*/}" "$(cat "$_t/comm" 2>/dev/null)" \
                    "$(cat "$_t/wchan" 2>/dev/null)"
            done
            printf '       判据：一条停在 core_sys_select、另有多条停在 futex\n'
            printf '       等 join —— 那就是「先到的 Stop 被吃掉了」。\n'
            kill "$_wd_pid" 2>/dev/null || true
            wait "$_wd_pid" 2>/dev/null || true
            return 1
        fi
        sleep 0.02
        _wd_spent=$((_wd_spent + 20))
    done
    wait "$_wd_pid" 2>/dev/null
    return 0
}

HUNG=0
HUNG_ROUNDS=""
_i=1
while [ "$_i" -le "$ROUNDS" ]; do
    : >"$JournalA"

    taskset -c 0 "$IPC_TEST_BIN/ipc_testmod" --conf "$ConfPath" --module alpha \
        --ns racens --script "$LAB/a.script" --journal "$JournalA" \
        >"$LAB/a.out" 2>&1 < /dev/null &
    PID_A=$!

    if ! WaitOrDump "$PID_A" 3000; then
        HUNG=$((HUNG + 1))
        HUNG_ROUNDS="$HUNG_ROUNDS $_i"
        printf '  !! 第 %s 轮的 journal：\n' "$_i"
        sed 's/^/       /' "$JournalA" 2>/dev/null || true
    fi

    # 挂住的那一轮是被 kill 掉的，端点文件会残留；下一轮不能被它影响
    # （库虽然会清理陈旧端点，但那是被测行为，不该让回归用例去依赖它）。
    rm -f "$LAB/alpha.sock" "$LAB/alpha.sock.lock"
    _i=$((_i + 1))
done

if [ "$HUNG" -eq 0 ]; then
    TCheck "连续 ${ROUNDS} 轮「发一条就退」的进程全部退得出来" true
else
    TFail "${ROUNDS} 轮里有 ${HUNG} 轮挂住（轮次:$HUNG_ROUNDS）"
fi

StopMod "$PID_B"
LabClean
TFinish
