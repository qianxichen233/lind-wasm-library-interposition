// Cage exercising lind_marshal.h's LIND_SIZE_STRIDE_VECTOR and its
// lind_extent_operand evaluation (issue #26 review). See stridevec_grate.c
// for the real functions/specs each mode targets.
//
// Usage: <mode>
//   basic        -- n/incx/incy by value, non-1 strides: checks the EXACT
//                    expected values at the touched positions.
//   zero         -- n=0 is a no-op: must not reject, must leave Y untouched.
//   zerostride   -- stride=0 touches exactly one element regardless of n:
//                    must not reject, and every Y element gets the same
//                    X[0]-derived value.
//   negstride    -- a negative stride is unsupported (documented policy):
//                    must be rejected.
//   overflow     -- INT32_MAX n and stride: proves end-to-end rejection
//                    (see the comment at run_overflow for what it does and
//                    doesn't isolate).
//   narrow       -- n=536870913, stride=1, elem_size=8: the 64-bit result
//                    (4294967304) does not overflow uint64_t arithmetic,
//                    but truncates to 8 if narrowed to a 32-bit size_t
//                    without a range check first. Must be rejected.
//   arenaexhaust -- a count that fits every 64-bit and size_t check but
//                    exceeds the shadow arena's actual capacity: must be
//                    rejected (a different failure than narrow/overflow).
//   pointee      -- n/incx/incy all loaded through pointers (toy_daxpy_ref):
//                    checks the EXACT expected values, proving the pointee
//                    operand path sizes and computes as correctly as the
//                    value path does.
//   mixed        -- n/incy by reference, incx by value (toy_daxpy_mixed):
//                    same correctness check, for a spec that doesn't source
//                    every operand the same way.
//   nullpointee  -- toy_daxpy_ref with a NULL n pointer: must be rejected.
//   wrongptr     -- toy_daxpy_ref with an n pointer that isn't a valid
//                    address in this cage: must be rejected.
//   badindex     -- toy_daxpy_badindex, whose spec's count operand names an
//                    out-of-range argument index: must be rejected.
//
// Every rejection mode expects the corresponding handler's own "handler
// ran" marker to be ABSENT from the combined test output (asserted as a
// forbidden line in run_tests.sh) -- LIND_GRATE_ERR alone does not prove
// the real handler never executed.
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define LIND_GRATE_ERR (-536805379L)

extern int toy_daxpy(int n, double alpha, const double *x, int incx, double *y, int incy);
extern int toy_daxpy_ref(const int *n, double alpha, const double *x, const int *incx,
                          double *y, const int *incy);
extern int toy_daxpy_mixed(const int *n, double alpha, const double *x, int incx,
                            double *y, const int *incy);
extern int toy_daxpy_badindex(int n, double alpha, const double *x, int incx, double *y, int incy);

static int check_y(const char *label, const double *y, const double *expected, int len) {
    for (int i = 0; i < len; i++) {
        if (y[i] != expected[i]) {
            printf("[Cage|stridevec] FAIL: %s y[%d]=%g expected %g\n", label, i, y[i], expected[i]);
            return 1;
        }
    }
    printf("[Cage|stridevec] PASS: %s\n", label);
    return 0;
}

static int run_basic(void) {
    const int n = 4, incx = 2, incy = 3;
    double alpha = 2.0;
    double x[7] = { 1, 2, 3, 4, 5, 6, 7 };   // span = 1+(4-1)*2 = 7
    double y[10] = { 0 };                     // span = 1+(4-1)*3 = 10

    long r = toy_daxpy(n, alpha, x, incx, y, incy);
    if (r == LIND_GRATE_ERR) { printf("[Cage|stridevec] FAIL: basic wrongly rejected\n"); return 1; }

    double expected[10] = { 0 };
    expected[0] = alpha * x[0];  // 2
    expected[3] = alpha * x[2];  // 6
    expected[6] = alpha * x[4];  // 10
    expected[9] = alpha * x[6];  // 14
    return check_y("basic", y, expected, 10);
}

static int run_zero(void) {
    double x[1] = { 99 };
    double y[1] = { 42 };
    long r = toy_daxpy(0, 2.0, x, 1, y, 1);
    if (r == LIND_GRATE_ERR) { printf("[Cage|stridevec] FAIL: zero wrongly rejected\n"); return 1; }
    if (y[0] != 42) { printf("[Cage|stridevec] FAIL: zero y[0]=%g expected unchanged 42\n", y[0]); return 1; }
    printf("[Cage|stridevec] PASS: zero\n");
    return 0;
}

static int run_zerostride(void) {
    const int n = 4;
    double alpha = 2.0;
    double x[1] = { 5 };       // span = 1+(4-1)*0 = 1
    double y[4] = { 0, 0, 0, 0 };
    long r = toy_daxpy(n, alpha, x, /*incx=*/0, y, /*incy=*/1);
    if (r == LIND_GRATE_ERR) { printf("[Cage|stridevec] FAIL: zerostride wrongly rejected\n"); return 1; }
    double expected[4] = { 10, 10, 10, 10 };  // every y[i] += alpha*x[0]
    return check_y("zerostride", y, expected, 4);
}

static int run_negstride(void) {
    double x[8] = { 0 };
    double y[8] = { 0 };
    long r = toy_daxpy(4, 1.0, x, -2, y, 1);
    if (r != LIND_GRATE_ERR) { printf("[Cage|stridevec] FAIL: negstride did not reject (r=%ld)\n", r); return 1; }
    printf("[Cage|stridevec] PASS: negstride\n");
    return 0;
}

static int run_overflow(void) {
    double x[1] = { 0 };
    double y[1] = { 0 };
    // (n-1)*stride must overflow; both n and stride are real (if absurd)
    // values a corrupted or adversarial caller could pass. With int32-
    // bounded n/stride and an 8-byte elem_size, the unguarded product
    // happens to still exceed the arena size before any 64-bit wraparound
    // could land on a small, dangerously-wrong value (confirmed by direct
    // calculation), so this case alone can't isolate the explicit overflow
    // guards from that fallback. They're kept anyway: a spec can set any
    // const_size, and a genuinely wrapping combination isn't provably
    // unreachable in general.
    volatile int huge_n = 0x7FFFFFFF;
    volatile int huge_stride = 0x7FFFFFFF;
    long r = toy_daxpy(huge_n, 0.0, x, huge_stride, y, 1);
    if (r != LIND_GRATE_ERR) { printf("[Cage|stridevec] FAIL: overflow did not reject (r=%ld)\n", r); return 1; }
    printf("[Cage|stridevec] PASS: overflow\n");
    return 0;
}

static int run_narrow(void) {
    double x[1] = { 0 };
    double y[1] = { 0 };
    // 536870913 * 8 = 4294967304, which fits comfortably in uint64_t but
    // truncates to 8 if cast to a 32-bit size_t without a range check.
    volatile int n = 536870913;
    long r = toy_daxpy(n, 0.0, x, 1, y, 1);
    if (r != LIND_GRATE_ERR) { printf("[Cage|stridevec] FAIL: narrow did not reject (r=%ld)\n", r); return 1; }
    printf("[Cage|stridevec] PASS: narrow\n");
    return 0;
}

static int run_arenaexhaust(void) {
    double x[1] = { 0 };
    double y[1] = { 0 };
    // 20000 * 8 = 160000 bytes, comfortably inside both uint64_t and
    // size_t range, but bigger than the shadow arena's actual capacity.
    volatile int n = 20000;
    long r = toy_daxpy(n, 0.0, x, 1, y, 1);
    if (r != LIND_GRATE_ERR) { printf("[Cage|stridevec] FAIL: arenaexhaust did not reject (r=%ld)\n", r); return 1; }
    printf("[Cage|stridevec] PASS: arenaexhaust\n");
    return 0;
}

static int run_pointee(void) {
    int n = 4, incx = 2, incy = 3;
    double alpha = 2.0;
    double x[7] = { 1, 2, 3, 4, 5, 6, 7 };
    double y[10] = { 0 };

    long r = toy_daxpy_ref(&n, alpha, x, &incx, y, &incy);
    if (r == LIND_GRATE_ERR) { printf("[Cage|stridevec] FAIL: pointee wrongly rejected\n"); return 1; }

    double expected[10] = { 0 };
    expected[0] = alpha * x[0];
    expected[3] = alpha * x[2];
    expected[6] = alpha * x[4];
    expected[9] = alpha * x[6];
    return check_y("pointee", y, expected, 10);
}

static int run_mixed(void) {
    int n = 3, incy = 2;
    const int incx = 2;  // direct value
    double alpha = 3.0;
    double x[5] = { 1, 2, 3, 4, 5 };   // span = 1+(3-1)*2 = 5
    double y[5] = { 0 };                // span = 1+(3-1)*2 = 5

    long r = toy_daxpy_mixed(&n, alpha, x, incx, y, &incy);
    if (r == LIND_GRATE_ERR) { printf("[Cage|stridevec] FAIL: mixed wrongly rejected\n"); return 1; }

    double expected[5] = { 0 };
    expected[0] = alpha * x[0];  // 3
    expected[2] = alpha * x[2];  // 9
    expected[4] = alpha * x[4];  // 15
    return check_y("mixed", y, expected, 5);
}

static int run_nullpointee(void) {
    double x[8] = { 0 };
    double y[8] = { 0 };
    int incx = 1, incy = 1;
    long r = toy_daxpy_ref(NULL, 1.0, x, &incx, y, &incy);
    if (r != LIND_GRATE_ERR) { printf("[Cage|stridevec] FAIL: nullpointee did not reject (r=%ld)\n", r); return 1; }
    printf("[Cage|stridevec] PASS: nullpointee\n");
    return 0;
}

static int run_wrongptr(void) {
    double x[8] = { 0 };
    double y[8] = { 0 };
    int incx = 1, incy = 1;
    const int *bogus_n = (const int *)(intptr_t)0x7FFF0000; // not a real object
    long r = toy_daxpy_ref(bogus_n, 1.0, x, &incx, y, &incy);
    if (r != LIND_GRATE_ERR) { printf("[Cage|stridevec] FAIL: wrongptr did not reject (r=%ld)\n", r); return 1; }
    printf("[Cage|stridevec] PASS: wrongptr\n");
    return 0;
}

static int run_badindex(void) {
    double x[8] = { 0 };
    double y[8] = { 0 };
    long r = toy_daxpy_badindex(4, 1.0, x, 1, y, 1);
    if (r != LIND_GRATE_ERR) { printf("[Cage|stridevec] FAIL: badindex did not reject (r=%ld)\n", r); return 1; }
    printf("[Cage|stridevec] PASS: badindex\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <mode>\n", argv[0]); return 2; }
    const char *mode = argv[1];
    if (strcmp(mode, "basic") == 0) return run_basic();
    if (strcmp(mode, "zero") == 0) return run_zero();
    if (strcmp(mode, "zerostride") == 0) return run_zerostride();
    if (strcmp(mode, "negstride") == 0) return run_negstride();
    if (strcmp(mode, "overflow") == 0) return run_overflow();
    if (strcmp(mode, "narrow") == 0) return run_narrow();
    if (strcmp(mode, "arenaexhaust") == 0) return run_arenaexhaust();
    if (strcmp(mode, "pointee") == 0) return run_pointee();
    if (strcmp(mode, "mixed") == 0) return run_mixed();
    if (strcmp(mode, "nullpointee") == 0) return run_nullpointee();
    if (strcmp(mode, "wrongptr") == 0) return run_wrongptr();
    if (strcmp(mode, "badindex") == 0) return run_badindex();
    fprintf(stderr, "unknown mode: %s\n", mode);
    return 2;
}
