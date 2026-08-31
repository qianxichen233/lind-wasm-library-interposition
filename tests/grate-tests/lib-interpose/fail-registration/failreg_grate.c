// Grate proving a grate aborts startup rather than run with an incomplete
// handler table: one registration is deliberately given a NULL symbol name
// (register_lib_handler rejects null string pointers), and the grate must
// detect that and abort before ever calling execv -- never exec the cage
// with some interposed symbols silently unregistered.
#include <lind_syscall.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <assert.h>
#include <stdint.h>

static uint64_t handler_stub(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    return 0;
}

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
    return (int64_t)handler_stub(arg1, arg2, arg3, arg4, arg5, arg6);
}

int main(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <cage>\n", argv[0]); assert(0); }
    int grateid = getpid();
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); assert(0); }
    if (pid == 0) {
        int cageid = getpid();
        int ok = 0, fail = 0;

        int r1 = register_lib_handler(cageid, "env", "toy_add", grateid, (uint64_t)(uintptr_t)&handler_stub);
        if (r1 == 0) ok++; else { fail++; fprintf(stderr, "[Grate|fail-registration] register toy_add failed: %d\n", r1); }

        // Deliberately broken: NULL symbol name. register_lib_handler
        // rejects this (returns -1) -- this must be caught, not ignored.
        int r2 = register_lib_handler(cageid, "env", NULL, grateid, (uint64_t)(uintptr_t)&handler_stub);
        if (r2 == 0) ok++; else { fail++; fprintf(stderr, "[Grate|fail-registration] register (null symbol) failed: %d\n", r2); }

        printf("[Grate|fail-registration] registered %d/%d handlers\n", ok, ok + fail);
        if (fail > 0) {
            fprintf(stderr, "[Grate|fail-registration] FATAL: %d handler registration(s) failed, aborting startup\n", fail);
            assert(0);
        }
        if (execv(argv[1], &argv[1]) == -1) { perror("execv"); assert(0); }
    }
    int status;
    while (wait(&status) > 0) {}
    int ce = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    fprintf(stderr, "[Grate|fail-registration] app exited %d (WIFSIGNALED=%d)\n", ce, WIFSIGNALED(status));
    return 0;
}
