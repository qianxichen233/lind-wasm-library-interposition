// Constant-sourced StrideVector extent operands: a proven per-iteration
// step with no caller argument behind it at all (ExtentSource::Constant),
// distinct from the argument-derived "value"/"pointee_i32" sources every
// other fixture in this directory exercises. See CONFIG.md's "Confidence
// model" for how this composes with the rest of the
// StrideVector proof; this file only covers the constant-stride path
// itself, deliberately kept separate from the peeled-first-iteration
// family's zero-start problem -- constant-stride support
// alone must never make a nonzero-start loop marshalable (see
// skip_first_indexed/skip_first_pointerwalk below).
//
// clang lowers an identical source-level fixed-stride walk two different
// ways depending on whether the source uses array-index syntax or a raw
// incrementing pointer (see the ASE'16 array-length-inference paper's
// Listing 3 vs. Listing 4, and research/arg-marshalling/papers/). Both
// forms are covered here, and produce the SAME extent operand shape --
// confirmed by direct IR/ScalarEvolution inspection (`opt
// -passes='print<scalar-evolution>'`), not merely by comparing JSON
// output: the two shapes reach loopBoundValues' address-recurrence
// detection through genuinely different SCEV paths (an affine INDEX over
// a loop-invariant pointer operand, vs. an affine POINTER operand with a
// fixed index), which is why loopBoundValues checks both.

// `x[i]`: the classic indexed form. Stride is the constant 1.
double sum_indexed(int n, double *x) {
  double s = 0.0;
  for (int i = 0; i < n; i++)
    s += x[i];
  return s;
}

// `*p; p++`, with the trip count driven by a SEPARATE integer counter
// (not by comparing `p` itself against an end pointer). This is the
// pointer-walk form whose address recurrence loopBoundValues can actually
// prove a length for: the loop's own exact trip count comes from the
// ordinary `i<n` backedge-taken-count proof, entirely independent of how
// `p` itself behaves, so nothing needs to invert a pointer comparison into
// an argument.
double sum_pointerwalk(int n, double *x) {
  double s = 0.0;
  double *p = x;
  for (int i = 0; i < n; i++) {
    s += *p;
    p++;
  }
  return s;
}

// The OTHER pointer-walk idiom -- terminating the loop by comparing the
// walking pointer itself against a precomputed end pointer (`x + n`), no
// separate counter at all. This is the literal shape the ASE'16 paper's
// own Listing 4 uses. Address-recurrence detection (this file's actual
// subject) succeeds here exactly as it does for sum_pointerwalk above --
// but the LENGTH side does not: ScalarEvolution's backedge-taken-count for
// a pointer-comparison-terminated loop involves a ptrtoint/umax/udiv
// expression that unwrapArgumentSCEV correctly refuses to reduce to a bare
// argument (the same "don't guess, reject" discipline it applies to a Mul
// or an unresolved Add), so this force_locals on the LENGTH side, not the
// stride side. A separate, pre-existing limitation in trip-count inference
// for pointer-terminated loops -- not something this change introduces,
// widens, or is in scope to close (see run.sh's own comment on this case).
double sum_pointerwalk_cmp(int n, double *x) {
  double s = 0.0;
  double *end = x + n;
  while (x < end) {
    s += *x;
    x++;
  }
  return s;
}

// A fixed, non-unit compile-time stride (every other element) -- proves
// the constant-source mechanism isn't hardcoded to 1. `2*i`'s GEP index
// recurrence has step 2, a bare SCEVConstant, same proof as sum_indexed's
// step of 1.
double sum_every_other(int n, double *x) {
  double s = 0.0;
  for (int i = 0; i < n; i++)
    s += x[2 * i];
  return s;
}

// Classic Fortran BLAS calling convention for the LENGTH only (no stride
// argument exists at all -- this function has none): `n` is passed by
// reference, unpacked at entry, same as fortran_style.c's own convention.
// Proves the constant-stride path composes correctly with the EXISTING
// pointee_i32 length-source detection, rather than only ever pairing with
// a directly-passed-by-value length.
double sum_fortran_len(int *N, double *X) {
  int n = *N;
  double s = 0.0;
  for (int i = 0; i < n; i++)
    s += X[i];
  return s;
}

// Nonzero-start, indexed form: the SAME peeled-first-element shape as
// the max/min family (OpenBLAS's isamax_k and 27 siblings), with
// an ordinary constant unit stride standing in for their argument-derived
// inc_x -- and, unlike this file's OTHER fixtures, this one HAS a
// dominating x[0] access with the address IV starting exactly at the
// stride, the shape peeled_prefix.c's detectPeeledPrefixBound recognizes.
// It still correctly force_locals: there is no `if (n<=0) return;` guard
// at all, so dominatorProvesPositive can't corroborate the peeled access
// (see peeled_prefix.c's neg_peel_no_guard, which pins this exact
// requirement with the real inc_x-argument shape).
double sum_skip_first_indexed(int n, double *x) {
  double s = x[0];
  int ix = 1;
  for (int i = 1; i < n; i++) {
    s += x[ix];
    ix += 1;
  }
  return s;
}

// The same shape, pointer-walk form: `p` starts at `x + 1`, a SCEVAddExpr,
// not the bare SCEVUnknown loopBoundValues' pointer-operand case (`*p;
// p++`) requires for even an ORDINARY zero-start walk. Peeled-prefix
// recognition is implemented for the index-operand case only (see
// loopBoundValues' own comment) -- the pointer-operand case has no
// stride-start detection at all yet, so this stays rejected regardless of
// the missing guard too; a documented limitation, not a proof failure.
double sum_skip_first_pointerwalk(int n, double *x) {
  double s = x[0];
  double *p = x + 1;
  for (int i = 1; i < n; i++) {
    s += *p;
    p++;
  }
  return s;
}
