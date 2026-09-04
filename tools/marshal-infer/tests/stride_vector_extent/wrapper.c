// A public wrapper whose own body never walks x/y at all -- it immediately
// delegates to worker_axpy, defined in worker.c (a separate translation
// unit). Only declared here; marshal-infer must follow the one-hop
// delegation into worker.bc's actual body when both are fed to it together.
void worker_axpy(int n, double da, const double *x, int incx, double *y,
                  int incy);

void wrapper_axpy(int n, double da, const double *x, int incx, double *y,
                   int incy) {
  worker_axpy(n, da, x, incx, y, incy);
}
