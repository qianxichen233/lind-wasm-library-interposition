//! V2 (variable-width) library-call transport.
//!
//! Coexists with the fixed six-raw-argument path in `threei.rs`
//! (`dispatch_lib_call`, `GrateTrampolineFn`, `TRAMPOLINE_TABLE`) rather than
//! replacing it. A V1 handler keeps dispatching through the V1 path
//! unmodified; a V2 handler is registered and dispatched entirely through
//! the types and functions here.
//!
//! This module is the host transport and registration data model: a request
//! with an arbitrary, explicitly bounded argument count reaches a runtime's
//! registered handler and returns a typed, possibly multi-shape result
//! (`V2Outcome`), via the signature/registration types a runtime uses to
//! validate a real Wasm export before ever dispatching to it. It does not
//! itself resolve or validate a real Wasm export -- that requires Wasmtime
//! types and lives in `wasmtime-lind-3i::v2_adapter` -- nor does it touch
//! `Linker::instance_dylink`, which installs the caller-side portal in
//! `wasmtime`'s own `linker.rs`.

use std::sync::atomic::Ordering;

use cage::with_cage;
use dashmap::DashMap;
use lazy_static::lazy_static;

use crate::get_cage_runtime;

/// Transport ABI version for a V2 request. Bumped whenever the wire shape of
/// `V2RawArg`/`GrateTrampolineFnV2` changes incompatibly, so a runtime built
/// against an older layout fails a version check instead of misreading bytes.
pub const V2_ABI_VERSION: u32 = 2;

/// Hard ceiling on a V2 request's argument count. This bounds the request's
/// resource use (a checked allocation, not an unbounded one) -- it must not
/// become a new small fixed width. Chosen generously relative to any real
/// C function's parameter count; a request that legitimately needs more than
/// this many raw argument slots is not something this transport is meant to
/// carry raw (it would need aggregation into a struct/buffer instead).
pub const V2_MAX_ARGS: usize = 4096;

/// A V2 request currently supports at most one scalar result: `V2Outcome`
/// carries a `Vec<V2Value>` (not a fixed scalar) specifically so this bound
/// can widen later without changing the outcome's shape.
pub const V2_MAX_RESULTS: usize = 1;

/// The wasm value types the V2 transport can carry. Each argument and result
/// carries its own tag rather than being inferred from context, so a request
/// can mix types freely across its argument list.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u8)]
pub enum V2ValueType {
    I32 = 0,
    I64 = 1,
    F32 = 2,
    F64 = 3,
}

impl V2ValueType {
    fn from_u8(tag: u8) -> Option<Self> {
        match tag {
            0 => Some(V2ValueType::I32),
            1 => Some(V2ValueType::I64),
            2 => Some(V2ValueType::F32),
            3 => Some(V2ValueType::F64),
            _ => None,
        }
    }
}

/// An ordered, authoritative lowered signature: the exact param/result types
/// a generated adapter export must have. Registration-time data, checked
/// against whatever a worker actually resolves at runtime -- see
/// `V2Registration` in `lib_handler_table_v2.rs`.
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct V2Signature {
    pub params: Vec<V2ValueType>,
    pub results: Vec<V2ValueType>,
}

impl V2Signature {
    /// A deterministic identity for this signature shape, stable within one
    /// build (does not rely on Rust's randomized default hasher). Two
    /// `V2Signature`s with the same params/results always produce the same
    /// id; this is what lets a resolver detect a "stale ID" registration --
    /// one produced against an earlier version of a symbol whose real
    /// lowered signature has since changed -- without needing to compare
    /// every field by hand.
    pub fn id(&self) -> u64 {
        // FNV-1a: fixed, simple, and -- unlike SipHash-based DefaultHasher --
        // explicitly documented to be stable across processes and Rust
        // versions, which a persisted/compared signature identity requires.
        const OFFSET: u64 = 0xcbf2_9ce4_8422_2325;
        const PRIME: u64 = 0x0000_0100_0000_01B3;
        let mut h = OFFSET;
        let mut mix = |tag: u8| {
            h ^= tag as u64;
            h = h.wrapping_mul(PRIME);
        };
        mix(0xA1); // section marker: params
        for p in &self.params {
            mix(*p as u8);
        }
        mix(0xA2); // section marker: results
        for r in &self.results {
            mix(*r as u8);
        }
        h
    }
}

/// One typed value, carried as its exact raw bits rather than a native Rust
/// `f32`/`f64` -- Rust float equality collapses distinct NaN bit patterns,
/// which would silently corrupt a payload or signaling bit the transport is
/// supposed to preserve unchanged. `bits` holds the value zero-extended into
/// a `u64`; only the low 32 bits are significant for `I32`/`F32`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct V2Value {
    pub ty: V2ValueType,
    pub bits: u64,
}

impl V2Value {
    pub fn i32(v: i32) -> Self {
        V2Value {
            ty: V2ValueType::I32,
            bits: v as u32 as u64,
        }
    }
    pub fn i64(v: i64) -> Self {
        V2Value {
            ty: V2ValueType::I64,
            bits: v as u64,
        }
    }
    pub fn f32_bits(bits: u32) -> Self {
        V2Value {
            ty: V2ValueType::F32,
            bits: bits as u64,
        }
    }
    pub fn f64_bits(bits: u64) -> Self {
        V2Value {
            ty: V2ValueType::F64,
            bits,
        }
    }
}

/// One argument slot: a typed value plus the cage that owns it. `cageid`
/// plays the same role as V1's per-argument `argNcageid` -- the
/// address-space context a pointer-valued argument must be interpreted
/// against -- but pointer-ness itself is never inferred from `value` alone:
/// an `I32` carries the same address-space context here whether or not the
/// adapter's own signature happens to treat it as a pointer.
#[derive(Clone, Copy, Debug)]
pub struct V2Arg {
    pub value: V2Value,
    pub cageid: u64,
}

/// An owned, host-side V2 library-call request. Built once by the caller
/// (`Linker::instance_dylink`'s V2 portal, or a test driving
/// `dispatch_lib_call_v2` directly) and consumed by `dispatch_lib_call_v2`.
pub struct V2Request {
    pub abi_version: u32,
    /// Stable handler identity: an id minted by
    /// `lib_handler_table_v2::register_lib_handler_v2_entry`, resolved back
    /// to a `V2Registration` via `get_v2_registration_by_id` on the runtime
    /// side of `GrateTrampolineFnV2` -- never a raw grate-linear-memory
    /// function/context pointer (see `V2Registration`'s own doc for why).
    pub handler_id: u64,
    pub caller_cage: u64,
    /// Identity of the validated signature this request was checked
    /// against. A real caller sets this from the resolved
    /// `V2Registration.signature.id()`; the runtime side re-derives and
    /// compares it independently during adapter resolution
    /// (`wasmtime_lind_3i::v2_adapter::V2AdapterCache::resolve`) rather than
    /// trusting the caller's copy.
    pub signature_id: u64,
    pub args: Vec<V2Arg>,
    pub expected_results: Vec<V2ValueType>,
}

/// The result of a dispatched V2 call: a typed result list (0 or 1 values,
/// see `V2_MAX_RESULTS`), an explicit rejection (the request never reached
/// the handler), or a trap (the handler started running and did not return
/// normally). Distinguishing rejection from trap matters the same way it
/// does for V1's fail-closed tests: a caller-visible failure alone does not
/// prove *which* of these happened.
#[derive(Debug)]
pub enum V2Outcome {
    Ok(Vec<V2Value>),
    Rejected(String),
    Trapped(String),
}

/// Checked, explicit bounds for one V2 request. Failing this must never be
/// confused with a signature/type mismatch -- it is a resource bound, so the
/// diagnostic says so precisely rather than reusing a generic rejection
/// string.
pub fn check_v2_request_bounds(argc: usize, n_results: usize) -> Result<(), String> {
    if argc > V2_MAX_ARGS {
        return Err(format!(
            "V2 request carries {argc} arguments, exceeding the transport's \
             {V2_MAX_ARGS}-argument bound"
        ));
    }
    if n_results > V2_MAX_RESULTS {
        return Err(format!(
            "V2 request expects {n_results} results, exceeding the transport's \
             {V2_MAX_RESULTS}-result bound"
        ));
    }
    Ok(())
}

/// FFI-stable view of one `V2Arg`, used only for the trampoline handoff
/// below. `#[repr(C)]` so its layout does not depend on Rust's (unspecified)
/// default struct layout across the runtime-supplied trampoline boundary.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct V2RawArg {
    pub ty: u8,
    pub bits: u64,
    pub cageid: u64,
}

pub const V2_OUTCOME_OK: i32 = 0;
pub const V2_OUTCOME_REJECTED: i32 = 1;
pub const V2_OUTCOME_TRAPPED: i32 = 2;

/// Runtime-provided re-entry point for a V2 grate call, stored opaquely in
/// `TRAMPOLINE_TABLE_V2` the same way `GrateTrampolineFn` is stored in
/// `TRAMPOLINE_TABLE`. An owned `Vec<V2Arg>` cannot cross a raw
/// `extern "C"` function-pointer boundary directly, so this carries a
/// versioned host pointer/length view instead (`args_ptr`/`argc`), tagged
/// with `abi_version` so a mismatched caller/runtime build is rejected
/// outright rather than misreading the layout.
///
/// The callee MUST copy `args_ptr[..argc]` into its own owned storage before
/// doing anything that could yield, block on another call, or otherwise let
/// this call's stack frame (which owns the pointed-to storage) go away
/// before returning: the pointer is valid only for the duration of this
/// call, and this transport is synchronous end to end (see
/// `baseline-v1-library-call-transport.md`'s "Errno relay" section for why
/// that is already true of the existing V1 dispatch path this call is
/// analogous to).
///
/// On return, a `V2_OUTCOME_OK` tag with `expects_result == 1` must have
/// written the result's type tag to `*out_ty` and its raw bits to
/// `*out_bits`. A `V2_OUTCOME_REJECTED`/`V2_OUTCOME_TRAPPED` tag must have
/// recorded a diagnostic via `set_last_v2_diagnostic` before returning --
/// mirroring the existing errno relay's thread-local, synchronous,
/// read-immediately-after-return pattern.
pub type GrateTrampolineFnV2 = extern "C" fn(
    handler_id: u64,
    grateid: u64,
    caller_cage: u64,
    abi_version: u32,
    signature_id: u64,
    args_ptr: *const V2RawArg,
    argc: u64,
    expects_result: u8,
    out_ty: *mut u8,
    out_bits: *mut u64,
) -> i32;

lazy_static! {
    /// <runtime_id, GrateTrampolineFnV2>, parallel to `TRAMPOLINE_TABLE`.
    pub static ref TRAMPOLINE_TABLE_V2: DashMap<u64, GrateTrampolineFnV2> = DashMap::new();
}

pub fn register_trampoline_v2(runtime: u64, f: GrateTrampolineFnV2) {
    TRAMPOLINE_TABLE_V2.insert(runtime, f);
}

pub fn get_runtime_trampoline_v2(runtime: u64) -> Option<GrateTrampolineFnV2> {
    TRAMPOLINE_TABLE_V2.get(&runtime).map(|f| *f)
}

std::thread_local! {
    /// Rejection/trap diagnostic for the most recent V2 dispatch on this
    /// thread. Out-of-band for the same reason `LAST_GRATE_ERRNO` is: the
    /// trampoline boundary above returns only a plain `i32` tag, and this
    /// call is synchronous and thread-confined, so a thread-local round-trips
    /// safely as long as the caller reads it immediately after the matching
    /// call returns (`dispatch_lib_call_v2` does).
    static LAST_V2_DIAGNOSTIC: std::cell::RefCell<Option<String>> = std::cell::RefCell::new(None);
}

pub fn set_last_v2_diagnostic(msg: Option<String>) {
    LAST_V2_DIAGNOSTIC.with(|c| *c.borrow_mut() = msg);
}

fn take_last_v2_diagnostic() -> Option<String> {
    LAST_V2_DIAGNOSTIC.with(|c| c.borrow_mut().take())
}

/// Dispatch one V2 library call, mirroring `dispatch_lib_call`'s cage
/// liveness/in-flight bookkeeping and runtime-trampoline lookup, widened to
/// an arbitrary, explicitly bounded argument count and a structured outcome.
pub fn dispatch_lib_call_v2(handler_cage_id: u64, req: V2Request) -> V2Outcome {
    if let Err(reason) = check_v2_request_bounds(req.args.len(), req.expected_results.len()) {
        return V2Outcome::Rejected(reason);
    }

    let cage_dead = with_cage(handler_cage_id, |grate| {
        grate.grate_inflight.fetch_add(1, Ordering::AcqRel);
        if grate.is_dead.load(Ordering::Acquire) {
            grate.grate_inflight.fetch_sub(1, Ordering::AcqRel);
            return true;
        }
        false
    });
    match cage_dead {
        Some(false) => {}
        Some(true) => {
            return V2Outcome::Rejected(format!("handler cage {handler_cage_id} is dead"))
        }
        None => {
            return V2Outcome::Rejected(format!("handler cage {handler_cage_id} does not exist"))
        }
    }

    let outcome = (|| {
        let runtimeid = match get_cage_runtime(handler_cage_id) {
            Some(r) => r,
            None => {
                return V2Outcome::Rejected(format!(
                    "no runtime registered for cage {handler_cage_id}"
                ))
            }
        };
        let trampoline = match get_runtime_trampoline_v2(runtimeid) {
            Some(f) => f,
            None => {
                return V2Outcome::Rejected(format!(
                    "no V2 trampoline registered for runtime {runtimeid}"
                ))
            }
        };

        let raw_args: Vec<V2RawArg> = req
            .args
            .iter()
            .map(|a| V2RawArg {
                ty: a.value.ty as u8,
                bits: a.value.bits,
                cageid: a.cageid,
            })
            .collect();
        let expects_result = if req.expected_results.is_empty() {
            0u8
        } else {
            1u8
        };
        let mut out_ty: u8 = 0;
        let mut out_bits: u64 = 0;

        let tag = trampoline(
            req.handler_id,
            handler_cage_id,
            req.caller_cage,
            req.abi_version,
            req.signature_id,
            raw_args.as_ptr(),
            raw_args.len() as u64,
            expects_result,
            &mut out_ty,
            &mut out_bits,
        );

        match tag {
            V2_OUTCOME_OK => {
                if expects_result == 1 {
                    match V2ValueType::from_u8(out_ty) {
                        Some(ty) => V2Outcome::Ok(vec![V2Value { ty, bits: out_bits }]),
                        None => V2Outcome::Rejected(format!(
                            "handler returned an unrecognized result type tag {out_ty}"
                        )),
                    }
                } else {
                    V2Outcome::Ok(vec![])
                }
            }
            V2_OUTCOME_REJECTED => {
                V2Outcome::Rejected(take_last_v2_diagnostic().unwrap_or_else(|| "rejected".into()))
            }
            V2_OUTCOME_TRAPPED => {
                V2Outcome::Trapped(take_last_v2_diagnostic().unwrap_or_else(|| "trapped".into()))
            }
            other => V2Outcome::Rejected(format!("unknown V2 outcome tag {other}")),
        }
    })();

    with_cage(handler_cage_id, |grate| {
        grate.grate_inflight.fetch_sub(1, Ordering::AcqRel);
    });

    outcome
}
