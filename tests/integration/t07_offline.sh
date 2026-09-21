#!/usr/bin/env bash
# t07 -- offline peers: three ways to be absent, and the contract for each.
#
# handoff.md 11, row "离线": a module that was never started, one that exited
# cleanly, and one that died abnormally must all behave the same way from the
# sender's point of view -- the datagram fails immediately, broadcasts skip the
# absent peer, and *nothing* is queued for a later recovery.
#
# That last part is the whole reason the design uses datagrams: there is no
# store-and-forward layer, so "recovery" cannot happen by accident.  The test
# asserts that a module which starts later never receives traffic that was
# addressed to it while it was down.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./common.sh
require_root
require_lab

journal() { mod_journal "$1"; }

# Up: A1 A2 B1 B2 C1.  Never started: A3 B3 C2 C3.
echo "== five of nine modules are up"
for n in A1 A2 B1 B2 C1; do
    case "$n" in A*) u="$USER_A" ;; B*) u="$USER_B" ;; *) u="$USER_C" ;; esac
    mod_start "$n" "$u"
    mod_ready "$n" 8 || fail "$n failed to register"
done

echo
echo "== unicast to a module that was never started"
mod_cmd A1 "post B3 500 hello-void"
if jwait "$(journal A1)" '^POST dst=B3 event=500 rc=-6' 5; then
    ok "post to a never-started module returns IPC_ERR_OFFLINE"
else
    fail "post to a never-started module returned rc=$(jfield "$(journal A1)" '^POST dst=B3 event=500' rc)"
fi
if [ -e "$LAB_RUN/B/B3.sock" ]; then
    fail "a socket exists for a module that never registered"
else
    ok "no socket exists for the module that never registered"
fi

echo
echo "== broadcast skips the absent peers and still reaches the live ones"
mod_cmd A1 "bcast 501 fanout"
if jwait "$(journal A1)" '^BCAST event=501 rc=4' 5; then
    ok "broadcast reached the four live peers"
else
    fail "broadcast rc=$(jfield "$(journal A1)" '^BCAST event=501' rc), want 4"
fi
mod_stats A1
jwait "$(journal A1)" '^STATS ' 5
assert_eq "$(jfield "$(journal A1)" '^STATS' bcast_targets)" "8" \
          "the target set is the table, not the live set"
assert_eq "$(jfield "$(journal A1)" '^STATS' bcast_skipped)" "4" \
          "the four absent peers were counted as skipped"
for n in A2 B1 B2 C1; do
    if jwait "$(journal "$n")" "$(recv_re A1 "$n" 501)" 5; then
        ok "$n received the broadcast despite four absent peers"
    else
        fail "$n missed the broadcast"
    fi
done

echo
echo "== unicast to a module that exited cleanly"
# A graceful stop unlinks the address, so the path is simply gone.
mod_stop B2
if [ -e "$LAB_RUN/B/B2.sock" ]; then
    fail "a clean exit left its socket behind"
else
    ok "a clean exit removed its address"
fi
mod_cmd A1 "post B2 502 gone"
if jwait "$(journal A1)" '^POST dst=B2 event=502 rc=-6' 5; then
    ok "post to a cleanly exited module returns IPC_ERR_OFFLINE"
else
    fail "post to a cleanly exited module returned rc=$(jfield "$(journal A1)" '^POST dst=B2 event=502' rc)"
fi

echo
echo "== unicast to a module that died abnormally"
# _exit() skips teardown, so the address file survives with nobody bound to it.
# The kernel then answers ECONNREFUSED, which the library reports as OFFLINE and
# not as a hard failure.
mod_cmd B1 "exit"
wait_exit "${MOD_PID[B1]}" 5 && ok "B1 exited abruptly" || fail "B1 ignored the exit request"
if [ -S "$LAB_RUN/B/B1.sock" ]; then
    ok "the abnormal exit left a socket residue (expected)"
else
    fail "the residue disappeared, so this is not the crash case"
fi
mod_cmd A1 "post B1 503 dead"
if jwait "$(journal A1)" '^POST dst=B1 event=503 rc=-6' 5; then
    ok "post to a dead-but-present address returns IPC_ERR_OFFLINE"
else
    fail "post to a dead address returned rc=$(jfield "$(journal A1)" '^POST dst=B1 event=503' rc)"
fi

echo
echo "== the failed sends are accounted for, not lost silently"
mod_stats A1
jwait "$(journal A1)" '^STATS ' 5
failed="$(jfield "$(journal A1)" '^STATS' failed)"
if [ "${failed:-0}" -ge 3 ]; then
    ok "$failed send attempts failed and were counted"
else
    fail "only $failed failures were counted, expected at least 3"
fi

echo
echo "== nothing is queued for an offline peer"
# Start the two modules that were absent.  If the implementation had any
# store-and-forward behaviour, the earlier traffic would show up now.
mod_start B3 "$USER_B"
mod_ready B3 8 || fail "B3 failed to register"
mod_start A3 "$USER_A"
mod_ready A3 8 || fail "A3 failed to register"

# Give any hypothetical recovery thread room to run before asserting absence.
if jwait_gone "$(journal B3)" '^RECV ' 2; then
    ok "B3 received nothing that was addressed to it while it was down"
else
    fail "B3 received traffic that was sent while it was absent"
fi
if jwait_gone "$(journal A3)" "$(recv_re A1 A3 501)" 1; then
    ok "A3 received nothing from the earlier broadcast"
else
    fail "A3 received a broadcast that predates its registration"
fi

# And a fresh send to the newly started module works right away.
mod_cmd A1 "post B3 504 fresh"
if jwait "$(journal A1)" '^POST dst=B3 event=504 rc=0' 5; then
    ok "a new post to the restarted module succeeds"
else
    fail "a new post to the restarted module failed: $(jfield "$(journal A1)" '^POST dst=B3 event=504' rc)"
fi
if jwait "$(journal B3)" "$(recv_re A1 B3 504)" 5; then
    ok "the restarted module received the new message"
else
    fail "the restarted module did not receive the new message"
fi
assert_eq "$(jcount "$(journal B3)" '^RECV ')" "1" \
          "B3 handled exactly one message: its own new one"

echo
echo "== no sender-side retry machinery exists"
assert_no_re "$(journal A1)" '(RETRY|REQUEUE|QUEUED|DEFERRED)' \
             "A1's journal mentions no retry or deferral"
assert_no_re "$(mod_out A1)" '(retry|requeue|deferred)' \
             "A1's log mentions no retry or deferral"

all_stop
report_and_exit
