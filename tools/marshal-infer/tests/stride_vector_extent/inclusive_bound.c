// The loop condition is INCLUSIVE (i<=n), so the real element count is n+1,
// not n. The controlling induction variable's own start/step/predicate must
// be accounted for exactly (ScalarEvolution's trip-count analysis does this
// by construction); a length that can only be proven equal to a DIFFERENT
// quantity than n itself (here, n+1, which this tool has no way to
// represent) must be rejected, never silently reported as "n".
void walk_inclusive(int n, int stride, double *x) {
  int ix = 0;
  for (int i = 0; i <= n; i++) {
    x[ix] = 0.0;
    ix += stride;
  }
}
