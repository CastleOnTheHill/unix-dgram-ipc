#!/usr/bin/env bash
# setup_lab.sh -- prepare the isolated test lab.
#
# Creates:
#   - group ipcmembers (gid 1500)
#   - users ipca/ipcb/ipcc (uid 1501..1503, all in ipcmembers)
#     and ipcx (uid 1504, deliberately NOT in ipcmembers)
#   - $LAB/run/{A,B,C}  owned by the corresponding service UID
#   - $LAB/conf/ipc-modules.conf (root-owned, mode 0644, read-only to services)
#   - $LAB/{logs,fifo}  world-writable so each service writes its own journal
#   - $LAB/bin/         copies of the test binaries, mode 0755
#
# Nothing outside $LAB is modified except the five identities, and every
# identity is checked for a collision before it is created.
#
# Run as root:  sudo bash tests/integration/setup_lab.sh
# Reverse with: sudo bash tests/integration/teardown_lab.sh

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
LAB="${IPC_LAB:-/opt/ipc-lab}"
# Which build tree to install from.  The sanitizer variant lives in a separate
# tree on purpose (handoff.md 10: "debug, ASan/UBSan and performance builds
# must be kept apart"), so the same suite can be replayed under ASan with
#   BUILD=build-asan IPC_LAB=/opt/ipc-lab-asan bash tests/integration/run_all.sh
BUILD="${BUILD:-build}"

GID_MEMBERS=1500
NS=core

if [ "$(id -u)" -ne 0 ]; then
    echo "setup_lab.sh must run as root (it creates users and chowns files)" >&2
    exit 1
fi
if [ ! -x "$ROOT/$BUILD/bin/ipc_testmod" ]; then
    echo "build first: make all   (looked in $ROOT/$BUILD/bin)" >&2
    exit 1
fi

echo "== lab root: $LAB"
echo "== build   : $ROOT/$BUILD"

# ------------------------------------------------------------------ #
# identities, with collision checks                                   #
# ------------------------------------------------------------------ #

need_group() { # name gid
    if getent group "$1" >/dev/null; then
        local existing
        existing="$(getent group "$1" | cut -d: -f3)"
        if [ "$existing" != "$2" ]; then
            echo "REFUSING: group $1 exists with gid $existing, wanted $2" >&2
            exit 1
        fi
        echo "   group $1 already present (gid $2)"
    else
        groupadd -g "$2" "$1"
        echo "   created group $1 (gid $2)"
    fi
}

need_user() { # name uid group extra_groups
    local name="$1" uid="$2" primary="$3"
    if getent passwd "$name" >/dev/null; then
        local existing
        existing="$(getent passwd "$name" | cut -d: -f3)"
        if [ "$existing" != "$uid" ]; then
            echo "REFUSING: user $name exists with uid $existing, wanted $uid" >&2
            exit 1
        fi
        echo "   user $name already present (uid $uid)"
        return 0
    fi
    useradd --no-create-home --shell /usr/sbin/nologin --uid "$uid" \
            --gid "$primary" "$name"
    echo "   created user $name (uid $uid, gid $primary)"
}

need_group ipcmembers "$GID_MEMBERS"

# Each service identity gets its own primary group plus ipcmembers as a
# supplementary group.  This mirrors the design in handoff.md 5.2: the shared
# group exists so peers may write to each other's sockets, while each
# directory stays owned by exactly one service UID.
need_group ipca 1501; need_group ipcb 1502; need_group ipcc 1503
need_group ipcx 1504

need_user ipca 1501 ipca
need_user ipcb 1502 ipcb
need_user ipcc 1503 ipcc
need_user ipcx 1504 ipcx

for u in ipca ipcb ipcc; do
    usermod -aG ipcmembers "$u"
done
# ipcx is intentionally NOT added to ipcmembers.

# ------------------------------------------------------------------ #
# directories                                                         #
# ------------------------------------------------------------------ #

rm -rf "$LAB/run" "$LAB/logs" "$LAB/fifo" "$LAB/bin" "$LAB/conf"
mkdir -p "$LAB/conf" "$LAB/run" "$LAB/logs" "$LAB/fifo" "$LAB/bin" "$LAB/tmp"

# socket root: root:ipcmembers 0750 -- traversable by members, not writable.
# Everything below it is reachable only through group membership, which is
# exactly the property the permission tests exercise.
chown root:ipcmembers "$LAB/run"
chmod 0750 "$LAB/run"

# one directory per service UID
mkdir -p "$LAB/run/A" "$LAB/run/B" "$LAB/run/C"
chown "1501:$GID_MEMBERS" "$LAB/run/A"; chmod 0750 "$LAB/run/A"
chown "1502:$GID_MEMBERS" "$LAB/run/B"; chmod 0750 "$LAB/run/B"
chown "1503:$GID_MEMBERS" "$LAB/run/C"; chmod 0750 "$LAB/run/C"

# journals and command FIFOs are created by the services themselves
chmod 1777 "$LAB/logs" "$LAB/fifo" "$LAB/tmp"

# ------------------------------------------------------------------ #
# binaries                                                            #
# ------------------------------------------------------------------ #

install -m 0755 -o root -g root "$ROOT/$BUILD/bin/ipc_testmod" "$LAB/bin/"
install -m 0755 -o root -g root "$ROOT/$BUILD/bin/forge_peer"  "$LAB/bin/"
install -m 0755 -o root -g root "$ROOT/$BUILD/bin/ref_inherit" "$LAB/bin/" 2>/dev/null || true
install -m 0755 -o root -g root "$ROOT/$BUILD/bin/queue_probe" "$LAB/bin/" 2>/dev/null || true
install -m 0755 -o root -g root "$ROOT/$BUILD/bin/cred_probe"  "$LAB/bin/" 2>/dev/null || true

# ------------------------------------------------------------------ #
# static module table: three UIDs, nine modules                       #
# ------------------------------------------------------------------ #

CONF="$LAB/conf/ipc-modules.conf"
: >"$CONF"
{
    echo "# namespace module uid socket_path"
    echo "# generated by tests/integration/setup_lab.sh"
    echo "# namespace/uid pairs are configuration, not trust: the receiver still"
    echo "# verifies SCM_CREDENTIALS on every datagram."
    for m in A1 A2 A3; do echo "$NS $m 1501 $LAB/run/A/$m.sock"; done
    for m in B1 B2 B3; do echo "$NS $m 1502 $LAB/run/B/$m.sock"; done
    for m in C1 C2 C3; do echo "$NS $m 1503 $LAB/run/C/$m.sock"; done
    # a second namespace, to prove broadcast scope is the namespace and that
    # a plain "register" refuses an ambiguous module id
    echo "extra A1 1503 $LAB/run/C/extra-A1.sock"
} >>"$CONF"

chown root:root "$CONF"
chmod 0644 "$CONF"

echo "== module table"
sed 's/^/   /' "$CONF" | sed "s#$LAB#\$LAB#g"

# ------------------------------------------------------------------ #
# sanity                                                             #
# ------------------------------------------------------------------ #

echo "== permissions"
stat -c '   %A %U:%G %n' "$LAB" "$LAB/run" "$LAB/run/A" "$LAB/run/B" "$LAB/run/C" "$CONF"

: >"$LAB/results.txt"
echo "== lab ready. Config: $CONF"
echo "== note: 'extra' namespace module A1 exists on purpose (ambiguity test)"
