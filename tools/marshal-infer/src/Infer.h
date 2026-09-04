// Infer.h — the inference step. Given a function's LLVM IR body and its
// DWARF-derived parameter forest, annotate the forest with marshalling info:
// pointer direction (IN/OUT/INOUT), size kind (CONST/FROM_ARG/FROM_ARG_POINTEE/
// CSTR/UNKNOWN), opaque-handle flags, and the return kind — plus a per-function
// warnings (residue) list. This is the library-side, best-effort analog of
// KSplit's read/write + NesCheck + size analyses (see research/arg-marshalling/).
#pragma once

#include "ParamTree.h"

#include "llvm/ADT/StringMap.h"

namespace llvm {
class Function;
}

namespace marshal {

// name -> defining Function*, across EVERY resident module in this run (not
// just the one `f` below came from). Lets inference follow a direct call to a
// callee whose caller only holds an external declaration for it (compiled as
// a separate translation unit) -- e.g. a library's public wrapper delegates
// its actual array walk to an internal kernel compiled in a different TU; the
// wrapper's own IR has no loop to analyze, but the kernel's does. See
// Infer.cpp's one-hop delegation analysis.
//
// A callee DEFINED in the same module as its caller never needs this index
// at all -- the call site already references that exact body directly (see
// detectDelegatedArrayBound). This index exists only to resolve a genuinely
// cross-module reference, so it must be built to match that: only
// EXTERNALLY-LINKED definitions are indexed (an internal/static-linkage
// function is never visible outside its own TU, so indexing it by name would
// let an unrelated same-named static function in some other TU silently
// resolve a completely different external declaration). A name with more
// than one externally-linked definition across resident modules (e.g.
// several architecture-specific TUs all defining the same public kernel
// symbol, only one of which would actually be linked into a real binary) is
// GENUINELY AMBIGUOUS -- which one a given declaration resolves to isn't
// knowable from IR alone -- and is stored as nullptr, a sentinel meaning
// "known name, refuse to guess" rather than silently picking whichever
// definition happened to be inserted first.
using CalleeIndex = llvm::StringMap<const llvm::Function *>;

// Annotate `ft` in place using the IR of `f`. `ft` must already hold the DWARF
// parameter trees (from buildFunctionTrees); this fills dir/sizeKind/handle/ret.
// `calleeIndex` must cover every module resident in this run (built before any
// inferFunction call, since a callee can be analyzed regardless of which
// module the CALLER happens to be processed from).
void inferFunction(const llvm::Function &f, FunctionTrees &ft,
                   const CalleeIndex &calleeIndex);

} // namespace marshal
