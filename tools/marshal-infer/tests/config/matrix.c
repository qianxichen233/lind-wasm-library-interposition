// SAFETY MATRIX for ScalarEvolution's exact trip-count proof: exercises the
// DIRECT (non-unrolled) latch-predicate path across every axis that could
// plausibly hide an inclusive/exclusive mixup -- signed vs. unsigned
// counters, forward (`i OP n`) vs. reversed (`n OP i`) comparison operand
// order, and a plain `value` count argument vs. a Fortran-by-reference
// (`pointee_i32`) one. Compiled WITHOUT unrolling (-fno-unroll-loops), so
// none of these touch a masked-unroll bound at all -- this is exactly the
// shape of the reported bug (a real `for (int i = 0; i <= n; ++i)` loop, no
// masking anywhere) rather than the unrolled cases matrix.c's sibling
// fixtures (walk_heuristic.c, compound_guard.c, inclusive_reject.c) cover.
//
// Each function's own separate unit-step counter `i` (never used for
// indexing -- `ix` is the address accumulator) is what lets the _lt_
// (exclusive) functions' exact trip count reduce directly to a bare
// argument; the _le_ (inclusive) functions' real trip count is n+1, which
// never reduces that way, so they always force_local instead. No --config
// needed anywhere here -- this is the unconditional exact proof.

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
