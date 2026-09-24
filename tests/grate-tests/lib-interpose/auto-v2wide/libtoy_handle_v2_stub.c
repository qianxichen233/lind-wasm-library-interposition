// Preloaded library stub for handle_v2_real_cage.c's imports -- same role
// as libtoy_wide_real_stub.c: must never actually run once interposed.
#include <stdint.h>
#include <stdio.h>

void *toy_ctx_create_v2(int32_t val) {
    (void)val;
    fprintf(stderr, "[libtoy_handle_v2_stub] FAIL: real (uninterposed) toy_ctx_create_v2 ran\n");
    return (void *)0;
}

int32_t toy_ctx_get_val_v2(void *ctx) {
    (void)ctx;
    fprintf(stderr, "[libtoy_handle_v2_stub] FAIL: real (uninterposed) toy_ctx_get_val_v2 ran\n");
    return -1;
}

void toy_ctx_close_v2(void *ctx) {
    (void)ctx;
    fprintf(stderr, "[libtoy_handle_v2_stub] FAIL: real (uninterposed) toy_ctx_close_v2 ran\n");
}

void *toy_other_create_v2(int32_t val) {
    (void)val;
    fprintf(stderr, "[libtoy_handle_v2_stub] FAIL: real (uninterposed) toy_other_create_v2 ran\n");
    return (void *)0;
}
