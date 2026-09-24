// Gate 6 (Real-library proof, issue #22): calls the REAL OpenBLAS daxpby_
// with its true, direct 7-argument Fortran signature -- every argument a
// raw wasm32 pointer (n/alpha/incx/beta/incy included, unlike
// daxpby_v2_real_cage.c's by-value scalars), interposed entirely through
// __lind_v2_adapter_daxpby_ (generated fresh from the LIVE
// openblas.marshal.json each run -- see run_tests.sh), which in turn calls
// the REAL, statically-linked libopenblas.a implementation.
//
// Prints its own results the SAME way daxpby_fortran_baseline.c does --
// run_tests.sh diffs the two programs' outputs directly.
#include <stdint.h>
#include <stdio.h>

extern void daxpby_(uint32_t n_ptr, uint32_t alpha_ptr, uint32_t x_ptr, uint32_t incx_ptr,
                     uint32_t beta_ptr, uint32_t y_ptr, uint32_t incy_ptr);

static uint32_t as_u32(const void *p) { return (uint32_t)(uintptr_t)p; }

int main(void) {
    int n = 5, incx = 2, incy = 3;
    double alpha = 2.5, beta = 1.5;
    double x[9]  = { 1, 0, 2, 0, 3, 0, 4, 0, 5 };
    double y[13] = { 10, 0, 0, 20, 0, 0, 30, 0, 0, 40, 0, 0, 50 };

    daxpby_(as_u32(&n), as_u32(&alpha), as_u32(x), as_u32(&incx),
            as_u32(&beta), as_u32(y), as_u32(&incy));

    printf("[Cage|daxpby-fortran-v2] y[0]=%a\n", y[0]);
    printf("[Cage|daxpby-fortran-v2] y[3]=%a\n", y[3]);
    printf("[Cage|daxpby-fortran-v2] y[6]=%a\n", y[6]);
    printf("[Cage|daxpby-fortran-v2] y[9]=%a\n", y[9]);
    printf("[Cage|daxpby-fortran-v2] y[12]=%a\n", y[12]);
    return 0;
}
