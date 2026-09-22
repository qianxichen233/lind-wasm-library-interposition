#include <string.h>

// Regression coverage for the "all-access correctness blocker"
// (local-notes/active/plan-openblas-max-family-inference.md, Section 1): a
// proof that covers only ONE access pattern through a pointer is not
// automatically sound for the WHOLE pointer. detectDirectArrayBound must
// verify every access recorded in Access::staticAccesses is explained by
// whichever loop bound it's about to accept, not just return as soon as
// ANY loop resolves cleanly. See noOtherAccesses in Infer.cpp.
//
// This also covers three related soundness gaps: a bulk-memory operation
// (memcpy) reading more of a pointer than a loop's own extent proof
// covers; one-hop DELEGATION, where the wrapper itself -- not just the
// delegated callee -- must have every one of its own accesses accounted
// for before the callee's proof can be trusted for the whole pointer; and
// a recognized-but-UNBOUNDED C-string library call (strlen, strcpy, ...)
// on the same pointer, which classifyLibArg deliberately leaves with no
// length operand at all (it scans until a NUL byte, wherever that falls)
// -- see noOtherAccesses' acc.lengths/acc.stringOp checks and
// detectDelegatedArrayBound's wrapperBaseClean gate in Infer.cpp.
//
// Every strlen call below uses its return value: an unused call to a
// `pure`-attributed libc function like strlen is dead-code-eliminated
// before marshal-infer ever sees the IR (confirmed by inspecting the
// compiled bitcode), which would silently turn these into non-tests.

// Baseline: a clean n-element loop, nothing else touches x -- must still
// marshal. Proves the coverage check doesn't over-reject the ordinary case.
double clean_loop(int n, double *x) {
  double s = 0.0;
  for (int i = 0; i < n; i++)
    s += x[i];
  return s;
}

// The plan's own motivating regression: an otherwise-identical loop, plus
// a separate access at a DYNAMIC, unresolved offset (x[n], itself the
// argument) that the n-element envelope does not cover. Must NOT be
// accepted with an n-element extent -- the real function touches one more
// element than that would copy.
double loop_plus_unresolved_extra(int n, double *x) {
  double s = 0.0;
  for (int i = 0; i < n; i++)
    s += x[i];
  s += x[n];
  return s;
}

// The same hazard with a RESOLVED (compile-time-constant) extra offset
// instead of a dynamic one -- still must reject: this tool has no
// symbolic range prover to check "is 3 within [0, n-1]" for a runtime n,
// so a constant offset it cannot explain is treated the same as an
// unresolved one (see noOtherAccesses' own comment on why).
double loop_plus_resolved_extra(int n, double *x) {
  double s = 0.0;
  for (int i = 0; i < n; i++)
    s += x[i];
  s += x[3];
  return s;
}

// An unresolved pointer escape (to an unknown callee) before an otherwise
// clean loop -- the callee could do anything to the pointee, so the loop's
// own proof cannot be trusted as the pointer's WHOLE story either.
extern void mystery_callee(double *p);
double loop_with_escape(int n, double *x) {
  double s = 0.0;
  mystery_callee(x);
  for (int i = 0; i < n; i++)
    s += x[i];
  return s;
}

// A bulk-memory operation (memcpy) alongside an otherwise-clean loop,
// reading MORE than the loop's own n-element envelope covers. A memcpy's
// length is a BYTE count with no proven relationship to the loop's
// ELEMENT-based (length, stride) pair -- Access::lengths must block
// acceptance here exactly the way an unexplained StaticAccess does.
double loop_plus_memcpy_extra(int n, double *x, double *dst) {
  for (int i = 0; i < n; i++)
    x[i] += 1.0;
  __builtin_memcpy(dst, x, (n + 1) * sizeof(double));
  return x[0];
}

// One-hop delegation: the WRAPPER itself reads x[n] (an access the
// delegated callee's own proof knows nothing about) AFTER an otherwise-
// clean delegated call. detectDelegatedArrayBound must check the
// WRAPPER's own Access, not just the callee's.
__attribute__((noinline))
static void worker_for_wrapper(int n, double *x) {
  for (int i = 0; i < n; i++)
    x[i] += 1.0;
}
double wrapper_extra_access(int n, double *x) {
  worker_for_wrapper(n, x);
  return x[n];
}

// The same delegation hazard, via a bulk-memory operation in the WRAPPER
// instead of a direct GEP.
__attribute__((noinline))
static void worker_for_wrapper2(int n, double *x) {
  for (int i = 0; i < n; i++)
    x[i] += 1.0;
}
double wrapper_with_memcpy(int n, double *x, double *dst) {
  worker_for_wrapper2(n, x);
  __builtin_memcpy(dst, x, (n + 1) * sizeof(double));
  return dst[0];
}

// A SECOND, unrelated delegate call on the same pointer -- even though the
// FIRST call's own extent proof is clean, the second call's effect on `x`
// is unaccounted for, so accepting the first alone is not sound.
__attribute__((noinline))
static void worker_a(int n, double *x) {
  for (int i = 0; i < n; i++)
    x[i] += 1.0;
}
__attribute__((noinline))
static void worker_b(double *x) {
  x[0] = 0.0;
}
double wrapper_two_calls(int n, double *x) {
  worker_a(n, x);
  worker_b(x);
  return x[0];
}

// Positive contrast: a wrapper delegating to exactly one clean worker,
// with NOTHING else in the wrapper touching the pointer -- must still
// marshal. Proves the wrapperClean gate doesn't over-reject the ordinary
// delegation case (matches wrapper.c/worker.c's own convention: the
// wrapper's own body never walks x at all).
__attribute__((noinline))
static void worker_clean(int n, double *x) {
  for (int i = 0; i < n; i++)
    x[i] += 1.0;
}
void wrapper_clean(int n, double *x) {
  worker_clean(n, x);
}

// A clean n-element loop, plus an UNBOUNDED C-string scan (strlen) on the
// SAME pointer -- classifyLibArg gives strlen no length operand at all (it
// reads until a NUL byte, wherever that falls, possibly far past the
// loop's own n-element envelope). Must NOT be accepted with an n-element
// extent.
double loop_plus_strlen(int n, double *x) {
  double s = 0.0;
  for (int i = 0; i < n; i++)
    s += x[i];
  s += (double)strlen((char *)x);
  return s;
}

// The same hazard, one hop away: the WRAPPER itself calls strlen on the
// pointer after an otherwise-clean delegated call -- detectDelegatedArrayBound
// must check the WRAPPER's own Access for an unbounded string scan, not
// just the callee's.
__attribute__((noinline))
static void worker_for_strlen(int n, double *x) {
  for (int i = 0; i < n; i++)
    x[i] += 1.0;
}
double wrapper_plus_strlen(int n, double *x) {
  worker_for_strlen(n, x);
  return (double)strlen((char *)x);
}
