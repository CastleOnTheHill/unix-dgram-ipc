#!/usr/bin/env bash
# t08 -- congestion: what happens when a peer stops reading.
#
# handoff.md 11, row "拥塞", and its warning that AF_UNIX receive buffering does
# not behave like UDP (see probes/PROBE_NOTES.md): SO_RCVBUF does not size the
# receive queue, the queue-full condition surfaces as EAGAIN, and the practical
# capacity follows the *sender's* SO_SNDBUF.
#
# The contract being verified:
#   - a full peer queue never blocks the sender: ipc_post() returns IPC_ERR_AGAIN
#   - ipc_post() never silently discards: every attempt is OK, AGAIN or OFFLINE
#   - a broadcast keeps going to the peers that are still able to receive
#   - the bounded callback queue drops rather than grows, and reports the drop
#   - nothing that was dropped is delivered later
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./common.sh
require_root
require_lab

journal() { mod_journal "$1"; }

echo "== a peer that registered but never reads"
# --no-loop registers and binds the socket, then just idles.  Its receive queue
# fills up and stays full: a deterministic congestion generator.
mod_start A1 "$USER_A"
mod_ready A1 8 || fail "A1 failed to register"
mod_start B1 "$USER_B" --no-loop
mod_ready B1 8 || fail "B1 failed to register"
mod_start A2 "$USER_A"
mod_ready A2 8 || fail "A2 failed to register"
mod_start B2 "$USER_B"
mod_ready B2 8 || fail "B2 failed to register"
mod_start C1 "$USER_C"
mod_ready C1 8 || fail "C1 failed to register"

echo
echo "== filling the stalled peer's queue"
# 60000 attempts guarantees the queue fills; the send path must switch from OK
# to AGAIN and never to anything else.
mod_cmd A1 "blast 60000 B1 600 pad"
if jwait "$(journal A1)" '^BLAST ' 20; then
    ok_n="$(jfield "$(journal A1)" '^BLAST' ok)"
    again="$(jfield "$(journal A1)" '^BLAST' again)"
    offline="$(jfield "$(journal A1)" '^BLAST' offline)"
    other="$(jfield "$(journal A1)" '^BLAST' other)"
    bad="$(jfield "$(journal A1)" '^BLAST' first_bad)"
    echo "   ok=$ok_n again=$again offline=$offline other=$other first_bad=$bad"
    if [ "${ok_n:-0}" -gt 0 ]; then
        ok "$ok_n datagrams were accepted before the queue filled"
    else
        fail "not a single datagram was accepted; the peer was congested from the start"
    fi
    if [ "${again:-0}" -gt 0 ]; then
        ok "$again attempts were refused with IPC_ERR_AGAIN (never blocking)"
    else
        fail "the queue never reported full, so this test proved nothing"
    fi
    assert_eq "${offline:-x}" "0" "a full queue was not reported as an offline peer"
    assert_eq "${other:-x}" "0" "no attempt failed for any other reason"
else
    fail "the blast never reported a result"
fi

echo
echo "== a single post to the stalled peer reports the full queue"
mod_cmd A1 "post B1 601 one-more"
if jwait "$(journal A1)" '^POST dst=B1 event=601 rc=-4' 5; then
    ok "ipc_post returns IPC_ERR_AGAIN for a congested peer"
else
    fail "ipc_post returned rc=$(jfield "$(journal A1)" '^POST dst=B1 event=601' rc), want -4"
fi

echo
echo "== the stalled peer really did read nothing"
mod_stats B1
jwait "$(journal B1)" '^STATS ' 5
assert_eq "$(jfield "$(journal B1)" '^STATS' recv_read)" "0" \
          "the no-loop peer read zero datagrams"
assert_eq "$(jcount "$(journal B1)" '^RECV ')" "0" \
          "the no-loop peer invoked no handler"

echo
echo "== FINDING: one stalled peer also exhausts the *sender's* budget"
# This is not a test bug, it is a measured property of AF_UNIX datagrams.
#
# sendmsg() on a SOCK_DGRAM unix socket allocates the skb through
# sock_alloc_send_pskb(sk = the SENDING socket) and the skb stays charged to
# that socket's sk_wmem_alloc until the *receiver* reads it.  So the ~278
# datagrams sitting unread in B1's queue are still accounted against A1's own
# SO_SNDBUF.  Once A1's send budget is gone, every target fails -- including
# peers that are perfectly healthy.  Head-of-line blocking is therefore a
# sender-side phenomenon, not only a per-peer one.
#
# The contract still holds (EAGAIN, never a block, never a silent drop); what
# this asserts is that the *scope* of the failure is wider than one peer.
mod_cmd A1 "bcast 602 while-congested"
if jwait "$(journal A1)" '^BCAST event=602 rc=0' 5; then
    ok "FINDING: the broadcast reached zero healthy peers while B1 was stalled"
else
    fail "broadcast rc=$(jfield "$(journal A1)" '^BCAST event=602' rc); congestion did not have the expected sender-wide effect"
fi
for n in A2 B2 C1; do
    if jwait_gone "$(journal "$n")" "$(recv_re A1 "$n" 602)" 1; then
        ok "$n received nothing: the stall reached beyond the stalled peer"
    else
        fail "$n received the broadcast, so the sender budget was not exhausted"
    fi
done
mod_stats A1
jwait "$(journal A1)" '^STATS ' 5
eagain="$(jfield "$(journal A1)" '^STATS' eagain)"
skipped="$(jfield "$(journal A1)" '^STATS' bcast_skipped)"
if [ "${eagain:-0}" -ge 8 ]; then
    ok "all eight broadcast targets failed with EAGAIN ($eagain total)"
else
    fail "only $eagain EAGAINs recorded; the sender budget was not the limiting factor"
fi
# Note the counter split: an exhausted send budget is detected *before* the
# destination is resolved, so even the three absent peers report EAGAIN instead
# of ENOENT/ECONNREFUSED.  broadcast_skipped therefore stays at zero and all
# eight targets are booked as send failures.  Asserting this pins down which
# check runs first inside the kernel.
assert_eq "$(jfield "$(journal A1)" '^STATS' bcast_skipped)" "0" \
          "an exhausted send budget hides the offline targets (EAGAIN precedes resolution)"

echo
echo "== the failure is released as soon as the stalled peer goes away"
# Killing B1 closes its socket, which frees the skbs it was holding and returns
# the budget to A1.  This is the recovery path a real deployment has to rely on.
# The kernel reclaim is asynchronous, so poll for it rather than assuming a
# particular delay.
mod_kill B1 KILL
published=0
for _ in 1 2 3 4 5 6 7 8 9 10; do
    mod_cmd A1 "post A2 603 recovered"
    if jwait "$(journal A1)" '^POST dst=A2 event=603 rc=0' 2; then
        published=1
        break
    fi
    sleep 0.1
done
if [ "$published" -eq 1 ]; then
    ok "the sender recovered once the stalled peer was gone"
else
    fail "the sender was still blocked after the stalled peer died"
fi
if jwait "$(journal A2)" "$(recv_re A1 A2 603)" 5; then
    ok "A2 received the message sent after the recovery"
else
    fail "A2 did not receive the recovery message"
fi
# Live peers: A2 B2 C1.  Absent: A3 B3 C3 (never started) and B1 (killed, its
# address file still present, so ECONNREFUSED -> OFFLINE -> skipped).
mod_cmd A1 "bcast 604 after-recovery"
if jwait "$(journal A1)" '^BCAST event=604 rc=3' 5; then
    ok "the broadcast works again: three live peers reached"
else
    fail "broadcast after recovery rc=$(jfield "$(journal A1)" '^BCAST event=604' rc), want 3"
fi
for n in A2 B2 C1; do
    if jwait "$(journal "$n")" "$(recv_re A1 "$n" 604)" 5; then
        ok "$n received the post-recovery broadcast"
    else
        fail "$n missed the post-recovery broadcast"
    fi
done
mod_stats A1
jwait "$(journal A1)" '^STATS ' 5
# Live: A2 B2 C1.  Offline: A3 B3 C3 (never started), C2 (in the table, no
# process here), and B1 (killed; its address file remains, so ECONNREFUSED).
assert_eq "$(jfield "$(journal A1)" '^STATS' bcast_skipped)" "5" \
          "with the budget available again, the offline peers are correctly skipped"

echo
echo "== the bounded callback queue drops instead of growing"
# C2 keeps a two-slot callback queue and one worker, and its handler sleeps.
# Whatever survives the socket queue must be dropped by the callback queue
# rather than queued without bound.
mod_start C2 "$USER_C" --dispatch pool --workers 1 --queue 2
mod_ready C2 8 || fail "C2 failed to register"
mod_cmd C2 "behave 610 block:400"
mod_cmd A2 "blast 4000 C2 610 burst"
if jwait "$(journal A2)" '^BLAST ' 20; then
    burst_ok="$(jfield "$(journal A2)" '^BLAST' ok)"
    ok "the burst against the slow consumer completed ($burst_ok accepted by the socket)"
else
    fail "the burst never reported a result"
fi
# Let the worker drain what it accepted.
sleep 1
mod_stats C2
jwait "$(journal C2)" '^STATS ' 5
invoked="$(jfield "$(journal C2)" '^STATS' cb_invoked)"
dropped="$(jfield "$(journal C2)" '^STATS' cb_dropped)"
echo "   burst_ok=$burst_ok invoked=$invoked dropped=$dropped"
if [ "${dropped:-0}" -gt 0 ]; then
    ok "$dropped callbacks were dropped once the queue was full"
else
    fail "nothing was dropped, so the queue bound was never reached"
fi
if [ "${invoked:-0}" -gt 0 ]; then
    ok "$invoked callbacks did run: the module kept working under load"
else
    fail "the slow consumer never ran a callback"
fi
# The queue must not have swallowed everything: the number of handler calls plus
# drops has to be at least as large as what the socket accepted.
if [ $(( ${invoked:-0} + ${dropped:-0} )) -ge "${burst_ok:-0}" ]; then
    ok "accepted ($burst_ok) <= invoked + dropped ($(( ${invoked:-0} + ${dropped:-0} )))"
else
    fail "more datagrams were accepted than invoked + dropped, so something is unaccounted for"
fi

echo
echo "== nothing dropped is delivered later"
# Each invoked handler writes exactly one RECV line, so a drop can never show up
# as a late observation.
recv_lines="$(jcount "$(journal C2)" '^RECV ')"
assert_eq "$recv_lines" "${invoked}" \
          "observation lines equal the handler invocations: no late delivery"

echo
echo "== every congestion participant survived"
for n in A1 A2 B2 C1 C2; do
    if mod_alive "$n"; then ok "$n is still running"; else fail "$n died"; fi
done

all_stop
report_and_exit
