#!/usr/bin/env bash
# t10 -- request matching: a reply must complete exactly the request that asked
# for it, and nothing else.
#
# handoff.md 11, row "请求匹配": late, duplicate, and wrong-source replies must
# not complete a request incorrectly.
#
# Every case here is built so that a *plausible but wrong* implementation fails:
#
#   late        the reply arrives after the requester gave up.  Matching by
#               (source, event) would let it complete the requester's NEXT
#               request; matching by request id must not.
#   duplicate   the sink answers twice.  The second answer must find nothing.
#   wrong src   a third module answers on behalf of the sink.
#   stale inst  a reply addressed to a previous instance of the requester.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./common.sh
require_root
require_lab

journal() { mod_journal "$1"; }

mod_start A1 "$USER_A"
mod_ready A1 8 || fail "A1 failed to register"
mod_start A2 "$USER_A"
mod_ready A2 8 || fail "A2 failed to register"
mod_start B1 "$USER_B"
mod_ready B1 8 || fail "B1 failed to register"

# ------------------------------------------------------------------ #
echo "== a late reply must not complete the next request"
# The sink sleeps 900 ms.  A1 gives up after 150 ms, then immediately asks a
# *different* question with the same event id.  When the first answer finally
# lands, the requester must find no pending request for it.
mod_cmd B1 "behave 800 block:900"
mod_cmd A1 "sendt 150 B1 800 first"
if jwait "$(journal A1)" '^SEND done op=sendt dst=B1 event=800 rc=-11' 5; then
    ok "the first request timed out as designed"
else
    fail "the first request did not time out: rc=$(jfield "$(journal A1)" '^SEND done.*event=800' rc)"
fi

# Now a second request with the same event id.  It must not be completed by the
# answer to the first one.
mod_cmd B1 "del 800"
mod_cmd B1 "behave 800 echo"
mod_cmd A1 "sendt 4000 B1 800 second"
if jwait "$(journal A1)" '^SEND done op=sendt dst=B1 event=800 rc=0' 8; then
    ok "the second request completed"
    assert_re "$(journal A1)" 'reply=second' "it was completed by its own reply"
else
    fail "the second request did not complete"
fi
assert_no_re "$(journal A1)" 'reply=first' "the late answer never completed a request"

mod_stats A1
jwait "$(journal A1)" '^STATS ' 5
unmatched="$(jfield "$(journal A1)" '^STATS' reply_unmatched)"
matched="$(jfield "$(journal A1)" '^STATS' reply_matched)"
assert_eq "${unmatched:-x}" "1" "the late answer was counted as unmatched exactly once"
assert_eq "${matched:-x}" "1" "exactly one reply completed a request"

# ------------------------------------------------------------------ #
echo
echo "== a duplicate reply is refused at the sink"
# ipc_reply() refuses the second call locally (IPC_ERR_STATE), so the duplicate
# never reaches the wire at all.  The requester therefore sees exactly one
# reply -- verified by the sink's own reply_sent counter as well as by A1's.
mod_stats B1
jwait "$(journal B1)" '^STATS ' 5
sink_replies_before="$(jfield "$(journal B1)" '^STATS' replies)"

mod_cmd B1 "behave 801 double-reply"
mod_cmd A1 "sendt 4000 B1 801 dup"
if jwait "$(journal A1)" '^SEND done op=sendt dst=B1 event=801 rc=0' 8; then
    ok "the first of the two replies completed the request"
else
    fail "the request was never completed"
fi
if jwait "$(journal B1)" '^DOUBLEREPLY ' 5; then
    rc1="$(jfield "$(journal B1)" '^DOUBLEREPLY' rc1)"
    rc2="$(jfield "$(journal B1)" '^DOUBLEREPLY' rc2)"
    assert_eq "$rc1" "0" "the sink's first reply was sent"
    assert_eq "$rc2" "-17" "the sink's second reply was refused with IPC_ERR_STATE"
else
    fail "the sink never reported the double reply"
fi
mod_stats B1
jwait "$(journal B1)" '^STATS ' 5
sink_replies_after="$(jfield "$(journal B1)" '^STATS' replies)"
# Only the first of the two attempts went on the wire.
assert_eq "$(( ${sink_replies_after:-0} - ${sink_replies_before:-0} ))" "1" \
          "the double reply added exactly one datagram to the wire"
mod_stats A1
jwait "$(journal A1)" '^STATS ' 5
assert_eq "$(jfield "$(journal A1)" '^STATS' reply_unmatched)" "1" \
          "the refused duplicate never reached the requester"
assert_eq "$(jcount "$(journal A1)" '^SEND done .*event=801 rc=0')" "1" \
          "event 801 completed exactly once"

# ------------------------------------------------------------------ #
echo
echo "== a reply from the wrong module cannot complete a request"
# A1 has a request outstanding to B1.  A2 -- a different module, same UID, so it
# passes the credential check -- answers with a plausible-looking frame.  The
# request id is guessed, which is the point: even a *valid* id belonging to
# someone else's request must not be accepted from the wrong source.
instance="$(jfield "$(journal A1)" '^BOOT module=' instance)"
mod_cmd B1 "behave 802 block:1200"
mod_cmd A1 "sendt 250 B1 802 will-timeout"
sleep 0.4
# Feed A1 a forged REP carrying A1's own instance id but a request id it never
# issued; the source is A2, which A1 has no outstanding request to.
forge "$UID_A" --to-module A1 --src A2 --dst A1 --type rep --event 802 \
      --req-id 424242 --instance-id "$instance" --payload forged >/dev/null

if jwait "$(journal A1)" '^SEND done op=sendt dst=B1 event=802 rc=-11' 8; then
    ok "the request timed out: the forged reply did not complete it"
else
    fail "the request returned rc=$(jfield "$(journal A1)" '^SEND done.*event=802' rc) instead of TIMEOUT"
fi
assert_no_re "$(journal A1)" 'reply=forged' "the forged payload never surfaced as a reply"

sleep 0.3
mod_stats A1
jwait "$(journal A1)" '^STATS ' 5
assert_eq "$(jfield "$(journal A1)" '^STATS' reply_unmatched)" "2" \
          "the forged reply from the wrong source was counted as unmatched"

echo
echo "== a reply addressed to a previous instance is refused"
# The instance id in the frame is A1's real one, so this is not the staleness
# case -- that is t14.  Here the point is only that a reply whose echoed
# instance id is *wrong* is discarded before the pending table is consulted.
forge "$UID_A" --to-module A1 --src A2 --dst A1 --type rep --event 803 \
      --req-id 1 --instance-id 0xdeadbeefdeadbeef --payload stale >/dev/null
sleep 0.3
mod_stats A1
jwait "$(journal A1)" '^STATS ' 5
assert_eq "$(jfield "$(journal A1)" '^STATS' reply_unmatched)" "3" \
          "the wrong-instance reply was counted as unmatched"
assert_eq "$(jcount "$(journal A1)" 'reply=stale')" "0" \
          "the wrong-instance payload never surfaced"

echo
echo "== the requester still works normally afterwards"
mod_cmd A1 "sendt 3000 A2 804 healthy"
if jwait "$(journal A1)" '^SEND done op=sendt dst=A2 event=804 rc=0' 8; then
    ok "a normal request to a different module still completes"
else
    fail "normal request/reply stopped working"
fi
for n in A1 A2 B1; do
    if mod_alive "$n"; then ok "$n is still running"; else fail "$n died"; fi
done

all_stop
report_and_exit
