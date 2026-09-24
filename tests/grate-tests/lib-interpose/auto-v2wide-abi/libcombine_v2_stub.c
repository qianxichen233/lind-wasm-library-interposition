// Preloaded library stub for combine_v2_real_cage.c's combine_sret_byval
// import -- register_lib_handler_v2 cannot fabricate a symbol out of
// nothing, only intercept one (same role as
// auto-openblas-v2wide/libdaxpby_v2_stub.c). This body must never actually
// run once interposed: it writes an all-zero result and a distinguishable
// FAIL marker, rather than a numerically-plausible answer, so a silent
// interposition bypass couldn't coincidentally pass the baseline diff.
#include <stdio.h>

struct Big3 { int x, y, z; };
struct Wide { double a, b, c, d; };

struct Big3 combine_sret_byval(struct Wide w, int a, int b, int c, int d, int e) {
    (void)w; (void)a; (void)b; (void)c; (void)d; (void)e;
    fprintf(stderr, "[libcombine_v2_stub] FAIL: real (uninterposed) implementation ran\n");
    struct Big3 zero = { 0, 0, 0 };
    return zero;
}
