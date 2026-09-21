#!/usr/bin/env bash
# measure_footprint.sh -- user-space memory cost of a running nine-module lab.
#
# handoff.md 13 lists "user-space memory: sum of PSS and private memory of the
# relevant processes" as a required metric, and 13 also warns that a server's
# RSS is not the net saving after you remove it.  So this script reports, per
# process, the numbers the report needs:
#
#   VmRSS      resident set -- what most people quote, double counts shared pages
#   Pss        proportional set size -- shared pages divided by the sharers,
#              which is the number that is safe to add up across processes
#   Private    private-dirty + private-clean -- the part only this process owns
#   fds        open descriptor count, which is how a leak would show up first
#
# and then the totals, with the double-counting caveat stated in the output
# rather than buried.
#
# This measures the *candidate* shape (no central server: N module processes,
# one datagram socket and one lock file each).  It deliberately does not
# invent a baseline number: handoff.md 13's baseline is a synthetic
# stream-through-relay control, and its resident cost depends on a relay
# implementation we do not have.
#
# Usage: sudo bash tests/integration/measure_footprint.sh [--keep-alive S]
set -uo pipefail

COMMON_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tests/integration/common.sh
source "$COMMON_DIR/common.sh"

export TEST_NAME="measure_footprint"
require_root
require_lab

MODULES=(A1 A2 A3 B1 B2 B3 C1 C2 C3)
user_for() {
    case "$1" in
        A*) echo "$USER_A" ;;
        B*) echo "$USER_B" ;;
        C*) echo "$USER_C" ;;
        *) echo "" ;;
    esac
}

printf '== starting the nine-module lab (namespace core, 3 UIDs)\n'
for m in "${MODULES[@]}"; do
    mod_start "$m" "$(user_for "$m")" || exit 1
done
for m in "${MODULES[@]}"; do
    if ! mod_ready "$m" 10; then
        fail "module $m did not become ready"
        report_and_exit
    fi
done
ok "all nine modules registered"

# Emits exactly four fields: VmRSS_kB Pss_kB Private_kB fds.
# Nothing else -- an extra field makes `read -r a b c d` silently hand the
# remainder to d and the next line blow up on `set -u`.
sample() { # <pid>
    local pid="$1"
    local rss pss priv fds

    rss="$(awk '/^VmRSS:/ {print $2}' "/proc/$pid/status" 2>/dev/null)"
    priv="$(awk '/^Private_Clean:/ {c=$2} /^Private_Dirty:/ {d=$2} END {print c+d}' \
                 "/proc/$pid/smaps_rollup" 2>/dev/null)"
    pss="$(awk '/^Pss:/ {print $2}' "/proc/$pid/smaps_rollup" 2>/dev/null)"
    fds="$(ls "/proc/$pid/fd" 2>/dev/null | wc -l | tr -d ' ')"
    printf '%s %s %s %s\n' \
        "${rss:-0}" "${pss:-0}" "${priv:-0}" "${fds:-0}"
}

printf '\n%-10s %10s %10s %10s %6s\n' module VmRSS_kB Pss_kB Priv_kB fds
printf -- '------------------------------------------------------\n'

sum_rss=0; sum_pss=0; sum_priv=0; sum_fds=0
for m in "${MODULES[@]}"; do
    pid="${MOD_PID[$m]}"
    read -r rss pss priv fds <<<"$(sample "$pid" "$m")"
    printf '%-10s %10s %10s %10s %6s\n' "$m" "$rss" "$pss" "$priv" "$fds"
    sum_rss=$((sum_rss + rss)); sum_pss=$((sum_pss + pss))
    sum_priv=$((sum_priv + priv)); sum_fds=$((sum_fds + fds))
done
printf -- '------------------------------------------------------\n'
printf '%-10s %10s %10s %10s %6s\n' "9 modules" "$sum_rss" "$sum_pss" "$sum_priv" "$sum_fds"

# Per-UID subtotals: the three services are the three things that used to talk
# to three central servers.
printf '\n%-10s %10s %10s %10s %6s\n' uid VmRSS_kB Pss_kB Priv_kB fds
for u in "$USER_A" "$USER_B" "$USER_C"; do
    srss=0; spss=0; spriv=0; sfds=0
    for m in "${MODULES[@]}"; do
        [ "$(user_for "$m")" = "$u" ] || continue
        pid="${MOD_PID[$m]}"
        read -r rss pss priv fds <<<"$(sample "$pid" "$m")"
        srss=$((srss + rss)); spss=$((spss + pss))
        spriv=$((spriv + priv)); sfds=$((sfds + fds))
    done
    printf '%-10s %10s %10s %10s %6s\n' "$u" "$srss" "$spss" "$spriv" "$sfds"
done

# Kernel-side socket cost, so the report can say what is NOT in the PSS figure.
printf '\n-- kernel socket state for this lab (not included in PSS above)\n'
ss -x -a -m 2>/dev/null | grep -c "$LAB_RUN" | sed 's/^/   AF_UNIX sockets in the lab: /' || true
awk 'NR>1 { n++ } END { print "   AF_UNIX sockets system-wide: " n }' /proc/net/unix

cat <<'NOTE'

   How to read this:
     * Pss is the column that may be summed across processes; VmRSS double
       counts every shared page (libc, the loader), so the "9 modules" VmRSS
       total overstates real cost.
     * These are module processes only.  There is no central server in this
       topology -- that is the whole point -- so there is no server RSS to
       subtract.  handoff.md 13 is explicit that three server RSS values must
       not simply be added and presented as the net saving; the honest claim is
       "N module processes, no server", and the delta needs the old system's
       own measurement.
     * Kernel socket buffers are real memory and are not in PSS.  On this
       kernel each datagram socket's send buffer defaults to 212992 B, charged
       on demand (see probes/PROBE_NOTES.md 2).
NOTE

all_stop
printf '\nmeasure_footprint: measured %s modules\n' "${#MODULES[@]}"
