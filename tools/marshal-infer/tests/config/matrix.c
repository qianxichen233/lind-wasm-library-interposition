// SAFETY MATRIX for boundConfirmsExclusiveLength/loopLatchConfirmsExclusive-
// Length: exercises the DIRECT (non-unrolled) latch-predicate path across
// every axis that could plausibly hide an inclusive/exclusive mixup --
// signed vs. unsigned counters, forward (`i OP n`) vs. reversed (`n OP i`)
// comparison operand order, and a plain `value` count argument vs. a
// Fortran-by-reference (`pointee_i32`) one. Compiled WITHOUT unrolling
// (-fno-unroll-loops), so none of these touch the masked-bound branch at
// all -- this is exactly the shape of the reported bug (a real
// `for (int i = 0; i <= n; ++i)` loop, no masking anywhere) rather than the
// unrolled cases matrix.c's sibling fixtures (walk_heuristic.c,
// compound_guard.c, inclusive_reject.c) already cover.
//
// Each function's own dominating guard (`if (n<=0) return;`) exists so
// ScalarEvolution's exact trip-count proof is the ONLY thing tested by the
// _lt_ (exclusive) functions when it succeeds on its own; the _le_
// (inclusive) functions' real trip count (n+1) can never reduce to a bare
// argument, so they always fall through to the guard-derived candidate,
// which is exactly where a predicate/orientation bug would surface.
//
// Every function needs BOTH "analysis.policy":"relaxed" heuristics enabled
// to reach this code at all (see run.sh's "both.json"); the _lt_ functions
// are expected to marshal, the _le_ functions must always force_local.

void matrix_signed_lt_fwd(int n, int stride, double *x) {
  if (n <= 0) return;
  int ix = 0;
  for (int i = 0; i < n; ++i) {
    x[ix] = 0.0;
    ix += stride;
  }
}

void matrix_signed_le_fwd(int n, int stride, double *x) {
  if (n <= 0) return;
  int ix = 0;
  for (int i = 0; i <= n; ++i) {
    x[ix] = 0.0;
    ix += stride;
  }
}

void matrix_signed_lt_rev(int n, int stride, double *x) {
  if (n <= 0) return;
  int ix = 0;
  for (int i = 0; n > i; ++i) {
    x[ix] = 0.0;
    ix += stride;
  }
}

void matrix_signed_le_rev(int n, int stride, double *x) {
  if (n <= 0) return;
  int ix = 0;
  for (int i = 0; n >= i; ++i) {
    x[ix] = 0.0;
    ix += stride;
  }
}

void matrix_unsigned_lt_fwd(unsigned n, unsigned stride, double *x) {
  if (n == 0) return;
  unsigned ix = 0;
  for (unsigned i = 0; i < n; ++i) {
    x[ix] = 0.0;
    ix += stride;
  }
}

void matrix_unsigned_le_fwd(unsigned n, unsigned stride, double *x) {
  if (n == 0) return;
  unsigned ix = 0;
  for (unsigned i = 0; i <= n; ++i) {
    x[ix] = 0.0;
    ix += stride;
  }
}

void matrix_unsigned_lt_rev(unsigned n, unsigned stride, double *x) {
  if (n == 0) return;
  unsigned ix = 0;
  for (unsigned i = 0; n > i; ++i) {
    x[ix] = 0.0;
    ix += stride;
  }
}

void matrix_unsigned_le_rev(unsigned n, unsigned stride, double *x) {
  if (n == 0) return;
  unsigned ix = 0;
  for (unsigned i = 0; n >= i; ++i) {
    x[ix] = 0.0;
    ix += stride;
  }
}

// Fortran-by-reference count/stride: the same exclusive/inclusive contrast,
// but through one level of pointer-load (LoopBound/ExtentOperand's
// pointee_i32 source) rather than a plain value argument.
void matrix_fortran_lt_fwd(int *N, int *STRIDE, double *x) {
  int n = *N, stride = *STRIDE;
  if (n <= 0) return;
  int ix = 0;
  for (int i = 0; i < n; ++i) {
    x[ix] = 0.0;
    ix += stride;
  }
}

void matrix_fortran_le_fwd(int *N, int *STRIDE, double *x) {
  int n = *N, stride = *STRIDE;
  if (n <= 0) return;
  int ix = 0;
  for (int i = 0; i <= n; ++i) {
    x[ix] = 0.0;
    ix += stride;
  }
}
