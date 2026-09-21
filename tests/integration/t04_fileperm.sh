#!/usr/bin/env bash
# t04 -- file permissions: the filesystem is the second half of the boundary.
#
# handoff.md 11, row "文件权限": editing the config, seizing another module's
# address, and sending without group membership must all be refused.
#
# The library checks UID and credentials; the kernel checks the socket inode and
# the directory.  Both are asserted here, separately, because a design that
# relies on only one of them fails differently:
#
#   config        root:root 0644 -- a service cannot add a module for itself
#   directory     $LAB/run/<X> is <uid>:ipcmembers 0750 -- no group write, so a
#                 peer cannot create or remove files in a neighbour's directory
#   socket        0620 <uid>:ipcmembers -- members may write, outsiders may not
#   residue       a non-socket left at the socket path must not be deleted
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./common.sh
require_root
require_lab

journal() { mod_journal "$1"; }

# ------------------------------------------------------------------ #
echo "== the configuration is not writable by the services it describes"
conf_mode="$(stat -c '%a' "$LAB_CONF")"
conf_owner="$(stat -c '%U:%G' "$LAB_CONF")"
assert_eq "$conf_mode" "644" "config mode"
assert_eq "$conf_owner" "root:root" "config owner"

before_lines="$(wc -l <"$LAB_CONF")"
res="$(append_probe "$UID_B" "$GID_MEMBERS,$UID_B" "$LAB_CONF" "core HACK1 1502 $LAB_RUN/B/HACK1.sock")"
if [ "$res" = "errno=13" ]; then
    ok "a service UID cannot append a module to the config (EACCES)"
else
    fail "appending to the config returned $res, expected errno=13"
fi
assert_eq "$(wc -l <"$LAB_CONF")" "$before_lines" "the config is unchanged"
if grep -q HACK1 "$LAB_CONF"; then
    fail "a hijack entry landed in the config"
else
    ok "no hijack entry in the config"
fi

res="$(unlink_probe "$UID_B" "$GID_MEMBERS,$UID_B" "$LAB_CONF")"
if [ "$res" = "errno=13" ]; then
    ok "a service UID cannot remove the config"
else
    fail "removing the config returned $res, expected errno=13"
fi
[ -f "$LAB_CONF" ] && ok "the config still exists" || fail "the config was deleted"

# ------------------------------------------------------------------ #
echo
echo "== a neighbour's socket directory cannot be written into"
dir_mode="$(stat -c '%a' "$LAB_RUN/A")"
dir_owner="$(stat -c '%U:%G' "$LAB_RUN/A")"
assert_eq "$dir_mode" "750" "socket directory mode"
assert_eq "$dir_owner" "ipca:ipcmembers" "socket directory owner"
group_digit=$(( (0$dir_mode / 10) % 10 ))
if [ $(( group_digit & 2 )) -eq 0 ]; then
    ok "the directory grants no group write (mode $dir_mode)"
else
    fail "directory mode $dir_mode allows group write"
fi

res="$(sock_bind_probe "$UID_B" "$GID_MEMBERS,$UID_B" "$LAB_RUN/A/hijack.sock")"
if [ "$res" = "errno=13" ]; then
    ok "ipcb cannot create its own socket inside ipca's directory (EACCES)"
else
    fail "bind inside a neighbour's directory returned $res, expected errno=13"
fi
[ -e "$LAB_RUN/A/hijack.sock" ] && fail "a hijack socket was created" \
                               || ok "no hijack socket exists"

echo
echo "== a live module's address cannot be taken over"
mod_start A1 "$USER_A"
mod_ready A1 8 || fail "A1 failed to register"
sock="$LAB_RUN/A/A1.sock"
[ -S "$sock" ] && ok "A1's socket exists" || fail "A1's socket is missing"

sock_mode="$(stat -c '%a' "$sock")"
sock_owner="$(stat -c '%U:%G' "$sock")"
assert_eq "$sock_mode" "620" "socket mode"
assert_eq "$sock_owner" "ipca:ipcmembers" "socket owner"

res="$(sock_bind_probe "$UID_B" "$GID_MEMBERS,$UID_B" "$sock")"
if [ "$res" = "errno=98" ]; then
    ok "binding over A1's live address is refused with EADDRINUSE"
elif [ "$res" = "errno=13" ]; then
    ok "binding over A1's live address is refused with EACCES"
else
    fail "binding over A1's live address returned $res (expected 98 or 13)"
fi

res="$(unlink_probe "$UID_B" "$GID_MEMBERS,$UID_B" "$sock")"
if [ "$res" = "errno=13" ]; then
    ok "ipcb cannot unlink A1's socket (EACCES)"
else
    fail "unlinking a neighbour's socket returned $res, expected errno=13"
fi
[ -S "$sock" ] && ok "A1's socket survived the unlink attempt" \
               || fail "A1's socket was removed by a peer"

# ------------------------------------------------------------------ #
echo
echo "== sending needs group membership, not just a reachable path"
# A member may write to a neighbour's socket; the non-member may not even open
# it, because the inode is 0620 and the path component is 0710.
out="$(forge "$UID_B" --to-module A1 --src B1 --dst A1 --event 410 --payload member)"
if printf '%s' "$out" | grep -Eq 'rc=0'; then
    ok "a group member can write to A1's socket"
else
    fail "a group member was refused: $out"
fi

out="$(forge_outsider --to-module A1 --src A1 --dst A1 --event 411 --payload outsider)"
printf '%s\n' "$out" | sed 's/^/   /'
if printf '%s' "$out" | grep -Eq 'errno=13'; then
    ok "a non-member cannot write to A1's socket (EACCES)"
else
    fail "a non-member was allowed to write: $out"
fi
res="$(sock_bind_probe "$UID_X" "$UID_X" "$LAB_RUN/A/outsider.sock")"
if [ "$res" = "errno=13" ]; then
    ok "a non-member cannot traverse into the socket directory"
else
    fail "non-member traversal returned $res, expected errno=13"
fi

echo
echo "== a leftover non-socket at the module path is not deleted"
# Registration is the only thing that may clean up a residue, and it refuses to
# remove anything that is not a socket it owns.  A root-owned regular file must
# therefore survive and the registration must fail.
impostor="$LAB_RUN/B/B3.sock"
rm -f "$impostor"
printf 'not a socket\n' >"$impostor"
chown root:root "$impostor"
mod_try b3_impostor B3 "$USER_B"
if mod_expect_fail b3_impostor 5; then
    rc="$(jfield "$(journal b3_impostor)" '^BOOT module=' rc)"
    assert_eq "$rc" "-7" "registration refused a foreign non-socket residue"
    [ -f "$impostor" ] && ok "the impostor file was left alone" \
                       || fail "the impostor file was deleted"
else
    fail "registration succeeded over a foreign non-socket file"
fi
rm -f "$impostor"

mod_cmd A1 "stats"
jwait "$(journal A1)" '^STATS ' 5
assert_eq "$(jfield "$(journal A1)" '^STATS' rejected)" "0" "A1 rejected nothing"
if jwait "$(journal A1)" "$(recv_re B1 A1 410)" 5; then
    ok "A1 received the member's datagram"
else
    fail "A1 did not receive the member's datagram"
fi
assert_no_re "$(journal A1)" '^RECV src=A1 .*event=411' "A1 never saw the outsider's datagram"

all_stop
report_and_exit
