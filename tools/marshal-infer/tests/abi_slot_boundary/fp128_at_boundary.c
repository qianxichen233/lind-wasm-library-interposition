// 4 scalar params + 1 long double (fp128, split into 2 raw i64 slots by the
// wasm32 backend -- see Infer.cpp's lowerAbiReturn/inferVariadic neighbors
// for the general fp128-splitting rule) = 4 + 2 = 6 raw ABI slots: right at
// the cap, must still be accepted. A check counting the long double as one
// slot (its C-level arity) would wrongly see only 5 and miss the risk one
// slot later (see fp128_over_boundary.c).
int fp128_at_boundary(int a, int b, int c, int d, long double x) {
    return a + b + c + d + (int)x;
}
