#!/usr/bin/env bash
# t09 -- synchronous send that never gets an answer.
#
# handoff.md 11, row "同步等待": the legacy contract is to wait *indefinitely*,
# and the test has to be ended by an external watchdog rather than by asserting
# that the call eventually returns.
#
# This file contains the one place in the whole suite where a fixed sleep is
# legitimate, and it is marked: the subject of the test is "this call is still
# blocked after a long time", so time has to pass on purpose.
#
# Also verified here:
#   - the bounded variant (ipc_send_timeout) returns IPC_ERR_TIMEOUT, on time
#   - a blocked sender does not block its own receive loop
#   - the sink's silence is deliberate (drop), not a failure to deliver
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./common.sh
require_root
require_lab

journal() { mod_journal "$1"; }

echo "== a peer that receives but never answers"
mod_start A1 "$USER_A"
mod_ready A1 8 || fail "A1 failed to register"
mod_start B1 "$USER_B"
mod_ready B1 8 || fail "B1 failed to register"
mod_start B2 "$USER_B"
mod_ready B2 8 || fail "B2 failed to register"

mod_cmd B1 "behave 700 drop"

echo
echo "== the unbounded legacy call stays blocked (watchdog-bounded)"
mod_cmd A1 "send B1 700 forever"
if jwait "$(journal A1)" '^SEND begin op=send dst=B1 event=700' 5; then
    ok "the unbounded request was issued and is waiting"
else
    fail "the unbounded request was never issued"
fi

# The sink did receive it: the silence is a decision, not a delivery failure.
if jwait "$(journal B1)" "$(recv_re A1 B1 700)" 5; then
    ok "the sink received the request and chose not to answer"
else
    fail "the sink never received the request"
fi
assert_re "$(journal B1)" '^ACTION event=700 drop' "the sink's drop behaviour was applied"

# ---- the deliberate wait ------------------------------------------- #
# Subject of the test: "it is still waiting".  Two and a half seconds is long
# enough that any accidental timeout in the library would have fired.
WATCHDOG_S=2.5
sleep "$WATCHDOG_S"
if grep -Eq '^SEND done op=send dst=B1 event=700' "$(journal A1)"; then
    fail "the unbounded call returned after ${WATCHDOG_S}s; it must not time out"
else
    ok "still blocked after ${WATCHDOG_S}s: the unbounded contract holds"
fi
if mod_alive A1; then
    ok "the waiting process is alive and not spinning to death"
else
    fail "the waiting process died"
fi

echo
echo "== a blocked sender still serves its own receive loop"
# The FIFO command thread is the one parked in ipc_send(); the receive loop is a
# separate thread and must keep delivering.
mod_cmd B2 "post A1 701 while-waiting"
if jwait "$(journal A1)" "$(recv_re B2 A1 701)" 5; then
    ok "A1 delivered an inbound message while its caller was blocked"
else
    fail "A1's receive loop was blocked by the waiting caller"
fi

echo
echo "== the watchdog ends the test"
# There is no way to release the waiter from outside: its pending slot is private
# to the process.  Killing it is exactly what handoff.md asks for here.
mod_kill A1 KILL
if grep -Eq '^SEND done op=send dst=B1 event=700' "$(journal A1)"; then
    fail "the call completed during teardown, which changes the conclusion"
else
    ok "the call never completed: the test was ended by the watchdog"
fi

echo
echo "== the bounded variant times out, and does so on time"
mod_start A1 "$USER_A"
mod_ready A1 8 || fail "A1 failed to register again after the kill"
mod_cmd B1 "behave 702 drop"

t0="$(date +%s%N)"
mod_cmd A1 "sendt 400 B1 702 bounded"
if jwait "$(journal A1)" '^SEND done op=sendt dst=B1 event=702 rc=-11' 10; then
    t1="$(date +%s%N)"
    elapsed_ms=$(( (t1 - t0) / 1000000 ))
    ok "the bounded request returned IPC_ERR_TIMEOUT (-11)"
    if [ "$elapsed_ms" -ge 350 ] && [ "$elapsed_ms" -le 3000 ]; then
        ok "it timed out near the requested 400 ms (${elapsed_ms} ms)"
    else
        fail "it timed out after ${elapsed_ms} ms, which is not near 400 ms"
    fi
else
    fail "the bounded request never returned: rc=$(jfield "$(journal A1)" '^SEND done.*event=702' rc)"
fi
assert_re "$(journal B1)" "$(recv_re A1 B1 702)" "the bounded request did reach the sink"

echo
echo "== the timeout released the caller"
# If the pending slot had leaked, the next request would still work (the table is
# 64 deep), so the observable proof is that the caller is responsive again.
mod_stats A1
if jwait "$(journal A1)" '^STATS ' 5; then
    ok "the command loop is responsive again after the timeout"
else
    fail "the command loop stayed blocked after the timeout"
fi
assert_eq "$(jfield "$(journal A1)" '^STATS' pending_rejected)" "0" \
          "no request was rejected for a full pending table"

echo
echo "== a zero timeout is honoured immediately"
mod_cmd A1 "sendt 0 B1 703 zero"
if jwait "$(journal A1)" '^SEND done op=sendt dst=B1 event=703 rc=-11' 5; then
    ok "a zero timeout returns immediately with IPC_ERR_TIMEOUT"
else
    fail "a zero timeout returned rc=$(jfield "$(journal A1)" '^SEND done.*event=703' rc)"
fi

all_stop
report_and_exit
