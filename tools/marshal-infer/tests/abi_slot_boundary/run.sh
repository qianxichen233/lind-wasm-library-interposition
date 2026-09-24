#!/usr/bin/env bash
# ABI-edge boundary tests for the raw-ABI-slot width (LIND_RAW_ARGS_MAX): the
# hidden sret pointer and multi-slot (fp128) arguments have to be counted in
# post-ABI-lowering terms, not C-level argument count, or a function whose
# real width exceeds V1's fixed transport capacity could slip through
# gen_grate.py's own gate uncounted (see Infer.cpp's
# annotateWideRawArgSlots). Since plan-variable-width-library-calls.md's V2
# transport landed, marshal-infer no longer force_locals a function purely
# for width -- it stays "marshal" (annotated with its real slot count), and
# it's gen_grate.py's OWN independent width gate (unmarshalable_reason's
# max_args) that decides V1-eligibility specifically; gen_v2_adapter.py
# (max_args=None) has no such cap. Five fixtures cross the V1 boundary from
# different angles -- ALL decide "marshal" at the inference level now, only
# their V1-generated-handler eligibility differs:
#   six_ordinary          6 plain scalars, no sret            -> V1-eligible
#   five_scalar_sret       5 scalars + hidden sret  (=6)        -> V1-eligible
#   six_scalar_sret         6 scalars + hidden sret  (=7)        -> V2-only
#   fp128_at_boundary      4 scalars + fp128 (2 slots) (=6)     -> V1-eligible
#   fp128_over_boundary    5 scalars + fp128 (2 slots) (=7)     -> V2-only
#
# Each fixture is compiled via `lind_compile --emit-marshal` (the real
# toolchain entry point -- see CLAUDE.md) and its resulting <fixture>.marshal.json
# is checked against the expected slot count, gen_grate.py's is_marshalable()
# (V1, default max_args), and unmarshalable_reason(max_args=None) (V2) --
# confirming eligibility is decided at the RIGHT layer for each transport,
# not just that inference alone got the decision right.
#
# A sixth, synthetic case feeds gen_grate.py's is_marshalable() a hand-built
# dict, exercising its own independent slot-count gate directly (distinct
# from inference's own annotation) without depending on the compiler
# pipeline for a case real inference already covers via six_scalar_sret above.
#
# Usage: tools/marshal-infer/tests/abi_slot_boundary/run.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
LIND_COMPILE="$REPO_ROOT/scripts/lind_compile"

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

infer_one() {
    # Compiles $1.c (from this dir) via the real toolchain entry point and
    # prints the resulting <name>.marshal.json's path, or nothing on failure.
    local name="$1"
    cp "$SCRIPT_DIR/$name.c" "$WORK/$name.c"
    if ! ( cd "$WORK" && "$LIND_COMPILE" --emit-marshal "$name.c" ) >"$WORK/$name.compile.log" 2>&1; then
        echo "  FAIL  $name: lind_compile --emit-marshal failed"
        cat "$WORK/$name.compile.log"
        FAIL=$((FAIL + 1))
        return 1
    fi
    echo "$WORK/$name.marshal.json"
}

echo "=== ABI-edge raw-arg-slot boundary tests (real inference) ==="

for case in six_ordinary:6:True five_scalar_sret:6:True \
            six_scalar_sret:7:False \
            fp128_at_boundary:6:True fp128_over_boundary:7:False
do
    name="${case%%:*}"
    rest="${case#*:}"
    want_slots="${rest%%:*}"
    want_v1_eligible="${rest#*:}"

    json_path="$(infer_one "$name")" || continue
    got_decision="$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['functions'][0]['decision'])" "$json_path")"
    check "$name: decision (marshal regardless of width)" "$got_decision" "marshal"

    got_nargs="$(python3 -c "import json,sys; print(len(json.load(open(sys.argv[1]))['functions'][0].get('args', [])))" "$json_path")"
    check "$name: raw slot count (len(args))" "$got_nargs" "$want_slots"

    got_v1="$(python3 -c "
import json, sys
sys.path.insert(0, '$REPO_ROOT/tools/marshal-gen')
from gen_grate import is_marshalable
f = json.load(open(sys.argv[1]))['functions'][0]
print(is_marshalable(f))
" "$json_path")"
    check "$name: is_marshalable() (V1 generated-handler eligibility)" "$got_v1" "$want_v1_eligible"

    got_v2="$(python3 -c "
import json, sys
sys.path.insert(0, '$REPO_ROOT/tools/marshal-gen')
from gen_grate import unmarshalable_reason
f = json.load(open(sys.argv[1]))['functions'][0]
print(unmarshalable_reason(f, max_args=None) is None)
" "$json_path")"
    check "$name: V2-eligible (unmarshalable_reason(max_args=None) is None)" "$got_v2" "True"

    if [[ "$want_v1_eligible" == "False" ]]; then
        got_warning="$(python3 -c "
import json, sys
f = json.load(open(sys.argv[1]))['functions'][0]
print(any('needs $want_slots raw ABI slots' in w for w in f.get('warnings', [])))
" "$json_path")"
        check "$name: warning cites $want_slots raw ABI slots" "$got_warning" "True"
    fi
done

echo ""
echo "=== gen_grate.py's own independent slot-count check (V1-only, synthetic case) ==="

# A hand-built dict shaped like a "marshal"-decision, 7-arg entry, exercising
# is_marshalable()'s own independent width gate directly, without depending
# on the compiler pipeline. It must reject this for V1 (default max_args)
# but accept it for V2 (max_args=None), matching six_scalar_sret's real
# example above.
stale_v1="$(python3 -c "
import sys
sys.path.insert(0, '$REPO_ROOT/tools/marshal-gen')
from gen_grate import is_marshalable
stale_fn = {
    'name': 'stale_wide_fn',
    'decision': 'marshal',
    'ret': {'kind': 'scalar'},
    'args': [{'kind': 'scalar', 'type': 'int', 'size': 4}] * 7,
}
print(is_marshalable(stale_fn))
")"
check "7-arg 'marshal' entry: is_marshalable() (V1) rejects it" "$stale_v1" "False"

stale_v2="$(python3 -c "
import sys
sys.path.insert(0, '$REPO_ROOT/tools/marshal-gen')
from gen_grate import unmarshalable_reason
stale_fn = {
    'name': 'stale_wide_fn',
    'decision': 'marshal',
    'ret': {'kind': 'scalar'},
    'args': [{'kind': 'scalar', 'type': 'int', 'size': 4}] * 7,
}
print(unmarshalable_reason(stale_fn, max_args=None) is None)
")"
check "7-arg 'marshal' entry: unmarshalable_reason(max_args=None) (V2) accepts it" "$stale_v2" "True"

echo ""
echo "Results: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
