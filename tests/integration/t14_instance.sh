#!/usr/bin/env bash
# t14 -- instance generation: a reply produced for a previous life of the
# requester must not complete a request issued by the new one.
#
# handoff.md 11, row "实例代际": "the requester restarts and then receives an old
# reply -- the new instance must reject it".
#
# Why this needs an explicit generation id at all: the module id is stable, the
# socket path is stable, and the request id space restarts from zero in a fresh
# process.  Without a per-process identifier, a reply that arrives after a
# restart is indistinguishable from a legitimate answer to a request the new
# instance has not even issued yet.
#
# The reply carries back the requester's *instance id* (the sink echoes it), so
# the check is possible on the receiving side without any coordination.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./common.sh
require_root
require_lab

journal() { mod_journal "$1"; }

echo "== a slow sink, so a reply can outlive its requester"
mod_start B1 "$USER_B"
mod_ready B1 8 || fail "B1 failed to register"
mod_start A1 "$USER_A"
mod_ready A1 8 || fail "A1 failed to register"

# B1 sleeps ~2.5 s before answering.  A1 gives up after 200 ms, and the reply is
# guaranteed to arrive long after A1 has been replaced.
mod_cmd B1 "behave 1200 block:2500"

instance_first="$(jfield "$(journal A1)" '^BOOT module=' instance)"
echo "   A1 generation 1: $instance_first"

mod_cmd A1 "sendt 200 B1 1200 doomed"
if jwait "$(journal A1)" '^SEND done op=sendt dst=B1 event=1200 rc=-11' 6; then
    ok "the request timed out before the sink answered"
else
    fail "the request did not time out: rc=$(jfield "$(journal A1)" '^SEND done.*event=1200' rc)"
fi

echo
echo "== the requester is replaced while the answer is still in flight"
mod_kill A1 KILL
mod_start A1 "$USER_A"
if mod_ready A1 8; then
    ok "the requester registered again"
else
    fail "the requester failed to re-register: $(cat "$(journal A1)")"
fi
instance_second="$(jfield "$(journal A1)" '^BOOT module=' instance)"
echo "   A1 generation 2: $instance_second"
if [ "$instance_second" != "$instance_first" ]; then
    ok "the new instance has a different generation id"
else
    fail "the generation id was reused"
fi
assert_re "$(mod_out A1)" 'removing stale socket residue at' \
          "the restarted requester reclaimed the address its predecessor left"
# The new instance has issued nothing, so it has nothing pending at all.
assert_eq "$(jcount "$(journal A1)" '^SEND begin')" "0" \
          "the new generation has no request in flight"

echo
echo "== the late reply lands on the new instance and is refused"
# Poll until the sink's 2.5 s block expires and the reply has been processed.
# Waiting is inherent to the scenario: the whole point is that the answer
# arrives after the requester it was meant for is gone.
stale_seen=0
for _ in $(seq 1 14); do
    mod_stats A1
    cur="$(jfield "$(journal A1)" '^STATS' reply_unmatched)"
    if [ "${cur:-0}" -ge 1 ]; then
        stale_seen=1
        break
    fi
    sleep 0.3
done
jwait "$(journal A1)" '^STATS ' 5

unmatched="$(jfield "$(journal A1)" '^STATS' reply_unmatched)"
matched="$(jfield "$(journal A1)" '^STATS' reply_matched)"
echo "   new generation: reply_unmatched=$unmatched reply_matched=$matched"
if [ "$stale_seen" -eq 1 ]; then
    ok "the reply addressed to the previous generation was refused"
else
    fail "the stale reply was never recorded as refused (reply_unmatched=$unmatched)"
fi
assert_eq "${matched:-x}" "0" "no request of the new generation was completed"
assert_no_re "$(journal A1)" '^SEND done .*rc=0' \
             "the new generation completed no request at all"

echo
echo "== the sink also reports the outcome"
# Depending on timing the reply either reaches the new socket (and is refused
# there) or finds no socket at all.  Both are acceptable; what must never happen
# is a silent success.  Record which one it was so the report can say so.
mod_stats B1
jwait "$(journal B1)" '^STATS ' 5
failed="$(jfield "$(journal B1)" '^STATS' failed)"
replies="$(jfield "$(journal B1)" '^STATS' replies)"
echo "   sink: replies_sent=$replies send_failed=$failed"
if [ "${replies:-0}" -ge 1 ]; then
    ok "the sink did send the late reply"
else
    fail "the sink never sent a reply, so the staleness path was not exercised"
fi

echo
echo "== a fresh request in the new generation works normally"
mod_cmd B1 "behave 1201 echo"
mod_cmd A1 "sendt 3000 B1 1201 current"
if jwait "$(journal A1)" '^SEND done op=sendt dst=B1 event=1201 rc=0' 8; then
    ok "the new generation can complete a request of its own"
    assert_re "$(journal A1)" 'reply=current' "the reply belonged to the new request"
else
    fail "the new generation could not complete a request"
fi
mod_stats A1
jwait "$(journal A1)" '^STATS ' 5
assert_eq "$(jfield "$(journal A1)" '^STATS' reply_matched)" "1" \
          "exactly one request was completed, and it was the current one"
# The stale reply must not have been "parked" and delivered to the new request.
assert_eq "$(jfield "$(journal A1)" '^STATS' reply_unmatched)" "$unmatched" \
          "the refusal count did not change once the current request completed"

for k in A1 B1; do
    if mod_alive "$k"; then ok "$k is still running"; else fail "$k died"; fi
done

all_stop
report_and_exit
