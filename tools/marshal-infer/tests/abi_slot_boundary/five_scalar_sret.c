// 5 visible scalar params + a hidden sret pointer (the struct return doesn't
// fit in registers under the wasm32-wasi ABI, so clang lowers it to a
// synthetic leading out-pointer argument) = 6 raw ABI slots: right at the
// cap, must still be accepted.
struct Big3 { int x, y, z; };

struct Big3 five_scalar_sret(int a, int b, int c, int d, int e) {
    struct Big3 r = { a + b, c + d, e };
    return r;
}
