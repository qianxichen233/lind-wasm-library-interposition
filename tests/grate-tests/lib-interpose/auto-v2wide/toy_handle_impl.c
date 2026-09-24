// Real backing implementations for the V2 handle round-trip proof (issue
// #22 Gate 7 review item 6). Mirrors auto-handle/auto-handle_grate.c's own
// hand-written V1 handlers exactly (create/get_val/close), but generated
// end to end through gen_v2_adapter.py --emit-grate -- proving
// LIND_RET_HANDLE/LIND_ARG_HANDLE work correctly through the fully
// generated V2 pipeline, not just a hand-written V1 handler.
// toy_other_create_v2 issues a handle from a DIFFERENT class, used only to
// obtain a genuinely wrong-class token for the rejection checks.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct toy_ctx { int val; };

void *toy_ctx_create_v2(int32_t val) {
    struct toy_ctx *ctx = (struct toy_ctx *)malloc(sizeof(struct toy_ctx));
    ctx->val = val;
    fprintf(stderr, "[Grate|handle-v2] toy_ctx_create_v2 ran val=%d\n", val);
    fflush(stderr);
    return ctx;
}

int32_t toy_ctx_get_val_v2(void *ctx_ptr) {
    struct toy_ctx *ctx = (struct toy_ctx *)ctx_ptr;
    fprintf(stderr, "[Grate|handle-v2] toy_ctx_get_val_v2 ran val=%d\n", ctx->val);
    fflush(stderr);
    return ctx->val;
}

void toy_ctx_close_v2(void *ctx_ptr) {
    fprintf(stderr, "[Grate|handle-v2] toy_ctx_close_v2 ran\n");
    fflush(stderr);
    free(ctx_ptr);
}

void *toy_other_create_v2(int32_t val) {
    struct toy_ctx *ctx = (struct toy_ctx *)malloc(sizeof(struct toy_ctx));
    ctx->val = val;
    fprintf(stderr, "[Grate|handle-v2] toy_other_create_v2 ran val=%d\n", val);
    fflush(stderr);
    return ctx;
}
