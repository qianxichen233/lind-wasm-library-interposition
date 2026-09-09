// Positive cases for detectFactoredStrideTripCount (Infer.cpp): every
// function here must marshal unconditionally (no --config, no policy
// opt-in) with confidence "proven" -- see factored_bound_neg.c for the
// adversarial counterparts, and PATTERNS.md's "fused index/counter"
// entry for the real OpenBLAS shape this recovers.

void pos_signed_value(int n, int stride, double *x) {
  if (n <= 0 || stride <= 0) return;
  n *= stride;
  int i = 0;
  while (i < n) { x[i] += 1.0; i += stride; }
}

// Reversed comparison operand order (n > i instead of i < n).
void pos_reversed_cmp(int n, int stride, double *x) {
  if (n <= 0 || stride <= 0) return;
  n *= stride;
  int i = 0;
  while (n > i) { x[i] += 1.0; i += stride; }
}

// Fortran-by-reference: n/stride loaded through pointers.
void pos_fortran(int *N, int *STRIDE, double *x) {
  int n = *N, stride = *STRIDE;
  if (n <= 0 || stride <= 0) return;
  n *= stride;
  int i = 0;
  while (i < n) { x[i] += 1.0; i += stride; }
}

// Differently-named function/variables -- must not depend on identifiers.
void pos_differently_named(int count, int increment, double *buf) {
  if (count <= 0 || increment <= 0) return;
  count *= increment;
  int cursor = 0;
  while (cursor < count) { buf[cursor] += 1.0; cursor += increment; }
}
