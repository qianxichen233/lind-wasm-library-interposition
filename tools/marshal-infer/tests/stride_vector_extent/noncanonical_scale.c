// The per-iteration step is genuinely 2*stride at the SOURCE level (e.g.
// interleaved-pair access), not an artifact of loop unrolling. Collapsing
// this to "stride=stride" would silently halve the real byte extent -- the
// analysis must reject the stride rather than guess, leaving this
// array-shaped-but-unsized (force_local), never a wrong StrideVector spec.
void interleaved_walk(int n, int stride, double *x) {
  int idx = 0;
  for (int i = 0; i < n; i++) {
    x[idx] = 0.0;
    idx += 2 * stride;
  }
}
