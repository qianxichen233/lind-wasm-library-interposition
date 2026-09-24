// Registers ONLY toy_set_errno through the real V2 production path --
// isolated from v2wide_real_grate.c's own toy_wide_marshal registration so
// this test doesn't perturb that grate's "registered 1/1 handlers" message
// (and every test that asserts on it). Proves the V2 portal's errno
// seed/relay (linker.rs's seed_grate_errno_from_caller/
// relay_grate_errno_to_caller, shared with the V1 portal) carries the
// GRATE's own errno write back into the CALLING cage's own errno slot --
// see v2wide_real_cage.c's "errno" mode.
#include <lind_syscall.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>

// --compile-grate unconditionally requires a pass_fptr_to_wt export to
// exist, even though this grate registers no V1 handler -- see
// v2wide_real_grate.c's identical note.
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
    fprintf(stderr, "[Grate|v2wide-errno] FAIL: pass_fptr_to_wt reached (should be unreachable)\n");
    __builtin_trap();
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <app>\n", argv[0]); return 2; }
    int grateid = getpid();
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        int cageid = getpid();
        // Signature descriptor "2:i:": manifest version 2; one i32 param
        // (val), no result.
        int r = register_lib_handler_v2(cageid, "env", "toy_set_errno", grateid,
                                         "__lind_v2_adapter_toy_set_errno",
                                         "2:i:");
        int ok = (r == 0) ? 1 : 0;
        fprintf(stderr, "[Grate|v2wide-errno] registered %d/1 handlers\n", ok);
        if (!ok) { fprintf(stderr, "[Grate|v2wide-errno] FATAL: registration failed\n"); return 1; }
        if (execv(argv[1], &argv[1]) == -1) { perror("execv"); return 1; }
    }
    int status;
    while (wait(&status) > 0) {}
    int ce = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    fprintf(stderr, "[Grate|v2wide-errno] app exited %d\n", ce);
    return ce == 0 ? 0 : 1;
}
