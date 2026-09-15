#!/usr/bin/env python3
"""Recompute a library's interposition coverage across the pipeline's stages,
and enforce the OpenBLAS-specific gates issue #26/#27's follow-up requires.

Defaults to OpenBLAS (issue #5's motivating case: 106+ of ~200 functions too
wide for the transport at the time it was filed). Reports, for any
<lib>.marshal.json produced by an infer_*.sh script:
  discovered        -- functions marshal-infer examined at all
  inferred          -- functions marshal-infer assigned a decision to (should
                        equal discovered; a mismatch would itself be a bug)
  transport accepted -- functions whose raw ABI slot count fits the
                        interposition transport's LIND_RAW_ARGS_MAX (whether
                        or not they end up "marshal" for some OTHER reason)
  generated         -- functions gen_grate.py would actually emit a handler
                        for (marshal-decision AND is_marshalable())
  exercised         -- functions covered by a passing focused test today

"args" in the JSON is already one entry per raw wasm-level ABI slot (sret and
multi-slot params are pre-flattened by marshal-infer -- see Infer.cpp's
lowerAbiReturn/inferFunction), so len(f["args"]) IS the raw slot count.

marshal-infer's own --config coverage.min_marshal_count/min_marshal_pct gate
(see CONFIG.md) counts decision=="marshal" records -- a necessary but NOT
sufficient condition for a generated handler to exist (a "marshal" record
with, say, an unsupported return kind or a malformed contract operand
produces no handler at all -- see gen_grate.py's unmarshalable_reason()).
openblas.marshal.json has carried exactly this gap (42 marshal decisions, 40
generated handlers) with nothing to catch it. This script is the downstream
gate that actually matters for "can this be used": it measures GENERATED
handlers, additionally requires a fixed set of OpenBLAS symbols to be among
them, and reports each generated handler's confidence.

Exit code is nonzero iff any enforced gate fails (a missing required symbol
or a regression below GENERATED_FLOOR) -- meant to be wired into a test
suite's pre-flight, same role as check_raw_arg_slot_consistency.sh.

Usage: tools/marshal-infer/openblas_coverage.py [lib.marshal.json]
Default input: <repo>/openblas.marshal.json
"""
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "marshal-gen"))
from gen_grate import is_marshalable, unmarshalable_reason, LIND_RAW_ARGS_MAX  # noqa: E402

_SLOT_WARNING_RE = re.compile(r"needs (\d+) raw ABI slots")

# GENERATED_FLOOR is a REGRESSION floor pinned to what generation already
# achieves today (40), not a target padded down "for safety margin" -- any
# drop below it would let a silent generated-handler regression slip back
# through unnoticed. GENERATED_TARGET records the actual goal instead:
# roughly 80 of the 89 functions the transport can carry at all (see
# "transport accepted" above). Closing that gap is inference-side work
# (recognizing more of OpenBLAS's real loop shapes as sound proofs, or an
# analysis-friendlier compile profile for the library -- see PATTERNS.md --
# never a guess at unproven compiler output); this script can only report
# it, not close it, so it's a NOTE below, not a failure.
GENERATED_FLOOR = 40
GENERATED_TARGET = 80

# Symbols this project's OpenBLAS interposition work is required to cover --
# the two precisions of axpy/scal in both their CBLAS and classic
# Fortran-BLAS-name forms. A missing one fails the gate regardless of the
# aggregate GENERATED_FLOOR count: a library could stay above the floor by
# generating 48 handlers for the WRONG 48 functions.
REQUIRED_SYMBOLS = (
    "cblas_saxpy", "cblas_daxpy", "saxpy_", "daxpy_",
    "cblas_sscal", "cblas_dscal", "sscal_", "dscal_",
)

# Function-level confidence is its least-trusted marshalled argument. Only
# StrideVector arguments currently carry a per-argument "confidence" field
# (see CONFIG.md's "Confidence model") -- every other marshalled shape
# (const size, cstr, ptr_array, a scalar/handle passthrough, ...) is exact by
# construction, with no proof to grade, and is implicitly "proven".
CONFIDENCE_ORDER = ("proven", "configured")


def exceeds_slot_cap(f):
    """True iff this function needs more raw ABI slots than the transport
    allows. force_local functions carry no "args" (inference clears it, since
    gen_grate.py never needs a per-arg spec for one), so the slot count for
    those has to come from enforceRawArgSlotCap's own warning text instead of
    len(args)."""
    if "args" in f:
        return len(f["args"]) > LIND_RAW_ARGS_MAX
    for w in f.get("warnings", []):
        m = _SLOT_WARNING_RE.search(w)
        if m:
            return int(m.group(1)) > LIND_RAW_ARGS_MAX
    return False


def function_confidence(f):
    best = "proven"
    for a in f.get("args", []):
        c = a.get("confidence")
        if c in CONFIDENCE_ORDER and CONFIDENCE_ORDER.index(c) > CONFIDENCE_ORDER.index(best):
            best = c
    return best


def main():
    path = Path(sys.argv[1]) if len(sys.argv) > 1 else REPO_ROOT / "openblas.marshal.json"
    d = json.load(open(path))
    fns = d["functions"]
    by_name = {f["name"]: f for f in fns}

    discovered = len(fns)
    inferred = sum(1 for f in fns if f.get("decision") is not None)
    transport_accepted = sum(1 for f in fns if not exceeds_slot_cap(f))
    marshal_decision = [f for f in fns if f.get("decision") == "marshal"]
    generated = [f for f in marshal_decision if is_marshalable(f)]
    dropped = [(f["name"], unmarshalable_reason(f))
               for f in marshal_decision if not is_marshalable(f)]

    print(f"Interposition coverage ({path.name}):")
    print(f"  discovered:         {discovered}")
    print(f"  inferred:           {inferred}")
    print(f"  transport accepted: {transport_accepted} "
          f"(raw ABI slots <= {LIND_RAW_ARGS_MAX})")
    print(f"  marshal decisions:  {len(marshal_decision)}")
    print(f"  generated handlers: {len(generated)} (decision=marshal AND is_marshalable()) "
          f"-- regression floor {GENERATED_FLOOR}, target ~{GENERATED_TARGET}")
    print(f"  exercised:          0 (no grate-generation/test pipeline exists "
          f"yet for {path.name}'s FULL surface -- tests/grate-tests/lib-interpose/"
          f"auto-openblas-daxpy exercises cblas_daxpy/daxpy_ specifically; "
          f"generating and running a grate for the rest is future work)")

    if dropped:
        print(f"\n{len(dropped)} marshal-decision record(s) produced NO generated handler:")
        for dname, reason in sorted(dropped):
            print(f"  - {dname}: {reason}")

    by_conf = {c: [] for c in CONFIDENCE_ORDER}
    for f in generated:
        by_conf[function_confidence(f)].append(f["name"])
    print("\nGenerated handlers by confidence (function-level = least-trusted marshalled argument):")
    for c in CONFIDENCE_ORDER:
        print(f"  {c:10s}: {len(by_conf[c])}")
    print(f"  {'combined':10s}: {len(generated)}")

    ok = True

    missing = []
    for sym in REQUIRED_SYMBOLS:
        f = by_name.get(sym)
        if f is None:
            missing.append((sym, "not present in this JSON at all"))
        elif f.get("decision") != "marshal":
            missing.append((sym, f"decision={f.get('decision')!r}, not \"marshal\""))
        elif not is_marshalable(f):
            missing.append((sym, unmarshalable_reason(f)))
    if missing:
        ok = False
        print(f"\nFAIL: {len(missing)}/{len(REQUIRED_SYMBOLS)} required OpenBLAS "
              f"symbol(s) have no generated handler:")
        for sym, reason in missing:
            print(f"  - {sym}: {reason}")
    else:
        print(f"\nOK: all {len(REQUIRED_SYMBOLS)} required OpenBLAS symbols "
              f"have a generated handler.")

    if len(generated) < GENERATED_FLOOR:
        ok = False
        print(f"\nFAIL: {len(generated)} generated handlers is below the "
              f"regression floor of {GENERATED_FLOOR}.")
    elif len(generated) < GENERATED_TARGET:
        print(f"\nNOTE: {len(generated)}/{GENERATED_TARGET} of the intended "
              f"target reached ({transport_accepted} transport-eligible total) "
              f"-- not a failure; closing this gap is inference-side work "
              f"(more contracts, or recognizing more of OpenBLAS's real loop "
              f"shapes as sound proofs -- see PATTERNS.md), tracked "
              f"separately.")

    wide = discovered - transport_accepted
    print(f"\n{wide} of {discovered} functions exceed the "
          f"{LIND_RAW_ARGS_MAX}-slot transport capacity and are correctly "
          f"rejected (force_local) rather than marshalled.")

    print(f"\n{'PASS' if ok else 'FAIL'}: OpenBLAS coverage/required-symbol gate")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
