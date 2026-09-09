# Tricky functions marshal-infer has encountered

A running catalog of real functions, across every library marshal-infer
analyzes, that turned out to be tricky for it to reason about — in any
part of the tool (loop-shape/StrideVector inference, escape analysis,
delegation, handle detection, direction inference, whatever comes up
next), not just one. Each entry: the function, the shape that trips up (or
used to trip up) the analysis, and how it's handled today. Add to this
list whenever a function surfaces a new kind of difficulty, regardless of
which library or which part of the tool it stresses.

## Compound dominating guard — `kernel/riscv64/scal.c` (`sscal_k`/`dscal_k`) — OpenBLAS
*Category: StrideVector length/stride inference*

```c
int CNAME(BLASLONG n, ..., FLOAT *x, BLASLONG inc_x, ...) {
  BLASLONG i=0, j=0;
  if ((n <= 0) || (inc_x <= 0))
    return(0);
  while (j < n) {
    x[i] = da * x[i];
    i += inc_x;
    j++;
  }
}
```

**Issue:** the guard checks two arguments at once (`n` and `inc_x`), so
picking "the" length candidate from it is ambiguous — naively taking
whichever operand resolves first can grab `inc_x` instead of `n`.

**Status: handled.** The guard walk collects every argument-derived
candidate from the branch instead of just one, and the caller picks
whichever one does *not* coincide with the independently-resolved stride
(an argument can never legitimately be its own stride). `cblas_sscal`/
`sscal_`/`cblas_dscal`/`dscal_` marshal correctly today.

## Peeled first iteration — `kernel/riscv64/iamax.c` (`isamax_k`) — OpenBLAS
*Category: StrideVector length/stride inference*

```c
BLASLONG CNAME(BLASLONG n, FLOAT *x, BLASLONG inc_x) {
  BLASLONG i=0, ix=0;
  FLOAT maxf=0.0;
  BLASLONG max=0;
  if (n <= 0 || inc_x <= 0) return(max);

  maxf = ABS(x[0]);   // handle the first element outside the loop
  ix += inc_x;
  i++;
  while (i < n) {
    if (ABS(x[ix]) > maxf) { max = i; maxf = ABS(x[ix]); }
    ix += inc_x;
    i++;
  }
  return(max+1);
}
```

**Issue:** the first element is handled before the loop starts, so by the
time the loop's own address accumulator (`ix`) begins, it already starts
at `inc_x`, not `0`.

**Status: unhandled by design.** The address induction variable's
zero-start proof is unconditional — never relaxed by any heuristic or
config, because a nonzero start means the real touched region doesn't
begin where the spec would claim it does. Only a hand-verified `contracts`
entry can cover this shape.

## Fused index/counter with a rescaled bound — `kernel/riscv64/asum.c` (`sasum_k`) — OpenBLAS
*Category: StrideVector length/stride inference*

```c
FLOAT CNAME(BLASLONG n, FLOAT *x, BLASLONG inc_x) {
  BLASLONG i=0;
  FLOAT sumf = 0.0;
  if (n <= 0 || inc_x <= 0) return(sumf);

  n *= inc_x;              // rescale so a single variable can drive both roles
  while (i < n) {
    sumf += ABS(x[i]);
    i += inc_x;
  }
  return(sumf);
}
```

**Issue:** `i` is both the address index and the loop-exit counter, and it
steps by `inc_x` rather than `1`. To still run exactly `n` iterations, the
source has to rescale the bound to `n * inc_x` — which fuses length and
stride into one multiplication instead of leaving them as two separately
provable facts.

**Status: handled.** `detectFactoredStrideTripCount` (Infer.cpp) proves
this shape directly: the loop's exit bound is confirmed to be exactly
`mul nsw/nuw (already-proven stride), (length candidate)`, the
comparison's OTHER operand is confirmed to be the SAME induction variable
as the GEP's own address recurrence (not just any counter that happens to
reach the same bound -- an early version skipped this and accepted an
unrelated second counter, undercounting; see `neg_unrelated_counter` in
`tests/config/factored_bound_neg.c`), the length candidate resolves to an
argument, and both the length and the stride are independently proven
positive by the same dominating guard -- an unconditional exact proof
(`Confidence::Proven`, no `--config` needed), not a pattern-matched
guess. `cblas_sasum`/`sasum_`/`cblas_dasum`/`dasum_` marshal correctly
today. `kernel/arm/sum.c` (`ssum_`/`dsum_`) has the
identical rescaling but ALSO an extra SIMD-fast-path branch merging the
address induction variable's start value from two different control-flow
paths -- still unhandled, a strictly harder problem than this one.
`kernel/riscv64/nrm2.c` also has the identical rescaling but guards only
`inc_x != 0` (not `inc_x > 0`, since it supports walking backward with a
negative stride) and compares `abs(i) < abs(n)` rather than a bare `i < n`
-- also still unhandled, correctly declined rather than guessed at.

## Signed counter unrolled at `-O2` — `kernel/riscv64/copy.c` (`dcopy_k`) — OpenBLAS
*Category: StrideVector length/stride inference*

```c
int CNAME(BLASLONG n, FLOAT *x, BLASLONG inc_x, FLOAT *y, BLASLONG inc_y) {
  BLASLONG i=0, ix=0, iy=0;
  if (n <= 0) return(0);
  while (i < n) {
    y[iy] = x[ix];
    ix += inc_x;
    iy += inc_y;
    i++;
  }
  return(0);
}
```

**Issue:** this loop is completely clean on its own terms (separate
counter and address accumulators, unit step, zero start). But OpenBLAS's
real `-O2` release build unrolls it by 4 with a remainder tail, rewriting
the exit test into `i == (n & mask)`. For a *signed* counter, LLVM emits
the mask as a sign-bit-cleared positive constant (`n & 2147483644`, not
the simpler `n & -4`), which the exclusivity checker doesn't recognize.

**Status: solved, but not by widening the pattern match.** marshal-infer
analyzes a dedicated `-O1`, no-unroll/no-vectorize build of the library
instead of matching the real release flags — the loop's semantics are
unaffected by optimization level, so this is sound, and it recovers this
function (and the ~20 others like it) as `proven`, no heuristic needed.
