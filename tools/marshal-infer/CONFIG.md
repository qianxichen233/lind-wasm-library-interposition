# marshal-infer config files

Tracking issue: [lind-wasm-library-interposition#27](https://github.com/qianxichen233/lind-wasm-library-interposition/issues/27).

An optional `--config <file.json>` file gives a library a checked-in,
versioned, reviewable place to hold analysis knobs, human-asserted
per-argument contracts, and coverage expectations — instead of those
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

Every StrideVector decision is one of two confidence levels, always
recorded in the output JSON as `"confidence"`:

- **`proven`** — the length AND the address induction variable's zero
  start were both established by a genuine algebraic proof: either
  ScalarEvolution's own exact trip-count analysis, or (for a loop whose
  single index variable serves as both address and exit counter, stepping
  by the stride rather than by 1 — see "The factored-bound proof" below)
  `detectFactoredStrideTripCount`'s decomposition of the loop's own
  pre-scaled bound. Runtime memory safety follows from either proof
  directly, with no config or opt-in of any kind.
- **`configured`** — a checked-in config file's `contracts` entry (below)
  asserted the value; a human verified it, presumably because static
  analysis couldn't, and it was validated against the function's real
  signature before being applied (see "Why `contracts` exists" below).

There is no third, unproven-and-unasserted level: a pairing this tool
cannot prove and no contract asserts fails closed (force_local), never
accepted at reduced confidence. An earlier revision of this tool supported
exactly that — named, unproven compiler-shape heuristics, opted into via
`analysis.policy:"relaxed"` — and it has been removed entirely (see
"Historical note" below).

## What is, and is NOT, configurable

Besides `contracts` and the coverage floor, this file can only ever let a
library declare it needs **more scrutiny or narrower scope** than the
defaults — resource limits and delegation depth, in the analyzer's own
vocabulary. Nothing here can touch:

- The escape-based fail-closed gate (a pointer with no proven or asserted
  extent that escapes to an unanalyzable callee force_locals, never
  defaults to `sizeof(T)`).
- The dynamic-extent fail-closed gate (a pointer indexed by an offset
  that isn't provably a constant zero force_locals unless an exact extent
  was proven or asserted — never silently treated as a single scalar
  element; see "The single-element fallback requires full local
  visibility" below).
- The address-induction-variable zero-start proof, for the `proven` path
  (a `configured` contract instead asserts the whole envelope directly,
  including its start).
- Ambiguous cross-module callee resolution is always rejected, never
  first-picked.
- The raw-ABI-slot cap (`LIND_RAW_ARGS_MAX`) is a hardware/runtime
  constant, not a policy choice, and is not exposed here at all.
- No amount of configuration can make inference itself depend on a
  specific symbol's name -- a `contracts` entry is keyed by name because
  it is a per-symbol human assertion, not a recognizer; the analyzer code
  in Infer.cpp never branches on what function it is analyzing.

A `contracts` entry is the one thing in this file that CAN affect what
gets marshalled, and it does not weaken any of the above: it supplies the
exact same size/direction vocabulary the analyzer itself would have
produced had it been able to prove the value, checked against the target
function's real signature before it is ever applied. There is no opt-in
to accept an algebraically-unproven, human-unverified pairing.

## Schema (version 1)

```jsonc
{
  "config_version": 1,           // required, must be 1
  "profile_name": "openblas",    // optional, informational, echoed into output

  "analysis": {
    "max_type_depth": 6,         // 1..64, struct/pointer nesting recursion cap
    "max_delegation_hops": 1     // 0 or 1 only -- multi-hop delegation isn't
                                  // implemented; requesting anything else is
                                  // a hard error, not a silent clamp. 0
                                  // disables one-hop delegation entirely.
  },

  "coverage": {
    "enabled": true,
    "min_marshal_count": 42,     // absolute floor: #records with decision=="marshal"
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
`n = *N` at function entry). See `ParamTree.h`'s `ExtentSource`. This
vocabulary isn't specific to `contracts` — it's how the analyzer itself
describes, in its own JSON output, where a `size_operand`/`stride_operand`
value comes from; it appears on every StrideVector record, `proven` or
`configured` alike. A `contracts` entry just lets a human assert a
specific `(arg_index, source)` pair directly, in that same vocabulary,
instead of the analyzer discovering it.

The checked-in OpenBLAS profile (`profiles/openblas.json`) sets only
`coverage.min_marshal_count` — no `contracts` entry is currently needed
against OpenBLAS's real binary once `infer_openblas.sh` analyzes it at
`-O1` (see "Why analyze at a different optimization level" below), though
the mechanism remains available for the library's still-unresolved cases
(see "Why `contracts` exists" below).

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

Practically: this recovers strictly more than the removed heuristic layer
ever did (see "Historical note" below) — 42/203 marshal, every decision an
exact proof, with no per-library opt-in of any kind. `cblas_daxpy`/
`daxpy_` are among the functions this recovers directly: their real `-O2`
build couldn't be proven exactly, but at `-O1`, `daxpy_k`'s loop has the
same clean, separately-provable counter/accumulator shape as `dcopy_k` --
before this fix, `daxpy_`/`cblas_daxpy` needed a `contracts` entry
(below); moving the analysis build to `-O1` proved the same fact directly
instead, and the contract was removed as no longer necessary.

## Why `contracts` exists

Issue #26's own regression is the motivating case: a compiled loop's
runtime-unroll-with-remainder shape can make ScalarEvolution's exact
trip-count analysis (or the address induction variable's zero-start proof)
fail to resolve, even though the loop's real semantics are exactly the
canonical `(length, stride)` pair a human reading the *source* can see
immediately. Rather than loosen the analyzer's proof requirements (which
would reintroduce exactly the under-allocation bug issue #26 was filed
over), a `contracts` entry lets a human assert the verified answer for
that one, specific, reviewed argument — with the assertion itself checked
into version control, validated against the same schema as everything
else, and visibly marked in the output (`"confidence":"configured"`) so
nobody mistakes it for something the tool proved on its own.

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

OpenBLAS's own checked-in profile currently has no `contracts` at all —
`cblas_daxpy`/`daxpy_` are the one case that used to need one, and no
longer does (see above). The mechanism stays available, and matters, for
OpenBLAS's own remaining unresolved cases: the peeled-prefix max/min
family (`isamax_k` and 27 other exported symbols in the same kernel
family — see "Compound guard disambiguation" below), for instance, has a
real, sound `(length=n,
stride=inc_x)` relationship that this tool's automatic analysis cannot
currently derive (the address induction variable's provable start is
nonzero, because the loop's first element is handled by hand before the
loop begins) but that a human reading the source can verify directly. A
checked, signature-validated contract is the supported way to cover a
case like that -- preferable to inventing another narrow, single-purpose
inference recognizer for one library's idiom (see also KSplit's own
automatic-analysis-plus-manual-residue model, referenced in
research/arg-marshalling/).

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
unconditional: no `--config` needed, no opt-in of any kind. Covered by an adversarial
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

## Compound guard disambiguation, and the remaining coverage ceiling

`dominatingArgumentGuard` finds a COMPOUND dominating guard
(`if (n<=0 || inc_x<=0) return;` — confirmed across 46 of OpenBLAS's own
riscv64 kernel files, e.g. `kernel/riscv64/iamax.c`) and collects EVERY
candidate the branch could name, then `loopBoundValues` picks whichever
one does NOT collide with the independently-resolved stride (a parameter
can never legitimately be its own stride) — this disambiguation is only
ever used to make the descriptive "array-shaped" force_local warning name
the right argument (see "What is, and is NOT, configurable" above: no
guard-derived candidate is ever promoted to a real pairing on its own).
Confirmed correct with a dedicated synthetic test
(`tests/config/compound_guard.c`).

Of the 203-42=161 remaining `force_local` functions, 114 are blocked by
the raw-ABI-slot cap (`LIND_RAW_ARGS_MAX`, untouchable by any config or
analysis choice) — an absolute floor. 203-114-1(variadic) = 88 is the
practical ceiling for source-shape analysis against OpenBLAS's current
binary, not 203. Two families sit in that remaining gap, both real
candidates for a `contracts` entry (above) rather than a new inference
recognizer: the peeled-prefix max/min family (`isamax_k` and 27 other
exported symbols in the same kernel family — `PATTERNS.md`'s "peeled
first iteration" entry) handles its first element
before the loop starts, so the address induction variable's provable
start is nonzero even though the true touched region does start at
offset 0; and OpenBLAS's `ssum_`/`dsum_` family (`kernel/arm/sum.c`,
OpenBLAS's shared fallback for targets with no dedicated plain-sum
kernel) reassigns `n *= inc_x` and branches on a SIMD fast path before
its final scalar tail loop, so the tail loop's own address induction
variable is a phi merged from two different control-flow paths and
ScalarEvolution can't prove its start is the constant 0 the zero-start
proof requires. Neither is solvable by widening a compiler-output pattern
match (see "Historical note" below for why that's the wrong tool); both
have a real, source-verifiable `(length, stride)` relationship a
`contracts` entry could assert directly.

## Historical note: the removed relaxed-heuristic layer

Earlier revisions of this tool (and this file) supported a second,
opt-in acceptance path alongside the exact proofs and checked contracts
above: `analysis.policy:"relaxed"` plus named heuristics
(`guard_based_length`, `unroll_scaled_stride`) that accepted an
algebraically-unproven, human-*unverified* `(length, stride)` pairing
anyway. It existed for the same reason `contracts` does: analyzed at the
library's real, unrolling `-O2` release optimization level, several
OpenBLAS loops' exit tests became opaque, compiler-generated artifacts
(masked bounds, remainder loops) that ScalarEvolution's exact proof
couldn't see through, even though the source's own `(length, stride)`
relationship was in each case exactly what a human reading the loop could
see directly. Unlike a contract, though, a heuristic's guess was never
individually reviewed -- it applied automatically to every loop matching
its pattern, with no per-symbol sign-off.

That layer is gone. Once `infer_openblas.sh` was changed to analyze
OpenBLAS at a lower, non-unrolling optimization level instead (see "Why
analyze at a different optimization level" above), every loop shape the
heuristics used to paper over went back to being provable directly — the
relaxed-policy count these heuristics used to reach against the real
`-O2` build (26/203) is now exceeded by the strict, unconditional
baseline alone (42/203), with zero guessing. Removing
`analysis.policy`/`analysis.heuristics` from the config schema and every
heuristic-specific code path in Infer.cpp took real, load-bearing
complexity out of the tool for zero remaining benefit to any
currently-supported library. `contracts` and the `Confidence` enum's
`Proven`/`Configured` distinction are unrelated to this removal and
remain fully supported (above) -- a reviewed, signature-validated,
per-symbol assertion is a fundamentally different (and safe) mechanism
from an automatic, unverified pattern-match guess, and the two never
shared an implementation, only this file's schema.

Two real safety bugs were found and fixed while the heuristic layer
existed, worth knowing before ever reintroducing something like it: a
guard-derived length was, at one point, accepted as an exclusive loop
bound without checking that the loop's own latch predicate was actually
strict (an inclusive `for (i = 0; i <= n; ++i)` undercounted by one
stride's worth — exactly the class of bug issue #26 was filed over, now
recurring inside its own proposed fix); and the one-hop delegation path
independently accepted a callee's guard-derived length with no equivalent
confirmation at all, inflating a previously-reported 50/203 down to an
honestly verified 26/203 once fixed. A third candidate heuristic
(`latch_comparison_length`) was measured and dropped before ever
shipping, because it recovered zero additional functions beyond what
`guard_based_length` already found. Treat any future pattern-match over
compiler-generated loop shapes with the same suspicion: prefer moving the
analysis to a friendlier compile profile, or a checked `contracts` entry,
over widening a mask- or predicate-shape guess.

## Coverage thresholds

`coverage.enabled` fails the WHOLE marshal-infer run (nonzero exit; the
JSON output is still written first, so it stays inspectable) when the
library's marshal rate drops below the checked-in floor. This is what
would have caught issue #26's own regression automatically: OpenBLAS's
marshal count swung from 86 down to 18 as a soundness fix landed, with
nothing but a human noticing the number looked different.

## See also

- `profiles/openblas.json` — currently just a coverage floor
  (`min_marshal_count: 42`) to catch any future regression; no other field
  is needed against OpenBLAS's real binary once `infer_openblas.sh`
  analyzes it at `-O1` (see above) -- 42/203 marshal, every decision an
  exact proof.
- `infer_openblas.sh` — the analysis-specific `-O1`/no-unroll/no-vectorize
  compile profile, and the `COMMON_OPT` gotcha for actually making it
  stick against OpenBLAS's own Makefile.
- `PATTERNS.md` — real functions that stressed this analysis (solved and
  still-open), across every part of the tool, not just this file's own
  config mechanisms.
- `tests/config/` — regression tests for schema validation (including
  present-but-wrong-typed fields), the unrollable-loop-with-no-recovery-
  path safety case, compound-guard disambiguation, the inclusive-bound
  SAFETY property (`inclusive_reject.c`, unrolled) and its
  direct-predicate counterpart (`matrix.c`, non-unrolled, crossing
  signed/unsigned, forward/reversed, and plain/Fortran-by-reference
  counters), the factored-bound proof's own adversarial matrix
  (`factored_bound_pos.c`/`factored_bound_neg.c`), the single-element
  fallback's required full local visibility (`dynamic_extent.c`), contract
  application + provenance, contract-vs-signature validation, and
  stale/never-applied contracts (all hard errors), and coverage-threshold
  enforcement.
