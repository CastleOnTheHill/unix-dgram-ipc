#!/usr/bin/env bash
# t01 -- basic interoperability across three UIDs and nine modules.
#
# Covers the handoff table rows "基本互通" and part of "并发":
# every module of every UID can post, reply and broadcast, and the receiver
# sees the right source, destination, event and payload.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./common.sh
require_root
require_lab

NAMES=(A1 A2 A3 B1 B2 B3 C1 C2 C3)
user_of() {
    case "$1" in A*) echo "$USER_A" ;; B*) echo "$USER_B" ;; C*) echo "$USER_C" ;; esac
}
journal() { echo "$LAB_LOGS/$1.journal"; }

echo "== starting nine modules across three UIDs"
for n in "${NAMES[@]}"; do
    mod_start "$n" "$(user_of "$n")" --dispatch pool --workers 2
done
for n in "${NAMES[@]}"; do
    if mod_ready "$n" 8; then ok "$n registered"; else fail "$n failed to register"; fi
done

# Every module must have logged the UID the kernel really gave it: the whole
# permission model rests on uid == the UID in the static table.
echo
echo "== every module runs under its configured UID"
for n in "${NAMES[@]}"; do
    case "$(user_of "$n")" in
        "$USER_A") want=$UID_A ;;
        "$USER_B") want=$UID_B ;;
        *) want=$UID_C ;;
    esac
    got="$(jfield "$(journal "$n")" '^BOOT module=' uid)"
    rc="$(jfield "$(journal "$n")" '^BOOT module=' rc)"
    assert_eq "$rc" "0" "$n boot rc"
    assert_eq "$got" "$want" "$n real UID matches the table"
done

echo
echo "== unicast, one per UID pair"
mod_cmd A1 "post B1 14 hello-b1"
assert_re "$(journal A1)" '^POST dst=B1 event=14 rc=0' "A1 posts to B1"
if jwait "$(journal B1)" "$(recv_re A1 B1 14)" 5; then
    ok "B1 received A1's message"
    assert_re "$(journal B1)" 'event=14 .*data=hello-b1' "payload delivered intact"
else
    fail "B1 never received A1's message"
fi

mod_cmd B2 "post C3 14 hello-c3"
assert_re "$(journal B2)" '^POST dst=C3 event=14 rc=0' "B2 posts to C3"
if jwait "$(journal C3)" "$(recv_re B2 C3 14)" 5; then
    ok "C3 received B2's message"
    assert_re "$(journal C3)" 'src=B2 dst=C3' "source and destination fields correct"
else
    fail "C3 never received B2's message"
fi

mod_cmd C1 "post A2 14 hello-a2"
assert_re "$(journal C1)" '^POST dst=A2 event=14 rc=0' "C1 posts to A2"
if jwait "$(journal A2)" "$(recv_re C1 A2 14)" 5; then
    ok "A2 received C1's message"
else
    fail "A2 never received C1's message"
fi

echo
echo "== synchronous request/reply across UIDs"
mod_cmd C3 "sendt 3000 A1 10 ping-c3"
if jwait "$(journal C3)" '^SEND done op=sendt dst=A1 event=10 rc=0' 5; then
    ok "C3 -> A1 synchronous request completed"
    assert_re "$(journal C3)" 'rc=0 reply_len=7 reply=ping-c3' "reply payload echoed"
else
    fail "C3 -> A1 synchronous request did not complete"
fi

mod_cmd A3 "sendt 3000 B2 11 a-longer-payload-for-b2"
if jwait "$(journal A3)" '^SEND done op=sendt dst=B2 event=11 rc=0' 5; then
    ok "A3 -> B2 synchronous request completed"
    assert_re "$(journal A3)" 'reply=a-longer-payload-for-b2' "longer reply payload intact"
else
    fail "A3 -> B2 synchronous request did not complete"
fi

echo
echo "== broadcast across the whole namespace"
mod_cmd A2 "bcast 20 bc-all"
if jwait "$(journal A2)" '^BCAST event=20 rc=8' 5; then
    ok "A2 broadcast reached all eight other modules"
else
    got="$(jfield "$(journal A2)" '^BCAST event=20' rc)"
    fail "A2 broadcast rc=$got, want 8 (nine modules minus the sender)"
fi

for n in A1 A3 B1 B2 B3 C1 C2 C3; do
    if jwait "$(journal "$n")" "$(recv_re A2 "$n" 20)" 5; then
        ok "$n received the broadcast"
    else
        fail "$n did not receive the broadcast"
    fi
done
assert_no_re "$(journal A2)" '^RECV src=A2 dst=A2' "broadcast does not include the sender"

echo
echo "== sender counters"
mod_cmd A2 "stats"
if jwait "$(journal A2)" '^STATS ' 5; then
    # send_attempts counts one per broadcast target, so it must cover all eight
    # peers.  bcast_targets/bcast_skipped record the fan-out bookkeeping, and
    # enqueued only counts targets that actually accepted the datagram -- the
    # pair only diverges when a peer is offline, which is asserted in t05.
    attempts="$(jfield "$(journal A2)" '^STATS' attempts)"
    targets="$(jfield "$(journal A2)" '^STATS' bcast_targets)"
    skipped="$(jfield "$(journal A2)" '^STATS' bcast_skipped)"
    assert_eq "$targets" "8" "broadcast fan-out counted eight targets"
    assert_eq "$skipped" "0" "no broadcast target skipped while all peers are up"
    if [ "${attempts:-0}" -ge 8 ]; then
        ok "send attempts ($attempts) cover all eight broadcast targets"
    else
        fail "send attempts=$attempts should cover the eight broadcast targets"
    fi
else
    fail "no stats line from A2"
fi

all_stop
report_and_exit
