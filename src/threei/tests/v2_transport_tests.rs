// Host transport tests (issue #22) for the V2 (variable-width) library-call
// path in threei::lib_call_v2. These tests dispatch directly through
// dispatch_lib_call_v2 against a mock trampoline registered for a private
// test runtime id -- there is no real Wasm grate involved here by design,
// proving this module's own dispatch/bookkeeping in isolation from
// Wasmtime; see wasmtime_lind_3i's v2_adapter_tests.rs for the real-grate
// resolution/validation/call path this layer sits underneath.
//
// Compiled as its own test binary/process (Rust's default per-file test
// harness), so it does not share global state with, or inherit failures
// from, handler_tests.rs / make_syscall_tests.rs.
use std::collections::hash_map::DefaultHasher;
use std::hash::{Hash, Hasher};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64};
use std::sync::Arc;

// cage::Cage's fields are built against dashmap 5.x / parking_lot, not this
// crate's own dashmap 6.x / std sync types -- see Cargo.toml's dashmap5 alias.
use dashmap5::DashMap;
use parking_lot::{Mutex, RwLock};
use serial_test::serial;

use threei::{
    check_v2_request_bounds, dispatch_lib_call_v2, get_cage_runtime, register_trampoline_v2,
    set_cage_runtime, set_last_v2_diagnostic, V2Arg, V2Outcome, V2RawArg, V2Request, V2Value,
    V2ValueType, V2_ABI_VERSION, V2_MAX_ARGS, V2_OUTCOME_OK, V2_OUTCOME_REJECTED,
    V2_OUTCOME_TRAPPED,
};

// Any runtime id distinct from RUNTIME_TYPE_WASMTIME (1); tests only ever
// dispatch to the mock trampolines registered below, never a real runtime.
const TEST_RUNTIME: u64 = 9001;

fn ensure_test_cage(cageid: u64) {
    cage::cagetable_init();
    if cage::with_cage(cageid, |_| ()).is_none() {
        let test_cage = cage::Cage {
            cageid,
            parent: 0,
            cwd: RwLock::new(Arc::new(PathBuf::from("/"))),
            rev_shm: Mutex::new(Vec::new()),
            signalhandler: DashMap::new(),
            sigset: AtomicU64::new(0),
            pending_signals: RwLock::new(vec![]),
            epoch_handler: DashMap::new(),
            os_tid_map: DashMap::new(),
            main_threadid: RwLock::new(0),
            interval_timer: cage::IntervalTimer::new(cageid),
            zombies: RwLock::new(vec![]),
            child_num: AtomicU64::new(0),
            vmmap: RwLock::new(cage::Vmmap::new()),
            final_exit_status: RwLock::new(None),
            exit_group_initiated: AtomicBool::new(false),
            is_dead: AtomicBool::new(false),
            grate_inflight: AtomicU64::new(0),
        };
        cage::add_cage(cageid, test_cage);
    }
    if get_cage_runtime(cageid).is_none() {
        set_cage_runtime(cageid, TEST_RUNTIME);
    }
}

/// Reads `args_ptr[..argc]` into an owned `Vec` immediately -- the pointer
/// is only valid for the duration of this call (see GrateTrampolineFnV2's
/// doc). Shared by both mock trampolines below.
fn copy_args(args_ptr: *const V2RawArg, argc: u64) -> Vec<V2RawArg> {
    if argc == 0 {
        return Vec::new();
    }
    unsafe { std::slice::from_raw_parts(args_ptr, argc as usize) }.to_vec()
}

/// Echoes the LAST argument's type and exact bits back as the result.
/// Used to prove bit-exact (including NaN payload / high-bit) round trips
/// at varying argument counts and positions.
extern "C" fn echo_last_arg_trampoline(
    _handler_id: u64,
    _grateid: u64,
    _caller_cage: u64,
    abi_version: u32,
    _signature_id: u64,
    args_ptr: *const V2RawArg,
    argc: u64,
    expects_result: u8,
    out_ty: *mut u8,
    out_bits: *mut u64,
) -> i32 {
    if abi_version != V2_ABI_VERSION {
        set_last_v2_diagnostic(Some(format!("unexpected abi_version {abi_version}")));
        return V2_OUTCOME_REJECTED;
    }
    let args = copy_args(args_ptr, argc);
    if expects_result == 1 {
        let last = match args.last() {
            Some(a) => a,
            None => {
                set_last_v2_diagnostic(Some("no argument to echo".to_string()));
                return V2_OUTCOME_REJECTED;
            }
        };
        unsafe {
            *out_ty = last.ty;
            *out_bits = last.bits;
        }
    }
    V2_OUTCOME_OK
}

/// Folds every argument's raw bits (rotated by its own index) into one
/// result, so any single argument -- at any position, including 7 and 14 --
/// independently changes the result. Also XORs `argc` itself in, so calls
/// that differ only in argument COUNT (with the tail zero-padded) still
/// produce different results.
extern "C" fn combine_args_trampoline(
    _handler_id: u64,
    _grateid: u64,
    _caller_cage: u64,
    abi_version: u32,
    _signature_id: u64,
    args_ptr: *const V2RawArg,
    argc: u64,
    expects_result: u8,
    out_ty: *mut u8,
    out_bits: *mut u64,
) -> i32 {
    if abi_version != V2_ABI_VERSION {
        set_last_v2_diagnostic(Some(format!("unexpected abi_version {abi_version}")));
        return V2_OUTCOME_REJECTED;
    }
    let args = copy_args(args_ptr, argc);
    let mut acc: u64 = argc.wrapping_mul(0x9E37_79B9_7F4A_7C15);
    for (i, a) in args.iter().enumerate() {
        let mut h = DefaultHasher::new();
        (a.ty, a.bits, a.cageid, i as u64).hash(&mut h);
        acc ^= h.finish();
    }
    if expects_result == 1 {
        unsafe {
            *out_ty = V2ValueType::I64 as u8;
            *out_bits = acc;
        }
    }
    V2_OUTCOME_OK
}

extern "C" fn trapping_trampoline(
    _handler_id: u64,
    _grateid: u64,
    _caller_cage: u64,
    _abi_version: u32,
    _signature_id: u64,
    _args_ptr: *const V2RawArg,
    _argc: u64,
    _expects_result: u8,
    _out_ty: *mut u8,
    _out_bits: *mut u64,
) -> i32 {
    set_last_v2_diagnostic(Some("mock handler trapped".to_string()));
    V2_OUTCOME_TRAPPED
}

fn scalar_args(n: usize) -> Vec<V2Arg> {
    (0..n)
        .map(|i| V2Arg {
            value: V2Value::i32(i as i32 + 1),
            cageid: 100,
        })
        .collect()
}

fn base_request(args: Vec<V2Arg>, expect_result: bool) -> V2Request {
    V2Request {
        abi_version: V2_ABI_VERSION,
        handler_id: 0xDEAD_BEEF,
        caller_cage: 42,
        signature_id: 7,
        args,
        expected_results: if expect_result {
            vec![V2ValueType::I64]
        } else {
            vec![]
        },
    }
}

#[test]
#[serial]
fn echoes_exact_bits_at_0_1_6_7_14_and_larger_argc() {
    ensure_test_cage(1);
    register_trampoline_v2(TEST_RUNTIME, echo_last_arg_trampoline);

    // A deliberately awkward set of values: a signaling-bit-set NaN payload,
    // a canonical quiet NaN, the high bit of an i32/i64 set, and a negative
    // zero -- all values whose bit pattern a naive f32/f64 round trip (or an
    // accidental sign/zero-extension) could silently corrupt.
    let tricky: &[V2Value] = &[
        V2Value::i32(-1),       // 0xFFFFFFFF: every bit set
        V2Value::i32(i32::MIN), // high bit alone
        V2Value::i64(i64::MIN),
        V2Value::f32_bits(0x7FA0_0001), // signaling NaN payload
        V2Value::f32_bits(0xFF80_0000), // -infinity
        V2Value::f64_bits(0xFFF8_0000_0000_0001), // negative NaN, payload=1
        V2Value::f64_bits(0x8000_0000_0000_0000), // negative zero
    ];

    for &argc in &[0usize, 1, 6, 7, 14, 200] {
        for &tail in tricky {
            let mut args = scalar_args(argc.saturating_sub(1));
            args.push(V2Arg {
                value: tail,
                cageid: 7,
            });
            if argc == 0 {
                args.clear();
            }
            let expect_result = argc > 0;
            let req = base_request(args, expect_result);
            match dispatch_lib_call_v2(1, req) {
                V2Outcome::Ok(results) => {
                    if argc == 0 {
                        assert!(results.is_empty(), "argc=0 must produce no result");
                    } else {
                        assert_eq!(results.len(), 1, "argc={argc} expected exactly one result");
                        assert_eq!(
                            results[0], tail,
                            "argc={argc} tail={tail:?}: echoed value must match bit-for-bit"
                        );
                    }
                }
                other => panic!("argc={argc} tail={tail:?}: expected Ok, got {other:?}"),
            }
        }
    }
}

#[test]
#[serial]
fn argument_7_and_14_each_independently_affect_the_result() {
    ensure_test_cage(2);
    register_trampoline_v2(TEST_RUNTIME, combine_args_trampoline);

    let n = 20;
    let baseline = scalar_args(n);
    let call = |args: Vec<V2Arg>| -> u64 {
        match dispatch_lib_call_v2(2, base_request(args, true)) {
            V2Outcome::Ok(results) => {
                assert_eq!(results.len(), 1);
                assert_eq!(results[0].ty, V2ValueType::I64);
                results[0].bits
            }
            other => panic!("expected Ok, got {other:?}"),
        }
    };

    let base_result = call(baseline.clone());

    for &flip_index in &[6usize, 13] {
        // 0-indexed 6 and 13 are the 7th and 14th arguments -- one and two
        // slots past V1's fixed six-argument transport limit.
        let mut args = baseline.clone();
        args[flip_index].value = V2Value::i32(args[flip_index].value.bits as i32 + 1000);
        let flipped_result = call(args);
        assert_ne!(
            flipped_result,
            base_result,
            "flipping argument index {flip_index} (the {}th argument) must change the result",
            flip_index + 1
        );
    }
}

#[test]
#[serial]
fn zero_and_one_arg_calls_work() {
    ensure_test_cage(3);
    register_trampoline_v2(TEST_RUNTIME, combine_args_trampoline);

    let zero = dispatch_lib_call_v2(3, base_request(vec![], false));
    assert!(
        matches!(zero, V2Outcome::Ok(ref r) if r.is_empty()),
        "{zero:?}"
    );

    let one = dispatch_lib_call_v2(3, base_request(scalar_args(1), true));
    assert!(
        matches!(one, V2Outcome::Ok(ref r) if r.len() == 1),
        "{one:?}"
    );
}

#[test]
#[serial]
fn request_over_the_argument_bound_is_rejected_before_dispatch() {
    ensure_test_cage(4);
    // No trampoline registered for cage 4's runtime at all -- if the bound
    // check didn't fire before dispatch, this would fail with a *different*
    // rejection reason ("no V2 trampoline registered..."), which the
    // message assertion below distinguishes from the intended bound failure.
    let over = V2_MAX_ARGS + 1;
    let req = base_request(scalar_args(over), false);
    match dispatch_lib_call_v2(4, req) {
        V2Outcome::Rejected(msg) => {
            assert!(
                msg.contains(&format!("{over}")) && msg.contains("bound"),
                "expected a precise over-bound diagnostic, got: {msg}"
            );
        }
        other => panic!("expected Rejected, got {other:?}"),
    }
}

#[test]
fn bounds_helper_rejects_over_limit_args_and_results() {
    assert!(check_v2_request_bounds(0, 0).is_ok());
    assert!(check_v2_request_bounds(V2_MAX_ARGS, 1).is_ok());
    assert!(check_v2_request_bounds(V2_MAX_ARGS + 1, 0).is_err());
    assert!(check_v2_request_bounds(0, 2).is_err());
}

#[test]
#[serial]
fn trap_is_distinguished_from_rejection() {
    ensure_test_cage(5);
    register_trampoline_v2(TEST_RUNTIME, trapping_trampoline);

    match dispatch_lib_call_v2(5, base_request(scalar_args(3), false)) {
        V2Outcome::Trapped(msg) => assert_eq!(msg, "mock handler trapped"),
        other => panic!("expected Trapped, got {other:?}"),
    }
}

#[test]
#[serial]
fn dispatch_to_nonexistent_cage_is_rejected_not_panicked() {
    let req = base_request(scalar_args(2), false);
    match dispatch_lib_call_v2(999_999, req) {
        V2Outcome::Rejected(msg) => assert!(msg.contains("does not exist"), "{msg}"),
        other => panic!("expected Rejected, got {other:?}"),
    }
}

#[test]
#[serial]
fn dead_cage_is_rejected() {
    ensure_test_cage(6);
    cage::with_cage(6, |c| {
        c.is_dead.store(true, std::sync::atomic::Ordering::Release)
    });
    let req = base_request(scalar_args(2), false);
    match dispatch_lib_call_v2(6, req) {
        V2Outcome::Rejected(msg) => assert!(msg.contains("dead"), "{msg}"),
        other => panic!("expected Rejected, got {other:?}"),
    }
}
