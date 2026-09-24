// Gate 7 task 2 (issue #22): calls the REAL sret+byval-lowered
// combine_sret_byval with its true, direct signature -- ordinary C (struct
// Wide passed by value, struct Big3 returned by value); clang's own wasm32
// ABI lowering produces the exact same byval/sret-pointer shape
// combine_impl.c's real definition and marshal-infer's inference agree on,
// so this file needs no manual pointer plumbing at all (contrast
// auto-openblas-v2wide/daxpby_v2_real_cage.c, which manually passes
// uint32_t addresses because its real arguments ARE pointers at the C
// level already). Interposed entirely through the real V2 production path
// via combine_v2_real_grate.c.
//
// Prints its own results the SAME way combine_baseline.c does -- run_tests.sh
// diffs the two programs' outputs directly rather than hand-deriving an
// expected value here.
#include <stdio.h>

struct Big3 { int x, y, z; };
struct Wide { double a, b, c, d; };

extern struct Big3 combine_sret_byval(struct Wide w, int a, int b, int c, int d, int e);

int main(void) {
    struct Wide w = { 1.5, 2.5, 3.5, 4.5 };
    struct Big3 r = combine_sret_byval(w, 10, 20, 30, 40, 50);

    printf("[Cage|combine-v2] r.x=%d\n", r.x);
    printf("[Cage|combine-v2] r.y=%d\n", r.y);
    printf("[Cage|combine-v2] r.z=%d\n", r.z);
    return 0;
}
