// Real backing implementation for toy_wide_marshal, a wide (9-argument)
// function mixing scalar/pointer-IN/pointer-INOUT/handle/pointer-OUT
// arguments and an alias return -- see v2wide.spec.json for its marshal
// spec, statically linked into v2wide_shim_grate's generated V2 adapter.
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

double *toy_wide_marshal(int n, double alpha, const double *x, int incx,
                          double *y, int incy, void *handle_tok, int extra,
                          uint32_t *out_count) {
    printf("[Grate|v2wide-real] toy_wide_marshal handler ran\n");
    fflush(stdout);
    for (int i = 0; i < n; i++)
        y[i * incy] += alpha * x[i * incx];
    if (handle_tok) {
        int *counter = (int *)handle_tok;
        *counter += extra;
    }
    if (out_count)
        *out_count = (uint32_t)n;
    return y;
}

// Sets errno inside the GRATE's own address space, to prove the V2 portal's
// errno seed/relay (linker.rs's seed_grate_errno_from_caller/
// relay_grate_errno_to_caller) carries this back into the CALLING cage's
// own errno slot, not just the grate worker's internal thread-local -- see
// v2wide_real_cage.c's "errno" mode.
void toy_set_errno(int val) {
    errno = val;
    fprintf(stderr, "[Grate|v2wide-real] toy_set_errno ran val=%d\n", val);
    fflush(stderr);
}
