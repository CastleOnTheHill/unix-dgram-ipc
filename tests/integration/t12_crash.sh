#!/usr/bin/env bash
# t12 -- crash recovery: a killed process leaves its address behind, and the
# next instance has to reclaim it safely.
#
# handoff.md 11, rows "崩溃恢复" and part of "生命周期": after SIGKILL the socket
# file is still there (no teardown ran), so registration has to detect the
# residue, decide whether it is allowed to remove it, and only then bind.
#
# Safety has two halves, and both are asserted:
#
#   reclaim  a stale socket owned by *us* is removed while the lock is held, so
#            two instances can never race over the same path
#   refuse   a stale object owned by *someone else*, or one that is not a socket
#            at all, is never deleted
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./common.sh
require_root
require_lab

journal() { mod_journal "$1"; }

mod_start A1 "$USER_A"
mod_ready A1 8 || fail "A1 failed to register"
mod_start A3 "$USER_A"
mod_ready A3 8 || fail "A3 failed to register"

sock="$LAB_RUN/A/A3.sock"

# ------------------------------------------------------------------ #
echo "== SIGKILL leaves the address behind"
instance_before="$(jfield "$(journal A3)" '^BOOT module=' instance)"
inode_before="$(stat -c %i "$sock")"
mod_kill A3 KILL
if [ -S "$sock" ]; then
    ok "the killed instance left its socket file behind (no teardown ran)"
else
    fail "the socket file vanished, so this is not the crash case"
fi

# The address exists but nobody is bound to it: the kernel answers
# ECONNREFUSED, which the library reports as OFFLINE -- not as "module unknown".
mod_cmd A1 "post A3 1000 after-crash"
if jwait "$(journal A1)" '^POST dst=A3 event=1000 rc=-6' 5; then
    ok "a post to the crashed address reports IPC_ERR_OFFLINE, not NOENT"
else
    fail "a post to the crashed address returned rc=$(jfield "$(journal A1)" '^POST dst=A3 event=1000' rc)"
fi

# ------------------------------------------------------------------ #
echo "== the next instance reclaims it"
# The proof that the residue was *removed* rather than silently reused is the
# library's own log line.  An inode number cannot be used here: ext4 recycles
# inodes aggressively, so unlink()+bind() usually lands on the same number.
mod_start A3 "$USER_A"
if mod_ready A3 8; then
    ok "the module registered again over the residue"
else
    fail "registration failed over its own residue: $(cat "$(journal A3)")"
fi
if jwait "$(mod_out A3)" 'removing stale socket residue at' 3; then
    ok "the restart logged that it removed the stale address"
else
    fail "no residue removal was logged, so the stale file may have been reused"
fi
assert_re "$(mod_out A3)" "removing stale socket residue at $sock" \
          "the log names the exact path that was reclaimed"
assert_eq "$(stat -c '%U:%G' "$sock")" "ipca:ipcmembers" "the reclaimed socket is owned correctly"
assert_eq "$(stat -c '%a' "$sock")" "620" "the reclaimed socket has the right mode"
if [ -S "$sock" ]; then ok "the address is a socket again"; else fail "the address is not a socket"; fi

echo
echo "== the restarted instance is a new generation"
instance_after="$(jfield "$(journal A3)" '^BOOT module=' instance)"
if [ "$instance_after" != "$instance_before" ]; then
    ok "the instance id changed across the restart (generation is not reused)"
else
    fail "the instance id was reused across a restart"
fi
mod_cmd A1 "post A3 1001 after-restart"
if jwait "$(journal A3)" "$(recv_re A1 A3 1001)" 5; then
    ok "the restarted instance serves traffic"
else
    fail "the restarted instance is not reachable"
fi

# ------------------------------------------------------------------ #
echo "== repeated crash/restart cycles stay clean"
cycle_ok=1
for round in 1 2 3; do
    mod_kill A3 KILL
    if [ ! -S "$sock" ]; then
        cycle_ok=0
        echo "   round $round: the crash left no residue, so the round is not representative"
        break
    fi
    mod_start A3 "$USER_A"
    if ! mod_ready A3 6; then
        cycle_ok=0
        echo "   round $round failed to re-register"
        break
    fi
    # Each restart must reclaim the residue its predecessor left.
    if ! jwait "$(mod_out A3)" 'removing stale socket residue at' 3; then
        cycle_ok=0
        echo "   round $round did not log a residue removal"
        break
    fi
done
if [ "$cycle_ok" -eq 1 ]; then
    ok "three crash/restart cycles each reclaimed the address"
else
    fail "a crash/restart cycle did not recover"
fi
# Exactly one socket file must exist, and exactly one stale lock file.
sock_count="$(find "$LAB_RUN/A" -maxdepth 1 -name 'A3.sock' | wc -l | tr -d ' ')"
lock_count="$(find "$LAB_RUN/A" -maxdepth 1 -name 'A3.sock.lock' | wc -l | tr -d ' ')"
assert_eq "$sock_count" "1" "exactly one socket file for A3"
assert_eq "$lock_count" "1" "exactly one lock file for A3"

# ------------------------------------------------------------------ #
echo
echo "== a residue owned by someone else is never deleted"
# This is the dangerous case: if registration removed whatever it found, a
# misconfigured or hostile file at the module path would be destroyed -- and a
# real address owned by another UID could be hijacked.
mod_start B1 "$USER_B"
mod_ready B1 8 || fail "B1 failed to register"
b1_sock="$LAB_RUN/B/B1.sock"
foreign="$LAB_RUN/B/B3.sock"
rm -f "$foreign"
# A root-owned socket at B3's path.  B3's entry says uid 1502, so a process
# running as ipcb must refuse to remove it.
python3 - "$foreign" <<'PY'
import socket
import sys

s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(sys.argv[1])
s.close()          # the path stays behind after close()
PY
if [ -S "$foreign" ]; then
    ok "a foreign-owned socket was planted at B3's path"
else
    fail "could not plant the foreign socket"
fi
foreign_inode="$(stat -c %i "$foreign")"
mod_try b3_foreign B3 "$USER_B"
if mod_expect_fail b3_foreign 6; then
    assert_eq "$(jfield "$(journal b3_foreign)" '^BOOT module=' rc)" "-7" \
              "registration refused a socket owned by another UID"
    assert_re "$(mod_out b3_foreign)" 'refusing to remove' \
              "the refusal is logged as a deliberate refusal, not a bind failure"
else
    fail "registration succeeded over a socket owned by another UID"
fi
if [ -S "$foreign" ] && [ "$(stat -c %i "$foreign")" = "$foreign_inode" ]; then
    ok "the foreign socket was left untouched"
else
    fail "the foreign socket was modified or removed"
fi
rm -f "$foreign"

echo
echo "== a socket owned by us but with no live process is reclaimed in place"
# Same UID as the entry, so this one *is* ours to clean up.  It stands in for a
# crash where the lock was released but the path was not unlinked.
self_path="$LAB_RUN/B/B2.sock"
rm -f "$self_path"
as_uid "$UID_B" python3 - "$self_path" <<'PY'
import socket
import sys

s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(sys.argv[1])
s.close()
PY
if [ -S "$self_path" ]; then
    ok "a same-UID residue was planted at B2's path"
else
    fail "could not plant the same-UID residue"
fi
mod_try b2_self B2 "$USER_B"
if mod_ready b2_self 6; then
    ok "the same-UID residue was reclaimed and the module registered"
    assert_eq "$(stat -c '%U:%G' "$self_path")" "ipcb:ipcmembers" \
              "the reclaimed socket has the right owner"
else
    fail "registration failed over a same-UID residue: $(cat "$(journal b2_self)")"
fi
mod_stop b2_self

echo
echo "== every module is still healthy"
for k in A1 A3 B1; do
    if mod_alive "$k"; then ok "$k is still running"; else fail "$k died"; fi
done
mod_cmd A1 "post B1 1002 final"
if jwait "$(journal B1)" "$(recv_re A1 B1 1002)" 5; then
    ok "cross-UID traffic still works after all the recovery work"
else
    fail "cross-UID traffic broke"
fi

all_stop
report_and_exit
