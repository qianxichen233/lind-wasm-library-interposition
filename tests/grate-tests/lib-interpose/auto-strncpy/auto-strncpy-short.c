// Regression guard: src is shorter than n (see auto-strncpy_grate.c's spec,
// which sizes src's shadow copy from n). Kept separate from auto-strncpy.c,
// which uses a correctly n-sized src.
#include <stdio.h>
#include <string.h>

extern char *strncpy(char *dest, const char *src, size_t n);

int main(void) {
    char src[] = "lind-wasm";
    char dst[32] = {0};
    volatile size_t n = sizeof(dst);

    strncpy(dst, src, n);
    printf("[Cage|auto-strncpy-short] dst=\"%s\"\n", dst);
    return 0;
}
