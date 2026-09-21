#!/usr/bin/env bash
# t03 -- identity: who is allowed to be whom.
#
# handoff.md 11, row "身份": an unauthorised process must not be able to
# register, and no process may speak as a module that belongs to another UID.
# Rejections must happen before any business callback runs.
#
# Two layers are checked separately, because they fail differently:
#
#   registration   the static table names one UID per module; a process whose
#                  real/effective UID differs is refused with IPC_ERR_PERM
#   wire traffic   the receiver re-derives the sender's UID from
#                  SCM_CREDENTIALS and compares it with the UID the table
#                  assigns to the *claimed* source module
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./common.sh
require_root
require_lab

journal() { mod_journal "$1"; }

# ------------------------------------------------------------------ #
echo "== registration is refused for the wrong UID"
# Each of these would take over another UID's module.  They must all fail, and
# the journal must show IPC_ERR_PERM (-7), not a silent partial registration.
mod_try b_as_a1 A1 "$USER_B"
mod_try c_as_a1 A1 "$USER_C"
mod_try x_as_a1 A1 "$USER_X"
mod_try a_as_b1 B1 "$USER_A"
mod_try a_as_c3 C3 "$USER_A"

for key in b_as_a1 c_as_a1 x_as_a1 a_as_b1 a_as_c3; do
    if mod_expect_fail "$key" 5; then
        rc="$(jfield "$(journal "$key")" '^BOOT module=' rc)"
        assert_eq "$rc" "-7" "$key refused with IPC_ERR_PERM"
        assert_no_re "$(journal "$key")" '^SOCK path=' "$key created no socket"
    else
        fail "$key was not refused at all"
    fi
done

echo
echo "== a module id that is ambiguous or unknown is refused"
# A1 exists in both the 'core' and 'extra' namespaces, so a caller that does not
# say which one it means must be told, not guessed at.
mod_try ambiguous A1 "$USER_A" --no-ns
if mod_expect_fail ambiguous 5; then
    assert_eq "$(jfield "$(journal ambiguous)" '^BOOT module=' rc)" "-15" \
              "ambiguous module id refused with IPC_ERR_CONFIG"
else
    fail "ambiguous module id was accepted"
fi

mod_try unknown Z9 "$USER_A"
if mod_expect_fail unknown 5; then
    assert_eq "$(jfield "$(journal unknown)" '^BOOT module=' rc)" "-5" \
              "unconfigured module id refused with IPC_ERR_NOENT"
else
    fail "unconfigured module id was accepted"
fi

echo
echo "== the 'extra' namespace really is a separate module table entry"
# extra/A1 belongs to UID 1503, so the same module *name* under a different
# namespace has a different owner.  Registering it as ipca must fail, and as
# ipcc must succeed - which also proves the ambiguity above was real.
mod_try extra_a1_wrong A1 "$USER_A" --ns extra
if mod_expect_fail extra_a1_wrong 5; then
    ok "extra/A1 rejected for the core/A1 owner"
else
    fail "extra/A1 accepted for ipca (ns is not being applied?)"
fi
mod_try extra_a1_right A1 "$USER_C" --ns extra
if mod_ready extra_a1_right 5; then
    ok "extra/A1 accepted for ipcc, its configured owner"
else
    fail "extra/A1 refused for its configured owner: $(cat "$(journal extra_a1_right)")"
fi
mod_stop extra_a1_right

# ------------------------------------------------------------------ #
echo
echo "== a module runs under the UID the table assigns it"
mod_start A2 "$USER_A"
mod_ready A2 8 || fail "A2 failed to register"
mod_start B1 "$USER_B"
mod_ready B1 8 || fail "B1 failed to register"
mod_start A1 "$USER_A"
mod_ready A1 8 || fail "A1 failed to register"

echo
echo "== wire identity: claiming another UID's module"
# ipcb is in ipcmembers, so the *file* permission on A2's socket lets it write.
# The datagram must nevertheless be refused, because the kernel says the sender
# is uid 1502 while the claimed module A2 belongs to uid 1501.
out="$(forge "$UID_B" --to-module A2 --src A2 --dst A2 --event 400 --payload spoofed)"
printf '%s\n' "$out" | sed 's/^/   /'
if printf '%s' "$out" | grep -Eq 'rc=0'; then
    ok "the write itself was permitted by the socket mode (permission != authorisation)"
else
    fail "the forged datagram never reached the socket: $out"
fi
if jwait_gone "$(journal A2)" "$(recv_re A2 A2 400)" 1; then
    ok "A2 was not delivered to the handler for a spoofed source UID"
else
    fail "A2 accepted a datagram claiming its own id from another UID"
fi

echo
echo "== wire identity: claiming a module that is not in the table"
forge "$UID_A" --to-module A2 --src Z9 --dst A2 --event 401 --payload ghost >/dev/null
if jwait_gone "$(journal A2)" '^RECV src=Z9 ' 1; then
    ok "A2 rejected an unconfigured source module"
else
    fail "A2 accepted an unconfigured source module"
fi

echo
echo "== wire identity: the trust unit is the UID, not the module id"
# Same UID as A2's owner, different module name.  This is accepted by design:
# the static table maps a UID to a *set* of modules, and one UID is one trust
# domain.  Asserting it makes the boundary explicit instead of accidental.
forge "$UID_A" --to-module A2 --src A3 --dst A2 --event 402 --payload same-uid >/dev/null
if jwait "$(journal A2)" "$(recv_re A3 A2 402)" 5; then
    ok "a peer with A2's own UID may speak as another of that UID's modules (documented)"
else
    fail "a peer with the same UID was refused (inconsistent with the UID-based rule)"
fi

echo
echo "== the receiver counted every rejection and stayed healthy"
mod_stats A2
jwait "$(journal A2)" '^STATS ' 5
cred="$(jfield "$(journal A2)" '^STATS' rej_cred)"
if [ "${cred:-0}" -ge 2 ]; then
    ok "A2 recorded $cred credential rejections (spoofed UID + unknown module)"
else
    fail "A2 recorded only $cred credential rejections, expected at least 2"
fi
assert_eq "$(jfield "$(journal A2)" '^STATS' cb_invoked)" "1" \
          "exactly one datagram reached a business callback"
assert_eq "$(jcount "$(journal A2)" 'event=40[01]')" "0" \
          "no rejected datagram produced an observation line"

# And it must still work afterwards.
mod_start B2 "$USER_B"
mod_ready B2 8 || fail "B2 failed to register"
mod_cmd B2 "post A2 403 legitimate"
if jwait "$(journal A2)" "$(recv_re B2 A2 403)" 5; then
    ok "A2 still accepts legitimate traffic after the rejections"
else
    fail "A2 stopped accepting legitimate traffic"
fi

for k in A1 A2 B1 B2; do
    if mod_alive "$k"; then ok "$k is still running"; else fail "$k died"; fi
done

all_stop
report_and_exit
