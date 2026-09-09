// A canonical BLAS-style walk (length=n, stride=stride, both provably
// correct by reading the source), but with the loop FORCED to unroll by 2
// (matching the real transform OpenBLAS's actual -O2 build applies to its
// kernels) -- this defeats BOTH ScalarEvolution's exact trip-count proof
// (guard_based_length's target) and the address induction variable's exact
// step proof (unroll_scaled_stride's target) at once, requiring BOTH
// heuristics together to recover, exactly like the real OpenBLAS case.
// Compiled at -O2 (not the test suite's usual -O1) specifically to trigger
// this transform reliably.
//
// Unsigned, deliberately: for a SIGNED counter, LLVM's unroll-with-
// remainder transform emits an extra sign-bit-clearing variant of the
// rounding mask (confirmed: `n & 2147483646`, not the simpler `n & -2`) --
// boundConfirmsExclusiveLength's own comment covers why only the simpler,
// unsigned-equivalent form is recognized as a safety-proof-preserving
// shape. Real OpenBLAS kernels use signed BLASLONG counters but were
// empirically confirmed (via a real -O2 build) to still resolve via this
// same mechanism, so this fixture's choice of unsigned is a test-hygiene
// simplification, not a claim that only unsigned counters ever work.
void walk_heuristic(unsigned n, unsigned stride, double *x) {
  if (n == 0) return;
  unsigned ix = 0;
#pragma clang loop unroll_count(2)
  for (unsigned i = 0; i < n; i++) {
    x[ix] = 0.0;
    ix += stride;
  }
}
