//! V2 adapter resolution: given an already-instantiated grate module, resolve
//! and validate a registered adapter export's REAL Wasm signature against the
//! registration's authoritative signature, before ever calling it, and cache
//! the resolved `Func` for reuse.
//!
//! Each `GrateWorker` in `lib.rs` owns its own `V2AdapterCache` (a resolved
//! `Func` is tied to the `Store`/`Instance` it came from), so pooling and
//! concurrent worker reuse are already integrated there -- see
//! `GrateWorker::run_v2`. Individual worker teardown/recycling is not
//! implemented anywhere in `lib.rs` yet (workers live in their handler's
//! pool for the process's lifetime); `V2AdapterCache::invalidate_all` exists
//! as the entry point that teardown would call once it is.

use std::collections::HashMap;
use std::fmt;

use threei::{V2Registration, V2Signature, V2ValueType};
use wasmtime::{AsContextMut, Extern, Func, FuncType, Instance, Val, ValType};

/// The zero-arg, `i32`-returning function every V2 grate module must
/// export, reporting the manifest version its adapters were generated
/// against. One per module (a module is built by one generator run), not
/// one per adapter. A function, not a data global: an ordinary compiled C
/// global lives in linear memory (a data address), not as a wasm-level
/// `global` value, so a real exported function is what a real
/// `--compile-grate` build can actually give the resolver a value through.
pub const MANIFEST_VERSION_EXPORT: &str = "__lind_v2_manifest_version";

/// Every V2 adapter's real wasm signature has exactly this many leading
/// `i64` parameters -- `(source_cage, grate_cage)` -- before its logical
/// parameters. Transport-supplied call context, not something a calling app
/// passes explicitly (mirrors V1's `_lind_marshal_source_cage`/
/// `_lind_marshal_grate_cage`, set by the transport rather than the caller).
/// A `V2Registration`'s own `signature` describes only the logical part;
/// `resolve` strips these before comparing, and `call_v2_adapter` prepends
/// them before calling.
pub const V2_ADAPTER_LEADING_PARAMS: usize = 2;

/// Why a V2 adapter could not be resolved/validated. Each variant names the
/// exact fact that failed, mirroring `lib3i_unsupported_signature_reason`'s
/// style on the V1 side (linker.rs) -- a caller-visible rejection must say
/// specifically what was wrong, not just that something was.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum V2AdapterRejection {
    /// No export with the registered name exists in this module at all.
    MissingExport { export: String },
    /// An export with that name exists but is not a function.
    NotAFunction { export: String },
    /// The module has no `__lind_v2_manifest_version` zero-arg i32-returning
    /// function export, so its manifest version can't be checked at all.
    MissingVersionMarker,
    /// The module's manifest version does not match what the registration
    /// was produced against -- the registration is talking about a
    /// different generator run than the one that built this module.
    VersionMismatch { expected: u32, found: u32 },
    /// The resolved export's real signature is not representable in the V2
    /// transport's `I32`/`I64`/`F32`/`F64`-only vocabulary (a `v128`/
    /// `funcref`/`externref` param or result), or claims more results than
    /// `threei::V2_MAX_RESULTS` supports.
    UnsupportedType { export: String, detail: String },
    /// The resolved export's real signature does not match the
    /// registration's authoritative signature (by parameter/result type or
    /// count) -- includes both a genuinely wrong signature and a "stale ID"
    /// registration whose recorded signature no longer matches a rebuilt
    /// symbol's real one: both are the same fact from the resolver's point
    /// of view (recorded vs. actual disagree), so they share one variant.
    SignatureMismatch {
        export: String,
        expected: V2Signature,
        found: V2Signature,
    },
}

impl fmt::Display for V2AdapterRejection {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            V2AdapterRejection::MissingExport { export } => {
                write!(f, "no export named {export:?} in this grate module")
            }
            V2AdapterRejection::NotAFunction { export } => {
                write!(f, "export {export:?} exists but is not a function")
            }
            V2AdapterRejection::MissingVersionMarker => {
                write!(
                    f,
                    "module has no {MANIFEST_VERSION_EXPORT:?} i32 global export"
                )
            }
            V2AdapterRejection::VersionMismatch { expected, found } => write!(
                f,
                "registration expects manifest version {expected}, module reports {found}"
            ),
            V2AdapterRejection::UnsupportedType { export, detail } => {
                write!(f, "export {export:?}: {detail}")
            }
            V2AdapterRejection::SignatureMismatch {
                export,
                expected,
                found,
            } => write!(
                f,
                "export {export:?}: registered signature {expected:?} does not match \
                 resolved signature {found:?}"
            ),
        }
    }
}

fn v2_type_from_wasmtime(ty: &ValType) -> Option<V2ValueType> {
    match ty {
        ValType::I32 => Some(V2ValueType::I32),
        ValType::I64 => Some(V2ValueType::I64),
        ValType::F32 => Some(V2ValueType::F32),
        ValType::F64 => Some(V2ValueType::F64),
        _ => None,
    }
}

/// Reads a real Wasm function's actual lowered signature as a `V2Signature`,
/// or the specific `UnsupportedType` reason it can't be represented.
fn read_v2_signature(export: &str, ty: &FuncType) -> Result<V2Signature, V2AdapterRejection> {
    let mut params = Vec::with_capacity(ty.params().len());
    for (i, p) in ty.params().enumerate() {
        match v2_type_from_wasmtime(&p) {
            Some(v) => params.push(v),
            None => {
                return Err(V2AdapterRejection::UnsupportedType {
                    export: export.to_string(),
                    detail: format!(
                        "param {i} has type {p}, which the V2 transport cannot carry \
                         (only i32/i64/f32/f64 scalars are supported)"
                    ),
                });
            }
        }
    }
    let mut results = Vec::with_capacity(ty.results().len());
    for (i, r) in ty.results().enumerate() {
        match v2_type_from_wasmtime(&r) {
            Some(v) => results.push(v),
            None => {
                return Err(V2AdapterRejection::UnsupportedType {
                    export: export.to_string(),
                    detail: format!(
                        "result {i} has type {r}, which the V2 transport cannot carry \
                         (only i32/i64/f32/f64 scalars are supported)"
                    ),
                });
            }
        }
    }
    if results.len() > threei::V2_MAX_RESULTS {
        return Err(V2AdapterRejection::UnsupportedType {
            export: export.to_string(),
            detail: format!(
                "{} results, exceeding the V2 transport's {}-result capacity",
                results.len(),
                threei::V2_MAX_RESULTS
            ),
        });
    }
    Ok(V2Signature { params, results })
}

/// A resolved, validated V2 adapter ready to be called.
pub struct ResolvedV2Adapter {
    pub func: Func,
    pub signature: V2Signature,
    /// The exact registration this adapter was resolved and validated
    /// against, kept so a later cache lookup can tell a genuinely unchanged
    /// registration apart from a DIFFERENT one that happens to share the
    /// same `adapter_export` name (a re-registration with a bumped
    /// `manifest_version`, a corrected `signature`, or simply a different
    /// `grate_cage`) -- see `resolve`'s own doc.
    registration: V2Registration,
}

impl fmt::Debug for ResolvedV2Adapter {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ResolvedV2Adapter")
            .field("signature", &self.signature)
            .finish()
    }
}

/// Per-instance cache of resolved adapters, keyed by export name. A fresh
/// `Instance` (e.g. a new worker) needs its own cache -- a `Func` handle is
/// tied to the `Store`/`Instance` it was resolved from.
#[derive(Default)]
pub struct V2AdapterCache {
    cache: HashMap<String, ResolvedV2Adapter>,
}

impl V2AdapterCache {
    pub fn new() -> Self {
        Self::default()
    }

    /// Resolve `registration.adapter_export` against `instance`, validating
    /// the manifest version and the real signature BEFORE returning a
    /// callable handle for the ADAPTER itself -- the only call this function
    /// makes into `instance` is the version-marker function
    /// (`MANIFEST_VERSION_EXPORT`, dedicated side-effect-free metadata),
    /// never the adapter being validated.
    ///
    /// A repeat call for the same export name only reuses the cached entry
    /// when `registration` is IDENTICAL (by value) to the one the cached
    /// entry was validated against -- the export name alone is not a safe
    /// cache key, since a symbol can be re-registered (a new `V2Registration`
    /// under the same `adapter_export`) without this cache ever being told
    /// to invalidate. A changed registration always falls through to a
    /// fresh `resolve_uncached`, so a stale cache entry can never be handed
    /// back silently -- see the warm-cache negative tests in
    /// `v2_adapter_tests.rs`.
    pub fn resolve(
        &mut self,
        mut store: impl AsContextMut,
        instance: &Instance,
        registration: &V2Registration,
    ) -> Result<&ResolvedV2Adapter, V2AdapterRejection> {
        let stale = match self.cache.get(&registration.adapter_export) {
            Some(entry) => &entry.registration != registration,
            None => true,
        };
        if stale {
            let resolved = Self::resolve_uncached(&mut store, instance, registration)?;
            self.cache
                .insert(registration.adapter_export.clone(), resolved);
        }
        Ok(self
            .cache
            .get(&registration.adapter_export)
            .expect("just inserted above"))
    }

    /// Drops every cached resolution. Not currently called anywhere --
    /// individual worker teardown/recycling doesn't exist yet (see the
    /// module doc) -- but is the entry point that would need to run first,
    /// so a stale `Func` (tied to a `Store`/`Instance` that no longer
    /// exists) can never be reused once teardown is implemented.
    pub fn invalidate_all(&mut self) {
        self.cache.clear();
    }

    fn resolve_uncached(
        mut store: impl AsContextMut,
        instance: &Instance,
        registration: &V2Registration,
    ) -> Result<ResolvedV2Adapter, V2AdapterRejection> {
        let mut store = store.as_context_mut();
        let export = registration.adapter_export.as_str();

        let func = match instance.get_export(&mut store, export) {
            Some(Extern::Func(f)) => f,
            Some(_) => {
                return Err(V2AdapterRejection::NotAFunction {
                    export: export.to_string(),
                });
            }
            None => {
                return Err(V2AdapterRejection::MissingExport {
                    export: export.to_string(),
                });
            }
        };

        // Calling this is safe to do unconditionally during resolution: it
        // is dedicated, generator-emitted, side-effect-free metadata, never
        // the adapter being validated -- resolve() still never calls the
        // real handler before validation succeeds (see
        // rejection_never_calls_the_real_handler in the test suite).
        let version_func = match instance.get_export(&mut store, MANIFEST_VERSION_EXPORT) {
            Some(Extern::Func(f)) => f,
            _ => return Err(V2AdapterRejection::MissingVersionMarker),
        };
        let version_typed = version_func
            .typed::<(), i32>(&store)
            .map_err(|_| V2AdapterRejection::MissingVersionMarker)?;
        let found_version = version_typed
            .call(&mut store, ())
            .map_err(|_| V2AdapterRejection::MissingVersionMarker)?
            as u32;
        if found_version != registration.manifest_version {
            return Err(V2AdapterRejection::VersionMismatch {
                expected: registration.manifest_version,
                found: found_version,
            });
        }

        let ty = func.ty(&store);
        let full_sig = read_v2_signature(export, &ty)?;

        // Every V2 adapter's real wasm signature is
        // (i64 source_cage, i64 grate_cage, <logical params...>) ->
        // (<logical result>) -- see gen_v2_adapter.py's own doc for why:
        // these two leading values are transport-supplied context, not
        // something a calling app passes explicitly, so a `V2Registration`
        // only ever describes the LOGICAL signature. Strip them here before
        // comparing against the registration, rather than requiring every
        // registration to redundantly re-encode this fixed convention.
        if full_sig.params.len() < V2_ADAPTER_LEADING_PARAMS
            || full_sig.params[..V2_ADAPTER_LEADING_PARAMS] != [V2ValueType::I64, V2ValueType::I64]
        {
            return Err(V2AdapterRejection::UnsupportedType {
                export: export.to_string(),
                detail: format!(
                    "adapter's first {V2_ADAPTER_LEADING_PARAMS} params must be \
                     (i64 source_cage, i64 grate_cage), found {:?}",
                    full_sig
                        .params
                        .get(..V2_ADAPTER_LEADING_PARAMS.min(full_sig.params.len()))
                ),
            });
        }
        let logical_sig = V2Signature {
            params: full_sig.params[V2_ADAPTER_LEADING_PARAMS..].to_vec(),
            results: full_sig.results.clone(),
        };

        // Structural equality, not `.id()` equality: `V2Signature::id()` is a
        // compact FNV-1a hash, useful as a cheap index/diagnostic (see
        // `V2Request::signature_id`'s own doc) but never a substitute for
        // comparing the actual params/results -- a hash collision must never
        // let a genuinely mismatched signature through. `V2Signature`
        // derives `PartialEq`, so this is exactly as cheap for the sizes
        // this transport deals with.
        if logical_sig != registration.signature {
            return Err(V2AdapterRejection::SignatureMismatch {
                export: export.to_string(),
                expected: registration.signature.clone(),
                found: logical_sig,
            });
        }

        Ok(ResolvedV2Adapter {
            func,
            signature: logical_sig,
            registration: registration.clone(),
        })
    }
}

/// The outcome of running one V2 request inside a worker: a typed result
/// list, an explicit rejection (resolution failed, so the adapter was never
/// called), or a trap (the adapter was called and did not return normally).
/// Mirrors `threei::V2Outcome`, but framed as what a WORKER just observed
/// rather than what a caller-side portal reports.
pub enum V2CallOutcome {
    Ok(Vec<Val>),
    Rejected(String),
    Trapped(String),
}

/// Calls a resolved V2 adapter with dynamic (not fixed-arity) argument and
/// result vectors, scoped to the caller's own `Store`. This is the mechanism
/// proof for the plan's "worker calls the cached adapter using dynamic
/// Wasmtime argument/result vectors" -- no process-global mutable argument
/// buffer, no fixed six-slot tuple.
pub fn call_v2_adapter(
    mut store: impl AsContextMut,
    adapter: &ResolvedV2Adapter,
    source_cage: u64,
    grate_cage: u64,
    logical_args: &[Val],
) -> wasmtime::Result<Vec<Val>> {
    let mut full_args = Vec::with_capacity(V2_ADAPTER_LEADING_PARAMS + logical_args.len());
    full_args.push(Val::I64(source_cage as i64));
    full_args.push(Val::I64(grate_cage as i64));
    full_args.extend_from_slice(logical_args);

    let mut results: Vec<Val> = adapter
        .signature
        .results
        .iter()
        .map(|t| match t {
            V2ValueType::I32 => Val::I32(0),
            V2ValueType::I64 => Val::I64(0),
            V2ValueType::F32 => Val::F32(0),
            V2ValueType::F64 => Val::F64(0),
        })
        .collect();
    adapter
        .func
        .call(store.as_context_mut(), &full_args, &mut results)?;
    Ok(results)
}
