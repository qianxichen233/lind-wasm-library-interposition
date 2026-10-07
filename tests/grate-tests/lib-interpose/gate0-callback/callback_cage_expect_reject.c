// Cross-cage function-pointer callback: ABI-lowering mismatch probe,
// cage side. Calls the interposed `library_call` with its own real,
// genuinely `(i32) -> ()` callback, against a grate (see
// callback_grate_abimismatch.c) that declared a mismatched signature for
// it. Expects the call to be REJECTED (the ordinary GRATE_ERR sentinel,
// not a trap) and the callback to never have run at all -- proving the
// mismatch is caught before any proxy is installed, not discovered by
// actually invoking the wrong-shaped function and having it misbehave.
//
// Compile:
//   lind-clang -s callback_cage_expect_reject.c -- -Wl,--export-table
#include <stdint.h>
#include <stdio.h>

#define LIND_V2_GRATE_ERR ((int32_t)(-0x1FFF0003))

static int observed = 0;

static void callback(int value) {
    observed = value;
}

extern int32_t library_call(void (*callback)(int));

int main(void) {
    int32_t ret = library_call(callback);
    if (ret != LIND_V2_GRATE_ERR) {
        printf("[Cage|gate0-callback-abimismatch] FAIL: expected rejection, got ret=%d\n", ret);
        return 1;
    }
    if (observed != 0) {
        printf("[Cage|gate0-callback-abimismatch] FAIL: callback ran despite the ABI mismatch "
               "(observed=%d)\n",
               observed);
        return 1;
    }
    printf("[Cage|gate0-callback-abimismatch] PASS: ABI mismatch rejected, callback never ran\n");
    return 0;
}
