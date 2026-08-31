#!/usr/bin/env bash
# The interposition transport's raw-ABI-slot capacity (currently 6) is
# independently hard-coded in four places, not derived from one shared
# definition -- see each site's own comment for why:
#   1. tools/marshal-infer/src/Infer.cpp        (kMaxRawArgSlots)
#   2. tools/marshal-gen/gen_grate.py            (LIND_RAW_ARGS_MAX)
#   3. tests/grate-tests/lib-interpose/lind_marshal.h (LIND_RAW_ARGS_MAX)
#   4. src/wasmtime/crates/wasmtime/src/runtime/linker.rs (LIND_MAX_RAW_ARG_SLOTS)
#
# This script is the substitute for a real shared/generated constant: it
# extracts the numeric value from each of the four sources and fails loudly
# if any of them disagree, rather than letting a partial edit silently
# desync inference, generation, and the two runtime enforcement points.
#
# Usage: tests/grate-tests/lib-interpose/check_raw_arg_slot_consistency.sh
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

fail=0

extract() {
    local label="$1" file="$2" pattern="$3"
    local val
    val="$(grep -oP "$pattern" "$file" | head -1)"
    if [[ -z "$val" ]]; then
        echo "MISSING: could not find $label's raw-arg-slot constant in $file"
        fail=1
        echo ""
        return
    fi
    echo "$val"
}

infer_val="$(extract "marshal-infer" \
    "$REPO_ROOT/tools/marshal-infer/src/Infer.cpp" \
    'kMaxRawArgSlots\s*=\s*\K\d+')"
genrate_val="$(extract "gen_grate.py" \
    "$REPO_ROOT/tools/marshal-gen/gen_grate.py" \
    'LIND_RAW_ARGS_MAX\s*=\s*\K\d+')"
header_val="$(extract "lind_marshal.h" \
    "$SCRIPT_DIR/lind_marshal.h" \
    '#define\s+LIND_RAW_ARGS_MAX\s+\K\d+')"
linker_val="$(extract "linker.rs" \
    "$REPO_ROOT/src/wasmtime/crates/wasmtime/src/runtime/linker.rs" \
    'LIND_MAX_RAW_ARG_SLOTS:\s*usize\s*=\s*\K\d+')"

if [[ "$fail" -eq 1 ]]; then
    exit 1
fi

echo "Infer.cpp:kMaxRawArgSlots       = $infer_val"
echo "gen_grate.py:LIND_RAW_ARGS_MAX  = $genrate_val"
echo "lind_marshal.h:LIND_RAW_ARGS_MAX = $header_val"
echo "linker.rs:LIND_MAX_RAW_ARG_SLOTS = $linker_val"

if [[ "$infer_val" == "$genrate_val" && "$genrate_val" == "$header_val" && "$header_val" == "$linker_val" ]]; then
    echo "ok    all four raw-arg-slot constants agree ($infer_val)"
    exit 0
fi

echo "FAIL  raw-arg-slot constants disagree across inference/generation/runtime"
exit 1
