// Exercises the gen_grate.py-GENERATED toy_vec_scale handler -- built
// straight from a real, freshly-compiled marshal-infer inference record
// (issue -- constant-sourced StrideVector extent operands), not a
// hand-written lind_marshal_spec (compare fail-closed/stridevec_grate.c,
// which isolates the evaluator by hand instead). More than one element is
// required: n==1 can't distinguish a correct constant-stride extent
// computation (spanning all n elements) from an accidental single-element
// copy.
#include <stdio.h>

extern void toy_vec_scale(int n, double factor, double *x);

int main(void) {
    const int n = 5;
    double x[5] = { 1, 2, 3, 4, 5 };
    double expected[5] = { 2, 4, 6, 8, 10 };

    toy_vec_scale(n, 2.0, x);

    for (int i = 0; i < n; i++) {
        if (x[i] != expected[i]) {
            printf("[Cage|conststride] FAIL: toy_vec_scale x[%d]=%g expected %g\n",
                   i, x[i], expected[i]);
            return 1;
        }
    }
    printf("[Cage|conststride] PASS: toy_vec_scale\n");
    return 0;
}
