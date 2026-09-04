// The conflicting definition -- see workerA.c. A DIFFERENT body under the
// SAME externally-linked name; which one a cross-module reference to
// shared_worker actually resolves to isn't knowable from IR alone, so
// calleeIndex must record this name as ambiguous rather than picking one.
void shared_worker(int n, double da, const double *x, int incx, double *y,
                    int incy) {
  (void)n; (void)da; (void)x; (void)incx; (void)y; (void)incy;
}
