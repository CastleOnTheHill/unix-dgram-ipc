#!/usr/bin/env bash
# run_all.sh -- build the lab, run every integration test, print a summary.
#
# Usage:
#   sudo bash tests/integration/run_all.sh [test ...]     # default: all of them
#   sudo bash tests/integration/run_all.sh --no-setup     # reuse the existing lab
#   sudo bash tests/integration/run_all.sh --list
#
# Must run as root: the tests switch UIDs with setpriv, and without that the
# permission model cannot be exercised at all.  A non-root run does not fail --
# it reports the whole suite as BLOCKED, because "I could not test it" and "it
# passed" must never look the same in the output.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
export IPC_LAB="${IPC_LAB:-/opt/ipc-lab}"
export BUILD="${BUILD:-build}"
# shellcheck source=lab_guard.sh
. "$HERE/lab_guard.sh"
guard_lab_path "$IPC_LAB" || exit 1

RUN_SETUP=1
TESTS=()
for arg in "$@"; do
    case "$arg" in
        --no-setup) RUN_SETUP=0 ;;
        --list)
            for f in "$HERE"/t[0-9][0-9]_*.sh; do basename "$f"; done
            exit 0
            ;;
        -h|--help)
            sed -n '2,12p' "$0"
            exit 0
            ;;
        *) TESTS+=("$HERE/$arg") ;;
    esac
done
if [ ${#TESTS[@]} -eq 0 ]; then
    for f in "$HERE"/t[0-9][0-9]_*.sh; do
        TESTS+=("$f")
    done
fi

echo "================================================================"
echo " libipc integration suite"
echo "   lab      : $IPC_LAB"
echo "   build    : $ROOT/$BUILD"
echo "   tests    : ${#TESTS[@]}"
echo "================================================================"

if [ "$(id -u)" -ne 0 ]; then
    echo
    echo "BLOCKED: this suite needs root (it switches UIDs with setpriv)."
    echo "         Run it as:  sudo bash tests/integration/run_all.sh"
    mkdir -p "$(dirname "$IPC_LAB/results.txt")"
    : >"$IPC_LAB/results.txt"
    for f in "${TESTS[@]}"; do
        printf 'TEST %s blocked 0\n' "$(basename "$f" .sh)" >>"$IPC_LAB/results.txt"
    done
    echo
    echo "Every test is reported BLOCKED, not PASS.  Re-run as root to get a"
    echo "real verdict; handoff.md 11 requires the distinction."
    exit 2
fi

if [ "$RUN_SETUP" -eq 1 ]; then
    echo
    echo "=== preparing the lab"
    bash "$HERE/setup_lab.sh" || {
        echo "setup_lab.sh failed; nothing was run" >&2
        exit 1
    }
else
    echo
    echo "=== reusing the existing lab (--no-setup)"
    if [ ! -x "$IPC_LAB/bin/ipc_testmod" ]; then
        echo "the lab is not prepared; run without --no-setup" >&2
        exit 1
    fi
    : >"$IPC_LAB/results.txt"
fi

started=$(date +%s)
pass=0
fail=0
blocked_n=0
declare -a FAILED_TESTS

for f in "${TESTS[@]}"; do
    name="$(basename "$f" .sh)"
    echo
    echo "----------------------------------------------------------------"
    echo "=== $name"
    echo "----------------------------------------------------------------"
    rc=0
    bash "$f" || rc=$?
    # Classify from the verdict the *test* recorded, not from its exit status.
    #
    # A test can legitimately report BLOCKED and exit 0 (require_root,
    # require_tools), and a test that fails may or may not have written its
    # line yet.  Reading results.txt is the only way "blocked" can never be
    # counted as "passed", which is the one thing this suite promises.
    verdict="$(grep -E "^TEST $name (pass|fail|blocked) " "$IPC_LAB/results.txt" \
               2>/dev/null | tail -1 | awk '{ print $3 }')"
    if [ "$verdict" = "blocked" ]; then
        blocked_n=$((blocked_n + 1))
    elif [ "$verdict" = "pass" ] && [ "$rc" -eq 0 ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        FAILED_TESTS+=("$name")
        echo "   -> recorded verdict '${verdict:-none}' with exit status $rc" >&2
    fi
done
finished=$(date +%s)

echo
echo "================================================================"
echo " summary"
echo "================================================================"
if [ -f "$IPC_LAB/results.txt" ]; then
    awk '{ printf "   %-24s %-8s %s checks\n", $2, $3, $4 }' "$IPC_LAB/results.txt"
fi
echo
echo "   total tests : ${#TESTS[@]}"
echo "   passed      : $pass"
echo "   failed      : $fail"
echo "   blocked     : $blocked_n"
echo "   elapsed     : $((finished - started))s"
if [ "$fail" -gt 0 ]; then
    echo
    echo "   failing: ${FAILED_TESTS[*]}"
    echo
    echo "   per-test output is in $IPC_LAB/logs/ and $IPC_LAB/results.txt"
    exit 1
fi
echo
echo "   all requested tests passed."
exit 0
