// Peeled-first-iteration StrideVector recovery. OpenBLAS's isamax_k and
// 27 siblings peel the first
// element out of the loop by hand:
//
//   if (n <= 0 || inc_x <= 0) return(max);
//   maxf = ABS(x[0]);
//   ix += inc_x;  i++;
//   while (i < n) { ...x[ix]...; ix += inc_x; i++; }
//
// The address induction variable `ix` provably starts at `inc_x`, not 0 --
// but combined with the peeled x[0] access, the total footprint is exactly
// {0, inc_x, 2*inc_x, ..., (n-1)*inc_x}, the SAME set the ordinary
// (length=n, stride=inc_x) envelope already describes. detectPeeledPrefixBound
// (Infer.cpp) proves this directly from the IR: the address IV starts at
// exactly the already-proven stride, the loop's own trip count is exactly
// (length argument)-1, and a dominating offset-zero access -- reached only
// once the length argument is proven positive -- exists elsewhere on the
// same pointer. Every one of those conditions is independently checked;
// this file's negative cases each violate exactly one.

// ---- positive cases ----

// The canonical shape: CBLAS-style operands (by value), read-only.
double peeled_max(int n, double *x, int inc_x) {
  double maxf;
  int ix, i;
  if (n <= 0 || inc_x <= 0) return 0.0;
  maxf = x[0];
  ix = inc_x;
  for (i = 1; i < n; i++) {
    if (x[ix] > maxf) maxf = x[ix];
    ix += inc_x;
  }
  return maxf;
}

// A differently-named, semantically distinct equivalent (min instead of
// max) -- proves this isn't matched by name or by the specific comparison
// used inside the loop body.
double peeled_min(int n, double *x, int inc_x) {
  double minf;
  int ix, i;
  if (n <= 0 || inc_x <= 0) return 0.0;
  minf = x[0];
  ix = inc_x;
  for (i = 1; i < n; i++) {
    if (x[ix] < minf) minf = x[ix];
    ix += inc_x;
  }
  return minf;
}

// Classic Fortran BLAS calling convention: every scalar passed by
// reference.
double peeled_max_fortran(int *N, double *x, int *INCX) {
  int n = *N, inc_x = *INCX;
  double maxf;
  int ix, i;
  if (n <= 0 || inc_x <= 0) return 0.0;
  maxf = x[0];
  ix = inc_x;
  for (i = 1; i < n; i++) {
    if (x[ix] > maxf) maxf = x[ix];
    ix += inc_x;
  }
  return maxf;
}

// One-hop delegation: a CBLAS-style wrapper with no loop of its own,
// delegating to an internal kernel that has the actual peeled loop --
// OpenBLAS's real cblas_isamax/isamax_k shape.
static double peeled_max_kernel(int n, double *x, int inc_x) {
  double maxf;
  int ix, i;
  if (n <= 0 || inc_x <= 0) return 0.0;
  maxf = x[0];
  ix = inc_x;
  for (i = 1; i < n; i++) {
    if (x[ix] > maxf) maxf = x[ix];
    ix += inc_x;
  }
  return maxf;
}
double peeled_max_wrapper(int n, double *x, int inc_x) {
  return peeled_max_kernel(n, x, inc_x);
}

// ---- negative cases: each violates exactly one required condition ----

// Missing x[0]: the address IV starts at inc_x (not 0), but nothing
// peels a companion access at offset 0 anywhere -- must remain rejected
// (identical in spirit to nonzero_start.c, but with a trip count of n-1
// instead of n, so it specifically exercises the peeled-length proof
// finding no corroborating access rather than the ordinary proof's
// zero-start check).
double neg_missing_peel(int n, double *x, int inc_x) {
  double s = 0.0;
  int ix, i;
  if (n <= 0 || inc_x <= 0) return 0.0;
  ix = inc_x;
  for (i = 1; i < n; i++) {
    s += x[ix];
    ix += inc_x;
  }
  return s;
}

// Peeled access at the wrong offset (x[1], not x[0]).
double neg_peel_wrong_offset(int n, double *x, int inc_x) {
  double maxf;
  int ix, i;
  if (n <= 0 || inc_x <= 0) return 0.0;
  maxf = x[1];
  ix = inc_x;
  for (i = 1; i < n; i++) {
    if (x[ix] > maxf) maxf = x[ix];
    ix += inc_x;
  }
  return maxf;
}

// The address IV starts at 2*inc_x, not inc_x -- doesn't match the
// already-proven stride, so no peeled candidate is even formed.
double neg_peel_wrong_start(int n, double *x, int inc_x) {
  double maxf;
  int ix, i;
  if (n <= 0 || inc_x <= 0) return 0.0;
  maxf = x[0];
  ix = 2 * inc_x;
  for (i = 1; i < n; i++) {
    if (x[ix] > maxf) maxf = x[ix];
    ix += inc_x;
  }
  return maxf;
}

// Inclusive loop bound (i<=n instead of i<n): the loop's own trip count
// is n, not n-1, so unwrapArgumentMinusOne correctly finds no match --
// the ordinary proof also fails (address doesn't start at 0), so this
// stays array-shaped-no-stride, not a peeled pairing.
double neg_peel_inclusive_bound(int n, double *x, int inc_x) {
  double maxf;
  int ix, i;
  if (n <= 0 || inc_x <= 0) return 0.0;
  maxf = x[0];
  ix = inc_x;
  for (i = 1; i <= n; i++) {
    if (x[ix] > maxf) maxf = x[ix];
    ix += inc_x;
  }
  return maxf;
}

// The peeled access reads a DIFFERENT pointer (y[0]) than the loop walks
// (x[ix]) -- x's own analysis finds no offset-zero access at all.
double neg_peel_wrong_pointer(int n, double *x, double *y, int inc_x) {
  double maxf;
  int ix, i;
  if (n <= 0 || inc_x <= 0) return 0.0;
  maxf = y[0];
  ix = inc_x;
  for (i = 1; i < n; i++) {
    if (x[ix] > maxf) maxf = x[ix];
    ix += inc_x;
  }
  return maxf;
}

// The address IV starts at inc_x, but steps by a DIFFERENT argument
// (other_inc) -- start and step must be the SAME proven stride value.
double neg_peel_wrong_stride_var(int n, double *x, int inc_x, int other_inc) {
  double maxf;
  int ix, i;
  if (n <= 0 || inc_x <= 0 || other_inc <= 0) return 0.0;
  maxf = x[0];
  ix = inc_x;
  for (i = 1; i < n; i++) {
    if (x[ix] > maxf) maxf = x[ix];
    ix += other_inc;
  }
  return maxf;
}

// An extra access at a dynamic, unresolved offset (n*inc_x) after the
// loop -- not accounted for by either the loop or the peeled access, so
// the whole pairing must be rejected (the "all-access correctness
// blocker" this feature is built on top of).
double neg_peel_extra_access(int n, double *x, int inc_x) {
  double maxf, extra;
  int ix, i;
  if (n <= 0 || inc_x <= 0) return 0.0;
  maxf = x[0];
  ix = inc_x;
  for (i = 1; i < n; i++) {
    if (x[ix] > maxf) maxf = x[ix];
    ix += inc_x;
  }
  extra = x[n * inc_x];
  return maxf + extra;
}

// The peeled access is CONDITIONAL (behind `if (flag)`) and does not
// dominate the loop -- the loop is still reached even when the access
// never executed, so it cannot be trusted to always have happened.
double neg_peel_conditional(int n, double *x, int inc_x, int flag) {
  double maxf = 0.0;
  int ix, i;
  if (n <= 0 || inc_x <= 0) return 0.0;
  if (flag) {
    maxf = x[0];
  }
  ix = inc_x;
  for (i = 1; i < n; i++) {
    if (x[ix] > maxf) maxf = x[ix];
    ix += inc_x;
  }
  return maxf;
}

// The stride itself is an unresolvable computed expression (a+b, not a
// bare argument or a compile-time constant) -- no stride evidence at all,
// so no peeled candidate is ever formed regardless of the rest of the
// shape.
double neg_peel_unresolvable_stride(int n, double *x, int a, int b) {
  double maxf;
  int ix, i, inc_x;
  if (n <= 0) return 0.0;
  inc_x = a + b;
  maxf = x[0];
  ix = inc_x;
  for (i = 1; i < n; i++) {
    if (x[ix] > maxf) maxf = x[ix];
    ix += inc_x;
  }
  return maxf;
}

// The pointer escapes to an unknown callee before the peeled access --
// an unresolved escape could do anything to the pointee, so no static
// access proof (peeled or ordinary) can be sound in its presence.
extern void mystery_callee(double *p);
double neg_peel_escapes(int n, double *x, int inc_x) {
  double maxf;
  int ix, i;
  if (n <= 0 || inc_x <= 0) return 0.0;
  mystery_callee(x);
  maxf = x[0];
  ix = inc_x;
  for (i = 1; i < n; i++) {
    if (x[ix] > maxf) maxf = x[ix];
    ix += inc_x;
  }
  return maxf;
}

// No dominating positivity guard at all for `n` -- dominatorProvesPositive
// finds nothing to corroborate the peeled access against, so it must be
// rejected even though the shape is otherwise identical to peeled_max.
double neg_peel_no_guard(int n, double *x, int inc_x) {
  double maxf;
  int ix, i;
  maxf = x[0];
  ix = inc_x;
  for (i = 1; i < n; i++) {
    if (x[ix] > maxf) maxf = x[ix];
    ix += inc_x;
  }
  return maxf;
}
