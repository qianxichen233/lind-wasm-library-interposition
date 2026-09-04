// CBLAS-style calling convention: length and stride are passed BY VALUE.
// Both extent operands must be emitted with source "value".
void vec_axpy_direct(int n, double da, const double *x, int incx, double *y,
                      int incy) {
  int ix = 0, iy = 0;
  for (int i = 0; i < n; i++) {
    y[iy] += da * x[ix];
    ix += incx;
    iy += incy;
  }
}
