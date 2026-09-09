#!/usr/bin/env bash
# Regression tests for the versioned config system (issue #27): schema
# validation and failure modes, backward compatibility with no --config,
# analysis-knob threading (max_type_depth, max_delegation_hops), contract
# application + provenance recording, the "contract never applied" warning,
# and coverage-threshold enforcement. Fixtures are compiled via the real
# toolchain entry points (`lind_compile --emit-llvm` / `--emit-marshal` --
# see CLAUDE.md); C fixtures shared with the stride_vector_extent suite are
# referenced there rather than duplicated.
#
# Usage: tools/marshal-infer/tests/config/run.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
SVE_DIR="$(cd "$SCRIPT_DIR/../stride_vector_extent" && pwd)"
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
    python3 -c "
import json, sys
f = json.load(open(sys.argv[1]))
print($2)
" "$1"
}

# Compile $2.c from $1 (a directory) via the real toolchain entry point and
# print the resulting <name>.marshal.json's path (with optional --config).
infer_one() {
    local srcdir="$1" name="$2" configfile="${3:-}"
    cp "$srcdir/$name.c" "$WORK/$name.c"
    if [[ -n "$configfile" ]]; then
        if ! ( cd "$WORK" && "$LIND_COMPILE" --emit-llvm "$name.c" ) >"$WORK/$name.compile.log" 2>&1; then
            echo "  FAIL  $name: lind_compile --emit-llvm failed"; cat "$WORK/$name.compile.log"
            FAIL=$((FAIL + 1)); return 1
        fi
        local json="$WORK/$name.marshal.json"
        "$MARSHAL_INFER" --json --config "$configfile" -o "$json" "$WORK/$name.bc" 2>"$WORK/$name.infer.log"
        local rc=$?
        echo "$rc" > "$WORK/$name.exitcode"
        echo "$json"
    else
        if ! ( cd "$WORK" && "$LIND_COMPILE" --emit-marshal "$name.c" ) >"$WORK/$name.compile.log" 2>&1; then
            echo "  FAIL  $name: lind_compile --emit-marshal failed"; cat "$WORK/$name.compile.log"
            FAIL=$((FAIL + 1)); return 1
        fi
        echo "$WORK/$name.marshal.json"
    fi
}

# Attempt to load a config file and print marshal-infer's exit code + stderr,
# without needing any real .bc input (schema validation happens before any
# module is even read, but the CLI still requires >=1 positional input).
try_config() {
    local configfile="$1"
    cp "$SVE_DIR/scalar_out.c" "$WORK/probe.c"
    ( cd "$WORK" && "$LIND_COMPILE" --emit-llvm probe.c ) >/dev/null 2>&1
    "$MARSHAL_INFER" --config "$configfile" "$WORK/probe.bc" >"$WORK/probe.out" 2>"$WORK/probe.err"
    echo "$?"
}

echo "=== schema validation: failure modes ==="

echo '{"config_version": 1, "bogus_top_level_key": 1}' > "$WORK/bad_unknown_key.json"
rc="$(try_config "$WORK/bad_unknown_key.json")"
check "unknown top-level key: exit code" "$rc" "1"
check "unknown top-level key: error message" \
    "$(grep -c "unknown key 'bogus_top_level_key'" "$WORK/probe.err")" "1"

echo '{"config_version": 2}' > "$WORK/bad_version.json"
rc="$(try_config "$WORK/bad_version.json")"
check "unsupported config_version: exit code" "$rc" "1"
check "unsupported config_version: error message" \
    "$(grep -c "unsupported config_version 2" "$WORK/probe.err")" "1"

echo '{"config_version": 1, "analysis": {"max_delegation_hops": 5}}' > "$WORK/bad_hops.json"
rc="$(try_config "$WORK/bad_hops.json")"
check "unimplemented max_delegation_hops: exit code" "$rc" "1"

echo '{"config_version": 1, "analysis": {"max_type_depth": 0}}' > "$WORK/bad_depth.json"
rc="$(try_config "$WORK/bad_depth.json")"
check "out-of-range max_type_depth: exit code" "$rc" "1"

echo '{"config_version": 1, "coverage": {"enabled": true}}' > "$WORK/bad_vacuous_coverage.json"
rc="$(try_config "$WORK/bad_vacuous_coverage.json")"
check "vacuous coverage.enabled (no threshold set): exit code" "$rc" "1"

cat > "$WORK/bad_contract_source.json" <<'EOF'
{"config_version": 1, "contracts": {"f": {"0": {
  "size_operand": {"arg_index": 0, "source": "bogus"},
  "stride_operand": {"arg_index": 1, "source": "value"},
  "const_size": 8
}}}}
EOF
rc="$(try_config "$WORK/bad_contract_source.json")"
check "unknown contract source: exit code" "$rc" "1"

cat > "$WORK/bad_contract_missing_operand.json" <<'EOF'
{"config_version": 1, "contracts": {"f": {"0": {
  "stride_operand": {"arg_index": 1, "source": "value"},
  "const_size": 8
}}}}
EOF
rc="$(try_config "$WORK/bad_contract_missing_operand.json")"
check "missing size_operand: exit code" "$rc" "1"

cat > "$WORK/bad_contract_argkey.json" <<'EOF'
{"config_version": 1, "contracts": {"f": {"not_a_number": {
  "size_operand": {"arg_index": 0, "source": "value"},
  "stride_operand": {"arg_index": 1, "source": "value"},
  "const_size": 8
}}}}
EOF
rc="$(try_config "$WORK/bad_contract_argkey.json")"
check "non-integer contract argument key: exit code" "$rc" "1"

echo '{"config_version": 1, "analysis": {"heuristics": ["guard_based_length"]}}' > "$WORK/bad_heuristics_no_policy.json"
rc="$(try_config "$WORK/bad_heuristics_no_policy.json")"
check "heuristics set without policy=relaxed: exit code" "$rc" "1"

echo '{"config_version": 1, "analysis": {"policy": "relaxed"}}' > "$WORK/bad_relaxed_no_heuristics.json"
rc="$(try_config "$WORK/bad_relaxed_no_heuristics.json")"
check "policy=relaxed with no heuristics: exit code" "$rc" "1"

echo '{"config_version": 1, "analysis": {"policy": "relaxed", "heuristics": ["not_a_real_heuristic"]}}' > "$WORK/bad_unknown_heuristic.json"
rc="$(try_config "$WORK/bad_unknown_heuristic.json")"
check "unknown heuristic name: exit code" "$rc" "1"

echo '{"config_version": 1, "analysis": {"policy": "lenient"}}' > "$WORK/bad_policy_value.json"
rc="$(try_config "$WORK/bad_policy_value.json")"
check "invalid policy value: exit code" "$rc" "1"

echo "--- present-but-wrong-typed fields must be REJECTED, not silently defaulted ---"
echo '{"config_version": 1, "analysis": {"max_type_depth": "six"}}' > "$WORK/wrong_type_depth.json"
rc="$(try_config "$WORK/wrong_type_depth.json")"
check "max_type_depth as string: exit code" "$rc" "1"
check "max_type_depth as string: message" \
    "$(grep -c "max_type_depth: must be an integer" "$WORK/probe.err")" "1"

echo '{"config_version": 1, "analysis": {"policy": 1}}' > "$WORK/wrong_type_policy.json"
rc="$(try_config "$WORK/wrong_type_policy.json")"
check "policy as integer: exit code" "$rc" "1"
check "policy as integer: message" \
    "$(grep -c "policy: must be a string" "$WORK/probe.err")" "1"

echo '{"config_version": 1, "analysis": {"policy": "relaxed", "heuristics": "guard_based_length"}}' > "$WORK/wrong_type_heuristics.json"
rc="$(try_config "$WORK/wrong_type_heuristics.json")"
check "heuristics as bare string (not array): exit code" "$rc" "1"
check "heuristics as bare string (not array): message" \
    "$(grep -c "heuristics: must be an array" "$WORK/probe.err")" "1"

echo '{"config_version": 1, "coverage": {"enabled": "yes", "min_marshal_count": 1}}' > "$WORK/wrong_type_enabled.json"
rc="$(try_config "$WORK/wrong_type_enabled.json")"
check "coverage.enabled as string: exit code" "$rc" "1"
check "coverage.enabled as string: message" \
    "$(grep -c "enabled: must be a boolean" "$WORK/probe.err")" "1"

echo '{"config_version": 1, "coverage": {"enabled": true, "min_marshal_count": "40"}}' > "$WORK/wrong_type_count.json"
rc="$(try_config "$WORK/wrong_type_count.json")"
check "coverage.min_marshal_count as string: exit code" "$rc" "1"
check "coverage.min_marshal_count as string: message" \
    "$(grep -c "min_marshal_count: must be an integer" "$WORK/probe.err")" "1"

echo '{"config_version": 1, "profile_name": 42}' > "$WORK/wrong_type_profile_name.json"
rc="$(try_config "$WORK/wrong_type_profile_name.json")"
check "profile_name as integer: exit code" "$rc" "1"

cat > "$WORK/wrong_type_arg_index.json" <<'EOF'
{"config_version": 1, "contracts": {"f": {"0": {
  "size_operand": {"arg_index": "0", "source": "value"},
  "stride_operand": {"arg_index": 1, "source": "value"},
  "const_size": 8
}}}}
EOF
rc="$(try_config "$WORK/wrong_type_arg_index.json")"
check "contract arg_index as string: exit code" "$rc" "1"

echo '{"config_version": "1"}' > "$WORK/wrong_type_version.json"
rc="$(try_config "$WORK/wrong_type_version.json")"
check "config_version as string: exit code" "$rc" "1"

echo ""
echo "=== backward compatibility: no --config reproduces default behavior ==="
json_default="$(infer_one "$SVE_DIR" cblas_direct)"
cp "$SVE_DIR/cblas_direct.c" "$WORK/cblas_direct2.c"
( cd "$WORK" && "$LIND_COMPILE" --emit-llvm cblas_direct2.c ) >/dev/null 2>&1
json_noconfig="$WORK/cblas_direct2.marshal.json"
"$MARSHAL_INFER" --json -o "$json_noconfig" "$WORK/cblas_direct2.bc" 2>/dev/null
# Compare the parts that should be identical (decision + args), ignoring the
# "module" label which legitimately differs by input filename.
d1="$(pyjq "$json_default" "(f['functions'][0]['decision'], f['functions'][0]['args'])")"
d2="$(pyjq "$json_noconfig" "(f['functions'][0]['decision'], f['functions'][0]['args'])")"
check "no --config: identical decision+args to --emit-marshal's own default path" "$d2" "$d1"

echo ""
echo "=== max_type_depth threading ==="
cat > "$WORK/depth_default.json" <<'EOF'
{"config_version": 1}
EOF
cat > "$WORK/depth_shallow.json" <<'EOF'
{"config_version": 1, "analysis": {"max_type_depth": 2}}
EOF
json="$(infer_one "$SCRIPT_DIR" deep_struct "$WORK/depth_default.json")" && {
    check "default depth: decision" "$(pyjq "$json" "f['functions'][0]['decision']")" "marshal"
}
cp "$SCRIPT_DIR/deep_struct.c" "$WORK/deep_struct2.c"
( cd "$WORK" && "$LIND_COMPILE" --emit-llvm deep_struct2.c ) >/dev/null 2>&1
json2="$WORK/deep_struct2.marshal.json"
"$MARSHAL_INFER" --json --config "$WORK/depth_shallow.json" -o "$json2" "$WORK/deep_struct2.bc" 2>/dev/null
check "max_type_depth=2: decision (inner struct truncated)" \
    "$(pyjq "$json2" "f['functions'][0]['decision']")" "force_local"

echo ""
echo "=== max_delegation_hops=0 disables one-hop delegation ==="
cp "$SVE_DIR/wrapper.c" "$WORK/wrapper.c"
cp "$SVE_DIR/worker.c" "$WORK/worker.c"
( cd "$WORK" && "$LIND_COMPILE" --emit-llvm wrapper.c && "$LIND_COMPILE" --emit-llvm worker.c ) >/dev/null 2>&1
cat > "$WORK/nohop.json" <<'EOF'
{"config_version": 1, "analysis": {"max_delegation_hops": 0}}
EOF
json3="$WORK/nohop.marshal.json"
"$MARSHAL_INFER" --json --config "$WORK/nohop.json" -o "$json3" "$WORK/wrapper.bc" "$WORK/worker.bc" 2>/dev/null
check "max_delegation_hops=0: wrapper_axpy decision (delegation disabled)" \
    "$(pyjq "$json3" "[fn for fn in f['functions'] if fn['name']=='wrapper_axpy'][0]['decision']")" \
    "force_local"

echo ""
echo "=== heuristic-gated marshalling (relaxed policy) ==="
cp "$SCRIPT_DIR/walk_heuristic.c" "$WORK/walk_heuristic.c"
( cd "$WORK" && "$LIND_COMPILE" --emit-llvm walk_heuristic.c -- -O2 ) >/dev/null 2>&1

json_strict="$WORK/wh_strict.marshal.json"
"$MARSHAL_INFER" --json -o "$json_strict" "$WORK/walk_heuristic.bc" 2>/dev/null
check "strict (no config): decision" \
    "$(pyjq "$json_strict" "f['functions'][0]['decision']")" "force_local"

cat > "$WORK/only_length.json" <<'EOF'
{"config_version": 1, "analysis": {"policy": "relaxed", "heuristics": ["guard_based_length"]}}
EOF
json_len="$WORK/wh_len.marshal.json"
"$MARSHAL_INFER" --json --config "$WORK/only_length.json" -o "$json_len" "$WORK/walk_heuristic.bc" 2>/dev/null
check "guard_based_length alone: decision (unroll_scaled_stride also needed)" \
    "$(pyjq "$json_len" "f['functions'][0]['decision']")" "force_local"

cat > "$WORK/only_stride.json" <<'EOF'
{"config_version": 1, "analysis": {"policy": "relaxed", "heuristics": ["unroll_scaled_stride"]}}
EOF
json_str="$WORK/wh_str.marshal.json"
"$MARSHAL_INFER" --json --config "$WORK/only_stride.json" -o "$json_str" "$WORK/walk_heuristic.bc" 2>/dev/null
check "unroll_scaled_stride alone: decision (guard_based_length also needed)" \
    "$(pyjq "$json_str" "f['functions'][0]['decision']")" "force_local"

cat > "$WORK/both.json" <<'EOF'
{"config_version": 1, "analysis": {"policy": "relaxed", "heuristics": ["guard_based_length", "unroll_scaled_stride"]}}
EOF
json_both="$WORK/wh_both.marshal.json"
"$MARSHAL_INFER" --json --config "$WORK/both.json" -o "$json_both" "$WORK/walk_heuristic.bc" 2>/dev/null
check "both heuristics together: decision" \
    "$(pyjq "$json_both" "f['functions'][0]['decision']")" "marshal"
arg2="$(pyjq "$json_both" "__import__('json').dumps(f['functions'][0]['args'][2])")"
echo "$arg2" | python3 -c "
import json, sys
a = json.load(sys.stdin)
assert a['size_kind'] == 'stride_vector', a
assert a['confidence'] == 'heuristic', a['confidence']
assert a['size_operand'] == {'arg_index': 0, 'source': 'value'}, a['size_operand']
assert a['stride_operand'] == {'arg_index': 1, 'source': 'value'}, a['stride_operand']
print('ok')
" && { echo "  ok    both heuristics together: correct operands, confidence=heuristic"; PASS=$((PASS+1)); } \
  || { echo "  FAIL  both heuristics together: operand/confidence shape"; echo "$arg2"; FAIL=$((FAIL+1)); }

echo ""
echo "=== compound dominating guard disambiguation (n vs. stride) ==="
cp "$SCRIPT_DIR/compound_guard.c" "$WORK/compound_guard.c"
( cd "$WORK" && "$LIND_COMPILE" --emit-llvm compound_guard.c -- -O2 ) >/dev/null 2>&1
json_cg="$WORK/cg.marshal.json"
"$MARSHAL_INFER" --json --config "$WORK/both.json" -o "$json_cg" "$WORK/compound_guard.bc" 2>/dev/null
check "compound guard: decision" \
    "$(pyjq "$json_cg" "f['functions'][0]['decision']")" "marshal"
arg2cg="$(pyjq "$json_cg" "__import__('json').dumps(f['functions'][0]['args'][2])")"
echo "$arg2cg" | python3 -c "
import json, sys
a = json.load(sys.stdin)
assert a['size_operand'] == {'arg_index': 0, 'source': 'value'}, a['size_operand']
assert a['stride_operand'] == {'arg_index': 1, 'source': 'value'}, a['stride_operand']
print('ok')
" && { echo "  ok    compound guard: length correctly attributed to n, not stride"; PASS=$((PASS+1)); } \
  || { echo "  FAIL  compound guard: wrong operand attribution"; echo "$arg2cg"; FAIL=$((FAIL+1)); }

echo ""
echo "=== SAFETY: inclusive bound (i<=n) must NEVER be accepted, any heuristic combination ==="
cp "$SCRIPT_DIR/inclusive_reject.c" "$WORK/inclusive_reject.c"
( cd "$WORK" && "$LIND_COMPILE" --emit-llvm inclusive_reject.c -- -O2 ) >/dev/null 2>&1
json_incl="$WORK/incl.marshal.json"
"$MARSHAL_INFER" --json --config "$WORK/both.json" -o "$json_incl" "$WORK/inclusive_reject.bc" 2>/dev/null
check "inclusive bound: decision (both heuristics enabled)" \
    "$(pyjq "$json_incl" "f['functions'][0]['decision']")" "force_local"
check "inclusive bound: no stride_vector emitted anywhere" \
    "$(pyjq "$json_incl" "any(a.get('size_kind')=='stride_vector' for a in f['functions'][0].get('args', []))")" \
    "False"

echo ""
echo "=== SAFETY MATRIX: direct (non-unrolled) latch predicate orientation ==="
# See matrix.c's own comment for the full axis table. Every function here
# compiles WITHOUT unrolling (-fno-unroll-loops), so boundConfirmsExclusive-
# Length's DIRECT-match branch (not the masked-unroll branch) is exactly
# what's exercised -- this is the class of bug reported against a real
# `for (int i = 0; i <= n; ++i)` loop, which involves no masking at all.
cp "$SCRIPT_DIR/matrix.c" "$WORK/matrix.c"
( cd "$WORK" && "$LIND_COMPILE" --emit-llvm matrix.c -- -O1 -fno-unroll-loops ) >/dev/null 2>&1
json_matrix="$WORK/matrix.marshal.json"
"$MARSHAL_INFER" --json --config "$WORK/both.json" -o "$json_matrix" "$WORK/matrix.bc" 2>/dev/null

matrix_check() {
    local fn="$1" want="$2"
    local got
    got="$(pyjq "$json_matrix" "([x for x in f['functions'] if x['name']=='$fn'] or [{'decision':'MISSING'}])[0]['decision']")"
    check "matrix: $fn" "$got" "$want"
}
matrix_check matrix_signed_lt_fwd            marshal
matrix_check matrix_signed_le_fwd            force_local
matrix_check matrix_signed_lt_rev            marshal
matrix_check matrix_signed_le_rev            force_local
matrix_check matrix_unsigned_lt_fwd          marshal
matrix_check matrix_unsigned_le_fwd          force_local
matrix_check matrix_unsigned_lt_rev          marshal
matrix_check matrix_unsigned_le_rev          force_local
matrix_check matrix_fortran_lt_fwd           marshal
matrix_check matrix_fortran_le_fwd           force_local

echo ""
echo "=== factored stride trip count: unconditional exact proof, no --config ==="
# See factored_bound_pos.c/factored_bound_neg.c's own comments and
# PATTERNS.md's "fused index/counter with a rescaled bound" entry. No
# --config anywhere in this section: detectFactoredStrideTripCount is
# unconditionally available (Confidence::Proven), unlike guard_based_length/
# unroll_scaled_stride which require an explicit relaxed-policy opt-in.
cp "$SCRIPT_DIR/factored_bound_pos.c" "$WORK/factored_bound_pos.c"
( cd "$WORK" && "$LIND_COMPILE" --emit-llvm factored_bound_pos.c -- -O1 -fno-unroll-loops ) >/dev/null 2>&1
json_fbpos="$WORK/fbpos.marshal.json"
"$MARSHAL_INFER" --json -o "$json_fbpos" "$WORK/factored_bound_pos.bc" 2>/dev/null

# $2/$3: expected size_operand/stride_operand arg_index. $4: "value" or
# "pointee_i32" (same for both operands in every fixture here).
fbpos_check() {
    local fn="$1" wantSizeIdx="$2" wantStrideIdx="$3" wantSource="$4"
    local decision confidence sizeOp strideOp
    decision="$(pyjq "$json_fbpos" "([x for x in f['functions'] if x['name']=='$fn'] or [{'decision':'MISSING'}])[0]['decision']")"
    check "factored (+): $fn decision" "$decision" "marshal"
    confidence="$(pyjq "$json_fbpos" "next((a.get('confidence') for x in f['functions'] if x['name']=='$fn' for a in x.get('args',[]) if a.get('size_kind')=='stride_vector'), None)")"
    check "factored (+): $fn confidence" "$confidence" "proven"
    sizeOp="$(pyjq "$json_fbpos" "__import__('json').dumps(next((a.get('size_operand') for x in f['functions'] if x['name']=='$fn' for a in x.get('args',[]) if a.get('size_kind')=='stride_vector'), None))")"
    check "factored (+): $fn size_operand" "$sizeOp" "{\"arg_index\": $wantSizeIdx, \"source\": \"$wantSource\"}"
    strideOp="$(pyjq "$json_fbpos" "__import__('json').dumps(next((a.get('stride_operand') for x in f['functions'] if x['name']=='$fn' for a in x.get('args',[]) if a.get('size_kind')=='stride_vector'), None))")"
    check "factored (+): $fn stride_operand" "$strideOp" "{\"arg_index\": $wantStrideIdx, \"source\": \"$wantSource\"}"
}
fbpos_check pos_signed_value       0 1 value
fbpos_check pos_reversed_cmp       0 1 value
fbpos_check pos_fortran            0 1 pointee_i32
fbpos_check pos_differently_named  0 1 value

cp "$SCRIPT_DIR/factored_bound_neg.c" "$WORK/factored_bound_neg.c"
( cd "$WORK" && "$LIND_COMPILE" --emit-llvm factored_bound_neg.c -- -O1 -fno-unroll-loops ) >/dev/null 2>&1
json_fbneg="$WORK/fbneg.marshal.json"
"$MARSHAL_INFER" --json -o "$json_fbneg" "$WORK/factored_bound_neg.bc" 2>/dev/null

fbneg_check() {
    local fn="$1"
    local confidence
    confidence="$(pyjq "$json_fbneg" "next((a.get('confidence') for x in f['functions'] if x['name']=='$fn' for a in x.get('args',[]) if a.get('size_kind')=='stride_vector'), None)")"
    check "factored (-): $fn never resolves this pairing" "$confidence" "None"
}
fbneg_check neg_inclusive_bound
fbneg_check neg_nonzero_origin
fbneg_check neg_different_factor
fbneg_check neg_offset_bound
fbneg_check neg_missing_guard
fbneg_check neg_partial_guard
fbneg_check neg_unsigned_no_nuw
fbneg_check neg_unrelated_counter
fbneg_check neg_different_step_counter

# neg_missing_guard's pairing never resolving (checked above) isn't the whole
# story: before the fail-closed fix below, this exact function fell through
# to the single-element fallback and was observably "marshal" -- the overall
# decision must be checked directly, not just the absence of a resolved
# stride_vector pairing.
check "factored (-): neg_missing_guard overall decision" \
    "$(pyjq "$json_fbneg" "([x for x in f['functions'] if x['name']=='neg_missing_guard'] or [{'decision':'MISSING'}])[0]['decision']")" \
    "force_local"

echo ""
echo "=== fail-closed on unresolved indexed access: no --config ==="
# analyzeAccess/inferFunction must never fall through to the single-element
# fallback for a scalar-pointee pointer that's visibly indexed by anything
# other than a provable constant zero, regardless of whether that indexed
# access ever resolves to an exact extent (see Access::requiresDynamicExtent
# in Infer.cpp). Unconditionally available, no --config anywhere here.
cp "$SCRIPT_DIR/dynamic_extent.c" "$WORK/dynamic_extent.c"
( cd "$WORK" && "$LIND_COMPILE" --emit-llvm dynamic_extent.c -- -O1 -fno-unroll-loops ) >/dev/null 2>&1
json_dynext="$WORK/dynext.marshal.json"
"$MARSHAL_INFER" --json -o "$json_dynext" "$WORK/dynamic_extent.bc" 2>/dev/null

dynext_decision() {
    pyjq "$json_dynext" "([x for x in f['functions'] if x['name']=='$1'] or [{'decision':'MISSING'}])[0]['decision']"
}
check "dynamic extent: unresolved loop bound -> force_local" \
    "$(dynext_decision unresolved_loop_bound)" "force_local"
check "dynamic extent: dynamic non-loop index -> force_local" \
    "$(dynext_decision dynamic_direct_index)" "force_local"
check "dynamic extent: constant nonzero index -> force_local" \
    "$(dynext_decision constant_nonzero_index)" "force_local"
check "dynamic extent: negative offset -> force_local" \
    "$(dynext_decision negative_offset_index)" "force_local"
check "dynamic extent: direct *p access -> one-element marshal" \
    "$(dynext_decision direct_deref)" "marshal"
check "dynamic extent: p[0] only -> one-element marshal" \
    "$(dynext_decision zero_index_only)" "marshal"
check "dynamic extent: genuine scalar out-param unaffected" \
    "$(dynext_decision write_scalar_out)" "marshal"

echo ""
echo "=== contract application + provenance ==="
cat > "$WORK/contract.json" <<EOF
{"config_version": 1, "contracts": {"interleaved_walk": {"2": {
  "size_operand": {"arg_index": 0, "source": "value"},
  "stride_operand": {"arg_index": 1, "source": "value"},
  "const_size": 8,
  "dir": "inout"
}}}}
EOF
json4="$(infer_one "$SVE_DIR" noncanonical_scale "$WORK/contract.json")" && {
    check "contract override: decision" "$(pyjq "$json4" "f['functions'][0]['decision']")" "marshal"
    check "contract override: confidence recorded as configured" \
        "$(pyjq "$json4" "f['functions'][0]['args'][2].get('confidence')")" "configured"
    check "contract override: warning names the config file" \
        "$(pyjq "$json4" "any('asserted by config contract' in w for w in f['functions'][0]['warnings'])")" \
        "True"
}

echo ""
echo "=== contract never applied -> hard error, not silent no-op or warning ==="
cat > "$WORK/stale_contract.json" <<'EOF'
{"config_version": 1, "contracts": {"function_that_does_not_exist": {"0": {
  "size_operand": {"arg_index": 0, "source": "value"},
  "stride_operand": {"arg_index": 1, "source": "value"},
  "const_size": 8
}}}}
EOF
cp "$SVE_DIR/cblas_direct.c" "$WORK/cd3.c"
( cd "$WORK" && "$LIND_COMPILE" --emit-llvm cd3.c ) >/dev/null 2>&1
"$MARSHAL_INFER" --json --config "$WORK/stale_contract.json" -o "$WORK/stale.json" "$WORK/cd3.bc" 2>"$WORK/stale.err"
rc=$?
check "stale contract: exit code" "$rc" "1"
check "stale contract: ERROR printed" \
    "$(grep -c "ERROR.*never applied" "$WORK/stale.err")" "1"
check "stale contract: no JSON output written" \
    "$([[ -s "$WORK/stale.json" ]] && echo present || echo absent)" "absent"

echo ""
echo "=== contract vs. real signature: hard errors for every incompatible shape ==="
cp "$SVE_DIR/noncanonical_scale.c" "$WORK/ns.c"
( cd "$WORK" && "$LIND_COMPILE" --emit-llvm ns.c ) >/dev/null 2>&1

cat > "$WORK/contract_bad_target.json" <<'EOF'
{"config_version": 1, "contracts": {"interleaved_walk": {"99": {
  "size_operand": {"arg_index": 0, "source": "value"},
  "stride_operand": {"arg_index": 1, "source": "value"},
  "const_size": 8
}}}}
EOF
"$MARSHAL_INFER" --json --config "$WORK/contract_bad_target.json" -o "$WORK/o1.json" "$WORK/ns.bc" 2>"$WORK/e1.err"
check "contract target arg out of range: exit code" "$?" "1"
check "contract target arg out of range: message" \
    "$(grep -c "no such argument" "$WORK/e1.err")" "1"

cat > "$WORK/contract_target_not_pointer.json" <<'EOF'
{"config_version": 1, "contracts": {"interleaved_walk": {"0": {
  "size_operand": {"arg_index": 0, "source": "value"},
  "stride_operand": {"arg_index": 1, "source": "value"},
  "const_size": 8
}}}}
EOF
"$MARSHAL_INFER" --json --config "$WORK/contract_target_not_pointer.json" -o "$WORK/o2.json" "$WORK/ns.bc" 2>"$WORK/e2.err"
check "contract target not a pointer: exit code" "$?" "1"
check "contract target not a pointer: message" \
    "$(grep -c "is not a pointer argument" "$WORK/e2.err")" "1"

cat > "$WORK/contract_operand_out_of_range.json" <<'EOF'
{"config_version": 1, "contracts": {"interleaved_walk": {"2": {
  "size_operand": {"arg_index": 99, "source": "value"},
  "stride_operand": {"arg_index": 1, "source": "value"},
  "const_size": 8
}}}}
EOF
"$MARSHAL_INFER" --json --config "$WORK/contract_operand_out_of_range.json" -o "$WORK/o3.json" "$WORK/ns.bc" 2>"$WORK/e3.err"
check "contract size_operand out of range: exit code" "$?" "1"
check "contract size_operand out of range: message" \
    "$(grep -c "does not exist" "$WORK/e3.err")" "1"

cat > "$WORK/contract_operand_wrong_kind.json" <<'EOF'
{"config_version": 1, "contracts": {"interleaved_walk": {"2": {
  "size_operand": {"arg_index": 2, "source": "value"},
  "stride_operand": {"arg_index": 1, "source": "value"},
  "const_size": 8
}}}}
EOF
"$MARSHAL_INFER" --json --config "$WORK/contract_operand_wrong_kind.json" -o "$WORK/o4.json" "$WORK/ns.bc" 2>"$WORK/e4.err"
check "contract size_operand targets a pointer, not a scalar: exit code" "$?" "1"
check "contract size_operand targets a pointer, not a scalar: message" \
    "$(grep -c "is not a compatible integer scalar" "$WORK/e4.err")" "1"

cat > "$WORK/contract_pointee_i32_wrong_kind.json" <<'EOF'
{"config_version": 1, "contracts": {"interleaved_walk": {"2": {
  "size_operand": {"arg_index": 0, "source": "value"},
  "stride_operand": {"arg_index": 0, "source": "pointee_i32"},
  "const_size": 8
}}}}
EOF
"$MARSHAL_INFER" --json --config "$WORK/contract_pointee_i32_wrong_kind.json" -o "$WORK/o5.json" "$WORK/ns.bc" 2>"$WORK/e5.err"
check "contract pointee_i32 targets a plain scalar, not a pointer: exit code" "$?" "1"
check "contract pointee_i32 targets a plain scalar, not a pointer: message" \
    "$(grep -c "is not a pointer to a 32-bit" "$WORK/e5.err")" "1"

echo ""
echo "=== coverage threshold enforcement ==="
cat > "$WORK/cov_pass.json" <<'EOF'
{"config_version": 1, "coverage": {"enabled": true, "min_marshal_count": 1}}
EOF
cp "$SVE_DIR/cblas_direct.c" "$WORK/cd4.c"
( cd "$WORK" && "$LIND_COMPILE" --emit-llvm cd4.c ) >/dev/null 2>&1
"$MARSHAL_INFER" --json --config "$WORK/cov_pass.json" -o /dev/null "$WORK/cd4.bc" 2>/dev/null
check "coverage threshold met: exit code" "$?" "0"

cat > "$WORK/cov_fail.json" <<'EOF'
{"config_version": 1, "coverage": {"enabled": true, "min_marshal_count": 99}}
EOF
"$MARSHAL_INFER" --json --config "$WORK/cov_fail.json" -o /dev/null "$WORK/cd4.bc" 2>"$WORK/cov_fail.err"
rc=$?
check "coverage threshold NOT met: exit code" "$rc" "1"
check "coverage threshold NOT met: error message" \
    "$(grep -c "COVERAGE THRESHOLD FAILED" "$WORK/cov_fail.err")" "1"

echo ""
echo "=== the checked-in OpenBLAS profile itself validates ==="
# try_config's probe function (scalar_out.c's halve_and_report) has neither
# cblas_daxpy nor daxpy_, so it correctly trips the profile's OWN
# stale-contract warnings and coverage threshold (both tested in isolation
# above) -- this section checks SCHEMA validation succeeded specifically,
# not that the whole run's coverage passed against an unrelated probe.
try_config "$REPO_ROOT/tools/marshal-infer/profiles/openblas.json" >/dev/null
check "profiles/openblas.json: no schema/load error" \
    "$(grep -c -- '--config:' "$WORK/probe.err")" "0"

echo ""
echo "Results: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
