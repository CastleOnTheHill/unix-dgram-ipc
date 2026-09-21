#!/usr/bin/env bash
# t13 -- fork/dup reference inheritance, i.e. what "close" actually means here.
#
# handoff.md 11, row "引用继承": a forked or dup'ed reference keeps the object
# alive, and the last reference to close is the one that matters.
#
# Two independent lifetimes are involved and they behave differently, which is
# the whole reason the design needs an explicit lock:
#
#   the *socket*  stays bound and addressable as long as ANY process holds an
#                 open descriptor for it -- including a forked child that knows
#                 nothing about the library.  So killing the parent does not
#                 free the address.
#   the *lock*    is an flock on an open file description, and fork() shares
#                 that description.  So the child keeps the module "registered"
#                 from the lock's point of view even after the parent is gone.
#
# Consequence, asserted below: after the parent is killed, a new instance must
# be refused (BUSY) while datagrams to the old address still *succeed* -- and
# only when the child exits do both the address and the lock come free.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./common.sh
require_root
require_lab

journal() { mod_journal "$1"; }

CHILD_PID=""
cleanup_all() {
    if [ -n "$CHILD_PID" ]; then
        kill -KILL "$CHILD_PID" 2>/dev/null
    fi
    all_stop
}
trap cleanup_all EXIT

echo "== a handler forks a child that inherits the descriptors"
# forkprobe: the child keeps the socket, the lock and the FIFO, reports through
# its own file, and sleeps.  It is a stand-in for a service that forks workers.
mod_start A1 "$USER_A"
mod_ready A1 8 || fail "A1 failed to register"
mod_start B1 "$USER_B"
mod_ready B1 8 || fail "B1 failed to register"

mod_cmd B1 "behave 1100 forkprobe:4"
mod_cmd A1 "post B1 1100 fork-me"
if jwait "$(journal B1)" '^FORKPROBE child=' 8; then
    CHILD_PID="$(jfield "$(journal B1)" '^FORKPROBE' child)"
    ok "the handler forked a child (pid $CHILD_PID)"
else
    fail "the handler never forked"
fi
assert_re "$(journal B1)" "$(recv_re A1 B1 1100)" "the request reached the handler"

# The child must not be able to use the inherited context.
child_report="$(journal B1).child"
if jwait "$child_report" '^CHILD ' 5; then
    post_rc="$(jfield "$child_report" '^CHILD' post_rc)"
    expect_rc="$(jfield "$child_report" '^CHILD' expect_rc)"
    echo "   child report: post_rc=$post_rc expect_rc=$expect_rc"
    assert_eq "$post_rc" "-17" \
              "the forked child was refused with IPC_ERR_STATE, not allowed to share the parent's state"
else
    fail "the forked child produced no report"
fi

echo
echo "== killing the parent does not free the address"
b1_sock="$LAB_RUN/B/B1.sock"
lock_path="$b1_sock.lock"
[ -S "$b1_sock" ] && ok "the parent's socket exists before the kill" \
                 || fail "the parent's socket is missing"
mod_kill B1 KILL
if ! kill -0 "$CHILD_PID" 2>/dev/null; then
    fail "the child died with the parent, so this test proves nothing"
else
    ok "the forked child outlived the parent"
fi
if [ -S "$b1_sock" ]; then
    ok "the address is still present (the child holds a reference)"
else
    fail "the address disappeared although a reference is open"
fi

echo
echo "== the datagram path still works, because the reference is still open"
# This is the counter-intuitive half: sendto() succeeds and the datagram is
# queued on a socket nobody will read.  It is exactly why a lifecycle lock is
# needed -- the address alone does not tell you whether a module is alive.
mod_cmd A1 "post B1 1101 into-the-void"
if jwait "$(journal A1)" '^POST dst=B1 event=1101 rc=0' 5; then
    ok "a post to the orphaned address was accepted (still bound)"
else
    fail "a post to the orphaned address failed: rc=$(jfield "$(journal A1)" '^POST dst=B1 event=1101' rc)"
fi

echo
echo "== the lock is still held, so no new instance may register"
mod_try b1_after_kill B1 "$USER_B"
if mod_expect_fail b1_after_kill 6; then
    assert_eq "$(jfield "$(journal b1_after_kill)" '^BOOT module=' rc)" "-8" \
              "registration is refused with IPC_ERR_BUSY while the child holds the lock"
    assert_no_re "$(journal b1_after_kill)" '^SOCK path=' \
                 "the refused instance did not touch the address"
else
    fail "registration succeeded although a forked child still holds the lock"
fi
if [ -S "$b1_sock" ]; then
    ok "the address survived the refused registration attempt"
else
    fail "the refused attempt removed the address"
fi

echo
echo "== the last reference releases both the address and the lock"
# Wait for the child to finish its sleep and exit.
waited=0
while kill -0 "$CHILD_PID" 2>/dev/null && [ "$waited" -lt 200 ]; do
    sleep 0.1
    waited=$((waited + 1))
done
if kill -0 "$CHILD_PID" 2>/dev/null; then
    fail "the child is still alive after its sleep; cannot test the release"
else
    ok "the forked child exited"
    CHILD_PID=""
fi

mod_cmd A1 "post B1 1102 after-release"
if jwait "$(journal A1)" '^POST dst=B1 event=1102 rc=-6' 5; then
    ok "once the last reference closed, the orphaned address refuses datagrams"
else
    fail "a post after the release returned rc=$(jfield "$(journal A1)" '^POST dst=B1 event=1102' rc), want -6"
fi

mod_start B1 "$USER_B"
if mod_ready B1 8; then
    ok "the module registers again once the child is gone"
else
    fail "the lock was not released after the child exited: $(cat "$(journal B1)")"
fi
mod_cmd A1 "post B1 1103 new-generation"
if jwait "$(journal B1)" "$(recv_re A1 B1 1103)" 5; then
    ok "the new instance serves traffic normally"
else
    fail "the new instance does not serve traffic"
fi

echo
echo "== the descriptor-level probe agrees"
# ref_inherit is the focused probe for the same kernel behaviour.  Run it and
# assert on its individual findings rather than on a summary line, so the
# integration suite carries the mechanism, not just a verdict.
if [ -x "$LAB_BIN/ref_inherit" ]; then
    probe_out="$LAB_TMP/ref_inherit.out"
    "$LAB_BIN/ref_inherit" >"$probe_out" 2>&1
    printf '%s\n' "$(cat "$probe_out")" | sed 's/^/   /'

    # An unlinked path is unroutable even while an inherited fd is open: it is
    # the *path* that carries datagrams.
    assert_re "$probe_out" 'PARENT send_after_unlink rc=-2' \
              "send to an unlinked path fails with ENOENT (probe)"
    # ... yet the inherited descriptor itself is still valid.
    assert_re "$probe_out" 'CHILD inherited_fd_still_valid=1' \
              "the inherited descriptor is still valid (probe)"
    # fork() shares the open file description, so the flock is still held by the
    # child after the original owner is gone.
    assert_re "$probe_out" 'CHILD lock_while_inherited_fd_open rc=-1 errno=11' \
              "the flock is still held through the inherited description (probe)"
    assert_re "$probe_out" 'CHILD lock_after_last_reference_closed rc=0' \
              "the lock is released when the last reference closes (probe)"
else
    annotate "ref_inherit probe binary not present in the lab; skipped"
fi

for k in A1 B1; do
    if mod_alive "$k"; then ok "$k is still running"; else fail "$k died"; fi
done

report_and_exit
