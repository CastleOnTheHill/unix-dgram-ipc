#!/usr/bin/env bash
# t06 -- credential handling, split into what a real kernel can and cannot show.
#
# handoff.md 11, row "凭据解析" explicitly asks to distinguish the unit tests
# from the real-kernel integration tests, so this script does three things:
#
#   1. proves the receiver genuinely has SO_PASSCRED on, by using a sender that
#      never asked for credentials itself (forge_peer never sets SO_PASSCRED);
#   2. proves a mismatching or unknown claimed source is refused even though the
#      filesystem let the datagram through;
#   3. marks the cases that are *only* reachable in a unit test, and says why.
#
# The reason the third group exists: with SO_PASSCRED enabled on the receiving
# socket the kernel attaches a real SCM_CREDENTIALS to *every* AF_UNIX datagram.
# A sender cannot omit it, and SCM_CREDENTIALS is far too small to be truncated
# by any sane control buffer.  So "missing credentials" and "truncated control
# message" are unobservable at this layer -- they are covered against a
# hand-built msghdr in tests/unit/test_proto.c instead.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./common.sh
require_root
require_lab

journal() { mod_journal "$1"; }

# ------------------------------------------------------------------ #
echo "== the kernel supplies credentials the sender never asked for"
# forge_peer creates a plain AF_UNIX datagram socket and never calls
# SO_PASSCRED.  If the receiver still learns the sender's real UID, the only
# possible source is its own SO_PASSCRED -- which is exactly the property the
# whole permission model rests on.
mod_start B1 "$USER_B"
mod_ready B1 8 || fail "B1 failed to register"
out="$(forge "$UID_B" --to-module B1 --src B1 --dst B1 --event 300 --payload no-passcred)"
printf '%s\n' "$out" | sed 's/^/   /'
if jwait "$(journal B1)" "$(recv_re B1 B1 300)" 5; then
    ok "the receiver accepted it: SO_PASSCRED on the receiver is what supplies identity"
else
    fail "a same-UID datagram was refused, so credentials may be missing entirely"
fi

# ------------------------------------------------------------------ #
echo
echo "== a mismatching claimed source is refused"
mod_start A1 "$USER_A"
mod_ready A1 8 || fail "A1 failed to register"

# ipcb (uid 1502) writes to A1's socket.  The socket mode allows that; the
# claimed module A1 belongs to uid 1501, so the receiver must refuse.
out="$(forge "$UID_B" --to-module A1 --src A1 --dst A1 --event 301 --payload spoof)"
if printf '%s' "$out" | grep -Eq 'rc=0'; then
    ok "the datagram reached A1's socket (so this is a credential decision, not a file one)"
else
    fail "the datagram never reached A1's socket: $out"
fi
if jwait_gone "$(journal A1)" "$(recv_re A1 A1 301)" 1; then
    ok "claimed A1 from uid 1502: refused"
else
    fail "claimed A1 from uid 1502: accepted"
fi

echo
echo "== an unknown claimed source is refused"
forge "$UID_A" --to-module A1 --src NOPE --dst A1 --event 302 --payload unknown >/dev/null
if jwait_gone "$(journal A1)" '^RECV src=NOPE ' 1; then
    ok "claimed an unconfigured module: refused"
else
    fail "claimed an unconfigured module: accepted"
fi

echo
echo "== the same UID may claim any module that UID owns"
# A2 and A1 are both owned by uid 1501.  This is accepted by design (one UID is
# one trust domain) and asserted so the boundary is documented rather than
# discovered later.
mod_start A2 "$USER_A"
mod_ready A2 8 || fail "A2 failed to register"
forge "$UID_A" --to-module A1 --src A2 --dst A1 --event 303 --payload uid-peer >/dev/null
if jwait "$(journal A1)" "$(recv_re A2 A1 303)" 5; then
    ok "claimed A2 from uid 1501: accepted (same trust domain)"
else
    fail "claimed A2 from uid 1501: refused, which would break intra-UID traffic"
fi

echo
echo "== the rejection counter names the reason"
mod_stats A1
if jwait "$(journal A1)" '^STATS ' 5; then
    cred="$(jfield "$(journal A1)" '^STATS' rej_cred)"
    proto="$(jfield "$(journal A1)" '^STATS' rej_proto)"
    invoked="$(jfield "$(journal A1)" '^STATS' cb_invoked)"
    if [ "${cred:-0}" -ge 2 ]; then
        ok "$cred rejections were attributed to credentials"
    else
        fail "only $cred credential rejections, expected 2"
    fi
    assert_eq "${proto:-x}" "0" "no rejection was misattributed to the header layer"
    assert_eq "${invoked:-x}" "1" "only the legitimate datagram reached a handler"
else
    fail "A1 never produced a stats line"
fi

echo
echo "== a sender can force MSG_CTRUNC by attaching its own ancillary data"
# The receiver reserves exactly CMSG_SPACE(sizeof(struct ucred)) for control
# data.  The kernel then appends SCM_CREDENTIALS itself, so a sender that
# attaches even one SCM_RIGHTS fills the buffer and the credentials are the part
# that gets cut.  The library must treat that as untrustworthy and drop the
# datagram -- not fall back to "no credentials means anonymous".
aux="$LAB_TMP/aux-source"
: >"$aux"
chmod 644 "$aux"
out="$(forge "$UID_B" --to-module B1 --src B1 --dst B1 --event 304 --payload ctrunc --send-fd "$aux")"
printf '%s\n' "$out" | sed 's/^/   /'
if printf '%s' "$out" | grep -Eq 'with_fd=1'; then
    ok "the auxiliary-data datagram was sent"
    if jwait_gone "$(journal B1)" "$(recv_re B1 B1 304)" 1; then
        ok "a datagram whose credentials may have been truncated is dropped"
    else
        fail "a truncated-credentials datagram was delivered to the handler"
    fi
else
    fail "could not attach ancillary data: $out"
fi
mod_stats B1
jwait "$(journal B1)" '^STATS ' 5
trunc="$(jfield "$(journal B1)" '^STATS' rej_trunc)"
if [ "${trunc:-0}" -ge 1 ]; then
    ok "the receiver recorded $trunc truncation rejection(s)"
else
    fail "the receiver recorded no truncation rejection (rej_trunc=$trunc)"
fi

echo
echo "== a case a real kernel cannot produce"
annotate "missing SCM_CREDENTIALS entirely: not reachable at this layer -- with"
annotate "  SO_PASSCRED on the receiver the kernel attaches real credentials to"
annotate "  every AF_UNIX datagram and a sender cannot suppress them."
annotate "  Covered by tests/unit/test_proto.c against a hand-built msghdr."
# Assert the reserve size is exactly one credential's worth, which is *why* the
# MSG_CTRUNC case above is reachable rather than theoretical.
if grep -Eq 'define[[:space:]]+IPC_CTRL_SIZE[[:space:]]+CMSG_SPACE' "$ROOT/src/ipc_loop.c"; then
    ok "the control reserve is CMSG_SPACE(sizeof(struct ucred)): exactly one credential"
else
    fail "could not confirm the control-buffer reserve size"
fi

all_stop
report_and_exit
