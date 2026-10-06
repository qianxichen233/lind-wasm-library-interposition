// Focused tests for the cross-cage callback re-entry mechanism
// (`wasmtime::ActiveFrameGuard`/`with_active_frame`): the thread-local stack
// that lets a grate's callback proxy call back into a suspended caller's
// own `Store` without ever acquiring a second live reference to it. An
// integration test (not an in-crate `#[cfg(test)]` module) so it only needs
// the library's public API, not wasmtime's own differently-feature-gated
// internal test suite.

use wasmtime::{AsContextMut, Ref, RefType, Store, Table, TableType};

fn new_store_and_table() -> (Store<()>, Table) {
    let mut store = Store::<()>::default();
    let ty = TableType::new(RefType::FUNCREF, 1, None);
    let table = Table::new(&mut store, ty, Ref::Func(None)).unwrap();
    (store, table)
}

// `Table` has no cross-store equality check, so tests that need to tell two
// distinct tables apart grow them to distinguishable sizes instead and
// compare `size()` inside the `with_active_frame` closure, where a table is
// always paired with the one store it actually belongs to.
fn new_store_and_table_with_size(extra: u64) -> (Store<()>, Table) {
    let (mut store, table) = new_store_and_table();
    table.grow(&mut store, extra, Ref::Func(None)).unwrap();
    (store, table)
}

#[test]
fn lookup_with_no_active_frame_returns_none() {
    let result = wasmtime::with_active_frame::<(), _>(42, |_store, _table| ());
    assert!(result.is_none());
}

#[test]
fn normal_push_then_pop_leaves_the_stack_empty() {
    let (mut store, table) = new_store_and_table();
    {
        let _guard = wasmtime::ActiveFrameGuard::push(1, table, &mut store.as_context_mut());
        assert!(wasmtime::with_active_frame::<(), _>(1, |_s, _t| ()).is_some());
    }
    assert!(wasmtime::with_active_frame::<(), _>(1, |_s, _t| ()).is_none());
}

#[test]
fn nested_frames_for_different_cages_resolve_independently() {
    let (mut store_a, table_a) = new_store_and_table();
    let (mut store_b, table_b) = new_store_and_table();
    let _guard_a = wasmtime::ActiveFrameGuard::push(1, table_a, &mut store_a.as_context_mut());
    let _guard_b = wasmtime::ActiveFrameGuard::push(2, table_b, &mut store_b.as_context_mut());

    assert!(wasmtime::with_active_frame::<(), _>(1, |_s, _t| ()).is_some());
    assert!(wasmtime::with_active_frame::<(), _>(2, |_s, _t| ()).is_some());
    assert!(wasmtime::with_active_frame::<(), _>(3, |_s, _t| ()).is_none());
}

#[test]
fn same_cage_nested_frames_resolve_to_the_innermost() {
    // A cage calling back into itself through two hops (e.g. a callback
    // that triggers another interposed call) pushes a second frame for the
    // SAME cageid. Lookups must resolve to the most recently pushed one,
    // and popping the inner one must restore visibility of the outer one.
    let (mut store_outer, table_outer) = new_store_and_table_with_size(1); // size 2
    let (mut store_inner, table_inner) = new_store_and_table_with_size(9); // size 10
    let outer = wasmtime::ActiveFrameGuard::push(7, table_outer, &mut store_outer.as_context_mut());

    {
        let _inner =
            wasmtime::ActiveFrameGuard::push(7, table_inner, &mut store_inner.as_context_mut());
        let size = wasmtime::with_active_frame::<(), _>(7, |s, t| t.size(&s)).unwrap();
        assert_eq!(size, 10);
    }

    let size = wasmtime::with_active_frame::<(), _>(7, |s, t| t.size(&s)).unwrap();
    assert_eq!(size, 2);
    drop(outer);
}

#[test]
fn panic_while_a_frame_is_active_still_pops_it() {
    let (mut store, table) = new_store_and_table();
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let _guard = wasmtime::ActiveFrameGuard::push(5, table, &mut store.as_context_mut());
        panic!("simulated trap propagating through a callback");
    }));
    assert!(result.is_err());
    assert!(wasmtime::with_active_frame::<(), _>(5, |_s, _t| ()).is_none());
}

#[test]
fn out_of_order_guard_destruction_panics_instead_of_corrupting_the_stack() {
    let (mut store_a, table_a) = new_store_and_table();
    let (mut store_b, table_b) = new_store_and_table();
    let guard_a = wasmtime::ActiveFrameGuard::push(1, table_a, &mut store_a.as_context_mut());
    let guard_b = wasmtime::ActiveFrameGuard::push(2, table_b, &mut store_b.as_context_mut());

    // Dropping the OUTER guard while the INNER one is still alive violates
    // the LIFO nesting this module's safety depends on -- it must panic
    // loudly, in every build profile, rather than silently popping the
    // wrong frame.
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        drop(guard_a);
    }));
    assert!(result.is_err());

    // guard_b's own token no longer matches the (already-corrupted) top of
    // the stack either; forget it rather than let a second, uninteresting
    // panic escape this test during normal unwinding.
    std::mem::forget(guard_b);
}

#[test]
fn wrong_host_state_type_is_treated_as_no_active_frame() {
    let (mut store, table) = new_store_and_table();
    let _guard = wasmtime::ActiveFrameGuard::push(9, table, &mut store.as_context_mut());

    // Pushed under T = (), looked up under T = u32: a real type mismatch
    // is a caller bug, not a cross-cage safety issue, so it is reported the
    // same way as "no frame" rather than panicking.
    assert!(wasmtime::with_active_frame::<u32, _>(9, |_s, _t| ()).is_none());
    assert!(wasmtime::with_active_frame::<(), _>(9, |_s, _t| ()).is_some());
}
