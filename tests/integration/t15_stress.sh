#!/usr/bin/env bash
# t15 -- lifecycle stress: the properties that only show up after repetition.
#
# handoff.md 11, row "生命周期压力": high-frequency register/unregister,
# concurrent shutdown, and callback re-entry, with no deadlock, no descriptor
# leak and no unbounded memory growth.
#
# Each part is chosen so a leak or a deadlock has somewhere to accumulate:
#
#   churn      10 register/exit cycles in one directory -- leaked lock files,
#              leaked sockets or a lock that is never released all show up here
#   shutdown   nine modules asked to stop at the same instant
#   re-entry   ipc_send() from inside a handler, in both dispatch modes
#   accounting descriptors and RSS sampled around a 20k-message burst
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./common.sh
require_root
require_lab

journal() { mod_journal "$1"; }
NAMES=(A1 A2 A3 B1 B2 B3 C1 C2 C3)
user_of() {
    case "$1" in A*) echo "$USER_A" ;; B*) echo "$USER_B" ;; *) echo "$USER_C" ;; esac
}

# ------------------------------------------------------------------ #
echo "== ten register/exit cycles leave nothing behind"
cycle_fail=0
for round in $(seq 1 10); do
    # --exit-after-register registers and then _exit()s with no teardown, which
    # is the worst case for residue: every round leaves a socket file behind.
    mod_try "churn$round" A1 "$USER_A" --exit-after-register
    wait_exit "${MOD_PID[churn$round]}" 5
    rc="$(jfield "$(journal "churn$round")" '^BOOT module=' rc)"
    if [ "$rc" != "0" ]; then
        cycle_fail=1
        echo "   round $round: registration rc=$rc"
        break
    fi
    if ! grep -Eq '^EXIT after_register' "$(journal "churn$round")"; then
        cycle_fail=1
        echo "   round $round: did not exit as requested"
        break
    fi
done
if [ "$cycle_fail" -eq 0 ]; then
    ok "all ten register-then-die cycles registered successfully"
else
    fail "a register-then-die cycle failed; the residue was not reclaimed"
fi

sock_files="$(find "$LAB_RUN/A" -maxdepth 1 -name 'A1.sock' | wc -l | tr -d ' ')"
lock_files="$(find "$LAB_RUN/A" -maxdepth 1 -name 'A1.sock.lock' | wc -l | tr -d ' ')"
# Ten rounds x 1 residue each, but they all point at the same single path.
assert_eq "$sock_files" "1" "the ten cycles left exactly one address file, not ten"
assert_eq "$lock_files" "1" "the ten cycles left exactly one lock file"
if find "$LAB_RUN/A" -maxdepth 1 -name 'churn*' | grep -q .; then
    fail "the cycles leaked per-round files into the socket directory"
else
    ok "no per-round files leaked into the socket directory"
fi

# ------------------------------------------------------------------ #
echo
echo "== nine modules asked to stop at the same instant"
for n in "${NAMES[@]}"; do
    mod_start "$n" "$(user_of "$n")"
done
for n in "${NAMES[@]}"; do
    mod_ready "$n" 8 || fail "$n failed to register"
done
for n in "${NAMES[@]}"; do
    mod_stop_async "$n"
done
srv_fail=0
for n in "${NAMES[@]}"; do
    if ! mod_reap "$n" 8; then
        srv_fail=1
        echo "   $n did not shut down in time"
    fi
done
if [ "$srv_fail" -eq 0 ]; then
    ok "all nine modules shut down within the timeout"
else
    fail "a concurrent shutdown did not finish"
fi

leftover=0
for n in "${NAMES[@]}"; do
    case "$n" in A*) d="$LAB_RUN/A" ;; B*) d="$LAB_RUN/B" ;; *) d="$LAB_RUN/C" ;; esac
    if [ -e "$d/$n.sock" ]; then
        leftover=$((leftover + 1))
    fi
done
assert_eq "$leftover" "0" "every socket file was removed on the way out"
if pgrep -f "$LAB_BIN/ipc_testmod" >/dev/null; then
    fail "a module process is still running after the shutdown"
    pkill -f "$LAB_BIN/ipc_testmod" 2>/dev/null
else
    ok "no module process survived the shutdown"
fi

# ------------------------------------------------------------------ #
echo
echo "== callback re-entry is refused in INLINE mode and allowed in POOL mode"
# In INLINE mode the handler runs on the only thread that could deliver the
# reply, so ipc_send() from inside it must be refused rather than deadlock.
mod_start A1 "$USER_A"
mod_ready A1 8 || fail "A1 failed to register"
mod_start A2 "$USER_A"
mod_ready A2 8 || fail "A2 failed to register"
mod_start B1 "$USER_B"
mod_ready B1 8 || fail "B1 failed to register"

mod_cmd A2 "behave 1300 nested:B1:1301"
mod_cmd A1 "sendt 4000 A2 1300 inline-nested"
if jwait "$(journal A1)" '^SEND done op=sendt dst=A2 event=1300 rc=0' 8; then
    ok "the INLINE handler still answered its caller"
else
    fail "the INLINE handler did not answer"
fi
if jwait "$(journal A2)" '^NESTED ' 5; then
    assert_eq "$(jfield "$(journal A2)" '^NESTED' rc)" "-13" \
              "ipc_send from an INLINE handler is refused with IPC_ERR_DEADLOCK"
else
    fail "the nested call produced no result"
fi
if mod_alive A2; then
    ok "the module survived the refused re-entry (no deadlock)"
else
    fail "the module died on re-entry"
fi

# In POOL mode a worker thread can safely wait: the receive thread is free.
mod_start C1 "$USER_C" --dispatch pool --workers 2
mod_ready C1 8 || fail "C1 failed to register"
mod_cmd C1 "behave 1302 nested:B1:1303"
mod_cmd A1 "sendt 6000 C1 1302 pool-nested"
if jwait "$(journal C1)" '^NESTED ' 8; then
    assert_eq "$(jfield "$(journal C1)" '^NESTED' rc)" "0" \
              "ipc_send from a POOL handler succeeds"
    # The nested action sends the literal "nested" (6 bytes) and B1 echoes it.
    assert_re "$(journal C1)" '^NESTED .*reply_len=6' \
              "the nested reply came back synchronously"
else
    fail "the pool-mode nested call produced no result"
fi
assert_re "$(journal B1)" "$(recv_re C1 B1 1303)" "B1 served the nested request"
if jwait "$(journal A1)" '^SEND done op=sendt dst=C1 event=1302 rc=0' 8; then
    ok "the outer request still completed after the nested one"
else
    fail "the outer request did not complete"
fi

# ------------------------------------------------------------------ #
echo
echo "== descriptors and memory do not grow without bound"
mod_start B2 "$USER_B"
mod_ready B2 8 || fail "B2 failed to register"

# Warm up so one-off allocations are not counted as growth.
mod_cmd A1 "soak 2000 B2 1400 warmup"
jwait "$(journal A1)" '^SOAK ' 30 || fail "the warm-up burst did not finish"
# A1 has *sent* the warm-up burst; wait for B2 to have read all of it before
# sampling the baseline.  Poll the counter rather than sleeping a fixed time.
for _ in $(seq 1 40); do
    mod_stats B2
    warm="$(jfield "$(journal B2)" '^STATS' recv_read)"
    [ "${warm:-0}" -ge 2000 ] && break
    sleep 0.1
done
fds_before="$(proc_fds "${MOD_PID[B2]}")"
rss_before="$(proc_rss_kb "${MOD_PID[B2]}")"
echo "   before: fds=$fds_before rss=${rss_before}KiB"

# soak() retries on EAGAIN, so all 20000 messages really arrive rather than
# being counted as attempted.
mod_cmd A1 "soak 20000 B2 1401 load"
if jwait "$(journal A1)" '^SOAK n=20000 sent=20000 ' 60; then
    ok "20000 messages were delivered to the sink (retrying through congestion)"
else
    got="$(jfield "$(journal A1)" '^SOAK ' sent)"
    fail "only $got of 20000 messages were delivered"
fi
# Wait for the sink to drain everything it accepted.
drained=0
for _ in $(seq 1 40); do
    mod_stats B2
    read_n="$(jfield "$(journal B2)" '^STATS' recv_read)"
    invoked="$(jfield "$(journal B2)" '^STATS' cb_invoked)"
    dropped="$(jfield "$(journal B2)" '^STATS' cb_dropped)"
    if [ $(( ${invoked:-0} + ${dropped:-0} )) -ge 20000 ]; then
        drained=1
        break
    fi
    sleep 0.25
done
if [ "$drained" -eq 1 ]; then
    ok "the sink accounted for all 20000 messages"
else
    fail "the sink only accounted for $(( ${invoked:-0} + ${dropped:-0} )) messages"
fi

# The drain loop above proved every message was accounted for; the difference
# now under test is purely "did any descriptor or page stay behind", so wait for
# the /proc sample to stop moving instead of sleeping a fixed time.  This feeds
# an assertion, so it must not be a guess.
prev=""
for _ in $(seq 1 20); do
    cur="$(proc_fds "${MOD_PID[B2]}")/$(proc_rss_kb "${MOD_PID[B2]}")"
    [ "$cur" = "$prev" ] && break
    prev="$cur"
    sleep 0.1
done

fds_after="$(proc_fds "${MOD_PID[B2]}")"
rss_after="$(proc_rss_kb "${MOD_PID[B2]}")"
echo "   after:  fds=$fds_after rss=${rss_after}KiB"

if [ "${fds_after:-999}" -le "${fds_before:-0}" ]; then
    ok "descriptor count is stable ($fds_before -> $fds_after)"
else
    fail "descriptor count grew from $fds_before to $fds_after"
fi
growth=$(( ${rss_after:-0} - ${rss_before:-0} ))
if [ "$growth" -lt 8192 ]; then
    ok "resident memory grew by only ${growth} KiB over 20000 messages"
else
    fail "resident memory grew by ${growth} KiB, which looks unbounded"
fi

# The counters must add up.  Every datagram read off the socket was either
# handed to the handler (delivered) or refused by the bounded queue (dropped);
# the only slack allowed is a handler that is mid-flight when stats are read.
delivered="$(jfield "$(journal B2)" '^STATS' delivered)"
rejected="$(jfield "$(journal B2)" '^STATS' rejected)"
echo "   sink: read=$read_n delivered=$delivered invoked=$invoked dropped=$dropped rejected=$rejected"
if [ $(( ${delivered:-0} + ${dropped:-0} )) -le "${read_n:-0}" ]; then
    ok "delivered + dropped never exceeds what was read"
else
    fail "delivered + dropped exceeds the number of datagrams read"
fi
slack=$(( ${read_n:-0} - ${delivered:-0} - ${dropped:-0} ))
if [ "$slack" -le 2 ]; then
    ok "nothing was read without being accounted for (slack $slack)"
else
    fail "$slack datagrams were read but neither delivered nor counted as dropped"
fi
assert_eq "${rejected:-x}" "0" "the sink rejected nothing during the burst"

# ------------------------------------------------------------------ #
echo
echo "== everything is still responsive"
resp_fail=0
for n in A1 A2 B1 B2 C1; do
    mod_stats "$n" || resp_fail=1
    if ! jwait "$(journal "$n")" '^STATS ' 5; then
        resp_fail=1
        echo "   $n did not answer a stats request"
    fi
done
if [ "$resp_fail" -eq 0 ]; then
    ok "all five modules answered a command after the stress"
else
    fail "a module stopped responding"
fi

mod_cmd A1 "post B2 1402 final"
if jwait "$(journal B2)" "$(recv_re A1 B2 1402)" 5; then
    ok "cross-module traffic still works after the stress"
else
    fail "cross-module traffic broke"
fi

all_stop
report_and_exit
