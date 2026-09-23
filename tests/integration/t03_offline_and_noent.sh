#!/usr/bin/env bash
# ---------------------------------------------------------------------
# t03 —— 「对端离线」和「对端不存在」必须是两个不同的错（黑盒）
#
# 这两个错在运维上的处理完全不同：
#   OFFLINE（配置里有、现在没端点）—— 启动顺序问题，等一会儿或重试；
#   NOENT（配置里根本没有）      —— 配置写错了，重试一万次也没用。
# 如果库把它们混成一个，现场就只能靠猜。所以这条用例专门钉住这个区分。
# ---------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lab_guard.sh
. "$HERE/lab_guard.sh"
# shellcheck source=common.sh
. "$HERE/common.sh"

TCase t03_offline_and_noent_are_different

LabNew || { TFail "建实验目录失败"; TFinish; }

WriteConf "$ConfPath" integns alpha beta

# 只起 alpha。beta 只在配置表里、没有端点。
: >"$JournalA"
{
    printf 'post beta 7 x\n'      # 配置里有、端点不在 -> offline
    printf 'post nosuch 7 x\n'    # 配置里根本没有 -> noent
    printf 'post alpha 7 x\n'     # 发给自己 -> 应该成功
    printf 'stats\n'
    printf 'stop\n'
} >"$LAB/a.script"

StartMod "$JournalA" "$LAB" alpha --conf "$ConfPath" --module alpha \
    --ns integns --script "$LAB/a.script"
PID_A="$LAST_PID"

JWait "$JournalA" '^DONE ' 4000 || TFail "alpha 没能在 4s 内结束"

TCheck "对端离线报 offline" grep -qE '^SEND beta offline$' "$JournalA"
TCheck "对端不存在报 noent" grep -qE '^SEND nosuch noent$' "$JournalA"
TCheck "发给自己成功" grep -qE '^SEND alpha ok$' "$JournalA"

# 三条都会走到「真的试着发」，所以 sendAttempts = 3。
# 前两条进 sendFailed，第三条进 sendEnqueued。
JStatIs "$JournalA" sendAttempts 3
JStatIs "$JournalA" sendFailed 2
JStatIs "$JournalA" sendEnqueued 1

StopMod "$PID_A"
LabClean
TFinish
