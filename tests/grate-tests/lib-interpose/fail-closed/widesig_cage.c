// Cage exercising the >6-raw-ABI-slot portal rejection (widesig_grate.c
// registers a handler for toy_wide_sum7, whose real signature has 7 slots).
#include <stdio.h>

extern int toy_wide_sum7(int a, int b, int c, int d, int e, int f, int g);

int main(void) {
    int r = toy_wide_sum7(1, 2, 3, 4, 5, 6, 7);
    printf("[Cage|widesig] FAIL: call returned normally (r=%d)\n", r);
    fflush(stdout);
    return 0;
}
