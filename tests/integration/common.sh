#!/usr/bin/env bash
# common.sh -- shared helpers for the integration suite.
#
# The suite needs real UIDs, real directory ownership and real client
# credentials, so it runs as root and drops privileges per module with
# setpriv.  Everything lives under $IPC_LAB (= $HOME/ipc-lab by default) on a
# Linux filesystem: /mnt/c has no Unix ownership or locking semantics, so a
# run there would prove nothing.
#
# Synchronisation rule for the whole suite: a test waits for a *journal line*
# (with a deadline) or for a process to exit.  There is exactly one place where
# a fixed sleep is legitimate -- the test whose subject is "this call blocks
# forever" -- and it is marked as such.

set -uo pipefail

# ------------------------------------------------------------------ #
# locations                                                           #
# ------------------------------------------------------------------ #

COMMON_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$COMMON_DIR/../.." && pwd)"
# The lab deliberately lives outside /home and /run: it is a self-contained,
# clearly-named tree that cannot collide with a production socket root, and
# /opt is traversable by every test identity (unlike a private $HOME).
export IPC_LAB="${IPC_LAB:-/opt/ipc-lab}"

export LAB="$IPC_LAB"
export LAB_CONF="$LAB/conf/ipc-modules.conf"
export LAB_RUN="$LAB/run"
export LAB_LOGS="$LAB/logs"
export LAB_FIFOS="$LAB/fifo"
export LAB_BIN="$LAB/bin"
export LAB_TMP="$LAB/tmp"

# ------------------------------------------------------------------ #
# fixed identities (created by setup_lab.sh)                          #
# ------------------------------------------------------------------ #

export GID_MEMBERS=1500
export UID_A=1501
export UID_B=1502
export UID_C=1503
export UID_X=1504 # deliberately NOT a member of ipcmembers

USER_A=ipca
USER_B=ipcb
USER_C=ipcc
USER_X=ipcx

# ------------------------------------------------------------------ #
# result accounting                                                   #
# ------------------------------------------------------------------ #

TEST_NAME="${TEST_NAME:-$(basename "$0" .sh)}"
FAILURES=0
CHECKS=0
export RESULTS_FILE="$LAB/results.txt"

_record() { # kind message
    printf '%-6s [%s] %s\n' "$1" "$TEST_NAME" "$2"
}

ok() { CHECKS=$((CHECKS + 1)); _record "ok" "$1"; }

fail() { CHECKS=$((CHECKS + 1)); FAILURES=$((FAILURES + 1)); _record "FAIL" "$1"; }

blocked() { _record "BLOCKED" "$1"; }

# assert_eq <actual> <expected> <message>
assert_eq() {
    if [ "$1" = "$2" ]; then ok "$3 (=$1)"; else fail "$3: got '$1', want '$2'"; fi
}

# assert_re <file> <regex> <message>
assert_re() {
    if grep -Eq -- "$2" "$1" 2>/dev/null; then
        ok "$3"
    else
        fail "$3: /$2/ not found in $(basename "$1")"
    fi
}

# assert_no_re <file> <regex> <message>
assert_no_re() {
    if grep -Eq -- "$2" "$1" 2>/dev/null; then
        fail "$3: unexpected /$2/ in $(basename "$1")"
    else
        ok "$3"
    fi
}

report_and_exit() {
    mkdir -p "$(dirname "$RESULTS_FILE")"
    if [ "$FAILURES" -eq 0 ]; then
        printf 'TEST %s pass %s\n' "$TEST_NAME" "$CHECKS" >>"$RESULTS_FILE"
        printf '%s: PASS (%s checks)\n' "$TEST_NAME" "$CHECKS"
        exit 0
    fi
    printf 'TEST %s fail %s\n' "$TEST_NAME" "$CHECKS" >>"$RESULTS_FILE"
    printf '%s: FAIL (%s of %s checks failed)\n' "$TEST_NAME" "$FAILURES" "$CHECKS"
    exit 1
}

# ------------------------------------------------------------------ #
# waiting                                                             #
# ------------------------------------------------------------------ #

# jwait <file> <regex> [timeout_s] -- wait for a journal line to appear.
jwait() {
    local file="$1" re="$2" timeout="${3:-5}" waited=0
    while [ "$waited" -lt $((timeout * 50)) ]; do
        if [ -f "$file" ] && grep -Eq -- "$re" "$file" 2>/dev/null; then
            return 0
        fi
        sleep 0.02
        waited=$((waited + 1))
    done
    return 1
}

# jwait_gone <file> <regex> [timeout_s] -- wait for a line NOT to appear any
# more.  Used by the "must not happen" tests: we assert that after a bounded
# observation window the forbidden line is still absent.
jwait_gone() {
    local file="$1" re="$2" timeout="${3:-1}"
    sleep "$timeout"
    if [ -f "$file" ] && grep -Eq -- "$re" "$file" 2>/dev/null; then
        return 1
    fi
    return 0
}

# wait_exit <pid> [timeout_s]
wait_exit() {
    local pid="$1" timeout="${2:-5}" waited=0
    while [ "$waited" -lt $((timeout * 50)) ]; do
        if ! kill -0 "$pid" 2>/dev/null; then return 0; fi
        sleep 0.02
        waited=$((waited + 1))
    done
    return 1
}

# jfield <file> <regex> <key> [which] -- value of key=... on the matching line
jfield() {
    local file="$1" re="$2" key="$3" which="${4:-last}"
    local line
    if [ "$which" = "last" ]; then
        line="$(grep -E -- "$re" "$file" 2>/dev/null | tail -1)"
    else
        line="$(grep -E -- "$re" "$file" 2>/dev/null | head -1)"
    fi
    printf '%s\n' "$line" | tr ' ' '\n' | grep -E "^$key=" | tail -1 | cut -d= -f2-
}

# jcount <file> <regex> -- number of matching lines (0 when the file or the
# match is absent, so it is safe under `set -u`).
jcount() {
    local n
    n="$(grep -Ec -- "$2" "$1" 2>/dev/null)"
    printf '%s\n' "${n:-0}"
}

# recv_re <src> <dst> [event] -- ERE for a RECV journal line.
#
# The journal writes
#     RECV src=A1 dst=B1 ns=core event=14 type=1 len=8 instance=0x... data=...
# so the namespace sits between dst= and event=.  Never pin those two fields
# with a space; always let a wildcard absorb ns=, otherwise every receive
# assertion silently fails to match.
recv_re() {
    if [ "$#" -ge 3 ]; then
        printf '^RECV src=%s dst=%s .*event=%s' "$1" "$2" "$3"
    else
        printf '^RECV src=%s dst=%s ' "$1" "$2"
    fi
}

# ------------------------------------------------------------------ #
# module lifecycle                                                    #
# ------------------------------------------------------------------ #

declare -A MOD_PID MOD_JOURNAL MOD_FIFO MOD_OUT

# annotate <text> -- record a conclusion that is not a pass/fail check.  Used
# for statements such as "this case is only expressible as a unit test", so the
# final report can distinguish coverage from omission.
annotate() { _record "note" "$1"; }

uid_of_user() {
    case "$1" in
        "$USER_A") echo "$UID_A" ;;
        "$USER_B") echo "$UID_B" ;;
        "$USER_C") echo "$UID_C" ;;
        "$USER_X") echo "$UID_X" ;;
        *) echo "" ;;
    esac
}

# _mod_launch <key> <module> <user> [extra args...]
#
# `key` names the journal/fifo/output files; `module` is the id handed to the
# library.  They differ only in the tests that try to register under a module
# id that does not belong to the caller.
_mod_launch() {
    local key="$1" module="$2" user="$3"
    shift 3

    local uid has_ns=0 has_conf=0 a
    uid="$(uid_of_user "$user")"
    if [ -z "$uid" ]; then
        echo "_mod_launch: unknown user $user" >&2
        return 2
    fi

    # The callers below rely on the default namespace, but the ambiguity test
    # has to be able to *omit* --ns, so only add it when the caller did not.
    for a in "$@"; do
        [ "$a" = "--ns" ] && has_ns=1
        [ "$a" = "--conf" ] && has_conf=1
    done

    MOD_JOURNAL["$key"]="$LAB_LOGS/$key.journal"
    MOD_FIFO["$key"]="$LAB_FIFOS/$key.fifo"
    MOD_OUT["$key"]="$LAB_LOGS/$key.out"
    rm -f "${MOD_JOURNAL[$key]}" "${MOD_FIFO[$key]}" "${MOD_OUT[$key]}"
    # The fork-inheritance test writes a second report from the forked child.
    rm -f "${MOD_JOURNAL[$key]}.child"
    mkfifo "${MOD_FIFO[$key]}"
    chmod 666 "${MOD_FIFO[$key]}"
    : >"${MOD_OUT[$key]}"
    chmod 666 "${MOD_OUT[$key]}"

    # Deliberately no "rm -f $module.sock" here.  Cleaning up a stale address is
    # the library's job (handle_residue, under the lifetime lock), and a harness
    # that pre-deletes it would silently hide both that logic and the cases
    # where it must refuse -- see t04 (foreign non-socket) and t12 (crash
    # residue).  setup_lab.sh already rebuilds $LAB/run from scratch.

    local argv=(--module "$module")
    [ "$has_conf" -eq 0 ] && argv+=(--conf "$LAB_CONF")
    [ "$has_ns" -eq 0 ] && argv+=(--ns core)
    argv+=(--group ipcmembers)

    setpriv --reuid="$uid" --regid="$uid" --groups="$GID_MEMBERS,$uid" \
            --no-new-privs -- \
            env IPC_JOURNAL="${MOD_JOURNAL[$key]}" \
                IPC_FIFO="${MOD_FIFO[$key]}" \
                IPC_CONF="$LAB_CONF" \
                IPC_LOG_LEVEL="${IPC_LOG_LEVEL:-warn}" \
            "$LAB_BIN/ipc_testmod" "${argv[@]}" "$@" \
            >>"${MOD_OUT[$key]}" 2>&1 &
    MOD_PID["$key"]=$!
}

# mod_start <name> <user> [extra args...] -- register as `name`.
mod_start() {
    _mod_launch "$1" "$1" "$2" "${@:3}"
}

# mod_try <key> <module> <user> [extra args...] -- try to register as `module`
# while the journal is keyed by `key`, so several attempts can coexist.
mod_try() {
    _mod_launch "$1" "$2" "$3" "${@:4}"
}

# mod_ready <key> [timeout_s] -- wait until registration succeeded.
mod_ready() {
    local name="$1" timeout="${2:-5}"
    if ! jwait "${MOD_JOURNAL[$name]}" '^SOCK path=' "$timeout"; then
        return 1
    fi
    return 0
}

# mod_journal <key> -- path of the journal (also used for ad-hoc keys).
mod_journal() { echo "${MOD_JOURNAL[$1]:-$LAB_LOGS/$1.journal}"; }

# mod_out <key> -- path of the captured stdout+stderr.
mod_out() { echo "${MOD_OUT[$1]:-$LAB_LOGS/$1.out}"; }

# mod_expect_fail <name> [timeout_s] -- wait for a failed boot.
mod_expect_fail() {
    local name="$1" timeout="${2:-5}"
    jwait "${MOD_JOURNAL[$name]}" '^BOOT_FAIL' "$timeout"
}

# mod_cmd <name> <command...>
mod_cmd() {
    local name="$1"
    shift
    timeout 10 sh -c "printf '%s\n' \"\$1\" > \"\$2\"" sh "$*" "${MOD_FIFO[$name]}" \
        || fail "could not deliver command to $name (fifo gone?)"
}

# mod_stats <name> -- ask for a stats dump and block until a *new* STATS line
# has appeared.
#
# The wait matters: a journal accumulates one STATS line per request, so a
# caller that just greps for '^STATS' can silently read the previous dump and
# assert against stale counters.  Folding the freshness wait in here means no
# call site has to know about it.
mod_stats() {
    local name="$1" before waited=0
    before="$(jcount "${MOD_JOURNAL[$name]:-$LAB_LOGS/$name.journal}" '^STATS ')"
    mod_cmd "$name" "stats"
    while [ "$waited" -lt 250 ]; do
        if [ "$(jcount "${MOD_JOURNAL[$name]:-$LAB_LOGS/$name.journal}" '^STATS ')" -gt "$before" ]; then
            return 0
        fi
        sleep 0.02
        waited=$((waited + 1))
    done
    return 1
}

# mod_stop_async <name> -- ask a module to stop without waiting for it.  The
# concurrent-shutdown test needs every module to receive the request before any
# of them has finished tearing down.
mod_stop_async() {
    local name="$1"
    [ -n "${MOD_PID[$name]:-}" ] || return 0
    timeout 10 sh -c "printf '%s\n' stop > \"\$1\"" sh "${MOD_FIFO[$name]}" || true
}

# mod_reap <name> [timeout_s] -- wait for a module that was already asked to
# stop, force-killing it if it overruns.
mod_reap() {
    local name="$1" timeout="${2:-5}"
    [ -n "${MOD_PID[$name]:-}" ] || return 0
    if ! wait_exit "${MOD_PID[$name]}" "$timeout"; then
        kill -KILL "${MOD_PID[$name]}" 2>/dev/null
        wait "${MOD_PID[$name]}" 2>/dev/null
        unset "MOD_PID[$name]"
        return 1
    fi
    wait "${MOD_PID[$name]}" 2>/dev/null
    unset "MOD_PID[$name]"
    return 0
}

# mod_stop <name> [timeout_s]
mod_stop() {
    local name="$1" timeout="${2:-5}"
    [ -n "${MOD_PID[$name]:-}" ] || return 0
    mod_stop_async "$name"
    mod_reap "$name" "$timeout"
}

# mod_kill <name> <signal>
mod_kill() {
    local name="$1" sig="${2:-KILL}"
    [ -n "${MOD_PID[$name]:-}" ] || return 0
    kill "-$sig" "${MOD_PID[$name]}" 2>/dev/null
    wait "${MOD_PID[$name]}" 2>/dev/null
    unset "MOD_PID[$name]"
}

# mod_alive <name> -- 0 when the tracked pid is still running.
mod_alive() {
    local name="$1"
    [ -n "${MOD_PID[$name]:-}" ] || return 1
    kill -0 "${MOD_PID[$name]}" 2>/dev/null
}

# all_stop -- best-effort cleanup, used by the trap
all_stop() {
    local name
    for name in "${!MOD_PID[@]}"; do
        kill -KILL "${MOD_PID[$name]}" 2>/dev/null
        wait "${MOD_PID[$name]}" 2>/dev/null
    done
    MOD_PID=()
}

# ------------------------------------------------------------------ #
# /proc sampling -- FD and memory accounting                          #
# ------------------------------------------------------------------ #

# proc_fds <pid> -- number of open file descriptors.
proc_fds() { ls "/proc/$1/fd" 2>/dev/null | wc -l | tr -d ' '; }

# proc_rss_kb <pid> -- VmRSS in KiB, empty if the process is gone.
proc_rss_kb() {
    awk '/^VmRSS:/ { print $2 }' "/proc/$1/status" 2>/dev/null
}

# cap_of_keys <key...> -- highest cap file descriptor of the tracked modules.
max_fd_cap() {
    local name pid cap best=0
    for name in "$@"; do
        pid="${MOD_PID[$name]:-}"
        [ -n "$pid" ] || continue
        cap="$(awk -F: '/^Max open files/ { print $2 }' "/proc/$pid/limits" 2>/dev/null | tr -d ' ')"
        [ -n "$cap" ] && [ "$cap" -gt "$best" ] 2>/dev/null && best="$cap"
    done
    echo "$best"
}

# ------------------------------------------------------------------ #
# running helpers as a specific identity                              #
# ------------------------------------------------------------------ #

# _as <uid> <groups> <cmd...> -- run a command under another identity.
_as() {
    local uid="$1" groups="$2"
    shift 2
    setpriv --reuid="$uid" --regid="$uid" --groups="$groups" --no-new-privs -- "$@"
}

# as_uid <uid> <cmd...> -- that UID plus the shared ipcmembers group.
as_uid() { _as "$1" "$GID_MEMBERS,$1" "${@:2}"; }

# as_outsider <cmd...> -- uid 1504, deliberately not in ipcmembers.
as_outsider() { _as "$UID_X" "$UID_X" "$@"; }

# forge <uid> <args...> -- run forge_peer with a specific identity and print
# its FORGE line(s).
forge() {
    local uid="$1"
    shift
    as_uid "$uid" "$LAB_BIN/forge_peer" --conf "$LAB_CONF" "$@"
}

# forge_outsider <args...> -- raw sender as the non-member UID.
forge_outsider() {
    as_outsider "$LAB_BIN/forge_peer" --conf "$LAB_CONF" "$@"
}

# sock_bind_probe <uid> <groups> <path> -- try to bind a datagram socket at
# `path` as that identity and print "ok" or "errno=N".  Used to show that the
# filesystem, not just the library, refuses an address takeover.
sock_bind_probe() {
    _as "$1" "$2" python3 - "$3" <<'PY'
import socket
import sys

s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
try:
    s.bind(sys.argv[1])
    print("ok")
except OSError as exc:
    print("errno=%d" % exc.errno)
PY
}

# unlink_probe <uid> <groups> <path> -- try to remove a file as that identity.
unlink_probe() {
    _as "$1" "$2" python3 - "$3" <<'PY'
import os
import sys

try:
    os.unlink(sys.argv[1])
    print("ok")
except OSError as exc:
    print("errno=%d" % exc.errno)
PY
}

# append_probe <uid> <groups> <path> <text> -- try to append to a file.
append_probe() {
    _as "$1" "$2" python3 - "$3" "$4" <<'PY'
import sys

try:
    with open(sys.argv[1], "a") as handle:
        handle.write(sys.argv[2] + "\n")
    print("ok")
except OSError as exc:
    print("errno=%d" % exc.errno)
PY
}

# ------------------------------------------------------------------ #
# guards                                                              #
# ------------------------------------------------------------------ #

require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        blocked "test needs root to switch UIDs; run via scripts/wsl-run.sh"
        printf 'TEST %s blocked 0\n' "$TEST_NAME" >>"$RESULTS_FILE"
        exit 0
    fi
}

require_lab() {
    if [ ! -x "$LAB_BIN/ipc_testmod" ]; then
        echo "lab not prepared; run tests/integration/setup_lab.sh first" >&2
        exit 2
    fi
}

trap all_stop EXIT
