// Calls toy_wide_marshal with its REAL, direct 9-argument signature (unlike
// v2wide_cage.c, written for v2wide_shim_grate.c's V1-shim workaround, which
// packs those same 9 fields into one struct pointer instead). Under
// v2wide_real_grate.c, this import is interposed entirely through the real
// V2 (variable-width) path: register_lib_handler_v2 + Linker::
// instance_dylink's own V2 portal check + wasmtime_lind_3i's worker-pool
// integration, with no V1 six-slot transport involved anywhere.
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern uint32_t toy_wide_marshal(int32_t n, double alpha, uint32_t x_ptr, int32_t incx,
                                  uint32_t y_ptr, int32_t incy, uint32_t handle_tok,
                                  int32_t extra, uint32_t out_count_ptr);
extern void toy_set_errno(int32_t val);

#define LIND_GRATE_ERR (-536805379L)

static uint32_t as_u32(const void *p) { return (uint32_t)(uintptr_t)p; }

static int check_double(const char *label, double got, double want) {
    if (got != want) {
        printf("[Cage|v2wide-real] FAIL: %s got=%g want=%g\n", label, got, want);
        return 1;
    }
    return 0;
}

// Makes one successful call and checks its exact numeric results, without
// printing PASS itself -- shared by every mode that needs "a normal call
// succeeded" as a building block (basic, sequence, fork), not just the
// single-shot basic mode.
static int call_and_check_basic(const char *label) {
    const int n = 5, incx = 2, incy = 3, extra = 7;
    double alpha = 2.5;
    double x[9]  = { 1, 0, 2, 0, 3, 0, 4, 0, 5 };
    double y[13] = { 0 };
    uint32_t out_count = 0xDEADBEEF;

    uint32_t ret = toy_wide_marshal(n, alpha, as_u32(x), incx, as_u32(y), incy,
                                     0 /* no handle */, extra, as_u32(&out_count));
    if ((int32_t)ret == (int32_t)(LIND_GRATE_ERR & 0xffffffff)) {
        printf("[Cage|v2wide-real] FAIL: %s wrongly rejected\n", label);
        return 1;
    }

    int rc = 0;
    rc |= check_double("y[0]", y[0], alpha * x[0]);
    rc |= check_double("y[3]", y[3], alpha * x[2]);
    rc |= check_double("y[6]", y[6], alpha * x[4]);
    rc |= check_double("y[9]", y[9], alpha * x[6]);
    rc |= check_double("y[12]", y[12], alpha * x[8]);
    if (out_count != 5) { printf("[Cage|v2wide-real] FAIL: %s out_count=%u want=5\n", label, out_count); rc = 1; }
    if (ret != as_u32(y)) { printf("[Cage|v2wide-real] FAIL: %s alias-return ret=%u want=%u\n", label, ret, as_u32(y)); rc = 1; }
    return rc;
}

// Same valid inputs as call_and_check_basic, but expecting REJECTION --
// used by "stale" mode, where the target grate cage is already dead, not by
// any malformed-input case (that's call_narrow_expect_rejected below).
static int call_valid_expect_rejected(const char *label) {
    const int n = 5, incx = 2, incy = 3, extra = 7;
    double alpha = 2.5;
    double x[9]  = { 1, 0, 2, 0, 3, 0, 4, 0, 5 };
    double y[13] = { 0 };
    uint32_t out_count = 0xDEADBEEF;
    uint32_t ret = toy_wide_marshal(n, alpha, as_u32(x), incx, as_u32(y), incy,
                                     0, extra, as_u32(&out_count));
    if ((int32_t)ret != (int32_t)(LIND_GRATE_ERR & 0xffffffff)) {
        printf("[Cage|v2wide-real] FAIL: %s did not reject (ret=%u)\n", label, ret);
        return 1;
    }
    return 0;
}

static int call_narrow_expect_rejected(const char *label) {
    double x[1] = { 0 };
    double y[1] = { 0 };
    uint32_t out_count = 0;
    volatile int n = 536870913;
    uint32_t ret = toy_wide_marshal(n, 0.0, as_u32(x), 1, as_u32(y), 1, 0, 0, as_u32(&out_count));
    if ((int32_t)ret != (int32_t)(LIND_GRATE_ERR & 0xffffffff)) {
        printf("[Cage|v2wide-real] FAIL: %s did not reject (ret=%u)\n", label, ret);
        return 1;
    }
    return 0;
}

static int run_basic(void) {
    int rc = call_and_check_basic("basic");
    if (rc == 0) printf("[Cage|v2wide-real] PASS: basic\n");
    return rc;
}

static int run_narrow(void) {
    int rc = call_narrow_expect_rejected("narrow");
    if (rc == 0) printf("[Cage|v2wide-real] PASS: narrow\n");
    return rc;
}

// Proves worker reuse and trap cleanup: a successful call, a rejected call
// on the SAME worker (the pool defaults to one worker unless
// LIND_GRATE_WORKERS says otherwise), then another successful call -- the
// rejection must not corrupt the worker's Store/Instance/adapter cache for
// the call that follows it.
static int run_sequence(void) {
    int rc = 0;
    rc |= call_and_check_basic("sequence[0]");
    rc |= call_narrow_expect_rejected("sequence[1]");
    rc |= call_and_check_basic("sequence[2]");
    if (rc == 0) printf("[Cage|v2wide-real] PASS: sequence\n");
    return rc;
}

// Proves the V2 registration table's fork-copy: a real POSIX fork() from
// this app cage, with the parent's successful call as a baseline and the
// CHILD cage also making the same interposed call afterward. Without
// threei::copy_lib_handler_table_v2_to_cage (wired into fork_syscall), the
// child's own instance_dylink re-link would find no V2 registration under
// its own (new) cage id and this call would silently fall through to the
// real, uninterposed libtoy_wide_real_stub implementation instead --
// which prints its own FAIL line, so a false pass here is not possible.
static int run_fork(void) {
    if (call_and_check_basic("fork[parent-before]") != 0) return 1;

    pid_t pid = fork();
    if (pid < 0) {
        printf("[Cage|v2wide-real] FAIL: fork failed\n");
        return 1;
    }
    if (pid == 0) {
        int rc = call_and_check_basic("fork[child]");
        if (rc == 0) printf("[Cage|v2wide-real] PASS: fork[child]\n");
        return rc;
    }

    int status = 0;
    waitpid(pid, &status, 0);
    int child_rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    if (child_rc != 0) {
        printf("[Cage|v2wide-real] FAIL: child exited %d\n", child_rc);
        return 1;
    }
    printf("[Cage|v2wide-real] PASS: fork\n");
    return 0;
}

// Proves dispatch_lib_call_v2's cage-liveness check: this mode is only ever
// driven under v2wide_stale_grate.c, which registers this same symbol
// against a grate cage that has ALREADY exited (reaped) before this cage
// even starts. Expects a clean rejection, not a hang or crash.
static int run_stale(void) {
    int rc = call_valid_expect_rejected("stale");
    if (rc == 0) printf("[Cage|v2wide-real] PASS: stale\n");
    return rc;
}

#define N_CONCURRENT 4

struct concurrent_arg { int idx; int rc; };

// One thread's full call+check, entirely self-contained (no shared mutable
// state with any other thread) -- runs concurrently with the other threads
// below, all against the SAME grate's worker pool.  Each thread uses its
// own distinct `alpha`/`extra` so a genuinely corrupted cross-worker
// dispatch (e.g. one call reading another's arguments) would show up as a
// wrong numeric result for THIS thread, not just a coincidental pass.
static void *concurrent_worker(void *argp) {
    struct concurrent_arg *ca = (struct concurrent_arg *)argp;
    const int n = 5, incx = 2, incy = 3;
    const int extra = ca->idx;
    const double alpha = 2.0 + (double)ca->idx;
    double x[9]  = { 1, 0, 2, 0, 3, 0, 4, 0, 5 };
    double y[13] = { 0 };
    uint32_t out_count = 0xDEADBEEF;

    uint32_t ret = toy_wide_marshal(n, alpha, as_u32(x), incx, as_u32(y), incy,
                                     0, extra, as_u32(&out_count));
    int rc = 0;
    if ((int32_t)ret == (int32_t)(LIND_GRATE_ERR & 0xffffffff)) {
        rc = 1;
    } else {
        if (y[0] != alpha * x[0] || y[3] != alpha * x[2] || y[6] != alpha * x[4]
            || y[9] != alpha * x[6] || y[12] != alpha * x[8]) {
            rc = 1;
        }
        if (out_count != 5 || ret != as_u32(y)) {
            rc = 1;
        }
    }
    ca->rc = rc;
    return NULL;
}

// Proves the worker pool genuinely serves concurrent V2 calls from separate
// workers without cross-contaminating their arguments/results: N_CONCURRENT
// OS threads (LIND_GRATE_WORKERS defaults well above N_CONCURRENT) each
// make their own, distinctly-parameterized call at the same time.
static int run_concurrent(void) {
    pthread_t threads[N_CONCURRENT];
    struct concurrent_arg args[N_CONCURRENT];
    for (int i = 0; i < N_CONCURRENT; i++) {
        args[i].idx = i;
        args[i].rc = 1;
        if (pthread_create(&threads[i], NULL, concurrent_worker, &args[i]) != 0) {
            printf("[Cage|v2wide-real] FAIL: concurrent pthread_create[%d]\n", i);
            return 1;
        }
    }
    int rc = 0;
    for (int i = 0; i < N_CONCURRENT; i++) {
        pthread_join(threads[i], NULL);
        if (args[i].rc != 0) {
            printf("[Cage|v2wide-real] FAIL: concurrent worker %d produced wrong results\n", i);
            rc = 1;
        }
    }
    if (rc == 0) printf("[Cage|v2wide-real] PASS: concurrent\n");
    return rc;
}

// Proves the V2 portal's errno seed/relay (linker.rs's
// seed_grate_errno_from_caller/relay_grate_errno_to_caller): toy_set_errno's
// real handler runs inside the GRATE's own address space, whose errno write
// is otherwise invisible to this cage entirely -- this call must observe
// the SAME value in its own errno afterward. Runs under
// v2wide_errno_grate.c specifically (the only grate that registers
// toy_set_errno).
static int run_errno(void) {
    errno = 0;
    toy_set_errno(4242);
    if (errno != 4242) {
        printf("[Cage|v2wide-real] FAIL: errno got=%d want=4242\n", errno);
        return 1;
    }
    printf("[Cage|v2wide-real] PASS: errno\n");
    return 0;
}

// Proves a V2 registration survives exec(): this cage makes one successful
// call, then execs a FRESH copy of itself in "postexec" mode -- exec()
// replaces this cage's program image but keeps the same cage id, so
// Linker::instance_dylink's V2 portal check for the new image must still
// find the SAME (lib_handler_table_v2-keyed-by-cageid) registration
// v2wide_real_grate.c installed before the original execv into this
// program. Never returns on success (execv replaces this process).
static int run_exec(void) {
    if (call_and_check_basic("exec[pre]") != 0) return 1;
    printf("[Cage|v2wide-real] about to exec self\n");
    fflush(stdout);
    char *new_argv[] = { (char *)"/v2wide_real_cage.cwasm", (char *)"postexec", NULL };
    execv("/v2wide_real_cage.cwasm", new_argv);
    perror("execv");
    printf("[Cage|v2wide-real] FAIL: execv failed\n");
    return 1;
}

static int run_postexec(void) {
    int rc = call_and_check_basic("postexec");
    if (rc == 0) printf("[Cage|v2wide-real] PASS: exec\n");
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <mode>\n", argv[0]); return 2; }
    const char *mode = argv[1];
    if (strcmp(mode, "basic") == 0) return run_basic();
    if (strcmp(mode, "narrow") == 0) return run_narrow();
    if (strcmp(mode, "sequence") == 0) return run_sequence();
    if (strcmp(mode, "fork") == 0) return run_fork();
    if (strcmp(mode, "stale") == 0) return run_stale();
    if (strcmp(mode, "concurrent") == 0) return run_concurrent();
    if (strcmp(mode, "errno") == 0) return run_errno();
    if (strcmp(mode, "exec") == 0) return run_exec();
    if (strcmp(mode, "postexec") == 0) return run_postexec();
    fprintf(stderr, "unknown mode: %s\n", mode);
    return 2;
}
