// Cage exercising lind_marshal.h's fail-closed paths. Every mode below is
// expected to reject the call: the grate's handler never runs (confirmed
// separately by its own "handler ran" print being absent), and the call
// returns the runtime's GRATE_ERR sentinel to the caller rather than a
// real result. (Compiled with -fno-builtin: memcpy/strlen are clang
// builtins that would otherwise get inlined/folded away, bypassing
// interposition entirely -- see auto-memcpy.c/auto-libc/libc_app.c.)
//
// Usage: <mode>
//   arena    -- shadow-arena exhaustion (memcpy, n > the grate's 256-byte
//               test arena)
//   overflow -- near-SIZE_MAX allocation request (memcpy, n so close to
//               SIZE_MAX that size+7 wraps if unguarded, which would make
//               a huge request look like it trivially fits)
//   badcopy  -- failed cross-cage copy (memcpy from a bogus address)
//   unsized  -- unconfigured pointer size (strlen registered with a bad spec)
//   handle   -- invalid/never-registered handle token (toy_ctx_get_val)
//   nested   -- nested child allocation/size failure (toy_buf_checksum)
//   argv     -- pointer-array arena exhaustion (toy_argv_len)
//   ok       -- control: a real, valid memcpy -- must complete normally,
//               proving the other modes' rejections are real, not always-on
#include <stdio.h>
#include <stdint.h>
#include <string.h>

// threei::GRATE_ERR (src/threei/src/threei_const.rs), the sentinel a call
// into a grate returns when the grate side fails (here: traps via
// _lind_marshal_abort) instead of completing.
#define LIND_GRATE_ERR (-536805379L)

extern void *memcpy(void *dest, const void *src, size_t n);
extern size_t strlen(const char *s);
extern int toy_ctx_get_val(void *ctx);
struct toy_buffer { const char *data; unsigned len; };
extern int toy_buf_checksum(const struct toy_buffer *b);
extern long toy_argv_len(const char **argv);

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <mode>\n", argv[0]); return 2; }
    const char *mode = argv[1];
    long r;

    if (strcmp(mode, "arena") == 0) {
        char src[300] = {0};
        char dst[300];
        volatile size_t n = sizeof(src);  // > the grate's 256-byte test arena
        r = (long)(intptr_t)memcpy(dst, src, n);
    } else if (strcmp(mode, "overflow") == 0) {
        char src[16] = {0};
        char dst[16];
        // size_t is 32-bit on wasm32: SIZE_MAX here is 0xFFFFFFFF, and
        // SIZE_MAX+7 wraps in 32-bit arithmetic -- an unguarded
        // "aligned = (size+7)&~7" would treat this as a tiny request and
        // let it trivially "fit" in any arena. (The cross-cage copy's own
        // length/range validation is a second, independent layer that also
        // rejects a request this size -- this test proves the request is
        // rejected end to end, not that the allocator's own guard is the
        // only thing standing in the way.)
        volatile size_t n = (size_t)-1 - 3;
        r = (long)(intptr_t)memcpy(dst, src, n);
    } else if (strcmp(mode, "badcopy") == 0) {
        void *bogus = (void *)0xdeadbeef;
        char dst[16];
        volatile size_t n = 16;
        r = (long)(intptr_t)memcpy(dst, bogus, n);
    } else if (strcmp(mode, "unsized") == 0) {
        char s[] = "hello";
        r = (long)strlen(s);
    } else if (strcmp(mode, "handle") == 0) {
        r = toy_ctx_get_val((void *)(uintptr_t)999999u);
    } else if (strcmp(mode, "nested") == 0) {
        char data[300] = {0};  // > the grate's 256-byte test arena
        struct toy_buffer b = { .data = data, .len = sizeof(data) };
        r = toy_buf_checksum(&b);
    } else if (strcmp(mode, "argv") == 0) {
        // Each element needs its own shadow copy (LIND_SIZE_CSTR), so a
        // handful of modest strings is enough to exceed the 256-byte arena.
        static const char *fake_argv[] = {
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
            "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
            "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd",
            "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
            NULL,
        };
        r = toy_argv_len(fake_argv);
    } else if (strcmp(mode, "ok") == 0) {
        char src[16] = "0123456789abcde";
        char dst[16] = {0};
        volatile size_t n = sizeof(src);
        memcpy(dst, src, n);
        if (memcmp(dst, src, n) != 0) {
            fprintf(stderr, "[Cage|fail-closed] FAIL: ok memcpy produced wrong data\n");
            return 1;
        }
        printf("[Cage|fail-closed] PASS: ok completed normally\n");
        return 0;
    } else {
        fprintf(stderr, "unknown mode: %s\n", mode);
        return 2;
    }

    if (r == LIND_GRATE_ERR) {
        printf("[Cage|fail-closed] PASS: %s rejected (GRATE_ERR)\n", mode);
        return 0;
    }
    printf("[Cage|fail-closed] FAIL: %s did not reject (r=%ld)\n", mode, r);
    return 1;
}
