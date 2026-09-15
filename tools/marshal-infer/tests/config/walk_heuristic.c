// A canonical BLAS-style walk (length=n, stride=stride, both provably
// correct by reading the source), but with the loop FORCED to unroll by 2
// (matching the real transform OpenBLAS's actual -O2 build applies to its
// kernels) -- this defeats both ScalarEvolution's exact trip-count proof
// and the address induction variable's exact step proof at once. Compiled
// at -O2 (not the test suite's usual -O1) specifically to trigger this
// transform reliably. There is no longer any recovery path for this shape
// (see PATTERNS.md's "signed counter unrolled at -O2" entry): the
// supported answer is analyzing the library at a lower optimization level
// instead, which this fixture is not meant to demonstrate -- it always
// force_locals, confirming the tool degrades safely rather than guessing.
void walk_heuristic(unsigned n, unsigned stride, double *x) {
  if (n == 0) return;
  unsigned ix = 0;
#pragma clang loop unroll_count(2)
  for (unsigned i = 0; i < n; i++) {
    x[ix] = 0.0;
    ix += stride;
  }
}
