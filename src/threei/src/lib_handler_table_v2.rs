//! V2 (variable-width) library-call registration table.
//!
//! Parallel to `lib_handler_table.rs`'s V1 table, but the stored identity is
//! deliberately NOT a raw `(handler_cage_id, fn_ptr)` pair: a grate-linear-
//! memory address is meaningless across the FFI trampoline boundary a V2
//! call crosses, and would force that boundary to reinterpret an opaque
//! integer as a live Rust reference. An interned `u64` id resolved back
//! through this table's own registry sidesteps that entirely. This table
//! also carries the checked signature/version metadata a V1 registration
//! never needed, since V1's fixed six-slot shape made per-symbol signature
//! checking unnecessary.

use std::collections::HashMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};

use crate::lib_call_v2::{V2Signature, V2ValueType};

/// One parameter of a callback's OWN signature -- never the host
/// function's. `Pointer` is representable so this schema does not change
/// shape once pointer-bearing callback marshalling exists, but a
/// registration containing one is rejected today (see `CallbackSignature`'s
/// own doc): nothing resolves a callback parameter's pointer extent yet.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CallbackParamKind {
    Scalar(V2ValueType),
    Pointer,
}

/// A callback's own return value. `FunctionPointer` (a callback that
/// itself returns a callback) is representable for the same forward-
/// compatibility reason as `CallbackParamKind::Pointer`, and is equally
/// rejected today.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CallbackRetKind {
    Void,
    Scalar(V2ValueType),
    FunctionPointer,
}

/// How long a callback endpoint may be invoked after the call that
/// supplied it returns. `DuringCall` is the only lifetime the dispatcher
/// currently enforces (a proxy is installed, used, and reclaimed within
/// one outer call -- see `GrateWorker::install_callback_proxies`);
/// `Retained` is representable, and rejected as unsupported today, until
/// a lifecycle-managed proxy registration exists.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CallbackLifetime {
    DuringCall,
    Retained,
}

/// A callback's full, versioned contract: the lowered Wasm shape of the
/// function pointer itself, independent of (and nested one level below)
/// the `V2Signature` of the host function the callback was passed into.
///
/// Deliberately NOT referenced by a named, separately-registered id the
/// way the inference tool's own `callback_signatures` config registry
/// works (see `tools/marshal-infer/CONFIG.md`'s "Callback contracts"):
/// that registry's whole job is resolving a human-reviewed name into a
/// fully-lowered shape once, at config-load time, long before anything
/// reaches this runtime -- by the time a registration reaches here, there
/// is no id left to look up, only an already-resolved shape. A second,
/// runtime-side name registry would just be a redundant copy of a check
/// the inference layer already owns end to end.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CallbackSignature {
    pub params: Vec<CallbackParamKind>,
    pub ret: CallbackRetKind,
    pub lifetime: CallbackLifetime,
    pub nullable: bool,
    pub reentry_policy: ReentryPolicy,
}

/// Which invocation-thread/re-entry policy a callback requires. A closed
/// vocabulary, not a recorded-but-uninterpreted string: a descriptor
/// naming a policy this runtime does not actually implement must be
/// rejected at registration time, not accepted and silently ignored.
///
/// `SameThreadOnly` is the only variant because it is the only policy
/// implemented: `wasmtime::callback_reentry`'s active-frame stack is
/// thread-local, and a callback proxy is only ever invoked synchronously,
/// on the same OS thread that is suspended inside the outer call that
/// supplied it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ReentryPolicy {
    SameThreadOnly,
}

impl ReentryPolicy {
    fn parse(s: &str) -> Option<Self> {
        match s {
            "same_thread_only" => Some(ReentryPolicy::SameThreadOnly),
            _ => None,
        }
    }
}

/// One function-pointer argument of a `V2Registration`: which parameter
/// index (into the outer `V2Signature`) carries it, and the callback's own
/// contract.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CallbackArgContract {
    pub arg_index: u32,
    pub signature: CallbackSignature,
}

/// One symbol's V2 registration: which grate cage owns it, the exported
/// adapter function's name inside that grate's module (resolved by name at
/// worker-creation time, never by address -- see the module doc), the
/// manifest version the registration was produced against, and the
/// authoritative lowered signature the resolved export must match exactly.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct V2Registration {
    pub grate_cage: u64,
    pub adapter_export: String,
    pub manifest_version: u32,
    pub signature: V2Signature,
    /// Parameters, by index into `signature.params`, whose raw i32 is a
    /// table index into the CALLING cage's own indirect-function table --
    /// a function pointer the caller is passing in -- rather than an
    /// ordinary scalar value. Each entry's `arg_index` names a distinct
    /// I32 parameter (enforced by `parse_v2_signature_desc`): the
    /// dispatcher resolves the caller's real target function at that
    /// index, checks it against the entry's own `CallbackSignature`, and
    /// replaces the argument with a locally callable proxy before the
    /// grate's adapter ever runs (see
    /// `GrateWorker::install_callback_proxies`), so the adapter itself
    /// needs no cross-cage awareness.
    ///
    /// Deliberately not part of `V2Signature`: that type's equality is the
    /// WASM-level value-type identity checked against a caller's
    /// independently-derived signature (`v2_signature_from_func_ty` in
    /// `linker.rs`), which has no way to know about callback semantics and
    /// would then never match. Empty for every registration that isn't
    /// callback-aware -- the normal case, checked once at portal-install
    /// time so calls to every other interposed function pay nothing for
    /// this.
    pub callback_params: Vec<CallbackArgContract>,
}

fn lib_handler_table_v2() -> &'static Mutex<HashMap<u64, HashMap<(String, String), u64>>> {
    static TABLE: OnceLock<Mutex<HashMap<u64, HashMap<(String, String), u64>>>> = OnceLock::new();
    TABLE.get_or_init(|| Mutex::new(HashMap::new()))
}

/// Interned registry backing every V2 registration's stable identity: a
/// plain `u64` id a portal captures at link time and later hands to
/// `dispatch_lib_call_v2`, which passes it across the `GrateTrampolineFnV2`
/// FFI boundary as `handler_id`. An id, not a raw `Arc` pointer cast to
/// `u64`, so that boundary never needs to reinterpret an opaque integer as a
/// live Rust reference -- the runtime-side trampoline looks the id back up
/// through this same safe table instead.
fn v2_registration_registry() -> &'static Mutex<HashMap<u64, Arc<V2Registration>>> {
    static REGISTRY: OnceLock<Mutex<HashMap<u64, Arc<V2Registration>>>> = OnceLock::new();
    REGISTRY.get_or_init(|| Mutex::new(HashMap::new()))
}

fn next_v2_handler_id() -> u64 {
    static NEXT_ID: AtomicU64 = AtomicU64::new(1);
    NEXT_ID.fetch_add(1, Ordering::Relaxed)
}

/// Which cages currently hold a LIVE reference to each V2 registration id,
/// and the reverse index used to release them all at once on cage exit.
///
/// A cage holds a reference to an id for either of two reasons, both tied to
/// that SAME cage's own lifetime:
///   - its own (lib_name, symbol_name) -> id table entry could still be
///     looked up by a future portal install (e.g. on exec/dlopen replay);
///   - it already installed a portal (`Linker::instance_dylink`'s V2 portal
///     in `linker.rs`) whose closure captured this id directly.
/// The second kind is invisible to (and outlives) the table: re-registering
/// the same (lib_name, symbol_name) key later never touches an
/// already-installed portal's captured id (see `register_lib_handler_v2_entry`'s
/// own doc), so a table overwrite must never be treated as releasing a
/// reference. Both kinds are therefore only ever added during a cage's
/// lifetime and released together, in bulk, at that cage's exit -- see
/// `add_v2_registration_ref`/`release_v2_registration_refs`.
struct V2RefState {
    /// id -> cage_ids holding a reference to it.
    holders: HashMap<u64, std::collections::HashSet<u64>>,
    /// cage_id -> ids it holds a reference to (reverse index).
    by_cage: HashMap<u64, std::collections::HashSet<u64>>,
}

fn v2_ref_state() -> &'static Mutex<V2RefState> {
    static STATE: OnceLock<Mutex<V2RefState>> = OnceLock::new();
    STATE.get_or_init(|| {
        Mutex::new(V2RefState {
            holders: HashMap::new(),
            by_cage: HashMap::new(),
        })
    })
}

/// Records that `cage_id` now holds a live reference to registration `id`.
/// Idempotent -- safe to call more than once for the same (id, cage_id)
/// pair (e.g. a table-registration ref and a later install-time ref for the
/// same cage collapse into the same membership, since both die together at
/// that cage's exit anyway).
pub fn add_v2_registration_ref(id: u64, cage_id: u64) {
    let mut st = v2_ref_state().lock().unwrap();
    st.holders.entry(id).or_default().insert(cage_id);
    st.by_cage.entry(cage_id).or_default().insert(id);
}

/// Releases every reference `cage_id` holds -- from its own table entries
/// and from any portal it installed -- and reclaims the interned
/// registration for any id left with no remaining holder. Called once at
/// cage exit, alongside `rm_cage_from_lib_handler_table_v2`.
///
/// A registration is intentionally NOT reclaimed just because its owning
/// cage's table entries were removed or overwritten -- only cage exit
/// (this function) can drop a reference, since only cage exit guarantees no
/// live installed portal for that cage can still call it.
pub fn release_v2_registration_refs(cage_id: u64) {
    let mut reclaimable: Vec<u64> = Vec::new();
    {
        let mut st = v2_ref_state().lock().unwrap();
        if let Some(ids) = st.by_cage.remove(&cage_id) {
            for id in ids {
                if let Some(set) = st.holders.get_mut(&id) {
                    set.remove(&cage_id);
                    if set.is_empty() {
                        st.holders.remove(&id);
                        reclaimable.push(id);
                    }
                }
            }
        }
    }
    if !reclaimable.is_empty() {
        let mut registry = v2_registration_registry().lock().unwrap();
        for id in reclaimable {
            registry.remove(&id);
        }
    }
}

/// Test-only introspection: the current holder set for `id`, or `None` if
/// nothing references it (either never registered, or fully reclaimed).
#[cfg(test)]
fn v2_registration_holders(id: u64) -> Option<std::collections::HashSet<u64>> {
    v2_ref_state().lock().unwrap().holders.get(&id).cloned()
}

/// Register a (lib_name, symbol_name) -> V2Registration mapping for cage_id.
/// Each call mints a fresh handler id; a symbol registered again later gets
/// a new id and registration, matching V1's own "captured once at link
/// time" behavior -- a portal already installed against an older id keeps
/// working against the registration it was actually installed for.
pub fn register_lib_handler_v2_entry(
    cage_id: u64,
    lib_name: &str,
    symbol_name: &str,
    registration: V2Registration,
) {
    let id = next_v2_handler_id();
    v2_registration_registry()
        .lock()
        .unwrap()
        .insert(id, Arc::new(registration));
    add_v2_registration_ref(id, cage_id);

    let mut table = lib_handler_table_v2().lock().unwrap();
    table
        .entry(cage_id)
        .or_default()
        .insert((lib_name.to_string(), symbol_name.to_string()), id);
}

/// Look up the (handler id, V2 registration) for (cage_id, lib_name,
/// symbol_name). The id is what a portal passes on to
/// `dispatch_lib_call_v2`/`GrateTrampolineFnV2` as `handler_id`.
pub fn get_lib_handler_v2(
    cage_id: u64,
    lib_name: &str,
    symbol_name: &str,
) -> Option<(u64, Arc<V2Registration>)> {
    let id = {
        let table = lib_handler_table_v2().lock().unwrap();
        *table
            .get(&cage_id)?
            .get(&(lib_name.to_string(), symbol_name.to_string()))?
    };
    let registration = v2_registration_registry().lock().unwrap().get(&id)?.clone();
    Some((id, registration))
}

/// Resolve a handler id (as produced by `get_lib_handler_v2`, carried
/// through `dispatch_lib_call_v2`) back to its `V2Registration`. Called by
/// the runtime-side `GrateTrampolineFnV2` implementation.
pub fn get_v2_registration_by_id(id: u64) -> Option<Arc<V2Registration>> {
    v2_registration_registry().lock().unwrap().get(&id).cloned()
}

/// Remove all V2 lib handler entries for cage_id. Called on cage exit/cleanup.
///
/// Only removes this cage's OWN (lib_name, symbol_name) -> id mappings, not
/// the interned registrations those ids point to -- a registration id may
/// still be referenced by a portal already installed in some OTHER cage
/// (the caller cage that had this symbol interposed), so it is left in the
/// registry rather than invalidated here. `dispatch_lib_call_v2`'s existing
/// cage-liveness check (the handler/grate cage, not the registering cage)
/// is what actually rejects a call whose GRATE has exited.
pub fn rm_cage_from_lib_handler_table_v2(cage_id: u64) {
    let mut table = lib_handler_table_v2().lock().unwrap();
    table.remove(&cage_id);
}

/// Copy all V2 lib handler entries from src_cage_id to dst_cage_id. Called by
/// `fork_syscall` so a forked child cage inherits the parent's registered V2
/// handlers -- see `baseline-v1-library-call-transport.md`'s "Fork-copy and
/// teardown: two confirmed gaps" for why V1's own equivalent function exists
/// but was never actually wired into fork, a gap V2 does not repeat.
pub fn copy_lib_handler_table_v2_to_cage(src_cage_id: u64, dst_cage_id: u64) {
    let mut table = lib_handler_table_v2().lock().unwrap();
    if let Some(src_map) = table.get(&src_cage_id).cloned() {
        // The child cage is now itself a holder of every id it inherited --
        // without this, if only the PARENT later exits, `dst_cage_id`'s own
        // (now-copied) table entries and any portal it later installs from
        // them would reference an id that could already have been reclaimed.
        for &id in src_map.values() {
            add_v2_registration_ref(id, dst_cage_id);
        }
        table.insert(dst_cage_id, src_map);
    }
}

#[cfg(test)]
mod reclaim_tests {
    use super::*;

    // Every id minted by register_lib_handler_v2_entry is globally unique
    // (a process-wide AtomicU64 counter), so registrations from different
    // tests never collide even though these tests share the SAME process-
    // global tables/registry (cargo runs unit tests in this binary in
    // parallel by default). Cage ids only need to be unique WITHIN a test
    // to keep its own reasoning simple, so each test picks its own
    // disjoint small range via this counter.
    fn fresh_cage_id() -> u64 {
        static NEXT: AtomicU64 = AtomicU64::new(1_000_000);
        NEXT.fetch_add(1, Ordering::Relaxed)
    }

    fn dummy_registration() -> V2Registration {
        V2Registration {
            grate_cage: fresh_cage_id(),
            adapter_export: "__lind_v2_adapter_test".to_string(),
            manifest_version: 1,
            signature: V2Signature {
                params: vec![],
                results: vec![],
            },
            callback_params: vec![],
        }
    }

    #[test]
    fn registration_is_reclaimed_after_its_only_cage_exits() {
        let cage = fresh_cage_id();
        register_lib_handler_v2_entry(cage, "libtest", "sym_a", dummy_registration());
        let (id, _reg) = get_lib_handler_v2(cage, "libtest", "sym_a").expect("just registered");
        assert!(get_v2_registration_by_id(id).is_some());
        assert_eq!(
            v2_registration_holders(id),
            Some([cage].into_iter().collect())
        );

        rm_cage_from_lib_handler_table_v2(cage);
        release_v2_registration_refs(cage);

        assert!(
            get_v2_registration_by_id(id).is_none(),
            "registration must be reclaimed"
        );
        assert_eq!(v2_registration_holders(id), None);
    }

    #[test]
    fn re_registration_keeps_the_old_id_alive_until_this_cage_exits() {
        // Mirrors register_lib_handler_v2_entry's own doc: re-registering
        // the same (cage, lib, symbol) mints a NEW id: an already-installed
        // portal captured the OLD id directly and keeps working against it,
        // so the old id must remain resolvable until THIS cage exits, not
        // be dropped the moment the table entry is overwritten.
        let cage = fresh_cage_id();
        register_lib_handler_v2_entry(cage, "libtest", "sym_a", dummy_registration());
        let (old_id, _) = get_lib_handler_v2(cage, "libtest", "sym_a").unwrap();

        register_lib_handler_v2_entry(cage, "libtest", "sym_a", dummy_registration());
        let (new_id, _) = get_lib_handler_v2(cage, "libtest", "sym_a").unwrap();
        assert_ne!(old_id, new_id);

        // The table now only points at new_id, but old_id -- as if an
        // earlier-installed portal still captured it -- must stay resolvable.
        assert!(get_v2_registration_by_id(old_id).is_some());
        assert!(get_v2_registration_by_id(new_id).is_some());

        rm_cage_from_lib_handler_table_v2(cage);
        release_v2_registration_refs(cage);

        assert!(get_v2_registration_by_id(old_id).is_none());
        assert!(get_v2_registration_by_id(new_id).is_none());
    }

    #[test]
    fn fork_copied_cage_keeps_registration_alive_after_parent_exits() {
        let parent = fresh_cage_id();
        let child = fresh_cage_id();
        register_lib_handler_v2_entry(parent, "libtest", "sym_a", dummy_registration());
        let (id, _) = get_lib_handler_v2(parent, "libtest", "sym_a").unwrap();

        copy_lib_handler_table_v2_to_cage(parent, child);
        assert!(get_lib_handler_v2(child, "libtest", "sym_a").is_some());

        // Parent exits first: the child inherited its own reference at
        // fork-copy time, so the registration must survive.
        rm_cage_from_lib_handler_table_v2(parent);
        release_v2_registration_refs(parent);
        assert!(
            get_v2_registration_by_id(id).is_some(),
            "child's inherited reference must keep the registration alive"
        );

        // Only once the child ALSO exits is the registration unreachable.
        rm_cage_from_lib_handler_table_v2(child);
        release_v2_registration_refs(child);
        assert!(get_v2_registration_by_id(id).is_none());
    }

    #[test]
    fn install_time_reference_survives_a_later_table_overwrite() {
        // Simulates: cage installs a portal for id X (add_v2_registration_ref,
        // as linker.rs does at portal-creation time), then the SAME cage's
        // table entry for that (lib, symbol) is later overwritten by a fresh
        // registration (id Y). X must remain resolvable until the cage
        // itself exits -- an already-installed portal's captured id must
        // never be silently invalidated by an unrelated table overwrite.
        let cage = fresh_cage_id();
        register_lib_handler_v2_entry(cage, "libtest", "sym_a", dummy_registration());
        let (old_id, _) = get_lib_handler_v2(cage, "libtest", "sym_a").unwrap();
        add_v2_registration_ref(old_id, cage); // portal install, redundant with the table ref

        register_lib_handler_v2_entry(cage, "libtest", "sym_a", dummy_registration());
        let (new_id, _) = get_lib_handler_v2(cage, "libtest", "sym_a").unwrap();
        assert_ne!(old_id, new_id);

        assert!(get_v2_registration_by_id(old_id).is_some());

        rm_cage_from_lib_handler_table_v2(cage);
        release_v2_registration_refs(cage);
        assert!(get_v2_registration_by_id(old_id).is_none());
        assert!(get_v2_registration_by_id(new_id).is_none());
    }

    #[test]
    fn repeated_cage_id_reuse_starts_clean() {
        // A cage id reused after a prior generation fully exited must not
        // inherit any stale reference bookkeeping from that earlier
        // generation.
        let cage = fresh_cage_id();
        register_lib_handler_v2_entry(cage, "libtest", "sym_a", dummy_registration());
        let (id1, _) = get_lib_handler_v2(cage, "libtest", "sym_a").unwrap();
        rm_cage_from_lib_handler_table_v2(cage);
        release_v2_registration_refs(cage);
        assert_eq!(v2_registration_holders(id1), None);

        // Same numeric cage id, fresh "generation".
        register_lib_handler_v2_entry(cage, "libtest", "sym_b", dummy_registration());
        let (id2, _) = get_lib_handler_v2(cage, "libtest", "sym_b").unwrap();
        assert_eq!(
            v2_registration_holders(id2),
            Some([cage].into_iter().collect())
        );
        rm_cage_from_lib_handler_table_v2(cage);
        release_v2_registration_refs(cage);
        assert!(get_v2_registration_by_id(id2).is_none());
    }
}

/// Parses one type character into a `V2ValueType`: `i`=I32, `l`=I64 ("long"),
/// `f`=F32, `d`=F64 (double). Case matters; anything else is unrecognized.
fn parse_v2_type_char(c: char) -> Option<V2ValueType> {
    match c {
        'i' => Some(V2ValueType::I32),
        'l' => Some(V2ValueType::I64),
        'f' => Some(V2ValueType::F32),
        'd' => Some(V2ValueType::F64),
        _ => None,
    }
}

/// Parses one callback-parameter type character: the same `i`/`l`/`f`/`d`
/// vocabulary as an ordinary V2 value, plus `p` for a pointer-classified
/// callback parameter. A `p` always parses successfully here (the
/// grammar can represent it); whether it's actually ACCEPTED is a
/// separate, later check -- see `parse_callback_spec`'s own doc.
fn parse_callback_param_char(c: char) -> Option<CallbackParamKind> {
    if c == 'p' {
        Some(CallbackParamKind::Pointer)
    } else {
        parse_v2_type_char(c).map(CallbackParamKind::Scalar)
    }
}

/// Parses a callback's return-shape string: empty = void, one type
/// character (`i`/`l`/`f`/`d`) = that scalar, `p` = a function-pointer
/// result. More than one character is always malformed -- a callback, like
/// the outer V2 transport, carries at most one result.
fn parse_callback_ret_str(s: &str) -> Option<CallbackRetKind> {
    let mut chars = s.chars();
    let Some(c) = chars.next() else {
        return Some(CallbackRetKind::Void);
    };
    if chars.next().is_some() {
        return None;
    }
    if c == 'p' {
        Some(CallbackRetKind::FunctionPointer)
    } else {
        parse_v2_type_char(c).map(CallbackRetKind::Scalar)
    }
}

/// Parses one `@`-separated callback-argument spec:
/// `"<arg_index>@<params>@<ret>@<lifetime>@<nullable>@<reentry_policy>"`,
/// e.g. `"0@i@@D@0@same_thread_only"` -- parameter 0 is a callback taking
/// one I32 and returning void, `during_call`-scoped, non-nullable, with
/// reentry policy "same_thread_only". `lifetime` is `D` (`during_call`) or
/// `R` (`retained`); `nullable` is `0` or `1`; `reentry_policy` is one of
/// `ReentryPolicy`'s closed vocabulary (just "same_thread_only" today) --
/// an unrecognized policy name is malformed input, not a value recorded
/// verbatim and left for something else to (maybe) interpret later.
///
/// Successfully parsing a spec is NOT the same as accepting it: a `p`
/// (pointer) parameter, a `p` (function-pointer) return, or
/// `lifetime=="R"` all parse into a structurally valid `CallbackSignature`
/// -- the schema can represent them -- but `register_lib_handler_v2`
/// rejects all three explicitly as not yet supported, rather than this
/// parser pretending they don't exist or silently dropping them.
fn parse_callback_spec(spec: &str) -> Option<(u32, CallbackSignature)> {
    let mut parts = spec.splitn(6, '@');
    let arg_index: u32 = parts.next()?.parse().ok()?;
    let params_str = parts.next()?;
    let ret_str = parts.next()?;
    let lifetime_str = parts.next()?;
    let nullable_str = parts.next()?;
    let reentry_policy = parts.next()?;

    let params = params_str
        .chars()
        .map(parse_callback_param_char)
        .collect::<Option<Vec<_>>>()?;
    let ret = parse_callback_ret_str(ret_str)?;
    let lifetime = match lifetime_str {
        "D" => CallbackLifetime::DuringCall,
        "R" => CallbackLifetime::Retained,
        _ => return None,
    };
    let nullable = match nullable_str {
        "0" => false,
        "1" => true,
        _ => return None,
    };
    let reentry_policy = ReentryPolicy::parse(reentry_policy)?;

    Some((
        arg_index,
        CallbackSignature {
            params,
            ret,
            lifetime,
            nullable,
            reentry_policy,
        },
    ))
}

/// Parses a compact signature descriptor string of the form
/// `"<manifest_version>:<params>:<results>[:<callback_specs>]"`, where
/// `params`/`results` are each a (possibly empty) run of type characters
/// (see `parse_v2_type_char`) -- e.g. `"2:iid:d"` is manifest version 2,
/// params `[I32, I32, F64]`, results `[F64]`.
///
/// The optional 4th segment is a comma-separated list of callback-argument
/// specs (see `parse_callback_spec`), one per parameter whose raw i32 is a
/// table index into the CALLING cage's own indirect-function table (a
/// function pointer) rather than an ordinary scalar value. Absent or empty
/// means no callback parameters -- every descriptor without this segment
/// parses identically to one with an empty 4th segment.
///
/// A raw `extern "C"` syscall (see `register_lib_handler_v2` below) has a
/// fixed six-raw-argument-pair shape, the exact width limitation V2 exists
/// to work around for LIBRARY calls -- it cannot itself carry a variable-
/// length params/results list as separate slots. Encoding the whole
/// signature as one compact string keeps registration a single extra
/// pointer argument, reusing the same "pointer to a string the syscall
/// dispatch layer already translates to a host address" mechanism
/// `lib_name_ptr`/`symbol_name_ptr` already rely on, instead of inventing a
/// new argument-passing mechanism just for this one call.
fn parse_v2_signature_desc(s: &str) -> Option<(u32, V2Signature, Vec<CallbackArgContract>)> {
    let mut parts = s.splitn(4, ':');
    let version: u32 = parts.next()?.parse().ok()?;
    let params_str = parts.next()?;
    let results_str = parts.next()?;
    let callback_specs_str = parts.next().unwrap_or("");
    let params = params_str
        .chars()
        .map(parse_v2_type_char)
        .collect::<Option<Vec<_>>>()?;
    let results = results_str
        .chars()
        .map(parse_v2_type_char)
        .collect::<Option<Vec<_>>>()?;
    let callback_params = if callback_specs_str.is_empty() {
        Vec::new()
    } else {
        callback_specs_str
            .split(',')
            .map(|spec| {
                let (arg_index, signature) = parse_callback_spec(spec)?;
                Some(CallbackArgContract {
                    arg_index,
                    signature,
                })
            })
            .collect::<Option<Vec<_>>>()?
    };

    // Every callback arg_index must name a real, distinct, I32 parameter.
    // A duplicate is especially dangerous, not just redundant: the first
    // occurrence replaces the caller's raw index with a LOCAL proxy index
    // before the second occurrence is processed, so the second pass would
    // misinterpret that already-replaced proxy index as another raw
    // caller-side table index.
    let mut seen = std::collections::HashSet::new();
    for c in &callback_params {
        let idx = c.arg_index as usize;
        if idx >= params.len() || !seen.insert(idx) || params[idx] != V2ValueType::I32 {
            return None;
        }
    }

    Some((version, V2Signature { params, results }, callback_params))
}

/// A callback spec can parse into a structurally valid `CallbackSignature`
/// describing a shape nothing in this runtime actually implements yet:
/// a pointer-bearing callback parameter (callback argument marshalling is
/// scalar-only), a callback returning a function pointer, or a retained
/// (outlives the call that installed it) lifetime. Returns the rejection
/// message for the first such shape found, or `None` if every entry is
/// fully supported today -- checked explicitly, rather than silently
/// accepted and mishandled later.
fn reject_unsupported_callback_shape(callback_params: &[CallbackArgContract]) -> Option<String> {
    for c in callback_params {
        if c.signature.params.contains(&CallbackParamKind::Pointer) {
            return Some(format!(
                "arg{}: pointer-bearing callback parameters are not yet supported",
                c.arg_index
            ));
        }
        if c.signature.ret == CallbackRetKind::FunctionPointer {
            return Some(format!(
                "arg{}: a callback returning a function pointer is not yet supported",
                c.arg_index
            ));
        }
        if c.signature.lifetime == CallbackLifetime::Retained {
            return Some(format!(
                "arg{}: retained callback lifetime is not yet supported (during_call only)",
                c.arg_index
            ));
        }
    }
    None
}

/// Register a V2 (variable-width) library-level handler for
/// (lib_name, symbol_name) in target_cage. The syscall-shaped counterpart to
/// V1's `register_lib_handler` (`threei::register_lib_handler`), following
/// the same make_syscall argument convention.
///
/// Arguments:
///   arg1 = target_cage_id     -- cage whose library calls are being intercepted
///   arg2 = lib_name_ptr       -- host pointer to a NUL-terminated library name
///   arg3 = symbol_name_ptr    -- host pointer to a NUL-terminated symbol name
///   arg4 = handler_cage_id    -- grate cage that will handle the call
///   arg5 = adapter_export_ptr -- host pointer to the generated adapter's
///                                exported wasm function name (NUL-terminated)
///   arg6 = signature_desc_ptr -- host pointer to a NUL-terminated signature
///                                descriptor (see `parse_v2_signature_desc`)
pub fn register_lib_handler_v2(
    _self_cageid: u64,
    _target_cageid: u64,
    target_cage_id: u64,
    _arg1cage: u64,
    lib_name_ptr: u64,
    _arg2cage: u64,
    symbol_name_ptr: u64,
    _arg3cage: u64,
    handler_cage_id: u64,
    _arg4cage: u64,
    adapter_export_ptr: u64,
    _arg5cage: u64,
    signature_desc_ptr: u64,
    _arg6cage: u64,
) -> i32 {
    if lib_name_ptr == 0
        || symbol_name_ptr == 0
        || adapter_export_ptr == 0
        || signature_desc_ptr == 0
    {
        eprintln!("[3i|register_lib_handler_v2] null string pointer");
        return -1;
    }

    let read_cstr = |ptr: u64, what: &str| -> Option<String> {
        match unsafe { std::ffi::CStr::from_ptr(ptr as *const i8).to_str() } {
            Ok(s) => Some(s.to_string()),
            Err(_) => {
                eprintln!("[3i|register_lib_handler_v2] invalid {what} UTF-8");
                None
            }
        }
    };

    let Some(lib_name) = read_cstr(lib_name_ptr, "lib_name") else {
        return -1;
    };
    let Some(symbol_name) = read_cstr(symbol_name_ptr, "symbol_name") else {
        return -1;
    };
    let Some(adapter_export) = read_cstr(adapter_export_ptr, "adapter_export") else {
        return -1;
    };
    let Some(signature_desc) = read_cstr(signature_desc_ptr, "signature_desc") else {
        return -1;
    };

    let Some((manifest_version, signature, callback_params)) =
        parse_v2_signature_desc(&signature_desc)
    else {
        eprintln!(
            "[3i|register_lib_handler_v2] malformed signature descriptor: {signature_desc:?}"
        );
        return -1;
    };

    if let Some(reason) = reject_unsupported_callback_shape(&callback_params) {
        eprintln!("[3i|register_lib_handler_v2] {reason}");
        return -1;
    }

    register_lib_handler_v2_entry(
        target_cage_id,
        &lib_name,
        &symbol_name,
        V2Registration {
            grate_cage: handler_cage_id,
            adapter_export,
            manifest_version,
            signature,
            callback_params,
        },
    );

    0
}

#[cfg(test)]
mod signature_desc_tests {
    use super::*;

    #[test]
    fn no_fourth_segment_means_no_callback_params() {
        let (version, sig, callback_params) = parse_v2_signature_desc("1:ii:i").unwrap();
        assert_eq!(version, 1);
        assert_eq!(sig.params, vec![V2ValueType::I32, V2ValueType::I32]);
        assert_eq!(sig.results, vec![V2ValueType::I32]);
        assert_eq!(callback_params, Vec::<CallbackArgContract>::new());
    }

    #[test]
    fn empty_fourth_segment_means_no_callback_params() {
        let (_, _, callback_params) = parse_v2_signature_desc("1:i:").unwrap();
        assert!(callback_params.is_empty());
    }

    // --- schema round-trip: every declarable shape parses back out exactly ---

    #[test]
    fn round_trip_void_during_call_nonnullable() {
        let (_, _, cbs) = parse_v2_signature_desc("1:i::0@i@@D@0@same_thread_only").unwrap();
        assert_eq!(cbs.len(), 1);
        let sig = &cbs[0].signature;
        assert_eq!(cbs[0].arg_index, 0);
        assert_eq!(
            sig.params,
            vec![CallbackParamKind::Scalar(V2ValueType::I32)]
        );
        assert_eq!(sig.ret, CallbackRetKind::Void);
        assert_eq!(sig.lifetime, CallbackLifetime::DuringCall);
        assert!(!sig.nullable);
        assert_eq!(sig.reentry_policy, ReentryPolicy::SameThreadOnly);
    }

    #[test]
    fn round_trip_every_scalar_param_and_result_type() {
        let (_, _, cbs) = parse_v2_signature_desc("1:i::0@ilfd@d@D@1@same_thread_only").unwrap();
        let sig = &cbs[0].signature;
        assert_eq!(
            sig.params,
            vec![
                CallbackParamKind::Scalar(V2ValueType::I32),
                CallbackParamKind::Scalar(V2ValueType::I64),
                CallbackParamKind::Scalar(V2ValueType::F32),
                CallbackParamKind::Scalar(V2ValueType::F64),
            ]
        );
        assert_eq!(sig.ret, CallbackRetKind::Scalar(V2ValueType::F64));
        assert!(sig.nullable);
    }

    #[test]
    fn round_trip_retained_lifetime_parses_even_though_rejected_later() {
        // Parsing and accepting are different checks -- see
        // reject_unsupported_callback_shape_tests below for the rejection.
        let (_, _, cbs) = parse_v2_signature_desc("1:i::0@i@@R@0@same_thread_only").unwrap();
        assert_eq!(cbs[0].signature.lifetime, CallbackLifetime::Retained);
    }

    #[test]
    fn round_trip_pointer_param_and_function_pointer_result_parse_even_though_rejected_later() {
        let (_, _, cbs) = parse_v2_signature_desc("1:i::0@p@p@D@0@same_thread_only").unwrap();
        let sig = &cbs[0].signature;
        assert_eq!(sig.params, vec![CallbackParamKind::Pointer]);
        assert_eq!(sig.ret, CallbackRetKind::FunctionPointer);
    }

    #[test]
    fn multiple_distinct_callback_specs_are_accepted() {
        let (_, _, cbs) =
            parse_v2_signature_desc("1:ii::0@i@@D@0@same_thread_only,1@l@@D@0@same_thread_only")
                .unwrap();
        assert_eq!(cbs.len(), 2);
        assert_eq!(cbs[0].arg_index, 0);
        assert_eq!(cbs[1].arg_index, 1);
    }

    // --- malformed input ---

    #[test]
    fn out_of_range_callback_index_is_rejected() {
        assert!(parse_v2_signature_desc("1:i::1@i@@D@0@same_thread_only").is_none());
        assert!(parse_v2_signature_desc("1:i::99@i@@D@0@same_thread_only").is_none());
    }

    #[test]
    fn duplicate_callback_index_is_rejected() {
        // Rejected rather than silently deduplicated: the first occurrence
        // would replace the argument with a local proxy index before the
        // second occurrence runs, so the second pass would misread that
        // proxy index as another raw caller-side table index.
        assert!(parse_v2_signature_desc(
            "1:ii::0@i@@D@0@same_thread_only,0@i@@D@0@same_thread_only"
        )
        .is_none());
    }

    #[test]
    fn non_i32_outer_arg_targeted_by_callback_is_rejected() {
        assert!(parse_v2_signature_desc("1:l::0@i@@D@0@same_thread_only").is_none());
        assert!(parse_v2_signature_desc("1:f::0@i@@D@0@same_thread_only").is_none());
        assert!(parse_v2_signature_desc("1:d::0@i@@D@0@same_thread_only").is_none());
    }

    #[test]
    fn missing_spec_segments_are_rejected() {
        assert!(parse_v2_signature_desc("1:i::0@i@@D@0").is_none()); // no reentry_policy
        assert!(parse_v2_signature_desc("1:i::0").is_none()); // bare index, old grammar
    }

    #[test]
    fn unknown_callback_param_char_is_rejected() {
        assert!(parse_v2_signature_desc("1:i::0@x@@D@0@same_thread_only").is_none());
    }

    #[test]
    fn multi_character_callback_ret_is_rejected() {
        // A callback, like the outer V2 transport, carries at most one result.
        assert!(parse_v2_signature_desc("1:i::0@i@ii@D@0@same_thread_only").is_none());
    }

    #[test]
    fn unknown_callback_ret_char_is_rejected() {
        assert!(parse_v2_signature_desc("1:i::0@i@x@D@0@same_thread_only").is_none());
    }

    #[test]
    fn bad_lifetime_char_is_rejected() {
        assert!(parse_v2_signature_desc("1:i::0@i@@X@0@same_thread_only").is_none());
    }

    #[test]
    fn bad_nullable_char_is_rejected() {
        assert!(parse_v2_signature_desc("1:i::0@i@@D@2@same_thread_only").is_none());
    }

    #[test]
    fn empty_reentry_policy_is_rejected() {
        assert!(parse_v2_signature_desc("1:i::0@i@@D@0@").is_none());
    }

    #[test]
    fn unrecognized_reentry_policy_is_rejected() {
        // A closed vocabulary (ReentryPolicy), not a recorded-but-
        // uninterpreted string: a plausible-looking but unimplemented
        // policy name must be rejected the same as outright garbage.
        assert!(parse_v2_signature_desc("1:i::0@i@@D@0@any_thread").is_none());
        assert!(parse_v2_signature_desc("1:i::0@i@@D@0@not-alnum").is_none());
    }
}

#[cfg(test)]
mod reject_unsupported_callback_shape_tests {
    use super::*;

    fn contract(signature: CallbackSignature) -> CallbackArgContract {
        CallbackArgContract {
            arg_index: 0,
            signature,
        }
    }

    fn supported_signature() -> CallbackSignature {
        CallbackSignature {
            params: vec![CallbackParamKind::Scalar(V2ValueType::I32)],
            ret: CallbackRetKind::Void,
            lifetime: CallbackLifetime::DuringCall,
            nullable: false,
            reentry_policy: ReentryPolicy::SameThreadOnly,
        }
    }

    #[test]
    fn fully_supported_shape_is_accepted() {
        assert!(reject_unsupported_callback_shape(&[contract(supported_signature())]).is_none());
    }

    #[test]
    fn pointer_param_is_rejected() {
        let mut sig = supported_signature();
        sig.params.push(CallbackParamKind::Pointer);
        let reason = reject_unsupported_callback_shape(&[contract(sig)]).unwrap();
        assert!(reason.contains("pointer-bearing callback parameters"));
    }

    #[test]
    fn function_pointer_result_is_rejected() {
        let mut sig = supported_signature();
        sig.ret = CallbackRetKind::FunctionPointer;
        let reason = reject_unsupported_callback_shape(&[contract(sig)]).unwrap();
        assert!(reason.contains("function pointer"));
    }

    #[test]
    fn retained_lifetime_is_rejected() {
        let mut sig = supported_signature();
        sig.lifetime = CallbackLifetime::Retained;
        let reason = reject_unsupported_callback_shape(&[contract(sig)]).unwrap();
        assert!(reason.contains("retained"));
    }
}
