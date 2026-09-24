// Gate 6 (Real-library proof, issue #22): same-cage baseline for the
// classic Fortran-BLAS form daxpby_ -- every argument passed by reference
// (n/alpha/incx/beta/incy are pointers to the value, not the value itself),
// unlike cblas_daxpby's by-value CBLAS convention (see daxpby_baseline.c).
// Proves the OTHER calling convention marshal-infer's StrideVector
// pointee_i32 extent source exists for (see LIND_EXTENT_POINTEE_I32)
// actually round-trips through V2 for a real library, not just synthetic
// coverage. Same "%a hex-float, diffed against the interposed run" design
// as daxpby_baseline.c -- see that file's own comment for why.
#include <stdio.h>

extern void daxpby_(const int *n, const double *alpha, const double *x, const int *incx,
                     const double *beta, double *y, const int *incy);

int main(void) {
    int n = 5, incx = 2, incy = 3;
    double alpha = 2.5, beta = 1.5;
    double x[9]  = { 1, 0, 2, 0, 3, 0, 4, 0, 5 };
    double y[13] = { 10, 0, 0, 20, 0, 0, 30, 0, 0, 40, 0, 0, 50 };

    daxpby_(&n, &alpha, x, &incx, &beta, y, &incy);

    printf("[Baseline|daxpby-fortran-v2] y[0]=%a\n", y[0]);
    printf("[Baseline|daxpby-fortran-v2] y[3]=%a\n", y[3]);
    printf("[Baseline|daxpby-fortran-v2] y[6]=%a\n", y[6]);
    printf("[Baseline|daxpby-fortran-v2] y[9]=%a\n", y[9]);
    printf("[Baseline|daxpby-fortran-v2] y[12]=%a\n", y[12]);
    return 0;
}
