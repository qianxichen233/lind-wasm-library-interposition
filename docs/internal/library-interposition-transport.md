# Library-call interposition transport (V1/V2)

This is the transport a grate uses to intercept a *library* call (a normal C
function such as `cblas_daxpy` or `strlen`, exported by a dynamically-linked
`.so`) made by another cage, as distinct from the *syscall*-grate mechanism
described in [Grates](grates.md) and [Clamping](clamping.md), which
interposes on `register_handler`/`make_syscall` instead. The two mechanisms
are independent: nothing here changes the syscall-grate ABI, and nothing
there changes this one. For calling a *remote* (inter-process or
inter-machine) library implementation, see
[Remote Library Calls](remote-library-calls.md) — that is a different
concern (where the real implementation runs), orthogonal to which transport
carries an inter-cage call to it.

Two transport generations exist side by side today, V1 and V2. This page
states the policy for which one new work should use, and why.

## Policy

- **V1 is a frozen legacy compatibility path.** It is not the target for new
  library-interposition work of any kind.
- **All new grates, libraries, symbols, and transport features use V2.**
  This includes interposing a library not previously interposed at all, and
  adding a symbol to a library that already has some V1-generated handlers.
- **V1 may still receive correctness and security maintenance** (fixing a
  real bug in its existing dispatch/marshalling path), since existing
  generated V1 handlers keep running unchanged.
- **Any expansion of V1** — a new V1-generated handler, a new symbol added
  to `gen_grate.py`'s V1 code path, or widening V1's own transport capacity
  — **requires an explicit exception**, justified in the change itself, not
  assumed by precedent.

## Why two transports

V1 (`tools/marshal-gen/gen_grate.py`, `pass_fptr_to_wt`,
`lind_marshal_dispatch`, `register_lib_handler`) is fixed-arity: every call
is packed into exactly `LIND_RAW_ARGS_MAX` (6) raw wasm-level
argument/cage-id pairs, dispatched through one generic runtime handler per
grate via a K&R blind function-pointer cast, ABI-patched at the binary
level by `--fpcast-emu`. A function needing more than 6 raw ABI slots
(post-ABI-lowering: a hidden sret/fp128-return pointer counts as a slot, a
multi-slot argument counts as N) cannot be carried by V1 at all — the
runtime aborts the whole grate process on the first real call to a
V1-generated handler wider than that, rather than corrupting the call.

V2 (`tools/marshal-gen/gen_v2_adapter.py`, `register_lib_handler_v2`,
`Linker::instance_dylink`'s V2 portal, `wasmtime_lind_3i::V2AdapterCache`)
has no such cap: each interposed symbol gets its own generated adapter with
the function's *exact* lowered wasm signature — any number of
`i32`/`i64`/`f32`/`f64` parameters — calling the real library function
through a real, correctly-typed C prototype rather than a blind cast. V1
and V2 share the same underlying pointer/handle/copy-back marshalling
primitives (`_lind_marshal_prepare_arg` / `_lind_marshal_finish_shadow` /
`_lind_marshal_translate_return` in `lind_marshal.h`), so a function's
*semantic* marshalling soundness (does marshal-infer prove its pointer
extents?) is identical either way; only the call-width limit differs.

marshal-infer itself no longer force_locals a function purely for exceeding
V1's 6-slot width (`Infer.cpp`'s `annotateWideRawArgSlots` only records the
width as a warning now) — `decision:"marshal"` reflects semantic
soundness alone. `gen_grate.py`'s own width gate
(`unmarshalable_reason`'s `max_args`, defaulting to `LIND_RAW_ARGS_MAX`) is
what keeps V1 generation specifically from picking up a wide function;
`gen_v2_adapter.py` (`max_args=None`) has no such cap and is the only
generator that should be handed a symbol requiring more than 6 raw ABI
slots.

## Current status

As of this writing: V1 has 64 generated handlers; V2 adds 8 more (real
OpenBLAS symbols requiring 7 raw ABI slots — a scalar-scaled `axpby` and a
plane-rotation `rot`, each in both CBLAS and classic Fortran-BLAS form),
none overlapping V1's set. Of those, two (`cblas_daxpby`/`daxpby_`) are
exercised end-to-end against the real, statically-linked OpenBLAS archive
with a same-cage numeric baseline (`tests/grate-tests/lib-interpose/
auto-openblas-v2wide-real`, `-fortran-real`); the rest are generated and
inferred correctly but have no dedicated end-to-end test yet. Retiring V1's
generator/inference width gates outright is explicitly *not* appropriate
yet: the great majority of OpenBLAS's wide functions remain `force_local`
for unrelated semantic reasons (unresolved matrix-pointer extents, in
particular), not because V1 can't carry them, and V2's own lifecycle
coverage (concurrent/nested/reentrant interposed calls specifically) is
inherited-by-construction from V1's proven concurrency machinery rather
than independently re-tested. See
`tools/marshal-infer/openblas_coverage.py`'s output for the live,
authoritative breakdown (V1/V2 generated counts, their combined unique
total, and exactly which symbols are exercised against which backing
implementation) rather than treating any number in this document as
current beyond the day it was written.
