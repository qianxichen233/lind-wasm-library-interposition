// Classic Fortran BLAS calling convention: every scalar is passed BY
// REFERENCE. Both extent operands must be emitted with source "pointee_i32",
// pointing at N's and INCX's own argument indices -- NOT treated as if the
// raw argument slot held the value directly (that would read a pointer's
// bit pattern as if it were the count).
void vec_axpy_fortran(int *N, double *DA, const double *X, int *INCX,
                       double *Y, int *INCY) {
  int n = *N, incx = *INCX, incy = *INCY;
  double da = *DA;
  int ix = 0, iy = 0;
  for (int i = 0; i < n; i++) {
    Y[iy] += da * X[ix];
    ix += incx;
    iy += incy;
  }
}
