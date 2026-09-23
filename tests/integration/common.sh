#!/usr/bin/env bash
# ---------------------------------------------------------------------
# common.sh —— 集成测试的公共骨架。**只被 source，不单独执行**。
#
# 这里只放三样东西，别的都别往这儿加：
#   1. 一个极小的测试记账（pass / fail / blocked + 退出码）；
#   2. 「等 journal 行」的有界轮询 —— 集成测试**唯一**允许的同步手段；
#   3. 起停模块进程的包装。
#
# =====================================================================
# 为什么不用 sleep
# =====================================================================
# 固定 sleep 会同时带来两种错误：跑得快的机器上白白等，跑得慢的机器上
# 仍然偶然失败。而且失败时看不出「是没发生，还是没等到」。
# 所以这里一律用 `JWait <文件> <正则> <截止毫秒>`：等到就把那件事当作
# 已发生，等不到就带着「当前文件里有哪几行」一起失败。
#
# 一条纪律（上个项目踩过）：有界轮询必须盯住**断言实际读的那个完整表达式**，
# 不能只盯它的一项。曾经有一个用例在「差 2 条、恰好等于队列深度」的时候
# 就醒过来了，于是稳定假失败。所以 JWait 的判据永远是调用方给的那条正则。
# =====================================================================
#
# 环境变量：
#   IPC_TEST_BIN     ipc_testmod 所在目录（默认 <repo>/build/bin）
#   IPC_LAB          实验根目录（默认 /tmp/ipc-lab）
#   IPC_TEST_TIMEOUT_MS  单条 JWait 的默认截止（默认 5000）
#   IPC_INTEG_VERBOSE    1 = 打印每一步
# ---------------------------------------------------------------------

IPC_TEST_BIN="${IPC_TEST_BIN:-}"
IPC_LAB="${IPC_LAB:-/tmp/ipc-lab}"
IPC_TEST_TIMEOUT_MS="${IPC_TEST_TIMEOUT_MS:-5000}"
IPC_INTEG_VERBOSE="${IPC_INTEG_VERBOSE:-0}"

# ---- 记账 ----------------------------------------------------------
#
# 注意：每个 tNN_*.sh 都是**独立进程**（run_all.sh 用 bash 起它），所以
# 这里的计数器根本不会传回入口。曾经想过靠 exit code 传，但那有两个问题：
#   1. exit 2 既可能是「BLOCKED」也可能是「参数写错了」；
#   2. `set -e`、管道、`||` 都可能把真实退出码吃掉。
# 所以改用一行**机器可解析的结论**：
#
#     RESULT <用例名> PASS|FAIL|BLOCKED
#
# 由入口 grep 这一行来汇总。如果一个测试进程结束了却没有打出这一行，
# 入口把它算作 FAIL —— 「没给出结论」不能当成「没出问题」。

T_CASE=""
T_CASE_FAILED=0
T_CASE_BLOCKED=0

# 某个用例开始了。用例名会带进每一条失败信息里。
TCase() {
    T_CASE="$1"
    T_CASE_FAILED=0
    T_CASE_BLOCKED=0
    if [ "$IPC_INTEG_VERBOSE" = "1" ]; then
        printf '  [case] %s\n' "$T_CASE"
    fi
}

# 需要 root 才能做的检查。非 root 时把整条用例标为 BLOCKED —— 不假装通过，
# 也不假装失败：这两种都会把「没测」说成「测了」。
NeedRoot() {
    if [ "$(id -u)" -ne 0 ]; then
        printf '  BLOCKED  %s: 需要 root（%s）\n' "$T_CASE" "$1"
        T_CASE_BLOCKED=1
        return 1
    fi
    return 0
}

TCheck() {
    _tc_desc="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        if [ "$IPC_INTEG_VERBOSE" = "1" ]; then
            printf '     ok   %s\n' "$_tc_desc"
        fi
        return 0
    fi
    printf '  FAIL  %s: %s\n' "$T_CASE" "$_tc_desc"
    T_CASE_FAILED=1
    return 1
}

# 「失败但不中断」：一次用例里我们想一次看到所有失败点，
# 而不是修一个报一个。
TFail() {
    printf '  FAIL  %s: %s\n' "$T_CASE" "$1"
    T_CASE_FAILED=1
    return 1
}

# 每条 tNN 的最后一句。打结论行并按结论给退出码。
TFinish() {
    if [ -z "$T_CASE" ]; then
        TCase "$(basename "${0%.sh}")"
    fi
    if [ "$T_CASE_BLOCKED" -ne 0 ]; then
        printf 'RESULT %s BLOCKED\n' "$T_CASE"
        exit 2
    fi
    if [ "$T_CASE_FAILED" -ne 0 ]; then
        printf 'RESULT %s FAIL\n' "$T_CASE"
        exit 1
    fi
    printf '  pass    %s\n' "$T_CASE"
    printf 'RESULT %s PASS\n' "$T_CASE"
    exit 0
}

# ---- 等 journal 行（有界轮询） --------------------------------------

# JCount <file> <ere-pattern>  -> stdout 是匹配行数
JCount() {
    if [ ! -f "$1" ]; then
        echo 0
        return 0
    fi
    grep -cE "$2" "$1" 2>/dev/null || true
}

# JWait <file> <ere-pattern> [deadline-ms] [min-count]
# 等到匹配行数 >= min-count（默认 1）就返回 0；超时返回 1 并打印现场。
JWait() {
    _jw_file="$1"
    _jw_pat="$2"
    _jw_ms="${3:-$IPC_TEST_TIMEOUT_MS}"
    _jw_need="${4:-1}"
    _jw_step=20
    _jw_spent=0

    while : ; do
        _jw_n=$(JCount "$_jw_file" "$_jw_pat")
        if [ "$_jw_n" -ge "$_jw_need" ]; then
            return 0
        fi
        if [ "$_jw_spent" -ge "$_jw_ms" ]; then
            printf '  !! JWait 超时 %sms：等 %s 里的 /%s/ >= %s，实际 %s\n' \
                "$_jw_ms" "$_jw_file" "$_jw_pat" "$_jw_need" "$_jw_n"
            printf '  !! 该文件现在的全部内容：\n'
            if [ -f "$_jw_file" ]; then
                sed 's/^/       /' "$_jw_file"
            else
                printf '       （文件不存在）\n'
            fi
            return 1
        fi
        sleep 0.02
        _jw_spent=$((_jw_spent + _jw_step))
    done
}

# JLine <file> <ere-pattern>  -> 打印最后一条匹配的行
JLine() {
    grep -E "$2" "$1" 2>/dev/null | tail -1
}

# JStat <file> <key>  -> 从最后一行 STATS 里取某个键的值
# 例：JStat "$JournalA" recvDelivered
JStat() {
    _js_last=$(grep -E '^STATS ' "$1" 2>/dev/null | tail -1)
    if [ -z "$_js_last" ]; then
        printf ''
        return 0
    fi
    printf '%s\n' "$_js_last" | tr ' ' '\n' | grep -E "^$2=" | tail -1 | cut -d= -f2
}

# JStatIs <file> <key> <期望值> —— 断言统计值，失败时把整行打出来
# （只报「期望 1 实际空」不告诉人怎么查，等于没报）
JStatIs() {
    _jsi_got=$(JStat "$1" "$2")
    if [ "$_jsi_got" = "$3" ]; then
        if [ "$IPC_INTEG_VERBOSE" = "1" ]; then
            printf '     ok   %s.%s = %s\n' "$(basename "$1")" "$2" "$3"
        fi
        return 0
    fi
    printf '  FAIL  %s: %s.%s 期望 %s，实际 %s\n' "$T_CASE" "$(basename "$1")" \
        "$2" "$3" "${_jsi_got:-<空>}"
    printf '       该进程最后一行 STATS：%s\n' \
        "$(grep -E '^STATS ' "$1" 2>/dev/null | tail -1)"
    T_CASE_FAILED=1
    return 1
}

# ---- 模块进程 -------------------------------------------------------

# StartMod <journal> <工作目录> <tag> <ipc_testmod 的参数...>
# 后台起一个模块进程。返回 0；进程号写进 $LAST_PID。
#
# stdout/stderr 进 `<journal>.out`，**不进 journal**。理由：工具自己已经
# 用 --journal 往 journal 里写了一行一份；再把 stdout 也接进同一个文件的话
# 每行会出现两次，于是任何按行计数的断言都会翻倍 —— 而且翻倍之后
# 「看起来也像对的」，是个很难发现的假象。
StartMod() {
    _sm_journal="$1"; shift
    _sm_cwd="$1"; shift
    _sm_tag="$1"; shift

    LAST_PID=""
    LAST_JOURNAL="$_sm_journal"

    ( cd "$_sm_cwd" || exit 2
      exec "$IPC_TEST_BIN/ipc_testmod" "$@" --journal "$_sm_journal" ) \
        >"$_sm_journal.out" 2>&1 < /dev/null &
    LAST_PID=$!
    return 0
}

# WaitMod <pid> [deadline-ms]
# 等进程自然退出。超时就杀掉并返回 1 —— 一定要有超时，否则一个卡死的模块
# 会把整条套件挂住，而挂住的套件比失败的套件更难查。
WaitMod() {
    _wm_pid="$1"
    _wm_ms="${2:-$IPC_TEST_TIMEOUT_MS}"
    _wm_spent=0

    while kill -0 "$_wm_pid" 2>/dev/null; do
        if [ "$_wm_spent" -ge "$_wm_ms" ]; then
            printf '  !! 进程 %s 在 %sms 内没有退出，杀掉它\n' "$_wm_pid" "$_wm_ms"
            kill "$_wm_pid" 2>/dev/null || true
            wait "$_wm_pid" 2>/dev/null || true
            return 1
        fi
        sleep 0.02
        _wm_spent=$((_wm_spent + 20))
    done
    wait "$_wm_pid" 2>/dev/null
    return 0
}

# StopMod <pid> —— 无条件收掉（清理路径用，不产生失败）
StopMod() {
    _st_pid="$1"
    [ -z "$_st_pid" ] && return 0
    if kill -0 "$_st_pid" 2>/dev/null; then
        kill "$_st_pid" 2>/dev/null || true
    fi
    wait "$_st_pid" 2>/dev/null || true
    return 0
}

# ---- 实验目录 -------------------------------------------------------

# 新建一个实验目录，路径写进 $LAB。用 mktemp -d 保证不会撞已有的东西。
LabNew() {
    _ln_parent="${1:-/tmp}"
    mkdir -p "$_ln_parent" || return 1
    LAB=$(mktemp -d "$_ln_parent/ipc-lab-XXXXXX") || return 1
    # 护栏必须先过关再往里面写东西
    LabGuard "$LAB" || return 1
    ConfPath="$LAB/modules.conf"
    JournalA="$LAB/a.journal"
    JournalB="$LAB/b.journal"
    JournalC="$LAB/c.journal"
    return 0
}

# 写一份配置：<ns> <moduleId> <uid> <path>。uid 默认当前 uid。
WriteConf() {
    _wc_conf="$1"; _wc_ns="$2"; shift 2
    _wc_uid="${CONF_UID:-$(id -u)}"
    : >"$_wc_conf"
    for _wc_mod in "$@"; do
        printf '%s %s %s %s/%s.sock\n' "$_wc_ns" "$_wc_mod" "$_wc_uid" \
            "$LAB" "$_wc_mod" >>"$_wc_conf"
    done
}

# 收掉实验目录。
#
# 刻意**不用** `rm -rf "$LAB"`，而是先逐个删掉我们自己写进去的那几类文件，
# 最后 `rmdir`。理由：`rm -rf <变量>` 是那种「只要变量在某条路径上被算错
# 就会安静地删掉别的东西」的写法，而这个函数会被十几条测试调用。逐个删 +
# rmdir 的好处是——目录里一旦出现预期之外的东西，rmdir 会失败并吭声，
# 而不是把它一起抹掉。护栏 LabGuard 仍然在前面把一次关。
LabClean() {
    LabGuard "$LAB" || return 1
    for _lc_f in "$LAB"/*; do
        [ -e "$_lc_f" ] || continue
        case "${_lc_f##*/}" in
            *.sock|*.lock|*.conf|*.journal|*.out|*.script)
                rm -f "$_lc_f" ;;
            *)
                printf '  !! LabClean: 出现了预期之外的文件，拒绝继续清理：%s\n' \
                    "$_lc_f"
                return 1 ;;
        esac
    done
    rmdir "$LAB" 2>/dev/null || true
    return 0
}
