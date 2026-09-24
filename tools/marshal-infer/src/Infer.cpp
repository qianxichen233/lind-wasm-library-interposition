// Infer.cpp — see Infer.h. Library-side, best-effort marshalling inference over
// LLVM IR. We keep it deliberately heuristic ("infer the common case, flag the
// rest"): every uncertain decision becomes a warning rather than a silent guess.
#include "Infer.h"
#include "Annotations.h"

#include "llvm/IR/Argument.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/BinaryFormat/Dwarf.h"

#include <algorithm>
#include <optional>
#include <string>

using namespace llvm;

namespace marshal {
namespace {

// ---- small helpers ---------------------------------------------------------

// Strip pointer-preserving casts (bitcast/addrspacecast) to find the underlying
// pointer value.
const Value *stripPtr(const Value *v) { return v->stripPointerCasts(); }

// Strip integer width casts (zext/sext/trunc) to find the underlying integer.
const Value *stripIntCasts(const Value *v) {
  while (auto *ci = dyn_cast<CastInst>(v)) {
    if (!ci->getType()->isIntegerTy())
      break;
    v = ci->getOperand(0);
  }
  return v;
}

// Map an IR Argument back to its source (DWARF) parameter index, accounting for
// a leading sret hidden pointer.
int dwarfIndexOf(const Value *v, unsigned sretOffset) {
  if (auto *a = dyn_cast<Argument>(v)) {
    int idx = (int)a->getArgNo() - (int)sretOffset;
    return idx >= 0 ? idx : -1;
  }
  return -1;
}

// Same, but also resolves ONE level through a pointer load: `v` may not be
// the argument itself but `load T, ptr %P` where %P is. Needed for the
// Fortran-by-reference calling convention every classic BLAS entry point
// uses (`blasint *N` unpacked as `BLASLONG n = *N;` at function entry,
// versus CBLAS's `blasint n` taken directly) -- without this, a value
// derived from a Fortran-style scalar-by-reference argument never resolves
// back to that argument at all, only its CBLAS sibling (which passes the
// same logical value directly) would. Callers apply stripIntCasts()
// themselves before calling this, matching dwarfIndexOf's own contract.
int dwarfIndexOfMaybeLoaded(const Value *v, unsigned sretOffset) {
  if (int idx = dwarfIndexOf(v, sretOffset); idx >= 0)
    return idx;
  if (auto *ld = dyn_cast<LoadInst>(v))
    return dwarfIndexOf(stripPtr(ld->getPointerOperand()), sretOffset);
  return -1;
}

// Find the pointer an integer value derived from (via ptrtoint + arithmetic).
// Any arithmetic en route means the eventual pointer is reached with an offset.
const Value *underlyingPtrOfInt(const Value *iv, bool &hadOffset,
                                SmallPtrSetImpl<const Value *> &seen) {
  if (!iv || !seen.insert(iv).second) return nullptr;
  iv = stripIntCasts(iv);
  if (auto *bo = dyn_cast<BinaryOperator>(iv)) {
    hadOffset = true; // add/sub/or on a pointer-derived integer = an offset
    if (const Value *p = underlyingPtrOfInt(bo->getOperand(0), hadOffset, seen))
      return p;
    return underlyingPtrOfInt(bo->getOperand(1), hadOffset, seen);
  }
  if (auto *p2i = dyn_cast<PtrToIntInst>(iv))
    return p2i->getPointerOperand();
  return nullptr;
}

// Trace a returned pointer back to the source argument it derives from —
// following casts, GEPs, phi/select, and int<->ptr arithmetic. Sets `hadOffset`
// when the return is arg+offset (a pointer INTO the arg) vs the arg itself.
int traceReturnPtr(const Value *v, unsigned sretOffset, bool &hadOffset,
                   SmallPtrSetImpl<const Value *> &seen) {
  if (!v || !seen.insert(v).second) return -1;
  v = v->stripPointerCasts();
  if (int idx = dwarfIndexOf(v, sretOffset); idx >= 0) return idx;
  if (auto *gep = dyn_cast<GetElementPtrInst>(v)) {
    if (!gep->hasAllZeroIndices()) hadOffset = true;
    return traceReturnPtr(gep->getPointerOperand(), sretOffset, hadOffset, seen);
  }
  if (auto *i2p = dyn_cast<IntToPtrInst>(v)) {
    SmallPtrSet<const Value *, 16> iseen;
    if (const Value *p = underlyingPtrOfInt(i2p->getOperand(0), hadOffset, iseen))
      return traceReturnPtr(p, sretOffset, hadOffset, seen);
  }
  if (auto *phi = dyn_cast<PHINode>(v))
    for (const Value *iv : phi->incoming_values())
      if (int idx = traceReturnPtr(iv, sretOffset, hadOffset, seen); idx >= 0)
        return idx;
  if (auto *sel = dyn_cast<SelectInst>(v)) {
    if (int idx = traceReturnPtr(sel->getTrueValue(), sretOffset, hadOffset, seen);
        idx >= 0)
      return idx;
    return traceReturnPtr(sel->getFalseValue(), sretOffset, hadOffset, seen);
  }
  return -1;
}

// --- known opaque, library-owned types: handle, never deep-copied (C2/C4) ---
// Handle/allocator/searcher lookups now consult the (data-driven) annotation
// set — built-in defaults extended by any --annotations file. See Annotations.h.
std::string canonicalHandleClass(const std::string &typeName) {
  auto &ht = annotations().handleTypes;
  if (auto it = ht.find(typeName); it != ht.end()) return it->second;
  return typeName.empty() ? std::string("void") : typeName;
}
// A struct/union pointee is an opaque handle if either (a) it's in the
// curated handle-types table (FILE*/DIR*-style, name-driven), or (b) it's an
// INCOMPLETE type (sizeBytes==0 — forward-declared, no visible definition
// anywhere in the compilation unit, e.g. zlib's `struct internal_state;`
// behind z_stream's `state` field). (b) needs no annotation at all: there is
// provably no way to deep-copy a type whose layout doesn't exist in the TU,
// so treating it as opaque isn't a heuristic — it's the only sound option, a
// stronger signal than the curated table. Strictly gated on isComposite():
// a zero size for a NON-composite pointee (e.g. a function-pointer pointee,
// kind=Unknown, typeName="<func>") means something different — "we don't
// know this type at all" — and must NOT be swept into this handle path; that
// would silently blit a raw function pointer across cages, unsound unless
// the value is proven NULL at the call site (a still-unimplemented,
// separate, runtime-guarded capability).
bool isKnownOpaqueStruct(const TreeNode *pointee) {
  if (!pointee) return false;
  if (annotations().handleTypes.count(pointee->typeName)) return true;
  return pointee->isComposite() && pointee->sizeBytes == 0;
}

struct AllocInfo {
  bool isAlloc = false;      // plain allocation -> ptr_alloc
  bool forceLocal = false;   // realloc-style -> whole function local
  SmallVector<int, 2> sizeArgs;
};
AllocInfo allocatorInfo(StringRef raw) {
  StringRef n = raw;
  if (!n.consume_front("__libc_")) n.consume_front("__");
  AllocInfo a;
  auto &al = annotations().allocators;
  if (auto it = al.find(n.str()); it != al.end()) {
    if (it->second.forceLocal) a.forceLocal = true;
    else { a.isAlloc = true;
           for (int s : it->second.sizeArgs) a.sizeArgs.push_back(s); }
  }
  return a;
}

// Classic search functions whose result is a pointer INTO arg0, computed in a
// sibling TU (e.g. strchr -> __strchrnul) so the IR trace can't see it.
int searcherReturnsIntoArg0(StringRef raw) {
  StringRef n = raw;
  n.consume_front("__");
  return annotations().returnIntoArg0.count(n.str()) ? 0 : -1;
}

// Is arg `argIdx` a NULL-terminated array of cstr pointers (argv/envp)? The array
// walk is interprocedural (exec -> syscall), so this is data-listed.
bool isPtrArrayArg(StringRef raw, size_t argIdx) {
  StringRef n = raw;
  n.consume_front("__");
  auto &m = annotations().ptrArrayArgs;
  auto it = m.find(n.str());
  if (it == m.end()) return false;
  for (int i : it->second)
    if (i >= 0 && (size_t)i == argIdx) return true;
  return false;
}

// GENERAL detector for the "out pointer-to-pointer into another arg" case: does
// the function store into *P (P a char**/void** arg) a pointer value that derives
// from a *different* pointer argument? If so, *P is an offset into that arg (the
// endptr idiom), translatable soundly. Returns the target arg's DWARF index, or
// -1. This is the primary path — it works for any in-body write in any library;
// the strtoX name table below is only the fallback for glibc functions that
// write *endptr in a sibling TU (invisible here).
int detectOutPtrIntoArg(const Argument *P, unsigned sretOffset) {
  int pIdx = dwarfIndexOf(P, sretOffset);
  SmallVector<const Value *, 8> work;
  SmallPtrSet<const Value *, 16> seen;
  work.push_back(P);
  seen.insert(P);
  while (!work.empty()) {
    const Value *D = work.pop_back_val();
    for (const User *U : D->users()) {
      if (auto *st = dyn_cast<StoreInst>(U)) {
        if (st->getPointerOperand() == D &&
            st->getValueOperand()->getType()->isPointerTy()) {
          bool off = false;
          SmallPtrSet<const Value *, 16> s2;
          int q = traceReturnPtr(st->getValueOperand(), sretOffset, off, s2);
          if (q >= 0 && q != pIdx) return q; // *P = (value derived from arg q)
        }
      } else if (auto *g = dyn_cast<GetElementPtrInst>(U)) {
        if (g->getPointerOperand() == D && seen.insert(g).second) work.push_back(g);
      } else if (isa<BitCastInst>(U)) {
        if (seen.insert(U).second) work.push_back(U);
      }
    }
  }
  return -1;
}

// Does value V derive (via casts/GEP/phi/select) from a `load` of *P (the slot)?
bool tracesToLoadOfSlot(const Value *V, const Value *P,
                        SmallPtrSetImpl<const Value *> &seen) {
  if (!V || !seen.insert(V).second) return false;
  V = V->stripPointerCasts();
  if (auto *ld = dyn_cast<LoadInst>(V))
    return ld->getPointerOperand()->stripPointerCasts() == P;
  if (auto *g = dyn_cast<GetElementPtrInst>(V))
    return tracesToLoadOfSlot(g->getPointerOperand(), P, seen);
  if (auto *phi = dyn_cast<PHINode>(V)) {
    for (const Value *iv : phi->incoming_values())
      if (tracesToLoadOfSlot(iv, P, seen)) return true;
  }
  if (auto *sel = dyn_cast<SelectInst>(V))
    return tracesToLoadOfSlot(sel->getTrueValue(), P, seen) ||
           tracesToLoadOfSlot(sel->getFalseValue(), P, seen);
  return false;
}

// CURSOR: the function stores into *P a pointer derived from a load of *P — i.e.
// *P is advanced within its own pointee buffer (strsep stringp, mbsrtowcs *src).
bool detectCursor(const Argument *P) {
  for (const User *U : P->users()) {
    auto *st = dyn_cast<StoreInst>(U);
    if (!st || st->getPointerOperand() != P) continue;
    if (!st->getValueOperand()->getType()->isPointerTy()) continue;
    SmallPtrSet<const Value *, 16> seen;
    if (tracesToLoadOfSlot(st->getValueOperand(), P, seen)) return true;
  }
  return false;
}

// No store anywhere targets slot P, i.e. *P is only read, never written. A
// read-only inner pointer (qsort/scandir comparators: const struct dirent **).
bool slotIsReadOnly(const Argument *P) {
  for (const User *U : P->users())
    if (auto *st = dyn_cast<StoreInst>(U))
      if (st->getPointerOperand() == P) return false;
  return true;
}

// Does v resolve to the ADDRESS of a module global (a static buffer)? We follow
// address-arithmetic only (casts/GEP/PHI/select) — NOT loads: returning a value
// LOADED from a global (e.g. a heap pointer stored in a global) is not a static
// buffer, only returning &global (or &global.field) is.
bool ptrIsGlobalAddr(const Value *v, SmallPtrSetImpl<const Value *> &seen) {
  if (!v || !seen.insert(v).second) return false;
  v = v->stripPointerCasts();
  if (isa<GlobalVariable>(v)) return true;
  if (auto *g = dyn_cast<GEPOperator>(v))
    return ptrIsGlobalAddr(g->getPointerOperand(), seen);
  if (auto *phi = dyn_cast<PHINode>(v)) {
    for (const Value *iv : phi->incoming_values())
      if (ptrIsGlobalAddr(iv, seen)) return true;
    return false;
  }
  if (auto *sel = dyn_cast<SelectInst>(v))
    return ptrIsGlobalAddr(sel->getTrueValue(), seen) ||
           ptrIsGlobalAddr(sel->getFalseValue(), seen);
  return false;
}

// Every non-null return path yields a pointer into a module-global/static buffer
// (inet_ntoa/ctime/localtime return &static_buf). Conservative: if ANY return
// path is not a global address, this is not a clean static-buffer return.
bool returnDerivesFromGlobal(const Function &F) {
  bool any = false;
  for (const BasicBlock &bb : F)
    if (auto *ri = dyn_cast<ReturnInst>(bb.getTerminator()))
      if (Value *rv = ri->getReturnValue()) {
        SmallPtrSet<const Value *, 16> seen;
        if (!ptrIsGlobalAddr(rv, seen)) return false;
        any = true;
      }
  return any;
}

// Does the function return a pointer derived from a load of *P? (strsep returns
// the token, a pointer into P's cursor buffer.)
bool retDerivesFromCursor(const Function &F, const Value *P) {
  for (const BasicBlock &bb : F)
    if (auto *ri = dyn_cast<ReturnInst>(bb.getTerminator()))
      if (Value *rv = ri->getReturnValue()) {
        SmallPtrSet<const Value *, 16> seen;
        if (tracesToLoadOfSlot(rv, P, seen)) return true;
      }
  return false;
}

// The strtoX/wcstoX parse family: int strtol(const char *nptr, char **endptr,
// int base[, locale]). POSIX guarantees *endptr points INTO nptr (arg0). glibc
// writes *endptr in a sibling TU (____strtol_l_internal, only declared here), so
// detectOutPtrIntoArg can't see it — this name table is the interprocedural
// FALLBACK for that family (endptr=arg1, into=arg0).
bool isParseEndptrFn(StringRef raw) {
  StringRef n = raw.ltrim('_');   // __strtol / ____strtol_l_internal -> strip leading _
  n.consume_front("isoc23_");
  n.consume_back("_internal");    // __strtol_internal -> strtol
  n.consume_back("_l");           // strtol_l -> strtol
  if (!n.consume_front("strto") && !n.consume_front("wcsto")) return false;
  static const char *suf[] = {"l",  "ll",   "ul",  "ull",  "imax", "umax",
                              "q",  "uq",   "d",   "f",    "ld",   "f16",
                              "f32","f64",  "f128","f32x", "f64x"};
  for (const char *s : suf)
    if (n == s) return true;
  return false;
}

// Final decisiveness net (E2): any non-mappable node anywhere → force_local.
// Catches by-value struct/union/array args (which the pointer-only per-arg loop
// skips) and any other leftover. Handles are opaque — their uncopied pointee is
// not inspected.
bool treeHasUnmappable(const TreeNode *n) {
  if (n->isHandle || n->ptrIntoArg) return false;
  if (n->kind == NodeKind::Unknown) return true;
  if (n->kind == NodeKind::Pointer && n->sizeKind != SizeKind::Const &&
      n->sizeKind != SizeKind::FromArg &&
      n->sizeKind != SizeKind::FromArgPointee && n->sizeKind != SizeKind::Cstr &&
      n->sizeKind != SizeKind::PtrArray && n->sizeKind != SizeKind::StrideVector)
    return true;
  for (const auto &c : n->children)
    if (treeHasUnmappable(c.get())) return true;
  return false;
}

bool isSizeyTypeName(const std::string &t) {
  return t == "size_t" || t == "unsigned long" || t == "unsigned long long" ||
         t == "long" || t == "unsigned int" || t == "int" || t == "unsigned" ||
         t == "long unsigned int" || t == "long int";
}
// Prefer genuine size_t-shaped names over plain int when several integers exist.
int sizeyRank(const std::string &t) {
  if (t == "size_t" || t == "unsigned long" || t == "unsigned long long" ||
      t == "long unsigned int")
    return 3;
  if (t == "long" || t == "long int" || t == "unsigned int" || t == "unsigned")
    return 2;
  if (t == "int")
    return 1;
  return 0;
}

// A direct call, to a callee with a body we could analyze, that received this
// pointer as one of its own arguments -- a candidate for one-hop interprocedural
// length detection (see detectDelegatedArrayBound below).
struct DelegateCall {
  const CallBase *cb;
  unsigned argIdx; // which of cb's call-site operands received the pointer
};

// ELEMENT-count (and, separately, per-element stride) evidence from ONE
// hand-written counted loop indexing a pointer -- e.g. a BLAS-style
// `while(i<n){ y[iy]+=da*x[ix]; ix+=inc_x; ...}` walk: length=n, stride=
// inc_x. `stride` is null when no argument governs the per-iteration step;
// `constStride` covers the other common case instead -- a genuine
// compile-time-constant step (most often 1, an ordinary `x[i]`/pointer-walk
// loop with no separate increment parameter at all -- see
// unwrapConstantStride). At most one of `stride`/`constStride` is ever set.
// length alone is still meaningful (see its consumption in inferFunction)
// even without either.
struct LoopBound {
  const Value *length = nullptr;
  // True iff `length` came from an actual proof -- either ScalarEvolution's
  // own exact trip-count analysis, or detectFactoredStrideTripCount's
  // decomposition of a pre-scaled bound (loopBoundValues) -- rather than
  // dominatingArgumentGuard's dominator-tree walk. The guard walk is
  // DELIBERATELY imprecise (a dominating-but-unrelated argument check can
  // match -- see its own comment) and proves nothing about the loop's real
  // trip count, only that SOME argument is checked somewhere above it: sound
  // as "this pointer is array-shaped" evidence (a force_local warning) on
  // its own, but never accepted as a full extent pairing.
  bool lengthProven = false;
  const Value *stride = nullptr;
  std::optional<uint64_t> constStride;
  // Which GEP this bound was computed from -- needed both to exclude this
  // GEP's own (loop-varying, hence otherwise-"unresolved") entry from
  // Access::staticAccesses' coverage check, and to re-derive the same Loop/
  // DominatorTree later (a LoopBound outlives the analysis scope it was
  // computed under).
  const GetElementPtrInst *gep = nullptr;

  // Peeled-first-iteration candidate (OpenBLAS's isamax_k and 27 siblings).
  // Populated ONLY
  // when the address induction variable provably starts at EXACTLY the
  // stride recorded here instead of 0 -- kept in wholly SEPARATE fields
  // from `stride`/`constStride` above, never merged into them: a "starts
  // at stride" address recurrence must NEVER be accepted as grounds for
  // the ordinary (zero-start) pairing above, only for this one, which
  // additionally requires the loop's own trip count to be EXACTLY
  // peeledLengthArg-1 (a strictly stronger, separately-proven condition
  // from the ordinary exact-trip-count proof) and a corroborating
  // dominating offset-zero access elsewhere on the same pointer
  // (Access::staticAccesses) before this candidate is ever promoted to a
  // real extent -- see detectPeeledPrefixBound/corroboratePeeledPrefix.
  const Value *peeledLengthArg = nullptr;
  const Value *peeledStride = nullptr;
  std::optional<uint64_t> peeledConstStride;
};

// One access to the pointee through a SINGLE, non-loop-recurring GEP, or a
// direct dereference of the pointer itself (offset 0) -- e.g. x[0], x[3],
// x[-1], *x. Recorded for EVERY such access analyzeAccess finds, in
// addition to (not instead of) the specialized loopBounds/lengths sinks
// above. Consumed by noOtherAccesses to verify an accepted (length,
// stride) pairing accounts for EVERY access through the pointer, not just
// whichever loop happened to resolve first (see
// plan-openblas-max-family-inference.md's "all-access correctness
// blocker": a proof that covers one access pattern is not automatically
// sound for the whole pointer), and by corroboratePeeledPrefix to find the
// peeled family's own offset-zero access.
struct StaticAccess {
  // nullopt = a dynamic/unresolved index that isn't part of any recognized
  // loop either -- e.g. `x[m]` for some OTHER, unrelated argument `m`.
  // Never guessed at; an unresolved entry always fails the coverage check.
  std::optional<int64_t> offset;
  const Instruction *inst = nullptr;
};

// Per-argument access summary, accumulated by walking derived pointers.
struct Access {
  bool read = false;
  bool written = false;
  bool escapes = false;     // pointer stored away / passed to unknown callee
  bool stringOp = false;    // flows into a C-string libcall
  bool unknownCallee = false;
  // A GEP off this pointer was found whose indices are not PROVABLY all
  // constant zero -- i.e. the pointer is used as more than a single object,
  // whether or not that multi-element access ever resolves to an exact
  // extent. Covers an induction-variable GEP (whether or not
  // loopBoundValues can prove its trip count), a dynamic non-loop index
  // (`x[i]`), a nonzero constant index (`x[3]`), and a negative offset --
  // every one of these fails `hasAllZeroIndices()` the same way. Set
  // independently of `loopBounds` below so a GEP whose extent couldn't be
  // proven still blocks the single-element fallback instead of being
  // silently discarded (see its consumption in inferFunction).
  bool requiresDynamicExtent = false;
  SmallVector<const Value *, 4> lengths; // BYTE-count operands paired with this
                                          // ptr (memcpy-family length args) --
                                          // trusted directly as a byte size.
  // ELEMENT-count (+ stride) evidence from a hand-written counted loop (see
  // LoopBound above). Deliberately kept separate from `lengths` above:
  // unlike a memcpy length, `length` here is a number of ELEMENTS, not
  // bytes -- LIND_SIZE_FROM_ARG has no N*elemsize scaling, so feeding it
  // into the same trusted byte-count sink would make the tool confidently
  // emit a size wrong by a factor of the element size, with no warning at
  // all. Consumed to populate a LIND_SIZE_STRIDE_VECTOR spec when both
  // length AND stride resolve to real caller arguments; falls back to fail-
  // closed (force_local) when only length resolves.
  SmallVector<LoopBound, 2> loopBounds;
  SmallVector<DelegateCall, 2> delegateCalls;
  // Every single-point (non-loop-recurring) access found through this
  // pointer -- see StaticAccess's own comment.
  SmallVector<StaticAccess, 4> staticAccesses;
};

// Classify the role of operand `opIdx` in a call to a known lib function.
struct LibRole {
  bool known = false;
  bool reads = false;
  bool writes = false;
  bool isString = false;
  int lenOperand = -1; // operand index carrying the byte length, or -1
};

LibRole classifyLibArg(StringRef name, unsigned opIdx, const CallBase *cb) {
  LibRole r;
  auto set = [&](bool rd, bool wr, bool str, int len) {
    r.known = true; r.reads = rd; r.writes = wr; r.isString = str;
    r.lenOperand = len;
  };
  // dst=0 write, src=1 read; len at the noted operand.
  if (name == "memcpy" || name == "memmove" || name == "mempcpy") {
    if (opIdx == 0) set(false, true, false, 2);
    else if (opIdx == 1) set(true, false, false, 2);
  } else if (name == "memset") {
    if (opIdx == 0) set(false, true, false, 2);
  } else if (name == "memcmp") {
    if (opIdx <= 1) set(true, false, false, 2);
  } else if (name == "strcpy" || name == "stpcpy" || name == "strcat") {
    if (opIdx == 0) set(false, true, true, -1);
    else if (opIdx == 1) set(true, false, true, -1);
  } else if (name == "strncpy" || name == "stpncpy" || name == "strncat") {
    if (opIdx == 0) set(false, true, false, 2);
    else if (opIdx == 1) set(true, false, false, 2);
  } else if (name == "strlen") {
    if (opIdx == 0) set(true, false, true, -1);
  } else if (name == "strnlen") {
    if (opIdx == 0) set(true, false, false, 1);
  } else if (name == "strcmp" || name == "strchr" || name == "strrchr" ||
             name == "strstr" || name == "strspn" || name == "strcspn" ||
             name == "strpbrk" || name == "strdup" || name == "puts") {
    set(true, false, true, -1);
  } else if (name == "strncmp") {
    if (opIdx <= 1) set(true, false, false, 2);
  }
  (void)cb;
  return r;
}

// Peel a SCEV down to a bare argument-derived Value, but ONLY through casts
// and an smax/umax clamp against a constant with exactly one non-constant
// operand -- both are ScalarEvolution's OWN choice of representation for the
// SAME quantity (a clamp SCEV inserts to stay conservative when nothing
// upstream already proved positivity), never a transform that changes what
// the quantity IS. Any other shape -- in particular a Mul, or an Add that
// doesn't collapse away entirely under SCEV's own constant folding -- means
// the quantity is not PROVABLY just the bare argument, and must be rejected
// rather than guessed: e.g. a genuine `2*n` element count or a stride that's
// really `2*incx` at the source level must never collapse to "depends on n
// / incx".
const Value *unwrapArgumentSCEV(const SCEV *S) {
  for (unsigned depth = 0; S && depth < 8; ++depth) {
    if (auto *unk = dyn_cast<SCEVUnknown>(S))
      return unk->getValue();
    if (auto *cast = dyn_cast<SCEVCastExpr>(S)) { S = cast->getOperand(); continue; }
    if (isa<SCEVUMaxExpr>(S) || isa<SCEVSMaxExpr>(S)) {
      auto *nary = cast<SCEVNAryExpr>(S);
      const SCEV *nonConst = nullptr;
      unsigned nonConstCount = 0;
      for (unsigned i = 0; i < nary->getNumOperands(); ++i) {
        if (isa<SCEVConstant>(nary->getOperand(i))) continue;
        nonConst = nary->getOperand(i);
        ++nonConstCount;
      }
      if (nonConstCount != 1) return nullptr;
      S = nonConst;
      continue;
    }
    return nullptr; // Mul, unresolved Add, or any other shape -- reject
  }
  return nullptr;
}

// Peel a per-iteration step SCEV down to a small positive compile-time
// constant -- the OTHER common shape a stride takes, distinct from
// unwrapArgumentSCEV's argument-derived one: a plain `x[i]` or pointer-walk
// (`*p; p++`) loop, where nothing in the source names a separate increment
// parameter at all, just a fixed per-iteration element count (usually 1;
// `x[k*i]` for a fixed compile-time `k` also qualifies). Only a BARE
// SCEVConstant qualifies -- unlike unwrapArgumentSCEV, no cast-peeling or
// smax/umax-clamp unwrapping is needed or attempted here, since a literal
// step is never produced through either of those shapes. Anything else
// (an Add, a Mul by a non-constant, a value that depends on an argument at
// all) is not PROVABLY a fixed number and must be rejected, not guessed --
// this is the mirror image of unwrapArgumentSCEV's own "reject the rest"
// discipline, applied to the constant case instead of the argument case.
// A zero or negative step is rejected too: SCEV's own no-wrap flag on the
// address recurrence already rules out a step of exactly 0 meaning
// anything but "not actually incrementing" for a loop that visits more
// than one element, and a negative constant step would need the same
// backwards-walk handling LIND_SIZE_STRIDE_VECTOR's negative-stride check
// already fails closed on for an argument-derived stride -- not attempted
// here either, for the same reason.
std::optional<uint64_t> unwrapConstantStride(const SCEV *S) {
  auto *c = dyn_cast<SCEVConstant>(S);
  if (!c)
    return std::nullopt;
  const APInt &v = c->getAPInt();
  // Width-agnostic: an adversarial/pathological IR index type could be
  // arbitrarily wide, and getSExtValue()-style conversions assert rather
  // than fail gracefully outside a bounded width -- checking activeBits
  // first keeps this fail-closed (reject, don't crash) instead of trusting
  // the operand to already be a sane width.
  if (v.isNegative() || v.isZero() || v.getActiveBits() > 32)
    return std::nullopt;
  return v.getZExtValue();
}

// unwrapConstantStride above reads an ELEMENT-count step directly off a
// GEP's own INDEX operand (the `x[i]` shape). The other IR shape clang
// produces for an identical source-level fixed-stride walk -- `*p; p++`,
// with no array-index syntax at all -- puts the recurrence on the GEP's
// POINTER operand instead (see loopBoundValues' own comment on where this
// is used), and ScalarEvolution expresses THAT recurrence's step in BYTES,
// not elements, since it's a pointer-typed SCEV. This is the one place in
// the file that converts between the two units, kept narrowly scoped to a
// compile-time-constant byte step -- an argument-derived byte step (a
// pointer advanced by a runtime `inc_x` each iteration) is a SCEVMulExpr,
// not a bare constant, and is correctly left unattempted here for the same
// reason unwrapArgumentSCEV rejects a Mul outright: recovering "inc_x" back
// out of "8*inc_x" would need a scale-factor concept this schema doesn't
// have, not a guess. Fails closed (nullopt) on a non-exact division too --
// a byte step that isn't a whole multiple of the element size cannot be a
// clean per-element stride at all.
std::optional<uint64_t> unwrapConstantByteStride(const SCEV *S, uint64_t elemSize) {
  if (elemSize == 0)
    return std::nullopt;
  std::optional<uint64_t> bytes = unwrapConstantStride(S);
  if (!bytes || *bytes % elemSize != 0)
    return std::nullopt;
  return *bytes / elemSize;
}

// Peels a trip-count SCEV down to a bare argument when it's EXACTLY
// `argument - 1` -- the shape SE.getTripCountFromExitCount produces for a
// loop whose counter starts at the constant 1 instead of 0 (`i=1;
// while(i<n)` runs n-1 times, the
// first element having been handled by hand before the loop). Unlike
// unwrapArgumentSCEV (which requires the value to reduce to a BARE
// argument, rejecting any Add outright), this recognizes that one
// further-removed shape and only that one: a plain two-operand SCEVAddExpr
// of a SCEVUnknown-rooted argument and the constant -1. Anything else
// (more operands, no argument, a constant other than -1, ...) is rejected
// -- fail closed rather than approximate.
const Value *unwrapArgumentMinusOne(const SCEV *S) {
  auto *add = dyn_cast<SCEVAddExpr>(S);
  if (!add || add->getNumOperands() != 2)
    return nullptr;
  const SCEV *a = add->getOperand(0);
  const SCEV *b = add->getOperand(1);
  auto *c = dyn_cast<SCEVConstant>(a);
  const SCEV *other = b;
  if (!c) {
    c = dyn_cast<SCEVConstant>(b);
    other = a;
  }
  // isAllOnes() is exactly "every bit set", which for any bit width IS the
  // two's-complement encoding of -1 -- width-agnostic, no sign-extension
  // or truncation needed to compare against a literal -1 of some other
  // width.
  if (!c || !c->getAPInt().isAllOnes())
    return nullptr;
  return unwrapArgumentSCEV(other);
}

// Collect every icmp/fcmp reachable from a branch condition through and/or
// combinations (a guard often checks more than one thing at once, e.g.
// `if (n<=0 || alpha==0.0) return;`, compiled to
// `or i1 (icmp slt n, 1), (fcmp oeq alpha, 0.0)` feeding one branch --
// though for operands loaded from memory (the Fortran-by-reference
// idiom), InstCombine sometimes prefers the logically identical branchless
// form `select i1 (icmp slt n, 1), i1 true, i1 (fcmp oeq alpha, 0.0)`
// instead (confirmed: same source, same semantics, different compiled
// shape depending on whether the operands are direct SSA values or
// loads) -- recognized below as the unconditional equivalence it is
// (`select(c, true, X)` == `c or X`), not a guess.
void collectComparisons(const Value *cond, SmallVectorImpl<const CmpInst *> &out,
                        SmallPtrSetImpl<const Value *> &seen, unsigned depth) {
  if (depth > 6 || !cond || !seen.insert(cond).second)
    return;
  if (auto *cmp = dyn_cast<CmpInst>(cond)) {
    out.push_back(cmp);
    return;
  }
  if (auto *bo = dyn_cast<BinaryOperator>(cond)) {
    if (bo->getOpcode() == Instruction::And || bo->getOpcode() == Instruction::Or) {
      collectComparisons(bo->getOperand(0), out, seen, depth + 1);
      collectComparisons(bo->getOperand(1), out, seen, depth + 1);
    }
    return;
  }
  if (auto *sel = dyn_cast<SelectInst>(cond)) {
    auto *trueC = dyn_cast<ConstantInt>(sel->getTrueValue());
    auto *falseC = dyn_cast<ConstantInt>(sel->getFalseValue());
    if (trueC && trueC->isOne()) { // select(c, true, X) == c or X
      collectComparisons(sel->getCondition(), out, seen, depth + 1);
      collectComparisons(sel->getFalseValue(), out, seen, depth + 1);
    } else if (falseC && falseC->isZero()) { // select(c, X, false) == c and X
      collectComparisons(sel->getCondition(), out, seen, depth + 1);
      collectComparisons(sel->getTrueValue(), out, seen, depth + 1);
    }
  }
}

// Does `a` and `b` resolve to the SAME underlying function argument, either
// directly or through one level of pointer-load (the Fortran-by-reference
// idiom)? Argument* identity is stable/comparable directly since both
// Values always come from the SAME Function (the GEP's own).
bool sameArgument(const Value *a, const Value *b) {
  auto resolve = [](const Value *v) -> const Argument * {
    v = stripIntCasts(v);
    if (auto *arg = dyn_cast<Argument>(v)) return arg;
    if (auto *ld = dyn_cast<LoadInst>(v))
      if (auto *arg = dyn_cast<Argument>(stripPtr(ld->getPointerOperand())))
        return arg;
    return nullptr;
  };
  const Argument *aa = resolve(a), *bb = resolve(b);
  return aa && bb && aa == bb;
}

// Walk the dominator tree from a loop up to the function entry, looking for
// ANY dominating conditional branch that checks a function argument against
// a constant -- e.g. an `if (n<=0) return;` early-exit ahead of the loop's
// actual element walk. Fills `out` with EVERY argument-derived operand
// found in the FIRST such dominating branch, not just one: a COMPOUND
// guard (`if (n<=0 || inc_x<=0) return;` -- confirmed the standard idiom
// across dozens of OpenBLAS's own kernels) checks more than one argument at
// once, and the caller (loopBoundValues) needs every candidate to tell
// which one is the real trip count apart from an unrelated check in the
// same branch -- see its own comment on how it disambiguates them.
//
// Deliberately less precise than confirming "this exact branch is the one
// that skips exactly this loop" (Loop::getLoopGuardBranch() attempts that,
// but requires a strictly canonical GuardBB->Preheader->Header->Latch->
// ExitBlock shape that does not survive LLVM's runtime-unroll-with-remainder
// transform, which inserts an extra remainder-handling split between the
// guard and the real preheader). A dominating-but-unrelated argument check
// would also match here. That's acceptable because a match is never
// promoted to a real extent -- only used as "this pointer is array-shaped"
// evidence for the force_local warning.
void dominatingArgumentGuard(const Loop *L, DominatorTree &DT,
                             SmallVectorImpl<const Value *> &out) {
  BasicBlock *start = L->getLoopPreheader();
  if (!start)
    start = L->getHeader();
  DomTreeNode *node = DT.getNode(start);
  for (unsigned hops = 0; node && hops < 12; ++hops, node = node->getIDom()) {
    BasicBlock *bb = node->getBlock();
    if (!bb)
      continue;
    auto *br = dyn_cast_or_null<BranchInst>(bb->getTerminator());
    if (!br || !br->isConditional())
      continue;
    SmallVector<const CmpInst *, 4> cmps;
    SmallPtrSet<const Value *, 8> seen;
    collectComparisons(br->getCondition(), cmps, seen, 0);
    for (const CmpInst *cmp : cmps) {
      for (unsigned i = 0; i < 2; ++i) {
        const Value *op = stripIntCasts(cmp->getOperand(i));
        if (isa<Argument>(op)) {
          out.push_back(op);
          continue;
        }
        // Fortran-by-reference idiom: the compared value may be a LOCAL
        // loaded from a pointer argument (`BLASLONG n = *N;`), not the
        // argument itself -- collect the load; callers resolve it (and the
        // direct-Argument case above) uniformly via dwarfIndexOfMaybeLoaded.
        if (auto *ld = dyn_cast<LoadInst>(op))
          if (isa<Argument>(stripPtr(ld->getPointerOperand())))
            out.push_back(op);
      }
    }
    if (!out.empty())
      return; // first dominating branch with any candidate wins
  }
}

// True if an And combinator appears anywhere in `cond`'s and/or tree.
// dominatorProvesPositive below refuses to reason about a condition
// containing one: under an Or, every leaf being false is required for the
// whole condition to be false (De Morgan), which is what lets a single
// matched leaf prove something about the fallthrough edge; under an And,
// a false leaf says nothing about any OTHER leaf, so the same reasoning
// would be unsound.
bool conditionContainsAnd(const Value *cond, unsigned depth = 0) {
  if (depth > 6 || !cond)
    return false;
  if (auto *bo = dyn_cast<BinaryOperator>(cond)) {
    if (bo->getOpcode() == Instruction::And)
      return true;
    if (bo->getOpcode() == Instruction::Or)
      return conditionContainsAnd(bo->getOperand(0), depth + 1) ||
             conditionContainsAnd(bo->getOperand(1), depth + 1);
  }
  if (auto *sel = dyn_cast<SelectInst>(cond)) {
    // See collectComparisons' own comment on this equivalence.
    auto *trueC = dyn_cast<ConstantInt>(sel->getTrueValue());
    auto *falseC = dyn_cast<ConstantInt>(sel->getFalseValue());
    if (trueC && trueC->isOne()) // select(c, true, X) == c or X
      return conditionContainsAnd(sel->getCondition(), depth + 1) ||
             conditionContainsAnd(sel->getFalseValue(), depth + 1);
    if (falseC && falseC->isZero()) // select(c, X, false) == c and X
      return true;
  }
  return false;
}

// Proves `target` is strictly positive on every path that reaches
// `reachBlock`, walking the dominator-tree idom chain upward from
// `walkStart` (deliberately not llvm::isImpliedByDomCondition: its
// no-DominatorTree overload only inspects a single immediate predecessor
// block and is not safe to call outside a full pass pipeline -- confirmed
// via a real crash reproduction, not theorized). Requires the dominating
// branch's condition to be a PURE Or-tree (rejects outright if any And
// appears anywhere in it -- see conditionContainsAnd) containing a leaf of
// exactly the form `target < 1` / `target <= 0` (or the operand-reflected
// equivalent), AND that `reachBlock` is reached only through the edge
// where the WHOLE condition evaluates false -- i.e. this is the standard
// early-return guard idiom (`if (n<=0 || ...) return;`) this tool's own
// dominatingArgumentGuard already targets, not a general theorem prover.
// Refuses (returns false) on anything more complex than this exact,
// already-observed idiom: a hard proof requirement, not a best-effort
// heuristic, so it fails closed rather than guess. Shared by
// dominatorProvesPositive (target: a loop, reached only past its guard)
// and dominatorProvesPositiveBeforeInst (target: an arbitrary instruction,
// reached only past the SAME guard idiom) below -- identical reasoning,
// differing only in which block the proof is FOR.
bool dominatorProvesPositiveReaching(const Value *target, BasicBlock *walkStart,
                                     BasicBlock *reachBlock, DominatorTree &DT,
                                     BasicBlock **guardContinueOut) {
  const Value *t = stripIntCasts(target);
  DomTreeNode *node = DT.getNode(walkStart);
  for (unsigned hops = 0; node && hops < 12; ++hops, node = node->getIDom()) {
    BasicBlock *bb = node->getBlock();
    if (!bb)
      continue;
    auto *br = dyn_cast_or_null<BranchInst>(bb->getTerminator());
    if (!br || !br->isConditional())
      continue;
    bool reachOnTrue = DT.dominates(br->getSuccessor(0), reachBlock);
    bool reachOnFalse = DT.dominates(br->getSuccessor(1), reachBlock);
    if (reachOnTrue == reachOnFalse)
      continue; // ambiguous or unrelated to reaching the block -- keep walking
    if (!reachOnFalse)
      continue; // reached on the TRUE edge -- not the guard idiom this
                // proof targets; fail closed rather than reason about it
    if (conditionContainsAnd(br->getCondition()))
      continue;

    SmallVector<const CmpInst *, 4> cmps;
    SmallPtrSet<const Value *, 8> seen;
    collectComparisons(br->getCondition(), cmps, seen, 0);
    for (const CmpInst *c : cmps) {
      auto *icmp = dyn_cast<ICmpInst>(c);
      if (!icmp)
        continue;
      const Value *lhs = stripIntCasts(icmp->getOperand(0));
      const Value *rhs = stripIntCasts(icmp->getOperand(1));
      ICmpInst::Predicate p = icmp->getPredicate();
      const ConstantInt *C = nullptr;
      if (lhs == t) {
        C = dyn_cast<ConstantInt>(rhs);
      } else if (rhs == t) {
        C = dyn_cast<ConstantInt>(lhs);
        p = ICmpInst::getSwappedPredicate(p);
      }
      if (!C)
        continue;
      // Leaf being FALSE (required, since reachBlock lies on the false
      // edge of a pure Or-tree) must mean target > 0: `target < 1`
      // false -> target>=1; `target <= 0` false -> target>0. The
      // unsigned forms hold identically for a nonnegative type.
      int64_t cv = C->getSExtValue();
      bool leafProvesPositive =
          ((p == ICmpInst::ICMP_SLT || p == ICmpInst::ICMP_ULT) && cv == 1) ||
          ((p == ICmpInst::ICMP_SLE || p == ICmpInst::ICMP_ULE) && cv == 0);
      if (leafProvesPositive) {
        // `reachOnFalse` was already established true above (the branch
        // above `continue`s otherwise) -- successor(1) is exactly the
        // "continue toward reachBlock" edge, taken only when the guard
        // condition evaluated false, i.e. only once `target` is proven
        // positive. corroboratePeeledPrefix and
        // wrapperAccountsForOwnAccesses reuse this to prove a SEPARATE
        // access is reached only on that same path.
        if (guardContinueOut)
          *guardContinueOut = br->getSuccessor(1);
        return true;
      }
    }
  }
  return false;
}

// dominatorProvesPositiveReaching specialized to a loop: the block whose
// entry must be proven guarded is the loop header, and the walk starts one
// edge earlier (the preheader, when one exists) so the loop's OWN exit-test
// branch is never mistaken for the guard.
bool dominatorProvesPositive(const Value *target, const Loop *L, DominatorTree &DT,
                             BasicBlock **guardContinueOut = nullptr) {
  BasicBlock *start = L->getLoopPreheader();
  if (!start)
    start = L->getHeader();
  return dominatorProvesPositiveReaching(target, start, L->getHeader(), DT,
                                         guardContinueOut);
}

// dominatorProvesPositiveReaching specialized to an arbitrary instruction:
// the block whose entry must be proven guarded is the instruction's own
// parent, and (mirroring the no-preheader loop fallback above) the walk
// starts at that same block, checking its own terminator first before
// climbing to its idom.
bool dominatorProvesPositiveBeforeInst(const Value *target, const Instruction *inst,
                                       DominatorTree &DT,
                                       BasicBlock **guardContinueOut = nullptr) {
  BasicBlock *bb = const_cast<BasicBlock *>(inst->getParent());
  return dominatorProvesPositiveReaching(target, bb, bb, DT, guardContinueOut);
}

// A SECOND, independent exact proof for a loop's length, alongside the
// exact backedge-taken-count proof in loopBoundValues below: everything
// here is established directly from the loop's own IR plus `provenStride`
// (already exactly proven by the caller), so a successful result needs no
// separate confirmation step or opt-in -- it is unconditionally sound.
//
// The shape this proves: a loop whose SINGLE index variable does double
// duty as both the array address and the loop's own exit-test counter,
// stepping by `provenStride` each iteration instead of by 1 (the common
// BLAS idiom `n *= inc_x; while (i < n) { ...x[i]...; i += inc_x; }`).
// To keep running exactly N iterations with a step of
// `provenStride` rather than 1, the source has to pre-scale its own exit
// bound to `provenStride * N`, which is exactly why the exact backedge-
// taken-count proof above fails: the compiled bound is a genuine `mul`,
// not a value unwrapArgumentSCEV's cast-only peel can reduce to a bare
// argument. This function proves the SAME relationship a different way,
// straight from the multiplication's own operands, rather than attempting
// to simplify (N*S)/S back to N as a general symbolic identity -- which
// would be UNSOUND in general (S could be zero, the multiplication could
// wrap, S could be negative, the bound could carry an extra offset, ...).
// Every one of those failure modes is closed off by an explicit condition
// below; any one of them missing, this returns nullptr rather than a
// partial or best-effort answer:
//   - the loop has EXACTLY one exiting block (Loop::getExitingBlock()
//     returns non-null only then) -- otherwise some other, earlier exit
//     could make the real iteration count diverge from what this latch
//     predicate alone implies;
//   - the latch branch's "true" successor is the one that stays inside
//     the loop (getLatchCmpInst() guarantees only that this ICmp feeds
//     the latch branch, NOTHING about which edge continues vs. exits --
//     confirmed against LLVM's own implementation) -- a branch wired the
//     other way round means the loop actually continues while this
//     comparison is FALSE, a different (count-down-style) shape this
//     proof does not attempt;
//   - the latch predicate is STRICT (slt/ult/sgt/ugt), read in the
//     orientation that says which operand is the bound and which is the
//     counter;
//   - the comparison's COUNTER operand (the non-bound side) has the SAME
//     ScalarEvolution recurrence as `addressAR` -- the GEP's OWN address
//     induction variable -- either directly (a header-tested loop) or via
//     its post-increment value (the latch-tested shape comparing AFTER
//     `i += stride` in the same iteration). Without this, the bound
//     operand alone proves nothing: an entirely SEPARATE counter (its own
//     induction variable, unrelated to the array index) could run to
//     stride*length iterations while the array is actually walked by a
//     different variable with a different step, silently under-allocating;
//   - the bound operand is exactly a `mul` carrying the no-wrap flag
//     matching the comparison's own signedness (nsw for a signed
//     predicate, nuw for unsigned) -- required because an overflowing
//     multiplication's runtime value is undefined by the source language;
//     without the flag, nothing can be proven about what value it holds;
//   - EXACTLY ONE of the multiplication's two operands (after the same
//     cast-peeling unwrapArgumentSCEV performs) is the IDENTICAL Value to
//     `provenStride` -- not merely "some argument", the actual same SSA
//     value already independently proven as this GEP's own stride; the
//     OTHER operand is the length candidate;
//   - the length candidate resolves to a function argument, directly or
//     through one level of Fortran-by-reference load, exactly like every
//     other length source this tool recognizes;
//   - BOTH the length candidate and `provenStride` are proven strictly
//     positive by a dominating early-return guard (dominatorProvesPositive
//     below) -- required because the no-wrap flag alone rules out
//     overflow, not a negative operand, and "the loop runs exactly N
//     times" only holds for N,S > 0.
const Value *detectFactoredStrideTripCount(const Loop *L, ScalarEvolution &SE,
                                           DominatorTree &DT,
                                           const Value *provenStride,
                                           const SCEVAddRecExpr *addressAR) {
  BasicBlock *exiting = L->getExitingBlock();
  if (!exiting)
    return nullptr; // zero or more than one exiting block -- refuse
  ICmpInst *cmp = L->getLatchCmpInst();
  if (!cmp || cmp->getParent() != exiting)
    return nullptr;

  // getLatchCmpInst() guarantees only that this ICmp feeds the latch's
  // conditional branch -- NOTHING about which successor edge actually
  // continues the loop vs. exits it (confirmed directly against LLVM's own
  // implementation, not assumed). Require the standard shape explicitly:
  // successor(0) ("true") stays inside the loop, successor(1) ("false")
  // does not. A branch wired the other way round would mean the loop
  // actually continues while this comparison is FALSE -- a fundamentally
  // different (count-down-style) shape this proof does not attempt; refuse
  // rather than reinterpret the predicate to fit it.
  auto *latchBr = dyn_cast<BranchInst>(exiting->getTerminator());
  if (!latchBr || !latchBr->isConditional() || latchBr->getCondition() != cmp)
    return nullptr;
  bool trueContinues = L->contains(latchBr->getSuccessor(0));
  bool falseContinues = L->contains(latchBr->getSuccessor(1));
  if (!trueContinues || falseContinues)
    return nullptr;

  ICmpInst::Predicate pred = cmp->getPredicate();
  bool strictLess = pred == ICmpInst::ICMP_SLT || pred == ICmpInst::ICMP_ULT;
  bool strictGreater = pred == ICmpInst::ICMP_SGT || pred == ICmpInst::ICMP_UGT;
  bool predIsUnsigned = pred == ICmpInst::ICMP_ULT || pred == ICmpInst::ICMP_UGT;
  if (!strictLess && !strictGreater)
    return nullptr;
  Value *boundOperand = strictLess ? cmp->getOperand(1) : cmp->getOperand(0);
  Value *counterOperand = strictLess ? cmp->getOperand(0) : cmp->getOperand(1);

  // The comparison's OTHER operand must be the SAME induction variable the
  // GEP itself indexes with -- either the address recurrence's own value
  // (a header-tested loop, comparing before this iteration's increment) or
  // its post-increment value (the latch-tested shape this tool's other
  // fixtures use, comparing AFTER `i += stride` in the same iteration).
  // Without this, the bound operand alone proves nothing: a SEPARATE
  // counter (its own induction variable, unrelated to the array index)
  // could independently happen to run to stride*length iterations while
  // the array index itself is walked by a completely different variable
  // stepping by something else entirely -- silently under-allocating.
  // SCEV equality (pointer identity: ScalarEvolution uniques/hash-conses
  // its expressions, so structurally-equal SCEVs share one pointer) is the
  // right tool here, not syntactic Value identity: two distinct SSA
  // induction variables with identical PROVEN recurrences are genuinely
  // the same sequence of runtime values, so matching either is sound.
  const SCEV *counterSCEV = SE.getSCEV(counterOperand);
  if (counterSCEV != addressAR && counterSCEV != addressAR->getPostIncExpr(SE))
    return nullptr;

  auto *bo = dyn_cast<BinaryOperator>(stripIntCasts(boundOperand));
  if (!bo || bo->getOpcode() != Instruction::Mul)
    return nullptr;
  if (predIsUnsigned ? !bo->hasNoUnsignedWrap() : !bo->hasNoSignedWrap())
    return nullptr; // the multiplication's runtime value is otherwise unproven

  const Value *strideStripped = stripIntCasts(provenStride);
  Value *op0 = bo->getOperand(0);
  Value *op1 = bo->getOperand(1);
  Value *lengthCandidate = nullptr;
  if (stripIntCasts(op0) == strideStripped)
    lengthCandidate = op1;
  else if (stripIntCasts(op1) == strideStripped)
    lengthCandidate = op0;
  else
    return nullptr; // neither factor is the SAME value already proven as
                     // this GEP's own stride -- never guess which operand
                     // might correspond

  const Value *len = stripIntCasts(lengthCandidate);
  const LoadInst *lenLoad = dyn_cast<LoadInst>(len);
  if (!isa<Argument>(len) &&
      !(lenLoad && isa<Argument>(stripPtr(lenLoad->getPointerOperand()))))
    return nullptr;

  if (!dominatorProvesPositive(len, L, DT) ||
      !dominatorProvesPositive(strideStripped, L, DT))
    return nullptr;

  return lengthCandidate;
}

// Length AND (separately) stride for one GEP inside a countable loop --
// e.g. a BLAS-style `x[ix]` walk (ix+=inc_x each iteration): length=n,
// stride=inc_x. Builds DominatorTree/LoopInfo/AssumptionCache/
// ScalarEvolution once for both: length and stride for the SAME gep always
// need the SAME loop/analysis objects, so there's no reason to duplicate the
// work.
LoopBound loopBoundValues(const GetElementPtrInst *gep) {
  LoopBound result;
  Function *F = const_cast<Function *>(gep->getFunction());
  if (!F)
    return result;
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  const Loop *L = LI.getLoopFor(gep->getParent());
  if (!L)
    return result;
  AssumptionCache AC(*F);
  TargetLibraryInfoImpl TLII;
  TargetLibraryInfo TLI(TLII);
  ScalarEvolution SE(*F, TLI, AC, DT, LI);

  // Length: a property of the LOOP itself (its trip count), not of any one
  // induction variable inside it -- doesn't matter which IV the GEP happens
  // to index with, and doesn't matter what predicate/start/step the loop's
  // controlling comparison actually uses (`i<n`, `i<=n`, a non-zero start,
  // a non-unit step, ...): ScalarEvolution's own exact backedge-taken-count
  // analysis already accounts for all of that by construction, which no
  // amount of hand-rolled comparison-pattern-matching can safely replicate
  // (an `i<=n` loop's element count is n+1, not n -- getTripCountFromExitCount
  // computes that relationship correctly; a bare pattern match on the latch
  // comparison's operands, tried and abandoned here, could not). Falls back
  // to guard-scanning when the trip count doesn't resolve (e.g. LLVM's
  // runtime-unroll-with-remainder transform, which recomputes the trip
  // count via an equivalent but no-longer-argument-shaped udiv/zext/trunc
  // rewrite) -- but the guard fallback proves nothing exact (see
  // LoopBound::lengthProven); it is kept only as "this pointer is
  // array-shaped" evidence for the force_local warning, never accepted as
  // a real extent.
  // Width-preserving overload, not the default overflow-safe one: the
  // latter always widens by a bit (to represent ExitCount==UINT_MAX
  // exactly), which wraps the whole expression in an extra zext and defeats
  // unwrapArgumentSCEV's cast-then-bare-argument peel even for the ordinary
  // `for(i=0;i<n;i++)` case (ExitCount=n-1, TripCount=(n-1)+1, which only
  // folds back down to bare `n` when kept in ExitCount's own type). The
  // widening's purpose -- exactness at the ExitCount==UINT_MAX edge -- is
  // moot here: the runtime's own overflow checks (see LIND_SIZE_STRIDE_VECTOR
  // in lind_marshal.h) already guard the actual byte-size computation.
  // Stride is computed FIRST, ahead of length: it doesn't depend on
  // `result.length` at all, and its identity is needed below to
  // disambiguate a compound dominating guard. THIS gep's own index
  // operand's per-iteration step, if that index is itself an affine
  // recurrence of the SAME loop (a GEP whose index is invariant in this
  // loop, or varies with a DIFFERENT enclosing loop, has no meaningful
  // "stride" here) -- AND that recurrence PROVABLY STARTS AT ZERO. The
  // runtime always treats the pointer passed at the real call as the start
  // of the shadow-copied region (offset 0); if the address induction
  // variable actually starts somewhere else (e.g. `ix = inc_x;` before the
  // loop, rather than `ix = 0;`), the base pointer itself is never accessed
  // and the (1+(n-1)*stride) extent formula would silently miss everything
  // from the true first access up to n*stride, under-covering the real
  // accessed range. A `getelementptr <elemty>, ptr %p, iN %idx` (the only
  // shape this file's GEPs take -- always a single index, since none of
  // the pointee types involved are themselves aggregates) has its index as
  // the last operand.
  // Kept beyond this block (not just a local): detectFactoredStrideTripCount
  // below needs the GEP's OWN address recurrence to confirm the loop's
  // latch comparison is actually testing THIS induction variable, not some
  // unrelated one -- see its own comment.
  const SCEVAddRecExpr *addressAR = nullptr;
  if (gep->getNumOperands() >= 2) {
    Value *idxOperand = gep->getOperand(gep->getNumOperands() - 1);
    const SCEV *idxSCEV = SE.getSCEV(idxOperand);
    if (auto *ar = dyn_cast<SCEVAddRecExpr>(idxSCEV)) {
      // `x[i]`: the recurrence is on the INDEX, over a pointer operand
      // ScalarEvolution treats as loop-invariant.
      if (ar->getLoop() == L) {
        const SCEV *step = ar->getStepRecurrence(SE);
        const Value *strideArg = unwrapArgumentSCEV(step);
        std::optional<uint64_t> strideConstVal;
        if (!strideArg)
          strideConstVal = unwrapConstantStride(step);
        if (strideArg || strideConstVal) {
          auto *start = dyn_cast<SCEVConstant>(ar->getStart());
          if (start && start->getValue()->isZero()) {
            addressAR = ar;
            result.stride = strideArg;
            result.constStride = strideConstVal;
          } else {
            // Peeled-first-iteration candidate: the address
            // IV starts at EXACTLY the stride instead of 0 -- e.g.
            // `ix = inc_x;` before the loop, seeded by a peeled `x[0]`
            // access elsewhere. Recorded in wholly SEPARATE fields, never
            // merged into result.stride/constStride above: this candidate
            // is meaningless (and unsound to accept) on its own -- pairing
            // an uncorroborated nonzero start with this loop's OWN
            // (unrelated) trip count would silently under-cover the real
            // accessed range, exactly the bug the zero-start requirement
            // exists to prevent for the ordinary case. Only
            // detectPeeledPrefixBound, after finding a dominating
            // offset-zero access elsewhere AND a trip count proven to be
            // EXACTLY (a further argument) - 1, may ever promote this to
            // a real extent.
            bool startsAtStride = false;
            if (strideArg)
              startsAtStride = (ar->getStart() ==
                                 SE.getSCEV(const_cast<Value *>(strideArg)));
            else if (strideConstVal && start && !start->getAPInt().isNegative() &&
                     start->getAPInt().getActiveBits() <= 32)
              startsAtStride = start->getAPInt().getZExtValue() == *strideConstVal;
            if (startsAtStride) {
              result.peeledStride = strideArg;
              result.peeledConstStride = strideConstVal;
            }
          }
        }
      }
    }
    if (!addressAR) {
      // `*p; p++`: clang's OTHER lowering of the identical source-level
      // fixed-stride walk -- no array-index syntax at
      // all, so the GEP's own index is just a fixed per-iteration element
      // offset (typically `1`) and the recurrence instead lives on the
      // GEP's POINTER operand (a phi recurring on this same GEP around the
      // loop). ScalarEvolution computes that pointer-typed recurrence
      // directly; a bare SCEVUnknown start (no addition at all -- not even
      // a zero one, since SCEV normalizes that away already) means the
      // walk begins at exactly that one Value, the pointer-operand
      // equivalent of the index case's "starts at the constant 0" above.
      // Its step is in BYTES, not elements (see unwrapConstantByteStride);
      // only a compile-time-constant byte step is ever attempted here, for
      // the same reason unwrapArgumentSCEV never attempts a Mul.
      const SCEV *ptrSCEV =
          SE.getSCEV(const_cast<Value *>(gep->getPointerOperand()));
      if (auto *ar = dyn_cast<SCEVAddRecExpr>(ptrSCEV)) {
        if (ar->getLoop() == L && isa<SCEVUnknown>(ar->getStart())) {
          addressAR = ar;
          Type *elemTy = gep->getSourceElementType();
          uint64_t elemSize = F->getParent()->getDataLayout().getTypeAllocSize(elemTy);
          result.constStride =
              unwrapConstantByteStride(ar->getStepRecurrence(SE), elemSize);
        }
      }
    }
  }

  const SCEV *ec = SE.getBackedgeTakenCount(L);
  const SCEV *tripCount = nullptr;
  if (ec && !isa<SCEVCouldNotCompute>(ec)) {
    tripCount = SE.getTripCountFromExitCount(ec, ec->getType(), L);
    result.length = unwrapArgumentSCEV(tripCount);
  }
  if (result.length) {
    result.lengthProven = true;
  } else if (result.stride) {
    // A second, independent exact proof -- see
    // detectFactoredStrideTripCount's own comment -- for a loop whose
    // bound was pre-scaled by its own already-proven stride.
    if (const Value *factored = detectFactoredStrideTripCount(
            L, SE, DT, result.stride, addressAR)) {
      result.length = factored;
      result.lengthProven = true;
    }
  }
  // Peeled-first-iteration length candidate: independent of the ordinary
  // ec/tripCount usage above (both may be attempted against the SAME
  // tripCount SCEV; they ask different questions of it -- "is this a bare
  // argument" vs. "is this exactly argument-1"). Gated on having found a
  // peeled-eligible stride at all above; see detectPeeledPrefixBound for
  // where this actually gets corroborated and accepted.
  if ((result.peeledStride || result.peeledConstStride) && tripCount)
    result.peeledLengthArg = unwrapArgumentMinusOne(tripCount);
  if (!result.lengthProven) {
    // Guard fallback: a COMPOUND dominating guard (`if (n<=0 || inc_x<=0)
    // return;` -- confirmed the standard idiom across dozens of OpenBLAS's
    // own kernels) checks more than one argument in a single branch;
    // dominatingArgumentGuard returns every argument-derived operand it
    // found there, not just one, specifically so this can disambiguate
    // them against the (already resolved) stride: a parameter can never
    // legitimately be its own stride, so a candidate matching the stride
    // is almost certainly the guard's OTHER check, not the loop's real
    // trip count. Falls back to the first candidate when none avoid the
    // collision (e.g. no stride was found at all -- nothing to
    // disambiguate against, same behavior as before this refinement).
    // Never promoted to a real extent -- see LoopBound::lengthProven --
    // only used as "this pointer is array-shaped" evidence for the
    // force_local warning.
    SmallVector<const Value *, 4> candidates;
    dominatingArgumentGuard(L, DT, candidates);
    for (const Value *cand : candidates) {
      if (!result.stride || !sameArgument(cand, result.stride)) {
        result.length = cand;
        break;
      }
    }
    if (!result.length && !candidates.empty())
      result.length = candidates.front();
  }
  if (!result.length) {
    // No length -> a stride alone means nothing; never emit stride-only.
    result.stride = nullptr;
    result.constStride.reset();
  }
  if (!result.peeledLengthArg) {
    // Same discipline as above, for the peeled candidate's own fields.
    result.peeledStride = nullptr;
    result.peeledConstStride.reset();
  }
  result.gep = gep;
  return result;
}

// Walk all values derived (by GEP/cast/phi/select) from pointer argument `A`,
// recording how the pointee memory is read/written and any paired length.
Access analyzeAccess(const Argument *A) {
  Access acc;
  // Each worklist entry carries its OWN provable element-offset from `A`
  // (0 for `A` itself, propagated through casts unchanged, combined with a
  // GEP's own literal index when both are known) alongside the Value --
  // needed so a Load/Store found several hops away records the offset it
  // ACTUALLY dereferences, not just "0" (see StaticAccess). A PHI or
  // Select MERGES potentially different incoming offsets (the pointer-walk
  // idiom's own "current position" phi merges the base pointer with a
  // loop-varying GEP result -- see loopBoundValues' case 2) and is always
  // treated as unresolved rather than assuming either incoming value's
  // offset carries through.
  struct WorkItem {
    const Value *v;
    std::optional<int64_t> offset;
  };
  SmallVector<WorkItem, 16> work;
  SmallPtrSet<const Value *, 32> seen;
  work.push_back({A, 0});
  seen.insert(A);
  // A GEP that IS actually dereferenced (loaded/stored through) is a real
  // memory access, whatever else it's also used for; recorded here so the
  // post-pass below (dropping pure-address-computation GEPs) never prunes
  // one that's genuinely read or written.
  SmallPtrSet<const Value *, 16> dereferencedGeps;

  while (!work.empty()) {
    WorkItem item = work.pop_back_val();
    const Value *V = item.v;
    std::optional<int64_t> vOff = item.offset;
    for (const User *U : V->users()) {
      if (auto *ld = dyn_cast<LoadInst>(U)) {
        if (ld->getPointerOperand() == V) {
          acc.read = true;
          // A GEP result already recorded ITS OWN staticAccess entry below
          // -- recording the load that dereferences it too would double
          // the same access, and (for a loop-varying GEP) the load isn't
          // what noOtherAccesses' exclusion later matches against anyway.
          if (!isa<GetElementPtrInst>(V))
            acc.staticAccesses.push_back({vOff, ld});
          else
            dereferencedGeps.insert(V);
        }
      } else if (auto *st = dyn_cast<StoreInst>(U)) {
        if (st->getPointerOperand() == V) {
          acc.written = true;
          if (!isa<GetElementPtrInst>(V))
            acc.staticAccesses.push_back({vOff, st});
          else
            dereferencedGeps.insert(V);
        }
        if (st->getValueOperand() == V) acc.escapes = true; // ptr stored away
      } else if (auto *gep = dyn_cast<GetElementPtrInst>(U)) {
        if (gep->getPointerOperand() == V && seen.insert(gep).second) {
          if (!gep->hasAllZeroIndices())
            acc.requiresDynamicExtent = true;
          // Every GEP off this pointer is also a single-point access
          // candidate for the all-access coverage check (see StaticAccess)
          // -- a compile-time-constant index (x[0], x[3], x[-1]) records
          // its literal offset (relative to A, via vOff); a loop-varying
          // or otherwise dynamic index, or an unresolved base, is NOT
          // resolvable here and is recorded as unresolved (the owning
          // loop, if any, explains it separately via loopBounds below and
          // is excluded from the coverage check by identity).
          std::optional<int64_t> gepOffset;
          if (vOff) {
            if (gep->hasAllZeroIndices())
              gepOffset = *vOff;
            else if (gep->getNumOperands() == 2)
              if (auto *ci = dyn_cast<ConstantInt>(gep->getOperand(1)))
                gepOffset = *vOff + ci->getSExtValue();
          }
          work.push_back({gep, gepOffset});
          acc.staticAccesses.push_back({gepOffset, gep});
          // A hand-written counted loop indexing this pointer (a shape
          // memcpy-style library calls don't cover at all -- e.g. a
          // BLAS-style `while(i<n){ y[iy]+=da*x[ix]; ... }` walk, possibly
          // one hop away in a delegated callee). ELEMENT count (+ stride),
          // not a byte count -- goes to the separate loopBounds sink (see
          // its declaration above), never acc.lengths, so it's never
          // trusted as a byte size.
          LoopBound lb = loopBoundValues(gep);
          if (lb.length || lb.peeledLengthArg)
            acc.loopBounds.push_back(lb);
        }
      } else if (isa<BitCastInst>(U) || isa<AddrSpaceCastInst>(U)) {
        if (seen.insert(U).second) work.push_back({U, vOff});
      } else if (isa<PHINode>(U) || isa<SelectInst>(U)) {
        if (seen.insert(U).second) work.push_back({U, std::nullopt});
      } else if (auto *mi = dyn_cast<AnyMemIntrinsic>(U)) {
        // llvm.memcpy/memmove/memset
        if (auto *t = dyn_cast<AnyMemTransferInst>(mi)) {
          if (stripPtr(t->getRawDest()) == V) { acc.written = true; acc.lengths.push_back(t->getLength()); }
          if (stripPtr(t->getRawSource()) == V) { acc.read = true; acc.lengths.push_back(t->getLength()); }
        } else if (auto *s = dyn_cast<AnyMemSetInst>(mi)) {
          if (stripPtr(s->getRawDest()) == V) { acc.written = true; acc.lengths.push_back(s->getLength()); }
        }
      } else if (auto *cb = dyn_cast<CallBase>(U)) {
        const Function *callee = cb->getCalledFunction();
        bool handled = false;
        if (callee && callee->hasName()) {
          StringRef nm = callee->getName();
          for (unsigned oi = 0; oi < cb->arg_size(); ++oi) {
            if (cb->getArgOperand(oi) != V) continue;
            LibRole role = classifyLibArg(nm, oi, cb);
            if (role.known) {
              handled = true;
              acc.read |= role.reads;
              acc.written |= role.writes;
              acc.stringOp |= role.isString;
              if (role.lenOperand >= 0 &&
                  (unsigned)role.lenOperand < cb->arg_size())
                acc.lengths.push_back(cb->getArgOperand(role.lenOperand));
            }
          }
        }
        if (!handled) {
          // Unknown callee: be conservative — the pointee may be read and/or
          // written, and the pointer may escape.
          for (unsigned oi = 0; oi < cb->arg_size(); ++oi)
            if (cb->getArgOperand(oi) == V) {
              acc.escapes = true;
              acc.unknownCallee = true;
              // A direct call (known callee, not indirect) to a function whose
              // body we might have loaded elsewhere -- record it as a one-hop
              // delegation candidate. Bodyless callees (pure declarations) are
              // filtered by the caller of analyzeAccess, which only has a
              // CalleeIndex entry for defined functions in the first place.
              if (callee)
                acc.delegateCalls.push_back({cb, oi});
            }
        }
      }
    }
  }
  // A Load/Store that dereferences a PHI which is ALSO the pointer operand
  // of some GEP found during the walk (the pointer-walk idiom's own
  // "current position" phi -- loopBoundValues' case 2, e.g.
  // `%6=phi...; %8=load ptr %6; %10=gep ptr %6, i32 1`) is part of that
  // SAME address chain, not an independent access needing its own
  // coverage proof: the GEP's own staticAccesses entry (and, if it
  // resolved, its loopBounds entry) already accounts for it. Done as a
  // post-pass rather than inline: the walk can reach the load before the
  // GEP sharing the same phi, so this can't be decided at record time.
  SmallPtrSet<const Value *, 8> gepPointerOperands;
  for (const StaticAccess &sa : acc.staticAccesses)
    if (auto *g = dyn_cast<GetElementPtrInst>(sa.inst))
      gepPointerOperands.insert(g->getPointerOperand());

  // A GEP is address ARITHMETIC, not itself a memory access -- recording one
  // unconditionally (above) is deliberately pessimistic for the common case
  // (a loop's own indexing GEP, later loaded/stored), but over-conservative
  // for a real, common OpenBLAS idiom: `if (incx<0) x -= (n-1)*incx;`
  // rebases the pointer with a GEP whose result is NEVER dereferenced here
  // at all, only handed to a recognized library call or a one-hop delegate
  // (interface/axpy.c's negative-stride rebase before calling AXPYU_K, e.g.).
  // Whatever that call does with the rebased pointer is already accounted
  // for separately (a known LibRole's read/write/lengths effect, or the
  // delegated callee's own proof via detectDelegatedArrayBound) -- so a GEP
  // that is never itself dereferenced, and whose every other use is such a
  // call, contributes no access of its own and needs no separate
  // explanation. Deliberately narrow: ANY other use (an unnamed/indirect
  // call, a comparison, escaping as plain data, ...) leaves it exactly as
  // conservative as before.
  auto isPureAddressComputation = [&](const GetElementPtrInst *gep) {
    if (dereferencedGeps.count(gep))
      return false;
    for (const User *U : gep->users()) {
      if (isa<GetElementPtrInst>(U) || isa<BitCastInst>(U) ||
          isa<AddrSpaceCastInst>(U) || isa<PHINode>(U) || isa<SelectInst>(U))
        continue;
      auto *cb = dyn_cast<CallBase>(U);
      if (!cb || !cb->getCalledFunction())
        return false; // not a recognized address-chain link or named call
      bool isArgUse = false;
      for (unsigned oi = 0; oi < cb->arg_size(); ++oi)
        if (cb->getArgOperand(oi) == gep) { isArgUse = true; break; }
      if (!isArgUse)
        return false; // e.g. called AS the function value -- a real hazard
    }
    return true;
  };

  SmallVector<StaticAccess, 4> filtered;
  for (const StaticAccess &sa : acc.staticAccesses) {
    const Value *ptrOp = nullptr;
    if (auto *ld = dyn_cast<LoadInst>(sa.inst))
      ptrOp = ld->getPointerOperand();
    else if (auto *st = dyn_cast<StoreInst>(sa.inst))
      ptrOp = st->getPointerOperand();
    if (ptrOp && isa<PHINode>(ptrOp) && gepPointerOperands.count(ptrOp))
      continue; // explained by that same phi's own GEP entry
    if (auto *gep = dyn_cast<GetElementPtrInst>(sa.inst))
      if (isPureAddressComputation(gep))
        continue;
    filtered.push_back(sa);
  }
  acc.staticAccesses = std::move(filtered);
  return acc;
}

unsigned computeSretOffset(const Function &F) {
  return (F.arg_size() && F.getArg(0)->hasStructRetAttr()) ? 1 : 0;
}

// Resolve a Value to a top-level DWARF argument index, ALSO recording
// whether the raw wasm call passes that argument's VALUE directly or a
// POINTER to it (see dwarfIndexOfMaybeLoaded's own comment for why the
// second case exists -- classic Fortran BLAS's `blasint *N` unpacked as
// `BLASLONG n = *N;`). Distinct from dwarfIndexOfMaybeLoaded, which
// collapses this distinction into a single index -- fine for its own
// callers (FromArg vs. FromArgPointee is already a separate SizeKind per
// case), but StrideVector's two independent operands need the source
// preserved so the runtime knows how to read each one (see ExtentOperand).
ExtentOperand resolveExtentOperand(const Value *v, unsigned sretOffset) {
  if (int idx = dwarfIndexOf(v, sretOffset); idx >= 0)
    return {idx, ExtentSource::Value};
  if (auto *ld = dyn_cast<LoadInst>(v))
    if (int idx = dwarfIndexOf(stripPtr(ld->getPointerOperand()), sretOffset);
        idx >= 0)
      return {idx, ExtentSource::PointeeI32};
  return {};
}

// Build the ExtentOperand for a LoopBound::constStride result -- no
// argument to resolve at all, so unlike resolveExtentOperand above this
// never fails: `v` already IS the value (proven by unwrapConstantStride).
ExtentOperand constantExtentOperand(uint64_t v) {
  return {-1, ExtentSource::Constant, v};
}

// True iff every access recorded in `acc.staticAccesses` is explained by
// `explained` (the GEP(s)/instruction(s) that already justify the (length,
// stride) pairing under consideration -- typically the winning loop's own
// address GEP, and for a corroborated peeled candidate, the offset-zero
// access too), AND the pointer never escapes (stored away, or passed to a
// callee this tool can't account for -- an unresolved escape could do
// anything to the pointee, so no static access proof can be sound in its
// presence; plan-openblas-max-family-inference.md's Section 5), AND no
// bulk-memory operation (memcpy/memmove/memset -- Access::lengths) was
// ALSO found on the pointer. A memcpy's length is a BYTE count with no
// proven relationship to the ELEMENT-based (length, stride) pair under
// consideration -- e.g. a loop proving `n` elements at stride 1, followed
// by `memcpy(dst, x, (n+1)*sizeof(*x))`, reads one element the loop-
// derived envelope never covers. Likewise, AND no recognized-but-unbounded
// C-string library call (strlen, strcpy, strchr, ... -- Access::stringOp)
// was found on the pointer: classifyLibArg deliberately leaves these with
// no length operand at all (they scan until a NUL byte, wherever that
// falls), so a call like `strlen((char*)x)` after a loop proving `n`
// elements is a real access the loop's own envelope has no relationship
// to whatsoever -- it could read arbitrarily far past it. A single
// UNEXPLAINED access -- resolved to some other offset, genuinely
// unresolved, a bulk-memory length, or an unbounded string scan -- means
// the pairing under consideration does not actually account for
// everything the pointer touches, and must not be accepted: a proof that
// covers ONE access pattern through a pointer is not automatically sound
// for the WHOLE pointer (that same plan's "all-access correctness
// blocker"). Deliberately conservative rather than attempting to
// numerically prove an arbitrary resolved offset or byte length lies
// within the proven envelope: length and stride are themselves runtime
// argument VALUES at analysis time, not known numbers, so nothing beyond
// "this exact instruction is already accounted for" can be proven sound
// here without adding a symbolic range prover this tool does not have.
bool noOtherAccesses(const Access &acc,
                     const SmallPtrSetImpl<const Instruction *> &explained) {
  if (acc.escapes || !acc.lengths.empty() || acc.stringOp)
    return false;
  for (const StaticAccess &sa : acc.staticAccesses)
    if (!explained.count(sa.inst))
      return false;
  return true;
}

// Every OTHER loopBound entry whose OWN proven (length, stride) pair is
// IDENTICAL to `winner`'s (the exact same Value*s, or the exact same
// compile-time constant) is redundant confirmation, not a contradiction --
// a real, common OpenBLAS shape: kernel/riscv64/scal.c's sscal_k picks
// between an isfinite-checking and a plain loop body at runtime
// (`if (dummy2==1) {while(...)...} else {while(...)...}`), two
// STRUCTURALLY SEPARATE loops (two distinct GEP instructions) that happen
// to walk the exact identical (n, inc_x) range. Their own GEPs are
// explained by the SAME extent the winner already proves, and must be
// excluded from the coverage check too -- otherwise a sound, doubly-
// confirmed extent would be rejected as if the second loop were an
// unrelated, unexplained access. Deliberately exact (Value* identity, not
// "looks similar"): two DIFFERENT loops that happen to also prove SOME
// length/stride pair must never be conflated with each other unless
// provably the SAME value -- see noOtherAccesses' own comment on why nothing
// looser than exact-identity accounting is attempted here.
void collectAgreeingGeps(const Access &acc, const LoopBound &winner,
                         SmallPtrSetImpl<const Instruction *> &explained) {
  explained.insert(winner.gep);
  for (const LoopBound &lb2 : acc.loopBounds) {
    if (!lb2.gep || lb2.gep == winner.gep || !lb2.lengthProven ||
        lb2.length != winner.length)
      continue;
    bool sameStride =
        (lb2.stride && winner.stride && lb2.stride == winner.stride) ||
        (lb2.constStride && winner.constStride &&
         *lb2.constStride == *winner.constStride);
    if (sameStride)
      explained.insert(lb2.gep);
  }
}

// Corroborates a peeled-first-iteration LoopBound candidate (lb.
// peeledLengthArg set by loopBoundValues: the address IV provably starts
// at exactly the stride, and the loop's own trip count is exactly
// peeledLengthArg-1) against an ACTUAL offset-zero access recorded in
// `acc.staticAccesses` -- this family peels `x[0]` out by hand before
// the loop; loopBoundValues alone cannot
// see that separate access, only this loop's own shape.
//
// Requires, all independently proven, none approximated:
//   - a StaticAccess entry whose offset is EXACTLY 0 (not x[1], not a
//     dynamic/unresolved index);
//   - that access's instruction is dominated by the SAME positivity
//     guard's continue edge that proves peeledLengthArg > 0 -- i.e. the
//     access cannot execute unless the argument is already known
//     positive. Without this, the offset-zero access could execute even
//     when the argument is non-positive, a path the runtime's own
//     n<=0-sizes-to-0 rule would then under-cover (the shadow buffer
//     would be empty while the real function still touched x[0]),
//     making acceptance unsound;
//   - that access's instruction DOMINATES the loop's header -- it
//     executes, unconditionally, on every path that reaches the loop at
//     all, i.e. strictly before the loop's own walk begins.
// Returns the corroborating instruction (to be excluded, alongside the
// loop's own GEP, from noOtherAccesses -- it is now explained) or nullptr
// if no such access is found, in which case the peeled candidate is
// simply left unused, exactly like an unconfirmed dominatingArgumentGuard
// candidate.
const Instruction *corroboratePeeledPrefix(const LoopBound &lb, const Access &acc,
                                           const Loop *L, DominatorTree &DT) {
  if (!lb.peeledLengthArg)
    return nullptr;
  BasicBlock *guardContinue = nullptr;
  if (!dominatorProvesPositive(lb.peeledLengthArg, L, DT, &guardContinue) ||
      !guardContinue)
    return nullptr;
  for (const StaticAccess &sa : acc.staticAccesses) {
    if (!sa.offset || *sa.offset != 0 || sa.inst == lb.gep)
      continue;
    if (DT.dominates(guardContinue, sa.inst->getParent()) &&
        DT.dominates(sa.inst, L->getHeader()))
      return sa.inst;
  }
  return nullptr;
}

// Result of array-bound detection, direct or delegated: caller-argument
// operands, not raw Values -- both detectDirectArrayBound and
// detectDelegatedArrayBound resolve all the way down to this before
// returning, so their caller never has to know which path produced it.
// `stride` invalid with `length` valid is a real, meaningful outcome (array-
// shaped, but no distinct stride argument found/resolved) -- see its
// consumption in inferFunction for what that combination means for sizing.
struct ArrayBound {
  ExtentOperand length;
  ExtentOperand stride;
};

// Shared by detectDirectArrayBound and detectDelegatedArrayBound: given a
// peeled-eligible LoopBound `lb` (peeledLengthArg set) and the Access it
// came from, corroborate it (corroboratePeeledPrefix) and, if sound,
// resolve it to a real ArrayBound -- resolving each raw Value through
// `resolve`, so the SAME logic serves the function under direct analysis
// (resolveExtentOperand directly) and a one-hop delegated callee (the
// mapBack/checkOne composition, mapping through the call site's actual
// arguments) alike. Rebuilds a fresh DominatorTree/LoopInfo for lb.gep's
// own function -- lb crosses out of the scope loopBoundValues computed it
// under, so nothing from that analysis carries over.
template <typename ResolveFn>
ArrayBound detectPeeledPrefixBound(const Access &acc, const LoopBound &lb,
                                   ResolveFn resolve) {
  if (!lb.peeledLengthArg || !lb.gep)
    return {};
  Function *F = const_cast<Function *>(lb.gep->getFunction());
  if (!F)
    return {};
  DominatorTree DT(*F);
  LoopInfo LI(DT);
  const Loop *L = LI.getLoopFor(lb.gep->getParent());
  if (!L)
    return {};

  const Instruction *peelInst = corroboratePeeledPrefix(lb, acc, L, DT);
  if (!peelInst)
    return {};
  SmallPtrSet<const Instruction *, 4> explained;
  explained.insert(lb.gep);
  explained.insert(peelInst);
  if (!noOtherAccesses(acc, explained))
    return {};

  ExtentOperand len = resolve(stripIntCasts(lb.peeledLengthArg));
  if (!len.valid())
    return {};
  ExtentOperand stride = lb.peeledStride
      ? resolve(stripIntCasts(lb.peeledStride))
      : lb.peeledConstStride ? constantExtentOperand(*lb.peeledConstStride)
      : ExtentOperand{};
  if (!stride.valid() || stride.argIndex == len.argIndex)
    return {};
  return {len, stride};
}

// Array-bound detection WITHIN the function currently being analyzed (no
// delegation) -- e.g. a hand-written loop directly in the function's own
// body, or (rare) a memcpy-family call passing this pointer with a
// resolvable length. Resolves acc.loopBounds' raw Values down to caller
// argument operands; a resolved length with no resolved stride is still
// returned (length-only) rather than discarded.
//
// Scans ALL of acc.loopBounds and prefers, in order: (1) a PROVEN
// fully-resolved (length+stride) pairing -- stops here, the best possible
// outcome; (2) a length-only match (proven or not -- still valid
// "array-shaped" evidence for the force_local path even when no stride
// pairs with it). An UNPROVEN length never pairs with a stride: only ever
// used as descriptive "array-shaped" evidence, never a real extent (see
// LoopBound::lengthProven). Does NOT stop at the first length match found:
// analyzeAccess's worklist walk finds every GEP reachable from the
// pointer, across every branch, not just one "the" loop -- a guarded
// `if (inc_x==1) { <SIMD loop, step is a compile-time vector-width
// constant, not an argument> } else { <plain loop, step is inc_x> }`
// shape visits both loops' GEPs, and stopping at whichever is found first
// could miss a better (proven) entry later in the scan.
ArrayBound detectDirectArrayBound(const Access &acc, unsigned sretOffset) {
  ArrayBound best;
  for (const LoopBound &lb : acc.loopBounds) {
    ExtentOperand len = resolveExtentOperand(stripIntCasts(lb.length), sretOffset);
    if (!len.valid())
      continue;
    ExtentOperand stride = lb.stride
        ? resolveExtentOperand(stripIntCasts(lb.stride), sretOffset)
        : lb.constStride ? constantExtentOperand(*lb.constStride)
        : ExtentOperand{};
    // A parameter can never legitimately be its own stride -- len==stride is
    // proof the "length" side mis-resolved (dominatingArgumentGuard's
    // comparison-collector doesn't know WHICH operand of a compound
    // `n<=0 || inc_x<=0` guard is the real count vs. the increment, and can
    // pick either one). Discard the WHOLE entry, not just the stride half:
    // we have direct evidence this length is wrong, so it must not be kept
    // as a length-only fallback either.
    if (stride.valid() && stride.argIndex == len.argIndex)
      continue;
    if (lb.lengthProven && stride.valid()) {
      // A proof that covers only this ONE loop's own access is not
      // automatically sound for the whole pointer -- see noOtherAccesses'
      // own comment. Do not accept a pairing while some other, unexplained
      // access through the same pointer exists; fall through to keep
      // scanning (a LATER loopBound entry, or the peeled-prefix pass
      // below, might still find something sound). A SEPARATE loop proving
      // the exact same (length, stride) pair (collectAgreeingGeps) is
      // redundant confirmation, not an unexplained access.
      SmallPtrSet<const Instruction *, 4> explained;
      collectAgreeingGeps(acc, lb, explained);
      if (noOtherAccesses(acc, explained))
        return {len, stride}; // proven and fully resolved -- stop here
      continue;
    }
    if (!best.length.valid())
      best.length = len; // remember the first length-only match, keep looking
  }
  // Peeled-first-iteration candidates: only tried once no ordinary pairing
  // above was accepted. See detectPeeledPrefixBound/corroboratePeeledPrefix
  // for the full proof requirements (a dominating offset-zero access,
  // reached only once the argument is proven positive).
  for (const LoopBound &lb : acc.loopBounds) {
    ArrayBound peeled = detectPeeledPrefixBound(
        acc, lb, [&](const Value *v) { return resolveExtentOperand(v, sretOffset); });
    if (peeled.length.valid() && peeled.stride.valid())
      return peeled;
  }
  if (best.length.valid())
    return best;
  for (const Value *lv : acc.lengths) {
    ExtentOperand len = resolveExtentOperand(stripIntCasts(lv), sretOffset);
    if (len.valid())
      return {len, {}};
  }
  return {};
}

// A wrapper's own StaticAccess entries (after analyzeAccess's pure-address-
// computation pruning) are real accesses a one-hop delegated proof knows
// nothing about -- but each one is still sound to accept into the SAME
// envelope the delegate proves, PROVIDED it touches only offset 0 (always
// inside ANY [0,(length-1)*stride] envelope) AND is reachable only once
// `rawLength` -- the SAME wrapper-side value feeding the delegate's own
// accepted length operand -- is already proven positive by a dominating
// guard. Without that guard, a zero-length call could take this exact path
// while still really touching the one element it reads, a case the
// runtime's own n<=0-sizes-to-0 rule would then under-cover. A real,
// common OpenBLAS idiom: interface/axpy.c's `if (incx==0 && incy==0) {
// *y += n*alpha*(*x); return; }` is mutually exclusive with (and, once
// n>=1 is known, exactly subsumed by) the general AXPYU_K delegation a few
// lines later. Mirrors corroboratePeeledPrefix's reasoning, generalized
// from "reaches a loop header" to "reaches an arbitrary instruction" via
// dominatorProvesPositiveBeforeInst.
bool wrapperAccountsForOwnAccesses(const Access &acc, const Value *rawLength) {
  if (acc.staticAccesses.empty())
    return true;
  if (!rawLength)
    return false;
  const Instruction *anchor = nullptr;
  for (const StaticAccess &sa : acc.staticAccesses)
    if (sa.inst) { anchor = sa.inst; break; }
  if (!anchor)
    return false;
  Function *F = const_cast<Function *>(anchor->getFunction());
  DominatorTree DT(*F);
  for (const StaticAccess &sa : acc.staticAccesses) {
    if (!sa.offset || *sa.offset != 0)
      return false;
    if (!dominatorProvesPositiveBeforeInst(rawLength, sa.inst, DT))
      return false;
  }
  return true;
}

// One-hop interprocedural array-bound detection. `acc` is the CALLER-side
// access summary for a pointer argument that has no local evidence of its
// own (acc.lengths/acc.loopBounds both empty) -- e.g. a public wrapper whose
// own compiled body never indexes its array arguments at all, immediately
// delegating to an internal kernel compiled as a separate translation unit.
// If `acc` recorded a direct call to a callee whose BODY
// is available -- either defined right in the caller's own module (used
// directly, unambiguous by construction), or, for a genuinely cross-module
// reference, the ONE unambiguous externally-linked definition found across
// every resident module in `calleeIndex` (built once, up front; see
// CalleeIndex in Infer.h) -- re-run the same access analysis on the
// callee's OWN corresponding parameter. If the callee's own dataflow shows
// that parameter is itself bounded by (and, separately, walked with a step
// derived from) other parameters of the callee's own, map those
// callee-parameter indices back through the SAME call site's actual
// arguments to the CALLER's own DWARF argument list -- that's what gets
// reported, in the caller's own terms.
//
// Fixed at exactly one hop: deep enough for the confirmed public-wrapper/
// internal-kernel shape, shallow enough to bound cost and rule out cycles
// without a visited-set.
ArrayBound detectDelegatedArrayBound(const Access &acc, unsigned callerSretOffset,
                                     const CalleeIndex &calleeIndex,
                                     std::string *calleeNameOut) {
  ArrayBound best;
  std::string bestName;
  // A one-hop delegated bound only explains what happens INSIDE the
  // callee it's derived from (calleeAcc below) -- it says nothing about
  // what the WRAPPER itself does to the same pointer. A bulk-memory
  // operation, an unbounded C-string library call (strlen, strcpy, ... --
  // see noOtherAccesses' own comment on Access::stringOp), or a SECOND
  // unaccounted call on the same pointer, are all invisible to the
  // callee's own proof and must block acceptance outright, the same way
  // noOtherAccesses blocks an unexplained access inside the callee --
  // computed once, not per DelegateCall, since all three depend only on
  // the wrapper's own Access, the SAME for every candidate callee tried
  // below. A direct access in the wrapper itself (e.g. `worker(n, x);
  // return x[n];`) is handled more precisely, per accepted candidate, by
  // wrapperAccountsForOwnAccesses below -- an offset-0 access dominated by
  // the SAME positivity guard the delegate's own length is keyed to is
  // sound to accept; anything else is not.
  bool wrapperBaseClean = acc.lengths.empty() && !acc.stringOp &&
                          acc.delegateCalls.size() == 1;
  for (const DelegateCall &dc : acc.delegateCalls) {
    const Function *callee = dc.cb->getCalledFunction();
    if (!callee)
      continue; // indirect call -- callee statically unknown, cannot follow
    const Function *calleeDef = nullptr;
    if (!callee->isDeclaration()) {
      // Defined right here, in the caller's own module -- the call site
      // already references the exact body directly; no name lookup (and no
      // possibility of ambiguity) needed.
      calleeDef = callee;
    } else if (callee->hasName()) {
      // A genuinely cross-module reference (only declared in this TU).
      // calleeIndex maps a name to nullptr when more than one resident
      // module defines an externally-linked function with that name --
      // refuse to guess which one this declaration actually resolves to,
      // rather than silently picking one (see CalleeIndex in Infer.h).
      auto it = calleeIndex.find(callee->getName());
      if (it == calleeIndex.end() || !it->second)
        continue;
      calleeDef = it->second;
    } else {
      continue;
    }
    if (dc.argIdx >= calleeDef->arg_size())
      continue; // shouldn't happen (argIdx came from this exact call), but be safe
    unsigned calleeSretOffset = computeSretOffset(*calleeDef);
    Access calleeAcc = analyzeAccess(calleeDef->getArg(dc.argIdx));

    // Map a Value found INSIDE the callee back to the raw Value the CALLER
    // passed for it at THIS call site -- the wrapper-side value, before
    // resolveExtentOperand collapses it to a DWARF argument operand.
    // Exposed separately from mapBack (below) so
    // wrapperAccountsForOwnAccesses can reason about the SAME wrapper-side
    // value a length resolved from, in the wrapper's own IR terms.
    auto rawAtCallSite = [&](const Value *v) -> const Value * {
      const Value *s = stripIntCasts(v);
      int calleeParamIdx = dwarfIndexOfMaybeLoaded(s, calleeSretOffset);
      if (calleeParamIdx < 0 || (unsigned)calleeParamIdx >= dc.cb->arg_size())
        return nullptr;
      return stripIntCasts(dc.cb->getArgOperand(calleeParamIdx));
    };
    // Map a Value found INSIDE the callee (a length OR a stride -- same
    // mapping either way) back to one of the CALLER's own DWARF argument
    // operands, via this same call site's actual arguments.
    auto mapBack = [&](const Value *v) -> ExtentOperand {
      const Value *atCallSite = rawAtCallSite(v);
      if (!atCallSite)
        return {};
      // resolveExtentOperand, not a plain index lookup: the CALLER's own
      // value passed into this slot might itself be a local loaded from one
      // of the caller's OWN pointer arguments -- the Fortran-by-reference
      // idiom (a Fortran entry point's `n` is `load i32, ptr %N`, where %N
      // -- not `n` -- is its actual argument; its CBLAS sibling takes `n`
      // directly, so only the Fortran entry point's call needs the extra
      // hop, and the runtime needs to know it's there).
      return resolveExtentOperand(atCallSite, callerSretOffset);
    };
    auto checkOne = [&](const Value *v) -> ExtentOperand {
      if (ExtentOperand r = mapBack(v); r.valid())
        return r;
      // (*lenptr) idiom inside the callee, mirroring the same check the
      // top-level acc.lengths loop does for the function under direct analysis.
      if (auto *ld = dyn_cast<LoadInst>(stripIntCasts(v)))
        return mapBack(stripPtr(ld->getPointerOperand()));
      return {};
    };

    // Loop-bound evidence (the common BLAS-style delegation case): try to
    // resolve BOTH length and stride back to the caller for EVERY loop
    // bound found, not just the first whose length resolves -- prefer a
    // PROVEN fully-resolved (length+stride) match over a length-only one,
    // for the same reason detectDirectArrayBound does (see its own
    // comment).
    for (const LoopBound &lb : calleeAcc.loopBounds) {
      ExtentOperand len = checkOne(lb.length);
      if (!len.valid())
        continue;
      ExtentOperand stride = lb.stride ? checkOne(lb.stride)
          : lb.constStride ? constantExtentOperand(*lb.constStride)
          : ExtentOperand{};
      // See the identical check in detectDirectArrayBound: a parameter can
      // never legitimately be its own stride, so this pairing is discarded
      // outright rather than kept as a length-only fallback.
      if (stride.valid() && stride.argIndex == len.argIndex)
        continue;
      if (lb.lengthProven && stride.valid()) {
        // Same "does this pairing account for EVERYTHING the pointer
        // touches" requirement as detectDirectArrayBound -- checked in the
        // CALLEE's own terms (calleeAcc.staticAccesses) for what happens
        // INSIDE it, wrapperBaseClean (above) for bulk-memory/second-call
        // hazards in the WRAPPER, and wrapperAccountsForOwnAccesses for any
        // direct access the WRAPPER itself makes.
        SmallPtrSet<const Instruction *, 4> explained;
        collectAgreeingGeps(calleeAcc, lb, explained);
        if (wrapperBaseClean && noOtherAccesses(calleeAcc, explained) &&
            wrapperAccountsForOwnAccesses(acc, rawAtCallSite(lb.length))) {
          if (calleeNameOut) *calleeNameOut = callee->getName().str();
          return {len, stride}; // proven and fully resolved -- stop here
        }
        continue;
      }
      if (!best.length.valid()) {
        best.length = len;
        bestName = callee->getName().str();
      }
    }
    // Peeled-first-iteration candidates inside the callee -- same proof as
    // detectDirectArrayBound's own pass, resolved back to the CALLER's
    // terms via this call site's checkOne instead of resolveExtentOperand.
    // Same wrapperBaseClean + wrapperAccountsForOwnAccesses gating as the
    // ordinary case above.
    if (wrapperBaseClean) {
      for (const LoopBound &lb : calleeAcc.loopBounds) {
        if (!lb.peeledLengthArg)
          continue;
        if (!wrapperAccountsForOwnAccesses(acc, rawAtCallSite(lb.peeledLengthArg)))
          continue;
        ArrayBound peeled = detectPeeledPrefixBound(calleeAcc, lb, checkOne);
        if (peeled.length.valid() && peeled.stride.valid()) {
          if (calleeNameOut) *calleeNameOut = callee->getName().str();
          return peeled;
        }
      }
    }
    // Byte-count evidence (memcpy-family call) inside the callee -- a valid
    // "array-shaped" signal on its own, but carries no stride concept, so
    // it can never beat an already-found fully-resolved loop-bound match;
    // only worth remembering as a length-only fallback.
    if (!best.length.valid())
      for (const Value *lv : calleeAcc.lengths)
        if (ExtentOperand r = checkOne(lv); r.valid()) {
          best.length = r;
          bestName = callee->getName().str();
          break;
        }
  }
  if (best.length.valid() && calleeNameOut)
    *calleeNameOut = bestName;
  return best;
}

// Annotate the fields of a struct/union pointee, best-effort. Returns whether the
// struct is fully *resolvable* (every field maps to a concrete action). We lack
// caller-side per-field access analysis, so: mark every field touched; size a
// pointer field from a size-like sibling FIELD only on a tight (buf,len) signal
// (exactly one pointer + one size_t-ish scalar, e.g. toy_buffer{data,len}); a
// function-pointer / truncated / unresolvable-nested field makes the struct
// unresolvable (caller decides shallow vs force_local).
bool annotateComposite(TreeNode *s, FunctionTrees &ft, size_t topArg) {
  // Detect the tight (buf,len) shape.
  int nPtr = 0, nSizeScalar = 0, sizeField = -1, sizeRank = 0;
  for (size_t g = 0; g < s->children.size(); ++g) {
    TreeNode *c = s->children[g].get();
    if (c->kind == NodeKind::Pointer) nPtr++;
    else if (c->kind == NodeKind::Scalar) {
      int rk = sizeyRank(c->typeName);
      if (rk >= 2) { nSizeScalar++; if (rk > sizeRank) { sizeRank = rk; sizeField = (int)g; } }
    }
  }
  bool tightPair = (nPtr == 1 && nSizeScalar == 1);

  bool resolvable = true;
  for (size_t f = 0; f < s->children.size(); ++f) {
    TreeNode *fld = s->children[f].get();
    fld->touched = true;

    if (fld->kind == NodeKind::Struct || fld->kind == NodeKind::Union) {
      if (!annotateComposite(fld, ft, topArg)) resolvable = false;
      continue;
    }
    if (fld->kind == NodeKind::Scalar || fld->kind == NodeKind::Array)
      continue; // plain data — fine
    if (fld->kind != NodeKind::Pointer) { // Unknown: fn ptr / depth-cut / cyclic
      resolvable = false;
      const char *why = fld->depthTruncated
                            ? "depth-truncated (deep/cyclic struct)"
                            : (fld->typeName == "<func>" ? "function pointer"
                                                         : "unresolved");
      ft.warnings.push_back("arg" + std::to_string(topArg) + " field '" +
          fld->fieldName + "': " + why + " — not marshalable");
      continue;
    }

    // --- a pointer field ---
    TreeNode *pe = fld->children.empty() ? nullptr : fld->children[0].get();
    fld->dir = Dir::In; // nested buffers default to IN (best-effort)

    if (fld->pointeeOpaque || !pe) {            // void* field -> handle
      fld->isHandle = true; fld->handleClass = "void";
    } else if (isKnownOpaqueStruct(pe)) { // FILE*/DIR*/incomplete-composite field -> handle
      fld->isHandle = true; fld->handleClass = canonicalHandleClass(pe->typeName);
    } else if (pe->isComposite()) {
      if (pe->sizeBytes > 0 && !pe->depthTruncated &&
          annotateComposite(pe, ft, topArg)) {
        fld->sizeKind = SizeKind::Const; fld->constSize = pe->sizeBytes;
      } else {
        resolvable = false; // nested struct we can't fully resolve
        ft.warnings.push_back("arg" + std::to_string(topArg) + " field '" +
            fld->fieldName + "': nested struct '" + pe->typeName +
            "' not resolvable — not marshalable");
      }
    } else if (pe->kind == NodeKind::Pointer) {       // a T** field
      TreeNode *elem = pe->children.empty() ? nullptr : pe->children[0].get();
      if (elem && elem->kind == NodeKind::Scalar && elem->sizeBytes == 1) {
        // char** field = NULL-terminated array of C-strings (alias lists:
        // group.gr_mem, hostent.h_aliases, *ent.X_aliases). Same shape as a
        // top-level argv: deep-copy each element string, NULL-terminate.
        fld->sizeKind = SizeKind::PtrArray;
        pe->sizeKind = SizeKind::Cstr;
        pe->dir = Dir::In;
        ft.warnings.push_back("arg" + std::to_string(topArg) + " field '" +
            fld->fieldName + "': char** treated as NULL-terminated string array; "
            "verify elements are C-strings (not fixed-size binary)");
      } else {
        resolvable = false; // T** to non-string (linked list / opaque)
        ft.warnings.push_back("arg" + std::to_string(topArg) + " field '" +
            fld->fieldName + "': pointer-to-pointer (non-string) — not marshalable");
      }
    } else if (pe->kind == NodeKind::Unknown) {       // ptr to fn/truncated
      resolvable = false;
      ft.warnings.push_back("arg" + std::to_string(topArg) + " field '" +
          fld->fieldName + "': pointer to fn/unresolved — not marshalable");
    } else {                                    // ptr to scalar
      uint64_t elem = pe->sizeBytes ? pe->sizeBytes : 1;
      if (tightPair) { fld->sizeKind = SizeKind::FromArg; fld->sizeArgIndex = sizeField; }
      else if (elem == 1) fld->sizeKind = SizeKind::Cstr; // char* -> assume string
      else { fld->sizeKind = SizeKind::Const; fld->constSize = elem; } // singleton
    }
  }
  return resolvable;
}

Dir directionFrom(const Argument *A, const Access &acc) {
  bool read = acc.read, written = acc.written;
  // Refine with optimizer-proven attributes.
  if (A->onlyReadsMemory()) written = false;
  if (A->hasAttribute(Attribute::WriteOnly)) read = false;
  if (read && written) return Dir::InOut;
  if (read) return Dir::In;
  if (written) return Dir::Out;
  return Dir::Unknown; // never dereferenced here (handle candidate / unused)
}

// =============================================================================
// Variadic argument handling (design: local-notes/active/
// design-variadic-argument-handling.md). None of this is keyed on a function
// name — every detector below is a pure IR/dataflow shape test rooted at a
// "walk root": either the alloca that `llvm.va_start` writes into (for a
// variadic function), or a named parameter whose DWARF type is `va_list`
// (vprintf-shaped, not itself variadic). Sub-cases, matched in this order:
//   Z (va_copy present)      -> force_local, unclassifiable
//   D (escapes to a call)    -> force_local, type sequence not recoverable
//   A (never read, no escape)-> vacuous, nothing to marshal
//   C (sentinel-terminated loop over a pointer-shaped read) -> ptr_array
//   B (fixed, bounded reads) -> synthetic trailing args, fed through the
//                               existing per-argument machinery
//   otherwise                -> Z, force_local with a specific reason
// =============================================================================

bool isVaBookkeepingIntrinsic(Intrinsic::ID id) {
  switch (id) {
  case Intrinsic::vastart:
  case Intrinsic::vaend:
  case Intrinsic::lifetime_start:
  case Intrinsic::lifetime_end:
  case Intrinsic::dbg_declare:
  case Intrinsic::dbg_value:
  case Intrinsic::dbg_assign:
    return true;
  default:
    return false;
  }
}

// Find the alloca `llvm.va_start` writes the initial cursor into, if any.
const AllocaInst *findVaStartAlloca(const Function &F) {
  for (const BasicBlock &bb : F)
    for (const Instruction &I : bb)
      if (auto *II = dyn_cast<IntrinsicInst>(&I))
        if (II->getIntrinsicID() == Intrinsic::vastart)
          if (auto *AI = dyn_cast<AllocaInst>(
                  II->getArgOperand(0)->stripPointerCasts()))
            return AI;
  return nullptr;
}

// Does `llvm.va_copy` touch `root` (as either its dest or src operand)? A
// second, independently-walked va_list is exactly the Sub-case Z pattern the
// design calls out — never silently folded into A/B/C/D.
bool hasVaCopyTouching(const Value *root) {
  for (const User *U : root->users())
    if (auto *cb = dyn_cast<CallBase>(U))
      if (auto *II = dyn_cast<IntrinsicInst>(cb))
        if (II->getIntrinsicID() == Intrinsic::vacopy)
          return true;
  return false;
}

// Does "the current va_list value" rooted at `v` escape whole into a real call
// (i.e. get passed as an actual argument), without first being read through a
// recognized va_arg cursor-advance? Starting `v` at the alloca itself catches
// both "&ap forwarded to a callee expecting va_list*" and, by following loads
// transitively, "ap forwarded by value" (wasm32 va_list is a plain pointer, so
// a load of the alloca IS the va_list value). Starting `v` at a va_list-typed
// Argument directly catches the vprintf-shaped case with no alloca at all.
// va_start/va_end/lifetime/dbg are bookkeeping, not escapes; va_copy is
// deliberately NOT excluded here — callers check hasVaCopyTouching separately.
bool cursorEscapesToCall(const Value *v) {
  SmallPtrSet<const Value *, 16> seen;
  SmallVector<const Value *, 16> work{v};
  while (!work.empty()) {
    const Value *cur = work.pop_back_val();
    if (!seen.insert(cur).second) continue;
    for (const User *U : cur->users()) {
      if (auto *cb = dyn_cast<CallBase>(U)) {
        if (auto *II = dyn_cast<IntrinsicInst>(cb))
          if (isVaBookkeepingIntrinsic(II->getIntrinsicID()) ||
              II->getIntrinsicID() == Intrinsic::vacopy)
            continue;
        for (const Use &use : cb->args())
          if (use.get() == cur) return true;
        continue;
      }
      if (auto *ld = dyn_cast<LoadInst>(U)) {
        if (ld->getPointerOperand() == cur) work.push_back(ld);
        continue;
      }
      if (isa<BitCastInst>(U)) work.push_back(U);
    }
  }
  return false;
}

// One recovered variadic-tail access: the LoadInst that reads the actual value
// (not the cursor-advance machinery), and its concrete LLVM type.
struct VarargRead {
  const LoadInst *valueLoad;
  Type *type;
};

// A cursor-snapshot VALUE, generalized beyond "a bare load of root": either
// (a) a LoadInst reading root directly (the plain, unlooped shape), or (b) a
// PHINode that traces back to a load of root via tracesToLoadOfSlot (already
// built for the strsep cursor detector, reused here). (b) is what the
// optimizer produces for a LOOPED va_arg walk at -O1+: the cursor gets
// promoted out of the alloca into a phi merging the initial load with the
// advanced value from the previous iteration, instead of re-loading the
// alloca every iteration — confirmed by inspecting actual -O1 wasm32 codegen
// for a sentinel loop over va_arg. Both are additionally required to feed a
// GEP whose result is stored back into `root` (the advance step), so this
// stays a precise, conjunctive signal rather than matching arbitrary phis.
bool isCursorCandidate(const Value *cand, const Value *root) {
  if (auto *ld = dyn_cast<LoadInst>(cand))
    return ld->getPointerOperand() == root;
  if (isa<PHINode>(cand)) {
    SmallPtrSet<const Value *, 16> seen;
    return tracesToLoadOfSlot(cand, root, seen);
  }
  return false;
}

// Find every cursor-snapshot value for `root` that is genuinely advanced (fed
// through a GEP whose result is stored back into `root` — the advance step
// clang/LLVM emit for wasm32 va_arg, whether or not the optimizer has since
// promoted the read side into a phi), then collect every direct load of each
// confirmed snapshot, as the recovered value sequence. Returns false if `root`
// has no recognizable cursor usage at all (caller then distinguishes vacuous
// vs unclassified separately).
bool collectVarargReads(const Function &F, const Value *root,
                        SmallVectorImpl<VarargRead> &out) {
  SmallPtrSet<const Value *, 8> candidates;
  for (const User *U : root->users())
    if (auto *ld = dyn_cast<LoadInst>(U); ld && ld->getPointerOperand() == root)
      candidates.insert(ld);
  for (const BasicBlock &bb : F)
    for (const Instruction &I : bb)
      if (isa<PHINode>(&I) && isCursorCandidate(&I, root))
        candidates.insert(&I);

  SmallPtrSet<const Value *, 8> confirmed;
  for (const Value *cand : candidates) {
    for (const User *U2 : cand->users())
      if (auto *gep = dyn_cast<GetElementPtrInst>(U2))
        for (const User *U3 : gep->users())
          if (auto *st = dyn_cast<StoreInst>(U3))
            if (st->getPointerOperand() == root && st->getValueOperand() == gep)
              confirmed.insert(cand);
  }
  if (confirmed.empty()) return false;

  for (const BasicBlock &bb : F)
    for (const Instruction &I : bb)
      if (auto *ld = dyn_cast<LoadInst>(&I))
        if (confirmed.count(ld->getPointerOperand()))
          out.push_back({ld, ld->getType()});
  return true;
}

// Is `bb` part of a control-flow cycle (has a path back to itself)? Function
// CFGs here are small; a bounded forward-reachability search is sufficient —
// no need for full LoopInfo/DominatorTree machinery.
bool blockInCycle(const BasicBlock *bb) {
  SmallPtrSet<const BasicBlock *, 16> visited;
  SmallVector<const BasicBlock *, 16> work(succ_begin(bb), succ_end(bb));
  while (!work.empty()) {
    const BasicBlock *cur = work.pop_back_val();
    if (cur == bb) return true;
    if (!visited.insert(cur).second) continue;
    for (const BasicBlock *s : successors(cur)) work.push_back(s);
  }
  return false;
}

// Does `val` feed an `icmp eq/ne` against a null pointer / zero-integer
// constant whose result drives a conditional branch (a sentinel-terminated
// loop's exit test)?
bool feedsNullSentinelBranch(const Value *val) {
  for (const User *U : val->users()) {
    if (isa<BitCastInst>(U)) { if (feedsNullSentinelBranch(U)) return true; continue; }
    auto *cmp = dyn_cast<ICmpInst>(U);
    if (!cmp || !(cmp->getPredicate() == ICmpInst::ICMP_EQ ||
                  cmp->getPredicate() == ICmpInst::ICMP_NE))
      continue;
    const Value *other =
        (cmp->getOperand(0) == val) ? cmp->getOperand(1) : cmp->getOperand(0);
    bool isNullLike = isa<ConstantPointerNull>(other) ||
                      (isa<ConstantInt>(other) && cast<ConstantInt>(other)->isZero());
    if (!isNullLike) continue;
    for (const User *U2 : cmp->users())
      if (isa<BranchInst>(U2)) return true;
  }
  return false;
}

// Does a named parameter's RAW (unstripped) DWARF type resolve to va_list?
// (`buildTreeFromDIType`'s TreeNode::typeName has already stripped typedefs by
// the time we'd see it, so this walks the DISubprogram's own type array
// directly.) Returns the 1-based DWARF parameter index, or -1.
int findVaListParam(const DISubprogram *sp) {
  if (!sp) return -1;
  auto *subTy = sp->getType();
  if (!subTy) return -1;
  DITypeRefArray types = subTy->getTypeArray();
  for (unsigned i = 1; i < types.size(); ++i) {
    const DIType *ty = types[i];
    while (auto *dt = dyn_cast_or_null<DIDerivedType>(ty)) {
      if (dt->getTag() == dwarf::DW_TAG_typedef) {
        StringRef n = dt->getName();
        if (n == "va_list" || n == "__builtin_va_list" || n == "__gnuc_va_list")
          return (int)i - 1;
        ty = dt->getBaseType();
        continue;
      }
      break;
    }
  }
  return -1;
}

// A single StoreInst spilling `arg` directly into a stack slot, if the
// function makes one (only relevant for a va_list-typed PARAMETER root: it
// starts as a plain SSA value, not an alloca-backed cursor, unless the body
// itself needs to mutate it locally).
const AllocaInst *findSpillAlloca(const Argument *arg) {
  for (const User *U : arg->users())
    if (auto *st = dyn_cast<StoreInst>(U))
      if (st->getValueOperand() == arg)
        if (auto *AI = dyn_cast<AllocaInst>(st->getPointerOperand()))
          return AI;
  return nullptr;
}

// Build a synthetic TreeNode for a recovered variadic scalar/pointer read.
// There is no DWARF type here (that's the whole reason this is hard) — the
// recovered LLVM type is the entry-level NodeKind only, exactly as
// buildTreeFromDIType's DWARF-derived NodeKind is for a named argument; a
// recovered pointer still needs the existing per-argument use-def
// classification to go further, which the caller runs afterward if it can.
std::unique_ptr<TreeNode> synthNodeFromType(Type *ty) {
  auto node = std::make_unique<TreeNode>();
  if (ty->isPointerTy()) {
    node->kind = NodeKind::Pointer;
    node->typeName = "<variadic ptr>";
    node->sizeBytes = 4; // wasm32
    node->pointeeOpaque = true; // no DWARF pointee for a variadic slot, ever —
    // leave children empty (matches how buildTreeFromDIType represents a
    // named void* argument): treeHasUnmappable treats an Unknown-kind CHILD
    // as unmappable regardless of the parent's own resolved sizeKind, so an
    // opaque pointee must be represented by pointeeOpaque=true + no child,
    // never a synthesized Unknown child node.
  } else {
    node->kind = NodeKind::Scalar;
    node->typeName = "<variadic scalar>";
    node->sizeBytes = ty->getPrimitiveSizeInBits() / 8;
  }
  return node;
}

// Does `ptrVal` (a recovered opaque variadic pointer) get directly
// load/store-dereferenced within this function's own body? If so, that's real
// use-def evidence of pointer use (B' unlock condition #2) — recover the
// accessed width and a direction from it. Only a DIRECT dereference counts
// (not "forwarded to another call that might dereference it") — anything less
// direct is exactly the unproven case the conservative B' default exists for.
bool provenDirectDereference(const Value *ptrVal, uint64_t &widthOut, Dir &dirOut) {
  bool read = false, written = false;
  uint64_t width = 0;
  for (const User *U : ptrVal->users()) {
    if (auto *ld = dyn_cast<LoadInst>(U)) {
      if (ld->getPointerOperand() == ptrVal) {
        read = true;
        width = std::max(width, ld->getType()->getPrimitiveSizeInBits() / 8);
      }
    } else if (auto *st = dyn_cast<StoreInst>(U)) {
      if (st->getPointerOperand() == ptrVal) {
        written = true;
        width = std::max(width,
            st->getValueOperand()->getType()->getPrimitiveSizeInBits() / 8);
      }
    }
  }
  if (!read && !written) return false;
  widthOut = width ? width : 4;
  dirOut = (read && written) ? Dir::InOut : (written ? Dir::Out : Dir::In);
  return true;
}

// Classify one walk root (an alloca cursor, or a plain va_list-typed value)
// and mutate `ft` accordingly: append synthetic B-classified trailing args,
// or set forceLocal with a specific warning for D/Z, or do nothing for A.
// `rootForCollection` is what collectVarargReads walks (an alloca); `rootForEscape`
// is what cursorEscapesToCall/hasVaCopyTouching check (may be the same alloca,
// or a bare Argument for an unspilled va_list parameter).
void classifyVarargRoot(FunctionTrees &ft, const Function &F,
                        const Value *rootForEscape,
                        const AllocaInst *rootForCollection) {
  ft.isVariadic = true;

  if (hasVaCopyTouching(rootForEscape) ||
      (rootForCollection && hasVaCopyTouching(rootForCollection))) {
    ft.forceLocal = true;
    ft.warnings.push_back(
        "variadic: va_copy present (independently-walked va_list) — force_local");
    return;
  }
  if (cursorEscapesToCall(rootForEscape)) {
    ft.forceLocal = true;
    ft.warnings.push_back(
        "variadic: va_list escapes to another call before being read; "
        "type sequence not statically recoverable — force_local");
    return;
  }

  if (!rootForCollection) {
    // A va_list-typed PARAMETER with no spill-to-alloca and no escape: we have
    // no recognized way to see how it's used locally. Real confirmed examples
    // of this root type (vprintf/vfprintf/...) are all escape-shaped and
    // already handled above; anything else here is genuinely unclassified.
    ft.forceLocal = true;
    ft.warnings.push_back(
        "variadic: va_list parameter with unrecognized usage pattern — force_local");
    return;
  }

  SmallVector<VarargRead, 8> reads;
  bool sawCursor = collectVarargReads(F, rootForCollection, reads);
  if (!sawCursor) {
    // No recognized cursor-advance pattern, and (checked above) it doesn't
    // escape either. If the alloca has no other real uses beyond va_start/
    // va_end/lifetime/dbg, it is genuinely vacuous (Sub-case A). Any other
    // usage we don't recognize falls to Z, not a silent A.
    for (const User *U : rootForCollection->users()) {
      auto *cb = dyn_cast<CallBase>(U);
      bool bookkeeping = cb && dyn_cast<IntrinsicInst>(cb) &&
          isVaBookkeepingIntrinsic(cast<IntrinsicInst>(cb)->getIntrinsicID());
      if (!bookkeeping) {
        ft.forceLocal = true;
        ft.warnings.push_back(
            "variadic: va_arg walk shape not recognized (unmatched cursor usage) "
            "— force_local");
        return;
      }
    }
    ft.warnings.push_back("variadic: tail never read — no marshalling needed");
    return;
  }

  // Partition into a loop-shaped group (Sub-case C) and a fixed group
  // (Sub-case B), by whether each recovered read's block is in a CFG cycle.
  SmallVector<VarargRead, 8> loopGroup, fixedGroup;
  for (const VarargRead &r : reads)
    (blockInCycle(r.valueLoad->getParent()) ? loopGroup : fixedGroup)
        .push_back(r);

  if (!loopGroup.empty()) {
    if (!fixedGroup.empty()) {
      // A loop-shaped group PLUS additional fixed reads (execle's trailing
      // envp after the argv loop) needs byte-offset-aware composition this
      // pass doesn't implement yet — conservative Z rather than guessing.
      ft.forceLocal = true;
      ft.warnings.push_back(
          "variadic: sentinel loop plus additional fixed reads (composed shape) "
          "— force_local");
      return;
    }
    // Every loop-group read must be pointer-typed (that's the only shape the
    // schema has a NULL-terminated-array representation for) and every read
    // must prove its own sentinel-branch termination.
    bool allPtr = std::all_of(loopGroup.begin(), loopGroup.end(),
        [](const VarargRead &r) { return r.type->isPointerTy(); });
    bool allSentinel = std::all_of(loopGroup.begin(), loopGroup.end(),
        [](const VarargRead &r) { return feedsNullSentinelBranch(r.valueLoad); });
    if (!allPtr || !allSentinel) {
      ft.forceLocal = true;
      ft.warnings.push_back(
          "variadic: loop-shaped va_arg read is not a proven pointer sentinel "
          "loop — force_local");
      return;
    }
    auto node = std::make_unique<TreeNode>();
    node->kind = NodeKind::Pointer;
    node->typeName = "<variadic ptr_array>";
    node->sizeBytes = 4;
    node->dir = Dir::In;
    node->sizeKind = SizeKind::PtrArray;
    auto elem = std::make_unique<TreeNode>();
    elem->kind = NodeKind::Pointer;
    elem->typeName = "<variadic ptr_array element>";
    elem->sizeBytes = 4;
    elem->dir = Dir::In;
    elem->sizeKind = SizeKind::Cstr;
    auto elemPointee = std::make_unique<TreeNode>();
    elemPointee->kind = NodeKind::Scalar;
    elemPointee->typeName = "char";
    elemPointee->sizeBytes = 1;
    elem->children.push_back(std::move(elemPointee));
    node->children.push_back(std::move(elem));
    ft.params.push_back(std::move(node));
    ft.warnings.push_back(
        "variadic: sentinel-terminated loop over pointer reads — treated as a "
        "NULL-terminated array of C-string pointers (ptr_array)");
    return;
  }

  // Sub-case B: every recovered read is a fixed, unconditional (or at most
  // named-argument-gated) access — no loop involved at all.
  for (const VarargRead &r : fixedGroup) {
    auto node = synthNodeFromType(r.type);
    if (node->kind == NodeKind::Scalar) {
      ft.params.push_back(std::move(node));
      continue;
    }
    // A recovered POINTER read has no DWARF pointee — ever. Default: force
    // local (B'), UNLESS the function itself directly dereferences it (proven
    // use-def pointer use — one of the design's three B' unlock conditions).
    uint64_t width; Dir dir;
    if (provenDirectDereference(r.valueLoad, width, dir)) {
      node->dir = dir;
      node->sizeKind = SizeKind::Const;
      node->constSize = width;
      ft.params.push_back(std::move(node));
      ft.warnings.push_back(
          "variadic: opaque pointer slot proven dereferenced in-body ("
          + std::to_string(width) + " bytes) — marshalled");
    } else {
      // Left as sizeKind=NA/Pointer: the existing per-argument safety net
      // (treeHasUnmappable) already force_locals any unresolved pointer node,
      // so this is the natural, correct default without extra plumbing —
      // just make the reason explicit rather than relying on the generic
      // catch-all message.
      ft.params.push_back(std::move(node));
      ft.warnings.push_back(
          "variadic: opaque pointer slot, meaning gated by an earlier value "
          "with no proven in-body dereference — force_local (B', no unlock "
          "condition met)");
    }
  }
}

// Entry point: find every walk root on `F` (the va_start alloca if variadic,
// and/or any va_list-typed parameter) and classify each. No-op if neither
// root exists.
void inferVariadic(const Function &F, FunctionTrees &ft, unsigned sretOffset) {
  bool anyRoot = false;

  if (F.isVarArg()) {
    anyRoot = true;
    const AllocaInst *alloca = findVaStartAlloca(F);
    const Value *escapeRoot = alloca ? static_cast<const Value *>(alloca) : nullptr;
    if (escapeRoot) {
      classifyVarargRoot(ft, F, escapeRoot, alloca);
    } else {
      // isVarArg but no va_start call anywhere (never touches the tail at
      // all, or an optimizer removed the whole va_list machinery as dead
      // code) -- if there's truly no va_start, there's nothing that could
      // read the tail, matching Sub-case A.
      ft.isVariadic = true;
      ft.warnings.push_back(
          "variadic: no va_start found — tail never read, no marshalling needed");
    }
  }

  if (const DISubprogram *sp = F.getSubprogram()) {
    int vaParamIdx = findVaListParam(sp);
    if (vaParamIdx >= 0) {
      unsigned irNo = (unsigned)vaParamIdx + sretOffset;
      if (irNo < F.arg_size()) {
        anyRoot = true;
        const Argument *arg = F.getArg(irNo);
        const AllocaInst *spill = findSpillAlloca(arg);
        classifyVarargRoot(ft, F, arg, spill);
      }
    }
  }

  (void)anyRoot;
}

// =============================================================================
// WebAssembly ABI lowering layer.
//
// marshal-infer analyzes .bc produced by `-emit-llvm -c` — LLVM IR PRE-backend.
// For most C types, the ABI-lowered shape (how a value actually crosses a wasm
// function-call boundary) already matches what this IR shows: a scalar stays a
// scalar, a T* stays one pointer argument. Two families of C types do NOT
// match, and need this layer to reconcile the DWARF/IR view with reality:
//
//   1. AGGREGATES too wide to pass/return directly (an ordinary large struct
//      passed by value, or C99 _Complex, which is structurally a 2-element
//      record) — clang's FRONTEND lowers these during -emit-llvm itself, so
//      the .bc already shows the real shape as LLVM attributes: `byval` on an
//      argument, `sret` on a hidden return-pointer argument
//      (`hasByValAttr()`/`hasStructRetAttr()`, both IR-visible, no guessing
//      needed).
//   2. SCALARS wider than any native wasm value type (i32/i64/f32/f64) with no
//      dedicated hardware representation — fp128 (`long double` on this
//      target) is the only known instance. The wasm32 BACKEND legalizes these
//      by (a) returning via a hidden pointer exactly like sret, and (b)
//      splitting an argument into N raw same-width parts (2x i64 for fp128,
//      confirmed via wasm-objdump: two incoming values, stored directly,
//      never loaded through any pointer — there is no address involved at
//      all). This legalization runs strictly AFTER -emit-llvm, so there is no
//      IR attribute for it — detected via a hardcoded, target-constant rule
//      keyed on the LLVM type itself (`Type::isFP128Ty()`), not inferred.
//
// Both families funnel into representations the rest of this file already
// understands: a wrapped pointer node (byval) or extra raw scalar slots
// (fp128 args, TreeNode::abiSlots — see its comment in ParamTree.h) alongside
// the existing per-argument tree, and a synthetic leading OUT-pointer argument
// (any sret-shaped return, real or fp128-hardcoded) via
// FunctionTrees::retSretArg. Neither needs ft.params's own length/indexing to
// change — dwarfIndexOf/sretOffset elsewhere in this file, which assume one
// DWARF argument == one ft.params entry, are untouched by this layer.
// =============================================================================

// Case 2 (scalar splitting): an fp128 argument is pure raw-bits passthrough,
// never an address — dir/sizeKind stay meaningless, nothing to translate.
void lowerFp128Arg(TreeNode *node, unsigned p, FunctionTrees &ft) {
  node->abiSlots = 2;
  ft.warnings.push_back("arg" + std::to_string(p) +
      ": long double argument (fp128) — the wasm32 backend splits this into "
      "2 raw i64 slots (low half, then high half); represented as 2 "
      "plain-scalar args, no address translation needed");
}

// Case 1 (indirect aggregate passing): wrap the existing, DWARF-correct node
// as the pointee of a synthetic pointer layer — the exact shape a real T*
// argument already produces, so nested struct fields (an ordinary byval
// struct) still flow through annotateComposite and the existing global safety
// net. dir is FORCED to In regardless of what analyzeAccess would otherwise
// infer: byval guarantees the callee's copy is private, so any writes the
// callee makes to its own copy must never be read back into the caller.
void lowerByvalArg(FunctionTrees &ft, size_t p, const Argument *A,
                   const Function &F) {
  TreeNode *node = ft.params[p].get();
  uint64_t byvalSize = node->sizeBytes;
  if (Type *bt = A->getParamByValType())
    byvalSize = F.getParent()->getDataLayout().getTypeAllocSize(bt);
  auto pointee = std::move(ft.params[p]);
  if (pointee->isComposite() && pointee->sizeBytes > 0 && !pointee->depthTruncated)
    annotateComposite(pointee.get(), ft, p); // resolves nested fields; the
        // global safety net below force_locals the whole function if any
        // field comes back unresolved, exactly as for a named struct ptr.
  auto wrapper = std::make_unique<TreeNode>();
  wrapper->kind = NodeKind::Pointer;
  wrapper->typeName = pointee->typeName;
  wrapper->sizeBytes = 4; // wasm32 pointer
  wrapper->dir = Dir::In; // byval = callee's own copy; writes never reach the caller
  wrapper->sizeKind = SizeKind::Const;
  wrapper->constSize = byvalSize;
  wrapper->children.push_back(std::move(pointee));
  ft.params[p] = std::move(wrapper);
  ft.warnings.push_back("arg" + std::to_string(p) +
      ": byval-lowered aggregate (complex/large struct) — marshalled as "
      "a const-sized IN pointer, no copy-back");
}

// Case 1 (return) + case 2 (return): a hidden-pointer-shaped return, real
// (sret attribute) or hardcoded (fp128). Builds ft.retSretArg; the caller is
// responsible for forcing ft.retKind = RetKind::Void for the fp128 case (true
// sret already naturally computes Void, since F.getReturnType() really is
// void there).
void lowerAbiReturn(const Function &F, FunctionTrees &ft, bool trueSret,
                    bool fp128Ret) {
  const DataLayout &DL = F.getParent()->getDataLayout();
  Type *rt = F.getReturnType();
  uint64_t sretSize = (trueSret && F.getArg(0)->getParamStructRetType())
      ? DL.getTypeAllocSize(F.getArg(0)->getParamStructRetType())
      : DL.getTypeAllocSize(rt);
  auto sret = std::make_unique<TreeNode>();
  sret->kind = NodeKind::Pointer;
  sret->typeName = "<sret>";
  sret->sizeBytes = 4; // wasm32 pointer
  sret->dir = Dir::Out;
  sret->sizeKind = SizeKind::Const;
  sret->constSize = sretSize;
  ft.retSretArg = std::move(sret);
  ft.warnings.push_back(trueSret
      ? "return: sret-shaped (struct/complex-by-value) — copied out via a "
        "synthetic leading pointer argument, not a return value"
      : "return: long double (fp128) is sret-lowered by the wasm32 "
        "backend, invisible at this IR level — copied out via a "
        "synthetic leading pointer argument, not a return value");
}

// The V1 interposition transport is fixed-arity: the portal (pass_fptr_to_wt)
// captures exactly LIND_RAW_ARGS_MAX=6 raw wasm-level argument/cage-id pairs
// (tests/grate-tests/lib-interpose/lind_marshal.h), and V1's
// lind_marshal_dispatch aborts the whole grate process if a spec claims more
// than that -- see the file's `if (spec->nargs > LIND_RAW_ARGS_MAX)
// _lind_marshal_abort(...)` guard. THIS is the single authoritative source of
// the "6" below; keep it in sync if that constant ever changes.
//
// A wider function is NOT unmarshalable, though -- the variable-width V2
// transport (local-notes/active/plan-variable-width-library-calls.md, Gates
// 2-5) carries any argument count: gen_v2_adapter.py generates a real,
// correctly-typed adapter and V1's own pointer/handle/copy-back safety logic
// was refactored (Gate 4) into shared, nargs-parameterized primitives both
// transports call, so a wide function's semantics are exactly as sound (or
// unsound) as a narrow one's. This function therefore only ANNOTATES the
// width now, rather than rejecting on it: gen_grate.py's OWN independent
// width gate (unmarshalable_reason's max_args, defaulting to
// LIND_RAW_ARGS_MAX) is what keeps V1 generation from picking up a function
// this wide -- only gen_v2_adapter.py's max_args=None path does.
//
// A function's true raw-slot count is NOT its DWARF/C-level argument count --
// it's post-ABI-lowering: the synthetic sret/fp128-return pointer (if any)
// counts as one slot, and any argument with abiSlots>1 (fp128, see above)
// counts as N slots, not one. Undercounting here would mean a function V1
// genuinely cannot carry goes unlabeled, gen_grate.py's own default-max_args
// check never seeing a reason to exclude it, and the FIRST real V1 call to
// it aborting the entire grate process -- taking down every other function
// sharing that grate, not just the wide one.
constexpr unsigned kMaxRawArgSlots = 6;

void annotateWideRawArgSlots(FunctionTrees &ft) {
  unsigned slots = ft.retSretArg ? 1 : 0;
  for (const auto &p : ft.params)
    slots += std::max<uint32_t>(1, p->abiSlots);
  if (slots <= kMaxRawArgSlots)
    return;
  ft.warnings.push_back(
      "function needs " + std::to_string(slots) + " raw ABI slots" +
      (ft.retSretArg ? " (including a synthetic sret/fp128-return pointer)"
                     : "") +
      ", exceeding the V1 transport's fixed " +
      std::to_string(kMaxRawArgSlots) +
      "-slot capacity (LIND_RAW_ARGS_MAX) — requires the variable-width V2 "
      "transport (plan-variable-width-library-calls.md); gen_grate.py's own "
      "width gate keeps V1 generation from picking this up");
}

// Config.h's ContractExtentOperand/dir strings are validated at load time
// (loadConfig) to already be one of exactly these values -- no further
// validation needed here, just translation into ParamTree.h's vocabulary.
ExtentOperand extentOperandFromContract(const ContractExtentOperand &c) {
  return {c.argIndex, c.source == "pointee_i32" ? ExtentSource::PointeeI32
                                                : ExtentSource::Value};
}
Dir dirFromContract(const std::string &s) {
  if (s == "out") return Dir::Out;
  if (s == "inout") return Dir::InOut;
  return Dir::In;
}

} // namespace

bool validateContractAgainstSignature(const FunctionTrees &ft,
                                      const FunctionContract &contract,
                                      std::string &err) {
  auto argDesc = [&](int idx) {
    return "'" + ft.funcName + "' arg" + std::to_string(idx);
  };
  // A "value" operand must be a plain integer scalar -- reject a pointer,
  // struct, array, or (best-effort, via the DWARF type name -- TreeNode has
  // no dedicated int/float distinction) a floating-point scalar, any of
  // which would have the runtime reinterpret the wrong wasm value-type's
  // raw bits as an element count. A "pointee_i32" operand must be a pointer
  // whose pointee is itself a plain 4-byte integer scalar (the Fortran-by-
  // reference idiom this source names).
  auto checkOperand = [&](int targetArg, const ContractExtentOperand &op,
                          const char *label) -> bool {
    if (op.argIndex < 0 || (size_t)op.argIndex >= ft.params.size()) {
      err = "config contract for " + argDesc(targetArg) + "." + label +
            ": arg_index " + std::to_string(op.argIndex) +
            " does not exist (function has " +
            std::to_string(ft.params.size()) + " parameter(s))";
      return false;
    }
    const TreeNode *t = ft.params[(size_t)op.argIndex].get();
    if (op.source == "value") {
      if (t->kind != NodeKind::Scalar || t->typeName == "float" ||
          t->typeName == "double" || t->typeName == "long double") {
        err = "config contract for " + argDesc(targetArg) + "." + label +
              ": arg" + std::to_string(op.argIndex) +
              " (source=\"value\") is not a compatible integer scalar "
              "(actual type: " + t->typeName + ")";
        return false;
      }
    } else { // "pointee_i32" -- loadConfig already restricted source to
             // exactly these two strings
      if (t->kind != NodeKind::Pointer || t->children.empty() ||
          t->children[0]->kind != NodeKind::Scalar ||
          t->children[0]->sizeBytes != 4 ||
          t->children[0]->typeName == "float") {
        err = "config contract for " + argDesc(targetArg) + "." + label +
              ": arg" + std::to_string(op.argIndex) +
              " (source=\"pointee_i32\") is not a pointer to a 32-bit "
              "integer (actual type: " + t->typeName + ")";
        return false;
      }
    }
    return true;
  };
  for (const auto &kv : contract) {
    int p = kv.first;
    const StrideVectorContract &c = kv.second;
    if (p < 0 || (size_t)p >= ft.params.size()) {
      err = "config contract for " + argDesc(p) +
            ": no such argument (function has " +
            std::to_string(ft.params.size()) + " parameter(s))";
      return false;
    }
    if (ft.params[(size_t)p]->kind != NodeKind::Pointer) {
      err = "config contract for " + argDesc(p) +
            ": target is not a pointer argument (actual type: " +
            ft.params[(size_t)p]->typeName + ")";
      return false;
    }
    if (!checkOperand(p, c.sizeOperand, "size_operand"))
      return false;
    if (!checkOperand(p, c.strideOperand, "stride_operand"))
      return false;
  }
  return true;
}

void inferFunction(const Function &F, FunctionTrees &ft,
                   const CalleeIndex &calleeIndex, const Config *config) {
  unsigned sretOffset = computeSretOffset(F);
  const FunctionContract *contract = nullptr;
  if (config) {
    auto it = config->contracts.find(ft.funcName);
    if (it != config->contracts.end())
      contract = &it->second;
  }
  int cursorArg = -1; // a char** arg whose *p walks its own buffer (strsep)

  // ---- variadic tail (if any) ----
  // Runs before the named-argument loop and appends any recovered synthetic
  // trailing args to ft.params first. This is safe: scalar synthetic nodes are
  // skipped by the loop below (kind != Pointer); pointer synthetic nodes index
  // past F.arg_size(), which the loop's own bounds check already skips,
  // leaving whatever this step decided untouched.
  inferVariadic(F, ft, sretOffset);

  // ---- per-parameter analysis ----
  for (size_t p = 0; p < ft.params.size(); ++p) {
    TreeNode *node = ft.params[p].get();
    unsigned irNo = (unsigned)p + sretOffset;
    const Argument *Airn = (irNo < F.arg_size()) ? F.getArg(irNo) : nullptr;

    // WASM ABI lowering layer (see the block comment above lowerFp128Arg) --
    // both checks below fire on the LLVM Argument itself, before this node's
    // DWARF-derived NodeKind (which knows nothing about ABI-level lowering)
    // would otherwise cause it to be silently skipped or misclassified.
    if (Airn && Airn->getType()->isFP128Ty()) {
      lowerFp128Arg(node, (unsigned)p, ft);
      continue;
    }
    if (node->kind != NodeKind::Pointer && Airn && Airn->hasByValAttr()) {
      lowerByvalArg(ft, p, Airn, F);
      continue;
    }

    if (node->kind != NodeKind::Pointer)
      continue; // scalars need no marshalling
    if (irNo >= F.arg_size())
      continue;
    const Argument *A = Airn;

    Access acc = analyzeAccess(A);
    node->dir = directionFrom(A, acc);

    // --- resolve a byte length paired with this pointer (FROM_ARG / *lenptr) ---
    int sizeArg = -1;
    bool fromPointee = false;
    for (const Value *lv : acc.lengths) {
      const Value *s = stripIntCasts(lv);
      if (int idx = dwarfIndexOf(s, sretOffset); idx >= 0) { sizeArg = idx; break; }
      if (auto *ld = dyn_cast<LoadInst>(s)) {
        const Value *pp = stripPtr(ld->getPointerOperand());
        if (int idx = dwarfIndexOf(pp, sretOffset); idx >= 0) {
          sizeArg = idx; fromPointee = true; break;
        }
      }
    }
    const TreeNode *pointee =
        node->children.empty() ? nullptr : node->children[0].get();
    bool opaque = node->pointeeOpaque || !pointee;
    bool charPointee = pointee && pointee->kind == NodeKind::Scalar &&
                       pointee->sizeBytes == 1;
    bool sizeHeuristic = false; // set when sizeArg is a guess, not from dataflow

    // (buf, *lenptr) idiom: a byte/void buffer immediately followed by a pointer
    // to a size-like scalar is sized by *that pointer — compress2/uncompress's
    // (dest, uLongf *destLen). This is FROM_ARG_POINTEE; check it BEFORE the
    // plain-scalar heuristic so `dest` is sized by *destLen, not a later scalar
    // (and `source`, followed by a scalar `sourceLen`, still gets plain FROM_ARG).
    if (sizeArg < 0 && (charPointee || opaque)) {
      size_t q = p + 1;
      if (q < ft.params.size() && ft.params[q]->kind == NodeKind::Pointer &&
          !ft.params[q]->children.empty()) {
        const TreeNode *lenPointee = ft.params[q]->children[0].get();
        if (lenPointee->kind == NodeKind::Scalar &&
            sizeyRank(lenPointee->typeName) >= 2) {
          sizeArg = (int)q; fromPointee = true; sizeHeuristic = true;
        }
      }
    }

    // Heuristic fallback: in the (buf,len) idiom the length is the size-like
    // scalar that *follows* the buffer — e.g. adler32(seed, buf, len) is sized by
    // `len`, NOT the preceding `seed` (which may outrank it by type). So prefer
    // the nearest qualifying scalar after the pointer, then fall back to the
    // nearest before. A char* needs a strong (size_t-shaped, rank>=2) companion
    // so a plain int (e.g. strchr's char) doesn't count and it falls through to
    // CSTR; non-char buffers accept rank>=1.
    if (sizeArg < 0) {
      // Highest-rank size-like scalar in one half (after: q>p, before: q<p).
      auto bestSizey = [&](bool after, int minR) -> int {
        int best = -1, bestRank = 0;
        for (size_t q = 0; q < ft.params.size(); ++q) {
          if (q == p || ft.params[q]->kind != NodeKind::Scalar) continue;
          if (after ? (q < p) : (q > p)) continue;
          int rk = sizeyRank(ft.params[q]->typeName);
          if (rk >= minR && rk > bestRank) { bestRank = rk; best = (int)q; }
        }
        return best;
      };
      // A genuine size_t-shaped scalar (rank>=2) is the strong length signal;
      // prefer the one AFTER the buffer (the (buf,len) idiom: adler32(seed,buf,
      // len)->len), then before. Strong type dominates position, so memchr(s,
      // int c, size_t n) sizes by n (rank 3), not the nearer c (rank 1). Only
      // non-char buffers fall back to a weak (plain int) length.
      int best = bestSizey(/*after=*/true, 2);
      if (best < 0) best = bestSizey(/*after=*/false, 2);
      if (best < 0 && !charPointee) best = bestSizey(/*after=*/true, 1);
      if (best < 0 && !charPointee) best = bestSizey(/*after=*/false, 1);
      if (best >= 0) { sizeArg = best; sizeHeuristic = true; }
    }

    // --- pick a size kind ---

    if (opaque) {
      // void*: a buffer only if we can size it (paired length). Otherwise it is
      // an opaque object passed by reference — i.e. a handle. We bias to handle
      // because misclassifying a handle as a deep-copy buffer corrupts memory,
      // while the reverse only loses an optimization.
      if (sizeArg >= 0) {
        node->sizeKind = fromPointee ? SizeKind::FromArgPointee : SizeKind::FromArg;
        node->sizeArgIndex = sizeArg;
        if (sizeHeuristic)
          ft.warnings.push_back("arg" + std::to_string(p) +
              (fromPointee ? ": void* size taken from *arg" : ": void* size paired to arg") +
              std::to_string(sizeArg) + " heuristically");
      } else {
        node->isHandle = true;
        node->handleClass = "void";
        node->sizeKind = SizeKind::Unknown;
        node->note = "opaque void* with no length — handle candidate";
        ft.warnings.push_back("arg" + std::to_string(p) +
            ": opaque void* treated as handle (verify; could be an unsized buffer)");
      }
    } else if (pointee->isComposite()) {
      if (isKnownOpaqueStruct(pointee)) {
        node->isHandle = true;          // FILE*/DIR*/incomplete-composite -> handle
        node->handleClass = canonicalHandleClass(pointee->typeName);
      } else if (pointee->sizeBytes > 0 && !pointee->depthTruncated &&
                 annotateComposite(node->children[0].get(), ft, p)) {
        node->sizeKind = SizeKind::Const;               // resolvable -> deep marshal
        node->constSize = pointee->sizeBytes;
      } else {
        ft.forceLocal = true;                           // cycle/fn-ptr/unsized inner
        ft.warnings.push_back("arg" + std::to_string(p) + ": struct '" +
            pointee->typeName + "' not fully resolvable — force_local");
      }
    } else if (pointee->kind == NodeKind::Unknown) {
      ft.forceLocal = true;                             // pointer to function/etc
      ft.warnings.push_back("arg" + std::to_string(p) +
          ": pointer to function/unresolved type — force_local");
    } else if (pointee->kind == NodeKind::Pointer &&
               isPtrArrayArg(ft.funcName, p)) {
      // argv/envp: a NULL-terminated array of cstr pointers (IN). Deep-copy each
      // element string and rebuild a shadow array of shadow pointers.
      node->dir = Dir::In;
      node->sizeKind = SizeKind::PtrArray;
      TreeNode *elem = node->children[0].get(); // the inner char* element
      elem->dir = Dir::In;
      elem->sizeKind = SizeKind::Cstr;
      ft.warnings.push_back("arg" + std::to_string(p) +
          ": NULL-terminated array of C-string pointers (argv/envp)");
    } else if (pointee->kind == NodeKind::Pointer) {
      // OUT pointer-to-pointer whose inner value is an offset into another arg
      // (endptr idiom). General dataflow first; name-table fallback for the glibc
      // parse family that writes *endptr in a sibling TU.
      int into = detectOutPtrIntoArg(A, sretOffset);
      bool byTable = false;
      if (into < 0 && isParseEndptrFn(ft.funcName) && p == 1) { into = 0; byTable = true; }
      bool isCursor = detectCursor(A);
      if (into >= 0 && isCursor) {
        // DUAL SOURCE: *p is written from BOTH a foreign arg AND a load of its own
        // slot (a runtime branch selects which). This is the stateful/reentrant
        // token idiom (strtok_r: s ?: *save_ptr): on continuation calls the buffer
        // lives only behind *p from a PREVIOUS call, which we never deep-copy this
        // call. Neither endptr nor cursor marshalling is sound -> run locally.
        ft.forceLocal = true;
        ft.warnings.push_back("arg" + std::to_string(p) +
            ": stateful cursor (*p sourced from arg" + std::to_string(into) +
            " or *p across calls) — force_local");
      } else if (into >= 0) {
        node->dir = Dir::Out;
        node->sizeKind = SizeKind::Const;
        node->constSize = pointee->sizeBytes ? pointee->sizeBytes : 4;
        TreeNode *inner = node->children[0].get();
        inner->ptrIntoArg = true;
        inner->intoArgIndex = into;
        ft.warnings.push_back(
            "arg" + std::to_string(p) + ": *p translated as offset into arg" +
            std::to_string(into) + (byTable ? " (parse-family fallback)"
                                            : " (out-ptr-into-arg)"));
      } else if (isCursor) {
        // CURSOR: *p walks its own deep-copied pointee buffer (strsep/mbsrtowcs).
        node->dir = Dir::InOut;
        node->sizeKind = SizeKind::Const;
        node->constSize = pointee->sizeBytes ? pointee->sizeBytes : 4;
        TreeNode *inner = node->children[0].get();
        inner->cursor = true;
        inner->sizeKind = SizeKind::Cstr;
        inner->dir = inner->pointeeConst ? Dir::In : Dir::InOut;
        cursorArg = (int)p;
        ft.warnings.push_back("arg" + std::to_string(p) +
            ": cursor — *p walks its own deep-copied buffer");
      } else if (pointee->pointeeConst && slotIsReadOnly(A) &&
                 !pointee->children.empty() &&
                 pointee->children[0]->isComposite() &&
                 !treeHasUnmappable(pointee->children[0].get())) {
        // READ-ONLY NESTED: `const struct T **p` that is read but never written
        // (qsort/scandir comparators: alphasort/versionsort take two
        // `const struct dirent **`). The inner pointer points at a single,
        // fully-mappable const struct. Deep-copy ONE element IN: copy the inner
        // pointer (4 bytes), then deep-copy its const struct pointee.
        node->dir = Dir::In;
        node->sizeKind = SizeKind::Const;
        node->constSize = pointee->sizeBytes ? pointee->sizeBytes : 4;
        TreeNode *innerPtr = node->children[0].get(); // == pointee
        innerPtr->dir = Dir::In;
        innerPtr->sizeKind = SizeKind::Const;
        innerPtr->constSize = innerPtr->children[0]->sizeBytes;
        ft.warnings.push_back("arg" + std::to_string(p) +
            ": const ** read-only — deep-copied as a single element "
            "(comparator-style); verify it is not a NULL-terminated array");
      } else {
        // genuine pointer-to-pointer (char**/void** to an independent or fresh
        // buffer): the inner pointer needs translation we can't perform, so a
        // flat const copy would be silently wrong.
        ft.forceLocal = true;
        ft.warnings.push_back("arg" + std::to_string(p) +
            ": pointer-to-pointer (inner pointer untranslatable) — force_local");
      }
    } else {
      // pointer to a scalar/array element.
      uint64_t elem = pointee->sizeBytes ? pointee->sizeBytes : 1;
      bool charLike = (elem == 1);
      if (sizeArg >= 0 && !sizeHeuristic) {
        node->sizeKind = fromPointee ? SizeKind::FromArgPointee : SizeKind::FromArg;
        node->sizeArgIndex = sizeArg;
      } else if (charLike && (acc.stringOp ||
                 /* char* with no length companion */ sizeArg < 0)) {
        node->sizeKind = SizeKind::Cstr;
        if (!acc.stringOp)
          ft.warnings.push_back("arg" + std::to_string(p) +
              ": char* assumed NUL-terminated (CSTR) — verify");
      } else if (charLike && sizeArg >= 0) {
        // byte buffer paired with a length arg (heuristically) — *lenptr or len.
        node->sizeKind = fromPointee ? SizeKind::FromArgPointee : SizeKind::FromArg;
        node->sizeArgIndex = sizeArg;
        ft.warnings.push_back("arg" + std::to_string(p) +
            (fromPointee ? ": byte buffer size taken from *arg"
                         : ": byte buffer size paired to arg") +
            std::to_string(sizeArg) + " heuristically");
      } else if (contract && contract->count((int)p)) {
        // A checked-in config contract (Config.h) asserts this argument's
        // extent -- a human has verified it, presumably because static
        // analysis couldn't (see StrideVectorContract's own comment). This
        // wins over whatever the analyzer below would otherwise conclude,
        // and is recorded as such (Confidence::Configured) so the output
        // JSON always shows which functions were analyzed vs. asserted.
        const StrideVectorContract &c = contract->at((int)p);
        node->sizeKind = SizeKind::StrideVector;
        node->sizeOperand = extentOperandFromContract(c.sizeOperand);
        node->strideOperand = extentOperandFromContract(c.strideOperand);
        node->constSize = c.constSize;
        node->confidence = Confidence::Configured;
        if (!c.dir.empty())
          node->dir = dirFromContract(c.dir);
        ft.warnings.push_back("arg" + std::to_string(p) +
            ": StrideVector extent asserted by config contract (" +
            (config ? config->sourcePath : std::string()) +
            "), not proven by analysis");
      } else {
        // non-char scalar pointee with no proven BYTE-count length
        // (acc.lengths empty/unresolved). Before silently assuming one
        // element, check for ELEMENT-count array evidence -- either found
        // directly in THIS function's own loop (detectDirectArrayBound), or
        // one hop away via a delegated call to a callee whose body is
        // available (detectDelegatedArrayBound, skipped entirely when
        // config->maxDelegationHops==0). A genuinely single-scalar
        // out-param (e.g. frexp's int *exp) is UNCHANGED: neither check ever
        // fires for it, so it falls through to the same const-size path as
        // before.
        ArrayBound bound = detectDirectArrayBound(acc, sretOffset);
        std::string delegateName;
        if (!bound.length.valid() && (!config || config->maxDelegationHops >= 1))
          bound = detectDelegatedArrayBound(acc, sretOffset, calleeIndex,
                                            &delegateName);
        auto describe = [&](const ExtentOperand &op) {
          if (op.source == ExtentSource::Constant)
            return std::string("a compile-time constant (") +
                std::to_string(op.constValue) + ")";
          return "arg" + std::to_string(op.argIndex) +
              (op.source == ExtentSource::PointeeI32 ? " (via pointer)" : "");
        };

        if (bound.length.valid() && bound.stride.valid()) {
          // Full evidence: length AND a distinct stride operand both
          // resolved to real caller arguments -- the runtime marshals the
          // whole contiguous envelope those two values describe
          // (LIND_SIZE_STRIDE_VECTOR, computed from the real call's actual
          // argument values at dispatch time; see lind_marshal.h). No
          // force_local needed: both operands were independently PROVEN
          // (see detectDirectArrayBound/detectFactoredStrideTripCount),
          // never guessed.
          node->sizeKind = SizeKind::StrideVector;
          node->sizeOperand = bound.length;
          node->strideOperand = bound.stride;
          node->constSize = elem;
          ft.warnings.push_back("arg" + std::to_string(p) +
              ": strided vector (length=" + describe(bound.length) +
              ", stride=" + describe(bound.stride) +
              (delegateName.empty() ? ", found via loop analysis)"
                                    : ", found via kernel delegation to `" +
                                          delegateName + "`)") +
              " — the whole contiguous envelope [0, (length-1)*stride] is "
              "copied in/out, evaluated at dispatch time (an argument-"
              "sourced operand is read from the real call; a constant one "
              "is fixed) — see LIND_SIZE_STRIDE_VECTOR in lind_marshal.h");
        } else if (bound.length.valid()) {
          // Array-shaped, but no distinct stride operand resolved. The
          // runtime has no "N contiguous elements" primitive separate from
          // LIND_SIZE_STRIDE_VECTOR (which needs an explicit stride operand
          // to point at) -- still can't be safely sized. force_local rather
          // than silently marshal it as one element.
          node->sizeKind = SizeKind::Const;
          node->constSize = elem;
          ft.forceLocal = true;
          ft.warnings.push_back("arg" + std::to_string(p) +
              ": array-shaped (length governed by " + describe(bound.length) +
              (delegateName.empty() ? ", found via loop analysis)"
                                    : ", found via kernel delegation to `" +
                                          delegateName + "`)") +
              " but no distinct stride operand was found, and this tool "
              "has no \"N contiguous elements\" size primitive separate "
              "from the strided one — force_local rather than silently "
              "marshalling it as one element");
        } else if (!acc.escapes && !acc.requiresDynamicExtent) {
          // No length evidence anywhere, but analyzeAccess also found no
          // store-away, no call outside its small recognized set, and no
          // GEP off this pointer with a non-provably-zero index -- the
          // pointer's entire usage is visible right here, and none of it
          // walked more than one object (e.g. frexp's int *exp: loaded/
          // stored directly, never passed anywhere else). That absence of
          // any array-shaped evidence, combined with full local visibility,
          // is positive evidence of single-object access, not a guess.
          node->sizeKind = SizeKind::Const;
          node->constSize = elem;
        } else {
          // Either the pointer escapes to a sink this analysis can't see
          // into (stored into memory, or passed to a call outside the small
          // recognized set), or it's visibly indexed as more than one
          // object (a loop induction variable whose trip count didn't
          // resolve, a dynamic index, a nonzero constant index, or a
          // negative offset) with no exact extent proven or configured.
          // Either way, assuming one element would be a guess, not a proof
          // -- fail closed rather than silently marshal a partial object.
          ft.forceLocal = true;
          std::string why;
          if (acc.requiresDynamicExtent && acc.escapes)
            why = "pointer is indexed by an offset that isn't provably a "
                  "constant zero and also escapes to an unanalyzable callee "
                  "or store";
          else if (acc.requiresDynamicExtent)
            why = "pointer is indexed by an offset that isn't provably a "
                  "constant zero (loop, dynamic, constant-nonzero, or "
                  "negative), but no exact element extent was proven or "
                  "configured";
          else
            why = "pointer escapes to an unanalyzable callee or store with "
                  "no proven extent";
          ft.warnings.push_back("arg" + std::to_string(p) + ": " + why +
              " — refusing to assume a single element — force_local");
        }
      }
    }

    // const-qualified pointee is a hard read-only signal: the callee cannot
    // legally write through it. This overrides an unobserved/conservative guess.
    if (node->pointeeConst && !node->isHandle)
      node->dir = Dir::In;

    // Direction defaults when not observable here (access in a callee, or via
    // pointer arithmetic we don't track).
    if (node->dir == Dir::Unknown && !node->isHandle) {
      if (node->sizeKind == SizeKind::Cstr) {
        // C strings are read-only inputs by overwhelming convention.
        node->dir = Dir::In;
      } else if (node->sizeKind != SizeKind::Unknown &&
                 node->sizeKind != SizeKind::NA) {
        // Sized buffer: copy both ways (correct, just not minimal).
        node->dir = Dir::InOut;
        ft.warnings.push_back("arg" + std::to_string(p) +
            ": direction unobserved (escapes to callee?) — defaulted to INOUT");
      } else {
        ft.warnings.push_back("arg" + std::to_string(p) +
            ": pointer direction undetermined");
      }
    }

    // Any pointer we still couldn't size (and isn't a handle) is non-mappable →
    // the whole function runs locally (E2 / V4).
    if (!node->isHandle && node->sizeKind != SizeKind::Const &&
        node->sizeKind != SizeKind::FromArg &&
        node->sizeKind != SizeKind::FromArgPointee &&
        node->sizeKind != SizeKind::Cstr &&
        node->sizeKind != SizeKind::PtrArray &&
        node->sizeKind != SizeKind::StrideVector) {
      ft.forceLocal = true;
    }
  }

  // Safety net: by-value struct/union args (skipped by the pointer-only loop) and
  // any other non-mappable leftover → run the call locally.
  for (const auto &pn : ft.params)
    if (treeHasUnmappable(pn.get())) {
      ft.forceLocal = true;
      ft.warnings.push_back(
          "non-mappable component (by-value struct / nested pointer) — force_local");
      break;
    }

  // ---- return kind ----
  Type *rt = F.getReturnType();

  // WASM ABI lowering layer (see the block comment above lowerFp128Arg) --
  // TRUE sret is IR-visible via hasStructRetAttr() on arg0 (sretOffset==1);
  // fp128 return is the hardcoded case, since rt genuinely says `fp128`, not
  // void, at this level (confirmed via wasm-objdump: the real compiled
  // function takes a hidden i32 sret pointer as arg0 despite the .bc showing
  // a plain `fp128 @f(...)`). Either way, lowerAbiReturn represents the
  // hidden pointer as a synthetic LEADING argument (matching its real
  // position as the first raw wasm-level call argument) via ft.retSretArg,
  // spliced in as args[0] only at JSON-emission time in main.cpp.
  bool trueSret = sretOffset == 1;
  bool fp128Ret = rt->isFP128Ty();
  if (trueSret || fp128Ret)
    lowerAbiReturn(F, ft, trueSret, fp128Ret);

  if (fp128Ret || rt->isVoidTy()) {
    ft.retKind = RetKind::Void; // fp128's real ABI-level return IS void here
  } else if (!rt->isPointerTy()) {
    ft.retKind = RetKind::Scalar;
  } else if (cursorArg >= 0 &&
             retDerivesFromCursor(F, F.getArg(cursorArg + sretOffset))) {
    // strsep returns the token — a pointer into the cursor arg's deep-copied buffer.
    ft.retKind = RetKind::PtrIntoCursor;
    ft.retAliasArg = cursorArg;
  } else {
    AllocInfo ai = allocatorInfo(ft.funcName);
    const TreeNode *retPointee =
        (ft.ret && !ft.ret->children.empty()) ? ft.ret->children[0].get() : nullptr;
    bool retKnownOpaque = retPointee && isKnownOpaqueStruct(retPointee);

    if (ai.forceLocal) {                          // realloc-style alloc+copy
      ft.forceLocal = true;
      ft.warnings.push_back("return: realloc-style allocation — force_local");
    } else if (ai.isAlloc) {                      // malloc/calloc/... -> caller-cage alloc
      ft.retKind = RetKind::PtrAlloc;
      for (int s : ai.sizeArgs) ft.retAllocSizeArgs.push_back(s);
    } else if (retKnownOpaque) {                  // fopen/opendir -> FILE*/DIR* handle
      ft.retKind = RetKind::Handle;
      ft.retHandleClass = canonicalHandleClass(retPointee->typeName);
    } else {
      // does the return derive from an argument's buffer (alias vs into)?
      int aliasArg = -1; bool hadOffset = false;
      for (const BasicBlock &bb : F)
        if (auto *ri = dyn_cast<ReturnInst>(bb.getTerminator()))
          if (Value *rv = ri->getReturnValue()) {
            SmallPtrSet<const Value *, 16> seen;
            if (int idx = traceReturnPtr(rv, sretOffset, hadOffset, seen); idx >= 0)
              aliasArg = idx;
          }
      if (aliasArg >= 0) {
        ft.retKind = hadOffset ? RetKind::PtrIntoArg : RetKind::PtrAliasArg;
        ft.retAliasArg = aliasArg;
      } else if (int si = searcherReturnsIntoArg0(ft.funcName); si >= 0) {
        ft.retKind = RetKind::PtrIntoArg; ft.retAliasArg = si;  // strchr/index/...
      } else if (returnDerivesFromGlobal(F) && retPointee &&
                 retPointee->kind == NodeKind::Scalar &&
                 retPointee->sizeBytes == 1) {
        // &static char buffer (inet_ntoa/ctime/asctime, fixed strings) -> copy out.
        ft.retKind = RetKind::PtrToStatic;
        ft.retStaticSize = 0;                     // 0 => NUL-terminated C-string
        ft.warnings.push_back("return: pointer into a static/global buffer — "
            "copy-out (cstr); valid until next call");
      } else if (returnDerivesFromGlobal(F) && retPointee &&
                 (retPointee->isComposite() ||
                  retPointee->kind == NodeKind::Scalar ||
                  retPointee->kind == NodeKind::Array) &&
                 retPointee->sizeBytes && !treeHasUnmappable(retPointee)) {
        // &static struct/scalar (localtime/gmtime -> struct tm*) -> copy out.
        ft.retKind = RetKind::PtrToStatic;
        ft.retStaticSize = retPointee->sizeBytes;
        ft.warnings.push_back("return: pointer into a static/global buffer — "
            "copy-out (" + std::to_string(retPointee->sizeBytes) +
            " bytes); valid until next call");
      } else {
        ft.forceLocal = true;                     // unclassified pointer return
        ft.warnings.push_back("return: pointer of undetermined provenance — force_local");
      }
    }
  }

  // Final pass: regardless of how every individual argument/return
  // classified, a function whose total raw ABI slot count exceeds V1's fixed
  // transport width still needs that width recorded so gen_grate.py (V1-only)
  // can exclude it while gen_v2_adapter.py (variable-width) still picks it
  // up -- see annotateWideRawArgSlots's own comment. Runs last (not folded
  // into the per-arg loop above) because it needs the FINAL abiSlots/
  // retSretArg state, which per-arg classification only finishes determining
  // by the time this function returns.
  annotateWideRawArgSlots(ft);
}

} // namespace marshal
