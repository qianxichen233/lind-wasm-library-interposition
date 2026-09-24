// Gate 6 (Real-library proof, issue #22): calls the REAL OpenBLAS
// cblas_daxpby with its true, direct 7-argument signature -- no six-slot V1
// transport involved anywhere. Interposed entirely through
// daxpby_v2_real_grate.c's real V2 (variable-width) production path:
// register_lib_handler_v2 + Linker::instance_dylink's own V2 portal check +
// wasmtime_lind_3i's worker-pool integration, dispatching straight to
// __lind_v2_adapter_cblas_daxpby (generated fresh from the LIVE
// openblas.marshal.json each run -- see run_tests.sh), which in turn calls
// the REAL, statically-linked libopenblas.a implementation.
//
// Prints its own results the SAME way daxpby_baseline.c does (matching
// tags, %a hex-float format) -- run_tests.sh diffs the two programs'
// outputs directly rather than hand-deriving an expected value here, so
// this file makes no claim about what the "right" answer is, only that
// this run produced the same one the uninterposed baseline did.
#include <stdint.h>
#include <stdio.h>

extern void cblas_daxpby(int32_t n, double alpha, uint32_t x_ptr, int32_t incx,
                          double beta, uint32_t y_ptr, int32_t incy);

static uint32_t as_u32(const void *p) { return (uint32_t)(uintptr_t)p; }

int main(void) {
    const int n = 5, incx = 2, incy = 3;
    const double alpha = 2.5, beta = 1.5;
    double x[9]  = { 1, 0, 2, 0, 3, 0, 4, 0, 5 };
    double y[13] = { 10, 0, 0, 20, 0, 0, 30, 0, 0, 40, 0, 0, 50 };

    cblas_daxpby(n, alpha, as_u32(x), incx, beta, as_u32(y), incy);

    printf("[Cage|daxpby-v2] y[0]=%a\n", y[0]);
    printf("[Cage|daxpby-v2] y[3]=%a\n", y[3]);
    printf("[Cage|daxpby-v2] y[6]=%a\n", y[6]);
    printf("[Cage|daxpby-v2] y[9]=%a\n", y[9]);
    printf("[Cage|daxpby-v2] y[12]=%a\n", y[12]);
    return 0;
}
