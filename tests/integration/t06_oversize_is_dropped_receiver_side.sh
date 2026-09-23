#!/usr/bin/env bash
# ---------------------------------------------------------------------
# t06 —— 接收方配小 maxPayload 时：静默丢弃，而发送方仍然看到成功（黑盒）
#
# 想证明的这件事是设计里**明写的陷阱**，不是 bug：
#
#   接收端的缓冲是 `IPC_HDR_SIZE + 自己的 maxPayload`。所以同一个命名空间里
#   如果两个模块配了不同的 maxPayload，配小的那个会丢掉大报文 ——
#   而**发送方看到的是 IPC_OK**，它完全不知道对方丢了。
#
# 之所以要专门测它：这条性质会让「我发了啊，你没收到？」变成一个查不出
# 来的问题。集成测试要能一眼看出「丢在了接收端、记在 recvRejTrunc 上」，
# 运维才知道去哪儿看。
#
# 顺带说明：这条不是「发送方不该报 OK」。发送成功与交付成功本来就是两件
# 事，库把它们分成了两组计数器。要交付确认只能靠业务层自己的应答。
# ---------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lab_guard.sh
. "$HERE/lab_guard.sh"
# shellcheck source=common.sh
. "$HERE/common.sh"

TCase t06_oversize_is_dropped_receiver_side

LabNew || { TFail "建实验目录失败"; TFinish; }

WriteConf "$ConfPath" integns alpha beta

# beta 只给 64 字节载荷位；alpha 给 1024。
: >"$JournalB"
{
    printf 'handler 7 note\n'
    printf 'sleep 2500\n'
    printf 'stats\n'
    printf 'stop\n'
} >"$LAB/b.script"

StartMod "$JournalB" "$LAB" beta --conf "$ConfPath" --module beta \
    --ns integns --max-payload 64 --script "$LAB/b.script"
PID_B="$LAST_PID"
JWait "$JournalB" '^READY beta ' 3000 || TFail "beta 没能进入 READY"

# 造一条 300 字节的载荷。
BIG=$(awk 'BEGIN { for (i = 0; i < 300; i++) printf "Q"; }')

: >"$JournalA"
{
    printf 'post beta 7 %s\n' "$BIG"
    printf 'stats\n'
    printf 'stop\n'
} >"$LAB/a.script"

StartMod "$JournalA" "$LAB" alpha --conf "$ConfPath" --module alpha \
    --ns integns --max-payload 1024 --script "$LAB/a.script"
PID_A="$LAST_PID"
JWait "$JournalA" '^DONE ' 5000 || TFail "alpha 没能在 5s 内结束"

# 先让 beta 有时间把那条报文读掉并丢掉。
JWait "$JournalB" '^STATS ' 3000 || true

# ---- 断言：发送方看到成功 ----
TCheck "发送方看到的是 ok（它不知道对方丢了）" \
    grep -qE '^SEND beta ok$' "$JournalA"
JStatIs "$JournalA" sendEnqueued 1
JStatIs "$JournalA" sendFailed 0

# ---- 接收方：丢在截断这一格，而且没有进业务层 ----
JStatIs "$JournalB" recvRejTrunc 1
JStatIs "$JournalB" recvRejected 1
JStatIs "$JournalB" recvDelivered 0
TCheck "beta 的业务回调一次都没跑（报文根本没到业务层）" \
    bash -c "! grep -qE '^RECV ' '$JournalB'"

# 对照：把 maxPayload 调大之后同样的报文必须能收到。
# 没有这个对照，上面那条「丢弃」的结论也可能是「根本发不出去」造成的。
: >"$JournalC"
{
    printf 'handler 7 note\n'
    printf 'sleep 2500\n'
    printf 'stats\n'
    printf 'stop\n'
} >"$LAB/c.script"

# 用第三个模块名，避免撞上 beta 的端点（beta 还在跑）。
WriteConf "$ConfPath" integns alpha beta gamma
StartMod "$JournalC" "$LAB" gamma --conf "$ConfPath" --module gamma \
    --ns integns --max-payload 1024 --script "$LAB/c.script"
PID_C="$LAST_PID"
JWait "$JournalC" '^READY gamma ' 3000 || TFail "gamma 没能进入 READY"

: >"$LAB/a2.journal"
{
    printf 'post gamma 7 %s\n' "$BIG"
    printf 'stop\n'
} >"$LAB/a2.script"

StartMod "$LAB/a2.journal" "$LAB" alpha --conf "$ConfPath" --module alpha \
    --ns integns --max-payload 1024 --script "$LAB/a2.script"
PID_A2="$LAST_PID"
JWait "$LAB/a2.journal" '^DONE ' 4000 || TFail "第二个 alpha 没能结束"
JWait "$JournalC" '^RECV alpha 7 300 ' 3000 \
    || TFail "对照：maxPayload 够大时这条报文**应该**被收到"

StopMod "$PID_A"
StopMod "$PID_A2"
WaitMod "$PID_B" 4000 || true
WaitMod "$PID_C" 4000 || true

LabClean
TFinish
