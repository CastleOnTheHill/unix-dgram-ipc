#!/usr/bin/env bash
# t02 -- concurrency: many modules, many threads, interleaved replies.
#
# handoff.md 11, row "并发": multi-module, multi-thread, interleaved replies
# must not cross packets, must not mis-match a reply, and must stay bounded.
#
# The three sub-tests are deliberately shaped so the *only* way to pass is for
# request/reply matching to be per-request rather than per-source:
#
#   interleaved   three modules block the same target at the same time
#   multi-thread  eight threads in one process issue simultaneous requests and
#                 each one compares the reply payload against its own request
#   storm         two modules broadcast at the same instant
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./common.sh
require_root
require_lab

NAMES=(A1 A2 A3 B1 B2 B3 C1 C2 C3)
user_of() {
    case "$1" in A*) echo "$USER_A" ;; B*) echo "$USER_B" ;; C*) echo "$USER_C" ;; esac
}
journal() { mod_journal "$1"; }

echo "== nine modules, four worker threads each"
for n in "${NAMES[@]}"; do
    mod_start "$n" "$(user_of "$n")" --dispatch pool --workers 4
done
for n in "${NAMES[@]}"; do
    mod_ready "$n" 8 || fail "$n failed to register"
done

# ------------------------------------------------------------------ #
echo
echo "== interleaved requests from three UIDs to one target"
# A3 sleeps in its handler, so all three requests are inside the same target at
# the same time and the replies come back in the order A3 finishes, not in the
# order the requests arrived.
mod_cmd A3 "behave 40 block:800"
mod_cmd A1 "sendt 6000 A3 40 from-a1"
mod_cmd B1 "sendt 6000 A3 40 from-b1"
mod_cmd C1 "sendt 6000 A3 40 from-c1"

for pair in "A1:from-a1" "B1:from-b1" "C1:from-c1"; do
    who="${pair%%:*}"; payload="${pair##*:}"
    if jwait "$(journal "$who")" "^SEND done op=sendt dst=A3 event=40 rc=0" 8; then
        ok "$who completed its request while the others were in flight"
        assert_re "$(journal "$who")" "^SEND done .*reply=$payload" \
                  "$who got its own reply, not a neighbour's"
    else
        fail "$who never completed its request against a busy target"
    fi
done

# ------------------------------------------------------------------ #
echo
echo "== eight concurrent waiters inside one process"
# A2 sleeps per request; B1 fires eight threads.  Each thread sends a distinct
# payload and refuses to accept a reply that is not its own, so a reply routed
# to the wrong waiter shows up as match=0.
mod_cmd A2 "behave 41 block:300"
mod_cmd B1 "sendmany 8 8000 A2 41"
if jwait "$(journal B1)" '^SENDMANY_DONE started=8 of 8' 15; then
    ok "all eight threads were started and joined"
else
    fail "the concurrent batch did not finish"
fi
matched="$(jcount "$(journal B1)" '^SENDMANY n=[0-9]+ rc=0 reply=T[0-9]+ match=1')"
assert_eq "$matched" "8" "every waiter received the reply to its own request"
assert_eq "$(jcount "$(journal B1)" '^SENDMANY .* match=0')" "0" \
          "no waiter received someone else's reply"

# A2 must have delivered each request exactly once, in its own handler.
mod_stats A2
jwait "$(journal A2)" '^STATS ' 5
assert_eq "$(jfield "$(journal A2)" '^STATS' delivered)" "8" \
          "the target delivered eight messages"
assert_eq "$(jfield "$(journal A2)" '^STATS' cb_dropped)" "0" \
          "no callback was dropped: the queue was never saturated"
assert_eq "$(jfield "$(journal A2)" '^STATS' reply_unmatched)" "0" \
          "no reply failed to find its request"
assert_eq "$(jfield "$(journal A2)" '^STATS' rejected)" "0" \
          "the target rejected nothing"

# ------------------------------------------------------------------ #
echo
echo "== simultaneous broadcast from two modules"
mod_cmd B2 "bcast 42 storm"
mod_cmd C2 "bcast 42 storm"
for who in B2 C2; do
    if jwait "$(journal "$who")" '^BCAST event=42 rc=8' 6; then
        ok "$who reached all eight peers while the other was broadcasting"
    else
        fail "$who broadcast rc=$(jfield "$(journal "$who")" '^BCAST event=42' rc), want 8"
    fi
done
for n in A1 A2 A3 B1 B3 C1 C3; do
    if jwait "$(journal "$n")" "$(recv_re B2 "$n" 42)" 6; then
        ok "$n received B2's broadcast"
    else
        fail "$n missed B2's broadcast"
    fi
    if jwait "$(journal "$n")" "$(recv_re C2 "$n" 42)" 6; then
        ok "$n received C2's broadcast"
    else
        fail "$n missed C2's broadcast"
    fi
done
# the two broadcasters still receive each other
if jwait "$(journal B2)" "$(recv_re C2 B2 42)" 6; then
    ok "B2 received C2's broadcast"
else
    fail "B2 missed C2's broadcast"
fi
if jwait "$(journal C2)" "$(recv_re B2 C2 42)" 6; then
    ok "C2 received B2's broadcast"
else
    fail "C2 missed B2's broadcast"
fi

# ------------------------------------------------------------------ #
echo
echo "== resource accounting stays bounded"
for n in A1 A2 B1 B2 C1 C2; do
    mod_stats "$n"
done
sleep 0.3
for n in A1 A2 B1 B2 C1 C2; do
    jwait "$(journal "$n")" '^STATS ' 5 || fail "$n never produced a stats line"
done
assert_eq "$(jfield "$(journal A1)" '^STATS' rejected)" "0" "A1 rejected nothing"
assert_eq "$(jfield "$(journal B1)" '^STATS' rejected)" "0" "B1 rejected nothing"
assert_eq "$(jfield "$(journal C1)" '^STATS' rejected)" "0" "C1 rejected nothing"
for n in A1 A2 B1 B2 C1 C2; do
    d="$(proc_fds "${MOD_PID[$n]}")"
    if [ "${d:-0}" -gt 0 ] && [ "${d:-0}" -lt 64 ]; then
        ok "$n holds a bounded number of descriptors ($d)"
    else
        fail "$n holds $d descriptors"
    fi
done

all_stop
report_and_exit
