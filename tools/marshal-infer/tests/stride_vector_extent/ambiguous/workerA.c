// One of two conflicting externally-linked definitions of the same name
// (see workerB.c) -- e.g. what several architecture-specific TUs compiled
// into the same analysis run would look like, only one of which would
// actually be linked into a real binary.
void shared_worker(int n, double da, const double *x, int incx, double *y,
                    int incy) {
  int ix = 0, iy = 0;
  for (int i = 0; i < n; i++) {
    y[iy] += da * x[ix];
    ix += incx;
    iy += incy;
  }
}
