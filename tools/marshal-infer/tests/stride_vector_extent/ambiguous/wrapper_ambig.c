// Delegates to shared_worker, which is ambiguous once workerA.bc and
// workerB.bc are BOTH resident (see their own comments) -- must refuse to
// guess which body it resolves to, rather than silently picking whichever
// module happened to be indexed first.
void shared_worker(int n, double da, const double *x, int incx, double *y,
                    int incy);

void wrapper_ambig(int n, double da, const double *x, int incx, double *y,
                    int incy) {
  shared_worker(n, da, x, incx, y, incy);
}
