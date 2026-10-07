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
#include <optional>
#include <string>
#include <vector>

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

// One declared parameter of a reviewed callback signature, in the SAME
// marshalling vocabulary an ordinary function parameter uses -- but
// describing the CALLBACK's own parameter, never the host function's.
// kind=="ptr" MUST carry a complete dir+size_kind classification; there
// is no partial/placeholder pointer entry here. An incomplete pointer
// classification is rejected outright, not downgraded to a residue,
// because nothing analyzes a callback's own body to recover what a
// human left out.
struct CallbackParamContract {
  std::string kind;      // "scalar" | "ptr"
  std::string dir;       // "in" | "out" | "inout" -- required for kind=="ptr"
  std::string sizeKind;  // "const" | "from_arg" | "cstr" | "stride_vector"
                         // -- required for kind=="ptr"
  uint64_t constSize = 0;              // sizeKind=="const": byte count
  int sizeArgIndex = -1;               // sizeKind=="from_arg": index into
                                       // THIS callback's OWN param list,
                                       // never the host function's
  ContractExtentOperand sizeOperand;   // sizeKind=="stride_vector"
  ContractExtentOperand strideOperand; // sizeKind=="stride_vector"
  uint64_t strideElemSize = 0;         // sizeKind=="stride_vector": per-element bytes
  // "int32" | "int64" | "float32" | "float64" -- required for kind=="scalar",
  // forbidden for kind=="ptr" (a pointer always lowers to a flat i32
  // address on this target, with no such ambiguity). The runtime
  // descriptor needs the EXACT raw wasm value-type slot a scalar
  // parameter occupies, not merely "it's a scalar" -- validated against
  // the callback's real DWARF parameter type (Infer.cpp's
  // classifyScalarAbiType) before being trusted, the same way every
  // other contract field here is.
  std::string scalarType;
};

// A callback's own return value. "function_pointer" (a callback that
// itself returns a callback) is represented here so the schema does not
// need to change shape once that case is supported, but nothing consumes
// it yet.
struct CallbackReturnContract {
  std::string kind;        // "void" | "scalar" | "function_pointer"
  std::string scalarType;  // "int32" | "int64" | "float32" | "float64" --
                           // required for kind=="scalar"
};

// One versioned, named callback signature. Defined ONCE in a config's
// top-level "callback_signatures" registry (Config::callbackSignatures)
// and referenced BY ID from a per-argument contract (CallbackRef below),
// so "unknown callback signature id" is a real, checkable condition and
// a signature shared across many call sites (e.g. every OpenBLAS
// function taking an xerbla-shaped error handler) is described exactly
// once, not repeated per call site.
struct CallbackSignature {
  std::vector<CallbackParamContract> params;
  CallbackReturnContract ret;
  std::string lifetime;       // "during_call" | "retained"
  bool nullable = false;
  // Closed vocabulary: "same_thread_only" is the only value this build
  // accepts, matching the runtime's own ReentryPolicy enum
  // (threei::lib_handler_table_v2) -- a free-form string here could let
  // this tool emit "decision":"marshal" for a contract the runtime
  // consumer rejects outright at registration time.
  std::string reentryPolicy;
};

// A per-argument reference to a registered callback signature -- the
// callback-world analog of StrideVectorContract for an ordinary pointer
// argument. The referenced id is resolved and ABI-validated against the
// real function-pointer argument's own DWARF signature by Infer.cpp's
// validateCallbackContractAgainstSignature, never trusted unchecked.
struct CallbackRef {
  std::string signatureId;
};

// One argument's contract entry is EITHER a StrideVector size assertion
// for an ordinary pointer, OR a callback-signature reference for a
// function-pointer argument -- never both; an argument is structurally
// one or the other. Exactly one of these is populated, enforced by
// Config.cpp's parser (which shape it parses is decided by which
// discriminating keys are present), not by this struct itself.
struct FunctionContractEntry {
  std::optional<StrideVectorContract> strideVector;
  std::optional<CallbackRef> callback;
};

// Keyed by argIndex: a DWARF/source-parameter position, the same you'd get
// counting the C function's own declared parameters left-to-right -- NOT
// necessarily the final JSON output's "args" array position, which shifts
// when the function has a hidden sret argument or a multi-slot (fp128)
// parameter ahead of it (see ParamTree.h's TreeNode::abiSlots /
// FunctionTrees::retSretArg for why).
using FunctionContract = std::map<int, FunctionContractEntry>;

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
  // Named callback signatures, defined once and referenced by id from a
  // CallbackRef in `contracts` -- see CallbackSignature's own comment.
  std::map<std::string, CallbackSignature> callbackSignatures;
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
