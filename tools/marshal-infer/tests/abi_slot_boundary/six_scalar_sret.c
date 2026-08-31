// 6 visible scalar params + a hidden sret pointer = 7 raw ABI slots: one
// past the cap, must be rejected (force_local) even though every individual
// argument is an ordinary int -- the hidden sret slot is what pushes it
// over, and a check that only counts visible C-level params would miss it.
struct Big3 { int x, y, z; };

struct Big3 six_scalar_sret(int a, int b, int c, int d, int e, int f) {
    struct Big3 r = { a + b, c + d, e + f };
    return r;
}
