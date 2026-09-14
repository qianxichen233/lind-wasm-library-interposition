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

## Peeled first iteration — `kernel/riscv64/{i,}{max,min,amax,amin}.c` — OpenBLAS
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

**Issue:** the first element is handled before the loop starts (seeding
the running max/min from real data instead of a fake `0.0` sentinel), so
by the time the loop's own address accumulator (`ix`) begins, it already
starts at `inc_x`, not `0`.

**Status: unhandled by design.** The address induction variable's
zero-start proof is unconditional — never relaxed by any heuristic or
config, because a nonzero start means the real touched region doesn't
begin where the spec would claim it does. The LENGTH side still resolves
fine (`n` — the loop's own trip count is otherwise clean), which is why
this shows up as "array-shaped ... but no distinct stride operand", not a
length failure. Only a hand-verified `contracts` entry can cover this
shape; the real touched region genuinely does start at offset 0 (`x[0]`
is read, just outside the loop), so such a contract would be sound.

Not just `isamax_k`: this exact idiom is shared verbatim by all 8 kernel
files in the family — `amax.c`, `amin.c`, `iamax.c`, `iamin.c`, `imax.c`,
`imin.c`, `max.c`, `min.c` — across both precisions and both CBLAS/Fortran
naming forms, 28 exported symbols total, all force_local for the identical
reason.

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
`kernel/riscv64/nrm2.c` has the identical rescaling too, but is its own,
harder case -- see the next entry.

## Signed stride with a caller-side rebase — `kernel/riscv64/nrm2.c` (`nrm2_k`) — OpenBLAS
*Category: StrideVector length/stride inference; runtime capability gap*

```c
// interface/nrm2.c -- the exported wrapper (what marshal-infer actually
// analyzes; NRM2_K below is a separate, non-exported symbol)
FLOATRET NAME(blasint *N, FLOAT *x, blasint *INCX) {
  BLASLONG n = *N, incx = *INCX;
  if (n <= 0) return 0.;
  if (n == 1) return fabs(x[0]);
  if (incx == 0) return sqrt((double)n) * fabs(x[0]);
  if (incx < 0) x -= (n - 1) * incx;   // rebase so the kernel always walks forward
  return NRM2_K(n, x, incx);
}

// kernel/riscv64/nrm2.c
FLOAT CNAME(BLASLONG n, FLOAT *x, BLASLONG inc_x) {
  BLASLONG i = 0;
  if (n <= 0 || inc_x == 0) return(0.0);
  if (n == 1) return(ABS(x[0]));
  n *= inc_x;
  while (abs(i) < abs(n)) { ...; i += inc_x; }
}
```

**Issue, layered:**

1. The kernel's own loop condition is `abs(i) < abs(n)`, not a bare
   `i < n` -- `detectFactoredStrideTripCount` matches a direct `icmp`
   against the induction variable and the `stride*length` bound, so
   wrapping both sides in `abs()` defeats the match outright, independent
   of anything below.
2. Unlike `sasum_k`'s guard (`inc_x <= 0`, stride forced positive before
   the loop ever runs), this kernel only guards `inc_x != 0` -- BLAS
   explicitly supports a negative increment (walking the vector backward),
   and this kernel takes advantage of it directly rather than normalizing
   it away first. A caller-supplied `inc_x` can genuinely be negative at
   the point the kernel's own loop runs.
3. That matters beyond just "harder to prove": `LIND_SIZE_STRIDE_VECTOR`'s
   runtime handler (`lind_marshal.h`) hard-`_lind_marshal_abort`s the
   *entire grate process* on a negative stride operand -- shadow-copying a
   negative-stride span would mean touching memory *before* the passed
   pointer, which no `lind_arg_spec` kind currently does, so the runtime
   fails closed instead. Proving `(length=n, stride=inc_x)` for the kernel
   exactly as written and marshalling it would mean a real, legitimate
   negative-`inc_x` call kills the whole grate the first time it runs --
   worse than today's `force_local`, which just takes the slow path
   correctly.

**A real fix exists, but it isn't inference-only.** The *exported* wrapper
above already normalizes this for its own caller: when `incx<0` it rebases
`x` by `(n-1)*incx` before delegating, specifically so the kernel always
walks forward from the rebased pointer. Worked through algebraically, the
set of elements the kernel actually touches, expressed relative to the
wrapper's own *original* `x` argument (not the rebased one), is always
`x[0], x[|incx|], x[2|incx|], ..., x[(n-1)|incx|]` regardless of `incx`'s
sign -- the true stride as seen from the caller is `abs(incx)`, always
non-negative. Marshalling the wrapper (not the kernel) with a stride of
`abs(incx)` would be both provable and safe -- but `lind_extent_operand`
only knows how to read an argument's raw value or one pointer dereference
(`ExtentSource::Value`/`PointeeI32`); there's no "absolute value of an
argument" source. Closing this needs a schema addition on *both* sides
(marshal-infer's `ExtentOperand`/`Confidence` machinery, and the runtime's
`lind_extent_operand`/`_lind_eval_extent_operand`), not a marshal-infer-only
change -- coordinate with whoever owns the runtime marshalling code before
attempting it.

**Status: unhandled, correctly declined rather than guessed at.**
`cblas_snrm2`/`snrm2_`/`cblas_dnrm2`/`dnrm2_` stay `force_local`.

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
