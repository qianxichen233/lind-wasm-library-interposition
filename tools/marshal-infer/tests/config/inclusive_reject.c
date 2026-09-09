// SAFETY REGRESSION: an INCLUSIVE bound (i<=n) has a real element count of
// n+1, not n. Forced to unroll like walk_heuristic.c, which normalizes the
// loop's own latch predicate to an opaque equality check (`i_next ==
// mask(n)`) that no longer syntactically says "exclusive" vs "inclusive" --
// guard_based_length could otherwise accept this length unconditionally
// (dominatingArgumentGuard examines an EARLIER, separate precondition
// branch, never the loop's own exit test, so it has no way to see this on
// its own) and silently undercount by exactly one stride's worth --
// precisely the class of bug issue #26 was filed over.
// loopLatchConfirmsExclusiveLength must reject this regardless of which
// heuristic is enabled; see its own comment in Infer.cpp. matrix.c covers
// the complementary NON-unrolled, direct-predicate case this fixture (by
// construction) never exercises.
void walk_inclusive_reject(unsigned n, unsigned stride, double *x) {
  if (n == 0) return;
  unsigned ix = 0;
#pragma clang loop unroll_count(2)
  for (unsigned i = 0; i <= n; i++) {
    x[ix] = 0.0;
    ix += stride;
  }
}
