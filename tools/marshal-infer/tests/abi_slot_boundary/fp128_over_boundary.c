// 5 scalar params + 1 long double (2 raw i64 slots) = 5 + 2 = 7 raw ABI
// slots: one past the cap, must be rejected (force_local). The multi-slot
// long double is what crosses the boundary here, not an extra scalar param.
int fp128_over_boundary(int a, int b, int c, int d, int e, long double x) {
    return a + b + c + d + e + (int)x;
}
