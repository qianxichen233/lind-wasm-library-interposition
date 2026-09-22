// Regression coverage for the fail-closed gap in analyzeAccess/inferFunction:
// a scalar-pointee pointer used through any GEP whose index isn't provably a
// constant zero must never fall through to the single-element fallback, even
// when no exact extent (loop-proven, guard-derived, or configured) was ever
// found. See Access::requiresDynamicExtent in Infer.cpp.

// (1) Induction-variable GEP whose trip count cannot be proven (an opaque
// loop condition, not a bound expressible in terms of any argument).
volatile int g_flag;
void unresolved_loop_bound(double *x) {
  int i = 0;
  while (g_flag) {
    x[i] += 1.0;
    i++;
  }
}

// (2) A dynamic index with no loop at all.
void dynamic_direct_index(double *x, int index) {
  x[index] = 1.0;
}

// (3) A constant, nonzero index.
void constant_nonzero_index(double *x) {
  x[3] = 1.0;
}

// (4) A negative constant offset.
void negative_offset_index(double *x) {
  x[-1] = 1.0;
}

// (5) Direct dereference, no indexing at all: genuinely one element.
void direct_deref(double *x) {
  *x = 1.0;
}

// (6) Explicit index 0 only: still provably a single element.
void zero_index_only(double *x) {
  x[0] = 1.0;
}

// (7) Genuine scalar out-param (frexp-shaped): unaffected by this gate.
void write_scalar_out(double x, int *exp) {
  *exp = 42;
}
