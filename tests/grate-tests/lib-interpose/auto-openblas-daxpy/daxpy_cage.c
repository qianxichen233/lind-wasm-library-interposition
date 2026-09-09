// Exercises the gen_grate.py-GENERATED cblas_daxpy/daxpy_ handlers -- built
// straight from openblas.marshal.json's real, contract-backed StrideVector
// records (issues #26/#27) -- not a hand-written lind_marshal_spec (compare
// fail-closed/stridevec_grate.c, which isolates the evaluator by hand
// instead). Multiple elements and a non-unit stride on BOTH arrays are
// required: n==1 or incx==incy==1 can't distinguish a correct strided-extent
// computation from the old, wrong plain n*elem_size formula.
#include <stdio.h>

extern void cblas_daxpy(int n, double alpha, const double *x, int incx,
                         double *y, int incy);
extern void daxpy_(const int *n, const double *alpha, const double *x, const int *incx,
                    double *y, const int *incy);

static int check(const char *label, const double *y, const double *expected, int len) {
    for (int i = 0; i < len; i++) {
        if (y[i] != expected[i]) {
            printf("[Cage|openblas-daxpy] FAIL: %s y[%d]=%g expected %g\n", label, i, y[i], expected[i]);
            return 1;
        }
    }
    printf("[Cage|openblas-daxpy] PASS: %s\n", label);
    return 0;
}

static int run_cblas(void) {
    const int n = 5, incx = 2, incy = 3;
    double alpha = 2.5;
    double x[9]  = { 1, 0, 2, 0, 3, 0, 4, 0, 5 };   // span = 1+(5-1)*2 = 9
    double y[13] = { 0 };                            // span = 1+(5-1)*3 = 13

    cblas_daxpy(n, alpha, x, incx, y, incy);

    double expected[13] = { 0 };
    expected[0]  = alpha * x[0];   // 2.5
    expected[3]  = alpha * x[2];   // 5
    expected[6]  = alpha * x[4];   // 7.5
    expected[9]  = alpha * x[6];   // 10
    expected[12] = alpha * x[8];   // 12.5
    return check("cblas_daxpy", y, expected, 13);
}

static int run_fortran(void) {
    int n = 5, incx = 2, incy = 3;
    double alpha = 2.5;
    double x[9]  = { 1, 0, 2, 0, 3, 0, 4, 0, 5 };
    double y[13] = { 0 };

    daxpy_(&n, &alpha, x, &incx, y, &incy);

    double expected[13] = { 0 };
    expected[0]  = alpha * x[0];
    expected[3]  = alpha * x[2];
    expected[6]  = alpha * x[4];
    expected[9]  = alpha * x[6];
    expected[12] = alpha * x[8];
    return check("daxpy_", y, expected, 13);
}

int main(void) {
    int rc = 0;
    rc |= run_cblas();
    rc |= run_fortran();
    return rc;
}
