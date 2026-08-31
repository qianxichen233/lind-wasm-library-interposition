// Cage exercising the pointer-field-tracking allocation exhaustion path
// (ptfexhaust_grate.c's 16-byte test arena). No mode argument: this grate
// only registers one function.
#include <stdio.h>

#define LIND_GRATE_ERR (-536805379L)

struct toy_buffer { const char *data; unsigned len; };
extern int toy_buf_checksum(const struct toy_buffer *b);

int main(void) {
    // A tiny, valid data buffer: the struct's own 8-byte shadow allocation
    // must succeed (leaving the tracking-array allocation, not this field,
    // as what actually exhausts the arena).
    char data[1] = {0};
    struct toy_buffer b = { .data = data, .len = sizeof(data) };
    long r = toy_buf_checksum(&b);

    if (r == LIND_GRATE_ERR) {
        printf("[Cage|ptfexhaust] PASS: rejected (GRATE_ERR)\n");
        fflush(stdout);
        return 0;
    }
    printf("[Cage|ptfexhaust] FAIL: did not reject (r=%ld)\n", r);
    fflush(stdout);
    return 1;
}
