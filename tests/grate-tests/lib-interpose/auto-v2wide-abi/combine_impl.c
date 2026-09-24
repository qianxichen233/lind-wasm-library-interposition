// Real implementation for the sret+byval V2 ABI proof (issue #22, gate 7
// task 2: "add V2 generator/inference coverage for promised ABI shapes").
// Compiled TWICE by run_tests.sh: once via `lind_compile --emit-marshal`
// for REAL inference (not a hand-authored JSON spec -- see
// combine_sret_byval.marshal.json's own generation step), and once
// statically linked into combine_v2_real_grate.cwasm as the real backing
// implementation the generated adapter calls into.
//
// One function exercises BOTH promised-but-previously-untested-through-V2
// ABI shapes at once, each pushing the raw ABI slot count up by one beyond
// a plain scalar argument/return would:
//   - `struct Wide` (32 bytes) passed BY VALUE: too large to pass in
//     registers, so clang lowers it to a `byval` pointer argument --
//     Infer.cpp's own comment: "a byval/sret argument is just an ordinary
//     kind:'ptr' entry once marshal-infer lowers it."
//   - `struct Big3` (12 bytes) returned BY VALUE: too large to return in
//     registers, so clang lowers it to a hidden leading `sret` pointer
//     argument and the real wasm function returns void.
// Combined with 5 plain int arguments, the real lowered wasm signature is
// 7 raw ABI slots (sret ptr, byval ptr, 5 ints) -- one past V1's fixed
// LIND_RAW_ARGS_MAX=6, so this is V2-only by construction, the same way
// cblas_daxpby's 7-slot shape is (see auto-openblas-v2wide/).
struct Big3 { int x, y, z; };
struct Wide { double a, b, c, d; };

struct Big3 combine_sret_byval(struct Wide w, int a, int b, int c, int d, int e) {
    struct Big3 r;
    r.x = (int)(w.a + w.b) + a + b;
    r.y = (int)(w.c + w.d) + c + d;
    r.z = a + b + c + d + e;
    return r;
}
