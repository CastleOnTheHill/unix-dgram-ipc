#!/usr/bin/env bash
# t05 -- protocol robustness: malformed datagrams must not do anything.
#
# handoff.md 11, row "协议健壮性": short header, wrong version, oversized
# length, overflow, truncation -- no crash, no out-of-bounds access, no leak.
#
# Every datagram here is sent from a UID that is genuinely authorised for the
# claimed source module, so the credential check passes and the *protocol* layer
# is the only thing left that can reject it.  That is the point: a rejection
# has to be attributable.
#
# A1 runs with a small --max-payload on purpose, so one of the cases exercises
# the kernel's MSG_TRUNC path rather than the library's length check.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./common.sh
require_root
require_lab

journal() { mod_journal "$1"; }

RECV_MAX=4096

echo "== receiver with a small payload cap"
mod_start A1 "$USER_A" --max-payload $RECV_MAX
mod_ready A1 8 || fail "A1 failed to register"
mod_start B1 "$USER_B"
mod_ready B1 8 || fail "B1 failed to register"

fds_before="$(proc_fds "${MOD_PID[A1]}")"
echo "   A1 descriptors before: $fds_before"

# Each case carries a distinct event id so the journal can prove that not one of
# them reached a business callback.
forge_case() { # <event> <description> <forge args...>
    local event="$1" desc="$2"
    shift 2
    local out
    out="$(forge "$UID_A" --to-module A1 --src A1 --dst A1 \
                 --event "$event" "$@" 2>&1)"
    if printf '%s' "$out" | grep -Eq 'rc=0'; then
        ok "$desc: the datagram was sent (rejection is the receiver's job)"
    else
        fail "$desc: could not send the probe at all: $out"
    fi
}

echo
echo "== malformed headers"
forge_case 200 "one-byte datagram (shorter than any header)" --raw-hex 55
forge_case 201 "zero-length datagram"                        --raw-hex ""
forge_case 202 "wrong magic"                                 --payload x --bad-magic
forge_case 203 "wrong protocol version"                      --payload x --bad-version 9
forge_case 204 "reserved type value"                         --payload x --bad-type 0
forge_case 205 "non-zero reserved flags"                     --payload x --bad-flags 1
forge_case 206 "header length field that is not 112"         --payload x --bad-hdrlen 0
forge_case 207 "payload length beyond the hard cap"          --payload x --declare-len 999999
forge_case 208 "declared length disagrees with the datagram" --payload hello --declare-len 4096
forge_case 209 "header cut short mid-field"                  --payload hello --truncate 60
forge_case 210 "addressed to a different module"             --payload hello --dst B1

echo
echo "== a datagram larger than the receiver's buffer"
# 60000 bytes is a legal declared payload (under the 64 KiB hard cap) but far
# above A1's 4096-byte runtime cap, so recvmsg() reports MSG_TRUNC.
forge_case 211 "oversized datagram" --payload-size 60000

echo
echo "== the receiver rejected all of them, without touching a handler"
mod_stats A1
if jwait "$(journal A1)" '^STATS ' 5; then
    rejected="$(jfield "$(journal A1)" '^STATS' rejected)"
    proto="$(jfield "$(journal A1)" '^STATS' rej_proto)"
    trunc="$(jfield "$(journal A1)" '^STATS' rej_trunc)"
    cred="$(jfield "$(journal A1)" '^STATS' rej_cred)"
    invoked="$(jfield "$(journal A1)" '^STATS' cb_invoked)"
    read_n="$(jfield "$(journal A1)" '^STATS' recv_read)"
    echo "   read=$read_n rejected=$rejected proto=$proto trunc=$trunc cred=$cred invoked=$invoked"

    if [ "${rejected:-0}" -ge 12 ]; then
        ok "all 12 malformed datagrams were rejected"
    else
        fail "only $rejected of 12 malformed datagrams were rejected"
    fi
    if [ "${proto:-0}" -ge 10 ]; then
        ok "$proto were rejected by header validation"
    else
        fail "only $proto header rejections, expected at least 10"
    fi
    if [ "${trunc:-0}" -ge 1 ]; then
        ok "$trunc were rejected as truncated by the kernel"
    else
        fail "the oversized datagram was not reported as truncated"
    fi
    assert_eq "${cred:-x}" "0" "no malformed datagram was blamed on credentials"
    assert_eq "${invoked:-x}" "0" "no malformed datagram reached a business handler"
else
    fail "A1 never produced a stats line"
fi

# No journal observation for any of the probe events.
for e in 200 201 202 203 204 205 206 207 208 209 210 211; do
    if grep -Eq "event=$e( |\$)" "$(journal A1)"; then
        fail "event $e produced an observation line"
    fi
done
ok "no probe event id appears in A1's journal"

echo
echo "== the receiver is still alive and still useful"
if mod_alive A1; then ok "A1 is still running"; else fail "A1 died"; fi
fds_after="$(proc_fds "${MOD_PID[A1]}")"
if [ "${fds_after:-999}" -le "$fds_before" ]; then
    ok "descriptor count did not grow ($fds_before -> $fds_after)"
else
    fail "descriptor count grew from $fds_before to $fds_after"
fi
rss="$(proc_rss_kb "${MOD_PID[A1]}")"
if [ -n "$rss" ] && [ "$rss" -lt 65536 ]; then
    ok "resident memory is still small (${rss} KiB)"
else
    fail "resident memory looks unbounded (${rss} KiB)"
fi

mod_cmd B1 "post A1 212 legitimate"
if jwait "$(journal A1)" "$(recv_re B1 A1 212)" 5; then
    ok "A1 still delivers a well-formed datagram"
else
    fail "A1 stopped delivering well-formed datagrams"
fi

echo
echo "== a legal payload right at the receiver's cap still works"
# 4096 is exactly A1's runtime cap, so this is the boundary case: it must be
# accepted, which shows the rejections above were about the malformed framing
# and not about a blanket size refusal.
out="$(forge "$UID_B" --to-module A1 --src B1 --dst A1 --event 213 --payload-size $RECV_MAX 2>&1)"
if printf '%s' "$out" | grep -Eq 'rc=0'; then
    if jwait "$(journal A1)" "$(recv_re B1 A1 213)" 5; then
        ok "a payload exactly at the cap is accepted"
    else
        fail "a payload exactly at the cap was not delivered"
    fi
else
    fail "could not send the boundary payload: $out"
fi
out="$(forge "$UID_B" --to-module A1 --src B1 --dst A1 --event 214 --payload-size $((RECV_MAX + 1)) 2>&1)"
if printf '%s' "$out" | grep -Eq 'rc=0'; then
    if jwait_gone "$(journal A1)" "$(recv_re B1 A1 214)" 1; then
        ok "one byte over the cap is rejected"
    else
        fail "a payload one byte over the cap was delivered"
    fi
else
    fail "could not send the over-cap payload: $out"
fi

all_stop
report_and_exit
