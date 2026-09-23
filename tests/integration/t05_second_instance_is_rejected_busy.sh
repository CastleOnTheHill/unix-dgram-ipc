#!/usr/bin/env bash
# ---------------------------------------------------------------------
# t05 —— 同一个模块被两个进程抢注册：只有一个赢（黑盒）
#
# 想证明的这件事：独占性靠的是**文件锁**，而不是配置或约定。而且——
# 这条更关键——**输的那个不许破坏赢的那个**。
#
# 为什么这一点特别值得单独测：以前版本的一个典型错误是「检测到残留就先
# 删掉再 bind」，于是第二个进程会把第一个进程正在用的 socket 文件删掉、
# 自己 bind 成功，两个实例都「注册成功」了，但消息开始随机丢。
# 这种问题在现场表现为「偶发丢消息」，几乎不可能从现象倒推。
# ---------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lab_guard.sh
. "$HERE/lab_guard.sh"
# shellcheck source=common.sh
. "$HERE/common.sh"

TCase t05_second_instance_is_rejected_busy

LabNew || { TFail "建实验目录失败"; TFinish; }

WriteConf "$ConfPath" integns alpha beta

# ---- 第一个 beta：正常注册，服务 3 秒 ----
: >"$JournalB"
{
    printf 'handler 7 note\n'
    printf 'sleep 8000\n'
    printf 'stats\n'
    printf 'stop\n'
} >"$LAB/b.script"

StartMod "$JournalB" "$LAB" beta --conf "$ConfPath" --module beta \
    --ns integns --script "$LAB/b.script"
PID_B1="$LAST_PID"
JWait "$JournalB" '^READY beta ' 3000 || TFail "第一个 beta 没能进入 READY"

# 记下端点文件的 inode：用来证明第二个进程没有把它换掉。
if [ -e "$LAB/beta.sock" ]; then
    INODE_BEFORE=$(ls -i "$LAB/beta.sock" | awk '{print $1}')
else
    INODE_BEFORE=""
    TFail "第一个 beta 没有建立端点文件"
fi

# ---- 第二个 beta：同一个模块，必须 BUSY ----
: >"$LAB/b2.journal"
StartMod "$LAB/b2.journal" "$LAB" beta --conf "$ConfPath" --module beta \
    --ns integns
PID_B2="$LAST_PID"
JWait "$LAB/b2.journal" '^REGFAIL |^DONE ' 3000 \
    || TFail "第二个 beta 既没报 REGFAIL 也没结束"

TCheck "第二个实例被判为 busy" grep -qE '^REGFAIL busy$' "$LAB/b2.journal"
# REGFAIL 路径的退出码是 3（注册失败），不是 0。
TCheck "第二个实例的退出码是 3" grep -qE '^DONE 3$' "$LAB/b2.journal"

WaitMod "$PID_B2" 3000 || TFail "第二个 beta 没能在 3s 内退出"

# ---- 断言：赢家没被破坏 ----
if [ -n "$INODE_BEFORE" ] && [ -e "$LAB/beta.sock" ]; then
    INODE_AFTER=$(ls -i "$LAB/beta.sock" | awk '{print $1}')
    if [ "$INODE_BEFORE" = "$INODE_AFTER" ]; then
        TCheck "赢家的端点文件还是原来那一个（inode 未变）" true
    else
        TFail "端点文件被第二个实例替换了（inode $INODE_BEFORE -> $INODE_AFTER）"
    fi
else
    TFail "赢家的端点文件不见了"
fi
TCheck "第一个 beta 仍在运行" kill -0 "$PID_B1"

# ---- 而且赢家还能正常干活 ----
: >"$JournalA"
{
    printf 'post beta 7 stillalive\n'
    printf 'stop\n'
} >"$LAB/a.script"

StartMod "$JournalA" "$LAB" alpha --conf "$ConfPath" --module alpha \
    --ns integns --script "$LAB/a.script"
PID_A="$LAST_PID"
JWait "$JournalA" '^DONE ' 4000 || TFail "alpha 没能结束"
TCheck "alpha 发送成功" grep -qE '^SEND beta ok$' "$JournalA"
JWait "$JournalB" '^RECV alpha 7 11 stillalive$' 2000 \
    || TFail "赢家没能收到注册竞争期间发来的报文"

StopMod "$PID_A"
WaitMod "$PID_B1" 10000 || TFail "第一个 beta 没能在 10s 内退出"

LabClean
TFinish
