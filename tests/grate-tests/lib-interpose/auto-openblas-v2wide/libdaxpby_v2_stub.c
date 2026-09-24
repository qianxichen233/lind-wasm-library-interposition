// Preloaded library stub for daxpby_v2_real_cage.c's cblas_daxpby import --
// register_lib_handler_v2 cannot fabricate a symbol out of nothing, only
// intercept a real one, so the dynamic linker still needs SOME "env" module
// exporting cblas_daxpby to resolve the import against. This body must
// never actually run once interposed (same convention as
// auto-v2wide/libtoy_wide_real_stub.c and fail-registration): it exists
// purely to satisfy the linker and to make a silent interposition-bypass
// loudly visible if one ever happened, not to compute a real result --
// leaving y untouched, rather than a numerically-plausible-looking wrong
// answer, so a fallthrough couldn't coincidentally pass the baseline diff.
#include <stdio.h>

void cblas_daxpby(int n, double alpha, const double *x, int incx,
                   double beta, double *y, int incy) {
    (void)n; (void)alpha; (void)x; (void)incx; (void)beta; (void)y; (void)incy;
    fprintf(stderr, "[libdaxpby_v2_stub] FAIL: real (uninterposed) implementation ran\n");
}
