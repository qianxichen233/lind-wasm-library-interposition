// SAFETY REGRESSION: an INCLUSIVE bound (i<=n) has a real element count of
// n+1, not n. Forced to unroll like walk_heuristic.c, which normalizes the
// loop's own latch predicate to an opaque equality check (`i_next ==
// mask(n)`) that no longer syntactically says "exclusive" vs "inclusive".
// dominatingArgumentGuard examines an EARLIER, separate precondition
// branch, never the loop's own exit test, so a guard-derived length alone
// says nothing about whether THIS loop's real bound is inclusive or
// exclusive -- exactly the class of bug issue #26 was filed over. An
// unproven guard-derived length is never promoted to a real pairing at
// all (only ScalarEvolution's own exact trip-count proof, or
// detectFactoredStrideTripCount, ever is), so this class of bug is closed
// by construction, not by a separate confirmation step. matrix.c covers
// the complementary NON-unrolled case this fixture (by construction)
// never exercises.
void walk_inclusive_reject(unsigned n, unsigned stride, double *x) {
  if (n == 0) return;
  unsigned ix = 0;
#pragma clang loop unroll_count(2)
  for (unsigned i = 0; i <= n; i++) {
    x[ix] = 0.0;
    ix += stride;
  }
}
