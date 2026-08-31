#!/usr/bin/env bash
# ABI-edge boundary tests for the raw-ABI-slot cap (LIND_RAW_ARGS_MAX): the
# hidden sret pointer and multi-slot (fp128) arguments have to be counted in
# post-ABI-lowering terms, not C-level argument count, or a function that's
# actually over the transport's capacity slips through as "marshal" and
# aborts the whole grate process on its first real call (see Infer.cpp's
# enforceRawArgSlotCap). Five fixtures cross the boundary from different
# angles:
#   six_ordinary          6 plain scalars, no sret            -> accepted
#   five_scalar_sret       5 scalars + hidden sret  (=6)        -> accepted
#   six_scalar_sret         6 scalars + hidden sret  (=7)        -> rejected
#   fp128_at_boundary      4 scalars + fp128 (2 slots) (=6)     -> accepted
#   fp128_over_boundary    5 scalars + fp128 (2 slots) (=7)     -> rejected
#
# Each fixture is compiled via `lind_compile --emit-marshal` (the real
# toolchain entry point -- see CLAUDE.md) and its resulting <fixture>.marshal.json
# is checked against the expected decision/slot count/warning text. For the
# "accepted" cases, gen_grate.py's is_marshalable() is also asserted True,
# confirming eligibility survives into generation, not just inference.
#
# A sixth, synthetic case feeds gen_grate.py's is_marshalable() a hand-built
# dict shaped like STALE JSON: decision:"marshal" with a real 7-entry args
# array, as a pre-slot-cap-fix marshal-infer (or a hand-edited sidecar) could
# have produced. This is gen_grate.py's OWN independent slot-count check
# (distinct from inference's), and this is the only way to test it directly:
# a freshly-inferred JSON can never actually contain a "marshal"-decision,
# >6-slot entry, since inference now catches it first.
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

for case in six_ordinary:marshal:6 five_scalar_sret:marshal:6 \
            six_scalar_sret:force_local:7 \
            fp128_at_boundary:marshal:6 fp128_over_boundary:force_local:7
do
    name="${case%%:*}"
    rest="${case#*:}"
    want_decision="${rest%%:*}"
    want_slots="${rest#*:}"

    json_path="$(infer_one "$name")" || continue
    got_decision="$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['functions'][0]['decision'])" "$json_path")"
    check "$name: decision" "$got_decision" "$want_decision"

    if [[ "$want_decision" == "marshal" ]]; then
        got_nargs="$(python3 -c "import json,sys; print(len(json.load(open(sys.argv[1]))['functions'][0].get('args', [])))" "$json_path")"
        check "$name: raw slot count (len(args))" "$got_nargs" "$want_slots"

        got_marshalable="$(python3 -c "
import json, sys
sys.path.insert(0, '$REPO_ROOT/tools/marshal-gen')
from gen_grate import is_marshalable
f = json.load(open(sys.argv[1]))['functions'][0]
print(is_marshalable(f))
" "$json_path")"
        check "$name: is_marshalable() (generated-handler eligibility)" "$got_marshalable" "True"
    else
        got_warning="$(python3 -c "
import json, sys
f = json.load(open(sys.argv[1]))['functions'][0]
print(any('needs $want_slots raw ABI slots' in w for w in f.get('warnings', [])))
" "$json_path")"
        check "$name: force_local warning cites $want_slots raw ABI slots" "$got_warning" "True"
    fi
done

echo ""
echo "=== gen_grate.py's own independent slot-count check (stale-JSON case) ==="

# A hand-built dict shaped like a "marshal"-decision, 7-arg entry -- what a
# stale sidecar (predating the inference-side fix, or hand-edited) could
# still contain. is_marshalable() must reject this itself; it cannot rely on
# inference having already done so.
stale_result="$(python3 -c "
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
check "stale 7-arg 'marshal' entry: is_marshalable() rejects it" "$stale_result" "False"

echo ""
echo "Results: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
