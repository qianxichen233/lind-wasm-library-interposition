#!/usr/bin/env bash
# Unit tests for run_tests.sh's own matching/classification logic (lib.sh).
# Pure bash, no compilation, no Lind execution -- fast enough to run on
# every change to the harness itself.
#
# Usage: bash tests/grate-tests/lib-interpose/selftest.sh

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib.sh"

PASS=0
FAIL=0

check() {
    local desc="$1" got="$2" want="$3"
    if [[ "$got" == "$want" ]]; then
        echo "  ok    $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $desc"
        echo "        got:  $got"
        echo "        want: $want"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== match_ordered_lines ==="

# All expected lines present, in order -> match.
out=$'a\nb\nc'
match_ordered_lines "$out" "a" "c" >/dev/null
check "in-order subsequence matches" "$?" "0"

# A candidate line embedded inside a LONGER line must not count as a match:
# "PASS compress" must not match a line that only contains "PASS compressBound".
out=$'PASS  compressBound  = 103'
match_ordered_lines "$out" "PASS  compress" >/dev/null
check "prefix-of-longer-line is not a match" "$?" "1"

# Correct lines present but in the wrong relative order -> no match (the
# scan only looks forward from the previous match's position).
out=$'b\na'
match_ordered_lines "$out" "a" "b" >/dev/null
check "out-of-order lines fail" "$?" "1"

# Missing dispatch-evidence line -> no match, and it's reported.
out=$'[Cage] PASS'
missing="$(match_ordered_lines "$out" "[Cage] PASS" "[Grate] intercepted: n=1")"
check "missing evidence line is reported" "$missing" "[Grate] intercepted: n=1"

echo ""
echo "=== decide_outcome ==="

# The original issue #14 bug: output contains PASS, but the process exited
# nonzero -- must still fail.
check "PASS text + nonzero exit = fail" "$(decide_outcome 1 0)" "fail"
check "clean exit + all lines matched = pass" "$(decide_outcome 0 0)" "pass"
check "clean exit + missing line(s) = fail" "$(decide_outcome 0 2)" "fail"

echo ""
echo "=== category_for ==="

check "timeout (exit 124)" "$(category_for "" 124)" "timeout"
check "registration failure" \
    "$(category_for "failed to register grate workers for cage 1" 1)" "registration"
check "startup failure" \
    "$(category_for "unknown import: lind::lind-longjmp has not been defined" 1)" "startup"
check "trap" "$(category_for "wasm trap: unreachable" 1)" "trap"
check "assertion" "$(category_for "[Cage] FAIL: expected 42, got 7" 1)" "assertion"
check "build" "$(category_for "COMPILE_STEP_FAILED (cage)" 1)" "build"
check "semantic fallback" "$(category_for "nothing matched any pattern" 1)" "semantic"

echo ""
echo "=== find_undeclared_dirs ==="

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT
mkdir -p "$tmpdir/declared-test" "$tmpdir/undeclared-test" "$tmpdir/full-libc" "$tmpdir/full-libm"
: > "$tmpdir/declared-test/foo_grate.c"
: > "$tmpdir/undeclared-test/bar_grate.c"
: > "$tmpdir/full-libc/baz_grate.c"
: > "$tmpdir/full-libm/qux_grate.c"

undeclared="$(find_undeclared_dirs "$tmpdir" "declared-test")"
check "a newly added test dir is flagged" "$undeclared" "undeclared-test"

undeclared_none="$(find_undeclared_dirs "$tmpdir" "declared-test" "undeclared-test")"
check "nothing flagged once declared" "$undeclared_none" ""

echo ""
echo "Results: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
