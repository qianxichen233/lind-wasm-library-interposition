// Cross-cage function-pointer callback: nullable contract probe.
//
// Identical to callback_grate.c except the registration descriptor sets
// `nullable=1`, and the adapter checks for a null table index before
// calling through it (a real cage A legitimately passing NULL must not
// crash the grate). See callback_cage_null_accepted.c.
//
// Compile:
//   lind-clang -s --compile-grate callback_grate_nullable.c \
//       -- -Wl,--export-table -Wl,--growable-table
#include <lind_syscall.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>

__attribute__((export_name("__lind_v2_adapter_library_call")))
int32_t __lind_v2_adapter_library_call(uint64_t source_cage, uint64_t grate_cage,
                                        int32_t callback_tableidx) {
    (void)source_cage;
    (void)grate_cage;
    if (callback_tableidx != 0) {
        void (*callback)(int) = (void (*)(int))(uintptr_t)callback_tableidx;
        callback(42);
    }
    return 0;
}

__attribute__((export_name("__lind_v2_manifest_version")))
int __lind_v2_manifest_version(void) {
    return 1;
}

int64_t pass_fptr_to_wt(uint64_t fn_ptr_uint, uint64_t cageid, uint64_t arg1,
                         uint64_t arg1cage, uint64_t arg2, uint64_t arg2cage,
                         uint64_t arg3, uint64_t arg3cage, uint64_t arg4,
                         uint64_t arg4cage, uint64_t arg5, uint64_t arg5cage,
                         uint64_t arg6, uint64_t arg6cage) {
    (void)fn_ptr_uint;
    (void)cageid;
    (void)arg1;
    (void)arg1cage;
    (void)arg2;
    (void)arg2cage;
    (void)arg3;
    (void)arg3cage;
    (void)arg4;
    (void)arg4cage;
    (void)arg5;
    (void)arg5cage;
    (void)arg6;
    (void)arg6cage;
    fprintf(stderr, "[gate0-callback-grate-nullable] FAIL: pass_fptr_to_wt reached\n");
    __builtin_trap();
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <app> [args...]\n", argv[0]);
        __builtin_trap();
    }
    int grateid = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        __builtin_trap();
    }
    if (pid == 0) {
        int cageid = getpid();
        // nullable=1 -- see descriptor grammar's 5th spec field.
        int r = register_lib_handler_v2(cageid, "env", "library_call", grateid,
                                         "__lind_v2_adapter_library_call",
                                         "1:i:i:0@i@@D@1@same_thread_only");
        if (r != 0) {
            fprintf(stderr, "[gate0-callback-grate-nullable] register library_call failed: %d\n", r);
            __builtin_trap();
        }
        fprintf(stderr, "[gate0-callback-grate-nullable] registered 1/1 handlers\n");
        if (execv(argv[1], &argv[1]) == -1) {
            perror("execv");
            __builtin_trap();
        }
    }
    int status;
    while (wait(&status) > 0) {
    }
    int ce = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    fprintf(stderr, "[gate0-callback-grate-nullable] app exited %d\n", ce);
    return ce == 0 ? 0 : 1;
}
