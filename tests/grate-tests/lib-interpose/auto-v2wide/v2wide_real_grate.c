// Registers toy_wide_marshal through the REAL V2 (variable-width) path --
// register_lib_handler_v2 + Linker::instance_dylink's own V2 portal check +
// wasmtime_lind_3i's worker-pool integration -- rather than
// v2wide_shim_grate.c's V1-shim workaround. No outer six-slot transport is
// involved at all here: the portal converts every Val directly into a
// V2Request and dispatches straight to the generated adapter
// (__lind_v2_adapter_toy_wide_marshal). This grate exports no
// pass_fptr_to_wt -- it needs none, since nothing here goes through V1's
// dispatch path.
#include <lind_syscall.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>

// --compile-grate unconditionally requires a pass_fptr_to_wt export to
// exist, even though this grate registers no V1 handler and so never
// actually reaches it -- the REAL dispatch here goes entirely through the
// V2 portal instead (see the file header comment).
int64_t pass_fptr_to_wt(uint64_t fn_ptr_uint, uint64_t cageid,
                    uint64_t arg1, uint64_t arg1cage,
                    uint64_t arg2, uint64_t arg2cage,
                    uint64_t arg3, uint64_t arg3cage,
                    uint64_t arg4, uint64_t arg4cage,
                    uint64_t arg5, uint64_t arg5cage,
                    uint64_t arg6, uint64_t arg6cage) {
    (void)fn_ptr_uint; (void)cageid; (void)arg1; (void)arg1cage;
    (void)arg2; (void)arg2cage; (void)arg3; (void)arg3cage;
    (void)arg4; (void)arg4cage; (void)arg5; (void)arg5cage;
    (void)arg6; (void)arg6cage;
    fprintf(stderr, "[Grate|v2wide-real] FAIL: pass_fptr_to_wt reached (should be unreachable)\n");
    __builtin_trap();
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <app>\n", argv[0]); return 2; }
    int grateid = getpid();
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        int cageid = getpid();
        // Signature descriptor "2:idiiiiiii:i": manifest version 2; params
        // n(i32) alpha(f64) x(i32) incx(i32) y(i32) incy(i32) handle_tok(i32)
        // extra(i32) out_count_ptr(i32); one i32 result (the alias return).
        int r = register_lib_handler_v2(cageid, "env", "toy_wide_marshal", grateid,
                                         "__lind_v2_adapter_toy_wide_marshal",
                                         "2:idiiiiiii:i");
        int ok = (r == 0) ? 1 : 0;
        fprintf(stderr, "[Grate|v2wide-real] registered %d/1 handlers\n", ok);
        if (!ok) { fprintf(stderr, "[Grate|v2wide-real] FATAL: registration failed\n"); return 1; }
        if (execv(argv[1], &argv[1]) == -1) { perror("execv"); return 1; }
    }
    int status;
    while (wait(&status) > 0) {}
    int ce = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    fprintf(stderr, "[Grate|v2wide-real] app exited %d\n", ce);
    return ce == 0 ? 0 : 1;
}
