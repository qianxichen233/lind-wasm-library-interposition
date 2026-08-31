// Grate probing lind_marshal.h's fail-closed paths: arena exhaustion,
// unconfigured pointer size, failed cross-cage copy, invalid handle,
// nested allocation/size failure, and pointer-array arena exhaustion.
//
// Interposes REAL functions (memcpy/strlen from libc, toy_ctx_get_val/
// toy_buf_checksum/toy_argv_len from libtoy) rather than invented symbols:
// register_lib_handler overrides an existing call site's dispatch target,
// it does not fabricate a callable symbol out of thin air, so a brand-new
// name with no real provider anywhere never reaches the handler at all.
// Each failure is triggered by a deliberately wrong spec or argument, not
// by the real function's own behavior.
//
// A tiny 256-byte arena (instead of the default 128KB) makes exhaustion
// reachable with small, safe buffers.
#define LIND_MARSHAL_ARENA_SIZE 256
#include <lind_syscall.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <assert.h>
#include <stdint.h>
#include <stddef.h>

#include "../lind_marshal.h"

extern void *memcpy(void *dest, const void *src, size_t n);
extern size_t strlen(const char *s);
extern int toy_ctx_get_val(void *ctx);
struct toy_buffer { const char *data; unsigned len; };
extern int toy_buf_checksum(const struct toy_buffer *b);
extern long toy_argv_len(const char **argv);

// --- memcpy: PTR IN/OUT + SCALAR n. Used for BOTH the arena-exhaustion
// and failed-copy probes -- which one triggers depends only on the cage's
// call arguments (see failclosed_cage.c), not on this spec.
static struct lind_marshal_spec memcpy_spec = {
    .nargs = 3,
    .args = {
        { .kind = LIND_ARG_PTR, .ptr_direction = LIND_PTR_OUT,
          .size_kind = LIND_SIZE_FROM_ARG, .size_arg_index = 2 },
        { .kind = LIND_ARG_PTR, .ptr_direction = LIND_PTR_IN,
          .size_kind = LIND_SIZE_FROM_ARG, .size_arg_index = 2 },
        { .kind = LIND_ARG_SCALAR },
    },
    .ret = { .kind = LIND_RET_PTR_ALIAS_ARG, .alias_arg_index = 0 },
};
static uint64_t handler_memcpy(uint64_t dest, uint64_t src, uint64_t n,
                                uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    printf("[Grate|fail-closed] memcpy handler ran (should not happen)\n");
    fflush(stdout);
    return LIND_RET_PTR(memcpy(LIND_AS_PTR(dest), LIND_AS_CPTR(src), LIND_AS_SIZE(n)));
}
LIND_DEFINE_MARSHAL_HANDLER(memcpy, &memcpy_spec, handler_memcpy)

// --- strlen: deliberately misconfigured spec (size_kind left as
// LIND_SIZE_NONE instead of LIND_SIZE_CSTR) -- the marshaller has no way
// to know how much of the source cage's string to copy.
static struct lind_marshal_spec strlen_bad_spec = {
    .nargs = 1,
    .args = {
        { .kind = LIND_ARG_PTR, .ptr_direction = LIND_PTR_IN },
    },
    .ret = { .kind = LIND_RET_SCALAR },
};
static uint64_t handler_strlen(uint64_t s, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    printf("[Grate|fail-closed] strlen handler ran (should not happen)\n");
    fflush(stdout);
    return LIND_RET_INT(strlen(LIND_AS_CSTR(s)));
}
LIND_DEFINE_MARSHAL_HANDLER(strlen, &strlen_bad_spec, handler_strlen)

// --- toy_ctx_get_val: HANDLE arg; cage passes a token that was never
// registered (no toy_ctx_create call anywhere in this test).
#define PROBE_HANDLE_CLASS 1
static struct lind_marshal_spec ctx_get_val_spec = {
    .nargs = 1,
    .args = {
        { .kind = LIND_ARG_HANDLE, .handle_class = PROBE_HANDLE_CLASS },
    },
    .ret = { .kind = LIND_RET_SCALAR },
};
static uint64_t handler_ctx_get_val(uint64_t real_ptr, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    printf("[Grate|fail-closed] toy_ctx_get_val handler ran (should not happen)\n");
    fflush(stdout);
    return LIND_RET_INT(toy_ctx_get_val(LIND_AS_PTR(real_ptr)));
}
LIND_DEFINE_MARSHAL_HANDLER(toy_ctx_get_val, &ctx_get_val_spec, handler_ctx_get_val)

// --- toy_buf_checksum: struct { const char *data; unsigned len; }, data
// sized by sibling field len -- cage passes len > the 256-byte test arena.
static struct lind_arg_spec _data_field_spec = {
    .kind = LIND_ARG_PTR, .ptr_direction = LIND_PTR_IN,
    .size_kind = LIND_SIZE_FROM_ARG, .size_arg_index = 1,  // sibling field 1 = len
};
static struct lind_field _buf_fields[2] = {
    { .offset = offsetof(struct toy_buffer, data), .spec = &_data_field_spec, .touched = 1 },
    { .offset = offsetof(struct toy_buffer, len),  .spec = NULL,               .touched = 1 },
};
static struct lind_layout _buf_layout = {
    .kind = LIND_LO_STRUCT, .nfields = 2, .fields = _buf_fields,
    .struct_size = sizeof(struct toy_buffer),
};
static struct lind_marshal_spec buf_checksum_spec = {
    .nargs = 1,
    .args = {
        { .kind = LIND_ARG_PTR, .ptr_direction = LIND_PTR_IN,
          .size_kind = LIND_SIZE_CONST, .const_size = sizeof(struct toy_buffer),
          .layout = &_buf_layout },
    },
    .ret = { .kind = LIND_RET_SCALAR },
};
static uint64_t handler_buf_checksum(uint64_t b, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    printf("[Grate|fail-closed] toy_buf_checksum handler ran (should not happen)\n");
    fflush(stdout);
    return LIND_RET_INT(toy_buf_checksum((const struct toy_buffer *)LIND_AS_PTR(b)));
}
LIND_DEFINE_MARSHAL_HANDLER(toy_buf_checksum, &buf_checksum_spec, handler_buf_checksum)

// --- toy_argv_len: NULL-terminated pointer array of cstrs; cage passes
// enough real strings to exceed the 256-byte test arena.
static struct lind_arg_spec _argv_elem_spec = {
    .kind = LIND_ARG_PTR, .ptr_direction = LIND_PTR_IN, .size_kind = LIND_SIZE_CSTR,
};
static struct lind_marshal_spec argv_len_spec = {
    .nargs = 1,
    .args = {
        { .kind = LIND_ARG_PTR, .ptr_direction = LIND_PTR_IN,
          .size_kind = LIND_SIZE_PTR_ARRAY, .element = &_argv_elem_spec },
    },
    .ret = { .kind = LIND_RET_SCALAR },
};
static uint64_t handler_argv_len(uint64_t argv, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    printf("[Grate|fail-closed] toy_argv_len handler ran (should not happen)\n");
    fflush(stdout);
    return LIND_RET_LONG(toy_argv_len((const char **)LIND_AS_PTR(argv)));
}
LIND_DEFINE_MARSHAL_HANDLER(toy_argv_len, &argv_len_spec, handler_argv_len)

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
        fprintf(stderr, "[Grate|fail-closed] invalid fn ptr\n");
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
    if (argc < 2) { fprintf(stderr, "Usage: %s <cage> <mode>\n", argv[0]); assert(0); }
    int grateid = getpid();
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); assert(0); }
    if (pid == 0) {
        int cageid = getpid();
        int ret;
        ret = register_lib_handler(cageid, "env", "memcpy", grateid, (uint64_t)(uintptr_t)&lind_mh_memcpy);
        if (ret != 0) { fprintf(stderr, "[Grate|fail-closed] register memcpy failed\n"); assert(0); }
        ret = register_lib_handler(cageid, "env", "strlen", grateid, (uint64_t)(uintptr_t)&lind_mh_strlen);
        if (ret != 0) { fprintf(stderr, "[Grate|fail-closed] register strlen failed\n"); assert(0); }
        ret = register_lib_handler(cageid, "env", "toy_ctx_get_val", grateid, (uint64_t)(uintptr_t)&lind_mh_toy_ctx_get_val);
        if (ret != 0) { fprintf(stderr, "[Grate|fail-closed] register toy_ctx_get_val failed\n"); assert(0); }
        ret = register_lib_handler(cageid, "env", "toy_buf_checksum", grateid, (uint64_t)(uintptr_t)&lind_mh_toy_buf_checksum);
        if (ret != 0) { fprintf(stderr, "[Grate|fail-closed] register toy_buf_checksum failed\n"); assert(0); }
        ret = register_lib_handler(cageid, "env", "toy_argv_len", grateid, (uint64_t)(uintptr_t)&lind_mh_toy_argv_len);
        if (ret != 0) { fprintf(stderr, "[Grate|fail-closed] register toy_argv_len failed\n"); assert(0); }

        printf("[Grate|fail-closed] registered 5/5 handlers\n");
        fflush(stdout);
        if (execv(argv[1], &argv[1]) == -1) { perror("execv"); assert(0); }
    }
    int status;
    while (wait(&status) > 0) {}
    int ce = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    fprintf(stderr, "[Grate|fail-closed] app exited %d\n", ce);
    return 0;
}
