// Cage for the V2 handle round-trip proof (issue #22 Gate 7 review item 6).
// Mirrors auto-handle/auto-handle.c's own V1 round trip exactly (create /
// get_val / close), but interposed entirely through the real V2 production
// path -- register_lib_handler_v2 + Linker::instance_dylink's own V2 portal
// + wasmtime_lind_3i's worker-pool integration -- via a grate generated end
// to end by gen_v2_adapter.py --emit-grate from handle_v2.spec.json.
//
// "roundtrip" is the successful case (create returns a nonzero handle, a
// later call consumes it, close releases it). "wrongclass" and "stale" each
// deliberately trigger lind_marshal.h's existing LIND_ARG_HANDLE fail-closed
// check ("a nonzero token that fails to translate traps") with a token from
// the wrong handle_class, and a token already released by close,
// respectively -- both are expected to TRAP the process (a hard wasm trap,
// not a soft GRATE_ERR-sentinel return -- see lind_marshal.h's own comment),
// so run_tests.sh checks them as bespoke exit-code assertions rather than
// through run_test's own PASS-line convention.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void *toy_ctx_create_v2(int32_t val);
extern int32_t toy_ctx_get_val_v2(void *ctx);
extern void toy_ctx_close_v2(void *ctx);
extern void *toy_other_create_v2(int32_t val);

static int run_roundtrip(void) {
    void *h = toy_ctx_create_v2(42);
    if (h == NULL) {
        printf("[Cage|handle-v2] FAIL: create returned NULL\n");
        return 1;
    }
    int32_t val = toy_ctx_get_val_v2(h);
    if (val != 42) {
        printf("[Cage|handle-v2] FAIL: get_val = %d, expected 42\n", val);
        return 1;
    }
    toy_ctx_close_v2(h);
    printf("[Cage|handle-v2] PASS: roundtrip, val=%d\n", val);
    return 0;
}

// Never returns normally: get_val on a token from a DIFFERENT handle_class
// must trap before printing anything past this point.
static int run_wrongclass(void) {
    void *h = toy_ctx_create_v2(42);
    if (h == NULL) { printf("[Cage|handle-v2] FAIL: create returned NULL\n"); return 1; }
    if (toy_ctx_get_val_v2(h) != 42) { printf("[Cage|handle-v2] FAIL: get_val wrong\n"); return 1; }
    void *other = toy_other_create_v2(7);
    if (other == NULL) { printf("[Cage|handle-v2] FAIL: other-create returned NULL\n"); return 1; }
    printf("[Cage|handle-v2] about to access wrong-class token\n");
    fflush(stdout);
    int32_t bad = toy_ctx_get_val_v2(other); // must trap
    printf("[Cage|handle-v2] FAIL: wrong-class token was not rejected (got %d)\n", bad);
    return 1;
}

// Never returns normally: get_val on an ALREADY-CLOSED token must trap
// before printing anything past this point.
static int run_stale(void) {
    void *h = toy_ctx_create_v2(42);
    if (h == NULL) { printf("[Cage|handle-v2] FAIL: create returned NULL\n"); return 1; }
    if (toy_ctx_get_val_v2(h) != 42) { printf("[Cage|handle-v2] FAIL: get_val wrong\n"); return 1; }
    toy_ctx_close_v2(h);
    printf("[Cage|handle-v2] about to access stale token\n");
    fflush(stdout);
    int32_t bad = toy_ctx_get_val_v2(h); // must trap
    printf("[Cage|handle-v2] FAIL: stale token was not rejected (got %d)\n", bad);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <mode>\n", argv[0]); return 2; }
    const char *mode = argv[1];
    if (strcmp(mode, "roundtrip") == 0) return run_roundtrip();
    if (strcmp(mode, "wrongclass") == 0) return run_wrongclass();
    if (strcmp(mode, "stale") == 0) return run_stale();
    fprintf(stderr, "unknown mode: %s\n", mode);
    return 2;
}
