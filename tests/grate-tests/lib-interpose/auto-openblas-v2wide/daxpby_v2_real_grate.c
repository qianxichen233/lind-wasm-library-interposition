// Gate 6 (Real-library proof, issue #22): registers the REAL OpenBLAS
// cblas_daxpby through the REAL V2 (variable-width) production path --
// register_lib_handler_v2 + Linker::instance_dylink's own V2 portal check +
// wasmtime_lind_3i's worker-pool integration -- the same machinery
// auto-v2wide/v2wide_real_grate.c proved against a hand-written 9-argument
// toy function. This grate is the first to combine that path with a REAL
// wide (7-raw-ABI-slot > LIND_RAW_ARGS_MAX=6) library function and a REAL,
// unmodified static archive (libopenblas.a) supplying the actual
// implementation, not a toy stand-in -- see run_tests.sh for how
// __lind_v2_adapter_cblas_daxpby (linked in alongside this file) is
// generated fresh from the LIVE openblas.marshal.json each run.
#include <lind_syscall.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>

// --compile-grate unconditionally requires a pass_fptr_to_wt export to
// exist, even though this grate registers no V1 handler and so never
// actually reaches it -- the REAL dispatch here goes entirely through the
// V2 portal instead (see auto-v2wide/v2wide_real_grate.c's identical note).
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
    fprintf(stderr, "[Grate|daxpby-v2] FAIL: pass_fptr_to_wt reached (should be unreachable)\n");
    __builtin_trap();
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <app>\n", argv[0]); return 2; }
    int grateid = getpid();
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        int cageid = getpid();
        // Signature descriptor "1:idiidii:": manifest version 1; params
        // n(i32) alpha(f64) x(i32) incx(i32) beta(f64) y(i32) incy(i32);
        // no result (cblas_daxpby is void).
        int r = register_lib_handler_v2(cageid, "env", "cblas_daxpby", grateid,
                                         "__lind_v2_adapter_cblas_daxpby",
                                         "1:idiidii:");
        int ok = (r == 0) ? 1 : 0;
        fprintf(stderr, "[Grate|daxpby-v2] registered %d/1 handlers\n", ok);
        if (!ok) { fprintf(stderr, "[Grate|daxpby-v2] FATAL: registration failed\n"); return 1; }
        if (execv(argv[1], &argv[1]) == -1) { perror("execv"); return 1; }
    }
    int status;
    while (wait(&status) > 0) {}
    int ce = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    fprintf(stderr, "[Grate|daxpby-v2] app exited %d\n", ce);
    return ce == 0 ? 0 : 1;
}
