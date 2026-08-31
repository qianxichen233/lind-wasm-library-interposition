// Exactly LIND_RAW_ARGS_MAX (6) ordinary scalar slots, no sret, no
// multi-slot argument -- the plain case a bare "count the C params" reading
// would also get right. Included as the control alongside the sret/fp128
// boundary cases, which a plain C-level count gets wrong.
int six_ordinary(int a, int b, int c, int d, int e, int f) {
    return a + b + c + d + e + f;
}
