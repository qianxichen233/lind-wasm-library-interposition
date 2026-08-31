// Grate probing the pointer-field-tracking allocation itself (the
// ptf = _lind_marshal_alloc(nfields * sizeof(...)) call in the struct
// walk), as distinct from the per-field child-buffer allocations tested
// by failclosed_grate.c's "nested" mode.
//
// A 16-byte arena (vs. the default 128KB) leaves just enough room for the
// struct's own 8-byte shadow (toy_buffer: a 4-byte ptr + 4-byte unsigned)
// but not the 2*24=48 bytes the pointer-field-tracking array needs
// (sizeof(struct _lind_ptr_field_track) is 24 on wasm32) -- the exhaustion
// happens at the tracking allocation itself, before any per-field
// processing (including the data field's own, separately-tested,
// allocation) is even attempted.
//
// Separate grate (not failclosed_grate.c, arena=256): the correct
// toy_buf_checksum spec here would collide with failclosed_grate.c's
// register_lib_handler entry for the same symbol if run in the same grate,
// and neither grate's arena size suits the other test.
#define LIND_MARSHAL_ARENA_SIZE 16
#include <lind_syscall.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <assert.h>
#include <stdint.h>
#include <stddef.h>

#include "../lind_marshal.h"

struct toy_buffer { const char *data; unsigned len; };
extern int toy_buf_checksum(const struct toy_buffer *b);

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
    printf("[Grate|ptfexhaust] toy_buf_checksum handler ran (should not happen)\n");
    fflush(stdout);
    return LIND_RET_INT(toy_buf_checksum((const struct toy_buffer *)LIND_AS_PTR(b)));
}
LIND_DEFINE_MARSHAL_HANDLER(toy_buf_checksum, &buf_checksum_spec, handler_buf_checksum)

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
        fprintf(stderr, "[Grate|ptfexhaust] invalid fn ptr\n");
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
        int ret = register_lib_handler(cageid, "env", "toy_buf_checksum", grateid, (uint64_t)(uintptr_t)&lind_mh_toy_buf_checksum);
        if (ret != 0) { fprintf(stderr, "[Grate|ptfexhaust] register toy_buf_checksum failed\n"); assert(0); }

        printf("[Grate|ptfexhaust] registered 1/1 handlers\n");
        fflush(stdout);
        if (execv(argv[1], &argv[1]) == -1) { perror("execv"); assert(0); }
    }
    int status;
    while (wait(&status) > 0) {}
    int ce = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    fprintf(stderr, "[Grate|ptfexhaust] app exited %d\n", ce);
    return 0;
}
