// Cage exercising lind_marshal.h's post-call pointer-provenance checks
// (issue #7) against provenance_grate.c's three deliberately-adversarial
// handlers. See that file's header for the shared mode table.
//
// Each target loops over every one of its modes in a single call (a
// _lind_marshal_abort trap inside the grate worker only fails that one
// interposed call -- see threei::GRATE_ERR's doc -- so the cage process
// itself keeps running normally across every mode).
//
// Usage: <target>
//   scan    -- LIND_RET_PTR_INTO_ARG   (toy_scan_buf),     modes 0..7
//   strtol  -- out_ptr_into_arg1        (toy_strtol_like),  modes 0..7
//   stream  -- struct-field OUT/cursor  (toy_stream_process), modes 0..6
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#define LIND_GRATE_ERR (-536805379L)

extern const char *toy_scan_buf(const char *buf, int c, unsigned n);
extern long toy_strtol_like(const char *nptr, char **endptr);
struct toy_stream { const char *next_in; unsigned avail_in; char *next_out; unsigned avail_out; };
extern int toy_stream_process(struct toy_stream *s);

static int run_scan(void) {
    char buf[8] = "abcdefg";
    volatile unsigned n = sizeof(buf) - 1;
    int fail = 0;

    for (int mode = 0; mode <= 7; mode++) {
        const char *r = toy_scan_buf(buf, mode, n);
        long rv = (long)(intptr_t)r;
        switch (mode) {
            case 0: // null: valid, expect exactly NULL
                if (r != NULL) { printf("[Cage|provenance] FAIL: scan mode 0 expected NULL, got %ld\n", rv); fail = 1; }
                break;
            case 1: // interior: valid, expect exactly &buf[0]
                if (r != buf) { printf("[Cage|provenance] FAIL: scan mode 1 expected &buf[0], got %ld\n", rv); fail = 1; }
                break;
            default: // 2..7: invalid, must be rejected -- toy_scan_buf's
                     // spec does not set ALLOW_ONE_PAST, so mode 2
                     // (one-past) is invalid here too, unlike stream's;
                     // mode 7's candidate has high bits set (see the
                     // grate's handler comment) and must be rejected before
                     // narrowing to 32 bits, not aliased into range by it.
                if (rv != LIND_GRATE_ERR) { printf("[Cage|provenance] FAIL: scan mode %d did not reject (r=%ld)\n", mode, rv); fail = 1; }
                break;
        }
    }
    if (!fail) printf("[Cage|provenance] PASS: scan (all 8 modes)\n");
    return fail;
}

static int run_strtol(void) {
    int fail = 0;

    for (int mode = 0; mode <= 7; mode++) {
        // Padded well past _lind_measure_cstr's 64-byte-chunk over-read (it
        // scans past the NUL looking for one, up to a full chunk at a time)
        // so that read never runs past this array's own mapped stack slot.
        char nptr[128] = { 0 };
        nptr[0] = (char)('0' + mode);
        size_t len = strlen(nptr) + 1;  // matches the CSTR shadow's allocated length
        char *endptr = (char *)(intptr_t)0x12345678; // sentinel, must be overwritten
        long rv = toy_strtol_like(nptr, &endptr);
        switch (mode) {
            case 0: // null: valid, expect exactly NULL
                if (endptr != NULL) { printf("[Cage|provenance] FAIL: strtol mode 0 expected NULL endptr, got %ld\n", (long)(intptr_t)endptr); fail = 1; }
                break;
            case 1: // interior: valid, expect exactly nptr
                if (endptr != nptr) { printf("[Cage|provenance] FAIL: strtol mode 1 expected nptr, got %ld\n", (long)(intptr_t)endptr); fail = 1; }
                break;
            case 2: // interior-max (at the NUL, offset len-1): valid,
                    // expect exactly nptr+len-1 -- the real max endptr value
                if (endptr != nptr + len - 1) { printf("[Cage|provenance] FAIL: strtol mode 2 expected nptr+len-1, got %ld\n", (long)(intptr_t)endptr); fail = 1; }
                break;
            default: // 3..7: invalid, must be rejected -- out_ptr_into_arg1
                     // here does not set ALLOW_ONE_PAST, so mode 3
                     // (one-past the NUL) is invalid too, unlike stream's.
                if (rv != LIND_GRATE_ERR) { printf("[Cage|provenance] FAIL: strtol mode %d did not reject (r=%ld)\n", mode, rv); fail = 1; }
                break;
        }
    }
    if (!fail) printf("[Cage|provenance] PASS: strtol (all 8 modes)\n");
    return fail;
}

static int run_stream(void) {
    int fail = 0;

    for (int mode = 0; mode <= 6; mode++) {
        char in[4] = "wxyz";
        char out[4] = { 0x7f, 0x7f, 0x7f, 0x7f };
        struct toy_stream s = { .next_in = in, .avail_in = (unsigned)mode, .next_out = out, .avail_out = sizeof(out) };
        long rv = toy_stream_process(&s);
        switch (mode) {
            case 0: // interior, unchanged: valid, must not reject
            case 1: // one-past: valid, must not reject -- _next_out_fspec
                    // DOES set ALLOW_ONE_PAST, unlike scan/strtol's specs.
                if (rv == LIND_GRATE_ERR) { printf("[Cage|provenance] FAIL: stream mode %d wrongly rejected\n", mode); fail = 1; }
                break;
            case 2: // null (no output written): valid, expect next_out == NULL
                if (rv == LIND_GRATE_ERR) { printf("[Cage|provenance] FAIL: stream mode 2 wrongly rejected\n"); fail = 1; }
                if (s.next_out != NULL) { printf("[Cage|provenance] FAIL: stream mode 2 expected next_out == NULL, got %ld\n", (long)(intptr_t)s.next_out); fail = 1; }
                break;
            default: // 3..6: invalid, must be rejected. Also: validation
                     // must fail before ANY caller-memory write, so s.next_out
                     // must still be exactly the cage's own original pointer
                     // here -- never the raw grate shadow address the
                     // handler left in it (which a blind copy-back-then-
                     // validate ordering would have exposed before the abort).
                if (rv != LIND_GRATE_ERR) { printf("[Cage|provenance] FAIL: stream mode %d did not reject (r=%ld)\n", mode, rv); fail = 1; }
                if (s.next_out != out) { printf("[Cage|provenance] FAIL: stream mode %d leaked a raw pointer into next_out (got %ld)\n", mode, (long)(intptr_t)s.next_out); fail = 1; }
                break;
        }
    }
    if (!fail) printf("[Cage|provenance] PASS: stream (all 7 modes)\n");
    return fail;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <target>\n", argv[0]); return 2; }
    const char *target = argv[1];

    if (strcmp(target, "scan") == 0) return run_scan();
    if (strcmp(target, "strtol") == 0) return run_strtol();
    if (strcmp(target, "stream") == 0) return run_stream();

    fprintf(stderr, "unknown target: %s\n", target);
    return 2;
}
