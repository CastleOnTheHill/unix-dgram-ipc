#!/usr/bin/env bash
# teardown_lab.sh -- remove everything setup_lab.sh created.
#
# Deliberately conservative: it only touches $LAB and the five identities with
# the exact UIDs/GIDs the setup script created.  It refuses to delete a user or
# group whose UID/GID does not match, so it can never take out a real account.
#
# Run as root: sudo bash tests/integration/teardown_lab.sh [--keep-users]

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LAB="${IPC_LAB:-/opt/ipc-lab}"
# `rm -rf "$LAB"` as root is the single most destructive line in the repository,
# so the guard runs before anything else.
# shellcheck source=lab_guard.sh
. "$HERE/lab_guard.sh"
guard_lab_path "$LAB" || exit 1
KEEP_USERS=0
[ "${1:-}" = "--keep-users" ] && KEEP_USERS=1

if [ "$(id -u)" -ne 0 ]; then
    echo "teardown_lab.sh must run as root" >&2
    exit 1
fi

echo "== killing any leftover test modules"
pkill -f "$LAB/bin/ipc_testmod" 2>/dev/null
sleep 0.2

echo "== removing $LAB"
rm -rf "$LAB"

if [ "$KEEP_USERS" -eq 0 ]; then
    for spec in "ipca:1501" "ipcb:1502" "ipcc:1503" "ipcx:1504"; do
        name="${spec%%:*}"
        uid="${spec##*:}"
        if getent passwd "$name" >/dev/null; then
            actual="$(getent passwd "$name" | cut -d: -f3)"
            if [ "$actual" = "$uid" ]; then
                userdel "$name" && echo "   removed user $name"
            else
                echo "   KEEPING user $name (uid $actual != $uid)"
            fi
        fi
    done
    for spec in "ipcmembers:1500" "ipca:1501" "ipcb:1502" "ipcc:1503" "ipcx:1504"; do
        name="${spec%%:*}"
        gid="${spec##*:}"
        if getent group "$name" >/dev/null; then
            actual="$(getent group "$name" | cut -d: -f3)"
            if [ "$actual" = "$gid" ]; then
                groupdel "$name" && echo "   removed group $name"
            else
                echo "   KEEPING group $name (gid $actual != $gid)"
            fi
        fi
    done
fi

echo "== done"
