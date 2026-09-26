#!/usr/bin/env bash
# NEED_ROOT
# ---------------------------------------------------------------------
# t07 —— 跨 uid 的授权校验（黑盒）
#
# =====================================================================
# 这条用例到底在测什么、以及它**不能**测什么
# =====================================================================
# 能测（本用例的主体）：配置表里的 uid 字段真的在起作用。三个模块分属两个
# uid，各自只能注册属于自己的那一个，注册成功后**双向**互通：
#
#     gamma(root)   --post-->  beta(nobody)    收到
#     beta(nobody)  --post-->  alpha(root)     收到
#
# 还能测（负向）：以 nobody 去注册配置里 uid=0 的 alpha，必须被 uid 自检
# 拒绝。只测「合法的能通」是不够的 —— 那说明不了「非法的会被拒」。
#
# 不能测（必须说清楚，免得被当成「已覆盖」）：
# **接收侧的 uid 不符拒绝**在这条用例里构造不出来。原因是流水线自己堵住了：
# 库发送时 src 永远填自己的 moduleId，而注册时又强制「本进程 uid 必须等于
# 配置里该模块的 uid」。也就是说，通过库本身**发不出**一条「自称 A、但
# 真实 uid 不是 A 的授权 uid」的报文。
#
# 那个拒绝分支真正的防御对象是**不使用本库的发送方**（敌意进程、或者还没
# 升级的老客户端）。所以它只能用手工构造的数据报来测 —— 那属于白盒，已在
# tests/unit/test_forward.c 的 `malformed_datagrams_are_rejected_by_category`
# 里覆盖（伪造 src=imposter 那一步，断言 recvRejCred=1）。
#
# 本项目的纪律：**没测到就说没测到**，不要用一条邻近的用例冒充它。
# =====================================================================
# 跨 uid 互通到底靠什么（这一条原来写错了，记在这里免得再错）
# =====================================================================
# 靠的是**属组 + 组权限**，也就是 IpcModuleOptions.groupName，不是「把实验
# 目录放开到 0777」。库有意把端点设成 0600（只属主可写）；只有显式给了
# groupName 才会变成「chown 到该属组 + 0660」（见 src/ipc_io.c 的
# IPC_MODE_PRIVATE / IPC_MODE_GROUP 与 ApplyOwnership）。
#
# 实测踩过：目录 0777 之后，以 nobody 运行的 beta 向 root 的 alpha.sock 发送
# 仍然拿到 EACCES —— journal 里是 `SEND alpha perm`，而 root 从来没收到过。
# 目录权限管的是「能不能在目录里建文件」，完全管不了「能不能往这个 socket
# 发送」。
#
# 这里直接用 nobody 自己的那个组当共享组：root 当然能 chown 到它，而以
# --regid=<该 gid> 运行的 beta 也在该组里，于是两边都合法。这样就不必往
# 系统里加组，也就不会留下残留。
# =====================================================================
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lab_guard.sh
. "$HERE/lab_guard.sh"
# shellcheck source=common.sh
. "$HERE/common.sh"

TCase t07_cross_uid_authorization

NeedRoot "跨 uid 需要以 root 起第二个进程（setpriv 换 uid）" || TFinish

if ! command -v setpriv >/dev/null 2>&1; then
    printf '  BLOCKED  %s: 没有 setpriv（util-linux 的一部分），换不了 uid\n' \
        "$T_CASE"
    T_CASE_BLOCKED=1
    TFinish
fi

NOBODY_UID=$(id -u nobody 2>/dev/null || echo 65534)
NOBODY_GID=$(id -g nobody 2>/dev/null || echo 65534)
NOBODY_GROUP=$(id -gn nobody 2>/dev/null || echo "")

# 共享组必须能被 getgrnam 解析出来（库走的是 getgrnam，不接受裸 gid）。
if [ -z "$NOBODY_GROUP" ] || ! getent group "$NOBODY_GROUP" >/dev/null 2>&1; then
    printf '  BLOCKED  %s: 取不到 nobody 的属组名（getgrnam 解析不了），\n' \
        "$T_CASE"
    printf '           没法用属组机制搭跨 uid 的台子\n'
    T_CASE_BLOCKED=1
    TFinish
fi

LabNew || { TFail "建实验目录失败"; TFinish; }

# 两个 uid 都要能在这个目录里**建**端点文件（这需要目录的写权限）。
# 端点本身怎么可写由库的属组机制负责，与这里无关。
chmod 0777 "$LAB" || TFail "放开实验目录权限失败"

{
    printf 'integns alpha 0 %s/alpha.sock\n' "$LAB"
    printf 'integns beta %s %s/beta.sock\n' "$NOBODY_UID" "$LAB"
    printf 'integns gamma 0 %s/gamma.sock\n' "$LAB"
} >"$ConfPath"
chmod 0644 "$ConfPath"

# ---- alpha（root）：服务 7 秒，只记录不回 ----
: >"$JournalA"
{
    printf 'handler 7 note\n'
    printf 'sleep 7000\n'
    printf 'stats\n'
    printf 'stop\n'
} >"$LAB/a.script"

StartMod "$JournalA" "$LAB" alpha --conf "$ConfPath" --module alpha \
    --ns integns --group "$NOBODY_GROUP" --script "$LAB/a.script"
PID_A="$LAST_PID"
JWait "$JournalA" '^READY alpha ' 4000 || TFail "alpha(root) 没能注册成功"

# alpha 的端点：属主是 root（进程自己），属组是共享组，模式 0660。
# 这三条一起才说明「库真的按 groupName 设了属组」—— 只看属主的话，
# 0600 和 0660 都会满足，而 0600 恰恰是跨 uid 不通的那种。
TCheck "alpha 的端点属主是 0" \
    test "$(stat -c %u "$LAB/alpha.sock" 2>/dev/null)" = "0"
TCheck "alpha 的端点是 0660（给了属组就该是属组可写，而不是 0600）" \
    test "$(stat -c %a "$LAB/alpha.sock" 2>/dev/null)" = "660"
TCheck "alpha 的端点属组是共享组 $NOBODY_GROUP($NOBODY_GID)" \
    test "$(stat -c %g "$LAB/alpha.sock" 2>/dev/null)" = "$NOBODY_GID"

INODE_A_BEFORE=""
if [ -e "$LAB/alpha.sock" ]; then
    INODE_A_BEFORE=$(ls -i "$LAB/alpha.sock" | awk '{print $1}')
fi

# ---- beta（nobody）：收一条，再发一条给 alpha ----
: >"$JournalB"
{
    printf 'handler 7 note\n'
    printf 'sleep 600\n'                 # 等 gamma 把它那条发过来
    printf 'post alpha 7 fromnobody\n'
    printf 'sleep 2500\n'
    printf 'stats\n'
    printf 'stop\n'
} >"$LAB/b.script"
chmod 0644 "$LAB/b.script"
touch "$JournalB"; chmod 0666 "$JournalB"

(
    cd "$LAB" || exit 2
    exec setpriv --reuid="$NOBODY_UID" --regid="$NOBODY_GID" --clear-groups \
        "$IPC_TEST_BIN/ipc_testmod" --conf "$ConfPath" --module beta \
        --ns integns --group "$NOBODY_GROUP" \
        --script "$LAB/b.script" --journal "$JournalB"
) >"$LAB/b.out" 2>&1 < /dev/null &
PID_B=$!

if JWait "$JournalB" '^READY beta ' 4000; then
    TCheck "beta 的端点属主就是配置里写的那个 uid（$NOBODY_UID）" \
        test "$(stat -c %u "$LAB/beta.sock" 2>/dev/null)" = "$NOBODY_UID"
    TCheck "beta 的端点是 0660" \
        test "$(stat -c %a "$LAB/beta.sock" 2>/dev/null)" = "660"
    TCheck "beta 的端点属组是共享组 $NOBODY_GID" \
        test "$(stat -c %g "$LAB/beta.sock" 2>/dev/null)" = "$NOBODY_GID"
else
    TFail "以 uid $NOBODY_UID 运行的 beta 没能注册成功"
fi

# ---- 负向：以 nobody 冒充 alpha（配置里 uid=0）必须被拒 ----
#
# 这条才是「配置表里的 uid 字段真的在起作用」的直接证据：上面那些断言
# 只能说明「合法的能通」，说明不了「非法的会被拒」。
: >"$LAB/imposter.journal"
chmod 0666 "$LAB/imposter.journal"
(
    cd "$LAB" || exit 2
    exec setpriv --reuid="$NOBODY_UID" --regid="$NOBODY_GID" --clear-groups \
        "$IPC_TEST_BIN/ipc_testmod" --conf "$ConfPath" --module alpha \
        --ns integns --journal "$LAB/imposter.journal" --serve-ms 0
) >"$LAB/imposter.out" 2>&1 < /dev/null &
PID_I=$!

if JWait "$LAB/imposter.journal" '^REGFAIL |^DONE ' 3000; then
    TCheck "以 nobody 注册配置里 uid=0 的 alpha：被判为 perm" \
        grep -qE '^REGFAIL perm$' "$LAB/imposter.journal"
    # 对方注册失败不能反过来破坏赢家：端点文件必须还是原来那一个。
    if [ -n "$INODE_A_BEFORE" ] && [ -e "$LAB/alpha.sock" ]; then
        if [ "$INODE_A_BEFORE" = "$(ls -i "$LAB/alpha.sock" | awk '{print $1}')" ]; then
            TCheck "被拒的注册没有动过 alpha 的端点文件（inode 未变）" true
        else
            TFail "alpha 的端点文件被那个注册失败的进程替换掉了"
        fi
    else
        TFail "alpha 的端点文件不见了"
    fi
else
    TFail "冒充 alpha 的进程既没报 REGFAIL 也没结束"
fi
WaitMod "$PID_I" 3000 || true

# ---- gamma（root）：发一条给 beta ----
: >"$JournalC"
{
    printf 'post beta 7 fromroot\n'
    printf 'stop\n'
} >"$LAB/c.script"

StartMod "$JournalC" "$LAB" gamma --conf "$ConfPath" --module gamma \
    --ns integns --group "$NOBODY_GROUP" --script "$LAB/c.script"
PID_C="$LAST_PID"
JWait "$JournalC" '^DONE ' 4000 || TFail "gamma 没能结束"
TCheck "gamma(root) 发送成功" grep -qE '^SEND beta ok$' "$JournalC"

# ---- 断言：两个方向都通 ----
if [ "${T_CASE_BLOCKED:-0}" -eq 0 ]; then
    JWait "$JournalB" '^RECV gamma 7 8 fromroot$' 3000 \
        || TFail "beta(nobody) 没有收到 root 发来的报文"
    # beta → alpha 这一条是本用例里唯一需要**属组**才可能成立的方向：
    # alpha.sock 属主是 root，nobody 只能靠组权限写进去。
    JWait "$JournalA" '^RECV beta 7 10 fromnobody$' 5000 \
        || TFail "alpha(root) 没有收到 nobody(beta) 发来的报文"
fi

WaitMod "$PID_B" 8000 || TFail "beta 没能在 8s 内退出"
WaitMod "$PID_A" 10000 || true
StopMod "$PID_C"
StopMod "$PID_A"

# 收尾前把目录权限收回来，免得 LabClean 里删文件时因为属主/权限出怪问题。
chmod 0755 "$LAB" 2>/dev/null || true
LabClean
TFinish
