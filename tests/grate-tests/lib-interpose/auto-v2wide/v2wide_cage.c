// Calls toy_wide_marshal (real signature, 9 logical args) exactly like an
// ordinary dynamically-linked library call; under the grate it is
// interposed and routed to the generated V2 adapter via a thin V1 shim
// (see v2wide_shim_grate.c). Values here live in this cage's own memory;
// the grate, a genuinely different cage, can only reach them through the
// real checked cross-cage copy path.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct toy_wide_args_t {
    int32_t  n;
    double   alpha;
    uint32_t x_ptr;
    int32_t  incx;
    uint32_t y_ptr;
    int32_t  incy;
    uint32_t handle_tok;
    int32_t  extra;
    uint32_t out_count_ptr;
};

extern uint32_t toy_wide_marshal(const struct toy_wide_args_t *args);

#define LIND_GRATE_ERR (-536805379L)

static uint32_t as_u32(const void *p) { return (uint32_t)(uintptr_t)p; }

static int check_double(const char *label, double got, double want) {
    if (got != want) {
        printf("[Cage|v2wide] FAIL: %s got=%g want=%g\n", label, got, want);
        return 1;
    }
    return 0;
}

static int run_basic(void) {
    const int n = 5, incx = 2, incy = 3, extra = 7;
    double alpha = 2.5;
    double x[9]  = { 1, 0, 2, 0, 3, 0, 4, 0, 5 };
    double y[13] = { 0 };
    int counter = 100;
    uint32_t out_count = 0xDEADBEEF;

    struct toy_wide_args_t args = {
        .n = n, .alpha = alpha, .x_ptr = as_u32(x), .incx = incx,
        .y_ptr = as_u32(y), .incy = incy,
        // No real handle registered in THIS cage (handles are grate-local) --
        // a raw nonzero value here is exactly what a stale/forged/never-
        // registered token looks like, which run_badtoken below expects to
        // be rejected. "basic" instead sends 0 ("no handle"), matching
        // LIND_ARG_HANDLE's documented passthrough, and checks the counter
        // is untouched.
        .handle_tok = 0, .extra = extra, .out_count_ptr = as_u32(&out_count),
    };
    uint32_t ret = toy_wide_marshal(&args);
    if ((int32_t)ret == (int32_t)(LIND_GRATE_ERR & 0xffffffff)) {
        printf("[Cage|v2wide] FAIL: basic wrongly rejected\n");
        return 1;
    }

    int rc = 0;
    rc |= check_double("y[0]", y[0], alpha * x[0]);
    rc |= check_double("y[3]", y[3], alpha * x[2]);
    rc |= check_double("y[6]", y[6], alpha * x[4]);
    rc |= check_double("y[9]", y[9], alpha * x[6]);
    rc |= check_double("y[12]", y[12], alpha * x[8]);
    if (counter != 100) { printf("[Cage|v2wide] FAIL: basic counter changed to %d, expected untouched\n", counter); rc = 1; }
    if (out_count != 5) { printf("[Cage|v2wide] FAIL: basic out_count=%u want=5\n", out_count); rc = 1; }
    if (ret != as_u32(y)) { printf("[Cage|v2wide] FAIL: basic alias-return ret=%u want=%u\n", ret, as_u32(y)); rc = 1; }
    if (rc == 0) printf("[Cage|v2wide] PASS: basic\n");
    return rc;
}

static int run_narrow(void) {
    // Same regression fail-closed/stridevec_* proves for V1's stride-vector
    // sizing, now proven through the shared primitive the generated V2
    // adapter also calls: 536870913 * 8 = 4294967304, which fits uint64_t
    // but truncates to 8 if narrowed to a 32-bit size_t unchecked.
    double x[1] = { 0 };
    double y[1] = { 0 };
    uint32_t out_count = 0;
    volatile int n = 536870913;
    struct toy_wide_args_t args = {
        .n = n, .alpha = 0.0, .x_ptr = as_u32(x), .incx = 1,
        .y_ptr = as_u32(y), .incy = 1,
        .handle_tok = 0, .extra = 0, .out_count_ptr = as_u32(&out_count),
    };
    uint32_t ret = toy_wide_marshal(&args);
    if ((int32_t)ret != (int32_t)(LIND_GRATE_ERR & 0xffffffff)) {
        printf("[Cage|v2wide] FAIL: narrow did not reject (ret=%u)\n", ret);
        return 1;
    }
    printf("[Cage|v2wide] PASS: narrow\n");
    return 0;
}

static int run_badtoken(void) {
    double x[1] = { 0 };
    double y[1] = { 0 };
    uint32_t out_count = 0;
    struct toy_wide_args_t args = {
        .n = 1, .alpha = 0.0, .x_ptr = as_u32(x), .incx = 1,
        .y_ptr = as_u32(y), .incy = 1,
        .handle_tok = 0xBADC0FFEu,  // never registered
        .extra = 0, .out_count_ptr = as_u32(&out_count),
    };
    uint32_t ret = toy_wide_marshal(&args);
    if ((int32_t)ret != (int32_t)(LIND_GRATE_ERR & 0xffffffff)) {
        printf("[Cage|v2wide] FAIL: badtoken did not reject (ret=%u)\n", ret);
        return 1;
    }
    printf("[Cage|v2wide] PASS: badtoken\n");
    return 0;
}

static int run_wrongoutptr(void) {
    double x[1] = { 0 };
    double y[1] = { 0 };
    struct toy_wide_args_t args = {
        .n = 1, .alpha = 0.0, .x_ptr = as_u32(x), .incx = 1,
        .y_ptr = as_u32(y), .incy = 1,
        .handle_tok = 0, .extra = 0,
        .out_count_ptr = 0x7FFF0000u,  // plausible-looking, never allocated
    };
    uint32_t ret = toy_wide_marshal(&args);
    if ((int32_t)ret != (int32_t)(LIND_GRATE_ERR & 0xffffffff)) {
        printf("[Cage|v2wide] FAIL: wrongoutptr did not reject (ret=%u)\n", ret);
        return 1;
    }
    printf("[Cage|v2wide] PASS: wrongoutptr\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <mode>\n", argv[0]); return 2; }
    const char *mode = argv[1];
    if (strcmp(mode, "basic") == 0) return run_basic();
    if (strcmp(mode, "narrow") == 0) return run_narrow();
    if (strcmp(mode, "badtoken") == 0) return run_badtoken();
    if (strcmp(mode, "wrongoutptr") == 0) return run_wrongoutptr();
    fprintf(stderr, "unknown mode: %s\n", mode);
    return 2;
}
