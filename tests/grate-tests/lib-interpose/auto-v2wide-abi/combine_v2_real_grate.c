// Gate 7 task 2 (issue #22): registers combine_sret_byval -- a REAL,
// inference-discovered (not hand-authored JSON), V2-only (7 raw ABI slots)
// function exercising BOTH a hidden sret return and a byval aggregate
// argument -- through the real V2 production path: register_lib_handler_v2
// + Linker::instance_dylink's own V2 portal check + wasmtime_lind_3i's
// worker-pool integration, same machinery auto-openblas-v2wide-real
// already proved for a scalar/pointer-only shape.
//
// The signature descriptor below ("1:iiiiiii:") is hand-derived from the
// REAL lowered wasm type combine_impl.c's byval+sret lowering produces
// (sret ptr, byval ptr, 5 plain ints = 7 raw i32 slots, void result -- see
// combine_impl.c's own comment) and independently cross-checked against
// gen_v2_adapter.py's own generated extern/adapter signatures for the same
// function. If it were wrong, V2AdapterCache::resolve's SignatureMismatch
// check (Gate 3) would reject this registration at first-call resolution
// time rather than let it dispatch -- this test's own successful execution
// IS the proof the descriptor matches the adapter's actual lowered type.
#include <lind_syscall.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>

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
    fprintf(stderr, "[Grate|combine-v2] FAIL: pass_fptr_to_wt reached (should be unreachable)\n");
    __builtin_trap();
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <app>\n", argv[0]); return 2; }
    int grateid = getpid();
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        int cageid = getpid();
        int r = register_lib_handler_v2(cageid, "env", "combine_sret_byval", grateid,
                                         "__lind_v2_adapter_combine_sret_byval",
                                         "1:iiiiiii:");
        int ok = (r == 0) ? 1 : 0;
        fprintf(stderr, "[Grate|combine-v2] registered %d/1 handlers\n", ok);
        if (!ok) { fprintf(stderr, "[Grate|combine-v2] FATAL: registration failed\n"); return 1; }
        if (execv(argv[1], &argv[1]) == -1) { perror("execv"); return 1; }
    }
    int status;
    while (wait(&status) > 0) {}
    int ce = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    fprintf(stderr, "[Grate|combine-v2] app exited %d\n", ce);
    return ce == 0 ? 0 : 1;
}
