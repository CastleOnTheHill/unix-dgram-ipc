#!/usr/bin/env bash
# ---------------------------------------------------------------------
# t02 —— 跨进程同步请求 / 回复（黑盒）
#
# 想证明的这件事：一次 IpcSend 能真的等到对端的回复，而且回的**内容**
# 是这一条请求的回复（不是「有个回复来了」）。
#
# 这条用例同时是「宿主线程模型」的验收：请求方的主线程阻塞在 IpcSend 上，
# 而回复必须由**另一个线程**（宿主的 select 线程）读回来。如果参考宿主的
# 线程安排错了，这里会一直超时 —— 也就是说，这条用例同时测了库和宿主。
# ---------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lab_guard.sh
. "$HERE/lab_guard.sh"
# shellcheck source=common.sh
. "$HERE/common.sh"

TCase t02_sync_request_gets_a_reply

LabNew || { TFail "建实验目录失败"; TFinish; }

WriteConf "$ConfPath" integns alpha beta

# beta：把 event 7 注册成 echo（回 "echo:" + 收到的内容），服务 2 秒。
: >"$JournalB"
{
    printf 'handler 7 echo\n'
    printf 'sleep 2000\n'
    printf 'stats\n'
    printf 'stop\n'
} >"$LAB/b.script"

StartMod "$JournalB" "$LAB" beta --conf "$ConfPath" --module beta \
    --ns integns --script "$LAB/b.script"
PID_B="$LAST_PID"
JWait "$JournalB" '^READY beta ' 3000 || TFail "beta 没能进入 READY"

# alpha：一条同步请求，必须拿到回复；然后退。
: >"$JournalA"
{
    printf 'send beta 7 ping123\n'
    printf 'stats\n'
    printf 'stop\n'
} >"$LAB/a.script"

StartMod "$JournalA" "$LAB" alpha --conf "$ConfPath" --module alpha \
    --ns integns --script "$LAB/a.script"
PID_A="$LAST_PID"

JWait "$JournalA" '^DONE ' 5000 || TFail "alpha 没能在 5s 内结束"

# ---- 断言：发出去的是 REQ，回来的是那一条的回复 ----
TCheck "alpha 的同步发送成功" grep -qE '^SEND beta ok$' "$JournalA"
# "echo:" 5 字节 + "ping123" 7 字节 = 12
TCheck "回复是这一条请求的回复（内容为 echo:ping123，12 字节）" \
    grep -qE '^GOT 12 echo:ping123$' "$JournalA"
TCheck "beta 收到的是 REQ 而不是 POST" grep -qE '^RECV alpha 7 7 ping123$' "$JournalB"
TCheck "beta 回复成功" grep -qE '^REPLY alpha ok$' "$JournalB"

JStatIs "$JournalA" replyMatched 1
JStatIs "$JournalA" replyUnmatched 0
JStatIs "$JournalA" pendingTimeout 0
JStatIs "$JournalB" replySent 1
JStatIs "$JournalB" recvDelivered 1

StopMod "$PID_A"
WaitMod "$PID_B" 4000 || TFail "beta 没能在 4s 内退出"

LabClean
TFinish
