#!/usr/bin/env bash
# Focused lib-interpose grate test suite (NOT full-libc/full-libm, which have
# their own runners under full-libc/ and full-libm/).
#
# Usage:
#   bash tests/grate-tests/lib-interpose/run_tests.sh [--allow-skips]
#
# Every fixture (cage, grate, and the shared libtoy library) is compiled
# fresh from source each run; nothing here trusts a locally-built artifact.
# Each test asserts the exact, ordered lines its cage/grate must print (see
# match_ordered_lines and run_test's header). Failures are categorized (see
# category_for). Every directory with a `*_grate.c` (other than full-libc/
# full-libm) must be declared as a test below or the run fails.
#
# --allow-skips: without it, any skipped maintained test (e.g. a missing
# fixture dependency) fails the run; pass it for an explicitly optional
# local run.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib.sh"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
LINDFS="$REPO_ROOT/lindfs"
GRATES_DIR="$LINDFS/grates"
LIND_COMPILE="$REPO_ROOT/scripts/lind_compile"
LIND_RUN="$REPO_ROOT/scripts/lind_run"

# Route lind_log! diagnostics (Default category is on by default) to stderr,
# where run_test's output capture can see them, instead of their default
# destination (a LIND.log file) -- needed for fail-closed-widesig to assert
# on the rejected-portal diagnostic rather than just a bare exit code.
export LIND_LOG_OUTPUT=stderr

# auto-libz / auto-libz-spike statically link the real libz implementation
# (a static grate must resolve every symbol it interposes at link time) from
# the sibling lind-wasm-apps checkout's zlib build, matching the sibling-repo
# layout convention already used by openblas/compile_openblas.sh. Overridable
# for a non-standard checkout layout; tests that need it SKIP (not FAIL) with
# a clear message if it's missing, rather than failing the whole suite.
LIBZ_A="${LIBZ_A:-$(cd "$REPO_ROOT/.." 2>/dev/null && pwd)/lind-wasm-apps/zlib/libz.a}"

ALLOW_SKIPS="no"
for arg in "$@"; do
    [[ "$arg" == "--allow-skips" ]] && ALLOW_SKIPS="yes"
done

mkdir -p "$GRATES_DIR" "$LINDFS/lib"

# custom-lib/libtoy.c is the preloaded fixture library shared by 5 of the
# tests below (custom-lib, auto-scalar, auto-handle, auto-nested, auto-argv).
# Build it fresh here too, same reasoning as every cage/grate: a stale local
# copy in lindfs/lib/ would silently mask a real regression.
echo "Building shared fixture: libtoy.so"
if ! "$LIND_COMPILE" --compile-library "$SCRIPT_DIR/custom-lib/libtoy.c" \
        > /tmp/lib-interpose-compile.log 2>&1; then
    echo "FATAL: failed to build custom-lib/libtoy.c (needed by 5 tests):"
    cat /tmp/lib-interpose-compile.log
    exit 2
fi
cp "$SCRIPT_DIR/custom-lib/libtoy.so" "$LINDFS/lib/libtoy.so"
echo ""

PASS=0
FAIL=0
SKIP=0
declare -A CATEGORY_COUNTS
declare -a FAILURES
declare -a DECLARED_TESTS

pass_test() {
    echo "  PASS  $1"
    PASS=$((PASS + 1))
}

fail_test() {
    local name="$1" category="$2" output="$3"
    echo "  FAIL  $name [$category]"
    echo "$output" | tail -12 | sed 's/^/         /'
    FAIL=$((FAIL + 1))
    CATEGORY_COUNTS["$category"]=$(( ${CATEGORY_COUNTS["$category"]:-0} + 1 ))
    FAILURES+=("$name [$category]")
}

skip_test() {
    echo "  SKIP  $1 -- $2"
    SKIP=$((SKIP + 1))
}

# compile_src <src.c> [extra clang args...]
# Compiles a dynamic (non-grate) executable next to its source. -fno-builtin
# disables builtin lowering, so a call like memcpy/strlen/memcmp stays a real
# interposable call/import instead of being inlined or constant-folded away.
compile_src() {
    local src="$1"; shift
    "$LIND_COMPILE" "$src" -- -fno-builtin "$@" > /tmp/lib-interpose-compile.log 2>&1
}

# compile_grate <src.c> [extra clang/source args...]
compile_grate() {
    local src="$1"; shift
    "$LIND_COMPILE" -s --compile-grate --fpcast-emu "$src" -I "$SCRIPT_DIR" "$@" \
        > /tmp/lib-interpose-compile.log 2>&1
}

# run_test <name> <cage_src|""> <grate_src> <preload_csv|""> <strict:yes|no> \
#          <run_args...> -- <expect...> -- <evidence...> [-- <forbidden...>]
#
# cage_src/grate_src: source path relative to this directory; output name is
#   derived automatically (lind_compile always names <src>.c -> <src>.cwasm,
#   next to the source). Pass "" for cage_src if there's no cage of its own
#   (e.g. zlib-python, whose "cage" is a pre-existing lindfs-resident app).
#
# preload_csv: comma-separated `module=path` preload specs (e.g.
#   "env=/lib/libtoy.so" or "env=/lib/libz.so,env=/lib/libpython3.14.so").
# strict: if "yes", every preload above gets `:interposed` appended -- only
#   safe for a preload whose library surface the cage exercises ENTIRELY
#   through registered handlers (see each test's own note below for why).
# expect: exact literal lines that MUST all appear in the combined output,
#   in this order -- catches "prints PASS but something afterward is wrong"
#   and "prints PASS by coincidence without dispatch ever firing" alike.
# evidence: same as expect, but specifically the marker line(s) that could
#   ONLY appear if the grate's handler genuinely ran (as opposed to a silent
#   fallback to the real, uninterposed implementation happening to produce
#   the same result) -- required in addition to `expect` for any test whose
#   handler's return value alone is indistinguishable from what the real,
#   uninterposed function would have produced.
# forbidden: optional (the trailing `--` may be omitted entirely if empty)
#   -- lines that must NOT appear anywhere in the output. A fail-closed
#   rejection test must forbid its handler's own "handler ran" marker: a
#   marshaller that wrongly invokes the handler, which then dereferences a
#   foreign pointer and traps, produces the same caller-visible GRATE_ERR as
#   a correct rejection -- expect[] alone can't tell those apart.
run_test() {
    local name="$1" cage_src="$2" grate_src="$3" preload_csv="$4" strict="$5"
    shift 5
    local run_args=()
    while [[ "$#" -gt 0 && "$1" != "--" ]]; do run_args+=("$1"); shift; done
    shift # consume the first --
    local expect=()
    while [[ "$#" -gt 0 && "$1" != "--" ]]; do expect+=("$1"); shift; done
    shift # consume the second --
    local evidence=()
    while [[ "$#" -gt 0 && "$1" != "--" ]]; do evidence+=("$1"); shift; done
    local forbidden=()
    if [[ "$#" -gt 0 ]]; then
        shift # consume the third --
        forbidden=("$@")
    fi

    DECLARED_TESTS+=("$name")

    # lind_compile always names its output after the (first) source file,
    # next to it: foo.c -> foo.cwasm.
    local grate_cwasm="$(basename "${grate_src%.c}").cwasm"

    # -- build --
    if [[ -n "$cage_src" ]]; then
        if ! compile_src "$SCRIPT_DIR/$cage_src"; then
            fail_test "$name" build "COMPILE_STEP_FAILED (cage)
$(cat /tmp/lib-interpose-compile.log)"
            return
        fi
    fi
    if ! compile_grate "$SCRIPT_DIR/$grate_src" "${GRATE_EXTRA[@]}"; then
        fail_test "$name" build "COMPILE_STEP_FAILED (grate)
$(cat /tmp/lib-interpose-compile.log)"
        return
    fi

    # -- stage --
    local staged=()
    cp "$SCRIPT_DIR/$(dirname "$grate_src")/$grate_cwasm" "$GRATES_DIR/"
    staged+=("$GRATES_DIR/$grate_cwasm")
    if [[ -n "$cage_src" ]]; then
        local cage_cwasm="$(basename "${cage_src%.c}").cwasm"
        local cage_dst="$LINDFS/$cage_cwasm"
        cp "$SCRIPT_DIR/$(dirname "$cage_src")/$cage_cwasm" "$cage_dst"
        staged+=("$cage_dst")
    fi

    # -- assemble preload args --
    local preload_args=()
    if [[ -n "$preload_csv" ]]; then
        local IFS=','
        local p
        for p in $preload_csv; do
            if [[ "$strict" == "yes" ]]; then
                preload_args+=(--preload "${p}:interposed")
            else
                preload_args+=(--preload "$p")
            fi
        done
    fi

    # -- run, correctly capturing the ACTUAL command's exit code (not `true`'s) --
    local output exit_code
    output=$(cd "$LINDFS" && timeout 30 "$LIND_RUN" "${preload_args[@]}" \
        "grates/$grate_cwasm" "${run_args[@]}" 2>&1)
    exit_code=$?

    rm -f "${staged[@]}"

    # -- validate: expect[] and evidence[] are each their own ordered
    # sequence of complete lines, independent of each other (evidence
    # commonly precedes expect in the real output). --
    local missing=() m line
    if ! m="$(match_ordered_lines "$output" "${expect[@]}")"; then
        while IFS= read -r line; do missing+=("$line"); done <<<"$m"
    fi
    if [[ ${#evidence[@]} -gt 0 ]] && ! m="$(match_ordered_lines "$output" "${evidence[@]}")"; then
        while IFS= read -r line; do missing+=("[dispatch evidence] $line"); done <<<"$m"
    fi
    if [[ ${#forbidden[@]} -gt 0 ]] && ! m="$(find_forbidden_lines "$output" "${forbidden[@]}")"; then
        while IFS= read -r line; do missing+=("[forbidden, but present] $line"); done <<<"$m"
    fi

    if [[ "$(decide_outcome "$exit_code" "${#missing[@]}")" == "fail" ]]; then
        if [[ "$exit_code" -ne 0 ]]; then
            fail_test "$name" "$(category_for "$output" "$exit_code")" "$output"
        else
            fail_test "$name" "$(category_for "$output" 0)" \
                "validation failed:
$(printf '  - %s\n' "${missing[@]}")
--- actual output ---
$output"
        fi
    else
        pass_test "$name"
    fi
}

# Pre-flight: the interposition transport's raw-ABI-slot capacity is
# duplicated (not shared) across inference/generation/runtime -- fail fast,
# before compiling anything, if those copies have drifted out of sync.
if ! bash "$SCRIPT_DIR/check_raw_arg_slot_consistency.sh"; then
    echo "FATAL: raw-arg-slot constant consistency check failed (see above)" >&2
    exit 1
fi
echo ""

echo "=== lib-interpose focused test suite ==="
echo ""

# --------------------------------------------------------------------------
# libc-rand: intercepts rand() and returns a fixed value. Not strict-safe:
# the cage also uses printf/assert from libc, which have no handlers.
# --------------------------------------------------------------------------
GRATE_EXTRA=()
run_test "libc-rand" \
    "libc-rand/libc-rand.c" \
    "libc-rand/libc-rand_grate.c" \
    "" "no" \
    "/libc-rand.cwasm" \
    -- "[Cage] rand() = 42" "[Cage] PASS" "[Grate|libc-rand] PASS" \
    -- # 42 three times in a row cannot come from the real rand()

# --------------------------------------------------------------------------
# libc-strlen: intercepts strlen() and returns len*2.
# --------------------------------------------------------------------------
GRATE_EXTRA=()
run_test "libc-strlen" \
    "libc-strlen/libc-strlen.c" \
    "libc-strlen/libc-strlen_grate.c" \
    "" "no" \
    "/libc-strlen.cwasm" \
    -- "[Cage] strlen(\"hello\") = 10" "[Cage] PASS" "[Grate|libc-strlen] PASS" \
    -- # real strlen("hello")=5 != 10

# --------------------------------------------------------------------------
# custom-lib: intercepts toy_add/toy_mul from a preloaded wasm library.
# Strict-safe: the cage calls nothing else from libtoy.
# --------------------------------------------------------------------------
GRATE_EXTRA=()
run_test "custom-lib" \
    "custom-lib/custom-lib.c" \
    "custom-lib/custom-lib_grate.c" \
    "env=/lib/libtoy.so" "yes" \
    "/custom-lib.cwasm" \
    -- "[Cage] toy_add(3, 4) = 14" "[Cage] toy_mul(5, 6) = 11" "[Cage] PASS" "[Grate|lib-interpose] PASS" \
    --

# --------------------------------------------------------------------------
# zlib-python: intercepts deflate() so Python's zlib.compress() returns
# b"LIND". No cage source of its own (the Python interpreter + test-zlib.py
# are lindfs-resident, out of this directory's scope). Not strict-safe: the
# cage exercises far more of libz/libpython than these 3 symbols.
# --------------------------------------------------------------------------
GRATE_EXTRA=()
run_test "zlib-python" \
    "" \
    "zlib-python/zlib-python_grate.c" \
    "env=/lib/libz.so,env=/lib/libpython3.14.so" "no" \
    "/usr/local/bin/python" "/test-zlib.py" \
    -- "Compressed bytes: b'LIND'" "[Grate|zlib-python] PASS: deflate intercepted 1 time(s), Python exited 0" \
    -- "[Grate|zlib-python] deflate intercepted — wrote 4 fixed bytes, returning Z_STREAM_END"

# --------------------------------------------------------------------------
# Stage-1 automated marshalling tests
# --------------------------------------------------------------------------

# auto-scalar: SCALAR spec; handler returns a*b instead of the real a+b.
GRATE_EXTRA=()
run_test "auto-scalar" \
    "auto-scalar/auto-scalar.c" \
    "auto-scalar/auto-scalar_grate.c" \
    "env=/lib/libtoy.so" "yes" \
    "/auto-scalar.cwasm" \
    -- "[Cage|auto-scalar] PASS: toy_add(10,3) = 30 (intercepted as multiply)" \
    --

# auto-memcpy: PTR IN/OUT + return alias. The handler calls the REAL memcpy,
# so its result is indistinguishable from an uninterposed call on its own --
# dispatch evidence (the grate's own trace line) is required in addition.
GRATE_EXTRA=()
run_test "auto-memcpy" \
    "auto-memcpy/auto-memcpy.c" \
    "auto-memcpy/auto-memcpy_grate.c" \
    "" "no" \
    "/auto-memcpy.cwasm" \
    -- "[Cage|auto-memcpy] PASS: memcpy copied \"hello, lind!\", return == dst" \
    -- "[Grate|auto-memcpy] memcpy intercepted: n=13"

# auto-strncpy: same pattern as auto-memcpy (real strncpy call, needs
# dispatch evidence).
GRATE_EXTRA=()
run_test "auto-strncpy" \
    "auto-strncpy/auto-strncpy.c" \
    "auto-strncpy/auto-strncpy_grate.c" \
    "" "no" \
    "/auto-strncpy.cwasm" \
    -- "[Cage|auto-strncpy] PASS: strncpy produced \"lind-wasm\", return == dst" \
    -- "[Grate|auto-strncpy] strncpy intercepted: n=32"

# auto-strncpy-short: regression guard for a known marshalling-safety gap
# (see auto-strncpy.c) -- asserts the current, deterministic (non-crashing
# but incorrect) behavior so any change to it is caught. Reuses the
# already-built auto-strncpy grate.
GRATE_EXTRA=()
run_test "auto-strncpy-short" \
    "auto-strncpy/auto-strncpy-short.c" \
    "auto-strncpy/auto-strncpy_grate.c" \
    "" "no" \
    "/auto-strncpy-short.cwasm" \
    -- "[Cage|auto-strncpy-short] dst=\"\"" "[Grate|auto-strncpy] PASS" \
    --

# --------------------------------------------------------------------------
# Stage-3 marshalling tests
# --------------------------------------------------------------------------

# auto-cstr: LIND_SIZE_CSTR; intercepts strlen, returns len*2.
GRATE_EXTRA=()
run_test "auto-cstr" \
    "auto-cstr/auto-cstr.c" \
    "auto-cstr/auto-cstr_grate.c" \
    "" "no" \
    "/auto-cstr.cwasm" \
    -- "[Cage|auto-cstr] PASS: strlen(\"hello\") = 10 (intercepted as len*2)" \
    --

# auto-compress2: LIND_SIZE_FROM_ARG_POINTEE; the handler ignores the real
# source data and writes a fixed "LIND"/destLen=4, so no real zlib output
# could coincidentally match -- no extra dispatch evidence needed.
# Strict-safe: the cage calls only compress2 from libz.
GRATE_EXTRA=()
run_test "auto-compress2" \
    "auto-compress2/auto-compress2.c" \
    "auto-compress2/auto-compress2_grate.c" \
    "env=/lib/libz.so" "yes" \
    "/auto-compress2.cwasm" \
    -- "[Cage|auto-compress2] PASS: got \"LIND\" destLen=4" \
    --

# auto-memchr: LIND_RET_PTR_INTO_ARG; the handler calls the real memchr and
# the *value* found (offset 2) is by design identical to what an
# uninterposed call would report -- this test is verifying the shadow-to-
# source-cage pointer translation, not an output difference, so the grate's
# own dispatch trace is the only possible evidence and is required.
GRATE_EXTRA=()
run_test "auto-memchr" \
    "auto-memchr/auto-memchr.c" \
    "auto-memchr/auto-memchr_grate.c" \
    "" "no" \
    "/auto-memchr.cwasm" \
    -- "[Cage|auto-memchr] PASS: found 'l' at offset 2" \
    -- "[Grate|auto-memchr] memchr dispatched, offset=2"

# auto-handle: LIND_ARG_HANDLE + LIND_RET_HANDLE. The round-tripped value
# (42) is unavoidably identical either way by design (it's a correctness
# test of the handle table, not a wrong-value marker) -- the grate's own
# create/get/close trace lines are the only possible evidence and are
# required. Strict-safe: the cage calls only the 3 registered libtoy symbols.
GRATE_EXTRA=()
run_test "auto-handle" \
    "auto-handle/auto-handle.c" \
    "auto-handle/auto-handle_grate.c" \
    "env=/lib/libtoy.so" "yes" \
    "/auto-handle.cwasm" \
    -- "[Cage|auto-handle] PASS: create/get_val/close round-trip, val=42" \
    -- "[Grate|auto-handle] toy_ctx_create dispatched, val=42" \
       "[Grate|auto-handle] toy_ctx_get_val dispatched, val=42" \
       "[Grate|auto-handle] toy_ctx_close dispatched"

# auto-nested: nested struct layout; handler returns sum+1 instead of the
# real sum. Strict-safe: the cage calls only toy_buf_checksum from libtoy.
GRATE_EXTRA=()
run_test "auto-nested" \
    "auto-nested/auto-nested.c" \
    "auto-nested/auto-nested_grate.c" \
    "env=/lib/libtoy.so" "yes" \
    "/auto-nested.cwasm" \
    -- "[Cage|auto-nested] PASS: toy_buf_checksum = 199 (sum+1)" \
    --

# --------------------------------------------------------------------------
# Auto-generated (gen_grate.py-style) marshalling tests: a single generic
# ctx-dispatch pass_fptr_to_wt calling the REAL underlying function for
# every registered symbol -- every one of these needs dispatch evidence
# (the grate's own registration/trace lines), since the checked values are
# all real, correct results a completely uninterposed run would print too.
# --------------------------------------------------------------------------

# auto-argv: LIND_SIZE_PTR_ARRAY (NULL-terminated argv marshalling).
# Strict-safe: the cage calls only toy_argv_len from libtoy.
GRATE_EXTRA=("$SCRIPT_DIR/auto-argv/toy_argv_impl.c")
run_test "auto-argv" \
    "auto-argv/argv_app.c" \
    "auto-argv/argv_auto_grate.c" \
    "env=/lib/libtoy.so" "yes" \
    "/argv_app.cwasm" \
    -- "[argv-app] PASS: toy_argv_len(argv) = 11 (ptr_array marshalled)" \
    -- "[libtoy-grate] registered 1/1 handlers"

# auto-libz: broad libz surface (adler32/crc32/compress2/uncompress +
# force_local zlibVersion). NOT strict-safe: zlibVersion is deliberately
# un-registered and would trap under :interposed. Needs the sibling repo's
# static libz.a (see LIBZ_A above) -- skips gracefully if unavailable.
if [[ -f "$LIBZ_A" ]]; then
    GRATE_EXTRA=("$LIBZ_A")
    run_test "auto-libz" \
        "auto-libz/libz_app.c" \
        "auto-libz/libz_auto_grate.c" \
        "env=/lib/libz.so" "no" \
        "/libz_app.cwasm" \
        -- "  PASS  adler32  = 0x11e60398 (want 0x11e60398)" \
           "  PASS  crc32  = 0xcbf43926 (want 0xcbf43926)" \
           "  PASS  compress  rc=0, valid size" \
           "  PASS  uncompress  ru=0 back_len=90 (want 90, roundtrip)" \
           "[libz-app] 6 passed, 0 failed (of marshalled+local libz calls)" \
        -- "[libz-grate] registered 48/48 handlers"
else
    DECLARED_TESTS+=("auto-libz")
    skip_test "auto-libz" "libz.a not found at $LIBZ_A (needs a sibling lind-wasm-apps checkout with zlib built)"
fi

# auto-libz-spike: single-symbol adler32 mechanism-verification spike
# (documented as intentionally calling the real adler32). Strict-safe
# (single symbol). Also needs libz.a.
if [[ -f "$LIBZ_A" ]]; then
    GRATE_EXTRA=("$LIBZ_A")
    run_test "auto-libz-spike" \
        "auto-libz-spike/spike_cage.c" \
        "auto-libz-spike/spike_grate.c" \
        "env=/lib/libz.so" "yes" \
        "/spike_cage.cwasm" \
        -- "[Cage|spike] PASS: adler32(\"Wikipedia\")=0x11e60398" \
        -- "[Grate|spike] dispatch returned 0x11e60398"
else
    DECLARED_TESTS+=("auto-libz-spike")
    skip_test "auto-libz-spike" "libz.a not found at $LIBZ_A (needs a sibling lind-wasm-apps checkout with zlib built)"
fi

# auto-libc: 8-symbol libc string-function surface (strlen/strnlen/memcmp/
# strcmp/strncmp/memchr/strchr/strtol), exercising out_ptr_into_arg1
# (strtol's endptr) and ptr_into_arg (strchr/memchr) on top of the simpler
# specs the other tests cover. No extra --preload: the wrapper's built-in
# libc.cwasm preload already provides these symbols.
GRATE_EXTRA=()
run_test "auto-libc" \
    "auto-libc/libc_app.c" \
    "auto-libc/libc_auto_grate.c" \
    "" "no" \
    "/libc_app.cwasm" \
    -- "  PASS  strlen  = 11 (want 11)" \
       "  PASS  strnlen  = 5 (want 5)" \
       "  PASS  memcmp  eq=0 lt=-1" \
       "  PASS  strcmp  eq=0 lt=-1" \
       "  PASS  strncmp  = 0 (want 0)" \
       "  PASS  memchr  off=6 (want 6, ptr-into-arg)" \
       "  PASS  strchr  off=4 (want 4, ptr-into-arg)" \
       "  PASS  strtol(endptr)  v=12345 endoff=5 (want 12345, 5)" \
       "[libc-app] 8 passed, 0 failed" \
    -- "[libc-grate] registered 8/8 handlers"

# --------------------------------------------------------------------------
# fail-closed: lind_marshal.h's fail-closed paths (issue #6). Each mode
# below deliberately triggers one marshalling failure against a REAL
# interposed function (memcpy/strlen from libc, toy_ctx_get_val/
# toy_buf_checksum/toy_argv_len from libtoy) and expects the call to be
# rejected -- the grate's own handler must never run, and the caller sees
# threei::GRATE_ERR rather than a real (or foreign-pointer-derived) result.
# "ok" is the control: the same shapes with valid arguments, proving these
# are genuine rejections and not a broken/always-failing grate.
# The libtoy preload is marked :interposed (strict mode): the cage touches
# only the 3 registered libtoy symbols, so this also directly demonstrates
# criterion #7 (rejection under strict mode, not a silent local/mixed-mode
# fallback).
#
# Each rejection mode also FORBIDS its handler's own "handler ran" marker.
# Without that, a marshaller bug that wrongly invokes the handler -- which
# then dereferences the foreign/corrupt data itself and traps -- would
# produce the exact same caller-visible GRATE_ERR as a correct rejection,
# passing the test despite the isolation violation actually happening.
# --------------------------------------------------------------------------
for mode_desc in \
    "arena:memcpy" \
    "overflow:memcpy" \
    "badcopy:memcpy" \
    "unsized:strlen" \
    "handle:toy_ctx_get_val" \
    "nested:toy_buf_checksum" \
    "argv:toy_argv_len"
do
    mode="${mode_desc%%:*}"
    fn="${mode_desc#*:}"
    GRATE_EXTRA=("$SCRIPT_DIR/custom-lib/libtoy.c")
    run_test "fail-closed-$mode" \
        "fail-closed/failclosed_cage.c" \
        "fail-closed/failclosed_grate.c" \
        "env=/lib/libtoy.so" "yes" \
        "/failclosed_cage.cwasm" "$mode" \
        -- "[Cage|fail-closed] PASS: $mode rejected (GRATE_ERR)" \
        -- \
        -- "[Grate|fail-closed] $fn handler ran (should not happen)"
done

# Control: the same shapes with valid arguments, proving the rejections
# above are genuine and not a broken/always-failing grate. Here the
# handler running is the expected/desired behavior, not forbidden.
GRATE_EXTRA=("$SCRIPT_DIR/custom-lib/libtoy.c")
run_test "fail-closed-ok" \
    "fail-closed/failclosed_cage.c" \
    "fail-closed/failclosed_grate.c" \
    "env=/lib/libtoy.so" "yes" \
    "/failclosed_cage.cwasm" "ok" \
    -- "[Cage|fail-closed] PASS: ok completed normally" \
    -- "[Grate|fail-closed] memcpy handler ran (should not happen)"

# Additional adversarial cases: bounds checks on spec-provided indices
# (badspec_grate.c registers real functions with deliberately out-of-range
# size_arg_index values, at both the top level and inside a nested struct
# field) and pointer-field-tracking allocation exhaustion (ptfexhaust_grate.c,
# a 16-byte arena that fits the struct shadow but not the tracking array).
# Separate grates from failclosed_grate.c to avoid symbol-registration
# collisions on toy_buf_checksum (see comments in each grate file).
GRATE_EXTRA=("$SCRIPT_DIR/custom-lib/libtoy.c")
run_test "fail-closed-topindex" \
    "fail-closed/badspec_cage.c" \
    "fail-closed/badspec_grate.c" \
    "env=/lib/libtoy.so" "yes" \
    "/badspec_cage.cwasm" "topindex" \
    -- "[Cage|badspec] PASS: topindex rejected (GRATE_ERR)" \
    -- \
    -- "[Grate|badspec] memmove handler ran (should not happen)"

GRATE_EXTRA=("$SCRIPT_DIR/custom-lib/libtoy.c")
run_test "fail-closed-nestedindex" \
    "fail-closed/badspec_cage.c" \
    "fail-closed/badspec_grate.c" \
    "env=/lib/libtoy.so" "yes" \
    "/badspec_cage.cwasm" "nestedindex" \
    -- "[Cage|badspec] PASS: nestedindex rejected (GRATE_ERR)" \
    -- \
    -- "[Grate|badspec] toy_buf_checksum handler ran (should not happen)"

# fail-closed-badnargs: toy_mul's real signature is 2 raw ABI slots (well
# within the transport's 6-slot capacity, so it links normally), but its
# spec deliberately claims .nargs=7 -- lind_marshal_dispatch's own bound on
# spec->nargs must reject this itself, since the linker-level check (which
# only sees the real function's wasm type) cannot.
GRATE_EXTRA=("$SCRIPT_DIR/custom-lib/libtoy.c")
run_test "fail-closed-badnargs" \
    "fail-closed/badspec_cage.c" \
    "fail-closed/badspec_grate.c" \
    "env=/lib/libtoy.so" "yes" \
    "/badspec_cage.cwasm" "badnargs" \
    -- "[Cage|badspec] PASS: badnargs rejected (GRATE_ERR)" \
    -- \
    -- "[Grate|badspec] toy_mul handler ran (should not happen)"

GRATE_EXTRA=("$SCRIPT_DIR/custom-lib/libtoy.c")
run_test "fail-closed-ptfexhaust" \
    "fail-closed/ptfexhaust_cage.c" \
    "fail-closed/ptfexhaust_grate.c" \
    "env=/lib/libtoy.so" "yes" \
    "/ptfexhaust_cage.cwasm" \
    -- "[Cage|ptfexhaust] PASS: rejected (GRATE_ERR)" \
    -- \
    -- "[Grate|ptfexhaust] toy_buf_checksum handler ran (should not happen)"

# fail-closed-widesig: toy_wide_sum7 has 7 raw wasm32 ABI slots -- one more
# than the interposition transport's 6-slot capacity. This is rejected at
# link/portal-install time (linker.rs), before any grate delegation, so the
# cage traps on the call itself and can never report its own PASS/FAIL; the
# parent grate judges success from the cage's exit status instead (see
# widesig_grate.c). Distinct from the topindex/nestedindex/ptfexhaust cases
# above, which are rejected inside an already-installed portal's dispatch.
#
# The evidence line requires LIND_LOG_OUTPUT=stderr (set above): without it,
# a bare nonzero cage exit could also mean an unrelated crash, and the test
# would pass for the wrong reason.
GRATE_EXTRA=("$SCRIPT_DIR/custom-lib/libtoy.c")
run_test "fail-closed-widesig" \
    "fail-closed/widesig_cage.c" \
    "fail-closed/widesig_grate.c" \
    "env=/lib/libtoy.so" "yes" \
    "/widesig_cage.cwasm" \
    -- "[Grate|widesig] registered 1/1 handlers" "[Grate|widesig] PASS: rejected (cage crashed as expected, exit=1)" \
    -- "    1: lib-3i portal: env.toy_wide_sum7 has 7 raw ABI argument slots, exceeding the interposition transport's 6-slot capacity" \
    -- "[Cage|widesig] FAIL: call returned normally" "[Grate|widesig] toy_wide_sum7 handler ran (should not happen)"

# fail-closed-provenance-*: post-call pointer-provenance validation (issue
# #7). provenance_grate.c's three handlers deliberately fabricate the values
# a buggy or adversarial interposed library might leave behind -- before the
# shadow's base, past its one-past-the-end, wildly out of range, and
# pointing at a real but unrelated grate address -- across the three
# post-call translation paths (LIND_RET_PTR_INTO_ARG, out_ptr_into_arg1, and
# struct-field OUT/cursor copy-back+fixup). Each target loops over every one
# of its modes in a single cage invocation and reports one aggregate PASS
# line; a _lind_marshal_abort trap only fails the one call it's in (see
# threei::GRATE_ERR's doc), so invalid modes don't stop the cage from
# reaching its later, valid ones. The per-mode "handler ran" lines are
# evidence every mode actually executed, not skipped.
GRATE_EXTRA=("$SCRIPT_DIR/custom-lib/libtoy.c")
run_test "fail-closed-provenance-scan" \
    "fail-closed/provenance_cage.c" \
    "fail-closed/provenance_grate.c" \
    "env=/lib/libtoy.so" "yes" \
    "/provenance_cage.cwasm" "scan" \
    -- "[Grate|provenance] registered 3/3 handlers" "[Cage|provenance] PASS: scan (all 8 modes)" \
    -- "[Grate|provenance] toy_scan_buf handler ran, mode=0" \
       "[Grate|provenance] toy_scan_buf handler ran, mode=1" \
       "[Grate|provenance] toy_scan_buf handler ran, mode=2" \
       "[Grate|provenance] toy_scan_buf handler ran, mode=3" \
       "[Grate|provenance] toy_scan_buf handler ran, mode=4" \
       "[Grate|provenance] toy_scan_buf handler ran, mode=5" \
       "[Grate|provenance] toy_scan_buf handler ran, mode=6" \
       "[Grate|provenance] toy_scan_buf handler ran, mode=7"

GRATE_EXTRA=("$SCRIPT_DIR/custom-lib/libtoy.c")
run_test "fail-closed-provenance-strtol" \
    "fail-closed/provenance_cage.c" \
    "fail-closed/provenance_grate.c" \
    "env=/lib/libtoy.so" "yes" \
    "/provenance_cage.cwasm" "strtol" \
    -- "[Grate|provenance] registered 3/3 handlers" "[Cage|provenance] PASS: strtol (all 8 modes)" \
    -- "[Grate|provenance] toy_strtol_like handler ran, mode='0'" \
       "[Grate|provenance] toy_strtol_like handler ran, mode='1'" \
       "[Grate|provenance] toy_strtol_like handler ran, mode='2'" \
       "[Grate|provenance] toy_strtol_like handler ran, mode='3'" \
       "[Grate|provenance] toy_strtol_like handler ran, mode='4'" \
       "[Grate|provenance] toy_strtol_like handler ran, mode='5'" \
       "[Grate|provenance] toy_strtol_like handler ran, mode='6'" \
       "[Grate|provenance] toy_strtol_like handler ran, mode='7'"

GRATE_EXTRA=("$SCRIPT_DIR/custom-lib/libtoy.c")
run_test "fail-closed-provenance-stream" \
    "fail-closed/provenance_cage.c" \
    "fail-closed/provenance_grate.c" \
    "env=/lib/libtoy.so" "yes" \
    "/provenance_cage.cwasm" "stream" \
    -- "[Grate|provenance] registered 3/3 handlers" "[Cage|provenance] PASS: stream (all 7 modes)" \
    -- "[Grate|provenance] toy_stream_process handler ran, mode=0" \
       "[Grate|provenance] toy_stream_process handler ran, mode=1" \
       "[Grate|provenance] toy_stream_process handler ran, mode=2" \
       "[Grate|provenance] toy_stream_process handler ran, mode=3" \
       "[Grate|provenance] toy_stream_process handler ran, mode=4" \
       "[Grate|provenance] toy_stream_process handler ran, mode=5" \
       "[Grate|provenance] toy_stream_process handler ran, mode=6"

DECLARED_TESTS+=("fail-closed")

# --------------------------------------------------------------------------
# fail-registration: a grate must abort startup rather than exec the cage
# with an incomplete handler table. One registration is deliberately given
# a NULL symbol name (register_lib_handler rejects it); the grate must
# detect the failure and abort before execv. The payload path
# (/custom-lib.cwasm) is a placeholder reused from an existing test binary
# and is never actually run -- if it were, that would itself be the bug
# this test exists to catch.
# --------------------------------------------------------------------------
GRATE_EXTRA=()
run_test "fail-registration" \
    "custom-lib/custom-lib.c" \
    "fail-registration/failreg_grate.c" \
    "" "no" \
    "/custom-lib.cwasm" \
    -- "[Grate|fail-registration] register (null symbol) failed: -1" \
       "[Grate|fail-registration] registered 1/2 handlers" \
       "[Grate|fail-registration] FATAL: 1 handler registration(s) failed, aborting startup" \
    --

# --------------------------------------------------------------------------
# Completeness check: every directory with a *_grate.c must be declared
# above. full-libc/ and full-libm/ are separate, much larger suites with
# their own dedicated runners and are intentionally excluded here.
# --------------------------------------------------------------------------
echo ""
missing_from_manifest=()
while IFS= read -r dir; do
    missing_from_manifest+=("$dir")
done < <(find_undeclared_dirs "$SCRIPT_DIR" "${DECLARED_TESTS[@]}")

if [[ ${#missing_from_manifest[@]} -gt 0 ]]; then
    echo "MANIFEST ERROR: found *_grate.c in these directories but run_tests.sh"
    echo "does not declare a test for them (add a run_test call, or an explicit"
    echo "skip_test with a reason, above):"
    printf '  - %s\n' "${missing_from_manifest[@]}"
    FAIL=$((FAIL + ${#missing_from_manifest[@]}))
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
if [[ -n "${CATEGORY_COUNTS[*]+x}" ]]; then
    echo "Failure categories:"
    for cat in "${!CATEGORY_COUNTS[@]}"; do
        echo "  $cat: ${CATEGORY_COUNTS[$cat]}"
    done
fi
if [[ $FAIL -gt 0 ]]; then
    echo "Failed tests: ${FAILURES[*]}"
    exit 1
fi
if [[ $SKIP -gt 0 && "$ALLOW_SKIPS" != "yes" ]]; then
    echo "$SKIP maintained test(s) were skipped -- not a pass for gating/CI purposes."
    echo "Pass --allow-skips to accept this for an explicitly optional local run."
    exit 1
fi
