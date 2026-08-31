// Grate probing lind_marshal.h's bounds checks on spec-provided indices --
// a misconfigured or corrupt lind_arg_spec must not turn into an
// out-of-bounds read, even though the index values come from the spec
// (grate-author-controlled), not the calling cage.
//
// Interposes REAL functions (memmove from libc, toy_buf_checksum from
// libtoy) with DELIBERATELY WRONG specs -- separate from failclosed_grate.c
// so the deliberately-broken toy_buf_checksum spec here doesn't collide
// with that grate's correct one (register_lib_handler keys on
// (cage, lib_name, symbol_name); the same symbol can't carry two specs in
// one grate).
#include <lind_syscall.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <assert.h>
#include <stdint.h>
#include <stddef.h>

#include "../lind_marshal.h"

extern void *memmove(void *dest, const void *src, size_t n);
struct toy_buffer { const char *data; unsigned len; };
extern int toy_buf_checksum(const struct toy_buffer *b);

// --- memmove: size_arg_index deliberately out of range (real memmove's n
// is arg index 2; LIND_RAW_ARGS_MAX is 6, so 99 is unreachably out of
// bounds for any real function's argument list). ---
static struct lind_marshal_spec memmove_bad_spec = {
    .nargs = 3,
    .args = {
        { .kind = LIND_ARG_PTR, .ptr_direction = LIND_PTR_OUT,
          .size_kind = LIND_SIZE_FROM_ARG, .size_arg_index = 99 },
        { .kind = LIND_ARG_PTR, .ptr_direction = LIND_PTR_IN,
          .size_kind = LIND_SIZE_FROM_ARG, .size_arg_index = 99 },
        { .kind = LIND_ARG_SCALAR },
    },
    .ret = { .kind = LIND_RET_PTR_ALIAS_ARG, .alias_arg_index = 0 },
};
static uint64_t handler_memmove(uint64_t dest, uint64_t src, uint64_t n,
                                 uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    printf("[Grate|badspec] memmove handler ran (should not happen)\n");
    fflush(stdout);
    return LIND_RET_PTR(memmove(LIND_AS_PTR(dest), LIND_AS_CPTR(src), LIND_AS_SIZE(n)));
}
LIND_DEFINE_MARSHAL_HANDLER(memmove, &memmove_bad_spec, handler_memmove)

// --- toy_buf_checksum: nested sibling size_arg_index deliberately out of
// range (the real struct has 2 fields, index 0/1; 99 is out of bounds for
// lo->fields[]). ---
static struct lind_arg_spec _data_field_bad_spec = {
    .kind = LIND_ARG_PTR, .ptr_direction = LIND_PTR_IN,
    .size_kind = LIND_SIZE_FROM_ARG, .size_arg_index = 99,
};
static struct lind_field _buf_fields_bad[2] = {
    { .offset = offsetof(struct toy_buffer, data), .spec = &_data_field_bad_spec, .touched = 1 },
    { .offset = offsetof(struct toy_buffer, len),  .spec = NULL,                   .touched = 1 },
};
static struct lind_layout _buf_layout_bad = {
    .kind = LIND_LO_STRUCT, .nfields = 2, .fields = _buf_fields_bad,
    .struct_size = sizeof(struct toy_buffer),
};
static struct lind_marshal_spec buf_checksum_bad_spec = {
    .nargs = 1,
    .args = {
        { .kind = LIND_ARG_PTR, .ptr_direction = LIND_PTR_IN,
          .size_kind = LIND_SIZE_CONST, .const_size = sizeof(struct toy_buffer),
          .layout = &_buf_layout_bad },
    },
    .ret = { .kind = LIND_RET_SCALAR },
};
static uint64_t handler_buf_checksum(uint64_t b, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;
    printf("[Grate|badspec] toy_buf_checksum handler ran (should not happen)\n");
    fflush(stdout);
    return LIND_RET_INT(toy_buf_checksum((const struct toy_buffer *)LIND_AS_PTR(b)));
}
LIND_DEFINE_MARSHAL_HANDLER(toy_buf_checksum, &buf_checksum_bad_spec, handler_buf_checksum)

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
        fprintf(stderr, "[Grate|badspec] invalid fn ptr\n");
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
        ret = register_lib_handler(cageid, "env", "memmove", grateid, (uint64_t)(uintptr_t)&lind_mh_memmove);
        if (ret != 0) { fprintf(stderr, "[Grate|badspec] register memmove failed\n"); assert(0); }
        ret = register_lib_handler(cageid, "env", "toy_buf_checksum", grateid, (uint64_t)(uintptr_t)&lind_mh_toy_buf_checksum);
        if (ret != 0) { fprintf(stderr, "[Grate|badspec] register toy_buf_checksum failed\n"); assert(0); }

        printf("[Grate|badspec] registered 2/2 handlers\n");
        fflush(stdout);
        if (execv(argv[1], &argv[1]) == -1) { perror("execv"); assert(0); }
    }
    int status;
    while (wait(&status) > 0) {}
    int ce = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    fprintf(stderr, "[Grate|badspec] app exited %d\n", ce);
    return 0;
}
