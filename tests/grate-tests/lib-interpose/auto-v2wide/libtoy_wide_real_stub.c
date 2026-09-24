// Preloaded library stub for v2wide_real_cage.c's direct 9-argument import
// shape -- register_lib_handler_v2 cannot fabricate a symbol out of
// nothing, only intercept a real one. This body must never actually run
// once interposed (see fail-registration's identical convention); it
// exists purely to satisfy the dynamic linker.
#include <stdint.h>
#include <stdio.h>

uint32_t toy_wide_marshal(int32_t n, double alpha, uint32_t x_ptr, int32_t incx,
                           uint32_t y_ptr, int32_t incy, uint32_t handle_tok,
                           int32_t extra, uint32_t out_count_ptr) {
    (void)n; (void)alpha; (void)x_ptr; (void)incx; (void)y_ptr;
    (void)incy; (void)handle_tok; (void)extra; (void)out_count_ptr;
    fprintf(stderr, "[libtoy_wide_real] FAIL: real (uninterposed) implementation ran\n");
    return 0;
}

void toy_set_errno(int32_t val) {
    (void)val;
    fprintf(stderr, "[libtoy_wide_real] FAIL: real (uninterposed) toy_set_errno ran\n");
}
