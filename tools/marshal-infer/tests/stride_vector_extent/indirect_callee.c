// An indirect call through an opaque function pointer: marshal-infer cannot
// statically know which function this targets (CallBase::getCalledFunction
// returns null for it), so one-hop delegation has nothing to follow. x/y
// must fail closed, not silently default to a single element.
typedef void (*axpy_fn)(int, double, const double *, int, double *);

void wrapper_indirect(void *fp, int n, double da, const double *x, int incx,
                       double *y) {
  ((axpy_fn)fp)(n, da, x, incx, y);
}
