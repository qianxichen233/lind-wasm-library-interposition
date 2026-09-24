// Registers one ordinary V1 handler (toy_wide_marshal) whose entire job is
// to unpack a source-cage struct -- copied in by V1's own fixed-arity
// pointer machinery -- and forward its 9 logical fields into the
// GENERATED V2 adapter (__lind_v2_adapter_toy_wide_marshal, built by
// tools/marshal-gen/gen_v2_adapter.py from v2wide.spec.json), which does the
// real per-field pointer/handle/alias marshalling under test here. Reusing
// V1's fork/register_lib_handler/dispatch machinery gets two genuinely
// separate, cross-cage-addressable cages talking to the adapter without a
// full V2 syscall/portal/worker path: this shim's own OUTER transport (one
// V1 struct-pointer argument) is fixed-arity, but that limit is irrelevant
// to what is under test -- the adapter's marshalling of its OWN 9 logical
// arguments, three past V1's own six-argument limit.
#include <lind_syscall.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>

#include "lind_marshal.h"

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

extern uint32_t __lind_v2_adapter_toy_wide_marshal(
    uint64_t source_cage, uint64_t grate_cage,
    int32_t n, double alpha, uint32_t x_ptr, int32_t incx,
    uint32_t y_ptr, int32_t incy, uint32_t handle_tok,
    int32_t extra, uint32_t out_count_ptr);

static struct lind_marshal_spec toy_wide_shim_spec = {
    .nargs = 1,
    .args  = { { .kind = LIND_ARG_PTR, .ptr_direction = LIND_PTR_IN,
                  .size_kind = LIND_SIZE_CONST, .const_size = sizeof(struct toy_wide_args_t) } },
    .ret   = { .kind = LIND_RET_SCALAR },
};

static uint64_t toy_wide_shim(uint64_t args_shadow_ptr, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    const struct toy_wide_args_t *a = (const struct toy_wide_args_t *)(uintptr_t)args_shadow_ptr;
    printf("[Grate|v2wide] toy_wide_shim ran\n");
    fflush(stdout);
    uint32_t ret = __lind_v2_adapter_toy_wide_marshal(
        LIND_SOURCE_CAGE(), LIND_GRATE_CAGE(),
        a->n, a->alpha, a->x_ptr, a->incx, a->y_ptr, a->incy,
        a->handle_tok, a->extra, a->out_count_ptr);
    return (uint64_t)ret;
}
LIND_DEFINE_MARSHAL_HANDLER(toy_wide_marshal, &toy_wide_shim_spec, toy_wide_shim)

// cageid + 6x(arg,argcage) = 13 uint64_t params, matching
// LIND_DEFINE_MARSHAL_HANDLER's generated lind_mh_##name signature exactly.
typedef int64_t (*toy_mh_fn_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t);

int64_t pass_fptr_to_wt(uint64_t fn_ptr_uint, uint64_t cageid,
                    uint64_t arg1, uint64_t arg1cage,
                    uint64_t arg2, uint64_t arg2cage,
                    uint64_t arg3, uint64_t arg3cage,
                    uint64_t arg4, uint64_t arg4cage,
                    uint64_t arg5, uint64_t arg5cage,
                    uint64_t arg6, uint64_t arg6cage) {
    if (fn_ptr_uint == 0) {
        fprintf(stderr, "[Grate|v2wide] invalid fn ptr\n");
        __builtin_trap();
    }
    toy_mh_fn_t fn = (toy_mh_fn_t)(uintptr_t)fn_ptr_uint;
    return fn(cageid, arg1, arg1cage, arg2, arg2cage, arg3, arg3cage,
              arg4, arg4cage, arg5, arg5cage, arg6, arg6cage);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <app>\n", argv[0]); return 2; }
    int grateid = getpid();
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        int cageid = getpid();
        int r = register_lib_handler(cageid, "env", "toy_wide_marshal", grateid,
                                      (uint64_t)(uintptr_t)&lind_mh_toy_wide_marshal);
        int ok = (r == 0) ? 1 : 0;
        fprintf(stderr, "[Grate|v2wide] registered %d/1 handlers\n", ok);
        if (!ok) { fprintf(stderr, "[Grate|v2wide] FATAL: registration failed\n"); return 1; }
        if (execv(argv[1], &argv[1]) == -1) { perror("execv"); return 1; }
    }
    int status;
    while (wait(&status) > 0) {}
    int ce = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    fprintf(stderr, "[Grate|v2wide] app exited %d\n", ce);
    return ce == 0 ? 0 : 1;
}
