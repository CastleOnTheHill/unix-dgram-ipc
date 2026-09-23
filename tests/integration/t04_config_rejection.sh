#!/usr/bin/env bash
# ---------------------------------------------------------------------
# t04 —— 配置表非法时整表拒绝，且进程不许留下半成品（黑盒）
#
# 想证明的这件事有两半，都很要紧：
#   1. 非法的配置表返回的是 IPC_ERR_CONFIG，不是「跳过那一行继续」——
#      静默跳过最危险：少一行配置的后果是「某个模块永远收不到消息」，
#      而现象上看不出来；
#   2. 拒绝之后**不留半成品**：不许留下 socket 文件、不许留下锁。
#      一个「注册失败但端点文件已经建出来了」的进程会让下一次启动
#      撞在残留上，于是问题往外传了一层。
# ---------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lab_guard.sh
. "$HERE/lab_guard.sh"
# shellcheck source=common.sh
. "$HERE/common.sh"

TCase t04_invalid_config_is_rejected_whole

LabNew || { TFail "建实验目录失败"; TFinish; }

# ---- 情形 1：同一个 (ns, moduleId) 出现两次 ----
{
    printf 'integns alpha %s %s/alpha.sock\n' "$(id -u)" "$LAB"
    printf 'integns alpha %s %s/alpha2.sock\n' "$(id -u)" "$LAB"
} >"$LAB/dup.conf"

: >"$LAB/dup.journal"
StartMod "$LAB/dup.journal" "$LAB" alpha --conf "$LAB/dup.conf" --module alpha \
    --ns integns
PID_D="$LAST_PID"
if JWait "$LAB/dup.journal" '^REGFAIL |^DONE ' 3000; then
    TCheck "重复 (ns,moduleId) 被判为 config 错误" \
        grep -qE '^REGFAIL config$' "$LAB/dup.journal"
else
    TFail "重复配置的进程既没报 REGFAIL 也没结束"
fi
WaitMod "$PID_D" 2000 || true
TCheck "重复配置失败后没有留下端点文件" test ! -e "$LAB/alpha.sock"

# ---- 情形 2：路径重复（两条不同模块指向同一个路径） ----
{
    printf 'integns alpha %s %s/shared.sock\n' "$(id -u)" "$LAB"
    printf 'integns beta %s %s/shared.sock\n' "$(id -u)" "$LAB"
} >"$LAB/duppath.conf"

: >"$LAB/duppath.journal"
StartMod "$LAB/duppath.journal" "$LAB" alpha --conf "$LAB/duppath.conf" \
    --module alpha --ns integns
PID_E="$LAST_PID"
JWait "$LAB/duppath.journal" '^REGFAIL |^DONE ' 3000 \
    || TFail "路径重复的进程既没报 REGFAIL 也没结束"
TCheck "重复 path 被判为 config 错误" \
    grep -qE '^REGFAIL config$' "$LAB/duppath.journal"
WaitMod "$PID_E" 2000 || true
TCheck "重复路径失败后没留下端点文件" test ! -e "$LAB/shared.sock"

# ---- 情形 3：相对路径（配置规范要求绝对路径） ----
{
    printf 'integns alpha %s relative/alpha.sock\n' "$(id -u)"
} >"$LAB/rel.conf"

: >"$LAB/rel.journal"
StartMod "$LAB/rel.journal" "$LAB" alpha --conf "$LAB/rel.conf" --module alpha \
    --ns integns
PID_F="$LAST_PID"
JWait "$LAB/rel.journal" '^REGFAIL |^DONE ' 3000 \
    || TFail "相对路径的进程既没报 REGFAIL 也没结束"
TCheck "相对路径被判为 config 错误" \
    grep -qE '^REGFAIL config$' "$LAB/rel.journal"
WaitMod "$PID_F" 2000 || true

# ---- 情形 4：模块不在配置表里 -> noent（不是 config） ----
{
    printf 'integns alpha %s %s/alpha.sock\n' "$(id -u)" "$LAB"
} >"$LAB/one.conf"

: >"$LAB/miss.journal"
StartMod "$LAB/miss.journal" "$LAB" alpha --conf "$LAB/one.conf" --module ghost \
    --ns integns
PID_G="$LAST_PID"
JWait "$LAB/miss.journal" '^REGFAIL ' 3000 || TFail "缺模块的进程没有报 REGFAIL"
TCheck "不在配置表里的模块报 noent（而不是 config）" \
    grep -qE '^REGFAIL noent$' "$LAB/miss.journal"
WaitMod "$PID_G" 2000 || true

# ---- 情形 5：命名空间写错 -> noent ----
: >"$LAB/wrongns.journal"
StartMod "$LAB/wrongns.journal" "$LAB" alpha --conf "$LAB/one.conf" --module alpha \
    --ns otherns
PID_H="$LAST_PID"
JWait "$LAB/wrongns.journal" '^REGFAIL ' 3000 || TFail "ns 写错的进程没有报 REGFAIL"
TCheck "ns 写错报 noent" grep -qE '^REGFAIL noent$' "$LAB/wrongns.journal"
WaitMod "$PID_H" 2000 || true

LabClean
TFinish
