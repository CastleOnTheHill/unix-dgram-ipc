#!/usr/bin/env bash
# ---------------------------------------------------------------------
# t01 —— 两个独立进程直连（黑盒）
#
# 想证明的这件事：**没有中心转发**也能通。两个模块进程各自 bind 一个
# 数据报端点，一个直接投给另一个，中间没有任何第三方进程、没有连接建立
# 过程。这正是本次改造的立项理由，所以它应该是第一条用例。
#
# 怎么看出来「没有中心」：全程只有两个进程被起过（$PID_B 与 alpha 的前台
# 进程），而报文从 alpha 到了 beta。这条断言反过来写不出来 —— 我们能证明
# 的是「不需要中心」，不是「绝不存在中心」；后一句话要靠架构评审，不靠
# 集成测试。措辞别夸大。
# ---------------------------------------------------------------------
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lab_guard.sh
. "$HERE/lab_guard.sh"
# shellcheck source=common.sh
. "$HERE/common.sh"

TCase t01_two_processes_connect_directly

LabNew || { TFail "建实验目录失败"; TFinish; }

WriteConf "$ConfPath" integns alpha beta

# ---- beta：起一个服务端，声明只处理 event 7，然后服务 1.5 秒 ----
: >"$JournalB"
{
    printf 'handler 7 note\n'
    printf 'sleep 1500\n'
    printf 'stats\n'
    printf 'stop\n'
} >"$LAB/b.script"

StartMod "$JournalB" "$LAB" beta --conf "$ConfPath" --module beta \
    --ns integns --script "$LAB/b.script"
PID_B="$LAST_PID"

if ! JWait "$JournalB" '^READY beta ' 3000; then
    TFail "beta 没能进入 READY"
    StopMod "$PID_B"; LabClean; TFinish
fi

# 端点文件必须真的存在 —— 这是「直连」的物理证据。
TCheck "beta 的端点文件已建立" test -S "$LAB/beta.sock"
TCheck "alpha 的端点文件此时还不该存在" test ! -e "$LAB/alpha.sock"

# ---- alpha：在一个进程里发一条就退 ----
: >"$JournalA"
{
    printf 'handler 9 note\n'
    printf 'post beta 7 hello\n'
    printf 'stats\n'
    printf 'stop\n'
} >"$LAB/a.script"

StartMod "$JournalA" "$LAB" alpha --conf "$ConfPath" --module alpha \
    --ns integns --script "$LAB/a.script"
PID_A="$LAST_PID"

JWait "$JournalA" '^DONE ' 4000 || TFail "alpha 没能在 4s 内结束"

# beta 的 journal 里 `RECV` 记的是**来源**模块，不是收件方 —— 所以等的是
# `RECV alpha`。原来写的是 `^RECV beta`，那个正则**结构上永远匹配不上**，
# 于是这条 JWait 每次都要把 2000ms 的截止烧完；更坏的是它顺手把后面的
# 断言推到了 beta 打完 STATS 之后，于是掩盖了两处真问题（见下面两段注释）。
JWait "$JournalB" '^RECV alpha ' 2000 || TFail "beta 没在 2s 内收到 alpha 的报文"

# beta 的脚本是「服务 1.5s 再停」，它自己的 STATS 要等那一觉睡完才出来。
# 不先等它，下面两条 JStatIs 读到的是空 —— 而空值会显示成「期望 1 实际空」，
# 看起来像计数错，其实只是还没打印。t06 里本来就有这一步，t01 漏了。
JWait "$JournalB" '^STATS ' 4000 || TFail "beta 没有打出 STATS"

# ---- 断言 ----
TCheck "alpha 报发送成功" grep -qE '^SEND beta ok$' "$JournalA"
TCheck "beta 收到来自 alpha 的 event 7、5 字节、内容 hello" \
    grep -qE '^RECV alpha 7 5 hello$' "$JournalB"

JStatIs "$JournalA" sendEnqueued 1
JStatIs "$JournalA" sendFailed 0
JStatIs "$JournalB" recvDelivered 1
JStatIs "$JournalB" recvRejected 0

# 两个进程都在自己的模块名下建了端点 —— 这就是「直连」而不是「连到中心」。
#
# 这里**不能**在 alpha 结束之后去看文件。alpha 的脚本是「发一条就退」，
# 它的寿命只有几十毫秒，退出时会把端点删掉；原来那条 `test -S beta.sock`
# 放在等 alpha DONE 之后，读到的一定是「文件已经没了」—— 那测的不是实现，
# 是时钟。实测：alpha 在 +95ms 就 DONE，beta 在 +1622ms 自己停掉并 unlink，
# 而断言在 +2.4s 才执行。
#
# 改成从两条 READY 行里取**端点路径**，判据全部是构造性的：
#   1) 路径等于 <lab>/<自己的模块名>.sock —— 每个模块各有各的端点；
#   2) 两条路径互不相同 —— 不是两个模块共用一个中心端点。
#
# 为什么不必再用 `test -S` 去物理校验 alpha.sock：能打出 `READY` 就说明
# IpcRegister 成功了，而它内部（src/ipc_io.c 的 ApplyOwnership）在 bind
# 之后已经 lstat 回读校验过「这个路径确实是个 socket 且模式/属主正确」，
# 不满足就直接以 IPC_ERR_PERM 返回、根本走不到 READY。库自己验过的证据
# 比 shell 再 `-S` 一次更硬 —— 而 shell 那一看是有 70ms 竞态窗口的。
APath="$(JLine "$JournalA" '^READY alpha ' | awk '{print $3}')"
BPath="$(JLine "$JournalB" '^READY beta ' | awk '{print $3}')"
TCheck "alpha 在自己的模块名下建了端点（$LAB/alpha.sock）" \
    test "$APath" = "$LAB/alpha.sock"
TCheck "beta 在自己的模块名下建了端点（$LAB/beta.sock）" \
    test "$BPath" = "$LAB/beta.sock"
TCheck "两个端点不是同一个（各有各的端点，不存在共用中心）" \
    test "$APath" != "$BPath"

StopMod "$PID_A"
WaitMod "$PID_B" 4000 || TFail "beta 没能在 4s 内退出"
JWait "$JournalB" '^DONE ' 1000

LabClean
TFinish
