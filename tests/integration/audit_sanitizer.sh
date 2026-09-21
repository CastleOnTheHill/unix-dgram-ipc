#!/usr/bin/env bash
# audit_sanitizer.sh -- scan a lab's captured module output for sanitizer
# diagnostics.
#
# Why this exists: an ASan/UBSan run can report "15/15 tests passed" while a
# module was quietly printing a heap-overflow or a leak report into its own
# captured stdout.  The suite's pass/fail counters only look at journal lines,
# so the sanitizer verdict has to be collected separately.
#
# Usage:
#   bash tests/integration/audit_sanitizer.sh [log-dir]
#   # default log-dir: ${IPC_LAB:-/opt/ipc-lab}/logs
#
# Exit status: 0 when nothing was found, 1 when a diagnostic was found.
set -uo pipefail

DIR="${1:-${IPC_LAB:-/opt/ipc-lab}/logs}"

if [ ! -d "$DIR" ]; then
    echo "audit_sanitizer: no such log directory: $DIR" >&2
    exit 2
fi

mapfile -t FILES < <(find "$DIR" -maxdepth 1 -name '*.out' -type f | sort)
if [ "${#FILES[@]}" -eq 0 ]; then
    echo "audit_sanitizer: no *.out files in $DIR" >&2
    exit 2
fi

echo "== scanning ${#FILES[@]} captured outputs in $DIR"
echo "== patterns: AddressSanitizer / LeakSanitizer / UndefinedBehaviorSanitizer"
echo "==           runtime error / SUMMARY / ERROR:"

hits=0
for f in "${FILES[@]}"; do
    n="$(grep -cE 'AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|runtime error|^==[0-9]+==ERROR|SUMMARY: ' "$f")"
    if [ "${n:-0}" -gt 0 ]; then
        echo
        echo "--- $(basename "$f") : $n diagnostic line(s)"
        grep -nE 'AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|runtime error|^==[0-9]+==ERROR|SUMMARY: ' "$f" | head -20
        hits=$((hits + n))
    fi
done

echo
if [ "$hits" -eq 0 ]; then
    echo "CLEAN: no sanitizer diagnostics in ${#FILES[@]} outputs"
    exit 0
fi
echo "DIRTY: $hits diagnostic line(s) across the captured outputs"
exit 1
