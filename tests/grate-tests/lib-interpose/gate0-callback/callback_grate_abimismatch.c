// Cross-cage function-pointer callback: ABI-lowering mismatch probe.
//
// Identical to callback_grate.c except the registration descriptor
// declares the callback as taking one i64 ("l") where the real callback
// cage A supplies actually takes one i32 ("i") -- see
// callback_cage_expect_reject.c. Proves GrateWorker::install_callback_proxies
// rejects the mismatch once it resolves the REAL function and compares its
// actual lowered type against the declared signature, before ever
// installing a proxy or calling the adapter below -- the adapter's own
// `callback(42)` is unreachable in this test, kept identical to
// callback_grate.c only so the two fixtures are easy to diff against each
// other.
//
// Compile:
//   lind-clang -s --compile-grate callback_grate_abimismatch.c \
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
    void (*callback)(int) = (void (*)(int))(uintptr_t)callback_tableidx;
    callback(42);
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
    fprintf(stderr, "[gate0-callback-grate-abimismatch] FAIL: pass_fptr_to_wt reached\n");
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
        // "l" (i64) where the real callback actually takes one i32 ("i")
        // -- see callback_cage_expect_reject.c.
        int r = register_lib_handler_v2(cageid, "env", "library_call", grateid,
                                         "__lind_v2_adapter_library_call",
                                         "1:i:i:0@l@@D@0@same_thread_only");
        if (r != 0) {
            fprintf(stderr, "[gate0-callback-grate-abimismatch] register library_call failed: %d\n", r);
            __builtin_trap();
        }
        fprintf(stderr, "[gate0-callback-grate-abimismatch] registered 1/1 handlers\n");
        if (execv(argv[1], &argv[1]) == -1) {
            perror("execv");
            __builtin_trap();
        }
    }
    int status;
    while (wait(&status) > 0) {
    }
    int ce = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    fprintf(stderr, "[gate0-callback-grate-abimismatch] app exited %d\n", ce);
    return ce == 0 ? 0 : 1;
}
