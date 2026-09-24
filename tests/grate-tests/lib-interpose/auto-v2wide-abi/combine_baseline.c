// Same-cage baseline for the sret+byval V2 ABI proof: a plain,
// non-interposed program statically linking combine_impl.c directly (no
// dylink/grate/marshalling anywhere) and calling combine_sret_byval with
// the same fixed inputs combine_v2_real_cage.c uses. run_tests.sh diffs
// this program's own stdout against the interposed cage's, the same
// "compare against a real, uninterposed run of the same real code" proof
// auto-openblas-v2wide's daxpby_baseline.c uses.
#include <stdio.h>

struct Big3 { int x, y, z; };
struct Wide { double a, b, c, d; };

extern struct Big3 combine_sret_byval(struct Wide w, int a, int b, int c, int d, int e);

int main(void) {
    struct Wide w = { 1.5, 2.5, 3.5, 4.5 };
    struct Big3 r = combine_sret_byval(w, 10, 20, 30, 40, 50);

    printf("[Baseline|combine-v2] r.x=%d\n", r.x);
    printf("[Baseline|combine-v2] r.y=%d\n", r.y);
    printf("[Baseline|combine-v2] r.z=%d\n", r.z);
    return 0;
}
