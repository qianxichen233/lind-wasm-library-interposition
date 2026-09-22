// A COMPOUND dominating guard (`if (n<=0 || stride<=0) return;`) checks
// TWO arguments in one branch -- the standard idiom across dozens of real
// OpenBLAS kernels (e.g. kernel/riscv64/iamax.c's `if (n<=0||inc_x<=0)
// return(max);`). dominatingArgumentGuard must disambiguate which operand
// is the real trip count using the independently-resolved stride (a
// parameter can never legitimately be its own stride), not just take
// whichever comparison resolves first -- see Infer.cpp's
// dominatingArgumentGuard and loopBoundValues. Forced to unroll (like
// walk_heuristic.c) so ScalarEvolution's own exact trip-count proof fails:
// the disambiguated guard candidate is never promoted to a real pairing
// (this function always force_locals), but it's still what the
// "array-shaped" warning names, so the disambiguation itself stays
// checkable even though nothing here marshals.
void walk_compound_guard(int n, int stride, double *x) {
  if (n <= 0 || stride <= 0) return;
  int ix = 0;
#pragma clang loop unroll_count(2)
  for (int i = 0; i < n; i++) {
    x[ix] = 0.0;
    ix += stride;
  }
}
