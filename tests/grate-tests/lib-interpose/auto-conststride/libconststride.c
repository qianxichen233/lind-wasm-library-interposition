// Minimal library function whose stride is a genuine compile-time
// constant -- an ordinary contiguous `x[i]` walk, not a caller-supplied
// BLAS-style increment argument -- exercising marshal-infer's
// constant-sourced StrideVector extent operand (ExtentSource::Constant)
// end to end: real inference, real gen_grate.py generation, a real
// compiled-and-run grate.
//
// Prints a marker on entry: gen_grate.py's generic dispatcher has no
// per-call trace of its own, so this is the only way to distinguish "the
// auto-generated handler genuinely marshalled and called this" from a
// coincidentally-matching result produced some other way (compare
// auto-openblas-daxpy/libblastoy.c's identical reasoning).
#include <stdio.h>

void toy_vec_scale(int n, double factor, double *x) {
    fprintf(stderr, "[libconststride] toy_vec_scale handler ran n=%d\n", n);
    for (int i = 0; i < n; i++)
        x[i] *= factor;
}
