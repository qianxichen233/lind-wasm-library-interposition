// Cross-cage function-pointer callback: non-nullable contract, NULL
// callback probe. Calls the interposed `library_call` (via
// callback_grate.c, whose descriptor declares `nullable=0`) with a NULL
// function pointer. Expects rejection (the ordinary GRATE_ERR sentinel,
// not a trap) -- GrateWorker::install_callback_proxies must check
// `nullable` before treating index 0 as "nothing to proxy, pass through."
//
// Compile:
//   lind-clang -s callback_cage_null_rejected.c -- -Wl,--export-table
#include <stdint.h>
#include <stdio.h>

#define LIND_V2_GRATE_ERR ((int32_t)(-0x1FFF0003))

extern int32_t library_call(void (*callback)(int));

int main(void) {
    int32_t ret = library_call((void (*)(int))0);
    if (ret != LIND_V2_GRATE_ERR) {
        printf("[Cage|gate0-callback-null-rejected] FAIL: expected rejection, got ret=%d\n", ret);
        return 1;
    }
    printf("[Cage|gate0-callback-null-rejected] PASS: NULL callback rejected (not nullable)\n");
    return 0;
}
