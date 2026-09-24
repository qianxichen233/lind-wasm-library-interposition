// Gate 6 (Real-library proof, issue #22): the "same-cage baseline" half of
// the comparison. A plain, non-interposed, statically-linked program that
// calls the REAL cblas_daxpby (from the same libopenblas.a the V2 grate
// below also links) directly, with no dylink/grate/marshalling anywhere in
// the picture. Its output is the reference every interposed run must match
// bit-for-bit -- run_tests.sh diffs this program's own stdout against
// daxpby_v2_real_cage.c's, rather than hand-deriving expected values (which
// would only prove marshal-infer's own StrideVector arithmetic is right,
// not that the real OpenBLAS kernel's numeric behavior survives
// interposition unchanged).
//
// %a (hex float) is printed rather than %g/%f: it round-trips a double's
// exact bit pattern through decimal text, so two runs agreeing here means
// bit-identical results, not merely equal to printed precision.
#include <stdio.h>

extern void cblas_daxpby(int n, double alpha, const double *x, int incx,
                          double beta, double *y, int incy);

int main(void) {
    const int n = 5, incx = 2, incy = 3;
    const double alpha = 2.5, beta = 1.5;
    double x[9]  = { 1, 0, 2, 0, 3, 0, 4, 0, 5 };       // span = 1+(5-1)*2 = 9
    double y[13] = { 10, 0, 0, 20, 0, 0, 30, 0, 0, 40, 0, 0, 50 }; // span = 13

    cblas_daxpby(n, alpha, x, incx, beta, y, incy);

    printf("[Baseline|daxpby-v2] y[0]=%a\n", y[0]);
    printf("[Baseline|daxpby-v2] y[3]=%a\n", y[3]);
    printf("[Baseline|daxpby-v2] y[6]=%a\n", y[6]);
    printf("[Baseline|daxpby-v2] y[9]=%a\n", y[9]);
    printf("[Baseline|daxpby-v2] y[12]=%a\n", y[12]);
    return 0;
}
