// Cage exercising lind_marshal.h's spec-index bounds checks (badspec_grate.c
// registers both functions with deliberately out-of-range size_arg_index
// values). Both modes are expected to reject.
//
// Usage: <mode>
//   topindex    -- invalid top-level size_arg_index (memmove)
//   nestedindex -- invalid nested sibling size_arg_index (toy_buf_checksum)
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define LIND_GRATE_ERR (-536805379L)

extern void *memmove(void *dest, const void *src, size_t n);
struct toy_buffer { const char *data; unsigned len; };
extern int toy_buf_checksum(const struct toy_buffer *b);

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <mode>\n", argv[0]); return 2; }
    const char *mode = argv[1];
    long r;

    if (strcmp(mode, "topindex") == 0) {
        char src[16] = {0};
        char dst[16];
        volatile size_t n = 16;
        r = (long)(intptr_t)memmove(dst, src, n);
    } else if (strcmp(mode, "nestedindex") == 0) {
        char data[8] = {0};
        struct toy_buffer b = { .data = data, .len = sizeof(data) };
        r = toy_buf_checksum(&b);
    } else {
        fprintf(stderr, "unknown mode: %s\n", mode);
        return 2;
    }

    if (r == LIND_GRATE_ERR) {
        printf("[Cage|badspec] PASS: %s rejected (GRATE_ERR)\n", mode);
        return 0;
    }
    printf("[Cage|badspec] FAIL: %s did not reject (r=%ld)\n", mode, r);
    return 1;
}
