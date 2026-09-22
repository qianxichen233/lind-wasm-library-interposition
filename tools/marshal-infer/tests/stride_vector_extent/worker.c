// The one-hop delegation target for wrapper.c, compiled as a SEPARATE
// translation unit (a real body, not just a declaration) -- worker.bc must
// be resident alongside wrapper.bc for wrapper_axpy's delegated array-bound
// detection to find it.
void worker_axpy(int n, double da, const double *x, int incx, double *y,
                  int incy) {
  int ix = 0, iy = 0;
  for (int i = 0; i < n; i++) {
    y[iy] += da * x[ix];
    ix += incx;
    iy += incy;
  }
}
