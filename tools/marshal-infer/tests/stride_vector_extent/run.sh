#!/usr/bin/env bash
# Regression tests for StrideVector extent-operand inference (issue #26
# review): value-vs-pointee-vs-constant provenance, canonical-relationship
# proof for length/stride, the escape-based fail-closed policy, and
# cross-module callee resolution (direct-body use, no internal-name
# aliasing, ambiguity detection). Each fixture is compiled via the real
# toolchain entry points (`lind_compile --emit-llvm` / `--emit-marshal` --
# see CLAUDE.md) and its resulting JSON is checked against the expected
# decision, extent-operand shape, and/or warning text. Later sections run a
# real "marshal"-decision record through gen_grate.py itself, asserting the
# generated C source, not a hand-built LIND_SIZE_STRIDE_VECTOR initializer.
#
# Usage: tools/marshal-infer/tests/stride_vector_extent/run.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
LIND_COMPILE="$REPO_ROOT/scripts/lind_compile"
MARSHAL_INFER="$REPO_ROOT/tools/marshal-infer/build/marshal-infer"

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

pyjq() {
    # pyjq <json-file> <python-expr-over-f>  -- f is the loaded JSON dict.
    python3 -c "
import json, sys
f = json.load(open(sys.argv[1]))
print($2)
" "$1"
}

# Compile $1.c (from this dir) via the real toolchain entry point and print
# the resulting <name>.marshal.json's path, or nothing on failure.
infer_one() {
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

# Compile every "$@" (bare names, no .c) via `lind_compile --emit-llvm`, then
# run the LOCALLY BUILT marshal-infer binary directly over all the resulting
# .bc files together -- the multi-module one-hop-delegation / cross-module
# ambiguity capabilities under test here have no single-file entry point
# (lind_compile --emit-marshal compiles exactly one .c to one .bc; see its
# do_emit_marshal). Prints the resulting JSON's path, module-named "combo".
infer_multi() {
    local bcs=()
    for name in "$@"; do
        cp "$SCRIPT_DIR/$name.c" "$WORK/$name.c" 2>/dev/null || \
            cp "$SCRIPT_DIR/ambiguous/$name.c" "$WORK/$name.c"
        if ! ( cd "$WORK" && "$LIND_COMPILE" --emit-llvm "$name.c" ) >"$WORK/$name.compile.log" 2>&1; then
            echo "  FAIL  $name: lind_compile --emit-llvm failed"
            cat "$WORK/$name.compile.log"
            FAIL=$((FAIL + 1))
            return 1
        fi
        bcs+=("$WORK/$name.bc")
    done
    local json="$WORK/combo_$1.marshal.json"
    "$MARSHAL_INFER" --json -o "$json" --module combo "${bcs[@]}" >"$WORK/infer.log" 2>&1
    echo "$json"
}

first_arg() {
    # $1=json path, $2=function name, $3=arg index -> that arg's dict, as JSON.
    pyjq "$1" "json.dumps([fn for fn in f['functions'] if fn['name']=='$2'][0]['args'][$3])"
}

echo "=== direct CBLAS-style: length/stride passed by value ==="
json="$(infer_one cblas_direct)" && {
    check "decision" "$(pyjq "$json" "f['functions'][0]['decision']")" "marshal"
    x_arg="$(first_arg "$json" vec_axpy_direct 2)"
    echo "$x_arg" | python3 -c "
import json, sys
a = json.load(sys.stdin)
assert a['size_kind'] == 'stride_vector', a
assert a['size_operand'] == {'arg_index': 0, 'source': 'value'}, a['size_operand']
assert a['stride_operand'] == {'arg_index': 3, 'source': 'value'}, a['stride_operand']
print('ok')
" && { echo "  ok    x: size_operand/stride_operand both source=value"; PASS=$((PASS+1)); } \
  || { echo "  FAIL  x: size_operand/stride_operand shape"; echo "$x_arg"; FAIL=$((FAIL+1)); }
}

echo ""
echo "=== Fortran BLAS-style: length/stride passed by reference ==="
json="$(infer_one fortran_style)" && {
    check "decision" "$(pyjq "$json" "f['functions'][0]['decision']")" "marshal"
    x_arg="$(first_arg "$json" vec_axpy_fortran 2)"
    echo "$x_arg" | python3 -c "
import json, sys
a = json.load(sys.stdin)
assert a['size_kind'] == 'stride_vector', a
assert a['size_operand'] == {'arg_index': 0, 'source': 'pointee_i32'}, a['size_operand']
assert a['stride_operand'] == {'arg_index': 3, 'source': 'pointee_i32'}, a['stride_operand']
print('ok')
" && { echo "  ok    X: size_operand/stride_operand both source=pointee_i32"; PASS=$((PASS+1)); } \
  || { echo "  FAIL  X: size_operand/stride_operand shape"; echo "$x_arg"; FAIL=$((FAIL+1)); }
}

echo ""
echo "=== wrapper + worker in separate modules (one-hop delegation) ==="
json="$(infer_multi wrapper worker)" && {
    check "wrapper_axpy: decision" \
        "$(pyjq "$json" "[fn for fn in f['functions'] if fn['name']=='wrapper_axpy'][0]['decision']")" \
        "marshal"
    x_arg="$(first_arg "$json" wrapper_axpy 2)"
    echo "$x_arg" | python3 -c "
import json, sys
a = json.load(sys.stdin)
assert a['size_kind'] == 'stride_vector', a
assert a['size_operand']['arg_index'] == 0
assert a['stride_operand']['arg_index'] == 3
print('ok')
" && { echo "  ok    wrapper_axpy: x resolved via delegation to worker_axpy"; PASS=$((PASS+1)); } \
  || { echo "  FAIL  wrapper_axpy: x not resolved via delegation"; echo "$x_arg"; FAIL=$((FAIL+1)); }
}

echo ""
echo "=== inclusive bound (i<=n): element count is n+1, not n -> reject ==="
json="$(infer_one inclusive_bound)" && {
    check "decision" "$(pyjq "$json" "f['functions'][0]['decision']")" "force_local"
    check "no stride_vector emitted anywhere" \
        "$(pyjq "$json" "any(a.get('size_kind')=='stride_vector' for a in f['functions'][0].get('args', []))")" \
        "False"
}

echo ""
echo "=== address IV starts nonzero: base pointer never accessed -> reject ==="
json="$(infer_one nonzero_start)" && {
    check "decision" "$(pyjq "$json" "f['functions'][0]['decision']")" "force_local"
    check "no stride_vector emitted anywhere" \
        "$(pyjq "$json" "any(a.get('size_kind')=='stride_vector' for a in f['functions'][0].get('args', []))")" \
        "False"
}

echo ""
echo "=== missing callee: nothing to delegate into -> fail closed ==="
json="$(infer_one missing_callee)" && {
    check "decision" "$(pyjq "$json" "f['functions'][0]['decision']")" "force_local"
    check "warning cites unanalyzable escape" \
        "$(pyjq "$json" "any('unanalyzable callee' in w for w in f['functions'][0]['warnings'])")" \
        "True"
}

echo ""
echo "=== indirect callee: cannot statically follow -> fail closed ==="
json="$(infer_one indirect_callee)" && {
    check "decision" "$(pyjq "$json" "f['functions'][0]['decision']")" "force_local"
    check "warning cites unanalyzable escape" \
        "$(pyjq "$json" "any('unanalyzable callee' in w for w in f['functions'][0]['warnings'])")" \
        "True"
}

echo ""
echo "=== ambiguous cross-module definitions -> refuse, not first-picked ==="
json="$(infer_multi wrapper_ambig workerA workerB)" && {
    check "wrapper_ambig: decision" \
        "$(pyjq "$json" "[fn for fn in f['functions'] if fn['name']=='wrapper_ambig'][0]['decision']")" \
        "force_local"
}

echo ""
echo "=== noncanonical 2*stride: must reject, never silently halve the extent ==="
json="$(infer_one noncanonical_scale)" && {
    check "decision" "$(pyjq "$json" "f['functions'][0]['decision']")" "force_local"
    check "no stride_vector emitted anywhere" \
        "$(pyjq "$json" "any(a.get('size_kind')=='stride_vector' for a in f['functions'][0].get('args', []))")" \
        "False"
}

echo ""
echo "=== constant-sourced stride: no caller argument behind the operand ==="
# See constant_stride.c's own top comment for the full rationale (including
# why sum_pointerwalk_cmp force_locals for an unrelated, pre-existing
# reason). $2/$3: expected size_operand/stride_operand const_value ("" to
# skip if not constant-sourced).
json="$(infer_one constant_stride)" && {
    cs_check() {
        local fn="$1" wantDecision="$2" wantStrideConst="${3:-}"
        local decision
        decision="$(pyjq "$json" "([x for x in f['functions'] if x['name']=='$fn'] or [{'decision':'MISSING'}])[0]['decision']")"
        check "constant stride: $fn decision" "$decision" "$wantDecision"
        if [[ -n "$wantStrideConst" ]]; then
            local strideOp
            strideOp="$(pyjq "$json" "__import__('json').dumps(next((a.get('stride_operand') for x in f['functions'] if x['name']=='$fn' for a in x.get('args',[]) if a.get('size_kind')=='stride_vector'), None))")"
            check "constant stride: $fn stride_operand" "$strideOp" \
                "{\"arg_index\": -1, \"source\": \"constant\", \"const_value\": $wantStrideConst}"
        fi
    }
    cs_check sum_indexed               marshal      1
    cs_check sum_pointerwalk           marshal      1
    cs_check sum_pointerwalk_cmp       force_local
    cs_check sum_every_other           marshal      2
    cs_check sum_fortran_len           marshal      1
    cs_check sum_skip_first_indexed    force_local
    cs_check sum_skip_first_pointerwalk force_local
    check "constant stride: no stride_vector emitted for either skip_first variant" \
        "$(pyjq "$json" "any(a.get('size_kind')=='stride_vector' for x in f['functions'] if 'skip_first' in x['name'] for a in x.get('args', []))")" \
        "False"
}

echo ""
echo "=== peeled-first-iteration recovery (max/min family) ==="
# See peeled_prefix.c's own top comment for the full rationale. $2:
# expected decision. $3/$4: expected size_operand/stride_operand arg_index
# ("" to skip the shape check for a force_local case).
json="$(infer_one peeled_prefix)" && {
    pp_check() {
        local fn="$1" wantDecision="$2" wantSizeIdx="${3:-}" wantStrideIdx="${4:-}"
        local decision
        decision="$(pyjq "$json" "([x for x in f['functions'] if x['name']=='$fn'] or [{'decision':'MISSING'}])[0]['decision']")"
        check "peeled: $fn decision" "$decision" "$wantDecision"
        if [[ -n "$wantSizeIdx" ]]; then
            local sizeOp strideOp conf
            sizeOp="$(pyjq "$json" "__import__('json').dumps(next((a.get('size_operand') for x in f['functions'] if x['name']=='$fn' for a in x.get('args',[]) if a.get('size_kind')=='stride_vector'), None))")"
            check "peeled: $fn size_operand.arg_index" \
                "$(echo "$sizeOp" | python3 -c "import json,sys; print(json.load(sys.stdin)['arg_index'])")" \
                "$wantSizeIdx"
            strideOp="$(pyjq "$json" "__import__('json').dumps(next((a.get('stride_operand') for x in f['functions'] if x['name']=='$fn' for a in x.get('args',[]) if a.get('size_kind')=='stride_vector'), None))")"
            check "peeled: $fn stride_operand.arg_index" \
                "$(echo "$strideOp" | python3 -c "import json,sys; print(json.load(sys.stdin)['arg_index'])")" \
                "$wantStrideIdx"
            conf="$(pyjq "$json" "next((a.get('confidence') for x in f['functions'] if x['name']=='$fn' for a in x.get('args',[]) if a.get('size_kind')=='stride_vector'), None)")"
            check "peeled: $fn confidence" "$conf" "proven"
        fi
    }
    pp_check peeled_max                    marshal 0 2
    pp_check peeled_min                    marshal 0 2
    pp_check peeled_max_fortran            marshal 0 2
    pp_check peeled_max_wrapper            marshal 0 2
    pp_check neg_missing_peel              force_local
    pp_check neg_peel_wrong_offset         force_local
    pp_check neg_peel_wrong_start          force_local
    pp_check neg_peel_inclusive_bound      force_local
    pp_check neg_peel_wrong_pointer        force_local
    pp_check neg_peel_wrong_stride_var     force_local
    pp_check neg_peel_extra_access         force_local
    pp_check neg_peel_conditional          force_local
    pp_check neg_peel_unresolvable_stride  force_local
    pp_check neg_peel_escapes              force_local
    pp_check neg_peel_no_guard             force_local
}

echo ""
echo "=== malformed constant-operand metadata: gen_grate.py must reject, not repair ==="
python3 -c "
import sys
sys.path.insert(0, '$REPO_ROOT/tools/marshal-gen')
from gen_grate import _valid_extent_operand, EXTENT_CONST_VALUE_MAX

cases = [
    ('missing const_value',        {'source': 'constant'}),
    ('const_value zero',           {'source': 'constant', 'const_value': 0}),
    ('const_value negative',       {'source': 'constant', 'const_value': -1}),
    ('const_value non-integer',    {'source': 'constant', 'const_value': 1.5}),
    ('const_value bool',           {'source': 'constant', 'const_value': True}),
    ('const_value too large',      {'source': 'constant', 'const_value': EXTENT_CONST_VALUE_MAX + 1}),
    ('arg_index alongside constant', {'source': 'constant', 'const_value': 1, 'arg_index': 0}),
]
bad = [desc for desc, o in cases if _valid_extent_operand(o, nargs=4)]
if bad:
    print('FAIL: accepted malformed operand(s): ' + ', '.join(bad))
    sys.exit(1)
good = {'source': 'constant', 'const_value': 1}
if not _valid_extent_operand(good, nargs=4):
    print('FAIL: rejected a well-formed constant operand')
    sys.exit(1)
print('ok')
" && { echo "  ok    gen_grate.py: rejects every malformed constant operand, accepts a well-formed one"; PASS=$((PASS+1)); } \
  || { echo "  FAIL  gen_grate.py: malformed constant-operand validation"; FAIL=$((FAIL+1)); }

# arg_spec_body's own operand() builder is the SECOND, defense-in-depth gate
# (see its comment) -- confirm it independently raises, not just
# _valid_extent_operand (a caller could reach arg_spec_body directly without
# going through is_marshalable() first).
python3 -c "
import sys
sys.path.insert(0, '$REPO_ROOT/tools/marshal-gen')
from gen_grate import Emitter

bad_fn = {
    'name': 'bad', 'decision': 'marshal',
    'args': [{'kind': 'ptr', 'dir': 'in', 'size_kind': 'stride_vector',
              'size_operand': {'arg_index': 0, 'source': 'value'},
              'stride_operand': {'source': 'constant'},  # missing const_value
              'const_size': 8}],
    'ret': {'kind': 'void'},
}
e = Emitter()
try:
    e.emit_function_spec(bad_fn)
    print('FAIL: arg_spec_body accepted a missing const_value')
    sys.exit(1)
except ValueError as e:
    print('ok')
" && { echo "  ok    gen_grate.py: arg_spec_body raises ValueError on malformed constant operand"; PASS=$((PASS+1)); } \
  || { echo "  FAIL  gen_grate.py: arg_spec_body did not raise on malformed constant operand"; FAIL=$((FAIL+1)); }

echo ""
echo "=== genuine single-scalar out-param: unaffected by the escape gate ==="
json="$(infer_one scalar_out)" && {
    check "decision" "$(pyjq "$json" "f['functions'][0]['decision']")" "marshal"
    exp_arg="$(first_arg "$json" halve_and_report 1)"
    check "exp_out: size_kind" "$(echo "$exp_arg" | python3 -c "import json,sys; print(json.load(sys.stdin)['size_kind'])")" "const"
}

echo ""
echo "=== inference-to-generator: gen_grate.py emits from REAL inferred JSON ==="
json="$(infer_one cblas_direct)" && {
    generated="$(python3 -c "
import json, sys
sys.path.insert(0, '$REPO_ROOT/tools/marshal-gen')
from gen_grate import Emitter
f = json.load(open(sys.argv[1]))['functions'][0]
e = Emitter()
name, body = e.emit_function_spec(f)
print(body)
" "$json")"
    check "generated spec declares size_operand" \
        "$(echo "$generated" | grep -c 'size_operand = { .arg_index = 0, .source = LIND_EXTENT_VALUE }')" \
        "2"
    check "generated spec declares stride_operand" \
        "$(echo "$generated" | grep -c '.stride_operand = { .arg_index = 3, .source = LIND_EXTENT_VALUE }')" \
        "1"
}

json_cs="$(infer_one constant_stride)" && {
    generated_cs="$(python3 -c "
import json, sys
sys.path.insert(0, '$REPO_ROOT/tools/marshal-gen')
from gen_grate import Emitter
f = json.load(open(sys.argv[1]))
fn = [x for x in f['functions'] if x['name']=='sum_pointerwalk'][0]
e = Emitter()
name, body = e.emit_function_spec(fn)
print(body)
" "$json_cs")"
    check "generated spec declares constant-sourced stride_operand" \
        "$(echo "$generated_cs" | grep -c '.stride_operand = { .source = LIND_EXTENT_CONSTANT, .const_value = 1 }')" \
        "1"
}

echo ""
echo "Results: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
