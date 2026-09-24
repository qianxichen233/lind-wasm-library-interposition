// Preloaded library stub for daxpby_fortran_v2_real_cage.c's daxpby_
// import -- same role as libdaxpby_v2_stub.c, matching the Fortran form's
// all-pointer signature. Must never actually run once interposed (same
// convention as libdaxpby_v2_stub.c); leaves y untouched rather than
// computing a numerically-plausible-looking wrong answer, so a silent
// fallthrough couldn't coincidentally pass the baseline diff.
#include <stdio.h>

void daxpby_(const int *n, const double *alpha, const double *x, const int *incx,
             const double *beta, double *y, const int *incy) {
    (void)n; (void)alpha; (void)x; (void)incx; (void)beta; (void)y; (void)incy;
    fprintf(stderr, "[libdaxpby_fortran_v2_stub] FAIL: real (uninterposed) implementation ran\n");
}
