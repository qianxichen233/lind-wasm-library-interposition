// Grate registering a handler for toy_wide_sum7, whose real signature has 7
// raw wasm32 ABI slots -- one beyond the interposition transport's 6-slot
// capacity (register_lib_handler / pass_fptr_to_wt / lind_marshal_dispatch
// are all fixed at 6 argument pairs). linker.rs rejects the portal install
// outright for such a wide import, before this grate's handler is ever
// reachable -- the .nargs=7 spec below is deliberately unreachable, kept
// only because LIND_DEFINE_MARSHAL_HANDLER requires a spec/handler pair to
// generate the raw wrapper register_lib_handler needs a pointer to.
#include <lind_syscall.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <assert.h>
#include <stdint.h>

#include "../lind_marshal.h"

extern int toy_wide_sum7(int a, int b, int c, int d, int e, int f, int g);

static struct lind_marshal_spec wide_sum7_spec = {
    .nargs = 7,
    .args  = {
        { .kind = LIND_ARG_SCALAR }, { .kind = LIND_ARG_SCALAR },
        { .kind = LIND_ARG_SCALAR }, { .kind = LIND_ARG_SCALAR },
        { .kind = LIND_ARG_SCALAR }, { .kind = LIND_ARG_SCALAR },
        { .kind = LIND_ARG_SCALAR },
    },
    .ret = { .kind = LIND_RET_SCALAR },
};
static uint64_t handler_wide_sum7(uint64_t a, uint64_t b, uint64_t c,
                                   uint64_t d, uint64_t e, uint64_t f) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    printf("[Grate|widesig] toy_wide_sum7 handler ran (should not happen)\n");
    fflush(stdout);
    return 0;
}
LIND_DEFINE_MARSHAL_HANDLER(toy_wide_sum7, &wide_sum7_spec, handler_wide_sum7)

// ---------------------------------------------------------------------------
// Standard grate dispatcher — required export in every grate.
// ---------------------------------------------------------------------------
int64_t pass_fptr_to_wt(uint64_t fn_ptr_uint, uint64_t cageid,
                    uint64_t arg1, uint64_t arg1cage,
                    uint64_t arg2, uint64_t arg2cage,
                    uint64_t arg3, uint64_t arg3cage,
                    uint64_t arg4, uint64_t arg4cage,
                    uint64_t arg5, uint64_t arg5cage,
                    uint64_t arg6, uint64_t arg6cage) {
    if (fn_ptr_uint == 0) {
        fprintf(stderr, "[Grate|widesig] invalid fn ptr\n");
        assert(0);
    }
    int64_t (*fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
              uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
              uint64_t, uint64_t, uint64_t) =
        (int64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                 uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                 uint64_t, uint64_t, uint64_t))(uintptr_t)fn_ptr_uint;
    return fn(cageid, arg1, arg1cage, arg2, arg2cage,
              arg3, arg3cage, arg4, arg4cage,
              arg5, arg5cage, arg6, arg6cage);
}

int main(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <cage>\n", argv[0]); assert(0); }
    int grateid = getpid();
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); assert(0); }
    if (pid == 0) {
        int cageid = getpid();
        int ret = register_lib_handler(cageid, "env", "toy_wide_sum7", grateid, (uint64_t)(uintptr_t)&lind_mh_toy_wide_sum7);
        if (ret != 0) { fprintf(stderr, "[Grate|widesig] register toy_wide_sum7 failed\n"); assert(0); }

        printf("[Grate|widesig] registered 1/1 handlers\n");
        fflush(stdout);
        if (execv(argv[1], &argv[1]) == -1) { perror("execv"); assert(0); }
    }
    int status;
    while (wait(&status) > 0) {}
    int ce = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    fprintf(stderr, "[Grate|widesig] app exited %d\n", ce);

    // A rejected >6-slot portal install traps directly in the cage's own
    // call to toy_wide_sum7 (no marshalling/dispatch ever runs), so the cage
    // dies before it can print anything itself -- unlike the other
    // fail-closed tests, whose cages observe a returned GRATE_ERR value and
    // report their own PASS/FAIL. A clean exit (0) here means the call
    // wrongly returned, so the cage's own "FAIL: call returned normally"
    // line would have printed.
    if (ce != 0) {
        printf("[Grate|widesig] PASS: rejected (cage crashed as expected, exit=%d)\n", ce);
        return 0;
    }
    printf("[Grate|widesig] FAIL: cage did not crash (exit=%d)\n", ce);
    return 1;
}
