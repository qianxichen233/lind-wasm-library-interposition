// Config.h — versioned, checked-in configuration for marshal-infer
// (github.com/qianxichen233/lind-wasm-library-interposition issue #27):
// resource/analysis knobs, human-asserted per-symbol/per-argument
// "contracts" for cases static analysis cannot soundly prove on its own,
// and coverage thresholds a library profile can enforce. Loaded from an
// OPTIONAL `--config <file.json>` file; omitting it reproduces
// marshal-infer's built-in default behavior exactly (see loadConfig's own
// comment on backward compatibility).
//
// A "contract" entry is the only knob here that can affect what gets
// marshalled. It explicitly replaces an automatic extent proof with a
// human-reviewed assertion in the SAME size/direction vocabulary the
// analyzer produces. Its operands are validated against the target
// function's real signature before it is applied (see Infer.cpp's
// validateContractAgainstSignature), and its configured provenance is
// recorded in the output JSON (see ParamTree.h's TreeNode::confidence).
// There is no separate opt-in for an unproven automatic heuristic.
#pragma once

#include <cstdint>
#include <map>
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
