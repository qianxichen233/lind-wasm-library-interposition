// Cross-cage function-pointer callback feasibility probe: cage A.
//
// With no argument, makes one ordinary call: passes its own function
// pointer into the interposed `library_call` and expects the grate to call
// back into THIS instance before the outer call returns.
//
// With a repeat count N > 1, additionally proves the callback-proxy
// mechanism doesn't leak a table slot per call: makes N ordinary calls (a
// healthy implementation reuses one proxy slot throughout, not growing the
// grate's table every time), then one call whose callback deliberately
// traps (proving a failed callback surfaces as an ordinary rejected return
// value rather than crashing this cage), then one more ordinary call
// (proving the slot the trapping call used was reclaimed and is safely
// reusable, not left in a broken state).
//
// Compiled statically (-s) with the indirect-function table exported, so
// the host can resolve this cage's own table entries by export name.
//
// Compile:
//   lind-clang -s callback_cage.c -- -Wl,--export-table
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

// Mirrors `linker.rs`'s V2_GRATE_ERR sentinel, cast down to i32 (it fits
// without truncation): what a rejected or trapped interposed call returns
// through its result slot instead of a real value.
#define LIND_V2_GRATE_ERR ((int32_t)(-0x1FFF0003))

static int observed = 0;

static void callback(int value) {
    observed = value;
}

static void trapping_callback(int value) {
    (void)value;
    __builtin_trap();
}

extern int32_t library_call(void (*callback)(int));

static int run_normal_call(int expected) {
    observed = 0;
    int32_t ret = library_call(callback);
    if (ret == LIND_V2_GRATE_ERR) {
        printf("[Cage|gate0-callback] FAIL: unexpected rejection on an ordinary call\n");
        return 1;
    }
    if (observed != expected) {
        printf("[Cage|gate0-callback] FAIL: observed=%d expected %d\n", observed, expected);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int repeat_calls = (argc > 1) ? atoi(argv[1]) : 1;

    for (int i = 0; i < repeat_calls; i++) {
        if (run_normal_call(42) != 0) {
            return 1;
        }
    }

    if (repeat_calls <= 1) {
        printf("[Cage|gate0-callback] PASS: callback executed, observed=%d\n", observed);
        return 0;
    }

    observed = 0;
    int32_t trap_ret = library_call(trapping_callback);
    if (trap_ret != LIND_V2_GRATE_ERR) {
        printf("[Cage|gate0-callback] FAIL: trapping callback did not surface as rejected (ret=%d)\n",
               trap_ret);
        return 1;
    }
    printf("[Cage|gate0-callback] trapping callback correctly rejected\n");

    if (run_normal_call(42) != 0) {
        return 1;
    }
    printf("[Cage|gate0-callback] PASS: recovered after trap, callback executed, observed=%d\n",
           observed);
    return 0;
}
