#!/usr/bin/env bash
# Negative-test suite for tools/marshal-gen/gen_v2_adapter.py's fail-closed
# generation gate: a partially-generated V2 grate that silently omits a
# symbol is a worse failure than refusing to generate at all (the omission
# would otherwise surface only as a missing-import/uninterposed-fallback bug
# much later, at run time, not at generation time). These tests exercise the
# GENERATOR's own input-validation logic directly against hand-built JSON
# fixtures -- the same use of synthetic JSON as abi_slot_boundary/run.sh's
# "stale JSON" case and config/run.sh's schema-validation tests, not a
# substitute for real-toolchain ABI-shape coverage (see
# tools/marshal-infer/tests/config/all_access_coverage.c and friends for
# that; this suite is about the GENERATOR's error handling, not inference).
#
# Usage: tools/marshal-infer/tests/gen_v2_adapter/run.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
GEN="$REPO_ROOT/tools/marshal-gen/gen_v2_adapter.py"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

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

# A minimal, fully V2-marshalable scalar function -- no pointers, nothing
# for is_v2_marshalable() to ever reject.
clean_fn() {
    local name="$1"
    echo "{\"name\": \"$name\", \"decision\": \"marshal\", \"ret\": {\"kind\": \"void\"}, \"args\": []}"
}

# A decision:"marshal" record with an unsupported V2 shape (ptr_to_static
# return -- hands the app a grate-cage pointer it can't dereference; the
# same shape openblas_get_config/openblas_get_corename hit for real).
unsupported_fn() {
    local name="$1"
    echo "{\"name\": \"$name\", \"decision\": \"marshal\", \"ret\": {\"kind\": \"ptr_to_static\", \"copyout_bytes\": 0}, \"args\": []}"
}

# A force_local record -- never eligible for ANY generator, V1 or V2.
rejected_fn() {
    local name="$1"
    echo "{\"name\": \"$name\", \"decision\": \"force_local\", \"warnings\": [\"synthetic rejection\"]}"
}

write_json() {
    local path="$1"; shift
    printf '{"functions": [%s]}' "$(IFS=,; echo "$*")" > "$path"
}

echo "=== missing symbol: --only names a symbol absent from the JSON entirely ==="
write_json "$WORK/clean.json" "$(clean_fn ok_fn)"
python3 "$GEN" "$WORK/clean.json" --lib-name t --only nonexistent_fn --out "$WORK/out1.c" \
    >"$WORK/out1.log" 2>&1
rc=$?
check "missing symbol: exit code" "$rc" "1"
check "missing symbol: error names the symbol" \
    "$(grep -c "nonexistent_fn: not present in this JSON at all" "$WORK/out1.log")" "1"
check "missing symbol: no output file written" \
    "$([[ -s "$WORK/out1.c" ]] && echo present || echo absent)" "absent"

echo ""
echo "=== unsupported record: --only names a marshal-decision record V2 can't emit ==="
write_json "$WORK/unsupported.json" "$(clean_fn ok_fn)" "$(unsupported_fn bad_fn)"
python3 "$GEN" "$WORK/unsupported.json" --lib-name t --only bad_fn --out "$WORK/out2.c" \
    >"$WORK/out2.log" 2>&1
rc=$?
check "unsupported record: exit code" "$rc" "1"
check "unsupported record: error names the reason" \
    "$(grep -c "bad_fn: unsupported return kind 'ptr_to_static'" "$WORK/out2.log")" "1"

echo ""
echo "=== decision not marshal: --only names a force_local record ==="
write_json "$WORK/rejected.json" "$(clean_fn ok_fn)" "$(rejected_fn rej_fn)"
python3 "$GEN" "$WORK/rejected.json" --lib-name t --only rej_fn --out "$WORK/out3.c" \
    >"$WORK/out3.log" 2>&1
rc=$?
check "decision not marshal: exit code" "$rc" "1"
check "decision not marshal: error names the actual decision" \
    "$(grep -c "rej_fn: decision='force_local', not \"marshal\"" "$WORK/out3.log")" "1"

echo ""
echo "=== empty output: whole-file JSON with zero marshal-decision functions ==="
write_json "$WORK/nothing.json" "$(rejected_fn only_rejected)"
python3 "$GEN" "$WORK/nothing.json" --lib-name t --out "$WORK/out4.c" \
    >"$WORK/out4.log" 2>&1
rc=$?
check "empty output: exit code" "$rc" "1"
check "empty output: error message" \
    "$(grep -c "ERROR: zero handlers would be emitted" "$WORK/out4.log")" "1"

echo ""
echo "=== whole-file mode: a dropped (unsupported) record, no --allow-partial ==="
write_json "$WORK/mixed.json" "$(clean_fn good_fn)" "$(unsupported_fn bad_fn)"
python3 "$GEN" "$WORK/mixed.json" --lib-name t --out "$WORK/out5.c" \
    >"$WORK/out5.log" 2>&1
rc=$?
check "whole-file, dropped record: exit code" "$rc" "1"
check "whole-file, dropped record: error lists the omitted symbol" \
    "$(grep -c "bad_fn: unsupported return kind 'ptr_to_static'" "$WORK/out5.log")" "1"
check "whole-file, dropped record: no output file written" \
    "$([[ -s "$WORK/out5.c" ]] && echo present || echo absent)" "absent"

echo ""
echo "=== --allow-partial: whole-file mode generates the rest, lists the gap ==="
python3 "$GEN" "$WORK/mixed.json" --lib-name t --allow-partial --out "$WORK/out6.c" \
    >"$WORK/out6.log" 2>&1
rc=$?
check "--allow-partial (whole-file): exit code" "$rc" "0"
check "--allow-partial (whole-file): generates the marshalable function" \
    "$(grep -c "__lind_v2_adapter_good_fn" "$WORK/out6.c")" "2"
check "--allow-partial (whole-file): does NOT generate the dropped one" \
    "$(grep -c "__lind_v2_adapter_bad_fn" "$WORK/out6.c")" "0"
check "--allow-partial (whole-file): still lists the omitted symbol" \
    "$(grep -c "bad_fn: unsupported return kind 'ptr_to_static'" "$WORK/out6.log")" "1"

echo ""
echo "=== --allow-partial: --only mode omits the unavailable requested symbol ==="
write_json "$WORK/mixed2.json" "$(clean_fn good_fn)" "$(unsupported_fn bad_fn)"
python3 "$GEN" "$WORK/mixed2.json" --lib-name t --only good_fn,bad_fn,missing_fn --allow-partial \
    --out "$WORK/out7.c" >"$WORK/out7.log" 2>&1
rc=$?
check "--allow-partial (--only): exit code" "$rc" "0"
check "--allow-partial (--only): generates the available symbol" \
    "$(grep -c "__lind_v2_adapter_good_fn" "$WORK/out7.c")" "2"
check "--allow-partial (--only): lists both omitted symbols" \
    "$(grep -c "bad_fn: unsupported return kind 'ptr_to_static'" "$WORK/out7.log")$(grep -c "missing_fn: not present in this JSON at all" "$WORK/out7.log")" \
    "11"

echo ""
echo "=== positive baseline: a clean multi-function file generates cleanly ==="
write_json "$WORK/clean_multi.json" "$(clean_fn fn_a)" "$(clean_fn fn_b)" "$(clean_fn fn_c)"
python3 "$GEN" "$WORK/clean_multi.json" --lib-name t --out "$WORK/out8.c" \
    >"$WORK/out8.log" 2>&1
rc=$?
check "positive baseline: exit code" "$rc" "0"
for fn in fn_a fn_b fn_c; do
    check "positive baseline: generates $fn" \
        "$(grep -c "__lind_v2_adapter_$fn" "$WORK/out8.c")" "2"
done

echo ""
echo "Results: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
