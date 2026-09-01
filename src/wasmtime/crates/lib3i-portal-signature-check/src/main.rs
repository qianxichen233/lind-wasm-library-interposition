//! Runnable regression check for the lib-3i portal's install-time signature
//! validation (issue #13). See this crate's Cargo.toml for why it exists as
//! a standalone binary using raw WAT: v128, funcref/externref, and
//! multi-result signatures cannot be produced by any C source the Lind
//! toolchain compiles, so `tests/grate-tests/lib-interpose`'s C-based
//! fail-closed suite structurally cannot reach these shapes.
//!
//! This exercises the REAL `Linker::instance_dylink` portal-install code
//! path (the same one `wasmtime-lind-dylink`/lind-boot calls for every real
//! cage), not a reimplementation of its logic -- only the surrounding cage
//! bootstrap (a real running grate process for `dispatch_lib_call` to
//! reach) is out of scope here, so "supported" cases are verified by
//! confirming dispatch was genuinely attempted (see `run_case`'s doc)
//! rather than by completing a full round trip through a real handler.
//!
//! Run: `cargo run -p lib3i-portal-signature-check`
//! (from src/wasmtime/; needs no special feature flags of its own -- the
//! `wat` feature this crate needs from `wasmtime` is pulled in by its own
//! Cargo.toml, independent of lind-boot's normal, `wat`-free feature set)

use wasmtime::error::Context;
use wasmtime::{bail, ensure, Config, Engine, Instance, Linker, Module, Result, Store, Val, V128};

// A handler cage id that intentionally does not exist. dispatch_lib_call's
// first step (threei::with_cage) looks up this id and fails gracefully
// (ESRCH) if there's no real cage -- so calling through a portal that
// genuinely reached dispatch_lib_call returns an ordinary value (whatever
// ESRCH encodes to), while a portal that rejected the signature up front
// traps instead, with the "cannot be transported" message. That gap is
// what each check below distinguishes: reaching dispatch at all (a normal
// return, not a trap) is exactly the evidence that the real handler
// function this fake handler_cage_id/fn_ptr stands in for was never (and,
// for accepted signatures, would legitimately have been) entered.
const FAKE_CALLER_CAGE_ID: u64 = 0x5151_0001;
const FAKE_HANDLER_CAGE_ID: u64 = 0x5151_0002;
const FAKE_FN_PTR: u64 = 0xDEAD_BEEF;

const LIB_WAT: &str = r#"
(module
  (func (export "unsupported_v128_param") (param v128) (result i32)
    i32.const 0)
  (func (export "unsupported_v128_result") (param i32) (result v128)
    v128.const i32x4 0 0 0 0)
  (func (export "unsupported_funcref_param") (param funcref) (result i32)
    i32.const 0)
  (func (export "unsupported_funcref_result") (param i32) (result funcref)
    ref.null func)
  (func (export "unsupported_externref_param") (param externref) (result i32)
    i32.const 0)
  (func (export "unsupported_externref_result") (param i32) (result externref)
    ref.null extern)
  (func (export "multi_result") (param i32) (result i32 i32)
    local.get 0
    local.get 0)
  (func (export "zero_result") (param i32)
    nop)
  (func (export "single_result") (param i32 i32) (result i32)
    local.get 0
    local.get 1
    i32.add)
)
"#;

struct Case {
    symbol: &'static str,
    args: Vec<Val>,
    /// Some((reason substring, full formatted signature)) if this signature
    /// must be rejected at install time -- the trap message must contain
    /// both; None if it must be accepted (dispatch genuinely attempted).
    must_reject: Option<(&'static str, &'static str)>,
}

fn main() -> Result<()> {
    let mut config = Config::new();
    config.wasm_simd(true);
    config.wasm_reference_types(true);
    let engine = Engine::new(&config).context("building engine")?;
    let mut store = Store::new(&engine, ());

    let lib_module = Module::new(&engine, LIB_WAT).context("parsing fixture WAT")?;
    // Sanity-instantiate once against the throwaway store above, just to
    // confirm the fixture module itself is well-formed before looping.
    Instance::new(&mut store, &lib_module, &[]).context("instantiating fixture WAT")?;

    let cases = [
        Case {
            symbol: "unsupported_v128_param",
            args: vec![Val::V128(V128::from(0))],
            must_reject: Some(("param 0 has type v128", "(v128) -> (i32)")),
        },
        Case {
            symbol: "unsupported_v128_result",
            args: vec![Val::I32(0)],
            must_reject: Some(("result 0 has type v128", "(i32) -> (v128)")),
        },
        // funcref/externref are reference types, distinct from v128 (a
        // vector type) -- reference-type handling can evolve independently
        // (e.g. the GC proposal), so it's covered by its own cases rather
        // than assumed to follow the same code path as v128.
        Case {
            symbol: "unsupported_funcref_param",
            args: vec![Val::FuncRef(None)],
            must_reject: Some((
                "param 0 has type (ref null func)",
                "((ref null func)) -> (i32)",
            )),
        },
        Case {
            symbol: "unsupported_funcref_result",
            args: vec![Val::I32(0)],
            must_reject: Some((
                "result 0 has type (ref null func)",
                "(i32) -> ((ref null func))",
            )),
        },
        Case {
            symbol: "unsupported_externref_param",
            args: vec![Val::ExternRef(None)],
            must_reject: Some((
                "param 0 has type (ref null extern)",
                "((ref null extern)) -> (i32)",
            )),
        },
        Case {
            symbol: "unsupported_externref_result",
            args: vec![Val::I32(0)],
            must_reject: Some((
                "result 0 has type (ref null extern)",
                "(i32) -> ((ref null extern))",
            )),
        },
        Case {
            symbol: "multi_result",
            args: vec![Val::I32(0)],
            must_reject: Some(("2 results, exceeding", "(i32) -> (i32, i32)")),
        },
        Case {
            symbol: "zero_result",
            args: vec![Val::I32(0)],
            must_reject: None,
        },
        Case {
            symbol: "single_result",
            args: vec![Val::I32(1), Val::I32(2)],
            must_reject: None,
        },
    ];

    let mut fail = 0;
    for case in &cases {
        if let Err(e) = run_case(&engine, &lib_module, case) {
            println!("FAIL  {}: {e:#}", case.symbol);
            fail += 1;
        } else {
            println!("PASS  {}", case.symbol);
        }
    }

    println!();
    if fail == 0 {
        println!("Results: {} passed, 0 failed", cases.len());
        Ok(())
    } else {
        println!("Results: {} passed, {fail} failed", cases.len() - fail);
        std::process::exit(1);
    }
}

fn run_case(engine: &Engine, lib_module: &Module, case: &Case) -> Result<()> {
    // Every real handler registration a grate performs goes through this
    // exact entry point (threei::register_lib_handler wraps it for the
    // syscall ABI; this is the plain-Rust core it calls into) -- so
    // get_lib_handler, which Linker::instance_dylink's portal-install code
    // queries, sees this the same way it would see a registration made by
    // a real interposing grate.
    threei::lib_handler_table::register_lib_handler_entry(
        FAKE_CALLER_CAGE_ID,
        "env",
        case.symbol,
        FAKE_HANDLER_CAGE_ID,
        FAKE_FN_PTR,
    );

    let mut store = Store::new(engine, ());
    let lib_instance =
        Instance::new(&mut store, lib_module, &[]).context("instantiating fixture WAT")?;
    let mut linker = Linker::<()>::new(engine);
    linker
        .instance_dylink(
            &mut store,
            "env",
            lib_instance,
            Some(FAKE_CALLER_CAGE_ID),
            vec![],
            None,
            false,
        )
        .context("instance_dylink (portal install)")?;

    let func = linker
        .get(&mut store, "env", case.symbol)
        .with_context(|| format!("{} was not linked as env.{}", case.symbol, case.symbol))?
        .into_func()
        .with_context(|| format!("env.{} did not link as a function", case.symbol))?;

    let ty = func.ty(&store);
    let mut results = vec![Val::I32(0); ty.results().len()];
    let call_result = func.call(&mut store, &case.args, &mut results);

    match (&case.must_reject, call_result) {
        (Some((reason, signature)), Err(e)) => {
            let msg = format!("{e:#}");
            ensure!(
                msg.contains("cannot be transported") && msg.contains(reason),
                "expected a rejection containing {reason:?}, got: {msg}"
            );
            ensure!(
                msg.contains(&format!("env.{}", case.symbol)),
                "diagnostic missing symbol name: {msg}"
            );
            ensure!(
                msg.contains(signature),
                "diagnostic missing full signature {signature:?}: {msg}"
            );
            Ok(())
        }
        (Some((reason, _)), Ok(())) => {
            bail!("expected rejection containing {reason:?}, but the call succeeded")
        }
        (None, Err(e)) => {
            bail!("expected this supported signature to reach dispatch, but it was rejected: {e:#}")
        }
        (None, Ok(())) => Ok(()),
    }
}
