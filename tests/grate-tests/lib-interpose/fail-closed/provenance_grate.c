// Grate exercising lind_marshal.h's post-call pointer-provenance checks
// (issue #7): a handler is ordinary interposed-library code from the
// runtime's point of view, and its OUT-direction writes and returns must be
// treated as untrusted just like any other library data. These handlers
// deliberately fabricate the values a buggy or adversarial library might
// leave behind, covering the three post-call translation paths:
//
//   toy_scan_buf       LIND_RET_PTR_INTO_ARG        (memchr-style return)
//   toy_strtol_like    out_ptr_into_arg1            (strtol-style **endptr)
//   toy_stream_process struct-field OUT/cursor       (zlib next_out-style)
//
// Each handler's behavior is selected by a value already present in its own
// arguments (never a separate "test mode" channel). One-past-the-end
// (pointer == shadow base + allocated length) is a valid pointer VALUE per
// C semantics, but never assumed globally permitted -- toy_scan_buf and
// toy_strtol_like model real memchr/strtol semantics, where a legitimate
// result is never exactly one-past, so their specs do NOT set
// LIND_ARGSPEC_ALLOW_ONE_PAST and a one-past value must be REJECTED;
// toy_stream_process models a cursor-advance OUT buffer (zlib's next_out),
// where ending up exactly one-past is the ordinary "buffer completely
// filled" outcome, so its field spec opts in and one-past must be ACCEPTED.
// See each handler's own mode table below -- they differ per function to
// match what a real implementation could legitimately produce.
#include <lind_syscall.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <assert.h>
#include <stdint.h>
#include <stddef.h>

#include "../lind_marshal.h"

// ---------------------------------------------------------------------------
// toy_scan_buf: LIND_RET_PTR_INTO_ARG
// ---------------------------------------------------------------------------
extern const char *toy_scan_buf(const char *buf, int c, unsigned n);

static struct lind_marshal_spec scan_buf_spec = {
    .nargs = 3,
    .args = {
        { .kind = LIND_ARG_PTR, .ptr_direction = LIND_PTR_IN,
          .size_kind = LIND_SIZE_FROM_ARG, .size_arg_index = 2 },
        { .kind = LIND_ARG_SCALAR },  // mode (repurposes memchr's "c")
        { .kind = LIND_ARG_SCALAR },  // n
    },
    .ret = { .kind = LIND_RET_PTR_INTO_ARG, .alias_arg_index = 0 },
};

static char _unrelated_buf1[8];

// Mode table (scan_buf_spec.ret does NOT set ALLOW_ONE_PAST -- a real
// memchr-alike never legitimately returns exactly one-past):
//   0 null          1 interior (offset 0)      2 one-past (now INVALID)
//   3 before-base   4 after-end                5 huge delta
//   6 unrelated object
//   7 high-bits-set: a genuinely-64-bit handler_ret whose LOW 32 bits alias
//     a valid interior address, but whose value as a whole is nowhere near
//     any real wasm32 address -- a candidate this wide can only reach
//     lind_marshal_dispatch through this specific path (LIND_RET_PTR_INTO_ARG
//     hands back the callee's raw uint64_t return value untouched; the
//     struct-field and out_ptr_into_arg1 paths both read the callee's
//     value out of a 4-byte shadow slot, so it's naturally already
//     truncated to 32 bits well before any validation runs).
static uint64_t handler_scan_buf(uint64_t buf, uint64_t mode, uint64_t n,
                                  uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    printf("[Grate|provenance] toy_scan_buf handler ran, mode=%d\n", (int)mode);
    fflush(stdout);
    uint8_t *base = (uint8_t *)LIND_AS_PTR(buf);
    size_t len = LIND_AS_SIZE(n);
    switch ((int)mode) {
        case 0: return 0;                                   // null
        case 1: return LIND_RET_PTR(base);                   // interior
        case 2: return LIND_RET_PTR(base + len);              // one-past
        case 3: return LIND_RET_PTR(base - 1);                // before-base
        case 4: return LIND_RET_PTR(base + len + 1);          // after-end
        case 7: return LIND_RET_PTR(base) | 0x100000000ULL;   // high bits set
        case 5: return LIND_RET_PTR(base + 0x40000000u);      // huge delta
        case 6: return LIND_RET_PTR(_unrelated_buf1);         // unrelated object
    }
    return 0;
}
LIND_DEFINE_MARSHAL_HANDLER(toy_scan_buf, &scan_buf_spec, handler_scan_buf)

// ---------------------------------------------------------------------------
// toy_strtol_like: out_ptr_into_arg1 (OUT pointer-to-pointer aliasing arg0)
// ---------------------------------------------------------------------------
extern long toy_strtol_like(const char *nptr, char **endptr);

static struct lind_marshal_spec strtol_like_spec = {
    .nargs = 2,
    .args = {
        { .kind = LIND_ARG_PTR, .ptr_direction = LIND_PTR_IN, .size_kind = LIND_SIZE_CSTR },
        { .kind = LIND_ARG_PTR, .ptr_direction = LIND_PTR_OUT, .size_kind = LIND_SIZE_CONST,
          .const_size = 4, .out_ptr_into_arg1 = 1 },
    },
    .ret = { .kind = LIND_RET_SCALAR },
};

static char _unrelated_buf2[8];

// Mode table (arg[1]'s out_ptr_into_arg1 spec does NOT set ALLOW_ONE_PAST --
// a real strtol's endptr never advances past the string's own NUL
// terminator, which is already the shadow's last INTERIOR byte, not
// one-past-the-end of the shadow):
//   0 null                       1 interior (offset 0, start of string)
//   2 interior-max (at the NUL, offset len-1 -- the real max endptr value)
//   3 one-past (offset len, now INVALID)   4 before-base
//   5 after one-past (offset len+1)        6 huge delta
//   7 unrelated object
static uint64_t handler_strtol_like(uint64_t nptr, uint64_t endptr_slot,
                                     uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    const char *base = (const char *)LIND_AS_CPTR(nptr);
    char mode = base[0];  // nptr's first byte selects the scenario
    printf("[Grate|provenance] toy_strtol_like handler ran, mode='%c'\n", mode);
    fflush(stdout);
    size_t len = strlen(base) + 1;  // matches the CSTR shadow's allocated length
    uint32_t *out = (uint32_t *)LIND_AS_PTR(endptr_slot);
    uint8_t *val;
    switch (mode) {
        case '0': val = 0; break;
        case '1': val = (uint8_t *)base; break;                    // interior
        case '2': val = (uint8_t *)base + len - 1; break;           // interior-max (at the NUL)
        case '3': val = (uint8_t *)base + len; break;                // one-past
        case '4': val = (uint8_t *)base - 1; break;                  // before-base
        case '5': val = (uint8_t *)base + len + 1; break;            // after one-past
        case '6': val = (uint8_t *)base + 0x40000000u; break;        // huge delta
        case '7': val = (uint8_t *)_unrelated_buf2; break;           // unrelated object
        default:  val = 0;
    }
    *out = (uint32_t)(uintptr_t)val;
    return 0;
}
LIND_DEFINE_MARSHAL_HANDLER(toy_strtol_like, &strtol_like_spec, handler_strtol_like)

// ---------------------------------------------------------------------------
// toy_stream_process: struct-field OUT/cursor (next_out advance + fixup)
// ---------------------------------------------------------------------------
struct toy_stream { const char *next_in; unsigned avail_in; char *next_out; unsigned avail_out; };
extern int toy_stream_process(struct toy_stream *s);

static struct lind_arg_spec _next_out_fspec = {
    .kind = LIND_ARG_PTR, .ptr_direction = LIND_PTR_OUT,
    .size_kind = LIND_SIZE_FROM_ARG, .size_arg_index = 3,  // sibling: avail_out
    // A completely filled output buffer is the normal outcome for a
    // cursor-advance OUT buffer, not an edge case (mirrors zlib's next_out).
    .flags = LIND_ARGSPEC_ALLOW_ONE_PAST,
};
static struct lind_field _stream_fields[4] = {
    { .offset = offsetof(struct toy_stream, next_in),   .spec = NULL,             .touched = 0 },
    { .offset = offsetof(struct toy_stream, avail_in),  .spec = NULL,             .touched = 0 },
    { .offset = offsetof(struct toy_stream, next_out),  .spec = &_next_out_fspec, .touched = 1 },
    { .offset = offsetof(struct toy_stream, avail_out), .spec = NULL,             .touched = 1 },
};
static struct lind_layout _stream_layout = {
    .kind = LIND_LO_STRUCT, .nfields = 4, .fields = _stream_fields,
    .struct_size = sizeof(struct toy_stream),
};
static struct lind_marshal_spec stream_process_spec = {
    .nargs = 1,
    .args = {
        { .kind = LIND_ARG_PTR, .ptr_direction = LIND_PTR_INOUT,
          .size_kind = LIND_SIZE_CONST, .const_size = sizeof(struct toy_stream),
          .layout = &_stream_layout },
    },
    .ret = { .kind = LIND_RET_SCALAR },
};

// Mirrors struct toy_stream's wasm32 layout (all pointers are uint32_t) so
// the handler can read/write next_out's raw shadow-pointer field directly.
struct toy_stream_wasm32 { uint32_t next_in; uint32_t avail_in; uint32_t next_out; uint32_t avail_out; };

// Mode table (_next_out_fspec DOES set ALLOW_ONE_PAST -- a fully-filled
// cursor-advance buffer is the normal outcome):
//   0 interior, unchanged (offset 0)     1 one-past (offset avail_out)
//   2 null (no output written)           3 before-base
//   4 after-end (offset avail_out+1)     5 huge delta
//   6 unrelated object
static uint64_t handler_stream_process(uint64_t s_u64, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    struct toy_stream_wasm32 *s = (struct toy_stream_wasm32 *)LIND_AS_PTR(s_u64);
    uint32_t mode = s->avail_in;  // avail_in is otherwise unused here -- repurposed as mode
    printf("[Grate|provenance] toy_stream_process handler ran, mode=%u\n", mode);
    fflush(stdout);
    uint8_t *base = (uint8_t *)(uintptr_t)s->next_out;  // == this field's own shadow_start
    uint32_t avail_out = s->avail_out;
    uint8_t *val;
    switch (mode) {
        case 0: val = base; break;                            // interior, unchanged
        case 1: val = base + avail_out; break;                  // one-past
        case 2: val = 0; break;                                 // null (no output)
        case 3: val = base - 1; break;                          // before-base
        case 4: val = base + avail_out + 1; break;               // after-end
        case 5: val = base + 0x40000000u; break;                 // huge delta
        case 6: { static uint8_t u[8]; val = u; break; }         // unrelated object
        default: val = base;
    }
    s->next_out = (uint32_t)(uintptr_t)val;
    return 0;
}
LIND_DEFINE_MARSHAL_HANDLER(toy_stream_process, &stream_process_spec, handler_stream_process)

// ---------------------------------------------------------------------------
// Standard grate dispatcher — required export in every grate.
// ---------------------------------------------------------------------------
int64_t pass_fptr_to_wt(uint64_t fn_ptr_uint, uint64_t cageid,
                    uint64_t arg1, uint64_t arg1cage,
                    uint64_t arg2, uint64_t arg2cage,
                    uint64_t arg3, uint64_t arg3cage,
                    uint64_t arg4, uint64_t arg4cage,
                    uint64_t arg5, uint64_t arg5cage,
                    uint64_t arg6, uint64_t arg6cage) {
    if (fn_ptr_uint == 0) {
        fprintf(stderr, "[Grate|provenance] invalid fn ptr\n");
        assert(0);
    }
    int64_t (*fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
              uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
              uint64_t, uint64_t, uint64_t) =
        (int64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                 uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                 uint64_t, uint64_t, uint64_t))(uintptr_t)fn_ptr_uint;
    return fn(cageid, arg1, arg1cage, arg2, arg2cage,
              arg3, arg3cage, arg4, arg4cage,
              arg5, arg5cage, arg6, arg6cage);
}

int main(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <cage>\n", argv[0]); assert(0); }
    int grateid = getpid();
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); assert(0); }
    if (pid == 0) {
        int cageid = getpid();
        int ret;
        ret = register_lib_handler(cageid, "env", "toy_scan_buf", grateid, (uint64_t)(uintptr_t)&lind_mh_toy_scan_buf);
        if (ret != 0) { fprintf(stderr, "[Grate|provenance] register toy_scan_buf failed\n"); assert(0); }
        ret = register_lib_handler(cageid, "env", "toy_strtol_like", grateid, (uint64_t)(uintptr_t)&lind_mh_toy_strtol_like);
        if (ret != 0) { fprintf(stderr, "[Grate|provenance] register toy_strtol_like failed\n"); assert(0); }
        ret = register_lib_handler(cageid, "env", "toy_stream_process", grateid, (uint64_t)(uintptr_t)&lind_mh_toy_stream_process);
        if (ret != 0) { fprintf(stderr, "[Grate|provenance] register toy_stream_process failed\n"); assert(0); }

        printf("[Grate|provenance] registered 3/3 handlers\n");
        fflush(stdout);
        if (execv(argv[1], &argv[1]) == -1) { perror("execv"); assert(0); }
    }
    int status;
    while (wait(&status) > 0) {}
    int ce = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    fprintf(stderr, "[Grate|provenance] app exited %d\n", ce);
    return 0;
}
