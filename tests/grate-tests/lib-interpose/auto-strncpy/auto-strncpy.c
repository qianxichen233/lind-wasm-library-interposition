// Cage for auto-strncpy marshalling test.
// Calls strncpy from libc and verifies copy and return alias.
#include <stdio.h>
#include <string.h>

extern char *strncpy(char *dest, const char *src, size_t n);

int main(void) {
    // src is sized to n bytes: the grate's spec requires it (see
    // auto-strncpy_grate.c). A shorter src is a separate, known marshalling
    // gap -- see auto-strncpy-short.c.
    char src[32] = "lind-wasm";
    char dst[32] = {0};
    // volatile: disable builtin lowering so the call stays interposable.
    volatile size_t n = sizeof(dst);

    char *ret = strncpy(dst, src, n);

    if (strncmp(dst, src, strlen(src)) != 0) {
        fprintf(stderr, "[Cage|auto-strncpy] FAIL: dst mismatch: got \"%s\"\n", dst);
        return 1;
    }
    if (ret != dst) {
        fprintf(stderr, "[Cage|auto-strncpy] FAIL: return %p != dst %p\n",
                (void *)ret, (void *)dst);
        return 1;
    }
    printf("[Cage|auto-strncpy] PASS: strncpy produced \"%s\", return == dst\n", dst);
    return 0;
}
