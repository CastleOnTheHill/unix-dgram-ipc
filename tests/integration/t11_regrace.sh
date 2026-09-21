#!/usr/bin/env bash
# t11 -- registration races: a live instance must never be deleted or replaced.
#
# handoff.md 11, row "注册竞争": duplicate registration and parallel contention
# must leave the living instance untouched.
#
# The mechanism under test is the non-blocking exclusive flock on
# <socket>.lock.  Three shapes are exercised:
#
#   same process   ipc_register() called twice -> IPC_ERR_BUSY
#   second process an identical module id started while the first is alive
#   parallel race  six processes started together; exactly one may win
#
# In every case the assertions are made on the *winner's* observable state (its
# socket inode, its reachability, its journal), not on the loser's exit status
# alone -- a lock that is acquired by clobbering the incumbent would still make
# the loser look correct.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./common.sh
require_root
require_lab

journal() { mod_journal "$1"; }

# ------------------------------------------------------------------ #
echo "== the same process cannot register twice"
mod_start A1 "$USER_A"
mod_ready A1 8 || fail "A1 failed to register"
a1_inode_before="$(stat -c %i "$LAB_RUN/A/A1.sock")"

mod_cmd A1 "re-register"
if jwait "$(journal A1)" '^REREGISTER ' 5; then
    assert_eq "$(jfield "$(journal A1)" '^REREGISTER' rc)" "-8" \
              "a second registration in the same process returns IPC_ERR_BUSY"
else
    fail "the re-register command produced no result"
fi
assert_no_re "$(journal A1)" '^REREGISTER unexpected_success' \
             "no second context was created"
assert_eq "$(stat -c %i "$LAB_RUN/A/A1.sock")" "$a1_inode_before" \
          "the live socket was not recreated"

echo
echo "== a second process cannot take a live module"
mod_start A2 "$USER_A"
mod_ready A2 8 || fail "A2 failed to register"
a2_inode_before="$(stat -c %i "$LAB_RUN/A/A2.sock")"

mod_try a2_dup A2 "$USER_A"
if mod_expect_fail a2_dup 6; then
    assert_eq "$(jfield "$(journal a2_dup)" '^BOOT module=' rc)" "-8" \
              "the duplicate process was refused with IPC_ERR_BUSY"
    assert_no_re "$(journal a2_dup)" '^SOCK path=' "the duplicate created no socket"
else
    fail "the duplicate process was not refused"
fi
assert_eq "$(stat -c %i "$LAB_RUN/A/A2.sock")" "$a2_inode_before" \
          "the incumbent's socket inode is unchanged"
assert_eq "$(stat -c '%U:%G' "$LAB_RUN/A/A2.sock")" "ipca:ipcmembers" \
          "the incumbent still owns the socket"

# The incumbent is still fully functional.
mod_cmd A1 "post A2 900 incumbent-lives"
if jwait "$(journal A2)" "$(recv_re A1 A2 900)" 5; then
    ok "the incumbent instance kept serving after the failed takeover"
else
    fail "the incumbent instance stopped serving"
fi

# ------------------------------------------------------------------ #
echo "== six processes race for one module; exactly one may win"
RACE_KEYS=(race1 race2 race3 race4 race5 race6)
for k in "${RACE_KEYS[@]}"; do
    mod_try "$k" C1 "$USER_C"
done

winners=0
busy=0
other=0
winner_key=""
for k in "${RACE_KEYS[@]}"; do
    # A loser exits by itself; the winner stays alive waiting for commands, so
    # the wait is short on purpose.
    wait_exit "${MOD_PID[$k]}" 2
    if grep -Eq '^SOCK path=' "$(journal "$k")"; then
        winners=$((winners + 1))
        winner_key="$k"
    else
        rc="$(jfield "$(journal "$k")" '^BOOT module=' rc)"
        if [ "$rc" = "-8" ]; then
            busy=$((busy + 1))
        else
            other=$((other + 1))
            echo "   unexpected outcome for $k: rc=$rc"
        fi
    fi
done

assert_eq "$winners" "1" "exactly one contender registered"
assert_eq "$busy" "5" "the other five were refused with IPC_ERR_BUSY"
assert_eq "$other" "0" "no contender failed for any other reason"

if [ -n "$winner_key" ]; then
    ok "the winner is $winner_key"
    c1_inode="$(stat -c %i "$LAB_RUN/C/C1.sock")"
    assert_eq "$(stat -c '%U:%G' "$LAB_RUN/C/C1.sock")" "ipcc:ipcmembers" \
              "the winner owns the address"

    # A further three late contenders must also be refused, and must leave the
    # winner's inode alone: a lock that is released and re-taken, or an address
    # that is unlinked and rebound, would show up here.
    for k in late1 late2 late3; do
        mod_try "$k" C1 "$USER_C"
        wait_exit "${MOD_PID[$k]}" 2
        assert_eq "$(jfield "$(journal "$k")" '^BOOT module=' rc)" "-8" \
                  "$k was refused too"
    done
    assert_eq "$(stat -c %i "$LAB_RUN/C/C1.sock")" "$c1_inode" \
              "the winner's socket inode survived three more contenders"

    # Reachability is the only proof that matters.
    mod_cmd A1 "post C1 901 still-here"
    if jwait "$(journal "$winner_key")" "$(recv_re A1 C1 901)" 6; then
        ok "the winning instance is reachable under its own module id"
    else
        fail "the winner is not reachable"
    fi
    assert_eq "$(jcount "$(journal "$winner_key")" '^SOCK path=')" "1" \
              "the winner registered exactly once"

    mod_stop "$winner_key"
    if [ -e "$LAB_RUN/C/C1.sock" ]; then
        fail "the winner's graceful stop left its socket behind"
    else
        ok "after the winner stopped, the address is free again"
    fi
    # And a fresh registration now succeeds, proving the lock really was released.
    mod_start C1 "$USER_C"
    if mod_ready C1 8; then
        ok "a fresh instance registers once the race winner has stopped"
    else
        fail "the lock was not released: $(cat "$(journal C1)")"
    fi
fi

echo
echo "== the module table itself was never modified"
assert_eq "$(stat -c '%a' "$LAB_CONF")" "644" "config mode unchanged"
assert_eq "$(wc -l <"$LAB_CONF")" "14" "config line count unchanged"

for k in A1 A2 C1; do
    if mod_alive "$k"; then ok "$k is still running"; else fail "$k died"; fi
done

all_stop
report_and_exit
