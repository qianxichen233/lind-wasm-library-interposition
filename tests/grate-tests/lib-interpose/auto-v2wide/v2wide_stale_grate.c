// Registers toy_wide_marshal against a grate cage that has ALREADY exited
// (and been reaped) before the app cage that will call it is even forked --
// proving dispatch_lib_call_v2's existing cage-liveness check rejects a
// stale call cleanly instead of hanging or crashing (issue #22 Gate 7
// review item 4: "grate exit followed by a stale call").
//
// Unlike every other grate here, the process that stays alive and waits for
// the app is NOT the one the handler is registered against: an ephemeral
// cage is forked, registers nothing, and exits immediately; this process
// waits for it to be fully reaped (so its cage is genuinely dead, not
// merely racing to become so) BEFORE forking the real app cage and
// registering toy_wide_marshal's handler_cage_id as the ephemeral cage's
// (now-dead) pid.
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
    fprintf(stderr, "[Grate|v2wide-real] FAIL: pass_fptr_to_wt reached (should be unreachable)\n");
    __builtin_trap();
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <app>\n", argv[0]); return 2; }

    pid_t ephemeral_pid = fork();
    if (ephemeral_pid < 0) { perror("fork(ephemeral)"); return 1; }
    if (ephemeral_pid == 0) {
        return 0;
    }
    int eph_status = 0;
    waitpid(ephemeral_pid, &eph_status, 0);
    fprintf(stderr, "[Grate|v2wide-real] ephemeral grate cage %d exited\n", (int)ephemeral_pid);

    pid_t pid = fork();
    if (pid < 0) { perror("fork(app)"); return 1; }
    if (pid == 0) {
        int cageid = getpid();
        // Same signature descriptor as v2wide_real_grate.c's own
        // registration -- see its comment for the field-by-field mapping.
        int r = register_lib_handler_v2(cageid, "env", "toy_wide_marshal",
                                         (int)ephemeral_pid,
                                         "__lind_v2_adapter_toy_wide_marshal",
                                         "2:idiiiiiii:i");
        int ok = (r == 0) ? 1 : 0;
        fprintf(stderr, "[Grate|v2wide-real] registered %d/1 handlers\n", ok);
        if (!ok) { fprintf(stderr, "[Grate|v2wide-real] FATAL: registration failed\n"); return 1; }
        if (execv(argv[1], &argv[1]) == -1) { perror("execv"); return 1; }
    }
    int status = 0;
    while (wait(&status) > 0) {}
    int ce = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    fprintf(stderr, "[Grate|v2wide-real] app exited %d\n", ce);
    return ce == 0 ? 0 : 1;
}
