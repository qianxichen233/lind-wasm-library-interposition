# Pure helper functions shared by run_tests.sh and selftest.sh. No side
# effects, no Lind execution -- safe to source standalone for unit testing.

# category_for <output> <exit_code>
# Classifies a failure from its combined stdout+stderr, in priority order
# (most specific/actionable first).
category_for() {
    local output="$1" exit_code="$2"
    if [[ "$exit_code" -eq 124 ]]; then
        echo "timeout"; return
    fi
    if echo "$output" | grep -q "COMPILE_STEP_FAILED"; then
        echo "build"; return
    fi
    if echo "$output" | grep -qE "failed to register grate workers|failed to create worker|register_lib_handler"; then
        echo "registration"; return
    fi
    if echo "$output" | grep -qE "failed to run main module|failed to instantiate|unknown import|has not been defined"; then
        echo "startup"; return
    fi
    if echo "$output" | grep -qE "wasm trap|unreachable|Trap\("; then
        echo "trap"; return
    fi
    if echo "$output" | grep -qE "FAIL:|Assertion.*failed|assert"; then
        echo "assertion"; return
    fi
    echo "semantic"
}

# match_ordered_lines <output> <line1> [line2 ...]
# True iff every given line appears as a COMPLETE line of `output` (not a
# substring match against the whole blob -- "PASS  compress" must not match
# inside "PASS  compressBound"), each found at or after the previous match's
# position (so lines out of order are also caught). Prints any unmatched
# lines (in the order given) to stdout on failure.
match_ordered_lines() {
    local output="$1"; shift
    local -a out_lines
    mapfile -t out_lines <<<"$output"
    local pos=0 line i found
    local -a missing=()
    for line in "$@"; do
        found=0
        for ((i = pos; i < ${#out_lines[@]}; i++)); do
            if [[ "${out_lines[$i]}" == "$line" ]]; then
                pos=$((i + 1))
                found=1
                break
            fi
        done
        [[ $found -eq 0 ]] && missing+=("$line")
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
        printf '%s\n' "${missing[@]}"
        return 1
    fi
    return 0
}

# find_forbidden_lines <output> <line1> [line2 ...]
# Prints (and fails) any given line that DOES appear as a complete line of
# `output`, anywhere (no ordering/position requirement -- unlike
# match_ordered_lines, this is an absence check, not a presence check).
# Use this to prove a code path was never reached at all: e.g. a marshaller
# rejection test must forbid the handler's own "handler ran" marker, or a
# handler that runs on a value the marshaller should have rejected (and
# then itself traps on it, producing the same caller-visible failure as a
# correct rejection) would otherwise pass undetected.
find_forbidden_lines() {
    local output="$1"; shift
    local -a out_lines
    mapfile -t out_lines <<<"$output"
    local line ol found
    local -a present=()
    for line in "$@"; do
        found=0
        for ol in "${out_lines[@]}"; do
            if [[ "$ol" == "$line" ]]; then
                found=1
                break
            fi
        done
        [[ $found -eq 1 ]] && present+=("$line")
    done
    if [[ ${#present[@]} -gt 0 ]]; then
        printf '%s\n' "${present[@]}"
        return 1
    fi
    return 0
}

# decide_outcome <exit_code> <missing_count>
# Prints "pass" or "fail". A nonzero exit_code fails regardless of
# missing_count -- printing every expected line (PASS included) does not
# excuse a nonzero exit.
decide_outcome() {
    local exit_code="$1" missing_count="$2"
    if [[ "$exit_code" -ne 0 ]]; then
        echo "fail"
    elif [[ "$missing_count" -gt 0 ]]; then
        echo "fail"
    else
        echo "pass"
    fi
}

# find_undeclared_dirs <search_dir> <declared_name1> [declared_name2 ...]
# Prints the name of every immediate subdirectory of search_dir that
# contains a *_grate.c file (other than full-libc/full-libm, which have
# their own dedicated runners) and is not among the declared names.
find_undeclared_dirs() {
    local search_dir="$1"; shift
    local -a declared=("$@")
    local grate_file dir found d
    while IFS= read -r -d '' grate_file; do
        dir="$(basename "$(dirname "$grate_file")")"
        case "$dir" in
            full-libc|full-libm) continue ;;
        esac
        found=0
        for d in "${declared[@]}"; do
            [[ "$d" == "$dir" ]] && { found=1; break; }
        done
        [[ $found -eq 0 ]] && echo "$dir"
    done < <(find "$search_dir" -mindepth 2 -maxdepth 2 -name '*_grate.c' -print0) | sort -u
}
