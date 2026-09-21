#!/usr/bin/env bash
# selftest_audit.sh -- prove audit_sanitizer.sh is not vacuous.
#
# audit_sanitizer.sh claims "CLEAN" for the real lab.  That claim is only worth
# something if the same scanner reliably reports DIAGNOSTIC for a program that
# is genuinely broken.  So: build four tiny controls with the sanitizers,
# capture their output exactly the way the lab captures a module's, and require
# the audit to give the right verdict.
#
#   control 1  heap-buffer-overflow     -> ASan must fire
#   control 2  genuine memory leak      -> LSan must fire
#   control 3  signed integer overflow  -> UBSan must fire
#   control 4  correct program          -> must stay CLEAN
#
# Implementation note, learned the hard way: the first version of these
# controls was a single translation unit, and GCC deleted the entire
# allocation/memset because the result was unused.  The controls therefore
# "passed" as clean and the self-test failed.  They are split into a helper
# translation unit now, which puts the allocation out of the optimizer's
# reach (no LTO is used), so a sanitizer that is working really does see the
# defect.
#
# Usage: bash tests/integration/selftest_audit.sh
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
AUDIT="$HERE/audit_sanitizer.sh"
SAN=(-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1)
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0

check() { # <expected-rc> <description> <dir> <program-rc>
    local want="$1" what="$2" dir="$3" prc="${4:-?}" rc out
    out="$(bash "$AUDIT" "$dir" 2>&1)"
    rc=$?
    if [ "$rc" -eq "$want" ]; then
        echo "ok     selftest.audit: $what (audit rc=$rc, program rc=$prc)"
        pass=$((pass + 1))
    else
        echo "FAIL   selftest.audit: $what (audit rc=$rc, want $want, program rc=$prc)"
        echo "$out" | sed 's/^/         | /'
        echo "         | --- captured output of the control ---"
        sed 's/^/         | /' "$dir"/*.out
        fail=$((fail + 1))
    fi
}

# ---------------------------------------------------------------- #
# helper translation unit: every defect lives behind a link-time   #
# boundary so the optimizer cannot reason it away                  #
# ---------------------------------------------------------------- #

cat >"$WORK/ctrl.h" <<'EOF'
#ifndef CTRL_H
#define CTRL_H
void *ctrl_alloc(unsigned long n);
int   ctrl_heap_overflow(unsigned long n, unsigned long extra);
int   ctrl_ub_overflow(unsigned long base);
#endif
EOF

cat >"$WORK/ctrl.c" <<'EOF'
#include <stdlib.h>
#include <string.h>

#include "ctrl.h"

void *ctrl_alloc(unsigned long n) { return malloc((size_t)n); }

/*
 * The store is volatile on purpose.  GCC's dead-store elimination otherwise
 * removes the out-of-bounds write entirely -- nothing ever reads the buffer
 * before free() -- and then the sanitizer is *correctly* silent about a defect
 * that no longer exists in the binary.  That trap made the first two versions
 * of these controls report "clean" and is exactly the kind of false comfort a
 * self-test has to rule out.
 */
int ctrl_heap_overflow(unsigned long n, unsigned long extra)
{
    char          *p = (char *)malloc((size_t)n);
    volatile char *vp;

    if (p == NULL) {
        return -1;
    }
    vp                = (volatile char *)p;
    vp[n + extra - 1] = 'x'; /* past the end of an n-byte object */
    free(p);
    return 0;
}

/*
 * `x` is returned rather than discarded.  A dead accumulator lets GCC delete
 * the whole loop, and the signed overflow it was supposed to demonstrate goes
 * with it -- which is what originally silenced UBSan here.
 */
int ctrl_ub_overflow(unsigned long base)
{
    int x = (int)base;

    for (int i = 0; i < 10; i++) {
        x += i * 3;
    }
    return x;
}
EOF

cat >"$WORK/main_overflow.c" <<'EOF'
#include "ctrl.h"
int main(void)
{
    return ctrl_heap_overflow(8, 24) != 0;
}
EOF

cat >"$WORK/main_leak.c" <<'EOF'
#include "ctrl.h"
int main(void)
{
    (void)ctrl_alloc(4096);   /* returned pointer is dropped: definitely lost */
    return 0;
}
EOF

cat >"$WORK/main_ubsan.c" <<'EOF'
#include "ctrl.h"
int main(void)
{
    volatile int r = ctrl_ub_overflow(2147483640ul);
    (void)r;
    return 0;
}
EOF

cat >"$WORK/main_clean.c" <<'EOF'
#include <stdlib.h>
#include <string.h>
#include "ctrl.h"
int main(void)
{
    char *p = (char *)ctrl_alloc(64);
    if (p == NULL) {
        return 1;
    }
    memset(p, 0, 64);
    free(p);
    return 0;
}
EOF

run_control() { # <name> <main.c> <expected-rc>
    local name="$1" main="$2" want="$3"
    if ! cc "${SAN[@]}" -I"$WORK" "$WORK/$main" "$WORK/ctrl.c" -o "$WORK/$name" \
            2>"$WORK/$name.cc.log"; then
        echo "FAIL   selftest.audit: could not build control '$name'"
        sed 's/^/         | /' "$WORK/$name.cc.log"
        fail=$((fail + 1))
        return
    fi
    rm -rf "$WORK/$name.d"; mkdir -p "$WORK/$name.d"
    # Exactly the lab's capture shape: one <module>.out holding stdout+stderr.
    "$WORK/$name" >"$WORK/$name.d/mod.out" 2>&1
    local prc=$?
    # Guard against the self-test degenerating into "ran a file that does not
    # exist, found no diagnostics, called that a pass".
    if [ "$prc" -eq 127 ]; then
        echo "FAIL   selftest.audit: control '$name' did not execute (rc=127)"
        fail=$((fail + 1))
        return
    fi
    check "$want" "$name" "$WORK/$name.d" "$prc"
}

echo "== audit_sanitizer.sh self-test (controls are known-broken programs)"
echo "== workdir: $WORK"
echo

run_control overflow main_overflow.c 1
run_control leak     main_leak.c     1
run_control ubsan    main_ubsan.c    1
run_control clean    main_clean.c    0

echo
if [ "$fail" -eq 0 ]; then
    echo "selftest_audit: PASS ($pass controls)"
    exit 0
fi
echo "selftest_audit: FAIL ($fail of $((pass + fail)) controls)"
exit 1
