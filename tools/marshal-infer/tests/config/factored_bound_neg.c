// Adversarial cases for detectFactoredStrideTripCount (Infer.cpp): the
// proof that a loop's exit bound of the form `stride * length` really
// does mean length elements walked with that stride, for a loop whose
// single index variable serves as both the array address and the exit
// counter. Each function
// here violates exactly one of the proof's required conditions and must
// force_local -- the pointer is indexed by an offset that isn't provably
// zero, so the dynamic-extent fail-closed check applies whenever no exact
// extent is proven.
//
// NOT covered here: a latch branch whose "true" successor exits the loop
// (rather than continuing it) -- the function also checks this
// (detectFactoredStrideTripCount's `trueContinues`/`falseContinues`), but
// this exact clang/LLVM version canonicalizes every natural C source
// shape tried back to the standard "true continues" form before this
// tool ever sees it (confirmed by inspecting the compiled IR of several
// deliberately-inverted source constructions), so no realistic compiled
// C fixture reaches it. Verified instead by direct inspection of
// Loop::getLatchCmpInst()'s own implementation, which documents no
// guarantee about branch direction at all.

// Inclusive bound.
void neg_inclusive_bound(int n, int stride, double *x) {
  if (n <= 0 || stride <= 0) return;
  int bound = n * stride;
  int i = 0;
  while (i <= bound) { x[i] += 1.0; i += stride; }
}

// Nonzero origin for the index.
void neg_nonzero_origin(int n, int stride, double *x) {
  if (n <= 0 || stride <= 0) return;
  n *= stride;
  int i = stride;
  while (i < n) { x[i] += 1.0; i += stride; }
}

// Different factor: bound uses `width`, but the index steps by `stride`.
void neg_different_factor(int n, int stride, int width, double *x) {
  if (n <= 0 || stride <= 0 || width <= 0) return;
  int bound = n * width;
  int i = 0;
  while (i < bound) { x[i] += 1.0; i += stride; }
}

// Offset bound (n*stride + 1).
void neg_offset_bound(int n, int stride, double *x) {
  if (n <= 0 || stride <= 0) return;
  int bound = n * stride + 1;
  int i = 0;
  while (i < bound) { x[i] += 1.0; i += stride; }
}

// Missing positivity proof: same shape as the real case, but no guard.
// force_locals directly: the pointer is indexed by a loop-carried offset
// that isn't provably a constant zero, and no exact extent was proven, so
// the dynamic-extent fail-closed check applies -- never silently treated
// as a single scalar element.
void neg_missing_guard(int n, int stride, double *x) {
  int bound = n * stride;
  int i = 0;
  while (i < bound) { x[i] += 1.0; i += stride; }
}

// Only ONE of the two factors is guarded positive.
void neg_partial_guard(int n, int stride, double *x) {
  if (n <= 0) return;
  int bound = n * stride;
  int i = 0;
  while (i < bound) { x[i] += 1.0; i += stride; }
}

// A SEPARATE induction variable (its own counter, step 1, nothing to do
// with the array index) drives the exit test, while `i` -- the actual GEP
// index -- advances independently by `stride`. `bound` (n*stride) is a
// real, positive-proven, no-wrap multiplication, and every OTHER
// condition holds -- only the counter/index tie-in check catches this.
// Without it, this would be accepted with a FAR too small extent: for
// n=10, stride=4, the loop truly runs n*stride=40 times, touching indices
// up to (40-1)*4=156 (extent 157 elements), while the wrongly-accepted
// spec would compute only (10-1)*4+1=37.
void neg_unrelated_counter(int n, int stride, double *x) {
  if (n <= 0 || stride <= 0) return;
  int bound = n * stride;
  int i = 0;
  volatile int iterations = 0;
  while (iterations < bound) {
    x[i] += 1.0;
    i += stride;
    iterations += 1;
  }
}

// A second induction variable again, this time stepping by a DIFFERENT
// amount than `stride` (not just an unrelated one entirely) -- still
// must not match the GEP's own recurrence.
void neg_different_step_counter(int n, int stride, double *x) {
  if (n <= 0 || stride <= 0) return;
  int bound = n * stride;
  int i = 0;
  volatile int j = 0;
  while (j < bound) {
    x[i] += 1.0;
    i += stride;
    j += 2;
  }
}

// Unsigned multiplication carries no nuw flag: unsigned overflow is
// well-defined (silent wraparound), not UB, so the compiler has no basis
// to mark it no-wrap without extra provable bounds -- correctly rejected
// (the runtime value genuinely could differ from the mathematical product
// if it wraps, so nothing sound can be concluded from the bound alone).
void neg_unsigned_no_nuw(unsigned n, unsigned stride, double *x) {
  if (n < 1 || stride < 1) return;
  n *= stride;
  unsigned i = 0;
  while (i < n) { x[i] += 1.0; i += stride; }
}
