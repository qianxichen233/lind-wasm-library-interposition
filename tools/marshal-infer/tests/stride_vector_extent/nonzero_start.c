// The loop itself is canonical (n iterations, exact trip count), but the
// ADDRESS induction variable used for the GEP starts at `stride`, not 0 --
// the base pointer passed at the real call is never itself accessed
// (x[0] is skipped entirely; the walk covers x[stride]..x[n*stride]). The
// runtime always shadow-copies starting at the passed pointer (offset 0),
// so the (1+(n-1)*stride) formula computed from a zero-start assumption
// would under-cover the real accessed range. Must be rejected, never
// silently treated as if the address IV started at 0.
void walk_nonzero_start(int n, int stride, double *x) {
  int ix = stride;
  for (int i = 0; i < n; i++) {
    x[ix] = 0.0;
    ix += stride;
  }
}
