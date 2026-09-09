// Config.h — versioned, checked-in configuration for marshal-infer
// (github.com/qianxichen233/lind-wasm-library-interposition issue #27):
// resource/analysis knobs, human-asserted per-symbol/per-argument
// "contracts" for cases static analysis cannot soundly prove on its own,
// and coverage thresholds a library profile can enforce. Loaded from an
// OPTIONAL `--config <file.json>` file; omitting it reproduces
// marshal-infer's built-in default behavior exactly (see loadConfig's own
// comment on backward compatibility).
//
// Two of this file's knobs CAN affect runtime memory-safety exposure, and
// both require deliberate, visible opt-in rather than a silent default:
//   - A "contract" entry does not bypass soundness -- it supplies the SAME
//     size/direction vocabulary the analyzer itself would have produced had
//     it been able to prove the value, asserted by a human who verified it.
//   - "analysis.policy":"relaxed" + a named entry in "analysis.heuristics"
//     DOES accept a genuinely unproven pairing (see Infer.cpp's
//     loopBoundValues/detectDirectArrayBound) -- this carries the SAME
//     runtime exposure a proven decision would if the heuristic is wrong,
//     since the runtime has no reduced-trust code path for a lower-
//     confidence spec. It requires BOTH the policy switch AND the specific
//     heuristic to be named, and is rejected as vacuous if either is set
//     without the other.
// Every other soundness-critical check (the escape-based fail-closed gate,
// the address-induction-variable zero-start proof, ambiguous-callee
// rejection) is compiled-in and unconditional -- nothing in this file can
// weaken those. Every decision's provenance (proven / configured /
// heuristic) is recorded in the output JSON (see ParamTree.h's
// TreeNode::confidence) so a reader can always tell which functions were
// analyzed, asserted, or guessed.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace marshal {

// One end of a human-asserted StrideVector pairing -- see ExtentOperand in
// ParamTree.h, which this mirrors. Kept as a separate, ParamTree-independent
// type so Config.h has no dependency on the LLVM/DWARF-facing parts of this
// tool; Infer.cpp converts between the two at the point of application.
struct ContractExtentOperand {
  int argIndex = -1;
  std::string source; // "value" | "pointee_i32" -- validated at load time
};

// A human-asserted override for exactly one argument of one exported
// symbol, in the SAME sizeKind vocabulary the analyzer itself produces
// (currently only StrideVector -- the demonstrated need, from issue #26's
// OpenBLAS regression: a compiled loop's shape can be too transformed for
// static analysis to PROVE the canonical relationship
// LIND_SIZE_STRIDE_VECTOR needs, even when a human reading the source can
// see it clearly). Generalizing to other sizeKinds is a straightforward
// extension of this same mechanism, not implemented here because nothing
// has yet demonstrated the need for it.
struct StrideVectorContract {
  ContractExtentOperand sizeOperand;
  ContractExtentOperand strideOperand;
  uint64_t constSize = 0;
  std::string dir; // "in" | "out" | "inout", or empty = let the analyzer's
                    // own read/write observation decide (unaffected by this
                    // contract).
};

// Keyed by argIndex: a DWARF/source-parameter position, the same you'd get
// counting the C function's own declared parameters left-to-right -- NOT
// necessarily the final JSON output's "args" array position, which shifts
// when the function has a hidden sret argument or a multi-slot (fp128)
// parameter ahead of it (see ParamTree.h's TreeNode::abiSlots /
// FunctionTrees::retSretArg for why).
using FunctionContract = std::map<int, StrideVectorContract>;

// Fails the whole marshal-infer run (nonzero exit, no JSON written) when
// this library's MARSHAL rate drops below an expected floor -- the concrete
// gap issue #26 exposed: a soundness fix elsewhere in the tool silently
// swung OpenBLAS's marshal count from 86 down to 18 with nothing to catch
// the regression except a human noticing the number looked different.
struct CoveragePolicy {
  bool enabled = false;
  int minMarshalCount = 0;    // absolute floor: #records with decision=="marshal"
  double minMarshalPct = 0.0; // 0..100: marshal-decision records as a
                              // percentage of exported symbols (--exports)
                              // when given, else of all covered records
};

// Strict (the built-in default) accepts only Proven StrideVector decisions.
// Relaxed additionally accepts whichever NAMED heuristics are listed in
// Config::heuristics -- see KnownHeuristics below for the closed set of
// names loadConfig will accept, and Confidence::Heuristic in ParamTree.h
// for exactly what accepting one means for runtime memory-safety exposure.
enum class InferencePolicy { Strict, Relaxed };

// The closed set of heuristic names loadConfig accepts in
// "analysis.heuristics" (only meaningful when policy=="relaxed"). Adding a
// new heuristic means implementing its actual mechanism in Infer.cpp AND
// adding its name here -- listing an unimplemented name here would let a
// profile silently enable nothing while believing it enabled something.
//
// "guard_based_length": pairs a stride with a length that came from
// dominatingArgumentGuard's dominator-tree walk rather than an exact
// ScalarEvolution trip-count proof (see LoopBound::lengthProven and
// loopBoundValues in Infer.cpp) -- the ONE mechanism issue #26's fix
// removed, reinstated here as an explicit, attributable, opt-in choice
// instead of a silent default.
//
// "unroll_scaled_stride": accepts a per-iteration step recurrence of the
// form `K*incx` (K a small power of two) as meaning plain `incx` -- the
// shape LLVM's loop-unroll transform produces for an induction variable's
// step (see LoopBound::strideProven and unwrapArgumentSCEVWithUnrollGuess
// in Infer.cpp). In practice needed TOGETHER with "guard_based_length" to
// recover a real -O2-optimized loop: unrolling transforms BOTH the trip-
// count computation (defeating the exact SCEV proof, hence
// guard_based_length) and the address induction variable's own step
// (defeating the exact stride proof) at once. A genuine source-level
// "stride is 2*incx" relationship is indistinguishable from this pattern
// and would be silently misread as plain incx.
//
// A third fallback deriving length directly from a loop's own latch
// comparison (rather than a separate dominating guard) was prototyped and
// removed: even with an independent ScalarEvolution proof gating it, it
// recovered zero additional OpenBLAS functions beyond what
// guard_based_length already finds (LLVM's own unroll transform already
// inserts the guard branches that heuristic looks for first), so it was
// pure added attack surface with no measured benefit. Do not re-add a
// pattern-matching length fallback without first demonstrating measurable
// coverage it uniquely provides.
inline const std::set<std::string> &knownHeuristics() {
  static const std::set<std::string> k = {"guard_based_length",
                                          "unroll_scaled_stride"};
  return k;
}

struct Config {
  int configVersion = 0;
  std::string profileName;   // informational, echoed into output JSON
  std::string sourcePath;    // the file this was loaded from, for provenance
  unsigned maxTypeDepth = 6; // matches buildFunctionTrees's built-in default
  // 0 disables one-hop interprocedural delegation entirely; 1 (the built-in
  // default) is the only other currently-implemented value -- multi-hop
  // delegation does not exist yet, so loadConfig REJECTS anything else
  // rather than silently clamping it to 1 (a request this loader can't
  // fulfill must fail loudly, not be quietly downgraded).
  unsigned maxDelegationHops = 1;
  InferencePolicy policy = InferencePolicy::Strict;
  std::set<std::string> heuristics; // only consulted when policy==Relaxed
  std::map<std::string, FunctionContract> contracts; // exported symbol -> contract
  CoveragePolicy coverage;
};

// Parses and STRICTLY validates a config JSON file (schema version 1 -- see
// CONFIG.md): unknown top-level or nested keys, wrong-typed values, and
// out-of-range or unimplemented-feature values are all rejected with a
// specific, actionable message in `err` (deliberately stricter than
// Annotations.h's loader, which is a looser, best-effort merge -- this file
// is meant to be checked in and trusted, so a typo or an unimplemented
// request must fail loudly rather than be silently ignored or downgraded).
// Returns false and leaves `out` unspecified on any failure.
bool loadConfig(const std::string &path, Config &out, std::string &err);

} // namespace marshal
