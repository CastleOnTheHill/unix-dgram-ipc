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
JWait "$JournalB" '^RECV beta' 2000

# ---- 断言 ----
TCheck "alpha 报发送成功" grep -qE '^SEND beta ok$' "$JournalA"
TCheck "beta 收到来自 alpha 的 event 7、5 字节、内容 hello" \
    grep -qE '^RECV alpha 7 5 hello$' "$JournalB"

JStatIs "$JournalA" sendEnqueued 1
JStatIs "$JournalA" sendFailed 0
JStatIs "$JournalB" recvDelivered 1
JStatIs "$JournalB" recvRejected 0

# 两个进程都在自己的模块名下建了端点 —— 这就是「直连」而不是「连到中心」。
TCheck "beta 的端点仍然存在" test -S "$LAB/beta.sock"

StopMod "$PID_A"
WaitMod "$PID_B" 4000 || TFail "beta 没能在 4s 内退出"
JWait "$JournalB" '^DONE ' 1000

LabClean
TFinish
