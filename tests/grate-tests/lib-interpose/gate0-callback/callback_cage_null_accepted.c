// Cross-cage function-pointer callback: nullable contract probe, cage
// side. Calls the interposed `library_call` (via callback_grate_nullable.c,
// whose descriptor declares `nullable=1`) with a NULL function pointer.
// Expects the call to succeed ordinarily (no proxy installed, the real
// GRATE_ERR sentinel never surfaces) -- a null the contract explicitly
// allows must pass through, not be mistaken for a rejection.
//
// Compile:
//   lind-clang -s callback_cage_null_accepted.c -- -Wl,--export-table
#include <stdint.h>
#include <stdio.h>

#define LIND_V2_GRATE_ERR ((int32_t)(-0x1FFF0003))

extern int32_t library_call(void (*callback)(int));

int main(void) {
    int32_t ret = library_call((void (*)(int))0);
    if (ret == LIND_V2_GRATE_ERR) {
        printf("[Cage|gate0-callback-null-accepted] FAIL: unexpected rejection of a nullable "
               "NULL callback\n");
        return 1;
    }
    printf("[Cage|gate0-callback-null-accepted] PASS: NULL callback accepted (nullable)\n");
    return 0;
}
