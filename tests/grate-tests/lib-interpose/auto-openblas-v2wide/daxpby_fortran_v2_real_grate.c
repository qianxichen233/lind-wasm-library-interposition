// Gate 6 (Real-library proof, issue #22): registers the REAL OpenBLAS
// daxpby_ (classic Fortran-BLAS form) through the REAL V2 (variable-width)
// production path -- same machinery as daxpby_v2_real_grate.c, now proving
// the Fortran-by-reference calling convention (every argument a pointer,
// LIND_EXTENT_POINTEE_I32) survives the real path too, not just CBLAS's
// by-value one.
#include <lind_syscall.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>

// --compile-grate unconditionally requires a pass_fptr_to_wt export to
// exist, even though this grate registers no V1 handler -- see
// daxpby_v2_real_grate.c's identical note.
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
    fprintf(stderr, "[Grate|daxpby-fortran-v2] FAIL: pass_fptr_to_wt reached (should be unreachable)\n");
    __builtin_trap();
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <app>\n", argv[0]); return 2; }
    int grateid = getpid();
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        int cageid = getpid();
        // Signature descriptor "1:iiiiiii:": manifest version 1; params
        // n_ptr alpha_ptr x_ptr incx_ptr beta_ptr y_ptr incy_ptr, all i32
        // (every Fortran-BLAS argument is a raw wasm32 pointer); no result.
        int r = register_lib_handler_v2(cageid, "env", "daxpby_", grateid,
                                         "__lind_v2_adapter_daxpby_",
                                         "1:iiiiiii:");
        int ok = (r == 0) ? 1 : 0;
        fprintf(stderr, "[Grate|daxpby-fortran-v2] registered %d/1 handlers\n", ok);
        if (!ok) { fprintf(stderr, "[Grate|daxpby-fortran-v2] FATAL: registration failed\n"); return 1; }
        if (execv(argv[1], &argv[1]) == -1) { perror("execv"); return 1; }
    }
    int status;
    while (wait(&status) > 0) {}
    int ce = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    fprintf(stderr, "[Grate|daxpby-fortran-v2] app exited %d\n", ce);
    return ce == 0 ? 0 : 1;
}
