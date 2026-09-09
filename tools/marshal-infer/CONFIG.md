# marshal-infer config files

Tracking issue: [lind-wasm-library-interposition#27](https://github.com/qianxichen233/lind-wasm-library-interposition/issues/27).

An optional `--config <file.json>` file gives a library a checked-in,
versioned, reviewable place to hold analysis knobs, human-asserted
per-argument overrides, and coverage expectations — instead of those
choices living as magic numbers in a shell script, or (issue #26's actual
failure mode) nowhere at all, so a soundness fix elsewhere in the tool can
silently swing a library's marshal count with nothing to notice.

Omitting `--config` entirely reproduces marshal-infer's built-in defaults
exactly — this is not a required file.

## Precedence

1. **Built-in sound defaults**, compiled into marshal-infer (e.g.
   `max_type_depth=6`, `max_delegation_hops=1`).
2. **This config file**, if `--config` is given. Every field is optional;
   an omitted field keeps the built-in default.
3. There is currently no third, narrower CLI-override tier beyond
   `--config` itself — `--annotations` remains a separate, older mechanism
   (handle types / allocator specs / searcher tables; see Annotations.h)
   and is unaffected by this file.

## Confidence model

Every StrideVector decision (the only sizeKind this dimension currently
applies to) is one of three confidence levels, always recorded in the
output JSON as `"confidence"`:

- **`proven`** — the length AND the address induction variable's zero
  start were both established by a genuine algebraic proof: either
  ScalarEvolution's own exact trip-count analysis, or (for a loop whose
  single index variable serves as both address and exit counter, stepping
  by the stride rather than by 1 — see "The factored-bound proof" below)
  `detectFactoredStrideTripCount`'s decomposition of the loop's own
  pre-scaled bound. Runtime memory safety follows from either proof. This
  is what `analysis.policy:"strict"` (the default) accepts — neither proof
  needs `--config` or any policy opt-in at all.
- **`configured`** — a `contracts` entry (below) asserted the value; a
  human verified it, presumably because static analysis couldn't.
- **`heuristic`** — `analysis.policy:"relaxed"` opted into a NAMED
  heuristic (below) that accepts an UNPROVEN pairing anyway.

**Read this carefully before enabling `relaxed` policy or any heuristic:**
the runtime has no reduced-trust code path for a lower-confidence spec — a
`heuristic` decision is marshalled exactly like a `proven` one. Lowering
confidence makes a wrong guess *visible and attributable*, not *safer*. A
wrong `heuristic` decision carries the identical memory-safety exposure a
wrong `proven` one would (impossible by construction) or a wrong
`configured` one would (only possible if the human who wrote the contract
made a mistake).

## What is, and is NOT, configurable

Besides `policy`/`heuristics`, this file can only ever let a library
declare it needs **more scrutiny or narrower scope** than the defaults —
resource limits, delegation depth, and verified per-argument overrides in
the analyzer's own vocabulary. Nothing here can touch:

- The escape-based fail-closed gate (a pointer with no proven extent that
  escapes to an unanalyzable callee force_locals, never defaults to
  `sizeof(T)`) — unconditional, no heuristic relaxes it.
- The address-induction-variable zero-start proof — unconditional even
  under `relaxed` policy; only the LENGTH side (`guard_based_length`) and
  the per-iteration STEP's unroll-scaling (`unroll_scaled_stride`) can be
  relaxed, independently, each requiring its own explicit opt-in.
- Ambiguous cross-module callee resolution is always rejected, never
  first-picked.
- The raw-ABI-slot cap (`LIND_RAW_ARGS_MAX`) is a hardware/runtime
  constant, not a policy choice, and is not exposed here at all.

## Schema (version 1)

```jsonc
{
  "config_version": 1,           // required, must be 1
  "profile_name": "openblas",    // optional, informational, echoed into output

  "analysis": {
    "max_type_depth": 6,         // 1..64, struct/pointer nesting recursion cap
    "max_delegation_hops": 1,    // 0 or 1 only -- multi-hop delegation isn't
                                  // implemented; requesting anything else is
                                  // a hard error, not a silent clamp. 0
                                  // disables one-hop delegation entirely.
    "policy": "strict",          // "strict" (default) | "relaxed"
    "heuristics": [              // only consulted when policy=="relaxed";
                                  // required together with policy=="relaxed"
                                  // (each vacuous alone -- either shape is
                                  // rejected as a mistake)
      "guard_based_length",      // accept a length from dominatingArgumentGuard's
                                  // dominator-tree walk (proves "array-shaped",
                                  // never an exact count) instead of ScalarEvolution's
                                  // exact trip-count proof -- ONLY once the loop's
                                  // OWN latch comparison structurally confirms the
                                  // guard-derived candidate as the exclusive bound
                                  // (see "Why the heuristics exist" below)
      "unroll_scaled_stride"     // accept a `K*incx` step recurrence (K a small
                                  // power of two -- LLVM's loop-unroll artifact)
                                  // as meaning plain `incx`
    ]
  },

  "coverage": {
    "enabled": true,
    "min_marshal_count": 46,     // absolute floor: #records with decision=="marshal"
    "min_marshal_pct": 5.0       // 0..100: marshal-decision records as a % of
                                  // exported symbols (--exports) when given,
                                  // else of all covered records. At least one
                                  // of the two thresholds must be set when
                                  // enabled -- a vacuous "enabled" is rejected.
  },

  "contracts": {
    // Keyed by the EXPORTED symbol name (the same name --exports lists /
    // the JSON output's "name" field), then by argument index -- a DWARF/
    // source-parameter position (counting the C function's own declared
    // parameters left-to-right), NOT necessarily the final JSON "args"
    // array position: those differ when the function has a hidden sret
    // argument or a multi-slot (fp128) parameter shifting things.
    "cblas_daxpy": {
      "2": {
        "size_operand":   { "arg_index": 0, "source": "value" },
        "stride_operand": { "arg_index": 3, "source": "value" },
        "const_size": 8,
        "dir": "in"            // optional: "in" | "out" | "inout". Omit to
                                // let the analyzer's own read/write
                                // observation decide (unaffected by this
                                // contract).
      }
    }
  }
}
```

Every field is validated against this **closed** schema: an unknown key,
wrong-typed value, out-of-range value, or a request for an unimplemented
feature (e.g. `max_delegation_hops: 2`) is a hard `--config` load error —
this file is meant to be checked in and trusted, so a typo must fail
loudly rather than be silently ignored or downgraded (contrast with
`--annotations`, which does a looser best-effort merge). This includes a
field that is PRESENT with the wrong JSON type (`"max_type_depth": "six"`):
Config.cpp's loader distinguishes "key absent, keep the default" from "key
present but malformed" for every field, so a typo in a value never gets
mistaken for an omission and silently ignored.

### `source` values

`"value"` — the raw wasm argument slot IS the number itself (CBLAS-style
`int n`). `"pointee_i32"` — the raw slot is a pointer to a 32-bit int
holding the number (classic Fortran BLAS-style `int *N`, unpacked as
`n = *N` at function entry). See `ParamTree.h`'s `ExtentSource`.

The `analysis`/`heuristics`/`contracts` blocks above illustrate the full
schema; the checked-in OpenBLAS profile (`profiles/openblas.json`)
currently uses none of them — see "Why analyze at a different
optimization level" and "Why contracts exists" below for why.

## Why analyze at a different optimization level than the shipped library

`infer_openblas.sh` compiles the bitcode it feeds marshal-infer at a
LOWER optimization level than the real `-O2` release build
`compile_openblas.sh` produces, with loop-unrolling and vectorization
explicitly disabled — an analysis-specific compile profile, deliberately
different from the shipped binary's own codegen.

This is sound, not a shortcut: optimization level changes how a compiler
*expresses* a well-defined program, never what the program actually does.
A function's real `(length, stride)` relationship is a fact about its
source semantics, unaffected by whether the loop implementing it gets
unrolled. What changes is how EASY that fact is for static analysis to
prove — LLVM's runtime-unroll-with-remainder transform rewrites a loop's
exit test into an opaque equality check against a compiler-computed,
sign-masked bound that is no longer the loop's original source-level
condition at all (see `PATTERNS.md`'s "signed counter unrolled at `-O2`"
entry). Analyzing the unrolled form means proving facts about an
artifact of the optimizer; analyzing the un-unrolled form means proving
facts about the loop the source actually wrote.

One real gotcha, worth knowing before touching this: OpenBLAS's own
`Makefile.system` unconditionally appends `$(COMMON_OPT)` (defaulting to
`-O2` if unset) AFTER whatever `CFLAGS` a caller passes in — so a bare
`CFLAGS="-O1 ..."` override is silently overridden right back to `-O2`.
Getting the lower optimization level to actually take effect requires
setting `COMMON_OPT` directly (confirmed by tracing the real compile
command generated both ways).

Practically: this recovers strictly more than the heuristics ever did.
Against OpenBLAS, `guard_based_length`/`unroll_scaled_stride` together
used to recover 26/203 marshal (from a 20/203 strict baseline) by
reproducing a bounded slice of the pre-issue-#26 loop-shape analysis, as
an explicit, unproven, opt-in guess. Switching the analysis build to
`-O1` recovers 42/203 with ZERO heuristics, ZERO config, every decision
`proven` — the loops the heuristics used to guess about are simply no
longer unrolled, so ScalarEvolution's own exact proof succeeds directly.

## Why `contracts` exists

Issue #26's own regression is the motivating case: a compiled loop's
runtime-unroll-with-remainder shape can make ScalarEvolution's exact
trip-count analysis (or the address induction variable's zero-start proof)
fail to resolve, even though the loop's real semantics are exactly the
canonical `(length, stride)` pair a human reading the *source* can see
immediately. Rather than loosen the analyzer's proof requirements (which
would reintroduce exactly the under-allocation bug issue #26 was filed
over), a `contracts` entry lets a human assert the verified answer for that
one, specific, reviewed argument — with the assertion itself checked into
version control, validated against the same schema as everything else, and
visibly marked in the output (`"confidence":"configured"`) so nobody
mistakes it for something the tool proved on its own.

`loadConfig` validates a contract's own JSON *shape* (argument indices are
non-negative integers, `source` is one of the two known strings, `dir` is
one of the three known strings) but has no function signature to check
against — it runs before any bitcode is even read. Two further checks
happen once the target function's real, lowered signature IS known
(`validateContractAgainstSignature` in Infer.cpp, called from
`buildRecord` in main.cpp before the contract is ever applied):

- **Signature compatibility**: the target argument must exist and be a
  pointer; a `size_operand`/`stride_operand` argument index must exist; a
  `"value"` operand must name a compatible integer scalar; a
  `"pointee_i32"` operand must name a compatible pointer to a 32-bit
  integer. An out-of-range or wrong-typed operand index would otherwise be
  baked into the emitted JSON verbatim and read back by the runtime as if
  it had been proven correct.
- **Actually applied**: a contract whose symbol was never emitted at all
  (a stale entry after a refactor, or a typo in the function name), or
  whose specific argument never reached the contract-check branch for any
  other reason (checked after every function has been analyzed).

Either failure is a **hard configuration error**: marshal-infer prints
every violation found and exits nonzero WITHOUT writing any JSON output at
all — unlike a coverage-threshold shortfall (below), a bad contract could
otherwise mean the output actively contains a wrong-typed or out-of-range
spec, so nothing from that run should be treated as trustworthy.

OpenBLAS's own checked-in profile currently has no `contracts` at all:
`cblas_daxpy`/`daxpy_` originally needed one (their real `-O2` build
couldn't be proven exactly), but once the analysis build moved to `-O1`
(see above), `daxpy_k`'s loop turned out to have the same clean,
separately-provable counter/accumulator shape as `dcopy_k` — proven
directly, no assertion needed. The mechanism stays available for whatever
the next library's build genuinely can't prove on its own.

## The factored-bound proof

Some loops use a SINGLE variable as both the array address and the loop's
own exit counter, stepping by the stride rather than by 1 (see
`PATTERNS.md`'s "fused index/counter with a rescaled bound" entry, e.g.
`kernel/riscv64/asum.c`'s `sasum_k`). To still run exactly `length`
iterations with a step of `stride`, the source has to pre-scale its own
exit bound to `stride * length` — which is exactly why the ordinary exact
trip-count proof fails: the compiled bound is a genuine multiplication,
not a value reducible to a bare argument.

`detectFactoredStrideTripCount` (Infer.cpp) proves this shape directly
from the loop's own IR rather than attempting to algebraically simplify
`(length*stride)/stride` back to `length` as a general symbolic identity
(unsound in general — the stride could be zero, the multiplication could
wrap, the stride could be negative, the bound could carry an extra
offset, the loop could start at a nonzero index, ...). Every one of those
failure modes is closed off by an explicit, independently-checked
condition:

- the loop has exactly one exiting block;
- that block's latch branch actually continues the loop on its "true"
  edge (`getLatchCmpInst()` guarantees only that the comparison feeds the
  latch branch, NOTHING about which successor continues vs. exits —
  confirmed against LLVM's own implementation);
- the latch predicate is strict (`slt`/`ult`/`sgt`/`ugt`);
- the comparison's COUNTER operand (the non-bound side) has the SAME
  ScalarEvolution recurrence as the GEP's own address induction variable
  — directly, or via its post-increment value;
- the bound operand is exactly a `mul` carrying the no-wrap flag matching
  the comparison's own signedness (required: an overflowing
  multiplication's runtime value is otherwise undefined);
- exactly one of the multiplication's two operands is the IDENTICAL
  value already independently proven as this same GEP's own stride (not
  merely "some argument" — the actual same SSA value); the other operand
  is the length candidate;
- the length candidate resolves to a function argument, directly or
  through one level of Fortran-by-reference load;
- BOTH the length candidate and the stride are proven strictly positive
  by a dominating early-return guard (`dominatorProvesPositive`) — the
  no-wrap flag alone rules out overflow, not a negative operand.

Every condition here is either a direct IR match or an already-
independent proof (the stride's own SCEV proof, a dominating comparison
against a constant) — nothing is approximated. A successful result is
unconditionally `Confidence::Proven`: no `--config`, no policy opt-in,
available even under the default strict policy. Covered by an adversarial
test matrix (`tests/config/factored_bound_pos.c`/`factored_bound_neg.c`)
exercising reversed comparison operands, Fortran-by-reference operands,
and every one of the failure modes above individually.

**A real gap, found in review before this ever shipped:** the first
version proved everything above the counter/index tie-in but never
checked it — meaning the bound operand alone (`stride * length`,
positive, no-wrap) was accepted regardless of what the comparison's
OTHER operand actually was. A genuinely different induction variable
(its own counter, unrelated to the array index — e.g. `while (iterations
< n*stride) { x[i] += 1.0; i += stride; iterations += 1; }`, `i` and
`iterations` both real induction variables but never proven equal) sailed
through as if `i` itself ran to `n*stride`, undercounting exactly the way
every other bug in this file's history has: for `n=10, stride=4`, the
real extent needed is 157 elements (accesses run up to index `(40-1)*4`
since the loop truly runs `n*stride=40` times); the wrongly-accepted spec
computed only 37. Fixed by requiring the counter operand's SCEV to
literally match the GEP's own address recurrence (`tests/config/
factored_bound_neg.c`'s `neg_unrelated_counter`/`neg_different_step_counter`
pin this). The branch-direction check was added at the same time, for the
same reason (an unverified assumption baked into the first version) —
though no realistic compiled C fixture reaches an inverted branch (this
clang/LLVM version's own canonicalization prevents it), so that condition
is verified by inspection rather than an adversarial test.

This recovered `cblas_sasum`/`sasum_`/`cblas_dasum`/`dasum_` (46/203
strict at the time). `kernel/arm/sum.c`'s `ssum_`/`dsum_` and
`kernel/riscv64/nrm2.c` have the identical rescaling but each add a
further complication (a SIMD-fast-path control-flow merge; a
possibly-negative stride compared via `abs()`) this proof correctly
declines rather than reach for — see `PATTERNS.md` for both.

## The single-element fallback requires full local visibility

`analyzeAccess` walks every GEP reachable from a pointer argument, but its
result only ever fed one question directly: did any of those GEPs resolve
to a proven or configured multi-element extent (`loopBounds`,
`delegateCalls`)? A GEP that was visibly there — a loop whose trip count
didn't resolve, a plain `x[i]`, a constant `x[3]`, a negative `x[-1]` — but
produced no such resolved extent left no trace at all once discarded. The
single-element fallback (`!acc.escapes` → treat the pointer as one scalar)
had no way to distinguish that case from a pointer genuinely never indexed
past its own address (`*p`, `p[0]`, a plain scalar out-param) — both looked
identical: no length evidence, no escape.

`Access::requiresDynamicExtent` closes this: set whenever a GEP off the
pointer has any index that isn't provably an all-zero constant
(`GetElementPtrInst::hasAllZeroIndices()`), independent of whether
`loopBoundValues` proves anything about it. The single-element fallback
now requires `!acc.escapes && !acc.requiresDynamicExtent`; a pointer that
trips the flag with no exact extent proven or configured force_locals
instead, with a diagnostic naming the argument.

**Found via review, not measurement — and it was already live in the
checked-in OpenBLAS profile:** `srotmg_`/`drotmg_`/`cblas_srotmg`/
`cblas_drotmg`'s output parameter (BLAS's 5-element `P` array, written as
`P[0]`..`P[4]`) has no loop, no distinct length argument, and never
escapes — every condition the old fallback checked. It was marshalled as a
single `float`/`double`, four elements short of what the function actually
writes. Fixing the gap drops the checked-in floor from 46/203 back to
42/203 (`min_marshal_count` updated accordingly) — the sasum/dasum family
above are still proven and still marshal; these four were never sound to
begin with. Covered by `tests/config/dynamic_extent.c`: an unresolved loop
bound, a dynamic non-loop index, a constant nonzero index, and a negative
offset must all force_local; a direct dereference, an explicit `p[0]`, and
a genuine scalar out-param must all still marshal as one element.

## Why the heuristics exist, and their real ceiling

`guard_based_length` and `unroll_scaled_stride` exist for a library
that's only analyzable at a higher, unrolling optimization level (this
tool must analyze whatever bitcode it's actually given — not every
library's build necessarily has an `-O1`-equivalent option, or one that
still exposes the debug info this tool needs). Enabled TOGETHER (a real
`-O2` unrolled loop entangles both the trip-count computation and the
address induction variable's own step at once, so recovering one without
the other rarely helps), they reproduce a bounded slice of the loop-shape
analysis that existed before issue #26's soundness fix, but as an
explicit, attributable, per-library opt-in instead of the tool's
unconditional default.

**Against OpenBLAS specifically, neither is needed at all anymore.**
Historically (analyzing the real `-O2` release build), they took OpenBLAS
from 20/203 strict to 26/203 relaxed. Once the analysis build moved to
`-O1` (see above), the strict baseline alone reached 42/203 — MORE than
the old heuristic-relaxed number, with zero guessing — and the heuristics
added nothing further on top (they have nothing left to do: the loops
they used to guess about are no longer unrolled). `profiles/openblas.json`
does not enable them. The mechanism, and the safety history below, stay
relevant for whatever future library genuinely can't be analyzed
un-unrolled.

Of the 203-42=161 remaining `force_local` functions, 114 are blocked by
the raw-ABI-slot cap (`LIND_RAW_ARGS_MAX`, untouchable by any config or
analysis choice) — an absolute floor. 203-114-1(variadic) = 88 is the
practical ceiling for source-shape analysis against OpenBLAS's current
binary, not 203.

**A real safety gap, found and fixed:** `boundConfirmsExclusiveLength`'s
first version accepted a DIRECT (unmasked) match between a latch
comparison's operand and a guard-derived candidate length with NO check of
the comparison's actual predicate at all -- so a non-unrolled,
directly-inclusive loop (`for (int i = 0; i <= n; ++i)`, no masking
involved anywhere, nothing for the mask-shape check below to even look at)
sailed through exactly as if `n` were an exclusive bound, undercounting by
one stride's worth. This is exactly the class of bug issue #26 was filed
over, now living inside that fix's own safety check. Fixed
(`loopLatchConfirmsExclusiveLength` in Infer.cpp) by requiring, for a
direct match, that the comparison's predicate be PROVABLY STRICT
(`slt`/`ult`, or `sgt`/`ugt` with the bound read from the correspondingly
opposite operand) -- an inclusive or opaque predicate now NEVER accepts a
direct match, regardless of which operand happens to equal the candidate.
Covered by a dedicated test matrix (`tests/config/matrix.c`) crossing
signed/unsigned counters, forward/reversed comparison operand order, and
plain/Fortran-by-reference count arguments, all compiled WITHOUT
unrolling (`-fno-unroll-loops`) so the direct-match path specifically is
what's exercised, alongside the pre-existing unrolled-mask regression test
(`tests/config/inclusive_reject.c`).

A SEPARATE instance of the same missing check was found and fixed in the
one-hop delegation path (`detectDelegatedArrayBound`): its own
`lengthOk` computation accepted `allowGuardHeuristic` alone, without ALSO
requiring `loopLatchConfirmsExclusiveLength`'s confirmation -- meaning a
length recovered from a DELEGATED callee's guard was accepted completely
unconditionally, never checked against the callee's own loop shape at
all. Fixing this dropped OpenBLAS's `-O2`-analysis relaxed count from a
previously-reported 50/203 to an honestly-verified 26/203 at the time:
roughly two dozen functions (the `cblas_dcopy`/`cblas_dswap`/... family,
delegating into `kernel/riscv64`'s plain unrolled kernels) were being
accepted via delegation with ZERO structural confirmation of loop
exclusivity, not even the always-available masked-bound check -- those
kernels' real `-O2` IR mask constant for a SIGNED 32-bit counter is the
sign-bit-cleared form (`n & 2147483644`, i.e. `n & (-4 & INT_MAX)`, not the
simpler `n & -4`), which `maskConfirmsExclusiveLength`'s `m<0` check does
not recognize. Widening that specific mask pattern would have been
exactly the kind of guess issue #27 asks this tool to stop making --
instead, moving the analysis build to `-O1` (above) sidestepped the mask
question entirely by never unrolling the loop in the first place, which
is how this whole family ended up recovered anyway, as `proven`.

**A previously-known, still-open conservatism, unrelated to either fix
above:** `dominatingArgumentGuard` found a COMPOUND guard
(`if (n<=0 || inc_x<=0) return;` -- confirmed across 46 of OpenBLAS's own
riscv64 kernel files, e.g. `kernel/riscv64/iamax.c`) used to pick
*whichever* operand resolved first, with no way to tell `n` apart from
`inc_x` -- `dominatingArgumentGuard` now collects EVERY candidate from such
a branch and `loopBoundValues` picks the one that does NOT collide with the
independently-resolved stride (a parameter can never legitimately be its
own stride), confirmed correct with a dedicated synthetic test
(`tests/config/compound_guard.c`). OpenBLAS's `ssum_`/`dsum_` family
(`kernel/arm/sum.c`, OpenBLAS's shared fallback for targets with no
dedicated plain-sum kernel) has a DEEPER blocker the disambiguation fix
doesn't touch -- the kernel reassigns `n *= inc_x` and branches on a SIMD
fast path (`if (inc_x==1) {...}`) before its final scalar tail loop, so the
tail loop's own address induction variable is a phi merged from BOTH the
SIMD path's own advanced index and the skip-SIMD path's literal 0 --
ScalarEvolution can't prove that phi's start is the constant 0 the
zero-start proof requires, regardless of the guard being disambiguated
correctly. Recovering these would need reasoning about which CONTROL-FLOW
PATH into a loop is actually taken -- issue #27's OWN guidance is to
prefer a hand-verified `contracts` entry or analysis-friendlier IR over
building that, not another compiler-output pattern guess.

Treat any further attempt to widen a mask-shape or predicate pattern-match
with real suspicion: this class of check has already surprised its own
author more than once, on both the direct-analysis and the delegated
paths. A previously-explored third heuristic
(`latch_comparison_length`, deriving length directly from a loop's own
latch comparison when neither an exact proof nor a dominating guard found
anything) was removed entirely after measurement showed it recovered ZERO
additional OpenBLAS functions beyond what `guard_based_length` already
finds -- LLVM's own unroll transform already inserts the same guard
branches that heuristic looks for first, so the scenario it existed for
("no dominating guard at all") does not occur in practice once a loop has
been unrolled. Pure added complexity with no measured benefit; do not
re-add it without first demonstrating coverage it uniquely provides.

## Coverage thresholds

`coverage.enabled` fails the WHOLE marshal-infer run (nonzero exit; the
JSON output is still written first, so it stays inspectable) when the
library's marshal rate drops below the checked-in floor. This is what
would have caught issue #26's own regression automatically: OpenBLAS's
marshal count swung from 86 down to 18 as a soundness fix landed, with
nothing but a human noticing the number looked different.

## See also

- `profiles/openblas.json` — currently just a coverage floor
  (`min_marshal_count: 42`) to catch any future regression; neither
  `contracts` nor `analysis.policy:"relaxed"` is needed against OpenBLAS's
  real binary once `infer_openblas.sh` analyzes it at `-O1` (see above) --
  42/203 marshal, strict, every decision `proven`.
- `infer_openblas.sh` — the analysis-specific `-O1`/no-unroll/no-vectorize
  compile profile, and the `COMMON_OPT` gotcha for actually making it
  stick against OpenBLAS's own Makefile.
- `PATTERNS.md` — real functions that stressed this analysis (solved and
  still-open), across every part of the tool, not just this file's own
  config mechanisms.
- `tests/config/` — regression tests for schema validation (including
  present-but-wrong-typed fields), contract application + provenance,
  contract-vs-signature validation and stale/never-applied contracts (all
  hard errors), heuristic-gated marshalling (each heuristic combination
  that should and shouldn't unlock a case), compound-guard disambiguation,
  the inclusive-bound SAFETY property (`inclusive_reject.c`, unrolled) and
  its direct-predicate counterpart (`matrix.c`, non-unrolled, crossing
  signed/unsigned, forward/reversed, and plain/Fortran-by-reference
  counters), the factored-bound proof's own adversarial matrix
  (`factored_bound_pos.c`/`factored_bound_neg.c`), the single-element
  fallback's required full local visibility (`dynamic_extent.c`), and
  coverage-threshold
  enforcement.
