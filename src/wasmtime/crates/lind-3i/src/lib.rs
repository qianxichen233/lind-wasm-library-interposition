//! This module provides a global runtime-state lookup mechanism for lind-3i and lind-wasm, enabling
//! controlled transfers of execution across cages, grates, and threads.
//!
//! In lind-wasm, runtime control is not always confined to a single Wasmtime instance or a single
//! linear call stack. There are two primary scenarios in which lind-3i must explicitly locate and
//! re-enter a different runtime state.
//!
//! Importantly, not all re-entries into Wasmtime are equivalent. Some operations require resuming
//! execution in the *same continuation context* (i.e., the same instance and asyncify state),
//! while others only require access to a compatible instance that shares the same linear memory.
//!
//! The mechanisms in this module distinguish between these cases explicitly.
//!
//! ---
//! ## Scenario 1: Process-like operations (`fork`, `exec`, `exit`)
//!
//! The first scenario occurs during process-like operations such as `fork`, `exec`, and `exit`. These
//! operations require Wasmtime to create, clone, or destroy existing Wasm instances. After RawPOSIX
//! completes the semantic handling of a `fork`, `exec`, or `exit` operation, execution must return to
//! Wasmtime to continue running Wasm code. Importantly, the cage that performs the `fork`, `exec`, or
//! `exit` logic is not necessarily the same cage or grate that originally issued the system call. As
//! a result, lind-3i cannot rely on an implicit “current” runtime state. Instead, it must be able to
//! retrieve the Wasmtime execution context.
//!
//! These operations are also continuation-sensitive, which means that the execution must resume in
//! the *same Wasmtime instance* that originally issued the syscall.
//!
//! In particular, `fork` / `exit` rely on Asyncify to suspend and later resume execution via paired
//! `start_unwind` / `stop_unwind` and `start_rewind` / `stop_rewind` operations. These transitions
//! must occur within the same continuation context, which is the same Wasmtime instance and
//! associated asyncify runtime state. Resuming execution in a different instance, even one that
//! shares linear memory, breaks this invariant and can result in missed callbacks or incorrect
//! return values.
//!
//! As a result, lind-3i must be able to retrieve *the active execution context* corresponding
//! to a specific `(cage_id, tid)` when handling these operations.
//!
//! ---
//! ## Scenario 2: Grate calls (cross-module execution transfers)
//!
//! The second scenario arises during grate calls. Grate calls involve cross-module execution transfers,
//! where control jumps from one Wasm module to another (for example, from a cage into a grate, or between
//! grates). Supporting these jumps similarly requires the ability to locate and enter the runtime state
//! of a different module than the one currently executing.
//!
//! Unlike fork, exec, and exit, grate calls are not continuation-sensitive. A grate call does not need
//! to resume execution in the exact Wasmtime instance that originally issued the transfer. Instead, it
//! only needs to enter a compatible execution context for the target grate. In lind-3i, this compatible
//! context is represented not as a single shared runtime state, but as a pool of independent grate workers.
//! Each worker consists of:
//! - its own Wasmtime `Store`,
//! - its own instantiated grate `Instance`, and
//! - its own independent Wasm call stack region.
//!
//! Operationally, a grate call is executed by leasing one worker from the target grate’s worker pool,
//! invoking the grate entry function inside that worker, and returning the worker to the pool when the
//! call finishes. This means that a grate call should be understood as a transfer into an available worker
//! context, rather than as a re-entry into one globally shared grate instance. This worker-based
//! structure is what makes grate-call concurrency possible.
//!
//! A Wasmtime `Store` is an execution boundary: it owns the runtime state associated with a particular
//! instance execution, including stack state and other mutable execution-local state. By giving each
//! worker its own `Store` and `Instance`, lind-3i ensures that concurrent grate calls do not execute
//! inside the same Wasmtime runtime context. As a result, parallel grate calls do not contend on a shared
//! Wasm call stack, do not overwrite each other’s instance-local execution state, and do not require
//! continuation matching of the kind needed by Asyncify-based process operations.
//!
//! This design also explains why grate calls use a different lookup mechanism from continuation-sensitive
//! operations. For fork / exit, lind-3i must recover the specific active execution context associated with
//! a given (cage_id, tid), because execution must resume in the same continuation. For grate calls, by
//! contrast, lind-3i only needs to obtain some available worker for the target grate, because correctness
//! depends on entering a compatible grate instance, not on resuming a previously suspended continuation.
pub mod v2_adapter;

use anyhow::Context;
use std::collections::{HashMap, VecDeque};
use std::env;
use std::ffi::c_void;
use std::ptr::NonNull;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Condvar, Mutex, MutexGuard, OnceLock};
use sysdefs::constants::lind_platform_const;
use sysdefs::constants::lind_platform_const::*;
use wasmtime::error::Context as WasmtimeContext;
use wasmtime::{
    Caller, Engine, Extern, Func, FuncType, Global, Instance, Linker, Module, Ref, Store,
    TypedFunc, Val, ValType,
};

type PassFptrTyped = TypedFunc<
    (
        u64,
        u64,
        u64,
        u64,
        u64,
        u64,
        u64,
        u64,
        u64,
        u64,
        u64,
        u64,
        u64,
        u64,
    ),
    i64,
>;

type WorkerId = u64;

const DEFAULT_GRATE_WORKERS: usize = MAX_GRATE_WORKERS;
const GRATE_WORKERS_ENV: &str = "LIND_GRATE_WORKERS";

/// Concurrency policy for a grate handler.
///
/// This determines whether multiple submitted grate calls may execute
/// concurrently on different workers, or whether entry must be serialized.
pub enum ConcurrencyMode {
    /// Allow multiple calls to execute concurrently as long as distinct
    /// workers are available in the pool.
    Parallel,

    /// A single call request is addressed at a time even though requests
    /// are made concurrently.
    Serialized,
}

/// Template used to instantiate grate workers.
///
/// A `GrateTemplate` contains the shared, immutable ingredients needed to
/// construct worker-local execution contexts for the same grate module.
/// Each worker clones or reuses these components to create its own
/// `Store + Instance` runtime state.
pub struct GrateTemplate<T> {
    /// The Wasmtime engine used to create worker-local stores and instances.
    ///
    /// This is shared across all workers for the same grate.
    pub engine: Engine,

    /// The compiled grate module that each worker instantiates.
    ///
    /// All workers created from the same template execute this same module,
    /// but do so inside independent stores / instances.
    pub module: Module,

    /// The linker used to instantiate the grate module and attach its imports.
    ///
    /// Each worker starts from this template linker and clones it during
    /// worker creation so that instantiation can proceed independently.
    pub linker: Linker<T>,
}

/// Marshalled arguments for one grate call.
///
/// A `GrateRequest` represents one cross-module execution transfer into a
/// grate worker. It includes the callee function pointer together with the
/// calling cage identity and up to six `(value, cageid)` argument pairs.
///
/// The `argNcageid` fields identify the ownership / address-space context
/// associated with pointer-like arguments, allowing the callee side to
/// interpret cross-cage values correctly.
pub struct GrateRequest {
    /// Address of the function ptr argument passed by the caller,
    /// used for indirect dispatch inside the grate.
    pub handler_addr: u64,
    /// Identity of the calling cage, used for invoking the correct handler.
    pub cageid: u64,
    pub arg1: u64,
    pub arg1cageid: u64,
    pub arg2: u64,
    pub arg2cageid: u64,
    pub arg3: u64,
    pub arg3cageid: u64,
    pub arg4: u64,
    pub arg4cageid: u64,
    pub arg5: u64,
    pub arg5cageid: u64,
    pub arg6: u64,
    pub arg6cageid: u64,
}

struct SerialExecutor {
    lock: Mutex<()>,
}

impl SerialExecutor {
    /// Create a serialization gate for grate calls.
    ///
    /// This executor is used when a grate handler runs in `Serialized` mode.
    /// In that mode, callers may still submit requests concurrently, but only
    /// one request is allowed to enter the grate at a time.
    fn new() -> Self {
        Self {
            lock: Mutex::new(()),
        }
    }

    /// Enter the serialized execution region.
    ///
    /// This acquires the internal mutex and returns a guard that keeps the
    /// grate in exclusive-execution mode for the duration of the call.
    /// If the mutex has been poisoned, execution continues by recovering the
    /// inner guard, since poisoning does not invalidate the grate runtime state.
    fn enter(&self) -> MutexGuard<'_, ()> {
        match self.lock.lock() {
            Ok(guard) => {
                #[cfg(feature = "debug-grate-calls")]
                {
                    println!("SerialExecutor: acquired lock");
                }

                guard
            }
            Err(poisoned) => {
                #[cfg(feature = "debug-grate-calls")]
                {
                    println!("SerialExecutor: lock poisoned, but continuing anyway");
                }

                poisoned.into_inner()
            }
        }
    }
}

/// One reusable grate execution worker.
///
/// A `GrateWorker` is the concrete execution unit for grate calls. Each worker
/// owns its own Wasmtime `Store` and `Instance`, but may still be attached to
/// the same underlying linear memory as other workers. To preserve isolation,
/// each worker is assigned a dedicated stack slot inside the shared stack arena.
struct GrateWorker<T: 'static> {
    /// Logical identifier of this worker within the handler’s pool.
    ///
    /// The worker id is also used to derive the worker’s private stack slot.
    worker_id: WorkerId,

    /// Worker-local Wasmtime store.
    ///
    /// This store holds the execution state for this worker and isolates its
    /// runtime context from other concurrently executing workers.
    store: Store<T>,

    /// The grate module instantiated into this worker's own `store`. Needed
    /// (not just the specific exports V1 resolves once below) so a V2 call
    /// can resolve an arbitrary generated adapter export by name at
    /// dispatch time -- see `v2_adapter_cache`.
    instance: Instance,

    /// Per-worker cache of resolved V2 (variable-width) adapters, keyed by
    /// export name. A fresh `Instance` needs its own cache (a resolved
    /// `Func` handle is tied to the `Store`/`Instance` it came from), so
    /// this lives on the worker, not the handler -- see
    /// `v2_adapter::V2AdapterCache`.
    v2_adapter_cache: v2_adapter::V2AdapterCache,

    /// Typed handle to the grate entry export, if present.
    ///
    /// This is usually the `pass_fptr_to_wt` trampoline used to enter the
    /// grate from the handler. It is cached here to avoid resolving the export
    /// on every call.
    pass_fptr_func: Option<PassFptrTyped>,

    /// Handle to this worker's mutable Wasm stack pointer global.
    ///
    /// This is cached for the same reason as `pass_fptr_func`: every grate call
    /// resets the worker stack, so resolving the export on each call adds fixed
    /// overhead to even trivial grate calls.
    stack_pointer: Global,

    /// Typed handle to the grate's own `__errno_location` export, if present.
    ///
    /// Read right after the grate call returns, so its value can be relayed
    /// back to the calling cage's own errno slot (see
    /// `threei::take_last_grate_errno`'s doc). Grates that don't link libc's
    /// errno support (e.g. non-libc freestanding grates) simply have `None`
    /// here, and no propagation is attempted.
    errno_location_func: Option<TypedFunc<(), i32>>,

    /// This worker's own linear memory export, cached for the same reason as
    /// `errno_location_func` -- both are resolved once here rather than
    /// looked up on every call. Kept as the raw `Extern` since a `-pthread`
    /// grate build exports `memory` as `SharedMemory` rather than `Memory`
    /// (see the read logic in `run`, which handles both).
    memory: Option<Extern>,

    /// Base address of this worker’s assigned stack slot in the stack arena.
    ///
    /// This marks the first usable byte of the worker’s dedicated stack region.
    stack_base: u32,

    /// Top address of this worker’s assigned stack slot.
    ///
    /// Before each call, the worker resets its `__stack_pointer` to this value
    /// so execution starts from a clean stack state within its own slot.
    stack_top: u32,

    /// Indices into this worker's own indirect-function table that
    /// previously held a callback proxy (see `install_callback_proxies`)
    /// and have since been cleared back to null. A wasm table can only
    /// grow, never shrink, so a slot a callback argument no longer needs is
    /// reclaimed into this list instead of abandoned: the next call that
    /// needs a proxy reuses one of these indices before growing the table
    /// again, bounding table growth across repeated calls rather than
    /// leaking one slot per call forever.
    callback_proxy_free_slots: Vec<u64>,

    /// Counts how many times `install_callback_proxies` has actually grown
    /// this worker's table (as opposed to reusing a freed slot), logged at
    /// each occurrence. A healthy, long-running grate should see this stay
    /// small and flat under repeated callback-bearing calls; a steadily
    /// climbing count means slots are leaking rather than being reclaimed.
    callback_proxy_grow_count: u64,
}

/// Compute the base address of the stack region assigned to a specific worker.
///
/// Each grate worker owns a dedicated stack slot inside the global stack arena.
/// This function maps a logical `worker_id` to the first usable byte of that
/// worker’s stack, skipping over the guard region placed before the slot.
fn worker_stack_base(cageid: u64, workerid: WorkerId) -> u32 {
    let stack_arena_base = lind_platform_const::get_stack_arena_base(cageid as usize)
        .unwrap_or_else(|| {
            panic!("STACK_ARENA_BASE is not initialized for cageid {}", cageid);
        });
    stack_arena_base
        + (workerid as u32 - 1) * (GRATE_STACK_GUARD_SIZE + GRATE_STACK_SLOT_SIZE)
        + GRATE_STACK_GUARD_SIZE
}

/// Compute the top address of the stack region assigned to a specific worker.
///
/// The returned address is used to reset the worker’s `__stack_pointer` before
/// starting a new grate call, ensuring that each invocation begins with a clean
/// stack state inside that worker’s private stack slot.
fn worker_stack_top(cageid: u64, workerid: WorkerId) -> u32 {
    worker_stack_base(cageid, workerid) + GRATE_STACK_SLOT_SIZE
}

fn configured_grate_workers() -> usize {
    // Defaults to MAX_GRATE_WORKERS; set LIND_GRATE_WORKERS to configure a smaller positive pool size.
    env::var(GRATE_WORKERS_ENV)
        .ok()
        .and_then(|value| value.parse::<usize>().ok())
        .filter(|count| *count > 0)
        .map(|count| count.min(MAX_GRATE_WORKERS))
        .unwrap_or(DEFAULT_GRATE_WORKERS)
}

/// Scoped ownership of a borrowed worker.
///
/// A `WorkerLease` represents a worker temporarily checked out from a
/// `GrateHandler`. When the lease is dropped, the worker is automatically
/// returned to the handler’s pool.
struct WorkerLease<'a, T: 'static> {
    /// The handler that owns the leased worker.
    ///
    /// This is used to return the worker to the pool on drop.
    owner: &'a GrateHandler<T>,

    /// The leased worker, if still held by this lease.
    ///
    /// The worker is wrapped in `Option` so it can be taken during drop and
    /// returned exactly once.
    worker: Option<GrateWorker<T>>,
}

impl<'a, T> WorkerLease<'a, T> {
    /// Create a scoped lease for a grate worker borrowed from a handler.
    ///
    /// A `WorkerLease` ensures that the worker is automatically returned to the
    /// owning handler when the lease is dropped, even if the grate call exits
    /// early due to an error or trap.
    fn new(owner: &'a GrateHandler<T>, worker: GrateWorker<T>) -> Self {
        Self {
            owner,
            worker: Some(worker),
        }
    }

    /// Get mutable access to the leased worker.
    ///
    /// This is used by the submission path to run a grate request inside the
    /// borrowed worker before the worker is returned to the pool on drop.
    fn worker_mut(&mut self) -> &mut GrateWorker<T> {
        self.worker.as_mut().unwrap()
    }
}

impl<'a, T: 'static> Drop for WorkerLease<'a, T> {
    /// Return the leased worker to its handler when the lease goes out of scope.
    ///
    /// This guarantees that worker-pool bookkeeping remains correct even when
    /// execution unwinds due to an error or trap.
    fn drop(&mut self) {
        if let Some(worker) = self.worker.take() {
            self.owner.return_worker(worker);
        }
    }
}

/// Scheduler and worker-pool owner for one grate.
///
/// A `GrateHandler` manages the reusable worker pool for a grate and defines
/// how incoming grate requests are admitted, scheduled, and shut down.
/// It is the main runtime object responsible for grate-call concurrency.
pub struct GrateHandler<T: 'static> {
    /// Identifier of the grate or cage associated with this handler.
    ///
    /// This is mainly used for diagnostics and error reporting.
    grate_id: u64,

    /// Configured concurrency policy for this grate.
    ///
    /// Determines whether `submit()` dispatches through serialized or parallel
    /// execution.
    concurrency_mode: ConcurrencyMode,

    /// Serialization gate used only when the handler is in `Serialized` mode.
    serial_executor: SerialExecutor,

    /// Mutex-protected internal worker-pool state.
    ///
    /// This protects the queue of available workers.
    inner: Mutex<GrateHandlerInner<T>>,

    /// Condition variable used to block until a worker becomes available or
    /// until shutdown-related state changes.
    cv: Condvar,

    /// Flag indicating that shutdown has started.
    ///
    /// Once set, new submissions are rejected, while existing in-flight calls
    /// are allowed to complete
    ///
    /// todo: not integrated with actual grate teardown yet
    shutting_down: AtomicBool,

    /// Number of grate calls currently in flight.
    ///
    /// This is used to coordinate graceful shutdown and to detect when the
    /// handler has become idle.
    ///
    /// todo: not integrated with actual grate teardown yet
    active_calls: AtomicUsize,
}

/// Mutex-protected internal state for a `GrateHandler`.
///
/// This structure exists to keep the lock scope narrow and separate the
/// worker-pool state from the rest of the handler’s control fields.
struct GrateHandlerInner<T: 'static> {
    workers: VecDeque<GrateWorker<T>>,
}

impl<T: Clone + 'static> GrateHandler<T> {
    /// Pre-create the worker pool for a grate handler.
    ///
    /// Each worker is an independent `store + instance + call stack` execution
    /// context for the same grate module. Pre-initializing the configured pool allows
    /// later grate calls to lease a ready-to-run worker without paying the cost
    /// of instantiation on the fast path.
    ///
    /// This worker replication is what enables grate-call concurrency: parallel
    /// calls execute in different Wasmtime stores rather than contending on a
    /// single shared execution context.
    fn init_workers(
        &mut self,
        template: &GrateTemplate<T>,
        host: &T,
        cageid: u64,
    ) -> anyhow::Result<()> {
        let worker_count = configured_grate_workers();

        for handler_id in 1_u64..=worker_count as u64 {
            let worker =
                create_worker(template, host.clone(), cageid, handler_id).with_context(|| {
                    format!(
                        "failed to create worker {} for cageid {}",
                        handler_id, cageid
                    )
                })?;

            self.inner.lock().unwrap().workers.push_back(worker);
        }

        Ok(())
    }
}

impl<T: 'static> GrateHandler<T> {
    /// Lease one available worker from the pool, blocking until one is free.
    ///
    /// This function is the core worker-pool acquisition primitive. If all
    /// workers are currently in use, the caller waits on the condition variable
    /// until another call finishes and returns a worker to the pool.
    fn take_worker_blocking(&self) -> GrateWorker<T> {
        let mut inner = self.inner.lock().unwrap();

        loop {
            if let Some(worker) = inner.workers.pop_front() {
                return worker;
            }
            inner = self.cv.wait(inner).unwrap();
        }
    }

    /// Return a worker to the pool and wake one waiting submitter.
    ///
    /// This makes the worker available for reuse by future grate calls.
    /// Returning workers through the handler centralizes pool management and
    /// ensures that blocked callers can resume when capacity becomes available.
    fn return_worker(&self, worker: GrateWorker<T>) {
        let mut inner = self.inner.lock().unwrap();
        let should_notify = inner.workers.is_empty();
        inner.workers.push_back(worker);
        if should_notify {
            self.cv.notify_one();
        }
    }

    /// Mark this grate handler as shutting down.
    ///
    /// After shutdown begins, new submissions are rejected by `ActiveCallGuard`.
    /// Existing in-flight calls are allowed to finish, and waiters are notified
    /// so that shutdown coordination can make progress.
    ///
    /// todo: not integrated with actual grate teardown yet
    pub fn begin_shutdown(&self) {
        self.shutting_down.store(true, Ordering::Release);
        self.cv.notify_all();
    }

    /// Block until all in-flight grate calls have completed.
    ///
    /// This is typically used during shutdown after `begin_shutdown()` has
    /// prevented new calls from entering. The function waits until the active
    /// call count drops to zero.
    ///
    /// todo: not integrated with actual grate teardown yet
    pub fn wait_for_idle(&self) {
        let mut guard = self.inner.lock().unwrap();
        while self.active_calls.load(Ordering::Acquire) != 0 {
            guard = self.cv.wait(guard).unwrap();
        }
    }

    /// Execute a grate request under serialized execution.
    ///
    /// This path acquires the serialization lock before leasing a worker,
    /// ensuring that at most one call enters the grate at a time even though
    /// the handler may still own multiple workers.
    fn submit_serialized(&self, req: GrateRequest) -> anyhow::Result<i64> {
        let _serial_guard = self.serial_executor.enter();
        let worker = self.take_worker_blocking();
        let mut lease = WorkerLease::new(self, worker);
        lease.worker_mut().run(req)
    }

    /// Execute a grate request under parallel execution.
    ///
    /// In parallel mode, the handler simply leases an available worker and
    /// runs the request immediately. Different callers may therefore execute
    /// concurrently as long as different workers are available.
    fn submit_parallel(&self, req: GrateRequest) -> anyhow::Result<i64> {
        let worker = self.take_worker_blocking();
        let mut lease = WorkerLease::new(self, worker);
        lease.worker_mut().run(req)
    }

    /// Submit a grate request to this handler.
    ///
    /// This is the main entry point for grate calls. It first registers the
    /// request as an active in-flight call, rejecting it if shutdown has begun,
    /// and then dispatches the request according to the handler’s configured
    /// concurrency mode.
    pub fn submit(&self, req: GrateRequest) -> anyhow::Result<i64> {
        let _reentrancy_guard = ReentrancyGuard::enter(self.grate_id)?;
        let _active_guard = ActiveCallGuard::new(self)?;

        match self.concurrency_mode {
            ConcurrencyMode::Serialized => self.submit_serialized(req),
            ConcurrencyMode::Parallel => self.submit_parallel(req),
        }
    }

    /// V2 counterpart to `submit_serialized`: acquires the serialization
    /// lock BEFORE leasing a worker, same ordering as the V1 path -- taking
    /// a worker only after the serial gate keeps "at most one call enters
    /// the grate at a time" true for the worker-acquisition step itself,
    /// not just the call inside it.
    fn submit_v2_serialized(
        &self,
        registration: &threei::V2Registration,
        source_cage: u64,
        args: &[Val],
    ) -> v2_adapter::V2CallOutcome {
        let _serial_guard = self.serial_executor.enter();
        let worker = self.take_worker_blocking();
        let mut lease = WorkerLease::new(self, worker);
        lease.worker_mut().run_v2(registration, source_cage, args)
    }

    /// V2 counterpart to `submit_parallel`.
    fn submit_v2_parallel(
        &self,
        registration: &threei::V2Registration,
        source_cage: u64,
        args: &[Val],
    ) -> v2_adapter::V2CallOutcome {
        let worker = self.take_worker_blocking();
        let mut lease = WorkerLease::new(self, worker);
        lease.worker_mut().run_v2(registration, source_cage, args)
    }

    /// Submit a V2 (variable-width) grate request to this handler.
    ///
    /// Reuses the exact same admission (`ActiveCallGuard`), worker leasing
    /// (`WorkerLease`, which returns the worker to the pool even if the
    /// adapter call traps or this function returns early), and
    /// concurrency-mode dispatch as `submit` -- a V2 call runs through the
    /// same pool and the same lifecycle guarantees, not a parallel,
    /// independently-maintained path. No request may use another call's
    /// `Store`, scratch frame, or cage identity: each leased worker owns its
    /// own `Store`/`Instance`, and a lease is never shared or reused
    /// concurrently by construction.
    pub fn submit_v2(
        &self,
        registration: &threei::V2Registration,
        source_cage: u64,
        args: &[Val],
    ) -> anyhow::Result<v2_adapter::V2CallOutcome> {
        let _reentrancy_guard = ReentrancyGuard::enter(self.grate_id)?;
        let _active_guard = ActiveCallGuard::new(self)?;

        Ok(match self.concurrency_mode {
            ConcurrencyMode::Serialized => {
                self.submit_v2_serialized(registration, source_cage, args)
            }
            ConcurrencyMode::Parallel => self.submit_v2_parallel(registration, source_cage, args),
        })
    }
}

std::thread_local! {
    /// Grate ids this OS thread is CURRENTLY executing a call into,
    /// innermost call included. Used only to detect self-reentrancy (a
    /// grate call that, before returning, causes another call back into a
    /// grate already active on this same thread) -- never consulted across
    /// threads, since a different thread's calls into the same grate are
    /// legitimate concurrent use of the worker pool, not reentrancy.
    static ACTIVE_GRATES_ON_THREAD: std::cell::RefCell<std::collections::HashSet<u64>> =
        std::cell::RefCell::new(std::collections::HashSet::new());
}

/// RAII guard against self-reentrant grate calls.
///
/// Both `submit`'s serialized path (`SerialExecutor::enter`, a plain
/// `Mutex<()>` -- not reentrant) and its worker-pool path
/// (`take_worker_blocking`, which blocks until a worker is returned) deadlock
/// if the SAME OS thread calls back into the SAME grate before its
/// outermost call has returned: a `Serialized` handler's mutex is already
/// held by this thread, and a fully-leased `Parallel` handler's only free
/// worker can never appear, because the call that would return it is the
/// one blocked waiting. This guard turns that deadlock into an immediate,
/// diagnosable rejection instead.
struct ReentrancyGuard {
    grate_id: u64,
}

impl ReentrancyGuard {
    fn enter(grate_id: u64) -> anyhow::Result<Self> {
        let already_active = ACTIVE_GRATES_ON_THREAD.with(|set| !set.borrow_mut().insert(grate_id));
        if already_active {
            anyhow::bail!(
                "reentrant grate call detected: this thread is already executing a call into \
                 grate {grate_id} -- a self-reentrant V2 call would deadlock on the serialized \
                 gate or an exhausted worker pool instead of ever returning"
            );
        }
        Ok(Self { grate_id })
    }
}

impl Drop for ReentrancyGuard {
    fn drop(&mut self) {
        ACTIVE_GRATES_ON_THREAD.with(|set| {
            set.borrow_mut().remove(&self.grate_id);
        });
    }
}

#[cfg(test)]
mod reentrancy_guard_tests {
    use super::ReentrancyGuard;

    #[test]
    fn same_grate_on_one_thread_is_rejected() {
        let _outer = ReentrancyGuard::enter(42).expect("first entry must succeed");
        match ReentrancyGuard::enter(42) {
            Err(e) => assert!(e.to_string().contains("reentrant grate call")),
            Ok(_) => panic!("nested entry into the SAME grate id must be rejected"),
        }
    }

    #[test]
    fn different_grates_nest_fine() {
        let _outer = ReentrancyGuard::enter(1).unwrap();
        let inner = ReentrancyGuard::enter(2);
        assert!(
            inner.is_ok(),
            "a nested call into a DIFFERENT grate must not be rejected"
        );
    }

    #[test]
    fn reentry_after_drop_succeeds() {
        {
            let _g = ReentrancyGuard::enter(7).unwrap();
        }
        // The first guard's Drop must have released grate 7; entering it
        // again (e.g. a later, unrelated call on this same thread) must
        // succeed, not be permanently poisoned by the earlier call.
        assert!(ReentrancyGuard::enter(7).is_ok());
    }

    #[test]
    fn independent_on_different_threads() {
        // A DIFFERENT thread calling into the same grate id concurrently is
        // legitimate worker-pool concurrency, not reentrancy -- the
        // thread-local set must never leak across threads.
        let _outer = ReentrancyGuard::enter(99).unwrap();
        let handle = std::thread::spawn(|| ReentrancyGuard::enter(99).is_ok());
        assert!(
            handle.join().unwrap(),
            "a different thread must not be blocked by this one"
        );
    }
}

/// RAII guard representing one active in-flight grate call.
///
/// An `ActiveCallGuard` increments the handler’s active-call counter when a
/// submission begins and decrements it automatically when execution ends,
/// ensuring correct shutdown coordination even in the presence of errors.
struct ActiveCallGuard<'a, T: 'static> {
    owner: &'a GrateHandler<T>,
}

impl<'a, T> ActiveCallGuard<'a, T> {
    /// Register one in-flight grate call against the handler.
    ///
    /// This guard prevents shutdown races by incrementing the active-call count
    /// before execution begins and double-checking whether shutdown started in
    /// the small window after the increment. If shutdown is already in progress,
    /// the increment is rolled back and submission fails.
    fn new(owner: &'a GrateHandler<T>) -> anyhow::Result<Self> {
        if owner.shutting_down.load(Ordering::Acquire) {
            anyhow::bail!("grate handler {} is shutting down", owner.grate_id);
        }

        owner.active_calls.fetch_add(1, Ordering::AcqRel);

        // double-check, avoid shutdown between fetch_add and return
        if owner.shutting_down.load(Ordering::Acquire) {
            owner.active_calls.fetch_sub(1, Ordering::AcqRel);
            owner.cv.notify_all();
            anyhow::bail!("grate handler {} is shutting down", owner.grate_id);
        }

        Ok(Self { owner })
    }
}

impl<'a, T> Drop for ActiveCallGuard<'a, T> {
    /// Deregister one in-flight grate call.
    ///
    /// Dropping this guard decrements the active-call count and notifies waiters,
    /// allowing shutdown code to observe when the handler has become idle.
    fn drop(&mut self) {
        self.owner.active_calls.fetch_sub(1, Ordering::AcqRel);
        if self.owner.shutting_down.load(Ordering::Acquire) {
            self.owner.cv.notify_all();
        }
    }
}

/// Lowers one `V2ValueType` to Wasmtime's own `ValType` -- the inverse of
/// `val_type_to_v2_value_type` below. `ValType` has no `PartialEq` (hence
/// going through `V2ValueType`, which does, for every comparison in this
/// file), so this direction exists only to build the proxy's actual
/// `FuncType` once a shape has already been decided.
fn v2_value_type_to_val_type(ty: threei::V2ValueType) -> ValType {
    match ty {
        threei::V2ValueType::I32 => ValType::I32,
        threei::V2ValueType::I64 => ValType::I64,
        threei::V2ValueType::F32 => ValType::F32,
        threei::V2ValueType::F64 => ValType::F64,
    }
}

/// Lowers a real, resolved Wasm value type to `V2ValueType`, or `None` if
/// it's a value type the V2 transport (and so a callback signature, which
/// reuses its vocabulary) cannot carry at all -- e.g. `v128` or a
/// reference type. A real target callback using one of these is an
/// "unsupported Wasm value type" rejection, the same condition
/// `lib3i_v2_unsupported_signature_reason` in `linker.rs` checks for the
/// OUTER function's own signature.
fn val_type_to_v2_value_type(ty: &ValType) -> Option<threei::V2ValueType> {
    match ty {
        ValType::I32 => Some(threei::V2ValueType::I32),
        ValType::I64 => Some(threei::V2ValueType::I64),
        ValType::F32 => Some(threei::V2ValueType::F32),
        ValType::F64 => Some(threei::V2ValueType::F64),
        _ => None,
    }
}

/// The declared, lowered `V2ValueType` shape of one callback signature:
/// its own parameter list and (zero- or one-element) result list, in the
/// same vocabulary a real resolved function's type is compared against
/// (`val_type_to_v2_value_type`) -- so "declared shape" and "real shape"
/// are always compared at the SAME level, never `ValType` (no
/// `PartialEq`) against `CallbackParamKind`/`CallbackRetKind` directly.
/// `Pointer`/`FunctionPointer` never reach here: `register_lib_handler_v2`
/// rejects a callback signature containing either at registration time,
/// before any `V2Registration` carrying one could exist.
fn callback_signature_v2_values(
    sig: &threei::CallbackSignature,
) -> (Vec<threei::V2ValueType>, Vec<threei::V2ValueType>) {
    let params = sig
        .params
        .iter()
        .map(|p| match p {
            threei::CallbackParamKind::Scalar(v) => *v,
            threei::CallbackParamKind::Pointer => {
                unreachable!("pointer-bearing callback parameters are rejected at registration")
            }
        })
        .collect();
    let results = match sig.ret {
        threei::CallbackRetKind::Void => Vec::new(),
        threei::CallbackRetKind::Scalar(v) => vec![v],
        threei::CallbackRetKind::FunctionPointer => {
            unreachable!("function-pointer-returning callbacks are rejected at registration")
        }
    };
    (params, results)
}

impl<T: 'static> GrateWorker<T> {
    /// Reset this worker’s stack pointer to the top of its private stack slot.
    ///
    /// Grate workers are reusable execution contexts. Before each new grate call,
    /// the worker’s Wasm stack pointer is reset so that the next invocation starts
    /// from a clean stack state rather than inheriting frames or stack position
    /// from a previous call.
    fn reset_worker_stack(&mut self) {
        let sp = self.stack_top;
        self.stack_pointer
            .set(&mut self.store, Val::I32(sp as i32))
            .expect("failed to set __stack_pointer");
    }

    /// Run one grate request inside this worker.
    ///
    /// Execution happens inside this worker’s private `Store` and `Instance`,
    /// which isolates its runtime state from other concurrently executing workers.
    /// The worker resets its stack, resolves the exported grate entry function,
    /// and invokes `pass_fptr_to_wt` with the marshalled request arguments.
    fn run(&mut self, req: GrateRequest) -> anyhow::Result<i64> {
        #[cfg(feature = "debug-grate-calls")]
        {
            println!(
                "Worker {} handling grate request for cage {}, handler_addr: {:#x}",
                self.worker_id, req.cageid, req.handler_addr
            );
        }

        self.reset_worker_stack();
        self.seed_errno_before_call();

        let func = self.pass_fptr_func.as_ref().ok_or_else(|| {
            anyhow::anyhow!("no pass_fptr_func found in worker {}", self.worker_id)
        })?;

        let ret = func
            .call(
                &mut self.store,
                (
                    req.handler_addr,
                    req.cageid,
                    req.arg1,
                    req.arg1cageid,
                    req.arg2,
                    req.arg2cageid,
                    req.arg3,
                    req.arg3cageid,
                    req.arg4,
                    req.arg4cageid,
                    req.arg5,
                    req.arg5cageid,
                    req.arg6,
                    req.arg6cageid,
                ),
            )
            .map_err(|e| {
                anyhow::anyhow!(
                    "pass_fptr_to_wt trapped in worker {}: {:#}",
                    self.worker_id,
                    e
                )
            })?;

        #[cfg(feature = "debug-grate-calls")]
        println!(
            "Worker {} got result {} from pass_fptr_to_wt",
            self.worker_id, ret
        );

        self.relay_errno_after_call();

        Ok(ret)
    }

    /// Reset before a call so a grate lacking `__errno_location` (or a
    /// failed resolve here) never leaks a stale value from a previous call
    /// into this one's dispatch, then seed this worker's own errno with the
    /// caller's pre-call value (see `threei::take_next_grate_errno_seed`'s
    /// doc). Without seeding, the grate's errno -- a separate value that
    /// outlives any single call -- would carry over whatever an earlier,
    /// unrelated dispatch happened to leave it at, leaking into every later
    /// call that doesn't itself touch errno. Shared by V1's `run` and V2's
    /// `run_v2` -- both dispatch into this same worker's grate instance.
    fn seed_errno_before_call(&mut self) {
        threei::set_last_grate_errno(None);

        if let Some(seed) = threei::take_next_grate_errno_seed() {
            if let (Some(errno_func), Some(mem)) =
                (self.errno_location_func.as_ref(), self.memory.as_ref())
            {
                if let Ok(addr) = errno_func.call(&mut self.store, ()) {
                    let addr = addr as u32 as usize;
                    match mem {
                        Extern::Memory(m) => {
                            let _ = m.write(&mut self.store, addr, &seed.to_le_bytes());
                        }
                        Extern::SharedMemory(sm) => {
                            let data = sm.data();
                            if addr + 4 <= data.len() {
                                for (i, b) in seed.to_le_bytes().into_iter().enumerate() {
                                    unsafe {
                                        *data[addr + i].get() = b;
                                    }
                                }
                            }
                        }
                        _ => {}
                    }
                }
            }
        }
    }

    /// Relay this worker's own errno (as set by whatever the grate just ran,
    /// inside its own address space) out to the caller-side portal via the
    /// thread-local in `threei`. Best-effort: a grate without
    /// `__errno_location` or without linear memory just leaves this unset.
    /// Shared by V1's `run` and V2's `run_v2`.
    fn relay_errno_after_call(&mut self) {
        if let (Some(errno_func), Some(mem)) =
            (self.errno_location_func.as_ref(), self.memory.as_ref())
        {
            if let Ok(addr) = errno_func.call(&mut self.store, ()) {
                // addr is a wasm32 pointer; zero-extend through u32 first, since
                // `i32 as usize` sign-extends and a high address (top bit set)
                // would otherwise become a bogus, huge 64-bit offset.
                let addr = addr as u32 as usize;
                let value = match mem {
                    Extern::Memory(m) => {
                        let mut buf = [0u8; 4];
                        m.read(&self.store, addr, &mut buf)
                            .ok()
                            .map(|_| i32::from_le_bytes(buf))
                    }
                    // A `-pthread` grate build exports `memory` as shared
                    // rather than unshared; SharedMemory has no `read`/`write`
                    // (concurrent access must go through its UnsafeCell
                    // `data()`, matching how threaded wasm code itself would
                    // touch this memory).
                    Extern::SharedMemory(sm) => {
                        let data = sm.data();
                        (addr + 4 <= data.len()).then(|| {
                            let mut buf = [0u8; 4];
                            for i in 0..4 {
                                buf[i] = unsafe { *data[addr + i].get() };
                            }
                            i32::from_le_bytes(buf)
                        })
                    }
                    _ => None,
                };
                if let Some(value) = value {
                    threei::set_last_grate_errno(Some(value));
                }
            }
        }
    }

    /// For every `registration.callback_params` entry, the caller's raw
    /// i32 at that parameter index is a table index into ITS OWN
    /// indirect-function table (`source_cage`) -- a function pointer the
    /// caller is passing in -- not an ordinary scalar. Resolve the real
    /// target `Func` there now -- while `source_cage`'s re-entry frame is
    /// still active -- checked against the entry's declared
    /// `CallbackSignature` (a "callback ABI mismatch" rejection if the
    /// real resolved function's lowered type doesn't match), build a host
    /// proxy of that exact type that calls back into it, install the
    /// proxy into THIS worker's own table, and replace the argument with
    /// the proxy's local index before the adapter ever sees it. `0`
    /// (wasm's conventional null funcref slot) passes through unchanged
    /// when the declared signature allows it (`nullable`); when it
    /// doesn't, `0` is rejected before anything else runs -- a null the
    /// contract declared impossible is a caller bug, not a value to
    /// silently tolerate.
    ///
    /// Reuses a slot from `callback_proxy_free_slots` where available
    /// instead of always growing the table, so a long-running cage making
    /// repeated calls doesn't grow this table without bound (a wasm table
    /// can grow but never shrink). Returns the possibly-replaced arguments
    /// together with every table index this call newly occupied (reused or
    /// grown) -- the caller is responsible for reclaiming them via
    /// `release_callback_proxies` once the call this proxy was installed
    /// for has finished, regardless of outcome.
    ///
    /// Returns `Err` (never a fabricated callback or a silent local call)
    /// if `source_cage` has no active frame, the index is out of bounds,
    /// the table slot isn't a function, the real function's type doesn't
    /// match the declared signature, or table growth fails -- having
    /// already released back to the free list any proxy THIS call
    /// installed before the failure, so a later callback parameter's
    /// failure never leaks an earlier one's slot.
    fn install_callback_proxies(
        &mut self,
        source_cage: u64,
        callback_params: &[threei::CallbackArgContract],
        args: &[Val],
    ) -> Result<(Vec<Val>, Vec<u64>), String> {
        let mut args = args.to_vec();
        let mut installed = Vec::new();
        for contract in callback_params {
            let idx = contract.arg_index as usize;
            let Some(&Val::I32(raw_index)) = args.get(idx) else {
                self.release_callback_proxies(&installed);
                return Err(format!(
                    "callback parameter {idx} is not present or not an I32 table index"
                ));
            };
            if raw_index == 0 {
                if !contract.signature.nullable {
                    self.release_callback_proxies(&installed);
                    return Err(format!(
                        "callback parameter {idx} is null, but its declared signature is \
                         not nullable"
                    ));
                }
                continue;
            }

            let (expected_params, expected_results) =
                callback_signature_v2_values(&contract.signature);

            let target =
                match wasmtime::with_active_frame::<T, _>(source_cage, |mut store_a, table_a| {
                    let f = match table_a.get(&mut store_a, raw_index as u64) {
                        Some(Ref::Func(Some(f))) => f,
                        Some(Ref::Func(None)) => {
                            return Err(
                                "callback table slot is null in the source cage".to_string()
                            );
                        }
                        Some(_) => {
                            return Err("callback table slot is not a funcref".to_string());
                        }
                        None => {
                            return Err(
                                "callback table index out of bounds in source cage".to_string()
                            );
                        }
                    };
                    let real_ty = f.ty(&store_a);
                    let Some(real_params) = real_ty
                        .params()
                        .map(|t| val_type_to_v2_value_type(&t))
                        .collect::<Option<Vec<_>>>()
                    else {
                        return Err(
                            "callback's real resolved type uses an unsupported Wasm value type"
                                .to_string(),
                        );
                    };
                    let Some(real_results) = real_ty
                        .results()
                        .map(|t| val_type_to_v2_value_type(&t))
                        .collect::<Option<Vec<_>>>()
                    else {
                        return Err(
                            "callback's real resolved type uses an unsupported Wasm value type"
                                .to_string(),
                        );
                    };
                    if real_params != expected_params || real_results != expected_results {
                        return Err(format!(
                            "callback ABI mismatch: declared signature params={expected_params:?} \
                             results={expected_results:?}, but the real callback's resolved type \
                             is params={real_params:?} results={real_results:?}"
                        ));
                    }
                    Ok(f)
                }) {
                    Some(resolved) => match resolved {
                        Ok(f) => f,
                        Err(reason) => {
                            self.release_callback_proxies(&installed);
                            return Err(reason);
                        }
                    },
                    None => {
                        self.release_callback_proxies(&installed);
                        return Err(format!(
                            "no active re-entry frame for source cage {source_cage}"
                        ));
                    }
                };

            let proxy_param_types: Vec<ValType> = expected_params
                .iter()
                .map(|v| v2_value_type_to_val_type(*v))
                .collect();
            let proxy_result_types: Vec<ValType> = expected_results
                .iter()
                .map(|v| v2_value_type_to_val_type(*v))
                .collect();
            let proxy_ty =
                FuncType::new(self.store.engine(), proxy_param_types, proxy_result_types);
            let proxy = Func::new(
                &mut self.store,
                proxy_ty,
                move |_caller: Caller<'_, T>, params, results| match wasmtime::with_active_frame::<
                    T,
                    _,
                >(
                    source_cage,
                    |store_a, _table_a| target.call(store_a, params, results),
                ) {
                    Some(inner) => inner,
                    None => Err(wasmtime::Error::msg(format!(
                        "callback proxy: no active re-entry frame for cage {source_cage}"
                    ))),
                },
            );

            let Some(Extern::Table(dest_table)) = self
                .instance
                .get_export(&mut self.store, "__indirect_function_table")
            else {
                self.release_callback_proxies(&installed);
                return Err(
                    "this grate's module does not export __indirect_function_table".to_string(),
                );
            };
            let index = match self.callback_proxy_free_slots.pop() {
                Some(reused) => reused,
                None => match dest_table.grow(&mut self.store, 1, Ref::Func(None)) {
                    Ok(grown) => {
                        self.callback_proxy_grow_count += 1;
                        eprintln!(
                            "[lind-3i] callback proxy table growth event #{}",
                            self.callback_proxy_grow_count
                        );
                        grown
                    }
                    Err(e) => {
                        self.release_callback_proxies(&installed);
                        return Err(format!("callback proxy table growth failed: {e:#}"));
                    }
                },
            };
            if let Err(e) = dest_table.set(&mut self.store, index, Ref::Func(Some(proxy))) {
                self.callback_proxy_free_slots.push(index);
                self.release_callback_proxies(&installed);
                return Err(format!("callback proxy table install failed: {e:#}"));
            }

            installed.push(index);
            args[idx] = Val::I32(index as i32);
        }
        Ok((args, installed))
    }

    /// Clears every listed table index back to a null funcref and returns
    /// it to `callback_proxy_free_slots` for reuse. Called once a call
    /// `install_callback_proxies` installed proxies for has finished,
    /// regardless of whether it succeeded, was rejected, or trapped --
    /// cleanup must not depend on the call's outcome, only on whether a
    /// proxy was installed for it.
    ///
    /// A slot is returned to the free list ONLY after it is confirmed
    /// cleared. An index is never recycled on a failed clear: the slot
    /// would still hold a proxy scoped to a call that has already
    /// returned, and `install_callback_proxies`'s own `Table::set` would
    /// overwrite it on reuse in the ordinary case, but there is no such
    /// guarantee against this SAME index being reached directly by an
    /// unrelated `call_indirect` in the meantime -- that would let a
    /// `during_call`-scoped callback stay callable after its call ended.
    /// Failing to clear a slot this code itself just finished using is not
    /// a recoverable error to work around; it means something about this
    /// worker's table is no longer behaving as this code assumes, so it
    /// panics rather than silently keep serving calls against it.
    fn release_callback_proxies(&mut self, indices: &[u64]) {
        if indices.is_empty() {
            return;
        }
        let Some(Extern::Table(dest_table)) = self
            .instance
            .get_export(&mut self.store, "__indirect_function_table")
        else {
            panic!(
                "callback proxy cleanup: this grate's module no longer exports \
                 __indirect_function_table, with {} proxy slot(s) still to reclaim",
                indices.len()
            );
        };
        for &idx in indices {
            if let Err(e) = dest_table.set(&mut self.store, idx, Ref::Func(None)) {
                panic!("callback proxy cleanup: failed to clear table slot {idx}: {e:#}");
            }
            self.callback_proxy_free_slots.push(idx);
        }
    }

    /// Run one V2 (variable-width) grate request inside this worker: resolve
    /// (and cache) the registered adapter export, then call it with dynamic
    /// argument/result vectors -- no process-global mutable argument buffer,
    /// no fixed six-slot tuple. Mirrors `run`'s stack-reset/errno-seed/
    /// errno-relay structure exactly; the only difference is the shape of
    /// the call itself.
    fn run_v2(
        &mut self,
        registration: &threei::V2Registration,
        source_cage: u64,
        args: &[Val],
    ) -> v2_adapter::V2CallOutcome {
        self.reset_worker_stack();
        self.seed_errno_before_call();

        // Resolved before the adapter lookup below: both mutably borrow
        // `self` (the proxy installer through `self.store`/`self.instance`,
        // the cache lookup through `self.v2_adapter_cache`), and the
        // adapter lookup's result stays borrowed from `self` for the rest
        // of this function, so the two calls cannot be interleaved.
        let mut installed_proxies: Vec<u64> = Vec::new();
        let args_owned;
        let args = if registration.callback_params.is_empty() {
            args
        } else {
            match self.install_callback_proxies(source_cage, &registration.callback_params, args) {
                Ok((replaced, installed)) => {
                    installed_proxies = installed;
                    args_owned = replaced;
                    &args_owned
                }
                Err(reason) => return v2_adapter::V2CallOutcome::Rejected(reason),
            }
        };

        let outcome =
            match self
                .v2_adapter_cache
                .resolve(&mut self.store, &self.instance, registration)
            {
                Ok(adapter) => match v2_adapter::call_v2_adapter(
                    &mut self.store,
                    adapter,
                    source_cage,
                    registration.grate_cage,
                    args,
                ) {
                    Ok(results) => v2_adapter::V2CallOutcome::Ok(results),
                    Err(e) => v2_adapter::V2CallOutcome::Trapped(format!(
                        "V2 adapter trapped in worker {}: {e:#}",
                        self.worker_id
                    )),
                },
                Err(rejection) => v2_adapter::V2CallOutcome::Rejected(rejection.to_string()),
            };

        // Every installed proxy is `during_call`-scoped (there is no
        // retained-callback lifetime yet): reclaimed here unconditionally,
        // regardless of whether the call above succeeded, was rejected, or
        // trapped, so a repeated callback-bearing call never leaks a table
        // slot.
        self.release_callback_proxies(&installed_proxies);

        self.relay_errno_after_call();

        outcome
    }
}

/// Create a single grate worker.
///
/// A worker is an independently executable `Store + Instance + call stack`
/// context for one grate module. Workers are the unit of grate-call execution:
/// each submitted request runs inside one worker borrowed from the handler’s
/// pool.
///
/// Although different workers own different Wasmtime `Store`s and `Instance`s,
/// they are attached to the same underlying linear memory region. As a result,
/// workers must not share the same stack range inside linear memory.
///
/// To preserve isolation between concurrent grate calls, lind-wasm partitions
/// the grate stack arena into per-worker stack slots at instantiation time.
/// Each worker is then assigned its own dedicated stack slot, and later resets
/// its `__stack_pointer` to that slot before beginning execution.
///
/// The stack-slot partitioning is established by
/// `instantiate_with_lind_thread()`. See that function’s comments for the
/// detailed layout and attachment semantics.
pub fn create_worker<T>(
    template: &GrateTemplate<T>,
    host: T,
    cageid: u64,
    worker_id: WorkerId,
) -> anyhow::Result<GrateWorker<T>>
where
    T: Clone + 'static,
{
    let mut store = Store::new(&template.engine, host);

    let linker: Linker<T> = template.linker.clone();

    let (instance, _, _) = linker
        .instantiate_with_lind_thread(&mut store, &template.module, false)
        .context("failed to instantiate grate module")?;

    let pass_fptr_func = match instance.get_export(&mut store, "pass_fptr_to_wt") {
        Some(_) => Some(instance.get_typed_func::<(
            u64,
            u64,
            u64,
            u64,
            u64,
            u64,
            u64,
            u64,
            u64,
            u64,
            u64,
            u64,
            u64,
            u64,
        ), i64>(&mut store, "pass_fptr_to_wt")?),
        None => None,
    };

    let errno_location_func = match instance.get_export(&mut store, "__errno_location") {
        Some(_) => instance
            .get_typed_func::<(), i32>(&mut store, "__errno_location")
            .ok(),
        None => None,
    };
    let memory = instance.get_export(&mut store, "memory");

    let stack_base = worker_stack_base(cageid, worker_id);
    let stack_top = worker_stack_top(cageid, worker_id);
    let stack_pointer = instance
        .get_global(&mut store, "__stack_pointer")
        .ok_or_else(|| anyhow::anyhow!("missing __stack_pointer"))?;

    Ok(GrateWorker {
        worker_id,
        store,
        instance,
        v2_adapter_cache: v2_adapter::V2AdapterCache::new(),
        pass_fptr_func,
        stack_pointer,
        errno_location_func,
        memory,
        stack_base,
        stack_top,
        callback_proxy_free_slots: Vec::new(),
        callback_proxy_grow_count: 0,
    })
}

/// Create and initialize a grate handler for one cage.
///
/// A `GrateHandler` owns the reusable worker pool for the target grate and
/// defines how incoming grate calls are scheduled. In `Parallel` mode, multiple
/// calls may execute concurrently by leasing different workers. In `Serialized`
/// mode, calls still use the same worker-pool abstraction, but entry is gated
/// so that only one call runs at a time.
///
/// This function eagerly creates the configured worker pool so that the handler
/// is ready to serve grate calls immediately after registration.
///
/// By default, the pool size is MAX_GRATE_WORKERS; set LIND_GRATE_WORKERS
/// to configure it.
pub fn create_handler_for_cage<T: Clone + 'static>(
    template: &GrateTemplate<T>,
    host: T,
    cageid: u64,
    concurrency_mode: ConcurrencyMode,
) -> anyhow::Result<GrateHandler<T>> {
    let mut handler = GrateHandler {
        grate_id: cageid,
        concurrency_mode,
        serial_executor: SerialExecutor::new(),
        inner: Mutex::new(GrateHandlerInner {
            workers: VecDeque::new(),
        }),
        cv: Condvar::new(),
        shutting_down: AtomicBool::new(false),
        active_calls: AtomicUsize::new(0),
    };

    handler.init_workers(template, &host, cageid)?;

    Ok(handler)
}

#[derive(Clone, Copy)]
pub struct VmCtxWrapper {
    pub vmctx: NonNull<c_void>,
}

unsafe impl Send for VmCtxWrapper {}
unsafe impl Sync for VmCtxWrapper {}

impl VmCtxWrapper {
    // exposes the raw mutable pointer
    #[inline]
    pub fn as_ptr(self) -> *mut c_void {
        self.vmctx.as_ptr()
    }
}

/// Per-cage, per-thread *active* `VMContext` table.
///
/// This table stores the *currently active* Wasmtime execution context for each thread and is
/// used exclusively for **continuation-sensitive operations** that must resume execution in the
/// same Wasmtime instance that originally issued the syscall.
static VMCTX_THREADS: OnceLock<Vec<Mutex<HashMap<u64, VmCtxWrapper>>>> = OnceLock::new();

/// Initialize the global `VMContext` pool.
///
/// This function must be called exactly once during lind-wasm startup, before any `VMContext` is
/// pushed to or retrieved from the pool. It eagerly allocates one empty queue per possible `cage_id`.
pub fn init_vmctx_pool() {
    VMCTX_THREADS.get_or_init(|| {
        (0..lind_platform_const::MAX_CAGEID)
            .map(|_| Mutex::new(HashMap::new()))
            .collect()
    });
}

/// Register a VMContext according to `(cage_id, tid)` in the per-thread active table.
///
/// This is used exclusively for pthread-related syscalls and thread exit.
/// Grate calls and normal execution never consult this table.
pub fn set_vmctx_thread(cage_id: u64, tid: u64, vmctx: VmCtxWrapper) {
    let tables = VMCTX_THREADS.get().expect("VMCTX_THREADS not initialized");
    let t = tables.get(cage_id as usize).expect("invalid cage_id");
    t.lock().unwrap().insert(tid, vmctx);
}

/// Look up the VMContext
///
/// Returns `None` if the thread has exited or was never registered.
pub fn get_vmctx_thread(cage_id: u64, tid: u64) -> Option<VmCtxWrapper> {
    let tables = VMCTX_THREADS.get().expect("VMCTX_THREADS not initialized");
    let t = tables.get(cage_id as usize).expect("invalid cage_id");
    t.lock().unwrap().get(&tid).copied()
}

/// Remove a single thread entry.
///
/// Special case:
/// - if `tid == 0`, remove all VMContext entries under `cage_id`.
pub fn rm_vmctx_thread(cage_id: u64, tid: u64) -> bool {
    let Some(tables) = VMCTX_THREADS.get() else {
        println!("rm_vmctx_thread: VMCTX_THREADS not initialized");
        return false;
    };
    let Some(t) = tables.get(cage_id as usize) else {
        println!("rm_vmctx_thread: invalid cage_id {}", cage_id);
        return false;
    };

    let mut guard = t.lock().unwrap();

    if tid == 0 {
        let had_entries = !guard.is_empty();
        guard.clear();
        had_entries
    } else {
        guard.remove(&tid).is_some()
    }
}
