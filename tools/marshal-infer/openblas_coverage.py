#!/usr/bin/env python3
"""Recompute a library's interposition coverage across the pipeline's stages,
and enforce the OpenBLAS-specific gates issue #26/#27's follow-up requires.

Defaults to OpenBLAS (issue #5's motivating case: 106+ of ~200 functions too
wide for the transport at the time it was filed). Reports, for any
<lib>.marshal.json produced by an infer_*.sh script:
  discovered        -- functions marshal-infer examined at all
  inferred          -- functions marshal-infer assigned a decision to (should
                        equal discovered; a mismatch would itself be a bug)
  V1 transport accepted -- functions whose raw ABI slot count fits V1's fixed
                        LIND_RAW_ARGS_MAX (whether or not they end up
                        "marshal" for some OTHER reason)
  generated (V1)    -- functions gen_grate.py would actually emit a V1
                        handler for (marshal-decision AND is_marshalable())
  generated (V2)    -- marshal-decision functions wider than V1's cap that
                        gen_v2_adapter.py can still generate as variable-width
                        adapters; marshal-infer records width as transport
                        metadata rather than treating it as a semantic failure
  exercised (V1/V2) -- functions with a REAL passing end-to-end grate test
                        today, reported separately per transport (see
                        EXERCISED_V1_SYMBOLS/EXERCISED_V2_SYMBOLS below, so
                        generated coverage is never confused with execution)

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
# achieves today (64, after peeled-first-iteration recovery and the
# all-access correctness fix), not a target padded down "for safety
# margin" -- any drop below it would let a silent generated-handler regression slip back
# through unnoticed. GENERATED_TARGET records the actual goal instead:
# roughly 80 of the 89 functions the transport can carry at all (see
# "transport accepted" above). Closing that gap is inference-side work
# (recognizing more of OpenBLAS's real loop shapes as sound proofs, or an
# analysis-friendlier compile profile for the library, never a guess at
# unproven compiler output); this script can only report
# it, not close it, so it's a NOTE below, not a failure.
GENERATED_FLOOR = 64
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

# Symbols with a REAL, passing, end-to-end focused test TODAY (not derivable
# from the JSON itself -- "exercised" means a real compiled-and-run grate
# proved the generated/adapted handler correct, which requires an actual C
# test fixture, not just a sound marshal-infer decision). Kept as an explicit
# list, not a count, so this can
# never silently drift from what tests/grate-tests/lib-interpose/run_tests.sh
# actually runs.
#
# V1's own exercise (auto-openblas-daxpy) is against libblastoy.c, a
# hand-written stand-in sharing OpenBLAS's real exported symbol names/wasm32
# ABI shapes, NOT the real statically-linked libopenblas.a -- see that
# fixture's own comment for why. It proves V1 generation + dispatch/
# marshalling machinery, not real OpenBLAS numerical behavior. Only
# cblas_daxpby/daxpby_ (V2, auto-openblas-v2wide-real / -fortran-real) are
# exercised against the REAL archive, with a same-cage numeric baseline --
# do not conflate the two when reporting "real OpenBLAS execution".
EXERCISED_V1_SYMBOLS = (
    "cblas_daxpy", "daxpy_",  # auto-openblas-daxpy (toy-backed, NOT the real archive)
)
EXERCISED_V2_SYMBOLS = (
    "cblas_daxpby", "daxpby_",  # auto-openblas-v2wide-real / -fortran-real (REAL libopenblas.a)
)

# Function-level confidence is its least-trusted marshalled argument. Only
# StrideVector arguments currently carry a per-argument "confidence" field
# (see CONFIG.md's "Confidence model") -- every other marshalled shape
# (const size, cstr, ptr_array, a scalar/handle passthrough, ...) is exact by
# construction, with no proof to grade, and is implicitly "proven".
CONFIDENCE_ORDER = ("proven", "configured")


def exceeds_slot_cap(f):
    """True iff this function needs more raw ABI slots than V1's transport
    allows. force_local functions carry no "args" (inference clears it, since
    gen_grate.py never needs a per-arg spec for one), so the slot count for
    those has to come from annotateWideRawArgSlots' own warning text instead
    of len(args). A "marshal"-decision function CAN be this wide now (V1
    stays capped; V2 has no cap -- see unmarshalable_reason's max_args), so
    this is purely a WIDTH question, independent of decision."""
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
    # Each function's V1/V2 eligibility, computed once (unmarshalable_reason
    # prints on rejection when warn=True; is_marshalable passes warn=True, so
    # calling it more than once per function would print duplicate lines).
    # V2 (variable-width) eligibility is checked only for V1-ineligible
    # functions -- a V1-eligible one is already counted as generated and
    # gen_v2_adapter.py has no reason to duplicate it.
    classified = [(f, is_marshalable(f)) for f in marshal_decision]
    generated = [f for f, v1_ok in classified if v1_ok]
    v1_ineligible = [(f, unmarshalable_reason(f, max_args=None))
                      for f, v1_ok in classified if not v1_ok]
    v2_generated = [f for f, v2_reason in v1_ineligible if v2_reason is None]
    dropped = [(f["name"], unmarshalable_reason(f))
               for f, v2_reason in v1_ineligible if v2_reason is not None]

    print(f"Interposition coverage ({path.name}):")
    print(f"  discovered:            {discovered}")
    print(f"  inferred:              {inferred}")
    print(f"  V1 transport accepted: {transport_accepted} "
          f"(raw ABI slots <= {LIND_RAW_ARGS_MAX})")
    print(f"  marshal decisions:     {len(marshal_decision)}")
    print(f"  generated handlers (V1): {len(generated)} (decision=marshal AND is_marshalable()) "
          f"-- regression floor {GENERATED_FLOOR}, target ~{GENERATED_TARGET}")
    print(f"  generated handlers (V2): {len(v2_generated)} (marshal, wider than V1's "
          f"{LIND_RAW_ARGS_MAX}-slot cap, still gen_v2_adapter.py-eligible)")
    # V1/V2 eligibility is mutually exclusive by construction (v2_generated
    # is drawn only from the V1-ineligible subset), so len(V1)+len(V2)
    # already IS the unique count -- stated explicitly anyway so a future
    # change that makes the two sets overlap can't silently under/over-count
    # here without this line visibly needing a real union instead of a sum.
    combined_generated = generated + v2_generated
    print(f"  generated handlers (combined, unique): {len(combined_generated)}")
    exercised_v1 = [s for s in EXERCISED_V1_SYMBOLS if by_name.get(s, {}).get("decision") == "marshal"]
    exercised_v2 = [s for s in EXERCISED_V2_SYMBOLS if by_name.get(s, {}).get("decision") == "marshal"]
    print(f"  exercised (V1, toy-backed libblastoy.c, NOT the real archive): "
          f"{len(exercised_v1)}/{len(EXERCISED_V1_SYMBOLS)} "
          f"({', '.join(exercised_v1) or 'none'} -- tests/grate-tests/lib-interpose/"
          f"auto-openblas-daxpy)")
    print(f"  exercised (V2, REAL libopenblas.a, same-cage baseline-verified): "
          f"{len(exercised_v2)}/{len(EXERCISED_V2_SYMBOLS)} "
          f"({', '.join(exercised_v2) or 'none'} -- tests/grate-tests/lib-interpose/"
          f"auto-openblas-v2wide-real, -fortran-real)")
    print(f"  (exercised = a real compiled-and-run grate test passes for that exact "
          f"symbol TODAY, not merely a sound marshal-infer decision; the remaining "
          f"{len(combined_generated) - len(exercised_v1) - len(exercised_v2)} "
          f"generated-but-unexercised handlers are inferred/generated correctly but have "
          f"no dedicated end-to-end test yet)")

    if v2_generated:
        print(f"\n{len(v2_generated)} marshal-decision record(s) exceed V1's "
              f"{LIND_RAW_ARGS_MAX}-slot cap but ARE gen_v2_adapter.py-eligible "
              f"(expected: V2 is the variable-width transport):")
        for f in sorted(v2_generated, key=lambda f: f["name"]):
            print(f"  - {f['name']} ({len(f['args'])} raw ABI slots)")

    if dropped:
        print(f"\n{len(dropped)} marshal-decision record(s) produced NO generated "
              f"handler under EITHER transport:")
        for dname, reason in sorted(dropped):
            print(f"  - {dname}: {reason}")

    by_conf = {c: [] for c in CONFIDENCE_ORDER}
    for f in combined_generated:
        by_conf[function_confidence(f)].append(f["name"])
    print("\nGenerated handlers by confidence (V1+V2 combined; function-level = "
          "least-trusted marshalled argument):")
    for c in CONFIDENCE_ORDER:
        print(f"  {c:10s}: {len(by_conf[c])}")
    print(f"  {'total':10s}: {len(combined_generated)}")

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
              f"shapes as sound proofs), tracked "
              f"separately.")

    wide = discovered - transport_accepted
    wide_marshal = sum(1 for f in fns if f.get("decision") == "marshal" and exceeds_slot_cap(f))
    wide_force_local = wide - wide_marshal
    print(f"\n{wide} of {discovered} functions exceed V1's {LIND_RAW_ARGS_MAX}-slot "
          f"transport capacity: {wide_marshal} are marshal-eligible for the V2 "
          f"transport, {wide_force_local} remain force_local for other (semantic) "
          f"reasons.")

    print(f"\n{'PASS' if ok else 'FAIL'}: OpenBLAS coverage/required-symbol gate")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
