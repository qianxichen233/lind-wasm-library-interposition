// A wrapper delegating to a callee that is declared but never DEFINED
// anywhere in this test's compiled set -- one-hop delegation has nothing to
// follow into. x/y must not silently default to a single element; the
// pointer escapes to an unresolvable callee with no length evidence found,
// so the whole function must fail closed.
void undefined_worker(int n, double da, const double *x, int incx, double *y,
                       int incy);

void wrapper_missing(int n, double da, const double *x, int incx, double *y,
                      int incy) {
  undefined_worker(n, da, x, incx, y, incy);
}
