// Preloaded library stub: the cage dynamically links against this so
// toy_wide_marshal resolves as an import at all -- register_lib_handler
// cannot fabricate a symbol out of nothing, it can only intercept a real
// one. This body must never actually run once interposed (see
// fail-registration's identical convention); it exists purely to satisfy
// the dynamic linker.
#include <stdint.h>
#include <stdio.h>

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

uint32_t toy_wide_marshal(const struct toy_wide_args_t *args) {
    (void)args;
    fprintf(stderr, "[libtoy_wide] FAIL: real (uninterposed) implementation ran\n");
    return 0;
}
